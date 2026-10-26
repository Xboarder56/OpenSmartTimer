"""Exercise the real OTA writer: a bad signed digest cannot commit flash."""
from pathlib import Path
import re
import tempfile
from release_update_crypto_test import compile_run

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / 'src/WebApplication.cpp').read_text()
methods = '\n'.join(re.search(r'void WebApplication::' + name + r'\([^)]*\) \{.*?^\}', source, re.M | re.S)[0]
                    for name in ('otaAcceptBytes', 'otaCompleteImage', 'otaAbortImage'))
stub = r'''
#include "ReleaseUpdate.h"
#include <cassert>
#include <cstring>
#include <string>
using String = std::string;
uint32_t millis() { return 100; }
namespace Hardware { void writeStatus(bool) {} }
namespace Platform { const char* otaImageDescription() { return "test timer"; } }
String updateErrorString() { return "test failure"; }
constexpr uint32_t kLegacy030Size = 0, kLegacy130Size = 0, kLegacy030Crc32 = 0, kLegacy130Crc32 = 0;
struct Writer {
  unsigned written = 0, expected = 3; bool committed = false, running = true;
  size_t write(const uint8_t*, size_t n) { written += n; return n; }
  bool end(bool = false) { committed = written == expected; running = false; return committed; }
  bool isRunning() { return running; }
} Update;
struct WebApplication {
  uint32_t otaQuietUntilMs_ = 0, otaReceivedSize_ = 0, otaExpectedSize_ = 3;
  uint32_t otaCrc32_ = 0xffffffff, otaExpectedCrc32_ = 0x352441c2;
  static constexpr uint32_t kOtaUploadQuietWindowMs = 1000;
  bool otaInstallActive_ = true, otaSignedRelease_ = true, otaHeaderValid_ = true;
  bool otaMarkerFound_ = true, otaUploadUnverifiedAllowed_ = false, otaSucceeded_ = false;
  String otaError_;
  uint8_t otaPendingChunk_[32]{}; size_t otaPendingChunkLength_ = 0;
  ReleaseUpdate::Descriptor otaRelease_;
  ReleaseUpdate::ImageHash otaImageHash_;
  void inspectFirmwareBytes(const uint8_t* data, size_t size) {
    otaReceivedSize_ += size;
    for (size_t i = 0; i < size; ++i) {
      otaCrc32_ ^= data[i];
      for (unsigned b = 0; b < 8; ++b) otaCrc32_ = (otaCrc32_ >> 1) ^ (0xedb88320 & -(otaCrc32_ & 1));
    }
  }
  void otaAcceptBytes(const uint8_t*, size_t, bool = true); void otaCompleteImage(); void otaAbortImage();
};
'''
checks = r'''
int main() {
  const uint8_t image[] = {'a','b','c'};
  for (unsigned mode = 0; mode < 4; ++mode) {
    Update = Writer{};
    WebApplication app;
    assert(ReleaseUpdate::decodeHex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", app.otaRelease_.sha256, 32));
    if (mode == 1) app.otaRelease_.sha256[0] ^= 1;
    app.otaImageHash_.begin();
    app.otaAcceptBytes(image, 1);
    assert(Update.written == 0);
    if (mode != 2) app.otaAcceptBytes(image + 1, 2);
    if (mode == 3) app.otaAbortImage(); else app.otaCompleteImage();
    assert(Update.committed == (mode == 0));
    assert(app.otaSucceeded_ == (mode == 0));
    if (mode != 0) assert(Update.written < Update.expected);
  }
}
'''
with tempfile.TemporaryDirectory() as directory:
    compile_run(stub + methods + checks, directory, 'ota-signed-integrity')
print('Real OTA callbacks commit the valid signed hash, reject a changed hash/truncation and preserve the final chunk on abort')
