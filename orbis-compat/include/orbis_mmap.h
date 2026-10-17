// Copyright © 2026 Mikołaj Mikołajczyk
// SPDX-License-Identifier: MIT
// musl's anonymous mappings, served out of direct memory instead of flexible memory.
//
// ------------------------------------------------------------------ why this exists
//
// A PS4 process has two disjoint budgets (see orbis_mem.h). malloc grows into flexible
// memory, which gave the title 427008 KiB at boot; loading a large world needed more than
// 405 MiB of it and ran out while direct memory still had 4601856 KiB free (measured
// 2026-08-04). This file lets malloc use direct memory, without changing the application.
//
// ------------------------------------------------------------------ what is interposed
//
// Not `mmap`, but three internal names that only musl's allocator uses (a fourth, the
// small-heap growth `__expand_heap`, only with ORBIS_MMAP_EXPAND_STEP_MAX_MIB; see "build
// knobs"). In the SDK's libc.a:
//
//   mmap.lo      `__mmap`    is one instruction: `jmp mmap`  (R_X86_64_PLT32 mmap-0x4)
//   munmap.lo    `__munmap`  is one instruction: `jmp munmap`
//   madvise.lo   `__madvise` is one instruction: `jmp madvise`
//
// and the callers:
//
//   expand_heap.lo  U __mmap                            heap growth
//   malloc.lo       U __mmap __munmap __madvise __mremap large allocations
//   __map_file.lo   U __mmap                            locale/timezone, file-backed
//   __tz.lo, locale_map.lo, dcngettext.lo  U __munmap    the unmap half of the above
//
// Nothing else in libc.a, libc++.a, libc++abi.a, libunwind.a or the crt objects references
// them (llvm-nm). Other users (archive mmap, `__init_tls`, `catopen`) call plain
// `mmap`/`munmap` and never reach this file.
//
// mmap.lo defines `__mmap` plus weak `mmap64` and `__vm_wait`, which nothing references,
// so defining `__mmap` in the executable keeps mmap.lo out of the link: no duplicate
// symbol, and libkernel's `mmap` stays reachable by name. munmap.lo and madvise.lo are
// the same. The forward path calls the same function musl jumped to, rather than
// reimplementing it over sceKernelMmap. If a future SDK defines these names elsewhere,
// the link fails with a duplicate symbol.
//
// ------------------------------------------------------------------ what is taken
//
// Only the exact request musl's two allocator call sites emit:
//
//   addr == nullptr, prot == PROT_READ|PROT_WRITE (3), flags == MAP_PRIVATE|MAP_ANON
//   (0x1002) with no other bit set, fd < 0, offset == 0
//
// (malloc+0x6b..0x83 and __expand_heap+0x5c..0x6a: `movl $3,%edx; movl $0x1002,%ecx;
// movl $-1,%r8d; xorl %r9d,%r9d`). Anything else (file-backed, MAP_FIXED, an address
// hint, PROT_EXEC) is forwarded to the real `mmap` unexamined.
//
// ------------------------------------------------------------------ granularity
//
// Direct memory is allocated in 2 MiB units. musl's MMAP_THRESHOLD is 0x38000 = 224 KiB
// (`cmpq $0x38001,%rbx` at malloc+0x51) and mappings are rounded to 16 KiB pages
// (`addq $0x400f; andq $-0x4000`), so the smallest anonymous mapping is 240 KiB. A size
// threshold would leave all large allocations on the flexible pool, and one 2 MiB block
// per mapping would waste 88%. So: 128 MiB carve-outs, first-fit from an address-sorted
// block list per carve-out, 16 KiB granularity (2 MiB bases are 16 KiB aligned;
// static_asserted).
//
// ------------------------------------------------------------------ munmap and reuse
//
// musl always unmaps exactly what it was given (free+0x20 recomputes the base from the
// chunk header; realloc does the same). Blocks are freed on an exact (base, length) match
// and coalesced with both neighbours; a partial or unaligned unmap inside a carve-out is
// reported and the block is leaked rather than guessed at.
//
// Recycled blocks are zeroed before reuse. musl's calloc skips clearing for mmapped
// chunks (`testb $0x1,-0x8(%rbx); je` at calloc+0x5f) because MAP_ANON memory is zero
// filled; a dirty block would make calloc() return garbage. It is unconditional because
// it is not documented whether sceKernelAllocateDirectMemory returns zeroed pages.
//
// `__mremap` needs nothing: mremap.lo is a stub returning -1/ENOSYS (78), so realloc of a
// mapped chunk falls back to malloc + memcpy + __munmap.
//
// `__madvise`: musl's __bin_chunk releases the interior of a large freed span with
// `__madvise(a, b-a, 4)` (MADV_DONTNEED). Direct memory is physical and cannot be
// reclaimed by advice, so it is a no-op returning success inside our carve-outs and
// forwarded everywhere else.
//
// ------------------------------------------------------------------ GNM allocator coexistence
//
// The GNM allocator also takes direct memory: a 16 MiB onion and a 96 MiB garlic arena,
// at most 4 arenas per bus, so at most 4*16 + 4*96 = 448 MiB unless a single allocation
// exceeds an arena.
//
// This file is capped at 1536 MiB, leaving 2.89 GiB of the 4.39 GiB measured free. The
// pool grows on demand, one 128 MiB carve-out at a time, because direct memory is
// committed in full when taken. Every carve-out logs its size and physical address.
//
// Carve-outs are not released by default: musl never returns heap-expansion mappings, so
// the first one never empties, and a wrong release path would hand out pages that are no
// longer ours. The high-water figures in the report size any future reclaim.
//
// ------------------------------------------------------------------ build knobs (all opt-in)
//
// Each defaults to the behaviour described above.
//
//   ORBIS_MMAP_MAX_CARVES=N      slot count (default 12).
//   ORBIS_MMAP_DIRECT_CAP_MIB=N  cap on direct memory held, in MiB (default: slots x 128 MiB).
//                                Lets the slot count grow without raising the budget.
//   ORBIS_MMAP_CARVE_LADDER=1    a refused carve-out is retried at half the length, down to
//                                max(need, 32 MiB); a carve-out that would cross the cap is
//                                shrunk to fit; once, a carve-out of at most 128 MiB may go
//                                past the cap (overdraft), which marks the heap critical.
//   ORBIS_MMAP_TRIM=1            mmapDirectTrim() returns carve-outs that are entirely free.
//                                Without it the function is a no-op that returns 0.
//   ORBIS_MMAP_EXPAND_STEP_MAX_MIB=N
//                                defines musl's `__expand_heap` here (expand_heap.lo then stays out
//                                of the link) with each small-heap growth step capped at N MiB.
//                                musl's own step is 16 KiB << (step/2): 256 MiB past a 512 MiB heap,
//                                512 MiB past 1 GiB, never returned. 0 keeps musl's formula and only
//                                counts (MmapDirectStats::small_heap).
//   ORBIS_MMAP_FLEX_FLOOR_MIB=N  a request the carve-outs cannot serve is refused (MAP_FAILED, ENOMEM)
//                                instead of falling back to flexible memory when the fallback would
//                                leave less than N MiB of it; libkernel takes thread stacks from that
//                                pool. Logged once, counted in the report, marks the heap critical.
//   ORBIS_MMAP_HEAP_BAND_MIB=N   physical placement of new carve-outs at the top of the pool: the top of
//                                the highest free run that fits (from the ledger's walk; the first carve-out
//                                probes the ledger), else first fit in [total - N MiB, total), else first
//                                fit anywhere. Every other direct-memory user allocates first fit from 0, so
//                                the two meet only when the pool is full instead of interleaving. Undefined
//                                or 0: first fit over the whole pool.
//   ORBIS_MMAP_SEGREGATE=1       musl's small-heap steps (__expand_heap) and large mappings live in
//                                separate carve-outs: steps first fit in small-heap carve-outs (filled
//                                from one end), large mappings best fit in large ones, and a mapping above
//                                ORBIS_MMAP_DEDICATED_MIB (default 32) gets a carve-out of its own size
//                                while at least 8 slots are free. A kind borrows the other kind's free
//                                blocks only when no new carve-out can be taken. Large carve-outs empty
//                                out when their mappings are freed, so TRIM can return them.
//   ORBIS_MMAP_TRIM_OFF_FLAG="path"   with TRIM, an existing file at `path` turns trimming and SEGREGATE
//                                off for the process (checked once, at the first request).
//   ORBIS_MMAP_QUERY_OFF_FLAG="path"  an existing file at `path` keeps the ledger off
//                                sceKernelDirectMemoryQuery (estimated ledger, see DirectLedger).
//
// With ORBIS_MMAP_CARVE_LADDER, an overdraft that fails at every ladder size down to the 32 MiB
// floor is not retried: physical direct memory is short, and each retry would cost three failing
// sceKernelAllocateDirectMemory calls under the allocator lock. The overdraft is also refused while it
// would leave less than max(reserve, 256 MiB) of direct memory free (see mmapDirectSetReserve).
//
// With ORBIS_MMAP_TRIM, a request that no carve-out can serve and that cannot get a new one first
// returns the carve-outs that are entirely free (they are too small for it) and tries once more.
//
// ------------------------------------------------------------------ thread safety
//
// The block lists are protected by a spin/yield lock, not a pthread mutex: a statically
// initialised mutex in a FreeBSD-derived libthr allocates on first lock, which would
// recurse into malloc while malloc's own lock is held.
//
// Nothing on any path here allocates (fixed-capacity arrays in .bss). `orbis_log` is safe:
// it formats into a 512-byte stack buffer and musl's vsnprintf/vfprintf do not allocate.
//
// ------------------------------------------------------------------ turning it off
//
// ORBIS_MMAP_DIRECT=0 at build time (build.sh --define ORBIS_MMAP_DIRECT=0) makes the
// interposer a plain `return mmap(...)`, identical to musl's `__mmap`. The active path is
// logged at the first anonymous mapping and in every census line.
#pragma once

