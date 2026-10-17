#include "../../rpcs3/util/ps4_virtual_memory.hpp"
#include "../../rpcs3/ps4_launcher.h"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>

#ifdef __ORBIS__
#include <orbis/libkernel.h>
#else
struct OrbisKernelVirtualQueryInfo;
#endif

namespace
{
// Native ABI from libkernel and shadPS4 kernel/memory.h. Avoid the SDK's
// unnamed query fields and keep this boundary independent of host headers.
struct query_info
{
    void* start;
    void* end;
    std::int64_t offset;
    std::int32_t protection, memory_type;
    std::uint8_t flags;
    char name[32];
};
static_assert(sizeof(query_info) == 72);
static_assert(offsetof(query_info, flags) == 32);

extern "C"
{
int sceKernelReserveVirtualRange(void**, std::size_t, int, std::size_t);
int sceKernelVirtualQuery(const void*, int, OrbisKernelVirtualQueryInfo*, std::size_t);
int sceKernelAllocateDirectMemory(std::int64_t, std::int64_t, std::size_t,
    std::size_t, int, std::int64_t*);
std::size_t sceKernelGetDirectMemorySize();
int sceKernelMapDirectMemory(void**, std::size_t, int, int, std::int64_t, std::size_t);
int sceKernelReleaseDirectMemory(std::int64_t, std::size_t);
int sceKernelMunmap(void*, std::size_t);
int sceKernelMprotect(const void*, std::size_t, int);
int sceKernelUsleep(unsigned int);
void scePthreadYield();
}

constexpr std::size_t page_size = 0x4000;
// Commit unit, large-page size and the largest lazy block.
constexpr std::size_t commit_chunk = 0x200000;
// Smallest lazy block: 4 native pages per fault and per allocation record.
constexpr std::size_t lazy_block = 0x10000;
constexpr int map_fixed = 0x10, map_no_overwrite = 0x80;
constexpr std::uint8_t committed_flag = 0x10;

struct allocation
{
    std::uintptr_t address;
    std::size_t size;
    std::int64_t physical;
};
struct address_range { std::uintptr_t start, end; };

constexpr bool is_empty(const allocation& entry) { return !entry.address; }
constexpr bool is_empty(const address_range& entry) { return !entry.start; }

// Counters are written while holding memory_lock but are read lock-free by
// get_statistics(), so every access to them is a relaxed atomic.
template <typename T> T relaxed_load(const T& value) { return __atomic_load_n(&value, __ATOMIC_RELAXED); }
template <typename T> void relaxed_store(T& target, T value) { __atomic_store_n(&target, value, __ATOMIC_RELAXED); }
template <typename T> void relaxed_add(T& target, T value) { __atomic_fetch_add(&target, value, __ATOMIC_RELAXED); }

struct counters
{
    unsigned long long direct_bytes, shared_direct_bytes, lazy_bytes;
    unsigned long long lazy_faults, lazy_blocks, lazy_failures;
    int lazy_last_error;
    unsigned long long large_chunks, large_fallbacks;
    unsigned long long lock_contended, lock_yields, lock_sleeps;
};

// No heap allocation or dynamic initialization: RPCS3 emits JIT code in
// global constructors before main. Serialize metadata and kernel operations.
constinit counters stats{};
// Bounds of the most recently enabled lazy arena. They are kept after disable so that
// the final decommit is still subtracted from lazy_bytes.
constinit std::uintptr_t lazy_account_start = 0, lazy_account_end = 0;

std::size_t overlap(std::uintptr_t first, std::uintptr_t last, std::uintptr_t start, std::uintptr_t end)
{
    const auto low = first > start ? first : start;
    const auto high = last < end ? last : end;
    return high > low ? high - low : 0;
}

void on_insert(const allocation& entry)
{
    relaxed_add(stats.direct_bytes, static_cast<unsigned long long>(entry.size));
    if (const auto lazy = overlap(entry.address, entry.address + entry.size, lazy_account_start, lazy_account_end))
        relaxed_add(stats.lazy_bytes, static_cast<unsigned long long>(lazy));
}
void on_erase(const allocation& entry)
{
    relaxed_add(stats.direct_bytes, 0ull - entry.size);
    if (const auto lazy = overlap(entry.address, entry.address + entry.size, lazy_account_start, lazy_account_end))
        relaxed_add(stats.lazy_bytes, 0ull - lazy);
}
void on_insert(const address_range&) {}
void on_erase(const address_range&) {}

// Fixed-capacity unordered table. Scans stop at the high-water mark: every
// index >= high is empty. Index order is irrelevant to the backend, and
// free_slot still returns the lowest empty index, as the old full scans did.
template <typename T, std::size_t N>
struct slot_table
{
    static constexpr std::size_t capacity = N;
    T entries[N];
    std::size_t high; // entries[high, N) are empty
    std::size_t hint; // entries[0, hint) are in use
    std::size_t used, peak;

