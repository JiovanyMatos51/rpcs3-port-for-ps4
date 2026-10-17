#include "barriers.h"
#include "buffer_object.h"
#include "commands.h"
#include "device.h"
#include "garbage_collector.h"
#include "sync.h"
#include "shared.h"

#include "Emu/Cell/timers.hpp"

#include "util/sysinfo.hpp"
#include "util/asm.hpp"
#include "util/logs.hpp"

#ifdef RPCS3_PS4
#include "../VKDMA.h"
#include "Emu/RSX/RSXThread.h"
#include "ps4_fatal.h"
#include "ps4_launcher.h"
#include "util/ps4_adapt.hpp"
#include "util/ps4_gpu_health.hpp"
#include "util/ps4_i18n.hpp"
#include "util/ps4_vram_policy.hpp"

#include <atomic>
#include <chrono>
#include <thread>
#endif

namespace vk
{
#ifdef RPCS3_PS4
	// 0.39: a confirmed GPU hang (util/ps4_gpu_health.hpp hang_confirmed, or a fence wait of 10 s) stops the game and
	// returns to the XMB through the stop request of util/ps4_vram_policy.hpp, which the frontend serves as the VRAM
	// stop (notice in the XMB). The adapt key gpu_hang is noted for the title's next boots. The first request of a game
	// wins; a thread stuck in the hung wait keeps waiting, and the frontend opens the XMB after its stop timeout.
	// No-op with /data/rpcs3/no-hang-stop.flag. Any thread.
	void ps4_request_gpu_hang_stop(const char* why) noexcept
	{
		if (!utils::ps4::gpu::hang_stop())
		{
			return;
		}

		utils::ps4::adapt::note(utils::ps4::adapt::key_gpu_hang);

		if (!utils::ps4::vram::request_stop(utils::ps4::vram::stop_cause::gpu_hang, why))
		{
			return;
		}

		rsx_log.error("GPU: confirmed hang (%s): stopping to the XMB (no-hang-stop.flag keeps waiting)", why ? why : "");
		ps4_fatal_note("GPU: confirmed hang, stopping to the XMB: ", why ? why : "");
		ps4_notify(utils::ps4::i18n::tr(utils::ps4::i18n::str::TOAST_RPCS3_GPU_PAROU_RESPONDER_JOGO));

		// The frontend's regular stop (as L1+R1+Triangle): GracefulShutdown, then the XMB.
		ps4_request_return_to_menu();
	}

	void ps4_gpu_wait_backoff(u32 iteration)
	{
		// Callers count from 0 and increase by 1 per call, so iteration == 64 happens once per wait.
		thread_local u64 s_yield_start_us = 0;

		// Most GPU waits complete within a few polls; keep those fast.
		if (iteration < 64)
		{
			utils::pause();
			return;
		}

		if (iteration == 64)
		{
			s_yield_start_us = get_system_time();
		}

		// 0.29: since 0.26 the RSX runs one step above the PPU/SPU threads by default
		// (692 vs 700). A yield at that fixed priority never runs a guest thread, so for the raised RSX the
		// yield phase below was a ~1 ms busy spin with a syscall per poll. It now spins without syscalls,
		// then takes the (tick-quantized) sleep. Other callers (PPU/SPU threads in the access-violation
		// flush, and the RSX with rsx-normal-priority.flag) keep the yield phase: there a yield does hand
		// the core to equal-priority threads (round-robin since 0.26).
		// The spin is bounded by time (>= 200 us, like the FIFO-idle poll), not by 256 polls
		// (only tens of us): otherwise every hard sync / ZCULL readback / CB recycle the GPU finishes after
		// that turned into a whole ~1 ms tick on the RSX's critical path, which 0.28 caught at once.
		if (rsx::ps4_rsx_priority_elevated() && rsx::ps4_is_rsx_thread())
		{
			if (iteration < 256 || get_system_time() - s_yield_start_us < 200)
			{
				utils::pause();
				return;
			}

			std::this_thread::sleep_for(std::chrono::microseconds(50));
			return;
		}

		// Equal priorities: yield gives the core to every runnable PPU/SPU thread. Sleep only after
		// >= 1 ms of yielding (like utils::progress_backoff): an Orbis sleep lasts about a whole kernel
		// tick (~1 ms) and would otherwise add it to nearly every hard sync / ZCULL readback.
		if (iteration < 128 || get_system_time() - s_yield_start_us < 1000)
		{
			std::this_thread::yield();
			return;
		}

		// Long wait (GPU genuinely busy for > ~1 ms): release the core.
		std::this_thread::sleep_for(std::chrono::microseconds(50));
	}

