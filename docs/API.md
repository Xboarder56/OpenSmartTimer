# OpenSmartTimer HTTP API

## Overview

This is the shared HTTP interface exposed by OpenSmartTimer on the **MagBound Smart Timer V1 (Legacy Timer)** and **MagBound Smart Timer V2**. It is served directly by the device over plain HTTP. There is no device login, API token, or TLS listener. Put the timer on a trusted or isolated IoT network.

API status describes firmware state and commanded output state; it is not proof that an external lock has mechanically released. Read the [safety, warranty, and liability notice](../SAFETY.md) first.

Typical base addresses are:

```text
http://192.168.4.1           setup/recovery AP
http://smarttimer.local      default station-mode mDNS name
http://<device-ip>           station address reported by your router
```

Changing the device name also changes the `.local` hostname. Read `/api/v1/status` or `/api/v1/device/name` to discover the active value.

Service-aware clients can browse `_smarttimer._tcp.local`. Each mDNS service record advertises port 80 plus TXT keys `api=v1`, `id`, `version`, `model`, `path=/api/v1/status`, and `events=/api/v1/events/stream`. Keep the ordinary `_http._tcp` record as a generic fallback.

Durations are integer milliseconds. Valid remaining duration is 1–86,400,000 ms (24 hours).

## Request and response conventions

Modern endpoints return JSON with `Cache-Control: no-store`, except for the explicitly opened SSE endpoint, which returns `text/event-stream`. Except for multipart OTA, mutation inputs may be either URL-encoded form fields or one flat JSON object.

Equivalent examples:

```sh
curl -X POST -d 'duration_ms=900000' \
  http://smarttimer.local/api/v1/timer/start

curl -X POST -H 'Content-Type: application/json' \
  -d '{"duration_ms":900000}' \
  http://smarttimer.local/api/v1/timer/start
```

JSON input is one complete, flat object with endpoint-specific field types:
integer milliseconds, boolean switches, and strings for names, credentials and
IDs. Only `if_session_id` accepts `null` (equivalent to no session precondition).
Use either fixed `duration_ms` or both `min_ms`/`max_ms`, never both forms.
JSON numeric strings, fractions/exponents, nested objects/arrays, unknown fields,
duplicate names, malformed/trailing content and invalid UTF-8 are rejected.
Standard string escapes and Unicode `\uXXXX` escapes, including surrogate pairs,
are decoded. Embedded NUL is rejected because the platform uses C strings.

Limits are 2,048 raw JSON bytes, 16 fields and 1,024 decoded bytes including
field names and terminators. An over-limit request returns `413 request_too_large`.
Other parsing failures return `400` with `invalid_json`, `duplicate_field`,
`invalid_field` or `invalid_field_type`. The core HTTP server receives the body
before this validation; these limits are not a general network DoS defense.
Form/query compatibility is retained, but duplicate fields are rejected. Do not
combine a JSON body with query/form arguments (`400 ambiguous_request`): a second
input source cannot silently override a boot/session precondition.
The ESP8266 server also exposes a raw copy of URL-encoded form data internally;
that copy is not treated as a second JSON body. This applies to normal forms
and OTA preparation on V1 as well as V2.

Successful timer mutations return the same complete object as `GET /api/v1/status`, plus a `mutation` object containing `action`, `changed`, `duplicate`, and the supplied `operation_id` or `null`. Accepted operations that will disrupt the current connection, such as Wi-Fi changes and restart, normally return HTTP 202.

Status serialization is bounded and checks every output allocation. If it cannot
construct a complete status object, REST returns `503` / `response_unavailable`
instead of publishing partial JSON; an SSE status stream is closed so clients
can reconnect or fall back to polling. A timer mutation may already have applied
before response construction fails. Read fresh status and use the existing
operation ID/retry rules; do not repeat an adjustment with a new ID blindly.

Errors preserve a readable message and provide a stable code and current timer state:

```json
{
  "error": "timer is already active",
  "code": "timer_active",
  "state": "running"
}
```

Error response schema: [`schemas/local/error-response.schema.json`](schemas/local/error-response.schema.json).
`code` is an extensible, nonempty string. Clients may recognize specific codes,
but must handle unknown ones using the HTTP status and readable `error` message.
For example, parser failures include `request_too_large`, `ambiguous_request`,
`duplicate_field`, `invalid_field` and `invalid_field_type`; temporary network
mode changes can report `wifi_disabled`, `network_mode_busy` or
`boot_validation_pending`. Examples in the shared schema are not a closed list.

Common status codes:

| Status | Meaning |
| ---: | --- |
| `200` | Request completed |
| `202` | Accepted; network transition or restart follows |
| `400` | Invalid or missing input |
| `403` | Origin or host rejected, or OTA not physically armed |
| `404` | Unknown API route |
| `409` | Valid request not allowed in the current device/timer state |
| `410` | Retired vendor cloud operation |
| `413` | Request body, decoded fields or field count exceeds the supported limit |
| `503` | Temporary device work, such as OTA traffic quieting, a scan-start failure, or all SSE slots being occupied |

Browser mutations are allowed only when `Origin` is absent or exactly `http://<Host>`. Requests explicitly identified as cross-site are rejected. Backend integrations such as `curl`, Home Assistant, or Node-RED normally omit `Origin` and remain compatible. The device does not enable permissive CORS.

Every API request must also be addressed to the timer by an IP address, a single-label name (`smarttimer`) or a name under a private suffix (`.local`, `.lan`, `.home.arpa`, `.internal`, `.localdomain`, `.home`); any other `Host` gets `403 host_rejected`. This keeps a website that re-points its own domain at the timer (DNS rebinding) out of the API. A local DNS name under a public domain you own is refused too; use the IP address or `.local` name instead. Requests over the timer's own setup access point are exempt, because phones open setup under their captive-portal check's name.

JSON Schema files for the core local API live under
[`schemas/local/`](schemas/local/); covered endpoint sections link to their
schemas. The directory README lists coverage and remaining gaps, including
relay owner messages and multipart OTA. The prose here remains the
human-readable reference. CI validates preview responses, SSE status, accepted
requests and regression fixtures using these schemas; this does not replace
checking captured hardware responses.

## Endpoint summary

| Method and path | Purpose |
| --- | --- |
| `GET /api/v1/status` | Compact timer/device/network polling snapshot |
| `POST /api/v1/timer/start` | Start a fixed or device-randomized duration |
| `POST /api/v1/timer/stop` | Open an unprotected active timer |
| `POST /api/v1/timer/release` | Preferred semantic alias for `stop` |
| `POST /api/v1/timer/pause` | Pause an unprotected running timer |
| `POST /api/v1/timer/resume` | Resume an unprotected paused timer |
| `POST /api/v1/timer/adjust` | Add or deduct remaining time |
| `POST /api/v1/timer/lock-controls` | Disable software mutations until expiry |
| `POST /api/v1/timer/seal` | Compatibility alias for locking controls |
| `POST /api/v1/wifi/scan` | Start a non-blocking nearby-network scan while open |
| `GET /api/v1/wifi/scan` | Read the current scan state and results |
| `GET /api/v1/wifi/status` | Detailed station/AP and provisioning state |
| `GET /api/v1/timer/controls` | Read whether pause and control locking are allowed |
| `POST /api/v1/timer/controls` | Save `allow_pause` and/or `allow_control_lock` while open |
| `GET /api/v1/wifi/ap` | Read setup-AP identity and security state |
| `PUT /api/v1/wifi/ap` | Change generated/custom AP name and open/WPA2 security |
| `POST /api/v1/wifi/configure` | Test and then commit candidate credentials |
| `POST /api/v1/wifi/forget` | Clear saved station Wi-Fi and use setup AP |
| `GET /api/v1/device/name` | Read friendly name, hostname, and AP SSID |
| `PUT /api/v1/device/name` | Save name and restart while open |
| `POST /api/v1/system/identify` | Rapidly blink the blue LED for five seconds |
| `POST /api/v1/system/restart` | Safely restart while open |
| `GET /api/v1/system/diagnostics` | Read bounded support/health information |
| `GET /api/v1/bluetooth` | V2-only radio, connection, pairing, authentication, and command status |
| `GET /api/v1/bluetooth/enable` | V2-only, persistently enable the radio while open |
| `GET /api/v1/bluetooth/disable` | V2-only, persistently disable the radio while open; pairing is retained |
| `POST /api/v1/bluetooth/pairing/start` | V2-only, open a physically confirmed app-pairing window while open |
| `POST /api/v1/bluetooth/pairing/cancel` | V2-only, cancel an in-progress ceremony without changing the approved app |
| `POST /api/v1/bluetooth/pairing/revoke` | V2-only, remove all approved apps and BLE bonds while open |
| `GET /api/v1/bluetooth/apps` | V2-only, Bluetooth status including approved apps and connection usage |
| `PUT /api/v1/bluetooth/apps/label` | V2-only, rename an approved app using `app_id` and `label` |
| `POST /api/v1/bluetooth/apps/revoke` | V2-only, remove only the specified `app_id` |
| `GET /api/v1/relay` | V2-only, experimental remote-relay status and diagnostics |
| `POST /api/v1/relay/enroll` | V2-only, stage owner-reviewed setup with `relay_url`, `partner_origin`, `enrollment_code`, `spki_pin`, `if_boot_id` while open |
| `POST /api/v1/relay/enroll/cancel` | Cancel the pending `setup_request_id` using current `if_boot_id` |
| `POST /api/v1/relay/enable` | V2-only, turn an enrolled relay back on while open |
| `POST /api/v1/relay/disable` | V2-only, end every share link and turn the relay off; allowed during a session |
| `POST /api/v1/relay/forget` | V2-only, erase relay credentials, links and signing keys while open |
| `GET /api/v1/relay/links` | V2-only, list current share links |
| `POST /api/v1/relay/links` | V2-only, asynchronous job: create a share link with `job_id`, `if_boot_id`, `ttl_s` and optional `label` |
| `GET /api/v1/relay/links/share` | V2-only, share URL for `fp_hash` once the relay confirmed it |
| `POST /api/v1/relay/links/revoke` | V2-only, asynchronous job: end the share link `fp_hash` (`job_id`, `if_boot_id`) |
| `POST /api/v1/relay/links/extend` | V2-only, asynchronous job: add `ttl_s` to share link `fp_hash`, same URL, also mid-session (`job_id`, `if_boot_id`) |
| `GET /api/v1/events` | Read the latest 16 current-boot events |
| `GET /api/v1/events/stream` | Open a full-status SSE stream; at most two clients on V1, three on V2 |
| `GET /api/v1/capabilities` | Discover limits and supported features |
| `POST /api/v1/ota/prepare` | Validate metadata and reserve a quiet OTA window |
| `POST /api/v1/ota` | Physically armed, validated firmware upload |

