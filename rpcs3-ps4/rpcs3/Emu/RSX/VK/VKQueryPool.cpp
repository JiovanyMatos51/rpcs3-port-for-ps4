#include "stdafx.h"
#include "vkutils/query_pool.hpp"
#include "VKHelpers.h"
#include "VKQueryPool.h"
#include "VKRenderPass.h"
#include "VKResourceManager.h"
#include "util/asm.hpp"
#include "VKGSRender.h"

#ifdef RPCS3_PS4
#include "Emu/Cell/timers.hpp"
#include "ps4_fatal.h"
#include "ps4_launcher.h"
#include "vkutils/memory.h"
#include "util/ps4_gpu_health.hpp"

#include <atomic>
#include <new>
#endif

namespace vk
{
#ifdef RPCS3_PS4
	namespace
	{
		// An occlusion query result stayed unavailable for 2 s and now counts as visible. The first one of the
		// process is logged; every one is counted in the 60 s 'PS4 VRAM:' line.
		void ps4_note_query_unavailable(u32 index, u32 polls)
		{
			if (utils::ps4::gpu::note_query_timeout() != 1)
			{
				return;
			}

			rsx_log.error("PS4 GPU: occlusion query %u unavailable after 2 s (%u polls); it counts as visible", index, polls);
			ps4_startup_checkpoint(fmt::format("PS4 GPU: occlusion query %u unavailable after 2 s; it counts as visible "
				"(later ones are counted as 'occlusion query timeouts' in the PS4 VRAM line)", index).c_str());
		}

		// The occlusion query pool could not be created after a memory reclaim and one retry: no pool for the rest of
		// this renderer, every occlusion query answers visible (VKDraw.cpp). One manager per renderer, RSX thread.
		std::atomic<bool> s_ps4_no_query_pool{false};
	}

	bool ps4_query_pool_unavailable() noexcept
	{
		return s_ps4_no_query_pool.load(std::memory_order_relaxed);
	}
#endif

	inline bool query_pool_manager::poke_query(query_slot_info& query, u32 index, VkQueryResultFlags flags)
	{
		// Query is ready if:
		// 1. Any sample has been determined to have passed the Z test
		// 2. The backend has fully processed the query and found no hits

		u32 result[2] = { 0, 0 };
		switch (const auto error = vkGetQueryPoolResults(*owner, *query.pool, index, 1, 8, result, 8, flags | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT))
		{
		case VK_SUCCESS:
		{
			if (result[0])
			{
				query.any_passed = true;
				query.ready = true;
				query.data = result[0];
				return true;
			}
			else if (result[1])
			{
				query.any_passed = false;
				query.ready = true;
				query.data = 0;
				return true;
			}

			return false;
		}
		case VK_NOT_READY:
		{
			query.any_passed = !!result[0];
			query.ready = query.any_passed && !!(flags & VK_QUERY_RESULT_PARTIAL_BIT);
			query.data = result[0];
			return query.ready;
		}
		default:
			die_with_error(error);
			return false;
		}
	}

	query_pool_manager::query_pool_manager(vk::render_device& dev, VkQueryType type, u32 num_entries)
	{
		ensure(num_entries > 0);

		owner = &dev;
		query_type = type;
		query_slot_status.resize(num_entries, {});

		for (unsigned i = 0; i < num_entries; ++i)
		{
			m_available_slots.push_back(i);
		}

#ifdef RPCS3_PS4
		// A new renderer tries again
		s_ps4_no_query_pool.store(false, std::memory_order_relaxed);
#endif
	}

	query_pool_manager::~query_pool_manager()
	{
		if (m_current_query_pool)
		{
			m_current_query_pool.reset();
			owner = nullptr;
		}
	}

