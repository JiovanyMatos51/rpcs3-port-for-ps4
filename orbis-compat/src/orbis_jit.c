/* One arena of writable, executable memory, shared by every recompiler in a module.
 * See include/orbis_jit.h for what the console allows; this file is the implementation.
 *
 * A suballocator, because one kernel mapping per consumer failed on hardware:
 *
 *   paraLLEl-RSP mapped a 64 MiB arena per module load, and module destructors do not run here,
 *   so after four loads (256 MiB) the next got `no direct memory for 65536 KiB -> 0x80020023`
 *   (EAGAIN).
 *
 *   Play!'s CodeGen mapped one 16 KiB page per compiled basic block; Grand Theft Auto III reached
 *   its intro at 3-5 fps and then wrote to (void*)-1 when the kernel stopped handing them out.
 *
 * One arena, grown on demand, with a coalescing free list serves flycast's three code caches,
 * mupen64plus's two recompilers and Play!'s thousands of blocks.
 *
 * SPDX-License-Identifier: MIT
 */
#include <orbis_jit.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <orbis/libkernel.h>

#include <orbis_env.h>
#include "orbis_report.h"

/* FreeBSD's MAP_FIXED, as Sony's mapper takes it. The SDK headers do not name it; mesa-ps4's
 * ac_orbis_drm.c uses the same constant. */
#define ORBIS_JIT_MAP_FIXED 0x0010

#define ORBIS_JIT_PROT_RW  (ORBIS_KERNEL_PROT_CPU_RW)                              /* 0x03 */
#define ORBIS_JIT_PROT_RWX (ORBIS_KERNEL_PROT_CPU_RW | ORBIS_KERNEL_PROT_CPU_EXEC) /* 0x07 */

/* sceKernelVirtualQuery returns a 32-character name per mapping; it identifies code arenas when
 * walking the address space (e.g. beetle-psx's orbis_lr_range_free prints it). */
#define ORBIS_JIT_ARENA_NAME "orbis_jit"

/* A rel32 displacement is signed 32-bit, so the reach is 2 GiB either way... */
#define ORBIS_JIT_REACH      0x80000000ull
/* ...minus how much the module itself is assumed to span, since the anchor below is one address
 * inside it. flycast has 47 MiB of code caches and swanstation 50 MiB of .bss; 256 MiB leaves a
 * 5x margin. */
#define ORBIS_JIT_MODULE_SPAN 0x10000000ull
/* ...leaving 1.5 GiB on either side for the arena. Derived, so the search below and the re-check
 * in orbis_jit_alloc stay consistent. */
#define ORBIS_JIT_WINDOW     (ORBIS_JIT_REACH - 2ull * ORBIS_JIT_MODULE_SPAN)

/* Blocks are cache-line aligned: 16 would satisfy every emitter, 64 also keeps a store into one
 * block off the line another block is executing from (e.g. Play! compiling while the EE thread
 * runs).
 *
 * One such unit in front of every block stores its length, since orbis_jit_free takes no size
 * (dynarmic's POSIX allocator uses a whole page for this, block_of_code.cpp:81-102). A block
 * overrun therefore corrupts the next block's length and shows up as a bad free. */
#define ORBIS_JIT_ALIGN 64u

#define ORBIS_JIT_MAX_ARENAS 8
/* Free spans only exist between live blocks and the list coalesces on every free. Running out is
 * not fatal: the span is dropped (leaked) and reported. */
#define ORBIS_JIT_MAX_SPANS  256

typedef struct {
   uint8_t *at;
   size_t   len;
} orbis_jit_span;

typedef struct {
   uint8_t       *base;
   size_t         size;
   off_t          phys;
   /* An arena from orbis_jit_alloc_at belongs to one caller at one address: it is never
    * suballocated, and freeing it unmaps it rather than returning a span. */
   int            dedicated;
   unsigned       nspans;
   orbis_jit_span span[ORBIS_JIT_MAX_SPANS];
} orbis_jit_arena;

/* .bss, so it is zero before any dynamic initialiser runs: the first caller can be a global
 * constructor (RSP::JIT::CPU, constructed while the frontend's menu reads a core's info). */
