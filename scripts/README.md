# Build and check entry points

Run these from the repository root. They also resolve their own root when
invoked by absolute path from another directory.

Source API checks require the root `requirements-dev.txt` in your Python
environment. See [test setup](../tests/README.md); `check.sh` uses `python3`
by default. Select another interpreter with
`./scripts/check.sh all --python=/path/to/venv/bin/python` or the existing
`PYTHON=/path/to/venv/bin/python` environment setting. The explicit option
takes precedence.

| Task | Command |
| --- | --- |
| All source tests, no SDK required | `./scripts/check.sh all` |
| One source-test group | `./scripts/check.sh host`, `web`, or `integration` |
| V1, V2, or both firmware builds | `./scripts/build_firmware.sh v1`, `v2`, or `all` |
| Build/verify the pinned V2 SDK package | `./scripts/build_framework_esp32c3.sh` |
| Complete release gate and artifact staging | `./scripts/check_release.sh` |

The full release gate requires complete qualification records matching its
exact staged bytes. Source validation can accept structurally valid partial
records without claiming physical qualification. Binaries are distributed as
release assets rather than committed into the repository.
Neither build command nor check command flashes hardware.

## Helper directories

| Directory | Responsibility |
| --- | --- |
| `build/` | PlatformIO hooks (image name, compressed UI, source build ID) and the standalone Pages packager |
| `checks/` | Source-only UI, app and API-contract checks |
| `framework/` | Finalize/install/package/verify the locked V2 compiler input |
| `release/` | Image/qualification verification, application packaging, tag/changelog checks and exact release metadata |

The PlatformIO hooks in `build/` run inside PlatformIO, not as standalone
Python scripts. `build/build_pages.py` is the exception: it stages the static
website independently of either firmware build; see [Pages deployment](../docs/GITHUB_PAGES.md).
Framework helpers are independent of V1.

For an already-built pair of images:

```sh
python3 scripts/release/verify_release.py
python3 scripts/release/package_firmware.py
python3 scripts/release/package_source_materials.py
```

Packaging replaces the disposable `dist/firmware/` output directory. Preserve
any artifacts you need before staging another build. Store private inputs and
retained image copies outside this disposable directory.

Source packaging requires a clean reviewed Git commit, both installed Arduino
packages and the verified V2 framework archive. It stages a separate
`dist/source-materials/` bundle with collected notices, actual Arduino sources
and hash-pinned upstream sources. It excludes ignored bench data and private
keys. See [third-party notices](../THIRD_PARTY_NOTICES.md).

`release/release_metadata.py check --tag v<version>` validates the tag against
`platformio.ini` and requires a matching `CHANGELOG.md` section. Its `prepare`
mode verifies a CI package before adding `release.json`; `verify` checks an
existing published package against its tag/source/actual files. None of these
commands builds, signs, flashes or publishes firmware. The tag workflow uses
`prepare` and `gh release create --draft`; only a human publishes it. See the
[release artifact contract](../docs/BUILDING.md#release-artifacts).

Preview and hardware utilities are documented in [tools](../tools/README.md).
Relay utilities have [their own guide](../relay/tools/README.md).
The [build guide](../docs/BUILDING.md) covers dependencies, CI and reproducibility.
