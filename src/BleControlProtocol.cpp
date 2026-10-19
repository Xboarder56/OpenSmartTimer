#include "BleControlProtocol.h"

#include <cstring>


BleControlProtocol::BleControlProtocol(TimerController& timer)
    : timer_(timer), authority_(timer) {}

void BleControlProtocol::begin(uint32_t bootId) {
  bootId_ = bootId;
  authority_.begin(bootId);
  for (auto& slot : recent_) {
    for (RecentCommand& entry : slot) entry = RecentCommand{};
  }
  memset(recentCursor_, 0, sizeof(recentCursor_));
  memset(recentAppIds_, 0, sizeof(recentAppIds_));
}

BleControlExecution BleControlProtocol::execute(const uint8_t* frame,
                                                bool authorized,
                                                uint32_t nowMs,
                                                uint32_t randomValue,
                                                bool mutationsAllowed,
                                                uint32_t appId, uint8_t appSlot) {
  if (frame == nullptr || frame[0] != kProtocolVersion) {
    return result(frame, BleControlResultCode::InvalidFrame, false, false,
                  nowMs);
  }

  const uint16_t requestId = readUint16Le(frame + 2);
  if (requestId == 0) {
    return result(frame, BleControlResultCode::InvalidFrame, false, false,
                  nowMs);
  }
  if (!authorized || !appId || appSlot >= BleAppRegistry::kCapacity) {
    return result(frame, BleControlResultCode::Unauthorized, false, false,
                  nowMs);
  }

  if (recentAppIds_[appSlot] != appId) {
    for (RecentCommand& entry : recent_[appSlot]) entry = RecentCommand{};
    recentCursor_[appSlot] = 0;
    recentAppIds_[appSlot] = appId;
  }
  const int8_t priorIndex = findRecent(requestId, appSlot);
  if (priorIndex >= 0) {
    const RecentCommand& prior = recent_[appSlot][priorIndex];
    if (memcmp(prior.request, frame, kSemanticCommandSize) != 0) {
      return result(frame, BleControlResultCode::OperationConflict, false,
                    false, nowMs);
    }
    BleControlExecution duplicate{};
    memcpy(duplicate.frame, prior.response, kResultFrameSize);
    duplicate.frame[5] |= 0x02;
    duplicate.code = static_cast<BleControlResultCode>(duplicate.frame[4]);
    duplicate.changed = (duplicate.frame[5] & 0x01) != 0;
    duplicate.duplicate = true;
    return duplicate;
  }

  // The caller owns the shared REST/BLE system-transition gate. Check it
  // after exact-retry handling so a lost response remains unambiguous, but
  // before any new timer mutation is inspected or remembered.
  if (!mutationsAllowed) {
    return result(frame, BleControlResultCode::SystemTransition, false, false,
                  nowMs);
  }

  const uint32_t requestedBootId = readUint32Le(frame + 4);
  const uint32_t requestedSession = readUint32Le(frame + 8);
  TimerCommand command;
  BleControlResultCode code = BleControlResultCode::Applied;
  bool changed = false;
  if (!commandOp(static_cast<BleControlOpcode>(frame[1]), command.op)) {
    code = BleControlResultCode::UnsupportedOpcode;
  } else {
    command.valueA = static_cast<int32_t>(readUint32Le(frame + 12));
    command.valueB = static_cast<int32_t>(readUint32Le(frame + 16));
    command.boot = TimerPrecondition::of(requestedBootId);
    command.session = TimerPrecondition::of(requestedSession);
    command.randomValue = randomValue;
    const TimerCommandOutcome outcome = authority_.execute(command, nowMs);
    code = resultCode(outcome.result);
    changed = outcome.changed;
  }

  BleControlExecution execution = result(frame, code, changed, false, nowMs);
  remember(frame, execution.frame, requestId, appSlot);
  return execution;
}

const char* BleControlProtocol::resultName(BleControlResultCode code) {
  switch (code) {
    case BleControlResultCode::Applied:
      return "applied";
    case BleControlResultCode::InvalidFrame:
      return "invalid_frame";
    case BleControlResultCode::Unauthorized:
      return "unauthorized";
    case BleControlResultCode::StaleBoot:
      return "stale_boot";
    case BleControlResultCode::StaleSession:
      return "stale_session";
    case BleControlResultCode::InvalidState:
      return "invalid_state";
    case BleControlResultCode::InvalidDuration:
      return "invalid_duration";
    case BleControlResultCode::EmergencyInputActive:
      return "emergency_input_active";
    case BleControlResultCode::ControlsLocked:
      return "controls_locked";
    case BleControlResultCode::MaximumDurationReached:
      return "maximum_duration_reached";
    case BleControlResultCode::UnsupportedOpcode:
      return "unsupported_opcode";
    case BleControlResultCode::OperationConflict:
      return "operation_conflict";
    case BleControlResultCode::SystemTransition:
      return "system_transition";
    case BleControlResultCode::ControlDisabled:
      return "control_disabled";
    default:
      return "unknown";
  }
}

