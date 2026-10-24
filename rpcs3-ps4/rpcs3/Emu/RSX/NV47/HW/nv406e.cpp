#include "stdafx.h"
#include "nv406e.h"
#include "nv47_sync.hpp"

#include "Emu/RSX/RSXThread.h"
#include "Emu/system_config.h"

#include "context_accessors.define.h"

#ifdef RPCS3_PS4
#include "Emu/Cell/SPUThread.h"
#include "util/ps4_hr_wait.hpp"
#include "util/ps4_memory_governor.hpp"

#include <orbis/libkernel.h>
#include <thread>

// Utilities/Thread.cpp (__ORBIS__): native priority of the guest/emulator threads (700 on the console).
int ps4_thread_base_native_priority() noexcept;

void ps4_startup_checkpoint(const char* stage);

namespace rsx
{
	// SPU work items and PPU/SPU compiles in progress, packed: any change is compile progress for the TDR clock.
	static u64 ps4_compile_progress_mark() noexcept
	{
		return (u64{spu_thread::g_spu_work_count.load()} << 32) | utils::ps4::governor::active_compiles();
	}

	// One line per NV406E timeout: startup.log for the first 32 of the process, RPCS3.log always. `zcull`: whether
	// ZCULL still locks the label's page and the report queue depth (thread::ps4_label_zcull_text).
	static void ps4_note_semaphore_timeout(u32 addr, u64 waited_ms, u64 extension_ms, const std::string& zcull) noexcept
	{
		static atomic_t<u32> s_notes{0};

		const std::string line = fmt::format("RSX: semaphore 0x%x TIMED OUT after %llu ms (compile extension %llu ms); %s", addr, waited_ms, extension_ms, zcull);
		rsx_log.error("PS4 %s", line);

		if (s_notes.fetch_add(1) < 32)
		{
			ps4_startup_checkpoint(line.c_str());
		}
	}

	// 0.29: one NV406E semaphore wait on the RSX thread. Sets the STALL wait reason, counts the wait for the 10 s
	// 'RSX gameplay' line and owns the temporary priority step-down: the destructor restores the raised priority
	// on every exit (match, test_stopped return, TDR break).
	// 0.34: wait_once is the default wait (rsx-sema-spin.flag restores the 0.33 spin) and ends in a sleep phase.
	// The wait also feeds the label ring and the startup.log note of waits over 1 s.
	class ps4_semaphore_wait
	{
		ps4_rsx_wait_scope m_reason{ps4_wait_semaphore};
		const u64 m_start = get_system_time();
		const u32 m_sleep_us = ps4_rsx_sema_sleep_us();
		u64 m_phase_start = m_start;
		u64 m_flushes = ps4_rsx_flush_services();
		int m_saved_prio = -1;
		bool m_extended = false;

	public:
		ps4_semaphore_wait(u32 addr, u32 expected, u32 get) noexcept
		{
			ps4_label_ring_note(ps4_label_wait, addr, expected, get);
		}

		ps4_semaphore_wait(const ps4_semaphore_wait&) = delete;
		ps4_semaphore_wait& operator=(const ps4_semaphore_wait&) = delete;

		~ps4_semaphore_wait()
		{
			if (m_saved_prio >= 0)
			{
				scePthreadSetprio(scePthreadSelf(), m_saved_prio);
			}

			g_ps4_rsx_counters.sema_waits++;
			g_ps4_rsx_counters.sema_wait_us += get_system_time() - m_start;
		}

		// The wait outlived the TDR setting and goes on (counted once per wait).
		void extended() noexcept
		{
			if (!m_extended)
			{
				m_extended = true;
				g_ps4_rsx_counters.sema_extended++;
			}
		}

		// The wait ended with the expected value or by the TDR; not called on the test_stopped return.
		void finish(u32 addr, u32 expected, u32 value, u32 get, bool timed_out) noexcept
		{
			ps4_label_ring_note(timed_out ? ps4_label_timeout : ps4_label_acquired, addr, timed_out ? value : expected, get);

			const u64 now = get_system_time();

			if (timed_out)
			{
				g_ps4_rsx_counters.sema_timeouts++;
				g_ps4_rsx_counters.sema_timeout_at.release(now);
			}

			if (now - m_start > 1'000'000)
			{
				ps4_note_semaphore(addr, expected, value, get, now - m_start, spu_thread::g_spu_work_count.load(),
					utils::ps4::governor::active_compiles(), timed_out);
			}
		}