## Status

### `GET /api/v1/status`

Response schema: [`schemas/local/status.schema.json`](schemas/local/status.schema.json).

This is the preferred compact snapshot endpoint and the fallback for every client. The bundled UI serializes requests and adds a small randomized delay around one second so multiple polling pages do not repeatedly transmit at the same instant. It leaves larger support data to `/api/v1/system/diagnostics` and `/api/v1/events`.

```sh
curl http://smarttimer.local/api/v1/status
```

Example while running on station Wi-Fi:

```json
{
  "state": "running",
  "remaining_ms": 897412,
  "session_duration_ms": 900000,
  "sealed": false,
  "emergency_input_active": false,
  "max_duration_ms": 86400000,
  "api_version": "v1",
  "version": "1.0.0",
  "device_id": "12ab34",
  "hardware_model": "MagBound Smart Timer V1",
  "chip_family": "ESP8266",
  "build_id": "bb0629a425882236a0337089",
  "boot_id": "7ca9f21b",
  "session_id": "7ca9f21b-3",
  "state_revision": 42,
  "latest_event_id": 18,
  "device_name": "Smart Timer",
  "hostname": "smarttimer",
  "uptime_ms": 193411,
  "server_time_ms": 193411,
  "last_release_reason": "boot",
  "ap_ssid": "SMARTTIMER-12AB34",
  "network_mode": "wifi",
  "ap_running": false,
  "ap_role": "off",
  "ap_clients": 0,
  "ap_security": "wpa2",
  "ap_password_set": true,
  "ap_uses_device_name": true,
  "ota_armed": false,
  "ota_armed_remaining_ms": 0,
  "ota_unverified_allowed": false,
  "identifying": false,
  "station_connected": true,
  "station_ip": "192.168.1.42",
  "station_mac": "A4:CF:12:34:56:78",
  "connected_ssid": "Workshop-IoT",
  "saved_ssid": "Workshop-IoT",
  "target_ssid": "",
  "last_attempted_ssid": "Workshop-IoT",
  "wifi_state": "connected",
  "rssi_dbm": -48,
  "last_wifi_error": "",
  "retry_in_ms": 0,
  "station_retry_paused": false
}
```

Response fields:

| Field | Meaning |
| --- | --- |
| `state` | `open`, `running`, or `paused` |
| `remaining_ms` | Current remaining time; zero while open |
| `session_duration_ms` | Progress baseline retained across browser reloads |
| `sealed` | `true` when software controls are locked for the session |
| `pause_allowed` | `false` when the owner has switched pause off; pause then returns `409 control_disabled` (the legacy `/pause` returns `409` with body `failed`) |
| `control_lock_allowed` | `false` when the owner has switched control locking off; lock-controls then returns `409 control_disabled` (the legacy `/setLockMode` returns `409` with body `failed`) |
| `emergency_input_active` | `true` while the physical accessory circuit is asserted; outputs are open and start/resume/positive adjust are blocked |
| `last_release_reason` | `boot`, `expired`, `user`, `emergency`, `adjusted_to_zero`, or `share_link_expired` (V2 relay: the share link holding the session expired) |
| `share_link_limit_ms` | `null`, or ms until the timer opens by itself because the share link that started, extended, paused or sealed this session expires (V2 relay) |
| `share_links_active` | Relay builds only: number of live share links (someone else can control the timer while it is above zero) |
| `hardware_model` / `chip_family` | Exact product target and controller family selected at build time; use these to reject the wrong update artifact. |
| `build_id` | Deterministic source and target-configuration fingerprint. Match it to release `BUILD.json`; use that manifest's SHA-256 for exact artifact verification. |
| `boot_id` | Volatile identifier generated at boot. A changed value means the controller restarted and cached state must be discarded. It is not a secret. |
| `session_id` | Boot-scoped identity assigned when a timer starts, or `null` before the first session of a boot. It stays attached to that session after release until another session starts. |
| `state_revision` | Monotonic current-boot revision for meaningful timer/network/device state changes. Lower revisions are stale; a gap means a client should accept the full supplied snapshot or fetch status again. |
| `latest_event_id` | ID of the newest current-boot event; use it as the `after` cursor for `/api/v1/events`. |
| `station_mac` | Router-facing station-interface MAC address; use this for a DHCP reservation/static lease. It remains available in AP Mode. |
| `uptime_ms` / `server_time_ms` | Wraparound-safe device monotonic time |
| `ota_armed_remaining_ms` | Physical update window remaining; zero when disarmed |
| `ota_unverified_allowed` | `true` only after a second physical short press during the current arm window; permits one otherwise-unrecognized application image and is never persistent |
| `ap_security` | `open` or `wpa2`; this protects the setup AP only, not REST access through the router LAN |
| `ap_password_set` | Whether an AP password exists; the password itself is never returned |
| `ap_uses_device_name` | Whether AP SSID is generated from the device name and chip ID |
| `network_mode` | `ap` when no station network is saved, otherwise `wifi`; V2 temporary Bluetooth-only mode overrides this with `bluetooth` |
| `ap_role` | `primary` for intentional AP Mode, `recovery` when saved Wi-Fi is unavailable, or `off` |
| `wifi_state` | `standalone`, `connecting`, `connected`, `failed`, `disconnected`, or V2-only `disabled` while Wi-Fi/AP is suspended |
| `saved_ssid` | Last successfully committed station network |
| `target_ssid` | Candidate currently being tested |
| `last_attempted_ssid` | Most recent provisioning target, useful after failure |
| `rssi_dbm` | Station RSSI; zero when not connected |
| `retry_in_ms` | Delay until normal saved-network retry; zero when not applicable |
| `station_retry_paused` | `true` when a Recovery AP client is present and automatic saved-network attempts are being held to protect the AP session |

Neither this endpoint nor any other endpoint returns a Wi-Fi password.

### Live status with SSE

`GET /api/v1/events/stream` opens a Server-Sent Events connection. Merely installing or booting firmware does not create background traffic: a client must keep this GET open. Each bundled UI page opens one stream automatically, pauses its one-second status polling while the stream is healthy, and resumes REST polling plus a jittered SSE reconnect attempt if the stream closes. Timer actions and all non-status operations continue to use REST while SSE is connected.

```sh
curl -N http://smarttimer.local/api/v1/events/stream
```

The initial event and each meaningful state change carry the same complete JSON object as `/api/v1/status`. Emergency-input activation and clearing each advance the event and state revisions, so a connected stream receives the changed `emergency_input_active` value immediately. Use `latest_event_id` with `/api/v1/events` when the client also needs the detailed `emergency_input_activated`, `emergency_input_cleared`, or forced `emergency_release` record. A 15-second full-state refresh keeps long sessions synchronized without sending a one-second network update for the countdown, which clients can derive from `remaining_ms` and their receive time.

```text
retry: 5000

id: 7ca9f21b:18:42
event: status
data: {"state":"running","remaining_ms":897412,...,"boot_id":"7ca9f21b","state_revision":42}

```

The V1 accepts at most two simultaneous SSE streams and the V2 three (`sse_client_limit` in diagnostics), independent of the four-device AP association limit. The V1's TCP stack allows five connections in total, so a third stream would leave too few for page loads and commands. One phone can consume multiple SSE slots if it opens multiple tabs. When every slot is occupied, another stream receives `503 sse_client_limit`; ordinary REST requests remain available. Streams are closed before OTA traffic begins and by every reboot, including the physical factory reset. Clients must always retain REST polling as a fallback.

The HTTP/1.1 response uses `Transfer-Encoding: chunked`; each decoded chunk contains ordinary SSE text. This framing is required because a live response has no predetermined content length. Diagnostic counters record accepted and closed streams plus the most recent close stage so transport failures can be distinguished from a user closing the page.

The SSE ID is `<boot_id>:<latest_event_id>:<state_revision>`. `boot_id` and `state_revision` make duplicate, stale, and post-reboot data recognizable; `latest_event_id` provides the cursor for fetching any missed detailed events. The stream always begins with a full snapshot, so this implementation does not retain or replay a separate SSE history and does not depend on `Last-Event-ID` for recovery.

## Timer operations

Response schema for every endpoint in this section: [`schemas/local/timer-mutation-response.schema.json`](schemas/local/timer-mutation-response.schema.json).

Every successful modern timer mutation returns the complete JSON object documented under `GET /api/v1/status`, including `remaining_ms`, `state`, `sealed`, `boot_id`, `session_id`, and `state_revision`. It also adds mutation metadata:

```json
{
  "state": "paused",
  "remaining_ms": 583221,
  "session_id": "7ca9f21b-3",
  "mutation": {
    "action": "pause",
    "changed": true,
    "duplicate": false,
    "operation_id": "phone-a-pause-17"
  }
}
```

Clients should consume this response directly instead of issuing a confirmation read. A rejected mutation returns a structured error whose `state` field reports the current timer state.

All modern timer mutations accept an optional printable ASCII `operation_id` of at most 64 characters. The firmware remembers the eight most recent successful IDs for the current boot. Repeating an ID with the same action and inputs returns `200`, `changed=false`, and `duplicate=true` without applying it again. Reusing an ID for another action or different inputs returns `409 operation_id_conflict`. The cache is intentionally RAM-only; discard pending IDs whenever `boot_id` changes.

Every modern timer mutation also accepts optional `if_boot_id`. Send the `boot_id` from the status snapshot on which the command was based. A mismatch returns `409 stale_boot` without changing timer state, preventing a delayed command created before a reboot from acting afterward. Exact successful `operation_id` replays are checked first, so retrying a response that was lost immediately before a reboot-safe state change remains idempotent during the same boot.

