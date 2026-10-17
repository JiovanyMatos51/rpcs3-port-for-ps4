#include "stdafx.h"
#include "sys_ppu_thread.h"

#include "Emu/System.h"
#include "Emu/IdManager.h"

#include "Emu/Cell/ErrorCodes.h"
#include "Emu/Cell/PPUThread.h"
#include "Emu/Cell/PPUCallback.h"
#include "Emu/Cell/PPUOpcodes.h"
#include "Emu/Cell/timers.hpp"
#include "Emu/Memory/vm_locking.h"
#include "sys_event.h"
#include "sys_process.h"
#include "sys_mmapper.h"
#include "sys_memory.h"

#include "util/asm.hpp"

#ifdef RPCS3_PS4
#include "util/ps4_memory_governor.hpp" // retry_refused_create, refusal_line_due (also declares ps4_startup_checkpoint)
#endif

#include <thread>

LOG_CHANNEL(sys_ppu_thread);

#ifdef RPCS3_PS4
namespace
{
	// Read once: ppu-start-wait.flag restores the upstream wait of sys_ppu_thread_start for a lower or equal priority thread.
	bool ps4_ppu_start_waits_for_child() noexcept
	{
		static const bool s_wait = []() noexcept
		{
			bool wait = false;

			try
			{
				wait = fs::is_file("/data/rpcs3/ppu-start-wait.flag");
			}
			catch (...)
			{
			}

			ps4_startup_checkpoint(wait ? "PPU threads: sys_ppu_thread_start waits for a lower or equal priority thread to take its entry command (ppu-start-wait.flag)"
			                            : "PPU threads: sys_ppu_thread_start returns at once for a lower or equal priority thread, as lv2 does (ppu-start-wait.flag restores the upstream wait)");
			return wait;
		}();

		return s_wait;
	}
}
#endif

// Simple structure to cleanup previous thread, because can't remove its own thread
struct ppu_thread_cleaner
{
	shared_ptr<named_thread<ppu_thread>> old;

	shared_ptr<named_thread<ppu_thread>> clean(shared_ptr<named_thread<ppu_thread>> ptr)
	{
		return std::exchange(old, std::move(ptr));
	}

	ppu_thread_cleaner() = default;

	ppu_thread_cleaner(const ppu_thread_cleaner&) = delete;

	ppu_thread_cleaner& operator=(const ppu_thread_cleaner&) = delete;

	ppu_thread_cleaner& operator=(thread_state state) noexcept
	{
		reader_lock lock(id_manager::g_mutex);

		if (old)
		{
			// It is detached from IDM now so join must be done explicitly now
			*static_cast<named_thread<ppu_thread>*>(old.get()) = state;
		}

		return *this;
	}
};

void ppu_thread_exit(ppu_thread& ppu, ppu_opcode_t, be_t<u32>*, struct ppu_intrp_func*)
{
	ppu.state += cpu_flag::exit + cpu_flag::wait;

	// Deallocate Stack Area
	ensure(vm::dealloc(ppu.stack_addr, vm::stack) == ppu.stack_size);

	if (auto dct = g_fxo->try_get<lv2_memory_container>())
	{
		dct->free(ppu.stack_size);
	}

	if (ppu.call_history.index)
	{
		ppu_log.notice("Calling history: %s", ppu.call_history);
		ppu.call_history.index = 0;
	}

	if (ppu.syscall_history.index)
	{
		ppu_log.notice("HLE/LV2 history: %s", ppu.syscall_history);
		ppu.syscall_history.index = 0;
	}
}

constexpr u32 c_max_ppu_name_size = 28;

