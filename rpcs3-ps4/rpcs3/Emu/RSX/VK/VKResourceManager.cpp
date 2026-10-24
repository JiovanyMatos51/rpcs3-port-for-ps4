#include "stdafx.h"
#include "VKResourceManager.h"
#include "VKGSRender.h"
#include "VKCommandStream.h"

#ifdef RPCS3_PS4
#include "VKDMA.h"
#include "Emu/Cell/timers.hpp"
#include "ps4_fatal.h"
#include "ps4_launcher.h"
#include "util/ps4_adapt.hpp"
#include "util/ps4_gpu_health.hpp"

#include <atomic>
#include <cstdio>
#endif

namespace vk
{
#ifdef RPCS3_PS4
	// 0.31: severity + 1 of the strongest trim requested by a thread that may not run one (0 = none). The RSX
	// thread serves it in vmm_check_memory_usage() at its next frame end.
	static std::atomic<u32> s_ps4_vram_trim_request{ 0 };

	static void ps4_request_vram_trim(rsx::problem_severity severity)
	{
		// The allocation that asked already failed, so the trim is at least severe.
		const u32 value = static_cast<u32>(std::max(severity, rsx::problem_severity::severe)) + 1;
		u32 current = s_ps4_vram_trim_request.load(std::memory_order_relaxed);
		while (current < value && !s_ps4_vram_trim_request.compare_exchange_weak(current, value, std::memory_order_relaxed))
		{
		}

		utils::ps4::gpu::note_deferred_trim();
	}

	void ps4_note_vram_pressure(const char* what) noexcept
	{
		static std::atomic<u32> s_events{ 0 };
		static std::atomic<u64> s_next_note_us{ 0 };

		const u32 count = s_events.fetch_add(1, std::memory_order_relaxed) + 1;

		// startup.log lines are written with fsync, and a game at the cap can fail many allocations in a single
		// frame. Only the first event and then at most one every 30 s get a line; the event number shows the rest.
		const u64 now = get_system_time();
		if (u64 next = s_next_note_us.load(std::memory_order_relaxed);
			now >= next && s_next_note_us.compare_exchange_strong(next, now + 30'000'000, std::memory_order_relaxed))
		{
			char detail[192];
			std::snprintf(detail, sizeof(detail), "%s (event %u)", what ? what : "pressure", count);
			ps4_fatal_note("VRAM: ", detail);
		}

		// adapt::note writes the key once per game session. On the next boots the policy lowers
		// vram_allocation_limit for this game, so its caches are trimmed before the orbis-drm window runs out.
		utils::ps4::adapt::note(utils::ps4::adapt::key_vram_low);
	}

	// vkutils/memory.cpp (0.39): every empty VMA block released (bytes); a window-rated fatal load that a trim can change
	// (util/ps4_vram_policy.hpp fatal_load); whether the current allocator runs the vma_release feature.
	u64 ps4_vma_free_empty_blocks();
	bool ps4_fatal_rating_confirmed();
	bool ps4_vma_release_on() noexcept;

	// Fatal trims hard-sync the GPU (flush_command_queue + driver drain) before evicting anything. Near the window
	// budget the load stays above 95% while nothing more can be evicted, which turned every frame into a hard sync
	// (Wipeout: 1024 fatal trims in 79 s). Default (vma_release off, as 0.38): after a fatal trim that relieved nothing,
	// the window-load check runs at severe for the next 500 ms. With vma_release (GTA IV 0.38: two hard syncs per frame
	// for 18 min): window-rated fatal trims run at most once a second, and 4 s apart while the last one did not lower
	// the allocator's blocks by 16 MiB (util/ps4_vram_policy.hpp); the others run at severe. Allocation failures and
	// trims requested by other threads keep the fatal path. Only the RSX thread runs trims (vmm_handle_memory_pressure
	// hands the others over), so plain statics suffice.
	constexpr u64 ps4_fatal_trim_backoff_us = 500'000;
	static u64 s_ps4_last_fatal_trim_us = 0;
	static bool s_ps4_last_fatal_trim_relieved = true;
	static u64 s_ps4_fatal_trim_blocks_before = 0; // VMA block total right before the last fatal trim (vma_release)
	static u32 s_ps4_empty_block_passes = 0;
	// /data/rpcs3/vram-trim-every-frame.flag restores a fatal trim on every check. 0 = not read yet for this
	// renderer, 1 = back-off on, 2 = flag present. Reset with the renderer (vmm_reset).
	static u32 s_ps4_trim_every_frame_state = 0;
	static bool s_ps4_trim_backoff_logged = false;

