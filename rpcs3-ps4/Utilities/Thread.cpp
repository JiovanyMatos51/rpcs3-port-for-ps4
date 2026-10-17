#include "stdafx.h"
#include "Emu/System.h"
#include "Emu/Cell/SPUThread.h"
#include "Emu/Cell/PPUThread.h"
#include "Emu/Cell/lv2/sys_mmapper.h"
#include "Emu/Cell/lv2/sys_event.h"
#include "Emu/Cell/lv2/sys_process.h"
#include "Emu/RSX/RSXThread.h"
#include "Emu/system_config.h"
#include "Thread.h"
#include "Utilities/JIT.h"
#include <cfenv>

#ifdef ARCH_ARM64
#include "Emu/CPU/Backends/AArch64/AArch64Signal.h"
#endif

#ifdef __cpp_lib_stacktrace
#include "rpcs3_version.h"
#include <stacktrace>
#endif

#ifdef _WIN32
#include <Windows.h>
#include <Psapi.h>
#include <process.h>
#include <sysinfoapi.h>

#include "stack_trace.h"
#include "util/dyn_lib.hpp"

DYNAMIC_IMPORT_RENAME("Kernel32.dll", SetThreadDescriptionImport, "SetThreadDescription", HRESULT(HANDLE hThread, PCWSTR lpThreadDescription));

#else
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifdef __APPLE__
#define _XOPEN_SOURCE
#define __USE_GNU
#include <mach/thread_act.h>
#include <mach/thread_policy.h>
#endif
#if defined(__DragonFly__) || (defined(__FreeBSD__) && !defined(__ORBIS__)) || defined(__OpenBSD__)
#include <pthread_np.h>
#define cpu_set_t cpuset_t
#endif
#include <errno.h>
#include <signal.h>
#ifndef __OpenBSD__
#include <ucontext.h>
#ifdef __ORBIS__
#include "util/ps4_signal_context.hpp"
#include "util/ps4_virtual_memory.hpp"
#include "util/ps4_thread_priority.hpp"
#include <orbis/libkernel.h>
#include "ps4_launcher.h"
#endif
#endif
#include <pthread.h>
#include <sched.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <time.h>
#endif
#ifdef __linux__
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <unistd.h>
#endif

#if defined(__APPLE__) || defined(__DragonFly__) || (defined(__FreeBSD__) && !defined(__ORBIS__)) || defined(__NetBSD__) || defined(__OpenBSD__)
# include <sys/sysctl.h>
# include <unistd.h>
# if defined(__DragonFly__) || defined(__FreeBSD__)
#  include <sys/user.h>
# endif
# if defined(__OpenBSD__)
#  include <sys/param.h>
#  include <sys/proc.h>
# endif

# if defined(__NetBSD__)
#  undef KERN_PROC
#  define KERN_PROC KERN_PROC2
#  define kinfo_proc kinfo_proc2
# endif

# if defined(__APPLE__)
#  define KP_FLAGS kp_proc.p_flag
# elif defined(__DragonFly__)
#  define KP_FLAGS kp_flags
# elif defined(__FreeBSD__)
#  define KP_FLAGS ki_flag
# elif defined(__NetBSD__)
#  define KP_FLAGS p_flag
# elif defined(__OpenBSD__)
#  define KP_FLAGS p_psflags
#  define P_TRACED PS_TRACED
# endif
#endif

#include "util/vm.hpp"
#include "util/logs.hpp"
#include "util/asm.hpp"
#include "util/v128.hpp"
#include "util/simd.hpp"
#include "util/cctype.hpp"
#include "util/sysinfo.hpp"
#include "Emu/Memory/vm_locking.h"

LOG_CHANNEL(sig_log, "SIG");
LOG_CHANNEL(sys_log, "SYS");
LOG_CHANNEL(vm_log, "VM");

thread_local u64 g_tls_fault_all = 0;
thread_local u64 g_tls_fault_rsx = 0;
thread_local u64 g_tls_fault_spu = 0;
thread_local u64 g_tls_wait_time = 0;
thread_local u64 g_tls_wait_fail = 0;
thread_local u64 g_tls_access_violation_recovered = umax;
extern thread_local std::string(*g_tls_log_prefix)();

namespace stx
{
	atomic_t<u32> g_launch_retainer{0};
}

// Report error and call std::abort(), defined in main.cpp
[[noreturn]] void report_fatal_error(std::string_view text, bool is_html = false, bool include_help_text = true);

enum cpu_threads_emulation_info_dump_t : u32 {};

std::string dump_useful_thread_info()
{
	std::string result;

	if (auto cpu = get_current_cpu_thread())
	{
		fmt::append(result, "%s", cpu_threads_emulation_info_dump_t{cpu->id});
	}

	return result;
}

#ifndef _WIN32
bool IsDebuggerPresent()
{
#if defined(__APPLE__) || defined(__DragonFly__) || (defined(__FreeBSD__) && !defined(__ORBIS__)) || defined(__NetBSD__) || defined(__OpenBSD__)
	int mib[] = {
		CTL_KERN,
		KERN_PROC,
		KERN_PROC_PID,
		getpid(),
# if defined(__NetBSD__) || defined(__OpenBSD__)
		sizeof(struct kinfo_proc),
		1,
# endif
	};
	u_int miblen = std::size(mib);
	struct kinfo_proc info;
	usz size = sizeof(info);

	if (sysctl(mib, miblen, &info, &size, NULL, 0))
	{
		return false;
	}

	return info.KP_FLAGS & P_TRACED;
#else
	char buf[4096];
	fs::file status_fd("/proc/self/status");
	if (!status_fd)
	{
		std::fprintf(stderr, "Failed to open /proc/self/status\n");
		return false;
	}

	const auto num_read = status_fd.read(buf, sizeof(buf) - 1);
	if (num_read == 0 || num_read == umax)
	{
		std::fprintf(stderr, "Failed to read /proc/self/status (%d)\n", errno);
		return false;
	}

	buf[num_read] = '\0';
	std::string_view status = buf;

	const auto found = status.find("TracerPid:");
	if (found == umax)
	{
		std::fprintf(stderr, "Failed to find 'TracerPid:' in /proc/self/status\n");
		return false;
	}

	for (const char* cp = status.data() + found + 10; cp <= status.data() + num_read; ++cp)
	{
		if (!utils::isspace(*cp))
		{
			return utils::isdigit(*cp) != 0 && *cp != '0';
		}
	}

	return false;
#endif
}
#endif

bool is_debugger_present()
{
	if (g_cfg.core.external_debugger)
		return true;
	return IsDebuggerPresent();
}

#if defined(ARCH_X64)
enum x64_reg_t : u32
{
	X64R_RAX = 0,
	X64R_RCX,
	X64R_RDX,
	X64R_RBX,
	X64R_RSP,
	X64R_RBP,
	X64R_RSI,
	X64R_RDI,
	X64R_R8,
	X64R_R9,
	X64R_R10,
	X64R_R11,
	X64R_R12,
	X64R_R13,
	X64R_R14,
	X64R_R15,

	X64R_XMM0 = 0,
	X64R_XMM1,
	X64R_XMM2,
	X64R_XMM3,
	X64R_XMM4,
	X64R_XMM5,
	X64R_XMM6,
	X64R_XMM7,
	X64R_XMM8,
	X64R_XMM9,
	X64R_XMM10,
	X64R_XMM11,
	X64R_XMM12,
	X64R_XMM13,
	X64R_XMM14,
	X64R_XMM15,

	X64R_AL,
	X64R_CL,
	X64R_DL,
	X64R_BL,
	X64R_AH,
	X64R_CH,
	X64R_DH,
	X64R_BH,

	X64_NOT_SET,
	X64_IMM8,
	X64_IMM16,
	X64_IMM32,

	X64_BIT_O = 0x90,
	X64_BIT_NO,
	X64_BIT_C,
	X64_BIT_NC,
	X64_BIT_Z,
	X64_BIT_NZ,
	X64_BIT_BE,
	X64_BIT_NBE,
	X64_BIT_S,
	X64_BIT_NS,
	X64_BIT_P,
	X64_BIT_NP,
	X64_BIT_L,
	X64_BIT_NL,
	X64_BIT_LE,
	X64_BIT_NLE,

	X64R_ECX = X64R_CL,
};

enum x64_op_t : u32
{
	X64OP_NONE,
	X64OP_LOAD, // obtain and put the value into x64 register
	X64OP_LOAD_BE,
	X64OP_LOAD_CMP,
	X64OP_LOAD_TEST,
	X64OP_STORE, // take the value from x64 register or an immediate and use it
	X64OP_STORE_BE,
	X64OP_MOVS,
	X64OP_STOS,
	X64OP_XCHG,
	X64OP_CMPXCHG,
	X64OP_AND, // lock and [mem], ...
	X64OP_OR,  // lock or  [mem], ...
	X64OP_XOR, // lock xor [mem], ...
	X64OP_INC, // lock inc [mem]
	X64OP_DEC, // lock dec [mem]
	X64OP_ADD, // lock add [mem], ...
	X64OP_ADC, // lock adc [mem], ...
	X64OP_SUB, // lock sub [mem], ...
	X64OP_SBB, // lock sbb [mem], ...
	X64OP_BEXTR,
};

static thread_local x64_reg_t s_tls_reg3{};

void decode_x64_reg_op(const u8* code, x64_op_t& out_op, x64_reg_t& out_reg, usz& out_size, usz& out_length)
{
	// simple analysis of x64 code allows to reinterpret MOV or other instructions in any desired way
	out_length = 0;

	u8 rex = 0, pg2 = 0;

	bool oso = false, lock = false, repne = false, repe = false;

	enum : u8
	{
		LOCK  = 0xf0,
		REPNE = 0xf2,
		REPE  = 0xf3,
	};

	// check prefixes:
	for (;; code++, out_length++)
	{
		switch (const u8 prefix = *code)
		{
		case LOCK: // group 1
		{
			if (lock)
			{
				sig_log.error("decode_x64_reg_op(%016llxh): LOCK prefix found twice", code - out_length);
			}

			lock = true;
			continue;
		}
		case REPNE: // group 1
		{
			if (repne)
			{
				sig_log.error("decode_x64_reg_op(%016llxh): REPNE/REPNZ prefix found twice", code - out_length);
			}

			repne = true;
			continue;
		}
		case REPE: // group 1
		{
			if (repe)
			{
				sig_log.error("decode_x64_reg_op(%016llxh): REP/REPE/REPZ prefix found twice", code - out_length);
			}

			repe = true;
			continue;
		}

		case 0x2e: // group 2
		case 0x36:
		case 0x3e:
		case 0x26:
		case 0x64:
		case 0x65:
		{
			if (pg2)
			{
				sig_log.error("decode_x64_reg_op(%016llxh): 0x%02x (group 2 prefix) found after 0x%02x", code - out_length, prefix, pg2);
			}
			else
			{
				pg2 = prefix; // probably, segment register
			}
			continue;
		}

		case 0x66: // group 3
		{
			if (oso)
			{
				sig_log.error("decode_x64_reg_op(%016llxh): operand-size override prefix found twice", code - out_length);
			}

			oso = true;
			continue;
		}

		case 0x67: // group 4
		{
			sig_log.error("decode_x64_reg_op(%016llxh): address-size override prefix found", code - out_length, prefix);
			out_op = X64OP_NONE;
			out_reg = X64_NOT_SET;
			out_size = 0;
			out_length = 0;
			return;
		}

		default:
		{
			if ((prefix & 0xf0) == 0x40) // check REX prefix
			{
				if (rex)
				{
					sig_log.error("decode_x64_reg_op(%016llxh): 0x%02x (REX prefix) found after 0x%02x", code - out_length, prefix, rex);
				}
				else
				{
					rex = prefix;
				}
				continue;
			}
		}
		}

		break;
	}

	auto get_modRM_reg = [](const u8* code, const u8 rex) -> x64_reg_t
	{
		return x64_reg_t{((*code & 0x38) >> 3 | (/* check REX.R bit */ rex & 4 ? 8 : 0)) + X64R_RAX};
	};

	auto get_modRM_reg_xmm = [](const u8* code, const u8 rex) -> x64_reg_t
	{
		return x64_reg_t{((*code & 0x38) >> 3 | (/* check REX.R bit */ rex & 4 ? 8 : 0)) + X64R_XMM0};
	};

	auto get_modRM_reg_lh = [](const u8* code) -> x64_reg_t
	{
		return x64_reg_t{((*code & 0x38) >> 3) + X64R_AL};
	};

	auto get_op_size = [](const u8 rex, const bool oso) -> usz
	{
		return rex & 8 ? 8 : (oso ? 2 : 4);
	};

	auto get_modRM_size = [](const u8* code) -> usz
	{
		switch (*code >> 6) // check Mod
		{
		case 0: return (*code & 0x07) == 4 ? 2 : 1; // check SIB
		case 1: return (*code & 0x07) == 4 ? 3 : 2; // check SIB (disp8)
		case 2: return (*code & 0x07) == 4 ? 6 : 5; // check SIB (disp32)
		default: return 1;
		}
	};

	const u8 op1 = (out_length++, *code++), op2 = code[0], op3 = code[1];

	switch (op1)
	{
	case 0x0f:
	{
		out_length++, code++;

		switch (op2)
		{
		case 0x11:
		case 0x29:
		{
			if (!repe && !repne) // MOVUPS/MOVAPS/MOVUPD/MOVAPD xmm/m, xmm
			{
				out_op = X64OP_STORE;
				out_reg = get_modRM_reg_xmm(code, rex);
				out_size = 16;
				out_length += get_modRM_size(code);
				return;
			}
			break;
		}
		case 0x7f:
		{
			if (repe != oso) // MOVDQU/MOVDQA xmm/m, xmm
			{
				out_op = X64OP_STORE;
				out_reg = get_modRM_reg_xmm(code, rex);
				out_size = 16;
				out_length += get_modRM_size(code);
				return;
			}
			break;
		}
		case 0xb0:
		{
			if (!oso) // CMPXCHG r8/m8, r8
			{
				out_op = X64OP_CMPXCHG;
				out_reg = rex & 8 ? get_modRM_reg(code, rex) : get_modRM_reg_lh(code);
				out_size = 1;
				out_length += get_modRM_size(code);
				return;
			}
			break;
		}
		case 0xb1:
		{
			if (true) // CMPXCHG r/m, r (16, 32, 64)
			{
				out_op = X64OP_CMPXCHG;
				out_reg = get_modRM_reg(code, rex);
				out_size = get_op_size(rex, oso);
				out_length += get_modRM_size(code);
				return;
			}
			break;
		}
		case 0x90:
		case 0x91:
		case 0x92:
		case 0x93:
		case 0x94:
		case 0x95:
		case 0x96:
		case 0x97:
		case 0x98:
		case 0x9a:
		case 0x9b:
		case 0x9c:
		case 0x9d:
		case 0x9e:
		case 0x9f:
		{
			if (!lock) // SETcc
			{
				out_op = X64OP_STORE;
				out_reg = x64_reg_t(X64_BIT_O + op2 - 0x90); // 0x90 .. 0x9f
				out_size = 1;
				out_length += get_modRM_size(code);
				return;
			}
			break;
		}
		case 0x38:
		{
			out_length++, code++;

			switch (op3)
			{
			case 0xf0:
			case 0xf1:
			{
				if (!repne) // MOVBE
				{
					out_op = op3 == 0xf0 ? X64OP_LOAD_BE : X64OP_STORE_BE;
					out_reg = get_modRM_reg(code, rex);
					out_size = get_op_size(rex, oso);
					out_length += get_modRM_size(code);
					return;
				}

				break;
			}
			}

			break;
		}
		}

		break;
	}
	case 0x20:
	{
		if (!oso)
		{
			out_op = X64OP_AND;
			out_reg = rex & 8 ? get_modRM_reg(code, rex) : get_modRM_reg_lh(code);
			out_size = 1;
			out_length += get_modRM_size(code);
			return;
		}
		break;
	}
	case 0x21:
	{
		if (true)
		{
			out_op = X64OP_AND;
			out_reg = get_modRM_reg(code, rex);
			out_size = get_op_size(rex, oso);
			out_length += get_modRM_size(code);
			return;
		}
		break;
	}
	case 0x80:
	{
		switch (get_modRM_reg(code, 0))
		{
		//case 0: out_op = X64OP_ADD; break; // TODO: strange info in instruction manual
		case 1: out_op = X64OP_OR; break;
		case 2: out_op = X64OP_ADC; break;
		case 3: out_op = X64OP_SBB; break;
		case 4: out_op = X64OP_AND; break;
		case 5: out_op = X64OP_SUB; break;
		case 6: out_op = X64OP_XOR; break;
		default: out_op = X64OP_LOAD_CMP; break;
		}

		out_reg = X64_IMM8;
		out_size = 1;
		out_length += get_modRM_size(code) + 1;
		return;
	}
	case 0x81:
	{
		switch (get_modRM_reg(code, 0))
		{
		case 0: out_op = X64OP_ADD; break;
		case 1: out_op = X64OP_OR; break;
		case 2: out_op = X64OP_ADC; break;
		case 3: out_op = X64OP_SBB; break;
		case 4: out_op = X64OP_AND; break;
		case 5: out_op = X64OP_SUB; break;
		case 6: out_op = X64OP_XOR; break;
		default: out_op = X64OP_LOAD_CMP; break;
		}

		out_reg = oso ? X64_IMM16 : X64_IMM32;
		out_size = get_op_size(rex, oso);
		out_length += get_modRM_size(code) + (oso ? 2 : 4);
		return;
	}
	case 0x83:
	{
		switch (get_modRM_reg(code, 0))
		{
		case 0: out_op = X64OP_ADD; break;
		case 1: out_op = X64OP_OR; break;
		case 2: out_op = X64OP_ADC; break;
		case 3: out_op = X64OP_SBB; break;
		case 4: out_op = X64OP_AND; break;
		case 5: out_op = X64OP_SUB; break;
		case 6: out_op = X64OP_XOR; break;
		default: out_op = X64OP_LOAD_CMP; break;
		}

		out_reg = X64_IMM8;
		out_size = get_op_size(rex, oso);
		out_length += get_modRM_size(code) + 1;
		return;
	}
	case 0x86:
	{
		if (!oso) // XCHG r8/m8, r8
		{
			out_op = X64OP_XCHG;
			out_reg = rex & 8 ? get_modRM_reg(code, rex) : get_modRM_reg_lh(code);
			out_size = 1;
			out_length += get_modRM_size(code);
			return;
		}
		break;
	}
	case 0x87:
	{
		if (true) // XCHG r/m, r (16, 32, 64)
		{
			out_op = X64OP_XCHG;
			out_reg = get_modRM_reg(code, rex);
			out_size = get_op_size(rex, oso);
			out_length += get_modRM_size(code);
			return;
		}
		break;
	}
	case 0x88:
	{
		if (!lock && !oso) // MOV r8/m8, r8
		{
			out_op = X64OP_STORE;
			out_reg = rex & 8 ? get_modRM_reg(code, rex) : get_modRM_reg_lh(code);
			out_size = 1;
			out_length += get_modRM_size(code);
			return;
		}
		break;
	}
	case 0x89:
	{
		if (!lock) // MOV r/m, r (16, 32, 64)
		{
			out_op = X64OP_STORE;
			out_reg = get_modRM_reg(code, rex);
			out_size = get_op_size(rex, oso);
			out_length += get_modRM_size(code);
			return;
		}
		break;
	}
	case 0x8a:
	{
		if (!lock && !oso) // MOV r8, r8/m8
		{
			out_op = X64OP_LOAD;
			out_reg = rex & 8 ? get_modRM_reg(code, rex) : get_modRM_reg_lh(code);
			out_size = 1;
			out_length += get_modRM_size(code);
			return;
		}
		break;
	}
	case 0x8b:
	{
		if (!lock) // MOV r, r/m (16, 32, 64)
		{
			out_op = X64OP_LOAD;
			out_reg = get_modRM_reg(code, rex);
			out_size = get_op_size(rex, oso);
			out_length += get_modRM_size(code);
			return;
		}
		break;
	}
	case 0xa4:
	{
		if (!oso && !lock && !repe && !rex) // MOVS
		{
			out_op = X64OP_MOVS;
			out_reg = X64_NOT_SET;
			out_size = 1;
			return;
		}
		if (!oso && !lock && repe) // REP MOVS
		{
			out_op = X64OP_MOVS;
			out_reg = rex & 8 ? X64R_RCX : X64R_ECX;
			out_size = 1;
			return;
		}
		break;
	}
	case 0xaa:
	{
		if (!oso && !lock && !repe && !rex) // STOS
		{
			out_op = X64OP_STOS;
			out_reg = X64_NOT_SET;
			out_size = 1;
			return;
		}
		if (!oso && !lock && repe) // REP STOS
		{
			out_op = X64OP_STOS;
			out_reg = rex & 8 ? X64R_RCX : X64R_ECX;
			out_size = 1;
			return;
		}
		break;
	}
	case 0xc4: // 3-byte VEX prefix
	case 0xc5: // 2-byte VEX prefix
	{
		// Last prefix byte: op2 or op3
		const u8 opx = op1 == 0xc5 ? op2 : op3;

		// Implied prefixes
		rex |= op2 & 0x80 ? 0 : 0x4; // REX.R
		rex |= op1 == 0xc4 && op3 & 0x80 ? 0x8 : 0; // REX.W ???
		oso = (opx & 0x3) == 0x1;
		repe = (opx & 0x3) == 0x2;
		repne = (opx & 0x3) == 0x3;

		const u8 vopm = op1 == 0xc5 ? 1 : op2 & 0x1f;
		const u8 vop1 = op1 == 0xc5 ? op3 : code[2];
		const u8 vlen = (opx & 0x4) ? 32 : 16;
		const u8 vreg = (~opx >> 3) & 0xf;
		out_length += op1 == 0xc5 ? 2 : 3;
		code += op1 == 0xc5 ? 2 : 3;

		s_tls_reg3 = x64_reg_t{vreg};

		if (vopm == 0x1) switch (vop1) // Implied leading byte 0x0F
		{
		case 0x11:
		case 0x29:
		{
			if (!repe && !repne) // VMOVAPS/VMOVAPD/VMOVUPS/VMOVUPD mem,reg
			{
				out_op = X64OP_STORE;
				out_reg = get_modRM_reg_xmm(code, rex);
				out_size = vlen;
				out_length += get_modRM_size(code);
				return;
			}
			break;
		}
		case 0x7f:
		{
			if (repe || oso) // VMOVDQU/VMOVDQA mem,reg
			{
				out_op = X64OP_STORE;
				out_reg = get_modRM_reg_xmm(code, rex);
				out_size = vlen;
				out_length += get_modRM_size(code);
				return;
			}
			break;
		}
		}

		if (vopm == 0x2) switch (vop1) // Implied leading bytes 0x0F 0x38
		{
		case 0xf7:
		{
			if (!repe && !repne && vlen == 16) // BEXTR r32,mem,r32
			{
				out_op = X64OP_BEXTR;
				out_reg = get_modRM_reg_xmm(code, rex);
				out_size = opx & 0x80 ? 8 : 4;
				out_length += get_modRM_size(code);
				return;
			}
			break;
		}
		}

		break;
	}
	case 0xc6:
	{
		if (!lock && !oso && get_modRM_reg(code, 0) == 0) // MOV r8/m8, imm8
		{
			out_op = X64OP_STORE;
			out_reg = X64_IMM8;
			out_size = 1;
			out_length += get_modRM_size(code) + 1;
			return;
		}
		break;
	}
	case 0xc7:
	{
		if (!lock && get_modRM_reg(code, 0) == 0) // MOV r/m, imm16/imm32 (16, 32, 64)
		{
			out_op = X64OP_STORE;
			out_reg = oso ? X64_IMM16 : X64_IMM32;
			out_size = get_op_size(rex, oso);
			out_length += get_modRM_size(code) + (oso ? 2 : 4);
			return;
		}
		break;
	}
	case 0xf6:
	{
		switch (get_modRM_reg(code, 0))
		{
		case 0: out_op = X64OP_LOAD_TEST; break;
		default: out_op = X64OP_NONE; break; // TODO...
		}

		out_reg = X64_IMM8;
		out_size = 1;
		out_length += get_modRM_size(code) + 1;
		return;
	}
	case 0xf7:
	{
		switch (get_modRM_reg(code, 0))
		{
		case 0: out_op = X64OP_LOAD_TEST; break;
		default: out_op = X64OP_NONE; break; // TODO...
		}

		out_reg = oso ? X64_IMM16 : X64_IMM32;
		out_size = get_op_size(rex, oso);
		out_length += get_modRM_size(code) + (oso ? 2 : 4);
		return;
	}
	}

	out_op = X64OP_NONE;
	out_reg = X64_NOT_SET;
	out_size = 0;
	out_length = 0;
}

#ifdef _WIN32

typedef CONTEXT x64_context;
typedef CONTEXT ucontext_t;

#define X64REG(context, reg) (&(&(context)->Rax)[reg])
#define XMMREG(context, reg) (reinterpret_cast<v128*>(&(&(context)->Xmm0)[reg]))
#define EFLAGS(context) ((context)->EFlags)

#define ARG1(context) RCX(context)
#define ARG2(context) RDX(context)

#else

#ifdef __ORBIS__
typedef utils::ps4::signal_context x64_context;
#else
typedef ucontext_t x64_context;
#endif

#ifdef __ORBIS__

#define X64REG(context, reg) ((context)->register_pointer(reg))
// FXSAVE's XMM bank starts at byte 160 of the inline native FP state.
#define XMMREG(context, reg) (reinterpret_cast<v128*>(reinterpret_cast<u8*>((context)->machine.fpstate) + 160 + 16 * (reg)))
#define EFLAGS(context) ((context)->machine.rflags)

#elif defined(__APPLE__)

#define X64REG(context, reg) (darwin_x64reg(context, reg))
#define XMMREG(context, reg) (reinterpret_cast<v128*>(&(context)->uc_mcontext->__fs.__fpu_xmm0.__xmm_reg[reg]))
#define EFLAGS(context) ((context)->uc_mcontext->__ss.__rflags)

u64* darwin_x64reg(x64_context *context, int reg)
{
	auto *state = &context->uc_mcontext->__ss;
	switch(reg)
	{
	case 0: return &state->__rax;
	case 1: return &state->__rcx;
	case 2: return &state->__rdx;
	case 3: return &state->__rbx;
	case 4: return &state->__rsp;
	case 5: return &state->__rbp;
	case 6: return &state->__rsi;
	case 7: return &state->__rdi;
	case 8: return &state->__r8;
	case 9: return &state->__r9;
	case 10: return &state->__r10;
	case 11: return &state->__r11;
	case 12: return &state->__r12;
	case 13: return &state->__r13;
	case 14: return &state->__r14;
	case 15: return &state->__r15;
	case 16: return &state->__rip;
	default:
		sig_log.error("Invalid register index: %d", reg);
		return nullptr;
	}
}

#elif defined(__DragonFly__) || (defined(__FreeBSD__) && !defined(__ORBIS__))

#define X64REG(context, reg) (freebsd_x64reg(context, reg))
#ifdef __DragonFly__
#  define XMMREG(context, reg) (reinterpret_cast<v128*>((reinterpret_cast<union savefpu*>(context)->uc_mcontext.mc_fpregs)->sv_xmm.sv_xmm[reg]))
#else
#  define XMMREG(context, reg) (reinterpret_cast<v128*>((reinterpret_cast<struct savefpu*>(context)->uc_mcontext.mc_fpstate)->sv_xmm[reg]))
#endif
#define EFLAGS(context) ((context)->uc_mcontext.mc_rflags)

