// Copyright © 2026 Mikołaj Mikołajczyk
// SPDX-License-Identifier: MIT
#include "orbis_stat.h"

#include <sys/stat.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <cstddef>
#include <cstdint>

// The two entry points that libkernel.so exports and unemups4 binds. Declared with a void*
// buffer rather than through <orbis/libkernel.h>, whose OrbisKernelStat is a typedef of the
// wrong `struct stat`.
extern "C" int sceKernelStat(const char* path, void* st);
extern "C" int _fstat(int fd, void* st);

// Declared, not included: orbis_paths.h needs <string>, which on this SDK reaches
// <orbis/libkernel.h>, whose sceKernelStat prototype would conflict with the one above.
namespace orbis {
const char* anchorPath(const char* path, char* buf, size_t bufSize);
}

namespace {

// Kernel-side field offsets. See orbis_stat.h for how they were established.
constexpr size_t kDev     = 0x00;  // uint32
constexpr size_t kIno     = 0x04;  // uint32
constexpr size_t kMode    = 0x08;  // uint16
constexpr size_t kNlink   = 0x0a;  // uint16
constexpr size_t kUid     = 0x0c;  // uint32
constexpr size_t kGid     = 0x10;  // uint32
constexpr size_t kRdev    = 0x14;  // uint32
constexpr size_t kAtim    = 0x18;  // timespec{int64 sec, int64 nsec}
constexpr size_t kMtim    = 0x28;
constexpr size_t kCtim    = 0x38;
constexpr size_t kSize    = 0x48;  // int64
constexpr size_t kBlocks  = 0x50;  // int64
constexpr size_t kBlksize = 0x58;  // uint32
constexpr size_t kFlags   = 0x5c;  // uint32
constexpr size_t kGen     = 0x60;  // uint32
constexpr size_t kBirth   = 0x68;

// memcpy rather than a cast to a packed struct: the buffer's alignment is not guaranteed.
template<class T>
static T fieldAt(const unsigned char* raw, size_t off) {
  T v = T();
  std::memcpy(&v,raw+off,sizeof(T));
  return v;
  }

static void readTimespec(const unsigned char* raw, size_t off, struct timespec& out) {
  out.tv_sec  = static_cast<decltype(out.tv_sec)>(fieldAt<int64_t>(raw,off));
  out.tv_nsec = static_cast<decltype(out.tv_nsec)>(fieldAt<int64_t>(raw,off+8));
  }

// Kernel bytes in, caller's struct stat out, by field name on the caller's side so a
// future SDK that fixes its declaration keeps working.
static void translate(const unsigned char* raw, struct stat* out) {
  std::memset(out,0,sizeof(*out));
  out->st_dev     = static_cast<decltype(out->st_dev)>    (fieldAt<uint32_t>(raw,kDev));
  out->st_ino     = static_cast<decltype(out->st_ino)>    (fieldAt<uint32_t>(raw,kIno));
  out->st_mode    = static_cast<decltype(out->st_mode)>   (fieldAt<uint16_t>(raw,kMode));
  out->st_nlink   = static_cast<decltype(out->st_nlink)>  (fieldAt<uint16_t>(raw,kNlink));
  out->st_uid     = static_cast<decltype(out->st_uid)>    (fieldAt<uint32_t>(raw,kUid));
  out->st_gid     = static_cast<decltype(out->st_gid)>    (fieldAt<uint32_t>(raw,kGid));
  out->st_rdev    = static_cast<decltype(out->st_rdev)>   (fieldAt<uint32_t>(raw,kRdev));
  out->st_size    = static_cast<decltype(out->st_size)>   (fieldAt<int64_t> (raw,kSize));
  out->st_blocks  = static_cast<decltype(out->st_blocks)> (fieldAt<int64_t> (raw,kBlocks));
  out->st_blksize = static_cast<decltype(out->st_blksize)>(fieldAt<uint32_t>(raw,kBlksize));
  out->st_flags   = fieldAt<uint32_t>(raw,kFlags);
  out->st_gen     = fieldAt<uint32_t>(raw,kGen);
  readTimespec(raw,kAtim, out->st_atim);
  readTimespec(raw,kMtim, out->st_mtim);
  readTimespec(raw,kCtim, out->st_ctim);
  readTimespec(raw,kBirth,out->st_birthtim);

  // ---------------------------------------------------------------- the second reader
  //
  // Two kinds of code read this buffer:
  //
  //   * code compiled against the SDK's <sys/stat.h>, which reads st_size at 0x50
  //     (served by the assignments above);
  //   * code prebuilt into the SDK, notably libc++'s <filesystem> in libc++.a, compiled
  //     against the real kernel layout, which reads st_size at 0x48.
  //
  // Measured on the emulator with only the assignments above:
  //     stat()                 rc=0 st_size=722595072 st_mode=0100644   <- correct
  //     filesystem::file_size  0 (no error)                             <- wrong
  //     filesystem::is_regular 1                                        <- correct
  // st_mode is at offset 8 in both layouts; 0x48 in the public layout is st_ctim.tv_nsec
  // (st_ctim = 0x40), which is 0. A zero file_size made OpenGothic skip every .vdf
  // archive (`failed to open resource: font_old_20_white.fnt`).
  //
  // So the size is also written at 0x48, overwriting st_ctim.tv_nsec, which nothing here
  // reads; st_ctim.tv_sec is untouched.
  static_assert(offsetof(struct stat,st_size)!=kSize,
                "the SDK header now agrees with the kernel - delete this block");
  static_assert(offsetof(struct stat,st_ctim)+8==kSize,
                "0x48 is no longer st_ctim.tv_nsec - re-measure before clobbering it");
  const int64_t trueSize = fieldAt<int64_t>(raw,kSize);
  std::memcpy(reinterpret_cast<unsigned char*>(out)+kSize,&trueSize,sizeof(trueSize));
  }

// 256 rather than 120, as a safety margin for whatever the kernel writes.
constexpr size_t kScratch = 256;

}

