#!/usr/bin/env bash
# dev_cert.sh — mint a self-signed cert for local development.
#
# Produces:
#   dev/relay.local.key    — private key (keep, never commit)
#   dev/relay.local.crt    — self-signed cert, 90-day validity
#   dev/relay.local.pin    — SHA-256 SPKI hash (paste into enrollment)
#
# Usage:
#   ./dev_cert.sh                              # default hostname: relay.local
#   ./dev_cert.sh my-relay.lan                 # custom hostname
#
# Requires: openssl.

set -euo pipefail
HOST="${1:-relay.local}"
OUT="dev"
mkdir -p "$OUT"

openssl req -x509 -newkey rsa:2048 -sha256 -days 90 -nodes \
  -keyout "$OUT/$HOST.key" -out "$OUT/$HOST.crt" \
  -subj "/CN=$HOST" \
  -addext "subjectAltName=DNS:$HOST,DNS:localhost,IP:127.0.0.1" \
  2>/dev/null

# Compute the SPKI pin — this is what the timer's enrollment bundle
# should carry in `pin_sha256_hex`.
"$(dirname "$0")/extract_spki_pin.sh" "$OUT/$HOST.crt" > "$OUT/$HOST.pin"

echo "[ok] wrote:"
echo "    $OUT/$HOST.key   (private key — never commit)"
echo "    $OUT/$HOST.crt   (cert, 90 days validity)"
echo "    $OUT/$HOST.pin   (SPKI hash: $(cat "$OUT/$HOST.pin"))"
