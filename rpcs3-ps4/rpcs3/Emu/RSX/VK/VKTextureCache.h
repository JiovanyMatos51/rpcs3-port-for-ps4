#pragma once

#include "VKDMA.h"
#include "VKRenderTargets.h"
#include "VKResourceManager.h"
#include "VKRenderPass.h"
#include "vkutils/image_helpers.h"

#include "../Common/texture_cache.h"
#include "../Common/tiled_dma_copy.hpp"
#include "../Utils/image_utils.hpp"

#include <memory>
#include <vector>

namespace vk
{
	class cached_texture_section;
	class texture_cache;

#ifdef RPCS3_PS4
	// 0.28 (VKTextureCache.cpp): true on the thread running VKGSRender::on_access_violation's flush once the
	// graphics queue was drained through a fence. DMA vk::events are never seen as set by the host on the
	// console's RADV, but after that drain every DMA they guard has completed.
	extern thread_local bool g_ps4_flush_queue_drained;
	void ps4_note_invisible_dma_event();

	// 0.37 (VKTextureCache.cpp): readbacks this thread recorded and its newest completed DMA submit wait. Reset and
	// read by VKGSRender::on_access_violation to time a flush.
	struct ps4_readback_tls_t
	{
		u32 first = 0;          // number of the first readback recorded since the reset (0: none)
		u32 last = 0;           // ... and of the newest
		u32 count = 0;
		u64 submit_done_us = 0; // get_system_time() after the newest DMA submit wait (cleanup_after_dma_transfers)
	};

	extern thread_local ps4_readback_tls_t g_ps4_readback_tls;
#endif

	struct texture_cache_traits
	{
		using commandbuffer_type      = vk::command_buffer;
		using section_storage_type    = vk::cached_texture_section;
		using texture_cache_type      = vk::texture_cache;
		using texture_cache_base_type = rsx::texture_cache<texture_cache_type, texture_cache_traits>;
		using image_resource_type     = vk::image*;
		using image_view_type         = vk::image_view*;
		using image_storage_type      = vk::image;
		using texture_format          = VkFormat;
		using viewable_image_type     = vk::viewable_image*;
	};

	class cached_texture_section : public rsx::cached_texture_section<vk::cached_texture_section, vk::texture_cache_traits>
	{
		using baseclass = typename rsx::cached_texture_section<vk::cached_texture_section, vk::texture_cache_traits>;
		friend baseclass;

		std::unique_ptr<vk::viewable_image> managed_texture = nullptr;

		// DMA relevant data
		std::unique_ptr<vk::event> dma_fence;
#ifdef RPCS3_PS4
		// 0.28: the command buffer (and its recording generation) that holds this section's DMA copy.
		// Command buffers live in the renderer's rings for the renderer's lifetime.
		vk::command_buffer* m_ps4_dma_cb = nullptr;
		u64 m_ps4_dma_gen = 0;
		// 0.32 (dma.flag cpu_swap): rows dma_transfer left in host byte order for imp_flush to swap; elem 0 = none.
		u32 m_ps4_swap_elem = 0;
		u32 m_ps4_swap_base = 0;
		u32 m_ps4_swap_rows = 0;
		u32 m_ps4_swap_row_bytes = 0;
		u32 m_ps4_swap_pitch = 0;
		// 0.38: frame of the last lookup (get_view) or creation: the age the window-driven eviction sorts by.
		u64 m_ps4_last_use = 0;
#endif
		vk::render_device* m_device = nullptr;
		vk::viewable_image* vram_texture = nullptr;

	public:
		using baseclass::cached_texture_section;

#ifdef RPCS3_PS4
		u64 ps4_last_use() const
		{
			return m_ps4_last_use;
		}

		// GPU memory of the image this section owns (0 for a surface or a DMA section).
		u64 ps4_vram_bytes() const
		{
			return (managed_texture && managed_texture->memory) ? managed_texture->memory->size() : 0;
		}
#endif

