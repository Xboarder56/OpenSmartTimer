"""The mock uses a real identity-signed handshake and preserves retry semantics."""
import hashlib
import importlib.util
import json
from pathlib import Path
import secrets
import sys

import pytest

spec = importlib.util.spec_from_file_location("relay_test_mock", Path(__file__).parents[1] / "mock-timer/mock.py")
mock = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = mock
spec.loader.exec_module(mock)


def timer():
    return mock.MockTimerSession(mock.MockCredentials("timer_test_a", bytes(32),
        "ws://127.0.0.1:8080/api/v1/timer", "", secrets.token_bytes(32)),partner_origin='https://partner.test')


def test_response_shape_and_exact_token_digest():
    session = timer()
    frame = session.response_frame("status", {"boot_id": session.timer.boot_id, "label": 'A "quoted" timer'})
    assert set(frame) == {"type", "upstream_seq", "body_json"}
    assert json.loads(frame["body_json"])["label"] == 'A "quoted" timer'
    wire, record = session.keys.mint(["view", "adjust"], 60000, session.timer.boot_id,
                                    session.uptime_ms(), session.cred.timer_id, "test")
    assert record.token_digest == mock.b64url(hashlib.sha256(wire.encode()).digest())
    assert len(mock.unb64url(json.loads(mock.unb64url(wire.split('.')[2]))["vk"])) == 33
    tampered = wire.rsplit('.', 1)[0] + '.' + mock.b64url(bytes(16))
    assert session.keys.verify(tampered, session.cred.timer_id, session.timer.boot_id, 0, "view")[0] == "unauthorized"


async def test_lost_send_retains_outcome_before_same_id_retry():
    session = timer()
    class Socket:
        failed = True
        frames = []
        async def send(self, raw):
            if self.failed:
                raise RuntimeError("lost connection")
            self.frames.append(json.loads(raw))
    sock = Socket(); session.ws = sock
    session.timer.start(60000, session.uptime_ms())
    wire, record = session.keys.mint(["view", "adjust"], 60000, session.timer.boot_id,
                               session.uptime_ms(), session.cred.timer_id, "test")
    from secure import BrowserOpen,timer_accept
    opened=BrowserOpen(wire)
    accepted,channel=timer_accept(opened.frame,session.cred.timer_id,session.timer.boot_id,record.fp,lambda message: mock.p256_sign(session.signer,message))
    browser=opened.accept(accepted)
    session.active_channel={'channel':channel}
    command = {"type": "command", "op": "adjust", "op_id": "one-adjust", "downstream_seq": 1,
               "cap_token": wire, "params": {"delta_ms": 1000}, "if_boot_id": session.timer.boot_id,
               "if_session_id": session.timer.session_id, "expiry_uptime_ms": session.uptime_ms() + 5000}
    with pytest.raises(RuntimeError, match="lost connection"):
        await session.handle_command(command)
    revision = session.timer.state_revision
    assert len(session.results) == 1
    sock.failed = False; command["downstream_seq"] = 2
    await session.handle_command(command)
    assert session.timer.state_revision == revision
    result = json.loads(browser.decrypt(sock.frames[-1])["body_json"])
    assert result["mutation"]["duplicate"] is True
    assert result["in_reply_to_downstream_seq"] == 2


def test_mock_release_obeys_controls_lock():
    session = timer()
    session.timer.start(10000, 0)
    session.timer.lock_controls()
    assert session.timer.release() == ("controls_locked", False)
    assert session.timer.state == "running"
