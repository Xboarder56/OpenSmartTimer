#pragma once

#include <cstdint>

// Host clock for ResetTrace tests.
inline uint32_t& hostMillis() {
  static uint32_t now = 0;
  return now;
}
inline uint32_t millis() { return hostMillis(); }
