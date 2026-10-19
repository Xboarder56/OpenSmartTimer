# Bluetooth Low Energy (`ost-ble/1`)

The V2 Bluetooth contract provides status, events and timer control without an
active Wi-Fi connection. The control frame's major byte and authentication,
session and command domain labels use version 1. Management and pairing-nickname
extensions have independent version 1 formats. Keep firmware and clients
compatible when changing these contracts.

BLE is a MagBound V2 / ESP32-C3 feature. V1 is an ESP8266 and contains no
Bluetooth stack or BLE protocol code. Read [Safety](../SAFETY.md) and verify
hardware with the controlled load disconnected.

## Safety and authority model

- `TimerController` remains the only timer state machine. BLE, REST, the web
  UI, the emergency input, and expiry all act on that same state.
- BLE callbacks queue commands for the main loop; they never drive lock GPIOs.
- Emergency input, maximum duration, controls-lock, boot identity, session
  identity, and idempotency checks apply equally to BLE and REST.
- The radio preference, up to three independent 128-bit app keys, and link-layer bonds
  persist across restart. The ten-second factory reset erases all three.
- Turning Bluetooth off persists that preference and ends the current BLE
  connection. It does not revoke the paired app.
- Five failed application authentications or command-integrity checks across
  reconnects close the offending connection and impose a 30-second
  advertising backoff. A successful authentication clears the failure count.
  The saved controller credential remains valid and can reconnect after that
  delay; no command is applied by a failed frame.
- Live status, event detail, and command results require both an encrypted BLE
  link and application authentication. Only protocol metadata and the compact
  capability frame are public.

BLE uses Secure Connections bonding with the ESP32-C3's no-display “Just
Works” link pairing. That encrypts traffic but does not authenticate a nearby
person by itself. OpenSmartTimer therefore also requires its independently
generated application key. Both the web button and the no-Wi-Fi hardware
gesture open the same physical-confirmation ceremony. The secret is delivered
only through the encrypted GATT characteristic after a short Reset-button
approval; the web response never contains it. Keep both the local network and
paired app trusted. Pair only in a trusted physical/RF environment: a nearby
active attacker could impersonate an endpoint during the roughly 60-second
post-approval setup window because a no-display device cannot
authenticate a Just Works peer against a passkey.

The application key is stored in the OpenSmartTimer configuration record with
an integrity checksum; it is not hardware-encrypted. Physical flash access can
recover it. This protocol is for local control, not direct Internet exposure.

## Pairing and recovery

### Three approved apps, one connection

The firmware stores three approvals, not three simultaneous BLE
connections. Disconnect the active app before switching phones/apps; while it
is connected the timer does not advertise, so other phones cannot find it. The
bundled web app lets go after one minute hidden and reconnects when shown. The
firmware drops an authenticated connection that sends no GATT read or write for
five minutes (its own notifications do not count); clients that stay connected
must read at least that often. A client that needs to stay connected in the
background can answer the 15-second status notification with a read at most
once a minute. Dropping a connection never changes the timer. Each
approval has a stable, non-reused ID and an editable UTF-8 label (24-byte limit).
Adding an app uses an empty slot. A fourth is refused; replacement must target
an explicit app ID in local settings. The 3–5-second hardware gesture only adds
to an empty slot and never evicts another app. Full tables require local
management access to remove or select a replacement. The current management
extension exposes these operations to an already-authenticated BLE app as well
as the local web interface. Every approved app is a pairing administrator: it
can rename/revoke other approvals or revoke all, subject to the existing
timer-state checks. There is no owner-only or timer-control-only role. Physical
Reset confirmation is still required to add or replace an approval, but not to
revoke one. Approve only apps you trust with this authority.

