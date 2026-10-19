#!/usr/bin/env bash
set -euo pipefail

readonly ARDUINO_COMMIT="554de56f40e68d591b945bd1a8c931798e8a9167"
readonly IDF_COMMIT="b66b5448e0dfac6be135e6f0ac21010aba231180"
readonly TINYUSB_COMMIT="8a78804ff91271fb32008288777fad9fe73f5735"

clone_exact() {
  local repository="$1"
  local commit="$2"
  local destination="$3"
  git init -q "$destination"
  git -C "$destination" remote add origin "$repository"
  git -C "$destination" fetch --depth 1 origin "$commit"
  git -C "$destination" checkout -q --detach FETCH_HEAD
  test "$(git -C "$destination" rev-parse HEAD)" = "$commit"
}

test "$(git rev-parse HEAD)" = \
  "afa5cdd0d41d7e465d2e39867b24aef48b1c7b84"
test -f configs/defconfig.opensmarttimer
test -f dependencies.lock

clone_exact "https://github.com/espressif/arduino-esp32.git" \
  "$ARDUINO_COMMIT" components/arduino
git -C components/arduino apply \
  ../../framework-inputs/arduino-component.patch

mkdir -p components/arduino_tinyusb
clone_exact "https://github.com/hathach/tinyusb.git" \
  "$TINYUSB_COMMIT" components/arduino_tinyusb/tinyusb

clone_exact "https://github.com/espressif/esp-idf.git" \
  "$IDF_COMMIT" esp-idf
# The upstream package-manifest generator calls `git describe --all` and
# rejects a detached commit with no local ref. This branch points only at the
# already-verified commit; it is never fetched or advanced.
git -C esp-idf branch release/v5.5 "$IDF_COMMIT"
git -C esp-idf submodule update --init --recursive --depth 1
./esp-idf/install.sh >/dev/null
source ./esp-idf/export.sh >/dev/null

export IDF_COMPONENT_OVERWRITE_MANAGED_COMPONENTS=1
export LC_ALL=C
export TZ=UTC
./build.sh -s -n -t esp32c3 opensmarttimer

test -f out/tools/esp32-arduino-libs/package.json
test -f out/tools/esp32-arduino-libs/esp32c3/sdkconfig
test -f out/tools/esp32-arduino-libs/esp32c3/dependencies.lock
