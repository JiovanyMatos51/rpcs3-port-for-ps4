#ifndef RPCS3_PS4
#error This translation unit is only for the PS4 target.
#endif

// PS4 fatal-error and exit policy (0.25). See ps4_fatal.h.
// Compiled with -fexceptions so the terminate handler can inspect the active exception.

#include "stdafx.h"
#include "ps4_fatal.h"
#include "ps4_launcher.h"
#include "ps4_pads.h"
#include "util/ps4_adapt.hpp"
#include "util/ps4_build_version.hpp"
#include "util/ps4_i18n.hpp" // tr(): constant tables, async-signal-safe
#include "util/logs.hpp"
#include "util/ps4_signal_context.hpp"
#include "util/tsc.hpp"
#include "Utilities/Thread.h" // 0.29: ps4_thread_registry counters (relaxed loads, fatal-path safe)

#include <orbis/libkernel.h>
#include <orbis/Sysmodule.h>
#include <execinfo.h>   // orbis-compat: backtrace() over _Unwind_Backtrace
#include <orbis_log.h>  // orbis-compat diagnostics channel (OOM, mmap, paths)
#include <orbis_mmap.h> // 0.31: orbis-compat heap counters (relaxed loads, fatal-path safe)
#include <cxxabi.h>

#include <atomic>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <cstring>
#include <exception>
#include <new>
#include <string_view>
#include <system_error>
#include <typeinfo>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

LOG_CHANNEL(ps4_fatal_log, "PS4");

// Do not include <orbis/SystemService.h>: it declares GetStatus/ReceiveEvent as void(void).
// libSceSystemService.so exports all three (llvm-nm -D). Layouts below follow the shadPS4
// headers (unverified); the buffers are deliberately larger than the known structures.
extern "C" int32_t sceSystemServiceGetStatus(void* status);
extern "C" int32_t sceSystemServiceReceiveEvent(void* event);
extern "C" int32_t sceSystemServiceLoadExec(const char* path, const char* argv[]);
extern char __text_start[];
void ps4_request_shutdown();

// 0.28: libkernel's internal memory pool backs every ScePthread object (threads, mutexes, condvars,
// attrs; Sony technote 235). Demon's Souls 0.27 died when it ran out (scePthreadCreate 0x80020023).
// Exported by libkernel but not declared by the SDK; weak and probed the same way orbis-compat does
// (orbis_mem.cpp internalFree): a zeroed scratch satisfies both plausible Sony signatures.
extern "C" unsigned long long sceKernelInternalMemoryGetAvailableSize(void*, unsigned long long, unsigned long long)
    __attribute__((weak));

