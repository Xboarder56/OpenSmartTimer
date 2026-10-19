// Real partner shell and shared appearance handlers, on disposable loopback.
// No capability, relay credentials, timer or production endpoint is accessed.
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs'), path = require('node:path'), http = require('node:http');
const playwright = require('playwright');
const spa = path.resolve(__dirname, '../spa');
const types = {'.html':'text/html', '.js':'text/javascript', '.css':'text/css', '.svg':'image/svg+xml'};

(async () => {
  const server = http.createServer((request, response) => {
    const pathname = new URL(request.url, 'http://127.0.0.1').pathname;
    const name = pathname === '/s/' ? 'index.html' : pathname.startsWith('/s/assets/')
      ? pathname.slice('/s/assets/'.length) : '';
    const file = path.join(spa, name);
    if (!name || name !== path.basename(name) || !fs.existsSync(file)) {
      response.writeHead(404); response.end(); return;
    }
    response.writeHead(200, {'Content-Type':types[path.extname(name)] || 'application/octet-stream',
      'Cache-Control':'no-store',
      'Content-Security-Policy':"default-src 'self'; script-src 'self'; style-src 'self'; connect-src 'self'; base-uri 'none'"});
    response.end(fs.readFileSync(file));
  });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  const url = 'http://127.0.0.1:' + server.address().port + '/s/';
  try {
    for (const engine of ['chromium', 'webkit']) {
      const browser = await playwright[engine].launch({headless:true});
      try {
        for (const touch of [false, true]) {
          const context = await browser.newContext({viewport:{width:touch ? 390 : 1280, height:950},
            hasTouch:touch, isMobile:touch, colorScheme:'light'});
          try {
            const page = await context.newPage(), errors = [];
            page.on('pageerror', error => errors.push(error.message));
            await page.goto(url);
            await page.locator('#endView').waitFor({state:'visible'});
            const menu = page.locator('.appearance-menu');
            const summary = menu.locator('summary');
            const activate = locator => touch ? locator.tap() : locator.click();
            for (const palette of ['teal', 'slate', 'purple']) {
              const previous = await page.locator('body').evaluate(node => getComputedStyle(node).backgroundColor);
              await activate(summary);
              await activate(menu.locator('[data-palette-choice="' + palette + '"]'));
              assert.equal(await page.locator('html').getAttribute('data-palette'), palette,
                engine + ': ' + (touch ? 'touch' : 'mouse') + ' must deliver the palette click');
              assert.equal(await menu.locator('[data-palette-choice="' + palette + '"]').getAttribute('aria-pressed'), 'true');
              await page.waitForFunction(previous => getComputedStyle(document.body).backgroundColor !== previous, previous);
              await page.keyboard.press('Escape');
              assert.equal(await menu.getAttribute('open'), null);
              assert.equal(await summary.evaluate(node => node === document.activeElement), true);
            }
            await activate(summary);
            await activate(menu.locator('[data-palette-choice="teal"]'));
            await page.locator('#endTitle').click();
            assert.equal(await menu.getAttribute('open'), null, 'outside clicks still dismiss the menu');
            await page.reload();
            await page.locator('#endView').waitFor({state:'visible'});
            assert.equal(await page.locator('html').getAttribute('data-palette'), 'teal', 'choice survives reload');
            await activate(summary);
            await menu.locator('[data-palette-choice="teal"]').focus();
            await page.keyboard.press('Tab');
            await page.waitForFunction(() => !document.querySelector('.appearance-menu').open);
            assert.equal(await menu.getAttribute('open'), null, 'keyboard focus leaving the menu dismisses it');
            await page.locator('#themeButton').click();
            assert.equal(await page.locator('html').getAttribute('data-theme'), 'dark');
            assert.deepEqual(errors, []);
          } finally { await context.close(); }
        }
        console.log('PASS ' + engine + ': partner palette mouse/touch, visible colors, storage, Escape, outside click, keyboard exit and light/dark');
      } finally { await browser.close(); }
    }
  } finally { await new Promise(resolve => server.close(resolve)); }
})().catch(error => { console.error(error); process.exitCode = 1; });
