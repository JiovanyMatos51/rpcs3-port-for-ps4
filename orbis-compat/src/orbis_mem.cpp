// Copyright © 2026 Mikołaj Mikołajczyk
// SPDX-License-Identifier: MIT
#include "orbis_mem.h"
#include "orbis_mmap.h"
#include "orbis_thread.h"
#include "orbis_timer.h"

#include <orbis_log.h>

#include <orbis_env.h>

#include <errno.h>
#include <pthread.h>
#include <sys/mman.h>
#include <time.h>
#include <sys/types.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>

// Bound to the libkernel exports by asm label rather than declared by name: the SDK
// header's prototypes are wrong (as in orbis_stat.cpp), and <orbis/libkernel.h> may
// arrive transitively, so redeclaring the same C++ name would be a hard error.
//
//   sceKernelAvailableFlexibleMemorySize - the installed toolchain declares it
//     `(size_t)`, by value, which cannot be a query. Upstream OpenOrbis fixed it to
//     `(size_t*)` (OpenOrbis-PS4-Toolchain include/orbis/libkernel.h:55).
//   sceKernelAvailableDirectMemorySize - the SDK declares the last two arguments by
//     value (`(off_t,off_t,size_t,off_t,size_t)`), the same defect, with no corrected
//     version upstream. The out-pointer form below is assumed; the return code is
//     checked and a non-zero one is printed.
//   sceKernelGetDirectMemorySize - as used by the SDK's own sample
//     (samples/_common/graphics.cpp:119); aliased here so all three reads are together.
extern "C" {
int32_t ogFlexibleAvailable(size_t* out)
        __asm__("sceKernelAvailableFlexibleMemorySize");
int32_t ogDirectAvailable(off_t start, off_t end, size_t align,
                          off_t* physOut, size_t* sizeOut)
        __asm__("sceKernelAvailableDirectMemorySize");
size_t  ogDirectTotal(void)
        __asm__("sceKernelGetDirectMemorySize");
}

