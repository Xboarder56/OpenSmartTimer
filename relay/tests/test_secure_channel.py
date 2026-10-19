"""Attack the real C++ endpoint, rather than trusting the relay's decisions."""
import json
import pytest
from secure import BrowserOpen, enc, dec, timer_accept, hkdf, P256_ORDER
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from test_cpp_interop import raw_low_s
from test_cpp_interop import cpp, cpp_binary


@pytest.fixture
def connected(cpp):
    info=cpp.call('inspect')
    cpp.call('connect')
    cpp.call('frame',frame={'type':'challenge','server_nonce':'AAECAwQFBgcICQoLDA0ODw'})
    assert cpp.call('ack')['frames']==[]
    return cpp,info


def open_channel(cpp,info):
    opened=BrowserOpen(info['wire'])
    accepted=cpp.call('frame',frame=opened.frame)['frames']
    channel=opened.accept(next(f for f in accepted if f['type']=='e2e_accept'))
    return opened,channel


def command(info,op_id='attack-test',**overrides):
    return {'type':'command','cap_token':info['wire'],'op':'adjust','op_id':op_id,
        'params':{'delta_ms':1000},'if_boot_id':'aabbccdd','if_session_id':'session-1',
        'expiry_uptime_ms':6000,**overrides}


def encrypted_frames(response):
    return [f for f in response['frames'] if f['type']=='e2e']


@pytest.mark.parametrize('field,value',[('tag',enc(bytes(32))),('boot_id','deadbeef'),
    ('client_id',enc(bytes(16))),('fp_hash',enc(bytes(16))),('client_key',enc(b'\4'+bytes(64)))])
def test_modified_open_never_gets_accept(connected,field,value):
    cpp,info=connected
    opened=BrowserOpen(info['wire'])
    assert cpp.call('frame',frame={**opened.frame,field:value})['frames']==[]


@pytest.mark.parametrize('field,value',[('tag',enc(bytes(32))),('server_id',enc(bytes(16))),
    ('server_key',enc(b'\4'+bytes(64))),('client_id',enc(bytes(16))),('fp_hash',enc(bytes(16))),
    ('sig',enc(bytes(64)))])
def test_browser_rejects_modified_accept(connected,field,value):
    cpp,info=connected
    opened=BrowserOpen(info['wire'])
    frame=cpp.call('frame',frame=opened.frame)['frames'][0]
    with pytest.raises(ValueError):opened.accept({**frame,field:value})


@pytest.mark.parametrize('field,value',[('seq',42),('request_id','another-request'),
    ('server_id',enc(bytes(16))),('client_id',enc(bytes(16))),('fp_hash',enc(bytes(16))),
    ('ciphertext',enc(bytes(40)))])
def test_tampering_cannot_mutate_or_consume_valid_sequence(connected,field,value):
    cpp,info=connected; _,channel=open_channel(cpp,info)
    frame=channel.encrypt(command(info),'attack-test')
    bad=cpp.call('frame',frame={**frame,field:value})
    assert bad['applies']==0 and not encrypted_frames(bad)
    good=cpp.call('frame',frame=frame)
    assert good['applies']==1
    assert json.loads(channel.decrypt(encrypted_frames(good)[0])['body_json'])['code']=='applied'


def test_plaintext_fallback_is_rejected(connected):
    cpp,info=connected
    assert cpp.call('frame',frame={**command(info),'downstream_seq':1})['applies']==0
    assert cpp.call('status')['frames']==[]
    assert cpp.call('event')['frames']==[]


def test_replay_survives_result_cache_eviction_and_reconnect(connected):
    cpp,info=connected; _,channel=open_channel(cpp,info)
    first=channel.encrypt(command(info),'attack-test')
    result=cpp.call('frame',frame=first)
    assert result['applies']==1
    for i in range(12):
        last=result['frames'][-1]['upstream_seq']
        cpp.call('frame',frame={'type':'ack','acked_upstream_seq':last})
        cpp.call('tick',ms=250)
        frame=channel.encrypt(command(info,'evict-'+str(i)),'evict-'+str(i))
        result=cpp.call('frame',frame=frame)
        assert result['applies']==i+2
    assert cpp.call('frame',frame=first)['applies']==13
    cpp.call('reconnect')
    cpp.call('frame',frame={'type':'challenge','server_nonce':'AAECAwQFBgcICQoLDA0ODw'})
    cpp.call('ack')
    assert cpp.call('frame',frame=first)['applies']==13


