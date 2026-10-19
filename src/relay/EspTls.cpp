#if defined(ESP_PLATFORM)
#include "EspTls.h"

#include <string.h>

#include "esp_transport_ssl.h"
#include "mbedtls/sha256.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

namespace opensmarttimer { namespace relay {
namespace {
TlsPinPolicy* g_policy = nullptr;  // one handshake at a time, relay task only
// mbedTLS refuses to verify without a CA chain; an empty chain plus the pin
// callback below makes the pin the only thing that can satisfy verification.
mbedtls_x509_crt g_empty_chain;

bool pinEquals(const uint8_t digest[32], const uint8_t* pin) {
    if (!pin) return false;
    uint8_t diff = 0;
    for (int i = 0; i < 32; ++i) diff |= uint8_t(digest[i] ^ pin[i]);
    return diff == 0;
}

bool leafKeyDigest(mbedtls_x509_crt* crt, uint8_t digest[32]) {
    // The parser retains the certificate's complete DER SubjectPublicKeyInfo.
    // Hash it in place: RSA-4096 SPKI is 550 bytes and exceeded the old
    // reserialization buffer. This also avoids a second key-sized stack copy.
    if (!crt->pk_raw.p || !crt->pk_raw.len) return false;
    return mbedtls_sha256(crt->pk_raw.p, crt->pk_raw.len, digest, 0) == 0;
}

// Called for each chain certificate, root first, leaf (depth 0) last. The
// chain itself is never trusted. With a pin, only a matching leaf key clears
// the flags. Without one the leaf is accepted and its digest recorded for the
// caller to authenticate (see EspTls.h); an unreadable key still fails.
int verifyPin(void* context, mbedtls_x509_crt* crt, int depth, uint32_t* flags) {
    TlsPinPolicy* policy = static_cast<TlsPinPolicy*>(context);
    *flags = 0;
    if (depth != 0) return 0;
    if (!leafKeyDigest(crt, policy->leaf_digest)) {
        *flags = MBEDTLS_X509_BADCERT_NOT_TRUSTED;
        return 0;
    }
    policy->leaf_seen = true;
    policy->pin_matched = pinEquals(policy->leaf_digest, policy->pin);
    if (policy->pin && !policy->pin_matched) *flags = MBEDTLS_X509_BADCERT_NOT_TRUSTED;
    return 0;
}

esp_err_t attachPinVerifier(void* conf) {
    mbedtls_ssl_config* ssl = static_cast<mbedtls_ssl_config*>(conf);
    mbedtls_x509_crt_init(&g_empty_chain);
    g_empty_chain.version = 3;  // non-empty marker, as esp_crt_bundle does
    mbedtls_ssl_conf_ca_chain(ssl, &g_empty_chain, nullptr);
    mbedtls_ssl_conf_verify(ssl, verifyPin, g_policy);
    return ESP_OK;
}
}  // namespace

esp_transport_handle_t newPinnedTlsTransport(TlsPinPolicy* policy) {
    esp_transport_handle_t ssl = esp_transport_ssl_init();
    if (!ssl) return nullptr;
    policy->leaf_seen = false;
    policy->pin_matched = false;
    memset(policy->leaf_digest, 0, sizeof(policy->leaf_digest));
    g_policy = policy;
    // The attach hook is the supported way to reach the mbedTLS config; it
    // installs the key verifier instead of a CA bundle. The hostname stays
    // set so the handshake carries SNI (Caddy, nginx and CDNs select the
    // certificate by it); verifyPin clears the resulting name-mismatch flag.
    esp_transport_ssl_crt_bundle_attach(ssl, attachPinVerifier);
    return ssl;
}

}}  // namespace opensmarttimer::relay
#endif
