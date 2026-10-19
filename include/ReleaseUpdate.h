#pragma once

#include <cstddef>
#include <cstdint>
#if defined(ESP8266)
#include <bearssl/bearssl.h>
#elif defined(ESP32)
#include <mbedtls/sha256.h>
#else
#include <openssl/sha.h>
#endif

namespace ReleaseUpdate {
constexpr size_t kVersionBytes = 48;
struct Descriptor {
  char version[kVersionBytes]{};
  uint32_t size = 0;
  uint32_t crc32 = 0;
  uint8_t sha256[32]{};
  uint8_t signature[64]{};
};
bool decodeHex(const char* text, uint8_t* out, size_t bytes);
bool validVersion(const char* version);
// SemVer ordering, including numeric prerelease identifiers (rc.10 > rc.2).
bool newer(const char* candidate, const char* installed);
const char* target();
bool configured();
bool hasVerificationMemory();
const char* baseUrl();
bool verify(const Descriptor& descriptor, const char* targetName,
            const char* publicKeyHex);

class ImageHash {
 public:
  void begin();
  void clear();
  void append(const uint8_t* bytes, size_t length);
  bool matches(const uint8_t expected[32]);
 private:
  bool active_ = false;
  bool valid_ = false;
#if defined(ESP8266)
  br_sha256_context context_{};
#elif defined(ESP32)
  mbedtls_sha256_context context_{};
#else
  SHA256_CTX context_{};
#endif
};
}  // namespace ReleaseUpdate
