// Bench-only: owner UI retry safety against real V2, with the local web/app.js
// injected. The Extend POST reaches the timer (applied) but its response is
// dropped. Expect exactly one +15 min extension per intended action.
const path = require('node:path'), fs = require('node:fs');
const { playwright, T } = require('./bench_config.cjs');
const APP = fs.readFileSync(path.join(__dirname, '../../../web/app.js'), 'utf8');
const I18N = fs.readFileSync(path.join(__dirname, '../../../web/i18n.js'), 'utf8');
const results = [];
const check = (name, ok, detail) => { results.push(!!ok); console.log(JSON.stringify({ [ok ? 'PASS' : 'FAIL']: name, ...(detail !== undefined ? { detail } : {}) })); };
const api = async (p, body) => (await fetch(T + p, body ? { method: 'POST', headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(body) } : {})).json();
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
async function job(p, fields) {
  const s = await api('/api/v1/relay'); const id = s.next_job_id;
  await api(p, { job_id: id, if_boot_id: s.boot_id, ...fields });
  for (let i = 0; i < 120; i++) { const r = await api('/api/v1/relay'); if (r.job.id === id && r.job.state === 'done') return r.job; await sleep(500); }
}
const link = async () => ((await api('/api/v1/relay/links')).links || [])[0];
(async () => {
  for (const l of (await api('/api/v1/relay/links')).links || []) await job('/api/v1/relay/links/revoke', { fp_hash: l.fp_hash });
  await job('/api/v1/relay/links', { ttl_s: 1800, label: 'retry-bench' });
  const browser = await playwright.chromium.launch();
  const page = await (await browser.newContext({ viewport: { width: 430, height: 932 } })).newPage();
  const errors = []; page.on('pageerror', (e) => errors.push(e.message));
  await page.route('**/app.js', (r) => r.fulfill({ contentType: 'text/javascript', body: APP }));
  await page.route('**/i18n.js', (r) => r.fulfill({ contentType: 'text/javascript', body: I18N }));
  let dropResponses = 0; const seen = [];
  await page.route('**/api/v1/relay/links/extend', async (route) => {
    const body = route.request().postData(); seen.push(body);
    const response = await route.fetch();          // reaches the timer: applied
    if (dropResponses > 0) { dropResponses -= 1; return route.abort('failed'); }  // response lost
    return route.fulfill({ response });
  });
  await page.goto(T + '/'); await page.waitForTimeout(2500);
  await page.click('#settingsOpen'); await page.locator('#relaySection').scrollIntoViewIfNeeded();
  await page.waitForSelector('.relay-extend-action', { timeout: 20000 });
  const jobIds = () => seen.map((b) => { try { return JSON.parse(b).job_id; } catch (_) { return new URLSearchParams(b).get('job_id'); } });

  // 1) First response lost; the UI retries the same job by itself.
  let before = (await link()).expires_in_ms;
  dropResponses = 1; seen.length = 0;
  await page.locator('.relay-extend-action').first().click();
  await page.waitForTimeout(9000);
  let after = (await link()).expires_in_ms;
  let added = Math.round((after - before) / 60000);
  check('lost response: one click adds 15 min once', added >= 14 && added <= 15, { added_min: added, job_ids: jobIds() });
  check('lost response: retries reuse one job_id', new Set(jobIds()).size === 1 && seen.length === 2, jobIds());

  // 2) Every response lost: the UI gives up without knowing, then the owner
  // clicks again. The second click must reconcile, not extend again.
  before = (await link()).expires_in_ms;
  dropResponses = 3; seen.length = 0;
  await page.locator('.relay-extend-action').first().click();
  await page.waitForTimeout(8000);
  const message = await page.textContent('#relayMessage');
  check('all responses lost: owner told it will not apply twice', /not be applied twice/.test(message || ''), message);
  await page.locator('.relay-extend-action').first().click();
  await page.waitForTimeout(8000);
  after = (await link()).expires_in_ms;
  added = Math.round((after - before) / 60000);
  check('repeat click after an unknown outcome still adds 15 min once', added >= 14 && added <= 15, { added_min: added, job_ids: jobIds() });
  check('repeat click reuses the unresolved job_id', new Set(jobIds()).size === 1, jobIds());

  // 3) A normal click afterwards is a new job.
  before = (await link()).expires_in_ms; seen.length = 0;
  await page.locator('.relay-extend-action').first().click();
  await page.waitForTimeout(6000);
  added = Math.round(((await link()).expires_in_ms - before) / 60000);
  check('next deliberate click is a new +15 min', added >= 14 && added <= 15, { added_min: added });
  check('no page errors', errors.length === 0, errors);
  for (const l of (await api('/api/v1/relay/links')).links || []) await job('/api/v1/relay/links/revoke', { fp_hash: l.fp_hash });
  console.log(JSON.stringify({ summary: `${results.filter(Boolean).length}/${results.length} passed` }));
  await browser.close();
  process.exitCode = results.every(Boolean) ? 0 : 1;
})();
