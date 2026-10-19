# Relay development checks

These checks operate only on `relay/` and temporary test resources. They do not
build, flash or modify the normal smart timer firmware.

## Source gate

From the repository root:

```sh
./relay/tests/check.sh
```

Requires the relay Python package/development test dependencies, Node and a
C++17 compiler, **OpenSSL 3 development headers/libraries and pkg-config**.
CI uses Python 3.12 and Node 24; newer local runtimes are recorded with results.
The runner discovers OpenSSL with `pkg-config --cflags --libs openssl`; it fails
with setup guidance if unavailable and never falls back to fake signing. The script chooses `relay/.venv/bin/python` when available;
set `PYTHON`, `NODE` or `CXX` for another runtime. It runs pytest, Node browser
logic tests and compiles/runs the actual C++ capability/parser/client host
programs with warnings as errors.
`timer_authority_control_test` also compiles the relay timer adapter against the
root `TimerController.cpp` and `TimerCommandAuthority.cpp` (V2 host stubs from
`tests/host/`), proving relay commands use the firmware's own rules. Binaries are placed in a temporary directory.
The host OpenSSL provider signs real ECDSA P-256 (RFC 6979, low-S). Tests check RFC 6979 vectors,
provider failure, and both directions of Python/C++ signature verification.
The actual C++ client runs through the real Python session/command/database
code using a test-only JSON-lines transport bridge; Node runs the actual WebCrypto handshake and channel against that C++
endpoint and loads the SPA parser for authenticated envelopes. Identity forgery
with a known link secret, missing/high-S signatures, full transcript binding and
AEAD tampering are rejected. A provider-call counter proves one signature per
channel, cached-accept reuse and zero ECDSA signing for routine responses. This bridge does not implement ESP networking.

To run just the cross-language checks:

```sh
relay/.venv/bin/python -m pytest relay/tests/test_cpp_interop.py -q
```

The bridge has deterministic test keys and no network listener. Generated
bearer tokens/fixtures stay in temporary directories, not deployment artifacts.

## HTTPS stack and real browser

The native partner reconnect/control suite uses real endpoint cryptography:

```sh
node relay/tests/watch_reconnect_browser.cjs
node relay/tests/watch_reconnect_browser.cjs --browser=webkit
```

It accepts `--browser=chromium|webkit`; the existing `OSTRELAY_BROWSER`
environment setting remains supported. The explicit option takes precedence.

The suite discards idle device traffic keys while leaving the capability valid,
then checks recovery by Retry, tab return and the automatic heartbeat. It also
checks a failed handshake followed by restored connectivity, fresh status
verification, preserved duration drafts and absence of control replay.

`./relay/tests/browser.sh` runs every browser suite in this section in the
same order as CI and the full release gate; the individual commands below are
for working on one of them.

For the landing, setup and admin pages alone (no Docker or timer needed):

```sh
node relay/tests/pages_browser.cjs
# Optional screenshots (run from the repository root):
OSTRELAY_PAGES_SCREENSHOTS=tmp/relay-ui-review node relay/tests/pages_browser.cjs
```

This launches Chromium and a temporary loopback relay with an in-memory
database. It never reads your `.env` or connects to a timer. It checks admin
sign-in, owner invitation, sign-out, saved landing-page visibility, the private
preview, mobile/desktop layouts and theme-switch geometry in light/dark mode.
The Python source gate separately checks restart persistence, authentication,
cross-origin rejection and the environment override. Playwright/Chromium and
the relay development dependencies must already be installed, as below.

For the full synthetic HTTPS/partner-control flow:

```sh
./relay/tests/relay_e2e.sh --browser
```

Requires Docker Compose, Python test dependencies, Node, Playwright and a
Chromium installation. From the repository root, install the locked browser
dependency once:

```sh
cd relay/tests
npm ci
npx playwright install chromium
cd ../..
```

No package install is run by the test itself. CI uses the same lockfile and
installs Chromium with `--with-deps` on Linux. Omit `--browser` for the Python
transport check alone.

The separate `node relay/tests/ota_browser.cjs` gate exercises the real embedded
and Bluetooth dialog callbacks with disposable signed feeds and mocked device
adapters: no releases, notes, Cancel, signed Install, physical-arm recheck and
320–1280px layouts. It never flashes hardware.

The runner creates a unique `ostrelay-check-*` Compose project, generated
short-lived local CA/certificate, random admin/cookie/enrollment credentials
and its own named volume. It never reads `relay/.env`. Only synthetic loopback
timers are used. Cleanup deletes only that test project's resources. Docker
must be allowed to bind localhost ports; the browser must be allowed to launch.

The Python HTTPS and WSS clients verify the generated CA and actual SPKI. Only
the headless browser ignores the generated localhost CA error. This browser
exception is restricted to this synthetic test and is not a product setting.

The test checks:

- Separate trusted-page/API HTTPS origins, exact CSP/CORS and relay `/s` refusal.
- Three authenticated mock timers and six SSE viewers with real ECDSA P-256.
- Authenticated ephemeral P-256 ECDH, opaque GCM routing, exact-ID dedup and encrypted device result lookup.
- Public routing refresh, per-client binding, token/MAC mutations and wrong-pin rejection.
- Same-boot reconnect, targeted revoke and logout terminating active streams.
- Chromium 320px/390px/1280px responsive layouts, light/dark themes, inline
  duration setup and start/adjust/pause/resume/release, lost HTTP response,
  refresh and result reconciliation without an extra adjustment.
- Actual Chromium parsing of authenticated C++ response fixtures. The separate
  WebCrypto/C++ crypto gate verifies timer identity and rejects altered GCM
  bytes/sequences; the fixture parser alone makes no cryptographic claim.
- Creator form disabled offline and enabled after connection recovery.
- Creator-selected 15-minute full-control link, lost creation reply plus page
  reload and idempotent retry, lost token ACK, confirmed copy and revoke ending
  the partner session. Creator screenshots mask the bearer-link field.
- Health after load, non-root application runtime, online SQLite backup,
  integrity-checked restore into a new DB and server restart on that DB.

The test writes ignored `tmp/relay-tests/stack-latest.json` and, with `--browser`,
`tmp/relay-tests/partner-smoke.png` plus setup, desktop and light-theme views,
and `creator-*.png` screenshots. Latency covers the Python command loop;
container memory/CPU is a sample **after** client cleanup, not a peak-load
measurement. `--no-build` reuses the local test image and is only appropriate
if no image input has changed. `--report /path/report.json` selects another
report file.

This short loopback test is not a hardware qualification, throughput benchmark,
long soak, public certificate deployment or cross-browser/mobile-device matrix.
CI runs both the operator-page browser check and this full isolated stack
before accepting firmware artifacts. These gates do not replace hardware tests.
