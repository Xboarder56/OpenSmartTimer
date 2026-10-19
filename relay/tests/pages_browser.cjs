// Real admin/landing/setup pages on a disposable, loopback-only relay.
// No production .env, credentials, SQLite file, relay or timer is accessed.
'use strict';
const assert = require('node:assert/strict');
const {spawn} = require('node:child_process');
const fs = require('node:fs');
const net = require('node:net');
const path = require('node:path');
const playwright = require('playwright');
const root = path.resolve(__dirname, '..');
const artifacts = process.env.OSTRELAY_PAGES_SCREENSHOTS;
const token = 'disposable-ui-test-token-' + '0'.repeat(32);

(async () => {
  const port = await new Promise((resolve, reject) => {
    const probe = net.createServer();
    probe.on('error', reject);
    probe.listen(0, '127.0.0.1', () => { const port = probe.address().port; probe.close(() => resolve(port)); });
  });
  const origin = `http://127.0.0.1:${port}`;
  const env = {...process.env, PYTHONPATH: path.join(root, 'src')};
  for (const key of Object.keys(env)) if (key.startsWith('OSTRELAY_')) delete env[key];
  const server = spawn(path.join(root, '.venv/bin/python'), ['-c', `
from ostrelay import config
config._settings = config.Settings(_env_file=None, db_path=":memory:",
    dev_mode=True, public_url="${origin}", spki_pin="A" * 43,
    admin_token="${token}", cookie_secret="00" * 32, enrollment="owner_codes,open",
    oidc_issuer="https://disposable-identity.example", oidc_client_id="ui-test",
    oidc_client_secret="disposable-ui-client-secret")
from ostrelay.main import create_app
import uvicorn
uvicorn.run(create_app(), host="127.0.0.1", port=${port}, log_level="error")
  `], {cwd: root, env, stdio: 'ignore'});
  let browser;
  try {
    let ready = false;
    for (let i = 0; i < 100; i++) {
      try { ready = (await fetch(origin + '/api/v1/health')).ok; } catch (_) {}
      if (ready) break;
      if (server.exitCode !== null) throw new Error('Disposable relay exited before startup');
      await new Promise(resolve => setTimeout(resolve, 50));
    }
    assert(ready, 'disposable relay must start');
    const engine = process.env.OSTRELAY_BROWSER || 'chromium';
    assert(['chromium', 'webkit'].includes(engine), 'supported browser engine');
    browser = await playwright[engine].launch({headless: true});
    const context = await browser.newContext({viewport: {width: 390, height: 844}, colorScheme: 'light', reducedMotion: 'reduce'});
    const page = await context.newPage();
    const errors = [];
    page.on('pageerror', error => errors.push(error.message));
    async function openSettings() {
      if (!await page.locator('#settingsMenu').evaluate(menu => menu.open)) await page.locator('#settingsOpen').click();
    }
    async function clickSetting(selector) {
      if (['#themeButton', '#languageButton'].includes(selector)) {
        if (await page.locator('#settingsMenu').evaluate(menu => menu.open)) await page.keyboard.press('Escape');
      } else await openSettings();
      await page.locator(selector).click();
      if (await page.locator('#settingsMenu').evaluate(menu => menu.open)) await page.keyboard.press('Escape');
    }
    async function fitsAndSwitch() {
      assert(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth), 'page must fit viewport');
      const header = await page.locator('.topbar').evaluate(node => {
        const brand = node.querySelector('.brand').getBoundingClientRect();
        const gear = node.querySelector('#settingsOpen').getBoundingClientRect();
        return {centerDifference: Math.abs((brand.top + brand.bottom - gear.top - gear.bottom) / 2),
          gearWidth: gear.width, gearHeight: gear.height,
          directTheme: Boolean(node.querySelector('.topbar-actions > #themeButton')),
          directLanguage: !node.querySelector('#languageButton') || Boolean(node.querySelector('.topbar-actions > #languageButton'))};
      });
      assert(header.centerDifference < 1 && header.gearWidth === 44 && header.gearHeight === 44 && header.directTheme && header.directLanguage,
        'header must keep light/dark and language directly accessible beside Settings');
      if (await page.locator('dialog[open]').count()) return;
      for (const palette of ['purple', 'slate', 'teal']) {
        await openSettings();
        await page.locator(`[data-palette-choice="${palette}"]`).click();
        assert.equal(await page.locator('html').getAttribute('data-palette'), palette);
        const bounds = await page.locator('.appearance-section').boundingBox();
        assert(bounds.x >= 0 && bounds.x + bounds.width <= page.viewportSize().width);
        await page.keyboard.press('Escape');
        assert.equal(await page.locator('.appearance-menu').getAttribute('open'), null);
      }
      assert(await page.locator('#themeButton').isVisible(), 'theme stays accessible with Settings closed');
      const box = await page.locator('#themeButton').evaluate(node => {
        const style = getComputedStyle(node), bubble = getComputedStyle(node, '::before');
        const width = node.getBoundingClientRect().width - 2;
        const left = parseFloat(bubble.left), size = parseFloat(bubble.width);
        return {width: node.offsetWidth, height: node.offsetHeight, padding: style.padding,
          radius: style.borderRadius, bubbleSizing: bubble.boxSizing, left, right: width - left - size};
      });
      assert(box.width >= 64 && box.width <= 84); assert.equal(box.height, 44);
      assert.equal(box.padding, '2px'); assert.equal(box.radius, '12px');
      assert.equal(box.bubbleSizing, 'border-box');
      const theme = await page.locator('html').getAttribute('data-theme');
      assert(Math.abs((theme === 'dark' ? box.right : box.left) - 2) < 0.1, 'bubble outer inset must be 2px: ' + JSON.stringify({theme, ...box}));
      await page.keyboard.press('Escape');
    }
    async function shot(name) {
      if (!artifacts) return;
      fs.mkdirSync(artifacts, {recursive: true});
      await page.screenshot({path: path.join(artifacts, name + '.png'), fullPage: true, animations: 'disabled'});
    }
    await page.goto(origin + '/admin/');
    assert.equal(await page.locator('html').getAttribute('data-theme'), 'light');
    await fitsAndSwitch(); await shot('admin-signin-light');
    await page.locator('#token').fill(token);
    await page.locator('#signInButton').click();
    await page.locator('#workspace').waitFor({state: 'visible'});
    await page.waitForFunction(() => !document.getElementById('landingToggle').disabled);
    await page.waitForFunction(() => !document.getElementById('openToggle').disabled);
    await page.locator('#openToggle').click();
    await page.waitForFunction(() => document.getElementById('openToggle').getAttribute('aria-checked') === 'false');
    assert.equal((await context.request.get(origin + '/setup')).status(), 404);
    await page.reload(); await page.locator('#workspace').waitFor({state:'visible'});
    await page.waitForFunction(() => !document.getElementById('openToggle').disabled);
    assert.equal(await page.locator('#openToggle').getAttribute('aria-checked'), 'false');
    await page.locator('#openToggle').click();
    await page.waitForFunction(() => document.getElementById('openToggle').getAttribute('aria-checked') === 'true');
    assert.equal((await context.request.get(origin + '/setup')).status(), 200);
    for (const width of [320, 390, 1280]) {
      await page.setViewportSize({width, height: 900});
      for (const theme of ['light', 'dark']) {
        if (await page.locator('html').getAttribute('data-theme') !== theme) await clickSetting('#themeButton');
        await fitsAndSwitch();
        if (width !== 320) await shot(`admin-${width}-${theme}`);
      }
    }
    await page.setViewportSize({width: 390, height: 844});
    await page.locator('#landingToggle').click();
    await page.waitForFunction(() => document.getElementById('landingToggle').getAttribute('aria-checked') === 'false');
    assert.equal((await context.request.get(origin + '/')).status(), 404);
    assert.equal((await context.request.get(origin + '/setup')).status(), 200);
    assert.equal((await context.request.get(origin + '/s/')).status(), 404);
    assert.equal((await context.request.get(origin + '/assets/appearance.js')).status(), 200);
    assert.equal((await context.request.get(origin + '/assets/app.js')).status(), 404);
    await page.reload();
    await page.locator('#workspace').waitFor({state: 'visible'});
    await page.waitForFunction(() => !document.getElementById('landingToggle').disabled);
    assert.equal(await page.locator('#landingToggle').getAttribute('aria-checked'), 'false');
    await page.goto(origin + '/admin/landing-preview');
    await fitsAndSwitch(); await shot('landing-private-preview');
    await page.goto(origin + '/admin/');
    await page.waitForFunction(() => !document.getElementById('landingToggle').disabled);
    await page.locator('#landingToggle').click();
    await page.waitForFunction(() => document.getElementById('landingToggle').getAttribute('aria-checked') === 'true');
    await page.locator('#inviteButton').click();
    await page.locator('#ownerName').fill('Household tablet');
    await page.locator('#inviteSubmit').click();
    await page.locator('#codeDialog').waitFor({state: 'visible'});
    assert.match(await page.locator('#codeValue').textContent(), /^ost-relay-setup:/);
    await fitsAndSwitch();
    await page.locator('#codeDialog [data-close]').click();
    await page.waitForFunction(() => document.getElementById('codeValue').textContent === '');
    const enrolled = await context.request.post(origin+'/api/v1/enroll', {data:{
      enrollment_code:'open-enrollment',hardware_model:'MagBound Smart Timer V2',
      chip_family:'ESP32-C3',build_id:'aabbccddeeff00112233aabb'}});
    assert.equal(enrolled.status(),200);
    await page.locator('#refresh').click();
    await page.waitForFunction(() => !document.getElementById('clearTimers').disabled);
    // The admin page is bilingual; rows and messages are rebuilt when the language changes.
    await clickSetting('#languageButton');
    assert.equal(await page.locator('html').getAttribute('lang'), 'zh-Hans');
    await page.waitForFunction(() => document.getElementById('clearTimers').textContent === '清除所有计时器'
      && document.getElementById('setLimit').textContent.endsWith('个计时器')
      && document.querySelector('#timers .status').textContent === '离线');
    assert.equal(await page.locator('#timersTitle').textContent(), '计时器');
    assert.equal(await page.locator('#settingsOpen').getAttribute('aria-label'), '设置');
    await clickSetting('#languageButton');
    await page.waitForFunction(() => document.getElementById('clearTimers').textContent === 'Clear all timers'
      && document.getElementById('setLimit').textContent.endsWith(' timers')
      && document.querySelector('#timers .status').textContent === 'Offline');
    await page.locator('#clearTimers').click();
    await page.locator('#confirmDialog').waitFor({state:'visible'});
    assert.match(await page.locator('#confirmBody').textContent(),/Owner accounts and relay settings are kept/);
    await page.locator('#confirmDialog [data-close]').click();
    assert.equal((await context.request.get(origin+'/admin/timers',{headers:{Authorization:'Bearer '+token}})).status(),200);
    assert.equal((await (await context.request.get(origin+'/admin/timers',{headers:{Authorization:'Bearer '+token}})).json()).length,1);
    await page.locator('#clearTimers').click(); await page.locator('#confirmAction').click();
    await page.waitForFunction(() => document.getElementById('clearTimers').disabled);
    assert.equal((await (await context.request.get(origin+'/admin/timers',{headers:{Authorization:'Bearer '+token}})).json()).length,0);
    assert((await (await context.request.get(origin+'/admin/owners',{headers:{Authorization:'Bearer '+token}})).json()).some(o=>o.name==='Household tablet'));
    await clickSetting('#signOut');
    assert.equal(await page.evaluate(() => sessionStorage.getItem('ostrelayAdminToken')), null);
    for (const route of ['/', '/setup']) {
      await page.goto(origin + route);
      await clickSetting('#languageButton');
      assert.equal(await page.locator('h1').textContent(), route === '/' ? '共享计时器的控制权' : '连接您的计时器');
      await clickSetting('#languageButton');
      assert.equal(await page.locator('html').getAttribute('lang'), 'en');
      assert.equal(await page.locator('h1').textContent(), route === '/' ? 'Share control of your timer' : 'Connect your timer');
      for (const theme of ['light', 'dark']) {
        if (await page.locator('html').getAttribute('data-theme') !== theme) await clickSetting('#themeButton');
        await fitsAndSwitch();
        await shot(`${route === '/' ? 'landing' : route.replaceAll('/', '')}-${theme}`);
      }
    }
    // Owner presentation is tested signed out and signed in. Its account API
    // is a fixed UI fixture; real session/authentication checks live in pytest.
    await page.goto(origin + '/owner');
    await page.locator('#signIn').waitFor({state: 'visible'});
    await fitsAndSwitch();
    let mutations = 0;
    page.on('request', request => {
      if (request.url().startsWith(origin + '/owner/') && request.method() !== 'GET') mutations++;
    });
    const ownerEmail = 'long-owner-name'.repeat(8) + '@example.test';
    await page.route('**/owner/api/me', route => route.fulfill({status: 200, contentType: 'application/json', body: JSON.stringify({
      name: 'Your timers', email: ownerEmail, max_timers: 5,
      timers: [
        {timer_id: 'fixture', label: 'Connected', connected_now: false, last_seen_at: Math.floor(Date.now()/1000) - 120},
        {timer_id: 'fixture-online', label: 'Kitchen timer', connected_now: true, last_seen_at: Math.floor(Date.now()/1000)},
      ],
    })}));
    await page.route('**/owner/api/code', route => route.fulfill({status: 200, contentType: 'application/json',
      body: JSON.stringify({setup_code: 'disposable-owner-code'})}));
    await page.reload();
    await page.locator('#portal').waitFor({state: 'visible'});
    await page.locator('#createCode').click();
    await page.locator('#codeBox').waitFor({state: 'visible'});
    const beforeLanguage = mutations;
    await clickSetting('#languageButton');
    assert.equal(await page.locator('html').getAttribute('lang'), 'zh-Hans');
    assert.equal(await page.locator('.portal-heading h1').textContent(), '您的计时器');
    assert.match(await page.locator('#timerCount').textContent(), /2.*5/);
    assert.match(await page.locator('#timerCount').textContent(), /计时器/);
    const rows = page.locator('.timer-row');
    assert.equal(await rows.count(), 2);
    assert.match(await rows.nth(0).locator('.status').textContent(), /离线.*2 分钟前在线/);
    assert.equal(await rows.nth(0).locator('strong').textContent(), 'Connected', 'owner labels are literal');
    assert.equal(await rows.nth(1).locator('strong').textContent(), 'Kitchen timer');
    assert.equal(await rows.nth(1).locator('.status').textContent(), '在线');
    assert.equal(await page.locator('#who').textContent(), 'Your timers · ' + ownerEmail);
    assert.equal(await page.locator('#codeValue').textContent(), 'disposable-owner-code');
    assert.equal(mutations, beforeLanguage, 'language switches must not mint codes, remove timers or sign out');
    for (const width of [320, 390, 1280]) {
      await page.setViewportSize({width, height: 900});
      for (const theme of ['light', 'dark']) {
        if (await page.locator('html').getAttribute('data-theme') !== theme) await clickSetting('#themeButton');
        await fitsAndSwitch();
        const aligned = await page.evaluate(() => {
          const header = document.querySelector('.topbar').getBoundingClientRect();
          const portal = document.querySelector('.portal').getBoundingClientRect();
          return Math.abs(header.x - portal.x) < 1 && Math.abs(header.width - portal.width) < 1;
        });
        assert(aligned, 'owner content and header must share the same width');
        const timerRows = await rows.evaluateAll(nodes => nodes.map(row => {
          const name = row.querySelector('strong').getBoundingClientRect();
          const status = row.querySelector('.status').getBoundingClientRect();
          const button = row.querySelector('button'), style = getComputedStyle(button), box = button.getBoundingClientRect();
          const dot = getComputedStyle(row.querySelector('.status'), '::before');
          return {statuses: row.querySelectorAll('.status').length, below: status.y >= name.bottom,
            aligned: Math.abs(status.x - name.x) < 1, border: parseFloat(style.borderTopWidth),
            background: style.backgroundColor, height: box.height, right: box.right, dotWidth: parseFloat(dot.width)};
        }));
        for (const row of timerRows) {
          assert(row.statuses === 1 && row.below && row.aligned && row.dotWidth === 7,
            'each timer has one dot status below its name');
          assert(row.border >= 1 && row.background !== 'rgba(0, 0, 0, 0)' && row.height >= 44 && row.right <= width,
            'Remove must be a filled, bordered button that fits on mobile');
        }
        await openSettings();
        const signOut = await page.locator('#signOut').evaluate(node => {
          const style = getComputedStyle(node), box = node.getBoundingClientRect();
          return {border: parseFloat(style.borderTopWidth), height: box.height, x: box.x, right: box.right};
        });
        assert(signOut.border >= 1 && signOut.height >= 44 && signOut.x >= 0 && signOut.right <= width,
          'sign out must be a visible, bordered button');
        const language = await page.locator('#languageButton').boundingBox();
        assert(language.width >= 64 && language.width <= 84); assert.equal(language.height, 44);
        await shot(`owner-settings-${width}-${theme}-zh`);
        await page.keyboard.press('Escape');
        assert.equal(await page.locator('#signOut').isVisible(), false, 'sign out belongs inside Settings');
        assert(await page.locator('#themeButton').isVisible() && await page.locator('#languageButton').isVisible(),
          'light/dark and language stay visible with Settings closed');
        await shot(`owner-${width}-${theme}-zh`);
      }
    }
    await clickSetting('#languageButton');
    assert.equal(await page.locator('.portal-heading h1').textContent(), 'Your timers');
    assert.equal(await page.locator('#createCode').textContent(), 'Create a new code');
    const primary = page.locator('#createCode');
    const gradients = [];
    for (const palette of ['purple', 'slate', 'teal']) {
      await openSettings();
      await page.locator(`[data-palette-choice="${palette}"]`).click();
      gradients.push(await primary.evaluate(node => getComputedStyle(node).backgroundImage));
      await page.keyboard.press('Escape');
    }
    assert.equal(new Set(gradients).size, 3, 'primary buttons must follow the selected palette');
    await clickSetting('#signOut');
    await page.locator('#signIn').waitFor({state: 'visible'});
    assert.equal(await page.locator('#portal').isVisible(), false);
    assert.deepEqual(errors, []);
    console.log('Relay pages passed (' + engine + '): mobile/desktop, palette selection, light/dark, admin sign-in/invite/sign-out, persisted enrollment pause/resume, bulk-clear Cancel/Confirm, saved visibility, preview, setup, signed-in owner layout, Chinese round trip, literal labels, code retention and partner independence.');
    await context.close();
  } finally {
    if (browser) await browser.close();
    server.kill('SIGTERM');
    await new Promise(resolve => { if (server.exitCode !== null) resolve(); else server.once('exit', resolve); });
  }
})().catch(error => { console.error(error); process.exitCode = 1; });