unsigned long long ps4_internal_memory_free() noexcept
{
    if (&sceKernelInternalMemoryGetAvailableSize == nullptr) return 0;
    unsigned long long scratch[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    const unsigned long long ret = sceKernelInternalMemoryGetAvailableSize(scratch, sizeof(scratch), 0);
    if (scratch[0] != 0) return scratch[0];
    // An `int f(size_t*)` shape that failed returns an SCE error (0x8xxxxxxx), not a size.
    const unsigned long long high = ret >> 32;
    if ((high == 0 || high == 0xffffffffull) && (ret & 0xf0000000ull) == 0x80000000ull) return 0;
    return ret;
}

// 0.28: thread stacks (2 MiB each through the orbis-compat floor) are mapped from flexible memory, which
// is a separate budget from both the internal pool and the direct-memory heap. Bound by asm label like
// orbis-compat (orbis_mem.cpp): the installed SDK header declares this query with a by-value argument.
extern "C" int32_t ps4_flexible_available(size_t* out) __asm__("sceKernelAvailableFlexibleMemorySize");

unsigned long long ps4_flexible_memory_free() noexcept
{
    size_t value = 0;
    return ps4_flexible_available(&value) == 0 ? value : 0;
}

namespace orbis
{
void threadCounts(unsigned long* created, unsigned long* raised);
unsigned long threadsFinished();

// 0.29 (orbis-compat include/orbis_thread.h): redeclared, not included; the layout is fixed on both sides
// (the library static_asserts 72 bytes). threadCensus only does relaxed atomic loads, so the fatal path may call it.
struct ThreadCensus
{
    unsigned long created_ok, failed, attr_failed, finished, exited, joined, live_estimate;
    unsigned long long stack_low, stack_high;
};
void threadCensus(ThreadCensus* out) noexcept;
}

static_assert(sizeof(orbis::ThreadCensus) == 72, "orbis::ThreadCensus layout must match orbis-compat");

void ps4_thread_census(unsigned long* created, unsigned long* finished) noexcept
{
    unsigned long raised = 0;
    orbis::threadCounts(created, &raised);
    if (finished) *finished = orbis::threadsFinished();
}

namespace
{
using ps4_fatal_detail::fixed_text;

// 0.29: flexible memory at main() (ps4_fatal_init). Every libkernel thread stack is 2 MiB of it (orbis-compat
// floor), and libkernel keeps joined stacks for reuse instead of unmapping them, so (this - now) / 2 MiB is the
// number of stacks libkernel holds (live + cached), the figure that matched the Demon's Souls 0.28 failure.
std::atomic<unsigned long long> s_boot_flexible{0};

template <std::size_t N>
void thread_census_text(fixed_text<N>& t) noexcept
{
    orbis::ThreadCensus c{};
    orbis::threadCensus(&c);
    const unsigned long long flex = ps4_flexible_memory_free();
    const unsigned long long boot = s_boot_flexible.load(std::memory_order_relaxed);
    // exited counts threadNoteExit (pthread_exit of a thread_base), joined counts threadNoteJoin; a thread_base
    // that returns (e.g. the own-stack self-test thread) is joined without an exit note, so clamp at 0.
    const unsigned long unjoined = c.exited > c.joined ? c.exited - c.joined : 0;
    t.s("flexible memory: ").u(flex).s(" bytes free (").u(boot).s(" at main); threads: orbis created ").u(c.created_ok)
     .s(", failed ").u(c.failed).s(" (attr ").u(c.attr_failed).s("), running ~").u(c.live_estimate)
     .s(", exited not joined ").u(unjoined).s(", joined ").u(c.joined).s(", returned ").u(c.finished)
     .s("; libkernel stacks held ~");
    if (boot && flex && flex <= boot) t.u((boot - flex + (1ull << 20)) >> 21);
    else t.s("?");
    t.s(" (2 MiB each = flexible used since main)");
    if (c.stack_high > c.stack_low && c.stack_low) t.s(", stack span ").u((c.stack_high - c.stack_low) >> 20).s(" MiB");
    const auto live = ps4_thread_registry::get_counters();
    t.s("; registry ").u(live.live).s(" live (peak ").u(live.peak).s(", ").u(live.entered).s(" entered, full ").u(live.full)
     .s(", leaked ").u(live.leaked).s(")");
}

constexpr const char* fatal_path = "/data/rpcs3/fatal.txt";
constexpr const char* startup_path = "/data/rpcs3/startup.log";

// Constant-initialized: usable before main and from any thread.
std::atomic<int> s_reporting{0};           // first fatal wins; later ones park
std::atomic<bool> s_pending{false};        // fatal.txt already holds this session's first record
std::atomic<bool> s_thread_fatal{false};   // consumed by the main loop
std::atomic<bool> s_thread_reason_set{false};
// 0.29: thread-resource exhaustion ("in function 'start'") has its own slot, so it is never
// dropped behind another pending thread fatal: the frontend must leave instead of trying a stop that needs threads.
std::atomic<bool> s_exhaustion_fatal{false};
std::atomic<bool> s_exhaustion_reason_set{false};
char s_exhaustion_reason[512];
std::atomic<bool> s_leaving{false};
std::atomic<bool> s_background{false};
std::atomic<bool> s_overlay{false};
std::atomic<unsigned> s_orbis_lines{0};
std::atomic<unsigned> s_orbis_fatal_lines{0};
std::atomic<unsigned> s_orbis_heap_lines{0};           // heap-cap lines (CAP, FALLING BACK, REFUSED), own budget
std::atomic<unsigned> s_orbis_fatal_heap_lines{0};
std::atomic<unsigned long long> s_orbis_hour{0};       // hour of process time the line counters above belong to
std::atomic<unsigned long long> s_orbis_fatal_hour{0};
char s_thread_reason[512];
fixed_text<8192> s_report; // constant-initialized; written only by the first reporter (s_reporting)
bool s_coredump_mode = false;              // /data/rpcs3/coredump.flag at init
bool s_no_loadexec = false;                // /data/rpcs3/no-loadexec.flag at init

void write_all(int fd, const char* p, std::size_t n) noexcept
{
    while (fd >= 0 && n)
    {
        const ssize_t w = ::write(fd, p, n);
        if (w <= 0) return;
        p += w;
        n -= static_cast<std::size_t>(w);
    }
}

void write_file(const char* path, const char* data, std::size_t size, bool truncate, bool durable) noexcept
{
    const int fd = ::open(path, O_WRONLY | O_CREAT | (truncate ? O_TRUNC : O_APPEND), 0666);
    if (fd < 0) return;
    write_all(fd, data, size);
    if (durable) ::fsync(fd); // survives the shell's kill that follows a fatal error
    ::close(fd);
}

template <std::size_t N>
void write_file(const char* path, const fixed_text<N>& t, bool truncate, bool durable = true) noexcept
{
    write_file(path, t.b, t.n, truncate, durable);
}

void startup_line(const char* a, const char* b = nullptr, bool durable = true) noexcept
{
    fixed_text<1024> t;
    t.u(sceKernelGetProcessTime()).s(" ").s(a).s(b ? b : "");
    t.s("\n");
    sceKernelDebugOutText(0, t.b); // a direct kernel write (not printf): pass finished text only
    write_file(startup_path, t, false, durable);
}

void notify(const char* message) noexcept
{
    // Same request as OpenOrbis samples/trophies/util.h and RoberixUSB/src/usb_runtime.cpp.
    OrbisNotificationRequest request{};
    request.type = NotificationRequest;
    request.targetId = -1;
    // 0.39: a long text is cut on a UTF-8 boundary (translated texts), never inside a character
    if (!message) message = "";
    const std::size_t n = utils::ps4::i18n::utf8_fit(message, sizeof(request.message) - 1);
    std::memcpy(request.message, message, n);
    request.message[n] = '\0';
    sceKernelSendNotificationRequest(0, &request, sizeof(request), 0);
}

template <std::size_t N>
void thread_name(fixed_text<N>& t) noexcept
{
    char name[64]{};
    const OrbisPthread self = scePthreadSelf();
    if (scePthreadGetname(self, name) == 0 && name[0]) t.s(name);
    else t.s("?");
    // Same value as the "atomic waiter: tid=" lines (thread_ctrl::get_tid == pthread_self).
    // C-style cast: pthread_t is an integer in some SDK headers and a pointer in others.
    t.s(" tid=").x(static_cast<unsigned long long>((std::uintptr_t)::pthread_self()));
}

// 0.39: "\ntitle: <TITLE ID>" of the game that was running (adapt title: seqlock read, no allocation); nothing outside
// a game. The next start names the game from it (ps4_fatal_detail::previous_fatal_title).
template <std::size_t N>
void title_text(fixed_text<N>& t) noexcept
{
    char title[utils::ps4::adapt::title_size];
    if (utils::ps4::adapt::current_title(title)) t.s("\ntitle: ").s(title);
}

template <std::size_t N>
void describe_exception(fixed_text<N>& t) noexcept
{
    const std::type_info* type = abi::__cxa_current_exception_type();
    if (!type)
    {
        t.s("none (explicit std::terminate: joinable std::thread destroyed/reassigned, noexcept violation, or rethrow without exception)");
        return;
    }
    t.s("type=").s(type->name());
#if defined(__cpp_exceptions) || defined(__EXCEPTIONS)
    try
    {
        std::rethrow_exception(std::current_exception());
    }
    catch (const std::system_error& e)
    {
        t.s(" code=").u(static_cast<unsigned>(e.code().value())).s(" category=").s(e.code().category().name()).s(" what=").s(e.what());
    }
    catch (const std::bad_alloc& e)
    {
        t.s(" what=").s(e.what()).s(" (heap exhausted; see 'orbis:' lines in startup.log)");
    }
    catch (const std::exception& e)
    {
        t.s(" what=").s(e.what());
    }
    catch (...)
    {
        t.s(" (non-std exception)");
    }
#else
    t.s(" (built without -fexceptions: what() unavailable)");
#endif
}

// 0.31: whether a process fatal is an out-of-memory death. Allocation-free:
// - a std::bad_alloc is in flight (operator new under the orbis-compat heap cap; in -fno-exceptions code it reaches
//   terminate with the exception still active);
// - LLVM's allocation failure handler reported it through report_fatal_error ("Allocation failed", "Buffer
//   allocation failed", "out of memory");
// - the orbis-compat heap reached its cap earlier in this session.
bool out_of_memory_fatal(const char* reason) noexcept
{
    if (const std::type_info* type = abi::__cxa_current_exception_type())
    {
        if (*type == typeid(std::bad_alloc) || *type == typeid(std::bad_array_new_length)) return true;
    }
    if (reason && (std::strstr(reason, "llocation failed") || std::strstr(reason, "out of memory"))) return true;
    orbis::MmapDirectStats heap{};
    orbis::mmapDirectStats(&heap);
    return heap.critical != 0;
}

// 0.34: a thread fatal caused by an allocation failure while the heap is at its cap or falling back to flexible
// memory (memory governor level 2). Allocation-free.
bool heap_exhausted_thread_fatal(const char* reason) noexcept
{
    if (!reason) return false;
    if (!std::strstr(reason, "simple_array") && !std::strstr(reason, "realloc") && !std::strstr(reason, "bad_alloc") &&
        !std::strstr(reason, "out of memory") && !std::strstr(reason, "llocation failed"))
    {
        return false;
    }
    orbis::MmapDirectStats heap{};
    orbis::mmapDirectStats(&heap);
    return heap.critical != 0 || heap.fallback_bytes != 0;
}

// 0.34: the heap counters for a fatal record (relaxed loads only).
template <std::size_t N>
void heap_text(fixed_text<N>& t) noexcept
{
    orbis::MmapDirectStats heap{};
    orbis::mmapDirectStats(&heap);
    t.s("heap: held ").u(heap.held >> 20).s(" of ").u(heap.cap >> 20).s(" MiB, live ").u(heap.live >> 20).s(" MiB (small heap ")
     .u(heap.small_heap >> 20).s(" MiB), ").u(heap.fallback_bytes >> 20).s(" MiB fell back to flexible").s(heap.critical ? ", AT CAP" : "");
}

template <std::size_t N>
void backtrace_offsets(fixed_text<N>& t) noexcept
{
    void* frames[40];
    const int count = backtrace(frames, 40); // works because the port builds -funwind-tables
    const auto base = reinterpret_cast<std::uintptr_t>(__text_start);
    t.s("backtrace (offsets into rpcs3_ps4.elf; llvm-symbolizer --obj=rpcs3_ps4.elf <offset>):\n");
    for (int i = 0; i < count && !t.full(); ++i)
    {
        const auto ip = reinterpret_cast<std::uintptr_t>(frames[i]);
        t.s("  #").u(static_cast<unsigned>(i)).s(" ");
        if (ip >= base && ip - base < 0x10000000) t.s("+").x(ip - base);
        else t.x(ip).s(" (outside the ELF: JIT or system module)");
        t.s("\n");
    }
}

// 0.39: the return addresses of the frame-pointer chain, from the reporter's frame on (clang keeps frame pointers for
// the FreeBSD target). The kernel enters a signal handler with the interrupted RBP, which the handler's prologue saves,
// so after the handler's frames and one return into the signal trampoline (outside the ELF) the chain goes on in the
// code that faulted, where backtrace() above stops. Every frame is checked against the mapping the kernel reports
// (readable, the 16 bytes inside it); the walk ends at a frame pointer that does not grow (one jump is allowed after a
// return outside the ELF: the handler may run on the alternate signal stack), at 0, or after 24 frames. Signal-safe:
// no allocation, one sceKernelQueryMemoryProtection per new mapping.
template <std::size_t N>
void frame_chain_offsets(fixed_text<N>& t) noexcept
{
    const auto base = reinterpret_cast<std::uintptr_t>(__text_start);
    t.s("frame chain (RBP walk; after a return outside the ELF come the frames of the interrupted code):");
    std::uintptr_t fp = reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0));
    std::uintptr_t mapped_lo = 0, mapped_hi = 0;
    bool jumped = false;
    for (unsigned depth = 0; depth < 24 && !t.full(); ++depth)
    {
        if (!fp || (fp & 7) || fp > UINTPTR_MAX - 16) break;
        if (fp < mapped_lo || fp + 16 > mapped_hi)
        {
            void* begin = nullptr;
            void* end = nullptr;
            int32_t protection = 0;
            if (sceKernelQueryMemoryProtection(reinterpret_cast<void*>(fp), &begin, &end, &protection) != 0 || !(protection & 1)) break;
            mapped_lo = reinterpret_cast<std::uintptr_t>(begin);
            mapped_hi = reinterpret_cast<std::uintptr_t>(end);
            if (fp < mapped_lo || fp + 16 > mapped_hi) break;
        }
        const auto* frame = reinterpret_cast<const std::uintptr_t*>(fp);
        const std::uintptr_t next = frame[0];
        const std::uintptr_t ret = frame[1];
        if (!ret) break;
        const bool inside = ret >= base && ret - base < 0x10000000;
        t.s(" ");
        if (inside) t.s("+").x(ret - base);
        else t.x(ret).s("(outside)");
        if (next <= fp)
        {
            if (inside || jumped) break;
            jumped = true;
        }
        fp = next;
    }
    t.s("\n");
}

