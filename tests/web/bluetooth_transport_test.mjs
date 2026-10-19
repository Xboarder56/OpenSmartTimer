import assert from 'node:assert/strict';
import { test } from 'node:test';
import { readFileSync } from 'node:fs';
import vm from 'node:vm';
import * as transport from '../../bluetooth-app/ble-transport.mjs';
import * as management from '../../bluetooth-app/ble-management.mjs';

const { GattQueue, resultMatches, exchangeCommand, snapshotFromResult, validPendingOperation } = transport;
const operation = { schema: 1, deviceId: 'abc123', op: 'release', opcode: 0x11,
  requestId: 31, bootId: 42, sessionSequence: 7, valueA: 0, valueB: 0 };
const result = { request_id: 31, opcode: 0x11, boot_id: 42, session_sequence: 7,
  result: 'applied', state: 'open', remaining_ms: 0, changed: true, duplicate: false };
const delay = ms => new Promise(resolve => setTimeout(resolve, ms));

test('one GATT queue serializes reads, writes, failures and notification setup', async () => {
  const queue = new GattQueue();
  const seen = [];
  const a = queue.run(async () => { seen.push('read'); await delay(5); seen.push('read done'); });
  const b = queue.run(() => { seen.push('write'); throw Error('write failed'); });
  const c = queue.run(() => seen.push('subscribe'));
  await Promise.all([a, assert.rejects(b, /write failed/), c]);
  assert.deepEqual(seen, ['read', 'read done', 'write', 'subscribe']);
});

test('disconnect invalidates in-flight and queued work; timeout closes the connection', async () => {
  let closeCount = 0;
  const queue = new GattQueue({ timeoutMs: 10, onTimeout: () => closeCount++ });
  const stalled = queue.run(() => new Promise(() => {}));
  let writes = 0;
  const queued = queue.run(() => writes++);
  await Promise.all([assert.rejects(stalled, /timed out/), assert.rejects(queued, /connection changed/)]);
  assert.equal(closeCount, 1);
  assert.equal(writes, 0);
  const inFlight = queue.run(async () => { queue.reset(); return 1; });
  await assert.rejects(inFlight, /connection changed/);
  await queue.run(() => writes++);
  assert.equal(writes, 1);
});

test('correlation checks ID, opcode, boot, session and resulting state', () => {
  assert.ok(resultMatches(operation, result));
  assert.equal(resultMatches(operation, { ...result, request_id: 9 }), false);
  for (const corrupt of [{ opcode: 0x10 }, { boot_id: 99 }, { session_sequence: 8 },
    { state: 'running' }, { remaining_ms: 1 }, { state: 'garbage' }])
    assert.throws(() => resultMatches(operation, { ...result, ...corrupt }));
  assert.ok(resultMatches(operation, { ...result, result: 'stale_boot', boot_id: 99 }));
  assert.ok(resultMatches(operation, { ...result, result: 'stale_session', session_sequence: 8 }));
  assert.ok(resultMatches({ ...operation, opcode: 0x10, sessionSequence: 0 },
    { ...result, opcode: 0x10, state: 'running', remaining_ms: 1000 }));
  assert.throws(() => resultMatches({ ...operation, opcode: 0x10 },
    { ...result, opcode: 0x10, state: 'running', remaining_ms: 1000, session_sequence: 0 }));
});

test('write ACK loss and lost notifications recover from the correlated cached result', async () => {
  for (const loseAck of [false, true]) {
    let writes = 0;
    const answer = await exchangeCommand({ operation, frame: new Uint8Array(32),
      connected: () => true, write: async () => { writes++; if (loseAck) throw Error('ACK lost'); },
      wait: async () => { throw Error('notification lost'); }, read: async () => result });
    assert.equal(answer, result);
    assert.equal(writes, 1);
  }
});

test('same-connection retry uses the identical signed bytes and request ID', async () => {
  const frame = new Uint8Array(32).fill(19);
  const writes = [];
  let reads = 0;
  const answer = await exchangeCommand({ operation, frame, connected: () => true,
    write: async bytes => writes.push(bytes), wait: async () => { throw Error('lost'); },
    read: async () => ++reads === 1 ? { ...result, request_id: 1 } : { ...result, duplicate: true } });
  assert.equal(answer.duplicate, true);
  assert.deepEqual(writes, [frame, frame]);
  assert.equal(writes[0], writes[1]);
});

