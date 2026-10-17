// Copyright © 2026 Mikołaj Mikołajczyk
// SPDX-License-Identifier: MIT
// See include/orbis_thread.h.
#include <orbis_thread.h>

#include <orbis_log.h>
#include <orbis_mem.h>
#include <orbis_env.h>

#include <execinfo.h>
#include <pthread.h>
#include <pthread_np.h>
#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <atomic>

// libkernel's own creator, which the SDK's pthread_create (0xda78) wraps. Calling it directly lets
// this file define pthread_create without recursing.
extern "C" int scePthreadCreate(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *,
                                const char *);

// 0.29: used by the create-failure diagnosis, the VA-map walk and the limits dump below. Bound by asm
// label, as in orbis_mem.cpp, so no SDK prototype can clash (orbis/libkernel.h declares
// sceKernelAvailableFlexibleMemorySize by value), and all weak: if one is missing, the line that needed
// it says "unavailable" instead of failing the link or calling through null. All exported by the
// toolchain's libkernel.so (llvm-nm -D, 2026-10-02): getrlimit d618,
// sceKernelAvailableFlexibleMemorySize dfe6, sceKernelGetProcessTime e4be,
// sceKernelInternalMemoryGetAvailableSize e7ca, sceKernelMapNamedFlexibleMemory e928, sceKernelMunmap
// e9e6, sceKernelReserveVirtualRange eb44, sceKernelUsleep ed38, sceKernelVirtualQuery ed56,
// sysctlbyname f530. libc.a defines none of them (it only references getrlimit).
//
// sysctlbyname here is libkernel's; this file must not include orbis-compat's <sys/sysctl.h>, whose
// static inline stub of the same name answers -1 to everything except hw.ncpu.
extern "C" {
int32_t  orbisThrVirtualQuery(const void*, int32_t, void*, size_t)
         __asm__("sceKernelVirtualQuery") __attribute__((weak));
int32_t  orbisThrReserve(void**, size_t, int32_t, size_t)
         __asm__("sceKernelReserveVirtualRange") __attribute__((weak));
int32_t  orbisThrMapFlexible(void**, size_t, int32_t, int32_t, const char*)
         __asm__("sceKernelMapNamedFlexibleMemory") __attribute__((weak));
int32_t  orbisThrMunmap(void*, size_t)
         __asm__("sceKernelMunmap") __attribute__((weak));
int32_t  orbisThrFlexAvailable(size_t*)
         __asm__("sceKernelAvailableFlexibleMemorySize") __attribute__((weak));
int32_t  orbisThrUsleep(uint32_t)
         __asm__("sceKernelUsleep") __attribute__((weak));
uint64_t orbisThrProcessTime(void)
         __asm__("sceKernelGetProcessTime") __attribute__((weak));
int      orbisThrSysctl(const char*, void*, size_t*, const void*, size_t)
         __asm__("sysctlbyname") __attribute__((weak));
// FreeBSD's struct rlimit is two 64-bit values, cur then max (same layout as musl's), but the
// resource numbers are FreeBSD's (see limitsReport), so <sys/resource.h> is not used.
int      orbisThrGetrlimit(int, unsigned long long*)
         __asm__("getrlimit") __attribute__((weak));
// Not in any SDK header. Called as orbis_mem.cpp's internalFree() does (see the comment there).
unsigned long long orbisThrInternalAvailable(void*, unsigned long long, unsigned long long)
         __asm__("sceKernelInternalMemoryGetAvailableSize") __attribute__((weak));
}