static orbis_jit_arena s_arena[ORBIS_JIT_MAX_ARENAS];
static unsigned        s_arenas;

/* -1 not asked, 0 refused, 1 granted and executed. See orbis_jit_state. */
static int s_state = -1;

/* ---------------------------------------------------------------------------- reporting */

/* Sony returns 0x8002xxxx with an errno in the low half. EACCES means refused on policy, EINVAL
 * means the request was malformed. */
static const char *orbis_jit_err(int32_t rc)
{
   switch ((uint32_t)rc)
   {
      case 0x80020001u: return "EPERM - not permitted";
      case 0x80020009u: return "EBADF - the flags word was not anonymous";
      case 0x8002000Cu: return "ENOMEM - out of memory or address space";
      case 0x8002000Du: return "EACCES - understood and refused on policy";
      case 0x8002000Eu: return "EFAULT - bad address argument";
      case 0x80020016u: return "EINVAL - malformed request, policy never reached";
      case 0x80020023u: return "EAGAIN - the direct memory pool is exhausted";
      default:          return "not in this file's table";
   }
}

/* Quiet on the repeating path: orbis_report is a synchronous 8-15 ms write, and the frontend loads
 * and unloads cores while browsing the menu (two full cycles in 82 ms). Failures are always
 * reported; success only for the first arena.
 *
 * orbis_env_get, not getenv: this file ends up inside a .prx with its own musl `environ`, which
 * the eboot's setenv() never reaches. scripts/check-env-knobs.sh checks for this. */
static int s_verbose = -1;

static int orbis_jit_should_log(void)
{
   if (s_verbose < 0)
   {
      const char *const e = orbis_env_get("ORBIS_JIT_VERBOSE");
      s_verbose = (e && *e && *e != '0') ? 1 : 0;
   }
   return s_verbose;
}

/* ---------------------------------------------------------------------------- the lock */

/* A spin lock, not a pthread_mutex_t: every ScePthread object comes from libkernel's internal
 * memory (technote 235), which can run out, and this allocator must keep working when it does. */
static volatile int s_lock;

static void orbis_jit_lock(void)
{
   while (__atomic_exchange_n(&s_lock, 1, __ATOMIC_ACQUIRE))
      sceKernelUsleep(50);
}

static void orbis_jit_unlock(void)
{
   __atomic_store_n(&s_lock, 0, __ATOMIC_RELEASE);
}

/* ---------------------------------------------------------------------------- rounding */

static size_t orbis_jit_round(size_t n, size_t to)
{
   return (n + (to - 1)) & ~(size_t)(to - 1);
}

/* ---------------------------------------------------------------------------- the proof */

/* Verifies execution by running code.
 *
 * sceKernelQueryMemoryProtection reports the protection a range was mapped with, not the result of
 * a later mprotect (it returned 0x03 for ranges executing code, 16 out of 16), so it would veto a
 * working recompiler. Instead, six bytes of x86-64 are written into the buffer and called:
 *
 *     b8 ee ff c0 00   mov eax, 0x00c0ffee
 *     c3               ret
 *
 * x86-64 needs no cache maintenance for this. A page that will not execute kills the process, so
 * the attempt is logged before the call; better here, at a known point, than on the first
 * recompiled block. */
static int s_first_stub = 1;

static int orbis_jit_verify(void *at)
{
   static const uint8_t stub[] = { 0xb8, 0xee, 0xff, 0xc0, 0x00, 0xc3 };
   uint32_t (*fn)(void);
   uint32_t got;

   memcpy(at, stub, sizeof(stub));
   /* Not a cast: an object pointer to a function pointer is not one. */
   memcpy(&fn, &at, sizeof(fn));

   if (s_first_stub || orbis_jit_should_log())
      orbis_report("jit", "calling a stub at %p to prove the promotion. If this is the last jit "
                          "line, the promotion to 0x07 was granted and not honoured, and that is "
                          "the answer rather than a crash to chase.", at);
   s_first_stub = 0;

   got = fn();
   if (got != 0x00c0ffeeu)
   {
      orbis_report("jit", "stub at %p ran and returned 0x%08x, not 0x00c0ffee - not trusting it "
                          "with recompiled code", at, (unsigned)got);
      return 0;
   }
   return 1;
}

