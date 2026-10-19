// RelayClient.cpp — outbound WSS session for ost-relay/1. Owns the
// state machine, hello HMAC, ack cadence, downstream command dispatch,
// in-RAM result ring, cooperative floor, and heap-floor gate.
//
// The transport itself (TLS handshake, WebSocket framing, SPKI pin
// enforcement) lives behind IWssTransport; this file is transport-
// agnostic.

#include "RelayClient.h"
#include <memory>
#include <new>
#include "IWssTransport.h"
#include "IClock.h"
#include "Crypto.h"
#include "RelayJson.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <initializer_list>

namespace opensmarttimer { namespace relay {

namespace {

// ---------------------------------------------------------------------------
// Tuning constants
// ---------------------------------------------------------------------------

constexpr uint32_t kBackoffBaseMs   = 1000;
constexpr uint32_t kBackoffCapMs    = 60 * 1000;
constexpr uint32_t kCooperativeFloorMs = 250;   // RELAY_PROTOCOL.md §10
constexpr size_t   kOutboundBufBytes   = 2048;  // per-frame scratch
constexpr size_t   kInboundMaxBytes    = 4096;  // enforce upstream cap
constexpr size_t   kHeapFloorFreeBytes         = kTlsHeapFloorFreeBytes;
constexpr size_t   kHeapFloorLargestBlockBytes = kTlsHeapFloorLargestBlockBytes;

// Utility: monotonic-safe uptime delta accounting for wraparound.
inline uint32_t sinceMs(uint32_t now, uint32_t past) {
    // Wraparound-safe elapsed time. A `past` slightly after `now` (the clock
    // was read before a nested call that recorded a newer time) is zero
    // elapsed, not ~49 days.
    const uint32_t elapsed = now - past;
    return elapsed > 0x80000000u ? 0 : elapsed;
}

// Utility: exponential backoff with 25% jitter.
uint32_t computeBackoff(uint32_t previous_ms) {
    uint32_t next = previous_ms ? (previous_ms * 2) : kBackoffBaseMs;
    if (next > kBackoffCapMs) next = kBackoffCapMs;
    // Simple deterministic jitter; a real bringup would tap IEntropy.
    uint32_t jitter = next / 4;
    return next - (jitter / 2);
}

// Small helper: convert TimerOp to the wire opcode string.
const char* opWireName(TimerOp op) {
    switch (op) {
        case TimerOp::Start:         return "start";
        case TimerOp::Release:       return "release";
        case TimerOp::Pause:         return "pause";
        case TimerOp::Resume:        return "resume";
        case TimerOp::Adjust:        return "adjust";
        case TimerOp::LockControls:  return "lock_controls";
        case TimerOp::Identify:      return "identify";
    }
    return "";
}

// Inverse mapping used when parsing downstream `command` frames.
bool parseOpWireName(const char* s, size_t n, TimerOp* out) {
    if (n == 5  && memcmp(s, "start",         5)  == 0) { *out = TimerOp::Start;        return true; }
    if (n == 7  && memcmp(s, "release",       7)  == 0) { *out = TimerOp::Release;      return true; }
    if (n == 5  && memcmp(s, "pause",         5)  == 0) { *out = TimerOp::Pause;        return true; }
    if (n == 6  && memcmp(s, "resume",        6)  == 0) { *out = TimerOp::Resume;       return true; }
    if (n == 6  && memcmp(s, "adjust",        6)  == 0) { *out = TimerOp::Adjust;       return true; }
    if (n == 13 && memcmp(s, "lock_controls", 13) == 0) { *out = TimerOp::LockControls; return true; }
    if (n == 8  && memcmp(s, "identify",      8)  == 0) { *out = TimerOp::Identify;     return true; }
    return false;
}

// Scope enum → CapabilityToken scope required for the op.
Scope scopeForOp(TimerOp op) {
    switch (op) {
        case TimerOp::Start:         return Scope::Start;
        case TimerOp::Release:       return Scope::Release;
        case TimerOp::Pause:
        case TimerOp::Resume:        return Scope::PauseResume;
        case TimerOp::Adjust:        return Scope::Adjust;
        case TimerOp::LockControls:  return Scope::Lock;
        case TimerOp::Identify:      return Scope::Identify;
    }
    return Scope::View;
}

// Emit a full StatusSnapshot into a JsonWriter as the body of an
// upstream `status` frame. Field names match local/status.schema.json.
void emitStatusBody(JsonWriter& jw, const StatusSnapshot& s) {
    jw.beginObject();
    jw.key("state");
    switch (s.state) {
        case StatusSnapshot::State::Open:    jw.valueString("open");    break;
        case StatusSnapshot::State::Running: jw.valueString("running"); break;
        case StatusSnapshot::State::Paused:  jw.valueString("paused");  break;
    }
    jw.key("remaining_ms");        jw.valueUint(s.remaining_ms);
    jw.key("session_duration_ms"); jw.valueUint(s.session_duration_ms);
    jw.key("max_duration_ms");     jw.valueUint(s.max_duration_ms);
    jw.key("sealed");              jw.valueBool(s.sealed);
    jw.key("pause_allowed");       jw.valueBool(!s.pause_disabled);
    jw.key("control_lock_allowed"); jw.valueBool(!s.control_lock_disabled);
    jw.key("emergency_input_active"); jw.valueBool(s.emergency_input_active);
    jw.key("boot_id");             jw.valueString(s.boot_id);
    if (s.session_id[0])          { jw.key("session_id"); jw.valueString(s.session_id); }
    else                          { jw.key("session_id"); jw.valueNull(); }
    jw.key("state_revision");      jw.valueUint(s.state_revision);
    jw.key("uptime_ms");           jw.valueUint(s.uptime_ms);
    jw.key("api_version");         jw.valueString("v1");
    // build_id is required by local/status.schema.json.
    jw.key("build_id");            jw.valueString(s.build_id);
    if (s.device_label[0]) { jw.key("device_name"); jw.valueString(s.device_label); }
    // The session opens by itself when the share link holding it expires.
    jw.key("share_link_limit_ms");
    if (s.hold_limited) jw.valueUint(s.hold_limit_remaining_ms); else jw.valueNull();
    // The live link's current expiry (timer uptime); owner Extend moves it.
    jw.key("share_link_expires_up_ms");
    if (s.link_expires_up_ms) jw.valueUint(s.link_expires_up_ms); else jw.valueNull();
    jw.endObject();
}

}  // namespace


void frameHandlerTrampoline_(void* ctx, const char* body, size_t len) {
    static_cast<RelayClient*>(ctx)->onFrame(body, len);
}

RelayClient::RelayClient(const Deps& deps)
    : deps_(deps), state_(SessionState::Disabled), next_backoff_ms_(0),
      state_entered_ms_(0), last_upstream_seq_(0), last_downstream_seq_seen_(0),
      last_acked_upstream_seq_(0), result_ring_head_(0),
      last_command_applied_up_ms_(0), command_applied_(false),
      last_status_ms_(0), last_received_ms_(0), last_loop_uptime_(0), reconnect_count_(0),
      dropped_command_count_(0), persistence_ok_(true) {
    memset(grants_, 0, sizeof(grants_));
    memset(&cred_, 0, sizeof(cred_));
    memset(result_ring_, 0, sizeof(result_ring_));
    memset(server_nonce_, 0, sizeof(server_nonce_));
}

bool RelayClient::begin() {
    if (!deps_.timer || !deps_.tokens || !deps_.clock || !deps_.transport)
        return false;
    last_loop_uptime_ = deps_.clock->uptime_ms();
    next_backoff_ms_ = 0;  // a (re)started relay connects without waiting out old backoff
    deps_.transport->setFrameHandler(&frameHandlerTrampoline_, this);
    if (!readEnrollmentCredentialsFromNvs()) {
        transitionTo(SessionState::NotEnrolled);
        return true;
    }
    transitionTo(readEnabledPreferenceFromNvs() ? SessionState::Backoff
                                               : SessionState::Disabled);
    return true;
}

void RelayClient::loop() {
    if (!deps_.clock || !deps_.transport) return;
    const uint32_t now = deps_.clock->uptime_ms();
    if (now < last_loop_uptime_) {
        // Wire uptimes currently use uint32. Stop before reading frames at
        // rollover so old deadlines cannot become valid again. V2 must use a
        // coordinated uint64 clock migration before long-uptime qualification.
        deps_.transport->close(1001, "uptime_rollover");
        transitionTo(SessionState::Disabled); last_loop_uptime_ = now; return;
    }
    last_loop_uptime_ = now;
    sweepSecure();
    deps_.transport->poll();
    // poll() may deliver frames that record newer timestamps (state entry,
    // last receipt); judge deadlines against the clock after it.
    const uint32_t tick = deps_.clock->uptime_ms();
    switch (state_) {
        case SessionState::Disabled:
        case SessionState::NotEnrolled: return;
        case SessionState::Backoff:
            if (sinceMs(tick, state_entered_ms_) < next_backoff_ms_) return;
            if ((heapFree() && heapFree() < kHeapFloorFreeBytes)
                || (heapLargestFreeBlock() && heapLargestFreeBlock() < kHeapFloorLargestBlockBytes)) {
                last_failure_ = "low_memory";  // visible reason for waiting
                state_entered_ms_ = tick;
                return;
            }
            openTransportOrRebackoff();
            return;
        case SessionState::Connecting:
            if (deps_.transport->state() == WssState::Open) {
                transitionTo(SessionState::AwaitingChallenge);
                return;
            }
            if (sinceMs(tick, state_entered_ms_) >= 10000
                || deps_.transport->state() == WssState::Failed
                || deps_.transport->state() == WssState::Closed)
                failTransport("connect_timeout_or_closed");
            return;
        case SessionState::AwaitingChallenge:
        case SessionState::HelloSent:
            if (sinceMs(tick, state_entered_ms_) >= 5000
                || deps_.transport->state() != WssState::Open)
                failTransport("handshake_timeout_or_closed");
            return;
        case SessionState::Live:
            if (deps_.transport->state() != WssState::Open
                || sinceMs(tick, last_received_ms_) >= kPingTimeoutMs) {
                failTransport("connection_stale_or_closed");
                return;
            }
            syncGrants();
            if (sinceMs(tick, last_status_ms_) >= kStatusRefreshPeriodMs)
                onStatusChanged();
            return;
        case SessionState::Failed:
            next_backoff_ms_ = computeBackoff(next_backoff_ms_);
            transitionTo(SessionState::Backoff);
            return;
    }
}

void RelayClient::failTransport(const char* reason) {
    last_failure_ = reason ? reason : "";
    deps_.transport->close(1011, reason);
    transitionTo(SessionState::Failed);
}
void RelayClient::openTransportOrRebackoff() {
    WssOpenParams params{};
    params.url = cred_.wss_endpoint;
    params.subprotocol = kWebSocketSubprotocol;
    memset(server_nonce_, 0, sizeof(server_nonce_));
    memset(hello_tag_, 0, sizeof(hello_tag_));
    ++reconnect_count_;
    if (!deps_.transport->open(params)) { transitionTo(SessionState::Failed); return; }
    transitionTo(SessionState::Connecting);
}
void RelayClient::transitionTo(SessionState next) {
    if (state_ == next) return;
    if (state_ == SessionState::Live && next != SessionState::Live)
        for (auto& g : grants_) if (g.state == GrantState::Ready || g.state == GrantState::Revoked)
            g.state = g.revoked ? GrantState::Revoking : GrantState::Pending;
    state_ = next;
    state_entered_ms_ = deps_.clock->uptime_ms();
    if (next == SessionState::Live) {
        next_backoff_ms_ = 0;
        last_received_ms_ = state_entered_ms_;
        replayResultRingAfterResume();
        // Registrations and revocations go first, so a link revoked while
        // offline never receives a fresh status frame.
        if (state_ == SessionState::Live) { syncGrants(true); onStatusChanged(); }
    }
}

bool RelayClient::computeAndSendHello() {
    const uint32_t up = deps_.clock->uptime_ms();
    StatusSnapshot snap{};
    deps_.timer->snapshot(&snap);
    if (!snap.boot_id[0]) return false;
    uint8_t msg[256]; size_t pos = 0;
    auto put = [&](const void* p, size_t n) {
        if (pos + n > sizeof(msg)) return false;
        memcpy(msg + pos, p, n); pos += n; return true;
    };
    // The TLS certificate is not verified by the transport. Binding the key
    // it presented into this tag means a relayed hello fails at the real
    // relay, which only accepts the key it serves itself.
    uint8_t peer_key[32];
    if (!deps_.transport->peerKeyDigest(peer_key)) return false;
    static const char label[] = "OST-RELAY-HELLO-v2";
    if (!put(label, sizeof(label)-1) || !put(server_nonce_, sizeof(server_nonce_))
        || !put(cred_.timer_id, strlen(cred_.timer_id))
        || !put(snap.boot_id, strlen(snap.boot_id))) return false;
    for (uint64_t value : {uint64_t(up), last_upstream_seq_, last_downstream_seq_seen_}) {
        uint8_t be[8];
        for (int i = 7; i >= 0; --i) be[7-i] = uint8_t(value >> (i*8));
        if (!put(be, sizeof(be))) return false;
    }
    if (!put(peer_key, sizeof(peer_key))) return false;
    uint8_t tag[32];
    if (!hmacSha256(cred_.enrollment_secret, kEnrollmentSecretBytes, msg, pos, tag)) return false;
    memcpy(hello_tag_, tag, sizeof(hello_tag_));
    char encoded[24];
    if (b64urlEncode(tag, kHelloTagBytes, encoded, sizeof(encoded)) < 0) return false;
    char buf[512]; JsonWriter jw(buf, sizeof(buf));
    jw.beginObject();
    jw.key("type"); jw.valueString("hello");
    jw.key("protocol"); jw.valueString(kProtocolTag);
    jw.key("timer_id"); jw.valueString(cred_.timer_id);
    jw.key("boot_id"); jw.valueString(snap.boot_id);
    jw.key("uptime_ms"); jw.valueUint(up);
    jw.key("resume_from_upstream_seq"); jw.valueUint(last_upstream_seq_);
    jw.key("resume_from_downstream_seq"); jw.valueUint(last_downstream_seq_seen_);
    jw.key("hello_tag"); jw.valueString(encoded);
    jw.endObject(); size_t n = jw.finalize();
    return n && deps_.transport->sendJson(buf, n-1);
}

bool RelayClient::acknowledgementValid(const char* body, size_t len) {
    char encoded[24];
    uint8_t received[kHelloTagBytes];
    if (jsonCopyString(body, len, "ack_tag", encoded, sizeof(encoded)) != 22
        || b64urlDecode(encoded, 22, received, sizeof(received)) != int(sizeof(received))) return false;
    static const char label[] = "OST-RELAY-ACK-v1";
    uint8_t msg[sizeof(label) - 1 + sizeof(server_nonce_) + sizeof(hello_tag_)];
    memcpy(msg, label, sizeof(label) - 1);
    memcpy(msg + sizeof(label) - 1, server_nonce_, sizeof(server_nonce_));
    memcpy(msg + sizeof(label) - 1 + sizeof(server_nonce_), hello_tag_, sizeof(hello_tag_));
    uint8_t expected[32];
    if (!hmacSha256(cred_.enrollment_secret, kEnrollmentSecretBytes, msg, sizeof(msg), expected)) return false;
    return constantTimeCompare(expected, received, sizeof(received)) == 0;
}

void RelayClient::onFrame(const char* body, size_t len) {
    if (!body || len > kInboundMaxBytes) return;
    char type[24];
    if (jsonCopyString(body, len, "type", type, sizeof(type)) < 0) return;
    if ((state_ == SessionState::AwaitingChallenge || state_ == SessionState::HelloSent)
        && sinceMs(deps_.clock->uptime_ms(), state_entered_ms_) >= 5000) {
        failTransport("handshake_timeout"); return;
    }
    // Some transports deliver the first frame in the same poll that opens
    // the socket. Accept its challenge without requiring another loop tick.
    if ((state_ == SessionState::AwaitingChallenge || state_ == SessionState::Connecting)
        && deps_.transport->state() == WssState::Open && strcmp(type, "challenge") == 0) {
        char nonce[24];
        if (jsonCopyString(body, len, "server_nonce", nonce, sizeof(nonce)) != 22
            || b64urlDecode(nonce, 22, server_nonce_, sizeof(server_nonce_)) != 16
            || !computeAndSendHello()) { failTransport("bad_challenge"); return; }
        transitionTo(SessionState::HelloSent);
        return;
    }
    if (state_ == SessionState::HelloSent && strcmp(type, "hello_ack") == 0) {
        // Only the relay that holds this timer's enrollment secret, and that
        // accepted a hello bound to its own TLS key, can produce this tag.
        // Nothing but the hello has been sent before it is checked.
        if (!acknowledgementValid(body, len)) { failTransport("relay_auth_failed"); return; }
        transitionTo(SessionState::Live); return;
    }
    if (state_ != SessionState::Live) return;
    last_received_ms_ = deps_.clock->uptime_ms();
    if (strcmp(type, "e2e_open") == 0) handleSecureOpen(body, len);
    else if (strcmp(type, "e2e") == 0) handleSecureData(body, len);
    else if (strcmp(type, "token_ack") == 0) handleTokenAck(body, len);
    else if (strcmp(type, "ack") == 0) {
        int64_t seq;
        if (jsonGetInt(body, len, "acked_upstream_seq", &seq) && seq >= 0
            && uint64_t(seq) <= last_upstream_seq_) handleDownstreamAck(uint64_t(seq));
    } else if (strcmp(type, "ping") == 0) handleDownstreamPing();
}

void RelayClient::handleDownstreamCommand(const uint8_t* raw, size_t len) {
    const char* body = (const char*)raw;
    int64_t dseq = 0, expiry = 0;
    char op_name[24] = {}, op_id[65] = {}, boot[24] = {}, session[32] = {};
    TimerOp op;
    if (!jsonGetInt(body, len, "downstream_seq", &dseq) || dseq <= 0
        || jsonCopyString(body, len, "op", op_name, sizeof(op_name)) <= 0
        || jsonCopyString(body, len, "op_id", op_id, sizeof(op_id)) <= 0) return;
    // Keep op_ids printable ASCII so identity is identical in every layer.
    for (const char* p = op_id; *p; ++p) if (*p < 0x21 || *p > 0x7e) return;
    if (!parseOpWireName(op_name, strlen(op_name), &op)) {
        emitResult(dseq, op_name, op_id, ResultCode::UnsupportedOpcode, false); return;
    }
    if (uint64_t(dseq) > last_downstream_seq_seen_) last_downstream_seq_seen_ = uint64_t(dseq);
    const uint32_t now = deps_.clock->uptime_ms();
    if (!jsonGetInt(body, len, "expiry_uptime_ms", &expiry)
        || expiry <= int64_t(now) || expiry > UINT32_MAX
        || expiry - int64_t(now) > kCommandFreshnessMaxMs) {
        emitResult(dseq, op_name, op_id, ResultCode::Expired, false); return;
    }
    const char* token; size_t token_len;
    if (!jsonGetString(body, len, "cap_token", &token, &token_len)) {
        emitResult(dseq, op_name, op_id, ResultCode::InvalidFrame, false); return;
    }
    TokenRecord* authorized_token = nullptr;
    VerifyResult vr = deps_.tokens->verify(token, token_len, scopeForOp(op), &authorized_token);
    if (vr != VerifyResult::Ok) {
        emitResult(dseq, op_name, op_id,
            vr == VerifyResult::StaleBoot ? ResultCode::StaleBoot :
            vr == VerifyResult::Expired ? ResultCode::Expired : ResultCode::Unauthorized, false);
        return;
    }
    char token_fp_hash[24];
    if (!CapabilityToken::fingerprintHash(authorized_token->fp, token_fp_hash)) {
        failTransport("hash_failed"); return;
    }
    JsonView params{}, session_view{};
    bool valid_session = jsonCopyString(body, len, "if_session_id", session, sizeof(session)) >= 0;
    if (!valid_session && jsonFindKey(body, len, "if_session_id", &session_view)
        && session_view.is_null) valid_session = true;
    if (jsonCopyString(body, len, "if_boot_id", boot, sizeof(boot)) <= 0
        || !valid_session || !jsonFindKey(body, len, "params", &params) || !params.is_object) {
        emitResult(dseq, op_name, op_id, ResultCode::InvalidFrame, false); return;
    }
    int64_t a = 0, b = 0;
    bool valid_params = true;
    if (op == TimerOp::Start) {
        // Fixed-duration starts only; random-range starts await an explicit
        // adapter contract instead of silently ignoring part of the request.
        JsonView unused{};
        valid_params = jsonGetInt(params.data, params.len, "duration_ms", &a)
            && a > 0 && a <= UINT32_MAX
            && !jsonFindKey(params.data, params.len, "duration_random_max_ms", &unused)
            && !jsonFindKey(params.data, params.len, "duration_random_min_ms", &unused);
    } else if (op == TimerOp::Adjust) {
        valid_params = jsonGetInt(params.data, params.len, "delta_ms", &a)
            && a >= INT32_MIN && a <= INT32_MAX && a != 0;
    }
    if (!valid_params) { emitResult(dseq, op_name, op_id, ResultCode::InvalidDuration, false); return; }
    // Hash semantic identity, excluding transport sequence and retry expiry.
    char identity[512]; JsonWriter ij(identity, sizeof(identity));
    ij.beginArray(); ij.valueString(op_name); ij.valueString(boot); ij.valueString(session);
    ij.valueInt(a); ij.valueInt(b); ij.valueStringN(params.data, params.len); ij.endArray();
    size_t id_len = ij.finalize(); uint8_t hash[32];
    if (!id_len) { emitResult(dseq, op_name, op_id, ResultCode::InvalidFrame, false); return; }
    if (!sha256((const uint8_t*)identity, id_len-1, hash)) {
        failTransport("hash_failed"); return;
    }
    for (auto& r : result_ring_) {
        if (!r.occupied || strcmp(r.op_id, op_id)) continue;
        if (memcmp(r.request_hash, hash, sizeof(hash))) {
            emitResult(dseq, op_name, op_id, ResultCode::OperationConflict, false); return;
        }
        strcpy(r.secure_client_id, active_secure_->client_id);
        r.duplicate = true; r.downstream_seq = uint64_t(dseq);
        r.upstream_seq = ++last_upstream_seq_; r.pending = true;
        sendResultUpstream(r); return;
    }
    if (command_applied_ && sinceMs(now, last_command_applied_up_ms_) < kCooperativeFloorMs) {
        ++dropped_command_count_;
        emitResult(dseq, op_name, op_id, ResultCode::Throttled, false); return;
    }
    ResultRecord* slot = availableResultSlot();
    if (!slot) {
        ++dropped_command_count_;
        emitResult(dseq, op_name, op_id, ResultCode::Throttled, false); return;
    }
    // Capacity is reserved BEFORE any timer mutation. The timer adapter owns
    // atomic operation deduplication and all physical-state preconditions.
    *slot = ResultRecord{};
    slot->occupied = slot->pending = true;
    strcpy(slot->secure_client_id, active_secure_->client_id);
    slot->op = op; strcpy(slot->op_id, op_id); memcpy(slot->request_hash, hash, sizeof(hash));
    slot->downstream_seq = uint64_t(dseq);
    MutationRequest req{op, op_id, boot, session, a, b};
    req.command_expires_ms = uint32_t(expiry);
    req.token_expires_ms = authorized_token->exp_up_ms;
    req.authorized_at_ms = now;
    req.token_fp_hash = token_fp_hash;
    MutationOutcome outcome = deps_.timer->apply(req);
    slot->code = outcome.code; slot->changed = outcome.changed; slot->duplicate = outcome.duplicate;
    deps_.timer->snapshot(&slot->status);
    slot->upstream_seq = ++last_upstream_seq_;
    last_command_applied_up_ms_ = now; command_applied_ = true;
    sendResultUpstream(*slot);
}

ResultRecord* RelayClient::availableResultSlot() {
    for (size_t i = 0; i < kResultRingSize; ++i) {
        size_t index = (result_ring_head_ + i) % kResultRingSize;
        if (!result_ring_[index].occupied || !result_ring_[index].pending) {
            result_ring_head_ = uint8_t((index+1) % kResultRingSize);
            return &result_ring_[index];
        }
    }
    return nullptr;
}
void RelayClient::handleDownstreamAck(uint64_t seq) {
    if (seq <= last_acked_upstream_seq_) return;
    last_acked_upstream_seq_ = seq;
    for (auto& r : result_ring_) if (r.occupied && r.upstream_seq <= seq) r.pending = false;
}
void RelayClient::handleDownstreamPing() {
    static const char pong[] = "{\"type\":\"pong\"}";
    if (!deps_.transport->sendJson(pong, sizeof(pong)-1)) failTransport("pong_send_failed");
}

void RelayClient::sendSecureStatus(const char* client_id, const char* request) {
    StatusSnapshot snap{}; deps_.timer->snapshot(&snap);snap.link_expires_up_ms=liveLinkExpiry();
    char body[kOutboundBufBytes/2];JsonWriter j(body,sizeof(body));emitStatusBody(j,snap);size_t n=j.finalize();
    if(n) sendResponseFrame("status",++last_upstream_seq_,body,n-1,client_id,request);
}
void RelayClient::onStatusChanged() {
    if (state_ != SessionState::Live) return;
    StatusSnapshot snap{}; deps_.timer->snapshot(&snap);
    snap.link_expires_up_ms = liveLinkExpiry();
    char body[kOutboundBufBytes/2]; JsonWriter jw(body, sizeof(body));
    emitStatusBody(jw, snap); size_t n = jw.finalize();
    if (n && sendResponseFrame("status", ++last_upstream_seq_, body, n-1))
        last_status_ms_ = deps_.clock->uptime_ms();
}
void RelayClient::onEventEmitted(uint32_t event_id) {
    if (state_ != SessionState::Live) return;
    StatusSnapshot snap{}; deps_.timer->snapshot(&snap);
    char body[256]; JsonWriter jw(body, sizeof(body));
    // The timer's own event name (for example "started" or
    // "share_link_expired"); "snapshot_changed" only if none is known.
    jw.beginObject(); jw.key("id"); jw.valueUint(event_id);
    jw.key("type"); jw.valueString(snap.last_event[0] ? snap.last_event : "snapshot_changed");
    jw.key("value_ms"); jw.valueInt(snap.last_event_value_ms);
    jw.key("via_share_link"); jw.valueBool(snap.last_event_via_link);
    jw.key("at_ms"); jw.valueUint(deps_.clock->uptime_ms());
    jw.key("boot_id"); jw.valueString(snap.boot_id);
    jw.endObject(); size_t n = jw.finalize();
    if (n) sendResponseFrame("event", ++last_upstream_seq_, body, n-1);
}
void RelayClient::emitResult(uint64_t dseq, const char* op, const char* op_id,
                             ResultCode code, bool changed, bool duplicate) {
    if (state_ != SessionState::Live) return;
    ResultRecord r{};
    if (!active_secure_) return;
    strcpy(r.secure_client_id, active_secure_->client_id);
    r.upstream_seq = ++last_upstream_seq_; r.downstream_seq = dseq;
    r.code = code; r.changed = changed; r.duplicate = duplicate;
    if (!parseOpWireName(op, strlen(op), &r.op)) r.op = static_cast<TimerOp>(0);
    if (op_id) strncpy(r.op_id, op_id, sizeof(r.op_id)-1);
    deps_.timer->snapshot(&r.status);
    sendResultUpstream(r);
}
void RelayClient::sendResultUpstream(const ResultRecord& r) {
    char body[kOutboundBufBytes]; JsonWriter jw(body, sizeof(body));
    const char* op = opWireName(r.op);
    jw.beginObject();
    jw.key("in_reply_to_downstream_seq"); jw.valueUint(r.downstream_seq);
    jw.key("op"); jw.valueString(op); jw.key("op_id"); jw.valueString(r.op_id);
    jw.key("code"); jw.valueString(resultCodeName(r.code));
    jw.key("mutation"); jw.beginObject();
    jw.key("action"); jw.valueString(op); jw.key("changed"); jw.valueBool(r.changed);
    jw.key("duplicate"); jw.valueBool(r.duplicate);
    jw.key("operation_id"); jw.valueString(r.op_id); jw.endObject();
    StatusSnapshot status = r.status;
    status.link_expires_up_ms = liveLinkExpiry();
    jw.key("status"); emitStatusBody(jw, status); jw.endObject();
    size_t n = jw.finalize();
    if (!n) { failTransport("result_encoding_failed"); return; }
    sendResponseFrame("result", r.upstream_seq, body, n-1, r.secure_client_id, r.op_id);
}
bool RelayClient::sendResponseFrame(const char* type, uint64_t seq,
                                    const char* body, size_t body_len, const char* client_id, const char* request_id) {
    // The identity-signed handshake authenticates these per-channel replies.
    // Encoding preserves the original body bytes; AEAD protects type/sequence/body.
    std::unique_ptr<char[]> envelope(new (std::nothrow) char[kOutboundBufBytes*2+128]);
    if (!envelope) return false;
    JsonWriter jw(envelope.get(), kOutboundBufBytes*2+128);
    jw.beginObject(); jw.key("type"); jw.valueString(type);
    jw.key("upstream_seq"); jw.valueUint(seq);
    jw.key("body_json"); jw.valueStringN(body, body_len);
    jw.endObject(); size_t n = jw.finalize();
    if (!n) return false;
    bool sent=true;
    for (auto& channel:secure_sessions_) {
        if (!channel.occupied || !channel.confirmed || !deps_.tokens->liveByHash(channel.fp_hash)) continue;
        if (client_id && strcmp(client_id,channel.client_id)) continue;
        sent=sendSecure(channel,envelope.get(),n-1,request_id) && sent;
    }
    return sent;
}
void RelayClient::replayResultRingAfterResume() {
    // Records can be reused or retried in a different array order. Replay
    // ascending sequence order, preserving the complete original result.
    uint64_t last = 0;
    for (size_t i = 0; i < kResultRingSize && state_ == SessionState::Live; ++i) {
        ResultRecord* best = nullptr;
        for (auto& r : result_ring_) if (r.occupied && r.pending && r.upstream_seq > last
            && (!best || r.upstream_seq < best->upstream_seq)) best = &r;
        if (!best) break;
        last = best->upstream_seq; sendResultUpstream(*best);
    }
}

const char* RelayClient::grantStateName(GrantState state) {
    switch (state) {
        case GrantState::Pending: return "pending";
        case GrantState::Ready: return "ready";
        case GrantState::Revoking: return "revoking";
        case GrantState::Revoked: return "revoked";
        case GrantState::Expired: return "expired";
        case GrantState::Error: return "error";
        default: return "unknown";
    }
}
GrantState RelayClient::grantState(const char* hash) const {
    if (!hash) return GrantState::Unknown;
    for (const auto& g : grants_) if (g.fp_hash[0] && strcmp(g.fp_hash, hash) == 0)
        return g.expiry <= deps_.clock->uptime_ms() ? GrantState::Expired : g.state;
    return GrantState::Unknown;
}
bool RelayClient::sharingAvailable() const {
    return state_ == SessionState::Live && deps_.transport->state() == WssState::Open
        && sinceMs(deps_.clock->uptime_ms(), last_received_ms_) < kPingTimeoutMs;
}
void RelayClient::forgetGrants() { memset(grants_, 0, sizeof(grants_)); }
// A slot is free once empty, expired, or revoked and acknowledged by the
// relay. A revocation still being delivered keeps its slot.
bool RelayClient::grantSlotFree(const GrantSync& g) const {
    return !g.fp_hash[0] || g.expiry <= deps_.clock->uptime_ms()
        || (g.revoked && g.state == GrantState::Revoked);
}
bool RelayClient::canQueueGrant() const {
    for (const auto& g : grants_) if (grantSlotFree(g)) return true;
    return false;
}
bool RelayClient::announceMintedToken(const char* wire, size_t len) {
    TokenRecord* rec = nullptr;
    if (deps_.tokens->verify(wire, len, Scope::View, &rec) != VerifyResult::Ok) return false;
    char hash[24];
    if (!CapabilityToken::fingerprintHash(rec->fp, hash)) return false;
    GrantSync* slot = nullptr;
    for (auto& g : grants_) {
        if (g.fp_hash[0] && strcmp(g.fp_hash, hash) == 0) return true;
        if (!slot && grantSlotFree(g)) slot = &g;
    }
    if (!slot) return false;
    *slot = {};
    uint8_t digest[32];
    if (!sha256((const uint8_t*)wire, len, digest)) return false;
    if (b64urlEncode(digest, sizeof(digest), slot->digest, sizeof(slot->digest)) < 0) return false;
    strcpy(slot->fp_hash, hash);
    StatusSnapshot snap{}; deps_.timer->snapshot(&snap);
    strcpy(slot->boot_id, snap.boot_id);
    memcpy(slot->label, rec->label, sizeof(slot->label));
    slot->scope_mask = rec->scope_mask; slot->expiry = rec->exp_up_ms;
    slot->state = GrantState::Pending;
    if (state_ == SessionState::Live) sendGrant(*slot);
    return true;
}
bool RelayClient::announceRevokedToken(const char* hash) {
    if (!hash || strlen(hash) != 22) return false;
    for (auto& g : grants_) if (strcmp(g.fp_hash, hash) == 0) {
        g.revoked = true; g.state = GrantState::Revoking;
        if (state_ == SessionState::Live) sendGrant(g);
        return true;
    }
    return false;
}
bool RelayClient::announceExtendedToken(const char* hash, uint32_t expires_up_ms) {
    if (!hash || strlen(hash) != 22) return false;
    for (auto& g : grants_) if (g.fp_hash[0] && strcmp(g.fp_hash, hash) == 0) {
        if (g.revoked) return false;
        g.expiry = expires_up_ms;
        g.state = GrantState::Pending;
        if (state_ == SessionState::Live) sendGrant(g);
        return true;
    }
    return false;
}
uint32_t RelayClient::liveLinkExpiry() const {
    for (const auto& g : grants_) if (g.fp_hash[0] && !g.revoked) return g.expiry;
    return 0;
}
void RelayClient::sendGrant(GrantSync& g) {
    if (g.expiry <= deps_.clock->uptime_ms()) { g.state = GrantState::Expired; return; }
    static const char* scopes[] = {"view", "pause_resume", "adjust", "release", "start", "lock", "identify"};
    char buf[768]; JsonWriter jw(buf, sizeof(buf));
    g.seq = ++last_upstream_seq_; g.sent_at = deps_.clock->uptime_ms();
    jw.beginObject(); jw.key("type"); jw.valueString(g.revoked ? "token_revoke" : "token_mint");
    jw.key("upstream_seq"); jw.valueUint(g.seq);
    jw.key("fp_hash"); jw.valueString(g.fp_hash);
    jw.key("token_digest"); jw.valueString(g.digest);
    jw.key("scopes"); jw.beginArray();
    for (size_t i = 0; i < size_t(Scope::_Count); ++i)
        if (g.scope_mask & (1u << i)) jw.valueString(scopes[i]);
    jw.endArray(); jw.key("boot_id"); jw.valueString(g.boot_id);
    jw.key("exp_up_ms"); jw.valueUint(g.expiry);
    jw.key("label"); jw.valueString(g.label); jw.endObject();
    const size_t n = jw.finalize();
    if (!n || !deps_.transport->sendJson(buf, n-1)) failTransport("token_sync_send_failed");
}
void RelayClient::syncGrants(bool reconnect) {
    const uint32_t now = deps_.clock->uptime_ms();
    for (auto& g : grants_) {
        if (!g.fp_hash[0]) continue;
        if (g.expiry <= now) { g.state = GrantState::Expired; continue; }
        if (reconnect) g.state = g.revoked ? GrantState::Revoking : GrantState::Pending;
        if (state_ != SessionState::Live) return;
        if ((g.state == GrantState::Pending || g.state == GrantState::Revoking)
            && (reconnect || !g.seq || sinceMs(now, g.sent_at) >= 2000)) sendGrant(g);
    }
}
void RelayClient::handleTokenAck(const char* body, size_t len) {
    char hash[24], boot[24], action[24], status[24]; int64_t seq;
    if (jsonCopyString(body, len, "fp_hash", hash, sizeof(hash)) != 22
        || jsonCopyString(body, len, "boot_id", boot, sizeof(boot)) <= 0
        || jsonCopyString(body, len, "action", action, sizeof(action)) <= 0
        || jsonCopyString(body, len, "status", status, sizeof(status)) <= 0
        || !jsonGetInt(body, len, "upstream_seq", &seq) || seq <= 0) return;
    for (auto& g : grants_) {
        if (strcmp(g.fp_hash, hash) || strcmp(g.boot_id, boot) || g.seq != uint64_t(seq)
            || strcmp(action, g.revoked ? "token_revoke" : "token_mint")) continue;
        if (!strcmp(status, "ready") && !g.revoked) g.state = GrantState::Ready;
        else if (!strcmp(status, "revoked")) { g.revoked = true; g.state = GrantState::Revoked; }
        else if (!strcmp(status, "expired")) g.state = GrantState::Expired;
        else if (!strcmp(status, "conflict") || !strcmp(status, "unknown")) g.state = GrantState::Error;
        // Capacity is retryable. A generic upstream ACK cannot mark Ready.
        return;
    }
}
bool RelayClient::saveEnrollmentCredentials(const EnrollmentCredentials& c) {
    if (!memchr(c.timer_id, 0, sizeof(c.timer_id)) || !c.timer_id[0]
        || !memchr(c.wss_endpoint, 0, sizeof(c.wss_endpoint))
        || strncmp(c.wss_endpoint, "wss://", 6)) return false;
    EnrollmentCredentials old = cred_; cred_ = c;
    persistence_ok_ = writeEnrollmentCredentialsToNvs() && setEnabledPreferencePersisted(true);
    if (!persistence_ok_) { cred_ = old; return false; }
    secureErase(secure_sessions_,sizeof(secure_sessions_));
    deps_.transport->close(1000, "reenroll"); transitionTo(SessionState::Backoff);
    return true;
}
bool RelayClient::clearEnrollmentCredentials() {
    secureErase(secure_sessions_,sizeof(secure_sessions_));
    deps_.transport->close(1000, "disabled"); transitionTo(SessionState::Disabled);
    bool disabled = setEnabledPreferencePersisted(false);
    bool erased = eraseEnrollmentCredentialsFromNvs();
    persistence_ok_ = disabled && erased;
    memset(&cred_, 0, sizeof(cred_));
    return persistence_ok_;
}
void RelayClient::wipeAll() {
    secureErase(secure_sessions_,sizeof(secure_sessions_));
    clearEnrollmentCredentials();
    memset(result_ring_, 0, sizeof(result_ring_));
}
size_t RelayClient::emergencyShutdown() {
    secureErase(secure_sessions_,sizeof(secure_sessions_));
    char hashes[kTokenTableCapacity][24];
    size_t n = deps_.tokens->revokeAll(hashes, kTokenTableCapacity);
    for (size_t i = 0; i < n; ++i) announceRevokedToken(hashes[i]);
    deps_.transport->close(4010, "emergency_revoke");
    persistence_ok_ = setEnabledPreferencePersisted(false)
        && deps_.tokens->lastPersistenceSucceeded();
    transitionTo(SessionState::Disabled); return n;
}
RelayDiagnostics RelayClient::diagnostics() const {
    RelayDiagnostics d{}; d.state = state_; d.last_backoff_ms = next_backoff_ms_;
    d.ms_in_current_state = deps_.clock ? deps_.clock->uptime_ms() - state_entered_ms_ : 0;
    d.last_upstream_seq = last_upstream_seq_; d.last_downstream_seq = last_downstream_seq_seen_;
    d.reconnect_count = reconnect_count_; d.dropped_command_count = dropped_command_count_;
    if (deps_.transport) { d.bytes_out = deps_.transport->bytesSent(); d.bytes_in = deps_.transport->bytesReceived(); }
    d.relay_reachable = state_ == SessionState::Live;
    for (const auto& r : result_ring_) if (r.occupied && r.pending) ++d.result_ring_used;
    d.last_failure = last_failure_;
    return d;
}
__attribute__((weak)) bool RelayClient::readEnrollmentCredentialsFromNvs() { return false; }
__attribute__((weak)) bool RelayClient::writeEnrollmentCredentialsToNvs() { return false; }
__attribute__((weak)) bool RelayClient::eraseEnrollmentCredentialsFromNvs() { return false; }
__attribute__((weak)) bool RelayClient::readEnabledPreferenceFromNvs() { return false; }
__attribute__((weak)) bool RelayClient::setEnabledPreferencePersisted(bool) { return false; }

}}  // namespace opensmarttimer::relay
