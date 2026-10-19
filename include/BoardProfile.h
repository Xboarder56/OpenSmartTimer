#pragma once

// PlatformIO selects explicit product targets, not a generic ESP board. Host
// tests retain the existing V2 flag/default-V1 selection for portability.
#if defined(SMART_TIMER_TARGET_MAGBOUND_V1_ESP8266) && defined(SMART_TIMER_TARGET_MAGBOUND_V2_ESP32C3)
#error "Select exactly one Smart Timer product target"
#endif
#if defined(ARDUINO) && !defined(SMART_TIMER_TARGET_MAGBOUND_V1_ESP8266) && !defined(SMART_TIMER_TARGET_MAGBOUND_V2_ESP32C3)
#error "Firmware builds must select an explicit Smart Timer product target"
#endif
#if defined(SMART_TIMER_TARGET_MAGBOUND_V1_ESP8266) && defined(SMART_TIMER_HARDWARE_V2)
#error "MagBoundV1-ESP8266 cannot enable V2 hardware"
#endif
#if defined(SMART_TIMER_TARGET_MAGBOUND_V2_ESP32C3) && !defined(SMART_TIMER_HARDWARE_V2)
#error "MagBoundV2-ESP32C3 requires SMART_TIMER_HARDWARE_V2"
#endif

#if defined(SMART_TIMER_HARDWARE_V2)
#if defined(ARDUINO) && (!defined(ESP32) || !defined(CONFIG_IDF_TARGET_ESP32C3))
#error "MagBoundV2-ESP32C3 requires the ESP32-C3 toolchain"
#endif
#include "boards/MagBoundV2Esp32C3.h"
namespace Board { using Profile = BoardProfiles::MagBoundV2Esp32C3; }
#else
#if defined(ARDUINO) && !defined(ESP8266)
#error "MagBoundV1-ESP8266 requires the ESP8266 toolchain"
#endif
#include "boards/MagBoundV1Esp8266.h"
namespace Board { using Profile = BoardProfiles::MagBoundV1Esp8266; }
#endif

namespace Board {
using Flash = Profile::Flash;
constexpr uint8_t kDistinctPins[] = {
  Profile::kLockPinA, Profile::kStatusPin, Profile::kEmergencyPin, Profile::kConfigPin,
#if defined(SMART_TIMER_HARDWARE_V2)
  Profile::kLockIndicatorPin, Profile::kAuxiliaryInputPin
#else
  Profile::kLockPinB
#endif
};
constexpr bool distinctPins() {
  for (size_t i = 0; i < sizeof(kDistinctPins); ++i)
    for (size_t j = i + 1; j < sizeof(kDistinctPins); ++j)
      if (kDistinctPins[i] == kDistinctPins[j]) return false;
  return true;
}
static_assert(distinctPins(), "Product GPIO roles must not overlap");
static_assert(Flash::kMaximumImageBytes < Flash::kPhysicalBytes,
              "OTA image must fit the recovered product flash layout");
}
