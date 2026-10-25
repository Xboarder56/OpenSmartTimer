#pragma once

// Experimental V2 relay runtime: the firmware-facing owner of the relay.
//
// Off by default. Nothing is allocated and no network activity happens until
// the owner enrolls or enables it through the local API. When running,
// RelayClient, the link table and TLS live in a dedicated FreeRTOS task with
// its own stack. The timer is touched only from the Arduino loop: relay
// commands are handed over through a mailbox and applied with the shared
// TimerCommandAuthority rules, exactly like REST and BLE.
//
// Every public method is called from the Arduino loop task.
#include <stddef.h>
#include <stdint.h>

#include "RelayClient.h"
#include "RelayOwnerJob.h"

class TimerController;

namespace opensmarttimer { namespace relay {

enum class RelaySetupState : uint8_t { Idle, Pending, Connecting, Complete, Cancelled, Expired, Failed };

enum class RelayOpResult : uint8_t {
    Ok,
    Accepted,        // queued, poll status; never retry with a new job ID
    Busy,            // not accepted, existing operation owns the slot
    Paused,          // paused while a Bluetooth app is connected or pairing
    Conflict,        // stale/reused job ID; no new operation was performed
    TimerActive,
    NotEnrolled,
    NotConnected,    // new links need a live relay connection
    NotRunning,
    InvalidRequest,
    OriginRequired,
    StorageFailed,
    TableFull,
    RateLimited,
    NotFound,
    NotReady,        // link not yet confirmed by the relay
    Unavailable,     // not enough memory to start
};

struct RelayLinkInfo {
    char fp_hash[24];
    char label[24];
    uint32_t expires_in_ms;
    GrantState state;
    bool url_available;
};

struct RelayStatusInfo {
    bool enabled;            // persisted preference
    bool enrolled;
    bool running;            // relay task alive
    bool enrolling;
    bool sharing_available;
    SessionState state;
    char relay_host[64];
    char timer_id[24];
    char last_error[40];
    uint32_t reconnects;
    uint32_t live_links;
    uint32_t task_stack_min_free_bytes;   // 0 until the task has run
    uint32_t free_heap_bytes;
    uint32_t largest_free_block_bytes;
    uint32_t tls_arena_bytes;               // 0 when no arena is reserved
    uint32_t tls_arena_free_bytes;
    uint32_t tls_arena_minimum_free_bytes;
    uint32_t tls_arena_fallback_count;
    bool stopping;
    bool maintenance;
    bool paused_for_bluetooth;
    uint32_t boot_id;
    uint32_t next_job_id;
    uint32_t job_id;
    RelayOwnerJob::State job_state;
    RelayOpResult job_result;
    char job_fp_hash[24];
    bool setup_pending;
    RelaySetupState setup_state;
    uint32_t setup_request_id;
    uint32_t setup_remaining_ms;
    char partner_origin[96];
    char setup_relay_origin[80];
    char setup_partner_origin[96];
};

class RelayRuntime {
public:
    static constexpr uint32_t kMaxLinkTtlMs = 24u * 60u * 60u * 1000u;
    static constexpr uint32_t kMaxLiveLinks = kTokenTableCapacity;

    RelayRuntime(TimerController& timer, const char* build_id, const char* hardware_model,
                 const char* chip_family);
    static RelayRuntime* active();

    // Reads the persisted preference; starts the task only if enabled and
    // enrolled. Call once, after NVS and the boot ID exist.
    void begin(uint32_t boot_id, const char* device_label);
    // Every loop: publishes the timer snapshot, applies queued relay
    // commands, and forwards timer events. network_up gates connections;
    // mutations_allowed is false during OTA/restart/network transitions.
    // bluetooth_active (an app connected or pairing open) pauses the relay:
    // its task stops and frees its stack; links stay in RAM; it resumes
    // after Bluetooth has been idle for a moment.
    void loop(bool network_up, bool mutations_allowed, bool bluetooth_active);
    void setIdentifyHandler(void (*handler)(void* context), void* context);

    // The pin (base64url SHA-256 SPKI from the relay operator) authenticates
    // both enrollment and every later WebSocket connection.
    RelayOpResult enroll(const char* relay_origin, const char* enrollment_code,
                         const uint8_t spki_pin[kSpkiPinBytes], const char* partner_origin);
    // Only the physical input handler calls confirmEnrollment. A request ID
    // captured on button-down binds release to the same immutable setup.
    RelayOpResult confirmEnrollment(uint32_t request_id);
    RelayOpResult cancelEnrollment(uint32_t request_id);
    bool enrollmentApprovalPending() const;
    RelayOpResult enable();
    // Ends every share link, then stops the task and closes the connection.
    RelayOpResult disable();
    // Erases credentials, links and signing keys.
    RelayOpResult forget();
    RelayOpResult mint(uint32_t job_id, uint32_t ttl_ms, const char* label);
    RelayOpResult revoke(uint32_t job_id, const char* fp_hash);
    // Adds add_ms to the link's expiry (from now if it already expired, same
    // boot only); capped at 24 h from now. Works during a session; the same
    // URL keeps working and an open partner page follows the new expiry.
    RelayOpResult extend(uint32_t job_id, const char* fp_hash, uint32_t add_ms);
    // Share URL once the relay confirmed the link; NotReady before that.
    RelayOpResult shareUrl(const char* fp_hash, char* out, size_t cap, GrantState* state);
    // Visits current links (at most kTokenTableCapacity) after a refresh.
    RelayOpResult forEachLink(void (*visit)(const RelayLinkInfo& link, void* context), void* context);
    void status(RelayStatusInfo* out) const;
    // Storage operations hold the timer open; networking never stalls loop().
    bool maintenanceActive() const;
    // Poll until Ok before erasing other settings/rebooting. StorageFailed
    // means do not claim a completed reset; a later button gesture may retry.
    RelayOpResult eraseAllForFactoryReset();

    static const char* resultName(RelayOpResult result);
    static const char* stateName(SessionState state);
    static const char* setupStateName(RelaySetupState state);

    struct Impl;

private:
    RelayOpResult enrollApproved(const char* relay_origin, const char* enrollment_code,
                                const uint8_t spki_pin[kSpkiPinBytes], const char* partner_origin);
    Impl* impl_;
};

}}  // namespace opensmarttimer::relay