	// 0.31: hang reports of the process. startup.log only gets the first few, so a game that keeps stalling the
	// GPU cannot flood it.
	static std::atomic<u32> s_ps4_fence_hang_reports{ 0 };

	void ps4_fence_watchdog::check()
	{
		const u64 now = get_system_time();

		// Only count time the wait could really have been served. The clock restarts at the first check, after a
		// gap of more than 2 s between two checks (normally tens of ms apart, so the process was suspended, e.g.
		// rest mode) and while the app is in the background.
		if (!m_start_us || now - m_last_us > 2'000'000 || ps4_in_background())
		{
			m_start_us = now;
			m_last_us = now;
			return;
		}

		m_last_us = now;

		// 0.32: a GPU fault can get the process killed long before the 10 s report below (0.31 died without one). After
		// 1 s, record once per wait where the readback trace stands; a no-op while no readback was traced.
		if (!m_trace_snapshot_taken && now - m_start_us >= 1'000'000)
		{
			m_trace_snapshot_taken = true;
			ps4_dma_trace_snapshot("fence wait over 1 s");
		}

		if (m_reported || now - m_start_us < utils::ps4::gpu::fence_hang_us)
		{
			return;
		}

		m_reported = true;
		const u32 reports = s_ps4_fence_hang_reports.fetch_add(1, std::memory_order_relaxed) + 1;
		const bool rsx_thread = rsx::ps4_is_rsx_thread();

		rsx_log.error("GPU: fence not signalled for %llu s on the %s thread; still waiting (possible GPU hang, report %u)",
			(now - m_start_us) / 1'000'000, rsx_thread ? "RSX" : "a non-RSX", reports);

		if (reports <= 8)
		{
			ps4_fatal_note("GPU: fence not signalled for 10 s - possible GPU hang; still waiting on ", rsx_thread ? "the RSX thread" : "a non-RSX thread");
		}

		// 0.32: if a readback was traced, name the step the GPU finished last.
		ps4_dma_trace_report("fence watchdog");

		// Recorded for the game; the boot policy only reports this key.
		utils::ps4::adapt::note(utils::ps4::adapt::key_gpu_hang);

		const bool raised = utils::ps4::gpu::set_suspect_hang(true);

		if (utils::ps4::gpu::hang_stop())
		{
			// 0.39: the game is stopped and the XMB opens (its toast replaces the one below).
			ps4_request_gpu_hang_stop(rsx_thread ? "a fence of the RSX thread was not signalled for 10 s" : "a fence was not signalled for 10 s");
		}
		else if (raised && utils::ps4::gpu::hang_reports() <= 3)
		{
			// One toast per raised flag: a second waiter on the same hang does not repeat it, and a game that keeps
			// stalling gets at most three.
			ps4_notify(utils::ps4::i18n::tr(utils::ps4::i18n::str::TOAST_RPCS3_GPU_NAO_RESPONDE_HA));
		}
	}

	void ps4_fence_watchdog::recovered()
	{
		m_reported = false;
		utils::ps4::gpu::set_suspect_hang(false);

		rsx_log.warning("GPU: fence signalled after about %llu s; the GPU responds again", (get_system_time() - m_start_us) / 1'000'000);

		if (s_ps4_fence_hang_reports.load(std::memory_order_relaxed) <= 8)
		{
			ps4_fatal_note("GPU: fence signalled again after a hang report", "");
		}
	}
#endif

	namespace globals
	{
		static std::unique_ptr<gpu_debug_marker_pool> g_gpu_debug_marker_pool;
		static std::unique_ptr<gpu_label_pool> g_gpu_label_pool;

