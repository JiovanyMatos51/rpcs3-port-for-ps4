// Copyright © 2026 Mikołaj Mikołajczyk
// SPDX-License-Identifier: MIT
/* ENODATA, which this platform's errno table does not have.
 *
 * The SDK ships musl headers, but bits/errno.h uses FreeBSD numbering (ECONNREFUSED 61,
 * EOPNOTSUPP 45, ELAST 97). FreeBSD never adopted the STREAMS errors, so ENODATA is missing.
 *
 * ECONNREFUSED matches Mesa: radv_amdgpu_cs.c defines ENODATA that way under DETECT_OS_FREEBSD.
 * This port does not set that macro, so the value is provided here instead of -DENODATA=61.
 *
 * Side effect: `#ifdef ENODATA` in consumers now takes its true arm. Nothing in this port relies on
 * the name being absent; keeping it in a header makes the change visible.
 */
#ifndef _ORBIS_COMPAT_ERRNO_H
#define _ORBIS_COMPAT_ERRNO_H

#include_next <errno.h>

#ifndef ENODATA
#define ENODATA ECONNREFUSED
#endif

#endif /* _ORBIS_COMPAT_ERRNO_H */
