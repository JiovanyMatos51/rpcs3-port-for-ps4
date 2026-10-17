// Copyright © 2026 Mikołaj Mikołajczyk
// SPDX-License-Identifier: MIT
//
// Logging hook for this library's interposers (stat translation, operator new failure reports...),
// so they do not depend on any particular engine's logger.
//
// Nothing is registered by default and the calls are no-ops until something is. An application
// that wants the output calls orbis_set_log once, early.
#ifndef _ORBIS_LOG_H
#define _ORBIS_LOG_H

#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

/// The shape of a logger: printf, as far as these call sites are concerned.
typedef void (*orbis_log_fn)(const char *fmt, va_list ap);

/// Installs the logger, or removes it with nullptr. Not thread-safe against concurrent logging,
/// which is deliberate: it is meant to be called once before the threads exist.
void orbis_set_log(orbis_log_fn fn);

/// Writes a line through whatever was registered. A no-op if nothing was.
void orbis_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/// True when a logger is installed, for call sites that would otherwise build a message for nobody.
int orbis_log_enabled(void);

/// Separate channel for lines written by a process about to die. The normal logger may be UDP, and a
/// datagram from a process being killed may never leave the machine; a crash handler needs a channel
/// that has written before it returns. Only the application knows which channel that is.
void orbis_set_log_fatal(orbis_log_fn fn);

/// Writes through the fatal logger, or through the ordinary one if none was registered, or nowhere.
void orbis_log_fatal(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/// What to do once a fatal event has been reported (idle for a debugger, exit...). This is the
/// application's policy; a title that exits only gets a console error dialog.
/// `what` names the event: "terminate" or "signal". If the callback returns, the library exits.
typedef void (*orbis_fatal_action_fn)(const char *what);
void orbis_set_fatal_action(orbis_fatal_action_fn fn);
void orbis_fatal_action(const char *what);

#ifdef __cplusplus
}
#endif

#endif /* _ORBIS_LOG_H */