namespace orbis {

int rawKernelFstat(int fd, void* out) {
  std::memset(out,0,kSonyStatSize);
  return _fstat(fd,out);
  }

int64_t sonyStatQword(const void* raw, size_t offset) {
  return fieldAt<int64_t>(static_cast<const unsigned char*>(raw),offset);
  }

}

// ------------------------------------------------------------------ the interposition

extern "C" int fstat(int fd, struct stat* out) {
  if(out==nullptr) {
    errno = EFAULT;
    return -1;
    }
  unsigned char raw[kScratch] = {};
  const int rc = _fstat(fd,raw);
  if(rc!=0)
    return rc;
  translate(raw,out);
  return 0;
  }

extern "C" int stat(const char* path, struct stat* out) {
  if(path==nullptr || out==nullptr) {
    errno = EFAULT;
    return -1;
    }
  // Anchored like `open` (orbis_paths.h): there is no working directory (getcwd is ENOSYS),
  // and existence checks such as OpenGothic's FileUtil::exists stat the same relative names
  // that open() writes.
  char          abuf[512] = {};
  const char*   apath     = orbis::anchorPath(path,abuf,sizeof(abuf));
  unsigned char raw[kScratch] = {};
  const int rc = sceKernelStat(apath,raw);
  // sceKernelStat fails with 0x8002_0000 | errno, not -1 with errno set. Passed through,
  // libc++'s posix_stat (`== -1`) took a missing path for success (Panda3DS's
  // create_directories then threw "File exists"). The low bits are the FreeBSD errno, which
  // matches this SDK's numbering.
  if(rc!=0) {
    errno = (static_cast<uint32_t>(rc)>>16)==0x8002 ? (rc & 0xFFFF) : EIO;
    return -1;
    }
  translate(raw,out);
  return 0;
  }

// The filesystems a PS4 title reads have no symlinks, so lstat == stat (libc.a's version
// went through an fstatat stub that always returns ENOSYS).
extern "C" int lstat(const char* path, struct stat* out) {
  return stat(path,out);
  }

// Only the two forms used in practice: an absolute path (fd ignored) and a path relative
// to AT_FDCWD. Anything else keeps the SDK's ENOSYS rather than stat the wrong file.
extern "C" int fstatat(int fd, const char* path, struct stat* out, int flag) {
  (void)flag;
  if(path==nullptr || out==nullptr) {
    errno = EFAULT;
    return -1;
    }
  if(path[0]=='/' || fd==AT_FDCWD)
    return stat(path,out);
  errno = ENOSYS;
  return -1;
  }