REST and Bluetooth apply the same timer rules in the same order: request
parameters, `if_boot_id`, `if_session_id`, an accepted no-op when the requested
state is already true, locked controls, timer state, emergency input, then the
24-hour maximum. When two conditions apply, the earlier one decides the error.
REST keeps its endpoint-specific codes, for example `pause_not_allowed` for
both a locked and an open timer.

Active-session mutations also accept optional `if_session_id`. If it does not match the current `session_id`, the request returns `409 stale_session`. This prevents a delayed pause, adjustment, release, resume, or control-lock request intended for an older timer from changing a newer one. Check an exact operation-ID replay before the session precondition so retrying a response lost just before a session ended still produces an unambiguous duplicate result.

### Start a fixed duration

`POST /api/v1/timer/start`

Request schema: [`schemas/local/timer-start-request.schema.json`](schemas/local/timer-start-request.schema.json).

```sh
curl -X POST -d 'duration_ms=900000' \
  http://smarttimer.local/api/v1/timer/start
```

For a command that may be retried, send a unique ID:

```sh
curl -X POST -H 'Content-Type: application/json' \
  -d '{"duration_ms":900000,"operation_id":"client-73-start-1"}' \
  http://smarttimer.local/api/v1/timer/start
```

The timer must be open and `emergency_input_active` must be `false`. A second start does not replace or shorten an active timer; it returns `409 timer_active`. A held emergency input returns:

```json
{
  "error": "emergency input is active",
  "code": "emergency_input_active",
  "state": "open"
}
```

### Start a randomized duration

The random choice occurs on the controller using its hardware random source. Both boundaries are inclusive.

```sh
curl -X POST -H 'Content-Type: application/json' \
  -d '{"min_ms":600000,"max_ms":1800000}' \
  http://smarttimer.local/api/v1/timer/start
```

Both values are required, the minimum must be nonzero, the maximum must not be less than the minimum, and the maximum cannot exceed 24 hours.

### Pause and resume

```sh
curl -X POST http://smarttimer.local/api/v1/timer/pause
curl -X POST http://smarttimer.local/api/v1/timer/resume
```

Pause is valid only while running, and resume is valid only while paused. Repeating pause while paused or resume while running is an accepted no-op with `changed=false`. Both are rejected after software controls are locked unless the requested state is already true. A paused timer cannot be control-locked because it would have no automatic path to expiry. Resuming a paused timer while the emergency input is asserted returns `409 emergency_input_active`.

### Add or deduct time

Request schema: [`schemas/local/timer-adjust-request.schema.json`](schemas/local/timer-adjust-request.schema.json).

`delta_ms` is signed. These examples add five minutes and deduct one minute:

A zero adjustment on an eligible active timer is a no-op: `mutation.changed`
is `false`, and no adjustment event is added. It does not bypass locked controls
or the active-timer requirement. This matches the BLE zero-adjustment behavior.

```sh
curl -X POST -d 'delta_ms=300000' \
  http://smarttimer.local/api/v1/timer/adjust

curl -X POST -H 'Content-Type: application/json' \
  -d '{"delta_ms":-60000,"operation_id":"controller-42-adjust-7"}' \
  http://smarttimer.local/api/v1/timer/adjust
```

An adjustment that reaches zero opens the timer. An adjustment that would leave more than 24 hours remaining is rejected with `409 maximum_duration_reached`; the bundled UI displays a prominent maximum-duration notice. Adjustment is unavailable after controls are locked. A positive adjustment while the emergency input is asserted returns `409 emergency_input_active`; a negative adjustment is still accepted because it moves toward release.

The common `operation_id` rules above make an adjustment atomic from an integration's perspective: a lost-response retry cannot apply the delta twice. Omitting the ID preserves the original behavior, where every accepted request applies its delta once.

### Unlock now

```sh
curl -X POST http://smarttimer.local/api/v1/timer/stop
```

Preferred semantic alias for new integrations:

```sh
curl -X POST -H 'Content-Type: application/json' \
  -d '{"operation_id":"client-73-release-4","if_boot_id":"7ca9f21b","if_session_id":"7ca9f21b-3"}' \
  http://smarttimer.local/api/v1/timer/release
```

This is idempotent while already open. It is rejected after controls are locked. Control locking does not disable the emergency input, which this firmware still reads and acts on.

### Lock software controls

Preferred endpoint:

```sh
curl -X POST http://smarttimer.local/api/v1/timer/lock-controls
```

Compatibility endpoint:

```sh
curl -X POST -d 'sealed=1' \
  http://smarttimer.local/api/v1/timer/seal
```

Control locking is one-way for the active session. Pause, adjustment, and `stop` remain disabled until natural expiry or physical emergency release. There is deliberately no software-unlock endpoint.

### Session controls

Response schema (GET and POST): [`schemas/local/timer-controls.schema.json`](schemas/local/timer-controls.schema.json). `recovered` is `true` when a saved switch record was found but damaged (or V2 storage failed): both switches are then off until the owner sets them again, and diagnostics report `session_controls_recovered`. A record that was never saved, or one erased completely, reads as unsaved and leaves both on.
Request schema: [`schemas/local/timer-controls-request.schema.json`](schemas/local/timer-controls-request.schema.json).

Two saved owner settings decide whether a session may be paused and whether its
controls may be locked. Both are allowed until the owner saves otherwise, and a
ten-second reset allows both again.

```sh
curl http://smarttimer.local/api/v1/timer/controls
curl -X POST -H 'Content-Type: application/json' \
  -d '{"allow_pause":false,"allow_control_lock":false}' \
  http://smarttimer.local/api/v1/timer/controls
```

```json
{"allow_pause": false, "allow_control_lock": false, "recovered": false}
```

Send either field or both; an omitted field keeps its value. The settings can
change only while the timer is open (`409 timer_active` otherwise), so a session
in progress keeps whatever pause or lock it already has. A request without a
boolean field returns `400 invalid_field`; a failed save returns `503
storage_failed` and keeps the previous settings.

The rule is enforced by the timer for the local API, Bluetooth and share links
alike. A switched-off control returns `409 control_disabled` (Bluetooth result
`13`, relay result `control_disabled`). Resume, release and time changes are not
affected. Status reports the current settings as `pause_allowed` and
`control_lock_allowed`, and the bundled pages hide a control that is off. The
settings cannot be changed through a share link.

## Wi-Fi operations

Wi-Fi scan and mutation are allowed only while the timer is open. Scans run asynchronously so HTTP, timer expiration, the Reset button, and the emergency-input loop keep receiving service while the radio scans. Active-session scans remain rejected to avoid unnecessary radio disruption. On V2 a scan takes about three seconds; Bluetooth advertising pauses for its duration, because the two share one radio, and resumes when it ends.

### Scan networks

Response schema (GET and POST): [`schemas/local/wifi-scan-response.schema.json`](schemas/local/wifi-scan-response.schema.json).

Start a fresh scan:

```sh
curl -X POST http://smarttimer.local/api/v1/wifi/scan
```

```json
{"state":"scanning","retry_in_ms":500,"networks":[]}
```

Poll the result after the indicated delay:

```sh
curl http://smarttimer.local/api/v1/wifi/scan
```

```json
{
  "state": "ready",
  "retry_in_ms": 0,
  "networks": [
    {"ssid":"Workshop-IoT","rssi":-43,"secure":true},
    {"ssid":"Device-Lab","rssi":-61,"secure":false}
  ]
}
```

States are `idle`, `scanning`, `ready`, or `failed`. Clients may sort, deduplicate, or filter `networks`; an empty array in the `ready` state means no networks were found.

### Read Wi-Fi status

Response schema: [`schemas/local/wifi-status.schema.json`](schemas/local/wifi-status.schema.json).

```sh
curl http://smarttimer.local/api/v1/wifi/status
```

Example candidate failure:

```json
{
  "state": "failed",
  "network_mode": "wifi",
  "saved_ssid": "Workshop-IoT",
  "connected_ssid": "",
  "target_ssid": "",
  "last_attempted_ssid": "New-Network",
  "rssi_dbm": 0,
  "station_ip": "",
  "station_mac": "A4:CF:12:34:56:78",
  "last_error": "wrong password",
  "retry_in_ms": 27860,
  "station_retry_paused": true,
  "ap_running": true,
  "ap_role": "recovery",
  "ap_ssid": "SMARTTIMER-12AB34",
  "ap_clients": 1,
  "ap_security": "wpa2",
  "ap_password_set": true,
  "ap_uses_device_name": true
}
```

On V2, Wi-Fi status also includes the boolean `wifi_suspend_supported: true`.
Older firmware and V1 may omit it; clients must require `true` before offering
temporary Bluetooth-only mode. While suspended, Wi-Fi status reports
`state: "disabled"`, `network_mode: "bluetooth"`, `ap_running: false`,
`ap_role: "off"` and `retry_in_ms: 0`. The full status uses `wifi_state`
instead of `state` for the same network condition; its `state` remains the timer
state (`open`, `running` or `paused`).

