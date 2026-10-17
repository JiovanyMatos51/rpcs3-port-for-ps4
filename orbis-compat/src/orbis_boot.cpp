// Copyright © 2026 Mikołaj Mikołajczyk
// SPDX-License-Identifier: MIT
// See include/orbis_boot.h.
//
// Moved from OpenGothic ps4/og_ps4_boot.cpp on 2026-08-19, with ps4_log -> orbis_log,
// ps4_log_fatal -> orbis_log_fatal and ps4_idle_forever -> orbis_fatal_action.
#include <orbis_boot.h>

#include <execinfo.h>
#include <orbis_log.h>
#include <orbis_mmap.h>

#include <orbis/libkernel.h>

#include <clocale>
#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <stdexcept>
#include <system_error>

namespace {

[[noreturn]] static void ps4TerminateHandler() {
  char msg[512] = "std::terminate with no active exception";
  if(std::exception_ptr p = std::current_exception()) {
    try {
      std::rethrow_exception(p);
      }
    catch(const std::system_error& e) {
      std::snprintf(msg,sizeof(msg),"std::system_error(%s)",e.what());
      }
    catch(const std::runtime_error& e) {
      std::snprintf(msg,sizeof(msg),"std::runtime_error(%s)",e.what());
      }
    catch(const std::logic_error& e) {
      std::snprintf(msg,sizeof(msg),"std::logic_error(%s)",e.what());
      }
    catch(const std::bad_alloc& e) {
      std::snprintf(msg,sizeof(msg),"std::bad_alloc(%s)",e.what());
      }
    catch(const std::exception& e) {
      // GothicNotFoundException lands here: OpenGothic derives it from std::runtime_error
      // via Tempest, and CrashLog's own handler abort()s on it by name without printing.
      std::snprintf(msg,sizeof(msg),"%s","std::exception(");
      std::snprintf(msg+strlen(msg),sizeof(msg)-strlen(msg),"%s)",e.what());
      }
    catch(...) {
      std::snprintf(msg,sizeof(msg),"%s","a non-std exception");
      }
    }
  // The fatal channel: the process is about to die and a datagram might never be sent.
  orbis_log_fatal("fatal: unhandled exception - %s",msg);
  orbis_fatal_action("terminate");
  // Only reached when the host asked for autoexit; a console never gets here.
  _Exit(1);
  }

static const char* signalName(int sig) {
  switch(sig) {
    case SIGSEGV: return "SIGSEGV (bad address)";
    case SIGBUS:  return "SIGBUS (unaligned or unmapped)";
    case SIGFPE:  return "SIGFPE";
    case SIGILL:  return "SIGILL (illegal instruction)";
    case SIGABRT: return "SIGABRT";
    }
  return "?";
  }

// siginfo, not just the number. The faulting address distinguishes the likely causes:
//
//   si_addr just below a thread's stack        a stack overflow
//   si_addr at the instruction pointer         an illegal instruction (unsupported CPU feature)
//   si_addr anywhere else                      an ordinary bad pointer
//
// describeAddress() asks the kernel what an address belongs to (sceKernelVirtualQuery), at the
// moment of the fault. Once this handler catches a signal the kernel writes no crash dump, and
// musl's heap and GPU buffers are both direct memory in the same 0x2xxxxxxxx range, so the
// mapping is needed to tell them apart. prot tells whether rip's page is executable, i.e. "the CPU
// ran code here" vs "the fetch itself faulted".
//
// The SDK names the needed fields unk01, unk02 and unk04. A later revision of the same header
// names the identical struct
//
//     void* start_addr;  void* end_addr;  off_t offset;  int32_t prot;  int32_t mtype;
//
// with the same order, types and trailing bitfields, and `offset` in the same slot in both. The
// static_asserts turn a future reorder into a build failure instead of a wrongly labelled pointer.
static_assert(offsetof(OrbisKernelVirtualQueryInfo,offset)==2*sizeof(void*),
            "OrbisKernelVirtualQueryInfo no longer starts with two pointers - re-check which "
            "fields are start_addr/end_addr/prot before printing them under those names");
static_assert(sizeof(OrbisKernelVirtualQueryInfo)>=2*sizeof(void*)+sizeof(off_t)+2*sizeof(int32_t)+32,
            "OrbisKernelVirtualQueryInfo is smaller than the fields this file reads out of it");
static void describeAddress(const char* what, const void* p) {
  OrbisKernelVirtualQueryInfo vq;
  std::memset(&vq,0,sizeof(vq));
  if(sceKernelVirtualQuery(p,0,&vq,sizeof(vq))!=0) {
    // An address the kernel will not describe is not mapped; for rip that is the answer.
    orbis_log_fatal("fatal: %s %p is in NO mapping the kernel will describe - not mapped",what,p);
    return;
    }
  // CPU bits only; GPU_READ/GPU_WRITE in the same word are irrelevant to a CPU fault.
  const int32_t prot = vq.unk04;          // prot
  char rwx[4] = { char((prot&0x01)?'r':'-'), char((prot&0x02)?'w':'-'),
                  char((prot&0x04)?'x':'-'), '\0' };
  const char* pool = vq.isDirectMemory   ? "direct"
                   : vq.isFlexibleMemory ? "flexible"
                   : vq.isPooledMemory   ? "pooled"
                   : vq.isStack          ? "stack"
                   :                       "other";
  // name[] comes from the kernel and is not guaranteed terminated.
  char name[sizeof(vq.name)+1];
  std::memcpy(name,vq.name,sizeof(vq.name));
  name[sizeof(vq.name)] = '\0';
  orbis_log_fatal("fatal: %s %p is in %p-%p prot 0x%x (%s) %s%s%s%s",
                what,p,vq.unk01,vq.unk02,(unsigned)prot,rwx,pool,
                vq.isCommitted ? ", committed" : ", NOT committed",
                name[0] ? ", name " : "", name[0] ? name : "");

  // Which carve-out, if it is one of ours: the kernel only says "direct"; the mmap interposer's
  // table knows which are musl's heap and whether the block is still live.
  orbis::MmapDirectSpan span;
  if(orbis::mmapDirectDescribe(p,span)) {
    if(span.blockFound)
      orbis_log_fatal("fatal: %s is carve-out %u (base %p) +0x%llx, in a %s block of %llu KiB "
                    "at +0x%llx%s",
                    what,span.index,span.base,(unsigned long long)span.offset,
                    span.blockUsed ? "LIVE" : "FREED",
                    (unsigned long long)(span.blockSize/1024),
                    (unsigned long long)span.blockOff,
                    span.blockUsed ? "" : " - this is a USE AFTER FREE");
    else
      orbis_log_fatal("fatal: %s is carve-out %u (base %p) +0x%llx, in no block of it",
                    what,span.index,span.base,(unsigned long long)span.offset);
    }
  }

// The bytes around rip. An address in anonymous memory has no symbol, so the only way to tell
// generated code from a freed object's debris is to disassemble it on the build machine
// (llvm-mc --disassemble accepts the output as printed). 32 bytes.
//
// Read only where the kernel reports the mapping readable: a second fault in this handler is
// fatal and silent (`reentered` is set). The window is clamped to the mapping, so near its edges
// fewer bytes are printed.
static void dumpCodeAt(const void* p) {
  OrbisKernelVirtualQueryInfo vq;
  std::memset(&vq,0,sizeof(vq));
  if(sceKernelVirtualQuery(p,0,&vq,sizeof(vq))!=0)
    return;
  if(!vq.isCommitted || (vq.unk04&0x01)==0)   // prot
    return;                              // not readable: say nothing rather than fault

  const uint8_t* lo   = static_cast<const uint8_t*>(vq.unk01);   // start_addr
  const uint8_t* hi   = static_cast<const uint8_t*>(vq.unk02);   // end_addr
  const uint8_t* rip  = static_cast<const uint8_t*>(p);
  if(rip<lo || rip>=hi)
    return;
  const uint8_t* from = (rip-lo >= 16) ? rip-16 : lo;
  const uint8_t* to   = (hi-rip  > 16) ? rip+16 : hi;

  for(const uint8_t* row=from; row<to; row+=16) {
    char     line[3*16+1];
    unsigned k = 0;
    for(const uint8_t* b=row; b<to && b<row+16; ++b)
      k += unsigned(std::snprintf(line+k,sizeof(line)-k,"%02x ",unsigned(*b)));
    if(k>0)
      line[k-1] = '\0';
    orbis_log_fatal("fatal: code %p%s: %s",(const void*)row,
                  (rip>=row && rip<row+16) ? " (rip in this row)" : "",line);
    }
  }

static void ps4SignalAction(int sig, siginfo_t* info, void* uctx) {
  (void)uctx;
  // Intentionally not async-signal-safe: a silent crash gives no information, and
  // re-raising would only produce the CE-34878-0 dialog.
  static volatile int reentered = 0;
  if(reentered!=0)
    _Exit(2);
  reentered = 1;
  // orbis_log_fatal, not orbis_log: the ordinary channel may be UDP only, and a datagram from a
  // process about to be killed may never leave the machine.
  //
  // `info` is not trusted to be a pointer. With the SDK's Linux SA_SIGINFO (4, which this kernel
  // reads as SA_RESETHAND) the handler got FreeBSD's old (int sig, int code, struct sigcontext *)
  // signature with code == 2, and reading info->si_code faulted inside the handler.
  // include/signal.h now fixes the flags; the guard stays because anything below a page (16 KiB
  // here) cannot be a valid pointer.
  const bool info_is_pointer = (reinterpret_cast<uintptr_t>(info) >= 0x4000u);
  if(info_is_pointer)
    orbis_log_fatal("fatal: signal %d - %s, si_code %d, fault address %p",
                  sig,signalName(sig),info->si_code,info->si_addr);
  else
    orbis_log_fatal("fatal: signal %d - %s, and NO siginfo: the second argument was %p, which is "
                  "too small to be a pointer. The kernel called this handler with FreeBSD's "
                  "original (sig, code, scp) signature, so SA_SIGINFO did not reach it - check the "
                  "SA_* values in orbis-compat/include/signal.h against this SDK's bits/signal.h",
                  sig,signalName(sig),(void*)info);
  // Backtrace. Once this handler catches a signal the kernel writes no dump (no registers, rip
  // or module list), so the handler must report them itself. The return addresses are in the
  // module; subtract the module base to get function names. The base is not printed here:
  // libretro-common/dynamic/dylib.c logs each core's retro_run address at load for this; subtract
  // its address in the .elf.
  {
    void *frames[24];
    const int n = backtrace(frames, (int)(sizeof(frames) / sizeof(frames[0])));
    if(n > 0)
      for(int i = 0; i < n; i += 4)
      {
        const int r = (n - i) < 4 ? (n - i) : 4;
        orbis_log_fatal("fatal: frame[%02d..%02d] %016llx %016llx %016llx %016llx",
                      i, i + r - 1,
                      (unsigned long long)(r > 0 ? (uintptr_t)frames[i + 0] : 0),
                      (unsigned long long)(r > 1 ? (uintptr_t)frames[i + 1] : 0),
                      (unsigned long long)(r > 2 ? (uintptr_t)frames[i + 2] : 0),
                      (unsigned long long)(r > 3 ? (uintptr_t)frames[i + 3] : 0));
      }
    else
      orbis_log_fatal("fatal: backtrace returned %d frames", n);
  }

  // Register context. The SDK headers describe musl's mcontext while the kernel passes FreeBSD's
  // (`no member named 'mc_rip' in 'mcontext_t'`), so the offset is located at run time.
  //
  // On FreeBSD/amd64 a user thread's selectors are fixed: mc_cs is 0x43 and mc_ss is 0x3b, three
  // words apart, which fixes the base of every other field. Measured on hardware 2026-08-30:
  // mc_ss at ctx[32], so the mcontext starts at ctx[8] and mc_rip is ctx[28].
  //
  // As a second check, mc_addr must equal siginfo's si_addr before anything is printed under a
  // field name. If either check fails, the raw words are printed with the reason.
  if (uctx != nullptr)
  {
    // Field order of FreeBSD's amd64 mcontext_t, in 8-byte words from the start of the struct.
    enum : int {
      MC_TRAPNO = 16,   // uint32 trapno, then uint16 fs, gs - one word
      MC_ADDR   = 17,
      MC_FLAGS  = 18,   // uint32 flags, then uint16 es, ds
      MC_ERR    = 19,
      MC_RIP    = 20,
      MC_CS     = 21,
      MC_RFLAGS = 22,
      MC_RSP    = 23,
      MC_SS     = 24
    };
    const uint64_t *w = reinterpret_cast<const uint64_t*>(uctx);
    const int words   = 96;               // far past mc_ss for any plausible prefix
    int mc            = -1;

    for(int i = MC_CS; i + (MC_SS - MC_CS) < words; i++)
      if(w[i] == 0x43u && w[i + (MC_SS - MC_CS)] == 0x3bu) { mc = i - MC_CS; break; }

    const bool addr_agrees = (mc >= 0) && info_is_pointer
                          && (w[mc + MC_ADDR] == (uint64_t)(uintptr_t)info->si_addr);

    if(mc >= 0 && (addr_agrees || !info_is_pointer))
    {
      orbis_log_fatal("fatal: rip %016llx  rsp %016llx  addr %016llx",
                    (unsigned long long)w[mc + MC_RIP],
                    (unsigned long long)w[mc + MC_RSP],
                    (unsigned long long)w[mc + MC_ADDR]);
      orbis_log_fatal("fatal: trapno %u  err %llu  rflags %016llx  (mcontext at ctx[%d])",
                    (unsigned)(w[mc + MC_TRAPNO] & 0xffffffffu),
                    (unsigned long long)w[mc + MC_ERR],
                    (unsigned long long)w[mc + MC_RFLAGS], mc);
      // What the two addresses belong to.
      describeAddress("rip", (const void*)(uintptr_t)w[mc + MC_RIP]);
      if(w[mc + MC_ADDR] != 0)
        describeAddress("addr", (const void*)(uintptr_t)w[mc + MC_ADDR]);
      dumpCodeAt((const void*)(uintptr_t)w[mc + MC_RIP]);
      // Subtract the module base from rip to get a function (see the backtrace note above).
    }
    else
    {
      if(mc < 0)
        orbis_log_fatal("fatal: no mc_cs/mc_ss pair (0x43, 0x3b) in the first %d words of the "
                      "context - the layout is not FreeBSD's amd64 mcontext, raw words follow",
                      words);
      else
        orbis_log_fatal("fatal: found a context at ctx[%d] but mc_addr %016llx does not match "
                      "si_addr %p - not trusting the field names, raw words follow",
                      mc, (unsigned long long)w[mc + MC_ADDR], info_is_pointer ? info->si_addr : nullptr);

      for(int base = 0; base < 64; base += 4)
        orbis_log_fatal("fatal: ctx[%02d..%02d] %016llx %016llx %016llx %016llx",
                      base, base + 3,
                      (unsigned long long)w[base + 0], (unsigned long long)w[base + 1],
                      (unsigned long long)w[base + 2], (unsigned long long)w[base + 3]);
    }
  }

  orbis_fatal_action("signal");
  _Exit(2);
  }


}