    T* free_slot()
    {
        for (std::size_t i = hint; i < N; ++i)
            if (is_empty(entries[i])) { hint = i; return &entries[i]; }
        hint = N;
        return nullptr;
    }

    // Store a non-empty value in a slot from this table, which may already be in use.
    void put(T& slot, const T& value)
    {
        const auto index = static_cast<std::size_t>(&slot - entries);
        if (is_empty(slot))
        {
            relaxed_store(used, used + 1);
            if (used > peak) relaxed_store(peak, used);
        }
        else on_erase(slot);
        slot = value;
        on_insert(slot);
        if (index >= high) relaxed_store(high, index + 1);
        if (index == hint) hint = index + 1;
    }

    void clear(T& slot)
    {
        if (is_empty(slot)) return;
        const auto index = static_cast<std::size_t>(&slot - entries);
        on_erase(slot);
        slot = T{};
        relaxed_store(used, used - 1);
        if (index < hint) hint = index;
        if (index + 1 == high)
        {
            auto top = index;
            while (top && is_empty(entries[top - 1])) --top;
            relaxed_store(high, top);
        }
    }

    std::size_t available() const { return N - used; }
};

// 32768 records (768 KB): lazy faults now take one record per 64 KB-2 MB
// block instead of one per 16 KB page, and scans cost O(high), not O(N).
constinit slot_table<allocation, 32768> allocations{};
constinit slot_table<address_range, 8192> reservations{};
// Each guest shared-memory block takes three views (base, sudo, self). Games reached 3138-3585 of
// the old 8192 and kept growing; a full table makes vm::map throw. 32768 costs 384 KiB more BSS, and
// scans still stop at the high-water mark.
constinit slot_table<address_range, 32768> shared_views{};
constexpr std::size_t shared_views_warn = decltype(shared_views)::capacity / 4 * 3;
constinit unsigned char shared_views_warned = 0; // set under memory_lock
constinit unsigned char locked = 0;
constinit std::uintptr_t lazy_start = 0, lazy_end = 0;
// Off by default (0.25): partial mprotect/unmap inside a 2 MB direct mapping is unverified on the
// console. Opt in with /data/rpcs3/large-pages.flag (read in ps4_main before the first boot).
constinit unsigned char large_pages_enabled = 0;
// After a failed 2 MB-aligned allocation, the next chunks skip the attempt so
// a fragmented pool does not pay two syscalls per chunk. Guarded by the lock.
constinit unsigned large_backoff = 0;
constexpr unsigned large_backoff_chunks = 64;

inline void cpu_relax()
{
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#endif
}

// Holders run for one mprotect (~µs) up to one 2 MB commit (memset + 2
// syscalls). Spin on a plain load first (no cache-line ping-pong), then yield to
// equal-priority threads, then sleep so a lower-priority holder can run. The
// spin lock is async-signal-safe because the lazy fault handler takes it: a
// thread can fault in the arena only outside these critical sections.
constexpr unsigned lock_spin_limit = 256, lock_yield_limit = 16;

void lock_slow() noexcept
{
    relaxed_add(stats.lock_contended, 1ull);
    for (unsigned attempt = 0;; ++attempt)
    {
        if (attempt < lock_spin_limit) cpu_relax();
        else if (attempt < lock_spin_limit + lock_yield_limit)
        {
            relaxed_add(stats.lock_yields, 1ull);
            scePthreadYield();
        }
        else
        {
            relaxed_add(stats.lock_sleeps, 1ull);
            sceKernelUsleep(1);
        }
        if (!__atomic_load_n(&locked, __ATOMIC_RELAXED) && !__atomic_exchange_n(&locked, 1, __ATOMIC_ACQUIRE)) return;
    }
}

struct memory_lock
{
    memory_lock() noexcept { if (__atomic_exchange_n(&locked, 1, __ATOMIC_ACQUIRE)) lock_slow(); }
    ~memory_lock() { __atomic_store_n(&locked, 0, __ATOMIC_RELEASE); }
    memory_lock(const memory_lock&) = delete;
    memory_lock& operator=(const memory_lock&) = delete;
};

bool failure(int native_error)
{
    errno = native_error ? static_cast<unsigned int>(native_error) & 0xffff : EFAULT;
    return false;
}

bool rounded_size(std::size_t input, std::size_t& output)
{
    if (!input || input > std::numeric_limits<std::size_t>::max() - (page_size - 1))
        return failure(EINVAL);
    output = (input + page_size - 1) & ~(page_size - 1);
    return true;
}

bool range(void* pointer, std::size_t size, std::uintptr_t& start, std::uintptr_t& end)
{
    const auto address = reinterpret_cast<std::uintptr_t>(pointer);
    if (!address || !size || size > std::numeric_limits<std::uintptr_t>::max() - address)
        return failure(EINVAL);
    start = address & ~(page_size - 1);
    if (!start || address + size > std::numeric_limits<std::uintptr_t>::max() - (page_size - 1))
        return failure(EINVAL);
    end = (address + size + page_size - 1) & ~(page_size - 1);
    return true;
}

allocation* owner(std::uintptr_t address)
{
    for (std::size_t i = 0; i < allocations.high; ++i)
    {
        auto& entry = allocations.entries[i];
        if (entry.address && address >= entry.address && address - entry.address < entry.size) return &entry;
    }
    return nullptr;
}

template <std::size_t N>
address_range* containing(slot_table<address_range, N>& table, std::uintptr_t address)
{
    for (std::size_t i = 0; i < table.high; ++i)
    {
        auto& entry = table.entries[i];
        if (entry.start && address >= entry.start && address < entry.end) return &entry;
    }
    return nullptr;
}

bool remember_reservation(std::uintptr_t start, std::uintptr_t end)
{
    // Merge adjacent pieces after decommit, keeping metadata bounded even
    // when a large arena is repeatedly reset in small ranges.
    bool merged;
    do
    {
        merged = false;
        for (std::size_t i = 0; i < reservations.high; ++i)
        {
            auto& entry = reservations.entries[i];
            if (!entry.start || entry.start > end || entry.end < start) continue;
            if (entry.start < start) start = entry.start;
            if (entry.end > end) end = entry.end;
            reservations.clear(entry);
            merged = true;
        }
    } while (merged);
    if (auto* entry = reservations.free_slot()) { reservations.put(*entry, {start, end}); return true; }
    return failure(ENOMEM);
}

bool can_forget_reservations(std::uintptr_t start, std::uintptr_t end)
{
    std::size_t split = 0;
    for (std::size_t i = 0; i < reservations.high; ++i)
    {
        const auto& entry = reservations.entries[i];
        if (entry.start && entry.start < start && entry.end > end) ++split;
    }
    return split <= reservations.available() || failure(ENOMEM);
}

void forget_reservations(std::uintptr_t start, std::uintptr_t end)
{
    // high is re-read every iteration: a tail stored above the current index
    // starts at end and is skipped.
    for (std::size_t i = 0; i < reservations.high; ++i)
    {
        auto& entry = reservations.entries[i];
        if (!entry.start || entry.start >= end || entry.end <= start) continue;
        const auto old = entry;
        reservations.clear(entry);
        if (old.start < start) reservations.put(entry, {old.start, start});
        if (old.end > end)
        {
            if (auto* tail = reservations.free_slot()) reservations.put(*tail, {end, old.end});
        }
    }
}

bool protect_range(std::uintptr_t start, std::size_t size, int protection)
{
    const int result = sceKernelMprotect(reinterpret_cast<void*>(start), size, protection);
    ps4_trace_memory("sceKernelMprotect", reinterpret_cast<void*>(start), size, protection, result, reinterpret_cast<void*>(start));
    return result == 0 || failure(result);
}

bool want_large(std::uintptr_t start, std::size_t length, int protection)
{
    // GoldHEN promotes executable direct memory through Mprotect; keep code
    // on the 16 KB path. Data chunks must cover a whole aligned 2 MB.
    if (length != commit_chunk || (start & (commit_chunk - 1)) || (protection & 4)) return false;
    if (!relaxed_load(large_pages_enabled)) return false;
    if (large_backoff) { --large_backoff; return false; }
    return true;
}

void large_failed()
{
    relaxed_add(stats.large_fallbacks, 1ull);
    large_backoff = large_backoff_chunks;
}

// Back [start, start + length) with new direct memory, record it and zero it.
// The range must be reserved and not yet backed. Called with memory_lock held.
// trace=false keeps the signal-handler path free of stdio.
bool map_direct(std::uintptr_t start, std::size_t length, int protection, bool trace)
{
    allocation* record = allocations.free_slot();
    if (!record) return failure(ENOMEM);
    const auto search_end = static_cast<std::int64_t>(sceKernelGetDirectMemorySize());
    std::int64_t physical = -1;
    int result = -1;
    bool large = want_large(start, length, protection);
    if (large)
    {
        result = sceKernelAllocateDirectMemory(0, search_end, length, commit_chunk, 0, &physical);
        if (result) large_failed();
        large = !result && !(physical & static_cast<std::int64_t>(commit_chunk - 1));
    }
    if (result) result = sceKernelAllocateDirectMemory(0, search_end, length, page_size, 0, &physical);
    if (trace) ps4_trace_memory("sceKernelAllocateDirectMemory", reinterpret_cast<void*>(start), length, 0, result, nullptr);
    if (result) return failure(result);
    void* mapped = reinterpret_cast<void*>(start);
    // Map RW first. PS4 refuses EXEC at direct-map time; GoldHEN's
    // executable promotion uses the subsequent Mprotect operation.
    result = sceKernelMapDirectMemory(&mapped, length, 3, map_fixed, physical, large ? commit_chunk : page_size);
    if (large && result)
    {
        large_failed();
        large = false;
        mapped = reinterpret_cast<void*>(start);
        result = sceKernelMapDirectMemory(&mapped, length, 3, map_fixed, physical, page_size);
    }
    if (trace) ps4_trace_memory("sceKernelMapDirectMemory", reinterpret_cast<void*>(start), length, map_fixed, result, mapped);
    if (result || mapped != reinterpret_cast<void*>(start))
    {
        if (!result) sceKernelMunmap(mapped, length);
        sceKernelReleaseDirectMemory(physical, length);
        return failure(result);
    }
    allocations.put(*record, {start, length, physical});
    if (large) relaxed_add(stats.large_chunks, 1ull);
    // Direct pages can contain previous data. Match anonymous mmap's
    // zero-fill guarantee before narrowing or promoting protection.
    std::memset(mapped, 0, length);
    return true;
}

bool remove_range(std::uintptr_t start, std::uintptr_t end, bool preserve_reservation)
{
    for (std::size_t i = 0; i < shared_views.high; ++i)
    {
        const auto& view = shared_views.entries[i];
        if (view.start && view.start < end && view.end > start) return failure(EBUSY);
    }
    if (!can_forget_reservations(start, end)) return false;
    // A partial physical release can split an allocation into two survivors.
    // Ensure metadata capacity before changing the mappings.
    std::size_t required_slots = 0;
    for (std::size_t i = 0; i < allocations.high; ++i)
    {
        const auto& entry = allocations.entries[i];
        if (entry.address && entry.address < start && entry.address + entry.size > end) ++required_slots;
    }
    if (required_slots > allocations.available()) return failure(ENOMEM);

    const int unmapped = sceKernelMunmap(reinterpret_cast<void*>(start), end - start);
    ps4_trace_memory("sceKernelMunmap", reinterpret_cast<void*>(start), end - start, 0, unmapped, nullptr);
    if (unmapped) return failure(unmapped);
    forget_reservations(start, end);

    // high is re-read every iteration; tails start at end and are skipped.
    for (std::size_t i = 0; i < allocations.high; ++i)
    {
        auto& entry = allocations.entries[i];
        if (!entry.address || entry.address >= end || entry.address + entry.size <= start) continue;
        const auto old = entry;
        const auto first = old.address > start ? old.address : start;
        const auto last = old.address + old.size < end ? old.address + old.size : end;
        const int result = sceKernelReleaseDirectMemory(old.physical + static_cast<std::int64_t>(first - old.address), last - first);
        ps4_trace_memory("sceKernelReleaseDirectMemory", reinterpret_cast<void*>(first), last - first, 0, result, nullptr);
        if (result) return failure(result);
        allocations.clear(entry);
        if (first > old.address) allocations.put(entry, {old.address, first - old.address, old.physical});
        if (last < old.address + old.size)
        {
            allocation* tail = entry.address ? allocations.free_slot() : &entry;
            allocations.put(*tail, {last, old.address + old.size - last,
                old.physical + static_cast<std::int64_t>(last - old.address)});
        }
    }
    if (preserve_reservation)
    {
        void* pointer = reinterpret_cast<void*>(start);
        const int result = sceKernelReserveVirtualRange(&pointer, end - start,
            map_fixed | map_no_overwrite, page_size);
        ps4_trace_memory("sceKernelReserveVirtualRange(decommit)", reinterpret_cast<void*>(start), end - start,
            map_fixed | map_no_overwrite, result, pointer);
        if (result || pointer != reinterpret_cast<void*>(start)) return failure(result);
        if (!remember_reservation(start, end)) return false;
    }
    return true;
}

bool lazy_failure(int error)
{
    relaxed_add(stats.lazy_failures, 1ull);
    relaxed_store(stats.lazy_last_error, error);
    errno = error;
    return false;
}
}