register_t* freebsd_x64reg(x64_context *context, int reg)
{
	auto *state = &context->uc_mcontext;
	switch(reg)
	{
	case 0: return &state->mc_rax;
	case 1: return &state->mc_rcx;
	case 2: return &state->mc_rdx;
	case 3: return &state->mc_rbx;
	case 4: return &state->mc_rsp;
	case 5: return &state->mc_rbp;
	case 6: return &state->mc_rsi;
	case 7: return &state->mc_rdi;
	case 8: return &state->mc_r8;
	case 9: return &state->mc_r9;
	case 10: return &state->mc_r10;
	case 11: return &state->mc_r11;
	case 12: return &state->mc_r12;
	case 13: return &state->mc_r13;
	case 14: return &state->mc_r14;
	case 15: return &state->mc_r15;
	case 16: return &state->mc_rip;
	default:
		sig_log.error("Invalid register index: %d", reg);
		return nullptr;
	}
}

#elif defined(__OpenBSD__)

#define X64REG(context, reg) (openbsd_x64reg(context, reg))
#define XMMREG(context, reg) (reinterpret_cast<v128*>((context)->sc_fpstate->fx_xmm[reg]))
#define EFLAGS(context) ((context)->sc_rflags)

long* openbsd_x64reg(x64_context *context, int reg)
{
	auto *state = &context;
	switch(reg)
	{
	case 0: return &state->sc_rax;
	case 1: return &state->sc_rcx;
	case 2: return &state->sc_rdx;
	case 3: return &state->sc_rbx;
	case 4: return &state->sc_rsp;
	case 5: return &state->sc_rbp;
	case 6: return &state->sc_rsi;
	case 7: return &state->sc_rdi;
	case 8: return &state->sc_r8;
	case 9: return &state->sc_r9;
	case 10: return &state->sc_r10;
	case 11: return &state->sc_r11;
	case 12: return &state->sc_r12;
	case 13: return &state->sc_r13;
	case 14: return &state->sc_r14;
	case 15: return &state->sc_r15;
	case 16: return &state->sc_rip;
	default:
		sig_log.error("Invalid register index: %d", reg);
		return nullptr;
	}
}

#elif defined(__NetBSD__)

static const decltype(_REG_RAX) reg_table[] =
{
	_REG_RAX, _REG_RCX, _REG_RDX, _REG_RBX, _REG_RSP, _REG_RBP, _REG_RSI, _REG_RDI,
	_REG_R8, _REG_R9, _REG_R10, _REG_R11, _REG_R12, _REG_R13, _REG_R14, _REG_R15, _REG_RIP
};

#define X64REG(context, reg) (&(context)->uc_mcontext.__gregs[reg_table[reg]])
#define XMM_sig(context, reg) (reinterpret_cast<v128*>(((struct fxsave64*)(context)->uc_mcontext.__fpregs)->fx_xmm[reg]))
#define EFLAGS(context) ((context)->uc_mcontext.__gregs[_REG_RFL])

#else

static const int reg_table[] =
{
	REG_RAX, REG_RCX, REG_RDX, REG_RBX, REG_RSP, REG_RBP, REG_RSI, REG_RDI,
	REG_R8, REG_R9, REG_R10, REG_R11, REG_R12, REG_R13, REG_R14, REG_R15, REG_RIP
};

#define X64REG(context, reg) (&(context)->uc_mcontext.gregs[reg_table[reg]])
#ifdef __sun
#define XMMREG(context, reg) (reinterpret_cast<v128*>(&(context)->uc_mcontext.fpregs.fp_reg_set.fpchip_state.xmm[reg_table[reg]]))
#else
#define XMMREG(context, reg) (reinterpret_cast<v128*>(&(context)->uc_mcontext.fpregs->_xmm[reg]))
#endif // __sun
#define EFLAGS(context) ((context)->uc_mcontext.gregs[REG_EFL])

#endif // __APPLE__

#define ARG1(context) RDI(context)
#define ARG2(context) RSI(context)

#endif

#define RAX(c) (*X64REG((c), 0))
#define RCX(c) (*X64REG((c), 1))
#define RDX(c) (*X64REG((c), 2))
#define RSP(c) (*X64REG((c), 4))
#define RSI(c) (*X64REG((c), 6))
#define RDI(c) (*X64REG((c), 7))
#define RIP(c) (*X64REG((c), 16))

bool get_x64_reg_value(x64_context* context, x64_reg_t reg, usz d_size, usz i_size, u64& out_value)
{
	// get x64 reg value (for store operations)
	if (reg - X64R_RAX < 16)
	{
		// load the value from x64 register
		const u64 reg_value = *X64REG(context, reg - X64R_RAX);

		switch (d_size)
		{
		case 1: out_value = static_cast<u8>(reg_value); return true;
		case 2: out_value = static_cast<u16>(reg_value); return true;
		case 4: out_value = static_cast<u32>(reg_value); return true;
		case 8: out_value = reg_value; return true;
		}
	}
	else if (reg - X64R_AL < 4 && d_size == 1)
	{
		out_value = static_cast<u8>(*X64REG(context, reg - X64R_AL));
		return true;
	}
	else if (reg - X64R_AH < 4 && d_size == 1)
	{
		out_value = static_cast<u8>(*X64REG(context, reg - X64R_AH) >> 8);
		return true;
	}
	else if (reg == X64_IMM8)
	{
		// load the immediate value (assuming it's at the end of the instruction)
		const s8 imm_value = *reinterpret_cast<s8*>(RIP(context) + i_size - 1);

		switch (d_size)
		{
		case 1: out_value = static_cast<u8>(imm_value); return true;
		case 2: out_value = static_cast<u16>(imm_value); return true; // sign-extended
		case 4: out_value = static_cast<u32>(imm_value); return true; // sign-extended
		case 8: out_value = static_cast<u64>(imm_value); return true; // sign-extended
		}
	}
	else if (reg == X64_IMM16)
	{
		const s16 imm_value = *reinterpret_cast<s16*>(RIP(context) + i_size - 2);

		switch (d_size)
		{
		case 2: out_value = static_cast<u16>(imm_value); return true;
		}
	}
	else if (reg == X64_IMM32)
	{
		const s32 imm_value = *reinterpret_cast<s32*>(RIP(context) + i_size - 4);

		switch (d_size)
		{
		case 4: out_value = static_cast<u32>(imm_value); return true;
		case 8: out_value = static_cast<u64>(imm_value); return true; // sign-extended
		}
	}
	else if (reg == X64R_ECX)
	{
		out_value = static_cast<u32>(RCX(context));
		return true;
	}
	else if (reg >= X64_BIT_O && reg <= X64_BIT_NLE)
	{
		const u32 _cf = EFLAGS(context) & 0x1;
		const u32 _zf = EFLAGS(context) & 0x40;
		const u32 _sf = EFLAGS(context) & 0x80;
		const u32 _of = EFLAGS(context) & 0x800;
		const u32 _pf = EFLAGS(context) & 0x4;
		const u32 _l = (_sf << 4) ^ _of; // SF != OF

		switch (reg & ~1)
		{
		case X64_BIT_O: out_value = !!_of ^ (reg & 1); break;
		case X64_BIT_C: out_value = !!_cf ^ (reg & 1); break;
		case X64_BIT_Z: out_value = !!_zf ^ (reg & 1); break;
		case X64_BIT_BE: out_value = !!(_cf | _zf) ^ (reg & 1); break;
		case X64_BIT_S: out_value = !!_sf ^ (reg & 1); break;
		case X64_BIT_P: out_value = !!_pf ^ (reg & 1); break;
		case X64_BIT_L: out_value = !!_l ^ (reg & 1); break;
		case X64_BIT_LE: out_value = !!(_l | _zf) ^ (reg & 1); break;
		}

		return true;
	}

	sig_log.error("get_x64_reg_value(): invalid arguments (reg=%d, d_size=%lld, i_size=%lld)", +reg, d_size, i_size);
	return false;
}

bool put_x64_reg_value(x64_context* context, x64_reg_t reg, usz d_size, u64 value)
{
	// save x64 reg value (for load operations)
	if (reg - X64R_RAX < 16)
	{
		// save the value into x64 register
		switch (d_size)
		{
		case 1: *X64REG(context, reg - X64R_RAX) = (value & 0xff) | (*X64REG(context, reg - X64R_RAX) & 0xffffff00); return true;
		case 2: *X64REG(context, reg - X64R_RAX) = (value & 0xffff) | (*X64REG(context, reg - X64R_RAX) & 0xffff0000); return true;
		case 4: *X64REG(context, reg - X64R_RAX) = value & 0xffffffff; return true;
		case 8: *X64REG(context, reg - X64R_RAX) = value; return true;
		}
	}

	sig_log.error("put_x64_reg_value(): invalid destination (reg=%d, d_size=%lld, value=0x%llx)", +reg, d_size, value);
	return false;
}

bool set_x64_cmp_flags(x64_context* context, usz d_size, u64 x, u64 y, bool carry = true)
{
	switch (d_size)
	{
	case 1: break;
	case 2: break;
	case 4: break;
	case 8: break;
	default: sig_log.error("set_x64_cmp_flags(): invalid d_size (%lld)", d_size); return false;
	}

	const u64 sign = 1ull << (d_size * 8 - 1); // sign mask
	const u64 diff = x - y;
	const u64 summ = x + y;

	if (carry && ((x & y) | ((x ^ y) & ~summ)) & sign)
	{
		EFLAGS(context) |= 0x1; // set CF
	}
	else if (carry)
	{
		EFLAGS(context) &= ~0x1; // clear CF
	}

	if (x == y)
	{
		EFLAGS(context) |= 0x40; // set ZF
	}
	else
	{
		EFLAGS(context) &= ~0x40; // clear ZF
	}

	if (diff & sign)
	{
		EFLAGS(context) |= 0x80; // set SF
	}
	else
	{
		EFLAGS(context) &= ~0x80; // clear SF
	}

	if ((x ^ summ) & (y ^ summ) & sign)
	{
		EFLAGS(context) |= 0x800; // set OF
	}
	else
	{
		EFLAGS(context) &= ~0x800; // clear OF
	}

	const u8 p1 = static_cast<u8>(diff) ^ (static_cast<u8>(diff) >> 4);
	const u8 p2 = p1 ^ (p1 >> 2);
	const u8 p3 = p2 ^ (p2 >> 1);

	if ((p3 & 1) == 0)
	{
		EFLAGS(context) |= 0x4; // set PF
	}
	else
	{
		EFLAGS(context) &= ~0x4; // clear PF
	}

	if (((x & y) | ((x ^ y) & ~summ)) & 0x8)
	{
		EFLAGS(context) |= 0x10; // set AF
	}
	else
	{
		EFLAGS(context) &= ~0x10; // clear AF
	}

	return true;
}

usz get_x64_access_size(x64_context* context, x64_op_t op, x64_reg_t reg, usz d_size, usz i_size)
{
	if (op == X64OP_MOVS || op == X64OP_STOS)
	{
		if (EFLAGS(context) & 0x400 /* direction flag */)
		{
			// TODO
			return 0;
		}

		if (reg != X64_NOT_SET) // get "full" access size from RCX register
		{
			u64 counter = 1;
			if (!get_x64_reg_value(context, reg, 8, i_size, counter))
			{
				return -1;
			}

			return d_size * counter;
		}
	}

	return d_size;
}

#elif defined(ARCH_ARM64)

#ifdef _WIN32
#define RIP(context) (reinterpret_cast<CONTEXT*>((context))->Pc)
#define GPR(context, index) (reinterpret_cast<CONTEXT*>((context))->X[index])
#elif defined(__APPLE__)
// https://github.com/bombela/backward-cpp/issues/200
#define RIP(context) ((context)->uc_mcontext->__ss.__pc)
#define GPR(context, index) ((context)->uc_mcontext->__ss.__x[(index)])
#elif defined(__FreeBSD__)
#define RIP(context) ((context)->uc_mcontext.mc_gpregs.gp_elr)
#define GPR(context, index) ((context)->uc_mcontext.mc_gpregs.gp_x[(index)])
#elif defined(__NetBSD__)
#define RIP(context) ((context)->uc_mcontext.__gregs[_REG_PC])
#define GPR(context, index) ((context)->uc_mcontext.__gregs[(index)])
#elif defined(__OpenBSD__)
#define RIP(context) ((context)->sc_elr)
#define GPR(context, index) ((context)->sc_x[(index)])
#else
#define RIP(context) ((context)->uc_mcontext.pc)
#define GPR(context, index) ((context)->uc_mcontext.regs[(index)])
#endif

enum mem_a64_op_t
{
	A64_INVALID = 0,
	A64_LOAD,
	A64_STORE,
};

struct a64_mem_info_t
{
	mem_a64_op_t op;
	u32 mem_size;   // Bytes accessed in memory
	u32 reg_size;   // Register width (4 or 8 bytes)
	u32 reg_num;
	bool reg_signed;
};

a64_mem_info_t decode_a64_mem_inst(u32 inst)
{
	a64_mem_info_t r{ A64_INVALID, 0, 0, inst % 32, false };

	// Exclude SIMD/FP loads/stores
	if ((inst >> 26) & 1)
	{
		return r;
	}

	// Scalar load/store immediate, unsigned offset variants only:
	// size[31:30]
	// V[26]
	// opc[23:22]
	// class bits[29:24] = 111001
	if ((inst & 0x3B000000) == 0x39000000)
	{
		const u32 size = (inst >> 30) & 3;
		const u32 opc  = (inst >> 22) & 3;

		r.mem_size = 1u << size;

		switch (opc)
		{
		case 0:
		{
			// STR
			r.op = A64_STORE;
			r.reg_size = r.mem_size;
			return r;
		}
		case 1:
		{
			// LDR unsigned zero-extend
			// size=3 (64-bit) -> Xt; everything else -> Wt
			r.op = A64_LOAD;
			r.reg_size = (size == 3) ? 8u : 4u;
			r.reg_signed = false;
			return r;
		}
		case 2:
		case 3:
		{
			if (size == 3)
			{
				return r;
			}

			if (size == 2 && opc == 3)
			{
				// Invalid LDRSW
				return r;
			}

			// LDRSB/LDRSH/LDRSW
			// size determines extension type:
			// 00 LDRSB
			// 01 LDRSH
			// 10 LDRSW
			r.op = A64_LOAD;

			if (size == 2)
			{
				// LDUSW
				r.reg_size = 8;
			}
			else
			{
				// LDRSB/LDRSH
				// opc=2 -> Wt, opc=3 -> Xt
				r.reg_size = (opc == 3) ? 4 : 8;
			}

			r.reg_signed = true;
			return r;
		}
		default:
			return r;
		}
	}

	// Scalar load/store unscaled immediate (LDUR/STUR)
	// size[31:30]
	// V[26]
	// opc[23:22]
	if ((inst & 0x3B200C00u) == 0x38000000u)
	{
		const u32 size = (inst >> 30) & 3;
		const u32 opc  = (inst >> 22) & 3;

		r.mem_size = 1u << size;

		switch (opc)
		{
		case 0:
		{
			// STURB/STURH/STUR Wt/STUR Xt
			r.op = A64_STORE;

			// Source register width
			r.reg_size = r.mem_size;
			return r;
		}

		case 1:
		{
			// LDURB/LDURH/LDUR Wt/LDUR Xt
			r.op = A64_LOAD;

			// Destination register width
			r.reg_size = (size == 3) ? 8 : 4;
			r.reg_signed = false;
			return r;
		}

		case 2:
		case 3:
		{
			// LDURSB/LDURSH/LDURSW
			if (size == 3)
			{
				return r;
			}

			r.op = A64_LOAD;
			r.reg_signed = true;

			if (size == 2)
			{
				// LDURSW
				r.reg_size = 8;
			}
			else
			{
				// LDURSB/LDURSH
				// opc=2 -> Wt, opc=3 -> Xt
				r.reg_size = (opc == 3) ? 4 : 8;
			}

			return r;
		}
		default:
			return r;
		}
	}

	// 
	// Literal loads:
	// 
	// LDR Wt, label
	// LDR Xt, label
	// LDRSW Xt, label
	//

	// This is not needed for MMIO (which is the only use for this function)

	// if ((inst & 0x3B000000) == 0x18000000)
	// {
	// 	u32 opc = (inst >> 30) & 3;

	// 	r.op = A64_LOAD;

	// 	switch (opc)
	// 	{
	// 	case 0: // LDR Wt literal
	// 	{
	// 		r.mem_size = 4;
	// 		r.reg_size = 4;
	// 		return r;
	// 	}
	// 	case 1: // LDR Xt literal
	// 	{
	// 		r.mem_size = 8;
	// 		r.reg_size = 8;
	// 		return r;
	// 	}
	// 	case 2: // LDRSW literal
	// 	{
	// 		r.mem_size = 4;
	// 		r.reg_size = 8;
	// 		r.reg_signed = true;
	// 		return r;
	// 	}
	// 	default:
	// 	{
	// 		break;
	// 	}
	// 	}
	// }

	return r;
}

void put_a64_reg_value(ucontext_t* context, u32 reg_index, u32 reg_size, bool reg_signed, u32 mem_size, u64 value)
{
	ensure(mem_size == 1 || mem_size == 2 || mem_size == 4 || mem_size == 8);
	ensure(reg_size == 1 || reg_size == 2 || reg_size == 4 || reg_size == 8);
	ensure(reg_size >= mem_size);
	ensure(reg_index < 32);

	if (reg_index == 31)
	{
		// XZR "register" 
		ensure(false);
	}

	auto make_mask = [](u32 bytes) -> u64
	{
		if (bytes == 8)
		{
			return umax;
		}

		const u64 bits = bytes * 8;
		return (u64{1} << bits) - 1;
	};

	// Mask for sign-extending the value
	const u64 sign_bit = value & (make_mask(mem_size) / 2 + 1);
	const u64 sign_mask = (reg_signed && sign_bit != 0 && reg_size > mem_size) ? (make_mask(reg_size) & ~make_mask(mem_size)) : 0;

	u64 temp_reg_value = 0;
	temp_reg_value |= (value & make_mask(mem_size)); // Set value (adjusted by size)
	temp_reg_value |= sign_mask; // Apply sign-extension
	GPR(context, reg_index) = temp_reg_value;
}

u64 get_a64_reg_value(ucontext_t* context, u32 reg_index, u32 reg_size)
{
	ensure(reg_size == 1 || reg_size == 2 || reg_size == 4 || reg_size == 8);
	ensure(reg_index < 32);

	if (reg_index == 31)
	{
		// XZR "register"
		return 0;
	}

	auto make_mask = [](u32 bytes) -> u64
	{
		if (bytes == 8)
		{
			return umax;
		}

		const u64 bits = bytes * 8;
		return (u64{1} << bits) - 1;
	};

	return (GPR(context, reg_index) & make_mask(reg_size));
}

#endif /* ARCH_ARM64 */

namespace rsx
{
	extern std::function<bool(u32 addr, bool is_writing)> g_access_violation_handler;
}

#ifdef __ORBIS__
using fault_context = utils::ps4::signal_context;
#else
using fault_context = ucontext_t;
#endif

#ifdef RPCS3_PS4
void ps4_fatal_note(const char* prefix, const char* detail) noexcept;

namespace
{
	// 0.39: consecutive retries of this thread on one host page (handle_access_violation, guest flags allow the access)
	thread_local u64 t_ps4_av_page = umax;
	thread_local u64 t_ps4_av_count = 0;
	thread_local u64 t_ps4_av_first_us = 0;
	thread_local u64 t_ps4_av_last_us = 0;
	atomic_t<u32> s_ps4_av_forced{0};

