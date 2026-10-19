#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>

#include "Hardware.h"
#include "TimerCommandAuthority.h"

namespace {
std::array<uint8_t, 17> pinModes{};
std::array<uint8_t, 17> pinValues{};

constexpr uint32_t kBootId = 0x7ca9f21b;

TimerCommand command(TimerCommandOp op, int32_t valueA = 0,
                     int32_t valueB = 0) {
  TimerCommand result;
  result.op = op;
  result.valueA = valueA;
  result.valueB = valueB;
  return result;
}

void expect(const TimerCommandOutcome& outcome, TimerCommandResult result,
            bool changed = false) {
  assert(outcome.result == result);
  assert(outcome.changed == changed);
}

// Uses the interrupt latch, which counts as an asserted emergency input on
// every board profile without depending on the pin polarity.
void setEmergency(bool active) { Hardware::emergencyLatched = active; }
}  // namespace

void pinMode(uint8_t pin, uint8_t mode) { pinModes.at(pin) = mode; }
void digitalWrite(uint8_t pin, uint8_t value) { pinValues.at(pin) = value; }
int digitalRead(uint8_t pin) { return pinValues.at(pin); }

#if defined(SMART_TIMER_HARDWARE_V2)
extern "C" esp_err_t gpio_set_level(gpio_num_t gpioNumber,
                                    std::uint32_t level) {
  pinValues.at(static_cast<size_t>(gpioNumber)) =
      static_cast<uint8_t>(level);
  return ESP_OK;
}
#endif

