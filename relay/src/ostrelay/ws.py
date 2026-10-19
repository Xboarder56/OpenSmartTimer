"""Single-process timer transport: bounded challenge authentication and routing.

Encrypted payloads remain opaque. Only public routing and grant metadata
are parsed; capabilities and traffic keys never enter this process.
"""
from __future__ import annotations

import asyncio
import hashlib
import hmac
import json
import secrets
import struct
import time
from dataclasses import dataclass, field
from typing import Any

from fastapi import APIRouter, HTTPException, WebSocket, WebSocketDisconnect

from . import db, tlsid
from .config import settings
from .e2e import validate_frame
from .wire import b64decode as _b64url_decode, b64encode as _b64url_encode, integer, loads, scopes_valid

router = APIRouter()


@dataclass
class WsSession:
    timer_id: str
    ws: WebSocket | None
    session_id: str
    boot_id: str = ""
    last_acked_upstream_seq: int = 0
    last_activity_wall: float = 0.0
    last_activity_mono: float = 0.0
    subscribers: set[asyncio.Queue[dict[str, Any]]] = field(default_factory=set)
    hello_uptime_ms: int = 0
    hello_mono: float = 0.0
    opaque_pending: dict[tuple[str, str], asyncio.Future] = field(default_factory=dict)
    send_lock: asyncio.Lock = field(default_factory=asyncio.Lock)

    async def send(self, frame: dict[str, Any]) -> None:
        """Bounded write: a peer that stops reading cannot hold the caller.
        A timed-out (possibly partial) write poisons the connection, so close it."""
        try:
            await asyncio.wait_for(self._send_locked(frame), timeout=settings().ws_send_timeout_s)
        except asyncio.TimeoutError:
            asyncio.get_running_loop().create_task(self.abort(1011, "send_timeout"))
            raise

    async def _send_locked(self, frame: dict[str, Any]) -> None:
        async with self.send_lock:
            await self.ws.send_json(frame)

    async def abort(self, code: int, reason: str) -> None:
        try:
            await asyncio.wait_for(self.ws.close(code=code, reason=reason), timeout=2.0)
        except Exception:  # noqa: BLE001 - already failing; the read loop ends it
            pass


class WsRegistry:
    def __init__(self) -> None:
        self._sessions: dict[str, WsSession] = {}
        self._lock = asyncio.Lock()
        self.handshakes = 0

    async def register(self, session: WsSession) -> WsSession | None:
        async with self._lock:
            previous = self._sessions.get(session.timer_id)
            if previous is None and len(self._sessions) >= settings().max_connected_timers:
                raise ValueError("timer_capacity")
            self._sessions[session.timer_id] = session
            return previous

    async def deregister(self, session: WsSession) -> None:
        async with self._lock:
            if self._sessions.get(session.timer_id) is session:
                del self._sessions[session.timer_id]

    def get(self, timer_id: str) -> WsSession | None:
        return self._sessions.get(timer_id)

    async def close_unenrolled(self) -> None:
        """Disconnect timers whose enrollment was erased by another process
        (``python -m ostrelay.setup_code --revoke``); the admin API closes
        its own immediately."""
        if not self._sessions:
            return
        enrolled = {r["timer_id"] for r in db.connect().execute("SELECT timer_id FROM timers").fetchall()}
        for tid, session in list(self._sessions.items()):
            if tid not in enrolled:
                await session.abort(4003, "enrollment_erased")

    def cleanup(self) -> None:
        # Requests own their deadline and release their routing slot in finally.
        pass


registry = WsRegistry()


