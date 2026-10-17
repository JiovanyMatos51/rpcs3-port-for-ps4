#pragma once

/* Seed source for wolfSSL's Hash-DRBG on the console (CUSTOM_RAND_GENERATE_SEED).
 * This header is force-included into the wolfSSL C sources so random.c has the
 * prototype before wc_GenerateSeed calls it. It uses plain C types to stay free of
 * wolfSSL headers: byte is unsigned char and word32 is unsigned int on this target
 * (runtime/ps4_wolfssl_seed.c checks the latter at compile time). */
#ifdef __cplusplus
extern "C" {
#endif
int rpcs3_ps4_wolfssl_seed(unsigned char* output, unsigned int size);
#ifdef __cplusplus
}
#endif
