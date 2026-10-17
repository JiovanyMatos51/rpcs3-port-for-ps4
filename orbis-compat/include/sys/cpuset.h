// Copyright © 2026 Mikołaj Mikołajczyk
// SPDX-License-Identifier: MIT
/* FreeBSD's cpuset_t, for a platform that is a FreeBSD kernel with a musl libc.
 *
 * Mesa's u_thread.c does `#define cpu_set_t cpuset_t` under __FreeBSD__ and uses CPU_ZERO /
 * CPU_SET / CPU_ISSET and pthread_setaffinity_np. musl has these behind _GNU_SOURCE, which breaks
 * OpenOrbis' sched.h and alltypes.h (24 errors), so the type is declared here with musl's layout.
 *
 * `struct cpu_set_t` is the tag musl's pthread.h already references (OpenOrbis
 * include/pthread.h:226), so this is the type pthread_setaffinity_np expects.
 *
 * The macros work but affinity is not wired: Mesa only uses it to pin shader-compile threads, an
 * optimisation. A real implementation would use scePthreadSetaffinity. */
#pragma once
#include <stddef.h>

#ifndef _GNU_SOURCE
struct cpu_set_t { unsigned long __bits[128/sizeof(long)]; };
#define __CPU_BITS(i)  ((i)/(8*sizeof(unsigned long)))
#define __CPU_MASK(i)  (1UL << ((i)%(8*sizeof(unsigned long))))
#define CPU_SETSIZE    1024
#define CPU_ZERO(s)    do { for(size_t __i=0;__i<sizeof((s)->__bits)/sizeof((s)->__bits[0]);++__i) (s)->__bits[__i]=0; } while(0)
#define CPU_SET(i,s)   ((s)->__bits[__CPU_BITS(i)] |= __CPU_MASK(i))
#define CPU_CLR(i,s)   ((s)->__bits[__CPU_BITS(i)] &= ~__CPU_MASK(i))
#define CPU_ISSET(i,s) (((s)->__bits[__CPU_BITS(i)] & __CPU_MASK(i)) != 0)
#endif

typedef struct cpu_set_t cpuset_t;