namespace {

size_t g_defaultStack = 0;

std::atomic<unsigned long> g_created{0};
std::atomic<unsigned long> g_raised{0};
std::atomic<unsigned long> g_finished{0};

// Failure counters. Runs have ended with thousands of
//
//     [ScePthread/System] Internal Memory is running out.     technote 235
//
// in klog and nothing from this file, because the census only printed successful creates. These
// counters and reportPoolFailure() make failures visible, with the callers' addresses.
//
// The attr counter is the leading indicator: scePthreadAttrInit uses the same pool and is called
// first (twice per create here, plus once per stackOfSelf), so it fails before thread creation does.
std::atomic<unsigned long> g_createFailed{0};
std::atomic<unsigned long> g_attrFailed{0};

// 0.29: live thread count. g_finished counts start routines that return, but RPCS3's thread_base
// threads leave through pthread_exit (Utilities/Thread.cpp finalize, emergency_exit, silent_exit),
// which the trampoline never sees (0.28 reported "125 created, 5 returned" with ~85 alive).
// orbis::threadNoteExit(), called right before pthread_exit, completes it:
// live = created - returned - noted exits.
std::atomic<unsigned long> g_createdOk{0};      // scePthreadCreate returned 0 through this file
std::atomic<unsigned long> g_untracked{0};      // ... of those, started without the trampoline
std::atomic<unsigned long> g_exited{0};         // orbis::threadNoteExit() calls
// 0.29: orbis::threadNoteJoin() calls. Only a joined thread's stack can be reused by the next create
// (in 0.28, 3 creates cost 0 B of flexible memory), so exited - joined is how many finished threads
// still pin a 2 MiB stack.
std::atomic<unsigned long> g_joined{0};
std::atomic<unsigned long> g_callerStacks{0};   // attrs passed through because they carry a stack
std::atomic<unsigned long> g_explicitSizes{0};  // attrs passed through for an explicit size

// 0.29: which caller attrs pthread_create passes through unraised. On in the RPCS3 build, whose
// thread_base gives a refused thread its own stack and may size service threads deliberately. Off
// (raise everything) for every other consumer unless it defines the macros.
#ifndef ORBIS_THREAD_PASS_CALLER_STACK
#  ifdef RPCS3_PS4
#    define ORBIS_THREAD_PASS_CALLER_STACK 1
#  else
#    define ORBIS_THREAD_PASS_CALLER_STACK 0
#  endif
#endif
#ifndef ORBIS_THREAD_EXPLICIT_STACK_MIN
#  ifdef RPCS3_PS4
#    define ORBIS_THREAD_EXPLICIT_STACK_MIN (128*1024)
#  else
#    define ORBIS_THREAD_EXPLICIT_STACK_MIN 0
#  endif
#endif
// 0.29: print the limits report (orbis::limitsReport) on the first create made with a logger
// registered. RPCS3 never calls memCensusBaseline()/threadStackProbe(), where other consumers get the
// report, and the diagnosis needs the boot value of kern.threads.max_threads_hits. On for RPCS3, off
// otherwise.
#ifndef ORBIS_THREAD_LIMITS_AT_FIRST_CREATE
#  ifdef RPCS3_PS4
#    define ORBIS_THREAD_LIMITS_AT_FIRST_CREATE 1
#  else
#    define ORBIS_THREAD_LIMITS_AT_FIRST_CREATE 0
#  endif
#endif
// An attr carrying the caller's own stack (pthread_attr_setstack) keeps it, at any size.
constexpr bool   kPassCallerStack  = ORBIS_THREAD_PASS_CALLER_STACK!=0;
constexpr bool   kLimitsAtFirstCreate = ORBIS_THREAD_LIMITS_AT_FIRST_CREATE!=0;
// An attr whose stack size was set to at least this many bytes keeps it; 0 disables. A fresh attr
// reports 65536 here (measured), so 128 KiB or more was set on purpose.
constexpr size_t kExplicitStackMin = size_t(ORBIS_THREAD_EXPLICIT_STACK_MIN);

// 0.29: where libkernel puts thread stacks. Each new stack goes in a descending slot below the main
// stack (0.28: the first worker's alt stack at 0x7eede7ef0) and joined ones are reused, so a new low
// is a new allocation; the sequence of lows shows whether slots are running out. Caller-supplied
// stacks are excluded.
std::atomic<uintptr_t>     g_kstackLow{~uintptr_t(0)};
std::atomic<uintptr_t>     g_kstackHigh{0};
std::atomic<size_t>        g_kstackGuard{0};
std::atomic<unsigned long> g_kstackLows{0};

long liveNow() {
  return long(g_createdOk.load(std::memory_order_relaxed))
       - long(g_untracked.load(std::memory_order_relaxed))
       - long(g_finished.load(std::memory_order_relaxed))
       - long(g_exited.load(std::memory_order_relaxed));
  }

unsigned long liveClamped() {
  const long n = liveNow();
  return n>0 ? (unsigned long)n : 0ul;
  }

// libkernel's internal pool, exactly as orbis_mem.cpp's internalFree() and RPCS3's ps4_fatal.cpp read
// it; 0 when it cannot be read.
unsigned long long internalFreeNow() {
  if(&orbisThrInternalAvailable==nullptr)
    return 0;
  unsigned long long scratch[8] = {0,0,0,0,0,0,0,0};
  const unsigned long long ret = orbisThrInternalAvailable(scratch,sizeof(scratch),0);
  if(scratch[0]!=0)
    return scratch[0];
  // An `int f(size_t*)` shape that failed returns an SCE error (0x8xxxxxxx), not a size.
  const unsigned long long high = ret>>32;
  if((high==0 || high==0xffffffffull) && (ret & 0xf0000000ull)==0x80000000ull)
    return 0;
  return ret;
  }

// Flexible memory free in bytes, or 0 when it cannot be read.
unsigned long long flexibleFreeNow() {
  size_t v = 0;
  if(&orbisThrFlexAvailable==nullptr || orbisThrFlexAvailable(&v)!=0)
    return 0;
  return (unsigned long long)v;
  }

unsigned long long nowUs() {
  return (&orbisThrProcessTime!=nullptr) ? (unsigned long long)orbisThrProcessTime() : 0ull;
  }

// Powers of two, because failures are unbounded (klog carried 8592 in 133 s, 2026-08-31).
// 1, 2, 4 ... 4096 is twelve lines for four thousand failures.
bool powerOfTwo(unsigned long n) {
  return n!=0 && (n & (n-1))==0;
  }

void reportPoolFailure(const char* what, int rc, unsigned long n) {
  if(!powerOfTwo(n) || orbis_log_enabled()==0)
    return;

  // Addresses only (no symbolisation on the console); module bases are logged at load and
  // ps4/symbolise.py resolves them. Skips this function and the calling interposer.
  void* frames[10];
  const int nf = orbis_unwind_collect(frames,10,2);

  char   callers[10*20+1];
  size_t at = 0;
  callers[0] = '\0';
  for(int i=0; i<nf; i++) {
    const int n2 = snprintf(callers+at,sizeof(callers)-at,"%s%p",(i!=0) ? " " : "",frames[i]);
    // snprintf returns what it would have written; stop on truncation.
    if(n2<0 || (size_t)n2>=sizeof(callers)-at)
      break;
    at += (size_t)n2;
    }

  // 0.29: the cause is not assumed. This line used to blame the ScePthread internal pool (technote
  // 235), but in 0.28 (Demon's Souls) 13,657,696 bytes of internal memory were free at this error,
  // with 0 attr failures. It prints the numbers each candidate cause depends on (internal and
  // flexible memory free, live threads, lowest libkernel stack), and the one-shot 'thread create
  // DIAGNOSIS' and 'VERDICT' lines after the first failure identify the cause.
  //
  // Kept under 512 characters with ten callers: RPCS3's sink (ps4_fatal.cpp orbis_line) truncates
  // there, and the callers are at the end.
  const uintptr_t lo = g_kstackLow.load(std::memory_order_relaxed);
  const uintptr_t hi = g_kstackHigh.load(std::memory_order_relaxed);
  orbis_log("thread FAILURE: %s returned 0x%08x (%d), failure #%lu: %lu tried, %lu ok, %lu raised, "
            "~%lu live, %lu attr/%lu create failures; internal free %llu B, flexible free %llu KiB; "
            "libkernel stacks down to %p (%lu lows, span %llu KiB). Cause NOT assumed (see DIAGNOSIS). "
            "Callers, innermost first: %s",
            what,unsigned(rc),rc,n,
            g_created.load(std::memory_order_relaxed),
            g_createdOk.load(std::memory_order_relaxed),
            g_raised.load(std::memory_order_relaxed),
            liveClamped(),
            g_attrFailed.load(std::memory_order_relaxed),
            g_createFailed.load(std::memory_order_relaxed),
            internalFreeNow(),flexibleFreeNow()/1024,
            (void*)(lo==~uintptr_t(0) ? 0 : lo),
            g_kstackLows.load(std::memory_order_relaxed),
            (unsigned long long)((lo!=~uintptr_t(0) && hi>lo) ? (hi-lo)/1024 : 0),
            callers);
  }

// The stack size for a thread that did not choose one: the main thread's. Resolved once, on first
// use. Not configurable (it could only usefully go down, towards the guard page).
std::atomic<size_t> g_floor{~size_t(0)};   // ~0 means "not resolved yet"

size_t mainThreadStack();

size_t resolveFloor() {
  size_t v = g_floor.load(std::memory_order_relaxed);
  if(v!=~size_t(0))
    return v;

  v = mainThreadStack();

  // Below PTHREAD_STACK_MIN means the probe failed; then do nothing.
  if(v!=0 && v<size_t(PTHREAD_STACK_MIN))
    v = 0;

  g_floor.store(v,std::memory_order_relaxed);
  return v;
  }

// What a live thread actually got. pthread_attr_get_np is libkernel's (0xd88e) and fills an
// already initialised attr.
//
// 0.29: optionally also the stack base (pthread_attr_getstack, libkernel d8de) and guard size
// (pthread_attr_getguardsize, d8ac), each left at zero if its call fails. The size still comes from
// pthread_attr_getstacksize, which is known to work here and drives the alt-stack decision.
size_t stackOfSelf(int* rcOut, void** baseOut = nullptr, size_t* guardOut = nullptr) {
  pthread_attr_t a;
  size_t         sz = 0;

  const int rcInit = pthread_attr_init(&a);
  if(rcInit!=0) {
    reportPoolFailure("pthread_attr_init",rcInit,
                      g_attrFailed.fetch_add(1,std::memory_order_relaxed)+1);
    *rcOut = rcInit;
    return 0;
    }

  const int rcGet = pthread_attr_get_np(pthread_self(),&a);
  if(rcGet==0) {
    *rcOut = pthread_attr_getstacksize(&a,&sz);
    if(baseOut!=nullptr) {
      void*  base   = nullptr;
      size_t ignore = 0;
      if(pthread_attr_getstack(&a,&base,&ignore)==0)
        *baseOut = base;
      }
    if(guardOut!=nullptr) {
      size_t guard = 0;
      if(pthread_attr_getguardsize(&a,&guard)==0)
        *guardOut = guard;
      }
    }
  else
    *rcOut = rcGet;

  pthread_attr_destroy(&a);
  return sz;
  }

struct Result {
  size_t stack;
  int    rc;
  };

void* probeBody(void* p) {
  Result* r = static_cast<Result*>(p);
  r->stack  = stackOfSelf(&r->rc);
  return nullptr;
  }

// One thread, created the way the caller under test creates it. Returns 0 with rc non-zero if the
// thread could not be started.
size_t stackOfNewThread(const pthread_attr_t* attr, int* rcOut) {
  Result    r{0,0};
  pthread_t t;

  const int rcCreate = pthread_create(&t,attr,probeBody,&r);
  if(rcCreate!=0) { *rcOut = rcCreate; return 0; }

  pthread_join(t,nullptr);
  *rcOut = r.rc;
  return r.stack;
  }

// ------------------------------------------------------------------ the alternate signal stack
//
// See the header for why this comes from the thread's own stack rather than being allocated.
// Whether a new thread inherits its creator's td_sigstk is not verified: POSIX says it is not
// inherited, and FreeBSD 9's kern_thr.c zeroes td_startzero..td_endzero, but the field's position
// in sys/proc.h was not checked. So the first thread reads back what it had and logs it (an ss_sp
// equal to the main thread's buffer means inherited; zero or SS_DISABLE means not). The install is
// correct either way.
constexpr size_t kAltStackSize = 64*1024;

// Four times the alternate stack. A thread that cannot spare a quarter of itself is left alone.
constexpr size_t kAltStackMinThreadStack = 4*kAltStackSize;

std::atomic<unsigned long> g_altOk{0};
std::atomic<unsigned long> g_altFailed{0};
std::atomic<unsigned long> g_altSkipped{0};
std::atomic<int>           g_altReported{0};

struct StartArg {
  void* (*fn)(void*);
  void*   arg;
  int     callerStack;   // 0.29: the attr carried the caller's own stack - not libkernel's to place
  };

// noinline: the 64 KiB array below must live in one frame at the base of this thread until the
// thread body returns.
__attribute__((noinline))
void* runOnItsOwnAltStack(void* (*fn)(void*), void* arg) {
  // The handler enters at ss_sp + ss_size and grows down, so the signal frame stays inside this
  // array, which sits above every frame `fn` will push.
  alignas(16) char alt[kAltStackSize];

  // Touch each 16 KiB page once so the region is resident before the kernel writes a signal frame
  // into it (the frame was made with a single `sub rsp`). The thread stack is mapped by
  // scePthreadCreate, so this is probably not required, but it is cheap.
  for(size_t i=0; i<sizeof(alt); i+=16*1024)
    static_cast<volatile char*>(alt)[i] = 0;
  static_cast<volatile char*>(alt)[sizeof(alt)-1] = 0;

  stack_t prev;
  prev.ss_sp = nullptr; prev.ss_size = 0; prev.ss_flags = 0;
  errno = 0;
  const int prevRc  = sigaltstack(nullptr,&prev);
  const int prevErr = errno;

  stack_t ss;
  ss.ss_sp    = alt;
  ss.ss_size  = sizeof(alt);
  ss.ss_flags = 0;
  errno = 0;
  const int rc  = sigaltstack(&ss,nullptr);
  const int err = errno;

  if(rc==0)
    g_altOk.fetch_add(1,std::memory_order_relaxed);
  else
    g_altFailed.fetch_add(1,std::memory_order_relaxed);

  // Once per process; later threads would print the same values.
  if(g_altReported.exchange(1,std::memory_order_relaxed)==0 && orbis_log_enabled()!=0)
    orbis_log("thread alt stack: first worker thread - it INHERITED rc=%d errno=%d "
              "ss_sp=%p ss_size=%llu ss_flags=%d (0=nothing set, 4=SS_DISABLE, a non-null sp is "
              "the MAIN thread's buffer and means td_sigstk is copied on create); installed "
              "%llu KiB of this thread's own stack at %p rc=%d errno=%d - %s",
              prevRc,prevErr,prev.ss_sp,(unsigned long long)prev.ss_size,prev.ss_flags,
              (unsigned long long)(sizeof(alt)/1024),(void*)alt,rc,err,
              rc==0 ? "a stack overflow on a WORKER thread can now report itself"
                    : "REFUSED - worker threads still die silently on an overflow");

  void* r = fn(arg);

  // Keeps `alt` live across the call and prevents a tail call, which would pop this frame (still
  // referenced by td_sigstk) before `fn` runs.
  __asm__ __volatile__("" :: "r"(alt) : "memory");

  // Not disabled on the way out: td_sigstk goes away with the thread, and a crash during teardown
  // can still be reported on the still-mapped stack.
  return r;
  }

// 0.29: records one libkernel-allocated stack. Lock-free (two CAS loops). Logs only a new low, at
// powers of two and every 4th from the 64th to the 128th: about a dozen lines for ~85 stacks, still
// showing the stride (stack + guard) and where it stopped; RPCS3's sink keeps only 256 orbis-compat
// lines per session.
void noteStack(uintptr_t base, size_t size, size_t guard) {
  if(base==0 || size==0)
    return;
  if(guard!=0)
    g_kstackGuard.store(guard,std::memory_order_relaxed);

  const uintptr_t top = base+size;
  uintptr_t hi = g_kstackHigh.load(std::memory_order_relaxed);
  while(top>hi && !g_kstackHigh.compare_exchange_weak(hi,top,std::memory_order_relaxed))
    ;

  uintptr_t lo = g_kstackLow.load(std::memory_order_relaxed);
  while(base<lo) {
    if(!g_kstackLow.compare_exchange_weak(lo,base,std::memory_order_relaxed))
      continue;
    const unsigned long k = g_kstackLows.fetch_add(1,std::memory_order_relaxed)+1;
    if(orbis_log_enabled()!=0 && (powerOfTwo(k) || (k>=64 && k<=128 && k%4==0)))
      orbis_log("thread stack: new lowest libkernel stack #%lu [%p,%p) %llu KiB + %llu KiB guard, "
                "%llu KiB below the previous low, %llu KiB below the highest top; ~%lu live",
                k,(void*)base,(void*)top,(unsigned long long)(size/1024),
                (unsigned long long)(guard/1024),
                (unsigned long long)((lo!=~uintptr_t(0) && lo>base) ? (lo-base)/1024 : 0),
                (unsigned long long)((g_kstackHigh.load(std::memory_order_relaxed)-base)/1024),
                liveClamped());
    break;
    }
  }

void* threadTrampoline(void* p) {
  StartArg* sa                = static_cast<StartArg*>(p);
  void*   (*fn)(void*)        = sa->fn;
  void*     arg               = sa->arg;
  const int callerStack       = sa->callerStack;
  free(sa);

  // What the thread actually got (the interposer may have raised the request or passed it
  // through), via pthread_attr_get_np on self, as the probe does.
  int          rc    = 0;
  void*        base  = nullptr;
  size_t       guard = 0;
  const size_t sz    = stackOfSelf(&rc,&base,&guard);
  if(rc==0 && callerStack==0)
    noteStack(reinterpret_cast<uintptr_t>(base),sz,guard);

  void* r;
  if(rc==0 && sz>=kAltStackMinThreadStack)
    r = runOnItsOwnAltStack(fn,arg);
  else {
    g_altSkipped.fetch_add(1,std::memory_order_relaxed);
    r = fn(arg);
    }

  g_finished.fetch_add(1,std::memory_order_relaxed);
  return r;
  }

// ------------------------------------------------------------------ 0.29: the create-failure diagnosis
//
// scePthreadCreate refused Demon's Souls with 0x80020023 (SCE EAGAIN) at ~85 live threads in 0.27 and
// 0.28. Every new libkernel stack costs exactly 2 MiB of flexible memory and joined stacks are reused,
// so (a) running out of descending stack slots, (b) a hidden flexible-memory threshold and (c) a
// per-process thread cap all predict the same failure point; (f) the kernel's automatic VA area,
// (g) a map-entry limit and (h) RLIMIT_VMEM are VA-level candidates. Each experiment below separates
// one of them; all run once, on the first failure, and each undoes what it did.

// sceKernelVirtualQuery's info block, declared here rather than taken from orbis/libkernel.h (which
// names these fields unk01/unk02/unk04 and whose prototypes clash with this file's bindings). The
// SDK's five unsigned bitfields occupy the byte at 32 and name[] starts at 33 under the SysV ABI
// (checked by compiling both layouts with the console flags).
struct VqInfo {
  void*   start;
  void*   end;
  int64_t offset;
  int32_t prot;
  int32_t mtype;
  uint8_t flags;      // 0x01 flexible, 0x02 direct, 0x04 stack, 0x08 pooled, 0x10 committed
  char    name[32];   // not guaranteed to be terminated
  };
static_assert(sizeof(VqInfo)==72,"sceKernelVirtualQuery's info block is 72 bytes");
static_assert(offsetof(VqInfo,flags)==32 && offsetof(VqInfo,name)==33,"VqInfo layout drifted");

// SCE_KERNEL_VQ_FIND_NEXT = 1 is assumed (Sony's name; shadPS4 treats flags==1 as "the first mapping
// at or above addr"). Other callers in this library pass 0, where a wrong value could make a used
// range look free (orbis_jit.c). Here it only reads: a wrong value makes the first query fail and the
// walk prints that rc.
constexpr int32_t kVqFindNext = 1;

// Where hint-less mappings (0.28: carve-outs from 0x208400000 up) and libkernel's thread stacks
// (~0x7eedf8000 down) were seen. The fixed guest reservations at 0x300000000 and 0x900000000 are below
// it too; the guest's 0x1000000000 reservation, the JIT window and RPCS3's own windows are above it.
constexpr uintptr_t kAutoAreaEnd = 0x1000000000ull;

// SCE flags for sceKernelReserveVirtualRange, as ps4_virtual_memory.cpp uses them.
constexpr int32_t kMapFixed       = 0x10;
constexpr int32_t kMapNoOverwrite = 0x80;

std::atomic<int>       g_diagAnyDone{0};
std::atomic<int>       g_diagEagainDone{0};
std::atomic<int>       g_limitProbeActive{0};
std::atomic<int>       g_limitsReported{0};
std::atomic<long long> g_hitsBoot{-1};

bool isEagain(int rc) {
  return rc==35 || unsigned(rc)==0x80020023u;
  }

bool vqAvailable() {
  return &orbisThrVirtualQuery!=nullptr;
  }

int32_t vqNext(uintptr_t at, VqInfo& q) {
  memset(&q,0,sizeof(q));
  return orbisThrVirtualQuery(reinterpret_cast<const void*>(at),kVqFindNext,&q,sizeof(q));
  }

struct VmWalk {
  unsigned long      entries = 0, stacks = 0, autoEntries = 0;
  unsigned long long bytes = 0, committed = 0, flexible = 0, direct = 0, stackBytes = 0;
  unsigned long long autoBytes = 0, autoGap = 0, us = 0;
  uintptr_t          autoGapAt = 0, lowestStack = ~uintptr_t(0), highestStack = 0;
  int32_t            endRc = 0;
  bool               ran = false, truncated = false, stalled = false;
  };

// The whole map, one sceKernelVirtualQuery per entry. Read-only; bounded at 32768 entries, and it stops
// at the first rc != 0 (the end of the map - or, with no entry read, a refused walk) or the first entry
// that does not move forward.
void walkVm(VmWalk& w) {
  if(!vqAvailable())
    return;
  w.ran = true;
  const unsigned long long t0 = nowUs();
  uintptr_t at = 0, prevAutoEnd = 0;
  unsigned  i  = 0;
  for(; i<32768; ++i) {
    VqInfo  q;
    int32_t rc = vqNext(at,q);
    // 0.29: a refusal of address 0 itself (below the user map) is retried once from the eboot's base
    // (0x400000) before the walk is called refused.
    if(rc!=0 && i==0 && at==0) {
      at = 0x400000;
      rc = vqNext(at,q);
      }
    if(rc!=0) {
      w.endRc = rc;
      break;
      }
    const uintptr_t b = reinterpret_cast<uintptr_t>(q.start);
    const uintptr_t e = reinterpret_cast<uintptr_t>(q.end);
    if(e<=b || e<=at) {
      w.stalled = true;
      break;
      }
    const unsigned long long len = e-b;
    w.entries++;
    w.bytes += len;
    if(q.flags & 0x10) w.committed += len;
    if(q.flags & 0x01) w.flexible  += len;
    if(q.flags & 0x02) w.direct    += len;
    if(q.flags & 0x04) {
      w.stacks++;
      w.stackBytes += len;
      if(b<w.lowestStack)  w.lowestStack  = b;
      if(e>w.highestStack) w.highestStack = e;
      }
    if(e<=kAutoAreaEnd) {
      w.autoEntries++;
      w.autoBytes += len;
      if(prevAutoEnd!=0 && b>prevAutoEnd && b-prevAutoEnd>w.autoGap) {
        w.autoGap   = b-prevAutoEnd;
        w.autoGapAt = prevAutoEnd;
        }
      prevAutoEnd = e;
      }
    at = e;
    }
  if(i==32768)
    w.truncated = true;
  w.us = nowUs()-t0;
  }

// One integer sysctl, through libkernel. *errOut is 0, the errno, or -1 when the call is unavailable.
long long sysctlValue(const char* name, int* errOut) {
  if(&orbisThrSysctl==nullptr) {
    *errOut = -1;
    return -1;
    }
  long long v   = 0;
  size_t    len = sizeof(v);
  errno = 0;
  if(orbisThrSysctl(name,&v,&len,nullptr,0)!=0) {
    *errOut = (errno!=0) ? errno : -2;
    return -1;
    }
  *errOut = 0;
  if(len==sizeof(int)) {
    int iv = 0;
    memcpy(&iv,&v,sizeof(iv));
    return (long long)iv;
    }
  return v;
  }

void* diagNoop(void*) {
  return nullptr;
  }

// One throwaway thread through scePthreadCreate directly (not this file's pthread_create: no counter,
// trampoline or recursion into the diagnosis), joined before returning. *joined says whether the join
// happened (a caller-supplied stack may only be freed after it); *stage names the step a non-zero rc
// came from (only a refused create says anything about threads).
int diagThread(void* callerStack, size_t size, bool* joined, const char** stage) {
  if(joined!=nullptr)
    *joined = false;
  if(stage!=nullptr)
    *stage = "";
  pthread_attr_t a;
  int rc = pthread_attr_init(&a);
  if(rc!=0) {
    if(stage!=nullptr)
      *stage = " at attr init";
    return rc;
    }
  rc = (callerStack!=nullptr) ? pthread_attr_setstack(&a,callerStack,size)
                              : pthread_attr_setstacksize(&a,size);
  if(rc!=0) {
    if(stage!=nullptr)
      *stage = (callerStack!=nullptr) ? " at setstack" : " at setstacksize";
    }
  else {
    pthread_t t;
    rc = scePthreadCreate(&t,&a,diagNoop,nullptr,"orbis-diag");
    if(rc!=0) {
      if(stage!=nullptr)
        *stage = " at create";
      }
    else if(pthread_join(t,nullptr)==0 && joined!=nullptr)
      *joined = true;
    }
  pthread_attr_destroy(&a);
  return rc;
  }

void rcText(char* out, size_t cap, bool ran, int rc) {
  if(ran)
    snprintf(out,cap,"0x%08x",unsigned(rc));
  else
    snprintf(out,cap,"not-run");
  }

// The mappings in the 256 MiB under `top`, where libkernel's next stack would go. Collected before the
// diagnosis starts threads of its own (whose stacks libkernel may cache there) and printed after the
// verdict, nearest last.
struct BelowDump {
  static constexpr unsigned kKeep = 8;
  VqInfo    ring[kKeep];
  unsigned  seen  = 0;
  int32_t   rcEnd = 0;
  uintptr_t top   = 0;
  bool      ran   = false;
  };

void collectBelow(BelowDump& d, uintptr_t top) {
  if(!vqAvailable() || top==0)
    return;
  d.ran = true;
  d.top = top;
  uintptr_t at = (top>0x10000000ull) ? top-0x10000000ull : 0;
  for(unsigned i=0; i<4096; ++i) {
    VqInfo q;
    d.rcEnd = vqNext(at,q);
    if(d.rcEnd!=0)
      break;
    const uintptr_t b = reinterpret_cast<uintptr_t>(q.start);
    const uintptr_t e = reinterpret_cast<uintptr_t>(q.end);
    if(e<=b || e<=at || b>=top)
      break;
    d.ring[d.seen%BelowDump::kKeep] = q;
    d.seen++;
    at = e;
    }
  }

void printBelow(const BelowDump& d) {
  if(!d.ran)
    return;
  constexpr unsigned kKeep = BelowDump::kKeep;
  if(d.seen==0) {
    orbis_log("vm below stacks: nothing mapped in the 256 MiB under %p (walk ended rc 0x%08x)",
              (void*)d.top,unsigned(d.rcEnd));
    return;
    }
  if(d.seen>kKeep)
    orbis_log("vm below stacks: %u mapping(s) in the 256 MiB under %p, the nearest %u follow",
              d.seen,(void*)d.top,kKeep);
  for(unsigned i=(d.seen>kKeep ? d.seen-kKeep : 0); i<d.seen; ++i) {
    const VqInfo&   q = d.ring[i%kKeep];
    const uintptr_t b = reinterpret_cast<uintptr_t>(q.start);
    const uintptr_t e = reinterpret_cast<uintptr_t>(q.end);
    orbis_log("vm below stacks: [%p,%p) %llu KiB prot 0x%x type %d flags 0x%02x '%.32s' - "
              "%llu KiB free between its end and %p",
              q.start,q.end,(unsigned long long)((e-b)/1024),unsigned(q.prot),int(q.mtype),
              unsigned(q.flags),q.name,(unsigned long long)((d.top>e ? d.top-e : 0)/1024),
              (void*)d.top);
    }
  }

// orbis::vmReport's body. `force` is this file's own failure path; other calls are throttled after the
// 12th (only every 5th prints) because RPCS3 calls it every 60 s and its sink keeps only 256
// orbis-compat lines per session, which a long run would otherwise use up before any failure. The
// number after `where` counts the calls, so gaps show the throttle.
std::atomic<unsigned long> g_vmCalls{0};

void vmReportNow(const char* where, bool force) {
  if(orbis_log_enabled()==0)
    return;
  if(where==nullptr)
    where = "?";
  const unsigned long call = g_vmCalls.fetch_add(1,std::memory_order_relaxed)+1;
  if(!force && call>12 && call%5!=0)
    return;

  VmWalk w;
  walkVm(w);

  // FreeBSD's RLIMIT_VMEM (10), which every mapping - reservations included - counts against (h).
  unsigned long long vmem[2] = {0,0};
  char               vmemText[32];
  if(&orbisThrGetrlimit==nullptr || orbisThrGetrlimit(10,vmem)!=0)
    snprintf(vmemText,sizeof(vmemText),"unreadable");
  else if(vmem[0]>=0x7fffffffffffffffull)
    snprintf(vmemText,sizeof(vmemText),"unlimited");
  else
    snprintf(vmemText,sizeof(vmemText),"%llu MiB",vmem[0]>>20);

  if(!w.ran)
    orbis_log("vm [%s #%lu]: sceKernelVirtualQuery is unavailable - no map walk",where,call);
  else if(w.entries==0)
    orbis_log("vm [%s #%lu]: map walk REFUSED at its first query, rc 0x%08x%s - "
              "SCE_KERNEL_VQ_FIND_NEXT=%d is assumed and this firmware does not take it; no map numbers",
              where,call,unsigned(w.endRc),w.stalled ? " (no progress)" : "",int(kVqFindNext));
  else
    orbis_log("vm [%s #%lu]: %lu entries, %llu MiB mapped (RLIMIT_VMEM %s), %llu MiB committed (%llu "
              "flexible, %llu direct); %lu stack-flagged entries, %llu MiB in [%p,%p); below "
              "0x1000000000: %lu entries, %llu MiB, largest gap %llu MiB at %p; walk %llu us, end rc "
              "0x%08x%s%s",
              where,call,w.entries,w.bytes>>20,vmemText,w.committed>>20,w.flexible>>20,w.direct>>20,
              w.stacks,w.stackBytes>>20,
              (void*)(w.lowestStack==~uintptr_t(0) ? 0 : w.lowestStack),(void*)w.highestStack,
              w.autoEntries,w.autoBytes>>20,w.autoGap>>20,(void*)w.autoGapAt,
              w.us,unsigned(w.endRc),w.truncated ? " TRUNCATED at 32768 entries" : "",
              w.stalled ? " (stopped: no progress)" : "");

  const uintptr_t lo = g_kstackLow.load(std::memory_order_relaxed);
  const uintptr_t hi = g_kstackHigh.load(std::memory_order_relaxed);
  orbis_log("vm [%s #%lu] threads: ~%lu live (%lu ok, %lu returned, %lu exit notes, %lu join notes, "
            "%lu without trampoline), %lu create failures; libkernel stacks [%p,%p) %llu MiB, %lu lows, "
            "guard %llu KiB; %lu caller-stack and %lu explicit-size creates passed through; flexible "
            "free %llu KiB, internal free %llu B",
            where,call,liveClamped(),g_createdOk.load(std::memory_order_relaxed),
            g_finished.load(std::memory_order_relaxed),g_exited.load(std::memory_order_relaxed),
            g_joined.load(std::memory_order_relaxed),g_untracked.load(std::memory_order_relaxed),
            g_createFailed.load(std::memory_order_relaxed),
            (void*)(lo==~uintptr_t(0) ? 0 : lo),(void*)hi,
            (unsigned long long)((lo!=~uintptr_t(0) && hi>lo) ? (hi-lo)>>20 : 0),
            g_kstackLows.load(std::memory_order_relaxed),
            (unsigned long long)(g_kstackGuard.load(std::memory_order_relaxed)/1024),
            g_callerStacks.load(std::memory_order_relaxed),
            g_explicitSizes.load(std::memory_order_relaxed),
            flexibleFreeNow()/1024,internalFreeNow());
  }

// The diagnosis itself. Runs once, in the thread whose create just failed, for tens of milliseconds.
// The map-level probes (1-3) come before any thread is created here: libkernel caches a joined
// thread's stack, which could otherwise occupy the slot probe 1 checks.
void diagnoseCreateFailure(int failedRc, unsigned long failureNo) {
  const unsigned long long t0 = nowUs();
  const size_t floorNow = resolveFloor();
  const size_t stack    = (floorNow!=0) ? floorNow : size_t(0x200000);
  const size_t guardNow = g_kstackGuard.load(std::memory_order_relaxed);
  const size_t guard    = (guardNow!=0) ? guardNow : size_t(0x4000);   // assumed: one 16 KiB page
  const long long hitsBoot = g_hitsBoot.load(std::memory_order_relaxed);

  int he0 = 0;
  const long long hits0 = sysctlValue("kern.threads.max_threads_hits",&he0);

  // 1. The slot libkernel's next stack takes. FreeBSD's thr_stack.c maps [stack + guard] directly under
  //    the previous one (assumed for this libkernel; 0.28 shows them descending from 0x7eedf8000). The
  //    lowest stack seen by the trampoline starts its mapping one guard below its base; a stack-flagged
  //    VA entry lower than that (a Sony library's thread, created without this interposer) moves the
  //    slot down to it. Reserved FIXED|NO_OVERWRITE, which can never replace a mapping, then unmapped.
  VmWalk pre;
  walkVm(pre);
  const uintptr_t lo      = g_kstackLow.load(std::memory_order_relaxed);
  uintptr_t       slotTop = (lo!=~uintptr_t(0) && lo>guard) ? lo-guard : 0;
  const char*     slotWhy = (slotTop!=0) ? "lowest interposer stack minus its guard" : "no stack seen";
  if(pre.lowestStack!=~uintptr_t(0) && pre.lowestStack<kAutoAreaEnd &&
     (slotTop==0 || (pre.lowestStack<slotTop && slotTop-pre.lowestStack<=0x40000000ull))) {
    slotTop = pre.lowestStack;
    slotWhy = "lowest stack-flagged VA entry";
    }
  // 0.29: rounded up to the 16 KiB the reserve call takes; a 4 KiB guard would otherwise make the
  // length invalid and the refusal look like an occupied slot.
  const size_t slotLen  = (stack+guard+0x3fff) & ~size_t(0x3fff);
  void*        slotWant = nullptr;
  void*        slot     = nullptr;
  int32_t      rcSlot   = 0;
  const bool   slotRan  = slotTop>slotLen && &orbisThrReserve!=nullptr && &orbisThrMunmap!=nullptr;
  if(slotRan) {
    slotWant = reinterpret_cast<void*>((slotTop-slotLen) & ~uintptr_t(0x3fff));
    slot     = slotWant;
    rcSlot   = orbisThrReserve(&slot,slotLen,kMapFixed|kMapNoOverwrite,0x4000);
    if(rcSlot==0 && slot!=nullptr)
      orbisThrMunmap(slot,slotLen);
    }
  BelowDump below;
  collectBelow(below,slotTop);

  // 2. A hint-less 2 MiB VA reservation: the kernel's automatic area (f), or the whole map against
  //    RLIMIT_VMEM (h). Reserving charges no memory.
  void*      va    = nullptr;
  const bool vaRan = &orbisThrReserve!=nullptr && &orbisThrMunmap!=nullptr;
  const int  rcVa  = vaRan ? orbisThrReserve(&va,0x200000,0,0x4000) : 0;
  if(vaRan && rcVa==0 && va!=nullptr)
    orbisThrMunmap(va,0x200000);

  // 3. 2 MiB of flexible memory, the pool every libkernel stack is charged to (b). Unmapped at once.
  void*      fx      = nullptr;
  const bool flexRan = &orbisThrMapFlexible!=nullptr && &orbisThrMunmap!=nullptr;
  const int  rcFlex  = flexRan ? orbisThrMapFlexible(&fx,0x200000,0x3,0,"orbis-diag") : 0;
  if(flexRan && rcFlex==0 && fx!=nullptr)
    orbisThrMunmap(fx,0x200000);

  // 4. A thread that needs no stack from libkernel: 256 KiB of this process's own heap (direct memory
  //    through orbis_mmap.cpp's carve-outs). If this is refused too, the thread itself is refused
  //    ((c) a cap, or (e) its TCB/TLS) and no stack scheme can help.
  constexpr size_t kCallerStack = 256*1024;
  void*      callerBuf    = nullptr;
  const bool callerRan    = posix_memalign(&callerBuf,0x4000,kCallerStack)==0 && callerBuf!=nullptr;
  bool        callerJoined = false;
  const char* callerStage  = "";
  const int   rcCaller     = callerRan ? diagThread(callerBuf,kCallerStack,&callerJoined,&callerStage) : 0;
  // Freed only when nothing can still run on it; if the thread started but was not joined, the
  // buffer is leaked rather than freed under it.
  if(callerBuf!=nullptr && (rcCaller!=0 || callerJoined))
    free(callerBuf);

  // 5. A thread on a 64 KiB libkernel stack - the platform's own default size.
  const char* smallStage = "";
  const int   rcSmall    = diagThread(nullptr,64*1024,nullptr,&smallStage);

  // 6. The failing shape itself, once more: a libkernel stack of the floor's size. If it now succeeds,
  //    its stack is cached by libkernel and the caller's own retry reuses it.
  const char* sameStage = "";
  const int   rcSame    = diagThread(nullptr,stack,nullptr,&sameStage);

  int he1 = 0;
  const long long hits1 = sysctlValue("kern.threads.max_threads_hits",&he1);

  char sCaller[16], sSlot[16], sVa[16], sFlex[16];
  rcText(sCaller,sizeof(sCaller),callerRan,rcCaller);
  rcText(sSlot,  sizeof(sSlot),  slotRan,  rcSlot);
  rcText(sVa,    sizeof(sVa),    vaRan,    rcVa);
  rcText(sFlex,  sizeof(sFlex),  flexRan,  rcFlex);

  // Three lines, each under the 512 characters RPCS3's sink keeps.
  orbis_log("thread create DIAGNOSIS (failure #%lu rc 0x%08x, ~%lu live, %lu ok, internal free %llu B, "
            "flexible free %llu KiB, took %llu us): next stack slot [%p,+%llu KiB) FIXED reserve %s%s; "
            "hint-less 2 MiB VA reserve %s at %p; 2 MiB flexible map %s at %p",
            failureNo,unsigned(failedRc),liveClamped(),g_createdOk.load(std::memory_order_relaxed),
            internalFreeNow(),flexibleFreeNow()/1024,nowUs()-t0,
            slotWant,(unsigned long long)(slotLen/1024),sSlot,
            (slotRan && rcSlot==0 && slot!=slotWant) ? " (LANDED ELSEWHERE)" : "",
            sVa,va,sFlex,fx);
  orbis_log("thread create DIAGNOSIS: thread on a caller-supplied %llu KiB heap stack %s%s; on a 64 KiB "
            "libkernel stack 0x%08x%s; repeat of the failing %llu KiB libkernel-stack create 0x%08x%s; "
            "kern.threads.max_threads_hits %lld -> %lld (boot %lld, err %d/%d)",
            (unsigned long long)(kCallerStack/1024),sCaller,callerStage,unsigned(rcSmall),smallStage,
            (unsigned long long)(stack/1024),unsigned(rcSame),sameStage,
            hits0,hits1,hitsBoot,he0,he1);
  orbis_log("thread create DIAGNOSIS: interposer-seen libkernel stacks [%p,%p), %lu lows, guard %llu "
            "KiB%s; %lu stack-flagged VA entries, lowest %p; slot below %p (%s); pre-walk %lu entries, "
            "end rc 0x%08x",
            (void*)(lo==~uintptr_t(0) ? 0 : lo),(void*)g_kstackHigh.load(std::memory_order_relaxed),
            g_kstackLows.load(std::memory_order_relaxed),(unsigned long long)(guard/1024),
            guardNow!=0 ? "" : " ASSUMED (never observed)",
            pre.stacks,(void*)(pre.lowestStack==~uintptr_t(0) ? 0 : pre.lowestStack),
            (void*)slotTop,slotWhy,pre.entries,unsigned(pre.endRc));

  // max_threads_hits is a kernel-wide counter. 0.29: only a move during this diagnosis (while its own
  // probe creates ran) is taken as the cap; a move since boot could be another process's, so it is
  // only printed as a caveat.
  const bool hitsMovedNow  = he0==0 && he1==0 && hits1>hits0;
  const bool hitsSinceBoot = he0==0 && hitsBoot>=0 && hits0>hitsBoot;
  // A FIXED reserve refused as EINVAL (SCE 0x80020016) means the probe itself was malformed for this
  // kernel, not that the slot is occupied.
  const bool slotOccupied  = slotRan && rcSlot!=0 && unsigned(rcSlot)!=0x80020016u;
  // 0.29: only an EAGAIN-class refusal of the caller-stack create means the thread is refused.
  // Whether libkernel accepts a heap stack at all is unverified on this console, and any other code
  // (EINVAL, EFAULT ...) concerns that stack, not a thread cap (libthr maps EPROCLIM, a TCB and a
  // stack shortage all to EAGAIN).
  const bool callerAtCreate = callerRan && rcCaller!=0 && strcmp(callerStage," at create")==0;
  const bool callerCapped   = callerAtCreate && isEagain(rcCaller);
  const bool callerOk       = callerRan && rcCaller==0;
  const char* verdict =
      hitsMovedNow
        ? "(c) kern.threads.max_threads_hits moved while this diagnosis created threads: the kernel's "
          "PER-PROCESS THREAD CAP refuses them. Own stacks cannot fix this; fewer live threads can."
    : callerCapped
        ? "(c/e) the THREAD itself is refused (EAGAIN) even on a caller-supplied stack (no libkernel "
          "stack, no flexible memory): a per-process thread limit or its TCB/TLS allocation. Own stacks "
          "cannot fix this; fewer live threads can."
    : (vaRan && rcVa!=0)
        ? "(f/h) even a hint-less 2 MiB VA reservation is refused: the kernel's automatic VA area is "
          "full (f) or the map reached RLIMIT_VMEM (h) - compare 'mapped' with RLIMIT_VMEM below."
    : (flexRan && rcFlex!=0)
        ? "(b) flexible memory refuses 2 MiB although some is reported free: a hidden reserve or "
          "threshold. RPCS3-owned direct-memory stacks avoid it."
    : slotOccupied
        ? "(a) the next descending stack slot is OCCUPIED - see 'vm below stacks'. RPCS3-owned stacks "
          "in a private VA window avoid it."
    : (rcSmall!=0)
        ? "libkernel refuses even a 64 KiB stack while the slot, VA and flexible probes pass: its "
          "stack allocator is at a limit of its own. RPCS3-owned stacks avoid it if the caller-stack "
          "thread above was created."
    : (rcSame==0)
        ? "TRANSIENT: the identical create succeeded during this diagnosis (its stack is now cached for "
          "the caller's retry). If 'thread FAILURE' lines continue, it is a size/placement effect."
    :     "only libkernel's own stack of this size is refused while a 64 KiB one and the slot, VA and "
          "flexible probes pass: a size-specific limit on libkernel stacks ((a) placement further "
          "down, or (b) a stack-only threshold). RPCS3-owned stacks avoid it if the caller-stack "
          "thread above was created.";
  orbis_log("thread create VERDICT: %s",verdict);
  // 0.29: also when the create itself refused the heap stack with a code other than EAGAIN.
  if(!callerOk && !callerCapped)
    orbis_log("thread create VERDICT caveat: the caller-supplied-stack thread was not created (%s%s, rc "
              "0x%08x)%s - a per-process thread cap (c) is NOT excluded by this run, and that RPCS3-owned "
              "stacks would be accepted is NOT shown",
              callerRan ? "refused" : "no heap buffer for it",callerRan ? callerStage : "",
              unsigned(rcCaller),
              callerAtCreate ? ": not EAGAIN, so libkernel turned down the heap STACK, not the thread" : "");
  if(hitsSinceBoot && !hitsMovedNow)
    orbis_log("thread create VERDICT caveat: kern.threads.max_threads_hits rose since boot (%lld -> %lld) "
              "but not during the diagnosis: the refused create may have hit the per-process thread cap "
              "(c) at a moment with more live threads - or another process did (the counter is "
              "kernel-wide)",hitsBoot,hits0);
  if(slotRan && rcSlot!=0 && !slotOccupied)
    orbis_log("thread create VERDICT caveat: the next-slot reserve was refused as EINVAL - the probe, "
              "not the slot; cause (a) is untested by this run");

  printBelow(below);
  vmReportNow("create failure",true);
  if(g_limitsReported.load(std::memory_order_relaxed)==0)
    orbis::limitsReport();
  }

// scePthreadCreate, plus freeing the trampoline payload when the thread never starts.
int createThread(pthread_t* thread, const pthread_attr_t* attr,
                 void* (*entry)(void*), void* entryArg, StartArg* sa) {
  // 0.29: counted before the call and taken back on failure, so a thread that exits before this
  // returns cannot push the live count below zero.
  g_createdOk.fetch_add(1,std::memory_order_relaxed);
  const int rc = scePthreadCreate(thread,attr,entry,entryArg,"orbis");
  if(rc==0) {
    // Without the trampoline nothing will ever count this thread's return.
    if(sa==nullptr)
      g_untracked.fetch_add(1,std::memory_order_relaxed);
    return 0;
    }
  g_createdOk.fetch_sub(1,std::memory_order_relaxed);

  // The payload the thread would have freed; freed regardless of logging.
  if(sa!=nullptr)
    free(sa);
  const unsigned long n = g_createFailed.fetch_add(1,std::memory_order_relaxed)+1;
  reportPoolFailure("scePthreadCreate",rc,n);

  // 0.29: the one-shot diagnosis on the first failure (and on the first EAGAIN if the first failure
  // was something else), then a map walk at powers of four (two lines each, within the 256 lines a
  // session keeps). Skipped while threadLimitProbe() deliberately holds the process at its limit.
  if(orbis_log_enabled()!=0) {
    if(g_limitProbeActive.load(std::memory_order_relaxed)!=0) {
      if(powerOfTwo(n))
        orbis_log("thread create: failure #%lu happened while the thread limit probe holds its "
                  "threads - not diagnosed",n);
      return rc;
      }
    const bool first       = g_diagAnyDone.exchange(1,std::memory_order_acq_rel)==0;
    const bool firstEagain = isEagain(rc) && g_diagEagainDone.exchange(1,std::memory_order_acq_rel)==0;
    if(first || firstEagain)
      diagnoseCreateFailure(rc,n);
    else if(powerOfTwo(n) && (n & 0x5555555555555555ul)!=0)
      vmReportNow("create failure",true);
    }
  return rc;
  }

// 0.29: -1 until checked; 1 when a fresh attr reports no stack address (FreeBSD's default, assumed for
// this libkernel); 0 when it reports one, in which case a caller's stack cannot be told from a fresh
// attr and the caller-stack test below stays off rather than let every default attr skip the floor.
std::atomic<int> g_freshAttrClean{-1};

bool freshAttrHasNoStack() {
  const int known = g_freshAttrClean.load(std::memory_order_relaxed);
  if(known>=0)
    return known==1;
  pthread_attr_t a;
  if(pthread_attr_init(&a)!=0)
    return false;   // unknown this time: no pass-through, decided again on the next call
  void*  base = nullptr;
  size_t size = 0;
  pthread_attr_getstack(&a,&base,&size);
  pthread_attr_destroy(&a);
  const int v = (base==nullptr) ? 1 : 0;
  if(g_freshAttrClean.exchange(v,std::memory_order_relaxed)<0 && v==0 && orbis_log_enabled()!=0)
    orbis_log("thread create: a FRESH attr reports stack address %p - caller-supplied stacks cannot be "
              "recognised, so they are raised like any other attr",base);
  return v==1;
  }

// 0.29: counts a caller attr passed through unchanged and logs the first one, showing whether
// RPCS3's own stacks (or explicit sizes) reached libkernel as asked.
void noteCallerAttr(const pthread_attr_t* attr, bool ownStack, size_t want) {
  if(ownStack) {
    const unsigned long k = g_callerStacks.fetch_add(1,std::memory_order_relaxed)+1;
    if(k==1 && orbis_log_enabled()!=0) {
      void*  base = nullptr;
      size_t size = 0;
      pthread_attr_getstack(attr,&base,&size);
      orbis_log("thread create: first caller-supplied stack passed through unchanged - [%p,+%llu KiB)",
                base,(unsigned long long)(size/1024));
      }
    return;
    }
  const unsigned long k = g_explicitSizes.fetch_add(1,std::memory_order_relaxed)+1;
  if(k==1 && orbis_log_enabled()!=0)
    orbis_log("thread create: first explicit %llu KiB stack size honoured unraised",
              (unsigned long long)(want/1024));
  }

}

