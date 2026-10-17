#pragma once

#include <mutex>
#include "util/types.hpp"
#include "util/atomic.hpp"

#ifdef RPCS3_PS4
#include <atomic>

class shared_mutex;

// 0.33: the shared_mutex holds of the current thread. thread_ctrl::emergency_exit and silent_exit end a thread with
// pthread_exit, which does not unwind on the PS4, so the lock guards of a dying thread never run and its waiters
// (the emulator stop included) block for good. Utilities/Thread.cpp hands the recorded holds to the stop sequence
// that follows the thread fatal. Constant-initialized and trivially destructible: no TLS guard on access, nothing
// runs at thread exit. Only the owning thread writes it; a slot is reserved before it is filled and searches run
// from the top, so a lock/unlock pair in a fault handler that interrupts an add or a remove leaves it intact.
struct ps4_held_locks
{
	static constexpr u32 c_slots = 16;

	uptr slot[c_slots]; // shared_mutex address, bit 0 set for an exclusive hold; oldest first
	u32 count;
	u32 lost; // oldest holds forgotten to make room (never released for a dead thread)
};

inline constinit thread_local ps4_held_locks g_tls_ps4_held_locks{};

// Cleared at startup by /data/rpcs3/no-lock-release.flag: nothing is recorded and nothing is released
inline constinit atomic_t<bool> g_ps4_lock_record{true};

// Holds of dead threads not given back yet. While non-zero, shared_mutex waiters poll (Utilities/mutex.cpp).
inline constinit atomic_t<u32> g_ps4_dead_holds{0};

inline void ps4_lock_record_add(const void* mutex, uptr exclusive) noexcept
{
	if (!g_ps4_lock_record.observe()) [[unlikely]]
	{
		return;
	}

	ps4_held_locks& held = g_tls_ps4_held_locks;
	const u32 n = held.count;

	if (n < ps4_held_locks::c_slots) [[likely]]
	{
		held.count = n + 1;
		std::atomic_signal_fence(std::memory_order_seq_cst);
		held.slot[n] = reinterpret_cast<uptr>(mutex) | exclusive;
	}
	else
	{
		// Full: forget the oldest hold (it would stay held if the thread died). A record whose lock was released by
		// another thread (a guard moved across threads) ages out the same way instead of filling the record for good.
		for (u32 i = 1; i < n; i++)
		{
			held.slot[i - 1] = held.slot[i];
		}

		std::atomic_signal_fence(std::memory_order_seq_cst);
		held.slot[n - 1] = reinterpret_cast<uptr>(mutex) | exclusive;
		held.lost++;
	}
}

// Runs whether or not recording is on, so a hold recorded before the flag was read is still removed
inline void ps4_lock_record_remove(const void* mutex, uptr exclusive) noexcept
{
	ps4_held_locks& held = g_tls_ps4_held_locks;
	const uptr key = reinterpret_cast<uptr>(mutex) | exclusive;
	const u32 n = held.count;

	for (u32 i = n; i-- > 0;)
	{
		if (held.slot[i] == key)
		{
			for (u32 j = i + 1; j < n; j++)
			{
				held.slot[j - 1] = held.slot[j];
			}

			std::atomic_signal_fence(std::memory_order_seq_cst);
			held.count = n - 1;
			return;
		}
	}
}

// Upgrade/downgrade: the same hold changes mode; a hold that was never recorded is recorded in the new mode
inline void ps4_lock_record_retag(const void* mutex, uptr from, uptr to) noexcept
{
	ps4_held_locks& held = g_tls_ps4_held_locks;
	const uptr key = reinterpret_cast<uptr>(mutex) | from;

	for (u32 i = held.count; i-- > 0;)
	{
		if (held.slot[i] == key)
		{
			held.slot[i] = reinterpret_cast<uptr>(mutex) | to;
			return;
		}
	}

	ps4_lock_record_add(mutex, to);
}

// 0.37: what a PPU thread holds and waits for at its blocking points, for another thread's STALL dump. It lives in
// static storage (never freed, so a reader can never touch a dead thread's TLS); only the owning thread writes it,
// through g_tls_ps4_lock_view, which cpu_thread sets for PPU threads. Snapshots are taken when the thread blocks in
// shared_mutex::imp_wait and when lv2 parks it while it holds host locks; the holds are valid while either is set.
struct ps4_lock_view
{
	static constexpr u32 c_shown = 8;

	atomic_t<uptr> waiting_on{0}; // shared_mutex blocked on in imp_wait
	atomic_t<u32> parked{0};      // parked by lv2 (cpu_thread::check_state) with host locks held
	atomic_t<u32> count{0};       // holds at the last snapshot (all of them, the newest c_shown are listed)
	atomic_t<uptr> held[c_shown]{};

	void snapshot() noexcept
	{
		const ps4_held_locks& own = g_tls_ps4_held_locks;
		const u32 n = own.count < ps4_held_locks::c_slots ? own.count : ps4_held_locks::c_slots;
		const u32 first = n > c_shown ? n - c_shown : 0;

		for (u32 i = 0; i < c_shown; i++)
		{
			held[i].release(first + i < n ? own.slot[first + i] : 0);
		}

		count.release(n);
	}

