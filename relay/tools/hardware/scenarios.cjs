// Bench-only V2 relay scenarios against real hardware: Extend (same URL, open
// partner page follows, hold limit raised), fail-open at link expiry, and
// Reactivate. Never prints share URLs. Run with nobody attached to the timer.
const path = require('node:path'), fs = require('node:fs');
const { playwright, T, ADMIN, adminToken, shot } = require('./bench_config.cjs');

const only = process.argv[2] || 'all';
const results = [];
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const log = (obj) => console.log(JSON.stringify({ t: new Date().toISOString().slice(11, 19), ...obj }));
function check(name, ok, detail) {
  results.push({ name, ok: !!ok });
  log({ [ok ? 'PASS' : 'FAIL']: name, ...(detail ? { detail } : {}) });
}

async function api(method, p, body) {
  const res = await fetch(T + p, { method, headers: body ? { 'Content-Type': 'application/json' } : {},
                                   body: body ? JSON.stringify(body) : undefined });
  let json = {};
  try { json = await res.json(); } catch (_) { /* empty */ }
  return [res.status, json];
}
const relay = async () => (await api('GET', '/api/v1/relay'))[1];
const status = async () => (await api('GET', '/api/v1/status'))[1];
const links = async () => (await api('GET', '/api/v1/relay/links'))[1].links || [];
async function waitFor(fn, seconds, label) {
  const end = Date.now() + seconds * 1000;
  let last;
  while (Date.now() < end) {
    last = await fn();
    if (last) return last;
    await sleep(1000);
  }
  throw new Error(label + ' timed out');
}
async function job(p, fields) {
  const s = await relay();
  const id = s.next_job_id;
  const [code, body] = await api('POST', p, { job_id: id, if_boot_id: s.boot_id, ...fields });
  if (code !== 202) return { http: code, code: body.code };
  const done = await waitFor(async () => { const r = await relay(); return r.job.id === id && r.job.state === 'done' && r; }, 60, 'job ' + id);
  return done.job;
}
async function mint(label, ttl) {
  const j = await job('/api/v1/relay/links', { ttl_s: ttl, label });
  if (j.result !== 'ok') throw new Error('mint failed ' + JSON.stringify(j));
  const share = await waitFor(async () => {
    const [c, b] = await api('GET', '/api/v1/relay/links/share?fp_hash=' + j.fp_hash);
    return c === 200 && b;
  }, 30, 'share url');
  return { fp: j.fp_hash, url: share.share_url };
}
async function clearLinks() {
  for (const l of await links()) await job('/api/v1/relay/links/revoke', { fp_hash: l.fp_hash });
}
async function ensureOpen() {
  const s = await status();
  if (s.state !== 'open') await api('POST', '/api/v1/timer/release', { if_boot_id: s.boot_id, operation_id: 'scn-' + Date.now() });
  await waitFor(async () => (await status()).state === 'open', 15, 'timer open');
}

async function partner(browser, url) {
  const context = await browser.newContext({ ignoreHTTPSErrors: true, viewport: { width: 390, height: 844 } });
  const page = await context.newPage();
  const errors = [];
  page.on('pageerror', (e) => errors.push(e.message));
  await page.goto(url);
  await page.waitForFunction(() => document.getElementById('connectionState').textContent === 'online', null, { timeout: 30000 });
  await page.waitForTimeout(1500);
  const snap = () => page.evaluate(() => ({
    connection: document.getElementById('connectionState')?.textContent,
    status: document.getElementById('statusLine')?.textContent,
    expiry: document.getElementById('expiryCountdown')?.textContent,
    result: document.getElementById('lastResult')?.classList.contains('hidden') ? '' : document.getElementById('lastResult')?.textContent,
    ended: !document.getElementById('endView')?.classList.contains('hidden'),
    endTitle: document.getElementById('endTitle')?.textContent,
    href: location.pathname,
  }));
  const act = async (selectors) => {
    const before = (await snap()).result;
    for (const s of selectors) { const b = page.locator(s).first(); await b.waitFor({ state: 'visible', timeout: 10000 }); await b.click(); }
    await page.waitForFunction((prev) => { const el = document.getElementById('lastResult'); return el && !el.classList.contains('hidden') && el.textContent !== prev; }, before, { timeout: 20000 }).catch(() => {});
    await page.waitForTimeout(2500);
    return snap();
  };
  return { page, context, errors, snap, act };
}
// "14:59" / "1:02:03" / "59s" style countdown text to seconds (best effort).
function countdownSeconds(text) {
  const m = (text || '').match(/(\d+):(\d{2})(?::(\d{2}))?/);
  if (!m) return null;
  return m[3] !== undefined ? (+m[1]) * 3600 + (+m[2]) * 60 + (+m[3]) : (+m[1]) * 60 + (+m[2]);
}

