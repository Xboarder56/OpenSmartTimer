#pragma once
#include "flash/MagBoundV2Esp32C3.h"
namespace BoardProfiles {
struct MagBoundV2Esp32C3 {
  using Flash = FlashContracts::MagBoundV2Esp32C3;
  static constexpr const char* kTargetName = "MagBoundV2-ESP32C3";
  static constexpr const char* kHardwareModel = "MagBound Smart Timer V2";
  static constexpr const char* kChipFamily = "ESP32-C3";
  static constexpr const char* kFirmwareMarker = "OPEN_SMART_TIMER_MAGBOUND_V2_ESP32C3";
  static constexpr const char* kImageDescription = "OpenSmartTimer MagBound V2 ESP32-C3 firmware image";
  static constexpr uint8_t kLockPinA = 3;
  static constexpr uint8_t kLockPinB = 3;
  static constexpr uint8_t kLockIndicatorPin = 6;
  static constexpr uint8_t kStatusPin = 5;
  static constexpr uint8_t kEmergencyPin = 0;
  static constexpr uint8_t kConfigPin = 4;
  // Recovered accessory input: initialized only, no unverified behavior.
  static constexpr uint8_t kAuxiliaryInputPin = 1;
  static constexpr bool kHasSeparateLockPinB = false, kHasLockIndicatorPin = true;
  static constexpr bool kLockActiveHigh = true, kStatusActiveHigh = true, kInputActiveHigh = true;
};
}
