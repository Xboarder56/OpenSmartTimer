"""Compile the actual HTTP OTA callbacks with host spies; no radio/flash writes."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'src/WebApplication.cpp').read_text()
methods = '\n'.join(re.search(r'void WebApplication::' + name + r'\(\) \{.*?^\}',
                               source, re.MULTILINE | re.DOTALL)[0]
                    for name in ('handleOtaUpload', 'handleOtaComplete'))
stub = r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <string>
using String = std::string;
#define ESP32 1
void esp_task_wdt_reset() {}
void yield() {}
uint32_t millis() { return 100; }
namespace ReleaseUpdate { struct Descriptor { uint8_t sha256[32]{}; }; bool memory = true; bool hasVerificationMemory() { return memory; } }
enum { UPLOAD_FILE_START, UPLOAD_FILE_WRITE, UPLOAD_FILE_END, UPLOAD_FILE_ABORTED };
struct HTTPUpload { int status = 0; uint8_t* buf = nullptr; unsigned currentSize = 1; };
struct { HTTPUpload data; String host = "192.168.1.42"; HTTPUpload& upload() { return data; } String header(const char*) { return host; } } server_;
struct HttpRequestFields { static bool localHost(const char* host) { return std::string(host) != "evil.example.com"; } };
struct { int ended = 0; bool isRunning() { return true; } void end() { ++ended; } } Update;
enum class TimerEventType { OtaFailed, OtaSucceeded };
struct { bool isOpen() { return true; } void recordEvent(TimerEventType, uint32_t, int32_t = 0) {} } timer_;
struct { bool scanInProgress() { return false; } } network_;
struct WebApplication {
  bool bluetoothOtaActive_ = false, httpOtaRejectedDuringBluetooth_ = false;
  bool otaAllowed_ = true, otaSucceeded_ = false, otaInstallActive_ = true;
  bool otaUploadUnverifiedAllowed_ = false;
  uint32_t otaArmedUntilMs_ = 700, otaQuietUntilMs_ = 800, restartAtMs_ = 0;
  uint32_t otaPrepareExpiresAtMs_ = 0, otaPreparedSize_ = 0, otaPreparedCrc32_ = 0;
  uint32_t otaExpectedSize_ = 1000, otaExpectedCrc32_ = 123, otaReceivedSize_ = 100;
  bool otaUnverifiedAllowed_ = true;
  bool signedRequested = false;
  bool otaSignedRelease_ = false, otaPreparedSignedRelease_ = false;
  ReleaseUpdate::Descriptor otaRelease_;
  uint8_t otaPreparedSha256_[32]{};
  String otaError_;
  int resets = 0, begins = 0, writes = 0, finishes = 0, aborts = 0, busy = 0;
  bool interlock = false;  // e.g. V2 relay flash maintenance
  bool mutationInterlockActive() const { return interlock; }
  void otaResetUploadState() { ++resets; }
  bool otaArmed(uint32_t) { return true; }
  bool otaUnverifiedAllowed(uint32_t) { return true; }
  bool origin = true;
  bool sameOrigin() { return origin; }
  bool requestFromAccessPoint() { return false; }
  bool parseOtaMetadata(uint32_t&, uint32_t&) { return true; }
  String requestValue(const char*) const { return signedRequested ? "signature" : ""; }
  bool parseReleaseMetadata(uint32_t, uint32_t, ReleaseUpdate::Descriptor&, bool&) { return true; }
  void otaBeginImage(uint32_t) { ++begins; }
  void otaAcceptBytes(uint8_t*, unsigned) { ++writes; }
  void otaCompleteImage() { ++finishes; otaSucceeded_ = true; }
  void otaAbortImage() { ++aborts; }
  void sendOtaBusy() { ++busy; }
  void sendError(int, String) {}
  void sendJson(int, String) {}
  void handleOtaUpload(); void handleOtaComplete();
};
'''
checks = r'''
int main() {
  {
    WebApplication app;
    app.signedRequested = true; ReleaseUpdate::memory = false;
    server_.data.status = UPLOAD_FILE_START; app.handleOtaUpload();
    assert(app.begins == 0 && !app.otaError_.empty());
    ReleaseUpdate::memory = true;
  }
  for (const int terminal : {UPLOAD_FILE_END, UPLOAD_FILE_ABORTED}) {
    WebApplication app;
    app.bluetoothOtaActive_ = true;
    server_.data.status = UPLOAD_FILE_START; app.handleOtaUpload();
    server_.data.status = UPLOAD_FILE_WRITE; app.handleOtaUpload();
    // Even if the BLE transfer is cancelled meanwhile, this HTTP request
    // must stay rejected and must not operate on the shared Update instance.
    app.bluetoothOtaActive_ = false;
    server_.data.status = terminal; app.handleOtaUpload();
    app.handleOtaComplete();
    assert(app.busy == 1 && app.resets == 0 && app.begins == 0);
    assert(app.writes == 0 && app.finishes == 0 && app.aborts == 0 && Update.ended == 0);
    assert(app.otaArmedUntilMs_ == 700 && app.otaQuietUntilMs_ == 800);
    assert(app.otaInstallActive_ && app.otaAllowed_ && app.otaError_.empty());
    assert(app.otaExpectedSize_ == 1000 && app.otaExpectedCrc32_ == 123 && app.otaReceivedSize_ == 100);
    assert(app.restartAtMs_ == 0 && app.otaUnverifiedAllowed_);
    // The next ordinary HTTP transfer remains usable, including on V1.
    server_.data.status = UPLOAD_FILE_START; app.handleOtaUpload();
    server_.data.status = UPLOAD_FILE_WRITE; app.handleOtaUpload();
    server_.data.status = UPLOAD_FILE_END; app.handleOtaUpload();
    app.handleOtaComplete();
    assert(app.resets == 1 && app.begins == 1 && app.writes == 1 && app.finishes == 1);
    assert(app.restartAtMs_ == 1600);
  }
  {
    // A subsystem holding the mutation interlock (relay flash maintenance)
    // refuses a new HTTP image before any Update call.
    WebApplication app;
    app.interlock = true;
    server_.data.status = UPLOAD_FILE_START; app.handleOtaUpload();
    server_.data.status = UPLOAD_FILE_WRITE; app.handleOtaUpload();
    server_.data.status = UPLOAD_FILE_END; app.handleOtaUpload();
    assert(!app.otaAllowed_ && !app.otaError_.empty());
    assert(app.begins == 0 && app.writes == 0 && app.finishes == 0);
  }
  for (const bool foreignHost : {false, true}) {
    // A rejected upload (another site, or a public name pointed at the timer)
    // must not use up the owner's physical arm or recovery override.
    WebApplication app;
    app.origin = foreignHost;
    server_.host = foreignHost ? "evil.example.com" : "192.168.1.42";
    server_.data.status = UPLOAD_FILE_START; app.handleOtaUpload();
    assert(!app.otaAllowed_ && app.begins == 0);
    assert(app.otaArmedUntilMs_ == 700 && app.otaUnverifiedAllowed_);
    server_.host = "192.168.1.42";
  }
}
'''
with tempfile.TemporaryDirectory() as directory:
    binary = str(Path(directory) / 'ota-isolation')
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                    '-x', 'c++', '-', '-o', binary], input=stub + methods + checks,
                   text=True, check=True)
    subprocess.run([binary], check=True)
print('Actual HTTP OTA callbacks preserve an active BLE upload, accept the next HTTP upload, and honor the mutation interlock')