	void ps4_av_livelock_net(u32 addr, bool is_writing) noexcept
	{
		constexpr u64 host_page = 0x4000; // utils::get_page_size() on the console
		const u64 page = reinterpret_cast<u64>(vm::base(addr)) & ~(host_page - 1);
		const u64 now = get_system_time();

		// A gap of more than 10 ms starts a new episode: only back-to-back retries count
		if (page != t_ps4_av_page || now - t_ps4_av_last_us > 10'000)
		{
			t_ps4_av_page = page;
			t_ps4_av_count = 0;
			t_ps4_av_first_us = now;
		}

		t_ps4_av_last_us = now;
		const u64 count = ++t_ps4_av_count;
		const u64 elapsed = now - t_ps4_av_first_us;

		if ((count < 10'000 || elapsed < 100'000) && elapsed < 1'000'000)
		{
			return;
		}

		static const bool s_off = fs::is_file("/data/rpcs3/no-av-livelock-net.flag");

		if (s_off)
		{
			return;
		}

		t_ps4_av_page = umax;
		utils::memory_protect(reinterpret_cast<void*>(page), host_page, utils::protection::rw);

		if (const u32 n = ++s_ps4_av_forced; n <= 8 || (n & (n - 1)) == 0)
		{
			char line[256];
			std::snprintf(line, sizeof(line), "access violation livelock at 0x%x (%s; host page of guest 0x%x stayed protected while the guest flags allow it, "
				"page owner unresolved by the RSX fault handler): %llu retries in %llu ms; host page forced RW (#%u; no-av-livelock-net.flag disables)",
				addr, is_writing ? "writing" : "reading", static_cast<u32>(addr & ~(host_page - 1)), static_cast<unsigned long long>(count),
				static_cast<unsigned long long>(elapsed / 1000), n);
			sig_log.error("%s", line);
			ps4_fatal_note("", line);
		}
	}

	// 0.39: the guest access violation that pauses the emulation (Emu.Pause(true) below), for the frozen-game notice.
	// State: 0 empty, 1 being written or read, 2 ready.
	atomic_t<u32> s_ps4_guest_fault_state{0};
	atomic_t<u32> s_ps4_guest_fault_notes{0};
	char s_ps4_guest_fault[160]{};

	void ps4_note_guest_fault(cpu_thread* cpu, u32 addr, bool is_writing, bool is_exec) noexcept
	{
		// A fault while the emulation stops or loads does not freeze it
		if (Emu.IsStopped())
		{
			return;
		}

		const bool note = s_ps4_guest_fault_notes++ < 4;
		const bool keep = s_ps4_guest_fault_state.compare_and_swap_test(0, 1);

		if (!note && !keep)
		{
			return;
		}

		char text[sizeof(s_ps4_guest_fault)];
		const std::string name = cpu ? cpu->get_name() : thread_ctrl::get_name();
		std::snprintf(text, sizeof(text), "%s: %s 0x%x (%s)", name.c_str(), is_writing ? "writing" : (is_exec ? "executing" : "reading"), addr,
			(is_writing && vm::check_addr(addr)) ? "read-only memory" : "unmapped memory");

		if (note)
		{
			ps4_fatal_note("guest access violation: ", text);
		}

		if (keep)
		{
			std::memcpy(s_ps4_guest_fault, text, sizeof(text));
			s_ps4_guest_fault_state.store(2);
		}
	}
}

bool ps4_take_guest_fault(char* out, std::size_t size) noexcept
{
	if (!s_ps4_guest_fault_state.compare_and_swap_test(2, 1))
	{
		return false;
	}

	if (out && size)
	{
		const std::size_t n = std::min(size - 1, std::strlen(s_ps4_guest_fault));
		std::memcpy(out, s_ps4_guest_fault, n);
		out[n] = '\0';
	}

	s_ps4_guest_fault[0] = '\0';
	s_ps4_guest_fault_state.store(0);
	return true;
}
#endif

bool handle_access_violation(u32 addr, bool is_writing, bool is_exec, fault_context* context) noexcept
{
	g_tls_fault_all++;

	const auto cpu = get_current_cpu_thread();

	struct spu_unsavable
	{
		spu_thread* _spu;

		spu_unsavable(cpu_thread* cpu) noexcept
			: _spu(cpu ? cpu->try_get<spu_thread>() : nullptr)
		{
			if (_spu)
			{
				if (_spu->unsavable)
				{
					_spu = nullptr;
				}
				else
				{
					// Must not be saved inside access violation handler because it is unpredictable
					_spu->unsavable = true;
				}
			}
		}

		~spu_unsavable() noexcept
		{
			if (_spu)
			{
				_spu->unsavable = false;
			}
		}
	} spu_protection{cpu};

	if (!is_exec && addr < RAW_SPU_BASE_ADDR && vm::check_addr(addr) && rsx::g_access_violation_handler)
	{
		bool state_changed = false;

		if (cpu)
		{
			state_changed = vm::temporary_unlock(*cpu);
		}

		bool handled = rsx::g_access_violation_handler(addr, is_writing);

		if (state_changed && (cpu->state += cpu_flag::temp, cpu->test_stopped()))
		{
			//
		}

		if (handled)
		{
			g_tls_fault_rsx++;
			return true;
		}
	}

#if defined(ARCH_X64)
	const u8* const code = reinterpret_cast<u8*>(RIP(context));

	x64_op_t op;
	x64_reg_t reg;
	usz d_size;
	usz i_size;

	// decode single x64 instruction that causes memory access
	decode_x64_reg_op(code, op, reg, d_size, i_size);

	auto report_opcode = [=]()
	{
		if (op == X64OP_NONE)
		{
			be_t<v128> dump;
			std::memcpy(&dump, code, sizeof(dump));
			sig_log.error("decode_x64_reg_op(%p): unsupported opcode: %s", code, dump);
		}
	};

	if (0x1'0000'0000ull - addr < d_size)
	{
		sig_log.error("Invalid d_size (0x%llx)", d_size);
		report_opcode();
		return false;
	}

	// get length of data being accessed
	usz a_size = get_x64_access_size(context, op, reg, d_size, i_size);

	if (0x1'0000'0000ull - addr < a_size)
	{
		sig_log.error("Invalid a_size (0x%llx)", a_size);
		report_opcode();
		return false;
	}

	// check if address is RawSPU MMIO register
	do if (addr - RAW_SPU_BASE_ADDR < (6 * RAW_SPU_OFFSET) && (addr % RAW_SPU_OFFSET) >= RAW_SPU_PROB_OFFSET)
	{
		auto thread = idm::get_unlocked<named_thread<spu_thread>>(spu_thread::find_raw_spu((addr - RAW_SPU_BASE_ADDR) / RAW_SPU_OFFSET));

		if (!thread || is_exec)
		{
			break;
		}

		if (!a_size || !d_size || !i_size)
		{
			sig_log.error("Invalid or unsupported instruction (op=%d, reg=%d, d_size=%lld, a_size=0x%llx, i_size=%lld)", +op, +reg, d_size, a_size, i_size);
			report_opcode();
			return false;
		}

		bool handled = true;

		switch (op)
		{
		case X64OP_LOAD:
		case X64OP_LOAD_BE:
		case X64OP_LOAD_CMP:
		case X64OP_LOAD_TEST:
		{
			u32 value;
			const u32 addr_aligned = addr & -4;

			if (addr % 4 + a_size > 4)
			{
				handled = false;
				break;
			}

			if (is_writing || !thread->read_reg(addr_aligned, value))
			{
				return false;
			}

			// Adjust value for 8-bit and 16-bit reads
			value >>= ((4 - a_size) * 8) - ((addr % 4) * 8);
			value &= a_size == 4 ? u32{umax} : ((1u << (a_size * 8)) - 1);

			if (op != X64OP_LOAD_BE)
			{
				if (a_size == 4)
				{
					value = stx::se_storage<u32>::swap(value);
				}
				else if (a_size == 2)
				{
					value = stx::se_storage<u16>::swap(value);
				}
				else
				{
					ensure(a_size == 1);
				}
			}

			if (op == X64OP_LOAD_CMP)
			{
				u64 rvalue;
				if (!get_x64_reg_value(context, reg, d_size, i_size, rvalue) || !set_x64_cmp_flags(context, d_size, value, rvalue))
				{
					return false;
				}

				break;
			}

			if (op == X64OP_LOAD_TEST)
			{
				u64 rvalue;
				if (!get_x64_reg_value(context, reg, d_size, i_size, rvalue) || !set_x64_cmp_flags(context, d_size, value & rvalue, 0))
				{
					return false;
				}

				break;
			}

			if (!put_x64_reg_value(context, reg, d_size, value))
			{
				return false;
			}

			break;
		}
		case X64OP_BEXTR:
		{
			u32 value;
			const u32 addr_aligned = addr & -4;

			if (addr % 4 + a_size > 4)
			{
				handled = false;
				break;
			}

			if (is_writing || !thread->read_reg(addr_aligned, value))
			{
				return false;
			}

			// Adjust value for 8-bit and 16-bit reads
			value >>= ((4 - a_size) * 8) - ((addr % 4) * 8);
			value &= a_size == 4 ? u32{umax} : ((1u << (a_size * 8)) - 1);

			if (a_size == 4)
			{
				value = std::bit_cast<be_t<u32>>(value);
			}
			else if (a_size == 2)
			{
				value = std::bit_cast<be_t<u16>>(static_cast<u16>(value));
			}
			else
			{
				ensure(a_size == 1);
			}

			u64 ctrl;
			if (!get_x64_reg_value(context, s_tls_reg3, d_size, i_size, ctrl))
			{
				return false;
			}

			u8 start = ctrl & 0xff;
			u8 _len = (ctrl & 0xff00) >> 8;
			if (_len > 32)
				_len = 32;
			if (start > 32)
				start = 32;
			value = (u64{value} >> start) & ~(u64{umax} << _len);

			if (!put_x64_reg_value(context, reg, d_size, value) || !set_x64_cmp_flags(context, d_size, value, 0))
			{
				return false;
			}

			break;
		}
		case X64OP_STORE:
		case X64OP_STORE_BE:
		{
			if (a_size != 4)
			{
				// Might be unimplemented, such as writing MFC proxy EAL+EAH using 64-bit store
				handled = false;
				break;
			}

			u64 reg_value;
			if (!is_writing || !get_x64_reg_value(context, reg, d_size, i_size, reg_value))
			{
				return false;
			}

			u32 val32 = static_cast<u32>(reg_value);
			if (!thread->write_reg(addr, op == X64OP_STORE ? stx::se_storage<u32>::swap(val32) : val32))
			{
				return false;
			}

			break;
		}
		case X64OP_MOVS: // possibly, TODO
		case X64OP_STOS:
		default:
		{
			sig_log.error("Invalid or unsupported operation (op=%d, addr=0x%x, reg=%d, d_size=%lld, i_size=%lld, a_size=%d)", +op, addr, +reg, d_size, i_size, a_size);
			report_opcode();
			return false;
		}
		}

		if (!handled)
		{
			sig_log.error("Invalid or unsupported operation (op=%d, addr=0x%x, reg=%d, d_size=%lld, i_size=%lld, a_size=%d)", +op, addr, +reg, d_size, i_size, a_size);
			report_opcode();
			break;
		}

		// skip processed instruction
		RIP(context) += i_size;
		g_tls_fault_spu++;
		return true;
	} while (0);
#elif defined(ARCH_ARM64)
	const u8* const code = reinterpret_cast<u8*>(RIP(context));

	const u32 instruction = read_from_ptr_unsafe<u32>(code);

	const auto [op, mem_size, reg_size, reg_index, reg_signed] = decode_a64_mem_inst(instruction);

	auto report_opcode = [&]()
	{
		sig_log.error("decode_a64_mem_inst(%p): unsupported opcode: %s", code, +std::bit_cast<be_t<u32>>(instruction));
	};

	if (0x1'0000'0000ull - addr < mem_size)
	{
		sig_log.error("Invalid mem_size (0x%llx)", mem_size);
		report_opcode();
		return false;
	}

	// check if address is RawSPU MMIO register
	do if (addr - RAW_SPU_BASE_ADDR < (6 * RAW_SPU_OFFSET) && (addr % RAW_SPU_OFFSET) >= RAW_SPU_PROB_OFFSET)
	{
		auto thread = idm::get_unlocked<named_thread<spu_thread>>(spu_thread::find_raw_spu((addr - RAW_SPU_BASE_ADDR) / RAW_SPU_OFFSET));

		if (!thread || is_exec)
		{
			break;
		}

		if (!mem_size)
		{
			sig_log.error("Invalid or unsupported instruction (reg=%d, mem_size=%lld, reg_size=0x%llx)", reg_index, mem_size, reg_size);
			report_opcode();
			return false;
		}

		bool handled = true;

		switch (op)
		{
		case A64_LOAD:
		{
			u32 value;
			const u32 addr_aligned = addr & -4;

			if (addr % 4 + mem_size > 4)
			{
				handled = false;
				break;
			}

			if (is_writing || !thread->read_reg(addr_aligned, value))
			{
				return false;
			}

			// Adjust value for 8-bit and 16-bit reads
			value >>= ((4 - mem_size) * 8) - ((addr % 4) * 8);
			value &= mem_size == 4 ? u32{umax} : ((1u << (mem_size * 8)) - 1);

			if (mem_size == 4)
			{
				value = std::bit_cast<be_t<u32>>(value);
			}
			else if (mem_size == 2)
			{
				value = std::bit_cast<be_t<u16>>(static_cast<u16>(value));
			}
			else
			{
				ensure(mem_size == 1);
			}

			// Update register value
			put_a64_reg_value(context, reg_index, reg_size, reg_signed, mem_size, value);
			break;
		}
		case A64_STORE:
		{
			if (mem_size != 4)
			{
				// Might be unimplemented, such as writing MFC proxy EAL+EAH using 64-bit store
				handled = false;
				break;
			}

			if (!is_writing)
			{
				return false;
			}

			const u64 reg_value = get_a64_reg_value(context, reg_index, reg_size);
			const u32 val32 = static_cast<u32>(reg_value);
			if (!thread->write_reg(addr, std::bit_cast<be_t<u32>>(val32)))
			{
				return false;
			}

			break;
		}
		default:
		{
			sig_log.error("Invalid or unsupported operation (reg=%d, mem_size=%lld, reg_size=0x%llx)", reg_index, mem_size, reg_size);
			report_opcode();
			return false;
		}
		}

		if (!handled)
		{
			sig_log.error("Invalid or unsupported operation (reg=%d, mem_size=%lld, reg_size=0x%llx)", reg_index, mem_size, reg_size);
			report_opcode();
			break;
		}

		// skip processed instruction
		RIP(context) = reinterpret_cast<std::remove_cvref_t<decltype(RIP(context))>>(reinterpret_cast<const char*>(RIP(context)) + 4);
		g_tls_fault_spu++;
		return true;
	} while (0);
#endif /* ARCH_ */

	const auto required_page_perms = (is_writing ? vm::page_writable : vm::page_readable) + (is_exec ? vm::page_executable : 0);

	if (vm::check_addr(addr, required_page_perms))
	{
#ifdef RPCS3_PS4
		// 0.39: the guest flags allow the access but the host view faulted again: retried at once. A host page that
		// stays protected (e.g. a ZCULL report page whose fault handler answered "already faulted") makes that retry
		// endless. After 10000 consecutive retries on one host page, or 1 s of them, the page is made RW and logged
		// once per page episode (no-av-livelock-net.flag: retry forever, as 0.38).
		ps4_av_livelock_net(addr, is_writing);
#endif
		return true;
	}

	// Hack: allocate memory in case the emulator is stopping
	const auto hack_alloc = [&]()
	{
		const bool added_flag = cpu && !cpu->state.test_and_set(cpu_flag::wait);

		if (vm::check_addr(addr, required_page_perms))
		{
			if (added_flag)
			{
				cpu->check_state();
			}

			return true;
		}

		const auto area = vm::reserve_map(vm::any, addr & -0x10000, 0x10000);

		if (!area)
		{
			if (added_flag)
			{
				cpu->check_state();
			}

			return false;
		}

		extern void ppu_register_range(u32 addr, u32 size);

		bool reprotected = false;

		if (vm::writer_lock mlock; area->flags & vm::preallocated || vm::check_addr(addr, 0))
		{
			// For allocated memory with protection lower than required (such as protection::no or read-only while writing to it)
			utils::memory_protect(vm::base(addr & -0x1000), 0x1000, utils::protection::rw);
			reprotected = true;
		}

		if (reprotected)
		{
			if (is_exec && !vm::check_addr(addr, vm::page_executable))
			{
				ppu_register_range(addr & -0x10000, 0x10000);
			}
			
			if (added_flag)
			{
				cpu->check_state();
			}

			g_tls_access_violation_recovered = addr;
			return true;
		}

		const bool allocated = area->falloc(addr & -0x10000, 0x10000);

		if (allocated)
		{
			if (is_exec && !vm::check_addr(addr, vm::page_executable))
			{
				ppu_register_range(addr & -0x10000, 0x10000);
			}

			if (added_flag)
			{
				cpu->check_state();
			}

			g_tls_access_violation_recovered = addr;
			return true;
		}

		if (added_flag)
		{
			cpu->check_state();
		}
	
		return false;
	};

	if (cpu && (cpu->get_class() == thread_class::ppu || cpu->get_class() == thread_class::spu) && !is_exec)
	{
		vm::temporary_unlock(*cpu);
		u32 pf_port_id = 0;

		if (auto& pf_entries = g_fxo->get<page_fault_notification_entries>(); true)
		{
			if (auto mem = vm::get(vm::any, addr))
			{
				reader_lock lock(pf_entries.mutex);

				for (const auto& entry : pf_entries.entries)
				{
					if (entry.start_addr == mem->addr)
					{
						pf_port_id = entry.port_id;
						break;
					}
				}
			}
		}

		if (auto pf_port = idm::get_unlocked<lv2_obj, lv2_event_port>(pf_port_id); pf_port && pf_port->queue)
		{
			// We notify the game that a page fault occurred so it can rectify it.
			// Note, for data3, were the memory readable AND we got a page fault, it must be due to a write violation since reads are allowed.
			u64 data1 = addr;
			u64 data2 = 0;

			if (cpu->try_get<ppu_thread>())
			{
				data2 = (SYS_MEMORY_PAGE_FAULT_TYPE_PPU_THREAD << 32) | cpu->id;
			}
			else if (auto spu = cpu->try_get<spu_thread>())
			{
				const u64 type = spu->get_type() == spu_type::threaded ?
					SYS_MEMORY_PAGE_FAULT_TYPE_SPU_THREAD :
					SYS_MEMORY_PAGE_FAULT_TYPE_RAW_SPU;

				data2 = (type << 32) | spu->lv2_id;
			}

			u64 data3;
			{
				vm::writer_lock rlock;
				if (vm::check_addr(addr, is_writing ? vm::page_writable : vm::page_readable))
				{
					// Memory was allocated inbetween, retry
					return true;
				}
				else if (vm::check_addr(addr))
				{
					data3 = SYS_MEMORY_PAGE_FAULT_CAUSE_READ_ONLY; // TODO
				}
				else
				{
					data3 = SYS_MEMORY_PAGE_FAULT_CAUSE_NON_MAPPED;
				}
			}


			// Now, place the page fault event onto table so that other functions [sys_mmapper_free_address and pagefault recovery funcs etc]
			// know that this thread is page faulted and where.

			auto& pf_events = g_fxo->get<page_fault_event_entries>();

			// De-schedule
			if (cpu->get_class() == thread_class::ppu)
			{
				cpu->state -= cpu_flag::signal; // Cannot use check_state here and signal must be removed if exists
				lv2_obj::sleep(*cpu);
			}

			auto send_event = [&]() -> error_code
			{
				lv2_obj::notify_all_t notify_later{};

				std::lock_guard pf_lock(pf_events.pf_mutex);

				if (auto error = pf_port->queue->send(pf_port->name ? pf_port->name : ((u64{process_getpid() + 0u} << 32) | u64{pf_port_id}), data1, data2, data3))
				{
					return error;
				}

				pf_events.events.emplace(cpu, addr);
				return {};
			};

			sig_log.warning("Page_fault %s location 0x%x because of %s memory", is_writing ? "writing" : "reading",
				addr, data3 == SYS_MEMORY_PAGE_FAULT_CAUSE_READ_ONLY ? "writing read-only" : "using unmapped");

			if (cpu->get_class() == thread_class::ppu)
			{
				if (const auto func = static_cast<ppu_thread*>(cpu)->current_function)
				{
					sig_log.warning("Page_fault while in function %s", func);
				}
			}

			error_code sending_error = not_an_error(CELL_EBUSY);

			// If we fail due to being busy, wait a bit and try again.
			for (u64 sleep_until = get_system_time(); static_cast<u32>(sending_error) == CELL_EBUSY; thread_ctrl::wait_until(&sleep_until, 1000))
			{
				sending_error = send_event();

				if (cpu->is_stopped())
				{
					sending_error = {};
					break;
				}
			}

			if (sending_error)
			{
				vm_log.error("Unknown error 0x%x while trying to pass page fault.", +sending_error);
				return false;
			}
			else
			{
				// Wait until the thread is recovered
				while (auto state = cpu->state.fetch_sub(cpu_flag::signal))
				{
					if (is_stopped(state) || state & cpu_flag::signal)
					{
						break;
					}

					thread_ctrl::wait_on(cpu->state, state);
				}
			}

			// Reschedule, test cpu state and try recovery if stopped
			if (cpu->test_stopped() && !hack_alloc())
			{
				return false;
			}

			return true;
		}

		if (cpu->get_class() == thread_class::spu)
		{
			if (g_tls_access_violation_recovered != addr)
			{
				vm_log.notice("\n%s", dump_useful_thread_info());
				vm_log.always()("[%s] Access violation %s location 0x%x (%s)", cpu->get_name(), is_writing ? "writing" : "reading", addr, (is_writing && vm::check_addr(addr)) ? "read-only memory" : "unmapped memory");
			}

			// TODO:
			// RawSPU: Send appropriate interrupt
			// SPUThread: Send sys_spu exception event
			cpu->state += cpu_flag::dbg_pause;

			if (cpu->check_state() && !hack_alloc())
			{
				return false;
			}

			return true;
		}
		else
		{
			if (auto last_func = static_cast<ppu_thread*>(cpu)->current_function)
			{
				ppu_log.fatal("Function aborted: %s", last_func);
			}

			lv2_obj::sleep(*cpu);
		}
	}

	if (cpu)
	{
		cpu->state += cpu_flag::wait;
	}

	// Note: a thread may access violate more than once after hack_alloc recovery
	// Do not log any further access violations in this case.
	if (g_tls_access_violation_recovered != addr)
	{
		vm_log.notice("\n%s", dump_useful_thread_info());
		vm_log.fatal("Access violation %s location 0x%x (%s)", is_writing ? "writing" : (is_exec ? "executing" : "reading"), addr, (is_writing && vm::check_addr(addr)) ? "read-only memory" : "unmapped memory");
#ifdef RPCS3_PS4
		// Named in startup.log (first 4 per process) and kept for the frozen-game notice before Emu.Pause(true) below
		ps4_note_guest_fault(cpu, addr, is_writing, is_exec);
#endif
	}

	while (Emu.IsPausedOrReady())
	{
		if (cpu)
		{
			auto state = +cpu->state;

			if (::is_paused(state) && !::is_stopped(state))
			{
				thread_ctrl::wait_on(cpu->state, state);
			}
			else
			{
				// Temporary until Emulator updates state
				std::this_thread::yield();
			}
		}
		else
		{
			thread_ctrl::wait_for(1000);
		}
	}

	Emu.Pause(true);

	while (Emu.IsPaused())
	{
		if (cpu)
		{
			auto state = +cpu->state;

			if (::is_paused(state) && !::is_stopped(state))
			{
				thread_ctrl::wait_on(cpu->state, state);
			}
			else
			{
				// Temporary until Emulator updates state
				std::this_thread::yield();
			}
		}
		else
		{
			thread_ctrl::wait_for(1000);
		}
	}

	if (Emu.IsStopped())
	{
		// Keep retrying until the page is mapped: the thread must be able to resume so it can observe cpu_flag::exit and terminate cleanly.
		// Reporting the violation as unhandled here would escalate a guest crash into an unhandled host exception.
		while (!hack_alloc())
		{
			thread_ctrl::wait_for(1000);
		}
	}

	return true;
}

static void append_thread_name(std::string& msg)
{
	if (thread_ctrl::get_current())
	{
		fmt::append(msg, "Emu Thread Name: '%s'.\n", thread_ctrl::get_name());
	}
	else if (thread_ctrl::is_main())
	{
		fmt::append(msg, "Thread: Main Thread.\n");
	}
	else
	{
		fmt::append(msg, "Thread id = %u.\n", thread_ctrl::get_tid());
	}
}

#ifdef _WIN32

static LONG exception_handler(PEXCEPTION_POINTERS pExp) noexcept
{
	if (pExp->ExceptionRecord->ExceptionCode == EXCEPTION_BREAKPOINT)
	{
		return EXCEPTION_CONTINUE_SEARCH;
	}

	const auto ptr = reinterpret_cast<u8*>(pExp->ExceptionRecord->ExceptionInformation[1]);
	const bool is_writing = pExp->ExceptionRecord->ExceptionInformation[0] == 1;
	const bool is_executing = pExp->ExceptionRecord->ExceptionInformation[0] == 8;

	if (pExp->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && !is_executing)
	{
		u32 addr = 0;
		bool is_exec = false;

		if (auto [addr0, ok] = vm::try_get_addr(ptr); ok)
		{
			addr = addr0;
		}
		else if (const usz exec64 = (ptr - vm::g_exec_addr) / 2; exec64 <= u32{umax})
		{
			is_exec = true;
			addr = static_cast<u32>(exec64);
		}
		else if (const usz seg_off = (ptr - vm::g_exec_addr - vm::g_exec_addr_seg_offset); seg_off <= u32{umax} / 2)
		{
			// Segment map holds one u16 per 4 bytes of guest code: ptr = g_exec_addr + g_exec_addr_seg_offset + (addr >> 1)
			is_exec = true;
			addr = static_cast<u32>(seg_off * 2);
		}
		else 
		{
			std::this_thread::sleep_for(1ms);
			return EXCEPTION_CONTINUE_SEARCH;
		}

		if (thread_ctrl::get_current() && handle_access_violation(addr, is_writing, is_exec, pExp->ContextRecord))
		{
			return EXCEPTION_CONTINUE_EXECUTION;
		}
	}

	switch (pExp->ExceptionRecord->ExceptionCode)
	{
	case EXCEPTION_ACCESS_VIOLATION:
	case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:
	case EXCEPTION_DATATYPE_MISALIGNMENT:
	case EXCEPTION_ILLEGAL_INSTRUCTION:
	case EXCEPTION_IN_PAGE_ERROR:
	case EXCEPTION_INT_DIVIDE_BY_ZERO:
	case EXCEPTION_NONCONTINUABLE_EXCEPTION:
	case EXCEPTION_PRIV_INSTRUCTION:
	//case EXCEPTION_STACK_OVERFLOW:
	{
		sys_log.notice("\n%s", dump_useful_thread_info());
		logs::listener::sync_all();
		break;
	}
	default:
	{
		break;
	}
	}

	return EXCEPTION_CONTINUE_SEARCH;
}

static LONG exception_filter(PEXCEPTION_POINTERS pExp) noexcept
{
	std::string msg = fmt::format("Unhandled Win32 exception 0x%08X.\n", pExp->ExceptionRecord->ExceptionCode);

	if (pExp->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
	{
		const auto cause =
			pExp->ExceptionRecord->ExceptionInformation[0] == 8 ? "executing" :
			pExp->ExceptionRecord->ExceptionInformation[0] == 1 ? "writing" : "reading";

		fmt::append(msg, "Segfault %s location %p at %p.\n", cause, pExp->ExceptionRecord->ExceptionInformation[1], pExp->ExceptionRecord->ExceptionAddress);

		if (vm::try_get_addr(reinterpret_cast<u8*>(pExp->ExceptionRecord->ExceptionInformation[1])).second)
		{
			fmt::append(msg, "Sudo Addr: %p, VM Addr: %p\n", vm::g_sudo_addr, vm::g_base_addr);
		}
	}
	else
	{
		fmt::append(msg, "Exception address: %p.\n", pExp->ExceptionRecord->ExceptionAddress);

		for (DWORD i = 0; i < pExp->ExceptionRecord->NumberParameters; i++)
		{
			fmt::append(msg, "ExceptionInformation[0x%x]: %p.\n", i, pExp->ExceptionRecord->ExceptionInformation[i]);
		}
	}

	append_thread_name(msg);

	std::vector<HMODULE> modules;
	for (DWORD size = 256; modules.size() != size; size /= sizeof(HMODULE))
	{
		modules.resize(size);
		if (!EnumProcessModules(GetCurrentProcess(), modules.data(), size * sizeof(HMODULE), &size))
		{
			modules.clear();
			break;
		}
	}

#if defined(ARCH_X64)
	const auto exec_addr = pExp->ContextRecord->Rip;
#elif defined(ARCH_ARM64)
	const auto exec_addr = pExp->ContextRecord->Pc;
#else
#error "Unimplemented exception handling for this architecture"
#endif

	fmt::append(msg, "Instruction address: %p.\n", exec_addr);

	DWORD64 unwind_base;
	if (const auto rtf = RtlLookupFunctionEntry(exec_addr, &unwind_base, nullptr))
	{
		// Get function address
		const DWORD64 func_addr = rtf->BeginAddress + unwind_base;
		fmt::append(msg, "Function address: %p (base+0x%x).\n", func_addr, rtf->BeginAddress);

		// Access UNWIND_INFO structure
		//const auto uw = (u8*)(unwind_base + rtf->UnwindData);
	}

	for (HMODULE _module : modules)
	{
		MODULEINFO info;
		if (GetModuleInformation(GetCurrentProcess(), _module, &info, sizeof(info)))
		{
			const DWORD64 base = reinterpret_cast<DWORD64>(info.lpBaseOfDll);

			if (exec_addr >= base && exec_addr < base + info.SizeOfImage)
			{
				std::string module_name;
				for (DWORD size = 15; module_name.size() != size;)
				{
					module_name.resize(size);
					size = GetModuleBaseNameA(GetCurrentProcess(), _module, &module_name.front(), size + 1);
					if (!size)
					{
						module_name.clear();
						break;
					}
				}

				fmt::append(msg, "Module name: '%s'.\n", module_name);
				fmt::append(msg, "Module base: %p.\n", info.lpBaseOfDll);
			}
		}
	}

	fmt::append(msg, "RPCS3 image base: %p.\n", GetModuleHandle(NULL));
	
#if defined(ARCH_X64)
	fmt::append(msg, "RAX: %016llX	RBX: %016llX\n", pExp->ContextRecord->Rax, pExp->ContextRecord->Rbx);
	fmt::append(msg, "RCX: %016llX	RDX: %016llX\n", pExp->ContextRecord->Rcx, pExp->ContextRecord->Rdx);
	fmt::append(msg, "RSI: %016llX  RDI: %016llX\n", pExp->ContextRecord->Rsi, pExp->ContextRecord->Rdi);
	fmt::append(msg, "RBP: %016llX  RSP: %016llX\n", pExp->ContextRecord->Rbp, pExp->ContextRecord->Rsp);
	fmt::append(msg, "R8:  %016llX  R9:  %016llX\n", pExp->ContextRecord->R8,  pExp->ContextRecord->R9);
	fmt::append(msg, "R10: %016llX  R11: %016llX\n", pExp->ContextRecord->R10, pExp->ContextRecord->R11);
	fmt::append(msg, "R12: %016llX  R13: %016llX\n", pExp->ContextRecord->R12, pExp->ContextRecord->R13);
	fmt::append(msg, "R14: %016llX  R15: %016llX\n", pExp->ContextRecord->R14, pExp->ContextRecord->R15);
	fmt::append(msg, "RFLAGS: %08X\n", pExp->ContextRecord->EFlags);
#elif defined(ARCH_ARM64)
	for (int i = 0; i < 29; i += 2)
	{
		if (i + 1 < 29)
			fmt::append(msg, "X%-2d: %016llX  X%-2d: %016llX\n", i, pExp->ContextRecord->X[i], i + 1, pExp->ContextRecord->X[i + 1]);
		else
			fmt::append(msg, "X%-2d: %016llX\n", i, pExp->ContextRecord->X[i]);
	}
	fmt::append(msg, "SP: %016llX  FP: %016llX  LR: %016llX\n", pExp->ContextRecord->Sp, pExp->ContextRecord->Fp, pExp->ContextRecord->Lr);
	fmt::append(msg, "CPSR: %08X\n", pExp->ContextRecord->Cpsr);
#endif

	const auto stack_trace = utils::get_backtrace(64, pExp->ContextRecord);
	const auto stack_symbols = utils::get_backtrace_symbols(stack_trace);

	msg += "Stack Trace:\n";

	for (const auto& symbol : stack_symbols)
	{
		fmt::append(msg, "%s\n", symbol);
	}
	
	sys_log.fatal("\n%s", msg);
	logs::listener::sync_all();

	thread_ctrl::emergency_exit(msg);
}

const bool s_exception_handler_set = []() -> bool
{
#ifdef USE_ASAN
	if (!AddVectoredExceptionHandler(FALSE, static_cast<PVECTORED_EXCEPTION_HANDLER>(exception_handler)))
#else
	if (!AddVectoredExceptionHandler(1, static_cast<PVECTORED_EXCEPTION_HANDLER>(exception_handler)))
#endif
	{
		report_fatal_error("AddVectoredExceptionHandler() failed.");
	}

	if (!SetUnhandledExceptionFilter(static_cast<LPTOP_LEVEL_EXCEPTION_FILTER>(exception_filter)))
	{
		report_fatal_error("SetUnhandledExceptionFilter() failed.");
	}

	return true;
}();

#else

static void signal_handler(int /*sig*/, siginfo_t* info, void* uct) noexcept
{
	fault_context* context = static_cast<fault_context*>(uct);

#if defined(ARCH_X64)
#ifdef __ORBIS__
	const u64 err = context->machine.error;
	if (utils::ps4::memory::handle_lazy_fault(info->si_addr, err))
	{
		// Retry the original data instruction after backing its private page.
		return;
	}
	ps4_trace_fault(*context, info->si_addr);
#elif defined(__APPLE__)
	const u64 err = context->uc_mcontext->__es.__err;
#elif defined(__DragonFly__) || (defined(__FreeBSD__) && !defined(__ORBIS__))
	const u64 err = context->uc_mcontext.mc_err;
#elif defined(__OpenBSD__)
	const u64 err = context->sc_err;
#elif defined(__NetBSD__)
	const u64 err = context->uc_mcontext.__gregs[_REG_ERR];
#else
	const u64 err = context->uc_mcontext.gregs[REG_ERR];
#endif

	const bool is_executing = err & 0x10;
	const bool is_writing = err & 0x2;
#elif defined(ARCH_ARM64)
	const bool is_executing = uptr(info->si_addr) == uptr(RIP(context));

#if defined(__linux__) || defined(__APPLE__)
	// Current CPU state decoder is reverse-engineered from the linux kernel and may not work on other platforms.
	const auto decoded_reason = aarch64::decode_fault_reason(context);
	const bool is_writing = (decoded_reason == aarch64::fault_reason::data_write);

	if (decoded_reason != aarch64::fault_reason::data_write &&
		decoded_reason != aarch64::fault_reason::data_read)
	{
		// We don't expect other classes of exceptions during normal executions
		sig_log.warning("Unexpected fault. Reason: %d", static_cast<int>(decoded_reason));
	}

#else
	const u32 insn = is_executing ? 0 : read_from_ptr_unsafe<u32>(RIP(context));
	const bool is_writing =
		(insn & 0xbfff0000) == 0x0c000000 ||  // STR <Wt>, [<Xn>, #<imm>] (store word with immediate offset)
		(insn & 0xbfe00000) == 0x0c800000 ||  // STP <Wt1>, <Wt2>, [<Xn>, #<imm>] (store pair of registers with immediate offset)
		(insn & 0xbfdf0000) == 0x0d000000 ||  // STR <Wt>, [<Xn>, <Xm>] (store word with register offset)
		(insn & 0xbfc00000) == 0x0d800000 ||  // STP <Wt1>, <Wt2>, [<Xn>, <Xm>] (store pair of registers with register offset)
		(insn & 0x3f400000) == 0x08000000 ||  // STR <Vd>, [<Xn>, #<imm>] (store SIMD/FP register with immediate offset)
		(insn & 0x3bc00000) == 0x39000000 ||  // STR <Wt>, [<Xn>, #<imm>] (store word with immediate offset)
		(insn & 0x3fc00000) == 0x3d800000 ||  // STR <Vd>, [<Xn>, <Xm>] (store SIMD/FP register with register offset)
		(insn & 0x3bc00000) == 0x38000000 ||  // STR <Wt>, [<Xn>, <Xm>] (store word with register offset)
		(insn & 0x3fe00000) == 0x3c800000 ||  // STUR <Vd>, [<Xn>, #<imm>] (store unprivileged register with immediate offset)
		(insn & 0x3fe00000) == 0x3ca00000 ||  // STR <Vd>, [<Xn>, #<imm>] (store SIMD/FP register with immediate offset)
		(insn & 0x3a400000) == 0x28000000 ||  // STP <Wt1>, <Wt2>, [<Xn>, #<imm>] (store pair of registers with immediate offset)
		(insn & 0xbf000000) == 0xad000000 ||  // STP <Vd1>, <Vd2>, [<Xn>, #<imm>] (store SIMD/FP 128-bit register pair with immediate offset)
		(insn & 0xbf000000) == 0x6d000000;    // STP <Dd1>, <Dd2>, [<Xn>, #<imm>] (store SIMD/FP 64-bit register pair with immediate offset)
#endif

#else
#error "signal_handler not implemented"
#endif

	const u64 exec64 = (reinterpret_cast<u64>(info->si_addr) - reinterpret_cast<u64>(vm::g_exec_addr)) / 2;
	// Segment map holds one u16 per 4 bytes of guest code: si_addr = g_exec_addr + g_exec_addr_seg_offset + (addr >> 1)
	const u64 seg_off = (reinterpret_cast<u64>(info->si_addr) - reinterpret_cast<u64>(vm::g_exec_addr)) - vm::g_exec_addr_seg_offset;
	const auto cause = is_executing ? "executing" : is_writing ? "writing" : "reading";

	if (auto [addr, ok] = vm::try_get_addr(info->si_addr); ok && !is_executing)
	{
		// Try to process access violation
		if (thread_ctrl::get_current() && handle_access_violation(addr, is_writing, false, context))
		{
			return;
		}
	}

	if (exec64 < 0x100000000ull && !is_executing)
	{
		if (thread_ctrl::get_current() && handle_access_violation(static_cast<u32>(exec64), is_writing, true, context))
		{
			return;
		}
	}
	else if (seg_off < 0x80000000ull && !is_executing)
	{
		if (thread_ctrl::get_current() && handle_access_violation(static_cast<u32>(seg_off * 2), is_writing, true, context))
		{
			return;
		}
	}

	std::string msg = fmt::format("Segfault %s location %p at %p.\n", cause, info->si_addr, RIP(context));

#ifdef __ORBIS__
	ps4_trace_fault(*context, info->si_addr, true);
	extern char __text_start[];
	const auto base = reinterpret_cast<u64>(__text_start);
	fmt::append(msg, "PS4 native context: RIP=0x%x RSP=0x%x RBP=0x%x ERR=0x%x TRAP=%u\n",
		context->machine.rip, context->machine.rsp, context->machine.rbp, err, context->machine.trapno);
	fmt::append(msg, "Module base=0x%x RIP offset=0x%x fault address=0x%x context address=0x%x\n",
		base, context->machine.rip - base, reinterpret_cast<u64>(info->si_addr), context->machine.address);
#endif

	if (vm::try_get_addr(info->si_addr).second)
	{
		fmt::append(msg, "Sudo Addr: %p, VM Addr: %p\n", vm::g_sudo_addr, vm::g_base_addr);
	}

	append_thread_name(msg);

#ifdef __APPLE__
	thread_local bool s_tls_is_attempting_recovery = false;
	thread_local bool s_tls_last_cause_is_executing = false;

	if (reinterpret_cast<u64>(info->si_addr) < 0x10000)
	{
		// Do not recover from the virtual page of 0x0 (such as nullptr)
	}
	else if (is_executing || is_writing)
	{
		if (s_tls_is_attempting_recovery && s_tls_last_cause_is_executing != is_executing)
		{
			// Cause changed, inform recovery
			s_tls_is_attempting_recovery = false;
		}

		if (!s_tls_is_attempting_recovery)
		{
			s_tls_last_cause_is_executing = is_executing;
			s_tls_is_attempting_recovery = true;
			pthread_jit_write_protect_np(is_executing ? true : false);

			sys_log.error("\n%s", msg);
			sys_log.notice("\n%s", dump_useful_thread_info());
			sys_log.error("Attempting recovery using pthread_jit_write_protect_np()");
			logs::listener::sync_all();
			return;
		}
	}
#endif

	sys_log.fatal("\n%s", msg);
	sys_log.notice("\n%s", dump_useful_thread_info());
	logs::listener::sync_all();

	if (IsDebuggerPresent())
	{
		// Convert to SIGTRAP
		raise(SIGTRAP);
		return;
	}

	thread_ctrl::emergency_exit(msg);
}

#ifdef __ORBIS__
// 0.33: where an instruction lives, for fatal records. ELF offsets match llvm-nm of rpcs3_ps4.elf (.text at address 0).
// jit_runtime holds asmjit and SPU LLVM code; PPU LLVM code lives in the reserve_jit window.
static std::string ps4_code_region(u64 addr)
{
	extern char __text_start[];
	extern char __eh_frame_start[];

	const u64 text_lo = reinterpret_cast<u64>(__text_start);
	const u64 text_hi = reinterpret_cast<u64>(__eh_frame_start);

	if (addr >= text_lo && addr < text_hi)
	{
		return fmt::format("ELF text, elf+0x%x", addr - text_lo);
	}

	const u64 rt_lo = reinterpret_cast<u64>(jit_runtime::alloc(0, 0, true));
	const u64 rt_hi = reinterpret_cast<u64>(jit_runtime::peek(true));

	if (addr >= rt_lo && addr < rt_hi)
	{
		return fmt::format("JIT arena: jit_runtime code+0x%x (asmjit, SPU LLVM)", addr - rt_lo);
	}

	if (addr >= utils::ps4::memory::jit_virtual_begin && addr < utils::ps4::memory::jit_virtual_end)
	{
		return fmt::format("JIT arena: PPU LLVM window 0x%x-0x%x", utils::ps4::memory::jit_virtual_begin, utils::ps4::memory::jit_virtual_end);
	}

	return "not in the ELF text or a JIT arena";
}

// 0.33: the CPU the JIT generates code for (the console always targets its own CPU), with the host brand
static std::string ps4_jit_cpu_text()
{
#ifdef LLVM_AVAILABLE
	return fmt::format("LLVM target '%s', host '%s'", jit_compiler::cpu({}), utils::get_cpu_brand());
#else
	return fmt::format("asmjit only (built without LLVM), host '%s'", utils::get_cpu_brand());
#endif
}
#endif

static void sigill_handler(int /*sig*/, siginfo_t* info, [[maybe_unused]] void* uct) noexcept
{
	std::string msg = fmt::format("Illegal instruction at %p (%s).\n", info->si_addr, *reinterpret_cast<be_t<u128>*>(info->si_addr));

#ifdef __ORBIS__
	// An instruction the Jaguar lacks (AVX2/BMI2/FMA/...) in JIT code points at the JIT target, not at the emulator
	ps4_trace_fault(*static_cast<fault_context*>(uct), info->si_addr, true);
	fmt::append(msg, "Code: %s.\nJIT CPU: %s.\n", ps4_code_region(reinterpret_cast<u64>(info->si_addr)), ps4_jit_cpu_text());
#endif

	append_thread_name(msg);

	sys_log.fatal("\n%s", msg);
	sys_log.notice("\n%s", dump_useful_thread_info());
	logs::listener::sync_all();

	if (IsDebuggerPresent())
	{
		// Convert to SIGTRAP
		raise(SIGTRAP);
		return;
	}

	thread_ctrl::emergency_exit(msg);
}

#ifdef __ORBIS__
// 0.33: SIGFPE (integer divide error), SIGTRAP (int3, debug trap), SIGEMT and SIGXFSZ had no handler, so the default
// action ended the process (CE-34878-0) with nothing recorded. They become a recorded thread fatal and a return to
// the XMB, as an integer divide by zero already is on Windows. Signal numbers are FreeBSD's.
static void ps4_fatal_signal_handler(int sig, siginfo_t* info, void* uct) noexcept
{
	const auto context = static_cast<fault_context*>(uct);
	const auto& machine = context->machine;

	const char* const name = sig == 8 ? "SIGFPE, arithmetic exception" : sig == 5 ? "SIGTRAP, trap instruction" :
		sig == 7 ? "SIGEMT" : sig == 25 ? "SIGXFSZ, file size limit exceeded" : "fatal signal";

	ps4_trace_fault(*context, info->si_addr, true);

	extern char __text_start[];
	const u64 base = reinterpret_cast<u64>(__text_start);

	std::string msg = fmt::format("Exception (signal %d: %s; code %d) at RIP 0x%x.\n", sig, name, info->si_code, machine.rip);
	fmt::append(msg, "PS4 native context: RIP=0x%x RSP=0x%x RBP=0x%x ERR=0x%x TRAP=%u\n", machine.rip, machine.rsp, machine.rbp, machine.error, machine.trapno);
	fmt::append(msg, "Module base=0x%x RIP offset=0x%x signal address=0x%x\n", base, machine.rip - base, reinterpret_cast<u64>(info->si_addr));
	fmt::append(msg, "Code: %s.\n", ps4_code_region(machine.rip));

	append_thread_name(msg);

	sys_log.fatal("\n%s", msg);
	sys_log.notice("\n%s", dump_useful_thread_info());
	logs::listener::sync_all();

	thread_ctrl::emergency_exit(msg);
}
#endif

#ifdef RPCS3_PS4
// 0.29: counts SIGPIPE deliveries; the own-stack self-test sends one to itself to prove that the kernel can
// build a signal frame on an RPCS3-owned direct-memory stack (every access violation needs exactly that).
static std::atomic<u32> s_ps4_sigpipe_seen{0};
#endif

void sigpipe_signaling_handler(int)
{
#ifdef RPCS3_PS4
	s_ps4_sigpipe_seen.fetch_add(1, std::memory_order_relaxed);
#endif
}

const bool s_exception_handler_set = []() -> bool
{
	struct ::sigaction sa;
	sa.sa_flags = SA_SIGINFO;
	sigemptyset(&sa.sa_mask);
	sa.sa_sigaction = signal_handler;

	if (::sigaction(SIGSEGV, &sa, NULL) == -1)
	{
		std::fprintf(stderr, "sigaction(SIGSEGV) failed (%d).\n", errno);
		std::abort();
	}

#if defined(__APPLE__) || defined(__ORBIS__)
#ifdef __ORBIS__
	// OpenOrbis bits/signal.h also retains Linux's SIGBUS=7; PS4 uses 10.
	constexpr int native_sigbus = 10;
#else
	constexpr int native_sigbus = SIGBUS;
#endif
	if (::sigaction(native_sigbus, &sa, NULL) == -1)
	{
		std::fprintf(stderr, "sigaction(SIGBUS) failed (%d).\n", errno);
		std::abort();
	}
#endif

	sa.sa_sigaction = sigill_handler;
	if (::sigaction(SIGILL, &sa, NULL) == -1)
	{
		std::fprintf(stderr, "sigaction(SIGILL) failed (%d).\n", errno);
		std::abort();
	}

	sa.sa_handler = sigpipe_signaling_handler;
	if (::sigaction(SIGPIPE, &sa, NULL) == -1)
	{
		std::fprintf(stderr, "sigaction(SIGPIPE) failed (%d).\n", errno);
		std::abort();
	}

#ifdef __ORBIS__
	// 0.33: FreeBSD SIGFPE 8, SIGTRAP 5, SIGEMT 7, SIGXFSZ 25 as literals: the SDK headers keep Linux values (their 7
	// is SIGBUS). Not SIGSYS 12: libkernel's _exit ends in it and the port never calls _exit. Not fatal on failure:
	// the default action stays, as before.
	sa.sa_flags = SA_SIGINFO;
	sa.sa_sigaction = ps4_fatal_signal_handler;

	for (const int native_sig : {8, 5, 7, 25})
	{
		if (::sigaction(native_sig, &sa, NULL) == -1)
		{
			std::fprintf(stderr, "sigaction(%d) failed (%d).\n", native_sig, errno);
		}
	}
#endif

	std::printf("Debugger: %d\n", +IsDebuggerPresent());
	return true;
}();

#endif

#ifdef RPCS3_PS4
// Defined in rpcs3/ps4_fatal.cpp. Declared at namespace scope with the same attributes as
// ps4_fatal.h (a block-scope declaration without [[noreturn]] is ill-formed).
[[noreturn]] void ps4_terminate_handler() noexcept;
void ps4_note_thread_fatal(const char* reason) noexcept;
void ps4_fatal_note(const char* prefix, const char* detail) noexcept;

// rpcs3/ps4_fatal.cpp: memory and orbis-compat thread counters (0.28 diagnostics).
unsigned long long ps4_internal_memory_free() noexcept;
unsigned long long ps4_flexible_memory_free() noexcept;
void ps4_thread_census(unsigned long* created, unsigned long* finished) noexcept;
#endif

#ifdef __ORBIS__
namespace orbis
{
	// 0.29 (orbis-compat orbis_thread.cpp): a thread_base thread leaves through pthread_exit, so the
	// interposer's trampoline never sees it return; this note keeps orbis' live-thread count exact.
	// Called once per thread_base thread, on that thread, right before each of its pthread_exit calls.
	void threadNoteExit() noexcept;
	// 0.29: called once after each successful pthread_join of a thread_base thread (~thread_base),
	// so exited-but-unjoined threads (whose libkernel stacks are still held) = noted exits - noted joins.
	void threadNoteJoin() noexcept;
	// Threads created through the interposer minus those that exited (main thread excluded).
	unsigned long threadsLive() noexcept;

	// 0.39 (orbis-compat include/orbis_thread.h, redeclared as in ps4_fatal.cpp; fixed 72-byte layout): every thread
	// counter at once, relaxed loads only.
	struct ThreadCensus
	{
		unsigned long created_ok, failed, attr_failed, finished, exited, joined, live_estimate;
		unsigned long long stack_low, stack_high;
	};

	void threadCensus(ThreadCensus* out) noexcept;
}

static_assert(sizeof(orbis::ThreadCensus) == 72, "orbis::ThreadCensus layout must match orbis-compat");
#endif

#ifdef RPCS3_PS4
// 0.39: libkernel's firmware version query, weak under a private name as in ps4_main.cpp (same import).
extern "C" s32 ps4_thread_sw_version(void* version) __asm__("sceKernelGetSystemSwVersion") __attribute__((weak));
#endif

const bool s_terminate_handler_set = []() -> bool
{
#ifdef RPCS3_PS4
	// PS4: records the exception type/what(), thread and backtrace, and never calls abort()
	// (abort => SIGABRT core dump => CE-34878-0). ps4_fatal_init() installs it again in main().
	std::set_terminate(ps4_terminate_handler);
#else
	std::set_terminate([]()
	{
		if (IsDebuggerPresent())
		{
			logs::listener::sync_all();
			utils::trap();
		}

		report_fatal_error("RPCS3 has abnormally terminated.");
	});
#endif

	return true;
}();

#ifdef __ORBIS__
namespace
{
	// Native priority of the main thread, captured during static initialization (which runs on the
	// main thread before any RPCS3 thread exists). 700 on the console (ORBIS_KERNEL_PRIO_FIFO_NORMAL,
	// orbis/_types/user.h). Orbis threads inherit their creator's priority,
	// so RPCS3 named threads are reset to this value when they start.
	const int s_ps4_base_priority = []() -> int
	{
		int prio = 0;

		if (scePthreadGetprio(scePthreadSelf(), &prio) != 0 || prio < 256 || prio > 767)
		{
			return 0x2BC;
		}

		return prio;
	}();

	atomic_t<bool> s_ps4_sched_logged{false};

	// 0.26: round-robin scheduling. libkernel (FreeBSD libthr)
	// numbering: 1 = SCHED_FIFO, 2 = SCHED_OTHER, 3 = SCHED_RR. The SDK <sched.h> is musl/Linux numbering
	// (its SCHED_RR 2 is FreeBSD's SCHED_OTHER), so the SDK macros are never passed to libkernel.
	// Game threads run SCHED_FIFO at 700 (logged on the console): equal priorities are never time-sliced,
	// so a thread that spins or yields elsewhere can keep another runnable thread off a core for good.
	constexpr int ps4_policy_rr = 3;

	// 1: round-robin accepted on the main thread -> applied to every RPCS3 thread; -1: off; 0: not decided
	atomic_t<int> s_ps4_rr{0};
	atomic_t<u32> s_ps4_rr_failed{0};

	// Switch the calling thread to round-robin at its current priority. Accepted only if libthr reads back
	// policy 3 with the same priority; otherwise the previous policy is restored. Never throws.
	bool ps4_set_rr_self(int* old_policy = nullptr, int* new_policy = nullptr, int* rc_out = nullptr) noexcept
	{
		int policy = -1;
		sched_param param{};

		if (pthread_getschedparam(pthread_self(), &policy, &param) != 0 || param.sched_priority < 256 || param.sched_priority > 767)
		{
			return false;
		}

		if (old_policy) *old_policy = policy;
		if (new_policy) *new_policy = policy;

		if (policy == ps4_policy_rr)
		{
			return true;
		}

		sched_param want{};
		want.sched_priority = param.sched_priority;
		const int rc = pthread_setschedparam(pthread_self(), ps4_policy_rr, &want);
		if (rc_out) *rc_out = rc;

		int after = -1;
		sched_param after_param{};
		const int read_rc = pthread_getschedparam(pthread_self(), &after, &after_param);
		if (new_policy) *new_policy = after;

		const bool ok = rc == 0 && read_rc == 0 && after == ps4_policy_rr && after_param.sched_priority == param.sched_priority;

		if (!ok && rc == 0)
		{
			sched_param back{};
			back.sched_priority = param.sched_priority;
			pthread_setschedparam(pthread_self(), policy, &back);
		}

		return ok;
	}

	void ps4_reset_inherited_priority() noexcept
	{
		const int base = s_ps4_base_priority;

		if (base < 256)
		{
			// Static initialization not done yet (thread started from another static initializer)
			return;
		}

		const auto self = scePthreadSelf();
		int current = 0;

		if (scePthreadGetprio(self, &current) == 0 && current != base)
		{
			// E.g. a thread created by the raised RSX/audio thread (692 -> 684 chains in the 0.24 log),
			// or by a compile worker. Fixed-priority scheduler: keep every RPCS3 thread at the base.
			const int error = scePthreadSetprio(self, base);
			sig_log.notice("PS4 native priority: inherited %d reset to base %d (error=0x%x)", current, base, static_cast<u32>(error));
		}

		if (!s_ps4_sched_logged.exchange(true))
		{
			// Diagnostic only (once per process). libkernel's pthread_getschedparam reports the native
			// policy in FreeBSD numbering (1=FIFO, 2=OTHER, 3=RR), while the SDK <sched.h> uses Linux
			// numbering (SCHED_RR=2). Because the round-robin constant is ambiguous, RPCS3 does not
			// change the scheduling policy; this line documents what the console actually uses.
			int policy = -1;
			sched_param param{};
			const int error = pthread_getschedparam(pthread_self(), &policy, &param);
			ps4_startup_checkpoint(fmt::format("PS4 thread scheduling: raw policy=%d sched_priority=%d native prio=%d base=%d (error=%d); round-robin state=%d",
				policy, param.sched_priority, current, base, error, s_ps4_rr.load()).c_str());
		}

		// Threads normally inherit round-robin from the main thread (PTHREAD_INHERIT_SCHED); threads made
		// by foreign code with explicit attributes are switched here.
		if (s_ps4_rr == 1 && !ps4_set_rr_self())
		{
			if (s_ps4_rr_failed++ < 4)
			{
				sig_log.warning("PS4 thread scheduling: round-robin refused for this thread; it keeps its policy");
			}
		}
	}
}

// Process base native priority (700 on the console); used by PS4AudioBackend to boost exactly one step.
int ps4_thread_base_native_priority() noexcept
{
	return s_ps4_base_priority;
}

#ifdef RPCS3_PS4
// 0.33: shared_mutex holds are recorded from process start (Utilities/mutex.h); /data/rpcs3/no-lock-release.flag
// turns off the record (lock/unlock then only test a flag and an empty record) and the release of locks left by
// dead threads.
static void ps4_lock_release_configure() noexcept
{
	if (fs::is_file("/data/rpcs3/no-lock-release.flag"))
	{
		g_ps4_lock_record.store(false);
		ps4_startup_checkpoint("PS4 locks: held-lock record and release OFF (no-lock-release.flag)");
		return;
	}

	ps4_startup_checkpoint("PS4 locks: held-lock record ON; locks left by a dead thread are released only while the emulator stops (off: /data/rpcs3/no-lock-release.flag)");
}
#endif

// Called first thing in main() (before any RPCS3 thread, so std::thread/Mesa children inherit the policy).
// Opt-out: /data/rpcs3/no-rr.flag. Returns a one-line description for startup.log.
std::string ps4_thread_apply_round_robin() noexcept
{
#ifdef RPCS3_PS4
	// Same point of main for the lock-record switch: no RPCS3 thread exists yet
	ps4_lock_release_configure();
#endif

	if (fs::is_file("/data/rpcs3/no-rr.flag"))
	{
		s_ps4_rr = -1;
		return "PS4 thread scheduling: round-robin disabled by no-rr.flag (SCHED_FIFO kept)";
	}

	int old_policy = -1, new_policy = -1, rc = -1;
	const bool ok = ps4_set_rr_self(&old_policy, &new_policy, &rc);
	s_ps4_rr = ok ? 1 : -1;
	return fmt::format("PS4 thread scheduling: round-robin %s (policy %d -> %d, setschedparam rc=%d)",
		ok ? "enabled for all RPCS3 threads" : "REFUSED by the kernel; FIFO kept", old_policy, new_policy, rc);
}
#endif

#ifdef RPCS3_PS4
#ifndef __ORBIS__
#error "The 0.29 PS4 thread runtime needs the console build (__ORBIS__ together with RPCS3_PS4, ps4-port/toolchain.cmake)"
#endif

namespace
{
	namespace reg = ps4_thread_registry;

	// ------------------------------------------------------------------------------------------------
	// 0.29 live-thread registry (Thread.h ps4_thread_registry). One slot per
	// running RPCS3 thread: entered in thread_base::initialize, left at the top of thread_base::finalize(u64).
	// Constant-initialized (usable by threads started before main), lock-free and allocation-free, so the
	// sampling profiler's signal handler may read its own slot.
	struct alignas(64) ps4_live_slot
	{
		std::atomic<u32> state{reg::slot_free};
		std::atomic<u32> pins{0};
		std::atomic<u32> gen{0};
		std::atomic<u32> name_seq{0}; // odd while the owner rewrites the name

		// Written by the owner while the slot is 'claim'; read by others only under a pin
		u64 native = 0;
		u64 stack_lo = 0;
		u64 stack_hi = 0;
		u64 enter_us = 0;
		s32 tid = 0;
		u32 stack_slot = umax;

		// Rewritten by the owner on rename (seqlock name_seq)
		std::atomic<u64> name[reg::name_size / sizeof(u64)]{};
	};

	static_assert(reg::name_size % sizeof(u64) == 0);

	constinit ps4_live_slot s_ps4_live[reg::slot_count]{};
	constinit thread_local int t_ps4_live_slot = -1;
	constinit thread_local bool t_ps4_running = false;

	constinit std::atomic<u32> s_ps4_live_count{0};
	constinit std::atomic<u32> s_ps4_live_peak{0};
	constinit std::atomic<u32> s_ps4_live_hint{0};
	constinit std::atomic<u64> s_ps4_live_entered{0};
	constinit std::atomic<u64> s_ps4_live_full{0};
	constinit std::atomic<u64> s_ps4_live_leaked{0};
	constinit std::atomic<reg::hook> s_ps4_hook_enter{nullptr};
	constinit std::atomic<reg::hook> s_ps4_hook_leave{nullptr};

	// thread_base bookkeeping: native creates that succeeded, threads between initialize() and finalize(u64),
	// joins in ~thread_base. started - joined - running = threads that have exited but were not joined yet,
	// i.e. whose libkernel stack is still held (finished fxo threads are joined only at Emu stop).
	constinit std::atomic<u64> s_ps4_started{0};
	constinit std::atomic<u32> s_ps4_running{0};
	constinit std::atomic<u64> s_ps4_joined{0};
	constinit std::atomic<u64> s_ps4_join_failed{0};

	void ps4_live_store_name(ps4_live_slot& s, const char* name) noexcept
	{
		char text[reg::name_size]{};

		for (usz i = 0; name && name[i] && i + 1 < reg::name_size; i++)
		{
			text[i] = name[i];
		}

		u64 words[reg::name_size / sizeof(u64)];
		std::memcpy(words, text, sizeof(words));

		const u32 seq = s.name_seq.load(std::memory_order_relaxed);
		s.name_seq.store(seq + 1, std::memory_order_relaxed);
		std::atomic_thread_fence(std::memory_order_release);

		for (usz i = 0; i < std::size(words); i++)
		{
			s.name[i].store(words[i], std::memory_order_relaxed);
		}

		s.name_seq.store(seq + 2, std::memory_order_release);
	}

	void ps4_live_load_name(const ps4_live_slot& s, char (&out)[reg::name_size]) noexcept
	{
		u64 words[reg::name_size / sizeof(u64)]{};

		// Bounded: a reader interrupting the owner's own rename (signal handler) keeps the last copy
		for (u32 attempt = 0; attempt < 16; attempt++)
		{
			const u32 seq = s.name_seq.load(std::memory_order_acquire);

			for (usz i = 0; i < std::size(words); i++)
			{
				words[i] = s.name[i].load(std::memory_order_relaxed);
			}

			std::atomic_thread_fence(std::memory_order_acquire);

			if (!(seq & 1) && s.name_seq.load(std::memory_order_relaxed) == seq)
			{
				break;
			}
		}

		std::memcpy(out, words, sizeof(out));
		out[reg::name_size - 1] = 0;
	}

	// Name grouping for the live summary and the stack high-water table:
	// 'PPU[0x1000022] Thread (_libsail)' -> 'PPU[..]', 'RSX Worker 3' -> 'RSX Worker #', 'SPUW.1' -> 'SPUW.#'
	void ps4_thread_group_key(const char* name, char (&key)[reg::name_size]) noexcept
	{
		usz n = 0;

		while (name && name[n] && n + 1 < reg::name_size)
		{
			n++;
		}

		if (!n)
		{
			std::memcpy(key, "?", 2);
			return;
		}

		for (usz i = 1; i < n && i < 12; i++)
		{
			if (name[i] == '[')
			{
				std::memcpy(key, name, i);
				std::memcpy(key + i, "[..]", 5);
				return;
			}
		}

		std::memcpy(key, name, n);
		key[n] = 0;

		usz end = n;

		while (end && key[end - 1] >= '0' && key[end - 1] <= '9')
		{
			end--;
		}

		if (end < n)
		{
			key[end] = '#';
			key[end + 1] = 0;
		}
	}

	// Upper bound for stack scans on a libkernel stack: orbis-compat's trampoline carves the 64 KiB alternate
	// signal stack out of the thread's own stack above every frame of the thread body. Without one, a point just
	// above the registering frame: everything between a later rsp and that point is this thread's mapped stack
	// (a scan from an rsp at or above it finds nothing). sigaltstack is orbis-compat's FreeBSD-layout shim.
	u64 ps4_alt_stack_top() noexcept
	{
		const u64 here = reinterpret_cast<u64>(__builtin_frame_address(0));
		stack_t ss{};

		if (sigaltstack(nullptr, &ss) == 0 && !(ss.ss_flags & SS_DISABLE) && ss.ss_sp && ss.ss_size)
		{
			const u64 lo = reinterpret_cast<u64>(ss.ss_sp);

			if (lo > here && lo - here < 0x800000)
			{
				return lo + ss.ss_size;
			}
		}

		return here + 256;
	}

	int ps4_live_enter(const char* name, u64 stack_lo, u64 stack_hi, u32 stack_slot) noexcept
	{
		if (const int self = t_ps4_live_slot; self >= 0)
		{
			ps4_live_store_name(s_ps4_live[self], name);
			return self;
		}

		const u32 start = s_ps4_live_hint.load(std::memory_order_relaxed);

		for (u32 k = 0; k < reg::slot_count; k++)
		{
			const u32 index = (start + k) % reg::slot_count;
			ps4_live_slot& s = s_ps4_live[index];
			u32 expected = reg::slot_free;

			if (s.state.load(std::memory_order_relaxed) != reg::slot_free || !s.state.compare_exchange_strong(expected, reg::slot_claim, std::memory_order_acq_rel))
			{
				continue;
			}

			s.native = static_cast<u64>(pthread_self());
			s.tid = scePthreadGetthreadid();
			s.enter_us = sceKernelGetProcessTime();
			s.stack_lo = stack_lo;
			s.stack_hi = stack_hi;
			s.stack_slot = stack_slot;
			ps4_live_store_name(s, name);
			s.gen.fetch_add(1, std::memory_order_relaxed);
			s.state.store(reg::slot_live, std::memory_order_seq_cst);

			t_ps4_live_slot = static_cast<int>(index);
			s_ps4_live_hint.store((index + 1) % reg::slot_count, std::memory_order_relaxed);
			s_ps4_live_entered.fetch_add(1, std::memory_order_relaxed);

			const u32 live = s_ps4_live_count.fetch_add(1, std::memory_order_relaxed) + 1;
			u32 peak = s_ps4_live_peak.load(std::memory_order_relaxed);

			while (live > peak && !s_ps4_live_peak.compare_exchange_weak(peak, live, std::memory_order_relaxed))
			{
			}

			if (const reg::hook on_enter = s_ps4_hook_enter.load(std::memory_order_acquire))
			{
				on_enter(index);
			}

			return static_cast<int>(index);
		}

		s_ps4_live_full.fetch_add(1, std::memory_order_relaxed);
		return -1;
	}

	void ps4_live_leave() noexcept
	{
		const int index = t_ps4_live_slot;

		if (index < 0)
		{
			return;
		}

		// From here a (profiler) signal on this thread finds no slot
		t_ps4_live_slot = -1;
		std::atomic_signal_fence(std::memory_order_seq_cst);

		ps4_live_slot& s = s_ps4_live[index];
		s.state.store(reg::slot_leave, std::memory_order_seq_cst);

		u64 deadline = 0;

		for (u32 spins = 0; s.pins.load(std::memory_order_seq_cst) != 0; spins++)
		{
			if (spins < 64)
			{
				utils::pause();
				continue;
			}

			// Pins last microseconds. Never hang an exiting thread: give up after 1 s and leak the slot. Bounded by
			// time, not by a count: a short kernel sleep may last ~1 ms on this console.
			const u64 now = sceKernelGetProcessTime();

			if (!deadline)
			{
				deadline = now + 1'000'000;
			}
			else if (now >= deadline)
			{
				s_ps4_live_leaked.fetch_add(1, std::memory_order_relaxed);
				s_ps4_live_count.fetch_sub(1, std::memory_order_relaxed);
				return;
			}

			sceKernelUsleep(50);
		}

		if (const reg::hook on_leave = s_ps4_hook_leave.load(std::memory_order_acquire))
		{
			on_leave(static_cast<u32>(index));
		}

		s_ps4_live_count.fetch_sub(1, std::memory_order_relaxed);
		s.state.store(reg::slot_free, std::memory_order_release);
	}

	// ------------------------------------------------------------------------------------------------
	// 0.29 RPCS3-owned thread stacks. 2 MiB of direct memory per
	// stack at a fixed slot of a private VA window, with the 2 MiB below each stack left unmapped as its guard.
	// Used as a rescue when libkernel refuses a thread (Demon's Souls 0.27/0.28: 0x80020023 at ~85 threads,
	// each libkernel stack costing 2 MiB of the flexible pool), own stack first once a rescue worked, and for
	// every thread_base thread under /data/rpcs3/own-stacks.flag. A stack goes back to the pool after the
	// pthread_join in ~thread_base and is re-zeroed, which makes its high-water mark measurable. Stack overflows
	// are still not reported (no SA_ONSTACK on SIGSEGV), so stacks stay exactly 2 MiB, as libkernel's.
	constexpr usz c_ps4_stack_size = 0x200000;  // the orbis-compat floor every thread gets today
	constexpr usz c_ps4_slot_stride = 0x400000; // [guard 2 MiB, never mapped][stack 2 MiB]
	constexpr u32 c_ps4_max_slots = 1024;       // 4 GiB of VA, reserved slot by slot on demand
	constexpr u32 c_ps4_max_cached = 8;         // committed, all-zero stacks kept for the next thread (16 MiB)
	// 0.29: at most 256 MiB of direct memory in stacks (direct memory also backs guest and texture memory, ~850 MiB
	// free in Demon's Souls, estimated); beyond, libkernel's. Demon's Souls needs ~85-90 with own-stacks.flag.
	// 0.38: the process memory budget reserves 64 MiB of direct memory for these stacks: 32 live in rescue mode
	// (beyond, libkernel's); own-stacks.flag keeps 128 (every thread_base thread on a pool stack by design).
	constexpr u32 c_ps4_max_live = 128;
	constexpr u32 c_ps4_max_live_rescue = 32;
	constexpr u32 c_ps4_max_roles = 48;         // high-water table entries
	// [0x1900000000, 0x2000000000) is free: guest reservations end at 0x1800000000+4G, the JIT starts at 0x2000000000
	constexpr std::uintptr_t c_ps4_windows[] = {0x1C00000000ull, 0x4000000000ull};

	enum : u32
	{
		ps4_pool_uninit = 0,
		ps4_pool_ready = 1,
		ps4_pool_off = 2,
	};

	struct ps4_stack_role
	{
		char key[reg::name_size];
		u32 depth_kib;
		u32 count;
	};

	struct ps4_stack_pool
	{
		std::atomic<u32> lock{0};

		// Set once by ps4_thread_stacks_init before the state becomes ready
		std::uintptr_t window = 0;

		// Guarded by lock (bookkeeping only: maps, scans and syscalls run outside it)
		u32 next = 0;
		u32 cached[c_ps4_max_cached]{};
		u32 cached_count = 0;
		u16 unmapped[c_ps4_max_slots]{};
		u32 unmapped_count = 0;
		u32 live = 0;
		u32 peak = 0;
		u32 retired = 0;
		u64 committed = 0;
		u64 recycled = 0;
		u64 released = 0;
		u64 failures = 0;
		int last_errno = 0;
		usz deepest = 0;
		char deepest_name[reg::name_size]{};
		ps4_stack_role roles[c_ps4_max_roles]{};
		u32 role_count = 0;
		char why[200]{};
	};

	constinit ps4_stack_pool s_ps4_pool{};
	constinit std::atomic<u32> s_ps4_pool_state{ps4_pool_uninit};
	constinit std::atomic<bool> s_ps4_own_always{false}; // own-stacks.flag
	constinit std::atomic<bool> s_ps4_own_first{false};  // own stack before libkernel (flag, or sticky after a rescue)
	// 0.38: direct memory committed for pool stacks (live, cached, retired), for the process memory budget. These
	// commits are also part of utils::ps4::memory's private bytes.
	constinit std::atomic<u64> s_ps4_stack_bytes{0};

	u32 ps4_stack_max_live() noexcept
	{
		return s_ps4_own_always.load(std::memory_order_relaxed) ? c_ps4_max_live : c_ps4_max_live_rescue;
	}

	// Create-helper counters and once-only log latches
	constinit std::atomic<u64> s_ps4_create_failures{0};
	constinit std::atomic<u64> s_ps4_rescued{0};
	constinit std::atomic<u64> s_ps4_rescue_refused{0};
	constinit std::atomic<u64> s_ps4_rescue_no_stack{0};
	constinit std::atomic<u64> s_ps4_own_first_refused{0};
	constinit std::atomic<bool> s_ps4_logged_rescued{false};
	constinit std::atomic<bool> s_ps4_logged_refused{false};
	constinit std::atomic<bool> s_ps4_logged_no_stack{false};
	constinit std::atomic<bool> s_ps4_logged_attr{false};
	constinit std::atomic<bool> s_ps4_logged_join{false};

	struct ps4_pool_guard
	{
		ps4_pool_guard() noexcept
		{
			for (u32 i = 0; s_ps4_pool.lock.exchange(1, std::memory_order_acquire); i++)
			{
				// No pthread mutex here: this runs exactly when libkernel's thread resources are short
				if (i < 64)
				{
					utils::pause();
				}
				else
				{
					sceKernelUsleep(50);
				}
			}
		}

		~ps4_pool_guard()
		{
			s_ps4_pool.lock.store(0, std::memory_order_release);
		}

		ps4_pool_guard(const ps4_pool_guard&) = delete;
		ps4_pool_guard& operator=(const ps4_pool_guard&) = delete;
	};

	bool ps4_pool_ready_now() noexcept
	{
		return s_ps4_pool_state.load(std::memory_order_acquire) == ps4_pool_ready;
	}

	void ps4_pool_set_why(std::string_view why) noexcept
	{
		ps4_pool_guard lock;
		const usz n = std::min<usz>(why.size(), sizeof(s_ps4_pool.why) - 1);
		std::memcpy(s_ps4_pool.why, why.data(), n);
		s_ps4_pool.why[n] = 0;
	}

	u8* ps4_stack_at(std::uintptr_t window, u32 slot) noexcept
	{
		return reinterpret_cast<u8*>(window + usz{slot} * c_ps4_slot_stride + (c_ps4_slot_stride - c_ps4_stack_size));
	}

	u8* ps4_stack_base(u32 slot) noexcept
	{
		return ps4_stack_at(s_ps4_pool.window, slot);
	}

	// A committed, all-zero 2 MiB stack, or umax (no window, slots exhausted, VA taken, no direct memory).
	u32 ps4_stack_acquire() noexcept
	{
		auto& p = s_ps4_pool;

		if (!p.window)
		{
			return umax;
		}

		for (u32 tries = 0; tries < 4; tries++)
		{
			u32 slot = umax;
			{
				ps4_pool_guard lock;

				if (p.cached_count)
				{
					slot = p.cached[--p.cached_count];
					p.recycled++;
					p.peak = std::max(p.peak, ++p.live);
					return slot;
				}

				if (p.live >= ps4_stack_max_live())
				{
					p.failures++;
					p.last_errno = ENOSPC;
					return umax;
				}

				if (p.unmapped_count)
				{
					slot = p.unmapped[--p.unmapped_count];
				}
				else if (p.next < c_ps4_max_slots)
				{
					slot = p.next++;
				}
				else
				{
					p.failures++;
					p.last_errno = ENOSPC;
					return umax;
				}
			}

			// Map outside the lock: the slot is exclusively ours now
			u8* const stack = ps4_stack_at(p.window, slot);

			if (!utils::ps4::memory::reserve(c_ps4_stack_size, stack))
			{
				// A foreign mapping took this VA (NO_OVERWRITE refused): retire the slot, try another one
				const int error = errno;
				ps4_pool_guard lock;
				p.failures++;
				p.retired++;
				p.last_errno = error;
				continue;
			}

			// Zero-filled WB_ONION direct memory, CPU read/write (the memory layer memsets it)
			if (!utils::ps4::memory::commit(stack, c_ps4_stack_size, 3))
			{
				const int error = errno;
				utils::ps4::memory::release(stack, c_ps4_stack_size);
				ps4_pool_guard lock;
				p.failures++;
				p.last_errno = error;
				p.unmapped[p.unmapped_count++] = static_cast<u16>(slot);
				return umax;
			}

			s_ps4_stack_bytes.fetch_add(c_ps4_stack_size, std::memory_order_relaxed);
			ps4_pool_guard lock;
			p.committed++;
			p.peak = std::max(p.peak, ++p.live);
			return slot;
		}

		return umax;
	}

	// Record how deep a joined thread went; true = new overall deepest. Called with the pool lock held.
	bool ps4_note_depth_locked(const char* name, usz used) noexcept
	{
		auto& p = s_ps4_pool;
		char key[reg::name_size];
		ps4_thread_group_key(name, key);

		ps4_stack_role* role = nullptr;

		for (u32 i = 0; i < p.role_count; i++)
		{
			if (std::strcmp(p.roles[i].key, key) == 0)
			{
				role = &p.roles[i];
				break;
			}
		}

		if (!role && p.role_count < c_ps4_max_roles)
		{
			role = &p.roles[p.role_count++];
			std::memcpy(role->key, key, sizeof(key));
		}

		if (role)
		{
			role->count++;
			role->depth_kib = std::max<u32>(role->depth_kib, static_cast<u32>(used / 1024));
		}

		if (used <= p.deepest)
		{
			return false;
		}

		p.deepest = used;
		std::memcpy(p.deepest_name, key, sizeof(key));
		return true;
	}

	// Back to the pool after a successful pthread_join, or after a create that never ran (name == nullptr: not
	// recorded). Free stacks are all zero (commit() zero-fills, this re-zeroes), so the lowest 16 KiB page with
	// a non-zero word bounds how deep the thread went (only pool stacks are measured; reused libkernel
	// stacks are dirty). Returns that depth in bytes.
	usz ps4_stack_release(u32 slot, const char* name) noexcept
	{
		auto& p = s_ps4_pool;
		u8* const stack = ps4_stack_at(p.window, slot);
		usz used = 0;

		for (usz off = 0; off < c_ps4_stack_size && !used; off += 0x4000)
		{
			const u64* const page = reinterpret_cast<const u64*>(stack + off);

			for (usz i = 0; i < 0x4000 / sizeof(u64); i++)
			{
				if (page[i])
				{
					used = c_ps4_stack_size - off;
					break;
				}
			}
		}

		std::memset(stack + (c_ps4_stack_size - used), 0, used);

		bool keep = false;
		bool deepest = false;
		{
			ps4_pool_guard lock;
			p.live--;

			if (name)
			{
				deepest = ps4_note_depth_locked(name, used);
			}

			if (p.cached_count < c_ps4_max_cached)
			{
				p.cached[p.cached_count++] = slot;
				keep = true;
			}
		}

		if (deepest)
		{
			sig_log.notice("PS4 stacks: new deepest use %u KiB of %u KiB by '%s'", used / 1024, c_ps4_stack_size / 1024, name);
		}

		if (!keep)
		{
			if (utils::ps4::memory::release(stack, c_ps4_stack_size))
			{
				s_ps4_stack_bytes.fetch_sub(c_ps4_stack_size, std::memory_order_relaxed);
				ps4_pool_guard lock;
				p.released++;
				p.unmapped[p.unmapped_count++] = static_cast<u16>(slot);
			}
			else
			{
				// The slot stays committed and out of circulation
				const int error = errno;
				ps4_pool_guard lock;
				p.failures++;
				p.retired++;
				p.last_errno = error;
			}
		}

		return used;
	}

	// Unmaps every cached stack (pool turned off after its self-test: no direct memory held for nothing)
	void ps4_stack_flush_cache() noexcept
	{
		auto& p = s_ps4_pool;

		while (true)
		{
			u32 slot = umax;
			{
				ps4_pool_guard lock;

				if (!p.cached_count)
				{
					return;
				}

				slot = p.cached[--p.cached_count];
			}

			const bool ok = utils::ps4::memory::release(ps4_stack_at(p.window, slot), c_ps4_stack_size);
			const int error = errno;
			ps4_pool_guard lock;

			if (ok)
			{
				s_ps4_stack_bytes.fetch_sub(c_ps4_stack_size, std::memory_order_relaxed);
				p.released++;
				p.unmapped[p.unmapped_count++] = static_cast<u16>(slot);
			}
			else
			{
				p.failures++;
				p.retired++;
				p.last_errno = error;
			}
		}
	}

	int ps4_pool_last_errno() noexcept
	{
		ps4_pool_guard lock;
		return s_ps4_pool.last_errno;
	}

	// One create on a pool stack. Returns the create's result, or -1 with no_stack when the pool had none.
	// 0.29: stage names the call that failed. Only a refused pthread_create points to a thread cap: libkernel's attr
	// objects come from its internal allocator too, so a failing pthread_attr_init proves nothing about stacks.
	int ps4_create_on_own_stack(pthread_t& thread_id, thread_base::native_entry entry, void* arg, u32& stack_slot, bool& no_stack, const char*& stage) noexcept
	{
		no_stack = false;
		stage = "stack";
		const u32 slot = ps4_stack_acquire();

		if (slot == umax)
		{
			no_stack = true;
			return -1;
		}

		// A 2 MiB caller stack is at the orbis-compat floor, so its interposer passes this attr unchanged
		pthread_attr_t attr;
		stage = "pthread_attr_init";
		int rc = pthread_attr_init(&attr);

		if (rc == 0)
		{
			stage = "pthread_attr_setstack";
			rc = pthread_attr_setstack(&attr, ps4_stack_base(slot), c_ps4_stack_size);

			if (rc == 0)
			{
				// Published before the create: the new thread records its stack bounds from it (registry)
				stack_slot = slot;
				stage = "pthread_create";
				rc = pthread_create(&thread_id, &attr, entry, arg);

				if (rc != 0)
				{
					stack_slot = umax;
				}
			}

			pthread_attr_destroy(&attr);
		}

		if (rc != 0)
		{
			// Never ran: straight back to the pool
			ps4_stack_release(slot, nullptr);
		}

		return rc;
	}

	std::string ps4_thread_create_diag(int create_error)
	{
		unsigned long created = 0, finished = 0;
		ps4_thread_census(&created, &finished);
		return fmt::format("error 0x%x; kernel internal memory free %llu bytes; flexible memory free %llu bytes; orbis threads: ~%u live, %u created, %u returned",
			static_cast<u32>(create_error), ps4_internal_memory_free(), ps4_flexible_memory_free(), orbis::threadsLive(), created, finished);
	}

	// 0.29: the one native-create path of thread_base::start() and try_start_native(). libkernel first;
	// when it refuses, one rescue create on an RPCS3-owned stack, whose result tells a stack allocation limit
	// from a thread cap. After the first rescue, own stack first. No retry and no sleep here: start() keeps its 0.28 retry for must-succeed threads.
	int ps4_create_native(pthread_t& thread_id, thread_base::native_entry entry, void* arg, u32& stack_slot, const std::string& name) noexcept
	{
		// A restarted object still owns the stack of its previous run until ~thread_base: libkernel only
		const bool pool = ps4_pool_ready_now() && stack_slot == umax;
		bool tried_own = false;
		bool no_stack = false;
		const char* stage = "";

		if (pool && s_ps4_own_first.load(std::memory_order_relaxed))
		{
			tried_own = true;
			const int own_error = ps4_create_on_own_stack(thread_id, entry, arg, stack_slot, no_stack, stage);

			if (own_error == 0)
			{
				return 0;
			}

			if (const u64 n = s_ps4_own_first_refused.fetch_add(1, std::memory_order_relaxed) + 1; !(n & (n - 1)))
			{
				sig_log.warning("PS4 stacks: own stack %s for '%s' (%s rc=0x%x, #%u); trying libkernel's", no_stack ? "unavailable" : "refused", name, stage, static_cast<u32>(own_error), n);
			}
		}

		const int create_error = pthread_create(&thread_id, nullptr, entry, arg);

		if (create_error == 0)
		{
			return 0;
		}

		if (const u64 failures = s_ps4_create_failures.fetch_add(1, std::memory_order_relaxed) + 1; failures == 1)
		{
			// Who is alive at the first refusal, and the pool state
			const std::string line = fmt::format("PS4 threads: FIRST pthread_create refusal, thread '%s' (%s). %s. %s", name,
				ps4_thread_create_diag(create_error), ps4_thread_live_summary(), ps4_thread_stacks_report());
			sig_log.error("%s", line);
			ps4_startup_checkpoint(line.c_str());
		}
		else if (!(failures & (failures - 1)))
		{
			sig_log.error("PS4 threads: pthread_create refusal #%u, thread '%s' (%s)", failures, name, ps4_thread_create_diag(create_error));
		}

		if (!pool || tried_own)
		{
			return create_error;
		}

		const int own_error = ps4_create_on_own_stack(thread_id, entry, arg, stack_slot, no_stack, stage);

		if (own_error == 0)
		{
			const u64 rescued = s_ps4_rescued.fetch_add(1, std::memory_order_relaxed) + 1;

			if (!s_ps4_logged_rescued.exchange(true))
			{
				const std::string line = fmt::format("PS4 stacks: VERDICT thread '%s' refused by libkernel (error 0x%x) was rescued by own stack"
					" (RPCS3-owned 2 MiB direct-memory stack, slot %u at 0x%x): libkernel's stack allocation is the limit, not a thread cap",
					name, static_cast<u32>(create_error), stack_slot, reinterpret_cast<u64>(ps4_stack_base(stack_slot)));
				sig_log.error("%s", line);
				ps4_startup_checkpoint(line.c_str());
			}

			if (!s_ps4_own_first.exchange(true))
			{
				const std::string line = fmt::format("PS4 stacks: own stack first from now on (sticky after rescue #%u); libkernel's stacks only when the pool has none", rescued);
				sig_log.warning("%s", line);
				ps4_startup_checkpoint(line.c_str());
			}

			return 0;
		}

		if (no_stack)
		{
			s_ps4_rescue_no_stack.fetch_add(1, std::memory_order_relaxed);

			if (!s_ps4_logged_no_stack.exchange(true))
			{
				const std::string line = fmt::format("PS4 stacks: rescue of '%s' impossible: no own stack could be mapped (pool errno %d); %s",
					name, ps4_pool_last_errno(), ps4_thread_stacks_report());
				sig_log.error("%s", line);
				ps4_startup_checkpoint(line.c_str());
			}
		}
		else if (std::strcmp(stage, "pthread_create") != 0)
		{
			// 0.29: the rescue never reached its create, so it tells nothing about stack limit vs thread cap (that latch stays open)
			s_ps4_rescue_no_stack.fetch_add(1, std::memory_order_relaxed);

			if (!s_ps4_logged_attr.exchange(true))
			{
				const std::string line = fmt::format("PS4 stacks: rescue of '%s' not attempted: %s failed rc=0x%x before the create (libkernel error 0x%x); no verdict",
					name, stage, static_cast<u32>(own_error), static_cast<u32>(create_error));
				sig_log.error("%s", line);
				ps4_startup_checkpoint(line.c_str());
			}
		}
		else
		{
			s_ps4_rescue_refused.fetch_add(1, std::memory_order_relaxed);

			if (!s_ps4_logged_refused.exchange(true))
			{
				const std::string line = fmt::format("PS4 stacks: VERDICT own stack also refused rc=0x%x for '%s' (libkernel error 0x%x):"
					" the limit is not the stack allocation (thread cap or another kernel resource)", static_cast<u32>(own_error), name, static_cast<u32>(create_error));
				sig_log.error("%s", line);
				ps4_startup_checkpoint(line.c_str());
			}
		}

		return create_error;
	}

	// thread_base::initialize: enter the registry with this thread's stack bounds
	void ps4_thread_enter_registry(const char* name, u32 stack_slot) noexcept
	{
		if (!t_ps4_running)
		{
			t_ps4_running = true;
			s_ps4_running.fetch_add(1, std::memory_order_relaxed);
		}

		u64 lo = 0;
		u64 hi = 0;

		if (stack_slot != umax)
		{
			lo = reinterpret_cast<u64>(ps4_stack_base(stack_slot));
			hi = lo + c_ps4_stack_size;
		}
		else
		{
			hi = ps4_alt_stack_top();
		}

		ps4_live_enter(name, lo, hi, stack_slot);
	}

	// 0.39: names of thread_base threads that exited and were not joined yet (census line). Keyed by the native
	// handle ~thread_base joins; diagnostic only (a full table drops names, the counters stay exact).
	struct ps4_unjoined_slot
	{
		std::atomic<u64> key{0}; // 0 free, 1 being written, else the native handle
		std::atomic<u64> name[reg::name_size / sizeof(u64)]{};
	};

	constinit ps4_unjoined_slot s_ps4_unjoined[32]{};

	void ps4_unjoined_add(u64 key, const char (&name)[reg::name_size]) noexcept
	{
		if (key <= 1)
		{
			return;
		}

		for (ps4_unjoined_slot& s : s_ps4_unjoined)
		{
			u64 expected = 0;

			if (!s.key.compare_exchange_strong(expected, 1, std::memory_order_acquire))
			{
				continue;
			}

			u64 words[reg::name_size / sizeof(u64)]{};
			std::memcpy(words, name, sizeof(words));

			for (usz i = 0; i < std::size(words); i++)
			{
				s.name[i].store(words[i], std::memory_order_relaxed);
			}

			s.key.store(key, std::memory_order_release);
			return;
		}
	}

	void ps4_unjoined_remove(u64 key) noexcept
	{
		for (ps4_unjoined_slot& s : s_ps4_unjoined)
		{
			u64 expected = key;

			if (key > 1 && s.key.compare_exchange_strong(expected, 0, std::memory_order_acq_rel))
			{
				return;
			}
		}
	}

	// thread_base::finalize(u64): leave the registry before pthread_exit
	void ps4_thread_exit_registry() noexcept
	{
		// 0.39: remember the name until ~thread_base joins this thread
		if (const int index = t_ps4_live_slot; index >= 0)
		{
			char name[reg::name_size];
			ps4_live_load_name(s_ps4_live[index], name);
			ps4_unjoined_add(static_cast<u64>(pthread_self()), name);
		}

		ps4_live_leave();

		if (t_ps4_running)
		{
			t_ps4_running = false;
			s_ps4_running.fetch_sub(1, std::memory_order_relaxed);
		}
	}

	// ~thread_base after pthread_join: count it and recycle an own stack (records its high-water mark)
	void ps4_thread_joined(int join_rc, u32& stack_slot, const std::string& name) noexcept
	{
		if (join_rc != 0)
		{
			s_ps4_join_failed.fetch_add(1, std::memory_order_relaxed);

			if (!s_ps4_logged_join.exchange(true))
			{
				sig_log.error("PS4: pthread_join of '%s' failed (rc=0x%x)%s", name, static_cast<u32>(join_rc),
					stack_slot != umax ? "; its own stack stays out of the pool" : "");
			}

			return;
		}

		s_ps4_joined.fetch_add(1, std::memory_order_relaxed);

		if (stack_slot != umax)
		{
			// Joined: the thread no longer runs on its stack
			ps4_stack_release(std::exchange(stack_slot, umax), name.c_str());
		}
	}

	struct ps4_selftest_probe
	{
		void* base = nullptr;
		usz size = 0;
		int attr_rc = -1;
		u64 local = 0;
		bool handler_ours = false;
		int kill_rc = -1;
		u32 sig_before = 0;
		u32 sig_after = 0;
	};

	[[gnu::noinline]] void ps4_stack_selftest_frames() noexcept
	{
		// 256 KiB of frames, touched page by page from the deepest one: the high-water scan must see them
		volatile u8 frames[256 * 1024];

		for (usz i = 0; i < sizeof(frames); i += 4096)
		{
			frames[i] = 1;
		}

		// 0.36: the array must escape. Otherwise the optimizer unrolls the loop and splits the array into 64 one-byte
		// slots 16 bytes apart (1 KiB of stack), and the self-test measured 80 KiB ("telemetry INVALID").
		asm volatile("" : : "r"(frames) : "memory");
	}

	void* ps4_stack_selftest_body(void* arg) noexcept
	{
		auto& r = *static_cast<ps4_selftest_probe*>(arg);
		volatile u8 marker = 1;
		r.local = reinterpret_cast<u64>(&marker);

		pthread_attr_t self;
		r.attr_rc = pthread_attr_init(&self);

		if (r.attr_rc == 0)
		{
			r.attr_rc = pthread_attr_get_np(pthread_self(), &self);

			if (r.attr_rc == 0)
			{
				r.attr_rc = pthread_attr_getstack(&self, &r.base, &r.size);
			}

			pthread_attr_destroy(&self);
		}

		// A kernel round trip on this stack
		sceKernelUsleep(1000);

		// A signal frame on this stack (every access violation needs one), only while SIGPIPE still runs RPCS3's
		// no-op handler: its default action would end the process.
		struct ::sigaction current{};

		if (::sigaction(SIGPIPE, nullptr, &current) == 0 && current.sa_handler == sigpipe_signaling_handler)
		{
			r.handler_ours = true;
			r.sig_before = s_ps4_sigpipe_seen.load();
			r.kill_rc = pthread_kill(pthread_self(), SIGPIPE);
			sceKernelUsleep(1000); // a pending signal is delivered on the way back from this syscall at the latest
			r.sig_after = s_ps4_sigpipe_seen.load();
		}

		ps4_stack_selftest_frames();
		return nullptr;
	}
}

int ps4_thread_registry::enter(const char* name) noexcept
{
	return ps4_live_enter(name, 0, ps4_alt_stack_top(), umax);
}

void ps4_thread_registry::rename(const char* name) noexcept
{
	if (const int index = t_ps4_live_slot; index >= 0)
	{
		ps4_live_store_name(s_ps4_live[index], name);
	}
}

void ps4_thread_registry::leave() noexcept
{
	ps4_live_leave();
}

int ps4_thread_registry::self() noexcept
{
	return t_ps4_live_slot;
}

u32 ps4_thread_registry::state(u32 slot) noexcept
{
	return slot < slot_count ? s_ps4_live[slot].state.load(std::memory_order_relaxed) : u32{slot_free};
}

u32 ps4_thread_registry::generation(u32 slot) noexcept
{
	return slot < slot_count ? s_ps4_live[slot].gen.load(std::memory_order_relaxed) : 0;
}

bool ps4_thread_registry::pin(u32 slot) noexcept
{
	if (slot >= slot_count)
	{
		return false;
	}

	ps4_live_slot& s = s_ps4_live[slot];
	s.pins.fetch_add(1, std::memory_order_seq_cst);

	if (s.state.load(std::memory_order_seq_cst) == slot_live)
	{
		return true;
	}

	s.pins.fetch_sub(1, std::memory_order_release);
	return false;
}

void ps4_thread_registry::unpin(u32 slot) noexcept
{
	if (slot < slot_count)
	{
		s_ps4_live[slot].pins.fetch_sub(1, std::memory_order_release);
	}
}

bool ps4_thread_registry::read(u32 slot, slot_info& out) noexcept
{
	if (slot >= slot_count)
	{
		return false;
	}

	const ps4_live_slot& s = s_ps4_live[slot];

	if (const u32 st = s.state.load(std::memory_order_acquire); st != slot_live && st != slot_leave)
	{
		return false;
	}

	out.native = s.native;
	out.stack_lo = s.stack_lo;
	out.stack_hi = s.stack_hi;
	out.enter_us = s.enter_us;
	out.tid = s.tid;
	out.gen = s.gen.load(std::memory_order_relaxed);
	out.stack_slot = s.stack_slot;
	ps4_live_load_name(s, out.name);
	return true;
}

void ps4_thread_registry::set_hooks(hook on_enter, hook on_leave) noexcept
{
	s_ps4_hook_enter.store(on_enter, std::memory_order_release);
	s_ps4_hook_leave.store(on_leave, std::memory_order_release);
}

ps4_thread_registry::counters ps4_thread_registry::get_counters() noexcept
{
	counters result{};
	result.live = s_ps4_live_count.load(std::memory_order_relaxed);
	result.peak = s_ps4_live_peak.load(std::memory_order_relaxed);
	result.entered = s_ps4_live_entered.load(std::memory_order_relaxed);
	result.full = s_ps4_live_full.load(std::memory_order_relaxed);
	result.leaked = s_ps4_live_leaked.load(std::memory_order_relaxed);
	return result;
}

namespace
{
	constinit std::atomic<u32> s_ps4_first_refusal{0}; // 0 none, 1 being recorded, 2 recorded
	constinit ps4_thread_load s_ps4_first_refusal_load{};

	// A create the console refused (not a test refusal): the load right after the first one, for the thread governor
	void ps4_note_create_refused() noexcept
	{
		u32 expected = 0;

		if (s_ps4_first_refusal.compare_exchange_strong(expected, 1, std::memory_order_acq_rel))
		{
			s_ps4_first_refusal_load = ps4_thread_load_now();
			s_ps4_first_refusal.store(2, std::memory_order_release);
		}
	}
}

bool ps4_thread_first_refusal(ps4_thread_load& out) noexcept
{
	if (s_ps4_first_refusal.load(std::memory_order_acquire) != 2)
	{
		return false;
	}

	out = s_ps4_first_refusal_load;
	return true;
}

ps4_thread_load ps4_thread_load_now() noexcept
{
	orbis::ThreadCensus c{};
	orbis::threadCensus(&c);

	ps4_thread_load result{};
	result.live = static_cast<u32>(std::min<unsigned long>(c.live_estimate, 0xffff));
	result.unjoined = static_cast<u32>(c.exited > c.joined ? std::min<unsigned long>(c.exited - c.joined, 0xffff) : 0);
	result.total = result.live + result.unjoined;
	result.joined = c.joined;
	return result;
}

const char* ps4_firmware_key() noexcept
{
	static const struct firmware_key
	{
		char text[32]{};

		firmware_key() noexcept
		{
			if (&ps4_thread_sw_version == nullptr)
			{
				return;
			}

			// OrbisKernelSwVersion: size (8 bytes), version text (0x1c, " 13.020.001"), version number
			struct
			{
				u64 size;
				char text[0x1c];
				u32 number;
			} version{};

			version.size = sizeof(version);

			if (ps4_thread_sw_version(&version) != 0)
			{
				return;
			}

			usz pos = 0;

			while (pos < sizeof(version.text) && version.text[pos] == ' ')
			{
				pos++;
			}

			for (usz i = 0; pos < sizeof(version.text) && i + 1 < sizeof(text); pos++, i++)
			{
				const char ch = version.text[pos];

				if (ch == '\0' || ch == ' ')
				{
					break;
				}

				text[i] = ch;
			}
		}
	} s_key;

	return s_key.text;
}

std::string ps4_thread_live_summary() noexcept
{
	struct group
	{
		char key[reg::name_size];
		u32 count;
	};

	group groups[64];
	u32 group_count = 0;
	u32 other = 0;
	u32 live = 0;

	for (u32 i = 0; i < reg::slot_count; i++)
	{
		if (s_ps4_live[i].state.load(std::memory_order_relaxed) != reg::slot_live || !reg::pin(i))
		{
			continue;
		}

		char name[reg::name_size];
		ps4_live_load_name(s_ps4_live[i], name);
		reg::unpin(i);
		live++;

		char key[reg::name_size];
		ps4_thread_group_key(name, key);

		group* found = nullptr;

		for (u32 j = 0; j < group_count; j++)
		{
			if (std::strcmp(groups[j].key, key) == 0)
			{
				found = &groups[j];
				break;
			}
		}

		if (!found && group_count < std::size(groups))
		{
			found = &groups[group_count++];
			std::memcpy(found->key, key, sizeof(key));
			found->count = 0;
		}

		if (found)
		{
			found->count++;
		}
		else
		{
			other++;
		}
	}

	std::sort(groups, groups + group_count, [](const group& a, const group& b)
	{
		return a.count != b.count ? a.count > b.count : std::strcmp(a.key, b.key) < 0;
	});

	const u64 started = s_ps4_started.load(std::memory_order_relaxed);
	const u64 joined = s_ps4_joined.load(std::memory_order_relaxed);
	const u32 running = s_ps4_running.load(std::memory_order_relaxed);
	const u64 held = started > joined ? started - joined : 0;
	const u64 unjoined = held > running ? held - running : 0;

	std::string out = fmt::format("PS4 threads: %u live in the registry (peak %u); thread_base: %u running, %u exited but not joined yet (stack still held), %u started, %u joined",
		live, s_ps4_live_peak.load(std::memory_order_relaxed), running, unjoined, started, joined);

	if (const u64 failed = s_ps4_join_failed.load(std::memory_order_relaxed))
	{
		fmt::append(out, ", %u join failure(s)", failed);
	}

	if (const u64 full = s_ps4_live_full.load(std::memory_order_relaxed))
	{
		fmt::append(out, ", %u never registered (registry full)", full);
	}

	if (const u64 leaked = s_ps4_live_leaked.load(std::memory_order_relaxed))
	{
		fmt::append(out, ", %u registry slot(s) leaked", leaked);
	}

	// 0.39: which threads exited without a join yet
	if (unjoined)
	{
		u32 named = 0;

		for (const ps4_unjoined_slot& s : s_ps4_unjoined)
		{
			if (s.key.load(std::memory_order_acquire) <= 1)
			{
				continue;
			}

			u64 words[reg::name_size / sizeof(u64)];

			for (usz i = 0; i < std::size(words); i++)
			{
				words[i] = s.name[i].load(std::memory_order_relaxed);
			}

			char name[reg::name_size];
			std::memcpy(name, words, sizeof(name));
			name[sizeof(name) - 1] = '\0';
			fmt::append(out, "%s%s", named++ ? ", " : "; exited not joined: ", name[0] ? name : "?");
		}
	}

	out += "; by name:";

	for (u32 i = 0; i < group_count; i++)
	{
		fmt::append(out, "%s %s", i ? "," : "", groups[i].key);

		if (groups[i].count > 1)
		{
			fmt::append(out, " x%u", groups[i].count);
		}
	}

	if (other)
	{
		fmt::append(out, ", %u other", other);
	}

	return out;
}

std::string ps4_thread_stacks_report() noexcept
{
	auto& p = s_ps4_pool;
	const u32 state = s_ps4_pool_state.load(std::memory_order_acquire);
	const u64 refusals = s_ps4_create_failures.load(std::memory_order_relaxed);

	if (state != ps4_pool_ready)
	{
		char why[sizeof(p.why)];
		{
			ps4_pool_guard lock;
			std::memcpy(why, p.why, sizeof(why));
		}

		return fmt::format("PS4 stacks: own-stack pool %s (%s); libkernel refusals %u", state == ps4_pool_uninit ? "not initialised" : "OFF",
			why[0] ? why : "-", refusals);
	}

	ps4_stack_pool copy;
	{
		ps4_pool_guard lock;
		copy.next = p.next;
		copy.cached_count = p.cached_count;
		copy.live = p.live;
		copy.peak = p.peak;
		copy.retired = p.retired;
		copy.committed = p.committed;
		copy.recycled = p.recycled;
		copy.released = p.released;
		copy.failures = p.failures;
		copy.last_errno = p.last_errno;
		copy.deepest = p.deepest;
		std::memcpy(copy.deepest_name, p.deepest_name, sizeof(copy.deepest_name));
		std::memcpy(copy.roles, p.roles, sizeof(copy.roles));
		copy.role_count = p.role_count;
	}

	const char* const mode = s_ps4_own_always.load() ? "own stack for every thread (own-stacks.flag)" :
		s_ps4_own_first.load() ? "own stack first (sticky after a rescue)" : "rescue only";

	std::string out = fmt::format("PS4 stacks: %s; %u live of %u (peak %u), %u cached, %u MiB direct held, %u slot(s) used (%u retired), %u committed, %u recycled, %u released,"
		" %u failure(s) (last errno %d); libkernel refusals %u, rescues %u ok / %u refused / %u not attempted (no stack or attr failure), own-first refusals %u;"
		" deepest use %u KiB of %u KiB ('%s')",
		mode, copy.live, ps4_stack_max_live(), copy.peak, copy.cached_count, s_ps4_stack_bytes.load(std::memory_order_relaxed) >> 20,
		copy.next, copy.retired, copy.committed, copy.recycled, copy.released,
		copy.failures, copy.last_errno, refusals, s_ps4_rescued.load(), s_ps4_rescue_refused.load(), s_ps4_rescue_no_stack.load(),
		s_ps4_own_first_refused.load(), copy.deepest / 1024, c_ps4_stack_size / 1024, copy.deepest_name[0] ? copy.deepest_name : "-");

	if (copy.role_count)
	{
		std::sort(copy.roles, copy.roles + copy.role_count, [](const ps4_stack_role& a, const ps4_stack_role& b)
		{
			return a.depth_kib > b.depth_kib;
		});

		out += "; by role:";

		for (u32 i = 0; i < copy.role_count && i < 10; i++)
		{
			fmt::append(out, "%s %s %uK x%u", i ? "," : "", copy.roles[i].key, copy.roles[i].depth_kib, copy.roles[i].count);
		}
	}

	return out;
}

std::string ps4_thread_stacks_init() noexcept
{
	static atomic_t<bool> s_done{false};

	if (s_done.exchange(true))
	{
		return ps4_thread_stacks_report();
	}

	auto& p = s_ps4_pool;
	const std::string pending = "/data/rpcs3/own-stacks-selftest.pending";

	const auto off = [](const std::string& reason) -> std::string
	{
		ps4_pool_set_why(reason);
		s_ps4_pool_state.store(ps4_pool_off, std::memory_order_release);
		return fmt::format("PS4 stacks: own-stack rescue OFF (%s); libkernel allocates every stack and a refused thread is not rescued", reason);
	};

	if (fs::is_file("/data/rpcs3/no-stack-rescue.flag"))
	{
		return off("no-stack-rescue.flag");
	}

	if (fs::is_file(pending))
	{
		return off("the previous boot died inside the own-stack self-test; delete /data/rpcs3/own-stacks-selftest.pending to retry");
	}

	// Private VA window: the first candidate where slot 0 can be reserved (NO_OVERWRITE)
	int probe_errno = 0;

	for (const std::uintptr_t window : c_ps4_windows)
	{
		u8* const first = ps4_stack_at(window, 0);

		if (utils::ps4::memory::reserve(c_ps4_stack_size, first))
		{
			utils::ps4::memory::release(first, c_ps4_stack_size);
			p.window = window;
			break;
		}

		probe_errno = errno;
	}

	if (!p.window)
	{
		return off(fmt::format("no free VA window at 0x%x or 0x%x (reserve errno %d)", c_ps4_windows[0], c_ps4_windows[1], probe_errno));
	}

	// Self-test: one thread on a pool stack must run on it, survive a kernel round trip and a signal frame,
	// and leave 256 KiB of frames that the high-water scan finds. A crash in here leaves the marker behind,
	// which turns the pool off on every later boot until it is deleted.
	ps4_startup_checkpoint("PS4 stacks: own-stack self-test starting (crash marker /data/rpcs3/own-stacks-selftest.pending)");

	if (!fs::write_file(pending, fs::rewrite, std::string("own thread stack self-test\n")))
	{
		return off("cannot write the self-test crash marker /data/rpcs3/own-stacks-selftest.pending");
	}

	ps4_selftest_probe probe;
	const u32 slot = ps4_stack_acquire();
	int create_rc = -1;
	int join_rc = -1;
	usz depth = 0;
	u64 base = 0;

	if (slot != umax)
	{
		base = reinterpret_cast<u64>(ps4_stack_base(slot));

		pthread_attr_t attr;
		const int attr_rc = pthread_attr_init(&attr);
		create_rc = attr_rc;

		if (attr_rc == 0)
		{
			create_rc = pthread_attr_setstack(&attr, ps4_stack_base(slot), c_ps4_stack_size);
			pthread_t tid{};

			if (create_rc == 0)
			{
				create_rc = pthread_create(&tid, &attr, ps4_stack_selftest_body, &probe);
			}

			if (create_rc == 0)
			{
				join_rc = pthread_join(tid, nullptr);
			}

			pthread_attr_destroy(&attr);
		}

		// Release only a stack nothing can run on any more (never started, or joined); else it stays out
		if (create_rc != 0 || join_rc == 0)
		{
			depth = ps4_stack_release(slot, nullptr);
		}
	}

	const bool marker_removed = fs::remove_file(pending);

	const bool runs_on_it = probe.local >= base && probe.local < base + c_ps4_stack_size;
	const bool sees_it = probe.attr_rc == 0 && reinterpret_cast<u64>(probe.base) == base && probe.size == c_ps4_stack_size;
	const bool ok = slot != umax && create_rc == 0 && join_rc == 0 && runs_on_it;
	const bool depth_ok = depth >= 256 * 1024 && depth < c_ps4_stack_size;

	std::string signal = "skipped (SIGPIPE is not on RPCS3's handler)";

	if (probe.handler_ours)
	{
		signal = probe.kill_rc != 0 ? fmt::format("not sent (pthread_kill rc=0x%x)", static_cast<u32>(probe.kill_rc)) :
			probe.sig_after > probe.sig_before ? std::string("delivered") : std::string("not observed (still pending?)");
	}

	const std::string detail = fmt::format("slot %s at 0x%x, create rc=0x%x, join rc=0x%x; the thread %s (local at 0x%x), attr says 0x%x+%u KiB (rc %d, %s);"
		" depth %u KiB (%s); signal frame on own stack: %s%s",
		slot != umax ? "ok" : "unavailable", base, static_cast<u32>(create_rc), static_cast<u32>(join_rc), runs_on_it ? "ran on it" : "did NOT run on it",
		probe.local, reinterpret_cast<u64>(probe.base), probe.size / 1024, probe.attr_rc, sees_it ? "match" : "MISMATCH",
		depth / 1024, depth_ok ? "high-water telemetry valid" : "high-water telemetry INVALID", signal,
		marker_removed ? "" : "; could NOT remove the crash marker: the next boot turns the pool off");

	if (!ok)
	{
		ps4_stack_flush_cache();

		if (slot == umax)
		{
			return off(fmt::format("self-test could not map a stack in window 0x%x (errno %d)", p.window, ps4_pool_last_errno()));
		}

		return off("self-test failed: " + detail);
	}

	// 0.29: every thread on an own stack (opt-in) only when a signal frame was seen being built on one: every
	// access violation (texture cache, lazy memory) needs exactly that. A rescued thread would die anyway.
	const bool flag_always = fs::is_file("/data/rpcs3/own-stacks.flag");
	const bool signal_proven = probe.handler_ours && probe.kill_rc == 0 && probe.sig_after > probe.sig_before;
	const bool own_always = flag_always && signal_proven;
	s_ps4_own_always.store(own_always);
	s_ps4_own_first.store(own_always);
	ps4_pool_set_why("ready");
	s_ps4_pool_state.store(ps4_pool_ready, std::memory_order_release);

	// The self-test stack stays cached: the first rescue needs no new direct memory (2 MiB held from now on)
	return fmt::format("PS4 stacks: own-stack %s ON - window 0x%x, %u KiB stacks above %u KiB unmapped guards, up to %u cached (1 ready now), at most %u live; self-test: %s",
		own_always ? "pool for EVERY thread_base thread (own-stacks.flag)" : flag_always ? "rescue (own-stacks.flag IGNORED: no signal frame proven on an own stack)" : "rescue",
		p.window, c_ps4_stack_size / 1024, (c_ps4_slot_stride - c_ps4_stack_size) / 1024, c_ps4_max_cached, ps4_stack_max_live(), detail);
}

// 0.38: direct memory committed for RPCS3-owned stacks (memory budget, util/ps4_memory_governor.hpp). Any thread.
unsigned long long ps4_thread_stack_direct_bytes() noexcept
{
	return s_ps4_stack_bytes.load(std::memory_order_relaxed);
}
#endif

thread_local DECLARE(thread_ctrl::g_tls_this_thread) = nullptr;

thread_local DECLARE(thread_ctrl::g_tls_error_callback) = nullptr;

DECLARE(thread_ctrl::g_native_core_layout) { native_core_arrangement::undefined };

void thread_base::start()
{
	m_sync.atomic_op([&](u32& v)
	{
		v &= ~static_cast<u32>(thread_state::mask);
		v |= static_cast<u32>(thread_state::created);
	});

#ifdef _WIN32
	m_thread = ::_beginthreadex(nullptr, 0, entry_point, this, CREATE_SUSPENDED, nullptr);
	ensure(m_thread);
	ensure(::ResumeThread(reinterpret_cast<HANDLE>(+m_thread)) != static_cast<DWORD>(-1));
#elif defined(__APPLE__)
	pthread_attr_t attrs;
	pthread_t thread_id{};
	struct sched_param sp;
	memset(&sp, 0, sizeof(struct sched_param));
	sp.sched_priority=99;
	pthread_attr_init(&attrs);
	pthread_attr_setstacksize(&attrs, 0x800000);
	
	pthread_attr_set_qos_class_np(&attrs, QOS_CLASS_USER_INTERACTIVE, 0);
	pthread_attr_setschedpolicy(&attrs, SCHED_RR);
	pthread_attr_setschedparam(&attrs, &sp);
	ensure(pthread_create(&thread_id, &attrs, entry_point, this) == 0);
#else
	pthread_t thread_id{};
#ifdef __ORBIS__
	// 0.28: every thread, mutex and condvar on the console comes from libkernel's internal ScePthread pool,
	// which can run out (Demon's Souls 0.27: EAGAIN 0x80020023 entering its menu). ps4_create_native() tries
	// libkernel, then one rescue on an RPCS3-owned stack (0.29). Must-succeed threads then retry at most
	// 3 x 25 ms before failing as upstream does: callers such as _sys_ppu_thread_create hold id_manager's lock,
	// and in Demon's Souls 0.28 the refusal lasted 0.78 s, so longer retries only delayed the fatal.
	// Threads that may be refused start through try_start_native() (ps4_try_start) and never sleep here.
	const std::string ps4_name = *m_tname.load();
	int create_error = ps4_create_native(thread_id, entry_point, this, m_ps4_stack_slot, ps4_name);
	for (u32 attempt = 1; create_error != 0 && attempt <= 3; attempt++)
	{
		if (attempt == 1)
		{
			unsigned long created = 0, finished = 0;
			ps4_thread_census(&created, &finished);
			sig_log.error("PS4: pthread_create failed for must-succeed thread '%s' (error 0x%x; kernel internal memory free: %llu bytes;"
				" flexible memory free: %llu bytes; threads created %lu, returned %lu, ~%lu live); retrying 3 x 25 ms",
				ps4_name, static_cast<u32>(create_error), ps4_internal_memory_free(), ps4_flexible_memory_free(), created, finished,
				orbis::threadsLive());
		}

		std::this_thread::sleep_for(std::chrono::milliseconds(25));
		create_error = ps4_create_native(thread_id, entry_point, this, m_ps4_stack_slot, ps4_name);
	}

	if (create_error == 0)
	{
		s_ps4_started.fetch_add(1, std::memory_order_relaxed);
	}
#ifdef RPCS3_PS4
	else
	{
		ps4_note_create_refused();
	}
#endif

	ensure(create_error == 0);
#else
	ensure(pthread_create(&thread_id, nullptr, entry_point, this) == 0);
#endif
#endif

#ifndef _WIN32
	// Update m_thread atomically
	u64 dest_id = 0;
	std::memcpy(&dest_id, &thread_id, sizeof(thread_id));

	if (!m_thread && !m_thread.compare_and_swap_test(0, dest_id))
	{
		ensure(m_thread == dest_id);
	}
#endif
}

#ifdef RPCS3_PS4
namespace
{
	// 0.31: test hook for the graceful thread paths. /data/rpcs3/refuse-threads.flag lists thread-name prefixes, one
	// per line ('#' starts a comment line). try_start_native() refuses every thread whose name starts with one of
	// them, exactly as if libkernel had refused it. Read once, at the first optional thread, and logged. start()
	// (must-succeed threads) ignores the file.
	struct ps4_refuse_list
	{
		static constexpr u32 max_prefixes = 16;
		char prefix[max_prefixes][48]{};
		u32 count = 0;
	};

	const ps4_refuse_list& ps4_test_refuse_list() noexcept
	{
		static const ps4_refuse_list list = []() noexcept
		{
			ps4_refuse_list result{};
			std::FILE* file = std::fopen("/data/rpcs3/refuse-threads.flag", "rb");

			if (!file)
			{
				return result;
			}

			char text[1024]{};
			const usz size = std::fread(text, 1, sizeof(text) - 1, file);
			std::fclose(file);

			for (usz pos = 0; pos < size && result.count < ps4_refuse_list::max_prefixes;)
			{
				usz end = pos;
				while (end < size && text[end] != '\n') end++;
				usz first = pos, last = end;
				while (first < last && (text[first] == ' ' || text[first] == '\t' || text[first] == '\r')) first++;
				while (last > first && (text[last - 1] == ' ' || text[last - 1] == '\t' || text[last - 1] == '\r')) last--;

				if (last > first && text[first] != '#')
				{
					const usz len = std::min<usz>(last - first, sizeof(result.prefix[0]) - 1);
					std::memcpy(result.prefix[result.count], text + first, len);
					result.prefix[result.count][len] = '\0';
					result.count++;
				}

				pos = end + 1;
			}

			std::string line = "PS4 test: refuse-threads.flag: optional threads named ";

			for (u32 i = 0; i < result.count; i++)
			{
				fmt::append(line, "%s'%s*'", i ? ", " : "", result.prefix[i]);
			}

			line += result.count ? " are refused as if libkernel had refused them" : "(none listed): nothing is refused";
			sig_log.warning("%s", line);
			ps4_startup_checkpoint(line.c_str());
			return result;
		}();

		return list;
	}

	bool ps4_test_refuses_thread(const std::string& name) noexcept
	{
		const ps4_refuse_list& list = ps4_test_refuse_list();

		for (u32 i = 0; i < list.count; i++)
		{
			if (name.starts_with(list.prefix[i]))
			{
				return true;
			}
		}

		return false;
	}
}

bool thread_base::try_start_native() noexcept
{
	m_sync.atomic_op([](u32& v)
	{
		v &= ~static_cast<u32>(thread_state::mask);
		v |= static_cast<u32>(thread_state::created);
	});

	// 0.29: the same create path as start() (libkernel, then one own-stack rescue), but no retry, no
	// sleep and no ensure(): a refusal is reported to the caller instead of ending the application.
	pthread_t thread_id{};
	const std::string ps4_name = *m_tname.load();
	// 0.31: a test refusal takes the same path as libkernel's EAGAIN (0x80020023)
	const bool test_refusal = ps4_test_refuses_thread(ps4_name);
	const int create_error = test_refusal ? static_cast<int>(0x80020023u) : ps4_create_native(thread_id, entry_point, this, m_ps4_stack_slot, ps4_name);

	if (create_error != 0)
	{
		// Never ran and never published: back to stand-by, so ~named_thread and join() return at once
		m_sync.atomic_op([](u32& v)
		{
			v &= ~static_cast<u32>(thread_state::mask);
			v |= static_cast<u32>(thread_state::finished);
		});

		if (!test_refusal)
		{
			ps4_note_create_refused();
		}

		static atomic_t<u32> s_refused{0};

		if (const u32 refused = ++s_refused; refused <= 4 || (refused & (refused - 1)) == 0)
		{
			const std::string line = fmt::format("PS4: thread '%s' not started (pthread_create error 0x%x, refusal #%u; own-stack rescue %s); continuing without it",
				ps4_name, static_cast<u32>(create_error), refused, test_refusal ? "not tried: test refusal from refuse-threads.flag" :
				ps4_pool_ready_now() ? "tried and failed, see the 'PS4 stacks' lines" : "off");
			sig_log.error("%s", line);
			ps4_startup_checkpoint(line.c_str());
		}

		return false;
	}

	s_ps4_started.fetch_add(1, std::memory_order_relaxed);

	// Update m_thread atomically (as start())
	u64 dest_id = 0;
	std::memcpy(&dest_id, &thread_id, sizeof(thread_id));

	if (!m_thread && !m_thread.compare_and_swap_test(0, dest_id))
	{
		ensure(m_thread == dest_id);
	}

	return true;
}

bool ps4_lazy_threads_enabled() noexcept
{
	static const bool enabled = []() noexcept
	{
		const bool keep = fs::is_file("/data/rpcs3/keep-service-threads.flag");
		ps4_startup_checkpoint(keep ? "PS4 threads: service threads start at boot (keep-service-threads.flag)"
		                            : "PS4 threads: service threads start on first use (keep-service-threads.flag starts them at boot)");
		return !keep;
	}();

	return enabled;
}

bool ps4_service_idle(std::string_view name) noexcept
{
	if (!ps4_lazy_threads_enabled())
	{
		return false;
	}

	// The conditions under which these bodies return at once (cellCamera.cpp, cellMic.cpp, CPUThread.cpp, lv2.cpp)
	const char* why = nullptr;

	if (name == "Camera Thread"sv)
	{
		why = g_cfg.io.camera == camera_handler::null ? "no device" : nullptr;
	}
	else if (name == "Microphone Thread"sv)
	{
		why = g_cfg.audio.microphone_type == microphone_handler::null ? "no device" : nullptr;
	}
	else if (name == "CPU Profiler"sv)
	{
		why = !g_cfg.core.ppu_prof && !g_cfg.core.spu_prof && !g_cfg.core.spu_debug ? "every profiler off" : nullptr;
	}
	else if (name == "PPU Syscall Usage Thread"sv)
	{
		why = "its counters are read without it";
	}

	if (!why)
	{
		return false;
	}

	ps4_startup_checkpoint(fmt::format("PS4 threads: %s not started (%s)", name, why).c_str());
	return true;
}

bool thread_base::ps4_start_lazy() noexcept
{
	for (;;)
	{
		const u32 state = m_ps4_lazy.load();

		if (state == 0)
		{
			return true;
		}

		if (state == 2)
		{
			// Another thread is starting it
			m_ps4_lazy.wait(2);
			continue;
		}

		if (!m_ps4_lazy.compare_and_swap_test(1, 2))
		{
			continue;
		}

		const std::string name = *m_tname.load();
		bool started = false;

		// Never after the stop began: the stop may have joined this stand-by object already
		for (u32 attempt = 0; attempt < 3 && !Emu.IsStopped(); attempt++)
		{
			if (attempt)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(25));
			}

			if (try_start_native())
			{
				started = true;
				break;
			}
		}

		m_ps4_lazy = started ? 0 : 1;
		m_ps4_lazy.notify_all();

		if (started)
		{
			ps4_startup_checkpoint(fmt::format("PS4 threads: %s started on first use", name).c_str());
		}
		else if (!Emu.IsStopped())
		{
			static atomic_t<u32> s_failed{0};

			if (const u32 n = ++s_failed; n <= 4 || (n & (n - 1)) == 0)
			{
				ps4_startup_checkpoint(fmt::format("PS4 threads: %s not started on first use (thread refused, #%u); tried again at its next use", name, n).c_str());
			}
		}

		return started;
	}
}
#endif

