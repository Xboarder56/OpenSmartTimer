# Relay firmware integration

The V2 timer client for `ost-relay/1`. Default V2 builds include the client, inactive until the owner enrolls or enables it through the local [relay API](API.md#experimental-remote-relay-v2-only). V1 contains no relay code. See [Remote sharing](REMOTE_SHARING.md) for owner setup and [the protocol](RELAY_PROTOCOL.md) for the wire contract.

## Code structure

| Path | Responsibility |
| --- | --- |
| `src/relay/`, `include/relay/` | Portable protocol core and ESP32 TLS, crypto, storage, transport and runtime adapters |
| `src/WebRelay.cpp` | Owner `/api/v1/relay/*` routes |
| `src/main.cpp`, `src/WebApi.cpp`, `src/WebStatus.cpp`, `include/WebApplication.h` | Runtime wiring, routes and `features.relay` |
| `relay/tests/firmware/` | Portable C++ tests, OpenSSL provider and interoperability driver |

The build switch removes the relay code, API and advertised capability. V1 always excludes it. See [relay build options](BUILDING.md#relay-build-option).

## Runtime and timer authority

`RelayRuntime` runs TLS, protocol state and cryptography in one FreeRTOS task with a 16 KiB stack. The Arduino loop receives commands through a mailbox and applies them through `TimerAuthorityControl` and the shared `TimerCommandAuthority`. The relay task reads published snapshots and never drives GPIOs or accesses `TimerController` directly.

Owner API calls become relay-task jobs. The loop continues servicing the mailbox while waiting; a job that cannot start within the bound returns `503 relay_busy`. Link mutation jobs use a caller-supplied `job_id` and `if_boot_id`; accepted arguments are immutable and exact retries return the same job.

Connections require station Wi-Fi, sufficient heap and no active OTA/restart/network transition. Blocking TLS connection work runs outside shared locks. Enrollment uses HTTPS pinned to the key in the setup code. Device traffic uses outbound WSS with the `ost-relay.v1` subprotocol and bounded frames; its TLS layer records the server's key without checking a stored pin, the hello binds that key, and the session goes live only after the relay's acknowledgement tag verifies (`relay_auth_failed` otherwise). A renewed relay certificate therefore needs no new setup.

The adapter accepts fixed-duration starts; the partner UI chooses a duration within a random range before submitting a fixed start. Its operation ledger retains replay protection after the client result ring evicts a result. History attributes relay actions with `via_share_link`.

## Owner lifecycle

| Operation | Behavior |
| --- | --- |
| Enroll/enable | Requires an open timer and station Wi-Fi; setup origins require local review and physical confirmation |
| Create link | Requires a live authenticated relay and token-specific acknowledgment before returning a usable URL |
| Revoke | Refuses the link immediately, rechecks before mutation and queues relay notification; storage waits until the timer is open |
| Extend/reactivate | Keeps the URL and changes expiry, within the 24-hour limit; available until the timer reboots |
| Disable | Revokes every link and stops the task; retained objects carry pending revocations until restart or re-enable |
| Forget/settings reset | Erases relay configuration, tokens, capability key and signing key |
| Emergency input | Disables sharing and ends the grant; the owner must enable sharing again |
| Restart | Ends all links; cross-boot capabilities are not supported |

The timer holds one live link. Creating another returns `409 link_table_full`; acknowledged revocation frees the slot. Renew revokes the previous grant and mints a new one.

A link's local hold deadline prevents its commands from keeping a session active beyond grant expiry. Sessions started, extended, paused or sealed through the link open at that deadline even if the relay is unavailable. Disabling or revoking a link removes its authority; it does not itself command release.

Token storage occurs on lifecycle changes rather than each command, status or connection. Creation is limited to 20 per hour. Relay storage writes require an open timer and a maintenance interlock; other mutations and OTA receive `409 relay_maintenance` during that work. Offline revocations are sent before status after reconnect.

## Cryptography and resources

Each ephemeral P-256 ECDH accept is authenticated once with the timer's ECDSA P-256 identity, RFC 6979 deterministic signing and low-S encoding. Identical retries reuse the cached signed accept. Routine status, events and results use directional AES-256-GCM, without per-response signatures.

ECDSA, ECDH, HKDF and AES-GCM use providers already present in the pinned V2 framework. Relay deployments use the same firmware image with runtime configuration; no per-host build is required. Enrollment checks the server's SPKI key against the setup code's pin; later connections bind the hello to the key the server presents and require the relay's acknowledgement, proving it holds this timer's enrollment secret. Neither step uses CA-chain, hostname or certificate-expiry validation. See [Security](../SECURITY.md).

The client and enrollment refuse connections below 40 KiB free heap or a 20 KiB largest block (the largest TLS allocation is its ~17 KB receive buffer). These floors are not reservations against subsequent allocations. Result records and channel keys are bounded RAM state; they do not promise exactly-once behavior across power loss. Uptime wrap disables sharing, and link creation refuses an expiry crossing that boundary.

The image verifier requires at least 30 KiB free OTA space. Flash fit alone cannot establish runtime fit.

## Bluetooth priority

A Bluetooth app connection or open pairing window pauses the relay task. WSS closes with `1001 local_bluetooth`, the task stack is freed and protocol state remains in RAM without a storage write.

The relay resumes three seconds after Bluetooth becomes idle, including during an active timer session. Failed stack allocation retries every five seconds and reports `low_memory`. The client also waits, with the same `low_memory` reason, while less than 40 KB of heap or a 20 KB contiguous block is free; that reason replaces any older connection error until the relay is live again. Turning the Bluetooth radio off releases the Bluetooth stack's heap two seconds after its link closes, so the relay is not left short of memory until the next restart. Disable or Forget requested while paused completes storage cleanup when the timer opens. Temporary Bluetooth-only mode also makes the relay unavailable while networking is suspended.

## Verification

Run the source tests and build-boundary checks:

```sh
./relay/tests/check.sh
./scripts/build_firmware.sh all
./scripts/checks/check_relay_boundary_images.sh
```

The portable core is tested against OpenSSL and Python/browser peers. ESP32 adapter behavior requires tests on the actual image and hardware; [hardware tools](../relay/tools/hardware/README.md) describe the automated transport suites. See [Testing and verification](VALIDATION.md) for physical verification limits.
