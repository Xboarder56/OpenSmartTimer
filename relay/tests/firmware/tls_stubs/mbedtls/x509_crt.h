#pragma once
#include <cstring>
#include "pk.h"
struct mbedtls_x509_buf { size_t len; unsigned char* p; };
struct mbedtls_x509_crt {
    int version;
    mbedtls_x509_buf pk_raw;
    mbedtls_pk_context pk;
};
constexpr uint32_t MBEDTLS_X509_BADCERT_NOT_TRUSTED = 8;
inline void mbedtls_x509_crt_init(mbedtls_x509_crt* crt) { std::memset(crt, 0, sizeof(*crt)); }
