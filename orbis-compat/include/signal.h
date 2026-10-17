// Copyright © 2026 Mikołaj Mikołajczyk
// SPDX-License-Identifier: MIT
/* The two sigevent accessors FreeBSD defines and this SDK omits.
 *
 * The SDK's struct sigevent is FreeBSD's (signal.h:144): the thread-notification function and its
 * pthread_attr_t are in _sigev_un._sigev_thread, but the two accessor macros are missing, so
 * portable code that fills a SIGEV_THREAD event (e.g. dEQP's deTimer.c) does not compile.
 *
 * The platform's own timer_create with SIGEV_THREAD never returns (measured 2026-08-19, also with
 * our pthread_create interposer disabled); SIGEV_NONE timers work, and CLOCK_MONOTONIC is refused
 * while CLOCK_REALTIME works. The macros are still correct for the struct, and SIGEV_THREAD is
 * implemented in userspace by src/orbis_timer.cpp (see orbis_timer.h).
 *
 * deqp-vk does not call deTimer_*, so that object is dropped at link time anyway.
 */
#ifndef _ORBIS_COMPAT_SIGNAL_H
#define _ORBIS_COMPAT_SIGNAL_H

#include_next <signal.h>

/* SDK typo: signal.h:136 names the union member `__sa_sigaction`, but signal.h:142 defines the
   macro as `__sa_handler.sa_sigaction`, so sa_sigaction does not compile. Redefined to the real
   member. */
#undef sa_sigaction
#define sa_sigaction __sa_handler.__sa_sigaction

/* The SDK's SA_* flag values are Linux's; the kernel is FreeBSD-derived.
 *
 * include/bits/signal.h defines SA_SIGINFO 4, SA_ONSTACK 0x08000000, SA_RESTART 0x10000000. This
 * kernel reads the field as:
 *
 *     SA_ONSTACK 0x0001   SA_RESTART 0x0002   SA_RESETHAND 0x0004
 *     SA_NOCLDSTOP 0x0008 SA_NODEFER 0x0010   SA_NOCLDWAIT 0x0020   SA_SIGINFO 0x0040
 *
 * So `sa_flags = SA_SIGINFO` requested SA_RESETHAND: a one-shot handler without siginfo. Measured
 * on hardware, a SIGSEGV arrived as rdi = 11, rsi = 2, rdx = stack address - FreeBSD's old
 * void (*)(int sig, int code, struct sigcontext *scp) signature. Play!'s
 * CEeExecutor::HandleException read si_addr from it and faulted at 0x1a; src/orbis_boot.cpp's own
 * handler faulted the same way reading info->si_code and went straight to _Exit(2), so no
 * "fatal: signal" line was ever logged.
 *
 * SA_ONSTACK 0x08000000 is unknown to this kernel and was silently dropped, so stack overflows
 * were never handled on the alt stack.
 *
 * Same family as libc++'s ETIMEDOUT being Linux's 110 on a FreeBSD target. The values below are
 * FreeBSD's. */
#undef  SA_ONSTACK
#define SA_ONSTACK   0x0001
#undef  SA_RESTART
#define SA_RESTART   0x0002
#undef  SA_RESETHAND
#define SA_RESETHAND 0x0004
#undef  SA_NOCLDSTOP
#define SA_NOCLDSTOP 0x0008
#undef  SA_NODEFER
#define SA_NODEFER   0x0010
#undef  SA_NOCLDWAIT
#define SA_NOCLDWAIT 0x0020
#undef  SA_SIGINFO
#define SA_SIGINFO   0x0040
#undef  SA_ONESHOT
#define SA_ONESHOT   SA_RESETHAND
#undef  SA_NOMASK
#define SA_NOMASK    SA_NODEFER

/* stack_t's layout is also Linux's, which is why sigaltstack returned -1 on this console.
 *
 *   include/bits/signal.h:91      struct sigaltstack { void *ss_sp; int ss_flags; size_t ss_size; };
 *   FreeBSD 9 sys/sys/signal.h    typedef struct sigaltstack {
 *     :358-365                         char *ss_sp; __size_t ss_size; int ss_flags; } stack_t;
 *
 * Same 24 bytes with ss_size and ss_flags swapped. Filling the SDK struct with ss_size = 65536,
 * ss_flags = 0 hands the kernel ss_size = 0 and ss_flags = 0x00010000, and FreeBSD's
 * kern_sigaltstack rejects any flag bit outside SS_DISABLE with EINVAL (per FreeBSD's kern_sig.c,
 * this check runs before ss_size is examined). The struct layout, SS_*, SA_* and errno values are
 * taken from FreeBSD 9's sys/sys/signal.h and sys/sys/errno.h.
 *
 * SS_DISABLE is Linux's 2 in the SDK; this kernel tests 0x0004. SS_ONSTACK is 1 on both.
 *
 * SIGSTKSZ is also Linux's (8192 vs FreeBSD 9's 34816) but is left alone: the kernel only enforces
 * MINSIGSTKSZ (2048, correct in the SDK), and raising it would grow every `char buf[SIGSTKSZ]`.
 *
 * The fix is a translating shim, not a redeclared struct: libc++ and prebuilt archives were
 * compiled against the SDK's stack_t, so only the bytes crossing the syscall are reordered. It is
 * a function-like macro so `struct sigaltstack` as a type name is not rewritten.
 */
#undef  SS_DISABLE
#define SS_DISABLE 0x0004

struct __orbis_stack_bsd {
        void         *ss_sp;
        __SIZE_TYPE__ ss_size;
        int           ss_flags;
        int           __orbis_pad;
};

/* The real symbol comes from libkernel.so; the SDK's libc.a has an empty sigaltstack.o, so there
 * is no libc wrapper in between. errno is meaningful: libc.a's __errno_location is a single
 * `jmp __error`, so the application's errno is libkernel's. */
extern int __orbis_sigaltstack_kernel(const struct __orbis_stack_bsd *__ss,
                                      struct __orbis_stack_bsd *__oss) __asm__("sigaltstack");

static __inline int __orbis_sigaltstack(const stack_t *__ss, stack_t *__oss)
{
        struct __orbis_stack_bsd __in;
        struct __orbis_stack_bsd __out;
        int __rc;

        __in.ss_sp = 0; __in.ss_size = 0; __in.ss_flags = 0; __in.__orbis_pad = 0;
        __out = __in;

        if (__ss != 0) {
                __in.ss_sp    = __ss->ss_sp;
                __in.ss_size  = __ss->ss_size;
                __in.ss_flags = __ss->ss_flags;
        }

        __rc = __orbis_sigaltstack_kernel(__ss  != 0 ? &__in  : 0,
                                          __oss != 0 ? &__out : 0);

        if (__oss != 0) {
                __oss->ss_sp    = __out.ss_sp;
                __oss->ss_size  = __out.ss_size;
                __oss->ss_flags = __out.ss_flags;
        }
        return __rc;
}

#define sigaltstack(ss, oss) __orbis_sigaltstack((ss), (oss))

#ifndef sigev_notify_function
#define sigev_notify_function   _sigev_un._sigev_thread._function
#endif
#ifndef sigev_notify_attributes
#define sigev_notify_attributes _sigev_un._sigev_thread._attribute
#endif

#endif /* _ORBIS_COMPAT_SIGNAL_H */
