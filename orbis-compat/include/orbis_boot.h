// Copyright © 2026 Mikołaj Mikołajczyk
// SPDX-License-Identifier: MIT
/* Two title-independent boot helpers, originally from OpenGothic's ps4/og_ps4_boot.cpp:
 *
 *   probeCtype()           checks whether this musl folds case.
 *   installCrashHandlers() reports fatal signals; it lives here because this library also
 *                          supplies backtrace(3).
 *
 * The library only reports. After a fatal event it calls orbis_fatal_action(), registered by the
 * application, which decides whether to idle (keeps the screen, debugger can attach) or exit (the
 * system shows an error dialog).
 */
#ifndef _ORBIS_BOOT_H
#define _ORBIS_BOOT_H

namespace orbis {

/// Reports through orbis_log whether std::tolower/toupper fold ASCII, and says what breaks if not.
void probeCtype();

/// Installs a std::terminate handler and SA_SIGINFO handlers for SIGSEGV/BUS/FPE/ILL/ABRT, on an
/// alternate stack so that a stack overflow can still report itself. Reports what it managed.
void installCrashHandlers();

}

#endif /* _ORBIS_BOOT_H */