		gpu_debug_marker_pool& get_shared_marker_pool(const vk::render_device& dev)
		{
			if (!g_gpu_debug_marker_pool)
			{
				g_gpu_debug_marker_pool = std::make_unique<gpu_debug_marker_pool>(dev, 65536);
				vk::get_gc()->add_exit_callback([]()
				{
					g_gpu_debug_marker_pool.reset();
				});
			}

			return *g_gpu_debug_marker_pool;
		}

#ifdef RPCS3_PS4
		// 0.32: labels are the default host DMA events here, and the first ones can be created by the RSX thread and a
		// faulting thread at the same time; two pools would leave labels pointing into a destroyed buffer.
		static std::mutex g_ps4_label_pool_mutex;
#endif

		gpu_label_pool& get_shared_label_pool(const vk::render_device& dev)
		{
#ifdef RPCS3_PS4
			std::lock_guard lock(g_ps4_label_pool_mutex);
#endif
			if (!g_gpu_label_pool)
			{
				g_gpu_label_pool = std::make_unique<gpu_label_pool>(dev, 65536);
				vk::get_gc()->add_exit_callback([]()
				{
					g_gpu_label_pool.reset();
				});
			}

			return *g_gpu_label_pool;
		}
	}

	// Util
	namespace v1_utils
	{
		VkPipelineStageFlags gather_src_stages(const VkDependencyInfoKHR& dependency)
		{
			VkPipelineStageFlags stages = VK_PIPELINE_STAGE_NONE;
			for (u32 i = 0; i < dependency.bufferMemoryBarrierCount; ++i)
			{
				stages |= dependency.pBufferMemoryBarriers[i].srcStageMask;
			}
			for (u32 i = 0; i < dependency.imageMemoryBarrierCount; ++i)
			{
				stages |= dependency.pImageMemoryBarriers[i].srcStageMask;
			}
			for (u32 i = 0; i < dependency.memoryBarrierCount; ++i)
			{
				stages |= dependency.pMemoryBarriers[i].srcStageMask;
			}
			return stages;
		}

		VkPipelineStageFlags gather_dst_stages(const VkDependencyInfoKHR& dependency)
		{
			VkPipelineStageFlags stages = VK_PIPELINE_STAGE_NONE;
			for (u32 i = 0; i < dependency.bufferMemoryBarrierCount; ++i)
			{
				stages |= dependency.pBufferMemoryBarriers[i].dstStageMask;
			}
			for (u32 i = 0; i < dependency.imageMemoryBarrierCount; ++i)
			{
				stages |= dependency.pImageMemoryBarriers[i].dstStageMask;
			}
			for (u32 i = 0; i < dependency.memoryBarrierCount; ++i)
			{
				stages |= dependency.pMemoryBarriers[i].dstStageMask;
			}
			return stages;
		}

		auto get_memory_barriers(const VkDependencyInfoKHR& dependency)
		{
			std::vector<VkMemoryBarrier> result;
			for (u32 i = 0; i < dependency.memoryBarrierCount; ++i)
			{
				result.push_back
				({
					VK_STRUCTURE_TYPE_MEMORY_BARRIER,
					nullptr,
					static_cast<VkAccessFlags>(dependency.pMemoryBarriers[i].srcAccessMask),
					static_cast<VkAccessFlags>(dependency.pMemoryBarriers[i].dstAccessMask)
				});
			}
			return result;
		}

		auto get_image_memory_barriers(const VkDependencyInfoKHR& dependency)
		{
			std::vector<VkImageMemoryBarrier> result;
			for (u32 i = 0; i < dependency.imageMemoryBarrierCount; ++i)
			{
				result.push_back
				({
					VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
					nullptr,
					static_cast<VkAccessFlags>(dependency.pImageMemoryBarriers[i].srcAccessMask),
					static_cast<VkAccessFlags>(dependency.pImageMemoryBarriers[i].dstAccessMask),
					dependency.pImageMemoryBarriers[i].oldLayout,
					dependency.pImageMemoryBarriers[i].newLayout,
					dependency.pImageMemoryBarriers[i].srcQueueFamilyIndex,
					dependency.pImageMemoryBarriers[i].dstQueueFamilyIndex,
					dependency.pImageMemoryBarriers[i].image,
					dependency.pImageMemoryBarriers[i].subresourceRange
				});
			}
			return result;
		}

