#pragma once

#include "../VulkanAPI.h"
#include "device.h"
#include "sync.h"

#ifdef RPCS3_PS4
#include "util/atomic.hpp"
#endif

#include <span>

namespace vk
{
	class command_pool
	{
		vk::render_device* owner = nullptr;
		VkCommandPool pool       = nullptr;
		u32 queue_family         = 0;

	public:
		command_pool()  = default;
		~command_pool() = default;

		void create(vk::render_device& dev, u32 queue_family);
		void destroy();

		vk::render_device& get_owner() const;
		u32 get_queue_family() const;

		operator VkCommandPool() const;
	};

	struct queue_submit_t
	{
		VkQueue queue = VK_NULL_HANDLE;
		fence* pfence = nullptr;
		VkCommandBuffer commands = VK_NULL_HANDLE;
		std::array<VkSemaphore, 4> wait_semaphores {};
		std::array<VkSemaphore, 4> signal_semaphores {};
		std::array<VkPipelineStageFlags, 4> wait_stages {};
		u32 wait_semaphores_count = 0;
		u32 signal_semaphores_count = 0;

		queue_submit_t() = default;
		queue_submit_t(VkQueue queue_, vk::fence* fence_)
			: queue(queue_), pfence(fence_) {}

		queue_submit_t(const queue_submit_t& other)
		{
			std::memcpy(static_cast<void*>(this), &other, sizeof(queue_submit_t));
		}

		inline queue_submit_t& wait_on(VkSemaphore semaphore, VkPipelineStageFlags stage)
		{
			::at32(wait_semaphores, wait_semaphores_count) = semaphore;
			wait_stages[wait_semaphores_count++] = stage;
			return *this;
		}

		inline queue_submit_t& queue_signal(VkSemaphore semaphore)
		{
			::at32(signal_semaphores, signal_semaphores_count++) = semaphore;
			return *this;
		}
	};

	class command_buffer
	{
	protected:
		bool is_open = false;
		bool is_pending = false;
		fence* m_submit_fence = nullptr;

		command_pool* pool = nullptr;
		VkCommandBuffer commands = nullptr;

		// State cache
		mutable std::array<VkDescriptorSet, 2> m_bound_descriptor_sets {{ VK_NULL_HANDLE }};
		mutable std::array<VkPipeline, 2> m_bound_pipelines{{ VK_NULL_HANDLE }};

		void clear_state_cache();

	public:
		enum access_type_hint
		{
			flush_only, // Only to be submitted/opened/closed via command flush
			all         // Auxiliary, can be submitted/opened/closed at any time
		}
		access_hint = flush_only;

		enum command_buffer_data_flag : u32
		{
			cb_has_occlusion_task = 0x01,
			cb_has_blit_transfer  = 0x02,
			cb_has_dma_transfer   = 0x04,
			cb_has_open_query     = 0x08,
			cb_load_occluson_task = 0x10,
			cb_has_conditional_render = 0x20,
			cb_reload_dynamic_state   = 0x40
		};
		u32 flags = 0;

#ifdef RPCS3_PS4
		// PS4 0.28: DMA completion by recording generation. RADV on the console never makes DMA vk::events
		// visible to the host, but fences work, so every fence wait that covers a recording marks it complete.
		// record_gen advances each time recording starts; completed_gen is the newest recording known finished.
		// 0.32: written by the thread that owns the buffer and read by imp_flush on other threads, so atomic.
		// The wrapper keeps command_buffer copyable (VKAsyncScheduler stores them in a std::vector).
		struct ps4_gen_counter
		{
			atomic_t<u64> value{ 0 };

			ps4_gen_counter() = default;
			ps4_gen_counter(const ps4_gen_counter& other) : value(other.value.load()) {}
			ps4_gen_counter& operator=(const ps4_gen_counter& other) { value.store(other.value.load()); return *this; }
			operator u64() const { return value.load(); }
		};

		ps4_gen_counter ps4_record_gen;
		ps4_gen_counter ps4_completed_gen;
		void ps4_mark_completed() { ps4_completed_gen.value.store(ps4_record_gen); }

		// Any thread. True when recording 'gen' is known to have finished. Only the RSX thread, which owns the
		// primary buffers, may additionally wait up to 'timeout' us on a primary buffer's own fence; other threads
		// never touch m_submit_fence, because its owner may reset it at any time (vkResetFences needs external sync).
		bool ps4_recording_complete(u64 gen, u64 timeout);

		// Owner thread only. Waits up to timeout_us for this buffer's own submission (submit() without a fence).
		// On VK_SUCCESS the buffer is idle again: fence reset, recording marked complete. On any other result it
		// stays pending, so the next begin() waits for it (untimed, with the hang watchdog) instead of re-recording
		// memory the GPU may still read. The result is returned so callers can tell VK_ERROR_DEVICE_LOST apart.
		VkResult ps4_wait_pending(u64 timeout_us);
#endif

	public:
		command_buffer() = default;
		~command_buffer() = default;

		void create(command_pool& cmd_pool);
		void destroy();
		void reset();

		void begin();
		void end();
		void submit(queue_submit_t& submit_info, VkBool32 flush = VK_FALSE);

		// Abstractions with caching
		void bind_pipeline(VkPipeline pipeline, VkPipelineBindPoint bind_point) const;
		void bind_descriptor_sets(const std::span<VkDescriptorSet>& sets, VkPipelineBindPoint bind_point, VkPipelineLayout pipe_layout) const;
		void bind_descriptor_sets(const std::span<VkDescriptorSet>& sets, const std::span<u32>& dynamic_offsets, VkPipelineBindPoint bind_point, VkPipelineLayout pipe_layout) const;

		// Properties
		command_pool& get_command_pool() const { return *pool; }
		u32 get_queue_family() const { return pool->get_queue_family(); }
		bool is_recording() const { return is_open; }

		void clear_flags() { flags = 0; }
		void set_flag(command_buffer_data_flag flag) { flags |= flag; }

		operator VkCommandBuffer() const
		{
			return commands;
		}
	};
}
