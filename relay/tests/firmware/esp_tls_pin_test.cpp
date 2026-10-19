// Compile the real EspTls.cpp callback against host transport/type doubles.
// Certificates and SPKI/SHA-256 operations use OpenSSL; this does not emulate
// ESP-IDF's TLS handshake or replace the real-device certificate checks.
#include "EspTls.h"
#include "esp_transport_ssl.h"
#include "mbedtls/ssl.h"
#include <openssl/sha.h>
#include <openssl/x509.h>
#include <algorithm>
#include <array>
#include <cassert>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

struct esp_transport_item_t { mbedtls_ssl_config config{}; };
esp_transport_handle_t esp_transport_ssl_init() { return new esp_transport_item_t; }
void esp_transport_ssl_crt_bundle_attach(esp_transport_handle_t handle, esp_err_t (*attach)(void*)) {
    assert(attach(&handle->config) == ESP_OK);
}
static bool hash_failure = false;
int mbedtls_sha256(const unsigned char* input, size_t size, unsigned char* output, int is224) {
    assert(is224 == 0);
    return hash_failure || !SHA256(input, size, output) ? -1 : 0;
}
int mbedtls_pk_write_pubkey_der(mbedtls_pk_context* key, unsigned char* output, size_t size) {
    const int count = i2d_PUBKEY(key->key, nullptr);
    if (count <= 0 || size < size_t(count)) return -1;
    unsigned char* cursor = output + size - count;
    return i2d_PUBKEY(key->key, &cursor);
}

int main(int argc, char** argv) {
    assert(argc == 3);
    std::ifstream file(argv[1], std::ios::binary);
    std::vector<unsigned char> der((std::istreambuf_iterator<char>(file)), {});
    const unsigned char* cursor = der.data();
    X509* certificate = d2i_X509(nullptr, &cursor, long(der.size()));
    assert(certificate);
    EVP_PKEY* key = X509_get_pubkey(certificate);
    assert(key);
    const int size = i2d_PUBKEY(key, nullptr);
    assert(size > 0);
    std::vector<unsigned char> spki(size);
    unsigned char* output = spki.data();
    assert(i2d_PUBKEY(key, &output) == size);
    mbedtls_x509_crt crt{};
    crt.pk_raw = {spki.size(), spki.data()};
    crt.pk.key = key;
    std::array<uint8_t, 32> pin{};
    const std::string encoded = argv[2];
    assert(encoded.size() == pin.size() * 2);
    for (size_t i = 0; i < pin.size(); ++i) pin[i] = uint8_t(std::stoul(encoded.substr(i * 2, 2), nullptr, 16));
    auto wrong = pin;
    wrong[0] ^= 1;
    using opensmarttimer::relay::TlsPinPolicy;
    using opensmarttimer::relay::newPinnedTlsTransport;
    // `expected`: the server key was read; `matched`: a pin was given and it matched.
    auto verify = [&](const uint8_t* required, bool expected, bool matched, bool accepted) {
        TlsPinPolicy policy{required, true, true, {}};
        policy.leaf_digest[0] = 0xff;
        auto transport = newPinnedTlsTransport(&policy);
        assert(transport && !policy.leaf_seen && !policy.pin_matched && policy.leaf_digest[0] == 0);
        uint32_t flags = 0xffffffff;
        assert(transport->config.verify(transport->config.context, &crt, 1, &flags) == 0);
        assert(flags == 0 && !policy.leaf_seen && !policy.pin_matched);
        flags = 0xffffffff;
        assert(transport->config.verify(transport->config.context, &crt, 0, &flags) == 0);
        assert(policy.leaf_seen == expected && policy.pin_matched == matched);
        assert(flags == (accepted ? 0 : MBEDTLS_X509_BADCERT_NOT_TRUSTED));
        // The recorded digest is the leaf key's, whatever pin was required.
        if (expected) assert(std::equal(pin.begin(), pin.end(), policy.leaf_digest));
        delete transport;
    };
    // Enrollment: a required pin must match the leaf key.
    verify(pin.data(), true, true, true);
    verify(wrong.data(), true, false, false);
    // WebSocket: no pin is required; the key is recorded for the caller to authenticate.
    verify(nullptr, true, false, true);
    // A key that cannot be read is never accepted, with or without a pin.
    hash_failure = true;
    verify(pin.data(), false, false, false);
    verify(nullptr, false, false, false);
    hash_failure = false;
    crt.pk_raw = {0, nullptr};
    verify(pin.data(), false, false, false);
    verify(nullptr, false, false, false);
    std::cout << "SPKI bytes=" << size << "; required-pin and record-only leaf checks passed\n";
    EVP_PKEY_free(key);
    X509_free(certificate);
}