test('write exception never means not applied; unrelated/corrupt results remain unknown', async () => {
  for (const cached of [{ ...result, request_id: 2 }, { ...result, opcode: 0x10 }, { ...result, boot_id: 99 }]) {
    let writes = 0;
    await assert.rejects(exchangeCommand({ operation, frame: new Uint8Array(32), connected: () => true,
      write: async () => { writes++; throw Error('ACK lost'); },
      wait: async () => { throw Error('no result'); }, read: async () => cached }),
    error => error.outcome === 'unknown' && error.operation === operation);
    assert.equal(writes, 2);
  }
});

test('a result received before a disconnect still confirms the operation', async () => {
  let connected = true;
  const answer = await exchangeCommand({ operation, frame: new Uint8Array(32), connected: () => connected,
    write: async () => { connected = false; throw Error('ACK lost'); },
    wait: async () => result, read: async () => { throw Error('disconnected'); } });
  assert.equal(answer, result);
});

test('fresh results update immediately; historical duplicate results do not overwrite current state', () => {
  const before = { state: 'running', remaining_ms: 9000, session_duration_ms: 10000,
    controls_locked: false, event_revision: 5 };
  const next = snapshotFromResult(before, operation, result);
  assert.equal(next.state, 'open');
  assert.equal(next.remaining_ms, 0);
  assert.equal(next.lock_output_energized, false);
  assert.equal(next.event_revision, 6);
  assert.equal(snapshotFromResult(before, operation, { ...result, duplicate: true }), null);
  assert.equal(snapshotFromResult(before, operation, { ...result, result: 'stale_session' }), null);
});

// Exercise the actual application command functions, not a reimplementation.
// Only DOM rendering, Web Crypto and the hardware boundary are replaced.
function appHarness(storage = new Map()) {
  let now = 1000;
  const nodes = new Map();
  const elements = id => {
    if (!nodes.has(id)) {
      const classes = new Set();
      nodes.set(id, { disabled: false, textContent: '', style: { setProperty: () => {} }, replaceChildren: () => {},
        classList: { remove: name => classes.delete(name), toggle: (name, on) => on ? classes.add(name) : classes.delete(name),
          contains: name => classes.has(name) } });
    }
    return nodes.get(id);
  };
  const context = vm.createContext({ ...transport, ...management, Uint8Array, DataView, TextEncoder, TextDecoder,
    setTimeout, clearTimeout, performance: { now: () => now }, console,
    requestAnimationFrame: () => {},
    document: { getElementById: elements, querySelectorAll: () => [], hidden: false,
      documentElement: { dataset: {} } }, window: {},
    localStorage: { getItem: key => storage.get(key) ?? null,
      setItem: (key, value) => storage.set(key, value), removeItem: key => storage.delete(key) },
  });
  let source = readFileSync(new URL('../../bluetooth-app/app.js', import.meta.url), 'utf8');
  source = source.replace(/import \{[\s\S]*?\} from '\.\/ble-(?:transport|management)\.mjs';/g, '');
  source = source.slice(0, source.lastIndexOf('initialize().catch('));
  vm.runInContext(source + `
    const feedback = [];
    log = () => {};
    showFeedback = (...args) => feedback.push(args);
    showCommandSuccess = (op, result) => feedback.push(['success', op, result]);
    const actualRenderLive = renderLive;
    const actualReadGatt = readGatt;
    renderLive = next => { app.live = next; app.sessionSequence = next.session_sequence; };
    signCommand = async frame => frame;
    allocateRequestId = () => 31;
    Object.assign(app, { server: { connected: true }, authenticated: true, bootId: 42,
      sessionSequence: 7, protocolInfo: { device_id: 'abc123' },
      liveNeedsRead: false, liveConfirmedAt: performance.now(), receivedAt: performance.now(),
      chars: { liveStatus: 'status', systemInfo: 'system' },
      live: { state: 'running', remaining_ms: 9000, session_duration_ms: 10000,
        session_sequence: 7, controls_locked: false, event_revision: 5 } });
    function liveFrame(snapshot = app.live) {
      const frame = new Uint8Array(20);
      const view = new DataView(frame.buffer);
      frame[0] = 1;
      frame[1] = STATE_NAMES.indexOf(snapshot.state);
      frame[2] = snapshot.controls_locked ? 1 : 0;
      view.setUint32(4, snapshot.remaining_ms, true);
      view.setUint32(8, snapshot.session_duration_ms, true);
      view.setUint32(12, snapshot.event_revision, true);
      view.setUint32(16, snapshot.session_sequence, true);
      return frame;
    }
    readGatt = async () => liveFrame();
    globalThis.subject = { app, feedback, gattQueue, executeCommand, enqueueCommand, loadPendingOperation,
      reconcilePendingOperation, updateControlAvailability, acceptLiveSnapshot, commandIntent,
      liveStatusFresh, readLiveSnapshot, refreshLiveStatus, maintainLiveStatus, handlePageVisibility,
      invalidateLiveStatus, onStatusFrame, refreshSystemInfo, renderClock, renderStatusFreshness,
      requestControlLock };
  `, context);
  return { ...context.subject, context, storage, nodes, advance: ms => { now += ms; },
    run: code => vm.runInContext(code, context) };
}

