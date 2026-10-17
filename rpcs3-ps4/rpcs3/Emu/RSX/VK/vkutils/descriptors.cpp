#include "Emu/IdManager.h"
#include "descriptors.h"
#include "garbage_collector.h"
#include "util/logs.hpp"

#ifdef RPCS3_PS4
#include "memory.h"
#include "ps4_fatal.h"
#include "util/ps4_gpu_health.hpp"
#include "util/ps4_rsx_programs.hpp"
#include "Emu/RSX/RSXThread.h"
#include "../VKHelpers.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <new>
#include <utility>
#endif

namespace vk
{
	// Error handler callback
	extern void on_descriptor_pool_fragmentation(bool fatal);

#ifdef RPCS3_PS4
	// VKProgramPipeline.cpp (0.38): the pipe compiler's nullable-link flag of this thread.
	bool ps4_link_nullable() noexcept;

	// Declared for the PS4 Vulkan files in VKPipelineCompiler.h; defined below.
	void ps4_reclaim_for_driver(VkResult result, const char* what, bool defer_trim) noexcept;
	bool ps4_set_descriptor_nullable(bool nullable) noexcept;
	u64 ps4_descriptor_sets_refused() noexcept;

	namespace
	{
		std::atomic<u32> s_ps4_descriptor_sets{ 0 };
		std::atomic<u64> s_ps4_descriptor_bytes{ 0 };
		std::atomic<u32> s_ps4_descriptor_pools{ 0 };
		std::atomic<u64> s_ps4_descriptor_pool_window{ 0 };
		std::atomic<u64> s_ps4_descriptor_pool_window_releasing{ 0 }; // pools handed to the GC, not destroyed yet
		std::atomic<u64> s_ps4_descriptor_spills{ 0 };
		std::atomic<u32> s_ps4_driver_reclaims{ 0 };

		// 0.38: set by the draw path around its descriptor commits (ps4_set_descriptor_nullable); the refusal streak
		// of those allocations (RSX thread only) and the sets refused.
		thread_local bool t_ps4_set_nullable = false;
		utils::ps4::rsx_programs::set_refusals s_ps4_set_refusals;
		std::atomic<u64> s_ps4_sets_refused{ 0 };

		u64 ps4_steady_us()
		{
			return static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
		}

		// GPU window of one VkDescriptorPool sized for set_count sets of these descriptors (RADV's per-type sizes).
		u64 ps4_estimate_pool_window(const rsx::simple_array<VkDescriptorPoolSize>& sizes, u32 set_count)
		{
			u64 bytes = 0;
			u64 descriptors_16 = 0;

			for (const auto& size : sizes)
			{
				const u64 count = u64{ size.descriptorCount } * set_count;
				switch (size.type)
				{
				case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
					bytes += 96 * count;
					break;
				case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
				case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
				case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
					bytes += 64 * count;
					break;
				default:
					bytes += 16 * count;
					descriptors_16 += count;
					break;
				}
			}

			return utils::ps4::gpu::descriptor_pool_window_bytes(bytes, descriptors_16, set_count);
		}

		// simple_array keeps its heap block while empty; destroying and re-creating it is the only way to free it.
		template <typename T>
		void ps4_free_array(rsx::simple_array<T>& array)
		{
			array.~simple_array();
			new (&array) rsx::simple_array<T>();
		}

		// The driver refused a new descriptor pool for a reason other than fragmentation (die_with_error follows
		// if the retries fail too). RADV takes the pool's host memory from the CPU heap and its descriptor buffer
		// from the GPU window (see ps4_reclaim_for_driver). 0.36 behaviour: the trim may run right here.
		void ps4_release_memory_for_pool(VkResult result)
		{
			vk::ps4_reclaim_for_driver(result, "descriptor pool creation", false);
		}

		// 0.38: a descriptor pool or set the current draw needs was refused again after the first reclaim: the deepest
		// reclaim allowed here (the full RSX-thread reclaim outside uninterruptible sections, otherwise finished work
		// retired and a fatal trim queued for the frame end; the CPU heap's new_handler again), before a last attempt.
		void ps4_last_reclaim(VkResult result, const char* what)
		{
			if (result != VK_ERROR_OUT_OF_DEVICE_MEMORY)
			{
				if (const std::new_handler handler = std::get_new_handler())
				{
					handler();
				}
			}

			if (!vk::ps4_vram_deep_reclaim(what))
			{
				vk::ps4_vram_relieve_deferred(rsx::problem_severity::fatal);
			}
		}