	// 0.36: severe trims rated by the window load alone that keep freeing nothing (Wolverine 0.35: 9746 pressure trims
	// in 885 s while the VMA blocks stayed at 580 MiB) back off to moderate (futile_trim_backoff). RSX thread only.
	static utils::ps4::gpu::futile_trim_backoff s_ps4_futile_trims{};
	static bool s_ps4_futile_trims_logged = false;

	static bool ps4_trim_every_frame()
	{
		if (!s_ps4_trim_every_frame_state)
		{
			s_ps4_trim_every_frame_state = fs::is_file("/data/rpcs3/vram-trim-every-frame.flag") ? 2 : 1;
			if (s_ps4_trim_every_frame_state == 2)
			{
				ps4_startup_checkpoint("VRAM: fatal and window-only severe trims run on every check, also when the last ones freed nothing (vram-trim-every-frame.flag)");
			}
		}

		return s_ps4_trim_every_frame_state == 2;
	}

	// Bytes of the allocator's blocks and dedicated allocations in all heaps (the window load's numerator).
	static u64 ps4_vma_block_total()
	{
		utils::ps4::gpu::vram_snapshot heaps{};
		ps4_query_vram_heaps(heaps);

		u64 total = 0;
		for (u32 i = 0; i < heaps.heap_count; ++i)
		{
			total += heaps.heap_usage[i];
		}

		return total;
	}

	static bool ps4_back_off_fatal_trim()
	{
		if (!ps4_vma_release_on())
		{
			// The 0.38 back-off: 500 ms at severe after a fatal trim that freed nothing.
			if (s_ps4_last_fatal_trim_relieved)
			{
				return false;
			}

			const u64 now = get_system_time();
			if (now - s_ps4_last_fatal_trim_us >= ps4_fatal_trim_backoff_us)
			{
				return false;
			}

			if (ps4_trim_every_frame())
			{
				return false;
			}

			utils::ps4::gpu::note_trim_backoff();
			if (!s_ps4_trim_backoff_logged)
			{
				s_ps4_trim_backoff_logged = true;
				ps4_startup_checkpoint(fmt::format("VRAM: fatal trim backed off to severe (the fatal trim %llu ms ago freed nothing); "
					"later ones are counted as 'fatal trims backed off' in the PS4 VRAM line (vram-trim-every-frame.flag disables this)",
					(now - s_ps4_last_fatal_trim_us) / 1000).c_str());
			}

			return true;
		}

		if (!s_ps4_last_fatal_trim_us || ps4_trim_every_frame())
		{
			return false;
		}

		// Measured now, not right after the trim: the garbage collector frees what it evicted once the GPU is done.
		const u64 now = get_system_time();
		const u64 blocks_now = ps4_vma_block_total();
		const bool relieved = utils::ps4::vram::fatal_trim_relieved(s_ps4_fatal_trim_blocks_before, blocks_now);
		if (!utils::ps4::vram::fatal_trim_back_off(now, s_ps4_last_fatal_trim_us, relieved))
		{
			return false;
		}

		utils::ps4::gpu::note_trim_backoff();
		if (!s_ps4_trim_backoff_logged)
		{
			s_ps4_trim_backoff_logged = true;
			ps4_startup_checkpoint(fmt::format("VRAM: fatal trim backed off to severe (the fatal trim %llu ms ago took the allocator's blocks from %llu to %llu MiB; "
				"window-rated fatal trims run at most once a second, 4 s apart while one frees less than 16 MiB of blocks); later ones are counted as "
				"'fatal trims backed off' in the PS4 VRAM line (vram-trim-every-frame.flag disables this)",
				(now - s_ps4_last_fatal_trim_us) / 1000, s_ps4_fatal_trim_blocks_before >> 20, blocks_now >> 20).c_str());
		}

		return true;
	}
#endif

