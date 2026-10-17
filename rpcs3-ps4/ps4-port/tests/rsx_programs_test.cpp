// Host test of the 0.38 RSX shader/pipeline memory rules in util/ps4_rsx_programs.hpp: generic interpreter form,
// interpreter cap, LRU victims with a pinned entry, idle pipeline victims (cap/target/idle), newest disk cache
// entries, descriptor pool set caps, the refused-pipeline retry and the refusal reports of a link.
// 0.39: the interpreter texture environment of texture-less variants drawn with the generic form, the image array
// write count, the RSX.W compile slot level, the telemetry counters and their duration totals, the window maximum of
// the program translation time and the asynchronous translation constants.
// Build: clang++ -std=c++20 -static -I rpcs3 -I . ps4-port/tests/rsx_programs_test.cpp
#include "util/ps4_rsx_programs.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <set>
#include <thread>
#include <vector>

namespace rp = utils::ps4::rsx_programs;

namespace
{
// ShaderInterpreter.h option bits (copied: the host test does not include RSX headers).
constexpr u64 opt_textures = 1 << 0, opt_depth_export = 1 << 1, opt_f32_export = 1 << 2, opt_packing = 1 << 3, opt_kil = 1 << 4,
    opt_stippling = 1 << 5, opt_flow_ctrl = 1 << 6, opt_rop_remap = 1 << 7, opt_instancing = 1 << 8, opt_vtx_textures = 1 << 9;
constexpr u64 opt_alpha_mask = 0b111111ull << 10;
constexpr u64 all_vs_mask = opt_instancing | opt_vtx_textures;
constexpr u64 base_fs_mask = 0b11111111;
constexpr u64 vs_excl_mask = opt_instancing;
constexpr u64 fs_excl_mask = opt_alpha_mask | opt_stippling | opt_depth_export | opt_f32_export;
constexpr u64 base_vs = all_vs_mask & ~vs_excl_mask;
constexpr u64 base_fs = base_fs_mask & ~fs_excl_mask;

void test_compatible_opts()
{
    // ShaderInterpreter.cpp: compatible_shader_opts = (opt & ~base) | base, per stage.
    for (u64 opts = 0; opts < (1u << 16); ++opts)
    {
        const u64 vs = opts & all_vs_mask;
        const u64 fs = opts & (base_fs_mask | opt_alpha_mask);
        const u64 upstream = ((vs & ~base_vs) | base_vs) | ((fs & ~base_fs) | base_fs);
        assert(rp::compatible_opts(opts, base_vs, base_fs) == upstream);

        // The generic form keeps every exclusive option of the variant and is its own generic form.
        const u64 generic = rp::compatible_opts(opts, base_vs, base_fs);
        assert((generic & (vs_excl_mask | fs_excl_mask)) == (opts & (vs_excl_mask | fs_excl_mask)));
        assert(rp::compatible_opts(generic, base_vs, base_fs) == generic);
    }

    // Instancing and alpha test are exclusive (never added); textures, packing, KIL, flow control, ROP remap and vertex
    // textures always are.
    const u64 g = rp::compatible_opts(0, base_vs, base_fs);
    assert(!(g & opt_instancing) && !(g & opt_alpha_mask) && !(g & opt_stippling) && !(g & opt_depth_export) && !(g & opt_f32_export));
    assert((g & (opt_textures | opt_packing | opt_kil | opt_flow_ctrl | opt_rop_remap | opt_vtx_textures)) ==
        (opt_textures | opt_packing | opt_kil | opt_flow_ctrl | opt_rop_remap | opt_vtx_textures));
}

void test_interpreter_cap()
{
    assert(rp::interpreter_cap(0) == rp::interpreter_live_cap);
    assert(rp::interpreter_cap(1) == rp::interpreter_live_min);
    assert(rp::interpreter_cap(rp::interpreter_live_min) == rp::interpreter_live_min);
    assert(rp::interpreter_cap(32) == 32);
    assert(rp::interpreter_cap(500) == 500);
}

void test_lru_victims()
{
    // At or below the cap: nothing.
    assert(rp::lru_victims({}, {}, 0).empty());
    assert(rp::lru_victims({ 5, 1, 3 }, { false, false, false }, 3).empty());

    // Oldest first.
    {
        const auto v = rp::lru_victims({ 50, 10, 30, 20, 40 }, { false, false, false, false, false }, 3);
        assert((v == std::vector<std::size_t>{ 1, 3 }));
    }

    // The pinned (bound) entry is skipped even when it is the oldest; the next oldest go instead.
    {
        const auto v = rp::lru_victims({ 50, 10, 30, 20, 40 }, { false, true, false, false, false }, 3);
        assert((v == std::vector<std::size_t>{ 3, 2 }));
    }

    // Everything pinned: cannot reach the cap, nothing evicted.
    assert(rp::lru_victims({ 1, 2, 3 }, { true, true, true }, 1).empty());

    // A short pinned vector counts the missing entries as unpinned.
    {
        const auto v = rp::lru_victims({ 3, 2, 1 }, { true }, 1);
        assert((v == std::vector<std::size_t>{ 2, 1 }));
    }

    // Equal stamps keep index order (stable).
    {
        const auto v = rp::lru_victims({ 7, 7, 7, 7 }, {}, 2);
        assert((v == std::vector<std::size_t>{ 0, 1 }));
    }

    // Random: size - victims == cap unless pins prevent it; victims are never pinned and are the oldest unpinned.
    std::mt19937 rng(38);
    for (int round = 0; round < 2000; ++round)
    {
        const std::size_t n = rng() % 300;
        std::vector<u64> stamps(n);
        std::vector<bool> pinned(n);
        std::size_t pins = 0;
        for (std::size_t i = 0; i < n; ++i)
        {
            stamps[i] = rng() % 1000;
            pinned[i] = rng() % 50 == 0;
            pins += pinned[i];
        }

        const std::size_t cap = rng() % 200;
        const auto v = rp::lru_victims(stamps, pinned, cap);
        const std::set<std::size_t> chosen(v.begin(), v.end());
        assert(chosen.size() == v.size());

        if (n <= cap)
        {
            assert(v.empty());
            continue;
        }

        assert(v.size() == std::min(n - cap, n - pins));
        u64 newest_victim = 0;
        for (const std::size_t i : v)
        {
            assert(i < n && !pinned[i]);
            newest_victim = std::max(newest_victim, stamps[i]);
        }

        for (std::size_t i = 0; i < n; ++i)
        {
            if (!pinned[i] && !chosen.count(i))
            {
                assert(stamps[i] >= newest_victim);
            }
        }
    }
}

void test_idle_victims()
{
    constexpr u32 idle = rp::pipeline_idle_s;

    // At or below the cap: nothing, however idle.
    {
        std::vector<u32> last(rp::pipeline_live_cap, 0);
        assert(rp::idle_victims(last, 100000, idle, rp::pipeline_live_cap, rp::pipeline_live_target).empty());
    }

    // Above the cap: idle entries oldest first, down to the target.
    {
        std::vector<u32> last(rp::pipeline_live_cap + 10, 5000);
        last[7] = 100;
        last[3] = 50;
        last[9] = 0; // never bound
        const auto v = rp::idle_victims(last, 5000 + idle - 1, idle, rp::pipeline_live_cap, rp::pipeline_live_target);
        assert((v == std::vector<std::size_t>{ 9, 3, 7 }));
    }

    // Many idle: stops at the target.
    {
        std::vector<u32> last(rp::pipeline_live_cap + 500, 10);
        const auto v = rp::idle_victims(last, 10 + idle, idle, rp::pipeline_live_cap, rp::pipeline_live_target);
        assert(v.size() == last.size() - rp::pipeline_live_target);
    }

    // Exactly idle_s is idle; one second less is not; a stamp in the future (clock reset) is not.
    {
        std::vector<u32> last(rp::pipeline_live_cap + 3, 1000);
        last[0] = 1000 - idle;
        last[1] = 1000 - idle + 1;
        last[2] = 2000;
        const auto v = rp::idle_victims(last, 1000, idle, rp::pipeline_live_cap, rp::pipeline_live_target);
        assert((v == std::vector<std::size_t>{ 0 }));
    }

    // Target above the live count: nothing.
    assert(rp::idle_victims(std::vector<u32>(10, 0), 100000, idle, 5, 20).empty());
}

void test_newest_entries()
{
    // Fewer than max: all of them, in directory order.
    assert((rp::newest_entries({ 3, 1, 2 }, 5) == std::vector<std::size_t>{ 0, 1, 2 }));
    assert(rp::newest_entries({}, 5).empty());

    // The newest 'max', returned in directory order.
    assert((rp::newest_entries({ 10, 50, 20, 40, 30 }, 3) == std::vector<std::size_t>{ 1, 3, 4 }));

    // Ties keep directory order (the earlier one wins).
    assert((rp::newest_entries({ 5, 9, 9, 9, 1 }, 2) == std::vector<std::size_t>{ 1, 2 }));

    // max 0: nothing.
    assert(rp::newest_entries({ 1, 2 }, 0).empty());

    // Random against a reference.
    std::mt19937 rng(3000);
    for (int round = 0; round < 500; ++round)
    {
        const std::size_t n = rng() % 5000;
        std::vector<s64> mtimes(n);
        for (auto& t : mtimes)
        {
            t = static_cast<s64>(rng() % 100000);
        }

        const std::size_t max = rng() % 4000;
        const auto v = rp::newest_entries(mtimes, max);
        assert(v.size() == std::min(n, max));
        for (std::size_t i = 1; i < v.size(); ++i)
        {
            assert(v[i - 1] < v[i]);
        }

        if (n > max && max)
        {
            s64 oldest_kept = INT64_MAX;
            for (const std::size_t i : v)
            {
                oldest_kept = std::min(oldest_kept, mtimes[i]);
            }

            const std::set<std::size_t> kept(v.begin(), v.end());
            for (std::size_t i = 0; i < n; ++i)
            {
                if (!kept.count(i))
                {
                    assert(mtimes[i] <= oldest_kept);
                }
            }
        }
    }
}

void test_pool_max_sets()
{
    // The interpreter's fragment table: 4 arrays of 16 samplers.
    assert(rp::pool_max_sets(64, 1024) == rp::interpreter_pool_max_sets);
    assert(rp::pool_max_sets(80, 1024) == rp::interpreter_pool_max_sets);
    // A game fragment program (up to 16 textures) keeps 1024.
    assert(rp::pool_max_sets(16, 1024) == 1024);
    assert(rp::pool_max_sets(0, 1024) == 1024);
    // Never above the caller's own maximum.
    assert(rp::pool_max_sets(64, 32) == 32);
    // The pool code requires at least 16 sets (descriptor_pool::create).
    assert(rp::interpreter_pool_max_sets >= 16);
}

void test_refused_retry()
{
    assert(!rp::refused_retry_due(100, 100));
    assert(!rp::refused_retry_due(100, 100 + rp::pipeline_refused_retry_s - 1));
    assert(rp::refused_retry_due(100, 100 + rp::pipeline_refused_retry_s));
    // A clock that went backwards never makes it due.
    assert(!rp::refused_retry_due(100, 50));
}

void test_null_reports()
{
    // Inline link: one report; no pipeline means refused.
    assert(rp::null_report_is_refusal(false, 0));

    // Asynchronous link, worker refused: two null reports in either order; only the second is the refusal.
    assert(!rp::null_report_is_refusal(true, 0));
    assert(rp::null_report_is_refusal(true, 1));

    // Asynchronous link that succeeded: the single null report (queued) is never a refusal, whatever the order.
    u32 nulls = 0;
    assert(!rp::null_report_is_refusal(true, nulls++));
}

void test_pinned_by_stamp()
{
    // ProgramStateCache.h pins the pipeline handed out last by giving it the current time as its last use.
    std::vector<u32> last(rp::pipeline_live_cap + 1, 0);
    const u32 now = 10 * rp::pipeline_idle_s;
    last[5] = now;
    const auto v = rp::idle_victims(last, now, rp::pipeline_idle_s, rp::pipeline_live_cap, rp::pipeline_live_target);
    assert(v.size() == last.size() - rp::pipeline_live_target);
    for (const std::size_t i : v)
    {
        assert(i != 5);
    }
}

void test_stats()
{
    rp::g_pipeline_stats.evicted.fetch_add(3);
    assert(rp::g_pipeline_stats.evicted.load() == 3);
    rp::g_pipeline_stats.evicted.store(0);
}

void test_constants()
{
    static_assert(rp::interpreter_live_min <= rp::interpreter_live_cap);
    static_assert(rp::pipeline_live_target < rp::pipeline_live_cap);
    static_assert(rp::preload_max <= rp::pipeline_live_cap);
    static_assert(rp::interpreter_pending_max >= 1);
    static_assert(rp::pool_window_cap == 48ull << 20);

    // 0.39 kill switches (exact names of the release notes).
    assert(!std::strcmp(rp::interp_no_lane_flag, "/data/rpcs3/interp-no-lane.flag"));
    assert(!std::strcmp(rp::gl_async_flag, "/data/rpcs3/gl-async.flag"));
    assert(!std::strcmp(rp::compile_no_slot_flag, "/data/rpcs3/rsx-compile-no-slot.flag"));
    assert(!std::strcmp(rp::interpreter_sync_flag, "/data/rpcs3/interpreter-sync-link.flag"));
    assert(!std::strcmp(rp::translate_async_flag, "/data/rpcs3/shader-translate-async.flag"));
    static_assert(rp::translate_take_back_us == 250'000);
}

void test_texture_env()
{
    // A variant that samples a texture always binds its environment.
    assert(rp::interpreter_texture_env_needed(1, false));
    assert(rp::interpreter_texture_env_needed(0x8000, true));

    // A texture-less variant drawn with its own (texture-less) program: nothing to bind (0.38 and upstream).
    assert(!rp::interpreter_texture_env_needed(0, false));

    // A texture-less variant drawn with a program that declares the texture arrays: bound (otherwise a null-sampler crash on RADV).
    assert(rp::interpreter_texture_env_needed(0, true));

    // Every texture-less variant's generic form declares the arrays, so its draws bind the environment.
    for (u64 opts = 0; opts < (1u << 16); ++opts)
    {
        if (opts & opt_textures)
        {
            continue;
        }

        const u64 generic = rp::compatible_opts(opts, base_vs, base_fs);
        assert(generic & opt_textures);
        assert(rp::interpreter_texture_env_needed(0, (generic & opt_textures) != 0));
        assert(!rp::interpreter_texture_env_needed(0, (opts & opt_textures) != 0));
    }
}

void test_image_array_write_count()
{
    static_assert(rp::interpreter_texture_array_size == 16);
    assert(rp::image_array_write_count(16) == 16);
    assert(rp::image_array_write_count(1) == 1);
    assert(rp::image_array_write_count(0) == 0);
    assert(rp::image_array_write_count(17) == 16);
    assert(rp::image_array_write_count(~0ull) == 16);
}

void test_compile_slot_level()
{
    // Slots enabled: only a real shortage (level 2 or 3) makes RSX.W jobs take one.
    assert(!rp::rsx_job_takes_slot(true, 0));
    assert(!rp::rsx_job_takes_slot(true, 1));
    assert(rp::rsx_job_takes_slot(true, 2));
    assert(rp::rsx_job_takes_slot(true, 3));

    // rsx-compile-no-slot.flag: never.
    for (int level = 0; level <= 3; ++level)
    {
        assert(!rp::rsx_job_takes_slot(false, level));
    }
}

void test_stats_fields()
{
    rp::pipeline_stats s;
    assert(s.interp_fallback_env.load() == 0 && s.unbound_sampler_slots.load() == 0 && s.lane_links.load() == 0);
    assert(s.gl_compiles.load() == 0 && s.gl_compile_ms.load() == 0 && s.gl_compile_us.load() == 0);
    assert(s.zcull_query_waits.load() == 0 && s.zcull_wait_ms.load() == 0 && s.zcull_wait_us.load() == 0);
    assert(s.rsx_translate.load() == 0 && s.rsx_translate_us.load() == 0 && s.rsx_translate_ms.load() == 0);
    assert(s.rsx_translate_max_us.load() == 0 && s.translate_async.load() == 0 && s.translate_taken_back.load() == 0);
}

void test_note_max()
{
    std::atomic<u64> m{0};
    rp::note_max(m, 0);
    assert(m.load() == 0);
    rp::note_max(m, 19'000);
    assert(m.load() == 19'000);
    rp::note_max(m, 7'000); // never lowered
    assert(m.load() == 19'000);
    rp::note_max(m, 27'000);
    assert(m.load() == 27'000);

    // The printer takes the window maximum and starts the next window at 0.
    assert(m.exchange(0) == 27'000);
    rp::note_max(m, 21'000);
    assert(m.load() == 21'000);

    // Concurrent writers: the largest value wins.
    std::atomic<u64> c{0};
    std::vector<std::thread> threads;
    for (u64 t = 0; t < 4; ++t)
    {
        threads.emplace_back([&c, t]()
        {
            for (u64 i = 0; i < 20000; ++i)
            {
                rp::note_max(c, i * 4 + t);
            }
        });
    }

    for (auto& thread : threads)
    {
        thread.join();
    }

    assert(c.load() == 19999 * 4 + 3);

    // Translation telemetry as the renderer records it: count, exact time, longest one.
    rp::pipeline_stats s;
    for (const u64 us : {19'000ull, 27'000ull, 23'000ull})
    {
        s.rsx_translate.fetch_add(1);
        rp::add_duration(s.rsx_translate_us, s.rsx_translate_ms, us);
        rp::note_max(s.rsx_translate_max_us, us);
    }

    assert(s.rsx_translate.load() == 3 && s.rsx_translate_us.load() == 69'000 && s.rsx_translate_ms.load() == 69);
    assert(s.rsx_translate_max_us.exchange(0) == 27'000 && s.rsx_translate_max_us.load() == 0);
}

void test_add_duration()
{
    std::atomic<u64> us{0}, ms{0};
    rp::add_duration(us, ms, 999);
    assert(us.load() == 999 && ms.load() == 0);
    rp::add_duration(us, ms, 1);
    assert(us.load() == 1000 && ms.load() == 1);
    rp::add_duration(us, ms, 0);
    assert(ms.load() == 1);
    rp::add_duration(us, ms, 2'500'000);
    assert(us.load() == 2'501'000 && ms.load() == 2501);

    // Many sub-millisecond waits add up (a per-event millisecond count would stay 0).
    std::atomic<u64> us2{0}, ms2{0};
    for (int i = 0; i < 4000; ++i)
    {
        rp::add_duration(us2, ms2, 250);
    }
    assert(us2.load() == 1'000'000 && ms2.load() == 1000);

    // Concurrent adders: the millisecond field ends at the exact total.
    std::atomic<u64> us3{0}, ms3{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
    {
        threads.emplace_back([&]()
        {
            for (int i = 0; i < 25000; ++i)
            {
                rp::add_duration(us3, ms3, 3);
            }
        });
    }

    for (auto& thread : threads)
    {
        thread.join();
    }

    assert(us3.load() == 300'000 && ms3.load() == 300);
}
} // namespace

int main()
{
    test_compatible_opts();
    test_interpreter_cap();
    test_lru_victims();
    test_idle_victims();
    test_newest_entries();
    test_pool_max_sets();
    test_refused_retry();
    test_null_reports();
    test_pinned_by_stamp();
    test_stats();
    test_constants();
    test_texture_env();
    test_image_array_write_count();
    test_compile_slot_level();
    test_stats_fields();
    test_add_duration();
    test_note_max();
    std::puts("rsx_programs_test: all tests passed");
    return 0;
}
