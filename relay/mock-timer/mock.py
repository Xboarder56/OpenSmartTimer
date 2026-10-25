#!/usr/bin/env python3
"""Mock OpenSmartTimer that speaks ost-relay/1.

Purpose: dev-loop the reference relay and the partner SPA without
touching hardware. This is NOT a firmware simulator — it fakes
timer state, capability minting, and the wire protocol, nothing else.

Usage:
    python3 mock.py --relay http://localhost:8080 --code <enrollment_code>
    python3 mock.py --config ~/.config/ostrelay-mock.json

The first form enrolls fresh; the second reuses saved credentials.
On startup the mock enrolls or loads credentials, prints a private loopback
creator address, and maintains its WSS connection with automatic reconnect.
Use the Smart Timer creator page to choose a link lifetime, generate/copy a
confirmed full-control partner link, or revoke it. Ctrl-C stops the mock.

HTTPS enrollment verifies the server certificate. WSS verifies the certificate,
binds the hello to the key it presents and requires the relay's acknowledgement;
--ca accepts a local test CA. Signed status/events/
results use real ECDSA P-256. The simplified timer state machine and bounded RAM
result history are development fakes, not firmware behavior qualification.
"""

from __future__ import annotations

import argparse
import asyncio
import base64
import hashlib
import hmac
import json
import logging
import os
import secrets
import ssl
import struct
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any
from collections import OrderedDict
from secure import timer_accept, enc as secure_enc
from cryptography.exceptions import InvalidTag

from cryptography import x509
from cryptography.hazmat.primitives import serialization
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature

import websockets
from websockets.exceptions import ConnectionClosed, WebSocketException
from websockets.protocol import State as SocketState
try:
    import httpx
except ImportError:  # pragma: no cover
    print("mock.py requires httpx + websockets: pip install httpx websockets",
          file=sys.stderr)
    sys.exit(2)

log = logging.getLogger("mock-timer")


# ---------------------------------------------------------------------------
# Fake TimerController
# ---------------------------------------------------------------------------

@dataclass
class TimerState:
    state: str = "open"                 # open | running | paused
    remaining_ms: int = 0
    duration_ms: int = 0
    sealed: bool = False
    emergency_input_active: bool = False
    max_duration_ms: int = 86_400_000
    boot_id: str = ""
    session_id: str | None = None
    session_counter: int = 0
    state_revision: int = 0
    started_at_up_ms: int = 0
    last_release_reason: str = "boot"

    def snapshot(self, uptime_ms: int, build_id: str) -> dict[str, Any]:
        if self.state == "running":
            elapsed = uptime_ms - self.started_at_up_ms
            self.remaining_ms = max(0, self.duration_ms - elapsed)
            if self.remaining_ms == 0:
                self.expire()
        return {
            "state": self.state,
            "remaining_ms": self.remaining_ms,
            "session_duration_ms": self.duration_ms,
            "sealed": self.sealed,
            # Owner settings; this mock always allows both controls.
            "pause_allowed": True,
            "control_lock_allowed": True,
            "emergency_input_active": self.emergency_input_active,
            "max_duration_ms": self.max_duration_ms,
            "api_version": "v1",
            "version": "1.0.0-rc.3",
            "device_id": "mockmocktimer",
            "hardware_model": "MagBound Smart Timer V2",
            "chip_family": "ESP32-C3",
            "build_id": build_id,
            "boot_id": self.boot_id,
            "session_id": self.session_id,
            "state_revision": self.state_revision,
            "latest_event_id": self.session_counter,
            "device_name": "MockTimer",
            "uptime_ms": uptime_ms,
            "server_time_ms": uptime_ms,
            "last_release_reason": self.last_release_reason,
        }

    # -- Mutation surface used by the WSS command handler --
    def start(self, duration_ms: int, up_ms: int) -> tuple[str, bool]:
        if self.state != "open":
            return ("invalid_state", False)
        if duration_ms <= 0 or duration_ms > self.max_duration_ms:
            return ("invalid_duration", False)
        if self.emergency_input_active:
            return ("emergency_input_active", False)
        self.session_counter += 1
        self.session_id = f"{self.boot_id}-{self.session_counter}"
        self.state = "running"
        self.duration_ms = duration_ms
        self.remaining_ms = duration_ms
        self.started_at_up_ms = up_ms
        self.state_revision += 1
        return ("applied", True)

    def pause(self) -> tuple[str, bool]:
        if self.state != "running":
            return ("invalid_state", False)
        if self.sealed:
            return ("controls_locked", False)
        self.state = "paused"
        self.state_revision += 1
        return ("applied", True)

    def resume(self, up_ms: int) -> tuple[str, bool]:
        if self.state != "paused":
            return ("invalid_state", False)
        if self.emergency_input_active:
            return ("emergency_input_active", False)
        if self.sealed:
            return ("controls_locked", False)
        self.state = "running"
        self.started_at_up_ms = up_ms - (self.duration_ms - self.remaining_ms)
        self.state_revision += 1
        return ("applied", True)

    def adjust(self, delta_ms: int, up_ms: int) -> tuple[str, bool]:
        if self.state not in ("running", "paused"):
            return ("invalid_state", False)
        if self.sealed:
            return ("controls_locked", False)
        new_remaining = self.remaining_ms + delta_ms
        if new_remaining <= 0:
            self.expire()
            return ("applied", True)
        if new_remaining > self.max_duration_ms:
            return ("maximum_duration_reached", False)
        self.remaining_ms = new_remaining
        self.duration_ms = max(self.duration_ms, new_remaining)
        if self.state == "running":
            self.started_at_up_ms = up_ms - (self.duration_ms - self.remaining_ms)
        self.state_revision += 1
        return ("applied", True)

    def release(self) -> tuple[str, bool]:
        if self.sealed:
            return ("controls_locked", False)
        if self.state == "open":
            return ("invalid_state", False)
        self.state = "open"
        self.duration_ms = 0
        self.remaining_ms = 0
        self.sealed = False
        self.last_release_reason = "user"
        self.state_revision += 1
        return ("applied", True)

    def lock_controls(self) -> tuple[str, bool]:
        if self.state not in ("running", "paused"):
            return ("invalid_state", False)
        if self.sealed:
            return ("applied", False)  # already locked; no change
        self.sealed = True
        self.state_revision += 1
        return ("applied", True)

    def expire(self) -> None:
        self.state = "open"
        self.duration_ms = 0
        self.remaining_ms = 0
        self.sealed = False
        self.last_release_reason = "expired"
        self.state_revision += 1


