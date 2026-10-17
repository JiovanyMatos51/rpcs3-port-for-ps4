// Copyright © 2026 Mikołaj Mikołajczyk
// SPDX-License-Identifier: MIT
#include "orbis_mmap.h"

#include <orbis_log.h>

#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>

// The three musl-internal names (libc.a's linkage between the allocator and the kernel; no
// header declares them). orbis_mmap.h explains why these are intercepted rather than `mmap`.
extern "C" void* __mmap(void* addr, size_t len, int prot, int flags, int fd, off_t off);
extern "C" int   __munmap(void* addr, size_t len);
extern "C" int   __madvise(void* addr, size_t len, int advice);

// The ledger's walk. Not imported by the executable before, so weak and null-checked: without the
// export the ledger is estimated. SceKernelDirectMemoryQueryInfo is {off_t start; off_t end; int
// memoryType;} (24 bytes); flag 1 (SCE_KERNEL_DMQ_FIND_NEXT) returns the range holding `offset` or
// the next one above it.
extern "C" int32_t ommDirectQuery(off_t offset, int32_t flags, void* info, size_t infoSize)
        __asm__("sceKernelDirectMemoryQuery") __attribute__((weak));
// The SDK declares the last two arguments by value (see orbis_mem.cpp); the out-pointer form.
extern "C" int32_t ommDirectAvailable(off_t start, off_t end, size_t align, off_t* physOut, size_t* sizeOut)
        __asm__("sceKernelAvailableDirectMemorySize");

// Optional knobs, see orbis_mmap.h "build knobs". Undefined ORBIS_MMAP_EXPAND_STEP_MAX_MIB leaves
// musl's own __expand_heap in the link; 0 for the floor means no floor.
#ifdef ORBIS_MMAP_EXPAND_STEP_MAX_MIB
#define ORBIS_MMAP_EXPAND_HEAP 1
#else
#define ORBIS_MMAP_EXPAND_HEAP 0
#endif
#ifndef ORBIS_MMAP_FLEX_FLOOR_MIB
#define ORBIS_MMAP_FLEX_FLOOR_MIB 0
#endif

#if ORBIS_MMAP_FLEX_FLOOR_MIB
// The SDK header declares this by value, which cannot be a query (see orbis_mem.cpp); bound to the
// out-pointer form by asm label so a transitively included prototype cannot clash.
extern "C" int32_t ommFlexibleAvailable(size_t* out) __asm__("sceKernelAvailableFlexibleMemorySize");
#endif

