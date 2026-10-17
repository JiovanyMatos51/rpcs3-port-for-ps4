// SPDX-License-Identifier: MIT
//
// statfs/statvfs family over libkernel's _fstatfs.
//
// libc.a(statvfs.lo) never writes the caller's struct statvfs. statvfs/fstatvfs call
// statfs/_fstatfs into a private stack buffer and return only the sign of the result
// (llvm-objdump of the member):
//
//     statvfs64:  sub $0x928,%rsp; mov %rsp,%rsi; call statfs64; sar $31,%eax; add $0x928,%rsp; ret
//
// So every call "succeeded" and left the caller's struct as it was. Callers that zero it (libc++
// filesystem::space, RPCS3's disk health check, which logged "bsize 0, frsize 0, blocks 0" on the
// console and turned itself off) read zeros; the others (RPCS3's fs::statfs and so the JIT cache
// budget, llvm::sys::fs::disk_space) read stack garbage.
//
// The member defines eight global names: statfs, statfs64, fstatfs, fstatfs64, statvfs, statvfs64,
// fstatvfs, fstatvfs64. All eight are defined here. If one were left out, a reference to it would
// extract the member and its other seven definitions would collide with these (duplicate symbol).
// With this archive linked --whole-archive the member is then never extracted.
#include <orbis_log.h>

#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <fcntl.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>

// With _GNU_SOURCE or _LARGEFILE64_SOURCE the SDK headers turn these into macros for the plain
// names; the definitions below need the real symbols.
#undef statfs64
#undef fstatfs64
#undef statvfs64
#undef fstatvfs64

// The libkernel import statvfs.lo itself uses: FreeBSD fstatfs(2), -1 and errno on failure. Bound by
// label so no SDK prototype of the same name can conflict.
extern "C" int orbisKernelFstatfs(int fd, struct statfs* buf) __asm__("_fstatfs");

namespace {

// The SDK's struct statfs is FreeBSD 12's layout (MNAMELEN 1024, 0x928 bytes). FreeBSD 9, which this
// kernel derives from, writes 472 bytes with the same prefix; everything read below is in it.
static_assert(offsetof(struct statfs, f_version) == 0x00, "FreeBSD statfs layout");
static_assert(offsetof(struct statfs, f_flags)   == 0x08, "FreeBSD statfs layout");
static_assert(offsetof(struct statfs, f_bsize)   == 0x10, "FreeBSD statfs layout");
static_assert(offsetof(struct statfs, f_iosize)  == 0x18, "FreeBSD statfs layout");
static_assert(offsetof(struct statfs, f_blocks)  == 0x20, "FreeBSD statfs layout");
static_assert(offsetof(struct statfs, f_bfree)   == 0x28, "FreeBSD statfs layout");
static_assert(offsetof(struct statfs, f_bavail)  == 0x30, "FreeBSD statfs layout");
static_assert(offsetof(struct statfs, f_files)   == 0x38, "FreeBSD statfs layout");
static_assert(offsetof(struct statfs, f_ffree)   == 0x40, "FreeBSD statfs layout");
static_assert(offsetof(struct statfs, f_namemax) == 0xb8, "FreeBSD statfs layout");
static_assert(offsetof(struct statfs, f_fsid)    == 0xc0, "FreeBSD statfs layout");
static_assert(sizeof(struct statfs) >= 0x928, "smaller than the buffer libc.a hands the kernel");

// STATFS_VERSION before and after FreeBSD 12's ino64 change; the prefix read here is the same in both.
constexpr uint32_t kStatfsVersion   = 0x20030518;
constexpr uint32_t kStatfsVersion12 = 0x20140518;

constexpr uint64_t kMntRdonly = 0x1; // FreeBSD MNT_RDONLY
constexpr uint64_t kMntNosuid = 0x8; // FreeBSD MNT_NOSUID

std::atomic<bool> s_rejectLogged{false};
std::atomic<bool> s_firstLogged{false};

// A guard against a layout that is not the one above, not a filesystem policy. Upper bound 32 MiB:
// exFAT allows clusters that large, and msdosfs-style drivers report the cluster as f_bsize.
bool plausible(const struct statfs& k) {
  if(k.f_version!=kStatfsVersion && k.f_version!=kStatfsVersion12)
    return false;
  const uint64_t b = k.f_bsize;
  if(b<512 || b>(32ull<<20) || (b&(b-1))!=0)
    return false;
  return k.f_blocks!=0 && k.f_bfree<=k.f_blocks;
  }

// Same field mapping as FreeBSD's own statvfs(3): f_frsize is the fragment size the block counts are
// in, f_bsize the preferred I/O size.
void translate(const struct statfs& k, struct statvfs* out) {
  std::memset(out,0,sizeof(*out));
  out->f_bsize   = static_cast<unsigned long>(k.f_iosize!=0 ? k.f_iosize : k.f_bsize);
  out->f_frsize  = static_cast<unsigned long>(k.f_bsize);
  out->f_blocks  = k.f_blocks;
  out->f_bfree   = k.f_bfree;
  // Negative while root's reserve is in use; unsigned in struct statvfs.
  out->f_bavail  = k.f_bavail<0 ? 0 : static_cast<uint64_t>(k.f_bavail);
  out->f_files   = k.f_files;
  out->f_ffree   = k.f_ffree<0 ? 0 : static_cast<uint64_t>(k.f_ffree);
  out->f_favail  = out->f_ffree;
  out->f_fsid    = k.f_fsid;
  out->f_flag    = ((k.f_flags & kMntRdonly)!=0 ? ST_RDONLY : 0) | ((k.f_flags & kMntNosuid)!=0 ? ST_NOSUID : 0);
  out->f_namemax = k.f_namemax!=0 ? k.f_namemax : 255;
  }

int answer(int rc, const struct statfs& k, struct statvfs* out) {
  if(rc!=0)
    return -1; // errno from open or _fstatfs
  if(!plausible(k)) {
    if(orbis_log_enabled()!=0 && !s_rejectLogged.exchange(true,std::memory_order_relaxed))
      orbis_log("statvfs: kernel statfs reply is not a FreeBSD struct statfs (version 0x%08x, bsize %llu, "
                "blocks %llu, bfree %llu); statvfs/fstatvfs fail with ENOSYS instead of returning it",
                unsigned(k.f_version),(unsigned long long)k.f_bsize,(unsigned long long)k.f_blocks,
                (unsigned long long)k.f_bfree);
    errno = ENOSYS;
    return -1;
    }
  translate(k,out);
  if(orbis_log_enabled()!=0 && !s_firstLogged.exchange(true,std::memory_order_relaxed))
    orbis_log("statvfs: served from libkernel _fstatfs (version 0x%08x): frsize %llu, iosize %llu, blocks %llu, "
              "bavail %lld",unsigned(k.f_version),(unsigned long long)k.f_bsize,(unsigned long long)k.f_iosize,
              (unsigned long long)k.f_blocks,(long long)k.f_bavail);
  return 0;
  }

}

