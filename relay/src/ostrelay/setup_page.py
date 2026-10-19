"""Public ``/setup`` page for open (self-service) enrollment.

Served only when ``OSTRELAY_ENROLLMENT`` includes ``open``; otherwise 404.
It shows the relay's setup code, which carries the certificate pin. That pin
is trustworthy here because the browser has just verified this relay's HTTPS
certificate. The page holds no secret: open enrollment is bounded by
``OSTRELAY_MAX_ENROLLED_TIMERS`` and a per-IP enrollment rate limit.
"""

from __future__ import annotations

import html

from fastapi import APIRouter, HTTPException
from fastapi.responses import HTMLResponse, Response

from .admin import setup_code_for
from .admin_page import PAGE_CSP, PAGES_CSS, THEME_JS, page_head
from .page_strings import PAGE_STRINGS_JS
from . import tlsid
from .config import settings
from .owners import OPEN_ENROLLMENT_CODE
from .site_settings import landing_page_settings, open_enrollment_settings

router = APIRouter()

SETUP_JS = THEME_JS + r"""(() => {
  const button = document.getElementById('copy'), code = document.getElementById('code');
  if (!button) return;
  const t = (key) => window.SmartTimerI18n?.text(key) ?? key;
  button.addEventListener('click', async () => {
    try { await navigator.clipboard.writeText(code.textContent.trim()); button.textContent = t('Copied'); }
    catch (_) {
      document.getElementById('manual').open = true;
      getSelection().selectAllChildren(code);
      button.textContent = t('Select and copy the code');
    }
    setTimeout(() => { button.textContent = t('Copy setup code'); }, 3000);
  });
})();
"""

_PAGE = page_head("Connect your timer", "/setup/assets/pages.css", "Smart Timer relay", "{host}",
                  language=True, strings="/setup/assets/strings.js") + """
  <section class="setup">
    <h1>Connect your timer</h1>
    <p>Connect a Smart Timer V2 running OpenSmartTimer firmware. You can then create time-limited control links from your timer's page.</p>
    <div class="panel">
      <ol class="steps">
        <li><div><strong>Copy the setup code</strong>
          <span>It tells your timer where this relay is and how to recognize it.</span>
          <button id="copy" class="primary" type="button">Copy setup code</button>
          <details id="manual"><summary>Show the code</summary><div class="code-box"><output id="code">{code}</output></div></details>
        </div></li>
        <li><div><strong>Open your timer's page</strong>
          <span>With the timer unlocked and connected to Wi-Fi, open its local page and go to Device settings &rarr; Remote sharing. V1 timers do not support the relay.</span></div></li>
        <li><div><strong>Paste and connect</strong>
          <span>Paste the code and select Connect relay. Review both hosting addresses, then briefly press Reset on the unlocked timer within 60 seconds to approve. Wait for Connected before creating a share link.</span></div></li>
      </ol>
      <dl class="relay-facts">
        <div><dt>Relay</dt><dd>{host}</dd></div>
      </dl>
    </div>
    <aside class="trust-note"><strong>Review both hosting addresses</strong><p>The control page must be hosted independently of the relay. Its host must be trusted with the private share link. Anyone with the complete link can use its controls. Your timer still enforces its limits. Never rely on an internet connection for emergency release.</p></aside>
    <p class="foot">Experimental software, provided as is, without warranty.</p>
  </section>
</main>
<script src="/setup/assets/setup.js" defer></script>
</body>
</html>
"""
_HEADERS = {"Cache-Control": "no-store", "Content-Security-Policy": PAGE_CSP, "Referrer-Policy": "no-referrer"}


@router.get("/setup", include_in_schema=False)
@router.get("/setup/", include_in_schema=False)
def setup_page() -> HTMLResponse:
    cfg = settings()
    if not open_enrollment_settings()["open_enrollment_enabled"] or not tlsid.current_pin():
        raise HTTPException(404, "not_found")
    host = cfg.public_url.split("://", 1)[-1]
    # The timer page shows the same relay address once the code is pasted.
    page = (_PAGE.replace("{host}", html.escape(host))
            .replace("{code}", html.escape(setup_code_for(OPEN_ENROLLMENT_CODE))))
    return HTMLResponse(page, headers=_HEADERS)