# ---------------------------------------------------------------------------
# Capability token minting/verifying (mirrors firmware CapabilityToken)
# ---------------------------------------------------------------------------

def b64url(value: bytes) -> str:
    return base64.urlsafe_b64encode(value).rstrip(b"=").decode("ascii")


def unb64url(value: str) -> bytes:
    padding = "=" * (-len(value) % 4)
    return base64.urlsafe_b64decode(value + padding)


PARTNER_SCOPES = ["view", "pause_resume", "adjust", "release", "start", "lock", "identify"]


@dataclass
class TokenRecord:
    fp: bytes
    fp_hash: str
    scopes: list[str]
    boot_id: str
    exp_up_ms: int
    token_digest: str = ""
    label: str = ""
    revoked: bool = False
    sync_state: str = "pending"
    sync_seq: int = 0
    sync_sent_at: float = 0
    sync_error: str = ""
    wire: str = ""  # Local creator only; never sent to the relay registration API.
    request_id: str = ""


@dataclass
class CapabilityKeyStore:
    key: bytes                              # 32 bytes
    verify_key: str = ""
    tokens: list[TokenRecord] = field(default_factory=list)

    recent_mints: list[int] = field(default_factory=list)

    def mint(
        self,
        scopes: list[str],
        ttl_ms: int,
        boot_id: str,
        now_up_ms: int,
        timer_id: str,
        label: str,
    ) -> tuple[str, TokenRecord]:
        if "view" not in scopes:
            raise ValueError("view scope is required")
        if not 0 < ttl_ms <= 86_400_000:
            raise ValueError("TTL must be between 1 ms and 24 hours")
        self.tokens[:] = [t for t in self.tokens if t.boot_id == boot_id and t.exp_up_ms > now_up_ms]
        # Retain revoked metadata until expiry for reconnect reconciliation.
        if len(self.tokens) >= 32:
            raise RuntimeError("token table full; wait for a link to expire")
        self.recent_mints[:] = [t for t in self.recent_mints if now_up_ms - t < 3600000]
        if len(self.recent_mints) >= 20:
            raise RuntimeError("mint rate limit: 20 links per hour")
        self.recent_mints.append(now_up_ms)

        fp = secrets.token_bytes(16)
        payload = {
            "aud": "ost-relay/1",
            "tid": timer_id,
            "fp": b64url(fp),
            "sc": scopes,
            "boot": boot_id,
            "exp_up_ms": now_up_ms + ttl_ms,
            "mint_up_ms": now_up_ms,
            "vk": self.verify_key,
        }
        payload_bytes = json.dumps(payload, separators=(",", ":")).encode("utf-8")
        tag = hmac.new(self.key, payload_bytes, hashlib.sha256).digest()[:16]
        wire = f"ost-cap.v1.{b64url(payload_bytes)}.{b64url(tag)}"

        fp_hash = b64url(hashlib.sha256(fp).digest()[:16])
        rec = TokenRecord(fp=fp, fp_hash=fp_hash, scopes=list(scopes),
                          boot_id=boot_id, exp_up_ms=payload["exp_up_ms"],
                          label=label, token_digest=b64url(hashlib.sha256(wire.encode()).digest()))
        self.tokens.append(rec)
        return wire, rec

    def verify(
        self, wire: str, timer_id: str, current_boot_id: str,
        now_up_ms: int, required_scope: str,
    ) -> tuple[str, TokenRecord | None]:
        parts = wire.split(".")
        if len(parts) != 4 or parts[0] != "ost-cap" or parts[1] != "v1":
            return ("invalid_frame", None)
        try:
            payload_bytes = unb64url(parts[2])
            payload = json.loads(payload_bytes)
            tag = unb64url(parts[3])
        except Exception:
            return ("invalid_frame", None)

        expected = hmac.new(self.key, payload_bytes, hashlib.sha256).digest()[:16]
        if not hmac.compare_digest(expected, tag):
            return ("unauthorized", None)
        if payload.get("aud") != "ost-relay/1":
            return ("unauthorized", None)
        if payload.get("tid") != timer_id:
            return ("unauthorized", None)
        if payload.get("boot") != current_boot_id:
            return ("stale_boot", None)

        fp_hash = b64url(hashlib.sha256(unb64url(payload["fp"])).digest()[:16])
        for rec in self.tokens:
            if rec.fp_hash == fp_hash:
                if rec.revoked or rec.exp_up_ms <= now_up_ms:
                    return ("unauthorized", rec)
                if required_scope not in rec.scopes:
                    return ("unauthorized", rec)
                return ("ok", rec)
        return ("unauthorized", None)

    def revoke_by_fp_hash(self, fp_hash: str) -> bool:
        for rec in self.tokens:
            if rec.fp_hash == fp_hash and not rec.revoked:
                rec.revoked = True
                return True
        return False

    def wipe(self) -> list[str]:
        wiped = [t.fp_hash for t in self.tokens if not t.revoked]
        for rec in self.tokens:
            rec.revoked = True
        return wiped


