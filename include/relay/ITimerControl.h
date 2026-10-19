#pragma once

// Adapter the RelayClient uses to observe and mutate the timer.
//
// The real TimerController exposes a richer surface (state machine,
// event history, indicator management, GPIO). RelayClient only needs
// a narrow slice — one observation call, one mutation call — so the
// integration layer wraps TimerController behind this interface.
// TimerAuthorityControl is the concrete adapter: it applies commands through
// the firmware's shared TimerCommandAuthority (docs/RELAY_FIRMWARE.md).
//
// Rationale: this keeps RelayClient decoupled from
// TimerController's exact API, so refactors on either side stay
// local. It also makes host tests trivial: a fake ITimerControl
// implementation can substitute for the whole timer state machine.

#include <stdint.h>
#include <stddef.h>

#include "RelayEnvelope.h"

namespace opensmarttimer { namespace relay {

// A compact status snapshot for outbound `status` frames. Serialized
// verbatim into JSON body matching local/status.schema.json. The
// adapter fills every field defined here from TimerController state.
struct StatusSnapshot {
    // These match the enum in docs/schemas/local/common.schema.json.
    enum class State : uint8_t { Open, Running, Paused };

    State    state;
    uint32_t remaining_ms;
    uint32_t session_duration_ms;
    uint32_t max_duration_ms;
    uint32_t uptime_ms;
    uint32_t state_revision;
    uint32_t session_sequence;   // 0 before the first session of a boot
    bool     sealed;
    bool     emergency_input_active;
    // Owner settings; a partner page hides a control that is switched off.
    // Stored as "disabled" so a zeroed snapshot means allowed and the struct
    // stays trivially copyable (it is cleared with memset).
    bool     pause_disabled;
    bool     control_lock_disabled;
    // NUL-terminated boot_id + session_id (may be empty for session_id).
    char     boot_id[24];
    char     session_id[32];
    // 24-hex build_id from local/status.schema.json, so status frames
    // satisfy the schema they claim to match.
    char     build_id[25];       // 24 hex chars + NUL
    // Optional label used by the partner SPA header. May be empty.
    char     device_label[33];     // configured name: 32 printable bytes + NUL
    // A share link limits this session: it opens by itself in this many ms.
    bool     hold_limited;
    uint32_t hold_limit_remaining_ms;
    // Most recent timer event, for the partner page's activity list.
    char     last_event[32];
    int32_t  last_event_value_ms;
    bool     last_event_via_link;
    // Current expiry (timer uptime) of the live share link, 0 if none. Set by
    // RelayClient; the partner page follows it, so an owner Extend reaches
    // an open page without a new URL.
    uint32_t link_expires_up_ms;
};

// Ops the relay may forward. Ordered to match RELAY_PROTOCOL.md §4 / §7.1.
enum class TimerOp : uint8_t {
    Start = 1,
    Release,
    Pause,
    Resume,
    Adjust,
    LockControls,
    Identify,
};

struct MutationRequest {
    TimerOp     op;
    const char* op_id;             // NUL-terminated, ≤64 bytes
    const char* if_boot_id;        // required; empty string not allowed
    const char* if_session_id;     // may be empty when op == Start
    // Parameters vary by op. Fields the adapter is expected to read:
    //   Start:   duration_ms (uint32) and duration_random_min/max
    //   Adjust:  delta_ms (int32)
    //   others:  unused
    int64_t     value_a;
    int64_t     value_b;
    // Mandatory for actual relay commands; zero defaults allow existing
    // direct authority tests. Checked again immediately before mutation.
    uint32_t    command_expires_ms = 0;
    uint32_t    token_expires_ms = 0;
    uint32_t    authorized_at_ms = 0;
    // fp_hash of the link that authorized the command, so a revocation
    // accepted after verification still refuses it before mutation.
    const char* token_fp_hash = nullptr;
};

struct MutationOutcome {
    ResultCode code;
    bool       changed;
    bool       duplicate;
};

class ITimerControl {
public:
    virtual ~ITimerControl() = default;

    // Fill `out` with the current snapshot. MUST NOT block.
    virtual void snapshot(StatusSnapshot* out) = 0;

    // Apply the requested mutation. TimerAuthorityControl routes it
    // through TimerCommandAuthority, so the ordinary preconditions
    // (emergency, sealed, max_duration, if_boot_id / if_session_id) apply
    // exactly as they do for REST and BLE callers, plus operation-ID
    // deduplication and the shared system-transition gate.
    virtual MutationOutcome apply(const MutationRequest& req) = 0;
};

// Free helper the platform provides. Returns the largest single
// contiguous heap allocation currently available, in bytes.
// RelayClient uses this in its heap-floor gate before opening a WSS
// session (see docs/RELAY_FIRMWARE.md). Return 0 on platforms without a
// meaningful measurement (host tests, etc.); the gate falls back to a
// permissive default.
size_t heapLargestFreeBlock();

// Total currently-free heap, in bytes. Same fallback rule as above.
size_t heapFree();

}  // namespace relay
}  // namespace opensmarttimer