		auto get_buffer_memory_barriers(const VkDependencyInfoKHR& dependency)
		{
			std::vector<VkBufferMemoryBarrier> result;
			for (u32 i = 0; i < dependency.bufferMemoryBarrierCount; ++i)
			{
				result.push_back
				({
					VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
					nullptr,
					static_cast<VkAccessFlags>(dependency.pBufferMemoryBarriers[i].srcAccessMask),
					static_cast<VkAccessFlags>(dependency.pBufferMemoryBarriers[i].dstAccessMask),
					dependency.pBufferMemoryBarriers[i].srcQueueFamilyIndex,
					dependency.pBufferMemoryBarriers[i].dstQueueFamilyIndex,
					dependency.pBufferMemoryBarriers[i].buffer,
					dependency.pBufferMemoryBarriers[i].offset,
					dependency.pBufferMemoryBarriers[i].size
				});
			}
			return result;
		}
	}

	// Objects
	fence::fence(VkDevice dev)
	{
		owner                  = dev;
		VkFenceCreateInfo info = {};
		info.sType             = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		CHECK_RESULT(vkCreateFence(dev, &info, nullptr, &handle));
	}

	fence::~fence()
	{
		if (handle)
		{
			vkDestroyFence(owner, handle, nullptr);
			handle = VK_NULL_HANDLE;
		}
	}

	void fence::reset()
	{
		vkResetFences(owner, 1, &handle);
		flushed.release(false);
	}

	void fence::signal_flushed()
	{
		flushed.release(true);
	}

	void fence::wait_flush()
	{
#ifdef RPCS3_PS4
		// The flush is signalled by the submitting thread, which may have a lower fixed priority.
		for (u32 spins = 0; !flushed; spins++)
		{
			ps4_gpu_wait_backoff(spins);
		}
#else
		while (!flushed)
		{
			utils::pause();
		}
#endif
	}

	fence::operator bool() const
	{
		return (handle != VK_NULL_HANDLE);
	}

	semaphore::semaphore(const render_device& dev)
		: m_device(dev)
	{
		VkSemaphoreCreateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
		CHECK_RESULT(vkCreateSemaphore(m_device, &info, nullptr, &m_handle));
	}

	semaphore::~semaphore()
	{
		vkDestroySemaphore(m_device, m_handle, nullptr);
	}

	semaphore::operator VkSemaphore() const
	{
		return m_handle;
	}

	event::event(const render_device& dev, sync_domain domain)
		: m_device(&dev), m_domain(domain)
	{
		m_backend = dev.get_synchronization2_support()
			? sync_backend::events_v2
			: sync_backend::events_v1;

#ifdef RPCS3_PS4
		// 0.32: the console's GCN GPU runs RADV ORBIS, which reports an unknown chip, so the AMD check below never
		// selects labels. Host-visible VkEvents are set by an EOP write into a freshly created event BO, which the
		// host has never seen set on this console. dma.flag events=vk restores them.
		const bool ps4_use_label = vk::ps4_dma_events_use_labels();
#else
		constexpr bool ps4_use_label = false;
#endif

		if (domain == sync_domain::host &&
			(ps4_use_label ||
			(vk::get_driver_vendor() == vk::driver_vendor::AMD &&
			vk::get_chip_family() < vk::chip_class::AMD_navi1x)))
		{
			// Events don't work quite right on AMD drivers
			m_backend = sync_backend::gpu_label;

			m_label = std::make_unique<vk::gpu_label>(globals::get_shared_label_pool(dev));
			return;
		}

		VkEventCreateInfo info
		{
			.sType = VK_STRUCTURE_TYPE_EVENT_CREATE_INFO,
			.pNext = nullptr,
			.flags = 0
		};

		if (domain == sync_domain::gpu && m_backend == sync_backend::events_v2)
		{
			info.flags = VK_EVENT_CREATE_DEVICE_ONLY_BIT_KHR;
		}

		CHECK_RESULT(vkCreateEvent(dev, &info, nullptr, &m_vk_event));
	}

	event::~event()
	{
		if (m_vk_event) [[likely]]
		{
			vkDestroyEvent(*m_device, m_vk_event, nullptr);
		}
	}

