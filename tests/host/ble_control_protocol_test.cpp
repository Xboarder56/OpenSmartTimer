#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>

#include "BleControlProtocol.h"
#include "Hardware.h"

namespace {
std::array<uint8_t, 17> pinModes{};
std::array<uint8_t, 17> pinValues{};

void write16(uint8_t* destination, uint16_t value) {
  destination[0] = static_cast<uint8_t>(value);
  destination[1] = static_cast<uint8_t>(value >> 8);
}

void write32(uint8_t* destination, uint32_t value) {
  destination[0] = static_cast<uint8_t>(value);
  destination[1] = static_cast<uint8_t>(value >> 8);
  destination[2] = static_cast<uint8_t>(value >> 16);
  destination[3] = static_cast<uint8_t>(value >> 24);
}

uint32_t read32(const uint8_t* source) {
  return static_cast<uint32_t>(source[0]) |
         static_cast<uint32_t>(source[1]) << 8 |
         static_cast<uint32_t>(source[2]) << 16 |
         static_cast<uint32_t>(source[3]) << 24;
}

std::array<uint8_t, BleControlProtocol::kCommandFrameSize> command(
    BleControlOpcode opcode, uint16_t requestId, uint32_t bootId,
    uint32_t sessionSequence, int32_t valueA = 0, int32_t valueB = 0) {
  std::array<uint8_t, BleControlProtocol::kCommandFrameSize> frame{};
  frame[0] = BleControlProtocol::kProtocolVersion;
  frame[1] = static_cast<uint8_t>(opcode);
  write16(frame.data() + 2, requestId);
  write32(frame.data() + 4, bootId);
  write32(frame.data() + 8, sessionSequence);
  write32(frame.data() + 12, static_cast<uint32_t>(valueA));
  write32(frame.data() + 16, static_cast<uint32_t>(valueB));
  return frame;
}

void expect(const BleControlExecution& result, BleControlResultCode code,
            uint8_t state, uint32_t bootId, uint32_t session,
            bool changed = false, bool duplicate = false) {
  assert(result.code == code);
  assert(result.changed == changed);
  assert(result.duplicate == duplicate);
  assert(result.frame[0] == BleControlProtocol::kProtocolVersion);
  assert(result.frame[4] == static_cast<uint8_t>(code));
  assert(result.frame[6] == state);
  assert(read32(result.frame + 8) == bootId);
  assert(read32(result.frame + 12) == session);
}
}  // namespace

void pinMode(uint8_t pin, uint8_t mode) { pinModes.at(pin) = mode; }
void digitalWrite(uint8_t pin, uint8_t value) { pinValues.at(pin) = value; }
int digitalRead(uint8_t pin) { return pinValues.at(pin); }

extern "C" esp_err_t gpio_set_level(gpio_num_t gpioNumber,
                                    std::uint32_t level) {
  pinValues.at(static_cast<size_t>(gpioNumber)) =
      static_cast<uint8_t>(level);
  return ESP_OK;
}

