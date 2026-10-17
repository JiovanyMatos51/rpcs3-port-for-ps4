#include "../../rpcs3/util/ps4_virtual_memory.hpp"

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <cstdio>
#include <thread>
#include <vector>

namespace
{
constexpr std::size_t arena_size = 32 * 1024 * 1024;
alignas(8192) unsigned char arena_storage[arena_size + 65536];
unsigned char* const arena = reinterpret_cast<unsigned char*>(
    (reinterpret_cast<std::uintptr_t>(arena_storage) + 65535) & ~std::uintptr_t(65535));
// Host VA for tests that touch hundreds of MB: reserved up front, committed
// only while the mock kernel has a direct mapping there.
constexpr std::size_t big_size = 512 * 1024 * 1024;
unsigned char* big = nullptr;
struct region { std::uintptr_t start, end; bool committed; };
struct physical_region { std::int64_t start; std::size_t size; };
std::vector<region> regions;
std::vector<physical_region> physical_regions;
std::size_t physical_bytes = 0, reserved_bytes = 0;
std::int64_t next_physical = 0;
unsigned reserve_calls = 0, map_calls = 0, protect_calls = 0, query_calls = 0;
unsigned large_allocations = 0, large_maps = 0;
bool fail_reserve = false, fail_allocate = false, fail_map = false, fail_protect = false;
bool fail_large_allocate = false, fail_large_map = false;
std::size_t fail_allocate_above = 0;
int reserve_error = 0;
bool redirect_mapping = false;
bool fail_query = false;
std::atomic<unsigned> usleep_calls{0}, yield_calls{0};
std::atomic<int> kernel_inside{0};
std::chrono::microseconds protect_delay{0};
constexpr int invalid = static_cast<int>(0x80020016u);
constexpr int no_memory = static_cast<int>(0x8002000cu);
constexpr int denied = static_cast<int>(0x8002000du);

// Every mocked call that touches shared mock state runs under the backend
// lock. Two at once means memory_lock failed to exclude.
struct exclusive_call
{
    exclusive_call() { const int previous = kernel_inside.fetch_add(1); assert(previous == 0); (void)previous; }
    ~exclusive_call() { kernel_inside.fetch_sub(1); }
};

bool in_big(std::uintptr_t start, std::uintptr_t end)
{
    const auto base = reinterpret_cast<std::uintptr_t>(big);
    return big && start >= base && end <= base + big_size;
}

void erase(std::uintptr_t start, std::uintptr_t end)
{
    std::vector<region> survivors;
    for (auto r : regions)
    {
        if (r.end <= start || r.start >= end) survivors.push_back(r);
        else
        {
            if (r.start < start) survivors.push_back({r.start, start, r.committed});
            if (r.end > end) survivors.push_back({end, r.end, r.committed});
        }
    }
    regions.swap(survivors);
}

bool mapped(const void* pointer)
{
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    for (auto r : regions) if (r.committed && address >= r.start && address < r.end) return true;
    return false;
}

std::size_t committed_bytes(const void* pointer, std::size_t size)
{
    const auto start = reinterpret_cast<std::uintptr_t>(pointer), end = start + size;
    std::size_t total = 0;
    for (auto r : regions)
        if (r.committed && r.start < end && r.end > start) total += std::min(r.end, end) - std::max(r.start, start);
    return total;
}

bool all_zero(const unsigned char* pointer, std::size_t size)
{
    return std::all_of(pointer, pointer + size, [](unsigned char v) { return v == 0; });
}

utils::ps4::memory::statistics stats() { return utils::ps4::memory::get_statistics(); }
}

// Mock native kernel calls only; the production adapter, ownership table,
// page alignment, zero-fill and error handling run unchanged in this test.
extern "C" int sceKernelReserveVirtualRange(void** pointer, std::size_t size, int flags, std::size_t alignment)
{
    exclusive_call guard;
    ++reserve_calls;
    if (fail_reserve) return no_memory;
    if (reserve_error) return reserve_error;
    assert(size && size % 0x4000 == 0 && (alignment == 0x4000 || alignment == 0x10000));
    assert(flags == 0 || flags == 0x90);
    if (!*pointer) *pointer = arena;
    const auto start = reinterpret_cast<std::uintptr_t>(*pointer);
    for (auto r : regions) if (r.start < start + size && r.end > start) return no_memory;
    regions.push_back({start, start + size, false});
    reserved_bytes = size;
    return 0;
}

extern "C" int sceKernelVirtualQuery(const void* pointer, int flags, void* destination, std::size_t size)
{
    exclusive_call guard;
    ++query_calls;
    assert(flags == 0 && size == 72);
    if (fail_query) return denied;
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    for (auto r : regions)
    {
        if (address < r.start || address >= r.end) continue;
        auto* bytes = static_cast<unsigned char*>(destination);
        std::memcpy(bytes, &r.start, sizeof(r.start));
        std::memcpy(bytes + 8, &r.end, sizeof(r.end));
        bytes[32] = r.committed ? 0x12 : 0;
        return 0;
    }
    return invalid;
}