/* The module-wide state only gets worse: one buffer that will not run makes the recompiler
 * unusable regardless of later successes. */
static void orbis_jit_record(int ok)
{
   s_state = (s_state < 0) ? ok : (s_state && ok);
}

/* ---------------------------------------------------------------------------- reachability */

/* The address this module's rel32 displacements are measured from: a function in this object.
 * orbis_jit.o is an archive member pulled into whichever module references it, so this file is
 * always in the calling core's module. */
static uintptr_t orbis_jit_anchor(void)
{
   return (uintptr_t)(void *)&orbis_jit_alloc;
}

/* Whether an arena at [base, base+size) is reachable from anywhere in this module. */
static int orbis_jit_near_module(const uint8_t *base, size_t size)
{
   const uintptr_t anchor = orbis_jit_anchor();
   return orbis_jit_reachable((const void *)(anchor - ORBIS_JIT_MODULE_SPAN),
                              2 * ORBIS_JIT_MODULE_SPAN, base, size);
}

int orbis_jit_reachable(const void *from, size_t len, const void *to, size_t len2)
{
   const uintptr_t a = (uintptr_t)from;
   const uintptr_t b = (uintptr_t)to;
   const uintptr_t lo = (a < b) ? a : b;
   const uintptr_t hi = ((a + len) > (b + len2)) ? (a + len) : (b + len2);

   /* Conservative: the distance between the outermost ends bounds every displacement in both
    * directions. */
   return (hi - lo) < ORBIS_JIT_REACH;
}

/* ---------------------------------------------------------------------------- address search */

/* MAP_FIXED on this kernel replaces existing mappings and there is no MAP_FIXED_NOREPLACE, so
 * every fixed mapping checks the range first.
 *
 * sceKernelVirtualQuery is called with flags=0, where a nonzero return means nothing is mapped
 * (the meaning established by mesa-ps4's ac_orbis_drm.c). flags=1 is undocumented; if it made the
 * call fail, every address would look free and MAP_FIXED would land on the heap.
 *
 * Taken granules are skipped using the containing mapping's end from the info block, instead of
 * walking one 16 KiB granule at a time (114688 calls to cross 1.75 GiB): one call per mapping in
 * the way plus one per granule of the chosen span.
 *
 * If the end field is not past the queried address (a misread struct), the walk advances one
 * granule, so a misread costs speed, not correctness. */
static uint8_t *orbis_jit_find_free(uintptr_t lo, uintptr_t hi, size_t len)
{
   const uintptr_t granule = ORBIS_JIT_GRANULE;
   uintptr_t       p       = (lo + (granule - 1)) & ~(granule - 1);
   uintptr_t       run     = p;

   while (p + granule <= hi)
   {
      OrbisKernelVirtualQueryInfo info;
      memset(&info, 0, sizeof(info));

      if (sceKernelVirtualQuery((const void *)p, 0, &info, sizeof(info)) == 0)
      {
         const uintptr_t end = (uintptr_t)info.unk02;
         p   = (end > p) ? ((end + (granule - 1)) & ~(granule - 1)) : (p + granule);
         run = p;
         continue;
      }

      p += granule;
      if (p - run >= len)
         return (uint8_t *)run;
   }
   return NULL;
}

/* ---------------------------------------------------------------------------- arena creation */

/* Map `len` bytes of direct memory read-write, promote them, and prove they run. `at` is NULL to
 * let the kernel choose or an address to insist on. */