		void create(u16 w, u16 h, u16 depth, u16 mipmaps, vk::image* image, u32 rsx_pitch, bool managed, u32 gcm_format, bool pack_swap_bytes = false)
		{
			auto new_texture = static_cast<vk::viewable_image*>(image);
			ensure(!exists() || !is_managed() || vram_texture == new_texture);
#ifdef RPCS3_PS4
			m_ps4_last_use = vk::get_current_frame_id();
#endif

			if (vram_texture != new_texture && !managed_texture && get_protection() == utils::protection::no)
			{
				// In-place image swap, still locked. Likely a color buffer that got rebound as depth buffer or vice-versa.
				vk::as_rtt(vram_texture)->on_swap_out();

				if (!managed)
				{
					// Incoming is also an external resource, reference it immediately
					vk::as_rtt(image)->on_swap_in(is_locked());
				}
			}

			vram_texture = new_texture;

			ensure(rsx_pitch);

			width = w;
			height = h;
			this->depth = depth;
			this->mipmaps = mipmaps;
			this->rsx_pitch = rsx_pitch;

			this->gcm_format = gcm_format;
			this->pack_unpack_swap_bytes = pack_swap_bytes;

			if (managed)
			{
				managed_texture.reset(vram_texture);
			}

			if (auto rtt = dynamic_cast<vk::render_target*>(image))
			{
				swizzled = (rtt->raster_type != rsx::surface_raster_type::linear);
			}

			if (synchronized)
			{
				// Even if we are managing the same vram section, we cannot guarantee contents are static
				// The create method is only invoked when a new managed session is required
				release_dma_resources();
				synchronized = false;
				flushed = false;
				sync_timestamp = 0ull;
#ifdef RPCS3_PS4
				m_ps4_swap_elem = 0;
#endif
			}

			// Notify baseclass
			baseclass::on_section_resources_created();
		}

		void create(u16 w, u16 h, u16 depth, u16 mipmaps, vk::image* image, u32 rsx_pitch, bool managed, const vk::render_target* surface)
		{
			u32 gcm_format;
			bool swap_bytes;

			if (surface->is_depth_surface())
			{
				gcm_format = (surface->get_surface_depth_format() != rsx::surface_depth_format::z16) ? CELL_GCM_TEXTURE_DEPTH16 : CELL_GCM_TEXTURE_DEPTH24_D8;
				swap_bytes = true;
			}
			else
			{
				auto info = get_compatible_gcm_format(surface->get_surface_color_format());
				gcm_format = info.first;
				swap_bytes = info.second;
			}

			create(w, h, depth, mipmaps, image, rsx_pitch, managed, gcm_format, swap_bytes);
		}

		void release_dma_resources()
		{
			if (dma_fence)
			{
				auto gc = vk::get_resource_manager();
				gc->dispose(dma_fence);
			}
		}

		void dma_abort() override
		{
			// Called if a reset occurs, usually via reprotect path after a bad prediction.
			// Discard the sync event, the next sync, if any, will properly recreate this.
			ensure(synchronized);
			ensure(!flushed);
			ensure(dma_fence);
			vk::get_resource_manager()->dispose(dma_fence);
#ifdef RPCS3_PS4
			m_ps4_swap_elem = 0;
#endif
		}

		void destroy()
		{
			if (!exists() && context != rsx::texture_upload_context::dma)
				return;

			m_tex_cache->on_section_destroyed(*this);

			vram_texture = nullptr;
			ensure(!managed_texture);
			release_dma_resources();

			baseclass::on_section_resources_destroyed();
		}

		bool exists() const
		{
			return (vram_texture != nullptr);
		}

		bool is_managed() const
		{
			return !exists() || managed_texture;
		}

		vk::image_view* get_view(const rsx::texture_channel_remap_t& remap)
		{
			ensure(vram_texture != nullptr);
#ifdef RPCS3_PS4
			m_ps4_last_use = vk::get_current_frame_id();
#endif
			return vram_texture->get_view(remap);
		}

