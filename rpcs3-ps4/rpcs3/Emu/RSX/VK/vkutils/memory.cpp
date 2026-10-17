#include "device.h"
#include "memory.h"

#include "Emu/RSX/Utils/algorithm.hpp"
#include "Emu/system_config.h"

#ifdef RPCS3_PS4
#include "../VKDMA.h"
#include "Emu/Cell/timers.hpp"
#include "ps4_fatal.h"
#include "ps4_launcher.h"
#include "util/ps4_gpu_health.hpp"
#include "util/ps4_i18n.hpp"
#include "util/ps4_vram_policy.hpp"

#include <array>
#include <atomic>
#include <cstdio>
#include <new>
#include <optional>
#include <string>

// 0.38: orbis-drm winsys of the linked RADV (ac_orbis_drm.c, whole-archive): fills {window size, bytes mapped, largest
// free range} of the one GPU VA window from its live VA list under the VA lock; the device, heap and flags arguments
// are not read. Weak: without it the meter reports nothing and the 0.32 VMA-only load applies.
struct ps4_drm_heap_info
{
	u64 heap_size;
	u64 heap_usage;
	u64 max_allocation;
};
extern "C" int ac_drm_query_heap_info(void* dev, u32 heap, u32 flags, ps4_drm_heap_info* info) __attribute__((weak));
#endif

namespace vk
{
#ifdef RPCS3_PS4
	namespace
	{
		namespace vram_policy = utils::ps4::vram;

		// 0.38: window meter. 0 = not probed for this allocator, 1 = on, 2 = off (vram-meter-off.flag), 3 = unavailable.
		std::atomic<u32> s_ps4_meter_state{ 0 };
		std::atomic<u64> s_ps4_meter_sample_us{ 0 };
		std::atomic<u64> s_ps4_meter_size{ 0 };
		std::atomic<u64> s_ps4_meter_used{ 0 };
		std::atomic<u64> s_ps4_meter_hole{ 0 };

		// 0.38: emergency reserve (raw device memory, never bound) and the counters of admission and rescue.
		std::atomic<VkDevice> s_ps4_reserve_device{ VK_NULL_HANDLE };
		std::atomic<VkDeviceMemory> s_ps4_reserve_memory{ VK_NULL_HANDLE };
		std::atomic<u64> s_ps4_reserve_bytes{ 0 };
		std::atomic<u64> s_ps4_last_admission_us{ 0 };
		std::atomic<u32> s_ps4_admission_passes{ 0 };
		std::atomic<u32> s_ps4_rescues{ 0 };
		std::atomic<u32> s_ps4_rescue_failures{ 0 };

		bool ps4_flag_present(const char* path)
		{
			if (std::FILE* file = std::fopen(path, "rb"))
			{
				std::fclose(file);
				return true;
			}

			return false;
		}

		// Reads the window meter now. False (and the meter marked unavailable) when the query fails.
		bool ps4_meter_query()
		{
			if (!ac_drm_query_heap_info)
			{
				s_ps4_meter_state.store(3, std::memory_order_relaxed);
				return false;
			}

			ps4_drm_heap_info info{};
			if (ac_drm_query_heap_info(nullptr, 0, 0, &info) != 0 || !info.heap_size || info.heap_usage > info.heap_size)
			{
				s_ps4_meter_state.store(3, std::memory_order_relaxed);
				return false;
			}

			s_ps4_meter_size.store(info.heap_size, std::memory_order_relaxed);
			s_ps4_meter_used.store(info.heap_usage, std::memory_order_relaxed);
			s_ps4_meter_hole.store(info.max_allocation, std::memory_order_relaxed);
			return true;
		}

		// The first allocator of a renderer reads the switch and probes the query once (startup.log line).
		void ps4_meter_probe()
		{
			if (ps4_flag_present("/data/rpcs3/vram-meter-off.flag"))
			{
				s_ps4_meter_state.store(2, std::memory_order_relaxed);
				ps4_startup_checkpoint("PS4 VRAM: window meter off (vram-meter-off.flag); window load from the allocator's blocks only (0.32 rule)");
				return;
			}

			s_ps4_meter_state.store(1, std::memory_order_relaxed);
			s_ps4_meter_sample_us.store(get_system_time(), std::memory_order_relaxed);
			if (!ps4_meter_query())
			{
				ps4_startup_checkpoint(ac_drm_query_heap_info
					? "PS4 VRAM: window meter unavailable (ac_drm_query_heap_info failed); window load from the allocator's blocks only"
					: "PS4 VRAM: window meter unavailable (ac_drm_query_heap_info not linked); window load from the allocator's blocks only");
				return;
			}

			ps4_startup_checkpoint(fmt::format("PS4 VRAM: window meter on (orbis-drm VA list, sampled every %llu ms): window %llu MiB, mapped %llu MiB, "
				"largest free range %llu MiB; trims and admission use it (vram-meter-off.flag disables it)",
				vram_policy::meter_period_us / 1000, s_ps4_meter_size.load() >> 20, s_ps4_meter_used.load() >> 20,
				s_ps4_meter_hole.load() >> 20).c_str());
		}

		// Allocator whose heaps ps4_query_vram_heaps() reads. It is created with the render device before the RSX
		// thread starts and destroyed after it has stopped, and the RSX thread is the only caller of the query.
		std::atomic<VmaAllocator> s_ps4_vma{ nullptr };
		std::atomic<u32> s_ps4_device_cap_mib{ 0 };
		std::atomic<u32> s_ps4_host_cap_mib{ 0 };

		// 0.32: budget for the bytes VMA holds in all heaps together. Every heap lives in one ~991 MiB orbis-drm
		// window that RADV shares (command streams, swapchain, shader arenas, descriptor pools). VMA judges each heap
		// against 80% of its own size or cap, and those budgets add up to more than the window, so 0.31 never trimmed
		// before the driver ran out of address space. This budget only drives the trim severity. It is fixed rather
		// than derived from vram_allocation_limit, which caps the device-local heap alone (448 MiB after vram=low).
		// Estimated from the 0.31 VA layout: ~650-700 MiB fit beside RADV's own objects with 128 command buffers per
		// chain. A driver failure below the budget lowers it (ps4_note_allocation_failure).
		constexpr u64 ps4_window_budget_default = 640 * 0x100000ull;
		constexpr u64 ps4_window_budget_floor = 384 * 0x100000ull;
		// VMA_DEFAULT_LARGE_HEAP_BLOCK_SIZE (VKMemAlloc.cpp).
		constexpr u64 ps4_vma_block_bytes = 32 * 0x100000ull;
		std::atomic<u64> s_ps4_window_budget{ ps4_window_budget_default };
		// pHeapSizeLimit per heap (VK_WHOLE_SIZE = no cap), to tell a VMA cap refusal from a driver failure.
		std::array<std::atomic<u64>, VK_MAX_MEMORY_HEAPS> s_ps4_heap_limits{};
		// 0.39: /data/rpcs3/vma-keep-empty.flag (read when the allocator is created): VMA keeps its spare empty block
		// per memory type as upstream (no release under window pressure, no frame-end release pass).
		std::atomic<bool> s_ps4_vma_keep_empty{ false };