Suspension disables both station and AP, making HTTP/SSE unavailable. These
shared JSON shapes remain accessible through authenticated BLE management.
The suspend/resume operations are BLE-only and have no HTTP route. Saved
network settings stay unchanged in flash. On current V2 firmware,
`wifi_disconnect_restore_supported: true` indicates automatic restoration on
Bluetooth disconnect/reset, including a rapid reconnect. Explicit BLE resume or
a restart also restores networking. Saved router mode keeps its recovery AP
fallback. Older firmware without this flag requires resume or restart. See [temporary Bluetooth-only mode](BLUETOOTH_MANAGEMENT.md#temporary-bluetooth-only-mode).

### Configure the setup access point

Schemas: [`schemas/local/wifi-ap.schema.json`](schemas/local/wifi-ap.schema.json) (GET response), [`schemas/local/wifi-ap-update-request.schema.json`](schemas/local/wifi-ap-update-request.schema.json) (PUT request), [`schemas/local/wifi-ap-update-response.schema.json`](schemas/local/wifi-ap-update-response.schema.json) (PUT response).

The local AP configuration is independent of the router credentials above. It is the primary connection in AP Mode and becomes the Recovery AP if saved router Wi-Fi is unavailable. While a client is using a Recovery AP, background saved-network retries pause to protect the controller's single-radio AP session. New or upgraded installations default to an open generated SSID so an update cannot create a surprise lockout.

Read its non-secret settings:

```sh
curl http://smarttimer.local/api/v1/wifi/ap
```

```json
{
  "ssid": "SMARTTIMER-12AB34",
  "security": "open",
  "password_set": false,
  "uses_device_name": true,
  "clients": 1
}
```

Set a custom WPA2 AP while the timer is open:

```sh
curl -X PUT -H 'Content-Type: application/json' \
  -d '{"ssid":"PRIVATE-TIMER","security":"wpa2","password":"example-password","use_device_name":false,"standalone":true}' \
  http://smarttimer.local/api/v1/wifi/ap
```

```json
{
  "updated": true,
  "restarting": true,
  "ap_ssid": "PRIVATE-TIMER",
  "ap_security": "wpa2",
  "standalone": true
}
```

The SSID must contain 1–32 printable ASCII characters. A WPA2 password must contain 8–63 printable ASCII characters. To keep an existing WPA2 password while changing only the name, omit `password` and send `keep_password=true`. To return to an open AP, send `security=open`. Set `use_device_name=true` to ignore `ssid` and restore the generated `<hostname>-<chip-id>` name. Set `standalone=true` to atomically clear saved station credentials and restart in AP Mode after returning the reconnection response. Omitting it updates the Recovery AP without changing station configuration.

The password is write-only. It is never returned by status, AP configuration, diagnostics, or events. The response is sent before the device restarts; a client connected directly to the AP must then join the reported SSID and reopen `192.168.4.1`.

An AP settings storage failure returns HTTP `503` with `code: "storage_failed"`.
The previous in-memory settings are restored and no restart is scheduled.
For `standalone=true`, the AP configuration and cleared station credentials
are committed together; neither change is reported successful on a failed
commit. A flash hardware failure is not a guarantee of recoverable storage.

If the SSID or password is mistyped or forgotten, hold the Reset button for ten seconds while the timer is open. This physical settings reset restores router/AP settings, the friendly device name, and supported Bluetooth settings, then restarts a generated open setup AP. It does not change firmware.

### Configure Wi-Fi safely

Schemas: [`schemas/local/wifi-configure-request.schema.json`](schemas/local/wifi-configure-request.schema.json) (request), [`schemas/local/wifi-configure-response.schema.json`](schemas/local/wifi-configure-response.schema.json) (immediate response).

```sh
curl -X POST -H 'Content-Type: application/json' \
  -d '{"ssid":"Workshop-IoT","password":"router-password"}' \
  http://192.168.4.1/api/v1/wifi/configure
```

Immediate response:

```json
{"connecting":true,"wifi_state":"connecting"}
```

The network change is applied after that response. The setup AP remains available while the controller tests the candidate. Poll `/api/v1/wifi/status` or `/api/v1/status`:

1. `connecting`: candidate remains only in RAM.
2. `connected`: candidate is committed to persistent configuration and mDNS is rebound. The AP closes 1.5 seconds after the first successful status response, or after the two-second acknowledgement fallback if no client polls it.
3. `failed`: candidate password is erased from RAM, the previous saved record remains untouched, and the setup AP remains available. If an older network was saved, the station interface also attempts to recover it.

An empty password is valid for an open network.

The immediate `connecting` response acknowledges a test, not a durable save.
If association succeeds but saving fails, status reports `failed` with a
storage error in Wi-Fi status `last_error` (or full status `last_wifi_error`);
the previous saved credentials are retained
and recovery access stays available.

### Forget station Wi-Fi

Response schema: [`schemas/local/wifi-forget-response.schema.json`](schemas/local/wifi-forget-response.schema.json).

```sh
curl -X POST http://smarttimer.local/api/v1/wifi/forget
```

```json
{"standalone":true}
```

The request clears only saved station credentials and returns to the configured setup AP, which may be open or WPA2-protected. It does not erase the AP configuration, device name, timer firmware, vendor filesystem, or OTA rollback allowlist. Use the physical ten-second Reset-button recovery when AP credentials also need to be cleared.

## Device identity

### Read identity

Response schema: [`schemas/local/device-name.schema.json`](schemas/local/device-name.schema.json).

```sh
curl http://smarttimer.local/api/v1/device/name
```

```json
{
  "name": "Smart Timer",
  "hostname": "smarttimer",
  "ap_ssid": "SMARTTIMER-12AB34",
  "ap_uses_device_name": true
}
```

### Change identity

Schemas: [`schemas/local/device-name-update-request.schema.json`](schemas/local/device-name-update-request.schema.json) (request), [`schemas/local/device-name-update-response.schema.json`](schemas/local/device-name-update-response.schema.json) (response).

The timer must be open. The value is trimmed and must contain 1–32 printable ASCII characters. The current identity record and hostname generator do not accept non-ASCII display names; this does not prevent the browser interface itself from being shown in Simplified Chinese.

```sh
curl -X PUT -H 'Content-Type: application/json' \
  -d '{"name":"Workshop Timer"}' \
  http://smarttimer.local/api/v1/device/name
```

```json
{
  "updated": true,
  "restarting": true,
  "name": "Workshop Timer",
  "hostname": "workshop-timer"
}
```

The device restarts after responding. Reconnect using `http://workshop-timer.local`. In AP Mode, the AP SSID also changes when `ap_uses_device_name` is true; a custom AP override remains unchanged. The friendly name is stored in a checksummed record separate from Wi-Fi credentials.

If the name cannot be saved, the response is HTTP `503` with
`code: "storage_failed"`, the previous name remains in memory, and no restart
is scheduled. Validation failures remain HTTP `400`.

## System and support operations

### Identify the physical device

Response schema: [`schemas/local/system-identify-response.schema.json`](schemas/local/system-identify-response.schema.json).

```sh
curl -X POST http://smarttimer.local/api/v1/system/identify
```

```json
{"identifying":true,"duration_ms":5000}
```

The blue status LED blinks rapidly for five seconds. Identify requires an open timer. If a timer starts during the window, blue immediately returns to network status; the separate amber hardware indicator represents the energized lock driver.

### Restart

Response schema: [`schemas/local/system-restart-response.schema.json`](schemas/local/system-restart-response.schema.json).

```sh
curl -X POST http://smarttimer.local/api/v1/system/restart
```

```json
{"restarting":true}
```

Restart requires an open timer and is performed after the HTTP response. As with power loss, boot begins fail-open. If the emergency input is held through boot, it remains open and start requests remain blocked.

### Diagnostics

Response schema: [`schemas/local/system-diagnostics.schema.json`](schemas/local/system-diagnostics.schema.json).

```sh
curl http://smarttimer.local/api/v1/system/diagnostics
```

```json
{
  "uptime_ms": 245110,
  "reset_reason": "Software/System restart",
  "hardware_model": "MagBound Smart Timer V1",
  "chip_family": "ESP8266",
  "build_id": "bb0629a425882236a0337089",
  "free_heap_bytes": 43216,
  "minimum_observed_free_heap_bytes": 39784,
  "max_free_block_bytes": 37184,
  "heap_fragmentation_percent": 8,
  "loop_stack_size_bytes": 4096,
  "loop_stack_min_free_bytes": 1712,
  "sketch_size_bytes": 381920,
  "free_sketch_space_bytes": 638976,
  "flash_size_bytes": 4194304,
  "flash_layout": "4M/2M OTA",
  "sse_clients": 1,
  "sse_client_limit": 3,
  "sse_accepted_count": 3,
  "sse_closed_count": 2,
  "sse_last_close_reason": "peer_closed",
  "status_json_failure_count": 0,
  "sdk_version": "2.2.2-dev(38a443e)",
  "wifi_state": "connected",
  "wifi_channel": 6,
  "wifi_power_save_mode": "none",
  "station_mac": "A4:CF:12:34:56:78",
  "rssi_dbm": -48,
  "ap_clients": 0,
  "station_retry_paused": false,
  "station_retry_in_ms": 0,
  "wifi_connect_attempt_count": 2,
  "wifi_disconnect_count": 1,
  "wifi_unexpected_disconnect_count": 1,
  "wifi_dhcp_timeout_count": 0,
  "wifi_sa_query_timeout_count": 0,
  "wifi_last_disconnect_reason": 200,
  "wifi_last_disconnect_reason_name": "beacon_timeout",
  "wifi_last_disconnect_expected": false,
  "wifi_last_disconnect_at_ms": 198302,
  "wifi_last_disconnect_rssi_dbm": null,
  "wifi_last_connected_at_ms": 201144,
  "wifi_connected_duration_ms": 43966,
  "lock_output_energized": false,
  "emergency_input_active": false,
  "event_count": 7
}
```

`lock_output_energized` is the firmware's commanded logical state; it is not electrical feedback from the external driver or lock. `emergency_input_active` is the live input level used by the interlock, not proof that the external accessory switch itself is mechanically healthy. `build_id` identifies the source/configuration build while release `BUILD.json` supplies the exact binary SHA-256. `free_heap_bytes` is total currently free heap, `minimum_observed_free_heap_bytes` is the current boot's observed low-water mark, and `max_free_block_bytes` plus `heap_fragmentation_percent` show whether that memory remains useful for larger allocations. `loop_stack_size_bytes` is the Arduino loop stack (4 KiB `cont` on V1, 8 KiB task on V2) and `loop_stack_min_free_bytes` is its lowest remaining headroom observed since boot; the web server, BLE processing and timer logic all share that stack, so values near zero indicate overflow risk. `sketch_size_bytes`, `free_sketch_space_bytes`, `flash_size_bytes`, and `flash_layout` expose installed image size, current OTA image capacity, physical flash, and the fixed linker layout. `sse_clients` counts occupied live-stream slots. `sse_accepted_count`, `sse_closed_count`, and `sse_last_close_reason` are volatile current-boot transport diagnostics; normal reasons include `peer_closed` and `ota_quiet`, while header/write timeout values point to transport failures. `station_retry_paused` and `station_retry_in_ms` expose Recovery AP retry protection.

All `wifi_*_count` and Wi-Fi timestamps are volatile and reset at boot. `wifi_disconnect_count` includes deliberate firmware disconnects during configuration or AP-mode transitions; `wifi_unexpected_disconnect_count` excludes those expected transitions. The numeric reason comes from the selected controller SDK, while the stable lowercase reason name makes common causes easier to consume. `wifi_connected_duration_ms` is zero while disconnected. These detailed fields are intentionally diagnostic/API-only and are not added to the normal control dashboard.

V1 queues ordinary HTTP replies cooperatively, at most 512 bytes per loop tick.
It retains each reply until all bytes are accepted by TCP, and does not parse
the next request on that connection meanwhile. Static assets stay in flash.
If a reply cannot finish within five seconds, or its peer disconnects, the
connection is aborted; retry reads on a new connection. For a mutation with a
lost reply, read status first and use the existing operation-ID retry rules.
`http_response_completed_count` counts fully queued replies, not confirmed
peer receipt. `http_response_failed_count` and `http_response_last_failure`
expose current-boot failures (`send_timeout`, `peer_closed`, `write_failed`,
`allocation_failed`, or `reply_overlap`). These fields are V1-only and reset
at boot. Scheduled network changes and restarts wait for the queued response
to complete or fail before proceeding. Timer and physical-input processing
continue during that wait.

`status_json_failure_count` counts checked status-serialization failures across
REST, timer mutation replies and SSE during the current boot; it does not count
transport truncation or every other JSON endpoint. `wifi_power_save_mode` reports
the actual SDK mode (`none`, `min_modem`, `max_modem`, `light`, or `unknown`). Both
models request no modem sleep (`none`) to favor response latency over power
consumption. Disabling modem sleep favors responsiveness at the cost of power
consumption; battery runtime depends on the installation and workload.

On V2, `wifi_sa_query_timeout_count` counts unexpected reason-209 disconnects
(`sa_query_timeout`); `wifi_last_disconnect_rssi_dbm` captures the SDK's signal
reading at the last disconnect. V1 reports zero/null for these two fields.
Reason 209 means the AP did not reply to an SA query, not necessarily that the
Wi-Fi password was wrong. PMF/WPA3 negotiation remains unchanged. See the
[ESP-IDF Wi-Fi guide](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32c3/api-guides/wifi.html)
for the SDK reason codes and modem-sleep tradeoffs.

V2 adds `bluetooth_initialized`, `bluetooth_enabled`,
`bluetooth_configured_enabled`, `bluetooth_paired`, `bluetooth_synchronized`,
`bluetooth_advertising`, `bluetooth_connections`,
`bluetooth_connection_limit`, `bluetooth_last_error`,
`bluetooth_last_error_name`, nullable
`bluetooth_connection_rssi_dbm`, `bluetooth_connection_mtu`,
`bluetooth_connection_encrypted`, `bluetooth_connection_bonded`,
`bluetooth_control_enabled`, `bluetooth_control_authenticated`,
`bluetooth_control_authentication_failure_count`,
`bluetooth_authentication_backoff_remaining_ms`,
`bluetooth_control_command_count`, `bluetooth_control_last_result`,
`bluetooth_connection_mtu_sufficient`, `bluetooth_bond_cleanup_pending`, and
`bluetooth_bond_cleanup_last_error`. Connection, counter, error, backoff, and
authentication fields are current-boot diagnostics. The configured radio
preference and paired state persist, but the key itself never appears in
diagnostics. Peer discovery is not exposed by the application.

`reset_reason` comes from the selected controller's current-boot ROM/SDK reset reason. It is not read from settings and is not a persistent reboot-history log: each boot replaces it while the in-memory event timeline starts over.

`last_reset_trace`, when present, says where the previous run was when the chip reset: `phase` (such as `web_request`, `network_scan`, `network_mdns` or `loop_end`), the last request path in `uri`, and that run's `uptime_ms`. It is kept in memory that survives a watchdog reset (RTC memory), so after an unexpected `reset_reason` it locates the hang; after a normal restart it simply shows the last step before it.

`loop_stall` records the longest pause in the timer's main loop this boot: `longest_ms`, the loop `phase` it happened in (the same names as `last_reset_trace`; `loop_end` is time spent outside the loop), the request path in `uri` when that phase was a web request, the `uptime_ms` when the pause ended and how many pauses lasted a second or more (`over_1s_count`). Timer expiry is handled in the main loop. On V1 a long pause can delay a timed release by that long. On V2 a deadline backstop, a hardware timer set 200 ms after each timed release (expiry or a share link's hold limit), turns the lock output off even while the loop is paused; the loop records the release when it resumes. The emergency input cuts the lock output through an interrupt on both models and does not wait for the loop. A firmware upload shows as a long `web_request` pause; it needs an open timer. Phases are marked at a few points in the loop, so time in a step without its own mark is counted to the phase before it. On V2, Bluetooth start-up (`bluetooth_start`) and the post-update boot check (`boot_validation`) have their own phases.

