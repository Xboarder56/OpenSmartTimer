#include <array>
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <iostream>
#include <utility>
#include <string>
#include <vector>

#include "Hardware.h"
#include "OtaImageValidation.h"
#include "TimerController.h"

namespace {
std::array<uint8_t, 17> pinModes{};
std::array<uint8_t, 17> pinValues{};
std::vector<std::pair<char, uint8_t>> gpioOperations;
// Simulates the emergency interrupt firing just before the next energizing
// lock write, after start() has already checked the input.
bool emergencyEdgeBeforeNextLockWrite = false;
}

void pinMode(uint8_t pin, uint8_t mode) {
  gpioOperations.emplace_back('M', pin);
  pinModes.at(pin) = mode;
}
void digitalWrite(uint8_t pin, uint8_t value) {
  if (emergencyEdgeBeforeNextLockWrite && pin == Hardware::kLockPinA &&
      value == (Hardware::kLockActiveHigh ? HIGH : LOW)) {
    emergencyEdgeBeforeNextLockWrite = false;
    Hardware::emergencyLatched = true;
  }
  gpioOperations.emplace_back('W', pin);
  pinValues.at(pin) = value;
}
int digitalRead(uint8_t pin) { return pinValues.at(pin); }

#if defined(SMART_TIMER_HARDWARE_V2)
extern "C" esp_err_t gpio_set_level(gpio_num_t gpioNumber,
                                      std::uint32_t level) {
  gpioOperations.emplace_back('L', static_cast<uint8_t>(gpioNumber));
  pinValues.at(static_cast<size_t>(gpioNumber)) =
      static_cast<uint8_t>(level);
  return ESP_OK;
}
#endif