namespace {

size_t mainThreadStack() {
  int          rc = 0;
  const size_t sz = stackOfSelf(&rc);
  return (rc==0) ? sz : 0;
  }

}

size_t orbis::threadDefaultStack() {
  return g_defaultStack;
  }

size_t orbis::threadStackFloor() {
  return resolveFloor();
  }

void orbis::threadFailures(unsigned long* createFailed, unsigned long* attrFailed) {
  if(createFailed!=nullptr) *createFailed = g_createFailed.load(std::memory_order_relaxed);
  if(attrFailed  !=nullptr) *attrFailed   = g_attrFailed.load(std::memory_order_relaxed);
  }

void orbis::threadCounts(unsigned long* created, unsigned long* raised) {
  if(created!=nullptr) *created = g_created.load(std::memory_order_relaxed);
  if(raised !=nullptr) *raised  = g_raised.load(std::memory_order_relaxed);
  }

void orbis::threadAltStacks(unsigned long* installed, unsigned long* failed,
                            unsigned long* skipped) {
  if(installed!=nullptr) *installed = g_altOk.load(std::memory_order_relaxed);
  if(failed   !=nullptr) *failed    = g_altFailed.load(std::memory_order_relaxed);
  if(skipped  !=nullptr) *skipped   = g_altSkipped.load(std::memory_order_relaxed);
  }

