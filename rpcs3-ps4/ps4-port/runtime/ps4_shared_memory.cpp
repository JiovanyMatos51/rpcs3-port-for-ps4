#include "../../rpcs3/util/ps4_shared_memory.hpp"
#include "../../rpcs3/util/ps4_virtual_memory.hpp"
#include "../../rpcs3/ps4_launcher.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <limits>

#ifdef __ORBIS__
#include <orbis/libkernel.h>
#else
extern "C"
{
int sceKernelGetModuleList(std::uint32_t*, std::size_t, std::size_t*);
int sceKernelDlsym(int, const char*, void**);
std::size_t sceKernelGetDirectMemorySize();
int sceKernelAllocateDirectMemory(std::int64_t, std::int64_t, std::size_t, std::size_t, int, std::int64_t*);
int sceKernelMapDirectMemory(void**, std::size_t, int, int, std::int64_t, std::size_t);
int sceKernelReleaseDirectMemory(std::int64_t, std::size_t);
int sceKernelMprotect(const void*, std::size_t, int);
int sceKernelMunmap(void*, std::size_t);
int sceKernelReserveVirtualRange(void**, std::size_t, int, std::size_t);
}
#endif

namespace
{
constexpr std::size_t page = 0x4000, alignment = 0x10000, chunk_size = 0x200000;
bool failed(int error)
{
    errno = error ? static_cast<unsigned>(error) & 0xffff : EFAULT;
    return false;
}

bool enable_aliasing()
{
    static std::once_flag once;
    static int result = ENOSYS;
    std::call_once(once, []
    {
        // Resolve the real kernel export; the stock SDK has no stub for it.
        // Enumerate loaded modules rather than assuming a firmware-specific ID.
        std::uint32_t modules[256]{};
        std::size_t count = 0;
        result = sceKernelGetModuleList(modules, 256, &count);
        if (result) return;
        result = ENOSYS;
        for (std::size_t i = 0; i < std::min(count, std::size(modules)); ++i)
        {
            void* symbol = nullptr;
            if (sceKernelDlsym(modules[i], "sceKernelEnableDmemAliasing", &symbol) == 0 && symbol)
            {
                result = reinterpret_cast<int (*)()>(symbol)();
                break;
            }
        }
        ps4_trace_memory("sceKernelEnableDmemAliasing", nullptr, 0, 0, result, nullptr);
        if (!result) ps4_startup_checkpoint("shared direct-memory aliasing enabled");
    });
    return result == 0 || failed(result);
}
}

namespace utils::ps4
{
shared_memory::shared_memory(std::size_t size) : m_size(size)
{
    if (!size || size % alignment) { errno = EINVAL; return; }
    if (!enable_aliasing()) return;
    m_chunks.reserve((size - 1) / chunk_size + 1);
    // A partial chunk goes first, so in a view whose end is 2 MB-aligned every
    // other chunk is 2 MB-aligned in VA. Guest main memory (0xfff0000 bytes at
    // 0x10000, ending at 0x10000000) is such a view. Objects that are multiples of 2 MB are unaffected.
    const auto remainder = size % chunk_size;
    for (std::size_t offset = 0; offset < size;)
    {
        const auto length = offset == 0 && remainder ? remainder : chunk_size;
        std::int64_t physical = -1;
        int result = -1;
        if (length == chunk_size && memory::large_pages())
        {
            // 2 MB-aligned physical chunks let 2 MB-aligned views use large pages.
            result = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), length, chunk_size, 0, &physical);
            memory::account_large_page(result == 0);
        }
        if (result) result = sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), length, page, 0, &physical);
        ps4_trace_memory("shared AllocateDirectMemory", nullptr, length, 0, result, nullptr);
        if (result)
        {
            for (const auto& allocated : m_chunks)
            {
                sceKernelReleaseDirectMemory(allocated.physical, allocated.size);
                memory::account_shared_direct(-static_cast<long long>(allocated.size));
            }
            m_chunks.clear();
            failed(result);
            return;
        }
        m_chunks.push_back({physical, length});
        memory::account_shared_direct(static_cast<long long>(length));
        offset += length;
    }
    m_valid = true;
    if (size >= 0x1000000)
    {
        char stage[128];
        std::snprintf(stage, sizeof(stage), "shared direct-memory object allocated: size=0x%llx chunks=%u",
            static_cast<unsigned long long>(size), static_cast<unsigned>(m_chunks.size()));
        ps4_startup_checkpoint(stage);
    }
}

