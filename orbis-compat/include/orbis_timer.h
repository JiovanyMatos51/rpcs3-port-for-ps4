// Copyright © 2026 Mikołaj Mikołajczyk
// SPDX-License-Identifier: MIT
/* POSIX timers with SIGEV_THREAD support.
 *
 * include/signal.h adds the sigev_notify_function/sigev_notify_attributes macros (the SDK's
 * FreeBSD struct sigevent already has the fields), so portable timer code compiles. Whether the
 * notification arrives is a separate question; dEQP's deTimer.c depends on it.
 *
 * Platform behaviour (measured 2026-08-19):
 *
 *     SIGEV_NONE countdown     works - 100 ms armed, read at 250 ms, nothing left
 *     CLOCK_MONOTONIC          refused by timer_create; CLOCK_REALTIME works
 *     SIGEV_THREAD             timer_create never returns
 *     with our interposer off  hangs identically, so it is the platform, not our pthread_create
 *                              (libc.a's timer_create calls pthread_create and waits on a barrier)
 *
 * timerProbe() asks two questions to separate the causes: the timer never ran, SIGEV_THREAD is
 * not implemented, or the clock id is not accepted.
 *
 * Implementation: all five timer calls are interposed. SIGEV_THREAD is a userspace construct
 * everywhere (glibc uses a helper thread); here it is built from a SIGEV_NONE kernel timer plus
 * one thread per timer.
 *
 *   1. musl's timer_t encoding is internal. timer_settime decodes it as
 *          testq %rdi,%rdi ; jns <plain-id> ; movl 0xa0(%rdi,%rdi),%edi ; andl $0x7fffffff,%edi
 *      i.e. a SIGEV_THREAD handle is a pointer with the sign bit set and the kernel id at offset
 *      0xa0 of musl's thread structure. Instead of depending on that layout, SIGEV_THREAD handles
 *      here are pointers into our own static table, and all five calls are interposed.
 *
 *   2. Defining timer_create makes musl's unreachable, so ordinary timers are created here too via
 *      ktimer_create and returned as the plain kernel id (musl's `jns` encoding).
 *
 * Every timer in every consumer (Mesa, the title, the CTS) passes through this file; the
 * pass-through path is exercised by timerProbe()'s countdown on every boot.
 *
 * Notifications run on an ordinary thread, not in a signal context; overruns come from the
 * underlying kernel timer. ORBIS_SIGEV_THREAD=0 makes timer_create refuse SIGEV_THREAD with
 * ENOTSUP instead of hanging.
 *
 * Measured results:
 *
 *     countdown, SIGEV_NONE     rc 0, expired               the pass-through, on every boot
 *     SIGEV_THREAD one-shot     fired 1 after 100 ms        kernel timer 3 underneath
 *     SIGEV_THREAD interval     fired 5 in 300 ms @ 50 ms   overrun 0
 *     carried after the probe   0 timers, 6 notifications   both torn down cleanly
 *
 * The interval case matters because deTimer.c is a periodic watchdog. With it working, the CTS
 * fork no longer patches deTimer.c and libdeutil.a resolves timer_create here.
 */
#ifndef _ORBIS_TIMER_H
#define _ORBIS_TIMER_H

namespace orbis {

/// Runs both questions and reports through orbis_log. Bounded: it waits at most a second, installs
/// no signal handler, and deletes every timer it creates. A no-op when no logger is registered.
void timerProbe();

/// Number of live SIGEV_THREAD timers and notifications delivered so far, so a consumer can check
/// that its watchdog actually fires.
void timerCounts(unsigned *live, unsigned long *fired);


}

#endif /* _ORBIS_TIMER_H */
