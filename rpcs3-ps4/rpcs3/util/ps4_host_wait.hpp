#pragma once

// Blocking host waits of guest PPU threads, kept out of the lv2 run queue.
//
// A PPU thread that blocks in host code (a host mutex, an atomic wait, a timed sleep) while it is still queued in
// the lv2 scheduler can freeze emulation: if lv2 preempts it meanwhile, its unanswered preemption keeps every other
// PPU thread suspended (schedule_all resumes nobody while g_pending != 0); without cpu_flag::wait it also stalls
// vm writers, suspend_all and one of the vm passive-lock slots. When the awaited party needs a PPU thread to run
// (a lock holder parked by lv2, a joined thread) the freeze is permanent. The loops below leave the run queue
// before blocking and re-enter it afterwards, as sys_fs and lock_lv2_mutex_alike do. The scheduler side is
// injected (Ops), so the loops run unchanged in a host test with a mock scheduler.

namespace utils::ps4
{
	// Result of leaving the run queue (Ops::detach)
	enum class host_detach : unsigned char
	{
		not_queued, // not in the run queue (already asleep in lv2, or stopping): waiting holds nobody up; no attach
		detached,   // left the run queue: wait, then Ops::attach(), which may park until a slot is free
		signaled,   // a scheduler wake-up raced the detach and was consumed (thread attached again): detach again
	};

	// Detach attempts that may meet a racing wake-up before the wait runs attached, as it did before
	inline constexpr unsigned host_detach_retries = 8;

	// Runs 'wait' outside the run queue. Ops: host_detach detach(); void attach().
	template <typename Ops, typename F>
	host_detach host_wait(Ops& ops, F&& wait)
	{
		host_detach result = ops.detach();

		for (unsigned i = 0; result == host_detach::signaled && i < host_detach_retries; i++)
		{
			result = ops.detach();
		}

		wait();

		if (result == host_detach::detached)
		{
			ops.attach();
		}

		return result;
	}

	// How host_lock obtained the lock
	enum class host_lock_path : unsigned char
	{
		uncontended, // first try
		spun,        // during the short spin (holds of a few microseconds)
		plain,       // not a guest thread: plain blocking lock
		nested,      // the caller holds another host lock: plain blocking lock (never park holding a lock)
		stopping,    // the guest thread is stopping: plain blocking lock (the holder unwinds too)
		waited,      // waited for the lock outside the run queue
	};

	// Spin tries before waiting outside the run queue
	inline constexpr unsigned host_lock_spins = 10;

	// Lock acquisition that never blocks inside the run queue and never re-enters it (attach, which may park)
	// while owning the lock. Ops:
	//   bool try_lock(); void lock();                plain operations of the lock
	//   void spin();                                 one short busy wait
	//   bool guest(); bool other_locks_held(); bool stopping();
	//   host_detach detach(); void attach();         see host_wait
	//   void wait_free();                            blocks until the lock looks free, without taking it
	//   void note_wait(); void note_nested();        diagnostics, called once per contended acquisition
	template <typename Ops>
	host_lock_path host_lock(Ops& ops, unsigned spins = host_lock_spins)
	{
		if (ops.try_lock())
		{
			return host_lock_path::uncontended;
		}

		if (!ops.guest())
		{
			ops.lock();
			return host_lock_path::plain;
		}

		for (unsigned i = 0; i < spins; i++)
		{
			ops.spin();

			if (ops.try_lock())
			{
				return host_lock_path::spun;
			}
		}

		if (ops.other_locks_held())
		{
			ops.note_nested();
			ops.lock();
			return host_lock_path::nested;
		}

		ops.note_wait();

		while (true)
		{
			if (ops.stopping())
			{
				ops.lock();
				return host_lock_path::stopping;
			}

			host_wait(ops, [&] { ops.wait_free(); });

			if (ops.try_lock())
			{
				return host_lock_path::waited;
			}
		}
	}
}
