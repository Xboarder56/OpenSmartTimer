# Remote relay

Self-hosted remote sharing for **MagBound Smart Timer V2**. The timer maintains
an outbound WSS connection to the relay; a partner opens a time-limited link
in an independently hosted HTTPS control page. The project provides software
and does not operate a hosted relay.

> [!WARNING]
> Remote sharing is **experimental**. The relay, the control page and the
> timer's relay client have had no independent security review. Run it only
> for timers and people you trust, keep it off when it is not needed, and
> never treat remote control as a way to release.

Default V2 builds include the client, inactive until owner setup in
**Settings → Remote sharing**. V1 has no relay support. Read
[Safety](../SAFETY.md) and the [owner guide](../docs/REMOTE_SHARING.md) before use.

Original code uses the repository's [AGPL-3.0-only license](../LICENSE) and is
provided as is, without warranty of any kind.
Dependencies retain their own licenses; see
[third-party notices](../THIRD_PARTY_NOTICES.md).

## Components

| Location | Purpose | Runs on |
| --- | --- | --- |
| [`src/ostrelay/`](src/ostrelay/) | FastAPI/SQLite server: enrollment, timer WSS, routing and encrypted command forwarding | Operator host |
| [`spa/`](spa/) | Trusted partner page and authenticated encrypted channels | Partner browser |
| [`mock-timer/`](mock-timer/README.md) | Timer simulator and loopback owner page | Development machine |
| [`tests/`](tests/README.md) | Python, Node, C++ and browser interoperability tests | Development machine/CI |
| [`tools/`](tools/README.md) | Certificate/pin utilities and hardware transport checks | Operator/development machine |
| [`ops/`](ops/) | Caddy configuration and database backup/restore | Operator host |

The client lives in the firmware tree; see
[firmware integration](../docs/RELAY_FIRMWARE.md). Firmware and relay share the
release version in `platformio.ini`. The wire contract is
[`ost-relay/1`](../docs/RELAY_PROTOCOL.md), independent of hardware V1/V2.
Keep firmware, server and trusted page implementations compatible.

## Trust model

The relay forwards opaque AES-256-GCM traffic and public routing metadata. By
design it receives no complete link capability or raw grant fingerprint and
cannot derive session traffic keys; none of this has been independently
reviewed. The trusted page authenticates the timer's
ephemeral P-256 ECDH handshake with its identity signature and requires fresh
authenticated status before enabling controls.

The owner reviews and physically approves the control-page HTTPS origin during
setup. Relay responses and reconnects cannot replace it. Hosting addresses
are runtime settings; no per-host firmware build is needed.

Host the trusted page with deployment credentials, assets and TLS keys
inaccessible to the relay. A separate port or container on the same host does
not isolate it from compromise of that host. A stolen link or compromised
trusted page/device remains able to exercise link permissions; routing
metadata and denial of service are also outside the encryption boundary.
See [Security](../SECURITY.md).

The timer independently enforces link scopes, boot/session identity, controls
lock, emergency input and duration limits. It checks the relay's TLS key pin
at enrollment; afterwards each connection is bound to the key the relay
presents and authenticated with the timer's enrollment secret.

## Deploy and enroll

Follow [the operator guide](OPERATOR.md) to configure TLS, the independent
trusted-page service, admin access and enrollment policy. From `relay/`:

```sh
docker compose --profile https up -d --build --wait
```

Use **one worker and one replica**. Connected timers and pending results live
in process memory; SQLite persists enrollment and public routing metadata.
The admin port binds to localhost by default.

The public landing page, open self-enrollment and admin interface have separate
controls. Hiding the landing page does not disable enrollment or existing
share links. See [landing-page settings](OPERATOR.md#public-landing-page) and
the enrollment/admin settings in the operator guide.

Obtain a setup code through the configured portal or administrator. On an open
V2 timer connected to station Wi-Fi, enter it in **Settings → Remote sharing**,
review both HTTPS addresses and briefly press Reset to approve. Create a link
there after the relay reports ready.

## Development and verification

Install the hash-locked dependencies and browser tools described in
[relay test setup](tests/README.md), then run from the repository root:

```sh
./relay/tests/check.sh
./relay/tests/relay_e2e.sh --browser
```

The disposable stack creates its own credentials, mock timers and volumes
without reading `relay/.env` or contacting physical timers. Keep generated
reports outside source control.

For actual devices, use the [hardware tools](tools/hardware/README.md).
Automated transport checks do not qualify electrical release behavior,
recovery or production hosting isolation; see
[Testing and verification](../docs/VALIDATION.md).
