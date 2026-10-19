"""Owner self-service portal at ``/owner`` (optional; OIDC sign-in).

Enabled only when ``OSTRELAY_OIDC_ISSUER`` is set. Owners sign in at the
operator's identity provider and then create their own setup code and manage
their own timers. Without OIDC the relay works as before: the operator issues
setup codes from the admin page (admin token) or the command line.

The OIDC exchange is authlib's standard client (authorization code + PKCE,
discovery, ID token signature/iss/aud/nonce checks); nothing here parses or
verifies tokens by hand. The provider's issuer and signing algorithm are
pinned to configuration, like the author's other deployment.
"""

from __future__ import annotations

import hashlib
import hmac
import logging
import secrets
import time
from collections import deque
from typing import Any

from authlib.integrations.base_client.errors import OAuthError
from authlib.integrations.starlette_client import OAuth
from fastapi import APIRouter, HTTPException, Request
from fastapi.responses import HTMLResponse, JSONResponse, RedirectResponse, Response

from . import db, owners, ws
from .admin import setup_code_for
from .admin_page import PAGE_CSP, PAGES_CSS, THEME_JS, page_head
from .config import settings

log = logging.getLogger(__name__)
router = APIRouter()

OWNER_COOKIE = "__Host-ostrelay-owner"
STATE_COOKIE = "__Host-ostrelay-oidc"  # authlib state/nonce/PKCE verifier, 10 minutes
_HEADERS = {"Cache-Control": "no-store", "Content-Security-Policy": PAGE_CSP, "Referrer-Policy": "no-referrer"}
_oauth: OAuth | None = None
_login_buckets: dict[str, deque[float]] = {}


def state_secret() -> str:
    """Signing key for the short-lived login-state cookie, derived from the
    relay's cookie secret (separate label, so it can never sign a session)."""
    return hmac.new(bytes.fromhex(settings().cookie_secret), b"ostrelay-oidc-state-v1", hashlib.sha256).hexdigest()


def oauth_client() -> Any:
    global _oauth
    if _oauth is None:
        cfg = settings()
        _oauth = OAuth()
        _oauth.register(
            "provider",
            client_id=cfg.oidc_client_id,
            client_secret=cfg.oidc_client_secret,
            server_metadata_url=f"{cfg.oidc_issuer}/.well-known/openid-configuration",
            client_kwargs={"scope": cfg.oidc_scopes, "code_challenge_method": "S256",
                           "token_endpoint_auth_method": "client_secret_post"},
        )
    return _oauth.create_client("provider")


def reset_for_tests() -> None:
    global _oauth
    _oauth = None
    _login_buckets.clear()


def _require_enabled() -> None:
    if not settings().oidc_enabled():
        raise HTTPException(404, "not_found")


def _redirect_uri() -> str:
    return f"{settings().public_url}/owner/callback"


async def _pin_metadata(client: Any) -> None:
    """Refuse a provider whose discovery document names another issuer, and
    narrow accepted ID-token algorithms to the configured one."""
    cfg = settings()
    metadata = await client.load_server_metadata()
    if str(metadata.get("issuer", "")).rstrip("/") != cfg.oidc_issuer:
        raise OAuthError(error="invalid_issuer", description="OIDC issuer mismatch")
    if cfg.oidc_signing_alg not in (metadata.get("id_token_signing_alg_values_supported") or ["RS256"]):
        raise OAuthError(error="invalid_alg", description="signing algorithm not offered")
    client.server_metadata["id_token_signing_alg_values_supported"] = [cfg.oidc_signing_alg]


def _rate_limited(request: Request) -> bool:
    now = time.monotonic()
    ip = request.client.host if request.client else "unknown"
    for key, bucket in list(_login_buckets.items()):
        while bucket and bucket[0] <= now - 60:
            bucket.popleft()
        if not bucket:
            del _login_buckets[key]
    bucket = _login_buckets.setdefault(ip, deque())
    if len(bucket) >= 20 or len(_login_buckets) > settings().max_rate_limit_keys:
        return True
    bucket.append(now)
    return False


