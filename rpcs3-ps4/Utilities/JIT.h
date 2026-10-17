#pragma once

#include "util/types.hpp"
#include "util/atomic.hpp"

// Include asmjit with warnings ignored
#define ASMJIT_EMBED
#define ASMJIT_STATIC
#define ASMJIT_BUILD_DEBUG
#undef Bool

#ifdef _MSC_VER
#pragma warning(push, 0)
#include <asmjit/asmjit.h>
#pragma warning(pop)
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wstrict-aliasing"
#pragma GCC diagnostic ignored "-Wredundant-decls"
#pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
#pragma GCC diagnostic ignored "-Weffc++"
#ifdef __clang__
#pragma GCC diagnostic ignored "-Wdeprecated-anon-enum-enum-conversion"
#pragma GCC diagnostic ignored "-Wcast-qual"
#else
#pragma GCC diagnostic ignored "-Wduplicated-branches"
#pragma GCC diagnostic ignored "-Wdeprecated-enum-enum-conversion"
#endif
#include <asmjit/asmjit.h>
#if defined(ARCH_ARM64)
#include <asmjit/a64.h>
#endif
#pragma GCC diagnostic pop
#endif

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <util/v128.hpp>

#if defined(ARCH_X64)
using native_asm = asmjit::x86::Assembler;
using native_args = std::array<asmjit::x86::Gp, 4>;
#elif defined(ARCH_ARM64)
using native_asm = asmjit::a64::Assembler;
using native_args = std::array<asmjit::a64::Gp, 4>;
#endif

void jit_announce(uptr func, usz size, std::string_view name);

void jit_announce(auto* func, usz size, std::string_view name)
{
	jit_announce(uptr(func), size, name);
}

enum class jit_class
{
	ppu_code,
	ppu_data,
	spu_code,
	spu_data,
};

struct jit_runtime_base
{
	jit_runtime_base() noexcept = default;
	virtual ~jit_runtime_base() = default;

	jit_runtime_base(const jit_runtime_base&) = delete;
	jit_runtime_base& operator=(const jit_runtime_base&) = delete;

	const asmjit::Environment& environment() const noexcept;
	void* _add(asmjit::CodeHolder* code, usz align = 64) noexcept;
	virtual uchar* _alloc(usz size, usz align) noexcept = 0;
};

// ASMJIT runtime for emitting code in a single 2G region
struct jit_runtime final : jit_runtime_base
{
	jit_runtime();
	~jit_runtime() override;

	// Allocate executable memory
	uchar* _alloc(usz size, usz align) noexcept override;

	// Allocate memory
	static u8* alloc(usz size, usz align, bool exec = true) noexcept;

#ifdef RPCS3_PS4
	// As alloc(), but null when the console refuses to commit the pages (direct memory exhausted) instead of ending the
	// thread: for callers with a fallback (SPU LLVM sections, ubertrampolines, spu_fast code, patchpoints)
	static u8* ps4_try_alloc(usz size, usz align, bool exec = true) noexcept;
#endif

	// Allocate 0 bytes, observe memory location
	// Same as alloc(0, 1, exec)
	static u8* peek(bool exec = true) noexcept;

	// Should be called at least once after global initialization
	static void initialize();

	// Deallocate all memory
	static void finalize() noexcept;
};

#ifdef RPCS3_PS4
// JIT ledger: direct memory committed for jit_runtime code and data, the PPU link arenas and the PPU executable and
// segment tables. Lock-free counters; any thread.
namespace utils::ps4::jit
{
	enum class owner : u32
	{
		runtime, // jit_runtime code and data (committed marks of its two ranges)
		link,    // PPU link and compile arenas (LLVM MemoryManager1)
		exec,    // PPU executable and segment tables
	};

	// Total, the figure the memory budget counts as "jit" (governor::set_jit_source)
	u64 committed_total() noexcept;
	u64 committed(owner o) noexcept;

