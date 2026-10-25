#pragma once

// RelayClient — the timer side of an ost-relay/1 session: hello
// authentication, identity-signed ECDH channels, encrypted replies, command dispatch and
// link (grant) synchronization. Transport, storage and timer access are
// injected; on the timer they come from esp/ and run in the relay task
// (see RelayRuntime.h).
#include <stdint.h>
#include <stddef.h>
#include "RelayEnvelope.h"
#include "RelaySecure.h"
#include "CapabilityToken.h"
#include "ITimerControl.h"

namespace opensmarttimer { namespace relay {

struct EnrollmentCredentials {
    char timer_id[24];
    char wss_endpoint[128];
    uint8_t enrollment_secret[kEnrollmentSecretBytes];
    // The pin the owner approved for the enrollment request. The WebSocket no
    // longer checks a stored pin (see IWssTransport::peerKeyDigest); the three
    // fields keep the saved record's layout so existing enrollments still load.
    uint8_t spki_pin_primary[kSpkiPinBytes];
    uint8_t spki_pin_alternate[kSpkiPinBytes];
    uint32_t alternate_pin_expires_up_ms;
};
class IClock;
class IWssTransport;

enum class SessionState : uint8_t {
    Disabled, NotEnrolled, Backoff, Connecting, AwaitingChallenge,
    HelloSent, Live, Failed,
};

// A command result kept in RAM for same-boot replay after a reconnect; not
// persisted. Pending records cannot be evicted. Acknowledged records form a
// bounded retry cache; TimerAuthorityControl's operation ledger covers
// replays after a record is evicted.
struct ResultRecord {
    uint64_t upstream_seq;
    uint64_t downstream_seq;
    TimerOp op;
    char op_id[65];
    ResultCode code;
    bool changed;
    bool duplicate;
    bool occupied;
    bool pending;
    uint8_t request_hash[32];
    StatusSnapshot status;
    char secure_client_id[24];
};
constexpr size_t kResultRingSize = 8;

// Memory a new TLS connection (enrollment or session) waits for. The largest
// TLS allocation is its ~17 KB receive buffer; 20 KB clears it without letting
// fragmentation after Bluetooth use keep sharing offline with plenty free.
constexpr size_t kTlsHeapFloorFreeBytes = 40 * 1024;
constexpr size_t kTlsHeapFloorLargestBlockBytes = 20 * 1024;
// On V2 the relay reserves this much once, at start, as a private heap for
// its TLS allocations (TlsArena), and the floors above are then measured in
// it. Idle, it must still clear both floors after the relay's long-lived
// allocations.
constexpr size_t kTlsArenaBytes = 48 * 1024;
static_assert(kTlsArenaBytes >= kTlsHeapFloorFreeBytes + 4 * 1024,
              "the idle TLS arena must clear the heap floor");

enum class GrantState : uint8_t { Unknown, Pending, Ready, Revoking, Revoked, Expired, Error };
struct GrantSync {
    char fp_hash[24];
    char digest[44];
    char boot_id[24];
    char label[24];
    uint64_t seq;
    uint32_t expiry;
    uint32_t sent_at;
    uint16_t scope_mask;
    GrantState state;
    bool revoked;
};

struct RelayDiagnostics {
    SessionState state;
    uint32_t reconnect_count;
    uint32_t dropped_command_count;
    uint32_t last_backoff_ms;
    uint32_t ms_in_current_state;
    uint64_t last_upstream_seq;
    uint64_t last_downstream_seq;
    uint32_t bytes_out;
    uint32_t bytes_in;
    bool relay_reachable;
    uint32_t result_ring_used;
    const char* last_failure;   // why the last session ended; "" if none
};

class RelayClient {
public:
    struct Deps {
        ITimerControl* timer;
        CapabilityToken* tokens;
        IClock* clock;
        IWssTransport* transport;
    };
    explicit RelayClient(const Deps& deps);
    bool begin();
    void loop();
    bool saveEnrollmentCredentials(const EnrollmentCredentials& cred);
    bool clearEnrollmentCredentials();
    RelayDiagnostics diagnostics() const;
    void wipeAll();
    size_t emergencyShutdown();
    bool lastPersistenceSucceeded() const { return persistence_ok_; }
    void onStatusChanged();
    void onEventEmitted(uint32_t event_id);
    // True means queued locally, NOT registered. Only a matching token_ack
    // changes grantState() to Ready. Metadata survives same-boot reconnects.
    bool announceMintedToken(const char* wire, size_t wire_len);
    bool announceRevokedToken(const char* fp_hash);
    // Re-announces an extended link (same fingerprint and digest, later
    // expiry). False if no grant is tracked for it.
    bool announceExtendedToken(const char* fp_hash, uint32_t expires_up_ms);
    uint32_t liveLinkExpiry() const;
    GrantState grantState(const char* fp_hash) const;
    bool canQueueGrant() const;
    bool grantSlotFree(const GrantSync& g) const;
    // Drops all grant sync state, e.g. after enrolling with another relay
    // identity whose database never saw these links.
    void forgetGrants();
    bool sharingAvailable() const;
    static const char* grantStateName(GrantState state);

private:
    SecureSession secure_sessions_[kSecureSessions];
    SecureSession* active_secure_ = nullptr;
    uint32_t last_secure_open_ = 0;
    bool secure_open_seen_ = false;
    void handleSecureOpen(const char* body, size_t len);
    void handleSecureData(const char* body, size_t len);
    bool sendSecure(SecureSession& session, const char* plain, size_t len, const char* request);
    bool sendSecureAccept(SecureSession& session);
    void sweepSecure();
    void sendSecureStatus(const char* client_id, const char* request);
    GrantSync grants_[kTokenTableCapacity];
    void sendGrant(GrantSync& grant);
    void syncGrants(bool reconnect = false);
    void handleTokenAck(const char* body, size_t len);
    Deps deps_;
    SessionState state_;
    uint32_t next_backoff_ms_;
    uint32_t state_entered_ms_;
    uint64_t last_upstream_seq_;
    uint64_t last_downstream_seq_seen_;
    uint64_t last_acked_upstream_seq_;
    EnrollmentCredentials cred_;
    ResultRecord result_ring_[kResultRingSize];
    uint8_t result_ring_head_;
    uint32_t last_command_applied_up_ms_;
    bool command_applied_;
    uint32_t last_status_ms_;
    uint32_t last_received_ms_;
    uint32_t last_loop_uptime_;
    uint32_t reconnect_count_;
    uint32_t dropped_command_count_;
    bool persistence_ok_;
    uint8_t server_nonce_[16];
    uint8_t hello_tag_[kHelloTagBytes] = {};   // this connection's hello tag; input to the acknowledgement check
    const char* last_failure_ = "";  // static string from failTransport()

