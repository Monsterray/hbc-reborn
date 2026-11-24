/* Minimal mbedTLS 3.6 configuration for HBC's updates (sdk/wiiupdate): a TLS 1.2 client with
 * ECDHE key exchange on P-256 or P-384, AES-GCM, and X.509 chain
 * verification against the roots in certs.h. That is all GitHub and its
 * download hosts need; libwiiupdate's P-521, X25519, ChaCha20-Poly1305 and
 * CBC suites are left out, as are SHA-1, the NIST curves' faster
 * reduction, and PEM (certs.h is DER); AES keeps its smaller tables and no
 * decryption. So the code fits in MEM1 next to zmem.c's arena.
 *
 * Certificate dates are not checked (MBEDTLS_HAVE_TIME_DATE is off) because
 * many Wii consoles have a wrong clock; the chain and host name still are. */
#ifndef HBC_MBEDTLS_CONFIG_H
#define HBC_MBEDTLS_CONFIG_H

#ifdef GEKKO
#define MBEDTLS_HAVE_ASM
#endif
#define MBEDTLS_HAVE_TIME
#define MBEDTLS_PLATFORM_MS_TIME_ALT /* mbedtls_ms_time() is in https.c */
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_DEPRECATED_REMOVED
#define MBEDTLS_AES_FEWER_TABLES
#define MBEDTLS_BLOCK_CIPHER_NO_DECRYPT

#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PKCS1_V21
#define MBEDTLS_SSL_EXTENDED_MASTER_SECRET
#define MBEDTLS_SSL_SERVER_NAME_INDICATION
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_MAX_FRAGMENT_LENGTH

#define MBEDTLS_AES_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_CTR_DRBG_C
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_ERROR_C
#define MBEDTLS_GCM_C
#define MBEDTLS_MD_C
#define MBEDTLS_OID_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_RSA_C
#define MBEDTLS_SHA224_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C

#endif
