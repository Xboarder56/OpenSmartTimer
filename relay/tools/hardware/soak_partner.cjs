// Keeps one real partner page attached during the soak; capability arrives on stdin.
'use strict';
const fs = require('node:fs');
const readline = require('node:readline');
const { playwright } = require('./bench_config.cjs');
const output = process.argv[2];
const lines = readline.createInterface({ input: process.stdin });
let stop;
const stopping = new Promise(resolve => { stop = resolve; });
let started = false;
lines.on('line', async line => {
  if (started) { if (line === 'stop') stop(); return; }
  started = true;
  const report = { samples: 0, online_samples: 0, offline_samples: 0, errors: [], transitions: [] };
  let browser;
  let interval;
  const save = () => fs.writeFileSync(output, JSON.stringify(report, null, 2) + '\n', { mode: 0o600 });
  try {
    const { url } = JSON.parse(line);
    browser = await playwright.chromium.launch({ headless: true });
    const context = await browser.newContext({ ignoreHTTPSErrors: true });
    const page = await context.newPage();
    page.on('pageerror', error => { report.errors.push(error.name); save(); });
    await page.goto(url);
    await page.waitForFunction(() => document.getElementById('connectionState')?.textContent === 'online', null, { timeout: 30000 });
    let previous;
    let sampling = false;
    const sample = async () => {
      if (sampling) return;
      sampling = true;
      try {
        const state = await page.locator('#connectionState').textContent();
        report.current_state = state;
        report.samples++;
        if (state === 'online') report.online_samples++; else report.offline_samples++;
        if (state !== previous) { report.transitions.push({ utc: new Date().toISOString(), state }); previous = state; }
        save();
      } catch (error) { report.errors.push(error.name); save(); }
      finally { sampling = false; }
    };
    await sample();
    interval = setInterval(sample, 5000);
    await stopping;
    clearInterval(interval);
    interval = undefined;
    await sample();
  } catch (error) { report.errors.push(error.name); }
  finally {
    if (interval) clearInterval(interval);
    if (browser) await browser.close();
    save();
    lines.close();
    process.exit(report.errors.length ? 1 : 0);
  }
});
