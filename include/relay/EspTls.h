#pragma once

// TLS client transport for the relay task; one TLS connection at a time.
//
// No public CA bundle is used (about 60 KB of V2 flash), so certificate chain,
// expiry and hostname are not checked. The server is authenticated through the
// SHA-256 of its leaf certificate's SubjectPublicKeyInfo instead, in one of
// two ways:
//   - Enrollment supplies `pin` from the owner's setup code and the handshake
//     fails unless the leaf key matches it.
//   - The WebSocket passes no pin. The handshake then accepts any leaf and
//     only records its key digest; the caller MUST authenticate the relay
//     before trusting the connection, which RelayClient does by binding that
//     digest into the hello and checking the relay's acknowledgement tag.
//     A renewed certificate therefore needs nothing from the owner.
#include <stddef.h>
#include <stdint.h>

#include "esp_transport.h"

namespace opensmarttimer { namespace relay {

struct TlsPinPolicy {
    const uint8_t* pin;         // 32-byte digest to enforce, or nullptr to record only
    bool leaf_seen;             // the server presented a leaf certificate
    bool pin_matched;           // `pin` was given and the leaf key matched it
    uint8_t leaf_digest[32];    // SHA-256 of the leaf SPKI; valid when leaf_seen
};

// Returns a new esp_transport SSL handle that enforces `policy`. The policy
// must outlive the connection attempt.
esp_transport_handle_t newPinnedTlsTransport(TlsPinPolicy* policy);

}}  // namespace opensmarttimer::relay
