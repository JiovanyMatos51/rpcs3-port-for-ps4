#pragma once

// 0.38: memory rules of the RSX shader and pipeline caches (header-only, no Vulkan headers; host test:
// ps4-port/tests/rsx_programs_test.cpp).
// - Shader interpreter (VKShaderInterpreter.cpp): a variant missing from the cache is linked asynchronously on its
//   first use, in its generic form (compatible_opts: every non-exclusive option enabled), which serves every variant
//   with the same exclusive options; at most interpreter_pending_max such links are queued at a time. The cache keeps
//   interpreter_live_cap pipelines; the least recently bound are evicted first, never the bound one.
// - Disk shader cache preload (rsx_cache.h): at most preload_max pipelines, newest file time first. A pipeline compiled
//   at run time rewrites its file, so a pipeline still in use comes back into the newest set.
// - Graphics pipelines (ProgramStateCache.h): above pipeline_live_cap, pipelines unbound for pipeline_idle_s are
//   destroyed, oldest first, down to pipeline_live_target (never the last one handed to the renderer). A pipeline the
//   driver refused after the reclaim and retry stays "not ready" (interpreter or skipped draw) and is compiled again
//   after pipeline_refused_retry_s (the refusal time is kept in its placeholder entry). A shader whose module the driver
//   refused stays cached with its SPIR-V; its pipelines are not ready and the module is created again at most once
//   per pipeline_refused_retry_s.
// - Descriptor pools (VKProgramPipeline.cpp): a table whose set holds interpreter_set_samplers or more combined image
//   samplers (the interpreter's 4 x 16 texture arrays, ~6 KiB of descriptors per set) grows its pools to
//   interpreter_pool_max_sets sets instead of 1024; pools of tables not committed since the previous sweep are
//   released, least recently used first, while the live pools' estimated GPU window exceeds pool_window_cap.
// - Pipeline compile jobs (VKPipelineCompiler.cpp, RSX.W threads): 0.39: a job takes one of the memory governor's
//   compile slots only from memory level compile_slot_level up (0.38: at every level, serialised with PPU/SPU compiles
//   from level 1); shader interpreter links run on a priority lane drained before the game pipeline queue.
// - 0.39: a shader interpreter draw without textures whose bound program declares the texture arrays (generic form)
//   binds the null-texture environment, so no sampler slot of that program is ever committed empty.
// - 0.39: with shader-translate-async.flag, a new Vulkan vertex/fragment program is translated (decompiled, module
//   created) on the pipeline workers instead of the renderer thread; it is not ready meanwhile (interpreter or skipped
//   draw), and one no worker started within translate_take_back_us is translated by the renderer thread.

#include "util/types.hpp"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <numeric>
#include <vector>

