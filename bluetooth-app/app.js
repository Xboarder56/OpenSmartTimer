// Smart Timer Web Bluetooth client for OpenSmartTimer's ost-ble/1 protocol.
// The controller credential is transactionally stored in IndexedDB. Pairing
// is not promoted until the timer confirms durable storage and accepts an
// authenticated session using the pending key.
import { GattQueue, UnknownCommandOutcome, resultMatches, withTimeout,
  exchangeCommand, snapshotFromResult, validPendingOperation } from './ble-transport.mjs';
import { MGMT_OPCODES, MGMT_FIRMWARE_CHUNK, carriesSecret, managementFrame, managementFragments,
  parseManagementReady, parseManagementSegment } from './ble-management.mjs';

const SERVICE_UUID = '70ba26f4-2f3d-44df-9b73-bfb3d3625961';
const CHAR = {
  protocolInfo: '8d826f5b-7282-499f-bbde-fe361c19a8ed',
  capabilities: '6fb27438-e619-4291-ae73-448a1e973c5a',
  systemInfo: 'f00df299-464f-4921-9285-7749593b1144',
  liveStatus: 'd1a93d5e-d4fe-434e-acc6-42d86d79ab63',
  eventDetail: '917421bb-67a0-4b34-92a5-5219427a670c',
  controlChallenge: 'eab30710-b1bb-42eb-ba07-115d18d91f64',
  controlCommand: '1fa0980f-7224-4d51-a315-c84fde9cbdc5',
  controlResult: '2df3b9a1-f2ad-4660-97f5-14325d9eb6db',
  pairingSecret: '546d8ce1-3570-4f2b-a1d8-c5632cb4f091',
};
const PAIRING_NICKNAME_UUID = '63af724d-0379-4191-b785-4e6baa457612';
// Management extension (docs/BLUETOOTH_MANAGEMENT.md).
const MANAGEMENT_REQUEST_UUID = '4b546245-44a0-4cd0-9eed-8be6c51195ba';
const MANAGEMENT_RESPONSE_UUID = '93b52e61-25f4-46a6-a597-5724013f60f9';
const MANAGEMENT_TIMEOUT_MS = 15000;

const PROTOCOL_VERSION = 1;
const KEY_DB_NAME = 'opensmarttimer-bluetooth';
const KEY_DB_VERSION = 1;
const KEY_STORE_NAME = 'credentials';
const LEGACY_KEY_STORAGE = 'ost.ble.test.appKey';
const DEV_MODE_KEY = 'ost.ble.test.devMode';
const REQUEST_ID_KEY = 'ost.ble.test.requestId';
const DEVICE_ID_KEY = 'ost.ble.app.deviceId';
const PENDING_OPERATION_KEY = 'ost.ble.app.pendingOperation';
// Timers this browser has authenticated with, most recent first. Keys live in
// IndexedDB per timer; this list holds only IDs and display names.
const SAVED_TIMERS_KEY = 'ost.ble.app.timers';
const SAVED_TIMER_LIMIT = 8;
const THEME_STORAGE = 'smartTimerTheme';
// The timer sends a live-status keepalive every 15 seconds. GATT may still
// report connected after delivery stops, so allow only two missed intervals.
const LIVE_STATUS_MAX_AGE_MS = 30000;
const LIVE_STATUS_RETRY_MS = 5000;
// The timer accepts one connection. A hidden tab is usually suspended by the
// browser, so it cannot watch anything; let go so another approved phone can
// connect, and reconnect to the remembered timer when shown again.
const BACKGROUND_RELEASE_MS = 60000;
const BACKGROUND_RELEASE_RETRY_MS = 5000;

const OP_CODES = {
  authenticate: 0x01,
  pairingCommit: 0x02,
  start: 0x10,
  release: 0x11,
  pause: 0x12,
  resume: 0x13,
  adjust: 0x14,
  lock: 0x15,
  identify: 0x16,
};

const RESULT_CODES = [
  'applied', 'invalid_frame', 'unauthorized', 'stale_boot', 'stale_session',
  'invalid_state', 'invalid_duration', 'emergency_input_active', 'controls_locked',
  'maximum_duration_reached', 'unsupported_opcode', 'operation_conflict',
  'system_transition', 'control_disabled',
];

const STATE_NAMES = ['open', 'running', 'paused'];
const RELEASE_REASONS = ['boot', 'expired', 'user', 'emergency', 'adjusted_to_zero', 'share_link_expired'];
const EVENT_NAMES = [
  'boot', 'started', 'remaining_set', 'paused', 'resumed', 'adjusted',
  'controls_locked', 'expired', 'opened_by_user', 'emergency_release',
  'emergency_input_activated', 'emergency_input_cleared', 'adjusted_to_zero',
  'wifi_connecting', 'wifi_connected', 'wifi_failed', 'standalone_mode',
  'wifi_disconnected', 'dhcp_timeout', 'ota_started', 'ota_succeeded',
  'ota_failed', 'restart_requested', 'device_renamed', 'identify_started',
  'access_point_configured', 'bluetooth_pairing_started',
  'bluetooth_pairing_completed', 'bluetooth_pairing_revoked', 'share_link_expired',
];

const app = {
  device: null,
  server: null,
  service: null,
  chars: {},
  activeKey: null,
  pendingKey: null,
  bootId: 0,
  sessionKey: null,
  sessionSequence: 0,
  commandCounter: 0,
  authenticated: false,
  connecting: false,
  connectingRemembered: false,
  pairing: false,
  commandBusy: false,
  lockConfirmationPending: false,
  systemRefreshBusy: false,
  subscribed: false,
  pendingResult: null,
  pendingOperation: null,
  reconciliationBusy: false,
  live: null,
  system: null,
  systemReceivedAt: 0,
  events: [],
  receivedAt: 0,
  // Set when an authenticated link drops without the user asking; cleared on reconnect.
  lostAt: 0,
  credentialScope: null,
  credentialLegacy: false,
  liveConfirmedAt: null,
  liveNeedsRead: true,
  liveGeneration: 0,
  liveReadTask: null,
  liveRefreshTask: null,
  liveRetryAt: 0,
  renderedFreshness: null,
  sessionDurationMs: 0,
  protocolInfo: null,
  settingsReturnFocus: null,
  guideReturnFocus: null,
  feedbackTimer: 0,
  toastTimer: 0,
  backgroundReleaseTimer: 0,
  releasedInBackground: false,
  managementRequestId: 1 + Math.floor(Math.random() * 0xfffe),
  managementWaiter: null,
  managementEarlyReady: null,
  managementBusy: false,
};

let commandQueue = Promise.resolve();
let updateNotice = null;
const gattQueue = new GattQueue({ onTimeout: () => disconnectTimer({ lost: true }) });

const $ = (id) => document.getElementById(id);
const t = (key, replacements = {}) => {
  if (window.SmartTimerI18n?.text) return window.SmartTimerI18n.text(key, replacements);
  let value = key;
  for (const [name, replacement] of Object.entries(replacements)) {
    value = value.replace(`{${name}}`, String(replacement));
  }
  return value;
};

const delay = (milliseconds) => new Promise((resolve) => setTimeout(resolve, milliseconds));
const setText = (id, value) => { const element = $(id); if (element) element.textContent = value; };
const bytesOf = (value) => new Uint8Array(value.buffer, value.byteOffset, value.byteLength).slice();
const hex = (bytes) => Array.from(bytes, (byte) => byte.toString(16).padStart(2, '0')).join(' ');
const compactHex = (bytes) => Array.from(bytes, (byte) => byte.toString(16).padStart(2, '0')).join('');

function parseHex(value) {
  const clean = value.trim().replace(/\s+/g, '');
  if (!/^[0-9a-fA-F]*$/.test(clean) || clean.length % 2 !== 0) return null;
  const result = new Uint8Array(clean.length / 2);
  for (let index = 0; index < result.length; index += 1) {
    result[index] = Number.parseInt(clean.slice(index * 2, index * 2 + 2), 16);
  }
  return result;
}

function u32le(bytes, offset) {
  return new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength).getUint32(offset, true);
}

function i32le(bytes, offset) {
  return new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength).getInt32(offset, true);
}

function u32ToLE(value) {
  const result = new Uint8Array(4);
  new DataView(result.buffer).setUint32(0, value >>> 0, true);
  return result;
}

function concat(...arrays) {
  const result = new Uint8Array(arrays.reduce((size, array) => size + array.length, 0));
  let offset = 0;
  for (const array of arrays) {
    result.set(array, offset);
    offset += array.length;
  }
  return result;
}

async function hmacSha256(keyBytes, messageBytes) {
  const key = await crypto.subtle.importKey(
    'raw', keyBytes, { name: 'HMAC', hash: 'SHA-256' }, false, ['sign'],
  );
  return new Uint8Array(await crypto.subtle.sign('HMAC', key, messageBytes));
}

function log(tag, message, bytes) {
  const item = document.createElement('li');
  const time = document.createElement('span');
  const label = document.createElement('span');
  const body = document.createElement('span');
  const now = new Date();
  const safeTag = ['tx', 'rx', 'err', 'info'].includes(tag) ? tag : 'info';

  time.className = 'time';
  time.textContent = `${now.toTimeString().split(' ')[0]}.${String(now.getMilliseconds()).padStart(3, '0')}`;
  label.className = `tag ${safeTag}`;
  label.textContent = safeTag;
  body.className = 'log-message';
  body.textContent = String(message);
  item.append(time, document.createTextNode(' '), label, body);

  if (bytes?.length) {
    const frame = document.createElement('span');
    frame.className = 'hex';
    frame.textContent = hex(bytes);
    item.append(document.createTextNode(' '), frame);
  }

  const list = $('logList');
  list.prepend(item);
  while (list.children.length > 200) list.lastElementChild.remove();
}

function openCredentialDatabase() {
  return new Promise((resolve, reject) => {
    const request = indexedDB.open(KEY_DB_NAME, KEY_DB_VERSION);
    request.onupgradeneeded = () => {
      if (!request.result.objectStoreNames.contains(KEY_STORE_NAME)) {
        request.result.createObjectStore(KEY_STORE_NAME);
      }
    };
    request.onsuccess = () => resolve(request.result);
    request.onerror = () => reject(request.error || new Error('Unable to open credential storage'));
  });
}

// Keys are stored per timer device_id. A key saved before multi-timer support
// has no scope; it is tried on the next timer and moved once it authenticates.
function credentialName(name, scope) {
  return scope ? `${name}:${scope}` : name;
}

async function credentialRead(name, scope = app.credentialScope) {
  const database = await openCredentialDatabase();
  try {
    return await new Promise((resolve, reject) => {
      const request = database.transaction(KEY_STORE_NAME, 'readonly').objectStore(KEY_STORE_NAME).get(credentialName(name, scope));
      request.onsuccess = () => resolve(request.result ?? null);
      request.onerror = () => reject(request.error || new Error('Unable to read credential'));
    });
  } finally {
    database.close();
  }
}

async function credentialWrite(name, key, scope = app.credentialScope) {
  const database = await openCredentialDatabase();
  try {
    await new Promise((resolve, reject) => {
      const transaction = database.transaction(KEY_STORE_NAME, 'readwrite');
      transaction.objectStore(KEY_STORE_NAME).put(compactHex(key), credentialName(name, scope));
      transaction.oncomplete = resolve;
      transaction.onabort = () => reject(transaction.error || new Error('Credential storage was aborted'));
      transaction.onerror = () => reject(transaction.error || new Error('Unable to store credential'));
    });
  } finally {
    database.close();
  }
}

async function credentialDelete(name, scope = app.credentialScope) {
  const database = await openCredentialDatabase();
  try {
    await new Promise((resolve, reject) => {
      const transaction = database.transaction(KEY_STORE_NAME, 'readwrite');
      transaction.objectStore(KEY_STORE_NAME).delete(credentialName(name, scope));
      transaction.oncomplete = resolve;
      transaction.onabort = () => reject(transaction.error || new Error('Credential deletion was aborted'));
      transaction.onerror = () => reject(transaction.error || new Error('Unable to delete credential'));
    });
  } finally {
    database.close();
  }
}

function validatedStoredKey(value) {
  if (typeof value !== 'string') return null;
  const bytes = parseHex(value);
  return bytes?.length === 16 ? bytes : null;
}

async function loadCredentials(scope = savedTimers()[0]?.id || null) {
  app.credentialScope = scope;
  let active = scope ? validatedStoredKey(await credentialRead('active')) : null;
  let pending = scope ? validatedStoredKey(await credentialRead('pending')) : null;
  app.credentialLegacy = false;
  if (!active && !pending) {
    active = validatedStoredKey(await credentialRead('active', null));
    pending = validatedStoredKey(await credentialRead('pending', null));
    const legacy = validatedStoredKey(localStorage.getItem(LEGACY_KEY_STORAGE) || '');
    if (!active && legacy) {
      await credentialWrite('active', legacy, null);
      active = legacy;
    }
    localStorage.removeItem(LEGACY_KEY_STORAGE);
    app.credentialLegacy = Boolean(active || pending);
  }
  app.activeKey = active;
  app.pendingKey = pending;
  updateCredentialUi();
}

function timerScope(deviceId) {
  return typeof deviceId === 'string' && /^[0-9a-f]{1,32}$/i.test(deviceId) ? deviceId.toLowerCase() : null;
}

function savedTimers() {
  try {
    const list = JSON.parse(localStorage.getItem(SAVED_TIMERS_KEY) || '[]');
    return Array.isArray(list) ? list.filter((timer) => timerScope(timer?.id)).slice(0, SAVED_TIMER_LIMIT) : [];
  } catch {
    return [];
  }
}

function storeSavedTimers(list) {
  try { localStorage.setItem(SAVED_TIMERS_KEY, JSON.stringify(list.slice(0, SAVED_TIMER_LIMIT))); }
  catch (error) { log('err', `Saved timer list was not updated: ${error.message}`); }
  renderSavedTimers();
}

// Called after this browser proves its key: the timer joins the saved list,
// and a key stored before multi-timer support moves under this timer's ID.
async function rememberAuthenticatedTimer() {
  const id = app.credentialScope;
  if (!id) return;
  if (app.credentialLegacy && app.activeKey) {
    try {
      await credentialWrite('active', app.activeKey);
      await credentialDelete('active', null);
      await credentialDelete('pending', null);
      app.credentialLegacy = false;
    } catch (error) {
      log('err', `Saved pairing will be moved to this timer next time: ${error.message}`);
    }
  }
  const entry = {
    id,
    bluetoothId: app.device?.id || null,
    name: String(app.protocolInfo?.device_name || app.device?.name || 'Smart Timer').slice(0, 48),
    lastUsedAt: Date.now(),
  };
  storeSavedTimers([entry, ...savedTimers().filter((timer) => timer.id !== id)]);
}

async function forgetSavedTimer(id) {
  const current = app.credentialScope === id;
  try {
    await credentialDelete('active', id);
    await credentialDelete('pending', id);
    if (current && app.credentialLegacy) {
      await credentialDelete('active', null);
      await credentialDelete('pending', null);
    }
  } catch (error) {
    showFeedback('error', 'Timer was not forgotten', error.message, 0);
    return false;
  }
  const remaining = savedTimers().filter((timer) => timer.id !== id);
  storeSavedTimers(remaining);
  if (current) {
    if (app.server?.connected) disconnectTimer();
    localStorage.removeItem(DEVICE_ID_KEY);
    await loadCredentials(null);
    app.credentialScope = null;
  }
  updateConnectionCard();
  return true;
}

// The same list appears on the main screen (to switch timers) and in
// settings (to forget one). Names come from the timer, so only textContent is used.
function renderSavedTimers() {
  const timers = savedTimers();
  const connected = Boolean(app.server?.connected);
  const busy = app.connecting || app.pairing;
  for (const [containerId, listId, withForget] of [['savedTimersCard', 'savedTimerList', false], ['savedTimersSettings', 'savedTimerSettingsList', true]]) {
    const list = $(listId);
    if (!list) continue;
    $(containerId).classList.toggle('hidden', withForget ? timers.length === 0 : connected || timers.length < 2);
    list.replaceChildren(...timers.map((timer) => {
      const row = document.createElement('li');
      row.className = 'saved-timer';
      const label = document.createElement('div');
      const name = document.createElement('strong');
      name.textContent = timer.name;
      const detail = document.createElement('small');
      const isCurrent = connected && app.credentialScope === timer.id;
      detail.textContent = `${isCurrent ? `${t('Connected')} · ` : ''}ID …${timer.id.slice(-6).toUpperCase()}`;
      label.append(name, detail);
      row.append(label);
      if (!isCurrent) {
        const connect = document.createElement('button');
        connect.type = 'button';
        connect.className = 'secondary';
        connect.textContent = t(connected ? 'Switch' : 'Connect');
        connect.disabled = busy;
        connect.addEventListener('click', () => void switchToSavedTimer(timer.id));
        row.append(connect);
      }
      if (withForget) {
        const forget = document.createElement('button');
        forget.type = 'button';
        forget.className = 'quiet-action';
        forget.textContent = t('Forget');
        forget.disabled = busy;
        forget.addEventListener('click', () => void forgetSavedTimer(timer.id));
        row.append(forget);
      }
      return row;
    }));
  }
}

