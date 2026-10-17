#include "stdafx.h"
#include "VKCommandStream.h"
#include "VKResourceManager.h"
#include "vkutils/descriptors.h"
#include "vkutils/sync.h"

#include "Emu/IdManager.h"
#include "Emu/RSX/RSXOffload.h"
#include "Emu/RSX/RSXThread.h"
#include "Emu/system_config.h"

#ifdef RPCS3_PS4
#include "VKDMA.h"
#include "VKGSRender.h"

#include <atomic>
#endif

namespace vk
{
#ifdef RPCS3_PS4
	// 0.32: a failed vkQueueSubmit used to be silent. The fence of a failed submit never signals, so the next wait on
	// it hangs; the log line names the cause. Logged at the 1st, 2nd, 4th, 8th... failure.
	static void ps4_note_submit_failure(VkResult result)
	{
		static std::atomic<u32> s_failures{ 0 };
		if (const u32 count = s_failures.fetch_add(1, std::memory_order_relaxed) + 1; !(count & (count - 1)))
		{
			rsx_log.error("PS4: vkQueueSubmit failed with VkResult %d (failure %u)", static_cast<s32>(result), count);
		}

		if (result == VK_ERROR_DEVICE_LOST)
		{
			ps4_record_device_lost("queue submit");
		}
	}

	// 0.32: debug guard. Outside the RSX thread a submission is only expected from the RSX offloader and from an
	// access-violation flush while the RSX thread is parked for it (recording and descriptor state are not locked).
	static void ps4_check_submit_thread()
	{
		if (rsx::ps4_is_rsx_thread() || g_ps4_rsx_parked_for_flush || g_fxo->get<rsx::dma_manager>().is_current_thread())
		{
			return;
		}

		static std::atomic<u32> s_reports{ 0 };
		if (s_reports.fetch_add(1, std::memory_order_relaxed) < 8)
		{
			rsx_log.error("PS4: Vulkan submit from a non-RSX thread while the RSX runs (thread '%s')", thread_ctrl::get_name());
		}
	}
#endif

	// global submit guard to prevent race condition on queue submit
	shared_mutex g_submit_mutex;

	void acquire_global_submit_lock()
	{
		g_submit_mutex.lock();
	}

	void release_global_submit_lock()
	{
		g_submit_mutex.unlock();
	}

	FORCE_INLINE
	static void queue_submit_impl(const queue_submit_t& submit_info)
	{
		ensure(submit_info.pfence);
		acquire_global_submit_lock();
		VkSubmitInfo info
		{
			.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
			.pNext = nullptr,
			.waitSemaphoreCount = submit_info.wait_semaphores_count,
			.pWaitSemaphores = submit_info.wait_semaphores.data(),
			.pWaitDstStageMask = submit_info.wait_stages.data(),
			.commandBufferCount = 1,
			.pCommandBuffers = &submit_info.commands,
			.signalSemaphoreCount = submit_info.signal_semaphores_count,
			.pSignalSemaphores = submit_info.signal_semaphores.data()
		};

#ifdef RPCS3_PS4
		const VkResult result = vkQueueSubmit(submit_info.queue, 1, &info, submit_info.pfence->handle);
#else
		vkQueueSubmit(submit_info.queue, 1, &info, submit_info.pfence->handle);
#endif
		release_global_submit_lock();

		// Signal fence
		submit_info.pfence->signal_flushed();

#ifdef RPCS3_PS4
		if (result != VK_SUCCESS) [[unlikely]]
		{
			ps4_note_submit_failure(result);
		}
#endif
	}

	void queue_submit(const queue_submit_t& submit_info, VkBool32 flush)
	{
		rsx::get_current_renderer()->get_stats().submit_count++;

#ifdef RPCS3_PS4
		ps4_check_submit_thread();
#endif

		// Access to this method must be externally synchronized.
		// Offloader is guaranteed to never call this for async flushes.
#ifdef RPCS3_PS4
		{
			// 0.37: descriptor writes deferred to the submit, 'radv us/frame' (RSX thread only).
			const ps4_radv_scope ps4_radv(ps4_radv_descriptors);
			vk::descriptors::flush();
		}
#else
		vk::descriptors::flush();
#endif

		if (!flush && g_cfg.video.multithreaded_rsx)
		{
			auto packet = new queue_submit_t(submit_info);
			g_fxo->get<rsx::dma_manager>().backend_ctrl(rctrl_queue_submit, packet);
		}
		else
		{
			queue_submit_impl(submit_info);
		}
	}

	void queue_submit(const queue_submit_t* packet)
	{
		// Flush-only version used by asynchronous submit processing (MTRSX)
		queue_submit_impl(*packet);
	}

	void driver_manager_t::operator()()
	{
		while (thread_ctrl::state() != thread_state::aborting)
		{
			const auto wake_token = m_wake_event.observe();
			const auto last_eid = m_last_completed_eid.load();

			if (auto eid = m_eid_ctr.load(); eid != last_eid)
			{
				vk::get_resource_manager()->eid_completed(eid);

				m_last_completed_eid.store(eid);
				m_completed_signal++;
				m_completed_signal.notify_all();
			}

			thread_ctrl::wait_on(m_wake_event, wake_token);
		}
	}

	void driver_manager_t::notify_completed(u64 eid)
	{
		m_eid_ctr.atomic_op([eid](u64& value)
		{
			value = std::max(value, eid);
		});

		m_wake_event++;
		m_wake_event.notify_one();
	}

	void driver_manager_t::drain()
	{
		const u64 target_eid = m_eid_ctr.load(); //<- Last request at time of calling

		// Now, we spam queue wake until our EID is reached
		while (true)
		{
			const u32 completion_token = m_completed_signal.observe();
			if (m_last_completed_eid.load() >= target_eid)
			{
				// Abort, watermark reached.
				break;
			}

			if (thread_ctrl::state() == thread_state::aborting)
			{
				// Abort, emulation state changed.
				break;
			}

			// Wake the worker thread.
			m_wake_event++;
			m_wake_event.notify_one();

			// Wait for worker thread.
			thread_ctrl::wait_on(m_completed_signal, completion_token);
		}
	}
}
