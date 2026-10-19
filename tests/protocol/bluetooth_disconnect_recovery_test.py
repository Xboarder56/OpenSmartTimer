"""Compile the real recovery method; callbacks signal, main loop restores."""
from pathlib import Path
import os, re, subprocess, tempfile

root = Path(__file__).resolve().parents[2]
source = (root/'src/BluetoothApplication.cpp').read_text()
method = re.search(r'void BluetoothApplication::serviceNetworkRecovery\(\) \{.*?^\}', source, re.M|re.S)[0]
loop = source.split('void BluetoothApplication::loop(uint32_t nowMs, bool mutationsAllowed) {', 1)[1]
assert loop.index('serviceNetworkRecovery();') < loop.index('if (!initialized_ || !enabled_)')
for block in (source.split('void BluetoothApplication::onHostReset(int reason) {',1)[1],
              source.split('case BLE_GAP_EVENT_DISCONNECT: {',1)[1]):
    assert 'networkRestorePending_.store(true);' in block.split('connected_ = false;',1)[0]
fixture = r'''
#include <atomic>
#include <cassert>
struct Network {
  bool off=false, busy=false; int resumes=0;
  bool wifiSuspended() const { return off; }
  bool setWifiSuspended(bool value) { assert(!value); ++resumes; if(busy)return false; off=false; return true; }
};
class BluetoothApplication {
 public:
  Network network_; bool connected_=true;
  std::atomic<bool> networkRestorePending_{false};
  void serviceNetworkRecovery();
};
'''
checks = r'''
int main() {
  BluetoothApplication app;
  // Disconnect while networking is already on must not poison the next mode.
  app.networkRestorePending_.store(true); app.serviceNetworkRecovery();
  app.network_.off=true; app.serviceNetworkRecovery();
  assert(app.network_.off && app.network_.resumes==0);
  // Physical link loss restores without requiring an open timer/allowed mutation.
  app.connected_=false; app.serviceNetworkRecovery();
  assert(!app.network_.off && app.network_.resumes==1);
  app.serviceNetworkRecovery(); assert(app.network_.resumes==1);
  // A rapid reconnect does not erase the latched disconnect.
  app.network_.off=true; app.connected_=true; app.networkRestorePending_.store(true);
  app.serviceNetworkRecovery(); assert(!app.network_.off && app.network_.resumes==2);
  // If restoration is temporarily busy, retry even after reconnection.
  app.network_.off=true; app.network_.busy=true; app.networkRestorePending_.store(true);
  app.serviceNetworkRecovery(); assert(app.network_.off && app.networkRestorePending_.load());
  app.network_.busy=false; app.serviceNetworkRecovery();
  assert(!app.network_.off && !app.networkRestorePending_.load());
}
'''
with tempfile.TemporaryDirectory(prefix='ost-ble-recovery-') as directory:
    cpp=Path(directory)/'test.cpp'; binary=Path(directory)/'test'
    cpp.write_text(fixture+method+checks)
    subprocess.run([os.environ.get('CXX','c++'),'-std=c++17','-Wall','-Wextra','-Werror',str(cpp),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
print('Bluetooth disconnect/reset recovery: latched loss, rapid reconnect, busy retry and main-loop-only restoration passed')