async function switchToSavedTimer(id) {
  const target = savedTimers().find((timer) => timer.id === id);
  if (!target || app.connecting || app.pairing) return;
  if (app.server?.connected) disconnectTimer();
  await loadCredentials(target.id);
  await connectTimer(target);
}

async function promotePendingCredential() {
  if (!app.pendingKey) throw new Error('No pending controller key is available');
  await credentialWrite('active', app.pendingKey);
  app.activeKey = app.pendingKey;
  try {
    await credentialDelete('pending');
    app.pendingKey = null;
  } catch (error) {
    // Keeping the same proven key in both slots is recoverable on the next
    // launch. Do not turn a completed device-side pairing into a false
    // failure merely because browser-side cleanup was interrupted.
    log('err', `Pending credential cleanup will be retried: ${error.message}`);
  }
  updateCredentialUi();
}

function updateCredentialUi() {
  const hasActive = Boolean(app.activeKey);
  const hasPending = Boolean(app.pendingKey);
  setText('controllerStatus', hasActive ? t('Pairing saved') : hasPending ? t('Pairing recovery available') : t('Not stored'));
  setText('keyStatus', hasActive
    ? t('Pairing saved in this browser.')
    : hasPending
      ? t('A pending pairing is preserved for recovery. Reconnect to prove it.')
      : t('No pairing saved in this browser.'));
  $('controllerReadyNote').classList.toggle('hidden', !hasActive);
  $('forgetKeyButton').classList.toggle('hidden', !hasActive && !hasPending);
  $('appKeyInput').value = hasActive ? compactHex(app.activeKey) : '';
  updateConnectionCard();
}

function allocateRequestId() {
  let previous = Number.parseInt(localStorage.getItem(REQUEST_ID_KEY) || '', 10);
  if (!Number.isInteger(previous) || previous < 1 || previous > 65535) {
    const random = new Uint16Array(1);
    crypto.getRandomValues(random);
    previous = random[0] || 1;
  }
  const next = previous >= 65535 ? 1 : previous + 1;
  localStorage.setItem(REQUEST_ID_KEY, String(next));
  return next;
}

function applyTheme(theme) {
  const selected = theme === 'light' ? 'light' : 'dark';
  document.documentElement.dataset.theme = selected;
  $('themeButton').dataset.theme = selected;
  $('themeButton').setAttribute('aria-checked', String(selected === 'dark'));
  $('themeButton').setAttribute('aria-label', t(selected === 'dark' ? 'Switch to light mode' : 'Switch to dark mode'));
  document.querySelector('meta[name="theme-color"]').setAttribute('content', selected === 'light' ? '#f7f3fa' : '#0b0710');
}

function loadTheme() {
  let selected = 'dark';
  try {
    const saved = sessionStorage.getItem(THEME_STORAGE);
    if (saved === 'light' || saved === 'dark') selected = saved;
    else if (window.matchMedia?.('(prefers-color-scheme: light)').matches) selected = 'light';
  } catch {}
  applyTheme(selected);
}

function toggleTheme() {
  const selected = document.documentElement.dataset.theme === 'light' ? 'dark' : 'light';
  try { sessionStorage.setItem(THEME_STORAGE, selected); } catch {}
  applyTheme(selected);
}

function setDeveloperMode(enabled) {
  document.body.classList.toggle('dev-mode', enabled);
  $('developerModeToggle').checked = enabled;
  document.querySelectorAll('.dev-only').forEach((element) => {
    element.setAttribute('aria-hidden', String(!enabled));
  });
}

let lastConnection = ['offline', 'Disconnected'];
function setConnection(kind, label) {
  lastConnection = [kind, label];
  $('connectionShortcut').className = `connection ${kind}`;
  const connectionLabel = `${t('Bluetooth')} · ${t(label)}`;
  setText('connectionText', connectionLabel);
  setText('footerConnection', connectionLabel);
  const connected = Boolean(app.server?.connected);
  $('settingsConnectionBadge').className = `local-badge ${connected ? '' : 'inactive'}`.trim();
  setText('settingsConnectionBadge', t(connected ? (app.authenticated ? (liveStatusFresh() ? 'READY' : 'STATUS UNAVAILABLE') : 'CONNECTED') : 'OFFLINE'));
  $('settingsDisconnectButton').disabled = !connected;
  $('settingsConnectButton').disabled = connected || app.connecting;
}

function updateConnectionCard() {
  renderManagementAvailability();
  renderSavedTimers();
  const connected = Boolean(app.server?.connected);
  const hasCredential = Boolean(app.activeKey || app.pendingKey);
  const showPairing = connected && !app.authenticated && !app.connecting;
  $('scanButton').classList.toggle('hidden', connected);
  $('disconnectButton').classList.toggle('hidden', !connected);
  $('pairButton').classList.toggle('hidden', !showPairing);
  $('pairingHint').classList.toggle('hidden', !showPairing);
  $('scanButton').disabled = app.connecting || app.pairing;
  $('pairButton').disabled = app.pairing;
  $('disconnectButton').disabled = app.pairing;

  if (app.connecting) {
    setText('connectEyebrow', t('Bluetooth'));
    setText('connectHeading', t('Connecting…'));
    setText('connectHelper', t('Keep this page open while the secure Bluetooth connection is prepared.'));
  } else if (app.authenticated) {
    const fresh = liveStatusFresh();
    setText('connectEyebrow', t(fresh ? 'Connected' : 'Status unavailable'));
    setText('connectHeading', t(fresh ? 'Bluetooth control is ready' : 'Refreshing timer status…'));
    setText('connectHelper', t(fresh
      ? 'This browser proved its saved controller key. Timer controls and live updates are available.'
      : 'The Bluetooth link is connected, but current timer status is unconfirmed. Controls return after a fresh status update.'));
  } else if (connected) {
    setText('connectEyebrow', t(app.pendingKey ? 'Recover pairing' : 'Approve this app'));
    setText('connectHeading', t(app.pendingKey ? 'Finish the saved pairing' : 'Physical approval required'));
    setText('connectHelper', app.pendingKey
      ? t('A pending key is safely preserved. If automatic recovery did not complete, reopen pairing and approve this app again.')
      : t('Briefly press and release the timer Reset button, then select Finish pairing.'));
  } else if (hasCredential) {
    setText('connectEyebrow', t('Reconnect'));
    setText('connectHeading', t('Connect to your approved timer'));
    setText('connectHelper', t('Select Reconnect and choose the same timer. This browser will authenticate automatically.'));
    setText('scanButton', t('Reconnect'));
  } else {
    setText('connectEyebrow', t('Pair a timer'));
    setText('connectHeading', t('Open Bluetooth pairing first'));
    setText('connectHelper', t('Hold the timer Reset button for 3–5 seconds until the blue LED pulses, then select Connect.'));
    setText('scanButton', t('Connect'));
  }
}

function showFeedback(kind, title, message, timeout = 6500) {
  clearTimeout(app.feedbackTimer);
  const panel = $('commandFeedback');
  panel.className = `command-feedback ${kind}`;
  setText('commandFeedbackTitle', t(title));
  setText('commandFeedbackMessage', t(message));
  if (timeout > 0) {
    app.feedbackTimer = setTimeout(() => panel.classList.add('hidden'), timeout);
  }
}

function showToast(title, message) {
  clearTimeout(app.toastTimer);
  setText('statusToastTitle', t(title));
  setText('statusToastMessage', t(message));
  $('statusToast').classList.add('show');
  app.toastTimer = setTimeout(() => $('statusToast').classList.remove('show'), 4300);
}

function readProtocolFrame(frame, expectedLength, label) {
  if (frame.length !== expectedLength || frame[0] !== PROTOCOL_VERSION) {
    throw new Error(`Invalid ${label} frame (${frame.length} bytes)`);
  }
}

function decodeLive(frame) {
  readProtocolFrame(frame, 20, 'live status');
  if (!STATE_NAMES[frame[1]] || u32le(frame, 4) > 86400000 || u32le(frame, 8) > 86400000)
    throw new Error('Invalid live timer state');
  return {
    state: STATE_NAMES[frame[1]] || `unknown_${frame[1]}`,
    controls_locked: Boolean(frame[2] & 0x01),
    emergency_input_active: Boolean(frame[2] & 0x02),
    lock_output_energized: Boolean(frame[2] & 0x04),
    // Set bits mean the owner switched the control off; older firmware sends 0.
    pause_allowed: !(frame[2] & 0x08),
    control_lock_allowed: !(frame[2] & 0x10),
    last_release_reason: RELEASE_REASONS[frame[3]] || `unknown_${frame[3]}`,
    remaining_ms: u32le(frame, 4),
    session_duration_ms: u32le(frame, 8),
    event_revision: u32le(frame, 12),
    session_sequence: u32le(frame, 16),
  };
}

function decodeSystemInfo(frame) {
  readProtocolFrame(frame, 44, 'system information');
  const flags = frame[3];
  const stationConnected = Boolean(flags & 0x01);
  return {
    reset_reason_code: frame[1],
    wifi_state_code: frame[2],
    station_connected: stationConnected,
    ap_running: Boolean(flags & 0x02),
    standalone_mode: Boolean(flags & 0x04),
    station_retry_paused: Boolean(flags & 0x08),
    bluetooth_connected: Boolean(flags & 0x10),
    bluetooth_authenticated: Boolean(flags & 0x20),
    uptime_ms: u32le(frame, 4),
    free_heap_bytes: u32le(frame, 8),
    largest_free_block_bytes: u32le(frame, 12),
    minimum_free_heap_bytes: u32le(frame, 16),
    sketch_size_bytes: u32le(frame, 20),
    free_sketch_space_bytes: u32le(frame, 24),
    flash_size_bytes: u32le(frame, 28),
    station_ip: stationConnected ? Array.from(frame.slice(32, 36)).join('.') : '',
    wifi_rssi_dbm: new DataView(frame.buffer, frame.byteOffset, frame.byteLength).getInt8(36),
    ap_clients: frame[37],
    ap_client_limit: frame[38],
    heap_fragmentation_percent: frame[39],
    bluetooth_mtu: frame[40] | (frame[41] << 8),
    bluetooth_rssi_dbm: new DataView(frame.buffer, frame.byteOffset, frame.byteLength).getInt8(42),
  };
}

function decodeEvent(frame) {
  readProtocolFrame(frame, 20, 'event');
  return {
    event_type: EVENT_NAMES[frame[1]] || `unknown_${frame[1]}`,
    event_code: frame[1],
    state: STATE_NAMES[frame[2]] || `unknown_${frame[2]}`,
    controls_locked: Boolean(frame[3] & 0x01),
    emergency_input_active: Boolean(frame[3] & 0x02),
    history_truncated: Boolean(frame[3] & 0x04),
    event_id: u32le(frame, 4),
    at_ms: u32le(frame, 8),
    value_ms: i32le(frame, 12),
    session_sequence: u32le(frame, 16),
  };
}

function decodeResult(frame) {
  readProtocolFrame(frame, 20, 'control result');
  return {
    opcode: frame[1],
    request_id: frame[2] | (frame[3] << 8),
    result: RESULT_CODES[frame[4]] || `unknown_${frame[4]}`,
    changed: Boolean(frame[5] & 0x01),
    duplicate: Boolean(frame[5] & 0x02),
    state: STATE_NAMES[frame[6]] || `unknown_${frame[6]}`,
    release_reason: RELEASE_REASONS[frame[7]] || `unknown_${frame[7]}`,
    boot_id: u32le(frame, 8),
    session_sequence: u32le(frame, 12),
    remaining_ms: u32le(frame, 16),
  };
}

function onStatusFrame(frame) {
  try {
    log('rx', 'live status', frame);
    if (app.commandBusy || document.hidden) return; // A queued notification can predate the result.
    acceptLiveSnapshot(decodeLive(frame), false, !app.liveNeedsRead);
  } catch (error) {
    log('err', error.message);
  }
}

function onEventFrame(frame) {
  try {
    log('rx', 'event', frame);
    const event = decodeEvent(frame);
    const existing = app.events.findIndex((candidate) => candidate.event_id === event.event_id);
    if (existing >= 0) app.events.splice(existing, 1);
    app.events.push(event);
    app.events.sort((left, right) => left.event_id - right.event_id);
    if (app.events.length > 16) app.events.splice(0, app.events.length - 16);
    renderRecentActivity();
    log('info', `event ${event.event_id}: ${event.event_type}; state=${event.state}; session=${event.session_sequence}`);
    if (event.event_type === 'emergency_input_activated') {
      showToast('Emergency input activated', 'The timer is forced open while the input remains active.');
    } else if (event.event_type === 'emergency_release') {
      showToast('Emergency release activated', 'The timer unlocked immediately.');
    }
  } catch (error) {
    log('err', error.message);
  }
}

function onResultFrame(frame) {
  try {
    log('rx', 'control result', frame);
    const result = decodeResult(frame);
    log('info', `request ${result.request_id}: ${result.result}${result.duplicate ? ' (duplicate)' : ''}; resulting state=${result.state}`);
    if (app.pendingResult && resultMatches(app.pendingResult.operation, result)) {
      app.pendingResult.resolve(result);
    }
  } catch (error) {
    log('err', error.message);
    app.pendingResult?.reject(error);
  }
}

function liveStatusFresh() {
  return Boolean(app.server?.connected && app.authenticated && app.live &&
    !document.hidden && !app.liveNeedsRead && app.liveConfirmedAt !== null &&
    performance.now() - app.liveConfirmedAt < LIVE_STATUS_MAX_AGE_MS);
}

function acceptLiveSnapshot(snapshot, authoritative = false, confirmed = true) {
  if (!authoritative && app.live &&
      ((snapshot.event_revision - app.live.event_revision) >>> 0) >= 0x80000000) return false;
  // Result-derived snapshots are useful feedback, but do not prove that the
  // complete live-status stream (including interlocks) is still current.
  if (confirmed) app.liveConfirmedAt = performance.now();
  renderLive(snapshot);
  return true;
}

async function readLiveSnapshot(authoritative = false) {
  const epoch = gattQueue.epoch;
  const generation = app.liveGeneration;
  const previous = app.liveReadTask;
  if (previous) {
    if (previous.epoch === epoch && previous.generation === generation) return previous.promise;
    // Do not queue a second read behind a pending browser operation on resume.
    try { await previous.promise; } catch {}
    if (epoch !== gattQueue.epoch || generation !== app.liveGeneration)
      throw new Error('Timer status refresh was superseded');
    return readLiveSnapshot(authoritative);
  }
  const task = { epoch, generation, promise: null };
  app.liveReadTask = task;
  task.promise = (async () => {
    try {
      const snapshot = decodeLive(bytesOf(await readGatt(app.chars.liveStatus)));
      if (epoch !== gattQueue.epoch || generation !== app.liveGeneration || !app.authenticated)
        throw new Error('Timer connection changed while reading status');
      const needsRead = app.liveNeedsRead;
      app.liveNeedsRead = false;
      if (!acceptLiveSnapshot(snapshot, authoritative, true)) {
        app.liveNeedsRead = needsRead;
        throw new Error('An older timer status was ignored');
      }
      return snapshot;
    } finally {
      if (app.liveReadTask === task) app.liveReadTask = null;
    }
  })();
  return task.promise;
}

function invalidateLiveStatus() {
  app.liveGeneration += 1;
  app.liveNeedsRead = true;
  app.liveConfirmedAt = null;
  app.liveRetryAt = 0;
  renderStatusFreshness();
}

async function refreshLiveStatus() {
  if (document.hidden || !app.server?.connected || !app.authenticated ||
      !app.chars.liveStatus || app.commandBusy || app.pairing || app.reconciliationBusy ||
      app.liveRefreshTask || performance.now() < app.liveRetryAt) return;
  const epoch = gattQueue.epoch;
  const task = {};
  app.liveRefreshTask = task;
  app.liveRetryAt = performance.now() + LIVE_STATUS_RETRY_MS;
  try { await readLiveSnapshot(); }
  catch (error) {
    if (epoch === gattQueue.epoch) log('err', `Timer status refresh failed: ${error.message}`);
  } finally {
    if (app.liveRefreshTask === task) app.liveRefreshTask = null;
    if (epoch === gattQueue.epoch) renderStatusFreshness();
  }
}