BleControlExecution BleControlProtocol::result(
    const uint8_t* request, BleControlResultCode code, bool changed,
    bool duplicate, uint32_t nowMs) const {
  BleControlExecution execution{};
  execution.code = code;
  execution.changed = changed;
  execution.duplicate = duplicate;
  execution.frame[0] = kProtocolVersion;
  execution.frame[1] = request == nullptr ? 0 : request[1];
  if (request != nullptr) {
    execution.frame[2] = request[2];
    execution.frame[3] = request[3];
  }
  execution.frame[4] = static_cast<uint8_t>(code);
  execution.frame[5] = (changed ? 0x01 : 0) | (duplicate ? 0x02 : 0);
  const TimerSnapshot snapshot = timer_.snapshot(nowMs);
  execution.frame[6] = stateCode(snapshot.state);
  execution.frame[7] = releaseReasonCode(snapshot.lastReleaseReason);
  writeUint32Le(execution.frame + 8, bootId_);
  writeUint32Le(execution.frame + 12, timer_.sessionSequence());
  writeUint32Le(execution.frame + 16, snapshot.remainingMs);
  return execution;
}

int8_t BleControlProtocol::findRecent(uint16_t requestId, uint8_t slot) const {
  for (uint8_t index = 0; index < kRecentCapacity; ++index) {
    if (recent_[slot][index].used && recent_[slot][index].requestId == requestId) {
      return static_cast<int8_t>(index);
    }
  }
  return -1;
}

void BleControlProtocol::remember(const uint8_t* request,
                                  const uint8_t* response,
                                  uint16_t requestId, uint8_t slot) {
  RecentCommand& entry = recent_[slot][recentCursor_[slot]];
  memcpy(entry.request, request, kSemanticCommandSize);
  memcpy(entry.response, response, kResultFrameSize);
  entry.requestId = requestId;
  entry.used = true;
  recentCursor_[slot] = (recentCursor_[slot] + 1) % kRecentCapacity;
}

bool BleControlProtocol::commandOp(BleControlOpcode opcode,
                                   TimerCommandOp& op) {
  switch (opcode) {
    case BleControlOpcode::Start:
      op = TimerCommandOp::Start;
      return true;
    case BleControlOpcode::Release:
      op = TimerCommandOp::Release;
      return true;
    case BleControlOpcode::Pause:
      op = TimerCommandOp::Pause;
      return true;
    case BleControlOpcode::Resume:
      op = TimerCommandOp::Resume;
      return true;
    case BleControlOpcode::Adjust:
      op = TimerCommandOp::Adjust;
      return true;
    case BleControlOpcode::LockControls:
      op = TimerCommandOp::LockControls;
      return true;
    case BleControlOpcode::Identify:
      op = TimerCommandOp::Identify;
      return true;
    case BleControlOpcode::Authenticate:
    case BleControlOpcode::PairingCommit:
    default:
      return false;
  }
}

BleControlResultCode BleControlProtocol::resultCode(TimerCommandResult result) {
  switch (result) {
    case TimerCommandResult::Applied:
      return BleControlResultCode::Applied;
    case TimerCommandResult::StaleBoot:
      return BleControlResultCode::StaleBoot;
    case TimerCommandResult::StaleSession:
      return BleControlResultCode::StaleSession;
    case TimerCommandResult::InvalidState:
      return BleControlResultCode::InvalidState;
    case TimerCommandResult::InvalidDuration:
      return BleControlResultCode::InvalidDuration;
    case TimerCommandResult::EmergencyInputActive:
      return BleControlResultCode::EmergencyInputActive;
    case TimerCommandResult::ControlsLocked:
      return BleControlResultCode::ControlsLocked;
    case TimerCommandResult::MaximumDurationReached:
      return BleControlResultCode::MaximumDurationReached;
    case TimerCommandResult::ControlDisabled:
      return BleControlResultCode::ControlDisabled;
    case TimerCommandResult::InvalidParameters:
    default:
      return BleControlResultCode::InvalidFrame;
  }
}

uint16_t BleControlProtocol::readUint16Le(const uint8_t* source) {
  return static_cast<uint16_t>(source[0]) |
         static_cast<uint16_t>(source[1]) << 8;
}

uint32_t BleControlProtocol::readUint32Le(const uint8_t* source) {
  return static_cast<uint32_t>(source[0]) |
         static_cast<uint32_t>(source[1]) << 8 |
         static_cast<uint32_t>(source[2]) << 16 |
         static_cast<uint32_t>(source[3]) << 24;
}

void BleControlProtocol::writeUint32Le(uint8_t* destination, uint32_t value) {
  destination[0] = static_cast<uint8_t>(value);
  destination[1] = static_cast<uint8_t>(value >> 8);
  destination[2] = static_cast<uint8_t>(value >> 16);
  destination[3] = static_cast<uint8_t>(value >> 24);
}

uint8_t BleControlProtocol::stateCode(TimerState state) {
  switch (state) {
    case TimerState::Running:
      return 1;
    case TimerState::Paused:
      return 2;
    case TimerState::Open:
    default:
      return 0;
  }
}

uint8_t BleControlProtocol::releaseReasonCode(ReleaseReason reason) {
  switch (reason) {
    case ReleaseReason::Expired:
      return 1;
    case ReleaseReason::User:
      return 2;
    case ReleaseReason::Emergency:
      return 3;
    case ReleaseReason::AdjustedToZero:
      return 4;
    case ReleaseReason::ShareLinkExpired:
      return 5;
    case ReleaseReason::Boot:
    default:
      return 0;
  }
}
