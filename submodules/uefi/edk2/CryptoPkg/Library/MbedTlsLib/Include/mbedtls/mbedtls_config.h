/**
 * @file   mbedtls_config.h
 * @brief  Minimal mbedTLS configuration for the SFB BDS.
 *
 * Only what the security boot flow needs: SHA-256/512, RSA (PKCS#1 v1.5
 * verify), ECDSA verify, X.509 certificate parsing and PKCS#7 verification.
 */

#ifndef MBEDTLS_CONFIG_H
#define MBEDTLS_CONFIG_H

#define MBEDTLS_CONFIG_VERSION 0x03000000

/* System */
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_MEMORY
#define MBEDTLS_PLATFORM_NO_STD_FUNCTIONS
#define MBEDTLS_HAVE_TIME
#define MBEDTLS_PLATFORM_MS_TIME_ALT
#define MBEDTLS_PLATFORM_SNPRINTF_MACRO  my_snprintf

/* Implemented by MbedTlsLib's CrtWrapper.c (and by the host test shim). */
#include <stddef.h>
int my_snprintf (char *str, size_t size, const char *format, ...);
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_NO_DEFAULT_ENTROPY_SOURCES
/* No compiler-rt __udivti3 in the freestanding AARCH64 build. */
#define MBEDTLS_NO_UDBL_DIVISION

/* BIGNUM is always required */
#define MBEDTLS_BIGNUM_C

/* Hashes */
#define MBEDTLS_MD_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA512_C
#define MBEDTLS_MD_CAN_SHA256
#define MBEDTLS_MD_CAN_SHA384
#define MBEDTLS_MD_CAN_SHA512

/* ASN.1 / OID */
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_OID_C

/* Public key abstraction */
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15

/* ECDSA (verify only) */
#define MBEDTLS_ECP_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED

/* X.509 certificate parsing */
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_X509_CRL_PARSE_C
#define MBEDTLS_BASE64_C

/* PKCS#7 verification */
#define MBEDTLS_PKCS7_C

#endif /* MBEDTLS_CONFIG_H */