		// The last attempt failed too: the game is stopped through the regular path (XMB notice, vram=low for its next
		// boots) before the error that ends this draw. Allocation-free.
		void ps4_stop_for(VkResult result, const char* what)
		{
			char detail[160];
			std::snprintf(detail, sizeof(detail), "%s failed after two memory reclaims (VkResult %d)", what, static_cast<int>(result));
			vk::ps4_request_vram_stop(detail);
		}

		// A draw's set was refused: the draw is skipped (VKDraw.cpp). Allocation-free note (the first 16 and then every
		// power of two); refusals that keep coming for set_refusal_stop_us request the graceful stop.
		void ps4_refuse_set(VkResult result)
		{
			const u64 count = s_ps4_sets_refused.fetch_add(1, std::memory_order_relaxed) + 1;
			if (count <= 16 || !(count & (count - 1)))
			{
				char detail[192];
				std::snprintf(detail, sizeof(detail), "vkAllocateDescriptorSets refused a draw's set after the reclaim ladder (VkResult %d): "
					"the draw is skipped (%llu skipped so far)", static_cast<int>(result), static_cast<unsigned long long>(count));
				ps4_fatal_note("GPU: ", detail);
			}

			if (utils::ps4::rsx_programs::note_set_refusal(s_ps4_set_refusals, ps4_steady_us()))
			{
				char detail[160];
				std::snprintf(detail, sizeof(detail), "draw descriptor sets refused for %llu s in a row (VkResult %d)",
					static_cast<unsigned long long>(utils::ps4::rsx_programs::set_refusal_stop_us / 1'000'000), static_cast<int>(result));
				vk::ps4_request_vram_stop(detail);
			}
		}
	}

	bool ps4_set_descriptor_nullable(bool nullable) noexcept
	{
		return std::exchange(t_ps4_set_nullable, nullable);
	}

	u64 ps4_descriptor_sets_refused() noexcept
	{
		return s_ps4_sets_refused.load(std::memory_order_relaxed);
	}

	void ps4_reclaim_for_driver(VkResult result, const char* what, bool defer_trim) noexcept
	{
		// RADV objects take host memory from the CPU heap and descriptor buffers, shader code and command streams from
		// the GPU window: release CPU heap through the std::new_handler (the memory governor's reserve) for a host
		// failure, trim video memory for a device failure, both for anything else. Allocation-free logging (the CPU heap
		// may be at its cap here): the first 16 notes and then every power of two.
		const bool host = result != VK_ERROR_OUT_OF_DEVICE_MEMORY;
		const bool device = result != VK_ERROR_OUT_OF_HOST_MEMORY;

		if (const u32 count = s_ps4_driver_reclaims.fetch_add(1, std::memory_order_relaxed) + 1; count <= 16 || !(count & (count - 1)))
		{
			char detail[224];
			std::snprintf(detail, sizeof(detail), "%s failed (VkResult %d); %s%s%s, retrying once (%u driver retries so far)",
				what ? what : "driver call", static_cast<int>(result), host ? "CPU heap new_handler called" : "", host && device ? " and " : "",
				device ? (defer_trim ? "video memory trim requested" : "video memory trimmed") : "", count);
			ps4_fatal_note("GPU: ", detail);
		}

		if (host)
		{
			if (const std::new_handler handler = std::get_new_handler())
			{
				handler();
			}
		}

		if (device)
		{
			// On the RSX thread a trim evicts textures and surfaces at once, which must not happen while a draw is
			// being set up: inside an uninterruptible section the trim is handed to the frame end and only completed
			// work is retired now (VKResourceManager.cpp). Other threads always take that path.
			const auto renderer = rsx::get_current_renderer();
			const bool enter = defer_trim && renderer && renderer->is_current_thread() && !vk::is_uninterruptible();
			if (enter)
			{
				vk::enter_uninterruptible();
			}

			vk::vmm_handle_memory_pressure(rsx::problem_severity::severe);

			if (enter)
			{
				vk::leave_uninterruptible();
			}
		}
	}

	u32 ps4_descriptor_sets_inited() noexcept
	{
		return s_ps4_descriptor_sets.load(std::memory_order_relaxed);
	}

	u64 ps4_descriptor_storage_bytes() noexcept
	{
		return s_ps4_descriptor_bytes.load(std::memory_order_relaxed);
	}

	u32 ps4_descriptor_pools_live() noexcept
	{
		return s_ps4_descriptor_pools.load(std::memory_order_relaxed);
	}

	u64 ps4_descriptor_pool_window_bytes() noexcept
	{
		return s_ps4_descriptor_pool_window.load(std::memory_order_relaxed);
	}

	// 0.38 (VKProgramPipeline.cpp sweep): the part of ps4_descriptor_pool_window_bytes() already handed to the GC.
	u64 ps4_descriptor_pool_window_releasing() noexcept
	{
		return s_ps4_descriptor_pool_window_releasing.load(std::memory_order_relaxed);
	}