test('actual app clears busy and propagates preparation failures before sending', async () => {
  for (const injection of ["allocateRequestId = () => { throw Error('storage failed'); }",
    "signCommand = async () => { throw Error('crypto failed'); }",
    "localStorage.setItem = () => { throw Error('quota failed'); }"]) {
    const h = appHarness();
    h.run(injection + "; writeGatt = async () => { throw Error('must not write'); };");
    await assert.rejects(h.enqueueCommand('release'), error => error.outcome === 'not_sent');
    assert.equal(h.app.commandBusy, false);
    assert.equal(h.app.pendingOperation, null);
    assert.equal(h.feedback.at(-1)[1], 'Command not sent');
  }
});

test('management self-revoke clears only this timer key after a verified receipt; lost replies keep it', async () => {
  for (const lost of [false, true]) {
    const h = appHarness();
    h.run(`
      app.credentialScope = 'abc123'; app.activeKey = new Uint8Array(16);
      globalThis.deleted = []; globalThis.closed = 0; globalThis.messages = [];
      confirmManagement = async () => true;
      runManagement = async (_, work) => messages.push(await work());
      disconnectTimer = () => { closed++; };
      credentialDelete = async (name, scope) => deleted.push([name, scope]);
      updateCredentialUi = () => {};
      manageJson = async () => ${lost ? "{ throw Error('lost response'); }" : "({ status: 200, json: { approval_removed: true, disconnecting: true } })"};
    `);
    const action = h.run("applyChange('Remove', 'Confirm', 'Text', MGMT_OPCODES.appRevoke, {app_id:2})");
    if (lost) {
      await assert.rejects(action, /may have applied/);
      assert.equal(h.context.deleted.length, 0);
      assert.ok(h.app.activeKey);
    } else {
      await action;
      assert.equal(h.context.closed, 1);
      assert.deepEqual(Array.from(h.context.deleted, pair => Array.from(pair)), [['active', 'abc123'], ['pending', 'abc123']]);
      assert.equal(h.app.activeKey, null);
    }
  }
});

test('actual app journals unknown operations before writing, restores on reload and never resends during reconciliation', async () => {
  const h = appHarness();
  h.run(`
    writeGatt = async () => {
      if (!app.pendingOperation) throw Error('journal missing');
      app.server.connected = false;
      app.pendingResult.reject(Error('connection lost'));
      throw Error('write acknowledgement lost');
    };
  `);
  await assert.rejects(h.enqueueCommand('adjust', { valueA: 60000 }), error => error.outcome === 'unknown');
  assert.equal(h.feedback.at(-1)[1], 'Command outcome unknown');
  assert.equal(h.app.commandBusy, false);
  assert.equal(h.app.pendingOperation.requestId, 31);
  const reloaded = appHarness(h.storage);
  reloaded.loadPendingOperation();
  assert.equal(reloaded.app.pendingOperation.op, 'adjust');
  reloaded.updateControlAvailability();
  assert.equal(reloaded.nodes.get('stopButton').disabled, false);
  assert.equal(reloaded.nodes.get('pauseButton').disabled, true);
  reloaded.run(`
    writeGatt = async () => { throw Error('reconciliation must not write'); };
    readGatt = async () => new Uint8Array(20);
    decodeResult = () => ({ request_id: 31, opcode: 0x14, boot_id: 42, session_sequence: 7,
      result: 'applied', state: 'running', remaining_ms: 69000, duplicate: true });
    readLiveSnapshot = async () => app.live;
  `);
  await reloaded.reconcilePendingOperation();
  assert.equal(reloaded.app.pendingOperation, null);
  assert.equal(reloaded.app.live.remaining_ms, 9000); // Current snapshot, not historical cached total.
  assert.equal(reloaded.feedback.at(-1)[1], 'Previous command confirmed');
});