def _hello_tag(enrollment_secret: bytes, timer_id: str, boot_id: str,
               uptime_ms: int, upstream_seq: int, downstream_seq: int,
               server_nonce: bytes, tls_key: bytes) -> bytes:
    # tls_key is the SHA-256 of the leaf SPKI the timer saw. The timer does not
    # validate the certificate; a hello relayed through another TLS endpoint
    # carries that endpoint's key and fails here.
    message = (b"OST-RELAY-HELLO-v2" + server_nonce + timer_id.encode("ascii")
               + boot_id.encode("ascii") + struct.pack(">QQQ", uptime_ms, upstream_seq, downstream_seq)
               + tls_key)
    return hmac.new(enrollment_secret, message, hashlib.sha256).digest()[:16]


def _ack_tag(enrollment_secret: bytes, server_nonce: bytes, hello_tag: bytes) -> bytes:
    # Proves to the timer that this relay holds its enrollment secret and
    # accepted the hello above, and with it the TLS key that hello names.
    return hmac.new(enrollment_secret, b"OST-RELAY-ACK-v1" + server_nonce + hello_tag,
                    hashlib.sha256).digest()[:16]


async def _verify_hello(hello: dict[str, Any], server_nonce: bytes) -> dict[str, Any] | None:
    import re
    try:
        if set(hello) != {"type", "protocol", "timer_id", "boot_id", "uptime_ms",
                          "resume_from_upstream_seq", "resume_from_downstream_seq", "hello_tag"}:
            return None
        if hello["type"] != "hello" or hello["protocol"] != "ost-relay/1":
            return None
        if not re.fullmatch(r"[A-Za-z0-9_-]{1,64}", hello["timer_id"]):
            return None
        if not re.fullmatch(r"[0-9a-f]{1,16}", hello["boot_id"]):
            return None
        if not all(integer(hello[k]) for k in ("uptime_ms", "resume_from_upstream_seq", "resume_from_downstream_seq")):
            return None
        tag = _b64url_decode(hello["hello_tag"], 16)
        row = db.connect().execute("SELECT * FROM timers WHERE timer_id=?", (hello["timer_id"],)).fetchone()
        if row is None or len(server_nonce) != 16:
            return None
        secret = bytes(row["enrollment_secret"])
        matched = False
        for tls_key in tlsid.pins():
            expected = _hello_tag(secret, hello["timer_id"], hello["boot_id"],
                                  hello["uptime_ms"], hello["resume_from_upstream_seq"],
                                  hello["resume_from_downstream_seq"], server_nonce, tls_key)
            matched = hmac.compare_digest(expected, tag) or matched
        if not matched:
            # The certificate may have been renewed since the last look.
            if tlsid.follows_certificate():
                tlsid.request_refresh()
            return None
        return {**dict(row), "ack_tag": _b64url_encode(_ack_tag(secret, server_nonce, tag))}
    except (KeyError, TypeError, ValueError, UnicodeError):
        return None


async def _receive(socket: WebSocket) -> dict[str, Any]:
    msg = await socket.receive()
    if msg["type"] == "websocket.disconnect":
        raise WebSocketDisconnect(msg.get("code", 1000))
    if msg.get("text") is None:
        raise ValueError("text frames required")
    return loads(msg["text"], settings().max_frame_bytes)


async def _close_socket(socket: WebSocket, code: int, reason: str = "") -> None:
    try:
        await asyncio.wait_for(socket.close(code=code, reason=reason), timeout=2.0)
    except (asyncio.TimeoutError, WebSocketDisconnect, OSError, RuntimeError):
        pass