	u64 ps4_descriptor_spills() noexcept
	{
		return s_ps4_descriptor_spills.load(std::memory_order_relaxed);
	}

	void* descriptor_set::ps4_spill(const void* data, usz bytes)
	{
		auto block = std::make_unique<u64[]>(std::max<usz>((bytes + sizeof(u64) - 1) / sizeof(u64), 1));
		std::memcpy(block.get(), data, bytes);
		void* const result = block.get();
		m_ps4_spill.push_back(std::move(block));

		if (s_ps4_descriptor_spills.fetch_add(1, std::memory_order_relaxed) == 0)
		{
			char detail[256];
			std::snprintf(detail, sizeof(detail), "descriptor scratch array full (%u image, %u buffer, %u view entries reserved); "
				"entries kept in their own block until the next flush (10 s 'Vulkan backend' line: descriptor spills)",
				m_image_info_pool.capacity(), m_buffer_info_pool.capacity(), m_buffer_view_pool.capacity());
			rsx_log.warning("PS4: %s", detail);
			ps4_fatal_note("GPU: ", detail);
		}

		return result;
	}

	void descriptor_pool::ps4_dispose(std::unique_ptr<descriptor_pool>& pool)
	{
		if (!pool)
		{
			return;
		}

		// Sets of this pool may be bound in command buffers up to the one being recorded; the collector runs this
		// once that one has completed (on the driver thread). Pending subpool resets turn into no-ops (m_ps4_alive).
		// 0.38: its window counts as released from now on for the sweep's byte cap (destroy() lowers the total later).
		u64 window = 0;
		for (const auto& subpool : pool->m_device_subpools)
		{
			window += subpool.handle ? subpool.ps4_window_bytes : 0;
		}

		s_ps4_descriptor_pool_window_releasing.fetch_add(window, std::memory_order_relaxed);

		descriptor_pool* const owned = pool.release();
		auto release = std::make_unique<gc_callback_t>([owned, window]()
		{
			owned->destroy();
			delete owned;
			s_ps4_descriptor_pool_window_releasing.fetch_sub(window, std::memory_order_relaxed);
		});
		vk::get_gc()->dispose(release);
	}
#endif

	namespace descriptors
	{
		class dispatch_manager
		{
		public:
			inline void flush_all()
			{
				std::lock_guard lock(m_notifications_lock);

				for (auto& set : m_notification_list)
				{
					set->flush();
				}

				m_notification_list.clear();
			}

			void register_(descriptor_set* set)
			{
				std::lock_guard lock(m_notifications_lock);

				m_notification_list.push_back(set);
				// rsx_log.notice("[descriptor_manager::register] Now monitoring %u descriptor sets", m_notification_list.size());
			}

			void deregister(descriptor_set* set)
			{
				std::lock_guard lock(m_notifications_lock);

				m_notification_list.erase_if(FN(x == set));
				// rsx_log.notice("[descriptor_manager::deregister] Now monitoring %u descriptor sets", m_notification_list.size());
			}

			void destroy()
			{
				std::lock_guard lock(m_notifications_lock);
				m_notification_list.clear();
			}

			dispatch_manager() = default;

		private:
			rsx::simple_array<descriptor_set*> m_notification_list;
			std::mutex m_notifications_lock;

			dispatch_manager(const dispatch_manager&) = delete;
			dispatch_manager& operator = (const dispatch_manager&) = delete;
		};

		void init()
		{
			g_fxo->init<dispatch_manager>();
		}

		void flush()
		{
			g_fxo->get<dispatch_manager>().flush_all();
		}

		void destroy()
		{
			g_fxo->get<dispatch_manager>().destroy();
		}

