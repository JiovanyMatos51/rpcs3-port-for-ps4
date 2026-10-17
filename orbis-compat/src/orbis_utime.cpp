// SPDX-License-Identifier: MIT
//
// utimensat/futimens/futimesat over libkernel's sceKernelUtimes/sceKernelFutimes.
//
// libc.a(utimensat.lo) is a stub: errno = ENOSYS, return -1, no system call. utime, utimes, futimes,
// lutimes and futimens all end in it, so every timestamp update failed: libc++
// filesystem::last_write_time, llvm::sys::fs::setLastAccessAndModificationTime, Mesa's disk cache
// marker, and anything else reaching musl's utime family.
//
// utimensat.lo defines two global names, utimensat and futimesat; both are defined here, otherwise a
// reference to futimesat would extract the member and its utimensat would collide with this one.
// futimens is defined too, so futimens.lo is not needed. utime.lo, utimes.lo (through futimesat.lo's
// __futimesat), futimes.lo and lutimes.lo stay musl's and call into this file.
//
// libkernel's own utimes/futimes cannot be named: libc.a members of those names are linked first.
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>

// Weak: a firmware without the export keeps today's ENOSYS instead of failing to load.
extern "C" {
int32_t orbisKernelUtimes(const char* path, const struct timeval* times) __asm__("sceKernelUtimes") __attribute__((weak));
int32_t orbisKernelFutimes(int fd, const struct timeval* times) __asm__("sceKernelFutimes") __attribute__((weak));
int32_t orbisKernelGettimeofday(struct timeval* tv) __asm__("sceKernelGettimeofday");
}

// Declared, not included, as in orbis_stat.cpp: orbis_paths.h needs <string>, which can reach
// <orbis/libkernel.h> and its conflicting prototypes.
namespace orbis {
const char* anchorPath(const char* path, char* buf, size_t bufSize);
}

namespace {

// SCE calls fail with 0x8002_0000 | errno, the FreeBSD errno this SDK's headers also use.
int sceFailure(int32_t rc) {
  errno = (static_cast<uint32_t>(rc)>>16)==0x8002 ? (rc & 0xFFFF) : EIO;
  return -1;
  }

bool validNsec(long ns) {
  return ns==UTIME_NOW || ns==UTIME_OMIT || (ns>=0 && ns<1000000000L);
  }

}

// path == nullptr acts on dirfd, as musl's futimens (utimensat(fd, 0, times, 0)) and Linux expect.
// AT_SYMLINK_NOFOLLOW (lutimes) is accepted and has no effect: the filesystems a title sees have no
// symlinks, which is also why lstat == stat in orbis_stat.cpp.
extern "C" int utimensat(int dirfd, const char* path, const struct timespec times[2], int flags) {
  if((flags & ~AT_SYMLINK_NOFOLLOW)!=0) {
    errno = EINVAL;
    return -1;
    }
  const bool byFd = path==nullptr;
  if(!byFd && path[0]=='\0') {
    errno = ENOENT;
    return -1;
    }
  // No *at system call to resolve against: only absolute paths and AT_FDCWD are handled.
  if(!byFd && path[0]!='/' && dirfd!=AT_FDCWD) {
    errno = ENOTSUP;
    return -1;
    }

  struct timeval        tv[2] = {};
  const struct timeval* ptv   = nullptr; // nullptr = both "now", the kernel's own form
  if(times!=nullptr) {
    const long ns0 = times[0].tv_nsec;
    const long ns1 = times[1].tv_nsec;
    if(!validNsec(ns0) || !validNsec(ns1)) {
      errno = EINVAL;
      return -1;
      }
    // Linux returns success here without looking at the file.
    if(ns0==UTIME_OMIT && ns1==UTIME_OMIT)
      return 0;
    if(!(ns0==UTIME_NOW && ns1==UTIME_NOW)) {
      struct stat    cur     = {};
      bool           haveCur = false;
      struct timeval now     = {};
      bool           haveNow = false;
      for(int i=0; i<2; ++i) {
        const long ns = times[i].tv_nsec;
        if(ns==UTIME_NOW) {
          if(!haveNow) {
            const int32_t rc = orbisKernelGettimeofday(&now);
            if(rc!=0)
              return sceFailure(rc);
            haveNow = true;
            }
          tv[i] = now;
          } else if(ns==UTIME_OMIT) {
          // utimes has no "leave alone", so the current value is written back. stat/fstat are the
          // orbis_stat.cpp translations, which anchor a relative path the same way.
          if(!haveCur) {
            if((byFd ? ::fstat(dirfd,&cur) : ::stat(path,&cur))!=0)
              return -1;
            haveCur = true;
            }
          const struct timespec& t = (i==0) ? cur.st_atim : cur.st_mtim;
          tv[i].tv_sec  = t.tv_sec;
          tv[i].tv_usec = static_cast<suseconds_t>(t.tv_nsec/1000);
          } else {
          tv[i].tv_sec  = times[i].tv_sec;
          tv[i].tv_usec = static_cast<suseconds_t>(ns/1000);
          }
        }
      ptv = tv;
      }
    }

  int32_t rc = 0;
  if(byFd) {
    if(orbisKernelFutimes==nullptr) {
      errno = ENOSYS;
      return -1;
      }
    rc = orbisKernelFutimes(dirfd,ptv);
    } else {
    if(orbisKernelUtimes==nullptr) {
      errno = ENOSYS;
      return -1;
      }
    char abuf[512] = {};
    rc = orbisKernelUtimes(orbis::anchorPath(path,abuf,sizeof(abuf)),ptv);
    }
  return rc!=0 ? sceFailure(rc) : 0;
  }

extern "C" int futimens(int fd, const struct timespec times[2]) {
  return utimensat(fd,nullptr,times,0);
  }

// Same conversion as musl's __futimesat, which utimes() still reaches through futimesat.lo.
extern "C" int futimesat(int dirfd, const char* path, const struct timeval times[2]) {
  if(times==nullptr)
    return utimensat(dirfd,path,nullptr,0);
  struct timespec ts[2] = {};
  for(int i=0; i<2; ++i) {
    if(times[i].tv_usec<0 || times[i].tv_usec>=1000000) {
      errno = EINVAL;
      return -1;
      }
    ts[i].tv_sec  = times[i].tv_sec;
    ts[i].tv_nsec = static_cast<long>(times[i].tv_usec*1000);
    }
  return utimensat(dirfd,path,ts,0);
  }