	void event::resolve_dependencies(const command_buffer& cmd, const VkDependencyInfoKHR& dependency)
	{
		ensure(m_backend != sync_backend::gpu_label);

		if (m_backend == sync_backend::events_v2)
		{
			_vkCmdPipelineBarrier2KHR(cmd, &dependency);
			return;
		}

		const auto src_stages = v1_utils::gather_src_stages(dependency);
		const auto dst_stages = v1_utils::gather_dst_stages(dependency);
		const auto memory_barriers = v1_utils::get_memory_barriers(dependency);
		const auto image_memory_barriers = v1_utils::get_image_memory_barriers(dependency);
		const auto buffer_memory_barriers = v1_utils::get_buffer_memory_barriers(dependency);

		vkCmdPipelineBarrier(cmd, src_stages, dst_stages, dependency.dependencyFlags,
			::size32(memory_barriers), memory_barriers.data(),
			::size32(buffer_memory_barriers), buffer_memory_barriers.data(),
			::size32(image_memory_barriers), image_memory_barriers.data());
	}

	void event::signal(const command_buffer& cmd, const VkDependencyInfoKHR& dependency)
	{
		if (m_backend == sync_backend::gpu_label)
		{
			// Fallback path
			m_label->signal(cmd, dependency);
			return;
		}

		if (m_domain != sync_domain::host)
		{
			// As long as host is not involved, keep things consistent.
			// The expectation is that this will be awaited using the gpu_wait function.
			if (m_backend == sync_backend::events_v2) [[ likely ]]
			{
				_vkCmdSetEvent2KHR(cmd, m_vk_event, &dependency);
			}
			else
			{
				const auto dst_stages = v1_utils::gather_dst_stages(dependency);
				vkCmdSetEvent(cmd, m_vk_event, dst_stages);
			}

			return;
		}

		// Host sync doesn't behave intuitively with events, so we use some workarounds.
		// 1. Resolve the actual dependencies on a pipeline barrier.
		resolve_dependencies(cmd, dependency);

		// 2. Signalling won't wait. The caller is responsible for setting up the dependencies correctly.
		if (m_backend != sync_backend::events_v2)
		{
			vkCmdSetEvent(cmd, m_vk_event, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
			return;
		}

		// We need a memory barrier to keep AMDVLK from hanging
		VkMemoryBarrier2KHR mem_barrier =
		{
			.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2_KHR,
			.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT_KHR,
			.srcAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT
		};

		// Empty dependency that does nothing
		VkDependencyInfoKHR empty_dependency
		{
			.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO_KHR,
			.memoryBarrierCount = 1,
			.pMemoryBarriers = &mem_barrier
		};

		_vkCmdSetEvent2KHR(cmd, m_vk_event, &empty_dependency);
	}

	void event::host_signal() const
	{
		if (m_backend != sync_backend::gpu_label) [[ likely ]]
		{
			vkSetEvent(*m_device, m_vk_event);
			return;
		}

		m_label->set();
	}

	void event::gpu_wait(const command_buffer& cmd, const VkDependencyInfoKHR& dependency) const
	{
		ensure(m_domain != sync_domain::host);

		if (m_backend == sync_backend::events_v2) [[ likely ]]
		{
			_vkCmdWaitEvents2KHR(cmd, 1, &m_vk_event, &dependency);
			return;
		}

		const auto src_stages = v1_utils::gather_src_stages(dependency);
		const auto dst_stages = v1_utils::gather_dst_stages(dependency);
		const auto memory_barriers = v1_utils::get_memory_barriers(dependency);
		const auto image_memory_barriers = v1_utils::get_image_memory_barriers(dependency);
		const auto buffer_memory_barriers = v1_utils::get_buffer_memory_barriers(dependency);

		vkCmdWaitEvents(cmd,
			1, &m_vk_event,
			src_stages, dst_stages,
			::size32(memory_barriers), memory_barriers.data(),
			::size32(buffer_memory_barriers), buffer_memory_barriers.data(),
			::size32(image_memory_barriers), image_memory_barriers.data());
	}

	void event::reset() const
	{
		if (m_backend != sync_backend::gpu_label) [[ likely ]]
		{
			vkResetEvent(*m_device, m_vk_event);
			return;
		}

		m_label->reset();
	}

	VkResult event::status() const
	{
		if (m_backend != sync_backend::gpu_label) [[ likely ]]
		{
			return vkGetEventStatus(*m_device, m_vk_event);
		}

		return m_label->signaled() ? VK_EVENT_SET : VK_EVENT_RESET;
	}

	gpu_label_pool::gpu_label_pool(const vk::render_device& dev, u32 count)
		: pdev(&dev), m_count(count)
	{}

	gpu_label_pool::~gpu_label_pool()
	{
		if (m_mapped)
		{
			ensure(m_buffer);
			m_buffer->unmap();
		}

#ifdef RPCS3_PS4
		// Destroyed with the device's global resources, after the labels.
		for (auto& retired : m_ps4_retired)
		{
			retired.buf->unmap();
		}
#endif
	}

	std::tuple<VkBuffer, u64, volatile u32*> gpu_label_pool::allocate()
	{
		if (!m_buffer || m_offset >= m_count)
		{
			create_impl();
		}

		const auto out_offset = m_offset;
		m_offset ++;
		return { m_buffer->value, out_offset * 4, m_mapped + out_offset };
	}

#ifdef RPCS3_PS4
	std::tuple<VkBuffer, u64, volatile u32*> gpu_label_pool::ps4_allocate(std::shared_ptr<void>& keep_alive)
	{
		// 0.32: the slot counter and the wrap are not atomic. Two readbacks recorded at once (the RSX thread and a
		// faulting thread whose flush request a hard sync served without parking the RSX) could share a slot, so one
		// section's event would report the other's copy as done, or see the pool mid-wrap.
		std::unique_lock lock(m_ps4_mutex);

		while (!m_buffer || m_offset >= m_count)
		{
			// The next buffer is created without the lock: its allocation can run the video memory pressure handler,
			// which may wait for a lock (the texture cache's) held by a thread that is itself waiting here.
			lock.unlock();
			auto [next, next_mapped] = ps4_create_buffer();
			lock.lock();

			if (!m_buffer || m_offset >= m_count)
			{
				ps4_install_buffer(std::move(next), next_mapped);
			}
			else
			{
				// Another thread wrapped the pool meanwhile; this buffer was never handed out.
				next->unmap();
			}
		}

		const auto out_offset = m_offset++;
		keep_alive = m_ps4_users;
		return { m_buffer->value, out_offset * 4, m_mapped + out_offset };
	}

	std::pair<std::unique_ptr<buffer>, volatile u32*> gpu_label_pool::ps4_create_buffer() const
	{
		auto next = std::make_unique<buffer>
		(
			*pdev,
			m_count * 4,
			pdev->get_memory_mapping().host_visible_coherent,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
			VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			0,
			VMM_ALLOCATION_POOL_SYSTEM
		);

		const auto mapped = reinterpret_cast<volatile u32*>(next->map(0, VK_WHOLE_SIZE));
		return { std::move(next), mapped };
	}

	void gpu_label_pool::ps4_install_buffer(std::unique_ptr<buffer>&& next, volatile u32* next_mapped)
	{
		if (m_buffer)
		{
			// 0.32: live labels still point into this mapping (upstream unmapped it here). Keep it mapped until its
			// last label is gone; retired buffers are released on later wraps.
			m_ps4_retired.push_back({ std::move(m_buffer), std::move(m_ps4_users) });
			ps4_release_unused_buffers();
		}

		m_buffer = std::move(next);
		m_mapped = next_mapped;
		m_offset = 0;
		m_ps4_users = std::make_shared<u8>(u8{0});
	}

	void gpu_label_pool::ps4_release_unused_buffers()
	{
		// Runs under m_ps4_mutex where labels are allocated, so no new reference to a retired buffer can appear
		// meanwhile; a label destroyed concurrently (e.g. by the garbage collector thread) is caught by the next pass.
		std::erase_if(m_ps4_retired, [](ps4_retired_buffer& retired)
		{
			if (retired.users.use_count() > 1)
			{
				return false;
			}

			// use_count() is a relaxed read: order the unmap after the last label's final access to the mapping.
			std::atomic_thread_fence(std::memory_order_acquire);
			retired.buf->unmap();
			vk::get_gc()->dispose(retired.buf);
			return true;
		});
	}
#endif

	void gpu_label_pool::create_impl()
	{
#ifdef RPCS3_PS4
		// Labels use ps4_allocate(); this keeps allocate() consistent for a single-threaded caller.
		auto [next, next_mapped] = ps4_create_buffer();
		ps4_install_buffer(std::move(next), next_mapped);
#else
		if (m_buffer)
		{
			m_buffer->unmap();
			vk::get_gc()->dispose(m_buffer);
		}

		m_buffer = std::make_unique<buffer>
		(
			*pdev,
			m_count * 4,
			pdev->get_memory_mapping().host_visible_coherent,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
			VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			0,
			VMM_ALLOCATION_POOL_SYSTEM
		);

		m_mapped = reinterpret_cast<volatile u32*>(m_buffer->map(0, VK_WHOLE_SIZE));
		m_offset = 0;
#endif
	}

	gpu_label::gpu_label(gpu_label_pool& pool)
	{
#ifdef RPCS3_PS4
		std::tie(m_buffer_handle, m_buffer_offset, m_ptr) = pool.ps4_allocate(m_ps4_keep_alive);
#else
		std::tie(m_buffer_handle, m_buffer_offset, m_ptr) = pool.allocate();
#endif
		reset();
	}

	gpu_label::~gpu_label()
	{
		m_ptr = nullptr;
		m_buffer_offset = 0;
		m_buffer_handle = VK_NULL_HANDLE;
	}

	void gpu_label::signal(const vk::command_buffer& cmd, const VkDependencyInfoKHR& dependency)
	{
		const auto src_stages = v1_utils::gather_src_stages(dependency);
		auto dst_stages = v1_utils::gather_dst_stages(dependency);
		auto memory_barriers = v1_utils::get_memory_barriers(dependency);
		const auto image_memory_barriers = v1_utils::get_image_memory_barriers(dependency);
		const auto buffer_memory_barriers = v1_utils::get_buffer_memory_barriers(dependency);

		// Ensure wait before filling the label
		dst_stages |= VK_PIPELINE_STAGE_TRANSFER_BIT;
		if (memory_barriers.empty())
		{
			VkMemoryBarrier signal_barrier =
			{
				.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
				.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
				.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT
			};
			memory_barriers.push_back(std::move(signal_barrier));
		}
		else
		{
			auto& barrier = memory_barriers.front();
			barrier.dstAccessMask |= VK_ACCESS_TRANSFER_WRITE_BIT;
		}

		vkCmdPipelineBarrier(cmd, src_stages, dst_stages, dependency.dependencyFlags,
			::size32(memory_barriers), memory_barriers.data(),
			::size32(buffer_memory_barriers), buffer_memory_barriers.data(),
			::size32(image_memory_barriers), image_memory_barriers.data());

		vkCmdFillBuffer(cmd, m_buffer_handle, m_buffer_offset, 4, label_constants::set_);
	}

	gpu_debug_marker::gpu_debug_marker(gpu_debug_marker_pool& pool, std::string message)
		: gpu_label(pool), m_message(std::move(message))
	{}

	gpu_debug_marker::~gpu_debug_marker()
	{
		if (!m_printed)
		{
			dump();
		}
	}

	void gpu_debug_marker::dump()
	{
		if (*m_ptr == gpu_label::label_constants::reset_)
		{
			rsx_log.error("DEBUG MARKER NOT REACHED: %s", m_message);
		}

		m_printed = true;
	}

	void gpu_debug_marker::dump() const
	{
		if (*m_ptr == gpu_label::label_constants::reset_)
		{
			rsx_log.error("DEBUG MARKER NOT REACHED: %s", m_message);
		}
		else
		{
			rsx_log.error("DEBUG MARKER: %s", m_message);
		}
	}

	void gpu_debug_marker::insert(
		const vk::render_device& dev,
		const vk::command_buffer& cmd,
		std::string message,
		VkPipelineStageFlags stages,
		VkAccessFlags access)
	{
		VkMemoryBarrier2KHR barrier =
		{
			.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2_KHR,
			.srcStageMask = stages,
			.srcAccessMask = access,
			.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT_KHR,
			.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT
		};

		VkDependencyInfoKHR dependency =
		{
			.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO_KHR,
			.memoryBarrierCount = 1,
			.pMemoryBarriers = &barrier
		};

		auto result = std::make_unique<gpu_debug_marker>(globals::get_shared_marker_pool(dev), message);
		result->signal(cmd, dependency);
		vk::get_gc()->dispose(result);
	}

	debug_marker_scope::debug_marker_scope(const vk::command_buffer& cmd, const std::string& message)
		: m_device(&cmd.get_command_pool().get_owner()), m_cb(&cmd), m_message(message), m_tag(rsx::get_shared_tag())
	{
		vk::gpu_debug_marker::insert(
			*m_device,
			*m_cb,
			fmt::format("0x%llx: Enter %s", m_tag, m_message)
		);
	}

	debug_marker_scope::~debug_marker_scope()
	{
		ensure(m_cb && m_cb->is_recording());

		vk::gpu_debug_marker::insert(
			*m_device,
			*m_cb,
			fmt::format("0x%x: Exit %s", m_tag, m_message)
		);
	}

	VkResult wait_for_fence(fence* pFence, u64 timeout)
	{
#ifdef RPCS3_PS4
		// 0.29: STALL dump wait reason (no-op off the RSX thread).
		const rsx::ps4_rsx_wait_scope ps4_wait(rsx::ps4_wait_vk_fence);
#endif
		pFence->wait_flush();

		if (timeout)
		{
#ifdef RPCS3_PS4
			const VkResult result = vkWaitForFences(*g_render_device, 1, &pFence->handle, VK_FALSE, timeout * 1000ull);

			if (result == VK_TIMEOUT)
			{
				// 0.29: per 10 s in the 'RSX gameplay' line ('fence timeouts').
				rsx::g_ps4_rsx_counters.fence_timeouts++;
			}

			return result;
#else
			return vkWaitForFences(*g_render_device, 1, &pFence->handle, VK_FALSE, timeout * 1000ull);
#endif
		}
		else
		{
#ifdef RPCS3_PS4
			u32 spins = 0;
			// 0.31: this wait has no deadline, so a GPU hang used to freeze the game without a word.
			ps4_fence_watchdog watchdog;
#endif
			while (auto status = vkGetFenceStatus(*g_render_device, pFence->handle))
			{
				switch (status)
				{
				case VK_NOT_READY:
#ifdef RPCS3_PS4
					// Untimed wait (command_buffer::begin, flushes): back off instead of spinning.
					watchdog.poll(spins);
					ps4_gpu_wait_backoff(spins++);
#else
					utils::pause();
#endif
					continue;
				default:
					die_with_error(status);
					return status;
				}
			}

#ifdef RPCS3_PS4
			watchdog.done();
#endif
			return VK_SUCCESS;
		}
	}

	VkResult wait_for_event(event* pEvent, u64 timeout)
	{
		// Convert timeout to TSC cycles. Timeout accuracy isn't super-important, only fast response when event is signaled (within 10us if possible)
		const u64 freq = utils::get_tsc_freq();

		if (freq)
		{
			timeout *= (freq / 1'000'000);
		}

		u64 start = 0;
#ifdef RPCS3_PS4
		u32 spins = 0;
		// 0.29: STALL dump wait reason (no-op off the RSX thread).
		const rsx::ps4_rsx_wait_scope ps4_wait(rsx::ps4_wait_vk_event);
#endif

		while (true)
		{
			switch (const auto status = pEvent->status())
			{
			case VK_EVENT_SET:
				return VK_SUCCESS;
			case VK_EVENT_RESET:
				break;
			default:
				die_with_error(status);
				return status;
			}

			if (timeout)
			{
				const auto now = freq ? utils::get_tsc() : get_system_time();

				if (!start)
				{
					start = now;
					continue;
				}

				if ((now > start) &&
					(now - start) > timeout)
				{
					rsx_log.error("[vulkan] vk::wait_for_event has timed out!");
#ifdef RPCS3_PS4
					// 0.29: exact count per 10 s ('event timeouts' in the 'RSX gameplay' line).
					rsx::g_ps4_rsx_counters.event_timeouts++;
#endif
					return VK_TIMEOUT;
				}
			}

#ifdef RPCS3_PS4
			// The timeout above is time-based, so backing off does not change it.
			ps4_gpu_wait_backoff(spins++);
#else
			utils::pause();
#endif
		}
	}
}
