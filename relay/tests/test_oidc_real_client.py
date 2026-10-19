"""Real Authlib flow and RSA/JWKS validation against an isolated HTTP transport.

No real identity provider, credentials or network is contacted. This supplements
portal contract stubs; operator-specific live SSO qualification stays separate.
"""
import base64, hashlib, time
from urllib.parse import parse_qs, urlsplit
import httpx
import pytest
from joserfc import jwt
from joserfc.jwk import RSAKey

ISSUER='https://isolated-id.example'


@pytest.fixture()
def real_client(fresh_db, fresh_registry, monkeypatch):
    from ostrelay import config, portal
    from ostrelay.main import create_app
    from authlib.integrations.httpx_client import AsyncOAuth2Client
    from fastapi.testclient import TestClient
    for key,value in {'oidc_issuer':ISSUER,'oidc_client_id':'relay',
                      'oidc_client_secret':'isolated-client-secret','public_url':'https://testserver'}.items():
        monkeypatch.setattr(config._settings,key,value)
    key=RSAKey.generate_key(parameters={'kid':'test-key'})
    provider={'change':{},'queries':{},'tokens':0,'key':key,'omit_id_token':False}
    def transport(request):
        assert str(request.url).startswith(ISSUER+'/')
        path=request.url.path
        if path=='/.well-known/openid-configuration':
            return httpx.Response(200,json={'issuer':ISSUER,'authorization_endpoint':ISSUER+'/authorize',
                'token_endpoint':ISSUER+'/token','jwks_uri':ISSUER+'/jwks',
                'response_types_supported':['code'],'subject_types_supported':['public'],
                'id_token_signing_alg_values_supported':['RS256'],
                'code_challenge_methods_supported':['S256']})
        if path=='/jwks':return httpx.Response(200,json={'keys':[key.as_dict()]})
        if path=='/token':
            provider['tokens']+=1
            body=parse_qs(request.content.decode());verifier=body['code_verifier'][0]
            challenge=base64.urlsafe_b64encode(hashlib.sha256(verifier.encode()).digest()).decode().rstrip('=')
            assert challenge==provider['queries']['code_challenge'][0]
            assert body['client_id']==['relay'] and body['client_secret']==['isolated-client-secret']
            assert body['redirect_uri']==['https://testserver/owner/callback']
            now=int(time.time())
            claims={'iss':ISSUER,'aud':'relay','sub':'owner-1','iat':now,'exp':now+300,
                    'nonce':provider['queries']['nonce'][0],'preferred_username':'test-owner'}
            claims.update(provider['change'])
            value={'access_token':'isolated-access-token','token_type':'Bearer'}
            if provider['omit_id_token']:value['userinfo']={'sub':'unsigned-owner','preferred_username':'unsigned'}
            else:value['id_token']=jwt.encode({'alg':'RS256','kid':'test-key'},claims,provider['key'])
            return httpx.Response(200,json=value)
        raise AssertionError('Unexpected provider endpoint')
    original=AsyncOAuth2Client.__init__
    def initialize(self,*args,**kwargs):
        kwargs['transport']=httpx.MockTransport(transport);original(self,*args,**kwargs)
    monkeypatch.setattr(AsyncOAuth2Client,'__init__',initialize)
    portal.reset_for_tests()
    try:
        with TestClient(create_app(),base_url='https://testserver',follow_redirects=False) as client:
            response=client.get('/owner/login');assert response.status_code==302
            provider['queries']=parse_qs(urlsplit(response.headers['location']).query)
            assert provider['queries']['code_challenge_method']==['S256']
            assert provider['queries']['nonce'] and provider['queries']['state']
            yield client,provider
    finally:portal.reset_for_tests()


def callback(client,provider,state=None):
    return client.get('/owner/callback',params={'code':'isolated-code','state':state or provider['queries']['state'][0]})


def test_real_pkce_token_exchange_jwks_and_owner_admin_separation(real_client):
    client,provider=real_client
    assert callback(client,provider).headers['location']=='/owner'
    assert client.get('/owner/api/me').json()['name']=='test-owner'
    assert client.get('/admin/timers').status_code==401
    assert provider['tokens']==1
    # Callback state is one-use; a replay cannot perform another token exchange.
    assert callback(client,provider).headers['location']=='/owner?error=sign_in_failed'
    assert provider['tokens']==1


@pytest.mark.parametrize('change',[{'iss':'https://wrong.example'},{'aud':'wrong-client'},
                                 {'nonce':'wrong-nonce'},{'exp':1},{'sub':123},
                                 {'nonce':'wrong-nonce','nonce_supported':False}])
def test_real_id_token_claims_are_required(real_client,change):
    client,provider=real_client;provider['change']=change
    assert callback(client,provider).headers['location']=='/owner?error=sign_in_failed'
    assert client.get('/owner/api/me').status_code==401


def test_real_id_token_signature_is_required(real_client):
    client,provider=real_client;provider['key']=RSAKey.generate_key(parameters={'kid':'test-key'})
    assert callback(client,provider).headers['location']=='/owner?error=sign_in_failed'
    assert client.get('/owner/api/me').status_code==401


def test_unsigned_token_endpoint_userinfo_cannot_create_an_owner(real_client):
    client,provider=real_client;provider['omit_id_token']=True
    assert callback(client,provider).headers['location']=='/owner?error=sign_in_failed'
    assert client.get('/owner/api/me').status_code==401


def test_wrong_state_never_exchanges_code(real_client):
    client,provider=real_client
    assert callback(client,provider,'wrong-state').headers['location']=='/owner?error=sign_in_failed'
    assert provider['tokens']==0
