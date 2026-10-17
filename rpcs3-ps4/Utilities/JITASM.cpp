#include "util/types.hpp"
#include "util/sysinfo.hpp"
#include "JIT.h"
#include "StrFmt.h"
#include "File.h"
#include "util/logs.hpp"
#include "util/vm.hpp"
#include "util/asm.hpp"
#include "util/v128.hpp"
#include "util/simd.hpp"

#ifdef __linux__
#include <unistd.h>
#define CAN_OVERCOMMIT
#endif

#if defined(__APPLE__)
#include <mutex>
#endif

#ifdef RPCS3_PS4
#include "util/ps4_virtual_memory.hpp"
#include "util/ps4_memory_governor.hpp"
#include <sys/mman.h>
#include <cerrno>
#include <chrono>
#include <thread>

void ps4_startup_checkpoint(const char* stage);
#endif

LOG_CHANNEL(jit_log, "JIT");

void jit_announce(uptr func, usz size, std::string_view name)
{
#ifdef __linux__
#if 0
	static const struct tmp_perf_map
	{
		std::string name{fmt::format("/tmp/perf-%d.map", getpid())};
		fs::file data{name, fs::rewrite + fs::append};

		tmp_perf_map() = default;
		tmp_perf_map(const tmp_perf_map&) = delete;
		tmp_perf_map& operator=(const tmp_perf_map&) = delete;

		~tmp_perf_map()
		{
			fs::remove_file(name);
		}
	} s_map;

	if (size && name.size())
	{
		s_map.data.write(fmt::format("%x %x %s\n", func, size, name));
	}

	if (!func && !size && !name.size())
	{
		fs::remove_file(s_map.name);
		return;
	}
#endif
#endif

	if (!size)
	{
		jit_log.error("Empty function announced: %s (%p)", name, func);
		return;
	}

	// If directory ASMJIT doesn't exist, nothing will be written
	static constexpr u64 c_dump_size = 0x1'0000'0000;
	static constexpr u64 c_index_size = c_dump_size / 16;
	static atomic_t<u64> g_index_off = 0;
	static atomic_t<u64> g_data_off = c_index_size;

	static void* g_asm = []() -> void*
	{
		fs::remove_all(fs::get_cache_dir() + "/ASMJIT/", false);

		fs::file objs(fmt::format("%s/ASMJIT/.objects", fs::get_cache_dir()), fs::read + fs::rewrite);

		if (!objs || !objs.trunc(c_dump_size))
		{
			return nullptr;
		}

		return utils::memory_map_fd(objs.get_handle(), c_dump_size, utils::protection::rw);
	}();

	if (g_asm && size < c_index_size)
	{
		struct entry
		{
			u64 addr; // RPCS3 process address
			u32 size; // Function size
			u32 off; // Function offset
		};

		// Write index entry at the beginning of file, and data + NTS name at fixed offset
		const u64 index_off = g_index_off.fetch_add(1);
		const u64 size_all = size + name.size() + 1;
		const u64 data_off = g_data_off.fetch_add(size_all);

		// If either index or data area is exhausted, nothing will be written
		if (index_off < c_index_size / sizeof(entry) && data_off + size_all < c_dump_size)
		{
			entry& index = static_cast<entry*>(g_asm)[index_off];

			std::memcpy(static_cast<char*>(g_asm) + data_off, reinterpret_cast<char*>(func), size);
			std::memcpy(static_cast<char*>(g_asm) + data_off + size, name.data(), name.size());
			index.size = static_cast<u32>(size);
			index.off = static_cast<u32>(data_off);
			atomic_storage<u64>::store(index.addr, func);
		}
	}

	if (g_asm && !name.empty() && name[0] != '_')
	{
		// Save some objects separately
		fs::file dump(fmt::format("%s/ASMJIT/%s", fs::get_cache_dir(), name), fs::rewrite);

		if (dump)
		{
			dump.write(reinterpret_cast<uchar*>(func), size);
		}
	}
}

static u8* get_jit_memory()
{
	// Reserve 2G memory (magic static)
	static void* const s_memory2 = []() -> void*
	{
		void* ptr = utils::memory_reserve(0x80000000, true);
		ensure(ptr, "Failed to reserve the JIT code/data address space");
#ifdef CAN_OVERCOMMIT
		utils::memory_commit(ptr, 0x80000000);
		utils::memory_protect(ptr, 0x40000000, utils::protection::wx);
#endif
		return ptr;
	}();

	return static_cast<u8*>(s_memory2);
}

// Allocation counters (1G code, 1G data subranges)
static atomic_t<u64> s_code_pos{0}, s_data_pos{0};

#ifdef RPCS3_PS4
// Commits refused by the console (direct memory exhausted) since the process started
static atomic_t<u32> s_ps4_commit_refusals{0};