void thread_base::initialize(void (*error_cb)())
{
#ifndef _WIN32
#ifdef __APPLE__
	while (!m_thread)
	{
		busy_wait();
	}
	[[maybe_unused]] u64 new_tid = 0;
#elif defined(ANDROID) || defined(__ORBIS__)
	const u64 new_tid = pthread_self();
#else
	const u64 new_tid = reinterpret_cast<u64>(pthread_self());
#endif

	if (!m_thread && !m_thread.compare_and_swap_test(0, new_tid))
	{
		ensure(m_thread == new_tid);
	}
#endif

#if !defined(ANDROID) && (defined(__linux__) || defined(__DragonFly__) || defined(__FreeBSD__))
	// A new thread inherits its creator's affinity mask (e.g. compile workers spawned by a pinned PPU thread): reset it to the process mask
	if (g_cfg.core.thread_scheduler != thread_scheduler_mode::os)
	{
		thread_ctrl::set_thread_affinity_mask(0);
	}
#endif

#ifdef __ORBIS__
	ps4_reset_inherited_priority();
#endif

	// Initialize TLS variables
	thread_ctrl::g_tls_this_thread = this;

	thread_ctrl::g_tls_error_callback = error_cb;

	g_tls_log_prefix = []
	{
		return thread_ctrl::get_name_cached();
	};

	atomic_wait_engine::set_wait_callback([](const void*, u64 attempts, u64 stamp0) -> bool
	{
		if (attempts == umax)
		{
			g_tls_wait_time += utils::get_tsc() - stamp0;
		}
		else if (attempts > 1)
		{
			g_tls_wait_fail += attempts - 1;
		}

		return true;
	});

	set_name(thread_ctrl::get_name_cached());

#ifdef RPCS3_PS4
	// 0.29: enter the live-thread registry (ps4_thread_live_summary, sampling profiler) with this thread's stack
	// bounds; thread_base::finalize(u64), which every exit path passes before pthread_exit, leaves it.
	ps4_thread_enter_registry(thread_ctrl::get_name_cached().c_str(), m_ps4_stack_slot);
#endif
}