void _sys_ppu_thread_exit(ppu_thread& ppu, u64 errorcode)
{
	ppu.state += cpu_flag::wait;
	u64 writer_mask = 0;

	sys_ppu_thread.trace("_sys_ppu_thread_exit(errorcode=0x%llx)", errorcode);

	ppu_join_status old_status;

	// Avoid cases where cleaning causes the destructor to be called inside IDM lock scope (for performance)
	shared_ptr<named_thread<ppu_thread>> old_ppu;
	{
		lv2_obj::notify_all_t notify;
		lv2_obj::prepare_for_sleep(ppu);

		std::lock_guard lock(id_manager::g_mutex);

		// Get joiner ID
		old_status = ppu.joiner.fetch_op([](ppu_join_status& status)
		{
			if (status == ppu_join_status::joinable)
			{
				// Joinable, not joined
				status = ppu_join_status::zombie;
				return;
			}

			// Set deleted thread status
			status = ppu_join_status::exited;
		});

		if (old_status >= ppu_join_status::max)
		{
			lv2_obj::append(idm::check_unlocked<named_thread<ppu_thread>>(static_cast<u32>(old_status)));
		}

		if (old_status != ppu_join_status::joinable)
		{
			// Remove self ID from IDM, move owning ptr
			old_ppu = g_fxo->get<ppu_thread_cleaner>().clean(idm::withdraw<named_thread<ppu_thread>>(ppu.id, 0, std::false_type{}));
		}

		// Get writers mask (wait for all current writers to quit)
		writer_mask = vm::g_range_lock_bits[1];

		// Unqueue
		lv2_obj::sleep(ppu);
		notify.cleanup();

		// Remove suspend state (TODO)
		ppu.state -= cpu_flag::suspend;
	}

	while (ppu.joiner == ppu_join_status::zombie)
	{
		if (ppu.is_stopped() && ppu.joiner.compare_and_swap_test(ppu_join_status::zombie, ppu_join_status::joinable))
		{
			// Abort
			ppu.state += cpu_flag::again;
			return;
		}

		// Wait for termination
		thread_ctrl::wait_on(ppu.joiner, ppu_join_status::zombie);
	}

	ppu_thread_exit(ppu, {}, nullptr, nullptr);

	if (old_ppu)
	{
		// It is detached from IDM now so join must be done explicitly now
		*old_ppu = thread_state::finished;
	}

	// Need to wait until the current writers finish
	if (ppu.state & cpu_flag::memory)
	{
		for (; writer_mask; writer_mask &= vm::g_range_lock_bits[1])
		{
			busy_wait(200);
		}
	}
}

s32 sys_ppu_thread_yield(ppu_thread& ppu)
{
	ppu.state += cpu_flag::wait;

	sys_ppu_thread.trace("sys_ppu_thread_yield()");

	const s32 success = lv2_obj::yield(ppu) ? CELL_OK : CELL_CANCEL;

	if (success == CELL_CANCEL)
	{
		// Do other work in the meantime
		lv2_obj::notify_all();
	}

	// Return 0 on successful context switch, 1 otherwise
	return success;
}

error_code sys_ppu_thread_join(ppu_thread& ppu, u32 thread_id, vm::ptr<u64> vptr)
{
	lv2_obj::prepare_for_sleep(ppu);

	sys_ppu_thread.trace("sys_ppu_thread_join(thread_id=0x%x, vptr=*0x%x)", thread_id, vptr);

	if (thread_id == ppu.id)
	{
		return CELL_EDEADLK;
	}

	auto thread = idm::get<named_thread<ppu_thread>>(thread_id, [&, notify = lv2_obj::notify_all_t()](ppu_thread& thread) -> CellError
	{
		CellError result = thread.joiner.atomic_op([&](ppu_join_status& value) -> CellError
		{
			switch (value)
			{
			case ppu_join_status::joinable:
				value = ppu_join_status{ppu.id};
				return {};
			case ppu_join_status::zombie:
				value = ppu_join_status::exited;
				return CELL_EAGAIN;
			case ppu_join_status::exited:
				return CELL_ESRCH;
			case ppu_join_status::detached:
			default:
				return CELL_EINVAL;
			}
		});

		if (!result)
		{
			lv2_obj::prepare_for_sleep(ppu);
			lv2_obj::sleep(ppu);
		}

		notify.cleanup();
		return result;
	});

	if (!thread)
	{
		return CELL_ESRCH;
	}

	if (thread.ret && thread.ret != CELL_EAGAIN)
	{
		return thread.ret;
	}

	if (thread.ret == CELL_EAGAIN)
	{
		// Notify thread if waiting for a joiner
		thread->joiner.notify_one();
	}

	// Wait for cleanup
	(*thread.ptr)();

	if (thread->joiner != ppu_join_status::exited)
	{
		// Thread aborted, log it later
		ppu.state += cpu_flag::again;
		return {};
	}

	static_cast<void>(ppu.test_stopped());

	// Get the exit status from the register
	const u64 vret = thread->gpr[3];

	if (thread.ret == CELL_EAGAIN)
	{
		// Cleanup
		ensure(idm::remove_verify<named_thread<ppu_thread>>(thread_id, std::move(thread.ptr)));
	}

	if (!vptr)
	{
		return not_an_error(CELL_EFAULT);
	}

	*vptr = vret;
	return CELL_OK;
}

