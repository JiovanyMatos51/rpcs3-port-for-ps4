#include "util/types.hpp"
#include "util/sysinfo.hpp"
#include "Utilities/Thread.h"
#include "JIT.h"
#include "StrFmt.h"
#include "File.h"
#include "util/logs.hpp"
#include "mutex.h"
#include "util/vm.hpp"
#include "util/asm.hpp"
#include "Crypto/unzip.h"

#include <charconv>
#include <cerrno>

#if defined(__APPLE__)
#include <pthread.h>
#elif defined(ANDROID)
#include "util/cctype.hpp"
#endif

LOG_CHANNEL(jit_log, "JIT");

#ifdef LLVM_AVAILABLE

#include <unordered_map>

#ifdef _MSC_VER
#pragma warning(push, 0)
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wstrict-aliasing"
#pragma GCC diagnostic ignored "-Wredundant-decls"
#pragma GCC diagnostic ignored "-Weffc++"
#pragma GCC diagnostic ignored "-Wmissing-noreturn"
#endif
#include <llvm/Support/CodeGen.h>
#include "llvm/Support/TargetSelect.h"
#ifdef RPCS3_PS4
#include "llvm/Config/llvm-config.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/DynamicLibrary.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SmallVectorMemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "util/ps4_virtual_memory.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>
#endif
#include "llvm/TargetParser/Host.h"
#include "llvm/ExecutionEngine/ExecutionEngine.h"
#include "llvm/ExecutionEngine/RTDyldMemoryManager.h"
#include "llvm/ExecutionEngine/ObjectCache.h"
#include "llvm/ExecutionEngine/JITEventListener.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Object/SymbolSize.h"
#ifdef _MSC_VER
#pragma warning(pop)
#else
#pragma GCC diagnostic pop
#endif

#ifdef ARCH_ARM64
#include "Emu/CPU/Backends/AArch64/AArch64Common.h"
#endif

namespace
{
	thread_local std::string* g_llvm_fatal_message = nullptr;

#ifdef RPCS3_PS4
	// 0.29: inline code generations after a refused 'LLVM JIT' helper (one counter for every run_recoverable_llvm
	// instantiation, so the rate-limited log's #n is the real total)
	atomic_t<u32> g_ps4_inline_codegen{0};

	// jit_compiler instances alive, and process totals of ps4_add_and_free() / MemoryManager2 refusals
	atomic_t<u32> g_ps4_live_compilers{0};
	atomic_t<u64> g_ps4_freed_modules{0};
	atomic_t<u64> g_ps4_freed_emitted_bytes{0};
	atomic_t<u64> g_ps4_refused_sections{0};
#endif

	template <typename F>
	bool run_recoverable_llvm(F&& func, std::string& error)
	{
		error.clear();

		// Run LLVM codegen in a disposable thread. If LLVM invokes the fatal
		// handler, only this helper thread exits.
#ifdef RPCS3_PS4
		// 0.29: the helper thread is optional. When the console refuses it (thread resources exhausted), generate the
		// code on this thread instead of ending the emulator in thread_base::start(). An LLVM fatal error then ends
		// this thread through fmt::throw_exception (the handler sees g_llvm_fatal_message == nullptr here), as for
		// the non-recoverable jit_compiler::add(). 'body' captures func by reference, so moving it into the stand-by
		// object leaves func intact, and only one of the two calls ever runs.
		auto body = [&]()
		{
			g_llvm_fatal_message = &error;

			std::forward<F>(func)();

			g_llvm_fatal_message = nullptr;
		};

		named_thread worker(stx::try_launch, "LLVM JIT", std::move(body));

		if (!worker.ps4_try_start())
		{
			if (const u32 count = ++g_ps4_inline_codegen; count <= 4 || (count & (count - 1)) == 0)
			{
				jit_log.warning("PS4: 'LLVM JIT' helper thread not started (thread resources exhausted); generating code on this thread without crash recovery (#%u)", count);
			}

			std::forward<F>(func)();
			return true;
		}
#else
		named_thread worker("LLVM JIT", [&]()
		{
#if defined(__APPLE__)
			pthread_jit_write_protect_np(false);
#endif
			g_llvm_fatal_message = &error;

			std::forward<F>(func)();

			g_llvm_fatal_message = nullptr;
#if defined(__APPLE__)
			pthread_jit_write_protect_np(true);
#endif
		});
#endif

		worker();
		const bool result = static_cast<thread_state>(worker) == thread_state::finished;

		if (!result && error.empty())
		{
			error = "LLVM crash recovery invoked";
		}

		return result;
	}
}

