# Smart Timer Bluetooth app

Original app code is covered by the repository's [AGPL-3.0-only license](../LICENSE).

This directory contains the browser-based `ost-ble/1` companion app for the
MagBound Smart Timer V2 target. It controls one nearby timer without requiring
the timer or phone to be connected to Wi-Fi. Approved devices can also manage
network settings, device names, approvals, and firmware updates over BLE.
Adding an approval and arming an update still require the physical Reset button;
factory reset remains a physical operation.

The app has one interface with two views:

- **User view** matches the timer-hosted UI: duration wheel, quick presets,
  random duration, live countdown, adjustment controls, pause/resume, immediate
  unlock, and software control locking.
- **Developer view** is enabled from App Settings and adds protocol metadata,
  parsed live state, credential recovery, and a bounded wire log.

Normal App Settings also show the configured name, BLE name, device/build IDs,
firmware, current boot/reset reason, AP or Wi-Fi state, local address, hostname,
AP clients, heap health, application/OTA/flash sizes, BLE MTU and signal, plus
the timer's retained 16-event current-boot activity history. The physical guide
documents every supported Reset-button gesture and LED pattern.

## Folder location

This checkout stores the web app in `bluetooth-app/`, matching the root
documentation and aggregate checks.

## Browser support

Web Bluetooth requires a Chromium-family browser and a secure context.

| Platform | Support |
| --- | --- |
| Android Chrome, Edge, Samsung Internet, or Brave | Supported over HTTPS or localhost |
| Desktop Chrome or Edge | Supported over HTTPS or localhost |
| iOS Safari | Not supported because iOS does not expose Web Bluetooth |
| Firefox | Not supported |

## Run locally

From the repository root:

```sh
python3 -m http.server 8000
```

Open `http://localhost:8000/bluetooth-app/`. For an Android device attached by
USB, forwarding the port preserves the required localhost secure context:

```sh
adb reverse tcp:8000 tcp:8000
```

Then open `http://127.0.0.1:8000/bluetooth-app/` on the phone.

For normal use, serve the directory from one stable HTTPS origin. The browser
credential and selected app installation belong to that origin; changing
temporary tunnel hostnames creates a separate browser storage identity.

### GitHub Pages

The **Firmware** part of Settings includes manual **Check for updates** and signed firmware download
over the existing physically armed Bluetooth installer. The browser needs
Internet access; checks and downloaded images are not stored in the offline
app shell or device flash. Once the app has read the timer's settings it also
checks at most once a day while the timer is open and shows a dismissible
**Update available** notice; **Check when this page opens** in the same section
turns that off. See [GitHub firmware updates](../docs/GITHUB_UPDATES.md).

The app is entirely static and can be hosted from GitHub Pages. Use the
[curated Pages workflow](../docs/GITHUB_PAGES.md), not the complete repository
tree. It includes the app and its shared styles/translations, then serves:

```text
https://<account>.github.io/<repository>/bluetooth-app/
```

GitHub Pages supplies the HTTPS secure context Web Bluetooth requires. Hosting
does not proxy timer traffic or receive controller keys: BLE communication and
IndexedDB storage stay in the user's browser. Use one stable production URL,
because changing the origin creates a different browser credential store.

## Correct physical pairing sequence

Do not connect first and then try to open pairing. The timer intentionally
requires the BLE connection to begin after the physical pairing window opens.

1. The timer must be open.
2. Open pairing before connecting:
   - without Wi-Fi, hold **Reset for 3–5 seconds**, release it, and wait for the
     blue LED's slow pairing pulse; or
   - from the timer-hosted interface, use **Settings → Bluetooth → Pair device**.
3. In this app, select **Connect** and choose the `OST-*` timer.
4. Complete the operating system's BLE link pairing if prompted.
5. Briefly press and release **Reset** to approve that connected app. The blue
   LED changes to a rapid pattern.
6. Select **Finish pairing** in this app.
7. The app stores a pending credential, sends Pairing Commit, waits for the
   timer's durable-commit confirmation, authenticates using that pending key,
   and only then promotes it to the active browser credential.

If the connection or response is lost after commit, the pending credential is
preserved. Reconnect and the app attempts to prove it before falling back to the
last known-good credential. A failed replacement therefore does not silently
overwrite the browser's working key.

Pair only in a trusted physical and RF environment. The ESP32-C3 has no display
or keypad, so its encrypted Secure Connections link uses “Just Works” pairing;
physical approval limits the window but cannot display an authenticated
passkey.

## Command reliability