def test_cross_session_and_reflected_ciphertext_fail(connected):
    cpp,info=connected; _,a=open_channel(cpp,info)
    sync=a.encrypt({'type':'sync','cap_token':info['wire']},'first-status')
    upstream=encrypted_frames(cpp.call('frame',frame=sync))[0]
    # Opposite direction has a different key and AAD direction.
    assert cpp.call('frame',frame={k:v for k,v in upstream.items() if k!='upstream_seq'})['applies']==0
    cpp.call('tick',ms=1000); _,b=open_channel(cpp,info)
    frame=a.encrypt(command(info),'attack-test')
    assert cpp.call('frame',frame={**frame,'client_id':enc(b.client),'server_id':enc(b.server)})['applies']==0
    assert cpp.call('frame',frame=frame)['applies']==1


@pytest.mark.parametrize('action',['revoke','view_only','expiry','idle'])
def test_timer_grant_and_session_limits_are_authoritative(connected,action):
    cpp,info=connected; _,channel=open_channel(cpp,info)
    if action=='expiry':cpp.call('tick',ms=3600001)
    elif action=='idle':cpp.call('tick',ms=10001)  # Unconfirmed opens expire.
    else:cpp.call(action)
    response=cpp.call('frame',frame=channel.encrypt(command(info),'attack-test'))
    assert response['applies']==0
    if action=='view_only':
        body=json.loads(channel.decrypt(encrypted_frames(response)[0])['body_json'])
        assert body['code']=='unauthorized'
    else:assert not encrypted_frames(response)


@pytest.mark.parametrize('changes',[{'expiry_uptime_ms':1000},{'expiry_uptime_ms':11001},
    {'if_boot_id':'deadbeef'},{'if_session_id':'old-session'},{'params':{'delta_ms':0}},
    {'params':{'delta_ms':True}},{'params':{'delta_ms':2**31}}])
def test_encryption_preserves_command_preconditions(connected,changes):
    cpp,info=connected; _,channel=open_channel(cpp,info)
    assert cpp.call('frame',frame=channel.encrypt(command(info,**changes),'attack-test'))['applies']==0


def test_owner_reactivation_requires_fresh_channel(connected):
    cpp,info=connected; _,old=open_channel(cpp,info)
    cpp.call('tick',ms=3600001)
    cpp.call('reconnect')
    cpp.call('frame',frame={'type':'challenge','server_nonce':'AAECAwQFBgcICQoLDA0ODw'})
    cpp.call('ack')
    assert cpp.call('extend',ms=60000)['accepted']
    assert cpp.call('frame',frame=old.encrypt({'type':'sync','cap_token':info['wire']},'old'))['frames']==[]
    _,fresh=open_channel(cpp,info)
    frame=fresh.encrypt({'type':'sync','cap_token':info['wire']},'reactivated')
    assert fresh.decrypt(encrypted_frames(cpp.call('frame',frame=frame))[0])['type']=='status'


def test_authenticated_malformed_data_consumes_sequence(connected):
    cpp,info=connected; _,channel=open_channel(cpp,info)
    bad=channel.encrypt({'type':'command','cap_token':'invalid'},'attack-test')
    assert not encrypted_frames(cpp.call('frame',frame=bad))
    channel.tx_seq=0  # Deliberate nonce misuse in the attacking test endpoint.
    valid=channel.encrypt(command(info),'attack-test')
    assert cpp.call('frame',frame=valid)['applies']==0


def test_hkdf_rfc5869_known_answer():
    result=hkdf(bytes.fromhex('0b'*22),bytes.fromhex('000102030405060708090a0b0c'),
        bytes.fromhex('f0f1f2f3f4f5f6f7f8f9'),42)
    assert result.hex()=='3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865'