bool shared_memory::remove_view(const view& mapping)
{
    memory::unregister_shared_view(mapping.address, m_size);
    return mapping.restore_reservation ? memory::decommit(mapping.address, m_size) : memory::release(mapping.address, m_size);
}

shared_memory::~shared_memory()
{
    // Unmap every remaining alias before releasing its physical storage.
    for (const auto& mapping : m_views) remove_view(mapping);
    for (const auto& allocated : m_chunks)
    {
        const int result = sceKernelReleaseDirectMemory(allocated.physical, allocated.size);
        ps4_trace_memory("shared ReleaseDirectMemory", nullptr, allocated.size, 0, result, nullptr);
        memory::account_shared_direct(-static_cast<long long>(allocated.size));
    }
}

void* shared_memory::map(void* address, int protection, bool cow, bool overwrite)
{
    std::lock_guard lock(m_mutex);
    if (!m_valid) { errno = EINVAL; return nullptr; }
    // The only original COW user is the hook arena, handled separately.
    if (cow) { errno = ENOTSUP; return nullptr; }
    const auto numeric = reinterpret_cast<std::uintptr_t>(address);
    if (numeric % alignment || numeric > std::numeric_limits<std::uintptr_t>::max() - m_size)
        { errno = EINVAL; return nullptr; }
    if (std::any_of(m_views.begin(), m_views.end(), [address](const view& v) { return address && v.address == address; }))
        { errno = EBUSY; return nullptr; }

    const bool fixed = address != nullptr;
    if (!fixed || !overwrite)
    {
        address = memory::reserve(m_size, address);
        if (!address) return nullptr;
    }
    const view mapping{address, fixed};
    if (!memory::register_shared_view(address, m_size))
    {
        const int saved = errno;
        if (!fixed || !overwrite) memory::release(address, m_size);
        errno = saved;
        return nullptr;
    }
    std::size_t offset = 0;
    for (const auto& allocated : m_chunks)
    {
        void* const target = static_cast<char*>(address) + offset;
        void* mapped = target;
        // First view must be writable to zero recycled pages. Subsequent views
        // get their own permissions without changing any other alias.
        const int map_protection = !m_initialized || (protection & 4) ? 3 : protection;
        // Large pages need a whole chunk aligned to 2 MB both physically and in VA.
        // Executable views keep the 16 KB path, as private commits do.
        const bool large = allocated.size == chunk_size && !(protection & 4) && memory::large_pages()
            && !(allocated.physical & static_cast<std::int64_t>(chunk_size - 1))
            && !(reinterpret_cast<std::uintptr_t>(target) & (chunk_size - 1));
        int result = sceKernelMapDirectMemory(&mapped, allocated.size, map_protection, 0x10, allocated.physical, large ? chunk_size : page);
        if (large && result)
        {
            memory::account_large_page(false);
            mapped = target;
            result = sceKernelMapDirectMemory(&mapped, allocated.size, map_protection, 0x10, allocated.physical, page);
        }
        ps4_trace_memory("shared MapDirectMemory", target, allocated.size, 0x10, result, mapped);
        if (result || mapped != target)
        {
            const int saved = result ? static_cast<unsigned>(result) & 0xffff : EFAULT;
            if (!result && mapped != target) sceKernelMunmap(mapped, allocated.size);
            remove_view(mapping);
            errno = saved;
            return nullptr;
        }
        if (!m_initialized) std::memset(mapped, 0, allocated.size);
        offset += allocated.size;
    }
    if (!m_initialized || (protection & 4))
    {
        const int result = sceKernelMprotect(address, m_size, protection);
        ps4_trace_memory("shared Mprotect", address, m_size, protection, result, address);
        if (result)
        {
            const int saved = static_cast<unsigned>(result) & 0xffff;
            remove_view(mapping);
            errno = saved;
            return nullptr;
        }
    }
    m_initialized = true;
    m_views.push_back(mapping);
    if (m_size >= 0x1000000)
    {
        char stage[128];
        std::snprintf(stage, sizeof(stage), "shared direct-memory view mapped: address=%p size=0x%llx protection=0x%x",
            address, static_cast<unsigned long long>(m_size), protection);
        ps4_startup_checkpoint(stage);
    }
    return address;
}

bool shared_memory::unmap(void* address, bool restore_reservation)
{
    std::lock_guard lock(m_mutex);
    const auto found = std::find_if(m_views.begin(), m_views.end(), [address](const view& v) { return v.address == address; });
    if (found == m_views.end()) return failed(EINVAL);
    if (!remove_view({address, restore_reservation})) return false;
    m_views.erase(found);
    return true;
}
}