void thread_base::set_name(std::string name)
{
#ifdef _WIN32
	if (SetThreadDescriptionImport)
	{
		SetThreadDescriptionImport(GetCurrentThread(), utf8_to_wchar(name).c_str());
	}
#endif

#ifdef _MSC_VER
	struct THREADNAME_INFO
	{
		DWORD dwType;
		LPCSTR szName;
		DWORD dwThreadID;
		DWORD dwFlags;
	};

	// Set thread name for VS debugger
	if (IsDebuggerPresent()) [&]() NEVER_INLINE
	{
		THREADNAME_INFO info;
		info.dwType = 0x1000;
		info.szName = name.c_str();
		info.dwThreadID = -1;
		info.dwFlags = 0;

		__try
		{
			RaiseException(0x406D1388, 0, sizeof(info) / sizeof(ULONG_PTR), (ULONG_PTR*)&info);
		}
		__except (EXCEPTION_EXECUTE_HANDLER)
		{
		}
	}();
#endif

#if defined(__APPLE__)
	name.resize(std::min<usz>(15, name.size()));
	pthread_setname_np(name.c_str());
#elif defined(__DragonFly__) || (defined(__FreeBSD__) && !defined(__ORBIS__)) || defined(__OpenBSD__)
	pthread_set_name_np(pthread_self(), name.c_str());
#elif defined(__ORBIS__)
#ifdef RPCS3_PS4
	// 0.29: the registry keeps the full name (the kernel keeps 15 characters); no-op for unregistered threads
	ps4_thread_registry::rename(name.c_str());
#endif
	name.resize(std::min<usz>(15, name.size()));
	pthread_set_name_np(pthread_self(), name.c_str());
#elif defined(__NetBSD__)
	pthread_setname_np(pthread_self(), "%s", name.data());
#elif !defined(_WIN32)
	name.resize(std::min<usz>(15, name.size()));
	pthread_setname_np(pthread_self(), name.c_str());
#endif
}