namespace {

std::atomic<uint64_t> flexBoot{0};
std::atomic<uint64_t> flexLow{~uint64_t(0)};
std::atomic<uint64_t> biggestRequest{0};
std::atomic<int32_t>  flexQueryErr{0};
std::atomic<int>      oomAnnounced{0};

uint64_t flexAvailable() {
  size_t  v  = 0;
  int32_t rc = ogFlexibleAvailable(&v);
  flexQueryErr.store(rc,std::memory_order_relaxed);
  if(rc!=0)
    return 0;

  // Monotone low-water mark, updated by every census and refusal, so a phase that spikes
  // and gives the memory back is still visible afterwards.
  uint64_t low = flexLow.load(std::memory_order_relaxed);
  while(v<low && !flexLow.compare_exchange_weak(low,v,std::memory_order_relaxed))
    ;
  return v;
  }

uint64_t directAvailable(int32_t& rc) {
  off_t  phys = 0;
  size_t sz   = 0;
  rc = ogDirectAvailable(0,off_t(ogDirectTotal()),0x200000,&phys,&sz);
  return rc==0 ? uint64_t(sz) : 0;
  }

void report(const char* where) {
  const uint64_t flex   = flexAvailable();
  const int32_t  flexRc = flexQueryErr.load(std::memory_order_relaxed);
  // A consumer that never calls memCensusBaseline() gets its baseline from the first successful
  // sample; a zero baseline printed "boot 0 KiB" and a negative "consumed since boot".
  uint64_t unset = 0;
  if(flexRc==0 && flex!=0)
    flexBoot.compare_exchange_strong(unset,flex,std::memory_order_relaxed);
  const uint64_t boot   = flexBoot.load(std::memory_order_relaxed);
  const uint64_t low    = flexLow.load(std::memory_order_relaxed);

  int32_t        dRc    = 0;
  const uint64_t dAvail = directAvailable(dRc);
  const uint64_t dTotal = uint64_t(ogDirectTotal());

  if(flexRc!=0) {
    orbis_log("mem census v1 [%s]: flexible query FAILED rc=0x%08x - this build cannot see "
            "the pool musl allocates from",where,unsigned(flexRc));
    } else {
    orbis_log("mem census v1 [%s]: flexible avail %llu KiB, boot %llu KiB, low-water %llu KiB, "
            "consumed since boot %lld KiB",
            where,(unsigned long long)(flex/1024),(unsigned long long)(boot/1024),
            (unsigned long long)(low/1024),(long long)((int64_t(boot)-int64_t(flex))/1024));
    }

  if(dRc!=0)
    orbis_log("mem census v1 [%s]: direct total %llu KiB, avail query rc=0x%08x (unread)",
            where,(unsigned long long)(dTotal/1024),unsigned(dRc));
  else
    orbis_log("mem census v1 [%s]: direct total %llu KiB, avail %llu KiB",
            where,(unsigned long long)(dTotal/1024),(unsigned long long)(dAvail/1024));

  // How much of the direct figure is the mmap interposer's (CPU heap vs GNM arenas), and
  // which pool musl is currently fed from.
  orbis::mmapDirectReport(where);
  // The "avail" figure above is only the largest free run; the ledger has the free total. The first
  // census probes sceKernelDirectMemoryQuery (one "mem ledger: query" line).
  orbis::directLedgerReport(where);
  }

// The refusal. Runs with malloc already returning null, so it must not allocate, and neither
// may the registered logger (e.g. one formatting into a 512-byte stack buffer). The kernel
// queries write into locals.
void announceOom(size_t want, size_t align) {
  // Bounded: after the first failure every allocation tends to fail, and a flood would
  // bury the first, most informative report.
  const int announced = oomAnnounced.fetch_add(1,std::memory_order_relaxed);
  if(announced>=8)
    return;

  const uint64_t flex   = flexAvailable();
  const int32_t  flexRc = flexQueryErr.load(std::memory_order_relaxed);
  int32_t        dRc    = 0;
  const uint64_t dAvail = directAvailable(dRc);

  orbis_log("mem OUT OF MEMORY: operator new(%llu bytes, align %llu) returned null",
          (unsigned long long)want,(unsigned long long)align);
  orbis_log("mem OUT OF MEMORY:   flexible avail %llu KiB (rc=0x%08x), boot %llu KiB, "
          "low-water %llu KiB - THIS is the pool musl's malloc grows into",
          (unsigned long long)(flex/1024),unsigned(flexRc),
          (unsigned long long)(flexBoot.load(std::memory_order_relaxed)/1024),
          (unsigned long long)(flexLow.load(std::memory_order_relaxed)/1024));
  orbis_log("mem OUT OF MEMORY:   direct total %llu KiB, avail %llu KiB (rc=0x%08x) - a "
          "large number here CANNOT serve this request, the pools are disjoint",
          (unsigned long long)(ogDirectTotal()/1024),
          (unsigned long long)(dAvail/1024),unsigned(dRc));
  orbis_log("mem OUT OF MEMORY:   largest single request this process ever made: %llu bytes",
          (unsigned long long)biggestRequest.load(std::memory_order_relaxed));
  // The first time only (allocation-free): the heap's counters and the direct-memory ledger (the free
  // total, not just one run).
  if(announced==0) {
    orbis::mmapDirectReport("out of memory");
    orbis::directLedgerReport("out of memory");
    }
  }

void noteRequest(size_t n) {
  // Racy on purpose: a diagnostic watermark that costs one relaxed load per allocation.
  if(n>biggestRequest.load(std::memory_order_relaxed))
    biggestRequest.store(n,std::memory_order_relaxed);
  }

void* allocPlain(size_t n) {
  noteRequest(n);
  return std::malloc(n!=0 ? n : 1);
  }

void* allocAligned(size_t n, size_t align) {
  noteRequest(n);
  // posix_memalign, as libc++'s own operator new uses on this toolchain (`U posix_memalign`
  // in stdlib_new_delete.cpp.o), so libc++'s operator delete frees these correctly.
  void* p = nullptr;
  if(align<sizeof(void*))
    align = sizeof(void*);
  if(posix_memalign(&p,align,n!=0 ? n : 1)!=0)
    return nullptr;
  return p;
  }

}