- Timer status becomes unavailable after 30 seconds without an accepted live
  snapshot (the device keepalive is 15 seconds). The dial shows a dash instead
  of continuing an unconfirmed countdown, and ordinary timer changes are
  disabled in both the interface and command API. System-information reads and
  optimistic command-result updates do not renew live-status freshness.
- Returning to the page requires a new live-status read before controls return.
  Notifications queued before resume cannot satisfy this check. Recovery reads
  are coalesced, retried at most every five seconds, and use the same bounded
  GATT queue as commands; a slow timer cannot accumulate a polling backlog.
- An explicit Unlock remains reachable when permitted by the last observed
  controls-lock state. Every Unlock reads current status before signing, uses
  the returned session, and stops without sending if status cannot be read or
  software controls are locked. It does not bypass the timer's interlocks.
- Only one command is in flight at a time. All characteristic reads, writes and
  subscriptions share a GATT queue, including system-info polling. A stalled
  operation has a ten-second deadline and closes its connection before reuse.
- Each command receives a browser-persistent, nonzero request ID and a strictly
  increasing counter for the authenticated connection.
- The app subscribes for the matching result before writing.
- If a write acknowledgement or notification is lost, it reads the cached result.
  If needed, it retries the exact same signed frame once on that same connection.
  Result validation checks ID, opcode, boot, applicable session and timer state.
- Before writing, it journals the semantic command and request ID in per-device
  browser storage (no key is included). An unconfirmed operation is **outcome
  unknown**, never “not applied.” Reconnect/reload reads the result and current
  status without automatically resending: the timer's eight-entry replay cache
  is bounded. Do not repeat a time adjustment under a new ID while unresolved.
- While unresolved, other changes wait for confirmation, an open snapshot, or a
  new boot. Unlock remains available when the timer normally permits it; this
  does not override software-locked controls or create a physical release path.
- Rejections remain visible in the user interface and include the firmware's
  reason. A successful result is followed by a live-status refresh.
- In a duplicate result, `changed` describes the cached original operation;
  `duplicate` means this replay was recognized and not executed again.

## Credential storage

Keys are stored in IndexedDB per timer, named by the timer's protocol
`device_id`, so this browser can stay paired with several timers. **Saved
timers** (on the main screen when more than one is saved, and in Settings →
Paired device) switches between them; Forget removes only that timer's key
here, not its approval on the timer. A key saved before this change has no
timer ID: it is tried on the next connection and moved under that timer's ID
once it authenticates.

In Settings → Paired device, **Device name** names this phone/computer's
approval, not the timer. Set it before finishing pairing. After the physical
Reset approval, the app sends it over encrypted BLE and the timer saves it
atomically with the key. No Wi-Fi/AP connection is needed. Names are limited
to 24 UTF-8 bytes (normally eight Chinese characters). A nickname explicitly
chosen on the timer's local web page takes priority. The optional extension is
discovered from protocol metadata; clients/firmware without it use slot names.

The timer can approve three independent apps but accepts one connection at a
time. Disconnect before switching apps. While the page is hidden for one
minute the app disconnects on its own, so another approved phone can connect;
it waits for any command still awaiting a result or pairing in progress. Shown
again, it reconnects to the remembered timer without the device chooser where
the browser supports `navigator.bluetooth.getDevices()`, and otherwise asks you
to select Connect. Nothing is replayed. A full approval table is never silently
replaced; use local settings to remove or select an approval for replacement.

With timer firmware that includes the Bluetooth
[management extension](../docs/BLUETOOTH_MANAGEMENT.md), **Settings → Timer
settings** changes the timer itself over Bluetooth: Wi-Fi (with a scan), the
access point, the timer name, restart, approved devices, turning Bluetooth off,
and firmware updates. The rules are the same as the timer's web page: the timer
must be unlocked, and a firmware update needs a short Reset press first.
Passwords are encrypted in the browser before they are sent. Timer controls
pause while a settings request runs.

Controller keys are stored in an IndexedDB database named
`opensmarttimer-bluetooth`, not in `localStorage` or the wire log.

| Record | Meaning |
| --- | --- |
| `active` | Last credential successfully authenticated by both sides |
| `pending` | Candidate saved before Pairing Commit; retained until proven or explicitly forgotten |

The prior `ost.ble.test.appKey` localStorage value is migrated once into the
active IndexedDB record and removed. Developer-view and request-ID preferences
remain non-secret browser settings.

Forgetting the controller in App Settings removes only this browser's records.
It does not revoke the timer's approval. While connected and the timer is unlocked,
use **Timer settings → Approved devices** to rename or remove an approval,
including this browser's approval. Removing this browser disconnects it;
physical pairing is required to approve it again. Factory reset also clears approvals.

