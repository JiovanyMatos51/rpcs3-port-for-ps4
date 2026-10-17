// Copyright © 2026 Mikołaj Mikołajczyk
// SPDX-License-Identifier: MIT
// See include/orbis_clock.h for the defect, the measurement and why no clock id is used.
#ifdef __PS4__

#include <orbis_clock.h>
#include <orbis_log.h>

#include <orbis/libkernel.h>

#include <chrono>
#include <cerrno>
#include <cstdint>
#include <ctime>
#include <pthread.h>
#include <sys/time.h>

// Not gettimeofday (see realtimeNow() below): the kernel call, with no libc layer above it.
extern "C" int32_t sceKernelGettimeofday(struct timeval* tv);

// For clockIdProbe() only. The kernel's clock ids (FreeBSD numbering), which clock_gettime below
// hides; and the SCE name of pthread_condattr_setclock, which the SDK declares as void(). Weak, so a
// missing export is reported instead of failing the load.
extern "C" {
int32_t orbisKernelClockGettime(int32_t id, struct timespec* ts)
        __asm__("sceKernelClockGettime") __attribute__((weak));
int32_t orbisCondattrSetclock(pthread_condattr_t* attr, int32_t id)
        __asm__("scePthreadCondattrSetclock") __attribute__((weak));
}

namespace {

struct ClockBase {
  uint64_t freq   = 0;   // counter ticks per second, 0 if the SDK would not say
  uint64_t origin = 0;   // counter at first use, so tv_sec stays small and cannot overflow
  };

// Initialised on first use, not from a constructor: libc++ can call this before this
// translation unit's static initialisers have run.
ClockBase& clockBase() {
  static ClockBase b = [] {
    ClockBase t;
    t.freq   = sceKernelGetProcessTimeCounterFrequency();
    t.origin = sceKernelGetProcessTimeCounter();
    return t;
    }();
  return b;
  }

int monotonicNow(struct timespec* ts) {
  ClockBase& b = clockBase();
  if(b.freq==0) {
    // Documented in microseconds.
    const uint64_t us = sceKernelGetProcessTime();
    ts->tv_sec  = static_cast<decltype(ts->tv_sec)> (us/1000000ull);
    ts->tv_nsec = static_cast<decltype(ts->tv_nsec)>((us%1000000ull)*1000ull);
    return 0;
    }
  const uint64_t d = sceKernelGetProcessTimeCounter()-b.origin;
  // Seconds split off first: the remainder is < freq, so the multiply is bounded by
  // freq*1e9, far inside uint64 at any plausible counter frequency.
  ts->tv_sec  = static_cast<decltype(ts->tv_sec)> (d/b.freq);
  ts->tv_nsec = static_cast<decltype(ts->tv_nsec)>((d%b.freq)*1000000000ull/b.freq);
  return 0;
  }

// Must not call gettimeofday: musl's gettimeofday.lo calls clock_gettime, i.e. this file:
//
//     xor %edi,%edi          ; clockid = 0 = CLOCK_REALTIME
//     mov %r14,%rsi
//     call clock_gettime
//
// That infinite recursion crashed the first savegame (rip at gettimeofday+0x19, "the stack
// frames are too many to display"); only CLOCK_REALTIME users such as a save timestamp hit it.
// In general an interposer must not call a libc function implemented in terms of the one it
// replaces; check what the archive member actually calls.
int realtimeNow(struct timespec* ts) {
  struct timeval tv = {};
  if(sceKernelGettimeofday(&tv)!=0)
    return -1;
  ts->tv_sec  = static_cast<decltype(ts->tv_sec)> (tv.tv_sec);
  ts->tv_nsec = static_cast<decltype(ts->tv_nsec)>(tv.tv_usec*1000);
  return 0;
  }

int  unknownId     = -1;
bool unknownIdSeen = false;

// Nanoseconds on a kernel clock id, or -1 with the SCE code in *rc.
int64_t kernelClockNs(int32_t id, int32_t* rc) {
  struct timespec ts = {};
  const int32_t   r  = orbisKernelClockGettime!=nullptr ? orbisKernelClockGettime(id,&ts) : -1;
  if(rc!=nullptr)
    *rc = r;
  return r==0 ? int64_t(ts.tv_sec)*1000000000ll+int64_t(ts.tv_nsec) : -1;
  }

int64_t counterUs(uint64_t c0, uint64_t c1, uint64_t freq) {
  return freq!=0 ? int64_t((c1-c0)*1000000ull/freq) : -1;
  }

} // namespace

