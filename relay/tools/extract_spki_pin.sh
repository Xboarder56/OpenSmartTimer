#!/usr/bin/env bash
# extract_spki_pin.sh — print a certificate's SPKI pin for the relay.
#
# Output is the unpadded base64url SHA-256 of the SubjectPublicKeyInfo: the
# value OSTRELAY_SPKI_PIN and the timer's POST /api/v1/relay/enroll `spki_pin`
# expect (43 characters). Works on a live server or a PEM/DER file.
#
# Usage:
#   ./extract_spki_pin.sh relay.example.com                # live server, port 443
#   ./extract_spki_pin.sh relay.example.com 8443           # live server, custom port
#   ./extract_spki_pin.sh ./cert.pem                       # PEM/DER cert file
#   ./extract_spki_pin.sh --hex ./cert.pem                 # hex form, for comparison
#
# Requires: openssl.

set -euo pipefail

if ! command -v openssl >/dev/null; then
  echo "openssl is required" >&2
  exit 2
fi

emit_pin() {
  local input="$1"
  local port="${2:-443}"

  local cert_pem
  if [[ -f "$input" ]]; then
    cert_pem="$(openssl x509 -in "$input" -inform PEM 2>/dev/null \
                || openssl x509 -in "$input" -inform DER)"
  else
    cert_pem="$(openssl s_client -servername "$input" \
                  -connect "$input:$port" </dev/null 2>/dev/null \
                  | openssl x509 -outform PEM)"
  fi

  # SHA-256 over the DER SubjectPublicKeyInfo: what the timer compares.
  local spki_der
  spki_der="$(echo "$cert_pem" | openssl x509 -pubkey -noout | openssl pkey -pubin -outform DER | base64)"
  if [[ "$format" == hex ]]; then
    echo "$spki_der" | base64 -d | openssl dgst -sha256 -hex | awk '{print $NF}'
  else
    echo "$spki_der" | base64 -d | openssl dgst -sha256 -binary | base64 | tr '+/' '-_' | tr -d '=\n'
    echo
  fi
}

format=base64url
if [[ "${1:-}" == --hex ]]; then format=hex; shift; fi
case "${1:-}" in
  ""|-h|--help)
    sed -n '2,15p' "$0"; exit 0 ;;
  *)
    emit_pin "$@"
    ;;
esac