On V2, `deadline_backstop` counts how often this boot the backstop turned the output off because the loop had not yet released (`fired_count`) and how often it could not be armed (`arm_failed_count`; the loop then releases as on V1). In normal operation both stay 0.

`heap_low_water` records what held memory at the lowest free heap seen this boot: `free_bytes`, `uptime_ms`, the live streams open (`sse_clients`) and their unsent bytes (`sse_pending_bytes`), the last request path (`last_uri`) and, on V1, open TCP connections (`tcp_active`), connections still closing (`tcp_time_wait`) and bytes queued in the TCP stack (`tcp_queued_bytes`). It is for diagnosing memory pressure; the V2 reports the since-boot minimum, which includes brief Bluetooth start-up peaks.

### Recent events

`after` is an unsigned decimal 32-bit cursor (0–4,294,967,295). Negative,
non-integer and overflowing values return `400 invalid_event_cursor`.

Response schema: [`schemas/local/events-list.schema.json`](schemas/local/events-list.schema.json).

```sh
curl http://smarttimer.local/api/v1/events
```

```json
{
  "boot_id": "7ca9f21b",
  "oldest_event_id": 1,
  "latest_event_id": 18,
  "next_after": 18,
  "cursor_reset": false,
  "truncated": false,
  "events": [
    {"id":1,"type":"boot","at_ms":0,"value_ms":0,"state":"open","sealed":false,"emergency_input_active":false,"session_sequence":0},
    {"id":2,"type":"wifi_connected","at_ms":1821,"value_ms":0,"state":"open","sealed":false,"emergency_input_active":false,"session_sequence":0},
    {"id":3,"type":"started","at_ms":9214,"value_ms":900000,"state":"running","sealed":false,"emergency_input_active":false,"session_sequence":1},
    {"id":4,"type":"adjusted","at_ms":18220,"value_ms":60000,"state":"running","sealed":false,"emergency_input_active":false,"session_sequence":1}
  ]
}
```

The ring holds the latest 16 events and resets at boot. Each event receives a monotonic current-boot `id`. `state`, `sealed`, `emergency_input_active`, and `session_sequence` are captured when that event occurs, so historical replay never borrows the current state. `value_ms` contains the relevant duration, remaining value, adjustment delta, OTA image size, or zero depending on event type. For `wifi_disconnected`, it retains the raw SDK reason for compatibility and the object also contains explicit `wifi_disconnect_reason`, `wifi_disconnect_reason_name`, and `unexpected=true` fields. Only unexpected station disconnects enter the event ring; deliberate configuration transitions remain visible in diagnostics counters.

Incremental consumers should retain both `boot_id` and `next_after`, then request:

```sh
curl 'http://smarttimer.local/api/v1/events?after=18&boot_id=7ca9f21b'
```

Only events with a greater ID are returned. `cursor_reset=true` means the supplied boot is stale or the cursor is ahead of the device, so the response contains the retained current-boot history and the client must replace its cursor. `truncated=true` means more than 16 events occurred and the requested cursor fell behind the oldest retained item. The new `next_after` is safe to save even when `events` is empty.

Possible event names include:

```text
boot, started, remaining_set, paused, resumed, adjusted,
controls_locked, expired, opened_by_user, emergency_release,
emergency_input_activated, emergency_input_cleared,
adjusted_to_zero, wifi_connecting, wifi_connected, wifi_failed,
wifi_standalone, wifi_disconnected, wifi_dhcp_timeout,
ota_started, ota_succeeded, ota_failed,
restart_requested, device_renamed, identify_started,
access_point_configured, bluetooth_pairing_started,
bluetooth_pairing_completed, bluetooth_pairing_revoked, share_link_expired
```

### Capabilities

Response schema: [`schemas/local/capabilities.schema.json`](schemas/local/capabilities.schema.json).

```sh
curl http://smarttimer.local/api/v1/capabilities
```

Clients should use this endpoint when they need feature discovery rather than inferring features from firmware version strings.

```json
{
  "api_version": "v1",
  "max_duration_ms": 86400000,
  "hardware_model": "MagBound Smart Timer V1",
  "chip_family": "ESP8266",
  "build_id": "bb0629a425882236a0337089",
  "request_content_types": [
    "application/x-www-form-urlencoded",
    "application/json"
  ],
  "features": {
    "pause": true,
    "adjust": true,
    "adjust_operation_id": true,
    "mutation_operation_id": true,
    "session_id": true,
    "session_precondition": true,
    "boot_precondition": true,
    "wifi_disconnect_diagnostics": true,
    "station_mac": true,
    "release_alias": true,
    "event_cursor": true,
    "mdns_discovery": true,
    "random_duration": true,
    "lock_controls": true,
    "wifi_scan": true,
    "wifi_async_scan": true,
    "wifi_staged_credentials": true,
    "device_name": true,
    "identify_led": true,
    "diagnostics": true,
    "event_history": true,
    "emergency_input_status": true,
    "ap_configuration": true,
    "ap_wpa2": true,
    "sse": true,
    "sse_opt_in": false,
    "sse_ui_auto_connect": true,
    "ota": true,
    "legacy_api": true,
    "bluetooth_low_energy": false,
    "bluetooth_read_only": false,
    "bluetooth_control": false,
    "bluetooth_authenticated_control": false,
    "bluetooth_notifications": false,
    "bluetooth_peer_discovery": false,
    "bluetooth_peer_control": false
  },
  "limits": {
    "ap_clients": 4,
    "sse_clients": 3
  },
  "sse": {
    "path": "/api/v1/events/stream",
    "default_enabled": true,
    "persistent": false,
    "rest_fallback": true
  },
  "mutations": {
    "operation_id_max_length": 64,
    "deduplication_entries": 8,
    "scope": "current_boot",
    "boot_precondition_field": "if_boot_id",
    "session_precondition_field": "if_session_id"
  },
  "events": {
    "history_entries": 16,
    "cursor_parameter": "after",
    "boot_parameter": "boot_id"
  },
  "discovery": {
    "mdns_service": "_smarttimer._tcp.local",
    "port": 80
  },
  "ota": {
    "physical_arm_required": true,
    "firmware_only": true,
    "unverified_override": true,
    "unverified_override_volatile": true,
    "unverified_override_uploads": 1
  }
}
```

