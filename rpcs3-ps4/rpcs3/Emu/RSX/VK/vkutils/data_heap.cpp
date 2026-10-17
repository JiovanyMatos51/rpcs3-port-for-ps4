#include "barriers.h"
#include "data_heap.h"
#include "device.h"

#include "../../RSXOffload.h"
#include "../VKHelpers.h"
#include "../VKResourceManager.h"
#include "Emu/IdManager.h"
#include "Emu/system_config.h"

#include <memory>

#ifdef RPCS3_PS4
#include "ps4_launcher.h"

#include <algorithm>
#include <atomic>
#endif

namespace vk
{
	data_heap g_upload_heap;

#ifdef RPCS3_PS4
	namespace
	{
		std::atomic<u32> s_ps4_ring_growths{ 0 };
		std::atomic<u32> s_ps4_ring_recoveries{ 0 };
		std::atomic<u32> s_ps4_ring_fallbacks{ 0 };

		// ps4_resize_ring: the heap whose next grow() builds exactly s_ps4_resize_size bytes, accepts a refusal and never
		// makes room or grows the upstream way. RSX thread (the rings are only used there).
		std::atomic<const data_heap*> s_ps4_resize_heap{ nullptr };
		usz s_ps4_resize_size = 0;
		bool s_ps4_resize_done = false;

		// RPCS3.log gets every event's line; startup.log (fsync'd) the 1st, 2nd, 4th, 8th... of each kind.
		void ps4_note_ring_event(u32 count, const std::string& line)
		{
			rsx_log.warning("%s", line);
			if (!(count & (count - 1)))
			{
				ps4_startup_checkpoint(line.c_str());
			}
		}
	}
#endif

	void data_heap::create(VkBufferUsageFlags usage, usz size, rsx::flags32_t flags, const char* name, usz guard, VkBool32 notify)
	{
		rsx::data_heap::init(size, name, guard);

		const auto& memory_map = g_render_device->get_memory_mapping();

		ensure(std::popcount(flags & (heap_pool_force_vram_shadow | heap_pool_low_latency)) <= 1,
			"Invalid data heap flag combination detected");

		if ((flags & heap_pool_low_latency) && g_cfg.video.vk.use_rebar_upload_heap)
		{
			// Prefer uploading to BAR if low latency is desired.
			const int max_usage = memory_map.device_bar_total_bytes <= (256 * 0x100000) ? 75 : 90;
			m_prefer_writethrough = can_allocate_heap(memory_map.device_bar, size, max_usage);

			// Log it
			if (m_prefer_writethrough)
			{
				rsx_log.notice("Data heap %s will attempt to use Re-BAR memory", m_name);
			}
			else
			{
				rsx_log.warning("Could not fit heap '%s' into Re-BAR memory", m_name);
			}
		}

		VkFlags memory_flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
		auto memory_index = m_prefer_writethrough ? memory_map.device_bar : memory_map.host_visible_coherent;

		const bool vram_shadow_requested = !!(flags & heap_pool_force_vram_shadow);
		const bool use_shadow = vram_shadow_requested || !(get_heap_compatible_buffer_types() & usage);

		if (use_shadow)
		{
			if (!vram_shadow_requested)
			{
				rsx_log.warning("Buffer usage %u is not heap-compatible using this driver, explicit staging buffer in use", usage);
			}

			shadow = std::make_unique<buffer>(*g_render_device, size, memory_index, memory_flags, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 0, VMM_ALLOCATION_POOL_SYSTEM);
			usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
			memory_flags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
			memory_index = memory_map.device_local;
			m_prefer_writethrough = false;
		}

		VkFlags create_flags = 0;
		if (m_prefer_writethrough)
		{
			create_flags |= (VK_BUFFER_CREATE_ALLOW_NULL_RPCS3 | VK_BUFFER_CREATE_IGNORE_VMEM_PRESSURE_RPCS3);
		}

		heap = std::make_unique<buffer>(*g_render_device, size, memory_index, memory_flags, usage, create_flags, VMM_ALLOCATION_POOL_SYSTEM);

		if (!heap->value)
		{
			rsx_log.warning("Could not place heap '%s' into Re-BAR memory. Will attempt to use regular host-visible memory.", m_name);
			ensure(m_prefer_writethrough);

			// We failed to place the buffer in rebar memory. Try again in host-visible.
			m_prefer_writethrough = false;
			auto gc = get_resource_manager();
			gc->dispose(heap);
			heap = std::make_unique<buffer>(*g_render_device, size, memory_map.host_visible_coherent, memory_flags, usage, 0, VMM_ALLOCATION_POOL_SYSTEM);
		}

		m_flags = flags;
		initial_size = size;
		notify_on_grow = bool(notify);
	}