		u64 ps4_vma_total_bytes(VmaAllocator allocator)
		{
			// Without VK_EXT_memory_budget, usage = bytes of VMA's blocks and dedicated allocations.
			const VkPhysicalDeviceMemoryProperties* properties = nullptr;
			vmaGetMemoryProperties(allocator, &properties);
			if (!properties)
			{
				return 0;
			}

			std::array<VmaBudget, VK_MAX_MEMORY_HEAPS> budgets{};
			vmaGetHeapBudgets(allocator, budgets.data());

			u64 total = 0;
			for (u32 i = 0; i < properties->memoryHeapCount && i < VK_MAX_MEMORY_HEAPS; ++i)
			{
				total += budgets[i].usage;
			}

			return total;
		}

		// True when one of the request's heaps has a cap that a new VMA block would exceed: then VMA itself may
		// have refused the allocation, and the failure says nothing about the window.
		bool ps4_near_heap_cap(VmaAllocator allocator, const memory_allocation_request& request)
		{
			const VkPhysicalDeviceMemoryProperties* properties = nullptr;
			vmaGetMemoryProperties(allocator, &properties);
			if (!properties || !request.memory_type)
			{
				return true;
			}

			std::array<VmaBudget, VK_MAX_MEMORY_HEAPS> budgets{};
			vmaGetHeapBudgets(allocator, budgets.data());

			for (const u32 type : *request.memory_type)
			{
				if (type >= properties->memoryTypeCount)
				{
					continue;
				}

				const u32 heap = properties->memoryTypes[type].heapIndex;
				if (heap >= VK_MAX_MEMORY_HEAPS)
				{
					continue;
				}

				const u64 limit = s_ps4_heap_limits[heap].load(std::memory_order_relaxed);
				if (limit != u64{ VK_WHOLE_SIZE } && budgets[heap].usage + std::max(request.size, ps4_vma_block_bytes) > limit)
				{
					return true;
				}
			}

			return false;
		}

		// Text of vram.flag, or nothing when the file is missing. Like the boot policy's reader, a file larger than
		// vram_flag_max_bytes is ignored as a whole ('note' then says so, for the caps log line).
		std::optional<std::string> ps4_read_vram_flag(std::string& note)
		{
			std::FILE* file = std::fopen(utils::ps4::gpu::vram_flag_path, "rb");
			if (!file)
			{
				return std::nullopt;
			}

			std::string text(utils::ps4::gpu::vram_flag_max_bytes + 1, '\0');
			const usz length = std::fread(text.data(), 1, text.size(), file);
			std::fclose(file);

			if (length > utils::ps4::gpu::vram_flag_max_bytes)
			{
				note = "; vram.flag is larger than 4 KiB and was ignored";
				return std::nullopt;
			}

			text.resize(length);
			return text;
		}

		// 0.34: 'window_mb=' from vram.flag (an A/B switch for the window budget), or 0 for the default budget.
		// 'note' says what was found, for the caps log line.
		u32 ps4_read_window_budget_mib(const std::optional<std::string>& text, std::string& note)
		{
			const u32 window_mib = text ? utils::ps4::gpu::parse_flag_mib(*text, "window_mb") : 0;
			if (!window_mib)
			{
				return 0;
			}

			if (window_mib < utils::ps4::gpu::window_budget_min_mib || window_mib > utils::ps4::gpu::window_budget_max_mib)
			{
				note = fmt::format(" (vram.flag window_mb=%u ignored, allowed %u..%u)", window_mib,
					utils::ps4::gpu::window_budget_min_mib, utils::ps4::gpu::window_budget_max_mib);
				return 0;
			}

			note = " from vram.flag window_mb";
			return window_mib;
		}

		// 'host_mb=' from vram.flag, or 0 for no host cap. 'note' says what was found, for the caps log line.
		u32 ps4_read_host_cap_mib(const std::optional<std::string>& text, std::string& note)
		{
			if (!text)
			{
				return 0;
			}

			const u32 host_mib = utils::ps4::gpu::parse_flag_mib(*text, "host_mb");
			if (!host_mib)
			{
				note = "; vram.flag has no valid host_mb";
				return 0;
			}

			if (host_mib < utils::ps4::gpu::host_cap_min_mib || host_mib > utils::ps4::gpu::host_cap_max_mib)
			{
				note = fmt::format("; vram.flag host_mb=%u ignored (allowed %u..%u)", host_mib,
					utils::ps4::gpu::host_cap_min_mib, utils::ps4::gpu::host_cap_max_mib);
				return 0;
			}

			note = "; host cap from vram.flag";
			return host_mib;
		}

