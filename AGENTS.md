# Working on OpenSmartTimer

Orientation for anyone changing this repository, human or coding agent.
[CONTRIBUTING.md](CONTRIBUTING.md) covers licensing and review; this file is
the practical map: what to run, what must stay in sync, and what not to touch.

This firmware drives a physical lock that can be used with human restraints.
Read [SAFETY.md](SAFETY.md). When a change could affect whether the lock
releases, prefer the option that fails open, and say so in the review.

## Layout

| Path | What it is |
| --- | --- |
| `src/`, `include/` | Firmware for both models. Timer rules live in `TimerController` and `TimerCommandAuthority` |
| `src/relay/`, `include/relay/`, `src/WebRelay.cpp` | V2-only relay client |
| `web/` | Device-hosted page, embedded into both firmware images |
| `bluetooth-app/` | V2 Web Bluetooth app; loads `../web/styles.css`, `i18n.js`, `appearance.js` and `updates.js` |
| `pages/` | GitHub Pages home page; not built into firmware, so its Chinese text lives in `pages/home.js` |
| `relay/src/ostrelay/` | Relay server (FastAPI, SQLite) |
| `relay/spa/` | Partner control page, hosted separately from the relay |
| `tests/` | Host C++ (`host/`), Python (`protocol/`, `integration/`) and Node (`web/`) tests |
| `relay/tests/` | Relay pytest, Node, C++ interoperability and browser suites |
| `scripts/` | Build, check and release entry points |
| `tools/` | UI preview and hardware utilities |
| `docs/` | Guides and protocol references; start at [docs/README.md](docs/README.md) |

## Commands

Run from the repository root. Setup details are in
[tests/README.md](tests/README.md) and [relay/tests/README.md](relay/tests/README.md).

```sh
# Firmware, web and API source tests (needs requirements-dev.txt in the venv)
./scripts/check.sh all --python=tmp/check-venv/bin/python
./scripts/check.sh host        # or: web, integration

# Relay server, partner page and relay firmware modules on the host
./relay/tests/check.sh

# Every browser suite, Chromium and WebKit; the list CI and the release gate use
./relay/tests/browser.sh

# Device page without hardware (mock API; OTA disabled)
python3 tools/preview_ui.py --v2 --port 8766

# Firmware images and the full release gate
./scripts/build_firmware.sh all
./scripts/check_release.sh
```

None of these flash a timer or contact a real one.

CI runs on Ubuntu with GCC, Python 3.12 and Node 24. Apple clang and libc++
accept code that GCC with `-Werror` and libstdc++ reject (`memset` over a
struct with default member initializers, a missing `<algorithm>`, statements
sharing a line with an `if`). A green local run on macOS is not proof CI will
pass; watch the first CI run after touching C++.

## Rules that are easy to break

**One timer authority.** HTTP, Bluetooth and relay handlers request changes
through the shared controller. A transport callback never drives lock GPIOs,
and a remote command can never override the emergency input, a controls lock,
duration limits or the boot/session checks.

**Relay stays V2-only and isolated.** `SMART_TIMER_RELAY` may appear only in the
wiring points listed in `scripts/checks/check_repository.py`. Put relay
behavior in `src/relay/`. V1 has no Bluetooth and no relay.

**Flash space is tight on V2.** A release image must leave 30 KiB of its OTA
slot free (`scripts/release/verify_release.py`), and V2 currently has under
70 KB free. `web/` is compressed into both images, so UI text and CSS cost
flash. Check the size line from `verify_release.py` after firmware or web
changes.

**RAM is shared between Bluetooth and the relay on V2.** The relay's TLS
connection needs 40 KB of free heap and a 20 KB block, and Bluetooth holds about
51 KB while its radio is on. The Bluetooth settings in
`framework/esp32c3/defconfig.opensmarttimer` are sized for that; check free heap
on a timer with both active before raising a pool, a connection count or an
SSE/event limit.

**Local HTTP is unauthenticated by design.** The LAN is the trust boundary; see
[SECURITY.md](SECURITY.md). Do not add partial authentication that implies
otherwise. Relay and Bluetooth do authenticate, and their rejection cases have
tests that must keep passing.

**Hardware tools energize outputs.** `tools/validate_bench.py`,
`tools/validate_soak.py` and `relay/tools/hardware/` act on a real timer. Run
them only when a person asks, on an unloaded bench unit.

## Things that must change together

Tests enforce most of these; the list saves a failed run.

- **Shared web files.** `relay/spa/timer-ui.css`, `relay/spa/i18n.js` and
  `relay/spa/appearance.js` are byte-identical copies of `web/styles.css`,
  `web/i18n.js` and `web/appearance.js`. Edit the `web/` file, then copy it.
