#include "ReleaseUpdate.h"
#include "ReleaseUpdateConfig.h"
#include <cstdio>
#include <cstring>
#if defined(ESP8266)
#include <Arduino.h>
#include <StackThunk.h>
extern "C" uint32_t thunk_br_ecdsa_i15_vrfy_raw(const br_ec_impl*, const void*, size_t,
    const br_ec_public_key*, const void*, size_t);
make_stack_thunk(br_ecdsa_i15_vrfy_raw)
#endif
#if defined(ESP32)
#include <mbedtls/ecdsa.h>
#else
#if !defined(ESP8266)
#include <openssl/evp.h>
#include <openssl/ec.h>
#include <openssl/ecdsa.h>
#endif
#endif

namespace ReleaseUpdate {
bool decodeHex(const char* text, uint8_t* out, size_t bytes) {
  if (!text || strlen(text) != bytes * 2) return false;
  for (size_t i = 0; i < bytes; ++i) {
    unsigned value = 0;
    for (unsigned j = 0; j < 2; ++j) {
      const char c = text[i * 2 + j];
      // The wire format is canonical lowercase hex.
      const int digit = c >= '0' && c <= '9' ? c - '0' :
                       c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
      if (digit < 0) return false;
      value = value * 16 + static_cast<unsigned>(digit);
    }
    out[i] = static_cast<uint8_t>(value);
  }
  return true;
}

namespace {
bool number(const char*& p, uint32_t& result) {
  const char* first = p;
  result = 0;
  while (*p >= '0' && *p <= '9') {
    const uint32_t n = static_cast<uint32_t>(*p++ - '0');
    if (result > (UINT32_MAX - n) / 10) return false;
    result = result * 10 + n;
  }
  return p != first && (p - first == 1 || *first != '0');
}
bool core(const char*& p, uint32_t parts[3]) {
  for (unsigned i = 0; i < 3; ++i) {
    if (!number(p, parts[i])) return false;
    if (i < 2 && *p++ != '.') return false;
  }
  return true;
}
bool identifierChar(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
         (c >= '0' && c <= '9') || c == '-';
}
int identifierCompare(const char*& a, const char*& b) {
  const char *ae = a, *be = b;
  bool an = true, bn = true;
  while (*ae && *ae != '.') { an &= *ae >= '0' && *ae <= '9'; ++ae; }
  while (*be && *be != '.') { bn &= *be >= '0' && *be <= '9'; ++be; }
  const size_t al = ae - a, bl = be - b;
  int result = 0;
  if (an != bn) result = an ? -1 : 1;
  else if (an && al != bl) result = al > bl ? 1 : -1;
  else {
    result = memcmp(a, b, al < bl ? al : bl);
    if (!result && al != bl) result = al > bl ? 1 : -1;
  }
  a = ae; b = be;
  return result;
}
}

bool validVersion(const char* version) {
  if (!version || strlen(version) >= kVersionBytes) return false;
  const char* p = version;
  uint32_t parts[3];
  if (!core(p, parts)) return false;
  if (!*p) return true;
  if (*p++ != '-') return false;
  do {
    const char* start = p;
    bool numeric = true;
    while (identifierChar(*p)) { numeric &= *p >= '0' && *p <= '9'; ++p; }
    if (p == start || (numeric && p - start > 1 && *start == '0')) return false;
    if (!*p) return true;
  } while (*p++ == '.');
  return false;
}

bool newer(const char* candidate, const char* installed) {
  if (!validVersion(candidate) || !validVersion(installed)) return false;
  const char *a = candidate, *b = installed;
  uint32_t ap[3], bp[3];
  core(a, ap); core(b, bp);
  for (unsigned i = 0; i < 3; ++i) if (ap[i] != bp[i]) return ap[i] > bp[i];
  if (!*a || !*b) return !*a && *b;
  ++a; ++b;
  for (;;) {
    const int order = identifierCompare(a, b);
    if (order) return order > 0;
    if (!*a || !*b) return *a && !*b;
    ++a; ++b;
  }
}

const char* target() {
#if defined(SMART_TIMER_HARDWARE_V2)
  return "magbound_v2_esp32c3";
#else
  return "magbound_v1_esp8266";
#endif
}
bool configured() {
  if (SMART_TIMER_UPDATE_BASE_URL[0] == '\0') return false;
  if (SMART_TIMER_UPDATE_REPOSITORY[0] == '\0') return false;
  return strlen(SMART_TIMER_UPDATE_PUBLIC_KEY) == 130;
}
const char* baseUrl() { return SMART_TIMER_UPDATE_BASE_URL; }
bool hasVerificationMemory() {
#if defined(ESP8266)
  return stack_thunk_get_refcnt() ||
         (ESP.getFreeHeap() >= 12000 && ESP.getMaxFreeBlockSize() >= 7168);
#else
  return true;
#endif
}

void ImageHash::begin() {
  clear();
  active_ = valid_ = true;
#if defined(ESP8266)
  br_sha256_init(&context_);
#elif defined(ESP32)
  mbedtls_sha256_init(&context_);
  valid_ = mbedtls_sha256_starts(&context_, 0) == 0;
#else
  valid_ = SHA256_Init(&context_) == 1;
#endif
}
void ImageHash::clear() {
#if defined(ESP32)
  // Frees SDK/hardware SHA state too, including on truncated/aborted uploads.
  if (active_) mbedtls_sha256_free(&context_);
#endif
  active_ = valid_ = false;
}
void ImageHash::append(const uint8_t* bytes, size_t length) {
  if (!active_ || !valid_) return;
#if defined(ESP8266)
  br_sha256_update(&context_, bytes, length);
#elif defined(ESP32)
  valid_ = mbedtls_sha256_update(&context_, bytes, length) == 0;
#else
  valid_ = SHA256_Update(&context_, bytes, length) == 1;
#endif
}
bool ImageHash::matches(const uint8_t expected[32]) {
  if (!active_ || !valid_) { clear(); return false; }
  uint8_t digest[32];
#if defined(ESP8266)
  br_sha256_out(&context_, digest);
#elif defined(ESP32)
  const bool finished = mbedtls_sha256_finish(&context_, digest) == 0;
#else
  const bool finished = SHA256_Final(digest, &context_) == 1;
#endif
  clear();
#if !defined(ESP8266)
  if (!finished) return false;
#endif
  uint8_t difference = 0;
  for (size_t i = 0; i < sizeof(digest); ++i) difference |= digest[i] ^ expected[i];
  return difference == 0;
}

bool verify(const Descriptor& d, const char* targetName, const char* publicKeyHex) {
  if (!validVersion(d.version) || !d.size || d.size > 4194304 || !targetName ||
      (strcmp(targetName, "magbound_v1_esp8266") &&
       strcmp(targetName, "magbound_v2_esp32c3"))) return false;
  char sha[65];
  for (size_t i = 0; i < 32; ++i) snprintf(sha + i * 2, 3, "%02x", d.sha256[i]);
  char message[200];
  const int length = snprintf(message, sizeof(message),
      "OpenSmartTimer-Update-v1\n%s\n%s\n%lu\n%08lx\n%s\n", targetName,
      d.version, static_cast<unsigned long>(d.size),
      static_cast<unsigned long>(d.crc32), sha);
  if (length < 0 || static_cast<size_t>(length) >= sizeof(message)) return false;
  uint8_t key[65], digest[32];
  if (!decodeHex(publicKeyHex, key, sizeof(key)) || key[0] != 4) return false;
#if defined(ESP8266)
  br_sha256_context hash;
  br_sha256_init(&hash); br_sha256_update(&hash, message, length); br_sha256_out(&hash, digest);
  const br_ec_public_key pk{BR_EC_secp256r1, key, sizeof(key)};
  // The SDK's generic EC ladder exceeds the Arduino 4 KiB continuation stack.
  // Borrow its supported 6 KiB crypto stack only for this verification, before
  // Update.begin(). No TLS connection or persistent heap allocation is needed.
  if (!hasVerificationMemory()) return false;
  stack_thunk_add_ref();
  const bool valid = thunk_br_ecdsa_i15_vrfy_raw(&br_ec_prime_i15, digest, sizeof(digest), &pk,
                                               d.signature, sizeof(d.signature)) == 1;
  stack_thunk_del_ref();
  return valid;
#elif defined(ESP32)
  if (mbedtls_sha256(reinterpret_cast<const uint8_t*>(message), length, digest, 0) != 0) return false;
  mbedtls_ecdsa_context ec;
  mbedtls_ecdsa_init(&ec);
  mbedtls_mpi r, s;
  mbedtls_mpi_init(&r); mbedtls_mpi_init(&s);
  const bool valid =
      mbedtls_ecp_group_load(&ec.MBEDTLS_PRIVATE(grp), MBEDTLS_ECP_DP_SECP256R1) == 0 &&
      mbedtls_ecp_point_read_binary(&ec.MBEDTLS_PRIVATE(grp), &ec.MBEDTLS_PRIVATE(Q), key, sizeof(key)) == 0 &&
      mbedtls_ecp_check_pubkey(&ec.MBEDTLS_PRIVATE(grp), &ec.MBEDTLS_PRIVATE(Q)) == 0 &&
      mbedtls_mpi_read_binary(&r, d.signature, 32) == 0 &&
      mbedtls_mpi_read_binary(&s, d.signature + 32, 32) == 0 &&
      mbedtls_ecdsa_verify(&ec.MBEDTLS_PRIVATE(grp), digest, sizeof(digest),
                           &ec.MBEDTLS_PRIVATE(Q), &r, &s) == 0;
  mbedtls_mpi_free(&r); mbedtls_mpi_free(&s); mbedtls_ecdsa_free(&ec);
  return valid;
#else
  SHA256(reinterpret_cast<const uint8_t*>(message), length, digest);
  EC_KEY* ec = EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
  const unsigned char* cursor = key;
  ECDSA_SIG* signature = ECDSA_SIG_new();
  bool valid = ec && signature && o2i_ECPublicKey(&ec, &cursor, sizeof(key));
  if (valid) {
    ECDSA_SIG_set0(signature, BN_bin2bn(d.signature, 32, nullptr), BN_bin2bn(d.signature + 32, 32, nullptr));
    valid = ECDSA_do_verify(digest, sizeof(digest), signature, ec) == 1;
  }
  ECDSA_SIG_free(signature); EC_KEY_free(ec);
  return valid;
#endif
}
}  // namespace ReleaseUpdate
