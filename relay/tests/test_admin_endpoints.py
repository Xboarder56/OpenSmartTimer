"""Admin endpoint contract tests.

Focus: auth gate, enrollment-code single-use rule, timer listing +
delete, and token revocation cascading to sessions.
"""

from __future__ import annotations

import time

import pytest


# ---------------------------------------------------------------------------
# Auth
# ---------------------------------------------------------------------------

def test_admin_requires_bearer(client):
    resp = client.post("/admin/enroll-code")
    assert resp.status_code == 401
    assert resp.json()["detail"] == "admin_token_required"


def test_admin_rejects_wrong_bearer(client):
    resp = client.post("/admin/enroll-code",
                       headers={"Authorization": "Bearer wrong"})
    assert resp.status_code == 401


# ---------------------------------------------------------------------------
# Enrollment codes
# ---------------------------------------------------------------------------

def test_mint_code_returns_expiry(client, admin_headers):
    resp = client.post("/admin/enroll-code", headers=admin_headers)
    assert resp.status_code == 200
    body = resp.json()
    assert len(body["enrollment_code"]) >= 24
    assert body["expires_at_unix"] > int(time.time())
    assert body["expires_at_unix"] < int(time.time()) + 16 * 60
    assert body["setup_code"] == ""  # loopback dev mode has no SPKI pin


def test_setup_code_bundles_origin_code_and_pin(monkeypatch):
    import base64, json
    from types import SimpleNamespace
    from ostrelay import admin
    monkeypatch.setattr(admin, "settings", lambda: SimpleNamespace(
        public_url="https://api.example.com", partner_origin="https://relay.example.com"))
    # The pin is whichever TLS key the relay currently serves.
    monkeypatch.setattr(admin.tlsid, "current_pin", lambda: "A" * 43)
    setup = admin.setup_code_for("CODE123")
    assert setup.startswith("ost-relay-setup:")
    encoded = setup.split(":", 1)[1]
    decoded = json.loads(base64.urlsafe_b64decode(encoded + "=" * (-len(encoded) % 4)))
    assert decoded == {"url": "https://api.example.com", "code": "CODE123", "pin": "A" * 43,
                       "partner_origin": "https://relay.example.com"}
    # Until that key is known, no setup code can be built.
    monkeypatch.setattr(admin.tlsid, "current_pin", lambda: "")
    assert admin.setup_code_for("CODE123") == ""


def test_mint_code_is_persisted(client, admin_headers, fresh_db):
    resp = client.post("/admin/enroll-code", headers=admin_headers)
    code = resp.json()["enrollment_code"]
    row = fresh_db.connect().execute(
        "SELECT code, consumed_at FROM enrollment_codes WHERE code = ?",
        (code,),
    ).fetchone()
    assert row is not None
    assert row["consumed_at"] is None


def test_enroll_consumes_code(client, admin_headers):
    resp = client.post("/admin/enroll-code", headers=admin_headers)
    code = resp.json()["enrollment_code"]

    r = client.post("/api/v1/enroll", json={
        "enrollment_code": code,
        "hardware_model": "MagBound Smart Timer V2",
        "chip_family": "ESP32-C3",
        "build_id": "aabbccddeeff00112233aabb",
    })
    assert r.status_code == 200
    body = r.json()
    assert body["timer_id"]
    assert body["enrollment_secret"]

    # Second attempt with the same code MUST fail 410 code_consumed.
    r2 = client.post("/api/v1/enroll", json={
        "enrollment_code": code,
        "hardware_model": "MagBound Smart Timer V2",
        "chip_family": "ESP32-C3",
        "build_id": "aabbccddeeff00112233aabb",
    })
    assert r2.status_code == 410