[[noreturn]] void park(const char* why) noexcept
{
    // Never return and never self-terminate: the shell's kill is the only CE-free end.
    for (unsigned round = 0;; ++round)
    {
        if (round % 10 == 0 && round < 30) startup_line("fatal: thread parked; close the application from the PS button menu: ", why);
        sceKernelUsleep(30'000'000);
    }
}

[[noreturn]] void finish(const char* why) noexcept
{
    if (s_coredump_mode)
    {
        startup_line("fatal: coredump.flag present; raising SIGABRT for a system crash dump");
        logs::listener::sync_all();
        std::signal(SIGABRT, SIG_DFL);
        ::raise(SIGABRT);
    }
    park(why);
}

// 0.39: the result line of orbis::threadLimitProbe ("thread limit probe: N extra thread(s) alive at once ... - THIS IS A
// PER-PROCESS LIMIT ON THREADS ..."), kept for ps4_take_thread_limit_probe. Written on the probing thread before the
// probe returns; state 0 = none, 1 = stopped at a per-process limit, 2 = any other answer.
std::atomic<unsigned> s_probe_extra{0};
std::atomic<int> s_probe_state{0};

void note_thread_limit_probe(std::string_view line) noexcept
{
    constexpr std::string_view prefix = "thread limit probe: ";
    if (!line.starts_with(prefix) || line.find(" extra thread(s) alive at once") == std::string_view::npos) return;
    unsigned extra = 0;
    std::size_t i = prefix.size();
    for (; i < line.size() && line[i] >= '0' && line[i] <= '9' && extra < 100'000; ++i) extra = extra * 10 + static_cast<unsigned>(line[i] - '0');
    if (i == prefix.size()) return;
    s_probe_extra.store(extra, std::memory_order_relaxed);
    s_probe_state.store(line.find("PER-PROCESS LIMIT ON THREADS") != std::string_view::npos ? 1 : 2, std::memory_order_release);
}

void orbis_line(const char* fmt, va_list ap, bool fatal) noexcept
{
    // Bounded: some orbis-compat sites can repeat (mmap fallback, path anchoring). 0.34: the budget is per hour of
    // process time instead of per process, so a long session still gets its late lines. Lines about the heap cap
    // (CAP, FALLING BACK, REFUSED) count against a budget of their own, so other lines can never crowd them out.
    // orbis-compat thins those out where they are written (once, a fixed number, or at 1, 2, 4, 8 ... occurrences,
    // with the totals in the census), so they stay far below it; it only bounds a site that would repeat per
    // request, because the heap lines are written while the allocator lock is held.
    auto& window = fatal ? s_orbis_fatal_hour : s_orbis_hour;
    const unsigned long long hour = sceKernelGetProcessTime() / 3'600'000'000ull;
    unsigned long long seen = window.load(std::memory_order_relaxed);
    if (seen != hour && window.compare_exchange_strong(seen, hour, std::memory_order_relaxed))
    {
        (fatal ? s_orbis_fatal_lines : s_orbis_lines).store(0, std::memory_order_relaxed);
        (fatal ? s_orbis_fatal_heap_lines : s_orbis_heap_lines).store(0, std::memory_order_relaxed);
    }
    char line[512];
    std::vsnprintf(line, sizeof(line), fmt, ap);
    if (!fatal) note_thread_limit_probe(line); // before the line budget below can drop it
    const bool heap_line = std::strstr(line, "CAP") || std::strstr(line, "FALLING BACK") || std::strstr(line, "REFUSED");
    const unsigned budget = heap_line ? (fatal ? 64u : 128u) : (fatal ? 64u : 256u);
    auto& counter = heap_line ? (fatal ? s_orbis_fatal_heap_lines : s_orbis_heap_lines) : (fatal ? s_orbis_fatal_lines : s_orbis_lines);
    const unsigned index = counter.fetch_add(1, std::memory_order_relaxed);
    if (index > budget) return;
    if (index == budget)
    {
        std::snprintf(line, sizeof(line), "further orbis-compat %s suppressed until the next hour of this session",
            heap_line ? "heap-cap lines (CAP, FALLING BACK, REFUSED)" : "lines");
    }
    startup_line(fatal ? "orbis fatal: " : "orbis: ", line, fatal); // fsync only for fatal lines
}

void orbis_to_startup(const char* fmt, va_list ap)
{
    orbis_line(fmt, ap, false);
}

void orbis_to_startup_fatal(const char* fmt, va_list ap)
{
    orbis_line(fmt, ap, true);
}

void termination_signal(int) noexcept
{
    // Async-signal-safe: open/write/close and an atomic store. Never _exit (SIGSYS => CE).
    // This handler returns, so preserve errno for the interrupted thread.
    const int saved_errno = errno;
    static constexpr char marker[] = "shutdown: termination signal received; exit requested\n";
    const int fd = ::open(startup_path, O_WRONLY | O_APPEND);
    if (fd >= 0)
    {
        write_all(fd, marker, sizeof(marker) - 1);
        ::close(fd);
    }
    ps4_request_shutdown();
    errno = saved_errno;
}

// 0.36: the two logs an abort() leaves behind. orbis-compat's abort(), assert and libc++abi abort_message append to
// /data/orbis-abort.log across sessions; vkloader rewrites /data/tempest-vkloader.log with the first line a process
// logs (its FATAL line comes right before its abort()). Their state at boot (ps4_fatal_init) lets the SIGABRT handler
// quote only what this process wrote.
constexpr const char* abort_log_path = "/data/orbis-abort.log";
constexpr const char* vkloader_log_path = "/data/tempest-vkloader.log";

struct log_state
{
    long long size = -1; // -1: missing or unreadable
    long long mtime_sec = 0;
    long long mtime_nsec = 0;
};

log_state s_abort_log_boot;
log_state s_vkloader_log_boot;

log_state file_state(int fd) noexcept
{
    log_state state;
    struct ::stat info{};
    if (fd >= 0 && ::fstat(fd, &info) == 0)
    {
        state.size = static_cast<long long>(info.st_size);
        state.mtime_sec = static_cast<long long>(info.st_mtim.tv_sec);
        state.mtime_nsec = static_cast<long long>(info.st_mtim.tv_nsec);
    }
    return state;
}

log_state file_state(const char* path) noexcept
{
    const int fd = ::open(path, O_RDONLY);
    const log_state state = file_state(fd);
    if (fd >= 0) ::close(fd);
    return state;
}

// What this process added to `path`, at most its last size - 1 bytes, NUL-terminated; returns the byte count (0 when
// nothing). appended: the bytes after the boot size (all of it when the file shrank); otherwise the whole file once it
// differs from its boot state. open/fstat/lseek/read only.
std::size_t read_new_tail(const char* path, const log_state& boot, bool appended, char* buf, std::size_t size) noexcept
{
    buf[0] = 0;
    const int fd = ::open(path, O_RDONLY);
    if (fd < 0) return 0;
    const log_state now = file_state(fd);
    long long from = -1;
    if (now.size > 0)
    {
        if (appended) from = boot.size >= 0 && now.size >= boot.size ? boot.size : 0;
        else if (now.size != boot.size || now.mtime_sec != boot.mtime_sec || now.mtime_nsec != boot.mtime_nsec) from = 0;
    }
    std::size_t got = 0;
    if (from >= 0 && now.size > from)
    {
        const long long window = static_cast<long long>(size - 1);
        const long long start = now.size - from > window ? now.size - window : from;
        if (::lseek(fd, static_cast<off_t>(start), SEEK_SET) == static_cast<off_t>(start))
        {
            while (got < size - 1)
            {
                const ssize_t r = ::read(fd, buf + got, size - 1 - got);
                if (r <= 0) break;
                got += static_cast<std::size_t>(r);
            }
        }
    }
    ::close(fd);
    buf[got] = 0;
    return got;
}

// The index-th non-empty line from the end of text (0: the last one), without its line break; empty when none.
std::string_view line_from_end(const char* text, std::size_t n, unsigned index) noexcept
{
    std::size_t end = n;
    for (;;)
    {
        while (end && (text[end - 1] == '\n' || text[end - 1] == '\r')) end--;
        if (!end) return {};
        std::size_t begin = end;
        while (begin && text[begin - 1] != '\n') begin--;
        if (!index--) return {text + begin, end - begin};
        end = begin;
    }
}

// The hexadecimal number after `key` in line (an optional 0x prefix), as orbis-compat prints %p.
bool hex_after(std::string_view line, std::string_view key, unsigned long long& value) noexcept
{
    const std::size_t at = line.find(key);
    if (at == std::string_view::npos) return false;
    std::size_t i = at + key.size();
    if (line.substr(i, 2) == "0x") i += 2;
    unsigned long long v = 0;
    unsigned digits = 0;
    for (; i < line.size() && digits < 16; i++, digits++)
    {
        const char c = line[i];
        const int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) break;
        v = v * 16 + static_cast<unsigned>(d);
    }
    if (!digits) return false;
    value = v;
    return true;
}