def _groups(claims: dict[str, Any]) -> set[str]:
    value = claims.get(settings().oidc_groups_claim)
    if isinstance(value, str):
        return {value}
    if isinstance(value, list):
        return {str(v) for v in value if isinstance(v, (str, int))}
    return set()


# ---- sessions -------------------------------------------------------------

def _hash(raw: str) -> str:
    return hashlib.sha256(raw.encode("ascii")).hexdigest()


def _create_session(owner_id: str) -> str:
    raw = secrets.token_urlsafe(32)
    now = db.now_unix()
    with db.transaction() as tx:
        tx.execute("DELETE FROM owner_sessions WHERE expires_at <= ?", (now,))
        total = tx.execute("SELECT count(*) AS n FROM owner_sessions").fetchone()["n"]
        per_owner = tx.execute("SELECT count(*) AS n FROM owner_sessions WHERE owner_id=?", (owner_id,)).fetchone()["n"]
        if total >= settings().max_owner_sessions or per_owner >= settings().max_owner_sessions_per_owner:
            raise HTTPException(429, "owner_session_capacity")
        tx.execute("INSERT INTO owner_sessions (session_hash, owner_id, created_at, expires_at) VALUES (?,?,?,?)",
                   (_hash(raw), owner_id, now, now + settings().owner_session_ttl_s))
    return raw


def current_owner(request: Request) -> dict[str, Any]:
    """The signed-in, unrevoked owner, or 401."""
    _require_enabled()
    raw = request.cookies.get(OWNER_COOKIE, "")
    if not raw or len(raw) > 128 or not raw.isascii():
        raise HTTPException(401, "sign_in_required")
    row = db.connect().execute(
        "SELECT o.owner_id, o.name, o.email, o.revoked FROM owner_sessions s JOIN owners o ON o.owner_id = s.owner_id "
        "WHERE s.session_hash=? AND s.expires_at > ?", (_hash(raw), db.now_unix())).fetchone()
    if row is None:
        raise HTTPException(401, "sign_in_required")
    if row["revoked"]:
        raise HTTPException(403, "owner_revoked")
    return dict(row)


def _set_owner_cookie(response: Response, raw: str) -> None:
    response.set_cookie(OWNER_COOKIE, raw, max_age=settings().owner_session_ttl_s, path="/",
                        secure=True, httponly=True, samesite="lax")


# ---- sign-in flow -----------------------------------------------------------

@router.get("/owner/login", include_in_schema=False)
async def login(request: Request) -> Response:
    _require_enabled()
    if _rate_limited(request):
        return RedirectResponse("/owner?error=rate_limited", status_code=302)
    try:
        client = oauth_client()
        await _pin_metadata(client)
        return await client.authorize_redirect(request, _redirect_uri())
    except Exception:  # noqa: BLE001 - provider unreachable or misconfigured
        log.warning("ostrelay: OIDC sign-in could not start", exc_info=True)
        return RedirectResponse("/owner?error=provider_unavailable", status_code=302)