function maintainLiveStatus() {
  renderStatusFreshness();
  renderConnectionWarning();
  if (!liveStatusFresh()) void refreshLiveStatus();
}

function handlePageVisibility() {
  // A browser can suspend JS/BLE delivery while hidden. A notification queued
  // before suspension is insufficient: require a read started after resume.
  invalidateLiveStatus();
  clearTimeout(app.backgroundReleaseTimer);
  app.backgroundReleaseTimer = 0;
  if (document.hidden) {
    if (app.server?.connected) {
      app.backgroundReleaseTimer = setTimeout(releaseInBackground, BACKGROUND_RELEASE_MS);
    }
    return;
  }
  if (app.releasedInBackground) {
    app.releasedInBackground = false;
    void reconnectAfterBackground();
    return;
  }
  void refreshLiveStatus();
}

function releaseInBackground() {
  app.backgroundReleaseTimer = 0;
  if (!document.hidden || !app.server?.connected) return;
  // Never cut a pairing ceremony, connection setup, or a command still
  // waiting for its result; check again shortly instead.
  if (app.pairing || app.connecting || app.commandBusy || app.pendingResult ||
      app.reconciliationBusy) {
    app.backgroundReleaseTimer = setTimeout(releaseInBackground, BACKGROUND_RELEASE_RETRY_MS);
    return;
  }
  app.releasedInBackground = true;
  log('info', 'Released the Bluetooth connection while hidden so another approved phone can connect.');
  disconnectTimer();
}

async function reconnectAfterBackground() {
  // requestDevice() needs a user gesture, so only a remembered, already
  // permitted timer reconnects automatically. An unresolved command journal
  // is reconciled by the normal connection path; nothing is replayed.
  const savedId = localStorage.getItem(DEVICE_ID_KEY);
  let remembered = false;
  try {
    if (savedId && (app.activeKey || app.pendingKey) && navigator.bluetooth?.getDevices) {
      const granted = await navigator.bluetooth.getDevices();
      remembered = granted.some((device) => device.id === savedId);
    }
  } catch {
    remembered = false;
  }
  if (!remembered) {
    showFeedback('info', 'Reconnect to continue',
      'The app let go of the timer while it was in the background. Select Connect to reconnect.', 0);
    return;
  }
  await connectTimer();
}

function estimatedRemaining() {
  if (!app.live) return 0;
  if (app.live.state !== 'running') return app.live.remaining_ms;
  return Math.max(0, app.live.remaining_ms - (performance.now() - app.receivedAt));
}

function formatTime(milliseconds) {
  const seconds = Math.max(0, Math.ceil(milliseconds / 1000));
  const hours = Math.floor(seconds / 3600);
  const minutes = Math.floor((seconds % 3600) / 60);
  const remainder = seconds % 60;
  return `${String(hours).padStart(2, '0')}:${String(minutes).padStart(2, '0')}:${String(remainder).padStart(2, '0')}`;
}

function formatStorage(bytes) {
  if (!Number.isFinite(bytes)) return '—';
  if (bytes >= 1024 * 1024) return `${(bytes / (1024 * 1024)).toFixed(2)} MiB`;
  return `${(bytes / 1024).toFixed(1)} KiB`;
}

function formatUptime(milliseconds) {
  if (!Number.isFinite(milliseconds)) return '—';
  const totalSeconds = Math.floor(milliseconds / 1000);
  const days = Math.floor(totalSeconds / 86400);
  const hours = Math.floor((totalSeconds % 86400) / 3600);
  const minutes = Math.floor((totalSeconds % 3600) / 60);
  if (days) return `${days}d ${hours}h ${minutes}m`;
  if (hours) return `${hours}h ${minutes}m`;
  return `${minutes}m ${totalSeconds % 60}s`;
}

function resetReasonName(code) {
  return ({
    0: 'Unknown reset',
    1: 'Power-on reset',
    2: 'External system reset',
    3: 'Software/System restart',
    4: 'Software panic',
    5: 'Interrupt watchdog',
    6: 'Task watchdog',
    7: 'Other watchdog',
    8: 'Deep-sleep wake',
    9: 'Brownout reset',
    10: 'SDIO reset',
  })[code] || `Unknown reset (${code})`;
}

function eventDisplayName(name) {
  const named = {
    boot: 'Device started',
    started: 'Timer started',
    remaining_set: 'Duration set',
    paused: 'Timer paused',
    resumed: 'Timer resumed',
    adjusted: 'Duration adjusted',
    controls_locked: 'Software controls locked',
    expired: 'Timer expired',
    opened_by_user: 'Unlocked by user',
    emergency_release: 'Emergency release used',
    emergency_input_activated: 'Emergency input activated',
    emergency_input_cleared: 'Emergency input cleared',
    adjusted_to_zero: 'Duration adjusted to zero',
    wifi_connecting: 'Connecting to local Wi-Fi',
    wifi_connected: 'Connected to local Wi-Fi',
    wifi_failed: 'Wi-Fi connection failed',
    standalone_mode: 'AP Mode started',
    wifi_disconnected: 'Local Wi-Fi disconnected',
    dhcp_timeout: 'Wi-Fi address timed out',
    ota_started: 'Firmware update started',
    ota_succeeded: 'Firmware update accepted',
    ota_failed: 'Firmware update failed',
    restart_requested: 'Restart requested',
    device_renamed: 'Device renamed',
    identify_started: 'Identify started',
    access_point_configured: 'AP settings changed',
    bluetooth_pairing_started: 'Bluetooth pairing started',
    bluetooth_pairing_completed: 'Bluetooth pairing completed',
    bluetooth_pairing_revoked: 'Bluetooth controller removed',
    share_link_expired: 'Share link expired; lock opened',
  };
  return t(named[name] || name.replaceAll('_', ' '));
}

function renderRecentActivity() {
  const list = $('activityList');
  if (!app.events.length) {
    const empty = document.createElement('li');
    empty.className = 'activity-empty';
    empty.textContent = t(app.authenticated
      ? 'No retained activity is available for this boot.'
      : 'Connect to load this boot\'s recent events.');
    list.replaceChildren(empty);
    setText('activityCount', '0');
    return;
  }
  const currentUptime = app.system?.uptime_ms;
  const items = [...app.events].reverse().map((event) => {
    const item = document.createElement('li');
    const copy = document.createElement('span');
    const title = document.createElement('strong');
    const detail = document.createElement('small');
    const sequence = event.session_sequence ? t(' · session {session}', { session: event.session_sequence }) : '';
    title.textContent = eventDisplayName(event.event_type);
    detail.textContent = t('State: {state}', { state: t(event.state) }) + sequence;
    copy.append(title, detail);
    const when = document.createElement('time');
    const age = Number.isFinite(currentUptime) ? Math.max(0, currentUptime - event.at_ms) : null;
    when.textContent = age === null ? `#${event.event_id}` : age < 60000
      ? t('{count}s ago', { count: Math.floor(age / 1000) })
      : t('{count}m ago', { count: Math.floor(age / 60000) });
    item.append(copy, when);
    return item;
  });
  list.replaceChildren(...items);
  setText('activityCount', String(app.events.length));
  const truncated = app.events.some((event) => event.history_truncated);
  setText('activityNotice', truncated
    ? t('Older activity was overwritten before this app connected; the newest retained events are shown.')
    : t('Showing {count} retained events from this boot.', { count: app.events.length }));
}

function renderSystemInfo(system) {
  app.system = system;
  app.systemReceivedAt = performance.now();
  const wifiNames = ['AP Mode', 'Connecting to local Wi-Fi', 'Connected to local Wi-Fi', 'Wi-Fi connection failed', 'Wi-Fi disconnected', 'Bluetooth'];
  let network = t(wifiNames[system.wifi_state_code] || 'Unknown ({code})', { code: system.wifi_state_code });
  if (system.ap_running && !system.standalone_mode && !system.station_connected) network += t(' · Recovery AP ready');
  setText('uptimeLabel', formatUptime(system.uptime_ms));
  setText('resetReasonLabel', t(resetReasonName(system.reset_reason_code)));
  setText('networkLabel', network);
  setText('ipAddressLabel', system.station_ip || (system.ap_running ? '192.168.4.1' : t('Not connected')));
  const host = app.protocolInfo?.hostname;
  setText('hostnameLabel', host ? `${host}.local` : '—');
  $('apNetworkRow').classList.toggle('hidden', !system.ap_running);
  $('apClientsRow').classList.toggle('hidden', !system.ap_running);
  setText('apNetworkLabel', app.protocolInfo?.ap_ssid || '—');
  setText('apClientsLabel', `${system.ap_clients}/${system.ap_client_limit}`);
  setText('freeHeapLabel', formatStorage(system.free_heap_bytes));
  setText('largestBlockLabel', formatStorage(system.largest_free_block_bytes));
  setText('minimumHeapLabel', formatStorage(system.minimum_free_heap_bytes));
  setText('heapFragmentationLabel', `${system.heap_fragmentation_percent}%`);
  setText('sketchSizeLabel', formatStorage(system.sketch_size_bytes));
  setText('otaSpaceLabel', formatStorage(system.free_sketch_space_bytes));
  setText('flashSizeLabel', formatStorage(system.flash_size_bytes));
  setText('mtuLabel', `${system.bluetooth_mtu} bytes`);
  setText('bleRssiLabel', system.bluetooth_rssi_dbm === 127 ? t('Unavailable') : `${system.bluetooth_rssi_dbm} dBm`);
  setText('statusPre', JSON.stringify({ identity: app.protocolInfo, timer: app.live, system }, null, 2));
  renderRecentActivity();
}

async function refreshSystemInfo() {
  if (!app.server?.connected || !app.authenticated || !app.chars.systemInfo ||
      app.systemRefreshBusy || app.commandBusy || app.pairing) return;
  app.systemRefreshBusy = true;
  const epoch = gattQueue.epoch;
  try {
    const frame = bytesOf(await readGatt(app.chars.systemInfo));
    if (epoch !== gattQueue.epoch) return;
    log('rx', 'system information', frame);
    renderSystemInfo(decodeSystemInfo(frame));
  } catch (error) {
    log('err', `System information refresh failed: ${error.message}`);
  } finally {
    if (epoch === gattQueue.epoch) app.systemRefreshBusy = false;
  }
  if (epoch === gattQueue.epoch) await reconcilePendingOperation();
}

function renderClock() {
  const fresh = liveStatusFresh();
  const remaining = estimatedRemaining();
  setText('clock', app.authenticated && !fresh ? '—' : formatTime(remaining));
  const base = Math.max(app.sessionDurationMs, remaining, 1);
  const progress = fresh && app.live.state !== 'open' ? Math.min(360, (remaining / base) * 360) : 0;
  $('timerDial').style.setProperty('--progress', `${progress}deg`);
  $('timerDial').classList.toggle('active', Boolean(fresh && app.live.state !== 'open'));
  if (app.authenticated && !fresh) {
    setText('completionTime', t('Waiting for current timer status'));
  } else if (app.live && app.live.state !== 'open') {
    setText('completionTime', app.live.state === 'paused'
      ? t('Completion time available after resume')
      : t('Opens at {time}', { time: new Date(Date.now() + remaining).toLocaleTimeString([], { hour: 'numeric', minute: '2-digit' }) }));
  }
  if (app.system) {
    setText('uptimeLabel', formatUptime(app.system.uptime_ms + (performance.now() - app.systemReceivedAt)));
  }
  requestAnimationFrame(renderClock);
}

function syncStateVisibility() {
  const state = app.live?.state || 'open';
  document.documentElement.dataset.state = app.live?.controls_locked ? 'sealed' : state;
  document.querySelectorAll('[data-open-only]').forEach((element) => {
    if (element.id !== 'fixedDuration' && element.id !== 'randomFields') {
      element.classList.toggle('hidden', state !== 'open');
    }
  });
  document.querySelectorAll('[data-session-only]').forEach((element) => {
    element.classList.toggle('hidden', !app.live || state === 'open');
  });
  document.querySelectorAll('[data-unsealed-only]').forEach((element) => {
    element.classList.toggle('hidden', !app.live || state === 'open' || app.live.controls_locked);
  });
  // Controls the owner switched off on the timer are not offered here either.
  // A session that is already paused keeps its Resume button.
  if (app.live && app.live.pause_allowed === false && state !== 'paused') $('pauseButton').classList.add('hidden');
  if (app.live && app.live.control_lock_allowed === false) $('sealButton').classList.add('hidden');
  $('sealedNotice').classList.toggle('hidden', !app.live?.controls_locked || state === 'open');
  syncDurationMode();
}

function renderLive(next) {
  app.live = next;
  app.receivedAt = performance.now();
  app.sessionSequence = next.session_sequence;
  app.sessionDurationMs = next.state === 'open' ? 0 : (next.session_duration_ms || next.remaining_ms);
  setText('sessionSeqLabel', String(next.session_sequence));
  setText('statusPre', JSON.stringify({ identity: app.protocolInfo, timer: next, system: app.system }, null, 2));
  updateNotice?.status({ state: next.state });

  renderTimerState();
  syncStateVisibility();
  renderStatusFreshness(true);
}

function renderTimerState() {
  const next = app.live;
  if (!next) return;
  const emergency = next.emergency_input_active;
  setText('stateBadge', t(emergency ? 'INTERLOCK OPEN' : next.controls_locked ? 'CONTROLS LOCKED' : next.state.toUpperCase()));
  $('stateBadge').className = `badge ${emergency ? 'emergency' : next.controls_locked ? 'sealed' : next.state}`;
  setText('stateTitle', emergency ? t('Emergency input is active') : '');
  setText('safetyNote', t(emergency
    ? 'The emergency input is active. The lock output is forced open and a timer cannot start.'
    : next.controls_locked
      ? 'Software controls are locked until expiry. The emergency input still commands release.'
      : next.state === 'open'
        ? 'The lock is unlocked.'
        : 'Local controls and the physical emergency release are available.'));
  setText('pauseButton', t(next.state === 'paused' ? 'Resume' : 'Pause'));
}

function renderStatusFreshness(force = false) {
  const fresh = liveStatusFresh();
  if (!force && app.renderedFreshness === fresh) return;
  app.renderedFreshness = fresh;
  if (app.authenticated) {
    if (fresh) renderTimerState();
    else {
      setText('stateBadge', t('STATUS UNAVAILABLE'));
      $('stateBadge').className = 'badge stale';
      setText('stateTitle', t('Refreshing timer status…'));
      setText('safetyNote', t('Current timer status is unconfirmed. Use the physical emergency release if needed.'));
    }
    setConnection(fresh ? 'online' : 'pairing', fresh ? 'Connected' : 'Status unavailable');
    updateConnectionCard();
  }
  updateControlAvailability();
}

function renderNotReady() {
  app.live = null;
  app.liveConfirmedAt = null;
  app.liveNeedsRead = true;
  app.renderedFreshness = null;
  app.sessionDurationMs = 0;
  setText('stateBadge', t(app.server?.connected ? 'CONNECTED' : 'OFFLINE'));
  $('stateBadge').className = `badge ${app.server?.connected ? 'stale' : 'offline'}`;
  setText('stateTitle', t(app.server?.connected ? 'Controller approval required' : 'Connect to your timer'));
  setText('safetyNote', t(app.server?.connected ? 'Finish secure pairing before timer controls become available.' : 'Connect to your timer to begin.'));
  setText('statusPre', t('No authenticated snapshot yet.'));
  syncStateVisibility();
  updateControlAvailability();
}

