#include "mutex.h"

#include "util/asm.hpp"

void shared_mutex::imp_lock_shared(u32 val)
{
	ensure(val < c_err); // "shared_mutex underflow"

	// Try to steal the notification bit
	if (val & c_sig && m_value.compare_exchange(val, val - c_sig + 1))
	{
		return;
	}

	for (int i = 0; i < 10; i++)
	{
#ifdef RPCS3_PS4
		if (raw_try_lock_shared())
#else
		if (try_lock_shared())
#endif
		{
			return;
		}

		const u32 old = m_value;

		if (old & c_sig && m_value.compare_and_swap_test(old, old - c_sig + 1))
		{
			return;
		}

		busy_wait();
	}

	// Acquire writer lock and downgrade
	const u32 old = m_value.fetch_add(c_one);

	if (old == 0)
	{
#ifdef RPCS3_PS4
		raw_lock_downgrade();
#else
		lock_downgrade();
#endif
		return;
	}

	ensure((old % c_sig) + c_one < c_sig); // "shared_mutex overflow"
	imp_wait();
#ifdef RPCS3_PS4
	raw_lock_downgrade();
#else
	lock_downgrade();
#endif
}

void shared_mutex::imp_unlock_shared(u32 old)
{
	ensure(old - 1 < c_err); // "shared_mutex underflow"

	// Check reader count, notify the writer if necessary
	if ((old - 1) % c_one == 0)
	{
		imp_signal();
	}
}

void shared_mutex::imp_wait()
{
#ifdef RPCS3_PS4
	u64 ps4_since = 0;

	// 0.37: the lock a PPU thread blocks on, for the STALL dump (a nested wait, e.g. from a fault handler, restores it)
	ps4_lock_view* const ps4_view = g_tls_ps4_lock_view;
	const uptr ps4_prev_wait = ps4_view ? ps4_view->wait_begin(this) : 0;
#endif

	while (true)
	{
		const auto [old, ok] = m_value.fetch_op([](u32& value)
		{
			if (value >= c_sig)
			{
				value -= c_sig;
				return true;
			}

			return false;
		});

		if (ok)
		{
			break;
		}

#ifdef RPCS3_PS4
		if (g_ps4_dead_holds.observe()) [[unlikely]]
		{
			// 0.33: a thread ended without unwinding while holding locks. Wait in 100 ms steps so that this waiter
			// can give such a hold back once the emulator is stopping (Utilities/Thread.cpp); a dying holder wakes
			// the waiters that were already asleep, so they get here too.
			m_value.wait(old, atomic_wait_timeout{100'000'000});
			ps4_dead_hold_wait(*this, ps4_since);
			continue;
		}
#endif

		m_value.wait(old);
	}

#ifdef RPCS3_PS4
	if (ps4_view)
	{
		ps4_view->wait_end(ps4_prev_wait);
	}
#endif
}

void shared_mutex::imp_signal()
{
	m_value += c_sig;
	m_value.notify_one();
}

void shared_mutex::imp_lock(u32 val)
{
	ensure(val < c_err); // "shared_mutex underflow"

	// Try to steal the notification bit
	if (val & c_sig && m_value.compare_exchange(val, val - c_sig + c_one))
	{
		return;
	}

	for (int i = 0; i < 10; i++)
	{
		busy_wait();

		const u32 old = m_value;

#ifdef RPCS3_PS4
		if (!old && raw_try_lock())
#else
		if (!old && try_lock())
#endif
		{
			return;
		}

		if (old & c_sig && m_value.compare_and_swap_test(old, old - c_sig + c_one))
		{
			return;
		}
	}

	const u32 old = m_value.fetch_add(c_one);

	if (old == 0)
	{
		return;
	}

	ensure((old % c_sig) + c_one < c_sig); // "shared_mutex overflow"
	imp_wait();
}

void shared_mutex::imp_unlock(u32 old)
{
	ensure(old - c_one < c_err); // "shared_mutex underflow"

	// 1) Notify the next writer if necessary
	// 2) Notify all readers otherwise if necessary (currently indistinguishable from writers)
	if (old - c_one)
	{
		imp_signal();
	}
}

void shared_mutex::imp_lock_upgrade()
{
	for (int i = 0; i < 10; i++)
	{
		busy_wait();

#ifdef RPCS3_PS4
		if (raw_try_lock_upgrade())
#else
		if (try_lock_upgrade())
#endif
		{
			return;
		}
	}

	// Convert to writer lock
	const u32 old = m_value.fetch_add(c_one - 1);

	ensure((old % c_sig) + c_one - 1 < c_sig); // "shared_mutex overflow"

	if (old % c_one == 1)
	{
		return;
	}

	imp_wait();
}

void shared_mutex::imp_lock_unlock()
{
	u32 _max = 1;

	for (int i = 0; i < 30; i++)
	{
		const u32 val = m_value;

		if (val % c_one == 0 && (val / c_one < _max || val >= c_sig))
		{
			// Return if have cought a state where:
			// 1) Mutex is free
			// 2) Total number of waiters decreased since last check
			// 3) Signal bit is set (if used on the platform)
			return;
		}

		_max = val / c_one;

		busy_wait(1500);
	}

	// Lock and unlock
	if (!m_value.fetch_add(c_one))
	{
#ifdef RPCS3_PS4
		raw_unlock();
#else
		unlock();
#endif
		return;
	}

	imp_wait();
#ifdef RPCS3_PS4
	raw_unlock();
#else
	unlock();
#endif
}

#ifdef RPCS3_PS4
bool shared_mutex::ps4_give_back(bool exclusive)
{
	// The transitions of unlock() and unlock_shared(), applied only while the lock is still held in that mode
	const auto [old, ok] = m_value.fetch_op([exclusive](u32& value)
	{
		if (value >= c_sig || (exclusive ? value < c_one || value % c_one != 0 : value % c_one == 0))
		{
			return false;
		}

		value -= exclusive ? +c_one : 1;
		return true;
	});

	if (!ok)
	{
		return false;
	}

	if (exclusive && old != c_one)
	{
		imp_unlock(old);
	}
	else if (!exclusive && old >= c_one)
	{
		imp_unlock_shared(old);
	}

	return true;
}
#endif
