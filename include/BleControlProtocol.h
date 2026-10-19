#pragma once
#include "BleAppRegistry.h"

#include <cstddef>

#include <Arduino.h>

#include "TimerCommandAuthority.h"
#include "TimerController.h"

enum class BleControlOpcode : uint8_t {
  Authenticate = 0x01,
  PairingCommit = 0x02,
  Start = 0x10,
  Release = 0x11,
  Pause = 0x12,
  Resume = 0x13,
  Adjust = 0x14,
  LockControls = 0x15,
  Identify = 0x16,
};

enum class BleControlResultCode : uint8_t {
  Applied = 0,
  InvalidFrame = 1,
  Unauthorized = 2,
  StaleBoot = 3,
  StaleSession = 4,
  InvalidState = 5,
  InvalidDuration = 6,
  EmergencyInputActive = 7,
  ControlsLocked = 8,
  MaximumDurationReached = 9,
  UnsupportedOpcode = 10,
  OperationConflict = 11,
  SystemTransition = 12,
  ControlDisabled = 13,
};

struct BleControlExecution {
  uint8_t frame[20];
  BleControlResultCode code;
  bool changed;
  bool duplicate;
};

// Parses the authenticated 32-byte ost-ble/1 command frame and applies it
// through the shared TimerCommandAuthority. The final 12 bytes carry the
// per-connection counter and command MAC, which the BLE transport verifies
// before invoking this parser. It owns only framing, per-app retry results
// and the transition gate; timer rules are the same as REST.
// BLE callbacks still queue work so TimerController mutations remain on the
// same thread as REST and GPIO input.
class BleControlProtocol {
 public:
  static constexpr uint8_t kProtocolVersion = 1;
  static constexpr size_t kCommandFrameSize = 32;
  static constexpr size_t kResultFrameSize = 20;
  // Bytes through value B describe the requested operation. Counter and MAC
  // bytes are transport-session material and change after a reconnect.
  static constexpr size_t kSemanticCommandSize = 20;

  explicit BleControlProtocol(TimerController& timer);

  void begin(uint32_t bootId);
  BleControlExecution execute(const uint8_t* frame, bool authorized,
                              uint32_t nowMs, uint32_t randomValue,
                              bool mutationsAllowed = true,
                              uint32_t appId = 1, uint8_t appSlot = 0);
  static const char* resultName(BleControlResultCode code);

 private:
  static constexpr uint8_t kRecentCapacity = 8;

  struct RecentCommand {
    uint8_t request[kSemanticCommandSize]{};
    uint8_t response[kResultFrameSize]{};
    uint16_t requestId = 0;
    bool used = false;
  };

  BleControlExecution result(const uint8_t* request,
                             BleControlResultCode code, bool changed,
                             bool duplicate, uint32_t nowMs) const;
  int8_t findRecent(uint16_t requestId, uint8_t slot) const;
  void remember(const uint8_t* request, const uint8_t* response,
                uint16_t requestId, uint8_t slot);
  static bool commandOp(BleControlOpcode opcode, TimerCommandOp& op);
  static BleControlResultCode resultCode(TimerCommandResult result);
  static uint16_t readUint16Le(const uint8_t* source);
  static uint32_t readUint32Le(const uint8_t* source);
  static void writeUint32Le(uint8_t* destination, uint32_t value);
  static uint8_t stateCode(TimerState state);
  static uint8_t releaseReasonCode(ReleaseReason reason);

  TimerController& timer_;
  TimerCommandAuthority authority_;
  uint32_t bootId_ = 0;
  RecentCommand recent_[BleAppRegistry::kCapacity][kRecentCapacity]{};
  uint32_t recentAppIds_[BleAppRegistry::kCapacity]{};
  uint8_t recentCursor_[BleAppRegistry::kCapacity]{};
};
