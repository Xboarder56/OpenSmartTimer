# Testing and verification

OpenSmartTimer separates source tests, firmware image checks and physical-device verification. A passing source suite does not establish electrical behavior, mechanical release or recovery on a particular device. Read [Safety](../SAFETY.md) before testing hardware.

## Source tests

Install the locked dependencies described in [test setup](../tests/README.md), then run from the repository root:

```sh
./scripts/check.sh all
```

The runner covers the shared controller on both hardware profiles, command authority, local API schemas, Bluetooth protocol, browser behavior and loopback preview integration. Use `host`, `web` or `integration` for individual groups. These tests do not flash hardware.

Relay checks have separate locked dependencies and include Python, Node and C++ interoperability tests, authenticated-channel rejection cases and browser checks. Follow [relay test setup](../relay/tests/README.md). The disposable HTTPS stack exercises the relay, mock timers and control page; it does not verify physical timer behavior or production hosting isolation.

## Firmware image checks

Build with the pinned frameworks and verify both model-specific application images:

```sh
./scripts/build_framework_esp32c3.sh
./scripts/build_firmware.sh all
python3 scripts/release/verify_release.py
```

Image checks cover target headers, flash layout, OTA capacity, model markers and transport boundaries. The [build guide](BUILDING.md) describes artifacts and framework verification.

## Physical-device verification

Test with the controlled load disconnected, stable power and an independent release method available. Follow the [post-install checks](INSTALLATION.md#post-install-checks) for output behavior, emergency input, timer controls, networking and recovery. Confirm electrical behavior for the actual board and installation before connecting a load.

For an installed device, this read-only tool checks SSE framing, stream-slot limits, REST responsiveness and slot cleanup:

```sh
python3 tools/validate_sse.py --url http://smarttimer.local
```

It does not measure emergency response, confirm release or prove behavior under every slow-client condition. [Hardware](HARDWARE.md) distinguishes supported controls from unresolved electrical questions.

Optional transport checks are documented alongside their tools: [relay hardware tools](../relay/tools/hardware/README.md) and [Bluetooth tools](../tools/README.md).

## Evidence and limits

[Qualification records](qualification/README.md) bind reported checks to a source identity and exact image size, CRC32, SHA-256 and build ID. A different binary or physical unit requires its own verification; a version string or build ID alone cannot establish byte identity.

The full `./scripts/check_release.sh` gate also checks these records against staged artifacts and reports incomplete qualification as a failure. Automated checks cannot observe the physical bench. Keep generated reports and credential-bearing captures outside source control.

Automatic bootloader rollback before Wi-Fi starts remains unverified and is not a supported recovery method. See [Installation](INSTALLATION.md), [Security](../SECURITY.md) and each component's guide for its current limits.
