"""C++ -> real Python API/database -> C++, with browser-verifier fixtures."""
import hashlib
import hmac
import json
import os
from pathlib import Path
import secrets
import subprocess

import httpx
import pytest
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature

from host_build import COMMON, ROOT, compile_host
from cpp_interop import CppClient
from ostrelay import ws
from ostrelay.session import parse_wire_token
from ostrelay.wire import b64encode, b64decode


@pytest.fixture(scope='module')
def cpp_binary(tmp_path_factory):
    binary = tmp_path_factory.mktemp('cpp-interop') / 'interop_driver'
    compile_host('interop_driver', COMMON + ['../src/relay/RelayClient.cpp', '../src/relay/RelayClientSecure.cpp'], binary)
    return binary


@pytest.fixture
def cpp(cpp_binary):
    client = CppClient(cpp_binary)
    yield client
    client.close()


P256_ORDER = 0xFFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551


def compressed(public_key):
    return public_key.public_bytes(serialization.Encoding.X962, serialization.PublicFormat.CompressedPoint)


def raw_low_s(der):
    r, s = decode_dss_signature(der)
    return r.to_bytes(32, 'big') + min(s, P256_ORDER - s).to_bytes(32, 'big')


def parse_authenticated_response(frame):
    # Only called after the verified handshake and successful channel.decrypt().
    assert set(frame) == {'type','upstream_seq','body_json'}
    return json.loads(frame['body_json'])


async def test_cpp_relay_api_and_browser_interoperability(cpp, app, fresh_db, tmp_path):
    from secure import BrowserOpen
    info = cpp.call('inspect')
    wire = info['wire']; payload = parse_wire_token(wire)
    fresh_db.connect().execute('INSERT INTO timers(timer_id,enrollment_secret,hardware_model,chip_family,build_id,spki_sha256_pin,created_at) VALUES(?,?,?,?,?,?,?)',
        ('timer_test_a',bytes([0x37])*32,'test','test','a'*24,'',0))
    cpp.call('connect')
    nonce = secrets.token_bytes(16)
    hello = cpp.call('frame',frame={'type':'challenge','server_nonce':b64encode(nonce)})['frames'][0]
    verified = await ws._verify_hello(hello,nonce)
    assert verified
    assert await ws._verify_hello(hello,bytes(16)) is None
    # The firmware client accepts the relay's own acknowledgement tag.
    assert cpp.call('frame',frame={'type':'hello_ack','ack_tag':verified['ack_tag']})['frames'] == []
    assert cpp.call('inspect')['live'] is True
    class Bridge:
        frames = []
        async def send_json(self, frame):
            self.frames.append(frame)
            response = cpp.call('frame',frame=frame)
            for outgoing in response['frames']:
                if outgoing['type'] in ('e2e','e2e_accept'):
                    ws._handle_opaque(session,outgoing)
        async def close(self,**kwargs): pass
    bridge=Bridge()
    session=ws.WsSession(timer_id='timer_test_a',ws=bridge,session_id='test',boot_id='aabbccdd',hello_uptime_ms=1000)
    await ws.registry.register(session)
    mint=cpp.call('mint')['frames'][0]
    assert ws._persist_token_mint(session,mint)=='ready'
    cpp.call('frame',frame={'type':'token_ack','action':'token_mint','fp_hash':info['fp_hash'],'boot_id':'aabbccdd','upstream_seq':mint['upstream_seq'],'status':'ready'})
    opened=BrowserOpen(wire)
    async with httpx.AsyncClient(transport=httpx.ASGITransport(app=app),base_url='https://testserver') as api:
        body={k:v for k,v in opened.frame.items() if k!='type'}
        response=await api.post('/api/v1/session',json=body)
        assert response.status_code==200,response.text
        routed=response.json(); channel=opened.accept(routed['frame'])
        headers={'X-OST-Session':routed['session_handle']}
        assert not response.cookies
        assert (await api.post('/api/v1/session',json={'token':wire})).status_code==422
        sync=channel.encrypt({'type':'sync','cap_token':wire},'fresh-status')
        response=await api.post('/api/v1/command',headers=headers,json=sync)
        assert response.status_code==200,response.text
        status=channel.decrypt(response.json())
        assert parse_authenticated_response(status)['device_name']=='Timer "A" \\ café'
        intent={'type':'command','cap_token':wire,'op':'adjust','op_id':'browser-cpp','params':{'delta_ms':1000},
                'if_boot_id':'aabbccdd','if_session_id':'session-1','expiry_uptime_ms':6000}
        command=channel.encrypt(intent,'browser-cpp')
        response=await api.post('/api/v1/command',headers=headers,json=command)
        assert response.status_code==200,response.text
        result=channel.decrypt(response.json())
        assert parse_authenticated_response(result)['code']=='applied'
        assert cpp.call('inspect')['applies']==1
        assert cpp.call('frame',frame=command)['applies']==1
        lookup=channel.encrypt({'type':'lookup','cap_token':wire},'browser-cpp')
        response=await api.post('/api/v1/command',headers=headers,json=lookup)
        assert channel.decrypt(response.json())['type']=='result'
        assert (await api.get('/api/v1/session',headers=headers)).status_code==200
        assert (await api.get('/api/v1/session')).status_code==401
        assert (await api.get('/s/')).status_code==404
        # The complete relay-facing HTTP/WSS surface and routing rows contain
        # neither the bearer nor fingerprint. Only endpoints hold these.
        assert wire not in json.dumps(bridge.frames)
        assert payload['fp'] not in json.dumps(bridge.frames)
        rows=[dict(r) for r in fresh_db.connect().execute('SELECT * FROM sessions').fetchall()]
        assert wire not in json.dumps(rows) and payload['fp'] not in json.dumps(rows)
    fixture=tmp_path/'cpp-envelopes.json'
    fixture.write_text(json.dumps({'wire':wire,'public_key':info['public_key'],'envelopes':[status,result],'intent':intent}))
    subprocess.run([os.environ.get('NODE','node'),str(ROOT/'tests/verify_cpp_browser.cjs'),str(fixture)],check=True)


