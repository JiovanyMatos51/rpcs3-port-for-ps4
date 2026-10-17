#include "../../rpcs3/util/ps4_shared_memory.hpp"
#include "../../rpcs3/util/ps4_virtual_memory.hpp"
#include <windows.h>
#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <map>
#include <vector>

namespace
{
struct physical { HANDLE section; std::size_t size; };
struct mapping { void* address; std::size_t size; std::int64_t physical; };
struct reservation { void* address; std::size_t size; };
std::map<std::int64_t, physical> physicals;
std::vector<mapping> mappings;
std::vector<reservation> reservations;
std::int64_t next_physical = 0;
std::size_t allocated_bytes = 0;
int alloc_calls = 0, map_calls = 0, large_allocs = 0, large_maps = 0;
int fail_alloc_call = -1, fail_map_call = -1;
bool fail_protection = false, aliases_enabled = false;
bool fail_large_alloc = false, fail_large_map = false;
std::vector<std::size_t> alloc_sizes;
constexpr int denied = static_cast<int>(0x8002000du);
constexpr int no_memory = static_cast<int>(0x8002000cu);
constexpr int invalid = static_cast<int>(0x80020016u);

bool overlaps(void* a, std::size_t an, void* b, std::size_t bn)
{
    const auto x = reinterpret_cast<std::uintptr_t>(a), y = reinterpret_cast<std::uintptr_t>(b);
    return x < y + bn && y < x + an;
}

int enable_aliasing() { aliases_enabled = true; return 0; }
}

