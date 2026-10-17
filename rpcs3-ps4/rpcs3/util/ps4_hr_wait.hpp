#pragma once

#include "util/types.hpp"

#include <algorithm>
#include <atomic>

#ifdef __ORBIS__
#include <orbis/libkernel.h>
#else
#include <chrono>
#include <thread>
#endif

namespace utils::ps4
{
#ifdef __ORBIS__
	// libkernel's high-resolution equeue timer: a wait ends ~30 us after its deadline, where sceKernelUsleep and
	// sceKernelNanosleep round up to the ~1 ms scheduler tick. Opt-in (/data/rpcs3/hr-wait.flag), and only after the
	// boot timer probe (ps4_main.cpp) showed that one equeue serves many waits without stray events or errors. The
	// probe resolves the functions at run time: sceKernelAddHRTimerEvent is commented out in the SDK header, so a
	// static import could break the load on a firmware without it.
	namespace hr_wait_detail
	{
		// FreeBSD amd64 struct timespec and struct kevent
		struct kernel_timespec
		{
			s64 sec;
			s64 nsec;
		};

		struct kernel_event
		{
			u64 ident;
			s16 filter;
			u16 flags;
			u32 fflags;
			s64 data;
			void* udata;
		};

		static_assert(sizeof(kernel_timespec) == 16 && sizeof(kernel_event) == 32);

		struct functions
		{
			s32 (*create)(OrbisKernelEqueue*, const char*);
			s32 (*destroy)(OrbisKernelEqueue);
			s32 (*wait)(OrbisKernelEqueue, void*, s32, s32*, OrbisKernelUseconds*);
			s32 (*add_hr)(OrbisKernelEqueue, s32, kernel_timespec*, void*);
		};

		// Written once by hr_wait_configure, before g_enabled is published
		inline functions g_functions{};
		inline std::atomic<bool> g_enabled{false};
		inline std::atomic<u64> g_waits{0};
		inline std::atomic<u64> g_thread_failures{0};
		inline std::atomic<s32> g_last_error{0};
		inline std::atomic<u64> g_timeouts{0}; // 0.39: waits whose event did not come by the deadline + 2 ms (equeue kept)

		// SCE ETIMEDOUT of sceKernelWaitEqueue
		constexpr s32 error_timed_out = static_cast<s32>(0x8002003cu);
		// Timed-out waits in a row after which a thread leaves the HR timer; sporadic ones keep it (0.38 moved a thread
		// to sceKernelUsleep for good at its first one)
		constexpr u32 max_timeouts_in_row = 8;

		// Every wait arms the same ident: a timer that turned out not to be one-shot is re-armed by the next wait
		// instead of a second timer being added.
		constexpr s32 timer_ident = 1;

		// Arms one timer of `us` on `queue` and waits for it, bounded by the deadline + 2 ms. Returns 0 or the error
		// code (-1: no event); `got` is the number of events returned (more than one: an older event was queued).
		inline s32 wait_once(const functions& f, OrbisKernelEqueue queue, u64 us, s32& got) noexcept
		{
			kernel_timespec period{static_cast<s64>(us / 1'000'000), static_cast<s64>(us % 1'000'000 * 1000)};
			kernel_event events[4]{};
			OrbisKernelUseconds timeout = static_cast<OrbisKernelUseconds>(us + 2000);
			got = 0;
			s32 rc = f.add_hr(queue, timer_ident, &period, nullptr);
			if (rc == 0) rc = f.wait(queue, events, 4, &got, &timeout);
			if (rc == 0 && got < 1) rc = -1;
			return rc;
		}

		inline void usleep_bounded(u64 us) noexcept
		{
			sceKernelUsleep(static_cast<OrbisKernelUseconds>(std::min<u64>(us, 0xffff'ffffu)));
		}
	}

	inline bool hr_wait_enabled() noexcept
	{
		return hr_wait_detail::g_enabled.load(std::memory_order_acquire);
	}

	// 0.39: waits that timed out (deadline + 2 ms) with the equeue kept, for the 'hr wait:' line.
	inline u64 hr_wait_timeouts() noexcept
	{
		return hr_wait_detail::g_timeouts.load(std::memory_order_relaxed);
	}

	// Boot timer probe only, before any game thread waits.
	inline void hr_wait_configure(const hr_wait_detail::functions& f, bool enable) noexcept
	{
		hr_wait_detail::g_functions = f;
		hr_wait_detail::g_enabled.store(enable && f.create && f.destroy && f.wait && f.add_hr, std::memory_order_release);
	}

	// Sleeps `us` microseconds on this thread's equeue (created by its first wait, deleted at thread exit). Without
	// hr_wait_enabled(), for 1 s or more, and on a thread whose equeue failed once (the equeue is then deleted and
	// that thread stays on the fallback) this is sceKernelUsleep. Never logs; any thread.
	inline void hr_wait_us(u64 us) noexcept
	{
		using namespace hr_wait_detail;

		if (!us) return;
		if (us >= 1'000'000 || !hr_wait_enabled())
		{
			usleep_bounded(us);
			return;
		}

		struct thread_queue
		{
			OrbisKernelEqueue queue = 0;
			s32 state = 0; // 0: not created yet, 1: ready, -1: failed once, sceKernelUsleep only
			u32 timeouts_in_row = 0;

			~thread_queue()
			{
				if (state == 1) g_functions.destroy(queue);
			}
		};

		static thread_local thread_queue t_queue;

		if (t_queue.state < 0)
		{
			usleep_bounded(us);
			return;
		}

		const functions& f = g_functions;
		s32 rc = 0;

		if (t_queue.state == 0)
		{
			rc = f.create(&t_queue.queue, "rpcs3_hr_wait");
			t_queue.state = rc == 0 ? 1 : -1;
		}

		if (rc == 0)
		{
			s32 got = 0;
			rc = wait_once(f, t_queue.queue, us, got);

			if (rc == 0)
			{
				t_queue.timeouts_in_row = 0;
				g_waits.fetch_add(1, std::memory_order_relaxed);
				return;
			}

			// 0.39: a timed-out wait already lasted the deadline + 2 ms and says nothing against the equeue (a late
			// event is consumed by the next wait): counted, the equeue is kept and the thread stays on the HR timer.
			// Only a timer that keeps timing out (each wait 2 ms late) sends the thread to the fallback, as any error.
			if (rc == error_timed_out)
			{
				g_timeouts.fetch_add(1, std::memory_order_relaxed);
				g_last_error.store(rc, std::memory_order_relaxed);

				if (++t_queue.timeouts_in_row < max_timeouts_in_row)
				{
					return;
				}
			}

			f.destroy(t_queue.queue);
			t_queue.state = -1;
		}

		g_thread_failures.fetch_add(1, std::memory_order_relaxed);
		g_last_error.store(rc, std::memory_order_relaxed);
		usleep_bounded(us);
	}
#else
	inline bool hr_wait_enabled() noexcept
	{
		return false;
	}

	inline u64 hr_wait_timeouts() noexcept
	{
		return 0;
	}

	inline void hr_wait_us(u64 us) noexcept
	{
		std::this_thread::sleep_for(std::chrono::microseconds(us));
	}
#endif
}
