// Copyright © 2026 Mikołaj Mikołajczyk
// SPDX-License-Identifier: MIT
//
// ---------------------------------------------------------------- the defect
//
// On this console setenv() in one image is invisible to another.
//
// The SDK's libc.a is a real static musl archive (1481 objects; getenv/setenv are defined
// there, not stubs into a shared libc), so the executable and every .prx it loads each have
// their own `environ`. The frontend's env-file reader setenv()s into the eboot's; a core's
// getenv() reads its own, which nothing wrote.
//
// Measured 2026-08-23: ORBIS_NCPU=1 in /data/retroarch-env.txt did not reach the core, which
// still reported "Threaded recompiler started with 5 workers".
//
// Knobs read by Mesa (ORBIS_3D_LINEAR, ORBIS_NO_TESS, MESA_LOG_FILE, RADV_DEBUG...) were not
// affected because Mesa is linked into the executable; the first knob read by a loadable module
// failed silently.
//
// ---------------------------------------------------------------- what this does
//
// orbis_env_get() checks the process's own environment first, so anything genuinely set still
// wins, then falls back to the env files, parsed here, in this image.
//
// The file list mirrors what RetroArch's platform_orbis.c applies, in the same order, so a
// module and its loader see the same values (libretro has no channel to pass them). Since
// 2026-09-18 both OpenGothic (game/main.cpp) and RetroArch (platform_orbis.c) read the generic
// /data/orbis-env.txt first; the per-product paths below are deprecated and kept only for
// packages already installed.
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

// Bounded: these files are short hand-written configuration.
constexpr int   kMaxEntries = 64;
constexpr int   kMaxLine    = 512;

struct Entry {
  char key[64];
  char val[256];
  };

Entry g_entry[kMaxEntries];
int   g_count  = 0;
bool  g_loaded = false;

char* trim_front(char* s) {
  while (*s == ' ' || *s == '\t')
    s++;
  return s;
  }

void trim_back(char* s) {
  char* end = s + std::strlen(s);
  while (end > s) {
    const char c = end[-1];
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
      break;
    *--end = '\0';
    }
  }

void store(const char* key, const char* val) {
  // A later file overrides an earlier one, which is the order the frontend applies them in.
  for (int i = 0; i < g_count; i++) {
    if (std::strcmp(g_entry[i].key, key) == 0) {
      std::snprintf(g_entry[i].val, sizeof(g_entry[i].val), "%s", val);
      return;
      }
    }
  if (g_count >= kMaxEntries)
    return;
  std::snprintf(g_entry[g_count].key, sizeof(g_entry[g_count].key), "%s", key);
  std::snprintf(g_entry[g_count].val, sizeof(g_entry[g_count].val), "%s", val);
  g_count++;
  }

// Same grammar as RetroArch's frontend_orbis_apply_env_file: KEY=VALUE, '#' comments, blank
// lines ignored, whitespace trimmed on both sides of the '=' (a trailing space would otherwise
// change the value).
void load_file(const char* path) {
  std::FILE* f = std::fopen(path, "r");
  if (!f)
    return;

  char line[kMaxLine];
  while (std::fgets(line, sizeof(line), f)) {
    char* key = trim_front(line);
    if (*key == '#' || *key == '\n' || *key == '\r' || *key == '\0')
      continue;

    char* eq = std::strchr(key, '=');
    if (!eq)
      continue;
    *eq = '\0';

    char* val = trim_front(eq + 1);
    trim_back(key);
    trim_back(val);
    if (*key)
      store(key, val);
    }

  std::fclose(f);
  }

void load_once() {
  if (g_loaded)
    return;
  g_loaded = true;
  // Generic file first, so any consumer can flip this library's switches (ORBIS_THREAD_STACK,
  // ORBIS_SIGEV_THREAD, ORBIS_UMTX_LIBKERNEL...) on a console without rebuilding. Loaded first so
  // a program-specific file still overrides it.
  load_file("/data/orbis-env.txt");

  // Deprecated 2026-09-18, kept for packages already installed: older OpenGothic and RetroArch
  // packages read these names, and the files persist on /data across reinstalls. Remove once
  // released OpenGothic and RetroArch packages both use /data/orbis-env.txt; dropping them
  // earlier would silently disable existing settings.
  load_file("/data/tempest-env.txt");    // deprecated 2026-09-18 - OpenGothic packages before that
  load_file("/data/retroarch-env.txt");  // deprecated 2026-09-18 - RetroArch packages before that
  // The retired desktop-GL RetroArch package (RTRG00001) had its own file. Loaded last so it
  // overrides the shared file, as that frontend did. Same removal terms as above.
  load_file("/data/retroarch-glcore-env.txt");  // deprecated 2026-09-18 - product itself retired
  }

}  // namespace

extern "C" const char* orbis_env_get(const char* name) {
  if (!name || !*name)
    return nullptr;

  // The real environment first: a value set in this image beats a file.
  if (const char* v = std::getenv(name))
    if (*v)
      return v;

  load_once();
  for (int i = 0; i < g_count; i++)
    if (std::strcmp(g_entry[i].key, name) == 0)
      return g_entry[i].val;

  return nullptr;
  }
