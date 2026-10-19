"""Exercise real runtime network methods with a fake radio/config, including reboot.

The host fake counts starts and persistent writes; no hardware is contacted.
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'src/LocalNetwork.cpp').read_text()
methods = '\n'.join(re.search(r'(?:bool|void|String|uint32_t) LocalNetwork::' + name + r'\([^\n]*\)(?: const)? \{.*?^\}', source, re.M | re.S)[0]
                    for name in ('setWifiSuspended', 'startConfiguredNetwork', 'connectStation', 'loop', 'stationState', 'retryInMs'))
header = (ROOT / 'include/LocalNetwork.h').read_text()
header = re.sub(r'^#(?:pragma|include).*\n', '', header, flags=re.M)
constants = source.split('namespace {', 1)[1].split('\n}', 1)[0]
stub = r'''
#include <cassert>
#include <cstdint>
#include <string>
using String = std::string;
uint32_t now = 100;
uint32_t millis() { return now; }
enum wl_status_t { WL_DISCONNECTED, WL_CONNECTED };
enum { WIFI_OFF, WIFI_STA, WIFI_AP };
struct Radio {
  int radioMode = WIFI_STA, starts = 0, touches = 0;
  bool autoReconnect = true, failOff = false;
  wl_status_t state = WL_CONNECTED;
  bool mode(int m) {
    ++touches;
    if (m == WIFI_OFF && failOff) return false;
    radioMode = m; if (m == WIFI_OFF) state = WL_DISCONNECTED;
    return true;
  }
  void setAutoReconnect(bool value) { ++touches; autoReconnect = value; }
  wl_status_t status() const { return state; }
  void begin(const char*, const char*) { ++starts; ++touches; state = WL_DISCONNECTED; }
  String macAddress() const { return "test"; }
} WiFi;
struct DNSServer { void stop() {} void processNextRequest() {} };
namespace ResetTrace { enum Phase { kNetworkDns, kNetworkScan, kNetworkStation, kNetworkMdns }; inline void mark(Phase) {} }
namespace Platform { void stopMdns() {} void serviceMdns() {} }
class DeviceConfig {
 public:
  bool saved = true;
  int writes = 0;
  bool hasWifi() const { return saved; }
  bool setWifi(const String&, const String&) { ++writes; return true; }
  const char* wifiSsid() const { return "saved-network"; }
  const char* wifiPassword() const { return "saved-secret"; }
};
'''
helpers = r'''
bool LocalNetwork::standaloneMode() const { return !wifiSuspended_ && !config_.hasWifi(); }
void LocalNetwork::startAccessPoint() { accessPointRunning_ = true; WiFi.mode(WIFI_AP); ++WiFi.starts; }
void LocalNetwork::stopAccessPoint() { accessPointRunning_ = false; }
void LocalNetwork::startMdnsIfNeeded() { mdnsStarted_ = true; }
void LocalNetwork::updateScan(uint32_t) {}
void LocalNetwork::failCredentialTest(const String&) { credentialTestPending_ = false; }
void LocalNetwork::expectStationDisconnect() {}
void LocalNetwork::discardScanResults() {}
bool LocalNetwork::scanInProgress() const { return scanState_ == ScanState::Scanning; }
uint8_t LocalNetwork::accessPointClients() const { return 0; }
const char* LocalNetwork::wifiErrorForStatus(wl_status_t) { return nullptr; }
'''
checks = r'''
int main() {
  for (bool saved : {false, true}) {
    DeviceConfig config; config.saved = saved;
    LocalNetwork network(config);
    network.startConfiguredNetwork();
#if defined(SMART_TIMER_HARDWARE_V2)
    // Active setup must finish first, so a candidate can never later commit.
    network.credentialTestPending_ = true;
    assert(!network.setWifiSuspended(true));
    network.credentialTestPending_ = false;
    network.scanState_ = LocalNetwork::ScanState::Scanning;
    assert(!network.setWifiSuspended(true));
    network.scanState_ = LocalNetwork::ScanState::Idle;
    assert(network.setWifiSuspended(true));
    assert(WiFi.radioMode == WIFI_OFF && !WiFi.autoReconnect);
    assert(!network.accessPointRunning() && !network.stationConnected());
    assert(network.stationState(now) == "disabled");
    assert(std::string(network.modeName()) == "bluetooth");
    assert(!network.standaloneMode());
    const int touches = WiFi.touches;
    assert(network.setWifiSuspended(true)); // Idempotent without another stop.
    for (now = 1000; now <= 1800000; now += 1000) {
      network.loop(now);
      assert(network.retryInMs(now) == 0);
    }
    assert(WiFi.touches == touches && config.writes == 0);
    // Disconnection must not bring AP/retry back. Explicit resume does.
    const int starts = WiFi.starts;
    assert(network.setWifiSuspended(false));
    assert(WiFi.starts == starts + 1 && !network.wifiSuspended());
    assert(WiFi.radioMode == (saved ? WIFI_STA : WIFI_AP));
    assert(config.writes == 0 && config.saved == saved);
    assert(network.setWifiSuspended(true));
    LocalNetwork rebooted(config);
    rebooted.startConfiguredNetwork();
    assert(!rebooted.wifiSuspended());
    assert(WiFi.radioMode == (saved ? WIFI_STA : WIFI_AP));
    assert(config.writes == 0);
    // Radio failure cannot claim suspended; configured network is restored.
    WiFi.failOff = true;
    assert(!rebooted.setWifiSuspended(true));
    assert(!rebooted.wifiSuspended() && config.writes == 0);
    WiFi.failOff = false;
#else
    assert(!network.setWifiSuspended(true));
    assert(!network.wifiSuspended() && config.writes == 0);
#endif
  }
}
'''
with tempfile.TemporaryDirectory(prefix='ost-network-test-') as temp:
    cpp = Path(temp) / 'test.cpp'
    # Expose state only to the host fixture to test the real transition guards.
    cpp.write_text(stub + header.replace(' private:', ' public:') + constants + helpers + methods + checks)
    for variant in ('v1', 'v2'):
        binary = Path(temp) / variant
        command = [os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall', '-Wextra', '-Werror', '-Wno-unused-const-variable']
        if variant == 'v2': command.append('-DSMART_TIMER_HARDWARE_V2=1')
        subprocess.run(command + [str(cpp), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
print('Bluetooth-only network: no retries/AP/flash writes while suspended; explicit resume, reboot recovery, busy/failure guards, V1 exclusion passed')
