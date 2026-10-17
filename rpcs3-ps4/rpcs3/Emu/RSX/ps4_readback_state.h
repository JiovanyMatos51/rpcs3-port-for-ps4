#pragma once

// PS4: guest SPU threads waiting inside VKGSRender::on_access_violation for a framebuffer readback (Write Color or
// Depth Buffers). The faulting threads write it; the RSX report and label writers read it to log kicks that land while
// such a readback is in flight. Telemetry only: nothing decides behaviour from it.

#ifdef RPCS3_PS4
#include "util/types.hpp"
#include "util/atomic.hpp"
#include "Emu/Cell/timers.hpp"

#include <algorithm>

namespace rsx
{
	struct ps4_readback_state_t
	{
		// SPU number (lv2 thread id >> 24, the N of "BigCellSpursKernelN") of the newest SPU readback in flight.
		atomic_t<u32> spu_id{0};
		// get_system_time() when that readback started; 0 while no SPU readback is in flight.
		atomic_t<u64> since_us{0};
		// SPU readbacks in flight (several SPU threads can fault at the same time).
		atomic_t<u32> count{0};
	};

	inline ps4_readback_state_t g_ps4_readback_state;

	// Any thread. True while an SPU readback is in flight; spu_id and elapsed_ms then describe the newest one.
	inline bool ps4_spu_readback_in_flight(u32& spu_id, u32& elapsed_ms) noexcept
	{
		const u64 since = g_ps4_readback_state.since_us.load();
		if (!since || !g_ps4_readback_state.count.load())
		{
			return false;
		}

		const u64 now = get_system_time();
		spu_id = g_ps4_readback_state.spu_id.load();
		elapsed_ms = now > since ? static_cast<u32>(std::min<u64>((now - since) / 1000, u32{umax})) : 0;
		return true;
	}

	// Marks one SPU readback in flight for its lifetime (VKGSRender::on_access_violation, faulting SPU thread).
	class ps4_spu_readback_scope
	{
		u64 m_since = 0;

	public:
		explicit ps4_spu_readback_scope(u32 spu_id) noexcept
		{
			m_since = std::max<u64>(get_system_time(), 1);
			g_ps4_readback_state.count++;
			g_ps4_readback_state.spu_id.release(spu_id);
			g_ps4_readback_state.since_us.release(m_since);
		}

		~ps4_spu_readback_scope()
		{
			if (g_ps4_readback_state.count.sub_fetch(1) == 0)
			{
				// Only clears the start time this scope wrote; a readback that started meanwhile keeps its own.
				g_ps4_readback_state.since_us.compare_and_swap(m_since, 0);
			}
		}

		ps4_spu_readback_scope(const ps4_spu_readback_scope&) = delete;
		ps4_spu_readback_scope& operator=(const ps4_spu_readback_scope&) = delete;
	};
}
#endif
