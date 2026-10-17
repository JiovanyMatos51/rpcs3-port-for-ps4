// Copyright © 2026 Mikołaj Mikołajczyk
// SPDX-License-Identifier: MIT
/* FIONREAD and its neighbours, with FreeBSD's values.
 *
 * The SDK's <sys/ioctl.h> declares ioctl() but defines none of the FION* requests, so code that
 * polls a socket for pending bytes does not compile (e.g. gambatte-libretro's net_serial.cpp).
 *
 * musl's Linux values (FIONREAD 0x541B) would be wrong: this FreeBSD-derived kernel encodes
 * direction, length and group in the request number, and a Linux number names a different request
 * or none.
 *
 * <sys/mman.h> is not a parallel case: it ends with `#include <bits/mman.h>`, which redefines
 * MAP_* and PROT_* with FreeBSD's values (MAP_PRIVATE|MAP_ANON == 0x1002, static_assert'ed in
 * orbis_mmap.cpp). The SDK never defines FION* at all.
 *
 * FreeBSD's values, derived so they can be checked:
 *
 *     _IOC(inout, group, num, len) = inout | ((len & 0x1fff) << 16) | (group << 8) | num
 *     IOC_OUT  = 0x40000000   (kernel writes to userland)
 *     IOC_IN   = 0x80000000   (kernel reads from userland)
 *     group 'f' = 0x66
 *
 *     FIONREAD = _IOR('f', 127, int) = 0x40000000 | (4 << 16) | (0x66 << 8) | 127 = 0x4004667f
 *     FIONBIO  = _IOW('f', 126, int) = 0x80000000 | (4 << 16) | (0x66 << 8) | 126 = 0x8004667e
 *     FIOASYNC = _IOW('f', 125, int) = 0x80000000 | (4 << 16) | (0x66 << 8) | 125 = 0x8004667d
 *
 * Not yet verified against the kernel: nothing here has called ioctl() on the console. Suspect
 * these values first if a caller gets an unexpected answer.
 */
#ifndef _ORBIS_COMPAT_SYS_IOCTL_H
#define _ORBIS_COMPAT_SYS_IOCTL_H

#include_next <sys/ioctl.h>

#ifndef IOC_VOID
#define IOC_VOID  0x20000000U
#define IOC_OUT   0x40000000U
#define IOC_IN    0x80000000U
#define IOC_INOUT (IOC_IN | IOC_OUT)
#endif

#ifndef _IOC
#define _IOC(inout, group, num, len) \
   ((unsigned long)((inout) | (((len) & 0x1fff) << 16) | ((group) << 8) | (num)))
#define _IO(g, n)      _IOC(IOC_VOID,  (g), (n), 0)
#define _IOR(g, n, t)  _IOC(IOC_OUT,   (g), (n), sizeof(t))
#define _IOW(g, n, t)  _IOC(IOC_IN,    (g), (n), sizeof(t))
#define _IOWR(g, n, t) _IOC(IOC_INOUT, (g), (n), sizeof(t))
#endif

#ifndef FIONREAD
#define FIONREAD _IOR('f', 127, int)
#endif
#ifndef FIONBIO
#define FIONBIO  _IOW('f', 126, int)
#endif
#ifndef FIOASYNC
#define FIOASYNC _IOW('f', 125, int)
#endif

#endif /* _ORBIS_COMPAT_SYS_IOCTL_H */
