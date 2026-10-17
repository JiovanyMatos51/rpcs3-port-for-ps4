#include "stdafx.h"
#include "util/vm.hpp"
#include "util/asm.hpp"
#ifdef _WIN32
#include "Utilities/File.h"
#include "util/dyn_lib.hpp"
#include "Utilities/lockless.h"
#include <Windows.h>
#include <span>
#else
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <sys/types.h>
#endif

#ifdef __ORBIS__
#include <orbis/libkernel.h>
#include <cstdio>
#include "ps4_launcher.h"
#include "util/ps4_virtual_memory.hpp"
#include "util/ps4_shared_memory.hpp"
#endif

#if defined(__FreeBSD__) && !defined(__ORBIS__)
#include <sys/sysctl.h>
#include <vm/vm_param.h>
#endif

#ifdef __linux__
#include <sys/syscall.h>
#include <linux/memfd.h>

#ifdef __NR_memfd_create
#elif __x86_64__
#define __NR_memfd_create 319
#elif ARCH_ARM64
#define __NR_memfd_create 279
#endif

static int memfd_create_(const char *name, uint flags)
{
	return syscall(__NR_memfd_create, name, flags);
}
#elif defined(__FreeBSD__) && !defined(__ORBIS__)
# if __FreeBSD__ < 13
// XXX Drop after FreeBSD 12.* reaches EOL on 2024-06-30
#define MFD_CLOEXEC O_CLOEXEC
#define memfd_create_(name, flags) shm_open(SHM_ANON, O_RDWR | flags, 0600)
# else
#define memfd_create_ memfd_create
# endif
#endif

namespace utils
{
#ifndef _WIN32
	static void* native_mmap(void* addr, usz size, int prot, int flags, int fd, off_t offset)
	{
#ifdef __ORBIS__
		if (fd < 0 && prot == PROT_NONE && (flags & MAP_ANON))
		{
			if (flags & MAP_FIXED)
				return ps4::memory::decommit(addr, size) ? addr : MAP_FAILED;
			const auto reserved = ps4::memory::reserve(size, addr);
			return reserved ? reserved : MAP_FAILED;
		}
		void* mapped = nullptr;
		const int result = ::sceKernelMmap(addr, size, prot, flags, fd, offset, &mapped);
		ps4_trace_memory("sceKernelMmap", addr, size, flags, result, mapped);
		return result == 0 ? mapped : MAP_FAILED;
#else
		return ::mmap(addr, size, prot, flags, fd, offset);
#endif
	}

	static int native_munmap(void* addr, usz size)
	{
#ifdef __ORBIS__
		return ::sceKernelMunmap(addr, size) == 0 ? 0 : -1;
#else
		return ::munmap(addr, size);
#endif
	}

	static int native_mprotect(void* addr, usz size, int prot)
	{
#ifdef __ORBIS__
		return ps4::memory::protect(addr, size, prot) ? 0 : -1;
#else
		return ::mprotect(addr, size, prot);
#endif
	}
#endif

#ifdef MAP_NORESERVE
	constexpr int c_map_noreserve = MAP_NORESERVE;
#else
	[[maybe_unused]] constexpr int c_map_noreserve = 0;
#endif

#ifdef MADV_FREE
	[[maybe_unused]] constexpr int c_madv_free = MADV_FREE;
#elif defined(MADV_DONTNEED)
	[[maybe_unused]] constexpr int c_madv_free = MADV_DONTNEED;
#else
	[[maybe_unused]] constexpr int c_madv_free = 0;
#endif

#ifdef MADV_HUGEPAGE
	[[maybe_unused]] constexpr int c_madv_hugepage = MADV_HUGEPAGE;
#else
	[[maybe_unused]] constexpr int c_madv_hugepage = 0;
#endif

#if defined(MADV_DONTDUMP) && defined(MADV_DODUMP)
	[[maybe_unused]] constexpr int c_madv_no_dump = MADV_DONTDUMP;
	[[maybe_unused]] constexpr int c_madv_dump = MADV_DODUMP;
#elif defined(MADV_NOCORE) && defined(MADV_CORE)
	[[maybe_unused]] constexpr int c_madv_no_dump = MADV_NOCORE;
	[[maybe_unused]] constexpr int c_madv_dump = MADV_CORE;
#else
	[[maybe_unused]] constexpr int c_madv_no_dump = 0;
	[[maybe_unused]] constexpr int c_madv_dump = 0;
#endif

#if defined(MFD_HUGETLB) && defined(MFD_HUGE_2MB)
	constexpr int c_mfd_huge_2mb = MFD_HUGETLB | MFD_HUGE_2MB;
#elif defined(__linux__) || (defined(__FreeBSD__) && !defined(__ORBIS__))
	constexpr int c_mfd_huge_2mb = 0;
#endif

#ifdef _WIN32
	DYNAMIC_IMPORT("KernelBase.dll", VirtualAlloc2, PVOID(HANDLE Process, PVOID Base, SIZE_T Size, ULONG AllocType, ULONG Prot, MEM_EXTENDED_PARAMETER*, ULONG));
	DYNAMIC_IMPORT("KernelBase.dll", MapViewOfFile3, PVOID(HANDLE Handle, HANDLE Process, PVOID Base, ULONG64 Off, SIZE_T ViewSize, ULONG AllocType, ULONG Prot, MEM_EXTENDED_PARAMETER*, ULONG));
	DYNAMIC_IMPORT("KernelBase.dll", UnmapViewOfFile2, BOOL(HANDLE Process, PVOID BaseAddress, ULONG UnmapFlags));

	bool has_win10_memory_mapping_api()
	{
		return VirtualAlloc2 && MapViewOfFile3 && UnmapViewOfFile2;
	}