		vk::image_view* get_raw_view()
		{
			ensure(vram_texture != nullptr);
#ifdef RPCS3_PS4
			m_ps4_last_use = vk::get_current_frame_id();
#endif
			return vram_texture->get_view(rsx::default_remap_vector);
		}

		vk::viewable_image* get_raw_texture()
		{
			return managed_texture.get();
		}

		std::unique_ptr<vk::viewable_image>& get_texture()
		{
			return managed_texture;
		}

		vk::render_target* get_render_target() const
		{
			return vk::as_rtt(vram_texture);
		}

		VkFormat get_format() const
		{
			if (context == rsx::texture_upload_context::dma)
			{
				return VK_FORMAT_R32_UINT;
			}

			ensure(vram_texture != nullptr);
			return vram_texture->format();
		}

		bool is_flushed() const
		{
			//This memory section was flushable, but a flush has already removed protection
			return flushed;
		}

		void dma_transfer(vk::command_buffer& cmd, vk::image* src, const areai& src_area, const utils::address_range32& valid_range, u32 pitch);

		void copy_texture(vk::command_buffer& cmd, bool miss)
		{
			ensure(exists());

			if (!miss) [[likely]]
			{
				ensure(!synchronized);
				baseclass::on_speculative_flush();
			}
			else
			{
				baseclass::on_miss();
			}

			if (m_device == nullptr)
			{
				m_device = &cmd.get_command_pool().get_owner();
			}

			vk::image* locked_resource = vram_texture;
			u32 transfer_width = width;
			u32 transfer_height = height;
			u32 transfer_x = 0, transfer_y = 0;

			if (context == rsx::texture_upload_context::framebuffer_storage)
			{
				auto surface = vk::as_rtt(vram_texture);
				surface->memory_barrier(cmd, rsx::surface_access::transfer_read);
				locked_resource = surface->get_surface(rsx::surface_access::transfer_read);
				transfer_width *= surface->samples_x;
				transfer_height *= surface->samples_y;
			}

			vk::image* target = locked_resource;
			if (transfer_width != locked_resource->width() || transfer_height != locked_resource->height())
			{
				// TODO: Synchronize access to typeles textures
				target = vk::get_typeless_helper(vram_texture->format(), vram_texture->format_class(), transfer_width, transfer_height);
				target->change_layout(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

				// Allow bilinear filtering on color textures where compatibility is likely
				const auto filter = (target->aspect() == VK_IMAGE_ASPECT_COLOR_BIT) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;

				vk::copy_scaled_image(cmd, locked_resource, target,
					areai{ 0, 0, static_cast<s32>(locked_resource->width()), static_cast<s32>(locked_resource->height()) },
					areai{ 0, 0, static_cast<s32>(transfer_width), static_cast<s32>(transfer_height) },
					{}, true, filter);

				target->change_layout(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
			}

			const auto internal_bpp = vk::get_format_texel_width(vram_texture->format());
			const auto valid_range = get_confirmed_range();

			if (const auto section_range = get_section_range(); section_range != valid_range)
			{
				if (const auto offset = (valid_range.start - get_section_base()))
				{
					transfer_y = offset / rsx_pitch;
					transfer_x = (offset % rsx_pitch) / internal_bpp;

					ensure(transfer_width >= transfer_x);
					ensure(transfer_height >= transfer_y);
					transfer_width -= transfer_x;
					transfer_height -= transfer_y;
				}

				if (const auto tail = (section_range.end - valid_range.end))
				{
					const auto row_count = tail / rsx_pitch;

					ensure(transfer_height >= row_count);
					transfer_height -= row_count;
				}
			}

			areai src_area;
			src_area.x1 = static_cast<s32>(transfer_x);
			src_area.y1 = static_cast<s32>(transfer_y);
			src_area.x2 = s32(transfer_x + transfer_width);
			src_area.y2 = s32(transfer_y + transfer_height);
			dma_transfer(cmd, target, src_area, valid_range, rsx_pitch);
		}

		/**
		 * Flush
		 */
		void imp_flush() override
		{
			AUDIT(synchronized);

			// Synchronize, reset dma_fence after waiting
#ifdef RPCS3_PS4
			// 0.32: with dma.flag cpu_swap the last write into the mirror is the image copy itself (a compute shader
			// on RADV), which the event (a CP DMA fill) does not prove visible; only a completed fence does.
			const bool ps4_event_set = dma_fence->status() == VK_EVENT_SET;
			if (!ps4_event_set || m_ps4_swap_elem)
			{
				// 0.32: g_ps4_flush_queue_drained is only set by a drain that really completed, and recording
				// generations only advance on a successful fence wait; a fence wait here only happens on the RSX
				// thread for its own primary buffer.
				// 0.37: that fence wait (a working primitive here) gets the PC's 2 s: a GPU frame above 100 ms (cold
				// RADV compiles, heavy scenes) no longer lets this readback copy stale data. Other threads only poll.
				if (g_ps4_flush_queue_drained ||
					(m_ps4_dma_cb && m_ps4_dma_cb->ps4_recording_complete(m_ps4_dma_gen, GENERAL_WAIT_TIMEOUT)))
				{
					// A fence that was waited on covers the recording holding this DMA (queue drain, the
					// cleanup_after_dma_transfers fence, or the command buffer's own submit fence): the data is there.
					if (!ps4_event_set)
					{
						ps4_note_invisible_dma_event();
					}
				}
				else if (!ps4_event_set)
				{
					// Completion unknown (copy still in an open command buffer): wait, but never the 2 s that
					// froze 0.27 for every flush; the copy normally completes in well under 1 ms.
					// 0.29: counted per 10 s ('event waits'); a timeout is counted by wait_for_event.
					const u64 ps4_wait_start = get_system_time();
					vk::wait_for_event(dma_fence.get(), 100'000ull);
					rsx::g_ps4_rsx_counters.dma_event_waits++;
					rsx::g_ps4_rsx_counters.dma_event_wait_us += get_system_time() - ps4_wait_start;
				}
				else
				{
					static atomic_t<bool> s_reported{ false };
					if (!s_reported.exchange(true))
					{
						rsx_log.warning("PS4: CPU-byteswapped readback flushed on its completion event alone (no completed fence covered it yet)");
					}
				}
			}
#else
			vk::wait_for_event(dma_fence.get(), GENERAL_WAIT_TIMEOUT);
#endif

			// Calculate smallest range to flush - for framebuffers, the raster region is enough
			const auto range = (context == rsx::texture_upload_context::framebuffer_storage) ? get_section_range() : get_confirmed_range();
			auto flush_length = range.length();

			const auto tiled_region = rsx::get_current_renderer()->get_tiled_memory_region(range);
			if (tiled_region)
			{
				const auto available_tile_size = tiled_region.tile->size - (range.start - tiled_region.base_address);
				const auto max_content_size = tiled_region.tile->pitch * utils::align(height, 64);
				flush_length = std::min(max_content_size, available_tile_size);
			}

			vk::flush_dma(range.start, flush_length);

#ifdef RPCS3_PS4
			if (m_ps4_swap_elem)
			{
				// dma.flag cpu_swap: the rows dma_transfer copied are in host order; swap exactly those (not the gaps
				// between rows) in guest memory, like ShuffleBytes.glsl's bswap_u32 / bswap_u16 would have.
				// Only bytes flush_dma just wrote are touched: a last row that runs past the flushed range (a
				// confirmed range ending mid-row) must not swap guest data next to the section.
				const u64 swap_begin = range.start;
				const u64 swap_end = swap_begin + flush_length;
				for (u32 y = 0; y < m_ps4_swap_rows; ++y)
				{
					const u64 row_addr = u64{ m_ps4_swap_base } + u64{ y } * m_ps4_swap_pitch;
					if (row_addr < swap_begin || row_addr >= swap_end)
					{
						break;
					}

					u8* row = vm::get_super_ptr<u8>(static_cast<u32>(row_addr));
					const u32 row_bytes = static_cast<u32>(std::min<u64>(m_ps4_swap_row_bytes, swap_end - row_addr));

					if (m_ps4_swap_elem == 4)
					{
						for (u32 x = 0; x + 4 <= row_bytes; x += 4)
						{
							u32 texel;
							std::memcpy(&texel, row + x, 4);
							texel = __builtin_bswap32(texel);
							std::memcpy(row + x, &texel, 4);
						}
					}
					else
					{
						for (u32 x = 0; x + 2 <= row_bytes; x += 2)
						{
							u16 texel;
							std::memcpy(&texel, row + x, 2);
							texel = __builtin_bswap16(texel);
							std::memcpy(row + x, &texel, 2);
						}
					}
				}

				m_ps4_swap_elem = 0;
			}
#endif

#if DEBUG_DMA_TILING
			// Are we a tiled region?
			if (const auto tiled_region = rsx::get_current_renderer()->get_tiled_memory_region(range))
			{
				auto real_data = vm::get_super_ptr<u8>(range.start);
				auto out_data = rsx::simple_array<u8>(tiled_region.tile->size);
				rsx::tile_texel_data<u32>(
					out_data.data(),
					real_data,
					tiled_region.base_address,
					range.start - tiled_region.base_address,
					tiled_region.tile->size,
					tiled_region.tile->bank,
					tiled_region.tile->pitch,
					width,
					height
				);
				std::memcpy(real_data, out_data.data(), flush_length);
			}
#endif

			if (is_swizzled())
			{
				// This format is completely worthless to CPU processing algorithms where cache lines on die are linear.
				// If this is happening, usually it means it was not a planned readback (e.g shared pages situation)
				rsx_log.trace("[Performance warning] CPU readback of swizzled data");

				// Read-modify-write to avoid corrupting already resident memory outside texture region
				void* data = get_ptr(range.start);
				rsx::simple_array<u8> tmp_data(rsx_pitch * height);
				std::memcpy(tmp_data.data(), data, tmp_data.size());

				switch (gcm_format)
				{
				case CELL_GCM_TEXTURE_A8R8G8B8:
				case CELL_GCM_TEXTURE_DEPTH24_D8:
					rsx::convert_linear_swizzle<u32, false>(tmp_data.data(), data, width, height, rsx_pitch);
					break;
				case CELL_GCM_TEXTURE_R5G6B5:
				case CELL_GCM_TEXTURE_DEPTH16:
					rsx::convert_linear_swizzle<u16, false>(tmp_data.data(), data, width, height, rsx_pitch);
					break;
				default:
					rsx_log.error("Unexpected swizzled texture format 0x%x", gcm_format);
				}
			}
		}

		void* map_synchronized(u32, u32)
		{
			return nullptr;
		}

		void finish_flush()
		{}

		/**
		 * Misc
		 */
		void set_unpack_swap_bytes(bool swap_bytes)
		{
			pack_unpack_swap_bytes = swap_bytes;
		}

		void set_rsx_pitch(u32 pitch)
		{
			ensure(!is_locked());
			rsx_pitch = pitch;
		}

		void sync_surface_memory(const rsx::simple_array<cached_texture_section*>& surfaces)
		{
			auto rtt = vk::as_rtt(vram_texture);
			rtt->sync_tag();

			for (auto& surface : surfaces)
			{
				rtt->inherit_surface_contents(vk::as_rtt(surface->vram_texture));
			}
		}

		bool has_compatible_format(vk::image* tex) const
		{
			return vram_texture->info.format == tex->info.format;
		}

		bool is_depth_texture() const
		{
			return !!(vram_texture->aspect() & VK_IMAGE_ASPECT_DEPTH_BIT);
		}
	};

	class texture_cache : public rsx::texture_cache<vk::texture_cache, vk::texture_cache_traits>
	{
	private:
		using baseclass = rsx::texture_cache<vk::texture_cache, vk::texture_cache_traits>;
		friend baseclass;

		struct cached_image_reference_t
		{
			std::unique_ptr<vk::viewable_image> data;
			texture_cache* parent;

			cached_image_reference_t(texture_cache* parent, std::unique_ptr<vk::viewable_image>& previous);
			~cached_image_reference_t();
		};

		struct cached_image_t
		{
			u64 key;
			std::unique_ptr<vk::viewable_image> data;

			cached_image_t() = default;
			cached_image_t(u64 key_, std::unique_ptr<vk::viewable_image>& data_) :
				key(key_), data(std::move(data_)) {}
		};

	public:
		enum texture_create_flags : u32
		{
			initialize_image_contents = 1,
			do_not_reuse = 2,
			shareable = 4,
			mutable_format = 8
		};

		void on_section_destroyed(cached_texture_section& tex) override;

	private:

		// Vulkan internals
		vk::render_device* m_device;
		vk::memory_type_mapping m_memory_types;
		vk::gpu_formats_support m_formats_support;
		VkQueue m_submit_queue;
		vk::data_heap* m_texture_upload_heap;

		// Stuff that has been dereferenced by the GPU goes into these
		const u32 max_cached_image_pool_size = 256;
		std::deque<cached_image_t> m_cached_images;
		atomic_t<u64> m_cached_memory_size = { 0 };
		shared_mutex m_cached_pool_lock;

		// Blocks some operations when exiting
		atomic_t<bool> m_cache_is_exiting = false;

		void clear();

		VkComponentMapping apply_component_mapping_flags(u32 gcm_format, rsx::component_order flags, const rsx::texture_channel_remap_t& remap_vector) const;

		void copy_transfer_regions_impl(vk::command_buffer& cmd, vk::image* dst, const rsx::simple_array<copy_region_descriptor>& sections_to_transfer) const;

		vk::image* get_template_from_collection_impl(const rsx::simple_array<copy_region_descriptor>& sections_to_transfer) const;

		std::unique_ptr<vk::viewable_image> find_cached_image(VkFormat format, u16 w, u16 h, u16 d, u16 mipmaps, VkImageType type, VkImageCreateFlags create_flags, VkImageUsageFlags usage, VkSharingMode sharing);

	protected:
		vk::image_view* create_temporary_subresource_view_impl(
			vk::command_buffer& cmd, vk::image* source, VkImageType image_type, VkImageViewType view_type,
			u32 gcm_format, u16 w, u16 h, u16 d, u8 mips, const rsx::texture_channel_remap_t& remap_vector,
			const copy_region_descriptor* copy = nullptr);

		vk::image_view* create_temporary_subresource_view(vk::command_buffer& cmd, const deferred_subresource& desc) override;

		vk::image_view* generate_cubemap_from_images(vk::command_buffer& cmd, const deferred_subresource& desc) override;

		vk::image_view* generate_3d_from_2d_images(vk::command_buffer& cmd, const deferred_subresource& desc) override;

		vk::image_view* generate_atlas_from_images(vk::command_buffer& cmd, const deferred_subresource& desc) override;

		vk::image_view* generate_2d_mipmaps_from_images(vk::command_buffer& cmd, const deferred_subresource& desc) override;

		void release_temporary_subresource(vk::image_view* view) override;

		void initialize_subresource_from_memory(vk::command_buffer& cmd, vk::image* dst, const deferred_subresource& desc, rsx::texture_dimension_extended type) const;

		void update_image_contents(vk::command_buffer& cmd, vk::image_view* dst_view, const deferred_subresource& desc) override;

		cached_texture_section* create_new_texture(vk::command_buffer& cmd, const utils::address_range32& rsx_range, u16 width, u16 height, u16 depth, u16 mipmaps, u32 pitch,
			u32 gcm_format, rsx::texture_upload_context context, rsx::texture_dimension_extended type, bool swizzled, rsx::component_order swizzle_flags, rsx::flags32_t flags) override;

		cached_texture_section* create_nul_section(vk::command_buffer& cmd, const utils::address_range32& rsx_range, const rsx::image_section_attributes_t& attrs,
			const rsx::GCM_tile_reference& tile, bool memory_load) override;

		cached_texture_section* upload_image_from_cpu(vk::command_buffer& cmd, const utils::address_range32& rsx_range, u16 width, u16 height, u16 depth, u16 mipmaps, u32 pitch, u32 gcm_format,
			rsx::texture_upload_context context, const std::vector<rsx::subresource_layout>& subresource_layout, rsx::texture_dimension_extended type, bool swizzled) override;

		void set_component_order(cached_texture_section& section, u32 gcm_format, rsx::component_order expected_flags) override;

		void insert_texture_barrier(vk::command_buffer& cmd, vk::image* tex, bool strong_ordering) override;

		bool render_target_format_is_compatible(vk::image* tex, u32 gcm_format) override;

		void prepare_for_dma_transfers(vk::command_buffer& cmd) override;

		void cleanup_after_dma_transfers(vk::command_buffer& cmd) override;

	public:
		using baseclass::texture_cache;

#ifdef RPCS3_PS4
		// 0.28: submit an empty command buffer with a fence and wait for it, so that everything submitted to
		// the graphics queue before (including DMA copies recorded earlier) has completed. Caller must hold
		// the same external synchronization as any other submit (RSX thread parked or the RSX thread itself).
		// 0.32: returns false when the wait did not complete (2 s timeout or an error); nothing is drained then.
		bool ps4_drain_queue(vk::command_buffer& cmd);

		// 0.38: evicts sampled textures not looked up for min_age frames, least recently used first, until 'target'
		// bytes (utils::ps4::vram::plan_eviction); 'keep' holds the addresses of the bound textures. RSX thread. Returns
		// the bytes evicted (0 when nothing qualified or the cache is held by this thread).
		u64 ps4_evict_by_age(const std::set<u32>& keep, u64 now_frame, u64 min_age, u64 target);

		// 0.38: destroys the reusable image pool now. Its images are idle (they enter it only after the garbage
		// collector saw the GPU finish with them). Returns the bytes freed.
		u64 ps4_drop_image_pool();

		// 0.38: true when a section overlapping 'range' is a DMA section (the mirror is its storage) or holds a
		// readback the CPU has not flushed yet: that DMA mirror block must stay. Also true when the cache lock cannot be
		// taken (conservative). Never called with the DMA pool lock held.
		bool ps4_dma_range_pinned(const utils::address_range32& range);

		// 0.38: texture cache bytes above the PS4 quota (utils::ps4::vram::texture_quota), without the log line.
		bool ps4_over_quota() const;
#endif

		void initialize(vk::render_device& device, VkQueue submit_queue, vk::data_heap& upload_heap);

		void destroy() override;

		std::unique_ptr<vk::viewable_image> create_temporary_subresource_storage(
			rsx::format_class format_class, VkFormat format,
			u16 width, u16 height, u16 depth, u16 layers, u8 mips,
			VkImageType image_type, VkFlags image_flags, VkFlags usage_flags);

		void dispose_reusable_image(std::unique_ptr<vk::viewable_image>& tex);

		bool is_depth_texture(u32 rsx_address, u32 rsx_size) override;

		void on_frame_end() override;

		vk::viewable_image* upload_image_simple(vk::command_buffer& cmd, VkFormat format, u32 address, u32 width, u32 height, u32 pitch);

		bool blit(const rsx::blit_src_info& src, const rsx::blit_dst_info& dst, bool interpolate, vk::surface_cache& m_rtts, vk::command_buffer& cmd);

		u32 get_unreleased_textures_count() const override;

		bool handle_memory_pressure(rsx::problem_severity severity) override;

		u64 get_temporary_memory_in_use() const;

		bool is_overallocated() const;
	};
}
