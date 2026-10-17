// Host test for orbis-compat's direct-memory heap (src/orbis_mmap.cpp) with the libkernel calls stubbed over a
// model of the physical direct-memory pool (4608 MiB, ranges per owner): trim racing allocator threads and the
// lock-free mmapDirectDescribe reader, the carve-out ladder, the byte cap with its single overdraft (its admission
// against the pool, and the latch after a failed one), the flexible floor, musl's small-heap growth (__expand_heap,
// with and without the step cap), mmapDirectStats, and the direct-memory authority: the ledger (a
// sceKernelDirectMemoryQuery walk, estimated without it), the reservation (admission, ladder shrink, refusal log once
// a minute), live flexible-fallback bytes, the pressure hook, top-of-pool placement, small/large segregation and the
// trim on a refused carve-out. From ps4-port/:
//
//   clang++ -std=c++17 -O2 -static -D_FILE_OFFSET_BITS=64 -I tests/orbis-mmap-mocks -iquote <orbis-compat>/include
//       <knobs> tests/orbis_mmap_trim_test.cpp <orbis-compat>/src/orbis_mmap.cpp -o orbis_mmap_trim_test.exe
//
// Knob sets, each built and run on its own:
//   every 0.37 knob  -DORBIS_MMAP_MAX_CARVES=48 -DORBIS_MMAP_DIRECT_CAP_MIB=2048 -DORBIS_MMAP_CARVE_LADDER=1
//                    -DORBIS_MMAP_TRIM=1 -DORBIS_MMAP_EXPAND_STEP_MAX_MIB=0 -DORBIS_MMAP_FLEX_FLOOR_MIB=64
//   RPCS3 0.37       -DORBIS_MMAP_MAX_CARVES=48 -DORBIS_MMAP_DIRECT_CAP_MIB=2048 -DORBIS_MMAP_CARVE_LADDER=1
//                    -DORBIS_MMAP_TRIM=0 -DORBIS_MMAP_EXPAND_STEP_MAX_MIB=16 -DORBIS_MMAP_FLEX_FLOOR_MIB=64
//   none             no knob: the behaviour every other project that shares orbis-compat keeps
//   RPCS3 0.38       RPCS3 0.37 with -DORBIS_MMAP_TRIM=1 -DORBIS_MMAP_SEGREGATE=1 -DORBIS_MMAP_HEAP_BAND_MIB=2304
//                    -DORBIS_MMAP_TRIM_OFF_FLAG=\"t38-no-trim.flag\" -DORBIS_MMAP_QUERY_OFF_FLAG=\"t38-no-query.flag\"
//   flag files       RPCS3 0.38 with its own flag names and -DTEST_FLAG_FILES=1: the test creates both files before
//                    the first request (no trim, no segregation, estimated ledger)
//   no query export  RPCS3 0.38 with its own flag names and -DTEST_NO_DMQ=1: sceKernelDirectMemoryQuery is not
//                    defined, so the weak import reads null (estimated ledger)
//
// Windows host only (VirtualAlloc stands in for the mapping). Every carve-out gets a fresh address range that is
// never reused, so a reader that matches an address can be traced to exactly one carve-out.
#define NOMINMAX
#include "orbis_mmap.h"

#include <sys/mman.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <random>
#include <thread>
#include <utility>
#include <vector>

#ifndef ORBIS_MMAP_CARVE_LADDER
#define ORBIS_MMAP_CARVE_LADDER 0
#endif
#ifndef ORBIS_MMAP_TRIM
#define ORBIS_MMAP_TRIM 0
#endif
#ifndef ORBIS_MMAP_FLEX_FLOOR_MIB
#define ORBIS_MMAP_FLEX_FLOOR_MIB 0
#endif
#ifndef ORBIS_MMAP_SEGREGATE
#define ORBIS_MMAP_SEGREGATE 0
#endif
#ifndef ORBIS_MMAP_HEAP_BAND_MIB
#define ORBIS_MMAP_HEAP_BAND_MIB 0
#endif

extern "C" void* __mmap(void* addr, size_t len, int prot, int flags, int fd, off_t off);
extern "C" int __munmap(void* addr, size_t len);
extern "C" int __madvise(void* addr, size_t len, int advice);
#ifdef ORBIS_MMAP_EXPAND_STEP_MAX_MIB
extern "C" void* __expand_heap(size_t* pn);
#endif

