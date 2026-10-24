// Host test of the 0.38 RSX shader/pipeline memory rules in util/ps4_rsx_programs.hpp: generic interpreter form,
// interpreter cap, LRU victims with a pinned entry, idle pipeline victims (cap/target/idle), newest disk cache
// entries, descriptor pool set caps, the refused-pipeline retry and the refusal reports of a link.
// 0.39: the interpreter texture environment of texture-less variants drawn with the generic form, the image array
// write count, the RSX.W compile slot level, the telemetry counters and their duration totals, the window maximum of
// the program translation time and the asynchronous translation constants.
// 0.40: the compile slot under the 0.38 and the 0.39 governor rules; the interpreter step bound (the define, and both
// interpreter sources with and without it: the upstream loops when it is absent). Occlusion reports and conditional
// rendering of skipped draws, the pipeline wait histogram and its percentile, the pipeline burst line, the shared-queue
// job pick, the Resolution Scale deferral and slider maximum, the command buffer split and the render-pass close
// caller tally.
// Build: clang++ -std=c++20 -static -I rpcs3 -I . ps4-port/tests/rsx_programs_test.cpp
#include "util/ps4_rsx_programs.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <set>
#include <string>
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
    // 0.38 governor rules (the default): every job takes a slot at every level, as 0.38 did (the slot rules then
    // serialise it with PPU/SPU compiles from level 1, and it counts as an active compile at level 0).
    for (int level = 0; level <= 3; ++level)
    {
        assert(rp::rsx_job_takes_slot(true, level, false));
    }

    // 0.39 rules (governor_v039): only a real shortage (level 2 or 3) makes RSX.W jobs take one.
    assert(!rp::rsx_job_takes_slot(true, 0, true));
    assert(!rp::rsx_job_takes_slot(true, 1, true));
    assert(rp::rsx_job_takes_slot(true, 2, true));
    assert(rp::rsx_job_takes_slot(true, 3, true));

    // rsx-compile-no-slot.flag: never, with either rules.
    for (int level = 0; level <= 3; ++level)
    {
        assert(!rp::rsx_job_takes_slot(false, level, false));
        assert(!rp::rsx_job_takes_slot(false, level, true));
    }
}

// The interpreter sources exactly as ShaderInterpreter.h embeds them (raw string literals).
const std::string g_fragment_interpreter =
#include "Emu/RSX/Program/GLSLInterpreter/FragmentInterpreter.glsl"
    ;
const std::string g_vertex_interpreter =
#include "Emu/RSX/Program/GLSLInterpreter/VertexInterpreter.glsl"
    ;

std::vector<std::string> glsl_lines(const std::string& text)
{
    std::vector<std::string> lines;
    std::size_t pos = 0;
    while (pos <= text.size())
    {
        std::size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
        pos = end + 1;
    }
    return lines;
}

std::string trimmed(const std::string& s)
{
    const std::size_t b = s.find_first_not_of(" \t");
    if (b == std::string::npos) return {};
    const std::size_t e = s.find_last_not_of(" \t");
    return s.substr(b, e - b + 1);
}

// What the GLSL preprocessor keeps of the step-limit blocks, with the define injected or not. Every other directive is
// kept as text. Asserts the block shape: '#ifdef PS4_INTERP_STEP_LIMIT', bounded loop, '#else', upstream loop, '#endif'
// with no directive in between; returns the number of blocks.
std::size_t resolve_step_limit(const std::vector<std::string>& in, bool defined, std::vector<std::string>& out)
{
    std::size_t blocks = 0;
    for (std::size_t i = 0; i < in.size(); ++i)
    {
        if (trimmed(in[i]) != "#ifdef PS4_INTERP_STEP_LIMIT")
        {
            out.push_back(in[i]);
            continue;
        }

        blocks++;
        std::size_t j = i + 1;
        std::vector<std::string> bounded, upstream;
        for (; j < in.size() && trimmed(in[j]) != "#else"; ++j)
        {
            assert(trimmed(in[j]).rfind('#', 0) != 0);
            bounded.push_back(in[j]);
        }
        assert(j < in.size());
        for (++j; j < in.size() && trimmed(in[j]) != "#endif"; ++j)
        {
            assert(trimmed(in[j]).rfind('#', 0) != 0);
            upstream.push_back(in[j]);
        }
        assert(j < in.size());
        assert(!bounded.empty() && !upstream.empty());
        for (const auto& l : defined ? bounded : upstream) out.push_back(l);
        i = j;
    }
    return blocks;
}