		VkDescriptorSetLayout create_layout(const rsx::simple_array<VkDescriptorSetLayoutBinding>& bindings)
		{
			VkDescriptorSetLayoutCreateInfo infos = {};
			infos.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
			infos.pBindings = bindings.data();
			infos.bindingCount = ::size32(bindings);

			VkDescriptorSetLayoutBindingFlagsCreateInfo binding_infos = {};
			rsx::simple_array<VkDescriptorBindingFlags> binding_flags;

			const auto deferred_mask = g_render_device->get_descriptor_update_after_bind_support();
			binding_flags.resize(::size32(bindings));

			for (u32 i = 0; i < binding_flags.size(); ++i)
			{
				if ((1ull << bindings[i].descriptorType) & ~deferred_mask)
				{
					binding_flags[i] = 0u;
				}
				else
				{
					binding_flags[i] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
				}
			}

			binding_infos.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO;
			binding_infos.pNext = nullptr;
			binding_infos.bindingCount = ::size32(binding_flags);
			binding_infos.pBindingFlags = binding_flags.data();

			infos.pNext = &binding_infos;
			infos.flags |= VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;

			VkDescriptorSetLayout result;
#ifdef RPCS3_PS4
			// 0.38: CPU heap inside RADV: reclaim and retry once; a draw pipeline link then takes VK_NULL_HANDLE as a
			// refusal (program::create_pipeline_layout), any other caller ends as before.
			VkResult status = vkCreateDescriptorSetLayout(*g_render_device, &infos, nullptr, &result);
			if (status != VK_SUCCESS)
			{
				ps4_reclaim_for_driver(status, "vkCreateDescriptorSetLayout", true);
				status = vkCreateDescriptorSetLayout(*g_render_device, &infos, nullptr, &result);
			}

			if (status != VK_SUCCESS)
			{
				if (!ps4_link_nullable())
				{
					vk::die_with_error(status);
				}

				return VK_NULL_HANDLE;
			}
#else
			CHECK_RESULT(vkCreateDescriptorSetLayout(*g_render_device, &infos, nullptr, &result));
#endif
			return result;
		}
	}

	u32 descriptor_pool::autoscaling_config_t::get_pool_size()
	{
		if (current_size < min_pool_size)
		{
			current_size = min_pool_size;
			return min_pool_size;
		}

		if (current_size >= max_pool_size)
		{
			current_size = max_pool_size;
			return max_pool_size;
		}

		// Try grow
		if ((increment_steps++) < (increment_min_steps - 1u))
		{
			return current_size;
		}

		increment_steps = 0u;
		current_size = std::min(current_size * 2u, max_pool_size);
		return current_size;
	}

	void descriptor_pool::create(const vk::render_device& dev, const rsx::simple_array<VkDescriptorPoolSize>& pool_sizes, u32 min_sets, u32 max_sets)
	{
		m_autoscaling_config.min_pool_size = std::max(min_sets, 16u);
		m_autoscaling_config.max_pool_size = std::max(min_sets, max_sets);

		ensure(m_autoscaling_config.max_pool_size >= 16u);

		m_create_info_pool_sizes = pool_sizes;

		m_create_info.flags = dev.get_descriptor_update_after_bind_support() ? VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT : 0;
		m_create_info.maxSets = max_sets;
		m_create_info.poolSizeCount = m_create_info_pool_sizes.size();
		m_create_info.pPoolSizes = m_create_info_pool_sizes.data();
		m_create_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;

		m_owner = &dev;
		next_subpool();
	}

	void descriptor_pool::destroy()
	{
#ifdef RPCS3_PS4
		m_ps4_alive->store(false);
#endif
		if (m_device_subpools.empty()) return;

		for (auto& pool : m_device_subpools)
		{
#ifdef RPCS3_PS4
			if (pool.handle)
			{
				s_ps4_descriptor_pools.fetch_sub(1, std::memory_order_relaxed);
				s_ps4_descriptor_pool_window.fetch_sub(pool.ps4_window_bytes, std::memory_order_relaxed);
				pool.ps4_window_bytes = 0;
			}
#endif
			vkDestroyDescriptorPool((*m_owner), pool.handle, nullptr);
			pool.handle = VK_NULL_HANDLE;
		}

		m_owner = nullptr;
	}

	void descriptor_pool::reset(u32 subpool_id, VkDescriptorPoolResetFlags flags)
	{
		std::lock_guard lock(m_subpool_lock);

		CHECK_RESULT(vkResetDescriptorPool(*m_owner, m_device_subpools[subpool_id].handle, flags));
		m_device_subpools[subpool_id].busy = VK_FALSE;
	}

