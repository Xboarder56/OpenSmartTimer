"""Compile the real Bluetooth-only route and verify transition/OTA/active guards."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'src/WebManagement.cpp').read_text()
method = re.search(r'void WebApplication::routeBluetoothWifiMode\(bool suspended\) \{.*?^\}', source, re.M | re.S)[0]
stub = r'''
#include <cassert>
#include <string>
using String = std::string;
struct esp_partition_t {} partition;
enum esp_ota_img_states_t { ESP_OTA_IMG_PENDING_VERIFY, ESP_OTA_IMG_VALID };
constexpr int ESP_OK = 0;
bool pendingValidation = false;
const esp_partition_t* esp_ota_get_running_partition() { return &partition; }
int esp_ota_get_state_partition(const esp_partition_t*, esp_ota_img_states_t* state) {
  *state = pendingValidation ? ESP_OTA_IMG_PENDING_VERIFY : ESP_OTA_IMG_VALID;
  return ESP_OK;
}
struct WebApplication {
  bool transitionAllowed = true;
  int status = 0;
  String errorCode;
  struct { bool open = true; bool isOpen() { return open; } } timer_;
  struct {
    bool busy = false, suspended = false;
    int changes = 0;
    bool setWifiSuspended(bool value) { if (busy) return false; suspended = value; ++changes; return true; }
  } network_;
  bool allowRequest(bool mutation) { assert(mutation); return transitionAllowed; }
  void sendError(int s, const String&, const String& code) { status = s; errorCode = code; }
  void sendWifiStatus() { status = 200; }
  void routeBluetoothWifiMode(bool suspended);
};
'''
checks = r'''
int main() {
  for (const bool suspend : {false, true}) {
    WebApplication app;
    app.transitionAllowed = false; // OTA/restart/network/relay interlock.
    app.routeBluetoothWifiMode(suspend);
    assert(app.network_.changes == 0);
    app.transitionAllowed = true;
    app.timer_.open = false;
    app.routeBluetoothWifiMode(suspend);
    assert(app.status == 409 && app.errorCode == "timer_active" && app.network_.changes == 0);
    app.timer_.open = true;
    app.network_.busy = true;
    app.routeBluetoothWifiMode(suspend);
    assert(app.status == 409 && app.errorCode == "network_mode_busy" && app.network_.changes == 0);
    app.network_.busy = false;
    pendingValidation = true;
    if (suspend) {
      app.routeBluetoothWifiMode(true);
      assert(app.status == 409 && app.errorCode == "boot_validation_pending" && app.network_.changes == 0);
    }
    // Restore is always available after other transition guards pass.
    app.routeBluetoothWifiMode(false);
    assert(app.status == 200 && !app.network_.suspended);
    pendingValidation = false;
    app.routeBluetoothWifiMode(suspend);
    assert(app.status == 200 && app.network_.suspended == suspend);
  }
}
'''
with tempfile.TemporaryDirectory(prefix='ost-network-route-') as directory:
    binary = str(Path(directory) / 'route')
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall', '-Wextra', '-Werror', '-x', 'c++', '-', '-o', binary],
                   input=stub + method + checks, text=True, check=True)
    subprocess.run([binary], check=True)
print('Bluetooth-only route: active timer, transition, scan/setup and OTA boot-validation guards passed; restore remains available')
