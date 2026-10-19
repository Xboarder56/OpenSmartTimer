// Bench-only: drive the real partner page (share link) in Chromium and screenshot it.
// node partner.cjs <share-url> <view|start|pause|resume|add|release|lock> <out.png> [minutes|ms] [--light]
'use strict';
const { playwright } = require('./bench_config.cjs');

const [url, action = 'view', out = 'partner.png', arg] = process.argv.slice(2);
const light = process.argv.includes('--light');

(async () => {
  const browser = await playwright.chromium.launch({ headless: true });
  const context = await browser.newContext({ ignoreHTTPSErrors: true, viewport: { width: 390, height: 844 },
                                             deviceScaleFactor: 2, colorScheme: light ? 'light' : 'dark' });
  const page = await context.newPage();
  const errors = [];
  page.on('pageerror', e => errors.push(e.message));
  const snapshot = async () => page.evaluate(() => ({
    connection: document.getElementById('connectionState')?.textContent,
    badge: document.getElementById('stateBadge')?.textContent,
    status: document.getElementById('statusLine')?.textContent,
    dial: document.getElementById('dialTime')?.textContent,
    result: document.getElementById('lastResult')?.classList.contains('hidden') ? '' : document.getElementById('lastResult')?.textContent,
    ended: !document.getElementById('endView')?.classList.contains('hidden'),
    endTitle: document.getElementById('endTitle')?.textContent,
    endMessage: document.getElementById('endMessage')?.textContent,
    scope: document.getElementById('scopeSummary')?.textContent,
  }));
  let outcome = {};
  try {
    await page.goto(url);
    await page.waitForFunction(() => document.getElementById('connectionState').textContent === 'online'
      || !document.getElementById('endView').classList.contains('hidden'), null, { timeout: 20000 });
    await page.waitForTimeout(1500);
    const before = await snapshot();
    const click = async selector => {
      const button = page.locator(selector).first();
      await button.waitFor({ state: 'visible', timeout: 10000 });
      await button.click();
    };
    if (action === 'start') {
      await click(`.preset-button[data-preset="${arg || 5}"]`);
      await click('button[data-op="start"]');
    } else if (action === 'pause') await click('button[data-op="pause"]');
    else if (action === 'resume') await click('button[data-op="resume"]');
    else if (action === 'add') await click(`button[data-op="adjust"][data-adjust="${arg || 300000}"]`);
    else if (action === 'release') await click('button[data-op="release"]');
    else if (action === 'lock') await click('#sealButton');
    if (action !== 'view') {
      await page.waitForFunction(prev => {
        const el = document.getElementById('lastResult');
        return el && !el.classList.contains('hidden') && el.textContent && el.textContent !== prev;
      }, before.result, { timeout: 20000 }).catch(() => {});
      await page.waitForTimeout(2500);  // let the authenticated status stream catch up
    }
    outcome = { action, before, after: await snapshot() };
  } catch (e) {
    outcome = { action, error: e.message, after: await snapshot().catch(() => ({})) };
  }
  await page.screenshot({ path: out, fullPage: true });
  outcome.errors = errors;
  console.log(JSON.stringify(outcome));
  await browser.close();
})();