function updateControlAvailability() {
  renderManagementAvailability();
  const connected = Boolean(app.server?.connected && app.authenticated && app.live && !app.commandBusy && !app.lockConfirmationPending);
  const ready = connected && liveStatusFresh();
  const state = app.live?.state;
  const unresolved = Boolean(app.pendingOperation);
  const unlocked = ready && !app.live.controls_locked && !unresolved;
  const open = ready && !unresolved && state === 'open' && !app.live.emergency_input_active;
  ['hours', 'minutes', 'seconds', 'randomMin', 'randomMax', 'randomMode'].forEach((id) => {
    $(id).disabled = !open;
  });
  document.querySelectorAll('[data-preset], #resetDuration').forEach((button) => { button.disabled = !open; });
  $('startButton').disabled = !open;
  $('pauseButton').disabled = !unlocked || !['running', 'paused'].includes(state);
  // Uncertainty must never disable an otherwise available release.
  $('stopButton').disabled = !connected || app.live.controls_locked || (state === 'open' && !unresolved && liveStatusFresh());
  // A lost Start response may leave the last snapshot open while the timer
  // is actually running. Keep an explicit release reachable in that case.
  if (connected && (unresolved || !liveStatusFresh()) && !app.live.controls_locked) {
    $('activeControls').classList.remove('hidden');
    $('stopButton').classList.remove('hidden');
  }
  $('sealButton').disabled = !unlocked || state !== 'running';
  $('settingsIdentifyButton').disabled = !ready || unresolved || state !== 'open';
  document.querySelectorAll('[data-adjust]').forEach((button) => {
    button.disabled = !unlocked || !['running', 'paused'].includes(state);
  });
}

async function writeGatt(characteristic, frame, label) {
  if (!characteristic) throw new Error(`${label} characteristic is unavailable`);
  await gattQueue.run(() => characteristic.writeValueWithResponse(frame));
  log('tx', label, frame);
}

async function readGatt(characteristic) {
  if (!characteristic) throw new Error('Bluetooth characteristic is unavailable');
  return gattQueue.run(() => characteristic.readValue());
}

// ---- Device settings over Bluetooth -------------------------------------
// Same JSON as the HTTP API, run by the same firmware handler. Passwords go
// only in the AES-128-CTR secret; every reply is verified with the session key.
function managementSupported() {
  return Boolean(app.protocolInfo?.management === 1 && app.chars.managementRequest && app.chars.managementResponse);
}

function onManagementReady(bytes) {
  let ready;
  try { ready = parseManagementReady(bytes); } catch { return; }
  const waiter = app.managementWaiter;
  if (!waiter || waiter.requestId !== ready.requestId) {
    app.managementEarlyReady = ready;
    return;
  }
  clearTimeout(waiter.timer);
  app.managementWaiter = null;
  waiter.resolve(ready);
}

function waitForManagementReady(requestId, timeoutMs) {
  if (app.managementEarlyReady?.requestId === requestId) {
    const ready = app.managementEarlyReady;
    app.managementEarlyReady = null;
    return Promise.resolve(ready);
  }
  return new Promise((resolve, reject) => {
    const timer = setTimeout(() => {
      if (app.managementWaiter?.requestId !== requestId) return;
      app.managementWaiter = null;
      reject(new Error('The timer did not answer the settings request'));
    }, timeoutMs);
    app.managementWaiter = { requestId, resolve, reject, timer };
  });
}

function nextManagementRequestId() {
  app.managementRequestId = app.managementRequestId >= 0xffff ? 1 : app.managementRequestId + 1;
  return app.managementRequestId;
}

async function sendManagementFrame(frame, requestId, timeoutMs) {
  // Web Bluetooth hides the MTU; System Information reports the negotiated one.
  const maxWrite = Math.max(20, (app.system?.bluetooth_mtu || 23) - 3);
  const pieces = managementFragments(frame, maxWrite);
  for (let attempt = 0; ; attempt += 1) {
    app.managementEarlyReady = null;
    const ready = waitForManagementReady(requestId, timeoutMs);
    try {
      for (const piece of pieces.slice(0, -1)) {
        await gattQueue.run(() => app.chars.managementRequest.writeValueWithoutResponse(piece));
      }
      await gattQueue.run(() => app.chars.managementRequest.writeValueWithResponse(pieces[pieces.length - 1]));
    } catch (error) {
      if (app.managementWaiter?.requestId === requestId) {
        clearTimeout(app.managementWaiter.timer);
        app.managementWaiter = null;
      }
      ready.catch(() => {});
      // Busy or a lost fragment: resend the whole frame. Bounded, because an
      // integrity failure also surfaces here and five of them end the link.
      if (attempt < 2 && app.server?.connected) { await delay(150); continue; }
      throw error;
    }
    return ready;
  }
}

async function managementExchange(opcode, body = new Uint8Array(), { secret = new Uint8Array(), readOnSuccess = true, timeoutMs = MANAGEMENT_TIMEOUT_MS } = {}) {
  if (!managementSupported()) throw new Error('Update the timer firmware to change settings over Bluetooth');
  if (!app.authenticated || !app.sessionKey || !app.server?.connected) throw new Error('Connect and authenticate first');
  const sessionKey = app.sessionKey;
  const requestId = nextManagementRequestId();
  const counter = ++app.commandCounter;
  const frame = await managementFrame(crypto.subtle, sessionKey, { opcode, requestId, bootId: app.bootId, counter, body, secret });
  log('tx', `settings request 0x${opcode.toString(16)} #${requestId}`, frame.slice(0, 15));
  const ready = await sendManagementFrame(frame, requestId, timeoutMs);
  if (ready.status === 200 && !readOnSuccess) return { status: ready.status, body: new Uint8Array() };
  let segment = await parseManagementSegment(crypto.subtle, sessionKey, counter, bytesOf(await readGatt(app.chars.managementResponse)));
  if (segment.requestId !== requestId || segment.offset !== 0) throw new Error('The settings reply did not match the request');
  const parts = [segment.body];
  let received = segment.body.length;
  while (received < segment.totalLength) {
    const continueCounter = ++app.commandCounter;
    const request = new TextEncoder().encode(JSON.stringify({ request_id: requestId, offset: received }));
    const continueFrame = await managementFrame(crypto.subtle, sessionKey, {
      opcode: MGMT_OPCODES.continueResponse, requestId: nextManagementRequestId(), bootId: app.bootId, counter: continueCounter, body: request,
    });
    await sendManagementFrame(continueFrame, requestId, timeoutMs);
    segment = await parseManagementSegment(crypto.subtle, sessionKey, continueCounter, bytesOf(await readGatt(app.chars.managementResponse)));
    if (segment.requestId !== requestId || segment.offset !== received || !segment.body.length) throw new Error('A long settings reply was incomplete');
    parts.push(segment.body);
    received += segment.body.length;
  }
  const bytes = new Uint8Array(received);
  let offset = 0;
  for (const part of parts) { bytes.set(part, offset); offset += part.length; }
  return { status: segment.status, body: bytes };
}

// JSON in, JSON out, like fetch() against the local API.
async function manageJson(opcode, fields = {}, secretText = '') {
  if (secretText && !carriesSecret(opcode)) throw new Error('This change cannot carry a password');
  const body = new TextEncoder().encode(JSON.stringify(fields));
  const reply = await managementExchange(opcode, body, { secret: new TextEncoder().encode(secretText) });
  let json = {};
  try { json = reply.body.length ? JSON.parse(new TextDecoder().decode(reply.body)) : {}; } catch { json = {}; }
  return { status: reply.status, json };
}

// ---- Timer settings panel ---------------------------------------------------
function timerOpenForSettings() {
  return app.live?.state === 'open' && !app.live?.controls_locked;
}

function renderManagementAvailability() {
  const section = $('timerSettingsSection');
  const connected = Boolean(app.server?.connected && app.authenticated);
  section.classList.toggle('hidden', !connected);
  if (!connected) { $('mgmtRestart').disabled = true; return; }
  const supported = managementSupported();
  const open = timerOpenForSettings();
  const wifiOff = app.managementWifiStatus?.network_mode === 'bluetooth';
  let note = 'Changes apply to the connected timer while it is unlocked.';
  if (!supported) note = "This timer's firmware cannot change settings over Bluetooth. Update it from the timer's web page.";
  else if (!open) note = 'Unlock the timer to change its settings. Reading them still works.';
  else if (wifiOff) note = 'Bluetooth only: Wi-Fi, access point settings and remote relay control are unavailable. Restore networking to use them.';
  setText('mgmtAvailability', t(note));
  const blocked = !supported || app.commandBusy || app.pairing || app.lockConfirmationPending;
  const temporaryModeSupported = app.managementWifiStatus?.wifi_suspend_supported === true;
  const disconnectRecovery = app.managementWifiStatus?.wifi_disconnect_restore_supported === true;
  $('mgmtNetworkMode').classList.toggle('hidden', !temporaryModeSupported);
  $('mgmtWifiMode').disabled = blocked || !open || !temporaryModeSupported || !app.managementWifiStatus;
  setText('mgmtWifiMode', t(wifiOff ? 'Restore Wi-Fi / AP' : disconnectRecovery ? 'Use Bluetooth only while connected' : 'Use Bluetooth only until restart'));
  setText('mgmtNetworkPolicy', t(disconnectRecovery
    ? 'Bluetooth only turns off Wi-Fi and the access point while this Bluetooth connection is active. Networking is restored when Bluetooth disconnects. Saved network settings are kept.'
    : 'Bluetooth only turns off Wi-Fi and the access point until restart. Saved network settings are kept. To recover access, unplug the timer and reconnect power.'));
  for (const id of ['mgmtRefresh']) $(id).disabled = blocked;
  for (const id of ['mgmtWifiScan', 'mgmtWifiJoin', 'mgmtWifiForget', 'mgmtApSave', 'mgmtRename', 'mgmtRestart',
    'mgmtPairStart', 'mgmtPairCancel', 'mgmtRevokeAll', 'mgmtBluetoothOff', 'mgmtFirmwareInstall']) {
    $(id).disabled = blocked || !open || (wifiOff && ['mgmtWifiScan', 'mgmtWifiJoin', 'mgmtWifiForget', 'mgmtApSave', 'mgmtBluetoothOff'].includes(id)) || (id === 'mgmtPairStart' && app.approvedDeviceCount >= 3);
  }
  document.querySelectorAll('.mgmt-app-action').forEach((button) => { button.disabled = blocked || !open; });
  for (const id of ['mgmtWifiNetworks', 'mgmtWifiSsid', 'mgmtWifiPassword', 'mgmtApPassword', 'mgmtApUseName', 'mgmtApSecured', 'mgmtApStandalone']) {
    $(id).disabled = blocked || !open || wifiOff;
  }
  $('mgmtApSsid').disabled = blocked || !open || wifiOff || $('mgmtApUseName').checked;
}

function managementMessage(text) {
  setText('mgmtMessage', t(text || ''));
}

function managementError(json, status) {
  return json?.error ? `${json.error} (${status})` : t('The timer returned status {status}.', { status });
}

function validDeviceLabel(label) {
  return label.length > 0 && new TextEncoder().encode(label).length <= 24 && !/[\x00-\x1f\x7f]/.test(label);
}

function confirmManagement(title, message, accept = 'Apply', editName = null, releaseNotes = false, presentation = {}) {
  // One modal owns focus. Cancel stale intent on disconnect or another dialog.
  app.managementConfirmationCancel?.();
  const dialog = $('mgmtConfirm');
  const returnFocus = document.activeElement;
  const backgrounds = [document.querySelector('.shell'), $('settingsPanel'), $('hardwareGuide')];
  const priorInert = backgrounds.map(element => element.inert);
  backgrounds.forEach(element => { element.inert = true; });
  setText('mgmtConfirmEyebrow', t(presentation.eyebrow || 'Timer settings'));
  $('mgmtConfirmLockIcon').classList.toggle('hidden', !presentation.controlLock);
  setText('mgmtConfirmTitle', t(title));
  setText('mgmtConfirmMessage', t(message));
  setText('mgmtConfirmSafety', t(presentation.safety || ''));
  $('mgmtConfirmSafety').classList.toggle('hidden', !presentation.safety);
  if (releaseNotes) window.SmartTimerUpdates.renderNotes($('mgmtConfirmMessage'), t(message));
  $('mgmtConfirmMessage').classList.toggle('release-notes',releaseNotes);
  setText('mgmtConfirmAccept', t(accept));
  $('mgmtConfirmField').classList.toggle('hidden', editName === null);
  $('mgmtConfirmName').value = editName ?? '';
  setText('mgmtConfirmError', '');
  $('mgmtConfirmName').removeAttribute('aria-invalid');
  $('mgmtConfirmBackdrop').classList.add('show');
  dialog.classList.add('show');
  dialog.setAttribute('aria-hidden', 'false');
  return new Promise((resolve) => {
    let settled = false;
    const done = (value) => {
      if (settled) return;
      settled = true;
      app.managementConfirmationCancel = null;
      $('mgmtConfirmBackdrop').classList.remove('show');
      dialog.classList.remove('show');
      $('mgmtConfirmAccept').removeEventListener('click', yes);
      $('mgmtConfirmCancel').removeEventListener('click', no);
      $('mgmtConfirmBackdrop').removeEventListener('click', no);
      backgrounds.forEach((element, index) => { element.inert = priorInert[index]; });
      if (returnFocus?.isConnected && !returnFocus.disabled) returnFocus.focus();
      else if ($('settingsPanel').classList.contains('show')) $('settingsClose').focus();
      dialog.setAttribute('aria-hidden', 'true');
      resolve(value);
    };
    const yes = () => {
      if (editName === null) { done(true); return; }
      const label = $('mgmtConfirmName').value.trim();
      if (!validDeviceLabel(label)) {
        setText('mgmtConfirmError', t('Use a device name of 1–24 UTF-8 bytes, without control characters.'));
        $('mgmtConfirmName').setAttribute('aria-invalid', 'true');
        $('mgmtConfirmName').focus();
        return;
      }
      done(label);
    };
    const no = () => done(false);
    app.managementConfirmationCancel = no;
    $('mgmtConfirmAccept').addEventListener('click', yes);
    $('mgmtConfirmCancel').addEventListener('click', no);
    $('mgmtConfirmBackdrop').addEventListener('click', no);
    if (editName === null) $('mgmtConfirmCancel').focus();
    else { $('mgmtConfirmName').focus(); $('mgmtConfirmName').select(); }
  });
}

// Holds the shared counter: timer commands and settings never interleave.
async function runManagement(label, work) {
  if (app.commandBusy || app.pairing) return;
  app.commandBusy = true;
  updateControlAvailability();
  renderManagementAvailability();
  managementMessage(t('{action}…', { action: t(label) }));
  try {
    const message = await work();
    managementMessage(message || t('Settings updated.'));
  } catch (error) {
    managementMessage(error.message);
    log('err', `${label}: ${error.message}`);
  } finally {
    app.commandBusy = false;
    updateControlAvailability();
    renderManagementAvailability();
  }
}

function renderWifi(json, updateFields = true) {
  app.managementWifiStatus = json;
  const states = { idle: 'Not connected', connecting: 'Connecting to local Wi-Fi', connected: 'Connected to local Wi-Fi',
    disabled: json.wifi_disconnect_restore_supported === true ? 'Bluetooth only while connected.' : 'Bluetooth only (until restart)',
    failed: 'Wi-Fi connection failed', disconnected: 'Wi-Fi disconnected', ap: 'AP Mode', standalone: 'AP Mode' };
  const parts = [t(states[json.state] || json.state || '—')];
  if (json.connected_ssid) parts.push(json.connected_ssid);
  if (json.network_mode === 'ap' && states[json.state] !== 'AP Mode') parts.push(t('AP Mode'));
  if (json.station_ip) parts.push(json.station_ip);
  setText('mgmtWifiState', parts.join(' · '));
  if (updateFields) {
    $('mgmtApStandalone').checked = json.network_mode === 'ap';
    if (!$('mgmtWifiSsid').value && json.saved_ssid) $('mgmtWifiSsid').value = json.saved_ssid;
  }
  renderManagementAvailability();
}

function renderAccessPoint(json) {
  $('mgmtApSsid').value = json.ssid || '';
  $('mgmtApUseName').checked = Boolean(json.uses_device_name);
  $('mgmtApSecured').checked = json.security === 'wpa2';
  $('mgmtApPassword').placeholder = t(json.password_set ? 'Leave blank to keep the current password' : '8–63 characters');
  $('mgmtApSsid').disabled = $('mgmtApUseName').checked;
}

function renderApps(json) {
  const list = $('mgmtApps');
  list.textContent = '';
  const apps = Array.isArray(json.approved_apps) ? json.approved_apps : [];
  app.approvedDeviceCount = apps.length;
  if (!apps.length) {
    const empty = document.createElement('li');
    empty.textContent = t('No approved devices.');
    list.append(empty);
  }
  for (const entry of apps.slice(0, 3)) {
    const row = document.createElement('li');
    const name = document.createElement('strong');
    name.dataset.i18nLiteral = '';
    name.textContent = String(entry.label ?? t('Device {id}', { id: entry.app_id }));
    name.title = name.textContent;
    const detail = document.createElement('span');
    detail.textContent = t(entry.app_id === json.active_app_id ? 'This browser · connected' : 'Approved');
    const rename = document.createElement('button');
    rename.type = 'button';
    rename.className = 'quiet-action mgmt-app-action';
    rename.textContent = t('Rename');
    rename.addEventListener('click', () => renameApproval(entry));
    const remove = document.createElement('button');
    remove.type = 'button';
    remove.className = 'quiet-action mgmt-app-action';
    remove.textContent = t('Remove');
    remove.addEventListener('click', () => removeApproval(entry, entry.app_id === json.active_app_id));
    row.append(name, detail, rename, remove);
    list.append(row);
  }
  $('mgmtPairCapacity').classList.toggle('hidden', apps.length < 3);
  renderManagementAvailability();
}