error_code sys_ppu_thread_detach(ppu_thread& ppu, u32 thread_id)
{
	ppu.state += cpu_flag::wait;

	sys_ppu_thread.trace("sys_ppu_thread_detach(thread_id=0x%x)", thread_id);

	CellError result = CELL_ESRCH;

	auto [ptr, _] = idm::withdraw<named_thread<ppu_thread>>(thread_id, [&](ppu_thread& thread)
	{
		result = thread.joiner.atomic_op([](ppu_join_status& value) -> CellError
		{
			switch (value)
			{
			case ppu_join_status::joinable:
				value = ppu_join_status::detached;
				return {};
			case ppu_join_status::detached:
				return CELL_EINVAL;
			case ppu_join_status::zombie:
				value = ppu_join_status::exited;
				return CELL_EAGAIN;
			case ppu_join_status::exited:
				return CELL_ESRCH;
			default:
				return CELL_EBUSY;
			}
		});

		// Remove ID on EAGAIN
		return result != CELL_EAGAIN;
	});

	if (result)
	{
		if (result == CELL_EAGAIN)
		{
			// Join and notify thread (it is detached from IDM now so it must be done explicitly now)
			*ptr = thread_state::finished;
			return CELL_OK;
		}

		return result;
	}

	return CELL_OK;
}

error_code sys_ppu_thread_get_join_state(ppu_thread& ppu, vm::ptr<s32> isjoinable)
{
	sys_ppu_thread.trace("sys_ppu_thread_get_join_state(isjoinable=*0x%x)", isjoinable);

	if (!isjoinable)
	{
		return CELL_EFAULT;
	}

	*isjoinable = ppu.joiner != ppu_join_status::detached;
	return CELL_OK;
}

error_code sys_ppu_thread_set_priority(ppu_thread& ppu, u32 thread_id, s32 prio)
{
	ppu.state += cpu_flag::wait;

	sys_ppu_thread.trace("sys_ppu_thread_set_priority(thread_id=0x%x, prio=%d)", thread_id, prio);

	if (prio < (g_ps3_process_info.debug_or_root() ? -512 : 0) || prio > 3071)
	{
		return CELL_EINVAL;
	}

	if (thread_id == ppu.id)
	{
		// Fast path for self
		if (ppu.prio.load().prio != prio)
		{
			lv2_obj::set_priority(ppu, prio);
		}

		return CELL_OK;
	}

	const auto thread = idm::check<named_thread<ppu_thread>>(thread_id, [&, notify = lv2_obj::notify_all_t()](ppu_thread& thread)
	{
		lv2_obj::set_priority(thread, prio);
	});

	if (!thread)
	{
		return CELL_ESRCH;
	}

	return CELL_OK;
}

error_code sys_ppu_thread_get_priority(ppu_thread& ppu, u32 thread_id, vm::ptr<s32> priop)
{
	ppu.state += cpu_flag::wait;

	sys_ppu_thread.trace("sys_ppu_thread_get_priority(thread_id=0x%x, priop=*0x%x)", thread_id, priop);
	u32 prio{};

	if (thread_id == ppu.id)
	{
		// Fast path for self
		for (; !ppu.is_stopped(); std::this_thread::yield())
		{
			if (reader_lock lock(lv2_obj::g_mutex); cpu_flag::suspend - ppu.state)
			{
				prio = ppu.prio.load().prio;
				break;
			}

			ppu.check_state();
			ppu.state += cpu_flag::wait;
		}

		ppu.check_state();
		*priop = prio;
		return CELL_OK;
	}

	for (; !ppu.is_stopped(); std::this_thread::yield())
	{
		bool check_state = false;
		const auto thread = idm::check<named_thread<ppu_thread>>(thread_id, [&](ppu_thread& thread)
		{
			if (reader_lock lock(lv2_obj::g_mutex); cpu_flag::suspend - ppu.state)
			{
				prio = thread.prio.load().prio;
			}
			else
			{
				check_state = true;
			}
		});

		if (check_state)
		{
			ppu.check_state();
			ppu.state += cpu_flag::wait;
			continue;
		}

		if (!thread)
		{
			return CELL_ESRCH;
		}

		ppu.check_state();
		*priop = prio;
		break;
	}

	return CELL_OK;
}

