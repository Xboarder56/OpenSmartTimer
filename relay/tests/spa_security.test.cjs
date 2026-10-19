// Run with: node --test relay/tests/spa_security.test.cjs
// Authenticated response parsing and UI state; separate ECDH/AEAD tests cover real cryptography.
const { test } = require('node:test');
const assert = require('node:assert/strict');
const { readFileSync } = require('node:fs');
const { join } = require('node:path');
const vm = require('node:vm');
const { createECDH, webcrypto } = require('node:crypto');
const source = readFileSync(join(__dirname, '../spa/app.js'), 'utf8');
// A synthetic compressed identity anchor; the crypto gate verifies handshakes.
const ecdh = createECDH('prime256v1'); ecdh.generateKeys();
const publicKey = ecdh.getPublicKey(null, 'compressed').toString('base64url');
const snapshot = (overrides = {}) => ({
  boot_id: 'boot-a', session_id: 'session-a', state: 'running', uptime_ms: 1000,
  remaining_ms: 60000, session_duration_ms: 60000, sealed: false,
  emergency_input_active: false, ...overrides,
});
const intent = (overrides = {}) => ({
  op: 'adjust', op_id: 's-0011223344556677', params: { delta_ms: 60000 },
  if_boot_id: 'boot-a', if_session_id: 'session-a', observed_uptime_ms: 1000,
  ...overrides,
});
function envelope(type, seq, body) {
  const body_json = JSON.stringify(body);
  return { type, upstream_seq: seq, body_json };
}
function result(original, overrides = {}) {
  return { op: original.op, op_id: original.op_id, code: 'applied',
    mutation: { action: original.op, operation_id: original.op_id, changed: true, duplicate: false },
    status: snapshot(), ...overrides };
}
class Element {
  constructor() {
    const classes = new Set();
    this.classList = { add: (...items) => items.forEach((i) => classes.add(i)),
      remove: (...items) => items.forEach((i) => classes.delete(i)),
      toggle: (item, enabled) => enabled ? classes.add(item) : classes.delete(item),
      contains: (item) => classes.has(item) };
    this.style = { setProperty() {} }; this.dataset = {}; this.children = []; this.textContent = '';
    this.listeners = {};
  }
  replaceChildren(...items) { this.children = items; }
  append(...items) { this.children.push(...items); }
  appendChild(item) { this.children.push(item); }
  addEventListener(type, callback) { this.listeners[type] = callback; }
  removeEventListener() {}
  setAttribute(name, value) { this[name] = value; }
  focus() {}
  blur() {}
  closest() { this.label ||= new Element(); return this.label; }
}
async function harness({ storage = new Map(), hash = '', pathname = '/s/',
  search = '?relay='+Buffer.from('https://relay.test').toString('base64url') } = {}) {
  const elements = new Map();
  const get = (id) => {
    if (!elements.has(id)) elements.set(id, new Element());
    return elements.get(id);
  };
  const buttons = [['adjust', 'adjust'], ['release', 'release'], ['start', 'start'], ['lock_controls', 'lock']].map(([op, scope]) => {
    const button = new Element(); button.dataset = { op, scope, adjust: '60000' }; return button;
  });
  const sources = [];
  const timers = new Map();
  let timerId = 0;
  const context = vm.createContext({
    document: { documentElement: new Element(), getElementById: get, querySelector: selector => selector === 'main' ? get('main') : null, querySelectorAll: (selector) => selector === 'button[data-op]' ? buttons : [], createElement: () => new Element(), addEventListener() {} },
    matchMedia: () => ({matches: false}),
    location:{origin:'https://partner.test'}, URL, URLSearchParams, performance,
    window: { addEventListener() {}, location: { hash, pathname, search }, history: { replaceState() { context.window.location.hash = ''; } } },
    sessionStorage: { getItem: (key) => storage.get(key) || null, setItem: (key, value) => storage.set(key, value), removeItem: key => storage.delete(key) },
    crypto: webcrypto, TextEncoder, TextDecoder, Uint8Array, Uint32Array, DataView, AbortController,
    atob: (value) => Buffer.from(value, 'base64').toString('binary'),
    fetch: async () => { throw new Error('Unexpected fetch'); },
    EventSource: class {
      static CLOSED = 2;
      constructor(url) { this.url = url; this.listeners = {}; this.readyState = 0; sources.push(this); }
      addEventListener(type, callback) { this.listeners[type] = callback; }
      close() { this.readyState = 2; }
    },
    setTimeout: (callback, delay) => { timers.set(++timerId, { callback, delay }); return timerId; },
    clearTimeout: id => timers.delete(id), setInterval: () => 1, clearInterval() {},
  });
  vm.runInContext(source, context);
  const api = vm.runInContext('({state, main, readTokenFromFragmentAndClear, createSession, queueFrame, runCommand, onButton, closeLockConfirm, disconnectSession, syncStatus, saveTabState, reconcilePending, markStale, applyVerifiedStatus, updateButtonsForState, renderExpiryCountdown, openWatch, closeWatch})', context);
  // Timer UI unit tests supply a channel double; verify_secure_cpp.cjs and
  // the HTTPS Chromium gate cover real ECDH/AEAD and transport confidentiality.
  api.state.relayOrigin = 'https://relay.test';
  api.state.token = 'ost-cap.v1.'+Buffer.from(JSON.stringify({boot:'boot-a',vk:publicKey,tid:'timer-test',sc:['view','adjust','release','start'],exp_up_ms:50000})).toString('base64url')+'.tag';
  const sentBodies = [];
  api.state.channel = {
    decrypt:async frame => {
      if(frame && frame.type==='status' && !frame.request_id) {
        const active = [...api.state.syncRequests.keys()].at(-1);
        frame.request_id = active || 'unit-sync';
        if (!active) api.state.syncRequests.set('unit-sync',performance.now());
      }
      const {request_id, ...inner} = frame;
      return inner;
    },
    encrypt:async body => {
      sentBodies.push(body);
      if(body.type !== 'command')return body;
      const {type,cap_token,expiry_uptime_ms,...intent}=body;
      return intent;
    },
    close() {},
  };
  context.OSTSecure = {
    dec:value => Uint8Array.from(Buffer.from(value,'base64url')),
    open:async () => ({payload:{boot:'boot-a',tid:'timer-test'},frame:{fp_hash:'public-hash',client_id:'public-client',client_key:'public-key',tag:'mac'},accept:async () => api.state.channel}),
  };
  api.state.bootId = 'boot-a'; api.state.sessionHandle = 'abcdefghijklmnopqrstuv';
  api.state.lastStatusUptimeMs = 1000; api.state.lastStatusWallMs = Date.now();
  api.state.lastVerifiedWallMs = Date.now(); api.state.channelCreatedWallMs = Date.now();
  api.state.fresh = true;
  api.state.scopes = new Set(['view', 'adjust', 'release', 'start']);
  return { ...api, context, elements, get, buttons, storage, sources, timers, sentBodies };
}
function response(status, body) { return { status, ok: status >= 200 && status < 300, json: async () => body }; }