@router.websocket("/api/v1/timer")
async def timer_socket(socket: WebSocket) -> None:
    if "ost-relay.v1" not in socket.scope.get("subprotocols", []):
        await _close_socket(socket, 1008)
        return
    if registry.handshakes >= settings().max_ws_handshakes:
        await _close_socket(socket, 1013)
        return
    registry.handshakes += 1
    try:
        try:
            # One deadline covers acceptance, challenge write and hello read.
            # A peer that never reads must not occupy an authentication slot.
            async with asyncio.timeout(settings().hello_timeout_s):
                await socket.accept(subprotocol="ost-relay.v1")
                nonce = secrets.token_bytes(16)
                await socket.send_json({"type": "challenge", "server_nonce": _b64url_encode(nonce)})
                first = await _receive(socket)
                timer_row = await _verify_hello(first, nonce)
        except (asyncio.TimeoutError, WebSocketDisconnect, ValueError, RecursionError, UnicodeError, OSError, RuntimeError):
            await _close_socket(socket, 4000, "invalid_hello")
            return
        if timer_row is None:
            await _close_socket(socket, 4003, "unauthorized")
            return
    finally:
        registry.handshakes -= 1

    session = WsSession(timer_id=timer_row["timer_id"], ws=socket, session_id=secrets.token_urlsafe(16),
                        boot_id=first["boot_id"], last_activity_wall=time.time(), last_activity_mono=time.monotonic(),
                        hello_uptime_ms=first["uptime_ms"], hello_mono=time.monotonic())
    try:
        displaced = await registry.register(session)
    except ValueError:
        await _close_socket(socket, 1013, "timer_capacity")
        return
    # From here on the slot is held: every exit path must release it,
    # including a failed handshake reply or a stuck displaced connection.
    tasks: list[asyncio.Task[Any]] = []
    close_code = None
    try:
        if displaced:
            await displaced.abort(4009, "displaced_by_new_hello")
        # A new boot invalidates old grants, retained rows cannot refill the cap.
        db.connect().execute("DELETE FROM tokens WHERE timer_id=? AND boot_id<>?", (session.timer_id, session.boot_id))
        await session.send({"type": "hello_ack", "session_id": session.session_id,
                            "resume_ok": False, "server_time_unix": db.now_unix(),
                            "ack_tag": timer_row["ack_tag"]})
        db.connect().execute("UPDATE timers SET last_seen_at=? WHERE timer_id=?", (db.now_unix(), session.timer_id))
        tasks = [asyncio.create_task(_heartbeat(session)), asyncio.create_task(_ack_flusher(session))]
        close_code = await _read_loop(session)
    except (WebSocketDisconnect, asyncio.TimeoutError, OSError, RuntimeError):
        pass  # connection failed mid-handshake or mid-session; cleaned up below
    finally:
        for task in tasks:
            task.cancel()
        await asyncio.gather(*tasks, return_exceptions=True)
        await registry.deregister(session)
        if close_code == 4010:
            db.connect().execute("UPDATE tokens SET revoked=1 WHERE timer_id=?", (session.timer_id,))
        _fanout(session, {"type": "token_revoked" if close_code == 4010 else "timer_gone"})


async def _read_loop(session: WsSession) -> int | None:
    budget = settings().rate_timer_frames_per_min
    window_start, frames = time.monotonic(), 0
    while True:
        try:
            frame = await _receive(session.ws)
            now = time.monotonic()
            if now - window_start >= 60:
                window_start, frames = now, 0
            frames += 1
            if frames > budget:  # a flooding timer is disconnected, not throttled
                await session.abort(4008, "rate_limited")
                return 4008
            ftype = frame.get("type")
            if ftype == "pong":
                if set(frame) != {"type"}:
                    raise ValueError("bad pong")
            else:
                seq = frame.get("upstream_seq")
                if not integer(seq, 1):
                    raise ValueError("bad upstream sequence")
                if ftype in ("e2e", "e2e_accept"):
                    _handle_opaque(session,frame)
                elif ftype == "token_mint":
                    await _sync_token(session, frame)
                elif ftype == "token_revoke":
                    await _sync_token(session, frame)
                else:
                    raise ValueError("unknown frame")
                session.last_acked_upstream_seq = max(session.last_acked_upstream_seq, seq)
            session.last_activity_wall = time.time()
            session.last_activity_mono = time.monotonic()
        except WebSocketDisconnect as exc:
            return exc.code
        except (ValueError, TypeError, KeyError, UnicodeError, RecursionError):
            await session.ws.close(code=4001, reason="invalid_frame")
            return 4001


