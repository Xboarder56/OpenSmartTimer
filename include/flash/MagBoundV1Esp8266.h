#pragma once
#include <cstddef>
#include <cstdint>
namespace FlashContracts {
struct MagBoundV1Esp8266 {
  static constexpr uint32_t kPhysicalBytes = 0x400000;
  static constexpr size_t kMaximumImageBytes = 1044464;
  static constexpr uint8_t kImageMode = 0x03; // DOUT; GPIO10 emergency input depends on this.
  static constexpr uint8_t kImageSizeFrequency = 0x40; // 4 MB / 40 MHz
  static constexpr size_t kHeaderLength = 4;
  static constexpr uint32_t kApplicationOffset = 0x1000;
  static constexpr const char* kLayout = "4M/2M OTA";
  static constexpr const char* kMismatch = "firmware header does not match ESP-12 DOUT/4MB/40MHz";
};
}