const bool jit_initialize = []() -> bool
{
	llvm::InitializeNativeTarget();
	llvm::InitializeNativeTargetAsmPrinter();
	llvm::InitializeNativeTargetAsmParser();
	LLVMLinkInMCJIT();
#ifdef RPCS3_PS4
	// The console links libc/libm statically. Looking these up through the
	// process dynamic-symbol table cannot resolve generated LLVM libcalls.
	// Register their actual SDK implementations with LLVM's symbol resolver.
#define PS4_JIT_MEMORY(name) llvm::sys::DynamicLibrary::AddSymbol(#name, reinterpret_cast<void*>(&::name))
	PS4_JIT_MEMORY(memcpy); PS4_JIT_MEMORY(memmove); PS4_JIT_MEMORY(memset); PS4_JIT_MEMORY(memcmp);
#undef PS4_JIT_MEMORY
#define PS4_JIT_UNARY(name) \
	llvm::sys::DynamicLibrary::AddSymbol(#name, reinterpret_cast<void*>(static_cast<double(*)(double)>(&::name))); \
	llvm::sys::DynamicLibrary::AddSymbol(#name "f", reinterpret_cast<void*>(static_cast<float(*)(float)>(&::name##f)))
	PS4_JIT_UNARY(sqrt); PS4_JIT_UNARY(cbrt); PS4_JIT_UNARY(sin); PS4_JIT_UNARY(cos);
	PS4_JIT_UNARY(tan); PS4_JIT_UNARY(asin); PS4_JIT_UNARY(acos); PS4_JIT_UNARY(atan);
	PS4_JIT_UNARY(exp); PS4_JIT_UNARY(exp2); PS4_JIT_UNARY(log); PS4_JIT_UNARY(log2);
	PS4_JIT_UNARY(log10); PS4_JIT_UNARY(floor); PS4_JIT_UNARY(ceil); PS4_JIT_UNARY(trunc);
	PS4_JIT_UNARY(round); PS4_JIT_UNARY(rint); PS4_JIT_UNARY(nearbyint);
#undef PS4_JIT_UNARY
#define PS4_JIT_BINARY(name) \
	llvm::sys::DynamicLibrary::AddSymbol(#name, reinterpret_cast<void*>(static_cast<double(*)(double, double)>(&::name))); \
	llvm::sys::DynamicLibrary::AddSymbol(#name "f", reinterpret_cast<void*>(static_cast<float(*)(float, float)>(&::name##f)))
	PS4_JIT_BINARY(pow); PS4_JIT_BINARY(atan2); PS4_JIT_BINARY(fmod);
	PS4_JIT_BINARY(remainder); PS4_JIT_BINARY(copysign);
#undef PS4_JIT_BINARY
	llvm::sys::DynamicLibrary::AddSymbol("fma", reinterpret_cast<void*>(static_cast<double(*)(double, double, double)>(&::fma)));
	llvm::sys::DynamicLibrary::AddSymbol("fmaf", reinterpret_cast<void*>(static_cast<float(*)(float, float, float)>(&::fmaf)));
#endif
	return true;
}();

#ifdef RPCS3_PS4
// rpcs3/ps4_main.cpp and rpcs3/ps4_fatal.cpp (PS4 frontend).
[[noreturn]] void report_fatal_error(std::string_view text, bool is_html = false, bool include_help_text = true);
void ps4_fatal_note(const char* prefix, const char* detail) noexcept;
#endif

[[noreturn]] static void null(const char* name)
{
#ifdef RPCS3_PS4
	// Generated code reached a symbol that was never resolved.
	// Record the name durably first (startup.log, fsync, no allocation): RPCS3.log may never
	// be flushed if the process is closed right after. No C++ exception crosses the JIT frames
	// below: fmt::throw_exception ends this thread through thread_ctrl::emergency_exit, which
	// records fatal.txt and returns the frontend to the XMB (ps4_note_thread_fatal).
	ps4_fatal_note("LLVM: generated code called unresolved symbol: ", name ? name : "?");
#endif
	fmt::throw_exception("Null function: %s", name);
}

namespace vm
{
	extern u8* const g_sudo_addr;
}

static shared_mutex null_mtx;

static std::unordered_map<std::string, u64> null_funcs;

static u64 make_null_function(const std::string& name)
{
	if (name.starts_with("__0x"))
	{
		u32 addr = -1;
		auto res = std::from_chars(name.c_str() + 4, name.c_str() + name.size(), addr, 16);

		if (res.ec == std::errc() && res.ptr == name.c_str() + name.size() && addr < 0x8000'0000)
		{
			fmt::throw_exception("Unhandled symbols cementing! (name='%s'", name);
		}
	}

	std::lock_guard lock(null_mtx);

	if (u64& func_ptr = null_funcs[name]) [[likely]]
	{
		// Already exists
		return func_ptr;
	}
	else
	{
		using namespace asmjit;

		// Build a "null" function that contains its name
		const auto func = build_function_asm<void (*)()>("NULL", [&](native_asm& c, auto& args)
		{
#if defined(ARCH_X64)
			Label data = c.newLabel();
			c.lea(args[0], x86::qword_ptr(data, 0));
			c.jmp(Imm(&null));
			c.align(AlignMode::kCode, 16);
			c.bind(data);

			// Copy function name bytes
			for (char ch : name)
				c.db(ch);
			c.db(0);
			c.align(AlignMode::kData, 16);
#else
			// AArch64 implementation
			Label data = c.newLabel();
			Label jump_address = c.newLabel();
			c.ldr(args[0], arm::ptr(data, 0));
			c.ldr(a64::x14, arm::ptr(jump_address, 0));
			c.br(a64::x14);

			// Data frame
			c.align(AlignMode::kCode, 16);
			c.bind(jump_address);
			c.embedUInt64(reinterpret_cast<u64>(&null));

			c.align(AlignMode::kData, 16);
			c.bind(data);
			c.embed(name.c_str(), name.size());
			c.embedUInt8(0U);
			c.align(AlignMode::kData, 16);
#endif
		});

		func_ptr = reinterpret_cast<u64>(func);
		return func_ptr;
	}
}

struct JITAnnouncer : llvm::JITEventListener
{
	void notifyObjectLoaded(u64, const llvm::object::ObjectFile& obj, const llvm::RuntimeDyld::LoadedObjectInfo& info) override
	{
		using namespace llvm;

		object::OwningBinary<object::ObjectFile> debug_obj_ = info.getObjectForDebug(obj);
		if (!debug_obj_.getBinary())
		{
#ifdef __linux__
			jit_log.error("LLVM: Failed to announce JIT events (no debug object)");
#endif
			return;
		}

		const object::ObjectFile& debug_obj = *debug_obj_.getBinary();

		for (const auto& [sym, size] : computeSymbolSizes(debug_obj))
		{
			Expected<object::SymbolRef::Type> type_ = sym.getType();
			if (!type_ || *type_ != object::SymbolRef::ST_Function)
				continue;

			Expected<StringRef> name = sym.getName();
			if (!name)
				continue;

			Expected<u64> addr = sym.getAddress();
			if (!addr)
				continue;

			jit_announce(*addr, size, {name->data(), name->size()});
		}
	}
};

// Simple memory manager
struct MemoryManager1 : llvm::RTDyldMemoryManager
{
	// 256 MiB for code or data
	static constexpr u64 c_max_size = 0x1000'0000;

	// Allocation unit (2M)
	static constexpr u64 c_page_size = 2 * 1024 * 1024;

	// Reserve 256 MiB blocks
	void* m_code_mems = nullptr;
	void* m_data_ro_mems = nullptr;
	void* m_data_rw_mems = nullptr;

	u64 code_ptr = 0;
	u64 data_ro_ptr = 0;
	u64 data_rw_ptr = 0;

	// First fallback for non-existing symbols
	// May be a memory container internally
	std::function<u64(const std::string&)> m_symbols_cement;

#ifdef RPCS3_PS4
	// Commit unit: 256 KiB instead of 2 MiB pages, so 20+ arenas do not each hold up to 2 MiB per stream unused
	static constexpr u64 c_ps4_commit_unit = 256 * 1024;

	// Committed end of the code and data streams (stream offsets), and the bytes this arena counted in the JIT ledger
	u64 m_ps4_code_committed = 0;
	u64 m_ps4_data_committed = 0;
	u64 m_ps4_counted = 0;

	// Refusal record of the owning jit_compiler (it outlives this manager); null for none
	jit_compiler::ps4_memory_refusal* m_ps4_refusal = nullptr;

	// Scratch blocks of refused sections, chained through their first pointer, freed with the manager
	void* m_ps4_scratch = nullptr;

	// The arena could not hold a section (commit refused after the retries, test cap, or a stream past its range).
	// RuntimeDyld has no recoverable failure for a section, so it gets a heap block and the refusal is recorded: the
	// owner checks the record after loading and never runs that object's code (the PPU link drops the whole engine).
	u8* ps4_refused(uptr size, u64 align)
	{
		const uptr alignment = align ? align : 16;
		u8* const block = static_cast<u8*>(std::malloc(sizeof(void*) + alignment + size));

		if (m_ps4_refusal)
		{
			m_ps4_refusal->bytes += size;
			m_ps4_refusal->sections++;
		}

		if (!block)
		{
			return nullptr;
		}

		*reinterpret_cast<void**>(block) = m_ps4_scratch;
		m_ps4_scratch = block;
		return reinterpret_cast<u8*>(utils::align<uptr>(reinterpret_cast<uptr>(block) + sizeof(void*), alignment));
	}

	// Backs the stream up to 'end' (stream offset) in commit units; false (nothing moved) when refused
	bool ps4_commit_to(void* block, u64& committed, u64 end, utils::protection prot, bool code)
	{
		end = utils::align(end, c_ps4_commit_unit);

		if (end <= committed)
		{
			return true;
		}

		const usz size = end - committed;

		if (utils::ps4::jit::test_cap_refuses(size))
		{
			return false;
		}

		const int native = prot == utils::protection::wx ? PROT_READ | PROT_WRITE | PROT_EXEC : PROT_READ | PROT_WRITE;

		// Link and compile threads may wait for the pool (heap trim, budget shedders) before giving up
		if (!utils::ps4::jit::commit(static_cast<u8*>(block) + (committed % c_max_size), size, native, 2000, code ? "PPU link code" : "PPU link data"))
		{
			return false;
		}

		committed = end;
		m_ps4_counted += size;
		utils::ps4::jit::account(utils::ps4::jit::owner::link, static_cast<s64>(size));
		return true;
	}
#endif

	MemoryManager1(std::function<u64(const std::string&)> symbols_cement = {})
		: m_symbols_cement(std::move(symbols_cement))
	{
#ifdef RPCS3_PS4
		auto ptr = static_cast<u8*>(utils::ps4::memory::reserve_jit(c_max_size * 3));
#else
		auto ptr = reinterpret_cast<u8*>(utils::memory_reserve(c_max_size * 3, true));
#endif
		if (!ptr)
		{
			fmt::throw_exception("LLVM: unable to reserve JIT arena (size=0x%x, errno=%d)", c_max_size * 3, errno);
		}
		m_code_mems = ptr;
		// ptr += c_max_size;
		// m_data_ro_mems = ptr;
		 ptr += c_max_size;
		m_data_rw_mems = ptr;
	}

	MemoryManager1(const MemoryManager1&) = delete;

	MemoryManager1& operator=(const MemoryManager1&) = delete;

	~MemoryManager1() override
	{
		// Hack: don't release to prevent reuse of address space, see jit_announce
		// constexpr auto how_much = [](u64 pos) { return utils::align(pos, pos < c_page_size ? c_page_size / 4 : c_page_size); };
		// utils::memory_decommit(m_code_mems, how_much(code_ptr));
		// utils::memory_decommit(m_data_ro_mems, how_much(data_ro_ptr));
		// utils::memory_decommit(m_data_rw_mems, how_much(data_rw_ptr));
#ifdef RPCS3_PS4
		// Auxiliary compilers are created for each object module. Decommit
		// releases physical pages but keeps 768 MiB of virtual space forever.
		// The console's automatic address pool ran out after eleven modules.
		// The engine owns this arena; no executable survives its destruction.
		utils::memory_release(m_code_mems, c_max_size * 3);
		utils::ps4::jit::account(utils::ps4::jit::owner::link, -static_cast<s64>(m_ps4_counted));

		for (void* block = m_ps4_scratch; block;)
		{
			void* const next = *static_cast<void**>(block);
			std::free(block);
			block = next;
		}
#else
		utils::memory_decommit(m_code_mems, c_max_size * 3, true);
#endif
	}

	llvm::JITSymbol findSymbol(const std::string& name) override
	{
		u64 addr = RTDyldMemoryManager::getSymbolAddress(name);

		if (!addr && m_symbols_cement)
		{
			addr = m_symbols_cement(name);
		}

		if (!addr)
		{
			addr = make_null_function(name);

			if (!addr)
			{
				fmt::throw_exception("Failed to link '%s'", name);
			}
		}

		return {addr, llvm::JITSymbolFlags::Exported};
	}

	u8* allocate(u64& alloc_pos, void* block, uptr size, u64 align, utils::protection prot)
	{
		align = align ? align : 16;
 
		const u64 sizea = utils::align(size, align);

		if (!size || align > c_page_size || sizea > c_max_size || sizea < size)
		{
			jit_log.fatal("Unsupported size/alignment (size=0x%x, align=0x%x)", size, align);
			return nullptr;
		}

		u64 oldp = alloc_pos;

		u64 olda = utils::align(oldp, align);

		ensure(olda >= oldp);
		ensure(olda < ~sizea);

		u64 newp = olda + sizea;

#ifdef RPCS3_PS4
		{
			const bool code = block == m_code_mems;
			u64& committed = code ? m_ps4_code_committed : m_ps4_data_committed;

			// A stream past its 256 MiB range would wrap onto its own start (upstream addresses it modulo the range);
			// a refused commit leaves the allocation counter where it was, so the next section tries the same pages
			if (newp > c_max_size || !ps4_commit_to(block, committed, newp, prot, code))
			{
				static atomic_t<u32> s_logged{0};

				if (const u32 n = ++s_logged; n <= 8 || (n & (n - 1)) == 0)
				{
					jit_log.error("PS4: PPU link arena: no memory for a %s section of %u KiB (stream at %u KiB%s); the object is not used (#%u)",
						code ? "code" : "data", size >> 10, oldp >> 10, newp > c_max_size ? ", past its range" : "", n);
				}

				return ps4_refused(size, align);
			}

			alloc_pos = newp;
			return reinterpret_cast<u8*>(block) + olda;
		}
#endif

		if ((newp - 1) / c_max_size != (oldp - 1) / c_max_size)
		{
			constexpr usz num_of_allocations = 1;

			if ((newp - 1) / c_max_size > num_of_allocations)
			{
				// Allocating more than one region does not work for relocations, needs more robust solution
				fmt::throw_exception("Out of memory (size=0x%x, align=0x%x)", size, align);
			}
		}

		// Update allocation counter
		alloc_pos = newp;

		constexpr usz page_quarter = c_page_size / 4;

		// Optimization: split the first allocation to 512 KiB for single-module compilers
		if (oldp < c_page_size && align < page_quarter && (std::min(newp, c_page_size) - 1) / page_quarter != (oldp - 1) / page_quarter)
		{
			const u64 pagea = utils::align(oldp, page_quarter);
			const u64 psize = utils::align(std::min(newp, c_page_size) - pagea, page_quarter);
			utils::memory_commit(reinterpret_cast<u8*>(block) + (pagea % c_max_size), psize, prot);

			// Advance
			oldp = pagea + psize;
		}

		if ((newp - 1) / c_page_size != (oldp - 1) / c_page_size)
		{
			// Allocate pages on demand
			const u64 pagea = utils::align(oldp, c_page_size);
			const u64 psize = utils::align(newp - pagea, c_page_size);
			utils::memory_commit(reinterpret_cast<u8*>(block) + (pagea % c_max_size), psize, prot);
		}

		return reinterpret_cast<u8*>(block) + (olda % c_max_size);
	}

	u8* allocateCodeSection(uptr size, uint align, uint /*sec_id*/, llvm::StringRef /*sec_name*/) override
	{
		return allocate(code_ptr, m_code_mems, size, align, utils::protection::wx);
	}

	u8* allocateDataSection(uptr size, uint align, uint /*sec_id*/, llvm::StringRef /*sec_name*/, bool is_ro) override
	{
		if (is_ro)
		{
			// Disabled
			//return allocate(data_ro_ptr, m_data_ro_mems, size, align, utils::protection::rw);
		}

		return allocate(data_rw_ptr, m_data_rw_mems, size, align, utils::protection::rw);
	}

	bool finalizeMemory(std::string* = nullptr) override
	{
		return false;
	}

	void registerEHFrames(u8*, u64, usz) override
	{
	}

	void deregisterEHFrames() override
	{
	}
};

// Simple memory manager
struct MemoryManager2 : llvm::RTDyldMemoryManager
{
	// First fallback for non-existing symbols
	// May be a memory container internally
	std::function<u64(const std::string&)> m_symbols_cement;

#ifdef RPCS3_PS4
	// Refusal record of the owning jit_compiler (it outlives this manager); null for none
	jit_compiler::ps4_memory_refusal* m_ps4_refusal = nullptr;

	// Scratch blocks of refused sections, chained through their first pointer, freed with the manager
	void* m_ps4_scratch = nullptr;

	// jit_runtime refused the memory (direct memory exhausted, or its 1 GiB range used up). RuntimeDyld has no
	// recoverable failure for a section (null ends in report_fatal_error, a thread fatal outside run_recoverable_llvm),
	// so the section gets a heap block and the refusal is recorded: the caller checks the record after loading,
	// never runs that code and drops the compiler. Null only when the heap refuses too.
	u8* ps4_refused(uptr size, uint align)
	{
		const uptr alignment = align ? align : 16;
		u8* const block = static_cast<u8*>(std::malloc(sizeof(void*) + alignment + size));

		if (!block)
		{
			return nullptr;
		}

		*reinterpret_cast<void**>(block) = m_ps4_scratch;
		m_ps4_scratch = block;

		if (m_ps4_refusal)
		{
			m_ps4_refusal->bytes += size;
			m_ps4_refusal->sections++;
		}

		return reinterpret_cast<u8*>(utils::align<uptr>(reinterpret_cast<uptr>(block) + sizeof(void*), alignment));
	}
#endif

	MemoryManager2(std::function<u64(const std::string&)> symbols_cement = {}) noexcept
		: m_symbols_cement(std::move(symbols_cement))
	{
	}

	~MemoryManager2() override
	{
#ifdef RPCS3_PS4
		for (void* block = m_ps4_scratch; block;)
		{
			void* const next = *static_cast<void**>(block);
			std::free(block);
			block = next;
		}
#endif
	}

	llvm::JITSymbol findSymbol(const std::string& name) override
	{
		u64 addr = RTDyldMemoryManager::getSymbolAddress(name);

		if (!addr && m_symbols_cement)
		{
			addr = m_symbols_cement(name);
		}

		if (!addr)
		{
			addr = make_null_function(name);

			if (!addr)
			{
				fmt::throw_exception("Failed to link '%s' (MM2)", name);
			}
		}

		return {addr, llvm::JITSymbolFlags::Exported};
	}

	u8* allocateCodeSection(uptr size, uint align, uint /*sec_id*/, llvm::StringRef /*sec_name*/) override
	{
#ifdef RPCS3_PS4
		if (u8* const ptr = jit_runtime::ps4_try_alloc(size, align, true))
		{
			return ptr;
		}

		return ps4_refused(size, align);
#else
		return jit_runtime::alloc(size, align, true);
#endif
	}

	u8* allocateDataSection(uptr size, uint align, uint /*sec_id*/, llvm::StringRef /*sec_name*/, bool /*is_ro*/) override
	{
#ifdef RPCS3_PS4
		if (u8* const ptr = jit_runtime::ps4_try_alloc(size, align, false))
		{
			return ptr;
		}

		return ps4_refused(size, align);
#else
		return jit_runtime::alloc(size, align, false);
#endif
	}

	bool finalizeMemory(std::string* = nullptr) override
	{
		return false;
	}

	void registerEHFrames(u8*, u64, usz) override
	{
	}

	void deregisterEHFrames() override
	{
	}
};

// Helper class
class ObjectCache final : public llvm::ObjectCache
{
	const std::string& m_path;
	const std::add_pointer_t<jit_compiler> m_compiler = nullptr;

public:
	ObjectCache(const std::string& path, jit_compiler* compiler = nullptr)
		: m_path(path)
		, m_compiler(compiler)
	{
	}

	~ObjectCache() override = default;

	void notifyObjectCompiled(const llvm::Module* _module, llvm::MemoryBufferRef obj) override
	{
		std::string name = m_path;

		name.append(_module->getName());
		//fs::file(name, fs::rewrite).write(obj.getBufferStart(), obj.getBufferSize());
		name.append(".gz");

		if (!obj.getBufferSize())
		{
			jit_log.error("LLVM: Nothing to write: %s", name);
			return;
		}

		ensure(m_compiler);

		fs::pending_file module_file;

		if (!module_file.open((name)))
		{
			jit_log.error("LLVM: Failed to create module file: %s (%s)", name, fs::g_tls_error);
			return;
		}

		// Bold assumption about upper limit of space consumption
		const usz max_size = obj.getBufferSize() * 4;

		if (!m_compiler->add_sub_disk_space(0 - max_size))
		{
			jit_log.error("LLVM: Failed to create module file: %s (not enough disk space left)", name);
			return;
		}

		if (!zip(obj.getBufferStart(), obj.getBufferSize(), module_file.file))
		{
			jit_log.error("LLVM: Failed to compress module: %s", std::string(_module->getName()));
			return;
		}

		jit_log.trace("LLVM: Created module: %s", std::string(_module->getName()));

		// Restore space that was overestimated
		ensure(m_compiler->add_sub_disk_space(max_size - module_file.file.size()));
		module_file.commit();
	}

	static std::unique_ptr<llvm::MemoryBuffer> load(const std::string& path)
	{
		if (fs::file cached{path + ".gz", fs::read})
		{
			const std::vector<u8> cached_data = cached.to_vector<u8>();

			if (cached_data.empty()) [[unlikely]]
			{
				return nullptr;
			}

			const std::vector<u8> out = unzip(cached_data);

			if (out.empty())
			{
				jit_log.error("LLVM: Failed to unzip module: '%s'", path);
				return nullptr;
			}

			auto buf = llvm::WritableMemoryBuffer::getNewUninitMemBuffer(out.size());
			std::memcpy(buf->getBufferStart(), out.data(), out.size());
			return buf;
		}

		if (fs::file cached{path, fs::read})
		{
			if (cached.size() == 0) [[unlikely]]
			{
				return nullptr;
			}

			auto buf = llvm::WritableMemoryBuffer::getNewUninitMemBuffer(cached.size());
			cached.read(buf->getBufferStart(), buf->getBufferSize());
			return buf;
		}

		return nullptr;
	}

	std::unique_ptr<llvm::MemoryBuffer> getObject(const llvm::Module* _module) override
	{
		std::string path = m_path;
		path.append(_module->getName().data());

		if (auto buf = load(path))
		{
			jit_log.notice("LLVM: Loaded module: %s", _module->getName().data());
			return buf;
		}

		return nullptr;
	}
};

std::string jit_compiler::cpu(std::string_view _cpu)
{
	std::string m_cpu = std::string(_cpu);

#if defined(RPCS3_PS4) && defined(ARCH_X64)
	// 0.33: the console always targets its own CPU, in every config mode (automatic, user-tuned, custom YAML,
	// keep-user-settings). A 'Use LLVM CPU' value copied from a PC config (haswell, znver*, bdver*, icelake...)
	// would make the JIT emit BMI2/XOP/FMA4/GFNI (SIGILL in guest code), and make cpu_translator run AVX-512,
	// GFNI or VNNI on the compile thread while folding constants. This also keeps PPU object names on the host
	// CPU, so cache files stay valid. A name equal to the host's (verify_llvm passes "btver2") is kept as is.
	if (!m_cpu.empty())
	{
		static const std::string s_host = []() -> std::string
		{
			std::string host = llvm::sys::getHostCPUName().str();
			return host == "generic" ? std::string(fallback_cpu_detection()) : host;
		}();

		if (m_cpu != s_host)
		{
			static atomic_t<bool> s_warned{false};

			if (!s_warned.exchange(true))
			{
				jit_log.warning("PS4: ignoring 'Use LLVM CPU' = '%s'; the JIT always targets the host CPU '%s'", m_cpu, s_host);
			}

			m_cpu.clear();
		}
	}
#endif

	if (m_cpu.empty())
	{
		m_cpu = llvm::sys::getHostCPUName().str();

		if (m_cpu == "generic")
		{
			// Try to detect a best match based on other criteria
			m_cpu = fallback_cpu_detection();
		}

		if (m_cpu == "sandybridge" ||
			m_cpu == "ivybridge" ||
			m_cpu == "haswell" ||
			m_cpu == "broadwell" ||
			m_cpu == "skylake" ||
			m_cpu == "skylake-avx512" ||
			m_cpu == "cascadelake" ||
			m_cpu == "cooperlake" ||
			m_cpu == "cannonlake" ||
			m_cpu == "icelake" ||
			m_cpu == "icelake-client" ||
			m_cpu == "icelake-server" ||
			m_cpu == "tigerlake" ||
			m_cpu == "rocketlake" ||
			m_cpu == "alderlake" ||
			m_cpu == "raptorlake" ||
			m_cpu == "meteorlake")
		{
			// Downgrade if AVX is not supported by some chips
			if (!utils::has_avx())
			{
				m_cpu = "nehalem";
			}
		}

		if (m_cpu == "skylake-avx512" ||
			m_cpu == "cascadelake" ||
			m_cpu == "cooperlake" ||
			m_cpu == "cannonlake" ||
			m_cpu == "icelake" ||
			m_cpu == "icelake-client" ||
			m_cpu == "icelake-server" ||
			m_cpu == "tigerlake" ||
			m_cpu == "rocketlake")
		{
			// Downgrade if AVX-512 is disabled or not supported
			if (!utils::has_avx512())
			{
				m_cpu = "skylake";
			}
		}

		if (m_cpu == "znver1" && utils::has_clwb())
		{
			// Upgrade
			m_cpu = "znver2";
		}

		if ((m_cpu == "znver3" || m_cpu == "goldmont" || m_cpu == "alderlake" || m_cpu == "raptorlake" || m_cpu == "meteorlake") && utils::has_avx512_icl())
		{
			// Upgrade
			m_cpu = "icelake-client";
		}

		if (m_cpu == "goldmont" && utils::has_avx2())
		{
			// Upgrade
			m_cpu = "alderlake";
		}
	}

	return m_cpu;
}

std::string jit_compiler::triple1()
{
#if defined(_WIN32)
	return llvm::Triple::normalize(llvm::sys::getProcessTriple());
#elif defined(__APPLE__) && defined(ARCH_X64)
	return llvm::Triple::normalize("x86_64-unknown-linux-gnu");
#elif (defined(__ANDROID__) || defined(__APPLE__)) && defined(ARCH_ARM64)
	return llvm::Triple::normalize("aarch64-unknown-linux-android"); // Set environment to android to reserve x18
#elif defined(__ANDROID__) && defined(ARCH_X64)
	return llvm::Triple::normalize("x86_64-unknown-linux-android");
#else
	return llvm::Triple::normalize(llvm::sys::getProcessTriple());
#endif
}

std::string jit_compiler::triple2()
{
#if defined(_WIN32) && defined(ARCH_X64)
	return llvm::Triple::normalize("x86_64-unknown-linux-gnu");
#elif defined(_WIN32) && defined(ARCH_ARM64)
	return llvm::Triple::normalize("aarch64-unknown-linux-gnu");
#elif defined(__APPLE__) && defined(ARCH_X64)
	return llvm::Triple::normalize("x86_64-unknown-linux-gnu");
#elif (defined(__ANDROID__) || defined(__APPLE__)) && defined(ARCH_ARM64)
	return llvm::Triple::normalize("aarch64-unknown-linux-android"); // Set environment to android to reserve x18
#elif defined(__ANDROID__) && defined(ARCH_X64)
	return llvm::Triple::normalize("x86_64-unknown-linux-android"); // Set environment to android to reserve x18
#else
	return llvm::Triple::normalize(llvm::sys::getProcessTriple());
#endif
}

bool jit_compiler::add_sub_disk_space(ssz space)
{
	if (space >= 0)
	{
		ensure(m_disk_space.fetch_add(space) < ~static_cast<usz>(space));
		return true;
	}

	return m_disk_space.fetch_op([sub_size = static_cast<usz>(0 - space)](usz& val)
	{
		if (val >= sub_size)
		{
			val -= sub_size;
			return true;
		}

		return false;
	}).second;
}

#if defined(RPCS3_PS4) && defined(ARCH_X64)
// Feature switches passed to every engine (setMAttrs); the same for the whole process
static std::vector<std::string> ps4_feature_switches()
{
	std::vector<std::string> attributes;

	// A CPU name alone can enable AVX even when the kernel's XCR0 does not
	// permit its register state. Use RPCS3's CPUID + OSXSAVE/XCR0 checks.
	if (!utils::has_avx())
	{
		attributes.emplace_back("-avx");
		attributes.emplace_back("-f16c");
	}
	if (!utils::has_avx2()) attributes.emplace_back("-avx2");
	if (!utils::has_fma3()) attributes.emplace_back("-fma");
	if (!utils::has_avx512()) attributes.emplace_back("-avx512f");

	// 0.33: also disable every feature LLVM's own CPUID/XGETBV probe reports absent (BMI2, ADX, XOP, FMA4, TBM,
	// LWP, GFNI, VAES, VPCLMULQDQ, AVX-VNNI, SHA, MWAITX, CLZERO...), so no CPU name can make the JIT emit an
	// instruction Jaguar lacks. It only ever removes features: one misreported as absent costs code quality,
	// never correctness. "64bit" is the x86-64 mode bit, not an extension, and is never removed. Computed once.
	static const std::vector<std::string> s_host_absent = []()
	{
		std::vector<std::string> result;

		for (const auto& feature : llvm::sys::getHostCPUFeatures())
		{
			if (!feature.getValue() && feature.getKey() != "64bit")
			{
				result.emplace_back("-" + feature.getKey().str());
			}
		}

		// StringMap order is hash order; sort so the logged list and the feature string are stable
		std::sort(result.begin(), result.end());
		return result;
	}();

	attributes.insert(attributes.end(), s_host_absent.begin(), s_host_absent.end());
	return attributes;
}
#endif

jit_compiler::jit_compiler(const std::unordered_map<std::string, u64>& _link, std::string_view _cpu, u32 flags, std::function<u64(const std::string&)> symbols_cement)
	: m_context(new llvm::LLVMContext)
	, m_cpu(cpu(_cpu))
{
	[[maybe_unused]] static const bool s_install_llvm_error_handler = []()
	{
		llvm::remove_fatal_error_handler();
		llvm::install_fatal_error_handler([](void*, const char* msg, bool)
		{
			const std::string_view out = msg ? msg : "";

			if (g_llvm_fatal_message)
			{
				*g_llvm_fatal_message = out;
				thread_ctrl::silent_exit();
			}

			fmt::throw_exception("LLVM Emergency Exit Invoked: '%s'", out);
		}, nullptr);

#ifdef RPCS3_PS4
		// Without a handler LLVM writes "LLVM ERROR: out of memory" to fd 2 (goes nowhere on
		// this kernel) and calls abort() (CE-34878-0). The PS4 report_fatal_error copies into
		// fixed buffers, records fatal.txt/startup.log and parks: no allocation on this path.
		llvm::install_bad_alloc_error_handler([](void*, const char* msg, bool)
		{
			::report_fatal_error(msg ? std::string_view{msg} : std::string_view{"LLVM out of memory"});
		}, nullptr);
#endif

		return true;
	}();

	std::string result;

	auto null_mod = std::make_unique<llvm::Module> ("null_", *m_context);
	null_mod->setTargetTriple(llvm::Triple(jit_compiler::triple1()));

	std::unique_ptr<llvm::RTDyldMemoryManager> mem;

	if (_link.empty())
	{
		// Auxiliary JIT (does not use custom memory manager, only writes the objects)
		if (flags & 0x1)
		{
			mem = std::make_unique<MemoryManager1>(std::move(symbols_cement));
#ifdef RPCS3_PS4
			static_cast<MemoryManager1*>(mem.get())->m_ps4_refusal = &m_ps4_refusal;
#endif
		}
		else
		{
			mem = std::make_unique<MemoryManager2>(std::move(symbols_cement));
			null_mod->setTargetTriple(llvm::Triple(jit_compiler::triple2()));
#ifdef RPCS3_PS4
			static_cast<MemoryManager2*>(mem.get())->m_ps4_refusal = &m_ps4_refusal;
#endif
		}
	}
	else
	{
		mem = std::make_unique<MemoryManager1>(std::move(symbols_cement));
#ifdef RPCS3_PS4
		static_cast<MemoryManager1*>(mem.get())->m_ps4_refusal = &m_ps4_refusal;
#endif
	}

	std::vector<std::string> attributes;

#if defined(RPCS3_PS4) && defined(ARCH_X64)
	attributes = ps4_feature_switches();
#endif

#if defined(ARCH_ARM64)
	if (utils::has_sha3())
		attributes.push_back("+sha3");
	else
		attributes.push_back("-sha3");

	if (utils::has_dotprod())
		attributes.push_back("+dotprod");
	else
		attributes.push_back("-dotprod");

	// The recompilers emit i8mm intrinsics (e.g. ummla) gated on utils::has_i8mm().
	// The JIT target features must advertise i8mm too, otherwise the backend fails
	// with "Cannot select: intrinsic %llvm.aarch64.neon.ummla" whenever the resolved
	// -mcpu does not already imply it (e.g. the cortex-a78 fallback on Apple silicon).
	if (utils::has_i8mm())
		attributes.push_back("+i8mm");
	else
		attributes.push_back("-i8mm");

	if (utils::has_sve())
		attributes.push_back("+sve");
	else
		attributes.push_back("-sve");

	if (utils::has_sve2())
		attributes.push_back("+sve2");
	else
		attributes.push_back("-sve2");
#endif

	{
		m_engine.reset(llvm::EngineBuilder(std::move(null_mod))
			.setErrorStr(&result)
			.setEngineKind(llvm::EngineKind::JIT)
			.setMCJITMemoryManager(std::move(mem))
			.setOptLevel(llvm::CodeGenOptLevel::Aggressive)
			.setCodeModel(flags & 0x2 ? llvm::CodeModel::Large : llvm::CodeModel::Small)
#ifdef __APPLE__
			//.setCodeModel(llvm::CodeModel::Large)
#endif
			.setRelocationModel(llvm::Reloc::Model::PIC_)
			.setMAttrs(attributes)
			.setMCPU(m_cpu)
			.create());
	}

	if (!m_engine)
	{
		jit_log.fatal("LLVM: Failed to create ExecutionEngine: %s", result);
		fmt::throw_exception("LLVM: Failed to create ExecutionEngine: %s", result);
	}

#if defined(RPCS3_PS4) && defined(ARCH_X64)
	// 0.33: one line per process with the effective JIT target, for SIGILL triage from RPCS3.log
	static atomic_t<bool> s_target_logged{false};

	if (!s_target_logged.exchange(true))
	{
		std::string features;

		for (const std::string& attr : attributes)
		{
			if (!features.empty())
			{
				features += ',';
			}

			features += attr;
		}

		jit_log.notice("PS4: JIT target CPU '%s' (LLVM host CPU '%s'); %u feature switches: %s", m_cpu, llvm::sys::getHostCPUName().str(), attributes.size(), features);
	}
#endif

	if (!_link.empty())
	{
		for (auto&& [name, addr] : _link)
		{
			m_engine->updateGlobalMapping(name, addr);
		}
	}

#ifdef RPCS3_PS4
	// MCJIT's constructor registers the GDB JIT registration listener, which keeps a full relocated copy of
	// every loaded object until the engine dies (GDBRegistrationListener::ObjectBufferMap). With all PPU
	// modules of a large game linked, that doubled the object memory in the musl heap and exhausted it
	// (Demon's Souls: std::bad_alloc while "Linking PPU Modules", 0.25). No debugger can attach on the
	// console, and JITAnnouncer only feeds desktop profilers (another transient copy per object): skip both.
	m_engine->UnregisterJITEventListener(llvm::JITEventListener::createGDBRegistrationListener());
#else
	if (!_link.empty() || !(flags & 0x1))
	{
		m_engine->RegisterJITEventListener(llvm::JITEventListener::createIntelJITEventListener());
		m_engine->RegisterJITEventListener(new JITAnnouncer);
	}
#endif

	fs::device_stat stats{};

#ifdef RPCS3_PS4
	// 0.33: the SDK libc statvfs returns success without filling its output, so fs::statfs can hand back stack
	// garbage. A small garbage budget refuses every object write ("not enough disk space left": the PPU/SPU caches
	// are rebuilt on every boot). Use only a plausible reading; otherwise keep the default (no budget), which is
	// what PC does when statfs fails.
	const std::string cache_dir = fs::get_cache_dir();
	const bool stat_ok = fs::statfs(cache_dir, stats);
	const bool stat_sane = stat_ok
		&& stats.total_size > 0
		&& stats.total_free <= stats.total_size
		&& stats.avail_free <= stats.total_size
		&& stats.block_size >= 512 && stats.block_size <= 0x100000
		&& (stats.block_size & (stats.block_size - 1)) == 0;

	if (stat_sane)
	{
		m_disk_space = stats.avail_free / 4;
	}

	// Each outcome is logged once per process (an early compiler may run before the cache directory exists)
	static atomic_t<bool> s_disk_logged_ok{false};
	static atomic_t<bool> s_disk_logged_bad{false};

	if (stat_sane)
	{
		if (!s_disk_logged_ok.exchange(true))
		{
			jit_log.notice("PS4: JIT object cache budget %u MiB ('%s': %u MiB available of %u MiB, block %u)", (stats.avail_free / 4) >> 20, cache_dir, stats.avail_free >> 20, stats.total_size >> 20, stats.block_size);
		}
	}
	else if (!s_disk_logged_bad.exchange(true))
	{
		if (!stat_ok)
		{
			jit_log.warning("PS4: statfs('%s') failed (%s); JIT object cache writes are not size-limited", cache_dir, fs::g_tls_error);
		}
		else
		{
			jit_log.warning("PS4: statfs('%s') looks wrong (block %u, total 0x%x, free 0x%x, avail 0x%x); JIT object cache writes are not size-limited", cache_dir, stats.block_size, stats.total_size, stats.total_free, stats.avail_free);
		}
	}
#else
	if (fs::statfs(fs::get_cache_dir(), stats))
	{
		m_disk_space = stats.avail_free / 4;
	}
#endif

#ifdef RPCS3_PS4
	g_ps4_live_compilers++;
#endif
}

jit_compiler& jit_compiler::operator=(thread_state s) noexcept
{
	if (s == thread_state::destroying_context)
	{
		// Release resources explicitly
		m_engine.reset();
		m_context.reset();
	}

	return *this;
}

jit_compiler::~jit_compiler() noexcept
{
#ifdef RPCS3_PS4
	g_ps4_live_compilers--;
#endif
}

void jit_compiler::add(std::unique_ptr<llvm::Module> _module, const std::string& path)
{
	ObjectCache cache{path, this};
	m_engine->setObjectCache(&cache);

	const auto ptr = _module.get();
	m_engine->addModule(std::move(_module));
	m_engine->generateCodeForModule(ptr);
	m_engine->setObjectCache(nullptr);

	for (auto& func : ptr->functions())
	{
		// Delete IR to lower memory consumption
		func.deleteBody();
	}
}

bool jit_compiler::try_add(std::unique_ptr<llvm::Module> _module, const std::string& path, std::string& error)
{
	ObjectCache cache{path, this};
	m_engine->setObjectCache(&cache);

	const auto ptr = _module.get();
	m_engine->addModule(std::move(_module));

	if (!run_recoverable_llvm([&]()
	{
		m_engine->generateCodeForModule(ptr);
	}, error))
	{
		return false;
	}

	m_engine->setObjectCache(nullptr);

	for (auto& func : ptr->functions())
	{
		// Delete IR to lower memory consumption
		func.deleteBody();
	}

	return true;
}

void jit_compiler::add(std::unique_ptr<llvm::Module> _module)
{
	const auto ptr = _module.get();
	m_engine->addModule(std::move(_module));
	m_engine->generateCodeForModule(ptr);

	for (auto& func : ptr->functions())
	{
		// Delete IR to lower memory consumption
		func.deleteBody();
	}
}

bool jit_compiler::try_add(std::unique_ptr<llvm::Module> _module, std::string& error)
{
	const auto ptr = _module.get();
	m_engine->addModule(std::move(_module));

	if (!run_recoverable_llvm([&]()
	{
		m_engine->generateCodeForModule(ptr);
	}, error))
	{
		return false;
	}

	for (auto& func : ptr->functions())
	{
		// Delete IR to lower memory consumption
		func.deleteBody();
	}

	return true;
}

#ifdef RPCS3_PS4
// Freeing object bytes once loaded relies on MCJIT (LoadedObjects is only used for listener keys in ~MCJIT) and
// RuntimeDyld (object bytes are read only inside loadObject; relocations are resolved from its own copies) as of
// llvm-project ca7933e4.
static_assert(LLVM_VERSION_MAJOR == 22, "Re-check that MCJIT and RuntimeDyld never read object bytes after loadObject()");

namespace
{
	atomic_t<u64> g_ps4_released_object_bytes{0};
	atomic_t<u64> g_ps4_released_objects{0};

	// Object bytes the engine no longer reads (see add(path))
	void ps4_release_object_bytes(std::vector<std::unique_ptr<llvm::MemoryBuffer>>& bytes) noexcept
	{
		if (bytes.empty())
		{
			return;
		}

		u64 total = 0;

		for (const auto& buffer : bytes)
		{
			total += buffer ? buffer->getBufferSize() : 0;
		}

		const u64 count = bytes.size();
		bytes.clear();
		bytes.shrink_to_fit();

		g_ps4_released_object_bytes += total;
		g_ps4_released_objects += count;
	}
}
#endif

bool jit_compiler::add(const std::string& path)
{
	auto cache = ObjectCache::load(path);

	if (!cache)
	{
		jit_log.error("ObjectCache: Failed to read file. (path='%s', error=%s)", path, fs::g_tls_error);
		return false;
	}

	if (auto object_file = llvm::object::ObjectFile::createObjectFile(*cache))
	{
#ifdef RPCS3_PS4
		if (!ps4_keep_object_bytes())
		{
			// MCJIT would keep this buffer until the engine dies (~1 MiB per inflated PPU module, 300+ MiB of heap for
			// a large game). RuntimeDyld copies every section into the memory manager while loading and keeps its own
			// copies of symbol names and relocation entries (finalizeObject() resolves them from those copies); after
			// that only JIT event listeners read the object bytes, and none is registered on PS4 (see the
			// constructor). So the buffer is freed as soon as the object is loaded: a link of hundreds of modules
			// holds one inflated object at a time.
			m_engine->addObjectFile(std::move(*object_file));
			m_ps4_object_bytes.emplace_back(std::move(cache));
			ps4_release_object_bytes(m_ps4_object_bytes);
		}
		else
#endif
		{
			m_engine->addObjectFile(llvm::object::OwningBinary<llvm::object::ObjectFile>(std::move(*object_file), std::move(cache)));
		}

		jit_log.trace("ObjectCache: Successfully added %s", path);
		return true;
	}
	else
	{
		jit_log.error("ObjectCache: Adding failed: %s", path);
		return false;
	}
}

bool jit_compiler::check(const std::string& path)
{
	if (auto cache = ObjectCache::load(path))
	{
		if (auto object_file = llvm::object::ObjectFile::createObjectFile(*cache))
		{
			return true;
		}

		if (fs::remove_file(path))
		{
			jit_log.error("ObjectCache: Removed damaged file: %s", path);
		}
	}

	return false;
}

void jit_compiler::update_global_mapping(const std::string& name, u64 addr)
{
	m_engine->updateGlobalMapping(name, addr);
}

#ifdef RPCS3_PS4
u64 jit_compiler::ps4_released_object_bytes() noexcept
{
	return g_ps4_released_object_bytes.load();
}

u64 jit_compiler::ps4_released_objects() noexcept
{
	return g_ps4_released_objects.load();
}

bool jit_compiler::ps4_keep_object_bytes() noexcept
{
	static const bool s_keep = fs::is_file("/data/rpcs3/ppu-keep-objects.flag");
	return s_keep;
}

// Relies on the same LLVM facts as the object byte release above: RuntimeDyld copies the sections while loading and
// keeps its own copies of symbol names and relocations; MCJIT::finalizeObject() only compiles modules it owns (none
// from this path) and resolves the recorded relocations.
void jit_compiler::ps4_add_and_free(std::unique_ptr<llvm::Module> _module, const std::function<void(const void*, usz)>& on_emitted)
{
	// MCJIT::emitObject: legacy pass manager on the engine's TargetMachine, verifier as configured for the engine
	llvm::SmallVector<char, 0> object;
	{
		llvm::raw_svector_ostream stream(object);
		llvm::legacy::PassManager pm;
		llvm::MCContext* mc = nullptr;

		if (m_engine->getTargetMachine()->addPassesToEmitMC(pm, mc, stream, !m_engine->getVerifyModules()))
		{
			fmt::throw_exception("LLVM: the target cannot emit machine code");
		}

		pm.run(*_module);
	}

	// The IR is not read again: the code is looked up by symbol name
	_module.reset();
	g_ps4_freed_modules++;

	auto bytes = std::make_unique<llvm::SmallVectorMemoryBuffer>(std::move(object), false);
	auto object_file = llvm::object::ObjectFile::createObjectFile(bytes->getMemBufferRef());

	if (!object_file)
	{
		fmt::throw_exception("LLVM: emitted object is unreadable: %s", llvm::toString(object_file.takeError()));
	}

	if (on_emitted)
	{
		on_emitted(bytes->getBufferStart(), bytes->getBufferSize());
	}

	m_ps4_emitted_bytes += bytes->getBufferSize();
	m_ps4_emitted_modules++;
	m_ps4_emitted_objects.emplace_back(std::move(bytes));
	m_engine->addObjectFile(std::move(*object_file));
}

bool jit_compiler::ps4_add_object(const void* data, usz size)
{
	std::unique_ptr<llvm::WritableMemoryBuffer> bytes = llvm::WritableMemoryBuffer::getNewUninitMemBuffer(size, "ps4-cached-object");

	if (!bytes)
	{
		return false;
	}

	std::memcpy(bytes->getBufferStart(), data, size);
	auto object_file = llvm::object::ObjectFile::createObjectFile(bytes->getMemBufferRef());

	if (!object_file)
	{
		llvm::consumeError(object_file.takeError());
		return false;
	}

	m_ps4_emitted_bytes += size;
	m_ps4_emitted_modules++;
	m_ps4_emitted_objects.emplace_back(std::move(bytes));
	m_engine->addObjectFile(std::move(*object_file));
	return true;
}

bool jit_compiler::ps4_object_symbols(const void* data, usz size, std::vector<std::string>& defined, std::vector<std::string>& undefined)
{
	const llvm::MemoryBufferRef ref(llvm::StringRef(static_cast<const char*>(data), size), "ps4-object");
	auto object_file = llvm::object::ObjectFile::createObjectFile(ref);

	if (!object_file)
	{
		llvm::consumeError(object_file.takeError());
		return false;
	}

	for (const llvm::object::SymbolRef& symbol : (*object_file)->symbols())
	{
		auto flags = symbol.getFlags();
		auto name = symbol.getName();
		auto type = symbol.getType();

		if (!flags || !name || !type)
		{
			if (!flags) llvm::consumeError(flags.takeError());
			if (!name) llvm::consumeError(name.takeError());
			if (!type) llvm::consumeError(type.takeError());
			return false;
		}

		// The null symbol, section and file symbols
		if (name->empty() || *type == llvm::object::SymbolRef::ST_File || *type == llvm::object::SymbolRef::ST_Debug)
		{
			continue;
		}

		if (*flags & llvm::object::SymbolRef::SF_Undefined)
		{
			undefined.emplace_back(name->str());
		}
		else if (*flags & llvm::object::SymbolRef::SF_Global)
		{
			defined.emplace_back(name->str());
		}
	}

	return true;
}

const std::string& jit_compiler::ps4_target_signature()
{
	static const std::string s_signature = []()
	{
		std::string features;
#ifdef ARCH_X64
		for (const std::string& attr : ps4_feature_switches())
		{
			if (!features.empty())
			{
				features += ',';
			}

			features += attr;
		}
#endif
		// Engine options of the constructor (flags 0: small code model)
		return fmt::format("llvm %s; triple %s; cpu %s; features %s; opt aggressive; code model small; reloc pic", LLVM_VERSION_STRING, triple2(), cpu(""), features);
	}();

	return s_signature;
}

jit_compiler::ps4_memory_refusal jit_compiler::ps4_take_memory_refusal() noexcept
{
	const ps4_memory_refusal result = m_ps4_refusal;
	m_ps4_refusal = {};
	g_ps4_refused_sections += result.sections;
	return result;
}

u32 jit_compiler::ps4_live_compilers() noexcept
{
	return g_ps4_live_compilers.load();
}

u64 jit_compiler::ps4_freed_modules() noexcept
{
	return g_ps4_freed_modules.load();
}

u64 jit_compiler::ps4_freed_emitted_bytes() noexcept
{
	return g_ps4_freed_emitted_bytes.load();
}

u64 jit_compiler::ps4_refused_sections() noexcept
{
	return g_ps4_refused_sections.load();
}

namespace
{
	// Emitted objects of ps4_add_and_free() after finalizeObject(), counted apart from the PPU link figures
	void ps4_release_emitted(std::vector<std::unique_ptr<llvm::MemoryBuffer>>& bytes) noexcept
	{
		if (bytes.empty())
		{
			return;
		}

		u64 total = 0;

		for (const auto& buffer : bytes)
		{
			total += buffer ? buffer->getBufferSize() : 0;
		}

		bytes.clear();
		g_ps4_freed_emitted_bytes += total;
	}
}
#endif

void jit_compiler::fin()
{
	m_engine->finalizeObject();

#ifdef RPCS3_PS4
	ps4_release_object_bytes(m_ps4_object_bytes);
	ps4_release_emitted(m_ps4_emitted_objects);
#endif
}

bool jit_compiler::try_fin(std::string& error)
{
#ifdef RPCS3_PS4
	if (!run_recoverable_llvm([&]()
	{
		m_engine->finalizeObject();
	}, error))
	{
		return false;
	}

	ps4_release_object_bytes(m_ps4_object_bytes);
	ps4_release_emitted(m_ps4_emitted_objects);
	return true;
#else
	return run_recoverable_llvm([&]()
	{
		m_engine->finalizeObject();
	}, error);
#endif
}

u64 jit_compiler::get(const std::string& name)
{
	return m_engine->getGlobalValueAddress(name);
}

const char * fallback_cpu_detection()
{
#if defined(ARCH_X64)
	// If we got here we either have a very old and outdated CPU or a new CPU that has not been seen by LLVM yet.
	const std::string brand = utils::get_cpu_brand();
	const auto family = utils::get_cpu_family();
	const auto model = utils::get_cpu_model();

	jit_log.error("CPU wasn't identified by LLVM, brand = %s, family = 0x%x, model = 0x%x", brand, family, model);

#ifdef RPCS3_PS4
	// 0.33: every PS4/PS4 Pro CPU is AMD family 16h (Jaguar). Checked before the brand test, which the console's
	// CPUID brand string is not guaranteed to pass; the AMD default below would pick bdver1 (XOP/FMA4/LWP).
	if (family == 0x16)
	{
		return utils::has_avx() ? "btver2" : "btver1";
	}
#endif

	if (brand.starts_with("AMD"))
	{
		switch (family)
		{
		case 0x10:
		case 0x12: // Unimplemented in LLVM
			return "amdfam10";
		case 0x15:
			// Bulldozer class, includes piledriver, excavator, steamroller, etc
			return utils::has_avx2() ? "bdver4" : "bdver1";
		case 0x17:
		case 0x18:
			// No major differences between znver1 and znver2, return the lesser
			return "znver1";
		case 0x19:
			// Models 0-Fh are zen3 as are 20h-60h. The rest we can assume are zen4
			return ((model >= 0x20 && model <= 0x60) || model < 0x10) ? "znver3" : "znver4";
		case 0x1a:
			// Only one generation in family 1a so far, zen5, which we do not support yet.
			// Return zen4 as a workaround until the next LLVM upgrade.
			return "znver4";
		default:
			// Safest guesses
			return utils::has_avx512() ? "znver4" :
			       utils::has_avx2()   ? "znver1" :
			       utils::has_avx()    ? "bdver1" :
			                             "nehalem";
		}
	}
	else if (brand.find("Intel") != std::string::npos)
	{
		if (!utils::has_avx())
		{
			return "nehalem";
		}
		if (!utils::has_avx2())
		{
			return "ivybridge";
		}
		if (!utils::has_avx512())
		{
			return "skylake";
		}
		if (utils::has_avx512_icl())
		{
			return "cannonlake";
		}
		return "icelake-client";
	}
	else if (brand.starts_with("VirtualApple"))
	{
		// No AVX. This will change in MacOS 15+, at which point we may revise this.
		return utils::has_avx() ? "haswell" : "nehalem";
	}

#elif defined(ARCH_ARM64)
#ifdef ANDROID
	static std::string s_result = []() -> std::string
	{
		std::string result = aarch64::get_cpu_name();
		if (result.empty())
		{
			return "cortex-a78";
		}

		std::transform(result.begin(), result.end(), result.begin(), utils::tolower<char>);
		return result;
	}();

	return s_result.c_str();
#else
	// TODO: Read the data from /proc/cpuinfo. ARM CPU registers are not accessible from usermode.
	// This will be a pain when supporting snapdragon on windows but we'll cross that bridge when we get there.
	// Require at least armv8-2a. Older chips are going to be useless anyway.
	return "cortex-a78";
#endif
#endif

	// Failed to guess, use generic fallback
	return "generic";
}

#endif // LLVM_AVAILABLE
