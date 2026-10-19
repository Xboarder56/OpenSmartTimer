# Firmware and app regression tests

Use `./scripts/check.sh all` from the repository root. CI invokes the same
runner so the local command and CI cannot silently diverge. No test here
connects to a physical timer.

| Folder | What it exercises | Group |
| --- | --- | --- |
| `host/` | Actual C++ controller, GPIO assertions, HTTP parser, BLE protocol/pairing policy, bounded SSE writes | `host` |
| `protocol/` | BLE vectors, board/transport guardrails, Pages packaging and release-metadata rejection tests | `host` |
| `vectors/` | Checked-in BLE protocol fixtures, not live keys | Fixture data |
| `web/` | Shared BLE transport plus real app functions with injected failures | `web` |
| `integration/` | Mock HTTP server and UI API contracts for V1/V2 | `integration` |

`host/Arduino.h` and `host/driver/gpio.h` are deliberate host-only stubs. They
must not enter the firmware include path. Host executables go under
`.pio/host-checks/`; source files and repository root stay free of build output.

Dependencies: C++17 compiler, Python 3.12 or newer and Node.js 24 or newer, plus
the hash-locked packages in root `requirements-dev.txt`. Set up once:

```sh
python3 -m venv tmp/check-venv
tmp/check-venv/bin/python -m pip install --require-hashes -r requirements-dev.txt
./scripts/check.sh all --python=tmp/check-venv/bin/python
```

An activated environment works with the ordinary `./scripts/check.sh all`
command. The runner never installs dependencies or skips a missing validator.
Integration tests bind temporary loopback ports and require permission to do so. They do
not contact a timer or the Internet. The runner suppresses Python bytecode
cache generation. No PlatformIO SDK is needed for these source tests.

New host test files are detected but require explicit dependency registration
in `scripts/check.sh`; unknown tests fail loudly instead of being skipped.
Python protocol/integration tests and browser `*_test.mjs` files are discovered
within their respective folders.

Physical-device verification remains separate; see
[Testing and verification](../docs/VALIDATION.md) and the
[hardware utilities](../tools/README.md). Relay tests are documented
[separately](../relay/tests/README.md); server tests do not prove hardware behavior.

## API contract checks

`integration/api_contract.py` shares an explicit method/path-to-schema map
across the V1/V2 preview tests. Successes, errors, preconditions, duplicate
mutations and SSE status are validated with Draft 2020-12. Accepted JSON
requests are also checked where a request schema exists. Unknown successful
test routes fail until a mapping is added. `schema_contract_test.py` checks
schema definitions/references and compatibility regressions, including
Bluetooth-only state, optional/null IDs, extensible errors and AP settings.

All schema references resolve from the repository without network access.
These tests validate mocks and clearly labeled synthetic examples; they do
not establish what a physical firmware image emitted. Validate a saved capture
locally with the environment above:

```sh
tmp/check-venv/bin/python tests/integration/api_contract.py status /path/to/reply.json
```

See [schema coverage and limits](../docs/schemas/local/README.md#validation-and-coverage-limits).
