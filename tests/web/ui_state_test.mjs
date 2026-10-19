import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import test from 'node:test';
import vm from 'node:vm';

const i18nSource = readFileSync(new URL('../../web/i18n.js', import.meta.url), 'utf8');
const webSource = readFileSync(new URL('../../web/app.js', import.meta.url), 'utf8');

// Minimal DOM contract for the actual shared translator. Real browser layout,
// focus, SSE and offline tests supplement this dependency-free CI regression.
function languageHarness({ language = 'en', storageFails = false } = {}) {
  let observer;
  class Element {
    constructor({ literal = false, attrs = {}, children = [] } = {}) {
      this.nodeType = 1;
      this.literal = literal;
      this.attrs = { ...attrs };
      this.dataset = {};
      this.children = children;
      children.forEach(child => { child.parentElement = this; });
    }
    closest() { return this.literal ? this : this.parentElement?.closest() || null; }
    getAttribute(key) { return this.attrs[key] ?? null; }
    hasAttribute(key) { return key in this.attrs; }
    setAttribute(key, value) {
      this.attrs[key] = value;
      observer?.record({ type: 'attributes', target: this, attributeName: key });
    }
    querySelectorAll() {
      return this.children.filter(child => child.nodeType === 1).flatMap(child => [child, ...child.querySelectorAll()]);
    }
  }
  class Text {
    constructor(value) { this.nodeType = 3; this.value = value; }
    get nodeValue() { return this.value; }
    set nodeValue(value) {
      this.value = value;
      observer?.record({ type: 'characterData', target: this });
    }
  }
  const heading = new Text('  Set duration\n');
  const dynamic = new Text('');
  const literal = new Text('Pause');
  const input = new Element({ attrs: { placeholder: 'Password' } });
  input.value = 'unsaved-local-only';
  input.checked = true;
  const button = new Element();
  const body = new Element({ children: [heading, dynamic, input, button,
    new Element({ literal: true, children: [literal] })] });
  const document = { body, documentElement: { lang: 'en' }, activeElement: input,
    getElementById: () => button,
    createTreeWalker(root) {
      const texts = [];
      const collect = node => node.nodeType === 3 ? texts.push(node) : node.children.forEach(collect);
      collect(root);
      let index = 0;
      return { nextNode() { this.currentNode = texts[index++]; return Boolean(this.currentNode); } };
    } };
  let changes = 0;
  const window = { dispatchEvent: event => { assert.equal(event.type, 'smarttimer:languagechange'); changes++; } };
  const context = vm.createContext({ document, window, Event, NodeFilter: { SHOW_TEXT: 4 },
    sessionStorage: { getItem: () => language, setItem() { if (storageFails) throw Error('blocked'); } },
    MutationObserver: class {
      constructor() { observer = this; this.records = []; }
      observe() { this.active = true; }
      disconnect() { this.active = false; this.records = []; }
      record(record) { if (this.active) this.records.push(record); }
      takeRecords() { const records = this.records; this.records = []; return records; }
    } });
  vm.runInContext(i18nSource, context);
  return { i18n: window.SmartTimerI18n, heading, dynamic, literal, input, document,
    changes: () => changes };
}

test('language changes in place both ways without resetting inputs, focus or literal device names', () => {
  const h = languageHarness();
  for (let index = 0; index < 8; index++) {
    h.i18n.toggleLanguage();
    assert.equal(h.heading.nodeValue, index % 2 === 0 ? '  设置时长\n' : '  Set duration\n');
    assert.equal(h.input.value, 'unsaved-local-only');
    assert.equal(h.input.checked, true);
    assert.equal(h.document.activeElement, h.input);
    assert.equal(h.literal.nodeValue, 'Pause');
  }
  assert.equal(h.changes(), 8);
  assert.ok(!i18nSource.includes('location.reload'));
});

