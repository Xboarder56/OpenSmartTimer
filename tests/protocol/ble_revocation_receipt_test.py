"""Execute real revocation/receipt methods with NVS, radio and crypto spies.

The crypto spy checks key selection, not cryptography (covered by golden vectors).
No BLE adapter or timer is contacted.
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]


def methods(filename, names):
    source = (ROOT / 'src' / filename).read_text()
    return '\n'.join(re.search(
        r'(?:bool|void|int) BluetoothApplication::' + name + r'\(.*?^\}',
        source, re.MULTILINE | re.DOTALL)[0] for name in names)


actual = methods('BluetoothApplication.cpp', [
    'revokeApp', 'revokePairing', 'loadPersistentControlKey', 'disableControl',
    'clearSessionKey', 'clearControlKey']) + methods('BluetoothManagement.cpp', [
    'deferManagementDisconnect', 'managementReceiptReadable', 'onManagementRead',
    'storeManagementReply', 'publishManagementSegment'])

stub = r'''
#include "BleAppRegistry.h"
#include "BleAccessPolicy.h"
#include "BleManagementProtocol.h"
#include <cassert>
#include <string>
#include <vector>
using namespace BleManagement;
using String = std::string;
#define portENTER_CRITICAL(x) (void)0
#define portEXIT_CRITICAL(x) (void)0
constexpr int BLE_HS_CONN_HANDLE_NONE = 0xffff, BLE_ERR_REM_USER_CONN_TERM = 0x13;
constexpr int BLE_ATT_ERR_INSUFFICIENT_RES = 0x11;
uint32_t clockMs = 100;
uint32_t millis() { return clockMs; }
int terminated = 0;
int ble_gap_terminate(uint16_t, int) { ++terminated; return 0; }
struct ble_gatt_access_ctxt { std::vector<uint8_t>* om; };
int os_mbuf_append(std::vector<uint8_t>* out, const uint8_t* p, size_t n) {
  out->insert(out->end(), p, p + n); return 0;
}
uint8_t signedWith[32]{};
bool hmacParts(const uint8_t* key, size_t n, const uint8_t*, size_t,
               const uint8_t*, size_t, const uint8_t*, size_t, uint8_t* mac) {
  assert(n == sizeof(signedWith)); memcpy(signedWith, key, n);
  memset(mac, 0xaa, 32); return true;
}
enum class TimerEventType { BluetoothPairingRevoked };
struct Config {
  BleAppRegistry registry;
  bool fail = false;
  Config() { registry.apps[0].id = 1; registry.apps[1].id = 2; }
  bool removeBluetoothApp(uint32_t id) {
    const int slot = registry.find(id);
    if (fail || slot < 0) return false;
    registry.apps[slot] = {}; return true;
  }
  bool clearBluetoothPairing() {
    if (fail) return false;
    registry.apps[0] = {}; registry.apps[1] = {}; return true;
  }
  const BleAppRegistry& bluetoothApps() const { return registry; }
  bool bluetoothPaired() const { return registry.count() != 0; }
};
struct BluetoothApplication {
  static constexpr size_t kSessionKeySize = 32;
  Config config_;
  struct { int events = 0; void recordEvent(TimerEventType, uint32_t) { ++events; } } timer_;
  bool connected_ = true, connectionEncrypted_ = true, connectionBonded_ = true;
  bool controlAuthenticated_ = true, controlEnabled_ = true, enabled_ = true;
  bool initialized_ = true, managementDispatchActive_ = true, pairing = false;
  bool preserveCurrentBond_ = true, bondCleanupPending_ = false;
  bool pendingControl_ = true, pendingControlAuthorized_ = true;
  uint16_t connectionHandle_ = 7, pendingControlConnectionHandle_ = 7;
  uint32_t authenticatedAppId_ = 1, authorizationGeneration_ = 1;
  uint32_t nextBondCleanupAtMs_ = 100, lastCommandCounter_ = 2;
  uint32_t lastAuthenticationAttemptAtMs_ = 0;
  uint8_t sessionKey_[32], controlKey_[16]{}, lastAuthenticatedCommand_[32]{};
  BleAppRegistry::App controlApps_[3]{};
  uint32_t knownAppIds_[3]{}, appLastSeenMs_[3]{}, appResultIds_[3]{};
  uint8_t appResults_[3][32]{};
  uint32_t managementDisconnectAtMs_ = 0, managementReplyGeneration_ = 0;
  bool managementTerminalReplyReady_ = false;
  uint16_t managementTerminalConnection_ = 0xffff;
  uint8_t managementScratch_[514]{}, managementResponseValue_[512]{};
  size_t managementResponseLength_ = 0;
  uint8_t managementOpcode_ = 0;
  uint16_t managementRequestId_ = 0, managementStatus_ = 0;
  String managementBody_;
  int cleanup = 0, ready = 0;
  BluetoothApplication() { memset(sessionKey_, 0x5a, sizeof(sessionKey_)); }
  uint32_t activeAppId() const { return authenticatedAppId_; }
  bool pairingActive(uint32_t) const { return pairing; }
  void attemptBondCleanup(uint32_t) { ++cleanup; }
  void cancelPairing() { pairing = false; }
  void rotateControlChallenge() {}
  void updateControlChallengeFrame() {}
  void sendManagementReady() { ++ready; }
  bool revokeApp(uint32_t); bool revokePairing();
  bool loadPersistentControlKey(bool); void disableControl();
  void clearSessionKey(); void clearControlKey();
  void deferManagementDisconnect();
  bool managementReceiptReadable(uint16_t, uint32_t) const;
  int onManagementRead(uint16_t, ble_gatt_access_ctxt*);
  void storeManagementReply(uint8_t, uint16_t, uint32_t, int, const String&, const uint8_t* = nullptr);
  bool publishManagementSegment(uint16_t, uint32_t, const uint8_t* = nullptr);
};
'''

checks = r'''
int main() {
  for (bool all : {false, true}) {
    for (bool viaBle : {false, true}) {
      for (bool saveFails : {false, true}) {
        BluetoothApplication app;
        app.managementDispatchActive_ = viaBle; app.config_.fail = saveFails;
        terminated = 0;
        uint8_t capturedKey[32]; memcpy(capturedKey, app.sessionKey_, 32);
        assert((all ? app.revokePairing() : app.revokeApp(1)) == !saveFails);
        if (saveFails) {
          assert(app.controlAuthenticated_ && app.authenticatedAppId_ == 1);
          assert(app.config_.registry.count() == 2 && app.timer_.events == 0);
          assert(app.managementDisconnectAtMs_ == 0 && app.cleanup == 0 && terminated == 0);
          assert(!memcmp(app.sessionKey_, capturedKey, 32)); continue;
        }
        assert(app.config_.registry.count() == (all ? 0 : 1));
        assert(!app.controlAuthenticated_ && app.authenticatedAppId_ == 0);
        assert(!app.pendingControl_ && !app.pendingControlAuthorized_);
        for (auto b : app.sessionKey_) assert(b == 0);
        assert(app.timer_.events == 1 && app.bondCleanupPending_);
        if (!viaBle) {
          assert(terminated == 1 && app.cleanup == 1 && app.managementDisconnectAtMs_ == 0);
          continue;
        }
        assert(terminated == 0 && app.cleanup == 0);
        assert(app.managementDisconnectAtMs_ == clockMs + BleAccessPolicy::kReceiptDrainMs);
        std::vector<uint8_t> bytes;
        ble_gatt_access_ctxt context{&bytes};
        assert(app.onManagementRead(7, &context) == 0x83 && bytes.empty());
        const String body = "{\"approval_removed\":true,\"disconnecting\":true}";
        app.storeManagementReply(all ? 0x59 : 0x56, 9, 10, 200, body, capturedKey);
        assert(!memcmp(signedWith, capturedKey, 32)); // not the erased live key
        assert(app.ready == 1 && app.managementTerminalReplyReady_);
        assert(app.onManagementRead(8, &context) == 0x83 && bytes.empty());
        assert(app.onManagementRead(7, &context) == 0);
        assert(bytes.size() == kResponseHeaderBytes + body.length() + kMacBytes);
        assert(!memcmp(bytes.data() + kResponseHeaderBytes, body.data(), body.length()));
        bytes.clear();
        app.connectionEncrypted_ = false;
        assert(app.onManagementRead(7, &context) == 0x0f && bytes.empty());
        app.connectionEncrypted_ = true; app.connectionBonded_ = false;
        assert(app.onManagementRead(7, &context) == 0x05 && bytes.empty());
        app.connectionBonded_ = true;
        clockMs += BleAccessPolicy::kReceiptDrainMs;
        assert(app.onManagementRead(7, &context) == 0x83 && bytes.empty());
        clockMs = 100;
      }
    }
  }
  BluetoothApplication other;
  terminated = 0;
  assert(other.revokeApp(2));
  assert(other.controlAuthenticated_ && other.activeAppId() == 1);
  assert(other.managementDisconnectAtMs_ == 0 && terminated == 0 && other.cleanup == 1);
  assert(other.sessionKey_[0] == 0x5a);
  BluetoothApplication pairing;
  pairing.pairing = true;
  assert(!pairing.revokeApp(1) && pairing.config_.registry.count() == 2);
}
'''

with tempfile.TemporaryDirectory() as directory:
    binary = str(Path(directory) / 'ble-revocation')
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-I' + str(ROOT / 'include'), '-x', 'c++', '-', '-o', binary],
                   input=stub + actual + checks, text=True, check=True)
    subprocess.run([binary], check=True)
print('Actual BLE revocation/receipt methods passed NVS failure, authority wipe, key selection and read-window tests')