def test_accept_requires_timer_identity_even_with_the_share_secret(connected):
    cpp, info = connected
    opened = BrowserOpen(info['wire'])
    accepted = cpp.call('frame', frame=opened.frame)['frames'][0]
    missing = {k: v for k, v in accepted.items() if k != 'sig'}
    with pytest.raises(ValueError): opened.accept(missing)
    signature = dec(accepted['sig'], 64)
    high = signature[:32] + (P256_ORDER - int.from_bytes(signature[32:], 'big')).to_bytes(32, 'big')
    with pytest.raises(ValueError): opened.accept({**accepted, 'sig': enc(high)})
    # A holder of the complete link can generate a valid grant MAC and a new
    # ECDH key, but cannot sign as the timer anchored in that retained link.
    attacker = ec.generate_private_key(ec.SECP256R1())
    forged, _ = timer_accept(opened.frame, opened.payload['tid'], opened.payload['boot'],
        dec(opened.payload['fp'], 16), lambda message: raw_low_s(attacker.sign(message, ec.ECDSA(hashes.SHA256()))))
    forged['vk'] = enc(attacker.public_key().public_bytes(
        serialization.Encoding.X962,
        serialization.PublicFormat.CompressedPoint))
    with pytest.raises(ValueError): opened.accept(forged)
    # Failed accepts do not consume the legitimate pending exchange.
    assert opened.accept(accepted).rx_seq == 0


def test_one_identity_signature_per_channel_and_none_for_routine_replies(connected):
    cpp, info = connected
    assert info['signing_calls'] == 0
    opened = BrowserOpen(info['wire'])
    first = cpp.call('frame', frame=opened.frame)
    assert first['signing_calls'] == 1
    accepted = first['frames'][0]
    repeated = cpp.call('frame', frame=opened.frame)
    assert repeated['signing_calls'] == 1
    assert {k: v for k, v in accepted.items() if k != 'upstream_seq'} == {
        k: v for k, v in repeated['frames'][0].items() if k != 'upstream_seq'}
    channel = opened.accept(accepted)
    seqs = []
    def check(response):
        assert response['signing_calls'] == 1
        for frame in encrypted_frames(response):
            seqs.append(frame['seq'])
            inner = channel.decrypt(frame)
            assert set(inner) == {'type', 'upstream_seq', 'body_json'}
    check(cpp.call('frame', frame=channel.encrypt({'type': 'sync', 'cap_token': info['wire']}, 'proof-1')))
    check(cpp.call('frame', frame=channel.encrypt(command(info), 'attack-test')))
    check(cpp.call('frame', frame=channel.encrypt({'type': 'lookup', 'cap_token': info['wire']}, 'attack-test')))
    check(cpp.call('event'))
    check(cpp.call('reconnect'))
    check(cpp.call('frame', frame={'type': 'challenge', 'server_nonce': 'AAECAwQFBgcICQoLDA0ODw'}))
    check(cpp.call('ack'))
    check(cpp.call('frame', frame=channel.encrypt({'type': 'sync', 'cap_token': info['wire']}, 'proof-2')))
    assert len(seqs) >= 5 and seqs == sorted(set(seqs))
    # Retransmission after confirmation/reconnect still reuses the handshake.
    repeated = cpp.call('frame', frame=opened.frame)
    assert repeated['signing_calls'] == 1 and repeated['frames'][0]['sig'] == accepted['sig']
    check(cpp.call('tick', ms=1000))
    fresh = BrowserOpen(info['wire'])
    next_open = cpp.call('frame', frame=fresh.frame)
    assert next_open['signing_calls'] == 2
    assert fresh.accept(next_open['frames'][0]).rx_seq == 0


def test_identity_signer_failure_creates_no_usable_channel(connected):
    cpp, info = connected
    opened = BrowserOpen(info['wire'])
    cpp.call('fail_signing', value=True)
    response = cpp.call('frame', frame=opened.frame)
    assert response['signing_calls'] == 1 and response['frames'] == []
    assert cpp.call('status')['frames'] == []
    assert cpp.call('event')['frames'] == []
    cpp.call('fail_signing', value=False)
    cpp.call('tick', ms=1000)
    response = cpp.call('frame', frame=opened.frame)
    assert response['signing_calls'] == 2
    channel = opened.accept(response['frames'][0])
    response = cpp.call('frame', frame=channel.encrypt({'type': 'sync', 'cap_token': info['wire']}, 'recovered'))
    assert response['signing_calls'] == 2
    assert channel.decrypt(encrypted_frames(response)[0])['type'] == 'status'
