"""Challenge-response authentication, including actual WebSocket replay rejection."""
import asyncio
import secrets
import pytest
from ostrelay import ws
from ostrelay.wire import b64encode, b64decode


def seed(db, timer_id="timer_a"):
    secret = secrets.token_bytes(32)
    db.connect().execute("INSERT INTO timers (timer_id,enrollment_secret,hardware_model,chip_family,build_id,spki_sha256_pin,created_at) VALUES (?,?,?,?,?,?,?)",
                         (timer_id, secret, "V2", "C3", "aa" * 12, "pin", db.now_unix()))
    return secret


def hello(secret, nonce, **changes):
    frame = {"type": "hello", "protocol": "ost-relay/1", "timer_id": "timer_a", "boot_id": "aabbccdd",
             "uptime_ms": 123, "resume_from_upstream_seq": 0, "resume_from_downstream_seq": 0}
    frame["hello_tag"] = b64encode(ws._hello_tag(secret, "timer_a", "aabbccdd", 123, 0, 0, nonce, bytes(32)))
    return frame | changes


def test_hello_nonce_binding(fresh_db):
    secret = seed(fresh_db)
    nonce = secrets.token_bytes(16)
    frame = hello(secret, nonce)
    assert asyncio.run(ws._verify_hello(frame, nonce))["timer_id"] == "timer_a"
    assert asyncio.run(ws._verify_hello(frame, secrets.token_bytes(16))) is None
    for change in ({"hello_tag": b64encode(bytes(16))}, {"timer_id": "missing"},
                   {"uptime_ms": -1}, {"uptime_ms": True}, {"uptime_ms": 1 << 64},
                   {"boot_id": []}, {"hello_tag": "!"}, {"unknown": 1}):
        assert asyncio.run(ws._verify_hello(frame | change, nonce)) is None


def test_challenge_fresh_replay_rejected(client, fresh_db):
    from starlette.websockets import WebSocketDisconnect
    secret = seed(fresh_db)
    with client.websocket_connect("/api/v1/timer", subprotocols=["ost-relay.v1"]) as socket:
        first = socket.receive_json()
        nonce = b64decode(first["server_nonce"], 16)
        frame = hello(secret, nonce)
        socket.send_json(frame)
        ack = socket.receive_json()
        assert ack["type"] == "hello_ack"
        # Only a relay holding the enrollment secret can acknowledge this hello.
        assert b64decode(ack["ack_tag"], 16) == ws._ack_tag(secret, nonce, b64decode(frame["hello_tag"], 16))
    with client.websocket_connect("/api/v1/timer", subprotocols=["ost-relay.v1"]) as socket:
        assert socket.receive_json()["server_nonce"] != first["server_nonce"]
        socket.send_json(frame)
        with pytest.raises(WebSocketDisconnect) as exc:
            socket.receive_json()
        assert exc.value.code == 4003


def test_bad_first_frame_closed_cleanly(client):
    from starlette.websockets import WebSocketDisconnect
    with client.websocket_connect("/api/v1/timer", subprotocols=["ost-relay.v1"]) as socket:
        socket.receive_json()
        socket.send_text('{"type":"hello","type":"hello"}')
        with pytest.raises(WebSocketDisconnect) as exc:
            socket.receive_json()
        assert exc.value.code == 4000


def test_hello_is_bound_to_the_tls_key_the_relay_serves(fresh_db, monkeypatch):
    import hashlib
    from ostrelay import config, tlsid
    secret = seed(fresh_db)
    nonce = secrets.token_bytes(16)
    ours, other = hashlib.sha256(b"relay key").digest(), hashlib.sha256(b"interposed key").digest()
    monkeypatch.setattr(config.settings(), "spki_pin", tlsid.encode(ours))

    def bound_to(key):
        frame = hello(secret, nonce)
        frame["hello_tag"] = b64encode(ws._hello_tag(secret, "timer_a", "aabbccdd", 123, 0, 0, nonce, key))
        return frame

    accepted = asyncio.run(ws._verify_hello(bound_to(ours), nonce))
    assert accepted["timer_id"] == "timer_a"
    assert b64decode(accepted["ack_tag"], 16) == ws._ack_tag(
        secret, nonce, b64decode(bound_to(ours)["hello_tag"], 16))
    # A hello relayed through another TLS endpoint names that endpoint's key.
    assert asyncio.run(ws._verify_hello(bound_to(other), nonce)) is None
    # The no-TLS development form is refused once the relay has a key.
    assert asyncio.run(ws._verify_hello(bound_to(bytes(32)), nonce)) is None
    # Any of several configured keys is accepted (for example RSA and ECDSA certificates).
    monkeypatch.setattr(config.settings(), "spki_pin", tlsid.encode(other) + "," + tlsid.encode(ours))
    assert asyncio.run(ws._verify_hello(bound_to(ours), nonce))["timer_id"] == "timer_a"
    assert tlsid.current_pin() == tlsid.encode(other)


def test_relay_follows_the_certificate_it_serves(fresh_settings, monkeypatch):
    import hashlib
    from ostrelay import config, tlsid
    cfg = config.settings()
    monkeypatch.setattr(cfg, "public_url", "https://relay.example.com")
    monkeypatch.setattr(cfg, "dev_mode", False)
    tlsid.reset_for_tests()
    served = {"key": hashlib.sha256(b"first certificate").digest()}
    monkeypatch.setattr(tlsid, "probe", lambda: served["key"])
    try:
        # Unknown until read: no hello can be accepted and no setup code carries a pin.
        assert tlsid.follows_certificate() and tlsid.pins() == [] and tlsid.current_pin() == ""
        assert asyncio.run(tlsid.refresh(force=True)) and tlsid.pins() == [served["key"]]
        first = served["key"]
        served["key"] = hashlib.sha256(b"renewed certificate").digest()
        # A hello bound to an unknown key asks for a refresh; those are rate-limited.
        assert asyncio.run(tlsid.refresh()) and tlsid.pins() == [first]
        assert asyncio.run(tlsid.refresh(force=True)) and tlsid.pins() == [served["key"]]
        assert tlsid.current_pin() == tlsid.encode(served["key"])

        def unreachable():
            raise OSError("proxy is down")
        monkeypatch.setattr(tlsid, "probe", unreachable)
        # An outage keeps the last key that was read.
        assert asyncio.run(tlsid.refresh(force=True)) and tlsid.pins() == [served["key"]]
    finally:
        tlsid.reset_for_tests()


def test_plain_development_relay_uses_the_no_tls_binding(fresh_settings):
    from ostrelay import tlsid
    tlsid.reset_for_tests()
    assert not tlsid.follows_certificate()
    assert tlsid.pins() == [tlsid.NO_TLS] and tlsid.current_pin() == ""