// Overrides libkernel.so's export for every reference inside this executable - the same
// mechanism orbis_stat.cpp uses against `stat` and orbis_mmap.cpp against `mmap`. libc++ is
// linked statically here, so its steady_clock and high_resolution_clock resolve to this.
/* Call counters.
 *
 * The graphics driver loses ~146 bytes of libkernel's internal memory per expired syncobj wait
 * (~81 per frame). Excluded so far: the driver's sceKernel/sceVideoOut calls (submits, flips,
 * mprotect, direct memory, usleep), the log sink, and the futex shim (0 timedwaits/timeouts).
 * What remains on the failing path is one mutex round trip and two clock reads
 * (orbis_wait_begin, orbis_wait_continue). If the leak is here, the monotonic count runs at about
 * twice the expired-wait count. Relaxed atomics, but still a locked add on one shared cache line from
 * every core, and steady_clock::now() is on hot paths (log timestamps, wait deadlines). An embedding
 * build that does not read them compiles them out with ORBIS_CLOCK_COUNTERS=0; orbis_clock_counts()
 * then reports 0. */
#ifndef ORBIS_CLOCK_COUNTERS
#define ORBIS_CLOCK_COUNTERS 1
#endif
static unsigned long long clkMonotonic;
static unsigned long long clkRealtime;

extern "C" void orbis_clock_counts(unsigned long long* monotonic, unsigned long long* realtime) {
  if(monotonic!=nullptr) *monotonic = __atomic_load_n(&clkMonotonic,__ATOMIC_RELAXED);
  if(realtime !=nullptr) *realtime  = __atomic_load_n(&clkRealtime, __ATOMIC_RELAXED);
  }

extern "C" int clock_gettime(clockid_t id, struct timespec* ts) {
  if(ts==nullptr) {
    errno = EFAULT;
    return -1;
    }
  switch(id) {
    case CLOCK_MONOTONIC:
    case CLOCK_MONOTONIC_RAW:
    case CLOCK_MONOTONIC_COARSE:
    case CLOCK_BOOTTIME:
#if ORBIS_CLOCK_COUNTERS
      __atomic_fetch_add(&clkMonotonic,1ull,__ATOMIC_RELAXED);
#endif
      return monotonicNow(ts);
    case CLOCK_REALTIME:
    case CLOCK_REALTIME_COARSE:
#if ORBIS_CLOCK_COUNTERS
      __atomic_fetch_add(&clkRealtime,1ull,__ATOMIC_RELAXED);
#endif
      return realtimeNow(ts);
    default:
      // CPU-time clocks. Nothing here uses them; answer with the monotonic clock rather than
      // EINVAL, and report it in clockProbe().
      if(!unknownIdSeen) {
        unknownIdSeen = true;
        unknownId     = int(id);
        }
      return monotonicNow(ts);
    }
  }