namespace utils::ps4::memory
{
void* reserve(std::size_t size, void* hint)
{
    std::size_t length = 0;
    if (!rounded_size(size, length)) return nullptr;
    if (hint && reinterpret_cast<std::uintptr_t>(hint) % 0x10000) { failure(EINVAL); return nullptr; }
    if (hint && length > std::numeric_limits<std::uintptr_t>::max() - reinterpret_cast<std::uintptr_t>(hint))
        { failure(EINVAL); return nullptr; }
    memory_lock guard;
    void* pointer = hint;
    const int flags = hint ? map_fixed | map_no_overwrite : 0;
    const int result = sceKernelReserveVirtualRange(&pointer, length, flags, 0x10000);
    ps4_trace_memory("sceKernelReserveVirtualRange", hint, length, flags, result, pointer);
    if (result) { failure(result); return nullptr; }
    if (!pointer || reinterpret_cast<std::uintptr_t>(pointer) % 0x10000 || (hint && pointer != hint))
    {
        if (pointer) sceKernelMunmap(pointer, length);
        failure(EFAULT);
        return nullptr;
    }
    if (!remember_reservation(reinterpret_cast<std::uintptr_t>(pointer), reinterpret_cast<std::uintptr_t>(pointer) + length))
    {
        sceKernelMunmap(pointer, length);
        return nullptr;
    }
    return pointer;
}

void* reserve_jit(std::size_t size)
{
    std::size_t length = 0;
    if (!rounded_size(size, length)) return nullptr;
    if (length > jit_virtual_end - jit_virtual_begin) { failure(ENOMEM); return nullptr; }
    // A null hint chooses the small system-managed pool. Persistent LLVM
    // engines (including PRX loaded after boot) exhausted that pool even
    // after auxiliary arenas were freed. Use a bounded part of native user
    // VA above the guest, alias, hook and dispatch-table reservations.
    std::uintptr_t cursor = jit_virtual_begin;
    while (cursor <= jit_virtual_end - length)
    {
        {
            memory_lock guard;
            // Skip our owned reservations without repeated failing syscalls.
            // The kernel NO_OVERWRITE flag handles foreign mappings/races.
            bool overlap;
            do
            {
                overlap = false;
                for (std::size_t i = 0; i < reservations.high; ++i)
                {
                    const auto& entry = reservations.entries[i];
                    if (entry.start && entry.start < cursor + length && entry.end > cursor)
                    {
                        cursor = (entry.end + 0xffff) & ~std::uintptr_t(0xffff);
                        overlap = true;
                        break;
                    }
                }
            } while (overlap && cursor <= jit_virtual_end - length);
        }
        if (cursor > jit_virtual_end - length) break;
        if (void* pointer = reserve(length, reinterpret_cast<void*>(cursor))) return pointer;
        // Only a collision/exhausted region permits trying another address.
        // Permission, alignment and ABI errors must remain visible.
        if (errno != ENOMEM && errno != EEXIST) return nullptr;
        cursor += (length + 0xffff) & ~std::size_t(0xffff);
    }
    failure(ENOMEM);
    return nullptr;
}

bool commit(void* pointer, std::size_t size, int protection)
{
    if (!size) return true;
    std::uintptr_t start = 0, end = 0;
    if (!range(pointer, size, start, end)) return false;
    memory_lock guard;
    while (start < end)
    {
        auto length = end - start;
        if (const auto* existing = owner(start))
        {
            if (length > existing->address + existing->size - start) length = existing->address + existing->size - start;
            if (!protect_range(start, length, protection)) return false;
        }
        else if (const auto* shared = containing(shared_views, start))
        {
            if (length > shared->end - start) length = shared->end - start;
            if (!protect_range(start, length, protection)) return false;
        }
        else
        {
            const auto* reserved = containing(reservations, start);
            if (!reserved)
            {
                // Unknown mappings still require a kernel query. Never map
                // new physical pages over memory this backend does not own.
                query_info info{};
                const int query = sceKernelVirtualQuery(reinterpret_cast<void*>(start), 0,
                    reinterpret_cast<OrbisKernelVirtualQueryInfo*>(&info), sizeof(info));
                ps4_trace_memory("sceKernelVirtualQuery(commit)", reinterpret_cast<void*>(start), sizeof(info), 0, query, info.start);
                if (query) return failure(query);
                const auto limit = reinterpret_cast<std::uintptr_t>(info.end);
                if (reinterpret_cast<std::uintptr_t>(info.start) > start || limit <= start || limit % page_size)
                    return failure(EFAULT);
                if (!(info.flags & committed_flag)) return failure(EACCES);
                if (length > limit - start) length = limit - start;
                if (!protect_range(start, length, protection)) return false;
                start += length;
                continue;
            }
            if (length > reserved->end - start) length = reserved->end - start;
            // The next already-backed interval may start inside this commit.
            for (std::size_t i = 0; i < allocations.high; ++i)
            {
                const auto& allocated = allocations.entries[i];
                if (allocated.address > start && allocated.address - start < length) length = allocated.address - start;
            }
            for (std::size_t i = 0; i < shared_views.high; ++i)
            {
                const auto& shared = shared_views.entries[i];
                if (shared.start > start && shared.start - start < length) length = shared.start - start;
            }
            // Cut chunks at 2 MB-aligned addresses, so whole interior chunks are
            // eligible for 2 MB pages. Each chunk is still at most 2 MB.
            const auto to_boundary = commit_chunk - (start & (commit_chunk - 1));
            if (length > to_boundary) length = to_boundary;
            if (!map_direct(start, length, protection, true)) return false;
            if (protection != 3 && !protect_range(start, length, protection))
            {
                const int protection_error = errno;
                remove_range(start, start + length, true);
                errno = protection_error;
                return false;
            }
        }
        start += length;
    }
    return true;
}

bool decommit(void* pointer, std::size_t size)
{
    if (!size) return true;
    std::uintptr_t start = 0, end = 0;
    if (!range(pointer, size, start, end)) return false;
    memory_lock guard;
    return remove_range(start, end, true);
}

bool release(void* pointer, std::size_t size)
{
    if (!size) return true;
    std::uintptr_t start = 0, end = 0;
    if (!range(pointer, size, start, end)) return false;
    memory_lock guard;
    return remove_range(start, end, false);
}

bool protect(void* pointer, std::size_t size, int protection)
{
    if (!size) return true;
    std::uintptr_t start = 0, end = 0;
    if (!range(pointer, size, start, end)) return false;
    memory_lock guard;
    return protect_range(start, end - start, protection);
}

bool register_shared_view(void* pointer, std::size_t size)
{
    std::uintptr_t start = 0, end = 0;
    if (!range(pointer, size, start, end) || start != reinterpret_cast<std::uintptr_t>(pointer) || end - start != size)
        return failure(EINVAL);
    std::size_t warn_used = 0;
    {
        memory_lock guard;
        const auto* reserved = containing(reservations, start);
        if (!reserved || reserved->end < end) return failure(EACCES);
        for (std::size_t i = 0; i < shared_views.high; ++i)
        {
            const auto& entry = shared_views.entries[i];
            if (entry.start && entry.start < end && entry.end > start) return failure(EBUSY);
        }
        for (std::size_t i = 0; i < allocations.high; ++i)
        {
            const auto& allocated = allocations.entries[i];
            if (allocated.address && allocated.address < end && allocated.address + allocated.size > start) return failure(EBUSY);
        }
        auto* empty = shared_views.free_slot();
        if (!empty) return failure(ENOMEM);
        shared_views.put(*empty, {start, end});
        if (shared_views.used >= shared_views_warn && !shared_views_warned)
        {
            shared_views_warned = 1;
            warn_used = shared_views.used;
        }
    }
    // Logged after the lock is released: the lazy fault handler takes memory_lock, and the log
    // writer must never wait while holding it.
    if (warn_used)
    {
        char line[192];
        std::snprintf(line, sizeof(line), "PS4 memory: shared views at %llu/%llu (75%% of the table); a full table "
            "refuses guest memory mappings (ENOMEM)", static_cast<unsigned long long>(warn_used),
            static_cast<unsigned long long>(decltype(shared_views)::capacity));
        ps4_startup_checkpoint(line);
    }
    return true;
}

void unregister_shared_view(void* pointer, std::size_t size)
{
    memory_lock guard;
    const auto start = reinterpret_cast<std::uintptr_t>(pointer);
    for (std::size_t i = 0; i < shared_views.high; ++i)
    {
        auto& entry = shared_views.entries[i];
        if (entry.start == start && entry.end - start == size) { shared_views.clear(entry); return; }
    }
}

bool enable_lazy_zero(void* pointer, std::size_t size)
{
    std::uintptr_t start = 0, end = 0;
    if (!range(pointer, size, start, end) || start != reinterpret_cast<std::uintptr_t>(pointer)
        || end - start != size) return failure(EINVAL);
    memory_lock guard;
    if (__atomic_load_n(&lazy_start, __ATOMIC_ACQUIRE)) return failure(EBUSY);
    // Never replace committed/shared mappings with anonymous zero pages.
    for (auto cursor = start; cursor < end;)
    {
        query_info info{};
        const int result = sceKernelVirtualQuery(reinterpret_cast<void*>(cursor), 0,
            reinterpret_cast<OrbisKernelVirtualQueryInfo*>(&info), sizeof(info));
        ps4_trace_memory("sceKernelVirtualQuery(lazy zero)", reinterpret_cast<void*>(cursor), sizeof(info), 0, result, info.start);
        if (result) return failure(result);
        const auto region_end = reinterpret_cast<std::uintptr_t>(info.end);
        if (reinterpret_cast<std::uintptr_t>(info.start) > cursor || region_end <= cursor
            || region_end % page_size || info.flags & committed_flag || owner(cursor))
            return failure(EINVAL);
        cursor = region_end < end ? region_end : end;
    }
    lazy_account_start = start;
    lazy_account_end = end;
    unsigned long long inside = 0;
    for (std::size_t i = 0; i < allocations.high; ++i)
    {
        const auto& entry = allocations.entries[i];
        if (entry.address) inside += overlap(entry.address, entry.address + entry.size, start, end);
    }
    relaxed_store(stats.lazy_bytes, inside);
    __atomic_store_n(&lazy_end, end, __ATOMIC_RELAXED);
    __atomic_store_n(&lazy_start, start, __ATOMIC_RELEASE);
    ps4_trace_memory("PS4 lazy zero arena enabled", pointer, size, 3, 0, pointer);
    return true;
}

void disable_lazy_zero()
{
    memory_lock guard;
    __atomic_store_n(&lazy_start, 0, __ATOMIC_RELEASE);
}

bool handle_lazy_fault(const void* address, unsigned long long error)
{
    // x86 page-fault bits: present/protection=1, instruction fetch=16.
    // Only missing data pages in the explicitly registered arena are ours.
    if (error & 0x11) return false;
    const auto start = __atomic_load_n(&lazy_start, __ATOMIC_ACQUIRE);
    const auto end = __atomic_load_n(&lazy_end, __ATOMIC_RELAXED);
    const auto fault = reinterpret_cast<std::uintptr_t>(address);
    if (!start || fault < start || fault >= end) return false;

    // SIGSEGV context: no heap, stdio or trace output below. The only lock is
    // memory_lock, which no thread holds while touching arena memory.
    const int saved_errno = errno;
    memory_lock guard;
    relaxed_add(stats.lazy_faults, 1ull);
    const auto page = fault & ~(page_size - 1);
    const auto* reserved = containing(reservations, page);
    if (!reserved) return lazy_failure(EACCES);

    // Limits: the 2 MB region of the page, inside the arena and reservation.
    auto floor = page & ~(commit_chunk - 1);
    auto ceiling = floor + commit_chunk;
    if (floor < start) floor = start;
    if (floor < reserved->start) floor = reserved->start;
    if (ceiling < floor || ceiling > end) ceiling = end;
    if (ceiling > reserved->end) ceiling = reserved->end;
    // Default: the 64 KB block holding the page.
    auto low = page & ~(lazy_block - 1);
    auto high = low + lazy_block;
    if (low < floor) low = floor;
    if (high > ceiling) high = ceiling;

    // One pass: detect a race (page already backed), backed neighbours of the
    // block (sequential use), and the uncommitted run around the page.
    const bool check_before = low > start, check_after = high < end;
    const auto before = low - page_size, after = high;
    bool before_backed = false, after_backed = false;
    auto run_low = floor, run_high = ceiling;
    const auto visit = [&](std::uintptr_t first, std::uintptr_t last)
    {
        if (check_before && before >= first && before < last) before_backed = true;
        if (check_after && after >= first && after < last) after_backed = true;
        if (last <= page) { if (last > run_low) run_low = last; }
        else if (first > page && first < run_high) run_high = first;
    };
    for (std::size_t i = 0; i < allocations.high; ++i)
    {
        const auto& entry = allocations.entries[i];
        if (!entry.address) continue;
        if (page >= entry.address && page - entry.address < entry.size)
        {
            // Another thread backed it after this fault: retry the access.
            errno = saved_errno;
            return true;
        }
        visit(entry.address, entry.address + entry.size);
    }
    for (std::size_t i = 0; i < shared_views.high; ++i)
    {
        const auto& view = shared_views.entries[i];
        if (!view.start) continue;
        // Never replace a shared alias. Reporting the fault is better than retrying it forever.
        if (page >= view.start && page < view.end) return lazy_failure(EBUSY);
        visit(view.start, view.end);
    }
    // Sequential use: grow to the 2 MB region boundary on the far side.
    if (before_backed) high = ceiling;
    if (after_backed) low = floor;
    if (low < run_low) low = run_low;
    if (high > run_high) high = run_high;

    if (!map_direct(low, high - low, 3, false))
    {
        // Fragmented physical memory: back only the faulting page.
        if (high - low == page_size || !map_direct(page, page_size, 3, false)) return lazy_failure(errno);
    }
    relaxed_add(stats.lazy_blocks, 1ull);
    errno = saved_errno;
    return true;
}

void set_large_pages(bool enabled) noexcept
{
    memory_lock guard;
    relaxed_store(large_pages_enabled, static_cast<unsigned char>(enabled));
    large_backoff = 0;
}

bool large_pages() noexcept
{
    return relaxed_load(large_pages_enabled) != 0;
}

statistics get_statistics() noexcept
{
    statistics result{};
    result.slots_used = relaxed_load(allocations.used);
    result.slots_high = relaxed_load(allocations.high);
    result.slots_peak = relaxed_load(allocations.peak);
    result.slots_capacity = allocations.capacity;
    result.reservations_used = relaxed_load(reservations.used);
    result.reservations_capacity = reservations.capacity;
    result.shared_views_used = relaxed_load(shared_views.used);
    result.shared_views_capacity = shared_views.capacity;
    result.direct_bytes = relaxed_load(stats.direct_bytes);
    result.shared_direct_bytes = relaxed_load(stats.shared_direct_bytes);
    result.lazy_bytes = relaxed_load(stats.lazy_bytes);
    result.lazy_faults = relaxed_load(stats.lazy_faults);
    result.lazy_blocks = relaxed_load(stats.lazy_blocks);
    result.lazy_failures = relaxed_load(stats.lazy_failures);
    result.lazy_last_error = relaxed_load(stats.lazy_last_error);
    result.large_chunks = relaxed_load(stats.large_chunks);
    result.large_fallbacks = relaxed_load(stats.large_fallbacks);
    result.lock_contended = relaxed_load(stats.lock_contended);
    result.lock_yields = relaxed_load(stats.lock_yields);
    result.lock_sleeps = relaxed_load(stats.lock_sleeps);
    return result;
}

void account_shared_direct(long long delta) noexcept
{
    relaxed_add(stats.shared_direct_bytes, static_cast<unsigned long long>(delta));
}

void account_large_page(bool aligned) noexcept
{
    relaxed_add(aligned ? stats.large_chunks : stats.large_fallbacks, 1ull);
}
}