@router.get("/owner/callback", include_in_schema=False)
async def callback(request: Request) -> Response:
    _require_enabled()
    cfg = settings()
    if _rate_limited(request):
        return RedirectResponse("/owner?error=rate_limited", status_code=302)
    try:
        client = oauth_client()
        await _pin_metadata(client)
        token = await client.authorize_access_token(
            request, claims_options={"iss": {"values": [cfg.oidc_issuer]}, "aud": {"values": [cfg.oidc_client_id]}},
            leeway=60)
    except OAuthError as exc:
        log.info("ostrelay: OIDC callback refused: %s", exc.error)
        return RedirectResponse("/owner?error=sign_in_failed", status_code=302)
    except Exception:  # noqa: BLE001
        log.warning("ostrelay: OIDC callback failed", exc_info=True)
        return RedirectResponse("/owner?error=sign_in_failed", status_code=302)
    # Authlib only parses/validates an ID token when one is present. Never
    # substitute an unsigned token-endpoint userinfo object for OIDC identity.
    if not isinstance(token.get("id_token"), str) or not token["id_token"]:
        return RedirectResponse("/owner?error=sign_in_failed", status_code=302)
    claims = dict(token.get("userinfo") or {})
    # This browser flow always sends a nonce. Authlib supports providers opting
    # out via nonce_supported=false; that weaker policy is not allowed here.
    if (claims.get("nonce_supported") is False or not isinstance(claims.get("sub"), str)
            or not 1 <= len(claims["sub"]) <= 255):
        return RedirectResponse("/owner?error=sign_in_failed", status_code=302)
    allowed = cfg.oidc_allowed_group_set()
    if allowed and cfg.oidc_groups_claim not in claims:
        # Some providers send groups only from the userinfo endpoint.
        try:
            extra = dict(await client.userinfo(token=token))
            if extra.get("sub") == claims["sub"]:
                claims.setdefault(cfg.oidc_groups_claim, extra.get(cfg.oidc_groups_claim))
        except Exception:  # noqa: BLE001 - no groups means no access below
            log.info("ostrelay: OIDC userinfo unavailable for group check")
    if allowed and not (allowed & _groups(claims)):
        return RedirectResponse("/owner?error=not_allowed", status_code=302)
    email = str(claims.get("email") or "") if claims.get("email_verified", True) is not False else ""
    display = str(claims.get("preferred_username") or claims.get("name") or email.split("@")[0] or "owner")
    try:
        owner = owners.owner_for_sign_in(f"{cfg.oidc_issuer}|{claims['sub']}", email, display)
    except ValueError:
        return RedirectResponse("/owner?error=rate_limited", status_code=302)
    if owner["revoked"]:
        return RedirectResponse("/owner?error=revoked", status_code=302)
    response = RedirectResponse("/owner", status_code=302)
    try:
        raw = _create_session(owner["owner_id"])
    except HTTPException as exc:
        if exc.status_code != 429:
            raise
        return RedirectResponse("/owner?error=rate_limited", status_code=302)
    _set_owner_cookie(response, raw)
    return response


@router.post("/owner/logout", include_in_schema=False)
def logout(request: Request) -> Response:
    _require_enabled()
    raw = request.cookies.get(OWNER_COOKIE, "")
    if raw and len(raw) <= 128 and raw.isascii():
        with db.transaction() as tx:
            tx.execute("DELETE FROM owner_sessions WHERE session_hash=?", (_hash(raw),))
    response = JSONResponse({"signed_out": True})
    response.delete_cookie(OWNER_COOKIE, path="/", secure=True, httponly=True, samesite="lax")
    return response


# ---- owner API --------------------------------------------------------------

@router.get("/owner/api/me", include_in_schema=False)
def me(request: Request) -> dict[str, Any]:
    owner = current_owner(request)
    rows = db.connect().execute(
        "SELECT timer_id, label, hardware_model, created_at, last_seen_at FROM timers WHERE owner_id=? "
        "ORDER BY created_at DESC", (owner["owner_id"],)).fetchall()
    return {
        "name": owner["name"], "email": owner["email"],
        "relay": settings().public_url.split("://", 1)[-1],
        "max_timers": settings().max_timers_per_owner,
        "timers": [{"timer_id": r["timer_id"], "label": r["label"], "hardware_model": r["hardware_model"],
                    "created_at": r["created_at"], "last_seen_at": r["last_seen_at"],
                    "connected_now": ws.registry.get(r["timer_id"]) is not None} for r in rows],
    }