	struct vmm_memory_stats
	{
		std::unordered_map<uptr, vmm_allocation_t> allocations;
		std::unordered_map<uptr, atomic_t<u64>> memory_usage;
		std::unordered_map<vmm_allocation_pool, atomic_t<u64>> pool_usage;
		shared_mutex mutex;

		void clear()
		{
			if (!allocations.empty())
			{
				rsx_log.error("Leaking memory allocations!");
				for (auto& leak : allocations)
				{
					rsx_log.error("Memory handle 0x%llx (%llu bytes) allocated from pool %d was not freed.",
						leak.first, leak.second.size, static_cast<int>(leak.second.pool));
				}
			}

			allocations.clear();
			memory_usage.clear();
			pool_usage.clear();
		}
	}
	g_vmm_stats;

	resource_manager g_resource_manager;
	atomic_t<u64> g_event_ctr;
	atomic_t<u64> g_last_completed_event;

	constexpr u64 s_vmm_warn_threshold_size = 2000 * 0x100000; // Warn if allocation on a single heap exceeds this value

	resource_manager* get_resource_manager()
	{
		return &g_resource_manager;
	}

	garbage_collector* get_gc()
	{
		return &g_resource_manager;
	}

	void resource_manager::trim()
	{
		// For any managed resources, try to keep the number of unused/idle resources as low as possible.
		// Improves search times as well as keeping us below the hardware limit.
		const auto& limits = get_current_renderer()->gpu().get_limits();
		const auto allocated_sampler_count = vmm_get_application_pool_usage(VMM_ALLOCATION_POOL_SAMPLER);
		const auto max_allowed_samplers = std::min((limits.maxSamplerAllocationCount * 3u) / 4u, 2048u);

		if (allocated_sampler_count > max_allowed_samplers)
		{
			ensure(max_allowed_samplers);
			rsx_log.warning("Trimming allocated samplers. Allocated = %u, Max = %u", allocated_sampler_count, limits.maxSamplerAllocationCount);

			auto filter_expr = [](const cached_sampler_object_t& sampler)
			{
				// Pick only where we have no ref
				return !sampler.has_refs();
			};

			for (auto& object : m_sampler_pool.collect(filter_expr))
			{
				dispose(object);
			}
		}
	}

	u64 get_event_id()
	{
		return g_event_ctr++;
	}

	u64 current_event_id()
	{
		return g_event_ctr.load();
	}

	u64 last_completed_event_id()
	{
		return g_last_completed_event.load();
	}

	void on_event_completed(u64 event_id)
	{
		g_fxo->get<vk::driver_manager_thread>().notify_completed(event_id);
		g_last_completed_event.atomic_op([event_id](u64& value)
		{
			value = std::max(event_id, value);
		});
	}

	void print_debug_markers()
	{
		for (const auto marker : g_resource_manager.gather_debug_markers())
		{
			marker->dump();
		}
	}

	static constexpr f32 size_in_GiB(u64 size)
	{
		return size / (1024.f * 1024.f * 1024.f);
	}