u64 thread_base::finalize(thread_state result_state) noexcept
{
	// Report pending errors
	error_code::error_report(0, nullptr, nullptr, nullptr, nullptr);

#ifdef _WIN32
	static thread_local ULONG64 tls_cycles{};
	static thread_local u64 tls_time{};
	ULONG64 cycles{};
	QueryThreadCycleTime(GetCurrentThread(), &cycles);
	cycles -= tls_cycles;
	tls_cycles += cycles;
	FILETIME ctime, etime, ktime, utime;
	GetThreadTimes(GetCurrentThread(), &ctime, &etime, &ktime, &utime);
	const u64 time = ((ktime.dwLowDateTime | static_cast<u64>(ktime.dwHighDateTime) << 32) + (utime.dwLowDateTime | static_cast<u64>(utime.dwHighDateTime) << 32)) * 100ull - tls_time;
	tls_time += time;
	const u64 fsoft = 0;
	const u64 fhard = 0;
	const u64 ctxvol = 0;
	const u64 ctxinv = 0;
#elif defined(RUSAGE_THREAD)
	static thread_local u64 tls_time{}, tls_fsoft{}, tls_fhard{}, tls_ctxvol{}, tls_ctxinv{};
	const u64 cycles = 0; // Not supported
	struct ::rusage stats{};
	::getrusage(RUSAGE_THREAD, &stats);
	const u64 time = (stats.ru_utime.tv_sec + stats.ru_stime.tv_sec) * 1000000000ull + (stats.ru_utime.tv_usec + stats.ru_stime.tv_usec) * 1000ull - tls_time;
	tls_time += time;
	const u64 fsoft = stats.ru_minflt - tls_fsoft;
	tls_fsoft += fsoft;
	const u64 fhard = stats.ru_majflt - tls_fhard;
	tls_fhard += fhard;
	const u64 ctxvol = stats.ru_nvcsw - tls_ctxvol;
	tls_ctxvol += ctxvol;
	const u64 ctxinv = stats.ru_nivcsw - tls_ctxinv;
	tls_ctxinv += ctxinv;
#else
	const u64 cycles = 0;
	const u64 time = 0;
	const u64 fsoft = 0;
	const u64 fhard = 0;
	const u64 ctxvol = 0;
	const u64 ctxinv = 0;
#endif

	g_tls_log_prefix = []
	{
		return thread_ctrl::get_name_cached();
	};

	const bool is_cpu_thread = !!cpu_thread::get_current();
	auto& thread_log = (is_cpu_thread || g_tls_fault_all ? sig_log.notice : sig_log.trace);

	thread_log("Thread time: %fs (%fGc); Faults: %u [rsx:%u, spu:%u]; [soft:%u hard:%u]; Switches:[vol:%u unvol:%u]; Wait:[%.3fs, spur:%u]",
		time / 1000000000.,
		cycles / 1000000000.,
		g_tls_fault_all,
		g_tls_fault_rsx,
		g_tls_fault_spu,
		fsoft, fhard, ctxvol, ctxinv,
		g_tls_wait_time / (utils::get_tsc_freq() / 1.),
		g_tls_wait_fail);

	atomic_wait_engine::set_wait_callback(nullptr);

	// Avoid race with the destructor
	const u64 _self = m_thread;

	// Set result state (errored or finalized)
	m_sync.fetch_op([&](u32& v)
	{
		v &= -4;
		v |= static_cast<u32>(result_state);
	});

	// Signal waiting threads
	m_sync.notify_all();

	return _self;
}