	// Bytes added to (or, negative, removed from) an owner counted by its caller (link, exec)
	void account(owner o, s64 delta) noexcept;

	// Exec tables are decommitted as a whole at the stop
	void reset(owner o) noexcept;

	// Commits [ptr, ptr + size) with 'prot' (PROT_* bits). On a refusal: empty heap carve-outs go back to the pool and
	// the commit is tried again; with 'wait_ms' > 0 it keeps trying for up to that long (the budget and its shedders
	// run meanwhile). False when still refused (logged; errno kept). Pages of a refused commit may stay backed; the
	// caller tries the same range again later. Counts nothing: the caller accounts what it owns.
	bool commit(void* ptr, usz size, int prot, u32 wait_ms, const char* what) noexcept;

	// Commits refused for good since the process started
	u32 refusals() noexcept;

	// Test drills only (/data/rpcs3/test-jit-cap-mib.txt): true when 'size' more bytes would pass the test cap
	bool test_cap_refuses(usz size) noexcept;
}
#endif

namespace asmjit
{
	// Should only be used to build global functions
	jit_runtime_base& get_global_runtime();

	// Don't use directly
	class inline_runtime : public jit_runtime_base
	{
		uchar* m_data;
		usz m_size;

	public:
		inline_runtime(uchar* data, usz size);

		~inline_runtime();

		uchar* _alloc(usz size, usz align) noexcept override;
	};

	// Emit xbegin and adjacent loop, return label at xbegin (don't use xabort please)
	template <typename F>
	[[nodiscard]] inline asmjit::Label build_transaction_enter(asmjit::x86::Assembler& c, asmjit::Label fallback, F func)
	{
		Label fall = c.newLabel();
		Label begin = c.newLabel();
		c.jmp(begin);
		c.bind(fall);

		// Don't repeat on zero status (may indicate syscall or interrupt)
		c.test(x86::eax, x86::eax);
		c.jz(fallback);

		// First invoked after failure (can fallback to proceed, or jump anywhere else)
		func();

		// Other bad statuses are ignored regardless of repeat flag (TODO)
		c.align(AlignMode::kCode, 16);
		c.bind(begin);
		return fall;

		// xbegin should be issued manually, allows to add more check before entering transaction
	}

	// Helper to spill RDX (EDX) register for RDTSC
	inline void build_swap_rdx_with(asmjit::x86::Assembler& c, std::array<x86::Gp, 4>& args, const asmjit::x86::Gp& with)
	{
#ifdef _WIN32
		c.xchg(args[1], with);
		args[1] = with;
#else
		c.xchg(args[2], with);
		args[2] = with;
#endif
	}

	// Get full RDTSC value into chosen register (clobbers rax/rdx or saves only rax with other target)
	inline void build_get_tsc(asmjit::x86::Assembler& c, const asmjit::x86::Gp& to = asmjit::x86::rax)
	{
		if (&to != &x86::rax && &to != &x86::rdx)
		{
			// Swap to save its contents
			c.xchg(x86::rax, to);
		}

		c.rdtsc();
		c.shl(x86::rdx, 32);

		if (&to == &x86::rax)
		{
			c.or_(x86::rax, x86::rdx);
		}
		else if (&to == &x86::rdx)
		{
			c.or_(x86::rdx, x86::rax);
		}
		else
		{
			// Swap back, maybe there is more effective way to do it
			c.xchg(x86::rax, to);
			c.mov(to.r32(), to.r32());
			c.or_(to.r64(), x86::rdx);
		}
	}

	inline void build_init_args_from_ghc(native_asm& c, native_args& args)
	{
#if defined(ARCH_X64)
		// TODO: handle case when args don't overlap with r13/rbp/r12/rbx
		c.mov(args[0], x86::r13);
		c.mov(args[1], x86::rbp);
		c.mov(args[2], x86::r12);
		c.mov(args[3], x86::rbx);
#else
		static_cast<void>(c);
		static_cast<void>(args);
#endif
	}