	VkDescriptorSet descriptor_pool::allocate(VkDescriptorSetLayout layout, VkBool32 use_cache)
	{
		if (use_cache)
		{
			if (m_descriptor_set_cache.empty())
			{
				// For optimal cache utilization, each pool should only allocate one layout
				m_cached_layout = layout;
			}
			else if (m_cached_layout != layout)
			{
				use_cache = VK_FALSE;
			}
			else
			{
				return m_descriptor_set_cache.pop_back();
			}
		}

		if (!can_allocate(use_cache ? 4 : 1, m_current_subpool_offset))
		{
			next_subpool();
		}

		VkDescriptorSet new_descriptor_set;
		VkDescriptorSetAllocateInfo alloc_info = {};
		alloc_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
		alloc_info.descriptorPool = m_current_pool_handle;
		alloc_info.descriptorSetCount = 1;
		alloc_info.pSetLayouts = &layout;

#ifdef RPCS3_PS4
		// 0.38: a refused allocation (pool memory or fragmentation, or RADV's CPU heap) reclaims memory and retries from a
		// fresh subpool, then reclaims deeper and tries once more. A draw's set (ps4_set_descriptor_nullable) is then
		// returned as VK_NULL_HANDLE and the draw skipped; that full ladder runs at most once per second, other refused
		// draws give up after their first attempt (a fresh subpool per refused draw would only grow the pools). Any
		// other caller needs its set: a third refusal requests the graceful stop of the game before the error.
		const bool ps4_nullable = t_ps4_set_nullable;
		bool ps4_ladder = !ps4_nullable;

		for (int attempt = 0;; attempt++)
		{
			alloc_info.descriptorPool = m_current_pool_handle;
			VkResult result = VK_ERROR_OUT_OF_POOL_MEMORY;

			if (!m_current_pool_handle)
			{
				// No subpool: its creation was refused for a draw's set (next_subpool).
			}
			else if (use_cache)
			{
				const auto alloc_size = std::min<u32>(max_sets() - m_current_subpool_offset, max_cache_size);
				m_allocation_request_cache.resize(alloc_size);
				for (auto& layout_ : m_allocation_request_cache)
				{
					layout_ = m_cached_layout;
				}

				ensure(m_descriptor_set_cache.empty());
				alloc_info.descriptorSetCount = alloc_size;
				alloc_info.pSetLayouts = m_allocation_request_cache.data();

				m_descriptor_set_cache.resize(alloc_size);
				result = vkAllocateDescriptorSets(*m_owner, &alloc_info, m_descriptor_set_cache.data());

				if (result == VK_SUCCESS)
				{
					m_current_subpool_offset += alloc_size;
					new_descriptor_set = m_descriptor_set_cache.pop_back();
					return new_descriptor_set;
				}

				m_descriptor_set_cache.clear();
			}
			else
			{
				alloc_info.descriptorSetCount = 1;
				alloc_info.pSetLayouts = &layout;
				result = vkAllocateDescriptorSets(*m_owner, &alloc_info, &new_descriptor_set);

				if (result == VK_SUCCESS)
				{
					m_current_subpool_offset++;
					return new_descriptor_set;
				}
			}

			if (ps4_nullable && attempt == 0)
			{
				const u64 now = ps4_steady_us();
				ps4_ladder = utils::ps4::rsx_programs::set_ladder_due(s_ps4_set_refusals, now);
				if (ps4_ladder)
				{
					s_ps4_set_refusals.ladder_us = now;
				}
			}

			if (attempt >= (ps4_ladder ? 2 : 0))
			{
				if (ps4_nullable)
				{
					ps4_refuse_set(result);
					return VK_NULL_HANDLE;
				}

				ps4_stop_for(result, "vkAllocateDescriptorSets");
				vk::die_with_error(result);
				fmt::throw_exception("Unreachable");
			}

			if (attempt)
			{
				ps4_last_reclaim(result, "vkAllocateDescriptorSets");
			}
			else
			{
				ps4_reclaim_for_driver(result, "vkAllocateDescriptorSets", true);
			}

			next_subpool();
		}
#else
		if (use_cache)
		{
			const auto alloc_size = std::min<u32>(max_sets() - m_current_subpool_offset, max_cache_size);
			m_allocation_request_cache.resize(alloc_size);
			for (auto& layout_ : m_allocation_request_cache)
			{
				layout_ = m_cached_layout;
			}

			ensure(m_descriptor_set_cache.empty());
			alloc_info.descriptorSetCount = alloc_size;
			alloc_info.pSetLayouts = m_allocation_request_cache.data();

			m_descriptor_set_cache.resize(alloc_size);
			CHECK_RESULT(vkAllocateDescriptorSets(*m_owner, &alloc_info, m_descriptor_set_cache.data()));

			m_current_subpool_offset += alloc_size;
			new_descriptor_set = m_descriptor_set_cache.pop_back();
		}
		else
		{
			m_current_subpool_offset++;
			CHECK_RESULT(vkAllocateDescriptorSets(*m_owner, &alloc_info, &new_descriptor_set));
		}

		return new_descriptor_set;
#endif
	}