test('actual app rejects stale queued intent and prevents stale status undoing release', async () => {
  const h = appHarness();
  const intent = h.commandIntent('release', {});
  h.gattQueue.reset();
  await assert.rejects(h.executeCommand('release', {}, intent), error => error.outcome === 'not_sent');
  h.acceptLiveSnapshot({ ...h.app.live, state: 'open', remaining_ms: 0, event_revision: 6 });
  h.acceptLiveSnapshot({ ...h.app.live, state: 'running', remaining_ms: 9000, event_revision: 5 });
  assert.equal(h.app.live.state, 'open');
});

test('actual app refreshes after rejection and does not forget an older unknown adjustment when unlock is rejected', async () => {
  const h = appHarness();
  h.app.pendingOperation = { ...operation, op: 'adjust', opcode: 0x14, requestId: 30, valueA: 60000 };
  h.run(`
    writeGatt = async () => app.pendingResult.resolve({ request_id: 31, opcode: 0x11,
      boot_id: 42, session_sequence: 8, result: 'stale_session' });
    readLiveSnapshot = async () => { app.live.state = 'paused'; return app.live; };
  `);
  await assert.rejects(h.executeCommand('release'), error => error.outcome === 'rejected');
  assert.equal(h.app.live.state, 'paused');
  assert.equal(h.app.pendingOperation.requestId, 30);
  assert.equal(h.app.commandBusy, false);
  assert.ok(validPendingOperation(h.app.pendingOperation));
});

test('reboot reconciliation never treats a colliding old request ID as current proof', async () => {
  const h = appHarness();
  h.app.pendingOperation = operation;
  h.app.bootId = 43;
  h.run(`readGatt = async () => new Uint8Array(20);
    decodeResult = () => ({ request_id: 31, opcode: 0x10, boot_id: 43 });
    readLiveSnapshot = async () => app.live;`);
  await h.reconcilePendingOperation();
  assert.equal(h.app.pendingOperation, null);
  assert.equal(h.feedback.at(-1)[1], 'Previous result unavailable');
});

test('unknown Start keeps Unlock reachable and reads the current session before a new release', async () => {
  const h = appHarness();
  h.app.live.state = 'open';
  h.app.pendingOperation = { ...operation, op: 'start', opcode: 0x10, sessionSequence: 0, requestId: 30, valueA: 60000 };
  h.updateControlAvailability();
  assert.equal(h.nodes.get('stopButton').disabled, false);
  h.run(`
    let readCount = 0;
    readLiveSnapshot = async () => { readCount++; app.live.session_sequence = 8; return app.live; };
    writeGatt = async () => {
      if (!readCount || app.pendingOperation.sessionSequence !== 8) throw Error('guessed session');
      app.pendingResult.resolve({ request_id: 31, opcode: 0x11, boot_id: 42, session_sequence: 8,
        result: 'applied', state: 'open', remaining_ms: 0, changed: true });
    };
  `);
  const answer = await h.executeCommand('release');
  assert.equal(answer.state, 'open');
  assert.equal(h.app.pendingOperation, null);
});