	struct map_info_t
	{
		u64 addr = 0;
		u64 size = 0;
		atomic_t<u8> state{};
	};

	lf_array<map_info_t, 32> s_is_mapping{};

	bool is_memory_mappping_memory(u64 addr)
	{
		if (!addr)
		{
			return false;
		}

		return s_is_mapping.for_each([addr](const map_info_t& info)
		{
			if (info.state == 1)
			{
				if (addr >= info.addr && addr < info.addr + info.size)
				{
					return true;
				}
			}

			return false;
		}).second;
	}

	u64 unmap_mappping_memory(u64 addr, u64 size)
	{
		if (!addr || !size)
		{
			return 0;
		}

		return s_is_mapping.for_each([addr, size](map_info_t& info) -> u64
		{
			if (info.state == 1)
			{
				if (addr == info.addr && size == info.size)
				{
					if (info.state.compare_and_swap_test(1, 0))
					{
						return info.size;
					}
				}
			}

			return 0;
		}).second;
	}

	bool map_mappping_memory(u64 addr, u64 size)
	{
		if (!addr || !size)
		{
			return false;
		}

		ensure(s_is_mapping.for_each([addr, size](map_info_t& info)
		{
			if (!info.addr && info.state.compare_and_swap_test(0, 2))
			{
				info.addr = addr;
				info.size = size;
				info.state = 1;
				return true;
			}

			return false;
		}, false).second);

		return true;
	}

	bool is_memory_mappping_memory(const void* addr)
	{
		return is_memory_mappping_memory(reinterpret_cast<u64>(addr));
	}
#endif

	long get_page_size()
	{
		static const long r = []() -> long
		{
#ifdef _WIN32
			SYSTEM_INFO info;
			::GetSystemInfo(&info);
			return info.dwPageSize;
#elif defined(__ORBIS__)
			return 0x4000;
#else
			return ::sysconf(_SC_PAGESIZE);
#endif
		}();

		return ensure(r, FN(((x & (x - 1)) == 0 && x > 0 && x <= 0x10000)));
	}

	// Convert memory protection (internal)
	static auto operator +(protection prot)
	{
#ifdef _WIN32
		DWORD _prot = PAGE_NOACCESS;
		switch (prot)
		{
		case protection::rw: _prot = PAGE_READWRITE; break;
		case protection::ro: _prot = PAGE_READONLY; break;
		case protection::no: break;
		case protection::wx: _prot = PAGE_EXECUTE_READWRITE; break;
		case protection::rx: _prot = PAGE_EXECUTE_READ; break;
		}
#else
		int _prot = PROT_NONE;
		switch (prot)
		{
		case protection::rw: _prot = PROT_READ | PROT_WRITE; break;
		case protection::ro: _prot = PROT_READ; break;
		case protection::no: break;
		case protection::wx: _prot = PROT_READ | PROT_WRITE | PROT_EXEC; break;
		case protection::rx: _prot = PROT_READ | PROT_EXEC; break;
		}
#endif

		return _prot;
	}

	void* memory_reserve(usz size, void* use_addr, [[maybe_unused]] bool is_memory_mapping, [[maybe_unused]] bool can_be_jit)
	{
#ifdef __ORBIS__
		return ps4::memory::reserve(size, use_addr);
#elif defined(_WIN32)
		if (is_memory_mapping && has_win10_memory_mapping_api())
		{
			if (auto ptr = VirtualAlloc2(nullptr, use_addr, size, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, PAGE_NOACCESS, nullptr, 0))
			{
				map_mappping_memory(reinterpret_cast<u64>(ptr), size);
				return ptr;
			}

			return nullptr;
		}

		return ::VirtualAlloc(use_addr, size, MEM_RESERVE, PAGE_NOACCESS);
#else
		if (use_addr && reinterpret_cast<uptr>(use_addr) % 0x10000)
		{
			return nullptr;
		}

		[[maybe_unused]] const auto orig_size = size;

		if (!use_addr)
		{
			// Hack: Ensure aligned 64k allocations
			size += 0x10000;
		}

		// Memory mapping regions will be replaced by file-backed MAP_FIXED mappings
		// (via shm::map), which is incompatible with MAP_JIT. Only use MAP_JIT for
		// non-mapping regions that need JIT executable support.
#ifdef __APPLE__
		const int jit_flag = is_memory_mapping || !can_be_jit ? 0 : MAP_JIT;
#ifdef ARCH_ARM64
		auto ptr = native_mmap(use_addr, size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE | jit_flag | c_map_noreserve, -1, 0);
#else
		auto ptr = native_mmap(use_addr, size, PROT_NONE, MAP_ANON | MAP_PRIVATE | jit_flag | c_map_noreserve, -1, 0);
#endif
#else
		auto ptr = native_mmap(use_addr, size, PROT_NONE, MAP_ANON | MAP_PRIVATE | c_map_noreserve, -1, 0);
#endif

		if (ptr == reinterpret_cast<void*>(uptr{umax}))
		{
			return nullptr;
		}

		if (use_addr && ptr != use_addr)
		{
			native_munmap(ptr, size);
			return nullptr;
		}

		if (!use_addr && ptr)
		{
			// Continuation of the hack above
			const auto misalign = reinterpret_cast<uptr>(ptr) % 0x10000;
			native_munmap(ptr, 0x10000 - misalign);

			if (misalign)
			{
				native_munmap(static_cast<u8*>(ptr) + size - misalign, misalign);
			}

			ptr = static_cast<u8*>(ptr) + (0x10000 - misalign);
		}

#ifndef __ORBIS__
		if constexpr (c_madv_hugepage != 0)
		{
			if (orig_size % 0x200000 == 0)
			{
				::madvise(ptr, orig_size, c_madv_hugepage);
			}
		}

		if constexpr (c_madv_no_dump != 0)
		{
			ensure(::madvise(ptr, orig_size, c_madv_no_dump) != -1);
		}
		else
		{
			ensure(::madvise(ptr, orig_size, c_madv_free) != -1);
		}

#endif
		return ptr;
#endif
	}

