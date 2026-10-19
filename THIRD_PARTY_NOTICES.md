# Third-party components and release notices

Original OpenSmartTimer code uses [GNU AGPL version 3](LICENSE) with the
[narrow Espressif linking permission](LICENSE-ESPRESSIF-EXCEPTION). Third-party
components retain their own licenses. The permission does not relicense them
or waive the Arduino cores' source and relinking obligations.

The RC2 review collected 156 upstream license/notice files in
[third-party/notices](third-party/notices/). The
[inventory](third-party/inventory.json) records their origins and SHA-256 values,
the pinned IDF submodules and all 30 locked relay runtime packages. Packaging
verifies every notice hash. Preserve these files with distributed copies.
The inventory includes build-tool notices and framework components beyond those
linked into a particular image; inclusion is not a claim that every listed
component executes on the timer.

## Firmware inputs

| Input | Pinned generation | License materials |
| --- | --- | --- |
| Arduino ESP8266 core | 3.1.2, PlatformIO package 3.30102.0 / platform 4.2.1 | [LGPL 2.1](third-party/notices/arduino-esp8266/LICENSE) and component notices beneath that directory |
| ESP8266 vendor SDK | SDK supplied by that pinned core package | [Espressif license](third-party/notices/arduino-esp8266/tools/sdk/License), limited to ESP8266 use; vendor binary source is not supplied |
| Arduino ESP32 core | 3.3.0, commit 554de56f40e68d591b945bd1a8c931798e8a9167 | [LGPL 2.1](third-party/notices/arduino-esp32/LICENSE.md) and component notices |
| ESP-IDF and custom C3 profile | IDF 5.5, commit b66b5448e0dfac6be135e6f0ac21010aba231180; profile 5.5.0+opensmarttimer.8 | [IDF/component notices](third-party/notices/esp-idf/); exact profile and archives in [lock.json](framework/esp32c3/lock.json) |
| IDF submodules | Eight commits in the inventory | cJSON, Mbed TLS, lwIP, protobuf-c, NimBLE and vendor radio notices |
| Managed IDF components | mDNS 1.8.2, network_provisioning 1.0.2, esp_diagnostics 1.2.1 | Notices under `third-party/notices/` and hash-pinned source archives |
| Compiler/runtime inputs | Pinned Xtensa and RISC-V toolchains from the two PlatformIO targets | GNU compiler/runtime notices, including the GCC Runtime Library Exception, under `third-party/notices/` |

The C3 Wi-Fi, Bluetooth-controller and PHY repositories contain vendor binary
libraries with their [Wi-Fi](third-party/notices/esp32-wifi-lib/LICENSE),
[Bluetooth](third-party/notices/esp32c3-bt-lib/LICENSE) and
[PHY](third-party/notices/esp-phy-lib/LICENSE) licenses. Their preferred
proprietary source is unavailable. The Espressif permission covers the identified
combination; these vendor terms still apply. FreeRTOS, newlib, TLS and other
IDF components have their own preserved notices.

## Source and relinking materials

Every firmware draft attaches `OpenSmartTimer-<version>-Source-Materials.tar.gz`
and `SOURCE-SHA256SUMS` separately from flashable images. It contains:

- `project.tar`: the reviewed Git commit, including all original firmware,
  embedded UI, relay, companion, tests, build scripts, locks and notices;
- source snapshots from the exact Arduino core packages used by both builds;
- checksum-pinned IDF, builder, free submodule and managed-component source
  archives listed in [source-inputs.json](third-party/source-inputs.json);
- the exact preferred source distributions for all 30 locked relay runtime
  packages, with their upstream PyPI hashes in the same source-input catalog;
- the verified custom C3 SDK compiler input, including its vendor binaries;
- `MATERIALS.json`, binding the source commit and every included file's hash.

Extract `project.tar` and follow [Building](docs/BUILDING.md). The supplied
Arduino sources can be modified and relinked with the original project source;
the pinned PlatformIO tools and vendor SDK inputs remain available through that
recipe. Use the supplied C3 archive as described in the build guide, or rebuild
it from the locked sources and profile. These materials are compiler inputs,
not timer firmware. [Installation](docs/INSTALLATION.md) describes manual upload
of locally built images and recovery limitations. Manual local-file upload
does not require possession of the project's release-signing private key.

The packaging tool refuses dirty source, altered notices, wrong core package
versions, missing or altered archives, and symlinked inputs. It exports tracked
Git source only. Private bench settings, certificates, signing keys, vendor
firmware dumps and raw hardware captures in ignored directories are excluded.

## Relay and web dependencies

The Python runtime versions, package license identifiers and preserved wheel
license files are recorded in the inventory and locked by
[relay/requirements.lock](relay/requirements.lock). Most use MIT or BSD terms;
Cryptography preserves its Apache/BSD and bundled native notices, Certifi uses
MPL 2.0, and typing-extensions uses PSF terms. Itsdangerous 2.2.0's wheel metadata
omits a license identifier; its preserved `LICENSE.txt` contains BSD 3-clause
terms. Certifi's corresponding certificate data is available in its exact
[2026.7.22 source distribution](https://pypi.org/project/certifi/2026.7.22/#files).

The relay image carries the project license, linking permission, this document
and the collected notices under `/opt/ostrelay/licenses`. The pinned Python
container preserves its own base-system notices. The separate control-page
server uses Caddy 2.11.4 with its [Apache license](third-party/notices/caddy/LICENSE).
Browser applications use the project's own shipped assets; Playwright and other
test tools are development dependencies rather than runtime assets.

This review covers the pinned RC2 firmware and relay materials. Dependency
updates require refreshed inventory, notices, source hashes and build checks.