namespace
{
	atomic_t<u64> g_ps4_jit_link{0};
	atomic_t<u64> g_ps4_jit_exec{0};
	atomic_t<u32> g_ps4_jit_refusals{0};
	atomic_t<u32> g_ps4_jit_retry_lines{0};
	atomic_t<u64> g_ps4_jit_relief_us{0};

	u64 ps4_jit_now_us() noexcept
	{
		return static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
	}

	// Gives the pool a chance before a commit is tried again: empty heap carve-outs back to the pool, then a budget
	// tick (new reservation, shedders of the current level). At most every 100 ms; false when skipped.
	bool ps4_jit_relief() noexcept
	{
		const u64 now = ps4_jit_now_us();
		u64 last = g_ps4_jit_relief_us.load();

		if (now - last < 100'000 || !g_ps4_jit_relief_us.compare_and_swap_test(last, now))
		{
			return false;
		}

		orbis::mmapDirectTrim(0);
		utils::ps4::governor::budget_tick(true);
		return true;
	}

	// The budget's JIT figure (installed before main; any thread)
	[[maybe_unused]] const bool s_ps4_jit_source = []() noexcept
	{
		utils::ps4::governor::set_jit_source(&utils::ps4::jit::committed_total);
		return true;
	}();
}

namespace utils::ps4::jit
{
	u64 committed(owner o) noexcept
	{
		switch (o)
		{
		case owner::runtime: return (s_code_pos.load() >> 32) + (s_data_pos.load() >> 32);
		case owner::link: return g_ps4_jit_link.load();
		case owner::exec: return g_ps4_jit_exec.load();
		}

		return 0;
	}

	u64 committed_total() noexcept
	{
		return committed(owner::runtime) + committed(owner::link) + committed(owner::exec);
	}

	void account(owner o, s64 delta) noexcept
	{
		if (o == owner::link)
		{
			g_ps4_jit_link += static_cast<u64>(delta);
		}
		else if (o == owner::exec)
		{
			g_ps4_jit_exec += static_cast<u64>(delta);
		}
	}

	void reset(owner o) noexcept
	{
		if (o == owner::link)
		{
			g_ps4_jit_link = 0;
		}
		else if (o == owner::exec)
		{
			g_ps4_jit_exec = 0;
		}
	}

	bool commit(void* ptr, usz size, int prot, u32 wait_ms, const char* what) noexcept
	{
		if (utils::ps4::memory::commit(ptr, size, prot))
		{
			return true;
		}

		int error = errno;
		const u64 start = ps4_jit_now_us();
		u32 tries = 0;

		while (true)
		{
			const bool relieved = ps4_jit_relief();

			if (relieved || wait_ms)
			{
				tries++;

				if (utils::ps4::memory::commit(ptr, size, prot))
				{
					if (g_ps4_jit_retry_lines++ < 16)
					{
						ps4_startup_checkpoint(fmt::format("JIT memory: commit of %u KiB for %s refused (errno %d), done at try %u after %u ms (heap trim + budget)",
							size >> 10, what, error, tries + 1, (ps4_jit_now_us() - start) / 1000).c_str());
					}

					return true;
				}

				error = errno;
			}

			if (!wait_ms || ps4_jit_now_us() - start >= u64{wait_ms} * 1000)
			{
				break;
			}

			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}

		const u32 count = ++g_ps4_jit_refusals;

		if (count <= 8 || (count & (count - 1)) == 0)
		{
			ps4_startup_checkpoint(fmt::format("JIT memory: commit of %u KiB for %s refused after %u tries in %u ms (errno %d); JIT %u MiB (runtime %u, link %u, exec %u) (#%u)",
				size >> 10, what, tries + 1, (ps4_jit_now_us() - start) / 1000, error, committed_total() >> 20, committed(owner::runtime) >> 20,
				committed(owner::link) >> 20, committed(owner::exec) >> 20, count).c_str());
		}

		errno = error;
		return false;
	}

