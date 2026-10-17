/* SPDX-License-Identifier: MIT */
#ifndef _ORBIS_SYS_DIRENT_H
#define _ORBIS_SYS_DIRENT_H

/*
 * <sys/dirent.h> - the BSD spelling, which this target asks for and the SDK does not have.
 *
 * Same gap as <sys/endian.h>: --target=x86_64-pc-freebsd12-elf predefines __FreeBSD__ 12, so
 * portable code takes its FreeBSD arm and includes <sys/dirent.h> (e.g. libretro-common's
 * vfs_implementation.c). The SDK's <dirent.h> already has struct dirent, DT_* and the DIR API,
 * so this forwards to it instead of declaring the struct a second time.
 */

#include <dirent.h>

#endif /* _ORBIS_SYS_DIRENT_H */