# ---------------------------------------------------------------------------
# Enrollment + persistence
# ---------------------------------------------------------------------------

@dataclass
class MockCredentials:
    timer_id: str
    enrollment_secret: bytes
    wss_endpoint: str
    spki_sha256_pin: str
    capability_key: bytes
    signing_seed: bytes = field(default_factory=lambda: new_p256_secret())
    build_id: str = "aabbccddeeff00112233aabb"

    def to_dict(self) -> dict[str, Any]:
        return {
            "timer_id": self.timer_id,
            "enrollment_secret": b64url(self.enrollment_secret),
            "wss_endpoint": self.wss_endpoint,
            "spki_sha256_pin": self.spki_sha256_pin,
            "capability_key": b64url(self.capability_key),
            "signing_seed": b64url(self.signing_seed),
            "build_id": self.build_id,
        }

    @classmethod
    def from_dict(cls, d: dict[str, Any]) -> "MockCredentials":
        return cls(
            timer_id=d["timer_id"],
            enrollment_secret=unb64url(d["enrollment_secret"]),
            wss_endpoint=d["wss_endpoint"],
            spki_sha256_pin=d["spki_sha256_pin"],
            capability_key=unb64url(d["capability_key"]),
            signing_seed=unb64url(d["signing_seed"]) if "signing_seed" in d else new_p256_secret(),
            build_id=d.get("build_id", "aabbccddeeff00112233aabb"),
        )


async def enroll(relay_url: str, code: str, tls_context: ssl.SSLContext | bool = True) -> MockCredentials:
    async with httpx.AsyncClient(timeout=15.0, verify=tls_context, trust_env=False) as client:
        r = await client.post(f"{relay_url}/api/v1/enroll", json={
            "enrollment_code": code,
            "hardware_model": "MagBound Smart Timer V2",
            "chip_family": "ESP32-C3",
            "build_id": "aabbccddeeff00112233aabb",
        })
        r.raise_for_status()
        body = r.json()
    return MockCredentials(
        timer_id=body["timer_id"],
        enrollment_secret=unb64url(body["enrollment_secret"]),
        wss_endpoint=body["wss_endpoint"],
        spki_sha256_pin=body["spki_sha256_pin"],
        capability_key=secrets.token_bytes(32),
    )


# ---------------------------------------------------------------------------
# WSS session
# ---------------------------------------------------------------------------

NO_TLS_KEY = bytes(32)   # binding value on a connection without TLS (loopback development only)