test('first authenticated uptime corrects displayed link expiry without an interval tick', async () => {
  const h = await harness();
  const wall = 1700000000000;
  h.context.Date = class extends Date { static now() { return wall; } };
  h.state.lastStatusWallMs = wall;
  h.state.tokenExpUpMs = 4200000;
  h.state.fresh = false;
  h.renderExpiryCountdown();
  assert.equal(h.get('expiryCountdown').textContent, 'Expires in 1h 9m');
  assert(await h.queueFrame(envelope('status', 1, snapshot({
    state: 'open', session_id: null, uptime_ms: 600000,
  })), 'status'));
  assert.equal(h.state.fresh, true);
  assert.equal(h.get('expiryCountdown').textContent, 'Expires in 1h 0m');
});

test('public path and private fragment are bound before any relay request', async () => {
  const id = 'AAAAAAAAAAAAAAAAAAAAAA';
  const secret = 'ost-cap.v1.private.token';
  const h = await harness({pathname:'/s/'+id, search:'',
    hash:'#relay='+Buffer.from('https://relay.test').toString('base64url')+'&cap='+secret});
  const link = h.readTokenFromFragmentAndClear();
  assert.equal(h.context.window.location.hash, '');
  assert.equal(link.routeHash, id);
  assert.equal(link.relayOrigin, 'https://relay.test');
  h.state.routeHash = id;
  let requests = 0;
  h.context.fetch = async () => { requests++; return response(200,{frame:{}}); };
  await assert.rejects(h.createSession(link.token)); // mismatched route hash
  assert.equal(requests, 0);
  h.context.OSTSecure.open = async () => ({payload:{boot:'boot-a',tid:'timer-test'},frame:{fp_hash:id,client_id:'public-client'},accept:async()=>h.state.channel});
  h.context.fetch = async (url, opts) => {
    requests++;
    assert.equal(h.context.window.location.hash, '');
    assert.equal(url, 'https://relay.test/api/v1/session');
    assert(!JSON.stringify({url,opts}).includes(secret));
    return response(200,{frame:{},boot_id:'boot-a',timer_id:'timer-test',session_handle:'abcdefghijklmnopqrstuv'});
  };
  await h.createSession(link.token);
  assert.equal(requests, 1);
});

test('malformed fragment, duplicate values, query secrets and same-origin relay fail closed', async () => {
  const relay = Buffer.from('https://relay.test').toString('base64url');
  const cap = 'ost-cap.v1.private.token';
  for (const [hash, search] of [
    ['#relay='+relay+'&cap='+cap+'&cap='+cap, ''],
    ['#relay='+relay+'&relay='+relay+'&cap='+cap, ''],
    ['#relay='+relay+'&cap='+cap+'&extra=1', ''],
    ['#relay='+relay+'&cap='+cap, '?secret='+cap],
    ['#relay='+Buffer.from('https://partner.test').toString('base64url')+'&cap='+cap, ''],
  ]) {
    const h = await harness({pathname:'/s/AAAAAAAAAAAAAAAAAAAAAA', hash, search});
    assert.throws(h.readTokenFromFragmentAndClear);
    assert.equal(h.context.window.location.hash, '');
  }
});

test('closed watches reopen with one bounded backoff timer and no control replay', async () => {
  const h = await harness();
  h.context.fetch = async () => response(200, { session_handle: 'abcdefghijklmnopqrstuv', boot_id: 'boot-a' });
  h.state.pendingIntents = [intent()];
  h.openWatch();
  const delays = [];
  for (let attempt = 0; attempt < 6; attempt++) {
    const es = h.sources.at(-1);
    es.readyState = 2;
    es.listeners.error();
    const id = h.state.watchRetryTimer;
    es.listeners.error();
    assert.equal(h.state.watchRetryTimer, id, 'duplicate error must not add a retry');
    const timer = h.timers.get(id);
    delays.push(timer.delay);
    h.timers.delete(id);
    timer.callback();
  }
  assert.deepEqual(delays, [3000, 6000, 12000, 24000, 30000, 30000]);
  assert.equal(h.sources.length, 7);
  assert.equal(h.state.pendingIntents.length, 1);
  assert.equal(h.state.attemptsById['s-0011223344556677'], undefined);
  await h.queueFrame(envelope('status', 1, snapshot()), 'status');
  assert.equal(h.state.watchRetryDelayMs, 3000, 'verified live recovery resets backoff');
});

test('native connecting retries and superseded watch errors do not start another watch', async () => {
  const h = await harness();
  h.context.fetch = async () => response(200, { session_handle: 'abcdefghijklmnopqrstuv', boot_id: 'boot-a' });
  h.openWatch();
  const original = h.sources[0];
  original.listeners.error();
  assert.equal(h.state.watchRetryTimer, 0);
  h.openWatch();
  h.state.fresh = true;
  original.listeners.error();
  assert.equal(h.sources.length, 2);
  assert.equal(h.state.watchRetryTimer, 0);
  assert.equal(h.state.fresh, true, 'an old stream cannot stale the current stream');
});

test('closing or ending a watch cancels scheduled retry work', async () => {
  const h = await harness();
  h.context.fetch = async () => response(200, { session_handle: 'abcdefghijklmnopqrstuv', boot_id: 'boot-a' });
  h.openWatch();
  h.sources[0].readyState = 2;
  h.sources[0].listeners.error();
  const id = h.state.watchRetryTimer;
  const callback = h.timers.get(id).callback;
  h.state.closed = true;
  h.closeWatch();
  assert.equal(h.state.watchRetryTimer, 0);
  assert.equal(h.timers.has(id), false);
  callback();
  assert.equal(h.sources.length, 1, 'a queued callback cannot reopen a terminal session');
});

