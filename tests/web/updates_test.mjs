import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import test from 'node:test';
import vm from 'node:vm';
import {createHash, webcrypto} from 'node:crypto';
import {File} from 'node:buffer';

const source = readFileSync(new URL('../../web/updates.js', import.meta.url), 'utf8');
function fixture(respond) {
  const requests = [];
  const context = vm.createContext({URL, TextDecoder, TextEncoder, Uint8Array, File, Response, AbortController,
    setTimeout, clearTimeout, crypto:webcrypto,
    fetch:async (url, options) => {requests.push({url, options}); return respond(url, options);},
  });
  vm.runInContext(source, context);
  return {api:context.SmartTimerUpdates, requests};
}
const image = Uint8Array.from({length:24}, (_, i) => i ? i : 0xe9);
const makeRecord = api => ({target:'magbound_v1_esp8266', release_version:'1.1.0', image_size:image.length,
  image_crc32:api.crc32(image), image_sha256:createHash('sha256').update(image).digest('hex'),
  release_signature:'ab'.repeat(64), file:'OpenSmartTimer-1.1.0-MagBoundV1-ESP8266.bin'});

test('update versions follow SemVer and reject ambiguous or injected versions', () => {
  const {api} = fixture();
  assert(api.newer('1.0.0-rc.10','1.0.0-rc.2'));
  assert(api.newer('1.0.0','1.0.0-rc.10'));
  assert(!api.newer('1.0.0-rc.10','1.0.0'));
  assert(!api.newer('1.0.0','1.0.0'));
  assert(api.newer('1.0.0-rc.1.more','1.0.0-rc.1'));
  for (const bad of ['1.0.0-rc.01','4294967296.0.0','1.0.0\n','1.0.0-a..b']) assert(!api.newer(bad,'0.0.0'));
});

test('feed validation binds model, versioned filename and bounded digests', () => {
  const {api} = fixture(); const record = makeRecord(api);
  assert.equal(api.validate(record, record.target), record);
  for (const change of [{target:'magbound_v2_esp32c3'}, {file:'../private.bin'},
    {image_size:4194305}, {image_size:1.5}, {image_sha256:'0'.repeat(63)},
    {release_signature:'0'.repeat(127)}, {release_notes:'x'.repeat(8193)}]) {
    assert.throws(() => api.validate({...record,...change}, record.target));
  }
  for (const bad of ['http://example-owner.github.io/OpenSmartTimer/', 'https://evil.example/timer/',
    'https://example-owner.github.io/OpenSmartTimer/?x=1','https://user@example-owner.github.io/OpenSmartTimer/']) assert.throws(() => api.baseUrl(bad));
});

test('signed metadata survives browser download and credentials never leave the device', async () => {
  let record;
  const {api,requests} = fixture(url => new Response(url.endsWith('.json') ? JSON.stringify(record) : image));
  record = makeRecord(api);
  const key = await webcrypto.subtle.generateKey({name:'ECDSA',namedCurve:'P-256'},true,['sign','verify']);
  const publicKey = Buffer.from(await webcrypto.subtle.exportKey('raw',key.publicKey)).toString('hex');
  record.release_signature = Buffer.from(await webcrypto.subtle.sign({name:'ECDSA',hash:'SHA-256'},key.privateKey,api.canonical(record))).toString('hex');
  const config = {configured:true,target:record.target,base_url:'https://example-owner.github.io/OpenSmartTimer/',public_key:publicKey};
  const candidate = await api.check(config,'1.0.0');
  assert(candidate.available);
  const file = await api.download(candidate);
  assert.deepEqual(new Uint8Array(await file.arrayBuffer()), image);
  assert.equal(api.metadata(record).release_signature,record.release_signature);
  assert.equal(api.metadata(record).image_sha256,record.image_sha256);
  for (const request of requests) {
    assert.equal(request.options.credentials,'omit'); assert.equal(request.options.redirect,'error');
    assert.equal(request.options.cache,'no-store'); assert(request.options.signal);
  }
  await assert.rejects(api.verifyRecord({...record,image_size:25},publicKey), /signature is invalid/);
});

test('oversized, incomplete, corrupted and failed downloads never produce an image', async () => {
  for (const data of [new Uint8Array(25), image.slice(0,23), new Uint8Array(24)]) {
    const {api} = fixture(() => new Response(data)); const record = makeRecord(api);
    await assert.rejects(api.download({record,base:'https://example-owner.github.io/OpenSmartTimer/'}));
  }
  const {api} = fixture(() => new Response('',{status:404}));
  await assert.rejects(api.boundedFetch('https://example.test',768,15000), /unavailable/);
});

