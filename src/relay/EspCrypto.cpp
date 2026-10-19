// ESP32 provider for Crypto.h over the framework's mbedTLS (already linked by
// Wi-Fi/BLE). SHA-256, HMAC-SHA256 and ECDSA P-256 signing with RFC 6979
// nonces and low-S output. The timer only signs; browsers verify.
#if defined(ESP_PLATFORM)
#include "Crypto.h"

#include <string.h>

#include "esp_random.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/md.h"
#include "mbedtls/sha256.h"

namespace opensmarttimer { namespace relay {
namespace {
// Blinding RNG for scalar multiplication side-channel countermeasures. The
// signature nonce itself is deterministic and never depends on it.
int blindingRng(void*, unsigned char* out, size_t len) {
    esp_fill_random(out, len);
    return 0;
}

struct KeyPair {
    mbedtls_ecp_group group;
    mbedtls_mpi d;
    mbedtls_ecp_point q;
    KeyPair() { mbedtls_ecp_group_init(&group); mbedtls_mpi_init(&d); mbedtls_ecp_point_init(&q); }
    ~KeyPair() { mbedtls_ecp_point_free(&q); mbedtls_mpi_free(&d); mbedtls_ecp_group_free(&group); }
    // Signing needs only the group and validated private scalar.
    bool loadPrivate(const uint8_t priv[kSigningPrivateBytes]) {
        return mbedtls_ecp_group_load(&group, MBEDTLS_ECP_DP_SECP256R1) == 0
            && mbedtls_mpi_read_binary(&d, priv, kSigningPrivateBytes) == 0
            && mbedtls_ecp_check_privkey(&group, &d) == 0;
    }
    bool derivePublic() {
        return mbedtls_ecp_mul(&group, &q, &d, &group.G, blindingRng, nullptr) == 0;
    }
};
}  // namespace

bool sha256(const uint8_t* msg, size_t len, uint8_t out[32]) {
    if (mbedtls_sha256(msg, len, out, 0) == 0) return true;
    memset(out, 0, 32);
    return false;
}

bool hmacSha256(const uint8_t* key, size_t key_len, const uint8_t* msg, size_t msg_len,
                uint8_t out[32]) {
    const mbedtls_md_info_t* info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (info && mbedtls_md_hmac(info, key, key_len, msg, msg_len, out) == 0) return true;
    memset(out, 0, 32);
    return false;
}

bool ecdsaP256PublicFromPrivate(const uint8_t priv[kSigningPrivateBytes],
                                uint8_t out[kSigningPublicBytes]) {
    memset(out, 0, kSigningPublicBytes);
    KeyPair key;
    size_t written = 0;
    if (!key.loadPrivate(priv) || !key.derivePublic()
        || mbedtls_ecp_point_write_binary(&key.group, &key.q, MBEDTLS_ECP_PF_COMPRESSED, &written,
                                          out, kSigningPublicBytes) != 0
        || written != kSigningPublicBytes) {
        memset(out, 0, kSigningPublicBytes);
        return false;
    }
    return true;
}

bool ecdsaP256Sign(const uint8_t priv[kSigningPrivateBytes], const uint8_t* msg, size_t len,
                   uint8_t sig[kSignatureBytes]) {
    memset(sig, 0, kSignatureBytes);
    uint8_t digest[32];
    if (mbedtls_sha256(msg, len, digest, 0) != 0) return false;
    KeyPair key;
    mbedtls_mpi r, s, half;
    mbedtls_mpi_init(&r); mbedtls_mpi_init(&s); mbedtls_mpi_init(&half);
    bool ok = key.loadPrivate(priv)
        && mbedtls_ecdsa_sign_det_ext(&key.group, &r, &s, &key.d, digest, sizeof(digest),
                                      MBEDTLS_MD_SHA256, blindingRng, nullptr) == 0
        && mbedtls_mpi_copy(&half, &key.group.N) == 0
        && mbedtls_mpi_shift_r(&half, 1) == 0;
    // Low-S: the (r, n - s) twin also verifies; emit only the smaller s.
    if (ok && mbedtls_mpi_cmp_mpi(&s, &half) > 0)
        ok = mbedtls_mpi_sub_mpi(&s, &key.group.N, &s) == 0;
    ok = ok && mbedtls_mpi_write_binary(&r, sig, 32) == 0
            && mbedtls_mpi_write_binary(&s, sig + 32, 32) == 0;
    mbedtls_mpi_free(&half); mbedtls_mpi_free(&s); mbedtls_mpi_free(&r);
    if (!ok) memset(sig, 0, kSignatureBytes);
    return ok;
}

// The timer never verifies its own frames; only browsers and tests do.
bool ecdsaP256Verify(const uint8_t*, const uint8_t*, size_t, const uint8_t*) { return false; }

}}  // namespace opensmarttimer::relay
#endif