	inline void build_init_ghc_args(native_asm& c, native_args& args)
	{
#if defined(ARCH_X64)
		// TODO: handle case when args don't overlap with r13/rbp/r12/rbx
		c.mov(x86::r13, args[0]);
		c.mov(x86::rbp, args[1]);
		c.mov(x86::r12, args[2]);
		c.mov(x86::rbx, args[3]);
#else
		static_cast<void>(c);
		static_cast<void>(args);
#endif
	}

#if defined(ARCH_X64)
	struct simd_builder : native_asm
	{
		std::unordered_map<v128, Label> consts;

		Operand v0, v1, v2, v3, v4, v5;

		uint vsize = 16;
		uint vmask = 0;

		simd_builder(CodeHolder* ch) noexcept;
		~simd_builder();

		void operator()() noexcept;

		void _init(uint new_vsize = 0);
		void vec_cleanup_ret();
		void vec_set_all_zeros(const Operand& v);
		void vec_set_all_ones(const Operand& v);
		void vec_set_const(const Operand& v, const v128& value);
		void vec_clobbering_test(u32 esize, const Operand& v, const Operand& rhs);
		void vec_broadcast_gpr(u32 esize, const Operand& v, const x86::Gp& r);

		// return x86::ptr(base, ctr, X, 0) where X is set for esize accordingly
		x86::Mem ptr_scale_for_vec(u32 esize, const x86::Gp& base, const x86::Gp& index);

		void vec_load_unaligned(u32 esize, const Operand& v, const x86::Mem& src);
		void vec_store_unaligned(u32 esize, const Operand& v, const x86::Mem& dst);

		void _vec_binary_op(x86::Inst::Id sse_op, x86::Inst::Id vex_op, x86::Inst::Id evex_op, const Operand& dst, const Operand& lhs, const Operand& rhs);

		void vec_shuffle_xi8(const Operand& dst, const Operand& lhs, const Operand& rhs)
		{
			using enum x86::Inst::Id;
			_vec_binary_op(kIdPshufb, kIdVpshufb, kIdVpshufb, dst, lhs, rhs);
		}

		void vec_xor(u32, const Operand& dst, const Operand& lhs, const Operand& rhs)
		{
			using enum x86::Inst::Id;
			_vec_binary_op(kIdPxor, kIdVpxor, kIdVpxord, dst, lhs, rhs);
		}

		void vec_or(u32, const Operand& dst, const Operand& lhs, const Operand& rhs)
		{
			using enum x86::Inst::Id;
			_vec_binary_op(kIdPor, kIdVpor, kIdVpord, dst, lhs, rhs);
		}

		void vec_andn(u32, const Operand& dst, const Operand& lhs, const Operand& rhs)
		{
			using enum x86::Inst::Id;
			_vec_binary_op(kIdPandn, kIdVpandn, kIdVpandnd, dst, lhs, rhs);
		}

		void vec_umin(u32 esize, const Operand& dst, const Operand& lhs, const Operand& rhs);
		void vec_umax(u32 esize, const Operand& dst, const Operand& lhs, const Operand& rhs);
		void vec_cmp_eq(u32 esize, const Operand& dst, const Operand& lhs, const Operand& rhs);

		void vec_extract_high(u32 esize, const Operand& dst, const Operand& src);
		void vec_extract_gpr(u32 esize, const x86::Gp& dst, const Operand& src);

		simd_builder& keep_if_not_masked()
		{
			if (vmask && vmask < 8)
			{
				this->k(x86::KReg(vmask));
			}

			return *this;
		}

		simd_builder& zero_if_not_masked()
		{
			if (vmask && vmask < 8)
			{
				this->k(x86::KReg(vmask));
				this->z();
			}

			return *this;
		}