#include <cstddef>

#ifndef ORBIS_MMAP_DIRECT
// On by default: the fix should not depend on every build remembering to enable it.
#define ORBIS_MMAP_DIRECT 1

/* How much of musl's allocation traffic reaches libkernel. Requests served from carve-outs cost no
   syscall; the rest fall through to the platform's mmap/munmap, the only libkernel calls made by the
   frontend's and zink's allocations. Used to attribute per-frame internal-memory loss. Any pointer
   may be null. */
extern "C" void orbis_mmap_counts(unsigned long long* maps, unsigned long long* maps_carved,
                                  unsigned long long* maps_fell, unsigned long long* unmaps,
                                  unsigned long long* unmaps_carved, unsigned long long* unmaps_fell);

#endif

namespace orbis {

// One line of interposer state, appended to a census. `where` is printed verbatim and
// should be the same phase name the census used. Allocates nothing and is safe to call
// after malloc has started returning null.
void mmapDirectReport(const char* where);

// What a single address is, in this file's terms. Filled by mmapDirectDescribe below.
//
// `index` numbers the carve-out as the "carve-out N taken" log line does. The block fields
// describe the suballocated block the address falls in; `blockUsed == false` means the
// address is inside freed memory, i.e. a use-after-free.
struct MmapDirectSpan {
  unsigned           index;      // 1-based, as the banner prints it
  const void*        base;       // carve-out base
  unsigned long long size;       // carve-out size in bytes
  unsigned long long offset;     // addr - base
  bool               blockFound;
  bool               blockUsed;  // false == the address is inside memory that was FREED
  unsigned long long blockOff;   // block start, as an offset into the carve-out
  unsigned long long blockSize;
  };

// True if `addr` falls inside a carve-out this file handed out, with `out` filled in.
//
// Safe to call from a signal handler (unlike mmapDirectReport): no lock, no allocation.
// See the definition for why reading the table unlocked is sound.
bool mmapDirectDescribe(const void* addr, MmapDirectSpan& out);

// Counters of the direct-memory heap, for a caller that budgets memory (for example a
// compile governor that runs fewer compiles at once when the heap gets close to its cap).
// Relaxed atomic loads only: no lock, no allocation, callable from any thread. With
// ORBIS_MMAP_DIRECT=0 every field but `cap` and `small_heap` reads 0.
struct MmapDirectStats {
  unsigned long long held;           // direct memory held in carve-outs, bytes
  unsigned long long live;           // bytes handed to musl and not unmapped yet
  unsigned long long peak;           // high-water mark of `live`
  unsigned long long cap;            // budget for `held` (build cap, or a lower run-time cap)
  unsigned long long fallback_bytes; // bytes that had to come from flexible memory instead
  unsigned           carves;         // carve-outs currently held
  unsigned           critical;       // 1 once the heap reached its cap (overdraft or refusal) or
                                     // the flexible floor refused a request
  // Bytes musl's small heap grew by (never returned); included in `live` unless a step fell back to
  // flexible memory. 0 when ORBIS_MMAP_EXPAND_STEP_MAX_MIB is undefined (musl's own __expand_heap).
  unsigned long long small_heap;
  // Flexible-memory fallback bytes still mapped: falls when musl unmaps a fallback mapping (fallback_bytes
  // above only grows). Includes fallback_small, which musl never unmaps.
  unsigned long long fallback_live;
  unsigned long long fallback_small;   // small-heap steps served from flexible memory
  unsigned long long reserve;          // mmapDirectSetReserve
  unsigned long long refusals;         // new carve-outs refused (cap, slots, reserve, kernel), cumulative
  unsigned long long reserve_refusals; // ...of which by the reservation
  unsigned long long pressure_events;  // requests no carve-out could serve (hook, fallback or ENOMEM next)
  unsigned long long trimmed;          // bytes returned to the direct pool by trimming, cumulative
  unsigned long long held_small;       // ORBIS_MMAP_SEGREGATE: part of `held` in small-heap carve-outs
  unsigned long long last_refusal_us;  // sceKernelGetProcessTime of the latest refusal or pressure event
  unsigned           trim_enabled;     // 1 while mmapDirectTrim can return carve-outs
  };
void mmapDirectStats(MmapDirectStats* out) noexcept;

// Lowers the cap at run time, in bytes; 0 goes back to the build cap. It never raises the
// cap and never releases anything already held: later carve-outs just refuse sooner. Used by
// forced out-of-memory tests and by a budget authority that moves it every second (a repeated
// value is silent; a change is logged at most once a minute, except the first and a return to 0).
void mmapDirectSetCap(unsigned long long bytes);

// Unmaps and releases every carve-out whose whole range is free again, except the first
// `keep` slots, and returns the bytes given back. Only with ORBIS_MMAP_TRIM=1 and no
// ORBIS_MMAP_TRIM_OFF_FLAG file; otherwise it does nothing and returns 0. Takes the allocator
// lock: not for signal handlers.
unsigned long long mmapDirectTrim(unsigned keep);

// ------------------------------------------------------------------ direct-memory authority
//
// The process's direct-memory pool is shared by this heap, the GPU driver, guest memory, JIT
// commits, thread stacks and system allocations, all first fit from one pool. The functions below
// let a budget authority keep a reservation for the other owners: the heap takes a new carve-out
// only while the pool keeps that much free, and the authority hears about every request the heap
// could not serve.

// The pool as the kernel sees it. From a sceKernelDirectMemoryQuery walk (FIND_NEXT over every
// allocated range of this process) when the export exists and its first walk agrees with
// sceKernelAvailableDirectMemorySize; otherwise estimated: free_total = total - boot_allocated -
// heap_held - external (directLedgerSetExternal), and never less than the largest free run.
struct DirectLedger {
  unsigned long long total;          // sceKernelGetDirectMemorySize
  unsigned long long allocated;      // bytes in allocated ranges inside [0, total)
  unsigned long long free_total;     // total - allocated
  unsigned long long largest_run;    // largest free run
  unsigned long long heap_held;      // this file's carve-outs (part of `allocated`)
  unsigned long long boot_allocated; // `allocated` at the probe less the heap's carve-outs then
  unsigned long long reserve;        // mmapDirectSetReserve
  unsigned long long sampled_us;     // sceKernelGetProcessTime of the sample
  unsigned long long walk_us;        // duration of the walk (0 when estimated)
  unsigned           entries;        // allocated ranges seen by the walk (0 when estimated)
  int                rc;             // query result that ended the walk (0 = reached the end of the pool)
  unsigned           estimated;      // 1 when the numbers are not from a walk
  };

// Fills `out` from a sample at most `max_age_ms` old, walking again otherwise (and after the heap
// took or returned a carve-out). The probe (first use, here or at the first carve-out that needs the
// ledger) logs one line: "mem ledger: query rc=..., N entries, ... - ledger ON (kernel walk)" or
// "... - ledger ESTIMATED: <why>" (a probe made before orbis_set_log is logged by the first call after it).
// Allocation-free; takes the ledger lock, never the allocator lock,
// so any thread may call it, including a pressure hook (not a signal handler); it can wait for a
// carve-out being taken. A walk costs one syscall per allocated range (at most 4096; a longer or
// disagreeing walk gives an estimated sample, and 8 such samples in a row keep the ledger estimated).
// Returns false if out is null.
bool directLedger(DirectLedger* out, unsigned max_age_ms = 250) noexcept;

// One census line with the ledger, after mmapDirectReport's lines. Same rules as directLedger.
void directLedgerReport(const char* where) noexcept;

// Direct memory held by the owners this file cannot see (guest memory, JIT, GPU, own stacks), in
// bytes; only used while the ledger is estimated. Any thread.
void directLedgerSetExternal(unsigned long long bytes) noexcept;

// Direct memory a NEW carve-out must leave free for the other owners, in bytes; 0 (the default)
// turns the test off. A carve-out of L bytes is taken only while ledger.free_total - L >= reserve,
// from a sample at most 100 ms old with the heap's own later carve-outs applied (a walk under the
// allocator lock at most every 100 ms); with the ladder it shrinks to fit, not below its floor. The
// single overdraft past the cap (ladder) passes the same test with max(reserve, 256 MiB), also when
// no reserve is set. A refusal marks the heap critical, is logged at most once a minute with the
// numbers, and the request goes on: the other kind's free blocks (SEGREGATE), trimming, the pressure
// hook, flexible memory above its floor. The first call probes the ledger. Any thread; a repeated
// value is silent.
void mmapDirectSetReserve(unsigned long long bytes) noexcept;

// Clears the critical flag (the counters stay), for a governor whose levels are not sticky.
void mmapDirectClearCritical() noexcept;

// Why a request reached the pressure hook (bits).
enum : unsigned {
  MmapPressureSmallHeap = 1u,  // musl small-heap growth: malloc's heap lock is held by this thread
  MmapPressureReserve   = 2u,  // a new carve-out would cut into the reservation
  MmapPressureCap       = 4u,  // a new carve-out would pass the cap, or every slot is taken
  MmapPressureKernel    = 8u,  // sceKernelAllocateDirectMemory / sceKernelMapDirectMemory failed
  };

// Called with the request size (bytes) when no carve-out can serve a request, before the
// flexible-memory fallback and its floor. The allocator lock is not held. The hook must not
// allocate and must not wait for other threads to free memory when MmapPressureSmallHeap is set
// (they would need the lock this thread holds); it may free (a freed carve-out block serves the
// retry) and should return within ~200 ms. The carve-outs are tried once more after it returns.
// Not re-entered on a thread that is already inside it. Returns the previous hook; null removes it.
using MmapPressureHook = void (*)(unsigned long long bytes, unsigned why);
MmapPressureHook setPressureHook(MmapPressureHook hook) noexcept;

}