function renderUpdateStatus(json) {
  window.SmartTimerUpdates.allowance($('mgmtOtaState'), json, t);
}

async function readManagement(opcode) {
  const { status, json } = await manageJson(opcode);
  if (status !== 200) throw new Error(managementError(json, status));
  return json;
}

async function refreshTimerSettings() {
  if (!managementSupported()) { renderManagementAvailability(); return; }
  await runManagement('Reading timer settings', async () => {
    renderWifi(await readManagement(MGMT_OPCODES.wifiStatus));
    renderAccessPoint(await readManagement(MGMT_OPCODES.accessPoint));
    const name = await readManagement(MGMT_OPCODES.deviceName);
    $('mgmtName').value = name.name || '';
    renderApps(await readManagement(MGMT_OPCODES.bluetooth));
    const status = await readManagement(MGMT_OPCODES.status);
    renderUpdateStatus(status);
    const capabilities = await readManagement(MGMT_OPCODES.capabilities);
    // The feed check runs in the browser after the Bluetooth reads finish.
    void updateNotice?.status({ version: status.version, state: status.state, updates: capabilities.updates });
    return 'Timer settings are current.';
  });
}

async function applyChange(label, confirmTitle, confirmText, opcode, fields = {}, secret = '', after, editName = null) {
  const confirmation = await confirmManagement(confirmTitle, confirmText, editName === null ? 'Apply' : 'Save name', editName);
  if (!confirmation) return;
  if (editName !== null) fields = { ...fields, label: confirmation };
  await runManagement(label, async () => {
    let reply;
    try { reply = await manageJson(opcode, fields, secret); }
    catch (error) {
      throw new Error(t('The change was not confirmed and may have applied. Do not repeat it automatically. Reconnect and check settings. {detail}', { detail: error.message }));
    }
    const { status, json } = reply;
    if (status < 200 || status > 202) throw new Error(managementError(json, status));
    if ((opcode === MGMT_OPCODES.appRevoke || opcode === MGMT_OPCODES.pairingRevoke) &&
        json.approval_removed === true && json.disconnecting === true) {
      // Only a verified management receipt removes the local key. A lost reply
      // leaves credentials intact for explicit reconciliation, never a retry.
      const scope = app.credentialScope;
      disconnectTimer();
      app.activeKey = null;
      app.pendingKey = null;
      try {
        await credentialDelete('active', scope);
        await credentialDelete('pending', scope);
      } catch (error) {
        updateCredentialUi();
        return t('Approval removed on the timer. Local key cleanup failed: {detail}. Use Forget saved key before pairing again.', { detail: error.message });
      }
      updateCredentialUi();
      return 'Approval removed. This browser is disconnected; use physical pairing to approve it again.';
    }
    return after ? after(json) : t('Change accepted. Refresh settings to check the result.');
  });
}

async function scanWifi() {
  await runManagement('Scanning Wi-Fi', async () => {
    let { status, json } = await manageJson(MGMT_OPCODES.wifiScanStart);
    if (status < 200 || status > 202) throw new Error(managementError(json, status));
    // One radio serves Wi-Fi and Bluetooth, so the link can stall during a scan.
    for (let attempt = 0; attempt < 15 && json.state === 'scanning'; attempt += 1) {
      await delay(2000);
      ({ status, json } = await manageJson(MGMT_OPCODES.wifiScanResults));
      if (status !== 200) throw new Error(managementError(json, status));
    }
    const select = $('mgmtWifiNetworks');
    select.textContent = '';
    const networks = Array.isArray(json.networks) ? json.networks.slice(0, 40) : [];
    const placeholder = document.createElement('option');
    placeholder.value = '';
    placeholder.textContent = t(networks.length ? 'Choose a network' : 'No networks found');
    select.append(placeholder);
    for (const network of networks) {
      const option = document.createElement('option');
      option.value = String(network.ssid ?? '');
      option.textContent = t(network.secure ? '{ssid} · {rssi} dBm · Secured' : '{ssid} · {rssi} dBm · Open', { ssid: network.ssid, rssi: network.rssi });
      select.append(option);
    }
    return networks.length ? t('Found {count} networks.', { count: networks.length }) : t('No networks found.');
  });
}

function renameApproval(entry) {
  void applyChange('Renaming device', 'Rename approved device', 'Choose a name to recognize this device. This does not change its access.',
    MGMT_OPCODES.appLabel, { app_id: entry.app_id }, '', (json) => { renderApps(json); return t('Renamed.'); }, String(entry.label ?? ''));
}

function removeApproval(entry, current) {
  void applyChange('Removing device', 'Remove this approval?',
    current ? 'This browser will lose access and disconnect. Physical pairing is required to approve it again.'
      : t('“{name}” will lose Bluetooth access. Physical pairing is required to approve it again.', { name: entry.label }),
    MGMT_OPCODES.appRevoke, { app_id: entry.app_id }, '', (json) => { renderApps(json); return 'Approval removed.'; });
}

function crc32Hex(bytes) {
  let crc = 0xffffffff;
  for (const byte of bytes) {
    crc ^= byte;
    for (let bit = 0; bit < 8; bit += 1) crc = (crc >>> 1) ^ (0xedb88320 & -(crc & 1));
  }
  return ((crc ^ 0xffffffff) >>> 0).toString(16).padStart(8, '0');
}

async function installFirmwareOverBluetooth(fileOverride = null, releaseMetadata = {}) {
  const file = fileOverride || $('mgmtFirmwareFile').files?.[0];
  if (!file) { managementMessage('Choose a firmware .bin file first.'); return; }
  const image = new Uint8Array(await file.arrayBuffer());
  if (image.length < 24 || image.length > 4194304 || image[0] !== 0xe9) {
    managementMessage('Choose a firmware application .bin file. Combined flash images are not supported.');
    return;
  }
  const allowUnverified = $('mgmtFirmwareUnverified').checked;
  if (!(await confirmManagement('Install this firmware?',
    t('{name} ({size} KiB) will be sent over Bluetooth. This takes several minutes. Keep this page open, the device close, and the timer powered.', { name: file.name, size: Math.round(image.length / 1024) }),
    'Install'))) return;
  const progress = $('mgmtFirmwareProgress');
  await runManagement('Updating firmware', async () => {
    const status = await readManagement(MGMT_OPCODES.status);
    renderUpdateStatus(status);
    if (!status.ota_armed) throw new Error('Briefly press Reset on the unlocked timer to allow updates, then try again.');
    if (!fileOverride && status.ota_unverified_allowed && !allowUnverified) throw new Error('Unverified recovery mode is on. Confirm it with the checkbox, or wait for it to expire.');
    let { status: code, json } = await manageJson(MGMT_OPCODES.otaBegin, { image_size: image.length, image_crc32: crc32Hex(image), ...releaseMetadata });
    if (code !== 202) throw new Error(managementError(json, code));
    const chunk = Math.min(Number(json.chunk_bytes) || MGMT_FIRMWARE_CHUNK, MGMT_FIRMWARE_CHUNK);
    progress.classList.remove('hidden');
    try {
      for (let offset = 0; offset < image.length; offset += chunk) {
        if (!app.server?.connected) throw new Error('Bluetooth disconnected during the update. The timer keeps its current firmware.');
        const end = Math.min(offset + chunk, image.length);
        const body = new Uint8Array(4 + end - offset);
        new DataView(body.buffer).setUint32(0, offset, true);
        body.set(image.subarray(offset, end), 4);
        const reply = await managementExchange(MGMT_OPCODES.otaChunk, body, { readOnSuccess: false });
        if (reply.status !== 200) {
          const detail = reply.body.length ? JSON.parse(new TextDecoder().decode(reply.body)) : {};
          throw new Error(managementError(detail, reply.status));
        }
        progress.value = Math.round((end * 100) / image.length);
        managementMessage(t('Updating firmware… {percent}%', { percent: progress.value }));
      }
      managementMessage('Validating firmware…');
      ({ status: code, json } = await (async () => {
        const reply = await managementExchange(MGMT_OPCODES.otaFinish, new TextEncoder().encode('{}'), { timeoutMs: 30000 });
        return { status: reply.status, json: JSON.parse(new TextDecoder().decode(reply.body) || '{}') };
      })());
      if (code !== 200 || !json.updated) throw new Error(managementError(json, code));
      return 'Firmware installed. The timer restarts now; reconnect and check its version.';
    } catch (error) {
      if (app.server?.connected) {
        try { await managementExchange(MGMT_OPCODES.otaAbort, new TextEncoder().encode('{}')); } catch { /* best effort */ }
      }
      throw error;
    } finally {
      progress.classList.add('hidden');
    }
  });
}

function wireTimerSettings() {
  updateNotice = window.SmartTimerUpdates.notice({
    root: $('updateNotice'), toggle: $('updateNoticeToggle'), translate: t,
    // The notice only points at the Firmware section's own check and dialog.
    view: () => {
      openSettings($('settingsOpen'));
      setTimeout(() => {
        $('githubUpdates').scrollIntoView({ behavior: 'smooth', block: 'start' });
        $('githubUpdates').querySelector('[data-update-check]').click();
      }, 50);
    },
  });
  window.SmartTimerUpdates.panel({
    root: $('githubUpdates'), translate: t,
    confirm: record => {
      const prompt = window.SmartTimerUpdates.prompt(record, t);
      return confirmManagement(prompt.title, prompt.notes, 'Install', null, true,
        { eyebrow: prompt.eyebrow, safety: prompt.safety });
    },
    device: async () => {
      if (!managementSupported()) throw new Error('Connect and authenticate with the timer first.');
      const current = await readManagement(MGMT_OPCODES.status);
      const capabilities = await readManagement(MGMT_OPCODES.capabilities);
      renderUpdateStatus(current);
      return { updates: capabilities.updates, version: current.version, armed: current.ota_armed, state: current.state };
    },
    install: installFirmwareOverBluetooth,
  });
  $('mgmtWifiMode').addEventListener('click', () => {
    if (!app.managementWifiStatus || app.managementWifiStatus?.wifi_suspend_supported !== true) return;
    const wifiOff = app.managementWifiStatus.network_mode === 'bluetooth';
    const disconnectRecovery = app.managementWifiStatus.wifi_disconnect_restore_supported === true;
    void applyChange('Changing connection mode', wifiOff ? 'Restore Wi-Fi / AP?' : disconnectRecovery ? 'Use Bluetooth only while connected?' : 'Use Bluetooth only until restart?',
      wifiOff ? 'The timer restores its saved Wi-Fi or AP mode. Bluetooth stays available.'
        : disconnectRecovery ? 'Wi-Fi, the access point and remote relay control will stop while this Bluetooth connection is active. If Bluetooth disconnects, the timer automatically restores its saved Wi-Fi or AP mode, including the recovery AP if Wi-Fi cannot connect. Saved settings are unchanged.'
          : 'Wi-Fi, the access point and remote relay control will stop. Bluetooth stays available. Unplug and reconnect power to restore networking if Bluetooth is lost. Saved settings are unchanged.',
      wifiOff ? MGMT_OPCODES.wifiResume : MGMT_OPCODES.wifiSuspend, {}, '', json => {
        renderWifi(json);
        void refreshSystemInfo();
        return wifiOff ? 'Wi-Fi / AP restored.' : disconnectRecovery ? 'Bluetooth only while connected.' : 'Bluetooth only until restart.';
      });
  });
  $('mgmtApUseName').addEventListener('change', () => { $('mgmtApSsid').disabled = $('mgmtApUseName').checked; });
  $('mgmtRefresh').addEventListener('click', () => { void refreshTimerSettings(); });
  $('mgmtWifiScan').addEventListener('click', () => { void scanWifi(); });
  $('mgmtWifiNetworks').addEventListener('change', (event) => {
    if (event.target.value) $('mgmtWifiSsid').value = event.target.value;
  });
  $('mgmtWifiJoin').addEventListener('click', () => {
    const ssid = $('mgmtWifiSsid').value;
    const password = $('mgmtWifiPassword').value;
    if (!ssid) { managementMessage('Enter or choose a Wi-Fi network.'); return; }
    void applyChange('Connecting to Wi-Fi', t('Join {ssid}?', { ssid }),
      'The timer tests the network and saves it only if it connects. Bluetooth may pause briefly while it does.',
      MGMT_OPCODES.wifiConfigure, { ssid }, password, () => {
        $('mgmtWifiPassword').value = '';
        return 'Wi-Fi connection requested. Refresh settings in a few seconds to see the result.';
      });
  });
  $('mgmtWifiForget').addEventListener('click', () => {
    void applyChange('Forgetting Wi-Fi', 'Forget router Wi-Fi?',
      "The timer clears its router credentials and uses its own network (AP Mode).", MGMT_OPCODES.wifiForget);
  });
  $('mgmtApSave').addEventListener('click', () => {
    const secured = $('mgmtApSecured').checked;
    const password = $('mgmtApPassword').value;
    const fields = {
      ssid: $('mgmtApSsid').value, use_device_name: $('mgmtApUseName').checked,
      security: secured ? 'wpa2' : 'open', standalone: $('mgmtApStandalone').checked,
    };
    if (secured && !password) fields.keep_password = true;
    void applyChange('Saving access point', 'Save access point and restart?',
      'The timer restarts, so this Bluetooth connection closes. Reconnect afterwards.',
      MGMT_OPCODES.accessPointSet, fields, secured ? password : '', () => {
        $('mgmtApPassword').value = '';
        return 'Access point saved. The timer is restarting.';
      });
  });
  $('mgmtRename').addEventListener('click', () => {
    const name = $('mgmtName').value.trim();
    void applyChange('Renaming timer', 'Rename the timer and restart?',
      'The timer restarts with the new name, so this Bluetooth connection closes.',
      MGMT_OPCODES.deviceNameSet, { name }, '', () => 'Name saved. The timer is restarting.');
  });
  $('mgmtRestart').addEventListener('click', () => {
    void applyChange('Restarting', 'Restart the timer?', 'The timer restarts and this Bluetooth connection closes.',
      MGMT_OPCODES.restart, {}, '', () => 'The timer is restarting.');
  });
  $('mgmtPairStart').addEventListener('click', () => {
    const label = $('mgmtPairLabel').value.trim();
    if (label && !validDeviceLabel(label)) {
      managementMessage('Use a device name of 1–24 UTF-8 bytes, without control characters.');
      return;
    }
    void applyChange('Opening pairing', 'Pair a new device?',
      'This connection closes. Connect from the new device within two minutes, then briefly press Reset on the timer to approve it.',
      MGMT_OPCODES.pairingStart, label ? { label } : {}, '', () => 'Pairing is open. This browser will disconnect.');
  });
  $('mgmtPairCancel').addEventListener('click', () => {
    void runManagement('Cancelling pairing', async () => {
      const { status, json } = await manageJson(MGMT_OPCODES.pairingCancel);
      if (status !== 200) throw new Error(managementError(json, status));
      renderApps(json);
      return 'Pairing cancelled.';
    });
  });
  $('mgmtRevokeAll').addEventListener('click', () => {
    void applyChange('Removing all devices', 'Remove every approved device?',
      'All approved devices, including this browser, lose Bluetooth access. Each needs physical pairing again.', MGMT_OPCODES.pairingRevoke);
  });
  $('mgmtBluetoothOff').addEventListener('click', () => {
    void applyChange('Turning Bluetooth off', 'Turn Bluetooth off?',
      "The timer's Bluetooth stops, including this connection. Turn it back on from the timer's web page or with the Reset button pairing gesture.",
      MGMT_OPCODES.bluetoothOff);
  });
  $('mgmtFirmwareInstall').addEventListener('click', () => { void installFirmwareOverBluetooth(); });
}

