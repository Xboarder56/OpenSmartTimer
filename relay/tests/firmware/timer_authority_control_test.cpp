// Relay ITimerControl adapter over the real firmware TimerController and
// shared TimerCommandAuthority (compiled from the repository root). Proves
// relay commands obey the same rules as REST/BLE without a relay-side copy.

#include <array>
#include <cassert>
#include <cstdio>
#include <cstring>

#include "Fakes.h"
#include "Hardware.h"
#include "TimerAuthorityControl.h"

using namespace opensmarttimer::relay;

namespace {
std::array<uint8_t, 17> pinValues{};

MutationRequest request(TimerOp op, const char* id, const char* boot,
                        const char* session, int64_t a = 0, int64_t b = 0) {
    return MutationRequest{op, id, boot, session, a, b};
}

void expect(const MutationOutcome& outcome, ResultCode code,
            bool changed = false, bool duplicate = false) {
    assert(outcome.code == code);
    assert(outcome.changed == changed);
    assert(outcome.duplicate == duplicate);
}
}  // namespace

void pinMode(uint8_t, uint8_t) {}
void digitalWrite(uint8_t pin, uint8_t value) { pinValues.at(pin) = value; }
int digitalRead(uint8_t pin) { return pinValues.at(pin); }
extern "C" esp_err_t gpio_set_level(gpio_num_t gpioNumber, std::uint32_t level) {
    pinValues.at(static_cast<size_t>(gpioNumber)) = static_cast<uint8_t>(level);
    return ESP_OK;
}