The example above is a V1 response. On V2,
`bluetooth_low_energy`, `bluetooth_control`,
`bluetooth_authenticated_control`, `bluetooth_notifications`,
`bluetooth_bonding`, `bluetooth_command_integrity`,
`bluetooth_physical_pairing`, `bluetooth_event_notifications`,
`bluetooth_event_replay`, `bluetooth_pairing_commit`, and
`bluetooth_identify`, and `bluetooth_system_information` are `true`; `bluetooth_read_only`,
`bluetooth_peer_discovery`, and `bluetooth_peer_control` remain `false`. V2
also adds:

```json
"bluetooth": {
  "protocol": "ost-ble/1",
  "mode": "bonded_authenticated_control",
  "inbound_connections": 1,
  "controller_connections": 1,
  "status_notifications_ms": 15000,
  "default_enabled": false,
  "persistent_enable": true,
  "control_volatile": false,
  "control_authentication": "bonded-hmac-sha256-112",
  "command_integrity": "hmac-sha256-64-counter",
  "capabilities_frame_bytes": 24,
  "command_frame_bytes": 32,
  "result_frame_bytes": 20,
  "status_frame_bytes": 20,
  "event_frame_bytes": 20,
  "required_control_mtu": 35,
  "event_history_entries": 16,
  "system_information_frame_bytes": 44,
  "system_information_uuid": "f00df299-464f-4921-9285-7749593b1144",
  "pairing_commit": "hmac-sha256-112",
  "path": "/api/v1/bluetooth",
  "enable_path": "/api/v1/bluetooth/enable",
  "disable_path": "/api/v1/bluetooth/disable",
  "pairing_start_path": "/api/v1/bluetooth/pairing/start",
  "pairing_cancel_path": "/api/v1/bluetooth/pairing/cancel",
  "pairing_revoke_path": "/api/v1/bluetooth/pairing/revoke"
}
```

### `GET /api/v1/bluetooth` (V2 only)

Response schema: [`schemas/local/bluetooth-status.schema.json`](schemas/local/bluetooth-status.schema.json).

This endpoint reports the V2 BLE radio preference, paired-controller state,
pairing ceremony, link security, application authentication, and commands
without exposing the stored key.

```sh
curl http://smarttimer.local/api/v1/bluetooth
```

```json
{
  "supported": true,
  "protocol": "ost-ble/1",
  "mode": "bonded_authenticated_control",
  "default_enabled": false,
  "configured_enabled": true,
  "persistent_enable": true,
  "volatile_enable": false,
  "enable_path": "/api/v1/bluetooth/enable",
  "disable_path": "/api/v1/bluetooth/disable",
  "advertised_name": "OST-f8e894",
  "initialized": true,
  "enabled": true,
  "synchronized": true,
  "advertising": true,
  "connected": false,
  "subscribed": false,
  "connections": 0,
  "connection_limit": 1,
  "accepted_count": 0,
  "closed_count": 0,
  "notification_count": 0,
  "last_error": 0,
  "last_error_name": "none",
  "last_disconnect_reason": 0,
  "last_disconnect_reason_name": "none",
  "connection_rssi_dbm": null,
  "connection_mtu": 23,
  "connection_encrypted": false,
  "connection_bonded": false,
  "paired": true,
  "pairing_state": "complete",
  "pairing_active": false,
  "pairing_remaining_ms": 0,
  "pairing_failure": "none",
  "physical_pairing_supported": true,
  "control_supported": true,
  "control_default_enabled": true,
  "control_volatile": false,
  "control_enabled": true,
  "control_authenticated": false,
  "control_authentication": "bonded-hmac-sha256-112",
  "command_integrity": "hmac-sha256-64-counter",
  "control_authentication_failure_count": 0,
  "control_authentication_failure_limit": 5,
  "control_authentication_backoff_remaining_ms": 0,
  "control_command_count": 0,
  "control_last_result": "unauthorized",
  "pairing_start_path": "/api/v1/bluetooth/pairing/start",
  "pairing_cancel_path": "/api/v1/bluetooth/pairing/cancel",
  "pairing_revoke_path": "/api/v1/bluetooth/pairing/revoke",
  "control_boot_id": 2091516443,
  "required_control_mtu": 35,
  "connection_mtu_sufficient": false,
  "bond_cleanup_pending": false,
  "bond_cleanup_last_error": 0,
  "peer_discovery_supported": false,
  "peer_control_supported": false
}
```

When Bluetooth is saved as enabled, firmware waits 15 seconds for the web/Wi-Fi
recovery path to become healthy and then starts BLE automatically. During that
delay, `configured_enabled` can be true while `enabled` remains false. With
the timer open, the following endpoints change the persistent preference:

```sh
curl http://smarttimer.local/api/v1/bluetooth/enable
```

The successful enable response has `initialized:true`, `enabled:true` and,
after host synchronization, `advertising:true`. The stack remains allocated
if switched off during the same boot. Disable is permitted only while open:

```sh
curl http://smarttimer.local/api/v1/bluetooth/disable
```

Both return `409 timer_active` if the timer is not open. Turning the radio off
does not erase pairing.

### Bluetooth pairing management (V2 only)

Three apps may be approved, with **one simultaneous BLE connection**. The
normal pairing-start request adds to an empty slot; full tables return HTTP
409 `app_table_full`, never evicting an app. To replace one intentionally,
include its current `replace_app_id`. Every addition/replacement still requires
physical Reset approval and durable Pairing Commit. Existing apps survive
cancellation or failed replacement.

```sh
# Add an app with a friendly name (optional, maximum 24 UTF-8 bytes).
curl -X POST http://smarttimer.local/api/v1/bluetooth/pairing/start \
  -H 'Content-Type: application/json' -d '{"label":"My phone"}'

# List approvals: app_id, label, connected, last_seen_at_ms.
curl http://smarttimer.local/api/v1/bluetooth/apps

# Explicit replacement; preserves this app until the new commit succeeds.
curl -X POST http://smarttimer.local/api/v1/bluetooth/pairing/start \
  -H 'Content-Type: application/json' -d '{"replace_app_id":1,"label":"New phone"}'

curl -X PUT http://smarttimer.local/api/v1/bluetooth/apps/label \
  -H 'Content-Type: application/json' -d '{"app_id":1,"label":"Tablet"}'
curl -X POST http://smarttimer.local/api/v1/bluetooth/apps/revoke \
  -H 'Content-Type: application/json' -d '{"app_id":1}'
```

All successful management responses use the Bluetooth-status schema. Added
fields are `approved_app_limit` (3), `approved_app_count`, `active_app_id`
(0 when unauthenticated), and `approved_apps`. Keys and bond addresses are
never returned. `last_seen_at_ms` is uptime in the current boot, or null; it is
not a calendar timestamp. No last-seen or command-counter writes occur in flash.
Labels accept valid UTF-8 (up to 24 bytes), excluding control characters.
Missing IDs return 404 `app_not_found`; malformed fields return 400.
Rename/removal require an open timer and no active pairing; cancel pairing first.
The local network remains the web management trust boundary, not a separate
authenticated owner account. Every authenticated approved BLE app also has
approval-management authority through the V2 management extension: it may
rename or revoke other apps, revoke all approvals, or open pairing, subject to
the same state checks. There is no separate owner-only role. Adding/replacing
an approval still requires physical Reset-button confirmation; revocation
does not. Pair only apps you trust with these administrative privileges.

V2 `/api/v1/events` entries include `bluetooth_app_id` (0 for non-BLE events).
SSE still sends status/revision updates: fetch the event timeline for attribution.
The 20-byte BLE event frame remains unchanged and does not include app identity.

Response schema for all three pairing endpoints: [`schemas/local/bluetooth-status.schema.json`](schemas/local/bluetooth-status.schema.json).

Open a two-minute physical-confirmation pairing window while the timer is
open. Pairing also enables the saved radio preference:

```sh
curl -X POST http://smarttimer.local/api/v1/bluetooth/pairing/start
```

The response uses the normal Bluetooth-status schema. It does not expose a
credential:

```json
{"paired":false,"pairing_active":true,"pairing_state":"waiting_for_connection","control_enabled":false,"control_authenticated":false}
```

The trusted app connects and completes encrypted link pairing. A short Reset
press approves that connected app, after which the app has 60 seconds to read
and durably save a pending encrypted Pairing Secret. It then proves possession
with the `0x02` Pairing Commit frame. Only a valid proof promotes and persists
the new 128-bit application key. A pre-existing connection cannot be adopted
by a newly opened pairing window. If pairing times out, disconnects, or fails
before commit, an existing approved app remains valid. Neither REST status nor
diagnostics disclose either key.

Cancel an in-progress ceremony without changing the approved key:

```sh
curl -X POST http://smarttimer.local/api/v1/bluetooth/pairing/cancel
```

Revoke-all requires an open timer and erases all application keys. Bond erasure
is retried if the controller reports a transient failure, and
`bond_cleanup_pending` exposes that safe partial state:

```sh
curl -X POST http://smarttimer.local/api/v1/bluetooth/pairing/revoke
```

The physical no-Wi-Fi pairing ceremony, encrypted GATT access rules, HMAC
calculations, 32-byte integrity-protected command frame, status and event
frames, and executable Bleak client are specified in
[Bluetooth Low Energy design](BLUETOOTH.md).

