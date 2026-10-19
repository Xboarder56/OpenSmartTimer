# Development and hardware tools

Run from the repository root. These are distinct from automated checks in
`scripts/` and the test cases in `tests/`.

## Device-hosted UI preview — no hardware

```sh
python3 tools/preview_ui.py --v2 --port 8766
```

Open `http://127.0.0.1:8766`. Omit `--v2` for V1. All timer/network actions affect
mock state only; OTA is disabled. The default port is 8765. The separate Bluetooth
app needs the repository's static files served as described in its own README.

## Read-only SSE qualification — real hardware

```sh
python3 tools/validate_sse.py --url http://smarttimer.local
```

Close other UI/SSE clients first. This temporarily opens three streams and
checks headers/chunks, slot limits, REST reads and cleanup. It does not lock,
unlock, reset, change configuration or flash. It is a transport check, not
physical safety certification. See `--help` for options.

## Opt-in REST/SSE control benchmark — unloaded bench only

```sh
python3 tools/validate_bench.py --url http://smarttimer.local \
  --output tmp/bench-results.json --seconds 180 --allow-timer-control
```

This **energizes the outputs**, starts short timers, pauses/resumes, adjusts and
releases them, and tests a three-second controls-locked session. Never run it
on a person or an occupied lock. Start unlocked with emergency input inactive,
close other UI/SSE clients, and keep independent physical release available.
Cleanup is attempted, not guaranteed if the network or hardware fails.

The report includes API assertions, request latency, resource samples and three
concurrent SSE streams. Existing output files are not overwritten. Wi-Fi,
approvals, firmware and factory-reset settings are not changed. A passing run
is limited bench evidence, not full release qualification or safety assurance.

BLE client examples are in `examples/`. See [relay tools](../relay/tools/README.md)
for certificate, pin and relay hardware utilities.

## Mixed transport soak — unloaded bench only

```sh
python3 tools/validate_soak.py --v1-url http://v1-timer.local \
  --v2-url http://v2-timer.local --candidate /path/to/frozen-bundle \
  --output tmp/soak-results --seconds 7200 \
  --ble-python /path/to/ble-venv/bin/python --ble-key /path/to/approved-app.key \
  --relay-container YOUR_BENCH_RELAY_CONTAINER --unloaded-bench
```

Requires the existing approved BLE key, `bleak`, pinned relay Playwright tools,
an enrolled/connected V2 bench relay, no pre-existing links and no other SSE
or BLE viewers. It uses short timer/control-lock cycles, every SSE slot (two on V1, three on V2),
periodic Wi-Fi scans, approved Bluetooth management/control and restarts only
the named bench relay. It creates and later revokes one temporary share link.
Timer configuration, approvals and firmware are retained. Run only on unloaded
bench units with independent physical release available; cleanup is attempted.

The private output directory must not already exist. Reports bind to the frozen
candidate's image hashes/build IDs and record HTTP latency, heap/block/stack
reserve, stream churn and transport recovery. A request that takes longer than
5 s is listed under `request_timeouts` and asked once more; the soak fails if
the retry also times out. Allocation counts are not exposed by the firmware. An interrupted/failed run is not a passed two-hour soak, and
a passing soak does not complete the physical qualification matrix.

## Install a candidate after the physical arm — real hardware

```sh
python3 tools/install_when_armed.py --timer http://smarttimer.local \
  --image /path/to/dist-copy/OpenSmartTimer-<version>-MagBoundV2-ESP32C3.bin
```

Waits until the owner arms OTA on the timer (Reset press while open), uploads
the image and confirms the timer reports the build ID recorded in `BUILD.json`
next to the image. It never arms OTA itself. Use it to install exactly the
packaged candidate you are qualifying. Relay qualification suites are in
[relay/tools/hardware](../relay/tools/hardware/README.md).
