// The console's log channel and its termination policy. Opt-in: optional/, not the archive.
//
// This is policy (a dying process idles instead of returning) and it requires -lSceNet, so it is
// not in liborbis-compat.a. It registers into the hook declared in orbis_log.h; nothing in src/
// depends on it.
//
// Logging goes to two independent channels:
//   * orbis_netlog() - UDP datagram to the dev host, fire-and-forget, primary
//     (orbis_netlog.h, received by scripts/ps4/logs.sh)
//   * klog     - sceKernelDebugOutText, relayed by GoldHEN's klog server; the
//     only channel that works when netlog never came up
//
// A console debug write blocks the calling thread for seconds when nothing drains
// the debug channel, so klog is only used for a bounded set of lines outside render
// loops. ps4_log_frame() is the UDP-only variant for per-frame lines.
//
// Termination: returning from main() on a retail console shows the error dialog
// (CE-34878-0), which reads as a crash, so a demo ends with ps4_idle_forever().
//
// Run config: automated runs need the process to return so the harness gets an exit
// code. The same binary is used for emulator and console runs; the host decides by
// placing a file next to the executable:
//
//   /app0/ps4-run.cfg   optional, plain text, one `key=value` per line, `#` comments
//     autoexit=1        ps4_idle_forever() returns instead of idling
//     frame-klog=1      ps4_log_frame() also writes klog, not netlog alone
//
// Without the file (every .pkg launch; make-pkg.sh does not ship it) both are 0:
// idle forever, per-frame lines over UDP only. scripts/ps4/run-tests.sh writes the
// file into the build directory before starting unemups4, which mounts that
// directory as /app0. There is intentionally no emulator detection.
#pragma once

#include <orbis_netlog.h>

// Printed in the first log line to identify the installed package. Set at configure
// time by optional/CMakeLists.txt; __DATE__ is only a fallback (frozen to 1980 in the
// reproducible-build devShell via SOURCE_DATE_EPOCH).
#ifndef PS4_APP_BUILD_STAMP
#define PS4_APP_BUILD_STAMP __DATE__ " " __TIME__
#endif
#define PS4_APP_STAMP PS4_APP_BUILD_STAMP

#ifdef __cplusplus
extern "C" {
#endif

// klog + netlog "alive" stamp, and the single read of /app0/ps4-run.cfg. Call it
// first in main(); ps4_log_frame() and ps4_idle_forever() depend on it.
void ps4_app_init(const char* app, const char* stamp);

// The two run-config values. Both are 0 (the console default) until ps4_app_init().
int ps4_run_autoexit(void);
int ps4_run_frame_klog(void);

// Change the "[<app>]" tag without announcing anything (e.g. a test runner tags each
// scene's lines with that scene's name).
void ps4_app_set_tag(const char* app);

// Tagged "[<app>] ..." line on both channels. Bounded call sites only.
void ps4_log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// Same, UDP only - safe to call from a render loop. Also klog with `frame-klog=1`,
// for emulator runs where klog is the only channel read and has no back-pressure.
void ps4_log_frame(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// Pre-formatted line, UDP only, no tag added - for a backend's own log sink. Currently unused
// (see the implementation).
void ps4_log_net_line(const char* line);

// Fatal path. A line from a process about to die cannot rely on UDP: the datagram may never
// leave before the process is killed (klog showed `App Crash reason=0x4` while the handler's
// UDP line never arrived).
//
// So this writes `sceKernelDebugOutText` first and unconditionally (synchronous), then the
// netlog. Use it only in crash handlers; klog costs 8-15 ms per line on this console.
void ps4_log_fatal(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// Hold the process alive with a slow heartbeat instead of returning from main().
// Returns immediately when the host asked for it (`autoexit=1`), and only then.
void ps4_idle_forever(const char* what);

#ifdef __cplusplus
}
#endif