// ------------------------------------------------------------------ ctype probe

// Does case folding work on this platform?
//
// Case-insensitive lookups depend on std::tolower: e.g. ZenKit's VFS orders entries with
// `tolower(a) < tolower(b)` and verifies hits with `tolower(a) == tolower(b)`
// (lib/ZenKit/src/Misc.cc:11-21), and archive entries are upper case while games ask in lower
// case. If std::tolower were the identity, every such lookup would miss.
//
// std::tolower uses the C locale's ctype tables and nothing calls setlocale(); glibc's defaults
// are populated, the SDK's musl is worth checking. Three calls at boot.
void orbis::probeCtype() {
  const int la = std::tolower('A'), lz = std::tolower('Z');
  const int ua = std::toupper('a'), uz = std::toupper('z');
  const char* loc = setlocale(LC_ALL,nullptr);
  orbis_log("boot: ctype probe: tolower('A')=%d('%c') tolower('Z')=%d('%c') "
          "toupper('a')=%d('%c') toupper('z')=%d('%c') isspace(' ')=%d locale=%s",
          la,(la>=32&&la<127)?char(la):'?', lz,(lz>=32&&lz<127)?char(lz):'?',
          ua,(ua>=32&&ua<127)?char(ua):'?', uz,(uz>=32&&uz<127)?char(uz):'?',
          std::isspace(' ') ? 1 : 0, loc!=nullptr ? loc : "(null)");

  const bool folds = (la=='a' && lz=='z' && ua=='A' && uz=='Z');
  if(folds) {
    orbis_log("boot: ctype probe - VERDICT: case folding works. ZenKit's case-insensitive "
            "VFS lookup is sound; a failed resource lookup is NOT a folding problem.");
    } else {
    orbis_log("boot: ctype probe - VERDICT: CASE FOLDING IS BROKEN. std::tolower is not "
            "folding ASCII, so ZenKit's icompare/iequals are case-SENSITIVE. Every "
            "lower-case lookup into an upper-case VDFS catalog - which is every font, "
            "every texture, every resource OpenGothic names in lower case - misses.");
    }
  }

