#pragma once

// Wire constants and framing helpers shared by CapabilityToken and
// RelayClient (docs/RELAY_PROTOCOL.md). Everything is one JSON object per
// WebSocket message; there is no binary path.

#include <stdint.h>
#include <stddef.h>

namespace opensmarttimer { namespace relay {

// Wire-version constants, kept together so a bump to /3 changes one file.
constexpr const char* kProtocolTag = "ost-relay/1";
// RFC 6455 subprotocols are HTTP tokens; slash is not permitted here.
constexpr const char* kWebSocketSubprotocol = "ost-relay.v1";
constexpr const char* kCapabilityTokenPrefix = "ost-cap.v1.";

// Fixed byte counts.
constexpr size_t kEnrollmentSecretBytes = 32;   // HMAC-SHA256 key
constexpr size_t kCapabilityKeyBytes    = 32;   // HMAC-SHA256 key
constexpr size_t kTokenFingerprintBytes = 16;   // random per-token id
constexpr size_t kTokenTagBytes         = 16;   // HMAC-SHA256[:16]
constexpr size_t kHelloTagBytes         = 16;   // HMAC-SHA256[:16]
constexpr size_t kSpkiPinBytes          = 32;   // SHA-256 of SubjectPublicKeyInfo

// Cadence.
constexpr uint32_t kStatusRefreshPeriodMs = 5 * 1000;
constexpr uint32_t kPingIntervalMs        = 30 * 1000;
constexpr uint32_t kPingTimeoutMs         = 90 * 1000;

// Downstream command TTL bound: relay may not set expiry_uptime_ms
// further than this past its most recent status.uptime_ms.
constexpr uint32_t kCommandFreshnessMaxMs = 10 * 1000;

// Frame type discriminators used in the JSON "type" field.
enum class UpstreamFrame : uint8_t {
    Hello,
    Status,
    Event,
    Result,
    TokenMint,
    TokenRevoke,
    Pong,
};

enum class DownstreamFrame : uint8_t {
    HelloAck,
    Command,
    Ack,
    RevokeBroadcast,
    Ping,
};

// Result codes the timer may emit to the relay. Matches the enum in
// docs/RELAY_PROTOCOL.md § 7.5 in the same order.
enum class ResultCode : uint8_t {
    Applied = 0,
    InvalidFrame,
    Unauthorized,
    StaleBoot,
    StaleSession,
    InvalidState,
    InvalidDuration,
    EmergencyInputActive,
    ControlsLocked,
    MaximumDurationReached,
    UnsupportedOpcode,
    OperationConflict,
    SystemTransition,
    Expired,
    Throttled,
    ControlDisabled,
};

const char* resultCodeName(ResultCode code);   // for JSON serialization

}  // namespace relay
}  // namespace opensmarttimer
