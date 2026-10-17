#pragma once

#include <cstddef>

namespace utils::ps4::memory
{
// Reserve address space without charging the flexible-memory pool. Commit
// allocates direct physical pages only for the requested portion.
void* reserve(std::size_t size, void* hint = nullptr);
// Native user VA, beyond the automatically selected system-managed pool.
// Reservations remain private to their owning JIT engine and commit on demand.
inline constexpr std::size_t jit_virtual_begin = 0x2000000000ull;
inline constexpr std::size_t jit_virtual_end = 0x4000000000ull;
void* reserve_jit(std::size_t size);
bool commit(void* pointer, std::size_t size, int protection);
bool decommit(void* pointer, std::size_t size);
bool release(void* pointer, std::size_t size);
bool protect(void* pointer, std::size_t size, int protection);

// Shared mappings have independent physical ownership. Commit may adjust
// their permissions, but must never replace or zero their backing pages.
bool register_shared_view(void* pointer, std::size_t size);
void unregister_shared_view(void* pointer, std::size_t size);

// One private zero-filled arena, backed on first data access. Register/reset
// while guest threads are stopped. Execution and protection faults are not
// handled; callers must preserve the original fault-reporting path.
// A fault backs a 64 KB block, or the rest of its 2 MB region when a
// neighbouring page is already backed. It never touches pages that are already backed.
// handle_lazy_fault is called from the SIGSEGV handler: it allocates no heap,
// does no stdio and only takes the backend's own spin lock.
bool enable_lazy_zero(void* pointer, std::size_t size);
void disable_lazy_zero();
bool handle_lazy_fault(const void* address, unsigned long long error);

// Whole 2 MB chunks that are 2 MB-aligned in VA request 2 MB-aligned direct
// memory, so the kernel can map them with large pages. If that fails, they fall back to 16 KB alignment.
// Executable commits never use it. Disabled by default; ps4_main enables it when
// /data/rpcs3/large-pages.flag exists. true also clears the back-off taken after
// a failed aligned allocation.
void set_large_pages(bool enabled) noexcept;
bool large_pages() noexcept;

// Diagnostics. Each field is read lock-free, so a snapshot may be slightly
// inconsistent between fields. It never blocks, so any thread can call it.
struct statistics
{
    std::size_t slots_used, slots_high, slots_peak, slots_capacity;
    std::size_t reservations_used, reservations_capacity;
    std::size_t shared_views_used, shared_views_capacity;
    unsigned long long direct_bytes;        // private commits (allocation table)
    unsigned long long shared_direct_bytes; // utils::ps4::shared_memory objects
    unsigned long long lazy_bytes;          // committed inside the lazy-zero arena
    unsigned long long lazy_faults, lazy_blocks, lazy_failures;
    int lazy_last_error;                    // errno of the last failed lazy fault
    unsigned long long large_chunks, large_fallbacks;
    unsigned long long lock_contended, lock_yields, lock_sleeps;
};
statistics get_statistics() noexcept;

// Bookkeeping hooks for ps4_shared_memory.cpp (diagnostics only).
void account_shared_direct(long long delta) noexcept;
void account_large_page(bool aligned) noexcept;
}

// Writes one line of text into buffer, always NUL-terminated and with no trailing newline. The line
// lists allocation slots (used/high-water/peak/capacity), reservations, shared views,
// private and shared direct memory, lazy-arena bytes and faults, 2 MB chunks and
// lock contention. It does not take the backend lock, so any normal thread can call it.
void ps4_memory_report(char* buffer, unsigned size) noexcept;