async function readChallenge() {
  const characteristic = app.chars.controlChallenge;
  if (!characteristic) throw new Error('Control challenge characteristic is unavailable');
  const frame = bytesOf(await readGatt(characteristic));
  readProtocolFrame(frame, 20, 'control challenge');
  app.bootId = u32le(frame, 4);
  setText('bootIdLabel', `0x${app.bootId.toString(16).padStart(8, '0')}`);
  log('rx', `control challenge; flags=0x${frame[1].toString(16)} failures=${frame[2]}/${frame[3]}`, frame);
  return frame;
}

async function authenticateWithKey(key, source) {
  const challenge = await readChallenge();
  if (!(challenge[1] & 0x01)) throw new Error('The timer has no approved controller yet');
  const clientNonce = crypto.getRandomValues(new Uint8Array(4));
  const context = concat(u32ToLE(app.bootId), challenge.slice(8, 20), clientNonce);
  const tag = (await hmacSha256(key, concat(new TextEncoder().encode('OST-AUTH-v1'), context))).slice(0, 14);
  const sessionKey = await hmacSha256(key, concat(new TextEncoder().encode('OST-SESSION-v1'), context));
  const frame = new Uint8Array(20);
  frame[0] = PROTOCOL_VERSION;
  frame[1] = OP_CODES.authenticate;
  frame.set(clientNonce, 2);
  frame.set(tag, 6);
  await writeGatt(app.chars.controlCommand, frame, `authenticate (${source})`);

  let confirmed = null;
  for (let attempt = 0; attempt < 20; attempt += 1) {
    await delay(attempt === 0 ? 40 : 100);
    confirmed = await readChallenge();
    if (confirmed[1] & 0x02) break;
  }
  if (!confirmed || !(confirmed[1] & 0x02)) {
    throw new Error('The timer did not accept this controller key');
  }

  app.sessionKey = sessionKey;
  app.commandCounter = 0;
  app.authenticated = true;
  log('info', `Application authentication confirmed using ${source} credential.`);
}

async function subscribeCharacteristic(name, handler) {
  const characteristic = app.chars[name];
  if (!characteristic) throw new Error(`${name} characteristic is unavailable`);
  const epoch = gattQueue.epoch;
  characteristic.addEventListener('characteristicvaluechanged', (event) => {
    if (epoch === gattQueue.epoch) handler(bytesOf(event.target.value));
  });
  await gattQueue.run(() => characteristic.startNotifications());
  log('info', `Subscribed to ${name}.`);
}

async function activateAuthorizedSession() {
  await rememberAuthenticatedTimer();
  if (!app.subscribed) {
    await subscribeCharacteristic('controlResult', onResultFrame);
    await subscribeCharacteristic('liveStatus', onStatusFrame);
    await subscribeCharacteristic('eventDetail', onEventFrame);
    if (managementSupported()) await subscribeCharacteristic('managementResponse', onManagementReady);
    app.subscribed = true;
  }
  await readLiveSnapshot(true);
  await refreshSystemInfo();
  await reconcilePendingOperation();
  renderStatusFreshness(true);
  updateConnectionCard();
  updateCredentialUi();
  if (managementSupported()) void refreshTimerSettings();
}

async function recoverOrAuthenticate() {
  if (app.pendingKey) {
    try {
      await authenticateWithKey(app.pendingKey, 'pending');
      await promotePendingCredential();
      await activateAuthorizedSession();
      showToast('Pairing recovered', 'The pending controller key was confirmed safely.');
      return true;
    } catch (error) {
      log('err', `Pending pairing recovery was not accepted: ${error.message}`);
      app.authenticated = false;
      app.sessionKey = null;
      if (!app.server?.connected) throw error;
      await delay(550);
    }
  }

  if (app.activeKey) {
    await authenticateWithKey(app.activeKey, 'saved');
    await activateAuthorizedSession();
    return true;
  }

  await readChallenge();
  renderNotReady();
  setConnection('pairing', 'Approval needed');
  updateConnectionCard();
  return false;
}

async function readPublicCharacteristics() {
  const infoBytes = bytesOf(await readGatt(app.chars.protocolInfo));
  const infoText = new TextDecoder().decode(infoBytes);
  const info = JSON.parse(infoText);
  if (info.protocol !== 'ost-ble/1') throw new Error(`Unsupported Bluetooth protocol: ${info.protocol || 'unknown'}`);
  app.protocolInfo = info;
  loadPendingOperation();
  const advertisedBootId = Number.parseInt(info.boot_id || '', 16);
  if (app.bootId && Number.isFinite(advertisedBootId) && app.bootId !== advertisedBootId) {
    app.events = [];
    renderRecentActivity();
  }
  if (Number.isFinite(advertisedBootId)) app.bootId = advertisedBootId;
  const friendlyName = info.device_name || 'Smart Timer';
  setText('protocolLabel', info.protocol);
  setText('deviceBrandName', friendlyName);
  setText('deviceName', friendlyName);
  setText('bleNameLabel', app.device?.name || '—');
  setText('modelLabel', info.model || 'MagBound Smart Timer V2');
  setText('deviceIdLabel', info.device_id?.toUpperCase?.() || '—');
  setText('firmwareLabel', info.firmware || '—');
  setText('buildIdLabel', info.build_id || '—');
  setText('hostnameLabel', info.hostname ? `${info.hostname}.local` : '—');
  setText('deviceLabel', app.device?.name || 'Bluetooth');
  setText('footerDevice', friendlyName);
  document.title = `${friendlyName} · ${t('Bluetooth control')}`;
  log('rx', `protocol info: protocol=${info.protocol}; firmware=${info.firmware}; model=${info.model}; device_id=${info.device_id}; boot_id=${info.boot_id}; build_id=${info.build_id}`);

  const capabilities = bytesOf(await readGatt(app.chars.capabilities));
  readProtocolFrame(capabilities, 24, 'capabilities');
  if (capabilities[1] !== 0xff || capabilities[2] !== 0xff ||
      capabilities[16] !== 32 || capabilities[17] !== 20 ||
      capabilities[18] !== 20 || capabilities[20] !== 20 ||
      capabilities[21] !== 44 || capabilities[22] !== 8 ||
      capabilities[23] !== 14) {
    throw new Error('This timer uses an incompatible Bluetooth schema. Update the timer firmware and this app together.');
  }
  log('rx', `capabilities; operations=0x${capabilities[1].toString(16)} security=0x${capabilities[2].toString(16)} connection_limit=${capabilities[3]} maximum_duration_ms=${u32le(capabilities, 4)} command_bytes=${capabilities[16]} status_bytes=${capabilities[18]} event_bytes=${capabilities[20]} system_bytes=${capabilities[21]}`, capabilities);
}

async function selectTimerDevice(target = null) {
  const savedId = target ? target.bluetoothId : localStorage.getItem(DEVICE_ID_KEY);
  if (savedId && (app.activeKey || app.pendingKey) && navigator.bluetooth.getDevices) {
    const granted = await navigator.bluetooth.getDevices();
    const remembered = granted.find((device) => device.id === savedId);
    if (remembered) {
      app.connectingRemembered = true;
      return remembered;
    }
  }
  app.connectingRemembered = false;
  return navigator.bluetooth.requestDevice({
    filters: [{ services: [SERVICE_UUID] }],
    optionalServices: [SERVICE_UUID],
  });
}

async function connectTimer(target = null) {
  if (app.connecting || app.server?.connected) return;
  if (!target?.id) target = null; // Also used directly as a click handler.
  if (!navigator.bluetooth) {
    showFeedback('error', 'Web Bluetooth unavailable', 'Use a supported Chromium-based browser over HTTPS or localhost.', 0);
    return;
  }

  app.connecting = true;
  gattQueue.reset();
  const epoch = gattQueue.epoch;
  const checkConnection = () => {
    if (epoch !== gattQueue.epoch) throw new Error('Connection attempt cancelled');
  };
  setConnection('pairing', 'Connecting…');
  updateConnectionCard();
  try {
    const device = await selectTimerDevice(target);
    checkConnection();
    app.device = device;
    localStorage.setItem(DEVICE_ID_KEY, device.id);
    device.addEventListener('gattserverdisconnected', () => {
      if (app.device !== device || epoch !== gattQueue.epoch) return;
      if (app.authenticated) app.lostAt = performance.now();
      handleDisconnected();
    }, { once: true });
    log('info', `Selected ${device.name || 'unnamed Smart Timer'} (${device.id.slice(0, 8)}…).`);
    app.server = await gattQueue.run(async () => {
      const server = await device.gatt.connect();
      if (epoch !== gattQueue.epoch) {
        server.disconnect();
        throw new Error('Connection attempt cancelled');
      }
      return server;
    });
    checkConnection();
    app.service = await gattQueue.run(() => app.server.getPrimaryService(SERVICE_UUID));
    app.chars = {};
    for (const [name, uuid] of Object.entries(CHAR)) {
      app.chars[name] = await gattQueue.run(() => app.service.getCharacteristic(uuid));
    }
    await readPublicCharacteristics();
    // Use this timer's own saved key; the chooser may have picked another timer.
    await loadCredentials(timerScope(app.protocolInfo.device_id));
    if (app.protocolInfo.pairing_nickname) {
      app.chars.pairingNickname = await gattQueue.run(() => app.service.getCharacteristic(PAIRING_NICKNAME_UUID));
    }
    if (app.protocolInfo.management === 1) {
      app.chars.managementRequest = await gattQueue.run(() => app.service.getCharacteristic(MANAGEMENT_REQUEST_UUID));
      app.chars.managementResponse = await gattQueue.run(() => app.service.getCharacteristic(MANAGEMENT_RESPONSE_UUID));
    }
    checkConnection();
    app.connecting = false;
    await recoverOrAuthenticate();
  } catch (error) {
    if (epoch !== gattQueue.epoch) return;
    log('err', `Connection failed: ${error.message}`);
    app.connecting = false;
    if (app.server?.connected) {
      app.server.disconnect();
    } else {
      handleDisconnected();
    }
    showFeedback('error', 'Bluetooth connection failed', error.message, 0);
    if (app.connectingRemembered) {
      localStorage.removeItem(DEVICE_ID_KEY);
      showFeedback('error', 'Reconnect failed', `${error.message}. Select Connect again to choose the timer manually.`, 0);
    }
  } finally {
    if (epoch === gattQueue.epoch) {
      app.connectingRemembered = false;
      updateConnectionCard();
    }
  }
}

function handleDisconnected() {
  app.managementConfirmationCancel?.();
  app.managementWifiStatus = null;
  app.approvedDeviceCount = null;
  gattQueue.reset();
  app.liveGeneration += 1;
  app.liveReadTask = null;
  app.liveRefreshTask = null;
  app.liveRetryAt = 0;
  const wasConnected = Boolean(app.server || app.authenticated);
  app.server = null;
  app.service = null;
  app.chars = {};
  app.sessionKey = null;
  app.authenticated = false;
  app.connecting = false;
  app.connectingRemembered = false;
  app.pairing = false;
  app.commandBusy = false;
  app.systemRefreshBusy = false;
  app.subscribed = false;
  if (app.pendingResult) {
    app.pendingResult.reject(new Error('Timer disconnected before the command result arrived'));
    app.pendingResult = null;
  }
  if (app.managementWaiter) {
    clearTimeout(app.managementWaiter.timer);
    app.managementWaiter.reject(new Error('Timer disconnected before the settings reply arrived'));
    app.managementWaiter = null;
  }
  app.managementEarlyReady = null;
  app.managementBusy = false;
  setConnection('offline', 'Disconnected');
  updateNotice?.status({ state: 'disconnected' });
  renderNotReady();
  updateConnectionCard();
  renderConnectionWarning();
  if (wasConnected) log('info', 'Bluetooth disconnected.');
}

// Mirrors the local web page: a lost link or out-of-date status gets a banner
// with the age of the last reply. Nothing here infers the timer's state.
function renderConnectionWarning() {
  const connected = Boolean(app.server?.connected && app.authenticated);
  if (connected && liveStatusFresh()) app.lostAt = 0;
  const lastReply = app.receivedAt || app.lostAt;
  const age = Math.max(0, Math.round((performance.now() - lastReply) / 1000));
  const stale = connected && !document.hidden && Boolean(app.live) &&
    performance.now() - app.receivedAt > LIVE_STATUS_MAX_AGE_MS;
  const lost = !connected && !app.connecting && app.lostAt > 0;
  $('connectionWarning').classList.toggle('hidden', !stale && !lost);
  if (!stale && !lost) return;
  setText('connectionWarningTitle', t(lost ? 'Connection lost' : 'Timer status unavailable'));
  setText('connectionWarningMessage', t('Last reply {seconds} s ago. The timer keeps running on its own; use the physical emergency release if needed.', { seconds: age }));
  setText('connectionRetry', t(lost ? 'Connect' : 'Retry'));
}

function retryConnection() {
  if (app.server?.connected) {
    app.liveRetryAt = 0;
    void refreshLiveStatus();
  } else {
    void connectTimer();
  }
}

function disconnectTimer({ lost = false } = {}) {
  const server = app.server || app.device?.gatt;
  app.lostAt = lost && app.authenticated ? performance.now() : 0;
  handleDisconnected();
  if (server?.connected) server.disconnect();
}

async function applyPairingNickname() {
  if (!app.chars.pairingNickname) return; // Optional protocol extension.
  const record = bytesOf(await readGatt(app.chars.pairingNickname));
  if (record.length < 2 || record[0] !== 1 || !(record[1] & 2))
    throw new Error('Nickname pairing window is not open');
  // A name explicitly chosen on the timer's local page takes priority.
  if (record[1] & 1) return;
  const nickname = $('pairingNickname').value.trim();
  const encoded = new TextEncoder().encode(nickname);
  if (!encoded.length || encoded.length > 24 || /[\u0000-\u001f\u007f]/.test(nickname))
    throw new Error('Device nickname must contain 1–24 UTF-8 bytes without controls');
  await writeGatt(app.chars.pairingNickname, encoded, 'pairing nickname');
  const accepted = bytesOf(await readGatt(app.chars.pairingNickname));
  if (accepted.length < 2 || accepted[0] !== 1 || !(accepted[1] & 2) ||
      new TextDecoder('utf-8', {fatal:true}).decode(accepted.slice(2)) !== nickname)
    throw new Error('Timer did not accept this device nickname');
}

async function finishPairing() {
  if (!app.server?.connected || app.pairing) return;
  app.pairing = true;
  let deviceCommitConfirmed = false;
  updateConnectionCard();
  showFeedback('working', 'Finishing pairing', 'Keep this page open while the timer saves and verifies this browser.', 0);
  try {
    const secret = bytesOf(await readGatt(app.chars.pairingSecret));
    if (secret.length !== 16) throw new Error(`The timer returned a ${secret.length}-byte pairing key instead of 16 bytes`);

    await applyPairingNickname();

    // Persist the candidate before asking the timer to promote it. A lost
    // response or disconnect can then be recovered without destroying the
    // last known-good active credential.
    await credentialWrite('pending', secret);
    app.pendingKey = secret;
    updateCredentialUi();
    if (navigator.storage?.persist) {
      try { await navigator.storage.persist(); } catch {}
    }

    const challenge = await readChallenge();
    const bootId = u32le(challenge, 4);
    const clientNonce = crypto.getRandomValues(new Uint8Array(4));
    const tag = (await hmacSha256(secret, concat(
      new TextEncoder().encode('OST-PAIR-COMMIT-v1'), u32ToLE(bootId), clientNonce,
    ))).slice(0, 14);
    const commit = new Uint8Array(20);
    commit[0] = PROTOCOL_VERSION;
    commit[1] = OP_CODES.pairingCommit;
    commit.set(clientNonce, 2);
    commit.set(tag, 6);
    await writeGatt(app.chars.controlCommand, commit, 'pairing commit');

    for (let attempt = 0; attempt < 50; attempt += 1) {
      await delay(100);
      const confirmation = await readChallenge();
      if (confirmation[1] & 0x04) {
        deviceCommitConfirmed = true;
        break;
      }
    }
    if (!deviceCommitConfirmed) {
      throw new Error('The timer did not confirm durable pairing storage; the pending key was preserved for recovery');
    }

    await authenticateWithKey(secret, 'newly paired');
    await promotePendingCredential();
    await activateAuthorizedSession();
    $('commandFeedback').classList.add('hidden');
    showToast('Pairing complete', 'This browser can now control the timer over Bluetooth.');
    log('info', 'Pairing was durably confirmed and authenticated before local promotion.');
  } catch (error) {
    log('err', `Pairing did not complete: ${error.message}`);
    app.authenticated = false;
    app.sessionKey = null;
    const recovery = deviceCommitConfirmed
      ? 'The timer confirmed storage of the new key. It remains pending in this browser; reconnect to verify and recover it.'
      : 'The timer did not confirm a controller change. The candidate key remains pending in this browser for diagnosis or recovery.';
    showFeedback('error', 'Pairing not completed', `${error.message}. ${recovery}`, 0);
  } finally {
    app.pairing = false;
    updateConnectionCard();
    updateControlAvailability();
  }
}