	void memory_commit(void* pointer, usz size, protection prot)
	{
		if (!size)
		{
			return;
		}

#ifdef _WIN32
		ensure(::VirtualAlloc(pointer, size, MEM_COMMIT, +prot));
#elif defined(__ORBIS__)
		const bool committed = ps4::memory::commit(pointer, size, +prot);
		if (!committed)
		{
			const int saved_error = errno;
			ps4_trace_memory("memory_commit failed", pointer, size, +prot, saved_error, nullptr);
			errno = saved_error;
		}
		ensure(committed);
#else
		const u64 ptr64 = reinterpret_cast<u64>(pointer);
		ensure(native_mprotect(reinterpret_cast<void*>(ptr64 & -get_page_size()), size + (ptr64 & (get_page_size() - 1)), +prot) != -1);

#ifndef __ORBIS__
		// These desktop paging/core-dump hints are not supported by the PS4
		// kernel. The protection change above is the required operation.
		if constexpr (c_madv_dump != 0)
		{
			ensure(::madvise(reinterpret_cast<void*>(ptr64 & -get_page_size()), size + (ptr64 & (get_page_size() - 1)), c_madv_dump) != -1);
		}
		else
		{
			ensure(::madvise(reinterpret_cast<void*>(ptr64 & -get_page_size()), size + (ptr64 & (get_page_size() - 1)), MADV_WILLNEED) != -1);
		}
#endif
#endif
	}

	void memory_decommit(void* pointer, usz size, [[maybe_unused]] bool can_be_jit)
	{
		if (!size)
		{
			return;
		}

#ifdef _WIN32
		ensure(::VirtualFree(pointer, size, MEM_DECOMMIT));
#elif defined(__ORBIS__)
		ensure(ps4::memory::decommit(pointer, size));
#else
		const u64 ptr64 = reinterpret_cast<u64>(pointer);
#if defined(__APPLE__) && defined(ARCH_ARM64)
		// Hack: on macOS, Apple explicitly fails mmap if you combine MAP_FIXED and MAP_JIT.
		// So we unmap the space and just hope it maps to the same address we got before instead.
		// The Xcode manpage says the pointer is a hint and the OS will try to map at the hint location
		// so this isn't completely undefined behavior.
		ensure(native_munmap(pointer, size) != -1);
		ensure(native_mmap(pointer, size, PROT_NONE,  MAP_ANON | MAP_PRIVATE | (can_be_jit ? MAP_JIT : 0), -1, 0) == pointer);
#else
		ensure(native_mmap(pointer, size, PROT_NONE, MAP_FIXED | MAP_ANON | MAP_PRIVATE | c_map_noreserve, -1, 0) != reinterpret_cast<void*>(uptr{umax}));
#endif

#ifndef __ORBIS__
		if constexpr (c_madv_no_dump != 0)
		{
			ensure(::madvise(reinterpret_cast<void*>(ptr64 & -get_page_size()), size + (ptr64 & (get_page_size() - 1)), c_madv_no_dump) != -1);
		}
		else
		{
			ensure(::madvise(reinterpret_cast<void*>(ptr64 & -get_page_size()), size + (ptr64 & (get_page_size() - 1)), c_madv_free) != -1);
		}
#endif
#endif
	}

	void memory_reset(void* pointer, usz size, protection prot, [[maybe_unused]] bool can_be_jit)
	{
		if (!size)
		{
			return;
		}

#ifdef _WIN32
		memory_decommit(pointer, size);
		memory_commit(pointer, size, prot);
#elif defined(__ORBIS__)
		ensure(ps4::memory::decommit(pointer, size));
		ensure(ps4::memory::commit(pointer, size, +prot));
#else
		const u64 ptr64 = reinterpret_cast<u64>(pointer);
#if defined(__APPLE__) && defined(ARCH_ARM64)
		ensure(native_munmap(pointer, size) != -1);
		ensure(native_mmap(pointer, size, +prot,  MAP_ANON | MAP_PRIVATE | (can_be_jit ? MAP_JIT : 0), -1, 0) == pointer);
#else
		ensure(native_mmap(pointer, size, +prot, MAP_FIXED | MAP_ANON | MAP_PRIVATE, -1, 0) != reinterpret_cast<void*>(uptr{umax}));
#endif

#ifndef __ORBIS__
		if constexpr (c_madv_hugepage != 0)
		{
			if (size % 0x200000 == 0)
			{
				::madvise(reinterpret_cast<void*>(ptr64 & -get_page_size()), size + (ptr64 & (get_page_size() - 1)), c_madv_hugepage);
			}
		}

		if constexpr (c_madv_dump != 0)
		{
			ensure(::madvise(reinterpret_cast<void*>(ptr64 & -get_page_size()), size + (ptr64 & (get_page_size() - 1)), c_madv_dump) != -1);
		}
		else
		{
			ensure(::madvise(reinterpret_cast<void*>(ptr64 & -get_page_size()), size + (ptr64 & (get_page_size() - 1)), MADV_WILLNEED) != -1);
		}
#endif
#endif
	}