static int orbis_jit_map(orbis_jit_arena *a, void *at, size_t len)
{
   off_t   phys = 0;
   void   *got  = at;
   int32_t rc;

   rc = sceKernelAllocateDirectMemory(0, (off_t)sceKernelGetDirectMemorySize(), len,
                                      ORBIS_JIT_GRANULE, ORBIS_KERNEL_WB_ONION, &phys);
   if (rc != 0)
   {
      orbis_report("jit", "no direct memory for %lu KiB -> 0x%08x (%s)",
                   (unsigned long)(len / 1024), (unsigned)rc, orbis_jit_err(rc));
      return 0;
   }

   /* Read-write at map time, execute only afterwards: asking sceKernelMapDirectMemory for
    * READ|EXECUTE up front returns 0x8002000d (EACCES). */
   rc = sceKernelMapNamedDirectMemory(&got, len, ORBIS_JIT_PROT_RW,
                                      at ? ORBIS_JIT_MAP_FIXED : 0,
                                      phys, ORBIS_JIT_GRANULE, ORBIS_JIT_ARENA_NAME);
   if (rc != 0 || !got || (at && got != at))
   {
      orbis_report("jit", "could not map %lu KiB at %p -> 0x%08x (%s), landed at %p",
                   (unsigned long)(len / 1024), at, (unsigned)rc, orbis_jit_err(rc), got);
      if (rc == 0 && got)
         sceKernelMunmap(got, len);
      sceKernelReleaseDirectMemory(phys, len);
      return 0;
   }

   rc = sceKernelMprotect(got, len, ORBIS_JIT_PROT_RWX);
   if (rc != 0)
   {
      orbis_report("jit", "%lu KiB at %p would not take execute -> 0x%08x (%s). The recompiler "
                          "has nowhere to write and must not be used.",
                   (unsigned long)(len / 1024), got, (unsigned)rc, orbis_jit_err(rc));
      sceKernelMunmap(got, len);
      sceKernelReleaseDirectMemory(phys, len);
      orbis_jit_record(0);
      return 0;
   }

   if (!orbis_jit_verify(got))
   {
      sceKernelMunmap(got, len);
      sceKernelReleaseDirectMemory(phys, len);
      orbis_jit_record(0);
      return 0;
   }

   a->base = (uint8_t *)got;
   a->size = len;
   a->phys = phys;
   orbis_jit_record(1);
   return 1;
}

/* How big an arena to take when one is needed. Never smaller than the request that triggered it. */
static size_t orbis_jit_arena_size(size_t atleast)
{
   /* 16 MiB, small on purpose (64 MiB per load exhausted the pool, see the file header). Shared
    * by the whole module and grown on demand, so this is not a ceiling. ORBIS_JIT_ARENA_MB
    * overrides it. */
   size_t      mb = 16;
   const char *e  = orbis_env_get("ORBIS_JIT_ARENA_MB");

   if (e && *e)
   {
      const long v = strtol(e, NULL, 10);
      if (v > 0 && v <= 512)
         mb = (size_t)v;
   }

   {
      const size_t want = mb * 1024u * 1024u;
      return orbis_jit_round((atleast > want) ? atleast : want, ORBIS_JIT_GRANULE);
   }
}

/* Take a new arena. `flags` carries ORBIS_JIT_NEAR_TEXT, which chooses the address by search. */
static orbis_jit_arena *orbis_jit_grow(size_t atleast, unsigned flags)
{
   orbis_jit_arena *a;
   size_t           len;

   if (s_arenas >= ORBIS_JIT_MAX_ARENAS)
   {
      orbis_report("jit", "all %d arena slots are in use and %lu KiB more was asked for",
                   ORBIS_JIT_MAX_ARENAS, (unsigned long)(atleast / 1024));
      return NULL;
   }

   a   = &s_arena[s_arenas];
   len = orbis_jit_arena_size(atleast);

   if (flags & ORBIS_JIT_NEAR_TEXT)
   {
      const uintptr_t anchor = orbis_jit_anchor();
      uint8_t        *at;

      /* Above the module first: arenas have always landed there, so the search usually ends at
       * the first free granule. Below is the fallback. */
      at = orbis_jit_find_free(anchor + ORBIS_JIT_MODULE_SPAN, anchor + ORBIS_JIT_WINDOW, len);
      if (!at)
         at = orbis_jit_find_free(anchor - ORBIS_JIT_WINDOW, anchor - ORBIS_JIT_MODULE_SPAN, len);

      if (!at)
      {
         orbis_report("jit", "no free %lu KiB within rel32 reach of this module (anchor %p). A "
                             "buffer outside it would not run slower, it would jump somewhere "
                             "arbitrary, so this is a refusal rather than a fallback.",
                      (unsigned long)(len / 1024), (void *)anchor);
         return NULL;
      }

      if (!orbis_jit_map(a, at, len))
         return NULL;
   }
   else if (!orbis_jit_map(a, NULL, len))
      return NULL;

   /* Explicit, not relying on .bss: freeing a dedicated arena compacts the table, so slots are
    * reused. */
   a->dedicated   = 0;
   a->nspans      = 1;
   a->span[0].at  = a->base;
   a->span[0].len = a->size;
   s_arenas++;

   if (s_arenas == 1 || orbis_jit_should_log())
      orbis_report("jit", "arena %u: %lu KiB at %p%s", s_arenas, (unsigned long)(len / 1024),
                   (void *)a->base,
                   (flags & ORBIS_JIT_NEAR_TEXT) ? ", within rel32 reach of this module" : "");
   return a;
}