	void query_pool_manager::allocate_new_pool(vk::command_buffer& cmd)
	{
		ensure(!m_current_query_pool);
#ifdef RPCS3_PS4
		if (s_ps4_no_query_pool.load(std::memory_order_relaxed))
		{
			return;
		}

		{
			std::lock_guard lock(m_query_pool_cache_lock);

			if (m_query_pool_cache.size() > 0)
			{
				m_current_query_pool = std::move(m_query_pool_cache.front());
				m_query_pool_cache.pop_front();
			}
		}

		if (!m_current_query_pool)
		{
			// Created outside the cache lock: the reclaim below may release pools into the cache.
			const u32 count = ::size32(query_slot_status);
			m_current_query_pool = std::make_unique<query_pool>(*owner, query_type, count);

			if (!m_current_query_pool->ps4_created())
			{
				const VkResult first = m_current_query_pool->m_ps4_result;
				m_current_query_pool.reset();

				// The reclaim of a failed required allocation: on the RSX thread the deep reclaim (two hard syncs that
				// keep this command buffer), elsewhere a deferred trim; host memory through the heap's new_handler.
				if (first == VK_ERROR_OUT_OF_HOST_MEMORY)
				{
					if (const std::new_handler handler = std::get_new_handler())
					{
						handler();
					}
				}
				else if (!vk::ps4_vram_deep_reclaim("occlusion query pool creation"))
				{
					vk::ps4_vram_relieve_deferred(rsx::problem_severity::fatal);
				}

				m_current_query_pool = std::make_unique<query_pool>(*owner, query_type, count);

				if (!m_current_query_pool->ps4_created())
				{
					const VkResult second = m_current_query_pool->m_ps4_result;
					m_current_query_pool.reset();
					s_ps4_no_query_pool.store(true, std::memory_order_relaxed);

					const std::string line = fmt::format("PS4 GPU: query pool creation failed after reclaim (VkResult %d, then %d); occlusion queries answer visible for this session",
						static_cast<int>(first), static_cast<int>(second));
					rsx_log.error("%s", line);
					ps4_startup_checkpoint(line.c_str());
					return;
				}

				rsx_log.warning("PS4 GPU: query pool created after a memory reclaim (the first creation returned VkResult %d)", static_cast<int>(first));
			}
		}
#else
		{
			std::lock_guard lock(m_query_pool_cache_lock); // If we're creating a new pool, we probably have items in the cache.

			if (m_query_pool_cache.size() > 0)
			{
				m_current_query_pool = std::move(m_query_pool_cache.front());
				m_query_pool_cache.pop_front();
			}
			else
			{
				const u32 count = ::size32(query_slot_status);
				m_current_query_pool = std::make_unique<query_pool>(*owner, query_type, count);
			}
		}
#endif

		// From spec: "After query pool creation, each query must be reset before it is used."
		vkCmdResetQueryPool(cmd, *m_current_query_pool.get(), 0, m_current_query_pool->size());
		m_pool_lifetime_counter = m_current_query_pool->size();
	}

	void query_pool_manager::reallocate_pool(vk::command_buffer& cmd)
	{
		if (m_current_query_pool)
		{
			if (!m_current_query_pool->has_refs())
			{
				auto ref = std::make_unique<query_pool_ref>(this, m_current_query_pool);
				vk::get_resource_manager()->dispose(ref);
			}
			else
			{
				m_consumed_pools.emplace_back(std::move(m_current_query_pool));

				// Sanity check
				if (m_consumed_pools.size() > 3)
				{
					rsx_log.error("[Robustness warning] Query pool discard pile size is now %llu. Are we leaking??", m_consumed_pools.size());
				}
			}
		}

		allocate_new_pool(cmd);
	}

	void query_pool_manager::run_pool_cleanup()
	{
		for (auto It = m_consumed_pools.begin(); It != m_consumed_pools.end();)
		{
			if (!(*It)->has_refs())
			{
				auto ref = std::make_unique<query_pool_ref>(this, *It);
				vk::get_resource_manager()->dispose(ref);
				It = m_consumed_pools.erase(It);
			}
			else
			{
				It++;
			}
		}
	}

	void query_pool_manager::set_control_flags(VkQueryControlFlags control_, VkQueryResultFlags result_)
	{
		control_flags = control_;
		result_flags = result_;
	}

	void query_pool_manager::begin_query(vk::command_buffer& cmd, u32 index)
	{
		ensure(query_slot_status[index].active == false);

		auto& query_info = query_slot_status[index];
		query_info.pool = m_current_query_pool.get();
		query_info.active = true;

		vkCmdBeginQuery(cmd, *query_info.pool, index, control_flags);
	}

	void query_pool_manager::end_query(vk::command_buffer& cmd, u32 index)
	{
		vkCmdEndQuery(cmd, *query_slot_status[index].pool, index);
	}

	bool query_pool_manager::check_query_status(u32 index)
	{
		return poke_query(query_slot_status[index], index, result_flags);
	}