test('missing link shows no stale timer status or access footer', async () => {
  const h = await harness();
  await h.main();
  assert.equal(h.get('endTitle').textContent, 'No share link');
  assert(h.get('timerStatusStrip').classList.contains('hidden'));
  assert(h.get('sessionFooter').classList.contains('hidden'));
  assert(h.state.closed);
});

test('only exact authenticated response shapes reach status, with boot and sequence guards', async () => {
  const h = await harness();
  const good = envelope('status', 1, snapshot());
  assert.equal(await h.queueFrame({ ...good, sig: 'A'.repeat(86) }, 'status'), false);
  assert.equal(await h.queueFrame({ ...good, primed: true }, 'status'), false);
  assert.equal(await h.queueFrame({ ...good, body: snapshot({ boot_id: 'evil' }) }, 'status'), false);
  assert.equal(await h.queueFrame({ ...good, body_json: 'not json' }, 'status'), false);
  assert.equal(await h.queueFrame({ ...good, body_json: '[]' }, 'status'), false);
  assert.equal(h.state.status, null);
  assert.equal(await h.queueFrame(good, 'status'), true);
  assert.equal(h.state.status.boot_id, 'boot-a');
  assert.equal(h.state.status.remaining_ms, 60000);
  assert.equal(h.state.lastSeenUpstreamSeq, 1);
  assert.equal(await h.queueFrame(good, 'status'), false);
  assert.equal(await h.queueFrame(envelope('status', 2, snapshot({ boot_id: 'boot-b' })), 'status'), false);
  assert.equal(await h.queueFrame({ ...envelope('status', 3, snapshot()), upstream_seq: Number.MAX_SAFE_INTEGER + 1 }, 'status'), false);
  assert.equal(h.state.lastSeenUpstreamSeq, 1);
});

test('missing authenticated channel cannot accept a plaintext status', async () => {
  const h = await harness(); h.state.channel = null;
  assert.equal(await h.queueFrame(envelope('status', 1, snapshot()), 'status'), false);
  assert.equal(h.state.status, null);
});

test('status/event sequence acceptance is serialized and events bind to boot', async () => {
  const h = await harness();
  const accepted = await Promise.all([
    h.queueFrame(envelope('status', 3, snapshot()), 'status'),
    h.queueFrame(envelope('status', 2, snapshot({ state: 'paused' })), 'status'),
    h.queueFrame(envelope('event', 4, { type: 'paused', boot_id: 'boot-b' }), 'event'),
    h.queueFrame(envelope('event', 5, { type: 'paused', boot_id: 'boot-a' }), 'event'),
  ]);
  assert.deepEqual(accepted, [true, false, false, true]);
  assert.equal(h.state.status.state, 'running');
  assert.equal(h.state.events.length, 1);
});

test('HTTP 200 with empty, malformed or mismatched results leaves exact intent unresolved', async () => {
  for (const body of [null, {}, result(intent()), envelope('result', 5, result(intent(), { op_id: 'someone-else' })),
    envelope('result', 5, result(intent(), { code: undefined })),
    envelope('result', 5, result(intent(), { mutation: undefined })),
    envelope('result', 5, result(intent(), { status: snapshot({ boot_id: 'boot-b' }) }))]) {
    const h = await harness();
    h.context.fetch = async () => response(200, body);
    await h.runCommand(intent());
    assert.equal(h.state.pendingIntents.length, 1);
    assert.match(h.get('lastResult').textContent, /outcome unknown/);
    assert.equal(h.get('lastResult').textContent.includes('· applied'), false);
    const saved = JSON.parse([...h.storage.values()][0]);
    assert.deepEqual(saved.pendingIntents[0], intent());
  }
});

test('network failure and HTTP timeout preserve intent; explicit retry reuses every byte', async () => {
  for (const failure of ['network', 'timeout']) {
    const h = await harness();
    const bodies = [];
    h.context.fetch = async (url, options) => {
      bodies.push(options.body);
      assert.equal(options.headers['X-OST-Session'], 'abcdefghijklmnopqrstuv');
      if (bodies.length === 1) {
        if (failure === 'network') throw new Error('offline');
        return response(504, { detail: 'timer_offline_or_slow' });
      }
      return response(200, envelope('result', 5, result(intent())));
    };
    await h.runCommand(intent());
    assert.equal(h.state.pendingIntents.length, 1);
    await h.runCommand(h.state.pendingIntents[0]);
    assert.equal(bodies[0], bodies[1]);
    assert.equal(h.state.pendingIntents.length, 0);
    assert.match(h.get('lastResult').textContent, /Adjust · applied/);
  }
});

test('authenticated rejection reports rejection and clears only its own pending operation', async () => {
  const h = await harness();
  const first = intent(); const second = intent({ op: 'release', op_id: 'release-id', params: {} });
  h.state.pendingIntents = [first, second];
  const rejected = result(first, { code: 'stale_intent', mutation: {
    action: first.op, operation_id: first.op_id, changed: false, duplicate: false,
  } });
  assert.equal(await h.queueFrame(envelope('result', 2, rejected), 'result'), true);
  assert.equal(h.state.pendingIntents.length, 1);
  assert.equal(h.state.pendingIntents[0].op, 'release');
  assert.match(h.get('lastResult').textContent, /was not applied: stale_intent/);
});

test('expired unknown intent is never resent with a renewed deadline', async () => {
  const h = await harness();
  h.state.pendingIntents = [intent()];
  h.state.lastStatusWallMs = Date.now() - 6000;
  let sent = false;
  h.context.fetch = async () => { sent = true; throw new Error('must not send'); };
  await h.runCommand(intent());
  assert.equal(sent, false);
  assert.equal(h.state.pendingIntents.length, 1);
  assert.match(h.get('lastResult').textContent, /retry window ended/);
});