@router.post("/owner/api/code", include_in_schema=False)
def new_code(request: Request) -> dict[str, str]:
    """Issue this owner's setup code. It replaces any earlier one; timers
    already connected stay connected."""
    owner = current_owner(request)
    try:
        code = owners.rotate_owner_id(owner["owner_id"])
    except ValueError:
        raise HTTPException(403, "owner_revoked") from None
    return {"setup_code": setup_code_for(code) or code}


@router.delete("/owner/api/timers/{timer_id}", include_in_schema=False)
async def remove_timer(timer_id: str, request: Request) -> dict[str, bool]:
    owner = current_owner(request)
    with db.transaction() as tx:
        cur = tx.execute("DELETE FROM timers WHERE timer_id=? AND owner_id=?", (timer_id, owner["owner_id"]))
        if cur.rowcount == 0:
            raise HTTPException(404, "not_found")  # someone else's timer looks absent
    session = ws.registry.get(timer_id)
    if session is not None:
        await session.abort(4003, "enrollment_erased")
    return {"removed": True}


# ---- page ---------------------------------------------------------------------

PORTAL_JS = THEME_JS + r"""(() => {
  const $ = (id) => document.getElementById(id);
  const t = (key, values) => window.SmartTimerI18n?.text(key, values) || key;
  const el = (tag, cls, text) => { const n = document.createElement(tag); if (cls) n.className = cls; if (text !== undefined) n.textContent = text; return n; };
  const ago = (unix) => {
    if (!unix) return t('Never connected');
    const s = Math.max(0, Math.floor(Date.now() / 1000) - unix);
    if (s < 60) return t('Seen just now');
    if (s < 3600) return t('Seen {count} min ago', {count: Math.floor(s / 60)});
    if (s < 86400) return t('Seen {count} hours ago', {count: Math.floor(s / 3600)});
    return t('Seen {count} days ago', {count: Math.floor(s / 86400)});
  };
  const ERRORS = {
    sign_in_failed: 'Sign-in did not complete. Try again.',
    not_allowed: 'Your account is not allowed to use this relay. Ask its operator.',
    revoked: 'Your access to this relay was removed. Ask its operator.',
    provider_unavailable: 'The sign-in service is not reachable right now. Try again shortly.',
    rate_limited: 'Too many sign-in attempts. Wait a minute and try again.',
  };
  async function call(method, path) {
    const res = await fetch(path, { method, credentials: 'same-origin', cache: 'no-store' });
    if (res.status === 401) { showSignIn(); throw new Error('signed_out'); }
    let body = {}; try { body = await res.json(); } catch (_) { /* empty */ }
    if (!res.ok) throw new Error(body.detail === 'owner_revoked' ? t(ERRORS.revoked) : t('Something went wrong (HTTP {status}).', {status: res.status}));
    return body;
  }
  function showSignIn(message) {
    $('portal').classList.add('hidden'); $('signOut').classList.add('hidden');
    $('settingsMenu').open = false;
    $('signIn').classList.remove('hidden'); document.body.classList.add('signed-out');
    $('signInError').textContent = message ? t(message) : '';
  }
  let currentMe = null;
  function render(me) {
    currentMe = me;
    $('signIn').classList.add('hidden'); $('portal').classList.remove('hidden'); $('signOut').classList.remove('hidden');
    document.body.classList.remove('signed-out');
    $('who').textContent = me.email ? me.name + ' · ' + me.email : me.name;
    $('timerCount').textContent = t(me.timers.length === 1 ? '{count} timer of {limit}' : '{count} timers of {limit}', {count: me.timers.length, limit: me.max_timers});
    const list = $('timers'); list.replaceChildren();
    for (const timer of me.timers) {
      const row = el('li', 'timer-row');
      const text = el('div', 'timer-info');
      const label = el('strong', '', timer.label || timer.hardware_model);
      label.setAttribute('data-i18n-literal', '');
      const state = el('span', 'status ' + (timer.connected_now ? 'ok' : 'off'),
        timer.connected_now ? t('Online') : t('Offline') + ' · ' + ago(timer.last_seen_at));
      text.append(label, state);
      const remove = el('button', 'secondary remove-timer', t('Remove')); remove.type = 'button';
      remove.addEventListener('click', async () => {
        if (remove.dataset.armed !== '1') { remove.dataset.armed = '1'; remove.textContent = t('Confirm remove'); setTimeout(() => { remove.dataset.armed = ''; remove.textContent = t('Remove'); }, 4000); return; }
        try { await call('DELETE', '/owner/api/timers/' + encodeURIComponent(timer.timer_id)); load(); }
        catch (error) { if (error.message !== 'signed_out') $('message').textContent = error.message; }
      });
      row.append(text, remove); list.append(row);
    }
    if (!me.timers.length) list.append(el('li', 'empty', t('No timers yet. Create a setup code and paste it on your timer page.')));
  }
  async function load() {
    try { render(await call('GET', '/owner/api/me')); } catch (error) { if (error.message !== 'signed_out') $('message').textContent = error.message; }
  }
  $('createCode').addEventListener('click', async () => {
    $('createCode').disabled = true; $('message').textContent = '';
    try {
      const r = await call('POST', '/owner/api/code');
      $('codeValue').textContent = r.setup_code; $('codeBox').classList.remove('hidden');
      $('createCode').textContent = t('Create a new code');
    } catch (error) { if (error.message !== 'signed_out') $('message').textContent = error.message; }
    finally { $('createCode').disabled = false; }
  });
  $('copyCode').addEventListener('click', async () => {
    try { await navigator.clipboard.writeText($('codeValue').textContent); $('copyCode').textContent = t('Copied'); }
    catch (_) { getSelection().selectAllChildren($('codeValue')); $('copyCode').textContent = t('Press Ctrl+C to copy'); }
    setTimeout(() => { $('copyCode').textContent = t('Copy code'); }, 3000);
  });
  $('signOut').addEventListener('click', async () => {
    $('settingsMenu').open = false;
    try { await fetch('/owner/logout', { method: 'POST', credentials: 'same-origin' }); } catch (_) { /* local sign-out anyway */ }
    showSignIn('');
    currentMe = null;
  });
  window.addEventListener('smarttimer:languagechange', () => {
    if (currentMe && !$('portal').classList.contains('hidden')) render(currentMe);
  });
  const error = new URLSearchParams(location.search).get('error');
  if (error) { history.replaceState(null, '', '/owner'); showSignIn(ERRORS[error] || ERRORS.sign_in_failed); }
  else load();
})();
"""