unsigned long orbis::threadsFinished() {
  return g_finished.load(std::memory_order_relaxed);
  }

size_t orbis::threadAltStackSize() {
  return kAltStackSize;
  }

// ------------------------------------------------------------------ the interposer
//
// This definition wins over libkernel's: a symbol defined in an object file is preferred to one from
// a shared library (verified: calls land here, not on libkernel.so:0xda78).
//
// It must be pulled in with --whole-archive: callers resolve pthread_create from libkernel anyway,
// so nothing references this archive member and the linker would drop it.
extern "C" int pthread_create(pthread_t* thread, const pthread_attr_t* attr,
                              void* (*start)(void*), void* arg) {
  g_created.fetch_add(1,std::memory_order_relaxed);

  // 0.29: the boot limits report, once, on the first create that has a logger to print to (see
  // ORBIS_THREAD_LIMITS_AT_FIRST_CREATE). One relaxed load per create after that.
  if(kLimitsAtFirstCreate && g_limitsReported.load(std::memory_order_relaxed)==0 && orbis_log_enabled()!=0)
    orbis::limitsReport();

  // The trampoline is installed on every path below, including those that raise nothing: the stack
  // floor and the alternate signal stack are independent (a caller with its own 8 MB stack still
  // wants overflows reported). Only a stack too small to spare 64 KiB is skipped, decided inside the
  // trampoline where the real size is known.
  //
  // The payload (two pointers) is malloc'd and freed by the thread itself, or by createThread() when
  // the thread never starts. If malloc fails, the caller's start routine is used unchanged.
  StartArg* sa                = static_cast<StartArg*>(malloc(sizeof(StartArg)));
  void*   (*entry)(void*)     = start;
  void*     entryArg          = arg;
  if(sa!=nullptr) {
    sa->fn          = start;
    sa->arg         = arg;
    sa->callerStack = 0;
    entry           = &threadTrampoline;
    entryArg        = sa;
    }

  // 0.29: does the caller's attr carry its own stack (pthread_attr_setstack)? Checked first because
  // every path below that passes `attr` through runs the thread on that stack, and the trampoline must
  // not record it as libkernel's. Only the raise path drops it.
  //
  // 0.29: the fresh-attr check is done at the first create, while libkernel is healthy. Done lazily,
  // it ran during RPCS3's own-stack fallback right after a refusal, when pthread_attr_init can fail
  // too, and the fallback stack was then recorded as libkernel's.
  if(kPassCallerStack && g_freshAttrClean.load(std::memory_order_relaxed)<0)
    freshAttrHasNoStack();
  bool ownStack = false;
  if(attr!=nullptr) {
    void*  ownBase = nullptr;
    size_t ownSize = 0;
    ownStack = pthread_attr_getstack(attr,&ownBase,&ownSize)==0 && ownBase!=nullptr &&
               freshAttrHasNoStack();
    }
  if(ownStack && sa!=nullptr)
    sa->callerStack = 1;

  const size_t floor = resolveFloor();
  if(floor==0)
    return createThread(thread,attr,entry,entryArg,sa);

  // 0.29: a caller that brought its own stack keeps it. The raise below rebuilds the attr from the
  // detach state and guard size only, so a caller stack smaller than the floor used to be dropped and
  // libkernel allocated a second one. RPCS3's thread_base gives a thread its own 2 MiB direct-memory
  // stack when libkernel refuses one, which only helps if the thread actually gets that stack.
  if(ownStack && kPassCallerStack) {
    noteCallerAttr(attr,true,0);
    return createThread(thread,attr,entry,entryArg,sa);
    }

  // What did the caller ask for? A fresh attr reports the platform default (65536, measured), so
  // "asked for the default" and "asked for nothing" look the same; both are raised (see the header).
  size_t want = 0;
  if(attr!=nullptr && pthread_attr_getstacksize(attr,&want)!=0)
    want = 0;

  if(want>=floor) {
    if(ownStack)
      noteCallerAttr(attr,true,want);
    return createThread(thread,attr,entry,entryArg,sa);
    }

  // 0.29: an explicit size of kExplicitStackMin or more is honoured. A fresh attr reports 65536, so a
  // request this large was set on purpose (RPCS3: a deliberately sized service thread).
  if(kExplicitStackMin!=0 && want>=kExplicitStackMin) {
    noteCallerAttr(attr,false,want);
    return createThread(thread,attr,entry,entryArg,sa);
    }

  // Raise it, on a local copy: the caller's attr is const and may be reused for other threads.
  pthread_attr_t raised;
  const int rcAttr = pthread_attr_init(&raised);
  if(rcAttr!=0) {
    reportPoolFailure("pthread_attr_init",rcAttr,
                      g_attrFailed.fetch_add(1,std::memory_order_relaxed)+1);
    return createThread(thread,attr,entry,entryArg,sa);
    }

  if(attr!=nullptr) {
    // Carry over what the caller chose; anything else stays at the platform default.
    int    detach = 0;
    size_t guard  = 0;
    if(pthread_attr_getdetachstate(attr,&detach)==0) pthread_attr_setdetachstate(&raised,detach);
    if(pthread_attr_getguardsize(attr,&guard)==0)    pthread_attr_setguardsize(&raised,guard);
    }

  const int rcSet = pthread_attr_setstacksize(&raised,floor);
  // 0.29: the rebuilt attr carries no stack, so libkernel allocates one. Only the fallback that
  // passes the caller's attr keeps the flag.
  if(rcSet==0 && sa!=nullptr)
    sa->callerStack = 0;
  const int rc    = (rcSet==0) ? createThread(thread,&raised,entry,entryArg,sa)
                               : createThread(thread,attr,entry,entryArg,sa);
  if(rcSet==0 && rc==0)
    g_raised.fetch_add(1,std::memory_order_relaxed);

  pthread_attr_destroy(&raised);

  // Consumers do not call the census after boot, so the interposer reports itself at powers of two
  // (1, 2, 4 ... 64).
  const unsigned long n = g_created.load(std::memory_order_relaxed);
  if(rc==0 && (n & (n-1))==0)
    orbis::memCensusThreads("live");

  return rc;
  }