void abort_signal(int) noexcept
{
    // Reached from orbis-compat abort() -> raise(SIGABRT) (Mesa, LLVM, libunwind, assert, vkloader).
    // 0.36: the reason names the caller orbis-compat logged (as an rpcs3_ps4.elf offset), the assert or libc++abi
    // message logged before it, and vkloader's FATAL line when the loader aborted on an entry point. Fixed buffers.
    fixed_text<1024> reason;
    reason.s("abort() called");
    bool caller_known = false;
    char tail[1024];
    std::size_t n = read_new_tail(abort_log_path, s_abort_log_boot, true, tail, sizeof(tail));
    for (unsigned i = 0; i < 2; i++)
    {
        std::string_view line = line_from_end(tail, n, i);
        if (line.empty()) break;
        if (line.starts_with("[orbis] ")) line.remove_prefix(8);
        unsigned long long caller = 0;
        if (line.starts_with("abort: ") && hex_after(line, "called from ", caller))
        {
            if (caller_known) break; // an older abort line: its message belongs to another abort
            const auto base = reinterpret_cast<std::uintptr_t>(__text_start);
            reason.s("; caller ");
            if (caller >= base && caller - base < 0x10000000) reason.s("+").x(caller - base).s(" (llvm-symbolizer --obj=rpcs3_ps4.elf)");
            else reason.x(caller).s(" (outside the ELF)");
            caller_known = true;
            continue;
        }
        reason.s("; ").s(line.substr(0, 320));
        break;
    }
    if (!caller_known) reason.s(" (no caller line from this session in /data/orbis-abort.log)");
    n = read_new_tail(vkloader_log_path, s_vkloader_log_boot, false, tail, sizeof(tail));
    if (const std::string_view line = line_from_end(tail, n, 0); line.find("FATAL") != std::string_view::npos)
        reason.s("; ").s(line.substr(0, 320));
    ps4_fatal_report(reason.b, "SIGABRT");
}

