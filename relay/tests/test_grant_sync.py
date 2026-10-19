"""Timed full-control links and durable relay registration acknowledgments."""
import importlib.util
import json
import sys
from pathlib import Path

import httpx
import pytest

from test_mock_protocol import mock, timer
from ostrelay import ws

spec = importlib.util.spec_from_file_location('relay_owner_test', Path(__file__).parents[1] / 'mock-timer/owner.py')
owner = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = owner
spec.loader.exec_module(owner)


class Socket:
    state = mock.SocketState.OPEN
    def __init__(self):
        self.frames = []

    async def send(self, text):
        self.frames.append(json.loads(text))

    async def send_json(self, frame):
        self.frames.append(frame)


def server_session(database, client):
    database.connect().execute('INSERT INTO timers(timer_id,enrollment_secret,hardware_model,chip_family,build_id,spki_sha256_pin,created_at) VALUES(?,?,?,?,?,?,?)',
                              (client.cred.timer_id, bytes(32), 'test', 'test', 'a' * 24, '', 0))
    return ws.WsSession(timer_id=client.cred.timer_id, ws=Socket(), session_id='s', boot_id=client.timer.boot_id,
                        hello_uptime_ms=client.uptime_ms())


async def test_ack_loss_retry_and_revoke_before_mint(fresh_db):
    client = timer(); client.ws = Socket(); client.connected = True
    server = server_session(fresh_db, client)
    rec = await client.create_partner_link(60, 'Partner', 'create-1')
    first = client.ws.frames[-1]
    await ws._sync_token(server, first)
    # Commit succeeded, ACK lost: sharing remains unavailable.
    assert server.ws.frames[-1]['status'] == 'ready'
    assert client.link_info(rec, 'https://relay.example')['url'] is None
    expiry = rec.exp_up_ms
    await client._sync_token(rec)
    retry = client.ws.frames[-1]
    await ws._sync_token(server, retry)
    client.handle_token_ack(server.ws.frames[-1])
    assert rec.sync_state == 'ready' and rec.exp_up_ms == expiry
    assert fresh_db.connect().execute('SELECT count(*) FROM tokens').fetchone()[0] == 1
    assert client.link_info(rec, 'https://relay.example')['url'].endswith(rec.wire)
    await client.send_token_revoke(rec.fp_hash)
    revoke = client.ws.frames[-1]
    client.handle_token_ack({'type':'token_ack', 'action':'token_mint', 'upstream_seq':retry['upstream_seq'],
                             'fp_hash':rec.fp_hash, 'boot_id':rec.boot_id, 'status':'ready'})
    assert rec.sync_state == 'revoking'
    assert client.keys.verify(rec.wire, client.cred.timer_id, rec.boot_id, client.uptime_ms(), 'view')[0] == 'unauthorized'
    # Simulate a restore that lost registration, then revoke arrives FIRST.
    fresh_db.connect().execute('DELETE FROM tokens')
    await ws._sync_token(server, revoke)
    client.handle_token_ack(server.ws.frames[-1])
    assert rec.sync_state == 'revoked'
    await ws._sync_token(server, first)
    assert server.ws.frames[-1]['status'] == 'revoked'
    assert fresh_db.connect().execute('SELECT revoked FROM tokens').fetchone()[0] == 1


async def test_ack_identity_capacity_conflict_and_expiry(fresh_db, monkeypatch):
    from ostrelay.config import settings
    client = timer(); client.ws = Socket(); client.connected = True; server = server_session(fresh_db, client)
    rec = await client.create_partner_link(60, '', 'one')
    frame = client.ws.frames[-1]
    monkeypatch.setattr(settings(), 'max_tokens_per_timer', 0)
    await ws._sync_token(server, frame)
    assert server.ws.frames[-1]['status'] == 'capacity'
    client.handle_token_ack(server.ws.frames[-1]); assert rec.sync_state == 'pending'
    monkeypatch.setattr(settings(), 'max_tokens_per_timer', 32)
    await ws._sync_token(server, frame)
    ack = server.ws.frames[-1]
    for wrong in ({'boot_id':'badboot'}, {'fp_hash':'bad'}, {'upstream_seq':rec.sync_seq+1}, {'action':'token_revoke'}):
        client.handle_token_ack({**ack, **wrong}); assert rec.sync_state == 'pending'
    client.handle_token_ack(ack); assert rec.sync_state == 'ready'
    # Owner Extend: the same grant with a later expiry is accepted and stored.
    await ws._sync_token(server, {**frame, 'exp_up_ms':frame['exp_up_ms']+60_000})
    assert server.ws.frames[-1]['status'] == 'ready'
    assert fresh_db.connect().execute('SELECT exp_up_ms FROM tokens').fetchone()[0] == frame['exp_up_ms'] + 60_000
    # A different token under the same fingerprint still conflicts.
    await ws._sync_token(server, {**frame, 'token_digest': 'A' * 43})
    assert server.ws.frames[-1]['status'] == 'conflict'
    server.hello_uptime_ms = rec.exp_up_ms
    await ws._sync_token(server, frame)
    assert server.ws.frames[-1]['status'] == 'expired'
    client.handle_token_ack(server.ws.frames[-1])
    assert client.link_info(rec, 'https://relay.example')['url'] is None


