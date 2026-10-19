"""Operator pages: the admin page (admin port) and shared page assets.

Both relay pages load the timer's own stylesheet (``/assets/timer-ui.css``,
the copy the partner page uses) for tokens, type and controls, plus
``PAGES_CSS`` for page layout, tables and dialogs. Scripts are separate files
so the CSP needs no ``unsafe-inline``. Nothing here is secret: admin actions
need the token the operator pastes, kept only in that browser tab.
"""

PAGES_CSS = r"""/* Relay operator pages. Tokens and controls come from timer-ui.css. */
.relay-page { --radius: 15px; --row: rgba(255, 255, 255, .025); --card: rgba(255, 255, 255, .025); }
html[data-theme="light"] .relay-page { --row: rgba(var(--light-shadow-rgb, 65, 43, 74), .03); --card: rgba(255, 255, 255, .72); }
.relay-page .shell { width: min(1040px, calc(100% - 32px)); padding-bottom: 48px; }
.relay-page .topbar { flex-wrap: nowrap; gap: 12px; }
.relay-page .brand { flex-basis: 0; }
.relay-page button:not(.theme-switch):not(.language-switch) { min-height: 44px; padding: 12px 15px; border-radius: 11px; font-size: 14px; font-weight: 750; }
.relay-page .danger-button { color: var(--red); border: 1px solid var(--red); background: transparent; }
.relay-page .ghost { border: 1px solid var(--line); color: var(--text); background: rgba(255, 255, 255, .045); }
.relay-page .ghost.danger { border-color: rgba(255, 107, 129, .38); color: var(--red); }
.relay-page .secret-field .ghost { border: 0; color: var(--muted); background: transparent; }
.topbar-actions .ghost { min-height: 36px; }
.settings-menu .settings-colors { margin-top: 18px; }
.settings-menu .settings-colors > span { color: var(--muted); font-size: 12px; }
.settings-menu .palette-options { margin-top: 8px; }
.settings-menu .settings-actions { display: grid; gap: 8px; margin-top: 16px; border-top: 1px solid var(--line); padding-top: 16px; }
.settings-menu .settings-actions:has(> .hidden:only-child), .settings-menu .settings-actions:empty { display: none; }
.settings-menu .settings-actions button { width: 100%; }
@media (max-width: 430px) {
  .relay-page .topbar { gap: 8px; }
  .relay-page .topbar-actions { gap: 6px; }
  .relay-page .topbar .brand-mark { display: none; }
  .relay-page .topbar .brand-copy strong { font-size: 15px; }
  .relay-page .topbar .theme-switch, .relay-page .topbar .language-switch { width: 72px; min-width: 72px; }
}
/* Hostnames and plain labels: never force the timer UI's uppercase eyebrow style. */
.relay-page .brand-copy small { color: var(--muted); font-size: 12px; font-weight: 500; letter-spacing: 0; text-transform: none; }
.relay-page .brand-mark { flex-shrink: 0; }
.relay-page .brand-copy small { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.relay-page a:focus-visible { outline: 2px solid var(--purple-bright); outline-offset: 3px; }
.muted { color: var(--muted); }
.mono { font-family: ui-monospace, SFMono-Regular, Menlo, monospace; font-size: 12px; }

/* Page header and summary */
.page-header { display: flex; align-items: flex-end; justify-content: space-between; gap: 16px; margin: 32px 0 20px; }
.page-header h1 { margin: 0; font-size: 26px; letter-spacing: -.03em; overflow-wrap: anywhere; }
.page-header p { margin: 4px 0 0; color: var(--muted); font-size: 13px; }
.page-actions { display: flex; flex-wrap: wrap; gap: 8px; }
.summary { display: grid; grid-template-columns: repeat(4, minmax(0, 1fr)); gap: 12px; margin: 0 0 28px; }
.summary > div { display: block; border: 1px solid var(--line); border-radius: var(--radius); padding: 14px 16px; background: var(--card); }
.summary dt { color: var(--muted); font-size: 12px; }
.summary dd { max-width: none; overflow: visible; margin: 6px 0 0; text-align: left; white-space: normal; font-size: 22px; font-weight: 750; letter-spacing: -.02em; font-variant-numeric: tabular-nums; }
.summary dd small { color: var(--muted); font-size: 13px; font-weight: 600; }

/* Sections and tables */
.section { margin-top: 14px; border: 1px solid var(--line); border-radius: var(--radius); padding: 18px; background: var(--card); }
.section-head { display: flex; align-items: flex-start; justify-content: space-between; gap: 12px; }
.section-head h2 { margin: 3px 0 0; font-size: 14px; }
.section-head p { margin: 8px 0 0; color: var(--muted); font-size: 12px; line-height: 1.55; }
.section-actions { display: flex; flex-wrap: wrap; justify-content: flex-end; gap: 8px; }
.section > .panel { overflow: visible; margin-top: 14px; border: 0; border-radius: 0; background: transparent; }
.section .table th { padding: 9px 12px 9px 0; border-top: 1px solid var(--line); background: transparent; }
.section .table td { padding: 12px 12px 12px 0; }
.section .table td.actions { padding-right: 0; }
.section .table td.actions button + button { margin-left: 8px; }
.section .settings-list > div { padding: 12px 0; }
.section .settings-list > div:first-child { border-top: 1px solid var(--line); }
.section .visibility-control { border-top: 1px solid var(--line); padding: 14px 0; }
.section .settings-list + .visibility-control { margin-top: 0; }
.section .panel-note { padding: 12px 0 0; }
.section .panel-links { padding: 0; }
.section .site-message { padding: 0 0 4px; }
.section .empty { border-top: 1px solid var(--line); }
.panel { overflow: hidden; border: 1px solid var(--line); border-radius: var(--radius); background: var(--card); }
.table { width: 100%; border-collapse: collapse; font-size: 13px; }
.table th { padding: 10px 16px; color: var(--muted); font-size: 12px; font-weight: 600; text-align: left; background: var(--row); }
.table td { padding: 12px 16px; border-top: 1px solid var(--line); vertical-align: middle; }
.table td.actions { width: 1%; white-space: nowrap; text-align: right; }
.table td.actions button { min-height: 44px; padding: 8px 10px; font-size: 13px; }
.cell-title { display: block; font-weight: 650; overflow-wrap: anywhere; }
.cell-sub { display: block; margin-top: 2px; color: var(--muted); font-size: 12px; overflow-wrap: anywhere; }
.status { display: inline-flex; align-items: center; gap: 7px; font-size: 12px; font-weight: 650; white-space: nowrap; }
.status::before { width: 7px; height: 7px; border-radius: 50%; background: var(--muted); content: ""; }
.status.ok { color: var(--green); } .status.ok::before { background: var(--green); }
.status.off { color: var(--muted); }
.status.bad { color: var(--red); } .status.bad::before { background: var(--red); }
.empty { padding: 28px 16px; color: var(--muted); font-size: 13px; text-align: center; }
.empty strong { display: block; margin-bottom: 4px; color: var(--text); font-size: 14px; }
.settings-list { margin: 0; }
.settings-list dd { max-width: 60%; white-space: normal; overflow: visible; overflow-wrap: anywhere; }
.settings-list > div { display: flex; align-items: center; justify-content: space-between; gap: 16px; padding: 13px 16px; border-top: 1px solid var(--line); }
.settings-list > div:first-child { border-top: 0; }
.settings-list dt { color: var(--text); font-size: 13px; font-weight: 600; }
.settings-list dt small { display: block; margin-top: 2px; color: var(--muted); font-size: 12px; font-weight: 400; }
.settings-list dd { margin: 0; font-size: 13px; text-align: right; }
.settings-list dd a { display: inline-flex; align-items: center; min-height: 44px; white-space: nowrap; }
.panel-note { margin: 0; padding: 12px 16px; border-top: 1px solid var(--line); color: var(--muted); font-size: 12px; }
.relay-page code { padding: 1px 5px; border-radius: 5px; background: var(--row); font-size: 12px; }
.relay-page a { color: var(--purple-bright); }

/* Sign in */
.signin { display: grid; min-height: calc(100vh - 150px); place-items: center; padding: 24px 0; }
.signin-card { width: min(100%, 380px); border: 1px solid var(--line); border-radius: 18px; padding: 28px; background: var(--surface); }
html[data-theme="light"] .signin-card { box-shadow: 0 24px 50px rgba(var(--light-shadow-rgb, 66, 41, 75), .09); }
.signin-card .brand-mark { margin: 0 auto 18px; }
.signin-card h1 { margin: 0; font-size: 20px; letter-spacing: -.02em; text-align: center; }
.signin-card > p { margin: 6px 0 6px; color: var(--muted); font-size: 13px; line-height: 1.55; text-align: center; }
.signin-card .field { margin-top: 18px; }
.signin-card .form-actions { margin-top: 16px; }
.signin-foot { margin: 18px 0 0; color: var(--muted); font-size: 12px; line-height: 1.5; text-align: center; }
.secret-field { position: relative; }
.secret-field input { padding-right: 70px; }
.secret-field button { position: absolute; top: 50%; right: 6px; min-height: 30px; padding: 4px 10px; transform: translateY(-50%); font-size: 12px; }
.field .error:empty { display: none; }

/* Fields */
.field { display: grid; gap: 6px; margin-top: 14px; }
.field label { font-size: 13px; font-weight: 600; }
.field input { width: 100%; min-height: 44px; border: 1px solid var(--line); border-radius: 10px; outline: 0; padding: 10px 12px; color: var(--text); background: var(--bg); font-size: 16px; }
.field .secret-field input { padding-right: 76px; }
.relay-page .secret-field button { min-height: 36px; padding: 6px 10px; }
.field input:focus { border-color: rgba(var(--accent-bright-rgb, 192, 132, 252), .55); box-shadow: 0 0 0 3px rgba(var(--accent-rgb, 168, 85, 247), .12); }
.field .hint { color: var(--muted); font-size: 12px; }
.field .error { min-height: 16px; color: var(--red); font-size: 12px; }
.form-actions { display: flex; justify-content: flex-end; gap: 8px; margin-top: 20px; }
.form-actions.full button { flex: 1; }

/* Dialogs */
dialog { width: min(calc(100% - 32px), 460px); border: 1px solid var(--line); border-radius: 16px; padding: 22px; color: var(--text); background: var(--surface-raised); box-shadow: 0 24px 70px rgba(0, 0, 0, .45); }
dialog::backdrop { background: rgba(5, 3, 8, .62); backdrop-filter: blur(2px); }
html[data-theme="light"] dialog::backdrop { background: rgba(36, 25, 42, .28); }
html[data-theme="light"] dialog { box-shadow: 0 24px 60px rgba(var(--light-shadow-rgb, 66, 41, 75), .16); }
dialog h2 { margin: 0; font-size: 18px; letter-spacing: -.02em; }
dialog > p, dialog .step > p { margin: 8px 0 0; color: var(--muted); font-size: 13px; line-height: 1.55; }
.code-box { position: relative; margin-top: 16px; border: 1px solid var(--line); border-radius: 10px; padding: 12px; background: var(--bg); }
.code-box output { display: block; max-height: 88px; overflow: auto; color: var(--text); font-family: ui-monospace, SFMono-Regular, Menlo, monospace; font-size: 12px; line-height: 1.5; word-break: break-all; user-select: all; }
.callout { margin-top: 14px; border-radius: 10px; padding: 10px 12px; color: var(--amber); background: rgba(247, 199, 107, .08); font-size: 12px; line-height: 1.5; }

/* Toast */
.toast { position: fixed; z-index: 40; left: 50%; bottom: 24px; transform: translateX(-50%); max-width: calc(100% - 32px); border: 1px solid var(--line); border-radius: 10px; padding: 10px 14px; color: var(--text); background: var(--surface-raised); box-shadow: 0 12px 40px rgba(0, 0, 0, .35); font-size: 13px; }
.toast.err { color: var(--red); }
.foot { margin-top: 36px; color: var(--muted); font-size: 12px; }

/* Setup page */
.setup { width: min(100%, 520px); margin: 7vh auto 0; }
.setup h1 { margin: 0; font-size: 28px; letter-spacing: -.035em; }
.setup > p { margin: 8px 0 0; color: var(--muted); font-size: 14px; line-height: 1.6; }
.setup .panel { margin-top: 24px; padding: 20px; }
.steps { display: grid; gap: 18px; margin: 0; padding: 0; list-style: none; counter-reset: step; }
.steps > li { display: grid; grid-template-columns: 28px minmax(0, 1fr); gap: 12px; counter-increment: step; }
.steps > li::before { display: grid; width: 28px; height: 28px; place-items: center; border-radius: 50%; color: var(--purple-bright); background: var(--purple-soft); content: counter(step); font-size: 13px; font-weight: 800; }
.steps strong { display: block; font-size: 14px; }
.steps span { display: block; margin-top: 2px; color: var(--muted); font-size: 13px; line-height: 1.5; }
.steps button { margin-top: 12px; }
.steps details { margin-top: 10px; font-size: 12px; }
.steps summary { min-height: 44px; padding: 12px 0; color: var(--muted); cursor: pointer; }
.relay-facts { margin: 18px 0 0; padding-top: 16px; border-top: 1px solid var(--line); }
.relay-facts > div { display: flex; justify-content: space-between; gap: 12px; border-top: 0; padding: 3px 0; font-size: 12px; }
.relay-facts dt { color: var(--muted); } .relay-facts dd { margin: 0; overflow-wrap: anywhere; text-align: right; }
.setup .foot { line-height: 1.6; }
.home-card h2 { margin: 0; font-size: 17px; }
.home-card p { margin: 8px 0 0; color: var(--muted); font-size: 14px; line-height: 1.6; }
.button-link { display: inline-flex; align-items: center; justify-content: center; min-height: 44px; margin-top: 14px; border-radius: 10px; padding: 10px 16px; color: #fff; font-size: 14px; font-weight: 700; text-decoration: none; }
.relay-page a.button-link { color: #fff; }
.trust-note { margin-top: 24px; border-left: 3px solid var(--purple-bright); padding: 2px 0 2px 16px; font-size: 14px; }
.trust-note p { margin: 6px 0 0; color: var(--muted); font-size: 13px; line-height: 1.65; }
.preview-note { margin-bottom: 20px; border: 1px solid var(--line-strong); border-radius: 10px; padding: 12px; color: var(--muted); font-size: 13px; line-height: 1.6; }
.preview-note a { display: block; }
.visibility-control { display: flex; align-items: center; justify-content: space-between; gap: 16px; padding: 16px; }
.visibility-control label { display: grid; gap: 4px; font-size: 14px; font-weight: 650; cursor: pointer; }
.visibility-control label small { color: var(--muted); font-size: 13px; line-height: 1.5; font-weight: 400; }
.relay-page button.visibility-toggle:not(.theme-switch):not(.language-switch) { position: relative; flex: 0 0 46px; width: 46px; min-width: 46px; height: 26px; min-height: 26px; border: 0; border-radius: 99px; padding: 3px; background: var(--inactive-bg, #322b38); transition: background .2s ease; }
/* Keeps a 44px touch target around the compact track. */
.visibility-toggle::after { position: absolute; inset: -9px 0; content: ""; }
.visibility-toggle::before { display: block; width: 20px; height: 20px; border-radius: 50%; background: var(--inactive-text, #938a99); box-shadow: 0 2px 5px rgba(0, 0, 0, .3); content: ""; transition: transform .2s ease, background .2s ease; }
html[data-theme="light"] .relay-page button.visibility-toggle:not(.theme-switch):not(.language-switch) { background: rgba(var(--light-shadow-rgb, 65, 43, 74), .2); }
html[data-theme="light"] .visibility-toggle::before { background: #fff; }
html .relay-page button.visibility-toggle[aria-checked="true"]:not(.theme-switch):not(.language-switch) { background: var(--purple-deep); }
html .visibility-toggle[aria-checked="true"]::before { transform: translateX(20px); background: #fff; }
.panel-links { display: flex; flex-wrap: wrap; gap: 8px 20px; padding: 0 16px 12px; }
.panel-links a { display: inline-flex; align-items: center; min-height: 44px; font-size: 13px; }
.site-message { margin: 0; padding: 0 16px 16px; color: var(--muted); font-size: 13px; line-height: 1.5; }
.site-message.err { color: var(--red); }

@media (max-width: 760px) {
  .summary { grid-template-columns: repeat(2, minmax(0, 1fr)); }
  .page-header { flex-direction: column; align-items: stretch; }
  .page-actions button { flex: 1; }
  .table thead { display: none; }
  .table tr { display: grid; grid-template-columns: minmax(0, 1fr) auto; gap: 4px 12px; padding: 12px 16px; border-top: 1px solid var(--line); }
  .section .table tr { padding: 12px 0; }
  .section .table td, .section .table td.actions { padding: 0; }
  .table tbody tr:first-child { border-top: 0; }
  .table td { padding: 0; border: 0; }
  .table td.actions { grid-column: 1 / -1; text-align: left; }
  .table td[data-optional] { display: none; }
  .table td[data-label]::before { color: var(--muted); content: attr(data-label) " "; }
  .table td.empty { grid-column: 1 / -1; padding: 16px 0; }
  .table td.actions { width: auto; white-space: normal; }
  .relay-page .topbar-actions { gap: 6px; }
  .section-head { flex-wrap: wrap; }
  .settings-list > div { align-items: flex-start; }
  .setup { margin-top: 32px; }
}
@media (prefers-reduced-motion: reduce) { *, *::before, *::after { transition: none !important; } }
"""

