// Copyright © 2026 Mikołaj Mikołajczyk
// SPDX-License-Identifier: MIT
// Reports which of the PS4's two memory pools a title has run out of.
//
// A PS4 process has two pools that never borrow from each other:
//
//   direct memory    physical, allocated with sceKernelAllocateDirectMemory and mapped
//                    explicitly (GNM allocator arenas live here).
//   flexible memory  what plain mmap(MAP_ANON) draws from, so what musl's malloc grows
//                    into: __expand_heap() is mmap(0,len,PROT_READ|PROT_WRITE,
//                    MAP_PRIVATE|MAP_ANON,-1,0) (SDK libc.a, expand_heap.lo), and requests
//                    above musl's mmap threshold get their own mapping. A separate, much
//                    smaller per-process ceiling.
//
// So std::bad_alloc means the flexible pool is exhausted, even while the direct pool is
// idle (e.g. a 96 MiB direct arena was allocated one second after a title failed with
// "out of memory").
//
// Nothing here allocates: the OOM report runs after malloc has already returned null.
#pragma once

namespace orbis {

// Record and print the flexible/direct figures at startup. Call once, as early as
// possible; later censuses report consumption relative to this baseline.
void memCensusBaseline();

// One line naming both pools at a point the caller wants attributed. `where` is printed
// verbatim, so it should be a phase name ("world zen parsed"), not a sentence.
void memCensus(const char* where);

/// Thread interposer statistics in the same format as the memory census, including the extra
/// address space reserved by raised thread stacks.
void memCensusThreads(const char* where);

/* Measures which primitive consumes libkernel's internal memory.
 *
 * The graphics driver loses ~146 bytes per expired syncobj wait (~81 per frame) until libkernel's
 * internal pool drops from 14013728 bytes to 96 and the process dies. Submits, flips, mprotect,
 * direct memory, usleep and the log sink were excluded; the failing path only locks a simple_mtx
 * and reads a clock twice.
 *
 * Each candidate primitive runs N times in isolation with sceKernelInternalMemoryGetAvailableSize
 * read before and after, next to a control loop, giving bytes per call. If every candidate is
 * clear, the allocation happens somewhere none of these counters reach.
 *
 * Off by default: it spends a few hundred KiB of the ~14 MiB pool. Enable with
 * ORBIS_INTERNAL_MEM_PROBE in the env file. */
void internalMemoryProbe();

}