namespace orbis {

void clockProbe() {
  static bool done = false;
  if(done || orbis_log_enabled()==0)
    return;
  done = true;

  ClockBase& b = clockBase();
  if(b.freq!=0)
    orbis_log("clock: CLOCK_MONOTONIC served from sceKernelGetProcessTimeCounter, %llu tick(s)/s "
              "(%.3f MHz); CLOCK_REALTIME from sceKernelGettimeofday, not from gettimeofday",
              (unsigned long long)b.freq, double(b.freq)/1000000.0);
  else
    orbis_log("clock: sceKernelGetProcessTimeCounterFrequency returned 0 - CLOCK_MONOTONIC falls "
              "back to sceKernelGetProcessTime at microsecond resolution");

  // The reference is the counter, not sceKernelGetProcessTime: against process time a slow clock
  // and a CPU-time clock give the same ratio in a busy loop. Process time is measured too.
  const uint64_t freq = sceKernelGetProcessTimeCounterFrequency();

  struct Window {
    const char* name;
    double      procRatio   = 0.0;
    double      steadyRatio = 0.0;
    double      hiresRatio  = 0.0;
    };
  Window win[2] = {{"busy"},{"asleep"}};

  for(unsigned k=0; k<2; ++k) {
    const uint64_t c0 = sceKernelGetProcessTimeCounter();
    const uint64_t p0 = sceKernelGetProcessTime();
    const auto     s0 = std::chrono::steady_clock::now();
    const auto     h0 = std::chrono::high_resolution_clock::now();

    if(k==0) {
      // ~20 ms, spun against the counter so the window does not depend on the clocks under test.
      const uint64_t want = (freq!=0) ? (freq/50) : 0;
      if(want!=0)
        while(sceKernelGetProcessTimeCounter()-c0 < want) {}
      else
        for(volatile uint64_t i=0; i<20000000; ++i) {}
      } else {
      sceKernelUsleep(20000);
      }

    const uint64_t c1 = sceKernelGetProcessTimeCounter();
    const uint64_t p1 = sceKernelGetProcessTime();
    const auto     s1 = std::chrono::steady_clock::now();
    const auto     h1 = std::chrono::high_resolution_clock::now();

    const int64_t realUs   = (freq!=0) ? int64_t((c1-c0)*1000000ull/freq) : 0;
    const int64_t procUs   = int64_t(p1-p0);
    const int64_t steadyUs = std::chrono::duration_cast<std::chrono::microseconds>(s1-s0).count();
    const int64_t hiresUs  = std::chrono::duration_cast<std::chrono::microseconds>(h1-h0).count();

    win[k].procRatio   = realUs>0 ? double(procUs)  /double(realUs) : 0.0;
    win[k].steadyRatio = realUs>0 ? double(steadyUs)/double(realUs) : 0.0;
    win[k].hiresRatio  = realUs>0 ? double(hiresUs) /double(realUs) : 0.0;

    orbis_log("clock probe [%s]: counter %lld us (ref) | process-time %lld us | steady_clock %lld us "
              "| high_resolution_clock %lld us", win[k].name, (long long)realUs, (long long)procUs,
              (long long)steadyUs, (long long)hiresUs);
    }

  // Ratios on their own line, easier to read than raw microseconds.
  for(unsigned k=0; k<2; ++k)
    orbis_log("clock probe [%s]: proc/ref %.4f steady/ref %.4f hires/ref %.4f  (1.0 = agrees with "
              "the counter; ~0.0 while asleep = it is a CPU-time clock)", win[k].name,
              win[k].procRatio, win[k].steadyRatio, win[k].hiresRatio);

  if(win[0].steadyRatio>0.9 && win[1].steadyRatio<0.1)
    orbis_log("clock probe: VERDICT THE INTERPOSER IS NOT IN THIS BINARY - steady_clock tracks the "
              "counter while busy and stops dead while asleep, which is CLOCK_VIRTUAL");
  else if(win[0].steadyRatio>0.9 && win[1].steadyRatio>0.9)
    orbis_log("clock probe: VERDICT the monotonic clock is WALL TIME - it agrees with the counter "
              "busy and asleep alike, which is what this file exists to guarantee");
  else
    orbis_log("clock probe: VERDICT INCONCLUSIVE - neither window gave a clean ratio");

  if(unknownIdSeen)
    orbis_log("clock probe: clock id %d was requested and answered with the monotonic clock - it is "
              "a CPU-time clock and nothing should be asking for it", unknownId);
  }

void clockIdProbe() {
  static bool done = false;
  if(orbis_log_enabled()==0 || __atomic_exchange_n(&done,true,__ATOMIC_ACQ_REL))
    return;

  // 1. What ids 0/1/4 measure: the same busy and asleep windows as clockProbe().
  const uint64_t freq     = sceKernelGetProcessTimeCounterFrequency();
  const bool     haveIds  = orbisKernelClockGettime!=nullptr && freq!=0;
  const int32_t  ids[3]   = {0,1,4};
  double         rate[2][3];
  for(auto& w : rate)
    for(double& r : w)
      r = -1.0;

  if(!haveIds) {
    orbis_log("clock ids: %s - kernel clock ids not measured", orbisKernelClockGettime==nullptr
              ? "sceKernelClockGettime is not exported" : "the counter frequency is 0");
    } else {
    const char* names[2] = {"busy","asleep"};
    int32_t     rc[3]    = {0,0,0};
    for(unsigned k=0; k<2; ++k) {
      int64_t v0[3], v1[3];
      const uint64_t c0 = sceKernelGetProcessTimeCounter();
      for(unsigned i=0; i<3; ++i)
        v0[i] = kernelClockNs(ids[i],&rc[i]);
      if(k==0) {
        while(sceKernelGetProcessTimeCounter()-c0 < freq/50) {}
        } else {
        sceKernelUsleep(20000);
        }
      const uint64_t c1 = sceKernelGetProcessTimeCounter();
      for(unsigned i=0; i<3; ++i)
        v1[i] = kernelClockNs(ids[i],nullptr);

      const int64_t refUs = counterUs(c0,c1,freq);
      long long     us[3];
      for(unsigned i=0; i<3; ++i) {
        us[i] = (v0[i]>=0 && v1[i]>=0) ? (long long)((v1[i]-v0[i])/1000) : -1;
        if(refUs>0 && us[i]>=0)
          rate[k][i] = double(us[i])/double(refUs);
        }
      orbis_log("clock ids [%s]: counter %lld us (ref) | id0 %lld us | id1 %lld us | id4 %lld us  "
                "(-1 = call failed)", names[k], (long long)refUs, us[0], us[1], us[2]);
      }
    for(unsigned i=0; i<3; ++i)
      if(rc[i]!=0)
        orbis_log("clock ids: sceKernelClockGettime(%d) failed with 0x%08x", int(ids[i]), unsigned(rc[i]));

    // A condvar on id 4 compares absolute deadlines against this value, not against ours.
    struct timespec own = {};
    monotonicNow(&own);
    const int64_t id4 = kernelClockNs(4,nullptr);
    if(id4>=0)
      orbis_log("clock ids: id4 reads %lld.%06lld s; this file's CLOCK_MONOTONIC reads %lld.%06lld s "
                "(counter since first use)", (long long)(id4/1000000000ll),
                (long long)((id4%1000000000ll)/1000), (long long)own.tv_sec, (long long)(own.tv_nsec/1000));
    }

  // 2. What each setclock entry point returns and stores. 3 is no FreeBSD clock, so its result shows
  // how scePthreadCondattrSetclock encodes an error.
  int     rcPosix4  = -1, clkPosix4 = -1, clkSce1 = -1, clkSce4 = -1;
  int32_t rcSce1    = -1, rcSce4    = -1, rcSce3  = -1;
  pthread_condattr_t attr;
  if(pthread_condattr_init(&attr)==0) {
    clockid_t c = -1;
    rcPosix4 = pthread_condattr_setclock(&attr,4);
    if(pthread_condattr_getclock(&attr,&c)==0)
      clkPosix4 = int(c);
    if(orbisCondattrSetclock!=nullptr) {
      rcSce1 = orbisCondattrSetclock(&attr,1);
      c = -1;
      if(pthread_condattr_getclock(&attr,&c)==0)
        clkSce1 = int(c);
      rcSce4 = orbisCondattrSetclock(&attr,4);
      c = -1;
      if(pthread_condattr_getclock(&attr,&c)==0)
        clkSce4 = int(c);
      rcSce3 = orbisCondattrSetclock(&attr,3);
      }
    pthread_condattr_destroy(&attr);
    }
  if(orbisCondattrSetclock!=nullptr)
    orbis_log("clock ids: pthread_condattr_setclock(4) rc %d -> clock %d | scePthreadCondattrSetclock(1) "
              "rc 0x%08x -> clock %d, (4) rc 0x%08x -> clock %d, (3) rc 0x%08x", rcPosix4, clkPosix4,
              unsigned(rcSce1), clkSce1, unsigned(rcSce4), clkSce4, unsigned(rcSce3));
  else
    orbis_log("clock ids: pthread_condattr_setclock(4) rc %d -> clock %d | scePthreadCondattrSetclock is "
              "not exported", rcPosix4, clkPosix4);

  // 3. Only when id 4 tracked the counter busy and asleep: a deadline on id 4 then lies 20 ms ahead
  // on a wall clock. A condvar that ignored the attribute would measure it against CLOCK_REALTIME,
  // where it is decades in the past, and return at once. Either way the wait is bounded.
  const bool id4Wall = haveIds && rate[0][2]>0.9 && rate[0][2]<1.1 && rate[1][2]>0.9 && rate[1][2]<1.1;
  const bool id1Cpu  = haveIds && rate[1][1]>=0.0 && rate[1][1]<0.1;
  int        waitRc  = -1;
  long long  waitUs  = -1;
  if(id4Wall && rcPosix4==0 && clkPosix4==4) {
    pthread_condattr_t ca;
    if(pthread_condattr_init(&ca)==0) {
      pthread_mutex_t mu;
      if(pthread_condattr_setclock(&ca,4)==0 && pthread_mutex_init(&mu,nullptr)==0) {
        pthread_cond_t cv;
        if(pthread_cond_init(&cv,&ca)==0) {
          const int64_t now4 = kernelClockNs(4,nullptr);
          if(now4>=0) {
            const int64_t   dl       = now4+20000000ll;
            struct timespec deadline = {};
            deadline.tv_sec  = static_cast<decltype(deadline.tv_sec)> (dl/1000000000ll);
            deadline.tv_nsec = static_cast<decltype(deadline.tv_nsec)>(dl%1000000000ll);
            const uint64_t c0 = sceKernelGetProcessTimeCounter();
            pthread_mutex_lock(&mu);
            waitRc = pthread_cond_timedwait(&cv,&mu,&deadline); // nothing signals it
            pthread_mutex_unlock(&mu);
            waitUs = (long long)counterUs(c0,sceKernelGetProcessTimeCounter(),freq);
            }
          pthread_cond_destroy(&cv);
          }
        pthread_mutex_destroy(&mu);
        }
      pthread_condattr_destroy(&ca);
      }
    orbis_log("clock ids: clock-4 condvar, 20 ms id-4 deadline, nothing signalling: rc %d after %lld us",
              waitRc, waitUs);
    }

  const bool waitOk = waitRc==ETIMEDOUT && waitUs>=15000 && waitUs<=60000;
  if(!id4Wall)
    orbis_log("clock ids: VERDICT a condattr remap to id 4 is NOT viable - id 4 did not track the counter "
              "both busy and asleep");
  else if(!waitOk)
    orbis_log("clock ids: VERDICT a condattr remap to id 4 is NOT viable yet - id 4 is wall time but a "
              "clock-4 condvar did not time out on its deadline");
  else
    orbis_log("clock ids: VERDICT id 4 is wall time and a clock-4 condvar honours it%s; a condattr remap "
              "1/4/7 -> 4 also needs this file's CLOCK_MONOTONIC on id 4's epoch",
              id1Cpu ? " (id 1 is CPU time, as Mesa's condvars get today)" : "");
  }

} // namespace orbis

#endif