		// RADV reports a 1322 MiB device-local heap and a 661 MiB host heap on this console, but both live in one
		// orbis-drm window of about 991 MiB. Without explicit heap limits VMA never reports out-of-memory before
		// that window is exhausted, so RPCS3's cache trimming never starts in time. Every heap therefore gets an
		// explicit entry: device-local heaps the configured vram_allocation_limit (the boot policy clamps it), host
		// heaps 'host_mb=' from vram.flag when present. A limit at or above the heap size is left as VK_WHOLE_SIZE.
		void ps4_fill_heap_limits(VkPhysicalDevice pdev, u64 device_limit, std::vector<VkDeviceSize>& heap_limits)
		{
			VkPhysicalDeviceMemoryProperties memory_properties;
			vkGetPhysicalDeviceMemoryProperties(pdev, &memory_properties);

			std::string flag_note;
			const std::optional<std::string> flag_text = ps4_read_vram_flag(flag_note);
			const u32 host_mib = ps4_read_host_cap_mib(flag_text, flag_note);
			const u64 host_limit = host_mib ? host_mib * 0x100000ull : u64{ VK_WHOLE_SIZE };
			std::string window_note;
			const u32 window_mib = ps4_read_window_budget_mib(flag_text, window_note);
			const u64 window_budget = window_mib ? window_mib * 0x100000ull : ps4_window_budget_default;

			std::string heaps;
			u64 device_cap = 0;
			u64 host_cap = 0;
			heap_limits.clear();

			for (u32 i = 0; i < memory_properties.memoryHeapCount; ++i)
			{
				const auto& heap = memory_properties.memoryHeaps[i];
				const bool device_local = !!(heap.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT);
				const u64 wanted = device_local ? device_limit : host_limit;
				const u64 limit = (wanted && wanted < heap.size) ? wanted : u64{ VK_WHOLE_SIZE };
				heap_limits.push_back(limit);
				if (i < VK_MAX_MEMORY_HEAPS)
				{
					s_ps4_heap_limits[i].store(limit, std::memory_order_relaxed);
				}

				fmt::append(heaps, "%sheap%u %s %llu MiB", i ? ", " : "", i, device_local ? "dev" : "host", heap.size / 0x100000);
				if (limit != VK_WHOLE_SIZE)
				{
					fmt::append(heaps, " capped to %llu MiB", limit / 0x100000);
					(device_local ? device_cap : host_cap) = limit;
				}
			}

			s_ps4_device_cap_mib.store(static_cast<u32>(device_cap / 0x100000), std::memory_order_relaxed);
			s_ps4_host_cap_mib.store(static_cast<u32>(host_cap / 0x100000), std::memory_order_relaxed);

			if (host_mib && !host_cap)
			{
				flag_note = fmt::format("; vram.flag host_mb=%u is not below any host heap, so it caps nothing", host_mib);
			}

			// 0.32: every renderer starts from the default window budget.
			// 0.34: or from 'window_mb=' in vram.flag (512..832 MiB), for an A/B of the default.
			s_ps4_window_budget.store(window_budget, std::memory_order_relaxed);

			const std::string line = fmt::format("PS4 VRAM caps: device %s host %s (orbis-drm window 991 MiB); %s%s; "
				"window budget %llu MiB (all heaps)%s, VMA blocks %llu MiB",
				device_cap ? fmt::format("%llu MiB", device_cap / 0x100000) : std::string("none"),
				host_cap ? fmt::format("%llu MiB", host_cap / 0x100000) : std::string("none"),
				heaps, flag_note, window_budget / 0x100000, window_note, ps4_vma_block_bytes / 0x100000);
			rsx_log.notice("%s", line);
			ps4_startup_checkpoint(line.c_str());

			// 0.38: the true window meter for this renderer.
			ps4_meter_probe();
		}

		// 0.31: an allocation the renderer needs ran out of video memory. Whether the trim that follows recovers it
		// or not, the game gets the smaller video memory budget on its next boots. Optional allocations (neither
		// recovered nor required) are skipped: their callers expect them to fail and cope.
		void ps4_note_allocation_failure(VkResult error_code, const memory_allocation_request& request)
		{
			if (error_code != VK_ERROR_OUT_OF_DEVICE_MEMORY || !(request.recover_vmem_on_fail || request.throw_on_fail))
			{
				return;
			}

			// RPCS3.log gets the 1st, 2nd, 4th, 8th... failure: a game at the cap can fail many uploads per frame.
			static std::atomic<u32> s_failures{ 0 };
			if (const u32 count = s_failures.fetch_add(1, std::memory_order_relaxed) + 1; !(count & (count - 1)))
			{
				rsx_log.warning("PS4 VRAM: allocation of %llu KiB ran out of video memory (pool %d, %s; failure %u)", request.size / 1024,
					static_cast<int>(request.pool), request.throw_on_fail ? "required" : "may fail", count);
			}

			ps4_note_vram_pressure(request.throw_on_fail ? "a required allocation ran out of video memory"
				: "an allocation ran out of video memory");

			// 0.32: self-calibration of the window budget. The window ran out while VMA held less than the budget
			// (and no heap cap explains the refusal): RADV holds more of the window than estimated, so trims must
			// start earlier. Only ever lowered, never below the floor.
			// 0.38: with the window meter on, the load already counts RADV's share: no calibration.
			const VmaAllocator allocator = s_ps4_vma.load();
			if (!allocator || ps4_near_heap_cap(allocator, request) || s_ps4_meter_state.load(std::memory_order_relaxed) == 1)
			{
				return;
			}

			const u64 total = ps4_vma_total_bytes(allocator);
			u64 budget = s_ps4_window_budget.load(std::memory_order_relaxed);
			const u64 before = budget;
			if (total > before)
			{
				// Above the budget already: the trims it drives were late, not wrong.
				return;
			}

			const u64 calibrated = std::max(ps4_window_budget_floor, total > ps4_vma_block_bytes ? total - ps4_vma_block_bytes : 0);
			while (calibrated < budget && !s_ps4_window_budget.compare_exchange_weak(budget, calibrated, std::memory_order_relaxed))
			{
			}

			if (calibrated < before)
			{
				char detail[160];
				std::snprintf(detail, sizeof(detail), "window ran out with VMA at %llu MiB; budget %llu -> %llu MiB",
					static_cast<unsigned long long>(total >> 20), static_cast<unsigned long long>(before >> 20),
					static_cast<unsigned long long>(calibrated >> 20));
				rsx_log.warning("PS4 VRAM: %s", detail);
				ps4_fatal_note("VRAM: ", detail);
			}
		}

		// 0.38: admission control. A request of 4 MiB or more that the largest free range of the window cannot hold
		// with 16 MiB to spare first gets a trim handed to the RSX thread's next frame end and the retirement of
		// finished GPU work, at most once per 250 ms. Nothing is evicted here: the caller may be inside the texture
		// cache or an upload.
		void ps4_admit(const memory_allocation_request& request)
		{
			if (!request.recover_vmem_on_fail || request.size < vram_policy::admission_min_request)
			{
				return;
			}

			u64 size = 0, used = 0, hole = 0;
			if (!ps4_window_meter(size, used, hole) || !vram_policy::admission_short(request.size, hole))
			{
				return;
			}

			const u64 now = get_system_time();
			u64 last = s_ps4_last_admission_us.load(std::memory_order_relaxed);
			if ((last && now - last < vram_policy::admission_period_us) ||
				!s_ps4_last_admission_us.compare_exchange_strong(last, now, std::memory_order_relaxed))
			{
				return;
			}

			const bool relieved = ps4_vram_relieve_deferred(rsx::problem_severity::severe);
			u64 hole_after = hole;
			ps4_window_meter(size, used, hole_after, true);

			// RPCS3.log and startup.log get the 1st, 2nd, 4th, 8th... pass.
			if (const u32 count = s_ps4_admission_passes.fetch_add(1, std::memory_order_relaxed) + 1; !(count & (count - 1)))
			{
				char detail[224];
				std::snprintf(detail, sizeof(detail), "admission: request of %llu KiB (pool %d) with the largest free range at %llu MiB: "
					"trim handed to the frame end, finished work retired (%s); largest free range now %llu MiB (pass %u)",
					static_cast<unsigned long long>(request.size >> 10), static_cast<int>(request.pool),
					static_cast<unsigned long long>(hole >> 20), relieved ? "memory returned" : "nothing returned yet",
					static_cast<unsigned long long>(hole_after >> 20), count);
				rsx_log.warning("PS4 VRAM: %s", detail);
				ps4_fatal_note("VRAM: ", detail);
			}
		}