/* ---------------------------------------------------------------------------- the free list */

static void *orbis_jit_take(orbis_jit_arena *a, size_t len)
{
   unsigned i;

   for (i = 0; i < a->nspans; i++)
   {
      uint8_t *at;

      if (a->span[i].len < len)
         continue;

      at = a->span[i].at;
      if (a->span[i].len == len)
      {
         memmove(&a->span[i], &a->span[i + 1],
                 (size_t)(a->nspans - i - 1) * sizeof(a->span[0]));
         a->nspans--;
      }
      else
      {
         a->span[i].at  += len;
         a->span[i].len -= len;
      }
      return at;
   }
   return NULL;
}

/* Put a block back and coalesce with its neighbours (the list is sorted by address). Coalescing
 * is required: Play! frees and reallocates a span per invalidated basic block, thousands of times
 * a session, which would otherwise fragment the arena. */
static void orbis_jit_give(orbis_jit_arena *a, uint8_t *at, size_t len)
{
   unsigned i = 0;

   while (i < a->nspans && a->span[i].at < at)
      i++;

   if (a->nspans >= ORBIS_JIT_MAX_SPANS)
   {
      orbis_report("jit", "no free-list slot for %lu KiB at %p - that block is leaked until the "
                          "arena is released", (unsigned long)(len / 1024), (void *)at);
      return;
   }

   memmove(&a->span[i + 1], &a->span[i], (size_t)(a->nspans - i) * sizeof(a->span[0]));
   a->span[i].at  = at;
   a->span[i].len = len;
   a->nspans++;

   if ((i + 1) < a->nspans && a->span[i].at + a->span[i].len == a->span[i + 1].at)
   {
      a->span[i].len += a->span[i + 1].len;
      memmove(&a->span[i + 1], &a->span[i + 2],
              (size_t)(a->nspans - i - 2) * sizeof(a->span[0]));
      a->nspans--;
   }
   if (i > 0 && a->span[i - 1].at + a->span[i - 1].len == a->span[i].at)
   {
      a->span[i - 1].len += a->span[i].len;
      memmove(&a->span[i], &a->span[i + 1],
              (size_t)(a->nspans - i - 1) * sizeof(a->span[0]));
      a->nspans--;
   }
}

/* ---------------------------------------------------------------------------- the interface */

void *orbis_jit_alloc(size_t size, unsigned flags)
{
   const size_t len = orbis_jit_round(size ? size : 1, ORBIS_JIT_ALIGN) + ORBIS_JIT_ALIGN;
   void        *at  = NULL;
   unsigned     i;

   orbis_jit_lock();

   for (i = 0; i < s_arenas && !at; i++)
   {
      if (s_arena[i].dedicated)
         continue;
      /* A NEAR_TEXT request is only served from a reachable arena. Arenas are created with and
       * without the flag by different consumers, so reachability is checked per arena. */
      if ((flags & ORBIS_JIT_NEAR_TEXT) &&
          !orbis_jit_near_module(s_arena[i].base, s_arena[i].size))
         continue;
      at = orbis_jit_take(&s_arena[i], len);
   }

   if (!at)
   {
      orbis_jit_arena *a = orbis_jit_grow(len, flags);
      if (a)
         at = orbis_jit_take(a, len);
   }

   if (at)
   {
      memcpy(at, &len, sizeof(len));
      at = (uint8_t *)at + ORBIS_JIT_ALIGN;
   }

   orbis_jit_unlock();
   return at;
}