namespace utils::ps4::rsx_programs
{
inline constexpr u32 interpreter_live_cap = 128;
inline constexpr u32 interpreter_live_min = 16;    // lowest cap a memory request can set
inline constexpr u32 interpreter_pending_max = 4;  // asynchronous interpreter links queued at once

inline constexpr u32 preload_max = 3000;

inline constexpr u32 pipeline_live_cap = 4000;
inline constexpr u32 pipeline_live_target = 3000;
inline constexpr u32 pipeline_idle_s = 600;
inline constexpr u32 pipeline_sweep_s = 60;        // at most one pipeline LRU pass per minute
inline constexpr u32 pipeline_refused_retry_s = 30;

inline constexpr u64 pool_window_cap = 48ull << 20;
inline constexpr u32 interpreter_set_samplers = 64;
inline constexpr u32 interpreter_pool_max_sets = 64;

// Kill switches (files under /data/rpcs3, read once per renderer).
inline constexpr char interpreter_sync_flag[] = "/data/rpcs3/interpreter-sync-link.flag";   // 0.37 synchronous first use
inline constexpr char interpreter_no_lru_flag[] = "/data/rpcs3/interpreter-no-lru.flag";    // no interpreter cap
inline constexpr char app_pipeline_cache_flag[] = "/data/rpcs3/vk-app-pipeline-cache.flag"; // 0.37 VkPipelineCache
inline constexpr char preload_all_flag[] = "/data/rpcs3/shader-preload-all.flag";           // whole disk history
inline constexpr char no_pipeline_lru_flag[] = "/data/rpcs3/no-pipeline-lru.flag";          // keep every pipeline
inline constexpr char compile_no_slot_flag[] = "/data/rpcs3/rsx-compile-no-slot.flag";      // RSX.W jobs never wait
inline constexpr char interp_no_lane_flag[] = "/data/rpcs3/interp-no-lane.flag";            // interpreter links in the FIFO
inline constexpr char gl_async_flag[] = "/data/rpcs3/gl-async.flag";                        // OpenGL: one async compile worker
inline constexpr char translate_async_flag[] = "/data/rpcs3/shader-translate-async.flag";   // Vulkan: new programs on the workers

// 0.39 (shader-translate-async.flag): a new program queued for the pipeline workers that none of them has started
// after this long is translated by the renderer thread at its next lookup (all workers may sit in long links).
inline constexpr u64 translate_take_back_us = 250'000;

// Process counters for the backend telemetry line (any thread, relaxed): live graphics pipelines at the last
// eviction check, pipelines evicted as idle, pipelines and shader modules the driver refused, disk cache entries
// left out of the newest-first preload.
// 0.39: interpreter draws without textures bound to the null-texture environment (VKDraw.cpp); combined image sampler
// slots committed without a sampler and written with the null sampler instead (VKProgramPipeline.cpp, 0 expected);
// interpreter links run on the priority lane (VKPipelineCompiler.cpp); OpenGL program links and blocking occlusion
// query reads with their time (the _us totals are exact, the _ms fields follow them).
// New vertex/fragment programs translated (decompile + module) on the renderer thread by a draw lookup, with their
// time and the longest one since the printer last took it (ProgramStateCache.h; the disk preload is not counted);
// programs translated on the pipeline workers instead, and those of them the renderer thread took back and
// translated itself because no worker had started them in time.
struct pipeline_stats
{
    std::atomic<u32> live{0};
    std::atomic<u64> evicted{0};
    std::atomic<u64> refused{0};
    std::atomic<u64> modules_refused{0};
    std::atomic<u32> preload_skipped{0};

    std::atomic<u64> interp_fallback_env{0};
    std::atomic<u64> unbound_sampler_slots{0};
    std::atomic<u64> lane_links{0};
    std::atomic<u64> gl_compiles{0};
    std::atomic<u64> gl_compile_ms{0};
    std::atomic<u64> gl_compile_us{0};
    std::atomic<u64> zcull_query_waits{0};
    std::atomic<u64> zcull_wait_ms{0};
    std::atomic<u64> zcull_wait_us{0};

