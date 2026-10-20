// Shared by the embedded Wi-Fi page and the Bluetooth companion. The device
// verifies the release signature and streaming SHA-256 before committing flash.
(() => {
  const targets = {
    magbound_v1_esp8266: 'MagBoundV1-ESP8266',
    magbound_v2_esp32c3: 'MagBoundV2-ESP32C3',
  };
  function versionParts(value) {
    if (typeof value !== 'string' || value.length >= 48) return null;
    const match = /^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-([0-9A-Za-z.-]+))?$/.exec(value);
    if (!match) return null;
    const parts = match.slice(1, 4).map(Number);
    if (parts.some(n => n > 0xffffffff)) return null;
    const pre = match[4] === undefined ? null : match[4].split('.');
    if (pre?.some(p => !p || (/^\d+$/.test(p) && p.length > 1 && p[0] === '0'))) return null;
    return { parts, pre };
  }
  function newer(candidate, installed) {
    const a = versionParts(candidate), b = versionParts(installed);
    if (!a || !b) return false;
    for (let i = 0; i < 3; i += 1) if (a.parts[i] !== b.parts[i]) return a.parts[i] > b.parts[i];
    if (!a.pre || !b.pre) return !a.pre && Boolean(b.pre);
    for (let i = 0; i < Math.max(a.pre.length, b.pre.length); i += 1) {
      const x = a.pre[i], y = b.pre[i];
      if (x === undefined || y === undefined) return x !== undefined;
      if (x === y) continue;
      const xn = /^\d+$/.test(x), yn = /^\d+$/.test(y);
      if (xn !== yn) return !xn;
      if (xn && x.length !== y.length) return x.length > y.length;
      return x > y;
    }
    return false;
  }
  function validate(record, target) {
    if (!targets[target] || record?.target !== target || !versionParts(record.release_version) ||
        !Number.isInteger(record.image_size) || record.image_size < 24 || record.image_size > 4194304 ||
        !/^[0-9a-f]{8}$/.test(record.image_crc32) || !/^[0-9a-f]{64}$/.test(record.image_sha256) ||
        !/^[0-9a-f]{128}$/.test(record.release_signature) ||
        record.file !== `OpenSmartTimer-${record.release_version}-${targets[target]}.bin` ||
        (record.release_notes !== undefined && (typeof record.release_notes !== 'string' || record.release_notes.length > 8192))) {
      throw new Error('The update feed is invalid or belongs to another timer model.');
    }
    return record;
  }
  function baseUrl(value) {
    const url = new URL(value);
    if (url.protocol !== 'https:' || !/^[a-z0-9-]+\.github\.io$/.test(url.hostname) ||
        !/^\/[A-Za-z0-9_.-]+\/$/.test(url.pathname) || url.port || url.username || url.password || url.search || url.hash) {
      throw new Error('The update source is not configured correctly.');
    }
    return url.href;
  }
  async function boundedFetch(url, maximum, timeoutMs, exact = false) {
    const controller = new AbortController();
    const timer = setTimeout(() => controller.abort(), timeoutMs);
    try {
      let response;
      try {
        response = await fetch(url, { signal: controller.signal, cache: 'no-store', credentials: 'omit', redirect: 'error' });
      } catch (error) {
        if (error.name !== 'AbortError') error.updateNetworkFailure = true;
        throw error;
      }
      if (!response.ok) {
        const error=new Error('The update server is unavailable. Try again later.');
        error.httpStatus=response.status;
        throw error;
      }
      const declared = Number(response.headers.get('content-length'));
      if (declared > maximum) throw new Error('The update download is larger than expected.');
      const reader = response.body.getReader();
      const data = new Uint8Array(maximum);
      let used = 0;
      try {
        for (;;) {
          const { done, value } = await reader.read();
          if (done) break;
          if (used + value.length > maximum) throw new Error('The update download is larger than expected.');
          data.set(value, used); used += value.length;
        }
      } catch (error) { await reader.cancel().catch(() => {}); throw error; }
      if (exact && used !== maximum) throw new Error('The update download was incomplete.');
      return data.slice(0, used);
    } catch (error) {
      if (error.name === 'AbortError') throw new Error('The update request timed out. Try again later.');
      throw error;
    } finally { clearTimeout(timer); }
  }
  function crc32(bytes) {
    let crc = 0xffffffff;
    for (const byte of bytes) {
      crc ^= byte;
      for (let bit = 0; bit < 8; bit += 1) crc = (crc >>> 1) ^ (0xedb88320 & -(crc & 1));
    }
    return ((crc ^ 0xffffffff) >>> 0).toString(16).padStart(8, '0');
  }
  function canonical(record) {
    return new TextEncoder().encode(`OpenSmartTimer-Update-v1\n${record.target}\n${record.release_version}\n${record.image_size}\n${record.image_crc32}\n${record.image_sha256}\n`);
  }
  async function verifyRecord(record, publicKey) {
    if (!/^04[0-9a-f]{128}$/.test(publicKey)) throw new Error('The update source is not configured correctly.');
    if (!globalThis.crypto?.subtle) return; // Plain HTTP: the timer checks it before writing.
    const bytes = text => Uint8Array.from(text.match(/../g), byte => parseInt(byte, 16));
    const key = await crypto.subtle.importKey('raw', bytes(publicKey), { name: 'ECDSA', namedCurve: 'P-256' }, false, ['verify']);
    if (!await crypto.subtle.verify({ name: 'ECDSA', hash: 'SHA-256' }, key, bytes(record.release_signature), canonical(record))) {
      throw new Error('The release signature is invalid.');
    }
  }
  async function check(config, installed) {
    if (!config?.configured) throw new Error('GitHub updates are not configured in this firmware.');
    const base = baseUrl(config.base_url);
    if (!targets[config.target]) throw new Error('The update source is not configured correctly.');
    let data;
    try { data=await boundedFetch(`${base}updates/${config.target}.json`,65536,15000); }
    catch(error) {
      // Before the first Pages deployment, GitHub's 404 has no CORS headers.
      // Confirm an empty public release list rather than treating a failed
      // download as evidence that there are no updates.
      if (error.httpStatus === 404 || error.updateNetworkFailure) {
        const source = new URL(base);
        const owner = source.hostname.slice(0, -'.github.io'.length);
        const repo = source.pathname.slice(1, -1);
        try {
          const releases = JSON.parse(new TextDecoder('utf-8', {fatal:true}).decode(
            await boundedFetch(`https://api.github.com/repos/${encodeURIComponent(owner)}/${encodeURIComponent(repo)}/releases?per_page=1`, 65536, 15000)));
          if (Array.isArray(releases) && releases.length === 0) return {base,available:false,record:null};
        } catch (releaseError) {
          // A private or unpublished repository has no anonymously available
          // releases. Never request GitHub credentials from a timer page.
          if (releaseError.httpStatus === 404) return {base,available:false,record:null};
          // Preserve the original feed error when absence is unconfirmed.
        }
      }
      throw error;
    }
    const record = validate(JSON.parse(new TextDecoder('utf-8',{fatal:true}).decode(data)), config.target);
    await verifyRecord(record, config.public_key);
    return { record, base, available: newer(record.release_version, installed) };
  }
  async function download(candidate) {
    const { record, base } = candidate;
    const image = await boundedFetch(`${base}updates/images/${encodeURIComponent(record.file)}`, record.image_size, 90000, true);
    if (crc32(image) !== record.image_crc32) throw new Error('The downloaded firmware checksum does not match.');
    // WebCrypto is unavailable on a plain HTTP timer page. The timer always
    // performs the signature and SHA-256 checks, regardless of this extra check.
    if (globalThis.crypto?.subtle) {
      const hash = new Uint8Array(await crypto.subtle.digest('SHA-256', image));
      const hex = Array.from(hash, n => n.toString(16).padStart(2, '0')).join('');
      if (hex !== record.image_sha256) throw new Error('The downloaded firmware SHA-256 does not match.');
    }
    return new File([image], record.file, { type: 'application/octet-stream' });
  }
  function metadata(record) {
    return { image_size: record.image_size, image_crc32: record.image_crc32,
      release_version: record.release_version, image_sha256: record.image_sha256,
      release_signature: record.release_signature };
  }
  // Install confirmation shared by both pages. Every published version uses
  // one feed, so a prerelease is named as one before the owner installs it.
  function prompt(record, translate = s => s) {
    const tag = versionParts(record.release_version)?.pre?.join('.') || '';
    return {
      eyebrow: tag ? 'Pre-release update' : 'Firmware update',
      title: translate('Update to {version}?', { version: record.release_version }),
      notes: record.release_notes || 'No release notes were provided.',
      safety: tag
        ? translate('Provided as is, without warranty. This is a pre-release ({tag}) and may be less tested than a stable release. Open the timer and briefly press Reset to allow installation. Keep the timer powered during the update.', { tag })
        : translate('Provided as is, without warranty. Open the timer and briefly press Reset to allow installation. Keep the timer powered during the update.'),
    };
  }
  // Release notes arrive as the changelog's Markdown. Show its small subset
  // (headings, bullets, bold, code and links) from text nodes, never as HTML.
  function renderNotes(element, text) {
    element.replaceChildren();
    const inline = (parent, line) => {
      let last = 0;
      for (const m of line.matchAll(/\*\*([^*]+)\*\*|`([^`]+)`|\[([^\]]+)\]\(([^)\s]*)\)/g)) {
        parent.append(line.slice(last, m.index));
        let node;
        if (m[1] !== undefined) { node = document.createElement('strong'); node.textContent = m[1]; }
        else if (m[2] !== undefined) { node = document.createElement('code'); node.textContent = m[2]; }
        else if (m[4].startsWith('https://')) {
          node = document.createElement('a'); node.textContent = m[3]; node.href = m[4];
          node.target = '_blank'; node.rel = 'noopener noreferrer';
        } else node = m[3];
        parent.append(node);
        last = m.index + m[0].length;
      }
      parent.append(line.slice(last));
    };
    let list = null;
    for (const raw of String(text).split('\n')) {
      const line = raw.trim();
      if (!line) { list = null; continue; }
      const item = /^[-*]\s+(.*)$/.exec(line);
      if (item) {
        if (!list) { list = document.createElement('ul'); element.append(list); }
        const entry = document.createElement('li'); inline(entry, item[1]); list.append(entry);
        continue;
      }
      list = null;
      const heading = /^#{1,6}\s+(.*)$/.exec(line);
      const block = document.createElement(heading ? 'h4' : 'p');
      inline(block, heading ? heading[1] : line);
      element.append(block);
    }
  }
  // Physical Reset allowance, worded identically on both pages. Only the
  // title is a live region, so a refreshed countdown is not announced.
  function allowance(root, status, translate = s => s) {
    const minutes = Math.max(1, Math.round((status.ota_armed_remaining_ms || 0) / 60000));
    const [state, title, detail] = !status.ota_armed
      ? ['locked', 'Updates are locked', 'Briefly press Reset on the unlocked timer to allow an update for five minutes.']
      : status.ota_unverified_allowed
        ? ['recovery', 'Recovery mode', 'Updates allowed for about {minutes} min, including one unverified recovery image.']
        : ['armed', 'Ready to update', 'Updates allowed for about {minutes} min.'];
    const write = (selector, text) => { const node = root.querySelector(selector); if (node.textContent !== text) node.textContent = text; };
    root.dataset.state = state;
    write('[data-update-arm-title]', translate(title));
    write('[data-update-arm-detail]', translate(detail, { minutes }));
  }
  function panel({ root, device, install, confirm, translate = s => s }) {
    const checkButton = root.querySelector('[data-update-check]');
    const message = root.querySelector('[data-update-message]');
    let busy = false;
    const say = (text, state = '') => { message.textContent = translate(text); message.setAttribute?.('data-state', state); };
    const render = () => { checkButton.disabled = busy; };
    checkButton.addEventListener('click', async () => {
      if (busy) return;
      busy = true; say('Checking for updates…', 'busy'); render();
      try {
        const current = await device();
        const candidate = await check(current.updates, current.version);
        if (!candidate.available) {say('No updates available.', 'ok');return;}
        say(translate('Update available: {version}',{version:candidate.record.release_version}), 'available');
        if (!await confirm(candidate.record)) {say('Update cancelled.');return;}
        const latest = await device();
        if (!newer(candidate.record.release_version, latest.version)) {
          say('No updates available.', 'ok'); return;
        }
        if (!latest.armed || latest.state !== 'open') throw new Error('Open the timer and briefly press Reset to allow updates, then try again.');
        say('Downloading update…', 'busy');
        const file = await download(candidate);
        say('Download complete. The timer will verify the signed firmware.', 'busy');
        // The install reports its own steps on this panel's line.
        await install(file, metadata(candidate.record), say);
      } catch (error) { say(error.message, 'error'); }
      finally { busy = false; render(); }
    });
    render();
  }
  // Optional reminder when a page opens. The browser, not the timer, checks
  // the feed at most once a day and only while the timer is open, and stays
  // silent on any error: the Firmware section reports problems. Installing
  // still goes through that section's check, dialog and physical Reset press.
  const NOTICE_KEY = 'smarttimer.updateNotice';
  const NOTICE_INTERVAL_MS = 24 * 60 * 60 * 1000;
  function notice({ root, toggle, view, config, translate = s => s }) {
    let saved = {};
    try { saved = JSON.parse(localStorage.getItem(NOTICE_KEY)) || {}; } catch (_) { /* Session-only. */ }
    const save = () => { try { localStorage.setItem(NOTICE_KEY, JSON.stringify(saved)); } catch (_) { /* Session-only. */ } };
    const enabled = () => saved.enabled !== false;
    let installed = '', open = false, attempted = false, known = null;
    const show = () => {
      const visible = enabled() && open && Boolean(saved.available) && newer(saved.available, installed) &&
        saved.dismissed !== saved.available;
      root.classList.toggle('hidden', !visible);
      if (visible) root.querySelector('[data-update-notice-title]').textContent =
        translate('Update available: {version}', { version: saved.available });
    };
    toggle.checked = enabled();
    toggle.addEventListener('change', () => { saved.enabled = toggle.checked; save(); show(); });
    root.querySelector('[data-update-notice-view]').addEventListener('click', () => view());
    root.querySelector('[data-update-notice-dismiss]').addEventListener('click', () => {
      saved.dismissed = saved.available; save(); show();
    });
    return {
      // Pass whatever the page knows; missing fields keep their last value.
      async status({ version, state, updates } = {}) {
        if (version) installed = version;
        if (state) open = state === 'open';
        if (updates) known = updates;
        show();
        // Without a fetcher (Bluetooth), wait for the page to supply the config.
        if (attempted || !enabled() || !open || !installed || (!known && !config) ||
            Date.now() - (Number(saved.checked_at) || 0) < NOTICE_INTERVAL_MS) return;
        attempted = true;  // One attempt per page load, even offline.
        try {
          const result = await check(known || await config(), installed);
          saved.checked_at = Date.now();
          saved.available = result.available ? result.record.release_version : '';
          save();
          show();
        } catch (_) { /* Silent; the manual check shows errors. */ }
      },
      refresh: show,
    };
  }
  globalThis.SmartTimerUpdates = { newer, validate, baseUrl, boundedFetch, crc32, canonical, verifyRecord, check, download, metadata, prompt, renderNotes, allowance, panel, notice };
})();