test('a retry rejection cannot claim the original unknown action was not applied', async () => {
  const h = await harness(); let count = 0;
  h.context.fetch = async () => {
    if (++count === 1) throw new Error('response lost after possible application');
    return response(200, envelope('result', 9, result(intent(), {
      code: 'expired', mutation: { action: 'adjust', operation_id: intent().op_id,
        changed: false, duplicate: false },
    })));
  };
  await h.runCommand(intent());
  await h.runCommand(h.state.pendingIntents[0]);
  assert.equal(h.state.pendingIntents.length, 1);
  assert.match(h.get('lastResult').textContent, /outcome unknown/);
  assert.equal(h.get('lastResult').textContent.includes('was not applied'), false);
});

test('cached correlated result resolves once behind newer status and never rewinds display', async () => {
  const h = await harness(); const original = intent();
  h.state.pendingIntents = [original];
  await h.queueFrame(envelope('status', 10, snapshot({ state: 'paused', uptime_ms: 8000 })), 'status');
  const old = envelope('result', 2, result(original));
  assert.equal(await h.queueFrame(old, 'result', { intent: original, cached: true }), true);
  assert.equal(h.state.lastSeenUpstreamSeq, 10);
  assert.equal(h.state.status.state, 'paused');
  assert.equal(await h.queueFrame(old, 'result', { intent: original, cached: true }), false);
});

test('fresh status cannot erase an unknown outcome and explicit release remains enabled when stale', async () => {
  const h = await harness();
  h.state.pendingIntents = [intent()];
  await h.queueFrame(envelope('status', 10, snapshot()), 'status');
  assert.equal(h.state.pendingIntents.length, 1);
  h.markStale();
  assert.equal(h.buttons.find((b) => b.dataset.op === 'adjust').disabled, true);
  assert.equal(h.buttons.find((b) => b.dataset.op === 'release').disabled, false);
  assert.match(h.get('dialKicker').textContent, /Last verified/);
});

test('cached status is visibly stale and a current authenticated status restores actions', async () => {
  const h = await harness();
  await h.queueFrame(envelope('status', 1, snapshot()), 'status', { cached: true });
  assert.equal(h.state.fresh, false);
  assert.equal(h.buttons[0].disabled, true);
  await h.queueFrame(envelope('status', 2, snapshot()), 'status');
  assert.equal(h.state.fresh, true);
  assert.equal(h.buttons[0].disabled, false);
  h.state.lastVerifiedWallMs = Date.now() - 16000;
  h.renderExpiryCountdown();
  assert.equal(h.state.fresh, false);
});

test('refresh opens fresh keys and preserves unresolved intent without mutation replay', async () => {
  const initial = await harness();
  initial.state.pendingIntents = [intent()]; initial.state.lastSeenUpstreamSeq = 7;
  initial.saveTabState();
  const h = await harness({storage:initial.storage}), requests=[];
  h.context.fetch=async (url,options) => {
    requests.push({url,options});
    return response(200,{session_handle:'abcdefghijklmnopqrstuv',timer_id:'timer-test',boot_id:'boot-a',frame:{type:'e2e_accept'}});
  };
  await h.main();
  assert.equal(requests.length,2);
  assert.equal(requests[0].options.method,'DELETE');
  assert.equal(requests[1].options.method,'POST');
  assert(!requests[1].options.body.includes(h.state.token));
  assert.equal(h.state.pendingIntents[0].op_id,intent().op_id);
  assert.equal(h.state.lastSeenUpstreamSeq,7);
  assert.equal(h.sources[0].url,'https://relay.test/api/v1/watch?session_handle=abcdefghijklmnopqrstuv');
  h.context.fetch=async (url,options) => {
    assert.equal(url,'https://relay.test/api/v1/command');
    assert.equal(JSON.parse(options.body).type,'lookup');
    return response(404,null);
  };
  await h.reconcilePending();
  assert.equal(h.state.pendingIntents.length,1);
});

test('wrong timer binding prevents watch setup', async () => {
  const h=await harness();h.saveTabState();
  h.context.fetch=async () => response(200,{session_handle:'abcdefghijklmnopqrstuv',timer_id:'other-timer',boot_id:'boot-a',frame:{type:'e2e_accept'}});
  await h.main();
  assert.equal(h.sources.length,0);
  assert.match(h.get('endTitle').textContent,/Another share link/);
});

test('binding failure during reconnect closes controls without a mutation', async () => {
  const h=await harness();h.openWatch();let checks=0;
  h.context.fetch=async (url,options) => {
    if (options.method === 'POST') return response(503, {}); // read-only startup sync
    checks++;assert.equal(options.method,undefined);return response(409,{detail:'session_mismatch'});
  };
  h.sources[0].listeners.error();await h.state.bindingCheck;
  assert.equal(h.state.closed,true);
  await h.runCommand(intent());assert.equal(checks,1);
});

test('command cookie collision preserves unknown intent and closes actions', async () => {
  const h = await harness(); let requests = 0;
  h.context.fetch = async () => {
    requests++;
    return response(409, { detail: 'session_mismatch' });
  };
  await h.runCommand(intent());
  assert.equal(h.state.pendingIntents.length, 1);
  assert.equal(h.state.closed, true);
  await h.runCommand(intent({ op: 'release', op_id: 'another-id' }));
  assert.equal(requests, 1);
});

test('cached primer cannot reopen an original retry deadline or extend token expiry', async () => {
  const h = await harness();
  h.state.pendingIntents = [intent()];
  h.state.lastStatusUptimeMs = 7000;
  await h.queueFrame(envelope('status', 1, snapshot()), 'status', { cached: true });
  assert.ok(h.state.lastStatusUptimeMs >= 7000);
  let sent = false;
  h.context.fetch = async () => { sent = true; throw new Error('must not send'); };
  await h.runCommand(intent());
  assert.equal(sent, false);
  h.state.tokenExpUpMs = 6500;
  h.renderExpiryCountdown();
  assert.equal(h.state.expired, true);
});