namespace {

// 16 KiB, musl's PAGE_SIZE here: `addq $0x400f,%rbx; andq $-0x4000,%rbx` at malloc+0x5a,
// and the same pair in __expand_heap.
constexpr uint64_t Page = 0x4000;

// The direct-memory quantum, used as both length round-up and alignment, as in the SDK
// sample samples/_common/graphics.cpp:59,116-119.
constexpr uint64_t DirectAlign = 0x200000;

// A 2 MiB-aligned base is 16 KiB-aligned, so any offset that is a multiple of Page gives a
// page-aligned address and musl never sees an unaligned mapping.
static_assert(DirectAlign%Page==0,"direct-memory alignment no longer covers musl's page");

constexpr uint64_t CarveBytes = 128ull*1024*1024;
// Overridable by the embedding build (e.g. RPCS3 links large LLVM JIT objects into the heap).
#ifndef ORBIS_MMAP_MAX_CARVES
#define ORBIS_MMAP_MAX_CARVES 12
#endif
constexpr uint32_t MaxCarves  = ORBIS_MMAP_MAX_CARVES;
// The cap on direct memory held. By default it follows the slot count; stated in MiB it stays
// put while the slot count grows (the ladder below makes smaller carve-outs).
#ifdef ORBIS_MMAP_DIRECT_CAP_MIB
constexpr uint64_t DirectCap  = uint64_t(ORBIS_MMAP_DIRECT_CAP_MIB)*1024*1024;
#else
constexpr uint64_t DirectCap  = uint64_t(MaxCarves)*CarveBytes;
#endif
static_assert(MaxCarves>0 && DirectCap>=CarveBytes,"the heap needs at least one full carve-out");

// Both off by default: one allocation attempt per carve-out, refused at the cap, and
// carve-outs are never returned. See orbis_mmap.h, "build knobs".
#ifndef ORBIS_MMAP_CARVE_LADDER
#define ORBIS_MMAP_CARVE_LADDER 0
#endif
#ifndef ORBIS_MMAP_TRIM
#define ORBIS_MMAP_TRIM 0
#endif
// Off by default: first fit over the whole pool, one carve-out list for every request.
#ifndef ORBIS_MMAP_HEAP_BAND_MIB
#define ORBIS_MMAP_HEAP_BAND_MIB 0
#endif
#ifndef ORBIS_MMAP_SEGREGATE
#define ORBIS_MMAP_SEGREGATE 0
#endif
#ifndef ORBIS_MMAP_DEDICATED_MIB
#define ORBIS_MMAP_DEDICATED_MIB 32
#endif

// Smallest carve-out the ladder asks for (unless the request itself is larger), and the
// largest one the single overdraft may take past the cap.
constexpr uint64_t LadderFloor  = 32ull*1024*1024;
constexpr uint64_t OverdraftMax = CarveBytes;
static_assert(LadderFloor%DirectAlign==0 && OverdraftMax%DirectAlign==0,"ladder sizes must be direct-memory quanta");
#if ORBIS_MMAP_CARVE_LADDER
// Direct memory the overdraft must leave free even when no reservation is set: the 0.37 overdraft took
// the last 128 MiB of the pool and a 2 MiB JIT commit failed 215 s later.
constexpr uint64_t OverdraftKeepFree = 256ull*1024*1024;
#endif

// Top band of the pool searched for new carve-outs (ORBIS_MMAP_HEAP_BAND_MIB).
constexpr uint64_t HeapBand = uint64_t(ORBIS_MMAP_HEAP_BAND_MIB)*1024*1024;
static_assert(HeapBand%DirectAlign==0,"the heap band must be a whole number of direct-memory quanta");
// Mappings above this get a carve-out of their own size (ORBIS_MMAP_SEGREGATE), while this many slots
// are free.
constexpr uint64_t Dedicated          = uint64_t(ORBIS_MMAP_DEDICATED_MIB)*1024*1024;
constexpr uint32_t DedicatedSlotsLeft = 8;
// Carve-out kinds. Without ORBIS_MMAP_SEGREGATE every carve-out is KindAny.
enum : uint32_t { KindAny = 0, KindSmall = 1, KindLarge = 2 };

#if ORBIS_MMAP_EXPAND_HEAP
// Largest single growth step of musl's small heap; 0 keeps musl's doubling.
constexpr uint64_t ExpandStepMax = uint64_t(ORBIS_MMAP_EXPAND_STEP_MAX_MIB)*1024*1024;
static_assert(ExpandStepMax%Page==0,"a small-heap step must be a whole number of musl pages");
#endif
// Flexible memory a fallback must leave free (libkernel takes thread stacks from it).
constexpr uint64_t FlexFloor = uint64_t(ORBIS_MMAP_FLEX_FLOOR_MIB)*1024*1024;

// 128 MiB / 240 KiB (musl's smallest mapping) is 546 live blocks, and a free block can
// only sit between two used ones, so 1536 is ~1.4x the worst case. Running out is not
// fatal: the next carve-out is tried, then the flexible pool, both logged.
constexpr uint32_t MaxBlocks = 1536;

// The exact tuple musl's two allocator call sites emit, and the only one claimed here.
// PROT_READ|PROT_WRITE == 3 and MAP_PRIVATE|MAP_ANON == 0x1002 (FreeBSD 9
// sys/sys/mman.h), i.e. `movl $3,%edx; movl $0x1002,%ecx` at malloc+0x70 and
// __expand_heap+0x5c.
constexpr int OwnedProt  = PROT_READ|PROT_WRITE;
constexpr int OwnedFlags = MAP_PRIVATE|MAP_ANON;
static_assert(OwnedProt==3 && OwnedFlags==0x1002,"the SDK's mman.h no longer agrees with the disassembly");

constexpr uint32_t LogCap = 8;

struct Block {
  uint64_t off;
  uint64_t size;
  uint32_t used;
  };

struct Carve {
  uint8_t* va;
  off_t    phys;
  uint64_t size;  // 0 = slot not in service (never used, or trimmed)
  uint32_t n;
  uint32_t gen;   // odd while the slot is being filled or trimmed (lock-free readers retry)
  uint32_t kind;  // KindAny, KindSmall or KindLarge; under the lock
  Block    blk[MaxBlocks];
  };

// .bss, so it is zero before any dynamic initialiser runs: the first anonymous mapping
// can happen inside a static constructor.
Carve    carve[MaxCarves];
uint32_t carves = 0;  // slots ever used; only grows. Written under the lock, read lock-free.

std::atomic<uint64_t> heldBytes{0};
std::atomic<uint64_t> liveBytes{0};
std::atomic<uint64_t> peakLive{0};
std::atomic<uint64_t> cumMapped{0};
std::atomic<uint64_t> mapCalls{0};
std::atomic<uint64_t> unmapCalls{0};
std::atomic<uint64_t> fallbackCalls{0};
std::atomic<uint64_t> fallbackBytes{0};
std::atomic<uint64_t> anomalies{0};
std::atomic<uint32_t> fallbackLogged{0};
std::atomic<uint32_t> anomalyLogged{0};
std::atomic<uint32_t> bannerDone{0};
std::atomic<uint32_t> carvesHeld{0};    // slots in service
std::atomic<uint64_t> runtimeCap{0};    // mmapDirectSetCap; 0 = build cap
std::atomic<uint32_t> heapCritical{0};  // the heap reached its cap
std::atomic<uint64_t> trimmedBytes{0};
std::atomic<uint64_t> smallHeapBytes{0};    // what __expand_heap handed musl; never returned
std::atomic<uint64_t> floorRefusals{0};
std::atomic<uint64_t> floorRefusedBytes{0};
// Conditions that can repeat on every request and are logged under the lock (see countForLog).
std::atomic<uint64_t> slotsFullRefusals{0};      // every carve-out slot taken
std::atomic<uint64_t> slotsFullRefusedBytes{0};
std::atomic<uint64_t> capRefusals{0};            // a new carve-out would pass the cap
std::atomic<uint64_t> allocFailures{0};          // sceKernelAllocateDirectMemory failed
std::atomic<uint64_t> mapFailures{0};            // sceKernelMapDirectMemory failed
std::atomic<uint64_t> unalignedBases{0};         // sceKernelMapDirectMemory returned an unaligned base
// The direct-memory authority (mmapDirectSetReserve, the pressure hook, live fallback bytes).
std::atomic<uint64_t> reserveBytes{0};
std::atomic<uint64_t> refusalsAll{0};            // new carve-outs refused, any reason
std::atomic<uint64_t> reserveRefusals{0};        // ...by the reservation
std::atomic<uint64_t> lastRefusalUs{0};
std::atomic<uint64_t> reserveLogUs{0};           // last reservation-refusal line (once a minute)
std::atomic<uint64_t> pressureEvents{0};         // requests no carve-out could serve
std::atomic<uint64_t> hookCalls{0};
std::atomic<uint64_t> fallbackLive{0};           // fallback bytes still mapped
std::atomic<uint64_t> fallbackSmall{0};          // ...of which small-heap steps (never unmapped)
std::atomic<uint64_t> fallbackUntracked{0};      // fallback bytes the table had no room for (never decremented)
std::atomic<uint64_t> heldSmall{0};              // held in KindSmall carve-outs
std::atomic<uint64_t> placedTop{0};              // carve-outs placed at the top of the highest fitting run
std::atomic<uint64_t> placedBand{0};             // ...first fit in the top band
std::atomic<uint64_t> placedAny{0};              // ...first fit anywhere
std::atomic<uint64_t> borrowedMaps{0};           // requests served from the other kind's carve-outs
std::atomic<uint64_t> refusalTrims{0};           // trims run because a request found no room
std::atomic<uint64_t> carveEvents{0};            // carve-outs taken (thins their log line)
std::atomic<uint64_t> capSetUs{0};               // last mmapDirectSetCap line
std::atomic<uint64_t> reserveSetUs{0};           // last mmapDirectSetReserve line
std::atomic<uint32_t> trimState{0};              // 0 = not decided yet, 1 = on, 2 = off by the flag file
#ifdef ORBIS_MMAP_TRIM_OFF_FLAG
std::atomic<uint32_t> trimOffLogged{0};
#endif
#if ORBIS_MMAP_EXPAND_HEAP
std::atomic<uint32_t> expandSteps{0};       // musl's mmap_step
#endif
#if ORBIS_MMAP_FLEX_FLOOR_MIB
std::atomic<uint32_t> floorLogged{0};
#endif
#if ORBIS_MMAP_CARVE_LADDER
std::atomic<uint32_t> capLogged{0};
// One refused carve-out costs up to three attempts; when physical memory stays short every new
// carve-out is refused again, and the log sink keeps only a limited number of lines per session.
std::atomic<uint32_t> allocFailLogged{0};
// Under the lock. 0 = available, 1 = taken, 2 = failed at every ladder size (not retried).
uint32_t              overdraftUsed = 0;
#endif

// A pthread mutex cannot be used here (see orbis_mmap.h, "thread safety"). Nothing in
// this lock or behind it reaches an allocator.
std::atomic<int> lockFlag{0};
// The ledger's lock, same kind. Order: allocator lock, then ledger lock; directLedger takes only this one.
std::atomic<int> ledgerFlag{0};

void lockAcq(std::atomic<int>& flag) {
  unsigned spins = 0;
  for(;;) {
    int idle = 0;
    if(flag.compare_exchange_weak(idle,1,std::memory_order_acquire,std::memory_order_relaxed))
      return;
    if(++spins<64) {
      __builtin_ia32_pause();
      } else {
      // Yield: the lock is held across sceKernelAllocateDirectMemory, which is slow.
      spins = 0;
      sceKernelUsleep(50);
      }
    }
  }

void lockRel(std::atomic<int>& flag) {
  flag.store(0,std::memory_order_release);
  }

struct Guard {
  Guard()  { lockAcq(lockFlag); }
  ~Guard() { lockRel(lockFlag); }
  };

struct LedgerGuard {
  LedgerGuard()  { lockAcq(ledgerFlag); }
  ~LedgerGuard() { lockRel(ledgerFlag); }
  };

struct LedgerGuardIf {
  const bool on;
  explicit LedgerGuardIf(bool take) : on(take) { if(on) lockAcq(ledgerFlag); }
  ~LedgerGuardIf() { if(on) lockRel(ledgerFlag); }
  };

uint64_t nowUs() {
  return uint64_t(sceKernelGetProcessTime());
  }

// A setting a budget authority moves every second: a line for the first change, a return to 0, and
// otherwise at most one a minute.
bool settingLogDue(std::atomic<uint64_t>& lastUs, uint64_t prev, uint64_t next) {
  const uint64_t now  = nowUs();
  const uint64_t last = lastUs.load(std::memory_order_relaxed);
  if(last!=0 && prev!=0 && next!=0 && now-last<60000000ull)
    return false;
  lastUs.store(now!=0 ? now : 1,std::memory_order_relaxed);
  return true;
  }

void noteRefusal() {
  refusalsAll.fetch_add(1,std::memory_order_relaxed);
  const uint64_t now = nowUs();
  lastRefusalUs.store(now!=0 ? now : 1,std::memory_order_relaxed);
  }

uint64_t alignUp(uint64_t v, uint64_t a) {
  return (v+a-1) & ~(a-1);
  }

// Counts one occurrence of a condition that can repeat on every request and says whether to log it: the 1st,
// 2nd, 4th, 8th ... occurrence (returns its number), otherwise 0. The log sink writes a file and the console's
// debug output, and these lines are written under the allocator lock, so one line per request would serialise
// every other thread's malloc behind file I/O and grow the log without bound; this keeps the first occurrence and
// a running count in a few dozen lines per process. mmapDirectReport prints the totals.
uint64_t countForLog(std::atomic<uint64_t>& counter) {
  const uint64_t n = counter.fetch_add(1,std::memory_order_relaxed)+1;
  return (n & (n-1))==0 ? n : 0;
  }

void banner(bool active) {
  if(bannerDone.exchange(1,std::memory_order_relaxed)!=0)
    return;
  if(active)
    orbis_log("mem mmap-direct: ACTIVE - musl's anonymous mappings are served from DIRECT "
            "memory, carve-out %llu KiB, cap %llu KiB (ORBIS_MMAP_DIRECT=1)",
            (unsigned long long)(CarveBytes/1024),(unsigned long long)(DirectCap/1024));
  else
    orbis_log("mem mmap-direct: DISABLED - musl's anonymous mappings go to FLEXIBLE memory, "
            "the pool that overflowed on 2026-08-04 (ORBIS_MMAP_DIRECT=0)");
  // Only when a build knob changed the default behaviour.
  if(active && (ORBIS_MMAP_CARVE_LADDER || ORBIS_MMAP_TRIM))
    orbis_log("mem mmap-direct: %u slots, carve-out ladder %s (floor %llu KiB, one overdraft of at "
            "most %llu KiB), trim %s",
            unsigned(MaxCarves),ORBIS_MMAP_CARVE_LADDER ? "on" : "off",
            (unsigned long long)(LadderFloor/1024),(unsigned long long)(OverdraftMax/1024),
            ORBIS_MMAP_TRIM ? "on" : "off");
  if(active && (ORBIS_MMAP_HEAP_BAND_MIB || ORBIS_MMAP_SEGREGATE))
    orbis_log("mem mmap-direct: placement %s (top band %llu KiB), small-heap/large segregation %s "
            "(own carve-out above %llu KiB)",
            ORBIS_MMAP_HEAP_BAND_MIB ? "top of the pool" : "first fit",(unsigned long long)(HeapBand/1024),
            ORBIS_MMAP_SEGREGATE ? "on" : "off",(unsigned long long)(Dedicated/1024));
  }

void noteLive(uint64_t n) {
  const uint64_t now = liveBytes.fetch_add(n,std::memory_order_relaxed)+n;
  uint64_t hi = peakLive.load(std::memory_order_relaxed);
  while(now>hi && !peakLive.compare_exchange_weak(hi,now,std::memory_order_relaxed))
    ;
  cumMapped.fetch_add(n,std::memory_order_relaxed);
  mapCalls.fetch_add(1,std::memory_order_relaxed);
  }

// Hands out block `i` of `c` (free, at least `len` long), splitting off the tail. Null when the tail
// needs a block slot and none is left: the block is then skipped rather than handed out whole.
uint8_t* takeLocked(Carve& c, uint32_t i, uint64_t len) {
  Block& b = c.blk[i];
  if(b.size==len) {
    b.used = 1;
    return c.va+b.off;
    }
  if(c.n>=MaxBlocks)
    return nullptr;
  std::memmove(&c.blk[i+2],&c.blk[i+1],sizeof(Block)*(c.n-i-1));
  c.blk[i+1].off  = b.off+len;
  c.blk[i+1].size = b.size-len;
  c.blk[i+1].used = 0;
  b.size = len;
  b.used = 1;
  ++c.n;
  return c.va+b.off;
  }

uint8_t* allocInLocked(Carve& c, uint64_t len) {
  for(uint32_t i=0; i<c.n; ++i) {
    const Block& b = c.blk[i];
    if(b.used!=0 || b.size<len)
      continue;
    if(uint8_t* p = takeLocked(c,i,len))
      return p;
    }
  return nullptr;
  }

// First fit (in slot and address order) or best fit (the smallest free block that fits) over the
// carve-outs of one kind.
uint8_t* allocKindLocked(uint64_t len, uint32_t kind, bool best) {
  if(!best) {
    for(uint32_t i=0; i<carves; ++i)
      if(carve[i].size!=0 && carve[i].kind==kind)
        if(uint8_t* p = allocInLocked(carve[i],len))
          return p;
    return nullptr;
    }
  Carve*   bc = nullptr;
  uint32_t bi = 0;
  uint64_t bs = ~uint64_t(0);
  for(uint32_t i=0; i<carves && bs!=len; ++i) {
    Carve& c = carve[i];
    if(c.size==0 || c.kind!=kind)
      continue;
    for(uint32_t k=0; k<c.n; ++k) {
      const Block& b = c.blk[k];
      if(b.used!=0 || b.size<len || b.size>=bs || (b.size!=len && c.n>=MaxBlocks))
        continue;
      bc = &c;
      bi = k;
      bs = b.size;
      if(bs==len)
        break;
      }
    }
  return bc!=nullptr ? takeLocked(*bc,bi,len) : nullptr;
  }

uint64_t effectiveCap() {
  const uint64_t r = runtimeCap.load(std::memory_order_relaxed);
  return r!=0 && r<DirectCap ? r : DirectCap;
  }

// ------------------------------------------------------------------ direct-memory ledger

constexpr int32_t  DmqFindNext  = 1;       // SCE_KERNEL_DMQ_FIND_NEXT
constexpr uint32_t WalkMax      = 4096;    // a longer walk is not trusted for that sample
constexpr uint32_t GapKeep      = 16;      // highest free runs remembered for placement
// Every carve-out is at least LadderFloor long, so a shorter free run is never a placement candidate.
constexpr uint64_t GapMin       = LadderFloor;
// A sample at most this old serves the admission and placement of a new carve-out. The heap's own carve-outs
// are applied to it as they are taken, so a burst of carve-outs costs one walk under the allocator lock.
constexpr uint64_t AdmitAgeUs   = 100000;
// Untrusted samples in a row after which the ledger stays estimated for the process.
constexpr uint32_t UntrustedMax = 8;

struct DmqInfo {
  int64_t start;
  int64_t end;
  int32_t type;
  int32_t pad;
  };
static_assert(sizeof(DmqInfo)==24,"SceKernelDirectMemoryQueryInfo is 24 bytes");

struct Gap {
  uint64_t start;  // 2 MiB-aligned
  uint64_t end;
  };

struct Walk {
  uint64_t allocated;
  uint64_t largest;         // largest free run
  uint64_t largestAligned;  // the same, measured from its first 2 MiB-aligned address
  uint32_t entries;
  int32_t  endRc;           // the query result that ended the walk; 0 = it reached the end of the pool
  bool     ok;
  uint32_t gaps;            // free runs in `gap`, ascending addresses (the highest ones kept)
  Gap      gap[GapKeep];
  };

void noteGap(Walk& w, uint64_t a, uint64_t b) {
  if(b-a>w.largest)
    w.largest = b-a;
  const uint64_t as = alignUp(a,DirectAlign);
  if(b<=as)
    return;
  if(b-as>w.largestAligned)
    w.largestAligned = b-as;
  if(b-as<GapMin)
    return;
  if(w.gaps==GapKeep) {
    std::memmove(&w.gap[0],&w.gap[1],sizeof(Gap)*(GapKeep-1));
    --w.gaps;
    }
  w.gap[w.gaps].start = as;
  w.gap[w.gaps].end   = b;
  ++w.gaps;
  }

// One FIND_NEXT walk over [0, total). Syscalls only: no allocation, no lock of this file. `ok` stays
// false when the walk cannot be trusted (bad range, no progress, too long).
void walkDirect(uint64_t total, Walk& w) {
  std::memset(&w,0,sizeof(w));
  if(&ommDirectQuery==nullptr) {
    w.endRc = -1;
    return;
    }
  uint64_t off = 0;  // end of the last range seen
  for(uint32_t i=0; off<total; ++i) {
    if(i>=WalkMax)
      return;
    // Room past the 24 bytes asked for, in case the kernel writes a longer record.
    union {
      DmqInfo info;
      uint8_t raw[64];
      } buf;
    std::memset(&buf,0,sizeof(buf));
    const int32_t rc = ommDirectQuery(off_t(off),DmqFindNext,&buf,sizeof(DmqInfo));
    if(rc!=0) {
      // No allocated range at or above `off`.
      w.endRc = rc;
      break;
      }
    if(buf.info.start<0 || buf.info.end<=buf.info.start)
      return;
    uint64_t s = uint64_t(buf.info.start);
    uint64_t e = uint64_t(buf.info.end);
    if((e & (Page-1))==Page-1)
      ++e;  // an inclusive end
    if(s<off)
      s = off;
    if(e<=off)
      return;
    if(s>=total)
      break;
    if(e>total)
      e = total;
    if(s>off)
      noteGap(w,off,s);
    w.allocated += e-s;
    ++w.entries;
    off = e;
    }
  if(off<total)
    noteGap(w,off,total);
  w.ok = true;
  }

// The cached sample, under ledgerFlag.
struct LedgerState {
  uint64_t total;
  uint64_t allocated;
  uint64_t freeTotal;
  uint64_t largest;
  uint64_t sampledUs;  // 0 = none
  uint64_t walkUs;
  uint32_t entries;
  int32_t  rc;
  uint32_t estimated;
  uint32_t gaps;       // highest free runs of a trusted walk, ascending; 0 when estimated
  Gap      gap[GapKeep];
  };
LedgerState ledgerCache{};
int32_t     ledgerEndRc     = 0;  // the code that ended a trusted walk; a walk ending with another one is not trusted
uint32_t    ledgerUntrusted = 0;  // untrusted samples in a row
std::atomic<uint32_t> ledgerMode{0};        // 0 = not probed, 1 = kernel walk, 2 = estimated
// Changes by the heap since the sample. Bit 1: a carve-out was taken (already applied to the sample; directLedger
// samples again for the largest run). Bit 2: one was returned (not applied; the next admission samples again).
std::atomic<uint32_t> ledgerDirty{0};
std::atomic<uint64_t> ledgerBoot{0};
std::atomic<uint64_t> ledgerExternal{0};
std::atomic<uint32_t> ledgerExternalSet{0};
std::atomic<uint64_t> ledgerWalkFailures{0};
// The probe line when it was made before the log sink was set (the first carve-out can come from a static
// constructor): kept here and logged by the first directLedger call that finds the sink set.
char                  probeLine[512];
std::atomic<uint32_t> probePending{0};

// Under ledgerFlag: logs the probe line now, or keeps it for later.
void emitProbeLocked(const char* line) {
  if(orbis_log_enabled()) {
    orbis_log("%s",line);
    return;
    }
  std::snprintf(probeLine,sizeof(probeLine),"%s (made before the log was set)",line);
  probePending.store(1,std::memory_order_release);
  }

// Without a trusted walk: the kernel's largest free run is a lower bound of the free total; with the
// other owners' counters (directLedgerSetExternal) the remainder is used when it is larger.
void estimateLedger(uint64_t total, LedgerState& s) {
  off_t         phys    = 0;
  size_t        sz      = 0;
  const int32_t rc      = ommDirectAvailable(0,off_t(total),size_t(DirectAlign),&phys,&sz);
  const uint64_t largest = rc==0 ? uint64_t(sz) : 0;
  uint64_t      freeTotal = largest;
  if(ledgerExternalSet.load(std::memory_order_relaxed)!=0) {
    const uint64_t used = ledgerBoot.load(std::memory_order_relaxed)+heldBytes.load(std::memory_order_relaxed)+
                          ledgerExternal.load(std::memory_order_relaxed);
    const uint64_t rest = used<total ? total-used : 0;
    if(rest>freeTotal)
      freeTotal = rest;
    }
  if(freeTotal>total)
    freeTotal = total;
  s.total     = total;
  s.allocated = total-freeTotal;
  s.freeTotal = freeTotal;
  s.largest   = largest;
  s.walkUs    = 0;
  s.entries   = 0;
  s.rc        = rc;
  s.estimated = 1;
  s.gaps      = 0;
  }

// A trusted walk's figures into `s`.
void sampleFromWalk(const Walk& w, uint64_t total, LedgerState& s) {
  s.total     = total;
  s.allocated = w.allocated;
  s.freeTotal = total-w.allocated;
  s.largest   = w.largest;
  s.entries   = w.entries;
  s.rc        = w.endRc;
  s.estimated = 0;
  s.gaps      = w.gaps;
  std::memcpy(s.gap,w.gap,sizeof(Gap)*w.gaps);
  }

// A walk is trusted when it ended normally and its largest free run (from an aligned start) is the one
// sceKernelAvailableDirectMemorySize reports: a walk that stopped early or missed ranges shows a larger
// one. `kRc`/`kLargest` receive the kernel's answer.
bool walkTrusted(const Walk& w, uint64_t total, int32_t& kRc, uint64_t& kLargest) {
  off_t  phys = 0;
  size_t sz   = 0;
  kRc      = ommDirectAvailable(0,off_t(total),size_t(DirectAlign),&phys,&sz);
  kLargest = kRc==0 ? uint64_t(sz) : 0;
  const uint64_t diff = w.largestAligned>kLargest ? w.largestAligned-kLargest : kLargest-w.largestAligned;
  return w.ok && w.allocated<=total && (kRc==0 ? diff<=2*DirectAlign : w.entries!=0);
  }

// A new sample into ledgerCache, under ledgerFlag with the mode decided.
void refreshLedgerLocked() {
  // Cleared before the walk: a change made after this point stays marked for the next sample.
  ledgerDirty.store(0,std::memory_order_relaxed);
  const uint64_t total = uint64_t(sceKernelGetDirectMemorySize());
  const uint64_t t0    = nowUs();
  LedgerState    s{};
  if(ledgerMode.load(std::memory_order_relaxed)==1) {
    Walk     w;
    int32_t  kRc      = 0;
    uint64_t kLargest = 0;
    bool     trusted  = false;
    // A second walk when another owner allocated or freed between the walk and the kernel's answer.
    for(unsigned attempt=0; attempt<2 && !trusted; ++attempt) {
      walkDirect(total,w);
      const bool endOk = w.endRc==0 || ledgerEndRc==0 || w.endRc==ledgerEndRc;
      trusted = endOk && walkTrusted(w,total,kRc,kLargest);
      }
    if(trusted) {
      ledgerUntrusted = 0;
      if(w.endRc!=0 && ledgerEndRc==0)
        ledgerEndRc = w.endRc;
      sampleFromWalk(w,total,s);
      s.walkUs = nowUs()-t0;
      } else {
      ledgerWalkFailures.fetch_add(1,std::memory_order_relaxed);
      estimateLedger(total,s);
      if(++ledgerUntrusted>=UntrustedMax) {
        // Each untrusted sample costs two walks and gives an estimate anyway.
        ledgerMode.store(2,std::memory_order_release);
        orbis_log("mem ledger: %u samples in a row disagree with sceKernelAvailableDirectMemorySize (last walk: %u "
                "entries, rc=0x%08x, largest aligned run %llu KiB, kernel %llu KiB rc=0x%08x) - ledger ESTIMATED from "
                "now on",
                ledgerUntrusted,w.entries,unsigned(w.endRc),(unsigned long long)(w.largestAligned/1024),
                (unsigned long long)(kLargest/1024),unsigned(kRc));
        }
      }
    } else {
    estimateLedger(total,s);
    }
  s.sampledUs = t0!=0 ? t0 : 1;
  ledgerCache = s;
  }

// Decides the mode once, under ledgerFlag, and leaves a sample in ledgerCache. The caller logs the line.
void probeLedgerLocked(char* line, size_t size) {
  const uint64_t total = uint64_t(sceKernelGetDirectMemorySize());
  const char*    why   = nullptr;
  Walk           w;
  std::memset(&w,0,sizeof(w));
  int32_t  kRc      = 0;
  uint64_t kLargest = 0;
  uint64_t walkUs   = 0;
  unsigned tries    = 0;
  bool     good     = false;
#ifdef ORBIS_MMAP_QUERY_OFF_FLAG
  if(access(ORBIS_MMAP_QUERY_OFF_FLAG,F_OK)==0)
    why = "off by " ORBIS_MMAP_QUERY_OFF_FLAG;
#endif
  if(why==nullptr && &ommDirectQuery==nullptr)
    why = "sceKernelDirectMemoryQuery is not exported";
  // A race with another thread's allocation is retried.
  while(why==nullptr && !good && tries<3) {
    ++tries;
    const uint64_t t0 = nowUs();
    walkDirect(total,w);
    walkUs = nowUs()-t0;
    good   = walkTrusted(w,total,kRc,kLargest);
    }
  if(why==nullptr && !good)
    why = !w.ok ? "the walk was not usable" : "the walk disagrees with sceKernelAvailableDirectMemorySize";

  ledgerDirty.store(0,std::memory_order_relaxed);
  if(good) {
    ledgerEndRc = w.endRc;
    // The heap's own carve-outs taken before the probe are not part of the boot figure.
    const uint64_t held = heldBytes.load(std::memory_order_relaxed);
    ledgerBoot.store(w.allocated>held ? w.allocated-held : 0,std::memory_order_relaxed);
    ledgerMode.store(1,std::memory_order_release);
    LedgerState s{};
    sampleFromWalk(w,total,s);
    s.walkUs    = walkUs;
    s.sampledUs = nowUs();
    if(s.sampledUs==0)
      s.sampledUs = 1;
    ledgerCache = s;
    std::snprintf(line,size,"mem ledger: query rc=0x%08x, %u entries, free %llu KiB, largest %llu KiB, allocated %llu "
                "KiB of %llu KiB (sceKernelAvailableDirectMemorySize %llu KiB rc=0x%08x), walk %llu us, %u attempt(s) "
                "- ledger ON (kernel walk)",
                unsigned(w.endRc),w.entries,(unsigned long long)(s.freeTotal/1024),
                (unsigned long long)(w.largest/1024),(unsigned long long)(w.allocated/1024),
                (unsigned long long)(total/1024),(unsigned long long)(kLargest/1024),unsigned(kRc),
                (unsigned long long)walkUs,tries);
    } else {
    if(tries==0) {
      off_t  phys = 0;
      size_t sz   = 0;
      kRc      = ommDirectAvailable(0,off_t(total),size_t(DirectAlign),&phys,&sz);
      kLargest = kRc==0 ? uint64_t(sz) : 0;
      }
    // An upper bound: the pool outside the largest free run, less the heap's own carve-outs.
    const uint64_t held  = heldBytes.load(std::memory_order_relaxed);
    const uint64_t other = kRc==0 && kLargest<=total ? total-kLargest : 0;
    ledgerBoot.store(other>held ? other-held : 0,std::memory_order_relaxed);
    ledgerMode.store(2,std::memory_order_release);
    refreshLedgerLocked();
    std::snprintf(line,size,"mem ledger: query rc=0x%08x, %u entries (walk: allocated %llu KiB, largest aligned run %llu "
                "KiB, %s), sceKernelAvailableDirectMemorySize %llu KiB rc=0x%08x, %u attempt(s) - ledger ESTIMATED: %s; "
                "free at least %llu KiB of %llu KiB, boot allocated %llu KiB",
                unsigned(w.endRc),w.entries,(unsigned long long)(w.allocated/1024),
                (unsigned long long)(w.largestAligned/1024),w.ok ? "complete" : "not usable",
                (unsigned long long)(kLargest/1024),unsigned(kRc),tries,why,
                (unsigned long long)(ledgerCache.freeTotal/1024),(unsigned long long)(total/1024),
                (unsigned long long)(ledgerBoot.load(std::memory_order_relaxed)/1024));
    }
  }

// The sample for a new carve-out, under ledgerFlag (held by the caller until the carve-out is applied to it):
// probed on first use (the probe line goes to `line`), walked again once older than AdmitAgeUs or after the
// heap returned a carve-out.
const LedgerState& admitSampleLocked(char* line, size_t size) {
  if(ledgerMode.load(std::memory_order_acquire)==0)
    probeLedgerLocked(line,size);
  const uint64_t now = nowUs();
  if(ledgerCache.sampledUs==0 || (ledgerDirty.load(std::memory_order_relaxed) & 2u)!=0 ||
     now-ledgerCache.sampledUs>=AdmitAgeUs)
    refreshLedgerLocked();
  return ledgerCache;
  }

// The carve-out [phys, phys + len) the heap just took, applied to the sample under ledgerFlag: the free total
// falls and the free run it came from is cut (pieces shorter than GapMin are dropped). The largest run is only
// clamped; directLedger samples again for it.
void ledgerTakenLocked(uint64_t phys, uint64_t len) {
  LedgerState& s = ledgerCache;
  ledgerDirty.fetch_or(1u,std::memory_order_relaxed);
  if(s.sampledUs==0)
    return;
  s.allocated += len;
  s.freeTotal  = s.freeTotal>len ? s.freeTotal-len : 0;
  if(s.largest>s.freeTotal)
    s.largest = s.freeTotal;
  const uint64_t a = phys;
  const uint64_t b = phys+len;
  Gap            out[2*GapKeep];
  uint32_t       n = 0;
  for(uint32_t i=0; i<s.gaps; ++i) {
    const Gap g = s.gap[i];
    if(b<=g.start || a>=g.end) {
      out[n++] = g;
      continue;
      }
    if(a>g.start && a-g.start>=GapMin)
      out[n++] = Gap{g.start,a};
    const uint64_t st = alignUp(b,DirectAlign);
    if(st<g.end && g.end-st>=GapMin)
      out[n++] = Gap{st,g.end};
    }
  // The highest GapKeep stay (ascending order is kept).
  const uint32_t drop = n>GapKeep ? n-GapKeep : 0;
  for(uint32_t i=drop; i<n; ++i)
    s.gap[i-drop] = out[i];
  s.gaps = n-drop;
  }

// Physical placement of a new carve-out: the top of the highest free run that fits (from a trusted walk,
// `lv` may be null), else first fit in the top band, else first fit anywhere (the only attempt without
// ORBIS_MMAP_HEAP_BAND_MIB).
int32_t allocPlacedLocked(uint64_t len, const LedgerState* lv, off_t* phys, const char*& where) {
  const uint64_t total = uint64_t(sceKernelGetDirectMemorySize());
#if ORBIS_MMAP_HEAP_BAND_MIB
  const uint32_t gaps = lv!=nullptr && lv->estimated==0 ? lv->gaps : 0;
  for(uint32_t i=gaps; i-->0;) {
    const Gap& gp = lv->gap[i];
    if(gp.end-gp.start<len)
      continue;
    const uint64_t start = (gp.end-len) & ~(DirectAlign-1);
    if(sceKernelAllocateDirectMemory(off_t(start),off_t(gp.end),size_t(len),size_t(DirectAlign),
                                     ORBIS_KERNEL_WB_ONION,phys)==0) {
      placedTop.fetch_add(1,std::memory_order_relaxed);
      where = "top of the pool";
      return 0;
      }
    // Taken meanwhile by another owner: the band below.
    break;
    }
  if(total>HeapBand &&
     sceKernelAllocateDirectMemory(off_t(total-HeapBand),off_t(total),size_t(len),size_t(DirectAlign),
                                   ORBIS_KERNEL_WB_ONION,phys)==0) {
    placedBand.fetch_add(1,std::memory_order_relaxed);
    where = "top band";
    return 0;
    }
#else
  (void)lv;
#endif
  where = "first fit";
  const int32_t rc = sceKernelAllocateDirectMemory(0,off_t(total),size_t(len),size_t(DirectAlign),
                                                   ORBIS_KERNEL_WB_ONION,phys);
  if(rc==0)
    placedAny.fetch_add(1,std::memory_order_relaxed);
  return rc;
  }

// Takes a new carve-out of at least `need` bytes for `kind` and returns its slot, or nullptr with the
// reason added to `why` (orbis::MmapPressure* bits).
Carve* addCarveLocked(uint64_t need, uint32_t kind, unsigned& why) {
  // A slot emptied by mmapDirectTrim is reused first. Without ORBIS_MMAP_TRIM every slot
  // below `carves` is in service, so this is always the next unused slot.
  uint32_t index     = carves;
  uint32_t freeSlots = MaxCarves-carves;
  for(uint32_t i=0; i<carves; ++i)
    if(carve[i].size==0) {
      if(index==carves)
        index = i;
      ++freeSlots;
      }
  if(index>=MaxCarves) {
    // Every later request that no carve-out can serve ends here too.
    noteRefusal();
    why |= orbis::MmapPressureCap;
    slotsFullRefusedBytes.fetch_add(need,std::memory_order_relaxed);
    if(const uint64_t n = countForLog(slotsFullRefusals))
      orbis_log("mem mmap-direct: REFUSED %llu KiB - all %u carve-outs taken (%llu KiB held); refusal %llu of "
              "this kind, logged at 1, 2, 4, 8 ... with the totals in the census. Further anonymous mappings "
              "fall back to flexible memory.",
              (unsigned long long)(need/1024),unsigned(MaxCarves),
              (unsigned long long)(heldBytes.load(std::memory_order_relaxed)/1024),(unsigned long long)n);
    return nullptr;
    }

  const uint64_t cap  = effectiveCap();
  const uint64_t held = heldBytes.load(std::memory_order_relaxed);
  // A large mapping past the dedicated size gets a carve-out of its own size while slots are plentiful,
  // so the carve-out is entirely free (and can be trimmed) as soon as that mapping is.
  const bool     dedicated = ORBIS_MMAP_SEGREGATE && kind==KindLarge && need>Dedicated &&
                             freeSlots>=DedicatedSlotsLeft;
  uint64_t       len  = alignUp(dedicated || need>CarveBytes ? need : CarveBytes,DirectAlign);
#if ORBIS_MMAP_CARVE_LADDER
  const uint64_t floorLen  = alignUp(need>LadderFloor ? need : LadderFloor,DirectAlign);
  bool           overdraft = false;
  if(held+len>cap) {
    const uint64_t room = held<cap ? ((cap-held) & ~(DirectAlign-1)) : 0;
    if(room>=floorLen) {
      // A smaller carve-out still fits under the cap.
      len = room;
      } else if(overdraftUsed==0 && floorLen<=OverdraftMax) {
      len       = len<OverdraftMax ? len : OverdraftMax;
      overdraft = true;
      } else {
      heapCritical.store(1,std::memory_order_relaxed);
      capRefusals.fetch_add(1,std::memory_order_relaxed);
      noteRefusal();
      why |= orbis::MmapPressureCap;
      if(capLogged.fetch_add(1,std::memory_order_relaxed)<LogCap)
        orbis_log("mem mmap-direct: REFUSED %llu KiB - direct memory held is %llu KiB of a %llu KiB "
                "cap and the overdraft is %s. Falling back to flexible memory; heap critical.",
                (unsigned long long)(need/1024),(unsigned long long)(held/1024),
                (unsigned long long)(cap/1024),
                overdraftUsed==1 ? "spent" : overdraftUsed==2 ? "unavailable (it failed)" : "too small for it");
      return nullptr;
      }
    }
  const uint64_t reserve = reserveBytes.load(std::memory_order_relaxed);
  const uint64_t keep    = overdraft && reserve<OverdraftKeepFree ? OverdraftKeepFree : reserve;
#else
  const uint64_t floorLen = len;
  const bool     overdraft = false;
  if(held+len>cap) {
    noteRefusal();
    why |= orbis::MmapPressureCap;
    // Without the ladder every later request that needs a new carve-out ends here too.
    if(const uint64_t n = countForLog(capRefusals))
      orbis_log("mem mmap-direct: REFUSED %llu KiB - a %llu KiB carve-out would put direct "
              "memory held at %llu KiB, past the %llu KiB cap that keeps the GNM allocator "
              "fed; refusal %llu of this kind, logged at 1, 2, 4, 8 ... Falling back to flexible memory.",
              (unsigned long long)(need/1024),(unsigned long long)(len/1024),
              (unsigned long long)((held+len)/1024),(unsigned long long)(cap/1024),(unsigned long long)n);
    return nullptr;
    }
  const uint64_t reserve = reserveBytes.load(std::memory_order_relaxed);
  const uint64_t keep    = reserve;
#endif

  // Admission: the pool must keep `keep` bytes free for the other owners (guest memory, JIT, GPU, stacks)
  // after this carve-out. The same sample gives the placement. The ledger lock stays held until the new
  // carve-out is applied to the sample, so no directLedger reader samples in between.
  const bool         useLedger = keep!=0 || HeapBand!=0;
  LedgerGuardIf      lg(useLedger);
  const LedgerState* lvp       = nullptr;
  if(useLedger) {
    char probe[512];
    probe[0] = '\0';
    lvp = &admitSampleLocked(probe,sizeof(probe));
    if(probe[0]!='\0')
      emitProbeLocked(probe);
    const LedgerState& lv = *lvp;
    if(keep!=0 && lv.freeTotal<len+keep) {
      const uint64_t room = lv.freeTotal>keep ? ((lv.freeTotal-keep) & ~(DirectAlign-1)) : 0;
      if(ORBIS_MMAP_CARVE_LADDER && room>=floorLen) {
        len = room;
        } else {
        heapCritical.store(1,std::memory_order_relaxed);
        const uint64_t n = reserveRefusals.fetch_add(1,std::memory_order_relaxed)+1;
        noteRefusal();
        why |= orbis::MmapPressureReserve;
        // Can repeat on every request while the pool is short: one line a minute.
        const uint64_t now  = nowUs();
        const uint64_t last = reserveLogUs.load(std::memory_order_relaxed);
        if(last==0 || now-last>=60000000ull) {
          reserveLogUs.store(now!=0 ? now : 1,std::memory_order_relaxed);
          orbis_log("mem mmap-direct: REFUSED a %llu KiB carve-out%s for %llu KiB - direct free %llu KiB (%s), "
                  "%llu KiB must stay free for the other owners (reserve %llu KiB); held %llu KiB, live %llu KiB; "
                  "reservation refusal %llu, logged at most once a minute. Heap critical.",
                  (unsigned long long)(floorLen/1024),overdraft ? " past the cap (overdraft)" : "",
                  (unsigned long long)(need/1024),(unsigned long long)(lv.freeTotal/1024),
                  lv.estimated ? "estimated" : "kernel ledger",(unsigned long long)(keep/1024),
                  (unsigned long long)(reserve/1024),(unsigned long long)(held/1024),
                  (unsigned long long)(liveBytes.load(std::memory_order_relaxed)/1024),(unsigned long long)n);
          }
        return nullptr;
        }
      }
    }

  off_t       phys  = 0;
  int32_t     errA  = 0;
  const char* where = "first fit";
  for(;;) {
    errA = allocPlacedLocked(len,lvp,&phys,where);
    if(errA==0)
      break;
#if ORBIS_MMAP_CARVE_LADDER
    allocFailures.fetch_add(1,std::memory_order_relaxed);
    const bool logIt = allocFailLogged.fetch_add(1,std::memory_order_relaxed)<LogCap*4;
#else
    // Without the ladder every later request that needs a carve-out repeats this call.
    const bool logIt = countForLog(allocFailures)!=0;
#endif
    if(logIt)
      orbis_log("mem mmap-direct: sceKernelAllocateDirectMemory(%llu KiB, align %llu, WB_ONION) "
              "failed 0x%08x - %llu KiB already held in %u carve-out(s)",
              (unsigned long long)(len/1024),(unsigned long long)DirectAlign,unsigned(errA),
              (unsigned long long)(held/1024),unsigned(carvesHeld.load(std::memory_order_relaxed)));
#if ORBIS_MMAP_CARVE_LADDER
    // Physical memory can be too fragmented for one long run while shorter ones are free.
    if(len>floorLen) {
      const uint64_t half = alignUp(len/2,DirectAlign);
      len = half>floorLen ? half : floorLen;
      if(logIt)
        orbis_log("mem mmap-direct: retrying with a %llu KiB carve-out",(unsigned long long)(len/1024));
      continue;
      }
    if(overdraft) {
      heapCritical.store(1,std::memory_order_relaxed);
      // Physical direct memory is short, not only the budget: without the latch every later request
      // past the cap repeats the same failing allocations under this lock. Only a floor-sized attempt
      // latches; a longer one failing says nothing about a LadderFloor run.
      if(floorLen<=LadderFloor) {
        overdraftUsed = 2;
        orbis_log("mem mmap-direct: OVERDRAFT FAILED - no carve-out down to %llu KiB could be allocated "
                "past the %llu KiB cap (last error 0x%08x, %llu KiB held); not retried, later carve-outs "
                "past the cap are refused at once",
                (unsigned long long)(floorLen/1024),(unsigned long long)(cap/1024),unsigned(errA),
                (unsigned long long)(held/1024));
        }
      }
#endif
    noteRefusal();
    why |= orbis::MmapPressureKernel;
    return nullptr;
    }

  // WB_ONION and CPU_RW (not the GNM allocator's 0x33): this is CPU heap. Write-combined
  // GARLIC suits surfaces, not a malloc arena that is read back constantly.
  void*         va   = nullptr;
  const int32_t errM = sceKernelMapDirectMemory(&va,size_t(len),ORBIS_KERNEL_PROT_CPU_RW,0,
                                                phys,size_t(DirectAlign));
  if(errM!=0) {
    if(const uint64_t n = countForLog(mapFailures))
      orbis_log("mem mmap-direct: sceKernelMapDirectMemory(%llu KiB, phys 0x%llx) failed 0x%08x (failure %llu, "
              "logged at 1, 2, 4, 8 ...)",
              (unsigned long long)(len/1024),(unsigned long long)phys,unsigned(errM),(unsigned long long)n);
    sceKernelReleaseDirectMemory(phys,size_t(len));
    noteRefusal();
    why |= orbis::MmapPressureKernel;
    return nullptr;
    }

  // Checked: musl uses the base directly in its chunk arithmetic, so a misaligned base
  // would corrupt the heap silently.
  if((reinterpret_cast<uintptr_t>(va) & (Page-1))!=0) {
    if(const uint64_t n = countForLog(unalignedBases))
      orbis_log("mem mmap-direct: sceKernelMapDirectMemory returned %p, which is NOT %llu-byte "
              "aligned - refusing to hand musl a base its page arithmetic cannot use (case %llu, "
              "logged at 1, 2, 4, 8 ...)",
              va,(unsigned long long)Page,(unsigned long long)n);
    sceKernelMunmap(va,size_t(len));
    sceKernelReleaseDirectMemory(phys,size_t(len));
    noteRefusal();
    why |= orbis::MmapPressureKernel;
    return nullptr;
    }

  // Filled between two generation bumps, size last: a lock-free reader either skips the
  // slot or sees it whole (see mmapDirectDescribe).
  Carve&         c   = carve[index];
  const uint32_t gen = c.gen;
  __atomic_store_n(&c.gen,gen+1,__ATOMIC_SEQ_CST);
  __atomic_store_n(&c.va,reinterpret_cast<uint8_t*>(va),__ATOMIC_RELAXED);
  c.phys        = phys;
  c.kind        = kind;
  c.blk[0].off  = 0;
  c.blk[0].size = len;
  c.blk[0].used = 0;
  __atomic_store_n(&c.n,uint32_t(1),__ATOMIC_RELAXED);
  __atomic_store_n(&c.size,len,__ATOMIC_RELEASE);
  __atomic_store_n(&c.gen,gen+2,__ATOMIC_RELEASE);
  if(index==carves)
    __atomic_store_n(&carves,index+1,__ATOMIC_RELEASE);
  carvesHeld.fetch_add(1,std::memory_order_relaxed);
  heldBytes.store(held+len,std::memory_order_relaxed);
  if(kind==KindSmall)
    heldSmall.fetch_add(len,std::memory_order_relaxed);
  if(useLedger)
    ledgerTakenLocked(uint64_t(phys),len);
  else
    ledgerDirty.fetch_or(3u,std::memory_order_relaxed);  // not applied: every reader samples again

#if ORBIS_MMAP_CARVE_LADDER
  if(overdraft) {
    overdraftUsed = 1;
    heapCritical.store(1,std::memory_order_relaxed);
    orbis_log("mem mmap-direct: CAP REACHED - one overdraft carve-out of %llu KiB past the %llu KiB "
            "cap; heap critical, the next refusal falls back to flexible memory",
            (unsigned long long)(len/1024),(unsigned long long)(cap/1024));
    }
#endif

  // Direct memory is physical, so this is when the pages are committed. The first 64 are always logged,
  // later ones at 1, 2, 4, 8 ... of the rest (carve-outs of a mapping's own size can be many).
  const uint64_t taken = carveEvents.fetch_add(1,std::memory_order_relaxed)+1;
  if(taken<=64 || ((taken-64) & (taken-65))==0)
    orbis_log("mem mmap-direct: carve-out %u taken - %llu KiB at %p (phys 0x%llx, %s%s), direct held "
            "%llu KiB of a %llu KiB cap",
            unsigned(index),(unsigned long long)(len/1024),va,(unsigned long long)phys,where,
            kind==KindSmall ? ", small heap" : dedicated ? ", own carve-out" : kind==KindLarge ? ", large" : "",
            (unsigned long long)((held+len)/1024),(unsigned long long)(cap/1024));
  return &c;
  }

// ------------------------------------------------------------------ trimming

struct TrimResult {
  uint64_t released;
  uint64_t kept;      // physical bytes whose release failed (still counted as held)
  unsigned count;
  unsigned failed;
  int32_t  lastErr;
  };

#if ORBIS_MMAP_TRIM
// TRIM can be turned off for the process by a flag file, checked once.
bool trimEnabled() {
  uint32_t s = trimState.load(std::memory_order_acquire);
  if(s==0) {
#ifdef ORBIS_MMAP_TRIM_OFF_FLAG
    s = access(ORBIS_MMAP_TRIM_OFF_FLAG,F_OK)==0 ? 2u : 1u;
#else
    s = 1;
#endif
    trimState.store(s,std::memory_order_release);
    }
  return s==1;
  }

// Unmaps and releases every carve-out from slot `keep` on that is one free block covering its whole range.
void trimLocked(unsigned keep, TrimResult& r) {
  for(uint32_t i=keep; i<carves; ++i) {
    Carve&         c   = carve[i];
    const uint64_t len = c.size;
    // Only a carve-out that is one free block covering its whole range.
    if(len==0 || c.n!=1 || c.blk[0].used!=0 || c.blk[0].off!=0 || c.blk[0].size!=len)
      continue;
    // Odd generation and size 0 first: from here on no lock-free reader can match the
    // range, and only then is it unmapped. The lock keeps malloc and munmap out.
    const uint32_t gen = c.gen;
    __atomic_store_n(&c.gen,gen+1,__ATOMIC_SEQ_CST);
    __atomic_store_n(&c.size,uint64_t(0),__ATOMIC_SEQ_CST);
    const int32_t errU = sceKernelMunmap(c.va,size_t(len));
    if(errU!=0) {
      // Still mapped and still ours: keep it in service.
      __atomic_store_n(&c.size,len,__ATOMIC_RELEASE);
      __atomic_store_n(&c.gen,gen+2,__ATOMIC_RELEASE);
      ++r.failed;
      r.lastErr = errU;
      continue;
      }
    const int32_t errR = sceKernelReleaseDirectMemory(c.phys,size_t(len));
    __atomic_store_n(&c.n,uint32_t(0),__ATOMIC_RELAXED);
    __atomic_store_n(&c.va,static_cast<uint8_t*>(nullptr),__ATOMIC_RELAXED);
    __atomic_store_n(&c.gen,gen+2,__ATOMIC_RELEASE);
    carvesHeld.fetch_sub(1,std::memory_order_relaxed);
    if(c.kind==KindSmall)
      heldSmall.fetch_sub(len,std::memory_order_relaxed);
    c.kind = KindAny;
    ledgerDirty.fetch_or(3u,std::memory_order_relaxed);
    if(errR!=0) {
      // The range is gone but the physical pages are still allocated to us; keep them in
      // `held` so the cap stays honest.
      r.kept += len;
      ++r.failed;
      r.lastErr = errR;
      continue;
      }
    heldBytes.fetch_sub(len,std::memory_order_relaxed);
    r.released += len;
    ++r.count;
    }
  trimmedBytes.fetch_add(r.released,std::memory_order_relaxed);
  }
#endif

// Segregation exists so that trimming can return large carve-outs; the trim flag file turns both off, and
// the heap then keeps one carve-out list as without ORBIS_MMAP_SEGREGATE. Decided at the first request.
bool segregateOn() {
#if ORBIS_MMAP_SEGREGATE && ORBIS_MMAP_TRIM
  return trimEnabled();
#else
  return ORBIS_MMAP_SEGREGATE!=0;
#endif
  }

// Logged after the lock is dropped: nothing may wait on the log sink while malloc is blocked.
void logTrim(const TrimResult& r, unsigned keep, const char* why) {
  if(r.count==0 && r.failed==0)
    return;
  orbis_log("mem mmap-direct: trim returned %llu KiB in %u carve-out(s) (first %u kept%s), %u failed "
          "(last 0x%08x, %llu KiB physical not released) - direct held %llu KiB, live %llu KiB",
          (unsigned long long)(r.released/1024),r.count,keep,why,r.failed,unsigned(r.lastErr),
          (unsigned long long)(r.kept/1024),
          (unsigned long long)(heldBytes.load(std::memory_order_relaxed)/1024),
          (unsigned long long)(liveBytes.load(std::memory_order_relaxed)/1024));
  }

bool freeInLocked(Carve& c, uint8_t* p, uint64_t len) {
  const uint64_t off = uint64_t(p-c.va);

  uint32_t lo = 0, hi = c.n;
  while(lo<hi) {
    const uint32_t m = (lo+hi)/2;
    if(c.blk[m].off<off)
      lo = m+1; else
      hi = m;
    }

  if(lo>=c.n || c.blk[lo].off!=off || c.blk[lo].used==0 || c.blk[lo].size!=len) {
    anomalies.fetch_add(1,std::memory_order_relaxed);
    if(anomalyLogged.fetch_add(1,std::memory_order_relaxed)<LogCap)
      orbis_log("mem mmap-direct: unmap(%p,%llu) does not name a live mapping this "
              "allocator handed out (carve-out offset %llu). musl always unmaps the exact "
              "base and length it was given, so this cannot happen - LEAKING the range "
              "rather than guessing which block it meant.",
              p,(unsigned long long)len,(unsigned long long)off);
    return false;
    }

  c.blk[lo].used = 0;
  liveBytes.fetch_sub(len,std::memory_order_relaxed);
  unmapCalls.fetch_add(1,std::memory_order_relaxed);

  // Coalesce forward first, so the index of the block being freed does not move.
  if(lo+1<c.n && c.blk[lo+1].used==0) {
    c.blk[lo].size += c.blk[lo+1].size;
    std::memmove(&c.blk[lo+1],&c.blk[lo+2],sizeof(Block)*(c.n-lo-2));
    --c.n;
    }
  if(lo>0 && c.blk[lo-1].used==0) {
    c.blk[lo-1].size += c.blk[lo].size;
    std::memmove(&c.blk[lo],&c.blk[lo+1],sizeof(Block)*(c.n-lo-1));
    --c.n;
    }
  return true;
  }

Carve* carveOfLocked(const void* p) {
  const uint8_t* q = reinterpret_cast<const uint8_t*>(p);
  for(uint32_t i=0; i<carves; ++i)
    if(q>=carve[i].va && q<carve[i].va+carve[i].size)
      return &carve[i];
  return nullptr;
  }

bool isOwnedRequest(const void* addr, int prot, int flags, int fd, off_t off) {
  return addr==nullptr && prot==OwnedProt && flags==OwnedFlags && fd<0 && off==0;
  }

// ------------------------------------------------------------------ flexible fallbacks still mapped

// musl unmaps a fallback mapping with its exact base, so live fallback bytes need the length of each
// one: an open-addressing table (linear probing, va 0 = empty) under the allocator lock.
constexpr uint32_t FallbackSlots = 1024;
constexpr uint32_t FallbackMask  = FallbackSlots-1;
static_assert((FallbackSlots & FallbackMask)==0,"the fallback table size must be a power of two");

struct FallbackEntry {
  uintptr_t va;
  uint64_t  len;
  };
FallbackEntry fallbackTab[FallbackSlots];
uint32_t      fallbackTabUsed = 0;

uint32_t fallbackHome(uintptr_t va) {
  return uint32_t((uint64_t(va>>14)*0x9e3779b97f4a7c15ull)>>54) & FallbackMask;
  }

void noteFallback(void* p, uint64_t len, bool small) {
  fallbackLive.fetch_add(len,std::memory_order_relaxed);
  if(small) {
    // musl never unmaps a small-heap step.
    fallbackSmall.fetch_add(len,std::memory_order_relaxed);
    return;
    }
  Guard g;
  if(fallbackTabUsed>=FallbackSlots*3/4) {
    fallbackUntracked.fetch_add(len,std::memory_order_relaxed);
    return;
    }
  const uintptr_t va = reinterpret_cast<uintptr_t>(p);
  uint32_t        i  = fallbackHome(va);
  while(fallbackTab[i].va!=0)
    i = (i+1) & FallbackMask;
  fallbackTab[i].va  = va;
  fallbackTab[i].len = len;
  ++fallbackTabUsed;
  }

// Under the allocator lock, for an unmap that is not inside a carve-out.
void dropFallbackLocked(const void* p) {
  if(fallbackTabUsed==0)
    return;
  const uintptr_t va = reinterpret_cast<uintptr_t>(p);
  uint32_t        i  = fallbackHome(va);
  while(fallbackTab[i].va!=va) {
    if(fallbackTab[i].va==0)
      return;
    i = (i+1) & FallbackMask;
    }
  fallbackLive.fetch_sub(fallbackTab[i].len,std::memory_order_relaxed);
  // Backward-shift deletion: an entry moves into the hole unless its home lies cyclically in (hole, j].
  uint32_t hole = i;
  for(uint32_t j=(i+1) & FallbackMask; fallbackTab[j].va!=0; j=(j+1) & FallbackMask) {
    const uint32_t home = fallbackHome(fallbackTab[j].va);
    if(((j-home) & FallbackMask)>=((j-hole) & FallbackMask)) {
      fallbackTab[hole] = fallbackTab[j];
      hole              = j;
      }
    }
  fallbackTab[hole].va  = 0;
  fallbackTab[hole].len = 0;
  --fallbackTabUsed;
  }

// ------------------------------------------------------------------ requests

std::atomic<orbis::MmapPressureHook> pressureHook{nullptr};

// Threads inside the pressure hook, by thread identity: a hook that allocated would otherwise recurse.
constexpr uint32_t HookSlots = 32;
std::atomic<uintptr_t> hookThreads[HookSlots];

uintptr_t selfId() {
#if defined(__x86_64__) && !defined(_WIN32)
  // The TCB self-pointer, unique per thread (orbis_cxa_guard.c uses the same; no TLS access needed).
  uintptr_t v = 0;
  __asm__ volatile("movq %%fs:0, %0" : "=r"(v));
  return v;
#else
  static thread_local unsigned char tag;
  return reinterpret_cast<uintptr_t>(&tag);
#endif
  }

int hookEnter() {
  const uintptr_t id = selfId();
  if(id==0)
    return -1;
  for(auto& t : hookThreads)
    if(t.load(std::memory_order_acquire)==id)
      return -1;
  for(uint32_t i=0; i<HookSlots; ++i) {
    uintptr_t idle = 0;
    if(hookThreads[i].compare_exchange_strong(idle,id,std::memory_order_acq_rel))
      return int(i);
    }
  return -1;
  }

// The carve-out paths of one request, under the lock: the request's own kind, a new carve-out, the other
// kind's free blocks (ORBIS_MMAP_SEGREGATE), and (ORBIS_MMAP_TRIM) the entirely free carve-outs, too small
// for this request, returned so that a new carve-out fits.
uint8_t* carveMapLocked(uint64_t want, bool small, unsigned& why, TrimResult& tr) {
  const bool     seg  = segregateOn();
  const uint32_t kind = !seg ? KindAny : small ? KindSmall : KindLarge;
  if(uint8_t* p = allocKindLocked(want,kind,kind==KindLarge))
    return p;
  if(Carve* fresh = addCarveLocked(want,kind,why))
    if(uint8_t* p = allocInLocked(*fresh,want))
      return p;
  if(seg)
    if(uint8_t* p = allocKindLocked(want,small ? KindLarge : KindSmall,true)) {
      borrowedMaps.fetch_add(1,std::memory_order_relaxed);
      return p;
      }
#if ORBIS_MMAP_TRIM
  if(trimEnabled()) {
    trimLocked(0,tr);
    if(tr.count!=0) {
      refusalTrims.fetch_add(1,std::memory_order_relaxed);
      if(Carve* fresh = addCarveLocked(want,kind,why))
        if(uint8_t* p = allocInLocked(*fresh,want))
          return p;
      }
    }
#else
  (void)tr;
#endif
  return nullptr;
  }

uint8_t* carveMap(uint64_t want, bool small, unsigned& why) {
  uint8_t*   out = nullptr;
  TrimResult tr{};
    {
    Guard g;
    out = carveMapLocked(want,small,why,tr);
    if(out!=nullptr)
      noteLive(want);
    }
  logTrim(tr,0,", to make room for a request");
  return out;
  }

// No carve-out could serve the request: the pressure hook, then one more try.
uint8_t* underPressure(uint64_t want, bool small, unsigned& why) {
  const uint64_t event = pressureEvents.fetch_add(1,std::memory_order_relaxed)+1;
  const uint64_t now   = nowUs();
  lastRefusalUs.store(now!=0 ? now : 1,std::memory_order_relaxed);
  uint8_t*                     out  = nullptr;
  const orbis::MmapPressureHook hook = pressureHook.load(std::memory_order_acquire);
  int                          slot = -1;
  if(hook!=nullptr && (slot = hookEnter())>=0) {
    hookCalls.fetch_add(1,std::memory_order_relaxed);
    hook(want,why);
    hookThreads[slot].store(0,std::memory_order_release);
    unsigned again = why & orbis::MmapPressureSmallHeap;
    out = carveMap(want,small,again);
    why |= again;
    }
  if(out==nullptr && (event & (event-1))==0)
    orbis_log("mem mmap-direct: PRESSURE - a %llu KiB %s request found no carve-out room (new carve-out:%s%s%s%s); held "
            "%llu KiB, live %llu KiB, reserve %llu KiB, fallback live %llu KiB; hook %s; event %llu, logged at 1, 2, 4, "
            "8 ...",
            (unsigned long long)(want/1024),small ? "small-heap" : "mapping",
            (why & orbis::MmapPressureReserve) ? " reserve" : "",(why & orbis::MmapPressureCap) ? " cap" : "",
            (why & orbis::MmapPressureKernel) ? " kernel" : "",
            (why & (orbis::MmapPressureReserve|orbis::MmapPressureCap|orbis::MmapPressureKernel)) ? "" : " not refused",
            (unsigned long long)(heldBytes.load(std::memory_order_relaxed)/1024),
            (unsigned long long)(liveBytes.load(std::memory_order_relaxed)/1024),
            (unsigned long long)(reserveBytes.load(std::memory_order_relaxed)/1024),
            (unsigned long long)(fallbackLive.load(std::memory_order_relaxed)/1024),
            hook==nullptr ? "none" : slot<0 ? "skipped (re-entered)" : "called",(unsigned long long)event);
  return out;
  }

void* mapOwned(size_t len, bool small) {
  const uint64_t want = alignUp(uint64_t(len),Page);
  unsigned       why  = small ? orbis::MmapPressureSmallHeap : 0u;
  uint8_t*       out  = carveMap(want,small,why);
  if(out==nullptr)
    out = underPressure(want,small,why);
  if(out!=nullptr) {
    // Required: musl assumes MAP_ANON memory is zero filled, and calloc returns mmapped
    // chunks without clearing them (`testb $0x1,-0x8(%rbx); je` at calloc+0x5f tests
    // C_INUSE, which is 0 for mmapped chunks, skipping mal0_clear). A dirty recycled block
    // would make calloc() return garbage.
    //
    // Unconditional, since it is not documented whether sceKernelAllocateDirectMemory
    // returns zeroed pages; the kernel would pay the same cost for an anonymous mapping.
    //
    // Outside the lock: the block is ours now, and a large memset under the global lock
    // would serialise every other thread.
    std::memset(out,0,size_t(want));
    return out;
    }
#if ORBIS_MMAP_FLEX_FLOOR_MIB
  // libkernel takes every new thread's stack from flexible memory. A fallback that would leave less
  // than the floor fails like an exhausted heap instead (malloc returns null, operator new runs the
  // new_handler), so the threads needed to stop the game can still be created. A failed query
  // refuses nothing.
  size_t flexFree = 0;
  if(ommFlexibleAvailable(&flexFree)==0 && uint64_t(flexFree)<FlexFloor+want) {
    heapCritical.store(1,std::memory_order_relaxed);
    floorRefusals.fetch_add(1,std::memory_order_relaxed);
    floorRefusedBytes.fetch_add(want,std::memory_order_relaxed);
    if(floorLogged.exchange(1,std::memory_order_relaxed)==0)
      orbis_log("mem mmap-direct: flexible floor: refused %llu KiB to keep thread stacks (flexible free "
              "%llu KiB, floor %llu KiB, direct held %llu KiB, %llu KiB live); later refusals are counted "
              "in the census",
              (unsigned long long)(want/1024),(unsigned long long)(flexFree/1024),
              (unsigned long long)(FlexFloor/1024),
              (unsigned long long)(heldBytes.load(std::memory_order_relaxed)/1024),
              (unsigned long long)(liveBytes.load(std::memory_order_relaxed)/1024));
    errno = ENOMEM;
    return MAP_FAILED;
    }
#endif
  fallbackCalls.fetch_add(1,std::memory_order_relaxed);
  fallbackBytes.fetch_add(want,std::memory_order_relaxed);
  if(fallbackLogged.fetch_add(1,std::memory_order_relaxed)<LogCap)
    orbis_log("mem mmap-direct: FALLING BACK to flexible memory for %llu KiB - direct "
            "held %llu KiB in %u carve-out(s), %llu KiB live. This request is served "
            "from the pool that overflows.",
            (unsigned long long)(want/1024),
            (unsigned long long)(heldBytes.load(std::memory_order_relaxed)/1024),
            unsigned(carvesHeld.load(std::memory_order_relaxed)),
            (unsigned long long)(liveBytes.load(std::memory_order_relaxed)/1024));
  void* p = mmap(nullptr,len,OwnedProt,OwnedFlags,-1,0);
  if(p!=MAP_FAILED && p!=nullptr)
    noteFallback(p,want,small);
  return p;
  }

}