test('initial Chinese, blocked storage, dynamic replacements and pending DOM updates preserve message keys', () => {
  const h = languageHarness({ language: 'zh-Hans', storageFails: true });
  assert.equal(h.heading.nodeValue, '  设置时长\n');
  h.dynamic.nodeValue = h.i18n.text('Opens at {time}', { time: '7:42 PM' });
  h.i18n.toggleLanguage(); // Flush pending mutation before the click's next microtask.
  assert.equal(h.dynamic.nodeValue, 'Opens at 7:42 PM');
  h.i18n.toggleLanguage();
  assert.equal(h.dynamic.nodeValue, h.i18n.text('Opens at {time}', { time: '7:42 PM' }));
  h.dynamic.nodeValue = 'untranslated device value';
  h.i18n.toggleLanguage();
  assert.equal(h.dynamic.nodeValue, 'untranslated device value');
});

test('Bluetooth settings translations cover management labels and preserve device values across language changes', () => {
  const h = languageHarness({ language: 'zh-Hans' });
  for (const key of ['Timer settings', 'Approved devices', 'Pair device', 'Nearby networks', 'Network name',
    'Password', 'Forget Wi-Fi', 'Access point name', 'Access point password', 'Timer name', 'Restart timer',
    'Rename', 'Approved', 'New device name (optional)', 'Install update over Bluetooth', 'Rename approved device']) {
    assert.notEqual(h.i18n.text(key), key, `Missing Chinese translation: ${key}`);
  }
  h.dynamic.nodeValue = h.i18n.text('Join {ssid}?', { ssid: 'Pause <test>' });
  assert.equal(h.dynamic.nodeValue, '连接到 Pause <test>？');
  h.i18n.toggleLanguage();
  assert.equal(h.dynamic.nodeValue, 'Join Pause <test>?');
  h.i18n.toggleLanguage();
  assert.equal(h.dynamic.nodeValue, '连接到 Pause <test>？');
});

function freshnessHarness() {
  const nodes = new Map();
  const $ = id => {
    if (!nodes.has(id)) {
      const classes = new Set();
      nodes.set(id, { classList: { contains: key => classes.has(key),
        toggle(key, on) { on ? classes.add(key) : classes.delete(key); } } });
    }
    return nodes.get(id);
  };
  const context = vm.createContext({ $, performance: { now: () => context.now },
    t: key => key, setText: (id, value) => { $(id).textContent = value; },
    setConnection: mode => { context.connectionMode = mode; },
    useRestFallback: () => { context.fallbacks++; context.liveOpen = false; },
    updateControls: () => { context.controlsUpdated++; },
    now: 0, status: null, online: false, receivedAt: 0, statusFreshnessMs: 25000,
    pageStartedAt: 0, connectionFailed: false, busy: false, liveSuspended: false,
    liveOpen: false, fallbacks: 0, controlsUpdated: 0 });
  vm.runInContext(webSource.slice(webSource.indexOf('  function statusIsFresh('),
    webSource.indexOf('  function formatTime(')), context);
  return { context, $, fresh: () => context.statusIsFresh(), update: () => context.updateConnectionHealth() };
}

test('15-second SSE cadence stays fresh; stalled stream falls back once and cannot report confirmed unlock', () => {
  const h = freshnessHarness();
  Object.assign(h.context, { status: { state: 'open' }, online: true, liveOpen: true });
  h.context.now = 15000;
  h.update();
  assert.equal(h.fresh(), true);
  assert.equal(h.$('connectionWarning').classList.contains('hidden'), true);
  h.context.now = 25001;
  h.update();
  h.update();
  assert.equal(h.fresh(), false);
  assert.equal(h.$('connectionWarning').classList.contains('hidden'), false);
  assert.equal(h.$('stateBadge').textContent, 'UNKNOWN');
  assert.equal(h.$('safetyNote').textContent, 'The current lock state is not confirmed.');
  assert.equal(h.context.fallbacks, 1);
  Object.assign(h.context, { receivedAt: 25001, online: true, connectionFailed: false });
  h.update();
  assert.equal(h.fresh(), true);
  assert.equal(h.$('connectionWarning').classList.contains('hidden'), true);
});

test('request failure warns immediately; deliberate OTA pause does not reconnect or show competing warning', () => {
  const h = freshnessHarness();
  h.context.connectionFailed = true;
  h.update();
  assert.equal(h.$('connectionWarning').classList.contains('hidden'), false);
  Object.assign(h.context, { busy: true, liveSuspended: true, liveOpen: true });
  h.update();
  assert.equal(h.$('connectionWarning').classList.contains('hidden'), true);
  assert.equal(h.context.fallbacks, 0);
});
