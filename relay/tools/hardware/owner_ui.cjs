// Bench-only: drive the timer's own Remote sharing UI against real V2 + relay.
// Creates a 15-min link, checks SHARED badge, extends, revokes. Never prints URLs.
const { playwright, T, shot } = require('./bench_config.cjs');
const results = [];
const check = (name, ok, detail) => { results.push(!!ok); console.log(JSON.stringify({ [ok ? 'PASS' : 'FAIL']: name, ...(detail !== undefined ? { detail } : {}) })); };
const api = async (p) => (await fetch(T + p)).json();
(async () => {
  const browser = await playwright.chromium.launch();
  const ctx = await browser.newContext({ viewport: { width: 430, height: 932 }, permissions: ['clipboard-read', 'clipboard-write'] });
  const page = await ctx.newPage();
  const errors = [];
  page.on('pageerror', (e) => errors.push(e.message));
  try {
    await page.goto(T + '/');
    await page.waitForSelector('#settingsOpen', { state: 'visible' });
    await page.waitForTimeout(2000);
    await page.click('#settingsOpen');
    await page.locator('#relaySection').scrollIntoViewIfNeeded();
    await page.waitForFunction(() => /Connected/i.test(document.getElementById('relayState')?.textContent || ''), null, { timeout: 20000 });
    check('Remote sharing shows Connected', true, await page.textContent('#relayState'));
    await page.fill('#relayLinkLabel', 'ui-bench');
    await page.locator('#relayDuration .preset-button').first().click();
    await page.click('#relayCreateButton');
    await page.waitForFunction(() => !document.getElementById('relayShareBox').classList.contains('hidden'), null, { timeout: 60000 });
    const url = await page.inputValue('#relayShareUrl');
    const share = new URL(url);
    const fragment = new URLSearchParams(share.hash.slice(1));
    check('create link shows the trusted-page share URL', share.protocol === 'https:' && /^\/s\/[A-Za-z0-9_-]{22}$/.test(share.pathname) && !share.search && fragment.has('relay') && fragment.get('cap')?.startsWith('ost-cap.v1.'), url ? 'share URL present' : 'missing');
    await page.locator('#relaySection').screenshot({ path: shot('owner-ui-created.png'), mask: [page.locator('#relayShareUrl')] });
    const links1 = (await api('/api/v1/relay/links')).links;
    check('one live link on the timer', links1.length === 1 && links1[0].label === 'ui-bench', links1.map((l) => ({ label: l.label, min: Math.round(l.expires_in_ms / 60000) })));
    await page.click('#settingsClose').catch(() => page.keyboard.press('Escape'));
    await page.waitForTimeout(3000);
    check('SHARED badge on the main page', await page.isVisible('#sharedBadge'));
    await page.click('#sharedBadge');
    await page.locator('#relaySection').scrollIntoViewIfNeeded();
    const before = links1[0].expires_in_ms;
    await page.locator('.relay-extend-action').first().click();
    await page.waitForTimeout(6000);
    const links2 = (await api('/api/v1/relay/links')).links;
    check('Extend +15 min from the UI keeps the same link', links2.length === 1 && links2[0].fp_hash === links1[0].fp_hash && links2[0].expires_in_ms > before + 10 * 60000,
          { before_min: Math.round(before / 60000), after_min: Math.round((links2[0] || {}).expires_in_ms / 60000) });
    await page.locator('.relay-revoke').first().click();
    await page.waitForSelector('#confirmDialog[aria-hidden="false"]');
    check('revoke confirmation text', /create a new link afterwards/.test(await page.textContent('#confirmMessage')), await page.textContent('#confirmMessage'));
    await page.click('#confirmAccept');
    await page.waitForFunction(async () => true, null, { timeout: 1000 }).catch(() => {});
    await page.waitForTimeout(6000);
    const links3 = (await api('/api/v1/relay/links')).links;
    check('Revoke from the UI removes the link', links3.length === 0, links3.length);
    await page.locator('#relaySection').screenshot({ path: shot('owner-ui-after-revoke.png') });
    await page.click('#settingsClose').catch(() => page.keyboard.press('Escape'));
    await page.waitForTimeout(3000);
    check('SHARED badge gone after revoke', !(await page.isVisible('#sharedBadge')));
  } catch (e) { check('UI flow completed', false, e.message.split('\n')[0]); }
  check('no page errors', errors.length === 0, errors);
  console.log(JSON.stringify({ summary: `${results.filter(Boolean).length}/${results.length} passed` }));
  await browser.close();
  process.exitCode = results.every(Boolean) ? 0 : 1;
})();