extern "C" std::size_t sceKernelGetDirectMemorySize() { return 0x180000000ull; }
extern "C" int sceKernelAllocateDirectMemory(std::int64_t first, std::int64_t last,
    std::size_t size, std::size_t alignment, int type, std::int64_t* physical)
{
    exclusive_call guard;
    assert(first == 0 && last == 0x180000000ll && type == 0);
    assert(alignment == 0x4000 || (alignment == 0x200000 && size == 0x200000));
    assert(size && size % 0x4000 == 0 && size <= 0x200000);
    if (fail_allocate) return no_memory;
    if (fail_allocate_above && size > fail_allocate_above) return no_memory;
    if (alignment == 0x200000 && fail_large_allocate) return no_memory;
    next_physical = (next_physical + static_cast<std::int64_t>(alignment) - 1) & ~static_cast<std::int64_t>(alignment - 1);
    *physical = next_physical;
    physical_regions.push_back({next_physical, size});
    next_physical += size;
    physical_bytes += size;
    if (alignment == 0x200000) ++large_allocations;
    return 0;
}

extern "C" int sceKernelMapDirectMemory(void** pointer, std::size_t size, int protection,
    int flags, std::int64_t physical, std::size_t alignment)
{
    exclusive_call guard;
    ++map_calls;
    assert(protection == 3 && flags == 0x10 && (alignment == 0x4000 || alignment == 0x200000));
    assert(physical >= 0 && pointer && *pointer);
    if (alignment == 0x200000)
    {
        // Only whole 2 MB chunks, aligned both physically and in VA.
        assert(size == 0x200000 && physical % 0x200000 == 0);
        assert(reinterpret_cast<std::uintptr_t>(*pointer) % 0x200000 == 0);
        if (fail_large_map) return invalid;
        ++large_maps;
    }
    if (fail_map) return denied;
    if (redirect_mapping) *pointer = arena + 0x400000;
    const auto start = reinterpret_cast<std::uintptr_t>(*pointer);
    const bool inside_arena = start >= reinterpret_cast<std::uintptr_t>(arena)
        && start + size <= reinterpret_cast<std::uintptr_t>(arena + arena_size);
    assert(inside_arena || in_big(start, start + size));
    // A new direct mapping must never replace pages that are already backed.
    for (auto r : regions) assert(!r.committed || r.end <= start || r.start >= start + size);
    erase(start, start + size);
    regions.push_back({start, start + size, true});
    if (!inside_arena)
    {
        void* committed = VirtualAlloc(*pointer, size, MEM_COMMIT, PAGE_READWRITE);
        assert(committed == *pointer); (void)committed;
    }
    // Recycled direct memory holds stale data; the backend must zero it.
    std::memset(*pointer, 0xa5, size);
    return 0;
}

extern "C" int sceKernelMprotect(const void* pointer, std::size_t size, int)
{
    exclusive_call guard;
    ++protect_calls;
    assert(pointer && size && size % 0x4000 == 0);
    if (protect_delay.count())
    {
        // Hold the backend lock long enough for waiters to reach yield/sleep.
        const auto until = std::chrono::steady_clock::now() + protect_delay;
        while (std::chrono::steady_clock::now() < until) {}
    }
    if (fail_protect) return denied;
    return 0;
}
extern "C" int sceKernelMunmap(void* pointer, std::size_t size)
{
    exclusive_call guard;
    assert(pointer && size && size % 0x4000 == 0);
    const auto start = reinterpret_cast<std::uintptr_t>(pointer);
    erase(start, start + size);
    if (big)
    {
        // Return host pages of the big test region; reserved VA stays.
        const auto base = reinterpret_cast<std::uintptr_t>(big);
        const auto first = std::max(start, base), last = std::min(start + size, base + big_size);
        if (first < last) assert(VirtualFree(reinterpret_cast<void*>(first), last - first, MEM_DECOMMIT));
    }
    return 0;
}
extern "C" int sceKernelReleaseDirectMemory(std::int64_t start, std::size_t size)
{
    exclusive_call guard;
    assert(size && size % 0x4000 == 0 && start % 0x4000 == 0);
    for (std::size_t i = 0; i < physical_regions.size(); ++i)
    {
        const auto old = physical_regions[i];
        if (start < old.start || static_cast<std::uint64_t>(start - old.start) + size > old.size) continue;
        physical_regions.erase(physical_regions.begin() + i);
        if (start > old.start) physical_regions.push_back({old.start, static_cast<std::size_t>(start - old.start)});
        const auto end = start + static_cast<std::int64_t>(size);
        if (end < old.start + static_cast<std::int64_t>(old.size))
            physical_regions.push_back({end, static_cast<std::size_t>(old.start + old.size - end)});
        physical_bytes -= size;
        return 0;
    }
    assert(false && "Physical range was freed twice or never allocated");
    return invalid;
}
extern "C" int sceKernelUsleep(unsigned int) { ++usleep_calls; std::this_thread::yield(); return 0; }
extern "C" void scePthreadYield() { ++yield_calls; std::this_thread::yield(); }
void ps4_trace_memory(const char*, const void*, std::size_t, int, int, const void*) {}