test('actual app disables stale controls at 30 seconds and does not show an invented countdown', async () => {
  const h = appHarness();
  h.run('renderLive = actualRenderLive;');
  h.advance(29999);
  h.updateControlAvailability();
  assert.equal(h.liveStatusFresh(), true);
  assert.equal(h.nodes.get('pauseButton').disabled, false);
  h.advance(1);
  h.renderStatusFreshness();
  h.renderClock();
  assert.equal(h.liveStatusFresh(), false);
  assert.equal(h.nodes.get('pauseButton').disabled, true);
  assert.equal(h.nodes.get('stopButton').disabled, false);
  assert.equal(h.nodes.get('clock').textContent, '—');
  assert.equal(h.nodes.get('stateBadge').textContent, 'STATUS UNAVAILABLE');
  assert.equal(h.nodes.get('completionTime').textContent, 'Waiting for current timer status');
  h.run('writeGatt = async () => { throw Error("must not send a stale adjustment"); };');
  await assert.rejects(h.executeCommand('adjust', { valueA: 60000 }),
    error => error.outcome === 'not_sent' && /Current timer status/.test(error.message));
  assert.equal(h.app.pendingOperation, null);
  await h.readLiveSnapshot();
  assert.equal(h.liveStatusFresh(), true);
  assert.equal(h.nodes.get('pauseButton').disabled, false);
  assert.equal(h.nodes.get('stateBadge').textContent, 'RUNNING');
  h.renderClock();
  assert.equal(h.nodes.get('clock').textContent, '00:00:09');
});

test('only accepted live status refreshes freshness; diagnostics, old revisions and command results do not', async () => {
  const h = appHarness();
  h.advance(30000);
  h.run(`readGatt = async () => { const frame = new Uint8Array(44); frame[0] = 1; return frame; };`);
  await h.refreshSystemInfo();
  assert.equal(h.app.systemReceivedAt, 31000);
  assert.equal(h.app.liveConfirmedAt, 1000);
  assert.equal(h.liveStatusFresh(), false);
  h.acceptLiveSnapshot({ ...h.app.live, event_revision: 6 }, false, false);
  assert.equal(h.liveStatusFresh(), false);
  h.acceptLiveSnapshot({ ...h.app.live, event_revision: 5 });
  assert.equal(h.liveStatusFresh(), false);
  h.run('onStatusFrame(liveFrame());');
  assert.equal(h.liveStatusFresh(), true);
  assert.equal(h.app.liveConfirmedAt, 31000);
});

test('foreground invalidates status and rejects reads and notifications queued before resume', async () => {
  const h = appHarness();
  h.run(`let resolveOldRead;
    readGatt = () => new Promise(resolve => { resolveOldRead = resolve; });`);
  const oldRead = h.readLiveSnapshot();
  h.context.document.hidden = true;
  h.handlePageVisibility();
  assert.equal(h.liveStatusFresh(), false);
  h.context.document.hidden = false;
  h.handlePageVisibility();
  h.run('onStatusFrame(liveFrame());');
  assert.equal(h.liveStatusFresh(), false);
  h.run('readGatt = async () => liveFrame(); resolveOldRead(liveFrame());');
  await assert.rejects(oldRead, /connection changed/i);
  // The single foreground refresh waits out the obsolete read, then starts a
  // new one. No stale notification can re-enable controls in between.
  await delay(0);
  assert.equal(h.liveStatusFresh(), true);
  assert.equal(h.app.liveReadTask, null);
  assert.equal(h.app.liveRefreshTask, null);
});

test('hidden app releases the single connection after a minute, never mid-command, and reconnects when shown', async () => {
  const h = appHarness(new Map([['ost.ble.app.deviceId', 'abc123']]));
  h.run(`var timers = []; var disconnects = 0; var connects = 0;
    setTimeout = (fn, ms) => { timers.push({ fn, ms }); return timers.length; };
    clearTimeout = id => { if (id) timers[id - 1].fn = null; };
    disconnectTimer = () => { disconnects++; app.server = null; };
    connectTimer = async () => { connects++; };
    app.activeKey = new Uint8Array(16);
    globalThis.navigator = { bluetooth: { getDevices: async () => [{ id: 'abc123' }] } };
    function fire() { const t = timers.filter(x => x.fn).pop(); const fn = t.fn; t.fn = null; fn(); return t.ms; }`);

  // Shown again before the deadline: nothing is released.
  h.context.document.hidden = true;
  h.handlePageVisibility();
  assert.equal(h.run('timers.filter(x => x.fn).length'), 1);
  assert.equal(h.run('timers[timers.length - 1].ms'), 60000);
  h.context.document.hidden = false;
  h.handlePageVisibility();
  assert.equal(h.run('timers.filter(x => x.fn).length'), 0);
  assert.equal(h.run('disconnects'), 0);

  // A command awaiting its result postpones the release instead of cutting it.
  h.context.document.hidden = true;
  h.handlePageVisibility();
  h.run('app.commandBusy = true;');
  assert.equal(h.run('fire()'), 60000);
  assert.equal(h.run('disconnects'), 0);
  assert.equal(h.run('timers.filter(x => x.fn).pop().ms'), 5000);
  h.run('app.commandBusy = false;');
  h.run('fire()');
  assert.equal(h.run('disconnects'), 1);
  assert.equal(h.app.releasedInBackground, true);

  // Returning reconnects only to the remembered, already permitted timer.
  h.context.document.hidden = false;
  h.handlePageVisibility();
  await delay(0);
  assert.equal(h.run('connects'), 1);
  assert.equal(h.app.releasedInBackground, false);

  // Without a remembered device the app asks for Connect instead of opening a chooser.
  h.run(`app.server = { connected: true }; navigator.bluetooth.getDevices = async () => [];`);
  h.context.document.hidden = true;
  h.handlePageVisibility();
  h.run('fire()');
  h.context.document.hidden = false;
  h.handlePageVisibility();
  await delay(0);
  assert.equal(h.run('connects'), 1);
  assert.equal(h.feedback.at(-1)[1], 'Reconnect to continue');
});

