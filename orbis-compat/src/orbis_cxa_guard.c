/* RetroArch - A frontend for libretro.
 *
 * __cxa_guard_acquire / __cxa_guard_release / __cxa_guard_abort - the C++ runtime's
 * function-local-static guards, replacing libc++abi's on this platform.
 *
 * melondsds aborted on hardware 6.5 seconds into loading content with
 *
 *     [orbis] libc++abi: __cxa_guard_acquire detected recursive initialization
 *
 * although there is no recursive initialization (all 34 guard variables and 39 acquire sites
 * were checked against a whole-module call graph). It is a false positive.
 *
 * libc++abi detects recursion by storing the initialising thread's id in the guard and
 * comparing it against the caller's. In melondsds.elf:
 *
 *     9eba0:  cmpl  $0x0, %edx                       ; init byte PENDING - someone is running it
 *     9ebbd:  callq LazyValue<PlatformThreadID>::get()
 *     9ebd2:  cmpl  (%rcx), %eax                     ; that thread's id == mine ?
 *     9ebe3:  callq abort_message                    ; -> "detected recursive initialization"
 *
 * and PlatformThreadID, in full:
 *
 *     000000000009eae0 <__cxxabiv1::(anonymous namespace)::PlatformThreadID()>:
 *       9eae4:  movl  $0x14, %edi                    ; syscall 20
 *       9eaeb:  callq syscall
 *       9eaf1:  retq
 *
 * Syscall 20 is getpid, as the SDK's headers say:
 *
 *     openorbis/include/bits/syscall.h:18    #define __NR_getpid  20
 *     openorbis/include/bits/syscall.h:410   #define __NR_gettid  __NR_getpid
 *     openorbis/include/bits/syscall.h:827   #define SYS_gettid   __NR_getpid
 *
 * This kernel has no gettid, so musl's headers alias it to getpid. libc++abi's cxa_guard_impl.h
 * takes its `#if defined(SYS_gettid)` arm and gets the process id, the same on every thread, so
 * when two threads race on the same function-local static the second one aborts instead of
 * waiting.
 *
 * Fixing the SDK header would not help: libc++.a is prebuilt with the syscall number baked into
 * cxa_guard.cpp.o. Overriding the three entry points is the only fix short of rebuilding libc++.
 *
 * They are overridden by definition order, as in orbis_abort_report.c. cxa_guard.cpp.o defines
 * only these three strong symbols:
 *
 *     libc++.a(cxa_guard.cpp.o):  T __cxa_guard_acquire  T __cxa_guard_release
 *                                 T __cxa_guard_abort    ... and nothing else strong
 *
 * so that member is never pulled. The core support archive and the frontend's object list both
 * come before -lc++ on their link lines.
 *
 * Only the thread identity changes. The locking (one process-wide statically initialised mutex
 * and condvar) has the same shape as libc++abi's InitByteGlobalMutex; the failing run shows a
 * statically zero-initialised pthread_mutex_t locks correctly on this kernel.
 *
 * The thread identity is %fs:0, the musl TCB self-pointer (on x86-64 the first word of the
 * thread control block points to itself), unique per thread. This module's stdio already relies
 * on it for every character:
 *
 *     000000000008acd4 <__lockfile>:
 *       8acdf:  movq  %fs:0x0, %rcx        ; the TCB
 *       8ace8:  movl  0x38(%rcx), %edx     ; ...this thread's id inside it
 *
 * and that path works on the console (it wrote the abort log).
 *
 * Alternatives considered:
 *
 *   - syscall(SYS_gettid) is the bug.
 *   - pthread_self() is an unresolved import here (`U pthread_self`, supplied by the console's
 *     PRX at load), which would add an import to every core.
 *   - The address of a __thread variable compiles to general-dynamic TLS (`U __tls_get_addr`),
 *     which goes through %fs:0 anyway plus a working DTV, and calls into libc from the C++
 *     runtime's bootstrap. No core currently uses __tls_get_addr.
 *
 * Degradation is towards waiting: if the owner table is full or %fs:0 reads 0 (see
 * owner_is_self), the recursion check is skipped. A missed detection turns a rare genuine
 * recursive initializer into a hang; a false detection kills a working core. libc++abi does the
 * same on platforms without a thread id.
 *
 * Written in C, not C++: a function-local static in this TU would call the functions below.
 *
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>

/* Declared rather than included: this file is linked into both the frontend and every core, and
 * the frontend's ps4 objects do not have the SDK's <orbis/...> headers on their include path. */
int32_t sceKernelDebugOutText(int32_t channel, const char *text, ...);

/* Byte 0 is the ABI; the rest of the guard is ours.
 *
 * The Itanium C++ ABI guard is 8 bytes, and the compiler inlines a fast path that reads the
 * first byte and skips the call when it is non-zero. From anchorPath in melondsds.elf:
 *
 *     80e6d:  movzbl 0x4e3674(%rip), %eax   # _ZGVZN5orbis10anchorRootEvE4root
 *     80e74:  testb  %al, %al
 *     80e76:  je     <slow path: call __cxa_guard_acquire>
 *
 * So byte 0 becomes non-zero exactly when initialization is complete, published with release
 * ordering since that reader does not take our mutex. Byte 1 is private to this file; bytes 2-7
 * are unused. The layout need not match libc++abi's since all three entry points are replaced. */
#define GUARD_DONE_BYTE(g)  ((unsigned char *)(g))
#define GUARD_FLAG_BYTE(g)  (((unsigned char *)(g)) + 1)

