/* Reporting channel for a core whose process is about to die.
 *
 * Not stderr: fd 2 is not connected on this console and dup2 onto it returns EPERM. Not libretro's
 * log_cb: it is NULL until the core is given one, which is after a module's global constructors
 * run, where these failures happen.
 *
 * So this writes sceKernelDebugOutText first and unconditionally. It is synchronous, unlike a UDP
 * datagram from a process about to abort(). It costs 8-15 ms per line, so use it only for
 * initialisation and failures.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef ORBIS_REPORT_H__
#define ORBIS_REPORT_H__

#ifdef __cplusplus
extern "C" {
#endif

/* Tagged, formatted, klog first and then /data/retroarch-abort.log. */
void orbis_report(const char *tag, const char *fmt, ...)
      __attribute__((format(printf, 2, 3)));

#ifdef __cplusplus
}
#endif

#endif
