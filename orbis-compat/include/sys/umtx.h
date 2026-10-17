// Copyright © 2026 Mikołaj Mikołajczyk
// SPDX-License-Identifier: MIT
/* FreeBSD's _umtx_op, the kernel primitive Mesa's FreeBSD futex is written against.
 *
 * The syscall (454, SYS__umtx_op in OpenOrbis's bits/syscall.h) is blocked by the sandbox: the
 * kernel answers ENOSYS (78) to both WAIT and WAKE.
 *
 * libkernel.so does export `_umtx_op` as a symbol (0xd366), and it works (measured 2026-08-19,
 * orbis_selftest_umtx_symbol): a WAIT whose expected value does not match returns immediately even
 * without a timeout, and with a matching value and a 150 ms deadline it slept 149906 us and returned
 * ETIMEDOUT. Note the mismatch case returns 0, not EWOULDBLOCK. The static inline definition below
 * shadows that symbol in every includer. OpenOrbis/musl PR #35 uses the libkernel symbol; its
 * behaviour under real contention (dEQP-VK.api.object_management.multithreaded_*) is untested here.
 * Build with -DORBIS_UMTX_LIBKERNEL=1 to use it.
 *
 * With only the blocked syscall, futex_wait failed instantly and every contended simple_mtx in Mesa
 * became a spin; dEQP-VK.api.object_management.multithreaded_per_thread_device hung for 180 s with
 * no output. So _umtx_op is implemented here as a small wait/wake on pthreads; Mesa's futex.c is
 * unchanged.
 *
 * Unlike a real futex, addresses hash onto a fixed table of 64 mutex+condvar pairs, so unrelated
 * addresses can wake each other spuriously. That is correct (futex waiters re-check their
 * condition, Mesa's do) but slower under heavy contention.
 *
 * The table is `static`, so a second includer would get its own buckets and waiters and wakers
 * would never meet. The non-static sentinel below makes a second includer fail at link time.
 */
#pragma once
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#define UMTX_OP_WAIT               2
#define UMTX_OP_WAKE               3
#define UMTX_OP_WAIT_UINT          11
#define UMTX_OP_WAIT_UINT_PRIVATE  15
#define UMTX_OP_WAKE_PRIVATE       16

/* The timed variant. futex.c uses _umtx_time with UMTX_ABSTIME for futex_wait's deadline (FreeBSD's
   stable ABI). */
#define UMTX_ABSTIME               0x01

struct _umtx_time {
  struct timespec _timeout;
  uint32_t        _flags;
  uint32_t        _clockid;
  };

