#pragma once

#include <cstddef>

#include <Arduino.h>

#include "TimerController.h"

// Timer mutations every remote transport may request. REST, BLE and the
// future relay adapter translate their own wire formats into these.
enum class TimerCommandOp : uint8_t {
  Start,
  Release,
  Pause,
  Resume,
  Adjust,
  LockControls,
  Identify,
};

enum class TimerCommandResult : uint8_t {
  Applied,
  InvalidParameters,
  StaleBoot,
  StaleSession,
  InvalidState,
  InvalidDuration,
  EmergencyInputActive,
  ControlsLocked,
  MaximumDurationReached,
  // The owner switched this control off in the timer's settings.
  ControlDisabled,
};

// A boot or session precondition as the transport received it. Unmatchable
// means the caller supplied text that cannot describe the current boot or
// session (another boot, malformed, non-canonical); it is rejected as stale
// rather than ignored.
struct TimerPrecondition {
  enum class Kind : uint8_t { Absent, Value, Unmatchable };
  Kind kind = Kind::Absent;
  uint32_t value = 0;

  static TimerPrecondition absent() { return {}; }
  static TimerPrecondition of(uint32_t value) { return {Kind::Value, value}; }
  static TimerPrecondition unmatchable() { return {Kind::Unmatchable, 0}; }
};

struct TimerCommand {
  TimerCommandOp op = TimerCommandOp::Identify;
  // Start: fixed duration, or random-range minimum when valueB is nonzero.
  // Adjust: signed delta. Every other operation requires zero.
  int32_t valueA = 0;
  // Start: inclusive random-range maximum, or zero for a fixed duration.
  int32_t valueB = 0;
  TimerPrecondition boot;
  // Session sequence. A Start may only be absent or claim sequence zero.
  TimerPrecondition session;
  // Caller-supplied hardware randomness for random-range starts.
  uint32_t randomValue = 0;
  // Nonzero for share-link commands: the session may not end later than this
  // many ms from now (the link's remaining lifetime). Checked with the 24-hour
  // rule, so it never changes the rule order. REST and BLE leave it zero.
  uint32_t maxHoldMs = 0;
};

struct TimerCommandOutcome {
  TimerCommandResult result = TimerCommandResult::Applied;
  bool changed = false;
};

// The single mutation policy shared by every control transport. Transports
// own authentication, framing, operation deduplication and the system
// transition gate; this class owns everything that decides whether a
// requested timer change is allowed and applies it. Checks run in one order:
//
//   1. parameters (range and shape)
//   2. boot precondition, then session precondition
//   3. requested state already true -> accepted no-op (changed=false)
//   3a. pause or control lock switched off by the owner
//   4. software controls lock
//   5. timer state (open/running/paused)
//   6. emergency input for start, resume and positive adjust
//   7. 24-hour maximum for positive adjust
//
// Call only from the loop task that owns TimerController.
class TimerCommandAuthority {
 public:
  static constexpr uint32_t kIdentifyDurationMs = 5000;
  // Longest session text: 8 hex digits, '-', 10 decimal digits, NUL.
  static constexpr size_t kSessionTextSize = 20;

  explicit TimerCommandAuthority(TimerController& timer);

  void begin(uint32_t bootId);
  uint32_t bootId() const { return bootId_; }

  TimerCommandOutcome execute(const TimerCommand& command, uint32_t nowMs);

  // Text forms shared by REST and the relay: boot "7ca9f21b" (lowercase hex,
  // no leading zeros) and session "7ca9f21b-3". Empty or null text is absent;
  // anything else must equal the current value exactly.
  TimerPrecondition bootFromText(const char* text) const;
  TimerPrecondition sessionFromText(const char* text) const;
  // Writes "" before the first session of a boot.
  void sessionText(char* output, size_t size) const;

  static const char* resultName(TimerCommandResult result);

 private:
  TimerCommandOutcome startTimer(const TimerCommand& command, uint32_t nowMs);
  TimerCommandOutcome adjustTimer(int32_t deltaMs, uint32_t maxHoldMs, uint32_t nowMs);
  static bool validParameters(const TimerCommand& command,
                              TimerCommandResult& failure);

  TimerController& timer_;
  uint32_t bootId_ = 0;
};

// Remembers the most recent successful string operation IDs for one
// transport, so a retried request after a lost response is reported as a
// duplicate instead of applied twice. RAM only; reset at boot.
class TimerOperationLedger {
 public:
  static constexpr uint8_t kCapacity = 8;
  static constexpr size_t kMaximumIdLength = 64;

  enum class Match : int8_t { Conflict = -1, New = 0, Duplicate = 1 };

  // An empty or over-long ID is always New and is never remembered.
  Match match(const char* operationId, TimerCommandOp op, int32_t valueA,
              int32_t valueB) const;
  void remember(const char* operationId, TimerCommandOp op, int32_t valueA,
                int32_t valueB);
  void clear();

 private:
  struct Entry {
    char operationId[kMaximumIdLength + 1]{};
    TimerCommandOp op = TimerCommandOp::Start;
    int32_t valueA = 0;
    int32_t valueB = 0;
  };

  static bool usable(const char* operationId);

  Entry entries_[kCapacity]{};
  uint8_t cursor_ = 0;
};
