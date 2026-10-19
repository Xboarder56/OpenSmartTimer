"""Opaque transport admission, binding and revocation regressions."""
import json
import time
import pytest
from pydantic import ValidationError
from ostrelay import db, ws
from ostrelay.models import CommandRequest
from ostrelay.wire import b64encode


def opaque():
    return dict(type='e2e',fp_hash=b64encode(bytes(16)),client_id=b64encode(bytes([1])*16),
        server_id=b64encode(bytes([2])*16),seq=1,request_id='request',ciphertext=b64encode(bytes(16)))


def test_clear_commands_rejected_and_opaque_shape_accepted():
    with pytest.raises(ValidationError):CommandRequest(op='pause',op_id='request',params={})
    assert CommandRequest(**opaque()).seq==1
    for extra in ('cap_token','params','op','if_boot_id'):
        with pytest.raises(ValidationError):CommandRequest(**{**opaque(),extra:'secret'})


@pytest.fixture
def routing(fresh_db,fresh_registry):
    conn=db.connect()
    fp,cid,handle=opaque()['fp_hash'],opaque()['client_id'],b64encode(bytes([3])*16)
    conn.execute('INSERT INTO timers(timer_id,enrollment_secret,hardware_model,chip_family,build_id,spki_sha256_pin,created_at) VALUES(?,?,?,?,?,?,?)',
        ('timer',bytes(32),'test','test','a'*24,'',0))
    conn.execute('INSERT INTO tokens(fp_hash,timer_id,scope_json,boot_id,exp_up_ms,minted_at) VALUES(?,?,?,?,?,?)',
        (fp,'timer',json.dumps(['view']),'aabbccdd',60000,0))
    conn.execute('INSERT INTO sessions VALUES(?,?,?,?,?)',(handle,fp,cid,0,int(time.time())+600))
    live=ws.WsSession(timer_id='timer',ws=None,session_id='wss',boot_id='aabbccdd')
    fresh_registry._sessions['timer']=live
    return conn,live,handle


def test_transport_session_needs_explicit_handle(client,routing):
    conn,live,handle=routing
    assert client.get('/api/v1/session').status_code==401
    response=client.get('/api/v1/session',headers={'X-OST-Session':handle})
    assert response.status_code==200
    assert set(response.json())=={'session_handle','timer_id','boot_id'}
    assert not response.cookies


@pytest.mark.parametrize('change',['boot','revoke','expiry'])
def test_transport_session_rechecks_binding(client,routing,change):
    conn,live,handle=routing
    if change=='boot':live.boot_id='deadbeef'
    elif change=='revoke':conn.execute('UPDATE tokens SET revoked=1')
    else:conn.execute('UPDATE sessions SET expires_at=0')
    assert client.get('/api/v1/session',headers={'X-OST-Session':handle}).status_code==401


def test_client_identity_cannot_be_swapped(client,routing):
    conn,live,handle=routing
    body={**opaque(),'client_id':b64encode(bytes(16))}
    assert client.post('/api/v1/command',json=body,headers={'X-OST-Session':handle}).status_code==409


def test_logout_releases_routing_admission(client,routing):
    conn,live,handle=routing
    assert client.delete('/api/v1/session',headers={'X-OST-Session':handle}).status_code==200
    assert not conn.execute('SELECT * FROM sessions').fetchall()
