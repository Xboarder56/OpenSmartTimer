#include "TimerCommandAuthority.h"

#include <cstdio>
#include <cstring>

#include "Hardware.h"

namespace {
bool matches(const TimerPrecondition& precondition, uint32_t current) {
  switch (precondition.kind) {
    case TimerPrecondition::Kind::Absent:
      return true;
    case TimerPrecondition::Kind::Value:
      return precondition.value == current;
    case TimerPrecondition::Kind::Unmatchable:
    default:
      return false;
  }
}

bool emptyText(const char* text) { return text == nullptr || text[0] == '\0'; }
}  // namespace

TimerCommandAuthority::TimerCommandAuthority(TimerController& timer)
    : timer_(timer) {}

void TimerCommandAuthority::begin(uint32_t bootId) { bootId_ = bootId; }

TimerCommandOutcome TimerCommandAuthority::execute(const TimerCommand& command,
                                                   uint32_t nowMs) {
  TimerCommandResult failure = TimerCommandResult::Applied;
  if (!validParameters(command, failure)) {
    return {failure, false};
  }
  if (!matches(command.boot, bootId_)) {
    return {TimerCommandResult::StaleBoot, false};
  }
  if (command.op == TimerCommandOp::Start) {
    // A start targets "no active session"; it may not name an existing one.
    if (command.session.kind != TimerPrecondition::Kind::Absent &&
        !(command.session.kind == TimerPrecondition::Kind::Value &&
          command.session.value == 0)) {
      return {TimerCommandResult::StaleSession, false};
    }
  } else if (!matches(command.session, timer_.sessionSequence())) {
    return {TimerCommandResult::StaleSession, false};
  }

  const TimerSnapshot before = timer_.snapshot(nowMs);
  switch (command.op) {
    case TimerCommandOp::Start:
      return startTimer(command, nowMs);

    case TimerCommandOp::Release:
      if (before.state == TimerState::Open) {
        return {TimerCommandResult::Applied, false};
      }
      if (before.sealed) {
        return {TimerCommandResult::ControlsLocked, false};
      }
      if (!timer_.stop(false, nowMs)) {
        return {TimerCommandResult::InvalidState, false};
      }
      return {TimerCommandResult::Applied, true};

    case TimerCommandOp::Pause:
      if (before.state == TimerState::Paused) {
        return {TimerCommandResult::Applied, false};
      }
      if (!before.pauseAllowed) {
        return {TimerCommandResult::ControlDisabled, false};
      }
      if (before.sealed) {
        return {TimerCommandResult::ControlsLocked, false};
      }
      if (before.state != TimerState::Running || !timer_.pause(nowMs)) {
        return {TimerCommandResult::InvalidState, false};
      }
      return {TimerCommandResult::Applied, true};

    case TimerCommandOp::Resume:
      if (before.state == TimerState::Running) {
        return {TimerCommandResult::Applied, false};
      }
      if (before.sealed) {
        return {TimerCommandResult::ControlsLocked, false};
      }
      if (before.state != TimerState::Paused) {
        return {TimerCommandResult::InvalidState, false};
      }
      if (timer_.emergencyInputActive()) {
        return {TimerCommandResult::EmergencyInputActive, false};
      }
      if (!timer_.resume(nowMs)) {
        return {TimerCommandResult::InvalidState, false};
      }
      return {TimerCommandResult::Applied, true};

    case TimerCommandOp::Adjust:
      return adjustTimer(command.valueA, command.maxHoldMs, nowMs);

    case TimerCommandOp::LockControls:
      if (before.state != TimerState::Open && before.sealed) {
        return {TimerCommandResult::Applied, false};
      }
      if (!before.sealAllowed) {
        return {TimerCommandResult::ControlDisabled, false};
      }
      if (before.state != TimerState::Running ||
          !timer_.setSealed(true, nowMs)) {
        return {TimerCommandResult::InvalidState, false};
      }
      return {TimerCommandResult::Applied, true};

    case TimerCommandOp::Identify:
      if (before.state != TimerState::Open) {
        return {TimerCommandResult::InvalidState, false};
      }
      timer_.recordEvent(TimerEventType::IdentifyStarted, nowMs,
                         static_cast<int32_t>(kIdentifyDurationMs));
      return {TimerCommandResult::Applied, true};
  }
  return {TimerCommandResult::InvalidParameters, false};
}

