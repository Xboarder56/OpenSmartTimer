"""OSTRELAY_ENROLLMENT: owner codes (reusable, per owner) and open enrollment."""

from __future__ import annotations

import pytest

ENROLL = {"hardware_model": "MagBound Smart Timer V2", "chip_family": "ESP32-C3",
          "build_id": "aabbccddeeff00112233aabb"}


def _enroll(client, code):
    return client.post("/api/v1/enroll", json={"enrollment_code": code, **ENROLL})


def _configure(monkeypatch, **values):
    from ostrelay import config
    for key, value in values.items():
        monkeypatch.setattr(config._settings, key, value)


def test_enrollment_setting_is_validated():
    from ostrelay import config
    assert config.Settings(_env_file=None, enrollment="owner_codes, open").enrollment_modes() == {"owner_codes", "open"}
    with pytest.raises(ValueError):
        config.Settings(_env_file=None, enrollment="everyone")


def test_first_owner_is_created_once_and_code_is_reusable(client, fresh_db):
    from ostrelay import owners
    assert [o["name"] for o in owners.list_owners()] == ["owner"]  # created at startup
    assert owners.ensure_first_owner() is None
    code = owners.rotate_owner("owner")
    first, second = _enroll(client, code), _enroll(client, code)
    assert first.status_code == 200 and second.status_code == 200
    # Each timer gets its own identity; the second must not replace the first.
    assert first.json()["timer_id"] != second.json()["timer_id"]
    rows = fresh_db.connect().execute("SELECT owner_id FROM timers").fetchall()
    assert len(rows) == 2 and rows[0]["owner_id"] and rows[0]["owner_id"] == rows[1]["owner_id"]
    assert owners.list_owners()[0]["timers"] == 2


@pytest.mark.parametrize("name", ["Kitchen timer", 'Timer "A" \\ room', "N" * 32, "  Bedroom timer  ", ""])
def test_enrollment_initializes_display_label(client, fresh_db, admin_headers, name):
    from ostrelay import owners
    code = owners.rotate_owner("owner")
    result = client.post("/api/v1/enroll", json={"enrollment_code": code, **ENROLL, "device_name": name})
    assert result.status_code == 200
    timer_id = result.json()["timer_id"]
    row = fresh_db.connect().execute("SELECT label FROM timers WHERE timer_id=?", (timer_id,)).fetchone()
    assert row["label"] == name.strip()
    listed = next(t for t in client.get("/admin/timers", headers=admin_headers).json() if t["timer_id"] == timer_id)
    assert listed["label"] == name.strip()


@pytest.mark.parametrize("name", ["N" * 33, "Timer\n", "Timer\x00", "计时器", None])
def test_invalid_enrollment_name_does_not_consume_code(client, fresh_db, admin_headers, name):
    code = client.post("/admin/enroll-code", headers=admin_headers).json()["enrollment_code"]
    result = client.post("/api/v1/enroll", json={"enrollment_code": code, **ENROLL, "device_name": name})
    assert result.status_code == 422
    assert fresh_db.connect().execute("SELECT count(*) AS n FROM timers").fetchone()["n"] == 0
    assert _enroll(client, code).status_code == 200


def test_owner_code_rejects_wrong_secret_rotation_and_revocation(client, fresh_db):
    from ostrelay import owners
    code = owners.create_owner("alex")
    prefix, owner_id, secret = code.split(".")
    assert _enroll(client, f"{prefix}.{owner_id}.{'A' * len(secret)}").status_code == 403
    assert _enroll(client, code).status_code == 200
    rotated = owners.rotate_owner("alex")
    assert _enroll(client, code).status_code == 403
    assert _enroll(client, rotated).status_code == 200
    assert len(owners.revoke_owner("alex")) == 2
    assert fresh_db.connect().execute("SELECT count(*) AS n FROM timers WHERE owner_id=?",
                                      (owner_id,)).fetchone()["n"] == 0
    assert _enroll(client, rotated).status_code == 403


def test_owner_codes_can_be_turned_off(client, monkeypatch):
    from ostrelay import owners
    code = owners.create_owner("alex")
    _configure(monkeypatch, enrollment="open")
    assert _enroll(client, code).status_code == 403


def test_per_owner_and_total_timer_caps(client, monkeypatch):
    from ostrelay import owners
    _configure(monkeypatch, max_timers_per_owner=2, max_enrolled_timers=3)
    alex, sam = owners.create_owner("alex"), owners.create_owner("sam")
    assert [_enroll(client, alex).status_code for _ in range(3)] == [200, 200, 409]
    assert _enroll(client, alex).json()["detail"] == "owner_timer_limit"
    assert _enroll(client, sam).status_code == 200
    full = _enroll(client, sam)
    assert full.status_code == 409 and full.json()["detail"] == "relay_full"


def test_open_enrollment_is_off_by_default(client):
    from ostrelay.owners import OPEN_ENROLLMENT_CODE
    assert _enroll(client, OPEN_ENROLLMENT_CODE).status_code == 403
    assert client.get("/setup").status_code == 404


def test_open_enrollment_and_rate_limit(client, monkeypatch):
    from ostrelay import enroll
    from ostrelay.owners import OPEN_ENROLLMENT_CODE
    _configure(monkeypatch, enrollment="open", rate_open_enrollments_per_ip_1h=2)
    monkeypatch.setattr(enroll, "_open_buckets", {})
    results = [_enroll(client, OPEN_ENROLLMENT_CODE) for _ in range(3)]
    assert [r.status_code for r in results] == [200, 200, 429]
    assert results[0].json()["timer_id"] != results[1].json()["timer_id"]