	void descriptor_pool::next_subpool()
	{
		if (m_current_subpool_index != umax)
		{
			// Enqueue release using gc
#ifdef RPCS3_PS4
			// A released pool (ps4_dispose) may be destroyed before this reset runs.
			auto release_func = [subpool_index=m_current_subpool_index, this, alive = m_ps4_alive]()
			{
				if (alive->load())
				{
					this->reset(subpool_index, 0);
				}
			};
#else
			auto release_func = [subpool_index=m_current_subpool_index, this]()
			{
				this->reset(subpool_index, 0);
			};
#endif

			auto cleanup_obj = std::make_unique<gc_callback_t>(release_func);
			vk::get_gc()->dispose(cleanup_obj);
		}

		m_current_subpool_offset = 0;
		m_current_subpool_index = umax;

		const int max_retries = 2;
		int retries = max_retries;
#ifdef RPCS3_PS4
		int ps4_memory_retries = 0;
#endif

		do
		{
			// Happy path - maybe we have a free subpool
			{
				reader_lock lock(m_subpool_lock);

				for (u32 index = 0; index < m_device_subpools.size(); ++index)
				{
					if (!m_device_subpools[index].busy)
					{
						m_current_subpool_index = index;
						m_device_subpools[m_current_subpool_index].busy = VK_TRUE;
						m_current_pool_handle = m_device_subpools[m_current_subpool_index].handle;
						break;
					}
				}

				if (m_current_subpool_index != umax)
				{
					return;
				}
			}

			const auto [result, subpool] = new_subpool();
			if (result != VK_SUCCESS)
			{
#ifdef RPCS3_PS4
				// 0.38: for a draw's set, never the fatal recovery (a hard sync in the middle of the draw's descriptor
				// commits); the draw's allocation reclaims and retries or skips the draw (allocate).
				if (retries-- && (result == VK_ERROR_FRAGMENTATION_EXT) && (!t_ps4_set_nullable || retries))
#else
				if (retries-- && (result == VK_ERROR_FRAGMENTATION_EXT))
#endif
				{
					rsx_log.warning("Descriptor pool creation failed with fragmentation error. Will attempt to recover.");
					vk::on_descriptor_pool_fragmentation(!retries);
					continue;
				}

#ifdef RPCS3_PS4
				if (t_ps4_set_nullable)
				{
					m_current_pool_handle = VK_NULL_HANDLE;
					return;
				}

				// 0.38: a second memory refusal gets the deeper reclaim and one more attempt; a third requests the
				// graceful stop of the game before the error.
				if (result != VK_ERROR_FRAGMENTATION_EXT && ps4_memory_retries < 2)
				{
					if (ps4_memory_retries++)
					{
						ps4_last_reclaim(result, "descriptor pool creation");
					}
					else
					{
						ps4_release_memory_for_pool(result);
					}

					continue;
				}

				ps4_stop_for(result, "descriptor pool creation");
#endif

				vk::die_with_error(result);
				fmt::throw_exception("Unreachable");
			}

			// New subpool created successfully
			std::lock_guard lock(m_subpool_lock);

#ifdef RPCS3_PS4
			const u64 ps4_window = ps4_estimate_pool_window(m_create_info_pool_sizes, m_autoscaling_config.current_size);
			s_ps4_descriptor_pool_window.fetch_add(ps4_window, std::memory_order_relaxed);

			m_device_subpools.push_back(
			{
				.handle = subpool,
				.size = m_autoscaling_config.current_size,
				.busy = VK_TRUE,
				.ps4_window_bytes = ps4_window
			});
#else
			m_device_subpools.push_back(
			{
				.handle = subpool,
				.size = m_autoscaling_config.current_size,
				.busy = VK_TRUE
			});
#endif

			m_current_subpool_index = m_device_subpools.size() - 1;
			m_current_pool_handle = m_device_subpools[m_current_subpool_index].handle;

		} while (m_current_subpool_index == umax);
	}

	std::pair<VkResult, VkDescriptorPool> descriptor_pool::new_subpool()
	{
		// Try autoscaling
		const auto prev_scaling_config = m_autoscaling_config;
		const u32 set_count = m_autoscaling_config.get_pool_size();

		// Configure request using current pool size
		auto descriptor_pool_sizes = m_create_info_pool_sizes.map([set_count](const VkDescriptorPoolSize& pool_size_info)
		{
			auto ret = pool_size_info;
			ret.descriptorCount *= set_count;
			return ret;
		});

		m_create_info.maxSets = set_count;
		m_create_info.poolSizeCount = descriptor_pool_sizes.size();
		m_create_info.pPoolSizes = descriptor_pool_sizes.data();

		VkDescriptorPool subpool = VK_NULL_HANDLE;
		VkResult result = vkCreateDescriptorPool(*m_owner, &m_create_info, nullptr, &subpool);

		if (result != VK_SUCCESS)
		{
			// Roll back autoscaling
			m_autoscaling_config = prev_scaling_config;
		}
#ifdef RPCS3_PS4
		else
		{
			s_ps4_descriptor_pools.fetch_add(1, std::memory_order_relaxed);
		}
#endif

		// Cleanup
		m_create_info.pPoolSizes = nullptr;
		m_create_info.poolSizeCount = 0;

		return { result, subpool };
	}

