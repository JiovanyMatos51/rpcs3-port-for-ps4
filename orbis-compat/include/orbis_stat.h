// Copyright © 2026 Mikołaj Mikołajczyk
// SPDX-License-Identifier: MIT
// Corrective interposition of the stat() family for PS4 builds.
//
// ---------------------------------------------------------------- the defect
//
// The OpenOrbis SDK's `struct stat` does not have the layout the PS4 kernel fills, and
// libc.a does not translate: `fstat` is a tail jump to libkernel's `_fstat` and `stat`
// is imported straight from libkernel.so. The caller reads the kernel's bytes through
// the wrong declaration.
//
// Both layouts, measured:
//
//   caller side, offsetof compiled with the PS4 toolchain against <sys/stat.h>:
//     st_dev 0(4)  st_ino 4(4)  st_mode 8(4)  st_nlink 12(2)  st_uid 16
//     st_atim 32   st_mtim 48   st_size 80    st_blocks 88    st_blksize 96
//     sizeof(struct stat) = 128
//
//   kernel side, the classic FreeBSD amd64 (pre-ino64) layout:
//     st_dev 0(4)  st_ino 4(4)  st_mode 8(2)  st_nlink 10(2)  st_uid 12  st_gid 16
//     st_rdev 20   st_atim 24   st_mtim 40    st_ctim 56      st_size 72(0x48)
//     st_blocks 80(0x50)        st_blksize 88 st_flags 92     st_gen 96
//     st_birthtim 104           sizeof = 120
//
// Two fields are visibly affected, both confirmed on the console:
//
//   * st_size. The caller reads offset 0x50, which holds the kernel's st_blocks.
//     A 64857-byte SystemPack.vdf reported st_size = 128 on hardware (128 x 512 =
//     65536) and 127 under unemups4 (ceil(64857/512)).
//   * st_mode. The caller's st_mode is 4 bytes at offset 8; the kernel wrote a 2-byte
//     mode plus a 2-byte st_nlink there. The console printed 0o300755 =
//     (nlink 1 << 16) | 0o100755.
//
// Consequence: ZenKit's Mmap sizes its mapping from fstat's st_size, so every .vdf was
// mapped 128 bytes long, the signature read returned zero bytes and loading failed with
// `VFS disk signature not recognized: ""`. mmap itself was fine (file-backed mmap matches
// read() on hardware).
//
// The SDK has no correct declaration: `OrbisKernelStat` is a typedef of the same
// `struct stat` (orbis/_types/kernel.h:177). The kernel layout above comes from:
//   1. the console's own bytes (two fields, both decoding exactly);
//   2. unemups4's fill_sce_stat (crates/libs/src/libkernel/fs.rs:133);
//   3. the classic FreeBSD amd64 `struct stat` (reconstructed, not from a header).
// st_size at 0x48 has not been observed directly yet, so probeVdfRead() prints the raw
// quadwords at both 0x48 and 0x50.
//
// Hooking: strong definitions of stat/fstat/lstat/fstatat in this TU. `fstat` and
// `lstat` are libc.a archive members, never extracted once the symbol is defined;
// `stat` resolves here because an object file beats a shared library. The underlying
// calls are `sceKernelStat` (path) and `_fstat` (fd), the two that both libkernel.so
// exports and unemups4 binds. sceKernelFstat is not used: unemups4 does not bind it and
// a missing import is fatal there.
//
// lstat and fstatat were previously unusable: libc.a's `fstatat` is a stub returning -1
// with errno 78 (ENOSYS), and musl's `lstat` is built on it.
#pragma once

#include <cstdint>
#include <cstddef>

struct stat;

namespace orbis {

// Raw bytes the kernel writes for one fd, for diagnostics. `out` must have room for
// kSonyStatSize. Returns the underlying call's return value.
constexpr size_t kSonyStatSize = 120;
int rawKernelFstat(int fd, void* out);

// Read a little-endian signed 64-bit field out of such a buffer.
int64_t sonyStatQword(const void* raw, size_t offset);

// Offsets used above, so the probe and the translation cannot drift apart.
constexpr size_t kSonyStatSizeOff   = 0x48;
constexpr size_t kSonyStatBlocksOff = 0x50;

}