int main() {
#if defined(SMART_TIMER_HARDWARE_V2)
  const uint8_t validOtaHeader[] = {
      0xE9, 0x06, 0x02, 0x2F, 0, 0, 0, 0, 0, 0, 0, 0, 0x05, 0x00};
  static_assert(OtaImageValidation::kRequiredHeaderLength == 14,
                "V2 OTA validation length changed");
  assert(OtaImageValidation::matchesTarget(validOtaHeader,
                                            sizeof(validOtaHeader)));
  for (const size_t index : {size_t{0}, size_t{2}, size_t{3}, size_t{12}}) {
    uint8_t invalid[sizeof(validOtaHeader)];
    std::copy(std::begin(validOtaHeader), std::end(validOtaHeader), invalid);
    invalid[index] ^= 0x01;
    assert(!OtaImageValidation::matchesTarget(invalid, sizeof(invalid)));
  }
#else
  const uint8_t validOtaHeader[] = {0xE9, 0x02, 0x03, 0x40};
  static_assert(OtaImageValidation::kRequiredHeaderLength == 4,
                "V1 OTA validation length changed");
  assert(OtaImageValidation::matchesTarget(validOtaHeader,
                                            sizeof(validOtaHeader)));
  for (const size_t index : {size_t{0}, size_t{2}, size_t{3}}) {
    uint8_t invalid[sizeof(validOtaHeader)];
    std::copy(std::begin(validOtaHeader), std::end(validOtaHeader), invalid);
    invalid[index] ^= 0x01;
    assert(!OtaImageValidation::matchesTarget(invalid, sizeof(invalid)));
  }
#endif
  assert(!OtaImageValidation::matchesTarget(validOtaHeader,
                                             sizeof(validOtaHeader) - 1));

  TimerController timer;
  timer.begin();
  assert(timer.isOpen());
  assert(timer.eventCount() == 1);
  assert(timer.eventRevision() == 1);
  assert(timer.eventAt(0).type == TimerEventType::Boot);
  assert(timer.eventAt(0).id == 1);
  assert(timer.eventAt(0).state == TimerState::Open);
  assert(!timer.eventAt(0).sealed);
  assert(!timer.eventAt(0).emergencyInputActive);
  assert(timer.eventAt(0).sessionSequence == 0);
  assert(timer.sessionSequence() == 0);
  assert(!timer.snapshot(0).emergencyInputActive);
  assert(pinModes[Hardware::kLockPinA] == OUTPUT);
  assert(pinValues[Hardware::kLockPinA] == LOW);
  assert(pinValues[Hardware::kLockPinB] == LOW);
#if defined(SMART_TIMER_HARDWARE_V2)
  assert(Hardware::failOpenInitializationSucceeded());
  const std::vector<std::pair<char, uint8_t>> expectedFailOpenPrefix = {
      {'L', Hardware::kLockPinA},
      {'L', Hardware::kLockIndicatorPin},
      {'L', Hardware::kStatusPin},
      {'M', Hardware::kLockPinA},
      {'M', Hardware::kLockIndicatorPin},
      {'M', Hardware::kStatusPin},
  };
  assert(gpioOperations.size() >= expectedFailOpenPrefix.size());
  assert(std::equal(expectedFailOpenPrefix.begin(),
                    expectedFailOpenPrefix.end(), gpioOperations.begin()));
  static_assert(Hardware::kLockPinA == 3, "V2 lock-output GPIO changed");
  static_assert(Hardware::kLockIndicatorPin == 6,
                "V2 amber-indicator GPIO changed");
  static_assert(Hardware::kStatusPin == 5, "V2 blue-indicator GPIO changed");
  static_assert(Hardware::kEmergencyPin == 0,
                "V2 emergency-input GPIO changed");
  static_assert(Hardware::kConfigPin == 4,
                "V2 Reset-button GPIO changed");
  static_assert(Hardware::kAuxiliaryInputPin == 1,
                "V2 auxiliary-input GPIO changed");
  assert(pinModes[Hardware::kLockIndicatorPin] == OUTPUT);
  assert(pinValues[Hardware::kLockIndicatorPin] == LOW);
  assert(pinModes[Hardware::kAuxiliaryInputPin] == INPUT);
#else
  static_assert(Hardware::kLockPinA == 4, "V1 lock GPIO A changed");
  static_assert(Hardware::kLockPinB == 13, "V1 lock GPIO B changed");
  static_assert(Hardware::kStatusPin == 12, "V1 blue-indicator GPIO changed");
  static_assert(Hardware::kEmergencyPin == 10,
                "V1 emergency-input GPIO changed");
  static_assert(Hardware::kConfigPin == 5,
                "V1 Reset-button GPIO changed");
#endif
  assert(pinValues[Hardware::kStatusPin] == LOW);
  assert(Hardware::statusLedOn(0, true, false, true));
  assert(Hardware::statusLedOn(500, true, false, true));
  assert(Hardware::statusLedOn(100, false, true, false));
  assert(Hardware::statusLedOn(100, true, false, false));
  assert(!Hardware::statusLedOn(750, true, false, false));
  assert(Hardware::statusLedOn(50, false, false, false));
  assert(!Hardware::statusLedOn(200, false, false, false));
  assert(Hardware::otaArmedLedOn(50));
  assert(!Hardware::otaArmedLedOn(150));
  assert(Hardware::otaArmedLedOn(300));
  assert(!Hardware::otaArmedLedOn(500));
  assert(Hardware::otaUnverifiedLedOn(50));
  assert(!Hardware::otaUnverifiedLedOn(150));
  assert(Hardware::otaUnverifiedLedOn(300));
  assert(Hardware::otaUnverifiedLedOn(550));
  assert(!Hardware::otaUnverifiedLedOn(700));
  assert(Hardware::otaInstallLedOn(0, false));
  assert(Hardware::otaInstallLedOn(1000, false));
  assert(Hardware::otaInstallLedOn(40, true));
  assert(!Hardware::otaInstallLedOn(120, true));
  assert(Hardware::factoryResetHoldLedOn(100, 1000));
  assert(!Hardware::factoryResetHoldLedOn(200, 1000));
  assert(Hardware::factoryResetHoldLedOn(50, 8000));
  assert(!Hardware::factoryResetHoldLedOn(180, 8000));

  // A held emergency circuit at boot must keep every output open and reject
  // an attempted start without requiring a new input edge.
  pinValues[Hardware::kEmergencyPin] = HIGH;
  TimerController heldAtBoot;
  heldAtBoot.begin();
  assert(heldAtBoot.emergencyInputActive());
  assert(heldAtBoot.eventCount() == 2);
  assert(heldAtBoot.eventAt(1).type ==
         TimerEventType::EmergencyInputActivated);
  assert(!heldAtBoot.start(1000, 0));
  assert(pinValues[Hardware::kLockPinA] == LOW);
  pinValues[Hardware::kEmergencyPin] = LOW;
  heldAtBoot.setEmergencyInputActive(false, 1);
  assert(heldAtBoot.eventAt(heldAtBoot.eventCount() - 1).type ==
         TimerEventType::EmergencyInputCleared);
  assert(heldAtBoot.start(1000, 1));
  heldAtBoot.setEmergencyInputActive(true, 2);
  assert(heldAtBoot.isOpen());
  assert(pinValues[Hardware::kLockPinA] == LOW);
  assert(heldAtBoot.snapshot(2).lastReleaseReason == ReleaseReason::Emergency);
  assert(heldAtBoot.eventAt(heldAtBoot.eventCount() - 2).type ==
         TimerEventType::EmergencyInputActivated);
  assert(heldAtBoot.eventAt(heldAtBoot.eventCount() - 1).type ==
         TimerEventType::EmergencyRelease);
  assert(!heldAtBoot.start(1000, 3));
  heldAtBoot.setEmergencyInputActive(false, 4);
  assert(heldAtBoot.eventAt(heldAtBoot.eventCount() - 1).type ==
         TimerEventType::EmergencyInputCleared);

  // A circuit asserted immediately before start must be observed from the raw
  // pin level even if the main-loop debouncer has not run yet.
  pinValues[Hardware::kEmergencyPin] = HIGH;
  assert(!timer.start(1000, 99));
  assert(timer.isOpen());
  pinValues[Hardware::kEmergencyPin] = LOW;

  assert(!timer.start(0, 100));
  assert(!timer.start(Hardware::kMaximumDurationMs + 1, 100));
  assert(timer.start(1000, 100));
  assert(timer.sessionSequence() == 1);
  const TimerEvent startedEvent = timer.eventAt(timer.eventCount() - 1);
  assert(startedEvent.type == TimerEventType::Started);
  assert(startedEvent.state == TimerState::Running);
  assert(startedEvent.sessionSequence == 1);
  assert(timer.eventRevision() > 1);
  assert(timer.snapshot(100).sessionDurationMs == 1000);
  assert(pinValues[Hardware::kLockPinA] == HIGH);
#if defined(SMART_TIMER_HARDWARE_V2)
  assert(pinValues[Hardware::kLockIndicatorPin] == HIGH);
#endif

  // A raw emergency assertion before the debounced release must refuse any
  // lengthening adjustment, while shortening toward release stays allowed.
  pinValues[Hardware::kEmergencyPin] = HIGH;
  const uint32_t revisionBeforeRefusedAdjust = timer.eventRevision();
  assert(!timer.adjust(500, 100));
  assert(timer.eventRevision() == revisionBeforeRefusedAdjust);
  assert(timer.snapshot(100).remainingMs == 1000);
  assert(timer.adjust(-100, 100));
  assert(timer.snapshot(100).remainingMs == 900);
  assert(timer.adjust(100, 100) == false);
  pinValues[Hardware::kEmergencyPin] = LOW;
  assert(timer.adjust(100, 100));
  assert(timer.snapshot(100).remainingMs == 1000);
  assert(timer.snapshot(100).sessionDurationMs == 1000);
  assert(pinValues[Hardware::kStatusPin] == LOW);
  assert(!timer.start(100, 100));  // An active session cannot be shortened.
  assert(timer.setRemaining(800, 100));  // Legacy absolute adjustment.
  assert(timer.snapshot(400).remainingMs == 500);
  timer.tick(899);
  assert(!timer.isOpen());
  timer.tick(900);
  assert(timer.isOpen());
  assert(pinValues[Hardware::kLockPinB] == LOW);

  assert(timer.start(1000, 0xFFFFFF00UL));
  assert(timer.sessionSequence() == 2);
  assert(timer.snapshot(0x000000F4UL).remainingMs == 500);  // millis() wrap.
  timer.tick(0x000002E8UL);
  assert(timer.isOpen());

  assert(timer.start(5000, 2000));
  assert(timer.sessionSequence() == 3);
  assert(timer.pause(3000));
  assert(!timer.setSealed(true, 3000));  // A paused sealed timer would never expire.
  assert(timer.snapshot(9000).remainingMs == 4000);
  assert(timer.resume(9000));
  assert(timer.adjust(1000, 10000));
  assert(timer.snapshot(10000).remainingMs == 4000);
  const uint32_t revisionBeforeNoop = timer.eventRevision();
  assert(timer.adjust(0, 10000));
  assert(timer.snapshot(10000).remainingMs == 4000);
  assert(timer.eventRevision() == revisionBeforeNoop);

  assert(timer.setSealed(true, 10000));
  const TimerEvent sealedEvent = timer.eventAt(timer.eventCount() - 1);
  assert(sealedEvent.type == TimerEventType::ControlsLocked);
  assert(sealedEvent.sealed);
  assert(sealedEvent.state == TimerState::Running);
  assert(!timer.setSealed(false, 10000));
  assert(!timer.setRemaining(1000, 10000));
  assert(!timer.stop(false, 10000));
  assert(!timer.pause(10000));
  assert(!timer.adjust(-1000, 10000));
  assert(timer.stop(true, 10100));  // Physical emergency release always wins.
  assert(timer.isOpen());
  assert(timer.snapshot(10100).lastReleaseReason == ReleaseReason::Emergency);
  assert(timer.eventAt(timer.eventCount() - 1).type ==
         TimerEventType::EmergencyRelease);
  const TimerEvent emergencyReleaseEvent =
      timer.eventAt(timer.eventCount() - 1);
  assert(emergencyReleaseEvent.state == TimerState::Open);
  assert(!emergencyReleaseEvent.sealed);
  assert(!emergencyReleaseEvent.emergencyInputActive);
  assert(!timer.setSealed(true, 10100));

  for (uint32_t index = 0; index < 12; ++index) {
    assert(timer.start(10, 11000 + index * 20));
    assert(timer.stop(false, 11001 + index * 20));
  }
  assert(timer.eventCount() == 16);  // The history remains strictly bounded.
  assert(timer.eventAt(timer.eventCount() - 1).type ==
         TimerEventType::OpenedByUser);
  assert(timer.eventAt(0).id + timer.eventCount() - 1 ==
         timer.eventAt(timer.eventCount() - 1).id);
  assert(timer.eventAt(timer.eventCount() - 1).id == timer.eventRevision());
  assert(timer.snapshot(12000).lastReleaseReason == ReleaseReason::User);

  // A momentary emergency contact caught only by the interrupt latch: the
  // pin is LOW again by the time loop() runs, yet nothing may start, resume
  // or extend while the edge is pending, and handling it releases the lock.
  assert(timer.start(60000, 13000));
  pinValues[Hardware::kEmergencyPin] = LOW;
  Hardware::emergencyLatched = true;
  assert(timer.emergencyInputActive());
  assert(!timer.adjust(1000, 13010));
  assert(timer.pause(13010));
  assert(!timer.resume(13020));
  const bool latchedEdge = Hardware::consumeEmergencyLatch();
  assert(latchedEdge && !Hardware::emergencyLatchPending());
  timer.setEmergencyInputActive(latchedEdge || Hardware::emergencyInputActive(), 13030);
  assert(timer.isOpen());
  assert(pinValues[Hardware::kLockPinA] == LOW);
  assert(timer.snapshot(13030).lastReleaseReason == ReleaseReason::Emergency);
  // The next loop pass sees the contact open and records the clear edge.
  timer.setEmergencyInputActive(Hardware::consumeEmergencyLatch() ||
                                    Hardware::emergencyInputActive(), 13040);
  assert(!timer.emergencyInputActive());
  assert(timer.eventAt(timer.eventCount() - 1).type ==
         TimerEventType::EmergencyInputCleared);
  Hardware::emergencyLatched = true;
  assert(!timer.start(1000, 13050));  // A pending edge also blocks a new start.
  assert(Hardware::consumeEmergencyLatch());
  assert(timer.start(1000, 13060));
  assert(timer.stop(false, 13070));

  // An emergency edge between start()'s check and its lock write: the write
  // undoes the interrupt's output-off, so start() must release before it
  // returns instead of leaving the lock energized until the next loop pass.
  emergencyEdgeBeforeNextLockWrite = true;
  assert(!timer.start(60000, 13080));
  assert(!emergencyEdgeBeforeNextLockWrite);
  assert(timer.isOpen() && pinValues[Hardware::kLockPinA] == LOW);
  assert(timer.snapshot(13080).lastReleaseReason == ReleaseReason::Emergency);
  assert(Hardware::consumeEmergencyLatch());
  assert(timer.start(1000, 13090));

  {
    // The owner's switches bind the controller itself, so the legacy /pause
    // and /setLockMode routes, which call it directly, honour them too.
    TimerController policy;
    policy.begin();
    policy.setControlPolicy(false, false);
    assert(policy.start(60000, 0));
    assert(!policy.pause(10));
    assert(!policy.setSealed(true, 20));
    TimerSnapshot unchanged = policy.snapshot(20);
    assert(unchanged.state == TimerState::Running && !unchanged.sealed);
    policy.setControlPolicy(true, true);
    assert(policy.pause(30));
    assert(policy.resume(40));
    assert(policy.setSealed(true, 50));
  }

  {
    // A share link's hold limit fails open by itself: paused or sealed, the
    // lock releases at the limit; the later of two limits wins; a new session
    // and every release clear it; an open timer ignores it.
    TimerController linked;
    linked.begin();
    linked.limitHoldUntil(5000);
    assert(!linked.snapshot(1000).holdLimited);
    assert(linked.start(60000, 1000));
    linked.limitHoldUntil(11000);
    linked.limitHoldUntil(6000);
    TimerSnapshot limited = linked.snapshot(1000);
    assert(limited.holdLimited && limited.holdLimitRemainingMs == 10000);
    assert(linked.pause(2000));
    linked.tick(10999);
    assert(!linked.isOpen());
    linked.tick(11000);
    assert(linked.isOpen() && pinValues[Hardware::kLockPinA] == LOW);
    assert(linked.snapshot(11000).lastReleaseReason == ReleaseReason::ShareLinkExpired);
    assert(!linked.snapshot(11000).holdLimited);
    assert(linked.eventAt(linked.eventCount() - 1).type == TimerEventType::ShareLinkExpired);
    assert(std::string(TimerController::releaseReasonName(ReleaseReason::ShareLinkExpired)) ==
           "share_link_expired");
    assert(linked.start(60000, 20000));
    linked.limitHoldUntil(25000);
    assert(linked.setSealed(true, 21000));
    assert(!linked.stop(false, 22000));  // software release still refused
    linked.tick(25000);
    assert(linked.isOpen());
    assert(linked.start(60000, 30000));
    assert(!linked.snapshot(30000).holdLimited);  // a new session starts unlimited
    linked.tick(40000);
    assert(!linked.isOpen());
  }

  std::cout << "timer safety tests passed\n";
  return 0;
}
