// Trusted partner page, hosted independently of the untrusted relay.
'use strict';

const $ = (id) => document.getElementById(id);
const TAB_STORAGE_KEY = 'ost-relay-tab-v1';
const STATUS_FRESH_MS = 15000;
const INTENT_WINDOW_MS = 5000;
const SESSION_HEADER = 'X-OST-Session';
const t = (key, values = {}) => window.SmartTimerI18n ? window.SmartTimerI18n.text(key, values)
  : Object.entries(values).reduce((text, [name, value]) => text.replaceAll(`{${name}}`, String(value)), key);
const state = {
  scopes: new Set(), bootId: '', sessionHandle: '', lastSeenUpstreamSeq: 0,
  tokenExpUpMs: Infinity, status: null,
  lastStatusUptimeMs: 0, lastStatusWallMs: 0, lastVerifiedWallMs: 0,
  eventSource: null, awaitingCommand: null, pendingIntents: [], attemptsById: {},
  events: [], expiryTimer: 0, expired: false, fresh: false, closed: false, disconnected: false,
  bindingCheck: null, lastBindingCheckMs: 0, setupEnabled: false,
  watchRetryTimer: 0, watchRetryDelayMs: 3000,
  frameQueue: Promise.resolve(),
  outboundQueue: Promise.resolve(), token: '', relayOrigin: '', routeHash: '', channel: null,
  syncRequests: new Map(), syncTimer: 0, lastAcceptedSyncRequest: '',
  syncPromise: null, preparingCommand: false,
  channelCreatedWallMs: 0, nextChannelRecoveryMs: 0, channelRecoveryDelayMs: STATUS_FRESH_MS,
  reconcileTimer: 0, reconcileDelayMs: 1000, reconcilePromise: null,
};

// The capability stays on this trusted origin. Traffic keys never persist.
function saveTabState() {
  if (state.disconnected) return;
  sessionStorage.setItem(TAB_STORAGE_KEY, JSON.stringify({
    sessionHandle: state.sessionHandle,
    bootId: state.bootId,
    lastSeenUpstreamSeq: state.lastSeenUpstreamSeq,
    pendingIntents: state.pendingIntents,
    attemptsById: state.attemptsById,
    token: state.token, relayOrigin: state.relayOrigin, routeHash: state.routeHash,
  }));
}
function loadTabState() {
  try {
    const saved = JSON.parse(sessionStorage.getItem(TAB_STORAGE_KEY));
    if (!saved || typeof saved.sessionHandle !== 'string' ||
        typeof saved.bootId !== 'string') return null;
    return saved;
  } catch (_) { return null; }
}

async function main() {
  setupPresentation();
  let token = null;
  try {
    const link = readTokenFromFragmentAndClear();
    if (link) {
      token = link.token;
      state.token = token;
      state.relayOrigin = link.relayOrigin;
      state.routeHash = link.routeHash;
      state.pendingIntents = [];
      state.attemptsById = {};
      state.lastSeenUpstreamSeq = 0;
    } else {
      const saved = loadTabState();
      if (!saved) return showEnd('No share link', 'Ask the owner for a fresh link.');
      Object.assign(state, saved);
      // Reloaded in-flight requests have an unknown outcome. Never replay them.
      state.pendingIntents = Array.isArray(saved.pendingIntents) ? saved.pendingIntents : [];
      state.attemptsById = saved.attemptsById && typeof saved.attemptsById === 'object' ? saved.attemptsById : {};
      state.lastSeenUpstreamSeq = Number.isSafeInteger(saved.lastSeenUpstreamSeq) ? saved.lastSeenUpstreamSeq : 0;
    }
    const payload = tokenPayloadJson(state.token);
    if (!payload || typeof payload.boot !== 'string' || typeof payload.vk !== 'string' ||
        !Array.isArray(payload.sc) || !payload.sc.includes('view') || !Number.isSafeInteger(payload.exp_up_ms)) throw makeError('invalid_or_expired',401);
    state.bootId = payload.boot;
    state.scopes = new Set(payload.sc);
    state.tokenExpUpMs = payload.exp_up_ms;
    // Release this tab's old routing admission before a fresh ECDH exchange.
    await releaseRoutingSession(state.sessionHandle);
    const info = await createSession(state.token);
    if (state.disconnected) {
      state.channel.close(); state.channel = null;
      await releaseRoutingSession(info.session_handle);
      return;
    }
    state.sessionHandle = info.session_handle;
    token = null;
    saveTabState();
    $('timerLabel').textContent = 'Smart Timer';
    $('scopeSummary').textContent = t('Partner link');
    revealScopedButtons();
    switchToLive();
    markStale('Waiting for verified timer status');
    renderPendingIntents();
    openWatch();
    startExpiryCountdown();
  } catch (err) {
    token = null;
    handleSessionError(err);
  }
}