std::size_t count_of(const std::vector<std::string>& lines, const std::string& needle)
{
    std::size_t n = 0;
    for (const auto& l : lines) n += l.find(needle) != std::string::npos;
    return n;
}

void test_interpreter_step_limit()
{
    // The injected line defines the macro the GLSL tests, with the limit of the header, as an unsigned literal.
    static_assert(rp::interp_step_limit == 65536);
    assert(std::string(rp::interp_step_define) == "#define PS4_INTERP_STEP_LIMIT " + std::to_string(rp::interp_step_limit) + "u\n");

    const std::string frag_upstream = "while (!inst.end)";
    const std::string vert_upstream = "while (current_instruction >= 0 && current_instruction < 544) // NOTE: CEX can hold upto 544 instructions";

    for (const bool defined : {false, true})
    {
        std::vector<std::string> frag, vert;
        assert(resolve_step_limit(glsl_lines(g_fragment_interpreter), defined, frag) == 1);
        assert(resolve_step_limit(glsl_lines(g_vertex_interpreter), defined, vert) == 1);

        if (!defined)
        {
            // Without the define the GLSL is the upstream text: the upstream loop line, no PS4 identifier at all.
            assert(count_of(frag, "ps4_") == 0 && count_of(vert, "ps4_") == 0);
            std::size_t f = 0, v = 0;
            for (const auto& l : frag) f += trimmed(l) == frag_upstream;
            for (const auto& l : vert) v += trimmed(l) == vert_upstream;
            assert(f == 1 && v == 1);
            continue;
        }

        // With it: one counter declared right before its loop, which tests the counter against the limit and keeps the
        // upstream condition first.
        for (auto* lines : {&frag, &vert})
        {
            std::size_t decl = 0, loop = 0;
            for (std::size_t i = 0; i < lines->size(); ++i)
            {
                const std::string t = trimmed((*lines)[i]);
                if (t == "uint ps4_steps = 0u;") decl = i + 1;
                if (t.find("ps4_steps++ < PS4_INTERP_STEP_LIMIT") != std::string::npos) loop = i + 1;
            }
            assert(decl && loop == decl + 1);
            assert(count_of(*lines, "ps4_steps") == 2);
        }
        std::size_t f = 0, v = 0;
        for (const auto& l : frag) f += trimmed(l) == "while (!inst.end && ps4_steps++ < PS4_INTERP_STEP_LIMIT)";
        for (const auto& l : vert)
            v += trimmed(l) == "while (current_instruction >= 0 && current_instruction < 544 && ps4_steps++ < PS4_INTERP_STEP_LIMIT) // NOTE: CEX can hold upto 544 instructions";
        assert(f == 1 && v == 1);
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

void test_report_with_skips()
{
    // Upstream sum when the query is complete, also for an empty count (occluded).
    assert(rp::report_with_skips(0, 0, false) == 0);
    assert(rp::report_with_skips(0, 7, false) == 7);
    assert(rp::report_with_skips(5, 7, false) == 12);
    // A skipped draw: an empty count answers visible (1); a count with samples stays what the host counted.
    assert(rp::report_with_skips(0, 0, true) == 1);
    assert(rp::report_with_skips(0, 9, true) == 9);
    assert(rp::report_with_skips(4, 0, true) == 4);
    assert(rp::report_with_skips(4, 3, true) == 7);
    static_assert(rp::report_with_skips(0, 0, true) == 1);

    // GPU predicate: only with the guard on, for an incomplete source or no host data at all.
    assert(!rp::cond_render_forced_visible(false, true, 0));
    assert(!rp::cond_render_forced_visible(false, false, 0));
    assert(rp::cond_render_forced_visible(true, true, 3));
    assert(rp::cond_render_forced_visible(true, false, 0));
    assert(!rp::cond_render_forced_visible(true, false, 1));
    assert(!rp::cond_render_forced_visible(true, false, 5));

    rp::pipeline_stats s;
    assert(s.queries_forced_visible.load() == 0 && s.cond_render_forced_visible.load() == 0);
}

void test_pipeline_waits()
{
    // Buckets: edges inclusive, the last bucket open.
    assert(rp::wait_bucket(0) == 0);
    assert(rp::wait_bucket(25'000) == 0);
    assert(rp::wait_bucket(25'001) == 1);
    assert(rp::wait_bucket(1'000'000) == 5);
    assert(rp::wait_bucket(30'000'000) == std::size(rp::wait_edges_ms) - 1);
    assert(rp::wait_bucket(30'000'001) == std::size(rp::wait_edges_ms));
    assert(rp::wait_bucket(~0ull) == rp::wait_bucket_count - 1);
    for (std::size_t i = 1; i < std::size(rp::wait_edges_ms); ++i)
    {
        assert(rp::wait_edges_ms[i - 1] < rp::wait_edges_ms[i]);
    }

    // Percentile: the edge of the bucket holding the rank, never above the window maximum.
    u64 counts[rp::wait_bucket_count]{};
    assert(rp::wait_percentile_ms(counts, 90, 0) == 0);
    counts[rp::wait_bucket(10'000)] = 9;    // nine fast pipelines
    counts[rp::wait_bucket(4'000'000)] = 1; // one 4 s wait
    assert(rp::wait_percentile_ms(counts, 90, 4000) == 25);
    assert(rp::wait_percentile_ms(counts, 100, 4000) == 4000);
    assert(rp::wait_percentile_ms(counts, 91, 4000) == 4000);
    assert(rp::wait_percentile_ms(counts, 0, 4000) == 25);   // clamped to the first rank
    assert(rp::wait_percentile_ms(counts, 500, 4000) == 4000); // clamped to 100
    // A single pipeline above the last edge: the maximum stands in for the open bucket.
    u64 one[rp::wait_bucket_count]{};
    one[rp::wait_bucket_count - 1] = 1;
    assert(rp::wait_percentile_ms(one, 90, 45'000) == 45'000);
    // The maximum caps an edge (a 30 ms wait sits in the 50 ms bucket).
    u64 small[rp::wait_bucket_count]{};
    small[rp::wait_bucket(30'000)] = 1;
    assert(rp::wait_percentile_ms(small, 90, 30) == 30);

    // The window and burst maxima.
    rp::pipeline_waits w;
    w.note(10'000);
    w.note(2'500'000);
    w.note(40'000);
    assert(w.burst_max_us.load() == 2'500'000);
    u64 taken[rp::wait_bucket_count]{};
    u64 taken_max = 0;
    w.take(taken, taken_max);
    assert(taken_max == 2'500'000);
    assert(taken[0] == 1 && taken[rp::wait_bucket(40'000)] == 1 && taken[rp::wait_bucket(2'500'000)] == 1);
    assert(rp::wait_percentile_ms(taken, 90, taken_max / 1000) == 2500);
    // Taking starts the window over, the burst maximum stays for the burst tracker.
    u64 again[rp::wait_bucket_count]{};
    u64 again_max = 1;
    w.take(again, again_max);
    assert(again_max == 0 && rp::wait_percentile_ms(again, 90, 0) == 0);
    assert(w.burst_max_us.load() == 2'500'000);

    // Concurrent workers: every wait is counted.
    rp::pipeline_waits c;
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
    {
        threads.emplace_back([&c, t]()
        {
            for (int i = 0; i < 10000; ++i)
            {
                c.note(static_cast<u64>(i % 100) * 1000 + static_cast<u64>(t));
            }
        });
    }

    for (auto& thread : threads)
    {
        thread.join();
    }

    u64 all[rp::wait_bucket_count]{};
    u64 all_max = 0;
    c.take(all, all_max);
    u64 total = 0;
    for (const u64 n : all)
    {
        total += n;
    }

    assert(total == 40000 && all_max == 99'003);
}

void test_burst_line()
{
    rp::burst_summary b;
    b.pipelines = 37;
    b.seconds = 6.25;
    b.ready_max_ms = 5120;
    b.links = 3;
    b.link_ms = 4410;
    b.skipped = 1534;
    b.frames_with_skips = 151;
    b.interpreter_draws = 20811;
    assert(rp::burst_line(b) == "PS4 RSX: pipeline burst: 37 new pipelines in 6.2 s, ready after 5120 ms (oldest), 3 interpreter links meanwhile (4410 ms), "
        "1534 draws skipped in 151 frames, 20811 drawn by the interpreter" ||
        rp::burst_line(b) == "PS4 RSX: pipeline burst: 37 new pipelines in 6.3 s, ready after 5120 ms (oldest), 3 interpreter links meanwhile (4410 ms), "
        "1534 draws skipped in 151 frames, 20811 drawn by the interpreter");
    b.last = true;
    b.seconds = 1.5;
    const std::string last = rp::burst_line(b);
    assert(last.find("in 1.5 s") != std::string::npos);
    const std::string suffix = " (last burst line of this renderer)";
    assert(last.size() > suffix.size() && last.compare(last.size() - suffix.size(), suffix.size(), suffix) == 0);

    // Large values still fit the line.
    rp::burst_summary big;
    big.pipelines = big.ready_max_ms = big.links = big.link_ms = big.skipped = big.frames_with_skips = big.interpreter_draws = ~0ull;
    big.seconds = 123456.7;
    big.last = true;
    const std::string line = rp::burst_line(big);
    assert(line.find("18446744073709551615 drawn by the interpreter (last burst line of this renderer)") != std::string::npos);
}

void test_pick_next_job()
{
    using rp::job_pick;
    static_assert(rp::max_links_running == 1);

    // A queued game pipeline always goes first, whatever links wait or run.
    assert(rp::pick_next_job(true, false, 0) == job_pick::game);
    assert(rp::pick_next_job(true, true, 0) == job_pick::game);
    assert(rp::pick_next_job(true, true, 1) == job_pick::game);
    // A link only when no game pipeline is queued and no other worker is linking one.
    assert(rp::pick_next_job(false, true, 0) == job_pick::link);
    assert(rp::pick_next_job(false, true, 1) == job_pick::none);
    assert(rp::pick_next_job(false, true, 2) == job_pick::none);
    assert(rp::pick_next_job(false, false, 0) == job_pick::none);

    // Model of two workers over a queue: links 30 s, game pipelines 0.2 s. A game pipeline queued while a link runs
    // is taken by the free worker at once; a second link waits for the first one.
    struct worker
    {
        job_pick job = job_pick::none;
        double busy_until = 0;
    };

    std::vector<double> game_queue_times = {0.0, 0.1, 0.2, 5.0};
    u32 links_queued = 2;
    u32 game_next = 0;
    worker workers[2];
    double max_game_wait = 0;
    u32 links_done = 0;

    for (double now = 0; now < 120 && (game_next < game_queue_times.size() || links_queued || workers[0].job != job_pick::none || workers[1].job != job_pick::none); now += 0.05)
    {
        for (auto& w : workers)
        {
            if (w.job != job_pick::none && now >= w.busy_until)
            {
                links_done += w.job == job_pick::link ? 1 : 0;
                w.job = job_pick::none;
            }
        }

        for (auto& w : workers)
        {
            if (w.job != job_pick::none)
            {
                continue;
            }

            u32 running = 0;
            for (const auto& other : workers)
            {
                running += other.job == job_pick::link ? 1 : 0;
            }

            const bool game_queued = game_next < game_queue_times.size() && game_queue_times[game_next] <= now;
            switch (rp::pick_next_job(game_queued, links_queued != 0, running))
            {
            case job_pick::game:
                max_game_wait = std::max(max_game_wait, now - game_queue_times[game_next]);
                game_next++;
                w = {job_pick::game, now + 0.2};
                break;
            case job_pick::link:
                links_queued--;
                w = {job_pick::link, now + 30};
                break;
            case job_pick::none:
                break;
            }
        }
    }

    assert(game_next == game_queue_times.size());
    assert(links_done == 2);
    assert(max_game_wait < 0.5); // never behind a 30 s link
}

void test_scale_rules()
{
    // A live Resolution Scale change waits for the next boot only with MSAA on.
    assert(rp::scale_change_deferred(true));
    assert(!rp::scale_change_deferred(false));

    // Slider maximum: 150 %, or the running value up to the console limit.
    static_assert(rp::scale_slider_max == 150 && rp::scale_console_limit == 200);
    assert(rp::scale_slider_maximum(25) == 150);
    assert(rp::scale_slider_maximum(100) == 150);
    assert(rp::scale_slider_maximum(150) == 150);
    assert(rp::scale_slider_maximum(175) == 175);
    assert(rp::scale_slider_maximum(200) == 200);
    assert(rp::scale_slider_maximum(650) == 200);
    assert(rp::scale_slider_maximum(800) == 200);
}

void test_cb_split()
{
    static_assert(rp::cb_split_draws == 384);
    // Off (limit 0): never.
    assert(!rp::cb_split_due(0, 0, false));
    assert(!rp::cb_split_due(100000, 0, false));
    // On: at the limit, not before, never under an active GPU conditional render.
    assert(!rp::cb_split_due(383, rp::cb_split_draws, false));
    assert(rp::cb_split_due(384, rp::cb_split_draws, false));
    assert(rp::cb_split_due(385, rp::cb_split_draws, false));
    assert(!rp::cb_split_due(384, rp::cb_split_draws, true));
    assert(!rp::cb_split_due(1000, rp::cb_split_draws, true));

    // 1,536 draws in one frame, counting reset by every submit: four parts.
    u32 draws = 0, submits = 0;
    for (u32 i = 0; i < 1536; ++i)
    {
        if (rp::cb_split_due(++draws, rp::cb_split_draws, false))
        {
            submits++;
            draws = 0;
        }
    }

    assert(submits == 4 && draws == 0);
}

void test_caller_tally()
{
    rp::caller_tally<4> t;
    // Nothing yet.
    u32 calls = 0;
    assert(t.take([&](u64, u64) { calls++; }) == 0 && calls == 0);

    for (int i = 0; i < 5; ++i) t.note(0x1000);
    for (int i = 0; i < 9; ++i) t.note(0x2000);
    t.note(0x3000);
    t.note(0x4000);
    t.note(0x5000); // no slot left
    t.note(0);      // unknown site

    std::vector<std::pair<u64, u64>> seen;
    const u64 more = t.take([&](u64 site, u64 count) { seen.emplace_back(site, count); });
    assert(more == 2);
    assert(seen.size() == 4);
    assert(seen[0] == std::make_pair(u64{0x2000}, u64{9})); // busiest first
    assert(seen[1] == std::make_pair(u64{0x1000}, u64{5}));
    assert((seen[2].second == 1 && seen[3].second == 1));

    // The counts start over, the sites keep their slots.
    seen.clear();
    t.note(0x3000);
    assert(t.take([&](u64 site, u64 count) { seen.emplace_back(site, count); }) == 0);
    assert(seen.size() == 1 && seen[0] == std::make_pair(u64{0x3000}, u64{1}));

    // Concurrent notes of the same sites from several threads: every close is counted once.
    rp::caller_tally<8> c;
    std::vector<std::thread> threads;
    for (int th = 0; th < 4; ++th)
    {
        threads.emplace_back([&c]()
        {
            for (int i = 0; i < 20000; ++i)
            {
                c.note(0x100 + static_cast<u64>(i % 6) * 0x10);
            }
        });
    }

    for (auto& thread : threads)
    {
        thread.join();
    }

    u64 total = 0;
    u32 sites = 0;
    const u64 rest = c.take([&](u64, u64 count) { total += count; sites++; });
    assert(rest == 0 && total == 80000 && sites == 6);
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
    test_interpreter_step_limit();
    test_stats_fields();
    test_add_duration();
    test_note_max();
    test_report_with_skips();
    test_pipeline_waits();
    test_burst_line();
    test_pick_next_job();
    test_scale_rules();
    test_cb_split();
    test_caller_tally();
    std::puts("rsx_programs_test: all tests passed");
    return 0;
}