void orbis::mmapDirectReport(const char* where) {
#if ORBIS_MMAP_DIRECT
  orbis_log("mem mmap-direct [%s]: ACTIVE - direct held %llu KiB in %u carve-out(s), live "
          "%llu KiB, peak live %llu KiB",
          where,(unsigned long long)(heldBytes.load(std::memory_order_relaxed)/1024),
          unsigned(carvesHeld.load(std::memory_order_relaxed)),
          (unsigned long long)(liveBytes.load(std::memory_order_relaxed)/1024),
          (unsigned long long)(peakLive.load(std::memory_order_relaxed)/1024));
  // A cumulative figure far above the held figure shows that unmapped blocks are reused.
  orbis_log("mem mmap-direct [%s]: %llu map / %llu unmap calls, %llu KiB mapped cumulatively, "
          "%llu fallback(s) to flexible for %llu KiB, %llu anomaly(s)",
          where,(unsigned long long)mapCalls.load(std::memory_order_relaxed),
          (unsigned long long)unmapCalls.load(std::memory_order_relaxed),
          (unsigned long long)(cumMapped.load(std::memory_order_relaxed)/1024),
          (unsigned long long)fallbackCalls.load(std::memory_order_relaxed),
          (unsigned long long)(fallbackBytes.load(std::memory_order_relaxed)/1024),
          (unsigned long long)anomalies.load(std::memory_order_relaxed));
#if ORBIS_MMAP_EXPAND_HEAP
  // The small heap is part of `live` and is never returned; the rest of `live` is large mappings.
  orbis_log("mem mmap-direct [%s]: small heap %llu KiB in %u step(s), step cap %llu KiB%s",
          where,(unsigned long long)(smallHeapBytes.load(std::memory_order_relaxed)/1024),
          unsigned(expandSteps.load(std::memory_order_relaxed)),(unsigned long long)(ExpandStepMax/1024),
          ExpandStepMax==0 ? " (none: musl's doubling)" : "");
#endif
  // Only printed when a knob or a run-time cap changed the default behaviour.
  const uint64_t rcap = runtimeCap.load(std::memory_order_relaxed);
  if(ORBIS_MMAP_CARVE_LADDER || ORBIS_MMAP_TRIM || ORBIS_MMAP_FLEX_FLOOR_MIB || rcap!=0 ||
     heapCritical.load(std::memory_order_relaxed)!=0)
    orbis_log("mem mmap-direct [%s]: cap %llu KiB (build %llu KiB), ladder %s, trim %s, critical %u, "
            "%llu KiB trimmed so far, flexible floor %llu KiB (%llu refusal(s) for %llu KiB)",
            where,(unsigned long long)(effectiveCap()/1024),(unsigned long long)(DirectCap/1024),
            ORBIS_MMAP_CARVE_LADDER ? "on" : "off",ORBIS_MMAP_TRIM ? "on" : "off",
            unsigned(heapCritical.load(std::memory_order_relaxed)),
            (unsigned long long)(trimmedBytes.load(std::memory_order_relaxed)/1024),
            (unsigned long long)(FlexFloor/1024),
            (unsigned long long)floorRefusals.load(std::memory_order_relaxed),
            (unsigned long long)(floorRefusedBytes.load(std::memory_order_relaxed)/1024));
  // The totals of the conditions whose own lines are thinned out (countForLog); only once one happened.
  const uint64_t full   = slotsFullRefusals.load(std::memory_order_relaxed);
  const uint64_t capped = capRefusals.load(std::memory_order_relaxed);
  const uint64_t allocs = allocFailures.load(std::memory_order_relaxed);
  const uint64_t maps   = mapFailures.load(std::memory_order_relaxed)+unalignedBases.load(std::memory_order_relaxed);
  if(full!=0 || capped!=0 || allocs!=0 || maps!=0)
    orbis_log("mem mmap-direct [%s]: REFUSED so far: %llu request(s) for %llu KiB with all %u carve-outs taken, "
            "%llu carve-out(s) at the cap; %llu sceKernelAllocateDirectMemory and %llu sceKernelMapDirectMemory "
            "failure(s)",
            where,(unsigned long long)full,
            (unsigned long long)(slotsFullRefusedBytes.load(std::memory_order_relaxed)/1024),unsigned(MaxCarves),
            (unsigned long long)capped,(unsigned long long)allocs,(unsigned long long)maps);
  // The direct-memory authority: only once a reservation, a pressure event, a fallback or a knob made it matter.
  const uint64_t reserve  = reserveBytes.load(std::memory_order_relaxed);
  const uint64_t pressure = pressureEvents.load(std::memory_order_relaxed);
  const uint64_t fbLive   = fallbackLive.load(std::memory_order_relaxed);
  if(reserve!=0 || pressure!=0 || fbLive!=0 || ORBIS_MMAP_HEAP_BAND_MIB || ORBIS_MMAP_SEGREGATE)
    orbis_log("mem mmap-direct [%s]: reserve %llu KiB, %llu refusal(s) (%llu by the reserve), %llu pressure "
            "event(s) (%llu hook call(s)), fallback live %llu KiB (small heap %llu KiB, untracked %llu KiB); "
            "placed %llu top / %llu band / %llu first fit; %llu borrowed, %llu trim(s) for room; small-heap "
            "carve-outs %llu KiB of %llu KiB held; trim %s",
            where,(unsigned long long)(reserve/1024),
            (unsigned long long)refusalsAll.load(std::memory_order_relaxed),
            (unsigned long long)reserveRefusals.load(std::memory_order_relaxed),(unsigned long long)pressure,
            (unsigned long long)hookCalls.load(std::memory_order_relaxed),(unsigned long long)(fbLive/1024),
            (unsigned long long)(fallbackSmall.load(std::memory_order_relaxed)/1024),
            (unsigned long long)(fallbackUntracked.load(std::memory_order_relaxed)/1024),
            (unsigned long long)placedTop.load(std::memory_order_relaxed),
            (unsigned long long)placedBand.load(std::memory_order_relaxed),
            (unsigned long long)placedAny.load(std::memory_order_relaxed),
            (unsigned long long)borrowedMaps.load(std::memory_order_relaxed),
            (unsigned long long)refusalTrims.load(std::memory_order_relaxed),
            (unsigned long long)(heldSmall.load(std::memory_order_relaxed)/1024),
            (unsigned long long)(heldBytes.load(std::memory_order_relaxed)/1024),
            !ORBIS_MMAP_TRIM ? "off (build)" : trimState.load(std::memory_order_relaxed)==2 ? "off (flag file)" : "on");
#else
  orbis_log("mem mmap-direct [%s]: DISABLED at build time - every anonymous mapping goes to "
          "flexible memory (ORBIS_MMAP_DIRECT=0)",where);
#endif
  }