	void data_heap::destroy()
	{
		if (mapped)
		{
			unmap(true);
		}

		heap.reset();
		shadow.reset();
	}

	bool data_heap::grow(usz size)
	{
#ifdef RPCS3_PS4
		// 0.35 died here: the attrib ring's 64 -> 128 MiB growth needed one free 128 MiB range of the fragmented GPU
		// window while the old ring was still mapped. With ring reclaim on, the renderer keeps room before every draw,
		// so only an allocation larger than that room gets here. Growth comes first, in 16 MiB steps up to twice the
		// initial size, with the new buffer built before the old one is released and a refusal accepted; then the
		// renderer makes room with a hard sync; only when both fail does the upstream growth below run.
		// 0.39: a resize asked by ps4_resize_ring (adaptive ring growth after a hard sync) builds exactly the requested
		// size and returns true either way; the caller drops its dummy block and checks the size.
		const bool ps4_resize = s_ps4_resize_heap.load(std::memory_order_relaxed) == this;

		if (rsx::data_heap::s_ps4_make_room || ps4_resize)
		{
			// alloc() places the block at the position it computed before calling grow(), so if room is made without
			// growing, put must still be where it was here.
			const usz grow_pos = m_ps4_grow_pos;
			const usz put_at_entry = m_put_pos;
			const usz cap = ps4_resize ? s_ps4_resize_size : (m_flags & heap_pool_fixed_size) ? initial_size : 2 * initial_size;
			const usz new_size = ps4_resize ? s_ps4_resize_size : utils::align(m_size + size, 16 * 0x100000);
			const char* refusal = "over the 2x cap";

			if (new_size <= cap)
			{
				const auto& memory_map = g_render_device->get_memory_mapping();
				auto gc = get_resource_manager();
				VkBufferUsageFlags usage = heap->info.usage;
				bool writethrough = m_prefer_writethrough;

				if (writethrough)
				{
					const int max_usage = memory_map.device_bar_total_bytes <= (256 * 0x100000) ? 75 : 90;
					writethrough = can_allocate_heap(memory_map.device_bar, new_size, max_usage);
				}

				VkFlags memory_flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
				auto memory_index = writethrough ? memory_map.device_bar : memory_map.host_visible_coherent;

				std::unique_ptr<buffer> new_shadow;
				if (shadow)
				{
					new_shadow = std::make_unique<buffer>(*g_render_device, new_size, memory_index, memory_flags, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
						VK_BUFFER_CREATE_ALLOW_NULL_RPCS3, VMM_ALLOCATION_POOL_SYSTEM);
					usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
					memory_flags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
					memory_index = memory_map.device_local;
					writethrough = false;
				}

				std::unique_ptr<buffer> new_heap;
				if (!new_shadow || new_shadow->value)
				{
					VkFlags create_flags = VK_BUFFER_CREATE_ALLOW_NULL_RPCS3;
					if (writethrough)
					{
						create_flags |= VK_BUFFER_CREATE_IGNORE_VMEM_PRESSURE_RPCS3;
					}

					new_heap = std::make_unique<buffer>(*g_render_device, new_size, memory_index, memory_flags, usage, create_flags, VMM_ALLOCATION_POOL_SYSTEM);

					if (!new_heap->value && writethrough)
					{
						// As in create(): Re-BAR is optional, regular host-visible memory comes next.
						writethrough = false;
						gc->dispose(new_heap);
						new_heap = std::make_unique<buffer>(*g_render_device, new_size, memory_map.host_visible_coherent, memory_flags, usage,
							VK_BUFFER_CREATE_ALLOW_NULL_RPCS3, VMM_ALLOCATION_POOL_SYSTEM);
					}
				}

				if (new_heap && new_heap->value)
				{
					const usz old_size = m_size;

					// The upstream sequence below, except that the old buffers go to the garbage collector only now
					// that their replacements exist.
					g_fxo->get<rsx::dma_manager>().sync();

					if (mapped)
					{
						unmap(true);
					}

					if (shadow)
					{
						gc->dispose(shadow);
					}

					gc->dispose(heap);
					heap = std::move(new_heap);
					shadow = std::move(new_shadow);
					m_prefer_writethrough = writethrough;

					rsx::data_heap::init(new_size, m_name, m_min_guard_size);

					if (notify_on_grow)
					{
						raise_status_interrupt(vk::heap_changed);
					}

					if (ps4_resize)
					{
						// The renderer logs the growth with its reason.
						s_ps4_resize_done = true;
						return true;
					}

					const u32 count = ++s_ps4_ring_growths;
					ps4_note_ring_event(count, fmt::format("PS4 RSX: ring '%s' grew %u -> %u MiB for an allocation of %u KiB (growth %u of the process)",
						m_name, static_cast<u32>(old_size >> 20), static_cast<u32>(new_size >> 20), static_cast<u32>(size >> 10), count));
					return true;
				}

				// The window had no room for the larger ring (the allocator's pressure pass already ran).
				if (new_heap)
				{
					gc->dispose(new_heap);
				}

				if (new_shadow)
				{
					gc->dispose(new_shadow);
				}

				refusal = "window";
			}

			if (ps4_resize)
			{
				// Refused: the ring stays as it is (the caller's dummy block is dropped right after).
				return true;
			}

			// Room without growth: the renderer submits and waits for everything recorded and retires every queued frame.
			if (rsx::data_heap::s_ps4_make_room(*this, size) && m_put_pos == put_at_entry && can_alloc_impl(grow_pos, size))
			{
				const u32 count = ++s_ps4_ring_recoveries;
				ps4_note_ring_event(count, fmt::format("PS4 RSX: ring '%s' growth refused (%u MiB, %s); recovered by hard sync (count %u)",
					m_name, static_cast<u32>(new_size >> 20), refusal, count));
				return true;
			}

			const u32 count = ++s_ps4_ring_fallbacks;
			ps4_note_ring_event(count, fmt::format("PS4 RSX: ring '%s' (%u MiB) still has no room for %u KiB after the growth refusal (%s) and "
				"the hard sync (or none was allowed here); it grows the upstream way (fallback %u)",
				m_name, static_cast<u32>(m_size >> 20), static_cast<u32>(size >> 10), refusal, count));
		}
#endif

		// Create new heap. All sizes are aligned up by 64M, upto 1GiB
		const usz size_limit = (m_flags & heap_pool_fixed_size) ? initial_size : 1024 * 0x100000;
#ifdef RPCS3_PS4
		// 0.38: the smallest growth that holds the request, in 16 MiB steps. 64 MiB steps asked the fragmented GPU
		// window for one contiguous range of up to twice the ring (0.35: 128 MiB, mesa 'out of device VA').
		usz aligned_new_size = utils::align(m_size + size, 16 * 0x100000);
#else
		usz aligned_new_size = utils::align(m_size + size, 64 * 0x100000);
#endif

		if (aligned_new_size >= size_limit)
		{
			// Too large, try to swap out the heap instead of growing.
			rsx_log.error("[%s] Pool limit was reached. Will attempt to swap out the current heap.", m_name);
			aligned_new_size = size_limit;
		}

		// Wait for DMA activity to end
		g_fxo->get<rsx::dma_manager>().sync();

		if (mapped)
		{
			// Force reset mapping
			unmap(true);
		}

		VkBufferUsageFlags usage = heap->info.usage;
		const auto& memory_map = g_render_device->get_memory_mapping();

		if (m_prefer_writethrough)
		{
			const int max_usage = memory_map.device_bar_total_bytes <= (256 * 0x100000) ? 75 : 90;
			m_prefer_writethrough = can_allocate_heap(memory_map.device_bar, aligned_new_size, max_usage);

			if (!m_prefer_writethrough)
			{
				rsx_log.warning("Could not fit heap '%s' into Re-BAR memory during reallocation", m_name);
			}
		}

		VkFlags memory_flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
		auto memory_index = m_prefer_writethrough ? memory_map.device_bar : memory_map.host_visible_coherent;

		// Update heap information and reset the allocator
		rsx::data_heap::init(aligned_new_size, m_name, m_min_guard_size);

		// Discard old heap and create a new one. Old heap will be garbage collected when no longer needed
		auto gc = get_resource_manager();
		if (shadow)
		{
			ensure(!m_prefer_writethrough);
			rsx_log.warning("Buffer usage %u is not heap-compatible using this driver, explicit staging buffer in use", usage);

			gc->dispose(shadow);
			shadow = std::make_unique<buffer>(*g_render_device, aligned_new_size, memory_index, memory_flags, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 0, VMM_ALLOCATION_POOL_SYSTEM);
			usage |= VK_BUFFER_USAGE_TRANSFER_DST_BIT;
			memory_flags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
			memory_index = memory_map.device_local;
		}

		gc->dispose(heap);

		VkFlags create_flags = 0;
		if (m_prefer_writethrough)
		{
			create_flags |= (VK_BUFFER_CREATE_ALLOW_NULL_RPCS3 | VK_BUFFER_CREATE_IGNORE_VMEM_PRESSURE_RPCS3);
		}

		heap = std::make_unique<buffer>(*g_render_device, aligned_new_size, memory_index, memory_flags, usage, create_flags, VMM_ALLOCATION_POOL_SYSTEM);

		if (!heap->value)
		{
			rsx_log.warning("Could not place heap '%s' into Re-BAR memory. Will attempt to use regular host-visible memory.", m_name);
			ensure(m_prefer_writethrough);

			// We failed to place the buffer in rebar memory. Try again in host-visible.
			m_prefer_writethrough = false;
			gc->dispose(heap);
			heap = std::make_unique<buffer>(*g_render_device, aligned_new_size, memory_map.host_visible_coherent, memory_flags, usage, 0, VMM_ALLOCATION_POOL_SYSTEM);
		}

		if (notify_on_grow)
		{
			raise_status_interrupt(vk::heap_changed);
		}

		return true;
	}