int main() {
  pinValues[Hardware::kEmergencyPin] =
      Hardware::kInputActiveHigh ? LOW : HIGH;
  TimerController timer;
  timer.begin();
  TimerCommandAuthority authority(timer);
  authority.begin(kBootId);
  constexpr int32_t kMaximum =
      static_cast<int32_t>(Hardware::kMaximumDurationMs);

  // Text preconditions use the exact REST/relay forms.
  char session[TimerCommandAuthority::kSessionTextSize];
  authority.sessionText(session, sizeof(session));
  assert(session[0] == '\0');
  assert(authority.bootFromText("").kind == TimerPrecondition::Kind::Absent);
  assert(authority.bootFromText(nullptr).kind ==
         TimerPrecondition::Kind::Absent);
  assert(authority.bootFromText("7ca9f21b").kind ==
         TimerPrecondition::Kind::Value);
  assert(authority.bootFromText("7CA9F21B").kind ==
         TimerPrecondition::Kind::Unmatchable);
  assert(authority.bootFromText("07ca9f21b").kind ==
         TimerPrecondition::Kind::Unmatchable);
  // Before the first session, no session text can match.
  assert(authority.sessionFromText("7ca9f21b-0").kind ==
         TimerPrecondition::Kind::Unmatchable);

  // Parameters are rejected before any state is inspected.
  expect(authority.execute(command(TimerCommandOp::Start, 0), 0),
         TimerCommandResult::InvalidDuration);
  expect(authority.execute(command(TimerCommandOp::Start, kMaximum + 1), 0),
         TimerCommandResult::InvalidDuration);
  expect(authority.execute(command(TimerCommandOp::Start, 2000, 1000), 0),
         TimerCommandResult::InvalidDuration);
  expect(authority.execute(command(TimerCommandOp::Adjust, 1, 1), 0),
         TimerCommandResult::InvalidDuration);
  expect(authority.execute(command(TimerCommandOp::Pause, 1), 0),
         TimerCommandResult::InvalidParameters);
  TimerCommand staleAndMalformed = command(TimerCommandOp::Release, 1);
  staleAndMalformed.boot = TimerPrecondition::of(kBootId + 1);
  expect(authority.execute(staleAndMalformed, 0),
         TimerCommandResult::InvalidParameters);

  // Boot is checked before session; a start may not name a session.
  TimerCommand staleBoot = command(TimerCommandOp::Start, 1000);
  staleBoot.boot = TimerPrecondition::unmatchable();
  staleBoot.session = TimerPrecondition::of(4);
  expect(authority.execute(staleBoot, 0), TimerCommandResult::StaleBoot);
  TimerCommand namedStart = command(TimerCommandOp::Start, 1000);
  namedStart.session = TimerPrecondition::of(1);
  expect(authority.execute(namedStart, 0), TimerCommandResult::StaleSession);
  assert(timer.isOpen());

  // Open-state rules.
  expect(authority.execute(command(TimerCommandOp::Release), 0),
         TimerCommandResult::Applied);
  expect(authority.execute(command(TimerCommandOp::Pause), 0),
         TimerCommandResult::InvalidState);
  expect(authority.execute(command(TimerCommandOp::Resume), 0),
         TimerCommandResult::InvalidState);
  expect(authority.execute(command(TimerCommandOp::Adjust, 1000), 0),
         TimerCommandResult::InvalidState);
  expect(authority.execute(command(TimerCommandOp::Adjust, 0), 0),
         TimerCommandResult::InvalidState);
  expect(authority.execute(command(TimerCommandOp::LockControls), 0),
         TimerCommandResult::InvalidState);
  const uint32_t revisionBeforeIdentify = timer.eventRevision();
  expect(authority.execute(command(TimerCommandOp::Identify), 0),
         TimerCommandResult::Applied, true);
  assert(timer.eventRevision() == revisionBeforeIdentify + 1);
  assert(timer.eventAt(timer.eventCount() - 1).valueMs ==
         static_cast<int32_t>(TimerCommandAuthority::kIdentifyDurationMs));

  setEmergency(true);
  expect(authority.execute(command(TimerCommandOp::Start, 1000), 0),
         TimerCommandResult::EmergencyInputActive);
  setEmergency(false);
  assert(timer.isOpen());

  // Fixed start with explicit "no session" claim, as BLE sends it.
  TimerCommand start = command(TimerCommandOp::Start, 10000);
  start.boot = authority.bootFromText("7ca9f21b");
  start.session = TimerPrecondition::of(0);
  expect(authority.execute(start, 100), TimerCommandResult::Applied, true);
  assert(timer.sessionSequence() == 1);
  authority.sessionText(session, sizeof(session));
  assert(std::strcmp(session, "7ca9f21b-1") == 0);
  assert(authority.sessionFromText("7ca9f21b-1").kind ==
         TimerPrecondition::Kind::Value);
  assert(authority.sessionFromText("7ca9f21b-2").kind ==
         TimerPrecondition::Kind::Unmatchable);
  assert(authority.sessionFromText("11111111-1").kind ==
         TimerPrecondition::Kind::Unmatchable);

  // Active timer: a second start never replaces it, even with emergency.
  setEmergency(true);
  expect(authority.execute(command(TimerCommandOp::Start, 1000), 100),
         TimerCommandResult::InvalidState);
  setEmergency(false);

  TimerCommand oldSession = command(TimerCommandOp::Pause);
  oldSession.session = TimerPrecondition::of(0);
  expect(authority.execute(oldSession, 100), TimerCommandResult::StaleSession);
  oldSession.session = TimerPrecondition::unmatchable();
  expect(authority.execute(oldSession, 100), TimerCommandResult::StaleSession);

  // Requested state already true is an accepted no-op.
  expect(authority.execute(command(TimerCommandOp::Resume), 100),
         TimerCommandResult::Applied);
  expect(authority.execute(command(TimerCommandOp::Identify), 100),
         TimerCommandResult::InvalidState);
  expect(authority.execute(command(TimerCommandOp::Pause), 200),
         TimerCommandResult::Applied, true);
  expect(authority.execute(command(TimerCommandOp::Pause), 300),
         TimerCommandResult::Applied);
  expect(authority.execute(command(TimerCommandOp::LockControls), 300),
         TimerCommandResult::InvalidState);

  // A paused timer cannot resume, or be lengthened, into an asserted input.
  setEmergency(true);
  expect(authority.execute(command(TimerCommandOp::Resume), 300),
         TimerCommandResult::EmergencyInputActive);
  expect(authority.execute(command(TimerCommandOp::Adjust, 1000), 300),
         TimerCommandResult::EmergencyInputActive);
  expect(authority.execute(command(TimerCommandOp::Adjust, -1000), 300),
         TimerCommandResult::Applied, true);
  setEmergency(false);
  assert(timer.snapshot(300).remainingMs == 8900);
  expect(authority.execute(command(TimerCommandOp::Resume), 400),
         TimerCommandResult::Applied, true);

  // Positive adjustments stop at 24 hours; emergency is reported first.
  expect(authority.execute(command(TimerCommandOp::Adjust, kMaximum), 400),
         TimerCommandResult::MaximumDurationReached);
  setEmergency(true);
  expect(authority.execute(command(TimerCommandOp::Adjust, kMaximum), 400),
         TimerCommandResult::EmergencyInputActive);
  setEmergency(false);
  const uint32_t revisionBeforeNoop = timer.eventRevision();
  expect(authority.execute(command(TimerCommandOp::Adjust, 0), 400),
         TimerCommandResult::Applied);
  assert(timer.eventRevision() == revisionBeforeNoop);
  expect(authority.execute(command(TimerCommandOp::Adjust, 1000), 400),
         TimerCommandResult::Applied, true);
  assert(timer.snapshot(400).remainingMs == 9900);

  // Locked controls: idempotent requests still succeed, changes do not.
  expect(authority.execute(command(TimerCommandOp::LockControls), 500),
         TimerCommandResult::Applied, true);
  expect(authority.execute(command(TimerCommandOp::LockControls), 500),
         TimerCommandResult::Applied);
  expect(authority.execute(command(TimerCommandOp::Resume), 500),
         TimerCommandResult::Applied);
  setEmergency(true);
  expect(authority.execute(command(TimerCommandOp::Resume), 500),
         TimerCommandResult::Applied);
  setEmergency(false);
  expect(authority.execute(command(TimerCommandOp::Pause), 500),
         TimerCommandResult::ControlsLocked);
  expect(authority.execute(command(TimerCommandOp::Release), 500),
         TimerCommandResult::ControlsLocked);
  expect(authority.execute(command(TimerCommandOp::Adjust, -1000), 500),
         TimerCommandResult::ControlsLocked);
  expect(authority.execute(command(TimerCommandOp::Adjust, 0), 500),
         TimerCommandResult::ControlsLocked);
  assert(!timer.isOpen());
  // The physical path is outside the authority and still opens.
  assert(timer.stop(true, 600));
  assert(timer.isOpen());

  // Random range: inclusive and chosen from the supplied hardware value.
  TimerCommand random = command(TimerCommandOp::Start, 1000, 2000);
  random.randomValue = 1000;
  expect(authority.execute(random, 700), TimerCommandResult::Applied, true);
  assert(timer.snapshot(700).sessionDurationMs == 2000);
  expect(authority.execute(command(TimerCommandOp::Adjust, -5000), 700),
         TimerCommandResult::Applied, true);
  assert(timer.isOpen());
  assert(timer.snapshot(700).lastReleaseReason ==
         ReleaseReason::AdjustedToZero);
  random.valueB = 1000;
  random.randomValue = 0xffffffff;
  expect(authority.execute(random, 800), TimerCommandResult::Applied, true);
  assert(timer.snapshot(800).sessionDurationMs == 1000);
  expect(authority.execute(command(TimerCommandOp::Release), 800),
         TimerCommandResult::Applied, true);

  assert(std::strcmp(TimerCommandAuthority::resultName(
                         TimerCommandResult::ControlsLocked),
                     "controls_locked") == 0);

  // Operation ledger: exact replay, conflict, empty and over-long IDs.
  TimerOperationLedger ledger;
  using Match = TimerOperationLedger::Match;
  assert(ledger.match("a", TimerCommandOp::Adjust, 5, 0) == Match::New);
  ledger.remember("a", TimerCommandOp::Adjust, 5, 0);
  assert(ledger.match("a", TimerCommandOp::Adjust, 5, 0) == Match::Duplicate);
  assert(ledger.match("a", TimerCommandOp::Adjust, 6, 0) == Match::Conflict);
  assert(ledger.match("a", TimerCommandOp::Pause, 5, 0) == Match::Conflict);
  ledger.remember("", TimerCommandOp::Pause, 0, 0);
  assert(ledger.match("", TimerCommandOp::Pause, 0, 0) == Match::New);
  char longest[TimerOperationLedger::kMaximumIdLength + 1];
  std::memset(longest, 'x', sizeof(longest) - 1);
  longest[sizeof(longest) - 1] = '\0';
  ledger.remember(longest, TimerCommandOp::Pause, 0, 0);
  assert(ledger.match(longest, TimerCommandOp::Pause, 0, 0) ==
         Match::Duplicate);
  char tooLong[TimerOperationLedger::kMaximumIdLength + 2];
  std::memset(tooLong, 'y', sizeof(tooLong) - 1);
  tooLong[sizeof(tooLong) - 1] = '\0';
  ledger.remember(tooLong, TimerCommandOp::Pause, 0, 0);
  assert(ledger.match(tooLong, TimerCommandOp::Pause, 0, 0) == Match::New);
  for (int index = 0; index < TimerOperationLedger::kCapacity; ++index) {
    char id[8];
    std::snprintf(id, sizeof(id), "n%d", index);
    ledger.remember(id, TimerCommandOp::Release, 0, 0);
  }
  assert(ledger.match("a", TimerCommandOp::Adjust, 5, 0) == Match::New);
  ledger.clear();
  assert(ledger.match("n7", TimerCommandOp::Release, 0, 0) == Match::New);

  {
    // Share-link commands carry the link's remaining lifetime: a start or an
    // extension past it is refused like the 24-hour rule; shortening is not.
    TimerController linkedTimer;
    linkedTimer.begin();
    TimerCommandAuthority linked(linkedTimer);
    linked.begin(kBootId);
    TimerCommand longStart = command(TimerCommandOp::Start, 60000);
    longStart.maxHoldMs = 30000;
    expect(linked.execute(longStart, 1000), TimerCommandResult::MaximumDurationReached);
    assert(linkedTimer.isOpen());
    TimerCommand fits = command(TimerCommandOp::Start, 30000);
    fits.maxHoldMs = 30000;
    expect(linked.execute(fits, 1000), TimerCommandResult::Applied, true);
    TimerCommand extend = command(TimerCommandOp::Adjust, 5000);
    extend.session = TimerPrecondition::of(linkedTimer.sessionSequence());
    extend.maxHoldMs = 29000;
    expect(linked.execute(extend, 2000), TimerCommandResult::MaximumDurationReached);
    extend.valueA = -5000;
    expect(linked.execute(extend, 2000), TimerCommandResult::Applied, true);
    extend.valueA = 5000;
    extend.maxHoldMs = 0;  // REST and BLE: no link limit
    expect(linked.execute(extend, 2000), TimerCommandResult::Applied, true);
  }

  {
    // Owner settings: pause and control locking can each be switched off.
    // The rule sits after the already-true no-op and before the controls
    // lock, and never touches resume, release or time changes.
    TimerController policyTimer;
    policyTimer.begin();
    TimerCommandAuthority policy(policyTimer);
    policy.begin(kBootId);
    assert(policyTimer.snapshot(0).pauseAllowed && policyTimer.snapshot(0).sealAllowed);
    expect(policy.execute(command(TimerCommandOp::Start, 60000), 0),
           TimerCommandResult::Applied, true);

    policyTimer.setControlPolicy(false, false);
    assert(!policyTimer.snapshot(10).pauseAllowed && !policyTimer.snapshot(10).sealAllowed);
    expect(policy.execute(command(TimerCommandOp::Pause), 10),
           TimerCommandResult::ControlDisabled);
    expect(policy.execute(command(TimerCommandOp::LockControls), 10),
           TimerCommandResult::ControlDisabled);
    assert(policyTimer.snapshot(10).state == TimerState::Running && !policyTimer.snapshot(10).sealed);
    assert(std::string(TimerCommandAuthority::resultName(TimerCommandResult::ControlDisabled)) == "control_disabled");
    // Everything that shortens or ends the session still works.
    expect(policy.execute(command(TimerCommandOp::Adjust, -1000), 20),
           TimerCommandResult::Applied, true);
    expect(policy.execute(command(TimerCommandOp::Adjust, 1000), 20),
           TimerCommandResult::Applied, true);

    // Each switch is independent.
    policyTimer.setControlPolicy(true, false);
    expect(policy.execute(command(TimerCommandOp::Pause), 30),
           TimerCommandResult::Applied, true);
    // A session paused while pause was allowed can always be resumed, and a
    // repeated pause stays an accepted no-op, even after the switch goes off.
    policyTimer.setControlPolicy(false, false);
    expect(policy.execute(command(TimerCommandOp::Pause), 40),
           TimerCommandResult::Applied);
    expect(policy.execute(command(TimerCommandOp::Resume), 50),
           TimerCommandResult::Applied, true);
    expect(policy.execute(command(TimerCommandOp::LockControls), 60),
           TimerCommandResult::ControlDisabled);

    policyTimer.setControlPolicy(false, true);
    expect(policy.execute(command(TimerCommandOp::LockControls), 70),
           TimerCommandResult::Applied, true);
    // Already locked: a repeat is an accepted no-op whatever the switch says,
    // and an existing lock is never lifted by changing the setting.
    policyTimer.setControlPolicy(false, false);
    expect(policy.execute(command(TimerCommandOp::LockControls), 80),
           TimerCommandResult::Applied);
    assert(policyTimer.snapshot(80).sealed);
    // With controls locked, a switched-off pause reports the switch, not the lock.
    expect(policy.execute(command(TimerCommandOp::Pause), 90),
           TimerCommandResult::ControlDisabled);
    expect(policy.execute(command(TimerCommandOp::Release), 90),
           TimerCommandResult::ControlsLocked);
  }

  std::cout << "Timer command authority rule-order and ledger tests passed\n";
  return 0;
}
