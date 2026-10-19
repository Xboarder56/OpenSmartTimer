#!/usr/bin/env bash
# Qualify one frozen candidate on a V2 bench timer: confirm the installed build
# is the candidate's (BUILD.json), then run every automated relay suite.
# Physical tests run separately (node emergency.cjs). See README.md.
set -euo pipefail
cd "$(dirname "$0")"
: "${OST_TIMER:?set OST_TIMER}" "${OST_CANDIDATE:?set OST_CANDIDATE to the packaged dist/firmware copy}"
want=$(python3 -c "import json,sys; print(next(a['build_id'] for a in json.load(open(sys.argv[1]))['artifacts'] if a['chip_family']=='ESP32-C3'))" "$OST_CANDIDATE/BUILD.json")
have=$(curl -fsS --max-time 5 "$OST_TIMER/api/v1/status" | python3 -c "import json,sys; print(json.load(sys.stdin)['build_id'])")
export OST_BENCH_OUT="${OST_BENCH_OUT:-$PWD/out}"
OUT="$OST_BENCH_OUT/qual-$want"
mkdir -p "$OST_BENCH_OUT"
mkdir "$OUT" || { echo "Choose a new OST_BENCH_OUT; existing reports are never overwritten."; exit 1; }
echo "V2 installed $have / candidate $want"
[ "$have" = "$want" ] || { echo "FAIL: the timer is not running the candidate"; exit 1; }
failures=0
run() {
  local name=$1 status=0
  shift
  echo "== $name"
  "$@" > "$OUT/$name.log" 2>&1 || status=$?
  echo "   exit=$status"
  rg 'summary|"FAIL"|^FAIL' "$OUT/$name.log" || true
  if [ "$status" -ne 0 ]; then failures=$((failures + 1)); fi
}
run scenarios node scenarios.cjs all
run relay-restart python3 relay_restart.py
run bluetooth python3 bluetooth_priority.py
run owner-ui node owner_ui.cjs
run owner-ui-retry node owner_ui_retry.cjs
run validate-bench python3 ../../../tools/validate_bench.py --url "$OST_TIMER" --output "$OUT/validate-bench.json" --allow-timer-control
echo "Qualification suites failed: $failures"
exit "$((failures > 0))"