// Reads the carve-out table without the lock because the caller is a signal handler: a thread
// that faults while holding the lock (e.g. inside malloc) would wait on itself forever.
//
// The unlocked read is sound: `carves` only grows, and addCarveLocked writes a carve-out's va,
// size and first block before publishing it, so a racing reader either misses the newest
// carve-out or sees it whole. The block list can be observed torn during a concurrent split
// (allocInLocked memmoves entries), which can only give a wrong block offset in a diagnostic
// line; nothing is dereferenced.
//
// With ORBIS_MMAP_TRIM a slot can be emptied and refilled. Each change happens between two bumps
// of the slot's `gen` (odd while in progress), and a trim publishes the odd value before
// unmapping. A reader that saw an odd value, or a different value after its reads, skips the
// slot.
bool orbis::mmapDirectDescribe(const void* addr, MmapDirectSpan& out) {
#if ORBIS_MMAP_DIRECT
  const uint8_t* p = static_cast<const uint8_t*>(addr);
  const uint32_t n = __atomic_load_n(&carves,__ATOMIC_ACQUIRE);
  for(uint32_t i=0; i<n && i<MaxCarves; ++i) {
    const Carve&   c    = carve[i];
    const uint32_t gen  = __atomic_load_n(&c.gen,__ATOMIC_ACQUIRE);
    if((gen&1)!=0)
      continue;
    const uint64_t size = __atomic_load_n(&c.size,__ATOMIC_ACQUIRE);
    const uint8_t* va   = __atomic_load_n(&c.va,__ATOMIC_RELAXED);
    if(size==0 || va==nullptr || p<va || p>=va+size)
      continue;
    MmapDirectSpan span{};
    span.index  = i+1;
    span.base   = va;
    span.size   = size;
    span.offset = uint64_t(p-va);
    const uint32_t bn = __atomic_load_n(&c.n,__ATOMIC_RELAXED);
    for(uint32_t b=0; b<bn && b<MaxBlocks; ++b) {
      const Block& blk = c.blk[b];
      if(span.offset<blk.off || span.offset>=blk.off+blk.size)
        continue;
      span.blockFound = true;
      span.blockUsed  = (blk.used!=0);
      span.blockOff   = blk.off;
      span.blockSize  = blk.size;
      break;
      }
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if(__atomic_load_n(&c.gen,__ATOMIC_RELAXED)!=gen)
      continue;
    out = span;
    return true;
    }
#else
  (void)addr;
#endif
  (void)out;
  return false;
  }

