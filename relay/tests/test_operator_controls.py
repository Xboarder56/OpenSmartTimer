"""Operator switches and reviewed bulk removal; isolated database only."""
import pytest

ENROLL = {"hardware_model": "MagBound Smart Timer V2", "chip_family": "ESP32-C3",
          "build_id": "aabbccddeeff00112233aabb"}


def pause(client, headers, requested):
    return client.put('/admin/enrollment-settings', headers=headers,
                      json={'open_enrollment_requested': requested})


def test_pause_rejects_copied_public_code_and_preserves_existing_timers(client, admin_headers, fresh_db, monkeypatch):
    from ostrelay import config, owners
    monkeypatch.setattr(config._settings, 'enrollment', 'owner_codes,open')
    monkeypatch.setattr(config._settings, 'spki_pin', 'A' * 43)
    body = {**ENROLL, 'enrollment_code': owners.OPEN_ENROLLMENT_CODE}
    before = client.post('/api/v1/enroll', json=body).json()
    assert client.get('/setup').status_code == 200
    assert pause(client, admin_headers, False).json()['open_enrollment_enabled'] is False
    assert client.get('/setup').status_code == 404
    assert client.post('/api/v1/enroll', json=body).status_code == 403
    saved = fresh_db.connect().execute('SELECT * FROM timers').fetchall()
    assert len(saved) == 1 and saved[0]['timer_id'] == before['timer_id']
    import base64
    assert bytes(saved[0]['enrollment_secret']) == base64.urlsafe_b64decode(before['enrollment_secret'] + '==')
    # Invited and one-time enrollment remain separate policies.
    code = client.post('/admin/enroll-code', headers=admin_headers).json()['enrollment_code']
    assert client.post('/api/v1/enroll', json={**ENROLL, 'enrollment_code': code}).status_code == 200
    assert pause(client, admin_headers, True).json()['open_enrollment_enabled'] is True
    assert client.get('/setup').status_code == 200
    assert client.post('/api/v1/enroll', json=body).status_code == 200


def test_environment_hard_gate_and_admin_auth(client, admin_headers):
    assert client.get('/admin/enrollment-settings').status_code == 401
    assert pause(client, {}, False).status_code == 401
    assert pause(client, admin_headers, True).status_code == 409
    assert pause(client, admin_headers, False).status_code == 200
    assert client.get('/admin/enrollment-settings', headers=admin_headers).json() == {
        'open_enrollment_requested': False, 'open_enrollment_allowed': False,
        'open_enrollment_enabled': False}
    assert pause(client, {**admin_headers, 'Origin': 'https://untrusted.example'}, False).status_code == 403


@pytest.mark.parametrize('body', [{}, {'open_enrollment_requested': 'false'},
                                  {'open_enrollment_requested': 1},
                                  {'open_enrollment_requested': False, 'enrollment': 'open'}])
def test_enrollment_setting_types_are_strict(client, admin_headers, body):
    assert client.put('/admin/enrollment-settings', headers=admin_headers, json=body).status_code == 422


def test_pause_survives_database_reopen(fresh_settings, tmp_path, monkeypatch):
    from ostrelay import config, db, site_settings
    monkeypatch.setattr(config._settings, 'db_path', str(tmp_path/'relay.sqlite'))
    monkeypatch.setattr(config._settings, 'enrollment', 'open')
    monkeypatch.setattr(db, '_conn', None)
    try:
        site_settings.set_open_enrollment_requested(False)
        db.connect().close(); db._conn = None
        assert not site_settings.open_enrollment_settings()['open_enrollment_enabled']
    finally:
        db.connect().close(); db._conn = None