test('fragment clears before networking and bearer stays on the trusted origin', async () => {
  const payload={boot:'boot-a',vk:publicKey,tid:'timer-test',sc:['view'],exp_up_ms:50000};
  const token='ost-cap.v1.'+Buffer.from(JSON.stringify(payload)).toString('base64url')+'.secret-tag';
  const h=await harness({hash:'#'+token});
  h.context.fetch=async (url,options) => {
    assert.equal(h.context.window.location.hash,'');
    if(options.body)assert(!options.body.includes(token));
    assert.equal(options.credentials,'omit');
    return response(200,{session_handle:'abcdefghijklmnopqrstuv',timer_id:'timer-test',boot_id:'boot-a',frame:{type:'e2e_accept'}});
  };
  await h.main();
  const saved=JSON.parse(h.storage.get('ost-relay-tab-v1'));
  assert.equal(saved.token,token);assert.equal(saved.relayOrigin,'https://relay.test');
  assert.equal(saved.channel,undefined);
});

test('inline start reads duration, refreshes its deadline and preserves the clicked session', async () => {
  const h = await harness();
  h.state.status = snapshot({ state: 'open', session_id: null, max_duration_ms: 600000 });
  h.get('hours').value = '0'; h.get('minutes').value = '3'; h.get('seconds').value = '5';
  let sent;
  h.context.fetch = async (_url, options) => {
    const body = JSON.parse(options.body);
    if (body.type === 'sync') return response(200, envelope('status', 1,
      snapshot({ state: 'open', session_id: null, max_duration_ms: 600000, uptime_ms: 11000 })));
    sent = body; throw new Error('lost response');
  };
  await h.onButton(h.buttons[2]);
  assert.equal(sent.params.duration_ms, 185000);
  assert.equal(sent.if_boot_id, 'boot-a');
  assert.equal(sent.if_session_id, null);
  assert.equal(sent.observed_uptime_ms, 11000);
  assert.equal(h.state.pendingIntents[0].op_id, sent.op_id);
});

test('a ten-second-old adjustment uses a fresh authenticated deadline, including deduction to zero', async () => {
  const h = await harness();
  h.state.status = snapshot({ remaining_ms: 900000 });
  h.state.lastStatusWallMs -= 10000;
  h.buttons[0].dataset.adjust = '-1800000';
  h.context.fetch = async (_url, options) => {
    const body = JSON.parse(options.body);
    if (body.type === 'sync') return response(200, envelope('status', 1,
      snapshot({ remaining_ms: 890000, uptime_ms: 11000 })));
    return response(200, envelope('result', 2, result({ ...body,
      mutation: { action: body.op, operation_id: body.op_id, changed: true, duplicate: false },
      status: snapshot({ state: 'open', remaining_ms: 0, uptime_ms: 11001 }) })));
  };
  await h.onButton(h.buttons[0]);
  const command = h.sentBodies.find(body => body.type === 'command');
  assert.equal(command.observed_uptime_ms, 11000);
  assert.equal(command.expiry_uptime_ms, 16000);
  assert.equal(command.params.delta_ms, -1800000);
  assert.equal(h.state.pendingIntents.length, 0);
});

test('preflight never retargets a new timer session or sends without fresh proof', async () => {
  for (const status of [snapshot({ session_id: 'other-session', uptime_ms: 11000 }), null]) {
    const h = await harness(); h.state.status = snapshot();
    h.context.fetch = async () => response(200, status ? envelope('status', 1, status) : {});
    await h.onButton(h.buttons[0]);
    assert.equal(h.sentBodies.filter(body => body.type === 'command').length, 0);
    assert.equal(h.state.pendingIntents.length, 0);
  }
});

test('ordinary in-flight commands do not flash an unconfirmed warning', async () => {
  const h = await harness(); let finish;
  h.context.fetch = () => new Promise(resolve => { finish = resolve; });
  const work = h.runCommand(intent());
  await new Promise(resolve => setImmediate(resolve));
  assert.equal(h.get('pendingCard').classList.contains('hidden'), true);
  finish(response(500, {})); await work;
  assert.equal(h.get('pendingCard').classList.contains('hidden'), false);
});

test('a control click immediately shows neutral feedback while awaiting fresh proof', async () => {
  const h = await harness();
  h.state.status = snapshot();
  let finish;
  h.context.fetch = () => new Promise(resolve => { finish = resolve; });
  const work = h.onButton(h.buttons[0]);
  assert.equal(h.get('lastResult').textContent, 'Waiting for current verified timer status.');
  assert.equal(h.get('lastResult').classList.contains('pending'), true);
  assert.equal(h.buttons[0].disabled, true);
  await new Promise(resolve => setImmediate(resolve));
  finish(response(503, {})); await work;
  assert.equal(h.sentBodies.filter(body => body.type === 'command').length, 0);
  assert.equal(h.get('lastResult').textContent, 'Unable to verify current timer status. No command was sent.');
});

test('an unknown result is automatically checked with read-only lookup, never replayed', async () => {
  const h = await harness();
  h.context.fetch = async () => { throw new Error('response lost'); };
  await h.runCommand(intent());
  const timer = h.timers.get(h.state.reconcileTimer);
  assert.ok(timer, 'unknown result schedules a bounded lookup');
  h.context.fetch = async (_url, options) => {
    assert.equal(JSON.parse(options.body).type, 'lookup');
    return response(200, envelope('result', 1, result(intent())));
  };
  await timer.callback();
  assert.equal(h.state.pendingIntents.length, 0);
  assert.equal(h.sentBodies.filter(body => body.type === 'command').length, 1);
});

test('lock controls is unavailable while paused', async () => {
  const h = await harness(); h.state.scopes.add('lock');
  h.updateButtonsForState(snapshot({ state: 'paused' }));
  assert.equal(h.buttons[3].classList.contains('hidden'), true);
});

test('lock confirmation cancellation and a changed session send no command', async () => {
  for (const accept of [false, true]) {
    const h = await harness(); h.state.status = snapshot(); h.state.scopes.add('lock');
    h.context.fetch = async () => response(200, envelope('status', 1, snapshot({ session_id: 'new-session', uptime_ms: 2000 })));
    const work = h.onButton(h.buttons[3]);
    assert.equal(h.get('confirmDialog')['aria-hidden'], 'false');
    assert.equal(h.sentBodies.length, 0);
    h.closeLockConfirm(accept); await work;
    assert.equal(h.sentBodies.filter(body => body.type === 'command').length, 0);
    assert.equal(h.state.preparingCommand, false);
    assert.equal(h.get('main').inert, false);
  }
});