void orbis::threadStackProbe() {
  if(!orbis_log_enabled())
    return;

  // 1. What a fresh attr reports. If it equals the size threads really get, "chose the default" and
  //    "chose nothing" look identical to an interposer.
  pthread_attr_t fresh;
  size_t         claimed = 0;
  int            rcFresh = pthread_attr_init(&fresh);
  int            rcClaim = (rcFresh==0) ? pthread_attr_getstacksize(&fresh,&claimed) : rcFresh;

  // 2. What the calling (main, loader-created) thread actually has.
  int          rcSelf = 0;
  const size_t self   = stackOfSelf(&rcSelf);

  // 3. What a thread created with no attributes gets: the default.
  int          rcNull = 0;
  const size_t viaNull = stackOfNewThread(nullptr,&rcNull);

  // 4. What a thread created with a default-initialised attr gets (dEQP's case). If 3 and 4 differ,
  //    the interposer has two cases to handle.
  int          rcAttr = 0;
  const size_t viaAttr = (rcFresh==0) ? stackOfNewThread(&fresh,&rcAttr) : 0;

  if(rcFresh==0)
    pthread_attr_destroy(&fresh);

  g_defaultStack = viaNull;

  orbis_log("thread probe: fresh attr claims %llu B (rc %d/%d), main thread has %llu B (rc %d)",
            (unsigned long long)claimed,rcFresh,rcClaim,
            (unsigned long long)self,rcSelf);
  orbis_log("thread probe: attr=NULL gives %llu B (rc %d), default-init attr gives %llu B (rc %d), "
            "PTHREAD_STACK_MIN %d",
            (unsigned long long)viaNull,rcNull,
            (unsigned long long)viaAttr,rcAttr,
            (int)PTHREAD_STACK_MIN);

  // With the interposer active, 3 and 4 measure the policy, not the platform (the probe's threads go
  // through pthread_create too), so they should report the floor. The platform's raw 65536 is
  // recorded in the header.
  const size_t floor = resolveFloor();
  orbis_log("thread probe: floor is %llu KiB (%s)",
            (unsigned long long)(floor/1024),
            (floor==0) ? "probe failed" : "the main thread's own size");

  // The verdict, so the log states the conclusion as well as the numbers.
  if(viaNull!=0 && viaNull<256*1024)
    orbis_log("thread probe: VERDICT threads get %llu KiB - a pipeline compile wants ~72 KB of "
              "frame, so every thread created without an explicit size is one deep call from the "
              "guard page. THE INTERPOSER IS NOT DOING ITS JOB.",
              (unsigned long long)(viaNull/1024));
  else if(viaNull!=0)
    orbis_log("thread probe: VERDICT threads get %llu KiB - the platform's own 64 KiB has been "
              "raised, and a shader compile fits",(unsigned long long)(viaNull/1024));
  else
    orbis_log("thread probe: VERDICT INCONCLUSIVE - the probe could not read a thread's stack "
              "(rc %d); pthread_attr_get_np may not do what its export suggests",rcNull);

  // 0.29: the process limits are reported here too, since this runs at boot.
  orbis::limitsReport();
  }