async def test_creator_connection_full_controls_idempotency_and_fixed_expiry():
    client = timer()
    with pytest.raises(ConnectionError):
        await client.create_partner_link(3600, 'Partner', 'same-request')
    assert not client.keys.tokens
    client.ws = Socket(); client.connected = True
    rec = await client.create_partner_link(3600, 'Partner', 'same-request')
    assert rec.scopes == mock.PARTNER_SCOPES
    assert rec.sync_state == 'pending'
    expiry = rec.exp_up_ms
    duplicate = await client.create_partner_link(3600, 'Partner', 'same-request')
    assert duplicate is rec and duplicate.exp_up_ms == expiry and len(client.keys.tokens) == 1
    with pytest.raises(ValueError, match='different settings'):
        await client.create_partner_link(7200, 'Partner', 'same-request')
    client.ws = Socket(); rec.sync_sent_at = 0; await client.retry_token_sync()
    assert client.ws.frames[-1]['exp_up_ms'] == expiry
    await client.send_token_revoke(rec.fp_hash)
    assert client.ws.frames[-1]['type'] == 'token_revoke'
    client.boot_wall -= 3601
    await client.retry_token_sync()
    assert client.link_info(rec, 'https://relay.example')['state'] == 'expired'


@pytest.mark.parametrize('ttl', [0, -1, 86401, True, 1.2, '60'])
async def test_creator_rejects_invalid_lifetime(ttl):
    with pytest.raises(ValueError):
        await timer().create_partner_link(ttl, '', 'test')


async def test_owner_api_auth_policy_and_pending_links():
    session = timer()
    app = owner.create_owner_app(session, 'https://relay.example', 'test-secret')
    async with httpx.AsyncClient(transport=httpx.ASGITransport(app=app), base_url='http://127.0.0.1:8081') as client:
        assert (await client.get('/')).status_code == 200
        assert (await client.get('/api/links')).status_code == 401
        client.headers['X-OST-Owner'] = 'test-secret'
        assert (await client.get('/api/links', headers={'Origin':'https://evil.example'})).status_code == 403
        assert (await client.get('/api/links', headers={'Host':'evil.example:8081'})).status_code == 403
        payload = {'ttl_seconds':60, 'label':'Partner', 'request_id':'request-1'}
        assert (await client.post('/api/links', json={**payload, 'scopes':['view']})).status_code == 400
        assert (await client.post('/api/links', json=payload)).status_code == 503
        assert not session.keys.tokens
        session.ws = Socket(); session.connected = True
        response = await client.post('/api/links', json=payload)
        assert response.status_code == 202 and response.json()['state'] == 'pending' and response.json()['url'] is None
        again = await client.post('/api/links', json=payload)
        assert again.json()['fp_hash'] == response.json()['fp_hash']
        assert (await client.post('/api/links', content='x'*2049)).status_code == 413
        session.connected = False; session.ws = None
        assert (await client.get('/api/links')).json()['can_share'] is False
        result = await client.delete('/api/links/' + response.json()['fp_hash'])
        assert result.status_code == 202 and result.json()['state'] == 'revoking'
        assert (await client.get('/api/links')).headers['cache-control'] == 'no-store'


async def test_mock_connection_supervisor_retries_without_new_boot(monkeypatch):
    import asyncio
    session = timer()
    boot = session.timer.boot_id
    session.ws = Socket(); session.connected = True
    rec = await session.create_partner_link(60, 'accepted-online', 'online-create')
    session.ws = None; session.connected = False
    attempts, delays, closes = [], [], []

    class Connection:
        async def close(self):
            closes.append(True)

    async def connect():
        attempts.append(True)
        if len(attempts) == 1:
            raise OSError('offline')
        session.ws = Connection()
        if len(attempts) == 3:
            session._panic_requested = True

    async def read():
        return  # Established connection subsequently closes.

    async def periodic():
        await asyncio.Event().wait()

    async def sleep(delay):
        delays.append(delay)

    monkeypatch.setattr(session, 'connect', connect)
    monkeypatch.setattr(session, 'read_loop', read)
    monkeypatch.setattr(session, 'periodic_status', periodic)
    monkeypatch.setattr(mock.asyncio, 'sleep', sleep)
    await session.maintain_connection()
    assert len(attempts) == 3 and len(closes) == 2 and delays == [1, 1]
    assert session.timer.boot_id == boot and session.keys.tokens == [rec]
    assert session.ws is None and not session.connected