    void transitionTo(SessionState next);
    void failTransport(const char* reason);
    void openTransportOrRebackoff();
    bool computeAndSendHello();
    bool acknowledgementValid(const char* body, size_t len);
    void onFrame(const char* body, size_t len);
    void handleDownstreamCommand(const uint8_t* body, size_t len);
    void handleDownstreamAck(uint64_t acked_seq);
    void handleDownstreamPing();
    void emitResult(uint64_t dseq, const char* op, const char* op_id,
                    ResultCode code, bool changed, bool duplicate = false);
    bool sendResponseFrame(const char* type, uint64_t seq,
                          const char* body_bytes, size_t body_len,
                          const char* client_id = nullptr, const char* request_id = "");
    void sendResultUpstream(const ResultRecord& r);
    void replayResultRingAfterResume();
    ResultRecord* availableResultSlot();

    // Missing platform adapters fail closed. Tests override these hooks.
    bool readEnrollmentCredentialsFromNvs();
    bool writeEnrollmentCredentialsToNvs();
    bool eraseEnrollmentCredentialsFromNvs();
    bool readEnabledPreferenceFromNvs();
    bool setEnabledPreferencePersisted(bool enabled);
    friend void frameHandlerTrampoline_(void*, const char*, size_t);
};

}}  // namespace opensmarttimer::relay