	u32 query_pool_manager::get_query_result(u32 index)
	{
		// Check for cached result
		auto& query_info = query_slot_status[index];

		if (!query_info.ready)
		{
			poke_query(query_info, index, result_flags);

#ifdef RPCS3_PS4
			// ZCULL readback waits on the GPU from the RSX thread: back off instead of spinning.
			// A result that never becomes available would hold the RSX thread for good: after 2 s the query counts
			// as visible (the object is drawn, as with ZCULL off). Like the fence watchdog, the clock restarts after
			// a gap of more than 2 s between two checks (suspend) and while the app is in the background.
			u64 ps4_start_us = 0;
			u64 ps4_last_us = 0;
			for (u32 spins = 0; !query_info.ready; spins++)
			{
				vk::ps4_gpu_wait_backoff(spins);
				poke_query(query_info, index, result_flags);

				if (query_info.ready || (spins % 16) != 15)
				{
					continue;
				}

				const u64 now = get_system_time();
				if (!ps4_start_us || now - ps4_last_us > 2'000'000 || ps4_in_background())
				{
					ps4_start_us = now;
				}
				ps4_last_us = now;

				// A wait of a second or more: the liveness scan runs from here too (a hung submission is confirmed
				// while the RSX thread waits on it).
				if (now - ps4_start_us >= 1'000'000)
				{
					vk::ps4_gpu_liveness_in_wait();
				}

				if (now - ps4_start_us >= 2'000'000)
				{
					ps4_note_query_unavailable(index, spins);
					query_info.any_passed = true;
					query_info.ready = true;
					query_info.data = 1;
				}
			}
#else
			while (!query_info.ready)
			{
				utils::pause();
				poke_query(query_info, index, result_flags);
			}
#endif
		}

		return query_info.data;
	}

	void query_pool_manager::get_query_result_indirect(vk::command_buffer& cmd, u32 index, u32 count, VkBuffer dst, VkDeviceSize dst_offset)
	{
		// We're technically supposed to stop any active renderpasses before streaming the results out, but that doesn't matter on IMR hw
		// On TBDR setups like the apple M series, the stop is required (results are all 0 if you don't flush the RP), but this introduces a very heavy performance loss.
		vkCmdCopyQueryPoolResults(cmd, *query_slot_status[index].pool, index, count, dst, dst_offset, 4, VK_QUERY_RESULT_WAIT_BIT);
	}

	void query_pool_manager::free_query(vk::command_buffer&/*cmd*/, u32 index)
	{
		// Release reference and discard
		auto& query = query_slot_status[index];

		ensure(query.active);
		query.pool->release();

		if (!query.pool->has_refs())
		{
			// No more refs held, remove if in discard pile
			run_pool_cleanup();
		}

		query = {};
		m_available_slots.push_back(index);
	}

	u32 query_pool_manager::allocate_query(vk::command_buffer& cmd)
	{
#ifdef RPCS3_PS4
		if (s_ps4_no_query_pool.load(std::memory_order_relaxed))
		{
			return ~0u;
		}
#endif
		if (!m_pool_lifetime_counter)
		{
			// Pool is exhaused, create a new one
			// This is basically a driver-level pool reset without synchronization
			// TODO: Alternatively, use VK_EXT_host_pool_reset to reset an old pool with no references and swap that in
			if (vk::is_renderpass_open(cmd))
			{
#ifdef RPCS3_PS4
				const utils::ps4::gpu::rp_close_cause_scope ps4_cause(rsx::ps4_rp_close_query);
#endif
				vk::end_renderpass(cmd);
			}

			reallocate_pool(cmd);
#ifdef RPCS3_PS4
			if (!m_current_query_pool)
			{
				// The new pool could not be created (allocate_new_pool): no slot without a pool
				return ~0u;
			}
#endif
		}

		if (!m_available_slots.empty())
		{
			m_pool_lifetime_counter--;

			const auto result = m_available_slots.front();
			m_available_slots.pop_front();
			return result;
		}

		return ~0u;
	}

	void query_pool_manager::on_query_pool_released(std::unique_ptr<vk::query_pool>& pool)
	{
		if (!vk::force_reuse_query_pools())
		{
			// Delete and let the driver recreate a new pool each time.
			pool.reset();
			return;
		}

		std::lock_guard lock(m_query_pool_cache_lock);
		m_query_pool_cache.emplace_back(std::move(pool));
	}

	query_pool_manager::query_pool_ref::~query_pool_ref()
	{
		m_pool_man->on_query_pool_released(m_object);
	}
}