thread_base::native_entry thread_base::finalize(u64 _self) noexcept
{
#ifdef RPCS3_PS4
	// 0.29: every thread_base exit path (normal return, emergency_exit, silent_exit) passes here before
	// pthread_exit. Leaving the live-thread registry first makes a late profiler signal harmless.
	ps4_thread_exit_registry();
#endif

	g_tls_fault_all = 0;
	g_tls_fault_rsx = 0;
	g_tls_fault_spu = 0;
	g_tls_wait_time = 0;
	g_tls_wait_fail = 0;
	g_tls_access_violation_recovered = umax;

	g_tls_log_prefix = []() -> std::string { return {}; };

	if (_self == umax)
	{
		thread_ctrl::g_tls_this_thread = nullptr;
		return nullptr;
	}

#ifdef _WIN32
	_endthreadex(0);
#else
#ifdef __ORBIS__
	// 0.29: orbis-compat's trampoline never sees a thread that leaves through pthread_exit return
	orbis::threadNoteExit();
#endif
	pthread_exit(nullptr);
#endif

	return nullptr;
}

thread_base::native_entry thread_base::make_trampoline(u64(*entry)(thread_base* _base))
{
	return build_function_asm<native_entry>("", [&](native_asm& c, auto& args)
	{
		using namespace asmjit;

#if defined(ARCH_X64)
		Label _ret = c.newLabel();
		c.push(x86::rbp);
		c.sub(x86::rsp, 0x20);

		// Call entry point (TODO: support for detached threads missing?)
		c.call(entry);

		// Call finalize, return if zero
		c.mov(args[0], x86::rax);
		c.call(static_cast<native_entry(*)(u64)>(&finalize));
		c.test(x86::rax, x86::rax);
		c.jz(_ret);

		// Otherwise, call it as an entry point with first arg = new current thread
		c.mov(x86::rbp, x86::rax);
		c.call(thread_ctrl::get_current);
		c.mov(args[0], x86::rax);
		c.add(x86::rsp, 0x28);
		c.jmp(x86::rbp);

		c.bind(_ret);
		c.add(x86::rsp, 0x28);
		c.ret();
#else
	UNUSED(c);
	UNUSED(args);
	UNUSED(entry);
#endif
	});
}

thread_state thread_ctrl::state()
{
	auto _this = g_tls_this_thread;

	// Guard for recursive calls (TODO: may be more effective to reuse one of m_sync bits)
	static thread_local bool s_tls_exec = false;

	// Drain execution queue
	if (!s_tls_exec)
	{
		s_tls_exec = true;
		_this->exec();
		s_tls_exec = false;
	}

	return static_cast<thread_state>(_this->m_sync & 3);
}

void thread_ctrl::wait_for(u64 usec, [[maybe_unused]] bool alert /* true */)
{
	if (!usec)
	{
		return;
	}

	auto _this = g_tls_this_thread;

	if (!alert && usec > 50000)
	{
		usec = 50000;
	}

#ifdef __linux__
	static thread_local struct linux_timer_handle_t
	{
		// Allocate timer only if needed (i.e. someone calls wait_for with alert and short period)
		const int m_timer = timerfd_create(CLOCK_MONOTONIC, 0);

		linux_timer_handle_t() noexcept
		{
			if (m_timer == -1)
			{
				sig_log.error("Linux timer allocation failed, using the fallback instead.");
			}
		}

		operator int() const
		{
			return m_timer;
		}

		~linux_timer_handle_t()
		{
			if (m_timer != -1)
			{
				close(m_timer);
			}
		}
	} fd_timer;

	if (!alert && fd_timer != -1)
	{
		struct itimerspec timeout;
		u64 missed;

		timeout.it_value.tv_nsec = usec % 1'000'000 * 1'000ull;
		timeout.it_value.tv_sec = usec / 1'000'000;
		timeout.it_interval.tv_sec = 0;
		timeout.it_interval.tv_nsec = 0;
		timerfd_settime(fd_timer, 0, &timeout, NULL);
		if (read(fd_timer, &missed, sizeof(missed)) != sizeof(missed))
			sig_log.error("timerfd: read() failed");
		return;
	}
#endif

	if (alert)
	{
		if (_this->m_sync.bit_test_reset(2) || _this->m_taskq)
		{
			return;
		}
	}

	// Wait for signal and thread state abort
	atomic_wait::list<2> list{};

	if (alert)
	{
		list.set<0>(_this->m_sync, 0);
		list.template set<1>(_this->m_taskq);
	}
	else
	{
		list.set<0>(_this->m_dummy, 0);
	}

	list.wait(atomic_wait_timeout{usec <= 0xffff'ffff'ffff'ffff / 1000 ? usec * 1000 : 0xffff'ffff'ffff'ffff});
}


void thread_ctrl::wait_until(u64* wait_time, u64 add_time, u64 min_wait, bool update_to_current_time)
{
	*wait_time = utils::add_saturate<u64>(*wait_time, add_time);

	// TODO: Implement proper support for "waiting until" inside atomic wait engine
	const u64 current_time = get_system_time();

	if (current_time > *wait_time)
	{
		if (update_to_current_time)
		{
			*wait_time = current_time + (add_time - (current_time - *wait_time) % add_time);
		}
		else if (!min_wait)
		{
			return;
		}
	}

	if (min_wait)
	{
		*wait_time = std::max<u64>(*wait_time, utils::add_saturate<u64>(current_time, min_wait));
	}

	wait_for(*wait_time - current_time);
}

void thread_ctrl::wait_for_accurate(u64 usec)
{
	if (!usec)
	{
		return;
	}

	if (usec > 50000)
	{
		fmt::throw_exception("thread_ctrl::wait_for_accurate: unsupported amount");
	}

#ifdef __linux__
	return wait_for(usec, false);
#else
	using namespace std::chrono_literals;

	const auto until = std::chrono::steady_clock::now() + 1us * usec;

	while (true)
	{
		// Host scheduler quantum for windows (worst case)
		constexpr u64 host_min_quantum = 500;

		if (usec >= host_min_quantum)
		{
			// Wait on multiple of min quantum for large durations to avoid overloading low thread cpus
			wait_for(usec - (usec % host_min_quantum), false);
		}
		// TODO: Determine best value for yield delay
		else if (usec >= host_min_quantum / 2)
		{
			std::this_thread::yield();
		}
		else
		{
			busy_wait(100);
		}

		const auto current = std::chrono::steady_clock::now();

		if (current >= until)
		{
			break;
		}

		usec = std::chrono::duration_cast<std::chrono::microseconds>(until - current).count();
	}
#endif
}

std::string thread_ctrl::get_name_cached()
{
	auto _this = thread_ctrl::g_tls_this_thread;

	if (!_this)
	{
		return {};
	}

	static thread_local shared_ptr<std::string> name_cache;

	if (!_this->m_tname.is_equal(name_cache)) [[unlikely]]
	{
		_this->m_tname.peek_op([&](const shared_ptr<std::string>& ptr)
		{
			if (ptr != name_cache)
			{
				name_cache = ptr;
			}
		});
	}

	return *name_cache;
}

thread_base::thread_base(native_entry entry, std::string name) noexcept
	: entry_point(entry)
	, m_tname(make_single_value(std::move(name)))
{
}

thread_base::~thread_base() noexcept
{
	// Cleanup abandoned tasks: initialize default results and signal
	this->exec();

	// Cleanup
	{
#ifdef _WIN32
		const HANDLE handle0 = reinterpret_cast<HANDLE>(m_thread.load());
		WaitForSingleObject(handle0, INFINITE);
		CloseHandle(handle0);
#elif defined(ANDROID) || defined(__ORBIS__)
#ifdef RPCS3_PS4
		// 0.29: a stand-by object that never started (refused ps4_try_start(), unused launch_retainer
		// object) has no thread, so there is nothing to join. After a join, an own stack goes back to the pool.
		if (const u64 handle = m_thread.load())
		{
			const int join_rc = pthread_join(handle, nullptr);

			if (join_rc == 0)
			{
				// 0.29: pairs with threadNoteExit, so orbis-compat can tell exited-but-unjoined threads
				orbis::threadNoteJoin();
				ps4_unjoined_remove(handle);
			}

			ps4_thread_joined(join_rc, m_ps4_stack_slot, *m_tname.load());
		}
#else
		pthread_join(m_thread.load(), nullptr);
#endif
#else
		pthread_join(reinterpret_cast<pthread_t>(m_thread.load()), nullptr);
#endif
	}
}

bool thread_base::join(bool dtor) const
{
	// Check if already finished
	if (m_sync & 2)
	{
		return (m_sync & 3) == 3;
	}

	// Hacked for too sleepy threads (1ms) TODO: make sure it's unneeded and remove
	const auto timeout = dtor && Emu.IsStopped() ? atomic_wait_timeout{1'000'000} : atomic_wait_timeout::inf;

	auto stamp0 = utils::get_tsc();

	for (u64 i = 0; (m_sync & 3) <= 1; i++)
	{
		m_sync.wait(m_sync & ~2, timeout);

		if (m_sync & 2)
		{
			break;
		}

		if (i >= 16 && !(i & (i - 1)) && timeout != atomic_wait_timeout::inf)
		{
			sig_log.error("Thread [%s] is too sleepy. Waiting for it %.3fus already!", *m_tname.load(), (utils::get_tsc() - stamp0) / (utils::get_tsc_freq() / 1000000.));
		}
	}

	return (m_sync & 3) == 3;
}

void thread_base::notify()
{
	// Set notification
	m_sync |= 4;
	m_sync.notify_all();
}

u64 thread_base::get_native_id() const
{
#ifdef _WIN32
	return GetThreadId(reinterpret_cast<HANDLE>(m_thread.load()));
#else
	return m_thread.load();
#endif
}

