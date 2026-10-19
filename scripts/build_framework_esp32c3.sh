#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/.." && pwd)"
lock="$repo_root/framework/esp32c3/lock.json"
package="$repo_root/.framework/esp32c3-libs"
archive="$repo_root/dist/framework/OpenSmartTimer-ESP32C3-Framework-ost2.tar.gz"
force_rebuild=false
case "${1:-}" in
  "") ;;
  --rebuild) force_rebuild=true ;;
  *)
    echo "usage: $0 [--rebuild]" >&2
    exit 2
    ;;
esac

builder_commit="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["builder_commit"])' "$lock")"
builder_image="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["builder_image"])' "$lock")"
source_date_epoch="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["source_date_epoch"])' "$lock")"

if ! $force_rebuild && \
   "$repo_root/scripts/framework/verify_framework_esp32c3.py" --package "$package" >/dev/null 2>&1; then
  "$repo_root/scripts/framework/package_framework_esp32c3.py" \
    --package "$package" --output "$archive" --mtime "$source_date_epoch"
  "$repo_root/scripts/framework/verify_framework_esp32c3.py" \
    --package "$package" --archive "$archive"
  exit 0
fi

build_root="$(mktemp -d "${TMPDIR:-/tmp}/opensmarttimer-framework.XXXXXX")"
cleanup() {
  local status=$?
  # On Linux the builder container writes its output as root, which this user
  # cannot delete; remove it from inside the same image. Cleanup never changes
  # the build's own exit status.
  if ! rm -rf "$build_root" 2>/dev/null; then
    docker run --rm -v "$build_root:/cleanup" --entrypoint find "$builder_image" \
      /cleanup -mindepth 1 -delete >/dev/null 2>&1 || true
    rm -rf "$build_root" 2>/dev/null || echo "warning: could not remove $build_root" >&2
  fi
  exit "$status"
}
trap cleanup EXIT
builder="$build_root/lib-builder"
git init -q "$builder"
git -C "$builder" remote add origin \
  "https://github.com/espressif/esp32-arduino-lib-builder.git"
git -C "$builder" fetch --depth 1 origin "$builder_commit"
git -C "$builder" checkout -q --detach FETCH_HEAD
test "$(git -C "$builder" rev-parse HEAD)" = "$builder_commit"

git -C "$builder" apply "$repo_root/framework/esp32c3/builder.patch"
mkdir -p "$builder/framework-inputs"
cp "$repo_root/framework/esp32c3/arduino-component.patch" \
  "$builder/framework-inputs/arduino-component.patch"
cp "$repo_root/framework/esp32c3/defconfig.opensmarttimer" \
  "$builder/configs/defconfig.opensmarttimer"
cp "$repo_root/framework/esp32c3/dependencies.lock" \
  "$builder/dependencies.lock"
cp "$repo_root/framework/esp32c3/run-builder.sh" "$builder/run-builder.sh"
chmod +x "$builder/run-builder.sh"

docker run --rm \
  -e "SOURCE_DATE_EPOCH=$source_date_epoch" \
  -e "TZ=UTC" \
  -e "LC_ALL=C" \
  -e "LIBBUILDER_GIT_SAFE_DIR=/arduino-esp32" \
  -v "$builder:/arduino-esp32" \
  -w /arduino-esp32 \
  "$builder_image" ./run-builder.sh

python3 "$repo_root/scripts/framework/finalize_framework_esp32c3.py" \
  --raw "$builder/out/tools/esp32-arduino-libs" \
  --output "$package" --lock "$lock"
python3 "$repo_root/scripts/framework/package_framework_esp32c3.py" \
  --package "$package" --output "$archive" --mtime "$source_date_epoch"
if grep -q '"PENDING"' "$lock"; then
  python3 "$repo_root/scripts/framework/verify_framework_esp32c3.py" \
    --package "$package" --archive "$archive" --allow-pending-lock
  echo "The first clean build completed. Copy the three printed SHA-256 values"
  echo "into framework/esp32c3/lock.json, then run this script again."
else
  python3 "$repo_root/scripts/framework/verify_framework_esp32c3.py" \
    --package "$package" --archive "$archive"
  echo "Clean framework build matches every locked hash."
fi
