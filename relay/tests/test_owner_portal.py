"""Owner self-service portal (/owner): OIDC sign-in via authlib, owner
sessions, setup codes and per-owner timer management. The identity provider
is replaced by a stand-in client; token validation itself is authlib's."""

from __future__ import annotations

import pytest

ENROLL = {"hardware_model": "MagBound Smart Timer V2", "chip_family": "ESP32-C3",
          "build_id": "aabbccddeeff00112233aabb"}
ISSUER = "https://id.example.com"


class FakeProvider:
    def __init__(self, claims=None, issuer=ISSUER, userinfo=None, fail=None):
        self.claims = claims or {"sub": "user-1", "email": "alex@example.com", "preferred_username": "alex"}
        self.issuer, self.userinfo_claims, self.fail = issuer, userinfo, fail
        self.server_metadata = {}
        self.options = None

    async def load_server_metadata(self):
        self.server_metadata = {"issuer": self.issuer, "id_token_signing_alg_values_supported": ["RS256", "HS256"]}
        return self.server_metadata

    async def authorize_redirect(self, request, redirect_uri):
        from starlette.responses import RedirectResponse
        self.redirect_uri = redirect_uri
        return RedirectResponse(f"{ISSUER}/authorize?client_id=relay", status_code=302)

    async def authorize_access_token(self, request, **options):
        self.options = options
        if self.fail:
            from authlib.integrations.base_client.errors import OAuthError
            raise OAuthError(error=self.fail)
        return {"access_token": "at", "id_token": "contract-stub-not-a-real-token", "userinfo": dict(self.claims)}

    async def userinfo(self, token):
        return self.userinfo_claims or {}


@pytest.fixture()
def portal_app(fresh_db, fresh_registry, monkeypatch):
    from ostrelay import config, enroll, portal
    for key, value in {"oidc_issuer": ISSUER, "oidc_client_id": "relay", "oidc_client_secret": "s" * 32,
                       "public_url": "https://testserver"}.items():
        monkeypatch.setattr(config._settings, key, value)
    # An HTTPS relay enrolls only once it knows its TLS key. The owner page
    # keeps showing raw owner codes (no key for it to embed).
    monkeypatch.setattr(enroll, "_spki_pin", lambda: "A" * 43)
    portal.reset_for_tests()
    provider = FakeProvider()
    monkeypatch.setattr(portal, "oauth_client", lambda: provider)
    from ostrelay.main import create_app
    from fastapi.testclient import TestClient
    with TestClient(create_app(), base_url="https://testserver", follow_redirects=False) as c:
        yield c, provider


def sign_in(client):
    r = client.get("/owner/callback?code=x&state=y")
    assert r.status_code == 302 and r.headers["location"] == "/owner", r.headers.get("location")
    return r


def test_portal_is_absent_without_oidc(client):
    for path in ("/owner", "/owner/login", "/owner/callback", "/owner/api/me", "/owner/assets/portal.js"):
        assert client.get(path).status_code == 404
    assert client.post("/owner/api/code").status_code == 404


def test_oidc_settings_are_validated():
    from ostrelay import config
    base = dict(_env_file=None, oidc_issuer=ISSUER, oidc_client_id="relay", oidc_client_secret="s")
    assert config.Settings(**base).oidc_enabled()
    for bad in ({"oidc_client_secret": ""}, {"oidc_scopes": "profile email"}, {"enrollment": "open"},
                {"oidc_signing_alg": "HS256"}, {"oidc_signing_alg": "none"}):
        with pytest.raises(ValueError):
            config.Settings(**{**base, **bad})


def test_sign_in_uses_pkce_pinned_issuer_and_audience(portal_app):
    client, provider = portal_app
    r = client.get("/owner/login")
    assert r.status_code == 302 and r.headers["location"].startswith(ISSUER)
    assert provider.redirect_uri == "https://testserver/owner/callback"
    # The advertised HS256 is narrowed to the configured asymmetric algorithm.
    assert provider.server_metadata["id_token_signing_alg_values_supported"] == ["RS256"]
    sign_in(client)
    assert provider.options["claims_options"] == {"iss": {"values": [ISSUER]}, "aud": {"values": ["relay"]}}
    from ostrelay import portal
    assert portal.oauth_client is not None


def test_owner_cookie_attributes(portal_app):
    client, _ = portal_app
    cookie = sign_in(client).headers["set-cookie"]
    assert cookie.startswith("__Host-ostrelay-owner=")
    for attribute in ("HttpOnly", "Secure", "Path=/", "SameSite=lax"):
        assert attribute.lower() in cookie.lower()
    assert "domain" not in cookie.lower()