- **Translations.** `web/i18n.js` is keyed by the exact English string. Any
  visible text added to `web/`, `bluetooth-app/` or `relay/spa/` needs a
  Simplified Chinese entry, and a reworded string needs its key updated.
  Remove keys that no longer have a caller. Strings used only by the relay's
  landing, setup and admin pages go in `relay/src/ostrelay/page_strings.py`
  instead, so they do not take up timer flash; text built in their scripts
  must go through `t()` or `count()`.
- **Update panel.** `web/updates.js` serves the device page and the Bluetooth
  app. Its status strings are quoted in `tests/web/updates_test.mjs` and
  `relay/tests/ota_browser.cjs`.
- **Version.** `firmware_version` in `platformio.ini` is the source of truth.
  A bump also changes `relay/pyproject.toml`, `relay/src/ostrelay/__init__.py`,
  the image tags in `relay/docker-compose.yml` and `relay/tests/compose.yml`,
  and needs a `## [version]` section in `CHANGELOG.md`.
- **Protocols.** Firmware, relay server and partner page implement
  `ost-relay/1` together; firmware and the Bluetooth clients implement
  `ost-ble/1`. Change the reference in `docs/`, every implementation and the
  rejection tests in one change.
- **Local API.** A new or changed endpoint needs `docs/API.md`, a schema under
  `docs/schemas/` and a route mapping in `tests/integration/api_contract.py`.
- **New tests.** A new `tests/host/*_test.cpp` must be registered in
  `scripts/check.sh` (unknown files fail the run). A new relay browser suite
  goes in `relay/tests/browser.sh`. Python and `*_test.mjs` files are found
  automatically.
- **Lockfiles.** Regenerate with the command in the header of each `.in` file.
  Keep `--universal --python-version 3.12` on `requirements-dev.txt`: a lock
  compiled on a newer Python drops packages CI needs.
- **Workflows.** Actions are pinned to commit SHAs; `tests/protocol/repository_contract_test.py`
  checks pins, permissions and job order.

## UI changes

Use the preview server and look at the result before calling it done:

- widths 320, 390 and 1280; light and dark; English and Simplified Chinese;
- every state the element can show, including errors and disabled controls;
- no horizontal page scroll and no button text wrapping.

The timer page (`web/`) is the design reference. The Bluetooth app and partner
page share its stylesheet, so check them when changing shared classes. The
relay's operator, owner, setup and landing pages add their own rules in
`PAGES_CSS` (`relay/src/ostrelay/admin_page.py`); keep those on the timer page's
cards, headings, buttons and switches rather than inventing new ones. The
example images in `images/screenshots/` come from the preview and test
fixtures with example data only. Run `./scripts/check.sh web` and
`node relay/tests/ota_browser.cjs` after touching the update panel.

## Releases

1. Bump the version in the files listed above and write the changelog section
   for timer owners (see below).
2. Push the release source (a branch is enough) and let CI build it. A person
   installs that run's `opensmarttimer-application-images` on the bench units,
   qualifies them and commits the record
   ([docs/qualification/](docs/qualification/README.md)). Docs are not build
   inputs, so the record commit builds the same bytes.
3. Push a `v<version>` tag. `release.yml` checks the tag against
   `platformio.ini`, the changelog and the record, reruns CI, refuses images
   that differ in any byte from the recorded ones, signs the two images with
   the `UPDATE_SIGNING_KEY` secret and creates an unpublished **draft**.
4. A person reviews the draft and publishes it in the GitHub UI.
5. Publishing triggers `pages.yml`, which verifies the published files and
   deploys the Bluetooth app and the signed update feed.

**The changelog is written for timer owners.** A version's `CHANGELOG.md`
section is shown as-is in the update dialog on the timer page and in the
Bluetooth app (up to 8 KB), and on the GitHub release page, at the moment
someone decides whether to install. Write each entry as what changes for the
person using the timer, in plain language:

- Say what they will notice and why it matters ("V2 now opens on time even if
  its page is slow to answer"), not how it was built.
- Leave out API paths, JSON field names, diagnostics keys, function names and
  internal mechanisms; those belong in `docs/API.md` and the other guides.
  Name a setting or button exactly as the interface shows it.
- Mention integrations or diagnostics only when an owner would act on them,
  in one plain sentence.
- Keep a **Known limits** entry for anything an owner might run into, with
  what to do about it.

Every published release with a valid version tag, prereleases included, is
offered to every timer of that model. There are no update channels.
[docs/GITHUB_UPDATES.md](docs/GITHUB_UPDATES.md) and
[docs/GITHUB_PAGES.md](docs/GITHUB_PAGES.md) have the one-time repository
settings.

## Do not commit

Signing keys, relay `.env` files, databases, live share links, setup codes,
vendor firmware or manuals, and anything under `tmp/`. Test fixtures use
disposable keys and neutral names such as `example-owner`. Built `.bin` and
`.elf` files are release assets, not source.