test('missing or delayed status recovery is single-flight, bounded and does not create a GATT backlog', async () => {
  const h = appHarness();
  h.advance(30000);
  h.run(`let reads = 0; let resolveRead;
    readGatt = () => { reads++; return new Promise(resolve => { resolveRead = resolve; }); };`);
  for (let i = 0; i < 50; i++) h.maintainLiveStatus();
  assert.equal(h.run('reads'), 1);
  const duplicate = h.readLiveSnapshot();
  assert.equal(h.run('reads'), 1);
  assert.equal(h.nodes.get('pauseButton').disabled, true);
  h.run('resolveRead(liveFrame());');
  await duplicate;
  await delay(0);
  assert.equal(h.liveStatusFresh(), true);
  h.advance(30000);
  h.run('readGatt = async () => { reads++; throw Error("status unavailable"); };');
  await h.refreshLiveStatus();
  assert.equal(h.run('reads'), 2);
  for (let i = 0; i < 50; i++) h.maintainLiveStatus();
  assert.equal(h.run('reads'), 2);
  assert.equal(h.liveStatusFresh(), false);
  h.advance(5000);
  await h.refreshLiveStatus();
  assert.equal(h.run('reads'), 3);
  h.context.document.hidden = true;
  h.advance(5000);
  await h.refreshLiveStatus();
  assert.equal(h.run('reads'), 3);

  // Exercise the actual GATT deadline as well as the app's coalescing logic.
  h.context.document.hidden = false;
  h.run(`readGatt = actualReadGatt;
    app.chars.liveStatus = { readValue: () => new Promise(() => {}) };
    gattQueue.timeoutMs = 10;
    disconnectTimer = () => { app.server.connected = false; };`);
  h.advance(5000);
  await h.refreshLiveStatus();
  assert.equal(h.app.server.connected, false);
  assert.equal(h.app.liveReadTask, null);
  assert.equal(h.app.liveRefreshTask, null);
});

test('invalid status and old-connection reads cannot confirm timer freshness', async () => {
  const h = appHarness();
  h.invalidateLiveStatus();
  for (const badFrame of ['new Uint8Array(0)', 'Uint8Array.from([1, 255, ...new Uint8Array(18)])']) {
    h.run(`readGatt = async () => ${badFrame};`);
    await assert.rejects(h.readLiveSnapshot(), /Invalid/);
    assert.equal(h.liveStatusFresh(), false);
  }
  h.run(`let resolveRead;
    readGatt = () => new Promise(resolve => { resolveRead = resolve; });`);
  const read = h.readLiveSnapshot();
  h.gattQueue.reset();
  h.run('resolveRead(liveFrame());');
  await assert.rejects(read, /connection changed/i);
  assert.equal(h.liveStatusFresh(), false);
  assert.equal(h.app.liveConfirmedAt, null);
});