PORTAL_CSS = PAGES_CSS + r"""
.owner-page .shell { width: min(640px, calc(100% - 32px)); }
.portal { width: 100%; margin: 32px auto 0; }
.portal h1 { margin: 0; font-size: 26px; letter-spacing: -.03em; }
.portal-heading { display: flex; flex-wrap: wrap; align-items: center; justify-content: space-between; gap: 12px; }
.portal > p { margin: 6px 0 0; color: var(--muted); font-size: 13px; overflow-wrap: anywhere; }
.portal .panel { margin-top: 18px; padding: 20px; }
.portal h2 { margin: 0; font-size: 15px; }
.portal .panel > p { margin: 4px 0 0; color: var(--muted); font-size: 13px; line-height: 1.55; }
.portal .panel > button { margin-top: 14px; }
.code-actions { display: flex; gap: 8px; margin-top: 10px; }
.timer-list { display: grid; gap: 0; margin: 12px 0 0; padding: 0; list-style: none; }
.timer-row { display: grid; grid-template-columns: minmax(0, 1fr) auto; gap: 12px; align-items: center; padding: 12px 0; border-top: 1px solid var(--line); }
.timer-info { min-width: 0; }
.timer-row strong { display: block; font-size: 13px; overflow-wrap: anywhere; }
.timer-row span { display: block; color: var(--muted); font-size: 12px; }
.timer-row .status { display: inline-flex; margin-top: 4px; white-space: normal; line-height: 1.45; }
.timer-row .status::before { flex: 0 0 7px; }
.timer-row button { min-height: 44px; padding: 10px 12px; font-size: 12px; }
.timer-row .remove-timer { color: var(--red); }
.section-row { display: flex; flex-wrap: wrap; align-items: baseline; justify-content: space-between; gap: 12px; }
.section-row span { color: var(--muted); font-size: 12px; }
.signin-card .button-link { display: flex; width: 100%; margin-top: 18px; }
.signin-error { margin: 12px 0 0; color: var(--red); font-size: 12px; text-align: center; }
.signin-error:empty { display: none; }
"""