function decodeBase64url(value, expectedLength) {
  if (typeof value !== 'string' || !/^[A-Za-z0-9_-]+$/.test(value)) throw new Error('bad_base64url');
  const bin = atob(value.replace(/-/g, '+').replace(/_/g, '/') + '='.repeat((4 - value.length % 4) % 4));
  const bytes = Uint8Array.from(bin, (c) => c.charCodeAt(0));
  if (expectedLength !== undefined && bytes.length !== expectedLength) throw new Error('bad_encoding');
  return bytes;
}
function tokenPayloadJson(wire) {
  if (typeof wire !== 'string') return null;
  const parts = wire.split('.');
  if (parts.length !== 4 || parts[0] !== 'ost-cap' || parts[1] !== 'v1') return null;
  try { return JSON.parse(new TextDecoder('utf-8', { fatal: true }).decode(decodeBase64url(parts[2]))); }
  catch (_) { return null; }
}
// Only queueFrame calls this after AEAD verification by an identity-authenticated
// channel. Parsing never grants authority to an unencrypted relay response.
function parseResponseFrame(envelope, expectedType) {
  try {
    if (!envelope || envelope.type !== expectedType ||
        !['status', 'event', 'result'].includes(expectedType) ||
        Object.keys(envelope).sort().join(',') !== 'body_json,type,upstream_seq' ||
        !Number.isSafeInteger(envelope.upstream_seq) || envelope.upstream_seq <= 0 ||
        typeof envelope.body_json !== 'string') return null;
    const body = JSON.parse(envelope.body_json);
    return body && typeof body === 'object' && !Array.isArray(body) ? body : null;
  } catch (_) { return null; }
}
function validSnapshot(snap) {
  return snap && typeof snap === 'object' && snap.boot_id === state.bootId &&
    ['open', 'running', 'paused'].includes(snap.state) &&
    Number.isSafeInteger(snap.uptime_ms) && snap.uptime_ms >= 0 &&
    Number.isSafeInteger(snap.remaining_ms) && snap.remaining_ms >= 0 &&
    Number.isSafeInteger(snap.session_duration_ms) && snap.session_duration_ms >= 0 &&
    typeof snap.sealed === 'boolean' && typeof snap.emergency_input_active === 'boolean' &&
    (snap.session_id === null || typeof snap.session_id === 'string');
}
function queueFrame(envelope, type, options = {}) {
  const channel = options.channel || state.channel;
  const work = state.frameQueue.then(async () => {
    if (!channel || channel !== state.channel || state.closed || state.expired) return false;
    const inner = await channel.decrypt(envelope);
    if (!inner || channel !== state.channel || state.closed || state.expired) return false;
    let syncRequest = '';
    if (inner.type === 'status') {
      // A new unpredictable challenge proves receipt within this time window.
      // Captured broadcasts or delayed status cannot enable controls.
      const sent = state.syncRequests.get(envelope.request_id);
      state.syncRequests.delete(envelope.request_id);
      if (sent === undefined || performance.now()-sent > INTENT_WINDOW_MS) return false;
      syncRequest = envelope.request_id;
    }
    if (type && inner.type !== type) return false;
    const accepted = await acceptFrame(inner,inner.type,options);
    // HTTP and SSE can carry the same response. Record success only after
    // nonce, timing, AEAD, sequence and snapshot verification all pass.
    if (accepted && syncRequest && !options.cached) state.lastAcceptedSyncRequest = syncRequest;
    return accepted;
  });
  state.frameQueue = work.catch(() => false);
  return work;
}
async function acceptFrame(envelope, type, { cached = false, intent = null } = {}) {
  const body = parseResponseFrame(envelope, type);
  if (!body) return false;
  const freshSequence = envelope.upstream_seq > state.lastSeenUpstreamSeq;
  if (type === 'result') {
    const pending = intent || state.pendingIntents.find((entry) => entry.op_id === body.op_id);
    if (!pending || !state.pendingIntents.some((entry) => entry.op_id === pending.op_id) ||
        body.op_id !== pending.op_id || body.op !== pending.op ||
        typeof body.code !== 'string' || !body.code || !validSnapshot(body.status) ||
        body.status.boot_id !== pending.if_boot_id ||
        !body.mutation || body.mutation.action !== pending.op ||
        body.mutation.operation_id !== pending.op_id ||
        typeof body.mutation.changed !== 'boolean' || typeof body.mutation.duplicate !== 'boolean' ||
        (body.code !== 'applied' && body.mutation.changed)) return false;
    // A retry can be rejected after the original action was applied (e.g. its
    // deadline passed in transit). That is not proof the original failed.
    if ((state.attemptsById[pending.op_id] || 0) > 1 && body.code !== 'applied' &&
        !body.mutation.duplicate) return false;
    // A cached result can arrive after newer status. It resolves only its exact
    // unresolved intent, once, and never replaces newer live state.
    state.pendingIntents = state.pendingIntents.filter((entry) => entry.op_id !== pending.op_id);
    delete state.attemptsById[pending.op_id];
    if (freshSequence) {
      state.lastSeenUpstreamSeq = envelope.upstream_seq;
    }
    // Results describe the outcome at execution time. Only a status heartbeat
    // may refresh the live dial; dedup replay can include an older snapshot.
    saveTabState();
    renderPendingIntents();
    showResult(body.code === 'applied', body.code === 'applied'
      ? body.mutation.changed ? '{action} · applied' : '{action} · applied (no state change)'
      : '{action} was not applied: {reason}', { action: labelForOp(pending.op), reason: body.code });
    updateButtonsForState(state.status);
    syncStatus();
    return true;
  }
  if (!freshSequence) return false;
  if (type === 'status') {
    if (!validSnapshot(body) || (state.status && body.uptime_ms < state.status.uptime_ms)) return false;
    state.lastSeenUpstreamSeq = envelope.upstream_seq;
    applyVerifiedStatus(body, cached);
  } else if (type === 'event') {
    if (body.boot_id !== state.bootId || typeof body.type !== 'string') return false;
    state.lastSeenUpstreamSeq = envelope.upstream_seq;
    appendEvent(body);
  } else return false;
  saveTabState();
  return true;
}
function applyVerifiedStatus(snap, cached = false) {
  const priorUptime = estimatedTimerUptime();
  state.status = snap;
  // A cached primer must not move the estimated clock backward and reopen a
  // command's deadline or extend the link countdown.
  state.lastStatusUptimeMs = Math.max(priorUptime, snap.uptime_ms);
  state.lastStatusWallMs = Date.now();
  state.lastVerifiedWallMs = Date.now();
  state.fresh = !cached;
  // Owner Extend: a later link expiry in a live, verified status (never a
  // cached primer) moves the countdown and refreshes this browser session.
  const linkExpiry = snap.share_link_expires_up_ms;
  if (!cached && Number.isSafeInteger(linkExpiry) && Number.isFinite(state.tokenExpUpMs) &&
      linkExpiry > state.tokenExpUpMs) {
    state.tokenExpUpMs = linkExpiry;
    resumeSession().catch(() => {});
  }
  applyStatusToDial(snap);
  if (cached) markStale('Last verified snapshot · waiting for live status');
  else {
    state.watchRetryDelayMs = 3000;
    state.nextChannelRecoveryMs = 0;
    state.channelRecoveryDelayMs = STATUS_FRESH_MS;
    setConn('online');
    $('connectionWarning').classList.add('hidden');
  }
  updateButtonsForState(snap);
  // The first authenticated uptime can differ from the initial display estimate.
  renderExpiryCountdown();
}
// Matches the local and Bluetooth apps: grey "Offline" after a lost
// connection, amber "Last known" while waiting for current status.
function markStale(message = 'Last verified snapshot · connection interrupted', lost = false) {
  state.fresh = false;
  setConn(lost ? 'offline' : 'reconnecting');
  $('statusLine').textContent = t(message);
  $('stateBadge').textContent = t(lost ? 'Offline' : 'Last known');
  $('stateBadge').className = 'badge ' + (lost ? 'offline' : 'stale');
  if (lost) showConnectionWarning(message);
  $('dialKicker').textContent = t(state.status ? 'Last verified snapshot' : 'Waiting for status');
  $('dialSubtext').textContent = t(message);
  $('timerDial').classList.remove('active');
  updateButtonsForState(state.status);
}
function readTokenFromFragmentAndClear() {
  const fragment = (window.location.hash || '').replace(/^#/, '');
  // Failure to clear is fatal: never send a token while its URL remains visible.
  window.history.replaceState(null, '', window.location.pathname + window.location.search);
  if (!fragment) return null;
  const match = /^\/s\/([A-Za-z0-9_-]{22})\/?$/.exec(window.location.pathname);
  if (match) {
    const params = new URLSearchParams(fragment);
    if (window.location.search || params.getAll('relay').length !== 1 || params.getAll('cap').length !== 1
        || [...params.keys()].some(key => key !== 'relay' && key !== 'cap')) throw Error('invalid_share_link');
    const token = params.get('cap');
    if (!token || token.length > 520 || !token.startsWith('ost-cap.v1.')) throw Error('invalid_share_link');
    return {token, relayOrigin: decodeRelayOrigin(params.get('relay')), routeHash: match[1]};
  }
  // Secure pre-public links remain usable until their device grant expires.
  if (!/^\/s\/?$/.test(window.location.pathname) || fragment.length > 520
      || !fragment.startsWith('ost-cap.v1.')) throw Error('invalid_share_link');
  return {token:fragment, relayOrigin:relayOriginFromLocation(), routeHash:''};
}
async function createSession(token) {
  const opened = await OSTSecure.open(token);
  if (state.routeHash && state.routeHash !== opened.frame.fp_hash) throw makeError('invalid_or_expired',401);
  const controller = new AbortController(), deadline = setTimeout(() => controller.abort(),10000);
  try {
    const res = await fetch(api('/api/v1/session'), {
      method: 'POST', credentials: 'omit', signal: controller.signal,
      headers: { 'Content-Type': 'application/json' }, body: JSON.stringify(opened.frame),
    });
    if (res.status === 401 || res.status === 403) throw makeError('invalid_or_expired', res.status);
    if (res.status === 503) throw makeError('timer_not_connected', 503);
    if (!res.ok) throw makeError('session_failed', res.status);
    const info = await res.json();
    let channel;
    try {
      channel = await opened.accept(info.frame);
      if (info.boot_id !== state.bootId || info.boot_id !== opened.payload.boot || info.timer_id !== opened.payload.tid ||
          typeof info.session_handle !== 'string' || !/^[A-Za-z0-9_-]{22}$/.test(info.session_handle)) {
        throw makeError('session_mismatch',409);
      }
    } catch (error) {
      if (channel) channel.close();
      await releaseRoutingSession(info?.session_handle);
      throw error;
    }
    if (state.channel && state.channel !== channel) state.channel.close();
    state.channel = channel;
    state.channelCreatedWallMs = Date.now();
    return info;
  } finally { clearTimeout(deadline); }
}
async function releaseRoutingSession(handle) {
  if (typeof handle !== 'string' || !/^[A-Za-z0-9_-]{22}$/.test(handle)) return;
  const controller = new AbortController(), deadline = setTimeout(() => controller.abort(),5000);
  try {
    await fetch(api('/api/v1/session'), {method:'DELETE',credentials:'omit',
      headers:{[SESSION_HEADER]:handle},signal:controller.signal});
  } catch (_) { /* Routing admission also expires on the relay. */ }
  finally { clearTimeout(deadline); }
}
function relayOriginFromLocation() {
  const encoded = new URLSearchParams(window.location.search).get('relay');
  return decodeRelayOrigin(encoded);
}
function decodeRelayOrigin(encoded) {
  const value = new TextDecoder('utf-8',{fatal:true}).decode(OSTSecure.dec(encoded));
  const url = new URL(value);
  if (url.protocol !== 'https:' || url.origin !== value || url.origin === location.origin) throw Error('invalid_relay_origin');
  return value;
}
function api(path) {
  const url = new URL(state.relayOrigin);
  if (url.protocol !== 'https:' || url.origin !== state.relayOrigin || url.origin === location.origin) throw Error('invalid_relay_origin');
  return url.origin + path;
}
function secureSend(body, request, signal) {
  const channel = state.channel, handle = state.sessionHandle;
  // Preserve sequence order through asynchronous crypto and HTTP delivery.
  const work = state.outboundQueue.then(async () => {
    if (!channel || channel !== state.channel || handle !== state.sessionHandle || state.closed || state.expired || signal?.aborted) throw Error('channel_closed');
    const frame = await channel.encrypt({...body,cap_token:state.token},request);
    if (channel !== state.channel || handle !== state.sessionHandle || state.closed || state.expired || signal?.aborted) throw Error('channel_closed');
    return fetch(api('/api/v1/command'), {method:'POST',credentials:'omit',
      headers:{'Content-Type':'application/json',[SESSION_HEADER]:handle},
      body:JSON.stringify(frame),signal});
  });
  state.outboundQueue = work.catch(() => {});
  return work;
}
async function syncStatus() {
  if (state.closed || state.expired) return false;
  if (state.syncPromise) return state.syncPromise;
  state.syncPromise = syncAndRecoverStatus();
  try { return await state.syncPromise; }
  finally { state.syncPromise = null; }
}
async function syncAndRecoverStatus() {
  if (await performStatusSync()) return true;
  if (state.closed || state.expired || state.awaitingCommand ||
      Date.now()-Math.max(state.lastVerifiedWallMs,state.channelCreatedWallMs) < STATUS_FRESH_MS) return false;
  markStale('Timer status is stale · last verified snapshot',true);
  if (Date.now() < state.nextChannelRecoveryMs) return false;
  state.nextChannelRecoveryMs = Date.now()+state.channelRecoveryDelayMs;
  // Capped at 30 s: after Bluetooth pauses the relay, or the relay restarts,
  // longer waits left a returning timer looking offline for up to 2 minutes.
  state.channelRecoveryDelayMs = Math.min(state.channelRecoveryDelayMs*2,30000);
  // The timer erases idle traffic keys after two minutes; restarting SSE alone
  // cannot restore them. Replace this tab's channel, retaining its journal and
  // sequence watermark. Only a new nonce-bound status can restore controls.
  closeWatch();
  const oldHandle = state.sessionHandle;
  if (state.channel) state.channel.close();
  state.channel = null;
  state.sessionHandle = '';
  state.syncRequests.clear();
  try {
    await releaseRoutingSession(oldHandle);
    if (state.closed || state.expired) return false;
    const info = await createSession(state.token);
    if (state.closed || state.expired) {
      state.channel.close(); state.channel = null;
      await releaseRoutingSession(info.session_handle);
      return false;
    }
    state.sessionHandle = info.session_handle;
    saveTabState();
    return await performStatusSync();
  } catch (error) {
    const code = error.code || error.message;
    if (['invalid_or_expired','session_mismatch','verification_unavailable'].includes(code)) handleSessionError(makeError(code,error.httpStatus));
    return false;
  } finally {
    // Polling continues even when a temporary outage prevents a new channel.
    if (!state.closed && !state.expired) openWatch();
  }
}
async function performStatusSync() {
  const channel = state.channel;
  if (!channel || !state.sessionHandle) return false;
  const request = generateOpId();
  // Bound the map even while a malicious relay drops every reply.
  for (const [id,sent] of state.syncRequests) if (performance.now()-sent > INTENT_WINDOW_MS) state.syncRequests.delete(id);
  state.syncRequests.set(request,performance.now());
  const abort = new AbortController(), deadline = setTimeout(() => abort.abort(),INTENT_WINDOW_MS);
  try {
    const response = await secureSend({type:'sync'},request,abort.signal);
    if (response.ok) {
      await queueFrame(await response.json(),'status',{channel});
      return state.lastAcceptedSyncRequest === request;
    }
  } catch (_) { /* An unavailable relay leaves controls stale. */ }
  finally { clearTimeout(deadline); state.syncRequests.delete(request); }
  return false;
}
async function resumeSession() {
  const res = await fetch(api('/api/v1/session'), {
    credentials: 'omit', headers: { [SESSION_HEADER]: state.sessionHandle },
  });
  if (res.status === 409) throw makeError('session_mismatch', 409);
  if (!res.ok) throw makeError('invalid_or_expired', res.status);
  const info = await res.json();
  if (info.session_handle !== state.sessionHandle) throw makeError('session_mismatch', 409);
  return info;
}

function handleSessionError(err) {
  if (state.disconnected) return;
  const code = err && err.code;
  if (code === 'verification_unavailable') {
    return showEnd('Browser verification unavailable',
      'Use an HTTPS connection in a current browser. No timer data or commands were accepted.');
  }
  if (code === 'session_mismatch') {
    return showEnd('Another share link is active',
      'This tab cannot use the other link’s cookie. Reopen this timer’s original share link in a separate browser profile.');
  }
  if (code === 'invalid_or_expired') {
    if (state.sessionHandle) return showLinkExpired();
    return showEnd('Share link expired',
      'This link is no longer valid. Ask the owner for a new one.');
  }
  if (code === 'timer_not_connected') {
    return showEnd('Timer offline',
      'The timer is not currently reachable through the relay. Try again once the owner confirms it is online.');
  }
  return showEnd('Something went wrong',
    'The relay could not verify this link. Ask the owner for a new one if the problem persists.');
}

// An expired link can be extended by the owner within the same timer boot.
// Check back quietly and reopen this page if that happens.
function showLinkExpired() {
  if (state.disconnected) return;
  showEnd('Share link expired',
    'If the owner extends this link, this page reopens by itself. Otherwise ask for a new link.');
  if (!state.sessionHandle || state.reactivationTimer) return;
  state.reactivationTimer = setInterval(async () => {
    if (document.visibilityState !== 'visible' || state.reactivationChecking) return;
    state.reactivationChecking = true;
    try {
      // Relay metadata cannot prove that the timer reactivated a grant.
      const info = await createSession(state.token);
      if (state.disconnected) {
        state.channel.close(); state.channel = null;
        await releaseRoutingSession(info.session_handle);
        return;
      }
      state.sessionHandle = info.session_handle;
      state.closed = state.expired = false;
      markStale('Waiting for verified timer status');
      // The old deadline has already elapsed. Get the timer's authenticated
      // new deadline before restarting the countdown or revealing controls.
      await syncStatus();
      if (!state.fresh || state.tokenExpUpMs <= estimatedTimerUptime()) {
        state.closed = state.expired = true;
        return;
      }
      saveTabState();
      clearInterval(state.reactivationTimer);
      state.reactivationTimer = 0;
      $('endView').classList.add('hidden');
      switchToLive();
      openWatch();
      startExpiryCountdown();
    } catch (_) { state.closed = state.expired = true; }
    finally { state.reactivationChecking = false; }
  }, 15000);
}

function makeError(code, httpStatus) {
  const e = new Error(code);
  e.code = code;
  e.httpStatus = httpStatus;
  return e;
}

// ---------------------------------------------------------------------------
// UI transitions
// ---------------------------------------------------------------------------

function showConnectionWarning(message) {
  const verified = state.lastVerifiedWallMs ? Math.round((Date.now() - state.lastVerifiedWallMs) / 1000) : null;
  $('connectionWarningTitle').textContent = t(message.startsWith('Timer offline') ? 'Timer offline' : 'Connection lost');
  $('connectionWarningMessage').textContent = verified === null
    ? t('No verified timer status yet. The timer keeps running on its own; use the physical emergency release if needed.')
    : t('Last verified status {seconds} s ago. The timer keeps running on its own; use the physical emergency release if needed.', { seconds: verified });
  $('connectionWarning').classList.remove('hidden');
}

function switchToLive() {
  $('loadingView').classList.add('hidden');
  $('liveView').classList.remove('hidden');
  $('timerStatusStrip').classList.remove('hidden');
  $('sessionFooter').classList.remove('hidden');
  $('disconnectControls').classList.remove('hidden');
  setConn('connecting');
}

function showEnd(title, message) {
  state.closed = true;
  closeLockConfirm(false);
  updateButtonsForState(state.status);
  $('loadingView').classList.add('hidden');
  $('liveView').classList.add('hidden');
  $('timerStatusStrip').classList.add('hidden');
  $('sessionFooter').classList.add('hidden');
  $('endView').classList.remove('hidden');
  $('endTitle').textContent = t(title);
  $('endMessage').textContent = t(message);
  setConn('offline');
  $('connectionWarning').classList.add('hidden');
  closeWatch();
  clearInterval(state.expiryTimer);
}

async function disconnectSession() {
  if (state.disconnected) return;
  const handle = state.sessionHandle;
  state.disconnected = true;
  state.fresh = false;
  showEnd('Disconnected', "This browser's control session is closed. The timer keeps running. Reopen the original share link to connect again.");
  clearInterval(state.reactivationTimer);
  state.reactivationTimer = 0;
  clearTimeout(state.reconcileTimer);
  state.reconcileTimer = 0;
  if (state.channel) state.channel.close();
  state.channel = null;
  state.sessionHandle = '';
  state.token = '';
  state.syncRequests.clear();
  state.pendingIntents = [];
  state.attemptsById = {};
  try { sessionStorage.removeItem(TAB_STORAGE_KEY); } catch (_) { /* Optional tab storage. */ }
  $('disconnectControls').classList.add('hidden');
  $('settingsMenu').open = false;
  $('settingsOpen').focus();
  // Only this tab's routing admission ends. No timer action or grant revocation.
  await releaseRoutingSession(handle);
}

function setConn(label) {
  const pill = $('connectionState');
  pill.textContent = t(label);
  pill.className = 'conn-pill ' + label;
}

// ---------------------------------------------------------------------------
// Scope-gated controls
// ---------------------------------------------------------------------------

function revealScopedButtons() {
  const buttons = document.querySelectorAll('button[data-op]');
  buttons.forEach((btn) => {
    const required = btn.dataset.scope;
    if (state.scopes.has(required)) {
      btn.classList.remove('hidden');
    }
  });
  buttons.forEach((btn) => btn.addEventListener('click', () => onButton(btn)));
}

function summarizeScopes(scopes) {
  const nice = {
    view: 'watch',
    pause_resume: 'pause & resume',
    adjust: 'adjust time',
    release: 'release',
    start: 'start',
    lock: 'lock controls',
    identify: 'identify',
  };
  const parts = scopes
    .filter((s) => s !== 'view')
    .map((s) => t(nice[s] || s));
  if (parts.length === 0) return t('View only');
  return capitalize(parts.join(' · '));
}

function capitalize(s) {
  return s.charAt(0).toUpperCase() + s.slice(1);
}

// ---------------------------------------------------------------------------
// Live status stream
// ---------------------------------------------------------------------------

function openWatch() {
  if (state.closed || state.expired) return;
  closeWatch();
  state.syncTimer = setInterval(syncStatus,10000);
  if (!state.channel || !state.sessionHandle) { syncStatus(); return; }
  const channel = state.channel;
  const es = new EventSource(api('/api/v1/watch?session_handle=' + encodeURIComponent(state.sessionHandle)));
  state.eventSource = es;
  const current = () => state.eventSource === es && channel === state.channel && !state.closed && !state.expired;
  es.addEventListener('open', () => {
    if (!current()) return;
    markStale('Connected to relay · waiting for verified timer status');
    reconcilePending();
    syncStatus();
  });
  const receive = (type, cached = false) => (event) => {
    if (!current()) return;
    try {
      const envelope = JSON.parse(event.data);
      queueFrame(envelope, type, { cached: cached || envelope.cached === true, channel }).catch(() => {
        if (current()) markStale('Unable to retain verified state');
      });
    } catch (_) { /* Invalid data never changes the timer display. */ }
  };
  es.addEventListener('e2e', receive(null));
  es.addEventListener('token_expired', () => { if (current()) showLinkExpired(); });
  es.addEventListener('token_revoked', () => {
    if (!current()) return;
    showEnd('Share link revoked', 'The owner revoked this link. It cannot be used again.');
  });
  es.addEventListener('timer_gone', () => { if (current()) markStale('Timer offline · last verified snapshot', true); });
  es.addEventListener('error', () => {
    if (state.eventSource !== es || state.closed || state.expired) return;
    markStale('Relay connection interrupted · last verified snapshot', true);
    checkSessionBinding();
    // Native EventSource stops retrying after an HTTP 503 (CLOSED). An
    // ordinary network failure remains CONNECTING and owns its native retry.
    // Reopen only this still-current stream; never resend a control intent.
    if (es.readyState === EventSource.CLOSED && !state.watchRetryTimer) {
      const delay = state.watchRetryDelayMs;
      state.watchRetryDelayMs = Math.min(delay * 2, 30000);
      state.watchRetryTimer = setTimeout(() => {
        state.watchRetryTimer = 0;
        if (state.eventSource === es && !state.closed && !state.expired) openWatch();
      }, delay);
    }
  });
  // Some browsers/proxies wait for the first stream body before firing open.
  // Request a fresh authenticated challenge immediately and keep read-only polling
  // independent of stream admission. No control is replayed by this timer.
  syncStatus();
}

async function checkSessionBinding() {
  // EventSource hides HTTP status codes. A read-only lookup distinguishes a
  // expired routing admission from an ordinary reconnect. Bound checks
  // avoid adding a request for every retry during an outage.
  if (state.closed || !state.channel || !state.sessionHandle || state.bindingCheck || Date.now() - state.lastBindingCheckMs < 10000) return;
  const handle = state.sessionHandle;
  state.lastBindingCheckMs = Date.now();
  const controller = new AbortController();
  const deadline = setTimeout(() => controller.abort(), 5000);
  state.bindingCheck = (async () => {
    try {
      const response = await fetch(api('/api/v1/session'), {
        credentials: 'omit', headers: { [SESSION_HEADER]: handle }, signal: controller.signal,
      });
      if (handle !== state.sessionHandle || state.closed || state.expired) return;
      if (response.status === 409) handleSessionError(makeError('session_mismatch', 409));
      else if (response.status === 401 || response.status === 403) handleSessionError(makeError('invalid_or_expired', response.status));
      else if (response.ok) {
        const info = await response.json();
        if (handle !== state.sessionHandle || state.closed || state.expired) return;
        if (info.session_handle !== state.sessionHandle || info.boot_id !== state.bootId) {
          handleSessionError(makeError('session_mismatch', 409));
        }
      }
    } catch (_) { /* An outage leaves the snapshot stale and EventSource retries. */ }
    finally { clearTimeout(deadline); }
  })();
  try { await state.bindingCheck; } finally { state.bindingCheck = null; }
}

function closeWatch() {
  clearInterval(state.syncTimer);
  state.syncTimer = 0;
  clearTimeout(state.watchRetryTimer);
  state.watchRetryTimer = 0;
  clearTimeout(state.reconcileTimer);
  state.reconcileTimer = 0;
  if (state.eventSource) {
    state.eventSource.close();
    state.eventSource = null;
  }
}

// ---------------------------------------------------------------------------
// Dial rendering
// ---------------------------------------------------------------------------

function applyStatusToDial(snap) {
  const dial = $('timerDial');
  const kicker = $('dialKicker');
  const time = $('dialTime');
  const subtext = $('dialSubtext');

  const stateName = snap.state || 'open';
  const remaining = Math.max(0, Number(snap.remaining_ms) || 0);
  const duration = Math.max(0, Number(snap.session_duration_ms) || 0);
  const sealed = !!snap.sealed;
  // Same page state as the local app, so the locked (amber) ring matches.
  document.documentElement.dataset.state = sealed ? 'sealed' : stateName;
  const emergency = !!snap.emergency_input_active;

  dial.classList.remove('active', 'paused');
  const badge = $('stateBadge');
  badge.textContent = t(emergency ? 'Emergency' : sealed ? 'Locked' : stateName === 'open' ? 'Unlocked' : capitalize(stateName));
  badge.className = 'badge ' + (emergency ? 'emergency' : sealed ? 'sealed' : stateName);
  $('statusLine').textContent = emergency ? t('Emergency input active') : '';

  if (emergency) {
    kicker.textContent = t('Emergency held');
    time.textContent = '—';
    subtext.textContent = t('Emergency release is held. The lock output is off, and a new timer cannot start.');
    dial.style.setProperty('--progress', '0deg');
    return;
  }

  if (stateName === 'running') {
    dial.classList.add('active');
    kicker.textContent = t(sealed ? 'Running · controls locked' : 'Time remaining');
    time.textContent = formatMs(remaining);
    setProgressFromRemaining(dial, remaining, duration);
    subtext.textContent = t('Session in progress. This page is not an emergency release.');
  } else if (stateName === 'paused') {
    dial.classList.add('paused');
    kicker.textContent = t(sealed ? 'Paused · controls locked' : 'Paused');
    time.textContent = formatMs(remaining);
    setProgressFromRemaining(dial, remaining, duration);
    subtext.textContent = t('Paused. Resume when ready.');
  } else {
    kicker.textContent = t(state.scopes.has('start') ? 'Set duration' : 'Ready');
    time.textContent = '00:00:00';
    dial.style.setProperty('--progress', '0deg');
    subtext.textContent = t(state.scopes.has('start') ? 'Choose a duration to start the timer.' : 'The lock is unlocked.');
  }
  // A share link never keeps the lock engaged past its own expiry.
  const limit = snap.share_link_limit_ms;
  if (stateName !== 'open' && Number.isSafeInteger(limit) && limit >= 0) {
    const at = new Date(Date.now() + limit).toLocaleTimeString([], { hour: 'numeric', minute: '2-digit' });
    subtext.textContent = t('Opens by {time} when the share link ends, even if paused or locked.', { time: at });
  }
}

function setProgressFromRemaining(dial, remaining, duration) {
  if (duration <= 0) {
    dial.style.setProperty('--progress', '0deg');
    return;
  }
  const pct = Math.min(1, Math.max(0, remaining / duration));
  dial.style.setProperty('--progress', (pct * 360).toFixed(1) + 'deg');
}

function formatMs(ms) {
  const total = Math.round(ms / 1000);
  const h = Math.floor(total / 3600);
  const m = Math.floor((total % 3600) / 60);
  const s = total % 60;
  const pad = (n) => n.toString().padStart(2, '0');
  return `${pad(h)}:${pad(m)}:${pad(s)}`;
}

// ---------------------------------------------------------------------------
// Button gating (state-dependent)
// ---------------------------------------------------------------------------

const SEALED_HIDDEN_OPS = new Set(['pause', 'resume', 'release', 'lock_controls']);
function updateButtonsForState(snap) {
  const sealed = !!snap && !!snap.sealed && snap.state !== 'open';
  let setupEnabled = false;
  const buttons = document.querySelectorAll('button[data-op]');
  buttons.forEach((btn) => {
    const op = btn.dataset.op;
    let visible = state.scopes.has(btn.dataset.scope);
    let enabled = !!snap && !state.expired && !state.closed;
    const stateName = snap && snap.state;
    if (op === 'start') visible = visible && (!snap || stateName === 'open');
    if (op === 'release') visible = visible && (stateName !== 'open' || state.pendingIntents.length > 0);
    if (op === 'pause') visible = visible && stateName === 'running';
    if (op === 'resume') visible = visible && stateName === 'paused';
    if (op === 'adjust') {
      visible = visible && (stateName === 'running' || stateName === 'paused');
    }
    if (op === 'lock_controls') visible = visible && stateName === 'running';
    // Locked controls: the timer refuses these from every client, so show the
    // locked notice instead of buttons that can only fail.
    if (sealed && SEALED_HIDDEN_OPS.has(op)) visible = false;
    // The owner can switch pause and control locking off in the timer's
    // settings; the timer then refuses them from every client.
    if (op === 'pause' && snap && snap.pause_allowed === false) visible = false;
    if (op === 'lock_controls' && snap && snap.control_lock_allowed === false) visible = false;
    // An authorized explicit release remains reachable with an uncertain prior
    // action or stale display. The timer still enforces all preconditions.
    if (op !== 'release') {
      enabled = enabled && state.fresh && state.pendingIntents.length === 0;
      if (snap && (snap.sealed || snap.emergency_input_active) && op !== 'identify') enabled = false;
    }
    btn.classList.toggle('hidden', !visible);
    btn.disabled = !enabled || state.awaitingCommand !== null || state.preparingCommand;
    if (op === 'start') setupEnabled = visible && !btn.disabled;
  });
  $('sealedNotice').classList.toggle('hidden', !sealed);
  // Setup waits for a fresh, idle timer: dim the fields and presets too.
  state.setupEnabled = setupEnabled;
  const random = !!$('randomMode').checked;
  for (const id of ['hours', 'minutes', 'seconds']) $(id).disabled = !setupEnabled || random;
  for (const id of ['randomMode', 'randomMin', 'randomMax']) $(id).disabled = !setupEnabled;
  $('resetDuration').disabled = !setupEnabled;
  const canSetDuration = !!snap && snap.state === 'open' && state.scopes.has('start') && !snap.emergency_input_active;
  $('setupControls').classList.toggle('hidden', !canSetDuration);
  $('fixedDuration').classList.toggle('hidden', !canSetDuration || random);
  $('randomFields').classList.toggle('hidden', !canSetDuration || !random);
  $('durationGestureHint').classList.toggle('hidden', !canSetDuration);
  $('dialTime').classList.toggle('hidden', canSetDuration);
  const canAdjust = !!snap && snap.state !== 'open' && !sealed && state.scopes.has('adjust');
  $('leftRail').classList.toggle('hidden', !canAdjust);
  $('rightRail').classList.toggle('hidden', !canAdjust);
  $('controlsCard').classList.toggle('no-adjust', !canAdjust);
  updateDurationPresets();
}

async function onButton(btn) {
  const op = btn.dataset.op;
  if (state.preparingCommand || state.awaitingCommand || !state.scopes.has(btn.dataset.scope)) return;
  if (!state.status || state.expired || state.closed || (!state.fresh && op !== 'release')) {
    showResult(false, 'Waiting for current verified timer status.');
    return;
  }
  if (state.pendingIntents.length && op !== 'release') return;
  const unresolvedRelease = state.pendingIntents.find((entry) => entry.op === 'release');
  if (op === 'release' && unresolvedRelease) return runCommand(unresolvedRelease);
  const clickedSnapshot = state.status;
  const params = {};
  if (op === 'adjust') params.delta_ms = Number(btn.dataset.adjust || '0');
  else if (op === 'start') {
    const durationMs = readStartDuration();
    if (durationMs == null) return;
    params.duration_ms = durationMs;
  }
  if (!operationAllowed(op, clickedSnapshot)) return;
  state.preparingCommand = true;
  updateButtonsForState(state.status);
  try {
    if (op === 'lock_controls' && !await confirmLockControls(btn)) return;
    showResult(null, 'Waiting for current verified timer status.');
    // The ordinary heartbeat can be ten seconds old. Authenticate a new
    // challenge before creating a five-second intent; never renew a retry.
    if (!await syncStatus() || !state.fresh || state.closed || state.expired) {
      showResult(false, 'Unable to verify current timer status. No command was sent.');
      return;
    }
    const current = state.status;
    if (current.boot_id !== clickedSnapshot.boot_id || current.session_id !== clickedSnapshot.session_id ||
        !operationAllowed(op, current) || (op === 'start' && params.duration_ms > current.max_duration_ms)) {
      showResult(false, 'The timer changed. Review its current state before trying again.');
      return;
    }
    await runCommand({
      op, op_id: generateOpId(op), params,
      if_boot_id: clickedSnapshot.boot_id,
      if_session_id: clickedSnapshot.session_id,
      observed_uptime_ms: current.uptime_ms,
    });
  } finally {
    state.preparingCommand = false;
    updateButtonsForState(state.status);
    if (op === 'lock_controls' && !state.closed && !btn.disabled && !btn.classList.contains('hidden')) btn.focus();
  }
}
function operationAllowed(op, snap) {
  if (!snap || (snap.emergency_input_active && op !== 'release' && op !== 'identify')) return false;
  if (snap.sealed && snap.state !== 'open' && op !== 'identify') return false;
  if (op === 'start') return snap.state === 'open';
  if (op === 'pause' || op === 'lock_controls') return snap.state === 'running';
  if (op === 'resume') return snap.state === 'paused';
  if (op === 'adjust') return snap.state === 'running' || snap.state === 'paused';
  return op === 'release' || op === 'identify';
}

async function runCommand(intent) {
  if (state.awaitingCommand || state.expired || state.closed) return;
  const alreadyPending = state.pendingIntents.some((entry) => entry.op_id === intent.op_id);
  if (alreadyPending && !retryWindowOpen(intent)) {
    showResult(false, '{action} outcome unknown. Its retry window ended; check the saved result. No new request was sent.', { action: labelForOp(intent.op) });
    return;
  }
  // Persist before network delivery. A refresh during fetch has an unknown
  // outcome and must retain exactly the original id, params and preconditions.
  if (!alreadyPending) {
    state.pendingIntents.push(intent);
  }
  const priorAttempts = state.attemptsById[intent.op_id] || (alreadyPending ? 1 : 0);
  state.attemptsById[intent.op_id] = priorAttempts + 1;
  try { saveTabState(); }
  catch (_) {
    if (!alreadyPending) state.pendingIntents = state.pendingIntents.filter((entry) => entry.op_id !== intent.op_id);
    if (priorAttempts) state.attemptsById[intent.op_id] = priorAttempts;
    else delete state.attemptsById[intent.op_id];
    showResult(false, 'Browser storage is unavailable. The command was not sent.');
    return;
  }
  state.awaitingCommand = intent.op_id;
  showResult(null, 'Sending {action}…', { action: labelForOp(intent.op) });
  renderPendingIntents();
  updateButtonsForState(state.status);
  const controller = new AbortController();
  const deadline = setTimeout(() => controller.abort(), 8000);
  try {
    const res = await secureSend({...intent,type:'command',expiry_uptime_ms:intent.observed_uptime_ms+INTENT_WINDOW_MS},intent.op_id,controller.signal);
    const envelope = await safeJson(res);
    if (!res.ok || !await queueFrame(envelope, 'result', { intent })) {
      // A valid result may have arrived over SSE before this HTTP response.
      if (state.pendingIntents.some((entry) => entry.op_id === intent.op_id)) showUnknown(intent);
      if (res.status === 409 && envelope && envelope.detail === 'session_mismatch') {
        showEnd('Another share link is active', 'Reopen the original share link in a separate browser profile. The previous command outcome remains unknown.');
      }
    }
  } catch (_) {
    if (state.pendingIntents.some((entry) => entry.op_id === intent.op_id)) showUnknown(intent);
  } finally {
    clearTimeout(deadline);
    state.awaitingCommand = null;
    renderPendingIntents();
    updateButtonsForState(state.status);
    scheduleReconcile();
  }
}
function showUnknown(intent) {
  showResult(false, '{action} outcome unknown. Check its result or retry the exact same request below.', { action: labelForOp(intent.op) });
}
function retryWindowOpen(intent) {
  return state.fresh && !state.closed && state.lastStatusWallMs > 0 &&
    estimatedTimerUptime() < intent.observed_uptime_ms + INTENT_WINDOW_MS;
}
function estimatedTimerUptime() {
  if (!state.lastStatusWallMs) return 0;
  return state.lastStatusUptimeMs + Math.max(0, Date.now() - state.lastStatusWallMs);
}
function renderPendingIntents() {
  const card = $('pendingCard');
  const list = $('pendingList');
  const unresolved = state.pendingIntents.filter(intent => intent.op_id !== state.awaitingCommand);
  card.classList.toggle('hidden', unresolved.length === 0);
  list.replaceChildren();
  for (const intent of unresolved) {
    const item = document.createElement('li');
    const label = document.createElement('p');
    label.textContent = t('{action} · outcome unknown ({id})', { action: t(labelForOp(intent.op)), id: intent.op_id });
    const retry = document.createElement('button');
    retry.type = 'button';
    retry.className = 'secondary';
    const inWindow = retryWindowOpen(intent);
    retry.textContent = inWindow ? t('Retry same {action} request', { action: t(labelForOp(intent.op)).toLowerCase() })
      : t(state.fresh ? 'Retry window ended · check results' : 'Waiting for live status · check results');
    retry.disabled = state.awaitingCommand !== null || state.expired || state.closed || !inWindow;
    retry.addEventListener('click', () => runCommand(intent));
    item.append(label, retry);
    list.appendChild(item);
  }
}
async function reconcilePending() {
  if (state.closed || state.expired) return;
  if (state.reconcilePromise) return state.reconcilePromise;
  clearTimeout(state.reconcileTimer);
  state.reconcileTimer = 0;
  state.reconcilePromise = lookupPendingResults();
  try { await state.reconcilePromise; }
  finally { state.reconcilePromise = null; scheduleReconcile(); }
}
function scheduleReconcile() {
  if (!state.pendingIntents.length) {
    clearTimeout(state.reconcileTimer);
    state.reconcileTimer = 0;
    state.reconcileDelayMs = 1000;
    return;
  }
  if (state.closed || state.expired || state.reconcileTimer || state.reconcilePromise) return;
  state.reconcileTimer = setTimeout(async () => {
    state.reconcileTimer = 0;
    state.reconcileDelayMs = Math.min(state.reconcileDelayMs * 2, 10000);
    await reconcilePending();
  }, state.reconcileDelayMs);
}
async function lookupPendingResults() {
  // Read-only lookup; no operation is automatically replayed.
  for (const intent of [...state.pendingIntents]) {
    if (state.closed || state.expired || intent.op_id === state.awaitingCommand) continue;
    try {
      const abort = new AbortController(), deadline = setTimeout(() => abort.abort(),5000);
      let res;
      try { res = await secureSend({type:'lookup'},intent.op_id,abort.signal); }
      finally { clearTimeout(deadline); }
      if (res.ok) await queueFrame(await safeJson(res), 'result', { intent, cached: true });
    } catch (_) { /* The explicit retry remains available. */ }
  }
}

async function safeJson(res) {
  try { return await res.json(); } catch (_) { return null; }
}

function generateOpId(op) {
  const rand = crypto.getRandomValues(new Uint8Array(16));
  let hex = '';
  rand.forEach((b) => { hex += b.toString(16).padStart(2, '0'); });
  return `s-${hex}`;
}

function labelForOp(op) {
  const map = {
    start: 'Start', pause: 'Pause', resume: 'Resume', adjust: 'Adjust',
    release: 'Release', lock_controls: 'Lock controls', identify: 'Identify',
  };
  return map[op] || op;
}

function resultText(message) {
  const values = { ...message.values };
  for (const key of ['action', 'reason']) if (values[key]) values[key] = t(values[key]);
  return t(message.key, values);
}
function showResult(ok, key, values = {}) {
  state.lastResultMessage = { key, values };
  const el = $('lastResult');
  el.textContent = resultText(state.lastResultMessage);
  el.classList.remove('hidden', 'ok', 'err', 'pending');
  el.classList.add(ok === null ? 'pending' : ok ? 'ok' : 'err');
  clearTimeout(showResult._t);
  if (state.pendingIntents.length === 0) showResult._t = setTimeout(() => el.classList.add('hidden'), 6000);
}

// Presentation mirrors the local HTTP UI; transport and authorization stay here.
function setupPresentation() {
  const applyTheme = (theme) => {
    document.documentElement.dataset.theme = theme;
    const button = $('themeButton');
    button.dataset.theme = theme;
    button.setAttribute('aria-checked', String(theme === 'dark'));
    button.setAttribute('aria-label', t(theme === 'dark' ? 'Switch to light mode' : 'Switch to dark mode'));
    const meta = document.querySelector('meta[name="theme-color"]');
    if (meta) meta.content = theme === 'dark' ? '#09070d' : '#f7f3fa';
    try { localStorage.setItem('ost-relay-theme', theme); } catch (_) { /* Optional preference. */ }
  };
  let theme = matchMedia('(prefers-color-scheme: light)').matches ? 'light' : 'dark';
  try {
    const saved = localStorage.getItem('ost-relay-theme');
    if (saved === 'light' || saved === 'dark') theme = saved;
  } catch (_) { /* System default. */ }
  applyTheme(theme);
  $('themeButton').addEventListener('click', () => applyTheme(document.documentElement.dataset.theme === 'dark' ? 'light' : 'dark'));
  $('languageButton').addEventListener('click', () => window.SmartTimerI18n?.toggleLanguage());
  $('disconnectButton').addEventListener('click', disconnectSession);
  window.addEventListener('smarttimer:languagechange', () => {
    applyTheme(document.documentElement.dataset.theme);
    if (state.status && state.fresh) applyStatusToDial(state.status);
    renderPendingIntents();
    renderEvents();
    renderEstimatedClock();
    if (!state.closed && !state.expired && Number.isFinite(state.tokenExpUpMs)) {
      $('expiryCountdown').textContent = t('Expires in {time}', { time: formatCountdown(Math.max(0, state.tokenExpUpMs - estimatedTimerUptime())) });
    }
    if (state.lastResultMessage) $('lastResult').textContent = resultText(state.lastResultMessage);
  });
  $('randomMode').addEventListener('change', () => updateButtonsForState(state.status));
  $('confirmCancel').addEventListener('click', () => closeLockConfirm(false));
  $('confirmBackdrop').addEventListener('click', () => closeLockConfirm(false));
  $('confirmAccept').addEventListener('click', () => closeLockConfirm(true));
  document.addEventListener('keydown', event => {
    if (!lockConfirmation) return;
    if (event.key === 'Escape') closeLockConfirm(false);
    if (event.key === 'Tab') {
      // Safari can omit buttons from native Tab navigation. Keep this two-
      // action dialog traversable regardless of the browser keyboard setting.
      event.preventDefault();
      const cancel = $('confirmCancel'), accept = $('confirmAccept');
      (document.activeElement === cancel ? accept : document.activeElement === accept
        ? cancel : event.shiftKey ? accept : cancel).focus();
    }
  });
  document.querySelectorAll('[data-preset]').forEach((button) => button.addEventListener('click', () => {
    const minutes = Number(button.dataset.preset);
    $('hours').value = String(Math.floor(minutes / 60));
    $('minutes').value = String(minutes % 60);
    $('seconds').value = '0';
    updateDurationPresets();
  }));
  // Retry reopens the verified status stream now instead of waiting for the
  // browser's EventSource backoff. It never resends a command.
  $('connectionRetry').addEventListener('click', () => {
    if (!state.sessionHandle || state.closed || state.expired) return;
    $('connectionWarningMessage').textContent = t('Reconnecting…');
    openWatch();
  });
  $('resetDuration').addEventListener('click', () => {
    for (const id of ['hours', 'minutes', 'seconds']) $(id).value = '0';
    updateDurationPresets();
  });
  for (const id of ['hours', 'minutes', 'seconds']) $(id).addEventListener('input', updateDurationPresets);
  for (const fieldId of ['hours', 'minutes', 'seconds']) wireDurationScrubber(
    fieldId, () => state.setupEnabled && !$('randomMode').checked,
    steps => adjustFixedDuration(fieldId, steps),
    () => setFixedDurationSeconds(fixedDurationSeconds()),
  );
  for (const fieldId of ['randomMin', 'randomMax']) wireDurationScrubber(
    fieldId, () => state.setupEnabled && $('randomMode').checked,
    steps => adjustRandomDuration(fieldId, steps),
    () => normalizeRandomDuration(fieldId),
  );
}
// Match the local and Bluetooth duration editor: 18px per swipe step and
// 42px per scroll step. Editing a draft never sends a timer command.
function fixedDurationSeconds() {
  return Number($('hours').value || 0) * 3600 + Number($('minutes').value || 0) * 60 + Number($('seconds').value || 0);
}
function durationLimitSeconds() {
  const limit = state.status && state.status.max_duration_ms;
  return Number.isSafeInteger(limit) && limit > 0 ? Math.min(86400, Math.floor(limit / 1000)) : 0;
}
function setFixedDurationSeconds(seconds) {
  const total = Math.max(0, Math.min(durationLimitSeconds(), Math.round(seconds) || 0));
  $('hours').value = String(Math.floor(total / 3600));
  $('minutes').value = String(Math.floor(total % 3600 / 60));
  $('seconds').value = String(total % 60);
  updateDurationPresets();
}
function adjustFixedDuration(fieldId, steps) {
  const units = {hours: 3600, minutes: 60, seconds: 1};
  setFixedDurationSeconds(fixedDurationSeconds() + steps * units[fieldId]);
}
function randomDurationValues() {
  const limit = Math.floor(durationLimitSeconds() / 60);
  const minimum = Math.max(1, Math.min(limit, Math.round(Number($('randomMin').value) || 1)));
  const maximum = Math.max(minimum, Math.min(limit, Math.round(Number($('randomMax').value) || minimum)));
  return {minimum, maximum, limit};
}
function adjustRandomDuration(fieldId, steps) {
  let {minimum, maximum, limit} = randomDurationValues();
  if (limit < 1) return;
  if (fieldId === 'randomMin') minimum = Math.max(1, Math.min(maximum, minimum + steps));
  else maximum = Math.max(minimum, Math.min(limit, maximum + steps));
  $('randomMin').value = String(minimum);
  $('randomMax').value = String(maximum);
}
function normalizeRandomDuration(fieldId) {
  const limit = Math.floor(durationLimitSeconds() / 60);
  if (limit < 1) return;
  let minimum = Math.max(1, Math.min(limit, Math.round(Number($('randomMin').value) || 1)));
  let maximum = Math.max(1, Math.min(limit, Math.round(Number($('randomMax').value) || 1)));
  if (minimum > maximum) {
    if (fieldId === 'randomMin') maximum = minimum;
    else minimum = maximum;
  }
  $('randomMin').value = String(minimum);
  $('randomMax').value = String(maximum);
}
function wireDurationScrubber(fieldId, enabled, adjust, normalize) {
  const input = $(fieldId), label = input.closest('label');
  const canAdjust = () => !input.disabled && enabled();
  let pointerId = null, pointerStartY = 0, appliedSteps = 0, dragged = false;
  let suppressClickUntil = 0, wheelPixels = 0, wheelReset = 0;
  input.addEventListener('pointerdown', event => {
    if (!canAdjust() || pointerId !== null || (event.button !== undefined && event.button !== 0)) return;
    pointerId = event.pointerId;
    pointerStartY = event.clientY;
    appliedSteps = 0;
    dragged = false;
    input.setPointerCapture?.(pointerId);
  });
  input.addEventListener('pointermove', event => {
    if (pointerId !== event.pointerId || !canAdjust()) return;
    const nextSteps = Math.trunc((pointerStartY - event.clientY) / 18);
    if (nextSteps === appliedSteps) return;
    event.preventDefault();
    adjust(nextSteps - appliedSteps);
    appliedSteps = nextSteps;
    dragged = true;
    label.classList.add('dragging');
  });
  const finish = event => {
    if (pointerId !== event.pointerId) return;
    if (dragged) { suppressClickUntil = performance.now() + 300; input.blur(); }
    label.classList.remove('dragging');
    pointerId = null;
  };
  input.addEventListener('pointerup', finish);
  input.addEventListener('pointercancel', finish);
  input.addEventListener('lostpointercapture', finish);
  input.addEventListener('click', event => {
    if (performance.now() < suppressClickUntil) event.preventDefault();
  });
  input.addEventListener('wheel', event => {
    if (!canAdjust()) return;
    event.preventDefault();
    clearTimeout(wheelReset);
    wheelPixels += event.deltaY * (event.deltaMode === 1 ? 16 : 1);
    const steps = Math.trunc(wheelPixels / 42);
    if (steps !== 0) { adjust(-steps); wheelPixels -= steps * 42; }
    wheelReset = setTimeout(() => { wheelPixels = 0; }, 180);
  }, {passive: false});
  input.addEventListener('change', () => { if (canAdjust()) normalize(); });
}
function updateDurationPresets() {
  const minutes = Number($('hours').value) * 60 + Number($('minutes').value) + Number($('seconds').value) / 60;
  const maxMs = state.status && state.status.max_duration_ms;
  document.querySelectorAll('[data-preset]').forEach((button) => {
    button.classList.toggle('active', Number(button.dataset.preset) === minutes);
    button.disabled = !state.setupEnabled || !!$('randomMode').checked ||
      (Number.isSafeInteger(maxMs) && Number(button.dataset.preset) * 60000 > maxMs);
  });
}
function readStartDuration() {
  if ($('randomMode').checked) {
    const values = ['randomMin', 'randomMax'].map(id => $(id).value.trim());
    const [min, max] = values.map(Number);
    const maxMs = Number(state.status && state.status.max_duration_ms);
    if (values.some(value => !/^\d+$/.test(value)) || ![min, max].every(Number.isSafeInteger) ||
        min < 1 || max < min || max > 1440 || !Number.isSafeInteger(maxMs) || max * 60000 > maxMs) {
      showResult(false, 'Choose a random range within this timer’s limit, with the minimum no greater than the maximum.');
      return null;
    }
    // Pick once on this trusted page. The original chosen duration is persisted
    // and reused byte-for-byte if the operator explicitly retries the intent.
    const width = (max - min) * 60 + 1;
    const ceiling = Math.floor(0x100000000 / width) * width;
    let sample;
    do { sample = crypto.getRandomValues(new Uint32Array(1))[0]; } while (sample >= ceiling);
    return (min * 60 + sample % width) * 1000;
  }
  const values = ['hours', 'minutes', 'seconds'].map(id => $(id).value.trim());
  const [hours, minutes, seconds] = values.map(Number);
  const maxMs = Number(state.status && state.status.max_duration_ms);
  const durationMs = (hours * 3600 + minutes * 60 + seconds) * 1000;
  if (values.some(value => !/^\d+$/.test(value)) || ![hours, minutes, seconds].every(Number.isSafeInteger) ||
      hours < 0 || hours > 24 || minutes < 0 || minutes > 59 || seconds < 0 || seconds > 59 ||
      !Number.isSafeInteger(maxMs) || maxMs <= 0 || durationMs <= 0 || durationMs > maxMs) {
    showResult(false, 'Enter a valid duration within this timer’s limit.');
    return null;
  }
  return durationMs;
}

let lockConfirmation = null;
function confirmLockControls(button) {
  return new Promise(resolve => {
    lockConfirmation = { resolve, button };
    $('confirmDialog').classList.add('show');
    $('confirmBackdrop').classList.add('show');
    $('confirmDialog').setAttribute('aria-hidden', 'false');
    document.querySelector('main').inert = true;
    $('confirmCancel').focus();
  });
}
function closeLockConfirm(accepted) {
  if (!lockConfirmation) return;
  const { resolve } = lockConfirmation;
  lockConfirmation = null;
  $('confirmDialog').classList.remove('show');
  $('confirmBackdrop').classList.remove('show');
  $('confirmDialog').setAttribute('aria-hidden', 'true');
  document.querySelector('main').inert = false;
  resolve(accepted);
}

// ---------------------------------------------------------------------------
// Share-link expiry countdown
// ---------------------------------------------------------------------------

function startExpiryCountdown() {
  clearInterval(state.expiryTimer);
  renderExpiryCountdown();
  state.expiryTimer = setInterval(renderExpiryCountdown, 1000);
}

function renderExpiryCountdown() {
  if (state.pendingIntents.length) renderPendingIntents();
  if (state.fresh && Date.now() - state.lastVerifiedWallMs > STATUS_FRESH_MS) {
    markStale('Timer status is stale · last verified snapshot', true);
  }
  renderEstimatedClock();
  const el = $('expiryCountdown');
  if (!Number.isFinite(state.tokenExpUpMs)) {
    el.classList.add('hidden');
    return;
  }
  const estUptime = estimatedTimerUptime();
  const remainingMs = state.tokenExpUpMs - estUptime;

  if (remainingMs <= 0 && !state.expired) {
    state.expired = true;
    clearInterval(state.expiryTimer);
    el.classList.add('hidden');
    showLinkExpired();
    return;
  }
  if (remainingMs <= 0) return;

  el.classList.remove('hidden', 'warn', 'urgent');
  if (remainingMs < 30_000) el.classList.add('urgent');
  else if (remainingMs < 5 * 60_000) el.classList.add('warn');
  el.textContent = t('Expires in {time}', { time: formatCountdown(remainingMs) });
}

function renderEstimatedClock() {
  const snap = state.status;
  if (!snap || !state.fresh || state.closed || snap.emergency_input_active || snap.state === 'open') return;
  const elapsed = snap.state === 'running' ? Math.max(0, Date.now() - state.lastVerifiedWallMs) : 0;
  const remaining = Math.max(0, snap.remaining_ms - elapsed);
  $('dialTime').textContent = formatMs(remaining);
  setProgressFromRemaining($('timerDial'), remaining, snap.session_duration_ms);
  // Display estimate only: never change the authenticated snapshot or infer an open state.
}

function formatCountdown(ms) {
  const total = Math.max(0, Math.round(ms / 1000));
  if (total >= 3600) {
    const h = Math.floor(total / 3600);
    const m = Math.floor((total % 3600) / 60);
    return t('{hours}h {minutes}m', { hours: h, minutes: m });
  }
  const m = Math.floor(total / 60);
  const s = total % 60;
  return `${m}:${s.toString().padStart(2, '0')}`;
}

// ---------------------------------------------------------------------------
// Event log rendering (DOM only; never innerHTML with event data)
// ---------------------------------------------------------------------------

function appendEvent(ev) {
  state.events.unshift({
    type: String(ev.type || 'event'),
    at: Date.now(),
    valueMs: Number(ev.value_ms || 0),
    viaLink: ev.via_share_link === true,
  });
  if (state.events.length > 40) state.events.length = 40;
  renderEvents();
}

function renderEvents() {
  $('eventCard').classList.toggle('hidden', state.events.length === 0);
  const list = $('eventList');
  list.replaceChildren();
  for (const ev of state.events) {
    const li = document.createElement('li');
    const tag = document.createElement('span');
    const label = document.createElement('span');
    const time = document.createElement('span');

    tag.className = 'event-tag';
    tag.textContent = t(ev.viaLink ? 'Share link' : 'Timer');
    label.textContent = readableEventLabel(ev);
    time.className = 'event-time';
    time.textContent = formatTimeAgo(ev.at);

    li.append(tag, label, time);
    list.appendChild(li);
  }
}

function readableEventLabel(ev) {
  const labels = { paused: 'Session paused', resumed: 'Session resumed', expired: 'Session expired',
    opened_by_user: 'Released by user', emergency_release: 'Emergency release',
    emergency_input_activated: 'Emergency input activated', emergency_input_cleared: 'Emergency input cleared',
    controls_locked: 'Controls locked', identify_started: 'Identify LED', remaining_set: 'Remaining time set',
    adjusted_to_zero: 'Adjusted to zero; lock opened', share_link_expired: 'Share link expired; lock opened',
    snapshot_changed: 'Timer updated' };
  if (ev.type === 'started') return ev.valueMs > 0 ? t('Session started · {duration}', { duration: formatMs(ev.valueMs) }) : t('Session started');
  if (ev.type === 'adjusted') return t('Adjusted {duration}', { duration: signed(ev.valueMs) });
  return t(labels[ev.type] || ev.type.replace(/_/g, ' '));
}

function signed(ms) {
  const secs = Math.round(ms / 1000);
  const abs = Math.abs(secs);
  const sign = secs >= 0 ? '+' : '−';
  if (abs >= 60) return t('{value} min', { value: sign + Math.round(abs / 60) });
  return t('{value} s', { value: sign + abs });
}

function formatTimeAgo(ts) {
  const s = Math.max(0, Math.round((Date.now() - ts) / 1000));
  if (s < 5) return t('now');
  if (s < 60) return t('{value}s', { value: s });
  if (s < 3600) return t('{value}m', { value: Math.round(s / 60) });
  return t('{value}h', { value: Math.round(s / 3600) });
}

// ---------------------------------------------------------------------------
// Boot
// ---------------------------------------------------------------------------

document.addEventListener('DOMContentLoaded', () => {
  $('checkPending').addEventListener('click', reconcilePending);
  document.addEventListener('visibilitychange', () => {
    if (document.visibilityState === 'visible' && state.sessionHandle && !state.expired && !state.closed) {
      markStale('Checking connection after returning to this tab');
      openWatch();
    }
  });
  main();
});
// Reopening the original link in this tab may change only its fragment.
// Start a fresh document so the usual validation, URL clearing and handshake run.
window.addEventListener('hashchange', () => {
  if (window.location.hash) window.location.reload();
});

// Refresh event timestamps periodically without touching the network.
setInterval(renderEvents, 15000);