void orbis::mmapDirectStats(MmapDirectStats* out) noexcept {
  if(out==nullptr)
    return;
  MmapDirectStats s{};
  s.cap = effectiveCap();
#if ORBIS_MMAP_DIRECT
  s.held           = heldBytes.load(std::memory_order_relaxed);
  s.live           = liveBytes.load(std::memory_order_relaxed);
  s.peak           = peakLive.load(std::memory_order_relaxed);
  s.fallback_bytes = fallbackBytes.load(std::memory_order_relaxed);
  s.carves         = carvesHeld.load(std::memory_order_relaxed);
  s.critical       = heapCritical.load(std::memory_order_relaxed);
#if ORBIS_MMAP_TRIM
  s.trim_enabled   = trimEnabled() ? 1u : 0u;
#endif
#endif
  s.small_heap       = smallHeapBytes.load(std::memory_order_relaxed);
  s.fallback_live    = fallbackLive.load(std::memory_order_relaxed);
  s.fallback_small   = fallbackSmall.load(std::memory_order_relaxed);
  s.reserve          = reserveBytes.load(std::memory_order_relaxed);
  s.refusals         = refusalsAll.load(std::memory_order_relaxed);
  s.reserve_refusals = reserveRefusals.load(std::memory_order_relaxed);
  s.pressure_events  = pressureEvents.load(std::memory_order_relaxed);
  s.trimmed          = trimmedBytes.load(std::memory_order_relaxed);
  s.held_small       = heldSmall.load(std::memory_order_relaxed);
  s.last_refusal_us  = lastRefusalUs.load(std::memory_order_relaxed);
  *out = s;
  }