	void vmm_notify_memory_allocated(void* handle, u32 memory_type, u64 memory_size, vmm_allocation_pool pool)
	{
		auto key = reinterpret_cast<uptr>(handle);
		const vmm_allocation_t info = { memory_size, memory_type, pool };

		std::lock_guard lock(g_vmm_stats.mutex);

		if (const auto ins = g_vmm_stats.allocations.insert_or_assign(key, info);
			!ins.second)
		{
			rsx_log.error("Duplicate vmm entry with memory handle 0x%llx", key);
		}

		g_vmm_stats.pool_usage[pool] += memory_size;

		auto& vmm_size = g_vmm_stats.memory_usage[memory_type];
		vmm_size += memory_size;

		if (vmm_size > s_vmm_warn_threshold_size && (vmm_size - memory_size) <= s_vmm_warn_threshold_size)
		{
			rsx_log.warning("Memory type 0x%x has allocated more than %04.2fG. Currently allocated %04.2fG",
				memory_type, size_in_GiB(s_vmm_warn_threshold_size), size_in_GiB(vmm_size));
		}
	}

	void vmm_notify_memory_freed(void* handle)
	{
		std::lock_guard lock(g_vmm_stats.mutex);

		auto key = reinterpret_cast<uptr>(handle);
		if (auto found = g_vmm_stats.allocations.find(key);
			found != g_vmm_stats.allocations.end())
		{
			const auto& info = found->second;
			g_vmm_stats.memory_usage[info.type_index] -= info.size;
			g_vmm_stats.pool_usage[info.pool] -= info.size;
			g_vmm_stats.allocations.erase(found);
		}
	}

	void vmm_reset()
	{
		std::lock_guard lock(g_vmm_stats.mutex);

		g_vmm_stats.clear();
		g_event_ctr = 0;
		g_last_completed_event = 0;

#ifdef RPCS3_PS4
		// The renderer is gone: the next one starts without a back-off and reads the flag again.
		s_ps4_last_fatal_trim_us = 0;
		s_ps4_last_fatal_trim_relieved = true;
		s_ps4_fatal_trim_blocks_before = 0;
		s_ps4_empty_block_passes = 0;
		s_ps4_trim_every_frame_state = 0;
		s_ps4_trim_backoff_logged = false;
		s_ps4_futile_trims = {};
		s_ps4_futile_trims_logged = false;
#endif
	}

	u64 vmm_get_application_memory_usage_impl(const memory_type_info& memory_type)
	{
		u64 result = 0;
		for (const auto& memory_type_index : memory_type)
		{
			auto it = g_vmm_stats.memory_usage.find(memory_type_index);
			if (it == g_vmm_stats.memory_usage.end())
			{
				continue;
			}

			result += it->second.observe();
		}

		return result;
	}

	u64 vmm_get_application_memory_usage(const memory_type_info& memory_type)
	{
		reader_lock lock(g_vmm_stats.mutex);
		return vmm_get_application_memory_usage_impl(memory_type);
	}

	u64 vmm_get_application_pool_usage_impl(vmm_allocation_pool pool)
	{
		if (auto found = g_vmm_stats.pool_usage.find(pool);
			found != g_vmm_stats.pool_usage.end())
		{
			return found->second;
		}
		return 0ull;
	}

	u64 vmm_get_application_pool_usage(vmm_allocation_pool pool)
	{
		reader_lock lock(g_vmm_stats.mutex);
		return vmm_get_application_pool_usage_impl(pool);
	}