extern "C" int sceKernelGetModuleList(std::uint32_t* ids, std::size_t capacity, std::size_t* count)
{
    assert(capacity == 256);
    ids[0] = 17; ids[1] = 29; *count = 2;
    return 0;
}
extern "C" int sceKernelDlsym(int module, const char* name, void** symbol)
{
    assert(std::strcmp(name, "sceKernelEnableDmemAliasing") == 0);
    if (module != 29) return invalid;
    *symbol = reinterpret_cast<void*>(enable_aliasing);
    return 0;
}
extern "C" std::size_t sceKernelGetDirectMemorySize() { return 0x180000000ull; }
extern "C" int sceKernelAllocateDirectMemory(std::int64_t start, std::int64_t end,
    std::size_t size, std::size_t alignment, int type, std::int64_t* result)
{
    assert(start == 0 && end == 0x180000000ll && size <= 0x200000 && size % 0x4000 == 0);
    assert((alignment == 0x4000 || (alignment == 0x200000 && size == 0x200000)) && type == 0);
    if (alignment == 0x200000 && fail_large_alloc) return no_memory;
    // From the given call on, so a 2 MB-aligned attempt and its fallback both fail.
    if (++alloc_calls >= fail_alloc_call && fail_alloc_call > 0) return no_memory;
    if (alignment == 0x200000) ++large_allocs;
    alloc_sizes.push_back(size);
    next_physical = (next_physical + static_cast<std::int64_t>(alignment) - 1) & ~static_cast<std::int64_t>(alignment - 1);
    HANDLE section = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, size, nullptr);
    assert(section);
    // Simulate recycled direct memory containing nonzero data.
    void* view = MapViewOfFile(section, FILE_MAP_WRITE, 0, 0, size);
    assert(view); std::memset(view, 0xa5, size); UnmapViewOfFile(view);
    *result = next_physical; physicals.emplace(next_physical, physical{section, size});
    next_physical += size; allocated_bytes += size;
    return 0;
}
extern "C" int sceKernelReserveVirtualRange(void** address, std::size_t size, int flags, std::size_t alignment)
{
    assert(size % 0x4000 == 0 && (alignment == 0x10000 || alignment == 0x4000) && (flags == 0 || flags == 0x90));
    if (!*address)
    {
        // Choose a contiguous address; metadata models the PS4 reservation.
        // Windows doesn't permit MapViewOfFileEx over a MEM_RESERVE range.
        *address = VirtualAlloc(nullptr, size, MEM_RESERVE, PAGE_NOACCESS);
        assert(*address); assert(VirtualFree(*address, 0, MEM_RELEASE));
    }
    for (const auto& v : mappings) if (overlaps(*address, size, v.address, v.size)) return no_memory;
    for (const auto& r : reservations) if (overlaps(*address, size, r.address, r.size)) return no_memory;
    reservations.push_back({*address, size});
    return 0;
}
extern "C" int sceKernelVirtualQuery(const void* address, int, void* output, std::size_t size)
{
    assert(size == 72);
    const auto pointer = reinterpret_cast<std::uintptr_t>(address);
    for (const auto& r : reservations)
    {
        const auto start = reinterpret_cast<std::uintptr_t>(r.address), end = start + r.size;
        if (pointer < start || pointer >= end) continue;
        std::memcpy(output, &start, 8); std::memcpy(static_cast<char*>(output) + 8, &end, 8);
        return 0;
    }
    return invalid;
}
extern "C" int sceKernelUsleep(unsigned int) { Sleep(0); return 0; }
extern "C" void scePthreadYield() { Sleep(0); }
extern "C" int sceKernelMapDirectMemory(void** address, std::size_t size, int prot,
    int flags, std::int64_t physical, std::size_t alignment)
{
    assert(aliases_enabled && flags == 0x10 && prot >= 0 && prot <= 3);
    assert(alignment == 0x4000 || alignment == 0x200000);
    if (alignment == 0x200000)
    {
        // Large pages only for whole chunks aligned physically and in VA.
        assert(size == 0x200000 && physical % 0x200000 == 0 && reinterpret_cast<std::uintptr_t>(*address) % 0x200000 == 0);
        if (fail_large_map) return invalid;
        ++large_maps;
    }
    if (++map_calls == fail_map_call) return denied;
    const auto& p = physicals.at(physical); assert(p.size == size);
    assert(std::any_of(reservations.begin(), reservations.end(), [=](const reservation& r)
    {
        const auto x = reinterpret_cast<std::uintptr_t>(*address), y = reinterpret_cast<std::uintptr_t>(r.address);
        return x >= y && x + size <= y + r.size;
    }));
    void* view = MapViewOfFileEx(p.section, FILE_MAP_WRITE, 0, 0, size, *address);
    assert(view == *address);
    DWORD previous = 0;
    assert(VirtualProtect(view, size, prot == 0 ? PAGE_NOACCESS : prot == 1 ? PAGE_READONLY : PAGE_READWRITE, &previous));
    mappings.push_back({view, size, physical});
    return 0;
}
extern "C" int sceKernelMprotect(const void* address, std::size_t size, int prot)
{
    if (fail_protection) return denied;
    for (const auto& v : mappings) if (overlaps(const_cast<void*>(address), size, v.address, v.size))
    {
        DWORD previous = 0;
        const auto protection = prot == 0 ? PAGE_NOACCESS : prot == 1 ? PAGE_READONLY : prot == 7 ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE;
        assert(VirtualProtect(v.address, v.size, protection, &previous));
    }
    return 0;
}
extern "C" int sceKernelMunmap(void* address, std::size_t size)
{
    for (auto i = mappings.begin(); i != mappings.end();)
    {
        if (overlaps(address, size, i->address, i->size))
        {
            assert(UnmapViewOfFile(i->address)); i = mappings.erase(i);
        }
        else ++i;
    }
    std::erase_if(reservations, [=](const reservation& r) { return r.address == address && r.size == size; });
    return 0;
}
extern "C" int sceKernelReleaseDirectMemory(std::int64_t physical, std::size_t size)
{
    assert(std::none_of(mappings.begin(), mappings.end(), [=](const mapping& v) { return v.physical == physical; }));
    const auto found = physicals.find(physical); assert(found != physicals.end() && found->second.size == size);
    assert(CloseHandle(found->second.section)); physicals.erase(found); allocated_bytes -= size;
    return 0;
}
void ps4_trace_memory(const char*, const void*, std::size_t, int, int, const void*) {}
void ps4_startup_checkpoint(const char*) {}

