/* Host build of the wolfCrypt pieces behind the RPCN password derivation (PBKDF2-HMAC-SHA3-256), for
 * ps4-port/tests/rpcn_account_test.cpp. Only hashing is built: no TLS, no RNG, no public-key code. The sources are
 * the same 3rdparty/wolfssl files the console build compiles. */
#pragma once

/* With NO_FILESYSTEM nothing else pulls these in before wc_port.h uses size_t and vsnprintf on MinGW. */
#include <stddef.h>
#include <stdio.h>

#define WOLFCRYPT_ONLY
#define SINGLE_THREADED
#define NO_FILESYSTEM
#define WC_NO_RNG
#define NO_RSA
#define NO_DH
#define NO_DSA
#define NO_RC4
#define NO_DES3
#define NO_MD4
#define NO_MD5
#define NO_SHA
#define NO_ASN
#define NO_CODING
#define NO_OLD_TLS
#define NO_PBKDF1
#define NO_PKCS8
#define NO_AES
#define NO_ERROR_STRINGS
#define WOLFSSL_SHA3
