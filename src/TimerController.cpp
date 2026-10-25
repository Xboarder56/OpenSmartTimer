#include "TimerController.h"

#include "DeadlineBackstop.h"
#include "Hardware.h"

void TimerController::begin() {
  state_ = TimerState::Open;
  deadlineMs_ = 0;
  pausedRemainingMs_ = 0;
  sessionDurationMs_ = 0;
  sealed_ = false;
  lastReleaseReason_ = ReleaseReason::Boot;
  eventStart_ = 0;
  eventCount_ = 0;
  eventRevision_ = 0;
  sessionSequence_ = 0;
  Hardware::beginFailOpen();
  DeadlineBackstop::cancel();
  emergencyInputActive_ = Hardware::emergencyInputActive();
  recordEvent(TimerEventType::Boot, 0, 0);
  if (emergencyInputActive_) {
    recordEvent(TimerEventType::EmergencyInputActivated, 0, 0);
  }
}

void TimerController::tick(uint32_t nowMs) {
  if (state_ == TimerState::Running &&
      static_cast<int32_t>(deadlineMs_ - nowMs) <= 0) {
    release(ReleaseReason::Expired, nowMs);
    return;
  }
  // Fail open when the share link holding this session expires, whether the
  // session is running, paused or sealed, and whether or not the relay is
  // reachable: the limit lives here, not in the relay.
  if (state_ != TimerState::Open && holdLimited_ &&
      static_cast<int32_t>(holdLimitMs_ - nowMs) <= 0) {
    release(ReleaseReason::ShareLinkExpired, nowMs);
    return;
  }
  // The backstop has already driven the output inactive because this loop
  // pass came late. Record the release it stood in for.
  if (state_ != TimerState::Open && DeadlineBackstop::fired()) {
    release(holdLimitFirst() ? ReleaseReason::ShareLinkExpired
                             : ReleaseReason::Expired,
            nowMs);
  }
}

bool TimerController::holdLimitFirst() const {
  return holdLimited_ &&
         (state_ == TimerState::Paused ||
          static_cast<int32_t>(holdLimitMs_ - deadlineMs_) < 0);
}

void TimerController::settleBackstop(uint32_t nowMs) {
  if (DeadlineBackstop::fired()) tick(nowMs);
}

void TimerController::armBackstop() {
  if (state_ == TimerState::Open ||
      (state_ == TimerState::Paused && !holdLimited_)) {
    DeadlineBackstop::cancel();
    return;
  }
  DeadlineBackstop::armAt(holdLimitFirst() ? holdLimitMs_ : deadlineMs_);
}

void TimerController::limitHoldUntil(uint32_t untilMs) {
  if (state_ == TimerState::Open) return;
  if (!holdLimited_ || static_cast<int32_t>(untilMs - holdLimitMs_) > 0) {
    holdLimitMs_ = untilMs;
  }
  holdLimited_ = true;
  armBackstop();
}

void TimerController::setEmergencyInputActive(bool active, uint32_t nowMs) {
  if (active == emergencyInputActive_) {
    return;
  }
  emergencyInputActive_ = active;
  recordEvent(active ? TimerEventType::EmergencyInputActivated
                     : TimerEventType::EmergencyInputCleared,
              nowMs);
  if (active && state_ != TimerState::Open) {
    release(ReleaseReason::Emergency, nowMs);
  }
}

bool TimerController::emergencyInputActive() const {
  // A raw HIGH rejects an energizing transition immediately. The debounced
  // value remains latched long enough to cover contact bounce.
  // An unhandled interrupt edge also counts, so nothing can start, resume or
  // extend between the interrupt and loop()'s release.
  return emergencyInputActive_ || Hardware::emergencyInputActive() ||
         Hardware::emergencyLatchPending();
}

bool TimerController::start(uint32_t durationMs, uint32_t nowMs) {
  if (state_ != TimerState::Open || durationMs == 0 ||
      durationMs > Hardware::kMaximumDurationMs || emergencyInputActive()) {
    return false;
  }
  state_ = TimerState::Running;
  ++sessionSequence_;
  if (sessionSequence_ == 0) {
    sessionSequence_ = 1;
  }
  deadlineMs_ = nowMs + durationMs;
  pausedRemainingMs_ = 0;
  sessionDurationMs_ = durationMs;
  sealed_ = false;
  holdLimited_ = false;
  holdLimitMs_ = 0;
  Hardware::writeLock(true);
  recordEvent(TimerEventType::Started, nowMs,
              static_cast<int32_t>(durationMs));
  // An emergency edge between the check above and writeLock(true) has
  // already driven the output off in the interrupt, and the write just
  // turned it back on. Release now rather than on the next loop pass.
  if (emergencyInputActive()) {
    release(ReleaseReason::Emergency, nowMs);
    return false;
  }
  armBackstop();
  return true;
}

