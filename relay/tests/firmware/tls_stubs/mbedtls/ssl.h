#pragma once
#include "x509_crt.h"
using PinVerifier = int (*)(void*, mbedtls_x509_crt*, int, uint32_t*);
struct mbedtls_ssl_config { PinVerifier verify; void* context; };
inline void mbedtls_ssl_conf_ca_chain(mbedtls_ssl_config*, mbedtls_x509_crt*, void*) {}
inline void mbedtls_ssl_conf_verify(mbedtls_ssl_config* ssl, PinVerifier verify, void* context) {
    ssl->verify = verify;
    ssl->context = context;
}