		void build_loop(u32 esize, const x86::Gp& reg_ctr, const x86::Gp& reg_cnt, auto&& build, auto&& reduce)
		{
			ensure((esize & (esize - 1)) == 0);
			ensure(esize <= vsize);

			Label body = this->newLabel();
			Label next = this->newLabel();
			Label exit = this->newLabel();

			const u32 step = vsize / esize;

			this->xor_(reg_ctr.r32(), reg_ctr.r32()); // Reset counter reg
			this->cmp(reg_cnt, step);
			this->jb(next); // If count < step, skip main loop body
			this->align(AlignMode::kCode, 16);
			this->bind(body);
			this->sub(reg_cnt, step);
			build();
			this->add(reg_ctr, step);
			this->cmp(reg_cnt, step);
			this->jae(body);
			this->bind(next);

			if (vmask)
			{
				// Build single last iteration (masked)
				this->test(reg_cnt, reg_cnt);
				this->jz(exit);

				if (esize == 1 && vsize == 64)
				{
					this->bzhi(reg_cnt.r64(), x86::Mem(consts[~u128()], 0), reg_cnt.r64());
					this->kmovq(x86::k7, reg_cnt.r64());
				}
				else
				{
					this->bzhi(reg_cnt.r32(), x86::Mem(consts[~u128()], 0), reg_cnt.r32());
					this->kmovd(x86::k7, reg_cnt.r32());
				}

				vmask = 7;
				build();

				// Rollout reduction step
				this->bind(exit);
				while (true)
				{
					vsize /= 2;
					if (vsize < esize)
						break;
					this->_init(vsize);
					reduce();
				}
			}
			else
			{
				// Build unrolled loop tail (reduced vector width)
				while (true)
				{
					vsize /= 2;
					if (vsize < esize)
						break;

					// Shall not clobber flags
					this->_init(vsize);
					reduce();

					if (vsize == esize)
					{
						// Last "iteration"
						this->test(reg_cnt, reg_cnt);
						this->jz(exit);
						build();
					}
					else
					{
						const u32 step = vsize / esize;
						Label next = this->newLabel();
						this->cmp(reg_cnt, step);
						this->jb(next);
						build();
						this->add(reg_ctr, step);
						this->sub(reg_cnt, step);
						this->bind(next);
					}
				}

				this->bind(exit);
			}

			this->_init(0);
		}
	};

	// for (; count > 0; ctr++, count--)
	inline void build_loop(native_asm& c, auto ctr, auto count, auto&& build)
	{
		asmjit::Label body = c.newLabel();
		asmjit::Label exit = c.newLabel();

		c.test(count, count);
		c.jz(exit);
		c.align(asmjit::AlignMode::kCode, 16);
		c.bind(body);
		build();
		c.inc(ctr);
		c.sub(count, 1);
		c.ja(body);
		c.bind(exit);
	}

	inline void maybe_flush_lbr(native_asm& c, uint count = 2)
	{
		// Workaround for bad LBR callstacks which happen in some situations (mainly TSX) - execute additional RETs
		Label next = c.newLabel();
		c.lea(x86::rcx, x86::qword_ptr(next));

		for (u32 i = 0; i < count; i++)
		{
			c.push(x86::rcx);
			c.sub(x86::rcx, 16);
		}

		for (u32 i = 0; i < count; i++)
		{
			c.ret();
			c.align(asmjit::AlignMode::kCode, 16);
		}

		c.bind(next);
	}
#endif
}

#ifdef __APPLE__
struct jit_write_guard
{
	jit_write_guard() noexcept
	{
		pthread_jit_write_protect_np(false);

		// Ensure stores are not reordered by the compiler
		atomic_fence_acq_rel();
	}

	~jit_write_guard() noexcept
	{
		// Ensure stores are not reordered by the compiler
		atomic_fence_seq_cst();

		pthread_jit_write_protect_np(true);
	}
};
#else
#define jit_write_guard [[maybe_unused]] int
#endif