@pytest.mark.parametrize("kind", ["one-time", "owner", "open"])
def test_enrollment_waits_for_tls_identity_without_consuming_code_or_capacity(
        client, admin_headers, fresh_db, monkeypatch, kind):
    from ostrelay import config, enroll, owners, tlsid

    monkeypatch.setattr(config._settings, "enrollment", "owner_codes,open")
    if kind == "one-time":
        code = client.post("/admin/enroll-code", headers=admin_headers).json()["enrollment_code"]
    elif kind == "owner":
        code = owners.create_owner("example-owner")
    else:
        code = owners.OPEN_ENROLLMENT_CODE
    monkeypatch.setattr(config._settings, "public_url", "https://relay.example.com")
    monkeypatch.setattr(config._settings, "dev_mode", False)
    monkeypatch.setattr(tlsid, "current_pin", lambda: "")
    body = {"enrollment_code": code, "hardware_model": "MagBound Smart Timer V2",
            "chip_family": "ESP32-C3", "build_id": "aabbccddeeff00112233aabb"}

    response = client.post("/api/v1/enroll", json=body)
    assert response.status_code == 503
    assert response.json()["detail"] == "tls_identity_unavailable"
    assert response.headers["retry-after"] == "15"
    assert fresh_db.connect().execute("SELECT count(*) FROM timers").fetchone()[0] == 0
    assert not enroll._enroll_buckets and not enroll._open_buckets
    if kind == "one-time":
        assert fresh_db.connect().execute(
            "SELECT consumed_at FROM enrollment_codes WHERE code=?", (code,)).fetchone()[0] is None

    # The same code remains usable once certificate discovery completes.
    monkeypatch.setattr(tlsid, "current_pin", lambda: "A" * 43)
    response = client.post("/api/v1/enroll", json=body)
    assert response.status_code == 200
    assert response.json()["spki_sha256_pin"] == "A" * 43
    assert fresh_db.connect().execute("SELECT count(*) FROM timers").fetchone()[0] == 1


def test_enrollment_uses_one_tls_identity_for_saved_and_returned_credentials(
        client, admin_headers, fresh_db, monkeypatch):
    from ostrelay import tlsid

    code = client.post("/admin/enroll-code", headers=admin_headers).json()["enrollment_code"]
    pins = iter(["A" * 43, "B" * 42 + "A"])
    monkeypatch.setattr(tlsid, "current_pin", lambda: next(pins))
    response = client.post("/api/v1/enroll", json={
        "enrollment_code": code, "hardware_model": "MagBound Smart Timer V2",
        "chip_family": "ESP32-C3", "build_id": "aabbccddeeff00112233aabb"})
    assert response.status_code == 200
    saved = fresh_db.connect().execute("SELECT spki_sha256_pin FROM timers").fetchone()[0]
    assert saved == response.json()["spki_sha256_pin"] == "A" * 43


# ---------------------------------------------------------------------------
# Timer inventory
# ---------------------------------------------------------------------------

def test_list_timers_empty(client, admin_headers):
    r = client.get("/admin/timers", headers=admin_headers)
    assert r.status_code == 200
    assert r.json() == []


def test_list_timers_after_enroll(client, admin_headers):
    code = client.post("/admin/enroll-code",
                       headers=admin_headers).json()["enrollment_code"]
    client.post("/api/v1/enroll", json={
        "enrollment_code": code,
        "hardware_model": "MagBound Smart Timer V2",
        "chip_family": "ESP32-C3",
        "build_id": "aabbccddeeff00112233aabb",
    })
    r = client.get("/admin/timers", headers=admin_headers)
    assert r.status_code == 200
    body = r.json()
    assert len(body) == 1
    assert body[0]["hardware_model"] == "MagBound Smart Timer V2"
    assert body[0]["connected_now"] is False   # no WSS attached in this test


def test_delete_timer_404_on_unknown(client, admin_headers):
    r = client.delete("/admin/timers/nope", headers=admin_headers)
    assert r.status_code == 404


def test_admin_health_ok(client):
    # Health is deliberately unauthenticated so uptime probes can hit it.
    r = client.get("/admin/health")
    assert r.status_code == 200
    body = r.json()
    assert body["ok"] is True
    assert body["protocol"] == "ost-relay/1"



def test_admin_page_is_static_and_locked_down(client):
    resp = client.get("/admin/")
    assert resp.status_code == 200
    assert "Invite owner" in resp.text and "One-time code" in resp.text and "OSTRELAY_ADMIN_TOKEN" in resp.text
    csp = resp.headers["content-security-policy"]
    assert "frame-ancestors 'none'" in csp and "unsafe-inline" not in csp
    assert "<script>" not in resp.text and " style=" not in resp.text
    for asset in ("/admin/assets/admin.js", "/admin/assets/pages.css"):
        assert asset in resp.text and client.get(asset).status_code == 200
    assert resp.headers["cache-control"] == "no-store"


def test_setup_code_command_mints_a_single_use_code(capsys, fresh_db):
    from ostrelay import setup_code
    assert setup_code.main([]) == 0
    printed = capsys.readouterr().out
    assert "Single use; expires in 15 minutes" in printed or "expires in 14 minutes" in printed
    rows = fresh_db.connect().execute("SELECT code, consumed_at FROM enrollment_codes").fetchall()
    assert len(rows) == 1 and rows[0]["consumed_at"] is None and rows[0]["code"] in printed