test('every explicit Unlock reads the current session and respects newly observed locked controls', async () => {
  for (const locked of [false, true]) {
    const h = appHarness();
    h.advance(30000);
    h.run(`let reads = 0; let writes = 0;
      readGatt = async () => { reads++; return liveFrame({ ...app.live, session_sequence: 8, controls_locked: ${locked} }); };
      writeGatt = async () => {
        writes++;
        if (!reads || app.pendingOperation.sessionSequence !== 8) throw Error('stale session used');
        app.pendingResult.resolve({ request_id: 31, opcode: 0x11, boot_id: 42, session_sequence: 8,
          result: 'applied', state: 'open', remaining_ms: 0, changed: true });
      };`);
    if (locked) {
      await assert.rejects(h.executeCommand('release'),
        error => error.outcome === 'not_sent' && /Software controls are locked/.test(error.message));
      assert.equal(h.run('writes'), 0);
      assert.equal(h.app.pendingOperation, null);
    } else {
      const answer = await h.executeCommand('release');
      assert.equal(answer.state, 'open');
      assert.equal(h.run('writes'), 1);
    }
    assert.ok(h.run('reads') >= 1);
    assert.equal(h.app.commandBusy, false);
  }
});

test('Unlock fails without sending when current status cannot be read; an unresolved journal is preserved', async () => {
  const h = appHarness();
  h.advance(30000);
  const previous = { ...operation, op: 'adjust', opcode: 0x14, requestId: 30, valueA: 60000 };
  h.app.pendingOperation = previous;
  h.run(`readGatt = async () => { throw Error('status missing'); };
    writeGatt = async () => { throw Error('must not write'); };`);
  await assert.rejects(h.executeCommand('release'), error => error.outcome === 'not_sent' && /status missing/.test(error.message));
  assert.equal(h.app.pendingOperation, previous);
  assert.equal(h.app.commandBusy, false);
});

test('freshness is rechecked after asynchronous signing and before journaling or sending', async () => {
  const h = appHarness();
  h.context.advanceClock = h.advance;
  h.run(`signCommand = async frame => { advanceClock(30000); return frame; };
    writeGatt = async () => { throw Error('must not write'); };`);
  await assert.rejects(h.executeCommand('adjust', { valueA: 60000 }),
    error => error.outcome === 'not_sent' && /status became unavailable/i.test(error.message));
  assert.equal(h.app.pendingOperation, null);
  assert.equal(h.app.commandBusy, false);
});

test('Bluetooth control lock waits for approval; Cancel leaves no command or journal', async () => {
  const h = appHarness();
  h.run(`var reads = 0, writes = 0;
    readGatt = async () => { reads++; return liveFrame(); };
    writeGatt = async () => { writes++; };
    var answer;
    confirmManagement = () => new Promise(resolve => { answer = resolve; });`);
  const pending = h.requestControlLock();
  assert.equal(h.app.lockConfirmationPending, true);
  assert.equal(h.nodes.get('sealButton').disabled, true);
  await h.requestControlLock(); // Duplicate activation cannot replace the dialog.
  assert.equal(h.run('reads + writes'), 0);
  assert.equal(h.app.pendingOperation, null);
  h.run('answer(false)');
  await pending;
  assert.equal(h.app.lockConfirmationPending, false);
  assert.equal(h.run('reads + writes'), 0);
  assert.equal(h.app.pendingOperation, null);
});

test('Bluetooth lock approval cannot transfer to another boot, connection, timer or session', async () => {
  for (const change of ['app.bootId++', 'gattQueue.reset()',
    'app.protocolInfo.device_id = "another-timer"', 'app.sessionSequence++']) {
    const h = appHarness();
    h.run(`var reads = 0, writes = 0, answer;
      confirmManagement = () => new Promise(resolve => { answer = resolve; });
      readGatt = async () => { reads++; return liveFrame(); };
      writeGatt = async () => { writes++; };`);
    const pending = h.requestControlLock();
    h.run(change + '; answer(true)');
    await pending;
    assert.equal(h.run('reads + writes'), 0, change);
    assert.equal(h.app.pendingOperation, null, change);
    assert.equal(h.app.lockConfirmationPending, false, change);
    assert.equal(h.feedback.at(-1)[1], 'Command not sent', change);
  }
});