void orbis::mmapDirectSetCap(unsigned long long bytes) {
  const uint64_t prev = runtimeCap.exchange(uint64_t(bytes),std::memory_order_relaxed);
  if(prev==uint64_t(bytes) || !settingLogDue(capSetUs,prev,uint64_t(bytes)))
    return;
  orbis_log("mem mmap-direct: run-time cap %llu KiB (build cap %llu KiB, %llu KiB held now)",
          (unsigned long long)(effectiveCap()/1024),(unsigned long long)(DirectCap/1024),
          (unsigned long long)(heldBytes.load(std::memory_order_relaxed)/1024));
  }

unsigned long long orbis::mmapDirectTrim(unsigned keep) {
#if ORBIS_MMAP_DIRECT && ORBIS_MMAP_TRIM
  if(!trimEnabled()) {
#ifdef ORBIS_MMAP_TRIM_OFF_FLAG
    if(trimOffLogged.exchange(1,std::memory_order_relaxed)==0)
      orbis_log("mem mmap-direct: trim is off for this process (%s exists); carve-outs are kept",
              ORBIS_MMAP_TRIM_OFF_FLAG);
#endif
    return 0;
    }
  TrimResult r{};
    {
    Guard g;
    trimLocked(keep,r);
    }
  logTrim(r,keep,"");
  return r.released;
#else
  (void)keep;
  return 0;
#endif
  }

