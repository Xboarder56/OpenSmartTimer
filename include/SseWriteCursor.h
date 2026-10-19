#pragma once

#include <cstddef>
#include <cstdint>

// Transport-independent partial-write/deadline bookkeeping, exercised by host
// tests. The caller retains immutable wire bytes until Complete or failure.
class SseWriteCursor {
 public:
  enum class Result { Pending, Complete, Failed, TimedOut };
  static constexpr size_t kBytesPerLoop = 512;
  void begin(size_t length, uint32_t nowMs) {
    length_ = length; offset_ = 0; startedAtMs_ = nowMs;
  }
  bool active() const { return offset_ < length_; }
  size_t offset() const { return offset_; }
  template <typename Write>
  Result pump(uint32_t nowMs, uint32_t timeoutMs, Write write) {
    if (!active()) return Result::Complete;
    if (static_cast<uint32_t>(nowMs - startedAtMs_) >= timeoutMs) return Result::TimedOut;
    const size_t remaining = length_ - offset_;
    const size_t requested = remaining < kBytesPerLoop ? remaining : kBytesPerLoop;
    const int sent = write(offset_, requested);
    if (sent < 0 || static_cast<size_t>(sent) > requested) return Result::Failed;
    offset_ += static_cast<size_t>(sent);
    return active() ? Result::Pending : Result::Complete;
  }
 private:
  size_t length_ = 0;
  size_t offset_ = 0;
  uint32_t startedAtMs_ = 0;
};