    std::atomic<u64> rsx_translate{0};
    std::atomic<u64> rsx_translate_us{0};
    std::atomic<u64> rsx_translate_ms{0};
    std::atomic<u64> rsx_translate_max_us{0}; // window maximum: the printer exchanges it with 0
    std::atomic<u64> translate_async{0};
    std::atomic<u64> translate_taken_back{0};
};

inline pipeline_stats g_pipeline_stats;

// Adds a duration to a microsecond total and moves the millisecond field up to it (never down).
inline void add_duration(std::atomic<u64>& total_us, std::atomic<u64>& total_ms, u64 us) noexcept
{
    const u64 ms = (total_us.fetch_add(us, std::memory_order_relaxed) + us) / 1000;
    u64 seen = total_ms.load(std::memory_order_relaxed);
    while (seen < ms && !total_ms.compare_exchange_weak(seen, ms, std::memory_order_relaxed))
    {
    }
}

// Raises a running maximum to value (never lowers it; any thread).
inline void note_max(std::atomic<u64>& maximum, u64 value) noexcept
{
    u64 seen = maximum.load(std::memory_order_relaxed);
    while (seen < value && !maximum.compare_exchange_weak(seen, value, std::memory_order_relaxed))
    {
    }
}

// 0.39: RSX.W pipeline jobs take a memory governor compile slot only from this memory level up (a real shortage);
// below it the workers run in parallel as in 0.37.
inline constexpr int compile_slot_level = 2;

constexpr bool rsx_job_takes_slot(bool slots_enabled, int memory_level) noexcept
{
    return slots_enabled && memory_level >= compile_slot_level;
}

// The shader interpreter's texture environment is bound when the fragment program samples a texture or when the
// bound program declares the texture arrays (the generic form serves texture-less variants too): an array slot left
// unbound would be committed with a null sampler, which RADV dereferences.
constexpr bool interpreter_texture_env_needed(u32 referenced_textures_mask, bool program_declares_textures) noexcept
{
    return referenced_textures_mask != 0 || program_declares_textures;
}

// Entries of one interpreter texture array binding (sampler1D/2D/3D/Cube_array[16]); an array write never goes past it.
inline constexpr u32 interpreter_texture_array_size = 16;

constexpr u32 image_array_write_count(u64 entries) noexcept
{
    return entries < interpreter_texture_array_size ? static_cast<u32>(entries) : interpreter_texture_array_size;
}

// Generic form of an interpreter variant: the base masks are the options the generic shaders always enable
// (ShaderInterpreter.cpp: COMPILER_OPT_ALL_VS_MASK & ~VS_EXCL, COMPILER_OPT_BASE_FS_MASK & ~FS_EXCL).
constexpr u64 compatible_opts(u64 opts, u64 base_vs_mask, u64 base_fs_mask) noexcept
{
    return opts | base_vs_mask | base_fs_mask;
}

// Effective interpreter cap: a request below the minimum is raised to it, 0 means the default.
constexpr u32 interpreter_cap(u32 requested) noexcept
{
    return requested == 0 ? interpreter_live_cap : std::max(requested, interpreter_live_min);
}

// Least recently used victims: indices into 'stamps' (larger = more recent), oldest first, skipping pinned entries,
// so that stamps.size() - victims <= cap where possible.
inline std::vector<std::size_t> lru_victims(const std::vector<u64>& stamps, const std::vector<bool>& pinned, std::size_t cap)
{
    std::vector<std::size_t> result;
    if (stamps.size() <= cap)
    {
        return result;
    }

    std::vector<std::size_t> order(stamps.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return stamps[a] < stamps[b]; });

    std::size_t excess = stamps.size() - cap;
    for (const std::size_t index : order)
    {
        if (!excess)
        {
            break;
        }

        if (index < pinned.size() && pinned[index])
        {
            continue;
        }

        result.push_back(index);
        excess--;
    }

    return result;
}

// Idle pipelines to destroy: none while live <= cap; otherwise those unbound for idle_s or longer (last use in
// seconds; 0 = never bound, counted as idle since boot), oldest first, until live reaches target.
inline std::vector<std::size_t> idle_victims(const std::vector<u32>& last_use_s, u32 now_s, u32 idle_s, std::size_t cap, std::size_t target)
{
    std::vector<std::size_t> result;
    const std::size_t live = last_use_s.size();
    if (live <= cap || live <= target)
    {
        return result;
    }

    std::vector<std::size_t> order;
    order.reserve(live);
    for (std::size_t i = 0; i < live; ++i)
    {
        const u32 last = last_use_s[i];
        if (last <= now_s && now_s - last >= idle_s)
        {
            order.push_back(i);
        }
    }

    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return last_use_s[a] < last_use_s[b]; });

    const std::size_t excess = live - target;
    if (order.size() > excess)
    {
        order.resize(excess);
    }

    return order;
}