// Build runtime function with asmjit::X86Assembler
template <typename FT, typename Asm = native_asm, typename F>
inline FT build_function_asm(std::string_view name, F&& builder, ::jit_runtime* custom_runtime = nullptr, bool reduced_size = false)
{
	jit_write_guard jit_guard;

	using namespace asmjit;

	auto& rt = custom_runtime ? *custom_runtime : get_global_runtime();

	CodeHolder code;
	code.init(rt.environment());

#if defined(ARCH_X64)
	native_args args;
#ifdef _WIN32
	args[0] = x86::rcx;
	args[1] = x86::rdx;
	args[2] = x86::r8;
	args[3] = x86::r9;
#else
	args[0] = x86::rdi;
	args[1] = x86::rsi;
	args[2] = x86::rdx;
	args[3] = x86::rcx;
#endif
#elif defined(ARCH_ARM64)
	native_args args;
	args[0] = a64::x0;
	args[1] = a64::x1;
	args[2] = a64::x2;
	args[3] = a64::x3;
#endif

	Asm compiler(&code);
	compiler.addEncodingOptions(EncodingOptions::kOptimizedAlign);
	compiler.addEncodingOptions(EncodingOptions::kOptimizeForSize);
	if constexpr (std::is_invocable_r_v<bool, F, Asm&, native_args&>)
	{
		if (!builder(compiler, args))
			return nullptr;
	}
	else
	{
		builder(compiler, args);
	}

	if constexpr (std::is_invocable_r_v<void, Asm>)
	{
		// Finalization
		compiler();
	}

	const auto result = rt._add(&code, reduced_size ? 16 : 64);
#ifdef RPCS3_PS4
	// Null when the runtime has no memory left (callers keep their fallback)
	if (result)
#endif
	jit_announce(result, code.codeSize(), name);
	return reinterpret_cast<FT>(uptr(result));
}

#if defined(__INTELLISENSE__) && !defined(LLVM_AVAILABLE)
#define LLVM_AVAILABLE
#endif

#ifdef LLVM_AVAILABLE

namespace llvm
{
	class LLVMContext;
	class ExecutionEngine;
	class Module;
	class StringRef;
#ifdef RPCS3_PS4
	class MemoryBuffer;
#endif
}

enum class thread_state : u32;

// Temporary compiler interface
class jit_compiler final
{
#ifdef RPCS3_PS4
public:
	// Sections the memory manager could not place in JIT memory (jit_runtime for MemoryManager2, the PPU link arena
	// for MemoryManager1): they went to scratch heap blocks instead of ending the thread in RuntimeDyld, so that code
	// must never run.
	struct ps4_memory_refusal
	{
		u64 bytes = 0;
		u32 sections = 0;
	};

private:
#endif
	// Local LLVM context
	std::unique_ptr<llvm::LLVMContext> m_context{};

#ifdef RPCS3_PS4
	// Bytes of the object file being loaded by add(path), freed as soon as it is loaded.
	// Declared before m_engine so they outlive it.
	std::vector<std::unique_ptr<llvm::MemoryBuffer>> m_ps4_object_bytes{};

	// Objects emitted by ps4_add_and_free(), freed by fin()/try_fin() (separate from the PPU link counters)
	std::vector<std::unique_ptr<llvm::MemoryBuffer>> m_ps4_emitted_objects{};

	// Written by MemoryManager1/2 while an object loads; declared before m_engine so it outlives the manager
	ps4_memory_refusal m_ps4_refusal{};

	// Totals of ps4_add_and_free() for this compiler (recycle budget)
	u64 m_ps4_emitted_bytes = 0;
	u32 m_ps4_emitted_modules = 0;
#endif

	// Execution instance
	std::unique_ptr<llvm::ExecutionEngine> m_engine{};

	// Arch
	std::string m_cpu{};

	// Disk Space left
	atomic_t<usz> m_disk_space = umax;

public:
	jit_compiler(const std::unordered_map<std::string, u64>& _link, std::string_view _cpu, u32 flags = 0, std::function<u64(const std::string&)> symbols_cement = {});
	jit_compiler& operator=(thread_state) noexcept;
	~jit_compiler() noexcept;

	// Get LLVM context
	auto& get_context()
	{
		return *m_context;
	}

