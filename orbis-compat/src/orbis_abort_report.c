/* The message a core prints on its way to abort(), on a console where stderr goes nowhere.
 *
 * assert(), libc++abi's abort_message() and __cxa_pure_virtual print the reason to stderr and
 * call abort(), but fd 2 goes nowhere on this kernel, so only a SIGABRT and a backtrace ending
 * at abort() reach the log. Redirecting fd 2 is not possible: dup2 onto 1 or 2 returns EPERM.
 * So the message is caught before it is written.
 *
 * abort_message (abort_message.cpp.o in libc++.a) and __assert_fail (assert.lo in libc.a) each
 * live in an archive member that defines only that symbol. Members are only pulled for symbols
 * still undefined, and the core support archive comes before -lc and -lc++ on the link line
 * (ps4/build-cores.sh), so these definitions win without duplicates.
 *
 * Reports go to klog first: sceKernelDebugOutText is synchronous, while a UDP datagram may not
 * leave a process about to abort() (same reasoning as ps4_log_fatal). The file copy is second.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdarg.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <orbis/libkernel.h>

#include "orbis_report.h"
#include <orbis_env.h>

/* Generic default path (it used to be per-product, e.g. /data/retroarch-abort.log). A title can
 * set ORBIS_ABORT_LOG in /data/orbis-env.txt (read through orbis_env_get, no rebuild needed). */
#define ORBIS_ABORT_LOG_DEFAULT "/data/orbis-abort.log"

/* Re-entrancy flag for the file copy below; see the note at the fopen. */
static int s_in_report;

void orbis_report(const char *tag, const char *fmt, ...)
{
   char    line[1024];
   size_t  n;
   va_list ap;
   FILE   *f;

   n = (size_t)snprintf(line, sizeof(line), "[orbis] %s: ", tag);
   va_start(ap, fmt);
   vsnprintf(line + n, sizeof(line) - n - 2, fmt, ap);
   va_end(ap);
   n = strlen(line);
   line[n]     = '\n';
   line[n + 1] = '\0';

   /* First, and unconditionally. */
   sceKernelDebugOutText(0, line);

   /* Append, not truncate: a core can abort during the menu's info read and again when loaded,
    * and the first message usually explains the second.
    *
    * Guarded against re-entry: fopen reaches the interposed open(), then anchorPath() and
    * anchorRoot() (a function-local static). anchorPath returns early for absolute paths, before
    * touching the guard:
    *
    *     80e5e:  cmpl   $0x2f, %eax           ; '/'
    *     80e61:  je     0x80f39               ; ...returns here
    *     80e6d:  movzbl _ZGVZN5orbis10anchorRootEvE4root(%rip), %eax   ; guard, only after that
    *
    * but with a relative path, or another interposer with a static, re-entry would deadlock on
    * the static-init guard (orbis_cxa_guard.c waits on a condvar).
    *
    * klog stays outside the guard since it cannot re-enter. A global flag rather than a
    * thread-local: concurrent reports lose one file copy but keep both klog lines. */
   if (__atomic_exchange_n(&s_in_report, 1, __ATOMIC_ACQ_REL) == 0)
   {
      /* orbis_env_get, not getenv: setenv() in one image is invisible to another. Read at report
       * time rather than cached; this runs once per process death. */
      const char *logpath = orbis_env_get("ORBIS_ABORT_LOG");
      if (logpath == NULL || *logpath == '\0')
         logpath = ORBIS_ABORT_LOG_DEFAULT;
      if ((f = fopen(logpath, "a")))
      {
         fputs(line, f);
         fflush(f);
         fclose(f);
      }
      __atomic_store_n(&s_in_report, 0, __ATOMIC_RELEASE);
   }
}

/* libc++abi's. Reached by std::terminate, uncaught exceptions, __cxa_pure_virtual, etc. The
 * message carries the diagnosis ("terminating with uncaught exception of type ..."). */
void abort_message(const char *format, ...)
{
   char    msg[960];
   va_list ap;

   va_start(ap, format);
   vsnprintf(msg, sizeof(msg), format, ap);
   va_end(ap);

   orbis_report("libc++abi", "%s", msg);
   abort();
}

/* musl's. Still reachable with -DNDEBUG: libretro-common, orbis-compat and vendored dependencies
 * may be built without it, and their assertions otherwise look like any other SIGABRT. */
void __assert_fail(const char *expr, const char *file, int line, const char *func)
{
   char msg[960];

   snprintf(msg, sizeof(msg), "%s:%d: %s: assertion failed: %s",
         file ? file : "?", line, func ? func : "?", expr ? expr : "?");

   orbis_report("assert", "%s", msg);
   abort();
}

/* abort() itself, for code that aborts without a message (libunwind's _LIBUNWIND_ABORT, GNU
 * lightning on an unencodable instruction, paraLLEl-RSP's allocator when its arena is full).
 * Cores are built with -fomit-frame-pointer and the console's backtracer cannot walk them, so
 * the log only shows abort().
 *
 * The return address identifies the caller even without frame pointers. The address of
 * orbis_report is printed too, so the module base (and the offset to symbolize) can be derived
 * from its .elf offset.
 *
 * musl's abort.lo also defines __abort_lock; since that member is no longer pulled, it is
 * defined here.
 */
volatile int __abort_lock[1];

void abort(void)
{
   char  msg[512];
   void *ra = __builtin_return_address(0);

   snprintf(msg, sizeof(msg),
         "abort() called from %p  (orbis_report is at %p in this image - "
         "subtract its .elf offset for the module base)",
         ra, (void*)&orbis_report);
   orbis_report("abort", "%s", msg);

   /* Die the usual way, so crash dumps and reason codes stay comparable. */
   raise(SIGABRT);
   _Exit(134);
}
