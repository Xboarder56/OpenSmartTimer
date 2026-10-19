# Building and continuous integration

This guide produces model-specific **application** images for the MagBound
Smart Timer V1 and V2. The targets use different chips, frameworks, flash
modes, linker layouts, package caches, and artifact names. Their images are not
interchangeable.

## Requirements

### Compile-time product contracts

`include/BoardProfile.h` selects an explicit product profile. The build wrappers
set `SMART_TIMER_TARGET_MAGBOUND_V1_ESP8266` or
`SMART_TIMER_TARGET_MAGBOUND_V2_ESP32C3`; V2 also retains its existing
`SMART_TIMER_HARDWARE_V2` feature guard. Arduino builds reject missing, crossed,
or wrong-chip selections. Host tests can use their existing V1/V2 defaults.

`include/boards/` holds the named product GPIO/polarity/identity profiles;
`include/flash/` holds each product's image-header and flash contracts.
`Hardware.h` still implements startup latches, input handling and output writes.
The profiles define the existing pin, polarity, image and flash contracts.
The V1 profile keeps DOUT for its GPIO10 emergency input; V2 keeps its existing
1,310,720-byte A/B slots. Release checks independently verify image headers and
the compiled partition table, while host compile tests reject invalid targets.

The CI reference environment uses:

- Python 3.12;
- PlatformIO Core 6.1.19;
- Node.js 24 for JavaScript syntax checks;
- a C++17 host compiler for controller tests;
- the root `requirements-dev.txt` packages for API JSON Schema checks;
- Docker and Git when generating the V2 framework from a clean checkout.

Install PlatformIO explicitly rather than relying on an unrelated global
version:

```sh
python3 -m pip install platformio==6.1.19
```

The first PlatformIO build downloads the pinned platform/toolchain packages.
The V2 framework build also fetches pinned upstream commits and its container
image, so a clean build requires network access.

For source checks, use a virtual environment with `requirements-dev.txt`
installed; [tests/README.md](../tests/README.md) gives the commands. CI installs
the same requirements before running `./scripts/check.sh all`. Once installed,
schema validation resolves only repository-local files and needs no network.

## Target matrix

| Product | PlatformIO environment | Framework source | Flash contract | Output |
| --- | --- | --- | --- | --- |
| MagBound Smart Timer V1 (Legacy Timer) | `magbound_v1_esp8266` | `espressif8266@4.2.1` / Arduino-ESP8266 3.1.2 | ESP8266, DOUT, 4 MB, 40 MHz, `4M/2M OTA` | `OpenSmartTimer-<version>-MagBoundV1-ESP8266.bin` |
| MagBound Smart Timer V2 | `magbound_v2_esp32c3` | pinned `ost-esp32c3-v2` package | ESP32-C3, DIO, 4 MB, 80 MHz, default A/B OTA | `OpenSmartTimer-<version>-MagBoundV2-ESP32C3.bin` |

`scripts/build_firmware.sh` assigns separate PlatformIO core and build
directories to the targets. The V2 framework override in `platformio.ini`
exists only inside the V2 environment.

## Versioning

The single `firmware_version` value in `platformio.ini` controls both embedded
version strings and both artifact names. Firmware and relay share that release
version. Published tags use `v<version>` and must match the versioned changelog
section.

The REST contract is `/api/v1`, Bluetooth is `ost-ble/1` and the relay is
`ost-relay/1`. Hardware V1/V2 and the SDK compiler profile revision are
independent of release and protocol versions.

## Build only V1

V1 does not require Docker or the custom ESP32-C3 framework:

```sh
./scripts/build_firmware.sh v1
```

The application is written beneath:

```text
.pio/build/v1/magbound_v1_esp8266/
```

## Build V2

The default V2 build includes relay support. Hosting addresses are owner
configuration stored at runtime after local review and physical confirmation.
No hosting domain or certificate is needed for an application build.
Use `OPENSMARTTIMER_RELAY=disabled` to omit relay support, as shown below. This override changes the image's build ID.

Generate or verify the locked V2 framework, then build its application:

```sh
./scripts/build_framework_esp32c3.sh
./scripts/build_firmware.sh v2
```

The framework archive in `dist/framework/OpenSmartTimer-ESP32C3-Framework-ost2.tar.gz`
is a compiler input and **must never be flashed to a timer**. An existing
verified archive can be installed instead of rebuilt:

