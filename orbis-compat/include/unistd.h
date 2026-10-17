// Copyright © 2026 Mikołaj Mikołajczyk
// SPDX-License-Identifier: MIT
/* sysconf(_SC_NPROCESSORS_ONLN), which this libc answers with one on a six-core console.
 *
 * Counterpart of the hw.ncpu fix in sys/sysctl.h: some callers use sysctlbyname("hw.ncpu"), others
 * (e.g. RetroArch's cpu_features_get_core_amount(), features_cpu.c:927) use
 * sysconf(_SC_NPROCESSORS_ONLN). musl answers the latter via sched_getaffinity, a Linux syscall this
 * kernel lacks, so it fails and callers fall back to one CPU.
 *
 * A macro rather than a redefined sysconf(): shadowing libc's symbol depends on link order. The
 * macro is resolved at the call site and orbis_sysconf forwards everything else to libc's sysconf.
 */
#ifndef _ORBIS_COMPAT_UNISTD_H
#define _ORBIS_COMPAT_UNISTD_H

#include_next <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

long orbis_sysconf(int name);

#ifdef __cplusplus
}
#endif

/* Defined after the declaration above. A later redeclaration of sysconf declares orbis_sysconf,
 * which has the same signature. */
#define sysconf orbis_sysconf

#endif /* _ORBIS_COMPAT_UNISTD_H */
