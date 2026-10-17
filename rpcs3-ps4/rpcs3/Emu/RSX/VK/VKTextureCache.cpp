#include "stdafx.h"
#include "Emu/RSX/VK/VKGSRenderTypes.hpp"
#include "VKTextureCache.h"
#include "VKCompute.h"
#include "VKAsyncScheduler.h"
#include "vkutils/data_heap.h"

#include "util/asm.hpp"

#ifdef RPCS3_PS4
#include "VKGSRender.h"
#include "ps4_fatal.h"
#include "ps4_launcher.h"
#include "vkutils/memory.h"
#include "util/ps4_gpu_health.hpp"
#include "util/ps4_vram_policy.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string_view>
#endif

namespace vk
{
#ifdef RPCS3_PS4
	namespace
	{
		// dma.flag switches (VKDMA.h). These are the defaults until ps4_load_dma_flags() has run.
		// 0.37: trace defaults to off (see ps4_load_dma_flags).
		std::atomic<bool> s_dma_labels{ true };
		std::atomic<bool> s_dma_isolate{ true };
		std::atomic<bool> s_dma_speculate{ false };
		std::atomic<bool> s_dma_trace{ false };
		std::atomic<bool> s_dma_cpu_swap{ false };

		constexpr char dma_flag_path[] = "/data/rpcs3/dma.flag";
		constexpr usz dma_flag_max_bytes = 4096;

		std::string_view trim_flag_text(std::string_view s)
		{
			while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
			while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
			return s;
		}

		bool parse_flag_switch(std::string_view value, bool& out)
		{
			if (value == "on" || value == "1" || value == "true" || value == "yes")
			{
				out = true;
				return true;
			}

			if (value == "off" || value == "0" || value == "false" || value == "no")
			{
				out = false;
				return true;
			}

			return false;
		}

		// Readback progress trace. Before each step of a readback a marker (seq << 4 | step) is written into one
		// small host-visible buffer once every earlier command has finished (barrier + CP DMA fill, the same
		// primitive the DMA mirror relies on for CPU visibility). After a hang the buffer holds the last step
		// the GPU got through. Steps: 1 = work before the readback done, 2 = image copied to the scratch buffer,
		// 3 = byteswap shader done, 4 = data copied into the DMA mirror, 5 = completion event signalled.
		// Readbacks are recorded by the RSX thread and by faulting threads, at the same time when a hard sync served a
		// flush request without parking the RSX. s_trace_mutex guards the creation and release of the buffer and keeps
		// the newest marker and its command buffer consistent; the buffer itself is only published through
		// s_trace_handle and s_trace_ptr, so recording reads no unique_ptr another thread may be assigning.
		std::mutex s_trace_mutex;
		std::unique_ptr<vk::buffer> s_trace_buffer;
		std::atomic<VkBuffer> s_trace_handle{ VK_NULL_HANDLE };
		std::atomic<const volatile u32*> s_trace_ptr{ nullptr };
		std::atomic<bool> s_trace_failed{ false };
		std::atomic<u32> s_trace_seq{ 0 };
		std::atomic<u32> s_trace_last{ 0 };
		std::atomic<const vk::command_buffer*> s_trace_last_cb{ nullptr };
		std::atomic<u64> s_trace_last_gen{ 0 };
		std::atomic<bool> s_trace_visibility_checked{ false };
		std::atomic<u32> s_trace_reports{ 0 };
		// Early snapshots (ps4_dma_trace_snapshot) taken this session; they get their own small budget so that slow
		// but healthy waits cannot use up the startup.log reports a real hang needs.
		std::atomic<u32> s_trace_snapshots{ 0 };
		constexpr u32 trace_snapshot_limit = 4;
		std::atomic<bool> s_first_readback_noted{ false };

		// Result of the newest cleanup_after_dma_transfers wait on this thread (ps4_drain_queue returns it).
		thread_local bool g_ps4_last_dma_submit_done = true;

		// Returns the sequence number of a new traced readback, or 0 when tracing is off.
		u32 ps4_dma_trace_begin(const vk::render_device& dev)
		{
			if (!s_dma_trace.load(std::memory_order_relaxed) || s_trace_failed.load(std::memory_order_relaxed))
			{
				return 0;
			}

			if (s_trace_handle.load() == VK_NULL_HANDLE)
			{
				// Created once per session under the lock: a second creation racing this one would destroy a buffer
				// that a marker fill was already recorded into (a GPU write to freed memory).
				std::lock_guard lock(s_trace_mutex);

				if (!s_trace_buffer)
				{
					if (s_trace_failed.load())
					{
						return 0;
					}

					// Diagnostics only: a failed allocation disables the trace instead of running VRAM recovery or dying.
					// The flags keep the pressure handler out, so nothing re-enters this lock.
					s_trace_buffer = std::make_unique<vk::buffer>(dev, 256, dev.get_memory_mapping().host_visible_coherent,
						VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
						VK_BUFFER_CREATE_ALLOW_NULL_RPCS3 | VK_BUFFER_CREATE_IGNORE_VMEM_PRESSURE_RPCS3, VMM_ALLOCATION_POOL_SYSTEM);

					if (s_trace_buffer->value == VK_NULL_HANDLE)
					{
						s_trace_buffer.reset();
						s_trace_failed = true;
						rsx_log.error("PS4: no video memory for the DMA readback trace; tracing stays off");
						return 0;
					}

					auto ptr = static_cast<volatile u32*>(s_trace_buffer->map(0, 256));
					*ptr = 0;
					s_trace_ptr.store(ptr);
					s_trace_handle.store(s_trace_buffer->value);
				}
			}

			// 28 bits of sequence number (the low 4 bits of a marker hold the step); 0 means "not traced".
			const u32 seq = (s_trace_seq.fetch_add(1, std::memory_order_relaxed) + 1) & 0x0fffffff;
			return seq ? seq : 1;
		}

		void ps4_dma_trace(const vk::command_buffer& cmd, u32 seq, u32 step)
		{
			const VkBuffer handle = s_trace_handle.load();
			if (!seq || handle == VK_NULL_HANDLE)
			{
				return;
			}

			// Execution and memory dependency on everything recorded before, so the marker means "done up to here".
			vk::insert_global_memory_barrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
				VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);

			const u32 value = (seq << 4) | step;
			vkCmdFillBuffer(cmd, handle, 0, 4, value);

			// The three values describe one marker; ps4_dma_trace_check_visibility reads them as a set.
			std::lock_guard lock(s_trace_mutex);
			s_trace_last_cb.store(&cmd);
			s_trace_last_gen.store(cmd.ps4_record_gen);
			s_trace_last.store(value);
		}

		void ps4_dma_trace_release()
		{
			std::lock_guard lock(s_trace_mutex);
			s_trace_handle.store(VK_NULL_HANDLE);
			s_trace_ptr.store(nullptr);

			if (s_trace_buffer)
			{
				// The renderer has waited for the device to go idle before the texture cache is destroyed.
				s_trace_buffer->unmap();
				s_trace_buffer.reset();
			}
		}

		// Once per session, after the first successful wait of a submission whose last trace marker it holds:
		// tells whether the CPU sees the markers at all, so a stale value is never taken for the stop point.
		void ps4_dma_trace_check_visibility(const vk::command_buffer& cmd, u64 gen)
		{
			if (s_trace_visibility_checked.load(std::memory_order_relaxed))
			{
				return;
			}

			u32 seen = 0;
			u32 last = 0;
			{
				// Read the newest marker and its command buffer as one set: another thread recording a readback in
				// between would otherwise pair this buffer with its marker and report a false "not CPU-visible".
				std::lock_guard lock(s_trace_mutex);

				const volatile u32* ptr = s_trace_ptr.load();
				if (!ptr || s_trace_last_cb.load() != &cmd || s_trace_last_gen.load() != gen || s_trace_visibility_checked.exchange(true))
				{
					return;
				}

				seen = *ptr;
				last = s_trace_last.load();
			}

			char line[192];

			if (seen == last)
			{
				std::snprintf(line, sizeof(line), "DMA trace: the CPU sees the GPU progress markers (readback #%u step %u after its fence)", last >> 4, last & 15);
				rsx_log.notice("PS4: %s", line);
				ps4_startup_checkpoint(line);
				return;
			}

			std::snprintf(line, sizeof(line), "DMA trace not CPU-visible: fence passed readback #%u step %u but the CPU reads #%u step %u; later trace lines are unreliable",
				last >> 4, last & 15, seen >> 4, seen & 15);
			rsx_log.error("PS4: %s", line);
			ps4_fatal_note("GPU: ", line);
		}

		// One fsync'd startup.log line describing the first readback of the session.
		void ps4_note_first_readback(const vk::command_buffer& cmd, VkFormat format, u32 width, u32 height,
			bool swap, bool tiled, bool conversion, bool cpu_swap)
		{
			if (s_first_readback_noted.exchange(true))
			{
				return;
			}

			char detail[320];
			std::snprintf(detail, sizeof(detail), "first readback of the session: VkFormat %d, %ux%u, byteswap %s, tiled %s, "
				"format conversion %s, %s thread, %s command buffer, events %s, isolate %s, trace %s, cpu_swap %s",
				static_cast<int>(format), width, height, swap ? "yes" : "no", tiled ? "yes" : "no", conversion ? "yes" : "no",
				rsx::ps4_is_rsx_thread() ? "RSX" : "non-RSX",
				cmd.access_hint == vk::command_buffer::access_type_hint::flush_only ? "primary" : "secondary",
				s_dma_labels.load() ? "label" : "vk", s_dma_isolate.load() ? "on" : "off",
				s_dma_trace.load() ? "on" : "off", cpu_swap ? "used" : (s_dma_cpu_swap.load() ? "on, not applicable" : "off"));
			rsx_log.notice("PS4 DMA: %s", detail);
			ps4_fatal_note("DMA: ", detail);
		}

		// 0.36: readbacks of this renderer, for the parameter log of the first ones.
		std::atomic<u32> s_ps4_readbacks{ 0 };