void *orbis_jit_alloc_at(void *addr, size_t size)
{
   const size_t len = orbis_jit_round(size ? size : 1, ORBIS_JIT_GRANULE);
   void        *at  = NULL;

   if (!addr || ((uintptr_t)addr & (ORBIS_JIT_GRANULE - 1)))
   {
      orbis_report("jit", "%p is not a %u-byte aligned address, which sceKernelMapDirectMemory "
                          "rejects as malformed rather than refusing on policy",
                   addr, ORBIS_JIT_GRANULE);
      return NULL;
   }

   orbis_jit_lock();

   if (s_arenas >= ORBIS_JIT_MAX_ARENAS)
      orbis_report("jit", "all %d arena slots are in use", ORBIS_JIT_MAX_ARENAS);
   else if (!orbis_jit_find_free((uintptr_t)addr, (uintptr_t)addr + len, len))
      orbis_report("jit", "%p is already mapped, and MAP_FIXED here would replace whatever is "
                          "there rather than fail", addr);
   else if (orbis_jit_map(&s_arena[s_arenas], addr, len))
   {
      s_arena[s_arenas].dedicated = 1;
      at = s_arena[s_arenas].base;
      s_arenas++;
   }

   orbis_jit_unlock();
   return at;
}

void orbis_jit_free(void *addr)
{
   unsigned i;

   if (!addr)
      return;

   orbis_jit_lock();

   for (i = 0; i < s_arenas; i++)
   {
      uint8_t *const at = (uint8_t *)addr;

      if (at < s_arena[i].base || at >= s_arena[i].base + s_arena[i].size)
         continue;

      if (s_arena[i].dedicated)
      {
         if (at != s_arena[i].base)
         {
            /* A dedicated arena is one block; an interior pointer must not unmap it. */
            orbis_report("jit", "%p is inside the dedicated arena at %p but is not its base",
                         addr, (void *)s_arena[i].base);
            break;
         }
         /* munmap alone would leave the physical pages allocated: direct memory is released by
          * its physical offset, which this table keeps. */
         sceKernelMunmap(s_arena[i].base, s_arena[i].size);
         sceKernelReleaseDirectMemory(s_arena[i].phys, s_arena[i].size);
         memmove(&s_arena[i], &s_arena[i + 1],
                 (size_t)(s_arenas - i - 1) * sizeof(s_arena[0]));
         s_arenas--;
         memset(&s_arena[s_arenas], 0, sizeof(s_arena[0]));
      }
      else
      {
         uint8_t *const blk = at - ORBIS_JIT_ALIGN;
         size_t         len = 0;

         memcpy(&len, blk, sizeof(len));
         /* An invalid length means the header was overwritten (block overrun) or the pointer was
          * not ours. Leak the block rather than put an arbitrary span on the free list. */
         if (len < 2 * ORBIS_JIT_ALIGN || (len & (ORBIS_JIT_ALIGN - 1)) ||
             blk + len > s_arena[i].base + s_arena[i].size)
            orbis_report("jit", "the block header in front of %p reads %lu, which is not a length "
                                "this file wrote - leaking it rather than handing that span out "
                                "again", addr, (unsigned long)len);
         else
            orbis_jit_give(&s_arena[i], blk, len);
      }
      break;
   }

   orbis_jit_unlock();
}

void orbis_jit_release_all(void)
{
   unsigned i;

   orbis_jit_lock();
   for (i = 0; i < s_arenas; i++)
   {
      sceKernelMunmap(s_arena[i].base, s_arena[i].size);
      sceKernelReleaseDirectMemory(s_arena[i].phys, s_arena[i].size);
   }
   memset(s_arena, 0, sizeof(s_arena));
   s_arenas = 0;
   orbis_jit_unlock();
}

/* ---------------------------------------------------------------------------- promotion */

