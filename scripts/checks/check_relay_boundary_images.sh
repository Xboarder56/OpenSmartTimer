#!/usr/bin/env bash
# After a dual-target build: the experimental relay must exist only in V2.
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/../.." && pwd)"
v1_elf="$(find "$repo_root/.pio/build/v1" -name '*.elf' -print -quit)"
v2_elf="$(find "$repo_root/.pio/build/v2" -name '*.elf' -print -quit)"
v1_nm="$(find "$repo_root/.pio-core/v1/packages" -name 'xtensa-lx106-elf-nm' -type f -print -quit)"
v2_nm="$repo_root/.pio-core/v2/packages/toolchain-riscv32-esp/bin/riscv32-esp-elf-nm"
[[ -f "$v1_elf" && -f "$v2_elf" && -x "$v1_nm" && -x "$v2_nm" ]] || {
  echo "Build both targets first (scripts/build_firmware.sh all)." >&2; exit 2; }
v1_symbols="$("$v1_nm" -C "$v1_elf")"
v2_symbols="$("$v2_nm" -C "$v2_elf")"
if grep -q 'opensmarttimer::relay' <<<"$v1_symbols"; then
  echo "ERROR: V1 image contains relay code" >&2; exit 1
fi
option_file="$(dirname "$v2_elf")/relay-option.txt"
[[ -f "$option_file" ]] || { echo "ERROR: V2 build did not record its relay option" >&2; exit 1; }
v2_option="$(tr -d '[:space:]' < "$option_file")"
if [[ "$v2_option" == enabled ]]; then
  grep -q 'opensmarttimer::relay::RelayRuntime' <<<"$v2_symbols" || {
    echo "ERROR: V2 image is missing the relay runtime" >&2; exit 1; }
  echo "Relay boundary: V2 image contains the relay; V1 image contains none."
else
  if grep -q 'opensmarttimer::relay' <<<"$v2_symbols"; then
    echo "ERROR: relay disabled, but the V2 image contains relay code" >&2; exit 1
  fi
  echo "Relay boundary: relay disabled; neither image contains relay code."
fi