test('Bluetooth lock reads current state after approval and refuses expiry, pause, emergency or a replaced session', async () => {
  for (const snapshot of ["{ ...app.live, state: 'open', remaining_ms: 0 }",
    "{ ...app.live, state: 'paused' }", "{ ...app.live, controls_locked: true }",
    "{ ...app.live, session_sequence: 8 }", "{ ...app.live, emergency_input_active: true }"]) {
    const h = appHarness();
    h.run(`var reads = 0, writes = 0;
      confirmManagement = async () => true;
      readGatt = async () => { reads++; return liveFrame(${snapshot}); };
      writeGatt = async () => { writes++; };`);
    // liveFrame encodes the actual protocol emergency input flag too.
    h.run('liveFrame = ((original) => (snapshot) => { const bytes = original(snapshot); if(snapshot.emergency_input_active) bytes[2] |= 2; return bytes; })(liveFrame)');
    await h.requestControlLock();
    assert.equal(h.run('reads'), 1, snapshot);
    assert.equal(h.run('writes'), 0, snapshot);
    assert.equal(h.app.pendingOperation, null, snapshot);
    assert.equal(h.feedback.at(-1)[1], 'Command not sent', snapshot);
  }
});

test('Bluetooth lock after a long confirmation refreshes proof and applies once to the original running session', async () => {
  const h = appHarness();
  h.context.advanceClock = h.advance;
  h.run(`var reads = 0, writes = 0;
    confirmManagement = async () => { advanceClock(45000); return true; };
    readGatt = async () => { reads++; return liveFrame(); };
    writeGatt = async () => {
      writes++;
      app.live.controls_locked = true;
      app.pendingResult.resolve({ opcode: 0x15, request_id: 31, boot_id: 42, session_sequence: 7,
        result: 'applied', state: 'running', remaining_ms: 9000, changed: true, duplicate: false });
    };`);
  await h.requestControlLock();
  assert.equal(h.run('reads'), 2);
  assert.equal(h.run('writes'), 1);
  assert.equal(h.app.pendingOperation, null);
  assert.equal(h.app.lockConfirmationPending, false);
  assert.equal(h.app.live.controls_locked, true);
});

test('keys are kept per timer; an unscoped key moves to the timer it proves and forgetting one keeps the other', async () => {
  const h = appHarness();
  h.context.document.createElement = () => ({ append() {}, addEventListener() {} });
  h.run(`var keys = new Map();
    credentialRead = async (name, scope = app.credentialScope) => keys.get(credentialName(name, scope)) ?? null;
    credentialWrite = async (name, key, scope = app.credentialScope) => { keys.set(credentialName(name, scope), compactHex(key)); };
    credentialDelete = async (name, scope = app.credentialScope) => { keys.delete(credentialName(name, scope)); };
    updateCredentialUi = () => {};
    keys.set('active', '00112233445566778899aabbccddeeff');
    app.device = { id: 'browser-a', name: 'ST-A' };`);
  await h.run(`loadCredentials(timerScope('AAAA01'))`);
  assert.equal(h.run('app.credentialLegacy'), true);
  assert.equal(h.run('compactHex(app.activeKey)'), '00112233445566778899aabbccddeeff');
  h.run(`app.protocolInfo = { device_id: 'aaaa01', device_name: 'Kitchen' };`);
  await h.run('rememberAuthenticatedTimer()');
  assert.deepEqual(JSON.parse(h.run('JSON.stringify([...keys.keys()].sort())')), ['active:aaaa01']);
  assert.equal(h.run('savedTimers()[0].name'), 'Kitchen');

  h.run(`keys.set('active:bbbb02', 'ffeeddccbbaa99887766554433221100');
    app.device = { id: 'browser-b' }; app.protocolInfo = { device_id: 'bbbb02', device_name: 'Hall' };`);
  await h.run(`loadCredentials('bbbb02')`);
  assert.equal(h.run('app.credentialLegacy'), false);
  await h.run('rememberAuthenticatedTimer()');
  assert.deepEqual(JSON.parse(h.run('JSON.stringify(savedTimers().map(timer => timer.id))')), ['bbbb02', 'aaaa01']);

  h.run('app.server = null;');
  await h.run(`forgetSavedTimer('aaaa01')`);
  assert.deepEqual(JSON.parse(h.run('JSON.stringify([...keys.keys()])')), ['active:bbbb02']);
  assert.deepEqual(JSON.parse(h.run('JSON.stringify(savedTimers().map(timer => timer.id))')), ['bbbb02']);
  assert.equal(h.run('compactHex(app.activeKey)'), 'ffeeddccbbaa99887766554433221100');
});