namespace
{
constexpr std::uint64_t mib = 1024ull * 1024;
std::atomic<int> g_failures{0};

#if defined(TEST_NO_DMQ) || defined(TEST_FLAG_FILES)
constexpr bool walk_expected = false;
#else
constexpr bool walk_expected = true;
#endif

#define CHECK(cond, ...)                                                       \
    do                                                                         \
    {                                                                          \
        if (!(cond))                                                           \
        {                                                                      \
            if (g_failures.fetch_add(1) < 20)                                  \
            {                                                                  \
                std::printf("FAIL %s:%d (%s): ", __FILE__, __LINE__, #cond);   \
                std::printf(__VA_ARGS__);                                      \
                std::printf("\n");                                             \
            }                                                                  \
        }                                                                      \
    } while (0)

// One entry per sceKernelMapDirectMemory: written only under the allocator lock, read by the reader threads.
struct region
{
    std::atomic<std::uintptr_t> va{0};
    std::atomic<std::uint64_t> len{0};
    std::atomic<std::uint64_t> unmap_seq{0}; // 0 = still mapped
};
constexpr std::uint32_t max_regions = 16384;
region g_regions[max_regions];
std::atomic<std::uint32_t> g_region_count{0};
std::atomic<std::uint64_t> g_seq{1};

std::atomic<std::uint64_t> g_fail_above{~0ull};     // sceKernelAllocateDirectMemory refuses longer requests
std::atomic<std::uint64_t> g_alloc_attempts{0};
std::atomic<std::uint64_t> g_munmap_calls{0};
std::atomic<std::uint64_t> g_munmap_failed{0};
std::atomic<std::uint64_t> g_release_calls{0};
std::atomic<std::uint64_t> g_release_failed_bytes{0};
std::atomic<std::uint64_t> g_flexible_maps{0};
std::atomic<std::uint64_t> g_trim_lines{0};
std::atomic<std::uint64_t> g_ladder_lines{0};
std::atomic<std::uint64_t> g_overdraft_lines{0};
std::atomic<std::uint64_t> g_overdraft_failed_lines{0};
std::atomic<std::uint64_t> g_refused_unavailable_lines{0};
std::atomic<std::uint64_t> g_floor_lines{0};
std::atomic<std::uint64_t> g_probe_lines{0};
std::atomic<std::uint64_t> g_untrusted_lines{0};
std::atomic<std::uint64_t> g_reserve_lines{0};
std::atomic<std::uint64_t> g_flexible_queries{0};
std::atomic<std::uint64_t> g_flexible_free{1ull << 30}; // sceKernelAvailableFlexibleMemorySize
std::atomic<std::int32_t> g_flexible_rc{0};
std::atomic<std::int64_t> g_clock_offset_us{0};     // added to sceKernelGetProcessTime
char g_small_heap_line[512];
char g_floor_report_line[512];
char g_probe_line[512];
char g_ledger_line[512];
std::atomic<bool> g_inject{false};                  // occasional munmap/release failures during the race
std::atomic<bool> g_verbose{false};
std::atomic<bool> g_log_set{true};                  // orbis_log_enabled: false = no sink yet, lines are dropped

region* find_region(const void* base)
{
    const std::uint32_t n = g_region_count.load(std::memory_order_acquire);
    for (std::uint32_t i = 0; i < n; i++)
        if (g_regions[i].va.load() == reinterpret_cast<std::uintptr_t>(base)) return &g_regions[i];
    return nullptr;
}

// The process's direct-memory pool: allocated ranges by owner (0 = sceKernelAllocateDirectMemory, i.e. the heap;
// 1 = the test standing in for the GPU, guest memory and the JIT), first fit inside the requested range.
namespace pool
{
constexpr std::uint64_t total = 4608 * mib;
constexpr std::uint64_t quantum = 2 * mib;
constexpr std::uint64_t none = ~0ull;
struct range
{
    std::uint64_t start;
    std::uint64_t end;
    int owner;
};
using span = std::pair<std::uint64_t, std::uint64_t>;
std::mutex mu;
std::vector<range> ranges; // ascending, disjoint
std::vector<span> mine;    // owner 1, as (start, length)
std::uint64_t last_start = 0, last_len = 0; // the latest successful sceKernelAllocateDirectMemory

std::uint64_t up(std::uint64_t v, std::uint64_t a)
{
    return (v + a - 1) / a * a;
}

// Free runs [a, b), ascending. Callers hold mu.
std::vector<span> gaps_locked()
{
    std::vector<span> out;
    std::uint64_t cur = 0;
    for (const range& r : ranges)
    {
        if (r.start > cur) out.push_back({cur, r.start});
        cur = std::max(cur, r.end);
    }
    if (cur < total) out.push_back({cur, total});
    return out;
}

std::uint64_t first_fit_locked(std::uint64_t lo, std::uint64_t hi, std::uint64_t len, std::uint64_t align)
{
    for (span g : gaps_locked())
    {
        const std::uint64_t a = std::max(g.first, lo), b = std::min(g.second, hi);
        const std::uint64_t s = up(a, align);
        if (s < b && b - s >= len) return s;
    }
    return none;
}

bool is_free_locked(std::uint64_t s, std::uint64_t len)
{
    if (s + len > total) return false;
    for (const range& r : ranges)
        if (r.start < s + len && s < r.end) return false;
    return true;
}

void insert_locked(std::uint64_t s, std::uint64_t len, int owner)
{
    const auto it = std::lower_bound(ranges.begin(), ranges.end(), s, [](const range& r, std::uint64_t v) { return r.start < v; });
    ranges.insert(it, range{s, s + len, owner});
}

bool erase_locked(std::uint64_t s, std::uint64_t len)
{
    for (auto it = ranges.begin(); it != ranges.end(); ++it)
        if (it->start == s && it->end == s + len)
        {
            ranges.erase(it);
            return true;
        }
    return false;
}

struct summary
{
    std::uint64_t allocated = 0, other = 0, largest = 0, largest_aligned = 0;
    std::uint32_t count = 0;
};

summary summarize()
{
    std::lock_guard<std::mutex> g(mu);
    summary s;
    for (const range& r : ranges)
    {
        s.allocated += r.end - r.start;
        if (r.owner) s.other += r.end - r.start;
        s.count++;
    }
    for (span g2 : gaps_locked())
    {
        s.largest = std::max(s.largest, g2.second - g2.first);
        const std::uint64_t as = up(g2.first, quantum);
        if (g2.second > as) s.largest_aligned = std::max(s.largest_aligned, g2.second - as);
    }
    return s;
}

// Where top placement puts `len`: the top of the highest free run that holds it (runs under 32 MiB never count).
[[maybe_unused]] std::uint64_t expected_top(std::uint64_t len)
{
    std::lock_guard<std::mutex> g(mu);
    const std::vector<span> gaps = gaps_locked();
    for (auto it = gaps.rbegin(); it != gaps.rend(); ++it)
    {
        const std::uint64_t as = up(it->first, quantum);
        if (it->second > as && it->second - as >= 32 * mib && it->second - as >= len)
            return (it->second - len) / quantum * quantum;
    }
    return none;
}

[[maybe_unused]] std::uint64_t expected_first_fit(std::uint64_t lo, std::uint64_t hi, std::uint64_t len)
{
    std::lock_guard<std::mutex> g(mu);
    return first_fit_locked(lo, hi, len, quantum);
}

[[maybe_unused]] bool take_at(std::uint64_t s, std::uint64_t len)
{
    std::lock_guard<std::mutex> g(mu);
    if (!is_free_locked(s, len)) return false;
    insert_locked(s, len, 1);
    mine.push_back({s, len});
    return true;
}

std::uint64_t take(std::uint64_t len)
{
    std::lock_guard<std::mutex> g(mu);
    const std::uint64_t s = first_fit_locked(0, total, len, quantum);
    if (s == none) return s;
    insert_locked(s, len, 1);
    mine.push_back({s, len});
    return s;
}

// Every free byte inside [lo, hi), except [keep, keep + keep_len).
[[maybe_unused]] void fill(std::uint64_t lo, std::uint64_t hi, std::uint64_t keep = 0, std::uint64_t keep_len = 0)
{
    std::lock_guard<std::mutex> g(mu);
    const auto add = [](std::uint64_t a, std::uint64_t b)
    {
        if (a >= b) return;
        insert_locked(a, b - a, 1);
        mine.push_back({a, b - a});
    };
    for (span g2 : gaps_locked())
    {
        const std::uint64_t a = std::max(g2.first, lo), b = std::min(g2.second, hi);
        if (a >= b) continue;
        if (keep_len && keep < b && a < keep + keep_len)
        {
            add(a, std::min(b, keep));
            add(std::max(a, keep + keep_len), b);
        }
        else
            add(a, b);
    }
}

void give_back(std::uint64_t s, std::uint64_t len)
{
    std::lock_guard<std::mutex> g(mu);
    CHECK(erase_locked(s, len), "the test gave back a range it never took");
    mine.erase(std::remove(mine.begin(), mine.end(), span{s, len}), mine.end());
}

[[maybe_unused]] void give_back_all()
{
    std::lock_guard<std::mutex> g(mu);
    for (span m : mine) erase_locked(m.first, m.second);
    mine.clear();
}
} // namespace pool
} // namespace

extern "C" int orbis_log_enabled(void)
{
    return g_log_set.load() ? 1 : 0;
}

extern "C" void orbis_log(const char* fmt, ...)
{
    if (!g_log_set.load()) return;
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    if (std::strstr(line, "trim returned")) g_trim_lines++;
    if (std::strstr(line, "retrying with a")) g_ladder_lines++;
    if (std::strstr(line, "CAP REACHED")) g_overdraft_lines++;
    if (std::strstr(line, "OVERDRAFT FAILED")) g_overdraft_failed_lines++;
    if (std::strstr(line, "overdraft is unavailable (it failed)")) g_refused_unavailable_lines++;
    if (std::strstr(line, "flexible floor: refused")) g_floor_lines++;
    if (std::strstr(line, "must stay free for the other owners")) g_reserve_lines++;
    if (std::strstr(line, "ESTIMATED from now on")) g_untrusted_lines++;
    if (std::strstr(line, "mem ledger: query"))
    {
        g_probe_lines++;
        std::snprintf(g_probe_line, sizeof(g_probe_line), "%s", line);
    }
    if (std::strstr(line, "mem ledger [")) std::snprintf(g_ledger_line, sizeof(g_ledger_line), "%s", line);
    if (std::strstr(line, "]: small heap ")) std::snprintf(g_small_heap_line, sizeof(g_small_heap_line), "%s", line);
    if (std::strstr(line, "refusal(s) for")) std::snprintf(g_floor_report_line, sizeof(g_floor_report_line), "%s", line);
    if (g_verbose) std::printf("  log: %s\n", line);
}

extern "C" int32_t sceKernelAvailableFlexibleMemorySize(size_t* out)
{
    g_flexible_queries++;
    if (g_flexible_rc.load()) return g_flexible_rc.load();
    *out = static_cast<size_t>(g_flexible_free.load());
    return 0;
}

extern "C" uint64_t sceKernelGetProcessTime()
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(now).count() + g_clock_offset_us.load());
}

extern "C" size_t sceKernelGetDirectMemorySize()
{
    return pool::total;
}

extern "C" int32_t sceKernelAllocateDirectMemory(off_t start, off_t end, size_t len, size_t align, int32_t, off_t* phys)
{
    g_alloc_attempts++;
    if (len > g_fail_above.load()) return static_cast<int32_t>(0x8002000cu);
    std::lock_guard<std::mutex> g(pool::mu);
    const std::uint64_t s = pool::first_fit_locked(std::uint64_t(start), std::uint64_t(end), len, align ? align : pool::quantum);
    if (s == pool::none) return static_cast<int32_t>(0x8002000cu);
    pool::insert_locked(s, len, 0);
    pool::last_start = s;
    pool::last_len = len;
    *phys = off_t(s);
    return 0;
}

// Out-pointer form (orbis_mmap.cpp binds it by asm label): the largest free run inside [start, end), measured from its
// first `align`-aligned address.
extern "C" int32_t sceKernelAvailableDirectMemorySize(off_t start, off_t end, size_t align, off_t* phys, size_t* size)
{
    std::lock_guard<std::mutex> g(pool::mu);
    std::uint64_t best = 0, at = 0;
    for (pool::span g2 : pool::gaps_locked())
    {
        const std::uint64_t a = std::max(g2.first, std::uint64_t(start)), b = std::min(g2.second, std::uint64_t(end));
        const std::uint64_t as = pool::up(a, align);
        if (b > as && b - as > best)
        {
            best = b - as;
            at = as;
        }
    }
    *phys = off_t(at);
    *size = size_t(best);
    return best ? 0 : static_cast<int32_t>(0x8002000cu);
}

#ifndef TEST_NO_DMQ
namespace
{
std::atomic<int> g_dmq_mode{0}; // bits: 1 inclusive end, 2 adjacent ranges merged, 4 every query fails
std::atomic<std::uint64_t> g_dmq_calls{0};
} // namespace

// FIND_NEXT (flag 1): the allocated range holding `offset`, or the next one above it.
extern "C" int32_t sceKernelDirectMemoryQuery(off_t offset, int32_t flags, void* info, size_t size)
{
    g_dmq_calls++;
    const int mode = g_dmq_mode.load();
    if (mode & 4) return static_cast<int32_t>(0x80020002u);
    CHECK(size == 24, "info size %zu", size);
    std::lock_guard<std::mutex> g(pool::mu);
    const std::uint64_t off = std::uint64_t(offset);
    for (std::size_t i = 0; i < pool::ranges.size(); i++)
    {
        const pool::range& r = pool::ranges[i];
        if (r.end <= off) continue;
        if (r.start > off && !(flags & 1)) break;
        std::uint64_t e = r.end;
        if (mode & 2)
            for (std::size_t k = i + 1; k < pool::ranges.size() && pool::ranges[k].start == e; k++) e = pool::ranges[k].end;
        const struct
        {
            std::int64_t start, end;
            std::int32_t type, pad;
        } out{std::int64_t(r.start), std::int64_t((mode & 1) ? e - 1 : e), 3, 0};
        std::memcpy(info, &out, 24);
        return 0;
    }
    return static_cast<int32_t>(0x8002000du);
}
#endif

extern "C" int32_t sceKernelMapDirectMemory(void** va, size_t len, int32_t, int32_t, off_t, size_t)
{
    // Reserve + commit; a later sceKernelMunmap only decommits, so the address range is never handed out again.
    void* p = VirtualAlloc(nullptr, len, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p) return static_cast<int32_t>(0x8002000cu);
    const std::uint32_t i = g_region_count.load(std::memory_order_relaxed);
    if (i >= max_regions)
    {
        VirtualFree(p, 0, MEM_RELEASE);
        return static_cast<int32_t>(0x8002000cu);
    }
    g_regions[i].len.store(len);
    g_regions[i].va.store(reinterpret_cast<std::uintptr_t>(p));
    g_region_count.store(i + 1, std::memory_order_release);
    *va = p;
    return 0;
}