test('random start remains within its range and an explicit retry preserves its original draw', async () => {
  const h = await harness();
  const open = snapshot({ state: 'open', session_id: null, max_duration_ms: 3600000, uptime_ms: 2000 });
  h.state.status = open; h.get('randomMode').checked = true;
  h.get('randomMin').value = '5'; h.get('randomMax').value = '15';
  h.context.fetch = async (_url, options) => JSON.parse(options.body).type === 'sync'
    ? response(200, envelope('status', 1, open)) : response(500, {});
  await h.onButton(h.buttons[2]);
  const original = h.state.pendingIntents[0];
  assert(original.params.duration_ms >= 300000 && original.params.duration_ms <= 900000);
  h.get('randomMin').value = '30'; h.get('randomMax').value = '60';
  await h.runCommand(original);
  const commands = h.sentBodies.filter(body => body.type === 'command');
  assert.equal(JSON.stringify(commands[0]), JSON.stringify(commands[1]));
});

test('invalid random ranges are rejected before synchronization or delivery', async () => {
  for (const [min, max] of [['', '10'], ['0', '10'], ['2', '1'], ['1.5', '10'], ['1', '1441'], ['1', '11']]) {
    const h = await harness();
    h.state.status = snapshot({ state: 'open', session_id: null, max_duration_ms: 600000 });
    h.get('randomMode').checked = true; h.get('randomMin').value = min; h.get('randomMax').value = max;
    await h.onButton(h.buttons[2]);
    assert.equal(h.sentBodies.length, 0);
    assert.equal(h.state.pendingIntents.length, 0);
  }
});

test('a click shares an in-flight sync and ignores a second click while preparing', async () => {
  const h = await harness(); h.state.status = snapshot(); let finish;
  h.context.fetch = async (_url, options) => JSON.parse(options.body).type === 'sync'
    ? new Promise(resolve => { finish = resolve; }) : response(500, {});
  const sync = h.syncStatus();
  await new Promise(resolve => setImmediate(resolve));
  const click = h.onButton(h.buttons[0]);
  await h.onButton(h.buttons[0]);
  finish(response(200, envelope('status', 1, snapshot({ uptime_ms: 2000 }))));
  await Promise.all([sync, click]);
  assert.equal(h.sentBodies.filter(body => body.type === 'sync').length, 1);
  assert.equal(h.sentBodies.filter(body => body.type === 'command').length, 1);
});

test('startup authenticates status before EventSource fires open', async () => {
  const h = await harness(); h.state.status = null; h.state.fresh = false;
  h.context.fetch = async () => response(200, envelope('status', 1, snapshot()));
  h.openWatch();
  await h.state.syncPromise;
  assert.equal(h.sources[0].readyState, 0);
  assert.equal(h.state.fresh, true);
  assert(h.state.syncTimer);
  assert.equal(h.sentBodies.length, 1);
  assert.equal(h.sentBodies[0].type, 'sync');
  h.closeWatch(); assert.equal(h.state.syncTimer, 0);
});

test('verified status received through SSE makes its duplicate HTTP response a successful sync', async () => {
  const h = await harness(); h.state.fresh = false;
  h.context.fetch = async () => {
    const request = [...h.state.syncRequests.keys()].at(-1);
    const frame = {...envelope('status', 1, snapshot()), request_id: request};
    assert.equal(await h.queueFrame(frame, null), true);
    return response(200, frame);
  };
  assert.equal(await h.syncStatus(), true);
  assert.equal(h.state.fresh, true);
  assert.equal(h.state.syncRequests.size, 0);
  assert.equal(h.sentBodies.length, 1);
});

test('duplicate delivery cannot make an invalid, stale or cached challenge successful', async () => {
  for (const kind of ['envelope', 'boot', 'sequence', 'cached', 'late']) {
    const h = await harness(); h.state.fresh = false;
    h.context.fetch = async () => {
      const request = [...h.state.syncRequests.keys()].at(-1);
      const frame = {...envelope('status', 1, snapshot(kind === 'boot' ? {boot_id: 'wrong'} : {})), request_id: request};
      if (kind === 'envelope') frame.sig = 'A'.repeat(86);
      if (kind === 'sequence') h.state.lastSeenUpstreamSeq = 1;
      if (kind === 'late') h.state.syncRequests.set(request, performance.now() - 6000);
      await h.queueFrame(frame, null, {cached: kind === 'cached'});
      return response(200, frame);
    };
    assert.equal(await h.syncStatus(), false, kind);
    assert.equal(h.state.fresh, false, kind);
    assert.equal(h.sentBodies.length, 1);
    assert.equal(h.sentBodies[0].type, 'sync');
  }
});

test('inline duration rejects blank, fractional, zero and over-limit values before sending', async () => {
  for (const [hours, minutes, seconds] of [['0', '', '0'], ['0', '1.5', '0'], ['0', '0', '0'], ['0', '11', '0'], ['0', '0', '60']]) {
    const h = await harness();
    h.state.status = snapshot({ state: 'open', session_id: null, max_duration_ms: 600000 });
    h.get('hours').value = hours; h.get('minutes').value = minutes; h.get('seconds').value = seconds;
    let calls = 0; h.context.fetch = async () => { calls++; throw new Error('unexpected'); };
    await h.onButton(h.buttons[2]);
    assert.equal(calls, 0);
    assert.equal(h.state.pendingIntents.length, 0);
  }
});

test('view-only and closed grants expose no setup or adjustment controls', async () => {
  const h = await harness();
  h.state.scopes = new Set(['view']);
  h.updateButtonsForState(snapshot({ state: 'open' }));
  assert.equal(h.get('setupControls').classList.contains('hidden'), true);
  assert.equal(h.get('fixedDuration').classList.contains('hidden'), true);
  assert.equal(h.get('leftRail').classList.contains('hidden'), true);
  assert(h.buttons.every(button => button.classList.contains('hidden')));
  h.state.scopes = new Set(['view', 'start']); h.state.closed = true;
  h.updateButtonsForState(snapshot({ state: 'open' }));
  assert(h.buttons.every(button => button.disabled));
});


