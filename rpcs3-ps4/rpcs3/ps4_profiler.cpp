#ifndef RPCS3_PS4
#error This translation unit is only for the PS4 target.
#endif

// 0.29 sampling profiler (see ps4_profiler.h), built on the live-thread registry of Utilities/Thread.h.
// Everything runs on the main thread
// (ps4_main.cpp main loop, ~1 kHz only while a census or burst is active) except prof_signal, which runs on the
// sampled thread itself. No thread, mutex, condvar or ScePthread attr is created, so the libkernel thread budget
// and flexible memory (Demon's Souls 0.27/0.28 scePthreadCreate 0x80020023) are not touched.
//
// Handler contract (async-signal-safe): reads RIP/RSP from the native ucontext, scans at most 64 stack words inside
// this thread's own stack (bound: the registry's stack_hi, copied into the request) for one return address into
// rpcs3_ps4.elf, reads the guest PC (cpu_thread TLS + plain loads), calls getrusage(RUSAGE_THREAD) (libkernel
// syscall stub) when asked and publishes with a seqlock into this thread's reply slot. No allocation, no lock, no
// stdio; it only writes the reply slot of its own registry slot (ps4_thread_registry::self(), static TLS).
//
// Thread-exit safety comes from the registry pin protocol: the main thread pins a slot (pin, read, generation
// check, pthread_kill, unpin) around every access. A leaving thread first makes self() return -1 (a late signal is
// inert), then waits for the pins to drop, so pthread_kill never targets a thread past pthread_exit and a late
// signal never writes a reused slot. The on_leave hook clears the slot's pending request.
//
// 0.33 signal scope: FreeBSD returns EINTR from poll/select/kevent/connect/nanosleep/sem_wait even with SA_RESTART,
// so signal 31 can fail a host wait that never fails on PC (0.32: 'Network P2P Thread' died on a sys_net EINTR 7 ms
// after census #1). The profiler is therefore off unless /data/rpcs3/profile.flag exists, and then signals only guest
// and compile threads; full=1 widens that to every thread except the service/I-O family, rsx=1 adds the RSX/Vulkan
// family. Live threads outside the scope are counted as 'not sampled' and never receive a signal.

#include "stdafx.h"
#include "ps4_profiler.h"
#include "ps4_fatal.h"
#include "Emu/CPU/CPUThread.h"
#include "Utilities/JIT.h"
#include "Utilities/Thread.h"
#include "util/ps4_build_version.hpp"
#include "util/ps4_signal_context.hpp"
#include "util/ps4_virtual_memory.hpp"
#include "util/sysinfo.hpp"

#include <orbis/libkernel.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>
#include <string_view>
#include <utility>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/resource.h>
#include <unistd.h>

extern char __text_start[];
extern char __eh_frame_start[];
extern char __eh_frame_hdr_start[];
extern char __eh_frame_hdr_end[];

// ps4_fatal.cpp: services a pending STALL host-stack request addressed to the current thread (async-signal-safe).
void ps4_stall_sample_signal(int sig, siginfo_t* info, void* context) noexcept;

namespace rsx
{
// RSXThread.cpp (0.26): pthread_t of the RSX thread, 0 before it starts.
u64 ps4_rsx_native_thread() noexcept;
}

// orbis-compat <signal.h> supplies the FreeBSD values the kernel parses (the SDK's are Linux's).
static_assert(SA_SIGINFO == 0x40 && SA_RESTART == 0x2 && SA_ONSTACK == 0x1, "FreeBSD SA_* values required");
static_assert(RUSAGE_THREAD == 1, "FreeBSD RUSAGE_THREAD");
static_assert(sizeof(OrbisKernelModuleInfo) == 0x160, "OpenOrbis OrbisKernelModuleInfo layout");