def test_python_signatures_verified_by_cpp(cpp):
    signer = ec.generate_private_key(ec.SECP256R1())
    key = b64encode(compressed(signer.public_key()))
    for data in (b'',b'\x00\xff\x80binary', 'Timer "A" café'.encode(), bytes(range(256))*8):
        sig = raw_low_s(signer.sign(data, ec.ECDSA(hashes.SHA256())))
        args = {'key':key,'message':b64encode(data),'signature':b64encode(sig)}
        assert cpp.call('verify_signature',**args)['verified'] == 1
        assert cpp.call('verify_signature',**{**args,'signature':b64encode(bytes([sig[0]^1])+sig[1:])})['verified'] == 0
        # The (r, n - s) twin verifies mathematically but is refused.
        high = sig[:32] + (P256_ORDER - int.from_bytes(sig[32:], 'big')).to_bytes(32, 'big')
        assert cpp.call('verify_signature',**{**args,'signature':b64encode(high)})['verified'] == 0


def test_actual_webcrypto_secure_client(cpp_binary):
    subprocess.run([os.environ.get('NODE','node'),str(ROOT/'tests/verify_secure_cpp.cjs'),str(cpp_binary)],check=True,timeout=30)


def test_python_capability_encoding_and_rejections(cpp):
    wire = cpp.call('inspect')['wire']; payload = parse_wire_token(wire)
    def encode(fields):
        raw = json.dumps(fields,separators=(',',':'),sort_keys=True).encode()
        return 'ost-cap.v1.'+b64encode(raw)+'.'+b64encode(hmac.new(bytes(range(32)),raw,hashlib.sha256).digest()[:16])
    assert cpp.call('verify_token',token=encode(payload))['verified'] == 0
    # The timer's record is authoritative for expiry (owner Extend keeps the
    # URL), so an embedded expiry may be earlier than the record, never later.
    for fields in ({**payload,'aud':'wrong'},{**payload,'tid':'other'},
                   {**payload,'boot':'deadbeef'},{**payload,'exp_up_ms':payload['exp_up_ms']+1}):
        assert cpp.call('verify_token',token=encode(fields))['verified'] != 0
    corrupt = wire.rsplit('.',1)[0]+'.'+b64encode(bytes(16))
    assert cpp.call('verify_token',token=corrupt)['verified'] != 0
