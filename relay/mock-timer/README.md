# Mock timer for the separate relay

This CLI lets developers exercise the relay and partner browser without
connecting hardware. It uses real HMAC/ECDSA P-256 and real HTTPS/WSS certificate
and SPKI verification. Its timer state machine and bounded RAM history are
simplified fakes; passing these tests does not establish physical firmware
behavior or complete local API parity.

## Creator UI

Install the relay Python dependencies and start a server using the
[operator runbook](../OPERATOR.md). Obtain a one-time enrollment code from the
private `/admin/enroll-code` endpoint. From `relay/`:

```sh
.venv/bin/python mock-timer/mock.py --relay https://your-host --code ENROLLMENT_CODE
```

Open the **creator address** printed by the mock (loopback port 8081 by default;
change it with `--owner-port`). This page is titled **Smart Timer** and uses the
same base stylesheet as the HTTP/BLE clients. It is served by the mock timer,
not by the relay. Its secret address is for the creator only; share the generated
partner link instead.

1. Connect the timer to the network and relay. Sharing remains grayed out until
   the authenticated relay connection is open. Set the link time limit (1–1,440 minutes; default 60) and optional name.
2. Generate the link. It stays pending until the relay confirms its database
   registration. New links cannot be generated offline. If a connection fails
   after a request was accepted, reconnection retries that existing registration.
3. Copy the ready link and share it with your partner. All supported timer
   controls are available, subject to existing timer state/lock rules.
4. Revoke when needed. Local authorization ends immediately; the page shows
   `revoking` until the relay confirms, then `revoked`.

Lifetime starts at generation and never renews on retry or first opening. It
can span timer sessions within the same boot. Expiring access does not change
the timer state. Restarting the mock creates a new boot and invalidates links.
There is no scope selector; token scope fields are reserved for future policy.

The mock saves enrollment credentials in the configured JSON file. For a
server with a generated local CA, add `--ca /path/to/ca.pem`. Public certificates
use the default trust store. Like the firmware, WSS binds the hello to the key the
server presents and requires the relay's acknowledgement.

```sh
.venv/bin/python mock-timer/mock.py --relay https://your-host --partner-origin https://trusted-host --config /path/to/mock.json --ca /path/to/ca.pem
```

See `--help` for defaults. Ctrl-C stops the local creator and mock transport.
A same-process connection loss preserves the boot, automatically reconnects,
reannounces unexpired grants/revocations and replays unacknowledged results.
The creator service binds only to `127.0.0.1`, checks Host/Origin and a per-run
management secret, bounds request bodies and sends no-store responses. It is a
developer stand-in for the firmware's local owner interface; it does not
establish HTTP/Bluetooth management parity.

## What is implemented

- Valid `ost-relay.v1` WebSocket handshake and fresh-challenge hello HMAC.
- Capability token with signing public key and exact wire-token digest registration.
- Endpoint-only authenticated ephemeral P-256 ECDH and AES-256-GCM; relay receives no bearer or fingerprint.
- A timer ECDSA P-256 (RFC 6979, low-S) signature over the full ECDH accept;
  routine status/events/results are authenticated only by channel AES-GCM.
- Token scope, boot, expiry and operation preconditions for mock mutations.
- Bounded eight-entry result/outbox history; record before send so a lost response
  does not automatically reapply an operation.
- Certificate verification and explicit rejection of a mismatched SPKI pin.
- Automatic same-process reconnect, explicit durable token ACKs, fixed-expiry
  retry, revoke-before-mint tombstones and controlled shutdown.
- Full-control creator links; 32 retained unexpired records including revoked
  records, and 20 mints/hour. Wait for expiry when the sync queue is full.

The config file contains mock capability/signing/enrollment secrets. Protect
it as development credential material; do not commit it. TLS key rotation or
server enrollment deletion may require explicit mock re-enrollment.

## Automated isolated check

From the repository root:

```sh
./relay/tests/relay_e2e.sh --browser
```

This creates its own localhost TLS stack and temporary mock credentials, runs
three timers/six viewers plus a browser flow, verifies backup/restore, and
cleans up its own resources. It never reads your saved mock config or relay
`.env`. See [the test guide](../tests/README.md); generated results stay in ignored
`tmp/relay-tests/`.
