// Copyright © 2026 Mikołaj Mikołajczyk
// SPDX-License-Identifier: MIT
/* Default thread stack size policy.
 *
 * The default thread stack here is 65536 bytes, too small for a Vulkan pipeline compile (a dEQP
 * worker died in radv_graphics_shaders_compile with ~72 KB of frames). pthread_create is undefined
 * in the SDK's libc.a and comes from libkernel.so, so the default is Sony's, not FreeBSD libthr's
 * (2 MB on 64-bit) or musl's (128 KiB).
 *
 * Measured on the console, 2026-08-19, all with rc 0:
 *
 *     a freshly pthread_attr_init'd attr claims      65536 B
 *     the main thread, created by the loader       2097152 B
 *     a thread created with attr = NULL              65536 B
 *     a thread created with a default-init attr      65536 B
 *     PTHREAD_STACK_MIN                                2048
 *
 * The main thread gets 2 MB, FreeBSD's THR_STACK_DEFAULT for 64-bit; other threads get 1/32 of it.
 *
 * A fresh attr reports the default, so "asked for 65536" and "asked for nothing" look the same.
 * Both are raised: a caller that wanted 64 KiB still works with more, while too little crashes.
 *
 * Policy: a thread that did not choose gets the main thread's stack size, measured at runtime.
 *
 * 0.29: two kinds of caller that did choose are passed through unraised (src/orbis_thread.cpp):
 *   ORBIS_THREAD_PASS_CALLER_STACK   an attr carrying the caller's own stack (pthread_attr_setstack)
 *                                    keeps it at any size; the raise used to drop it and make libkernel
 *                                    allocate a second stack.
 *   ORBIS_THREAD_EXPLICIT_STACK_MIN  an attr whose stack size was set to at least this many bytes keeps
 *                                    that size (a fresh attr reports 65536, so 128 KiB or more was set
 *                                    on purpose).
 * Both default ON (1, 128 KiB) when RPCS3_PS4 is defined and OFF (0, 0) otherwise, so every other
 * consumer keeps the policy above unless it defines them. A third, ORBIS_THREAD_LIMITS_AT_FIRST_CREATE,
 * follows the same rule: see limitsReport() below.
 */
#ifndef _ORBIS_THREAD_H
#define _ORBIS_THREAD_H

#include <stddef.h>