	descriptor_set::descriptor_set(VkDescriptorSet set)
	{
		flush();
		m_handle = set;
	}

#ifdef RPCS3_PS4
	void descriptor_set::ps4_release()
	{
		// The queued writes target the current handle and point into the scratch arrays.
		flush();

		std::lock_guard lock(m_storage_lock);
		m_handle = VK_NULL_HANDLE;

		if (!m_in_use)
		{
			return;
		}

		s_ps4_descriptor_sets.fetch_sub(1, std::memory_order_relaxed);
		s_ps4_descriptor_bytes.fetch_sub(m_ps4_storage_bytes, std::memory_order_relaxed);
		m_ps4_storage_bytes = 0;
		m_in_use = false;

		ps4_free_array(m_buffer_view_pool);
		ps4_free_array(m_buffer_info_pool);
		ps4_free_array(m_image_info_pool);
		ps4_free_array(m_pending_writes);
		ps4_free_array(m_pending_copies);
	}
#endif

	descriptor_set::~descriptor_set()
	{
		if (m_update_after_bind_mask)
		{
			g_fxo->get<descriptors::dispatch_manager>().deregister(this);
		}

#ifdef RPCS3_PS4
		if (m_in_use)
		{
			s_ps4_descriptor_sets.fetch_sub(1, std::memory_order_relaxed);
			s_ps4_descriptor_bytes.fetch_sub(m_ps4_storage_bytes, std::memory_order_relaxed);
		}
#endif
	}

	void descriptor_set::init(VkDescriptorSet new_set)
	{
		if (!m_in_use) [[unlikely]]
		{
			m_image_info_pool.reserve(m_pool_size);
			m_buffer_view_pool.reserve(m_pool_size);
			m_buffer_info_pool.reserve(m_pool_size);

#ifdef RPCS3_PS4
			// The queued writes grow by one commit's template at a time and are flushed at max_cache_size entries:
			// reserving the whole window once avoids an exact-size reallocation on every commit.
			m_pending_writes.reserve(m_pool_size);

			m_ps4_storage_bytes =
				u64{ m_image_info_pool.capacity() } * sizeof(VkDescriptorImageInfo) +
				u64{ m_buffer_view_pool.capacity() } * sizeof(VkBufferView) +
				u64{ m_buffer_info_pool.capacity() } * sizeof(VkDescriptorBufferInfo) +
				u64{ m_pending_writes.capacity() } * sizeof(WriteDescriptorSetT);
			s_ps4_descriptor_sets.fetch_add(1, std::memory_order_relaxed);
			s_ps4_descriptor_bytes.fetch_add(m_ps4_storage_bytes, std::memory_order_relaxed);
#endif

			m_in_use = true;
			m_update_after_bind_mask = g_render_device->get_descriptor_update_after_bind_support();
		}
		else if (m_push_type_mask & ~m_update_after_bind_mask)
		{
			flush();
		}

		m_handle = new_set;
	}

	void descriptor_set::swap(descriptor_set& other)
	{
		const auto other_handle = other.m_handle;
		other.flush();
		other.m_handle = m_handle;
		init(other_handle);
	}

	descriptor_set& descriptor_set::operator = (VkDescriptorSet set)
	{
		init(set);
		return *this;
	}

	VkDescriptorSet* descriptor_set::ptr()
	{
		if (!m_in_use) [[likely]]
		{
			init(m_handle);
		}

		return &m_handle;
	}

	void descriptor_set::push(const VkBufferView& buffer_view, VkDescriptorType type, u32 binding)
	{
		m_push_type_mask |= (1ull << type);
#ifdef RPCS3_PS4
		// 0.37: store() never moves entries that queued writes already point to.
		const VkBufferView* stored = store(buffer_view);
#else
		m_buffer_view_pool.push_back(buffer_view);
		const VkBufferView* stored = &m_buffer_view_pool.back();
#endif
		m_pending_writes.emplace_back(
			VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,    // sType
			nullptr,                                   // pNext
			m_handle,                                  // dstSet
			binding,                                   // dstBinding
			0,                                         // dstArrayElement
			1,                                         // descriptorCount
			type,                                      // descriptorType
			nullptr,                                   // pImageInfo
			nullptr,                                   // pBufferInfo
			stored                                     // pTexelBufferView
		);
	}

