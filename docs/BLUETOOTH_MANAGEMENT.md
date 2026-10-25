# Bluetooth device management (`ost-ble/1` management extension 1)

This extension lets an approved Bluetooth app use the same settings as the
local HTTP API: Wi-Fi and AP setup, device name, restart, app approvals, Wi-Fi
scanning and firmware updates. It is V2-only, like the rest of
[Bluetooth](BLUETOOTH.md).

Management operations use the shared firmware rules. Most map to an
existing HTTP route and run the same firmware function, with the same checks,
the same JSON body and the same HTTP-style status code, except for the compact
terminal receipt when an app removes its own authority (below). The temporary Wi-Fi suspend/resume operations are Bluetooth-only so a local
Wi-Fi page cannot accidentally shut down its own access path. The timer
authority (`TimerController`), the physical Reset-button arm for firmware
updates, and the "timer must be open" rule for configuration all apply unchanged.

Every approved, authenticated BLE app has administrative access to all
pairings, not just its own. It can rename/revoke another approval, revoke all,
or open a pairing window subject to the existing state checks. There is no
separate owner role. Adding or replacing an approval still requires physical
Reset-button confirmation; revocation does not require another button press.

## Discovery

Protocol information (see [Bluetooth](BLUETOOTH.md#protocol-information)) adds:

```json
{"management":1,"management_max_frame":512}
```

A client must not use the characteristics below unless `management` is `1`.
Older firmware omits the key. Two characteristics are appended to the primary
service, after Pairing nickname, so earlier handles do not move:

| Characteristic | UUID | Access |
| --- | --- | --- |
| Management request | `4b546245-44a0-4cd0-9eed-8be6c51195ba` | Encrypted, authenticated write (fragments below) |
| Management response | `93b52e61-25f4-46a6-a597-5724013f60f9` | Authenticated read (long reads allowed) and notify |

Both require an encrypted, bonded link and a completed application
authentication on this connection. An encrypted, bonded connection without
application authentication receives service-specific ATT error `0x83`, not
*Insufficient Authentication*: repeating OS pairing cannot repair a missing
application key. An unencrypted link receives `0x0f`, and an unbonded link
receives `0x05`. A request rejected before application authentication is not
counted as an integrity failure. The only exception is the bounded, read-only
self-revocation receipt described below.

Release checks allow exactly the existing command/nickname writers and this
management writer. Only the management writer allows unacknowledged fragments;
all three require encryption. Management callbacks authenticate and queue work;
the main loop rechecks the authorization generation, approved app, encryption
and bond before dispatch. Source-policy fault tests reject removed safeguards.

All numbers are little-endian. A frame (request or response value) is at most
512 bytes, the ATT attribute limit. The timer prefers an ATT MTU of 247; clients
should let the OS negotiate the largest MTU it supports.

### Request fragments

The timer's NimBLE build allows only four prepared-write entries, so requests
are not sent as ATT long writes. Instead each write to Management request
carries one fragment:

| Offset | Size | Meaning |
| ---: | ---: | --- |
| 0 | 1 | Fragment index, 0–31 |
| 1 | 1 | Flags: bit 0 = last fragment; other bits zero |
| 2 | ≤ MTU − 5 | Next bytes of the request frame |

Index 0 starts a new request and discards any unfinished one. Each following
index must be exactly one more than the previous. Non-final fragments may use
write-without-response; the final fragment must use write-with-response, and its
ATT result reports the outcome of receiving the whole frame:

| ATT result | Meaning |
| --- | --- |
| Success | Frame authenticated and queued; wait for the ready notification |
| `0x80` | Another request is still being processed; resend the whole frame shortly |
| `0x81` | Fragment out of order, missing or over 512 bytes; resend the whole frame |
| `0x82` | Frame malformed (version, lengths) |
| `0x83` | Application authentication required, revoked session, or MAC/counter failure. Only a failed authenticated integrity check counts toward the failure limit |
| `0x0f` / `0x05` | Link encryption / bonding required; distinct from application authentication |

The response is read with an ordinary GATT read; Web Bluetooth and other common
client stacks use read-blob automatically for values longer than the MTU.

## Request frame

| Offset | Size | Meaning |
| ---: | ---: | --- |
| 0 | 1 | Management version, `1` |
| 1 | 1 | Opcode (table below) |
| 2 | 2 | Nonzero request ID, chosen by the client |
| 4 | 4 | Exact boot ID |
| 8 | 4 | Counter: shares the strictly increasing per-authentication counter with control commands |
| 12 | 2 | Body length `B` |
| 14 | 1 | Secret length `S`, 0–64 |
| 15 | `B` | Body. Flat UTF-8 JSON object for every opcode except `0x71`, which is binary |
| 15+B | `S` | Secret ciphertext (AES-128-CTR, below) |
| 15+B+S | 16 | `HMAC-SHA256(session_key, "OST-MGMT-v1" ‖ frame[0 : 15+B+S])[0:16]` |

The counter rule is the command rule: each authenticated request must use a
counter greater than the last accepted command or management counter on this
connection, except an exact byte-for-byte retry of the immediately previous
frame. A bad MAC or an old counter counts as an integrity failure (five end the
connection, as for commands). Only one request may be outstanding; a write while
another is being processed is rejected with ATT error `0x80` (*busy, retry*).

The JSON body uses the same field names and types as the matching HTTP request
body, parsed by the same strict flat-object parser (no duplicate or unknown
fields, integers only). The secret field is never placed in the JSON. For the
two opcodes that carry a password, the firmware decrypts the secret and supplies
it as the `password` field.

### Secret encryption

The link is already encrypted, but "Just Works" pairing is not authenticated,
so passwords get an application-layer layer too:

```text
enc_key = HMAC-SHA256(session_key, "OST-MGMT-ENC-v1")[0:16]
iv      = LE32(counter) ‖ LE16(request_id) ‖ 0x00 × 10
secret  = AES-128-CTR(enc_key, iv, plaintext)
```

The counter is unique per session key, so an IV is never reused. The frame MAC
covers the ciphertext (encrypt-then-MAC). The firmware never returns a password
in any response; reads report only `password_set`.

## Response

When the firmware finishes a request it sets the Management response value and
notifies an 8-byte ready frame:

| Offset | Size | Meaning |
| ---: | ---: | --- |
| 0 | 1 | Management version, `1` |
| 1 | 1 | Echoed opcode |
| 2 | 2 | Echoed request ID |
| 4 | 2 | HTTP-equivalent status (200, 202, 400, 403, 409, 413, 503, …) |
| 6 | 2 | Total body length `T` |

The client then reads the Management response characteristic:

| Offset | Size | Meaning |
| ---: | ---: | --- |
| 0 | 1 | Management version, `1` |
| 1 | 1 | Echoed opcode |
| 2 | 2 | Echoed request ID |
| 4 | 2 | HTTP-equivalent status |
| 6 | 2 | Total body length `T` (at most 8192) |
| 8 | 2 | Segment offset |
| 10 | 2 | Segment length `L` |
| 12 | `L` | Body bytes `[offset, offset+L)` (UTF-8 JSON, identical to the HTTP body) |
| 12+L | 16 | `HMAC-SHA256(session_key, "OST-MGMT-RSP-v1" ‖ LE32(request_counter) ‖ value[0 : 12+L])[0:16]` |

The first read returns offset 0. If `L < T`, fetch the rest with **Continue**
(`0x3f`), whose JSON body is `{"request_id":N,"offset":K}`. Its response carries
the next segment of the same stored body and echoes the original opcode and
request ID. The firmware keeps only the most recent response; a new request
replaces it. For Continue, `request_counter` is the Continue frame's own counter.
A missed ready notification is harmless: the client may read the response
characteristic after its final write completes and check the echoed request ID
and a valid MAC. The ready frame itself carries no MAC; it is protected by the
encrypted link. Clients read the authenticated response for everything except
firmware chunks, where the ready status alone is enough because Finish checks
the whole image's length and CRC32.

Error bodies are the HTTP error object, for example
`{"error":"network changes require an open timer","code":"timer_active","state":"running"}`.

### Retries

Mutating opcodes (all except reads and Continue) are remembered per approved
app: the last four results, keyed by request ID and a hash of opcode, request
ID, boot ID, body and decrypted secret. Repeating the same request, including
after reconnecting and re-authenticating with a new counter, returns the saved
status and body without running it again. Reusing a request ID with different
contents returns `409 operation_id_conflict`. A request with a different boot
ID returns `409 stale_boot`.

## Operations

Every row runs the same firmware function as the HTTP route. Bodies and
responses are the ones documented in the [API](API.md).

### Read state and settings

| Opcode | Operation | HTTP equivalent |
| ---: | --- | --- |
| `0x3f` | Continue a long response | — |
| `0x40` | Wi-Fi status | `GET /api/v1/wifi/status` |
| `0x41` | Access point settings | `GET /api/v1/wifi/ap` |
| `0x42` | Device name | `GET /api/v1/device/name` |
| `0x43` | Bluetooth status and approved apps | `GET /api/v1/bluetooth` |
| `0x44` | Diagnostics | `GET /api/v1/system/diagnostics` |
| `0x45` | Current-boot events | `GET /api/v1/events` |
| `0x46` | Capabilities | `GET /api/v1/capabilities` |
| `0x47` | Timer and update status | `GET /api/v1/status` |

### Change settings

| Opcode | Operation | HTTP equivalent | Secret |
| ---: | --- | --- | --- |
| `0x50` | Join a Wi-Fi network | `POST /api/v1/wifi/configure` | Wi-Fi password |
| `0x51` | Forget Wi-Fi (AP Mode) | `POST /api/v1/wifi/forget` | — |
| `0x52` | Access point settings | `PUT /api/v1/wifi/ap` | AP password |
| `0x53` | Rename the timer | `PUT /api/v1/device/name` | — |
| `0x54` | Restart | `POST /api/v1/system/restart` | — |
| `0x55` | Rename an approved app | `PUT /api/v1/bluetooth/apps/label` | — |
| `0x56` | Remove an approved app | `POST /api/v1/bluetooth/apps/revoke` | — |
| `0x57` | Open pairing (add or replace) | `POST /api/v1/bluetooth/pairing/start` | — |
| `0x58` | Cancel pairing | `POST /api/v1/bluetooth/pairing/cancel` | — |
| `0x59` | Remove all approved apps | `POST /api/v1/bluetooth/pairing/revoke` | — |
| `0x5a` | Turn Bluetooth off | `GET /api/v1/bluetooth/disable` | — |
| `0x5b` | Suspend Wi-Fi/AP while Bluetooth is connected¹ | Bluetooth only | `{}`; no secret |
| `0x5c` | Restore saved Wi-Fi/AP mode | Bluetooth only | `{}`; no secret |

¹ Firmware advertising `wifi_disconnect_restore_supported: true` restores
networking on disconnect or host reset. Earlier firmware keeps it suspended
until an explicit resume or restart.

Changes that restart the timer (AP settings, rename, restart) reply first and
restart about 1.5 seconds later, so the Bluetooth connection then drops. Joining
Wi-Fi, forgetting it, starting pairing and turning Bluetooth off can also end
or interrupt the current connection; clients should treat a disconnect after
a verified `202` response as expected. Bluetooth-off persists the disabled
setting **before** acknowledging it; a save failure returns `500` and does not
schedule radio shutdown. Radio shutdown follows the accepted response by about
750 ms. Saved approvals are retained when merely turning Bluetooth off.

### Removing the connected app

Removing another app returns the normal Bluetooth JSON status. Removing the
current app (`0x56`), or all apps (`0x59`), instead returns status `200` with:

```json
{"approval_removed":true,"disconnecting":true}
```

The registry change must be durably saved first. The timer then immediately
erases the revoked session's live authorization and session key. It signs this
single-segment receipt with a request-local copy of the old session key and
erases that copy after constructing the response. The same encrypted, bonded
connection can read **only this already-signed receipt** for up to two seconds;
all writes, continuation requests, reauthentication and other characteristic
access are denied. The connection is then closed and orphan-bond cleanup runs.
No control authority is retained during the receipt window.

Clients must verify the response MAC, request ID and status before clearing
their saved key. A lost reply or disconnect by itself is an **unknown outcome**,
not proof that revocation failed. Keep the local credential for reconciliation,
do not retry automatically, and inspect approvals through another approved app
or the local interface. Only physical pairing can restore a revoked approval.

### Wi-Fi scan

| Opcode | Operation | HTTP equivalent |
| ---: | --- | --- |
| `0x60` | Start a scan | `POST /api/v1/wifi/scan` |
| `0x61` | Scan results | `GET /api/v1/wifi/scan` |

The C3 shares one radio between Wi-Fi and Bluetooth. While a scan runs the
Bluetooth link can stall for a few seconds or drop; poll `0x61` every two
seconds until `state` is no longer `scanning`, and reconnect if needed. Results
larger than one segment use Continue.

### Firmware update

HTTP uploads arriving during an active BLE update receive an OTA-busy response.
For a signed GitHub release, `OtaBegin` also carries `release_version`,
`image_sha256` and `release_signature` with the usual image size and CRC.
The shared writer verifies the model-bound signature before beginning and
streaming SHA-256 before committing. The encrypted management frame and
472-byte chunk protocol are unchanged. See [GitHub updates](GITHUB_UPDATES.md).
Their upload callbacks do not reset, write, finalize or abort the active BLE
transfer's shared validator or updater. A regression test executes the actual
HTTP callbacks with host spies, including a competing aborted request.

| Opcode | Operation | Body |
| ---: | --- | --- |
| `0x70` | Begin | JSON `{"image_size":N,"image_crc32":"8 hex digits"}` |
| `0x71` | Chunk | Binary: `LE32 offset` ‖ image bytes (1–472) |
| `0x72` | Finish | JSON `{}` |
| `0x73` | Abort | JSON `{}` |
| `0x74` | Download on the timer | JSON `{}` or `{"release_version":"…"}`; the same as `POST /api/v1/ota/download` |

Firmware updates keep every HTTP rule: the timer must be open, the Reset button
must have armed updates with a short press (a second press still allows one
unverified image), no Wi-Fi scan may be running, and the image passes the same
streaming checks as a web upload (header, product marker or allowlisted vendor
rollback, size and CRC32), including holding back the final chunk until every
check has passed.

- **Begin** reserves the updater and starts the same quiet window as a web
  upload: HTTP answers `503 ota_in_progress`, and timer commands answer
  `system_transition`. It returns `202` with `{"chunk_bytes":472,"image_size":N,"unverified_firmware_allowed":false}`.
  Once it reaches metadata validation, Begin consumes the one-use physical
  arm even if the metadata or supplied signature is rejected. A new short
  Reset press is required before another Begin; HTTP preparation alone does
  not consume its arm.
- **Chunk** offsets must be exactly the number of bytes accepted so far.
  Resending the immediately previous chunk with identical bytes returns the same
  success without writing it twice, so a lost reply is safe to retry. Any other
  offset returns `409 ota_offset_mismatch` with `{"expected_offset":N}`, letting
  a client resume after reconnecting within the quiet window.
- **Finish** validates length and CRC32, commits the image and returns
  `{"updated":true,"restarting":true}`. The timer restarts 1.5 seconds later and
  the new image confirms itself only after its boot health check, as for web
  uploads.
- **Abort**, a disconnect longer than the two-minute quiet window, or any failed
  check cancels the update; the running firmware is unchanged.

A chunk costs about three fragment writes and one notification. At typical
connection intervals that is roughly 4–8 KB/s, so a 1.3 MB image takes about
three to six minutes. Keep the phone close and the app open
while an update is in progress.

**Download on the timer** (`0x74`) avoids that transfer when the timer is on a
Wi-Fi network with Internet access: the app only starts it, and the timer
downloads, verifies and installs the image itself (see
[Download on the timer](API.md#download-on-the-timer-v2)). It needs the same
open timer and Reset-button arm, which it uses up, and returns `202` with
`{"started":true}`. Follow `ota_download` in status (`0x47`); the connection may
stay open or close. Every rejection uses the HTTP route's status and code.
Older firmware rejects the unknown opcode, so an app falls back to `0x70`–`0x73`.

## Limits

| Property | Value |
| --- | --- |
| Frame size (request and response value) | 512 bytes |
| Response body per request | 8192 bytes, in segments of up to 484 |
| Outstanding requests | 1 |
| Remembered mutation results | 4 per approved app |
| Secret length | 64 bytes |
| Firmware chunk | 472 bytes |
| Request fragments | 32 per frame |

## Test vectors

`tests/vectors/ost_ble_mgmt_v1.json` contains a request with an encrypted
secret, a response segment and the derived keys, generated from the reference
encoder in `tests/protocol/ost_ble_mgmt_v1_vectors_test.py`. The firmware codec
and the web app are each checked against it.

## Temporary Bluetooth-only mode

V2 Wi-Fi status (`0x40`) advertises `wifi_suspend_supported: true`. Clients
only show the option after reading this flag; older firmware remains compatible.
`0x5b` stops both the station and AP in RAM. While suspended, status reports
`state: "disabled"`, `network_mode: "bluetooth"`, no AP/station connection and
zero retry time. System Information uses Wi-Fi state code 5. There is no
automatic Wi-Fi retry or recovery AP while this deliberate mode is active.

Both operations require an authenticated approved app, an open timer and no
OTA/restart/network transition, credential test or scan in progress. Suspend
is also blocked until a newly installed OTA image has completed boot validation. They use
the existing signed receipts, counter checks and exact-request deduplication.
No DeviceConfig, EEPROM, NVS or persistent Wi-Fi settings are changed. A new
boot restores the saved Wi-Fi/AP configuration; `0x5c` does so without rebooting
or intentionally disconnecting Bluetooth. Current V2 firmware also advertises
`wifi_disconnect_restore_supported: true`: disconnecting Bluetooth or resetting
the Bluetooth host automatically restores the saved Wi-Fi/AP mode, even while
the timer is locked. Recovery is latched, so a rapid reconnect does not cancel
it. Wi-Fi operations run on the main loop, never in a Bluetooth callback. Saved
router mode keeps its usual recovery AP fallback if the router cannot connect.
Earlier firmware without this flag requires explicit resume or restart; clients
keep that older recovery guidance.

Scanning/joining/forgetting Wi-Fi is rejected until networking is restored.
Turning Bluetooth off is also rejected while Wi-Fi is suspended. Remote relay
control is unavailable while Wi-Fi is off; the relay's ordinary offline state
handles that loss of connectivity. Physical release and timer expiry are unchanged.
