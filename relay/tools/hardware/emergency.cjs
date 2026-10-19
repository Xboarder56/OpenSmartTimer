// Bench-only: emergency input with a live share link and a partner-started
// session. Prints READY, then waits (up to 10 min) for the physical trigger.
const { playwright, T, ADMIN, adminToken, shot } = require('./bench_config.cjs');
const TOKEN = adminToken();
const results = [];
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const log = (o) => console.log(JSON.stringify({ t: new Date().toISOString().slice(11, 19), ...o }));
const check = (name, ok, detail) => { results.push(!!ok); log({ [ok ? 'PASS' : 'FAIL']: name, ...(detail !== undefined ? { detail } : {}) }); };
async function api(method, p, body) {
  const res = await fetch(T + p, { method, headers: body ? { 'Content-Type': 'application/json' } : {}, body: body ? JSON.stringify(body) : undefined });
  let j = {}; try { j = await res.json(); } catch (_) {} return [res.status, j];
}
const relay = async () => (await api('GET', '/api/v1/relay'))[1];
const status = async () => (await api('GET', '/api/v1/status'))[1];
async function waitFor(fn, s, label) { const end = Date.now() + s * 1000; while (Date.now() < end) { const v = await fn().catch(() => null); if (v) return v; await sleep(500); } throw new Error(label + ' timed out'); }
async function job(p, fields) {
  const s = await relay(); const id = s.next_job_id;
  const [c, b] = await api('POST', p, { job_id: id, if_boot_id: s.boot_id, ...fields });
  if (c !== 202) throw new Error(p + ' http ' + c + ' ' + b.code);
  return (await waitFor(async () => { const r = await relay(); return r.job.id === id && r.job.state === 'done' && r; }, 60, 'job')).job;
}

(async () => {
  const browser = await playwright.chromium.launch();
  try {
    for (const l of (await api('GET', '/api/v1/relay/links'))[1].links || []) await job('/api/v1/relay/links/revoke', { fp_hash: l.fp_hash });
    const minted = await job('/api/v1/relay/links', { ttl_s: 3600, label: 'emergency-bench' });
    const share = await waitFor(async () => { const [c, b] = await api('GET', '/api/v1/relay/links/share?fp_hash=' + minted.fp_hash); return c === 200 && b; }, 30, 'share');
    const ctx = await browser.newContext({ ignoreHTTPSErrors: true, viewport: { width: 390, height: 844 } });
    const page = await ctx.newPage();
    await page.goto(share.share_url);
    await page.waitForFunction(() => document.getElementById('connectionState').textContent === 'online', null, { timeout: 30000 });
    await page.waitForTimeout(1500);
    await page.click('.preset-button[data-preset="5"]'); await page.click('button[data-op="start"]');
    const running = await waitFor(async () => { const s = await status(); return s.state === 'running' && s; }, 20, 'partner start');
    check('partner started a 5-min session through the link', true, { share_link_limit_ms: running.share_link_limit_ms });
    const before = await relay();
    log({ READY: 'Trigger the emergency input now', relay: before.state, live_links: before.live_links, timer: running.state });

    const t0 = Date.now();
    const tripped = await waitFor(async () => { const s = await status(); return s.state !== 'running' && s; }, 600, 'emergency trigger');
    log({ observed_after_s: Math.round((Date.now() - t0) / 1000), timer_state: tripped.state, reason: tripped.last_release_reason });
    check('timer released by the emergency input', tripped.last_release_reason === 'emergency', { state: tripped.state, reason: tripped.last_release_reason });
    const r = await waitFor(async () => { const x = await relay(); return !x.enabled && x; }, 15, 'relay disabled').catch(() => relay());
    check('remote sharing turned off', r.enabled === false, { state: r.state, enabled: r.enabled, last_error: r.last_error });
    check('reason shown to the owner is emergency_input', r.last_error === 'emergency_input', r.last_error);
    const links = (await api('GET', '/api/v1/relay/links'))[1].links || [];
    check('no live share link remains', links.length === 0, links.length);
    const off = await page.waitForFunction(() => document.getElementById('connectionState').textContent !== 'online'
      || !document.getElementById('endView').classList.contains('hidden'), null, { timeout: 30000 }).then(() => true).catch(() => false);
    const snap = await page.evaluate(() => ({ connection: document.getElementById('connectionState')?.textContent,
      ended: !document.getElementById('endView')?.classList.contains('hidden'), endTitle: document.getElementById('endTitle')?.textContent }));
    check('partner page loses control', off, snap);
    const tryStart = await page.locator('button[data-op="start"]').isVisible().catch(() => false);
    check('partner has no start control', !tryStart || snap.connection !== 'online', { start_visible: tryStart });
    await page.screenshot({ path: shot('emergency-partner.png'), fullPage: true });
    const admin = await fetch(ADMIN + '/admin/timers', { headers: { Authorization: 'Bearer ' + TOKEN } }).then((x) => x.json());
    const me = admin.find((t) => t.timer_id === before.timer_id) || {};
    check('relay server sees the timer disconnected', me.connected_now === false, { connected_now: me.connected_now });
    await sleep(3000);
    const s2 = await status();
    const [sc, sb] = await api('POST', '/api/v1/timer/start', { duration_ms: 3000, if_boot_id: s2.boot_id, operation_id: 'emg-local-' + Date.now() });
    check('local start still works afterwards', sc === 200 && sb.state === 'running', { http: sc, state: sb.state || sb.code });
    await waitFor(async () => (await status()).state === 'open', 15, 'local session end');
    await ctx.close();
  } catch (e) { check('emergency test completed', false, e.message.split('\n')[0]); }
  await browser.close();
  log({ summary: `${results.filter(Boolean).length}/${results.length} passed` });
  process.exitCode = results.every(Boolean) ? 0 : 1;
})();
