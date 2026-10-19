"""Regressions for pending-result correlation and connection liveness.

Both fixes live in ostrelay.ws. These tests exercise the module
functions directly rather than spinning up a full WSS because the
starlette TestClient's WSS surface is more trouble than it's worth
for these narrow guarantees.
"""

from __future__ import annotations

import asyncio
import time
import json
import pytest
from ostrelay.wire import b64encode


def envelope(body):
    return {"type": "result", "upstream_seq": 1, "body_json": json.dumps(body), "sig": b64encode(bytes(64))}


# ---------------------------------------------------------------------------
# R4 — pending map keyed by (op_id, downstream_seq)
# ---------------------------------------------------------------------------

async def test_same_request_id_across_clients_does_not_collide(fresh_registry):
    from ostrelay import ws
    session=ws.WsSession(timer_id='t',ws=None,session_id='s')
    a,b=asyncio.get_running_loop().create_future(),asyncio.get_running_loop().create_future()
    cid_a,cid_b=b64encode(bytes(16)),b64encode(bytes([1])*16)
    session.opaque_pending[(cid_a,'request')]=a
    session.opaque_pending[(cid_b,'request')]=b
    frame=dict(type='e2e',upstream_seq=1,fp_hash=b64encode(bytes(16)),client_id=cid_a,
        server_id=b64encode(bytes(16)),seq=1,request_id='request',ciphertext=b64encode(bytes(16)))
    ws._handle_opaque(session,frame)
    assert await a is frame and not b.done()
    ws._handle_opaque(session,{**frame,'client_id':cid_b})
    assert b.done()


async def test_result_without_request_id_cannot_resolve(fresh_registry):
    from ostrelay import ws
    session=ws.WsSession(timer_id='t',ws=None,session_id='s')
    frame=dict(type='e2e',upstream_seq=1,fp_hash=b64encode(bytes(16)),client_id=b64encode(bytes(16)),
        server_id=b64encode(bytes(16)),seq=1,ciphertext=b64encode(bytes(16)))
    with pytest.raises(ValueError):ws._handle_opaque(session,frame)


# ---------------------------------------------------------------------------
# R8 — heartbeat enforces ws_ping_timeout_s
# ---------------------------------------------------------------------------

async def test_heartbeat_closes_on_liveness_timeout(fresh_registry, monkeypatch):
    from ostrelay import ws as ws_mod
    from ostrelay import config as config_mod

    # Tight cadence so the test doesn't wait 90s.
    settings = config_mod.settings()
    monkeypatch.setattr(settings, "ws_ping_interval_s", 0.02)
    monkeypatch.setattr(settings, "ws_ping_timeout_s", 0.05)

    closed_with: dict[str, object] = {}

    class _FakeWs:
        async def send_json(self, obj: dict) -> None:
            # Simulate ping delivery without a real socket.
            return None

        async def close(self, code: int = 1000, reason: str = "") -> None:
            closed_with["code"] = code
            closed_with["reason"] = reason

    session = ws_mod.WsSession(
        timer_id="t", ws=_FakeWs(), session_id="s",
        last_activity_wall=time.time() - 10.0,   # far in the past
    )
    # Heartbeat should notice on its first tick and close with 4004.
    await asyncio.wait_for(ws_mod._heartbeat(session), timeout=0.5)
    assert closed_with.get("code") == 4004
    assert closed_with.get("reason") == "ping_timeout"


async def test_heartbeat_stays_open_when_active(fresh_registry, monkeypatch):
    from ostrelay import ws as ws_mod
    from ostrelay import config as config_mod

    settings = config_mod.settings()
    monkeypatch.setattr(settings, "ws_ping_interval_s", 0.02)
    monkeypatch.setattr(settings, "ws_ping_timeout_s", 0.5)

    pings_sent = 0

    class _FakeWs:
        async def send_json(self, obj: dict) -> None:
            nonlocal pings_sent
            pings_sent += 1

        async def close(self, code: int = 1000, reason: str = "") -> None:
            raise AssertionError("heartbeat should not close a live session")

    session = ws_mod.WsSession(
        timer_id="t", ws=_FakeWs(), session_id="s",
        last_activity_wall=time.time(),
    )
    task = asyncio.create_task(ws_mod._heartbeat(session))
    await asyncio.sleep(0.1)
    task.cancel()
    try:
        await task
    except asyncio.CancelledError:
        pass
    assert pings_sent >= 2