	void memory_release(void* pointer, usz size)
	{
		if (!size)
		{
			return;
		}

#ifdef _WIN32
		unmap_mappping_memory(reinterpret_cast<u64>(pointer), size);
		ensure(::VirtualFree(pointer, 0, MEM_RELEASE));
#elif defined(__ORBIS__)
		ensure(ps4::memory::release(pointer, size));
#else
		ensure(native_munmap(pointer, size) != -1);
#endif
	}

	void memory_protect(void* pointer, usz size, protection prot)
	{
		if (!size)
		{
			return;
		}

#ifdef _WIN32

		DWORD old;
		if (::VirtualProtect(pointer, size, +prot, &old))
		{
			return;
		}

		for (u64 addr = reinterpret_cast<u64>(pointer), end = addr + size; addr < end;)
		{
			const u64 boundary = (addr + 0x10000) & -0x10000;
			const u64 block_size = std::min(boundary, end) - addr;

			if (!::VirtualProtect(reinterpret_cast<LPVOID>(addr), block_size, +prot, &old))
			{
				fmt::throw_exception("VirtualProtect failed (%p, 0x%x, addr=0x%x, error=%s)", pointer, size, addr, fmt::win_error{GetLastError(), nullptr});
			}

			// Next region
			addr += block_size;
		}
#else
		const u64 ptr64 = reinterpret_cast<u64>(pointer);
		ensure(native_mprotect(reinterpret_cast<void*>(ptr64 & -get_page_size()), size + (ptr64 & (get_page_size() - 1)), +prot) != -1);
#endif
	}

	bool memory_lock(void* pointer, usz size)
	{
		if (!size)
		{
			return true;
		}

#ifdef _WIN32
		return ::VirtualLock(pointer, size);
#else
		return !::mlock(pointer, size);
#endif
	}

	void* memory_map_fd([[maybe_unused]] native_handle fd, [[maybe_unused]] usz size, [[maybe_unused]] protection prot)
	{
#ifdef _WIN32
		// TODO
		return nullptr;
#else
		const auto result = native_mmap(nullptr, size, +prot, MAP_SHARED, fd, 0);

		if (result == reinterpret_cast<void*>(uptr{umax}))
		{
			[[unlikely]] return nullptr;
		}

		return result;
#endif
	}

#ifdef __ORBIS__
	constexpr char c_ps4_shm_refuse_file[] = "/data/rpcs3/test-shm-refuse-mb.txt";

	static atomic_t<bool> s_ps4_shm_refuse_armed{false};
	static atomic_t<u32> s_ps4_shm_failures{0};

	// Reads the refusal size of the out-of-memory test once. Plain open/read: no allocation, no exceptions.
	static u64 ps4_read_shm_refuse_bytes() noexcept
	{
		const int fd = ::open(c_ps4_shm_refuse_file, O_RDONLY);

		if (fd < 0)
		{
			return 0;
		}

		char text[32]{};
		const auto n = ::read(fd, text, sizeof(text) - 1);
		::close(fd);

		long i = 0;
		while (i < n && (text[i] == ' ' || text[i] == '\t' || text[i] == '\r' || text[i] == '\n')) i++;

		u64 mib = 0;
		for (; i < n && text[i] >= '0' && text[i] <= '9' && mib < (1ull << 30); i++) mib = mib * 10 + static_cast<u64>(text[i] - '0');

		char line[160];
		if (!mib)
		{
			std::snprintf(line, sizeof(line), "shm: %s ignored (expected a size in MiB greater than 0)", c_ps4_shm_refuse_file);
			ps4_startup_checkpoint(line);
			return 0;
		}

		std::snprintf(line, sizeof(line), "shm: %s: objects above %llu MiB are refused once the game runs (test only)",
			c_ps4_shm_refuse_file, static_cast<unsigned long long>(mib));
		ps4_startup_checkpoint(line);
		return mib << 20;
	}

	u64 shm::ps4_test_refuse_bytes() noexcept
	{
		static const u64 s_bytes = ps4_read_shm_refuse_bytes();
		return s_bytes;
	}

	void shm::ps4_arm_test_refusal(bool armed) noexcept
	{
		// Called for every guest allocation while the test file exists: read before writing the shared flag
		if (s_ps4_shm_refuse_armed.load() == armed)
		{
			return;
		}

		if (s_ps4_shm_refuse_armed.exchange(armed) != armed && ps4_test_refuse_bytes())
		{
			ps4_startup_checkpoint(armed ? "shm: out-of-memory test armed" : "shm: out-of-memory test disarmed");
		}
	}

	// Creates the direct-memory object behind a utils::shm. Direct memory is shared by guest memory, Mesa, the JIT
	// and the heap carve-outs, so running out of it while a game runs is an expected event: the failure is returned
	// (nullptr + errno in error) for the caller to report to the guest instead of failing an ensure on its thread.
	static ps4::shared_memory* ps4_create_shared(u64 size, int& error)
	{
		error = 0;

		const u64 refuse = s_ps4_shm_refuse_armed ? shm::ps4_test_refuse_bytes() : 0;
		const bool injected = refuse && size > refuse;

		if (!injected)
		{
			errno = 0;
			const auto shared = new ps4::shared_memory(size);

			if (shared->valid())
			{
				return shared;
			}

			// The failed object holds no direct memory (its chunks were released), only its bookkeeping
			error = errno ? errno : ENOMEM;
			delete shared;
		}
		else
		{
			error = ENOMEM;
		}

		// A game that keeps retrying must not flood startup.log: the first failures in full, then one in 64
		const u32 count = ++s_ps4_shm_failures;

		if (count <= 4 || count % 64 == 0)
		{
			char line[192];
			std::snprintf(line, sizeof(line), "shm: shared memory object of 0x%llx bytes refused (errno %d%s, failure %u)",
				static_cast<unsigned long long>(size), error, injected ? ", injected by test-shm-refuse-mb.txt" : "", count);
			ps4_startup_checkpoint(line);

			if (count == 1)
			{
				char report[512];
				ps4_memory_report(report, sizeof(report));
				ps4_startup_checkpoint(report);
			}
		}

		return nullptr;
	}
#endif

