# JSON Schemas

This directory holds JSON Schema (Draft 2020-12) files that formalize
the core JSON shapes exchanged by OpenSmartTimer's local API. See
[`local/README.md`](local/README.md) for the coverage table and explicit gaps.

## Directory map

| Folder | Contract |
| --- | --- |
| [`local/`](local/) | Local HTTP API surface documented in [`../API.md`](../API.md) — status, timer mutations, Wi-Fi, device identity, system diagnostics, Bluetooth status, events, capabilities, OTA. |

The prose reference for the local API remains in
[`../API.md`](../API.md); these schemas exist for typed clients, CI
validators, and integration tests.

## Relay message contracts

The experimental share-link relay protocol is specified in
[`../RELAY_PROTOCOL.md`](../RELAY_PROTOCOL.md); its trust boundaries are in
[`../../SECURITY.md`](../../SECURITY.md).
This directory covers the local API and has no relay JSON Schema files. The
relay protocol specification and rejection/interoperability tests define
those messages.

## Consuming these

**In a browser client (TypeScript):** generate types with
[`json-schema-to-typescript`](https://github.com/bcherny/json-schema-to-typescript)
or an equivalent, then use them at both write and parse boundaries.

**In CI:** `scripts/check.sh all` runs Draft 2020-12 validation of the local
schema graph, preview requests/responses, SSE status and regression examples.
Install `requirements-dev.txt` for local runs; see [tests](../../tests/README.md).

## Stability policy

`local/*.schema.json` mirrors the fields returned by the current
firmware and follows the additive-change policy documented in
[`local/README.md`](local/README.md).
