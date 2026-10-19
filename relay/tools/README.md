# Relay tools

Run from the repository root. None of these build or flash firmware.

| Tool | Purpose |
| --- | --- |
| `device_relay_check.py` | Drive a real V2 timer's relay through its local API: enroll, connect, create a link, print the share URL, and optionally soak-sample heap/stack to CSV |
| `dev_cert.sh` | Generate a self-signed development certificate and key under the current directory's `dev/` |
| `extract_spki_pin.sh` | Print the SPKI pin of a certificate file or of an explicitly given TLS server |
| [`hardware/`](hardware/README.md) | Hardware qualification suites for a real V2 and relay (Extend/expiry, relay restart, Bluetooth priority, owner UI, retries, emergency input) and `qualify.sh` |

The certificate helper writes private key material. `dev/` locations are
ignored by Git; never distribute those keys. The pin helper's remote-host form
makes a network connection. Enrollment trusts the relay by this pin, so a
self-signed certificate works as long as `OSTRELAY_SPKI_PIN` names it.