def test_owner_gets_a_code_enrolls_and_manages_own_timers(portal_app, fresh_db):
    client, _ = portal_app
    assert client.get("/owner/api/me").status_code == 401
    sign_in(client)
    me = client.get("/owner/api/me").json()
    assert me["name"] == "alex" and me["email"] == "alex@example.com" and me["timers"] == []
    code = client.post("/owner/api/code").json()["setup_code"]
    assert code.startswith("o1.")  # no SPKI pin in the test relay, so the raw owner code
    timer = client.post("/api/v1/enroll", json={"enrollment_code": code, **ENROLL}).json()["timer_id"]
    assert [t["timer_id"] for t in client.get("/owner/api/me").json()["timers"]] == [timer]
    newer = client.post("/owner/api/code").json()["setup_code"]
    assert client.post("/api/v1/enroll", json={"enrollment_code": code, **ENROLL}).status_code == 403
    assert client.post("/api/v1/enroll", json={"enrollment_code": newer, **ENROLL}).status_code == 200
    assert client.delete(f"/owner/api/timers/{timer}").json() == {"removed": True}
    assert len(client.get("/owner/api/me").json()["timers"]) == 1


def test_owner_cannot_touch_another_owners_timer(portal_app):
    client, _ = portal_app
    from ostrelay import owners
    other = owners.create_owner("sam")
    theirs = client.post("/api/v1/enroll", json={"enrollment_code": other, **ENROLL}).json()["timer_id"]
    sign_in(client)
    assert client.delete(f"/owner/api/timers/{theirs}").status_code == 404
    assert client.get("/owner/api/me").json()["timers"] == []


def test_same_identity_same_owner_and_name_collisions(portal_app, fresh_db):
    client, provider = portal_app
    from ostrelay import owners
    owners.create_owner("alex")  # an invited owner already uses the name
    sign_in(client)
    sign_in(client)
    listed = {o["name"]: o for o in owners.list_owners()}
    assert {"alex", "alex-2"} <= set(listed) and listed["alex-2"]["signs_in"] and not listed["alex"]["signs_in"]
    assert sum(o["signs_in"] for o in listed.values()) == 1  # a second sign-in reuses the owner
    provider.claims = {"sub": "user-2", "preferred_username": "<script>"}
    sign_in(client)
    assert "script" in {o["name"] for o in owners.list_owners()}  # markup stripped from the name


def test_group_allow_list_including_userinfo_fallback(portal_app, monkeypatch):
    client, provider = portal_app
    from ostrelay import config
    monkeypatch.setattr(config._settings, "oidc_allowed_groups", "timer-owners, admins")
    r = client.get("/owner/callback?code=x&state=y")
    assert r.headers["location"] == "/owner?error=not_allowed"
    provider.userinfo_claims = {"sub": "someone-else", "groups": ["timer-owners"]}  # wrong subject: ignored
    assert client.get("/owner/callback?code=x&state=y").headers["location"] == "/owner?error=not_allowed"
    provider.userinfo_claims = {"sub": "user-1", "groups": ["timer-owners"]}
    sign_in(client)
    provider.claims = {"sub": "user-3", "groups": ["admins"]}
    sign_in(client)


def test_revoked_owner_is_refused_and_sessions_end(portal_app):
    client, _ = portal_app
    from ostrelay import owners
    sign_in(client)
    name = client.get("/owner/api/me").json()["name"]
    owners.revoke_owner(name)
    assert client.get("/owner/api/me").json()["detail"] == "owner_revoked"
    assert client.post("/owner/api/code").status_code == 403
    assert client.get("/owner/callback?code=x&state=y").headers["location"] == "/owner?error=revoked"


def test_failed_or_mismatched_provider_responses(portal_app):
    client, provider = portal_app
    provider.fail = "access_denied"
    assert client.get("/owner/callback?code=x&state=y").headers["location"] == "/owner?error=sign_in_failed"
    provider.fail, provider.issuer = None, "https://evil.example.com"
    assert client.get("/owner/callback?code=x&state=y").headers["location"] == "/owner?error=sign_in_failed"
    assert client.get("/owner/login").headers["location"] == "/owner?error=provider_unavailable"
    provider.issuer, provider.claims = ISSUER, {"email": "no-subject@example.com"}
    assert client.get("/owner/callback?code=x&state=y").headers["location"] == "/owner?error=sign_in_failed"


def test_logout_and_expiry(portal_app, fresh_db):
    client, _ = portal_app
    sign_in(client)
    assert client.post("/owner/logout").json() == {"signed_out": True}
    assert client.get("/owner/api/me").status_code == 401
    sign_in(client)
    fresh_db.connect().execute("UPDATE owner_sessions SET expires_at = 0")
    assert client.get("/owner/api/me").status_code == 401


def test_cross_site_writes_are_refused(portal_app):
    client, _ = portal_app
    sign_in(client)
    evil = {"Origin": "https://evil.example"}
    assert client.post("/owner/api/code", headers=evil).json()["detail"] == "origin_rejected"
    assert client.post("/owner/logout", headers=evil).status_code == 403
    assert client.post("/owner/api/code", headers={"Origin": "https://testserver"}).status_code == 200


def test_portal_page_has_strict_csp_and_no_inline_script(portal_app):
    client, _ = portal_app
    page = client.get("/owner")
    assert page.status_code == 200 and "Sign in with" in page.text and "single sign-on" in page.text
    assert "unsafe-inline" not in page.headers["content-security-policy"] and "<script>" not in page.text
    assert client.get("/owner/assets/portal.js").status_code == 200