		// No umwait/mwaitx on Jaguar: utils::spin_on_cacheline_once() is a bare yield there, and a yield at the
		// RSX's raised fixed priority never hands the core to the PPU/SPU thread that must release the semaphore
		// (priority inversion when every core is busy). Spin for 200 us, then drop to the guest base priority
		// (round-robin among equals since 0.26) so the yield can run that thread. Direct scePthread calls:
		// thread_ctrl::set_native_priority logs every change and keeps its own restore state.
		// 0.34: after m_sleep_us, sleep one kernel tick per poll instead of yielding: such a wait is a
		// frame-long wait and the 0.33 spin held a whole core for it.
		void wait_once() noexcept
		{
			const u64 now = get_system_time();

			// A serviced flush request means a guest thread is faulting on RSX memory and waits for each service
			// (on_semaphore_acquire_wait): poll without sleeping for another window while that goes on.
			if (const u64 flushes = ps4_rsx_flush_services(); flushes != m_flushes)
			{
				m_flushes = flushes;
				m_phase_start = now;
			}

			if (m_sleep_us && now - m_phase_start >= m_sleep_us)
			{
				if (m_saved_prio >= 0)
				{
					// Back to the raised priority while sleeping: a sleeping thread holds no core, and the raised
					// priority runs each wake-up at once instead of queueing it behind busy guest threads. The next
					// yield phase (after a flush request) steps down again.
					scePthreadSetprio(scePthreadSelf(), m_saved_prio);
					m_saved_prio = -1;
				}

				g_ps4_rsx_counters.sema_sleeps++;
				// sceKernelUsleep rounds up to the ~1 ms scheduler tick; the opt-in HR timer wakes ~30 us late.
				if (utils::ps4::hr_wait_enabled())
					utils::ps4::hr_wait_us(100);
				else
					sceKernelUsleep(50);
				return;
			}

			if (m_saved_prio == -1 && now - m_start >= 200)
			{
				int prio = 0;
				const auto self = scePthreadSelf();
				// 0.29: step down to the process base priority itself, not to prio + 8. If the raise did not
				// take effect (or the step ever changes), prio + 8 would put the RSX below the guest threads, which
				// is the 0.25 starvation (RSX stuck behind spinning PPU/SPU threads).
				const int base = ::ps4_thread_base_native_priority();

				if (!ps4_rsx_priority_elevated())
				{
					// rsx-normal-priority.flag: already at the guest priority, a yield reaches the releasing thread.
					m_saved_prio = -3;
				}
				else if (scePthreadGetprio(self, &prio) != 0 || prio < 256 || prio > 767 || base < 256 || base > 767)
				{
					// Unknown priority: never retry the syscalls on every poll of this wait; keep spinning.
					m_saved_prio = -2;
				}
				else if (prio >= base)
				{
					// Not above the guest threads: a yield already reaches the releasing thread (round-robin).
					m_saved_prio = -3;
				}
				else if (scePthreadSetprio(self, base) == 0)
				{
					m_saved_prio = prio;
					g_ps4_rsx_counters.sema_lowered++;
				}
				else
				{
					// Could not step down: never retry the syscalls on every poll of this wait.
					m_saved_prio = -2;
				}
			}

			if (m_saved_prio >= 0 || m_saved_prio == -3)
				std::this_thread::yield();
			else
				utils::pause();
		}
	};
}
#endif

namespace rsx
{
	namespace nv406e
	{
		void set_reference(context* ctx, u32 /*reg*/, u32 arg)
		{
			RSX(ctx)->sync();

#ifdef RPCS3_PS4
			// 0.37: the guest polls REF as a fence; earlier draws read their vertex sources first (RSX copy worker)
			ps4_copy_worker_barrier();
#endif

			// Write ref+get (get will be written again with the same value at command end)
			auto& dma = *vm::_ptr<RsxDmaControl>(RSX(ctx)->dma_address);
			dma.get.release(RSX(ctx)->fifo_ctrl->get_pos());
			dma.ref.store(arg);
		}

