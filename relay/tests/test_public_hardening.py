"""Internet-facing hardening: connection slots, deadlines, SSE admission,
owner-code races and open-enrollment capacity (review of 2026-09-29)."""

from __future__ import annotations

import asyncio
import secrets
import time

import pytest
from ostrelay.wire import b64encode

from ostrelay import db as db_mod
from ostrelay import ws
from ostrelay.wire import b64decode
from test_ws_hello import hello, seed

ENROLL = {"hardware_model": "MagBound Smart Timer V2", "chip_family": "ESP32-C3",
          "build_id": "aabbccddeeff00112233aabb"}


def _configure(monkeypatch, **values):
    from ostrelay import config
    for key, value in values.items():
        monkeypatch.setattr(config._settings, key, value)


# 1. A failed handshake reply must not keep the timer's slot -----------------

def test_failed_hello_ack_releases_the_slot(client, fresh_db, monkeypatch):
    secret = seed(fresh_db)
    _configure(monkeypatch, max_connected_timers=1)
    original = ws.WsSession.send

    async def failing_send(self, frame):
        if frame.get("type") == "hello_ack":
            raise RuntimeError("peer went away")
        return await original(self, frame)

    monkeypatch.setattr(ws.WsSession, "send", failing_send)
    for _ in range(3):  # would be refused from the second attempt if leaked
        with client.websocket_connect("/api/v1/timer", subprotocols=["ost-relay.v1"]) as socket:
            nonce = b64decode(socket.receive_json()["server_nonce"], 16)
            socket.send_json(hello(secret, nonce))
            time.sleep(0.05)
        assert ws.registry.get("timer_a") is None
    monkeypatch.setattr(ws.WsSession, "send", original)
    with client.websocket_connect("/api/v1/timer", subprotocols=["ost-relay.v1"]) as socket:
        nonce = b64decode(socket.receive_json()["server_nonce"], 16)
        socket.send_json(hello(secret, nonce))
        assert socket.receive_json()["type"] == "hello_ack"


# 2. Deadlines ------------------------------------------------------------------

class _BlockedSocket:
    async def send_json(self, _frame):
        await asyncio.sleep(3600)

    async def close(self, code=1000, reason=""):
        return None


def test_command_deadline_includes_a_blocked_send(fresh_db, monkeypatch):
    from fastapi import HTTPException
    _configure(monkeypatch, command_await_s=0.3, ws_send_timeout_s=30.0)
    session = ws.WsSession(timer_id="timer_a", ws=_BlockedSocket(), session_id="s", boot_id="aabbccdd",
                           last_activity_wall=time.time(), last_activity_mono=time.monotonic())
    session.last_status = {"boot_id": "aabbccdd", "uptime_ms": 10_000}
    session.last_status_mono = time.monotonic()

    async def run():
        started = time.monotonic()
        with pytest.raises(HTTPException) as exc:
            await ws.send_opaque(session, {'type':'e2e','fp_hash':b64encode(bytes(16)), 'client_id':b64encode(bytes(16)), 'server_id':b64encode(bytes(16)), 'seq':1,'request_id':'op-1','ciphertext':b64encode(bytes(16))})
        return exc.value.detail, time.monotonic() - started

    detail, elapsed = asyncio.run(run())
    assert detail == "timer_offline_or_slow" and elapsed < 1.0


def test_socket_write_is_bounded(fresh_db, monkeypatch):
    _configure(monkeypatch, ws_send_timeout_s=0.2)
    session = ws.WsSession(timer_id="timer_a", ws=_BlockedSocket(), session_id="s", boot_id="b",
                           last_activity_wall=time.time(), last_activity_mono=time.monotonic())

    async def run():
        with pytest.raises(asyncio.TimeoutError):
            await session.send({"type": "ping"})

    asyncio.run(run())


def test_slow_request_body_times_out(fresh_settings, monkeypatch):
    from ostrelay.limits import RequestLimits
    _configure(monkeypatch, http_body_timeout_s=0.2)
    sent = []

    async def app(scope, receive, send):  # never reached
        raise AssertionError("app must not run")

    async def receive():
        await asyncio.sleep(3600)

    async def send(message):
        sent.append(message)

    scope = {"type": "http", "method": "POST", "path": "/api/v1/session", "headers": []}
    asyncio.run(RequestLimits(app)(scope, receive, send))
    assert sent[0]["status"] == 408


# 3. SSE admission is reserved atomically ----------------------------------------