def hello_tag(secret: bytes, timer_id: str, boot_id: str, uptime_ms: int,
              upstream_seq: int, downstream_seq: int, server_nonce: bytes,
              tls_key: bytes) -> bytes:
    # tls_key: SHA-256 of the SubjectPublicKeyInfo of the leaf certificate seen
    # on this connection. The relay only accepts its own.
    msg = b"".join([
        b"OST-RELAY-HELLO-v2",
        server_nonce,
        timer_id.encode(),
        boot_id.encode(),
        struct.pack(">Q", uptime_ms),
        struct.pack(">Q", upstream_seq),
        struct.pack(">Q", downstream_seq),
        tls_key,
    ])
    return hmac.new(secret, msg, hashlib.sha256).digest()[:16]


def ack_tag(secret: bytes, server_nonce: bytes, tag: bytes) -> bytes:
    return hmac.new(secret, b"OST-RELAY-ACK-v1" + server_nonce + tag, hashlib.sha256).digest()[:16]


P256_ORDER = 0xFFFFFFFF00000000FFFFFFFFFFFFFFFFBCE6FAADA7179E84F3B9CAC2FC632551


def new_p256_secret() -> bytes:
    while True:
        secret = secrets.token_bytes(32)
        if 1 <= int.from_bytes(secret, "big") < P256_ORDER:
            return secret


def p256_private_key(secret: bytes) -> ec.EllipticCurvePrivateKey:
    """Map the stored 32 bytes to a valid P-256 scalar, as the timer does."""
    scalar = int.from_bytes(secret, "big")
    if not 1 <= scalar < P256_ORDER:
        raise ValueError("signing secret is not a valid P-256 scalar")
    return ec.derive_private_key(scalar, ec.SECP256R1())


def p256_sign(key: ec.EllipticCurvePrivateKey, message: bytes) -> bytes:
    """RFC 6979 ECDSA P-256/SHA-256 as raw low-S r || s, matching the timer."""
    r, s = decode_dss_signature(key.sign(message, ec.ECDSA(hashes.SHA256(), deterministic_signing=True)))
    s = min(s, P256_ORDER - s)
    return r.to_bytes(32, "big") + s.to_bytes(32, "big")


