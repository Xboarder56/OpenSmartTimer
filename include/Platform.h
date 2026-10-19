#pragma once

#include <Arduino.h>
#include "BoardProfile.h"

#if defined(ESP8266)
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include "CooperativeWebServer.h"
#include <ESP8266mDNS.h>
#include <cont.h>
extern "C" {
#include <user_interface.h>
}
using SmartTimerWebServer = CooperativeWebServer;
#elif defined(ESP32)
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_wifi.h>
using SmartTimerWebServer = WebServer;
#else
#error "OpenSmartTimer supports only ESP8266 and ESP32 targets"
#endif

#if defined(SMART_TIMER_HARDWARE_V2) && !defined(CONFIG_IDF_TARGET_ESP32C3)
#error "The MagBound V2 target requires an ESP32-C3 build"
#elif !defined(SMART_TIMER_HARDWARE_V2) && !defined(ESP8266)
#error "The MagBound V1 target requires an ESP8266 build"
#endif

namespace Platform {

inline String deviceId() {
#if defined(ESP8266)
  return String(ESP.getChipId(), HEX);
#else
  const uint64_t value = ESP.getEfuseMac() & 0x0000FFFFFFFFFFFFULL;
  char text[13];
  snprintf(text, sizeof(text), "%04x%08x",
           static_cast<unsigned int>(value >> 32),
           static_cast<unsigned int>(value));
  return String(text);
#endif
}

inline String shortDeviceId() {
  const String id = deviceId();
  return id.length() <= 6 ? id : id.substring(id.length() - 6);
}

inline uint32_t numericDeviceId() {
#if defined(ESP8266)
  return ESP.getChipId();
#else
  return static_cast<uint32_t>(ESP.getEfuseMac());
#endif
}

inline uint32_t random32() {
#if defined(ESP8266)
  return os_random();
#else
  return esp_random();
#endif
}

inline String resetReason() {
#if defined(ESP8266)
  return ESP.getResetReason();
#else
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:
      return "Power-on reset";
    case ESP_RST_EXT:
      return "External system reset";
    case ESP_RST_SW:
      return "Software/System restart";
    case ESP_RST_PANIC:
      return "Software panic";
    case ESP_RST_INT_WDT:
      return "Interrupt watchdog";
    case ESP_RST_TASK_WDT:
      return "Task watchdog";
    case ESP_RST_WDT:
      return "Other watchdog";
    case ESP_RST_DEEPSLEEP:
      return "Deep-sleep wake";
    case ESP_RST_BROWNOUT:
      return "Brownout reset";
    case ESP_RST_SDIO:
      return "SDIO reset";
    case ESP_RST_UNKNOWN:
    default:
      return "Unknown reset";
  }
#endif
}

inline uint32_t freeHeapBytes() { return ESP.getFreeHeap(); }

inline uint32_t minimumFreeHeapBytes() {
#if defined(ESP8266)
  // ESP8266 exposes no since-boot minimum, so WebApplication maintains the
  // observed low-water mark from startup using this current value.
  return ESP.getFreeHeap();
#else
  return ESP.getMinFreeHeap();
#endif
}

inline uint32_t largestFreeBlockBytes() {
#if defined(ESP8266)
  return ESP.getMaxFreeBlockSize();
#else
  return heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
#endif
}

inline uint8_t heapFragmentationPercent() {
#if defined(ESP8266)
  return ESP.getHeapFragmentation();
#else
  const uint32_t freeBytes = freeHeapBytes();
  const uint32_t largest = largestFreeBlockBytes();
  if (freeBytes == 0 || largest >= freeBytes) {
    return 0;
  }
  return static_cast<uint8_t>(100U -
                              static_cast<uint32_t>(largest * 100ULL /
                                                    freeBytes));
#endif
}

// Call only from loop() (including web handlers it runs). Both values are
// since-boot low-water marks of the Arduino loop stack, not current usage.
inline uint32_t loopStackMinimumFreeBytes() {
#if defined(ESP8266)
  return ESP.getFreeContStack();
#else
  // ESP-IDF counts StackType_t as bytes.
  return static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr));
#endif
}

inline uint32_t loopStackSizeBytes() {
#if defined(ESP8266)
  return CONT_STACKSIZE;
#else
  return static_cast<uint32_t>(getArduinoLoopTaskStackSize());
#endif
}

inline uint32_t sketchSizeBytes() { return ESP.getSketchSize(); }
inline uint32_t freeSketchSpaceBytes() { return ESP.getFreeSketchSpace(); }

inline uint32_t flashSizeBytes() {
#if defined(ESP8266)
  return ESP.getFlashChipRealSize();
#else
  return ESP.getFlashChipSize();
#endif
}

inline const char* flashLayout() { return Board::Flash::kLayout; }

inline const char* hardwareModel() { return Board::Profile::kHardwareModel; }

inline const char* chipFamily() { return Board::Profile::kChipFamily; }

inline const char* wifiPowerSaveMode() {
#if defined(ESP8266)
  switch (WiFi.getSleepMode()) {
    case WIFI_NONE_SLEEP: return "none";
    case WIFI_LIGHT_SLEEP: return "light";
    case WIFI_MODEM_SLEEP: return "min_modem";
    default: return "unknown";
  }
#else
  wifi_ps_type_t mode;
  if (esp_wifi_get_ps(&mode) != ESP_OK) return "unknown";
  switch (mode) {
    case WIFI_PS_NONE: return "none";
    case WIFI_PS_MIN_MODEM: return "min_modem";
    case WIFI_PS_MAX_MODEM: return "max_modem";
    default: return "unknown";
  }
#endif
}

inline const char* firmwareMarker() { return Board::Profile::kFirmwareMarker; }

inline const char* otaImageDescription() { return Board::Profile::kImageDescription; }

inline void configureStationHostname(const String& hostname) {
#if defined(ESP8266)
  WiFi.hostname(hostname.c_str());
#else
  WiFi.setHostname(hostname.c_str());
#endif
}

inline void stopMdns() {
#if defined(ESP8266)
  MDNS.close();
#else
  MDNS.end();
#endif
}

inline void serviceMdns() {
#if defined(ESP8266)
  MDNS.update();
#endif
}

inline bool scanNetworkIsOpen(int index) {
#if defined(ESP8266)
  return WiFi.encryptionType(index) == ENC_TYPE_NONE;
#else
  return WiFi.encryptionType(index) == WIFI_AUTH_OPEN;
#endif
}

}  // namespace Platform
