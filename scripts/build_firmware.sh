#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
target="${1:-all}"
case "$target" in
  v1)
    export PLATFORMIO_CORE_DIR="$repo_root/.pio-core/v1"
    export PLATFORMIO_BUILD_DIR="$repo_root/.pio/build/v1"
    exec pio run -d "$repo_root" -e magbound_v1_esp8266
    ;;
  v2)
    python3 "$repo_root/scripts/framework/verify_framework_esp32c3.py"
    framework_epoch="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["source_date_epoch"])' \
      "$repo_root/framework/esp32c3/lock.json")"
    export SOURCE_DATE_EPOCH="$framework_epoch"
    export TZ=UTC
    export PLATFORMIO_CORE_DIR="$repo_root/.pio-core/v2"
    export PLATFORMIO_BUILD_DIR="$repo_root/.pio/build/v2"
    exec pio run -d "$repo_root" -e magbound_v2_esp32c3
    ;;
  all)
    "$0" v1
    "$0" v2
    ;;
  *)
    echo "usage: $0 [v1|v2|all]" >&2
    exit 2
    ;;
esac
