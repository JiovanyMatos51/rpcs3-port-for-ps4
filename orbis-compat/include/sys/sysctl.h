// Copyright © 2026 Mikołaj Mikołajczyk
// SPDX-License-Identifier: MIT
/* sysctl for a kernel that does not answer it, plus hw.ncpu.
 *
 * Mesa queries the CPU count, process name and memory size (u_cpu_detect.c, u_process.c,
 * os_misc.c, u_thread.c) and Lightrec's recompiler queries the CPU count. This kernel has no
 * sysctl; the declarations exist so those compile, and everything except hw.ncpu returns -1 so
 * callers take their own fallback.
 *
 * hw.ncpu is answered because every caller's fallback is one CPU, which disables parallelism.
 * Measured 2026-08-23: Beetle PSX HW's recompiler reported
 *
 *     [Lightrec]: Threaded recompiler started with 1 workers.
 *
 * on a six-core console (recompiler.c: `sysctlbyname("hw.ncpu", ...) ? 1 : count`). Mesa's
 * u_cpu_detect.c does the same.
 *
 * The value is six, what a homebrew process gets on this console. sceKernelGetCpumode() may
 * distinguish a six- from a seven-core mode, but the SDK has no constants for it and the mapping
 * is unverified. ORBIS_NCPU overrides the value.
 */
#pragma once
#include <sys/types.h>
#include <stdlib.h>
#if defined(__ORBIS__) || defined(__PS4__)
#include <orbis_env.h>
#endif
#define CTL_HW      6
#define HW_NCPU     3
#define HW_PHYSMEM  5
#define HW_USERMEM  6
#define CTL_KERN    1
#define KERN_PROC   14
#define KERN_PROC_PATHNAME 12
/* u_process.c asks for its own argv through this. The sysctl fails, so the caller takes its
   false path and only the constants are needed. */
#define KERN_PROC_ARGS     7
#define KERN_PROC_ARGV     1
#ifdef __cplusplus
extern "C" {
#endif

#if defined(__ORBIS__) || defined(__PS4__)
static inline int orbis_hw_ncpu(void) {
  /* Not getenv(): a .prx has its own static-musl `environ` that the executable's setenv()
     never reaches (see orbis_env.h). */
  const char* const e = orbis_env_get("ORBIS_NCPU");
  if (e && e[0]) {
    const int n = atoi(e);
    if (n > 0)
      return n;
    }
  return 6;
  }

/* Both spellings are answered: Lightrec asks by name, Mesa builds a {CTL_HW, HW_NCPU} mib. */
static inline int orbis_sysctl_ncpu(void* old, size_t* oldlen) {
  int n;
  if (!old || !oldlen || *oldlen < sizeof(int))
    return -1;
  n = orbis_hw_ncpu();
  __builtin_memcpy(old, &n, sizeof(n));
  *oldlen = sizeof(n);
  return 0;
  }
#endif

static inline int sysctl(const int* name, unsigned namelen, void* old, size_t* oldlen,
                         const void* new_, size_t newlen) {
  (void)new_; (void)newlen;
#if defined(__ORBIS__) || defined(__PS4__)
  if (name && namelen >= 2 && name[0] == CTL_HW && name[1] == HW_NCPU)
    return orbis_sysctl_ncpu(old, oldlen);
#endif
  (void)name; (void)namelen; (void)old; (void)oldlen;
  return -1;
  }
static inline int sysctlbyname(const char* name, void* old, size_t* oldlen,
                              const void* new_, size_t newlen) {
  (void)new_; (void)newlen;
#if defined(__ORBIS__) || defined(__PS4__)
  if (name && __builtin_strcmp(name, "hw.ncpu") == 0)
    return orbis_sysctl_ncpu(old, oldlen);
#endif
  (void)name; (void)old; (void)oldlen;
  return -1;
  }
#ifdef __cplusplus
}
#endif
