#define VMA_IMPLEMENTATION
#define VMA_VULKAN_VERSION 1002000

#include "util/atomic.hpp"
#include "Utilities/mutex.h"

// Protect some STL headers from macro (add more if it fails to compile)
#include <atomic>
#include <thread>
#include <memory>
#include <mutex>

// Replace VMA atomics with atomic_t
#define VMA_ATOMIC_UINT32 atomic_t<u32>
#define VMA_ATOMIC_UINT64 atomic_t<u64>
#define compare_exchange_strong compare_exchange
#define compare_exchange_weak compare_exchange

// Replace VMA mutex with shared_mutex
class VmaRWMutex
{
public:
	void LockRead() { m_mutex.lock_shared(); }
	void UnlockRead() { m_mutex.unlock_shared(); }
	bool TryLockRead() { return m_mutex.try_lock_shared(); }
	void LockWrite() { m_mutex.lock(); }
	void UnlockWrite() { m_mutex.unlock(); }
	bool TryLockWrite() { return m_mutex.try_lock(); }
	void Lock() { m_mutex.lock(); }
	void Unlock() { m_mutex.unlock(); }
	bool TryLock() { return m_mutex.try_lock(); }
private:
	shared_mutex m_mutex;
};

#define VMA_RW_MUTEX VmaRWMutex
#define VMA_MUTEX VmaRWMutex

#ifdef RPCS3_PS4
// 0.32: every heap of the console lives in one ~991 MiB orbis-drm GPU window. VMA sizes the blocks of a heap of up to
// 1 GiB at 1/8 of the (capped) heap: 96 MiB at the 768 MiB device cap, 56 MiB at 448 MiB, 82 MiB on the 661 MiB host
// heap, each reserving that much contiguous window for a few MiB of data (0.31 died on "out of device VA" for such
// blocks). With no heap counted as small, every heap uses VMA_DEFAULT_LARGE_HEAP_BLOCK_SIZE, since RPCS3 never sets
// preferredLargeHeapBlockSize. Requests above half a block (16 MiB) become dedicated allocations.
#define VMA_SMALL_HEAP_MAX_SIZE 0ull
#define VMA_DEFAULT_LARGE_HEAP_BLOCK_SIZE (32ull * 1024 * 1024)
#endif

#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(push, 0)
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wsuggest-override"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wstrict-aliasing"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#ifdef __clang__
#pragma clang diagnostic ignored "-Winconsistent-missing-override"
#pragma clang diagnostic ignored "-Wnullability-completeness"
#else
#pragma GCC diagnostic ignored "-Wsuggest-attribute=noreturn"
#endif
#endif
#include <vk_mem_alloc.h>
#if defined(_MSC_VER) && !defined(__clang__)
#pragma warning(pop)
#else
#pragma GCC diagnostic pop
#endif