async function scenarioExtend(browser) {
  log({ scenario: 'extend' });
  await ensureOpen(); await clearLinks();
  const link = await mint('scn-extend', 120);
  const p = await partner(browser, link.url);
  const before = await p.snap();
  log({ partner_before: { connection: before.connection, expiry: before.expiry } });

  const refused = await p.act(['.preset-button[data-preset="5"]', 'button[data-op="start"]']);
  const s1 = await status();
  check('5-min start refused while the link has 2 min left', s1.state === 'open', { result: refused.result, state: s1.state });

  const ext = await job('/api/v1/relay/links/extend', { fp_hash: link.fp, ttl_s: 900 });
  check('extend job ok', ext.result === 'ok', ext);
  const l = (await links())[0] || {};
  check('same link after extend (fp unchanged, ~17 min left)', l.fp_hash === link.fp && l.expires_in_ms > 15 * 60000, { fp_same: l.fp_hash === link.fp, expires_in_ms: l.expires_in_ms });
  const followed = await waitFor(async () => { const s = await p.snap(); const sec = countdownSeconds(s.expiry); return sec && sec > 10 * 60 && s; }, 40, 'partner follows extension').catch(() => null);
  const after = await p.snap();
  check('open partner page shows the later expiry without a new URL', !!followed, { expiry_before: before.expiry, expiry_after: after.expiry, connection: after.connection });

  const started = await p.act(['.preset-button[data-preset="5"]', 'button[data-op="start"]']);
  const s2 = await status();
  check('5-min start accepted after extend (hold limit raised)', s2.state === 'running', { result: started.result, state: s2.state, share_link_limit_ms: s2.share_link_limit_ms });
  check('status reports the share-link hold limit', typeof s2.share_link_limit_ms === 'number' && s2.share_link_limit_ms > 10 * 60000, { share_link_limit_ms: s2.share_link_limit_ms });

  const ext2 = await job('/api/v1/relay/links/extend', { fp_hash: link.fp, ttl_s: 3600 });
  const s3 = await status();
  check('extend mid-session raises the hold limit', ext2.result === 'ok' && s3.share_link_limit_ms > 60 * 60000, { job: ext2.result, share_link_limit_ms: s3.share_link_limit_ms });

  const released = await p.act(['button[data-op="release"]']);
  const s4 = await status();
  check('partner release opens the timer', s4.state === 'open', { result: released.result });
  check('partner page had no script errors', p.errors.length === 0, p.errors);
  await p.context.close();
  await clearLinks();
}

async function scenarioFailOpen(browser) {
  log({ scenario: 'fail-open + reactivate' });
  await ensureOpen(); await clearLinks();
  const link = await mint('scn-failopen', 75);
  const mintedAt = Date.now();
  const s0 = await status();
  // Owner starts locally, so only the partner's pause ties the session to the link.
  const [sc, sb] = await api('POST', '/api/v1/timer/start', { duration_ms: 10 * 60000, if_boot_id: s0.boot_id, operation_id: 'scn-start-' + Date.now() });
  check('owner starts 10 min locally', sc === 200 && sb.state === 'running', { http: sc, state: sb.state });
  const s1 = await status();
  check('no hold limit before the partner acts', s1.share_link_limit_ms === null, { share_link_limit_ms: s1.share_link_limit_ms });

  const p = await partner(browser, link.url);
  const paused = await p.act(['button[data-op="pause"]']);
  const s2 = await status();
  check('partner pause ties the session to the link', s2.state === 'paused' && typeof s2.share_link_limit_ms === 'number' && s2.share_link_limit_ms <= 75000,
        { result: paused.result, state: s2.state, share_link_limit_ms: s2.share_link_limit_ms });

  const opened = await waitFor(async () => { const s = await status(); return s.state === 'open' && s; }, 110, 'fail-open').catch(() => null);
  const openedAfter = Math.round((Date.now() - mintedAt) / 1000);
  check('timer opens by itself when the link expires (still paused)', opened && opened.last_release_reason === 'share_link_expired',
        { state: opened && opened.state, reason: opened && opened.last_release_reason, seconds_after_mint: openedAfter });
  const [, events] = await api('GET', '/api/v1/events');
  const names = (events.events || []).map((e) => e.type || e.event).slice(-5);
  check('share_link_expired event logged', names.includes('share_link_expired'), names);
  const ended = await waitFor(async () => { const s = await p.snap(); return (s.ended || s.connection !== 'online') && s; }, 30, 'partner sees expiry').catch(() => null);
  check('partner page shows the link ended', !!ended, ended && { endTitle: ended.endTitle, connection: ended.connection });

  // Reactivate: extend the expired link. The same URL works again, and the
  // open partner page checks back on its own.
  const re = await job('/api/v1/relay/links/extend', { fp_hash: link.fp, ttl_s: 600 });
  const l = (await links())[0] || {};
  check('reactivate expired link (same fp, ~10 min)', re.result === 'ok' && l.fp_hash === link.fp && l.expires_in_ms > 8 * 60000, { job: re.result, expires_in_ms: l.expires_in_ms });
  const back = await waitFor(async () => { const s = await p.snap(); return s.connection === 'online' && !s.ended && s; }, 60, 'partner back online').catch(() => null);
  check('open partner page reconnects on the same URL', !!back, back && { connection: back.connection, expiry: back.expiry });
  const fresh = await partner(browser, link.url).catch((e) => ({ error: e.message }));
  check('same URL opens in a fresh browser after reactivation', !fresh.error, fresh.error);
  if (!fresh.error) await fresh.context.close();
  check('partner page had no script errors', p.errors.length === 0, p.errors);
  await p.context.close();
  await clearLinks();
  await ensureOpen();
}

(async () => {
  const browser = await playwright.chromium.launch({ headless: true });
  try {
    if (only === 'all' || only === 'extend') await scenarioExtend(browser);
    if (only === 'all' || only === 'failopen') await scenarioFailOpen(browser);
  } catch (e) {
    check('scenario completed', false, e.message);
    await clearLinks().catch(() => {}); await ensureOpen().catch(() => {});
  } finally {
    await browser.close();
  }
  const failed = results.filter((r) => !r.ok);
  log({ summary: `${results.length - failed.length}/${results.length} passed`, failed: failed.map((r) => r.name) });
  process.exit(failed.length ? 1 : 0);
})();