To recover on the same phone while retaining its OS bond, close other timer apps,
disconnect, open pairing before reconnecting, approve with Reset and finish
pairing again. The old app key is retained until durable replacement commit.
See [pairing recovery](../docs/BLUETOOTH.md#same-phone-browser-key-recovery) for
the different case where the OS bond was lost or is stale. Forgetting credentials
does not discard unresolved command journals.

Transport rules live in `ble-transport.mjs`; `app.js` owns the page, credentials
and per-device journal. Run the shared transport and actual app-function fault
tests with `node --test tests/web/bluetooth_transport_test.mjs` from the repository root.

## Appearance and diagnostics

Settings → Appearance offers Purple (the default), Slate and Teal. Each works
with the header's light/dark toggle. Palette choice persists in this browser's
`localStorage`; language and light/dark remain per-tab preferences. The timer
page and relay pages offer the same palettes, but different website origins
keep separate preferences. Appearance changes never write timer flash.

Identify LED and Restart device are grouped under Settings → Diagnostics.
Their availability still depends on connection and timer state.

## Temporary Bluetooth-only mode

When connected to supporting V2 firmware, open **Settings → Timer settings →
Connection mode** and select **Use Bluetooth only while connected** while the
timer is unlocked. Both Wi-Fi station and the access point turn off; Bluetooth
stays connected. This change is held in memory, so saved Wi-Fi/AP settings and
flash are untouched. The control is offered only when Wi-Fi status reports
`wifi_suspend_supported: true`.

Use **Restore Wi-Fi / AP** in the same place to restore saved networking.
On current V2 firmware (`wifi_disconnect_restore_supported: true`), a Bluetooth
disconnect or host reset restores saved Wi-Fi/AP automatically. A rapid reconnect
does not cancel recovery. Saved router mode still falls back to a recovery AP
when the router cannot connect. Restart also restores networking. Earlier firmware
without this flag retains **Use Bluetooth only until restart** and requires
explicit BLE resume or power-cycle.
Power loss also releases the timer as usual. Turning Bluetooth off is blocked
while Bluetooth-only mode is active. Network scans/joins must finish first,
and a newly updated device must finish its startup validation before Wi-Fi can
be suspended. See the [management protocol](../docs/BLUETOOTH_MANAGEMENT.md#temporary-bluetooth-only-mode).

## Offline behavior

`service-worker.js` pre-caches the app shell, shared Smart Timer theme, and
translation file. Once App Settings reports **Offline app: Ready offline**, the
installed page can launch without Internet access and establish BLE directly.
The worker serves one complete versioned shell both online and offline.
Updates download the complete next shell before installation; an available
update waits until all old app windows close. Close every app window and
reopen to activate it, avoiding mixed-version files during an active session.

English/Chinese and light/dark selections are local to the browser tab. Changing
language updates the existing page in place: it does not reload, drop the BLE
connection, clear unsent duration values, or re-authenticate. It does not write
settings to the timer. The shared interface assets are included in the offline
cache; reconnect online once to receive interface updates before using them
offline.

Web Bluetooth permission and the operating system bond remain browser/platform
state; clearing site data or uninstalling the browser can remove them.

## Protocol scope

The app implements all timer operations in `ost-ble/1`:

- fixed and randomized start;
- pause and resume;
- signed time adjustment;
- immediate release;
- software control locking, with a warning and explicit confirmation;
- identify;
- live status and detailed event notifications;
- authenticated current-boot, AP/Wi-Fi, heap, flash, BLE-link, and support
  diagnostics.

**Lock controls** is available while running, unless the owner has switched
control locking off in the timer's settings. Its warning explains that pause,
time changes and software unlock will remain disabled until expiry, and that
the emergency input and removing power are not guaranteed to open the lock.
Cancel sends nothing.
After confirmation, the app reads current status and verifies the original
timer, connection and session before sending. If the timer expires, pauses,
restarts or changes sessions while the warning is open, review the new state
before trying again.

Status flag bit `0x04` means `lock_output_energized`. Whether a timer session is
active is derived from `state != open`; the app does not relabel the output bit
as an active-session flag.

See [`../docs/BLUETOOTH.md`](../docs/BLUETOOTH.md) for byte-level frame layouts
and the full security model.

## Files

```text
bluetooth-app/
├── README.md
├── index.html
├── app.js
├── app.css
├── service-worker.js
├── manifest.webmanifest
└── icon.svg
```
