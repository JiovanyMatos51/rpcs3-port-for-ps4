/* wolfSSL DRBG seed from libSceRandom (0.31).
 *
 * wolfSSL's default wc_GenerateSeed() opens /dev/urandom (then /dev/random), which an
 * application on the console cannot rely on, and the SDK libc getrandom() only fails
 * with ENOSYS. A failed seed makes wolfSSL_new() return null, which RPCN reports as
 * "WolfSSL Error"; curl and clans share the same wolfSSL build.
 *
 * libSceRandom is the console's entropy source (ps4_main.cpp loads it and runs a
 * 64-byte self test at boot). It accepts at most 64 bytes per request, so larger
 * seeds are filled in chunks. A failed request returns READ_RAN_E and never falls
 * back to predictable bytes: the DRBG then fails to instantiate and the connection
 * is refused cleanly.
 *
 * Built into the wolfssl target with CUSTOM_RAND_GENERATE_SEED=rpcs3_ps4_wolfssl_seed
 * (dependencies.cmake). After a build, `llvm-nm libwolfssl.a` should show no `U open`
 * or `U read` for random.c.o.
 */
#include <wolfssl/wolfcrypt/libwolfssl_sources.h>

#include <stddef.h>

#include "../compat/ps4_wolfssl_seed.h"

/* The prototype in ps4_wolfssl_seed.h spells word32 as unsigned int. */
_Static_assert(_Generic((word32)0, unsigned int: 1, default: 0), "word32 is not unsigned int");
_Static_assert(_Generic((byte)0, unsigned char: 1, default: 0), "byte is not unsigned char");

/* The SDK header orbis/Random.h declares this export without parameters. Bind the
 * real SysV signature (buffer, byte count) under a local name instead, the same way
 * sys_ss.cpp does. Returns 0 on success.
 * The reference is weak because curl's configure step links its wolfSSL feature
 * checks without libSceRandom; a strong reference would make those checks fail and
 * silently change curl's configuration. The emulator ELF links libSceRandom and
 * sys_ss.cpp references the export strongly, so it stays a normal import there. */
extern int rpcs3_ps4_system_random(void* buffer, size_t size) __asm__("sceRandomGetRandomNumber")
    __attribute__((weak));

#define RPCS3_PS4_RANDOM_MAX_REQUEST 64u

int rpcs3_ps4_wolfssl_seed(unsigned char* output, unsigned int size)
{
    if (!rpcs3_ps4_system_random || (!output && size != 0))
    {
        return READ_RAN_E;
    }

    while (size != 0)
    {
        const unsigned int count = size < RPCS3_PS4_RANDOM_MAX_REQUEST ? size : RPCS3_PS4_RANDOM_MAX_REQUEST;

        if (rpcs3_ps4_system_random(output, count) != 0)
        {
            return READ_RAN_E;
        }

        output += count;
        size -= count;
    }

    return 0;
}