function buildCommand(op, requestId, parameters, intent) {
  const frame = new Uint8Array(32);
  const view = new DataView(frame.buffer);
  frame[0] = PROTOCOL_VERSION;
  frame[1] = OP_CODES[op];
  view.setUint16(2, requestId, true);
  view.setUint32(4, intent.bootId, true);
  view.setUint32(8, intent.sessionSequence, true);
  view.setInt32(12, parameters.valueA ?? 0, true);
  view.setInt32(16, parameters.valueB ?? 0, true);
  view.setUint32(20, ++app.commandCounter, true);
  return frame;
}

async function signCommand(frame) {
  if (!app.sessionKey) throw new Error('Bluetooth authentication is not active');
  const tag = (await hmacSha256(app.sessionKey, concat(
    new TextEncoder().encode('OST-CMD-v1'), frame.slice(0, 24),
  ))).slice(0, 8);
  frame.set(tag, 24);
  return frame;
}

function pendingStorageKey(deviceId) {
  return PENDING_OPERATION_KEY + ':' + deviceId;
}

function loadPendingOperation() {
  const encoded = localStorage.getItem(pendingStorageKey(app.protocolInfo.device_id));
  const operation = encoded ? JSON.parse(encoded) : null;
  if (operation && (!validPendingOperation(operation) || operation.deviceId !== app.protocolInfo.device_id ||
      OP_CODES[operation.op] !== operation.opcode)) {
    throw new Error('The saved unresolved command is invalid; do not retry it. Use local timer control to verify release.');
  }
  app.pendingOperation = operation;
}

function persistPendingOperation(operation) {
  // This journal contains command semantics, never controller/session keys.
  // It must be stored before a write can possibly affect the timer.
  localStorage.setItem(pendingStorageKey(operation.deviceId), JSON.stringify(operation));
  app.pendingOperation = operation;
}

function clearPendingOperation(operation) {
  if (app.pendingOperation?.requestId !== operation.requestId) return;
  try {
    localStorage.removeItem(pendingStorageKey(operation.deviceId));
  } catch (error) {
    log('err', 'The command was confirmed, but its browser journal could not be cleared: ' + error.message);
  }
  app.pendingOperation = null;
}

function showUnknownOperation(message) {
  if (message) log('err', message);
  showFeedback('error', 'Command outcome unknown',
    'The timer may have applied the command. Reconnect to check its result. Do not repeat a time adjustment. Unlock remains available; other changes wait for confirmation or an open timer.', 0);
}

async function reconcilePendingOperation() {
  const operation = app.pendingOperation;
  if (!operation || app.reconciliationBusy || app.commandBusy || !app.authenticated) return;
  app.reconciliationBusy = true;
  const epoch = gattQueue.epoch;
  try {
    const result = decodeResult(bytesOf(await readGatt(app.chars.controlResult)));
    // Always display current state, not the historical state of a cached retry.
    const current = await readLiveSnapshot(true);
    if (epoch !== gattQueue.epoch || app.pendingOperation !== operation || app.commandBusy) return;
    if (app.bootId !== operation.bootId) {
      clearPendingOperation(operation);
      showFeedback('working', 'Previous result unavailable',
        'The original outcome could not be recovered. The current timer state was read successfully; no old command was sent again.', 0);
    } else if (resultMatches(operation, result)) {
      clearPendingOperation(operation);
      if (result.result !== 'applied' && operation.predecessor && current.state !== 'open')
        persistPendingOperation(operation.predecessor);
      showFeedback(result.result === 'applied' ? 'success' : 'error',
        'Previous command confirmed',
        result.result === 'applied'
          ? 'The original command was applied. It was not sent again; the current timer state is shown.'
          : friendlyResult(result.result), 0);
    } else if (current.state === 'open') {
      clearPendingOperation(operation);
      showFeedback('working', 'Previous result unavailable',
        'The original outcome could not be recovered. The current timer state was read successfully; no old command was sent again.', 0);
    } else {
      showUnknownOperation();
    }
  } catch (error) {
    if (epoch === gattQueue.epoch && app.pendingOperation === operation)
      showUnknownOperation('The saved result could not be verified. Do not repeat the command. Reconnect, unlock, or wait for expiry. ' + error.message);
  } finally {
    app.reconciliationBusy = false;
    updateControlAvailability();
  }
}

function commandIntent(op, parameters) {
  return {
    op, opcode: OP_CODES[op], epoch: gattQueue.epoch,
    bootId: app.bootId, sessionSequence: op === 'start' ? 0 : app.sessionSequence,
    deviceId: app.protocolInfo?.device_id,
    valueA: parameters.valueA ?? 0, valueB: parameters.valueB ?? 0,
  };
}

async function executeCommand(op, parameters = {}, intent = commandIntent(op, parameters)) {
  if (!app.server?.connected || !app.authenticated || !app.live) {
    const error = new Error('Connect and authenticate before sending timer commands');
    error.outcome = 'not_sent';
    throw error;
  }
  if (app.pendingOperation && op !== 'release') {
    throw new UnknownCommandOutcome('Resolve the previous command first. Unlock is still available.', app.pendingOperation);
  }
  if (!['release', 'lock'].includes(op) && !liveStatusFresh()) {
    const error = new Error('Current timer status is unavailable. Wait for a fresh status update before changing the timer.');
    error.outcome = 'not_sent';
    throw error;
  }

  app.commandBusy = true;
  updateControlAvailability();
  let operation = null;
  let writeAttempted = false;
  let before = { ...app.live };
  const previousPending = app.pendingOperation;
  try {
    if (intent.epoch !== gattQueue.epoch || intent.bootId !== app.bootId ||
        intent.deviceId !== app.protocolInfo?.device_id ||
        (!['start', 'release'].includes(op) && intent.sessionSequence !== app.sessionSequence)) {
      throw new Error('The connection or timer session changed; this queued command was cancelled');
    }
    if (op === 'release' || op === 'lock') {
      // Unlock targets the currently observed session. Lock rechecks the
      // original running session after the operator's confirmation dialog.
      const current = await readLiveSnapshot(true);
      if (intent.epoch !== gattQueue.epoch || intent.bootId !== app.bootId || intent.deviceId !== app.protocolInfo?.device_id)
        throw new Error('The timer changed. Review its current state before trying again.');
      if (op === 'release') intent = { ...intent, sessionSequence: current.session_sequence };
      else if (current.state !== 'running' || current.session_sequence !== intent.sessionSequence || current.emergency_input_active) {
        throw new Error('The timer changed. Review its current state before trying again.');
      }
      before = { ...current };
    }
    if (before.controls_locked) throw new Error('Software controls are locked until expiry. Use the physical emergency release if needed.');
    const requestId = allocateRequestId();
    operation = { schema: 1, deviceId: intent.deviceId, op, opcode: OP_CODES[op], requestId,
      bootId: intent.bootId, sessionSequence: intent.sessionSequence,
      valueA: intent.valueA, valueB: intent.valueB };
    if (op === 'release' && previousPending)
      operation.predecessor = previousPending.predecessor || previousPending;
    if (!validPendingOperation(operation)) throw new Error('Invalid timer command parameters');
    const frame = await signCommand(buildCommand(op, requestId, parameters, intent));
    if (intent.epoch !== gattQueue.epoch) throw new Error('Bluetooth disconnected before the command was sent');
    if (!liveStatusFresh()) throw new Error('Timer status became unavailable before the command was sent');
    persistPendingOperation(operation);

    let resolveResult;
    let rejectResult;
    const promise = new Promise((resolve, reject) => { resolveResult = resolve; rejectResult = reject; });
    // Disconnect may reject this while a write is still awaiting its ACK.
    promise.catch(() => {});
    app.pendingResult = { operation, resolve: resolveResult, reject: rejectResult, promise };

    const result = await exchangeCommand({
      operation, frame,
      connected: () => Boolean(app.server?.connected && app.authenticated && intent.epoch === gattQueue.epoch),
      write: async (bytes, attempt) => {
        writeAttempted = true;
        await writeGatt(app.chars.controlCommand, bytes, op + (attempt ? ' retry' : ''));
      },
      wait: milliseconds => withTimeout(promise, milliseconds),
      read: async () => decodeResult(bytesOf(await readGatt(app.chars.controlResult))),
    });
    clearPendingOperation(operation);
    if (result.result !== 'applied') {
      if (operation.predecessor) persistPendingOperation(operation.predecessor);
      if (intent.epoch === gattQueue.epoch) {
        try { await readLiveSnapshot(); } catch {}
      }
      const rejection = new Error(friendlyResult(result.result));
      rejection.resultCode = result.result;
      rejection.outcome = 'rejected';
      throw rejection;
    }

    if (intent.epoch === gattQueue.epoch && app.server?.connected) {
      const optimistic = snapshotFromResult(before, operation, result);
      if (optimistic) acceptLiveSnapshot(optimistic, false, false);
      try { await readLiveSnapshot(); }
      catch (error) { log('err', 'Command confirmed, but current status refresh failed: ' + error.message); }
      if (intent.epoch === gattQueue.epoch) showCommandSuccess(op, result);
    } else {
      showFeedback('success', 'Previous command confirmed',
        'The command result was received before disconnection. Reconnect to read the current timer state.', 0);
    }
    return result;
  } catch (error) {
    if (!writeAttempted) {
      if (operation) clearPendingOperation(operation);
      if (previousPending && !app.pendingOperation) persistPendingOperation(previousPending);
      error.outcome = 'not_sent';
    }
    throw error;
  } finally {
    if (app.pendingResult?.operation === operation) app.pendingResult = null;
    app.commandBusy = false;
    updateControlAvailability();
  }
}

function enqueueCommand(op, parameters = {}, intent = commandIntent(op, parameters)) {
  // Capture intent now, not after a queued task/reconnection changes the timer.
  const result = commandQueue.then(() => executeCommand(op, parameters, intent));
  commandQueue = result.catch(() => {});
  return result.catch(error => {
    log('err', op + ' failed: ' + error.message);
    if (error.outcome === 'unknown') showUnknownOperation(error.message);
    else showFeedback('error', error.outcome === 'rejected' ? 'Command rejected' : 'Command not sent', error.message, 0);
    throw error; // Programmatic clients can distinguish success from failure.
  });
}

function dispatchCommand(op, parameters = {}) {
  // Event handlers deliberately consume the reported error; enqueueCommand
  // itself preserves rejection for tests and callers that need the outcome.
  void enqueueCommand(op, parameters).catch(() => {});
}

async function requestControlLock() {
  if (app.lockConfirmationPending || app.commandBusy || app.pendingOperation || !app.authenticated ||
      !app.server?.connected || !liveStatusFresh() || app.live?.state !== 'running' || app.live.controls_locked) return;
  // Approval belongs to the timer and session displayed when the dialog opened.
  // A reconnect, restart or new session must never inherit that approval.
  const intent = commandIntent('lock', {});
  // A Safari mouse click need not focus the button before opening the dialog.
  const trigger = $('sealButton');
  app.lockConfirmationPending = true;
  updateControlAvailability();
  try {
    if (!await confirmManagement('Lock software controls?',
      'Pause, time adjustment, and Unlock now will be disabled until this timer expires.',
      'Lock until expiry', null, false, {
        controlLock: true, eyebrow: 'One-way session setting',
        safety: 'After this, only the emergency input or removing power can end the session early. Neither is guaranteed to open the lock. Keep an independent way out.',
      })) return;
    await enqueueCommand('lock', {}, intent);
  } catch (error) {
    // enqueueCommand reports command errors and preserves unknown outcomes.
    if (!error.outcome) showFeedback('error', 'Command not sent', error.message, 0);
  }
  finally {
    app.lockConfirmationPending = false;
    updateControlAvailability();
    if (trigger?.isConnected && !trigger.disabled && trigger.offsetParent !== null) trigger.focus();
  }
}

function friendlyResult(code) {
  const messages = {
    invalid_frame: 'The command was malformed and was not applied.',
    unauthorized: 'Bluetooth authorization expired. Disconnect and reconnect.',
    stale_boot: 'The timer restarted. Disconnect and reconnect before retrying.',
    stale_session: 'The timer session changed before this command arrived. Status has been refreshed.',
    invalid_state: 'That action is not available in the current timer state.',
    invalid_duration: 'Choose a duration between 1 second and 24 hours.',
    emergency_input_active: 'The emergency input is active, so a timer cannot start.',
    controls_locked: 'Software controls are locked until the timer expires.',
    control_disabled: 'This control is switched off in the timer’s settings.',
    maximum_duration_reached: 'Maximum lock duration reached. The timer cannot exceed 24 hours.',
    unsupported_opcode: 'This firmware does not support that command.',
    operation_conflict: 'The request identifier conflicts with a recent command. Reconnect before retrying.',
    system_transition: 'The timer is restarting or updating and cannot accept this command.',
  };
  return messages[code] || `The timer rejected the command (${code}).`;
}

function showCommandSuccess(op, result) {
  if (result.duplicate) {
    showFeedback('success', 'Previous command confirmed', 'The original command was applied once. This duplicate did not change the timer; the current status is shown.');
    return;
  }
  const messages = {
    start: ['Timer started', 'Bluetooth returned the resulting timer state.'],
    pause: ['Timer paused', 'The remaining duration is preserved.'],
    resume: ['Timer resumed', 'The countdown is running again.'],
    release: ['Timer unlocked', 'The lock output was released.'],
    adjust: ['Duration updated', 'The timer returned the new remaining duration.'],
    lock: ['Software controls locked', 'The timer must expire; physical release methods remain available.'],
    identify: ['Identify started', 'The blue status LED is blinking temporarily.'],
  };
  const [title, message] = messages[op] || ['Command applied', 'The timer confirmed the resulting state.'];
  showFeedback('success', title, result.duplicate ? `${message} This was a safe duplicate response; it was not re-applied.` : message);
}

function fixedDurationSeconds() {
  return Number($('hours').value || 0) * 3600 + Number($('minutes').value || 0) * 60 + Number($('seconds').value || 0);
}

function setFixedDurationSeconds(seconds) {
  const requested = Math.round(seconds);
  if (requested > 86400) showFeedback('error', 'Maximum lock duration reached', 'The lock duration cannot exceed 24 hours.');
  const total = Math.max(0, Math.min(86400, requested));
  $('hours').value = String(Math.floor(total / 3600));
  $('minutes').value = String(Math.floor((total % 3600) / 60));
  $('seconds').value = String(total % 60);
  syncDurationMode();
}

function setFixedMinutes(minutes) {
  $('randomMode').checked = false;
  setFixedDurationSeconds(minutes * 60);
}

function syncDurationMode() {
  const editable = !app.live || app.live.state === 'open';
  const random = $('randomMode').checked;
  $('fixedDuration').classList.toggle('hidden', !editable || random);
  $('randomFields').classList.toggle('hidden', !editable || !random);
  $('durationGestureHint').classList.toggle('hidden', !editable);
  document.querySelectorAll('[data-preset]').forEach((button) => {
    const selectedMinutes = Number($('hours').value) * 60 + Number($('minutes').value);
    button.classList.toggle('active', !random && Number(button.dataset.preset) === selectedMinutes && Number($('seconds').value) === 0);
  });
}

function adjustFixedDuration(fieldId, steps) {
  const units = { hours: 3600, minutes: 60, seconds: 1 };
  setFixedDurationSeconds(fixedDurationSeconds() + steps * units[fieldId]);
}

function normalizeRandomDuration(fieldId) {
  const requestedMinimum = Number($('randomMin').value) || 1;
  const requestedMaximum = Number($('randomMax').value) || 1;
  if (requestedMinimum > 1440 || requestedMaximum > 1440) {
    showFeedback('error', 'Maximum lock duration reached', 'The lock duration cannot exceed 24 hours.');
  }
  let minimum = Math.max(1, Math.min(1440, requestedMinimum));
  let maximum = Math.max(1, Math.min(1440, requestedMaximum));
  if (minimum > maximum) {
    if (fieldId === 'randomMin') maximum = minimum;
    else minimum = maximum;
  }
  $('randomMin').value = String(minimum);
  $('randomMax').value = String(maximum);
}