		// 0.38: a required allocation failed after the upstream pressure pass. Each step that returns true is
		// followed by one retry: 0 the deep reclaim on the RSX thread (elsewhere a fatal trim handed to it and finished
		// work retired), 1 the std::new_handler for host memory, 2 the emergency reserve (the game is then stopped).
		thread_local bool t_ps4_in_rescue = false;

		const char* ps4_rescue_step_name(u32 step)
		{
			switch (step)
			{
			case 0: return "the deep reclaim";
			case 1: return "the heap reserve (std::new_handler)";
			default: return "the emergency reserve";
			}
		}

		bool ps4_rescue_step(u32 step, VkResult error_code, const memory_allocation_request& request)
		{
			char why[96];
			std::snprintf(why, sizeof(why), "required allocation of %llu KiB (pool %d)", static_cast<unsigned long long>(request.size >> 10),
				static_cast<int>(request.pool));

			switch (step)
			{
			case 0:
				if (!ps4_vram_deep_reclaim(why))
				{
					ps4_vram_relieve_deferred(rsx::problem_severity::fatal);
				}
				return true;
			case 1:
				if (error_code == VK_ERROR_OUT_OF_HOST_MEMORY)
				{
					if (const std::new_handler handler = std::get_new_handler())
					{
						handler();
						return true;
					}
				}
				return false;
			case 2:
				return ps4_vram_reserve_release(why);
			default:
				return false;
			}
		}

		void ps4_rescue_done(u32 step, const memory_allocation_request& request)
		{
			char detail[192];
			std::snprintf(detail, sizeof(detail), "required allocation of %llu KiB (pool %d) served after %s",
				static_cast<unsigned long long>(request.size >> 10), static_cast<int>(request.pool), ps4_rescue_step_name(step));

			if (const u32 count = s_ps4_rescues.fetch_add(1, std::memory_order_relaxed) + 1; !(count & (count - 1)) || step == 2)
			{
				rsx_log.warning("PS4 VRAM: %s (rescue %u)", detail, count);
				ps4_fatal_note("VRAM: ", detail);
			}

			if (step == 2)
			{
				// The window has nothing left to give: the game ends through the regular stop before the next failure.
				ps4_request_vram_stop(detail);
			}
		}

		void ps4_rescue_failed(VkResult error_code, const memory_allocation_request& request)
		{
			s_ps4_rescue_failures.fetch_add(1, std::memory_order_relaxed);

			char detail[192];
			std::snprintf(detail, sizeof(detail), "required allocation of %llu KiB (pool %d) failed after the deep reclaim and the emergency reserve (VkResult %d)",
				static_cast<unsigned long long>(request.size >> 10), static_cast<int>(request.pool), static_cast<int>(error_code));
			rsx_log.error("PS4 VRAM: %s", detail);

			// The thread error that follows also returns to the XMB; the stop request names the cause first.
			ps4_request_vram_stop(detail);
		}

		// Runs the rescue steps with a retry after each; true when 'attempt' succeeded. Not re-entered by the reclaim's
		// own allocations (those go straight to the reserve).
		template <typename Attempt>
		bool ps4_rescue(VkResult& error_code, const memory_allocation_request& request, Attempt&& attempt)
		{
			const bool nested = t_ps4_in_rescue;
			t_ps4_in_rescue = true;

			for (u32 step = nested ? 1 : 0; step < 3; ++step)
			{
				if (!ps4_rescue_step(step, error_code, request))
				{
					continue;
				}

				if (attempt())
				{
					t_ps4_in_rescue = nested;
					ps4_rescue_done(step, request);
					return true;
				}
			}

			t_ps4_in_rescue = nested;
			ps4_rescue_failed(error_code, request);
			return false;
		}
	}
#endif

	memory_type_info::memory_type_info(u32 index, u64 size)
	{
		push(index, size);
	}
	void memory_type_info::push(u32 index, u64 size)
	{
		type_ids.push_back(index);
		type_sizes.push_back(size);
	}

	memory_type_info::const_iterator memory_type_info::begin() const
	{
		return type_ids.data();
	}

	memory_type_info::const_iterator memory_type_info::end() const
	{
		return type_ids.data() + type_ids.size();
	}

	u32 memory_type_info::first() const
	{
		ensure(!type_ids.empty());
		return type_ids.front();
	}

	size_t memory_type_info::count() const
	{
		return type_ids.size();
	}

	u64 memory_type_info::total_bytes() const
	{
		u64 result = 0;
		for (const auto& size : type_sizes)
		{
			result += size;
		}
		return result;
	}

	memory_type_info::operator bool() const
	{
		return !type_ids.empty();
	}

	bool memory_type_info::operator == (const memory_type_info& other) const
	{
		if (type_ids.size() != other.type_ids.size())
		{
			return false;
		}

		switch (type_ids.size())
		{
		case 1:
			return type_ids[0] == other.type_ids[0];
		case 2:
			return ((type_ids[0] == other.type_ids[0] && type_ids[1] == other.type_ids[1]) ||
					(type_ids[0] == other.type_ids[1] && type_ids[1] == other.type_ids[0]));
		default:
			for (const auto& id : other.type_ids)
			{
				if (std::find(type_ids.begin(), type_ids.end(), id) == type_ids.end())
				{
					return false;
				}
			}
			return true;
		}
	}

	memory_type_info memory_type_info::get(const render_device& dev, u32 access_flags, u32 type_mask) const
	{
		memory_type_info result{};
		for (size_t i = 0; i < type_ids.size(); ++i)
		{
			if (type_mask & (1 << type_ids[i]))
			{
				result.push(type_ids[i], type_sizes[i]);
			}
		}

		if (!result)
		{
			u32 type;
			if (dev.get_compatible_memory_type(type_mask, access_flags, &type))
			{
				result = { type, 0ull };
			}
		}

		return result;
	}

	void memory_type_info::rebalance()
	{
		// Re-order indices with the least used one first.
		// This will avoid constant pressure on the memory budget in low memory systems.

		if (type_ids.size() <= 1)
		{
			// Nothing to do
			return;
		}

		std::vector<std::pair<u32, u64>> free_memory_map;
		const auto num_types = type_ids.size();
		u64 last_free = UINT64_MAX;
		bool to_reorder = false;

		for (u32 i = 0; i < num_types; ++i)
		{
			const auto heap_size = type_sizes[i];
			const auto type_id = type_ids[i];
			ensure(heap_size > 0);

			const u64 used_mem = vmm_get_application_memory_usage({ type_id, 0ull });
			const u64 free_mem = (used_mem >= heap_size) ? 0ull : (heap_size - used_mem);

			to_reorder |= (free_mem > last_free);
			last_free = free_mem;

			free_memory_map.push_back({ i, free_mem });
		}

		if (!to_reorder) [[likely]]
		{
			return;
		}

		ensure(free_memory_map.size() == num_types);
		std::sort(free_memory_map.begin(), free_memory_map.end(), FN(x.second > y.second));

		std::vector<u32> new_type_ids(num_types);
		std::vector<u64> new_type_sizes(num_types);

		for (u32 i = 0; i < num_types; ++i)
		{
			const u32 ref = free_memory_map[i].first;
			new_type_ids[i] = type_ids[ref];
			new_type_sizes[i] = type_sizes[ref];
		}

		type_ids = new_type_ids;
		type_sizes = new_type_sizes;

		rsx_log.warning("Rebalanced memory types successfully");
	}

