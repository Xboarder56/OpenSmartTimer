import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import test from 'node:test';
import vm from 'node:vm';

const source = readFileSync(new URL('../../web/appearance.js', import.meta.url), 'utf8');
function harness(saved, storageFails = false) {
  const writes = [], events = {};
  const root = {dataset: {theme: 'dark'}};
  const meta = {setAttribute(name, value) { this[name] = value; }};
  const buttons = ['purple', 'slate', 'teal'].map(palette => ({
    dataset: {paletteChoice: palette},
    setAttribute(name, value) { this[name] = value; },
    addEventListener(name, fn) { this[name] = fn; },
    focus() {},
  }));
  let observeTheme;
  const context = vm.createContext({document: {
    documentElement: root, querySelector: () => meta,
    querySelectorAll: selector => selector === '[data-palette-choice]' ? buttons : [],
  }, window: {addEventListener: (event, fn) => { events[event] = fn; }},
  localStorage: {
    getItem() { if (storageFails) throw Error('blocked'); return saved; },
    setItem(key, value) { if (storageFails) throw Error('blocked'); writes.push([key, value]); },
  }, MutationObserver: class {
    constructor(fn) { observeTheme = fn; }
    observe() {}
  }, fetch() { assert.fail('palette selection must never send device/network requests'); }});
  vm.runInContext(source, context);
  return {root, meta, buttons, writes, events, observeTheme};
}

test('Purple is the default; preferences are browser-local and independent of light/dark', () => {
  const h = harness(null);
  assert.equal(h.root.dataset.palette, 'purple');
  assert.deepEqual(h.writes, []);
  h.buttons[2].click();
  assert.equal(h.root.dataset.palette, 'teal');
  assert.equal(h.root.dataset.theme, 'dark');
  assert.deepEqual(h.buttons.map(b => b['aria-pressed']), ['false', 'false', 'true']);
  assert.deepEqual(h.writes, [['smartTimerPalette', 'teal']]);
  assert.equal(harness(h.writes[0][1]).root.dataset.palette, 'teal');
  h.root.dataset.theme = 'light'; h.observeTheme();
  assert.equal(h.root.dataset.palette, 'teal');
  assert.equal(h.meta.content, '#f1f8f7');
  assert.equal(h.writes.length, 1);
});

test('blocked or invalid storage falls back safely; same-origin tabs follow saved choices', () => {
  for (const saved of [null, '', 'unknown', '<script>']) assert.equal(harness(saved).root.dataset.palette, 'purple');
  const h = harness('teal', true);
  assert.equal(h.root.dataset.palette, 'purple');
  h.buttons[1].click();
  assert.equal(h.root.dataset.palette, 'slate');
  assert.deepEqual(h.writes, []);
  h.events.storage({key: 'unrelated', newValue: 'teal'});
  assert.equal(h.root.dataset.palette, 'slate');
  h.events.storage({key: 'smartTimerPalette', newValue: 'teal'});
  assert.equal(h.root.dataset.palette, 'teal');
  h.events.storage({key: null, newValue: null});
  assert.equal(h.root.dataset.palette, 'purple');
});

test('separately deployed relay uses the same preference behavior', () => {
  assert.equal(readFileSync(new URL('../../relay/spa/appearance.js', import.meta.url), 'utf8'), source);
});