extern "C" int32_t sceKernelMunmap(void* va, size_t len)
{
    const std::uint64_t call = ++g_munmap_calls;
    // By now no lock-free reader may be able to match the range any more.
    orbis::MmapDirectSpan span{};
    CHECK(!orbis::mmapDirectDescribe(va, span), "carve-out %p still visible to readers when it is unmapped", va);
    CHECK(!orbis::mmapDirectDescribe(static_cast<char*>(va) + len - 1, span), "last byte of %p still visible", va);
    if (g_inject && call % 13 == 0)
    {
        g_munmap_failed++;
        return static_cast<int32_t>(0x80020016u);
    }
    region* r = find_region(va);
    CHECK(r && r->len.load() == len, "unmap of an unknown range %p/%zu", va, len);
    if (r)
    {
        CHECK(r->unmap_seq.load() == 0, "range %p unmapped twice", va);
        r->unmap_seq.store(g_seq.fetch_add(1) + 1);
    }
    VirtualFree(va, len, MEM_DECOMMIT);
    return 0;
}

extern "C" int32_t sceKernelReleaseDirectMemory(off_t phys, size_t len)
{
    const std::uint64_t call = ++g_release_calls;
    // A failed release keeps the pages allocated (the heap keeps counting them as held); bounded so the race cannot
    // fill the pool with them.
    if (g_inject && call % 17 == 0 && g_release_failed_bytes.load() < 256 * mib)
    {
        g_release_failed_bytes += len;
        return static_cast<int32_t>(0x80020016u);
    }
    std::lock_guard<std::mutex> g(pool::mu);
    const bool ok = pool::erase_locked(std::uint64_t(phys), len);
    CHECK(ok, "release of a range that was never allocated: 0x%llx + %zu", static_cast<unsigned long long>(phys), len);
    return ok ? 0 : static_cast<int32_t>(0x80020016u);
}

extern "C" int32_t sceKernelUsleep(uint32_t)
{
    SwitchToThread();
    return 0;
}

