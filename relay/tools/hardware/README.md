# Relay hardware verification tools

Scripts that qualify the experimental relay on a real **V2 timer** against a
real relay. Bench use only: they start, pause and release the timer, so never
run them with anyone attached. They print no share links or keys; outputs
(including share links, which are bearer secrets) go to `out/`, which is
git-ignored.

## Setup

- A V2 timer on the bench network, enrolled with a relay you run (see
  [OPERATOR.md](../../OPERATOR.md)).
- Node.js with the pinned browser tools installed: from `relay/tests`, run
  `npm ci` and `npx playwright install chromium`.
- For the Bluetooth suites: a Python with `bleak`, and a Bluetooth app key
  file approved on the timer.

The Bluetooth-priority suite temporarily enables a disabled radio for its test
and restores the original preference afterward. Existing approvals are retained.

```sh
export OST_TIMER=http://192.168.4.1                # replace with the V2 timer origin
export OST_RELAY_URL=https://relay.example.com     # relay public origin
export OST_RELAY_ADMIN=http://127.0.0.1:8080       # relay admin port
export OST_RELAY_ENV=/path/to/relay/.env           # admin token and SPKI pin
export OST_RELAY_CONTAINER=ostrelay-relay-1        # for the restart test
export OST_BLE_PYTHON=/path/to/venv/bin/python     # has bleak
export OST_BLE_KEY=/path/to/bench-app.key
export OST_CANDIDATE=/path/to/dist-copy            # packaged BUILD.json + images
```

## Run

```sh
./qualify.sh                  # every automated suite against the candidate
node emergency.cjs            # physical: prints READY, then trigger the input
python3 relay_bench.py status # owner/operator helper: enroll, mint, revoke, links, ...
```

Install the signed candidate first through the timer's firmware-update UI,
after briefly pressing Reset to arm OTA. The image and signed descriptor must
come from the same frozen bundle. `qualify.sh` checks the installed build ID
against that bundle's `BUILD.json` and fails if any suite fails.
The relay env file must include `OSTRELAY_PARTNER_ORIGIN`, the independently
hosted control-page origin. Enrollment stages a 60-second approval request;
review both addresses in the local timer page and briefly press Reset to approve.

| Script | Covers |
| --- | --- |
| `scenarios.cjs` | Extend on the same URL (open partner page follows, hold limit raised), fail-open when the link expires, Reactivate |
| `relay_restart.py` | Relay server restart with a live link: timer reconnects, same URL works |
| `bluetooth_priority.py` | Full Bluetooth bench (`bluetooth_bench.py`) with a live link: relay pauses and resumes, no stuck lease, local start works, heap minimum |
| `owner_ui.cjs` | The timer's own Remote sharing page: create, SHARED badge, Extend, Revoke |
| `owner_ui_retry.cjs` | Lost responses never apply an owner action twice |
| `emergency.cjs` | Emergency input ends sharing and the link, reason shown, local control intact |
| `partner.cjs` | Drives one partner page action (used by the suites) |
| `bluetooth_soak.py`, `soak_partner.cjs` | Approved BLE phase and persistent partner viewer for the combined two-model soak |

See [Testing and verification](../../../docs/VALIDATION.md) for the distinction
between automated transport checks and physical-device verification.