	rsx::problem_severity vmm_determine_memory_load_severity()
	{
		reader_lock lock(g_vmm_stats.mutex);

#ifdef RPCS3_PS4
		// 0.32: all heaps share one orbis-drm GPU window; the per-heap load stayed at 59-64% in 0.31 until the window
		// ran out. Rate the load against the budget for the whole window as well.
		const f32 ps4_window_load = ps4_window_load_percent();
		const auto vmm_load = std::max(get_current_mem_allocator()->get_memory_usage(), ps4_window_load);
#else
		const auto vmm_load = get_current_mem_allocator()->get_memory_usage();
#endif
		rsx::problem_severity load_severity = rsx::problem_severity::low;

		// Fragmentation tuning
		if (vmm_load < 50.f)
		{
			get_current_mem_allocator()->set_fastest_allocation_flags();
		}
		else if (vmm_load > 75.f)
		{
			// Avoid fragmentation if we can
			get_current_mem_allocator()->set_safest_allocation_flags();

			if (vmm_load > 95.f)
			{
				// Drivers will often crash long before returning OUT_OF_DEVICE_MEMORY errors.
				load_severity = rsx::problem_severity::fatal;
#ifdef RPCS3_PS4
				// vma_release: blocks VMA holds but does not use (released by the allocator itself) do not make a fatal
				// rating; the allocations over 95% or no 64 MiB free range in the window do (vram-trim-every-frame.flag:
				// 0.38). Off (default): the 0.38 rating.
				if (ps4_vma_release_on() && !ps4_trim_every_frame() && !ps4_fatal_rating_confirmed())
				{
					load_severity = rsx::problem_severity::severe;
				}
#endif
			}
			else if (vmm_load > 90.f)
			{
				load_severity = rsx::problem_severity::severe;
			}
			else
			{
				load_severity = rsx::problem_severity::moderate;
			}

			// Query actual usage for comparison. Maybe we just have really fragmented memory...
			const auto mem_info = get_current_renderer()->get_memory_mapping();
			const auto local_memory_usage = vmm_get_application_memory_usage_impl(mem_info.device_local);

			constexpr u64 _1M = 0x100000;
			const auto res_scale = rsx::get_current_renderer()->resolution_scaling_config.scale_factor();
			const auto mem_threshold_1 = static_cast<u64>(256 * res_scale * res_scale) * _1M;
			const auto mem_threshold_2 = static_cast<u64>(64 * res_scale * res_scale) * _1M;

#ifdef RPCS3_PS4
			// 0.32: the relaxation below compares device-local usage with the device cap only. In 0.31 it rated a
			// nearly full window 'low' (device_local=93M of 768M). Keep the severity while the window is loaded.
			if (ps4_window_load > 75.f)
			{
				// Keep load_severity as rated from vmm_load above.
			}
			else
#endif
			if (local_memory_usage < (mem_info.device_local_total_bytes / 2) ||                   // Less than 50% VRAM usage OR
				(mem_info.device_local_total_bytes - local_memory_usage) > mem_threshold_1)       // Enough to hold all required resources left
			{
				// Lower severity to avoid slowing performance too much
				load_severity = rsx::problem_severity::low;
			}
			else if ((mem_info.device_local_total_bytes - local_memory_usage) > mem_threshold_2)  // Enough to hold basic resources like textures, buffers, etc
			{
				// At least 512MB left, do not overreact
				load_severity = rsx::problem_severity::moderate;
			}

#ifdef RPCS3_PS4
			// 0.32: this runs several times per frame, and near the window budget the load can stay above 75% for
			// minutes; log the pair at most every 10 s instead of flooding RPCS3.log.
			static std::atomic<u64> s_ps4_next_load_log_us{ 0 };
			const u64 ps4_now = get_system_time();
			u64 ps4_next = s_ps4_next_load_log_us.load(std::memory_order_relaxed);
			if (load_severity >= rsx::problem_severity::moderate && ps4_now >= ps4_next &&
				s_ps4_next_load_log_us.compare_exchange_strong(ps4_next, ps4_now + 10'000'000, std::memory_order_relaxed))
#else
			if (load_severity >= rsx::problem_severity::moderate)
#endif
			{
				// NOTE: For some reason fmt::format with a sized float followed by percentage sign causes random crashing.
				// This is a bug unrelated to this, but explains why we're going with integral percentages here.
				const auto application_memory_load = (local_memory_usage * 100) / mem_info.device_local_total_bytes;
				rsx_log.warning("Actual device memory used by internal allocations is %lluM (%llu%%)", local_memory_usage / 0x100000, application_memory_load);
				rsx_log.warning("Video memory usage is at %d%%. Will attempt to reclaim some resources.", static_cast<int>(vmm_load));
#ifdef RPCS3_PS4
				rsx_log.warning("PS4 VRAM: window load %d%% of the %u MiB budget", static_cast<int>(ps4_window_load), ps4_window_budget_mib());
#endif
			}
		}

		return load_severity;
	}