@router.get("/setup/assets/setup.js", include_in_schema=False)
def setup_script() -> Response:
    return Response(SETUP_JS, media_type="text/javascript", headers={"Cache-Control": "no-store"})


@router.get("/setup/assets/pages.css", include_in_schema=False)
def setup_styles() -> Response:
    return Response(PAGES_CSS, media_type="text/css", headers={"Cache-Control": "no-store"})


@router.get("/setup/assets/strings.js", include_in_schema=False)
def setup_strings() -> Response:
    return Response(PAGE_STRINGS_JS, media_type="text/javascript", headers={"Cache-Control": "no-store"})


_HOME = page_head("Smart Timer relay", "/setup/assets/pages.css", "Smart Timer relay", "{host}",
                  language=True, strings="/setup/assets/strings.js") + """
  <section class="setup">
    {preview}
    <h1>Share control of your timer</h1>
    <p>This relay connects a Smart Timer V2 to someone its owner invites with a time-limited link.</p>
    <div class="panel home-card">
      <h2>Received a share link?</h2>
      <p>Open the complete link from the owner. It takes you straight to that timer's controls; you don't need an account here.</p>
      <p class="muted">Keep the link private. Anyone who has it can use its controls while it is active.</p>
    </div>
    {action}
    <p class="foot">Review the relay and independently hosted control-page addresses on your timer. Keep complete share links private. An internet connection is not an emergency release method.</p>
    <p class="foot">Experimental software, provided as is, without warranty.</p>
  </section>
</main>
<script src="/setup/assets/setup.js" defer></script>
</body>
</html>
"""


@router.get("/", include_in_schema=False)
def home_page() -> HTMLResponse:
    """Public landing page: says what this address is, reveals nothing else."""
    if not landing_page_settings()["landing_page_enabled"]:
        raise HTTPException(404, "not_found", headers={"Cache-Control": "no-store"})
    return render_home_page()


@router.get("/admin/landing-preview", include_in_schema=False)
def home_preview() -> HTMLResponse:
    """Non-secret preview on the private admin surface, even when / is off.

    Like the admin sign-in page, this does not require a bearer token. It
    contains no credentials or inventory; settings writes remain authenticated.
    Caddy deliberately never publishes /admin paths.
    """
    if not settings().admin_enabled:
        raise HTTPException(404, "not_found")
    return render_home_page(preview=True)


def render_home_page(*, preview: bool = False) -> HTMLResponse:
    cfg = settings()
    host = cfg.public_url.split("://", 1)[-1]
    action = ('<div class="panel home-card"><h2>Connect your own timer</h2>'
              '<p>Sign in to get your setup code and manage the timers you connected.</p>'
              '<a class="primary button-link" href="/owner">Sign in</a></div>'
              if cfg.oidc_enabled() else
              '<div class="panel home-card"><h2>Connect your own timer</h2>'
              '<p>Public enrollment is available for V2 timers running OpenSmartTimer firmware.</p>'
              f'<a class="primary button-link" href="{html.escape(cfg.public_url, quote=True)}/setup">Connect your timer</a></div>'
              if open_enrollment_settings()["open_enrollment_enabled"] and tlsid.current_pin() else
              '<div class="panel home-card"><h2>Connect your own timer</h2>'
              '<p>Public setup is unavailable. Ask this relay\'s operator for a setup code.</p></div>')
    note = ('<div class="preview-note">Admin preview · The public page may be hidden. '
            '<a href="/admin/">Back to admin</a></div>' if preview else "")
    page = (_HOME.replace("{host}", html.escape(host)).replace("{action}", action)
            .replace("{preview}", note))
    return HTMLResponse(page, headers=_HEADERS)