int main()
{
    using utils::ps4::shared_memory;
    // Shipped default (0.25): 2 MB pages are opt-in via /data/rpcs3/large-pages.flag.
    assert(!utils::ps4::memory::large_pages());
    utils::ps4::memory::set_large_pages(true);
    constexpr std::size_t size = 0x610000; // Three 2 MB chunks plus a 64 KB tail.
    {
        alloc_sizes.clear();
        shared_memory memory(size); assert(memory.valid() && allocated_bytes == size);
        // The partial chunk comes first, so a view ending on 2 MB uses large pages.
        assert(alloc_sizes.size() == 4 && alloc_sizes[0] == 0x10000 && alloc_sizes[1] == 0x200000);
        assert(large_allocs == 3);
        assert(utils::ps4::memory::get_statistics().shared_direct_bytes == size);
        auto* a = static_cast<unsigned char*>(memory.map(nullptr, 3)); assert(a);
        assert(std::all_of(a, a + size, [](unsigned char v) { return v == 0; }));
        a[11] = 0x71; a[0x200009] = 0x72; a[size - 1] = 0x73;
        auto* b = static_cast<unsigned char*>(memory.map(nullptr, 3)); assert(b && a != b);
        assert(b[11] == 0x71 && b[0x200009] == 0x72 && b[size - 1] == 0x73);
        b[0x400001] = 0x84; assert(a[0x400001] == 0x84);
        auto* c = static_cast<unsigned char*>(memory.map(nullptr, 1)); assert(c && c[0x400001] == 0x84);
        // Real VM commit over a shared view must preserve its physical pages.
        assert(utils::ps4::memory::commit(b, size, 3));
        assert(b[0x400001] == 0x84 && allocated_bytes == size);
        assert(!utils::ps4::memory::decommit(b, size) && errno == EBUSY);
        {
            // An unrelated object must not overwrite an active alias. Reject
            // the map before any kernel call can damage its existing contents.
            shared_memory other(size); assert(other.valid());
            assert(!other.map(b, 3) && errno == EBUSY);
            assert(b[0x400001] == 0x84 && c[0x400001] == 0x84);
        }
        assert(allocated_bytes == size); // Views must not allocate duplicate physical storage.
        assert(memory.unmap(b, true));
        b = static_cast<unsigned char*>(memory.map(b, 3)); assert(b && b[0x400001] == 0x84);
        assert(!memory.map(b, 3) && errno == EBUSY);
        assert(!memory.map(nullptr, 3, true) && errno == ENOTSUP);
        assert(!memory.map(b + 1, 3) && errno == EINVAL);
        assert(memory.unmap(a, false));
        b[size - 2] = 0x85; assert(c[size - 2] == 0x85);
        // Remaining aliases are cleaned before the object frees its pages.
    }
    assert(allocated_bytes == 0 && mappings.empty() && physicals.empty());
    assert(utils::ps4::memory::get_statistics().shared_direct_bytes == 0);
    while (!reservations.empty())
    {
        const auto remaining = reservations.back();
        assert(utils::ps4::memory::release(remaining.address, remaining.size));
    }
    {
        // The exact size rejected by native ftruncate is now direct storage.
        shared_memory memory(0x10000000); assert(memory.valid() && physicals.size() == 128);
    }
    assert(allocated_bytes == 0 && physicals.empty());
    fail_alloc_call = alloc_calls + 3;
    { shared_memory memory(size); assert(!memory.valid() && errno == ENOMEM); }
    assert(allocated_bytes == 0 && physicals.empty()); fail_alloc_call = -1;
    // The failures below are injected by map call number. A failed 2 MB map is retried with 16 KB
    // alignment (tested further down), so with large pages on, that retry absorbed the injected failure
    // whenever the host happened to place a 2 MB chunk of the view on a 2 MB boundary.
    utils::ps4::memory::set_large_pages(false);
    {
        shared_memory memory(size); assert(memory.valid());
        fail_map_call = map_calls + 3;
        assert(!memory.map(nullptr, 3) && errno == EACCES);
        assert(mappings.empty() && reservations.empty() && allocated_bytes == size);
        fail_map_call = -1;
        fail_protection = true;
        assert(!memory.map(nullptr, 7) && errno == EACCES);
        assert(mappings.empty() && reservations.empty()); fail_protection = false;
        auto* a = static_cast<unsigned char*>(memory.map(nullptr, 3)); assert(a);
        assert(std::all_of(a, a + size, [](unsigned char v) { return v == 0; }));
        auto* target = utils::ps4::memory::reserve(size, nullptr); assert(target);
        fail_map_call = map_calls + 2;
        assert(!memory.map(target, 3) && errno == EACCES);
        fail_map_call = -1;
        assert(memory.map(target, 3) == target); // Failed fixed map restored the reservation.
        assert(memory.unmap(target, true));
        assert(!memory.map(target, 3, false, false)); // try_map must not overwrite a reservation.
        sceKernelMunmap(target, size);
        assert(memory.map(target, 3, false, false) == target);
        memory.unmap(target, false);
    }
    utils::ps4::memory::set_large_pages(true);
    assert(allocated_bytes == 0 && physicals.empty() && mappings.empty() && reservations.empty());
    {
        // A view whose end is 2 MB-aligned maps its three whole chunks with
        // large pages; the 64 KB head chunk keeps 16 KB alignment.
        void* host = VirtualAlloc(nullptr, 0x1000000, MEM_RESERVE, PAGE_NOACCESS); assert(host);
        assert(VirtualFree(host, 0, MEM_RELEASE));
        const auto aligned = (reinterpret_cast<std::uintptr_t>(host) + 0x1fffff) & ~std::uintptr_t(0x1fffff);
        auto* target = reinterpret_cast<unsigned char*>(aligned + 0x200000 - 0x10000);
        shared_memory memory(size); assert(memory.valid());
        assert(utils::ps4::memory::reserve(size, target) == target);
        const auto maps_before = large_maps;
        assert(memory.map(target, 3) == target && large_maps == maps_before + 3);
        assert(std::all_of(target, target + size, [](unsigned char v) { return v == 0; }));
        target[0x10005] = 0x31;
        auto* other = static_cast<unsigned char*>(memory.map(nullptr, 1)); assert(other && other[0x10005] == 0x31);
        // A failed large map retries with 16 KB alignment.
        assert(memory.unmap(target, true));
        fail_large_map = true;
        const auto fallbacks = utils::ps4::memory::get_statistics().large_fallbacks;
        assert(memory.map(target, 3) == target && target[0x10005] == 0x31);
        assert(utils::ps4::memory::get_statistics().large_fallbacks == fallbacks + 3);
        fail_large_map = false;
        assert(memory.unmap(target, false));
        // Executable views and disabled large pages keep 16 KB alignment.
        assert(utils::ps4::memory::reserve(size, target) == target);
        utils::ps4::memory::set_large_pages(false);
        const auto maps_disabled = large_maps;
        assert(memory.map(target, 3) == target && large_maps == maps_disabled);
        utils::ps4::memory::set_large_pages(true);
        assert(memory.unmap(target, false));
    }
    while (!reservations.empty())
    {
        const auto remaining = reservations.back();
        assert(utils::ps4::memory::release(remaining.address, remaining.size));
    }
    {
        // Physical 2 MB alignment falls back to 16 KB when the pool is fragmented.
        fail_large_alloc = true;
        const auto fallbacks = utils::ps4::memory::get_statistics().large_fallbacks;
        shared_memory memory(size); assert(memory.valid() && allocated_bytes == size);
        assert(utils::ps4::memory::get_statistics().large_fallbacks == fallbacks + 3);
        fail_large_alloc = false;
        auto* a = static_cast<unsigned char*>(memory.map(nullptr, 3)); assert(a);
        assert(std::all_of(a, a + size, [](unsigned char v) { return v == 0; }));
    }
    while (!reservations.empty())
    {
        const auto remaining = reservations.back();
        assert(utils::ps4::memory::release(remaining.address, remaining.size));
    }
    assert(allocated_bytes == 0 && physicals.empty() && mappings.empty() && reservations.empty());
    assert(utils::ps4::memory::get_statistics().shared_direct_bytes == 0);
    char line[512];
    ps4_memory_report(line, sizeof(line));
    std::puts(line);
    std::puts("shared_memory_test: PASS (real host aliasing, 256 MB, rollback and cleanup, 2 MB chunks/views + fallbacks, accounting)");
}