Five invalid authentication or command-integrity attempts disconnect only the
offending connection and impose a 30-second advertising backoff. The saved app
can reconnect after the delay without Wi-Fi recovery or re-pairing. Unknown
devices may create a bond only during the physically opened pairing window;
ordinary unknown connections are closed after a bounded interval.

Control commands require an ATT MTU of at least 35 bytes. Clients should read
`required_control_mtu` and `connection_mtu_sufficient` before sending a
32-byte command frame. Losing the BLE connection never releases, pauses, or
otherwise changes an active timer; its local state machine continues.

The raw error and disconnect values are NimBLE diagnostic codes and are not a
stable automation contract; `last_disconnect_reason_name` is the readable
contract for recognized causes. RSSI is `null` without a connected phone. V1
does not register any Bluetooth path.

Peer discovery and timer-to-timer control are not implemented. The application
uses the peripheral/GATT-server role; the pinned V2 framework also retains
central/observer/client libraries without exposing peer control.

## Experimental remote relay (V2 only)

> [!WARNING]
> Experimental. Remote control over the Internet adds a network path the
> timer does not otherwise have. Read [SAFETY.md](../SAFETY.md); remote
> access never replaces the local and physical release methods.

The relay lets a partner control the timer from a share link through a
self-hosted or explicitly trusted operator (see
[relay/README.md](../relay/README.md) and the
[relay protocol](RELAY_PROTOCOL.md)). It is **off by default**: until you
enroll or enable it, the timer allocates nothing for it and opens no
connection. V1 images do not contain it; `features.relay` in
`/api/v1/capabilities` reports support.

Relay commands are applied on the timer's own loop with the same rules as
this API and Bluetooth: boot/session preconditions, controls lock, emergency
input and the 24-hour maximum. OTA, restart and network-setting transitions
pause the relay and refuse its commands. **Local Bluetooth has priority:**
while a Bluetooth app is connected or the pairing window is open, the relay
pauses (`state` `paused`, `paused_for_bluetooth` true). Its connection closes
and partners see the timer offline; links stay valid and the relay resumes a
few seconds after Bluetooth disconnects. Both together do not fit in memory
with headroom. New links and enrollment return `409 bluetooth_active` while
paused; a revoke is accepted and completes when the relay resumes.

### Enroll

The relay provides its HTTPS origin, an enrollment code (single-use for 15
minutes, or a reusable owner code) and the relay certificate's SPKI pin
(`relay/tools/extract_spki_pin.sh`), plus an independently hosted HTTPS control
page origin. A single `ost-relay-setup:` code bundles these values in the UI.
Users supply their own hosting or choose a trusted operator; the project does
not operate a service. The timer does not use a public CA bundle, so
certificate expiry and hostname are not checked. Enrollment is refused unless
the server's leaf key matches the pin. Later connections do not use the pin:
each is authenticated with the enrollment secret and bound to the key the
server presented, so a renewed certificate needs no new setup
(`relay_auth_failed` when the relay cannot prove itself).

```sh
curl -X POST -H 'Content-Type: application/json' \
  -d '{"relay_url":"https://api.example.com","partner_origin":"https://control.example.com","enrollment_code":"...","spki_pin":"43-character-base64url","if_boot_id":"current-boot-id"}' \
  http://smarttimer.local/api/v1/relay/enroll
```

Returns `202` with `setup_state: "pending"`, `setup_pending`,
`setup_request_id`, `setup_remaining_ms` and the exact `setup_relay_origin` and
`setup_partner_origin` to review. Briefly press Reset while unlocked within 60
seconds. The gesture approves only the request captured on button-down; it
does not arm OTA. Exact resubmissions preserve the deadline; changed requests
are rejected while one is pending. No HTTP endpoint can approve setup.

Poll `GET /api/v1/relay` for that request and boot. `setup_state` progresses to
`connecting`, then `complete` or `failed`; it can also become `cancelled` or
`expired`. An old enrollment already connected does not prove the new request
succeeded. `last_error` explains enrollment failures such as
`enroll_pin_mismatch`, `enroll_rejected` or `network_unavailable`.
Cancel with `POST /api/v1/relay/enroll/cancel` and the current `if_boot_id` and
`setup_request_id` before physical approval. Setup requires an open timer,
station Wi-Fi and no Bluetooth pairing/connection or maintenance transition.
Status includes the saved `partner_origin` but never the setup code or pin.
An enrollment without a saved, approved control-page origin remains offline
until a new setup is physically approved.

### Status

```json
{
  "experimental": true, "enabled": true, "enrolled": true, "running": true,
  "enrolling": false, "stopping": false, "maintenance": false,
  "paused_for_bluetooth": false,
  "boot_id": "bcb49155", "next_job_id": 3,
  "job": {"id": 2, "state": "done", "result": "ok", "fp_hash": "..."},
  "state": "connected", "sharing_available": true,
  "relay_host": "relay.example.com", "timer_id": "abc123", "last_error": "",
  "reconnects": 1, "live_links": 1, "link_limit": 1, "task_stack_min_free_bytes": 5120,
  "free_heap_bytes": 81234, "largest_free_block_bytes": 57344,
  "tls_arena": {"size_bytes": 49152, "free_bytes": 47104,
                "minimum_free_bytes": 14336, "fallback_count": 0}
}
```

`state` is `disabled`, `not_enrolled`, `waiting`, `connecting`,
`authenticating`, `connected`, `failed`, `paused` (Bluetooth has priority) or
`stopping` (a disable, forget or reset is finishing; during a session this
completes when the timer opens, and links show `revoking`). While `waiting`,
`last_error` gives the reason, for example `low_memory`. `sharing_available` is true only
with a live, authenticated relay connection and no maintenance in progress.
`maintenance` is true while the relay writes its flash settings; other
mutations and OTA return `409 relay_maintenance` until it clears.
`stopping` is true while a disable, forget or factory reset is cleaning up.
`last_error` names the most recent enrollment or connection failure. The
last three numbers are for hardware qualification: the relay task's lowest
remaining stack and current heap. `tls_arena` describes the block the relay
reserves when it starts, which holds its TLS state so fragmentation of the
general heap cannot keep sharing offline: its size, what is free now, the
lowest free amount this boot, and how many allocations did not fit and used
the general heap instead. `size_bytes` is 0 if the block could not be
reserved; the relay then uses the general heap.

### Share links

Creating and revoking links are asynchronous jobs so relay storage and network
work never stall the timer loop. Read `GET /api/v1/relay` first and send its
`next_job_id` as `job_id` and its `boot_id` as `if_boot_id`:

```sh
curl -X POST -H 'Content-Type: application/json' \
  -d '{"job_id":3,"if_boot_id":"bcb49155","ttl_s":3600,"label":"Partner"}' \
  http://smarttimer.local/api/v1/relay/links
# 202 with the status object; poll GET /api/v1/relay until job.id == 3
# and job.state == "done"; job.result is "ok" and job.fp_hash names the link
curl 'http://smarttimer.local/api/v1/relay/links/share?fp_hash=...'
```

A job's arguments are fixed when it is accepted. Resending the same `job_id`
with the same arguments is a safe retry that returns `202` while the job runs
and its final result once done; a different operation with that ID, an older
ID, or an ID other than `next_job_id` returns `409 relay_job_conflict`. Only
one job runs at a time (`503 relay_busy`). A timer restart changes `boot_id`
(`409 stale_boot`).

Creating a link requires an open timer (`409 timer_active`) and a live relay
connection (job result `relay_unavailable`). A link grants every partner
control for `ttl_s` seconds (60–86400), starting now; restarting the timer
ends it. The share URL is returned only after the relay confirmed the
registration (`409 link_not_ready` with `state` until then). `GET /api/v1/relay/links`
lists live links with `fp_hash`, `label`, `expires_in_ms`, `state`
(`pending`, `ready`, `revoking`, `revoked`, `expired`, `error` or `unknown`)
and `url_available`. **One** link is live at a time (`409 link_table_full`;
revoke it or let it expire) and twenty can be created per hour. A revoked link
frees its place once the relay confirms the revocation; the web page's Renew
revokes the link and creates a fresh one (new URL) with the same name and
lifetime. A start or extension beyond a link's expiry returns
`maximum_duration_reached`, and the firmware commands release for a session the
link started, extended, paused or sealed when that link expires
(`last_release_reason` `share_link_expired`).

`POST /api/v1/relay/links/extend` takes `job_id`, `if_boot_id`, `fp_hash` and
`ttl_s` (60–86400). It adds `ttl_s` to the link's expiry (from now if it has
already expired in this boot: reactivation), capped at 24 hours from now. The
**URL stays the same**: the timer's record is authoritative, the relay updates
its grant, and an open partner page follows the new expiry from the timer's
signed status. It works during a session and moves the fail-open deadline of a
session this link holds. A revoked link, or one from before a restart, returns
`link_not_found`.

`POST /api/v1/relay/links/revoke` takes `job_id`, `if_boot_id` and `fp_hash`.
It works during a session and affects only that link: its commands are
refused from the moment the request is accepted (including one already in
flight), the relay is told right away, and other links keep working. Share
links end at restart, so the timer saves the revocation to flash once it is
open again. A storage failure refuses every link until the relay is disabled
and re-enabled. `POST /api/v1/relay/disable` ends every link and turns the relay
off; it works during a session and returns `202` while `stopping`. If the
relay is unreachable at that moment, it is told on the next enable in the
same boot. `POST /api/v1/relay/forget` also erases credentials and the
signing key; it requires an open timer.
After successful Disable or Forget cleanup, enrollment can start again on
the same boot. Wait until `stopping` and `maintenance` are both false before
submitting new enrollment details; cleanup failures must be resolved first.
The local timer UI polls relay availability outside Settings and shows a
separate relay badge for a configured relay. Failed or stale relay status
disables link creation until a fresh, ready connection is confirmed. An
offline relay does not mean the browser has lost its local timer connection.

Errors use the standard error object: `409 timer_active`, `409
bluetooth_active`, `409 relay_maintenance`, `409 relay_job_conflict`, `409 stale_boot`, `409
relay_not_enrolled`, `409 relay_disabled`, `409 link_table_full`, `429
link_rate_limited`, `404 link_not_found`, `503 relay_busy` (retry),
`503 relay_low_memory`, `503 storage_failed` and `400 invalid_relay_request`,
`invalid_relay_job_id`, `invalid_spki_pin`, `invalid_link_ttl` or
`invalid_link_label`. A finished job reports the same codes in `job.result`.