		// 0.36: GoW 0.35 hung the GPU in a compute dispatch whose buffer range ran past its mapping, recorded right after
		// a series of readbacks. The first 16 readbacks of a renderer are logged with their parameters, and so is every
		// readback whose parameters are impossible; dma_transfer then records no GPU work for it and keeps the CPU copy.
		// Returns true for such a readback.
		bool ps4_check_readback(const vk::image* src, const areai& src_area, u32 internal_bpp, u32 transfer_width, u32 transfer_height,
			u32 pitch, bool gpu_transform)
		{
			const u32 number = s_ps4_readbacks.fetch_add(1, std::memory_order_relaxed) + 1;

			// 0.37: the readback numbers of an access-violation flush, for its timing line.
			g_ps4_readback_tls.first = g_ps4_readback_tls.first ? g_ps4_readback_tls.first : number;
			g_ps4_readback_tls.last = number;
			g_ps4_readback_tls.count++;

			const u64 task = u64{ internal_bpp } * transfer_width * transfer_height;
			const u32 task32 = internal_bpp * transfer_width * transfer_height; // what dma_transfer computes
			const u64 working = gpu_transform ? calculate_working_buffer_size(task32, src->aspect()) : 0;

			const char* reason = nullptr;
			if (!transfer_width || !transfer_height || !internal_bpp)
			{
				reason = "empty transfer";
			}
			else if (src_area.x1 < 0 || src_area.y1 < 0 || src_area.x2 > static_cast<s32>(src->width()) || src_area.y2 > static_cast<s32>(src->height()))
			{
				reason = "area outside the source image";
			}
			else if (task > 64 * 0x100000)
			{
				reason = "task above 64 MiB";
			}
			else if (gpu_transform && (task32 != task || task > working))
			{
				reason = "task larger than the working buffer";
			}

			if (!reason && number > 16)
			{
				return false;
			}

			const std::string thread = rsx::ps4_is_rsx_thread() ? std::string("RSX") : thread_ctrl::get_name();
			char detail[384];
			std::snprintf(detail, sizeof(detail), "readback #%u%s%s%s area (%d,%d)-(%d,%d) of %ux%u, bpp %u, pitch %u, task %llu bytes, working buffer %llu, thread %s",
				number, reason ? " REJECTED (" : "", reason ? reason : "", reason ? ", CPU copy kept)" : "",
				src_area.x1, src_area.y1, src_area.x2, src_area.y2, src->width(), src->height(), internal_bpp, pitch,
				static_cast<unsigned long long>(task), static_cast<unsigned long long>(working), thread.c_str());

			if (!reason)
			{
				rsx_log.notice("PS4 DMA: %s", detail);
				ps4_fatal_note("DMA: ", detail);
				return false;
			}

			// RPCS3.log gets the 1st-16th and then every power of two; startup.log (fsync'd) the first 8.
			const u32 rejected = utils::ps4::gpu::note_rejected_readback();
			if (rejected <= 16 || !(rejected & (rejected - 1)))
			{
				rsx_log.error("PS4 DMA: %s (rejected readback %u)", detail, rejected);
			}

			if (rejected <= 8)
			{
				ps4_fatal_note("DMA: ", detail);
			}

			return true;
		}
	}

	bool ps4_dma_events_use_labels() { return s_dma_labels.load(std::memory_order_relaxed); }
	bool ps4_dma_isolate() { return s_dma_isolate.load(std::memory_order_relaxed); }
	bool ps4_dma_speculate() { return s_dma_speculate.load(std::memory_order_relaxed); }
	bool ps4_dma_trace_enabled() { return s_dma_trace.load(std::memory_order_relaxed); }
	bool ps4_dma_cpu_swap() { return s_dma_cpu_swap.load(std::memory_order_relaxed); }

	void ps4_load_dma_flags()
	{
		bool labels = true;
		bool isolate = true;
		bool speculate = false;
		// 0.37: off by default. The markers add 5 global barriers and buffer fills to every readback, each one waiting
		// for all earlier GPU work; dma.flag trace=on keeps them for a GPU-hang hunt.
		bool trace = false;
		bool cpu_swap = false;
		std::string source = "defaults, no dma.flag";
		std::string ignored;

		if (std::FILE* file = std::fopen(dma_flag_path, "rb"))
		{
			// Like vram.flag, a file larger than the limit is ignored as a whole.
			std::vector<char> text(dma_flag_max_bytes + 1);
			const usz length = std::fread(text.data(), 1, text.size(), file);
			std::fclose(file);

			if (length > dma_flag_max_bytes)
			{
				source = "dma.flag is larger than 4 KiB and was ignored; defaults";
			}
			else
			{
				source = "dma.flag";
				std::string_view rest(text.data(), length);

				while (!rest.empty())
				{
					const usz nl = rest.find('\n');
					const std::string_view line = trim_flag_text(rest.substr(0, nl));
					rest.remove_prefix(nl == std::string_view::npos ? rest.size() : nl + 1);

					if (line.empty() || line.front() == '#')
					{
						continue;
					}

					const usz eq = line.find('=');
					const std::string_view key = trim_flag_text(line.substr(0, eq == std::string_view::npos ? line.size() : eq));
					const std::string_view value = eq == std::string_view::npos ? std::string_view{} : trim_flag_text(line.substr(eq + 1));

					bool known = false;
					if (key == "events")
					{
						if (value == "label" || value == "vk")
						{
							labels = (value == "label");
							known = true;
						}
					}
					else if (key == "isolate") known = parse_flag_switch(value, isolate);
					else if (key == "speculate") known = parse_flag_switch(value, speculate);
					else if (key == "trace") known = parse_flag_switch(value, trace);
					else if (key == "cpu_swap") known = parse_flag_switch(value, cpu_swap);

					if (!known)
					{
						fmt::append(ignored, "%s'%s'", ignored.empty() ? "" : ", ", line.substr(0, 48));
					}
				}
			}
		}

		s_dma_labels = labels;
		s_dma_isolate = isolate;
		s_dma_speculate = speculate;
		s_dma_trace = trace;
		s_dma_cpu_swap = cpu_swap;

		// A new renderer starts a new trace session.
		s_trace_failed = false;
		s_trace_seq = 0;
		s_trace_last = 0;
		s_trace_last_cb = nullptr;
		s_trace_last_gen = 0;
		s_trace_visibility_checked = false;
		s_trace_reports = 0;
		s_trace_snapshots = 0;
		s_first_readback_noted = false;
		s_ps4_readbacks = 0;

		const std::string line = fmt::format("DMA flags: events=%s isolate=%s speculate=%s trace=%s cpu_swap=%s (%s)%s%s",
			labels ? "label" : "vk", isolate ? "on" : "off", speculate ? "on" : "off", trace ? "on" : "off", cpu_swap ? "on" : "off",
			source, ignored.empty() ? "" : "; ignored lines: ", ignored);
		rsx_log.notice("PS4: %s", line);
		ps4_startup_checkpoint(line.c_str());
	}

	void ps4_dma_trace_report(const char* where) noexcept
	{
		const volatile u32* ptr = s_trace_ptr.load();
		if (!ptr)
		{
			return;
		}

		const u32 seen = *ptr;
		const u32 last = s_trace_last.load();
		char line[224];

		if (!seen)
		{
			std::snprintf(line, sizeof(line), "DMA trace (%s): the GPU has not finished any traced step; last recorded readback #%u step %u",
				where ? where : "?", last >> 4, last & 15);
		}
		else
		{
			std::snprintf(line, sizeof(line), "DMA trace (%s): GPU finished step %u of readback #%u; last recorded #%u step %u%s",
				where ? where : "?", seen & 15, seen >> 4, last >> 4, last & 15, seen == last ? " (every recorded step finished)" : "");
		}

		rsx_log.error("PS4: %s", line);

		// startup.log is fsync'd; a game that keeps stalling must not flood it.
		if (s_trace_reports.fetch_add(1, std::memory_order_relaxed) < 16)
		{
			ps4_fatal_note("GPU: ", line);
		}
	}

	void ps4_dma_trace_snapshot(const char* where) noexcept
	{
		// Only once a readback was traced, so waits before the first readback do not use up the budget.
		if (!s_trace_ptr.load() || s_trace_snapshots.fetch_add(1, std::memory_order_relaxed) >= trace_snapshot_limit)
		{
			return;
		}

		ps4_dma_trace_report(where);
	}