test('countdown estimation changes only the display and stops with stale state', async () => {
  const h = await harness();
  h.state.status = snapshot();
  h.state.lastVerifiedWallMs = Date.now() - 2000;
  h.renderExpiryCountdown();
  assert.equal(h.state.status.remaining_ms, 60000);
  assert.equal(h.state.status.uptime_ms, 1000);
  assert.equal(h.get('dialTime').textContent, '00:00:58');
  h.state.fresh = false;
  h.get('dialTime').textContent = 'last snapshot';
  h.renderExpiryCountdown();
  assert.equal(h.get('dialTime').textContent, 'last snapshot');
});

test('locked controls replace refused buttons with the shared locked notice', async () => {
  const h = await harness();
  await h.queueFrame(envelope('status', 1, snapshot({ sealed: true })), 'status');
  const release = h.buttons.find((b) => b.dataset.op === 'release');
  assert.equal(release.classList.contains('hidden'), true);
  assert.equal(h.buttons.find((b) => b.dataset.op === 'adjust').disabled, true);
  assert.equal(h.get('sealedNotice').classList.contains('hidden'), false);
  assert.equal(h.get('stateBadge').className, 'badge sealed');
  assert.equal(h.get('statusLine').textContent, '');
  assert.equal(h.context.document.documentElement.dataset.state, 'sealed');
  await h.queueFrame(envelope('status', 2, snapshot({ uptime_ms: 2000 })), 'status');
  assert.equal(h.get('sealedNotice').classList.contains('hidden'), true);
  assert.equal(release.classList.contains('hidden'), false);
});

test('a lost connection shows the offline badge and warning until fresh status', async () => {
  const h = await harness();
  const open = (uptime) => snapshot({ state: 'open', remaining_ms: 0, session_duration_ms: 0, uptime_ms: uptime });
  await h.queueFrame(envelope('status', 1, open(1000)), 'status');
  const start = h.buttons.find((b) => b.dataset.op === 'start');
  assert.equal(start.disabled, false);
  assert.equal(h.get('hours').disabled, false);
  h.markStale('Relay connection interrupted · last verified snapshot', true);
  assert.equal(h.get('stateBadge').className, 'badge offline');
  assert.equal(h.get('connectionWarning').classList.contains('hidden'), false);
  assert.match(h.get('connectionWarningMessage').textContent, /physical emergency release/);
  // Setup dims with its fields while status is unconfirmed.
  assert.equal(start.disabled, true);
  assert.equal(h.get('hours').disabled, true);
  h.markStale('Connected to relay · waiting for verified timer status');
  assert.equal(h.get('stateBadge').className, 'badge stale');
  assert.equal(h.get('connectionWarning').classList.contains('hidden'), false);
  await h.queueFrame(envelope('status', 2, open(2000)), 'status');
  assert.equal(h.get('connectionWarning').classList.contains('hidden'), true);
  assert.equal(start.disabled, false);
  assert.equal(h.get('hours').disabled, false);
});

test('idle channel recovery shares one handshake and retains the draft and unknown intents', async () => {
  const h = await harness(), oldHandle = h.state.sessionHandle, newHandle = 'zyxwvutsrqponmlkjihgfe';
  await h.queueFrame(envelope('status',1,snapshot({state:'open',session_id:null})), 'status');
  h.get('minutes').value = '17';
  h.state.pendingIntents = [intent()]; h.state.attemptsById[intent().op_id] = 1;
  h.state.lastVerifiedWallMs = h.state.channelCreatedWallMs = Date.now()-1229000;
  h.markStale('Timer status is stale · last verified snapshot',true);
  const oldChannel = h.state.channel, newChannel = {...oldChannel};
  let oldClosed = 0, opens = 0; oldChannel.close = () => oldClosed++;
  h.context.OSTSecure.open = async () => {
    opens++;
    return {payload:{boot:'boot-a',tid:'timer-test'},frame:{fp_hash:'public-hash'},accept:async()=>newChannel};
  };
  const deleted = [];
  h.context.fetch = async (url,opts) => {
    if (opts.method === 'DELETE') { deleted.push(opts.headers['X-OST-Session']); return response(200,{}); }
    if (opts.method === 'POST' && url.endsWith('/session')) {
      assert(!opts.body.includes(h.state.token));
      return response(200,{boot_id:'boot-a',timer_id:'timer-test',session_handle:newHandle,frame:{}});
    }
    assert.equal(JSON.parse(opts.body).type,'sync','recovery only asks for status');
    if (opts.headers['X-OST-Session'] === oldHandle) return response(504,{});
    assert.equal(opts.headers['X-OST-Session'],newHandle);
    return response(200,{...envelope('status',2,snapshot({state:'open',session_id:null,uptime_ms:2000})),request_id:[...h.state.syncRequests.keys()].at(-1)});
  };
  const answers = await Promise.all([h.syncStatus(),h.syncStatus()]);
  assert.deepEqual(answers,[true,true]); assert.equal(opens,1); assert.equal(oldClosed,1);
  assert.deepEqual(deleted,[oldHandle]); assert.equal(h.state.channel,newChannel);
  assert.equal(h.state.lastSeenUpstreamSeq,2); assert.equal(h.state.fresh,true);
  assert.equal(h.get('connectionWarning').classList.contains('hidden'),true);
  assert.equal(h.get('minutes').value,'17'); assert.equal(h.state.pendingIntents.length,1);
  assert.equal(h.state.attemptsById[intent().op_id],1);
  assert(h.sentBodies.every(body=>body.type==='sync'));
});