	u32 refusals() noexcept
	{
		return g_ps4_jit_refusals.load();
	}
}

// 0.38: test drills only. /data/rpcs3/test-jit-cap-mib.txt (decimal MiB): commits for jit_runtime::ps4_try_alloc() and
// the PPU link arenas are refused once the JIT ledger would pass the cap, as if the console refused them. Read at the
// first such commit (never before main: the static trampolines use alloc()); 0 = no cap.
static u64 ps4_jit_test_cap()
{
	static const u64 s_cap = []() -> u64
	{
		const fs::file file("/data/rpcs3/test-jit-cap-mib.txt");

		if (!file)
		{
			return 0;
		}

		u64 mib = 0;

		for (const char c : file.to_string())
		{
			if (c < '0' || c > '9' || mib > 65536)
			{
				break;
			}

			mib = mib * 10 + (c - '0');
		}

		if (mib && mib <= 65536)
		{
			ps4_startup_checkpoint(fmt::format("JIT memory: test cap of %u MiB of private direct memory for the checked JIT allocations (test-jit-cap-mib.txt)", mib).c_str());
			return mib << 20;
		}

		return 0;
	}();

	return s_cap;
}

bool utils::ps4::jit::test_cap_refuses(usz size) noexcept
{
	const u64 cap = ps4_jit_test_cap();
	return cap && committed_total() + size > cap;
}

// Backs [ptr, ptr + size) of the JIT range. For jit_runtime::ps4_try_alloc() a refusal fails only the allocation that
// needed these pages (the SPU compile keeps the program's previous code, the trampoline builders give up); the next
// allocation that reaches these pages commits again. For alloc(), whose callers use the result unchecked, a refusal
// ends the thread as utils::memory_commit's check did.
static bool ps4_jit_commit(u8* ptr, usz size, bool exec, usz request, bool may_fail)
{
	const int prot = exec ? PROT_READ | PROT_WRITE | PROT_EXEC : PROT_READ | PROT_WRITE;
	const bool capped = may_fail && utils::ps4::jit::test_cap_refuses(size);

	// Unchecked callers (alloc(): PPU stubs, static trampolines) wait up to 3 s for memory before ending the thread
	if (!capped && utils::ps4::jit::commit(ptr, size, prot, may_fail ? 0 : 3000, exec ? "jit_runtime code" : "jit_runtime data"))
	{
		return true;
	}

	const int error = capped ? ENOMEM : errno;
	const u32 count = ++s_ps4_commit_refusals;

	if (!may_fail || count <= 8 || (count & (count - 1)) == 0)
	{
		const std::string line = fmt::format("JIT memory: commit of %u KiB of %s memory at %p refused (errno %d%s) for %u bytes; %s (#%u)",
			size >> 10, exec ? "code" : "data", ptr, error, capped ? ", test cap" : "", request,
			may_fail ? "the caller keeps its fallback" : "no fallback for this caller", count);
		jit_log.error("PS4: %s", line);
		ps4_startup_checkpoint(line.c_str());
	}

	if (!may_fail)
	{
		fmt::throw_exception("JIT memory exhausted: commit of %u KiB of %s memory refused (errno %d)", size >> 10, exec ? "code" : "data", error);
	}

	return false;
}
#endif

// Snapshot of code generated before main()
static std::vector<u8> s_code_init, s_data_init;

template <atomic_t<u64>& Ctr, uint Off, utils::protection Prot>
#ifdef RPCS3_PS4
static u8* add_jit_memory(usz size, usz align, bool ps4_may_fail = false)
#else
static u8* add_jit_memory(usz size, usz align)
#endif
{
	// Select subrange
	u8* pointer = get_jit_memory() + Off;

	if (!size && !align) [[unlikely]]
	{
		// Return subrange info
		return pointer;
	}

	if (!size && align == 1)
	{
		// Return memory top address
		return pointer + (Ctr.load() & 0xffff'ffff);
	}

	u64 olda, newa;

	// Simple allocation by incrementing pointer to the next free data
	const u64 pos = Ctr.atomic_op([&](u64& ctr) -> u64
	{
		const u64 _pos = utils::align(ctr & 0xffff'ffff, align);
		const u64 _new = utils::align(_pos + size, align);

		if (_new > 0x40000000) [[unlikely]]
		{
			// Sorry, we failed, and further attempts should fail too.
			ctr |= 0x40000000;
			return -1;
		}

		// Last allocation is stored in highest bits
		olda = ctr >> 32;
		newa = olda;

		// Check the necessity to commit more memory
		if (_new > olda) [[unlikely]]
		{
			newa = utils::align(_new, 0x200000);
		}

		ctr += _new - (ctr & 0xffff'ffff);
		return _pos;
	});

	if (pos == umax) [[unlikely]]
	{
		jit_log.error("Out of memory (size=0x%x, align=0x%x, off=0x%x)", size, align, Off);
		return nullptr;
	}

	if (olda != newa) [[unlikely]]
	{
#if defined(RPCS3_PS4)
		// Commit more memory. On a refusal the reserved range stays consumed and the committed mark is not moved.
		if (!ps4_jit_commit(pointer + olda, newa - olda, Prot == utils::protection::wx, size, ps4_may_fail))
		{
			return nullptr;
		}
#elif !defined(CAN_OVERCOMMIT)
		// Commit more memory.
		utils::memory_commit(pointer + olda, newa - olda, Prot);
#endif
		// Acknowledge committed memory
		Ctr.atomic_op([&](u64& ctr)
		{
			if ((ctr >> 32) < newa)
			{
				ctr += (newa - (ctr >> 32)) << 32;
			}
		});
	}

	ensure(pointer + pos >= get_jit_memory() + Off);
	ensure(pointer + pos < get_jit_memory() + Off + 0x40000000);

	return pointer + pos;
}

const asmjit::Environment& jit_runtime_base::environment() const noexcept
{
	static const asmjit::Environment g_env = asmjit::Environment::host();

	return g_env;
}

void* jit_runtime_base::_add(asmjit::CodeHolder* code, usz align) noexcept
{
	ensure(!code->flatten());
	ensure(!code->resolveUnresolvedLinks());
	usz codeSize = code->codeSize();
	if (!codeSize)
		return nullptr;

#ifdef RPCS3_PS4
	// No JIT memory (jit_runtime commit refused, or a fixed runtime full): the caller gets null and keeps its fallback
	auto p = this->_alloc(codeSize, align);

	if (!p)
	{
		static atomic_t<u32> s_logged{0};

		if (const u32 n = ++s_logged; n <= 8 || (n & (n - 1)) == 0)
		{
			jit_log.error("PS4: no JIT memory for %u bytes of assembled code (#%u)", codeSize, n);
		}

		return nullptr;
	}
#else
	auto p = ensure(this->_alloc(codeSize, align));
#endif
	ensure(!code->relocateToBase(uptr(p)));

	{
		// We manage rw <-> rx transitions manually on Apple
		// because it's easier to keep track of when and where we need to toggle W^X
#if !(defined(ARCH_ARM64) && defined(__APPLE__))
		asmjit::VirtMem::ProtectJitReadWriteScope rwScope(p, codeSize);
#endif

		for (asmjit::Section* section : code->_sections)
		{
			if (section->offset() + section->bufferSize() > utils::align<usz>(codeSize, align))
			{
				fmt::throw_exception("CodeHolder section exceeds range: Section->offset: 0x%x, Section->bufferSize: 0x%x, alloted-memory=0x%x", section->offset(), section->bufferSize(), utils::align<usz>(codeSize, align));
			}

			std::memcpy(p + section->offset(), section->data(), section->bufferSize());
		}
	}

	return p;
}

jit_runtime::jit_runtime()
{
}

jit_runtime::~jit_runtime()
{
}

uchar* jit_runtime::_alloc(usz size, usz align) noexcept
{
#ifdef RPCS3_PS4
	// Only reached through _add(), which answers null to its callers
	return jit_runtime::ps4_try_alloc(size, align, true);
#else
	return jit_runtime::alloc(size, align, true);
#endif
}

u8* jit_runtime::alloc(usz size, usz align, bool exec) noexcept
{
#if defined(__APPLE__)
	static std::mutex s_alloc_lock;
	std::lock_guard lock(s_alloc_lock);
#endif

	if (exec)
	{
		return add_jit_memory<s_code_pos, 0x0, utils::protection::wx>(size, align);
	}
	else
	{
		return add_jit_memory<s_data_pos, 0x40000000, utils::protection::rw>(size, align);
	}
}

#ifdef RPCS3_PS4
u8* jit_runtime::ps4_try_alloc(usz size, usz align, bool exec) noexcept
{
	if (exec)
	{
		return add_jit_memory<s_code_pos, 0x0, utils::protection::wx>(size, align, true);
	}
	else
	{
		return add_jit_memory<s_data_pos, 0x40000000, utils::protection::rw>(size, align, true);
	}
}
#endif

u8* jit_runtime::peek(bool exec) noexcept
{
	if (exec)
	{
		return add_jit_memory<s_code_pos, 0x0, utils::protection::wx>(0, 1);
	}
	else
	{
		return add_jit_memory<s_data_pos, 0x40000000, utils::protection::rw>(0, 1);
	}
}

void jit_runtime::initialize()
{
	if (!s_code_init.empty() || !s_data_init.empty())
	{
		return;
	}

	// Create code/data snapshot
	s_code_init.resize(s_code_pos & 0xffff'ffff);
	std::memcpy(s_code_init.data(), alloc(0, 0, true), s_code_init.size());
	s_data_init.resize(s_data_pos & 0xffff'ffff);
	std::memcpy(s_data_init.data(), alloc(0, 0, false), s_data_init.size());
}

void jit_runtime::finalize() noexcept
{
#ifdef __APPLE__
	pthread_jit_write_protect_np(false);
#endif
	// Reset JIT memory
#ifdef CAN_OVERCOMMIT
	utils::memory_reset(get_jit_memory(), 0x80000000, true);
	utils::memory_protect(get_jit_memory(), 0x40000000, utils::protection::wx);
#else
	utils::memory_decommit(get_jit_memory(), 0x80000000, true);
#endif

	s_code_pos = 0;
	s_data_pos = 0;

#ifdef RPCS3_PS4
	// vm::close() decommitted the PPU executable and segment tables just before
	utils::ps4::jit::reset(utils::ps4::jit::owner::exec);
#endif

	// Restore code/data snapshot
	std::memcpy(alloc(s_code_init.size(), 1, true), s_code_init.data(), s_code_init.size());
	std::memcpy(alloc(s_data_init.size(), 1, false), s_data_init.data(), s_data_init.size());

#ifdef __APPLE__
	pthread_jit_write_protect_np(true);
#endif
#ifdef ARCH_ARM64
	// Flush all cache lines after potentially writing executable code
	asm("ISB");
	asm("DSB ISH");
#endif
}

jit_runtime_base& asmjit::get_global_runtime()
{
	// 16 MiB for internal needs
	static constexpr u64 size = 1024 * 1024 * 16;

	struct custom_runtime final : jit_runtime_base
	{
		custom_runtime() noexcept
		{
			ensure(m_pos.raw() = static_cast<uchar*>(utils::memory_reserve(size, true)));

			// Initialize "end" pointer
			m_max = m_pos + size;

			// Make memory writable + executable
			utils::memory_commit(m_pos, size, utils::protection::wx);
		}

		uchar* _alloc(usz size, usz align) noexcept override
		{
			return m_pos.atomic_op([&](uchar*& pos) -> uchar*
			{
				const auto r = reinterpret_cast<uchar*>(utils::align(uptr(pos), align));

				if (r >= pos && r + size > pos && r + size <= m_max)
				{
					pos = r + size;
					return r;
				}

				return nullptr;
			});
		}

	private:
		atomic_t<uchar*> m_pos{};

		uchar* m_max{};
	};

	// Magic static
	static custom_runtime g_rt;
	return g_rt;
}

asmjit::inline_runtime::inline_runtime(uchar* data, usz size)
	: m_data(data)
	, m_size(size)
{
}

uchar* asmjit::inline_runtime::_alloc(usz size, usz align) noexcept
{
	ensure(align <= 4096);

	return size <= m_size ? m_data : nullptr;
}

asmjit::inline_runtime::~inline_runtime()
{
	utils::memory_protect(m_data, m_size, utils::protection::rx);
}

#if defined(ARCH_X64)
asmjit::simd_builder::simd_builder(CodeHolder* ch) noexcept
	: native_asm(ch)
{
	_init(0);
	consts[~v128()] = this->newLabel();
}

asmjit::simd_builder::~simd_builder()
{
}

void asmjit::simd_builder::_init(uint new_vsize)
{
	if ((!new_vsize && utils::has_avx512_icl()) || new_vsize == 64)
	{
		v0 = x86::zmm0;
		v1 = x86::zmm1;
		v2 = x86::zmm2;
		v3 = x86::zmm3;
		v4 = x86::zmm4;
		v5 = x86::zmm5;
		vsize = 64;
	}
	else if ((!new_vsize && utils::has_avx2()) || new_vsize == 32)
	{
		v0 = x86::ymm0;
		v1 = x86::ymm1;
		v2 = x86::ymm2;
		v3 = x86::ymm3;
		v4 = x86::ymm4;
		v5 = x86::ymm5;
		vsize = 32;
	}
	else
	{
		v0 = x86::xmm0;
		v1 = x86::xmm1;
		v2 = x86::xmm2;
		v3 = x86::xmm3;
		v4 = x86::xmm4;
		v5 = x86::xmm5;
		vsize = new_vsize ? new_vsize : 16;
	}

	if (utils::has_avx512())
	{
		if (!new_vsize)
			vmask = -1;
	}
	else
	{
		vmask = 0;
	}
}

void asmjit::simd_builder::operator()() noexcept
{
	for (auto&& [x, y] : consts)
	{
		this->align(AlignMode::kData, 16);
		this->bind(y);
		this->embed(&x, 16);
	}
}

void asmjit::simd_builder::vec_cleanup_ret()
{
	if (utils::has_avx() && vsize > 16)
		this->vzeroupper();
	this->ret();
}

void asmjit::simd_builder::vec_set_all_zeros(const Operand& v)
{
	x86::Xmm reg(v.id());
	if (utils::has_avx())
		this->vpxor(reg, reg, reg);
	else
		this->xorps(reg, reg);
}

void asmjit::simd_builder::vec_set_all_ones(const Operand& v)
{
	x86::Xmm reg(v.id());
	if (x86::Zmm zr(v.id()); zr == v)
		this->vpternlogd(zr, zr, zr, 0xff);
	else if (x86::Ymm yr(v.id()); yr == v)
		this->vpcmpeqd(yr, yr, yr);
	else if (utils::has_avx())
		this->vpcmpeqd(reg, reg, reg);
	else
		this->pcmpeqd(reg, reg);
}

void asmjit::simd_builder::vec_set_const(const Operand& v, const v128& val)
{
	if (!val._u)
		return vec_set_all_zeros(v);
	if (!~val._u)
		return vec_set_all_ones(v);
	else
	{
		Label co = consts[val];
		if (!co.isValid())
			co = consts[val] = this->newLabel();
		if (x86::Zmm zr(v.id()); zr == v)
			this->vbroadcasti32x4(zr, x86::oword_ptr(co));
		else if (x86::Ymm yr(v.id()); yr == v)
			this->vbroadcasti128(yr, x86::oword_ptr(co));
		else if (utils::has_avx())
			this->vmovaps(x86::Xmm(v.id()), x86::oword_ptr(co));
		else
			this->movaps(x86::Xmm(v.id()), x86::oword_ptr(co));
	}
}

void asmjit::simd_builder::vec_clobbering_test(u32 esize, const Operand& v, const Operand& rhs)
{
	if (esize == 64)
	{
		this->emit(x86::Inst::kIdVptestmd, x86::k0, v, rhs);
		this->ktestw(x86::k0, x86::k0);
	}
	else if (esize == 32)
	{
		this->emit(x86::Inst::kIdVptest, v, rhs);
	}
	else if (esize == 16 && utils::has_avx())
	{
		this->emit(x86::Inst::kIdVptest, v, rhs);
	}
	else if (esize == 16 && utils::has_sse41())
	{
		this->emit(x86::Inst::kIdPtest, v, rhs);
	}
	else
	{
		if (v != rhs)
			this->emit(x86::Inst::kIdPand, v, rhs);
		if (esize == 16)
			this->emit(x86::Inst::kIdPacksswb, v, v);
		this->emit(x86::Inst::kIdMovq, x86::rax, v);
		if (esize == 16 || esize == 8)
			this->test(x86::rax, x86::rax);
		else if (esize == 4)
			this->test(x86::eax, x86::eax);
		else if (esize == 2)
			this->test(x86::ax, x86::ax);
		else if (esize == 1)
			this->test(x86::al, x86::al);
		else
			fmt::throw_exception("Unimplemented");
	}
}

void asmjit::simd_builder::vec_broadcast_gpr(u32 esize, const Operand& v, const x86::Gp& r)
{
	if (esize == 2)
	{
		if (utils::has_avx512())
			this->emit(x86::Inst::kIdVpbroadcastw, v, r.r32());
		else if (utils::has_avx())
		{
			this->emit(x86::Inst::kIdVmovd, v, r.r32());
			if (utils::has_avx2())
				this->emit(x86::Inst::kIdVpbroadcastw, v, v);
			else
			{
				this->emit(x86::Inst::kIdVpunpcklwd, v, v, v);
				this->emit(x86::Inst::kIdVpshufd, v, v, Imm(0));
			}
		}
		else
		{
			this->emit(x86::Inst::kIdMovd, v, r.r32());
			this->emit(x86::Inst::kIdPunpcklwd, v, v);
			this->emit(x86::Inst::kIdPshufd, v, v, Imm(0));
		}
	}
	else if (esize == 4)
	{
		if (utils::has_avx512())
			this->emit(x86::Inst::kIdVpbroadcastd, v, r.r32());
		else if (utils::has_avx())
		{
			this->emit(x86::Inst::kIdVmovd, v, r.r32());
			if (utils::has_avx2())
				this->emit(x86::Inst::kIdVpbroadcastd, v, v);
			else
				this->emit(x86::Inst::kIdVpshufd, v, v, Imm(0));
		}
		else
		{
			this->emit(x86::Inst::kIdMovd, v, r.r32());
			this->emit(x86::Inst::kIdPshufd, v, v, Imm(0));
		}
	}
	else
	{
		fmt::throw_exception("Unimplemented");
	}
}

asmjit::x86::Mem asmjit::simd_builder::ptr_scale_for_vec(u32 esize, const x86::Gp& base, const x86::Gp& index)
{
	switch (ensure(esize))
	{
	case 1: return x86::ptr(base, index, 0, 0);
	case 2: return x86::ptr(base, index, 1, 0);
	case 4: return x86::ptr(base, index, 2, 0);
	case 8: return x86::ptr(base, index, 3, 0);
	default: fmt::throw_exception("Bad esize");
	}
}

void asmjit::simd_builder::vec_load_unaligned(u32 esize, const Operand& v, const x86::Mem& src)
{
	ensure(esize && !(esize & (esize - 1)));
	ensure(vsize && !(vsize & (vsize - 1)));

	if (esize == 2)
	{
		ensure(vsize >= 2);
		if (vsize == 2)
			vec_set_all_zeros(v);
		if (vsize == 2 && utils::has_avx())
			this->emit(x86::Inst::kIdVpinsrw, x86::Xmm(v.id()), x86::Xmm(v.id()), src, Imm(0));
		else if (vsize == 2)
			this->emit(x86::Inst::kIdPinsrw, v, src, Imm(0));
		else if ((vmask && vmask < 8) || vsize >= 64)
			this->emit(x86::Inst::kIdVmovdqu16, v, src);
		else
			return vec_load_unaligned(vsize, v, src);
	}
	else if (esize == 4)
	{
		ensure(vsize >= 4);
		if (vsize == 4 && utils::has_avx())
			this->emit(x86::Inst::kIdVmovd, x86::Xmm(v.id()), src);
		else if (vsize == 4)
			this->emit(x86::Inst::kIdMovd, v, src);
		else if ((vmask && vmask < 8) || vsize >= 64)
			this->emit(x86::Inst::kIdVmovdqu32, v, src);
		else
			return vec_load_unaligned(vsize, v, src);
	}
	else if (esize == 8)
	{
		ensure(vsize >= 8);
		if (vsize == 8 && utils::has_avx())
			this->emit(x86::Inst::kIdVmovq, x86::Xmm(v.id()), src);
		else if (vsize == 8)
			this->emit(x86::Inst::kIdMovq, v, src);
		else if ((vmask && vmask < 8) || vsize >= 64)
			this->emit(x86::Inst::kIdVmovdqu64, v, src);
		else
			return vec_load_unaligned(vsize, v, src);
	}
	else if (esize >= 16)
	{
		ensure(vsize >= 16);
		if ((vmask && vmask < 8) || vsize >= 64)
			this->emit(x86::Inst::kIdVmovdqu64, v, src); // Not really needed
		else if (utils::has_avx())
			this->emit(x86::Inst::kIdVmovdqu, v, src);
		else
			this->emit(x86::Inst::kIdMovups, v, src);
	}
	else
	{
		fmt::throw_exception("Unimplemented");
	}
}

void asmjit::simd_builder::vec_store_unaligned(u32 esize, const Operand& v, const x86::Mem& dst)
{
	ensure(esize && !(esize & (esize - 1)));
	ensure(vsize && !(vsize & (vsize - 1)));

	if (esize == 2)
	{
		ensure(vsize >= 2);
		if (vsize == 2 && utils::has_avx())
			this->emit(x86::Inst::kIdVpextrw, dst, x86::Xmm(v.id()), Imm(0));
		else if (vsize == 2 && utils::has_sse41())
			this->emit(x86::Inst::kIdPextrw, dst, v, Imm(0));
		else if (vsize == 2)
			this->push(x86::rax), this->pextrw(x86::eax, x86::Xmm(v.id()), 0), this->mov(dst, x86::ax), this->pop(x86::rax);
		else if ((vmask && vmask < 8) || vsize >= 64)
			this->emit(x86::Inst::kIdVmovdqu16, dst, v);
		else
			return vec_store_unaligned(vsize, v, dst);
	}
	else if (esize == 4)
	{
		ensure(vsize >= 4);
		if (vsize == 4 && utils::has_avx())
			this->emit(x86::Inst::kIdVmovd, dst, x86::Xmm(v.id()));
		else if (vsize == 4)
			this->emit(x86::Inst::kIdMovd, dst, v);
		else if ((vmask && vmask < 8) || vsize >= 64)
			this->emit(x86::Inst::kIdVmovdqu32, dst, v);
		else
			return vec_store_unaligned(vsize, v, dst);
	}
	else if (esize == 8)
	{
		ensure(vsize >= 8);
		if (vsize == 8 && utils::has_avx())
			this->emit(x86::Inst::kIdVmovq, dst, x86::Xmm(v.id()));
		else if (vsize == 8)
			this->emit(x86::Inst::kIdMovq, dst, v);
		else if ((vmask && vmask < 8) || vsize >= 64)
			this->emit(x86::Inst::kIdVmovdqu64, dst, v);
		else
			return vec_store_unaligned(vsize, v, dst);
	}
	else if (esize >= 16)
	{
		ensure(vsize >= 16);
		if ((vmask && vmask < 8) || vsize >= 64)
			this->emit(x86::Inst::kIdVmovdqu64, dst, v); // Not really needed
		else if (utils::has_avx())
			this->emit(x86::Inst::kIdVmovdqu, dst, v);
		else
			this->emit(x86::Inst::kIdMovups, dst, v);
	}
	else
	{
		fmt::throw_exception("Unimplemented");
	}
}

void asmjit::simd_builder::_vec_binary_op(x86::Inst::Id sse_op, x86::Inst::Id vex_op, x86::Inst::Id evex_op, const Operand& dst, const Operand& lhs, const Operand& rhs)
{
	if (utils::has_avx())
	{
		if (evex_op != x86::Inst::kIdNone && (vex_op == x86::Inst::kIdNone || this->_extraReg.isReg() || vsize >= 64))
		{
			this->evex().emit(evex_op, dst, lhs, rhs);
		}
		else
		{
			this->emit(vex_op, dst, lhs, rhs);
		}
	}
	else if (dst == lhs)
	{
		this->emit(sse_op, dst, rhs);
	}
	else if (dst == rhs)
	{
		fmt::throw_exception("Unimplemented");
	}
	else
	{
		this->emit(x86::Inst::kIdMovaps, dst, lhs);
		this->emit(sse_op, dst, rhs);
	}
}

void asmjit::simd_builder::vec_umin(u32 esize, const Operand& dst, const Operand& lhs, const Operand& rhs)
{
	using enum x86::Inst::Id;
	if (esize == 2)
	{
		if (utils::has_sse41())
			return _vec_binary_op(kIdPminuw, kIdVpminuw, kIdVpminuw, dst, lhs, rhs);
	}
	else if (esize == 4)
	{
		if (utils::has_sse41())
			return _vec_binary_op(kIdPminud, kIdVpminud, kIdVpminud, dst, lhs, rhs);
	}

	fmt::throw_exception("Unimplemented");
}

void asmjit::simd_builder::vec_umax(u32 esize, const Operand& dst, const Operand& lhs, const Operand& rhs)
{
	using enum x86::Inst::Id;
	if (esize == 2)
	{
		if (utils::has_sse41())
			return _vec_binary_op(kIdPmaxuw, kIdVpmaxuw, kIdVpmaxuw, dst, lhs, rhs);
	}
	else if (esize == 4)
	{
		if (utils::has_sse41())
			return _vec_binary_op(kIdPmaxud, kIdVpmaxud, kIdVpmaxud, dst, lhs, rhs);
	}

	fmt::throw_exception("Unimplemented");
}

void asmjit::simd_builder::vec_cmp_eq(u32 esize, const Operand& dst, const Operand& lhs, const Operand& rhs)
{
	using enum x86::Inst::Id;
	if (esize == 2)
	{
		if (vsize == 64)
		{
			this->evex().emit(kIdVpcmpeqw, x86::k0, lhs, rhs);
			this->evex().emit(kIdVpmovm2w, dst, x86::k0);
		}
		else
		{
			_vec_binary_op(kIdPcmpeqw, kIdVpcmpeqw, kIdNone, dst, lhs, rhs);
		}
	}
	else if (esize == 4)
	{
		if (vsize == 64)
		{
			this->evex().emit(kIdVpcmpeqd, x86::k0, lhs, rhs);
			this->evex().emit(kIdVpmovm2d, dst, x86::k0);
		}
		else
		{
			_vec_binary_op(kIdPcmpeqd, kIdVpcmpeqd, kIdNone, dst, lhs, rhs);
		}
	}
	else
	{
		fmt::throw_exception("Unimplemented");
	}
}

void asmjit::simd_builder::vec_extract_high(u32, const Operand& dst, const Operand& src)
{
	if (vsize == 32)
		this->vextracti32x8(x86::Ymm(dst.id()), x86::Zmm(src.id()), 1);
	else if (vsize == 16)
		this->vextracti128(x86::Xmm(dst.id()), x86::Ymm(src.id()), 1);
	else
	{
		if (utils::has_avx())
			this->vpsrldq(x86::Xmm(dst.id()), x86::Xmm(src.id()), vsize);
		else
		{
			this->movdqa(x86::Xmm(dst.id()), x86::Xmm(src.id()));
			this->psrldq(x86::Xmm(dst.id()), vsize);
		}
	}
}

void asmjit::simd_builder::vec_extract_gpr(u32 esize, const x86::Gp& dst, const Operand& src)
{
	if (esize == 8 && utils::has_avx())
		this->vmovq(dst.r64(), x86::Xmm(src.id()));
	else if (esize == 8)
		this->movq(dst.r64(), x86::Xmm(src.id()));
	else if (esize == 4 && utils::has_avx())
		this->vmovd(dst.r32(), x86::Xmm(src.id()));
	else if (esize == 4)
		this->movd(dst.r32(), x86::Xmm(src.id()));
	else if (esize == 2 && utils::has_avx())
		this->vpextrw(dst.r32(), x86::Xmm(src.id()), 0);
	else if (esize == 2)
		this->pextrw(dst.r32(), x86::Xmm(src.id()), 0);
	else
		fmt::throw_exception("Unimplemented");
}

#endif /* X86 */