def test_sse_capacity_counts_admitted_responses(fresh_db, monkeypatch):
    from fastapi import HTTPException
    from ostrelay import watch
    _configure(monkeypatch, max_subscribers_per_timer=2)
    live = ws.WsSession(timer_id="timer_a", ws=_BlockedSocket(), session_id="s", boot_id="b",
                        last_activity_wall=time.time(), last_activity_mono=time.monotonic())
    monkeypatch.setattr(ws.registry, "_sessions", {"timer_a": live})
    row = {"timer_id": "timer_a", "session_id": "x"}

    async def run():
        # Four concurrent admissions before any stream starts: only two pass.
        admitted, refused = [], 0
        for _ in range(4):
            try:
                admitted.append(await watch.watch(request=None, session_row=row))
            except HTTPException as exc:
                assert exc.status_code == 429
                refused += 1
        assert len(admitted) == 2 and refused == 2 and len(live.subscribers) == 2

        async def receive():
            return {"type": "http.disconnect"}

        async def send(_message):
            return None

        for response in admitted:  # ending a response always frees its slot
            await response({"type": "http", "method": "GET", "path": "/api/v1/watch", "headers": []},
                           receive, send)
        assert len(live.subscribers) == 0

    asyncio.run(run())


# 4. Owner revocation cannot race an enrollment ------------------------------------

def test_owner_check_runs_inside_the_enrollment_transaction(client, monkeypatch):
    from ostrelay import owners
    code = owners.create_owner("alex")
    seen = []
    original = owners.owner_for_code

    def spy(value, conn=None):
        seen.append(conn is not None and db_mod._lock._is_owned())  # held by the open transaction
        return original(value, conn)

    monkeypatch.setattr(owners, "owner_for_code", spy)
    assert client.post("/api/v1/enroll", json={"enrollment_code": code, **ENROLL}).status_code == 200
    assert seen == [True]
    owners.revoke_owner("alex")
    assert client.post("/api/v1/enroll", json={"enrollment_code": code, **ENROLL}).status_code == 403


# 5. Open enrollment capacity is reclaimed, connected timers never are -------------

def test_unused_open_enrollments_are_reclaimed(client, fresh_db, monkeypatch):
    from ostrelay.owners import OPEN_ENROLLMENT_CODE
    _configure(monkeypatch, enrollment="open", max_enrolled_timers=3, rate_open_enrollments_per_ip_1h=10)
    ids = [client.post("/api/v1/enroll", json={"enrollment_code": OPEN_ENROLLMENT_CODE, **ENROLL}).json()["timer_id"]
           for _ in range(3)]
    assert client.post("/api/v1/enroll", json={"enrollment_code": OPEN_ENROLLMENT_CODE, **ENROLL}).json()["detail"] == "relay_full"
    conn = fresh_db.connect()
    old = db_mod.now_unix() - 2 * 86400
    conn.execute("UPDATE timers SET created_at=?", (old,))
    conn.execute("UPDATE timers SET last_seen_at=? WHERE timer_id=?", (old, ids[0]))  # connected once, long ago
    fresh_db.cleanup()
    left = [r["timer_id"] for r in conn.execute("SELECT timer_id FROM timers").fetchall()]
    assert left == [ids[0]]
    assert client.post("/api/v1/enroll", json={"enrollment_code": OPEN_ENROLLMENT_CODE, **ENROLL}).status_code == 200


def test_owner_code_timers_are_not_reclaimed(client, fresh_db):
    from ostrelay import owners
    code = owners.create_owner("alex")
    assert client.post("/api/v1/enroll", json={"enrollment_code": code, **ENROLL}).status_code == 200
    fresh_db.connect().execute("UPDATE timers SET created_at=0")
    fresh_db.cleanup()
    assert fresh_db.connect().execute("SELECT count(*) AS n FROM timers").fetchone()["n"] == 1


# 6. Malformed owner codes are refused, never a server error -----------------------

@pytest.mark.parametrize("code", ["o1.AAAAAAAA." + "é" * 43, "o1.AAAAAAAA.short", "o1." + "A" * 60,
                                  "o1.AAAA.AAAA.AAAA", "o1.AAAAAAAA." + "A" * 42 + "=", "o1.\u0000" * 4])
def test_malformed_owner_codes_are_refused(client, code):
    response = client.post("/api/v1/enroll", json={"enrollment_code": code, **ENROLL})
    assert response.status_code in (403, 422)


def test_enrollment_attempts_are_budgeted_per_ip(client, monkeypatch):
    _configure(monkeypatch, rate_enrollments_per_ip_1h=3)
    codes = ["o1.AAAAAAAA." + secrets.token_urlsafe(32)[:43] for _ in range(4)]
    statuses = [client.post("/api/v1/enroll", json={"enrollment_code": c, **ENROLL}).status_code for c in codes]
    assert statuses == [403, 403, 403, 429]
