// Copyright © 2026 Mikołaj Mikołajczyk
// SPDX-License-Identifier: MIT
// See include/unistd.h for why this exists.
#include <sys/sysctl.h>

// Our unistd.h macro must not reach the forwarding call below or this function would call
// itself, so it is undefined here and libc's symbol is declared by hand.
#include <unistd.h>
#undef sysconf

#include <sys/resource.h>
#include <cerrno>
#include <cstdint>

extern "C" long sysconf(int);

#if defined(__ORBIS__) || defined(__PS4__)
// The SDK declares it taking size_t by value; the out-pointer form is the real one (see orbis_mem.cpp).
// Weak: without the export _SC_AVPHYS_PAGES is answered as unsupported.
extern "C" int32_t orbisSysconfFlexibleAvailable(size_t* out)
        __asm__("sceKernelAvailableFlexibleMemorySize") __attribute__((weak));
#endif

extern "C" long orbis_sysconf(int name) {
#if defined(__ORBIS__) || defined(__PS4__)
  // _SC_NPROCESSORS_CONF and _SC_NPROCESSORS_ONLN are the same here: cores are never offlined.
  if (name == _SC_NPROCESSORS_ONLN || name == _SC_NPROCESSORS_CONF) {
    size_t len = sizeof(int);
    int    n   = 0;
    if (sysctlbyname("hw.ncpu", &n, &len, nullptr, 0) == 0 && n > 0)
      return n;
    return 1;
    }
  // musl asks getrlimit(7), Linux's RLIMIT_NOFILE, which this kernel reads as RLIMIT_NPROC. FreeBSD's
  // RLIMIT_NOFILE is 8. Otherwise musl's answer: -1 for no limit (FreeBSD RLIM_INFINITY is INT64_MAX).
  if (name == _SC_OPEN_MAX) {
    struct rlimit rl = {};
    if (getrlimit(8, &rl) != 0)
      return -1;
    if (rl.rlim_cur >= 0x7fffffffffffffffull)
      return -1;
    return static_cast<long>(rl.rlim_cur);
    }
  // musl answers from sysinfo(), an ENOSYS stub here, and then reads the struct it never filled.
  // Flexible memory is what the process can still map; counted in libc's own page size so that
  // AVPHYS_PAGES * PAGESIZE is bytes.
  if (name == _SC_AVPHYS_PAGES) {
    size_t avail = 0;
    if (orbisSysconfFlexibleAvailable == nullptr || orbisSysconfFlexibleAvailable(&avail) != 0) {
      errno = EINVAL;
      return -1;
      }
    const long page = sysconf(_SC_PAGESIZE);
    return static_cast<long>(avail / static_cast<size_t>(page > 0 ? page : 0x4000));
    }
#endif
  return sysconf(name);
  }
