#pragma once
#include <array>
#include <cassert>
#include <cstring>
namespace fake { inline std::array<uint8_t, 512> eeprom{}; inline bool failEepromCommit = false; }
class EEPROMClass {
 public:
  explicit EEPROMClass(const char*) {}
  EEPROMClass() = default;
  bool begin(size_t size) { assert(size == 512); staged_ = fake::eeprom; return true; }
  template<class T> void get(size_t offset, T& value) { memcpy(&value, staged_.data() + offset, sizeof(value)); }
  template<class T> void put(size_t offset, const T& value) { memcpy(staged_.data() + offset, &value, sizeof(value)); }
  bool commit() { if (fake::failEepromCommit) return false; fake::eeprom = staged_; return true; }
 private:
  std::array<uint8_t, 512> staged_{};
};
