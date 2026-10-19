#pragma once

// Crypto primitives the relay code needs, as free functions. The timer binds
// them to mbedTLS (src/relay/EspCrypto.cpp); host tests bind them to
// relay/tests/firmware/HostCrypto.cpp and OpenSslEcdsaP256.cpp.

#include <stdint.h>
#include <stddef.h>

namespace opensmarttimer { namespace relay {

// Fills out[0..32) with SHA-256(msg[0..len)).
[[nodiscard]] bool sha256(const uint8_t* msg, size_t len, uint8_t out[32]);

// Fills out[0..32) with HMAC-SHA256(key, msg).
[[nodiscard]] bool hmacSha256(const uint8_t* key, size_t key_len,
                const uint8_t* msg, size_t msg_len,
                uint8_t out[32]);
// False is a provider failure, NEVER a digest. Callers must fail closed.

// Constant-time byte compare. Returns 0 iff equal.
int constantTimeCompare(const uint8_t* a, const uint8_t* b, size_t len);

// base64url without padding. Return values are number of ASCII bytes
// written (encode) / decoded bytes (decode); a negative value means
// the output buffer was too small or the input was invalid.
int b64urlEncode(const uint8_t* in, size_t in_len,
                 char* out, size_t out_capacity);
int b64urlDecode(const char* in, size_t in_len,
                 uint8_t* out, size_t out_capacity);


// ---------------------------------------------------------------------------
// ECDSA P-256 with SHA-256 — timer downstream frame signing (RELAY_PROTOCOL.md §7).
//
// P-256 replaced Ed25519 on 2026-09-28: the V2 image already links mbedTLS
// ECDSA, while libsodium Ed25519 did not fit the application slot. Browsers
// verify with WebCrypto ECDSA, which every supported browser implements.
//
// Rules every provider must follow:
//   - Deterministic nonces (RFC 6979), so signing never depends on the RNG.
//   - Low-S form: if s > n/2, emit n - s. Verifiers reject high-S.
//   - Signature bytes are raw r || s (IEEE P1363), 32 bytes each, big-endian.
//   - Public keys are SEC1 compressed points (0x02/0x03 || X), 33 bytes.
//   - The private key is a 32-byte big-endian scalar in [1, n-1]. A random
//     32-byte value outside that range is rejected; callers draw again.
// Failures return false and clear the output; callers must stop rather than
// emit unsigned or invalid data.
// ---------------------------------------------------------------------------

constexpr size_t kSigningPrivateBytes = 32;
constexpr size_t kSigningPublicBytes  = 33;
constexpr size_t kSignatureBytes      = 64;

// Derive the compressed public key. Returns false for an invalid scalar.
[[nodiscard]] bool ecdsaP256PublicFromPrivate(const uint8_t priv[kSigningPrivateBytes],
                                uint8_t out_public[kSigningPublicBytes]);

// Sign SHA-256(msg) deterministically; writes low-S r || s.
[[nodiscard]] bool ecdsaP256Sign(const uint8_t priv[kSigningPrivateBytes],
                   const uint8_t* msg, size_t msg_len,
                   uint8_t sig_out[kSignatureBytes]);

// Verify a low-S r || s signature over SHA-256(msg). Rejects high-S.
[[nodiscard]] bool ecdsaP256Verify(const uint8_t pub[kSigningPublicBytes],
                     const uint8_t* msg, size_t msg_len,
                     const uint8_t sig[kSignatureBytes]);

}  // namespace relay
}  // namespace opensmarttimer
