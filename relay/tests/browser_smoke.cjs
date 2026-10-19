// Real browser flow against stack_check.py's synthetic localhost timer only.
'use strict';
const assert = require('node:assert/strict');
const path = require('node:path');
const playwright = require('playwright');

(async () => {
  let target = new URL(process.env.OSTRELAY_TEST_BROWSER_URL || 'about:blank');
  assert.equal(target.hostname, 'localhost', 'browser smoke accepts only the generated local test stack');
  assert.equal(target.protocol, 'https:');
  const browser = await playwright.chromium.launch({ headless: true });
  const context = await browser.newContext({ ignoreHTTPSErrors: true, viewport: { width: 390, height: 844 }, colorScheme: 'dark' });
  const page = await context.newPage();
  let ownerPage;
  const errors = [];
  page.on('pageerror', e => errors.push(e.message));
  const online = () => page.waitForFunction(() => document.getElementById('connectionState').textContent === 'online', null, { timeout: 10000 });
  const result = text => page.locator('#lastResult').filter({ hasText: text }).waitFor({ state: 'visible', timeout: 10000 });
  try {
    if (process.env.OSTRELAY_TEST_CPP_FIXTURE) {
      const fixture = JSON.parse(require('node:fs').readFileSync(process.env.OSTRELAY_TEST_CPP_FIXTURE,'utf8'));
      const cppPage = await context.newPage();
      cppPage.on('pageerror', e => errors.push(e.message));
      // Load the actual SPA without exchanging another grant or changing cookies.
      await cppPage.goto(target.origin + '/s/');
      await cppPage.locator('#endView').waitFor({state:'visible'});
      const verified = await cppPage.evaluate(async data => {
        for (const frame of data.envelopes) {
          if (!parseResponseFrame(frame,frame.type)) return false;
          if (parseResponseFrame({...frame,sig:'A'.repeat(86)},frame.type)) return false;
          if (parseResponseFrame({...frame,upstream_seq:0},frame.type)) return false;
        }
        return true;
      }, fixture);
      assert(verified, 'Chromium must parse authenticated C++ response bodies and reject malformed/legacy shapes');
      await cppPage.close();
    }
    if (process.env.OSTRELAY_TEST_OWNER_URL) {
      const ownerTarget = new URL(process.env.OSTRELAY_TEST_OWNER_URL);
      assert.equal(ownerTarget.hostname, '127.0.0.1');
      ownerPage = await context.newPage();
      ownerPage.on('pageerror', e => errors.push(e.message));
      await ownerPage.goto(ownerTarget.href);
      assert.equal(await ownerPage.title(), 'Smart Timer');
      assert.equal(new URL(ownerPage.url()).hash, '');
      assert.equal(await ownerPage.locator('select').count(), 0);
      // Browser-level availability states, with the real creator DOM/styles.
      let disconnected = true;
      await ownerPage.route('**/api/links', async route => {
        if (route.request().method()==='GET' && disconnected)
          return route.fulfill({status:200,contentType:'application/json',body:JSON.stringify({connected:false,can_share:false,links:[]})});
        return route.continue();
      });
      await ownerPage.reload();
      await ownerPage.waitForFunction(() => document.getElementById('shareControls').disabled);
      assert(await ownerPage.locator('#generate').isDisabled());
      assert.match(await ownerPage.locator('#sharingUnavailable').textContent(),/Connect the timer/);
      await ownerPage.screenshot({path:path.join(path.dirname(process.env.OSTRELAY_TEST_SCREENSHOT),'creator-offline.png'),fullPage:true,animations:'disabled'});
      disconnected = false;
      await ownerPage.waitForFunction(() => !document.getElementById('shareControls').disabled);
      await ownerPage.locator('#lifetime').fill('15');
      await ownerPage.locator('#label').fill('Browser partner');
      let creates = 0, firstRequest;
      await ownerPage.route('**/api/links', async route => {
        if (route.request().method() !== 'POST') return route.continue();
        const body = route.request().postDataJSON();
        assert.equal(body.ttl_seconds, 900);
        assert.equal(body.scopes, undefined);
        creates++;
        if (creates === 1) { firstRequest = body; await route.fetch(); await route.abort('failed'); }
        else { assert.deepEqual(body, firstRequest); await route.continue(); }
      });
      await ownerPage.locator('#generate').click();
      await ownerPage.waitForFunction(() => !document.getElementById('generate').disabled);
      await ownerPage.reload();
      assert.equal(await ownerPage.locator('#lifetime').inputValue(), '15');
      await ownerPage.locator('#generate').click();
      const ready = ownerPage.locator('.link-card[data-state="ready"]').filter({hasText:'Browser partner'});
      await ready.waitFor({timeout:15000});
      assert.equal(await ready.count(), 1);
      assert.equal(creates, 2);
      target = new URL(await ready.locator('input').inputValue());
      await context.grantPermissions(['clipboard-read','clipboard-write'], {origin:ownerTarget.origin});
      await ready.getByRole('button', {name:'Copy link', exact:true}).click();
      assert.equal(await ownerPage.evaluate(() => navigator.clipboard.readText()), target.href);
      // Redact the bearer input in screenshots, while testing the real copy above.
      const artifacts = path.dirname(process.env.OSTRELAY_TEST_SCREENSHOT);
      await ownerPage.screenshot({path:path.join(artifacts,'creator-mobile.png'), fullPage:true, animations:'disabled', mask:[ready.locator('input')],maskColor:'#6d28d9'});
      for (const width of [320,1280]) {
        await ownerPage.setViewportSize({width,height:900});
        assert(await ownerPage.evaluate(() => document.documentElement.scrollWidth <= innerWidth));
      }
      await ownerPage.screenshot({path:path.join(artifacts,'creator-desktop.png'),fullPage:true,animations:'disabled', mask:[ready.locator('input')],maskColor:'#6d28d9'});
      await ownerPage.locator('#theme').click();
      await ownerPage.screenshot({path:path.join(artifacts,'creator-light.png'),fullPage:true,animations:'disabled', mask:[ready.locator('input')],maskColor:'#6d28d9'});
    }
    await page.goto(target.href);
    assert.equal(await page.title(), 'Smart Timer');
    await online();
    assert.equal(new URL(page.url()).hash, '');
    await page.locator('#minutes').fill('3');
    await page.locator('[data-op="start"]').click();
    await result('Start · applied');
    await page.locator('[data-op="pause"]').waitFor({ state: 'visible' });

    // Deliver one adjustment to the real server, then lose its HTTP reply.
    let sent = 0;
    let adjustmentDelivered = false;
    const relayTraffic=[];
    page.on('request',request => {
      if(request.url().includes('/api/v1/'))relayTraffic.push(request.postData() || request.url());
    });
    await page.route('**/api/v1/command', async route => {
      const body = route.request().postDataJSON();
      const adjustment = await page.evaluate(() => state.pendingIntents.find(i=>i.op==='adjust')?.op_id);
      if (!adjustmentDelivered && body.request_id === adjustment) {
        sent += 1;
        // Lose both delivery paths: otherwise SSE correctly resolves the
        // command even when its HTTP response is lost.
        await page.evaluate(() => closeWatch());
        const delivered = await route.fetch();
        assert.equal(delivered.status(), 200);
        const outer=await delivered.json();
        assert.equal(outer.type,'e2e');assert.equal(outer.body_json,undefined);
        adjustmentDelivered = true;
        await route.abort('failed');
      } else await route.continue();
    });
    await page.locator('[data-op="adjust"][data-adjust="60000"]').click();
    await result('Adjust outcome unknown');
    assert(adjustmentDelivered, 'the lost response test must deliver the adjustment before reload');
    await page.locator('#pendingCard').waitFor({ state: 'visible' });
    await page.reload();
    await online();
    // Opening SSE automatically performs a read-only result lookup.
    await result('Adjust · applied');
    await page.locator('#pendingCard').waitFor({ state: 'hidden', timeout: 10000 });
    assert.equal(sent, 1, 'refresh and reconciliation must not resend the adjustment');
    await page.locator('[data-op="pause"]').click();
    await result('Pause · applied');
    await page.locator('[data-op="resume"]').waitFor({ state: 'visible' });
    const artifacts = process.env.OSTRELAY_TEST_SCREENSHOT && path.dirname(process.env.OSTRELAY_TEST_SCREENSHOT);
    const fits = async () => {
      assert(await page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth), 'layout must fit viewport');
      assert(await page.evaluate(() => {
        const brand = document.querySelector('.brand').getBoundingClientRect();
        const gear = document.getElementById('settingsOpen').getBoundingClientRect();
        return Boolean(document.querySelector('.topbar-actions > #themeButton'))
          && Boolean(document.querySelector('.topbar-actions > #languageButton'))
          && Math.abs((brand.top + brand.bottom - gear.top - gear.bottom) / 2) < 1;
      }), 'light/dark and language must remain directly accessible beside the settings gear');
    };
    await fits();
    if (artifacts) await page.screenshot({ path: process.env.OSTRELAY_TEST_SCREENSHOT, fullPage: true, animations: 'disabled' });
    await page.setViewportSize({ width: 1280, height: 900 });
    await fits();
    assert(await page.locator('#leftRail').isVisible());
    assert(await page.locator('#rightRail').isVisible());
    if (artifacts) await page.screenshot({ path: path.join(artifacts, 'partner-desktop.png'), fullPage: true, animations: 'disabled' });
    await page.setViewportSize({ width: 320, height: 844 });
    await fits();
    await page.setViewportSize({ width: 390, height: 844 });
    await page.locator('#themeButton').click();
    assert.equal(await page.locator('html').getAttribute('data-theme'), 'light');
    // Shared stylesheet: pause/resume are neutral (light --text), not red.
    await page.waitForFunction(() => getComputedStyle(document.querySelector('[data-op="resume"]')).color === 'rgb(36, 25, 42)');
    if (artifacts) await page.screenshot({ path: path.join(artifacts, 'partner-light.png'), fullPage: true, animations: 'disabled' });
    await page.locator('#themeButton').click();
    await page.keyboard.press('Escape');
    await page.locator('[data-op="resume"]').click();
    await result('Resume · applied');
    await page.locator('[data-op="release"]').click();
    await result('Release · applied');
    await page.locator('[data-op="start"]').waitFor({ state: 'visible' });
    await fits();
    if (artifacts) await page.screenshot({ path: path.join(artifacts, 'partner-open.png'), fullPage: true, animations: 'disabled' });
    // Disconnect ends one tab, without releasing the timer or revoking its link.
    const previousSession = await page.evaluate(() => state.status.session_id);
    await page.locator('[data-op="start"]').click();
    await result('Start · applied');
    // The result can show before the status that carries the new session ID.
    await page.waitForFunction(previous => state.fresh && state.status.state === 'running'
      && state.status.session_id !== previous, previousSession);
    const admission = await page.evaluate(() => ({handle:state.sessionHandle,relay:state.relayOrigin,session:state.status.session_id}));
    const observer = await context.newPage();
    observer.on('pageerror', error => errors.push(error.message));
    await observer.goto(target.href);
    await observer.waitForFunction(() => state.fresh && state.status.state === 'running');
    const observerHandshakeWallMs = Date.now();
    await page.locator('#settingsOpen').click();
    await page.locator('#disconnectButton').click();
    await page.locator('#endView').waitFor({state:'visible'});
    assert.equal(await page.locator('#endTitle').textContent(), 'Disconnected');
    assert(await page.evaluate(() => !state.channel && state.closed && !state.token && !state.sessionHandle
      && sessionStorage.getItem('ost-relay-tab-v1') === null));
    let removed = false;
    for (let attempt = 0; attempt < 50; attempt++) {
      removed = (await context.request.get(admission.relay+'/api/v1/session', {headers:{'X-OST-Session':admission.handle}})).status() === 401;
      if (removed) break;
      await new Promise(resolve => setTimeout(resolve,100));
    }
    assert(removed, 'Disconnect must release this tab’s relay admission');
    const observed = await observer.evaluate(async () => {
      await syncStatus(); await syncStatus();
      return state.fresh && state.status.state === 'running' && state.status.session_id;
    });
    const seen = await observer.evaluate(() => JSON.stringify({fresh: state.fresh, closed: state.closed,
      expired: state.expired, status: state.status && {state: state.status.state, session: state.status.session_id,
      reason: state.status.release_reason, remaining: state.status.remaining_ms}}));
    assert.equal(observed, admission.session,
      `another browser must still verify the same running timer session (expected ${admission.session}; saw ${seen})`);
    await observer.locator('[data-op="release"]').click();
    await observer.waitForFunction(() => state.fresh && state.status.state === 'open');
    await observer.close();
    await page.reload();
    await page.locator('#endView').waitFor({state:'visible'});
    assert.equal(await page.locator('#endTitle').textContent(), 'No share link', 'refresh must not silently reconnect after Disconnect');
    // The timer bounds fresh key exchanges to one per second across all tabs.
    const handshakeDelay = 1100 - (Date.now() - observerHandshakeWallMs);
    if (handshakeDelay > 0) await new Promise(resolve => setTimeout(resolve, handshakeDelay));
    await page.goto(target.href); await online();
    if (ownerPage) {
      await ownerPage.locator('.link-card').filter({hasText:'Browser partner'}).getByRole('button', {name:'Revoke',exact:true}).click();
      await ownerPage.locator('.link-card[data-state="revoked"]').filter({hasText:'Browser partner'}).waitFor({timeout:10000});
      await page.locator('#liveView').waitFor({state:'hidden',timeout:10000});
      assert.equal(await ownerPage.getByRole('button',{name:'Copy link',exact:true}).count(),0);
    }
    assert.deepEqual(errors, []);
    const fragment = new URL(target).hash.slice(1);
    const bearer = new URLSearchParams(fragment).get('cap') || fragment;
    const payload=JSON.parse(Buffer.from(bearer.split('.')[2],'base64url'));
    for(const text of relayTraffic) {
      assert(!text.includes(bearer));assert(!text.includes(payload.fp));
    }
    console.log(JSON.stringify({ passed: true, engine: 'Chromium', viewport: '390x844',
      checks: ['cpp-authenticated-response-parser', 'verified-identity-handshake', 'verified-aead-live-status', 'start', 'lost-response', 'refresh-resume', 'result-reconciliation-without-resend', 'pause', 'resume', 'release', 'inline-duration', 'responsive-320-390-1280', 'settings-gear', 'theme-switch', 'disconnect-without-timer-mutation-or-grant-revocation', 'creator-timed-link', 'creator-offline-disabled', 'creator-lost-http-idempotent-retry', 'lost-token-ack-retry', 'creator-copy-link', 'creator-revoke-partner-session'],
      page_errors: errors.length }));
  } finally {
    await context.close();
    await browser.close();
  }
})().catch(error => { console.error(error.stack); process.exitCode = 1; });