bool TimerController::setRemaining(uint32_t durationMs, uint32_t nowMs) {
  settleBackstop(nowMs);
  if (durationMs == 0 || durationMs > Hardware::kMaximumDurationMs || sealed_ ||
      emergencyInputActive()) {
    return false;
  }
  if (state_ == TimerState::Open) {
    return start(durationMs, nowMs);
  }
  if (state_ == TimerState::Paused) {
    pausedRemainingMs_ = durationMs;
  } else {
    deadlineMs_ = nowMs + durationMs;
  }
  sessionDurationMs_ = durationMs;
  recordEvent(TimerEventType::RemainingSet, nowMs,
              static_cast<int32_t>(durationMs));
  armBackstop();
  return true;
}

bool TimerController::stop(bool physicalEmergency, uint32_t nowMs) {
  settleBackstop(nowMs);
  if (state_ == TimerState::Open) {
    return true;
  }
  if (sealed_ && !physicalEmergency) {
    return false;
  }
  release(physicalEmergency ? ReleaseReason::Emergency : ReleaseReason::User,
          nowMs);
  return true;
}

bool TimerController::pause(uint32_t nowMs) {
  // The owner's switches are enforced here too, so legacy routes that call
  // the controller directly cannot bypass them.
  settleBackstop(nowMs);
  if (state_ != TimerState::Running || sealed_ || !pauseAllowed_) {
    return false;
  }
  pausedRemainingMs_ = remainingAt(nowMs);
  if (pausedRemainingMs_ == 0) {
    release(ReleaseReason::Expired, nowMs);
    return false;
  }
  state_ = TimerState::Paused;
  recordEvent(TimerEventType::Paused, nowMs,
              static_cast<int32_t>(pausedRemainingMs_));
  armBackstop();
  return true;
}

bool TimerController::resume(uint32_t nowMs) {
  settleBackstop(nowMs);
  if (state_ != TimerState::Paused || sealed_ || pausedRemainingMs_ == 0 ||
      emergencyInputActive()) {
    return false;
  }
  deadlineMs_ = nowMs + pausedRemainingMs_;
  pausedRemainingMs_ = 0;
  state_ = TimerState::Running;
  recordEvent(TimerEventType::Resumed, nowMs,
              static_cast<int32_t>(remainingAt(nowMs)));
  armBackstop();
  return true;
}

bool TimerController::adjust(int32_t deltaMs, uint32_t nowMs) {
  settleBackstop(nowMs);
  if (state_ == TimerState::Open || sealed_) {
    return false;
  }
  if (deltaMs == 0) {
    return true;
  }
  // Like start/resume/setRemaining, never lengthen a session while the
  // emergency input is asserted (raw HIGH can precede the debounced release).
  // Shortening remains allowed: it moves toward release.
  if (deltaMs > 0 && emergencyInputActive()) {
    return false;
  }

  const int64_t adjusted = static_cast<int64_t>(remainingAt(nowMs)) + deltaMs;
  if (adjusted <= 0) {
    recordEvent(TimerEventType::Adjusted, nowMs, deltaMs);
    release(ReleaseReason::AdjustedToZero, nowMs);
    return true;
  }
  if (adjusted > Hardware::kMaximumDurationMs) {
    return false;
  }

  if (state_ == TimerState::Paused) {
    pausedRemainingMs_ = static_cast<uint32_t>(adjusted);
  } else {
    deadlineMs_ = nowMs + static_cast<uint32_t>(adjusted);
  }
  if (static_cast<uint32_t>(adjusted) > sessionDurationMs_) {
    sessionDurationMs_ = static_cast<uint32_t>(adjusted);
  }
  recordEvent(TimerEventType::Adjusted, nowMs, deltaMs);
  armBackstop();
  return true;
}

bool TimerController::setSealed(bool sealed, uint32_t nowMs) {
  // Sealing is intentionally one-way for the current session. Expiry or the
  // physical emergency release clears it as part of release().
  settleBackstop(nowMs);
  if (!sealed || state_ != TimerState::Running || sealed_ || !sealAllowed_) {
    return false;
  }
  sealed_ = true;
  recordEvent(TimerEventType::ControlsLocked, nowMs,
              static_cast<int32_t>(remainingAt(nowMs)));
  return true;
}

TimerSnapshot TimerController::snapshot(uint32_t nowMs) const {
  const bool limited = holdLimited_ && state_ != TimerState::Open;
  const int32_t limitRemaining = limited ? static_cast<int32_t>(holdLimitMs_ - nowMs) : 0;
  return {state_, remainingAt(nowMs), sessionDurationMs_, sealed_,
          emergencyInputActive(), lastReleaseReason_, limited,
          limitRemaining > 0 ? static_cast<uint32_t>(limitRemaining) : 0,
          pauseAllowed_, sealAllowed_};
}

void TimerController::setControlPolicy(bool allowPause, bool allowSeal) {
  pauseAllowed_ = allowPause;
  sealAllowed_ = allowSeal;
}

TimerEvent TimerController::eventAt(uint8_t chronologicalIndex) const {
  if (chronologicalIndex >= eventCount_) {
    return {0, 0, 0, 0, TimerEventType::Boot, TimerState::Open, false,
            false};
  }
  return events_[(eventStart_ + chronologicalIndex) % kEventCapacity];
}