The ESP32-C3 framework has capacity for two connections, while the current app
accepts one inbound connection. It retains four bonds (three approved peers
plus an atomic replacement candidate), and sixteen stored subscription records.
Overflow never invokes round-robin eviction. App credentials reference resolved
OS peer identities; two apps on the same phone can share a bond. Removing one
key removes that bond only if no remaining approval needs it. Unknown legacy
peer bindings conservatively retain existing bonds until the migrated app first
authenticates. A phone that has lost its OS bond may require removal of its old
approval(s) before fresh pairing; firmware never silently deletes a bond used
by another approved app.

Authentication tries the three app keys once and derives one session key.
Command counters stay volatile/per-session. Each app has eight independent
deduplication records; request IDs may overlap across apps. Reconnecting restores
only that app's latest result. A replaced slot receives a new ID and cannot
inherit retry results. No timer authority, GPIO, OTA partition, or safety limits
change. Attribution is available as `bluetooth_app_id` in REST event history;
the existing compact BLE command/result/event frames remain unchanged.

Approvals are an atomic, checksummed `ble_apps` NVS blob in `opensmarttimer`,
separate from the unchanged 512-byte configuration blob. Existing approval
migrates to “Existing app”; only after durable migration is the legacy key
retired. An older OpenSmartTimer firmware will therefore need BLE re-pairing
after downgrade, but Wi-Fi/AP settings and web OTA recovery are unchanged.
Invalid registries fail closed rather than restoring potentially revoked keys.
Factory reset clears the registry too. A durable reset-intent flag completes
an interrupted reset on next boot, covering both registry and network records.
V1 never accesses this registry.

Before downgrading to an older OpenSmartTimer build, remove all approved apps
and disable Bluetooth. Older builds have a smaller bond table and cannot manage
this registry. This is a clean BLE downgrade procedure, not a change to the OTA
partition layout or image recovery gate.