	shm::shm(u64 size, u32 flags)
		: m_flags(flags)
		, m_size(utils::align(size, 0x10000))
	{
#ifdef _WIN32
		const ULARGE_INTEGER max_size{ .QuadPart = m_size };
		m_handle = ensure(::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE, max_size.HighPart, max_size.LowPart, nullptr));
#elif defined(__ORBIS__)
		// 0.31: a refusal is recorded (ps4_failed()) instead of failing an ensure on the calling guest thread
		m_shared = ps4_create_shared(m_size, m_ps4_error);
#elif defined(__linux__) || defined(__FreeBSD__)
		m_file = -1;

		// Try to use 2MB pages for 2M-aligned shm
		if constexpr (c_mfd_huge_2mb != 0)
		{
			if (m_size % 0x200000 == 0 && flags & 2)
			{
				m_file = ::memfd_create_("2M", c_mfd_huge_2mb);
			}
		}

		if (m_file == -1)
		{
			m_file = ::memfd_create_("", 0);
		}

		ensure(m_file >= 0);
		ensure(::ftruncate(m_file, m_size) >= 0);
#else
		const std::string name = "/rpcs3-mem-" + std::to_string(reinterpret_cast<u64>(this));

		while ((m_file = ::shm_open(name.c_str(), O_RDWR | O_CREAT | O_EXCL, S_IWUSR | S_IRUSR)) == -1)
		{
			if (errno == EMFILE)
			{
				fmt::throw_exception("Too many open files. Raise the limit and try again.");
			}

			ensure(errno == EEXIST);
		}