PORTAL_PAGE = page_head("Your timers", "/owner/assets/portal.css", "Smart Timer relay", "@HOST@",
                        '<button id="signOut" class="secondary hidden" type="button">Sign out</button>',
                        body_class="signed-out owner-page", language=True) + """
  <section id="signIn" class="signin hidden">
    <div class="signin-card">
      <span class="brand-mark" aria-hidden="true">
        <svg viewBox="0 0 32 32"><path d="M10 14V9a6 6 0 0 1 12 0v5M8 14h16v13H8z"/><circle cx="16" cy="20" r="2"/></svg>
      </span>
      <h1>Connect your timers</h1>
      <p>Sign in to get your setup code and manage the timers you connected to this relay.</p>
      <a class="primary button-link" href="/owner/login"><span>Sign in with</span>&nbsp;<span data-i18n-literal>@PROVIDER@</span></a>
      <p id="signInError" class="signin-error" role="alert"></p>
      <p class="signin-foot">No account? Ask whoever runs this relay for a setup code.</p>
    </div>
  </section>

  <section id="portal" class="portal hidden">
    <div class="portal-heading"><h1>Your timers</h1></div>
    <p id="who" data-i18n-literal></p>
    <div class="panel">
      <h2>Setup code</h2>
      <p>Paste it on each timer under Device settings &rarr; Remote sharing. Creating a code replaces your previous one; timers already connected stay connected.</p>
      <button id="createCode" class="primary" type="button">Create setup code</button>
      <div id="codeBox" class="hidden">
        <div class="code-box"><output id="codeValue" data-i18n-literal></output></div>
        <div class="callout">Keep it private. Anyone with this code can connect a timer to your account.</div>
        <div class="code-actions"><button id="copyCode" class="secondary" type="button">Copy code</button></div>
      </div>
    </div>
    <div class="panel">
      <div class="section-row"><h2>Connected timers</h2><span id="timerCount"></span></div>
      <ul id="timers" class="timer-list"></ul>
    </div>
    <p id="message" class="foot" role="status"></p>
  </section>
</main>
<script src="/owner/assets/portal.js" defer></script>
</body>
</html>
"""


@router.get("/owner", include_in_schema=False)
@router.get("/owner/", include_in_schema=False)
def portal_page() -> HTMLResponse:
    _require_enabled()
    import html
    cfg = settings()
    page = (PORTAL_PAGE.replace("@HOST@", html.escape(cfg.public_url.split("://", 1)[-1]))
            .replace("@PROVIDER@", html.escape(cfg.oidc_provider_name)))
    return HTMLResponse(page, headers=_HEADERS)


@router.get("/owner/assets/portal.js", include_in_schema=False)
def portal_script() -> Response:
    _require_enabled()
    return Response(PORTAL_JS, media_type="text/javascript", headers={"Cache-Control": "no-store"})


@router.get("/owner/assets/portal.css", include_in_schema=False)
def portal_styles() -> Response:
    _require_enabled()
    return Response(PORTAL_CSS, media_type="text/css", headers={"Cache-Control": "no-store"})