void exit_guard()
{
    // atexit/at_quick_exit: runs inside exit() before static destructors registered before it.
    ps4_fatal_report("exit() was called; static destructors were blocked to keep emulation threads alive", "exit");
}
} // namespace

unsigned long long ps4_boot_flexible_memory_free() noexcept
{
    return s_boot_flexible.load(std::memory_order_relaxed);
}

std::size_t ps4_thread_census_line(char* out, std::size_t size) noexcept
{
    if (!out || !size) return 0;
    fixed_text<768> t;
    thread_census_text(t);
    const std::size_t n = std::min(t.n, size - 1);
    std::memcpy(out, t.b, n);
    out[n] = 0;
    return n;
}

extern "C" [[noreturn]] void ps4_exit_called(int code)
{
    fixed_text<64> t;
    t.s("exit(").u(static_cast<unsigned>(code)).s(") called");
    ps4_fatal_report(t.b, "exit");
}

void ps4_fatal_note(const char* prefix, const char* detail) noexcept
{
    startup_line(prefix ? prefix : "", detail, true);
}

void ps4_notify(const char* message) noexcept
{
    notify(message ? message : "");
}

void ps4_fatal_init() noexcept
{
    // Rotate startup.log/fatal.txt first (idempotent): every line below belongs to this session.
    ps4_rotate_session_logs();
    // 0.29: baseline of the libkernel stack estimate, before main creates any thread.
    s_boot_flexible.store(ps4_flexible_memory_free(), std::memory_order_relaxed);
    s_coredump_mode = ::access("/data/rpcs3/coredump.flag", F_OK) == 0;
    s_no_loadexec = ::access("/data/rpcs3/no-loadexec.flag", F_OK) == 0;
    orbis_set_log(orbis_to_startup);        // orbis-compat OOM / clock / mmap diagnostics were silent
    orbis_set_log_fatal(orbis_to_startup_fatal);
    std::set_terminate(ps4_terminate_handler); // Utilities/Thread.cpp also points here (pre-main)
    std::atexit(exit_guard);
    ::at_quick_exit(exit_guard);             // exported by the SDK libc.a (at_quick_exit.lo)
    struct sigaction termination{};
    sigemptyset(&termination.sa_mask);
    termination.sa_handler = termination_signal;
    for (const int sig : {SIGTERM, SIGHUP, SIGINT, SIGQUIT}) // same numbers on FreeBSD
        ::sigaction(sig, &termination, nullptr);
    // 0.36: what the abort handler compares against (see read_new_tail)
    s_abort_log_boot = file_state(abort_log_path);
    s_vkloader_log_boot = file_state(vkloader_log_path);
    if (!s_coredump_mode)
    {
        struct sigaction abort_action{};
        sigemptyset(&abort_action.sa_mask);
        abort_action.sa_handler = abort_signal;
        ::sigaction(SIGABRT, &abort_action, nullptr);
    }
    const int32_t system_service = sceSysmoduleLoadModuleInternal(ORBIS_SYSMODULE_INTERNAL_SYSTEM_SERVICE);
    startup_line(s_coredump_mode ? "fatal policy: record + SIGABRT core dump (coredump.flag)" : "fatal policy: record + park (no CE)");
    if (system_service < 0)
    {
        fixed_text<96> t;
        t.s("fatal policy: libSceSystemService load returned ").x(static_cast<std::uint32_t>(system_service));
        startup_line(t.b);
    }
}

[[noreturn]] void ps4_fatal_report(const char* reason, const char* origin) noexcept
{
    if (s_reporting.exchange(1) != 0) park(reason); // a second failure while reporting: keep the first record
    // A thread fatal recorded first keeps its place (it is usually the cause); append to it.
    const bool thread_fatal_recorded = s_pending.exchange(true);
    auto& t = s_report; // keep the large buffer off small native stacks
    t.s("RPCS3 PS4 EDITION ").s(ps4_build_version).s(" fatal error\n")
     .s("origin: ").s(origin ? origin : "?").s("\nreason: ").s(reason ? reason : "?")
     .s("\nthread: ");
    thread_name(t);
    title_text(t);
    t.s("\nkernel internal memory free: ").u(ps4_internal_memory_free()).s(" bytes\n");
    thread_census_text(t); // 0.29: thread census (relaxed loads only, no allocation)
    t.s("\nuptime_us: ").u(sceKernelGetProcessTime())
     .s("\nfocus: background=").u(s_background.load()).s(" overlay=").u(s_overlay.load()).s("\n");
    if (abi::__cxa_current_exception_type())
    {
        t.s("exception: ");
        describe_exception(t);
        t.s("\n");
    }
    backtrace_offsets(t);
    frame_chain_offsets(t);
    sceKernelDebugOutText(0, t.b);
    write_file(fatal_path, t, !thread_fatal_recorded); // fatal.txt: the XMB notice on the next boot
    write_file(startup_path, t, false); // startup.log, fsync
    // 0.31: an out-of-memory death makes the next boots of the running game compile with one thread (boot policy).
    // open/write/fsync only; nothing is written while no game title is set.
    if (out_of_memory_fatal(reason) && utils::ps4::adapt::note(utils::ps4::adapt::key_llvm_threads_1))
        startup_line("adapt: out of memory; the next boots of this game compile with one thread (llvm_threads=1)");
    // Toast before the logger: the logger (or the heap) may be what failed.
    notify(utils::ps4::i18n::tr(utils::ps4::i18n::str::FATAL_RPCS3_PS4_ERRO_FATAL_REGISTRADO));
    ps4_fatal_log.fatal("%s", static_cast<const char*>(t.b));
    finish(reason);
}