void orbis::memCensusBaseline() {
  size_t  v  = 0;
  int32_t rc = ogFlexibleAvailable(&v);
  flexQueryErr.store(rc,std::memory_order_relaxed);
  if(rc==0) {
    flexBoot.store(v,std::memory_order_relaxed);
    flexLow.store(v,std::memory_order_relaxed);
    }
  report("boot");

  // The other probes run here: this is called at boot with a logger registered, before the
  // application's threads exist. See include/orbis_thread.h.
  threadStackProbe();
  timerProbe();
  internalMemoryProbe();
  }

void orbis::memCensusThreads(const char* where) {
  unsigned long created = 0, raised = 0;
  threadCounts(&created,&raised);
  unsigned long altOk = 0, altFailed = 0, altSkipped = 0;
  threadAltStacks(&altOk,&altFailed,&altSkipped);
  // 0.29: "created" is cumulative (0.28 read 125 created at ~85 live), so the live estimate and the
  // exit/join notes ride along - see orbis::threadCensus.
  ThreadCensus tc{};
  threadCensus(&tc);
  orbis_log("thread census [%s]: %lu created, %lu raised to %llu KiB - %llu KiB of address space "
            "the platform's default would not have reserved; %lu on an alternate signal stack, "
            "%lu refused, %lu too small to spare one - %llu KiB, taken from those threads' own "
            "stacks rather than allocated; ~%lu live (%lu returned, %lu exit notes, %lu join notes, "
            "%lu create failures)",
            where,created,raised,
            (unsigned long long)(threadStackFloor()/1024),
            (unsigned long long)(raised*(threadStackFloor()>65536 ? threadStackFloor()-65536 : 0)/1024),
            altOk,altFailed,altSkipped,
            (unsigned long long)(altOk*threadAltStackSize()/1024),
            tc.live_estimate,tc.finished,tc.exited,tc.joined,tc.failed);
  }

void orbis::memCensus(const char* where) {
  report(where);
  }

// ------------------------------------------------------------ replaceable allocation
//
// libc++ ships these as weak symbols (`W _Znwm` in stdlib_new_delete.cpp.o), so a strong
// definition in the executable wins. The matching `operator delete`s are not replaced:
// libc++'s call free(), and everything below returns free-able pointers.
//
// Replaced so that a failure reports the request size and the state of both pools;
// libc++'s std::bad_alloc only says "out of memory".
//
// Each one keeps the standard loop ([new.delete.single]): while the allocation fails and a
// std::new_handler is installed, call it and retry. A handler frees memory, uninstalls itself or
// throws std::bad_alloc, so the loop ends; with no handler the first failure is final, as before.
// The report is only printed once no handler could help. The nothrow forms return null if the
// handler throws, as libc++'s do.

namespace {

void* plainWithHandler(size_t n) {
  for(;;) {
    if(void* p = allocPlain(n))
      return p;
    const std::new_handler h = std::get_new_handler();
    if(h==nullptr)
      return nullptr;
    h();
    }
  }

void* alignedWithHandler(size_t n, size_t align) {
  for(;;) {
    if(void* p = allocAligned(n,align))
      return p;
    const std::new_handler h = std::get_new_handler();
    if(h==nullptr)
      return nullptr;
    h();
    }
  }

void* plainNothrow(size_t n) noexcept {
  void* p = nullptr;
  try {
    p = plainWithHandler(n);
    } catch(...) {
    p = nullptr;
    }
  if(p==nullptr)
    announceOom(n,0);
  return p;
  }

void* alignedNothrow(size_t n, size_t align) noexcept {
  void* p = nullptr;
  try {
    p = alignedWithHandler(n,align);
    } catch(...) {
    p = nullptr;
    }
  if(p==nullptr)
    announceOom(n,align);
  return p;
  }

}

