#pragma once
#include <stddef.h>
#include <openssl/evp.h>
struct mbedtls_pk_context { EVP_PKEY* key; };
int mbedtls_pk_write_pubkey_der(mbedtls_pk_context*, unsigned char*, size_t);