error_code sys_ppu_thread_get_stack_information(ppu_thread& ppu, vm::ptr<sys_ppu_thread_stack_t> sp)
{
	sys_ppu_thread.trace("sys_ppu_thread_get_stack_information(sp=*0x%x)", sp);

	sp->pst_addr = ppu.stack_addr;
	sp->pst_size = ppu.stack_size;

	return CELL_OK;
}

error_code sys_ppu_thread_stop(ppu_thread& ppu, u32 thread_id)
{
	ppu.state += cpu_flag::wait;

	sys_ppu_thread.todo("sys_ppu_thread_stop(thread_id=0x%x)", thread_id);

	if (!g_ps3_process_info.has_root_perm())
	{
		return CELL_ENOSYS;
	}

	const auto thread = idm::check<named_thread<ppu_thread>>(thread_id, [](named_thread<ppu_thread>&) {});

	if (!thread)
	{
		return CELL_ESRCH;
	}

	return CELL_OK;
}

error_code sys_ppu_thread_restart(ppu_thread& ppu)
{
	ppu.state += cpu_flag::wait;

	sys_ppu_thread.todo("sys_ppu_thread_restart()");

	if (!g_ps3_process_info.has_root_perm())
	{
		return CELL_ENOSYS;
	}

	return CELL_OK;
}

