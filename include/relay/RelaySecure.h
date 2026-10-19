#pragma once
#include <stddef.h>
#include <stdint.h>
namespace opensmarttimer { namespace relay {
void secureErase(void* data, size_t len);
bool secureRandom(uint8_t* out, size_t len);
bool ecdhGenerate(uint8_t private_key[32], uint8_t public_key[65]);
bool ecdhShared(const uint8_t private_key[32], const uint8_t public_key[65], uint8_t shared[32]);
bool hkdfSha256(const uint8_t* ikm, size_t ikm_len, const uint8_t* salt, size_t salt_len,
                const uint8_t* info, size_t info_len, uint8_t* out, size_t out_len);
bool gcmEncrypt(const uint8_t key[32], const uint8_t nonce[12], const uint8_t* aad, size_t aad_len,
                const uint8_t* plain, size_t len, uint8_t* cipher);
bool gcmDecrypt(const uint8_t key[32], const uint8_t nonce[12], const uint8_t* aad, size_t aad_len,
                const uint8_t* cipher, size_t len, uint8_t* plain);
// All binary fields are fixed width; strings are ASCII and NUL-delimited.
size_t secureOpenTranscript(const char* timer, const char* boot, const uint8_t fp_hash[16],
                           const uint8_t client[16], const uint8_t public_key[65], uint8_t out[192]);
// Domain-separated identity proof and key context: full open plus both server fields.
constexpr size_t kSecureAcceptTranscriptBytes = 320;
size_t secureAcceptTranscript(const uint8_t* opened, size_t opened_len,
                             const uint8_t server[16], const uint8_t public_key[65],
                             uint8_t out[kSecureAcceptTranscriptBytes]);
size_t secureAAD(uint8_t direction, const uint8_t fp_hash[16], const uint8_t client[16],
                 const uint8_t server[16], uint64_t seq, const char* request, uint8_t out[160]);
void secureNonce(uint64_t seq, uint8_t out[12]);
constexpr size_t kSecureSessions = 8;
struct SecureSession {
    bool occupied = false, confirmed = false;
    char fp_hash[24] = {}, client_id[24] = {}, server_id[24] = {};
    uint8_t fp_raw_hash[16] = {}, client[16] = {}, server[16] = {};
    uint8_t client_key[65] = {}, server_key[65] = {}, accept_tag[32] = {}, accept_sig[64] = {};
    uint8_t rx_key[32] = {}, tx_key[32] = {};
    uint64_t rx_seq = 0, tx_seq = 0;
    uint32_t last_active = 0;
};
}}