	bool data_heap::can_allocate_heap(const vk::memory_type_info& target_heap, usz size, int max_usage_percent)
	{
		const auto current_usage = vmm_get_application_memory_usage(target_heap);
		const auto after_usage = current_usage + size;
		const auto limit = (target_heap.total_bytes() * max_usage_percent) / 100;
		return after_usage < limit;
	}

	void* data_heap::map_impl(usz offset, usz size)
	{
		if (!_ptr)
		{
			if (shadow)
				_ptr = shadow->map(0, shadow->size());
			else
				_ptr = heap->map(0, heap->size());

			mapped = true;
		}

		if (shadow)
		{
			bool insert = true;
			if (!dirty_ranges.empty() && (dirty_ranges.back().dstOffset + dirty_ranges.back().size) == offset) [[ likely ]]
			{
				dirty_ranges.back().size += size;
				insert = false;
			}

			if (insert)
			{
				dirty_ranges.push_back({ offset, offset, size });
			}

			raise_status_interrupt(runtime_state::heap_dirty);
		}

		return static_cast<u8*>(_ptr) + offset;
	}

	void data_heap::unmap(bool force)
	{
		if (force)
		{
			if (shadow)
				shadow->unmap();
			else
				heap->unmap();

			mapped = false;
			_ptr = nullptr;
		}
	}

