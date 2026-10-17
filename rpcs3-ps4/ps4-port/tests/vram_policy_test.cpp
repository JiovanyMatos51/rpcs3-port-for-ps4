// Host test of the 0.36 GPU memory rules in util/ps4_gpu_health.hpp: ring room (ring_contiguous_free, ring_guard),
// the futile severe-trim back-off, the descriptor pool window estimate, the render-pass close cause scope and the
// PS4 VRAM line; and of the 0.38 window policy in util/ps4_vram_policy.hpp: window load, admission, texture quota and
// age-based eviction plan, image pool caps, DMA release plan, reclaim registry, stop request; 0.39: GPU hang stop rule,
// dead FIFO stop cause, fatal rating from allocations and the fatal trim back-off, adaptive ring growth rules and the
// ring record, the stop notice texts in the interface languages and the encoded XMB notice.
// Build: clang++ -std=c++20 -static -I rpcs3 -I . ps4-port/tests/vram_policy_test.cpp
#include "util/ps4_gpu_health.hpp"
#include "util/ps4_vram_policy.hpp"

#include <cassert>
#include <thread>
#include <vector>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>

namespace gpu = utils::ps4::gpu;

namespace
{
// rsx::data_heap::can_alloc_impl (Common/ring_buffer_helper.h), the rule ring_contiguous_free describes.
bool can_alloc(u64 put, u64 get, u64 size, u64 alloc)
{
    const u64 end = put + alloc;
    if (end < size)
    {
        if (end < get) return true;
        if (put > get) return true;
        return false;
    }
    if (put < get) return false;
    if (alloc > get) return false;
    return true;
}

// Largest allocation can_alloc accepts, by bisection (can_alloc is monotonic in the size for a fixed state).
u64 largest_fit(u64 put, u64 get, u64 size)
{
    u64 lo = 0, hi = size;
    while (lo < hi)
    {
        const u64 mid = (lo + hi + 1) / 2;
        if (can_alloc(put, get, size, mid)) lo = mid;
        else hi = mid - 1;
    }
    return lo;
}

void test_ring_contiguous_free()
{
    constexpr u64 size = 64 * 0x100000;

    // Fresh ring (init: put 0, get size - 1) and the states named in the renderer's comments.
    assert(gpu::ring_contiguous_free(0, size - 1, size) == size - 1);
    assert(gpu::ring_contiguous_free(10, 30, size) == 20);
    assert(gpu::ring_contiguous_free(size - 0x100000, 0x200000, size) == 0x200000);
    assert(gpu::ring_contiguous_free(0x300000, 0x100000, size) == size - 0x300000);

    // Against can_alloc for random states: the reported room is the largest fit, give or take the one byte the
    // strict comparisons of can_alloc keep free.
    std::mt19937_64 rng(36);
    for (int i = 0; i < 200000; i++)
    {
        const u64 put = rng() % size;
        const u64 get = rng() % size;
        const u64 room = gpu::ring_contiguous_free(put, get, size);
        const u64 fit = largest_fit(put, get, size);
        assert(fit <= room && room <= fit + 1);
    }

    // After a reset (get = put - 1) at least half the ring fits, so a guard of up to a quarter is always met.
    for (u64 put = 1; put < size; put += 0x12345)
    {
        assert(gpu::ring_contiguous_free(put, put - 1, size) >= (size - 1) / 2);
    }
}

void test_ring_guard()
{
    constexpr u64 mib = 0x100000;
    assert(gpu::ring_guard(4 * mib, 64 * mib) == 4 * mib);    // attrib ring
    assert(gpu::ring_guard(32 * mib, 64 * mib) == 16 * mib);  // texture upload ring: a quarter
    assert(gpu::ring_guard(0x10000, 16 * mib) == mib);        // UBO rings: at least 1 MiB
    assert(gpu::ring_guard(512 * 16, 64 * mib) == mib);       // interpreter blocks
    for (u64 size = 4 * mib; size <= 1024 * mib; size += 4 * mib)
    {
        assert(gpu::ring_guard(64 * mib, size) <= size / 4);
    }

    // 0.38 ring sizes: attrib 32, texture upload 32, UBO/transform/index/instancing/interpreter 8, aux upload 16 MiB.
    assert(gpu::ring_guard(4 * mib, 32 * mib) == 4 * mib);
    assert(gpu::ring_guard(32 * mib, 32 * mib) == 8 * mib);
    assert(gpu::ring_guard(0x10000, 8 * mib) == mib);
    assert(gpu::ring_guard(512 * 16, 8 * mib) == mib);
    assert(gpu::ring_guard(mib, 16 * mib) == mib);
}

namespace vram = utils::ps4::vram;

void test_window_load()
{
    constexpr u64 mib = vram::mib;
    // No meter: the allocator's blocks against the budget (0.32 rule).
    assert(vram::window_load_percent(320 * mib, 640 * mib, 0, 0, 0) == 50.f);
    // The meter wins when RADV's share pushes the window higher: 959 usable MiB.
    const float l = vram::window_load_percent(320 * mib, 640 * mib, 863 * mib, vram::window_bytes_default, 0);
    assert(l > 89.9f && l < 90.1f);
    // The emergency reserve does not count.
    const float r = vram::window_load_percent(320 * mib, 640 * mib, 895 * mib, vram::window_bytes_default, 32 * mib);
    assert(r > 89.9f && r < 90.1f);
    // A fuller VMA still wins over a quiet window.
    assert(vram::window_load_percent(608 * mib, 640 * mib, 400 * mib, vram::window_bytes_default, 0) == 95.f);
    // A window not larger than the margin is ignored; a reserve larger than used counts as 0.
    assert(vram::window_load_percent(0, 0, 10 * mib, 16 * mib, 0) == 0.f);
    assert(vram::window_load_percent(0, 0, 8 * mib, vram::window_bytes_default, 32 * mib) == 0.f);
}

void test_admission()
{
    constexpr u64 mib = vram::mib;
    assert(!vram::admission_short(4 * mib - 1, 0));                 // small requests are never held back
    assert(vram::admission_short(4 * mib, 19 * mib));
    assert(!vram::admission_short(4 * mib, 20 * mib));
    assert(vram::admission_short(128 * mib, 143 * mib));            // the 0.35 Wolverine ring growth
    assert(!vram::admission_short(128 * mib, 144 * mib));
}

void test_texture_quota()
{
    constexpr u64 mib = vram::mib;
    assert(vram::texture_quota(192 * mib, 640 * mib) == 160 * mib); // default budget: a quarter
    assert(vram::texture_quota(128 * mib, 640 * mib) == 128 * mib); // vram=low keeps its smaller upstream quota
    assert(vram::texture_quota(512 * mib, 832 * mib) == 192 * mib); // never above 192 MiB
    assert(vram::texture_quota(512 * mib, 0) == 192 * mib);         // no budget: the cap alone
}

void test_eviction_plan()
{
    constexpr u64 mib = vram::mib;
    // Target from the meter: 863 MiB used of 959 usable is 90%; 85% is 815 MiB -> 48 MiB.
    const u64 t = vram::eviction_target(0, 640 * mib, 863 * mib, vram::window_bytes_default, 0);
    assert(t >= 47 * mib && t <= 49 * mib);
    // Never less than 16 MiB; from VMA without a meter.
    assert(vram::eviction_target(0, 640 * mib, 100 * mib, vram::window_bytes_default, 0) == vram::evict_min_bytes);
    assert(vram::eviction_target(608 * mib, 640 * mib, 0, 0, 0) == 64 * mib);

    std::vector<vram::evict_candidate> c = {
        {1000, 4 * mib, 0x100, false}, {0, 8 * mib, 0x200, false}, {600, 16 * mib, 0x300, false},
        {1290, 2 * mib, 0x400, false}, {100, 1 * mib, 0x500, false},
    };
    // Frame 1300, min age 300: candidates used at frames 0, 100, 600, 1000 qualify (1290 does not).
    u64 marked = vram::plan_eviction(c.data(), c.size(), 1300, 300, 0);
    assert(marked == 29 * mib);
    assert(c[0].base == 0x200 && c[0].evict && c[1].base == 0x500 && c[1].evict && c[3].base == 0x100 && c[3].evict);
    assert(c[4].base == 0x400 && !c[4].evict);
    // A target stops at the first candidates that reach it, oldest first.
    marked = vram::plan_eviction(c.data(), c.size(), 1300, 300, 9 * mib);
    assert(marked == 9 * mib && c[0].evict && c[1].evict && !c[2].evict && !c[3].evict);
    // A clock behind the stamps (renderer restart) evicts nothing.
    marked = vram::plan_eviction(c.data(), c.size(), 50, 300, 0);
    assert(marked == 0);
    for (const auto& e : c) assert(!e.evict);
    assert(vram::plan_eviction(nullptr, 0, 1300, 300, 0) == 0);
}

void test_image_pool()
{
    constexpr u64 mib = vram::mib;
    assert(!vram::image_pool_over(64, 32 * mib));
    assert(vram::image_pool_over(65, 1 * mib));
    assert(vram::image_pool_over(1, 32 * mib + 1));
}

void test_dma_plan()
{
    constexpr u64 mib = vram::mib;
    constexpr u64 s = 1'000'000;
    std::vector<vram::dma_candidate> c = {
        {100 * s, 16 * mib, 0x10000, false, false},  // idle 40 s
        {95 * s, 16 * mib, 0x20000, true, false},    // idle 45 s but pinned
        {139 * s, 32 * mib, 0x30000, false, false},  // idle 1 s
        {130 * s, 24 * mib, 0x40000, false, false},  // idle 10 s
        {120 * s, 8 * mib, 0x50000, false, false},   // idle 20 s
    };
    // Pool 96 MiB, cap 64: the 30 s-idle block (16), then LRU idle >= 2 s: the 20 s one (8) and the 10 s one (24).
    const u64 marked = vram::plan_dma_release(c.data(), c.size(), 140 * s, 96 * mib, vram::dma_idle_us, vram::dma_pool_cap, vram::dma_min_age_us);
    assert(marked == 48 * mib);
    for (const auto& e : c)
    {
        if (e.start == 0x10000 || e.start == 0x50000 || e.start == 0x40000) assert(e.release);
        else assert(!e.release);
    }

    // Under the cap only the idle ones go; pinned blocks never.
    for (auto& e : c) e.pinned = true;
    assert(vram::plan_dma_release(c.data(), c.size(), 1000 * s, 512 * mib, vram::dma_idle_us, vram::dma_pool_cap, vram::dma_min_age_us) == 0);
    for (auto& e : c) e.pinned = false;
    assert(vram::plan_dma_release(c.data(), c.size(), 140 * s, 16 * mib, vram::dma_idle_us, vram::dma_pool_cap, vram::dma_min_age_us) == 32 * mib);
    // Blocks stamped after 'now' (another thread) count as just used.
    assert(vram::plan_dma_release(c.data(), c.size(), 50 * s, 512 * mib, vram::dma_idle_us, vram::dma_pool_cap, vram::dma_min_age_us) == 0);
}

u64 reclaim_a(int level) noexcept { return level == 3 ? 10 : 1; }
u64 reclaim_b(int) noexcept { return 100; }

void test_reclaimers_and_stop()
{
    assert(vram::run_reclaimers(3) == 0);
    assert(vram::register_reclaimer(&reclaim_a));
    assert(vram::register_reclaimer(&reclaim_a)); // no duplicate
    assert(vram::register_reclaimer(&reclaim_b));
    assert(vram::run_reclaimers(3) == 110);
    assert(vram::run_reclaimers(2) == 101);
    vram::unregister_reclaimer(&reclaim_a);
    assert(vram::run_reclaimers(3) == 100);
    vram::unregister_reclaimer(&reclaim_b);
    assert(vram::run_reclaimers(3) == 0);
    assert(!vram::register_reclaimer(nullptr));

    char why[vram::stop_detail_size];
    assert(!vram::stop_requested());
    assert(vram::take_stop_request(why, sizeof(why)) == vram::stop_cause::none);
    assert(!vram::request_stop(vram::stop_cause::none, "x"));

    // Many threads: exactly one request wins and is taken once.
    std::vector<std::thread> threads;
    std::atomic<u32> winners{0};
    for (int i = 0; i < 8; i++)
    {
        threads.emplace_back([&winners, i]()
        {
            if (vram::request_stop(i & 1 ? vram::stop_cause::cpu_heap : vram::stop_cause::vram, "allocation of 32768 KiB (pool 3) failed after the deep reclaim"))
                winners++;
        });
    }
    for (auto& t : threads) t.join();
    assert(winners == 1);
    assert(vram::stop_requested());
    const vram::stop_cause cause = vram::take_stop_request(why, sizeof(why));
    assert(cause == vram::stop_cause::vram || cause == vram::stop_cause::cpu_heap);
    assert(!std::strcmp(why, "allocation of 32768 KiB (pool 3) failed after the deep reclaim"));
    assert(vram::take_stop_request(why, sizeof(why)) == vram::stop_cause::none);
    assert(std::strlen(vram::stop_notice(cause)) > 0 && std::strlen(vram::stop_notice(cause)) < 90);
    assert(vram::stop_requests() >= 8);

    // A long detail is cut; a small output buffer too.
    vram::reset_stop_request();
    assert(!vram::stop_requested());
    std::string long_why(400, 'x');
    assert(vram::request_stop(vram::stop_cause::vram, long_why.c_str()));
    char small[8];
    assert(vram::take_stop_request(small, sizeof(small)) == vram::stop_cause::vram);
    assert(std::strlen(small) == 7);
    vram::reset_stop_request();
    assert(vram::request_stop(vram::stop_cause::cpu_heap, nullptr));
    assert(vram::take_stop_request(why, sizeof(why)) == vram::stop_cause::cpu_heap && why[0] == '\0');
    vram::reset_stop_request();

    // Published window state round-trips.
    vram::window_state w{};
    w.time_us = 5;
    w.size = vram::window_bytes_default;
    w.used = 700 * vram::mib;
    w.largest_hole = 180 * vram::mib;
    w.vma = 402 * vram::mib;
    w.reserve = vram::emergency_reserve_bytes;
    vram::publish_window(w);
    const vram::window_state r = vram::current_window();
    assert(r.time_us == 5 && r.size == w.size && r.used == w.used && r.largest_hole == w.largest_hole && r.vma == w.vma && r.reserve == w.reserve);
}

void test_futile_trim_backoff()
{
    constexpr u64 ms = 1000;
    constexpr u64 total = 580ull << 20;
    gpu::futile_trim_backoff b;
    u64 now = 345'000'000;

    // 64 trims run, the 65th pauses.
    for (u32 i = 0; i < gpu::futile_trim_backoff::streak_limit; i++, now += 66 * ms)
    {
        assert(!b.should_skip(now, total));
    }
    assert(b.should_skip(now, total));
    assert(b.pauses == 1);
    const u64 paused_at = now;

    // Skipped for 10 s.
    for (now += 66 * ms; now < paused_at + gpu::futile_trim_backoff::backoff_us; now += 66 * ms)
    {
        assert(b.should_skip(now, total));
    }

    // One trim after the pause, then the next check pauses again.
    assert(!b.should_skip(now, total));
    now += 66 * ms;
    assert(b.should_skip(now, total));
    assert(b.pauses == 2);

    // About one trim per 10 s from now on: count over a simulated minute at 15 FPS.
    u32 runs = 0;
    for (const u64 end = now + 60'000'000; now < end; now += 66 * ms)
    {
        runs += b.should_skip(now, total) ? 0 : 1;
    }
    assert(runs >= 5 && runs <= 7);

    // A lower block total (memory came back) starts a new streak: trims run again.
    gpu::futile_trim_backoff d;
    u64 at = 0;
    for (u32 i = 0; i < gpu::futile_trim_backoff::streak_limit; i++, at += 66 * ms)
    {
        assert(!d.should_skip(at, total));
    }
    assert(d.should_skip(at, total));
    at += gpu::futile_trim_backoff::backoff_us;
    assert(!d.should_skip(at, total)); // the trim after the pause
    at += 66 * ms;
    assert(!d.should_skip(at, total - (32ull << 20)));
    for (u32 i = 1; i < gpu::futile_trim_backoff::streak_limit; i++)
    {
        at += 66 * ms;
        assert(!d.should_skip(at, total - (32ull << 20)));
    }
    at += 66 * ms;
    assert(d.should_skip(at, total - (32ull << 20)));

    // A growing total does not count as progress.
    gpu::futile_trim_backoff g;
    u64 t = total;
    for (u32 i = 0; i < gpu::futile_trim_backoff::streak_limit; i++)
    {
        assert(!g.should_skip(i * 66 * ms, t));
        t += 1 << 20;
    }
    assert(g.should_skip(64 * 66 * ms, t));
}

void test_descriptor_pool_window()
{
    constexpr u64 kib = 1024;
    // Vertex table, 16 sets of 2 UBO + 1 SSBO + 2 texel buffers (5 x 16 B): one page, the 48 KiB minimum.
    assert(gpu::descriptor_pool_window_bytes(16 * 5 * 16, 16 * 5, 16) == 48 * kib);
    // Fragment table with 16 combined samplers: 16 x (4 x 16 + 16 x 96) B is two pages, 64 KiB with pad and gap.
    assert(gpu::descriptor_pool_window_bytes(16 * (4 * 16 + 16 * 96), 16 * 4, 16) == 64 * kib);
    // A 1024-set subpool of the vertex layout: 80 KiB of descriptors + 16 KiB reserve = 96 KiB, plus 32 KiB.
    assert(gpu::descriptor_pool_window_bytes(1024 * 80, 1024 * 5, 1024) == 128 * kib);
    // An empty pool still takes a page.
    assert(gpu::descriptor_pool_window_bytes(0, 0, 16) == 48 * kib);
}

void test_rp_close_cause_scope()
{
    assert(gpu::t_rp_close_cause == 0);
    {
        const gpu::rp_close_cause_scope outer(3); // texture cache
        assert(gpu::t_rp_close_cause == 3);
        {
            const gpu::rp_close_cause_scope inner(1); // a barrier inside it
            assert(gpu::t_rp_close_cause == 3);
        }
        assert(gpu::t_rp_close_cause == 3);
    }
    assert(gpu::t_rp_close_cause == 0);
    {
        const gpu::rp_close_cause_scope only(1);
        assert(gpu::t_rp_close_cause == 1);
    }
    assert(gpu::t_rp_close_cause == 0);
}

void test_vram_line()
{
    // Generous values for a long session (the GPU window is 991 MiB), in the 512-byte buffer the health report uses
    // (util/ps4_health.hpp): the whole line fits, rare fields included.
    gpu::vram_snapshot s{};
    s.time_us = 1;
    s.heap_count = 2;
    s.device_local_mask = 2;
    s.heap_usage[0] = 999ull << 20;
    s.heap_budget[0] = 999ull << 20;
    s.heap_usage[1] = 999ull << 20;
    s.heap_budget[1] = 999ull << 20;
    s.dma_pool = 512ull << 20;
    s.texture_cache = 512ull << 20;
    s.surface_store = 512ull << 20;
    s.device_cap_mib = 768;
    s.descriptor_sets = 9999;
    s.descriptor_storage = 99999ull << 10;
    s.descriptor_pools = 9999;
    s.descriptor_pool_window = 999ull << 20;
    s.descriptor_pools_released = 99999;
    s.shaders_released = 9999;
    s.shader_text_released = 99999ull << 10;

    for (u32 i = 0; i < 99999; i++) gpu::note_pressure_trim();
    for (u32 i = 0; i < 99999; i++) gpu::note_futile_trim_skip();
    gpu::note_unfinished_submission(2999);
    assert(gpu::unfinished_submission_ms() == 0);

    char line[512];
    std::size_t n = gpu::format_line(s, line, sizeof(line));
    std::printf("%s\n(%zu chars)\n", line, n);
    std::fflush(stdout);
    assert(n + 1 < sizeof(line));
    assert(std::strstr(line, "futile severe trims skipped 99999"));
    assert(std::strstr(line, "descriptor pools 9999 (~999 MiB window), released 99999"));
    assert(std::strstr(line, "device lost no"));
    assert(!std::strstr(line, "unfinished"));
    assert(!std::strstr(line, "rejected"));

    gpu::note_unfinished_submission(999999);
    for (u32 i = 0; i < 9999; i++) gpu::note_rejected_readback();
    n = gpu::format_line(s, line, sizeof(line));
    std::printf("%s\n(%zu chars)\n", line, n);
    std::fflush(stdout);
    assert(n + 1 < sizeof(line));
    assert(!std::strncmp(line, "PS4 VRAM: GPU oldest unfinished submission 999999 ms; DMA: rejected readbacks 9999; heap0 ", 90));
    assert(std::strstr(line, "device lost no"));

    // Absurd values: the line is cut at the buffer, the rare fields still come first.
    s.heap_usage[0] = s.heap_budget[0] = s.heap_usage[1] = s.heap_budget[1] = 99999ull << 20;
    s.descriptor_storage = 99999999ull << 10;
    s.shader_text_released = 99999999ull << 10;
    s.host_cap_mib = 65536;
    n = gpu::format_line(s, line, sizeof(line));
    assert(n == sizeof(line) - 1 || n + 1 < sizeof(line));
    assert(std::strlen(line) == n);
    assert(!std::strncmp(line, "PS4 VRAM: GPU oldest unfinished submission", 42));

    gpu::note_unfinished_submission(0);
    assert(gpu::unfinished_submission_ms() == 0);
}
void test_gpu_hang_stop()
{
    // Liveness rule: 10 s unfinished and 10 s in the foreground.
    assert(!gpu::hang_confirmed(9'999'999, 60'000'000));
    assert(!gpu::hang_confirmed(60'000'000, 9'999'999));
    assert(gpu::hang_confirmed(10'000'000, 10'000'000));

    assert(gpu::hang_stop());
    gpu::set_hang_stop(false);
    assert(!gpu::hang_stop());
    gpu::set_hang_stop(true);

    // The stop request carries the cause and its notice; the first request of a game wins.
    vram::reset_stop_request();
    assert(vram::request_stop(vram::stop_cause::gpu_hang, "a command buffer submitted 10500 ms ago has not finished"));
    assert(!vram::request_stop(vram::stop_cause::vram, "later"));
    char detail[vram::stop_detail_size];
    assert(vram::take_stop_request(detail, sizeof(detail)) == vram::stop_cause::gpu_hang);
    assert(!std::strcmp(detail, "a command buffer submitted 10500 ms ago has not finished"));
    assert(!std::strcmp(vram::stop_notice(vram::stop_cause::gpu_hang), "A GPU parou de responder: o jogo foi fechado"));
    assert(std::strlen(vram::stop_notice(vram::stop_cause::gpu_hang)) < 110);
    vram::reset_stop_request();

    // Dead FIFO (dead-fifo-stop.flag): its own cause value and notice.
    static_assert(static_cast<u32>(vram::stop_cause::rsx_dead_fifo) == 4);
    assert(vram::request_stop(vram::stop_cause::rsx_dead_fifo, "Dead FIFO"));
    assert(!vram::request_stop(vram::stop_cause::cpu_heap, "later"));
    assert(vram::take_stop_request(detail, sizeof(detail)) == vram::stop_cause::rsx_dead_fifo);
    assert(!std::strcmp(detail, "Dead FIFO"));
    assert(!std::strcmp(vram::stop_notice(vram::stop_cause::rsx_dead_fifo), "O vídeo do jogo parou (Dead FIFO): o jogo foi fechado"));
    assert(std::strlen(vram::stop_notice(vram::stop_cause::rsx_dead_fifo)) < 110);
    vram::reset_stop_request();
}

void test_fatal_rating_and_backoff()
{
    using vram::mib;
    const u64 window = vram::window_bytes_default;

    // GTA IV 0.38 (history/2): VMA blocks 628 MiB of the 640 MiB budget (98%), live allocations ~415 MiB, window used
    // 873 of 991 MiB (RADV 245), largest free range 100 MiB: a full window of half-empty blocks is not fatal.
    assert(vram::window_load_percent(628 * mib, 640 * mib, 873 * mib, window, 32 * mib) > 95.f);
    assert(!vram::fatal_load(415 * mib, 628 * mib, 640 * mib, 873 * mib, window, 32 * mib, 100 * mib));
    // The same with the allocations themselves at the budget: fatal.
    assert(vram::fatal_load(615 * mib, 628 * mib, 640 * mib, 873 * mib, window, 32 * mib, 100 * mib));
    // No free range of 64 MiB left: fatal whatever the load.
    assert(vram::fatal_load(300 * mib, 400 * mib, 640 * mib, 600 * mib, window, 0, 63 * mib));
    assert(!vram::fatal_load(300 * mib, 400 * mib, 640 * mib, 600 * mib, window, 0, 64 * mib));
    // Window part: RADV's own objects count, the VMA slack does not.
    assert(vram::fatal_load(400 * mib, 400 * mib, 640 * mib, 930 * mib, window, 0, 64 * mib));
    assert(!vram::fatal_load(400 * mib, 600 * mib, 640 * mib, 930 * mib, window, 0, 64 * mib));
    // Without the meter (size 0) only the allocations against the budget count.
    assert(vram::fatal_load(620 * mib, 620 * mib, 640 * mib, 0, 0, 0, 0));
    assert(!vram::fatal_load(500 * mib, 640 * mib, 640 * mib, 0, 0, 0, 0));
    // Blocks below the allocations (dedicated allocations rounding): no negative slack.
    assert(!vram::fatal_load(100 * mib, 90 * mib, 640 * mib, 200 * mib, window, 0, 500 * mib));

    // Relief: the blocks fell by 16 MiB or more.
    assert(vram::fatal_trim_relieved(600 * mib, 584 * mib));
    assert(!vram::fatal_trim_relieved(600 * mib, 585 * mib));
    assert(!vram::fatal_trim_relieved(600 * mib, 700 * mib));

    // At most one per second; 4 s after a futile one; never before the first; a clock going back does not block.
    assert(!vram::fatal_trim_back_off(5'000'000, 0, false));
    assert(vram::fatal_trim_back_off(5'999'999, 5'000'000, true));
    assert(!vram::fatal_trim_back_off(6'000'000, 5'000'000, true));
    assert(vram::fatal_trim_back_off(8'999'999, 5'000'000, false));
    assert(!vram::fatal_trim_back_off(9'000'000, 5'000'000, false));
    assert(!vram::fatal_trim_back_off(4'000'000, 5'000'000, false));

    // 0.38 every-frame fatal trims at 30 FPS for 60 s (1800, 3600 hard syncs) against the new cap: at most 60 or 15.
    u32 trims_relieved = 0, trims_futile = 0;
    u64 last_relieved = 0, last_futile = 0;
    for (u64 t = 1'000'000; t < 61'000'000; t += 33'333)
    {
        if (!vram::fatal_trim_back_off(t, last_relieved, true)) { trims_relieved++; last_relieved = t; }
        if (!vram::fatal_trim_back_off(t, last_futile, false)) { trims_futile++; last_futile = t; }
    }
    assert(trims_relieved <= 61 && trims_relieved >= 59);
    assert(trims_futile <= 16 && trims_futile >= 14);
}

void test_ring_growth()
{
    using vram::mib;

    // Rate rule: more than 2 reclaims in each of 3 consecutive seconds.
    {
        vram::ring_rate r{};
        u64 t = 1'000'000;
        bool fired = false;
        for (u32 second = 0; second < 3 && !fired; second++)
        {
            for (u32 i = 0; i < 3; i++)
            {
                fired = vram::ring_rate_note(r, t + second * 1'000'000 + i * 100'000);
                assert(!fired || (second == 2 && i == 2));
            }
        }
        assert(fired);
        // The streak starts over after it fired.
        assert(!vram::ring_rate_note(r, t + 3'000'000));
    }
    {
        // Two reclaims per second never fire.
        vram::ring_rate r{};
        for (u32 second = 0; second < 30; second++)
        {
            assert(!vram::ring_rate_note(r, 5'000'000 + second * 1'000'000));
            assert(!vram::ring_rate_note(r, 5'000'000 + second * 1'000'000 + 500'000));
        }
    }
    {
        // A quiet second in between breaks the streak.
        vram::ring_rate r{};
        const auto burst = [&](u64 start) { bool f = false; for (u32 i = 0; i < 3; i++) f |= vram::ring_rate_note(r, start + i * 1000); return f; };
        assert(!burst(1'000'000));
        assert(!burst(2'000'000));
        assert(!burst(4'100'000)); // more than 2 s after the start of the last busy second
        assert(!burst(5'100'000));
        assert(burst(6'100'000));
    }
    {
        // Bursts late in each second still count as consecutive seconds.
        vram::ring_rate r{};
        assert(!vram::ring_rate_note(r, 10'000'000));
        for (u32 i = 0; i < 2; i++) assert(!vram::ring_rate_note(r, 10'900'000 + i));
        for (u32 i = 0; i < 3; i++) assert(!vram::ring_rate_note(r, 11'950'000 + i));
        bool f = false;
        for (u32 i = 0; i < 3; i++) f |= vram::ring_rate_note(r, 12'990'000 + i);
        assert(f);
    }
    {
        // A clock that goes back restarts the rule instead of underflowing.
        vram::ring_rate r{};
        for (u32 i = 0; i < 3; i++) vram::ring_rate_note(r, 50'000'000 + i);
        assert(!vram::ring_rate_note(r, 1'000));
        assert(r.busy_seconds == 0 && r.reclaims == 1);
    }

    // Targets: doubling up to the maximum.
    assert(vram::ring_growth_target(8 * mib, 32 * mib) == 16 * mib);
    assert(vram::ring_growth_target(16 * mib, 32 * mib) == 32 * mib);
    assert(vram::ring_growth_target(32 * mib, 32 * mib) == 0);
    assert(vram::ring_growth_target(48 * mib, 64 * mib) == 64 * mib);
    assert(vram::ring_growth_target(64 * mib, 64 * mib) == 0);
    assert(vram::ring_growth_target(8 * mib, 16 * mib) == 16 * mib);

    // Admission: largest free range >= new + 16 MiB and load < 85 %; no meter admits nothing.
    assert(vram::ring_growth_admitted(16 * mib, 32 * mib, 84.9f));
    assert(!vram::ring_growth_admitted(16 * mib, 32 * mib - 1, 50.f));
    assert(!vram::ring_growth_admitted(16 * mib, 447 * mib, 85.f));
    assert(!vram::ring_growth_admitted(16 * mib, 0, 0.f));

    // Record: "key=MiB" tokens.
    assert(vram::ring_record_key("transform constants buffer") == "transform_constants_buffer");
    assert(vram::ring_record_mib("", "attrib buffer") == 0);
    std::string rec = vram::ring_record_set("", "transform constants buffer", 16);
    assert(rec == "transform_constants_buffer=16");
    rec = vram::ring_record_set(rec, "attrib buffer", 48);
    assert(rec == "transform_constants_buffer=16 attrib_buffer=48");
    rec = vram::ring_record_set(rec, "transform constants buffer", 32);
    assert(rec == "transform_constants_buffer=32 attrib_buffer=48");
    assert(vram::ring_record_mib(rec, "transform constants buffer") == 32);
    assert(vram::ring_record_mib(rec + "\n", "attrib buffer") == 48);
    assert(vram::ring_record_mib(rec, "index buffer") == 0);
    rec = vram::ring_record_set(rec, "attrib buffer", 0);
    assert(rec == "transform_constants_buffer=32");

    // Hand-edited or damaged records: bad tokens are ignored (and dropped on rewrite), CRLF and tabs accepted.
    const std::string bad = "attrib_buffer=abc index_buffer= =16 Index_Buffer=16 vertex_env_buffer=0 x=999999 fragment_env_buffer=8\r\n\tindex_buffer=16";
    assert(vram::ring_record_mib(bad, "attrib buffer") == 0);
    assert(vram::ring_record_mib(bad, "vertex env buffer") == 0);
    assert(vram::ring_record_mib(bad, "fragment env buffer") == 8);
    assert(vram::ring_record_mib(bad, "index buffer") == 16);
    assert(vram::ring_record_set(bad, "index buffer", 8) == "fragment_env_buffer=8 index_buffer=8");
    // A duplicated key: the last token wins when read, one token is kept on rewrite.
    assert(vram::ring_record_mib("index_buffer=8 index_buffer=16", "index buffer") == 16);
    assert(vram::ring_record_set("index_buffer=8 index_buffer=16", "index buffer", 16) == "index_buffer=16");
    // At most ring_record_max_tokens tokens are read.
    std::string many;
    for (u32 i = 0; i < 40; i++) many += "k" + std::to_string(i) + "=1 ";
    assert(vram::ring_record_mib(many, "k15") == 1);
    assert(vram::ring_record_mib(many, "k16") == 0);
    // Every managed ring of the Vulkan backend and the auxiliary upload heap at their largest size fit one adapt record
    // (512 bytes with the newline, at most ring_record_max_tokens tokens).
    const char* const rings[] = {"attrib buffer", "fragment env buffer", "vertex env buffer", "fragment texture params buffer",
        "vertex layout buffer", "fragment constants buffer", "transform constants buffer", "index buffer", "texture upload buffer",
        "raster env buffer", "instancing data buffer", "vertex instructions buffer", "fragment instructions buffer",
        "auxilliary upload heap"};
    static_assert(std::size(rings) <= vram::ring_record_max_tokens);
    std::string full;
    for (const char* name : rings) full = vram::ring_record_set(full, name, 64);
    assert(full.size() + 1 <= 512);
    for (const char* name : rings) assert(vram::ring_record_mib(full, name) == 64);
}

void test_aux_upload_heap_growth()
{
    using vram::mib;

    // The auxiliary upload heap uses the ring rule with one note per upload that did not fit it. Sniper Elite V2 0.38:
    // 128 such uploads in 21 s (about 6 per second): busy in the third second; 16 -> 32 -> 64 MiB, then no target.
    vram::ring_rate rate{};
    u64 busy_at = 0;
    for (u64 t = 1'000'000; t < 22'000'000 && !busy_at; t += 166'000)
    {
        if (vram::ring_rate_note(rate, t)) busy_at = t;
    }
    assert(busy_at && busy_at - 1'000'000 < 3'200'000);
    assert(vram::ring_growth_target(16 * mib, 64 * mib) == 32 * mib);
    assert(vram::ring_growth_target(32 * mib, 64 * mib) == 64 * mib);
    assert(vram::ring_growth_target(64 * mib, 64 * mib) == 0); // rings-full.flag: 64 MiB from boot, never grows

    // One upload every 2 s (a level load now and then) never makes it busy.
    vram::ring_rate sparse{};
    for (u64 t = 1'000'000; t < 120'000'000; t += 2'000'000) assert(!vram::ring_rate_note(sparse, t));
}

// Stop notice texts in the four interface languages (VRAM_ ids) and the encoded XMB notice (util/ps4_xmb_notice.hpp).
void test_stop_notice_i18n()
{
    namespace i18n = utils::ps4::i18n;
    namespace notice = utils::ps4::xmb_notice;
    const vram::stop_cause causes[] = {vram::stop_cause::vram, vram::stop_cause::cpu_heap, vram::stop_cause::gpu_hang, vram::stop_cause::rsx_dead_fifo};
    const i18n::lang langs[] = {i18n::lang::pt_br, i18n::lang::en, i18n::lang::es, i18n::lang::ar};
    assert(i18n::current() == i18n::lang::pt_br);

    for (const vram::stop_cause cause : causes)
    {
        const bool compared = cause == vram::stop_cause::vram || cause == vram::stop_cause::cpu_heap;
        assert(!std::strcmp(vram::stop_notice(cause), vram::stop_text(cause, i18n::lang::pt_br)));
        for (const i18n::lang l : langs)
        {
            const std::size_t length = std::strlen(vram::stop_text(cause, l));
            assert(length > 0 && length < 110);
            // Plain message lines: whole after the frontend's 70-byte cut that makes room for the restart advice
            if (!compared) assert(length <= 70);
            if (l != i18n::lang::pt_br) assert(std::strcmp(vram::stop_text(cause, l), vram::stop_text(cause, i18n::lang::pt_br)) != 0);
        }
    }
    assert(!std::strcmp(vram::stop_notice(vram::stop_cause::vram), "Memória de vídeo esgotada: o jogo foi fechado e vai usar menos memória"));
    assert(!std::strcmp(vram::stop_notice(vram::stop_cause::cpu_heap), "Memória do emulador esgotada: o jogo foi fechado"));
    assert(!std::strcmp(vram::stop_text(vram::stop_cause::vram, i18n::lang::en), "Video memory exhausted: the game was closed and will use less memory"));
    assert(!*vram::stop_text(vram::stop_cause::none, i18n::lang::en));
    assert(!*vram::stop_notice(vram::stop_cause::none));

    assert(vram::stop_notice_kind(vram::stop_cause::vram) == notice::kind::vram_stop);
    assert(vram::stop_notice_kind(vram::stop_cause::cpu_heap) == notice::kind::heap_stop);
    assert(vram::stop_notice_kind(vram::stop_cause::gpu_hang) == notice::kind::text);
    assert(vram::stop_notice_kind(vram::stop_cause::rsx_dead_fifo) == notice::kind::text);
    assert(vram::stop_notice_encoded(vram::stop_cause::none).empty());
    assert(vram::stop_notice_encoded(vram::stop_cause::none, true).empty());

    // Encoded in the interface language: decodes to its kind, the restart flag and the text of that language
    for (const i18n::lang l : langs)
    {
        i18n::apply(i18n::setting(static_cast<std::uint8_t>(l) + 1), -1);
        assert(i18n::current() == l);
        for (const vram::stop_cause cause : causes)
        {
            for (const bool restart : {false, true})
            {
                const std::string encoded = vram::stop_notice_encoded(cause, restart);
                assert(notice::is_encoded(encoded));
                const notice::decoded d = notice::decode(encoded);
                assert(d.k == vram::stop_notice_kind(cause));
                assert(d.restart_advised == restart);
                assert(d.detail == vram::stop_text(cause, l));
            }
        }
        // Plain notice: vram / cpu_heap stay pt-BR (the launcher compares their start), the others follow the language
        assert(!std::strcmp(vram::stop_notice(vram::stop_cause::vram), vram::stop_text(vram::stop_cause::vram, i18n::lang::pt_br)));
        assert(!std::strcmp(vram::stop_notice(vram::stop_cause::cpu_heap), "Memória do emulador esgotada: o jogo foi fechado"));
        assert(!std::strcmp(vram::stop_notice(vram::stop_cause::gpu_hang), vram::stop_text(vram::stop_cause::gpu_hang, l)));
        assert(!std::strcmp(vram::stop_notice(vram::stop_cause::rsx_dead_fifo), vram::stop_text(vram::stop_cause::rsx_dead_fifo, l)));
    }
    i18n::apply(i18n::setting::automatic, -1);
    assert(i18n::current() == i18n::lang::pt_br);
}
} // namespace

int main()
{
    test_ring_contiguous_free();
    test_ring_guard();
    test_futile_trim_backoff();
    test_descriptor_pool_window();
    test_rp_close_cause_scope();
    test_vram_line();
    test_window_load();
    test_admission();
    test_texture_quota();
    test_eviction_plan();
    test_image_pool();
    test_dma_plan();
    test_reclaimers_and_stop();
    test_gpu_hang_stop();
    test_fatal_rating_and_backoff();
    test_ring_growth();
    test_aux_upload_heap_growth();
    test_stop_notice_i18n();
    std::printf("vram_policy_test: all tests passed\n");
    return 0;
}