	bool vmm_handle_memory_pressure(rsx::problem_severity severity)
	{
		if (auto vkthr = dynamic_cast<VKGSRender*>(rsx::get_current_renderer()))
		{
#ifdef RPCS3_PS4
			// 0.31: on_vram_exhausted() asserts that it runs on the RSX thread outside an uninterruptible section,
			// because evicting textures or surfaces anywhere else races the renderer. An allocation that ran out
			// of memory on a PPU/SPU thread (a DMA mirror created in the access-violation flush) or inside an
			// uninterruptible upload used to trip that assertion and kill the thread. Hand the trim to the RSX
			// thread instead (it runs at the next frame end).
			// 0.32: nothing may be evicted here, but finished GPU work can still return memory: retire it and run
			// the garbage collector, and report whether that freed anything so the caller retries once (0.31
			// returned false, and the required DMA-mirror allocation that killed it went straight to die_with_error).
			if (!vkthr->is_current_thread() || vk::is_uninterruptible())
			{
				ps4_request_vram_trim(severity);
				return vkthr->ps4_retire_completed_work();
			}

			if (!ps4_vma_release_on())
			{
				// 0.38: the trim's own result feeds the back-off.
				const bool relieved = vkthr->on_vram_exhausted(severity);
				if (severity >= rsx::problem_severity::fatal)
				{
					// Every fatal trim (window load or a failed allocation) feeds the back-off in vmm_check_memory_usage.
					s_ps4_last_fatal_trim_us = get_system_time();
					s_ps4_last_fatal_trim_relieved = relieved;
				}

				return relieved;
			}

			if (severity >= rsx::problem_severity::fatal)
			{
				// Every fatal trim (window load or a failed allocation) feeds the back-off in vmm_check_memory_usage, which
				// compares the allocator's blocks with their total before the trim.
				s_ps4_last_fatal_trim_us = get_system_time();
				s_ps4_fatal_trim_blocks_before = ps4_vma_block_total();
			}

			return vkthr->on_vram_exhausted(severity);
#else
			return vkthr->on_vram_exhausted(severity);
#endif
		}

		return false;
	}

#ifdef RPCS3_PS4
	bool ps4_vram_relieve_deferred(rsx::problem_severity severity)
	{
		auto vkthr = dynamic_cast<VKGSRender*>(rsx::get_current_renderer());
		if (!vkthr)
		{
			return false;
		}

		// The same as an allocation that failed off the RSX thread: the trim runs at the next frame end.
		ps4_request_vram_trim(severity);
		return vkthr->ps4_retire_completed_work();
	}

	bool ps4_vram_deep_reclaim(const char* why)
	{
		auto vkthr = dynamic_cast<VKGSRender*>(rsx::get_current_renderer());
		if (!vkthr || !vkthr->is_current_thread() || vk::is_uninterruptible())
		{
			return false;
		}

		return vkthr->ps4_deep_reclaim(why);
	}
#endif

