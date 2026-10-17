// Copyright © 2026 Mikołaj Mikołajczyk
// SPDX-License-Identifier: MIT
/* What every translation unit needs before it can include the SDK's own headers.
 *
 * The headers under <orbis/> use size_t, uint32_t etc. without including <stddef.h> or <stdint.h>;
 * 26 of the SDK's 189 orbis/ headers fail to compile alone for that reason.
 *
 * Measured, orbis/ headers that fail to compile alone (of 189):
 *
 *     none                     26
 *     -include stdlib.h        16
 *     -include orbis_prefix.h   7
 *
 * It also injects less than `-include stdlib.h`, which pulls a whole libc header into global scope
 * (it once caused an integer-only std::abs to truncate floats at 37 call sites).
 *
 * The remaining seven are SDK bugs, not missing includes:
 *
 *     orbis/JpegEnc.h:17       says OrbisJpgEncOutputInfo; the type is OrbisJpegEncOutputInfo
 *     orbis/SysCore.h:27       names OrbisAppInfo, which nothing defines
 *     orbis/libc.h:352         redeclares clang builtins (__sync_fetch_and_add_16 and neighbours)
 *     orbis/LibcInternal.h:432 the same redeclarations
 *     orbis/Font.h, FontFt.h, Usbd.h
 *
 * They are not shadowed here; that would hide the bugs instead of fixing them.
 */
#ifndef _ORBIS_PREFIX_H
#define _ORBIS_PREFIX_H

#include <stddef.h>
#include <stdint.h>

#endif /* _ORBIS_PREFIX_H */