[[noreturn]] void ps4_terminate_handler() noexcept
{
    ps4_fatal_report("std::terminate (uncaught C++ exception in -fno-exceptions code, or explicit terminate)", "terminate");
}

void ps4_note_thread_fatal(const char* reason) noexcept
{
    // A named thread is ending through emergency_exit (already logged as F in RPCS3.log).
    // Persist it for the next boot and ask the frontend to stop the guest.
    static std::atomic<unsigned> s_notes{0};
    const unsigned index = s_notes.fetch_add(1, std::memory_order_relaxed);
    fixed_text<4096> t;
    t.s("RPCS3 PS4 EDITION ").s(ps4_build_version).s(" thread fatal error\norigin: emergency_exit\nreason: ")
     .s(reason ? reason : "?").s("\nthread: ");
    thread_name(t);
    title_text(t);
    t.s("\nuptime_us: ").u(sceKernelGetProcessTime()).s("\nkernel internal memory free: ").u(ps4_internal_memory_free()).s(" bytes\n");
    thread_census_text(t); // 0.29: thread census (relaxed loads only, no allocation)
    t.s("\n");
    heap_text(t);
    t.s("\n");
    if (index < 8)
    {
        backtrace_offsets(t);
        frame_chain_offsets(t);
    }
    if (!s_pending.exchange(true)) write_file(fatal_path, t, true);
    if (index < 16) write_file(startup_path, t, false);
    if (reason && std::strstr(reason, "in function 'start'"))
    {
        // 0.29: thread resources exhausted (a refused must-succeed thread_base::start, or a refused 'Emulation Join
        // Thread'): its own slot, taken first by ps4_take_thread_fatal, so another pending reason cannot hide it.
        if (!s_exhaustion_reason_set.exchange(true))
        {
            std::snprintf(s_exhaustion_reason, sizeof(s_exhaustion_reason), "%s", reason);
            s_exhaustion_fatal.store(true, std::memory_order_release);
        }
    }
    else if (!s_thread_reason_set.exchange(true))
    {
        std::snprintf(s_thread_reason, sizeof(s_thread_reason), "%s", reason ? reason : "?");
        s_thread_fatal.store(true, std::memory_order_release);
    }
    if (index == 0)
    {
        // 0.34: out of heap at the 2 GiB cap: say so, since only an application restart gives that memory back.
        if (heap_exhausted_thread_fatal(reason))
        {
            startup_line("fatal: thread failed with the heap at its cap (memory governor level 2); restart advised");
            notify(utils::ps4::i18n::tr(utils::ps4::i18n::str::FATAL_MEMORIA_EMULADOR_ESGOTADA_2_GB));
        }
        else
        {
            notify(utils::ps4::i18n::tr(utils::ps4::i18n::str::FATAL_RPCS3_PS4_THREAD_EMULADOR_FALHOU));
        }
    }
}

bool ps4_fatal_pending() noexcept
{
    return s_pending.load();
}

bool ps4_take_thread_fatal(char* reason, std::size_t size) noexcept
{
    // 0.29: a pending thread-resource exhaustion is returned before any other pending reason.
    if (s_exhaustion_fatal.exchange(false, std::memory_order_acquire))
    {
        if (reason && size) std::snprintf(reason, size, "%s", s_exhaustion_reason);
        s_exhaustion_reason_set.store(false, std::memory_order_release);
        return true;
    }
    if (!s_thread_fatal.exchange(false, std::memory_order_acquire)) return false;
    if (reason && size) std::snprintf(reason, size, "%s", s_thread_reason);
    s_thread_reason_set.store(false, std::memory_order_release);
    return true;
}

bool ps4_thread_exhaustion_pending() noexcept
{
    return s_exhaustion_fatal.load(std::memory_order_acquire);
}

bool ps4_in_background() noexcept
{
    return s_background.load(std::memory_order_relaxed) || s_overlay.load(std::memory_order_relaxed);
}