void ps4_memory_report(char* buffer, unsigned size) noexcept
{
    if (!buffer || !size) return;
    const auto s = utils::ps4::memory::get_statistics();
    using ull = unsigned long long;
    std::snprintf(buffer, size,
        "PS4 memory: slots used=%llu high=%llu peak=%llu cap=%llu, reservations=%llu/%llu, views=%llu/%llu, "
        "direct private=%lluM shared=%lluM, lazy=%lluK blocks=%llu faults=%llu failed=%llu err=%d, "
        "2M chunks=%llu fallbacks=%llu, lock contended=%llu yields=%llu sleeps=%llu",
        static_cast<ull>(s.slots_used), static_cast<ull>(s.slots_high), static_cast<ull>(s.slots_peak),
        static_cast<ull>(s.slots_capacity), static_cast<ull>(s.reservations_used),
        static_cast<ull>(s.reservations_capacity), static_cast<ull>(s.shared_views_used),
        static_cast<ull>(s.shared_views_capacity), s.direct_bytes >> 20, s.shared_direct_bytes >> 20,
        s.lazy_bytes >> 10, s.lazy_blocks, s.lazy_faults, s.lazy_failures, s.lazy_last_error,
        s.large_chunks, s.large_fallbacks, s.lock_contended, s.lock_yields, s.lock_sleeps);
}
