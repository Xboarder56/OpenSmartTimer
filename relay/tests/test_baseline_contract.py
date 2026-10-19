"""Boundary failures found while exercising the real development stack."""
import asyncio
import json
import time

import pytest
from pydantic import ValidationError

from ostrelay.config import Settings
from ostrelay.models import CommandRequest
from ostrelay.wire import b64encode


def config(**kwargs):
    values = dict(admin_token="a" * 64, cookie_secret="01" * 32,
                  public_url="https://relay.example", spki_pin=b64encode(bytes(32)),
                  dev_mode=False, _env_file=None)
    values.update(kwargs)
    return Settings(**values)


def test_deployment_requires_actual_pin_shape_and_https():
    assert config().public_url == "https://relay.example"
    # No pin: the relay follows the certificate served at its public URL.
    assert config(spki_pin="").spki_pin == ""
    assert config(spki_pin=b64encode(bytes(32)) + "," + b64encode(bytes(range(32))))
    assert config(tls_probe_address="proxy:443").tls_probe_address == "proxy:443"
    for values in ({"spki_pin": "not-a-pin"}, {"spki_pin": b64encode(bytes(32)) + ",short"},
                   {"tls_probe_address": "no-port"},
                   {"public_url": "http://relay.example"},
                   {"public_url": "https://relay.example/path"},
                   {"public_url": "https://user:password@relay.example"},
                   {"public_url": "https://relay.example/#token"},
                   {"cookie_secret": "not-a-32-byte-secret"}):
        with pytest.raises(ValidationError):
            config(**values)
    assert config(public_url="https://relay.example/").public_url == "https://relay.example"


def test_development_exception_requires_explicit_loopback():
    assert config(public_url="http://localhost:8080", spki_pin="", dev_mode=True)
    with pytest.raises(ValidationError):
        config(public_url="http://public.example", spki_pin="", dev_mode=True)


@pytest.mark.parametrize('seq',[True,0,-1,1.5,2**53])
def test_opaque_model_rejects_invalid_sequence(seq):
    with pytest.raises(ValidationError):
        CommandRequest(type='e2e',fp_hash=b64encode(bytes(16)),client_id=b64encode(bytes(16)),server_id=b64encode(bytes(16)),seq=seq,request_id='a',ciphertext=b64encode(bytes(16)))


def test_expired_grants_free_capacity_without_reviving_revoke(fresh_db, fresh_settings, monkeypatch):
    from ostrelay import ws
    from ostrelay.config import settings
    fresh_db.connect().execute("INSERT INTO timers(timer_id,enrollment_secret,hardware_model,chip_family,build_id,spki_sha256_pin,created_at) VALUES(?,?,?,?,?,?,?)",
                             ("t", bytes(32), "test", "test", "a" * 24, "", 0))
    monkeypatch.setattr(settings(), "max_tokens_per_timer", 2)
    session = ws.WsSession(timer_id="t", ws=None, session_id="s", boot_id="aabbccdd", hello_uptime_ms=100)
    def mint(i, expiry):
        return {"type": "token_mint", "upstream_seq": i, "fp_hash": b64encode(bytes([i]) * 16),
                "token_digest": b64encode(bytes([i]) * 32), "scopes": ["view"], "boot_id": "aabbccdd",
                "exp_up_ms": expiry, "label": "test"}
    old = mint(1, 200)
    revoked = mint(2, 10000)
    ws._persist_token_mint(session, old)
    ws._persist_token_mint(session, revoked)
    ws._persist_token_revoke(session, {"fp_hash": revoked["fp_hash"]})
    session.hello_uptime_ms = 300
    ws._persist_token_mint(session, mint(3, 5000))  # Reclaims the expired row.
    ws._persist_token_mint(session, old)  # Expired replay ignored, not reinserted.
    ws._persist_token_mint(session, revoked)  # Still revoked after reannouncement.
    rows = fresh_db.connect().execute("SELECT * FROM tokens").fetchall()
    assert len(rows) == 2
    assert next(row for row in rows if row["fp_hash"] == revoked["fp_hash"])["revoked"] == 1
