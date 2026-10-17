#pragma once

#include "util/types.hpp"
#include "util/atomic.hpp"

#include <string>

namespace utils
{
#ifdef __ORBIS__
	namespace ps4 { class shared_memory; }
#endif
#ifdef _WIN32
	using native_handle = void*;
#else
	using native_handle = int;
#endif

	// Obtain system page size
	long get_page_size();

	// Memory protection type
	enum class protection
	{
		rw, // Read + write (default)
		ro, // Read only
		no, // No access
		wx, // Read + write + execute
		rx, // Read + execute
	};

	// Reserve `size` bytes of virtual memory and returns it.
	// The memory should be committed before usage.
	void* memory_reserve(usz size, void* use_addr, bool is_memory_mapping = false, bool can_be_jit = false);

	// Non-fixed address memory_reserve usage
	inline void* memory_reserve(usz size, bool can_be_jit = false)
	{
		return memory_reserve(size, nullptr, false, can_be_jit);
	}

	/**
	* Commit `size` bytes of virtual memory starting at pointer.
	* That is, bake reserved memory with physical memory.
	* pointer should belong to a range of reserved memory.
	*/
	void memory_commit(void* pointer, usz size, protection prot = protection::rw);

	// Decommit all memory committed via commit_page_memory.
	void memory_decommit(void* pointer, usz size, bool can_be_jit = false);

	// Decommit all memory and commit it again.
	void memory_reset(void* pointer, usz size, protection prot = protection::rw, bool can_be_jit = false);

	inline void memory_reset(void* pointer, usz size, bool can_be_jit = false)
	{
		return memory_reset(pointer, size, protection::rw, can_be_jit);
	}

	// Free memory after reserved by memory_reserve, should specify original size
	void memory_release(void* pointer, usz size);

	// Set memory protection
	void memory_protect(void* pointer, usz size, protection prot);

	// Lock pages in memory
	bool memory_lock(void* pointer, usz size);

	// Map file descriptor
	void* memory_map_fd(native_handle fd, usz size, protection prot);

	// Shared memory handle
	class shm
	{
#ifdef _WIN32
		void* m_handle{};
#elif defined(__ORBIS__)
		ps4::shared_memory* m_shared{}; // nullptr when the console refused the direct memory (see ps4_failed())
		int m_ps4_error = 0;
#else
		int m_file{};
#endif
		u32 m_flags{};
		u64 m_size{};
		atomic_t<void*> m_ptr{nullptr};
		std::string m_storage;

	public:
		explicit shm(u64 size, u32 flags = 0);

		// Construct with specified path as sparse file storage
		shm(u64 size, const std::string& storage);

		shm(const shm&) = delete;

		shm& operator=(const shm&) = delete;

		~shm();

		// Map shared memory
		u8* map(void* ptr, protection prot = protection::rw, bool cow = false) const;

		// Attempt to map shared memory fix fixed pointer
		u8* try_map(void* ptr, protection prot = protection::rw, bool cow = false) const;

		// Map shared memory over reserved memory region, which is unsafe (non-atomic) under Win32
		std::pair<u8*, std::string> map_critical(void* ptr, protection prot = protection::rw, bool cow = false);

		// Map shared memory into its own storage (not mapped by default)
		u8* map_self(protection prot = protection::rw);

		// Unmap shared memory
		void unmap(void* ptr) const;

		// Unmap shared memory, undoing map_critical
		void unmap_critical(void* ptr);

		// Unmap shared memory, undoing map_self()
		void unmap_self();

		// Get memory mapped by map_self()
		u8* get() const
		{
			return static_cast<u8*>(+m_ptr);
		}

		u64 size() const
		{
			return m_size;
		}

		// Flags are unspecified, consider it userdata
		u32 flags() const
		{
			return m_flags;
		}

		// Another userdata
		u64 info = 0;

#ifdef __ORBIS__
		// 0.31: true when creating the object failed, normally because the console had no direct memory left
		// (ps4_error() has the errno, and startup.log has the failure). Such an object maps nothing: map() and
		// try_map() return nullptr, map_critical() returns {nullptr, "ENOMEM"} and map_self() returns nullptr.
		// Owners check it right after construction and report ENOMEM to the guest.
		bool ps4_failed() const noexcept
		{
			return !m_shared;
		}

		// errno of the failed creation (ENOMEM for a refusal), 0 for a usable object
		int ps4_error() const noexcept
		{
			return m_ps4_error;
		}

		// Fault injection for the guest out-of-memory test: /data/rpcs3/test-shm-refuse-mb.txt holds a size in MiB.
		// While armed, every new object larger than that is refused as if direct memory had run out.
		// ps4_test_refuse_bytes() reads the file once per process (0 = no file). vm arms the test once the game is
		// running and disarms it when guest memory is closed.
		static u64 ps4_test_refuse_bytes() noexcept;
		static void ps4_arm_test_refusal(bool armed) noexcept;
#endif
	};
}