u64 thread_base::get_cycles()
{
	u64 cycles = 0;

	const u64 handle = m_thread;

#ifdef _WIN32
	if (QueryThreadCycleTime(reinterpret_cast<HANDLE>(handle), &cycles))
	{
#elif __APPLE__
	mach_port_name_t port = pthread_mach_thread_np(reinterpret_cast<pthread_t>(handle));
	mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
	thread_basic_info_data_t info;
	kern_return_t ret = thread_info(port, THREAD_BASIC_INFO, reinterpret_cast<thread_info_t>(&info), &count);
	if (ret == KERN_SUCCESS)
	{
		cycles = static_cast<u64>(info.user_time.seconds + info.system_time.seconds) * 1'000'000'000 +
			static_cast<u64>(info.user_time.microseconds + info.system_time.microseconds) * 1'000;
#else
	clockid_t _clock;
	struct timespec thread_time;
#if defined(ANDROID) || defined(__ORBIS__)
	pthread_t thread_id = handle;
#else
	pthread_t thread_id = reinterpret_cast<pthread_t>(handle);
#endif
	if (!pthread_getcpuclockid(thread_id, &_clock) && !clock_gettime(_clock, &thread_time))
	{
		cycles = static_cast<u64>(thread_time.tv_sec) * 1'000'000'000 + thread_time.tv_nsec;
#endif
		if (const u64 old_cycles = m_cycles.exchange(cycles))
		{
			return cycles - old_cycles;
		}

		// Report 0 the first time this function is called
		return 0;
	}
	else
	{
		return m_cycles;
	}
}

void thread_base::push(shared_ptr<thread_future> task)
{
#ifdef RPCS3_PS4
	// 0.39: a lazy task-queue thread starts with its first task
	if (m_ps4_lazy)
	{
		ps4_start_lazy();
	}
#endif

	const auto next = &task->next;
	m_taskq.push_head(*next, std::move(task));
	m_taskq.notify_one();
}

void thread_base::exec()
{
	if (!m_taskq) [[likely]]
	{
		return;
	}

	while (shared_ptr<thread_future> head = m_taskq.exchange(null_ptr))
	{
		// TODO: check if adapting reverse algorithm is feasible here
		thread_future* prev_head{head.get()};

		for (thread_future* prev{};;)
		{
			utils::prefetch_exec(prev_head->exec.load());

			if (auto next = prev_head->next.get())
			{
				prev = std::exchange(prev_head, next);
				prev_head->prev = prev;
			}
			else
			{
				break;
			}
		}

		for (auto ptr = prev_head; ptr; ptr = ptr->prev)
		{
			if (auto task = ptr->exec.load()) [[likely]]
			{
				// Execute or discard (if aborting)
				if ((m_sync & 3) == 0) [[likely]]
				{
					task(this, ptr);
				}
				else
				{
					task(nullptr, ptr);
				}

				// Notify waiters
				ptr->done.release(1);
				ptr->done.notify_all();
			}

			if (ptr->next)
			{
				// Partial cleanup
				ptr->next.reset();
			}
		}

		if (!m_taskq) [[likely]]
		{
			return;
		}
	}
}

void thread_ctrl::set_name(std::string name)
{
	ensure(g_tls_this_thread);
	g_tls_this_thread->m_tname.store(make_single<std::string>(name));
	g_tls_this_thread->set_name(std::move(name));
}

#ifdef RPCS3_PS4
// Image base (OpenOrbis link.x). Declared here: a block-scope declaration inside the unnamed namespace below would
// name a different, internal object.
extern char __text_start[];

namespace
{
	// 0.33: shared_mutex holds of threads that ended without unwinding (emergency_exit and silent_exit leave through
	// pthread_exit, which runs no destructors on the PS4). A hold is given back only while the emulator stops the
	// session in which its owner died, never while the guest runs: by a waiter of that lock once it has waited half
	// a second (a live holder of a lock whose record is out of date lets go sooner), or, for a lock in the image
	// (global), by ps4_release_dead_thread_locks at the end of the stop.
	struct ps4_dead_hold
	{
		atomic_t<u64> key{0}; // shared_mutex address | 1 for an exclusive hold; 0 free, c_ps4_hold_writing taken
		u64 stop_id = 0;      // Emu.GetEmulationIdentifier() when the owner died
		char owner[32]{};     // owner thread name, for the log
	};

	// Never a key: a shared_mutex is 4-byte aligned
	constexpr u64 c_ps4_hold_writing = 2;

	ps4_dead_hold s_ps4_dead_holds[64]{};

	using ps4_ull = unsigned long long;

	// A global (static) lock: inside the loaded image, which is never freed. The image is far below 1 GiB; the heap
	// lies at much higher addresses.
	bool ps4_in_image(u64 addr) noexcept
	{
		const u64 base = reinterpret_cast<u64>(__text_start);

		return addr >= base && addr - base < 0x4000'0000;
	}

	// "0x7313540 (elf+0x6f13540, exclusive)". .text starts at ELF address 0, so for a global lock the offset is the
	// address llvm-nm prints for the object holding it; a heap lock has no ELF offset.
	void ps4_lock_text(char (&out)[96], u64 key) noexcept
	{
		const u64 addr = key & ~u64{1};
		const u64 base = reinterpret_cast<u64>(__text_start);
		const char* const mode = key & 1 ? "exclusive" : "shared";

		if (ps4_in_image(addr))
		{
			std::snprintf(out, sizeof(out), "0x%llx (elf+0x%llx, %s)", ps4_ull{addr}, ps4_ull{addr - base}, mode);
		}
		else
		{
			std::snprintf(out, sizeof(out), "0x%llx (heap, %s)", ps4_ull{addr}, mode);
		}
	}

	// Moves the calling thread's held-lock record into the dead-hold table and wakes the current waiters of those
	// locks, which then wait in bounded steps (Utilities/mutex.cpp). Each hold is listed in startup.log (durable)
	// and RPCS3.log. Called on the dying thread only.
	void ps4_hand_over_held_locks(const char* why) noexcept
	{
		ps4_held_locks& held = g_tls_ps4_held_locks;
		const u32 count = std::min<u32>(held.count, ps4_held_locks::c_slots);
		const u32 lost = held.lost;

		if (!count && !lost)
		{
			return;
		}

		if (!g_ps4_lock_record.observe())
		{
			// no-lock-release.flag: only holds recorded before the flag was read can be here
			held.count = 0;
			held.lost = 0;
			return;
		}

		uptr keys[ps4_held_locks::c_slots]{};

		for (u32 i = 0; i < count; i++)
		{
			keys[i] = held.slot[i];
		}

		// Empty the record before touching any lock: a fault below re-enters emergency_exit, which then finds nothing
		held.count = 0;
		held.lost = 0;
		std::atomic_signal_fence(std::memory_order_seq_cst);

		const std::string name_text = thread_ctrl::get_name();
		const char* const name = name_text.c_str();
		const u64 stop_id = static_cast<u64>(Emu.GetEmulationIdentifier());

		char line[320];

		for (u32 i = 0; i < count; i++)
		{
			const u64 key = keys[i];
			const auto mutex = reinterpret_cast<shared_mutex*>(key & ~u64{1});

			// A slot that ps4_lock_record_add reserved but had not filled when the thread faulted still holds 0 (or a
			// stale value): never dereference something that cannot be a shared_mutex (4-byte aligned, not in page 0)
			if (const u64 addr = key & ~u64{1}; addr < 0x10000 || (addr & 3))
			{
				std::snprintf(line, sizeof(line), "slot value 0x%llx of '%s' (%s): not a lock address; ignored", ps4_ull{key}, name, why);
				ps4_fatal_note("held lock ", line);
				sig_log.warning("Held lock %s", line);
				continue;
			}

			char lock[96];
			ps4_lock_text(lock, key);

			// One thread cannot hold a lock exclusively twice: a repeated exclusive record would release someone else
			bool repeated = false;

			for (u32 j = 0; j < i && (key & 1); j++)
			{
				repeated |= keys[j] == key;
			}

			if (repeated || !mutex->ps4_looks_held(key & 1))
			{
				// The record is out of date: the lock went to another thread with its guard, or was reset
				std::snprintf(line, sizeof(line), "%s of '%s' (%s): %s; ignored", lock, name, why, repeated ? "recorded twice" : "not held any more");
				ps4_fatal_note("held lock ", line);
				sig_log.warning("Held lock %s", line);
				continue;
			}

			ps4_dead_hold* slot = nullptr;

			for (auto& candidate : s_ps4_dead_holds)
			{
				if (candidate.key.compare_and_swap_test(0, c_ps4_hold_writing))
				{
					slot = &candidate;
					break;
				}
			}

			if (!slot)
			{
				std::snprintf(line, sizeof(line), "%s of '%s' (%s): dead-hold table full; stays held", lock, name, why);
				ps4_fatal_note("held lock ", line);
				sig_log.error("Held lock %s", line);
				continue;
			}

			slot->stop_id = stop_id;
			std::snprintf(slot->owner, sizeof(slot->owner), "%s", name);
			g_ps4_dead_holds += 1;
			slot->key.release(key);

			std::snprintf(line, sizeof(line), "%s of '%s' (%s): released only while the emulator stops", lock, name, why);
			ps4_fatal_note("held lock ", line);
			sig_log.error("Held lock %s", line);

			// Waiters already asleep switch to bounded waits; the lock state itself is unchanged
			mutex->ps4_wake_waiters();
		}

		if (lost)
		{
			std::snprintf(line, sizeof(line), "'%s' (%s): %u older hold(s) were forgotten to make room in the %u-slot record; any of them still held stays held",
				name, why, lost, ps4_held_locks::c_slots);
			ps4_fatal_note("held lock ", line);
			sig_log.error("Held lock %s", line);
		}
	}

	// Gives back the dead holds of 'only' (every global one when null) recorded in the session being stopped. Records
	// of a session whose stop has already ended are dropped and their locks left as they are. Without a waiter a heap
	// lock is skipped: its object may already be freed, and only a waiter proves a live lock at that address (its
	// record is dropped at the next stop). Nothing happens unless the emulator is stopping.
	u32 ps4_give_back_dead_holds(const shared_mutex* only, const char* by) noexcept
	{
		if (!g_ps4_dead_holds || Emu.GetStatus(false) != system_state::stopping)
		{
			return 0;
		}

		const u64 stop_id = static_cast<u64>(Emu.GetEmulationIdentifier());

		u32 given = 0;

		for (auto& slot : s_ps4_dead_holds)
		{
			const u64 key = slot.key.load();

			if (key <= c_ps4_hold_writing)
			{
				continue;
			}

			const bool this_session = slot.stop_id == stop_id;
			const u64 addr = key & ~u64{1};

			if (this_session && (only ? addr != reinterpret_cast<u64>(only) : !ps4_in_image(addr)))
			{
				continue;
			}

			char owner[sizeof(slot.owner)];
			std::memcpy(owner, slot.owner, sizeof(owner));
			owner[sizeof(owner) - 1] = 0;

			// Claim the record: other waiters and the final sweep may race for it
			if (!slot.key.compare_and_swap_test(key, 0))
			{
				continue;
			}

			g_ps4_dead_holds -= 1;

			char lock[96];
			ps4_lock_text(lock, key);

			char line[320];

			if (!this_session)
			{
				std::snprintf(line, sizeof(line), "%s left by '%s': record dropped, the stop of its session has ended", lock, owner);
				ps4_fatal_note("lock release: ", line);
				sig_log.warning("Lock release: %s", line);
				continue;
			}

			const bool ok = reinterpret_cast<shared_mutex*>(addr)->ps4_give_back(key & 1);
			given += ok;

			std::snprintf(line, sizeof(line), "%s left by '%s': %s (emulator stopping; by %s)", lock, owner,
				ok ? "given back" : "not held in that mode any more; left alone", by);
			ps4_fatal_note("lock release: ", line);
			sig_log.warning("Lock release: %s", line);
		}

		return given;
	}
}

void ps4_dead_hold_wait(shared_mutex& mutex, u64& since) noexcept
{
	const u64 now = get_system_time();

	if (!since)
	{
		since = now;
		return;
	}

	if (now - since >= 500'000)
	{
		ps4_give_back_dead_holds(&mutex, "a waiter of that lock");
	}
}

u32 ps4_release_dead_thread_locks() noexcept
{
	return ps4_give_back_dead_holds(nullptr, "the end of the stop");
}
#endif

[[noreturn]] void thread_ctrl::emergency_exit(std::string_view reason)
{
	// Print stacktrace
#ifdef __cpp_lib_stacktrace
	if (rpcs3::is_local_build())
	{
		std::ostringstream oss;
		oss << std::stacktrace::current();
		sys_log.notice("StackTrace\n\n%s\n", oss.str());
	}
#endif

	if (const std::string info = dump_useful_thread_info(); !info.empty())
	{
		sys_log.notice("\n%s", info);
	}

	std::string reason_buf;

	if (auto ppu = cpu_thread::get_current<ppu_thread>())
	{
		if (auto func = ppu->current_function)
		{
			fmt::append(reason_buf, "%s (PPU: %s)", reason, func);
		}
	}

	if (auto [total, current] = utils::get_memory_usage(); total - current <= 256 * 1024 * 1024)
	{
		if (reason_buf.empty())
		{
			reason_buf = std::string{reason};
		}

		fmt::append(reason_buf, " (Possible RAM deficiency: free RAM: %dMB)", (total - current) / (1024 * 1024));
	}

	if (!reason_buf.empty())
	{
		reason = reason_buf;
	}

	sig_log.fatal("Thread terminated due to fatal error: %s", reason);

	logs::listener::sync_all();

	if (IsDebuggerPresent())
	{
		// Prevent repeatedly halting the debugger in case multiple threads crashed at once
		static atomic_t<u64> s_last_break = 0;
		const u64 current_break = get_system_time() & -2;

		if (s_last_break.fetch_op([current_break](u64& v)
		{
			if (current_break >= (v & -2) && current_break - (v & -2) >= 20'000'000)
			{
				v = current_break;
				return true;
			}

			// Let's allow a single more thread to halt the debugger so the programmer sees the pattern
			if (!(v & 1))
			{
				v |= 1;
				return true;
			}

			return false;
		}).second)
		{
#ifndef __APPLE__
			utils::trap();
#endif
		}
	}

	if (const auto _this = g_tls_this_thread)
	{
#ifdef RPCS3_PS4
		{
			// Durable record of the first thread fatal (fatal.txt / startup.log) before the thread exits.
			char buffer[1024];
			const usz n = std::min<usz>(reason.size(), sizeof(buffer) - 1);
			std::memcpy(buffer, reason.data(), n);
			buffer[n] = 0;
			ps4_note_thread_fatal(buffer);
		}

		// 0.33: the locks this thread holds stay held after pthread_exit; list them after the fatal record and hand
		// them to the stop that the fatal triggers
		ps4_hand_over_held_locks("thread fatal");
#endif

		g_tls_error_callback();

		u64 _self = _this->finalize(thread_state::errored);

		if (_self == umax)
		{
			// Unused, detached thread support remnant
			delete _this;
		}

		thread_base::finalize(umax);

#ifdef _WIN32
		_endthreadex(0);
#else
#ifdef __ORBIS__
#ifdef RPCS3_PS4
		// Whatever the error callback took and still holds
		ps4_hand_over_held_locks("thread fatal");
#endif
		// 0.29: keeps orbis-compat's live-thread count exact (see thread_base::finalize)
		orbis::threadNoteExit();
#endif
		pthread_exit(nullptr);
#endif
	}

	report_fatal_error(reason);
}

void thread_ctrl::silent_exit() noexcept
{
#ifdef __ORBIS__
	// 0.29: only a thread_base thread may note its exit (orbis-compat counts the threads its pthread_create
	// started); finalize(umax) below clears g_tls_this_thread, so decide first.
	const bool ps4_note_exit = g_tls_this_thread != nullptr;
#endif

#ifdef RPCS3_PS4
	// 0.33: same as emergency_exit: no unwinding, so hand the held locks to the next emulator stop
	ps4_hand_over_held_locks("silent exit");
#endif

	if (const auto _this = g_tls_this_thread)
	{
		g_tls_error_callback();

		u64 _self = _this->finalize(thread_state::errored);

		if (_self == umax)
		{
			// Unused, detached thread support remnant
			delete _this;
		}

		thread_base::finalize(umax);
	}

#ifdef _WIN32
	_endthreadex(0);
#else
#ifdef __ORBIS__
#ifdef RPCS3_PS4
	ps4_hand_over_held_locks("silent exit");
#endif
	// 0.29: keeps orbis-compat's live-thread count exact (see thread_base::finalize)
	if (ps4_note_exit)
	{
		orbis::threadNoteExit();
	}
#endif
	pthread_exit(nullptr);
#endif

	std::abort();
}

void thread_ctrl::detect_cpu_layout()
{
	if (!g_native_core_layout.compare_and_swap_test(native_core_arrangement::undefined, native_core_arrangement::generic))
		return;

	const auto system_id = utils::get_cpu_brand();
	if (system_id.find("Ryzen") != umax)
	{
		g_native_core_layout.store(native_core_arrangement::amd_ccx);
	}
	else if (system_id.find("Intel") != umax)
	{
#ifdef _WIN32
		const LOGICAL_PROCESSOR_RELATIONSHIP relationship = LOGICAL_PROCESSOR_RELATIONSHIP::RelationProcessorCore;
		DWORD buffer_size = 0;

		// If buffer size is set to 0 bytes, it will be overwritten with the required size
		if (GetLogicalProcessorInformationEx(relationship, nullptr, &buffer_size))
		{
			sig_log.error("GetLogicalProcessorInformationEx returned 0 bytes");
			return;
		}
		DWORD error_code = GetLastError();
		if (error_code != ERROR_INSUFFICIENT_BUFFER)
		{
			sig_log.error("Unexpected windows error code when detecting CPU layout: %u", error_code);
			return;
		}

		std::vector<u8> buffer(buffer_size);

		if (!GetLogicalProcessorInformationEx(relationship,
			reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *>(buffer.data()), &buffer_size))
		{
			sig_log.error("GetLogicalProcessorInformationEx failed (size=%u, error=%s)", buffer_size, fmt::win_error{GetLastError(), nullptr});
		}
		else
		{
			// Iterate through the buffer until a core with hyperthreading is found
			auto ptr = reinterpret_cast<uptr>(buffer.data());
			const uptr end = ptr + buffer_size;

			while (ptr < end)
			{
				auto info = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX *>(ptr);
				if (info->Relationship == relationship && info->Processor.Flags == LTP_PC_SMT)
				{
					g_native_core_layout.store(native_core_arrangement::intel_ht);
					break;
				}
				ptr += info->Size;
			}
		}
#else
		sig_log.todo("Thread scheduler is not implemented for Intel and this OS");
#endif
	}
}

u64 thread_ctrl::get_affinity_mask(thread_class group)
{
	detect_cpu_layout();

	if (const auto thread_count = utils::get_thread_count())
	{
		const u64 all_cores_mask = process_affinity_mask;

		switch (g_native_core_layout)
		{
		default:
		case native_core_arrangement::generic:
		{
			return all_cores_mask;
		}
		case native_core_arrangement::amd_ccx:
		{
			if (thread_count <= 8)
			{
				// Single CCX or not enough threads, do nothing
				return all_cores_mask;
			}

			u64 spu_mask, ppu_mask, rsx_mask;
			spu_mask = ppu_mask = rsx_mask = all_cores_mask; // Fallback, in case someone is messing with core config

			const auto system_id = utils::get_cpu_brand();
			const auto family_id = utils::get_cpu_family();
			const auto model_id = utils::get_cpu_model();

			switch (family_id)
			{
			case 0x17: // Zen, Zen+, Zen2
			case 0x18: // Dhyana core (Zen)
			{
				if (model_id > 0x30)
				{
					// Zen2 (models 49, 96, 113, 144)
					// Much improved inter-CCX latency
					switch (thread_count)
					{
					case 128:
					case 64:
					case 48:
					case 32:
						// TR 3000 series, or R9 3950X, Assign threads 9-32
						ppu_mask = 0b11111111000000000000000000000000;
						spu_mask = 0b00000000111111110000000000000000;
						rsx_mask = 0b00000000000000001111111100000000;
						break;
					case 24:
						// 3900X, Assign threads 7-24
						ppu_mask = 0b111111000000000000000000;
						spu_mask = 0b000000111111000000000000;
						rsx_mask = 0b000000000000111111000000;
						break;
					case 16:
						// 3700, 3800 family, Assign threads 1-16
						ppu_mask = 0b0000000011110000;
						spu_mask = 0b1111111100000000;
						rsx_mask = 0b0000000000001111;
						break;
					case 12:
						// 3600 family, Assign threads 1-12
						ppu_mask = 0b000000111000;
						spu_mask = 0b111111000000;
						rsx_mask = 0b000000000111;
						break;
					default:
						break;
					}
				}
				else
				{
					// Zen, Zen+ (models 1, 8(+), 17, 24(+), 32)
					switch (thread_count)
					{
					case 64:
						// TR 2990WX, Assign threads 17-32
						ppu_mask = 0b00000000111111110000000000000000;
						spu_mask = ppu_mask;
						rsx_mask = 0b11111111000000000000000000000000;
						break;
					case 48:
						// TR 2970WX, Assign threads 9-24
						ppu_mask = 0b000000111111000000000000;
						spu_mask = ppu_mask;
						rsx_mask = 0b111111000000000000000000;
						break;
					case 32:
						// TR 2950X, TR 1950X, Assign threads 17-32
						ppu_mask = 0b00000000111111110000000000000000;
						spu_mask = ppu_mask;
						rsx_mask = 0b11111111000000000000000000000000;
						break;
					case 24:
						// TR 1920X, 2920X, Assign threads 13-24
						ppu_mask = 0b000000111111000000000000;
						spu_mask = ppu_mask;
						rsx_mask = 0b111111000000000000000000;
						break;
					case 16:
						// 1700, 1800, 2700, TR 1900X family
						if (g_cfg.core.thread_scheduler == thread_scheduler_mode::alt)
						{
							ppu_mask = 0b0010000010000000;
							spu_mask = 0b0000101010101010;
							rsx_mask = 0b1000000000000000;
						}
						else // if (g_cfg.core.thread_scheduler == thread_scheduler_mode::old)
						{
							ppu_mask = 0b1111111100000000;
							spu_mask = ppu_mask;
							rsx_mask = 0b0000000000111100;
						}
						break;
					case 12:
						// 1600, 2600 family, Assign threads 3-12
						ppu_mask = 0b111111000000;
						spu_mask = ppu_mask;
						rsx_mask = 0b000000111100;
						break;
					default:
						break;
					}
				}
				break;
			}
			case 0x19: // Zen3
			{
				// Single-CCX architecture, just disable SMT if wide enough
				// CCX now holds upto 16 threads
				// Lack of hw availability makes testing difficult
				switch (thread_count)
				{
				case 24:
					// 5900X, Use same scheduler as 3900X
					// Unverified on windows, may be worse than just disabling SMT and scheduler
					ppu_mask = 0b111111000000000000000000;
					spu_mask = 0b000000111111000000000000;
					rsx_mask = 0b000000000000111111000000;
					break;
				case 16:
					// 5800X
					if (g_cfg.core.thread_scheduler == thread_scheduler_mode::alt)
					{
						ppu_mask = 0b0000000011110000;
						spu_mask = 0b1111111100000000;
						rsx_mask = 0b0000000000001111;
					}
					else // if (g_cfg.core.thread_scheduler == thread_scheduler_mode::old)
					{
						// Verified by more than one windows user on 16-thread CPU
						ppu_mask = spu_mask = rsx_mask = (0b10101010101010101010101010101010 & all_cores_mask);
					}
					break;
				case 12:
					// 5600X
					if (g_cfg.core.thread_scheduler == thread_scheduler_mode::alt)
					{
						ppu_mask = 0b000000001100;
						spu_mask = 0b111111110000;
						rsx_mask = 0b000000000011;
					}
					else // if (g_cfg.core.thread_scheduler == thread_scheduler_mode::old)
					{
						ppu_mask = spu_mask = rsx_mask = all_cores_mask;
					}
					break;
				default:
					if (thread_count > 24)
					{
						ppu_mask = spu_mask = rsx_mask = (0b10101010101010101010101010101010 & all_cores_mask);
					}
					break;
				}
				break;
			}
			default:
			{
				break;
			}
			}

			switch (group)
			{
			default:
			case thread_class::general:
				return all_cores_mask;
			case thread_class::rsx:
				return rsx_mask;
			case thread_class::ppu:
				return ppu_mask;
			case thread_class::spu:
				return spu_mask;
			}
		}
		case native_core_arrangement::intel_ht:
		{
			if (thread_count >= 12 && g_cfg.core.thread_scheduler == thread_scheduler_mode::alt)
				return (0b10101010101010101010101010101010 & all_cores_mask); // Potentially improves performance by mimicking HT off
			return all_cores_mask;
		}
		}
	}

	return -1;
}

void thread_ctrl::set_native_priority(int priority)
{
#ifdef _WIN32
	HANDLE _this_thread = GetCurrentThread();
	INT native_priority = THREAD_PRIORITY_NORMAL;

	if (priority > 0)
		native_priority = THREAD_PRIORITY_ABOVE_NORMAL;
	if (priority < 0)
		native_priority = THREAD_PRIORITY_BELOW_NORMAL;

	if (!SetThreadPriority(_this_thread, native_priority))
	{
		sig_log.error("SetThreadPriority() failed: %s", fmt::win_error{GetLastError(), nullptr});
	}
#elif defined(__ORBIS__)
    // Orbis schedules fixed priorities. A lowered helper (Log Writer, LLVM PPU/SPU compile workers)
    // can starve forever behind spinning PPU/SPU threads while emulator threads wait for it
    // (0.20 autosave freeze). Ignore lowering; raising (audio) and restoring still work.
    if (priority < 0)
        priority = 0;
    static thread_local utils::ps4::thread_priority native_priority;
    const auto self = scePthreadSelf();
    const int error = native_priority.apply(priority,
        [self](int* value) { return scePthreadGetprio(self, value); },
        [self, priority](int value)
        {
            sig_log.notice("PS4 native priority: request=%d, target=%d (lower values run first)", priority, value);
            return scePthreadSetprio(self, value);
        });
    if (error)
        sig_log.error("PS4: native thread priority request %d failed: 0x%x", priority, static_cast<u32>(error));
#else
	int policy;
	struct sched_param param;

	pthread_getschedparam(pthread_self(), &policy, &param);

	if (priority > 0)
		param.sched_priority = sched_get_priority_max(policy);
	if (priority < 0)
		param.sched_priority = sched_get_priority_min(policy);

	if (int err = pthread_setschedparam(pthread_self(), policy, &param))
	{
		sig_log.error("pthread_setschedparam() failed: %d", err);
	}
#endif
}

u64 thread_ctrl::get_process_affinity_mask()
{
	static const u64 mask = []() -> u64
	{
#ifdef _WIN32
		DWORD_PTR res, _sys;
		if (!GetProcessAffinityMask(GetCurrentProcess(), &res, &_sys))
		{
			sig_log.error("Failed to get process affinity mask.");
			return 0;
		}

		return res;
#else
		// Assume it's called from the main thread (this is a bit shaky)
		return thread_ctrl::get_thread_affinity_mask();
#endif
	}();

	return mask;
}

DECLARE(thread_ctrl::process_affinity_mask) = get_process_affinity_mask();

void thread_ctrl::set_thread_affinity_mask(u64 mask)
{
	sig_log.trace("set_thread_affinity_mask called with mask=0x%x", mask);

#ifdef _WIN32
	HANDLE _this_thread = GetCurrentThread();
	if (!SetThreadAffinityMask(_this_thread, !mask ? process_affinity_mask : mask))
	{
		sig_log.error("Failed to set thread affinity 0x%x: error: %s", mask, fmt::win_error{GetLastError(), nullptr});
	}
#elif __APPLE__
	// Supports only one core
	thread_affinity_policy_data_t policy = { static_cast<integer_t>(std::countr_zero(mask)) };
	thread_port_t mach_thread = pthread_mach_thread_np(pthread_self());
	thread_policy_set(mach_thread, THREAD_AFFINITY_POLICY, reinterpret_cast<thread_policy_t>(&policy), !mask ? 0 : 1);
#elif !defined(ANDROID) && (defined(__linux__) || defined(__DragonFly__) || defined(__FreeBSD__))
	if (!mask)
	{
		// Reset affinity mask
		mask = process_affinity_mask;
	}

	cpu_set_t cs;
	CPU_ZERO(&cs);

	for (u32 core = 0; core < 64u; ++core)
	{
		const u64 shifted = mask >> core;

		if (shifted & 1)
		{
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
			CPU_SET(core, &cs);
#pragma GCC diagnostic pop
		}

		if (shifted <= 1)
		{
			break;
		}
	}

	if (int err = pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cs))
	{
		sig_log.error("Failed to set thread affinity 0x%x: error %d.", mask, err);
	}
#endif
}

u64 thread_ctrl::get_thread_affinity_mask()
{
#ifdef _WIN32
	const u64 res = process_affinity_mask;

	if (DWORD_PTR result = SetThreadAffinityMask(GetCurrentThread(), res))
	{
		if (res != result)
		{
			SetThreadAffinityMask(GetCurrentThread(), result);
		}

		return result;
	}

	sig_log.error("Failed to get thread affinity mask.");
	return 0;
#elif !defined(ANDROID) && (defined(__linux__) || defined(__DragonFly__) || defined(__FreeBSD__))
	cpu_set_t cs;
	CPU_ZERO(&cs);

	if (int err = pthread_getaffinity_np(pthread_self(), sizeof(cpu_set_t), &cs))
	{
		sig_log.error("Failed to get thread affinity mask: error %d.", err);
		return 0;
	}

	u64 result = 0;

	for (u32 core = 0; core < 64u; core++)
	{
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wold-style-cast"
		if (CPU_ISSET(core, &cs))
#pragma GCC diagnostic pop
		{
			result |= 1ull << core;
		}
	}

	if (result == 0)
	{
		sig_log.error("Thread affinity mask is out of u64 range.");
		return 0;
	}

	return result;
#else
	return -1;
#endif
}

std::pair<void*, usz> thread_ctrl::get_thread_stack()
{
#ifdef _WIN32
	ULONG_PTR _min = 0;
	ULONG_PTR _max = 0;
	GetCurrentThreadStackLimits(&_min, &_max);
	const usz ssize = _max - _min;
	const auto saddr = reinterpret_cast<void*>(_min);
#else
	void* saddr = 0;
	usz ssize = 0;
#if defined(__linux__)
	pthread_attr_t attr;
	pthread_getattr_np(pthread_self(), &attr);
	pthread_attr_getstack(&attr, &saddr, &ssize);
#elif defined(__ORBIS__)
	// OpenOrbis declares pthread_attr_get_np and pthread_attr_getstack;
	// pthread_attr_getstackaddr is absent from its public pthread header.
	pthread_attr_t attr;
	ensure(pthread_attr_get_np(pthread_self(), &attr) == 0);
	ensure(pthread_attr_getstack(&attr, &saddr, &ssize) == 0);
	ensure(pthread_attr_destroy(&attr) == 0);
#elif defined(__APPLE__)
	saddr = pthread_get_stackaddr_np(pthread_self());
	ssize = pthread_get_stacksize_np(pthread_self());
#else
	pthread_attr_t attr;
	pthread_attr_get_np(pthread_self(), &attr);
	pthread_attr_getstackaddr(&attr, &saddr);
	pthread_attr_getstacksize(&attr, &ssize);
#endif
#endif
	return {saddr, ssize};
}

u64 thread_ctrl::get_tid()
{
	static thread_local u64 s_tls_tid = []() -> u64
	{
	#ifdef _WIN32
		return GetCurrentThreadId();
	#elif defined(ANDROID)
		return pthread_gettid_np(pthread_self());
	#elif defined(__linux__)
		return syscall(SYS_gettid);
	#elif defined(__APPLE__)
		u64 tid{};
		pthread_threadid_np(nullptr, &tid);
		return tid;
	#elif defined(__ORBIS__)
		return static_cast<u64>(pthread_self());
	#elif defined(__FreeBSD__)
		return pthread_getthreadid_np();
	#else
		return static_cast<u64>(pthread_self());
	#endif
	}();

	return s_tls_tid;
}

bool thread_ctrl::is_main()
{
	return get_tid() == utils::main_tid;
}

usz map_workload(std::string_view thread_name, usz thread_count, usz count, std::function<void(usz)>&& func)
{
	ensure(!!func);

	if (thread_count <= 1)
	{
		for (usz i = 0; i < count; i++)
		{
			func(i);
		}
		return 1;
	}

	atomic_t<u32> num_threads_succeeded {0}; // Check if any thread didn't finish. For example when hitting an exception.

	atomic_t<usz> indexer = 0;
	const auto iterate = [count, &func, &indexer]()
	{
		while (thread_ctrl::state() != thread_state::aborting)
		{
			// Make sure indexer does not exceed count
			const usz index = indexer.fetch_op([count](usz& v)
			{
				if (v < count)
				{
					v++;
					return true;
				}

				return false;
			}).first;

			if (index >= count)
			{
				break;
			}

			func(index);
		}
	};
	named_thread_group workers(thread_name, ::narrow<u32>(thread_count) - 1, [&iterate, &num_threads_succeeded]()
	{
		iterate();
		num_threads_succeeded++;
	});

	iterate();

	workers.join();

	return num_threads_succeeded + 1;
}

usz map_workload(std::string_view thread_name, usz thread_count, std::function<void()>&& func)
{
	ensure(!!func);

	if (thread_count <= 1)
	{
		func();
		return 1;
	}

	atomic_t<u32> num_threads_succeeded {0}; // Check if any thread didn't finish. For example when hitting an exception.

	named_thread_group workers(thread_name, ::narrow<u32>(thread_count) - 1, [&func, &num_threads_succeeded]()
	{
		func();
		num_threads_succeeded++;
	});

	func();

	workers.join();

	return num_threads_succeeded + 1;
}