// ------------------------------------------------------------------ 0.29: live count, VA map, limits

// Call only from a thread this interposer's pthread_create started, once, right before it leaves
// through pthread_exit. Not deduplicated: a call from any other thread (main, or one made with
// scePthreadCreate directly) makes the live count read one low. One relaxed increment.
void orbis::threadNoteExit() noexcept {
  g_exited.fetch_add(1,std::memory_order_relaxed);
  }

// 0.29: called after a successful pthread_join of a thread this interposer started
// (RPCS3: ~thread_base). Not deduplicated either; one relaxed increment.
void orbis::threadNoteJoin() noexcept {
  g_joined.fetch_add(1,std::memory_order_relaxed);
  }

unsigned long orbis::threadsLive() noexcept {
  return liveClamped();
  }

// 0.29: the counters as one struct, for RPCS3's 60 s line and fatal.txt. Relaxed loads only (no lock,
// no libkernel call), so it is safe on a fatal path; fields may be off by threads starting or stopping
// during the read.
static_assert(sizeof(orbis::ThreadCensus)==7*sizeof(unsigned long)+2*sizeof(unsigned long long),
              "orbis::ThreadCensus is redeclared by its consumers (RPCS3 ps4_main/ps4_fatal): keep its "
              "layout fixed");
void orbis::threadCensus(ThreadCensus* out) noexcept {
  if(out==nullptr)
    return;
  const uintptr_t lo = g_kstackLow.load(std::memory_order_relaxed);
  out->created_ok    = g_createdOk.load(std::memory_order_relaxed);
  out->failed        = g_createFailed.load(std::memory_order_relaxed);
  out->attr_failed   = g_attrFailed.load(std::memory_order_relaxed);
  out->finished      = g_finished.load(std::memory_order_relaxed);
  out->exited        = g_exited.load(std::memory_order_relaxed);
  out->joined        = g_joined.load(std::memory_order_relaxed);
  out->live_estimate = liveClamped();
  out->stack_low     = (lo==~uintptr_t(0)) ? 0ull : (unsigned long long)lo;
  out->stack_high    = (unsigned long long)g_kstackHigh.load(std::memory_order_relaxed);
  }