	void descriptor_set::push(const VkDescriptorBufferInfo& buffer_info, VkDescriptorType type, u32 binding)
	{
		m_push_type_mask |= (1ull << type);
#ifdef RPCS3_PS4
		// 0.37: store() never moves entries that queued writes already point to.
		const VkDescriptorBufferInfo* stored = store(buffer_info);
#else
		m_buffer_info_pool.push_back(buffer_info);
		const VkDescriptorBufferInfo* stored = &m_buffer_info_pool.back();
#endif
		m_pending_writes.emplace_back(
			VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,    // sType
			nullptr,                                   // pNext
			m_handle,                                  // dstSet
			binding,                                   // dstBinding
			0,                                         // dstArrayElement
			1,                                         // descriptorCount
			type,                                      // descriptorType
			nullptr,                                   // pImageInfo
			stored,                                    // pBufferInfo
			nullptr                                    // pTexelBufferView
		);
	}

	void descriptor_set::push(const VkDescriptorImageInfo& image_info, VkDescriptorType type, u32 binding)
	{
		m_push_type_mask |= (1ull << type);
#ifdef RPCS3_PS4
		// 0.37: store() never moves entries that queued writes already point to.
		const VkDescriptorImageInfo* stored = store(image_info);
#else
		m_image_info_pool.push_back(image_info);
		const VkDescriptorImageInfo* stored = &m_image_info_pool.back();
#endif
		m_pending_writes.emplace_back(
			VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,    // sType
			nullptr,                                   // pNext
			m_handle,                                  // dstSet
			binding,                                   // dstBinding
			0,                                         // dstArrayElement
			1,                                         // descriptorCount
			type,                                      // descriptorType
			stored,                                    // pImageInfo
			nullptr,                                   // pBufferInfo
			nullptr                                    // pTexelBufferView
		);
	}

	void descriptor_set::push(const VkDescriptorImageInfo* image_info, u32 count, VkDescriptorType type, u32 binding)
	{
		VkWriteDescriptorSet writer =
		{
			VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,    // sType
			nullptr,                                   // pNext
			m_handle,                                  // dstSet
			binding,                                   // dstBinding
			0,                                         // dstArrayElement
			count,                                     // descriptorCount
			type,                                      // descriptorType
			image_info,                                // pImageInfo
			nullptr,                                   // pBufferInfo
			nullptr                                    // pTexelBufferView
		};
		vkUpdateDescriptorSets(*g_render_device, 1, &writer, 0, nullptr);
	}

	void descriptor_set::push(const rsx::simple_array<VkCopyDescriptorSet>& copy_cmd, u32 type_mask)
	{
		m_push_type_mask |= type_mask;
		m_pending_copies += copy_cmd;
	}

	void descriptor_set::push(const rsx::simple_array<VkWriteDescriptorSet>& write_cmds, u32 type_mask)
	{
		m_push_type_mask |= type_mask;
		m_pending_writes += write_cmds;
	}

	void descriptor_set::push(const descriptor_set_dynamic_offset_t& offset)
	{
		ensure(offset.location >= 0 && offset.location <= 16);
		while (m_dynamic_offsets.size() < (static_cast<u32>(offset.location) + 1u))
		{
			m_dynamic_offsets.push_back(0);
		}

		m_dynamic_offsets[offset.location] = offset.value;
	}

	void descriptor_set::on_bind()
	{
		if (!m_push_type_mask)
		{
			ensure(m_pending_writes.empty());
			return;
		}

		// We have queued writes
		if ((m_push_type_mask & ~m_update_after_bind_mask) ||
			(m_pending_writes.size() >= max_cache_size) ||
			storage_cache_pressure())
		{
			flush();
			return;
		}

		// Register for async flush
		ensure(m_update_after_bind_mask);
		g_fxo->get<descriptors::dispatch_manager>().register_(this);
	}

	void descriptor_set::bind(const vk::command_buffer& cmd, VkPipelineBindPoint bind_point, VkPipelineLayout layout)
	{
		// Notify
		on_bind();

		VkDescriptorSet sets[1] = { m_handle };
		cmd.bind_descriptor_sets(sets, m_dynamic_offsets, bind_point, layout);
	}

	void descriptor_set::flush()
	{
		if (!m_push_type_mask)
		{
			return;
		}

		std::lock_guard lock(m_storage_lock);

		const auto num_writes = ::size32(m_pending_writes);
		const auto num_copies = ::size32(m_pending_copies);
		vkUpdateDescriptorSets(*g_render_device, num_writes, m_pending_writes.data(), num_copies, m_pending_copies.data());

		m_storage_cache_id++;

		m_push_type_mask = 0;
		m_pending_writes.clear();
		m_pending_copies.clear();
		m_image_info_pool.clear();
		m_buffer_info_pool.clear();
		m_buffer_view_pool.clear();
#ifdef RPCS3_PS4
		// Same lifetime as the arrays: the cache id moved on, so no template reuses a pointer into these.
		m_ps4_spill.clear();
#endif
	}
}
