// Copyright © 2026 Mikołaj Mikołajczyk
// SPDX-License-Identifier: MIT
/* Corrected pthread type sizes for the PlayStation 4, ahead of the toolchain's own header.
 *
 * The SDK ships musl's declarations over Sony's FreeBSD-derived implementation, where each of these
 * types is an opaque pointer while musl stores state inline. Where musl's type is larger the
 * mismatch is harmless (Sony writes 8 bytes into 40). The four below are declared smaller by musl
 * than what Sony writes, so each would overrun.
 *
 * musl guards every typedef with __DEFINED_<name>, so defining one here suppresses its own; the
 * toolchain's file still supplies the other 152 types.
 *
 * pthread_mutex_t, pthread_cond_t and pthread_rwlock_t are intentionally not changed (unlike
 * OpenOrbis/musl PR #29): std::mutex embeds pthread_mutex_t and the prebuilt libc++.a/libc++abi.a
 * use musl's 40-byte layout. Oversized is safe.
 *
 * pthread_once_t needs no change: on hardware Sony's pthread_once writes one byte into it.
 * See README section 3.
 */

#if defined(__NEED_pthread_mutexattr_t) && !defined(__DEFINED_pthread_mutexattr_t)
typedef struct { void *__opaque; } pthread_mutexattr_t;
#define __DEFINED_pthread_mutexattr_t
#endif

#if defined(__NEED_pthread_condattr_t) && !defined(__DEFINED_pthread_condattr_t)
typedef struct { void *__opaque; } pthread_condattr_t;
#define __DEFINED_pthread_condattr_t
#endif

#if defined(__NEED_pthread_barrierattr_t) && !defined(__DEFINED_pthread_barrierattr_t)
typedef struct { void *__opaque; } pthread_barrierattr_t;
#define __DEFINED_pthread_barrierattr_t
#endif

#if defined(__NEED_pthread_spinlock_t) && !defined(__DEFINED_pthread_spinlock_t)
typedef void *pthread_spinlock_t;
#define __DEFINED_pthread_spinlock_t
#endif

/* wchar_t is 16 bits in this SDK's C headers and 32 bits everywhere else.
 *
 * The SDK's bits/alltypes.h has `typedef unsigned short wchar_t` for C, while clang for
 * x86_64-pc-freebsd12-elf defines `__WCHAR_TYPE__ int`, `__WCHAR_WIDTH__ 32`; in C++ wchar_t is a
 * keyword, and the prebuilt libc++.a uses 32 bits. The SDK's libc.a wide functions were compiled
 * for 16 bits (wmemcpy uses `movw (%r8,%rdx,2)`, wcslen `cmpw $0,2(%rdi,%rax)`), so every
 * std::wstring went through a libc reading half of each character (e.g. SIGSEGV in wmemcpy from a
 * static initializer in sonic3air's librmx FileIO.cpp, before main()).
 *
 * Fixing the typedef alone would leave both languages wrong against libc.a, so this goes together
 * with src/orbis_wchar32.c, which replaces those libc.a members. Keep the two together.
 *
 * Not guarded by __NEED_wchar_t: the mismatch is between code that gets the type from the compiler
 * (all C++) and code that gets it from this header, so it is defined for every C compile;
 * __DEFINED_wchar_t then suppresses musl's.
 */
#if !defined(__cplusplus) && !defined(__DEFINED_wchar_t)
typedef __WCHAR_TYPE__ wchar_t;
#define __DEFINED_wchar_t
#endif

/* No include guard, on purpose: alltypes.h is included many times with different __NEED_ macros. */
#include_next <bits/alltypes.h>
