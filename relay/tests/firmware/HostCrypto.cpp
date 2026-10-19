// HostCrypto.cpp — host-test SHA-256 and HMAC-SHA256 for Crypto.h, so the
// tests need no mbedTLS. The timer uses esp/EspCrypto.cpp; base64url and
// constant-time compare are shared in src/RelayEncoding.cpp. Lives under
// tests/ so it never enters the firmware build.

#include "Crypto.h"

#include <string.h>
#include <stdint.h>

namespace opensmarttimer { namespace relay {

// ---------------------------------------------------------------------------
// SHA-256 (RFC 6234) — compact reference implementation.
// ---------------------------------------------------------------------------

namespace {

constexpr uint32_t K[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
};

inline uint32_t rotr(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

void sha256_transform(uint32_t state[8], const uint8_t block[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = ((uint32_t)block[i*4] << 24) | ((uint32_t)block[i*4+1] << 16)
             | ((uint32_t)block[i*4+2] << 8) | (uint32_t)block[i*4+3];
    }
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = rotr(w[i-15],7) ^ rotr(w[i-15],18) ^ (w[i-15] >> 3);
        uint32_t s1 = rotr(w[i-2],17) ^ rotr(w[i-2],19)  ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a=state[0],b=state[1],c=state[2],d=state[3],
             e=state[4],f=state[5],g=state[6],h=state[7];
    for (int i = 0; i < 64; ++i) {
        uint32_t S1 = rotr(e,6) ^ rotr(e,11) ^ rotr(e,25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t t1 = h + S1 + ch + K[i] + w[i];
        uint32_t S0 = rotr(a,2) ^ rotr(a,13) ^ rotr(a,22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    state[0]+=a; state[1]+=b; state[2]+=c; state[3]+=d;
    state[4]+=e; state[5]+=f; state[6]+=g; state[7]+=h;
}

}  // namespace


bool failHostSha256 = false;
bool failHostHmac = false;

bool sha256(const uint8_t* msg, size_t len, uint8_t out[32]) {
    if (failHostSha256) { memset(out, 0, 32); return false; }
    uint32_t state[8] = {
        0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
        0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19
    };
    uint8_t buffer[64];
    size_t buf_len = 0;
    uint64_t total = 0;

    while (len >= 64 - buf_len) {
        size_t take = 64 - buf_len;
        memcpy(buffer + buf_len, msg, take);
        sha256_transform(state, buffer);
        msg += take; len -= take; total += take;
        buf_len = 0;
    }
    memcpy(buffer + buf_len, msg, len);
    buf_len += len; total += len;

    // Padding.
    buffer[buf_len++] = 0x80;
    if (buf_len > 56) {
        while (buf_len < 64) buffer[buf_len++] = 0;
        sha256_transform(state, buffer);
        buf_len = 0;
    }
    while (buf_len < 56) buffer[buf_len++] = 0;
    uint64_t bits = total * 8;
    for (int i = 7; i >= 0; --i) buffer[buf_len++] = (uint8_t)(bits >> (i*8));
    sha256_transform(state, buffer);

    for (int i = 0; i < 8; ++i) {
        out[i*4  ] = (uint8_t)(state[i] >> 24);
        out[i*4+1] = (uint8_t)(state[i] >> 16);
        out[i*4+2] = (uint8_t)(state[i] >> 8);
        out[i*4+3] = (uint8_t)(state[i]);
    }
    return true;
}

// ---------------------------------------------------------------------------
// HMAC-SHA256 (RFC 2104)
// ---------------------------------------------------------------------------

bool hmacSha256(const uint8_t* key, size_t key_len,
                const uint8_t* msg, size_t msg_len,
                uint8_t out[32]) {
    if (failHostHmac) { memset(out, 0, 32); return false; }
    uint8_t k[64] = {0};
    if (key_len > 64) {
        if (!sha256(key, key_len, k)) return false;
    } else {
        memcpy(k, key, key_len);
    }
    uint8_t k_ipad[64], k_opad[64];
    for (int i = 0; i < 64; ++i) { k_ipad[i] = k[i] ^ 0x36; k_opad[i] = k[i] ^ 0x5c; }

    // inner = SHA256(k_ipad || msg)
    uint8_t inner[32];
    {
        // Streaming would be cleaner; the simple form fits in stack.
        uint8_t* buf = new uint8_t[64 + msg_len];
        memcpy(buf, k_ipad, 64);
        memcpy(buf + 64, msg, msg_len);
        const bool ok = sha256(buf, 64 + msg_len, inner);
        delete[] buf;
        if (!ok) return false;
    }
    // outer = SHA256(k_opad || inner)
    uint8_t outer_in[64 + 32];
    memcpy(outer_in, k_opad, 64);
    memcpy(outer_in + 64, inner, 32);
    return sha256(outer_in, 96, out);
}

// ---------------------------------------------------------------------------
// Constant-time compare
// ---------------------------------------------------------------------------

// Real ECDSA P-256 is linked from host/OpenSslEcdsaP256.cpp. No fallback signer.

// Host implementations of the ITimerControl.h free helpers.
size_t heapLargestFreeBlock() { return 0; }   // host has no meaningful measurement
size_t heapFree()             { return 0; }

}  // namespace relay
}  // namespace opensmarttimer