const char* TimerController::eventName(TimerEventType type) {
  switch (type) {
    case TimerEventType::Started:
      return "started";
    case TimerEventType::RemainingSet:
      return "remaining_set";
    case TimerEventType::Paused:
      return "paused";
    case TimerEventType::Resumed:
      return "resumed";
    case TimerEventType::Adjusted:
      return "adjusted";
    case TimerEventType::ControlsLocked:
      return "controls_locked";
    case TimerEventType::Expired:
      return "expired";
    case TimerEventType::OpenedByUser:
      return "opened_by_user";
    case TimerEventType::EmergencyRelease:
      return "emergency_release";
    case TimerEventType::EmergencyInputActivated:
      return "emergency_input_activated";
    case TimerEventType::EmergencyInputCleared:
      return "emergency_input_cleared";
    case TimerEventType::AdjustedToZero:
      return "adjusted_to_zero";
    case TimerEventType::WifiConnecting:
      return "wifi_connecting";
    case TimerEventType::WifiConnected:
      return "wifi_connected";
    case TimerEventType::WifiFailed:
      return "wifi_failed";
    case TimerEventType::WifiStandalone:
      return "wifi_standalone";
    case TimerEventType::WifiDisconnected:
      return "wifi_disconnected";
    case TimerEventType::WifiDhcpTimeout:
      return "wifi_dhcp_timeout";
    case TimerEventType::OtaStarted:
      return "ota_started";
    case TimerEventType::OtaSucceeded:
      return "ota_succeeded";
    case TimerEventType::OtaFailed:
      return "ota_failed";
    case TimerEventType::RestartRequested:
      return "restart_requested";
    case TimerEventType::DeviceRenamed:
      return "device_renamed";
    case TimerEventType::IdentifyStarted:
      return "identify_started";
    case TimerEventType::AccessPointConfigured:
      return "access_point_configured";
    case TimerEventType::BluetoothPairingStarted:
      return "bluetooth_pairing_started";
    case TimerEventType::BluetoothPairingCompleted:
      return "bluetooth_pairing_completed";
    case TimerEventType::BluetoothPairingRevoked:
      return "bluetooth_pairing_revoked";
    case TimerEventType::ShareLinkExpired:
      return "share_link_expired";
    case TimerEventType::Boot:
    default:
      return "boot";
  }
}

const char* TimerController::releaseReasonName(ReleaseReason reason) {
  switch (reason) {
    case ReleaseReason::Expired:
      return "expired";
    case ReleaseReason::User:
      return "user";
    case ReleaseReason::Emergency:
      return "emergency";
    case ReleaseReason::AdjustedToZero:
      return "adjusted_to_zero";
    case ReleaseReason::ShareLinkExpired:
      return "share_link_expired";
    case ReleaseReason::Boot:
    default:
      return "boot";
  }
}

uint32_t TimerController::remainingAt(uint32_t nowMs) const {
  if (state_ == TimerState::Open) {
    return 0;
  }
  if (state_ == TimerState::Paused) {
    return pausedRemainingMs_;
  }
  const int32_t remaining = static_cast<int32_t>(deadlineMs_ - nowMs);
  return remaining > 0 ? static_cast<uint32_t>(remaining) : 0;
}

void TimerController::release(ReleaseReason reason, uint32_t nowMs) {
  Hardware::writeLock(false);
  DeadlineBackstop::cancel();
  state_ = TimerState::Open;
  deadlineMs_ = 0;
  pausedRemainingMs_ = 0;
  sessionDurationMs_ = 0;
  sealed_ = false;
  holdLimited_ = false;
  holdLimitMs_ = 0;
  lastReleaseReason_ = reason;
  TimerEventType eventType = TimerEventType::OpenedByUser;
  if (reason == ReleaseReason::Expired) {
    eventType = TimerEventType::Expired;
  } else if (reason == ReleaseReason::Emergency) {
    eventType = TimerEventType::EmergencyRelease;
  } else if (reason == ReleaseReason::AdjustedToZero) {
    eventType = TimerEventType::AdjustedToZero;
  } else if (reason == ReleaseReason::ShareLinkExpired) {
    eventType = TimerEventType::ShareLinkExpired;
  }
  recordEvent(eventType, nowMs, 0);
}

void TimerController::recordEvent(TimerEventType type, uint32_t nowMs,
                                  int32_t valueMs) {
  ++eventRevision_;
  if (eventRevision_ == 0) {
    // Zero is reserved for an uninitialized observer. In practice this would
    // require billions of state changes during one boot.
    eventRevision_ = 1;
  }
  uint8_t index = 0;
  if (eventCount_ < kEventCapacity) {
    index = (eventStart_ + eventCount_) % kEventCapacity;
    ++eventCount_;
  } else {
    index = eventStart_;
    eventStart_ = (eventStart_ + 1) % kEventCapacity;
  }
  events_[index] = {nowMs,
                    valueMs,
                    eventRevision_,
                    sessionSequence_,
                    type,
                    state_,
                    sealed_,
                    emergencyInputActive()
#if defined(SMART_TIMER_HARDWARE_V2)
                    , bluetoothEventAppId_, shareLinkEvent_
#endif
                    };
}