void orbis::mmapDirectSetReserve(unsigned long long bytes) noexcept {
  // The probe runs here, outside the allocator lock, before an admission needs the ledger.
  if(ledgerMode.load(std::memory_order_acquire)==0)
    directLedger(nullptr,250);
  const uint64_t prev = reserveBytes.exchange(uint64_t(bytes),std::memory_order_relaxed);
  if(prev==uint64_t(bytes) || !settingLogDue(reserveSetUs,prev,uint64_t(bytes)))
    return;
  orbis_log("mem mmap-direct: reserve for the other direct-memory owners %llu KiB (was %llu KiB); new carve-outs "
          "must leave it free; %llu KiB held, live %llu KiB",
          (unsigned long long)(bytes/1024),(unsigned long long)(prev/1024),
          (unsigned long long)(heldBytes.load(std::memory_order_relaxed)/1024),
          (unsigned long long)(liveBytes.load(std::memory_order_relaxed)/1024));
  }

void orbis::mmapDirectClearCritical() noexcept {
  heapCritical.store(0,std::memory_order_relaxed);
  }

orbis::MmapPressureHook orbis::setPressureHook(MmapPressureHook hook) noexcept {
  return pressureHook.exchange(hook,std::memory_order_acq_rel);
  }

void orbis::directLedgerSetExternal(unsigned long long bytes) noexcept {
  ledgerExternal.store(uint64_t(bytes),std::memory_order_relaxed);
  ledgerExternalSet.store(1,std::memory_order_release);
  }

