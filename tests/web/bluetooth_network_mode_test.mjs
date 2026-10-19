import assert from 'node:assert/strict';
import {readFileSync} from 'node:fs';
import test from 'node:test';
import vm from 'node:vm';
import {MGMT_OPCODES} from '../../bluetooth-app/ble-management.mjs';
const source = readFileSync(new URL('../../bluetooth-app/app.js', import.meta.url), 'utf8');
function fixture() {
  const nodes = new Map();
  function get(id) {
    if (!nodes.has(id)) {
      const classes = new Set();
      nodes.set(id, {value: '', checked: false, disabled: false,
        classList: {toggle: (k, on) => on ? classes.add(k) : classes.delete(k), contains: k => classes.has(k)},
        addEventListener(type, fn) { this[type] = fn; },
      });
    }
    return nodes.get(id);
  }
  const requests = [];
  const app = {server: {connected: true}, authenticated: true, live: {state: 'open', controls_locked: false}};
  const context = vm.createContext({app, $: get, document: {querySelectorAll: () => []},
    window: {SmartTimerUpdates: {panel: () => {}, notice: () => ({status: async () => {}, refresh: () => {}})}},
    installFirmwareOverBluetooth: async () => {},
    t: text => text, setText: (id, text) => { get(id).textContent = text; },
    managementSupported: () => true, MGMT_OPCODES,
    applyChange: (...args) => requests.push(args), refreshSystemInfo: async () => {},
  });
  for (const name of ['timerOpenForSettings','renderManagementAvailability','renderWifi','wireTimerSettings']) {
    vm.runInContext(source.match(new RegExp(`^function ${name}\\([^\\n]*\\) \\{[\\s\\S]*?^\\}`, 'm'))[0], context);
  }
  context.wireTimerSettings();
  return {app, context, get, requests};
}

test('Bluetooth-only controls are capability gated, unlocked-only, and disable conflicting actions', () => {
  const f = fixture();
  f.context.renderWifi({network_mode: 'wifi'});
  assert(f.get('mgmtNetworkMode').classList.contains('hidden'));
  assert(f.get('mgmtWifiMode').disabled);
  f.context.renderWifi({state: 'connected', network_mode: 'wifi', wifi_suspend_supported: true});
  assert(!f.get('mgmtNetworkMode').classList.contains('hidden'));
  assert(!f.get('mgmtWifiMode').disabled);
  for (const state of ['running','paused']) {
    f.app.live.state = state; f.context.renderManagementAvailability();
    assert(f.get('mgmtWifiMode').disabled);
  }
  f.app.live.state = 'open';
  f.context.renderWifi({state: 'disabled', network_mode: 'bluetooth', wifi_suspend_supported: true});
  assert.equal(f.get('mgmtWifiState').textContent, 'Bluetooth only (until restart)');
  assert.equal(f.get('mgmtWifiMode').textContent, 'Restore Wi-Fi / AP');
  for (const id of ['mgmtWifiScan','mgmtWifiJoin','mgmtWifiForget','mgmtApSave','mgmtBluetoothOff']) assert(f.get(id).disabled, id);
  for (const id of ['mgmtWifiSsid','mgmtWifiPassword','mgmtWifiNetworks','mgmtApSsid','mgmtApPassword','mgmtApUseName','mgmtApSecured','mgmtApStandalone']) assert(f.get(id).disabled, id);
  assert(!f.get('mgmtWifiMode').disabled);
  assert(!f.get('mgmtRestart').disabled);
  f.app.server.connected = false; f.context.renderManagementAvailability();
  assert(f.get('mgmtRestart').disabled, 'restart in Diagnostics must disable after disconnect');
});

test('new firmware advertises disconnect recovery and old firmware keeps truthful recovery guidance', () => {
  const f=fixture();
  f.context.renderWifi({state:'connected',network_mode:'wifi',wifi_suspend_supported:true,wifi_disconnect_restore_supported:true});
  assert.equal(f.get('mgmtWifiMode').textContent,'Use Bluetooth only while connected');
  assert.match(f.get('mgmtNetworkPolicy').textContent,/restored when Bluetooth disconnects/);
  f.get('mgmtWifiMode').click();
  assert.match(f.requests[0][2],/automatically restores/);
  assert.match(f.requests[0][2],/recovery AP/);
  f.context.renderWifi({state:'disabled',network_mode:'bluetooth',wifi_suspend_supported:true,wifi_disconnect_restore_supported:true});
  assert.equal(f.get('mgmtWifiState').textContent,'Bluetooth only while connected.');
  f.context.renderWifi({state:'connected',network_mode:'wifi',wifi_suspend_supported:true});
  assert.equal(f.get('mgmtWifiMode').textContent,'Use Bluetooth only until restart');
  assert.match(f.get('mgmtNetworkPolicy').textContent,/unplug/);
});

test('mode selection uses explicit authenticated operations and renders the returned state', () => {
  const f = fixture();
  f.context.renderWifi({state: 'connected', network_mode: 'wifi', wifi_suspend_supported: true});
  f.get('mgmtWifiMode').click();
  assert.equal(f.requests[0][3], MGMT_OPCODES.wifiSuspend);
  assert.match(f.requests[0][2], /Unplug and reconnect power/);
  assert.deepEqual(Object.keys(f.requests[0][4]), []);
  f.requests[0][6]({state: 'disabled', network_mode: 'bluetooth', wifi_suspend_supported: true});
  f.get('mgmtWifiMode').click();
  assert.equal(f.requests[1][3], MGMT_OPCODES.wifiResume);
  f.requests[1][6]({state: 'standalone', network_mode: 'ap', wifi_suspend_supported: true});
  assert.equal(f.get('mgmtWifiState').textContent, 'AP Mode');
});
