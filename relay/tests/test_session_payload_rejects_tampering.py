"""Regression: session exchange must match the timer-registered token.

A session mint once that accepted a token whose
submitted payload had a different boot_id and exp_up_ms than the
values the timer registered via token_mint. This suite locks that
door: submitted payload fields MUST match the authoritative stored
values on the tokens row.
"""

from __future__ import annotations

import base64
import hashlib
import json
import time

import pytest


def _seed_timer_and_token(
    fresh_db, ws_last_status: dict[str, object], *,
    boot_id: str = "aabbccdd",
    exp_up_ms: int = 3_600_000,
    scopes: tuple[str, ...] = ("view", "pause_resume"),
) -> tuple[str, str]:
    """Insert a timer + token pair with matching authoritative values.
    Returns (timer_id, fp_hash).
    """
    import secrets
    fp = secrets.token_bytes(16)
    fp_hash = (
        base64.urlsafe_b64encode(hashlib.sha256(fp).digest()[:16])
        .rstrip(b"=").decode("ascii")
    )
    with fresh_db.transaction() as tx:
        tx.execute(
            "INSERT INTO timers "
            "(timer_id, enrollment_secret, hardware_model, chip_family, "
            "build_id, spki_sha256_pin, created_at) "
            "VALUES (?, ?, ?, ?, ?, ?, ?)",
            ("timer_r2", b"\x00" * 32, "MagBound Smart Timer V2",
             "ESP32-C3", "aabbccddeeff00112233aabb", "pin", fresh_db.now_unix()),
        )
        tx.execute(
            "INSERT INTO tokens "
            "(fp_hash, timer_id, scope_json, boot_id, exp_up_ms, label, "
            "minted_at, revoked) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, 0)",
            (fp_hash, "timer_r2", json.dumps(list(scopes)), boot_id,
             exp_up_ms, "test", fresh_db.now_unix()),
        )

    # Attach a fake WSS session so /api/v1/session gets past the
    # timer-not-connected gate. Fill last_status with a live boot_id.
    from ostrelay import ws as ws_mod
    session = ws_mod.WsSession(
        timer_id="timer_r2", ws=None, session_id="dummy",
    )
    session.last_status = {"boot_id": boot_id, "uptime_ms": 0, **ws_last_status}
    session.last_status_wall = time.time()
    ws_mod.registry._sessions["timer_r2"] = session   # noqa: SLF001

    return "timer_r2", (fp_hash, fp)


def _make_wire_token(
    fp: bytes, *, boot_id: str, exp_up_ms: int,
    scopes: list[str], timer_id: str = "timer_r2",
) -> str:
    payload = {
        "aud": "ost-relay/1",
        "tid": timer_id,
        "fp": base64.urlsafe_b64encode(fp).rstrip(b"=").decode(),
        "sc": scopes,
        "boot": boot_id,
        "exp_up_ms": exp_up_ms,
        "mint_up_ms": 0,
        "vk": base64.urlsafe_b64encode(bytes.fromhex("036b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296")).rstrip(b"=").decode(),  # P-256 generator
    }
    body = base64.urlsafe_b64encode(
        json.dumps(payload, separators=(",", ":")).encode()
    ).rstrip(b"=").decode()
    tag = base64.urlsafe_b64encode(b"\x00" * 16).rstrip(b"=").decode()
    wire = f"ost-cap.v1.{body}.{tag}"
    # Register the full wire digest as the authenticated timer now must.
    from ostrelay import db
    from ostrelay.session import token_digest, fingerprint_hash
    db.connect().execute("UPDATE tokens SET token_digest=? WHERE fp_hash=?",
                         (token_digest(wire), fingerprint_hash(payload["fp"])))
    return wire


def test_plaintext_capability_rejected_even_when_registered(client, fresh_db, fresh_registry):
    _, (fp_hash, fp) = _seed_timer_and_token(fresh_db, {})
    wire = _make_wire_token(
        fp, boot_id="aabbccdd", exp_up_ms=3_600_000,
        scopes=["view", "pause_resume"],
    )
    r = client.post("/api/v1/session", json={"token": wire})
    assert r.status_code == 422


def test_tampered_boot_rejected(client, fresh_db, fresh_registry):
    _, (fp_hash, fp) = _seed_timer_and_token(fresh_db, {})
    wire = _make_wire_token(
        fp, boot_id="00000000",              # LIE
        exp_up_ms=3_600_000, scopes=["view", "pause_resume"],
    )
    r = client.post("/api/v1/session", json={"token": wire})
    assert r.status_code == 422


def test_tampered_exp_rejected(client, fresh_db, fresh_registry):
    _, (fp_hash, fp) = _seed_timer_and_token(fresh_db, {})
    wire = _make_wire_token(
        fp, boot_id="aabbccdd",
        exp_up_ms=999_999_999,               # LIE — extended
        scopes=["view", "pause_resume"],
    )
    r = client.post("/api/v1/session", json={"token": wire})
    assert r.status_code == 422


def test_tampered_scopes_rejected(client, fresh_db, fresh_registry):
    _, (fp_hash, fp) = _seed_timer_and_token(fresh_db, {})
    wire = _make_wire_token(
        fp, boot_id="aabbccdd", exp_up_ms=3_600_000,
        scopes=["view", "pause_resume", "release"],   # LIE — escalated
    )
    r = client.post("/api/v1/session", json={"token": wire})
    assert r.status_code == 422


def test_expired_by_uptime_rejected(client, fresh_db, fresh_registry):
    # Store token with 1 ms TTL; WSS session's uptime is 10 s → expired.
    _, (fp_hash, fp) = _seed_timer_and_token(
        fresh_db, {"uptime_ms": 10_000}, exp_up_ms=1,
    )
    wire = _make_wire_token(
        fp, boot_id="aabbccdd", exp_up_ms=1,
        scopes=["view", "pause_resume"],
    )
    r = client.post("/api/v1/session", json={"token": wire})
    assert r.status_code == 422


@pytest.mark.parametrize("vk_hex", [
    "00" * 33,                                   # not a SEC1 point
    "02" + "ff" * 32,                            # x beyond the field / off curve
    "04" + "6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c2",  # wrong form/length
    "6b17d1f2e12c4247f8bce6e563a440f277037d812deb33a0f4a13945d898c296",     # 32-byte Ed25519-style key
])
def test_verify_key_must_be_compressed_p256(vk_hex):
    from fastapi import HTTPException
    from ostrelay.session import parse_wire_token
    payload = {"aud": "ost-relay/1", "tid": "timer_r2", "fp": base64.urlsafe_b64encode(bytes(16)).rstrip(b"=").decode(),
               "sc": ["view"], "boot": "aabbccdd", "exp_up_ms": 60_000, "mint_up_ms": 0,
               "vk": base64.urlsafe_b64encode(bytes.fromhex(vk_hex)).rstrip(b"=").decode()}
    body = base64.urlsafe_b64encode(json.dumps(payload, separators=(",", ":")).encode()).rstrip(b"=").decode()
    tag = base64.urlsafe_b64encode(bytes(16)).rstrip(b"=").decode()
    with pytest.raises(HTTPException):
        parse_wire_token(f"ost-cap.v1.{body}.{tag}")
