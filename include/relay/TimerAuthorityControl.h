#pragma once

// ITimerControl backed by the firmware's shared TimerCommandAuthority
// (root include/TimerCommandAuthority.h). Relay commands therefore obey the
// exact rule order REST and BLE use: parameters, boot, session, idempotent
// no-op, controls lock, state, emergency input and the 24-hour maximum.
//
// The adapter adds only what is relay-specific: the wire text preconditions,
// a relay-owned operation ledger that survives RelayClient result-ring
// eviction, and the shared REST/BLE system-transition gate, which starts
// closed until the integration wires it.

#include <stddef.h>
#include <stdint.h>

#include "IClock.h"
#include "ITimerControl.h"
#include "TimerCommandAuthority.h"

namespace opensmarttimer { namespace relay {

class TimerAuthorityControl : public ITimerControl {
public:
    // build_id: 24 lowercase hex characters from SMART_TIMER_BUILD_ID.
    TimerAuthorityControl(TimerController& timer, IClock& clock,
                          const char* build_id);

    // Call with WebApplication::bootId() at boot; clears remembered IDs.
    void begin(uint32_t boot_id);
    // Mirror !WebApplication::mutationTransitionActive() every loop.
    void setMutationsAllowed(bool allowed) { mutations_allowed_ = allowed; }
    void setDeviceLabel(const char* label);
    // Called after an applied, changed Identify so the owner can run the
    // same LED window the REST and BLE paths start.
    void setIdentifyHandler(void (*handler)(void* context), void* context) {
        identify_handler_ = handler; identify_context_ = context;
    }

    void snapshot(StatusSnapshot* out) override;
    MutationOutcome apply(const MutationRequest& req) override;

    static bool commandOp(TimerOp op, TimerCommandOp* out);
    static ResultCode resultCode(TimerCommandResult result);

private:
    TimerController& timer_;
    TimerCommandAuthority authority_;
    TimerOperationLedger ledger_;
    IClock& clock_;
    char build_id_[25] = {};
    char device_label_[33] = {};
    bool mutations_allowed_ = false;
    void (*identify_handler_)(void*) = nullptr;
    void* identify_context_ = nullptr;
};

}  // namespace relay
}  // namespace opensmarttimer