test('unverified recovery stays stale, releases unused admissions and backs off through an outage', async () => {
  const h = await harness(); let wall = Date.now(), opens = 0;
  h.context.Date = class extends Date { static now() { return wall; } };
  h.state.lastVerifiedWallMs = h.state.channelCreatedWallMs = wall-1229000;
  h.markStale('Timer status is stale · last verified snapshot',true);
  h.context.OSTSecure.open = async () => {
    opens++;
    return {payload:{boot:'boot-a',tid:'timer-test'},frame:{fp_hash:'public-hash'},accept:async()=>{throw Error('bad_accept');}};
  };
  let released = 0;
  h.context.fetch = async (url,opts) => {
    if (opts.method === 'DELETE') { released++; return response(200,{}); }
    if (url.endsWith('/session')) return response(200,{boot_id:'boot-a',timer_id:'timer-test',session_handle:'zyxwvutsrqponmlkjihgfe',frame:{}});
    return response(504,{});
  };
  const delays = [];
  for (let attempt=0;attempt<5;attempt++) {
    assert.equal(await h.syncStatus(),false);
    delays.push(h.state.nextChannelRecoveryMs-wall);
    assert.equal(opens,attempt+1);
    assert.equal(h.state.channel,null); assert.equal(h.state.fresh,false); assert.equal(h.state.closed,false);
    assert(h.state.syncTimer,'read-only polling survives failure to open a channel');
    assert(h.buttons.every(button=>button.dataset.op==='release'||button.disabled));
    assert.equal(await h.syncStatus(),false); assert.equal(opens,attempt+1,'no immediate repeated ECC opens');
    wall = h.state.nextChannelRecoveryMs+1;
  }
  assert.deepEqual(delays,[15000,30000,30000,30000,30000]);
  assert.equal(released,6,'old routing admission and each unverified new admission are released');
  assert(h.sentBodies.every(body=>body.type==='sync'));
});

test('a verified replacement handshake cannot restore controls without a fresh bound status', async () => {
  for (const kind of ['boot','nonce','late','sequence']) {
    const h = await harness(), channel = {...h.state.channel};
    h.state.lastSeenUpstreamSeq = 10;
    h.state.lastVerifiedWallMs = h.state.channelCreatedWallMs = Date.now()-1229000;
    h.markStale('Timer status is stale · last verified snapshot',true);
    let opens = 0;
    h.context.OSTSecure.open = async () => {
      opens++;
      return {payload:{boot:'boot-a',tid:'timer-test'},frame:{fp_hash:'public-hash'},accept:async()=>channel};
    };
    h.context.fetch = async (url,opts) => {
      if (opts.method==='DELETE') return response(200,{});
      if (url.endsWith('/session')) return response(200,{boot_id:'boot-a',timer_id:'timer-test',session_handle:'zyxwvutsrqponmlkjihgfe',frame:{}});
      if (h.state.channel!==channel) return response(504,{});
      const request = [...h.state.syncRequests.keys()].at(-1);
      if (kind==='late') h.state.syncRequests.set(request,performance.now()-6000);
      return response(200,{...envelope('status',kind==='sequence'?10:11,snapshot(kind==='boot'?{boot_id:'different'}:{})),request_id:kind==='nonce'?'unrelated':request});
    };
    assert.equal(await h.syncStatus(),false,kind);
    assert.equal(h.state.fresh,false,kind); assert.equal(h.state.lastSeenUpstreamSeq,10,kind);
    assert.equal(h.get('connectionWarning').classList.contains('hidden'),false,kind);
    assert.equal(h.buttons.find(button=>button.dataset.op==='start').disabled,true,kind);
    assert.equal(await h.syncStatus(),false,kind); assert.equal(opens,1,kind);
  }
});

test('late events from a replaced stream cannot expire or change the recovered tab', async () => {
  const h = await harness(); h.context.fetch = async () => response(504,{});
  h.openWatch(); const old = h.sources.at(-1); h.openWatch();
  await h.state.syncPromise;
  h.state.fresh = true;
  for (const type of ['open','token_expired','token_revoked','timer_gone','error']) old.listeners[type]();
  old.listeners.e2e({data:JSON.stringify(envelope('status',1,snapshot()))});
  await h.state.frameQueue;
  assert.equal(h.state.fresh,true); assert.equal(h.state.closed,false); assert.equal(h.state.expired,false);
  assert.equal(h.sources.length,2);
});

test('disconnecting during a handshake forgets the capability and releases the late admission', async () => {
  const h = await harness(), replacement = {...h.state.channel};
  h.state.lastVerifiedWallMs = h.state.channelCreatedWallMs = Date.now()-1229000;
  h.markStale('Timer status is stale · last verified snapshot',true);
  let complete, released = [];
  h.context.OSTSecure.open = async () => ({payload:{boot:'boot-a',tid:'timer-test'},frame:{fp_hash:'public-hash'},accept:async()=>replacement});
  h.context.fetch = async (url,opts) => {
    if (opts.method==='DELETE') { released.push(opts.headers['X-OST-Session']); return response(200,{}); }
    if (url.endsWith('/session')) return new Promise(resolve=>{complete=resolve;});
    return response(504,{});
  };
  const sync = h.syncStatus();
  await new Promise(resolve=>setImmediate(resolve)); assert(complete);
  h.saveTabState();
  await h.disconnectSession();
  complete(response(200,{boot_id:'boot-a',timer_id:'timer-test',session_handle:'zyxwvutsrqponmlkjihgfe',frame:{}}));
  assert.equal(await sync,false); assert.equal(h.state.closed,true); assert.equal(h.state.fresh,false);
  assert.equal(h.state.channel,null); assert.equal(h.sources.length,0); assert.equal(h.state.syncTimer,0);
  assert.deepEqual(released,['abcdefghijklmnopqrstuv','zyxwvutsrqponmlkjihgfe']);
  assert.equal(h.state.token, ''); assert.equal(h.state.sessionHandle, '');
  h.saveTabState();
  assert.equal(h.storage.size, 0, 'late work must not save a disconnected capability again');
  assert.equal(h.get('endTitle').textContent, 'Disconnected');
  await h.runCommand(intent({op:'release'}));
  assert.equal(h.sentBodies.filter(body => body.type === 'command').length, 0, 'disconnect never changes the timer');
});

test('a queued control bound to a replaced channel is retained as unknown and never sent', async () => {
  const h = await harness(); let drain;
  h.state.outboundQueue = new Promise(resolve=>{drain=resolve;});
  let deliveries = 0; h.context.fetch = async () => { deliveries++; return response(200,{}); };
  const request = h.runCommand(intent());
  h.state.channel = {...h.state.channel}; h.state.sessionHandle = 'zyxwvutsrqponmlkjihgfe';
  drain(); await request;
  assert.equal(deliveries,0); assert.equal(h.sentBodies.length,0);
  assert.equal(h.state.pendingIntents.length,1); assert.equal(h.state.attemptsById[intent().op_id],1);
});