error_code _sys_ppu_thread_create(ppu_thread& ppu, vm::ptr<u64> thread_id, vm::ptr<ppu_thread_param_t> param, u64 arg, u64 unk, s32 prio, u64 _stacksz, u64 flags, vm::cptr<char> threadname)
{
	ppu.state += cpu_flag::wait;

	sys_ppu_thread.warning("_sys_ppu_thread_create(thread_id=*0x%x, param=*0x%x, arg=0x%llx, unk=0x%llx, prio=%d, stacksize=0x%llx, flags=0x%llx, threadname=*0x%x)",
		thread_id, param, arg, unk, prio, _stacksz, flags, threadname);

	// thread_id is checked for null in stub -> CELL_ENOMEM
	// unk is set to 0 in sys_ppu_thread_create stub

	if (!param || !param->entry)
	{
		return CELL_EFAULT;
	}

	if (prio < (g_ps3_process_info.debug_or_root() ? -512 : 0) || prio > 3071)
	{
		return CELL_EINVAL;
	}

	if ((flags & 3) == 3) // Check two flags: joinable + interrupt not allowed
	{
		return CELL_EPERM;
	}

	const ppu_func_opd_t entry = param->entry.opd();
	const u32 tls = param->tls;

	// Compute actual stack size and allocate
	// 0 and UINT64_MAX both convert to 4096
	const u64 stack_size = FN(x ? x : 4096)(utils::align<u64>(_stacksz, 4096));

	auto& dct = g_fxo->get<lv2_memory_container>();

	// Try to obtain "physical memory" from the default container
	if (!dct.take(stack_size))
	{
		return {CELL_ENOMEM, dct.size - dct.used};
	}

	const vm::addr_t stack_base{vm::alloc(static_cast<u32>(stack_size), vm::stack, 4096)};

	if (!stack_base)
	{
		dct.free(stack_size);
		return CELL_ENOMEM;
	}

	std::string ppu_name;

	if (threadname)
	{
		constexpr u32 max_size = c_max_ppu_name_size - 1; // max size excluding null terminator

		if (!vm::read_string(threadname.addr(), max_size, ppu_name, true))
		{
			dct.free(stack_size);
			return CELL_EFAULT;
		}
	}

#ifdef RPCS3_PS4
	// 0.29: set when the console refused the host thread (as opposed to a full IDM), for the log line below
	bool ps4_host_thread_refused = false;
#endif

	const auto make_ppu = [&]() -> stx::shared_ptr<named_thread<ppu_thread>>
	{
		ppu_thread_params p;
		p.stack_addr = stack_base;
		p.stack_size = static_cast<u32>(stack_size);
		p.tls_addr = tls;
		p.entry = entry;
		p.arg0 = arg;
		p.arg1 = unk;

#ifdef RPCS3_PS4
		// 0.29: a host thread the console refuses (thread resources exhausted, SCE EAGAIN) becomes the CELL_EAGAIN
		// below, what LV2 reports for exhausted thread resources, instead of start()'s retries under idm's lock and an
		// emulator exit (Demon's Souls 0.27, _libsail-control). ps4_try_start() never sleeps or retries. The stand-by
		// object never ran and is not in the IDM yet, so destroying it here waits for nothing (~ppu_thread is empty,
		// cpu_thread registers itself only once its host thread runs).
		auto ppu_ptr = stx::make_shared<named_thread<ppu_thread>>(stx::launch_retainer{}, p, ppu_name, prio, 1 - static_cast<int>(flags & 3));

		if (!ppu_ptr->ps4_try_start())
		{
			ps4_host_thread_refused = true;
			return {};
		}

		return ppu_ptr;
#else
		return stx::make_shared<named_thread<ppu_thread>>(p, ppu_name, prio, 1 - static_cast<int>(flags & 3));
#endif
	};

#ifdef RPCS3_PS4
	// 0.39: a refused host thread gets up to 5 tries within 5 s (each after up to 50 ms of waiting for a thread slot to
	// come back), outside idm's lock, before CELL_EAGAIN; games do not expect EAGAIN below their own limits.
	const u64 ps4_since_us = utils::ps4::governor::detail::now_us();
	u32 ps4_tries = 1;
	u32 tid = idm::import<named_thread<ppu_thread>>(make_ppu);

	while (!tid && ps4_host_thread_refused &&
		utils::ps4::governor::retry_refused_create(ps4_tries, 5, ps4_since_us, [&]() noexcept { return ppu.is_stopped() || Emu.IsStopped(); }))
	{
		ps4_host_thread_refused = false;
		ps4_tries++;
		tid = idm::import<named_thread<ppu_thread>>(make_ppu);
	}

	if (tid && ps4_tries > 1)
	{
		const std::string line = fmt::format("PS4 threads: pthread_create refusal for PPU thread \"%s\" recovered after %u ms (%u tries)", ppu_name,
			(utils::ps4::governor::detail::now_us() - ps4_since_us) / 1000, ps4_tries);
		sys_ppu_thread.warning("%s", line);
		ps4_startup_checkpoint(line.c_str());
	}
#else
	const u32 tid = idm::import<named_thread<ppu_thread>>(make_ppu);
#endif

	if (!tid)
	{
		vm::dealloc(stack_base);
		dct.free(stack_size);
#ifdef RPCS3_PS4
		if (ps4_host_thread_refused)
		{
			// 0.39: the first 4 lines of a name, then every power of two (GT6 retried one name 15,193 times)
			if (const u32 n = utils::ps4::governor::refusal_line_due(ppu_name))
			{
				sys_ppu_thread.error("_sys_ppu_thread_create(): PS4 refused the host thread for \"%s\" (thread resources exhausted, %u tries in %u ms, refusal #%u of this name); returning CELL_EAGAIN to the game",
					ppu_name, ps4_tries, (utils::ps4::governor::detail::now_us() - ps4_since_us) / 1000, n);
			}
		}
#endif
		return CELL_EAGAIN;
	}

	sys_ppu_thread.warning("_sys_ppu_thread_create(): Thread \"%s\" created (id=0x%x, func=*0x%x, rtoc=0x%x, user-tls=0x%x)", ppu_name, tid, entry.addr, entry.rtoc, tls);

	ppu.check_state();
	*thread_id = tid;
	return CELL_OK;
}