class MockTimerSession:
    def __init__(self, cred: MockCredentials, tls_context: ssl.SSLContext | None = None,
                 partner_origin: str = "") -> None:
        self.cred = cred
        self.timer = TimerState(boot_id=secrets.token_hex(4))
        self.signer = p256_private_key(cred.signing_seed)
        verify_key = b64url(self.signer.public_key().public_bytes(
            serialization.Encoding.X962, serialization.PublicFormat.CompressedPoint))
        self.connected = False
        self.keys = CapabilityKeyStore(key=cred.capability_key, verify_key=verify_key)
        self.boot_wall = time.monotonic()
        self.tls_context = tls_context
        self.upstream_seq = 0
        self.last_downstream_seq = 0
        self.ws: websockets.WebSocketClientProtocol | None = None
        self._send_lock = asyncio.Lock()
        self._panic_requested = False
        self.results: OrderedDict[str, tuple[str, dict[str, Any]]] = OrderedDict()
        self.unacked: OrderedDict[int, dict[str, Any]] = OrderedDict()
        self.partner_origin = partner_origin.rstrip('/')
        self.channels = {}
        self.active_channel = None
        self.last_open = float('-inf')

    def uptime_ms(self) -> int:
        return int((time.monotonic() - self.boot_wall) * 1000)

    def next_upstream_seq(self) -> int:
        self.upstream_seq += 1
        return self.upstream_seq

    async def connect(self) -> None:
        options = {"ssl": self.tls_context or ssl.create_default_context()} if self.cred.wss_endpoint.startswith("wss:") else {}
        self.ws = await websockets.connect(
            self.cred.wss_endpoint, subprotocols=["ost-relay.v1"],
            ping_interval=30, ping_timeout=90, max_size=16 * 1024, **options,
        )
        # Like the firmware, bind the hello to the key this connection presented
        # instead of comparing it with a stored pin; the relay's acknowledgement
        # below then proves it is the server this timer enrolled with.
        tls_key = NO_TLS_KEY
        if self.cred.wss_endpoint.startswith("wss:"):
            ssl_socket = self.ws.transport.get_extra_info("ssl_object")
            cert = x509.load_der_x509_certificate(ssl_socket.getpeercert(binary_form=True))
            tls_key = hashlib.sha256(cert.public_key().public_bytes(
                serialization.Encoding.DER, serialization.PublicFormat.SubjectPublicKeyInfo)).digest()
        challenge = json.loads(await asyncio.wait_for(self.ws.recv(), 5))
        nonce = unb64url(challenge.get("server_nonce", ""))
        if challenge.get("type") != "challenge" or len(nonce) != 16:
            raise RuntimeError("expected a fresh 16-byte relay challenge")
        up = self.uptime_ms()
        hello = {
            "type": "hello", "protocol": "ost-relay/1",
            "timer_id": self.cred.timer_id, "boot_id": self.timer.boot_id,
            "uptime_ms": up,
            "resume_from_upstream_seq": self.upstream_seq,
            "resume_from_downstream_seq": self.last_downstream_seq,
        }
        tag = hello_tag(self.cred.enrollment_secret, self.cred.timer_id, self.timer.boot_id,
                        up, self.upstream_seq, self.last_downstream_seq, nonce, tls_key)
        hello["hello_tag"] = b64url(tag)
        await self._send(hello)
        first = json.loads(await asyncio.wait_for(self.ws.recv(), 5))
        if first.get("type") != "hello_ack":
            raise RuntimeError("expected hello_ack")
        try:
            received = unb64url(first.get("ack_tag", ""))
        except (ValueError, TypeError):
            received = b""
        if not hmac.compare_digest(received, ack_tag(self.cred.enrollment_secret, nonce, tag)):
            await self.ws.close()
            raise RuntimeError("the relay did not prove it holds this timer's enrollment secret")
        await self.send_status()
        self.connected = True
        # Replay both live grants and revocation tombstones with fixed expiry.
        for rec in self.keys.tokens:
            if rec.exp_up_ms > self.uptime_ms():
                rec.sync_state = "revoking" if rec.revoked else "pending"
                await self._sync_token(rec)
        log.info("relay accepted hello")

    async def _send(self, obj: dict[str, Any]) -> None:
        assert self.ws is not None
        async with self._send_lock:
            await self.ws.send(json.dumps(obj))

    # -- Outbound frame helpers --
    def response_frame(self, kind: str, body: dict[str, Any]) -> dict[str, Any]:
        seq = self.next_upstream_seq()
        body_json = json.dumps(body, separators=(",", ":"), ensure_ascii=False)
        return {"type": kind, "upstream_seq": seq, "body_json": body_json}

    async def send_status(self) -> None:
        self.sweep_channels()
        for item in list(self.channels.values()):
            if item['confirmed']:
                await self.secure_status(item)

    def sweep_channels(self):
        now = time.monotonic()
        for cid, item in list(self.channels.items()):
            rec = item['record']
            if rec.revoked or rec.exp_up_ms <= self.uptime_ms() or now-item['active'] > (120 if item['confirmed'] else 10):
                del self.channels[cid]

    async def secure_send(self, item, body, request=''):
        frame = item['channel'].encrypt(body,request)
        frame['upstream_seq'] = self.next_upstream_seq()
        await self._send(frame)

    async def secure_status(self, item, request=''):
        snap = self.timer.snapshot(self.uptime_ms(),self.cred.build_id)
        snap['share_link_expires_up_ms'] = item['record'].exp_up_ms
        await self.secure_send(item,self.response_frame('status',snap),request)

    async def handle_secure(self, frame):
        self.sweep_channels()
        try:
            if frame['type'] == 'e2e_open':
                cid = frame['client_id']
                old = self.channels.get(cid)
                if old:
                    if frame == old['open']:
                        await self._send({**old['accept'],'upstream_seq':self.next_upstream_seq()})
                    return
                rec = next((r for r in self.keys.tokens if r.fp_hash == frame['fp_hash'] and not r.revoked and r.exp_up_ms > self.uptime_ms()),None)
                if not rec or len(self.channels) >= 8 or sum(not i['confirmed'] for i in self.channels.values()) >= 2:
                    return
                if time.monotonic()-self.last_open < 1:
                    return
                accepted, channel = timer_accept(frame,self.cred.timer_id,self.timer.boot_id,rec.fp,
                                                lambda message: p256_sign(self.signer,message))
                self.last_open = time.monotonic()
                self.channels[cid] = dict(open=frame,accept=accepted,channel=channel,record=rec,confirmed=False,active=time.monotonic())
                await self._send({**accepted,'upstream_seq':self.next_upstream_seq()})
                return
            item = self.channels.get(frame['client_id'])
            if not item:
                return
            body = item['channel'].decrypt(frame)
            verdict,rec = self.keys.verify(body.get('cap_token',''),self.cred.timer_id,self.timer.boot_id,self.uptime_ms(),'view')
            if verdict != 'ok' or rec is not item['record']:
                return
            item['confirmed'] = True
            item['active'] = time.monotonic()
            request = frame['request_id']
            self.active_channel = item
            if body['type'] == 'sync':
                await self.secure_status(item,request)
            elif body['type'] == 'lookup':
                previous = self.results.get(request)
                if previous:
                    await self.secure_send(item,self.response_frame('result',previous[1]),request)
            elif body['type'] == 'command' and body.get('downstream_seq') == frame['seq'] and body.get('op_id') == request:
                await self.handle_command(body)
        except (ValueError,KeyError,TypeError,InvalidTag):
            return
        finally:
            self.active_channel = None

    async def _sync_token(self, rec: TokenRecord) -> None:
        if rec.exp_up_ms <= self.uptime_ms():
            rec.sync_state = "expired"
            return
        if self.ws is None:
            return
        rec.sync_seq = self.next_upstream_seq()
        rec.sync_sent_at = time.monotonic()
        try:
            await self._send({
                "type": "token_revoke" if rec.revoked else "token_mint",
                "upstream_seq": rec.sync_seq, "fp_hash": rec.fp_hash,
                "token_digest": rec.token_digest, "scopes": rec.scopes,
                "boot_id": rec.boot_id, "exp_up_ms": rec.exp_up_ms, "label": rec.label,
            })
        except (OSError, RuntimeError, ConnectionClosed):
            rec.sync_error = "Waiting for relay connection"

    async def send_token_mint(self, rec: TokenRecord) -> None:
        rec.sync_state = "revoking" if rec.revoked else "pending"
        await self._sync_token(rec)

    async def send_token_revoke(self, fp_hash: str) -> None:
        rec = next((r for r in self.keys.tokens if r.fp_hash == fp_hash), None)
        if rec is None:
            raise ValueError("unknown link")
        rec.revoked = True  # Local authorization stops immediately.
        rec.sync_state = "revoking"
        await self._sync_token(rec)

    def handle_token_ack(self, frame: dict[str, Any]) -> None:
        for rec in self.keys.tokens:
            if (frame.get("fp_hash") == rec.fp_hash and frame.get("boot_id") == rec.boot_id
                    and type(frame.get("upstream_seq")) is int and frame["upstream_seq"] == rec.sync_seq
                    and frame.get("action") == ("token_revoke" if rec.revoked else "token_mint")):
                status = frame.get("status")
                if status == "ready" and not rec.revoked:
                    rec.sync_state, rec.sync_error = "ready", ""
                elif status == "revoked":
                    rec.revoked = True
                    rec.sync_state, rec.sync_error = "revoked", ""
                elif status == "expired":
                    rec.sync_state = "expired"
                elif status == "capacity":
                    rec.sync_error = "Relay capacity reached; retrying"
                elif status in ("conflict", "unknown"):
                    rec.sync_state, rec.sync_error = "error", status
                return

    async def retry_token_sync(self) -> None:
        for rec in self.keys.tokens:
            if rec.exp_up_ms <= self.uptime_ms():
                rec.sync_state = "expired"
            elif rec.sync_state in ("pending", "revoking") and time.monotonic() - rec.sync_sent_at >= 2:
                await self._sync_token(rec)

    def can_share(self) -> bool:
        return bool(self.connected and self.ws is not None and self.ws.state == SocketState.OPEN)

    async def create_partner_link(self, ttl_seconds: int, label: str, request_id: str) -> TokenRecord:
        if type(ttl_seconds) is not int or not 1 <= ttl_seconds <= 86400:
            raise ValueError("Choose a link lifetime between 1 second and 24 hours")
        if not isinstance(label, str) or len(label) > 23:
            raise ValueError("Label must be at most 23 characters")
        if not isinstance(request_id, str) or not 1 <= len(request_id) <= 64:
            raise ValueError("A request ID is required")
        if not self.can_share():
            raise ConnectionError("Connect the timer to the network and relay before sharing")
        existing = next((r for r in self.keys.tokens if r.request_id == request_id), None)
        if existing:
            payload = json.loads(unb64url(existing.wire.split('.')[2]))
            if payload["exp_up_ms"] - payload["mint_up_ms"] != ttl_seconds * 1000 or existing.label != label:
                raise ValueError("Request ID already used with different settings")
            return existing
        wire, rec = self.keys.mint(PARTNER_SCOPES, ttl_seconds * 1000, self.timer.boot_id,
                                   self.uptime_ms(), self.cred.timer_id, label)
        rec.wire, rec.request_id = wire, request_id
        await self.send_token_mint(rec)
        return rec

    def link_info(self, rec: TokenRecord, public_url: str) -> dict[str, Any]:
        state = "expired" if rec.exp_up_ms <= self.uptime_ms() else rec.sync_state
        if state == "ready" and not self.can_share():
            state = "offline"
        return {"fp_hash": rec.fp_hash, "label": rec.label, "state": state,
                "remaining_seconds": max(0, (rec.exp_up_ms - self.uptime_ms()) // 1000),
                "error": rec.sync_error,
                "url": self.partner_origin + "/s/" + rec.fp_hash + "#relay=" + b64url(public_url.rstrip('/').encode()) + "&cap=" + rec.wire
                    if state == "ready" and rec.wire and self.partner_origin else None}

    async def send_result(self, dseq: int, op: str, op_id: str, code: str,
                          changed: bool, *, duplicate: bool = False,
                          snapshot: dict[str, Any] | None = None,
                          intent_fingerprint: str | None = None) -> dict[str, Any]:
        snap = snapshot if snapshot is not None else self.timer.snapshot(self.uptime_ms(), self.cred.build_id)
        body = {
            "in_reply_to_downstream_seq": dseq, "op": op, "op_id": op_id, "code": code,
            "mutation": {"action": op, "changed": changed, "duplicate": duplicate, "operation_id": op_id},
            "status": snap,
        }
        frame = self.response_frame("result", body)
        if intent_fingerprint is not None:
            # Record before sending: lost delivery must not permit a second actuation.
            self.results[op_id] = (intent_fingerprint, body)
            while len(self.results) > 8:
                self.results.popitem(last=False)
        if self.active_channel is not None:
            await self.secure_send(self.active_channel,frame,op_id)
        return body

    async def send_pong(self) -> None:
        await self._send({"type": "pong"})

    # -- Downstream frame dispatch --
    SCOPE_FOR_OP = {
        "start": "start", "release": "release",
        "pause": "pause_resume", "resume": "pause_resume",
        "adjust": "adjust", "lock_controls": "lock", "identify": "identify",
    }

    async def handle_command(self, frame: dict[str, Any]) -> None:
        dseq = int(frame.get("downstream_seq", 0))
        op = str(frame.get("op", ""))
        op_id = str(frame.get("op_id", ""))
        cap_token = str(frame.get("cap_token", ""))
        expiry = int(frame.get("expiry_uptime_ms", 0))
        params = frame.get("params") or {}

        required_scope = self.SCOPE_FOR_OP.get(op)
        if required_scope is None:
            await self.send_result(dseq, op, op_id, "unsupported_opcode", False)
            return

        up = self.uptime_ms()
        self.last_downstream_seq = max(self.last_downstream_seq, dseq)
        verdict, _ = self.keys.verify(
            cap_token, self.cred.timer_id, self.timer.boot_id, up, required_scope,
        )
        if verdict != "ok":
            await self.send_result(dseq, op, op_id, verdict, False)
            return

        fingerprint = json.dumps({k: frame.get(k) for k in (
            "op", "params", "if_boot_id", "if_session_id", "expiry_uptime_ms")}, sort_keys=True)
        previous = self.results.get(op_id)
        if previous:
            signature, result = previous
            if signature != fingerprint:
                await self.send_result(dseq, op, op_id, "operation_conflict", False)
            else:
                await self.send_result(dseq, op, op_id, result["code"], result["mutation"]["changed"],
                                       duplicate=True, snapshot=result["status"])
            return
        if not 0 < dseq < 2**53 or not 1 <= len(op_id) <= 64:
            await self.send_result(dseq, op, op_id, "invalid_frame", False)
            return
        if up >= expiry or expiry - up > 10000:
            await self.send_result(dseq, op, op_id, "expired", False)
            return
        if frame.get("if_boot_id") != self.timer.boot_id:
            await self.send_result(dseq, op, op_id, "stale_boot", False)
            return
        if op not in ("start", "identify") and frame.get("if_session_id") != self.timer.session_id:
            await self.send_result(dseq, op, op_id, "stale_session", False)
            return
        self.timer.snapshot(up, self.cred.build_id)

        if op == "start":
            code, changed = self.timer.start(int(params.get("duration_ms", 0)), up)
        elif op == "pause":
            code, changed = self.timer.pause()
        elif op == "resume":
            code, changed = self.timer.resume(up)
        elif op == "adjust":
            code, changed = self.timer.adjust(int(params.get("delta_ms", 0)), up)
        elif op == "release":
            code, changed = self.timer.release()
        elif op == "lock_controls":
            code, changed = self.timer.lock_controls()
        elif op == "identify":
            code, changed = ("applied", True)
        else:
            code, changed = ("unsupported_opcode", False)

        await self.send_result(dseq, op, op_id, code, changed, intent_fingerprint=fingerprint)
        if changed:
            await self.send_status()

    async def read_loop(self) -> None:
        assert self.ws is not None
        try:
            async for raw in self.ws:
                frame = json.loads(raw)
                ftype = frame.get("type")
                if ftype in ("e2e_open","e2e"):
                    await self.handle_secure(frame)
                elif ftype == "ping":
                    await self.send_pong()
                elif ftype == "token_ack":
                    self.handle_token_ack(frame)
                elif ftype == "ack":
                    seq = frame.get("acked_upstream_seq", 0)
                    if type(seq) is int and 0 <= seq <= self.upstream_seq:
                        for retained in list(self.unacked):
                            if retained <= seq:
                                del self.unacked[retained]
        except ConnectionClosed:
            return
        finally:
            self.connected = False
            for rec in self.keys.tokens:
                if rec.sync_state in ("ready", "revoked"):
                    rec.sync_state = "revoking" if rec.revoked else "pending"

    async def periodic_status(self, interval_s: float = 2.0) -> None:
        while True:
            await asyncio.sleep(interval_s)
            try:
                await self.send_status()
                await self.retry_token_sync()
            except Exception:
                return

    async def maintain_connection(self) -> None:
        delay = 1
        while not self._panic_requested:
            tasks = []
            try:
                await self.connect()
                delay = 1
                tasks = [asyncio.create_task(self.read_loop()), asyncio.create_task(self.periodic_status())]
                done, _ = await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
                for task in done:
                    task.result()
            except (OSError, RuntimeError, ValueError, asyncio.TimeoutError, WebSocketException) as exc:
                log.warning("Relay unavailable (%s); reconnecting", type(exc).__name__)
            finally:
                for task in tasks:
                    task.cancel()
                await asyncio.gather(*tasks, return_exceptions=True)
                self.connected = False
                if self.ws:
                    await self.ws.close()
                self.ws = None
            if not self._panic_requested:
                await asyncio.sleep(delay)
                delay = min(30, delay * 2)

    async def panic(self) -> None:
        """Owner-triggered emergency shutdown: revoke everything, close WSS."""
        log.warning("PANIC: revoking all tokens and dropping WSS")
        wiped = self.keys.wipe()
        for fp_hash in wiped:
            try:
                await self.send_token_revoke(fp_hash)
            except Exception:
                break
        self._panic_requested = True
        self.channels.clear()
        if self.ws is not None:
            try:
                await self.ws.close(code=4010, reason="emergency_revoke")
            except Exception:
                pass


# ---------------------------------------------------------------------------
# CLI / interactive prompt
# ---------------------------------------------------------------------------

async def run(args: argparse.Namespace) -> int:
    config_path = Path(args.config).expanduser() if args.config else None
    cred: MockCredentials
    if config_path and config_path.is_file():
        cred = MockCredentials.from_dict(json.loads(config_path.read_text()))
        log.info("loaded credentials from %s", config_path)
    else:
        if not args.code:
            print("first run requires --code <enrollment_code>", file=sys.stderr)
            return 2
        cred = await enroll(args.relay, args.code,
                            ssl.create_default_context(cafile=args.ca) if args.ca else True)
        if config_path:
            config_path.parent.mkdir(parents=True, exist_ok=True)
            config_path.write_text(json.dumps(cred.to_dict(), indent=2))
            os.chmod(config_path, 0o600)
            log.info("saved credentials to %s", config_path)

    session = MockTimerSession(cred, ssl.create_default_context(cafile=args.ca) if args.ca else None,args.partner_origin)
    from owner import create_owner_app
    import uvicorn
    owner_secret = secrets.token_urlsafe(32)
    app = create_owner_app(session, args.relay, owner_secret, args.owner_port)
    print(f"[creator] http://127.0.0.1:{args.owner_port}/#{owner_secret}", flush=True)
    server = uvicorn.Server(uvicorn.Config(app, host="127.0.0.1", port=args.owner_port, access_log=False))
    connection = asyncio.create_task(session.maintain_connection())
    try:
        await server.serve()
    finally:
        connection.cancel()
        await asyncio.gather(connection, return_exceptions=True)
        await session.panic()
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="ost-relay/1 mock timer")
    parser.add_argument("--relay", default="http://localhost:8080",
                        help="Relay base URL (default: http://localhost:8080)")
    parser.add_argument("--code", help="One-shot enrollment code from the relay admin")
    parser.add_argument("--partner-origin", required=True, help="Independent trusted HTTPS partner origin")
    parser.add_argument("--ca", help="CA PEM file for an isolated local HTTPS stack")
    parser.add_argument("--config",
                        default="~/.config/ostrelay-mock/credentials.json",
                        help="Path to persist enrollment credentials")
    parser.add_argument("--owner-port", type=int, default=8081, help="Loopback creator UI port (default: 8081)")
    parser.add_argument("--log-level", default="INFO")
    args = parser.parse_args()
    from urllib.parse import urlsplit
    partner = urlsplit(args.partner_origin)
    if partner.scheme != 'https' or not partner.hostname or partner.username or partner.password or partner.path or partner.query or partner.fragment or args.partner_origin.rstrip('/') == args.relay.rstrip('/'):
        parser.error('--partner-origin must be an independent HTTPS origin')

    logging.basicConfig(level=args.log_level.upper(),
                        format="%(asctime)s %(name)s %(levelname)s %(message)s")
    try:
        return asyncio.run(run(args))
    except KeyboardInterrupt:
        return 130


if __name__ == "__main__":
    sys.exit(main())
