// Host-only real ECDSA P-256 provider (OpenSSL 3): RFC 6979 deterministic
// nonces, SHA-256 digest, raw low-S r || s signatures, compressed public keys.
// OpenSSL 3.2+ supplies the deterministic nonce; 3.0/3.1 derive it here.
#if defined(ARDUINO) || defined(ESP_PLATFORM)
#error "OpenSslEcdsaP256 is host-only; the ESP build uses the mbedTLS provider"
#endif
#include "Crypto.h"
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/ec.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/param_build.h>
#include <openssl/sha.h>
#include <cstring>
#include <memory>

namespace opensmarttimer { namespace relay {
namespace {
unsigned signature_calls = 0;
bool fail_signing = false;
using Key = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
using KeyContext = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;
using DigestContext = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;
using Group = std::unique_ptr<EC_GROUP, decltype(&EC_GROUP_free)>;
using Point = std::unique_ptr<EC_POINT, decltype(&EC_POINT_free)>;
using Number = std::unique_ptr<BIGNUM, decltype(&BN_free)>;
using Signature = std::unique_ptr<ECDSA_SIG, decltype(&ECDSA_SIG_free)>;
using Builder = std::unique_ptr<OSSL_PARAM_BLD, decltype(&OSSL_PARAM_BLD_free)>;
using Params = std::unique_ptr<OSSL_PARAM, decltype(&OSSL_PARAM_free)>;

Group p256() { return Group(EC_GROUP_new_by_curve_name(NID_X9_62_prime256v1), EC_GROUP_free); }

// Scalar must be in [1, n-1]; writes the compressed public point.
bool publicPoint(const EC_GROUP* group, const uint8_t priv[32], uint8_t out[33]) {
    Number d(BN_bin2bn(priv, 32, nullptr), BN_free);
    if (!d || BN_is_zero(d.get()) || BN_cmp(d.get(), EC_GROUP_get0_order(group)) >= 0) return false;
    Point q(EC_POINT_new(group), EC_POINT_free);
    return q && EC_POINT_mul(group, q.get(), d.get(), nullptr, nullptr, nullptr) == 1
        && EC_POINT_point2oct(group, q.get(), POINT_CONVERSION_COMPRESSED, out, 33, nullptr) == 33;
}

Key makeKey(const uint8_t* priv, const uint8_t pub[33]) {
    Builder bld(OSSL_PARAM_BLD_new(), OSSL_PARAM_BLD_free);
    Number d(priv ? BN_bin2bn(priv, 32, nullptr) : nullptr, BN_free);
    if (!bld || (priv && !d)
        || !OSSL_PARAM_BLD_push_utf8_string(bld.get(), OSSL_PKEY_PARAM_GROUP_NAME, "prime256v1", 0)
        || !OSSL_PARAM_BLD_push_octet_string(bld.get(), OSSL_PKEY_PARAM_PUB_KEY, pub, 33)
        || (priv && !OSSL_PARAM_BLD_push_BN(bld.get(), OSSL_PKEY_PARAM_PRIV_KEY, d.get())))
        return Key(nullptr, EVP_PKEY_free);
    Params params(OSSL_PARAM_BLD_to_param(bld.get()), OSSL_PARAM_free);
    KeyContext ctx(EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr), EVP_PKEY_CTX_free);
    EVP_PKEY* raw = nullptr;
    if (!params || !ctx || EVP_PKEY_fromdata_init(ctx.get()) != 1
        || EVP_PKEY_fromdata(ctx.get(), &raw, priv ? EVP_PKEY_KEYPAIR : EVP_PKEY_PUBLIC_KEY, params.get()) != 1)
        return Key(nullptr, EVP_PKEY_free);
    return Key(raw, EVP_PKEY_free);
}

bool lowS(const EC_GROUP* group, const BIGNUM* s) {
    Number half(BN_dup(EC_GROUP_get0_order(group)), BN_free);
    return half && BN_rshift1(half.get(), half.get()) == 1 && BN_cmp(s, half.get()) <= 0;
}
#if defined(OSSL_SIGNATURE_PARAM_NONCE_TYPE) && !defined(OST_HOST_DERIVE_RFC6979)
// The provider generates the RFC 6979 nonce.
bool signRaw(const EC_GROUP*, const uint8_t priv[32], const uint8_t pub[33],
             const uint8_t* msg, size_t len, Number& r, Number& s) {
    Key key = makeKey(priv, pub);
    DigestContext md(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    EVP_PKEY_CTX* pctx = nullptr;
    unsigned int deterministic = 1;
    OSSL_PARAM nonce[] = {OSSL_PARAM_construct_uint(OSSL_SIGNATURE_PARAM_NONCE_TYPE, &deterministic), OSSL_PARAM_END};
    unsigned char der[80];
    size_t der_len = sizeof(der);
    if (!key || !md || EVP_DigestSignInit_ex(md.get(), &pctx, "SHA256", nullptr, nullptr, key.get(), nullptr) != 1
        || EVP_PKEY_CTX_set_params(pctx, nonce) != 1
        || EVP_DigestSign(md.get(), der, &der_len, msg, len) != 1) return false;
    const unsigned char* cursor = der;
    Signature sig(d2i_ECDSA_SIG(nullptr, &cursor, long(der_len)), ECDSA_SIG_free);
    if (!sig) return false;
    r.reset(BN_dup(ECDSA_SIG_get0_r(sig.get())));
    s.reset(BN_dup(ECDSA_SIG_get0_s(sig.get())));
    return r && s;
}
#else
// OpenSSL 3.0/3.1 (for example Ubuntu 24.04) cannot request a deterministic
// nonce. Derive k per RFC 6979 section 3.2 with HMAC-SHA-256 (qlen == hlen for
// P-256) and sign with OpenSSL's curve arithmetic. The RFC known answers in
// ecdsa_p256_test.cpp check this path wherever it is compiled.
bool signRaw(const EC_GROUP* group, const uint8_t priv[32], const uint8_t pub[33],
             const uint8_t* msg, size_t len, Number& r, Number& s) {
    if (!makeKey(priv, pub)) return false;  // keep the provider-failure surface
    const BIGNUM* n = EC_GROUP_get0_order(group);
    std::unique_ptr<BN_CTX, decltype(&BN_CTX_free)> ctx(BN_CTX_new(), BN_CTX_free);
    Number d(BN_bin2bn(priv, 32, nullptr), BN_clear_free), k(BN_new(), BN_clear_free);
    Number z(nullptr, BN_free), x(BN_new(), BN_free), inverse(nullptr, BN_clear_free);
    Point point(EC_POINT_new(group), EC_POINT_free);
    uint8_t digest[32], reduced[32], v[32], key[32], next[32], block[32 + 1 + 32 + 32];
    unsigned int mac_len = 0;
    if (!ctx || !d || !k || !x || !point || !SHA256(msg, len, digest)) return false;
    z.reset(BN_bin2bn(digest, 32, nullptr));
    if (!z || BN_nnmod(z.get(), z.get(), n, ctx.get()) != 1 || BN_bn2binpad(z.get(), reduced, 32) != 32) return false;
    std::memset(v, 0x01, 32);
    std::memset(key, 0x00, 32);
    // K = HMAC_K(V || separator [|| x || h1]); V = HMAC_K(V)
    auto update = [&](uint8_t separator, bool secret) {
        std::memcpy(block, v, 32);
        block[32] = separator;
        size_t size = 33;
        if (secret) { std::memcpy(block + 33, priv, 32); std::memcpy(block + 65, reduced, 32); size = 97; }
        if (!HMAC(EVP_sha256(), key, 32, block, size, next, &mac_len) || mac_len != 32) return false;
        std::memcpy(key, next, 32);
        if (!HMAC(EVP_sha256(), key, 32, v, 32, next, &mac_len) || mac_len != 32) return false;
        std::memcpy(v, next, 32);
        return true;
    };
    bool ok = update(0x00, true) && update(0x01, true);
    for (int attempt = 0; ok && attempt < 64; ++attempt) {
        ok = HMAC(EVP_sha256(), key, 32, v, 32, next, &mac_len) && mac_len == 32;
        if (!ok) break;
        std::memcpy(v, next, 32);
        if (!BN_bin2bn(v, 32, k.get())) { ok = false; break; }
        if (!BN_is_zero(k.get()) && BN_cmp(k.get(), n) < 0) {
            r.reset(BN_new());
            s.reset(BN_new());
            inverse.reset(BN_mod_inverse(nullptr, k.get(), n, ctx.get()));
            // r = (kG).x mod n; s = k^-1 (z + r d) mod n
            ok = r && s && inverse
                && EC_POINT_mul(group, point.get(), k.get(), nullptr, nullptr, ctx.get()) == 1
                && EC_POINT_get_affine_coordinates(group, point.get(), x.get(), nullptr, ctx.get()) == 1
                && BN_nnmod(r.get(), x.get(), n, ctx.get()) == 1
                && BN_mod_mul(s.get(), r.get(), d.get(), n, ctx.get()) == 1
                && BN_mod_add(s.get(), s.get(), z.get(), n, ctx.get()) == 1
                && BN_mod_mul(s.get(), s.get(), inverse.get(), n, ctx.get()) == 1;
            if (!ok || (!BN_is_zero(r.get()) && !BN_is_zero(s.get()))) break;
        }
        ok = update(0x00, false);
    }
    ok = ok && r && s && !BN_is_zero(r.get()) && !BN_is_zero(s.get());
    OPENSSL_cleanse(v, sizeof(v));
    OPENSSL_cleanse(key, sizeof(key));
    OPENSSL_cleanse(next, sizeof(next));
    OPENSSL_cleanse(block, sizeof(block));
    return ok;
}
#endif
}  // namespace

bool ecdsaP256PublicFromPrivate(const uint8_t priv[32], uint8_t out[33]) {
    std::memset(out, 0, 33);
    Group group = p256();
    uint8_t pub[33];
    // Building the provider key as well surfaces provider failure here.
    if (!group || !publicPoint(group.get(), priv, pub) || !makeKey(priv, pub)) return false;
    std::memcpy(out, pub, 33);
    return true;
}

bool ecdsaP256Sign(const uint8_t priv[32], const uint8_t* msg, size_t len, uint8_t out[64]) {
    ++signature_calls;
    std::memset(out, 0, 64);
    if (fail_signing) return false;
    Group group = p256();
    uint8_t pub[33];
    if (!group || !publicPoint(group.get(), priv, pub)) return false;
    Number r(nullptr, BN_free), s(nullptr, BN_free);
    if (!signRaw(group.get(), priv, pub, msg, len, r, s)) return false;
    if (!lowS(group.get(), s.get()) && BN_sub(s.get(), EC_GROUP_get0_order(group.get()), s.get()) != 1) return false;
    if (BN_bn2binpad(r.get(), out, 32) != 32 || BN_bn2binpad(s.get(), out + 32, 32) != 32) {
        std::memset(out, 0, 64); return false;
    }
    return true;
}

unsigned testOnlySigningCalls() { return signature_calls; }
void testOnlyFailSigning(bool value) { fail_signing = value; }

bool ecdsaP256Verify(const uint8_t pub[33], const uint8_t* msg, size_t len, const uint8_t sig[64]) {
    Group group = p256();
    Number r(BN_bin2bn(sig, 32, nullptr), BN_free), s(BN_bin2bn(sig + 32, 32, nullptr), BN_free);
    if (!group || !r || !s || !lowS(group.get(), s.get())) return false;
    Signature parsed(ECDSA_SIG_new(), ECDSA_SIG_free);
    if (!parsed || ECDSA_SIG_set0(parsed.get(), r.get(), s.get()) != 1) return false;
    r.release(); s.release();
    unsigned char der[80];
    unsigned char* cursor = der;
    const int der_len = i2d_ECDSA_SIG(parsed.get(), &cursor);
    Key key = makeKey(nullptr, pub);
    DigestContext md(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    return der_len > 0 && key && md
        && EVP_DigestVerifyInit_ex(md.get(), nullptr, "SHA256", nullptr, nullptr, key.get(), nullptr) == 1
        && EVP_DigestVerify(md.get(), der, size_t(der_len), msg, len) == 1;
}
}}  // namespace