	void data_heap::sync(const vk::command_buffer& cmd)
	{
		if (dirty_ranges.empty())
		{
			return;
		}

		ensure(shadow);
		ensure(heap);
		vkCmdCopyBuffer(cmd, shadow->value, heap->value, ::size32(dirty_ranges), dirty_ranges.data());
		dirty_ranges.clear();

		insert_buffer_memory_barrier(cmd, heap->value, 0, heap->size(),
			VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
			VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
	}

	bool data_heap::is_dirty() const
	{
		return !dirty_ranges.empty();
	}

#ifdef RPCS3_PS4
	namespace
	{
		std::atomic<bool> s_ps4_full_rings{ false };
		std::atomic<usz> s_ps4_aux_upload_record{ 0 };
		std::atomic<u32> s_ps4_staging_uploads{ 0 };
		std::atomic<u32> s_ps4_aux_staging_uploads{ 0 };
	}

	void ps4_set_full_rings(bool full)
	{
		s_ps4_full_rings.store(full, std::memory_order_relaxed);
	}

	void ps4_set_aux_upload_heap_record(usz size)
	{
		s_ps4_aux_upload_record.store(size, std::memory_order_relaxed);
	}

	u32 ps4_note_staging_upload(const data_heap& heap) noexcept
	{
		if (&heap == &g_upload_heap)
		{
			s_ps4_aux_staging_uploads.fetch_add(1, std::memory_order_relaxed);
		}

		return s_ps4_staging_uploads.fetch_add(1, std::memory_order_relaxed) + 1;
	}

	u32 ps4_aux_staging_uploads() noexcept
	{
		return s_ps4_aux_staging_uploads.load(std::memory_order_relaxed);
	}

	bool ps4_resize_ring(data_heap& ring, usz new_size)
	{
		const usz before = ring.size();
		if (!ring.heap || new_size <= before)
		{
			return false;
		}

		s_ps4_resize_size = new_size;
		s_ps4_resize_done = false;
		s_ps4_resize_heap.store(&ring, std::memory_order_relaxed);

		// A block the size of the whole ring never fits (can_alloc_impl), so alloc() calls grow(), which serves the
		// request above and returns true whether or not the larger buffer was built. The block is a dummy: the ring is
		// emptied right after (the caller hard-synced, nothing of it is in use).
		static_cast<void>(ring.alloc<256>(before));

		s_ps4_resize_heap.store(nullptr, std::memory_order_relaxed);
		ring.reset_allocation_stats();
		return s_ps4_resize_done && ring.size() == new_size;
	}

	usz ps4_aux_upload_heap_size()
	{
		// It is reset after every frame (reset_global_resources); an upload larger than its free part gets its own
		// staging buffer (vk::upload_image) instead of growing it. 0.39: the renderer grows it after a hard sync when
		// those come often, and a title that grew it starts at that size (ring record).
		constexpr usz full_size = 64 * 0x100000;
		const usz base = s_ps4_full_rings.load(std::memory_order_relaxed) ? full_size : 16 * 0x100000;
		return std::clamp<usz>(s_ps4_aux_upload_record.load(std::memory_order_relaxed), base, full_size);
	}
#endif

	data_heap* get_upload_heap()
	{
		if (!g_upload_heap.heap)
		{
#ifdef RPCS3_PS4
			g_upload_heap.create(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, ps4_aux_upload_heap_size(), vk::heap_pool_default, "auxilliary upload heap", 0x100000);
#else
			g_upload_heap.create(VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 64 * 0x100000, vk::heap_pool_default, "auxilliary upload heap", 0x100000);
#endif
		}

		return &g_upload_heap;
	}
}