```sh
python3 scripts/framework/install_framework_esp32c3.py \
  dist/framework/OpenSmartTimer-ESP32C3-Framework-ost2.tar.gz
python3 scripts/framework/verify_framework_esp32c3.py
./scripts/build_firmware.sh v2
```

The V2 application is written beneath:

```text
.pio/build/v2/magbound_v2_esp32c3/
```

### Relay build option

The V2 image includes the experimental remote relay by default. One line in
`platformio.ini` removes it completely: its code, the `/api/v1/relay` API,
the `features.relay` capability and the web page's Remote sharing section
(the page hides it when the API is absent).

```ini
[opensmarttimer]
relay = enabled   ; or: disabled
```

For a single build, override it without editing the file:

```sh
OPENSMARTTIMER_RELAY=disabled ./scripts/build_firmware.sh v2
```

Enabled relay builds use the same image for all deployments. The operator's
setup code supplies the relay and independent control-page addresses, code and
pin. The owner reviews both addresses in the local UI and briefly presses
Reset to approve. Relay responses cannot replace the approved page origin.

The build prints `OpenSmartTimer experimental relay: enabled|disabled`, the
setting is part of the embedded build ID, and
`scripts/checks/check_relay_boundary_images.sh` confirms the image matches it
(relay-free when disabled). V1 never includes the relay. Relay settings a
timer saved under an enabled build stay in flash, unused, until a factory
reset or a later enabled build's Forget.

## Build and verify everything

For day-to-day source work, use the shared local/CI runner:

```sh
./scripts/check.sh all
```

This covers host, protocol, browser behavior and loopback preview integration
tests without SDK downloads or hardware access. `host`, `web` and `integration`
select individual groups. See the [script map](../scripts/README.md) and
[test inventory](../tests/README.md); manual preview/hardware tools are listed
[separately](../tools/README.md).

For an ordinary dual-target build, run:

```sh
./scripts/build_framework_esp32c3.sh
./scripts/build_firmware.sh all
python3 scripts/release/verify_release.py
```

For the full local release gate (including exact-image qualification):

```sh
./scripts/check_release.sh
```

That command runs both controller variants, the V2 BLE command parser,
UI/API checks, the REST/SSE and V2 Bluetooth preview integration tests,
relay source/Chromium/HTTPS tests, framework
verification, both firmware
builds, final image validation, canonical artifact staging and a check of
the staged bytes against the qualification record (every check for a stable
version, the release-candidate rules for a prerelease). It does not
upload to a device.