// Flexible-memory fallback of the real mmap.
extern "C" void* mmap(void*, size_t len, int, int, int, off_t)
{
    g_flexible_maps++;
    void* p = VirtualAlloc(nullptr, len, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    return p ? p : reinterpret_cast<void*>(-1);
}

extern "C" int munmap(void* p, size_t)
{
    return VirtualFree(p, 0, MEM_RELEASE) ? 0 : -1;
}

extern "C" int madvise(void*, size_t, int)
{
    return 0;
}

namespace
{
struct block
{
    unsigned char* p;
    std::size_t len;
};

void* heap_map(std::size_t len)
{
    return __mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
}

[[maybe_unused]] bool mapped(const void* p)
{
    return p != nullptr && p != MAP_FAILED;
}

orbis::MmapDirectStats stats()
{
    orbis::MmapDirectStats s{};
    orbis::mmapDirectStats(&s);
    return s;
}

// TRIM compiled in and no flag file; segregation follows it.
bool trim_active()
{
    return stats().trim_enabled != 0;
}

[[maybe_unused]] bool seg_active()
{
    return ORBIS_MMAP_SEGREGATE && (!ORBIS_MMAP_TRIM || trim_active());
}

// A sample taken now. An estimated ledger is told the bytes the other owners hold (the test's ranges), as the
// budget authority does, so its free total is exact.
orbis::DirectLedger fresh_ledger()
{
    orbis::DirectLedger l{};
    orbis::directLedger(&l, 0);
    if (l.estimated)
    {
        orbis::directLedgerSetExternal(pool::summarize().other);
        orbis::directLedger(&l, 0);
    }
    return l;
}

void allocator(unsigned seed, const std::atomic<bool>& stop, std::atomic<std::uint64_t>& maps)
{
    static constexpr std::size_t sizes[] = {240 * 1024, 1 * mib, 3 * mib + 16384, 8 * mib, 24 * mib, 40 * mib};
    std::mt19937 rng(seed);
    std::vector<block> live;
    std::size_t live_bytes = 0;
    const auto release = [&](std::size_t i)
    {
        block b = live[i];
        CHECK(b.p[0] == 0xa5 && b.p[b.len - 1] == 0x5a, "block %p lost its contents", static_cast<void*>(b.p));
        CHECK(__munmap(b.p, b.len) == 0, "munmap of a live block %p failed", static_cast<void*>(b.p));
        live_bytes -= b.len;
        live[i] = live.back();
        live.pop_back();
    };

    while (!stop.load(std::memory_order_relaxed))
    {
        const unsigned op = rng() % 100;
        if (op < 55)
        {
            const std::size_t len = sizes[rng() % (sizeof(sizes) / sizeof(sizes[0]))];
            if (live_bytes + len > 160 * mib) continue;
            auto* p = static_cast<unsigned char*>(heap_map(len));
            CHECK(p && p != reinterpret_cast<unsigned char*>(-1), "map of %zu bytes failed", len);
            if (!p || p == reinterpret_cast<unsigned char*>(-1)) continue;
            // musl's calloc relies on MAP_ANON memory arriving zeroed
            CHECK(p[0] == 0 && p[len / 2] == 0 && p[len - 1] == 0, "block %p not zeroed", static_cast<void*>(p));
            p[0] = 0xa5;
            p[len - 1] = 0x5a;
            live.push_back({p, len});
            live_bytes += len;
            maps++;
        }
        else if (op < 97 && !live.empty())
        {
            release(rng() % live.size());
        }
        else
        {
            while (!live.empty()) release(live.size() - 1);
        }
    }
    while (!live.empty()) release(live.size() - 1);
}

void reader(unsigned seed, const std::atomic<bool>& stop, std::atomic<std::uint64_t>& hits, std::atomic<std::uint64_t>& misses)
{
    std::mt19937_64 rng(seed);
    while (!stop.load(std::memory_order_relaxed))
    {
        const std::uint32_t n = g_region_count.load(std::memory_order_acquire);
        if (!n) continue;
        const region& r = g_regions[rng() % n];
        const std::uint64_t len = r.len.load();
        const std::uintptr_t va = r.va.load();
        if (!len || !va) continue;
        const void* addr = reinterpret_cast<const void*>(va + (rng() % len));
        const std::uint64_t started = g_seq.load();
        orbis::MmapDirectSpan span{};
        if (!orbis::mmapDirectDescribe(addr, span))
        {
            misses++;
            continue;
        }
        hits++;
        const region* found = find_region(span.base);
        CHECK(found != nullptr, "reader matched %p in a carve-out base %p that was never mapped", addr, span.base);
        if (!found) continue;
        CHECK(found->len.load() == span.size, "torn carve-out: base %p with size %llu, mapped with %llu", span.base,
            static_cast<unsigned long long>(span.size), static_cast<unsigned long long>(found->len.load()));
        CHECK(addr >= span.base && static_cast<const char*>(addr) < static_cast<const char*>(span.base) + span.size,
            "address %p outside the matched carve-out", addr);
        const std::uint64_t unmapped = found->unmap_seq.load();
        CHECK(unmapped == 0 || unmapped > started, "reader matched %p in a carve-out unmapped before it started", addr);
    }
}

void race_phase()
{
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> maps{0}, hits{0}, misses{0}, trims{0}, trimmed{0};
    g_inject = true;
    std::vector<std::thread> threads;
    threads.emplace_back(allocator, 11u, std::cref(stop), std::ref(maps));
    threads.emplace_back(allocator, 23u, std::cref(stop), std::ref(maps));
    threads.emplace_back(reader, 31u, std::cref(stop), std::ref(hits), std::ref(misses));
    threads.emplace_back(reader, 47u, std::cref(stop), std::ref(hits), std::ref(misses));
    threads.emplace_back([&]
    {
        while (!stop.load(std::memory_order_relaxed))
        {
            trimmed += orbis::mmapDirectTrim(1);
            trims++;
            std::this_thread::sleep_for(std::chrono::microseconds(300));
        }
    });

    std::this_thread::sleep_for(std::chrono::seconds(3));
    stop = true;
    for (auto& t : threads) t.join();
    g_inject = false;

    std::printf("race: %llu maps, %llu trim calls returning %llu MiB, %llu carve-outs mapped, %llu munmaps (%llu failed on purpose), "
        "reader hits %llu misses %llu\n",
        static_cast<unsigned long long>(maps.load()), static_cast<unsigned long long>(trims.load()),
        static_cast<unsigned long long>(trimmed.load() / mib), static_cast<unsigned long long>(g_region_count.load()),
        static_cast<unsigned long long>(g_munmap_calls.load()), static_cast<unsigned long long>(g_munmap_failed.load()),
        static_cast<unsigned long long>(hits.load()), static_cast<unsigned long long>(misses.load()));
    CHECK(maps > 1000, "too few maps (%llu) for a meaningful race", static_cast<unsigned long long>(maps.load()));
    CHECK(hits > 1000, "readers never matched a live carve-out");
    if (trim_active())
    {
        CHECK(trimmed > 0 && g_munmap_calls > 10, "trim never returned anything during the race");
        CHECK(misses > 0, "readers never looked at a trimmed carve-out");
    }
    else
        CHECK(trimmed == 0 && g_munmap_calls == 0, "trim is supposed to be off in this build");

    const orbis::MmapDirectStats s = stats();
    CHECK(s.live == 0, "live bytes %llu after every block was unmapped", static_cast<unsigned long long>(s.live));
    CHECK(s.peak > 0 && s.held >= s.live, "inconsistent stats");
}

void drain_phase()
{
    const std::uint64_t released = orbis::mmapDirectTrim(0);
    const orbis::MmapDirectStats s = stats();
    if (trim_active())
    {
        // Everything is free: only pages whose release failed on purpose stay counted as held.
        CHECK(s.carves == 0, "%u carve-out(s) left after trim(0)", s.carves);
        CHECK(s.held == g_release_failed_bytes.load(), "held %llu MiB, expected %llu MiB (release failures)",
            static_cast<unsigned long long>(s.held / mib), static_cast<unsigned long long>(g_release_failed_bytes.load() / mib));
        CHECK(pool::summarize().allocated == s.held, "the pool holds %llu MiB for the heap, the heap counts %llu MiB",
            static_cast<unsigned long long>(pool::summarize().allocated / mib), static_cast<unsigned long long>(s.held / mib));
        std::printf("drain: trim(0) returned %llu MiB; held %llu MiB of which %llu MiB failed to release on purpose\n",
            static_cast<unsigned long long>(released / mib), static_cast<unsigned long long>(s.held / mib),
            static_cast<unsigned long long>(g_release_failed_bytes.load() / mib));
    }
    else
    {
        CHECK(released == 0 && s.carves > 0 && s.held == s.carves * 128 * mib, "this build must keep every carve-out");
        std::printf("drain: trim off, %u carve-out(s) of 128 MiB kept as before\n", s.carves);
    }
}

// The ledger against the pool: a walk gives the exact figures, an estimate gives the largest run or, once told the
// other owners' total, the pool less the boot figure, the heap and them.
void ledger_phase()
{
    CHECK(!orbis::directLedger(nullptr, 0), "a null output must return false");
    orbis::DirectLedger l{};
    CHECK(orbis::directLedger(&l, 0), "directLedger returned false");
    CHECK(g_probe_lines == 1, "%llu probe lines", static_cast<unsigned long long>(g_probe_lines.load()));
#if defined(TEST_NO_DMQ)
    CHECK(std::strstr(g_probe_line, "ESTIMATED: sceKernelDirectMemoryQuery is not exported"), "probe: %s", g_probe_line);
#elif defined(TEST_FLAG_FILES)
    CHECK(std::strstr(g_probe_line, "ESTIMATED: off by"), "probe: %s", g_probe_line);
#else
    CHECK(std::strstr(g_probe_line, "- ledger ON (kernel walk)"), "probe: %s", g_probe_line);
#endif
    // With the top band the first carve-out of the race probed the ledger while no log sink was set.
    CHECK(!ORBIS_MMAP_HEAP_BAND_MIB == !std::strstr(g_probe_line, "(made before the log was set)"), "probe: %s", g_probe_line);
    const pool::summary m = pool::summarize();
    const orbis::MmapDirectStats s = stats();
    CHECK(l.total == pool::total && l.heap_held == s.held && l.reserve == 0 && l.estimated == (walk_expected ? 0u : 1u),
        "ledger: total %llu held %llu reserve %llu estimated %u", static_cast<unsigned long long>(l.total),
        static_cast<unsigned long long>(l.heap_held), static_cast<unsigned long long>(l.reserve), l.estimated);
    if (walk_expected)
        CHECK(l.allocated == m.allocated && l.free_total == pool::total - m.allocated && l.largest_run == m.largest &&
            l.entries == m.count && (l.rc == 0 || l.rc == static_cast<int>(0x8002000du)),
            "walk: allocated %llu/%llu, largest %llu/%llu, entries %u/%u, rc 0x%08x",
            static_cast<unsigned long long>(l.allocated), static_cast<unsigned long long>(m.allocated),
            static_cast<unsigned long long>(l.largest_run), static_cast<unsigned long long>(m.largest), l.entries, m.count,
            static_cast<unsigned>(l.rc));
    else
        CHECK(l.entries == 0 && l.walk_us == 0 && l.largest_run == m.largest_aligned && l.free_total == l.largest_run,
            "estimate: entries %u, largest %llu/%llu, free %llu", l.entries, static_cast<unsigned long long>(l.largest_run),
            static_cast<unsigned long long>(m.largest_aligned), static_cast<unsigned long long>(l.free_total));

    // A sample is reused for max_age_ms while the heap changed nothing; another owner's allocation shows at the next walk.
    const std::uint64_t taken = pool::take(300 * mib);
    CHECK(taken != pool::none, "no 300 MiB run in the pool");
#ifndef TEST_NO_DMQ
    const std::uint64_t queries = g_dmq_calls.load();
#endif
    orbis::DirectLedger c{};
    orbis::directLedger(&c, 250);
    CHECK(c.sampled_us == l.sampled_us && c.allocated == l.allocated, "a fresh sample was not reused");
#ifndef TEST_NO_DMQ
    CHECK(g_dmq_calls == queries, "a reused sample walked the pool");
#endif
    const pool::summary m2 = pool::summarize();
    if (walk_expected)
    {
        orbis::directLedger(&c, 0);
        CHECK(c.allocated == l.allocated + 300 * mib && c.free_total == l.free_total - 300 * mib && c.entries == m2.count,
            "after another owner took 300 MiB: allocated %llu, free %llu", static_cast<unsigned long long>(c.allocated),
            static_cast<unsigned long long>(c.free_total));
#ifndef TEST_NO_DMQ
        // A kernel that reports inclusive ends, or merges adjacent ranges, gives the same figures.
        orbis::DirectLedger d{};
        g_dmq_mode = 1;
        orbis::directLedger(&d, 0);
        CHECK(d.allocated == c.allocated && d.largest_run == c.largest_run && d.estimated == 0, "inclusive ends: %llu",
            static_cast<unsigned long long>(d.allocated));
        g_dmq_mode = 2;
        orbis::directLedger(&d, 0);
        CHECK(d.allocated == c.allocated && d.largest_run == c.largest_run && d.entries <= c.entries && d.estimated == 0,
            "merged ranges: %llu in %u", static_cast<unsigned long long>(d.allocated), d.entries);
        g_dmq_mode = 0;
#endif
    }
    else
    {
        orbis::directLedger(&c, 0);
        CHECK(c.free_total == c.largest_run && c.largest_run == m2.largest_aligned, "estimate without the owners: %llu",
            static_cast<unsigned long long>(c.free_total));
        orbis::directLedgerSetExternal(m2.other);
        orbis::directLedger(&c, 0);
        const std::uint64_t used = c.boot_allocated + c.heap_held + m2.other;
        const std::uint64_t expect = std::max(used < pool::total ? pool::total - used : 0, c.largest_run);
        CHECK(c.free_total == expect && c.free_total == pool::total - m2.allocated && c.allocated == m2.allocated,
            "estimate with the owners: free %llu, expected %llu (pool %llu), boot %llu",
            static_cast<unsigned long long>(c.free_total), static_cast<unsigned long long>(expect),
            static_cast<unsigned long long>(pool::total - m2.allocated), static_cast<unsigned long long>(c.boot_allocated));
    }
    pool::give_back(taken, 300 * mib);
    if (!walk_expected) orbis::directLedgerSetExternal(0);

    orbis::directLedgerReport("ledger");
    char expected[96];
    std::snprintf(expected, sizeof(expected), "mem ledger [ledger]: direct %llu KiB", static_cast<unsigned long long>(pool::total / 1024));
    CHECK(std::strstr(g_ledger_line, expected) && std::strstr(g_ledger_line, walk_expected ? "kernel walk" : "ESTIMATED"),
        "report: %s", g_ledger_line);
    std::printf("ledger: %s, %u range(s), free %llu MiB, largest run %llu MiB, cached sample reused: ok\n",
        walk_expected ? "kernel walk (inclusive ends and merged ranges agree)" : "estimated (largest run, then the owners' total)",
        l.entries, static_cast<unsigned long long>(l.free_total / mib), static_cast<unsigned long long>(l.largest_run / mib));
}

#if ORBIS_MMAP_CARVE_LADDER && ORBIS_MMAP_TRIM
void ladder_and_cap_phase()
{
    const std::uint64_t base_held = stats().held; // pages that failed to release earlier

    // 1. Fragmented physical memory: 128 and 64 MiB refused, 32 MiB granted.
    g_fail_above = 32 * mib;
    const std::uint64_t ladder_before = g_ladder_lines;
    void* small = heap_map(240 * 1024);
    orbis::MmapDirectStats s = stats();
    CHECK(small && small != reinterpret_cast<void*>(-1), "small map failed");
    CHECK(s.held == base_held + 32 * mib && s.carves == 1, "ladder: held %llu MiB, %u carve-out(s)",
        static_cast<unsigned long long>(s.held / mib), s.carves);
    CHECK(g_ladder_lines - ladder_before == 2, "ladder should halve twice (128 -> 64 -> 32)");

    // 2. The ladder never goes below the request: 200 MiB cannot be split, so it falls back to flexible memory.
    const std::uint64_t flexible_before = g_flexible_maps;
    void* big = heap_map(200 * mib);
    CHECK(big && big != reinterpret_cast<void*>(-1), "fallback map failed");
    CHECK(g_flexible_maps == flexible_before + 1, "a 200 MiB request must fall back, not get a shorter carve-out");
    CHECK(stats().fallback_bytes >= 200 * mib, "fallback bytes not counted");
    __munmap(big, 200 * mib);
    g_fail_above = ~0ull;

    // 3. Room under the cap: a new carve-out shrinks to fit (64 MiB instead of 128). 32 MiB does not fit the first
    //    carve-out's free block and, with SEGREGATE, is not above the size that gets a carve-out of its own.
    orbis::mmapDirectSetCap(stats().held + 64 * mib);
    void* fit = heap_map(32 * mib);
    s = stats();
    const std::uint64_t held_fit = s.held;
    CHECK(fit && fit != reinterpret_cast<void*>(-1), "fit map failed");
    CHECK(s.held == base_held + 96 * mib && s.critical == 0, "fit: held %llu MiB critical %u",
        static_cast<unsigned long long>(s.held / mib), s.critical);

    // 4. No room under the cap: one overdraft carve-out (<= 128 MiB) past it, only while the pool keeps 256 MiB free
    //    for the other owners (no reservation set). The other owners take every free byte but one 366 MiB run.
    constexpr std::uint64_t keep_free = 256 * mib, run = 366 * mib;
    const std::uint64_t at = pool::expected_top(run);
    CHECK(at != pool::none, "no %llu MiB run left", static_cast<unsigned long long>(run / mib));
    pool::fill(0, pool::total, at, run);
    CHECK(pool::take_at(at, 46 * mib), "could not take the start of the kept run");
    orbis::DirectLedger l = fresh_ledger();
    CHECK(l.free_total == run - 46 * mib, "free %llu MiB, expected %llu", static_cast<unsigned long long>(l.free_total / mib),
        static_cast<unsigned long long>((run - 46 * mib) / mib));
    // 320 MiB free: a 100 MiB overdraft would leave 220. Refused, not spent.
    const std::uint64_t flexible_denied = g_flexible_maps, refusals_denied = stats().reserve_refusals;
    void* denied = heap_map(100 * mib);
    s = stats();
    CHECK(mapped(denied) && g_flexible_maps == flexible_denied + 1 && s.held == held_fit && s.critical == 1 &&
        s.reserve_refusals == refusals_denied + 1 && g_overdraft_lines == 0,
        "overdraft into the last 256 MiB: flexible %llu, held %llu MiB, refusals %llu, overdraft lines %llu",
        static_cast<unsigned long long>(g_flexible_maps - flexible_denied), static_cast<unsigned long long>(s.held / mib),
        static_cast<unsigned long long>(s.reserve_refusals - refusals_denied), static_cast<unsigned long long>(g_overdraft_lines.load()));
    __munmap(denied, 100 * mib);
    // 366 MiB free: a 128 MiB overdraft would leave 238, so it shrinks to 110 (with SEGREGATE the mapping's own
    // 100 MiB carve-out leaves 266).
    pool::give_back(at, 46 * mib);
    l = fresh_ledger();
    void* over = heap_map(100 * mib);
    const std::uint64_t over_len = seg_active() ? 100 * mib : run - keep_free;
    s = stats();
    CHECK(over && over != reinterpret_cast<void*>(-1), "overdraft map failed");
    CHECK(s.held == held_fit + over_len && s.critical == 1 && g_overdraft_lines == 1,
        "overdraft: held %llu MiB (expected %llu) critical %u lines %llu", static_cast<unsigned long long>(s.held / mib),
        static_cast<unsigned long long>((held_fit + over_len) / mib), s.critical, static_cast<unsigned long long>(g_overdraft_lines.load()));
    CHECK(pool::summarize().largest >= keep_free, "the overdraft left %llu MiB free",
        static_cast<unsigned long long>(pool::summarize().largest / mib));

    // ...and only once: the next carve-out is refused and the request goes to flexible memory.
    const std::uint64_t flexible_before2 = g_flexible_maps;
    void* refused = heap_map(100 * mib);
    CHECK(g_flexible_maps == flexible_before2 + 1, "second overdraft must be refused");
    __munmap(refused, 100 * mib);

    __munmap(over, 100 * mib);
    __munmap(fit, 32 * mib);
    __munmap(small, 240 * 1024);
    pool::give_back_all();
    orbis::mmapDirectSetCap(0);
    orbis::mmapDirectTrim(0);
    s = stats();
    CHECK(s.carves == 0 && s.live == 0 && s.held == base_held, "after the cap phase: %u carve-outs, held %llu MiB", s.carves,
        static_cast<unsigned long long>(s.held / mib));
    std::printf("ladder/cap: 128->64->32 MiB ladder, no split below the request, fit-under-cap, overdraft refused into the "
        "last 256 MiB, then one overdraft of %llu MiB, then refusal: ok\n", static_cast<unsigned long long>(over_len / mib));
}
#endif

void default_cap_phase()
{
    // Refused at the (run-time lowered) cap. Without the ladder the heap never reports critical; with it (and no
    // trim, the first RPCS3 configuration) a refusal at the cap marks it critical. 130 MiB is too long for the
    // overdraft, so the ladder build refuses it too.
    orbis::mmapDirectSetCap(1);
    const std::uint64_t flexible_before = g_flexible_maps;
    void* p = heap_map(130 * mib);
    CHECK(p && p != reinterpret_cast<void*>(-1), "fallback map failed");
    CHECK(g_flexible_maps == flexible_before + 1, "a carve-out past the cap must be refused");
    CHECK(stats().critical == (ORBIS_MMAP_CARVE_LADDER ? 1u : 0u), "critical is %u", stats().critical);
    __munmap(p, 130 * mib);
    orbis::mmapDirectSetCap(0);
    std::printf("cap: refused past the cap, fell back to flexible memory, critical %u: ok\n", stats().critical);
}

#if ORBIS_MMAP_CARVE_LADDER
// Physical direct memory short at the cap: the overdraft fails at 128, 64 and 32 MiB once and is then not retried
// (each retry was three failing ladder sizes under the allocator lock). With the top band each size is tried at the
// top of the highest free run (a walked ledger only), in the band, then anywhere.
void overdraft_latch_phase()
{
    const orbis::MmapDirectStats s = stats();
    CHECK(s.live == 0 && s.carves > 0, "needs free carve-outs: live %llu, %u carve-out(s)",
        static_cast<unsigned long long>(s.live), s.carves);
    // Every carve-out is one free 128 MiB block since the race; fill them so the next request needs a new one.
    std::vector<void*> full;
    for (unsigned i = 0; i < s.carves; i++)
    {
        void* p = heap_map(128 * mib);
        CHECK(p && p != MAP_FAILED, "fill %u failed", i);
        full.push_back(p);
    }
    CHECK(stats().held == s.held && stats().live == s.carves * 128 * mib, "filling took a new carve-out");

    const std::uint64_t per_size = !ORBIS_MMAP_HEAP_BAND_MIB ? 1 : walk_expected ? 3 : 2;
    orbis::mmapDirectSetCap(1);
    g_fail_above = 0;
    const std::uint64_t attempts = g_alloc_attempts, flexible = g_flexible_maps;
    void* first = heap_map(240 * 1024);
    CHECK(first && first != MAP_FAILED, "first fallback failed");
    CHECK(g_alloc_attempts == attempts + 3 * per_size, "the overdraft should try 128, 64 and 32 MiB once (%llu attempts)",
        static_cast<unsigned long long>(g_alloc_attempts - attempts));
    CHECK(g_overdraft_failed_lines == 1 && g_flexible_maps == flexible + 1 && stats().critical == 1,
        "failed overdraft: %llu lines, %llu flexible maps, critical %u",
        static_cast<unsigned long long>(g_overdraft_failed_lines.load()),
        static_cast<unsigned long long>(g_flexible_maps - flexible), stats().critical);
    const std::uint64_t unavailable = g_refused_unavailable_lines;
    void* second = heap_map(240 * 1024);
    CHECK(second && second != MAP_FAILED, "second fallback failed");
    CHECK(g_alloc_attempts == attempts + 3 * per_size, "a failed overdraft was retried (%llu attempts)",
        static_cast<unsigned long long>(g_alloc_attempts - attempts));
    CHECK(g_overdraft_failed_lines == 1 && g_refused_unavailable_lines == unavailable + 1 && g_flexible_maps == flexible + 2,
        "latched overdraft: %llu failed lines, %llu refusal lines", static_cast<unsigned long long>(g_overdraft_failed_lines.load()),
        static_cast<unsigned long long>(g_refused_unavailable_lines - unavailable));

    __munmap(first, 240 * 1024);
    __munmap(second, 240 * 1024);
    for (void* p : full) __munmap(p, 128 * mib);
    g_fail_above = ~0ull;
    orbis::mmapDirectSetCap(0);
    CHECK(stats().live == 0, "live %llu after the latch phase", static_cast<unsigned long long>(stats().live));
    std::printf("overdraft latch: failed once at 128/64/32 MiB, then refused without retrying: ok\n");
}
#endif

#if ORBIS_MMAP_FLEX_FLOOR_MIB
// A request the carve-outs cannot serve falls back to flexible memory only while that leaves the floor free.
void flex_floor_phase()
{
    constexpr std::uint64_t floor = std::uint64_t(ORBIS_MMAP_FLEX_FLOOR_MIB) * mib;
    constexpr std::uint64_t want = 200 * mib; // longer than any carve-out and than the overdraft
    orbis::mmapDirectSetCap(1);
    const std::uint64_t flexible = g_flexible_maps, fallback = stats().fallback_bytes, lines = g_floor_lines;

    g_flexible_free = floor + want - 1;
    errno = 0;
    void* refused = heap_map(want);
    CHECK(refused == MAP_FAILED && errno == ENOMEM, "below the floor: %p errno %d", refused, errno);
    CHECK(g_flexible_maps == flexible && stats().fallback_bytes == fallback, "a refused request reached flexible memory");
    CHECK(stats().critical == 1 && g_floor_lines == lines + 1, "critical %u, %llu floor lines", stats().critical,
        static_cast<unsigned long long>(g_floor_lines - lines));
    void* again = heap_map(want);
    CHECK(again == MAP_FAILED && g_floor_lines == lines + 1, "second refusal: %p, %llu floor lines", again,
        static_cast<unsigned long long>(g_floor_lines - lines));

    g_flexible_free = floor + want; // leaves exactly the floor
    void* fits = heap_map(want);
    CHECK(fits && fits != MAP_FAILED && g_flexible_maps == flexible + 1 && stats().fallback_bytes == fallback + want,
        "a fallback that leaves the floor must be served");
    __munmap(fits, want);

    // A failed query refuses nothing.
    g_flexible_free = 0;
    g_flexible_rc = static_cast<std::int32_t>(0x80020016u);
    void* unknown = heap_map(want);
    CHECK(unknown && unknown != MAP_FAILED && g_flexible_maps == flexible + 2, "failed query refused the request");
    __munmap(unknown, want);
    g_flexible_rc = 0;
    g_flexible_free = 1ull << 30;
    orbis::mmapDirectSetCap(0);

    orbis::mmapDirectReport("floor");
    char expected[128];
    std::snprintf(expected, sizeof(expected), "flexible floor %llu KiB (2 refusal(s) for %llu KiB)",
        static_cast<unsigned long long>(floor / 1024), static_cast<unsigned long long>(2 * want / 1024));
    CHECK(std::strstr(g_floor_report_line, expected), "report '%s' lacks '%s'", g_floor_report_line, expected);
    std::printf("flexible floor: refused below %llu MiB (ENOMEM, logged once, critical), served at the floor, "
        "failed query ignored: ok\n", static_cast<unsigned long long>(floor / mib));
}
#endif

#ifdef ORBIS_MMAP_EXPAND_STEP_MAX_MIB
constexpr std::uint64_t expand_cap = std::uint64_t(ORBIS_MMAP_EXPAND_STEP_MAX_MIB) * mib;

// musl's minimum for a step (16 KiB << (step/2), the shift count taken mod 64), capped when the knob is not 0.
std::uint64_t expected_step(std::uint32_t step)
{
    const std::uint32_t half = step / 2;
    if (!expand_cap) return 0x4000ull << (half & 63);
    return half >= 40 ? expand_cap : std::min<std::uint64_t>(0x4000ull << half, expand_cap);
}

// musl's __expand_heap contract: ENOMEM past SIZE_MAX/2 - 16 KiB, length rounded to 16 KiB and raised to the step
// minimum, *pn and the step counter updated only on success, memory zeroed and inside a carve-out.
void expand_heap_phase()
{
    const std::uint32_t steps = expand_cap ? 40 : 26; // ~400 MiB capped, ~470 MiB with musl's doubling
    const std::uint64_t live_before = stats().live;
    CHECK(stats().small_heap == 0, "small heap %llu before any growth", static_cast<unsigned long long>(stats().small_heap));

    size_t huge = 0x7fffffffffffc000ull;
    errno = 0;
    CHECK(!__expand_heap(&huge) && errno == ENOMEM && huge == 0x7fffffffffffc000ull, "overflow guard: errno %d", errno);

    std::vector<block> heap;
    std::uint64_t total = 0;
    bool adjacent = false;
    for (std::uint32_t step = 0; step < steps; step++)
    {
        // musl asks for one chunk plus overhead; every fifth step asks for more than the minimum.
        const size_t ask = step % 5 == 4 ? size_t(20 * mib + 1) : size_t(step * 48 + 1);
        const std::uint64_t rounded = (std::uint64_t(ask) + 0x3fff) & ~0x3fffull;
        const std::uint64_t expected = std::max(rounded, expected_step(step));
        size_t n = ask;
        auto* p = static_cast<unsigned char*>(__expand_heap(&n));
        CHECK(p != nullptr, "step %u failed", step);
        if (!p) break;
        CHECK(n == expected, "step %u: *pn %zu, expected %llu", step, n, static_cast<unsigned long long>(expected));
        CHECK(p[0] == 0 && p[n / 2] == 0 && p[n - 1] == 0, "step %u not zeroed", step);
        orbis::MmapDirectSpan span{};
        const bool described = orbis::mmapDirectDescribe(p, span);
        CHECK(described && span.blockUsed && span.blockSize == n && span.offset == span.blockOff,
            "step %u is not one used carve-out block of its own", step);
        if (expand_cap) CHECK(span.size <= 128 * mib, "capped step %u needed a %llu MiB carve-out", step,
            static_cast<unsigned long long>(span.size / mib));
        if (!heap.empty() && heap.back().p + heap.back().len == p) adjacent = true;
        heap.push_back({p, n});
        total += n;
        CHECK(stats().small_heap == total, "small heap %llu, expected %llu", static_cast<unsigned long long>(stats().small_heap),
            static_cast<unsigned long long>(total));
    }
    CHECK(adjacent, "no step continued the previous one (malloc merges those)");
    if (expand_cap) CHECK(expected_step(steps - 1) == expand_cap, "the capped steps were not reached");
    else CHECK(heap.size() == steps && heap.back().len == 64 * mib, "musl's step %u is not 64 MiB", steps - 1);
    if (seg_active()) CHECK(stats().held_small > 0, "small-heap steps outside small-heap carve-outs");

    // A failed mapping (past the cap, longer than any carve-out, refused by flexible memory too) leaves *pn and
    // the step counter alone.
    orbis::mmapDirectSetCap(1);
    size_t failing = size_t(1) << 40;
    CHECK(!__expand_heap(&failing) && failing == (size_t(1) << 40) && stats().small_heap == total, "failed step changed state");
    orbis::mmapDirectSetCap(0);
    size_t next = 1;
    auto* p = static_cast<unsigned char*>(__expand_heap(&next));
    CHECK(p && next == expected_step(steps), "after a failure: *pn %zu, expected %llu", next,
        static_cast<unsigned long long>(expected_step(steps)));
    if (p)
    {
        heap.push_back({p, next});
        total += next;
    }

    orbis::mmapDirectReport("expand");
    char expected[160];
    std::snprintf(expected, sizeof(expected), "small heap %llu KiB in %u step(s), step cap %llu KiB",
        static_cast<unsigned long long>(total / 1024), steps + 1, static_cast<unsigned long long>(expand_cap / 1024));
    CHECK(std::strstr(g_small_heap_line, expected), "report '%s' lacks '%s'", g_small_heap_line, expected);

    for (const auto& b : heap) CHECK(__munmap(b.p, b.len) == 0, "unmap of small-heap block %p failed", static_cast<void*>(b.p));
    CHECK(stats().live == live_before, "live %llu after the expand phase, %llu before",
        static_cast<unsigned long long>(stats().live), static_cast<unsigned long long>(live_before));
    std::printf("expand heap: %u steps, %llu MiB, %s, overflow guard and failure path: ok\n", steps + 1,
        static_cast<unsigned long long>(total / mib), expand_cap ? "capped" : "musl's doubling");
}
#endif

// The pressure hook of the test: counts, may free a block (the retry then uses it) and may map memory itself.
struct hook_probe
{
    std::atomic<int> calls{0};
    std::atomic<std::uint64_t> bytes{0};
    std::atomic<unsigned> why{0};
    void* victim = nullptr;
    std::size_t victim_len = 0;
    std::size_t nested_len = 0;
    void* nested = nullptr;
} g_hook;

void test_hook(unsigned long long bytes, unsigned why)
{
    g_hook.calls++;
    g_hook.bytes = bytes;
    g_hook.why = why;
    if (g_hook.victim)
    {
        CHECK(__munmap(g_hook.victim, g_hook.victim_len) == 0, "the hook could not free its block");
        g_hook.victim = nullptr;
    }
    if (g_hook.nested_len)
    {
        const std::size_t n = g_hook.nested_len;
        g_hook.nested_len = 0;
        g_hook.nested = heap_map(n);
    }
}

// The reservation, the live fallback bytes and the pressure hook.
void reserve_phase()
{
    constexpr std::uint64_t big = 136 * mib; // longer than a 128 MiB carve-out: always a carve-out of its own
    orbis::mmapDirectTrim(0);                // no entirely free carve-out left for a refusal to trim
    orbis::mmapDirectSetCap(0);
    orbis::mmapDirectClearCritical();
    CHECK(stats().critical == 0, "critical after mmapDirectClearCritical");
    g_clock_offset_us += 61000000; // past the minute of any earlier reservation line
    const orbis::MmapDirectStats s0 = stats();
    const std::uint64_t lines0 = g_reserve_lines, flexible0 = g_flexible_maps;

    // 1. A carve-out that would cut into the reservation is refused; the request goes to flexible memory, counted
    //    live until it is unmapped. Two refusals in a minute log one line.
    orbis::DirectLedger l = fresh_ledger();
    orbis::mmapDirectSetReserve(l.free_total - big + 2 * mib);
    CHECK(stats().reserve == l.free_total - big + 2 * mib, "reserve not stored");
    void* a = heap_map(big);
    void* b = heap_map(big);
    orbis::MmapDirectStats s = stats();
    CHECK(mapped(a) && mapped(b) && g_flexible_maps == flexible0 + 2, "refused requests must go to flexible memory");
    CHECK(s.held == s0.held && s.critical == 1 && s.reserve_refusals == s0.reserve_refusals + 2 && s.refusals == s0.refusals + 2 &&
        s.pressure_events == s0.pressure_events + 2 && s.last_refusal_us != 0,
        "refusal: held %llu/%llu MiB, critical %u, reserve refusals +%llu, refusals +%llu, pressure +%llu",
        static_cast<unsigned long long>(s.held / mib), static_cast<unsigned long long>(s0.held / mib), s.critical,
        static_cast<unsigned long long>(s.reserve_refusals - s0.reserve_refusals),
        static_cast<unsigned long long>(s.refusals - s0.refusals),
        static_cast<unsigned long long>(s.pressure_events - s0.pressure_events));
    CHECK(g_reserve_lines == lines0 + 1, "two refusals in a minute: %llu line(s)", static_cast<unsigned long long>(g_reserve_lines - lines0));
    CHECK(s.fallback_live == s0.fallback_live + 2 * big && s.fallback_bytes == s0.fallback_bytes + 2 * big,
        "fallback live %llu MiB, fallback %llu MiB", static_cast<unsigned long long>((s.fallback_live - s0.fallback_live) / mib),
        static_cast<unsigned long long>((s.fallback_bytes - s0.fallback_bytes) / mib));
    __munmap(a, big);
    CHECK(stats().fallback_live == s0.fallback_live + big, "live fallback bytes did not fall on munmap");
    __munmap(b, big);
    CHECK(stats().fallback_live == s0.fallback_live && stats().fallback_bytes == s0.fallback_bytes + 2 * big,
        "live fallback %llu MiB after both unmaps", static_cast<unsigned long long>((stats().fallback_live - s0.fallback_live) / mib));

    // 2. A carve-out that leaves the reservation intact is taken.
    l = fresh_ledger();
    orbis::mmapDirectSetReserve(l.free_total - big);
    void* v = heap_map(big);
    s = stats();
    orbis::MmapDirectSpan span{};
    CHECK(mapped(v) && g_flexible_maps == flexible0 + 2 && s.held == s0.held + big, "admitted: held %llu MiB",
        static_cast<unsigned long long>((s.held - s0.held) / mib));
    CHECK(orbis::mmapDirectDescribe(v, span) && span.size == big && span.offset == 0, "admitted carve-out of %llu MiB",
        static_cast<unsigned long long>(span.size / mib));

    // 3. With the ladder a carve-out shrinks to the room the reservation leaves: 48 MiB instead of 128. Without
    //    trimming, free blocks of earlier carve-outs would serve the request.
    void* shrunk = nullptr;
#if ORBIS_MMAP_CARVE_LADDER
    if (trim_active())
    {
        l = fresh_ledger();
        orbis::mmapDirectSetReserve(l.free_total - 48 * mib);
        shrunk = heap_map(1 * mib);
        CHECK(mapped(shrunk) && orbis::mmapDirectDescribe(shrunk, span) && span.size == 48 * mib &&
            stats().held == s0.held + big + 48 * mib && g_flexible_maps == flexible0 + 2,
            "ladder under the reservation: carve-out %llu MiB, held +%llu MiB", static_cast<unsigned long long>(span.size / mib),
            static_cast<unsigned long long>((stats().held - s0.held) / mib));
    }
#endif

    // 4. The pressure hook hears about a request no carve-out can serve; it frees a block and the retry uses it.
    l = fresh_ledger();
    orbis::mmapDirectSetReserve(l.free_total - 2 * mib); // no new carve-out from here on
    g_hook.victim = v;
    g_hook.victim_len = big;
    CHECK(orbis::setPressureHook(test_hook) == nullptr, "a hook was installed before the test");
    const std::uint64_t events = stats().pressure_events;
    void* again = heap_map(big);
    CHECK(again == v && g_flexible_maps == flexible0 + 2, "the retry after the hook must reuse the freed block (%p, %p)", again, v);
    CHECK(g_hook.calls == 1 && g_hook.bytes == big && (g_hook.why & orbis::MmapPressureReserve) &&
        !(g_hook.why & orbis::MmapPressureSmallHeap) && stats().pressure_events == events + 1,
        "hook: %d call(s), %llu bytes, why 0x%x", g_hook.calls.load(), static_cast<unsigned long long>(g_hook.bytes.load()), g_hook.why.load());

    // 5. A request made inside the hook does not enter it again: both requests go to flexible memory.
    g_hook.nested_len = big;
    void* outer = heap_map(big);
    CHECK(g_hook.calls == 2 && mapped(g_hook.nested) && mapped(outer) && g_flexible_maps == flexible0 + 4,
        "re-entry: %d call(s), nested %p, outer %p, flexible +%llu", g_hook.calls.load(), g_hook.nested, outer,
        static_cast<unsigned long long>(g_flexible_maps - flexible0));
    __munmap(outer, big);
    __munmap(g_hook.nested, big);
    g_hook.nested = nullptr;
    CHECK(stats().fallback_live == s0.fallback_live, "live fallback bytes after the re-entry test");

#ifdef ORBIS_MMAP_EXPAND_STEP_MAX_MIB
    // 6. Small-heap growth under pressure: the hook is told malloc's lock is held; the step comes from flexible memory
    //    and stays live (musl never unmaps it).
    size_t n = big;
    void* step = __expand_heap(&n);
    s = stats();
    CHECK(step && n == big && g_hook.calls == 3 && (g_hook.why & orbis::MmapPressureSmallHeap) &&
        (g_hook.why & orbis::MmapPressureReserve), "small-heap step under pressure: %p, why 0x%x", step, g_hook.why.load());
    CHECK(s.fallback_small == s0.fallback_small + big && s.fallback_live == s0.fallback_live + big,
        "small-heap fallback: small %llu MiB, live %llu MiB", static_cast<unsigned long long>((s.fallback_small - s0.fallback_small) / mib),
        static_cast<unsigned long long>((s.fallback_live - s0.fallback_live) / mib));
#endif

    // 7. The hook and the reservation go away; the governor clears the critical flag.
    CHECK(orbis::setPressureHook(nullptr) == test_hook, "setPressureHook did not return the installed hook");
    orbis::mmapDirectSetReserve(0);
    orbis::mmapDirectClearCritical();
    CHECK(stats().critical == 0 && stats().reserve == 0, "critical %u reserve %llu", stats().critical,
        static_cast<unsigned long long>(stats().reserve));

    // 8. A refusal a minute after the last line is logged again.
    g_clock_offset_us += 61000000;
    l = fresh_ledger();
    orbis::mmapDirectSetReserve(l.free_total - 2 * mib);
    void* late = heap_map(big);
    CHECK(mapped(late) && g_reserve_lines == lines0 + 2, "a minute later: %llu line(s)",
        static_cast<unsigned long long>(g_reserve_lines - lines0));
    __munmap(late, big);
    orbis::mmapDirectSetReserve(0);
    orbis::mmapDirectClearCritical();

    __munmap(again, big);
    if (shrunk) __munmap(shrunk, 1 * mib);
    orbis::mmapDirectTrim(0);
    std::printf("reserve: refusal (flexible, live fallback bytes back to %llu MiB on munmap, one line a minute), admission, %s"
        "pressure hook (freed block reused, no re-entry%s): ok\n", static_cast<unsigned long long>(stats().fallback_live / mib),
        shrunk ? "ladder shrink to 48 MiB, " : "",
#ifdef ORBIS_MMAP_EXPAND_STEP_MAX_MIB
        ", small-heap bit"
#else
        ""
#endif
    );
}

#if ORBIS_MMAP_HEAP_BAND_MIB
// New carve-outs at the top of the pool: the top of the highest free run that holds them (a walked ledger), else
// first fit in the top band, else first fit anywhere.
void placement_phase()
{
    // Longer than every carve-out left by the earlier phases (kept when the flag file turns trimming off).
    constexpr std::uint64_t len = 140 * mib, band = std::uint64_t(ORBIS_MMAP_HEAP_BAND_MIB) * mib;
    orbis::mmapDirectTrim(0);

    // a. Fresh sample.
    orbis::DirectLedger l = fresh_ledger();
    std::uint64_t expect = l.estimated ? pool::expected_first_fit(pool::total - band, pool::total, len) : pool::expected_top(len);
    void* a = heap_map(len);
    CHECK(mapped(a) && pool::last_start == expect && pool::last_len == len, "placement: phys 0x%llx, expected 0x%llx",
        static_cast<unsigned long long>(pool::last_start), static_cast<unsigned long long>(expect));

    // b. Another owner takes the top of that run after the sample: first fit in the top band instead.
    void* b = nullptr;
    if (!l.estimated)
    {
        l = fresh_ledger();
        const std::uint64_t top = pool::expected_top(len);
        CHECK(top != pool::none && pool::take_at(top + len - 64 * mib, 64 * mib), "could not take the top of the run");
        expect = pool::expected_first_fit(pool::total - band, pool::total, len);
        const std::uint64_t attempts = g_alloc_attempts;
        b = heap_map(len);
        CHECK(mapped(b) && pool::last_start == expect && g_alloc_attempts == attempts + 2,
            "stale run: phys 0x%llx, expected 0x%llx, %llu attempts", static_cast<unsigned long long>(pool::last_start),
            static_cast<unsigned long long>(expect), static_cast<unsigned long long>(g_alloc_attempts - attempts));
    }

    // c. The band full as well: first fit anywhere.
    l = fresh_ledger();
    const std::uint64_t top = l.estimated ? pool::none : pool::expected_top(len);
    pool::fill(pool::total - band, pool::total);
    expect = (top != pool::none && top + len <= pool::total - band) ? top : pool::expected_first_fit(0, pool::total, len);
    void* c = heap_map(len);
    CHECK(mapped(c) && pool::last_start == expect && expect < pool::total - band, "band full: phys 0x%llx, expected 0x%llx",
        static_cast<unsigned long long>(pool::last_start), static_cast<unsigned long long>(expect));

    __munmap(a, len);
    if (b) __munmap(b, len);
    __munmap(c, len);
    pool::give_back_all();
    orbis::mmapDirectTrim(0);
    std::printf("placement: %s, first fit anywhere with the band full: ok\n", l.estimated ?
        "band first fit (estimated ledger: no runs)" : "top of the highest free run, band first fit when it was taken meanwhile");
}
#endif

#if ORBIS_MMAP_SEGREGATE && defined(ORBIS_MMAP_EXPAND_STEP_MAX_MIB)
// Small-heap steps and large mappings in separate carve-outs; large ones empty out and are trimmed; a kind borrows
// the other's free blocks only when no carve-out can be taken; a refused carve-out first trims free carve-outs.
void segregation_phase()
{
    if (!seg_active())
    {
        CHECK(stats().held_small == 0 && orbis::mmapDirectTrim(0) == 0 && stats().trim_enabled == 0,
            "flag file: small-heap carve-outs %llu, trim on", static_cast<unsigned long long>(stats().held_small));
        std::printf("segregation: off with the flag file (one carve-out list, no trim): ok\n");
        return;
    }
    orbis::mmapDirectTrim(0);
    orbis::mmapDirectSetReserve(0);
    orbis::mmapDirectSetCap(0);
    const orbis::MmapDirectStats s0 = stats();
    const std::uint64_t flexible0 = g_flexible_maps;
    orbis::MmapDirectSpan small{}, span{}, own{};

    size_t n = 1;
    void* step = __expand_heap(&n);
    CHECK(step && orbis::mmapDirectDescribe(step, small) && small.size == 128 * mib &&
        stats().held_small == s0.held_small + 128 * mib, "small-heap carve-out: %llu MiB", static_cast<unsigned long long>(small.size / mib));
    // Above 32 MiB: a carve-out of the mapping's own size. Below: a large carve-out, never the small heap's.
    void* l40 = heap_map(40 * mib);
    CHECK(mapped(l40) && orbis::mmapDirectDescribe(l40, own) && own.size == 40 * mib && own.offset == 0, "own carve-out of %llu MiB",
        static_cast<unsigned long long>(own.size / mib));
    void* l8 = heap_map(8 * mib);
    CHECK(mapped(l8) && orbis::mmapDirectDescribe(l8, span) && span.base != small.base && span.base != own.base && span.size == 128 * mib,
        "8 MiB mapping in a %llu MiB carve-out", static_cast<unsigned long long>(span.size / mib));
    CHECK(stats().held == s0.held + 128 * mib + 40 * mib + 128 * mib && stats().held_small == s0.held_small + 128 * mib,
        "held %llu MiB", static_cast<unsigned long long>((stats().held - s0.held) / mib));
    // Freed large mappings leave their carve-outs empty: trim returns them and keeps the small heap's.
    __munmap(l40, 40 * mib);
    __munmap(l8, 8 * mib);
    const std::uint64_t back = orbis::mmapDirectTrim(0);
    CHECK(back == 168 * mib && stats().held == s0.held + 128 * mib && stats().held_small == s0.held_small + 128 * mib &&
        orbis::mmapDirectDescribe(step, span), "trim returned %llu MiB", static_cast<unsigned long long>(back / mib));

    // No new carve-out allowed: a large mapping borrows a free block of the small heap's carve-out.
    orbis::DirectLedger l = fresh_ledger();
    orbis::mmapDirectSetReserve(l.free_total - 2 * mib);
    void* lent = heap_map(1 * mib);
    CHECK(mapped(lent) && orbis::mmapDirectDescribe(lent, span) && span.base == small.base && g_flexible_maps == flexible0,
        "borrowed block in %p, small heap at %p", span.base, small.base);

    // A refused carve-out first returns the entirely free carve-outs, then is admitted.
    orbis::mmapDirectSetReserve(0);
    void* tmp = heap_map(8 * mib);
    CHECK(mapped(tmp) && orbis::mmapDirectDescribe(tmp, span) && span.base != small.base && span.size == 128 * mib, "temporary carve-out");
    __munmap(tmp, 8 * mib);
    l = fresh_ledger();
    orbis::mmapDirectSetReserve(l.free_total - 2 * mib);
    const std::uint64_t trimmed0 = stats().trimmed;
    void* wide = heap_map(130 * mib); // longer than the free carve-out and than the small heap's free blocks
    CHECK(mapped(wide) && orbis::mmapDirectDescribe(wide, own) && own.size == 130 * mib && g_flexible_maps == flexible0 &&
        stats().trimmed == trimmed0 + 128 * mib, "refusal trim: carve-out %llu MiB, trimmed %llu MiB, flexible +%llu",
        static_cast<unsigned long long>(own.size / mib), static_cast<unsigned long long>((stats().trimmed - trimmed0) / mib),
        static_cast<unsigned long long>(g_flexible_maps - flexible0));

    __munmap(wide, 130 * mib);
    __munmap(lent, 1 * mib);
    orbis::mmapDirectSetReserve(0);
    orbis::mmapDirectClearCritical();
    __munmap(step, n); // musl never does; the test returns the small heap's carve-out too
    orbis::mmapDirectTrim(0);
    CHECK(stats().held_small == s0.held_small && stats().carves == s0.carves && stats().held == s0.held,
        "after the segregation phase: small %llu MiB, %u carve-out(s)", static_cast<unsigned long long>(stats().held_small / mib),
        stats().carves);
    std::printf("segregation: small-heap carve-out, own carve-out above 32 MiB, large carve-outs trimmed, borrowing, "
        "trim on a refused carve-out: ok\n");
}
#endif

// A walk that keeps disagreeing with sceKernelAvailableDirectMemorySize gives estimates, and after 8 samples in a
// row the ledger stops walking.
void untrusted_phase()
{
#ifndef TEST_NO_DMQ
    if (!walk_expected) return;
    g_dmq_mode = 4;
    orbis::DirectLedger l{};
    for (int i = 0; i < 8; i++)
    {
        orbis::directLedger(&l, 0);
        CHECK(l.estimated == 1, "untrusted sample %d not estimated", i);
    }
    CHECK(g_untrusted_lines == 1, "%llu 'ESTIMATED from now on' line(s)", static_cast<unsigned long long>(g_untrusted_lines.load()));
    const std::uint64_t calls = g_dmq_calls;
    orbis::directLedger(&l, 0);
    CHECK(l.estimated == 1 && g_dmq_calls == calls, "the ledger kept walking after it gave up");
    g_dmq_mode = 0;
    std::printf("untrusted walk: estimated samples, ledger estimated after 8 in a row: ok\n");
#endif
}

#ifdef ORBIS_MMAP_TRIM_OFF_FLAG
// The flag files are checked at the first request: created (or removed) before it.
void set_flag_files()
{
    const char* paths[] = {ORBIS_MMAP_TRIM_OFF_FLAG,
#ifdef ORBIS_MMAP_QUERY_OFF_FLAG
        ORBIS_MMAP_QUERY_OFF_FLAG
#endif
    };
    for (const char* p : paths)
    {
#ifdef TEST_FLAG_FILES
        if (FILE* f = std::fopen(p, "wb")) std::fclose(f);
#else
        std::remove(p);
#endif
    }
}
#endif
} // namespace

int main(int argc, char** argv)
{
    g_verbose = argc > 1 && std::strcmp(argv[1], "-v") == 0;
#ifdef ORBIS_MMAP_TRIM_OFF_FLAG
    set_flag_files();
#endif
#ifdef ORBIS_MMAP_EXPAND_STEP_MAX_MIB
    const int expand_step = ORBIS_MMAP_EXPAND_STEP_MAX_MIB;
#else
    const int expand_step = -1;
#endif
    std::printf("orbis_mmap test: ladder %d, trim %d, small-heap step cap %d MiB (-1 = musl's own), flexible floor %d MiB, "
        "segregate %d, heap band %d MiB, ledger %s\n",
        ORBIS_MMAP_CARVE_LADDER, ORBIS_MMAP_TRIM, expand_step, ORBIS_MMAP_FLEX_FLOOR_MIB, ORBIS_MMAP_SEGREGATE,
        ORBIS_MMAP_HEAP_BAND_MIB,
#if defined(TEST_NO_DMQ)
        "estimated (no sceKernelDirectMemoryQuery)"
#elif defined(TEST_FLAG_FILES)
        "estimated (flag file), trim off by its flag file"
#else
        "walked"
#endif
    );
    // The first carve-out can come before the program sets the log sink; with the top band it probes the ledger.
    g_log_set = !ORBIS_MMAP_HEAP_BAND_MIB;
    race_phase();
    g_log_set = true;
    drain_phase();
    ledger_phase();
#if ORBIS_MMAP_CARVE_LADDER && ORBIS_MMAP_TRIM
    if (trim_active())
        ladder_and_cap_phase();
    else
#endif
        default_cap_phase();
#if ORBIS_MMAP_CARVE_LADDER
    if (!trim_active()) overdraft_latch_phase();
#endif
#if ORBIS_MMAP_FLEX_FLOOR_MIB
    flex_floor_phase();
#endif
#ifdef ORBIS_MMAP_EXPAND_STEP_MAX_MIB
    expand_heap_phase();
#else
    CHECK(stats().small_heap == 0, "small heap counted without the interposer");
#endif
    reserve_phase();
#if ORBIS_MMAP_HEAP_BAND_MIB
    placement_phase();
#endif
#if ORBIS_MMAP_SEGREGATE && defined(ORBIS_MMAP_EXPAND_STEP_MAX_MIB)
    segregation_phase();
#endif
    untrusted_phase();
    orbis::mmapDirectReport("test end");
    orbis::directLedgerReport("test end");
#if defined(ORBIS_MMAP_TRIM_OFF_FLAG) && defined(TEST_FLAG_FILES)
    std::remove(ORBIS_MMAP_TRIM_OFF_FLAG);
#ifdef ORBIS_MMAP_QUERY_OFF_FLAG
    std::remove(ORBIS_MMAP_QUERY_OFF_FLAG);
#endif
#endif
    if (g_failures)
    {
        std::printf("orbis_mmap test FAILED (%d check(s))\n", g_failures.load());
        return 1;
    }
    std::printf("orbis_mmap test passed\n");
    return 0;
}
