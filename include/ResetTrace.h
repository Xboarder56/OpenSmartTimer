#pragma once

// Where the firmware was when the chip last reset. The record lives in memory
// that a watchdog reset leaves intact, so a hang can be located afterwards
// from /api/v1/system/diagnostics. Writes are a few register stores.
#include <Arduino.h>
#include <cstring>
#if defined(ESP8266)
#include <user_interface.h>
#else
#include <esp_attr.h>
#endif

namespace ResetTrace {

enum Phase : uint8_t {
  kNone, kInputs, kTimer, kNetwork, kNetworkDns, kNetworkScan, kNetworkStation,
  kWeb, kWebRequest, kWebAfter, kWebStreams, kBluetooth, kRelay, kLoopEnd,
  kNetworkMdns,
  // Appended so stored traces from earlier builds keep their meaning.
  kBluetoothStart, kBootValidation,
};

struct Record {
  uint32_t magic;
  uint32_t atMs;
  uint8_t phase;
  uint8_t reserved[3];
  char uri[20];
};
static_assert(sizeof(Record) == 32, "RTC record must stay 32 bytes");
constexpr uint32_t kMagic = 0x5254534FUL;  // "OSTR"

#if defined(ESP8266)
// RTC user memory block 96 (byte 384): clear of the bootloader's update
// command at the start of user memory.
constexpr uint32_t kRtcBlock = 96;
inline void store(const Record& record) {
  ESP.rtcUserMemoryWrite(kRtcBlock, reinterpret_cast<uint32_t*>(const_cast<Record*>(&record)), sizeof(record));
}
inline bool load(Record& record) {
  return ESP.rtcUserMemoryRead(kRtcBlock, reinterpret_cast<uint32_t*>(&record), sizeof(record));
}
#else
inline Record& storage() {
  RTC_NOINIT_ATTR static Record record;
  return record;
}
inline void store(const Record& record) { storage() = record; }
inline bool load(Record& record) { record = storage(); return true; }
#endif

// The longest gap between two marks since boot, and which phase it was in.
// A long gap means loop() did not come round to service timer expiry; the
// emergency input does not wait for it (an interrupt cuts the lock output).
struct Stall {
  uint32_t longestMs;
  uint32_t atMs;  // uptime when that gap ended
  uint32_t overSecondCount;
  uint8_t phase;
  char uri[20];
};

inline Stall& stall() {
  static Stall record{0, 0, 0, 0, {0}};
  return record;
}

inline Record& current() {
  static Record record{kMagic, 0, kNone, {0, 0, 0}, {0}};
  return record;
}
inline Record& previous() {
  static Record record{};
  return record;
}

// Call once in setup(): keeps the last run's record, then starts a new one.
inline void begin() {
  Record last{};
  if (load(last) && last.magic == kMagic) {
    last.uri[sizeof(last.uri) - 1] = '\0';
    previous() = last;
  }
  store(current());
}

inline void noteGap(const Record& record, uint32_t nowMs) {
  if (record.phase == kNone) return;  // setup() is not a loop pass
  const uint32_t gap = nowMs - record.atMs;
  Stall& worst = stall();
  if (gap >= 1000 && worst.overSecondCount < UINT32_MAX) ++worst.overSecondCount;
  if (gap <= worst.longestMs) return;
  worst.longestMs = gap;
  worst.atMs = nowMs;
  worst.phase = record.phase;
  const bool web = record.phase == kWebRequest || record.phase == kWebAfter;
  memcpy(worst.uri, web ? record.uri : "", web ? sizeof(worst.uri) : 1);
}

inline void mark(Phase phase) {
  Record& record = current();
  const uint32_t nowMs = millis();
  noteGap(record, nowMs);
  record.phase = phase;
  record.atMs = nowMs;
  store(record);
}

inline void request(const char* uri) {
  Record& record = current();
  const uint32_t nowMs = millis();
  noteGap(record, nowMs);
  record.phase = kWebRequest;
  record.atMs = nowMs;
  // Printable ASCII only, so a cut multi-byte path never yields invalid JSON.
  size_t i = 0;
  for (; uri && uri[i] && i < sizeof(record.uri) - 1; ++i) {
    const char c = uri[i];
    record.uri[i] = (c >= 0x20 && c < 0x7f) ? c : '?';
  }
  record.uri[i] = '\0';
  store(record);
}

inline const char* phaseName(uint8_t phase) {
  switch (phase) {
    case kInputs: return "inputs";
    case kTimer: return "timer";
    case kNetwork: return "network";
    case kNetworkDns: return "network_dns";
    case kNetworkScan: return "network_scan";
    case kNetworkStation: return "network_station";
    case kWeb: return "web";
    case kWebRequest: return "web_request";
    case kWebAfter: return "web_after";
    case kWebStreams: return "web_streams";
    case kBluetooth: return "bluetooth";
    case kRelay: return "relay";
    case kLoopEnd: return "loop_end";
    case kNetworkMdns: return "network_mdns";
    case kBluetoothStart: return "bluetooth_start";
    case kBootValidation: return "boot_validation";
    default: return "none";
  }
}

}  // namespace ResetTrace