#endif

	u64 hash_image_properties(VkFormat format, u16 w, u16 h, u16 d, u16 mipmaps, VkImageType type, VkImageCreateFlags create_flags, VkSharingMode sharing_mode)
	{
		/**
		* Key layout:
		* 00-08: Format  (Max 255)
		* 08-24: Width   (Max 64K)
		* 24-40: Height  (Max 64K)
		* 40-48: Depth   (Max 255)
		* 48-54: Mipmaps (Max 63)   <- We have some room here, it is not possible to have more than 12 mip levels on PS3 and 16 on PC is pushing it.
		* 54-56: Type    (Max 3)
		* 56-57: Sharing (Max 1)    <- Boolean. Exclusive = 0, shared = 1
		* 57-64: Flags   (Max 127)  <- We have some room here, we only care about a small subset of create flags.
		*/
		ensure(static_cast<u32>(format) < 0xFF);
		return (static_cast<u64>(format) & 0xFF) |
			(static_cast<u64>(w) << 8) |
			(static_cast<u64>(h) << 24) |
			(static_cast<u64>(d) << 40) |
			(static_cast<u64>(mipmaps) << 48) |
			(static_cast<u64>(type) << 54) |
			(static_cast<u64>(sharing_mode) << 56) |
			(static_cast<u64>(create_flags) << 57);
	}

	texture_cache::cached_image_reference_t::cached_image_reference_t(texture_cache* parent, std::unique_ptr<vk::viewable_image>& previous)
	{
		ensure(previous);

		this->parent = parent;
		this->data = std::move(previous);
	}

	texture_cache::cached_image_reference_t::~cached_image_reference_t()
	{
		// Erase layout information to force TOP_OF_PIPE transition next time.
		data->current_layout = VK_IMAGE_LAYOUT_UNDEFINED;
		data->current_queue_family = VK_QUEUE_FAMILY_IGNORED;

		// Move this object to the cached image pool
		const auto key = hash_image_properties(data->format(), data->width(), data->height(), data->depth(), data->mipmaps(), data->info.imageType, data->info.flags, data->info.sharingMode);
		std::lock_guard lock(parent->m_cached_pool_lock);

		if (!parent->m_cache_is_exiting)
		{
			parent->m_cached_memory_size += data->memory->size();
			parent->m_cached_images.emplace_front(key, data);
		}
		else
		{
			// Destroy if the cache is closed. The GPU is done with this resource anyway.
			data.reset();
		}
	}

	void cached_texture_section::dma_transfer(vk::command_buffer& cmd, vk::image* src, const areai& src_area, const utils::address_range32& valid_range, u32 pitch)
	{
#ifdef RPCS3_PS4
		const utils::ps4::gpu::rp_close_cause_scope ps4_cause(rsx::ps4_rp_close_texture_cache);
#endif
		ensure(src->samples() == 1);

		if (!m_device)
		{
			m_device = &cmd.get_command_pool().get_owner();
		}

		if (dma_fence)
		{
			// NOTE: This can be reached if previously synchronized, or a special path happens.
			// If a hard flush occurred while this surface was flush_always the cache would have reset its protection afterwards.
			// DMA resource would still be present but already used to flush previously.
			vk::get_resource_manager()->dispose(dma_fence);
		}

		if (vk::is_renderpass_open(cmd))
		{
			vk::end_renderpass(cmd);
		}

		src->push_layout(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

#ifdef RPCS3_PS4
		// 0.32: GPU progress markers (dma.flag trace). Step 1: everything recorded before this readback, including
		// the draws into the source, has finished.
		const u32 ps4_trace_seq = ps4_dma_trace_begin(*m_device);
		ps4_dma_trace(cmd, ps4_trace_seq, 1);
		m_ps4_swap_elem = 0;
#endif

		const auto internal_bpp = vk::get_format_texel_width(src->format());
		const auto transfer_width = static_cast<u32>(src_area.width());
		const auto transfer_height = static_cast<u32>(src_area.height());
		real_pitch = internal_bpp * transfer_width;
		rsx_pitch = pitch;

		const bool require_format_conversion = !!(src->aspect() & VK_IMAGE_ASPECT_STENCIL_BIT) || src->format() == VK_FORMAT_D32_SFLOAT;
		const auto tiled_region = rsx::get_current_renderer()->get_tiled_memory_region(valid_range);
		const bool require_tiling = !!tiled_region;
#ifdef RPCS3_PS4
		// 0.32 (dma.flag cpu_swap=on, off by default): a small colour readback that only needs a byteswap skips the
		// scratch buffer, the shuffle shader and the buffer copy. The image is copied straight into the DMA mirror
		// and imp_flush swaps exactly those rows on the CPU (same per-element swap as ShuffleBytes.glsl).
		const u32 ps4_swap_elem = pack_unpack_swap_bytes ? static_cast<u32>(vk::get_format_element_size(src->format()).first) : 0;
		const bool ps4_cpu_swap = vk::ps4_dma_cpu_swap() && pack_unpack_swap_bytes && !require_format_conversion && !require_tiling &&
			!is_swizzled() && src->aspect() == VK_IMAGE_ASPECT_COLOR_BIT && (ps4_swap_elem == 2 || ps4_swap_elem == 4) &&
			internal_bpp && (rsx_pitch % internal_bpp) == 0 && (valid_range.start % internal_bpp) == 0 &&
			rsx_pitch >= real_pitch && // the direct copy's bufferRowLength may not be narrower than a row
			u64{ transfer_width } * transfer_height * internal_bpp <= 256 * 1024;
		const bool require_gpu_transform = require_format_conversion || (pack_unpack_swap_bytes && !ps4_cpu_swap) || require_tiling;

		ps4_note_first_readback(cmd, src->format(), transfer_width, transfer_height, pack_unpack_swap_bytes, require_tiling,
			require_format_conversion, ps4_cpu_swap);

		const bool ps4_rejected = ps4_check_readback(src, src_area, internal_bpp, transfer_width, transfer_height, rsx_pitch, require_gpu_transform);
#else
		const bool require_gpu_transform = require_format_conversion || pack_unpack_swap_bytes || require_tiling;
#endif

		auto dma_sync_region = valid_range;
		dma_mapping_handle dma_mapping = { 0, nullptr };

		auto dma_sync = [&](bool load, bool force = false)
		{
			if (dma_mapping.second && !force)
			{
				return;
			}

			dma_mapping = vk::map_dma(dma_sync_region.start, dma_sync_region.length());
			if (load)
			{
				vk::load_dma(dma_sync_region.start, dma_sync_region.length());
			}
		};

#ifdef RPCS3_PS4
		if (ps4_rejected)
		{
			// No GPU work: the mirror receives the current guest memory, so the flush writes back what the CPU sees.
			dma_sync(true);
		}
		else
#endif
		if (require_gpu_transform)
		{
			const auto transfer_pitch = real_pitch;
			const auto task_length = transfer_pitch * src_area.height();
			auto working_buffer_length = calculate_working_buffer_size(task_length, src->aspect());

#if !DEBUG_DMA_TILING
			if (require_tiling)
			{
				// Safety padding
				working_buffer_length += tiled_region.tile->size;

				// Calculate actual working section for the memory op
				dma_sync_region = tiled_region.tile_align(dma_sync_region);
			}
#endif
			u32 result_offset = 0;
			auto working_buffer = vk::get_scratch_buffer(cmd, working_buffer_length,
				VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
				VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT);

			VkBufferImageCopy region = {};
			region.imageSubresource = { src->aspect(), 0, 0, 1 };
			region.imageOffset = { src_area.x1, src_area.y1, 0 };
			region.imageExtent = { transfer_width, transfer_height, 1 };

			bool require_rw_barrier = true;
			image_readback_options_t xfer_options{};
			xfer_options.swap_bytes = require_format_conversion && pack_unpack_swap_bytes;
			vk::copy_image_to_buffer(cmd, src, working_buffer, region, xfer_options);
#ifdef RPCS3_PS4
			ps4_dma_trace(cmd, ps4_trace_seq, 2);
#endif

			// NOTE: For depth/stencil formats, copying to buffer and byteswap are combined into one step above
			if (pack_unpack_swap_bytes && !require_format_conversion)
			{
				const auto texel_layout = vk::get_format_element_size(src->format());
				const auto elem_size = texel_layout.first;
				vk::cs_shuffle_base* shuffle_kernel;

				if (elem_size == 2)
				{
					shuffle_kernel = vk::get_compute_task<vk::cs_shuffle_16>();
				}
				else if (elem_size == 4)
				{
					shuffle_kernel = vk::get_compute_task<vk::cs_shuffle_32>();
				}
				else
				{
					ensure(get_context() == rsx::texture_upload_context::dma);
					shuffle_kernel = nullptr;
				}

				if (shuffle_kernel)
				{
					vk::insert_buffer_memory_barrier(cmd, working_buffer->value, 0, task_length,
						VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
						VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);

					shuffle_kernel->run(cmd, working_buffer, task_length);
#ifdef RPCS3_PS4
					ps4_dma_trace(cmd, ps4_trace_seq, 3);
#endif

					if (!require_tiling)
					{
						vk::insert_buffer_memory_barrier(cmd, working_buffer->value, 0, task_length,
							VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
							VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);

						require_rw_barrier = false;
					}
				}
			}

			if (require_tiling)
			{
#if !DEBUG_DMA_TILING
				// Compute -> Compute barrier
				vk::insert_buffer_memory_barrier(cmd, working_buffer->value, 0, task_length,
					VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
					VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);

				// We don't need to calibrate write if two conditions are met:
				// 1. The start offset of our 2D region is a multiple of 64 lines
				// 2. We use the whole pitch.
				// If these conditions are not met, we need to upload the entire tile (or at least the affected tiles wholly)

				// FIXME: There is a 3rd condition - write onto already-persisted range. e.g One transfer copies half the image then the other half is copied later.
				// We don't need to load again for the second copy in that scenario.

				if (valid_range.start != dma_sync_region.start || real_pitch != tiled_region.tile->pitch)
				{
					// Tile indices run to the end of the row (full pitch).
					// Tiles address outside their 64x64 area too, so we need to actually load the whole thing and "fill in" missing blocks.
					// Visualizing "hot" pixels when doing a partial copy is very revealing, there's lots of data from the padding areas to be filled in.

					dma_sync(true);
					ensure(dma_mapping.second);

					// Upload memory to the working buffer
					const auto dst_offset = task_length; // Append to the end of the input
					VkBufferCopy mem_load{};
					mem_load.srcOffset = dma_mapping.first;
					mem_load.dstOffset = dst_offset;
					mem_load.size = dma_sync_region.length();
					vkCmdCopyBuffer(cmd, dma_mapping.second->value, working_buffer->value, 1, &mem_load);

					// Transfer -> Compute barrier
					vk::insert_buffer_memory_barrier(cmd, working_buffer->value, dst_offset, dma_sync_region.length(),
						VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
						VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_WRITE_BIT);
				}

				// Prepare payload
				const RSX_detiler_config config =
				{
					.tile_base_address = tiled_region.base_address,
					.tile_base_offset = valid_range.start - tiled_region.base_address,
					.tile_rw_offset = dma_sync_region.start - tiled_region.base_address,
					.tile_size = tiled_region.tile->size,
					.tile_pitch = tiled_region.tile->pitch,
					.bank = tiled_region.tile->bank,

					.dst = working_buffer,
					.dst_offset = task_length,
					.src = working_buffer,
					.src_offset = 0,

					// TODO: Check interaction with anti-aliasing
					.image_width = static_cast<u16>(transfer_width),
					.image_height = static_cast<u16>(transfer_height),
					.image_pitch = real_pitch,
					.image_bpp = context == rsx::texture_upload_context::dma ? internal_bpp : rsx::get_format_block_size_in_bytes(gcm_format)
				};

				// Execute
				const auto job = vk::get_compute_task<vk::cs_tile_memcpy<RSX_detiler_op::encode>>();
				job->run(cmd, config);

				// Update internal variables
				result_offset = task_length;
				real_pitch = tiled_region.tile->pitch; // We're always copying the full image. In case of partials we're "filling in" blocks, not doing partial 2D copies.
				require_rw_barrier = true;

#if VISUALIZE_GPU_TILING
				if (g_cfg.video.renderdoc_compatiblity)
				{
					vk::insert_buffer_memory_barrier(cmd, working_buffer->value, result_offset, working_buffer_length,
						VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
						VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);

					// Debug write
					auto scratch_img = vk::get_typeless_helper(VK_FORMAT_B8G8R8A8_UNORM, RSX_FORMAT_CLASS_COLOR, tiled_region.tile->pitch / 4, 768);
					scratch_img->change_layout(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

					VkBufferImageCopy dbg_copy{};
					dbg_copy.bufferOffset = config.dst_offset;
					dbg_copy.imageExtent.width = width;
					dbg_copy.imageExtent.height = height;
					dbg_copy.imageExtent.depth = 1;
					dbg_copy.bufferRowLength = tiled_region.tile->pitch / 4;
					dbg_copy.imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .mipLevel = 0, .baseArrayLayer = 0, .layerCount = 1 };
					vk::copy_buffer_to_image(cmd, working_buffer, scratch_img, dbg_copy);

					scratch_img->change_layout(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
				}
#endif

#endif
			}

			if (require_rw_barrier)
			{
				vk::insert_buffer_memory_barrier(cmd, working_buffer->value, result_offset, dma_sync_region.length(),
					VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
					VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
					VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
					VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT);
			}

			if (rsx_pitch == real_pitch) [[likely]]
			{
				dma_sync(false);

				VkBufferCopy copy = {};
				copy.srcOffset = result_offset;
				copy.dstOffset = dma_mapping.first;
				copy.size = dma_sync_region.length();
				vkCmdCopyBuffer(cmd, working_buffer->value, dma_mapping.second->value, 1, &copy);
			}
			else
			{
				dma_sync(true);

				rsx::simple_array<VkBufferCopy> copy;
				copy.reserve(transfer_height);

				u32 dst_offset = dma_mapping.first;
				u32 src_offset = result_offset;

				for (unsigned row = 0; row < transfer_height; ++row)
				{
					copy.push_back({ src_offset, dst_offset, transfer_pitch });
					src_offset += real_pitch;
					dst_offset += rsx_pitch;
				}

				vkCmdCopyBuffer(cmd, working_buffer->value, dma_mapping.second->value, transfer_height, copy.data());
			}
		}
		else
		{
#ifdef RPCS3_PS4
			// With dma.flag cpu_swap the rows can be narrower than the pitch: preload the mirror then, so the gaps
			// that flush_dma copies back to guest memory hold current data.
			dma_sync(ps4_cpu_swap && rsx_pitch != real_pitch);
#else
			dma_sync(false);
#endif

			VkBufferImageCopy region = {};
			region.bufferRowLength = (rsx_pitch / internal_bpp);
			region.imageSubresource = { src->aspect(), 0, 0, 1 };
			region.imageOffset = { src_area.x1, src_area.y1, 0 };
			region.imageExtent = { transfer_width, transfer_height, 1 };

			region.bufferOffset = dma_mapping.first;
			vkCmdCopyImageToBuffer(cmd, src->value, src->current_layout, dma_mapping.second->value, 1, &region);

#ifdef RPCS3_PS4
			if (ps4_cpu_swap)
			{
				// The mirror now holds host-order texels: imp_flush swaps these rows after copying them to guest memory.
				m_ps4_swap_elem = ps4_swap_elem;
				m_ps4_swap_base = valid_range.start;
				m_ps4_swap_rows = transfer_height;
				m_ps4_swap_row_bytes = transfer_width * internal_bpp;
				m_ps4_swap_pitch = rsx_pitch;
			}
#endif
		}

#ifdef RPCS3_PS4
		// Step 4: the data is in the DMA mirror.
		ps4_dma_trace(cmd, ps4_trace_seq, 4);
#endif

		// Post-transfer barrier on dma layer
		vk::insert_buffer_memory_barrier(
			cmd, dma_mapping.second->value,
			dma_mapping.first, dma_sync_region.length(),
			VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT
		);

		src->pop_layout(cmd);

		VkBufferMemoryBarrier2KHR mem_barrier =
		{
			.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2_KHR,
			.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT_KHR,      // Finish all transfer...
			.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT_KHR,
			.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT_KHR,  // ...before proceeding with any command
			.dstAccessMask = 0,
			.buffer = dma_mapping.second->value,
			.offset = dma_mapping.first,
			.size = valid_range.length()
		};

		// Create event object for this transfer and queue signal op
		dma_fence = std::make_unique<vk::event>(*m_device, sync_domain::host);
		dma_fence->signal(cmd,
		{
			.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
			.bufferMemoryBarrierCount = 1,
			.pBufferMemoryBarriers = &mem_barrier
		});

#ifdef RPCS3_PS4
		// Step 5: the completion event (label or VkEvent) has been signalled.
		ps4_dma_trace(cmd, ps4_trace_seq, 5);
#endif

		// Set cb flag for queued dma operations
		cmd.set_flag(vk::command_buffer::cb_has_dma_transfer);
#ifdef RPCS3_PS4
		m_ps4_dma_cb = &cmd;
		m_ps4_dma_gen = cmd.ps4_record_gen;
#endif

		if (get_context() == rsx::texture_upload_context::dma)
		{
			// Save readback hint in case transformation is required later
			switch (internal_bpp)
			{
			case 2:
				gcm_format = CELL_GCM_TEXTURE_R5G6B5;
				break;
			case 4:
			default:
				gcm_format = CELL_GCM_TEXTURE_A8R8G8B8;
				break;
			}
		}

		synchronized = true;
		sync_timestamp = rsx::get_shared_tag();
	}

	void texture_cache::on_section_destroyed(cached_texture_section& tex)
	{
		if (tex.is_managed() && tex.exists())
		{
			auto disposable = vk::disposable_t::make(new cached_image_reference_t(this, tex.get_texture()));
			vk::get_resource_manager()->dispose(disposable);
		}
	}

	void texture_cache::clear()
	{
		{
			std::lock_guard lock(m_cached_pool_lock);
			m_cache_is_exiting = true;
		}
		baseclass::clear();

		m_cached_images.clear();
		m_cached_memory_size = 0;
	}

	void texture_cache::copy_transfer_regions_impl(vk::command_buffer& cmd, vk::image* dst, const rsx::simple_array<copy_region_descriptor>& sections_to_transfer) const
	{
#ifdef RPCS3_PS4
		const utils::ps4::gpu::rp_close_cause_scope ps4_cause(rsx::ps4_rp_close_texture_copy);
		// 0.37: 'texture copies' in 'radv us/frame' (RSX thread only).
		const vk::ps4_radv_scope ps4_radv(vk::ps4_radv_texture_copy);
#endif
		const auto dst_aspect = dst->aspect();
		const auto dst_bpp = vk::get_format_texel_width(dst->format());

		std::unordered_set<decltype(sections_to_transfer.front().src)> processed_input_images;

		// Generates a region to write data to the final destination
		const auto get_output_region = [&](const copy_region_descriptor& section, s32 in_x, s32 in_y, u32 w, u32 h, vk::image* data_src)
		{
			VkImageCopy copy_rgn = {
				.srcSubresource = { data_src->aspect(), 0, 0, 1},
				.srcOffset = { in_x, in_y, 0 },
				.dstSubresource = { dst_aspect, section.level, 0, 1 },
				.dstOffset = { section.dst_x, section.dst_y, 0 },
				.extent = { w, h, 1 }
			};

			if (dst->info.imageType == VK_IMAGE_TYPE_3D)
			{
				copy_rgn.dstOffset.z = section.dst_z;
			}
			else
			{
				copy_rgn.dstSubresource.baseArrayLayer = section.dst_z;
			}

			return copy_rgn;
		};

		const auto configure_subresource = [&](const copy_region_descriptor& section, coord3i& coord, rsx::image_copy_subresource_layers& mip_layers)
		{
			mip_layers.dst_mip_level = section.level;
			coord.position = { section.dst_x, section.dst_y, 0 };
			coord.size = { section.dst_w, section.dst_h, 1 };

			if (dst->type() == VK_IMAGE_TYPE_3D)
			{
				coord.position.z = section.dst_z;
			}
			else
			{
				mip_layers.dst_layer = static_cast<u8>(section.dst_z);
			}
		};

		for (const auto& section : sections_to_transfer)
		{
			if (!section.src)
			{
				continue;
			}

			const bool typeless = section.src->aspect() != dst_aspect ||
				!formats_are_bitcast_compatible(dst, section.src);

			if (!processed_input_images.contains(section.src))
			{
				// Avoid inserting unnecessary barrier GENERAL->TRANSFER_SRC->GENERAL in active render targets
				const auto preferred_layout = (section.src->current_layout != VK_IMAGE_LAYOUT_GENERAL) ?
					VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;

				section.src->push_layout(cmd, preferred_layout);
				processed_input_images.insert(section.src);
			}

			auto src_image = section.src;
			auto src_x = section.src_x;
			auto src_y = section.src_y;
			auto src_w = section.src_w;
			auto src_h = section.src_h;

			rsx::flags32_t transform = section.xform;
			if (section.xform == rsx::surface_transform::coordinate_transform)
			{
				// Dimensions were given in 'dst' space. Work out the real source coordinates
				const auto src_bpp = vk::get_format_texel_width(section.src->format());
				src_x = (src_x * dst_bpp) / src_bpp;
				src_w = utils::aligned_div<u16>(src_w * dst_bpp, src_bpp);

				transform &= ~(rsx::surface_transform::coordinate_transform);
			}

			if (auto surface = dynamic_cast<vk::render_target*>(section.src))
			{
				surface->transform_samples_to_pixels(src_x, src_w, src_y, src_h);
			}

			if (typeless) [[unlikely]]
			{
				const auto src_bpp = vk::get_format_texel_width(section.src->format());
				const u16 convert_w = u16(src_w * src_bpp) / dst_bpp;
				const u16 convert_x = u16(src_x * src_bpp) / dst_bpp;

				if (convert_w == section.dst_w && src_h == section.dst_h &&
					transform == rsx::surface_transform::identity)
				{
					// Optimization to avoid double transfer
					coord3i dst_rect;
					rsx::image_copy_subresource_layers mip_layers;
					configure_subresource(section, dst_rect, mip_layers);

					const auto src_rect = coord3i{{ src_x, src_y, 0 }, { src_w, src_h, 1 }};
					vk::copy_image_typeless(cmd, section.src, dst, src_rect, dst_rect, mip_layers);
					continue;
				}

				src_image = vk::get_typeless_helper(dst->format(), dst->format_class(), convert_x + convert_w, src_y + src_h);
				src_image->change_layout(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

				const areai src_rect = coordi{{ src_x, src_y }, { src_w, src_h }};
				const areai dst_rect = coordi{{ 0, 0 }, { convert_w, src_h }};
				vk::copy_image_typeless(cmd, section.src, src_image, src_rect, dst_rect);
				src_image->change_layout(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

				src_x = 0;
				src_y = 0;
				src_w = convert_w;
			}

			ensure(src_image->current_layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL || src_image->current_layout == VK_IMAGE_LAYOUT_GENERAL);
			ensure(transform == rsx::surface_transform::identity);

			if (src_w == section.dst_w && src_h == section.dst_h) [[likely]]
			{
				const auto copy_rgn = get_output_region(section, src_x, src_y, src_w, src_h, src_image);
				vkCmdCopyImage(cmd, src_image->value, src_image->current_layout, dst->value, dst->current_layout, 1, &copy_rgn);
			}
			else
			{
				vk::image* _dst = dst;

				if (src_image->info.format != dst->info.format) [[ unlikely ]]
				{
					// Either a bitcast is required or a scale+copy to mipmap level / layer
					const u32 requested_width = dst->width();
					const u32 requested_height = src_y + src_h + section.dst_h; // Accounts for possible typeless ref on the same helper on src
					_dst = vk::get_typeless_helper(src_image->format(), src_image->format_class(), requested_width, requested_height);
					_dst->change_layout(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
				}

				auto dst_rect = coord3i{ { section.dst_x, section.dst_y, 0 }, { section.dst_w, section.dst_h, 1 } };
				rsx::image_copy_subresource_layers mip_layers{ .dst_mip_level = section.level };

				if (_dst != dst)
				{
					// We place the output after the source to account for the initial typeless-xfer if applicable
					// If src_image == _dst then this is just a write-to-self. Either way, use best-fit placement.
					dst_rect.position.x = 0;
					dst_rect.position.y = src_y + src_h;
					mip_layers = {};
				}
				else if (dst->type() == VK_IMAGE_TYPE_3D)
				{
					dst_rect.position.z = section.dst_z;
				}
				else
				{
					mip_layers.dst_layer = static_cast<u8>(section.dst_z);
				}

				vk::copy_scaled_image(cmd, src_image, _dst,
					coord3i{ { src_x, src_y, 0 }, { src_w, src_h, 1 } },
					dst_rect,
					mip_layers,
					src_image->format() == _dst->format(),
					VK_FILTER_NEAREST);

				if (_dst != dst) [[unlikely]]
				{
					// Casting comes after the scaling!
					const auto copy_rgn = get_output_region(section, dst_rect.position.x, dst_rect.position.y, section.dst_w, section.dst_h, _dst);
					_dst->change_layout(cmd, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
					vkCmdCopyImage(cmd, _dst->value, _dst->current_layout, dst->value, dst->current_layout, 1, &copy_rgn);
				}
			}
		}

		// Pop unique image layouts here
		for (auto& image : processed_input_images)
		{
			image->pop_layout(cmd);
		}
	}

	VkComponentMapping texture_cache::apply_component_mapping_flags(u32 gcm_format, rsx::component_order flags, const rsx::texture_channel_remap_t& remap_vector) const
	{
		switch (gcm_format)
		{
		case CELL_GCM_TEXTURE_DEPTH24_D8:
		case CELL_GCM_TEXTURE_DEPTH24_D8_FLOAT:
		case CELL_GCM_TEXTURE_DEPTH16:
		case CELL_GCM_TEXTURE_DEPTH16_FLOAT:
			// Dont bother letting this propagate
			return{ VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R };
		default:
			break;
		}

		VkComponentMapping mapping = {};
		switch (flags)
		{
		case rsx::component_order::default_:
		{
			mapping = vk::apply_swizzle_remap(vk::get_component_mapping(gcm_format), remap_vector);
			break;
		}
		case rsx::component_order::native:
		{
			mapping = { VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A };
			break;
		}
		case rsx::component_order::swapped_native:
		{
			mapping = { VK_COMPONENT_SWIZZLE_A, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B };
			break;
		}
		default:
			break;
		}

		return mapping;
	}

	vk::image* texture_cache::get_template_from_collection_impl(const rsx::simple_array<copy_region_descriptor>& sections_to_transfer) const
	{
		if (sections_to_transfer.size() == 1) [[likely]]
		{
			return sections_to_transfer.front().src;
		}

		vk::image* result = nullptr;
		for (const auto& section : sections_to_transfer)
		{
			if (!section.src)
				continue;

			if (!result)
			{
				result = section.src;
			}
			else
			{
				if (section.src->native_component_map.a != result->native_component_map.a ||
					section.src->native_component_map.r != result->native_component_map.r ||
					section.src->native_component_map.g != result->native_component_map.g ||
					section.src->native_component_map.b != result->native_component_map.b)
				{
					// TODO
					// This requires a far more complex setup as its not always possible to mix and match without compute assistance
					return nullptr;
				}
			}
		}

		return result;
	}

	std::unique_ptr<vk::viewable_image> texture_cache::find_cached_image(VkFormat format, u16 w, u16 h, u16 d, u16 mipmaps, VkImageType type, VkImageCreateFlags create_flags, VkImageUsageFlags usage, VkSharingMode sharing)
	{
		reader_lock lock(m_cached_pool_lock);

		if (!m_cached_images.empty())
		{
			const u64 desired_key = hash_image_properties(format, w, h, d, mipmaps, type, create_flags, sharing);
			lock.upgrade();

			for (auto it = m_cached_images.begin(); it != m_cached_images.end(); ++it)
			{
				if (it->key == desired_key && (it->data->info.usage & usage) == usage)
				{
					auto ret = std::move(it->data);
					m_cached_images.erase(it);
					m_cached_memory_size -= ret->memory->size();
					return ret;
				}
			}
		}

		return {};
	}

	std::unique_ptr<vk::viewable_image> texture_cache::create_temporary_subresource_storage(
		rsx::format_class format_class, VkFormat format,
		u16 width, u16 height, u16 depth, u16 layers, u8 mips,
		VkImageType image_type, VkFlags image_flags, VkFlags usage_flags)
	{
		auto image = find_cached_image(format, width, height, depth, mips, image_type, image_flags, usage_flags, VK_SHARING_MODE_EXCLUSIVE);

		if (!image)
		{
			image = std::make_unique<vk::viewable_image>(*vk::get_current_renderer(), m_memory_types.device_local, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
				image_type,
				format,
				width, height, depth, mips, layers, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
				VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, image_flags | VK_IMAGE_CREATE_ALLOW_NULL_RPCS3,
				VMM_ALLOCATION_POOL_TEXTURE_CACHE, format_class);

			if (!image->value)
			{
				// OOM, bail
				return nullptr;
			}
		}

		return image;
	}

	void texture_cache::dispose_reusable_image(std::unique_ptr<vk::viewable_image>& image)
	{
		auto disposable = vk::disposable_t::make(new cached_image_reference_t(this, image));
		vk::get_resource_manager()->dispose(disposable);
	}

	vk::image_view* texture_cache::create_temporary_subresource_view_impl(
		vk::command_buffer& cmd, vk::image* source, VkImageType image_type, VkImageViewType view_type,
		u32 gcm_format, u16 w, u16 h, u16 d, u8 mips,
		const rsx::texture_channel_remap_t& remap_vector,
		const copy_region_descriptor* copy)
	{
		const VkImageCreateFlags image_flags = (view_type == VK_IMAGE_VIEW_TYPE_CUBE) ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
		const VkImageUsageFlags usage_flags = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
		const VkFormat dst_format = vk::get_compatible_sampler_format(m_formats_support, gcm_format);
		const u16 layers = (view_type == VK_IMAGE_VIEW_TYPE_CUBE) ? 6 : 1;

		// Provision
		auto image = create_temporary_subresource_storage(rsx::classify_format(gcm_format), dst_format, w, h, d, layers, mips, image_type, image_flags, usage_flags);

		// OOM?
		if (!image)
		{
			return nullptr;
		}

		// This method is almost exclusively used to work on framebuffer resources
		// Keep the original swizzle layout unless there is data format conversion
		VkComponentMapping view_swizzle;
		if (!source || dst_format != source->info.format)
		{
			// This is a data cast operation
			// Use native mapping for the new type
			// TODO: Also simulate the readback+reupload step (very tricky)
			const auto remap = get_component_mapping(gcm_format);
			view_swizzle = { remap[1], remap[2], remap[3], remap[0] };
		}
		else
		{
			view_swizzle = source->native_component_map;
		}

		image->set_debug_name(fmt::format("Temp view, fmt=0x%x", gcm_format));
		image->set_native_component_layout(view_swizzle);
		auto view = image->get_view(remap_vector);

		if (copy)
		{
			rsx::simple_array<copy_region_descriptor> region = { *copy };
			vk::change_image_layout(cmd, image.get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
			copy_transfer_regions_impl(cmd, image.get(), region);
			vk::change_image_layout(cmd, image.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
		}

		// TODO: Floating reference. We can do better with some restructuring.
		image.release();
		return view;
	}

	vk::image_view* texture_cache::create_temporary_subresource_view(vk::command_buffer& cmd, const deferred_subresource& desc)
	{
		ensure(desc.sections_to_copy.size() == 1);
		const auto& section = desc.sections_to_copy.front();
		return create_temporary_subresource_view_impl(cmd, section.src, section.src->info.imageType, VK_IMAGE_VIEW_TYPE_2D,
			desc.gcm_format, desc.width, desc.height, 1, 1, desc.remap, &section);
	}

	vk::image_view* texture_cache::generate_cubemap_from_images(vk::command_buffer& cmd, const deferred_subresource& desc)
	{
		const auto& sections_to_copy = desc.sections_to_copy;
		auto _template = get_template_from_collection_impl(sections_to_copy);
		const u8 mip_count = desc.exact_mip_count();
		auto result = create_temporary_subresource_view_impl(cmd, _template, VK_IMAGE_TYPE_2D,
			VK_IMAGE_VIEW_TYPE_CUBE, desc.gcm_format, desc.width, desc.height, 1, mip_count, desc.remap);

		if (!result)
		{
			// Failed to create temporary object, bail
			return nullptr;
		}

		const auto image = result->image();
		VkImageAspectFlags dst_aspect = vk::get_aspect_flags(result->info.format);
		VkImageSubresourceRange dst_range = { dst_aspect, 0, mip_count, 0, 6 };
		vk::change_image_layout(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, dst_range);

		if (desc.force_bg_load)
		{
			// The memory load covers the whole image, no need to clear it first
			initialize_subresource_from_memory(cmd, image, desc, rsx::texture_dimension_extended::texture_dimension_cubemap);
		}
		else if (!(dst_aspect & VK_IMAGE_ASPECT_DEPTH_BIT))
		{
			VkClearColorValue clear = {};
			vkCmdClearColorImage(cmd, image->value, image->current_layout, &clear, 1, &dst_range);
		}
		else
		{
			VkClearDepthStencilValue clear = { 1.f, 0 };
			vkCmdClearDepthStencilImage(cmd, image->value, image->current_layout, &clear, 1, &dst_range);
		}

		vk::insert_image_memory_barrier(
			cmd,
			image->handle(),
			image->current_layout, image->current_layout,
			VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
			dst_range);

		copy_transfer_regions_impl(cmd, image, sections_to_copy);

		vk::change_image_layout(cmd, image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, dst_range);
		return result;
	}

	vk::image_view* texture_cache::generate_3d_from_2d_images(vk::command_buffer& cmd, const deferred_subresource& desc)
	{
		const auto& sections_to_copy = desc.sections_to_copy;
		auto _template = get_template_from_collection_impl(sections_to_copy);
		const u8 mip_count = desc.exact_mip_count();
		auto result = create_temporary_subresource_view_impl(cmd, _template, VK_IMAGE_TYPE_3D,
			VK_IMAGE_VIEW_TYPE_3D, desc.gcm_format, desc.width, desc.height, desc.depth, mip_count, desc.remap);

		if (!result)
		{
			// Failed to create temporary object, bail
			return nullptr;
		}

		const auto image = result->image();
		VkImageAspectFlags dst_aspect = vk::get_aspect_flags(result->info.format);
		VkImageSubresourceRange dst_range = { dst_aspect, 0, mip_count, 0, 1 };
		vk::change_image_layout(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, dst_range);

		if (desc.force_bg_load)
		{
			// The memory load covers the whole image, no need to clear it first
			initialize_subresource_from_memory(cmd, image, desc, rsx::texture_dimension_extended::texture_dimension_3d);
		}
		else if (!(dst_aspect & VK_IMAGE_ASPECT_DEPTH_BIT))
		{
			VkClearColorValue clear = {};
			vkCmdClearColorImage(cmd, image->value, image->current_layout, &clear, 1, &dst_range);
		}
		else
		{
			VkClearDepthStencilValue clear = { 1.f, 0 };
			vkCmdClearDepthStencilImage(cmd, image->value, image->current_layout, &clear, 1, &dst_range);
		}

		vk::insert_image_memory_barrier(
			cmd,
			image->handle(),
			image->current_layout, image->current_layout,
			VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
			dst_range);

		copy_transfer_regions_impl(cmd, image, sections_to_copy);

		vk::change_image_layout(cmd, image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, dst_range);
		return result;
	}

	vk::image_view* texture_cache::generate_atlas_from_images(vk::command_buffer& cmd, const deferred_subresource& desc)
	{
		const auto& sections_to_copy = desc.sections_to_copy;
		auto _template = get_template_from_collection_impl(sections_to_copy);
		auto result = create_temporary_subresource_view_impl(cmd, _template, VK_IMAGE_TYPE_2D,
			VK_IMAGE_VIEW_TYPE_2D, desc.gcm_format, desc.width, desc.height, 1, 1, desc.remap);

		if (!result)
		{
			// Failed to create temporary object, bail
			return nullptr;
		}

		const auto image = result->image();
		VkImageAspectFlags dst_aspect = vk::get_aspect_flags(result->info.format);
		VkImageSubresourceRange dst_range = { dst_aspect, 0, 1, 0, 1 };
		vk::change_image_layout(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, dst_range);

		if (desc.force_bg_load)
		{
			// The memory load covers the whole image, no need to clear it first
			initialize_subresource_from_memory(cmd, image, desc, rsx::texture_dimension_extended::texture_dimension_2d);
		}
		else if (sections_to_copy[0].dst_w != desc.width || sections_to_copy[0].dst_h != desc.height)
		{
			if (!(dst_aspect & VK_IMAGE_ASPECT_DEPTH_BIT))
			{
				VkClearColorValue clear = {};
				vkCmdClearColorImage(cmd, image->value, image->current_layout, &clear, 1, &dst_range);
			}
			else
			{
				VkClearDepthStencilValue clear = { 1.f, 0 };
				vkCmdClearDepthStencilImage(cmd, image->value, image->current_layout, &clear, 1, &dst_range);
			}
		}

		vk::insert_image_memory_barrier(
			cmd,
			image->handle(),
			image->current_layout, image->current_layout,
			VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
			dst_range);

		copy_transfer_regions_impl(cmd, image, sections_to_copy);

		vk::change_image_layout(cmd, image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, dst_range);
		return result;
	}

	vk::image_view* texture_cache::generate_2d_mipmaps_from_images(vk::command_buffer& cmd, const deferred_subresource& desc)
	{
		const auto& sections_to_copy = desc.sections_to_copy;
		const auto mipmaps = ::narrow<u8>(sections_to_copy.size());
		auto _template = get_template_from_collection_impl(sections_to_copy);
		auto result = create_temporary_subresource_view_impl(cmd, _template, VK_IMAGE_TYPE_2D,
			VK_IMAGE_VIEW_TYPE_2D, desc.gcm_format, desc.width, desc.height, 1, mipmaps, desc.remap);

		if (!result)
		{
			// Failed to create temporary object, bail
			return nullptr;
		}

		const auto image = result->image();
		VkImageAspectFlags dst_aspect = vk::get_aspect_flags(result->info.format);
		VkImageSubresourceRange dst_range = { dst_aspect, 0, mipmaps, 0, 1 };
		vk::change_image_layout(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, dst_range);

		if (desc.force_bg_load)
		{
			// The memory load covers the whole image, no need to clear it first
			initialize_subresource_from_memory(cmd, image, desc, rsx::texture_dimension_extended::texture_dimension_2d);
		}
		else if (!(dst_aspect & VK_IMAGE_ASPECT_DEPTH_BIT))
		{
			VkClearColorValue clear = {};
			vkCmdClearColorImage(cmd, image->value, image->current_layout, &clear, 1, &dst_range);
		}
		else
		{
			VkClearDepthStencilValue clear = { 1.f, 0 };
			vkCmdClearDepthStencilImage(cmd, image->value, image->current_layout, &clear, 1, &dst_range);
		}

		vk::insert_image_memory_barrier(
			cmd,
			image->handle(),
			image->current_layout, image->current_layout,
			VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
			dst_range);

		copy_transfer_regions_impl(cmd, image, sections_to_copy);

		vk::change_image_layout(cmd, image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, dst_range);
		return result;
	}

	void texture_cache::release_temporary_subresource(vk::image_view* view)
	{
		auto resource = dynamic_cast<vk::viewable_image*>(view->image());
		ensure(resource);

		auto image = std::unique_ptr<vk::viewable_image>(resource);
		auto disposable = vk::disposable_t::make(new cached_image_reference_t(this, image));
		vk::get_resource_manager()->dispose(disposable);
	}

	void texture_cache::initialize_subresource_from_memory(vk::command_buffer& cmd, vk::image* dst, const deferred_subresource& desc, rsx::texture_dimension_extended type) const
	{
		const auto subresources_layout = rsx::get_subresources_layout(desc, type);
		const u16 layer_count = (type == rsx::texture_dimension_extended::texture_dimension_cubemap) ? 6 : 1;
		vk::upload_image(cmd, dst, subresources_layout, desc.gcm_format, desc.swizzled, layer_count,
			dst->aspect(), *vk::get_upload_heap(), desc.pitch, vk::upload_contents_inline);
	}

	void texture_cache::update_image_contents(vk::command_buffer& cmd, vk::image_view* dst_view, const deferred_subresource& desc)
	{
		auto dst = dst_view->image();
		dst->push_layout(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
		copy_transfer_regions_impl(cmd, dst, desc.sections_to_copy);
		dst->pop_layout(cmd);
	}

	cached_texture_section* texture_cache::create_new_texture(vk::command_buffer& cmd, const utils::address_range32& rsx_range, u16 width, u16 height, u16 depth, u16 mipmaps, u32 pitch,
		u32 gcm_format, rsx::texture_upload_context context, rsx::texture_dimension_extended type, bool swizzled, rsx::component_order swizzle_flags, rsx::flags32_t flags)
	{
		const auto section_depth = depth;

		// Define desirable attributes based on type
		VkImageType image_type;
		VkImageUsageFlags usage_flags = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
		u8 layer = 0;

		switch (type)
		{
		case rsx::texture_dimension_extended::texture_dimension_1d:
			image_type = VK_IMAGE_TYPE_1D;
			height = 1;
			depth = 1;
			layer = 1;
			break;
		case rsx::texture_dimension_extended::texture_dimension_2d:
			image_type = VK_IMAGE_TYPE_2D;
			depth = 1;
			layer = 1;
			break;
		case rsx::texture_dimension_extended::texture_dimension_cubemap:
			image_type = VK_IMAGE_TYPE_2D;
			depth = 1;
			layer = 6;
			break;
		case rsx::texture_dimension_extended::texture_dimension_3d:
			image_type = VK_IMAGE_TYPE_3D;
			layer = 1;
			break;
		default:
			fmt::throw_exception("Unreachable");
		}

		// Check what actually exists at that address
		const rsx::image_section_attributes_t search_desc = { .gcm_format = gcm_format, .width = width, .height = height, .depth = section_depth, .mipmaps = mipmaps };
		const bool allow_dirty = (context != rsx::texture_upload_context::framebuffer_storage);
		cached_texture_section& region = *find_cached_texture(rsx_range, search_desc, true, true, allow_dirty);
		ensure(!region.is_locked());

		vk::viewable_image* image = nullptr;
		if (region.exists())
		{
			image = dynamic_cast<vk::viewable_image*>(region.get_raw_texture());
			bool reusable = true;

			if (flags & texture_create_flags::do_not_reuse)
			{
				reusable = false;
			}
			else if (flags & texture_create_flags::shareable)
			{
				reusable = (image && image->sharing_mode() == VK_SHARING_MODE_CONCURRENT);
			}

			if (!reusable || !image || region.get_image_type() != type || image->depth() != depth) // TODO
			{
				// Incompatible view/type
				region.destroy();
				image = nullptr;
			}
			else
			{
				ensure(region.is_managed());

				// Reuse
				region.set_rsx_pitch(pitch);

				if (flags & texture_create_flags::initialize_image_contents)
				{
					// Wipe memory
					image->change_layout(cmd, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

					VkImageSubresourceRange range{ image->aspect(), 0, image->mipmaps(), 0, image->layers() };
					if (image->aspect() & VK_IMAGE_ASPECT_COLOR_BIT)
					{
						VkClearColorValue color = { {0.f, 0.f, 0.f, 1.f} };
						vkCmdClearColorImage(cmd, image->value, image->current_layout, &color, 1, &range);
					}
					else
					{
						VkClearDepthStencilValue clear{ 1.f, 255 };
						vkCmdClearDepthStencilImage(cmd, image->value, image->current_layout, &clear, 1, &range);
					}

					vk::insert_image_memory_barrier(
						cmd,
						image->handle(),
						image->current_layout, image->current_layout,
						VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
						VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
						range);
				}
			}
		}

		if (!image)
		{
			const bool is_cubemap = type == rsx::texture_dimension_extended::texture_dimension_cubemap;
			const VkFormat vk_format = get_compatible_sampler_format(m_formats_support, gcm_format);
			VkImageCreateFlags create_flags = is_cubemap ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
			VkSharingMode sharing_mode = (flags & texture_create_flags::shareable) ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE;
			rsx::simple_array<VkFormat> mutable_format_list;

			if (flags & texture_create_flags::mutable_format)
			{
				const VkFormat snorm_fmt = get_compatible_snorm_format(vk_format);
				const VkFormat srgb_fmt = get_compatible_srgb_format(vk_format);

				if (snorm_fmt != VK_FORMAT_UNDEFINED)
				{
					mutable_format_list.push_back(snorm_fmt);
				}

				if (srgb_fmt != VK_FORMAT_UNDEFINED)
				{
					mutable_format_list.push_back(srgb_fmt);
				}

				if (!mutable_format_list.empty())
				{
					create_flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
					mutable_format_list.push_back(vk_format);
				}
			}

			if (auto found = find_cached_image(vk_format, width, height, depth, mipmaps, image_type, create_flags, usage_flags, sharing_mode))
			{
				image = found.release();
			}
			else
			{
				if (sharing_mode == VK_SHARING_MODE_CONCURRENT)
				{
					create_flags |= VK_IMAGE_CREATE_SHAREABLE_RPCS3;
				}

				VkFormatEx create_format = vk_format;
				if (!mutable_format_list.empty())
				{
					create_format.pViewFormats = mutable_format_list.data();
					create_format.viewFormatCount = mutable_format_list.size();
				}

				image = new vk::viewable_image(*m_device,
					m_memory_types.device_local, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
					image_type, create_format,
					width, height, depth, mipmaps, layer, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
					VK_IMAGE_TILING_OPTIMAL, usage_flags, create_flags,
					VMM_ALLOCATION_POOL_TEXTURE_CACHE, rsx::classify_format(gcm_format));
			}

			// New section, we must prepare it
			region.reset(rsx_range);
			region.set_gcm_format(gcm_format);
			region.set_image_type(type);
			region.create(width, height, section_depth, mipmaps, image, pitch, true, gcm_format);
		}

		region.set_view_flags(swizzle_flags);
		region.set_context(context);
		region.set_swizzled(swizzled);
		region.set_dirty(false);

		image->native_component_map = apply_component_mapping_flags(gcm_format, swizzle_flags, rsx::default_remap_vector);

		// Its not necessary to lock blit dst textures as they are just reused as necessary
		switch (context)
		{
		case rsx::texture_upload_context::shader_read:
		case rsx::texture_upload_context::blit_engine_src:
			region.protect(utils::protection::ro);
			read_only_range = region.get_min_max(read_only_range, rsx::section_bounds::locked_range);
			break;
		case rsx::texture_upload_context::blit_engine_dst:
			region.set_unpack_swap_bytes(true);
			no_access_range = region.get_min_max(no_access_range, rsx::section_bounds::locked_range);
			break;
		case rsx::texture_upload_context::dma:
		case rsx::texture_upload_context::framebuffer_storage:
			// Should not be initialized with this method
		default:
			fmt::throw_exception("Unexpected upload context 0x%x", u32(context));
		}

		update_cache_tag();
		return &region;
	}

	cached_texture_section* texture_cache::create_nul_section(
		vk::command_buffer& /*cmd*/,
		const utils::address_range32& rsx_range,
		const rsx::image_section_attributes_t& attrs,
		const rsx::GCM_tile_reference& tile,
		bool memory_load)
	{
		auto& region = *find_cached_texture(rsx_range, { .gcm_format = RSX_GCM_FORMAT_IGNORED }, true, false, false);
		ensure(!region.is_locked());

		// Prepare section
		region.reset(rsx_range);
		region.create_dma_only(attrs.width, attrs.height, attrs.pitch);
		region.set_dirty(false);
		region.set_unpack_swap_bytes(true);

		if (memory_load && !tile) // Memory load on DMA tiles will always happen during the actual copy command
		{
			vk::map_dma(rsx_range.start, rsx_range.length());
			vk::load_dma(rsx_range.start, rsx_range.length());
		}

		no_access_range = region.get_min_max(no_access_range, rsx::section_bounds::locked_range);
		update_cache_tag();
		return &region;
	}

	cached_texture_section* texture_cache::upload_image_from_cpu(vk::command_buffer& cmd, const utils::address_range32& rsx_range, u16 width, u16 height, u16 depth, u16 mipmaps, u32 pitch, u32 gcm_format,
		rsx::texture_upload_context context, const std::vector<rsx::subresource_layout>& subresource_layout, rsx::texture_dimension_extended type, bool swizzled)
	{
#ifdef RPCS3_PS4
		// 0.36: texture data uploaded from CPU memory, per frame ('tex uploads ... misses' in the 10 s line); render
		// passes closed by the upload count as the texture cache's.
		rsx::g_ps4_rsx_counters.tex_upload_misses++;
		const utils::ps4::gpu::rp_close_cause_scope ps4_cause(rsx::ps4_rp_close_texture_cache);
#endif
		if (context != rsx::texture_upload_context::shader_read)
		{
			if (vk::is_renderpass_open(cmd))
			{
				vk::end_renderpass(cmd);
			}
		}

		const bool upload_async = rsx::get_current_renderer()->get_backend_config().supports_asynchronous_compute;
		rsx::flags32_t create_flags = 0;

		if (context == rsx::texture_upload_context::shader_read &&
			!g_cfg.video.disable_hardware_texel_remapping)
		{
			create_flags |= texture_create_flags::mutable_format;
		}

		if (upload_async && ensure(g_fxo->try_get<AsyncTaskScheduler>())->is_host_mode())
		{
			create_flags |= texture_create_flags::do_not_reuse;
			if (m_device->get_graphics_queue() != m_device->get_transfer_queue())
			{
				create_flags |= texture_create_flags::shareable;
			}
		}

		auto section = create_new_texture(cmd, rsx_range, width, height, depth, mipmaps, pitch, gcm_format, context, type, swizzled,
			rsx::component_order::default_, create_flags);

		auto image = section->get_raw_texture();
		image->set_debug_name(fmt::format("Raw Texture @0x%x", rsx_range.start));

		vk::enter_uninterruptible();

		bool input_swizzled = swizzled;
		if (context == rsx::texture_upload_context::blit_engine_src)
		{
			// Swizzling is ignored for blit engine copy and emulated using remapping
			input_swizzled = false;
		}

		rsx::flags32_t upload_command_flags = initialize_image_layout | upload_contents_inline;
		if (context == rsx::texture_upload_context::shader_read && upload_async)
		{
			upload_command_flags |= upload_contents_async;
		}

		std::vector<rsx::subresource_layout> tmp;
		auto p_subresource_layout = &subresource_layout;
		u32 heap_align = upload_heap_align_default;

		if (auto tiled_region = rsx::get_current_renderer()->get_tiled_memory_region(rsx_range);
			context == rsx::texture_upload_context::blit_engine_src && tiled_region)
		{
			if (mipmaps > 1)
			{
				// This really shouldn't happen on framebuffer tiled memory
				rsx_log.error("Tiled decode of mipmapped textures is not supported.");
			}
			else
			{
				const auto bpp = rsx::get_format_block_size_in_bytes(gcm_format);
				const auto [scratch_buf, linear_data_scratch_offset] = vk::detile_memory_block(cmd, tiled_region, rsx_range, width, height, bpp);

				auto subres = subresource_layout.front();
				// FIXME: !!EVIL!!
				subres.data = { scratch_buf, linear_data_scratch_offset };
				subres.pitch_in_block = width;
				upload_command_flags |= source_is_gpu_resident;
				heap_align = width * bpp;

				tmp.push_back(std::move(subres));
				p_subresource_layout = &tmp;
			}
		}

		const u16 layer_count = (type == rsx::texture_dimension_extended::texture_dimension_cubemap) ? 6 : 1;
		vk::upload_image(cmd, image, *p_subresource_layout, gcm_format, input_swizzled, layer_count, image->aspect(),
			*m_texture_upload_heap, heap_align, upload_command_flags);

		vk::leave_uninterruptible();

		if (context != rsx::texture_upload_context::shader_read)
		{
			// Insert appropriate barrier depending on use. Shader read resources should be lazy-initialized before consuming.
			// TODO: All texture resources should be initialized on use, this is wasteful

			VkImageLayout preferred_layout;
			switch (context)
			{
			default:
				preferred_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
				break;
			case rsx::texture_upload_context::blit_engine_dst:
				preferred_layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
				break;
			case rsx::texture_upload_context::blit_engine_src:
				preferred_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
				break;
			}

			if (preferred_layout != image->current_layout)
			{
				image->change_layout(cmd, preferred_layout);
			}
			else
			{
				// Insert ordering barrier
				ensure(preferred_layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
				insert_image_memory_barrier(cmd, image->value, image->current_layout, preferred_layout,
					VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
					VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
					{ image->aspect(), 0, image->mipmaps(), 0, image->layers() });
			}
		}

		section->last_write_tag = rsx::get_shared_tag();
		return section;
	}

	void texture_cache::set_component_order(cached_texture_section& section, u32 gcm_format, rsx::component_order expected_flags)
	{
		if (expected_flags == section.get_view_flags())
			return;

		const VkComponentMapping mapping = apply_component_mapping_flags(gcm_format, expected_flags, rsx::default_remap_vector);
		auto image = static_cast<vk::viewable_image*>(section.get_raw_texture());

		ensure(image);
		image->set_native_component_layout(mapping);

		section.set_view_flags(expected_flags);
	}

	void texture_cache::insert_texture_barrier(vk::command_buffer& cmd, vk::image* tex, bool strong_ordering)
	{
		if (!strong_ordering && tex->current_layout == VK_IMAGE_LAYOUT_GENERAL)
		{
			// A previous barrier already exists, do nothing
			return;
		}

		vk::as_rtt(tex)->texture_barrier(cmd);
	}

	bool texture_cache::render_target_format_is_compatible(vk::image* tex, u32 gcm_format)
	{
		auto vk_format = tex->info.format;
		switch (gcm_format)
		{
		default:
			//TODO
			warn_once("Format incompatibility detected, reporting failure to force data copy (VK_FORMAT=0x%X, GCM_FORMAT=0x%X)", static_cast<u32>(vk_format), gcm_format);
			return false;
#if !defined(__APPLE__) || !defined(ARCH_X64)
		case CELL_GCM_TEXTURE_R5G6B5:
			return (vk_format == VK_FORMAT_R5G6B5_UNORM_PACK16);
#else
		// R5G6B5 is not supported by Metal on non-Apple GPUs
		case CELL_GCM_TEXTURE_R5G6B5:
			return (vk_format == VK_FORMAT_B8G8R8A8_UNORM);
#endif
		case CELL_GCM_TEXTURE_W16_Z16_Y16_X16_FLOAT:
			return (vk_format == VK_FORMAT_R16G16B16A16_SFLOAT);
		case CELL_GCM_TEXTURE_W32_Z32_Y32_X32_FLOAT:
			return (vk_format == VK_FORMAT_R32G32B32A32_SFLOAT);
		case CELL_GCM_TEXTURE_X32_FLOAT:
			return (vk_format == VK_FORMAT_R32_SFLOAT);
		case CELL_GCM_TEXTURE_A8R8G8B8:
		case CELL_GCM_TEXTURE_D8R8G8B8:
			return (vk_format == VK_FORMAT_B8G8R8A8_UNORM || vk_format == VK_FORMAT_D24_UNORM_S8_UINT || vk_format == VK_FORMAT_D32_SFLOAT_S8_UINT);
		case CELL_GCM_TEXTURE_B8:
			return (vk_format == VK_FORMAT_R8_UNORM);
		case CELL_GCM_TEXTURE_G8B8:
			return (vk_format == VK_FORMAT_R8G8_UNORM);
		case CELL_GCM_TEXTURE_DEPTH24_D8:
		case CELL_GCM_TEXTURE_DEPTH24_D8_FLOAT:
			return (vk_format == VK_FORMAT_D24_UNORM_S8_UINT || vk_format == VK_FORMAT_D32_SFLOAT_S8_UINT);
		case CELL_GCM_TEXTURE_X16:
		case CELL_GCM_TEXTURE_DEPTH16:
		case CELL_GCM_TEXTURE_DEPTH16_FLOAT:
			return (vk_format == VK_FORMAT_D16_UNORM || vk_format == VK_FORMAT_D32_SFLOAT);
		}
	}

#ifdef RPCS3_PS4
	thread_local bool g_ps4_flush_queue_drained = false;
	thread_local ps4_readback_tls_t g_ps4_readback_tls{};

	void ps4_note_invisible_dma_event()
	{
		// 0.29: exact count, read and reset by the 10 s 'RSX gameplay' line ('fence-covered'),
		// instead of logging only the 1st/101st/10001st event. RPCS3.log still marks the first one.
		rsx::g_ps4_rsx_counters.invisible_dma++;

		static atomic_t<bool> s_reported{false};
		if (!s_reported.observe() && !s_reported.exchange(true))
		{
			// 0.32: only reached when a fence wait really covered the copy (failed drains no longer count).
			rsx_log.warning("PS4: DMA completion event (%s backend) still unset after a flush that a completed fence covered; "
				"continuing without the event wait (counted per 10 s in startup.log 'RSX gameplay ... fence-covered=')",
				ps4_dma_events_use_labels() ? "gpu_label" : "VkEvent");
		}
	}

	bool texture_cache::ps4_drain_queue(vk::command_buffer& cmd)
	{
		// An empty batch through the existing DMA path: prepare + cleanup submit it with the buffer's fence and wait.
		g_ps4_last_dma_submit_done = true;
		prepare_for_dma_transfers(cmd);
		cleanup_after_dma_transfers(cmd);
		return g_ps4_last_dma_submit_done;
	}

	u64 texture_cache::ps4_evict_by_age(const std::set<u32>& keep, u64 now_frame, u64 min_age, u64 target)
	{
		// The types evict_unused takes; render targets and DMA sections never.
		const u32 type_to_evict = rsx::texture_upload_context::shader_read | rsx::texture_upload_context::blit_engine_src;
		std::vector<utils::ps4::vram::evict_candidate> candidates;

		{
			// This thread may already hold the cache (an allocation inside it): then nothing is evicted.
			if (!m_cache_mutex.try_lock_shared())
			{
				return 0;
			}

			for (auto It = m_storage.begin(); It != m_storage.end(); ++It)
			{
				for (auto& region : *It)
				{
					if (region.is_dirty() || !(region.get_context() & type_to_evict) || keep.contains(region.get_section_base()))
					{
						continue;
					}

					candidates.push_back({ region.ps4_last_use(), region.ps4_vram_bytes(), region.get_section_base(), false });
				}
			}

			m_cache_mutex.unlock_shared();
		}

		const u64 marked = utils::ps4::vram::plan_eviction(candidates.data(), candidates.size(), now_frame, min_age, target);
		if (!marked)
		{
			return 0;
		}

		// evict_unused takes every unprotected candidate whose address is not excluded: exclude the bound textures and
		// every candidate the plan keeps (a kept section protects the others at its address too).
		std::set<u32> exclusion_list = keep;
		for (const auto& c : candidates)
		{
			if (!c.evict)
			{
				exclusion_list.insert(c.base);
			}
		}

		return evict_unused(exclusion_list) ? marked : 0;
	}

	u64 texture_cache::ps4_drop_image_pool()
	{
		std::lock_guard lock(m_cached_pool_lock);

		const u64 freed = m_cached_memory_size;
		m_cached_images.clear();
		m_cached_memory_size = 0;
		return freed;
	}

	bool texture_cache::ps4_dma_range_pinned(const utils::address_range32& range)
	{
		if (!m_cache_mutex.try_lock_shared())
		{
			return true;
		}

		bool pinned = false;
		for (auto It = m_storage.range_begin(range, rsx::section_bounds::full_range); It != m_storage.range_end(); ++It)
		{
			auto& tex = *It;
			if (!tex.is_dirty() &&
				(tex.get_context() == rsx::texture_upload_context::dma || (tex.is_synchronized() && !tex.is_flushed())))
			{
				pinned = true;
				break;
			}
		}

		m_cache_mutex.unlock_shared();
		return pinned;
	}

	// The upstream quota of is_overallocated() for the device-local size (the allocator cap on PS4), in bytes.
	static u64 ps4_upstream_texture_quota(u64 total_device_memory_mib)
	{
		u64 quota = 0;
		if (total_device_memory_mib >= 2048)
		{
			quota = std::min<u64>(3072, (total_device_memory_mib * 40) / 100);
		}
		else if (total_device_memory_mib >= 1024)
		{
			quota = std::max<u64>(204, (total_device_memory_mib * 30) / 100);
		}
		else if (total_device_memory_mib >= 768)
		{
			quota = 192;
		}
		else
		{
			quota = std::min<u64>(128, total_device_memory_mib / 2);
		}

		return quota * 0x100000;
	}

	bool texture_cache::ps4_over_quota() const
	{
		const u64 upstream = ps4_upstream_texture_quota(m_device->get_memory_mapping().device_local_total_bytes / 0x100000);
		const u64 quota = utils::ps4::vram::texture_quota(upstream, u64{ vk::ps4_window_budget_mib() } * 0x100000);
		return vmm_get_application_pool_usage(VMM_ALLOCATION_POOL_TEXTURE_CACHE) > quota;
	}

	// Waits (2 s at most) for a DMA submission made with the buffer's own fence. True when it completed; otherwise the
	// buffer stays pending and the failure is reported. 'gen' is the recording that was submitted.
	static bool ps4_finish_dma_submit(vk::command_buffer& cmd, u64 gen, bool primary)
	{
		// A DMA submission normally completes within a frame. When it is still running after 250 ms, record where the
		// readback trace stands (fsync'd) before waiting on: a GPU fault can get the process killed before the 2 s
		// report below is written. The two waits add up to the same 2 s.
		constexpr u64 snapshot_after_us = 250'000;
		VkResult result = cmd.ps4_wait_pending(snapshot_after_us);
		if (result == VK_TIMEOUT)
		{
			ps4_dma_trace_snapshot(primary ? "primary DMA submit still running after 250 ms" : "auxiliary DMA submit still running after 250 ms");
			result = cmd.ps4_wait_pending(GENERAL_WAIT_TIMEOUT - snapshot_after_us);
		}

		g_ps4_last_dma_submit_done = (result == VK_SUCCESS);

		if (result == VK_SUCCESS)
		{
			ps4_dma_trace_check_visibility(cmd, gen);
			return true;
		}

		// Whatever this submission covered is not known to be complete.
		g_ps4_flush_queue_drained = false;

		rsx_log.error("PS4: DMA flush submit not finished after up to 2 s (%s command buffer, VkResult %d); the buffer stays pending",
			primary ? "primary" : "auxiliary", static_cast<s32>(result));

		if (result == VK_ERROR_DEVICE_LOST)
		{
			// Also writes the trace report.
			vk::ps4_record_device_lost("DMA flush fence");
		}
		else
		{
			ps4_dma_trace_report(primary ? "primary DMA submit not finished" : "auxiliary DMA submit not finished");
		}

		return false;
	}
#endif

	void texture_cache::prepare_for_dma_transfers(vk::command_buffer& cmd)
	{
#ifdef RPCS3_PS4
		if (cmd.access_hint == vk::command_buffer::access_type_hint::flush_only && vk::ps4_dma_isolate())
		{
			// 0.32: an RSX-side readback used to be appended to the frame's draws and submitted with them (the shape of
			// the 0.31 GPU hang). Submit the frame first so the readback is recorded and submitted alone, the shape
			// readbacks had when they ran fine on this console. dma.flag isolate=off restores the old behaviour.
			static_cast<VKGSRender*>(rsx::get_current_renderer())->ps4_submit_frame_before_dma(cmd);
		}
#endif

		if (!cmd.is_recording())
		{
			cmd.begin();
		}
	}

	void texture_cache::cleanup_after_dma_transfers(vk::command_buffer& cmd)
	{
		bool occlusion_query_active = !!(cmd.flags & vk::command_buffer::cb_has_open_query);
#ifdef RPCS3_PS4
		// 0.32: a pending query restart (e.g. requested when the frame was submitted ahead of an isolated readback)
		// must survive the submit below, which clears the flags.
		const bool ps4_reload_query = !!(cmd.flags & vk::command_buffer::cb_load_occluson_task);
#endif
		if (occlusion_query_active)
		{
			// We really stepped in it
			vk::do_query_cleanup(cmd);
		}

		// End recording
		cmd.end();

		if (cmd.access_hint != vk::command_buffer::access_type_hint::all)
		{
			// Flush any pending async jobs in case of blockers
			// TODO: Context-level manager should handle this logic
			auto async_scheduler = g_fxo->try_get<AsyncTaskScheduler>();
			vk::semaphore* async_sema = nullptr;

			if (async_scheduler && async_scheduler->is_recording())
			{
				if (async_scheduler->is_host_mode())
				{
					async_sema = async_scheduler->get_sema();
				}
				else
				{
					vk::queue_submit_t submit_info{};
					async_scheduler->flush(submit_info, VK_TRUE);
				}
			}

			// Primary access command queue, must restart it after
#ifdef RPCS3_PS4
			// 0.32: the buffer's own fence instead of a local one. On a timeout the buffer then stays pending (and
			// its fence alive), so it is neither reset nor re-recorded while the GPU may still execute it.
			vk::queue_submit_t submit_info{ m_submit_queue, nullptr };
#else
			vk::fence submit_fence(*m_device);
			vk::queue_submit_t submit_info{ m_submit_queue, &submit_fence };
#endif

			if (async_sema)
			{
				vk::queue_submit_t submit_info2{};
				submit_info2.queue_signal(*async_sema);
				async_scheduler->flush(submit_info2, VK_TRUE);

				submit_info.wait_on(*async_sema, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
			}

#ifdef RPCS3_PS4
			const u64 ps4_gen = cmd.ps4_record_gen;
			cmd.submit(submit_info, VK_TRUE);

			if (ps4_finish_dma_submit(cmd, ps4_gen, true))
			{
				cmd.reset();
			}

			// After a failed wait the buffer is still pending: begin() waits for it (untimed, hang watchdog after
			// 10 s) instead of resetting a command buffer the GPU may still execute.
#else
			cmd.submit(submit_info, VK_TRUE);
			vk::wait_for_fence(&submit_fence, GENERAL_WAIT_TIMEOUT);

			cmd.reset();
#endif
			cmd.begin();
		}
		else
		{
			// Auxilliary command queue with auto-restart capability
#ifdef RPCS3_PS4
			// 0.28: RADV on the console never makes the DMA vk::event visible to the host (0.27 Minecraft:
			// 75 "vk::wait_for_event has timed out!", 2 s each = 0.3 FPS in the world). Fences do work, so
			// the auxiliary submission waits on a fence exactly like the primary branch above. The fence's
			// first synchronization scope also covers everything submitted to this queue before.
			// 0.32: the buffer's own fence, so a timed-out submission stays pending (see the primary branch).
			vk::queue_submit_t submit_info{ m_submit_queue, nullptr };
			const u64 ps4_gen = cmd.ps4_record_gen;
			cmd.submit(submit_info, VK_TRUE);
			ps4_finish_dma_submit(cmd, ps4_gen, false);
#else
			vk::queue_submit_t submit_info{ m_submit_queue, nullptr };
			cmd.submit(submit_info, VK_TRUE);
#endif
		}

		ensure(cmd.flags == 0);

		if (occlusion_query_active)
		{
			ensure(cmd.is_recording());
			cmd.flags |= vk::command_buffer::cb_load_occluson_task;
		}
#ifdef RPCS3_PS4
		else if (ps4_reload_query && cmd.is_recording())
		{
			cmd.flags |= vk::command_buffer::cb_load_occluson_task;
		}

		// 0.37: end of the GPU part of a readback (its submit was waited above), for the access-violation timing.
		g_ps4_readback_tls.submit_done_us = get_system_time();
#endif
	}

	void texture_cache::initialize(vk::render_device& device, VkQueue submit_queue, vk::data_heap& upload_heap)
	{
		m_device = &device;
		m_memory_types = device.get_memory_mapping();
		m_formats_support = device.get_formats_support();
		m_submit_queue = submit_queue;
		m_texture_upload_heap = &upload_heap;
	}

	void texture_cache::destroy()
	{
		clear();

#ifdef RPCS3_PS4
		ps4_dma_trace_release();
#endif
	}

	bool texture_cache::is_depth_texture(u32 rsx_address, u32 rsx_size)
	{
		reader_lock lock(m_cache_mutex);

		auto& block = m_storage.block_for(rsx_address);

		if (block.get_locked_count() == 0)
			return false;

		for (auto& tex : block)
		{
			if (tex.is_dirty())
				continue;

			if (!tex.overlaps(rsx_address, rsx::section_bounds::full_range))
				continue;

			if ((rsx_address + rsx_size - tex.get_section_base()) <= tex.get_section_size())
			{
				switch (tex.get_format())
				{
				case VK_FORMAT_D16_UNORM:
				case VK_FORMAT_D32_SFLOAT:
				case VK_FORMAT_D32_SFLOAT_S8_UINT:
				case VK_FORMAT_D24_UNORM_S8_UINT:
					return true;
				default:
					return false;
				}
			}
		}

		//Unreachable; silence compiler warning anyway
		return false;
	}

	bool texture_cache::handle_memory_pressure(rsx::problem_severity severity)
	{
		auto any_released = baseclass::handle_memory_pressure(severity);

		// TODO: This can cause invalidation of in-flight resources
		if (severity <= rsx::problem_severity::low || !m_cached_memory_size)
		{
			// Nothing left to do
			return any_released;
		}

		constexpr u64 _1M = 0x100000;
		if (severity <= rsx::problem_severity::moderate && m_cached_memory_size < (64 * _1M))
		{
			// Some memory is consumed by the temporary resources, but no need to panic just yet
			return any_released;
		}

		std::unique_lock lock(m_cache_mutex, std::defer_lock);
		if (!lock.try_lock())
		{
			rsx_log.warning("Unable to remove temporary resources because we're already in the texture cache!");
			return any_released;
		}

		// Nuke temporary resources. They will still be visible to the GPU.
		auto gc = vk::get_resource_manager();
		any_released |= !m_cached_images.empty();
		for (auto& img : m_cached_images)
		{
			gc->dispose(img.data);
		}
		m_cached_images.clear();
		m_cached_memory_size = 0;

		any_released |= !m_temporary_subresource_cache.empty();
		for (auto& e : m_temporary_subresource_cache)
		{
			ensure(e.second.second);
			release_temporary_subresource(e.second.second);
		}
		m_temporary_subresource_cache.clear();

		return any_released;
	}

	void texture_cache::on_frame_end()
	{
		trim_sections();

		if (m_storage.m_unreleased_texture_objects >= m_max_zombie_objects)
		{
			purge_unreleased_sections();
		}

#ifdef RPCS3_PS4
		// 0.38: the pool keeps at most 64 images and 32 MiB, newest first. Textures evicted for window space pass
		// through it, so with upstream's 256 entries / 256 MiB an eviction freed nothing of the window. Its images are
		// idle, so the oldest are destroyed at once.
		if (utils::ps4::vram::image_pool_over(m_cached_images.size(), m_cached_memory_size))
		{
			std::lock_guard lock(m_cached_pool_lock);

			while (!m_cached_images.empty() && utils::ps4::vram::image_pool_over(m_cached_images.size(), m_cached_memory_size))
			{
				m_cached_memory_size -= m_cached_images.back().data->memory->size();
				m_cached_images.pop_back();
			}
		}
#else
		if (m_cached_images.size() > max_cached_image_pool_size ||
			m_cached_memory_size > 256 * 0x100000)
		{
			std::lock_guard lock(m_cached_pool_lock);

			const auto new_size = m_cached_images.size() / 2;
			for (usz i = new_size; i < m_cached_images.size(); ++i)
			{
				m_cached_memory_size -= m_cached_images[i].data->memory->size();
			}

			m_cached_images.resize(new_size);
		}
#endif

		baseclass::on_frame_end();
		reset_frame_statistics();
	}

	vk::viewable_image* texture_cache::upload_image_simple(vk::command_buffer& cmd, VkFormat format, u32 address, u32 width, u32 height, u32 pitch)
	{
		bool linear_format_supported = false;

		switch (format)
		{
		case VK_FORMAT_B8G8R8A8_UNORM:
			linear_format_supported = m_formats_support.bgra8_linear;
			break;
		case VK_FORMAT_R8G8B8A8_UNORM:
			linear_format_supported = m_formats_support.argb8_linear;
			break;
		default:
			rsx_log.error("Unsupported VkFormat 0x%x", static_cast<u32>(format));
			return nullptr;
		}

		if (!linear_format_supported)
		{
			return nullptr;
		}

		// Uploads a linear memory range as a BGRA8 texture
		auto image = std::make_unique<vk::viewable_image>(*m_device, m_memory_types.host_visible_coherent,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
			VK_IMAGE_TYPE_2D,
			format,
			width, height, 1, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_LAYOUT_PREINITIALIZED,
			VK_IMAGE_TILING_LINEAR, VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0,
			VMM_ALLOCATION_POOL_SWAPCHAIN);

		VkImageSubresource subresource{};
		subresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;

		VkSubresourceLayout layout{};
		vkGetImageSubresourceLayout(*m_device, image->value, &subresource, &layout);

		void* mem = image->memory->map(0, layout.rowPitch * height);

		auto src = vm::_ptr<const char>(address);
		auto dst = static_cast<char*>(mem);

		// TODO: SSE optimization
		for (u32 row = 0; row < height; ++row)
		{
			auto casted_src = reinterpret_cast<const be_t<u32>*>(src);
			auto casted_dst = reinterpret_cast<u32*>(dst);

			for (u32 col = 0; col < width; ++col)
				casted_dst[col] = casted_src[col];

			src += pitch;
			dst += layout.rowPitch;
		}

		image->memory->unmap();

		vk::change_image_layout(cmd, image.get(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);

		// Fully dispose immediately. These immages aren't really reusable right now.
		auto result = image.get();
		vk::get_resource_manager()->dispose(image);

		return result;
	}

	bool texture_cache::blit(const rsx::blit_src_info& src, const rsx::blit_dst_info& dst, bool interpolate, vk::surface_cache& m_rtts, vk::command_buffer& cmd)
	{
		blitter helper;
		auto reply = upload_scaled_image(src, dst, interpolate, cmd, m_rtts, helper);

		if (reply.succeeded)
		{
#ifdef RPCS3_PS4
			// 0.32: a speculative readback is appended to the frame's command buffer; off unless dma.flag speculate=on.
			// The CPU access that would have used it still gets its data through the regular flush.
			if (reply.dst_range.valid() && vk::ps4_dma_speculate())
#else
			if (reply.dst_range.valid())
#endif
			{
				flush_if_cache_miss_likely(cmd, reply.dst_range);
			}

			return true;
		}

		return false;
	}

	u32 texture_cache::get_unreleased_textures_count() const
	{
		return baseclass::get_unreleased_textures_count() + ::size32(m_cached_images);
	}

	u64 texture_cache::get_temporary_memory_in_use() const
	{
		// TODO: Technically incorrect, we should have separate metrics for cached evictable resources (this value) and temporary active resources.
		return m_cached_memory_size;
	}

	bool texture_cache::is_overallocated() const
	{
		const auto total_device_memory = m_device->get_memory_mapping().device_local_total_bytes / 0x100000;
		u64 quota = 0;

		if (total_device_memory >= 2048)
		{
			quota = std::min<u64>(3072, (total_device_memory * 40) / 100);
		}
		else if (total_device_memory >= 1024)
		{
			quota = std::max<u64>(204, (total_device_memory * 30) / 100);
		}
		else if (total_device_memory >= 768)
		{
			quota = 192;
		}
		else
		{
			quota = std::min<u64>(128, total_device_memory / 2);
		}

		quota *= 0x100000;

#ifdef RPCS3_PS4
		// 0.38: the quota is never above 192 MiB nor a quarter of the GPU window budget. A full window with textures
		// under it is handled by the age-based eviction (VKGSRender::ps4_evict_textures_by_age at severe and fatal), not
		// by evicting every unbound texture each fatal trim (the 0.33 Sonic/WipEout sessions sat at 100-119% of the window
		// with 115-120 MiB of textures, under the old quota, and nothing was evicted).
		quota = utils::ps4::vram::texture_quota(quota, u64{ vk::ps4_window_budget_mib() } * 0x100000);
#endif

		if (const u64 texture_cache_pool_usage = vmm_get_application_pool_usage(VMM_ALLOCATION_POOL_TEXTURE_CACHE);
			texture_cache_pool_usage > quota)
		{
			rsx_log.warning("Texture cache is using %lluM of memory which exceeds the allocation quota of %lluM",
				texture_cache_pool_usage, quota);
			return true;
		}

		return false;
	}
}