int main() {
    pinValues[Hardware::kEmergencyPin] = Hardware::kInputActiveHigh ? LOW : HIGH;
    TimerController timer;
    timer.begin();
    test::FakeClock clock;
    clock.set(1000);
    TimerAuthorityControl control(timer, clock, "0123456789abcdef01234567");
    control.begin(0xaabbccdd);
    control.setDeviceLabel("Bedroom");

    StatusSnapshot snap{};
    control.snapshot(&snap);
    assert(snap.state == StatusSnapshot::State::Open);
    assert(std::strcmp(snap.boot_id, "aabbccdd") == 0);
    assert(snap.session_id[0] == '\0');
    assert(snap.session_sequence == 0);
    assert(snap.max_duration_ms == Hardware::kMaximumDurationMs);
    assert(std::strcmp(snap.build_id, "0123456789abcdef01234567") == 0);
    assert(std::strcmp(snap.device_label, "Bedroom") == 0);
    // The owner's settings reach the partner page through the status body,
    // and a switched-off control is refused for a share link like any other.
    assert(!snap.pause_disabled && !snap.control_lock_disabled);
    timer.setControlPolicy(false, false);
    control.snapshot(&snap);
    assert(snap.pause_disabled && snap.control_lock_disabled);
    timer.setControlPolicy(true, true);

    // The transition gate starts closed and never blocks an exact replay.
    auto start = request(TimerOp::Start, "s1", "aabbccdd", "", 60000);
    expect(control.apply(start), ResultCode::SystemTransition);
    assert(timer.isOpen());
    control.setMutationsAllowed(true);

    // Validation before enqueue is not sufficient: expire at the execution
    // boundary, including token expiry and uint32 uptime rollover.
    auto queued = request(TimerOp::Start, "expired", "aabbccdd", "", 60000);
    queued.authorized_at_ms = 900;
    queued.command_expires_ms = 1000;
    queued.token_expires_ms = 2000;
    expect(control.apply(queued), ResultCode::Expired);
    queued.command_expires_ms = 2000;
    queued.token_expires_ms = 1000;
    expect(control.apply(queued), ResultCode::Expired);
    queued.command_expires_ms = queued.token_expires_ms = UINT32_MAX;
    queued.authorized_at_ms = UINT32_MAX - 100;
    expect(control.apply(queued), ResultCode::Expired);
    assert(timer.isOpen());

    expect(control.apply(request(TimerOp::Start, "bad", "", "", 60000)),
           ResultCode::InvalidFrame);
    expect(control.apply(request(TimerOp::Start, "", "aabbccdd", "", 60000)),
           ResultCode::InvalidFrame);
    expect(control.apply(request(TimerOp::Start, "r", "aabbccdd", "", 1000, 2000)),
           ResultCode::InvalidDuration);
    expect(control.apply(request(TimerOp::Start, "big", "aabbccdd", "", 1LL << 32)),
           ResultCode::InvalidDuration);
    expect(control.apply(request(TimerOp::Start, "old", "11223344", "", 60000)),
           ResultCode::StaleBoot);

    expect(control.apply(start), ResultCode::Applied, true);
    control.snapshot(&snap);
    assert(snap.state == StatusSnapshot::State::Running);
    assert(std::strcmp(snap.session_id, "aabbccdd-1") == 0);
    control.setMutationsAllowed(false);
    expect(control.apply(start), ResultCode::Applied, false, true);
    expect(control.apply(request(TimerOp::Start, "s1", "aabbccdd", "", 1000)),
           ResultCode::OperationConflict);
    control.setMutationsAllowed(true);

    // Non-start operations must name the live session.
    // A control the owner switched off is refused for a share link too.
    timer.setControlPolicy(false, true);
    expect(control.apply(request(TimerOp::Pause, "off", "aabbccdd", "aabbccdd-1")),
           ResultCode::ControlDisabled);
    timer.setControlPolicy(true, true);
    expect(control.apply(request(TimerOp::Pause, "p0", "aabbccdd", "")),
           ResultCode::StaleSession);
    expect(control.apply(request(TimerOp::Pause, "p0", "aabbccdd", "aabbccdd-2")),
           ResultCode::StaleSession);
    expect(control.apply(request(TimerOp::Pause, "p1", "aabbccdd", "aabbccdd-1")),
           ResultCode::Applied, true);
    expect(control.apply(request(TimerOp::Pause, "p2", "aabbccdd", "aabbccdd-1")),
           ResultCode::Applied);

    // Same emergency interlock as REST/BLE.
    Hardware::emergencyLatched = true;
    expect(control.apply(request(TimerOp::Resume, "r1", "aabbccdd", "aabbccdd-1")),
           ResultCode::EmergencyInputActive);
    expect(control.apply(request(TimerOp::Adjust, "a1", "aabbccdd", "aabbccdd-1", 1000)),
           ResultCode::EmergencyInputActive);
    Hardware::emergencyLatched = false;
    expect(control.apply(request(TimerOp::Resume, "r1", "aabbccdd", "aabbccdd-1")),
           ResultCode::Applied, true);
    expect(control.apply(request(TimerOp::Adjust, "a2", "aabbccdd", "aabbccdd-1",
                                 static_cast<int64_t>(Hardware::kMaximumDurationMs))),
           ResultCode::MaximumDurationReached);

    // Controls lock: an idempotent resume is accepted, releasing is not.
    expect(control.apply(request(TimerOp::LockControls, "l1", "aabbccdd", "aabbccdd-1")),
           ResultCode::Applied, true);
    expect(control.apply(request(TimerOp::Resume, "r2", "aabbccdd", "aabbccdd-1")),
           ResultCode::Applied);
    expect(control.apply(request(TimerOp::Release, "x1", "aabbccdd", "aabbccdd-1")),
           ResultCode::ControlsLocked);
    expect(control.apply(request(TimerOp::Identify, "i1", "aabbccdd", "aabbccdd-1")),
           ResultCode::InvalidState);
    assert(timer.stop(true, clock.uptime_ms()));

    // Start and identify ignore an ended session seen at click time.
    int identified = 0;
    control.setIdentifyHandler([](void* count) { ++*static_cast<int*>(count); }, &identified);
    expect(control.apply(request(TimerOp::Identify, "i2", "aabbccdd", "aabbccdd-1")),
           ResultCode::Applied, true);
    assert(identified == 1);
    expect(control.apply(request(TimerOp::Identify, "i2", "aabbccdd", "aabbccdd-1")),
           ResultCode::Applied, false, true);
    assert(identified == 1);  // an exact replay does not blink again
    expect(control.apply(request(TimerOp::Start, "s2", "aabbccdd", "aabbccdd-1", 5000)),
           ResultCode::Applied, true);
    expect(control.apply(request(TimerOp::Release, "x2", "aabbccdd", "aabbccdd-2")),
           ResultCode::Applied, true);
    assert(timer.isOpen());

    // Share links never keep the lock engaged past their own expiry.
    {
        const uint32_t now = clock.uptime_ms();
        auto fromLink = [now](MutationRequest r) {  // as RelayClient fills them
            r.token_expires_ms = now + 30000;
            r.command_expires_ms = now + 10000;
            r.authorized_at_ms = now;
            return r;
        };
        MutationRequest tooLong = request(TimerOp::Start, "L1", "aabbccdd", "", 60000);
        expect(control.apply(fromLink(tooLong)), ResultCode::MaximumDurationReached);
        MutationRequest fits = request(TimerOp::Start, "L2", "aabbccdd", "", 20000);
        expect(control.apply(fromLink(fits)), ResultCode::Applied, true);
        TimerSnapshot held = timer.snapshot(now);
        assert(held.holdLimited && held.holdLimitRemainingMs == 30000);
        char session[24];
        std::snprintf(session, sizeof(session), "aabbccdd-%lu",
                      static_cast<unsigned long>(timer.sessionSequence()));
        // A partner pause holds the session; the link's expiry still opens it.
        MutationRequest hold = request(TimerOp::Pause, "L3", "aabbccdd", session);
        expect(control.apply(fromLink(hold)), ResultCode::Applied, true);
        // A second, longer link repeating Pause changes nothing, so it must
        // not move the first link's fail-open deadline.
        MutationRequest again = request(TimerOp::Pause, "L4", "aabbccdd", session);
        again.token_expires_ms = now + 60000;
        again.command_expires_ms = now + 10000;
        again.authorized_at_ms = now;
        expect(control.apply(again), ResultCode::Applied, false);
        assert(timer.snapshot(now).holdLimitRemainingMs == 30000);
        timer.tick(now + 29999);
        assert(!timer.isOpen());
        timer.tick(now + 30000);
        assert(timer.isOpen());
        assert(timer.snapshot(now + 30000).lastReleaseReason == ReleaseReason::ShareLinkExpired);
        assert(pinValues[Hardware::kLockPinA] == LOW);
    }

    // A new boot forgets remembered IDs and rejects old-boot commands.
    control.begin(0x01020304);
    expect(control.apply(start), ResultCode::StaleBoot);

    std::printf("relay TimerAuthorityControl uses the shared firmware rules\n");
    return 0;
}