// FreeBSD's errno numbers (FreeBSD 9 sys/sys/errno.h), not Linux's. The tables agree up to 34
// and diverge from 35 (EAGAIN on FreeBSD, EDEADLK on Linux). Only the values sigaltstack can
// return are named, plus ENOSYS ("not implemented" vs "refused").
static const char* bsdErrnoName(int e) {
  switch(e) {
    case 0:  return "no error";
    case 1:  return "EPERM - already running on the alternate stack";
    case 12: return "ENOMEM - ss_size below MINSIGSTKSZ";
    case 14: return "EFAULT - the kernel could not read the stack_t we passed";
    case 22: return "EINVAL - a bit set in ss_flags that is not SS_DISABLE";
    case 78: return "ENOSYS - the kernel does not implement this call at all";
    }
  return "not in this file's table - decode against FreeBSD's errno.h, NOT Linux's";
  }

void orbis::installCrashHandlers() {
  std::set_terminate(&ps4TerminateHandler);

  // An alternate stack: without it a stack overflow cannot be reported, since the handler needs
  // stack to run. 64 KiB, intentionally leaked so it outlives everything.
  //
  // sigaltstack used to return -1: besides SA_ONSTACK's Linux value, the SDK declares stack_t in
  // Linux's field order while the kernel reads FreeBSD's. include/signal.h fixes both; the code
  // below verifies it on the console.
  //
  // The buffer is on the heap, not a static array: large static arrays on this console can lack
  // write permission (see orbis_jit.h), which a crash path cannot afford. `alt` itself is 24 bytes
  // of .bss and is known to be writable.
  static stack_t alt = {};
  alt.ss_size  = 64*1024;
  alt.ss_sp    = new char[alt.ss_size];
  alt.ss_flags = 0;
  errno = 0;
  const int altRc  = sigaltstack(&alt,nullptr);
  const int altErr = errno;

  // Legacy-layout probe, only when the corrected call failed (so it cannot replace a working
  // stack): the same 64 KiB in the SDK's field order. If both fail with the same errno, the
  // kernel refuses the call regardless of layout.
  int legacyRc = 0, legacyErr = 0;
  if(altRc != 0) {
    static stack_t legacy = {};
    legacy.ss_size  = 64*1024;
    legacy.ss_sp    = alt.ss_sp;
    legacy.ss_flags = 0;
    errno = 0;
    legacyRc  = __orbis_sigaltstack_kernel(
                  reinterpret_cast<const struct __orbis_stack_bsd*>(&legacy),nullptr);
    legacyErr = errno;
    }

  // Readback: sigaltstack(NULL,&oss) makes the kernel write 24 bytes, printed raw next to their
  // FreeBSD interpretation. The layout is identifiable whether or not the install worked:
  //
  //   installed, FreeBSD order   w0 = the buffer   w1 = 0000000000010000   w2 = 0 or 1 (SS_ONSTACK)
  //   refused,   FreeBSD order   w0 = 0            w1 = 0                 w2 = 4 (SS_DISABLE)
  //   refused,   Linux order     w0 = 0            w1 = 2 (Linux's        w2 = 0
  //                                                     SS_DISABLE)
  //
  // Called through the raw symbol, not the shim, so the shim's assumption does not affect it.
  union { struct __orbis_stack_bsd s; unsigned long long w[3]; } back;
  back.w[0] = back.w[1] = back.w[2] = 0;
  errno = 0;
  const int backRc  = __orbis_sigaltstack_kernel(nullptr,&back.s);
  const int backErr = errno;

  const bool bsdOrder = (back.w[2] <= 7ull) && (back.w[1] == 0ull || back.w[1] == 64ull*1024ull);
  const bool linOrder = (back.w[1] <= 7ull) && (back.w[2] == 0ull || back.w[2] == 64ull*1024ull);
  const char* order   = (back.w[0]==0ull && back.w[1]==0ull && back.w[2]==0ull)
                          ? "all three words zero - inconclusive, and also not what FreeBSD does "
                            "for a thread with no alternate stack (it sets SS_DISABLE)"
                      : (bsdOrder && !linOrder) ? "FreeBSD field order confirmed (sp, size, flags)"
                      : (linOrder && !bsdOrder) ? "⚠ LINUX field order - the shim in "
                            "orbis-compat/include/signal.h is backwards and must be reverted"
                      : "neither order fits - read the raw words by hand";

  struct sigaction sa = {};
  // The SDK's sa_sigaction macro is broken (see include/signal.h, which fixes it).
  sa.sa_sigaction = &ps4SignalAction;
  sa.sa_flags     = SA_SIGINFO | SA_ONSTACK;
  sigemptyset(&sa.sa_mask);
  const int segvRc = sigaction(SIGSEGV,&sa,nullptr);
  sigaction(SIGBUS, &sa,nullptr);
  sigaction(SIGFPE, &sa,nullptr);
  sigaction(SIGILL, &sa,nullptr);
  sigaction(SIGABRT,&sa,nullptr);

  orbis_log("boot: crash handlers installed (terminate + SIGSEGV/SIGBUS/SIGFPE/SIGILL/SIGABRT, "
          "sigaction rc=%d, sigaltstack rc=%d errno=%d %s %s)",segvRc,altRc,altErr,
          bsdErrnoName(altErr),
          altRc==0 ? "- a stack overflow on THIS thread can now report itself"
                   : "- NO alt stack, so a stack overflow will still die silently");
  orbis_log("boot: sigaltstack readback rc=%d errno=%d - raw %016llx %016llx %016llx - "
          "as FreeBSD stack_t: ss_sp=%016llx ss_size=%llu ss_flags=%d - %s",
          backRc,backErr,back.w[0],back.w[1],back.w[2],
          (unsigned long long)(uintptr_t)back.s.ss_sp,
          (unsigned long long)back.s.ss_size,back.s.ss_flags,order);
  if(altRc != 0)
    orbis_log("boot: sigaltstack legacy-layout probe (the SDK's own field order, i.e. what this "
            "port sent before today) rc=%d errno=%d %s - if BOTH orders fail with the same errno "
            "the layout was never the problem and this kernel is refusing the call itself",
            legacyRc,legacyErr,bsdErrnoName(legacyErr));

  // The alternate stack is per thread (td_sigstk) while the disposition is per process, so this
  // only covers the calling thread. Other threads get one from the pthread interposer's
  // trampoline in src/orbis_thread.cpp (64 KiB from the thread's own stack; see orbis_thread.h).
  // The "thread alt stack:" boot line and the thread census report it.
  //
  // The main thread keeps the heap buffer: this function is not its outermost frame, so a local
  // array would be reclaimed while td_sigstk still pointed at it.
  }