The gate compares a local build with the qualification record, which names the
images CI built. A V2 image built on macOS never matches those bytes (see
[Reproducibility](#reproducibility)), so on macOS the gate's last step fails
for V2; run it on Linux x86-64, or check the downloaded CI images with
`scripts/release/verify_qualification.py --release-candidate --allow-pending --images <dir>`.

After it passes, use only the generated files beneath:

```text
dist/
├── firmware/
│   ├── OpenSmartTimer-<version>-MagBoundV1-ESP8266.bin
│   ├── OpenSmartTimer-<version>-MagBoundV1-ESP8266.elf
│   ├── OpenSmartTimer-<version>-MagBoundV2-ESP32C3.bin
│   ├── OpenSmartTimer-<version>-MagBoundV2-ESP32C3.elf
│   ├── BUILD.json
│   ├── CRC32SUMS
│   └── SHA256SUMS
└── framework/
    └── OpenSmartTimer-ESP32C3-Framework-ost2.tar.gz
```

`.pio/` contains build intermediates. Root-level files under `dist/` are not
part of the artifact contract. `dist/firmware/BUILD.json` records the current
version, size, CRC32, SHA-256, model, and qualification level. The framework
archive is a compiler input and must never be flashed. Each artifact record also
contains the 24-hex `build_id` embedded in that image. It fingerprints the
application/UI source and target configuration; `sha256` remains the exact
artifact-integrity value.

Each `.elf` holds the symbols for its image, so a crash address from that
build can be decoded (`addr2line -pfiaC -e <image>.elf <address>` with the
target's toolchain). It is never flashed; `BUILD.json` records its size and
SHA-256 under `symbols`, and it is excluded from `SHA256SUMS`/`CRC32SUMS` and
the Pages flasher. The V2 framework keeps assertions but drops their text, so
the `.elf` is how a V2 crash is located.

CI adds the full Git source commit and repository to `BUILD.json`. A local
package made without those explicit inputs records `source: null`, not an
invented or potentially stale commit. Release preparation refuses missing or
mismatched source identity. Supplying a commit to the packager is a provenance
claim by the build process, not a cryptographic attestation.

The V2 framework retains peripheral/server and central/observer/client BLE
roles, with capacity for two simultaneous links (one app plus one future timer
peer). The application exposes only one inbound app connection and
does not scan for, connect to or control another timer. HTTPS client, certificate
bundle and OTA libraries are also retained. Manual GitHub updates use
browser downloads and device-side signature/hash verification; no device-side
GitHub downloader or automatic check is enabled. See the
[framework capability boundary](../framework/esp32c3/README.md#future-capabilities-versus-exposed-features).
The V1 build uses its independent ESP8266 framework and never links the V2 BLE
stack or protocol implementation.

## Which files are flashable

Flash only a filename containing the exact model and ending in `.bin`:

```text
OpenSmartTimer-<version>-MagBoundV1-ESP8266.bin
OpenSmartTimer-<version>-MagBoundV2-ESP32C3.bin
```

Use the device updater's **Firmware** field. Do not flash the framework
`.tar.gz`, `bootloader.bin`, `partitions.bin`, a filesystem image, an ELF, or a
merged image.
The release verifier rejects cross-target application headers,
incorrect flash contracts, oversized images, missing model markers, and an
unexpected V2 partition table.

## Reproducibility

`framework/esp32c3/lock.json` pins the upstream commits, builder-container
digest, resolved configuration/dependency hashes, package version, timestamp,
and expected framework archive hash. The V2 application build uses that locked
timestamp as `SOURCE_DATE_EPOCH` to stabilize build timestamps.

GCC records the compile directory and its own header paths in debug
information. `scripts/build/build_paths.py` rewrites the checkout, PlatformIO
and home-directory prefixes to fixed names in both builds, so neither the
images nor the published `.elf` symbol files reveal or depend on where they
were built (the V2 image embeds its ELF's SHA-256). `package_firmware.py`
refuses to stage a file that still contains the checkout or home path. The
paths left in the symbols come from upstream prebuilt libraries.

Two checkouts at different paths build identical bytes, and a V1 image built
on macOS matches the one CI builds on Linux. A V2 image does not: Espressif's
macOS and Linux toolchain packages record different build paths for their
prebuilt libraries in the debug information, and the V2 image embeds its ELF's
SHA-256, so it matches only a build on CI's host platform (Linux x86-64). Its
code and data are identical either way. Verify the actual SHA-256 rather than
assuming that a matching version or build ID implies identical bytes.

Run `./scripts/build_framework_esp32c3.sh --rebuild` from two clean checkouts
when intentionally changing the framework profile. Do not update its locked
hashes until the clean outputs match.

Firmware binaries and symbols are distributed as release assets. The
[qualification format](qualification/README.md) binds reported checks to exact
image hashes and source identity. [Testing and verification](VALIDATION.md)
covers source checks and physical-device limits.

## GitHub Actions boundary

`.github/workflows/ci.yml` runs four groups:

1. host controller, UI, API, and preview integration tests;
2. generation or verification of the locked V2 framework package;
3. relay Python/Node/C++ checks, actual operator pages in Chromium, and the
   disposable HTTPS/partner browser stack;
4. isolated V1/V2 builds followed by release-image verification.

Image verification includes the exact permitted BLE writable characteristics
and management authentication/dispatch/OTA guardrails. It is not a blanket
ban on management writes and does not replace physical BLE/OTA qualification.

The firmware job depends on the source, relay and framework jobs. Failed tests
prevent firmware artifact creation. Branch/PR CI can run without the baseline
records; the tagged release workflow validates the candidate's qualification
record before calling CI, and its draft job refuses images that differ from
the recorded ones. A stable tag needs every check passed. A prerelease may
have pending checks, but its physical emergency check must have passed and
none may have failed; whether to publish one is the maintainer's decision.

Actions use immutable commit pins. Python and browser test dependencies are
hash-locked or npm-lockfile constrained. Dependency changes require regenerated
locks and the same local gates; Dependabot proposes Actions/npm updates.

Successful branch/PR/manual CI uploads `dist/firmware/`, a separate
`dist/source-materials/` bundle and a stamped web-app preview. It does not
publish a release or deploy Pages. Version tags
use the same CI as a reusable workflow; they do not run a second independent
firmware build. The [Pages workflow](GITHUB_PAGES.md) deploys an allowlisted app
after publication, with one signed firmware feed per timer model. The
companion follows the same newest published version tag. See [GitHub updates](GITHUB_UPDATES.md) for public repository/key
configuration and the `UPDATE_SIGNING_KEY` Actions secret required before
drafting a release.

## Release artifacts

The version-tag workflow validates source metadata and invokes the same CI
builds as branch/PR checks. It creates an unpublished draft using artifacts
from that run. Publication is a maintainer action; successful builds and a
draft do not establish physical qualification.

Release tags and deployment credentials should be protected. Firmware builds
need no hosting domain: users configure relay hosting at runtime. Builds have
read-only repository permissions; the draft job has release-write permission,
and Pages deployment has only the additional Pages/OIDC permissions it needs.
An existing release is not overwritten on rerun.

Firmware packages include:

- one V1 `.bin` and one V2 `.bin` application image, each with its `.elf`
  symbols for decoding crash addresses (not flashable);
- `SHA256SUMS` and `CRC32SUMS` for those exact binary files;
- `BUILD.json` with sizes, model/chip, source commit, embedded build IDs and
  the deliberately conservative `software-verified-only` qualification label;
- `release.json`, a versioned machine-readable manifest with explicit target
  names, version/tag, source commit, sizes, checksums and per-image download URLs.
- `updates.json`, signed model-specific descriptors used by the manual GitHub
  updater and verified Pages feed staging;
- a source-materials archive and `SOURCE-SHA256SUMS`, containing the reviewed
  source, actual Arduino source packages, pinned upstream SDK sources,
  collected notices and the verified C3 compiler input. See
  [third-party notices](../THIRD_PARTY_NOTICES.md).

## Source and relinking materials

After committing reviewed source and building both targets and the framework,
generate a separate source bundle locally:

```sh
python3 scripts/release/package_source_materials.py
```

This downloads only the checksum-pinned upstream archives. For an offline
repackage, pass `--offline --cache /path/to/verified-source-cache`. The tool
requires a clean source commit and refuses altered dependency inputs. Extract
the bundle's `project.tar` before following this build guide. Arduino source
snapshots are provided for modification and relinking. The vendor C3 archive
can be installed using the framework command above. None of these source
materials are flashable. Locally built firmware can use manual file upload;
see [Installation](INSTALLATION.md).

To relink a modified Arduino core, first run the appropriate target build to
install its pinned package, then overlay the supplied source snapshot onto it.
From the extracted project's directory (with `materials` set to the extracted
source-materials directory), use the matching target:

```sh
# After ./scripts/build_firmware.sh v1 has installed the V1 package:
cp -R "$materials/arduino/arduino-esp8266-3.1.2/." .pio-core/v1/packages/framework-arduinoespressif8266/
./scripts/build_firmware.sh v1

# After the framework installation and first V2 build:
cp -R "$materials/arduino/arduino-esp32-3.3.0/." .pio-core/v2/packages/framework-arduinoespressif32/
./scripts/build_firmware.sh v2
```

Edit the supplied sources before overlaying them. Keep the installed package's
SDK binaries and package metadata; the snapshots contain source/build files
and notices rather than a replacement for the entire downloaded SDK package.

The versioned changelog section supplies the release description. GitHub's
auto-generated commit list is not substituted for human-readable change notes.
The manifest keeps `software-verified-only` after publication: record manual
qualification separately, without silently altering tested package metadata.

### Checksums and signed GitHub OTA

CRC32 is **CRC-32/ISO-HDLC**, encoded as eight lowercase hexadecimal digits,
over the complete raw `.bin` bytes (not a ZIP or HTTP envelope). It matches the
current uploader's integrity check. SHA-256 is 64 lowercase hexadecimal digits
over those same bytes and is the exact-artifact reference for qualification.

Neither checksum alone authenticates the publisher. `release.json` remains
unsigned provenance/checksum metadata; the separate `updates.json` descriptors
bind target, version, size, CRC32 and SHA-256 with P-256 signatures. Both
models use manual browser-assisted HTTPS downloads. Firmware verifies the
signature before the writer starts and the streaming SHA-256 before commit,
while retaining the physical arm and product/header checks. Update checks run in the browser, from the Firmware section or once a day
when a page opens; automatic installation and device-side downloads are not
implemented. See
[GitHub update configuration](GITHUB_UPDATES.md).
