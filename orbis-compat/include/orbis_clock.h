// Copyright © 2026 Mikołaj Mikołajczyk
// SPDX-License-Identifier: MIT
// Corrective interposition of clock_gettime() for PS4 builds.
//
// ---------------------------------------------------------------- the defect
//
// The SDK ships the LINUX clock-id table while `clock_gettime` is libkernel.so's
// entry point into a FreeBSD-derived kernel, and the two tables disagree:
//
//     id   OpenOrbis <time.h>          FreeBSD kernel
//     0    CLOCK_REALTIME              CLOCK_REALTIME
//     1    CLOCK_MONOTONIC             CLOCK_VIRTUAL      <- process USER cpu time
//     2    CLOCK_PROCESS_CPUTIME_ID    CLOCK_PROF
//     4    CLOCK_MONOTONIC_RAW         CLOCK_MONOTONIC
//
// So libc++ asks for CLOCK_MONOTONIC, passes 1, and gets process CPU time.
//
// Measured on console (2026-08-06): sceKernelGetProcessTime 20000 us vs steady_clock
// 2993 us (14.96%). The loop spent ~85% of its wall time in the kernel, where
// CLOCK_VIRTUAL does not advance; host-side UDP log timestamps agreed.
//
// Without the fix, anything that spins until a deadline waits 1/0.1496 = 6.68x too
// long (a 16 ms menu frame became 108 ms). In the driver this affects os_time_get_nano()
// (timespec_get(TIME_MONOTONIC) -> clock_gettime), util/futex.c deadlines and
// VK_KHR_calibrated_timestamps.
//
// The fix uses no clock id: sceKernelGetProcessTimeCounter and its frequency are
// monotonic and correctly scaled. Remapping 1 -> 4 was not done because the real
// FreeBSD id semantics on this kernel were not verified.
//
// ---------------------------------------------------------------- still open: condvars
//
// Only clock_gettime is interposed. pthread_condattr_setclock goes straight to
// libkernel, so Mesa's u_cnd_monotonic_init and dri2_create_sync ask for
// CLOCK_MONOTONIC (1) and get a FreeBSD CLOCK_VIRTUAL condvar, which libthr accepts.
// The kernel then compares their absolute deadlines, computed from the counter-based
// clock below, against process CPU time: a timed wait can expire at once or overrun.
// No failure has been traced to it yet.
//
// Not changed yet, because it needs two things together and neither is verified:
//   * a pthread_condattr_setclock interposer mapping Linux 1/4/7 (MONOTONIC,
//     MONOTONIC_RAW, BOOTTIME) to FreeBSD 4, forwarding through
//     scePthreadCondattrSetclock (the only other name for it, with SCE return codes);
//   * the monotonic clock below moved to id 4's epoch, since a remapped condvar
//     compares deadlines against kernel uptime and not against this counter's origin.
// clockIdProbe() measures what both depend on.
#ifndef _ORBIS_CLOCK_H
#define _ORBIS_CLOCK_H

namespace orbis {

/// Reports the clock's basis and checks it on hardware: two ~20 ms windows, one busy
/// and one asleep, against the counter. A CPU-time clock scores ~1.0 busy and ~0.0
/// asleep, which distinguishes "runs slow" from "measures the wrong quantity".
///
/// Runs once; later calls return immediately. Costs ~40 ms and five log lines, and
/// is a no-op when no logger is registered. Safe to call from a frame loop.
void clockProbe();

/// Measures what a condvar clock-id remap would rely on, and changes nothing:
/// sceKernelClockGettime ids 0/1/4 against the counter, busy and asleep; id 4's epoch
/// next to this file's monotonic clock; what scePthreadCondattrSetclock returns and
/// stores; and, only once id 4 has tracked the counter in both windows, how long a
/// clock-4 condvar nobody signals takes on a 20 ms id-4 deadline (bounded either way).
///
/// Runs once; costs ~60 ms and six log lines, and is a no-op without a logger. Call
/// it from one thread at boot, not from a frame loop.
void clockIdProbe();

} // namespace orbis

/* How many times clock_gettime has answered each family (see src/orbis_clock.cpp). Lets the
   graphics driver's leak report relate bytes lost to clock reads. Not namespaced: the consumer is C. */
extern "C" void orbis_clock_counts(unsigned long long* monotonic, unsigned long long* realtime);


#endif