int main() {
  constexpr uint32_t kBootId = 0x78563412;
  pinValues[Hardware::kEmergencyPin] = LOW;
  TimerController timer;
  timer.begin();
  BleControlProtocol protocol(timer);
  protocol.begin(kBootId);

  auto start = command(BleControlOpcode::Start, 1, kBootId, 0, 10000);
  auto result = protocol.execute(start.data(), false, 100, 0);
  expect(result, BleControlResultCode::Unauthorized, 0, kBootId, 0);
  assert(timer.isOpen());

  result = protocol.execute(start.data(), true, 100, 0, false);
  expect(result, BleControlResultCode::SystemTransition, 0, kBootId, 0);
  assert(timer.isOpen());

  result = protocol.execute(start.data(), true, 100, 0);
  expect(result, BleControlResultCode::Applied, 1, kBootId, 1, true);
  assert(read32(result.frame + 16) == 10000);
  assert(pinValues[Hardware::kLockPinA] == HIGH);

  auto retriedStart = start;
  // A reconnect creates a new authenticated session, counter and MAC. Request
  // idempotency is based on the semantic 20-byte operation prefix so a lost
  // response can be retried safely across BLE connections.
  for (size_t index = BleControlProtocol::kSemanticCommandSize;
       index < retriedStart.size(); ++index) {
    retriedStart[index] = static_cast<uint8_t>(0xa0 + index);
  }
  result = protocol.execute(retriedStart.data(), true, 200, 0);
  expect(result, BleControlResultCode::Applied, 1, kBootId, 1, true, true);
  assert(timer.sessionSequence() == 1);

  auto conflict = start;
  write32(conflict.data() + 12, 20000);
  result = protocol.execute(conflict.data(), true, 200, 0);
  expect(result, BleControlResultCode::OperationConflict, 1, kBootId, 1);

  auto staleBoot = command(BleControlOpcode::Release, 2, kBootId + 1, 1);
  result = protocol.execute(staleBoot.data(), true, 200, 0);
  expect(result, BleControlResultCode::StaleBoot, 1, kBootId, 1);

  auto pause = command(BleControlOpcode::Pause, 3, kBootId, 1);
  result = protocol.execute(pause.data(), true, 200, 0);
  expect(result, BleControlResultCode::Applied, 2, kBootId, 1, true);

  auto resume = command(BleControlOpcode::Resume, 4, kBootId, 1);
  result = protocol.execute(resume.data(), true, 300, 0);
  expect(result, BleControlResultCode::Applied, 1, kBootId, 1, true);

  const uint32_t beforeAdjust = timer.snapshot(300).remainingMs;
  auto adjust = command(BleControlOpcode::Adjust, 5, kBootId, 1, 1000);
  result = protocol.execute(adjust.data(), true, 300, 0);
  expect(result, BleControlResultCode::Applied, 1, kBootId, 1, true);
  assert(read32(result.frame + 16) == beforeAdjust + 1000);

  const uint32_t revisionBeforeNoop = timer.eventRevision();
  auto noopAdjust = command(BleControlOpcode::Adjust, 15, kBootId, 1, 0);
  result = protocol.execute(noopAdjust.data(), true, 300, 0);
  expect(result, BleControlResultCode::Applied, 1, kBootId, 1);
  assert(timer.eventRevision() == revisionBeforeNoop);

  auto tooLong = command(BleControlOpcode::Adjust, 6, kBootId, 1,
                         static_cast<int32_t>(Hardware::kMaximumDurationMs));
  result = protocol.execute(tooLong.data(), true, 300, 0);
  expect(result, BleControlResultCode::MaximumDurationReached, 1, kBootId, 1);

  auto malformedRelease = command(BleControlOpcode::Release, 7, kBootId, 1, 1);
  result = protocol.execute(malformedRelease.data(), true, 300, 0);
  expect(result, BleControlResultCode::InvalidFrame, 1, kBootId, 1);

  auto staleSession = command(BleControlOpcode::Release, 8, kBootId, 0);
  result = protocol.execute(staleSession.data(), true, 300, 0);
  expect(result, BleControlResultCode::StaleSession, 1, kBootId, 1);

  auto release = command(BleControlOpcode::Release, 9, kBootId, 1);
  result = protocol.execute(release.data(), true, 300, 0);
  expect(result, BleControlResultCode::Applied, 0, kBootId, 1, true);
  assert(pinValues[Hardware::kLockPinA] == LOW);

  pinValues[Hardware::kEmergencyPin] = HIGH;
  auto blockedStart = command(BleControlOpcode::Start, 10, kBootId, 0, 1000);
  result = protocol.execute(blockedStart.data(), true, 400, 0);
  expect(result, BleControlResultCode::EmergencyInputActive, 0, kBootId, 1);
  pinValues[Hardware::kEmergencyPin] = LOW;

  auto randomStart = command(BleControlOpcode::Start, 11, kBootId, 0,
                             1000, 2000);
  result = protocol.execute(randomStart.data(), true, 500, 500);
  expect(result, BleControlResultCode::Applied, 1, kBootId, 2, true);
  assert(read32(result.frame + 16) == 1500);

  // A raw emergency assertion refuses lengthening before the debounced
  // release, with the specific result code; shortening remains allowed.
  pinValues[Hardware::kEmergencyPin] = HIGH;
  auto blockedExtend = command(BleControlOpcode::Adjust, 100, kBootId, 2, 1000);
  result = protocol.execute(blockedExtend.data(), true, 500, 0);
  expect(result, BleControlResultCode::EmergencyInputActive, 1, kBootId, 2);
  assert(read32(result.frame + 16) == 1500);
  auto allowedShorten = command(BleControlOpcode::Adjust, 101, kBootId, 2, -100);
  result = protocol.execute(allowedShorten.data(), true, 500, 0);
  expect(result, BleControlResultCode::Applied, 1, kBootId, 2, true);
  assert(read32(result.frame + 16) == 1400);
  pinValues[Hardware::kEmergencyPin] = LOW;

  auto lock =command(BleControlOpcode::LockControls, 12, kBootId, 2);
  result = protocol.execute(lock.data(), true, 500, 0);
  expect(result, BleControlResultCode::Applied, 1, kBootId, 2, true);
  auto sealedRelease = command(BleControlOpcode::Release, 13, kBootId, 2);
  result = protocol.execute(sealedRelease.data(), true, 500, 0);
  expect(result, BleControlResultCode::ControlsLocked, 1, kBootId, 2);
  assert(timer.stop(true, 500));
  assert(timer.isOpen());

  auto identify = command(BleControlOpcode::Identify, 16, kBootId, 2);
  result = protocol.execute(identify.data(), true, 600, 0);
  expect(result, BleControlResultCode::Applied, 0, kBootId, 2, true);
  const uint32_t revisionAfterIdentify = timer.eventRevision();
  result = protocol.execute(identify.data(), true, 600, 0);
  expect(result, BleControlResultCode::Applied, 0, kBootId, 2, true, true);
  assert(timer.eventRevision() == revisionAfterIdentify);

  auto unsupported = command(static_cast<BleControlOpcode>(0x7f), 14,
                             kBootId, 2);
  result = protocol.execute(unsupported.data(), true, 500, 0);
  expect(result, BleControlResultCode::UnsupportedOpcode, 0, kBootId, 2);

  assert(std::strcmp(BleControlProtocol::resultName(
                         BleControlResultCode::StaleSession),
                     "stale_session") == 0);
  assert(std::strcmp(BleControlProtocol::resultName(
                         BleControlResultCode::SystemTransition),
                     "system_transition") == 0);
  // The owner's "switched off" refusal has its own wire code.
  assert(static_cast<int>(BleControlResultCode::ControlDisabled) == 13);
  assert(std::strcmp(BleControlProtocol::resultName(
                         BleControlResultCode::ControlDisabled),
                     "control_disabled") == 0);

  // Same vector is recalculated by tests/protocol/ost_ble_v1_vectors_test.py. The C++
  // parser intentionally ignores only the already-verified counter/MAC tail.
  const std::array<uint8_t, 32> goldenStart = {
      0x01, 0x10, 0x34, 0x12, 0x12, 0x34, 0x56, 0x78,
      0x00, 0x00, 0x00, 0x00, 0xc0, 0x27, 0x09, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
      0x04, 0x8d, 0x77, 0x8f, 0x26, 0x27, 0xe3, 0x6d};
  const std::array<uint8_t, 20> goldenResult = {
      0x01, 0x10, 0x34, 0x12, 0x00, 0x01, 0x01, 0x00,
      0x12, 0x34, 0x56, 0x78, 0x01, 0x00, 0x00, 0x00,
      0xc0, 0x27, 0x09, 0x00};
  TimerController goldenTimer;
  goldenTimer.begin();
  BleControlProtocol goldenProtocol(goldenTimer);
  goldenProtocol.begin(kBootId);
  const BleControlExecution goldenExecution = goldenProtocol.execute(
      goldenStart.data(), true, 1000, 0);
  assert(std::memcmp(goldenExecution.frame, goldenResult.data(),
                     goldenResult.size()) == 0);
  // Three apps may use exactly the same request IDs and semantics. Each gets
  // an independent result; reconnects retain only that credential's retries.
  timer.stop(true, 40000);
  protocol.begin(kBootId);
  identify = command(BleControlOpcode::Identify, 42, kBootId, timer.sessionSequence());
  auto a = protocol.execute(identify.data(), true, 40001, 0, true, 101, 0);
  auto b = protocol.execute(identify.data(), true, 40002, 0, true, 102, 1);
  auto c = protocol.execute(identify.data(), true, 40003, 0, true, 103, 2);
  assert(!a.duplicate && !b.duplicate && !c.duplicate);
  assert(protocol.execute(identify.data(), true, 40004, 0, true, 101, 0).duplicate);
  // Filling B's history cannot evict A's eight-operation retry window.
  for (unsigned i = 1; i <= 12; ++i) {
    auto request = command(BleControlOpcode::Identify, 100 + i, kBootId, timer.sessionSequence());
    protocol.execute(request.data(), true, 40004 + i, 0, true, 102, 1);
  }
  assert(protocol.execute(identify.data(), true, 40020, 0, true, 101, 0).duplicate);
  assert(!protocol.execute(identify.data(), true, 40021, 0, true, 104, 0).duplicate);
  assert(protocol.execute(identify.data(), true, 40022, 0, true, 0, 0).code == BleControlResultCode::Unauthorized);
  assert(protocol.execute(identify.data(), true, 40023, 0, true, 101, 3).code == BleControlResultCode::Unauthorized);
  timer.setBluetoothEventAppId(102);
  timer.recordEvent(TimerEventType::IdentifyStarted, 41000);
  timer.setBluetoothEventAppId(0);
  assert(timer.eventAt(timer.eventCount() - 1).bluetoothAppId == 102);
  std::cout << "BLE control protocol and three-app retry isolation tests passed\n";
  return 0;
}