namespace orbis {

/// Runs the probe and reports through orbis_log. Cheap and safe to call early: it creates and joins
/// two threads of its own and touches nothing else.
///
/// A no-op when no logger is registered.
void threadStackProbe();

/// The default stack size the probe observed, or 0 before it has run.
size_t threadDefaultStack();

/// What the interposer gives a thread that did not choose, in bytes; 0 when it is disabled.
size_t threadStackFloor();

/// How many threads have been created through the interposer, and how many it raised. Reported
/// with the memory census, since raised stacks are a significant amount of memory.
void threadCounts(unsigned long *created, unsigned long *raised);

/// How many threads created through the interposer have RETURNED from their start routine. A thread
/// that leaves through pthread_exit is not counted, so created - finished is an upper bound on the
/// live count. Pool exhaustion with a small live count points at mutexes/condvars, not at threads.
unsigned long threadsFinished();

/* Failed creates and attr inits. The census above only prints on successful creates, so a process
 * whose pool has run out would log nothing from here (seen with mupen64plus-next, Play! and
 * melonDS DS: thousands of `[ScePthread/System] Internal Memory is running out` in klog).
 *
 * Read the attr count first: scePthreadAttrInit uses the same pool and is called before the
 * create, so it fails first. If neither count moves while klog reports the pool running out, the
 * leaked objects are mutexes, condvars or keys, not threads. */
void threadFailures(unsigned long *createFailed, unsigned long *attrFailed);

/* ------------------------------------------------------------------ the alternate signal stack
 *
 * The signal disposition is per process but the alternate signal stack (td_sigstk) is per thread,
 * so installCrashHandlers() only covered the main thread. A worker that overflowed its stack died
 * silently while the kernel tried to push the signal frame onto the exhausted stack (deep
 * recursion lives in core threads, e.g. melonDS's renderer or flycast's emulator thread).
 *
 * The trampoline declares a 64 KiB array in its outermost frame and installs it as the thread's
 * alternate stack:
 *
 *   * Nothing is allocated: no malloc at thread start and no flexible or direct memory used; a
 *     2048 KiB thread now has 1984 KiB of usable stack.
 *   * Nothing leaks: the array is reclaimed when the thread exits (a heap buffer would need a TSD
 *     destructor, which is not verified to run on this kernel).
 *   * It is at the opposite end from the guard page: overflows happen at the low end, the array
 *     sits above every frame of the thread body, and FreeBSD grows the handler frame down from
 *     ss_sp + ss_size.
 *
 * Skipped for threads with less than 256 KiB of stack (a caller may ask for a small stack on
 * purpose); giving up a quarter of the stack is not worth it.
 */

/// How many threads installed an alternate signal stack of their own, how many were refused by the
/// kernel, and how many were passed over for having too little stack to spare.
void threadAltStacks(unsigned long *installed, unsigned long *failed, unsigned long *skipped);

/// The size of the alternate signal stack each thread gets, in bytes.
size_t threadAltStackSize();

/* ------------------------------------------------------------------ 0.29: why a create is refused
 *
 * scePthreadCreate has refused a title with 0x80020023 (SCE EAGAIN) at ~85 live threads, with 13 MB
 * of libkernel's internal pool and 82 MiB of flexible memory still free (RPCS3 0.28, Demon's Souls).
 * Candidate causes (descending stack slots exhausted, a hidden flexible threshold, a per-process
 * thread cap, the automatic VA area, a map-entry limit, RLIMIT_VMEM) cannot be told apart from the
 * logs. So the first refused create runs a one-shot diagnosis (a thread on a caller-supplied stack,
 * one on a 64 KiB libkernel stack, a FIXED|NO_OVERWRITE reserve of the next stack slot, a hint-less
 * 2 MiB VA reserve, a 2 MiB flexible map, the failing create repeated, and
 * kern.threads.max_threads_hits before and after), each undone at once, and prints 'thread create
 * DIAGNOSIS' and 'thread create VERDICT' lines. Every libkernel call it needs is bound weakly: a
 * missing export reports "unavailable" instead of crashing.
 */

/// RPCS3 thread_base threads leave through pthread_exit, which the trampoline never sees return.
/// Call this immediately before pthread_exit, once, and only from a thread this interposer's
/// pthread_create started, so threadsLive() can count it out. One relaxed atomic increment.
void threadNoteExit() noexcept;

/// 0.29: call right after a successful pthread_join of a thread this interposer started (RPCS3:
/// ~thread_base). A joined thread's stack is the only one libkernel can reuse, so exited - joined is how
/// many finished threads still pin their stack. One relaxed atomic increment.
void threadNoteJoin() noexcept;

/// Threads started through the interposer that have neither returned from their start routine nor
/// called threadNoteExit(). Excludes the main thread and anything made with scePthreadCreate directly
/// (Sony's own libraries); never negative.
unsigned long threadsLive() noexcept;

/// 0.29: every thread counter at once. The layout is fixed: RPCS3 redeclares this struct instead of
/// including this header, so fields may only be appended, changing both sides together.
///   created_ok     scePthreadCreate returned 0 through the interposer
///   failed         ... returned non-zero          attr_failed  pthread_attr_init refusals
///   finished       start routines that returned   exited       threadNoteExit() calls
///   joined         threadNoteJoin() calls          live_estimate  threadsLive()
///   stack_low/high the lowest base / highest top of a libkernel-allocated thread stack seen so far
///                  (caller-supplied stacks excluded); 0 when none has been seen.
struct ThreadCensus {
  unsigned long      created_ok, failed, attr_failed, finished, exited, joined, live_estimate;
  unsigned long long stack_low, stack_high;
};

/// Fills *out (ignored when null). Relaxed atomic loads only - no lock, no libkernel call - so it is
/// usable from a fatal path.
void threadCensus(ThreadCensus *out) noexcept;

/// Two lines through orbis_log: a walk of the whole VA map with sceKernelVirtualQuery (entries, mapped
/// and committed MiB against RLIMIT_VMEM, stack-flagged entries, the largest gap below 0x1000000000)
/// and the thread picture (live count, exit and join notes, libkernel stack span and its new lows,
/// pass-throughs, flexible and internal memory free). `where` names the moment. A few milliseconds
/// (the walk's own time is in the line); any thread; read-only. Not async-signal-safe (snprintf).
/// SCE_KERNEL_VQ_FIND_NEXT=1 is assumed, not documented; a walk the firmware refuses stops and
/// prints its rc.
/// Throttled for a periodic caller: after the 12th call only every 5th prints (the call number is in
/// the line), because RPCS3's log sink keeps 256 orbis-compat lines per session. The interposer's own
/// failure-path reports are never throttled.
void vmReport(const char *where) noexcept;

/// Two lines: every getrlimit (FreeBSD numbering - vmem is 10, not musl's 9) and the integer sysctls
/// that bound threads and the map (kern.threads.max_threads_per_proc/_hits, vm.max_proc_mmap,
/// kern.usrstack, kern.maxssiz, ...). threadStackProbe() ends with it; call it at boot when that probe
/// does not run. Also remembers max_threads_hits as the boot baseline the diagnosis compares against.
/// 0.29: printed in full once per process; a later call prints one line with max_threads_hits now.
/// ORBIS_THREAD_LIMITS_AT_FIRST_CREATE (ON when RPCS3_PS4 is defined, OFF otherwise) makes the first
/// pthread_create that finds a logger registered run it, so such a build needs no boot call at all.
void limitsReport() noexcept;

/// Diagnostic experiment; call it only behind an opt-in flag. Parks up to n (at most 4096)
/// threads at once on caller-supplied 64 KiB stacks from one heap block (n x 64 KiB, released after),
/// logs how many coexisted and what stopped them - a thread-count limit independent of stacks - then
/// releases and joins them all. A create elsewhere in the process may be refused while they are held.
void threadLimitProbe(unsigned n) noexcept;

}

#endif /* _ORBIS_THREAD_H */
