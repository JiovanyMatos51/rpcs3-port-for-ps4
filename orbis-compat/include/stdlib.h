// Copyright © 2026 Mikołaj Mikołajczyk
// SPDX-License-Identifier: MIT
/* malloc_usable_size, where a FreeBSD program looks for it.
 *
 * musl (and the SDK, malloc.h:19) declares it in <malloc.h>; FreeBSD puts it in <stdlib.h>. Code
 * that gates on __FreeBSD__ (defined for this triple) includes <stdlib.h> and fails, e.g. dEQP's
 * deMemory.c. A second identical declaration is harmless.
 */
#ifndef _ORBIS_COMPAT_STDLIB_H
#define _ORBIS_COMPAT_STDLIB_H

#include_next <stdlib.h>

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

size_t malloc_usable_size(void *);

#ifdef __cplusplus
}
#endif

#endif /* _ORBIS_COMPAT_STDLIB_H */
