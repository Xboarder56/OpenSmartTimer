"""Security boundary regressions: malformed authentication and stalled peers."""
import asyncio
import time

import pytest
from fastapi import HTTPException
from starlette.requests import Request

from ostrelay import admin, config, portal, session, ws


def test_non_ascii_admin_header_is_unauthorized(fresh_settings):
    with pytest.raises(HTTPException) as error:
        admin.require_admin("Bearer \xff")
    assert error.value.status_code == 401


def test_non_ascii_session_handle_is_a_mismatch(fresh_settings, monkeypatch):
    monkeypatch.setattr(session, "resolve_session", lambda _: {"session_id": "valid-session"})
    request = Request({"type": "http", "method": "GET", "scheme": "https",
                       "path": "/api/v1/watch", "query_string": b"session_handle=%C3%A9",
                       "headers": []})
    with pytest.raises(HTTPException) as error:
        asyncio.run(session.require_session(request))
    assert error.value.status_code == 401


@pytest.mark.parametrize("phase", ["accept", "challenge", "receive"])
def test_initial_handshake_deadline_reclaims_slot(fresh_settings, fresh_registry, monkeypatch, phase):
    monkeypatch.setattr(config._settings, "hello_timeout_s", 0.03)

    class StalledPeer:
        scope = {"subprotocols": ["ost-relay.v1"]}
        closed = False

        async def accept(self, **kwargs):
            if phase == "accept":
                await asyncio.sleep(3600)

        async def send_json(self, frame):
            if phase == "challenge":
                await asyncio.sleep(3600)

        async def receive(self):
            await asyncio.sleep(3600)

        async def close(self, **kwargs):
            self.closed = True

    async def run():
        peer = StalledPeer()
        try:
            await asyncio.wait_for(ws.timer_socket(peer), 0.3)
        except asyncio.TimeoutError:
            pytest.fail(f"{phase} held the unauthenticated handshake slot")
        assert peer.closed and fresh_registry.handshakes == 0

    asyncio.run(run())


def test_relay_does_not_host_partner_shell(client):
    response = client.get("/s/")
    assert response.status_code == 404
    assert response.headers.get("x-frame-options") == "DENY"
    assert response.headers.get("referrer-policy") == "no-referrer"
    assert response.headers.get("x-content-type-options") == "nosniff"
    assert response.headers.get("cache-control") == "no-store"


def test_operator_assets_cannot_expose_partner_code(client):
    for name in ("appearance.js", "timer-ui.css", "icon.svg", "i18n.js"):
        assert client.get("/assets/" + name).status_code == 200
    for name in ("app.js", "e2e.js", "index.html", "../spa/app.js"):
        assert client.get("/assets/" + name).status_code == 404
    for path in ("/s/", "/s/assets/app.js", "/s/assets/e2e.js"):
        assert client.get(path).status_code == 404


def test_partner_shared_styles_and_translations_match_local_ui():
    from pathlib import Path
    root = Path(__file__).resolve().parents[2]
    assert (root / "relay/spa/timer-ui.css").read_bytes() == (root / "web/styles.css").read_bytes()
    assert (root / "relay/spa/i18n.js").read_bytes() == (root / "web/i18n.js").read_bytes()


def test_owner_session_capacity_is_atomic_and_expired_rows_are_reclaimed(fresh_db, monkeypatch):
    from ostrelay import owners
    owners.create_owner("one")
    owners.create_owner("two")
    ids = [row["owner_id"] for row in fresh_db.connect().execute("SELECT owner_id FROM owners ORDER BY name").fetchall()]
    monkeypatch.setattr(config._settings, "max_owner_sessions", 2, raising=False)
    monkeypatch.setattr(config._settings, "max_owner_sessions_per_owner", 1, raising=False)
    portal._create_session(ids[0])
    with pytest.raises(HTTPException) as error:
        portal._create_session(ids[0])
    assert error.value.status_code == 429
    portal._create_session(ids[1])
    monkeypatch.setattr(config._settings, "max_owner_sessions_per_owner", 2)
    with pytest.raises(HTTPException) as error:
        portal._create_session(ids[1])
    assert error.value.status_code == 429
    assert fresh_db.connect().execute("SELECT count(*) AS n FROM owner_sessions").fetchone()["n"] == 2
    fresh_db.connect().execute("UPDATE owner_sessions SET expires_at=? WHERE owner_id=?", (int(time.time()) - 1, ids[0]))
    portal._create_session(ids[0])
    assert fresh_db.connect().execute("SELECT count(*) AS n FROM owner_sessions").fetchone()["n"] == 2


def test_owner_inventory_is_bounded_without_locking_out_existing_owners(fresh_db, monkeypatch):
    from ostrelay import owners
    monkeypatch.setattr(config._settings, "max_owners", 1)
    known = owners.owner_for_sign_in("issuer|one", "", "one")
    with pytest.raises(ValueError, match="owner_capacity"):
        owners.owner_for_sign_in("issuer|two", "", "two")
    with pytest.raises(ValueError, match="owner_capacity"):
        owners.create_owner("invited")
    assert owners.owner_for_sign_in("issuer|one", "", "one")["owner_id"] == known["owner_id"]


def test_database_and_wal_files_are_private(fresh_settings, monkeypatch, tmp_path):
    import os
    import stat
    from ostrelay import db
    if os.name != "posix":
        pytest.skip("POSIX file modes")
    path = tmp_path / "private.sqlite"
    path.touch(mode=0o644)
    monkeypatch.setattr(config._settings, "db_path", str(path))
    monkeypatch.setattr(db, "_conn", None)
    connection = db.connect()
    try:
        for file in (path, tmp_path / "private.sqlite-wal", tmp_path / "private.sqlite-shm"):
            assert file.exists()
            assert stat.S_IMODE(file.stat().st_mode) == 0o600
    finally:
        connection.close()
        db._conn = None


def test_public_protocol_has_no_pre_public_aliases(client):
    from starlette.websockets import WebSocketDisconnect
    assert client.get("/api/v1/health").json()["protocol"] == "ost-relay/1"
    assert client.get("/api/v2/health").status_code == 404
    with pytest.raises(WebSocketDisconnect):
        with client.websocket_connect("/api/v1/timer", subprotocols=["ost-relay.v2"]):
            pass