TimerCommandOutcome TimerCommandAuthority::startTimer(
    const TimerCommand& command, uint32_t nowMs) {
  if (!timer_.isOpen()) {
    return {TimerCommandResult::InvalidState, false};
  }
  if (timer_.emergencyInputActive()) {
    return {TimerCommandResult::EmergencyInputActive, false};
  }
  uint32_t duration = static_cast<uint32_t>(command.valueA);
  if (command.valueB != 0) {
    const uint32_t span =
        static_cast<uint32_t>(command.valueB - command.valueA) + 1;
    duration += command.randomValue % span;
  }
  if (command.maxHoldMs && duration > command.maxHoldMs) {
    return {TimerCommandResult::MaximumDurationReached, false};
  }
  if (!timer_.start(duration, nowMs)) {
    return {timer_.emergencyInputActive()
                ? TimerCommandResult::EmergencyInputActive
                : TimerCommandResult::InvalidState,
            false};
  }
  return {TimerCommandResult::Applied, true};
}

TimerCommandOutcome TimerCommandAuthority::adjustTimer(int32_t deltaMs,
                                                       uint32_t maxHoldMs,
                                                       uint32_t nowMs) {
  const TimerSnapshot before = timer_.snapshot(nowMs);
  if (before.sealed) {
    return {TimerCommandResult::ControlsLocked, false};
  }
  if (before.state == TimerState::Open) {
    return {TimerCommandResult::InvalidState, false};
  }
  if (deltaMs == 0) {
    return {TimerCommandResult::Applied, false};
  }
  if (deltaMs > 0 && timer_.emergencyInputActive()) {
    // Shortening stays allowed: it moves toward release.
    return {TimerCommandResult::EmergencyInputActive, false};
  }
  if (deltaMs > 0 && static_cast<int64_t>(before.remainingMs) + deltaMs >
                         Hardware::kMaximumDurationMs) {
    return {TimerCommandResult::MaximumDurationReached, false};
  }
  if (deltaMs > 0 && maxHoldMs &&
      static_cast<int64_t>(before.remainingMs) + deltaMs > maxHoldMs) {
    return {TimerCommandResult::MaximumDurationReached, false};
  }
  if (!timer_.adjust(deltaMs, nowMs)) {
    return {TimerCommandResult::InvalidState, false};
  }
  return {TimerCommandResult::Applied, true};
}

bool TimerCommandAuthority::validParameters(const TimerCommand& command,
                                            TimerCommandResult& failure) {
  constexpr int32_t kMaximum =
      static_cast<int32_t>(Hardware::kMaximumDurationMs);
  switch (command.op) {
    case TimerCommandOp::Start: {
      const bool randomRange = command.valueB != 0;
      if (command.valueA <= 0 || command.valueA > kMaximum ||
          (randomRange && (command.valueB < command.valueA ||
                           command.valueB > kMaximum))) {
        failure = TimerCommandResult::InvalidDuration;
        return false;
      }
      return true;
    }
    case TimerCommandOp::Adjust:
      if (command.valueB != 0 || command.valueA < -kMaximum ||
          command.valueA > kMaximum) {
        failure = TimerCommandResult::InvalidDuration;
        return false;
      }
      return true;
    case TimerCommandOp::Release:
    case TimerCommandOp::Pause:
    case TimerCommandOp::Resume:
    case TimerCommandOp::LockControls:
    case TimerCommandOp::Identify:
      if (command.valueA != 0 || command.valueB != 0) {
        failure = TimerCommandResult::InvalidParameters;
        return false;
      }
      return true;
  }
  failure = TimerCommandResult::InvalidParameters;
  return false;
}