error_code sys_ppu_thread_start(ppu_thread& ppu, u32 thread_id)
{
	ppu.state += cpu_flag::wait;

	sys_ppu_thread.trace("sys_ppu_thread_start(thread_id=0x%x)", thread_id);

	bool is_lower_prio = false;

	const auto thread = idm::get<named_thread<ppu_thread>>(thread_id, [&, notify = lv2_obj::notify_all_t()](ppu_thread& thread) -> CellError
	{
		if (!thread.state.test_and_reset(cpu_flag::stop))
		{
			// Already started
			return CELL_EBUSY;
		}

		is_lower_prio = thread.is_lower_priority_than(ppu);
		ensure(lv2_obj::awake(&thread));

		thread.cmd_list
		({
			{ppu_cmd::entry_call, 0},
		});

		return {};
	});

	if (!thread)
	{
		return CELL_ESRCH;
	}

	if (thread.ret)
	{
		return thread.ret;
	}

	thread->cmd_notify.store(1);
	thread->cmd_notify.notify_one();

	if (is_lower_prio)
	{
		// The new thread has higher priority: wait until it has started and stops running, blocked or preempted (bounded)
		// A preempted thread stays ahead of the caller in the scheduler queue, so there is no need to wait for it to block
		const auto is_running = [&]()
		{
			return lv2_obj::ppu_state(thread.ptr.get()).first == PPU_THREAD_STATUS_ONPROC;
		};

		// Keep waiting when the caller is suspended: another thread leaving its hardware thread may resume it before the new thread starts
		for (const u64 start = get_system_time(); (thread->cmd_queue.size() || is_running()) && !ppu.is_stopped() && get_system_time() - start < 5000;)
		{
			std::this_thread::yield();
		}
	}
	else
	{
#ifdef RPCS3_PS4
		// lv2 does not wait here. The wait let the new thread run its first instructions before the creator stored what
		// they read (Plants vs. Zombies 'UnlockTrophy Thread': access violation at 0x59c, then a frozen emulation).
		// The new thread still starts at once (cmd_notify above).
		if (!ps4_ppu_start_waits_for_child())
		{
			return CELL_OK;
		}
#endif
		// The new thread has lower or equal priority: if it got a free hardware thread, wait until it has taken the entry command (bounded)
		for (const u64 start = get_system_time(); thread->cmd_queue.size() && cpu_flag::suspend - thread->state && cpu_flag::suspend - ppu.state && !ppu.is_stopped() && get_system_time() - start < 5000;)
		{
			std::this_thread::yield();
		}
	}

	return CELL_OK;
}

error_code sys_ppu_thread_rename(ppu_thread& ppu, u32 thread_id, vm::cptr<char> name)
{
	ppu.state += cpu_flag::wait;

	sys_ppu_thread.warning("sys_ppu_thread_rename(thread_id=0x%x, name=*0x%x)", thread_id, name);

	const auto thread = idm::get_unlocked<named_thread<ppu_thread>>(thread_id);

	if (!thread)
	{
		return CELL_ESRCH;
	}

	if (!name)
	{
		return CELL_EFAULT;
	}

	constexpr u32 max_size = c_max_ppu_name_size - 1; // max size excluding null terminator

	// Make valid name
	std::string out_str;
	if (!vm::read_string(name.addr(), max_size, out_str, true))
	{
		return CELL_EFAULT;
	}

	auto _name = make_single<std::string>(std::move(out_str));

	// thread_ctrl name is not changed (TODO)
	sys_ppu_thread.warning("sys_ppu_thread_rename(): Thread renamed to \"%s\"", *_name);
	thread->ppu_tname.store(std::move(_name));
	thread_ctrl::set_name(*thread, thread->thread_name); // TODO: Currently sets debugger thread name only for local thread

	return CELL_OK;
}

error_code sys_ppu_thread_recover_page_fault(ppu_thread& ppu, u32 thread_id)
{
	ppu.state += cpu_flag::wait;

	sys_ppu_thread.warning("sys_ppu_thread_recover_page_fault(thread_id=0x%x)", thread_id);

	const auto thread = idm::get_unlocked<named_thread<ppu_thread>>(thread_id);

	if (!thread)
	{
		return CELL_ESRCH;
	}

	return mmapper_thread_recover_page_fault(thread.get());
}

error_code sys_ppu_thread_get_page_fault_context(ppu_thread& ppu, u32 thread_id, vm::ptr<sys_ppu_thread_icontext_t> ctxt)
{
	ppu.state += cpu_flag::wait;

	sys_ppu_thread.todo("sys_ppu_thread_get_page_fault_context(thread_id=0x%x, ctxt=*0x%x)", thread_id, ctxt);

	const auto thread = idm::get_unlocked<named_thread<ppu_thread>>(thread_id);

	if (!thread)
	{
		return CELL_ESRCH;
	}

	// We can only get a context if the thread is being suspended for a page fault.
	auto& pf_events = g_fxo->get<page_fault_event_entries>();
	reader_lock lock(pf_events.pf_mutex);

	const auto evt = pf_events.events.find(thread.get());
	if (evt == pf_events.events.end())
	{
		return CELL_EINVAL;
	}

	// TODO: Fill ctxt with proper information.

	return CELL_OK;
}