	uptr wait_begin(const void* mutex) noexcept
	{
		snapshot();
		return waiting_on.exchange(reinterpret_cast<uptr>(mutex));
	}

	void wait_end(uptr previous) noexcept
	{
		waiting_on.release(previous);
	}
};

inline constinit thread_local ps4_lock_view* g_tls_ps4_lock_view = nullptr;

// Utilities/Thread.cpp. Called by a waiter of 'mutex' after each bounded wait while dead holds exist ('since' is
// the waiter's first poll); gives back the dead holds of 'mutex' once the emulator is stopping.
void ps4_dead_hold_wait(shared_mutex& mutex, u64& since) noexcept;

// Utilities/Thread.cpp. Gives back every dead hold of the session being stopped; does nothing unless the emulator
// is stopping. Meant for the end of the stop, after every emulation thread has been joined. Returns the count.
u32 ps4_release_dead_thread_locks() noexcept;

// Holds of dead threads still recorded (not given back)
inline u32 ps4_dead_thread_lock_count() noexcept
{
	return g_ps4_dead_holds.load();
}
#endif

// Shared mutex with small size (u32).
class shared_mutex final
{
	enum : u32
	{
		c_one = 1u << 14, // Fixed-point 1.0 value (one writer, max_readers = c_one - 1)
		c_sig = 1u << 30,
		c_err = 1u << 31,
	};

	atomic_t<u32> m_value{};

	void imp_lock_shared(u32 val);
	void imp_unlock_shared(u32 old);
	void imp_wait();
	void imp_signal();
	void imp_lock(u32 val);
	void imp_unlock(u32 old);
	void imp_lock_upgrade();
	void imp_lock_unlock();

#ifdef RPCS3_PS4
	// 0.33: the operations without the held-lock record, for the slow paths in mutex.cpp (the public operation that
	// called them records the hold exactly once)
	bool raw_try_lock_shared()
	{
		const u32 value = m_value.load();

		return value < c_one - 1 && m_value.compare_and_swap_test(value, value + 1);
	}

	bool raw_try_lock()
	{
		return m_value.compare_and_swap_test(0, c_one);
	}

	bool raw_try_lock_upgrade()
	{
		const u32 value = m_value.load();

		return (value + c_one - 1) % c_one == 0 && m_value.compare_and_swap_test(value, value + c_one - 1);
	}

	void raw_unlock()
	{
		const u32 value = m_value.fetch_sub(c_one);

		if (value != c_one) [[unlikely]]
		{
			imp_unlock(value);
		}
	}

	void raw_lock_downgrade()
	{
		m_value -= c_one - 1;
	}
#endif

public:
	constexpr shared_mutex() = default;

	bool try_lock_shared()
	{
		const u32 value = m_value.load();

		// Conditional increment
#ifdef RPCS3_PS4
		if (!(value < c_one - 1 && m_value.compare_and_swap_test(value, value + 1)))
		{
			return false;
		}

		ps4_lock_record_add(this, 0);
		return true;
#else
		return value < c_one - 1 && m_value.compare_and_swap_test(value, value + 1);
#endif
	}

	void lock_shared()
	{
		const u32 value = m_value.load();

		if (value >= c_one - 1 || !m_value.compare_and_swap_test(value, value + 1)) [[unlikely]]
		{
			imp_lock_shared(value);
		}

#ifdef RPCS3_PS4
		ps4_lock_record_add(this, 0);
#endif
	}

	void lock_shared_hle()
	{
		const u32 value = m_value.load();

		if (value < c_one - 1) [[likely]]
		{
			u32 old = value;
			if (atomic_storage<u32>::compare_exchange_hle_acq(m_value.raw(), old, value + 1)) [[likely]]
			{
#ifdef RPCS3_PS4
				ps4_lock_record_add(this, 0);
#endif
				return;
			}
		}

		imp_lock_shared(value);

#ifdef RPCS3_PS4
		ps4_lock_record_add(this, 0);
#endif
	}

	void unlock_shared()
	{
#ifdef RPCS3_PS4
		// Forget the hold first: a thread that dies in between keeps the lock rather than releasing it twice
		ps4_lock_record_remove(this, 0);
#endif

		// Unconditional decrement (can result in broken state)
		const u32 value = m_value.fetch_sub(1);

		if (value >= c_one) [[unlikely]]
		{
			imp_unlock_shared(value);
		}
	}

	void unlock_shared_hle()
	{
#ifdef RPCS3_PS4
		ps4_lock_record_remove(this, 0);
#endif

		const u32 value = atomic_storage<u32>::fetch_add_hle_rel(m_value.raw(), -1);

		if (value >= c_one) [[unlikely]]
		{
			imp_unlock_shared(value);
		}
	}

	bool try_lock()
	{
#ifdef RPCS3_PS4
		if (!m_value.compare_and_swap_test(0, c_one))
		{
			return false;
		}

		ps4_lock_record_add(this, 1);
		return true;
#else
		return m_value.compare_and_swap_test(0, c_one);
#endif
	}

