#pragma once
#include <cstddef>
#include <cstdint>
namespace FlashContracts {
struct MagBoundV2Esp32C3 {
  static constexpr uint32_t kPhysicalBytes = 0x400000;
  static constexpr size_t kMaximumImageBytes = 0x140000;
  static constexpr uint8_t kImageMode = 0x02; // DIO
  static constexpr uint8_t kImageSizeFrequency = 0x2f; // 4 MB / 80 MHz
  static constexpr uint16_t kImageChipId = 5;
  static constexpr size_t kHeaderLength = 14;
  static constexpr uint32_t kNvsOffset = 0x9000, kNvsBytes = 0x5000;
  static constexpr uint32_t kOtaDataOffset = 0xe000, kOtaDataBytes = 0x2000;
  static constexpr uint32_t kApp0Offset = 0x10000, kApp1Offset = 0x150000;
  static constexpr uint32_t kFilesystemOffset = 0x290000, kFilesystemBytes = 0x160000;
  static constexpr uint32_t kCoreDumpOffset = 0x3f0000, kCoreDumpBytes = 0x10000;
  static constexpr const char* kLayout = "4M/default A/B OTA";
  static constexpr const char* kMismatch = "firmware header does not match ESP32-C3 DIO/4MB/80MHz";
  static_assert(kApp0Offset + kMaximumImageBytes == kApp1Offset &&
                kApp1Offset + kMaximumImageBytes == kFilesystemOffset &&
                kFilesystemOffset + kFilesystemBytes == kCoreDumpOffset &&
                kCoreDumpOffset + kCoreDumpBytes == kPhysicalBytes,
                "MagBoundV2-ESP32C3 recovered flash contract changed");
};
}