def _handle_opaque(session: WsSession, frame: dict[str, Any]) -> None:
    validate_frame(frame, upstream=True)
    request=frame["client_id"] if frame["type"]=="e2e_accept" else frame["request_id"]
    future=session.opaque_pending.get((frame["client_id"],request))
    if future is not None and not future.done():
        future.set_result(frame)
    _fanout(session,frame)


def _fanout(session: WsSession, envelope: dict[str, Any]) -> None:
    for queue in list(session.subscribers):
        try:
            queue.put_nowait(envelope)
        except asyncio.QueueFull:
            session.subscribers.discard(queue)
            # Make room for an explicit terminal event; do not leave a silent orphan.
            while not queue.empty():
                queue.get_nowait()
            queue.put_nowait({"type": "slow_consumer"})


def _persist_token_mint(session: WsSession, frame: dict[str, Any], *, revoked: bool = False) -> str:
    _b64url_decode(frame.get("fp_hash"), 16)
    _b64url_decode(frame.get("token_digest"), 32)
    if (not scopes_valid(frame.get("scopes")) or frame.get("boot_id") != session.boot_id
            or not integer(frame.get("exp_up_ms"), 1)
            or not isinstance(frame.get("label", ""), str) or len(frame.get("label", "")) > 64):
        raise ValueError("bad token mint")
    # Prune only grants whose expiry has actually passed in this boot. Keep
    # unexpired revoked rows as tombstones so reannouncing cannot revive them.
    confirmed_uptime = session.hello_uptime_ms + (int(max(0,time.monotonic()-session.hello_mono)*1000) if session.hello_mono else 0)
    if frame["exp_up_ms"] <= confirmed_uptime:
        return "expired"
    with db.transaction() as tx:
        # Keep the grant being announced: an owner may extend (reactivate) an
        # expired link, and its browser sessions should survive that.
        tx.execute("DELETE FROM tokens WHERE timer_id=? AND boot_id=? AND exp_up_ms<=? AND fp_hash<>?",
                   (session.timer_id, session.boot_id, confirmed_uptime, frame["fp_hash"]))
        existing = tx.execute("SELECT * FROM tokens WHERE fp_hash=?", (frame["fp_hash"],)).fetchone()
        if existing:
            # A re-announcement cannot move a grant between timers, change its
            # token, boot or scopes, or undo revocation. Its expiry follows the
            # authenticated timer, which owns it (owner Extend keeps the URL).
            if (existing["timer_id"] != session.timer_id or existing["token_digest"] != frame["token_digest"]
                    or existing["boot_id"] != frame["boot_id"]
                    or json.loads(existing["scope_json"]) != frame["scopes"]):
                raise ValueError("conflicting mint")
            if revoked:
                tx.execute("UPDATE tokens SET revoked=1 WHERE fp_hash=?", (frame["fp_hash"],))
                tx.execute("DELETE FROM sessions WHERE fp_hash=?", (frame["fp_hash"],))
            elif not existing["revoked"] and existing["exp_up_ms"] != frame["exp_up_ms"]:
                tx.execute("UPDATE tokens SET exp_up_ms=? WHERE fp_hash=?", (frame["exp_up_ms"], frame["fp_hash"]))
            return "revoked" if revoked or existing["revoked"] else "ready"
        count = tx.execute("SELECT count(*) AS n FROM tokens WHERE timer_id=?", (session.timer_id,)).fetchone()["n"]
        if count >= settings().max_tokens_per_timer:
            raise ValueError("token capacity")
        tx.execute("INSERT INTO tokens (fp_hash,timer_id,token_digest,scope_json,boot_id,exp_up_ms,label,minted_at) "
                   "VALUES (?,?,?,?,?,?,?,?)", (frame["fp_hash"], session.timer_id, frame["token_digest"],
                   json.dumps(frame["scopes"]), frame["boot_id"], frame["exp_up_ms"], frame.get("label", ""), db.now_unix()))
        if revoked:
            tx.execute("UPDATE tokens SET revoked=1 WHERE fp_hash=?", (frame["fp_hash"],))
    return "revoked" if revoked else "ready"