// ------------------------------------------------------------------ statfs: unchanged semantics
//
// What statvfs.lo did: FreeBSD struct statfs straight into the caller's buffer. open() is
// orbis_paths.cpp's, so a relative path is anchored like every other open.

extern "C" int fstatfs(int fd, struct statfs* buf) {
  return orbisKernelFstatfs(fd,buf);
  }

extern "C" int statfs(const char* path, struct statfs* buf) {
  const int fd = ::open(path,O_RDONLY);
  if(fd<0)
    return -1;
  const int rc    = orbisKernelFstatfs(fd,buf);
  const int saved = errno;
  ::close(fd);
  errno = saved;
  return rc;
  }

// ------------------------------------------------------------------ statvfs: translated

extern "C" int fstatvfs(int fd, struct statvfs* out) {
  if(out==nullptr) {
    errno = EFAULT;
    return -1;
    }
  struct statfs k;
  std::memset(&k,0,sizeof(k)); // an untouched buffer must fail the version check
  return answer(orbisKernelFstatfs(fd,&k),k,out);
  }

extern "C" int statvfs(const char* __restrict path, struct statvfs* __restrict out) {
  if(out==nullptr) {
    errno = EFAULT;
    return -1;
    }
  struct statfs k;
  std::memset(&k,0,sizeof(k));
  return answer(statfs(path,&k),k,out);
  }

// Weak aliases inside statvfs.lo; strong here, like open64 in orbis_paths.cpp.
extern "C" int statfs64(const char* path, struct statfs* buf) __attribute__((alias("statfs")));
extern "C" int fstatfs64(int fd, struct statfs* buf) __attribute__((alias("fstatfs")));
extern "C" int statvfs64(const char* __restrict path, struct statvfs* __restrict out) __attribute__((alias("statvfs")));
extern "C" int fstatvfs64(int fd, struct statvfs* out) __attribute__((alias("fstatvfs")));