function adjustRandomDuration(fieldId, steps) {
  let minimum = Math.max(1, Math.min(1440, Number($('randomMin').value) || 1));
  let maximum = Math.max(minimum, Math.min(1440, Number($('randomMax').value) || minimum));
  if (fieldId === 'randomMin') minimum = Math.max(1, Math.min(maximum, minimum + steps));
  else maximum = Math.max(minimum, Math.min(1440, maximum + steps));
  $('randomMin').value = String(minimum);
  $('randomMax').value = String(maximum);
}

function wireDurationScrubber(fieldId, enabled, adjust, normalize) {
  const input = $(fieldId);
  const label = input.closest('label');
  const canAdjust = () => !input.disabled && enabled();
  let pointerId = null;
  let pointerStartY = 0;
  let appliedSteps = 0;
  let dragged = false;
  let suppressClickUntil = 0;
  let wheelPixels = 0;
  let wheelReset = 0;

  input.addEventListener('pointerdown', (event) => {
    if (!canAdjust() || pointerId !== null || (event.button !== undefined && event.button !== 0)) return;
    pointerId = event.pointerId;
    pointerStartY = event.clientY;
    appliedSteps = 0;
    dragged = false;
    input.setPointerCapture?.(pointerId);
  });
  input.addEventListener('pointermove', (event) => {
    if (pointerId !== event.pointerId || !canAdjust()) return;
    const nextSteps = Math.trunc((pointerStartY - event.clientY) / 18);
    if (nextSteps === appliedSteps) return;
    event.preventDefault();
    adjust(nextSteps - appliedSteps);
    appliedSteps = nextSteps;
    dragged = true;
    label.classList.add('dragging');
  });
  const finish = (event) => {
    if (pointerId !== event.pointerId) return;
    if (dragged) {
      suppressClickUntil = performance.now() + 300;
      input.blur();
    }
    label.classList.remove('dragging');
    pointerId = null;
  };
  input.addEventListener('pointerup', finish);
  input.addEventListener('pointercancel', finish);
  input.addEventListener('lostpointercapture', finish);
  input.addEventListener('click', (event) => {
    if (performance.now() < suppressClickUntil) event.preventDefault();
  });
  input.addEventListener('wheel', (event) => {
    if (!canAdjust()) return;
    event.preventDefault();
    clearTimeout(wheelReset);
    wheelPixels += event.deltaY * (event.deltaMode === 1 ? 16 : 1);
    const steps = Math.trunc(wheelPixels / 42);
    if (steps !== 0) {
      adjust(-steps);
      wheelPixels -= steps * 42;
    }
    wheelReset = setTimeout(() => { wheelPixels = 0; }, 180);
  }, { passive: false });
  input.addEventListener('change', () => { if (canAdjust()) normalize(); });
}

function openSettings(trigger) {
  app.settingsReturnFocus = trigger || document.activeElement;
  $('settingsPanel').classList.add('show');
  $('settingsBackdrop').classList.add('show');
  $('settingsPanel').setAttribute('aria-hidden', 'false');
  $('settingsBackdrop').setAttribute('aria-hidden', 'false');
  document.querySelector('.shell').inert = true;
  document.body.classList.add('panel-open');
  $('settingsClose').focus();
}

function closeSettings(restoreFocus = true) {
  app.managementConfirmationCancel?.();
  $('settingsPanel').classList.remove('show');
  $('settingsBackdrop').classList.remove('show');
  $('settingsPanel').setAttribute('aria-hidden', 'true');
  $('settingsBackdrop').setAttribute('aria-hidden', 'true');
  document.querySelector('.shell').inert = false;
  document.body.classList.remove('panel-open');
  if (restoreFocus) app.settingsReturnFocus?.focus?.();
}

function openHardwareGuide(trigger) {
  app.guideReturnFocus = trigger || document.activeElement;
  $('hardwareGuide').classList.add('show');
  $('hardwareGuideBackdrop').classList.add('show');
  $('hardwareGuide').setAttribute('aria-hidden', 'false');
  $('hardwareGuideBackdrop').setAttribute('aria-hidden', 'false');
  document.querySelector('.shell').inert = true;
  document.body.classList.add('panel-open');
  $('hardwareGuideClose').focus();
}

function closeHardwareGuide() {
  $('hardwareGuide').classList.remove('show');
  $('hardwareGuideBackdrop').classList.remove('show');
  $('hardwareGuide').setAttribute('aria-hidden', 'true');
  $('hardwareGuideBackdrop').setAttribute('aria-hidden', 'true');
  document.querySelector('.shell').inert = false;
  document.body.classList.remove('panel-open');
  app.guideReturnFocus?.focus?.();
}

function trapFocus(container, event) {
  const focusable = [...container.querySelectorAll('button:not(:disabled), input:not(:disabled), select:not(:disabled), textarea:not(:disabled), summary, [href], [tabindex]:not([tabindex="-1"])')]
    .filter((element) => element.offsetParent !== null);
  if (!focusable.length) return;
  // Safari can omit buttons from native Tab navigation. Traverse the actual
  // visible controls explicitly so every dialog action remains reachable.
  const current = focusable.indexOf(document.activeElement);
  const next = current < 0 ? (event.shiftKey ? focusable.length - 1 : 0)
    : (current + (event.shiftKey ? -1 : 1) + focusable.length) % focusable.length;
  event.preventDefault();
  focusable[next].focus();
}

function handlePanelKeydown(event) {
  if ($('mgmtConfirm').classList.contains('show')) {
    if (event.key === 'Escape') { event.preventDefault(); app.managementConfirmationCancel?.(); }
    else if (event.key === 'Tab') trapFocus($('mgmtConfirm'), event);
    else if (event.key === 'Enter' && !event.isComposing && document.activeElement === $('mgmtConfirmName')) {
      event.preventDefault(); $('mgmtConfirmAccept').click();
    }
    return;
  }
  if (event.key === 'Escape' && $('hardwareGuide').classList.contains('show')) closeHardwareGuide();
  else if (event.key === 'Escape' && $('settingsPanel').classList.contains('show')) closeSettings();
  else if (event.key === 'Tab' && $('hardwareGuide').classList.contains('show')) trapFocus($('hardwareGuide'), event);
  else if (event.key === 'Tab' && $('settingsPanel').classList.contains('show')) trapFocus($('settingsPanel'), event);
}

function collectLogText() {
  return [...$('logList').children].reverse().map((item) => item.textContent.trim().replace(/\s+/g, ' ')).join('\n');
}

async function registerOfflineApp() {
  if (!('serviceWorker' in navigator)) {
    setText('offlineStatus', t('Not supported'));
    return;
  }
  try {
    const registration = await navigator.serviceWorker.register('./service-worker.js', { scope: './', updateViaCache: 'none' });
    const updateLabel = () => setText('offlineStatus', t(registration.waiting
      ? 'Update ready. Close all app windows and reopen.'
      : registration.active ? 'Ready offline'
        : registration.installing && registration.installing.state !== 'redundant'
          ? 'Preparing cache…' : 'Cache unavailable'));
    registration.addEventListener('updatefound', () => {
      registration.installing?.addEventListener('statechange', updateLabel);
    });
    await navigator.serviceWorker.ready;
    updateLabel();
  } catch (error) {
    setText('offlineStatus', t('Cache unavailable'));
    log('err', `Offline app setup failed: ${error.message}`);
  }
}

function wireInterface() {
  loadTheme();
  setDeveloperMode(localStorage.getItem(DEV_MODE_KEY) === '1');
  setText('browserSupport', t(navigator.bluetooth ? 'Web Bluetooth ready' : 'Web Bluetooth unavailable'));
  $('webBluetoothWarning').classList.toggle('hidden', Boolean(navigator.bluetooth));

  $('themeButton').addEventListener('click', toggleTheme);
  $('languageButton').addEventListener('click', () => window.SmartTimerI18n?.toggleLanguage?.());
  window.addEventListener('smarttimer:languagechange', () => {
    if (app.managementWifiStatus) renderWifi(app.managementWifiStatus, false);
    setConnection(...lastConnection);
    updateNotice?.refresh();
    applyTheme(document.documentElement.dataset.theme);
    document.title = `${app.protocolInfo?.device_name || t('Smart Timer')} · ${t('Bluetooth control')}`;
    renderRecentActivity();
  });
  ['settingsOpen', 'footerSettings', 'connectionShortcut'].forEach((id) => {
    $(id).addEventListener('click', (event) => openSettings(event.currentTarget));
  });
  $('settingsClose').addEventListener('click', () => closeSettings());
  $('settingsBackdrop').addEventListener('click', () => closeSettings());
  $('hardwareGuideOpen').addEventListener('click', (event) => openHardwareGuide(event.currentTarget));
  $('hardwareGuideClose').addEventListener('click', closeHardwareGuide);
  $('hardwareGuideBackdrop').addEventListener('click', closeHardwareGuide);
  document.addEventListener('keydown', handlePanelKeydown);

  $('developerModeToggle').addEventListener('change', (event) => {
    const enabled = event.currentTarget.checked;
    localStorage.setItem(DEV_MODE_KEY, enabled ? '1' : '0');
    setDeveloperMode(enabled);
    log('info', `Developer view ${enabled ? 'enabled' : 'disabled'}.`);
  });

  $('scanButton').addEventListener('click', connectTimer);
  $('connectionRetry').addEventListener('click', retryConnection);
  $('settingsConnectButton').addEventListener('click', () => { closeSettings(false); connectTimer(); });
  $('disconnectButton').addEventListener('click', disconnectTimer);
  $('settingsDisconnectButton').addEventListener('click', disconnectTimer);
  $('settingsIdentifyButton').addEventListener('click', () => dispatchCommand('identify'));
  wireTimerSettings();
  $('pairButton').addEventListener('click', finishPairing);

  $('startForm').addEventListener('submit', (event) => {
    event.preventDefault();
    if ($('randomMode').checked) {
      normalizeRandomDuration('randomMax');
      const minimum = Number($('randomMin').value) * 60000;
      const maximum = Number($('randomMax').value) * 60000;
      dispatchCommand('start', { valueA: minimum, valueB: maximum });
    } else {
      const duration = fixedDurationSeconds() * 1000;
      if (duration < 1000 || duration > 86400000) {
        showFeedback('error', 'Invalid duration', 'Choose a duration between 1 second and 24 hours.', 0);
        return;
      }
      dispatchCommand('start', { valueA: duration, valueB: 0 });
    }
  });
  $('pauseButton').addEventListener('click', () => dispatchCommand(app.live?.state === 'paused' ? 'resume' : 'pause'));
  $('stopButton').addEventListener('click', () => dispatchCommand('release'));
  $('sealButton').addEventListener('click', requestControlLock);
  document.querySelectorAll('[data-adjust]').forEach((button) => {
    button.addEventListener('click', (event) => {
      dispatchCommand('adjust', { valueA: Number(event.currentTarget.dataset.adjust), valueB: 0 });
      if (event.detail !== 0) event.currentTarget.blur();
    });
  });

  $('randomMode').addEventListener('change', syncDurationMode);
  ['hours', 'minutes', 'seconds'].forEach((id) => $(id).addEventListener('input', syncDurationMode));
  document.querySelectorAll('[data-preset]').forEach((button) => {
    button.addEventListener('click', (event) => {
      setFixedMinutes(Number(event.currentTarget.dataset.preset));
      if (event.detail !== 0) event.currentTarget.blur();
    });
  });
  $('resetDuration').addEventListener('click', (event) => {
    setFixedMinutes(15);
    if (event.detail !== 0) event.currentTarget.blur();
  });

  ['hours', 'minutes', 'seconds'].forEach((fieldId) => wireDurationScrubber(
    fieldId,
    () => Boolean(app.authenticated && app.live?.state === 'open' && !$('randomMode').checked),
    (steps) => adjustFixedDuration(fieldId, steps),
    () => setFixedDurationSeconds(fixedDurationSeconds()),
  ));
  ['randomMin', 'randomMax'].forEach((fieldId) => wireDurationScrubber(
    fieldId,
    () => Boolean(app.authenticated && app.live?.state === 'open' && $('randomMode').checked),
    (steps) => adjustRandomDuration(fieldId, steps),
    () => normalizeRandomDuration(fieldId),
  ));

  $('saveKeyButton').addEventListener('click', async () => {
    const key = parseHex($('appKeyInput').value);
    if (!key || key.length !== 16) {
      showFeedback('error', 'Invalid controller key', 'Enter exactly 32 hexadecimal characters.', 0);
      return;
    }
    try {
      await credentialWrite('active', key);
      await credentialDelete('pending');
      app.activeKey = key;
      app.pendingKey = null;
      localStorage.removeItem(DEVICE_ID_KEY);
      updateCredentialUi();
      showToast('Controller key saved', 'Reconnect to authenticate with this key.');
    } catch (error) {
      showFeedback('error', 'Controller key was not saved', error.message, 0);
    }
  });
  $('showKeyButton').addEventListener('click', (event) => {
    const show = $('appKeyInput').type === 'password';
    $('appKeyInput').type = show ? 'text' : 'password';
    event.currentTarget.setAttribute('aria-pressed', String(show));
    event.currentTarget.textContent = t(show ? 'Hide' : 'Show');
  });
  $('forgetKeyButton').addEventListener('click', async () => {
    try {
      await credentialDelete('active');
      await credentialDelete('pending');
      if (app.credentialLegacy) {
        await credentialDelete('active', null);
        await credentialDelete('pending', null);
      }
      if (app.credentialScope) storeSavedTimers(savedTimers().filter((timer) => timer.id !== app.credentialScope));
      app.activeKey = null;
      app.pendingKey = null;
      app.credentialLegacy = false;
      localStorage.removeItem(DEVICE_ID_KEY);
      updateCredentialUi();
      disconnectTimer();
      showToast('Controller forgotten', 'The OS bond and timer approval were kept. Close other timer apps, reopen physical pairing, connect this browser, and briefly press Reset to approve it again.');
    } catch (error) {
      showFeedback('error', 'Controller was not forgotten', error.message, 0);
    }
  });

  $('clearLogButton').addEventListener('click', () => $('logList').replaceChildren());
  $('copySupportButton').addEventListener('click', async () => {
    const report = {
      captured_at: new Date().toISOString(),
      identity: app.protocolInfo,
      timer: app.live,
      system: app.system,
      recent_activity: app.events,
      browser: {
        online: navigator.onLine,
        bluetooth_available: Boolean(navigator.bluetooth),
        controller_key_stored: Boolean(app.activeKey),
        pending_pairing_recovery: Boolean(app.pendingKey),
      },
    };
    try {
      await navigator.clipboard.writeText(JSON.stringify(report, null, 2));
      setText('supportCopyStatus', t('Support diagnostics copied. Controller keys are not included.'));
    } catch (error) {
      setText('supportCopyStatus', t('Could not copy diagnostics: {error}', { error: error.message }));
    }
  });
  $('copyLogButton').addEventListener('click', async () => {
    const text = collectLogText();
    if (!text) return;
    try {
      await navigator.clipboard.writeText(text);
      showToast('Log copied', `${text.split('\n').length} lines copied.`);
    } catch (error) {
      showFeedback('error', 'Log was not copied', error.message, 0);
    }
  });
  $('downloadLogButton').addEventListener('click', () => {
    const text = collectLogText();
    if (!text) return;
    const stamp = new Date().toISOString().replace(/[:.]/g, '-');
    const name = (app.device?.name || 'unknown').replace(/[^A-Za-z0-9._-]/g, '_');
    const url = URL.createObjectURL(new Blob([`${text}\n`], { type: 'text/plain' }));
    const link = document.createElement('a');
    link.href = url;
    link.download = `ost-ble-log-${name}-${stamp}.txt`;
    document.body.append(link);
    link.click();
    link.remove();
    URL.revokeObjectURL(url);
  });

  window.addEventListener('beforeunload', () => {
    if (app.server?.connected) app.server.disconnect();
  });
  document.addEventListener('visibilitychange', handlePageVisibility);
  window.addEventListener('pageshow', handlePageVisibility);
}

async function initialize() {
  wireInterface();
  await loadCredentials();
  setConnection('offline', 'Disconnected');
  updateNotice?.status({ state: 'disconnected' });
  renderNotReady();
  updateConnectionCard();
  syncDurationMode();
  registerOfflineApp();
  setInterval(refreshSystemInfo, 5000);
  setInterval(maintainLiveStatus, 1000);
  requestAnimationFrame(renderClock);
  log('info', 'Smart Timer Bluetooth app ready.');
}

initialize().catch((error) => {
  log('err', `App initialization failed: ${error.message}`);
  showFeedback('error', 'App initialization failed', error.message, 0);
});