// Newest 'max' entries by file time (ties keep directory order); all of them when there are fewer. Returned in
// directory order, so the preload keeps its usual order.
inline std::vector<std::size_t> newest_entries(const std::vector<s64>& mtimes, std::size_t max)
{
    std::vector<std::size_t> order(mtimes.size());
    std::iota(order.begin(), order.end(), std::size_t{0});

    if (mtimes.size() <= max)
    {
        return order;
    }

    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return mtimes[a] > mtimes[b]; });
    order.resize(max);
    std::sort(order.begin(), order.end());
    return order;
}

// Largest set count a descriptor pool of this table grows to.
constexpr u32 pool_max_sets(u32 image_samplers_per_set, u32 default_max_sets) noexcept
{
    return image_samplers_per_set >= interpreter_set_samplers ? std::min(default_max_sets, interpreter_pool_max_sets) : default_max_sets;
}

// A pipeline link reports through one callback: twice when asynchronous (no pipeline yet when queued, then the
// worker's result, in either order), once when inline. True when a report without a pipeline, after
// earlier_null_reports such reports of the same link, means the driver refused it.
constexpr bool null_report_is_refusal(bool async, u32 earlier_null_reports) noexcept
{
    return !async || earlier_null_reports == 1;
}

// A pipeline refused at refused_s (seconds) may be compiled again at now_s.
constexpr bool refused_retry_due(u32 refused_s, u32 now_s) noexcept
{
    return now_s >= refused_s && now_s - refused_s >= pipeline_refused_retry_s;
}

// Descriptor sets of draws (vkutils/descriptors.cpp, RSX thread): a set the driver still refuses after the reclaim
// ladder skips its draw. The full ladder (reclaims, fresh subpools) runs at most once per set_refusal_period_us;
// other refused draws give up after one attempt. Refusals no more than set_refusal_period_us apart form a streak; one
// lasting set_refusal_stop_us requests the graceful stop of the game.
inline constexpr u64 set_refusal_period_us = 1'000'000;
inline constexpr u64 set_refusal_stop_us = 10'000'000;

struct set_refusals
{
    u64 since_us = 0;  // first refusal of the current streak
    u64 last_us = 0;   // last refusal (0 = none)
    u64 ladder_us = 0; // last full ladder (0 = none)
};

constexpr bool set_ladder_due(const set_refusals& s, u64 now_us) noexcept
{
    return !s.ladder_us || now_us < s.ladder_us || now_us - s.ladder_us >= set_refusal_period_us;
}

// Records a refused draw at now_us. True when its streak has lasted set_refusal_stop_us.
constexpr bool note_set_refusal(set_refusals& s, u64 now_us) noexcept
{
    if (!s.last_us || now_us < s.last_us || now_us - s.last_us > set_refusal_period_us)
    {
        s.since_us = now_us;
    }

    s.last_us = now_us;
    return now_us - s.since_us >= set_refusal_stop_us;
}

// Memory shedding of the program caches (VKGSRender.cpp, checked about once per second): while the CPU heap is at
// level 2 or the GPU window at shed_window_percent, the interpreter cache is capped at shed_interpreter_cap and the
// descriptor pools of tables not committed since the previous sweep are released (once per shed_repeat_us). Shedding
// ends once the heap is below level 2 and the window below shed_window_resume_percent.
inline constexpr float shed_window_percent = 90.f;
inline constexpr float shed_window_resume_percent = 85.f;
inline constexpr u32 shed_interpreter_cap = 32;
inline constexpr u64 shed_repeat_us = 10'000'000;

constexpr bool shed_programs(int heap_level, float window_load_percent, bool shedding) noexcept
{
    return heap_level >= 2 || window_load_percent >= (shedding ? shed_window_resume_percent : shed_window_percent);
}
} // namespace utils::ps4::rsx_programs
