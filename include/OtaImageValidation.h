#pragma once

#include <cstddef>
#include <cstdint>
#include "BoardProfile.h"

namespace OtaImageValidation {

#if defined(SMART_TIMER_HARDWARE_V2)
constexpr size_t kRequiredHeaderLength = Board::Flash::kHeaderLength;

inline bool matchesTarget(const uint8_t* header, size_t length) {
  if (header == nullptr || length < kRequiredHeaderLength) {
    return false;
  }
  const uint16_t chipId = static_cast<uint16_t>(header[12]) |
                          static_cast<uint16_t>(header[13]) << 8;
  return header[0] == 0xE9 && header[2] == Board::Flash::kImageMode &&
         header[3] == Board::Flash::kImageSizeFrequency && chipId == Board::Flash::kImageChipId;
}

inline const char* mismatchMessage() {
  return Board::Flash::kMismatch;
}
#else
constexpr size_t kRequiredHeaderLength = Board::Flash::kHeaderLength;

inline bool matchesTarget(const uint8_t* header, size_t length) {
  return header != nullptr && length >= kRequiredHeaderLength &&
         header[0] == 0xE9 && header[2] == Board::Flash::kImageMode && header[3] == Board::Flash::kImageSizeFrequency;
}

inline const char* mismatchMessage() {
  return Board::Flash::kMismatch;
}
#endif

}  // namespace OtaImageValidation