THEME_JS = r"""(() => {
  const button = document.getElementById('themeButton');
  const apply = (theme) => {
    document.documentElement.dataset.theme = theme;
    button.dataset.theme = theme;
    button.setAttribute('aria-checked', String(theme === 'dark'));
    const label = theme === 'dark' ? 'Switch to light mode' : 'Switch to dark mode';
    button.setAttribute('aria-label', window.SmartTimerI18n?.text(label) || label);
    const gear = document.getElementById('settingsOpen');
    if (gear) gear.title = window.SmartTimerI18n?.text('Settings') || 'Settings';
    const meta = document.querySelector('meta[name="theme-color"]');
    if (meta) meta.content = theme === 'dark' ? '#09070d' : '#f7f3fa';
    try { localStorage.setItem('ost-relay-theme', theme); } catch (_) { /* Optional preference. */ }
  };
  // Follow the system setting until the viewer picks a theme.
  let theme = matchMedia('(prefers-color-scheme: light)').matches ? 'light' : 'dark';
  try {
    const saved = localStorage.getItem('ost-relay-theme');
    if (saved === 'light' || saved === 'dark') theme = saved;
  } catch (_) { /* System default. */ }
  apply(theme);
  button.addEventListener('click', () => apply(document.documentElement.dataset.theme === 'dark' ? 'light' : 'dark'));
  document.getElementById('languageButton')?.addEventListener('click', () => window.SmartTimerI18n?.toggleLanguage());
  window.addEventListener('smarttimer:languagechange', () => apply(document.documentElement.dataset.theme));
})();
"""