		ensure(::shm_unlink(name.c_str()) >= 0);
		ensure(::ftruncate(m_file, m_size) >= 0);
#endif
	}

	shm::shm(u64 size, const std::string& storage)
		: m_size(utils::align(size, 0x10000))
	{
#ifdef _WIN32
		fs::file f;

		auto open_with_cleanup = [](fs::file& f, const std::string& path)
		{
			f.close();

			for (u32 i = 1; i <= 3; i++)
			{
				// Cleanup
				fs::remove_file(fmt::format("%s.%d.tmp", path, i));
			}

			for (u32 try_count = 3, i = 0; !f && i < try_count; i++, ::Sleep(100))
			{
				// Bug workaround: removing old file may be safer than rewriting it
				if (!fs::remove_file(path) && fs::g_tls_error != fs::error::noent)
				{
					// Try MoveFile
					fs::rename(path, fmt::format("%s.%d.tmp", path, i + 1), true);
				}

				if (f.open(path, fs::read + fs::write + fs::create + fs::excl))
				{
					return true;
				}

				if (fs::g_tls_error != fs::error::exist && fs::g_tls_error != fs::error::acces)
				{
					return false;
				}
			}

			return false;
		};

		std::string storage1 = fs::get_temp_dir();
		std::string storage2 = fs::get_cache_dir();

		if (storage.empty())
		{
			storage1 += "rpcs3_vm_sparse.tmp";
			storage2 += "rpcs3_vm_sparse.tmp";
		}
		else
		{
			storage1 += storage;
			storage2 += storage;
		}

		std::function<bool(const std::string&, HANDLE, usz)> set_sparse_and_map = [&](const std::string& storagex, HANDLE h, usz m_size) -> bool
		{
			// In case failed, revert changes and forward failure
			auto clean = [&](bool result)
			{
				if (!result)
				{
					const fs::error last_fs_error = fs::g_tls_error;
					const DWORD last_win_error = ::GetLastError();

					fs::remove_file(storagex);

					fs::g_tls_error = last_fs_error;
					::SetLastError(last_win_error);
				}

				return result;
			};

			FILE_SET_SPARSE_BUFFER arg{.SetSparse = true};
			FILE_BASIC_INFO info0{};
			ensure(clean(GetFileInformationByHandleEx(h, FileBasicInfo, &info0, sizeof(info0))));

			if ((info0.FileAttributes & FILE_ATTRIBUTE_ARCHIVE) || (~info0.FileAttributes & FILE_ATTRIBUTE_TEMPORARY))
			{
				info0.FileAttributes &= ~FILE_ATTRIBUTE_ARCHIVE;
				info0.FileAttributes |= FILE_ATTRIBUTE_TEMPORARY;
				ensure(clean(SetFileInformationByHandle(h, FileBasicInfo, &info0, sizeof(info0))));
			}

			if (DWORD bytesReturned{}; (info0.FileAttributes & FILE_ATTRIBUTE_SPARSE_FILE) || DeviceIoControl(h, FSCTL_SET_SPARSE, &arg, sizeof(arg), nullptr, 0, &bytesReturned, nullptr))
			{
				if ((info0.FileAttributes & FILE_ATTRIBUTE_SPARSE_FILE) == 0 && !storagex.empty())
				{
					// Retry once (bug workaround)
					if (!open_with_cleanup(f, storagex))
					{
						return false;
					}

					return set_sparse_and_map("", f.get_handle(), m_size);
				}

				FILE_STANDARD_INFO info;
				FILE_END_OF_FILE_INFO _eof{};
				ensure(clean(GetFileInformationByHandleEx(h, FileStandardInfo, &info, sizeof(info))));
				ensure(clean(GetFileSizeEx(h, &_eof.EndOfFile)));

				if (info.AllocationSize.QuadPart && _eof.EndOfFile.QuadPart == static_cast<LONGLONG>(m_size))
				{
					// Truncate file since it may be dirty (fool-proof)
					DWORD ret = 0;
					FILE_ALLOCATED_RANGE_BUFFER dummy{};
					dummy.Length.QuadPart = m_size;

					if (!DeviceIoControl(h, FSCTL_QUERY_ALLOCATED_RANGES, &dummy, sizeof(dummy), nullptr, 0, &ret, 0) || ret)
					{
						_eof.EndOfFile.QuadPart = 0;
					}
				}

				if (_eof.EndOfFile.QuadPart != static_cast<LONGLONG>(m_size))
				{
					// Reset file size to 0 if it doesn't match
					_eof.EndOfFile.QuadPart = 0;
					ensure(clean(SetFileInformationByHandle(h, FileEndOfFileInfo, &_eof, sizeof(_eof))));
				}

				// It seems impossible to automatically delete file on exit when file mapping is used
				if (f.size() != m_size)
				{
					// Resize the file gradually (bug workaround)
					for (usz i = 0; i < m_size / (1024 * 1024 * 256); i++)
					{
						ensure(clean(f.trunc((i + 1) * (1024 * 1024 * 256))));
					}

					ensure(clean(f.trunc(m_size)));
				}

				m_handle = ::CreateFileMappingW(f.get_handle(), nullptr, PAGE_READWRITE, 0, 0, nullptr);
				if (clean(!m_handle))
				{
					ensure(storagex == storage1);
					return false;
				}

				return true;
			}

			return false;
		};

		// Attempt to remove from secondary storage in case this succeeds
		fs::remove_file(storage2);

		if (!open_with_cleanup(f, storage1) || !set_sparse_and_map(storage1, f.get_handle(), m_size))
		{
			// Attempt to remove from main storage in case this succeeds
			f.close();
			fs::remove_file(storage1);

			// Fallback storage
			ensure(open_with_cleanup(f, storage2));

			if (!set_sparse_and_map(storage2, f.get_handle(), m_size))
			{
				MessageBoxW(0, L"Failed to initialize sparse file.\nCan't find a filesystem with sparse file support (NTFS).", L"RPCS3", MB_ICONERROR);
			}

			m_storage = std::move(storage2);
		}
		else
		{
			m_storage = std::move(storage1);
		}
#else

#ifdef __ORBIS__
		// PS4 applications cannot assume the cache directory supports sparse
		// files. The named storage argument is only a backing-file hint here.
		static_cast<void>(storage);
		m_shared = ps4_create_shared(m_size, m_ps4_error);
		return;
#else
#ifdef __linux__
#ifdef ANDROID
		if constexpr (constexpr char c = '?')
#else
		if (const char c = fs::file("/proc/sys/vm/overcommit_memory").read<char>(); c == '0' || c == '1')
#endif
		{
			// Simply use memfd for overcommit memory
			m_file = ensure(::memfd_create_("", 0), FN(x >= 0));
			ensure(::ftruncate(m_file, m_size) >= 0);
			return;
		}
		else
		{
			fprintf(stderr, "Reading /proc/sys/vm/overcommit_memory: %c", c);
		}
#else
		int vm_overcommit = 0;

#if defined(__NetBSD__) || defined(__APPLE__)
		// Always ON
		vm_overcommit = 0;
#elif defined(__FreeBSD__)
		auto vm_sz = sizeof(int);
		int mib[2]{CTL_VM, VM_OVERCOMMIT};
		if (::sysctl(mib, 2, &vm_overcommit, &vm_sz, NULL, 0) != 0)
			vm_overcommit = -1;
#else
		vm_overcommit = -1;
#endif

		if ((vm_overcommit & 3) == 0)
		{
#if defined(__FreeBSD__)
			m_file = ensure(::memfd_create_("", 0), FN(x >= 0));
#else
			const std::string name = "/rpcs3-mem2-" + std::to_string(reinterpret_cast<u64>(this));

			while ((m_file = ::shm_open(name.c_str(), O_RDWR | O_CREAT | O_EXCL, S_IWUSR | S_IRUSR)) == -1)
			{
				if (errno == EMFILE)
				{
					fmt::throw_exception("Too many open files. Raise the limit and try again.");
				}

				ensure(errno == EEXIST);
			}

			ensure(::shm_unlink(name.c_str()) >= 0);
#endif
			ensure(::ftruncate(m_file, m_size) >= 0);
			return;
		}
#endif

		if (!storage.empty())
		{
			m_file = ::open(storage.c_str(), O_RDWR | O_CREAT, S_IWUSR | S_IRUSR);
			::unlink(storage.c_str());
		}
		else
		{
			std::string storage = fs::get_cache_dir() + "rpcs3_vm_sparse.tmp";
			m_file = ::open(storage.c_str(), O_RDWR | O_CREAT, S_IWUSR | S_IRUSR);
			::unlink(storage.c_str());
		}

		ensure(m_file >= 0);
		struct ::stat stats;
		ensure(::fstat(m_file, &stats) >= 0);

		if (!(stats.st_size ^ m_size) && !stats.st_blocks)
		{
			// Already initialized
			return;
		}

		// Truncate file since it may be dirty (fool-proof)
		ensure(::ftruncate(m_file, 0) >= 0);
		ensure(::ftruncate(m_file, 0x100000) >= 0);
		stats.st_size = 0x100000;

#ifdef SEEK_DATA
		errno = EINVAL;
		if (stats.st_blocks * 512 >= 0x100000 && ::lseek(m_file, 0, SEEK_DATA) ^ stats.st_size && errno != ENXIO)
		{
			fmt::throw_exception("Failed to initialize sparse file in '%s'\n"
				"It seems this filesystem doesn't support sparse files (%d).\n",
				storage.empty() ? fs::get_cache_dir().c_str() : storage.c_str(), +errno);
		}
#endif

		if (stats.st_size ^ m_size)
		{
			// Fix file size
			ensure(::ftruncate(m_file, m_size) >= 0);
		}
#endif
#endif
	}

	shm::~shm()
	{
		this->unmap_self();

#ifdef _WIN32
		::CloseHandle(m_handle);
#elif defined(__ORBIS__)
		delete m_shared;
#else
		::close(m_file);
#endif

		if (!m_storage.empty())
			fs::remove_file(m_storage);
	}

	u8* shm::map(void* ptr, protection prot, bool cow) const
	{
#ifdef __ORBIS__
		if (!m_shared)
		{
			errno = ENOMEM;
			return nullptr;
		}

		return static_cast<u8*>(m_shared->map(reinterpret_cast<void*>(reinterpret_cast<u64>(ptr) & -0x10000), +prot, cow));
#elif defined(_WIN32)
		DWORD access = FILE_MAP_WRITE;
		switch (prot)
		{
		case protection::rw:
		case protection::ro:
		case protection::no:
			break;
		case protection::wx:
		case protection::rx:
			access |= FILE_MAP_EXECUTE;
			break;
		}

		if (cow)
		{
			access |= FILE_MAP_COPY;
		}

		if (auto ret = static_cast<u8*>(::MapViewOfFileEx(m_handle, access, 0, 0, m_size, ptr)))
		{
			if (prot != protection::rw && prot != protection::wx)
			{
				DWORD old;
				if (!::VirtualProtect(ret, m_size, +prot, &old))
				{
					::UnmapViewOfFile(ret);
					return nullptr;
				}
			}

			return ret;
		}

		return nullptr;
#else
		const u64 ptr64 = reinterpret_cast<u64>(ptr) & -0x10000;

		if (ptr64)
		{
			const auto result = native_mmap(reinterpret_cast<void*>(ptr64), m_size, +prot, (cow ? MAP_PRIVATE : MAP_SHARED) | MAP_FIXED, m_file, 0);

			return result == MAP_FAILED ? nullptr : reinterpret_cast<u8*>(result);
		}
		else
		{
			const auto reservation = native_mmap(nullptr, m_size + 0xf000, PROT_NONE, MAP_ANON | MAP_PRIVATE, -1, 0);
			if (reservation == MAP_FAILED)
			{
				return nullptr;
			}

			const u64 res64 = reinterpret_cast<u64>(reservation);

			const u64 aligned = utils::align(res64, 0x10000);
			const auto result = native_mmap(reinterpret_cast<void*>(aligned), m_size, +prot, (cow ? MAP_PRIVATE : MAP_SHARED) | MAP_FIXED, m_file, 0);
			if (result == MAP_FAILED)
			{
				ensure(native_munmap(reservation, m_size + 0xf000) == 0);
				return nullptr;
			}

			// Now cleanup remnants
			if (aligned > res64)
			{
				ensure(native_munmap(reinterpret_cast<void*>(res64), aligned - res64) == 0);
			}

			if (aligned < res64 + 0xf000)
			{
				ensure(native_munmap(reinterpret_cast<void*>(aligned + m_size), (res64 + 0xf000) - (aligned)) == 0);
			}

			return reinterpret_cast<u8*>(result);
		}
#endif
	}

	u8* shm::try_map(void* ptr, protection prot, bool cow) const
	{
		// Non-null pointer shall be specified
		const auto target = ensure(reinterpret_cast<u8*>(reinterpret_cast<u64>(ptr) & -0x10000));

#ifdef __ORBIS__
		if (!m_shared)
		{
			errno = ENOMEM;
			return nullptr;
		}

		return static_cast<u8*>(m_shared->map(target, +prot, cow, false));
#elif defined(_WIN32)
		return this->map(target, prot, cow);
#else
		const auto result = reinterpret_cast<u8*>(native_mmap(reinterpret_cast<void*>(target), m_size, +prot, (cow ? MAP_PRIVATE : MAP_SHARED), m_file, 0));

		if (result == reinterpret_cast<void*>(uptr{umax}))
		{
			[[unlikely]] return nullptr;
		}

		return result;
#endif
	}

	std::pair<u8*, std::string> shm::map_critical(void* ptr, protection prot, bool cow)
	{
#ifdef __ORBIS__
		if (!m_shared)
		{
			// The direct memory for this object was refused when it was created
			return {nullptr, "ENOMEM"};
		}
#endif

		const auto target = reinterpret_cast<u8*>(reinterpret_cast<u64>(ptr) & -0x10000);

#ifdef _WIN32
		::MEMORY_BASIC_INFORMATION mem{};
		if (!::VirtualQuery(target, &mem, sizeof(mem)))
		{
			return {nullptr, fmt::format("VirtualQuery() Failed with %s", fmt::win_error{GetLastError(), nullptr})};
		}

		if (mem.State != MEM_RESERVE)
		{
			return {nullptr, fmt::format("VirtualQuery() reported unexpected memory info: state=0x%x, %s", mem.State, std::as_bytes(std::span(&mem, 1)))};
		}

		const auto base = static_cast<u8*>(mem.AllocationBase);
		const auto size = mem.RegionSize + (target - base);

		if (is_memory_mappping_memory(ptr))
		{
			if (base < target && !::VirtualFree(base, target - base, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER))
			{
				return {nullptr, "Failed to split allocation base"};
			}

			if (target + m_size < base + size && !::VirtualFree(target, m_size, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER))
			{
				return {nullptr, "Failed to split allocation end"};
			}

			DWORD access = 0;

			switch (prot)
			{
			case protection::rw:
			case protection::ro:
			case protection::no:
				access = cow ? PAGE_WRITECOPY : PAGE_READWRITE;
				break;
			case protection::wx:
			case protection::rx:
				access = cow ? PAGE_EXECUTE_WRITECOPY : PAGE_EXECUTE_READWRITE;
				break;
			}

			if (MapViewOfFile3(m_handle, GetCurrentProcess(), target, 0, m_size, MEM_REPLACE_PLACEHOLDER, access, nullptr, 0))
			{
				if (prot != protection::rw && prot != protection::wx)
				{
					DWORD old;
					if (!::VirtualProtect(target, m_size, +prot, &old))
					{
						UnmapViewOfFile2(GetCurrentProcess(), target, MEM_PRESERVE_PLACEHOLDER);
						return {nullptr, "Failed to protect"};
					}
				}

				return {target, {}};
			}

			return {nullptr, "Failed to map3"};
		}

		if (!::VirtualFree(mem.AllocationBase, 0, MEM_RELEASE))
		{
			return {nullptr, "VirtualFree() failed on allocation base"};
		}

		if (base < target && !::VirtualAlloc(base, target - base, MEM_RESERVE, PAGE_NOACCESS))
		{
			return {nullptr, "VirtualAlloc() failed to reserve allocation base"};
		}

		if (target + m_size < base + size && !::VirtualAlloc(target + m_size, base + size - target - m_size, MEM_RESERVE, PAGE_NOACCESS))
		{
			return {nullptr, "VirtualAlloc() failed to reserve allocation end"};
		}
#endif
		const auto mapped_addr = this->map(target, prot, cow);

		if (!mapped_addr)
		{
			return {nullptr, "Failed to map"};
		}

		return {mapped_addr, {}};
	}

	u8* shm::map_self(protection prot)
	{
		void* ptr = m_ptr;

		for (void* mapped = nullptr; !ptr;)
		{
			if (!mapped)
			{
				mapped = this->map(nullptr, prot);

#ifdef __ORBIS__
				if (!mapped)
				{
					// Refused view (or a failed object): retrying cannot succeed, so do not spin here forever.
					// Return the view another thread may have installed meanwhile, or nullptr.
					return static_cast<u8*>(+m_ptr);
				}
#endif
			}

			// Install mapped memory
			if (m_ptr.compare_exchange(ptr, mapped))
			{
				ptr = mapped;
			}
			else if (ptr)
			{
				// Mapped already, nothing to do.
				ensure(ptr != mapped);
				this->unmap(mapped);
			}
		}

		return static_cast<u8*>(ptr);
	}

	void shm::unmap(void* ptr) const
	{
#ifdef __ORBIS__
		if (!m_shared)
		{
			// A failed object never had a view
			return;
		}

		ensure(m_shared->unmap(ptr, false));
#elif defined(_WIN32)
		::UnmapViewOfFile(ptr);
#else
		native_munmap(ptr, m_size);
#endif
	}

	void shm::unmap_critical(void* ptr)
	{
		const auto target = reinterpret_cast<u8*>(reinterpret_cast<u64>(ptr) & -0x10000);

#ifdef __ORBIS__
		if (!m_shared)
		{
			// A failed object never had a view
			return;
		}

		// Remove this alias and restore its guest address reservation before
		// the shared object can be destroyed or another object mapped here.
		ensure(m_shared->unmap(target, true));
#elif defined(_WIN32)
		if (is_memory_mappping_memory(ptr))
		{
			ensure(UnmapViewOfFile2(GetCurrentProcess(), target, MEM_PRESERVE_PLACEHOLDER));

			::MEMORY_BASIC_INFORMATION mem{}, mem2{};
			ensure(::VirtualQuery(target - 1, &mem, sizeof(mem)) && ::VirtualQuery(target + m_size, &mem2, sizeof(mem2)));

			const auto size1 = mem.State == MEM_RESERVE ? target - static_cast<u8*>(mem.AllocationBase) : 0;
			const auto size2 =  mem2.State == MEM_RESERVE ? mem2.RegionSize : 0;

			if (!size1 && !size2)
			{
				return;
			}

			ensure(::VirtualFree(size1 ? mem.AllocationBase : target, m_size + size1 + size2, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS));
			return;
		}

		this->unmap(target);

		::MEMORY_BASIC_INFORMATION mem, mem2;
		if (!::VirtualQuery(target - 1, &mem, sizeof(mem)) || !::VirtualQuery(target + m_size, &mem2, sizeof(mem2)))
		{
			return;
		}

		if (mem.State == MEM_RESERVE && !::VirtualFree(mem.AllocationBase, 0, MEM_RELEASE))
		{
			return;
		}

		if (mem2.State == MEM_RESERVE && !::VirtualFree(mem2.AllocationBase, 0, MEM_RELEASE))
		{
			return;
		}

		const auto size1 = mem.State == MEM_RESERVE ? target - static_cast<u8*>(mem.AllocationBase) : 0;
		const auto size2 = mem2.State == MEM_RESERVE ? mem2.RegionSize : 0;

		if (!::VirtualAlloc(mem.State == MEM_RESERVE ? mem.AllocationBase : target, m_size + size1 + size2, MEM_RESERVE, PAGE_NOACCESS))
		{
			return;
		}
#else
		// This method is faster but leaves mapped remnants of the shm (until overwritten)
		ensure(native_mprotect(target, m_size, PROT_NONE) != -1);
#endif
	}

	void shm::unmap_self()
	{
		if (auto ptr = m_ptr.exchange(nullptr))
		{
			this->unmap(ptr);
		}
	}
} // namespace utils
