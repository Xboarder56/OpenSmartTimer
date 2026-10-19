import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import test from 'node:test';
import vm from 'node:vm';

const source = readFileSync(new URL('../../web/app.js', import.meta.url), 'utf8');
function harness() {
  const context = vm.createContext({ now: 100, performance: { now: () => context.now },
    online: true, relayFresh: true, relayReceivedAt: 100,
    status: { state: 'open' },
    relayCache: { enrolled: true, enabled: true, state: 'connected', sharing_available: true,
      partner_origin: 'https://control.example.com',
      live_links: 0, link_limit: 1 } });
  vm.runInContext(source.slice(source.indexOf('  function relayStatusIsFresh('),
    source.indexOf('  function relayExpiry(')), context);
  vm.runInContext(source.slice(source.indexOf('  function relayCreateBlock('),
    source.indexOf('  function updateRelayControls(')), context);
  return context;
}

test('configured relay distinguishes online, offline, Bluetooth pause, busy and off', () => {
  const h = harness();
  assert.equal(h.relayAvailability(h.relayCache, true).label, 'RELAY ONLINE');
  for (const [values, label] of [
    [{ state: 'backoff', sharing_available: false }, 'RELAY OFFLINE'],
    [{ paused_for_bluetooth: true }, 'RELAY PAUSED'],
    [{ maintenance: true }, 'RELAY BUSY'],
    [{ enabled: false }, 'RELAY OFF'],
  ]) assert.equal(h.relayAvailability({ ...h.relayCache, ...values }, true).label, label);
  assert.equal(h.relayAvailability({ enrolled: false }, true), null);
});

test('a failed request or stale cached connection cannot authorize link creation', () => {
  const h = harness();
  assert.equal(h.relayCreateBlock(), '');
  h.relayFresh = false;
  assert.equal(h.relayCreateBlock(), 'unavailable');
  assert.equal(h.relayAvailability(h.relayCache, h.relayStatusIsFresh()).label, 'RELAY UNAVAILABLE');
  h.relayFresh = true;
  h.now = 15100;
  assert.equal(h.relayCreateBlock(), 'unavailable');
  h.now = 100;
  h.online = false;
  assert.equal(h.relayCreateBlock(), 'unavailable');
});

test('creation respects relay readiness, disabled state, Bluetooth, timer state and link capacity', () => {
  const h = harness();
  for (const [values, reason] of [
    [{ state: 'backoff', sharing_available: false }, 'offline'],
    [{ enabled: false }, 'off'],
    [{ paused_for_bluetooth: true }, 'paused'],
    [{ maintenance: true }, 'busy'],
    [{ setup_pending: true }, 'busy'],
    [{ partner_origin: '' }, 'off'],
    [{ live_links: 1 }, 'full'],
  ]) {
    const original = h.relayCache;
    h.relayCache = { ...original, ...values };
    assert.equal(h.relayCreateBlock(), reason);
    h.relayCache = original;
  }
  h.status.state = 'running';
  assert.equal(h.relayCreateBlock(), 'open');
});