void* operator new(std::size_t n) {
  if(void* p = plainWithHandler(n))
    return p;
  announceOom(n,0);
  throw std::bad_alloc();
  }

void* operator new[](std::size_t n) {
  if(void* p = plainWithHandler(n))
    return p;
  announceOom(n,0);
  throw std::bad_alloc();
  }

void* operator new(std::size_t n, std::align_val_t a) {
  if(void* p = alignedWithHandler(n,static_cast<size_t>(a)))
    return p;
  announceOom(n,static_cast<size_t>(a));
  throw std::bad_alloc();
  }

void* operator new[](std::size_t n, std::align_val_t a) {
  if(void* p = alignedWithHandler(n,static_cast<size_t>(a)))
    return p;
  announceOom(n,static_cast<size_t>(a));
  throw std::bad_alloc();
  }

void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
  return plainNothrow(n);
  }

void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
  return plainNothrow(n);
  }

void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
  return alignedNothrow(n,static_cast<size_t>(a));
  }

void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
  return alignedNothrow(n,static_cast<size_t>(a));
  }


// ------------------------------------------------------- libkernel's internal memory, per primitive
//
// See the block in include/orbis_mem.h for why this exists.

namespace {

// Exported by libkernel.so but not declared in orbis/libkernel.h, so the signature is unknown.
// Calling it with a pointer to zeroed scratch covers both plausible shapes (`int f(size_t*)`
// writes through it and returns 0, `size_t f(void)` returns the size), and rsi/rdx get defined
// values in case it takes a length. Weak: if unresolved, the probe says so instead of failing
// to link.
extern "C" unsigned long long sceKernelInternalMemoryGetAvailableSize(void*, unsigned long long,
                                                                     unsigned long long)
    __attribute__((weak));

extern "C" int sceKernelUsleep(unsigned int);

unsigned long long internalFree() {
  if(&sceKernelInternalMemoryGetAvailableSize==nullptr)
    return 0;
  unsigned long long scratch[8] = {0,0,0,0,0,0,0,0};
  const unsigned long long ret = sceKernelInternalMemoryGetAvailableSize(scratch,sizeof(scratch),0);
  return (scratch[0]!=0) ? scratch[0] : ret;
  }

constexpr int kProbeIterations = 2000;

// A control loop is measured the same way, since other threads may consume the pool meanwhile.
void measureN(const char* what, void (*body)(), int n) {
  const unsigned long long before = internalFree();
  for(int i=0; i<n; i++)
    body();
  const unsigned long long after = internalFree();

  const long long lost = (long long)before - (long long)after;
  orbis_log("internal memory probe: %-26s %lld bytes over %d call(s) = %lld bytes each "
            "(%llu free after)",
            what,lost,n,lost/n,after);
  }

void measure(const char* what, void (*body)()) {
  measureN(what,body,kProbeIterations);
  }

void bodyNothing() {
  // Not empty, so the optimiser cannot delete the loop.
  static volatile int sink;
  sink = sink + 1;
  }

void bodyClockMonotonic() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC,&ts);
  }

void bodyClockRealtime() {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME,&ts);
  }

pthread_mutex_t g_probeMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t  g_probeCond  = PTHREAD_COND_INITIALIZER;

void bodyMutexLock() {
  pthread_mutex_lock(&g_probeMutex);
  pthread_mutex_unlock(&g_probeMutex);
  }

// The main candidate. The deadline is the epoch: a default cond may use CLOCK_REALTIME or
// CLOCK_MONOTONIC (both were seen in different processes), and {0,0} is in the past on either, so
// the wait never blocks.
void bodyCondTimedwaitExpired() {
  struct timespec epoch;
  epoch.tv_sec  = 0;
  epoch.tv_nsec = 0;
  pthread_mutex_lock(&g_probeMutex);
  pthread_cond_timedwait(&g_probeCond,&g_probeMutex,&epoch);
  pthread_mutex_unlock(&g_probeMutex);
  }

