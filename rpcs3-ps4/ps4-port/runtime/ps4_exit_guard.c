/* PS4 exit guard (0.25).
 *
 * musl exit() runs static destructors (__funcs_on_exit, libc_exit_fini) while emulation
 * threads still live, and then calls _Exit -> libkernel _exit, which this kernel answers
 * with SIGSYS: the console shows CE-34878-0 and nothing is recorded. _Exit alone ends the
 * same way. Route both to the PS4 fatal policy (rpcs3/ps4_fatal.cpp: record fatal.txt +
 * startup.log, notify, park; only the shell's "Close application" ends the process).
 *
 * Linked as an object of rpcs3_ps4_frontend, not from an archive: object definitions are
 * seen before libc.a is scanned, so lld never extracts exit.lo or _Exit.lo (each defines
 * only that one strong symbol plus weak helpers). quick_exit.lo still references _Exit and
 * therefore lands here too.
 *
 * Post-link check: `llvm-nm rpcs3_ps4.elf` shows exactly one `T exit` and one `T _Exit`,
 * both from this object (`__funcs_on_exit` stays: musl defines it in atexit.lo together
 * with __cxa_atexit, so its presence is expected).
 */
#include <stdlib.h>

_Noreturn void ps4_exit_called(int code);

_Noreturn void exit(int code)
{
    ps4_exit_called(code);
}

_Noreturn void _Exit(int code)
{
    ps4_exit_called(code);
}
