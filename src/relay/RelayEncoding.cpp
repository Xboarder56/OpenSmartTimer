// Constant-time compare and unpadded base64url for Crypto.h. Portable C++,
// shared by the host tests and the ESP build.
#include "Crypto.h"

#include <stdint.h>

namespace opensmarttimer { namespace relay {

int constantTimeCompare(const uint8_t* a, const uint8_t* b, size_t len) {
    uint8_t diff = 0;
    for (size_t i = 0; i < len; ++i) diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0 ? 0 : 1;
}

// ---------------------------------------------------------------------------
// base64url (no padding)
// ---------------------------------------------------------------------------

namespace {
constexpr const char* kAlpha =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

int decodeChar(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}
}

int b64urlEncode(const uint8_t* in, size_t in_len, char* out, size_t out_capacity) {
    size_t out_len = (in_len * 4 + 2) / 3;
    if (out_len + 1 > out_capacity) return -1;
    size_t oi = 0;
    for (size_t i = 0; i < in_len; i += 3) {
        uint32_t v = (uint32_t)in[i] << 16;
        int rem = 1;
        if (i + 1 < in_len) { v |= (uint32_t)in[i+1] << 8; rem = 2; }
        if (i + 2 < in_len) { v |= (uint32_t)in[i+2];      rem = 3; }
        out[oi++] = kAlpha[(v >> 18) & 0x3f];
        out[oi++] = kAlpha[(v >> 12) & 0x3f];
        if (rem >= 2) out[oi++] = kAlpha[(v >> 6)  & 0x3f];
        if (rem >= 3) out[oi++] = kAlpha[ v        & 0x3f];
    }
    out[oi] = '\0';
    return (int)oi;
}

int b64urlDecode(const char* in, size_t in_len, uint8_t* out, size_t out_capacity) {
    if (in_len % 4 == 1) return -1;
    size_t oi = 0;
    uint32_t v = 0;
    int bits = 0;
    for (size_t i = 0; i < in_len; ++i) {
        int d = decodeChar(in[i]);
        if (d < 0) return -1;
        v = (v << 6) | (uint32_t)d;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (oi >= out_capacity) return -1;
            out[oi++] = (uint8_t)((v >> bits) & 0xff);
        }
    }
    if (bits && (v & ((1u << bits) - 1))) return -1; // canonical unused bits
    return (int)oi;
}

}  // namespace relay
}  // namespace opensmarttimer
