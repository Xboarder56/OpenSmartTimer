# Local HTTP API JSON Schemas

JSON Schema (Draft 2020-12) files for the core endpoints documented in
[`../../API.md`](../../API.md). Shared shapes live in `common.schema.json`
and are referenced with `$ref`.

## Files

### Shared

| Schema | Purpose |
| --- | --- |
| [`common.schema.json`](common.schema.json) | Shared `$defs` — enums, mutation envelope, event shape, error envelope, AP summary |
| [`error-response.schema.json`](error-response.schema.json) | Modern JSON failures; error codes are extensible strings |

### Endpoints

| Schema | Endpoint |
| --- | --- |
| [`status.schema.json`](status.schema.json) | `GET /api/v1/status` |
| [`timer-start-request.schema.json`](timer-start-request.schema.json) | `POST /api/v1/timer/start` (request) |
| [`timer-adjust-request.schema.json`](timer-adjust-request.schema.json) | `POST /api/v1/timer/adjust` (request) |
| [`timer-mutation-response.schema.json`](timer-mutation-response.schema.json) | Response of every timer mutation under `POST /api/v1/timer/*` |
| [`timer-controls.schema.json`](timer-controls.schema.json) | `GET` and `POST /api/v1/timer/controls` |
| [`timer-controls-request.schema.json`](timer-controls-request.schema.json) | `POST /api/v1/timer/controls` (request) |
| [`wifi-scan-response.schema.json`](wifi-scan-response.schema.json) | `GET`/`POST /api/v1/wifi/scan` |
| [`wifi-status.schema.json`](wifi-status.schema.json) | `GET /api/v1/wifi/status` |
| [`wifi-ap.schema.json`](wifi-ap.schema.json) | `GET /api/v1/wifi/ap` |
| [`wifi-ap-update-request.schema.json`](wifi-ap-update-request.schema.json) | `PUT /api/v1/wifi/ap` (request) |
| [`wifi-ap-update-response.schema.json`](wifi-ap-update-response.schema.json) | `PUT /api/v1/wifi/ap` (response) |
| [`wifi-configure-request.schema.json`](wifi-configure-request.schema.json) | `POST /api/v1/wifi/configure` (request) |
| [`wifi-configure-response.schema.json`](wifi-configure-response.schema.json) | `POST /api/v1/wifi/configure` (response) |
| [`wifi-forget-response.schema.json`](wifi-forget-response.schema.json) | `POST /api/v1/wifi/forget` |
| [`device-name.schema.json`](device-name.schema.json) | `GET /api/v1/device/name` |
| [`device-name-update-request.schema.json`](device-name-update-request.schema.json) | `PUT /api/v1/device/name` (request) |
| [`device-name-update-response.schema.json`](device-name-update-response.schema.json) | `PUT /api/v1/device/name` (response) |
| [`system-identify-response.schema.json`](system-identify-response.schema.json) | `POST /api/v1/system/identify` |
| [`system-restart-response.schema.json`](system-restart-response.schema.json) | `POST /api/v1/system/restart` |
| [`system-diagnostics.schema.json`](system-diagnostics.schema.json) | `GET /api/v1/system/diagnostics` |
| [`bluetooth-status.schema.json`](bluetooth-status.schema.json) | V2 Bluetooth status, radio enable/disable, pairing and approved-app responses |
| [`events-list.schema.json`](events-list.schema.json) | `GET /api/v1/events` |
| [`capabilities.schema.json`](capabilities.schema.json) | `GET /api/v1/capabilities` |
| [`ota-prepare-request.schema.json`](ota-prepare-request.schema.json) | `POST /api/v1/ota/prepare` (request) |
| [`ota-prepare-response.schema.json`](ota-prepare-response.schema.json) | `POST /api/v1/ota/prepare` (response) |

## Consuming these

The prose in [`../../API.md`](../../API.md) remains the human-readable
reference; these schemas exist for typed clients, CI validators, and
integration tests. Covered endpoint sections in `API.md` link to their schemas.

The SSE frame at `GET /api/v1/events/stream` carries the same JSON
object as `status.schema.json` as its `data` field.

The **mutation response** for every `POST /api/v1/timer/*` endpoint
extends `status.schema.json` with a `mutation` object; see
`timer-mutation-response.schema.json`.

`wifi-status.schema.json` also describes the equivalent BLE management JSON
reply. Its `state: "disabled"` and `network_mode: "bluetooth"` values are
observable over BLE while Wi-Fi/AP (and therefore HTTP) is off. The optional
V2 `wifi_suspend_supported` flag must be true before offering suspend/resume.

## Validation and coverage limits

Install the root `requirements-dev.txt` in a Python virtual environment, then
run `./scripts/check.sh integration` from that environment (or set `PYTHON` to
its interpreter). CI installs the same dependencies and runs `check.sh all`.
The suite checks every schema and local reference, validates actual preview
HTTP responses, SSE status and accepted JSON requests, and exercises synthetic
Bluetooth-only/OTA examples plus rejected variants. Resources resolve locally;
validation never downloads schemas. See [test instructions](../../../tests/README.md).

The schemas do not yet cover local relay-owner request/response bodies, binary
BLE frames, management-specific OTA chunk messages, multipart HTTP OTA upload,
or separate request bodies for every timer/Bluetooth action. Relay contracts
remain in [API.md](../../API.md) and [RELAY_PROTOCOL.md](../../RELAY_PROTOCOL.md);
binary BLE contracts have their own protocol tests. JSON Schema also cannot
check device state, saved-password availability, byte limits on Unicode, raw
JSON duplicate keys, or relationships such as `min_ms <= max_ms`; firmware
parser/authority tests remain necessary.

Preview and synthetic examples are not firmware serialization proof. Validate
saved device JSON from the repository root and retain build/capture provenance
for hardware qualification:

```sh
python tests/integration/api_contract.py status /path/to/reply.json
```

## Stability

These schemas mirror the fields returned by the current firmware. Fields
may be added in additive backwards-compatible ways without a schema
version bump. Field removals or type changes accompany a firmware
version note in [`../../API.md`](../../API.md) and [`../../../CHANGELOG.md`](../../../CHANGELOG.md).

The **legacy compatibility API** (`/getTimer`, `/setMagnetLock`, etc.)
returns plain text, not JSON, and is intentionally not schematized.
