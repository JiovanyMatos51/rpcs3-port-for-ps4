#pragma once

#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <limits>
#include <mutex>

// OpenOrbis libc++ has semaphore headers but lacks atomic-wait runtime
// functions. Preserve RPCN's wakeup counter using its working pthread-backed
// mutex/condition variable primitives instead.
class ps4_rpcn_semaphore
{
	std::mutex m_mutex;
	std::condition_variable m_ready;
	std::ptrdiff_t m_count;

public:
	explicit ps4_rpcn_semaphore(std::ptrdiff_t count) : m_count(count)
	{
		assert(count >= 0);
	}

	void release(std::ptrdiff_t update = 1)
	{
		{
			std::lock_guard lock(m_mutex);
			assert(update > 0 && update <= std::numeric_limits<std::ptrdiff_t>::max() - m_count);
			m_count += update;
		}
		if (update == 1) m_ready.notify_one();
		else m_ready.notify_all();
	}

	void acquire()
	{
		std::unique_lock lock(m_mutex);
		m_ready.wait(lock, [this] { return m_count > 0; });
		--m_count;
	}

	template<class Rep, class Period>
	bool try_acquire_for(std::chrono::duration<Rep, Period> timeout)
	{
		std::unique_lock lock(m_mutex);
		if (!m_ready.wait_for(lock, timeout, [this] { return m_count > 0; }))
		{
			return false;
		}
		--m_count;
		return true;
	}
};
