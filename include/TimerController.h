#pragma once

#include <Arduino.h>

enum class TimerState : uint8_t {
  Open,
  Running,
  Paused,
};

enum class ReleaseReason : uint8_t {
  Boot,
  Expired,
  User,
  Emergency,
  AdjustedToZero,
  ShareLinkExpired,  // a share link's hold limit elapsed (fail open)
};

enum class TimerEventType : uint8_t {
  // These values are part of the stable ost-ble/1 wire contract. Append new
  // values; never reorder or implicitly renumber existing entries.
  Boot = 0,
  Started = 1,
  RemainingSet = 2,
  Paused = 3,
  Resumed = 4,
  Adjusted = 5,
  ControlsLocked = 6,
  Expired = 7,
  OpenedByUser = 8,
  EmergencyRelease = 9,
  EmergencyInputActivated = 10,
  EmergencyInputCleared = 11,
  AdjustedToZero = 12,
  WifiConnecting = 13,
  WifiConnected = 14,
  WifiFailed = 15,
  WifiStandalone = 16,
  WifiDisconnected = 17,
  WifiDhcpTimeout = 18,
  OtaStarted = 19,
  OtaSucceeded = 20,
  OtaFailed = 21,
  RestartRequested = 22,
  DeviceRenamed = 23,
  IdentifyStarted = 24,
  AccessPointConfigured = 25,
  BluetoothPairingStarted = 26,
  BluetoothPairingCompleted = 27,
  BluetoothPairingRevoked = 28,
  ShareLinkExpired = 29,
};

struct TimerEvent {
  uint32_t atMs;
  int32_t valueMs;
  uint32_t id;
  uint32_t sessionSequence;
  TimerEventType type;
  TimerState state;
  bool sealed;
  bool emergencyInputActive;
#if defined(SMART_TIMER_HARDWARE_V2)
  uint32_t bluetoothAppId = 0;
  bool viaShareLink = false;  // recorded while applying a share-link command
#endif
};

struct TimerSnapshot {
  TimerState state;
  uint32_t remainingMs;
  uint32_t sessionDurationMs;
  bool sealed;
  bool emergencyInputActive;
  ReleaseReason lastReleaseReason;
  // Set when a share link limits how long this session may stay locked.
  bool holdLimited;
  uint32_t holdLimitRemainingMs;
  // Owner settings: whether a session may be paused, and whether its
  // controls may be locked. Enforced by TimerCommandAuthority.
  bool pauseAllowed = true;
  bool sealAllowed = true;
};

class TimerController {
 public:
  void begin();
  void tick(uint32_t nowMs);
  void setEmergencyInputActive(bool active, uint32_t nowMs);

  bool start(uint32_t durationMs, uint32_t nowMs);
  bool setRemaining(uint32_t durationMs, uint32_t nowMs);
  bool stop(bool physicalEmergency, uint32_t nowMs);
  bool pause(uint32_t nowMs);
  bool resume(uint32_t nowMs);
  bool adjust(int32_t deltaMs, uint32_t nowMs);
  bool setSealed(bool sealed, uint32_t nowMs);
  // Saved owner settings, applied at boot and when the owner changes them.
  // They never alter a session in progress: an existing pause or lock stays.
  void setControlPolicy(bool allowPause, bool allowSeal);
  // A share link that starts, extends, pauses or seals the current session
  // limits how long it may stay locked: at untilMs the timer opens by itself,
  // like expiry, even if paused or sealed. Several links keep the latest
  // limit; a new session or any release clears it.
  void limitHoldUntil(uint32_t untilMs);

  TimerSnapshot snapshot(uint32_t nowMs) const;
  bool isOpen() const { return state_ == TimerState::Open; }
  bool emergencyInputActive() const;
  uint8_t eventCount() const { return eventCount_; }
  static constexpr uint8_t eventCapacity() { return kEventCapacity; }
  uint32_t eventRevision() const { return eventRevision_; }
  uint32_t sessionSequence() const { return sessionSequence_; }
  TimerEvent eventAt(uint8_t chronologicalIndex) const;
  void recordEvent(TimerEventType type, uint32_t nowMs, int32_t valueMs = 0);
#if defined(SMART_TIMER_HARDWARE_V2)
  void setBluetoothEventAppId(uint32_t id) { bluetoothEventAppId_ = id; }
  void setShareLinkEventSource(bool active) { shareLinkEvent_ = active; }
#endif
  static const char* eventName(TimerEventType type);
  static const char* releaseReasonName(ReleaseReason reason);

 private:
  static constexpr uint8_t kEventCapacity = 16;

  uint32_t remainingAt(uint32_t nowMs) const;
  void release(ReleaseReason reason, uint32_t nowMs);

  TimerState state_ = TimerState::Open;
  uint32_t deadlineMs_ = 0;
  uint32_t pausedRemainingMs_ = 0;
  uint32_t sessionDurationMs_ = 0;
  bool sealed_ = false;
  bool pauseAllowed_ = true;
  bool sealAllowed_ = true;
  bool emergencyInputActive_ = false;
  bool holdLimited_ = false;
  uint32_t holdLimitMs_ = 0;
  ReleaseReason lastReleaseReason_ = ReleaseReason::Boot;
  TimerEvent events_[kEventCapacity]{};
#if defined(SMART_TIMER_HARDWARE_V2)
  uint32_t bluetoothEventAppId_ = 0;
  bool shareLinkEvent_ = false;
#endif
  uint8_t eventStart_ = 0;
  uint8_t eventCount_ = 0;
  uint32_t eventRevision_ = 0;
  uint32_t sessionSequence_ = 0;
};