		void semaphore_acquire(context* ctx, u32 /*reg*/, u32 arg)
		{
			RSX(ctx)->sync_point_request.release(true);
			const u32 addr = get_address(REGS(ctx)->semaphore_offset_406e(), REGS(ctx)->semaphore_context_dma_406e());

			// Syncronization point, may be associated with memory changes without actually changing addresses
			RSX(ctx)->m_graphics_state |= rsx::pipeline_state::fragment_program_needs_rehash;

#ifdef RPCS3_PS4
			// The label page may be one ZCULL keeps without access (see ps4_rsx_label_sudo); off: the guest view
			const bool ps4_label_sudo = ps4_rsx_label_sudo();
			const auto& sema = ps4_label_sudo ? *vm::get_super_ptr<RsxSemaphore>(addr) : vm::_ref<RsxSemaphore>(addr);
			const auto& atomic_sema = ps4_label_sudo ? *vm::get_super_ptr<atomic_t<RsxSemaphore>>(addr) : vm::_ref<atomic_t<RsxSemaphore>>(addr);
#else
			const auto& sema = vm::_ref<RsxSemaphore>(addr);
			const auto& atomic_sema = vm::_ref<atomic_t<RsxSemaphore>>(addr);
#endif

			if (sema == arg)
			{
#ifdef RPCS3_PS4
				ps4_label_ring_note(ps4_label_acquire, addr, arg, RSX(ctx)->fifo_ctrl->get_pos());
#endif
				// Flip semaphore doesnt need wake-up delay
				if (addr != RSX(ctx)->label_addr + 0x10)
				{
					RSX(ctx)->flush_fifo();
					RSX(ctx)->fifo_wake_delay(2);
				}

				return;
			}
			else
			{
				RSX(ctx)->flush_fifo();
			}

			u64 start = get_system_time();
			u64 last_check_val = start;
#ifdef RPCS3_PS4
			ps4_semaphore_wait ps4_wait(addr, arg, RSX(ctx)->fifo_ctrl->get_pos());
			const bool ps4_yield_mode = ps4_rsx_sema_yield();
			// 0.34: own TDR clocks; 'start' keeps feeding idle_time below. ps4_clock restarts while LLVM compiles.
			u64 ps4_first = start;
			u64 ps4_clock = start;
			bool ps4_timed_out = false;
			// Compile counters at the previous check: the clock restarts only when they change (progress)
			u64 ps4_compile_mark = ps4_compile_progress_mark();
			// Deferred ZCULL report service of this wait (rsx::thread::ps4_service_reports_while_blocked).
			bool ps4_rep_hinted = false, ps4_rep_forced = false;
#endif

			while (sema != arg)
			{
				if (RSX(ctx)->test_stopped())
				{
					RSX(ctx)->state += cpu_flag::again;
					return;
				}

				if (const auto tdr = static_cast<u64>(g_cfg.video.driver_recovery_timeout))
				{
					const u64 current = get_system_time();

					if (current - last_check_val > 20'000)
					{
						// Suspicious amnount of time has passed
						// External pause such as debuggers' pause or operating system sleep may have taken place
						// Ignore it
						start += current - last_check_val;
#ifdef RPCS3_PS4
						ps4_first += current - last_check_val;
						ps4_clock += current - last_check_val;
#endif
					}

					last_check_val = current;

#ifdef RPCS3_PS4
					// 0.34: PS3 hardware never abandons a semaphore. On Jaguar the PPU/SPU code that releases it may
					// still be interpreted while LLVM compiles it, for seconds on a cold cache (God of War Ascension
					// timed out twice in such a burst, then the RSX ran ahead and the game deadlocked). The limit is
					// at least 3 s (Jaguar equivalent of the PC's 1 s TDR). 0.37: the clock restarts only while the
					// SPU/PPU compile counters change (a stuck compile or a lost release no longer extends the wait),
					// and no wait outlives 10 s.
					if (const u64 mark = ps4_compile_progress_mark(); mark != ps4_compile_mark)
					{
						ps4_compile_mark = mark;
						ps4_clock = current;
					}

					if ((current - ps4_clock) > std::max<u64>(tdr, 3'000'000) || (current - ps4_first) > 10'000'000)
#else
					if ((current - start) > tdr)
#endif
					{
						// If longer than driver timeout force exit
						rsx_log.error("nv406e::semaphore_acquire has timed out. semaphore_address=0x%X", addr);
#ifdef RPCS3_PS4
						ps4_timed_out = true;
						ps4_note_semaphore_timeout(addr, (current - ps4_first) / 1000, (ps4_clock - ps4_first) / 1000, RSX(ctx)->ps4_label_zcull_text(addr));
#endif
						break;
					}

#ifdef RPCS3_PS4
					if ((current - ps4_first) > tdr)
					{
						ps4_wait.extended();
					}
#endif
				}

				if (RSX(ctx)->external_interrupt_lock ||
					(RSX(ctx)->state & (cpu_flag::dbg_global_pause + cpu_flag::exit)) == cpu_flag::dbg_global_pause)
				{
					RSX(ctx)->cpu_wait({});
					continue;
				}

				RSX(ctx)->on_semaphore_acquire_wait();

#ifdef RPCS3_PS4
				// The awaited label may be written by work that a deferred ZCULL report starts (a report into a
				// SPURS workload flag): write those reports while blocked, as the RSX back end does.
				if (const u64 waited = get_system_time() - ps4_first; waited >= 200)
				{
					RSX(ctx)->ps4_service_reports_while_blocked(addr, waited, ps4_rep_hinted, ps4_rep_forced);
				}

				if (ps4_yield_mode)
				{
					// 0.34 default (rsx-sema-spin.flag restores the spin below): see ps4_semaphore_wait::wait_once.
					ps4_wait.wait_once();
					continue;
				}
#endif
				// Wait until the value changes or until 100us pass.
				utils::spin_on_cacheline_once(atomic_sema, sema, 100);
			}

#ifdef RPCS3_PS4
			ps4_wait.finish(addr, arg, sema, RSX(ctx)->fifo_ctrl->get_pos(), ps4_timed_out);
#endif
			RSX(ctx)->fifo_wake_delay();
			RSX(ctx)->performance_counters.idle_time += (get_system_time() - start);
		}

		void semaphore_release(context* ctx, u32 reg, u32 arg)
		{
			const u32 offset = REGS(ctx)->semaphore_offset_406e();

			if (offset % 4)
			{
				rsx_log.warning("NV406E semaphore release is using unaligned semaphore, ignoring. (offset=0x%x)", offset);
				return;
			}

			const u32 ctxt = REGS(ctx)->semaphore_context_dma_406e();

			// By avoiding doing this on flip's semaphore release
			// We allow last gcm's registers reset to occur in case of a crash
			if (const bool is_flip_sema = (offset == 0x10 && ctxt == CELL_GCM_CONTEXT_DMA_SEMAPHORE_R);
				!is_flip_sema)
			{
				RSX(ctx)->sync_point_request.release(true);
			}

			const u32 addr = get_address(offset, ctxt);

			// TODO: Check if possible to write on reservations
			if (RSX(ctx)->label_addr >> 28 != addr >> 28)
			{
				rsx_log.error("NV406E semaphore unexpected address. Please report to the developers. (offset=0x%x, addr=0x%x)", offset, addr);
				RSX(ctx)->recover_fifo();
				return;
			}

			if (addr == RSX(ctx)->device_addr + 0x30 && !arg)
			{
				// HW flip synchronization related, 1 is not written without display queue command (TODO: make it behave as real hw)
				arg = 1;
			}

#ifdef RPCS3_PS4
			// Label releases sync the ZCULL queue (and may flush): own bucket in the 10 s line.
			const ps4_rsx_phase_scope ps4_phase(g_ps4_rsx_phase.release_ticks);
#endif
			util::write_gcm_label<false, true>(ctx, reg, addr, arg);
		}
	}
}