namespace
{
char last_checkpoint[256];
unsigned checkpoint_lines = 0;
}

// The backend must log only after releasing memory_lock (the lazy fault handler takes it): another
// thread has to be able to take the lock while this runs.
void ps4_startup_checkpoint(const char* stage)
{
    std::snprintf(last_checkpoint, sizeof(last_checkpoint), "%s", stage);
    ++checkpoint_lines;
    std::atomic<bool> done{false};
    std::thread probe([&] { utils::ps4::memory::set_large_pages(utils::ps4::memory::large_pages()); done = true; });
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done && std::chrono::steady_clock::now() < until) std::this_thread::yield();
    if (!done)
    {
        std::puts("virtual_memory_test: FAIL (startup checkpoint written while memory_lock is held)");
        std::fflush(stdout);
        std::_Exit(1);
    }
    probe.join();
}

int main()
{
    using namespace utils::ps4::memory;
    // Shipped default (0.25): 2 MB pages are opt-in via /data/rpcs3/large-pages.flag.
    // The 2 MB sections below test the opt-in path; the off path is checked further down.
    assert(!large_pages());
    set_large_pages(true);
    std::memset(arena, 0xa5, arena_size);
    {
        const auto initial = stats();
        assert(initial.slots_used == 0 && initial.slots_high == 0 && initial.slots_capacity == 32768);
        assert(initial.reservations_used == 0 && initial.direct_bytes == 0 && initial.lazy_bytes == 0);
    }
    auto* base = static_cast<unsigned char*>(reserve(0x80000000));
    assert(base == arena && reserved_bytes == 0x80000000 && physical_bytes == 0);
    assert(reserve_calls == 1 && map_calls == 0);
    assert(stats().reservations_used == 1);
    fail_query = true;
    assert(commit(base + 3, 2, 7) && physical_bytes == 0x4000);
    assert(query_calls == 0); // Owned reservations don't need VirtualQuery.
    fail_query = false;
    for (unsigned i = 0; i < 0x4000; ++i) assert(base[i] == 0);
    base[9] = 0x55;
    const auto prior_maps = map_calls;
    assert(commit(base, 0x4000, 3) && base[9] == 0x55 && map_calls == prior_maps);

    // A middle decommit must preserve both physical survivors and free each
    // of them exactly once when the complete reservation is later released.
    assert(commit(base + 0x10000, 0x10000, 3));
    const auto prior_bytes = physical_bytes;
    assert(decommit(base + 0x14000, 0x4000) && physical_bytes == prior_bytes - 0x4000);
    assert(commit(base + 0x14000, 0x4000, 3) && physical_bytes == prior_bytes);
    base[0x14001] = 0x66;
    assert(decommit(base + 0x14000, 0x4000));
    assert(commit(base + 0x14000, 0x4000, 3) && base[0x14001] == 0);
    assert(stats().direct_bytes == physical_bytes);

    fail_allocate = true;
    assert(!commit(base + 0x30000, 0x4000, 3) && errno == ENOMEM);
    assert(physical_bytes == prior_bytes);
    fail_allocate = false;
    fail_map = true;
    assert(!commit(base + 0x30000, 0x4000, 3) && errno == EACCES && physical_bytes == prior_bytes);
    fail_map = false;
    fail_protect = true;
    assert(!commit(base + 0x30000, 0x4000, 7) && errno == EACCES && physical_bytes == prior_bytes);
    fail_protect = false;
    redirect_mapping = true;
    assert(!commit(base + 0x30000, 0x4000, 3) && errno == EFAULT && physical_bytes == prior_bytes);
    redirect_mapping = false;
    assert(stats().direct_bytes == physical_bytes);

    const auto previous_queries = query_calls, previous_protects = protect_calls;
    assert(!commit(nullptr, 0x200000, 7) && errno == EINVAL);
    assert(!protect(nullptr, 0x200000, 7) && errno == EINVAL);
    assert(query_calls == previous_queries && protect_calls == previous_protects);
    assert(!reserve(std::numeric_limits<std::size_t>::max()));
    assert(!reserve(0));
    assert(!commit(base, std::numeric_limits<std::size_t>::max(), 3));
    assert(commit(nullptr, 0, 3) && decommit(nullptr, 0) && release(nullptr, 0));

    std::vector<std::thread> workers;
    for (unsigned int i = 0; i < 4; ++i)
        workers.emplace_back([=] { assert(commit(base + 0x80000 + i * 0x4000, 0x4000, 3)); });
    for (auto& worker : workers) worker.join();
    assert(physical_bytes == prior_bytes + 4 * 0x4000);
    assert(release(base, 0x80000000));
    assert(physical_bytes == 0 && physical_regions.empty() && regions.empty());
    assert(stats().slots_used == 0 && stats().slots_high == 0 && stats().direct_bytes == 0);
    assert(stats().reservations_used == 0);

    fail_reserve = true;
    assert(!reserve(0x80000000) && errno == ENOMEM && physical_bytes == 0);
    fail_reserve = false;
    assert(reserve(0x200000000) == arena && reserved_bytes == 0x200000000 && physical_bytes == 0);
    assert(release(arena, 0x200000000));

    // Minecraft's first executable segment uses 0xba0000 guest bytes and
    // twice that size for the PPU dispatch table. A denied VirtualQuery must
    // not prevent commitment of a range the adapter itself reserved.
    assert(reserve(0x300000000) == arena);
    constexpr std::size_t ppu_table_size = 0x1740000;
    const auto queries_before_ppu = query_calls;
    fail_query = true;
    assert(commit(arena, ppu_table_size, 3));
    assert(physical_bytes == ppu_table_size && query_calls == queries_before_ppu);
    arena[0x200007] = 0x91;
    assert(commit(arena + 0x100000, 0x400000, 3));
    assert(arena[0x200007] == 0x91 && physical_bytes == ppu_table_size);
    // Partial release removes ownership too, so committing the hole must
    // fail rather than blindly overwrite memory that may belong elsewhere.
    assert(release(arena + 0x400000, 0x10000));
    assert(!commit(arena + 0x400000, 0x10000, 3) && errno == EACCES);
    assert(physical_bytes == ppu_table_size - 0x10000);
    assert(reserve(0x10000, arena + 0x400000) == arena + 0x400000);
    assert(commit(arena + 0x400000, 0x10000, 3));
    assert(arena[0x400000] == 0 && arena[0x200007] == 0x91);
    fail_query = false;
    assert(release(arena, 0x300000000) && physical_bytes == 0);

    // Commits are cut at 2 MB-aligned addresses; whole aligned chunks request
    // 2 MB-aligned direct memory (mock asserts the alignment invariants).
    {
        auto* aligned = reinterpret_cast<unsigned char*>((reinterpret_cast<std::uintptr_t>(arena) + 0x1fffff) & ~std::uintptr_t(0x1fffff));
        assert(reserve(0x1400000) == arena);
        const auto before = stats();
        const auto large_before = large_maps;
        // From an unaligned start: head piece, 3 whole 2 MB chunks, tail piece.
        assert(commit(aligned + 0x10000, 0x600000, 3));
        assert(large_maps - large_before == 2);
        assert(stats().large_chunks - before.large_chunks == large_maps - large_before);
        assert(stats().slots_used == 4 && physical_bytes == 0x600000);
        // Executable commits keep the 16 KB path.
        const auto large_exec_before = large_maps;
        assert(commit(aligned + 0x800000, 0x200000, 7) && large_maps == large_exec_before);
        // Disabled: no aligned requests.
        set_large_pages(false);
        assert(!large_pages());
        assert(commit(aligned + 0xa00000, 0x200000, 3) && large_maps == large_exec_before);
        set_large_pages(true);
        // A failed 2 MB-aligned allocation falls back to 16 KB alignment.
        fail_large_allocate = true;
        const auto fallback_before = stats().large_fallbacks;
        assert(commit(aligned + 0xc00000, 0x200000, 3));
        assert(stats().large_fallbacks == fallback_before + 1 && committed_bytes(aligned + 0xc00000, 0x200000) == 0x200000);
        fail_large_allocate = false;
        set_large_pages(true); // clears the back-off
        // A failed 2 MB-aligned map retries with 16 KB alignment.
        fail_large_map = true;
        assert(commit(aligned + 0xe00000, 0x200000, 3));
        assert(stats().large_fallbacks == fallback_before + 2 && committed_bytes(aligned + 0xe00000, 0x200000) == 0x200000);
        fail_large_map = false;
        assert(all_zero(aligned + 0xe00000, 0x200000));
        assert(release(arena, 0x1400000) && physical_bytes == 0 && stats().direct_bytes == 0);
        set_large_pages(true);
    }

    // High-water scans: the table tracks its highest used index and reuses
    // the lowest hole, exactly like the previous full scans.
    {
        assert(stats().slots_used == 0 && stats().slots_high == 0);
        auto* hw = static_cast<unsigned char*>(reserve(0x1000000));
        assert(hw == arena);
        for (unsigned i = 0; i < 64; ++i) assert(commit(hw + i * 0x40000, 0x4000, 3));
        assert(stats().slots_used == 64 && stats().slots_high == 64 && stats().slots_peak >= 64);
        for (unsigned i = 56; i < 64; ++i) assert(decommit(hw + i * 0x40000, 0x4000));
        assert(stats().slots_used == 56 && stats().slots_high == 56);
        assert(decommit(hw + 10 * 0x40000, 0x4000));
        assert(stats().slots_used == 55 && stats().slots_high == 56);
        hw[5 * 0x40000 + 1] = 0x42;
        const auto maps_before = map_calls;
        assert(commit(hw + 5 * 0x40000, 0x4000, 3) && map_calls == maps_before && hw[5 * 0x40000 + 1] == 0x42);
        assert(commit(hw + 10 * 0x40000, 0x4000, 3) && map_calls == maps_before + 1);
        assert(stats().slots_used == 56 && stats().slots_high == 56); // hole 10 reused
        assert(commit(hw + 63 * 0x40000, 0x4000, 3));
        assert(stats().slots_used == 57 && stats().slots_high == 57);
        // The entry at the high-water mark is still found by owner().
        hw[63 * 0x40000 + 3] = 0x24;
        assert(commit(hw + 63 * 0x40000, 0x4000, 3) && map_calls == maps_before + 2 && hw[63 * 0x40000 + 3] == 0x24);
        assert(release(hw, 0x1000000));
        assert(stats().slots_used == 0 && stats().slots_high == 0 && physical_bytes == 0);
    }

    // The hook arena is 32 GB of private demand-zero storage, not a shared
    // 32 GB physical object. Fault dispatch is exercised directly because
    // the PS4 signal-context ABI is unavailable on this Windows test host.
    constexpr std::size_t hook_size = 0x800000000;
    assert(reserve(hook_size) == arena && physical_bytes == 0);
    assert(!enable_lazy_zero(arena + 1, hook_size - 1) && errno == EINVAL);
    assert(enable_lazy_zero(arena, hook_size) && physical_bytes == 0);
    assert(!enable_lazy_zero(arena, hook_size) && errno == EBUSY);
    const auto queries_before_faults = query_calls;
    const auto arena_address = reinterpret_cast<std::uintptr_t>(arena);
    assert(!handle_lazy_fault(nullptr, 4));
    assert(!handle_lazy_fault(reinterpret_cast<void*>(arena_address - 1), 4));
    assert(!handle_lazy_fault(reinterpret_cast<void*>(arena_address + hook_size), 6));
    assert(!handle_lazy_fault(arena, 7)); // A protection violation remains fatal.
    assert(!handle_lazy_fault(arena, 20)); // Execution cannot allocate hook code.
    assert(query_calls == queries_before_faults && physical_bytes == 0);
    // One fault backs the whole 64 KB block, zeroed.
    errno = 1234;
    assert(handle_lazy_fault(arena + 23, 4) && physical_bytes == 0x10000);
    assert(errno == 1234); // The signal handler must not clobber errno.
    assert(all_zero(arena, 0x10000));
    arena[23] = 0x77;
    arena[0xc001] = 0x78;
    assert(handle_lazy_fault(arena + 23, 6) && arena[23] == 0x77 && physical_bytes == 0x10000);
    assert(handle_lazy_fault(arena + 0xc000, 6) && arena[0xc001] == 0x78 && physical_bytes == 0x10000);
    fail_allocate = true;
    const auto failures_before = stats().lazy_failures;
    assert(!handle_lazy_fault(arena + 0x20000, 6) && errno == ENOMEM && physical_bytes == 0x10000);
    assert(stats().lazy_failures == failures_before + 1 && stats().lazy_last_error == ENOMEM);
    fail_allocate = false;
    workers.clear();
    for (unsigned i = 0; i < 4; ++i)
        workers.emplace_back([=] {
            assert(handle_lazy_fault(arena + 0x30000, 6));
            assert(handle_lazy_fault(arena + 0x50000 + i * 0x4000, 4));
        });
    for (auto& worker : workers) worker.join();
    // Blocks [0x30000, 0x40000) and [0x50000, 0x60000): neighbours are not
    // backed, so neither grows.
    assert(physical_bytes == 3 * 0x10000);
    assert(stats().lazy_bytes == 3 * 0x10000);
    disable_lazy_zero();
    assert(!handle_lazy_fault(arena, 4));
    assert(!enable_lazy_zero(arena, hook_size) && errno == EINVAL);
    assert(decommit(arena, hook_size) && physical_bytes == 0);
    assert(stats().lazy_bytes == 0);
    assert(enable_lazy_zero(arena, hook_size));
    assert(handle_lazy_fault(arena + 23, 6) && arena[23] == 0);
    disable_lazy_zero();
    assert(release(arena, hook_size) && physical_bytes == 0 && physical_regions.empty() && regions.empty());
    assert(stats().lazy_bytes == 0 && stats().slots_used == 0);

    // More than 256 MB of lazy faults. The old backend took one slot per 16 KB
    // page: 320 MB would need 20480 of its 16384 slots and abort.
    {
        void* host = VirtualAlloc(nullptr, big_size + 0x200000, MEM_RESERVE, PAGE_NOACCESS);
        assert(host);
        big = reinterpret_cast<unsigned char*>((reinterpret_cast<std::uintptr_t>(host) + 0x1fffff) & ~std::uintptr_t(0x1fffff));
        assert(reserve(hook_size, big) == big);
        assert(enable_lazy_zero(big, hook_size));
        constexpr std::size_t dense = 320 * 1024 * 1024;
        const auto before = stats();
        const auto large_before = large_maps;
        unsigned faults = 0;
        for (std::size_t offset = 0; offset < dense; offset += 0x4000)
        {
            unsigned char* page = big + offset;
            if (!mapped(page))
            {
                assert(handle_lazy_fault(page + 5, 6));
                ++faults;
                assert(mapped(page));
            }
            assert(page[5] == 0 && page[0x3fff] == 0); // zero on first touch
            page[5] = static_cast<unsigned char>(offset >> 14);
        }
        // First block: 64 KB. Its neighbour is then backed, so the rest of
        // that 2 MB region follows in one fault, and then one fault per region.
        assert(faults == 2 + (dense / 0x200000 - 1));
        const auto after = stats();
        assert(after.slots_used == faults && after.slots_used < 200);
        assert(after.lazy_bytes == dense && physical_bytes == dense);
        assert(after.lazy_blocks - before.lazy_blocks == faults);
        assert(after.lazy_failures == before.lazy_failures);
        assert(large_maps - large_before == dense / 0x200000 - 1); // all aligned 2 MB regions
        for (std::size_t offset = 0; offset < dense; offset += 0x4000)
            assert(big[offset + 5] == static_cast<unsigned char>(offset >> 14));
        assert(decommit(big, dense) && physical_bytes == 0 && stats().lazy_bytes == 0);
        assert(stats().slots_used == 0 && stats().slots_high == 0);

        // Blocks never overlap pages that are already backed, and never zero them.
        unsigned char* r = big + 0x10000000; // 2 MB-aligned region
        assert(commit(r + 0x8000, 0x4000, 3));
        r[0x8001] = 0x5a;
        auto bytes = physical_bytes;
        assert(handle_lazy_fault(r + 0x100, 4));
        assert(physical_bytes == bytes + 0x8000 && committed_bytes(r, 0x10000) == 0xc000);
        assert(r[0x8001] == 0x5a && all_zero(r, 0x8000));
        assert(!mapped(r + 0xc000));
        bytes = physical_bytes;
        assert(handle_lazy_fault(r + 0xc000, 6) && physical_bytes == bytes + 0x4000);
        assert(r[0x8001] == 0x5a);
        // The previous page is backed, so this fault grows to the region end.
        bytes = physical_bytes;
        assert(handle_lazy_fault(r + 0x10000, 6) && physical_bytes == bytes + 0x200000 - 0x10000);
        assert(committed_bytes(r, 0x200000) == 0x200000 && r[0x8001] == 0x5a);
        // A fault on a page that is already backed (a racing thread) allocates nothing.
        bytes = physical_bytes;
        assert(handle_lazy_fault(r + 0x100, 6) && physical_bytes == bytes);
        // Downward sequential use: a backed page after the block grows to the region start.
        unsigned char* d = big + 0x10800000;
        bytes = physical_bytes;
        assert(handle_lazy_fault(d + 0x200000 - 0x100, 6) && physical_bytes == bytes + 0x10000);
        assert(handle_lazy_fault(d + 0x200000 - 0x10100, 6) && physical_bytes == bytes + 0x200000);
        assert(committed_bytes(d, 0x200000) == 0x200000 && all_zero(d, 0x200000));
        // Fragmented physical memory: only the faulting page is backed.
        unsigned char* f = big + 0x11000000;
        fail_allocate_above = 0x4000;
        bytes = physical_bytes;
        const auto failures_mid = stats().lazy_failures;
        assert(handle_lazy_fault(f + 0x14000, 6) && physical_bytes == bytes + 0x4000);
        assert(mapped(f + 0x14000) && !mapped(f + 0x10000) && stats().lazy_failures == failures_mid);
        fail_allocate_above = 0;
        fail_allocate = true;
        assert(!handle_lazy_fault(f + 0x40000, 6) && errno == ENOMEM);
        assert(stats().lazy_failures == failures_mid + 1 && stats().lazy_last_error == ENOMEM);
        fail_allocate = false;
        // A large-page fallback inside the fault path still backs the region.
        unsigned char* l = big + 0x11800000;
        assert(handle_lazy_fault(l + 0x100, 6));
        assert(handle_lazy_fault(l + 0x10000, 6) && committed_bytes(l, 0x200000) == 0x200000);
        fail_large_allocate = true;
        const auto fallbacks = stats().large_fallbacks;
        assert(handle_lazy_fault(l + 0x200000 + 0x100, 6)); // previous page backed: whole aligned 2 MB
        assert(stats().large_fallbacks == fallbacks + 1 && committed_bytes(l + 0x200000, 0x200000) == 0x200000);
        fail_large_allocate = false;
        set_large_pages(true);

        // Concurrent faults in the same blocks: each page is backed once, and
        // bytes written by one thread survive blocks committed by another.
        unsigned char* c = big + 0x12000000;
        workers.clear();
        for (unsigned t = 0; t < 8; ++t)
            workers.emplace_back([=] {
                for (unsigned i = 0; i < 256; ++i)
                {
                    unsigned char* p = c + ((i * 7 + t * 13) % 128) * 0x4000 + t * 16 + (i % 16);
                    assert(handle_lazy_fault(p, 6));
                    *p = static_cast<unsigned char>(t + 1);
                }
            });
        for (auto& worker : workers) worker.join();
        for (unsigned t = 0; t < 8; ++t)
            for (unsigned i = 0; i < 256; ++i)
                assert(c[((i * 7 + t * 13) % 128) * 0x4000 + t * 16 + (i % 16)] == t + 1);
        assert(committed_bytes(c, 0x200000) <= 0x200000);
        // Every backed byte in the region is inside the arena, including the commit() page.
        assert(stats().lazy_bytes == committed_bytes(big, big_size) && stats().lazy_bytes == physical_bytes);
        disable_lazy_zero();
        assert(release(big, hook_size) && physical_bytes == 0 && regions.empty());
        assert(stats().lazy_bytes == 0 && stats().direct_bytes == 0 && stats().slots_used == 0);
        assert(VirtualFree(host, 0, MEM_RELEASE));
        big = nullptr;
    }

    // Lock under contention: kernel calls stay exclusive (asserted in every
    // mock) and waiters back off from spinning to yielding and sleeping.
    {
        auto* lock_area = static_cast<unsigned char*>(reserve(0x100000));
        assert(lock_area == arena && commit(lock_area, 0x40000, 3));
        const auto before = stats();
        protect_delay = std::chrono::microseconds(50);
        workers.clear();
        for (unsigned t = 0; t < 4; ++t)
            workers.emplace_back([=] {
                for (unsigned i = 0; i < 100; ++i)
                    assert(protect(lock_area + (t * 4 + i % 4) * 0x4000, 0x4000, i % 2 ? 1 : 3));
            });
        for (auto& worker : workers) worker.join();
        protect_delay = std::chrono::microseconds(0);
        const auto after = stats();
        assert(after.lock_contended > before.lock_contended);
        assert(after.lock_yields + after.lock_sleeps > before.lock_yields + before.lock_sleeps);
        assert(yield_calls + usleep_calls > 0);
        std::printf("lock: contended=%llu yields=%llu sleeps=%llu\n", after.lock_contended - before.lock_contended,
            after.lock_yields - before.lock_yields, after.lock_sleeps - before.lock_sleeps);
        // Uncontended mix of commits/protects from many threads.
        workers.clear();
        for (unsigned t = 0; t < 8; ++t)
            workers.emplace_back([=] {
                for (unsigned i = 0; i < 200; ++i)
                {
                    assert(protect(lock_area + (i % 16) * 0x4000, 0x4000, 3));
                    assert(commit(lock_area + 0x40000 + ((t * 200 + i) % 48) * 0x4000, 0x4000, 3));
                }
            });
        for (auto& worker : workers) worker.join();
        assert(committed_bytes(lock_area, 0x100000) == 0x40000 + 48 * 0x4000);
        assert(release(lock_area, 0x100000) && physical_bytes == 0);
    }

    // LLVM creates a 768 MiB auxiliary arena per object module. Decommit
    // alone keeps its address reservation and exhausts a bounded pool even
    // though all physical pages have already been released.
    constexpr std::size_t llvm_arena_size = 0x30000000;
    assert(reserve(llvm_arena_size) == arena);
    assert(commit(arena, 0x80000, 7));
    assert(decommit(arena, llvm_arena_size) && physical_bytes == 0);
    assert(!reserve(llvm_arena_size) && errno == ENOMEM);
    assert(release(arena, llvm_arena_size));
    // More than a full firmware/game workload can reuse that virtual pool.
    // Both code/data mappings are released and each new arena starts zeroed.
    for (unsigned module = 0; module < 256; ++module)
    {
        assert(reserve(llvm_arena_size) == arena);
        assert(commit(arena, 0x80000, 7));
        assert(commit(arena + 0x100000, 0x80000, 3));
        assert(arena[23] == 0 && arena[0x100023] == 0);
        arena[23] = 0x71;
        arena[0x100023] = 0x81;
        assert(release(arena, llvm_arena_size));
        assert(physical_bytes == 0 && physical_regions.empty() && regions.empty());
    }
    // Persistent engines must coexist outside the default address pool.
    // Reserve more than 60 GiB of VA without allocating physical pages.
    std::vector<void*> jit_arenas;
    for (unsigned module = 0; module < 80; ++module)
    {
        auto* jit = reserve_jit(llvm_arena_size);
        const auto address = reinterpret_cast<std::uintptr_t>(jit);
        assert(address >= jit_virtual_begin && address + llvm_arena_size <= jit_virtual_end);
        for (const auto old : jit_arenas) assert(old != jit);
        jit_arenas.push_back(jit);
        assert(physical_bytes == 0);
    }
    assert(stats().reservations_used == 1); // adjacent arenas merge into one record
    auto* reusable = jit_arenas[15];
    assert(release(reusable, llvm_arena_size));
    assert(reserve_jit(llvm_arena_size) == reusable);
    for (const auto jit : jit_arenas) assert(release(jit, llvm_arena_size));
    assert(regions.empty() && stats().reservations_used == 0);
    // A foreign mapping is not in our metadata and must never be overwritten.
    regions.push_back({jit_virtual_begin, jit_virtual_begin + llvm_arena_size, false});
    auto* beyond_foreign = reserve_jit(llvm_arena_size);
    assert(reinterpret_cast<std::uintptr_t>(beyond_foreign) == jit_virtual_begin + llvm_arena_size);
    assert(release(beyond_foreign, llvm_arena_size));
    assert(regions.size() == 1 && regions[0].start == jit_virtual_begin);
    regions.clear();
    reserve_error = denied;
    const auto before_permission_failure = reserve_calls;
    assert(!reserve_jit(llvm_arena_size) && errno == EACCES);
    assert(reserve_calls == before_permission_failure + 1);
    reserve_error = 0;
    assert(!reserve_jit(jit_virtual_end - jit_virtual_begin + 0x4000) && errno == ENOMEM);
    assert(!reserve_jit(0) && errno == EINVAL);
    assert(!reserve_jit(std::numeric_limits<std::size_t>::max()) && errno == EINVAL);
    // The bounded range must fail cleanly rather than wrap or replace owners.
    auto* full = reserve_jit(jit_virtual_end - jit_virtual_begin);
    assert(reinterpret_cast<std::uintptr_t>(full) == jit_virtual_begin);
    assert(!reserve_jit(llvm_arena_size) && errno == ENOMEM);
    assert(release(full, jit_virtual_end - jit_virtual_begin));
    workers.clear();
    for (unsigned i = 0; i < 4; ++i) workers.emplace_back([]
    {
        for (unsigned pass = 0; pass < 100; ++pass)
        {
            auto* jit = reserve_jit(llvm_arena_size);
            assert(jit);
            assert(release(jit, llvm_arena_size));
        }
    });
    for (auto& worker : workers) worker.join();
    assert(physical_bytes == 0 && physical_regions.empty() && regions.empty());

    // Shared views (three per guest shared-memory block; games reached 3585 of the old 8192): the
    // table holds 32768, warns once at 75% after the lock is released, refuses the next view with
    // ENOMEM when full and reuses freed slots.
    {
        const auto initial = stats();
        assert(initial.shared_views_capacity == 32768 && initial.shared_views_used == 0);
        constexpr std::size_t view_size = 0x4000, views = 32768, warn_at = views / 4 * 3;
        auto* views_base = static_cast<unsigned char*>(reserve((views + 1) * view_size));
        assert(views_base == arena);
        const unsigned lines_before = checkpoint_lines;
        for (std::size_t i = 0; i < views; ++i)
        {
            assert(register_shared_view(views_base + i * view_size, view_size));
            assert(checkpoint_lines == lines_before + (i + 1 >= warn_at ? 1u : 0u));
        }
        assert(std::strstr(last_checkpoint, "PS4 memory: shared views at 24576/32768 (75% of the table)"));
        assert(stats().shared_views_used == views && stats().shared_views_capacity == views);
        assert(!register_shared_view(views_base + views * view_size, view_size) && errno == ENOMEM);
        assert(!register_shared_view(views_base + 7 * view_size, view_size) && errno == EBUSY);
        unregister_shared_view(views_base + 100 * view_size, view_size);
        assert(stats().shared_views_used == views - 1);
        assert(register_shared_view(views_base + views * view_size, view_size));
        unregister_shared_view(views_base + views * view_size, view_size);
        for (std::size_t i = views; i-- > 0;)
            if (i != 100) unregister_shared_view(views_base + i * view_size, view_size);
        assert(stats().shared_views_used == 0);
        // One warning per process, even after the table drained and filled again.
        for (std::size_t i = 0; i < warn_at + 8; ++i) assert(register_shared_view(views_base + i * view_size, view_size));
        assert(checkpoint_lines == lines_before + 1);
        for (std::size_t i = warn_at + 8; i-- > 0;) unregister_shared_view(views_base + i * view_size, view_size);
        assert(stats().shared_views_used == 0);
        assert(release(views_base, (views + 1) * view_size) && regions.empty());
    }

    // Diagnostic line for the 10 s report.
    char line[512];
    ps4_memory_report(line, sizeof(line));
    assert(std::strstr(line, "slots used=0 high=0 peak=") && std::strstr(line, "cap=32768"));
    assert(std::strstr(line, "lazy=0K") && !std::strchr(line, '\n'));
    char tiny[8];
    std::memset(tiny, 'x', sizeof(tiny));
    ps4_memory_report(tiny, sizeof(tiny));
    assert(std::strlen(tiny) == 7);
    ps4_memory_report(nullptr, 0);
    ps4_memory_report(tiny, 0);
    std::puts(line);
    const auto final = stats();
    assert(final.slots_peak < 1000 && final.lazy_failures >= 2);
    std::puts("virtual_memory_test: PASS (>256 MB lazy faults in 161 slots, non-overlapping zeroed blocks, 2 MB chunks + fallbacks, "
              "high-water tables, lock contention, 256 recycled arenas, 80 persistent JITs, user VA bounds, foreign mappings, "
              "permission failures, 32768 shared views with a one-time 75% warning outside the lock, and concurrency)");
}