test('firmware binaries and update feeds are outside the offline shell cache', () => {
  const worker = readFileSync(new URL('../../bluetooth-app/service-worker.js', import.meta.url),'utf8');
  assert.match(worker,/\.\.\/web\/updates\.js/);
  const shell = worker.slice(worker.indexOf('const APP_SHELL'),worker.indexOf('const shellUrls'));
  assert.doesNotMatch(shell,/\.bin|\/updates\//);
});

test('install confirmation names a prerelease and always carries the release notes', () => {
  const {api} = fixture();
  const stable = api.prompt({release_version: '1.1.0', release_notes: 'Fixes'});
  assert.equal(stable.eyebrow, 'Firmware update');
  assert.equal(stable.notes, 'Fixes');
  assert.doesNotMatch(stable.safety, /pre-release/i);
  assert.match(stable.safety, /^Provided as is, without warranty\. Open the timer and briefly press Reset/);
  const translated = [];
  const candidate = api.prompt({release_version: '1.1.0-rc.2'}, (key, values = {}) => {
    translated.push(key);
    return key.replace(/\{(\w+)\}/g, (_, name) => values[name]);
  });
  assert.equal(candidate.eyebrow, 'Pre-release update');
  assert.equal(candidate.title, 'Update to 1.1.0-rc.2?');
  assert.equal(candidate.notes, 'No release notes were provided.');
  assert.match(candidate.safety, /^Provided as is, without warranty\. This is a pre-release \(rc\.2\) .*briefly press Reset/);
  assert(translated.every(key => !key.includes('rc.2')), 'version text stays out of translation keys');
});

test('manual panel initialization makes no update request; checking needs a user click', async () => {
  const {api, requests} = fixture();
  const nodes = new Map(['channel', 'check', 'install', 'message'].map(name => [name, {
    value: 'stable', addEventListener(type, callback) {this[type] = callback;},
  }]));
  const root = {querySelector: selector => nodes.get(selector.match(/data-update-([a-z]+)/)[1])};
  let deviceReads = 0;
  api.panel({root, device: async () => {deviceReads += 1; return {updates: {configured: false}, version: '1.0.0'};}, install: () => {throw new Error('must not install');}});
  assert.equal(deviceReads, 0); assert.equal(requests.length, 0);
  await nodes.get('check').click();
  assert.equal(deviceReads, 1);
  assert.match(nodes.get('message').textContent, /not configured/);
  assert.equal(requests.length, 0);
});

async function panelFixture({status=200, installed='1.0.0', accept=false, armed=true, state='open', failImage=false}={}) {
  let record, imageDownloads=0, confirmations=0, deviceReads=0, installs=0;
  const {api,requests}=fixture(url => {
    if (url.startsWith('https://api.github.com/')) return new Response('[]');
    if (url.endsWith('.json')) return new Response(JSON.stringify(record),{status});
    imageDownloads++;
    return new Response(failImage ? new Uint8Array(24) : image);
  });
  record={...makeRecord(api),release_notes:'Fixes\n<script>untrusted()</script>'};
  const key=await webcrypto.subtle.generateKey({name:'ECDSA',namedCurve:'P-256'},true,['sign','verify']);
  const publicKey=Buffer.from(await webcrypto.subtle.exportKey('raw',key.publicKey)).toString('hex');
  record.release_signature=Buffer.from(await webcrypto.subtle.sign({name:'ECDSA',hash:'SHA-256'},key.privateKey,api.canonical(record))).toString('hex');
  const config={configured:true,target:record.target,base_url:'https://example-owner.github.io/OpenSmartTimer/',public_key:publicKey};
  const nodes=new Map(['check','message'].map(name=>[name,{addEventListener(type,callback){this[type]=callback;}}]));
  const root={querySelector: selector=>nodes.get(selector.match(/data-update-([a-z]+)/)[1])};
  api.panel({root,device:async()=>{deviceReads++;return {updates:config,version:installed,armed,state};},
    confirm:async shown=>{confirmations++;assert.equal(shown.release_notes,record.release_notes);return accept;},
    install:async(file,metadata)=>{installs++;assert.deepEqual(new Uint8Array(await file.arrayBuffer()),image);assert.equal(metadata.release_signature,record.release_signature);},
  });
  return {nodes,requests,counts:()=>({imageDownloads,confirmations,deviceReads,installs})};
}

test('no published or newer release reports no updates without a dialog or image', async()=>{
  for(const options of [{status:404},{installed:'1.1.0'},{installed:'1.2.0'}]) {
    const f=await panelFixture(options);await f.nodes.get('check').click();
    assert.equal(f.nodes.get('message').textContent,'No updates available.');
    assert.deepEqual(f.counts(),{imageDownloads:0,confirmations:0,deviceReads:1,installs:0});
    assert.match(f.requests[0].url,/updates\/magbound_v1_esp8266\.json$/);
    assert.equal(f.nodes.get('check').disabled,false);
  }
});

test('release-notes Cancel ends before image download; Install rechecks and sends authenticated bytes',async()=>{
  const cancel=await panelFixture();await cancel.nodes.get('check').click();
  assert.equal(cancel.nodes.get('message').textContent,'Update cancelled.');
  assert.deepEqual(cancel.counts(),{imageDownloads:0,confirmations:1,deviceReads:1,installs:0});
  const install=await panelFixture({accept:true});await install.nodes.get('check').click();
  assert.deepEqual(install.counts(),{imageDownloads:1,confirmations:1,deviceReads:2,installs:1});
});

test('Install refuses unarmed, closed, or corrupted firmware and check failures are errors',async()=>{
  for(const options of [{accept:true,armed:false},{accept:true,state:'running'}]) {
    const f=await panelFixture(options);await f.nodes.get('check').click();
    assert.match(f.nodes.get('message').textContent,/briefly press Reset/);
    assert.equal(f.counts().imageDownloads,0);assert.equal(f.counts().installs,0);
  }
  const corrupt=await panelFixture({accept:true,failImage:true});await corrupt.nodes.get('check').click();
  assert.match(corrupt.nodes.get('message').textContent,/checksum/);assert.equal(corrupt.counts().installs,0);
  const unavailable=await panelFixture({status:503});await unavailable.nodes.get('check').click();
  assert.match(unavailable.nodes.get('message').textContent,/unavailable/);
  assert.equal(unavailable.counts().confirmations,0);
});

test('missing Pages CORS confirms no releases through GitHub; published or unknown releases preserve failure',async()=>{
  const config={configured:true,target:'magbound_v1_esp8266',base_url:'https://example-owner.github.io/OpenSmartTimer/'};
  for (const feedError of ['cors','404']) {
    for (const releaseReply of ['[]','[{"tag_name":"v1.1.0"}]','{"message":"rate limited"}']) {
      const {api,requests}=fixture(url=>{
        if(url.startsWith('https://api.github.com/')) return new Response(releaseReply);
        if(feedError==='cors') throw new TypeError('Failed to fetch');
        return new Response('',{status:404});
      });
      if(releaseReply==='[]') assert.equal((await api.check(config,'1.0.0')).available,false);
      else await assert.rejects(api.check(config,'1.0.0'),/Failed to fetch|unavailable/);
      assert.equal(requests.length,2);
      assert.equal(requests[1].url,'https://api.github.com/repos/example-owner/OpenSmartTimer/releases?per_page=1');
      assert.equal(requests[1].options.credentials,'omit');
    }
  }
  const {api}=fixture(url=>{throw new TypeError('Offline');});
  await assert.rejects(api.check(config,'1.0.0'),/Offline/);
});

test('private or unpublished GitHub repositories have no public updates; API outages and rate limits remain errors',async()=>{
  const config={configured:true,target:'magbound_v1_esp8266',base_url:'https://example-owner.github.io/OpenSmartTimer/'};
  for(const feedError of ['cors','404'])for(const apiStatus of [404,403,503]) {
    const {api,requests}=fixture(url=>{
      if(url.startsWith('https://api.github.com/'))return new Response('{"message":"unavailable"}',{status:apiStatus});
      if(feedError==='cors')throw new TypeError('Failed to fetch');
      return new Response('',{status:404});
    });
    if(apiStatus===404)assert.equal((await api.check(config,'1.0.0')).available,false);
    else await assert.rejects(api.check(config,'1.0.0'),/Failed to fetch|unavailable/);
    assert.equal(requests.length,2);assert.equal(requests[1].options.credentials,'omit');
  }
});

// The page-open notice: browser-only, once a day, only while open, silent on errors.
function noticeFixture({respond, stored = {}, config = true} = {}) {
  let now = 1_000_000_000;
  const store = new Map(Object.entries(stored).map(([k, v]) => [k, JSON.stringify(v)]));
  const requests = [];
  const listeners = {};
  const element = name => ({textContent: '', addEventListener: (type, fn) => { listeners[name] = fn; }});
  const parts = {'[data-update-notice-title]': element('title'), '[data-update-notice-view]': element('view'),
    '[data-update-notice-dismiss]': element('dismiss')};
  const root = {hidden: true, classList: {toggle(_, hide) { root.hidden = hide; }}, querySelector: s => parts[s]};
  const toggle = {checked: false, addEventListener: (type, fn) => { listeners.toggle = fn; }};
  class FakeDate extends Date { static now() { return now; } }
  const context = vm.createContext({URL, TextDecoder, TextEncoder, Uint8Array, File, Response, AbortController,
    setTimeout, clearTimeout, Date: FakeDate,  // No WebCrypto: a plain-HTTP timer page.
    localStorage: {getItem: k => store.get(k) ?? null, setItem: (k, v) => store.set(k, v)},
    fetch: async url => { if (url.includes('/updates/')) requests.push(url); return respond(url); }});
  vm.runInContext(source, context);
  const feed = {configured: true, target: 'magbound_v1_esp8266', base_url: 'https://example-owner.github.io/OpenSmartTimer/', public_key: '04' + 'ab'.repeat(64)};
  let viewed = 0;
  const notice = context.SmartTimerUpdates.notice({root, toggle, view: () => { viewed += 1; },
    config: config ? async () => feed : undefined, translate: (s, v) => s.replace('{version}', v?.version)});
  return {notice, root, toggle, parts, listeners, requests, store, feed, viewed: () => viewed,
    advance: ms => { now += ms; }};
}
const feedRecord = version => ({target: 'magbound_v1_esp8266', release_version: version, image_size: 24,
  image_crc32: '0'.repeat(8), image_sha256: '0'.repeat(64), release_signature: 'ab'.repeat(64),
  file: `OpenSmartTimer-${version}-MagBoundV1-ESP8266.bin`});

test('page-open notice shows a newer release only while the timer is open, and Later hides that version', async () => {
  const f = noticeFixture({respond: () => new Response(JSON.stringify(feedRecord('1.0.1')))});
  await f.notice.status({version: '1.0.0', state: 'running'});
  assert.equal(f.requests.length, 0, 'no check during a session');
  await f.notice.status({state: 'open'});
  assert.equal(f.requests.length, 1);
  assert.equal(f.root.hidden, false);
  assert.equal(f.parts['[data-update-notice-title]'].textContent, 'Update available: 1.0.1');
  await f.notice.status({state: 'running'});
  assert.equal(f.root.hidden, true, 'hidden as soon as a session starts');
  await f.notice.status({state: 'open'});
  f.listeners.view(); assert.equal(f.viewed(), 1);
  f.listeners.dismiss();
  assert.equal(f.root.hidden, true);
  assert.equal(JSON.parse(f.store.get('smarttimer.updateNotice')).dismissed, '1.0.1');
  await f.notice.status({version: '1.0.1', state: 'open'});
  assert.equal(f.root.hidden, true, 'nothing to show once installed');
});

test('page-open notice checks at most once a day, once per page offline, and never when switched off', async () => {
  let fail = true;
  const f = noticeFixture({respond: () => { if (fail) throw new TypeError('offline'); return new Response(JSON.stringify(feedRecord('1.0.0'))); }});
  await f.notice.status({version: '1.0.0', state: 'open'});
  await f.notice.status({state: 'open'});
  assert.equal(f.requests.length, 1, 'one attempt per page load even when offline');
  assert.equal(f.root.hidden, true, 'errors stay silent');
  fail = false;
  const g = noticeFixture({respond: () => new Response(JSON.stringify(feedRecord('1.0.0'))),
    stored: {'smarttimer.updateNotice': {checked_at: 1_000_000_000 - 3_600_000}}});
  await g.notice.status({version: '1.0.0', state: 'open'});
  assert.equal(g.requests.length, 0, 'checked an hour ago');
  const h = noticeFixture({respond: () => new Response('{}'), stored: {'smarttimer.updateNotice': {enabled: false}}});
  assert.equal(h.toggle.checked, false);
  await h.notice.status({version: '1.0.0', state: 'open'});
  assert.equal(h.requests.length, 0, 'switched off');
});

test('Bluetooth notice waits for the page to supply the update config instead of fetching it', async () => {
  const f = noticeFixture({respond: () => new Response(JSON.stringify(feedRecord('1.0.1'))), config: false});
  await f.notice.status({version: '1.0.0', state: 'open'});
  assert.equal(f.requests.length, 0);
  await f.notice.status({version: '1.0.0', state: 'open', updates: f.feed});
  assert.equal(f.requests.length, 1);
  assert.equal(f.root.hidden, false);
});