_THEME_BUTTON = """<button id="themeButton" class="theme-switch" type="button" role="switch" aria-checked="true" aria-label="Switch to light mode" data-theme="dark">
        <svg class="theme-sun" viewBox="0 0 24 24" aria-hidden="true"><circle cx="12" cy="12" r="3.4"/><path d="M12 2.5v2M12 19.5v2M2.5 12h2M19.5 12h2M5.3 5.3l1.4 1.4M17.3 17.3l1.4 1.4M18.7 5.3l-1.4 1.4M6.7 17.3l-1.4 1.4"/></svg>
        <svg class="theme-moon" viewBox="0 0 24 24" aria-hidden="true"><path d="M19.4 15.2A7.8 7.8 0 0 1 8.8 4.6 8.1 8.1 0 1 0 19.4 15.2Z"/></svg>
      </button>"""

_LANGUAGE_BUTTON = """<button id="languageButton" class="language-switch" type="button" role="switch" aria-checked="false" aria-label="Switch to Simplified Chinese" data-language="en">
        <span class="language-option-en">EN</span><span class="language-option-zh">中文</span>
      </button>"""

PAGE_HEAD = """<!doctype html>
<html lang="en" data-theme="dark">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1, viewport-fit=cover">
<meta name="theme-color" content="#0b0710">
<meta name="referrer" content="no-referrer">
<title>@TITLE@</title>
<link rel="stylesheet" href="/assets/timer-ui.css">
<link rel="stylesheet" href="@CSS@">
<link rel="icon" type="image/svg+xml" href="/assets/icon.svg">
<script src="/assets/appearance.js" defer></script>
@STRINGS_SCRIPT@@LANGUAGE_SCRIPT@
</head>
<body class="relay-page@BODYCLASS@">
<main class="shell">
  <header class="topbar">
    <div class="brand">
      <span class="brand-mark" aria-hidden="true">
        <svg viewBox="0 0 32 32"><path d="M10 14V9a6 6 0 0 1 12 0v5M8 14h16v13H8z"/><circle cx="16" cy="20" r="2"/></svg>
      </span>
      <span class="brand-copy"><strong>@BRAND@</strong><small id="brandSubtitle">@SUBTITLE@</small></span>
    </div>
    <div class="topbar-actions">@LANGUAGE_BUTTON@
""" + _THEME_BUTTON + """
<details id="settingsMenu" class="appearance-menu settings-menu">
  <summary id="settingsOpen" class="icon-button" aria-label="Settings" title="Settings"><svg viewBox="0 0 24 24" aria-hidden="true"><path d="M12 15.2a3.2 3.2 0 1 0 0-6.4 3.2 3.2 0 0 0 0 6.4Z"/><path d="M19.4 15a1.7 1.7 0 0 0 .3 1.9l.1.1-2.8 2.8-.1-.1a1.7 1.7 0 0 0-1.9-.3 1.7 1.7 0 0 0-1 1.6v.2h-4V21a1.7 1.7 0 0 0-1-1.6 1.7 1.7 0 0 0-1.9.3l-.1.1L4.2 17l.1-.1a1.7 1.7 0 0 0 .3-1.9A1.7 1.7 0 0 0 3 14H2.8v-4H3a1.7 1.7 0 0 0 1.6-1 1.7 1.7 0 0 0-.3-1.9L4.2 7 7 4.2l.1.1a1.7 1.7 0 0 0 1.9.3A1.7 1.7 0 0 0 10 3v-.2h4V3a1.7 1.7 0 0 0 1 1.6 1.7 1.7 0 0 0 1.9-.3l.1-.1L19.8 7l-.1.1a1.7 1.7 0 0 0-.3 1.9 1.7 1.7 0 0 0 1.6 1h.2v4H21a1.7 1.7 0 0 0-1.6 1Z"/></svg></summary>
  <section class="appearance-section" aria-label="Settings">
          <h3>Settings</h3>
          <div class="settings-colors"><span>Color theme</span>
          <div class="palette-options" role="group" aria-label="Color theme">
            <button type="button" data-palette-choice="purple" aria-pressed="true">Purple</button>
            <button type="button" data-palette-choice="slate" aria-pressed="false">Slate</button>
            <button type="button" data-palette-choice="teal" aria-pressed="false">Teal</button>
          </div>
          </div>
          <p class="helper">Saved in this browser only.</p>
          <div class="settings-actions">@ACTIONS@</div>
        </section>
</details>
    </div>
  </header>
"""