bool orbis::directLedger(DirectLedger* out, unsigned max_age_ms) noexcept {
  char        line[512];
  LedgerState s{};
  line[0] = '\0';
    {
    LedgerGuard g;
    if(ledgerMode.load(std::memory_order_acquire)==0) {
      probeLedgerLocked(line,sizeof(line));
      emitProbeLocked(line);
      }
    const uint64_t now = nowUs();
    if(ledgerCache.sampledUs==0 || ledgerDirty.load(std::memory_order_relaxed)!=0 ||
       now-ledgerCache.sampledUs>=uint64_t(max_age_ms)*1000)
      refreshLedgerLocked();
    s = ledgerCache;
    }
  if(probePending.load(std::memory_order_acquire)!=0 && orbis_log_enabled() &&
     probePending.exchange(0,std::memory_order_acq_rel)!=0)
    orbis_log("%s",probeLine);
  if(out==nullptr)
    return false;
  DirectLedger l{};
  l.total          = s.total;
  l.allocated      = s.allocated;
  l.free_total     = s.freeTotal;
  l.largest_run    = s.largest;
  l.heap_held      = heldBytes.load(std::memory_order_relaxed);
  l.boot_allocated = ledgerBoot.load(std::memory_order_relaxed);
  l.reserve        = reserveBytes.load(std::memory_order_relaxed);
  l.sampled_us     = s.sampledUs;
  l.walk_us        = s.walkUs;
  l.entries        = s.entries;
  l.rc             = s.rc;
  l.estimated      = s.estimated;
  *out = l;
  return true;
  }

void orbis::directLedgerReport(const char* where) noexcept {
  DirectLedger l{};
  directLedger(&l,0);
  const uint64_t accounted = l.boot_allocated+l.heap_held;
  orbis_log("mem ledger [%s]: direct %llu KiB = allocated %llu KiB (boot %llu, heap carve-outs %llu, other %llu) + "
          "free %llu KiB; largest free run %llu KiB; %u range(s) in %llu us; reserve %llu KiB; %s (%llu untrusted "
          "walk(s))",
          where,(unsigned long long)(l.total/1024),(unsigned long long)(l.allocated/1024),
          (unsigned long long)(l.boot_allocated/1024),(unsigned long long)(l.heap_held/1024),
          (unsigned long long)((l.allocated>accounted ? l.allocated-accounted : 0)/1024),
          (unsigned long long)(l.free_total/1024),(unsigned long long)(l.largest_run/1024),l.entries,
          (unsigned long long)l.walk_us,(unsigned long long)(l.reserve/1024),
          l.estimated ? "ESTIMATED" : "kernel walk",
          (unsigned long long)ledgerWalkFailures.load(std::memory_order_relaxed));
  }

// ------------------------------------------------------------------ the interposition

/* Pass-through counters.
 *
 * Measured on hardware 2026-08-31 by reading libkernel's internal-memory meter at points around the
 * frame: 9073 of 9280 bytes per frame are lost above the graphics driver (frontend between frames,
 * and the draw/translate/record path); the winsys costs 0-8 bytes/ms and each of its sce* calls
 * measures zero in isolation.
 *
 * Bytes cannot leave that pool without a libkernel call. Mesa's malloc reaches musl and then this
 * file: requests served from carve-outs make no syscall, the rest fall through to the platform's
 * mmap/munmap below, which were not counted before.
 *
 * This matches the one comparison available: RetroArch's Vulkan driver (buffer chain and descriptor
 * pools grown once, reset per frame) does not leak, while zink (general caches whose misses become
 * malloc traffic) does.
 *
 * So the fall-throughs are counted, and orbis::internalMemoryProbe measures a mmap/munmap pair and a
 * malloc/free pair directly. Relaxed atomics: read every few seconds by a report. */
static unsigned long long mmapCalls, mmapCarved, mmapFell;
static unsigned long long munmapCalls, munmapCarved, munmapFell;

static inline void mmBump(unsigned long long* c) {
  __atomic_fetch_add(c,1ull,__ATOMIC_RELAXED);
  }

extern "C" void orbis_mmap_counts(unsigned long long* maps, unsigned long long* maps_carved,
                                  unsigned long long* maps_fell, unsigned long long* unmaps,
                                  unsigned long long* unmaps_carved, unsigned long long* unmaps_fell) {
  if(maps         !=nullptr) *maps         = __atomic_load_n(&mmapCalls,  __ATOMIC_RELAXED);
  if(maps_carved  !=nullptr) *maps_carved  = __atomic_load_n(&mmapCarved, __ATOMIC_RELAXED);
  if(maps_fell    !=nullptr) *maps_fell    = __atomic_load_n(&mmapFell,   __ATOMIC_RELAXED);
  if(unmaps       !=nullptr) *unmaps       = __atomic_load_n(&munmapCalls,__ATOMIC_RELAXED);
  if(unmaps_carved!=nullptr) *unmaps_carved= __atomic_load_n(&munmapCarved,__ATOMIC_RELAXED);
  if(unmaps_fell  !=nullptr) *unmaps_fell  = __atomic_load_n(&munmapFell, __ATOMIC_RELAXED);
  }


namespace {

// An owned request: from musl's large-allocation path (small = false) or its small-heap growth.
void* ownedMap(size_t len, bool small) {
  mmBump(&mmapCalls);
  banner(ORBIS_MMAP_DIRECT!=0);
#if ORBIS_MMAP_DIRECT
  mmBump(&mmapCarved);
  return mapOwned(len,small);
#else
  (void)small;
  // Same as musl's own __mmap: mmap.lo is just `jmp mmap`.
  mmBump(&mmapFell);
  return mmap(nullptr,len,OwnedProt,OwnedFlags,-1,0);
#endif
  }

}

extern "C" void* __mmap(void* addr, size_t len, int prot, int flags, int fd, off_t off) {
  /* The early return is a fall-through too (file mappings, fixed mappings...). */
  if(!isOwnedRequest(addr,prot,flags,fd,off)) {
    mmBump(&mmapCalls);
    mmBump(&mmapFell);
    return mmap(addr,len,prot,flags,fd,off);
    }
  return ownedMap(len,false);
  }

#if ORBIS_MMAP_EXPAND_HEAP
// musl's small-heap growth. llvm-nm on the SDK's libc.a: expand_heap.lo defines only `T __expand_heap`
// and its static `b mmap_step` (it needs `__mmap` and `___errno_location`); malloc.lo and lite_malloc.lo
// reference it. Defining it here keeps expand_heap.lo out of the link, as with `__mmap`.
//
// Its contract, from the disassembly (this SDK's build has no brk path):
//
//   n = *pn; n >= 0x7fffffffffffc000 -> errno = ENOMEM, return 0     (n > SIZE_MAX/2 - PAGE_SIZE)
//   n rounded up to 16 KiB; n = max(n, 0x4000 << (mmap_step/2))      (shlq %cl: shift count mod 64)
//   __mmap(0, n, 3, 0x1002, -1, 0) == MAP_FAILED -> return 0          (*pn and mmap_step untouched)
//   *pn = n; ++mmap_step; return the mapping
//
// malloc calls it under its heap_lock, merges a mapping that starts at the previous heap end and puts a
// sentinel chunk below any other one, so any 16 KiB multiple at any page-aligned address is valid. The
// step doubles every second call and nothing is ever returned: 256 MiB steps past a 512 MiB heap leave
// up to 256 MiB committed and unused, and a 512 MiB step past 1 GiB needs a carve-out the ladder cannot
// take (every small malloc would then fail with free blocks still in the carve-outs). A capped step is
// served from free carve-out blocks instead. The step counter is atomic because lite_malloc's copy, if
// linked, calls this under a different lock.
extern "C" void* __expand_heap(size_t* pn) {
  size_t n = *pn;
  if(n>SIZE_MAX/2-size_t(Page)) {
    errno = ENOMEM;
    return nullptr;
    }
  n += (size_t(0)-n) & size_t(Page-1);
  const uint32_t half = expandSteps.load(std::memory_order_relaxed)/2;
  uint64_t       min  = 0;
  if(ExpandStepMax==0) {
    min = Page << (half & 63);
    } else {
    min = Page;
    for(uint32_t i=0; i<half && min<ExpandStepMax; ++i)
      min <<= 1;
    if(min>ExpandStepMax)
      min = ExpandStepMax;
    }
  if(uint64_t(n)<min)
    n = size_t(min);
  // The same request __mmap would see, marked as small-heap growth (segregation, pressure hook).
  void* area = ownedMap(n,true);
  if(area==MAP_FAILED)
    return nullptr;
  *pn = n;
  expandSteps.fetch_add(1,std::memory_order_relaxed);
  smallHeapBytes.fetch_add(uint64_t(n),std::memory_order_relaxed);
  return area;
  }
#endif

extern "C" int __munmap(void* addr, size_t len) {
  mmBump(&munmapCalls);
    {
    Guard g;
    if(Carve* c = carveOfLocked(addr)) {
      if(freeInLocked(*c,reinterpret_cast<uint8_t*>(addr),alignUp(uint64_t(len),Page))) {
        mmBump(&munmapCarved);
        return 0;
        }
      errno = EINVAL;
      return -1;
      }
    // A flexible-memory fallback (if it is one) is no longer live.
    dropFallbackLocked(addr);
    }
  mmBump(&munmapFell);
  return munmap(addr,len);
  }

// MADV_DONTNEED has no defined meaning for physically backed direct memory, and musl issues
// it on the interior of every large freed span. Return success (musl never checks it).
extern "C" int __madvise(void* addr, size_t len, int advice) {
    {
    Guard g;
    if(carveOfLocked(addr)!=nullptr)
      return 0;
    }
  return madvise(addr,len,advice);
  }