void bodyUsleep() {
  sceKernelUsleep(1);
  }

/* Allocator paths.
 *
 * Per-frame accounting put 9073 of 9280 lost bytes per frame above the graphics driver (frontend
 * between frames, and the draw/translate/record path). The only uncounted libkernel calls there are
 * the ones musl's allocator makes through orbis_mmap.cpp when a carve-out cannot serve a request.
 *
 * 96 KiB is above musl's mmap threshold, so it takes the path under test (a small malloc makes no
 * syscall). The raw mmap/munmap pair is heavier and measured with a lower count (500 is plenty for
 * the meter's 32-byte resolution). */
void bodyMallocLarge() {
  void* p = malloc(96*1024);
  if(p!=nullptr) {
    // Touched, so the memory is actually backed.
    *static_cast<volatile char*>(p) = 1;
    free(p);
    }
  }

void bodyMmapPair() {
  void* p = mmap(nullptr,64*1024,PROT_READ|PROT_WRITE,MAP_ANON|MAP_PRIVATE,-1,0);
  if(p!=MAP_FAILED && p!=nullptr) {
    *static_cast<volatile char*>(p) = 1;
    munmap(p,64*1024);
    }
  }

}

void orbis::internalMemoryProbe() {
  if(orbis_log_enabled()==0)
    return;

  const char* e = orbis_env_get("ORBIS_INTERNAL_MEM_PROBE");
  if(e==nullptr || *e=='\0' || *e=='0')
    return;

  if(&sceKernelInternalMemoryGetAvailableSize==nullptr) {
    orbis_log("internal memory probe: sceKernelInternalMemoryGetAvailableSize did not resolve - "
              "nothing here can be measured");
    return;
    }

  orbis_log("internal memory probe: %llu bytes free before any candidate runs. Each line below is "
            "one primitive run %d times in isolation; the control must read ~0 or every other line "
            "is inheriting somebody else's drift.",
            internalFree(),kProbeIterations);

  measure("control (nothing)",       &bodyNothing);
  measure("clock_gettime MONOTONIC", &bodyClockMonotonic);
  measure("clock_gettime REALTIME",  &bodyClockRealtime);
  measure("pthread_mutex lock+unlock",&bodyMutexLock);
  measure("sceKernelUsleep(1)",      &bodyUsleep);
  measure("cond_timedwait EXPIRED",  &bodyCondTimedwaitExpired);
  measure("malloc+free 96 KiB",      &bodyMallocLarge);
  measureN("mmap+munmap 64 KiB",     &bodyMmapPair, 500);

  {
    unsigned long long m=0,mc=0,mf=0,u=0,uc=0,uf=0;
    orbis_mmap_counts(&m,&mc,&mf,&u,&uc,&uf);
    orbis_log("internal memory probe: mmap traffic so far: %llu map(s) (%llu from a carve-out, %llu "
              "fell through to libkernel), %llu unmap(s) (%llu carve-out, %llu fell through). The "
              "fall-throughs are the ones that can cost internal memory.",
              m,mc,mf,u,uc,uf);
    }

  orbis_log("internal memory probe: done. The frame ledger puts 9073 of 9280 bytes a frame above the "
            "graphics driver, and bytes cannot leave this pool without a libkernel call - so if the "
            "malloc or mmap line above is non-zero, that is the consumer and it is reached from every "
            "allocation the frontend and zink make. If every line is 0, the call is one this overlay "
            "does not make either, and the remaining route is instrumenting zink itself.");
  }


/* C entry point for the frontend (which is C). memCensusBaseline() is not called by the
 * frontend, so the probe needs its own entry.
 *
 * It must be called after the frontend has applied the env files
 * (frontend/drivers/platform_orbis.c:472-484); a boot-time constructor here would run before
 * the knob is set. */
extern "C" void orbis_internal_memory_probe(void) {
  orbis::internalMemoryProbe();
  }