def test_setup_page_shows_open_code_only_with_a_pin(client, monkeypatch):
    import base64, json, re, hashlib
    _configure(monkeypatch, enrollment="owner_codes,open")
    assert client.get("/setup").status_code == 404  # no pin in dev mode
    _configure(monkeypatch, spki_pin="A" * 43, public_url="https://relay.example.com")
    page = client.get("/setup")
    assert page.status_code == 200
    code = re.search(r"ost-relay-setup:[A-Za-z0-9_-]+", page.text).group(0).split(":", 1)[1]
    decoded = json.loads(base64.urlsafe_b64decode(code + "=" * (-len(code) % 4)))
    assert decoded == {"url": "https://relay.example.com", "code": "open-enrollment", "pin": "A" * 43,
                       "partner_origin": ""}
    csp = page.headers["content-security-policy"]
    assert "unsafe-inline" not in csp and "<script>" not in page.text
    for asset in ("/setup/assets/setup.js", "/setup/assets/pages.css"):
        assert client.get(asset).status_code == 200

def test_owner_admin_api(client, admin_headers):
    assert client.post("/admin/owners", json={"name": "alex"}).status_code == 401
    invited = client.post("/admin/owners", json={"name": "alex"}, headers=admin_headers)
    assert invited.status_code == 200 and invited.json()["owner_code"].startswith("o1.")
    assert client.post("/admin/owners", json={"name": "alex"}, headers=admin_headers).status_code == 409
    assert client.post("/admin/owners", json={"name": "<b>"}, headers=admin_headers).status_code == 409
    assert _enroll(client, invited.json()["owner_code"]).status_code == 200
    rotated = client.post("/admin/owners/alex/rotate", headers=admin_headers)
    assert rotated.json()["owner_code"] != invited.json()["owner_code"]
    timers = client.get("/admin/timers", headers=admin_headers).json()
    assert [t["owner"] for t in timers] == ["alex"]
    overview = client.get("/admin/overview", headers=admin_headers).json()
    assert overview["timers_enrolled"] == 1 and overview["enrollment"] == ["owner_codes"]
    assert client.get("/admin/overview").status_code == 401
    listed = {o["name"]: o for o in client.get("/admin/owners", headers=admin_headers).json()}
    assert listed["alex"]["timers"] == 1 and not listed["alex"]["revoked"]
    revoked = client.post("/admin/owners/alex/revoke", headers=admin_headers).json()
    assert revoked == {"revoked": True, "timers_removed": 1}
    assert client.post("/admin/owners/nobody/rotate", headers=admin_headers).status_code == 404


def test_setup_code_command_manages_owners(capsys, fresh_db):
    from ostrelay import setup_code
    assert setup_code.main(["--invite", "alex"]) == 0
    assert "o1." in capsys.readouterr().out  # no pin in dev mode: raw owner code
    assert setup_code.main(["--invite", "alex"]) == 1
    assert "already exists" in capsys.readouterr().out
    assert setup_code.main(["--list"]) == 0
    assert "alex" in capsys.readouterr().out
    assert setup_code.main(["--revoke", "alex"]) == 0
    assert setup_code.main(["--list"]) == 0
    assert "revoked" in capsys.readouterr().out.split("Revoked alex")[-1]


def test_older_database_gains_owner_column(tmp_path, monkeypatch):
    import sqlite3
    from ostrelay import config, db
    path = tmp_path / "old.db"
    raw = sqlite3.connect(path)
    raw.execute("CREATE TABLE timers (timer_id TEXT PRIMARY KEY, enrollment_secret BLOB NOT NULL, "
                "hardware_model TEXT NOT NULL, chip_family TEXT NOT NULL, build_id TEXT NOT NULL, "
                "spki_sha256_pin TEXT NOT NULL, label TEXT NOT NULL DEFAULT '', created_at INTEGER NOT NULL, "
                "last_seen_at INTEGER)")
    raw.commit(); raw.close()
    config._settings = config.Settings(_env_file=None, db_path=str(path))
    monkeypatch.setattr(db, "_conn", None)
    try:
        columns = {r[1] for r in db.connect().execute("PRAGMA table_info(timers)").fetchall()}
        assert "owner_id" in columns
    finally:
        db.connect().close()
        monkeypatch.setattr(db, "_conn", None)
        config._settings = None


def test_admin_page_origin_may_post_but_other_origins_may_not(client, admin_headers):
    same = {**admin_headers, "Origin": "http://127.0.0.1:8080", "Host": "127.0.0.1:8080"}
    assert client.post("/admin/enroll-code", headers=same).status_code == 200
    foreign = {**admin_headers, "Origin": "https://evil.example", "Host": "127.0.0.1:8080"}
    assert client.post("/admin/enroll-code", headers=foreign).json()["detail"] == "origin_rejected"
    # Partner endpoints keep the public-origin rule.
    assert client.post("/api/v1/session", json={}, headers={"Origin": "http://127.0.0.1:8080",
                                                         "Host": "127.0.0.1:8080"}).status_code == 403


def test_home_page_is_public_and_reveals_nothing(client, monkeypatch):
    page = client.get("/")
    assert page.status_code == 200 and "Smart Timer relay" in page.text
    assert "/setup" not in page.text.split("<main")[1].split("setup.js")[0].replace("/setup/assets", "")
    _configure(monkeypatch, enrollment="owner_codes,open", spki_pin="A" * 43, public_url="https://relay.example.com")
    assert 'href="https://relay.example.com/setup"' in client.get("/").text
    assert "unsafe-inline" not in page.headers["content-security-policy"]