def page_head(title: str, css: str, brand: str, subtitle: str, actions: str = "", body_class: str = "", *,
              language: bool = False, strings: str = "") -> str:
    """``strings`` is a script with page-only translations; it must run before i18n.js."""
    return (PAGE_HEAD.replace("@TITLE@", title).replace("@CSS@", css).replace("@BRAND@", brand)
            .replace("@SUBTITLE@", subtitle).replace("@ACTIONS@", actions)
            .replace("@STRINGS_SCRIPT@", f'<script src="{strings}" defer></script>\n' if language and strings else "")
            .replace("@LANGUAGE_SCRIPT@", '<script src="/assets/i18n.js" defer></script>' if language else "")
            .replace("@LANGUAGE_BUTTON@", _LANGUAGE_BUTTON if language else "")
            .replace("@BODYCLASS@", f" {body_class}" if body_class else ""))


PAGE_CSP = ("default-src 'none'; script-src 'self'; style-src 'self'; img-src 'self'; "
            "connect-src 'self'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'")

ADMIN_PAGE = page_head("Smart Timer · Relay admin", "/admin/assets/pages.css", "Smart Timer", "Relay admin",
                       '<button id="signOut" class="secondary hidden" type="button">Sign out</button>',
                       body_class="signed-out", language=True, strings="/admin/assets/strings.js") + """
  <section id="signIn" class="signin">
    <form id="signInForm" class="signin-card" novalidate>
      <span class="brand-mark" aria-hidden="true">
        <svg viewBox="0 0 32 32"><path d="M10 14V9a6 6 0 0 1 12 0v5M8 14h16v13H8z"/><circle cx="16" cy="20" r="2"/></svg>
      </span>
      <h1>Sign in to relay admin</h1>
      <p id="signInHost">Manage owners, setup codes and connected timers.</p>
      <div class="field">
        <label for="token">Admin token</label>
        <div class="secret-field">
          <input id="token" type="password" autocomplete="current-password" spellcheck="false" required>
          <button id="tokenToggle" class="ghost" type="button" aria-controls="token" aria-pressed="false">Show</button>
        </div>
        <span id="signInError" class="error" role="alert"></span>
      </div>
      <div class="form-actions full"><button id="signInButton" class="primary" type="submit">Sign in</button></div>
      <p class="signin-foot">Use the <code>OSTRELAY_ADMIN_TOKEN</code> configured on your relay. It is kept in this browser tab until you sign out or close the tab.</p>
    </form>
  </section>

  <div id="workspace" class="hidden">
    <div class="page-header">
      <div>
        <h1 id="relayHost">Relay</h1>
        <p id="relaySummary">Loading…</p>
      </div>
      <div class="page-actions">
        <button id="oneTimeButton" class="secondary" type="button">One-time code</button>
        <button id="inviteButton" class="primary" type="button">Invite owner</button>
      </div>
    </div>

    <dl class="summary">
      <div><dt>Timers</dt><dd id="statTimers">–</dd></div>
      <div><dt>Connected now</dt><dd id="statConnected">–</dd></div>
      <div><dt>Owners</dt><dd id="statOwners">–</dd></div>
      <div><dt>Public enrollment</dt><dd id="statOpen">–</dd></div>
    </dl>

    <section class="section" aria-labelledby="timersTitle">
      <div class="section-head">
        <div><span class="eyebrow">Devices</span><h2 id="timersTitle">Timers</h2><p>Timers enrolled with this relay.</p></div>
        <div class="section-actions"><button id="refresh" class="ghost" type="button">Refresh</button><button id="clearTimers" class="ghost danger" type="button" disabled>Clear all timers</button></div>
      </div>
      <div class="panel">
        <table class="table">
          <thead><tr><th>Timer</th><th>Owner</th><th>Status</th><th>Last seen</th><th><span class="visually-hidden">Actions</span></th></tr></thead>
          <tbody id="timers"></tbody>
        </table>
      </div>
    </section>

    <section class="section" aria-labelledby="ownersTitle">
      <div class="section-head">
        <div><span class="eyebrow">People</span><h2 id="ownersTitle">Owners</h2><p>Each owner can reuse their setup code, up to the per-owner timer limit.</p></div>
      </div>
      <div class="panel">
        <table class="table">
          <thead><tr><th>Name</th><th>Timers</th><th>Added</th><th><span class="visually-hidden">Actions</span></th></tr></thead>
          <tbody id="owners"></tbody>
        </table>
      </div>
    </section>

    <section class="section" aria-labelledby="enrollmentTitle">
      <div class="section-head">
        <div><span class="eyebrow">Policy</span><h2 id="enrollmentTitle">Timer enrollment</h2><p>Controls who can add timers, not who can use a share link. Configured with <code>OSTRELAY_ENROLLMENT</code>.</p></div>
      </div>
      <div class="panel">
        <dl class="settings-list">
          <div><dt>Owner codes<small>Reusable codes for people you invite</small></dt><dd id="setOwners"></dd></div>
          <div><dt>Public enrollment<small>Anyone can get a setup code at /setup when enabled</small></dt><dd id="setOpen"></dd></div>
          <div><dt>Relay limit<small>All timers on this relay</small></dt><dd id="setLimit"></dd></div>
          <div><dt>Per owner<small>Timers one owner can connect</small></dt><dd id="setPerOwner"></dd></div>
        </dl>
        <div class="visibility-control">
          <label id="openLabel" for="openToggle">Allow new public enrollments<small>Existing timers and share links keep working when paused.</small></label>
          <button id="openToggle" class="visibility-toggle" type="button" role="switch" aria-checked="false" aria-labelledby="openLabel" aria-describedby="openMessage" disabled></button>
        </div>
        <p id="openMessage" class="site-message" role="status" aria-live="polite"></p>
      </div>
    </section>

    <section class="section" aria-labelledby="landingTitle">
      <div class="section-head"><div><span class="eyebrow">Public site</span><h2 id="landingTitle">Public landing page</h2><p>The introduction visitors see at your relay's root address (/).</p></div></div>
      <div class="panel">
        <div class="visibility-control">
          <label id="landingLabel" for="landingToggle">Show the landing page<small>When hidden, / returns Not found. Share links and public enrollment are unchanged.</small></label>
          <button id="landingToggle" class="visibility-toggle" type="button" role="switch" aria-checked="false" aria-labelledby="landingLabel" aria-describedby="landingMessage" disabled></button>
        </div>
        <p id="landingMessage" class="site-message" role="status" aria-live="polite"></p>
        <div class="panel-links"><a href="/admin/landing-preview" target="_blank" rel="noreferrer">Preview landing page</a><a id="landingPublicLink" class="hidden" target="_blank" rel="noreferrer">Open public page</a></div>
        <p class="panel-note">Saved on this relay and kept after restart. <code>OSTRELAY_LANDING_PAGE_ENABLED=false</code> forces the public page off.</p>
      </div>
    </section>

    <p class="foot">Keep this admin address private. The supplied reverse proxy blocks public access to /admin. <span id="updated"></span></p>
  </div>
</main>

<dialog id="inviteDialog" aria-labelledby="inviteTitle">
  <form id="inviteForm" class="step" novalidate>
    <h2 id="inviteTitle">Invite owner</h2>
    <p>Create a reusable setup code for this owner. The per-owner timer limit still applies.</p>
    <div class="field">
      <label for="ownerName">Name</label>
      <input id="ownerName" maxlength="40" autocomplete="off" required>
      <span class="hint">Start with a letter or number. Up to 40 characters; spaces, dots, dashes and underscores are allowed. Visible only in admin.</span>
      <span id="inviteError" class="error" role="alert"></span>
    </div>
    <div class="form-actions"><button class="secondary" type="button" data-close>Cancel</button><button id="inviteSubmit" class="primary" type="submit">Create setup code</button></div>
  </form>
</dialog>

<dialog id="codeDialog" aria-labelledby="codeTitle">
  <h2 id="codeTitle">Setup code</h2>
  <p id="codeLead"></p>
  <div class="code-box"><output id="codeValue"></output></div>
  <p id="codeExpiry" class="muted"></p>
  <div id="codeWarning" class="callout"></div>
  <div class="form-actions"><button class="secondary" type="button" data-close>Done</button><button id="codeCopy" class="primary" type="button">Copy code</button></div>
</dialog>

<dialog id="confirmDialog" aria-labelledby="confirmTitle">
  <h2 id="confirmTitle"></h2>
  <p id="confirmBody"></p>
  <div class="form-actions"><button class="secondary" type="button" data-close>Cancel</button><button id="confirmAction" class="danger-button" type="button"></button></div>
</dialog>

<div id="toast" class="toast hidden" role="status" aria-live="polite"></div>
<script src="/admin/assets/admin.js" defer></script>
</body>
</html>
"""

