#pragma once

#include "util/types.hpp"
#include "util/asm.hpp"

#include <atomic>
#include <chrono>
#include <thread>

#ifdef __ORBIS__
#include <orbis/libkernel.h>
#endif

namespace utils
{
#ifdef __ORBIS__
	// 0.29: telemetry for the 60 s report (lv2.cpp ps4_lv2_sleep_stats): wait episodes that reached the sleep phase
	// and tick sleeps taken. Written only on the sleep path (already >= 1 ms into a wait), never on spin/yield.
	inline std::atomic<u64> g_ps4_backoff_sleep_episodes{0};
	inline std::atomic<u64> g_ps4_backoff_sleeps{0};
#endif

	// Back-off for host loops that wait until another thread makes progress
	// (sets a flag, releases a lock, drains a buffer).
	//
	// Upstream RPCS3 spins in such loops (pause()/yield()) and relies on the host
	// time-slicing runnable threads. Orbis schedules game threads with fixed
	// priorities (FIFO class, ORBIS_KERNEL_PRIO_FIFO_NORMAL = 700): a spinning
	// thread can keep the core that the awaited thread needs, which caused a
	// permanent livelock in Minecraft on 0.20.
	//
	// PS4 phases (everywhere else the methods are exactly the upstream calls):
	//   1. spin:  utils::pause() for the first `spin_limit` pause() calls (cheap handoffs);
	//   2. yield: sched_yield() so equal-priority threads on this core can run;
	//   3. sleep: once more than `sleep_after` has elapsed since the first yield, a short
	//      sceKernelUsleep() so lower-priority threads get the core too.
	// The sleep phase never starts within the first ~1 ms of yielding.
	//
	// 0.29: Orbis timed waits are tick-quantized (10 ms -> 10.3-11 ms, 20 ms -> 21 ms), so
	// sceKernelUsleep(sleep_us = 50) really lasts ~1-2 ms and the sleep phase notices progress
	// ~1 ms late. Kept on purpose: the phase starts only after >= 1 ms of fruitless yielding, and
	// only a real sleep hands the core to a descheduled or lower-priority thread; more spin/yield
	// is the 0.20 livelock again. The sleep path counts episodes and sleeps (g_ps4_backoff_*) for
	// the 60 s report.
	// 0.29: the elapsed-time check reads the TSC instead of steady_clock: on Orbis steady_clock is
	// orbis-compat clock_gettime, which bumps one global counter shared by every thread (a contended
	// cache line in exactly these contended loops) and then asks the kernel for the time.
	//
	// One instance per wait episode (construct it right before the loop). Not thread-safe.
	class progress_backoff
	{
#ifdef __ORBIS__
		u32 m_spins = 0;
		u32 m_yields = 0;
		bool m_sleeping = false;
		u64 m_yield_start = 0; // TSC at the first yield of the episode

		// TSC ticks in sleep_after. sceKernelGetTscFrequency() is the kernel's exact rate (independent
		// of RPCS3's calibration); 1.6 GHz (base PS4 Jaguar) only if the kernel ever answered 0.
		static u64 sleep_after_ticks() noexcept
		{
			static const u64 s_ticks = []() noexcept
			{
				const u64 freq = sceKernelGetTscFrequency();
				return (freq ? freq : 1'600'000'000ull) / 1'000'000 * static_cast<u64>(sleep_after.count());
			}();
			return s_ticks;
		}
#endif

	public:
		static constexpr u32 spin_limit = 200;
		static constexpr std::chrono::microseconds sleep_after{1000};
		static constexpr u32 sleep_us = 50;

		progress_backoff() noexcept = default;

		// Restart from the spin phase (e.g. after the awaited condition was observed once).
		void reset() noexcept
		{
#ifdef __ORBIS__
			m_spins = 0;
			m_yields = 0;
			m_sleeping = false;
#endif
		}

		// True once the wait has reached the sleep phase (PS4 only).
		bool sleeping() const noexcept
		{
#ifdef __ORBIS__
			return m_sleeping;
#else
			return false;
#endif
		}

		// Drop-in replacement for utils::pause() in a "wait for another thread" loop.
		FORCE_INLINE void pause() noexcept
		{
#ifdef __ORBIS__
			if (m_spins < spin_limit)
			{
				m_spins++;
				utils::pause();
				return;
			}

			yield();
#else
			utils::pause();
#endif
		}

		// Drop-in replacement for std::this_thread::yield() in such a loop
		// (the loop already had its own bounded busy-wait phase).
		void yield() noexcept
		{
#ifdef __ORBIS__
			if (m_sleeping)
			{
				g_ps4_backoff_sleeps.fetch_add(1, std::memory_order_relaxed);
				sceKernelUsleep(sleep_us); // ~1 tick (see the 0.29 note above)
				return;
			}

			if (m_yields++ == 0)
			{
				m_yield_start = utils::get_tsc();
			}
			else if (m_yields % 4 == 0 && utils::get_tsc() - m_yield_start >= sleep_after_ticks())
			{
				// Waited ~1 ms already: from now on really release the core. (A TSC read on another core that
				// is behind only makes the unsigned difference huge: the phase then starts early, which is safe.)
				m_sleeping = true;
				g_ps4_backoff_sleep_episodes.fetch_add(1, std::memory_order_relaxed);
				g_ps4_backoff_sleeps.fetch_add(1, std::memory_order_relaxed);
				sceKernelUsleep(sleep_us);
				return;
			}

			std::this_thread::yield();
#else
			std::this_thread::yield();
#endif
		}
	};
}