def test_paused_enrollment_keeps_sessions_and_registered_timer_reconnect(client, admin_headers, fresh_db, monkeypatch):
    import secrets
    from ostrelay import config, owners, ws
    from ostrelay.wire import b64decode, b64encode
    monkeypatch.setattr(config._settings, 'enrollment', 'open')
    enrolled = client.post('/api/v1/enroll', json={**ENROLL, 'enrollment_code': owners.OPEN_ENROLLMENT_CODE}).json()
    identifier = enrolled['timer_id']
    secret = b64decode(enrolled['enrollment_secret'], 32)
    boot = 'aabbccdd'
    handle = secrets.token_urlsafe(16)
    now = fresh_db.now_unix()
    with fresh_db.transaction() as tx:
        tx.execute('INSERT INTO tokens (fp_hash,timer_id,scope_json,boot_id,exp_up_ms,label,minted_at) VALUES (?,?,?,?,?,?,?)',
                   ('existing-public-grant', identifier, '["view"]', boot, 60000, 'test', now))
        tx.execute('INSERT INTO sessions VALUES (?,?,?,?,?)', (handle, 'existing-public-grant', 'client', now, now+60))
    for connection in range(2):
        with client.websocket_connect('/api/v1/timer', subprotocols=['ost-relay.v1']) as socket:
            nonce = b64decode(socket.receive_json()['server_nonce'], 16)
            frame = {'type': 'hello', 'protocol': 'ost-relay/1', 'timer_id': identifier,
                     'boot_id': boot, 'uptime_ms': 123, 'resume_from_upstream_seq': 0,
                     'resume_from_downstream_seq': 0,
                     'hello_tag': b64encode(ws._hello_tag(secret, identifier, boot, 123, 0, 0, nonce, bytes(32)))}
            socket.send_json(frame)
            assert socket.receive_json()['type'] == 'hello_ack'
            if connection == 0:
                assert pause(client, admin_headers, False).status_code == 200
            assert client.get('/api/v1/session', headers={'x-ost-session': handle}).status_code == 200
            assert client.get('/admin/timers', headers=admin_headers).json()[0]['connected_now']
    assert client.post('/api/v1/enroll', json={**ENROLL, 'enrollment_code': owners.OPEN_ENROLLMENT_CODE}).status_code == 403


def test_admin_off_removes_pages_assets_apis_preview_and_health(fresh_db, fresh_registry, monkeypatch, admin_headers):
    from ostrelay import config
    from ostrelay.main import create_app
    from fastapi.testclient import TestClient
    monkeypatch.setattr(config._settings, 'admin_enabled', False)
    with TestClient(create_app()) as client:
        for path in ('/admin', '/admin/', '/admin/assets/admin.js', '/admin/assets/pages.css',
                     '/admin/landing-preview', '/admin/health', '/admin/overview',
                     '/admin/timers', '/admin/owners', '/admin/site-settings', '/admin/enrollment-settings'):
            assert client.get(path, headers=admin_headers).status_code == 404, path
        assert client.post('/admin/enroll-code', headers=admin_headers).status_code == 404
        assert client.delete('/admin/timers', headers=admin_headers).status_code == 404
        assert client.get('/api/v1/health').status_code == 200
        assert client.get('/').status_code == 200


def clear_body(client, headers):
    return {'confirmation': 'clear_all_timers',
            'inventory_revision': client.get('/admin/overview', headers=headers).json()['timer_inventory_revision']}


def test_clear_all_requires_auth_confirmation_and_current_inventory(client, admin_headers):
    valid = clear_body(client, admin_headers)
    assert client.request('DELETE', '/admin/timers', json=valid).status_code == 401
    for body in ({}, {**valid, 'confirmation': 'yes'}, {**valid, 'extra': True}):
        assert client.request('DELETE', '/admin/timers', headers=admin_headers, json=body).status_code == 422
    code = client.post('/admin/enroll-code', headers=admin_headers).json()['enrollment_code']
    enrolled = client.post('/api/v1/enroll', json={**ENROLL, 'enrollment_code': code}).json()['timer_id']
    response = client.request('DELETE', '/admin/timers', headers=admin_headers, json=valid)
    assert response.status_code == 409 and response.json()['detail'] == 'timer_inventory_changed'
    assert client.get('/admin/timers', headers=admin_headers).json()[0]['timer_id'] == enrolled


