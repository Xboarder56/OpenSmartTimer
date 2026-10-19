// Real companion HTML, handlers, gestures and dialogs, with a synthetic
// authenticated session and GATT. Pairing and offline startup are separate gates.
'use strict';
const assert = require('node:assert/strict'), fs = require('node:fs'), path = require('node:path');
const http = require('node:http'), playwright = require('playwright');
const root = path.resolve(__dirname, '../..'), engine = process.env.OSTRELAY_BROWSER || 'chromium';
const output = path.join(root, 'tmp/relay-tests/bluetooth-ui', engine);
fs.mkdirSync(output, {recursive: true});
const fixture = `
  wireInterface();
  const stats = {writes: [], reads: 0};
  let timer = {state: 'open', remaining_ms: 0, session_duration_ms: 0, session_sequence: 7,
    controls_locked: false, emergency_input_active: false, event_revision: 5};
  Object.assign(app, {server: {connected: true, disconnect() {}}, authenticated: true,
    bootId: 42, protocolInfo: {device_id: 'synthetic-timer', device_name: 'UI fixture'},
    sessionKey: new Uint8Array(32), liveNeedsRead: false, chars: {liveStatus: 'status'}});
  const snapshot = changes => {
    timer = {...timer, ...changes}; app.liveNeedsRead = false; acceptLiveSnapshot(timer, true);
  };
  readGatt = async () => {
    stats.reads++;
    const frame = new Uint8Array(20), view = new DataView(frame.buffer);
    frame[0] = 1; frame[1] = STATE_NAMES.indexOf(timer.state);
    frame[2] = (timer.controls_locked ? 1 : 0) | (timer.emergency_input_active ? 2 : 0);
    view.setUint32(4, timer.remaining_ms, true); view.setUint32(8, timer.session_duration_ms, true);
    view.setUint32(12, timer.event_revision, true); view.setUint32(16, timer.session_sequence, true);
    return frame;
  };
  writeGatt = async () => {
    const operation = app.pendingResult.operation;
    stats.writes.push({op: operation.op, valueA: operation.valueA, requestId: operation.requestId});
    if (operation.op === 'lock') timer.controls_locked = true;
    else if (operation.op === 'adjust') {
      timer.remaining_ms = Math.max(0, timer.remaining_ms + operation.valueA);
      if (!timer.remaining_ms) Object.assign(timer, {state: 'open', controls_locked: false});
    } else throw Error('Fixture refuses unexpected timer mutation');
    timer.event_revision++;
    app.pendingResult.resolve({opcode: operation.opcode, request_id: operation.requestId,
      boot_id: app.bootId, session_sequence: timer.session_sequence, result: 'applied',
      state: timer.state, remaining_ms: timer.remaining_ms, changed: true, duplicate: false});
  };
  snapshot({});
  window.__bleUi = {app, stats, snapshot, gattQueue, invalidateLiveStatus, updateControlAvailability};
`;
const server = http.createServer((req, res) => {
  const pathname = new URL(req.url, 'http://localhost').pathname;
  if (!/^\/(bluetooth-app|web)\/[a-z0-9.-]+$/.test(pathname)) {res.writeHead(404);res.end();return;}
  const file = path.join(root, pathname);
  if (!fs.existsSync(file)) {res.writeHead(404);res.end();return;}
  let body = fs.readFileSync(file);
  if (pathname === '/bluetooth-app/app.js') {
    const source = body.toString();
    body = source.slice(0, source.lastIndexOf('initialize().catch(')) + fixture;
  }
  const types = {'.html': 'text/html', '.js': 'text/javascript', '.mjs': 'text/javascript', '.css': 'text/css', '.svg': 'image/svg+xml', '.webmanifest': 'application/manifest+json'};
  res.writeHead(200, {'Content-Type': types[path.extname(file)] || 'application/octet-stream'});res.end(body);
});
(async () => {
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  let browser;
  try {
    browser = await playwright[engine].launch({headless: true});
    const context = await browser.newContext({hasTouch: true, viewport: {width: 390, height: 950}});
    const page = await context.newPage(), errors = [];
    page.on('pageerror', error => errors.push(error.message));
    await page.goto('http://127.0.0.1:' + server.address().port + '/bluetooth-app/index.html');
    await page.waitForFunction(() => window.__bleUi && !document.getElementById('minutes').disabled);
    const drag = async (id, pixels) => {
      const input = page.locator('#' + id);await input.scrollIntoViewIfNeeded();const box = await input.boundingBox();
      const x = box.x + box.width / 2, y = box.y + box.height / 2;
      await page.mouse.move(x, y);await page.mouse.down();await page.mouse.move(x, y - pixels, {steps: 4});await page.mouse.up();
    };
    await drag('minutes', 36);assert.equal(await page.locator('#minutes').inputValue(), '17');
    await page.locator('#seconds').fill('59');await drag('seconds', 18);
    assert.equal(await page.locator('#minutes').inputValue(), '18');assert.equal(await page.locator('#seconds').inputValue(), '0');
    await page.locator('#minutes').hover();await page.mouse.wheel(0, 84);
    await page.waitForFunction(() => document.getElementById('minutes').value === '16');
    if (engine === 'chromium') {
      const touch = await context.newCDPSession(page), box = await page.locator('#minutes').boundingBox();
      const x = box.x + box.width / 2, y = box.y + box.height / 2;
      await touch.send('Input.dispatchTouchEvent', {type: 'touchStart', touchPoints: [{x, y}]});
      await touch.send('Input.dispatchTouchEvent', {type: 'touchMove', touchPoints: [{x, y: y - 36}]});
      await touch.send('Input.dispatchTouchEvent', {type: 'touchEnd', touchPoints: []});await touch.detach();
      assert.equal(await page.locator('#minutes').inputValue(), '18', 'native touch uses the same duration editor');
    }
    await page.locator('#randomMode').check();await drag('randomMin', 36);
    assert.equal(await page.locator('#randomMin').inputValue(), '12');
    await page.locator('#randomMax').hover();await page.mouse.wheel(0, 84);
    await page.waitForFunction(() => document.getElementById('randomMax').value === '28');
    await page.locator('#languageButton').click();
    assert.equal(await page.locator('html').getAttribute('lang'), 'zh-Hans');
    assert.equal(await page.locator('#randomMin').inputValue(), '12');
    assert.equal(await page.evaluate(() => __bleUi.stats.writes.length), 0, 'draft gestures and language never send GATT commands');
    await page.locator('#languageButton').click();
    // Disabled controls must refuse even a forced gesture during stale/busy state.
    await page.evaluate(() => __bleUi.invalidateLiveStatus());
    await page.locator('#randomMax').dispatchEvent('wheel', {deltaY: 84, deltaMode: 0});
    assert.equal(await page.locator('#randomMax').inputValue(), '28');
    await page.evaluate(() => {__bleUi.snapshot({}); __bleUi.app.commandBusy = true; __bleUi.updateControlAvailability();});
    await page.locator('#randomMax').dispatchEvent('wheel', {deltaY: 84, deltaMode: 0});
    assert.equal(await page.locator('#randomMax').inputValue(), '28');
    await page.evaluate(() => {__bleUi.app.commandBusy = false; __bleUi.snapshot({state: 'running', remaining_ms: 900000, session_duration_ms: 900000});});
    // Controls the owner switched off on the timer are not offered; Unlock stays.
    await page.evaluate(() => __bleUi.snapshot({pause_allowed: false, control_lock_allowed: false}));
    assert.equal(await page.locator('#pauseButton').isVisible(), false, 'pause is hidden when switched off');
    assert.equal(await page.locator('#sealButton').isVisible(), false, 'control lock is hidden when switched off');
    assert.equal(await page.locator('#stopButton').isVisible(), true, 'unlock stays available');
    await page.evaluate(() => __bleUi.snapshot({pause_allowed: true, control_lock_allowed: true}));
    assert.equal(await page.locator('#pauseButton').isVisible(), true);
    await page.locator('#sealButton').click();
    assert.equal(await page.locator('#mgmtConfirmTitle').textContent(), 'Lock software controls?');
    assert.match(await page.locator('#mgmtConfirmMessage').textContent(), /Unlock now will be disabled/);
    assert.match(await page.locator('#mgmtConfirmSafety').textContent(), /Neither is guaranteed to open the lock/);
    assert.equal(await page.locator('.shell').evaluate(element => element.inert), true);
    assert.equal(await page.locator('#mgmtConfirmCancel').evaluate(element => element === document.activeElement), true);
    await page.keyboard.press('Tab');assert.equal(await page.locator('#mgmtConfirmAccept').evaluate(element => element === document.activeElement), true);
    await page.keyboard.press('Tab');assert.equal(await page.locator('#mgmtConfirmCancel').evaluate(element => element === document.activeElement), true);
    await page.keyboard.press('Escape');await page.waitForFunction(() => !__bleUi.app.lockConfirmationPending);
    assert.equal(await page.evaluate(() => __bleUi.stats.writes.length), 0);
    assert.equal(await page.locator('#sealButton').evaluate(element => element === document.activeElement), true, 'Cancel restores the enabled trigger');
    await page.locator('#sealButton').click();await page.locator('#mgmtConfirmCancel').click();
    await page.waitForFunction(() => !__bleUi.app.lockConfirmationPending);
    assert.equal(await page.evaluate(() => __bleUi.stats.writes.length), 0);
    // A timer expiring while the operator reads the warning must not be sealed.
    await page.locator('#sealButton').click();await page.evaluate(() => __bleUi.snapshot({state: 'open', remaining_ms: 0}));
    await page.locator('#mgmtConfirmAccept').click();await page.waitForFunction(() => !__bleUi.app.lockConfirmationPending);
    assert.equal(await page.evaluate(() => __bleUi.stats.writes.length), 0);
    await page.evaluate(() => __bleUi.snapshot({state: 'paused', remaining_ms: 900000}));
    assert.equal(await page.locator('#sealButton').isDisabled(), true);
    await page.evaluate(() => __bleUi.snapshot({state: 'running'}));
    await page.locator('#languageButton').click();await page.locator('#sealButton').click();
    assert.equal(await page.locator('#mgmtConfirmTitle').textContent(), '锁定软件控制？');
    for (const width of [320, 390, 768, 1280]) {
      await page.setViewportSize({width, height: 950});
      await page.waitForFunction(() => getComputedStyle(document.getElementById('mgmtConfirm')).opacity === '1');
      const box = await page.locator('#mgmtConfirm').boundingBox();
      assert(box.x >= 0 && box.y >= 0 && box.x + box.width <= width && box.y + box.height <= 950, 'dialog fits ' + width);
      assert.equal(await page.evaluate(() => document.documentElement.scrollWidth > innerWidth), false);
      if (width === 390) await page.screenshot({path: path.join(output, 'lock-confirm-chinese.png')});
    }
    await page.locator('#mgmtConfirmAccept').click();
    await page.waitForFunction(() => !__bleUi.app.commandBusy && !__bleUi.app.lockConfirmationPending);
    assert.equal(await page.evaluate(() => __bleUi.stats.writes.length), 1);
    assert.equal(await page.locator('#sealedNotice').isVisible(), true);
    assert.equal(await page.locator('#stopButton').isVisible(), false);
    await page.evaluate(() => __bleUi.snapshot({state: 'running', controls_locked: false, remaining_ms: 900000}));
    await page.locator('[data-adjust="-1800000"]').click();
    await page.waitForFunction(() => __bleUi.app.live.state === 'open' && !__bleUi.app.commandBusy);
    assert.equal(await page.evaluate(() => __bleUi.stats.writes.length), 2);
    assert.equal(await page.locator('#fixedDuration').isVisible(), false); // Retained random draft.
    assert.equal(await page.locator('#randomFields').isVisible(), true);
    assert.deepEqual(errors, []);
    const report = {passed: true, engine, synthetic_gatt: true, page_errors: errors,
      checks: ['native-pointer-and-wheel-drafts', 'duration-carry', 'random-range-gestures', 'language-retains-draft',
        'disabled-gestures-stale-and-busy', 'lock-warning-before-send', 'cancel-and-escape-send-nothing',
        'dialog-focus-trap', 'expiry-during-confirmation-refused', 'paused-lock-disabled',
        'chinese-popup-320-to-1280', 'confirmed-lock-once', 'deduction-to-zero-opens']};
    fs.writeFileSync(path.join(output, 'RESULT.json'), JSON.stringify(report, null, 2) + '\n');
    console.log('PASS ' + engine + ': actual Bluetooth UI gestures, Chinese, lock warning/focus/Cancel/expiry and deduction to zero; synthetic GATT only.');
  } finally {
    if (browser) await browser.close();server.closeAllConnections();await new Promise(resolve => server.close(resolve));
  }
})().catch(error => {console.error(error.stack);process.exitCode = 1;});
