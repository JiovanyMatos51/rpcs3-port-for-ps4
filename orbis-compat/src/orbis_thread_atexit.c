/* Copied from RetroArch ps4/ for the Sonic 3 A.I.R. PS4 port (MIT, same maintainer).
 *
 * __cxa_thread_atexit_impl - the destructor hook this SDK's libc++ calls and its libc never
 * defines.
 *
 * A `thread_local` object with a non-trivial destructor makes the compiler call
 * __cxa_thread_atexit (libc++.a), which forwards to __cxa_thread_atexit_impl. glibc and musl
 * export that; this SDK's libc does not, so the first core using such an object fails to link:
 *
 *     ld.lld: error: undefined symbol: __cxa_thread_atexit_impl
 *     >>> referenced by cxa_thread_atexit.cpp
 *     >>>               cxa_thread_atexit.cpp.o:(__cxa_thread_atexit) in archive
 *     >>>               .../openorbis/lib/libc++.a
 *
 * (first seen with flycast). A no-op stub would link but silently skip every thread-local
 * destructor, leaking buffers, handles and mutexes per thread, so it is implemented.
 *
 * As glibc documents: destructors run on the registering thread, in reverse order, at thread
 * exit, via a pthread_key destructor (the SDK's libc.a has the pthread_key family).
 *
 * Known gap: the main thread never runs key destructors (it returns from main() or calls exit()),
 * so thread_local destructors registered there do not run at process exit. Harmless here since
 * the module is unloaded with the process.
 *
 * `dso_handle` is ignored: it only guards dlclose(), and cores are linked into one static module.
 *
 * Built into the core-build support archive, so it is only pulled in when the symbol is
 * undefined and nothing else provides it.
 *
 * SPDX-License-Identifier: MIT
 */
#include <pthread.h>
#include <stdlib.h>

struct orbis_thread_dtor
{
   void                     (*func)(void *);
   void                      *obj;
   struct orbis_thread_dtor  *next;
};

static pthread_key_t  orbis_thread_dtor_key;
static pthread_once_t orbis_thread_dtor_once = PTHREAD_ONCE_INIT;

/* Entries are pushed at the head, so walking forward runs them in reverse registration order. */
static void orbis_thread_dtor_run(void *head)
{
   struct orbis_thread_dtor *d = (struct orbis_thread_dtor*)head;

   while (d)
   {
      struct orbis_thread_dtor *next = d->next;
      d->func(d->obj);
      free(d);
      d = next;
   }
}

static void orbis_thread_dtor_init(void)
{
   pthread_key_create(&orbis_thread_dtor_key, orbis_thread_dtor_run);
}

int __cxa_thread_atexit_impl(void (*func)(void *), void *obj, void *dso_handle);

int __cxa_thread_atexit_impl(void (*func)(void *), void *obj, void *dso_handle)
{
   struct orbis_thread_dtor *d;

   (void)dso_handle;

   if (pthread_once(&orbis_thread_dtor_once, orbis_thread_dtor_init) != 0)
      return -1;

   if (!(d = (struct orbis_thread_dtor*)malloc(sizeof(*d))))
      return -1;

   d->func = func;
   d->obj  = obj;
   d->next = (struct orbis_thread_dtor*)pthread_getspecific(orbis_thread_dtor_key);

   /* pthread_setspecific arms the destructor; on failure drop the entry. */
   if (pthread_setspecific(orbis_thread_dtor_key, d) != 0)
   {
      free(d);
      return -1;
   }

   return 0;
}