void orbis::vmReport(const char* where) noexcept {
  vmReportNow(where,false);
  }

namespace {

// "inf" for FreeBSD's RLIM_INFINITY, whole MiB as "<n>M", anything else in bytes.
void limitText(char* out, size_t cap, unsigned long long v) {
  if(v>=0x7fffffffffffffffull)
    snprintf(out,cap,"inf");
  else if(v!=0 && (v & 0xfffffull)==0)
    snprintf(out,cap,"%lluM",v>>20);
  else
    snprintf(out,cap,"%llu",v);
  }

// Appends one space-separated item; a line that would overflow is printed under `head` first.
void appendItem(char* line, size_t cap, size_t& at, const char* head, const char* item) {
  const size_t need = strlen(item)+(at!=0 ? 1 : 0);
  if(at!=0 && at+need>=cap) {
    orbis_log("%s %s",head,line);
    at      = 0;
    line[0] = '\0';
    }
  const int n = snprintf(line+at,cap-at,"%s%s",at!=0 ? " " : "",item);
  if(n>0)
    at += (size_t(n)<cap-at) ? size_t(n) : cap-at-1;
  }

}

void orbis::limitsReport() noexcept {
  if(orbis_log_enabled()==0)
    return;
  // 0.29: the full report once per process, for whichever caller comes first (first create,
  // threadStackProbe(), the frontend or the diagnosis). Later calls print one short line with the
  // counter that can still change.
  if(g_limitsReported.exchange(1,std::memory_order_acq_rel)!=0) {
    int             err  = 0;
    const long long hits = sysctlValue("kern.threads.max_threads_hits",&err);
    orbis_log("limits: reported earlier; kern.threads.max_threads_hits now %lld (boot %lld, err %d)",
              hits,g_hitsBoot.load(std::memory_order_relaxed),err);
    return;
    }

  // 400, not 512: the head is printed in front of it and RPCS3's sink cuts a line at 512.
  char   line[400];
  size_t at = 0;
  line[0] = '\0';

  // FreeBSD 9 numbering, not the SDK's musl <sys/resource.h> (whose RLIMIT_AS is 9 where FreeBSD's
  // VMEM/AS is 10): getrlimit here is libkernel's.
  static const char* const kRlim[13] = {"cpu","fsize","data","stack","core","rss","memlock",
                                        "nproc","nofile","sbsize","vmem","npts","swap"};
  const char* const rlimHead = "limits: rlimit cur/max (FreeBSD numbering):";
  if(&orbisThrGetrlimit==nullptr)
    appendItem(line,sizeof(line),at,rlimHead,"getrlimit unavailable");
  else
    for(int r=0; r<13; ++r) {
      unsigned long long lim[2] = {0,0};
      char               item[80];
      errno = 0;
      if(orbisThrGetrlimit(r,lim)!=0)
        snprintf(item,sizeof(item),"%s=err%d",kRlim[r],errno);
      else {
        char cur[24], max[24];
        limitText(cur,sizeof(cur),lim[0]);
        limitText(max,sizeof(max),lim[1]);
        snprintf(item,sizeof(item),"%s=%s/%s",kRlim[r],cur,max);
        }
      appendItem(line,sizeof(line),at,rlimHead,item);
      }
  orbis_log("%s %s",rlimHead,line);

  // Integer sysctls through libkernel's sysctlbyname; a sandbox refusal prints as errN.
  static const char* const kSys[] = {"kern.threads.max_threads_per_proc",
                                     "kern.threads.max_threads_hits","vm.max_proc_mmap",
                                     "kern.usrstack","kern.maxssiz","kern.sgrowsiz","kern.maxdsiz",
                                     "vm.kmem_size","hw.ncpu","hw.pagesize"};
  const char* const sysHead = "limits: sysctl";
  at      = 0;
  line[0] = '\0';
  for(const char* name : kSys) {
    int             err = 0;
    const long long v   = sysctlValue(name,&err);
    char            item[96];
    if(err!=0)
      snprintf(item,sizeof(item),"%s=err%d",name,err);
    else
      snprintf(item,sizeof(item),"%s=%lld(0x%llx)",name,v,(unsigned long long)v);
    appendItem(line,sizeof(line),at,sysHead,item);
    if(err==0 && strcmp(name,"kern.threads.max_threads_hits")==0) {
      long long unset = -1;
      g_hitsBoot.compare_exchange_strong(unset,v,std::memory_order_relaxed);
      }
    }
  orbis_log("%s %s",sysHead,line);
  }