	auto& get_engine() const
	{
		return *m_engine;
	}

	// Add module (path to obj cache dir)
	void add(std::unique_ptr<llvm::Module> _module, const std::string& path);

	// Returns false after LLVM fatal recovery. The compiler must be discarded.
	bool try_add(std::unique_ptr<llvm::Module> _module, const std::string& path, std::string& error);

	// Add module (not cached)
	void add(std::unique_ptr<llvm::Module> _module);

	// Returns false after LLVM fatal recovery. The compiler must be discarded.
	bool try_add(std::unique_ptr<llvm::Module> _module, std::string& error);

	// Add object (path to obj file)
	bool add(const std::string& path);

	// Update global mapping for a single value
	void update_global_mapping(const std::string& name, u64 addr);

	// Check object file
	static bool check(const std::string& path);

	// Finalize
	void fin();

	// Returns false after LLVM fatal recovery. The compiler must be discarded.
	bool try_fin(std::string& error);

	// Get compiled function address
	u64 get(const std::string& name);

	// Get CPU info
	static std::string cpu(std::string_view _cpu);

	// Get system triple (PPU)
	static std::string triple1();

	// Get system triple (SPU)
	static std::string triple2();

	bool add_sub_disk_space(ssz space);

#ifdef RPCS3_PS4
	// Object file bytes released after finalization in this process (bytes, objects)
	static u64 ps4_released_object_bytes() noexcept;
	static u64 ps4_released_objects() noexcept;

	// True when /data/rpcs3/ppu-keep-objects.flag exists (read once per process): MCJIT keeps the bytes as upstream
	static bool ps4_keep_object_bytes() noexcept;

	// Emits the module's object on this thread (same passes as MCJIT::emitObject), frees the module at once and loads
	// the object; the object bytes are freed by fin()/try_fin(). MCJIT never owns the IR or the bytes, so a compiler
	// that builds many modules keeps only RuntimeDyld's symbol and section tables of each. The module's functions are
	// then found by symbol name with get(); no Function* of it may be used after the call. 'on_emitted', if set, sees the
	// object bytes before they are loaded.
	void ps4_add_and_free(std::unique_ptr<llvm::Module> _module, const std::function<void(const void*, usz)>& on_emitted = {});

	// Loads an object emitted earlier by this build for an identical module (persistent SPU object cache) the way
	// ps4_add_and_free() loads a fresh one: sections through the memory manager (jit_runtime; refusals recorded for
	// ps4_take_memory_refusal()), bytes kept until fin()/try_fin() and counted in ps4_emitted_bytes(). False if the
	// bytes are not an object file (nothing loaded).
	bool ps4_add_object(const void* data, usz size);

	// Names of the global defined and of the undefined symbols of an object file; false if it is not one
	static bool ps4_object_symbols(const void* data, usz size, std::vector<std::string>& defined, std::vector<std::string>& undefined);

	// Code generation target shared by every engine of this process: LLVM version, SPU triple, CPU, feature switches,
	// optimization level, code and relocation model
	static const std::string& ps4_target_signature();

	// Sections refused JIT memory since the last call (code loaded meanwhile must not run); clears the record
	ps4_memory_refusal ps4_take_memory_refusal() noexcept;

	// Object bytes and modules emitted by ps4_add_and_free() on this compiler
	u64 ps4_emitted_bytes() const noexcept
	{
		return m_ps4_emitted_bytes;
	}

	u32 ps4_emitted_modules() const noexcept
	{
		return m_ps4_emitted_modules;
	}

	// Process totals: compilers alive, modules freed by ps4_add_and_free(), their object bytes freed, refused sections
	static u32 ps4_live_compilers() noexcept;
	static u64 ps4_freed_modules() noexcept;
	static u64 ps4_freed_emitted_bytes() noexcept;
	static u64 ps4_refused_sections() noexcept;
#endif
};

const char *fallback_cpu_detection();

#endif // LLVM_AVAILABLE
