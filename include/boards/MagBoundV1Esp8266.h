#pragma once
#include "flash/MagBoundV1Esp8266.h"
namespace BoardProfiles {
struct MagBoundV1Esp8266 {
  using Flash = FlashContracts::MagBoundV1Esp8266;
  static constexpr const char* kTargetName = "MagBoundV1-ESP8266";
  static constexpr const char* kHardwareModel = "MagBound Smart Timer V1";
  static constexpr const char* kChipFamily = "ESP8266";
  static constexpr const char* kFirmwareMarker = "SMART_TIMER_ESP12_LOCAL_V1";
  static constexpr const char* kImageDescription = "OpenSmartTimer MagBound V1 ESP8266 DOUT firmware image";
  static constexpr uint8_t kLockPinA = 4, kLockPinB = 13;
  static constexpr uint8_t kStatusPin = 12, kEmergencyPin = 10, kConfigPin = 5;
  static constexpr bool kHasSeparateLockPinB = true, kHasLockIndicatorPin = false;
  static constexpr bool kLockActiveHigh = true, kStatusActiveHigh = true, kInputActiveHigh = true;
  static_assert(Flash::kImageMode == 0x03, "V1 GPIO10 requires the tested DOUT flash mode");
};
}