namespace {

// 0.29: the probe's release flag is static, not in threadLimitProbe's frame: a thread whose join
// failed may still poll it after the probe has returned.
std::atomic<int> g_limitGo{0};

void* limitWait(void* p) {
  const std::atomic<int>* go = static_cast<const std::atomic<int>*>(p);
  while(go->load(std::memory_order_acquire)==0)
    orbisThrUsleep(20000);
  return nullptr;
  }

}

// Diagnostic experiment, run only when the caller opts in (RPCS3: thread-limit-probe.flag). Parks up
// to n threads at once on caller-supplied 64 KiB stacks cut from one heap block (no libkernel stack
// slot, no flexible memory), so the count reached is a limit on threads alone (cause (c)). Everything
// is released and joined before it returns. Meanwhile a create elsewhere may be refused; that is
// logged but not diagnosed.
void orbis::threadLimitProbe(unsigned n) noexcept {
  if(orbis_log_enabled()==0)
    return;
  if(n==0 || &orbisThrUsleep==nullptr) {
    orbis_log("thread limit probe: not run (%s)",n==0 ? "asked for 0 threads" : "sceKernelUsleep unavailable");
    return;
    }
  if(n>4096)
    n = 4096;

  constexpr size_t kStack = 64*1024;
  void*      blockRaw = nullptr;
  if(posix_memalign(&blockRaw,0x4000,kStack*size_t(n))!=0)
    blockRaw = nullptr;
  pthread_t* tids = static_cast<pthread_t*>(calloc(n,sizeof(pthread_t)));
  if(blockRaw==nullptr || tids==nullptr) {
    orbis_log("thread limit probe: no memory for %u stacks of %llu KiB - not run",
              n,(unsigned long long)(kStack/1024));
    free(blockRaw);
    free(tids);
    return;
    }
  char* const block = static_cast<char*>(blockRaw);

  g_limitGo.store(0,std::memory_order_release);
  const unsigned long live = liveClamped();
  int             he0   = 0;
  const long long hits0 = sysctlValue("kern.threads.max_threads_hits",&he0);

  g_limitProbeActive.store(1,std::memory_order_relaxed);
  const unsigned long long t0 = nowUs();
  unsigned    made  = 0;
  int         rc    = 0;
  const char* stage = "";
  for(; made<n; ++made) {
    pthread_attr_t a;
    if((rc = pthread_attr_init(&a))!=0) {
      stage = " at attr init";
      break;
      }
    rc = pthread_attr_setstack(&a,block+size_t(made)*kStack,kStack);
    if(rc!=0)
      stage = " at setstack";
    else {
      rc = scePthreadCreate(&tids[made],&a,limitWait,&g_limitGo,"orbis-limit");
      if(rc!=0)
        stage = " at create";
      }
    pthread_attr_destroy(&a);
    if(rc!=0)
      break;
    }
  const unsigned long long t1 = nowUs();
  int             he1   = 0;
  const long long hits1 = sysctlValue("kern.threads.max_threads_hits",&he1);

  orbis_log("thread limit probe: %u extra thread(s) alive at once on caller-supplied %llu KiB stacks "
            "(asked %u) in %llu us, stopped by rc 0x%08x%s; ~%lu interposer threads already live; "
            "max_threads_hits %lld -> %lld (err %d/%d); internal free %llu B - %s",
            made,(unsigned long long)(kStack/1024),n,t1-t0,unsigned(rc),stage,live,hits0,hits1,he0,he1,
            internalFreeNow(),
            made==n                ? "NO limit on threads below this: cause (c) is out"
            // 0.29: EAGAIN only. Another code at the create (notably EINVAL on the first create)
            // means libkernel refused the heap stack, which says nothing about a thread count.
            : (strcmp(stage," at create")==0 && isEagain(rc))
                                   ? "THIS IS A PER-PROCESS LIMIT ON THREADS (c): live + Sony's own + this count"
            : strcmp(stage," at create")==0
                                   ? "the create refused a caller-supplied heap stack with a code other than "
                                     "EAGAIN - a verdict on that stack, not a thread-count answer"
                                   : "stopped before the create itself - not a thread-count answer");

  g_limitGo.store(1,std::memory_order_release);
  unsigned joined = 0;
  for(unsigned i=0; i<made; ++i)
    if(pthread_join(tids[i],nullptr)==0)
      joined++;
  g_limitProbeActive.store(0,std::memory_order_relaxed);
  orbis_log("thread limit probe: released and joined %u of %u thread(s) in %llu us%s",joined,made,
            nowUs()-t1,joined==made ? "" : " - the stack block is LEAKED, a thread may still run on it");

  free(tids);
  if(joined==made)
    free(blockRaw);
  }
