#include "TimerAuthorityControl.h"

#include <stdio.h>
#include <string.h>

#include "Hardware.h"

namespace opensmarttimer { namespace relay {

namespace {
void copyText(char* out, size_t size, const char* text) {
    if (!size) return;
    strncpy(out, text ? text : "", size - 1);
    out[size - 1] = '\0';
}

bool emptyText(const char* text) { return text == nullptr || text[0] == '\0'; }
}  // namespace

TimerAuthorityControl::TimerAuthorityControl(TimerController& timer,
                                             IClock& clock,
                                             const char* build_id)
    : timer_(timer), authority_(timer), clock_(clock) {
    copyText(build_id_, sizeof(build_id_), build_id);
}

void TimerAuthorityControl::begin(uint32_t boot_id) {
    authority_.begin(boot_id);
    ledger_.clear();
}

void TimerAuthorityControl::setDeviceLabel(const char* label) {
    copyText(device_label_, sizeof(device_label_), label);
}

void TimerAuthorityControl::snapshot(StatusSnapshot* out) {
    if (!out) return;
    const uint32_t now = clock_.uptime_ms();
    const TimerSnapshot timer = timer_.snapshot(now);
    *out = StatusSnapshot{};
    out->state = timer.state == TimerState::Running ? StatusSnapshot::State::Running
        : timer.state == TimerState::Paused ? StatusSnapshot::State::Paused
        : StatusSnapshot::State::Open;
    out->remaining_ms = timer.remainingMs;
    out->session_duration_ms = timer.sessionDurationMs;
    out->max_duration_ms = Hardware::kMaximumDurationMs;
    out->uptime_ms = now;
    // Timer-event revision; advances on every timer state change. Network-only
    // changes that bump the REST state_revision are not relay status.
    out->state_revision = timer_.eventRevision();
    out->session_sequence = timer_.sessionSequence();
    out->sealed = timer.sealed;
    out->emergency_input_active = timer.emergencyInputActive;
    out->pause_disabled = !timer.pauseAllowed;
    out->control_lock_disabled = !timer.sealAllowed;
    snprintf(out->boot_id, sizeof(out->boot_id), "%lx",
             static_cast<unsigned long>(authority_.bootId()));
    authority_.sessionText(out->session_id, sizeof(out->session_id));
    copyText(out->build_id, sizeof(out->build_id), build_id_);
    copyText(out->device_label, sizeof(out->device_label), device_label_);
    out->hold_limited = timer.holdLimited;
    out->hold_limit_remaining_ms = timer.holdLimitRemainingMs;
    if (timer_.eventCount()) {
        const TimerEvent event = timer_.eventAt(timer_.eventCount() - 1);
        copyText(out->last_event, sizeof(out->last_event), TimerController::eventName(event.type));
        out->last_event_value_ms = event.valueMs;
#if defined(SMART_TIMER_HARDWARE_V2)
        out->last_event_via_link = event.viaShareLink;
#endif
    }
}

MutationOutcome TimerAuthorityControl::apply(const MutationRequest& req) {
    TimerCommand command;
    if (!commandOp(req.op, &command.op) || emptyText(req.op_id)
        || emptyText(req.if_boot_id)) {
        return {ResultCode::InvalidFrame, false, false};
    }
    // Relay start is fixed-duration only; random ranges are not in the
    // ost-relay/1 command contract.
    if (req.value_a < INT32_MIN || req.value_a > INT32_MAX || req.value_b != 0) {
        return {ResultCode::InvalidDuration, false, false};
    }
    command.valueA = static_cast<int32_t>(req.value_a);

    // Exact successful replays are answered before the gate or any
    // precondition, matching REST and BLE retry semantics.
    const TimerOperationLedger::Match match =
        ledger_.match(req.op_id, command.op, command.valueA, command.valueB);
    if (match == TimerOperationLedger::Match::Conflict) {
        return {ResultCode::OperationConflict, false, false};
    }
    if (match == TimerOperationLedger::Match::Duplicate) {
        return {ResultCode::Applied, false, true};
    }
    if (!mutations_allowed_) {
        return {ResultCode::SystemTransition, false, false};
    }
    const uint32_t now = clock_.uptime_ms();
    if ((req.command_expires_ms || req.token_expires_ms) &&
        (now < req.authorized_at_ms || now >= req.command_expires_ms ||
         now >= req.token_expires_ms)) {
        return {ResultCode::Expired, false, false};
    }

    command.boot = authority_.bootFromText(req.if_boot_id);
    // The partner page sends the session it saw at click time. Start and
    // identify act on an open timer, so an ended session is not stale for
    // them. Other operations must name the current session; an omitted
    // session only matches before the first session of this boot.
    if (command.op != TimerCommandOp::Start && command.op != TimerCommandOp::Identify) {
        command.session = emptyText(req.if_session_id)
            ? TimerPrecondition::of(0)
            : authority_.sessionFromText(req.if_session_id);
    }

    // A share link never keeps the lock engaged beyond its own lifetime: it
    // cannot start or extend past its expiry, and anything it does to hold
    // the session (start, extend, pause, seal) makes the timer open by
    // itself at that expiry, even if the relay is unreachable by then.
    if (req.token_expires_ms) command.maxHoldMs = req.token_expires_ms - now;
    timer_.setShareLinkEventSource(true);  // "via share link" in Recent activity
    const TimerCommandOutcome outcome =
        authority_.execute(command, now);
    timer_.setShareLinkEventSource(false);
    if (outcome.result == TimerCommandResult::Applied) {
        const bool holds = command.op == TimerCommandOp::Start || command.op == TimerCommandOp::Pause
            || command.op == TimerCommandOp::LockControls
            || (command.op == TimerCommandOp::Adjust && command.valueA > 0);
        // Only a command that changed the session holds it; a repeated Pause
        // or Lock from another link must not move an earlier deadline.
        if (req.token_expires_ms && holds && outcome.changed) timer_.limitHoldUntil(req.token_expires_ms);
        ledger_.remember(req.op_id, command.op, command.valueA, command.valueB);
        if (command.op == TimerCommandOp::Identify && outcome.changed && identify_handler_)
            identify_handler_(identify_context_);
    }
    return {resultCode(outcome.result), outcome.changed, false};
}

bool TimerAuthorityControl::commandOp(TimerOp op, TimerCommandOp* out) {
    switch (op) {
        case TimerOp::Start:        *out = TimerCommandOp::Start; return true;
        case TimerOp::Release:      *out = TimerCommandOp::Release; return true;
        case TimerOp::Pause:        *out = TimerCommandOp::Pause; return true;
        case TimerOp::Resume:       *out = TimerCommandOp::Resume; return true;
        case TimerOp::Adjust:       *out = TimerCommandOp::Adjust; return true;
        case TimerOp::LockControls: *out = TimerCommandOp::LockControls; return true;
        case TimerOp::Identify:     *out = TimerCommandOp::Identify; return true;
    }
    return false;
}

ResultCode TimerAuthorityControl::resultCode(TimerCommandResult result) {
    switch (result) {
        case TimerCommandResult::Applied:                return ResultCode::Applied;
        case TimerCommandResult::StaleBoot:              return ResultCode::StaleBoot;
        case TimerCommandResult::StaleSession:           return ResultCode::StaleSession;
        case TimerCommandResult::InvalidState:           return ResultCode::InvalidState;
        case TimerCommandResult::InvalidDuration:        return ResultCode::InvalidDuration;
        case TimerCommandResult::EmergencyInputActive:   return ResultCode::EmergencyInputActive;
        case TimerCommandResult::ControlsLocked:         return ResultCode::ControlsLocked;
        case TimerCommandResult::MaximumDurationReached: return ResultCode::MaximumDurationReached;
        case TimerCommandResult::ControlDisabled:        return ResultCode::ControlDisabled;
        case TimerCommandResult::InvalidParameters:
        default:                                         return ResultCode::InvalidFrame;
    }
}

}  // namespace relay
}  // namespace opensmarttimer