Names/approval changes are persisted; last-seen values are RAM-only uptime from
this boot. Neither activity nor command counters cause recurring flash writes.
Management endpoints and examples are in [API.md](API.md#bluetooth-pairing-management-v2-only).

### Pair from the web interface

The settings list is called **Paired devices**. **Device name** identifies
the phone/computer approval, not the timer's own device name. Each browser/app
has its own credential, so two apps on one phone can occupy two slots.
An explicit nickname entered on the timer's web page takes priority. Otherwise
the connecting app can supply it over BLE after physical approval, before
Pairing Commit. Clients that omit it receive `Device 1`, `Device 2`, or `Device 3`.
The nickname and key are saved atomically, not on every edit or connection.
Existing labels are preserved. This naming flow needs no Wi-Fi/AP connection.

1. The timer must be open.
2. In Settings → Bluetooth, enable Bluetooth.
3. Select **Pair device**. This opens a two-minute pairing window.
4. Connect from the trusted app and complete OS link pairing.
5. Briefly press and release **Reset** to approve the connected app.
6. The app reads a pending encrypted setup key, stores it durably, then sends
   an authenticated Pairing Commit proof. Only that proof promotes the new
   key and completes pairing.

When explicitly replacing an app, its key remains valid until the timer durably
accepts the replacement Pairing Commit. Adding to an empty slot leaves all
existing approvals intact. Reading the pending secret does not
replace it. After commit, only the new key works; the app retains its pending
credential so a lost commit acknowledgement can be recovered by authentication.

### Pair with no Wi-Fi or AP connection

1. The timer must be open.
2. Hold the physical **Reset** button for 3–5 seconds, then release it. This
   enables the saved Bluetooth preference and opens a two-minute pairing
   window. The blue LED pulses once per 1.6 seconds while waiting.
3. Connect from the app and complete OS link pairing. The blue pattern changes
   while the encrypted connection is prepared.
4. Briefly press and release **Reset** to approve that connected app. The blue
   LED flashes rapidly and the app gets a full 60 seconds from this approval
   to finish setup, even near the end of the original two-minute window.
5. The app reads the pending 16-byte Pairing Secret, stores it durably, and
   writes the authenticated Pairing Commit frame.
6. The device completes replacement when it persists the pending key. The app
   then confirms authentication before promoting its local credential. Failure
   before durable commit preserves the prior key; failure afterward requires
   recovery with the new pending key, not the old one.

### Same-phone browser-key recovery

If browser storage was cleared but the OS bond still exists, close other apps
that might reconnect, disconnect the phone, open a new physical pairing window,
and connect again. The existing encrypted/bonded link is accepted as a candidate,
but still needs a fresh short Reset approval and Pairing Commit. Merely
auto-connecting cannot replace the application key. The prior key remains valid
until commit. A secret read can be repeated in `SecretReady` or
`WaitingForCommit` by that same selected encrypted/bonded peer until expiry.

This is different from losing the OS bond too. A mismatched/stale OS bond may
need to be forgotten in the phone's Bluetooth settings. If the timer still has
an approved controller and its bond is no longer usable, use **Remove approved
app** in the timer-hosted settings, then forget the old OS entry and pair afresh.
The ten-second physical factory reset also recovers access but clears all user
settings. The firmware does not silently delete a working bond on an unsolicited
SMP repeat-pairing request.

A short Reset press outside an active pairing window continues to arm OTA.
While any pairing window is active, a short press is reserved for pairing and
does not arm OTA. Holding Reset for ten seconds performs a factory reset and erases
Wi-Fi/AP settings, device name, Bluetooth preference, key, and bonds. Firmware
itself is unchanged.

### Revoke or disable

**Remove app** erases that approval and any bond no other app needs.
**Remove all approved apps** erases every application key and all BLE bonds;
the timer must be open. Turning Bluetooth off only disables the radio. These
are deliberately separate actions so a temporary radio shutdown does not
force re-pairing.

## Limits and advertising

While Bluetooth is enabled, the disconnected timer advertises continuously
so approved apps can discover it and reconnect. Advertising stops during a
connection and the authentication-failure backoff, pauses for the few seconds
of a Wi-Fi network scan (outside a pairing window), and turning Bluetooth off
stops it and returns the stack's memory. The service UUID and `OST-…` name are public discovery metadata;
advertising does not open pairing approval or expose timer state and controls.

| Property | Value |
| --- | --- |
| Advertised name | `OST-<last-six-device-ID-characters>` |
| Primary service | `70ba26f4-2f3d-44df-9b73-bfb3d3625961` |
| Inbound application connections | 1 |
| Compiled controller slots | 1 |
| Approved app credentials | 3, each with an independent key and label |
| Stored bonds | Up to 4 during atomic replacement; cleanup retains every approved peer |
| Peer scan/client role | Not exposed by the application; retained in the framework libraries |
| PHY | Ordinary BLE 1M |
| Periodic live keepalive | 15 seconds |
| Public metadata connection | One slot, automatically closed after 30 seconds if it does not authenticate |

REST, SSE, and BLE may be used together. A BLE connection does not consume an
HTTP SSE slot. The [management extension](BLUETOOTH_MANAGEMENT.md) gives an
authenticated app the same settings as the local HTTP API (Wi-Fi and AP setup,
device name, restart, app approvals, Wi-Fi scan and firmware update), running the
same firmware rules. Factory reset and physical pairing approval stay
button-only, and firmware updates still require the physical Reset-button arm.

## GATT characteristics

| Characteristic | UUID | Access |
| --- | --- | --- |
| Protocol information | `8d826f5b-7282-499f-bbde-fe361c19a8ed` | Public read |
| Capabilities | `6fb27438-e619-4291-ae73-448a1e973c5a` | Public read |
| System information | `f00df299-464f-4921-9285-7749593b1144` | Authenticated read |
| Live status | `d1a93d5e-d4fe-434e-acc6-42d86d79ab63` | Authenticated read/notify |
| Event detail | `917421bb-67a0-4b34-92a5-5219427a670c` | Authenticated read/notify |
| Control challenge | `eab30710-b1bb-42eb-ba07-115d18d91f64` | Encrypted read |
| Control command | `1fa0980f-7224-4d51-a315-c84fde9cbdc5` | Encrypted write-with-response |
| Control result | `2df3b9a1-f2ad-4660-97f5-14325d9eb6db` | Authenticated read/notify |
| Pairing secret | `546d8ce1-3570-4f2b-a1d8-c5632cb4f091` | Physical-confirmation encrypted read |
| Pairing nickname | `63af724d-0379-4191-b785-4e6baa457612` | Encrypted read / physically gated write-with-response |
| Management request | `4b546245-44a0-4cd0-9eed-8be6c51195ba` | Authenticated fragmented write ([management](BLUETOOTH_MANAGEMENT.md)) |
| Management response | `93b52e61-25f4-46a6-a597-5724013f60f9` | Authenticated read/notify ([management](BLUETOOTH_MANAGEMENT.md)) |

All numeric values are little-endian.

### Pairing nickname extension

Protocol information advertises `pairing_nickname: true`. Existing command,
result, event and capability frames are unchanged. The new characteristic is
appended to the service, preserving prior characteristic handles.

Read format: byte 0 = record version `1`; byte 1 = flags; remaining 0–24 bytes
are UTF-8 (no trailing NUL). Flag `0x02` means the physically approved pending
pairing window is open; `0x01` means the local owner supplied the nickname and
the client must retain it. After commit, only the authenticated app may read
its own nickname (flags zero). Other approvals' labels are not exposed here.

Write format: just 1–24 UTF-8 bytes, without NUL or control characters. Writes
require the same selected, encrypted, bonded, physically approved connection
and deadlines as the Pairing Secret. They are rejected after commit, before
physical approval, or if the local owner set a nickname. This changes only RAM;
the existing authenticated Pairing Commit atomically persists key and nickname.
Clients should write and verify readback before committing. This characteristic
does not grant device renaming, approval revocation, or any timer authority.

For headless Bleak pairing, open the physical window first, then run:

```sh
python3 examples/bleak_control.py --name OST-8481b0 --key-file my-phone.key pair --nickname "My phone"
```

The companion app provides the same field in Settings. Omit the nickname for
automatic slot naming. Changing the timer's own name remains a local REST/UI
setting; reading its identity and controlling the timer over BLE needs no Wi-Fi.

### Protocol information

This is UTF-8 JSON suitable for a GATT long read:

```json
{"protocol":"ost-ble/1","firmware":"1.0.0","model":"MagBoundV2","device_id":"b08184f8e894","device_name":"Smart Timer","hostname":"smarttimer","ap_ssid":"SMARTTIMER-E8E894","network_mode":"wifi","boot_id":"7ca9f21b","build_id":"4238efeaff57dd027e60c83e","control":"bonded_hmac","approved_app_limit":3,"pairing_nickname":true,"required_mtu":35,"management":1,"management_max_frame":512}
```

`device_name`, `hostname`, `ap_ssid`, and `network_mode` are the persistent
identity/network configuration present when BLE starts. Dynamic connection,
address, and memory values live in System Information. `build_id` is a
deterministic source and target-configuration fingerprint. Release `BUILD.json`
records this value alongside the exact image SHA-256.

### Capabilities frame (24 bytes)

V2 clients must validate this complete frame before pairing or authenticating.
The V2 development protocol was never publicly released, so there is no
fallback schema: firmware and controller software must agree on all declared
sizes.

| Offset | Size | Meaning |
| ---: | ---: | --- |
| 0 | 1 | Protocol major, `1` |
| 1 | 1 | Operation bitmap: bit 0 fixed start, 1 random start, 2 release, 3 pause, 4 resume, 5 adjust, 6 lock controls, 7 identify (`0xff`) |
| 2 | 1 | Security/transport bitmap: bit 0 encrypted link, 1 bonding, 2 application authentication, 3 command MAC/counter, 4 status notify, 5 event notify/replay, 6 physical pairing plus commit, 7 authenticated system information (`0xff`) |
| 3 | 1 | Inbound connection limit, `1` |
| 4 | 4 | Maximum duration milliseconds |
| 8 | 4 | Periodic status interval milliseconds |
| 12 | 4 | Physical pairing window milliseconds |
| 16 | 1 | Command frame bytes, `32` |
| 17 | 1 | Result frame bytes, `20` |
| 18 | 1 | Status frame bytes, `20` |
| 19 | 1 | Event-history capacity, `16` |
| 20 | 1 | Event frame bytes, `20` |
| 21 | 1 | System-information frame bytes, `44` |
| 22 | 1 | Command MAC bytes, `8` |
| 23 | 1 | Authentication and Pairing Commit MAC bytes, `14` |

### System information frame (44 bytes)

System Information is readable only after link encryption and application-key
authentication. The firmware refreshes the snapshot every five seconds; it is
intended for support and display, not tight control loops.

| Offset | Size | Meaning |
| ---: | ---: | --- |
| 0 | 1 | Protocol major, `1` |
| 1 | 1 | ESP reset reason: 0 unknown, 1 power-on, 2 external, 3 software, 4 panic, 5 interrupt watchdog, 6 task watchdog, 7 other watchdog, 8 deep-sleep, 9 brownout, 10 SDIO |
| 2 | 1 | Wi-Fi state: 0 AP Mode, 1 connecting, 2 connected, 3 failed, 4 disconnected, 5 temporarily disabled (Bluetooth only) |
| 3 | 1 | Flags: bit 0 station connected, 1 AP running, 2 configured AP Mode, 3 station retry paused, 4 BLE connected, 5 app authenticated |
| 4 | 4 | Uptime milliseconds |
| 8 | 4 | Current free heap bytes |
| 12 | 4 | Largest free heap block bytes |
| 16 | 4 | Minimum free heap bytes observed by the platform |
| 20 | 4 | Installed application size bytes |
| 24 | 4 | Maximum currently available OTA application space bytes |
| 28 | 4 | Physical flash bytes |
| 32 | 4 | Station IPv4 octets; all zero when not connected |
| 36 | 1 | Signed station RSSI dBm; zero when disconnected |
| 37 | 1 | Current AP client count |
| 38 | 1 | AP client limit |
| 39 | 1 | Heap fragmentation percent |
| 40 | 2 | Negotiated BLE MTU bytes |
| 42 | 1 | Signed BLE RSSI dBm; `127` when unavailable |
| 43 | 1 | Reserved; zero |

### Application authentication

Read the encrypted 20-byte challenge:

| Offset | Size | Meaning |
| ---: | ---: | --- |
| 0 | 1 | Protocol major, `1` |
| 1 | 1 | bit 0 paired key loaded; bit 1 this connection authenticated; bit 2 Pairing Commit durably accepted on this connection |
| 2 | 1 | Failed authentication/integrity attempts |
| 3 | 1 | Failure limit, `5` |
| 4 | 4 | Current boot ID |
| 8 | 12 | Random challenge nonce |

Choose a random four-byte client nonce and write this 20-byte authentication
frame to Control Command:

| Offset | Size | Meaning |
| ---: | ---: | --- |
| 0 | 1 | `1` |
| 1 | 1 | Authenticate opcode, `0x01` |
| 2 | 4 | Client nonce |
| 6 | 14 | First 14 bytes of authentication HMAC |

```text
context = LE32(boot_id) || challenge_nonce[12] || client_nonce[4]
auth_tag = HMAC-SHA256(app_key, "OST-AUTH-v1" || context)[0:14]
session_key = HMAC-SHA256(app_key, "OST-SESSION-v1" || context)
```

Authentication is rate-limited to one attempt per 500 ms. The challenge
rotates after success/failure and on connection changes. The derived 32-byte
session key exists only for that connection and is erased on disconnect or
host reset. Five failures disconnect that peer and pause advertising for 30
seconds; they do not erase or globally disable the approved key.

### Pairing Commit frame (20 bytes)

After reading and durably storing the pending Pairing Secret, write this frame
to Control Command:

| Offset | Size | Meaning |
| ---: | ---: | --- |
| 0 | 1 | Protocol major, `1` |
| 1 | 1 | Pairing Commit opcode, `0x02` |
| 2 | 4 | Random client nonce |
| 6 | 14 | First 14 bytes of the commit HMAC |

```text
commit_tag = HMAC-SHA256(pending_key,
  "OST-PAIR-COMMIT-v1" || LE32(boot_id) || client_nonce[4])[0:14]
```

The pending key is not authoritative until this proof is verified and the
device successfully commits it. Re-reading the secret returns the same pending
key while the window remains open. The device sets challenge flag `0x04` after
the main loop has durably accepted the commit and gives the connection a fresh
30-second authentication grace period. The app must then authenticate with the
pending key before promoting it locally. If the commit response or connection
is lost, reconnect and try that authentication proof; retain both the prior key
and `.pending` recovery file until one key is proven.

### Command frame (32 bytes)

| Offset | Size | Meaning |
| ---: | ---: | --- |
| 0 | 1 | Protocol major, `1` |
| 1 | 1 | Opcode |
| 2 | 2 | Nonzero request ID |
| 4 | 4 | Exact boot ID |
| 8 | 4 | Exact timer session sequence; Start uses `0` |
| 12 | 4 | Signed value A |
| 16 | 4 | Signed value B |
| 20 | 4 | Strictly increasing per-authentication command counter |
| 24 | 8 | First 8 bytes of command HMAC |

```text
command_tag = HMAC-SHA256(session_key, "OST-CMD-v1" || frame[0:24])[0:8]
```

On one authenticated connection, an exact retry may reuse the immediately
previous counter and all 32 identical bytes. Any modified replay or older
counter fails before the timer parser.

| Opcode | Operation | Session | Value A | Value B |
| ---: | --- | --- | --- | --- |
| `0x10` | Start fixed | `0` | duration ms | `0` |
| `0x10` | Start random | `0` | minimum ms | maximum ms |
| `0x11` | Release | current | `0` | `0` |
| `0x12` | Pause | current | `0` | `0` |
| `0x13` | Resume | current | `0` | `0` |
| `0x14` | Adjust | current | signed delta ms | `0` |
| `0x15` | Lock software controls | current | `0` | `0` |
| `0x16` | Blink Identify LED | current | `0` | `0` |

The parser caches eight request IDs for the current boot. Its identity compares
the semantic bytes `0..19` (protocol, opcode, request ID, boot/session and
values), deliberately excluding the connection-specific counter and MAC.
Repeating that semantic request after reconnecting and re-authenticating returns
its cached result without reapplying it. Reusing a request ID with different
semantic fields returns `operation_conflict`.

### Result frame (20 bytes)

| Offset | Size | Meaning |
| ---: | ---: | --- |
| 0 | 1 | Protocol major, `1` |
| 1 | 1 | Echoed opcode |
| 2 | 2 | Echoed request ID |
| 4 | 1 | Result code |
| 5 | 1 | bit 0 changed; bit 1 duplicate |
| 6 | 1 | Resulting state |
| 7 | 1 | Resulting release reason |
| 8 | 4 | Current boot ID |
| 12 | 4 | Resulting session sequence |
| 16 | 4 | Resulting remaining milliseconds |

Result codes: `0 applied`, `1 invalid_frame`, `2 unauthorized`, `3
stale_boot`, `4 stale_session`, `5 invalid_state`, `6 invalid_duration`, `7
emergency_input_active`, `8 controls_locked`, `9 maximum_duration_reached`,
`10 unsupported_opcode`, `11 operation_conflict`, `12 system_transition`,
`13 control_disabled`. Code 13 answers Pause or Lock controls when the owner
has switched that control off in the timer's settings ([API](API.md#session-controls)).
The final code is returned without applying a mutation while OTA, restart, or
a network-setting transition owns the shared recovery gate.
`emergency_input_active` also rejects Start, Resume and a positive Adjust while
the circuit is asserted; a negative Adjust is still applied.

The timer checks a command in the same order as the local HTTP API: an unknown
opcode, then values (`invalid_frame` or `invalid_duration`), boot, session, an
accepted no-op when the requested state is already true, `controls_locked`,
`invalid_state`, `emergency_input_active` and finally
`maximum_duration_reached`. For example, Resume on a running timer is
`applied` with `changed=0` even after controls are locked, and Start on an
active timer is `invalid_state` even while the emergency input is asserted.

Subscribe before writing. Correlate request ID, opcode, boot, applicable session,
and plausible resulting state before claiming success. A stale-boot or
system-transition rejection may report a different boot; a stale-session
rejection may report the newer session.

If either the write acknowledgement or notification is lost, read the cached
result. On the same authenticated connection, at most one retry of the identical
32-byte frame is used by the app. Across reconnects/reloads, it retains the
original semantic operation/ID and reads for reconciliation but does **not**
automatically resend: the eight-entry deduplication cache may have been evicted.
No correlated result means **outcome unknown**, not “not applied.”

Fresh command results update the app immediately. The firmware refreshes its
readable Live Status before publishing the result. A duplicate describes the
original result, not necessarily the current state; display a fresh status read
instead of applying a historical duplicate as if it just occurred.

### Live status and event frames

Live status is 20 bytes: protocol/state/flags/release reason followed by
remaining ms, session-duration ms, event revision, and session sequence as
four `uint32` values. Flags are controls locked (`0x01`), emergency active
(`0x02`), output energized (`0x04`), pause switched off by the owner (`0x08`)
and control locking switched off by the owner (`0x10`); a clear bit means the
control is allowed, which is also what firmware without these bits reports. A notification is sent on meaningful
change and at least every 15 seconds; clients should animate countdowns locally.

Event detail is 20 bytes:

| Offset | Size | Meaning |
| ---: | ---: | --- |
| 0 | 1 | Protocol major, `1` |
| 1 | 1 | `TimerEventType` numeric value |
| 2 | 1 | State captured when the event occurred |
| 3 | 1 | bit 0 controls locked at event time; bit 1 emergency active at event time; bit 2 history truncated |
| 4 | 4 | Event ID |
| 8 | 4 | Event uptime milliseconds |
| 12 | 4 | Signed event value milliseconds |
| 16 | 4 | Session sequence captured at event time |

Event codes are stable: `0` boot, `1` started, `2` remaining set, `3` paused,
`4` resumed, `5` adjusted, `6` controls locked, `7` expired, `8` opened by
user, `9` emergency release, `10` emergency input activated, `11` emergency
input cleared, `12` adjusted to zero, `13` Wi-Fi connecting, `14` Wi-Fi
connected, `15` Wi-Fi failed, `16` standalone/AP mode, `17` Wi-Fi
disconnected, `18` DHCP timeout, `19` OTA started, `20` OTA succeeded, `21`
OTA failed, `22` restart requested, `23` device renamed, `24` identify
started, `25` AP configured, `26` Bluetooth pairing started, `27` Bluetooth
pairing completed, and `28` Bluetooth pairing revoked. Unknown future values
must be ignored rather than treated as another event.

Status and event cursors are independent. Subscribed clients receive retained
events oldest-first from the fixed 16-entry ring. Every replayed state, flag,
and session field is the event-time snapshot, not the current timer state. If a
client falls behind the ring, the first delivered event sets the
history-truncated flag. A failed event notification is retried without
advancing its cursor.

## Local HTTP management

The endpoints below are described in full at
[`API.md § GET /api/v1/bluetooth`](API.md#get-apiv1bluetooth-v2-only) and
[`API.md § Bluetooth pairing management`](API.md#bluetooth-pairing-management-v2-only).
Response shape is normalized as
[`schemas/local/bluetooth-status.schema.json`](schemas/local/bluetooth-status.schema.json).

| Endpoint | Purpose | API.md anchor |
| --- | --- | --- |
| `GET /api/v1/bluetooth` | Full radio, pairing, security, and connection status | [`get-apiv1bluetooth-v2-only`](API.md#get-apiv1bluetooth-v2-only) |
| `GET /api/v1/bluetooth/enable` | Persistently enable radio; timer must be open | [`bluetooth-pairing-management-v2-only`](API.md#bluetooth-pairing-management-v2-only) |
| `GET /api/v1/bluetooth/disable` | Persistently disable radio; timer must be open | [`bluetooth-pairing-management-v2-only`](API.md#bluetooth-pairing-management-v2-only) |
| `POST /api/v1/bluetooth/pairing/start` | Enable radio and open the two-minute physical-confirmation pairing window | [`bluetooth-pairing-management-v2-only`](API.md#bluetooth-pairing-management-v2-only) |
| `POST /api/v1/bluetooth/pairing/cancel` | Cancel a pending ceremony without changing the approved key | [`bluetooth-pairing-management-v2-only`](API.md#bluetooth-pairing-management-v2-only) |
| `POST /api/v1/bluetooth/pairing/revoke` | Erase app key and bonds; timer must be open | [`bluetooth-pairing-management-v2-only`](API.md#bluetooth-pairing-management-v2-only) |

Bluetooth capability discovery lives under `GET /api/v1/capabilities`
(see [`schemas/local/capabilities.schema.json`](schemas/local/capabilities.schema.json))
and V2 diagnostics fields live under `GET /api/v1/system/diagnostics`
(see [`schemas/local/system-diagnostics.schema.json`](schemas/local/system-diagnostics.schema.json)).

## Bleak client and validation

[`examples/bleak_control.py`](../examples/bleak_control.py) is the executable
reference implementation:

```sh
# Public discovery, no key required
python3 examples/bleak_control.py --name OST-8481b0 info

# After holding Reset 3–5 seconds
python3 examples/bleak_control.py --name OST-8481b0 \
  --key-file ~/.config/opensmarttimer/OST-8481b0.key pair

# These can reuse that durably stored key file
python3 examples/bleak_control.py --name OST-8481b0 \
  --key-file ~/.config/opensmarttimer/OST-8481b0.key status
python3 examples/bleak_control.py --name OST-8481b0 watch --seconds 60
python3 examples/bleak_control.py --name OST-8481b0 start --duration-ms 900000
python3 examples/bleak_control.py --name OST-8481b0 pause
python3 examples/bleak_control.py --name OST-8481b0 resume
python3 examples/bleak_control.py --name OST-8481b0 adjust --delta-ms 60000
python3 examples/bleak_control.py --name OST-8481b0 identify
python3 examples/bleak_control.py --name OST-8481b0 release
```

Test with the controlled load disconnected. See
[Testing and verification](VALIDATION.md) for hardware verification limits and
[Installation](INSTALLATION.md) for recovery.

## Timer-to-timer control (not included)

The application's role is peripheral/GATT-server only, with one app
connection. The V2 framework is built the same way: central, observer/scanner
and GATT-client support and the second link are not compiled in, which keeps
Bluetooth small enough to run beside remote sharing. There is no peer-discovery
endpoint, and timer-to-timer mirroring is not implemented; adding it would need
those roles restored in a new framework profile.
