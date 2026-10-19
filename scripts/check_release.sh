#!/usr/bin/env bash
# Full release gate: source tests, SDK, both images and exact qualification.
set -euo pipefail
repo_root="$(cd "$(dirname "$0")/.." && pwd)"
export PYTHONDONTWRITEBYTECODE=1
"$repo_root/scripts/check.sh" all
# Match CI's relay gates using its own environment, not the root test venv.
relay_test_python="${RELAY_TEST_PYTHON:-$repo_root/relay/.venv/bin/python}"
PYTHON="$relay_test_python" "$repo_root/relay/tests/check.sh"
"$repo_root/relay/tests/browser.sh"
PYTHON="$relay_test_python" "$repo_root/relay/tests/relay_e2e.sh" --browser
"$repo_root/scripts/build_framework_esp32c3.sh"
"$repo_root/scripts/build_firmware.sh" all
"$repo_root/scripts/checks/check_relay_boundary_images.sh"
python3 "$repo_root/scripts/release/verify_release.py"
python3 "$repo_root/scripts/release/package_firmware.py"
python3 "$repo_root/scripts/release/package_source_materials.py"
# A prerelease may ship partially qualified, but its record must name these
# exact images, its physical emergency check must have passed and nothing may
# have failed. A stable release needs every check passed.
version="$(sed -n 's/^firmware_version *= *//p' "$repo_root/platformio.ini")"
if [[ "$version" == *-* ]]; then
  python3 "$repo_root/scripts/release/verify_qualification.py" --allow-pending --release-candidate --bundle "$repo_root/dist/firmware"
else
  python3 "$repo_root/scripts/release/verify_qualification.py" --bundle "$repo_root/dist/firmware"
fi
echo "Full dual-target release check passed."