ADMIN_JS = THEME_JS + r"""(() => {
  const $ = (id) => document.getElementById(id);
  const TOKEN_KEY = 'ostrelayAdminToken';
  let token = '', refreshTimer = 0, countdown = 0, toastTimer = 0, lastLoad = 0, busy = false, siteSaving = false;
  let siteSettings = null, publicUrl = '', siteRevision = 0;
  let enrollmentSettings = null, enrollmentSaving = false, enrollmentRevision = 0;
  let inventoryRevision = '', timerCount = 0;

  // ---- helpers -----------------------------------------------------------
  const el = (tag, cls, text) => {
    const node = document.createElement(tag);
    if (cls) node.className = cls;
    if (text !== undefined) node.textContent = text;
    return node;
  };
  // English text is the key; values fill {placeholders}. Chinese has no plural, English does.
  const t = (key, values = {}) => window.SmartTimerI18n?.text(key, values)
    ?? key.replace(/\{(\w+)\}/g, (_, name) => values[name]);
  const count = (n, one, many, values = {}) => t(n === 1 ? one : many, { ...values, count: n });
  function ago(unix) {
    if (!unix) return t('Never');
    const s = Math.max(0, Math.floor(Date.now() / 1000) - unix);
    if (s < 60) return t('Just now');
    if (s < 3600) return t('{count} min ago', { count: Math.floor(s / 60) });
    if (s < 86400) return count(Math.floor(s / 3600), '{count} hour ago', '{count} hours ago');
    if (s < 30 * 86400) return count(Math.floor(s / 86400), '{count} day ago', '{count} days ago');
    return new Date(unix * 1000).toLocaleDateString();
  }
  function updated(unix) {
    const s = Math.max(0, Math.floor(Date.now() / 1000) - unix);
    if (s < 60) return t('Updated just now.');
    if (s < 3600) return t('Updated {count} min ago.', { count: Math.floor(s / 60) });
    return count(Math.floor(s / 3600), 'Updated {count} hour ago.', 'Updated {count} hours ago.');
  }
  const date = (unix) => new Date(unix * 1000).toLocaleDateString(undefined, { year: 'numeric', month: 'short', day: 'numeric' });
  function toast(text, error) {
    const node = $('toast');
    node.textContent = text; node.className = 'toast' + (error ? ' err' : '');
    clearTimeout(toastTimer); toastTimer = setTimeout(() => node.classList.add('hidden'), 4200);
  }
  const ERRORS = {
    admin_token_required: 'The admin token is not correct.',
    not_found: 'That item no longer exists. The list has been refreshed.',
    'an owner with that name already exists': 'An owner with that name already exists.',
    landing_page_disabled_by_environment: 'The landing page is disabled by OSTRELAY_LANDING_PAGE_ENABLED. Update that setting and restart the relay to allow it.',
    open_enrollment_disabled_by_environment: 'Public enrollment is disabled by OSTRELAY_ENROLLMENT. Update that setting and restart the relay to allow it.',
    timer_inventory_changed: 'The timer list changed. Refresh and review it before clearing all timers.',
  };
  async function call(method, path, body) {
    const headers = { Authorization: 'Bearer ' + token };
    if (body) headers['Content-Type'] = 'application/json';
    let res;
    try { res = await fetch(path, { method, headers, body: body ? JSON.stringify(body) : undefined, cache: 'no-store' }); }
    catch (_) { throw new Error(t('The relay is not responding.')); }
    if (res.ok) return res.json();
    let detail = '';
    try { detail = (await res.json()).detail; } catch (_) { /* Status only. */ }
    const error = new Error(ERRORS[detail] ? t(ERRORS[detail])
      : (typeof detail === 'string' && detail ? detail : t('Request failed (HTTP {status}).', { status: res.status })));
    error.status = res.status;
    throw error;
  }

  // ---- dialogs -------------------------------------------------------------
  document.querySelectorAll('dialog').forEach((dialog) => {
    dialog.addEventListener('click', (event) => { if (event.target === dialog) dialog.close(); });
    dialog.querySelectorAll('[data-close]').forEach((b) => b.addEventListener('click', () => dialog.close()));
  });
  $('codeDialog').addEventListener('close', () => { clearInterval(countdown); $('codeValue').textContent = ''; });

  function showCode({ title, lead, code, warning, expiresAt }) {
    $('codeTitle').textContent = title;
    $('codeLead').textContent = lead;
    $('codeValue').textContent = code;
    $('codeWarning').textContent = warning;
    $('codeCopy').textContent = t('Copy code');
    clearInterval(countdown);
    $('codeExpiry').classList.toggle('hidden', !expiresAt);
    if (expiresAt) {
      const tick = () => {
        const left = Math.max(0, expiresAt - Math.floor(Date.now() / 1000));
        $('codeExpiry').textContent = left ? t('Expires in {time}.', { time: Math.floor(left / 60) + ':' + String(left % 60).padStart(2, '0') }) : t('Expired. Create a new code.');
        if (!left) clearInterval(countdown);
      };
      tick(); countdown = setInterval(tick, 1000);
    }
    $('codeDialog').showModal();
    $('codeCopy').focus();
  }
  $('codeCopy').addEventListener('click', async () => {
    try { await navigator.clipboard.writeText($('codeValue').textContent); $('codeCopy').textContent = t('Copied'); }
    catch (_) { getSelection().selectAllChildren($('codeValue')); $('codeCopy').textContent = t('Select and copy the code'); }
  });

  let confirmHandler = null;
  function confirmAction({ title, body, action, run }) {
    $('confirmTitle').textContent = title;
    $('confirmBody').textContent = body;
    $('confirmAction').textContent = action;
    $('confirmAction').disabled = false;
    confirmHandler = run;
    $('confirmDialog').showModal();
  }
  $('confirmAction').addEventListener('click', async () => {
    $('confirmAction').disabled = true;
    try { await confirmHandler(); $('confirmDialog').close(); }
    catch (error) { toast(error.message, true); $('confirmDialog').close(); load(); }
  });

  // ---- rendering -------------------------------------------------------------
  function emptyRow(table, cols, title, text) {
    const tr = el('tr'); const td = el('td', 'empty'); td.colSpan = cols;
    td.append(el('strong', '', title), el('span', '', text)); tr.append(td); table.append(tr);
  }

  function renderOverview(o, owners, revision, enrollmentLoadRevision) {
    publicUrl = o.public_url;
    const host = o.public_url.replace(/^https?:\/\//, '');
    $('brandSubtitle').textContent = host;
    $('relayHost').textContent = host;
    const codes = o.enrollment.includes('owner_codes'), anyone = o.enrollment.includes('open');
    $('relaySummary').textContent = t(codes && anyone ? 'Enrollment: owner codes and public enrollment'
      : codes ? 'Enrollment: owner codes' : 'Enrollment: public enrollment')
      + (o.spki_pin_set ? '' : ' · ' + t('TLS key not known yet'));
    const statTimers = $('statTimers'); statTimers.replaceChildren(String(o.timers_enrolled), el('small', '', ' / ' + o.max_enrolled_timers));
    $('statConnected').textContent = String(o.timers_connected);
    $('statOwners').textContent = String(owners.filter((x) => !x.revoked).length);
    const open = o.open_enrollment_enabled;
    $('statOpen').textContent = t(open ? (o.spki_pin_set ? 'Open' : 'Needs TLS key') : 'Invite only');
    const ownerCodes = o.enrollment.includes('owner_codes');
    $('inviteButton').disabled = !ownerCodes;
    $('inviteButton').title = ownerCodes ? '' : t('Owner codes are off (OSTRELAY_ENROLLMENT)');
    const setStatus = (id, on) => { const s = el('span', 'status ' + (on ? 'ok' : 'off'), t(on ? 'On' : 'Off')); $(id).replaceChildren(s); };
    setStatus('setOwners', ownerCodes);
    if (open && o.setup_page_url) {
      const a = el('a', '', t('Open setup page')); a.href = o.setup_page_url; a.target = '_blank'; a.rel = 'noreferrer';
      $('setOpen').replaceChildren(a);
    } else if (open) $('setOpen').textContent = t('Unavailable: TLS key not known yet');
    else setStatus('setOpen', false);
    $('setLimit').textContent = count(o.max_enrolled_timers, '{count} timer', '{count} timers');
    $('setPerOwner').textContent = count(o.max_timers_per_owner, '{count} timer', '{count} timers');
    if (!siteSaving && revision === siteRevision) renderSiteSettings(o);
    if (!enrollmentSaving && enrollmentLoadRevision === enrollmentRevision) renderEnrollmentSettings(o);
    inventoryRevision = o.timer_inventory_revision; timerCount = o.timers_enrolled;
    $('clearTimers').disabled = !timerCount;
  }

  function renderEnrollmentSettings(value) {
    enrollmentSettings = value;
    $('openToggle').setAttribute('aria-checked', String(value.open_enrollment_enabled));
    $('openToggle').disabled = enrollmentSaving || !value.open_enrollment_allowed;
    $('openMessage').className = 'site-message';
    $('openMessage').textContent = t(!value.open_enrollment_allowed
      ? 'Disabled by OSTRELAY_ENROLLMENT. The admin cannot override this policy.'
      : value.open_enrollment_enabled ? 'Anyone can enroll a new timer using the public setup page.'
        : 'New public enrollments are paused. Existing timers and share links keep working.');
  }
  $('openToggle').addEventListener('click', async () => {
    if (enrollmentSaving || !enrollmentSettings?.open_enrollment_allowed) return;
    const requested = !enrollmentSettings.open_enrollment_enabled;
    enrollmentRevision += 1; enrollmentSaving = true; $('openToggle').disabled = true;
    $('openMessage').textContent = t('Saving…');
    try {
      const value = await call('PUT', '/admin/enrollment-settings', {open_enrollment_requested: requested});
      enrollmentSaving = false; renderEnrollmentSettings(value); load();
      toast(t(requested ? 'Public enrollment enabled.' : 'New public enrollments paused. Existing timers keep working.'));
    } catch (error) {
      enrollmentSaving = false; renderEnrollmentSettings(enrollmentSettings);
      $('openMessage').className = 'site-message err';
      $('openMessage').textContent = t('{message} Refresh to confirm the saved setting.', { message: error.message });
      if (error.status === 401) signOut(error.message);
    }
  });
  $('clearTimers').addEventListener('click', () => {
    if (!timerCount || !inventoryRevision) return;
    const reviewed = inventoryRevision, total = timerCount;
    confirmAction({title: t('Clear all registered timers?'),
      body: count(total, 'Remove {count} timer and end their relay registrations and share links. Each timer must be enrolled again. Owner accounts and relay settings are kept.',
        'Remove {count} timers and end their relay registrations and share links. Each timer must be enrolled again. Owner accounts and relay settings are kept.'),
      action: t('Clear all timers'),
      run: async () => {
        const value = await call('DELETE', '/admin/timers', {confirmation: 'clear_all_timers', inventory_revision: reviewed});
        toast(count(value.timers_removed, '{count} timer removed.', '{count} timers removed.')); load();
      },
    });
  });

  function renderSiteSettings(value) {
    siteSettings = value;
    $('landingToggle').setAttribute('aria-checked', String(value.landing_page_enabled));
    $('landingToggle').disabled = siteSaving || !value.landing_page_allowed;
    $('landingMessage').className = 'site-message';
    $('landingMessage').textContent = t(!value.landing_page_allowed
      ? 'Hidden by environment setting. The admin preview remains available.'
      : value.landing_page_enabled ? 'Visible at the public root address.' : 'Hidden. Visitors to / receive Not found (404).');
    $('landingPublicLink').href = publicUrl + '/';
    $('landingPublicLink').classList.toggle('hidden', !value.landing_page_enabled);
  }
  $('landingToggle').addEventListener('click', async () => {
    if (siteSaving || !siteSettings || !siteSettings.landing_page_allowed) return;
    const visible = !siteSettings.landing_page_enabled;
    siteRevision += 1; // A refresh started before this write cannot undo its UI.
    siteSaving = true; $('landingToggle').disabled = true;
    $('landingMessage').textContent = t('Saving…');
    try {
      const value = await call('PUT', '/admin/site-settings', { landing_page_visible: visible });
      siteSaving = false; renderSiteSettings(value);
      toast(t(visible ? 'Landing page is now visible.' : 'Landing page hidden. Share links are unchanged.'));
    } catch (error) {
      siteSaving = false; renderSiteSettings(siteSettings);
      $('landingMessage').className = 'site-message err';
      $('landingMessage').textContent = t('{message} Refresh to confirm the saved setting.', { message: error.message });
      if (error.status === 401) signOut(error.message);
    }
  });

  function renderTimers(timers) {
    const body = $('timers'); body.replaceChildren();
    for (const timer of timers) {
      const tr = el('tr');
      const name = el('td');
      name.append(el('span', 'cell-title', timer.label || timer.hardware_model), el('span', 'cell-sub mono', timer.timer_id));
      const owner = el('td', timer.owner ? '' : 'muted', timer.owner || '—'); owner.dataset.optional = '';
      const state = el('td'); state.append(el('span', 'status ' + (timer.connected_now ? 'ok' : 'off'), t(timer.connected_now ? 'Connected' : 'Offline')));
      const seen = el('td', 'muted', timer.connected_now ? t('Now') : ago(timer.last_seen_at)); seen.dataset.optional = '';
      const actions = el('td', 'actions');
      const remove = el('button', 'ghost danger', t('Remove')); remove.type = 'button';
      remove.addEventListener('click', () => confirmAction({
        title: t('Remove this timer?'),
        body: t('It disconnects now and its share links stop working. Its owner needs a setup code to connect it again.'),
        action: t('Remove timer'),
        run: async () => { await call('DELETE', '/admin/timers/' + encodeURIComponent(timer.timer_id)); toast(t('Timer removed.')); load(); },
      }));
      actions.append(remove);
      tr.append(name, owner, state, seen, actions);
      body.append(tr);
    }
    if (!timers.length) emptyRow(body, 5, t('No timers yet'), t('Timers appear here after their owner pastes a setup code on the timer page.'));
  }

  function renderOwners(owners) {
    const body = $('owners'); body.replaceChildren();
    for (const o of owners) {
      const tr = el('tr');
      const name = el('td'); name.append(el('span', 'cell-title', o.name));
      if (o.revoked) name.append(el('span', 'cell-sub', t('Revoked')));
      else if (o.signs_in) name.append(el('span', 'cell-sub', t('Signs in') + (o.email ? ' · ' + o.email : '')));
      const timersCell = el('td', o.revoked ? 'muted' : '', o.revoked ? '—' : String(o.timers));
      if (!o.revoked) timersCell.dataset.label = t('Timers');
      const added = el('td', 'muted', date(o.created_at)); added.dataset.optional = '';
      const actions = el('td', 'actions');
      if (!o.revoked) {
        const rotate = el('button', 'ghost', t('New code')); rotate.type = 'button';
        rotate.addEventListener('click', () => confirmAction({
          title: t('Create a new code for {name}?', { name: o.name }),
          body: t('Their current setup code stops working. Timers they already connected stay connected.'),
          action: t('Create new code'),
          run: async () => {
            const r = await call('POST', '/admin/owners/' + encodeURIComponent(o.name) + '/rotate');
            $('confirmDialog').close();
            showCode({ title: t('New setup code'), lead: t('Send this to {name}. It connects any of their timers.', { name: o.name }),
                       code: r.setup_code || r.owner_code, warning: t('Shown only once. Anyone with this code can connect timers as {name}.', { name: o.name }) });
            load();
          },
        }));
        const revoke = el('button', 'ghost danger', t('Revoke')); revoke.type = 'button';
        revoke.addEventListener('click', () => confirmAction({
          title: t('Revoke {name}?', { name: o.name }),
          body: count(o.timers, 'Their setup code stops working and {count} timer is removed from this relay, ending any share links.',
            'Their setup code stops working and {count} timers are removed from this relay, ending any share links.'),
          action: t('Revoke owner'),
          run: async () => {
            const r = await call('POST', '/admin/owners/' + encodeURIComponent(o.name) + '/revoke');
            toast(count(r.timers_removed, '{name} revoked. {count} timer removed.', '{name} revoked. {count} timers removed.', { name: o.name })); load();
          },
        }));
        actions.append(rotate, revoke);
      }
      tr.append(name, timersCell, added, actions);
      body.append(tr);
    }
    if (!owners.length) emptyRow(body, 4, t('No owners yet'), t('Invite an owner to give them a reusable setup code.'));
  }

  async function load() {
    if (busy) return;
    busy = true;
    const revision = siteRevision;
    const enrollmentLoadRevision = enrollmentRevision;
    try {
      const [overview, owners, timers] = await Promise.all([
        call('GET', '/admin/overview'), call('GET', '/admin/owners'), call('GET', '/admin/timers')]);
      renderOverview(overview, owners, revision, enrollmentLoadRevision); renderTimers(timers); renderOwners(owners);
      lastLoad = Date.now(); $('updated').textContent = t('Updated just now.');
    } catch (error) {
      if (error.status === 401) signOut(error.message); else toast(error.message, true);
    } finally { busy = false; }
  }

  // ---- session ---------------------------------------------------------------
  function signOut(message) {
    token = ''; clearInterval(refreshTimer);
    try { sessionStorage.removeItem(TOKEN_KEY); } catch (_) { /* Nothing stored. */ }
    $('workspace').classList.add('hidden'); $('signOut').classList.add('hidden'); $('signIn').classList.remove('hidden');
    document.body.classList.add('signed-out');
    $('brandSubtitle').textContent = t('Relay admin');
    document.querySelectorAll('dialog[open]').forEach((dialog) => dialog.close());
    $('codeValue').textContent = '';
    $('token').value = ''; $('signInError').textContent = message || ''; $('token').focus();
  }
  async function signIn(candidate) {
    token = candidate;
    $('signInButton').disabled = true; $('signInError').textContent = '';
    try {
      await call('GET', '/admin/overview');
      try { sessionStorage.setItem(TOKEN_KEY, token); } catch (_) { /* Tab-only convenience. */ }
      $('signIn').classList.add('hidden'); $('workspace').classList.remove('hidden'); $('signOut').classList.remove('hidden');
      document.body.classList.remove('signed-out');
      await load();
      clearInterval(refreshTimer);
      refreshTimer = setInterval(() => { if (!document.hidden && !document.querySelector('dialog[open]')) load(); }, 20000);
    } catch (error) {
      token = '';
      $('signInError').textContent = error.message;
    } finally { $('signInButton').disabled = false; }
  }
  setInterval(() => { if (lastLoad) $('updated').textContent = updated(Math.floor(lastLoad / 1000)); }, 15000);
  // Rows and messages are built in the current language, so rebuild them when it changes.
  window.addEventListener('smarttimer:languagechange', () => { if (token) load(); });

  $('signInForm').addEventListener('submit', (event) => {
    event.preventDefault();
    const value = $('token').value.trim();
    if (!value) { $('signInError').textContent = t('Enter the admin token.'); return; }
    signIn(value);
  });
  $('signOut').addEventListener('click', () => signOut());
  $('tokenToggle').addEventListener('click', () => {
    const show = $('token').type === 'password';
    $('token').type = show ? 'text' : 'password';
    $('tokenToggle').textContent = t(show ? 'Hide' : 'Show');
    $('tokenToggle').setAttribute('aria-pressed', String(show));
    $('token').focus();
  });
  $('refresh').addEventListener('click', load);

  $('inviteButton').addEventListener('click', () => {
    $('ownerName').value = ''; $('inviteError').textContent = '';
    $('inviteDialog').showModal(); $('ownerName').focus();
  });
  $('inviteForm').addEventListener('submit', async (event) => {
    event.preventDefault();
    const name = $('ownerName').value.trim();
      if (!/^[A-Za-z0-9][A-Za-z0-9 ._-]{0,39}$/.test(name)) { $('inviteError').textContent = t('Start with a letter or number; use up to 40 letters, numbers, spaces, dots, dashes or underscores.'); return; }
    $('inviteSubmit').disabled = true;
    try {
      const r = await call('POST', '/admin/owners', { name });
      $('inviteDialog').close();
      showCode({ title: t('Setup code for {name}', { name: r.name }), lead: t('Send this to {name}. They paste it on each timer under Device settings → Remote sharing.', { name: r.name }),
                 code: r.setup_code || r.owner_code, warning: t('Shown only once. Anyone with this code can connect timers as {name}. Use New code if it leaks.', { name: r.name }) });
      load();
    } catch (error) { $('inviteError').textContent = error.message; }
    finally { $('inviteSubmit').disabled = false; }
  });
  $('oneTimeButton').addEventListener('click', async () => {
    $('oneTimeButton').disabled = true;
    try {
      const r = await call('POST', '/admin/enroll-code');
      showCode({ title: t('One-time setup code'), lead: t('Connects one timer without an owner account.'),
                 code: r.setup_code || r.enrollment_code, expiresAt: r.expires_at_unix,
                 warning: t(r.setup_code ? 'Works once. Share it privately.' : 'The relay does not know its TLS key yet, so this is only the enrollment code: the owner must also enter the relay address and key pin.') });
    } catch (error) { toast(error.message, true); }
    finally { $('oneTimeButton').disabled = false; }
  });

  let saved = '';
  try { saved = sessionStorage.getItem(TOKEN_KEY) || ''; } catch (_) { /* Start signed out. */ }
  if (saved) { $('token').value = saved; signIn(saved); } else $('token').focus();
})();
"""
