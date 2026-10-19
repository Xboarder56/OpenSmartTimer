# Contributing

Original project code uses GNU AGPL version 3 only with the additional
[Espressif linking permission](LICENSE-ESPRESSIF-EXCEPTION). Contributions to
original project code are offered under those same terms. Preserve third-party
license notices and distinguish upstream code from project changes.

Read [SAFETY.md](SAFETY.md) and [the architecture](docs/ARCHITECTURE.md) before
changing controller behavior. Use unloaded bench units for hardware tests.
Keep REST, Bluetooth and relay mutations behind the shared timer authority;
transport callbacks must not drive the lock GPIOs directly.

## Development checks

[AGENTS.md](AGENTS.md) is the practical map for people and coding agents: the
commands, the files that must change together and the rules that are easy to
break.

Follow [test setup](tests/README.md) for the hash-locked Python environment,
then run `./scripts/check.sh all`. Relay changes also require
`./relay/tests/check.sh`, the operator-page Chromium check and the isolated
HTTPS stack; [relay test setup](relay/tests/README.md) covers their locked
dependencies. Tests use disposable credentials and never contact a real timer.

Build both targets with the pinned frameworks using
[the build guide](docs/BUILDING.md). Verify image size and the V1/V2 relay
boundary after firmware changes. Source checks are separate from
[exact-image hardware qualification](docs/qualification/README.md).

## Changes for review

Explain the concrete behavior changed and how it was tested. Add regression
coverage for a reproduced bug or changed protocol boundary. Keep protocol
references, clients and rejection cases consistent. Hardware V1/V2 names do
not imply different public API revisions.

Put private captures, experiments and generated reports under ignored `tmp/`.
Update maintained guides for current behavior; do not add dated handoff notes
to the public tree. Never commit live credentials, share links, signing keys,
databases or vendor firmware/manuals. Test fixtures use disposable keys.

Dependency updates must regenerate their lockfiles and pass the same gates.
Actions are pinned to immutable commits; Dependabot proposes pin updates.
Build and packaging changes must preserve the documented
[release artifact contract](docs/BUILDING.md#release-artifacts).
Report security issues through [the security policy](SECURITY.md).