	mem_allocator_base::mem_allocator_base(const vk::render_device& dev, VkPhysicalDevice /*pdev*/)
		: m_device(dev), m_allocation_flags(0)
	{}

	mem_allocator_vma::mem_allocator_vma(const vk::render_device& dev, VkPhysicalDevice pdev, VkInstance inst)
		: mem_allocator_base(dev, pdev)
	{
		// Initialize stats pool
		std::fill(stats.begin(), stats.end(), VmaBudget{});

		VmaAllocatorCreateInfo allocatorInfo = {};
		allocatorInfo.physicalDevice = pdev;
		allocatorInfo.device = dev;
		allocatorInfo.instance = inst;
		allocatorInfo.vulkanApiVersion = VK_API_VERSION_1_2;

		std::vector<VkDeviceSize> heap_limits;
		const auto vram_allocation_limit = g_cfg.video.vk.vram_allocation_limit * 0x100000ull;
#ifdef RPCS3_PS4
		ps4_fill_heap_limits(pdev, vram_allocation_limit, heap_limits);
		allocatorInfo.pHeapSizeLimit = heap_limits.data();
#else
		if (vram_allocation_limit < dev.get_memory_mapping().device_local_total_bytes)
		{
			VkPhysicalDeviceMemoryProperties memory_properties;
			vkGetPhysicalDeviceMemoryProperties(pdev, &memory_properties);
			for (u32 i = 0; i < memory_properties.memoryHeapCount; ++i)
			{
				const u64 max_sz = (memory_properties.memoryHeaps[i].flags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
					? vram_allocation_limit
					: VK_WHOLE_SIZE;

				heap_limits.push_back(max_sz);
			}
			allocatorInfo.pHeapSizeLimit = heap_limits.data();
		}
#endif

		CHECK_RESULT(vmaCreateAllocator(&allocatorInfo, &m_allocator));
#ifdef RPCS3_PS4
		s_ps4_vma.store(m_allocator);

		s_ps4_vma_keep_empty.store(ps4_flag_present("/data/rpcs3/vma-keep-empty.flag"), std::memory_order_relaxed);
		ps4_startup_checkpoint(s_ps4_vma_keep_empty.load(std::memory_order_relaxed)
			? "PS4 VRAM: VMA keeps one spare empty block per memory type (vma-keep-empty.flag)"
			: "PS4 VRAM: VMA releases a block it empties while the window load is 85% or more, and severe or fatal ratings release every empty block "
			  "(vma-keep-empty.flag keeps one spare per memory type); a fatal rating needs the allocations over 95% or no 64 MiB free range");
#endif

		// Allow fastest possible allocation on start
		set_fastest_allocation_flags();

		// Determine the rebar heap. We will exclude it from stats
		const auto& memory_map = dev.get_memory_mapping();
		if (memory_map.device_bar_total_bytes !=
			memory_map.device_local_total_bytes)
		{
			for (u32 i = 0; i < ::size32(memory_map.heaps); ++i)
			{
				const auto& heap = memory_map.heaps[i];
				if ((heap.flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) &&
					heap.size == memory_map.device_bar_total_bytes)
				{
					m_rebar_heap_idx = i;
					break;
				}
			}
		}
	}

	void mem_allocator_vma::destroy()
	{
#ifdef RPCS3_PS4
		if (VmaAllocator current = m_allocator; s_ps4_vma.compare_exchange_strong(current, nullptr))
		{
			s_ps4_device_cap_mib.store(0, std::memory_order_relaxed);
			s_ps4_host_cap_mib.store(0, std::memory_order_relaxed);

			// 0.38: the next renderer probes the meter again; the published state describes no renderer.
			s_ps4_meter_state.store(0, std::memory_order_relaxed);
			utils::ps4::vram::publish_window({});
		}
#endif
		vmaDestroyAllocator(m_allocator);
	}

	mem_allocator_vk::mem_handle_t mem_allocator_vma::alloc(const memory_allocation_request& request)
	{
		VmaAllocation vma_alloc;
		VkMemoryRequirements mem_req = {};
		VmaAllocationCreateInfo create_info = {};
		VkResult error_code = VK_ERROR_UNKNOWN;

		auto do_vma_alloc = [&]() -> std::tuple<VkResult, u32>
		{
			for (const auto& memory_type_index : *request.memory_type)
			{
				mem_req.memoryTypeBits = 1u << memory_type_index;
				mem_req.size = rsx::align2(request.size, request.alignment);
				mem_req.alignment = request.alignment;
				create_info.memoryTypeBits = 1u << memory_type_index;
				create_info.flags = m_allocation_flags;

				error_code = vmaAllocateMemory(m_allocator, &mem_req, &create_info, &vma_alloc, nullptr);
				if (error_code == VK_SUCCESS)
				{
					return { VK_SUCCESS, memory_type_index };
				}
			}

			return { error_code, ~0u };
		};

#ifdef RPCS3_PS4
		ps4_admit(request);
#endif

		// On successful allocation, simply tag the transaction and carry on.
		{
			const auto [status, type] = do_vma_alloc();
			if (status == VK_SUCCESS)
			{
				vmm_notify_memory_allocated(vma_alloc, type, request.size, request.pool);
				return vma_alloc;
			}
		}

#ifdef RPCS3_PS4
		ps4_note_allocation_failure(error_code, request);
#endif

		if (request.recover_vmem_on_fail)
		{
			const auto severity = (request.throw_on_fail) ? rsx::problem_severity::fatal : rsx::problem_severity::severe;
			if (error_code == VK_ERROR_OUT_OF_DEVICE_MEMORY &&
				vmm_handle_memory_pressure(severity))
			{
				// Out of memory. Try again.
				const auto [status, type] = do_vma_alloc();
				if (status == VK_SUCCESS)
				{
					rsx_log.warning("Renderer ran out of video memory but successfully recovered.");
#ifdef RPCS3_PS4
					// 0.32: off the RSX thread or inside an uninterruptible section the pressure pass evicts nothing;
					// it retires finished GPU work and runs the garbage collector (VKResourceManager.cpp).
					rsx_log.warning("PS4 VRAM: allocation of %llu KiB recovered after a %s pressure pass (pool %d)", request.size / 1024,
						severity == rsx::problem_severity::fatal ? "fatal" : "severe", static_cast<int>(request.pool));
#endif
					vmm_notify_memory_allocated(vma_alloc, type, request.size, request.pool);
					return vma_alloc;
				}
			}
		}

		if (!request.throw_on_fail)
		{
			return VK_NULL_HANDLE;
		}

#ifdef RPCS3_PS4
		// 0.38: deep reclaim, host reserve and the emergency reserve before a required allocation is given up.
		if (u32 rescued_type = ~0u; ps4_rescue(error_code, request, [&]()
			{
				const auto [status, type] = do_vma_alloc();
				rescued_type = type;
				return status == VK_SUCCESS;
			}))
		{
			vmm_notify_memory_allocated(vma_alloc, rescued_type, request.size, request.pool);
			return vma_alloc;
		}
#endif

		die_with_error(error_code);
		fmt::throw_exception("Unreachable! Error_code=0x%x", static_cast<u32>(error_code));
	}

	void mem_allocator_vma::free(mem_handle_t mem_handle)
	{
		vmm_notify_memory_freed(mem_handle);
		vmaFreeMemory(m_allocator, static_cast<VmaAllocation>(mem_handle));
	}

	void* mem_allocator_vma::map(mem_handle_t mem_handle, u64 offset, u64 /*size*/)
	{
		void* data = nullptr;

		CHECK_RESULT(vmaMapMemory(m_allocator, static_cast<VmaAllocation>(mem_handle), &data));

		// Add offset
		data = static_cast<u8*>(data) + offset;
		return data;
	}

	void mem_allocator_vma::unmap(mem_handle_t mem_handle)
	{
		vmaUnmapMemory(m_allocator, static_cast<VmaAllocation>(mem_handle));
	}

	VkDeviceMemory mem_allocator_vma::get_vk_device_memory(mem_handle_t mem_handle)
	{
		if (!mem_handle)
		{
			return VK_NULL_HANDLE;
		}

		VmaAllocationInfo alloc_info;
		vmaGetAllocationInfo(m_allocator, static_cast<VmaAllocation>(mem_handle), &alloc_info);
		return alloc_info.deviceMemory;
	}

	u64 mem_allocator_vma::get_vk_device_memory_offset(mem_handle_t mem_handle)
	{
		VmaAllocationInfo alloc_info;

		vmaGetAllocationInfo(m_allocator, static_cast<VmaAllocation>(mem_handle), &alloc_info);
		return alloc_info.offset;
	}

	f32 mem_allocator_vma::get_memory_usage()
	{
		vmaGetHeapBudgets(m_allocator, stats.data());

		// Filter out the Re-BAR heap
		if (::size32(stats) > m_rebar_heap_idx)
		{
			stats[m_rebar_heap_idx].budget = 0;
		}

		float max_usage = 0.f;
		for (const auto& info : stats)
		{
			if (!info.budget)
			{
				break;
			}

			const float this_usage = (info.usage * 100.f) / info.budget;
			max_usage = std::max(max_usage, this_usage);
		}

		return max_usage;
	}

	void mem_allocator_vma::set_safest_allocation_flags()
	{
		m_allocation_flags = VMA_ALLOCATION_CREATE_STRATEGY_BEST_FIT_BIT;
	}

	void mem_allocator_vma::set_fastest_allocation_flags()
	{
		m_allocation_flags = VMA_ALLOCATION_CREATE_STRATEGY_MIN_TIME_BIT;
	}

	mem_allocator_vk::mem_handle_t mem_allocator_vk::alloc(const memory_allocation_request& request)
	{
		VkResult error_code = VK_ERROR_UNKNOWN;
		VkDeviceMemory memory;

		VkMemoryAllocateInfo info = {};
		info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		info.allocationSize = request.size;

		auto do_vk_alloc = [&]() -> std::tuple<VkResult, u32>
		{
			for (const auto& memory_type_index : *request.memory_type)
			{
				info.memoryTypeIndex = memory_type_index;
				error_code = vkAllocateMemory(m_device, &info, nullptr, &memory);
				if (error_code == VK_SUCCESS)
				{
					return { error_code, memory_type_index };
				}
			}

			return { error_code, ~0u };
		};

#ifdef RPCS3_PS4
		ps4_admit(request);
#endif

		{
			const auto [status, type] = do_vk_alloc();
			if (status == VK_SUCCESS)
			{
				vmm_notify_memory_allocated(memory, type, request.size, request.pool);
				return memory;
			}
		}

#ifdef RPCS3_PS4
		ps4_note_allocation_failure(error_code, request);
#endif

		if (request.recover_vmem_on_fail)
		{
			const auto severity = (request.throw_on_fail) ? rsx::problem_severity::fatal : rsx::problem_severity::severe;
			if (error_code == VK_ERROR_OUT_OF_DEVICE_MEMORY &&
				vmm_handle_memory_pressure(severity))
			{
				// Out of memory. Try again.
				const auto [status, type] = do_vk_alloc();
				if (status == VK_SUCCESS)
				{
					rsx_log.warning("Renderer ran out of video memory but successfully recovered.");
					vmm_notify_memory_allocated(memory, type, request.size, request.pool);
					return memory;
				}
			}
		}

		if (!request.throw_on_fail)
		{
			return VK_NULL_HANDLE;
		}

#ifdef RPCS3_PS4
		// 0.38: the same rescue steps as for the VMA allocator.
		if (u32 rescued_type = ~0u; ps4_rescue(error_code, request, [&]()
			{
				const auto [status, type] = do_vk_alloc();
				rescued_type = type;
				return status == VK_SUCCESS;
			}))
		{
			vmm_notify_memory_allocated(memory, rescued_type, request.size, request.pool);
			return memory;
		}
#endif

		die_with_error(error_code);
		fmt::throw_exception("Unreachable! Error_code=0x%x", static_cast<u32>(error_code));
	}

	void mem_allocator_vk::free(mem_handle_t mem_handle)
	{
		vmm_notify_memory_freed(mem_handle);
		vkFreeMemory(m_device, static_cast<VkDeviceMemory>(mem_handle), nullptr);
	}

	void* mem_allocator_vk::map(mem_handle_t mem_handle, u64 offset, u64 size)
	{
		void* data = nullptr;
		CHECK_RESULT(vkMapMemory(m_device, static_cast<VkDeviceMemory>(mem_handle), offset, std::max<u64>(size, 1u), 0, &data));
		return data;
	}

	void mem_allocator_vk::unmap(mem_handle_t mem_handle)
	{
		vkUnmapMemory(m_device, static_cast<VkDeviceMemory>(mem_handle));
	}

	VkDeviceMemory mem_allocator_vk::get_vk_device_memory(mem_handle_t mem_handle)
	{
		return static_cast<VkDeviceMemory>(mem_handle);
	}

	u64 mem_allocator_vk::get_vk_device_memory_offset(mem_handle_t /*mem_handle*/)
	{
		return 0;
	}

	f32 mem_allocator_vk::get_memory_usage()
	{
		return 0.f;
	}

	mem_allocator_base* get_current_mem_allocator()
	{
		return g_render_device->get_allocator();
	}

	memory_block::memory_block(VkDevice dev, const memory_allocation_request& alloc_request)
		: m_device(dev), m_size(alloc_request.size)
	{
		m_mem_allocator = get_current_mem_allocator();
		m_mem_handle    = m_mem_allocator->alloc(alloc_request);
	}

	memory_block::~memory_block()
	{
		if (m_mem_allocator && m_mem_handle)
		{
			m_mem_allocator->free(m_mem_handle);
		}
	}

	memory_block_host::memory_block_host(VkDevice dev, void* host_pointer, u64 size, const memory_type_info& memory_type) :
		m_device(dev), m_mem_handle(VK_NULL_HANDLE), m_host_pointer(host_pointer)
	{
		VkMemoryAllocateInfo alloc_info{};
		VkImportMemoryHostPointerInfoEXT import_info{};

		alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		alloc_info.memoryTypeIndex = memory_type.first();
		alloc_info.allocationSize = size;
		alloc_info.pNext = &import_info;

		import_info.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT;
		import_info.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
		import_info.pHostPointer = host_pointer;

		CHECK_RESULT(vkAllocateMemory(m_device, &alloc_info, nullptr, &m_mem_handle));
	}

	memory_block_host::~memory_block_host()
	{
		vkFreeMemory(m_device, m_mem_handle, nullptr);
	}

	VkDeviceMemory memory_block_host::get_vk_device_memory()
	{
		return m_mem_handle;
	}

	u64 memory_block_host::get_vk_device_memory_offset()
	{
		return 0ull;
	}

	void* memory_block_host::map(u64 offset, u64 /*size*/)
	{
		return reinterpret_cast<char*>(m_host_pointer) + offset;
	}

	void memory_block_host::unmap()
	{
		// NOP
	}

	VkDeviceMemory memory_block::get_vk_device_memory()
	{
		return m_mem_allocator->get_vk_device_memory(m_mem_handle);
	}

	u64 memory_block::get_vk_device_memory_offset()
	{
		return m_mem_allocator->get_vk_device_memory_offset(m_mem_handle);
	}

	u64 memory_block::size() const
	{
		return m_size;
	}

	void* memory_block::map(u64 offset, u64 size)
	{
		return m_mem_allocator->map(m_mem_handle, offset, size);
	}

	void memory_block::unmap()
	{
		m_mem_allocator->unmap(m_mem_handle);
	}

#ifdef RPCS3_PS4
	void ps4_query_vram_heaps(utils::ps4::gpu::vram_snapshot& out)
	{
		out.device_cap_mib = s_ps4_device_cap_mib.load(std::memory_order_relaxed);
		out.host_cap_mib = s_ps4_host_cap_mib.load(std::memory_order_relaxed);
		out.heap_count = 0;
		out.device_local_mask = 0;

		// With the built-in allocator (VMA disabled in the config) there are no per-heap numbers.
		const VmaAllocator allocator = s_ps4_vma.load();
		if (!allocator)
		{
			return;
		}

		const VkPhysicalDeviceMemoryProperties* properties = nullptr;
		vmaGetMemoryProperties(allocator, &properties);
		if (!properties)
		{
			return;
		}

		// Without VK_EXT_memory_budget VMA reports usage = bytes of its device memory blocks and budget = 80% of the
		// heap size (or of its cap), the same numbers get_memory_usage() turns into the load percentage.
		std::array<VmaBudget, VK_MAX_MEMORY_HEAPS> budgets{};
		vmaGetHeapBudgets(allocator, budgets.data());

		const u32 count = std::min<u32>({ properties->memoryHeapCount, utils::ps4::gpu::max_heaps, ::size32(budgets) });
		for (u32 i = 0; i < count; ++i)
		{
			out.heap_usage[i] = budgets[i].usage;
			out.heap_budget[i] = budgets[i].budget;

			if (properties->memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT)
			{
				out.device_local_mask |= 1u << i;
			}
		}

		out.heap_count = count;
	}

	f32 ps4_window_load_percent()
	{
		const VmaAllocator allocator = s_ps4_vma.load();
		const u64 budget = s_ps4_window_budget.load(std::memory_order_relaxed);
		if (!allocator || !budget)
		{
			return 0.f;
		}

		// 0.38: with the meter, the whole window's use (RADV's share included) counts as well.
		u64 size = 0, used = 0, hole = 0;
		if (!ps4_window_meter(size, used, hole))
		{
			size = used = 0;
		}

		return vram_policy::window_load_percent(ps4_vma_total_bytes(allocator), budget, used, size,
			s_ps4_reserve_bytes.load(std::memory_order_relaxed));
	}

	// 0.39: the window is under pressure for VMA's empty blocks (VmaBlockVector::Free hunk). From the last published
	// meter sample only: any thread, lock-free, no meter query (a free can run under the callers' locks).
	bool ps4_vma_window_pressure() noexcept
	{
		if (s_ps4_vma_keep_empty.load(std::memory_order_relaxed) || !s_ps4_vma.load())
		{
			return false;
		}

		const vram_policy::window_state w = vram_policy::current_window();
		return vram_policy::window_load_percent(w.vma, s_ps4_window_budget.load(std::memory_order_relaxed), w.used, w.size, w.reserve) >=
			vram_policy::vma_release_load_percent;
	}

	// 0.39: releases every empty VMA block (severe and fatal ratings, VKResourceManager.cpp). Returns the bytes freed.
	// RSX thread.
	u64 ps4_vma_free_empty_blocks()
	{
		const VmaAllocator allocator = s_ps4_vma.load();
		if (!allocator || s_ps4_vma_keep_empty.load(std::memory_order_relaxed))
		{
			return 0;
		}

		return vmaPs4FreeEmptyBlocks(allocator);
	}

	// 0.39: a fatal rating of the window that a trim can change (util/ps4_vram_policy.hpp fatal_load): the allocations,
	// not VMA's blocks, against the budget and the window, or the largest free range. True without VMA. Any thread.
	bool ps4_fatal_rating_confirmed()
	{
		const VmaAllocator allocator = s_ps4_vma.load();
		const u64 budget = s_ps4_window_budget.load(std::memory_order_relaxed);
		if (!allocator || !budget)
		{
			return true;
		}

		const VkPhysicalDeviceMemoryProperties* properties = nullptr;
		vmaGetMemoryProperties(allocator, &properties);
		if (!properties)
		{
			return true;
		}

		std::array<VmaBudget, VK_MAX_MEMORY_HEAPS> budgets{};
		vmaGetHeapBudgets(allocator, budgets.data());

		u64 blocks = 0, allocated = 0;
		for (u32 i = 0; i < properties->memoryHeapCount && i < VK_MAX_MEMORY_HEAPS; ++i)
		{
			blocks += budgets[i].usage;
			allocated += budgets[i].statistics.allocationBytes;
		}

		u64 size = 0, used = 0, hole = 0;
		if (!ps4_window_meter(size, used, hole))
		{
			size = used = hole = 0;
		}

		return vram_policy::fatal_load(allocated, blocks, budget, used, size, s_ps4_reserve_bytes.load(std::memory_order_relaxed), hole);
	}

	u32 ps4_window_budget_mib()
	{
		return static_cast<u32>(s_ps4_window_budget.load(std::memory_order_relaxed) / 0x100000);
	}

	bool ps4_window_meter(u64& size, u64& used, u64& largest_hole, bool force)
	{
		if (s_ps4_meter_state.load(std::memory_order_relaxed) != 1)
		{
			return false;
		}

		const u64 now = get_system_time();
		u64 last = s_ps4_meter_sample_us.load(std::memory_order_relaxed);
		if ((force || now - last >= vram_policy::meter_period_us) &&
			s_ps4_meter_sample_us.compare_exchange_strong(last, now, std::memory_order_relaxed))
		{
			if (!ps4_meter_query())
			{
				return false;
			}

			vram_policy::window_state state{};
			state.time_us = now;
			state.size = s_ps4_meter_size.load(std::memory_order_relaxed);
			state.used = s_ps4_meter_used.load(std::memory_order_relaxed);
			state.largest_hole = s_ps4_meter_hole.load(std::memory_order_relaxed);
			state.reserve = s_ps4_reserve_bytes.load(std::memory_order_relaxed);
			if (const VmaAllocator allocator = s_ps4_vma.load())
			{
				state.vma = ps4_vma_total_bytes(allocator);
			}
			vram_policy::publish_window(state);
		}

		size = s_ps4_meter_size.load(std::memory_order_relaxed);
		used = s_ps4_meter_used.load(std::memory_order_relaxed);
		largest_hole = s_ps4_meter_hole.load(std::memory_order_relaxed);
		return size != 0;
	}

	void ps4_vram_reserve_arm(VkDevice dev, u32 memory_type)
	{
		ps4_vram_reserve_disarm();

		if (ps4_flag_present("/data/rpcs3/vram-reserve-off.flag"))
		{
			ps4_startup_checkpoint("PS4 VRAM: no emergency reserve (vram-reserve-off.flag); a required allocation that still fails after the deep reclaim ends the game");
			return;
		}

		VkMemoryAllocateInfo info{};
		info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
		info.allocationSize = vram_policy::emergency_reserve_bytes;
		info.memoryTypeIndex = memory_type;

		VkDeviceMemory memory = VK_NULL_HANDLE;
		if (const VkResult result = vkAllocateMemory(dev, &info, nullptr, &memory); result != VK_SUCCESS || !memory)
		{
			ps4_startup_checkpoint(fmt::format("PS4 VRAM: emergency reserve not taken (VkResult %d)", static_cast<int>(result)).c_str());
			return;
		}

		s_ps4_reserve_device.store(dev);
		s_ps4_reserve_bytes.store(vram_policy::emergency_reserve_bytes, std::memory_order_relaxed);
		s_ps4_reserve_memory.store(memory);
		ps4_startup_checkpoint(fmt::format("PS4 VRAM: emergency reserve of %llu MiB held in the GPU window (memory type %u); "
			"released only for a required allocation that failed after the deep reclaim, then the game is stopped (vram-reserve-off.flag disables it)",
			vram_policy::emergency_reserve_bytes >> 20, memory_type).c_str());
	}

	void ps4_vram_reserve_disarm()
	{
		if (const VkDeviceMemory memory = s_ps4_reserve_memory.exchange(VK_NULL_HANDLE))
		{
			vkFreeMemory(s_ps4_reserve_device.load(), memory, nullptr);
		}

		s_ps4_reserve_bytes.store(0, std::memory_order_relaxed);
		s_ps4_reserve_device.store(VK_NULL_HANDLE);
	}

	bool ps4_vram_reserve_release(const char* why)
	{
		const VkDeviceMemory memory = s_ps4_reserve_memory.exchange(VK_NULL_HANDLE);
		if (!memory)
		{
			return false;
		}

		vkFreeMemory(s_ps4_reserve_device.load(), memory, nullptr);
		s_ps4_reserve_bytes.store(0, std::memory_order_relaxed);

		u64 size = 0, used = 0, hole = 0;
		ps4_window_meter(size, used, hole, true);

		char detail[224];
		std::snprintf(detail, sizeof(detail), "emergency reserve of %llu MiB released for %s; largest free range now %llu MiB",
			static_cast<unsigned long long>(vram_policy::emergency_reserve_bytes >> 20), why ? why : "an allocation",
			static_cast<unsigned long long>(hole >> 20));
		rsx_log.error("PS4 VRAM: %s", detail);
		ps4_fatal_note("VRAM: ", detail);
		return true;
	}

	u64 ps4_vram_reserve_held()
	{
		return s_ps4_reserve_bytes.load(std::memory_order_relaxed);
	}

	void ps4_request_vram_stop(const char* why) noexcept
	{
		if (!vram_policy::request_stop(vram_policy::stop_cause::vram, why))
		{
			return;
		}

		// Also notes vram=low for the game's next boots (adapt).
		ps4_note_vram_pressure("a required allocation could not be served");
		ps4_fatal_note("VRAM: graceful stop requested (game stopped, back to the XMB): ", why ? why : "");
		ps4_notify(utils::ps4::i18n::tr(utils::ps4::i18n::str::TOAST_RPCS3_MEMORIA_VIDEO_ESGOTADA_JOGO));

		// The frontend's regular stop (as L1+R1+Triangle): GracefulShutdown, then the XMB.
		ps4_request_return_to_menu();
	}

	void ps4_vram_counters(u32& admission_passes, u32& rescues, u32& rescue_failures)
	{
		admission_passes = s_ps4_admission_passes.load(std::memory_order_relaxed);
		rescues = s_ps4_rescues.load(std::memory_order_relaxed);
		rescue_failures = s_ps4_rescue_failures.load(std::memory_order_relaxed);
	}
#endif
}

#ifdef RPCS3_PS4
// 3rdparty vk_mem_alloc.h (VmaBlockVector::Free): release a block that a free empties.
bool vma_ps4_release_empty_blocks() noexcept
{
	return vk::ps4_vma_window_pressure();
}
#endif