namespace
{
using ps4_fatal_detail::fixed_text;
namespace reg = ps4_thread_registry;

constexpr int prof_signal_number = 31;           // native FreeBSD SIGUSR2 (the SDK SIGUSR2 macro is Linux's 12)
constexpr unsigned prof_slots = reg::slot_count; // one reply slot per registry slot
constexpr unsigned prof_name = reg::name_size;
constexpr unsigned prof_targets = 16;            // threads sampled per burst
constexpr unsigned prof_hist = 512;              // buckets per sampled thread (power of two)
constexpr unsigned prof_modules = 96;
constexpr unsigned prof_cpu_rows = 1024;

constexpr std::uint32_t want_rip = 1, want_cpu = 2, reply_rusage_failed = 0x100;

// Request/reply of one registry slot. want: main -> handler; the rest: handler -> main (seqlock seq, odd while
// the handler writes). stack_hi is written by main before want (scan bound of this request).
struct alignas(64) prof_reply
{
    std::atomic<std::uint32_t> want{0};
    std::atomic<std::uint32_t> seq{0};
    std::atomic<std::uint64_t> stack_hi{0};
    std::atomic<std::uint64_t> rip{0}, caller{0}, cpu_ns{0}, vcsw{0}, ivcsw{0}, tsc{0};
    std::atomic<std::uint32_t> gpc{0}, cls{0}, flags{0};
};

constinit prof_reply g_reply[prof_slots]{}; // constant-initialized: valid before main and in any handler
constinit std::atomic<std::uint32_t> g_foreign{0};

// Immutable once ps4_profiler_init has run (read by the handler; zero = feature off).
std::uintptr_t g_text_lo = 0, g_text_hi = 0, g_code_hi = 0, g_jitrt_lo = 0, g_jitrt_hi = 0;
constexpr std::uintptr_t g_mcjit_lo = utils::ps4::memory::jit_virtual_begin; // PPU LLVM (MemoryManager1)
constexpr std::uintptr_t g_mcjit_hi = utils::ps4::memory::jit_virtual_end;

std::uint64_t rdtsc() noexcept { return __builtin_ia32_rdtsc(); }

// ---------------------------------------------------------------- signal side (async-signal-safe)

bool looks_like_return(std::uintptr_t v) noexcept
{
    // A return address follows a call: e8 rel32, or ff /2 (call r/m) of 2, 3, 6 or 7 bytes.
    const auto* c = reinterpret_cast<const unsigned char*>(v);
    if (c[-5] == 0xe8) return true;
    if (c[-2] == 0xff && (c[-1] & 0x38) == 0x10) return true;
    if (c[-3] == 0xff && (c[-2] & 0x38) == 0x10) return true;
    if (c[-6] == 0xff && (c[-5] & 0x38) == 0x10) return true;
    if (c[-7] == 0xff && (c[-6] & 0x38) == 0x10) return true;
    return false;
}

std::uint64_t scan_caller(std::uint64_t rsp, std::uint64_t hi) noexcept
{
    // Only inside this thread's own stack ([rsp, stack_hi) is mapped when rsp < stack_hi), at most 64 words.
    if (!g_code_hi || !rsp || (rsp & 7) || rsp >= hi || hi - rsp > (16u << 20)) return 0;
    const auto* p = reinterpret_cast<const std::uint64_t*>(rsp);
    const std::uint64_t n = std::min<std::uint64_t>((hi - rsp) / 8, 64);
    for (std::uint64_t i = 0; i < n; ++i)
    {
        const std::uint64_t v = p[i];
        if (v >= g_text_lo + 16 && v < g_code_hi && looks_like_return(v)) return v;
    }
    return 0;
}

std::uint64_t rusage_ns(const struct ::rusage& ru) noexcept
{
    return (static_cast<std::uint64_t>(ru.ru_utime.tv_sec) + static_cast<std::uint64_t>(ru.ru_stime.tv_sec)) * 1'000'000'000ull +
        (static_cast<std::uint64_t>(ru.ru_utime.tv_usec) + static_cast<std::uint64_t>(ru.ru_stime.tv_usec)) * 1'000ull;
}

void prof_signal(int sig, siginfo_t* info, void* context) noexcept
{
    const int saved_errno = errno;
    ps4_stall_sample_signal(sig, info, context); // no-op unless a STALL request names this thread
    const int index = reg::self();
    if (index < 0 || static_cast<unsigned>(index) >= prof_slots || !context)
    {
        g_foreign.fetch_add(1, std::memory_order_relaxed);
        errno = saved_errno;
        return;
    }
    prof_reply& s = g_reply[index];
    const std::uint32_t w = s.want.exchange(0, std::memory_order_acq_rel);
    if (w)
    {
        const auto& m = static_cast<const utils::ps4::signal_context*>(context)->machine;
        std::uint32_t flags = w, gpc = 0, cls = 0;
        std::uint64_t caller = 0, cpu_ns = 0, vcsw = 0, ivcsw = 0;
        if ((w & want_rip) && !(m.rip >= g_text_lo && m.rip < g_text_hi)) caller = scan_caller(m.rsp, s.stack_hi.load(std::memory_order_relaxed));
        if (const auto cpu = cpu_thread::get_current())
        {
            cls = static_cast<std::uint32_t>(cpu->get_class());
            if (w & want_rip) gpc = cpu->get_pc();
        }
        if (w & want_cpu)
        {
            struct ::rusage ru{};
            if (::getrusage(RUSAGE_THREAD, &ru) == 0)
            {
                cpu_ns = rusage_ns(ru);
                vcsw = static_cast<std::uint64_t>(ru.ru_nvcsw);
                ivcsw = static_cast<std::uint64_t>(ru.ru_nivcsw);
            }
            else
            {
                flags |= reply_rusage_failed;
            }
        }
        const std::uint32_t q = s.seq.load(std::memory_order_relaxed);
        s.seq.store(q + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        s.rip.store(m.rip, std::memory_order_relaxed);
        s.caller.store(caller, std::memory_order_relaxed);
        s.cpu_ns.store(cpu_ns, std::memory_order_relaxed);
        s.vcsw.store(vcsw, std::memory_order_relaxed);
        s.ivcsw.store(ivcsw, std::memory_order_relaxed);
        s.tsc.store(rdtsc(), std::memory_order_relaxed);
        s.gpc.store(gpc, std::memory_order_relaxed);
        s.cls.store(cls, std::memory_order_relaxed);
        s.flags.store(flags, std::memory_order_relaxed);
        s.seq.store(q + 2, std::memory_order_release);
    }
    errno = saved_errno;
}

// Registry hook, on the leaving thread after the pins dropped: no request of this generation can be pending
// any more (pin() fails from now on), so a later owner of the slot starts clean.
void prof_on_leave(u32 slot) noexcept
{
    if (slot < prof_slots) g_reply[slot].want.store(0, std::memory_order_relaxed);
}

// ---------------------------------------------------------------- reply helpers (main thread)

void copy_name(char* dst, const char* src) noexcept
{
    std::size_t i = 0;
    for (; src && src[i] && i + 1 < prof_name; ++i) dst[i] = src[i];
    dst[i] = 0;
}

struct prof_sample
{
    std::uint64_t rip = 0, caller = 0, cpu_ns = 0, vcsw = 0, ivcsw = 0, tsc = 0;
    std::uint32_t gpc = 0, cls = 0, flags = 0, seq = 0;
};

bool read_reply(const prof_reply& s, std::uint32_t seen, prof_sample& r) noexcept
{
    const std::uint32_t q = s.seq.load(std::memory_order_acquire);
    if (q == seen || (q & 1)) return false;
    r.rip = s.rip.load(std::memory_order_relaxed);
    r.caller = s.caller.load(std::memory_order_relaxed);
    r.cpu_ns = s.cpu_ns.load(std::memory_order_relaxed);
    r.vcsw = s.vcsw.load(std::memory_order_relaxed);
    r.ivcsw = s.ivcsw.load(std::memory_order_relaxed);
    r.tsc = s.tsc.load(std::memory_order_relaxed);
    r.gpc = s.gpc.load(std::memory_order_relaxed);
    r.cls = s.cls.load(std::memory_order_relaxed);
    r.flags = s.flags.load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    if (s.seq.load(std::memory_order_relaxed) != q) return false;
    r.seq = q;
    return true;
}

// ---------------------------------------------------------------- code map (main thread)

struct prof_fde_index
{
    const std::int32_t* table = nullptr; // {initial_location, fde} pairs, datarel sdata4
    std::uint32_t count = 0;
    std::uintptr_t hdr = 0;
};
prof_fde_index g_fde;

bool fde_entry(std::uint32_t i, std::uintptr_t& start, std::uintptr_t& range) noexcept
{
    start = g_fde.hdr + static_cast<std::intptr_t>(g_fde.table[2 * i]);
    const std::uintptr_t fde = g_fde.hdr + static_cast<std::intptr_t>(g_fde.table[2 * i + 1]);
    if (fde < reinterpret_cast<std::uintptr_t>(__eh_frame_start) || fde + 16 > reinterpret_cast<std::uintptr_t>(__eh_frame_hdr_start)) return false;
    std::int32_t rel = 0;
    std::uint32_t len = 0;
    std::memcpy(&rel, reinterpret_cast<const void*>(fde + 8), 4);  // pc_begin, pcrel|sdata4
    std::memcpy(&len, reinterpret_cast<const void*>(fde + 12), 4); // pc_range
    if (fde + 8 + static_cast<std::intptr_t>(rel) != start) return false;
    range = len;
    return true;
}

std::uintptr_t fde_function(std::uintptr_t ip) noexcept
{
    if (!g_fde.count || ip < g_text_lo || ip >= g_text_hi) return 0;
    std::uint32_t lo = 0, hi = g_fde.count;
    while (hi - lo > 1)
    {
        const std::uint32_t mid = lo + (hi - lo) / 2;
        if (g_fde.hdr + static_cast<std::intptr_t>(g_fde.table[2 * mid]) <= ip) lo = mid;
        else hi = mid;
    }
    std::uintptr_t start = 0, range = 0;
    if (!fde_entry(lo, start, range) || ip < start || ip - start >= range) return 0;
    return start;
}

bool fde_init() noexcept
{
    const auto* h = reinterpret_cast<const unsigned char*>(__eh_frame_hdr_start);
    const std::size_t size = reinterpret_cast<std::uintptr_t>(__eh_frame_hdr_end) - reinterpret_cast<std::uintptr_t>(__eh_frame_hdr_start);
    // version 1; eh_frame_ptr 4-byte; fde_count udata4 (0x03); table datarel|sdata4 (0x3b): the 0.28 ELF has 01 1b 03 3b.
    if (size < 20 || h[0] != 1 || (h[1] & 0x0f) != 0x0b || h[2] != 0x03 || h[3] != 0x3b) return false;
    std::uint32_t count = 0;
    std::memcpy(&count, h + 8, 4);
    if (!count || count > (size - 12) / 8) return false;
    g_fde.table = reinterpret_cast<const std::int32_t*>(h + 12);
    g_fde.hdr = reinterpret_cast<std::uintptr_t>(h);
    g_fde.count = count;
    std::uintptr_t start = 0, range = 0;
    if (!fde_entry(count - 1, start, range) || start + range > g_text_hi)
    {
        g_fde.count = 0;
        return false;
    }
    g_code_hi = start + range;
    return true;
}

struct prof_module
{
    std::uintptr_t lo = 0, hi = 0;
    bool kernel = false;
    char name[40]{};
};
prof_module g_modules[prof_modules];
unsigned g_module_count = 0;

using module_info_fn = int (*)(std::uint32_t, OrbisKernelModuleInfo*);
module_info_fn g_module_info = nullptr;

void* dlsym_any(const char* symbol) noexcept
{
    // Same idiom as ps4_shared_memory.cpp enable_aliasing (works on the console): no new static import.
    std::uint32_t handles[256]{};
    std::size_t count = 0;
    if (sceKernelGetModuleList(handles, 256, &count) != 0) return nullptr;
    for (std::size_t i = 0; i < std::min<std::size_t>(count, 256); ++i)
    {
        void* address = nullptr;
        if (sceKernelDlsym(static_cast<std::int32_t>(handles[i]), symbol, &address) == 0 && address) return address;
    }
    return nullptr;
}

void refresh_modules() noexcept
{
    if (!g_module_info) return;
    std::uint32_t handles[256]{};
    std::size_t count = 0;
    if (sceKernelGetModuleList(handles, 256, &count) != 0) return;
    unsigned n = 0;
    for (std::size_t i = 0; i < std::min<std::size_t>(count, 256) && n < prof_modules; ++i)
    {
        OrbisKernelModuleInfo info{};
        info.size = sizeof(info);
        if (g_module_info(handles[i], &info) != 0) continue;
        const unsigned segments = std::min<unsigned>(info.segmentCount, 4);
        int pick = -1;
        for (unsigned k = 0; k < segments && pick < 0; ++k)
            if (info.segmentInfo[k].prot & 4) pick = static_cast<int>(k); // CPU_EXEC (assumed Sony value)
        if (pick < 0 && segments) pick = 0;
        if (pick < 0) continue;
        const auto lo = reinterpret_cast<std::uintptr_t>(info.segmentInfo[pick].address);
        const std::uintptr_t hi = lo + info.segmentInfo[pick].size;
        if (!lo || hi <= lo || (g_text_lo >= lo && g_text_lo < hi)) continue; // the eboot uses its ELF range
        prof_module& m = g_modules[n++];
        m.lo = lo;
        m.hi = hi;
        std::size_t k = 0;
        for (; k < sizeof(info.name) && info.name[k] && k + 1 < sizeof(m.name); ++k) m.name[k] = info.name[k];
        m.name[k] = 0;
        if (char* dot = std::strstr(m.name, ".sprx")) *dot = 0;
        else if (char* dot2 = std::strstr(m.name, ".prx")) *dot2 = 0;
        m.kernel = std::strncmp(m.name, "libkernel", 9) == 0;
    }
    g_module_count = n;
}

int find_module(std::uintptr_t ip) noexcept
{
    for (unsigned i = 0; i < g_module_count; ++i)
        if (ip >= g_modules[i].lo && ip < g_modules[i].hi) return static_cast<int>(i);
    return -1;
}

enum : std::uint8_t { kind_elf = 1, kind_elf_raw, kind_spu_jit, kind_ppu_jit, kind_jitrt, kind_mcjit, kind_module, kind_anon };
constexpr std::uint64_t payload_mask = (1ull << 56) - 1;

std::uint64_t make_key(std::uint8_t kind, std::uint64_t payload) noexcept { return (std::uint64_t{kind} << 56) | (payload & payload_mask); }

std::uint64_t classify(const prof_sample& r) noexcept
{
    const std::uintptr_t ip = r.rip;
    if (ip >= g_text_lo && ip < g_text_hi)
    {
        const std::uintptr_t f = fde_function(ip);
        return f ? make_key(kind_elf, f - g_text_lo) : make_key(kind_elf_raw, (ip - g_text_lo) & ~std::uint64_t{0xf});
    }
    // SPU LLVM code lives in jit_runtime (MemoryManager2), PPU LLVM in reserve_jit arenas (MemoryManager1).
    if (ip >= g_jitrt_lo && ip < g_jitrt_hi)
        return r.cls == 2 ? make_key(kind_spu_jit, r.gpc) : make_key(kind_jitrt, (ip - g_jitrt_lo) >> 4);
    if (ip >= g_mcjit_lo && ip < g_mcjit_hi)
        return r.cls == 1 ? make_key(kind_ppu_jit, r.gpc) : make_key(kind_mcjit, (ip - g_mcjit_lo) >> 4);
    if (const int mod = find_module(ip); mod >= 0)
    {
        std::uint64_t caller = 0;
        if (r.caller > g_text_lo && r.caller < g_text_hi)
        {
            const std::uintptr_t f = fde_function(r.caller - 1);
            caller = (f ? f : r.caller) - g_text_lo;
        }
        return make_key(kind_module, (std::uint64_t(mod) << 49) | ((((ip - g_modules[mod].lo) >> 4) & 0x1fffff) << 28) | (caller & 0xfffffff));
    }
    return make_key(kind_anon, ip >> 4);
}

unsigned key_group(std::uint64_t key) noexcept
{
    switch (static_cast<std::uint8_t>(key >> 56))
    {
    case kind_elf:
    case kind_elf_raw: return 0;
    case kind_spu_jit:
    case kind_ppu_jit:
    case kind_jitrt:
    case kind_mcjit: return 1;
    case kind_module:
    {
        const unsigned mod = static_cast<unsigned>((key & payload_mask) >> 49);
        return mod < g_module_count && g_modules[mod].kernel ? 2 : 3;
    }
    default: return 4;
    }
}

template <std::size_t N>
void describe(fixed_text<N>& t, std::uint64_t key) noexcept
{
    const std::uint64_t p = key & payload_mask;
    switch (static_cast<std::uint8_t>(key >> 56))
    {
    case kind_elf: t.s("elf+").x(p); break;
    case kind_elf_raw: t.s("elf~").x(p).s(" (no FDE)"); break;
    case kind_spu_jit: t.s("spu-jit pc=").x(p); break;
    case kind_ppu_jit: t.s("ppu-jit cia=").x(p); break;
    case kind_jitrt: t.s("jitrt+").x(p << 4); break;
    case kind_mcjit: t.s("mcjit+").x(p << 4); break;
    case kind_module:
    {
        const unsigned mod = static_cast<unsigned>(p >> 49);
        t.s(mod < g_module_count ? g_modules[mod].name : "module?").s("+").x(((p >> 28) & 0x1fffff) << 4);
        if (const std::uint64_t c = p & 0xfffffff) t.s(" <- elf+").x(c);
        break;
    }
    case kind_anon: t.s("anon ").x(p << 4); break;
    default: t.s("?"); break;
    }
}

const char* class_name(std::uint32_t cls) noexcept
{
    switch (cls) { case 1: return "ppu"; case 2: return "spu"; case 0x55: return "rsx"; default: return "-"; }
}

std::uint64_t permille(std::uint64_t part, std::uint64_t whole) noexcept { return whole ? (part * 1000 + whole / 2) / whole : 0; }

template <std::size_t N>
void put_permille(fixed_text<N>& t, std::uint64_t value) noexcept { t.u(value / 10).s(".").u(value % 10).s("%"); }

// One open/write/close per report, no fsync. Whole lines only: ps4_startup_checkpoint (another O_APPEND writer)
// may append between two flushes, never inside a line.
class prof_log
{
public:
    prof_log() noexcept : m_fd(::open("/data/rpcs3/startup.log", O_WRONLY | O_CREAT | O_APPEND, 0666)) {}
    prof_log(const prof_log&) = delete;
    prof_log& operator=(const prof_log&) = delete;
    ~prof_log()
    {
        flush();
        if (m_fd >= 0) ::close(m_fd);
    }

    template <std::size_t N>
    void line(const fixed_text<N>& t) noexcept
    {
        static_assert(N + 32 <= sizeof(m_buf), "a PROF line must fit the buffer");
        fixed_text<24> stamp;
        stamp.u(sceKernelGetProcessTime()).s(" ");
        if (m_n + stamp.n + t.n + 1 > sizeof(m_buf)) flush();
        std::memcpy(m_buf + m_n, stamp.b, stamp.n);
        m_n += stamp.n;
        std::memcpy(m_buf + m_n, t.b, t.n);
        m_n += t.n;
        m_buf[m_n++] = '\n';
    }

private:
    void flush() noexcept
    {
        const char* p = m_buf;
        std::size_t n = m_n;
        while (m_fd >= 0 && n)
        {
            const ssize_t w = ::write(m_fd, p, n);
            if (w <= 0) break;
            p += w;
            n -= static_cast<std::size_t>(w);
        }
        m_n = 0;
    }

    int m_fd;
    std::size_t m_n = 0;
    char m_buf[8192];
};

// Defaults are the light profile; full=1 in profile.flag switches to the full defaults (see load_config).
struct prof_config
{
    bool full = false, rsx = false, cpuusage = false;
    std::uint32_t first_s = 45, period_s = 60, burst_ms = 1000, threads = 4, top = 12, min_pct = 5, lines = 16;
};
prof_config s_conf;

std::uint32_t clamp_u32(std::uint64_t v, std::uint32_t lo, std::uint32_t hi) noexcept { return static_cast<std::uint32_t>(std::clamp<std::uint64_t>(v, lo, hi)); }

int g_main_slot = -1; // registry slot of the main thread (the sampler), -1 = not registered

// Guest threads (PPU, SPU, raw and isolated SPU) and compile threads (PPU/SPU LLVM workers, SPRX precompile, the
// LLVM JIT helper): the threads a CPU profile is about. Their blocking waits are atomic waits that recheck their
// condition, and their socket calls go through sys_net, which retries EINTR on this port.
constexpr std::string_view g_guest_or_compile[] = {"PPU[", "SPU[", "RawSPU[", "Iso[", "PPUW.", "SPUW.", "SPU Worker", "SPU LLVM", "SPRX Worker", "LLVM JIT"};

// Service threads parked in poll/select/connect/nanosleep/sem_wait or in device/library waits whose EINTR handling
// is not verified (network, RPCN/NP, USB, audio, microphone, camera, pads, patch download). Never signalled, not even
// with full=1.
constexpr std::string_view g_service_io[] = {"Network", "RPCN", "NP ", "Signaling", "UPnP", "UPNP", "Tcp Over Udp", "Usb", "cellAudio",
    "RsxAudio", "Microphone", "Camera", "Pad", "PS4 Patch Download"};

template <std::size_t N>
bool has_prefix(const char* name, const std::string_view (&prefixes)[N]) noexcept
{
    std::size_t len = 0;
    while (len < prof_name && name[len]) ++len;
    const std::string_view n{name, len};
    for (const std::string_view p : prefixes)
        if (n.substr(0, p.size()) == p) return true;
    return false;
}

bool rsx_family(std::uint64_t native, const char* name) noexcept
{
    // The RSX thread blocks inside the Mesa/orbis-drm winsys, whose EINTR handling is not verified; 'RSX Worker #',
    // 'RSX.W#' (VK pipeline compilers), 'Vulkan Driver Manager' (VKCommandStream.h: eid_completed frees VK objects,
    // i.e. RADV bo/winsys calls) and 'Vulkan Device Enumeration Thread' can be inside RADV/winsys calls too.
    const std::uint64_t rsx_thread = rsx::ps4_rsx_native_thread();
    if (rsx_thread && native == rsx_thread) return true;
    return std::strncmp(name, "RSX", 3) == 0 || std::strncmp(name, "rsx", 3) == 0 || std::strncmp(name, "Vulkan", 6) == 0;
}

// The only gate in front of every pthread_kill to another thread (census and burst).
bool signal_ok(std::uint64_t native, const char* name) noexcept
{
    if (!native || !name) return false;
    if (rsx_family(native, name)) return s_conf.rsx;
    return s_conf.full ? !has_prefix(name, g_service_io) : has_prefix(name, g_guest_or_compile);
}

const char* scope_text() noexcept
{
    if (s_conf.full) return s_conf.rsx ? "all except service/I-O threads, RSX family included" : "all except service/I-O and RSX family";
    return s_conf.rsx ? "guest+compile threads and RSX family" : "guest+compile threads";
}

// Calls f(key, value) for every 'key=value' line (value: up to 10 leading decimal digits, 0 otherwise).
template <typename F>
void for_each_setting(std::string_view text, F&& f) noexcept
{
    const auto blank = [](char c) { return c == ' ' || c == '\t' || c == '\r'; };
    while (!text.empty())
    {
        const std::size_t end = text.find('\n');
        const std::string_view line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos || line[0] == '#') continue;
        std::string_view key = line.substr(0, eq);
        while (!key.empty() && blank(key.back())) key.remove_suffix(1);
        while (!key.empty() && blank(key.front())) key.remove_prefix(1);
        std::size_t p = eq + 1;
        while (p < line.size() && (line[p] == ' ' || line[p] == '\t')) ++p;
        std::uint64_t v = 0;
        for (unsigned digits = 0; p < line.size() && line[p] >= '0' && line[p] <= '9' && digits < 10; ++p, ++digits)
            v = v * 10 + static_cast<unsigned>(line[p] - '0');
        f(key, v);
    }
}

// false: /data/rpcs3/profile.flag is absent and the profiler stays off (no signal is ever sent).
// Present: light defaults; full=1 selects the full defaults; the other keys override either set.
bool load_config() noexcept
{
    const int fd = ::open("/data/rpcs3/profile.flag", O_RDONLY);
    if (fd < 0) return false;
    char text[1024];
    const ssize_t got = ::read(fd, text, sizeof(text) - 1);
    ::close(fd);
    const std::string_view settings{text, got > 0 ? static_cast<std::size_t>(got) : 0};
    for_each_setting(settings, [](std::string_view key, std::uint64_t v)
    {
        if (key == "full") s_conf.full = v != 0;
    });
    if (s_conf.full)
    {
        s_conf.first_s = 20;
        s_conf.period_s = 15;
        s_conf.burst_ms = 2000;
        s_conf.threads = 12;
        s_conf.top = 20;
        s_conf.min_pct = 1;
        s_conf.lines = 48;
    }
    for_each_setting(settings, [](std::string_view key, std::uint64_t v)
    {
        if (key == "period") s_conf.period_s = clamp_u32(v, 5, 3600);
        else if (key == "first") s_conf.first_s = clamp_u32(v, 5, 3600);
        else if (key == "burst_ms") s_conf.burst_ms = clamp_u32(v, 0, 3'600'000);
        else if (key == "threads") s_conf.threads = clamp_u32(v, 1, prof_targets);
        else if (key == "top") s_conf.top = clamp_u32(v, 1, 64);
        else if (key == "min") s_conf.min_pct = clamp_u32(v, 0, 100);
        else if (key == "lines") s_conf.lines = clamp_u32(v, 4, 64);
        else if (key == "rsx") s_conf.rsx = v != 0;
        else if (key == "cpuusage") s_conf.cpuusage = v != 0;
    });
    s_conf.burst_ms = std::min<std::uint32_t>(s_conf.burst_ms, s_conf.period_s * 1000);
    return true;
}

// ---------------------------------------------------------------- census + burst (main thread)

// Per registry slot, valid for one generation of the slot (a reused slot starts a new book).
struct prof_book
{
    std::uint32_t gen = 0, seen = 0, cls = 0;
    std::int32_t tid = 0;
    bool known = false, asked = false, answered = false;
    std::uint64_t native = 0, stack_hi = 0;
    std::uint64_t base_cpu = 0, base_vcsw = 0, base_ivcsw = 0, base_us = 0;
    std::uint64_t win_cpu = 0, win_vcsw = 0, win_ivcsw = 0, win_us = 0;
    char name[prof_name]{};
};
prof_book g_book[prof_slots];

std::uint64_t book_permille(const prof_book& b) noexcept { return b.win_cpu / std::max<std::uint64_t>(b.win_us, 1); } // ns/us = per-mille

struct prof_bucket
{
    std::uint64_t key;
    std::uint32_t count;
};

struct prof_target
{
    int slot = -1;
    std::uint32_t gen = 0, seen = 0, cls = 0;
    std::uint64_t cpu_permille = 0, native = 0, stack_hi = 0;
    std::int32_t tid = 0;
    char name[prof_name]{};
    bool dead = false, out_of_scope = false;
    std::uint64_t send_tsc = 0, lat_max_us = 0;
    std::uint32_t asked = 0, got = 0, busy = 0, kill_fail = 0, used = 0, overflow = 0;
    std::uint32_t lat[5]{};    // signal-to-handler latency: <20us <100us <1ms <10ms >=10ms
    std::uint32_t groups[5]{}; // elf, jit, libkernel, other system modules, anonymous
};
prof_target g_targets[prof_targets];
prof_bucket g_hist[prof_targets][prof_hist];
unsigned g_target_count = 0;

enum class prof_phase { off, idle, census, burst, drain };
prof_phase g_phase = prof_phase::off;
std::uint64_t g_next_us = 0, g_phase_t0 = 0, g_phase_end = 0, g_census_t0 = 0, g_prev_census_us = 0;
std::uint32_t g_census_id = 0, g_burst_id = 0, g_asked = 0, g_got = 0, g_kill_fail = 0, g_ticks = 0;
std::uint32_t g_unsampled = 0;      // live threads outside signal_ok in the current census
fixed_text<1536> g_unsampled_names; // their names, collected once (census #1)
bool g_was_running = false;
std::uint64_t g_tsc0 = 0, g_us0 = 0, g_tsc_per_us = 1600;
bool g_tsc_from_kernel = false;

// Opt-in (profile.flag cpuusage=1): libkernel's per-thread CPU table, layout from the OpenOrbis header.
struct prof_cpu_row
{
    std::int32_t lo_data;
    std::uint32_t tid;
    std::int64_t user_sec, user_nsec, sys_sec, sys_nsec;
};
static_assert(sizeof(prof_cpu_row) == 0x28, "Proc_Stats layout (OpenOrbis orbis/libkernel.h)");
using cpu_usage_fn = int (*)(prof_cpu_row*, std::int32_t*);
using thread_name_fn = int (*)(std::uint32_t, char*);
cpu_usage_fn g_cpu_usage = nullptr;
thread_name_fn g_thread_name = nullptr;
prof_cpu_row* g_rows = nullptr; // prof_cpu_rows entries, half offered to the kernel (headroom for a byte count)
int g_rows_mode = 0;            // 0 untested, 1 *size = rows, 2 *size = bytes, -1 rejected
struct prof_tid
{
    std::uint32_t tid = 0;
    std::uint64_t ns = 0, us = 0;
};
prof_tid g_tids[512];

std::uint64_t row_ns(const prof_cpu_row& r) noexcept
{
    const auto part = [](std::int64_t sec, std::int64_t nsec) -> std::uint64_t
    {
        return sec < 0 || nsec < 0 || nsec >= 1'000'000'000 ? 0 : static_cast<std::uint64_t>(sec) * 1'000'000'000ull + static_cast<std::uint64_t>(nsec);
    };
    return part(r.user_sec, r.user_nsec) + part(r.sys_sec, r.sys_nsec);
}

// The slot's thread is pinned by the caller (or is the caller): info is a consistent copy.
void sync_book(unsigned i, const reg::slot_info& info) noexcept
{
    prof_book& b = g_book[i];
    if (!b.known || b.gen != info.gen)
    {
        b = prof_book{};
        b.known = true;
        b.gen = info.gen;
        b.seen = g_reply[i].seq.load(std::memory_order_acquire);
        b.base_us = info.enter_us; // first window: CPU since the thread entered the registry
    }
    b.tid = info.tid;
    b.native = info.native;
    b.stack_hi = info.stack_hi;
    copy_name(b.name, info.name); // follows set_name renames
}

void account(prof_book& b, std::uint64_t cpu_ns, std::uint64_t vcsw, std::uint64_t ivcsw, std::uint32_t cls, std::uint64_t now) noexcept
{
    b.cls = cls;
    b.win_cpu = cpu_ns >= b.base_cpu ? cpu_ns - b.base_cpu : 0;
    b.win_vcsw = vcsw >= b.base_vcsw ? vcsw - b.base_vcsw : 0;
    b.win_ivcsw = ivcsw >= b.base_ivcsw ? ivcsw - b.base_ivcsw : 0;
    b.win_us = now > b.base_us ? now - b.base_us : 1;
    b.base_cpu = cpu_ns;
    b.base_vcsw = vcsw;
    b.base_ivcsw = ivcsw;
    b.base_us = now;
    b.answered = true;
}

void census_start(std::uint64_t now) noexcept
{
    refresh_modules();
    ++g_census_id;
    g_phase = prof_phase::census;
    g_census_t0 = g_phase_t0 = now;
    g_asked = g_got = g_kill_fail = 0;
    for (prof_book& b : g_book) b.asked = b.answered = false;
    if (g_main_slot >= 0)
    {
        // The main thread (the sampler) measures itself; reading its own slot needs no pin.
        reg::slot_info info{};
        if (reg::read(static_cast<u32>(g_main_slot), info))
        {
            sync_book(static_cast<unsigned>(g_main_slot), info);
            struct ::rusage ru{};
            if (::getrusage(RUSAGE_THREAD, &ru) == 0)
                account(g_book[g_main_slot], rusage_ns(ru), static_cast<std::uint64_t>(ru.ru_nvcsw), static_cast<std::uint64_t>(ru.ru_nivcsw), 0, now);
        }
    }
    g_unsampled = 0;
    for (unsigned i = 0; i < prof_slots; ++i)
    {
        if (static_cast<int>(i) == g_main_slot || reg::state(i) != reg::slot_live || !reg::pin(i)) continue;
        reg::slot_info info{};
        if (!reg::read(i, info) || !info.native)
        {
            reg::unpin(i);
            continue;
        }
        if (!signal_ok(info.native, info.name))
        {
            // Booked (tid and name for the cpuusage table) but never signalled.
            sync_book(i, info);
            reg::unpin(i);
            if (g_census_id == 1 && !g_unsampled_names.full()) g_unsampled_names.s(g_unsampled ? ", '" : "'").s(g_book[i].name).s("'");
            g_unsampled++;
            continue;
        }
        sync_book(i, info);
        prof_book& b = g_book[i];
        prof_reply& r = g_reply[i];
        const std::uint32_t q = r.seq.load(std::memory_order_acquire);
        if (!(q & 1)) b.seen = q; // drop a late burst reply
        r.stack_hi.store(info.stack_hi, std::memory_order_relaxed);
        // A request still pending (signal not yet delivered) gets the CPU bit added, without a second signal.
        const std::uint32_t prev = r.want.fetch_or(want_cpu, std::memory_order_acq_rel);
        const int error = prev ? 0 : pthread_kill((pthread_t)info.native, prof_signal_number);
        if (error) r.want.fetch_and(~want_cpu, std::memory_order_relaxed);
        reg::unpin(i);
        if (error)
        {
            g_kill_fail++;
            continue;
        }
        b.asked = true;
        g_asked++;
    }
}

void cpu_usage_report(prof_log& log, std::uint64_t now) noexcept
{
    if (!s_conf.cpuusage || g_rows_mode < 0 || !g_cpu_usage || !g_rows || g_main_slot < 0) return;
    std::memset(static_cast<void*>(g_rows), 0, sizeof(prof_cpu_row) * prof_cpu_rows);
    struct ::rusage ru{};
    const std::uint64_t main_ns = ::getrusage(RUSAGE_THREAD, &ru) == 0 ? rusage_ns(ru) : 0;
    const std::uint32_t main_tid = static_cast<std::uint32_t>(g_book[g_main_slot].tid);
    std::int32_t size = prof_cpu_rows / 2;
    const int rc = g_cpu_usage(g_rows, &size);
    // Accept the table only if it contains the main thread with the CPU time getrusage reports for it.
    const auto main_matches = [&](std::uint64_t count) -> bool
    {
        for (std::uint64_t i = 0; i < count; ++i)
        {
            if (g_rows[i].tid != main_tid) continue;
            const std::uint64_t ns = row_ns(g_rows[i]);
            const std::uint64_t d = ns > main_ns ? ns - main_ns : main_ns - ns;
            return main_ns && d <= std::max<std::uint64_t>(50'000'000, main_ns / 4);
        }
        return false;
    };
    std::uint64_t rows = 0;
    if (rc == 0 && size > 0)
    {
        const auto n = static_cast<std::uint64_t>(size);
        if (g_rows_mode != 2 && n <= prof_cpu_rows && main_matches(n))
        {
            g_rows_mode = 1;
            rows = n;
        }
        else if (g_rows_mode != 1 && n % sizeof(prof_cpu_row) == 0 && n / sizeof(prof_cpu_row) <= prof_cpu_rows && main_matches(n / sizeof(prof_cpu_row)))
        {
            g_rows_mode = 2;
            rows = n / sizeof(prof_cpu_row);
        }
    }
    if (!rows)
    {
        if (g_rows_mode == 0)
        {
            g_rows_mode = -1;
            fixed_text<256> t;
            t.s("PROF cpuusage: sceKernelGetCpuUsage rejected (rc=").x(static_cast<std::uint32_t>(rc)).s(" size=").u(static_cast<std::uint32_t>(size))
             .s(" main getrusage=").u(main_ns / 1'000'000).s("ms) first words:");
            const auto* words = reinterpret_cast<const std::uint32_t*>(g_rows);
            for (unsigned i = 0; i < 10; ++i) t.s(" ").x(words[i]);
            log.line(t);
        }
        return;
    }
    struct entry
    {
        std::uint32_t tid;
        std::uint64_t permille;
        bool registered; // in the registry but without a census row (outside the signal scope, or silent)
    };
    entry top[8]{};
    unsigned count = 0;
    for (std::uint64_t i = 0; i < rows; ++i)
    {
        const std::uint32_t tid = g_rows[i].tid;
        if (!tid) continue;
        const std::uint64_t ns = row_ns(g_rows[i]);
        prof_tid* slot = nullptr;
        for (unsigned probe = 0, h = (tid * 2654435761u) & 511; probe < 512; ++probe, h = (h + 1) & 511)
        {
            if (g_tids[h].tid == tid || g_tids[h].tid == 0)
            {
                slot = &g_tids[h];
                break;
            }
        }
        if (!slot) continue;
        const bool fresh = slot->tid == 0;
        const std::uint64_t d_ns = !fresh && ns >= slot->ns ? ns - slot->ns : 0;
        const std::uint64_t d_us = !fresh && now > slot->us ? now - slot->us : 0;
        slot->tid = tid;
        slot->ns = ns;
        slot->us = now;
        int book = -1;
        for (unsigned k = 0; k < prof_slots && book < 0; ++k)
            if (g_book[k].known && reg::state(k) == reg::slot_live && static_cast<std::uint32_t>(g_book[k].tid) == tid) book = static_cast<int>(k);
        if ((book >= 0 && g_book[book].answered) || !d_us) continue; // already a 'PROF cpu' row
        const std::uint64_t pm = d_ns / d_us;
        if (pm < 5) continue;
        if (count < 8) top[count++] = {tid, pm, book >= 0};
        else
        {
            unsigned lowest = 0;
            for (unsigned k = 1; k < 8; ++k)
                if (top[k].permille < top[lowest].permille) lowest = k;
            if (pm > top[lowest].permille) top[lowest] = {tid, pm, book >= 0};
        }
    }
    std::sort(top, top + count, [](const entry& a, const entry& b) { return a.permille > b.permille; });
    {
        fixed_text<192> t;
        t.s("PROF cpuusage: ").u(rows).s(" kernel rows (mode ").u(static_cast<unsigned>(g_rows_mode)).s("); busiest threads without a census row (outside the RPCS3 registry, or not sampled):");
        log.line(t);
    }
    for (unsigned k = 0; k < count; ++k)
    {
        char name[512]{}; // generous: the real buffer size of sceKernelGetThreadName is not documented
        if (!g_thread_name || g_thread_name(top[k].tid, name) != 0) std::strcpy(name, "?");
        name[63] = 0;
        fixed_text<192> t;
        t.s("PROF cpu* ");
        put_permille(t, top[k].permille);
        t.s(" tid=").x(top[k].tid).s(" '").s(name).s("'").s(top[k].registered ? " (registry, not sampled)" : "");
        log.line(t);
    }
}

void census_finish(std::uint64_t now) noexcept
{
    std::uint16_t order[prof_slots];
    unsigned n = 0;
    std::uint64_t sum = 0;
    for (unsigned i = 0; i < prof_slots; ++i)
    {
        if (!g_book[i].answered) continue;
        order[n++] = static_cast<std::uint16_t>(i);
        sum += book_permille(g_book[i]);
    }
    std::sort(order, order + n, [](std::uint16_t a, std::uint16_t b) { return book_permille(g_book[a]) > book_permille(g_book[b]); });

    const reg::counters live = reg::get_counters();
    prof_log log;
    {
        fixed_text<512> t;
        t.s("PROF census #").u(g_census_id).s(" t=").u(now / 1'000'000).s("s window=")
         .u(g_prev_census_us ? (g_census_t0 - g_prev_census_us) / 1'000'000 : 0).s("s threads=").u(live.live)
         .s(" asked=").u(g_asked).s(" replied=").u(g_got).s(" silent=").u(g_asked - g_got).s(" killfail=").u(g_kill_fail)
         .s(" regfull=").u(live.full).s(" leaked=").u(live.leaked).s(" foreign=").u(g_foreign.load(std::memory_order_relaxed))
         .s(" not-sampled=").u(g_unsampled).s(s_conf.rsx ? "" : " rsx-family=not-signalled").s(" | cpu sum=");
        put_permille(t, sum);
        t.s(" of ").u(utils::get_thread_count() * 100ull).s("% (sampled threads only)");
        log.line(t);
    }
    if (g_census_id == 1 && g_unsampled)
    {
        fixed_text<1664> t;
        t.s("PROF not sampled (").u(g_unsampled).s(", scope ").s(scope_text()).s("): ").s(g_unsampled_names.b).s(g_unsampled_names.full() ? " ..." : "");
        log.line(t);
    }
    std::uint64_t rest_sum = 0;
    unsigned rest = 0;
    for (unsigned k = 0; k < n; ++k)
    {
        const unsigned i = order[k];
        const prof_book& b = g_book[i];
        const std::uint64_t pm = book_permille(b);
        if (k >= s_conf.lines || pm < 5)
        {
            rest++;
            rest_sum += pm;
            continue;
        }
        fixed_text<256> t;
        t.s("PROF cpu ").u(k + 1).s(" ");
        put_permille(t, pm);
        t.s(" ics/s=").u(b.win_ivcsw * 1'000'000 / std::max<std::uint64_t>(b.win_us, 1))
         .s(" vcs/s=").u(b.win_vcsw * 1'000'000 / std::max<std::uint64_t>(b.win_us, 1))
         .s(" ").s(static_cast<int>(i) == g_main_slot ? "main" : class_name(b.cls)).s(" tid=").x(static_cast<std::uint32_t>(b.tid)).s(" '").s(b.name).s("'");
        log.line(t);
    }
    if (rest)
    {
        fixed_text<96> t;
        t.s("PROF cpu rest: ").u(rest).s(" thread(s) ");
        put_permille(t, rest_sum);
        log.line(t);
    }
    cpu_usage_report(log, now);
    g_prev_census_us = g_census_t0;
    g_next_us = g_census_t0 + std::uint64_t{s_conf.period_s} * 1'000'000;

    // Burst targets: the busiest threads of this window (never the main thread, which is the sampler).
    g_target_count = 0;
    for (unsigned k = 0; k < n && g_target_count < s_conf.threads && s_conf.burst_ms; ++k)
    {
        const unsigned i = order[k];
        const prof_book& b = g_book[i];
        if (static_cast<int>(i) == g_main_slot || book_permille(b) < s_conf.min_pct * 10ull) continue;
        if (!reg::pin(i)) continue;
        if (reg::generation(i) == b.gen && signal_ok(b.native, b.name))
        {
            const unsigned slot_index = g_target_count++;
            prof_target& t = g_targets[slot_index];
            t = prof_target{};
            std::memset(static_cast<void*>(g_hist[slot_index]), 0, sizeof(g_hist[slot_index]));
            const std::uint32_t q = g_reply[i].seq.load(std::memory_order_acquire);
            t.slot = static_cast<int>(i);
            t.gen = b.gen;
            t.seen = (q & 1) ? b.seen : q;
            t.cls = b.cls;
            t.cpu_permille = book_permille(b);
            t.native = b.native;
            t.stack_hi = b.stack_hi;
            t.tid = b.tid;
            copy_name(t.name, b.name);
        }
        reg::unpin(i);
    }
    if (!g_target_count)
    {
        g_phase = prof_phase::idle;
        return;
    }
    if (!g_tsc_from_kernel)
    {
        if (const std::uint64_t us = now - g_us0; us > 1'000'000)
            g_tsc_per_us = std::max<std::uint64_t>((rdtsc() - g_tsc0) / us, 1);
    }
    ++g_burst_id;
    g_ticks = 0;
    g_phase = prof_phase::burst;
    g_phase_t0 = now;
    g_phase_end = now + std::uint64_t{s_conf.burst_ms} * 1000;
}

void census_poll(std::uint64_t now) noexcept
{
    for (unsigned i = 0; i < prof_slots; ++i)
    {
        prof_book& b = g_book[i];
        if (!b.asked || b.answered) continue;
        if (!reg::pin(i))
        {
            b.asked = false; // exited meanwhile
            g_asked--;
            continue;
        }
        if (reg::generation(i) != b.gen)
        {
            reg::unpin(i);
            b.asked = false;
            g_asked--;
            continue;
        }
        prof_sample r;
        if (read_reply(g_reply[i], b.seen, r))
        {
            b.seen = r.seq;
            if (r.flags & want_cpu)
            {
                if (r.flags & reply_rusage_failed)
                {
                    b.asked = false;
                    g_asked--;
                }
                else
                {
                    account(b, r.cpu_ns, r.vcsw, r.ivcsw, r.cls, now);
                    g_got++;
                }
            }
        }
        reg::unpin(i);
    }
    // Threads that are runnable but not scheduled answer late; 500 ms bounds the census.
    if (g_got >= g_asked || now - g_phase_t0 >= 500'000) census_finish(now);
}

void add_sample(unsigned k, std::uint64_t key) noexcept
{
    prof_target& t = g_targets[k];
    std::uint32_t h = static_cast<std::uint32_t>(((key ^ (key >> 29) ^ (key >> 47)) * 0x9e3779b97f4a7c15ull) >> 40) & (prof_hist - 1);
    for (unsigned probe = 0; probe < 32; ++probe, h = (h + 1) & (prof_hist - 1))
    {
        prof_bucket& b = g_hist[k][h];
        if (b.count == 0)
        {
            b.key = key;
            b.count = 1;
            t.used++;
            return;
        }
        if (b.key == key)
        {
            b.count++;
            return;
        }
    }
    t.overflow++;
}

void collect(unsigned k) noexcept
{
    prof_target& t = g_targets[k];
    prof_sample r;
    if (!read_reply(g_reply[t.slot], t.seen, r)) return;
    t.seen = r.seq;
    if (!(r.flags & want_rip)) return;
    t.got++;
    const std::uint64_t us = (r.tsc > t.send_tsc ? r.tsc - t.send_tsc : 0) / g_tsc_per_us;
    t.lat[us < 20 ? 0 : us < 100 ? 1 : us < 1000 ? 2 : us < 10000 ? 3 : 4]++;
    t.lat_max_us = std::max(t.lat_max_us, us);
    if (r.cls) t.cls = r.cls;
    const std::uint64_t key = classify(r);
    t.groups[key_group(key)]++;
    add_sample(k, key);
}

void burst_report(std::uint64_t now, bool aborted) noexcept
{
    prof_log log;
    const std::uint64_t duration = now > g_phase_t0 ? now - g_phase_t0 : 1;
    {
        fixed_text<256> t;
        t.s("PROF burst #").u(g_burst_id).s(" after census #").u(g_census_id).s(aborted ? " ABORTED (guest paused)" : "")
         .s(" duration=").u(duration / 1000).s("ms ticks=").u(g_ticks).s(" (~").u(g_ticks * 1'000'000ull / duration)
         .s(" Hz) threads=").u(g_target_count).s(" tsc/us=").u(g_tsc_per_us).s(" build ").s(ps4_build_version);
        log.line(t);
    }
    for (unsigned k = 0; k < g_target_count; ++k)
    {
        prof_target& t = g_targets[k];
        std::uint64_t total = 0;
        for (const std::uint32_t g : t.groups) total += g;
        {
            fixed_text<512> h;
            h.s("PROF thread '").s(t.name).s("' tid=").x(static_cast<std::uint32_t>(t.tid)).s(" ").s(class_name(t.cls)).s(" cpu=");
            put_permille(h, t.cpu_permille);
            h.s(" samples=").u(t.got).s("/").u(t.asked).s(" busy=").u(t.busy).s(" killfail=").u(t.kill_fail)
             .s(" lat<20us=").u(t.lat[0]).s(" <100us=").u(t.lat[1]).s(" <1ms=").u(t.lat[2]).s(" <10ms=").u(t.lat[3]).s(" >=10ms=").u(t.lat[4])
             .s(" max=").u(t.lat_max_us).s("us | elf ");
            put_permille(h, permille(t.groups[0], total));
            h.s(" jit ");
            put_permille(h, permille(t.groups[1], total));
            h.s(" kernel ");
            put_permille(h, permille(t.groups[2], total));
            h.s(" sys ");
            put_permille(h, permille(t.groups[3], total));
            h.s(" anon ");
            put_permille(h, permille(t.groups[4], total));
            h.s(" | buckets=").u(t.used).s(" overflow=").u(t.overflow).s(t.out_of_scope ? " (renamed out of the signal scope)" : t.dead ? " (thread exited)" : "");
            log.line(h);
        }
        std::uint16_t index[prof_hist];
        unsigned used = 0;
        for (unsigned b = 0; b < prof_hist; ++b)
            if (g_hist[k][b].count) index[used++] = static_cast<std::uint16_t>(b);
        const unsigned top = std::min<unsigned>(used, s_conf.top);
        std::partial_sort(index, index + top, index + used, [k](std::uint16_t a, std::uint16_t b) { return g_hist[k][a].count > g_hist[k][b].count; });
        for (unsigned r = 0; r < top; ++r)
        {
            const prof_bucket& b = g_hist[k][index[r]];
            fixed_text<192> line;
            line.s("PROF   ").u(r + 1).s(" ").u(b.count).s(" ");
            put_permille(line, permille(b.count, total));
            line.s(" ");
            describe(line, b.key);
            log.line(line);
        }
    }
}

void burst_tick(std::uint64_t now) noexcept
{
    const bool sending = g_phase == prof_phase::burst;
    if (sending) ++g_ticks;
    for (unsigned k = 0; k < g_target_count; ++k)
    {
        prof_target& t = g_targets[k];
        if (t.dead) continue;
        const auto slot = static_cast<u32>(t.slot);
        if (!reg::pin(slot))
        {
            t.dead = true;
            continue;
        }
        if (reg::generation(slot) != t.gen)
        {
            reg::unpin(slot);
            t.dead = true;
            continue;
        }
        collect(k);
        if (sending)
        {
            // Checked again per signal: set_name may have moved the thread out of the scope since the census.
            if (reg::slot_info info{}; !reg::read(slot, info) || !signal_ok(info.native, info.name))
            {
                reg::unpin(slot);
                t.dead = t.out_of_scope = true;
                continue;
            }
            prof_reply& r = g_reply[slot];
            // At most one request in flight per thread, and never one that would overwrite an unread reply.
            if (r.want.load(std::memory_order_acquire) == 0 && r.seq.load(std::memory_order_acquire) == t.seen)
            {
                t.send_tsc = rdtsc();
                r.stack_hi.store(t.stack_hi, std::memory_order_relaxed);
                r.want.store(want_rip, std::memory_order_release);
                if (const int error = pthread_kill((pthread_t)t.native, prof_signal_number))
                {
                    r.want.store(0, std::memory_order_relaxed);
                    t.kill_fail++;
                    if (error == ESRCH) t.dead = true;
                }
                else
                {
                    t.asked++;
                }
            }
            else
            {
                t.busy++;
            }
        }
        reg::unpin(slot);
    }
    if (sending && now >= g_phase_end)
    {
        g_phase = prof_phase::drain;
        g_phase_end = now + 20'000;
    }
    else if (!sending && now >= g_phase_end)
    {
        burst_report(now, false);
        g_phase = prof_phase::idle;
        if (std::uint64_t{s_conf.burst_ms} >= std::uint64_t{s_conf.period_s} * 1000) g_next_us = now; // continuous
    }
}

bool self_test(fixed_text<256>& out) noexcept
{
    // Signal ourselves once: proves sigaction flags, delivery, the ucontext layout and getrusage in the handler
    // before any emulator thread is ever signalled.
    if (g_main_slot < 0)
    {
        out.s("main thread not in the registry (full)");
        return false;
    }
    prof_reply& s = g_reply[g_main_slot];
    reg::slot_info info{};
    reg::read(static_cast<u32>(g_main_slot), info);
    s.stack_hi.store(info.stack_hi, std::memory_order_relaxed);
    const std::uint32_t before = s.seq.load(std::memory_order_acquire);
    s.want.store(want_rip | want_cpu, std::memory_order_release);
    if (const int error = pthread_kill(::pthread_self(), prof_signal_number))
    {
        s.want.store(0, std::memory_order_relaxed);
        out.s("pthread_kill(self) error ").u(static_cast<unsigned>(error));
        return false;
    }
    prof_sample r;
    bool delivered = false;
    for (int i = 0; i < 50 && !delivered; ++i)
    {
        delivered = read_reply(s, before, r);
        if (!delivered) sceKernelUsleep(1000);
    }
    if (!delivered)
    {
        s.want.store(0, std::memory_order_relaxed);
        out.s("handler did not run within 50 ms");
        return false;
    }
    out.s("delivered on main, rip=");
    describe(out, classify(r));
    out.s(" main cpu=").u(r.cpu_ns / 1'000'000).s("ms vcs=").u(r.vcsw).s(" ics=").u(r.ivcsw);
    if (r.flags & reply_rusage_failed) out.s(" (getrusage FAILED)");
    return r.rip != 0 && !(r.flags & reply_rusage_failed);
}
} // namespace

bool ps4_profiler_install_signal() noexcept
{
    static const bool s_installed = []() noexcept
    {
        struct sigaction action{};
        sigemptyset(&action.sa_mask);
        action.sa_sigaction = prof_signal;
        // FreeBSD values (orbis-compat <signal.h>): SA_ONSTACK runs the handler on orbis-compat's per-thread 64 KiB
        // alternate stack when the thread has one; SA_RESTART restarts an interrupted restartable syscall.
        action.sa_flags = SA_SIGINFO | SA_RESTART | SA_ONSTACK;
        if (::sigaction(prof_signal_number, &action, nullptr) != 0) return false;
        // Read back: only a handler that is really ours makes sending signal 31 safe.
        struct sigaction check{};
        if (::sigaction(prof_signal_number, nullptr, &check) != 0) return false;
        return check.sa_sigaction == prof_signal && (check.sa_flags & SA_SIGINFO) != 0;
    }();
    return s_installed;
}

void ps4_profiler_init() noexcept
{
    static bool s_done = false;
    if (std::exchange(s_done, true)) return;
    prof_log log;
    if (::access("/data/rpcs3/no-profile.flag", F_OK) == 0)
    {
        fixed_text<64> t;
        t.s("PROF: disabled by /data/rpcs3/no-profile.flag");
        log.line(t);
        return;
    }
    // 0.33: opt-in. Without the flag nothing is registered, installed or signalled (same path as no-profile.flag).
    if (!load_config())
    {
        fixed_text<128> t;
        t.s("PROF: off (opt-in: /data/rpcs3/profile.flag enables it; no thread is signalled)");
        log.line(t);
        return;
    }
    g_text_lo = reinterpret_cast<std::uintptr_t>(__text_start);
    g_text_hi = reinterpret_cast<std::uintptr_t>(__eh_frame_start);
    const bool fde_ok = fde_init();
    if (!fde_ok) g_code_hi = g_text_hi;
    const auto self_fn = reinterpret_cast<std::uintptr_t>(&ps4_profiler_tick);
    const bool fde_self = fde_ok && fde_function(self_fn + 1) == self_fn;
    g_jitrt_lo = reinterpret_cast<std::uintptr_t>(jit_runtime::alloc(0, 0, true)); // code subrange base, no allocation
    g_jitrt_hi = g_jitrt_lo ? g_jitrt_lo + 0x4000'0000 : 0;
    g_tsc0 = rdtsc();
    g_us0 = sceKernelGetProcessTime();
    if (const std::uint64_t hz = sceKernelGetTscFrequency(); hz >= 100'000'000 && hz <= 10'000'000'000ull)
    {
        g_tsc_per_us = hz / 1'000'000;
        g_tsc_from_kernel = true;
    }
    // The main thread is not a thread_base: register it so the self-test signal finds a reply slot.
    g_main_slot = reg::enter("main (ps4_main loop)");
    reg::set_hooks(nullptr, prof_on_leave);
    g_module_info = reinterpret_cast<module_info_fn>(dlsym_any("sceKernelGetModuleInfo"));
    refresh_modules();
    if (s_conf.cpuusage)
    {
        g_cpu_usage = reinterpret_cast<cpu_usage_fn>(dlsym_any("sceKernelGetCpuUsage"));
        g_thread_name = reinterpret_cast<thread_name_fn>(dlsym_any("sceKernelGetThreadName"));
        if (g_cpu_usage) g_rows = new (std::nothrow) prof_cpu_row[prof_cpu_rows];
    }
    const bool handler = ps4_profiler_install_signal();
    fixed_text<256> self;
    const bool self_ok = handler && self_test(self);
    {
        fixed_text<512> t;
        t.s("PROF init: ").s(s_conf.full ? "FULL (profile.flag full=1)" : "light (profile.flag)").s(" scope=").s(scope_text())
         .s(" period=").u(s_conf.period_s).s("s first=").u(s_conf.first_s)
         .s("s burst=").u(s_conf.burst_ms).s("ms threads=").u(s_conf.threads).s(" top=").u(s_conf.top).s(" min=").u(s_conf.min_pct)
         .s("% rsx=").u(s_conf.rsx).s(" cpuusage=").u(s_conf.cpuusage ? (g_cpu_usage ? 1u : 2u) : 0u)
         .s(" | text=").x(g_text_lo).s("..").x(g_code_hi).s(" fde=").u(g_fde.count).s(fde_self ? " (self-check ok)" : " (self-check FAILED: raw buckets)")
         .s(" jitrt=").x(g_jitrt_lo).s(" mcjit=").x(g_mcjit_lo).s("..").x(g_mcjit_hi).s(" modules=").u(g_module_count)
         .s(g_module_info ? "" : " (sceKernelGetModuleInfo unresolved)").s(" tsc/us=").u(g_tsc_per_us).s(g_tsc_from_kernel ? " (kernel)" : " (calibrated later)")
         .s(" main slot=");
        if (g_main_slot >= 0) t.u(static_cast<unsigned>(g_main_slot));
        else t.s("none");
        log.line(t);
    }
    for (unsigned i = 0; i < g_module_count; ++i)
    {
        fixed_text<160> t;
        t.s("PROF module ").s(g_modules[i].name).s(" text=").x(g_modules[i].lo).s(" size=").x(g_modules[i].hi - g_modules[i].lo);
        log.line(t);
    }
    {
        fixed_text<384> t;
        t.s("PROF signal 31: handler ").s(handler ? "installed" : "NOT installed").s("; self-test ").s(self_ok ? "ok: " : "FAILED: ").s(self.b)
         .s(self_ok ? "" : " - profiler disabled");
        log.line(t);
    }
    if (!fde_self) g_fde.count = 0;
    g_phase = handler && self_ok ? prof_phase::idle : prof_phase::off;
}

bool ps4_profiler_tick(std::uint64_t now, bool running) noexcept
{
    if (g_phase == prof_phase::off) return false;
    if (!running)
    {
        if (g_was_running)
        {
            g_was_running = false;
            if (g_phase == prof_phase::burst || g_phase == prof_phase::drain) burst_report(now, true);
            g_phase = prof_phase::idle; // late replies are harmless: the next census drops them
        }
        return false;
    }
    if (!g_was_running)
    {
        g_was_running = true;
        g_next_us = now + std::uint64_t{s_conf.first_s} * 1'000'000;
    }
    switch (g_phase)
    {
    case prof_phase::idle:
        if (now >= g_next_us) census_start(now);
        break;
    case prof_phase::census:
        census_poll(now);
        break;
    case prof_phase::burst:
    case prof_phase::drain:
        burst_tick(now);
        break;
    default:
        break;
    }
    return g_phase == prof_phase::census || g_phase == prof_phase::burst || g_phase == prof_phase::drain;
}
