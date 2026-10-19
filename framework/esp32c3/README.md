# ESP32-C3 framework profile

This directory defines the reproducible, reduced Arduino-ESP32/ESP-IDF library
package used **only** by the MagBound V2 target. The MagBound V1 target keeps
its field-tested ESP8266 PlatformIO platform, linker script, and framework.

## Why this exists

The stock ESP32-C3 precompiled library package left too little room in the
vendor-compatible 1.25 MiB OTA slot for the interface, HTTPS and Bluetooth.
This profile removes unused compiled features without changing the original V2
partition table, while retaining the foundations for HTTPS release downloads.
It also sizes Bluetooth for the one peripheral connection the application
uses, so the radio can run beside the relay's TLS connection.

It retains:

- IPv4 and IPv6;
- Wi-Fi station and access-point operation;
- HTTPS client support and the certificate bundle for a possible future
  signed release downloader;
- NimBLE peripheral/GATT-server roles, notifications, encryption, and bonding
  primitives for one inbound app connection. Central, observer and GATT-client
  roles, the second connection and the stock buffer-pool sizes were removed
  (profile 5.5.0+opensmarttimer.8) after bench measurements showed Bluetooth
  holding about 64 KB of heap and starving the relay's reconnect; it now holds
  about 51 KB while the radio is on. The controller keeps its connection
  feature (`BT_CTRL_BLE_MASTER`, which is not only the central role): without
  it connectable advertising is rejected;
- application rollback APIs and the existing V2 OTA layout.

It removes or constrains PPP, enterprise EAP, WPS, FTM, BLE 5 extras, C++ exceptions, full
Newlib formatting, core dumps, and unused diagnostic/error strings. Assertions
remain enabled but are silent: a failed check still panics and reboots (which
drives the lock output low) without storing expression, file or `esp_err_t`
name text that no one can read on the enclosed device. The
retained pairing primitives are framework capabilities, not authorization for
any new GATT service or peer-timer control.

## Future capabilities versus exposed features

The HTTPS client, redirect handling, certificate bundle, certificate-date
checks, SHA-256/SHA-512 and RSA/ECDSA verification primitives are retained for
a future release downloader. That downloader must set the system clock before
CA-validated HTTPS; the existing relay retains its separate pinned-key policy.
This package alone enables no GitHub polling, automatic download or extra OTA
route. The application implements manual browser-assisted GitHub updates,
verifying separately signed update descriptors and streaming SHA-256 through
the existing physically armed, open-timer OTA routes. `release.json` and raw
checksum files are not signed manifests; `updates.json` carries the signed
descriptors. See [GitHub updates](../../docs/GITHUB_UPDATES.md).

The framework no longer compiles BLE central, scanning or GATT-client support.
The application's connection limit is one and peer capability flags stay false.
Timer-to-timer Bluetooth would need those roles restored in a later framework
profile, along with explicit physical approval, separate peer credential
handling, disconnect/lease behavior and two-device tests. Four framework bond
slots remain available; the current three-app registry and replacement/cleanup
policy are unchanged.

Framework libraries are build inputs linked into the application image, not a
separate runtime package installed on the timer. A later application OTA can
include a revised framework without replacing the bootloader or partitions.
Keeping these foundations now prepares the build environment; it does not
make future features available without another application update. The release
gate still enforces OTA-slot margin on the actual linked image.

## Locked inputs

`lock.json` pins the library-builder, Arduino, ESP-IDF, TinyUSB, container image
digest, package identity, timestamp, and hashes of the resolved configuration,
dependency lock, and resulting archive. `builder.patch`,
`arduino-component.patch`, `defconfig.opensmarttimer`, and
`dependencies.lock` are all build inputs and must be reviewed together.

## Build and verify

Docker is required for a clean framework build:

```sh
./scripts/build_framework_esp32c3.sh
```

The script builds from pinned commits in a temporary directory, creates only
the ESP32-C3 package at `.framework/esp32c3-libs`, writes the deterministic
archive `dist/framework/OpenSmartTimer-ESP32C3-Framework-ost2.tar.gz`, and verifies it
against `lock.json`. The first build after intentionally changing the profile
prints replacement hashes; review and commit those values before using the
package for a release.

To demonstrate byte reproducibility, run the clean build in two fresh source
checkouts and compare the archive SHA-256 values. `--rebuild` forces the same
clean temporary-directory path even when a verified local package exists. CI
performs the locked framework verification before it compiles firmware.

```sh
./scripts/build_framework_esp32c3.sh --rebuild
```

An already-built archive is installed with:

```sh
python3 scripts/framework/install_framework_esp32c3.py \
  dist/framework/OpenSmartTimer-ESP32C3-Framework-ost2.tar.gz
python3 scripts/framework/verify_framework_esp32c3.py
```

Then build the targets independently:

```sh
./scripts/build_firmware.sh v1
./scripts/build_firmware.sh v2
```

The V2 wrapper also exports the locked `SOURCE_DATE_EPOCH` while compiling the
application so Arduino's remaining `__DATE__`/`__TIME__` uses do not make two
otherwise-identical firmware images differ. V1 is intentionally not given that
environment change.

## Recovery boundary

This package can make an application defer `esp_ota_mark_app_valid...`, but an
OTA application does not replace the timer's installed bootloader. Automatic
rollback occurs only if that existing bootloader was built to honor pending
OTA states. Until that is established on hardware, delayed confirmation is an
additional guard—not a guaranteed or supported recovery path. Deliberately
interrupting the only recoverable unit merely to test it is not required for a
release. Manual application rollback through the running OpenSmartTimer web
updater has been exercised successfully. USB-C is treated as power-only, and
inaccessible/unverified UART pads are not an operational fallback.
