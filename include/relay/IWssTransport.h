#pragma once

// Outbound WebSocket-over-TLS transport abstraction.
//
// RelayClient owns exactly one instance. It never speaks to
// mbedTLS/BearSSL directly; everything TLS-specific — including the
// SPKI pin check — lives behind this interface. Concrete
// implementations belong in a platform layer (proposed:
// src/platform/EspMbedtlsWssTransport.cpp when we integrate).
//
// Event model: RelayClient calls poll() from its main-loop tick. The
// transport delivers received frames via the FrameHandler callback
// registered at construction. Frames are always full JSON objects
// (one WebSocket message = one JSON body).

#include <stdint.h>
#include <stddef.h>

namespace opensmarttimer { namespace relay {

enum class WssState : uint8_t {
    Idle,          // not connecting; open() has not been called
    Resolving,     // DNS in flight
    Connecting,    // TCP + TLS handshake
    Handshaking,   // WebSocket upgrade
    Open,          // normal frames flowing
    Closing,       // close frame issued, awaiting peer response
    Closed,        // fully torn down; safe to open() again
    Failed,        // last attempt failed; state clears on next open()
};

enum class WssError : uint8_t {
    None,
    Dns,
    TcpConnect,
    TlsHandshake,
    WebsocketHandshake,
    PeerClosed,
    ProtocolError,          // malformed frame, oversize, non-JSON
    LocalTimeout,
    HeapExhausted,          // couldn't allocate handshake buffers
};

struct WssOpenParams {
    // Full wss:// URL (host+port+path). The transport parses out host
    // and path; the client does not need to.
    const char* url;

    // Sec-WebSocket-Protocol offered in the handshake ("ost-relay.v1").
    const char* subprotocol;
};

// Callback signature: frame body is a NUL-terminated JSON string in
// a transport-owned buffer valid only for the duration of the call.
// RelayClient MUST copy any bytes it needs to retain.
using FrameHandler = void (*)(void* ctx, const char* json_utf8, size_t len);

class IWssTransport {
public:
    virtual ~IWssTransport() = default;

    // Register the frame callback. Called before any open().
    virtual void setFrameHandler(FrameHandler cb, void* ctx) = 0;

    // Kick off connection. Non-blocking. State transitions become
    // visible via state(); poll() drives progression.
    virtual bool open(const WssOpenParams& params) = 0;

    // Cooperative tick. RelayClient calls this from its own loop().
    virtual void poll() = 0;

    // SHA-256 of the SubjectPublicKeyInfo of the TLS leaf certificate the
    // server presented on the open connection. The transport does not verify
    // that certificate: RelayClient binds this digest into its hello, and the
    // relay's acknowledgement proves both ends saw the same key. Returns
    // false while no TLS session is established.
    virtual bool peerKeyDigest(uint8_t out[32]) const = 0;

    // Enqueue a JSON text frame. Returns false when the outbound
    // queue is full or the socket is not Open. RelayClient's caller
    // should treat false as backpressure and retry next tick.
    virtual bool sendJson(const char* json_utf8, size_t len) = 0;

    // Initiate a graceful close. Sends a WebSocket close frame with
    // the given code (see RELAY_PROTOCOL.md §4.1; 4010 = emergency_revoke) and
    // an optional short reason string.
    virtual void close(uint16_t code, const char* reason) = 0;

    virtual WssState state() = 0;
    virtual WssError lastError() = 0;

    // Byte counters, for diagnostics.
    virtual uint32_t bytesSent() = 0;
    virtual uint32_t bytesReceived() = 0;
};

}  // namespace relay
}  // namespace opensmarttimer