/* Promoted ranges are tracked individually: flycast has three in-module caches (SH4_TCB 11 MiB,
 * ARM7_TCB 4 MiB, AICA DSP 32 KiB), each needing its own mprotect; a single cached answer would
 * leave the others unpromoted (SEGV_ACCERR on first write).
 *
 * Overlap is checked, not just containment: melonDS asks twice with the same length (131072 KiB)
 * and bases 224 KiB apart (0x800e14000, 0x800e4c000); with containment only, the two calls
 * alternated, each rewriting the verification stub over generated code. 16 slots is more than any
 * core here needs. */
#define ORBIS_JIT_MAX_PROMOTED 16
static struct { uintptr_t base, end; int ok; } s_promoted[ORBIS_JIT_MAX_PROMOTED];
static unsigned s_promoted_n;

int orbis_jit_protect(void *addr, size_t len)
{
   uintptr_t base = (uintptr_t)addr;
   uintptr_t end  = base + len;
   int       overlap = -1;
   int       ok;
   int32_t   rc;
   unsigned  i;

   /* Rounded outwards: sceKernelMprotect works in 16 KiB units and a caller's array may only be
    * 4 KiB aligned (mupen64plus uses ALIGN(4096)); rounding the base up would leave the buffer's
    * first pages, where the first block is written, unpromoted. Rounding down also promotes
    * neighbouring .bss in the same granule; callers can avoid that by aligning to
    * ORBIS_JIT_GRANULE, as flycast does. */
   base &= ~(uintptr_t)(ORBIS_JIT_GRANULE - 1);
   end   = (end + (ORBIS_JIT_GRANULE - 1)) & ~(uintptr_t)(ORBIS_JIT_GRANULE - 1);

   orbis_jit_lock();

   for (i = 0; i < s_promoted_n; i++)
      if (base >= s_promoted[i].base && end <= s_promoted[i].end)
      {
         ok = s_promoted[i].ok;
         orbis_jit_unlock();
         return ok;
      }

   for (i = 0; i < s_promoted_n; i++)
      if (base < s_promoted[i].end && end > s_promoted[i].base)
      {
         overlap = (int)i;
         break;
      }

   rc = sceKernelMprotect((void *)base, (size_t)(end - base), ORBIS_JIT_PROT_RWX);
   if (rc != 0)
   {
      orbis_report("jit", "%p (%lu KiB) would not take execute - sceKernelMprotect returned "
                          "0x%08x (%s). The recompiler has nowhere to write and must not be used.",
                   (void *)base, (unsigned long)((end - base) / 1024), (unsigned)rc,
                   orbis_jit_err(rc));
      ok = 0;
   }
   else if (overlap >= 0)
   {
      /* Overlaps an already verified range: reuse its result rather than writing a stub over
       * existing code, and widen the entry to the union. */
      ok = s_promoted[overlap].ok;
      if (base < s_promoted[overlap].base)
         s_promoted[overlap].base = base;
      if (end > s_promoted[overlap].end)
         s_promoted[overlap].end = end;
      orbis_jit_unlock();
      return ok;
   }
   else
   {
      const int say = s_first_stub || orbis_jit_should_log();
      ok = orbis_jit_verify(addr) ? 1 : 0;
      if (say || !ok)
         orbis_report("jit", "%lu KiB at %p is %s", (unsigned long)(len / 1024), addr,
                      ok ? "writable and executable" : "NOT executable");
   }

   orbis_jit_record(ok);

   if (s_promoted_n < ORBIS_JIT_MAX_PROMOTED)
   {
      s_promoted[s_promoted_n].base = base;
      s_promoted[s_promoted_n].end  = end;
      s_promoted[s_promoted_n].ok   = ok;
      s_promoted_n++;
   }
   else
      orbis_report("jit", "no slot to record %p - a repeat request for these pages will re-run the "
                          "stub check and overwrite generated code", (void *)base);

   orbis_jit_unlock();
   return ok;
}

/* The promotion result, for callers choosing a CPU mode. -1 until something has asked; treat it as
 * "unknown", not "no". */
int orbis_jit_state(void)
{
   return s_state;
}
