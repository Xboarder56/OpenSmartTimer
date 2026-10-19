#!/usr/bin/env bash
# Operator, timer, partner, Bluetooth and update pages in real browsers.
# CI and scripts/check_release.sh both run this one list. Synthetic fixtures
# only: nothing here contacts a timer or a deployed relay.
set -euo pipefail
tests="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
node="${NODE:-node}"
chromium() { "$node" "$tests/$1"; }
webkit() { OSTRELAY_BROWSER=webkit "$node" "$tests/$1"; }

chromium pages_browser.cjs
webkit pages_browser.cjs
chromium appearance_browser.cjs
chromium timer_status_browser.cjs
webkit timer_status_browser.cjs
chromium setup_approval_browser.cjs
chromium watch_reconnect_browser.cjs
webkit watch_reconnect_browser.cjs
chromium bluetooth_ui_browser.cjs
webkit bluetooth_ui_browser.cjs
chromium ota_browser.cjs
echo "Relay browser checks passed (Chromium and WebKit; synthetic fixtures)."