#ifdef __cplusplus
extern "C" {
#endif

#define ORBIS_UMTX_BUCKETS 64

struct orbis_umtx_bucket {
  pthread_mutex_t lock;
  pthread_cond_t  cond;
  };

static struct orbis_umtx_bucket orbis_umtx_bucket[ORBIS_UMTX_BUCKETS];
static pthread_once_t orbis_umtx_once = PTHREAD_ONCE_INIT;

/* Makes a second includer collide at link time (see the note at the top); the ODR lint is intended. */
int orbis_umtx_single_translation_unit; /* NOLINT(misc-definitions-in-headers) */

/* Which clock pthread_cond_timedwait measures its deadline against.
 *
 * futex.c passes an absolute deadline on CLOCK_MONOTONIC, while a default condvar uses
 * CLOCK_REALTIME per POSIX. Passing the timespec through compared uptime against the epoch, so every
 * timed wait expired instantly and the caller spun.
 *
 * pthread_condattr_setclock is not used: pthread_condattr_t here is a 4-byte
 * `struct { unsigned __attr; }`, the same shape as pthread_mutexattr_t, whose implementation writes
 * past it (it corrupted 24 bytes of an unrelated stack frame).
 *
 * So the condvar's clock is measured once and deadlines are converted to it. */
static int orbis_umtx_cond_clock = CLOCK_REALTIME;

/* The probe result is exported so it can be logged (a wrong conversion and a wrong clock choice are
 * otherwise indistinguishable). The accessors are not static: one translation unit owns this header
 * and the driver reads them through an extern declaration. */
static int64_t orbis_umtx_probe_ms;

int orbis_umtx_cond_clock_probed(void);
int64_t orbis_umtx_cond_probe_ms(void);

/* Waiter register. Every simple_mtx in Mesa that actually blocks calls futex_wait, which is this
 * file, so a thread stuck on any such lock is inside the WAIT arm below. Recording it costs two
 * atomics on a path that is about to sleep anyway. (The PS4 driver's own watchdog only covers its
 * own locks.)
 *
 * Not covered: pthread_mutex_lock on Sony's mutexes (u_mutex, mtx_t, anything not simple_mtx),
 * condvars waited on directly, and blocking syscalls. An empty dump while nothing progresses points
 * at one of those.
 */
#define ORBIS_UMTX_WAITERS 64

struct orbis_umtx_waiter {
  uint64_t    thread;    /* 0 when the slot is free */
  const void *addr;
  uint64_t    since_ns;  /* wall clock, because CLOCK_MONOTONIC is CPU time in some processes here */
  };

int  orbis_umtx_waiter_count(void);
int  orbis_umtx_waiter_at(int i, uint64_t *thread, const void **addr, uint64_t *since_ns);
uint64_t orbis_umtx_now_ns(void);

/* The deadline is built on MONOTONIC so the probe is bounded either way: 20 ms away if the cond is
 * monotonic, or decades in the past if it is realtime (no wait). The reverse would wait out the
 * ~55-year difference between the clocks.
 *
 * The wait is timed with CLOCK_REALTIME because in the CTS process CLOCK_MONOTONIC is per-thread CPU
 * time (measured, three runs; it is a wall clock in the title's process) and would read ~0 for a
 * thread that slept. CLOCK_REALTIME is a wall clock in both. */
static inline void orbis_umtx_probe_cond_clock(void) {
  struct orbis_umtx_bucket *const b = &orbis_umtx_bucket[0];
  struct timespec deadline, t0, t1;

  clock_gettime(CLOCK_MONOTONIC, &deadline);
  deadline.tv_nsec += 20 * 1000 * 1000;
  if (deadline.tv_nsec >= 1000000000L) {
    deadline.tv_nsec -= 1000000000L;
    deadline.tv_sec++;
    }

  clock_gettime(CLOCK_REALTIME, &t0);
  pthread_mutex_lock(&b->lock);
  /* Nothing can signal this - the once-guard means no waiter exists yet - so it must time out. */
  pthread_cond_timedwait(&b->cond, &b->lock, &deadline);
  pthread_mutex_unlock(&b->lock);
  clock_gettime(CLOCK_REALTIME, &t1);

  const int64_t elapsed_ms =
    (int64_t)(t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000;

  /* Half the interval: a monotonic wait may return slightly early, a realtime one returns in
   * microseconds. */
  orbis_umtx_cond_clock  = (elapsed_ms >= 10) ? CLOCK_MONOTONIC : CLOCK_REALTIME;
  orbis_umtx_probe_ms    = elapsed_ms;
  }

static inline void orbis_umtx_init(void) {
  for (int i = 0; i < ORBIS_UMTX_BUCKETS; i++) {
    pthread_mutex_init(&orbis_umtx_bucket[i].lock, NULL);
    pthread_cond_init(&orbis_umtx_bucket[i].cond, NULL);
    }
  orbis_umtx_probe_cond_clock();
  }

/* The probe result, for reporting. Reading either one runs the probe first. */
static inline int orbis_umtx_cond_clock_id(void) {
  pthread_once(&orbis_umtx_once, orbis_umtx_init);
  return orbis_umtx_cond_clock;
  }

int orbis_umtx_cond_clock_probed(void) { /* NOLINT(misc-definitions-in-headers) */
  return orbis_umtx_cond_clock_id();
  }

int64_t orbis_umtx_cond_probe_ms(void) { /* NOLINT(misc-definitions-in-headers) */
  pthread_once(&orbis_umtx_once, orbis_umtx_init);
  return orbis_umtx_probe_ms;
  }

static struct orbis_umtx_waiter orbis_umtx_waiters[ORBIS_UMTX_WAITERS];

/* Call counters for this shim.
 *
 * Measured on hardware 2026-08-31: libkernel's internal memory drains by ~146 bytes per expired
 * syncobj wait in the graphics driver (~81 per frame) until the process dies. The driver's own
 * sceKernel/sceVideoOut calls (submits, flips, mprotect, direct memory, usleep) and the log sink were
 * ruled out.
 *
 * That wait's only libkernel-facing work is simple_mtx, whose contended path is futex_wait, i.e. a
 * pthread_cond_timedwait here. A leak that is allocated on entry and only released on a signalled
 * return would show up as timeouts tracking the driver's syncobj timeouts. If so, the fix is
 * ORBIS_UMTX_LIBKERNEL=1 or a wait without a deadline; if these stay flat, the allocation is inside
 * libkernel's syncobj path.
 *
 * Relaxed atomics: read every five seconds by a report, and must not synchronise the measured path. */
static unsigned long long orbis_umtx_n_wait;
static unsigned long long orbis_umtx_n_timedwait;
static unsigned long long orbis_umtx_n_timeout;
static unsigned long long orbis_umtx_n_lock;

static inline void orbis_umtx_bump(unsigned long long *c) {
  __atomic_fetch_add(c, 1ull, __ATOMIC_RELAXED);
  }

/* Not static: one translation unit owns the table, and the graphics driver reads these through a
   weak extern to log them next to the bytes lost. */
void orbis_umtx_stats(unsigned long long *waits, unsigned long long *timedwaits, /* NOLINT(misc-definitions-in-headers) */
                      unsigned long long *timeouts, unsigned long long *locks) {
  if (waits     !=NULL) *waits     = __atomic_load_n(&orbis_umtx_n_wait,    __ATOMIC_RELAXED);
  if (timedwaits!=NULL) *timedwaits= __atomic_load_n(&orbis_umtx_n_timedwait,__ATOMIC_RELAXED);
  if (timeouts  !=NULL) *timeouts  = __atomic_load_n(&orbis_umtx_n_timeout, __ATOMIC_RELAXED);
  if (locks     !=NULL) *locks     = __atomic_load_n(&orbis_umtx_n_lock,    __ATOMIC_RELAXED);
  }

uint64_t orbis_umtx_now_ns(void) { /* NOLINT(misc-definitions-in-headers) */
  struct timespec t;
  clock_gettime(CLOCK_REALTIME, &t);
  return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
  }

/* Lock-free: this runs on the way into a sleep and must not add lock contention of its own. */
static inline struct orbis_umtx_waiter *orbis_umtx_waiter_claim(const void *addr) {
  const uint64_t me = (uint64_t)(uintptr_t)pthread_self();
  for (int i = 0; i < ORBIS_UMTX_WAITERS; i++) {
    uint64_t expected = 0;
    if (__atomic_compare_exchange_n(&orbis_umtx_waiters[i].thread, &expected, me, 0,
                                    __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
      orbis_umtx_waiters[i].addr     = addr;
      orbis_umtx_waiters[i].since_ns = orbis_umtx_now_ns();
      return &orbis_umtx_waiters[i];
      }
    }
  /* More concurrent sleepers than slots; the dump reports its capacity, so this shows as truncation. */
  return NULL;
  }

static inline void orbis_umtx_waiter_release(struct orbis_umtx_waiter *w) {
  if (w != NULL)
    __atomic_store_n(&w->thread, (uint64_t)0, __ATOMIC_RELEASE);
  }

int orbis_umtx_waiter_count(void) { /* NOLINT(misc-definitions-in-headers) */
  int n = 0;
  for (int i = 0; i < ORBIS_UMTX_WAITERS; i++)
    if (__atomic_load_n(&orbis_umtx_waiters[i].thread, __ATOMIC_ACQUIRE) != 0)
      n++;
  return n;
  }

/* Index over occupied slots, so a caller can walk 0..count-1. Returns 0 when i is past the end (the
   set can shrink between the count and the walk; this debug reader takes no lock). */
int orbis_umtx_waiter_at(int i, uint64_t *thread, const void **addr, /* NOLINT(misc-definitions-in-headers) */
                         uint64_t *since_ns) {
  int seen = 0;
  for (int k = 0; k < ORBIS_UMTX_WAITERS; k++) {
    const uint64_t t = __atomic_load_n(&orbis_umtx_waiters[k].thread, __ATOMIC_ACQUIRE);
    if (t == 0)
      continue;
    if (seen++ != i)
      continue;
    if (thread)   *thread   = t;
    if (addr)     *addr     = orbis_umtx_waiters[k].addr;
    if (since_ns) *since_ns = orbis_umtx_waiters[k].since_ns;
    return 1;
    }
  return 0;
  }

/* Converts the caller's deadline to the cond's clock, via a relative interval. */
static inline void orbis_umtx_deadline(const struct _umtx_time *t, struct timespec *out) {
  struct timespec rel, base;

  if ((t->_flags & UMTX_ABSTIME) == 0) {
    /* Already an interval. FreeBSD allows it; futex.c does not use it. */
    rel = t->_timeout;
    } else {
    struct timespec now;
    if (clock_gettime((clockid_t)t->_clockid, &now) != 0)
      clock_gettime(CLOCK_MONOTONIC, &now);

    rel.tv_sec  = t->_timeout.tv_sec - now.tv_sec;
    rel.tv_nsec = t->_timeout.tv_nsec - now.tv_nsec;
    if (rel.tv_nsec < 0) {
      rel.tv_nsec += 1000000000L;
      rel.tv_sec--;
      }
    if (rel.tv_sec < 0) {          /* the deadline has already passed */
      rel.tv_sec  = 0;
      rel.tv_nsec = 0;
      }
    }

  clock_gettime((clockid_t)orbis_umtx_cond_clock, &base);
  out->tv_sec  = base.tv_sec + rel.tv_sec;
  out->tv_nsec = base.tv_nsec + rel.tv_nsec;
  if (out->tv_nsec >= 1000000000L) {
    out->tv_nsec -= 1000000000L;
    out->tv_sec++;
    }
  }

/* The address, not its contents. Shifted past the low bits because these are 4-byte aligned words
   and the bottom two bits are always zero. */
static inline struct orbis_umtx_bucket *orbis_umtx_bucket_for(const void *addr) {
  const uintptr_t v = (uintptr_t)addr >> 2;
  return &orbis_umtx_bucket[(v ^ (v >> 8)) % ORBIS_UMTX_BUCKETS];
  }

/* Lost-wakeup race: the bucket lock is held across both the comparison and the wait, and wakers
   take the same lock before signalling, so a wake after the value changed cannot slip past a
   waiter that has already read the old value. */
#ifdef ORBIS_UMTX_LIBKERNEL
/* Sony's own, reached by name. Everything above this point is bypassed. */
extern int _umtx_op(void* obj, int op, unsigned long val, void* uaddr, void* uaddr2);
#else
static inline int _umtx_op(void* obj, int op, unsigned long val, void* uaddr, void* uaddr2) {
  pthread_once(&orbis_umtx_once, orbis_umtx_init);

  switch (op) {
    case UMTX_OP_WAIT:
    case UMTX_OP_WAIT_UINT:
    case UMTX_OP_WAIT_UINT_PRIVATE: {
      struct orbis_umtx_bucket *const b = orbis_umtx_bucket_for(obj);
      const volatile uint32_t *const word = (const volatile uint32_t *)obj;
      int ret = 0;

      orbis_umtx_bump(&orbis_umtx_n_wait);
      orbis_umtx_bump(&orbis_umtx_n_lock);
      pthread_mutex_lock(&b->lock);
      if (*word != (uint32_t)val) {
        /* Already changed. FreeBSD returns EWOULDBLOCK; Mesa treats any error as "re-check". */
        pthread_mutex_unlock(&b->lock);
        errno = EWOULDBLOCK;
        return -1;
        }

      /* Registered only around the sleep; a caller that returned above never slept. */
      struct orbis_umtx_waiter *const reg = orbis_umtx_waiter_claim(obj);

      if (uaddr2 != NULL) {
        const struct _umtx_time *const t = (const struct _umtx_time *)uaddr2;
        struct timespec deadline;
        /* Converted, not passed through: the caller's clock and the cond's clock differ (see
           orbis_umtx_deadline). */
        orbis_umtx_deadline(t, &deadline);
        orbis_umtx_bump(&orbis_umtx_n_timedwait);
        ret = pthread_cond_timedwait(&b->cond, &b->lock, &deadline);
        if (ret == ETIMEDOUT)
          orbis_umtx_bump(&orbis_umtx_n_timeout);
        } else {
        ret = pthread_cond_wait(&b->cond, &b->lock);
        }

      orbis_umtx_waiter_release(reg);
      pthread_mutex_unlock(&b->lock);

      if (ret != 0) {
        errno = ret;
        return -1;
        }
      return 0;
      }

    case UMTX_OP_WAKE:
    case UMTX_OP_WAKE_PRIVATE: {
      struct orbis_umtx_bucket *const b = orbis_umtx_bucket_for(obj);

      /* Broadcast even when asked to wake one: addresses share buckets, so a single signal could
         wake the wrong sleeper and lose the wakeup. */
      orbis_umtx_bump(&orbis_umtx_n_lock);
      pthread_mutex_lock(&b->lock);
      pthread_cond_broadcast(&b->cond);
      pthread_mutex_unlock(&b->lock);
      (void)val;
      return 0;
      }

    default:
      /* Other ops are not used by Mesa on this path. Refuse rather than return a success that
         would make a lock never block. */
      errno = ENOSYS;
      return -1;
    }

  (void)uaddr;
  }
#endif /* ORBIS_UMTX_LIBKERNEL */

#ifdef __cplusplus
}
#endif