def test_clear_all_cascades_grants_sessions_preserves_owners_settings_and_closes_sockets(client, admin_headers, fresh_db, monkeypatch):
    from ostrelay import owners, site_settings, ws
    from types import SimpleNamespace
    code = owners.create_owner('test-owner')
    ids = [client.post('/api/v1/enroll', json={**ENROLL, 'enrollment_code': code}).json()['timer_id'] for _ in range(2)]
    site_settings.set_landing_page_visible(False)
    site_settings.set_open_enrollment_requested(False)
    with fresh_db.transaction() as tx:
        tx.execute('INSERT INTO tokens (fp_hash,timer_id,scope_json,boot_id,exp_up_ms,label,minted_at,revoked) VALUES (?,?,?,?,?,?,?,?)',
                   ('public-fingerprint', ids[0], '["view"]', 'boot', 60000, 'test', fresh_db.now_unix(), 0))
        tx.execute('INSERT INTO sessions (session_id,fp_hash,client_id,created_at,expires_at) VALUES (?,?,?,?,?)',
                   ('session', 'public-fingerprint', 'client', fresh_db.now_unix(), fresh_db.now_unix()+60))
    closed = []
    class Socket:
        async def close(self, **kwargs): closed.append(kwargs)
    monkeypatch.setattr(ws.registry, 'get', lambda identifier: SimpleNamespace(ws=Socket()) if identifier in ids else None)
    response = client.request('DELETE', '/admin/timers', headers=admin_headers, json=clear_body(client, admin_headers))
    assert response.json() == {'timers_removed': 2}
    for table in ('timers', 'tokens', 'sessions'):
        assert fresh_db.connect().execute(f'SELECT count(*) AS n FROM {table}').fetchone()['n'] == 0
    assert owners.owner_for_code(code) is not None
    assert not site_settings.landing_page_settings()['landing_page_visible']
    assert not site_settings.open_enrollment_settings()['open_enrollment_requested']
    assert len(closed) == 2 and all(item['code'] == 4003 for item in closed)


@pytest.mark.parametrize('failure', ['closed', 'stalled'])
def test_clear_all_invalidates_sessions_even_when_socket_close_fails(client, admin_headers, fresh_db, monkeypatch, failure):
    import asyncio, secrets
    from types import SimpleNamespace
    from ostrelay import config, ws
    from ostrelay.session import resolve_session
    from fastapi import HTTPException
    code = client.post('/admin/enroll-code', headers=admin_headers).json()['enrollment_code']
    identifier = client.post('/api/v1/enroll', json={**ENROLL, 'enrollment_code': code}).json()['timer_id']
    handle = secrets.token_urlsafe(16)
    now = fresh_db.now_unix()
    with fresh_db.transaction() as tx:
        tx.execute('INSERT INTO tokens (fp_hash,timer_id,scope_json,boot_id,exp_up_ms,label,minted_at) VALUES (?,?,?,?,?,?,?)',
                   ('existing-public-grant', identifier, '["view"]', 'aabbccdd', 60000, 'test', now))
        tx.execute('INSERT INTO sessions VALUES (?,?,?,?,?)', (handle, 'existing-public-grant', 'client', now, now+60))
    class Socket:
        async def close(self, **kwargs):
            if failure == 'closed':
                raise RuntimeError('already closed')
            await asyncio.Event().wait()
    monkeypatch.setattr(config._settings, 'ws_send_timeout_s', 0.01)
    monkeypatch.setattr(ws.registry, 'get', lambda timer: SimpleNamespace(ws=Socket()) if timer == identifier else None)
    response = client.request('DELETE', '/admin/timers', headers=admin_headers, json=clear_body(client, admin_headers))
    assert response.status_code == 200 and response.json() == {'timers_removed': 1}
    assert client.get('/api/v1/session', headers={'x-ost-session': handle}).status_code == 401
    with pytest.raises(HTTPException) as refused:
        resolve_session(handle)
    assert refused.value.status_code == 401
    assert fresh_db.connect().execute('SELECT count(*) AS n FROM timers').fetchone()['n'] == 0


def test_same_count_replacement_invalidates_clear_review(client, admin_headers):
    def add():
        code = client.post('/admin/enroll-code', headers=admin_headers).json()['enrollment_code']
        return client.post('/api/v1/enroll', json={**ENROLL, 'enrollment_code': code}).json()['timer_id']
    original = add(); reviewed = clear_body(client, admin_headers)
    client.delete('/admin/timers/'+original, headers=admin_headers); replacement = add()
    assert client.request('DELETE', '/admin/timers', headers=admin_headers, json=reviewed).status_code == 409
    assert client.get('/admin/timers', headers=admin_headers).json()[0]['timer_id'] == replacement


def test_admin_environment_switch(monkeypatch):
    from ostrelay.config import Settings
    monkeypatch.setenv('OSTRELAY_ADMIN_ENABLED', 'false')
    assert Settings(_env_file=None).admin_enabled is False