	void lock()
	{
		const u32 value = m_value.compare_and_swap(0, c_one);

		if (value) [[unlikely]]
		{
			imp_lock(value);
		}

#ifdef RPCS3_PS4
		ps4_lock_record_add(this, 1);
#endif
	}

	void lock_hle()
	{
		u32 value = 0;

		if (!atomic_storage<u32>::compare_exchange_hle_acq(m_value.raw(), value, c_one)) [[unlikely]]
		{
			imp_lock(value);
		}

#ifdef RPCS3_PS4
		ps4_lock_record_add(this, 1);
#endif
	}

	void unlock()
	{
#ifdef RPCS3_PS4
		ps4_lock_record_remove(this, 1);
#endif

		// Unconditional decrement (can result in broken state)
		const u32 value = m_value.fetch_sub(c_one);

		if (value != c_one) [[unlikely]]
		{
			imp_unlock(value);
		}
	}

	void unlock_hle()
	{
#ifdef RPCS3_PS4
		ps4_lock_record_remove(this, 1);
#endif

		const u32 value = atomic_storage<u32>::fetch_add_hle_rel(m_value.raw(), ~c_one + 1);

		if (value != c_one) [[unlikely]]
		{
			imp_unlock(value);
		}
	}

	bool try_lock_upgrade()
	{
		const u32 value = m_value.load();

		// Conditional increment, try to convert a single reader into a writer, ignoring other writers
#ifdef RPCS3_PS4
		if (!((value + c_one - 1) % c_one == 0 && m_value.compare_and_swap_test(value, value + c_one - 1)))
		{
			return false;
		}

		ps4_lock_record_retag(this, 0, 1);
		return true;
#else
		return (value + c_one - 1) % c_one == 0 && m_value.compare_and_swap_test(value, value + c_one - 1);
#endif
	}

	void lock_upgrade()
	{
		if (!try_lock_upgrade()) [[unlikely]]
		{
			imp_lock_upgrade();

#ifdef RPCS3_PS4
			ps4_lock_record_retag(this, 0, 1);
#endif
		}
	}

	void lock_downgrade()
	{
		// Convert to reader lock (can result in broken state)
		m_value -= c_one - 1;

#ifdef RPCS3_PS4
		ps4_lock_record_retag(this, 1, 0);
#endif
	}

#ifdef RPCS3_PS4
	// 0.33: for holds of threads that ended without unwinding (Utilities/Thread.cpp)

	// The lock is held in that mode by someone (a pending hand-over or a broken state counts as not held)
	bool ps4_looks_held(bool exclusive) const
	{
		const u32 value = m_value.load();

		return value < c_sig && (exclusive ? value >= c_one && value % c_one == 0 : value % c_one != 0);
	}

	// Releases one such hold the way its dead owner would have (unlock or unlock_shared) without touching the
	// caller's own record. Refused, with nothing changed, when the lock is not held in that mode any more.
	bool ps4_give_back(bool exclusive);

	// Current waiters re-check their state (they then wait in bounded steps, see imp_wait)
	void ps4_wake_waiters()
	{
		m_value.notify_all();
	}

	// A hold handed to another thread together with its guard (a std::unique_lock moved into a new thread): the
	// giving thread calls ps4_hold_moved_out() right before the hand-over, the receiving thread ps4_hold_moved_in()
	// first thing, so that only the thread that will unlock it has it on record.
	void ps4_hold_moved_out(bool exclusive) const noexcept
	{
		ps4_lock_record_remove(this, exclusive);
	}

	void ps4_hold_moved_in(bool exclusive) const noexcept
	{
		ps4_lock_record_add(this, exclusive);
	}
#endif

	// Optimized wait for lockability without locking, relaxed
	void lock_unlock()
	{
		if (m_value != 0) [[unlikely]]
		{
			imp_lock_unlock();
		}
	}

	// Check whether can immediately obtain an exclusive (writer) lock
	bool is_free() const
	{
		return m_value.load() == 0;
	}

	// Check whether can immediately obtain a shared (reader) lock
	bool is_lockable() const
	{
		return m_value.load() < c_one - 1;
	}

	bool has_waiters() const
	{
		return m_value.load() > c_one;
	}
};

// Simplified shared (reader) lock implementation.
class reader_lock final
{
	shared_mutex& m_mutex;
	bool m_upgraded = false;

public:
	reader_lock(const reader_lock&) = delete;

	reader_lock& operator=(const reader_lock&) = delete;

	explicit reader_lock(shared_mutex& mutex)
		: m_mutex(mutex)
	{
		m_mutex.lock_shared();
	}

	// One-way lock upgrade; note that the observed state could have been changed
	void upgrade()
	{
		if (!m_upgraded)
		{
			m_mutex.lock_upgrade();
			m_upgraded = true;
		}
	}

	// Try to upgrade; if it succeeds, the observed state has NOT been changed
	bool try_upgrade()
	{
		return m_upgraded || (m_upgraded = m_mutex.try_lock_upgrade());
	}

	~reader_lock()
	{
		m_upgraded ? m_mutex.unlock() : m_mutex.unlock_shared();
	}
};