TimerPrecondition TimerCommandAuthority::bootFromText(const char* text) const {
  if (emptyText(text)) {
    return TimerPrecondition::absent();
  }
  char current[9];
  snprintf(current, sizeof(current), "%lx",
           static_cast<unsigned long>(bootId_));
  return strcmp(text, current) == 0 ? TimerPrecondition::of(bootId_)
                                    : TimerPrecondition::unmatchable();
}

TimerPrecondition TimerCommandAuthority::sessionFromText(
    const char* text) const {
  if (emptyText(text)) {
    return TimerPrecondition::absent();
  }
  char current[kSessionTextSize];
  sessionText(current, sizeof(current));
  return current[0] != '\0' && strcmp(text, current) == 0
             ? TimerPrecondition::of(timer_.sessionSequence())
             : TimerPrecondition::unmatchable();
}

void TimerCommandAuthority::sessionText(char* output, size_t size) const {
  if (output == nullptr || size == 0) {
    return;
  }
  if (timer_.sessionSequence() == 0) {
    output[0] = '\0';
    return;
  }
  snprintf(output, size, "%lx-%lu", static_cast<unsigned long>(bootId_),
           static_cast<unsigned long>(timer_.sessionSequence()));
}

const char* TimerCommandAuthority::resultName(TimerCommandResult result) {
  switch (result) {
    case TimerCommandResult::Applied:
      return "applied";
    case TimerCommandResult::InvalidParameters:
      return "invalid_parameters";
    case TimerCommandResult::StaleBoot:
      return "stale_boot";
    case TimerCommandResult::StaleSession:
      return "stale_session";
    case TimerCommandResult::InvalidState:
      return "invalid_state";
    case TimerCommandResult::InvalidDuration:
      return "invalid_duration";
    case TimerCommandResult::EmergencyInputActive:
      return "emergency_input_active";
    case TimerCommandResult::ControlsLocked:
      return "controls_locked";
    case TimerCommandResult::MaximumDurationReached:
      return "maximum_duration_reached";
    case TimerCommandResult::ControlDisabled:
      return "control_disabled";
    default:
      return "unknown";
  }
}

TimerOperationLedger::Match TimerOperationLedger::match(
    const char* operationId, TimerCommandOp op, int32_t valueA,
    int32_t valueB) const {
  if (!usable(operationId)) {
    return Match::New;
  }
  for (const Entry& entry : entries_) {
    if (entry.operationId[0] != '\0' &&
        strcmp(operationId, entry.operationId) == 0) {
      return entry.op == op && entry.valueA == valueA &&
                     entry.valueB == valueB
                 ? Match::Duplicate
                 : Match::Conflict;
    }
  }
  return Match::New;
}

void TimerOperationLedger::remember(const char* operationId,
                                    TimerCommandOp op, int32_t valueA,
                                    int32_t valueB) {
  if (!usable(operationId)) {
    return;
  }
  Entry& entry = entries_[cursor_];
  strncpy(entry.operationId, operationId, kMaximumIdLength);
  entry.operationId[kMaximumIdLength] = '\0';
  entry.op = op;
  entry.valueA = valueA;
  entry.valueB = valueB;
  cursor_ = static_cast<uint8_t>((cursor_ + 1) % kCapacity);
}

void TimerOperationLedger::clear() {
  for (Entry& entry : entries_) {
    entry = Entry{};
  }
  cursor_ = 0;
}

bool TimerOperationLedger::usable(const char* operationId) {
  if (operationId == nullptr || operationId[0] == '\0') {
    return false;
  }
  for (size_t length = 0; length <= kMaximumIdLength; ++length) {
    if (operationId[length] == '\0') {
      return true;
    }
  }
  return false;
}