def _persist_token_revoke(session: WsSession, frame: dict[str, Any]) -> str:
    # Full metadata creates a tombstone even when revoke arrives before mint,
    # or after a relay restore. A later mint can never revive the grant.
    if "token_digest" in frame:
        return _persist_token_mint(session, frame, revoked=True)
    _b64url_decode(frame.get("fp_hash"), 16)
    with db.transaction() as tx:
        row = tx.execute("SELECT timer_id FROM tokens WHERE fp_hash=?", (frame["fp_hash"],)).fetchone()
        if not row or row["timer_id"] != session.timer_id:
            return "unknown"
        tx.execute("UPDATE tokens SET revoked=1 WHERE fp_hash=? AND timer_id=?", (frame["fp_hash"], session.timer_id))
        tx.execute("DELETE FROM sessions WHERE fp_hash=?", (frame["fp_hash"],))
    return "revoked"


async def _sync_token(session: WsSession, frame: dict[str, Any]) -> None:
    try:
        status = (_persist_token_mint(session, frame) if frame["type"] == "token_mint"
                  else _persist_token_revoke(session, frame))
    except ValueError as exc:
        if str(exc) not in ("token capacity", "conflicting mint"):
            raise
        status = "capacity" if str(exc) == "token capacity" else "conflict"
    # This ACK is emitted only AFTER the SQLite transaction commits. Generic
    # upstream ACKs remain transport receipts, never share-link confirmation.
    await session.send({"type": "token_ack", "action": frame["type"],
                        "upstream_seq": frame["upstream_seq"], "fp_hash": frame["fp_hash"],
                        "boot_id": session.boot_id, "status": status})


async def _ack_flusher(session: WsSession) -> None:
    last_sent = 0
    while True:
        await asyncio.sleep(1.0)
        current = session.last_acked_upstream_seq
        if current > last_sent:
            await session.send({"type": "ack", "acked_upstream_seq": current})
            last_sent = current


async def _heartbeat(session: WsSession) -> None:
    cfg = settings()
    while True:
        await asyncio.sleep(cfg.ws_ping_interval_s)
        elapsed = (time.monotonic() - session.last_activity_mono if session.last_activity_mono
                   else time.time() - session.last_activity_wall)
        if elapsed > cfg.ws_ping_timeout_s:
            await session.ws.close(code=4004, reason="ping_timeout")
            return
        await session.send({"type": "ping"})


async def send_opaque(session: WsSession, frame: dict[str, Any]) -> dict[str, Any]:
    validate_frame(frame)
    request=frame["client_id"] if frame["type"]=="e2e_open" else frame["request_id"]
    if not request: raise HTTPException(400,"request_id_required")
    key=(frame["client_id"],request)
    if key in session.opaque_pending or len(session.opaque_pending)>=settings().max_pending_per_timer:
        raise HTTPException(429,"pending_capacity")
    future=asyncio.get_running_loop().create_future()
    session.opaque_pending[key]=future
    try:
        async with asyncio.timeout(settings().command_await_s):
            if frame["type"]=="e2e_open":
                # Device ECC admission is deliberately limited to one new
                # channel/second. Retrying this identical authenticated open
                # also recovers a lost accept without creating another key.
                while True:
                    await session.send(frame)
                    try:
                        return await asyncio.wait_for(asyncio.shield(future),1.05)
                    except asyncio.TimeoutError:
                        continue
            await session.send(frame)
            return await future
    except (asyncio.TimeoutError,WebSocketDisconnect,OSError,RuntimeError):
        raise HTTPException(504,"timer_offline_or_slow") from None
    finally:
        session.opaque_pending.pop(key,None)