bool ps4_try_leave_application(const char* why) noexcept
{
    startup_line("shutdown: leave application: ", why);
    if (s_no_loadexec)
    {
        startup_line("shutdown: no-loadexec.flag present; staying open for the PS button menu");
        return false;
    }
    // Common PS4 homebrew idiom (undocumented): the shell then closes us like "Close application".
    const int32_t result = sceSystemServiceLoadExec("exit", nullptr);
    fixed_text<128> t;
    t.s("shutdown: sceSystemServiceLoadExec(\"exit\") returned ").x(static_cast<std::uint32_t>(result));
    startup_line(t.b);
    for (int i = 0; i < 50; ++i) sceKernelUsleep(100'000); // accepted => closed within this window
    startup_line("shutdown: still running 5 s after LoadExec; the shell did not close the application");
    return false;
}

[[noreturn]] void ps4_leave_application(const char* why) noexcept
{
    if (!s_leaving.exchange(true)) ps4_try_leave_application(why);
    notify(utils::ps4::i18n::tr(utils::ps4::i18n::str::FATAL_RPCS3_PS4_EMULADOR_ENCERRADO_FECHE));
    park(why); // never _exit/_Exit/exit/abort: SIGSYS/SIGABRT => CE-34878-0
}

std::string ps4_take_previous_fatal(std::string* reason, std::string* title)
{
    if (reason) reason->clear();
    if (title) title->clear();
    const fs::file file("/data/rpcs3/previous-fatal.txt");
    if (!file || file.size() == 0 || file.size() > 64 * 1024) return {};
    const std::string record = file.to_string();
    const std::string notice = ps4_fatal_detail::previous_fatal_notice(record, utils::ps4::i18n::tr(utils::ps4::i18n::str::FATAL_SESSAO_ANTERIOR_TERMINOU_ERRO));
    if (!notice.empty()) ps4_fatal_log.warning("Previous session fatal record: %s", notice);
    if (reason) *reason = ps4_fatal_detail::previous_fatal_reason(record);
    if (title) *title = ps4_fatal_detail::previous_fatal_title(record);
    return notice;
}

bool ps4_take_thread_limit_probe(unsigned& extra, bool& limit) noexcept
{
    const int state = s_probe_state.exchange(0, std::memory_order_acquire);
    if (!state) return false;
    extra = s_probe_extra.load(std::memory_order_relaxed);
    limit = state == 1;
    return true;
}

namespace
{
// 0.33: rest-mode clock check. The guest time base is the raw TSC (sys_time.cpp get_system_time/get_timebased_time);
// if the TSC stops, jumps or restarts across rest mode, guest time does too. Rest mode suspends the whole process, so
// a poll gap (100 ms cadence) over 2 s keeps the samples on both sides of it, and ON_RESUME reports them.
struct clock_sample
{
    std::uint64_t tsc = 0, process_us = 0, wall_us = 0;
};

clock_sample read_clocks() noexcept
{
    clock_sample s;
    s.tsc = utils::get_tsc();
    s.process_us = sceKernelGetProcessTime();
    OrbisKernelTimeval tv{};
    if (sceKernelGettimeofday(&tv) == 0 && tv.tv_sec > 0 && tv.tv_usec >= 0)
        s.wall_us = static_cast<std::uint64_t>(tv.tv_sec) * 1'000'000ull + static_cast<std::uint64_t>(tv.tv_usec);
    return s;
}

std::uint64_t abs_diff(std::uint64_t a, std::uint64_t b) noexcept { return a > b ? a - b : b - a; }

// b - a in ms, signed.
template <std::size_t N>
void put_delta_ms(fixed_text<N>& t, std::uint64_t a, std::uint64_t b, std::uint64_t units_per_ms) noexcept
{
    t.s(b >= a ? "+" : "-").u(abs_diff(a, b) / units_per_ms).s(" ms");
}

void report_resume_clocks(const clock_sample& now, const clock_sample& before, const clock_sample& after) noexcept
{
    const std::uint64_t hz = sceKernelGetTscFrequency();
    const bool hz_ok = hz >= 1'000'000;
    const std::uint64_t tsc_per_ms = hz_ok ? hz / 1000 : 1; // unknown rate: raw ticks, no verdict
    fixed_text<768> t;
    t.s("resume clocks: tsc=").u(now.tsc).s(" process_us=").u(now.process_us).s(" wall_us=").u(now.wall_us).s(" tsc_hz=").u(hz);
    if (!before.process_us)
    {
        t.s(" | no poll gap over 2 s seen in this process: no pre-suspend sample");
    }
    else
    {
        t.s(" | last sample before the gap: tsc=").u(before.tsc).s(" process_us=").u(before.process_us).s(" wall_us=").u(before.wall_us)
         .s(" | across the gap: tsc ");
        if (hz_ok) put_delta_ms(t, before.tsc, after.tsc, tsc_per_ms);
        else t.s(after.tsc >= before.tsc ? "+" : "-").u(abs_diff(before.tsc, after.tsc)).s(" ticks");
        t.s(", process ");
        put_delta_ms(t, before.process_us, after.process_us, 1000);
        if (before.wall_us && after.wall_us)
        {
            t.s(", wall ");
            put_delta_ms(t, before.wall_us, after.wall_us, 1000);
            const std::uint64_t wall_ms = abs_diff(before.wall_us, after.wall_us) / 1000;
            const std::uint64_t tsc_ms = abs_diff(before.tsc, after.tsc) / tsc_per_ms;
            if (after.tsc < before.tsc) t.s(" => TSC WENT BACKWARDS");
            else if (!hz_ok) t.s(" (TSC rate unknown: no verdict)");
            else if (abs_diff(tsc_ms, wall_ms) <= std::max<std::uint64_t>(1000, wall_ms / 50)) t.s(" => tsc followed wall time");
            else if (tsc_ms < wall_ms) t.s(" => tsc stopped during the gap");
            else t.s(" => TSC JUMPED ahead of wall time");
        }
        t.s(" (gap ended ").u(abs_diff(now.process_us, after.process_us) / 1000).s(" ms of process time before this event)");
    }
    startup_line(t.b, nullptr, false);
    ps4_fatal_log.notice("%s", static_cast<const char*>(t.b));
}
}

void ps4_poll_system_service() noexcept
{
    static u64 next_poll = 0;
    const u64 now = sceKernelGetProcessTime();
    if (now < next_poll) return;
    next_poll = now + 100'000;

    // 0.33: samples on both sides of the latest long poll gap (main thread only, like the static buffers below).
    static clock_sample s_clock_last{}, s_gap_before{}, s_gap_after{};
    const clock_sample clocks = read_clocks();
    if (s_clock_last.process_us &&
        ((s_clock_last.wall_us && clocks.wall_us > s_clock_last.wall_us + 2'000'000) || clocks.process_us > s_clock_last.process_us + 2'000'000))
    {
        s_gap_before = s_clock_last;
        s_gap_after = clocks;
    }
    s_clock_last = clocks;

    // SceSystemServiceStatus: {s32 eventNum; bool isSystemUiOverlaid; bool isInBackgroundExecution; ...}
    // (~0x88 bytes, layout from shadPS4). Main thread only, so static buffers are fine.
    alignas(16) static unsigned char status[512];
    std::memset(status, 0, sizeof(status));
    if (sceSystemServiceGetStatus(status) < 0) return;
    std::int32_t events = 0;
    std::memcpy(&events, status, 4);
    const bool overlay = status[4] != 0, background = status[5] != 0;
    const bool old_overlay = s_overlay.exchange(overlay), old_background = s_background.exchange(background);
    if (overlay != old_overlay || background != old_background)
    {
        fixed_text<96> t;
        t.s("focus: system_ui_overlay=").u(overlay).s(" background=").u(background);
        startup_line(t.b, nullptr, false);
        ps4_fatal_log.notice("%s", static_cast<const char*>(t.b));
    }
    // SceSystemServiceEvent: {u32 eventType; u8 param[8192];} (layout from shadPS4).
    alignas(16) static unsigned char event[0x3000];
    for (int i = 0; i < events && i < 16; ++i)
    {
        std::memset(event, 0, sizeof(event));
        if (sceSystemServiceReceiveEvent(event) < 0) break;
        std::uint32_t type = 0;
        std::memcpy(&type, event, 4);
        fixed_text<96> t;
        t.s("system event: type=").x(type).s(type == 0x10000000u ? " (ON_RESUME)" : "");
        startup_line(t.b, nullptr, false);
        // ON_RESUME (back from rest mode): ps4_pads re-checks users, handles and light bars on its next refresh
        // (lock-free flag). AudioOut errors reach cellAudio through UNSPECIFIED_ERROR and the backend is reopened.
        if (type == 0x10000000u)
        {
            ps4_pads::on_resume();
            report_resume_clocks(clocks, s_gap_before, s_gap_after);
        }
    }
}

// 0.26: host-side stack sample of another thread for the STALL dump (e.g. the RSX thread stuck on one
// FIFO command in 0.25). The target receives native signal 31 (FreeBSD SIGUSR2; the SDK's SIGUSR2 macro is
// the Linux 12 = FreeBSD SIGSYS, so it is not used). The handler copies RIP/RSP and the first 2 KiB above
// RSP, keeping only words that point into the executable's text (candidate return addresses), then
// returns; SA_RESTART restarts an interrupted restartable syscall. Offsets are relative to __text_start,
// like the fatal backtraces (llvm-symbolizer --obj=rpcs3_ps4.elf 0x<offset>). Mesa/Zink/RADV and LLVM are
// statically linked, so driver frames are covered too.
extern char __eh_frame_start[];
// 0.29: the handler of signal 31 belongs to the sampling profiler (rpcs3/ps4_profiler.cpp); it calls
// ps4_stall_sample_signal first, so STALL samples keep working whether or not the profiler is active.
bool ps4_profiler_install_signal() noexcept;

namespace rsx
{
// RSXThread.cpp (0.26): pthread_t of the RSX thread, 0 before it starts.
u64 ps4_rsx_native_thread() noexcept;
}

namespace
{
constexpr int ps4_native_sigusr2 = 31;

struct ps4_thread_sample
{
    std::atomic<int> state{0}; // 0 idle, 4 arming, 1 requested, 3 capturing, 2 captured
    std::uint64_t target = 0;  // 0.29: pthread_t of the requested thread; the profiler signals other threads too
    std::uint64_t rip = 0, rsp = 0;
    std::uint64_t frames[32]{};
    unsigned count = 0;
};

ps4_thread_sample s_thread_sample;

void ps4_sample_signal(int, siginfo_t*, void* context) noexcept
{
    const int saved_errno = errno;
    int expected = 1;
    // 0.29: capture only on the requested thread (a profiler signal can arrive on any registered thread).
    if (context && s_thread_sample.state.load(std::memory_order_acquire) == 1 &&
        s_thread_sample.target == static_cast<std::uint64_t>((std::uintptr_t)::pthread_self()) &&
        s_thread_sample.state.compare_exchange_strong(expected, 3, std::memory_order_acq_rel))
    {
        // Native PS4 ucontext layout (rip 0xa0 / rsp 0xb8 static_asserts in ps4_signal_context.hpp).
        const auto& machine = static_cast<const utils::ps4::signal_context*>(context)->machine;
        const std::uint64_t rip = machine.rip;
        const std::uint64_t rsp = machine.rsp;
        const auto lo = reinterpret_cast<std::uint64_t>(__text_start);
        const auto hi = reinterpret_cast<std::uint64_t>(__eh_frame_start);
        s_thread_sample.rip = rip;
        s_thread_sample.rsp = rsp;
        unsigned n = 0;
        if (rsp && !(rsp & 7))
        {
            // 2 KiB above the interrupted RSP: older frames of the same stack (a blocked thread has many).
            // 0.29: never past the registry's scan bound of this thread ([rsp, stack_hi) is mapped): an RPCS3-owned
            // stack (own-stack pool) ends where the next slot's unmapped guard starts. read() of the own slot is async-signal-safe.
            unsigned words = 256;
            if (const int self = ps4_thread_registry::self(); self >= 0)
            {
                ps4_thread_registry::slot_info info{};
                if (ps4_thread_registry::read(static_cast<u32>(self), info) && info.stack_hi > rsp && (info.stack_hi - rsp) / 8 < words)
                    words = static_cast<unsigned>((info.stack_hi - rsp) / 8);
            }
            const auto* stack = reinterpret_cast<const std::uint64_t*>(rsp);
            for (unsigned i = 0; i < words && n < 32; ++i)
            {
                const std::uint64_t value = stack[i];
                if (value > lo && value < hi) s_thread_sample.frames[n++] = value;
            }
        }
        s_thread_sample.count = n;
        s_thread_sample.state.store(2, std::memory_order_release);
    }
    errno = saved_errno;
}
}

// 0.29: called first by the shared signal-31 handler (ps4_profiler.cpp prof_signal). Async-signal-safe.
void ps4_stall_sample_signal(int sig, siginfo_t* info, void* context) noexcept
{
    ps4_sample_signal(sig, info, context);
}

std::string ps4_sample_thread_stack(std::uint64_t native_thread)
{
    if (!native_thread) return "host sample unavailable (thread handle unknown)";
    // 0.33: the RSX thread waits inside the Mesa/orbis-drm winsys (kevent/poll/ioctl), whose EINTR handling is not
    // verified; a signal there could turn a stall that would recover (long shader, readback) into a VK error or a lost
    // device. Same rule as the profiler: opt-in only. Checked per dump (rare), so the flag works without a restart.
    if (native_thread == rsx::ps4_rsx_native_thread() && ::access("/data/rpcs3/stall-sample.flag", F_OK) != 0)
        return "host sample skipped (RSX thread; /data/rpcs3/stall-sample.flag enables it)";
    // 0.29: one handler for signal 31 (installed and read back once by the profiler module); a second sigaction
    // here would silently replace the profiler's. Nothing may send 31 without it: the default action ends the process.
    if (!ps4_profiler_install_signal()) return "host sample unavailable (sigaction failed)";

    int expected = 0;
    if (!s_thread_sample.state.compare_exchange_strong(expected, 4)) return "host sample busy";
    s_thread_sample.count = 0;
    s_thread_sample.target = native_thread;
    s_thread_sample.state.store(1, std::memory_order_release); // armed: target is visible before the handler can match

    if (const int error = pthread_kill(reinterpret_cast<pthread_t>(native_thread), ps4_native_sigusr2))
    {
        s_thread_sample.state.store(0);
        return "host sample failed (pthread_kill error " + std::to_string(error) + ")";
    }

    for (int i = 0; i < 300 && s_thread_sample.state.load(std::memory_order_acquire) != 2; ++i)
    {
        sceKernelUsleep(1000);
    }

    // Not delivered in 300 ms (signal blocked, or the thread is stuck in an uninterruptible kernel wait).
    // Withdraw the request unless the handler has already started writing.
    expected = 1;
    if (s_thread_sample.state.compare_exchange_strong(expected, 0))
    {
        return "host sample not delivered within 300 ms (thread in an uninterruptible kernel wait?)";
    }

    while (s_thread_sample.state.load(std::memory_order_acquire) != 2)
    {
        sceKernelUsleep(1000);
    }

    const auto base = reinterpret_cast<std::uint64_t>(__text_start);
    std::string out = fmt::format("host rip=%s rsp=0x%llx frames:",
        s_thread_sample.rip > base ? fmt::format("+0x%llx", s_thread_sample.rip - base) : fmt::format("0x%llx (outside text)", s_thread_sample.rip),
        s_thread_sample.rsp);
    for (unsigned i = 0; i < s_thread_sample.count; ++i)
    {
        fmt::append(out, " +0x%llx", s_thread_sample.frames[i] - base);
    }
    s_thread_sample.state.store(0, std::memory_order_release);
    return out;
}