## Firmware OTA

[GitHub update checks](GITHUB_UPDATES.md) run in both browser interfaces,
never on the timer. `GET /api/v1/capabilities` adds `updates`: `configured`, `base_url`,
`public_key` (uncompressed P-256 point in hex), `target`, and
`check_mode: "manual_browser"`. Feed/image downloads occur in the browser,
not through a device-side check endpoint, and do not write device flash.

For a signed release, supply `release_version`, `image_sha256` (64 lowercase
hex characters) and `release_signature` (128 lowercase hex characters) with
the size/CRC fields in both preparation and multipart upload, before the file
part. All three are required together. The signature binds target, version,
size, CRC and SHA-256; it is checked before flash writing. The prepared signed
hash must match the upload, and the streaming image SHA-256 must match before
the final chunk is committed. Invalid preparation signatures return
`400 invalid_release_signature`. If V1 cannot reserve its transient signature
verification stack, preparation returns `503 release_verification_busy`
without consuming the physical arm. Retry when memory is available.
Upload validation errors use the existing
OTA error response. Local file uploads without these fields keep their
existing physical-arm/product/rollback checks.

Schemas for the prepare step: [`schemas/local/ota-prepare-request.schema.json`](schemas/local/ota-prepare-request.schema.json) (request), [`schemas/local/ota-prepare-response.schema.json`](schemas/local/ota-prepare-response.schema.json) (response). The upload endpoint (`POST /api/v1/ota`) is multipart with the same size/CRC fields as the prepare step plus a `firmware` file part; success returns `{"updated": true, "restarting": true}`.

The easiest and safest OTA client is the local Settings interface. It computes the file size and CRC32 automatically and explains the physical-arm state.

An OTA between Smart Timer builds updates only the application slot. It does not erase the persistent station Wi-Fi, device-name, AP, or supported Bluetooth configuration records, and it does not touch the vendor filesystem partition. V1 keeps its records in its existing fixed EEPROM-emulation sector; V2 keeps them in the dedicated `opensmarttimer` NVS namespace. The ten-second physical factory reset is the operation that restores those OpenSmartTimer settings. Vendor firmware restored later may interpret or replace other retained settings differently even though the OTA write itself does not erase the settings area.

API OTA requirements:

1. Timer state is open.
2. Press and release the Reset button within two seconds to arm updates for five minutes. A double blue-LED flash repeats while armed. Holding it instead shows the settings-reset warning blink; reaching ten seconds restores every user setting and restarts with a default open AP without changing firmware. On V2, releasing after 3–5 seconds opens Bluetooth pairing rather than arming OTA.
3. Preferably call `POST /api/v1/ota/prepare` with `image_size` and eight-digit hexadecimal `image_crc32`, then wait about 1.5 seconds. This gives other bundled UI clients time to receive an HTTP 503/`ota_in_progress` response and back off for 30 seconds.
4. Send a multipart POST containing the same `image_size`, `image_crc32`, and file field `firmware`.
5. Do not interrupt power while the response and restart are pending.

Example using an application image for the connected model. Set `IMAGE` to
your downloaded file:

```sh
IMAGE=/path/to/firmware.bin
SIZE=$(wc -c < "$IMAGE" | tr -d ' ')
CRC=$(python3 -c 'import sys,zlib; data=open(sys.argv[1],"rb").read(); print(format(zlib.crc32(data),"08x"))' "$IMAGE")

curl -X POST \
  -d "image_size=$SIZE" -d "image_crc32=$CRC" \
  http://smarttimer.local/api/v1/ota/prepare
sleep 2

curl -X POST \
  -F "image_size=$SIZE" \
  -F "image_crc32=$CRC" \
  -F "firmware=@$IMAGE;type=application/octet-stream" \
  http://smarttimer.local/api/v1/ota
```

Success:

```json
{"updated":true,"restarting":true}
```

The firmware independently verifies declared length, CRC32, ESP image header, and project marker or exact vendor rollback allowlist. The filesystem partition is never written by this endpoint.

For recovery from an allowlist mismatch—such as a newer legitimate vendor main application—press and release the Reset button a second time while the ordinary arm window is still active. `ota_unverified_allowed` becomes `true`, the blue LED changes from two to three short flashes every two seconds, and `/api/v1/ota/prepare` returns `"unverified_firmware_allowed":true`. The bundled UI displays a page-wide warning, changes the install-button label, and requires a second on-screen confirmation.

This one-use override bypasses only the project marker and known-vendor allowlist. Target-family header validation (V1 ESP8266/DOUT/4 MB/40 MHz or V2 ESP32-C3/DIO/4 MB/80 MHz), OTA capacity, declared/actual length and CRC32, same-origin enforcement, and the open-timer requirement remain active. It is held only in RAM, does not extend the original five-minute window, expires or disappears on reboot, and is consumed when an upload begins even if that attempt fails. Size and CRC establish transfer integrity, not hardware compatibility or authenticity; a wrong image can remove all web recovery. The amber indicator cannot safely signal this mode because it represents energized lock-driver state.

The prepare step responds with `{"ready":true,"quiet_window_ms":10000,"unverified_firmware_allowed":false}` (the last flag reflects the physical authorization). It does not write flash or consume the physical arm. It reserves ten seconds for the matching upload; the quiet window is extended while upload chunks arrive. Direct `POST /api/v1/ota` remains supported for existing clients, but cannot warn other browsers before its upload begins. All SSE streams are closed before the quiet/upload window. During a reserved/uploading window, other HTTP traffic receives:

```json
{"error":"firmware update in progress","code":"ota_in_progress","state":"open","retry_after_ms":30000}
```

There is deliberately no remote OTA-arm endpoint.

## Legacy compatibility API

These text routes preserve compatibility with the legacy vendor UI contract. Their response bodies remain unchanged so existing exact-string and semicolon parsers continue to work. New integrations should use `/api/v1`; its successful timer mutations already return the complete JSON state and therefore do not require an immediate `/getTimer` confirmation request.

| Method and path | Response / behavior |
| --- | --- |
| `GET /isAlive` | `open` or `closed` |
| `GET /getVersion` | Firmware version |
| `GET /getTimer` | `<remaining_ms>;<open\|closed>;<paused_0\|1>` |
| `GET /setMagnetLock?t_state=1&i_state=<ms>` | Start or set remaining time; `closed`; returns HTTP 409 while the emergency input is active |
| `GET /setMagnetLock?t_state=0&i_state=0` | Release; `open` |
| `GET /pause` / `GET /resume` | `<remaining_ms>;closed`; `/pause` returns `409 failed` when pause is switched off |
| `GET /setLockMode?l_state=1` | Lock software controls; `OK`, or `409 failed` when control locking is switched off |
| `GET /getLockMode` | `0` or `1` |
| `GET /getSsid` | Cached SSIDs separated by vertical-tab characters; open timer required. If no cache exists, starts a non-blocking scan and returns an empty list so a legacy client can retry without stalling the safety loop. |
| `GET /connect?ssid=<name>&pw=<password>` | Test and save station credentials on success |
| `GET /connect?ssid=standalone-mode&pw=` | Clear station credentials and use AP Mode |
| `GET /getWifiMode` | `STA` or `AP` |
| `GET /getDeviceId` | ESP chip ID in hexadecimal |
| `GET /reset` | Restart; open timer required |
| `GET /UpdateCheck` | Always `false`; cloud update checking is disabled |
| `POST /UpdateRun` | HTTP 410; cloud updating is disabled |
| `GET /update` | Opens the local interface |
| `POST /update` | Same physically armed firmware-only OTA flow as `/api/v1/ota` |

The older 1.3 vendor `/mqtt`, `/mqttStop`, and `/pwValidation` routes are intentionally absent because they belonged to its remote-broker/password design. `/description.xml` UPnP discovery is also not reproduced; mDNS provides local discovery without the vendor service model.

## Other local routes

`/`, `/index.html`, `/styles.css`, `/i18n.js`, and `/app.js` serve the embedded UI. Language and dark/light appearance preferences are kept only in the browser tab's `sessionStorage`; they are never sent to or stored by the controller. `/generate_204`, `/hotspot-detect.html`, `/fwlink`, and `/connecttest.txt` return the UI for common captive-portal probes.

## Integration guidance

For a local dashboard or Home Assistant REST integration:

- Poll `GET /api/v1/status` no faster than once per second, or explicitly open one SSE stream and retain polling as fallback.
- Use `state`, `remaining_ms`, `sealed`, and `emergency_input_active` as primary sensor values.
- Use the action endpoints for commands; do not emulate legacy browser sequences.
- Consume the JSON returned by a successful timer mutation instead of issuing an immediate status read.
- Supply a unique `operation_id` for every timer mutation that may be retried after a lost response.
- Include the status `boot_id` as `if_boot_id` on every command based on that snapshot.
- Include the status `session_id` as `if_session_id` on commands intended for a particular active session.
- Keep the event `boot_id` and `next_after` together; reset local history on `cursor_reset` and disclose gaps on `truncated`.
- Treat `409` as a state conflict, not a transport failure.
- Read `/api/v1/capabilities` once after discovery or firmware change.
- Read diagnostics/events only on demand rather than polling them continuously.
- Expect the connection to disappear after successful Wi-Fi provisioning, renaming, restart, or OTA.
- Rediscover by the new hostname or router-assigned address after a rename.

SSE is available only through the explicit bounded endpoint documented above. WebSocket, outbound webhooks, MQTT, and Home Assistant discovery are not enabled in this release.

## Deliberately unsupported operations

- Cloud registration, cloud proxying, telemetry, or remote update checks.
- Device password validation or account endpoints.
- Software unlock of a control-locked session.
- Remote OTA arming.
- Raw filesystem upload through the replacement firmware.
- Remote/software settings reset. The complete reset deliberately requires a ten-second physical Reset-button hold while open; it restores the device name and all station/AP/Bluetooth settings but does not roll back firmware.
- Permissive cross-origin browser control.
