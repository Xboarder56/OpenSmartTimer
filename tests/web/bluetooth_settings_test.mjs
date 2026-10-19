import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import test from 'node:test';
import vm from 'node:vm';

const source = readFileSync(new URL('../../bluetooth-app/app.js', import.meta.url), 'utf8');
const section = (start, end) => source.slice(source.indexOf(start), source.indexOf(end));

function fixture() {
  const nodes = new Map();
  const document = { activeElement: null, querySelector: () => get('shell') };
  function get(id) {
    if (!nodes.has(id)) {
      const classes = new Set(), listeners = new Map(), attributes = new Map();
      nodes.set(id, { id, inert: false, isConnected: true, disabled: false, value: '',
        get offsetParent() { return id === 'mgmtConfirmName' && get('mgmtConfirmField').classList.contains('hidden') ? null : {}; },
        classList: { add: key => classes.add(key), remove: key => classes.delete(key), contains: key => classes.has(key),
          toggle: (key, on) => on ? classes.add(key) : classes.delete(key) },
        setAttribute: (key, value) => attributes.set(key, value), getAttribute: key => attributes.get(key),
        removeAttribute: key => attributes.delete(key),
        addEventListener: (key, listener) => { if (!listeners.has(key)) listeners.set(key, new Set()); listeners.get(key).add(listener); },
        removeEventListener: (key, listener) => listeners.get(key)?.delete(listener),
        click() { [...listeners.get('click') || []].forEach(listener => listener()); },
        focus() { document.activeElement = this; }, select() {},
        querySelectorAll: () => [get('mgmtConfirmName'), get('mgmtConfirmCancel'), get('mgmtConfirmAccept')],
      });
    }
    return nodes.get(id);
  }
  document.activeElement = get('renameTrigger');
  get('settingsPanel').classList.add('show');
  get('shell').inert = true;
  let settingsClosed = 0;
  const app = {};
  const context = vm.createContext({ app, document, TextEncoder, $: get, t: key => key,
    setText: (id, text) => { get(id).textContent = text; },
    closeSettings: () => { settingsClosed++; }, closeHardwareGuide() {},
  });
  vm.runInContext(section('function validDeviceLabel(', '// Holds the shared counter:'), context);
  vm.runInContext(section('function trapFocus(', 'function collectLogText('), context);
  return { context, app, get, document, settingsClosed: () => settingsClosed,
    key: (key, shiftKey = false) => context.handlePanelKeydown({ key, shiftKey, preventDefault() {} }) };
}

test('management confirmation owns focus, Escape cancels only the top dialog and restores prior inert state', async () => {
  const f = fixture();
  const result = f.context.confirmManagement('Restart?', 'Restart closes this connection.');
  assert.equal(f.get('settingsPanel').inert, true);
  assert.equal(f.document.activeElement.id, 'mgmtConfirmCancel');
  f.get('mgmtConfirmAccept').focus(); f.key('Tab');
  assert.equal(f.document.activeElement.id, 'mgmtConfirmCancel');
  f.key('Escape');
  assert.equal(await result, false);
  assert.equal(f.settingsClosed(), 0);
  assert.equal(f.get('settingsPanel').inert, false);
  assert.equal(f.get('shell').inert, true);
  assert.equal(f.document.activeElement.id, 'renameTrigger');
  assert.equal(f.get('mgmtConfirmBackdrop').classList.contains('show'), false);
});

test('device rename edits its own field, rejects excessive UTF-8 and accepts Enter', async () => {
  const f = fixture();
  f.get('mgmtPairLabel').value = 'Unrelated new device';
  const result = f.context.confirmManagement('Rename', 'Name', 'Save name', 'Tablet');
  assert.equal(f.get('mgmtConfirmName').value, 'Tablet');
  assert.equal(f.document.activeElement.id, 'mgmtConfirmName');
  f.get('mgmtConfirmName').value = '中'.repeat(9);
  f.key('Enter');
  assert.equal(f.get('mgmtConfirm').classList.contains('show'), true);
  assert.equal(f.get('mgmtConfirmName').getAttribute('aria-invalid'), 'true');
  f.get('mgmtConfirmName').value = ' 中'.repeat(6).trim();
  f.key('Enter');
  assert.equal(await result, ' 中'.repeat(6).trim());
  assert.equal(f.get('mgmtPairLabel').value, 'Unrelated new device');
  for (const name of ['', 'bad\0name', 'bad\x7fname', 'a'.repeat(25)]) assert.equal(f.context.validDeviceLabel(name), false);
  assert.equal(f.context.validDeviceLabel('中'.repeat(8)), true);
});

test('replaced dialogs, backdrop and disconnect cancel pending intent without accepting it', async () => {
  const f = fixture();
  const first = f.context.confirmManagement('Old', 'Old');
  const second = f.context.confirmManagement('New', 'New');
  assert.equal(await first, false);
  f.get('mgmtConfirmBackdrop').click();
  assert.equal(await second, false);
  const third = f.context.confirmManagement('Pending', 'Pending');
  f.app.managementConfirmationCancel();
  f.get('mgmtConfirmAccept').click();
  assert.equal(await third, false);
  assert.equal(f.app.managementConfirmationCancel, null);
  assert.match(section('function handleDisconnected()', 'function disconnectTimer('), /managementConfirmationCancel\?\.\(\)/);
});
