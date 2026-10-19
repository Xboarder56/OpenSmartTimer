// Actual local UI against an isolated preview and synthetic physical gestures.
// No hardware, real enrollment, credentials or production endpoint is used.
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs'), path = require('node:path');
const net = require('node:net'), {spawn} = require('node:child_process');
const {chromium} = require('playwright');
const root = path.resolve(__dirname, '../..');
const out = path.join(root, 'tmp/relay-tests/setup-approval');
fs.mkdirSync(out, {recursive:true});
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
async function preview() {
  const listener = net.createServer();
  await new Promise(resolve => listener.listen(0, '127.0.0.1', resolve));
  const port = listener.address().port;
  await new Promise(resolve => listener.close(resolve));
  const child = spawn(process.env.PYTHON || 'python3',
    [path.join(root, 'tools/preview_ui.py'), '--port', String(port), '--v2'], {cwd:root, stdio:'ignore'});
  const origin = 'http://127.0.0.1:'+port;
  for (let i=0; i<100; i++) {
    try { if ((await fetch(origin+'/api/v1/status')).ok) return {child, origin}; } catch {}
    await sleep(100);
  }
  child.kill(); throw Error('Preview failed to start');
}
(async () => {
  const {child, origin} = await preview();
  const browser = await chromium.launch({headless:true});
  try {
    const base = await (await fetch(origin+'/api/v1/relay')).json();
    const relayOrigin = 'https://'+'a'.repeat(51)+'.example.com:8443';
    const partnerOrigin = 'https://independent-control.'+'b'.repeat(31)+'.example.com:8444';
    // A still-connected old enrollment must never count as new setup success.
    let fixture = {...base, enrolled:true, enabled:true, running:true,
      state:'connected', sharing_available:true, partner_origin:''};
    let setupPosts = 0, cancelPosts = 0, approvalPosts = 0;
    const errors = [];
    const page = await browser.newPage({viewport:{width:390, height:950}});
    page.on('pageerror', error => errors.push(error.message));
    const fulfill = (route, value, status=200) => route.fulfill({status,
      contentType:'application/json', body:JSON.stringify(value)});
    await page.route('**/api/v1/relay', route => fulfill(route, fixture));
    await page.route('**/api/v1/relay/links', route => fulfill(route, {links:[]}));
    await page.route('**/api/v1/relay/enroll', route => {
      setupPosts++;
      const fields = Object.fromEntries(new URLSearchParams(route.request().postData()));
      assert.equal(fields.if_boot_id, base.boot_id);
      assert.equal(fields.relay_url, relayOrigin);
      assert.equal(fields.partner_origin, partnerOrigin);
      fixture = {...fixture, setup_pending:true, setup_state:'pending',
        setup_request_id:setupPosts, setup_remaining_ms:60000,
        setup_relay_origin:fields.relay_url, setup_partner_origin:fields.partner_origin};
      return fulfill(route, fixture, 202);
    });
    await page.route('**/api/v1/relay/enroll/cancel', route => {
      cancelPosts++;
      const fields = Object.fromEntries(new URLSearchParams(route.request().postData()));
      assert.equal(fields.setup_request_id, String(fixture.setup_request_id));
      assert.equal(fields.if_boot_id, base.boot_id);
      fixture = {...fixture, setup_pending:false, setup_state:'cancelled'};
      return fulfill(route, fixture);
    });
    page.on('request', req => { if (/enroll\/(confirm|approve)/.test(req.url())) approvalPosts++; });
    await page.goto(origin+'/');
    await page.locator('#settingsOpen').click();
    await page.locator('#relayEnrollForm').waitFor({state:'visible'});
    assert(await page.locator('#relayManage').isHidden(), 'legacy settings require approval');
    const setup = 'ost-relay-setup:'+Buffer.from(JSON.stringify({url:relayOrigin,
      partner_origin:partnerOrigin, code:'synthetic-enrollment', pin:'A'.repeat(43)})).toString('base64url');
    const submit = async () => {
      await page.locator('#relaySetupCode').fill(setup);
      await page.locator('#relayEnrollButton').click();
      await page.locator('#relaySetupApproval').waitFor({state:'visible'});
      assert.equal(await page.locator('#relayApprovalRelay').textContent(), relayOrigin);
      assert.equal(await page.locator('#relayApprovalPartner').textContent(), partnerOrigin);
      assert.equal(await page.locator('#relaySetupCode').inputValue(), '');
    };
    await submit();
    await sleep(1100);
    assert(!/Relay connected/.test(await page.locator('#relayMessage').textContent()), 'old connection is not setup success');
    assert(!await page.locator('#relaySetupCancel').isDisabled(), 'Cancel remains usable during polling');
    for (const width of [320,390,768,1280]) {
      await page.setViewportSize({width,height:950});
      assert(!await page.evaluate(() => document.documentElement.scrollWidth > innerWidth));
      for (const id of ['relayApprovalRelay','relayApprovalPartner']) {
        assert(await page.locator('#'+id).evaluate(el => {
          const bounds=el.getBoundingClientRect(), range=document.createRange();
          range.selectNodeContents(el);
          return [...range.getClientRects()].every(rect => rect.left >= bounds.left-1 &&
            rect.right <= bounds.right+1 && rect.top >= bounds.top-1 && rect.bottom <= bounds.bottom+1);
        }), `${id}: the full hostname and port must fit without ellipsis at ${width}px`);
      }
    }
    await page.setViewportSize({width:390,height:950});
    await page.locator('#relaySetupApproval').scrollIntoViewIfNeeded();
    await page.screenshot({path:path.join(out, 'pending.png'), fullPage:true});
    await page.locator('#relaySetupCancel').click();
    await page.locator('#relaySetupApproval').waitFor({state:'hidden'});
    await page.waitForFunction(() => !document.getElementById('relayEnrollButton').disabled);
    await submit();
    fixture = {...fixture, setup_state:'expired', setup_pending:false};
    await page.waitForFunction(() => document.getElementById('relayMessage').textContent.includes('approval expired'));
    await page.waitForFunction(() => !document.getElementById('relayEnrollButton').disabled);
    await submit();
    // Synthetic input only: actual firmware exposes no HTTP approval route.
    fixture = {...fixture, setup_state:'complete', setup_pending:false,
      partner_origin:partnerOrigin};
    await page.waitForFunction(() => document.getElementById('relayMessage').textContent.includes('Relay connected'));
    await page.locator('#relayManage').waitFor({state:'visible'});
    assert.equal(setupPosts, 3); assert.equal(cancelPosts, 1); assert.equal(approvalPosts, 0);
    assert.deepEqual(errors, []);
    const report = {passed:true, checks:['bundled-runtime-origin', 'legacy-reapproval',
      'no-old-connection-success', 'immutable-device-address-review', 'cancel-while-polling',
      'expired-approval', 'physical-completion-only', '320-1280-layout',
      'complete-long-origin-and-port-review', 'no-browser-errors']};
    fs.writeFileSync(path.join(out,'RESULT.json'), JSON.stringify(report,null,2)+'\n');
    console.log(JSON.stringify(report));
  } finally {await browser.close(); child.kill();}
})().catch(error => {console.error(error); process.exitCode=1;});