	void vmm_check_memory_usage()
	{
#ifdef RPCS3_PS4
		auto load_severity = vmm_determine_memory_load_severity();

		// Serve a trim another thread asked for (see vmm_handle_memory_pressure). This runs at the frame end on
		// the RSX thread, where the upstream check below may already run a fatal-severity trim.
		const u32 requested = s_ps4_vram_trim_request.exchange(0, std::memory_order_relaxed);
		if (requested)
		{
			const auto severity = static_cast<rsx::problem_severity>(std::min<u32>(requested - 1, static_cast<u32>(rsx::problem_severity::fatal)));
			load_severity = std::max(load_severity, severity);

			if (const u32 count = utils::ps4::gpu::deferred_trims(); count && !(count & (count - 1)))
			{
				// Logged at the 1st, 2nd, 4th, 8th... request of the process.
				ps4_startup_checkpoint(fmt::format("VRAM: trim requested off the RSX thread now runs on it (severity %s, %u request(s) so far)",
					severity == rsx::problem_severity::fatal ? "fatal" : "severe", count).c_str());
			}
		}

		const auto ps4_rated = load_severity;

		// vma_release: at a severe or fatal rating, every empty allocator block goes back to the window first (no GPU
		// sync: an empty block holds no allocation). Off (default): ps4_vma_free_empty_blocks() frees nothing.
		if (ps4_rated >= rsx::problem_severity::severe)
		{
			if (const u64 freed = ps4_vma_free_empty_blocks())
			{
				const u32 passes = ++s_ps4_empty_block_passes;
				if (!(passes & (passes - 1)))
				{
					// The 1st, 2nd, 4th... of this renderer.
					ps4_startup_checkpoint(fmt::format("VRAM: %llu MiB of empty allocator blocks released at a %s rating (release #%u); allocator blocks now %llu MiB",
						freed >> 20, ps4_rated == rsx::problem_severity::fatal ? "fatal" : "severe", passes, ps4_vma_block_total() >> 20).c_str());
				}
			}
		}

		if (load_severity == rsx::problem_severity::fatal && !requested && ps4_back_off_fatal_trim())
		{
			load_severity = rsx::problem_severity::severe;
		}

		// 0.36: a severe rating that the allocator's own heap load (90% or less) would not give comes from the window
		// load. When such trims keep leaving the block total where it was, they run at moderate, the rating of a
		// 75-90% load. Fatal ratings (also when backed off above), requested trims and failed allocations keep their
		// paths.
		if (ps4_rated == rsx::problem_severity::severe && !requested && get_current_mem_allocator()->get_memory_usage() <= 90.f &&
			!ps4_trim_every_frame() && s_ps4_futile_trims.should_skip(get_system_time(), ps4_vma_block_total()))
		{
			load_severity = rsx::problem_severity::moderate;
			utils::ps4::gpu::note_futile_trim_skip();

			if (!s_ps4_futile_trims_logged)
			{
				s_ps4_futile_trims_logged = true;
				ps4_startup_checkpoint(fmt::format("VRAM: %u window-only severe trims in a row left the VMA blocks at %llu MiB; they pause for %llu s at a time "
					"and run at moderate (counted as 'futile severe trims skipped' in the PS4 VRAM line; vram-trim-every-frame.flag disables this)",
					utils::ps4::gpu::futile_trim_backoff::streak_limit, s_ps4_futile_trims.reference >> 20,
					utils::ps4::gpu::futile_trim_backoff::backoff_us / 1'000'000).c_str());
			}
		}

		if (load_severity >= rsx::problem_severity::moderate)
		{
			vmm_handle_memory_pressure(load_severity);
		}
#else
		if (const auto load_severity = vmm_determine_memory_load_severity();
			load_severity >= rsx::problem_severity::moderate)
		{
			vmm_handle_memory_pressure(load_severity);
		}
#endif
	}

	void vmm_notify_object_allocated(vmm_allocation_pool pool)
	{
		std::lock_guard lock(g_vmm_stats.mutex);

		ensure(pool >= VMM_ALLOCATION_POOL_SAMPLER);
		g_vmm_stats.pool_usage[pool]++;
	}

	void vmm_notify_object_freed(vmm_allocation_pool pool)
	{
		std::lock_guard lock(g_vmm_stats.mutex);

		ensure(pool >= VMM_ALLOCATION_POOL_SAMPLER);
		g_vmm_stats.pool_usage[pool]--;
	}
}
