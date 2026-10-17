// Copyright © 2026 Mikołaj Mikołajczyk
// SPDX-License-Identifier: MIT
#pragma once

// Anchoring relative paths, because this process has no working directory.
//
// probeSavePaths() on hardware, 2026-08-07:
//
//   getcwd()                          errno 78  = ENOSYS
//   open("save_slot_probe.sav", ...)  errno 22  = EINVAL
//   mkdir("/data/OpenGothic")         created
//   /data/OpenGothic/probe.sav        write 19 B, stat 19, read-back identical
//   /data/probe.sav                   write 19 B, stat 19, read-back identical
//   /data/gothic2/probe.sav           write 19 B, stat 19, read-back identical
//   /app0/probe.sav                   errno 30  = EROFS (negative control)
//
// getcwd is ENOSYS, so there is no working directory at all, and a relative open() fails with
// EINVAL (chdir("/app0") also fails).
//
// Since every relative path currently fails, redirecting all of them cannot break anything that
// works; a blanket rewrite needs no per-title list of filenames. It also covers stat() of the same
// names (e.g. OpenGothic's FileUtil::exists for save slots) and files such as crash.log that are
// written next to the executable.
//
// /data is the writable area GoldHEN gives homebrew; a subdirectory keeps files apart from the
// game data (which may be on USB or /app0).
//
// The root used to be a fixed `/data/OpenGothic/`, which made other titles linking this library
// (e.g. RetroArch, opening `Main Menu.png`) write there too and risk reading each other's files.
// It is now chosen in this order, and the choice is logged once:
//
//   1. orbis_set_anchor_root(), if the application called it before its first relative path.
//   2. `/data/<TITLEID>/` from sceKernelGetAppInfo; unique per title.
//   3. `/data/orbis-compat/`, when 1 and 2 both fail.
//
// sceKernelGetAppInfo is not yet confirmed on this firmware; the log line reports what it returned.
//
// The C++ part is guarded so the C-linkage setter below can be used from C frontends.
#ifdef __cplusplus
#include <string>

namespace orbis {

// The anchor, with a trailing '/'. Decided and created on first use; the choice, the mkdir's
// result and what sceKernelGetAppInfo said are logged once.
const std::string& anchorRoot();

// Absolute paths are returned unchanged, relative ones prefixed with anchorRoot(). The
// return value points either at `path` or at `buf`, so no allocation is needed on the hot
// path (this runs inside `stat`, which some titles call for every path segment).
const char* anchorPath(const char* path, char* buf, size_t bufSize);

// Logs the root and the first anchored path, once. Not per call: some menus stat save
// slots every frame.
void anchorSayOnce(const char* original, const char* rewritten);

}
#endif /* __cplusplus */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Sets the directory relative paths are anchored under. A trailing '/' is added if missing, and
/// the directory is created on first use like any other anchor.
///
/// Call it before the first relative path, i.e. before anything opens a file. The anchor is fixed
/// on first use so stat() stays cheap; a later call has no effect and is reported in the log.
void orbis_set_anchor_root(const char *path);

#ifdef __cplusplus
}
#endif
