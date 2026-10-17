// Copyright © 2026 Mikołaj Mikołajczyk
// SPDX-License-Identifier: MIT
//
// One arena of writable, executable memory, shared by every recompiler in a module.
//
// Combines RetroArch/ps4/orbis_exec_mem.c (promotes pages a core already owns) and
// beetle-psx-libretro/ps4/orbis_lightrec_mem.c (maps a buffer up front), and adds
// suballocation so N recompilers in one module share one kernel arena.
//
// No W^X: the kernel's dual-mapping API (sceKernelJitCreateSharedMemory,
// CreateAliasOfSharedMemory, MapSharedMemory) is exported only by libkernel_sys/jvm/ps2emu/psmkit,
// not by lib/libkernel.so, which `-lkernel` links against, so referencing it is a link error.
// The SDK declares them without parameters.
//
// What works is map read-write, then promote with sceKernelMprotect. Asking
// sceKernelMapDirectMemory for READ|EXECUTE at map time returns 0x8002000d (EACCES). Everything
// handed out here is RWX for its whole lifetime.
//
// Consumers must disable any RW<->RX toggling (dynarmic's EnableWriting/DisableWriting,
// DYNARMIC_ENABLE_NO_EXECUTE_SUPPORT; paraLLEl-RSP's commit_execute setting PROT_EXEC alone):
// narrowing the protection may be refused and cannot be undone.
//
// No reserve-then-commit: executable pages come from direct memory, which commits physical pages
// at allocation. Consumers that reserve a large range and commit as they go (paraLLEl-RSP's
// jit_allocator, dynarmic's Windows arm) must request only what they will use.
//
// sceKernelQueryMemoryProtection reports the protection a range was mapped with, not the result of
// a later mprotect (it returned 0x03 for ranges executing code, 16 out of 16). So every range is
// verified by writing six bytes of x86-64 into it and calling them before it is returned.
#ifndef ORBIS_JIT_H
#define ORBIS_JIT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/// The unit sceKernelMprotect and sceKernelMapDirectMemory work in: 16 KiB, not 4 KiB
/// (swanstation's 4096-byte JIT guard page was rounded up to this and the recompiler's first write
/// landed inside it). Every length and address below is rounded outwards to it.
#define ORBIS_JIT_GRANULE 0x4000u

/// The allocation must be reachable by rel32 from this module's text. x86-64 emitters (Xbyak,
/// new_dynarec's emit_call, dolphin's x64Emitter) use 32-bit displacements for calls and jumps out
/// of generated code. flycast aborted with
///
///     libc++abi: terminating with uncaught exception of type Xbyak::Error: offset is too big
///
/// with a cache at 0x899200000 and the module at 0x800870000; with -DNDEBUG (libretro builds) the
/// asserts are gone and a truncated displacement jumps somewhere arbitrary.
///
/// With this flag the arena is placed by address search and MAP_FIXED inside the window around this
/// module; if that fails, NULL is returned rather than a far pointer.
#define ORBIS_JIT_NEAR_TEXT 0x1u

/// Suballocate `size` bytes of writable, executable memory. Returns NULL if the console refuses,
/// which callers must treat as "clamp to the interpreter" rather than as a transient error.
///
/// The arena grows on demand and is shared by every caller in the module. (paraLLEl-RSP mapped an
/// arena per module load and leaked it; on hardware the fourth load failed with
/// `no direct memory for 65536 KiB -> 0x80020023`, EAGAIN.)
void *orbis_jit_alloc(size_t size, unsigned flags);

/// Map a whole arena at exactly `addr`, for a caller that chooses its own layout - Lightrec's
/// custom map, which places the code buffer relative to its emulated address space.
///
/// MAP_FIXED on this kernel replaces existing mappings and there is no MAP_FIXED_NOREPLACE, so the
/// range is checked first and NULL is returned if anything is in the way.
void *orbis_jit_alloc_at(void *addr, size_t size);

/// Give a block back, from either allocator. Silently ignores addresses it never handed out (a
/// destructor after a failed construction is normal).
///
/// No length argument because Xbyak::Allocator's `void free(uint8_t*)` has none (dynarmic's POSIX
/// allocator wastes a page per allocation to store it, block_of_code.cpp:81-102); the length is
/// tracked here.
void orbis_jit_free(void *addr);

/// Promote pages the caller already owns, e.g. a code cache declared as a static array (mupen64plus,
/// flycast, melonDS). Returns 1 if code can be written and run there, 0 if not.
///
/// This also makes a large .bss writable: the module loader does not provide writable pages for
/// all of a 50 MB .bss (swanstation faulted with SEGV_ACCERR writing a static array 49 MB in).
///
/// Idempotent per range, which is required: the verification writes six bytes at the start of the
/// range, so promoting again would overwrite generated code. Overlapping ranges are merged (melonDS
/// asks twice with the same length and bases 224 KiB apart, 0x800e14000 and 0x800e4c000).
int orbis_jit_protect(void *addr, size_t len);

/// -1 nothing has asked yet, 0 the console refused, 1 there is executable memory.
///
/// Three values because callers choose their CPU mode before any buffer is requested; treating
/// "not asked" as "refused" would silently fall back to the interpreter. This is the AND over every
/// range requested in the module and never improves: one refused buffer makes the recompiler
/// unusable.
int orbis_jit_state(void);

/// Whether a rel32 from anywhere in [from, from+len) can reach anywhere in [to, to+len2), for a
/// caller that wants to check rather than to abort on Xbyak's behalf.
int orbis_jit_reachable(const void *from, size_t len, const void *to, size_t len2);

/// Unmap every arena and release its physical pages. Unused in this port (module destructors do
/// not run here); provided for hosts that unload cleanly and for the tests.
void orbis_jit_release_all(void);

#ifdef __cplusplus
}
#endif

#endif /* ORBIS_JIT_H */
