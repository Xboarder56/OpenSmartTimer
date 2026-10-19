"""Endpoint-only ost-relay/1 ECDH/AES-GCM implementation for mock and bench tests.

This module must never be imported by the relay service. It owns capabilities
and ephemeral traffic keys, just like the browser and firmware endpoints.
"""
import base64
import hashlib
import hmac
import json
import secrets
import struct
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import encode_dss_signature
from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives.ciphers.aead import AESGCM
from cryptography.hazmat.primitives.kdf.hkdf import HKDF


def enc(data):
    return base64.urlsafe_b64encode(data).rstrip(b'=').decode('ascii')


def dec(value, size=None):
    if not isinstance(value, str):
        raise ValueError('base64url required')
    raw = base64.b64decode(value + '=' * (-len(value) % 4), altchars=b'-_', validate=True)
    if enc(raw) != value or (size is not None and len(raw) != size):
        raise ValueError('noncanonical base64url')
    return raw


def hkdf(ikm, salt, info=b'', length=32):
    return HKDF(algorithm=hashes.SHA256(), length=length, salt=salt, info=info).derive(ikm)


def public(key):
    return key.public_key().public_bytes(serialization.Encoding.X962,
                                         serialization.PublicFormat.UncompressedPoint)


def transcript(timer, boot, fp_hash, client, peer):
    return b'OST-RELAY-E2E-OPEN-v1' + timer.encode('ascii') + b'\0' + boot.encode('ascii') + b'\0' + fp_hash + client + peer


def accept_transcript(opened, server, peer):
    return b'OST-RELAY-E2E-ACCEPT-v1' + opened + server + peer


P256_ORDER = 0xFFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551


def verify_accept(signature, message, public_key):
    raw = dec(signature, 64)
    r, s = int.from_bytes(raw[:32], 'big'), int.from_bytes(raw[32:], 'big')
    if not 0 < r < P256_ORDER or not 0 < s <= P256_ORDER // 2:
        raise ValueError('invalid identity signature')
    try:
        public_key.verify(encode_dss_signature(r, s), message, ec.ECDSA(hashes.SHA256()))
    except InvalidSignature:
        raise ValueError('invalid identity signature') from None


class Channel:
    def __init__(self, fp_hash, client, server, rx, tx, direction):
        self.fp_hash, self.client, self.server = fp_hash, client, server
        self.rx, self.tx, self.direction = AESGCM(rx), AESGCM(tx), direction
        self.rx_seq = self.tx_seq = 0

    def aad(self, direction, seq, request):
        if not isinstance(request, str) or len(request) > 64 or any(not 33 <= ord(c) <= 126 for c in request):
            raise ValueError('request ID')
        return b'OST-RELAY-E2E-DATA-v1' + bytes([direction]) + self.fp_hash + self.client + self.server + struct.pack('>Q', seq) + request.encode('ascii')

    def encrypt(self, body, request=''):
        self.tx_seq += 1
        if self.tx_seq >= 2**53:
            raise ValueError('sequence exhausted')
        if body.get('type') == 'command':
            body = {**body, 'downstream_seq': self.tx_seq}
        raw = json.dumps(body, separators=(',', ':'), ensure_ascii=False).encode()
        if len(raw) > 1800:
            raise ValueError('plaintext too large')
        seq = self.tx_seq
        cipher = self.tx.encrypt(b'\0'*4 + struct.pack('>Q', seq), raw, self.aad(self.direction, seq, request))
        return dict(type='e2e', fp_hash=enc(self.fp_hash), client_id=enc(self.client), server_id=enc(self.server), seq=seq, request_id=request, ciphertext=enc(cipher))

    def decrypt(self, frame):
        seq = frame['seq']
        if (type(seq) is not int or not self.rx_seq < seq < 2**53 or frame['type'] != 'e2e'
                or frame['fp_hash'] != enc(self.fp_hash) or frame['client_id'] != enc(self.client)
                or frame['server_id'] != enc(self.server)):
            raise ValueError('wrong channel or replay')
        cipher = dec(frame['ciphertext'])
        if not 16 <= len(cipher) <= 1816:
            raise ValueError('ciphertext too large')
        raw = self.rx.decrypt(b'\0'*4 + struct.pack('>Q', seq), cipher,
                              self.aad(3-self.direction, seq, frame['request_id']))
        self.rx_seq = seq  # Consume authenticated malformed payloads too.
        return json.loads(raw)


class BrowserOpen:
    def __init__(self, token):
        self.token = token
        self.payload = json.loads(dec(token.split('.')[2]))
        p = self.payload
        self.verify_key = ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), dec(p['vk'], 33))
        self.hash = hashlib.sha256(dec(p['fp'], 16)).digest()[:16]
        self.client = secrets.token_bytes(16)
        self.private = ec.generate_private_key(ec.SECP256R1())
        self.auth = hkdf(dec(p['fp'], 16), b'OST-RELAY-E2E-AUTH-v1')
        peer = public(self.private)
        self.transcript = transcript(p['tid'], p['boot'], self.hash, self.client, peer)
        self.frame = dict(type='e2e_open', fp_hash=enc(self.hash), client_id=enc(self.client), boot_id=p['boot'], client_key=enc(peer), tag=enc(hmac.digest(self.auth, self.transcript, 'sha256')))

    def accept(self, frame):
        if frame['type'] != 'e2e_accept' or frame['fp_hash'] != enc(self.hash) or frame['client_id'] != enc(self.client):
            raise ValueError('wrong accept')
        server, peer = dec(frame['server_id'], 16), dec(frame['server_key'], 65)
        joined = accept_transcript(self.transcript, server, peer)
        verify_accept(frame.get('sig'), joined, self.verify_key)
        if not hmac.compare_digest(dec(frame['tag'], 32), hmac.digest(self.auth, joined, 'sha256')):
            raise ValueError('unauthenticated accept')
        shared = self.private.exchange(ec.ECDH(), ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), peer))
        keys = hkdf(shared, self.auth, b'OST-RELAY-E2E-KEYS-v1' + hashlib.sha256(joined).digest(), 64)
        self.private = self.auth = None
        return Channel(self.hash, self.client, server, keys[32:], keys[:32], 1)


def timer_accept(frame, timer, boot, fp, sign):
    peer, client, fp_hash = dec(frame['client_key'], 65), dec(frame['client_id'], 16), dec(frame['fp_hash'], 16)
    if frame['boot_id'] != boot or not hmac.compare_digest(fp_hash, hashlib.sha256(fp).digest()[:16]):
        raise ValueError('wrong grant')
    auth = hkdf(fp, b'OST-RELAY-E2E-AUTH-v1')
    opened = transcript(timer, boot, fp_hash, client, peer)
    if not hmac.compare_digest(dec(frame['tag'], 32), hmac.digest(auth, opened, 'sha256')):
        raise ValueError('unauthenticated open')
    private = ec.generate_private_key(ec.SECP256R1())
    shared = private.exchange(ec.ECDH(), ec.EllipticCurvePublicKey.from_encoded_point(ec.SECP256R1(), peer))
    server, pub = secrets.token_bytes(16), public(private)
    joined = accept_transcript(opened, server, pub)
    keys = hkdf(shared, auth, b'OST-RELAY-E2E-KEYS-v1' + hashlib.sha256(joined).digest(), 64)
    accepted = dict(type='e2e_accept', fp_hash=enc(fp_hash), client_id=enc(client), server_id=enc(server), server_key=enc(pub), tag=enc(hmac.digest(auth, joined, 'sha256')), sig=enc(sign(joined)))
    return accepted, Channel(fp_hash, client, server, keys[:32], keys[32:], 2)
