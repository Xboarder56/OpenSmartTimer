(() => {
  const $ = (id) => document.getElementById(id);
  const i18n = window.SmartTimerI18n;
  const t = (key, values = {}) => i18n ? i18n.text(key, values) : key;
  const appView = $('appView');
  const connection = document.querySelector('.connection');
  let status = null;
  let receivedAt = 0;
  let sessionDuration = 0;
  let pollHandle = 0;
  let pollBackoffUntil = 0;
  let liveSource = null;
  let liveOpen = false;
  let liveRetryHandle = 0;
  let liveSuspended = false;
  let requestSequence = 0;
  let mutationSequence = 0;
  let latestRenderedSequence = 0;
  let busy = false;
  let online = false;
  let awaitingWifiResult = false;
  let candidateWifiName = '';
  let displayedWifiError = '';
  let deviceNameDirty = false;
  let accessPointDirty = false;
  let networkModeDirty = false;
  let onboardingDismissed = false;
  let updateNotice = null;
  let diagnosticsCache = null;
  let eventsCache = null;
  let bluetoothCache = null;
  let bluetoothBusy = false;
  let sessionControlsBusy = false;
  let bluetoothPairPollHandle = 0;
  const relayMessages = {
    timer_active: 'Unlock the timer first. This relay change needs an open timer.',
    bluetooth_active: 'Remote sharing is paused while a Bluetooth app is connected or pairing. Try again after it disconnects.',
    link_table_full: 'A share link is already active. Renew or revoke it first.',
    link_rate_limited: 'Too many links were created this hour. Try again later.',
    relay_unavailable: 'The relay is not connected. New links need a live connection.',
    relay_busy: 'The relay is busy. Try again in a moment.',
    relay_maintenance: 'The relay is saving settings. Try again in a moment.',
    relay_disabled: 'Turn on remote sharing first.',
    relay_not_enrolled: 'Set up the relay first.',
    relay_job_conflict: 'The timer state changed. Please try again.',
    stale_boot: 'The timer restarted. Please try again.',
    storage_failed: 'The timer could not save relay settings.',
    relay_low_memory: 'Not enough free memory to start the relay right now.',
    low_memory: 'Waiting for free memory before connecting.',
    start_failed_low_memory: 'Waiting for free memory before connecting.',
    invalid_spki_pin: 'The server key pin must be the 43-character value from your relay operator.',
    invalid_relay_request: 'Check the relay address and enrollment code.',
    invalid_relay_url: 'Check the relay address. The relay must use HTTPS.',
    link_not_found: 'That link no longer exists.',
    link_not_ready: 'The relay has not confirmed this link yet. Try again in a moment.',
    enroll_pin_mismatch: 'The relay server key does not match the pin. Check the pin with your relay operator.',
    relay_auth_failed: 'The relay could not prove it is the server this timer was set up with. Check the relay address, or set it up again with a new setup code.',
    enroll_rejected: 'The relay refused this setup code. It may be used, expired or revoked, or the relay may be full. A relay that has just started may need a minute; try again.',
    enroll_timeout: 'The relay did not answer in time.',
    enroll_tls_failed: 'Secure connection to the relay failed.',
    tls_handshake: 'Secure connection to the relay failed.',
    enroll_malformed_response: 'The relay sent an unexpected response.',
    network_unavailable: 'The timer could not reach the relay. Check Wi-Fi and the address.',
    dns: 'The relay address could not be found.',
    tcp_connect: 'The timer could not reach the relay. Check Wi-Fi and the address.',
    websocket_handshake: 'The relay refused the connection.',
    key_storage_failed: 'The timer could not use its saved relay keys. Turn remote sharing off and on again.',
    emergency_input: 'The emergency input turned remote sharing off and ended the share link. Turn it on again when ready.',
    relay_link_gone: 'That link can no longer be reactivated (the timer restarted or it was revoked). Use New link.',
  };
  const relayCreateBlockedHints = {
    open: 'Create links while the timer is unlocked.',
    paused: 'Links can be created after the Bluetooth app disconnects.',
    offline: 'Links can be created once the relay is connected.',
    unavailable: 'Relay status is unavailable. Wait for a fresh connection before creating a link.',
    off: 'Turn on remote sharing to create a link.',
    busy: 'The relay is busy. Wait until it is ready to create a link.',
    full: 'A share link is already active. Renew or revoke it first.',
  };
  let relayCache = null;
  let relaySupported = null;
  let relayFresh = false;
  let relayReceivedAt = 0;
  let relayRequestBusy = false;
  let localControlsExpanded = false;
  let relayLinks = [];
  let relayLinksAt = 0;
  let relayBusy = false;
  let relayPollHandle = 0;
  let relayLinksSignature = '';
  let relayTtl = 3600;
  let relayTtlCustom = false;
  // The last link's name and lifetime, so an ended link can be renewed in
  // one click. Tab-local convenience only (like the theme); the timer keeps no history.
  let relayLastLink = null;
  try {
    const saved = JSON.parse(sessionStorage.getItem('smartTimerRelayLastLink') || 'null');
    if (saved && typeof saved.label === 'string' && Number(saved.ttl) >= 60) relayLastLink = saved;
  } catch (error) {
    relayLastLink = null;
  }

  let customAccessPointName = '';
  let lastCompletionSecond = -1;
  let lastCompletionState = '';
  let settingsReturnFocus = null;
  let guideReturnFocus = null;
  let confirmReturnFocus = null;
  let confirmAction = null;
  let confirmResolve = null;
  let reconnectHost = '192.168.4.1';
  let toastHandle = 0;
  let durationLimitHandle = 0;
  let colorTheme = 'dark';
  // SSE publishes a full snapshot at least every 15 seconds. Allow jitter;
  // freshness is based on device data, not internet reachability/onLine.
  const statusFreshnessMs = 25000;
  const pageStartedAt = performance.now();
  let connectionFailed = false;
  let lastHealthSecond = -1;

  try {
    const savedTheme = sessionStorage.getItem('smartTimerTheme');
    colorTheme = savedTheme === 'light' || savedTheme === 'dark'
      ? savedTheme
      : (window.matchMedia && window.matchMedia('(prefers-color-scheme: light)').matches ? 'light' : 'dark');
  } catch (error) {
    colorTheme = 'dark';
  }

  function setText(id, value) {
    const element = $(id);
    if (element.textContent !== value) element.textContent = value;
  }

  // "IP / name.local": break between the parts, never inside a hostname.
  function setAddress(id, value) {
    const element = $(id);
    if (element.textContent === value) return;
    element.replaceChildren(...value.split(' / ').flatMap((part, index) => {
      const span = document.createElement('span');
      span.className = 'address-part';
      span.textContent = part;
      return index ? [' / ', span] : [span];
    }));
  }

  function applyColorTheme(theme) {
    colorTheme = theme === 'light' ? 'light' : 'dark';
    document.documentElement.dataset.theme = colorTheme;
    const button = $('themeButton');
    button.dataset.theme = colorTheme;
    button.setAttribute('aria-checked', String(colorTheme === 'dark'));
    button.setAttribute('aria-label', t(colorTheme === 'dark' ? 'Switch to light mode' : 'Switch to dark mode'));
    const themeColor = document.querySelector('meta[name="theme-color"]');
    if (themeColor) themeColor.setAttribute('content', colorTheme === 'light' ? '#f7f3fa' : '#0b0710');
  }

  function toggleColorTheme() {
    const next = colorTheme === 'dark' ? 'light' : 'dark';
    try {
      sessionStorage.setItem('smartTimerTheme', next);
    } catch (error) {
      // Private browsers may block session storage; the current page still
      // switches without writing anything to the timer.
    }
    applyColorTheme(next);
  }

  function showStatusToast(title, message) {
    clearTimeout(toastHandle);
    setText('statusToastTitle', t(title));
    setText('statusToastMessage', t(message));
    $('statusToast').classList.add('show');
    toastHandle = setTimeout(() => $('statusToast').classList.remove('show'), 4500);
  }

  function showDurationLimit() {
    clearTimeout(durationLimitHandle);
    setText('durationLimitTitle', t('Maximum lock duration reached'));
    $('durationLimitBanner').querySelector('p').textContent =
      t('The lock duration cannot exceed 24 hours.');
    $('durationLimitBanner').classList.remove('hidden');
    setText('actionError', '');
    durationLimitHandle = setTimeout(() => {
      $('durationLimitBanner').classList.add('hidden');
    }, 6000);
  }

  async function request(path, options = {}) {
    const response = await fetch(path, { cache: 'no-store', credentials: 'same-origin', ...options });
    const type = response.headers.get('content-type') || '';
    const body = type.includes('json') ? await response.json() : await response.text();
    if (!response.ok) {
      const message = typeof body === 'object' ? body.error : body;
      const error = new Error(message || t('Request failed ({status})', { status: response.status }));
      error.status = response.status;
      error.code = typeof body === 'object' ? body.code : '';
      const retryAfterSeconds = Number(response.headers.get('retry-after'));
      error.retryAfterMs = typeof body === 'object' && Number(body.retry_after_ms) > 0
        ? Number(body.retry_after_ms)
        : Number.isFinite(retryAfterSeconds) && retryAfterSeconds > 0
          ? retryAfterSeconds * 1000
          : 0;
      throw error;
    }
    return body;
  }

  function formBody(values) {
    return new URLSearchParams(values);
  }

  function wait(milliseconds) {
    return new Promise((resolve) => setTimeout(resolve, milliseconds));
  }

  function nextPollDelay() {
    const jitteredInterval = 900 + Math.floor(Math.random() * 451);
    return Math.max(jitteredInterval, pollBackoffUntil - Date.now());
  }

  async function fileCrc32(file) {
    const bytes = new Uint8Array(await file.arrayBuffer());
    let crc = 0xFFFFFFFF;
    for (const byte of bytes) {
      crc = (crc ^ byte) >>> 0;
      for (let bit = 0; bit < 8; bit += 1) {
        crc = ((crc >>> 1) ^ ((crc & 1) ? 0xEDB88320 : 0)) >>> 0;
      }
    }
    return ((~crc) >>> 0).toString(16).padStart(8, '0');
  }

  async function mutate(path, values = {}, method = 'POST') {
    return request(path, {
      method,
      headers: { 'Content-Type': 'application/x-www-form-urlencoded;charset=UTF-8' },
      body: formBody(values),
    });
  }

  function timerMutation(path, values = {}) {
    mutationSequence += 1;
    const mutationValues = {
      ...values,
      operation_id: `${status && status.boot_id ? status.boot_id : 'boot'}-${Date.now().toString(36)}-${mutationSequence}`,
    };
    if (status && status.boot_id) {
      mutationValues.if_boot_id = status.boot_id;
    }
    if (path !== '/api/v1/timer/start' && status && status.session_id) {
      mutationValues.if_session_id = status.session_id;
    }
    return mutate(path, mutationValues);
  }

  function setConnection(mode, text) {
    connection.className = `connection ${mode}`;
    setText('connectionText', t(text));
    setText('footerConnection', t(text));
  }

  function statusIsFresh(now = performance.now()) {
    return Boolean(status && online && now - receivedAt <= statusFreshnessMs);
  }

  function updateConnectionHealth() {
    const age = performance.now() - (status ? receivedAt : pageStartedAt);
    const stale = connectionFailed || age > statusFreshnessMs;
    const intentionalPause = (busy && liveSuspended) || $('reconnectDialog').classList.contains('show');
    $('connectionWarning').classList.toggle('hidden', !stale || intentionalPause);
    if (!stale) return;
    online = false;
    setText('connectionWarningTitle', t(status ? 'Connection lost' : 'Timer status unavailable'));
    setText('connectionWarningMessage', status
      ? t('Last reply {seconds} s ago. The timer keeps running on its own; use the physical emergency release if needed.', { seconds: Math.round(age / 1000) })
      : t('Reconnect to the timer to confirm its state. Use the physical emergency release if needed.'));
    setText('stateBadge', t('UNKNOWN'));
    $('stateBadge').className = 'badge paused';
    setText('stateTitle', t('Waiting for current timer status'));
    setText('safetyNote', t('The current lock state is not confirmed.'));
    if (!intentionalPause) {
      setConnection('offline', 'Status unavailable');
      // A silently stalled open stream must not suppress REST forever.
      if (liveOpen) useRestFallback('Status is out of date. Reconnecting…');
    }
    updateControls();
  }

  function formatTime(milliseconds) {
    const total = Math.max(0, Math.ceil(milliseconds / 1000));
    const hours = Math.floor(total / 3600);
    const minutes = Math.floor((total % 3600) / 60);
    const seconds = total % 60;
    return [hours, minutes, seconds].map((part) => String(part).padStart(2, '0')).join(':');
  }

  function formatBytes(bytes) {
    const value = Number(bytes) || 0;
    if (value >= 1024 * 1024) return `${(value / (1024 * 1024)).toFixed(2)} MiB`;
    if (value >= 1024) return `${(value / 1024).toFixed(1)} KiB`;
    return `${value.toLocaleString()} bytes`;
  }

  function streamCloseReasonLabel(reason) {
    const labels = {
      none: 'No stream closed yet',
      peer_closed: 'Browser or client closed',
      request_headers_too_large: 'Request headers too large',
      request_header_timeout: 'Request header timed out',
      response_write_failed: 'Response write failed',
      send_timeout: 'Status send timed out',
      ota_quiet: 'Firmware update quiet window',
    };
    return t(labels[reason] || reason || labels.none);
  }

  function bluetoothDisconnectReasonLabel(reason) {
    const labels = {
      none: 'None',
      connection_timeout: 'Connection timed out',
      remote_user_terminated: 'Phone disconnected',
      local_host_terminated: 'Timer disconnected',
      unknown: 'Unknown reason',
    };
    return t(labels[reason] || reason || labels.none);
  }

  function bluetoothControlResultLabel(result) {
    const labels = {
      applied: 'Applied',
      invalid_frame: 'Invalid command frame',
      unauthorized: 'Not authenticated',
      stale_boot: 'Stale device boot',
      stale_session: 'Stale timer session',
      invalid_state: 'Not allowed in current state',
      invalid_duration: 'Invalid duration',
      emergency_input_active: 'Emergency input active',
      controls_locked: 'Software controls locked',
      control_disabled: 'Switched off in settings',
      maximum_duration_reached: 'Maximum duration reached',
      unsupported_opcode: 'Unsupported command',
      operation_conflict: 'Request ID conflict',
      system_transition: 'Device transition in progress',
    };
    return t(labels[result] || result || 'None');
  }

  function estimatedRemaining() {
    if (!status) return 0;
    if (status.state !== 'running') return status.remaining_ms;
    return Math.max(0, status.remaining_ms - (performance.now() - receivedAt));
  }

  function renderClock() {
    const healthSecond = Math.floor(performance.now() / 1000);
    if (healthSecond !== lastHealthSecond) {
      lastHealthSecond = healthSecond;
      updateConnectionHealth();
      renderRelayAvailability();
      updateRelayControls(busy || !online || !status, Boolean(status && status.state === 'open'));
    }
    const remaining = estimatedRemaining();
    const fresh = statusIsFresh();
    setText('clock', fresh ? formatTime(remaining) : '—:—:—');
    const base = Math.max(sessionDuration, remaining, 1);
    const progress = status && status.state !== 'open' ? Math.min(360, (remaining / base) * 360) : 0;
    $('timerDial').style.setProperty('--progress', `${progress}deg`);
    $('timerDial').classList.toggle('active', Boolean(status && status.state !== 'open'));
    const completionSecond = Math.ceil(remaining / 1000);
    if (status && status.state !== 'open' && !fresh) {
      lastCompletionSecond = -1;
      setText('completionTime', t('Waiting for current timer status'));
    } else if (status && status.state !== 'open' &&
        (completionSecond !== lastCompletionSecond || status.state !== lastCompletionState)) {
      lastCompletionSecond = completionSecond;
      lastCompletionState = status.state;
      // A share link can end the session first (it fails open at its expiry).
      const linkLimit = status.share_link_limit_ms == null ? null
        : Math.max(0, status.share_link_limit_ms - (performance.now() - receivedAt));
      const clockTime = (ms) => new Date(Date.now() + ms).toLocaleTimeString([], { hour: 'numeric', minute: '2-digit' });
      const completion = remaining <= 0 && status.state === 'running'
        ? t('Waiting for timer confirmation')
        : linkLimit != null && (status.state === 'paused' || linkLimit < remaining)
        ? t('Opens by {time} when the share link expires', { time: clockTime(linkLimit) })
        : status.state === 'paused'
        ? t('Completion time available after resume')
        : t('Opens at {time}', { time: clockTime(remaining) });
      setText('completionTime', completion);
    } else if (!status || status.state === 'open') {
      lastCompletionSecond = -1;
      lastCompletionState = '';
    }
    requestAnimationFrame(renderClock);
  }

  function syncDurationMode() {
    const showEditor = !status || status.state === 'open';
    const random = $('randomMode').checked;
    $('fixedDuration').classList.toggle('hidden', !showEditor || random);
    $('randomFields').classList.toggle('hidden', !showEditor || !random);
    $('durationGestureHint').classList.toggle('hidden', !showEditor);
    document.querySelectorAll('[data-preset]').forEach((button) => {
      const selected = !random && Number(button.dataset.preset) === Number($('hours').value) * 60 + Number($('minutes').value) && Number($('seconds').value) === 0;
      button.classList.toggle('active', selected);
    });
  }

  function setAccessPointBadge(id, security) {
    const secured = security === 'wpa2';
    setText(id, t(secured ? 'WPA2 AP' : 'OPEN AP'));
    $(id).className = `network-security ${secured ? 'secured' : 'open'}`;
  }

  function generatedAccessPointName() {
    if (!status) return t('Generated from device name');
    return `${status.hostname}-${status.device_id}`.toUpperCase();
  }

  function syncAccessPointForm(preserveCustomName = true) {
    const secured = $('apSecurity').value === 'wpa2';
    const generated = $('apUseDeviceName').checked;
    const nameFieldWasDisabled = $('apSsid').disabled;
    if (generated) {
      if (preserveCustomName && !nameFieldWasDisabled) customAccessPointName = $('apSsid').value.trim();
      $('apSsid').value = generatedAccessPointName();
    } else if (nameFieldWasDisabled) {
      $('apSsid').value = customAccessPointName;
    }
    $('apSsid').disabled = generated;
    $('apSsid').required = !generated;
    $('apPasswordFields').classList.toggle('hidden', !secured);
    const canKeep = Boolean(status && status.ap_password_set);
    $('apPassword').required = secured && !canKeep;
    $('apPasswordConfirm').required = secured && !canKeep;
    setText('apPasswordHint', t(canKeep
      ? 'Leave both fields blank to keep the current WPA2 password, or enter a matching new password of 8–63 characters.'
      : 'Enter a matching password of 8–63 printable characters.'));
  }

  function syncNetworkMode(preserveCustomName = true) {
    const accessPointMode = $('apMode').checked;
    $('wifiMode').checked = !accessPointMode;
    $('apModeFields').classList.toggle('hidden', !accessPointMode);
    $('wifiModeFields').classList.toggle('hidden', accessPointMode);
    setText('networkModeHelp', t(accessPointMode
      ? 'The timer creates its own Wi-Fi network. Connect your phone directly to it.'
      : 'The timer joins an existing Wi-Fi network. Connect through that network.'));
    if (accessPointMode) syncAccessPointForm(preserveCustomName);
  }

  function render(next) {
    if (status && next.boot_id && next.boot_id === status.boot_id &&
        Number.isFinite(Number(next.state_revision)) &&
        Number(next.state_revision) < Number(status.state_revision || 0)) {
      return false;
    }
    const previous = status;
    if (previous && previous.boot_id !== next.boot_id) {
      relayCache = null;
      relayFresh = false;
      relaySupported = null;
    }
    status = next;
    online = true;
    connectionFailed = false;
    receivedAt = performance.now();
    $('connectionWarning').classList.add('hidden');
    sessionDuration = next.session_duration_ms || next.remaining_ms || 0;
    if (next.state === 'open') sessionDuration = 0;
    if (previous && previous.state !== 'open' && next.state === 'open' &&
        next.last_release_reason === 'emergency') {
      showStatusToast('Emergency release activated', 'The timer unlocked immediately.');
    } else if (previous && previous.state !== 'open' && next.state === 'open' &&
        next.last_release_reason === 'share_link_expired') {
      showStatusToast('Share link expired', 'The timer unlocked because the link that held it expired.');
    }

    // Someone else may control the timer while a share link is active.
    $('sharedBadge').classList.toggle('hidden', !(Number(next.share_links_active) > 0));
    applySharedView(Number(next.share_links_active) > 0);
    const emergencyActive = Boolean(next.emergency_input_active);
    setText('stateTitle', emergencyActive ? t('Emergency input is active') : '');
    updateNotice?.status({ version: next.version, state: next.state });
    setText('stateBadge', t(emergencyActive ? 'INTERLOCK OPEN' : next.sealed ? 'CONTROLS LOCKED' : next.state.toUpperCase()));
    $('stateBadge').className = `badge ${emergencyActive ? 'emergency' : next.sealed ? 'sealed' : next.state}`;
    document.documentElement.dataset.state = next.sealed ? 'sealed' : next.state;
    document.querySelectorAll('[data-open-only]').forEach((element) => {
      if (element.id !== 'fixedDuration' && element.id !== 'randomFields') element.classList.toggle('hidden', next.state !== 'open');
    });
    document.querySelectorAll('[data-session-only]').forEach((element) => element.classList.toggle('hidden', next.state === 'open'));
    document.querySelectorAll('[data-unsealed-only]').forEach((element) => element.classList.toggle('hidden', next.state === 'open' || next.sealed));
    // Controls the owner switched off are not offered. A session that is
    // already paused keeps its Resume button.
    if (next.pause_allowed === false && next.state !== 'paused') $('pauseButton').classList.add('hidden');
    if (next.control_lock_allowed === false) $('sealButton').classList.add('hidden');
    if (!sessionControlsBusy) {
      $('allowPauseToggle').checked = next.pause_allowed !== false;
      $('allowLockToggle').checked = next.control_lock_allowed !== false;
    }
    $('sealedNotice').classList.toggle('hidden', !next.sealed || next.state === 'open');
    syncDurationMode();
    setText('pauseButton', t(next.state === 'paused' ? 'Resume' : 'Pause'));
    setText('safetyNote', t(emergencyActive
      ? 'The emergency input is active. The lock output is forced open and a timer cannot start.'
      : next.sealed
      ? 'Software controls are locked until expiry. The emergency input still commands release.'
      : next.state === 'open'
        ? 'The lock is unlocked.'
        : 'Local controls and the physical emergency release are available.'));
    const recoveryActive = next.ap_role === 'recovery' ||
      (next.ap_running && next.network_mode === 'wifi' && !next.station_connected);
    const address = next.station_connected
      ? `${next.station_ip} / ${next.hostname}.local`
      : next.ap_running ? `192.168.4.1 (${t(recoveryActive ? 'Recovery AP' : 'AP Mode')})` : t('Reconnecting…');
    setAddress('deviceAddress', address);
    setAddress('panelDeviceAddress', address);
    setText('apName', `${next.ap_ssid}${t(next.ap_running ? ' (available)' : ' (standby)')}`);
    setText('apClients', next.ap_running ? String(next.ap_clients) : '0');
    setAccessPointBadge('apSecurityBadge', next.ap_security);
    ['apName', 'apSecurityBadge', 'apClients'].forEach((id) => {
      $(id).closest('div').classList.toggle('hidden', !next.ap_running);
    });
    setText('firmwareVersion', next.version);
    setText('updateVersion', next.version);
    setText('deviceId', next.device_id.toUpperCase());
    setText('deviceMac', (next.station_mac || '—').toUpperCase());
    setText('deviceBrandName', next.device_name || 'Smart Timer');
    document.title = `${next.device_name || t('Smart Timer')} · ${t('Smart Timer')}`;
    renderBrandSubtitle();
    if (!deviceNameDirty) $('deviceName').value = next.device_name || 'Smart Timer';
    setText('deviceHostname', t('Hostname: {hostname}.local', { hostname: next.hostname }));
    if (!networkModeDirty) {
      $('apMode').checked = !Boolean(next.saved_ssid);
      $('wifiMode').checked = Boolean(next.saved_ssid);
    }
    if (!accessPointDirty) {
      const generatedName = next.ap_uses_device_name !== false;
      $('apUseDeviceName').checked = generatedName;
      if (!generatedName) customAccessPointName = next.ap_ssid || '';
      $('apSsid').value = generatedName ? generatedAccessPointName() : customAccessPointName;
      $('apSecurity').value = next.ap_security === 'wpa2' ? 'wpa2' : 'open';
    }
    syncNetworkMode(false);
    const wifiLabel = next.wifi_state === 'connected'
      ? 'Wi-Fi Mode'
      : next.wifi_state === 'standalone'
        ? 'AP Mode'
        : next.wifi_state === 'connecting'
          ? 'Wi-Fi Mode — connecting'
          : next.wifi_state === 'failed'
            ? next.ap_running ? 'Recovery AP — Wi-Fi failed' : 'Wi-Fi Mode — failed'
            : next.ap_running ? 'Recovery AP' : 'Wi-Fi Mode — reconnecting';
    setText('wifiState', t(wifiLabel));
    const activeNetwork = next.station_connected
      ? next.connected_ssid
      : next.ap_running
        ? next.ap_ssid
        : '—';
    setText('activeNetworkLabel', t(next.station_connected
      ? 'Wi-Fi network label'
      : next.wifi_state === 'standalone'
        ? 'AP Mode network'
        : next.ap_running ? 'Recovery AP' : 'Target network'));
    setText('connectedSsid', activeNetwork);
    setText('wifiSignal', next.station_connected ? `${next.rssi_dbm} dBm` : '—');
    setText('emergencyInput', t(emergencyActive ? 'ACTIVE — start blocked' : 'Ready'));
    const bluetoothSupported = next.chip_family === 'ESP32-C3';
    $('bluetoothSection').classList.toggle('hidden', !bluetoothSupported);
    if (!bluetoothSupported) $('relaySection').classList.add('hidden');
    $('guideBluetoothPairing').classList.toggle('hidden', !bluetoothSupported);
    $('guideBluetoothLed').classList.toggle('hidden', !bluetoothSupported);
    setText('guideShortPressDescription', t(bluetoothSupported
      ? 'While unlocked, enables firmware updates for five minutes. During Bluetooth pairing, it approves the connected app instead.'
      : 'While unlocked, enables firmware updates for five minutes.'));
    setText('guideFactoryResetDescription', t(bluetoothSupported
      ? 'Restores the device name and all Wi-Fi, AP, and Bluetooth settings, then restarts. Installed firmware is unchanged.'
      : 'Restores the device name and all Wi-Fi and AP settings, then restarts. Installed firmware is unchanged.'));
    const retryExplanation = next.station_retry_paused
      ? 'Wi-Fi retries are paused while a device is connected to this recovery network.'
      : 'The timer will keep trying the saved network whenever the recovery network is not in use.';
    const recoveryNetwork = next.saved_ssid ? t('Saved network: {ssid}. ', { ssid: next.saved_ssid }) : '';
    const localizedRetryExplanation = t(retryExplanation);
    setText('recoveryBannerMessage', t('{message} Choose AP Mode to stop Wi-Fi retries.', { message: `${recoveryNetwork}${localizedRetryExplanation}` }));
    setText('recoveryModeMessage', `${recoveryNetwork}${localizedRetryExplanation}`);
    $('recoveryBanner').classList.toggle('hidden', !recoveryActive);
    $('recoveryModeCallout').classList.toggle('hidden', !recoveryActive);
    const unverifiedFirmwareAllowed = Boolean(next.ota_unverified_allowed);
    $('otaUnverifiedBanner').classList.toggle('hidden', !unverifiedFirmwareAllowed);
    $('otaUnverifiedScope').classList.toggle('hidden', !unverifiedFirmwareAllowed);
    window.SmartTimerUpdates.allowance($('otaArmMessage'), next, t);
    setText('otaButton', t(unverifiedFirmwareAllowed ? 'Install unverified firmware' : 'Install update'));
    if (awaitingWifiResult) {
      if (next.wifi_state === 'connecting') {
        setText('wifiMessage', t('Connecting to {ssid}… AP Mode remains available.', { ssid: candidateWifiName }));
      } else if (next.wifi_state === 'connected' && next.connected_ssid === candidateWifiName) {
        awaitingWifiResult = false;
        displayedWifiError = '';
        setText('wifiMessage', t('Connected to {ssid}. AP Mode is closing.', { ssid: next.connected_ssid }));
        showReconnect({
          ssid: next.connected_ssid,
          security: 'Your router Wi-Fi',
          address: next.hostname ? `${next.hostname}.local` : next.station_ip,
          message: t('The timer joined {ssid}. The temporary AP Mode network is closing; reconnect this device to your normal Wi-Fi, then reopen the timer.', { ssid: next.connected_ssid }),
        });
      } else if (next.wifi_state === 'failed') {
        awaitingWifiResult = false;
        setText('wifiMessage', t('Could not connect to {ssid}: {error}. Saved credentials were not replaced.', { ssid: candidateWifiName, error: t(next.last_wifi_error || 'connection failed') }));
      }
    } else if (next.wifi_state === 'connecting' && next.target_ssid) {
      setText('wifiMessage', t('Connecting to {ssid}… AP Mode remains available.', { ssid: next.target_ssid }));
    } else if (next.wifi_state === 'failed' && next.last_wifi_error &&
               displayedWifiError !== `${next.last_attempted_ssid}:${next.last_wifi_error}`) {
      displayedWifiError = `${next.last_attempted_ssid}:${next.last_wifi_error}`;
      const attempted = next.last_attempted_ssid || t('the saved network');
      setText('wifiMessage', t('Last Wi-Fi attempt to {ssid} failed: {error}. Saved credentials were not replaced.', { ssid: attempted, error: t(next.last_wifi_error) }));
    }
    const connectionMode = next.station_connected
      ? 'Wi-Fi Mode'
      : next.wifi_state === 'standalone' && next.ap_running
        ? 'AP Mode'
        : next.ap_running ? 'Recovery AP' : 'Reconnecting';
    setConnection(next.station_connected || next.ap_running ? 'online' : '', connectionMode);
    $('onboardingBanner').classList.toggle('hidden', onboardingDismissed || next.wifi_state !== 'standalone' || Boolean(next.saved_ssid));
    updateControls();
    if (next.chip_family === 'ESP32-C3' && relaySupported !== false && !relayCache &&
        !relayPollHandle && !relayRequestBusy) refreshRelay();
    return true;
  }

  function updateControls() {
    renderRelayAvailability();
    const unavailable = busy || !online || !status;
    const timerOpen = Boolean(status && status.state === 'open');
    $('startButton').disabled = unavailable || Boolean(status.emergency_input_active);
    $('pauseButton').disabled = unavailable || status.sealed;
    $('stopButton').disabled = unavailable || status.sealed;
    $('sealButton').disabled = unavailable || status.sealed || status.state !== 'running';
    $('allowPauseToggle').disabled = $('allowLockToggle').disabled = unavailable || !timerOpen || sessionControlsBusy;
    $('scanButton').disabled = unavailable || !timerOpen;
    $('wifiButton').disabled = unavailable || !timerOpen;
    $('apMode').disabled = unavailable || !timerOpen;
    $('wifiMode').disabled = unavailable || !timerOpen;
    $('apSaveButton').disabled = unavailable || !timerOpen;
    $('deviceNameButton').disabled = unavailable || !timerOpen;
    $('identifyButton').disabled = unavailable || !timerOpen;
    $('restartButton').disabled = unavailable || !timerOpen;
    $('bluetoothToggle').disabled = unavailable || !timerOpen || bluetoothBusy;
    $('bluetoothPairButton').disabled = unavailable || !timerOpen || bluetoothBusy ||
      Boolean(bluetoothCache && !bluetoothCache.pairing_active && bluetoothCache.approved_app_count >= 3);
    $('bluetoothAppList').querySelectorAll('button, input').forEach(element => {
      element.disabled = unavailable || !timerOpen || bluetoothBusy || Boolean(bluetoothCache && bluetoothCache.pairing_active);
    });
    $('bluetoothRevokeButton').disabled = unavailable || !timerOpen || bluetoothBusy ||
      !bluetoothCache || !bluetoothCache.paired;
    updateRelayControls(unavailable, timerOpen);
    $('diagnosticsRefresh').disabled = unavailable;
    $('eventsRefresh').disabled = unavailable;
    $('diagnosticsCopy').disabled = unavailable || !diagnosticsCache;
    $('otaButton').disabled = unavailable || !timerOpen || !status.ota_armed || !$('firmwareFile').files.length;
    $('confirmAccept').disabled = busy;
    document.querySelectorAll('[data-adjust]').forEach((button) => {
      button.disabled = unavailable || status.sealed;
    });
    document.querySelectorAll('.network-option').forEach((button) => {
      button.disabled = unavailable || !timerOpen;
    });
  }

  function setBusy(value) {
    busy = value;
    updateControls();
  }

  async function refresh() {
    const sequence = ++requestSequence;
    try {
      const next = await request('/api/v1/status');
      if (sequence >= latestRenderedSequence) {
        latestRenderedSequence = sequence;
        pollBackoffUntil = 0;
        render(next);
      }
    } catch (error) {
      online = false;
      connectionFailed = true;
      if (error.code === 'ota_in_progress') {
        pollBackoffUntil = Date.now() + (error.retryAfterMs || 30000);
        setConnection('offline', 'Firmware update in progress');
      } else {
        setConnection('offline', 'Device unavailable');
      }
      updateControls();
      updateConnectionHealth();
    }
  }

  function stopPolling() {
    clearTimeout(pollHandle);
    pollHandle = 0;
  }

  function schedulePoll(immediate = false) {
    stopPolling();
    if (liveOpen || $('reconnectDialog').classList.contains('show')) return;
    pollHandle = setTimeout(async () => {
      pollHandle = 0;
      if (!busy && !liveOpen) await refresh();
      if (!liveOpen && !$('reconnectDialog').classList.contains('show')) schedulePoll();
    }, immediate ? 0 : nextPollDelay());
  }

  function closeLiveTransport() {
    const source = liveSource;
    liveSource = null;
    liveOpen = false;
    if (source) {
      source.onopen = null;
      source.onerror = null;
      source.close();
    }
  }

  function clearLiveRetry() {
    clearTimeout(liveRetryHandle);
    liveRetryHandle = 0;
  }

  function scheduleLiveRetry() {
    clearLiveRetry();
    if (liveSuspended || $('reconnectDialog').classList.contains('show')) return;
    liveRetryHandle = setTimeout(() => {
      liveRetryHandle = 0;
      connectLiveUpdates();
    }, 4000 + Math.floor(Math.random() * 3000));
  }

  function useRestFallback(message) {
    closeLiveTransport();
    setText('liveUpdatesMessage', t(message));
    schedulePoll(true);
    scheduleLiveRetry();
  }

  function suspendLiveUpdates(message) {
    liveSuspended = true;
    clearLiveRetry();
    closeLiveTransport();
    stopPolling();
    setText('liveUpdatesMessage', t(message));
  }

  function resumeLiveUpdates() {
    if (!liveSuspended && (liveSource || liveOpen)) return;
    liveSuspended = false;
    schedulePoll(true);
    connectLiveUpdates();
  }

  function connectLiveUpdates() {
    if (liveSuspended || liveSource || $('reconnectDialog').classList.contains('show')) return;
    if (!('EventSource' in window)) {
      liveSuspended = true;
      setText('liveUpdatesMessage', t('This browser does not support live updates. REST polling is still active.'));
      schedulePoll(true);
      return;
    }
    clearLiveRetry();
    setText('liveUpdatesMessage', t('Connecting to a live-update slot…'));
    const source = new EventSource('/api/v1/events/stream');
    liveSource = source;
    source.onopen = () => {
      if (liveSource !== source || liveSuspended) return;
      liveOpen = true;
      stopPolling();
      setText('liveUpdatesMessage', t('Live — REST polling is paused for this page.'));
    };
    source.addEventListener('status', (event) => {
      if (liveSource !== source || liveSuspended) return;
      try {
        const next = JSON.parse(event.data);
        render(next);
      } catch (error) {
        useRestFallback('A live update was invalid. REST polling resumed; reconnecting automatically.');
      }
    });
    source.onerror = () => {
      if (liveSource !== source) return;
      useRestFallback('Live updates disconnected or all slots are in use. REST polling resumed; reconnecting automatically.');
    };
  }

  function showApp() {
    appView.classList.remove('hidden');
    schedulePoll(true);
    connectLiveUpdates();
  }

  async function runAction(action) {
    if (busy) return;
    setBusy(true);
    setText('actionError', '');
    const sequence = ++requestSequence;
    try {
      const next = await action();
      latestRenderedSequence = sequence;
      if (next && typeof next === 'object' && typeof next.remaining_ms === 'number') {
        render(next);
      } else {
        await refresh();
      }
    } catch (error) {
      if (error.code === 'maximum_duration_reached') {
        showDurationLimit();
      } else {
        setText('actionError', error.message);
      }
    } finally {
      setBusy(false);
    }
  }

  $('randomMode').addEventListener('change', syncDurationMode);
  ['hours', 'minutes', 'seconds'].forEach((id) => $(id).addEventListener('input', syncDurationMode));

  function setFixedMinutes(minutes) {
    $('randomMode').checked = false;
    setFixedDurationSeconds(minutes * 60);
  }

  function fixedDurationSeconds() {
    return Number($('hours').value || 0) * 3600 +
      Number($('minutes').value || 0) * 60 +
      Number($('seconds').value || 0);
  }

  function setFixedDurationSeconds(seconds) {
    const requested = Math.round(seconds);
    if (requested > 86400) showDurationLimit();
    const total = Math.max(0, Math.min(86400, requested));
    $('hours').value = Math.floor(total / 3600);
    $('minutes').value = Math.floor((total % 3600) / 60);
    $('seconds').value = total % 60;
    syncDurationMode();
  }

  function adjustFixedDuration(fieldId, steps) {
    const units = { hours: 3600, minutes: 60, seconds: 1 };
    setFixedDurationSeconds(fixedDurationSeconds() + steps * units[fieldId]);
  }

  function durationAdjustmentEnabled() {
    return (!$('randomMode').checked && (!status || status.state === 'open'));
  }

  function randomDurationAdjustmentEnabled() {
    return ($('randomMode').checked && (!status || status.state === 'open'));
  }

  function adjustRandomDuration(fieldId, steps) {
    let minimum = Math.max(1, Math.min(1440, Number($('randomMin').value) || 1));
    let maximum = Math.max(minimum, Math.min(1440, Number($('randomMax').value) || minimum));
    if (fieldId === 'randomMin') minimum = Math.max(1, Math.min(maximum, minimum + steps));
    else {
      if (maximum + steps > 1440) showDurationLimit();
      maximum = Math.max(minimum, Math.min(1440, maximum + steps));
    }
    $('randomMin').value = String(minimum);
    $('randomMax').value = String(maximum);
  }

  function normalizeRandomDuration(fieldId) {
    const requestedMinimum = Number($('randomMin').value) || 1;
    const requestedMaximum = Number($('randomMax').value) || 1;
    if (requestedMinimum > 1440 || requestedMaximum > 1440) showDurationLimit();
    let minimum = Math.max(1, Math.min(1440, requestedMinimum));
    let maximum = Math.max(1, Math.min(1440, requestedMaximum));
    if (minimum > maximum) {
      if (fieldId === 'randomMin') maximum = minimum;
      else minimum = maximum;
    }
    $('randomMin').value = String(minimum);
    $('randomMax').value = String(maximum);
  }

  function wireDurationScrubber(fieldId, enabled, adjust, normalize) {
    const input = $(fieldId);
    const label = input.closest('label');
    let pointerId = null;
    let pointerStartY = 0;
    let appliedSteps = 0;
    let dragged = false;
    let suppressClickUntil = 0;
    let wheelPixels = 0;
    let wheelReset = 0;

    input.addEventListener('pointerdown', (event) => {
      if (!enabled() || (event.button !== undefined && event.button !== 0)) return;
      pointerId = event.pointerId;
      pointerStartY = event.clientY;
      appliedSteps = 0;
      dragged = false;
      if (input.setPointerCapture) input.setPointerCapture(pointerId);
    });
    input.addEventListener('pointermove', (event) => {
      if (pointerId !== event.pointerId || !enabled()) return;
      const nextSteps = Math.trunc((pointerStartY - event.clientY) / 18);
      if (nextSteps === appliedSteps) return;
      event.preventDefault();
      adjust(nextSteps - appliedSteps);
      appliedSteps = nextSteps;
      dragged = true;
      label.classList.add('dragging');
    });
    const finishPointer = (event) => {
      if (pointerId !== event.pointerId) return;
      if (dragged) {
        suppressClickUntil = performance.now() + 300;
        input.blur();
      }
      label.classList.remove('dragging');
      pointerId = null;
    };
    input.addEventListener('pointerup', finishPointer);
    input.addEventListener('pointercancel', finishPointer);
    input.addEventListener('click', (event) => {
      if (performance.now() < suppressClickUntil) event.preventDefault();
    });
    input.addEventListener('wheel', (event) => {
      if (!enabled()) return;
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
    input.addEventListener('change', normalize);
  }

  ['hours', 'minutes', 'seconds'].forEach((fieldId) => wireDurationScrubber(
    fieldId,
    durationAdjustmentEnabled,
    (steps) => adjustFixedDuration(fieldId, steps),
    () => setFixedDurationSeconds(fixedDurationSeconds()),
  ));
  ['randomMin', 'randomMax'].forEach((fieldId) => wireDurationScrubber(
    fieldId,
    randomDurationAdjustmentEnabled,
    (steps) => adjustRandomDuration(fieldId, steps),
    () => normalizeRandomDuration(fieldId),
  ));

  document.querySelectorAll('[data-preset]').forEach((button) => {
    button.addEventListener('click', () => setFixedMinutes(Number(button.dataset.preset)));
  });
  $('resetDuration').addEventListener('click', (event) => {
    setFixedMinutes(15);
    // Clear touch/click focus without taking focus away from keyboard users.
    if (event.detail !== 0) event.currentTarget.blur();
  });

  function openSettings(trigger) {
    settingsReturnFocus = trigger || document.activeElement;
    $('settingsPanel').classList.add('show');
    $('settingsBackdrop').classList.add('show');
    $('settingsPanel').setAttribute('aria-hidden', 'false');
    $('settingsBackdrop').setAttribute('aria-hidden', 'false');
    syncModalState();
    $('settingsClose').focus();
    if (online) {
      setTimeout(async () => {
        if (!diagnosticsCache) await refreshDiagnostics();
        if (!eventsCache) await refreshEvents();
        if (status && status.chip_family === 'ESP32-C3') await refreshBluetooth();
        if (status && status.chip_family === 'ESP32-C3') await refreshRelay();
      }, 0);
    }
  }

  function closeSettings(restoreFocus = true) {
    clearTimeout(bluetoothPairPollHandle);
    bluetoothPairPollHandle = 0;
    $('settingsPanel').classList.remove('show');
    $('settingsBackdrop').classList.remove('show');
    $('settingsPanel').setAttribute('aria-hidden', 'true');
    $('settingsBackdrop').setAttribute('aria-hidden', 'true');
    syncModalState();
    if (restoreFocus && settingsReturnFocus && typeof settingsReturnFocus.focus === 'function') settingsReturnFocus.focus();
  }

  function syncModalState() {
    const settingsOpen = $('settingsPanel').classList.contains('show');
    const guideOpen = $('hardwareGuide').classList.contains('show');
    const confirmationOpen = $('confirmDialog').classList.contains('show');
    const reconnectOpen = $('reconnectDialog').classList.contains('show');
    document.querySelector('.shell').inert = settingsOpen || guideOpen || confirmationOpen || reconnectOpen;
    $('settingsPanel').inert = guideOpen || confirmationOpen || reconnectOpen;
    $('hardwareGuide').inert = confirmationOpen || reconnectOpen;
    $('confirmDialog').inert = reconnectOpen;
    document.body.classList.toggle('panel-open', settingsOpen || guideOpen || confirmationOpen || reconnectOpen);
  }

  function openHardwareGuide(trigger) {
    guideReturnFocus = trigger || document.activeElement;
    $('hardwareGuide').classList.add('show');
    $('hardwareGuideBackdrop').classList.add('show');
    $('hardwareGuide').setAttribute('aria-hidden', 'false');
    $('hardwareGuideBackdrop').setAttribute('aria-hidden', 'false');
    syncModalState();
    $('hardwareGuideClose').focus();
  }

  function closeHardwareGuide(restoreFocus = true) {
    $('hardwareGuide').classList.remove('show');
    $('hardwareGuideBackdrop').classList.remove('show');
    $('hardwareGuide').setAttribute('aria-hidden', 'true');
    $('hardwareGuideBackdrop').setAttribute('aria-hidden', 'true');
    syncModalState();
    if (restoreFocus && guideReturnFocus && typeof guideReturnFocus.focus === 'function') guideReturnFocus.focus();
  }

  function showReconnect({ ssid, security, address, message }) {
    suspendLiveUpdates('Paused during the network restart.');
    closeConfirm(false);
    closeSettings(false);
    reconnectHost = address;
    setText('reconnectMessage', t(message));
    setText('reconnectSsid', ssid || '—');
    setText('reconnectSecurity', t(security === 'wpa2' ? 'WPA2 password required' : security === 'open' ? 'Open — no password' : security));
    setText('reconnectAddress', address);
    $('reconnectDialog').classList.add('show');
    $('reconnectBackdrop').classList.add('show');
    $('reconnectDialog').setAttribute('aria-hidden', 'false');
    $('reconnectBackdrop').setAttribute('aria-hidden', 'false');
    busy = true;
    syncModalState();
    updateControls();
    $('reconnectOpen').focus();
  }

  function closeReconnect() {
    $('reconnectDialog').classList.remove('show');
    $('reconnectBackdrop').classList.remove('show');
    $('reconnectDialog').setAttribute('aria-hidden', 'true');
    $('reconnectBackdrop').setAttribute('aria-hidden', 'true');
    busy = false;
    syncModalState();
    updateControls();
    showApp();
    resumeLiveUpdates();
  }

  function openConfirm({ eyebrow, title, message, safety = '', accept, kind = 'warning', action }) {
    confirmResolve?.(false);
    confirmResolve = null;
    confirmReturnFocus = document.activeElement;
    confirmAction = action;
    setText('confirmEyebrow', t(eyebrow));
    setText('confirmTitle', t(title));
    setText('confirmMessage', t(message));
    if (kind === 'update') window.SmartTimerUpdates.renderNotes($('confirmMessage'), t(message));
    $('confirmMessage').classList.toggle('release-notes',kind==='update');
    setText('confirmSafety', t(safety));
    setText('confirmAccept', t(accept));
    $('confirmSafety').classList.toggle('hidden', !safety);
    $('confirmIcon').className = `confirm-icon ${kind === 'lock' ? 'confirm-lock' : 'confirm-warning'}`;
    $('confirmDialog').classList.add('show');
    $('confirmBackdrop').classList.add('show');
    $('confirmDialog').setAttribute('aria-hidden', 'false');
    $('confirmBackdrop').setAttribute('aria-hidden', 'false');
    syncModalState();
    $('confirmCancel').focus();
  }

  function closeConfirm(restoreFocus = true, accepted = false) {
    confirmResolve?.(accepted);
    confirmResolve = null;
    $('confirmDialog').classList.remove('show');
    $('confirmBackdrop').classList.remove('show');
    $('confirmDialog').setAttribute('aria-hidden', 'true');
    $('confirmBackdrop').setAttribute('aria-hidden', 'true');
    syncModalState();
    if (restoreFocus && confirmReturnFocus && typeof confirmReturnFocus.focus === 'function') confirmReturnFocus.focus();
  }

  function trapFocus(dialog, event) {
    const focusable = [...dialog.querySelectorAll('button:not(:disabled), input:not(:disabled), select:not(:disabled), [href], [tabindex]:not([tabindex="-1"])')]
      .filter((element) => element.offsetParent !== null);
    if (!focusable.length) return;
    const first = focusable[0];
    const last = focusable[focusable.length - 1];
    if (event.shiftKey && document.activeElement === first) {
      event.preventDefault();
      last.focus();
    } else if (!event.shiftKey && document.activeElement === last) {
      event.preventDefault();
      first.focus();
    }
  }

  $('sharedModeToggle').addEventListener('click', () => {
    localControlsExpanded = !localControlsExpanded;
    applySharedView(true);
  });

  ['sharedBadge', 'relayStatusBadge'].forEach(id => $(id).addEventListener('click', (event) => {
    openSettings(event.currentTarget);
    setTimeout(() => $('relaySection').scrollIntoView({ block: 'start', behavior: 'smooth' }), 150);
  }));

  ['settingsOpen', 'footerSettings'].forEach((id) => {
    $(id).addEventListener('click', (event) => openSettings(event.currentTarget));
  });
  $('hardwareGuideOpen').addEventListener('click', (event) => openHardwareGuide(event.currentTarget));
  $('hardwareGuideClose').addEventListener('click', () => closeHardwareGuide());
  $('hardwareGuideBackdrop').addEventListener('click', () => closeHardwareGuide());
  ['connectionShortcut'].forEach((id) => $(id).addEventListener('click', (event) => {
    openSettings(event.currentTarget);
    setTimeout(() => $('networkSection').scrollIntoView({ behavior: 'smooth', block: 'start' }), 50);
  }));
  $('settingsClose').addEventListener('click', closeSettings);
  $('settingsBackdrop').addEventListener('click', closeSettings);
  $('confirmCancel').addEventListener('click', closeConfirm);
  $('confirmBackdrop').addEventListener('click', closeConfirm);
  $('confirmAccept').addEventListener('click', () => {
    const action = confirmAction;
    closeConfirm(false,true);
    confirmAction = null;
    if (action) action();
  });
  $('reconnectOpen').addEventListener('click', () => {
    window.location.assign(`//${reconnectHost}/`);
  });
  $('reconnectClose').addEventListener('click', closeReconnect);
  $('connectionRetry').addEventListener('click', () => {
    setText('connectionWarningMessage', t('Reconnecting…'));
    refresh();
    resumeLiveUpdates();
  });
  $('languageButton').addEventListener('click', () => i18n && i18n.toggleLanguage());
  window.addEventListener('smarttimer:languagechange', () => {
    updateNotice?.refresh();
    applyColorTheme(colorTheme);
    document.title = `${status?.device_name || t('Smart Timer')} · ${t('Smart Timer')}`;
    renderBrandSubtitle();
    lastCompletionSecond = -1;
    if (eventsCache) renderEvents(eventsCache.events || []);
    if (relayCache) renderRelay(relayCache);
    updateConnectionHealth();
  });
  $('themeButton').addEventListener('click', toggleColorTheme);
  window.addEventListener('beforeunload', () => {
    clearLiveRetry();
    clearTimeout(bluetoothPairPollHandle);
    clearTimeout(relayPollHandle);
    closeLiveTransport();
  });
  document.addEventListener('keydown', (event) => {
    if (event.key === 'Escape') {
      if ($('reconnectDialog').classList.contains('show')) closeReconnect();
      else if ($('confirmDialog').classList.contains('show')) closeConfirm();
      else if ($('hardwareGuide').classList.contains('show')) closeHardwareGuide();
      else if ($('settingsPanel').classList.contains('show')) closeSettings();
    } else if (event.key === 'Tab') {
      if ($('reconnectDialog').classList.contains('show')) trapFocus($('reconnectDialog'), event);
      else if ($('confirmDialog').classList.contains('show')) trapFocus($('confirmDialog'), event);
      else if ($('hardwareGuide').classList.contains('show')) trapFocus($('hardwareGuide'), event);
      else if ($('settingsPanel').classList.contains('show')) trapFocus($('settingsPanel'), event);
    }
  });

  $('startForm').addEventListener('submit', (event) => {
    event.preventDefault();
    let values;
    if ($('randomMode').checked) {
      const minimumMinutes = Number($('randomMin').value);
      const maximumMinutes = Number($('randomMax').value);
      if (!Number.isFinite(minimumMinutes) || !Number.isFinite(maximumMinutes) ||
          minimumMinutes <= 0 || maximumMinutes < minimumMinutes || maximumMinutes > 1440) {
        if (minimumMinutes > 1440 || maximumMinutes > 1440) showDurationLimit();
        setText('actionError', t('Choose a random range between 1 minute and 24 hours, with the minimum no greater than the maximum.'));
        return;
      }
      values = {
        min_ms: Math.round(minimumMinutes * 60000),
        max_ms: Math.round(maximumMinutes * 60000),
      };
    } else {
      const seconds = Number($('hours').value) * 3600 + Number($('minutes').value) * 60 + Number($('seconds').value);
      if (!Number.isFinite(seconds) || seconds <= 0 || seconds > 86400) {
        if (seconds > 86400) showDurationLimit();
        setText('actionError', t('Choose a duration between 1 second and 24 hours.'));
        return;
      }
      values = { duration_ms: Math.round(seconds * 1000) };
    }
    runAction(() => timerMutation('/api/v1/timer/start', values));
  });

  $('pauseButton').addEventListener('click', () => runAction(() => {
    const action = status.state === 'paused' ? 'resume' : 'pause';
    return timerMutation(`/api/v1/timer/${action}`);
  }));
  $('stopButton').addEventListener('click', () => runAction(() => timerMutation('/api/v1/timer/release')));

  function openSealDialog() {
    if (!status || status.sealed || status.state !== 'running') return;
    openConfirm({
      eyebrow: 'One-way session setting',
      title: 'Lock software controls?',
      message: 'Pause, time adjustment, and Unlock now will be disabled until this timer expires.',
      safety: 'After this, only the emergency input or removing power can end the session early. Neither is guaranteed to open the lock. Keep an independent way out.',
      accept: 'Lock until expiry',
      kind: 'lock',
      action: () => runAction(() => timerMutation('/api/v1/timer/lock-controls')),
    });
  }

  $('sealButton').addEventListener('click', openSealDialog);
  const saveSessionControls = async (event) => {
    if (sessionControlsBusy) return;
    const toggle = event.currentTarget;
    sessionControlsBusy = true;
    updateControls();
    setText('sessionControlsMessage', t('Saving…'));
    try {
      const saved = await mutate('/api/v1/timer/controls', {
        allow_pause: $('allowPauseToggle').checked,
        allow_control_lock: $('allowLockToggle').checked,
      });
      $('allowPauseToggle').checked = saved.allow_pause !== false;
      $('allowLockToggle').checked = saved.allow_control_lock !== false;
      setText('sessionControlsMessage', t('Settings saved'));
    } catch (error) {
      if (error.status) {
        // The timer answered and refused, so its settings did not change.
        toggle.checked = !toggle.checked;
        setText('sessionControlsMessage', t(error.code === 'timer_active'
          ? 'Unlock the timer first. Session controls can only be changed while it is open.'
          : 'Could not save the setting. Nothing was changed.'));
      } else {
        // No answer: the timer may have saved it. Show what it reports.
        try {
          const current = await request('/api/v1/timer/controls');
          $('allowPauseToggle').checked = current.allow_pause !== false;
          $('allowLockToggle').checked = current.allow_control_lock !== false;
          setText('sessionControlsMessage', t('Could not confirm the save. The switches show what the timer reports.'));
        } catch (_readError) {
          setText('sessionControlsMessage', t('Could not reach the timer. Check these switches once it reconnects.'));
        }
      }
    } finally {
      sessionControlsBusy = false;
      updateControls();
    }
  };
  $('allowPauseToggle').addEventListener('change', saveSessionControls);
  $('allowLockToggle').addEventListener('change', saveSessionControls);
  document.querySelectorAll('[data-adjust]').forEach((button) => {
    button.addEventListener('click', () => runAction(() => timerMutation('/api/v1/timer/adjust', { delta_ms: button.dataset.adjust })));
  });

  function wirePasswordToggle(toggleId, inputIds, showLabel = 'Show', hideLabel = 'Hide') {
    $(toggleId).addEventListener('click', () => {
      const showing = $(inputIds[0]).type === 'text';
      inputIds.forEach((id) => { $(id).type = showing ? 'password' : 'text'; });
      $(toggleId).setAttribute('aria-pressed', String(!showing));
      setText(toggleId, t(showing ? showLabel : hideLabel));
      $(inputIds[0]).focus();
    });
  }

  wirePasswordToggle('wifiPasswordToggle', ['wifiPassword']);
  wirePasswordToggle('apPasswordToggle', ['apPassword', 'apPasswordConfirm'], 'Show both', 'Hide both');

  $('deviceName').addEventListener('input', () => {
    deviceNameDirty = true;
  });

  ['apSsid', 'apPassword', 'apPasswordConfirm'].forEach((id) => {
    $(id).addEventListener('input', () => { accessPointDirty = true; });
  });
  ['apUseDeviceName', 'apSecurity'].forEach((id) => {
    $(id).addEventListener('change', () => {
      accessPointDirty = true;
      syncAccessPointForm();
    });
  });
  ['apMode', 'wifiMode'].forEach((id) => {
    $(id).addEventListener('change', () => {
      networkModeDirty = true;
      if (!$('apMode').checked) {
        $('apPassword').value = '';
        $('apPasswordConfirm').value = '';
        $('apPassword').type = 'password';
        $('apPasswordConfirm').type = 'password';
        $('apPasswordToggle').setAttribute('aria-pressed', 'false');
        setText('apPasswordToggle', t('Show both'));
      }
      syncNetworkMode();
      updateControls();
    });
  });

  $('onboardingSettings').addEventListener('click', (event) => {
    openSettings(event.currentTarget);
    setTimeout(() => $('networkSection').scrollIntoView({ behavior: 'smooth', block: 'start' }), 50);
  });
  $('onboardingDismiss').addEventListener('click', () => {
    onboardingDismissed = true;
    $('onboardingBanner').classList.add('hidden');
  });
  $('recoverySettings').addEventListener('click', (event) => {
    openSettings(event.currentTarget);
    setTimeout(() => $('networkSection').scrollIntoView({ behavior: 'smooth', block: 'start' }), 50);
  });
  $('recoveryUseApMode').addEventListener('click', () => {
    $('apMode').checked = true;
    networkModeDirty = true;
    syncNetworkMode();
    setText('apSettingsMessage', t('Review the AP name and security, then apply AP Mode to stop Wi-Fi retries.'));
    setTimeout(() => $('apSaveButton').focus(), 50);
  });

  $('accessPointForm').addEventListener('submit', async (event) => {
    event.preventDefault();
    if (busy || !status) return;
    const useDeviceName = $('apUseDeviceName').checked;
    const ssid = $('apSsid').value.trim();
    const security = $('apSecurity').value;
    const password = $('apPassword').value;
    const confirmation = $('apPasswordConfirm').value;
    if (!useDeviceName && (!ssid || ssid.length > 32 || /[^\x20-\x7E]/.test(ssid))) {
      setText('apSettingsMessage', t('Enter a Local AP name of 1–32 printable characters.'));
      return;
    }
    const keepPassword = security === 'wpa2' && status.ap_password_set && !password && !confirmation;
    if (security === 'wpa2' && !keepPassword &&
        (password.length < 8 || password.length > 63 || password !== confirmation || /[^\x20-\x7E]/.test(password))) {
      setText('apSettingsMessage', t('Enter the same 8–63 character password in both fields.'));
      return;
    }
    setBusy(true);
    setText('apSettingsMessage', t('Applying AP Mode…'));
    try {
      const result = await mutate('/api/v1/wifi/ap', {
        ssid,
        security,
        password,
        use_device_name: useDeviceName,
        keep_password: keepPassword,
        standalone: true,
      }, 'PUT');
      accessPointDirty = false;
      networkModeDirty = false;
      $('apPassword').value = '';
      $('apPasswordConfirm').value = '';
      $('apPassword').type = 'password';
      $('apPasswordConfirm').type = 'password';
      $('apPasswordToggle').setAttribute('aria-pressed', 'false');
      setText('apPasswordToggle', t('Show both'));
      showReconnect({
        ssid: result.ap_ssid,
        security: result.ap_security,
        address: '192.168.4.1',
        message: t('The timer is restarting in AP Mode. Join {ssid}{password}, then reopen the timer.', {
          ssid: result.ap_ssid,
          password: result.ap_security === 'wpa2' ? t(' with the configured password') : '',
        }),
      });
    } catch (error) {
      setText('apSettingsMessage', t(error.message));
    } finally {
      if (!$('reconnectDialog').classList.contains('show')) setBusy(false);
    }
  });

  $('scanButton').addEventListener('click', async () => {
    if (busy) return;
    setBusy(true);
    setText('wifiMessage', t('Scanning…'));
    try {
      let scan = await mutate('/api/v1/wifi/scan');
      for (let attempt = 0; scan.state === 'scanning' && attempt < 40; attempt += 1) {
        setText('wifiMessage', t('Scanning… You can keep using the timer from another device.'));
        await wait(Math.max(250, Number(scan.retry_in_ms) || 500));
        scan = await request('/api/v1/wifi/scan');
      }
      if (scan.state === 'failed') throw new Error('Wi-Fi scan failed. Try again.');
      if (scan.state !== 'ready') throw new Error('Wi-Fi scan timed out. Try again.');
      const scannedNetworks = scan.networks || [];
      const strongestByName = new Map();
      scannedNetworks.forEach((network) => {
        const existing = strongestByName.get(network.ssid);
        if (network.ssid && (!existing || network.rssi > existing.rssi)) strongestByName.set(network.ssid, network);
      });
      const networks = [...strongestByName.values()].sort((left, right) => right.rssi - left.rssi);
      $('networkList').replaceChildren(...networks.map((network) => {
        const option = document.createElement('option');
        option.value = network.ssid;
        option.label = `${network.rssi} dBm${network.secure ? ' · secured' : ''}`;
        return option;
      }));
      const networkButtons = networks.map((network) => {
          const button = document.createElement('button');
          const name = document.createElement('span');
          const detail = document.createElement('span');
          button.type = 'button';
          button.className = 'network-option';
          name.className = 'network-name';
          name.textContent = network.ssid;
          name.setAttribute('data-i18n-literal', '');
          detail.className = 'network-detail';
          detail.textContent = `${t(network.secure ? 'Secured' : 'Open')} · ${network.rssi} dBm`;
          button.append(name, detail);
          button.addEventListener('click', () => {
            $('wifiSsid').value = network.ssid;
            document.querySelectorAll('.network-option').forEach((item) => {
              const selected = item === button;
              item.classList.toggle('selected', selected);
            });
            setText('wifiMessage', network.secure
              ? t('Selected {ssid}. Enter its password, then connect.', { ssid: network.ssid })
              : t('Selected open network {ssid}. Press connect to continue.', { ssid: network.ssid }));
            (network.secure ? $('wifiPassword') : $('wifiButton')).focus();
          });
          return button;
        });
      $('networkResults').replaceChildren(...networkButtons);
      $('networkResults').classList.toggle('hidden', networkButtons.length === 0);
      setText('wifiMessage', networkButtons.length
        ? t('Select one of {count} networks, then connect.', { count: networkButtons.length })
        : t('No Wi-Fi networks found.'));
    } catch (error) {
      setText('wifiMessage', t(error.message));
    } finally {
      setBusy(false);
    }
  });

  $('wifiForm').addEventListener('submit', async (event) => {
    event.preventDefault();
    if (busy) return;
    const ssid = $('wifiSsid').value.trim();
    displayedWifiError = '';
    setBusy(true);
    setText('wifiMessage', t('Testing {ssid}… Saved credentials will only change after a successful connection.', { ssid }));
    try {
      await mutate('/api/v1/wifi/configure', { ssid, password: $('wifiPassword').value });
      candidateWifiName = ssid;
      awaitingWifiResult = true;
      setText('wifiMessage', t('Connecting to {ssid}… Keep this setup network open until the result appears.', { ssid }));
      $('wifiPassword').value = '';
      $('wifiPassword').type = 'password';
      $('wifiPasswordToggle').setAttribute('aria-pressed', 'false');
      setText('wifiPasswordToggle', t('Show'));
    } catch (error) {
      setText('wifiMessage', t(error.message));
    } finally {
      setBusy(false);
    }
  });

  $('deviceNameForm').addEventListener('submit', async (event) => {
    event.preventDefault();
    if (busy) return;
    const name = $('deviceName').value.trim();
    if (!name || name.length > 32 || /[^\x20-\x7E]/.test(name)) {
      setText('deviceNameMessage', t('Enter a name using 1–32 printable English/ASCII characters.'));
      return;
    }
    const directConnection = !status.station_connected || window.location.hostname === '192.168.4.1';
    const stationName = status.connected_ssid;
    const apUsesDeviceName = status.ap_uses_device_name;
    setBusy(true);
    setText('deviceNameMessage', t('Saving name…'));
    try {
      const result = await mutate('/api/v1/device/name', { name }, 'PUT');
      deviceNameDirty = false;
      showReconnect(directConnection ? {
        ssid: result.ap_ssid,
        security: result.ap_security,
        address: '192.168.4.1',
        message: apUsesDeviceName
          ? t('The timer is restarting with the new generated setup network {ssid}. Rejoin it, then reopen the timer.', { ssid: result.ap_ssid })
          : t('The timer is restarting. Its custom setup network remains {ssid}; reconnect to it, then reopen the timer.', { ssid: result.ap_ssid }),
      } : {
        ssid: stationName || t('Your local Wi-Fi'),
        security: t('Your router Wi-Fi'),
        address: `${result.hostname}.local`,
        message: t('The device name was saved. Your Wi-Fi connection stays the same; reopen the timer at its new local hostname after restart.'),
      });
    } catch (error) {
      setText('deviceNameMessage', t(error.message));
    } finally {
      if (!$('reconnectDialog').classList.contains('show')) setBusy(false);
    }
  });

  $('identifyButton').addEventListener('click', async () => {
    if (busy) return;
    setBusy(true);
    setText('maintenanceMessage', t('Starting the blue identification blink…'));
    try {
      await mutate('/api/v1/system/identify');
      setText('maintenanceMessage', t('The blue LED will blink rapidly for five seconds.'));
    } catch (error) {
      setText('maintenanceMessage', t(error.message));
    } finally {
      setBusy(false);
    }
  });

  async function restartDevice() {
    if (busy) return;
    setBusy(true);
    setText('maintenanceMessage', t('Restarting…'));
    try {
      await mutate('/api/v1/system/restart');
      setText('maintenanceMessage', t('Restart requested. Reconnecting shortly…'));
    } catch (error) {
      setText('maintenanceMessage', t(error.message));
    } finally {
      setBusy(false);
    }
  }

  $('restartButton').addEventListener('click', () => {
    if (busy) return;
    openConfirm({
      eyebrow: 'Maintenance',
      title: 'Restart the device?',
      message: 'Local control will be unavailable briefly while the Smart Timer restarts.',
      safety: 'Restart is available only while the timer output is open.',
      accept: 'Restart',
      action: restartDevice,
    });
  });

  function formatUptime(milliseconds) {
    const totalMinutes = Math.floor(milliseconds / 60000);
    const days = Math.floor(totalMinutes / 1440);
    const hours = Math.floor((totalMinutes % 1440) / 60);
    const minutes = totalMinutes % 60;
    return i18n && i18n.language === 'zh-Hans'
      ? `${days ? `${days}天 ` : ''}${hours}小时 ${minutes}分钟`
      : `${days ? `${days}d ` : ''}${hours}h ${minutes}m`;
  }

  function eventLabel(event) {
    const labels = {
      boot: 'Device started',
      started: 'Timer started',
      remaining_set: 'Remaining time changed',
      paused: 'Timer paused',
      resumed: 'Timer resumed',
      adjusted: 'Timer adjusted',
      controls_locked: 'Software controls locked',
      control_disabled: 'Switched off in settings',
      expired: 'Timer expired',
      opened_by_user: 'Opened from local controls',
      emergency_release: 'Emergency release activated',
      emergency_input_activated: 'Emergency input activated',
      emergency_input_cleared: 'Emergency input cleared',
      adjusted_to_zero: 'Adjustment opened the lock',
      share_link_expired: 'Share link expired; lock opened',
      wifi_connecting: 'Wi-Fi Mode connecting',
      wifi_connected: 'Wi-Fi Mode connected',
      wifi_failed: 'Wi-Fi Mode connection failed',
      wifi_standalone: 'AP Mode started',
      ota_started: 'Firmware update started',
      ota_succeeded: 'Firmware update accepted',
      ota_failed: 'Firmware update rejected',
      restart_requested: 'Restart requested',
      device_renamed: 'Device renamed',
      identify_started: 'Identification blink started',
      access_point_configured: 'AP Mode settings changed',
    };
    let label = t(labels[event.type] || event.type.replaceAll('_', ' '));
    if (event.type === 'started' && event.value_ms > 0) label += ` · ${formatTime(event.value_ms)}`;
    if (event.type === 'adjusted' && event.value_ms) {
      const sign = event.value_ms > 0 ? '+' : '−';
      label += ` · ${sign}${formatTime(Math.abs(event.value_ms))}`;
    }
    if (event.via_share_link) label += ` · ${t('via share link')}`;
    if (event.bluetooth_app_id) {
      const app = (bluetoothCache?.approved_apps || []).find(app => app.app_id === event.bluetooth_app_id);
      label += ` · ${app ? app.label : `App #${event.bluetooth_app_id}`}`;
    }
    return label;
  }

  function renderEvents(events) {
    const diagnosticOnly = new Set(['wifi_connecting', 'wifi_disconnected', 'wifi_dhcp_timeout']);
    const visibleEvents = events.filter((event) => !diagnosticOnly.has(event.type));
    const rows = [...visibleEvents].reverse().map((event) => {
      const row = document.createElement('li');
      const label = document.createElement('span');
      const time = document.createElement('time');
      label.textContent = eventLabel(event);
      time.textContent = `+${formatTime(event.at_ms)}`;
      row.append(label, time);
      return row;
    });
    if (!rows.length) {
      const empty = document.createElement('li');
      empty.className = 'event-empty';
      empty.textContent = t('No activity has been recorded during this boot.');
      rows.push(empty);
    }
    $('eventTimeline').replaceChildren(...rows);
    return visibleEvents.length;
  }

  async function refreshEvents() {
    if (busy) return;
    setBusy(true);
    setText('eventsMessage', t('Reading recent activity…'));
    try {
      eventsCache = await request('/api/v1/events');
      const visibleCount = renderEvents(eventsCache.events || []);
      setText('eventsMessage', t('{count} activity items shown; low-level network events remain available through the JSON API.', { count: visibleCount }));
    } catch (error) {
      setText('eventsMessage', t(error.message));
    } finally {
      setBusy(false);
    }
  }

  async function refreshDiagnostics() {
    if (busy) return;
    setBusy(true);
    setText('diagnosticsMessage', t('Reading diagnostics…'));
    try {
      diagnosticsCache = await request('/api/v1/system/diagnostics');
      setText('diagnosticHardwareModel', diagnosticsCache.hardware_model || '—');
      setText('diagnosticChipFamily', diagnosticsCache.chip_family || '—');
      setText('diagnosticBuildId', diagnosticsCache.build_id || '—');
      setText('diagnosticUptime', formatUptime(diagnosticsCache.uptime_ms));
      setText('diagnosticHeap', formatBytes(diagnosticsCache.free_heap_bytes));
      setText('diagnosticMinimumHeap', formatBytes(diagnosticsCache.minimum_observed_free_heap_bytes));
      setText('diagnosticLargestBlock', formatBytes(diagnosticsCache.max_free_block_bytes));
      setText('diagnosticFragmentation', `${diagnosticsCache.heap_fragmentation_percent}%`);
      setText('diagnosticFirmwareSize', formatBytes(diagnosticsCache.sketch_size_bytes));
      setText('diagnosticOtaSpace', formatBytes(diagnosticsCache.free_sketch_space_bytes));
      setText('diagnosticFlashSize', formatBytes(diagnosticsCache.flash_size_bytes));
      setText('diagnosticFlashLayout', diagnosticsCache.flash_layout || '—');
      setText('diagnosticSseClients', `${diagnosticsCache.sse_clients} / ${diagnosticsCache.sse_client_limit}`);
      setText('diagnosticSseHistory', t('{accepted} opened / {closed} closed', {
        accepted: diagnosticsCache.sse_accepted_count || 0,
        closed: diagnosticsCache.sse_closed_count || 0,
      }));
      setText('diagnosticSseClose', streamCloseReasonLabel(diagnosticsCache.sse_last_close_reason));
      setText('diagnosticReset', t(diagnosticsCache.reset_reason));
      setText('diagnosticEvents', String(diagnosticsCache.event_count));
      setText('diagnosticsMessage', t('Diagnostics are current.'));
    } catch (error) {
      setText('diagnosticsMessage', t(error.message));
    } finally {
      setBusy(false);
    }
  }

  let bluetoothAppsSignature = '';
  function renderBluetoothApps(payload) {
    const apps = payload.approved_apps || [];
    $('bluetoothAppsFull').classList.toggle('hidden', apps.length < 3);
    $('bluetoothNewAppName').classList.toggle('hidden', apps.length >= 3);
    setText('bluetoothAppCount', `${apps.length} / ${payload.approved_app_limit || 3}`);
    const signature = JSON.stringify(apps.map(app => [app.app_id, app.label])) + t('Save name');
    const list = $('bluetoothAppList');
    if (signature !== bluetoothAppsSignature) {
      bluetoothAppsSignature = signature;
      list.replaceChildren();
      for (const app of apps) {
        const row = document.createElement('div');
        row.className = 'bluetooth-app-row';
        row.dataset.appId = String(app.app_id);
        const input = document.createElement('input');
        input.type = 'text'; input.maxLength = 24; input.value = app.label;
        input.setAttribute('aria-label', t('Device name'));
        const seen = document.createElement('small');
        seen.className = 'helper app-last-seen';
        const actions = document.createElement('div');
        actions.className = 'bluetooth-app-actions';
        const button = (label, action) => {
          const node = document.createElement('button');
          node.type = 'button'; node.className = 'secondary'; node.textContent = t(label);
          node.addEventListener('click', action); actions.append(node);
        };
        button('Save name', () => updateBluetoothApp('/api/v1/bluetooth/apps/label',
          { app_id: app.app_id, label: input.value.trim() }, 'PUT'));
        button('Replace', () => openConfirm({
          eyebrow: 'Bluetooth', title: 'Replace this device pairing?',
          message: t('Only {name} will be replaced after the new app is saved successfully.', { name: app.label }),
          safety: 'Connect from the new app, then briefly press Reset. The setup key transfers automatically.',
          accept: 'Start pairing', action: () => pairBluetoothApp(app.app_id, input.value.trim()),
        }));
        button('Remove', () => openConfirm({
          eyebrow: 'Bluetooth', title: 'Remove this device pairing?',
          message: t('Remove Bluetooth approval for {name}? Other apps will keep their access.', { name: app.label }),
          safety: 'You can pair again through this page or with the physical Reset-button flow.',
          accept: 'Remove pairing', action: () => updateBluetoothApp('/api/v1/bluetooth/apps/revoke', { app_id: app.app_id }),
        }));
        row.append(input, seen, actions); list.append(row);
      }
    }
    for (const app of apps) {
      const row = list.querySelector(`[data-app-id="${Number(app.app_id)}"]`);
      if (row) row.querySelector('.app-last-seen').textContent = t(app.connected ? 'Connected'
        : app.last_seen_at_ms != null ? 'Seen this boot' : 'Not seen since restart');
    }
  }

  async function updateBluetoothApp(path, fields, method = 'POST') {
    if (bluetoothBusy) return;
    bluetoothBusy = true; updateControls();
    try {
      renderBluetooth(await mutate(path, fields, method));
      setText('bluetoothMessage', t('Paired devices updated.'));
    } catch (error) { setText('bluetoothMessage', t(error.message)); }
    finally { bluetoothBusy = false; updateControls(); }
  }

  function renderBluetooth(payload) {
    const previousPairingActive = Boolean(bluetoothCache && bluetoothCache.pairing_active);
    const previousPairingState = bluetoothCache && bluetoothCache.pairing_state;
    bluetoothCache = payload;
    renderBluetoothApps(payload);
    const enabled = Boolean(payload && payload.enabled);
    const connected = Boolean(payload && payload.connected);
    setText('bluetoothState', t(!enabled ? 'Off' : connected ? 'Connected' : payload.advertising ? 'Advertising' : 'Starting'));
    setText('bluetoothName', payload && payload.advertised_name ? payload.advertised_name : '—');
    setText('bluetoothConnections', `${Number(payload && payload.connections) || 0} / ${Number(payload && payload.connection_limit) || 1}`);
    setText('bluetoothRssi', connected && Number.isFinite(Number(payload.connection_rssi_dbm))
      ? `${payload.connection_rssi_dbm} dBm` : '—');
    setText('bluetoothMtu', connected && payload.connection_mtu ? String(payload.connection_mtu) : '—');
    setText('bluetoothMtuStatus', !connected ? '—'
      : t(payload.connection_mtu_sufficient ? 'Ready' : 'Needs MTU 35'));
    setText('bluetoothNotifications', enabled ? String(payload && payload.notification_count || 0) : '—');
    const pairingState = payload && payload.pairing_state;
    const pairingLabel = payload && payload.pairing_active
      ? pairingState === 'waiting_for_button' ? 'Press Reset to approve'
        : pairingState === 'secret_ready' ? 'Ready for app'
          : pairingState === 'waiting_for_commit' ? 'Waiting for app to save'
            : pairingState === 'committing' ? 'Saving approval…'
          : pairingState === 'securing_connection' ? 'Securing connection…'
            : 'Waiting for app…'
      : payload && payload.paired ? 'Device paired' : 'None';
    setText('bluetoothPairingStatus', t(pairingLabel));
    const pairingSeconds = Math.max(0, Math.ceil((Number(payload && payload.pairing_remaining_ms) || 0) / 1000));
    $('bluetoothPairingRemainingRow').classList.toggle('hidden', !Boolean(payload && payload.pairing_active));
    setText('bluetoothPairingRemaining', pairingSeconds ? t('{seconds}s remaining', { seconds: pairingSeconds }) : '—');
    setText('bluetoothPairButton', t(payload && payload.pairing_active
      ? 'Cancel pairing'
      : 'Pair device'));
    $('bluetoothPairButton').classList.toggle('cancel-pairing', Boolean(payload && payload.pairing_active));
    const securityLabel = !connected ? 'Not connected'
      : payload.control_authenticated ? 'Authenticated'
        : payload.connection_encrypted && payload.connection_bonded ? 'Encrypted and bonded'
          : payload.connection_encrypted ? 'Encrypted' : 'Not secured';
    setText('bluetoothSecurity', t(securityLabel));
    setText('bluetoothAuthenticated', t(payload && payload.control_authenticated ? 'Yes' : 'No'));
    setText('bluetoothControlCommands', String(payload && payload.control_command_count || 0));
    setText('bluetoothControlResult', payload && payload.control_command_count
      ? bluetoothControlResultLabel(payload.control_last_result) : t('None'));
    setText('bluetoothDisconnect', enabled
      ? bluetoothDisconnectReasonLabel(payload && payload.last_disconnect_reason_name)
      : '—');
    setText('bluetoothLastError', enabled && payload && payload.last_error
      ? `${payload.last_error_name || 'unknown'} (${payload.last_error})`
      : t('None'));
    const backoffMs = Number(payload && payload.control_authentication_backoff_remaining_ms) || 0;
    setText('bluetoothAuthRecovery', backoffMs > 0
      ? t('{seconds}s remaining', { seconds: Math.ceil(backoffMs / 1000) })
      : t('Ready'));
    setText('bluetoothBondCleanup', payload && payload.bond_cleanup_pending
      ? t('Retrying') : t('Complete'));
    $('bluetoothToggle').checked = Boolean(payload && payload.configured_enabled);
    $('bluetoothRevokeButton').classList.toggle('hidden', !Boolean(payload && payload.paired));
    if (previousPairingActive && !payload.pairing_active) {
      const completed = payload.pairing_state === 'complete';
      const failed = payload.pairing_state === 'failed';
      setText('bluetoothMessage', t(completed
        ? 'App paired successfully. Pairing is now closed.'
        : failed
          ? 'Pairing failed. The previously approved app was not changed.'
          : 'Pairing canceled. The previously approved app was not changed.'));
    } else if (previousPairingState === 'committing' &&
               payload.pairing_state === 'failed') {
      setText('bluetoothMessage', t('Pairing failed. The previously approved app was not changed.'));
    }
    updateControls();
    if ($('settingsPanel').classList.contains('show')) {
      scheduleBluetoothPairPoll();
    }
  }

  function scheduleBluetoothPairPoll() {
    clearTimeout(bluetoothPairPollHandle);
    if (!bluetoothCache ||
        !$('settingsPanel').classList.contains('show')) {
      bluetoothPairPollHandle = 0;
      return;
    }
    bluetoothPairPollHandle = setTimeout(async () => {
      bluetoothPairPollHandle = 0;
      if (busy || bluetoothBusy || liveSuspended || document.hidden || !online) {
        scheduleBluetoothPairPoll();
        return;
      }
      try {
        const next = await request('/api/v1/bluetooth');
        renderBluetooth(next);
        if (next.pairing_state === 'waiting_for_button') {
          setText('bluetoothMessage', t('App connected. Briefly press Reset to approve it.'));
        } else if (next.pairing_state === 'secret_ready') {
          setText('bluetoothMessage', t('Approved. Waiting for the app to finish securely.'));
        } else if (next.pairing_state === 'waiting_for_commit' ||
                   next.pairing_state === 'committing') {
          setText('bluetoothMessage', t('The app received its key. Waiting for secure storage confirmation.'));
        }
      } catch (error) {
        setText('bluetoothMessage', t(error.message));
        scheduleBluetoothPairPoll();
      }
    }, (bluetoothCache.pairing_active ? 700 : 5000) + Math.floor(Math.random() * 301));
  }

  async function refreshBluetooth() {
    if (!status || status.chip_family !== 'ESP32-C3') return;
    try {
      const bluetooth = await request('/api/v1/bluetooth');
      renderBluetooth(bluetooth);
      setText('bluetoothMessage', t('Bluetooth diagnostics are current.'));
    } catch (error) {
      setText('bluetoothMessage', t(error.message));
    }
  }

  $('bluetoothToggle').addEventListener('change', async (event) => {
    if (bluetoothBusy) return;
    const requestedEnabled = event.currentTarget.checked;
    bluetoothBusy = true;
    updateControls();
    setText('bluetoothMessage', t(requestedEnabled
      ? 'Turning on Bluetooth…'
      : 'Turning off Bluetooth…'));
    try {
      renderBluetooth(await request(requestedEnabled
        ? '/api/v1/bluetooth/enable'
        : '/api/v1/bluetooth/disable'));
      if (!requestedEnabled) {
        clearTimeout(bluetoothPairPollHandle);
        bluetoothPairPollHandle = 0;
      }
      await wait(400);
      await refreshBluetooth();
    } catch (error) {
      event.currentTarget.checked = !requestedEnabled;
      setText('bluetoothMessage', t(error.message));
    } finally {
      bluetoothBusy = false;
      updateControls();
    }
  });

  async function pairBluetoothApp(replaceId = 0, label = $('bluetoothAppLabel').value.trim()) {
    if (bluetoothBusy) return;
    bluetoothBusy = true;
    updateControls();
    setText('bluetoothMessage', t('Opening Bluetooth pairing…'));
    try {
      const fields = {};
      if (label) fields.label = label;
      if (replaceId) fields.replace_app_id = replaceId;
      const next = await mutate('/api/v1/bluetooth/pairing/start', fields);
      renderBluetooth(next);
      setText('bluetoothMessage', t('Pairing is open for two minutes. Connect from your app, then briefly press Reset to approve it.'));
    } catch (error) {
      setText('bluetoothMessage', t(error.message));
    } finally {
      bluetoothBusy = false;
      updateControls();
    }
  }

  async function cancelBluetoothPairing() {
    if (bluetoothBusy) return;
    bluetoothBusy = true;
    updateControls();
    setText('bluetoothMessage', t('Canceling Bluetooth pairing…'));
    try {
      renderBluetooth(await mutate('/api/v1/bluetooth/pairing/cancel'));
      setText('bluetoothMessage', t('Pairing canceled. The previously approved app was not changed.'));
    } catch (error) {
      setText('bluetoothMessage', t(error.message));
    } finally {
      bluetoothBusy = false;
      updateControls();
    }
  }

  async function revokeBluetoothApp() {
    if (bluetoothBusy) return;
    bluetoothBusy = true;
    updateControls();
    try {
      renderBluetooth(await mutate('/api/v1/bluetooth/pairing/revoke'));
      setText('bluetoothMessage', t('All approved Bluetooth apps removed.'));
    } catch (error) {
      setText('bluetoothMessage', t(error.message));
    } finally {
      bluetoothBusy = false;
      updateControls();
    }
  }

  $('bluetoothPairButton').addEventListener('click', () => {
    if (bluetoothCache && bluetoothCache.pairing_active) {
      cancelBluetoothPairing();
      return;
    }
    pairBluetoothApp();
  });

  $('bluetoothRevokeButton').addEventListener('click', () => {
    openConfirm({
      eyebrow: 'Bluetooth',
      title: 'Remove all approved apps?',
      message: 'All approved apps will lose Bluetooth control. This does not unlock or change the timer.',
      safety: 'You can pair again through this page or with the physical Reset-button flow.',
      accept: 'Remove app',
      action: revokeBluetoothApp,
    });
  });

  // --- Experimental remote relay (V2 only; hidden when /api/v1/relay is absent) ---
  // Header subtitle: the timer's address normally; who can control it while
  // a share link is live.
  function renderBrandSubtitle() {
    const shared = Boolean(status && Number(status.share_links_active) > 0);
    setText('brandSubtitle', shared ? t('Shared control')
      : status && status.hostname ? `${status.hostname}.local` : t('Smart Timer'));
  }

  // Share mode: while a link is live, tuck local controls away (a half step
  // toward a view-only page). Unlock now and physical releases stay.
  function applySharedView(shared) {
    if (!shared) localControlsExpanded = false;
    document.documentElement.classList.toggle('shared-view', shared);
    document.documentElement.classList.toggle('local-expanded', shared && localControlsExpanded);
    $('sharedModeCard').classList.toggle('hidden', !shared);
    setText('sharedModeToggle', t(localControlsExpanded ? 'Hide controls' : 'Show controls'));
    $('sharedModeToggle').setAttribute('aria-expanded', String(localControlsExpanded));
  }

  function relayError(error) {
    const code = error && error.code;
    return t(relayMessages[code] || (error && error.message) || 'Request failed');
  }

  function relayStateLabel(relay) {
    if (relay.setup_pending) return 'Waiting for physical approval';
    if (relay.enrolled && !relay.partner_origin) return 'Approve remote sharing setup';
    if (!relay.enrolled) return relay.enrolling ? 'Setting up…' : 'Not set up';
    if (relay.paused_for_bluetooth) return 'Paused for Bluetooth';
    if (relay.stopping) return 'Stopping…';
    if (!relay.enabled) return 'Off';
    if (relay.state === 'connected') return relay.sharing_available ? 'Connected' : 'Connected, busy';
    if (relay.state === 'connecting' || relay.state === 'authenticating') return 'Connecting…';
    return 'Waiting to reconnect';
  }

  function relayStatusIsFresh() {
    return Boolean(relayFresh && online && performance.now() - relayReceivedAt < 15000);
  }

  function relayAvailability(relay, fresh) {
    if (!relay || !(relay.enrolled || relay.enrolling)) return null;
    if (!fresh) return { label: 'RELAY UNAVAILABLE', kind: 'stale', detail: 'Relay status is unavailable.' };
    if (relay.paused_for_bluetooth) return { label: 'RELAY PAUSED', kind: 'paused', detail: 'Paused for Bluetooth' };
    if (relay.enrolling || relay.stopping || relay.maintenance) return { label: 'RELAY BUSY', kind: 'paused', detail: 'The relay is busy. Try again in a moment.' };
    if (!relay.enabled) return { label: 'RELAY OFF', kind: 'offline', detail: 'Remote sharing is off.' };
    if (relay.state === 'connected') return relay.sharing_available
      ? { label: 'RELAY ONLINE', kind: 'open', detail: 'Connected' }
      : { label: 'RELAY BUSY', kind: 'paused', detail: 'Connected, busy' };
    return { label: 'RELAY OFFLINE', kind: 'stale', detail: 'Relay offline. Remote control is unavailable until it reconnects.' };
  }

  function renderRelayAvailability() {
    const availability = relayAvailability(relayCache, relayStatusIsFresh());
    const badge = $('relayStatusBadge');
    badge.classList.toggle('hidden', !availability);
    if (!availability) return;
    setText('relayStatusBadge', t(availability.label));
    badge.className = `badge relay ${availability.kind}`;
    badge.title = t(availability.detail);
    badge.setAttribute('aria-label', t('{status}. Open remote sharing settings.', { status: t(availability.label) }));
    if (!relayStatusIsFresh()) {
      setText('relayState', t('Relay status is unavailable.'));
      setText('relayBadge', t('UNKNOWN'));
      $('relayBadge').classList.add('inactive');
      $('relayBadge').classList.remove('scanning');
    }
  }

  function relayExpiry(link) {
    const remaining = Number(link.expires_in_ms) - (performance.now() - relayLinksAt);
    return remaining > 0 ? t('Expires in {time}', { time: formatTime(remaining) }) : t('Expired');
  }

  // Accept "relay.example.com" or a full HTTPS origin; the timer validates it.
  function relayOrigin(value) {
    const trimmed = value.trim().replace(/\/+$/, '');
    return /^[a-z][a-z0-9+.-]*:/i.test(trimmed) ? trimmed : `https:\/\/${trimmed}`;
  }

  function rememberRelayLink(label, ttl, fpHash = '') {
    relayLastLink = { label, ttl: Number(ttl), fp: fpHash };
    try {
      sessionStorage.setItem('smartTimerRelayLastLink', JSON.stringify(relayLastLink));
    } catch (error) {
      // Private browsing: renew still works for the rest of this page visit.
    }
  }

  function relayDurationText(seconds) {
    const hours = Math.floor(seconds / 3600);
    const minutes = Math.round((seconds % 3600) / 60);
    if (hours && minutes) return t('{hours} hr {minutes} min', { hours, minutes });
    return hours ? t('{hours} hr', { hours }) : t('{minutes} min', { minutes });
  }

  async function copyText(text) {
    if (navigator.clipboard && window.isSecureContext) {
      await navigator.clipboard.writeText(text);
      return;
    }
    const field = document.createElement('textarea');
    field.value = text;
    field.style.position = 'fixed';
    field.style.opacity = '0';
    document.body.append(field);
    field.select();
    const copied = document.execCommand('copy');
    field.remove();
    if (!copied) throw new Error('copy failed');
  }

  function renderRelayLinks() {
    const list = $('relayLinkList');
    const signature = JSON.stringify(relayLinks.map(link => [link.fp_hash, link.label, link.state, link.url_available]))
      + t('Revoke');
    if (signature !== relayLinksSignature) {
      relayLinksSignature = signature;
      list.replaceChildren();
      for (const link of relayLinks) {
        const row = document.createElement('div');
        row.className = 'relay-link-row';
        row.dataset.fp = link.fp_hash;
        const heading = document.createElement('div');
        heading.className = 'relay-link-heading';
        const name = document.createElement('strong');
        name.textContent = link.label || t('Unnamed link');
        const chip = document.createElement('span');
        chip.className = `relay-chip relay-chip-${link.state}`;
        chip.textContent = t(link.state === 'ready' ? 'Active' : link.state === 'pending' ? 'Confirming…'
          : link.state === 'revoking' ? 'Ending…' : link.state === 'error' ? 'Relay error' : 'Unknown');
        heading.append(name, chip);
        const expiry = document.createElement('small');
        expiry.className = 'helper relay-link-expiry';
        const actions = document.createElement('div');
        actions.className = 'bluetooth-app-actions';
        const button = (label, action, className = 'secondary') => {
          const node = document.createElement('button');
          node.type = 'button'; node.className = className; node.textContent = t(label);
          node.addEventListener('click', action); actions.append(node);
          return node;
        };
        if (link.url_available && link.state === 'ready') {
          button('Show link', () => showRelayShareUrl(link.fp_hash, link.label));
        }
        button('Renew', () => openConfirm({
          eyebrow: 'Remote sharing', title: 'Renew this share link?',
          message: 'The current link stops working now and a new link replaces it. Send the new link to your partner.',
          safety: 'Renewing never unlocks or changes the timer.',
          accept: 'Renew link', action: () => renewRelayLink(link.fp_hash, link.label),
        }), 'secondary relay-renew-action');
        button('Revoke', () => openConfirm({
          eyebrow: 'Remote sharing', title: 'Revoke this share link?',
          message: t('{name} loses control immediately. You can create a new link afterwards.', { name: link.label || t('This partner') }),
          safety: 'Revoking works during a session and never unlocks or changes the timer.',
          accept: 'Revoke link', action: () => revokeRelayLink(link.fp_hash),
        }), 'secondary relay-revoke');
        // Extend keeps the same URL; it also works during a session.
        const extend = document.createElement('div');
        extend.className = 'relay-extend';
        const extendLabel = document.createElement('span');
        extendLabel.textContent = t('Extend');
        extend.append(extendLabel);
        for (const [label, seconds] of [['+15 min', 900], ['+1 hr', 3600], ['+4 hr', 14400]]) {
          const node = document.createElement('button');
          node.type = 'button'; node.className = 'secondary relay-extend-action'; node.textContent = t(label);
          node.addEventListener('click', () => extendRelayLink(link.fp_hash, seconds));
          extend.append(node);
        }
        row.append(heading, expiry, actions, extend);
        list.append(row);
      }
    }
    list.querySelectorAll('.relay-link-row').forEach((row) => {
      const link = relayLinks.find(item => item.fp_hash === row.dataset.fp);
      if (link) row.querySelector('.relay-link-expiry').textContent = relayExpiry(link);
    });
    const renewable = relayLinks.length === 0 && Boolean(relayLastLink);
    $('relayRenewCard').classList.toggle('hidden', !renewable);
    if (renewable) {
      setText('relayRenewLabel', relayLastLink.label || t('Unnamed link'));
      const sameUrl = Boolean(relayLastLink.fp);
      setText('relayRenewText', t(sameUrl
        ? 'This link ended. Reactivate keeps the same link for {duration}; New link sends a fresh one.'
        : 'This link ended. New link issues a fresh link for {duration}.',
        { duration: relayDurationText(relayLastLink.ttl) }));
      $('relayReactivateButton').classList.toggle('hidden', !sameUrl);
    }
    $('relayLinksEmpty').classList.toggle('hidden', relayLinks.length > 0 || renewable);
    $('relayCreateForm').classList.toggle('hidden', relayLinks.length > 0);
  }

  function renderRelay(relay) {
    relayCache = relay;
    const enrolled = Boolean(relay.enrolled);
    setText('relayState', t(relayStateLabel(relay)));
    setText('relayHost', relay.relay_host || '—');
    setText('relayLinkCount', t(Number(relay.live_links) > 0 ? 'Active' : 'None'));
    const badge = $('relayBadge');
    const live = enrolled && relay.enabled && relay.state === 'connected' && !relay.paused_for_bluetooth;
    badge.textContent = t(relay.paused_for_bluetooth ? 'PAUSED' : live ? 'ON' : 'OFF');
    badge.classList.toggle('inactive', !live && !relay.paused_for_bluetooth);
    badge.classList.toggle('scanning', Boolean(relay.paused_for_bluetooth));
    $('relayPausedNotice').classList.toggle('hidden', !relay.paused_for_bluetooth);
    const setupPending = Boolean(relay.setup_pending);
    const configured = enrolled && Boolean(relay.partner_origin);
    $('relayEnrollForm').classList.toggle('hidden', configured || Boolean(relay.enrolling) || setupPending);
    $('relaySetupApproval').classList.toggle('hidden', !setupPending);
    setText('relayApprovalRelay', relay.setup_relay_origin || '—');
    setText('relayApprovalPartner', relay.setup_partner_origin || '—');
    setText('relayApprovalCountdown', t('Approval expires in {seconds} seconds.',
      { seconds: Math.max(0, Math.ceil(Number(relay.setup_remaining_ms || 0) / 1000)) }));
    $('relayManage').classList.toggle('hidden', !configured);
    $('relayToggle').checked = Boolean(relay.enabled);
    setText('relayTimerId', relay.timer_id || '—');
    setText('relayReconnects', String(Number(relay.reconnects) || 0));
    setText('relayLastError', relay.last_error ? relayError({ code: relay.last_error, message: relay.last_error }) : t('None'));
    setText('relayMemory', relay.running
      ? t('{heap} free, stack reserve {stack}', { heap: formatBytes(relay.free_heap_bytes), stack: formatBytes(relay.task_stack_min_free_bytes) })
      : '—');
    if (!enrolled || !relay.enabled) { relayLinks = []; }
    // Link changes are not timer events, so the status stream may report them
    // up to 15 s later; reflect the relay's own count on the page at once.
    if (status && 'share_links_active' in status) {
      status.share_links_active = relay.enabled ? Number(relay.live_links) || 0 : 0;
      $('sharedBadge').classList.toggle('hidden', !(status.share_links_active > 0));
      applySharedView(status.share_links_active > 0);
      renderBrandSubtitle();
    }
    renderRelayLinks();
    updateControls();
  }

  function relaySelectedTtl() {
    if (!relayTtlCustom) return relayTtl;
    const hours = Number($('relayCustomHours').value) || 0;
    const minutes = Number($('relayCustomMinutes').value) || 0;
    return Math.round(hours * 3600 + minutes * 60);
  }

  // The firmware accepts 60 s to 24 h. Show when the link would end.
  function renderRelayDuration() {
    document.querySelectorAll('[data-relay-ttl]').forEach((button) => {
      const selected = relayTtlCustom ? button.dataset.relayTtl === 'custom'
        : Number(button.dataset.relayTtl) === relayTtl;
      button.classList.toggle('active', selected);
      button.setAttribute('aria-checked', String(selected));
    });
    $('relayCustomDuration').classList.toggle('hidden', !relayTtlCustom);
    const ttl = relaySelectedTtl();
    const valid = ttl >= 60 && ttl <= 86400;
    const locale = i18n && i18n.language === 'zh-Hans' ? 'zh-CN' : undefined;
    setText('relayDurationHint', valid
      ? t('Ends around {time} unless revoked.', { time: new Date(Date.now() + ttl * 1000)
        .toLocaleString(locale, { weekday: 'short', hour: '2-digit', minute: '2-digit' }) })
      : t('Choose between 1 minute and 24 hours.'));
    return valid;
  }

  function relayCreateBlock() {
    const relay = relayCache;
    if (!relay || !relayStatusIsFresh()) return 'unavailable';
    if (relay.paused_for_bluetooth) return 'paused';
    if (!relay.enrolled || !relay.enabled || !relay.partner_origin) return 'off';
    if (relay.setup_pending || relay.maintenance || relay.stopping || relay.enrolling) return 'busy';
    if (!status || status.state !== 'open') return 'open';
    if (!relay.sharing_available) return 'offline';
    if ((Number(relay.live_links) || 0) >= (Number(relay.link_limit) || 1)) return 'full';
    return '';
  }

  function updateRelayControls(unavailable, timerOpen) {
    const relay = relayCache;
    const disabled = unavailable || relayBusy || !relay;
    const blocked = relayCreateBlock();
    let renewBlocked = true;
    ['relaySetupCode', 'relayUrl', 'relayPartnerOrigin', 'relayCode', 'relayPin', 'relayEnrollButton'].forEach((id) => {
      $(id).disabled = disabled || !timerOpen || Boolean(relay && relay.setup_pending);
    });
    // Cancellation stays available while the enrollment action is polling.
    $('relaySetupCancel').disabled = unavailable || !relay || !relay.setup_pending;
    // Turning sharing off works during a session; turning it on needs an open timer.
    $('relayToggle').disabled = disabled || Boolean(relay && (relay.stopping || relay.setup_pending)) ||
      (!timerOpen && !(relay && relay.enabled));
    const durationValid = renderRelayDuration();
    ['relayLinkLabel', 'relayCustomHours', 'relayCustomMinutes'].forEach((id) => {
      $(id).disabled = disabled || Boolean(blocked);
    });
    document.querySelectorAll('[data-relay-ttl]').forEach((button) => {
      button.disabled = disabled || Boolean(blocked);
    });
    $('relayCreateButton').disabled = disabled || Boolean(blocked) || !durationValid;
    setText('relayCreateHint', t(blocked ? relayCreateBlockedHints[blocked]
      : 'One share link at a time. It also ends when the timer restarts.'));
    $('relayForgetButton').disabled = disabled || !timerOpen;
    // Renew mints a new link, so it has the same needs as Create except room.
    renewBlocked = disabled || (Boolean(blocked) && blocked !== 'full');
    $('relayRenewButton').disabled = renewBlocked;
    $('relayReactivateButton').disabled = disabled;
    $('relayLinkList').querySelectorAll('button').forEach((button) => {
      button.disabled = button.classList.contains('relay-renew-action') ? renewBlocked : disabled;
    });
  }

  async function refreshRelay() {
    if (!status || status.chip_family !== 'ESP32-C3' || relayRequestBusy) return;
    relayRequestBusy = true;
    const bootId = status.boot_id;
    try {
      const relay = await request('/api/v1/relay');
      if (status.boot_id !== bootId) return;
      relaySupported = true;
      relayFresh = true;
      relayReceivedAt = performance.now();
      $('relaySection').classList.remove('hidden');
      renderRelay(relay);
      if (relay.enrolled && relay.running && $('settingsPanel').classList.contains('show')) {
        const payload = await request('/api/v1/relay/links');
        relayLinks = Array.isArray(payload.links) ? payload.links : [];
        relayLinksAt = performance.now();
        renderRelayLinks();
      }
    } catch (error) {
      relayFresh = false;
      if (error.status === 404) {
        relaySupported = false;
        relayCache = null;
        $('relaySection').classList.add('hidden');  // image without the relay
      } else {
        setText('relayMessage', relayError(error));
      }
      updateControls();
    } finally {
      relayRequestBusy = false;
      scheduleRelayPoll();
    }
  }

  function scheduleRelayPoll() {
    clearTimeout(relayPollHandle);
    relayPollHandle = 0;
    if (!status || status.chip_family !== 'ESP32-C3' || relaySupported === false) return;
    const settingsOpen = $('settingsPanel').classList.contains('show');
    const quick = relayCache && (relayCache.enrolling || relayCache.stopping ||
      (relayCache.enabled && relayCache.state !== 'connected' && !relayCache.paused_for_bluetooth));
    relayPollHandle = setTimeout(() => {
      relayPollHandle = 0;
      if (busy || relayBusy || liveSuspended || document.hidden || !online) {
        scheduleRelayPoll();
        return;
      }
      refreshRelay();
    }, (settingsOpen ? quick ? 1500 : 4000 : relayCache?.enrolled ? 5000 : 10000) + Math.floor(Math.random() * 301));
  }

  async function withRelayBusy(action) {
    if (relayBusy) return;
    relayBusy = true;
    updateControls();
    try {
      await action();
    } catch (error) {
      setText('relayMessage', relayError(error));
    } finally {
      relayBusy = false;
      updateControls();
      await refreshRelay();
    }
  }

  // Owner jobs are asynchronous: submit with the current next_job_id and
  // boot_id, then poll until that job is done. The firmware replays an exact
  // resubmission of the same job_id, so a request whose response was lost is
  // retried with the SAME job (never a new ID, which would apply it twice).
  let pendingRelayJob = null;  // { path, body, key } until its outcome is known

  const relayJobKey = (path, fields) => path + '|' + JSON.stringify(fields);

  async function awaitRelayJob(jobId, bootId) {
    for (let attempt = 0; attempt < 80; attempt += 1) {
      await wait(500);
      const relay = await request('/api/v1/relay');
      renderRelay(relay);
      if (relay.job && relay.job.id === jobId && relay.job.state === 'done') {
        pendingRelayJob = null;
        if (relay.job.result !== 'ok') {
          const error = new Error(relay.job.result);
          error.code = relay.job.result;
          throw error;
        }
        return relay.job;
      }
      if (relay.boot_id !== bootId) { pendingRelayJob = null; break; }  // rebooted: the job is gone
    }
    throw new Error(t('The relay did not finish in time. Check the link list before retrying.'));
  }

  // Resolve a job whose outcome is unknown before anything new is submitted.
  // Returns the job when it was the same request (so a repeated click reports
  // that outcome instead of applying it again), otherwise null.
  async function reconcileRelayJob(key) {
    const pending = pendingRelayJob;
    if (!pending) return null;
    const relay = await request('/api/v1/relay');
    if (relay.boot_id !== pending.body.if_boot_id) { pendingRelayJob = null; return null; }
    const id = pending.body.job_id;
    if (relay.job && relay.job.id === id) {
      const job = await awaitRelayJob(id, pending.body.if_boot_id);  // it was applied: wait for its result
      return pending.key === key ? job : null;
    }
    if (relay.next_job_id === id && pending.key === key) return null;  // never arrived: resubmit below
    pendingRelayJob = null;  // never arrived and the owner chose something else
    return null;
  }

  async function relayJob(path, fields) {
    const key = relayJobKey(path, fields);
    const settled = await reconcileRelayJob(key);
    if (settled) return settled;
    if (!pendingRelayJob) {
      const current = await request('/api/v1/relay');
      pendingRelayJob = { path, key, body: { ...fields, job_id: current.next_job_id, if_boot_id: current.boot_id } };
    }
    const { body } = pendingRelayJob;
    for (let attempt = 0; ; attempt += 1) {
      try {
        await mutate(path, body);
        break;
      } catch (error) {
        if (error.status) { pendingRelayJob = null; throw error; }  // the timer answered: not applied
        if (attempt >= 2) {
          throw new Error(t('The timer did not answer. Check the link list, then try again; it will not be applied twice.'));
        }
        await wait(800 * (attempt + 1));  // same job_id: a replay if the first one arrived
      }
    }
    return awaitRelayJob(body.job_id, body.if_boot_id);
  }

  async function showRelayShareUrl(fpHash, label) {
    await withRelayBusy(async () => {
      let shared = null;
      for (let attempt = 0; attempt < 20 && !shared; attempt += 1) {
        try {
          shared = await request(`/api/v1/relay/links/share?fp_hash=${encodeURIComponent(fpHash)}`);
        } catch (error) {
          if (error.code !== 'link_not_ready') throw error;
          await wait(1000);
        }
      }
      if (!shared) throw Object.assign(new Error('link_not_ready'), { code: 'link_not_ready' });
      $('relayShareUrl').value = shared.share_url;
      $('relayShareBox').dataset.fp = fpHash;
      setText('relayShareLabel', label ? t('Share link for {name}', { name: label }) : t('Share link'));
      $('relayShareBox').classList.remove('hidden');
      $('relayShareUrl').focus();
      $('relayShareUrl').select();
      setText('relayMessage', t('Share link ready. Copy it and send it privately.'));
    });
  }

  // With one link slot, a renewal can briefly see link_table_full until the
  // relay acknowledges the previous link's revocation; retry a few seconds.
  async function mintRelayLink(label, ttl) {
    for (let attempt = 0; ; attempt += 1) {
      try {
        const job = await relayJob('/api/v1/relay/links', { ttl_s: String(ttl), label });
        rememberRelayLink(label, ttl, job.fp_hash);
        return job;
      } catch (error) {
        if (error.code !== 'link_table_full' || attempt >= 9) throw error;
        await wait(1000);
      }
    }
  }

  async function renewRelayLink(fpHash, label) {
    const ttl = relayLastLink && relayLastLink.label === label ? relayLastLink.ttl : relaySelectedTtl();
    await withRelayBusy(async () => {
      setText('relayMessage', t('Renewing share link…'));
      if (fpHash) await relayJob('/api/v1/relay/links/revoke', { fp_hash: fpHash });
      $('relayShareBox').classList.add('hidden');
      $('relayShareUrl').value = '';
      const job = await mintRelayLink(label, ttl);
      relayBusy = false;  // hand over to the share-URL step
      await showRelayShareUrl(job.fp_hash, label);
    });
  }

  // Extend keeps the same URL: an open partner page follows the new expiry.
  async function extendRelayLink(fpHash, seconds, reactivating = false) {
    await withRelayBusy(async () => {
      setText('relayMessage', t(reactivating ? 'Reactivating share link…' : 'Extending share link…'));
      try {
        await relayJob('/api/v1/relay/links/extend', { fp_hash: fpHash, ttl_s: String(seconds) });
      } catch (error) {
        if (reactivating && error.code === 'link_not_found') {
          throw Object.assign(new Error('gone'), { code: 'relay_link_gone' });
        }
        throw error;
      }
      setText('relayMessage', t(reactivating
        ? 'Link reactivated. Your partner can use the same link again.'
        : 'Link extended. Your partner keeps the same link.'));
    });
  }

  async function revokeRelayLink(fpHash) {
    await withRelayBusy(async () => {
      setText('relayMessage', t('Revoking link…'));
      await relayJob('/api/v1/relay/links/revoke', { fp_hash: fpHash });
      if ($('relayShareBox').dataset.fp === fpHash) {
        $('relayShareBox').classList.add('hidden');
        $('relayShareUrl').value = '';
      }
      setText('relayMessage', t('Link revoked. That partner can no longer control the timer.'));
    });
  }

  // One setup code from the relay operator's enroll-code tool:
  // "ost-relay-setup:" + base64url(JSON {url, code, pin, partner_origin}).
  function parseRelaySetupCode(text) {
    const value = text.trim();
    const prefix = 'ost-relay-setup:';
    if (!value.startsWith(prefix) || value.length > 1024) return null;
    try {
      const base64 = value.slice(prefix.length).replace(/-/g, '+').replace(/_/g, '/');
      const padded = base64 + '='.repeat((4 - (base64.length % 4)) % 4);
      const bytes = Uint8Array.from(atob(padded), (c) => c.charCodeAt(0));
      const parsed = JSON.parse(new TextDecoder().decode(bytes));
      if (typeof parsed.url === 'string' && typeof parsed.code === 'string' &&
          typeof parsed.pin === 'string' && /^[A-Za-z0-9_-]{43}$/.test(parsed.pin) &&
          (parsed.partner_origin === undefined || typeof parsed.partner_origin === 'string')) return parsed;
    } catch (error) {
      // Not a valid setup code; reported below.
    }
    return null;
  }

  $('relaySetupCode').addEventListener('input', () => {
    const value = $('relaySetupCode').value.trim();
    const parsed = parseRelaySetupCode(value);
    if (parsed && parsed.partner_origin) $('relayPartnerOrigin').value = parsed.partner_origin;
    let host = '';
    try { host = parsed ? new URL(parsed.url).host : ''; } catch (error) { host = ''; }
    setText('relaySetupPreview', t(!value ? 'Paste the whole code. The timer must be unlocked and on Wi-Fi.'
      : host ? 'Relay: {host}' : 'That does not look like a complete setup code. Copy it again.', { host }));
  });

  $('relayEnrollForm').addEventListener('submit', (event) => {
    event.preventDefault();
    const setupText = $('relaySetupCode').value.trim();
    const setup = setupText ? parseRelaySetupCode(setupText) : null;
    if (setupText && !setup) {
      setText('relayMessage', t('That setup code is not valid. Copy it again, or enter the details manually.'));
      return;
    }
    const fields = setup
      ? { relay_url: relayOrigin(setup.url), enrollment_code: setup.code, spki_pin: setup.pin,
          partner_origin: relayOrigin(setup.partner_origin || $('relayPartnerOrigin').value) }
      : { relay_url: relayOrigin($('relayUrl').value), enrollment_code: $('relayCode').value.trim(),
          spki_pin: $('relayPin').value.trim(), partner_origin: relayOrigin($('relayPartnerOrigin').value) };
    if ((!setup && !$('relayUrl').value.trim()) || !fields.enrollment_code || !fields.spki_pin) {
      setText('relayMessage', t('Paste a setup code, or enter all four details manually.'));
      return;
    }
    if (!((setup && setup.partner_origin) || $('relayPartnerOrigin').value.trim())) {
      $('relayManualDetails').open = true;
      setText('relayMessage', t('Enter the independently hosted control page address.'));
      return;
    }
    withRelayBusy(async () => {
      setText('relayMessage', t('Waiting for physical approval'));
      const accepted = await mutate('/api/v1/relay/enroll', { ...fields, if_boot_id: relayCache.boot_id });
      renderRelay(accepted);
      const setupId = accepted.setup_request_id;
      const bootId = accepted.boot_id;
      $('relayCode').value = '';
      $('relaySetupCode').value = '';
      for (let attempt = 0; attempt < 180; attempt += 1) {
        await wait(500);
        const relay = await request('/api/v1/relay');
        renderRelay(relay);
        if (relay.boot_id !== bootId || relay.setup_request_id !== setupId) {
          setText('relayMessage', t('Setup changed or the timer restarted. Review the current setup.'));
          return;
        }
        if (relay.setup_state === 'complete' && relay.enrolled && relay.state === 'connected') {
          setText('relayMessage', t('Relay connected. You can now create share links.'));
          return;
        }
        if (relay.setup_state === 'cancelled' || relay.setup_state === 'expired') {
          setText('relayMessage', t(relay.setup_state === 'expired' ? 'Setup approval expired. Paste the code again.' : 'Setup cancelled.'));
          return;
        }
        if (relay.setup_state === 'failed') {
          throw Object.assign(new Error(relay.last_error), { code: relay.last_error });
        }
      }
      setText('relayMessage', t('Still connecting. The status above updates automatically.'));
    });
  });

  $('relaySetupCancel').addEventListener('click', async () => {
    const relay = relayCache;
    if (!relay || !relay.setup_pending) return;
    $('relaySetupCancel').disabled = true;
    try {
      const cancelled = await mutate('/api/v1/relay/enroll/cancel',
        { if_boot_id: relay.boot_id, setup_request_id: relay.setup_request_id });
      renderRelay(cancelled);
      setText('relayMessage', t('Setup cancelled.'));
    } catch (error) {
      setText('relayMessage', relayError(error));
    } finally {
      await refreshRelay();
    }
  });

  $('relayToggle').addEventListener('change', (event) => {
    const requested = event.currentTarget.checked;
    if (requested) {
      withRelayBusy(async () => {
        setText('relayMessage', t('Turning on remote sharing…'));
        await mutate('/api/v1/relay/enable');
        setText('relayMessage', t('Remote sharing is on.'));
      });
      return;
    }
    event.currentTarget.checked = true;  // until confirmed
    openConfirm({
      eyebrow: 'Remote sharing', title: 'Turn off remote sharing?',
      message: 'Every share link ends now and partners lose control. The timer keeps running under local control.',
      safety: 'You can turn sharing on again and create new links while the timer is unlocked.',
      accept: 'Turn off sharing',
      action: () => withRelayBusy(async () => {
        setText('relayMessage', t('Turning off remote sharing…'));
        await mutate('/api/v1/relay/disable');
        $('relayShareBox').classList.add('hidden');
        $('relayShareUrl').value = '';
        setText('relayMessage', t('Remote sharing is off. All share links have ended.'));
      }),
    });
  });

  $('relayCreateForm').addEventListener('submit', (event) => {
    event.preventDefault();
    const blocked = relayCreateBlock();
    if (blocked || relayBusy || busy || !online) {
      updateControls();
      setText('relayMessage', t(relayCreateBlockedHints[blocked] || 'Wait for the current device operation to finish.'));
      return;
    }
    const label = $('relayLinkLabel').value.trim();
    withRelayBusy(async () => {
      setText('relayMessage', t('Creating share link…'));
      const job = await mintRelayLink(label, relaySelectedTtl());
      $('relayLinkLabel').value = '';
      relayBusy = false;  // hand over to the share-URL step
      await showRelayShareUrl(job.fp_hash, label);
    });
  });

  document.querySelectorAll('[data-relay-ttl]').forEach((button) => {
    button.addEventListener('click', () => {
      relayTtlCustom = button.dataset.relayTtl === 'custom';
      if (!relayTtlCustom) relayTtl = Number(button.dataset.relayTtl);
      updateControls();
      if (relayTtlCustom) $('relayCustomHours').focus();
    });
  });
  ['relayCustomHours', 'relayCustomMinutes'].forEach((id) => {
    $(id).addEventListener('input', updateControls);
  });

  $('relayReactivateButton').addEventListener('click', () => {
    if (relayLastLink && relayLastLink.fp) extendRelayLink(relayLastLink.fp, relayLastLink.ttl, true);
  });

  $('relayRenewButton').addEventListener('click', () => {
    if (relayLastLink) renewRelayLink('', relayLastLink.label);
  });

  $('relayShareCopy').addEventListener('click', async () => {
    try {
      await copyText($('relayShareUrl').value);
      setText('relayMessage', t('Share link copied.'));
    } catch (error) {
      $('relayShareUrl').select();
      setText('relayMessage', t('Copy failed on this browser. Select the link and copy it manually.'));
    }
  });

  $('relayForgetButton').addEventListener('click', () => {
    openConfirm({
      eyebrow: 'Remote sharing', title: 'Forget this relay?',
      message: 'Erases the relay connection, every share link and the signing keys from this timer. Partners lose access immediately.',
      safety: 'To use a relay again you need a setup code for it.',
      accept: 'Forget relay',
      action: () => withRelayBusy(async () => {
        await mutate('/api/v1/relay/forget');
        $('relayShareBox').classList.add('hidden');
        $('relayShareUrl').value = '';
        setText('relayMessage', t('Relay forgotten.'));
      }),
    });
  });

  $('diagnosticsRefresh').addEventListener('click', refreshDiagnostics);
  $('eventsRefresh').addEventListener('click', refreshEvents);

  $('diagnosticsCopy').addEventListener('click', async () => {
    if (!diagnosticsCache) return;
    const text = JSON.stringify(diagnosticsCache, null, 2);
    try {
      if (navigator.clipboard && window.isSecureContext) {
        await navigator.clipboard.writeText(text);
      } else {
        const field = document.createElement('textarea');
        field.value = text;
        field.style.position = 'fixed';
        field.style.opacity = '0';
        document.body.append(field);
        field.select();
        document.execCommand('copy');
        field.remove();
      }
      setText('diagnosticsMessage', t('Diagnostics copied.'));
    } catch (error) {
      setText('diagnosticsMessage', t('Copy failed on this browser.'));
    }
  });

  $('firmwareFile').addEventListener('change', () => {
    const file = $('firmwareFile').files[0];
    const label = document.querySelector('.file-field span');
    if (label.textContent !== (file ? file.name : t('Choose firmware .bin'))) label.textContent = file ? file.name : t('Choose firmware .bin');
    updateControls();
  });
  // `report` shows progress on another panel's line (the online update);
  // without it, progress goes to the file section's line.
  async function installFirmware(file, releaseMetadata = {}, report = null) {
    if (releaseMetadata.release_signature && busy) throw new Error('Wait for the current device operation to finish.');
    if (busy || !status || !status.ota_armed || !file) return;
    const show = (text, state = 'busy') => {
      if (report) { report(text, state); return; }
      $('otaMessage').dataset.state = state;
      setText('otaMessage', t(text));
    };
    suspendLiveUpdates('Paused during firmware update.');
    setBusy(true);
    show('Checking image…');
    let otaQuietReserved = false;
    try {
      const checksum = await fileCrc32(file);
      show('Reserving the connection…');
      await mutate('/api/v1/ota/prepare', {
        image_size: String(file.size),
        image_crc32: checksum,
        ...releaseMetadata,
      });
      otaQuietReserved = true;
      show('Pausing other control pages…');
      await wait(1500);
      const body = new FormData();
      body.append('image_size', String(file.size));
      body.append('image_crc32', checksum);
      for (const [key, value] of Object.entries(releaseMetadata)) {
        if (key !== 'image_size' && key !== 'image_crc32') body.append(key, String(value));
      }
      body.append('firmware', file, file.name);
      show('Uploading and validating…');
      await request('/api/v1/ota', { method: 'POST', body });
      show('Update installed. The device is restarting…', 'ok');
      // The chosen file is installed; clearing it keeps Install update
      // disabled while the timer restarts.
      $('firmwareFile').value = '';
      $('firmwareFile').dispatchEvent(new Event('change'));
    } catch (error) {
      const connectionLost = /load failed|failed to fetch|networkerror/i.test(error.message);
      show(connectionLost
        ? 'The connection was interrupted. Do not remove power; wait 15 seconds, reconnect, and check the firmware version.'
        : error.message, 'error');
    } finally {
      setBusy(false);
      setTimeout(resumeLiveUpdates, otaQuietReserved ? 15000 : 0);
    }
  }

  $('otaForm').addEventListener('submit', (event) => {
    event.preventDefault();
    if (busy || !status || !status.ota_armed) return;
    const file = $('firmwareFile').files[0];
    if (!file) return;
    if (status.ota_unverified_allowed) {
      openConfirm({
        eyebrow: 'Firmware recovery',
        title: 'Install unverified firmware?',
        message: 'This image is not recognized as this project’s firmware or a known vendor rollback. A wrong image can make the timer unreachable through the web interface.',
        safety: 'Only the target-family header, declared size, and CRC are checked. Once upload begins, this one-use override is consumed even if the update fails.',
        accept: 'Install unverified firmware',
        action: () => installFirmware(file),
      });
      return;
    }
    installFirmware(file);
  });

  applyColorTheme(colorTheme);
  // A saved switch record that could not be read back turns both switches
  // off; say so where the owner sees them.
  request('/api/v1/timer/controls').then((controls) => {
    if (controls && controls.recovered) {
      setText('sessionControlsMessage', t('The saved session controls were damaged, so both were switched off. Change either switch to save your choice.'));
    }
  }).catch(() => {});
  updateNotice = window.SmartTimerUpdates.notice({
    root: $('updateNotice'), toggle: $('updateNoticeToggle'), translate: t,
    config: async () => (await request('/api/v1/capabilities')).updates,
    // The notice only points at the Firmware section's own check and dialog.
    view: () => {
      openSettings($('settingsOpen'));
      setTimeout(() => {
        $('githubUpdates').scrollIntoView({ behavior: 'smooth', block: 'start' });
        $('githubUpdates').querySelector('[data-update-check]').click();
      }, 50);
    },
  });
  if (status) updateNotice.status({ version: status.version, state: status.state });
  window.SmartTimerUpdates.panel({
    root: $('githubUpdates'), translate: t,
    confirm: record => new Promise(resolve => {
      const prompt = window.SmartTimerUpdates.prompt(record, t);
      openConfirm({eyebrow:prompt.eyebrow,title:prompt.title,message:prompt.notes,safety:prompt.safety,
        accept:'Install',kind:'update',action:null});
      confirmResolve=resolve;
    }),
    device: async () => {
      const [current, capabilities] = await Promise.all([request('/api/v1/status'), request('/api/v1/capabilities')]);
      render(current);
      return { updates: capabilities.updates, version: current.version, armed: current.ota_armed, state: current.state };
    },
    install: installFirmware,
  });
  updateControls();
  showApp();
  renderClock();
})();