#define FLAG_PENDING  0x01u   /* some thread is running the initializer */
#define FLAG_WAITING  0x02u   /* at least one thread is parked on the condvar */

static pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  s_cond  = PTHREAD_COND_INITIALIZER;

/* One instruction: no library call, import, relocation or TLS block. */
static uintptr_t orbis_guard_self(void)
{
   uintptr_t tp = 0;
   __asm__ __volatile__("movq %%fs:0, %0" : "=r" (tp));
   return tp;
}

/* Who is initialising what, for the recursion check: guard address -> owner's thread identity.
 * 64 simultaneous in-flight initializations is far beyond real use; if full, the check is
 * skipped. Accessed only under s_mutex. */
#define OWNERS 64
static struct { uint64_t *guard; uintptr_t owner; } s_owners[OWNERS];

static void owner_set(uint64_t *guard, uintptr_t owner)
{
   int i;
   for (i = 0; i < OWNERS; i++)
   {
      if (!s_owners[i].guard)
      {
         s_owners[i].guard = guard;
         s_owners[i].owner = owner;
         return;
      }
   }
}

static void owner_clear(uint64_t *guard)
{
   int i;
   for (i = 0; i < OWNERS; i++)
   {
      if (s_owners[i].guard == guard)
      {
         s_owners[i].guard = NULL;
         s_owners[i].owner = 0;
         return;
      }
   }
}

/* 1 only when this guard is recorded and owned by this thread. An unrecorded guard (table
 * full) answers 0, i.e. "wait". */
static int owner_is_self(uint64_t *guard, uintptr_t self)
{
   int i;
   if (!self)
      return 0;
   for (i = 0; i < OWNERS; i++)
      if (s_owners[i].guard == guard)
         return s_owners[i].owner == self;
   return 0;
}

int  __cxa_guard_acquire(uint64_t *guard);
void __cxa_guard_release(uint64_t *guard);
void __cxa_guard_abort(uint64_t *guard);

/* Not through orbis_report(): that is only in the core support archive, not the frontend, and a
 * weak reference fails FSELF creation (`missing library for symbol`). Writes to klog directly,
 * which is synchronous, so it links in both images.
 *
 * A function address from this file is printed next to the guard so the module base can be
 * derived from its .elf offset and the static symbolized. */
static void guard_recursion_abort(uint64_t *guard)
{
   char msg[320];

   snprintf(msg, sizeof(msg),
         "[orbis] cxa_guard: RECURSIVE initialization of the static guarded by %p - its "
         "initializer re-entered itself on one thread (__cxa_guard_acquire is at %p in this "
         "image; subtract its .elf offset for the module base)\n",
         (void*)guard, (void*)&__cxa_guard_acquire);

   sceKernelDebugOutText(0, msg);
   abort();
}

int __cxa_guard_acquire(uint64_t *guard)
{
   const uintptr_t self = orbis_guard_self();

   pthread_mutex_lock(&s_mutex);
   for (;;)
   {
      /* Acquire, to pair with the release store below for anything that got here without
       * taking the inlined fast path. */
      if (__atomic_load_n(GUARD_DONE_BYTE(guard), __ATOMIC_ACQUIRE))
      {
         pthread_mutex_unlock(&s_mutex);
         return 0;                      /* already initialized - do not run it again */
      }

      if (!(*GUARD_FLAG_BYTE(guard) & FLAG_PENDING))
      {
         *GUARD_FLAG_BYTE(guard) |= FLAG_PENDING;
         owner_set(guard, self);
         pthread_mutex_unlock(&s_mutex);
         return 1;                      /* ours to initialize */
      }

      /* Someone is already running it; that is only an error if it is this thread. */
      if (owner_is_self(guard, self))
      {
         pthread_mutex_unlock(&s_mutex);
         guard_recursion_abort(guard);  /* does not return */
      }

      /* The statically initialised condvar was not exercised by libc++abi's failing path. If
       * pthread_cond_wait fails without releasing the mutex, spinning with the mutex held would
       * block __cxa_guard_release, so fall back to unlock/sleep/relock on a non-zero return. */
      *GUARD_FLAG_BYTE(guard) |= FLAG_WAITING;
      if (pthread_cond_wait(&s_cond, &s_mutex) != 0)
      {
         pthread_mutex_unlock(&s_mutex);
         usleep(200);
         pthread_mutex_lock(&s_mutex);
      }
   }
}

void __cxa_guard_release(uint64_t *guard)
{
   int wake;

   pthread_mutex_lock(&s_mutex);
   /* Release, so a thread that sees this byte through the compiler's inlined acquire load also
    * sees everything the initializer wrote. */
   __atomic_store_n(GUARD_DONE_BYTE(guard), 1, __ATOMIC_RELEASE);
   wake = (*GUARD_FLAG_BYTE(guard) & FLAG_WAITING) != 0;
   *GUARD_FLAG_BYTE(guard) = 0;
   owner_clear(guard);
   pthread_mutex_unlock(&s_mutex);

   if (wake)
      pthread_cond_broadcast(&s_cond);
}

/* The initializer threw: reset the guard so the next caller tries again. */
void __cxa_guard_abort(uint64_t *guard)
{
   int wake;

   pthread_mutex_lock(&s_mutex);
   wake = (*GUARD_FLAG_BYTE(guard) & FLAG_WAITING) != 0;
   *GUARD_FLAG_BYTE(guard) = 0;
   owner_clear(guard);
   pthread_mutex_unlock(&s_mutex);

   if (wake)
      pthread_cond_broadcast(&s_cond);
}
