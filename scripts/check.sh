#!/usr/bin/env bash
# Source-only regression checks. No SDK downloads, flashing or baseline promotion.
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
group="${1:-all}"
export PYTHONDONTWRITEBYTECODE=1
python="${PYTHON:-python3}"
if [[ $# -gt 2 ]]; then
  echo "usage: $0 [all|host|web|integration] [--python=INTERPRETER]" >&2; exit 2
fi
if [[ -n "${2:-}" ]]; then
  case "$2" in
    --python=?*) python="${2#--python=}" ;;
    *) echo "usage: $0 [all|host|web|integration] [--python=INTERPRETER]" >&2; exit 2 ;;
  esac
fi
host_build="$repo_root/.pio/host-checks"

host_checks() {
  mkdir -p "$host_build"
  local test_source test_name variant
  local -a sources flags
  for test_source in "$repo_root"/tests/host/*_test.cpp; do
    test_name="$(basename "$test_source" .cpp)"
    sources=("$test_source")
    flags=(-Wall -Wextra -Werror)
    case "$test_name" in
      device_config_settings_test)
        for variant in v1 v2; do
          flags=(-Wall -Wextra -Werror)
          if [[ "$variant" == v2 ]]; then flags+=(-DSMART_TIMER_HARDWARE_V2=1 -DESP32=1); fi
          "${CXX:-c++}" -std=c++17 "${flags[@]}" \
            -I"$repo_root/tests/host/config_stubs" -I"$repo_root/include" \
            "$repo_root/src/DeviceConfig.cpp" "$test_source" -o "$host_build/${test_name}_$variant"
          "$host_build/${test_name}_$variant"
        done
        continue ;;
      timer_controller_test|timer_command_authority_test)
        for variant in v1 v2; do
          flags=(-Wall -Wextra -Werror)
          if [[ "$variant" == v2 ]]; then flags+=(-DSMART_TIMER_HARDWARE_V2=1); fi
          "${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror "${flags[@]}" \
            -I"$repo_root/tests/host" -I"$repo_root/include" \
            "$repo_root/src/TimerController.cpp" "$repo_root/src/TimerCommandAuthority.cpp" "$test_source" \
            -o "$host_build/${test_name}_$variant"
          "$host_build/${test_name}_$variant"
        done
        continue ;;
      ble_control_protocol_test)
        sources+=("$repo_root/src/TimerController.cpp" "$repo_root/src/TimerCommandAuthority.cpp" "$repo_root/src/BleControlProtocol.cpp")
        flags+=(-DSMART_TIMER_HARDWARE_V2=1) ;;
      http_request_fields_test) sources+=("$repo_root/src/HttpRequestFields.cpp") ;;
      reset_trace_test) flags+=(-I"$repo_root/tests/host/reset_trace_stubs") ;;
      device_config_apps_test)
        sources+=("$repo_root/src/DeviceConfig.cpp")
        flags+=(-DSMART_TIMER_HARDWARE_V2=1 -DESP32=1 -I"$repo_root/tests/host/config_stubs") ;;
      sse_writer_test|ble_pairing_policy_test|ble_app_registry_test|ble_management_protocol_test|ble_access_policy_test|checked_json_writer_test) ;;
      *) echo "Register dependencies for new host test: $test_name" >&2; exit 1 ;;
    esac
    "${CXX:-c++}" -std=c++17 -Wall -Wextra -Werror "${flags[@]}" \
      -I"$repo_root/tests/host" -I"$repo_root/include" \
      "${sources[@]}" -o "$host_build/$test_name"
    "$host_build/$test_name"
  done
  for test_source in "$repo_root"/tests/protocol/*_test.py; do "$python" "$test_source"; done
}

web_checks() {
  local script
  for script in "$repo_root"/scripts/checks/check_*.py; do "$python" "$script"; done
  for script in "$repo_root"/web/*.js "$repo_root"/bluetooth-app/*.js \
                "$repo_root"/bluetooth-app/*.mjs; do node --check "$script"; done
  node --test "$repo_root"/tests/web/*_test.mjs
}

integration_checks() {
  local test_source
  for test_source in "$repo_root"/tests/integration/*_test.py; do "$python" "$test_source"; done
}

case "$group" in
  host) host_checks ;;
  web) web_checks ;;
  integration) integration_checks ;;
  all) host_checks; web_checks; integration_checks ;;
  -h|--help)
    echo "usage: $0 [all|host|web|integration] [--python=INTERPRETER]"
    echo "Source tests only; full image/qualification gate: ./scripts/check_release.sh"
    exit 0 ;;
  *) echo "usage: $0 [all|host|web|integration] [--python=INTERPRETER]" >&2; exit 2 ;;
esac
echo "Source checks passed ($group). No firmware flashed or released."
