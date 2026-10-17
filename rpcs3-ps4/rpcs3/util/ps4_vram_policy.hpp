#pragma once

// 0.38: GPU window policy of the Vulkan backend (header-only, no Vulkan headers; host test ps4-port/tests/vram_policy_test.cpp).
// - The window meter: the orbis-drm GPU VA window (size, bytes mapped, largest free range) as Mesa's
//   ac_drm_query_heap_info reports it, RADV's own objects included. The backend samples it at most every 250 ms and
//   publishes it here for the budget line.
// - The pure rules on top of it: window load, admission control of large requests, the texture cache quota and the
//   age-based eviction plan, the reusable image pool caps, the DMA mirror age-out plan.
// - Deep reclaim callbacks other parts of the renderer register (shader interpreter, pipelines), run by the backend on
//   the RSX thread when a required allocation failed.
// - The stop request the backend raises when a required allocation cannot be served even after the deep reclaim and
//   the emergency reserve: the frontend stops the game and returns to the XMB with a notice.

#include "util/types.hpp"
#include "util/ps4_i18n.hpp"       // stop notice texts (tr: constant tables, no allocation)
#include "util/ps4_xmb_notice.hpp" // encoded stop notice

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <string>
#include <string_view>

namespace utils::ps4::vram
{
inline constexpr u64 mib = 0x100000;

// orbis-drm GPU VA window of the Mesa build (every Vulkan heap and RADV's own objects live in it).
inline constexpr u64 window_bytes_default = 991 * mib;
// The window's last 32 MiB never count as usable: RADV pad pages and the 16 KiB orbis-drm gap after every range.
inline constexpr u64 window_margin = 32 * mib;
// The meter walks the live VA list under Mesa's VA lock (2-5k ranges): sampled at most this often.
inline constexpr u64 meter_period_us = 250'000;

// Load of the window in percent: the larger of the allocator's blocks against its budget and the bytes mapped in the
// whole window (less the emergency reserve, held on purpose) against the window less the margin. used/size 0 = no meter.
inline float window_load_percent(u64 vma_bytes, u64 vma_budget, u64 used, u64 size, u64 held_reserve) noexcept
{
    float load = 0.f;
    if (vma_budget)
    {
        load = static_cast<float>(vma_bytes) * 100.f / static_cast<float>(vma_budget);
    }

    if (size > window_margin)
    {
        const u64 usable = size - window_margin;
        const u64 counted = used > held_reserve ? used - held_reserve : 0;
        const float window = static_cast<float>(counted) * 100.f / static_cast<float>(usable);
        load = std::max(load, window);
    }

    return load;
}

// Admission control: a request of 4 MiB or more needs a free range that holds it plus 16 MiB of headroom; below that,
// the backend reclaims before it allocates.
inline constexpr u64 admission_min_request = 4 * mib;
inline constexpr u64 admission_headroom = 16 * mib;
// At most one reclaim for admission per period (the meter is re-read after it).
inline constexpr u64 admission_period_us = meter_period_us;

inline bool admission_short(u64 request, u64 largest_hole) noexcept
{
    return request >= admission_min_request && largest_hole < request + admission_headroom;
}

// Texture cache quota: never above 192 MiB nor a quarter of the window budget (upstream: 192 MiB for a 768 MiB cap,
// a share of the device heap that RADV reports as 1322 MiB here).
inline constexpr u64 texture_quota_max = 192 * mib;

inline u64 texture_quota(u64 upstream_quota, u64 window_budget) noexcept
{
    u64 quota = std::min(upstream_quota, texture_quota_max);
    if (window_budget)
    {
        quota = std::min(quota, window_budget / 4);
    }

    return quota;
}

// Window load (percent) at which textures are evicted by age, and the age (frames since the last lookup) that makes a
// texture a candidate. The eviction aims at this load, and frees at least evict_min_bytes.
inline constexpr float evict_load_percent = 90.f;
inline constexpr u64 evict_target_percent = 85;
inline constexpr u64 evict_min_age_frames = 300;
inline constexpr u64 evict_min_bytes = 16 * mib;
// At most one age-based eviction per second (severe ratings repeat several times per frame).
inline constexpr u64 evict_period_us = 1'000'000;

// Bytes to free so that the load falls to evict_target_percent: from the window meter when it is valid, else from the
// allocator's blocks against their budget.
inline u64 eviction_target(u64 vma_bytes, u64 vma_budget, u64 used, u64 size, u64 held_reserve) noexcept
{
    u64 over = 0;
    if (size > window_margin)
    {
        const u64 usable = size - window_margin;
        const u64 counted = used > held_reserve ? used - held_reserve : 0;
        const u64 want = usable / 100 * evict_target_percent + usable % 100 * evict_target_percent / 100;
        over = counted > want ? counted - want : 0;
    }
    else if (vma_budget)
    {
        const u64 want = vma_budget / 100 * evict_target_percent + vma_budget % 100 * evict_target_percent / 100;
        over = vma_bytes > want ? vma_bytes - want : 0;
    }

    return std::max(over, evict_min_bytes);
}

struct evict_candidate
{
    u64 last_use = 0; // frame of the last lookup (0 = never looked up since it was created: oldest)
    u64 bytes = 0;    // GPU memory of its image
    u32 base = 0;     // guest address of the section
    bool evict = false;
};

// Marks candidates to evict, least recently used first: only those unused for at least min_age frames, until
// target_bytes are freed (0 = all of them). Sorts the array. Returns the bytes marked.
inline u64 plan_eviction(evict_candidate* c, std::size_t n, u64 now_frame, u64 min_age, u64 target_bytes) noexcept
{
    std::sort(c, c + n, [](const evict_candidate& a, const evict_candidate& b) { return a.last_use < b.last_use; });

    for (std::size_t i = 0; i < n; i++)
    {
        c[i].evict = false;
    }

    u64 marked = 0;
    for (std::size_t i = 0; i < n; i++)
    {
        if (target_bytes && marked >= target_bytes)
        {
            break;
        }

        if (c[i].last_use > now_frame || now_frame - c[i].last_use < min_age)
        {
            // Sorted: every later candidate is as recent.
            break;
        }

        c[i].evict = true;
        marked += c[i].bytes;
    }

    return marked;
}

// Reusable image pool (images whose sections were destroyed, kept for reuse): at most 64 entries and 32 MiB.
inline constexpr std::size_t image_pool_max_entries = 64;
inline constexpr u64 image_pool_max_bytes = 32 * mib;

inline bool image_pool_over(std::size_t entries, u64 bytes) noexcept
{
    return entries > image_pool_max_entries || bytes > image_pool_max_bytes;
}

// DMA mirror blocks: released after 30 s without use; above 64 MiB the least recently used ones not used for 2 s.
// The sweep runs every 600 frames on the RSX thread.
inline constexpr u64 dma_idle_us = 30'000'000;
inline constexpr u64 dma_min_age_us = 2'000'000;
inline constexpr u64 dma_pool_cap = 64 * mib;
inline constexpr u32 dma_sweep_frames = 600;

struct dma_candidate
{
    u64 last_use_us = 0;
    u64 bytes = 0;
    u32 start = 0;
    bool pinned = false; // holds data the CPU has not read back yet, or backs a DMA section: never released
    bool release = false;
};

// Marks blocks to release: every unpinned one idle for idle_us, then, while the pool stays above cap, the least
// recently used unpinned ones idle for min_age_us. Sorts the array. Returns the bytes marked.
inline u64 plan_dma_release(dma_candidate* c, std::size_t n, u64 now_us, u64 pool_bytes, u64 idle_us, u64 cap, u64 min_age_us) noexcept
{
    std::sort(c, c + n, [](const dma_candidate& a, const dma_candidate& b) { return a.last_use_us < b.last_use_us; });

    const auto idle_for = [now_us](const dma_candidate& e) { return e.last_use_us < now_us ? now_us - e.last_use_us : 0; };

    u64 marked = 0;
    for (std::size_t i = 0; i < n; i++)
    {
        c[i].release = !c[i].pinned && idle_for(c[i]) >= idle_us;
        marked += c[i].release ? c[i].bytes : 0;
    }

    for (std::size_t i = 0; i < n && pool_bytes > marked + cap; i++)
    {
        if (!c[i].release && !c[i].pinned && idle_for(c[i]) >= min_age_us)
        {
            c[i].release = true;
            marked += c[i].bytes;
        }
    }

    return marked;
}

// 0.39: trims rated fatal hard-sync the GPU twice. A fatal rating needs what a trim can change: the bytes allocated in
// the window (VMA's free space inside its blocks left out: the allocator reuses it, and releases emptied blocks itself
// while the window load is vma_release_load_percent or more) over fatal_load_percent, or no free range of
// fatal_hole_bytes left. A window full of half-empty blocks is rated severe.
inline constexpr float fatal_load_percent = 95.f;
inline constexpr u64 fatal_hole_bytes = 64 * mib;
inline constexpr float vma_release_load_percent = 85.f;

inline bool fatal_load(u64 allocated, u64 blocks, u64 budget, u64 used, u64 size, u64 held_reserve, u64 largest_hole) noexcept
{
    const u64 slack = blocks > allocated ? blocks - allocated : 0;
    const u64 counted = used > slack ? used - slack : 0;
    if (window_load_percent(allocated, budget, counted, size, held_reserve) > fatal_load_percent)
    {
        return true;
    }

    return size > window_margin && largest_hole < fatal_hole_bytes;
}

// Window-rated fatal trims: at most one per fatal_trim_period_us; after one that did not lower the allocator's blocks
// by fatal_relief_bytes (measured at the next fatal rating, once the garbage collector freed what it evicted), the
// next waits fatal_trim_futile_us. Trims for allocations that failed are never backed off.
inline constexpr u64 fatal_trim_period_us = 1'000'000;
inline constexpr u64 fatal_trim_futile_us = 4'000'000;
inline constexpr u64 fatal_relief_bytes = 16 * mib;

constexpr bool fatal_trim_relieved(u64 blocks_before, u64 blocks_now) noexcept
{
    return blocks_before >= blocks_now + fatal_relief_bytes;
}

// True: the window-rated fatal trim runs at severe now. last_us 0 = no fatal trim yet.
constexpr bool fatal_trim_back_off(u64 now_us, u64 last_us, bool relieved) noexcept
{
    if (!last_us || now_us < last_us)
    {
        return false;
    }

    return now_us - last_us < (relieved ? fatal_trim_period_us : fatal_trim_futile_us);
}

// 0.39: adaptive ring growth. A ring that needed a hard sync, or reclaimed more than ring_busy_reclaims times in
// each of ring_busy_seconds consecutive seconds, doubles up to its maximum when the window admits the new size: the
// largest free range holds it plus admission_headroom and the load is below ring_growth_max_load. The renderer grows
// it right after a hard sync (nothing in flight) and keeps the size per title for the next boot (ring record).
inline constexpr u32 ring_busy_reclaims = 2;
inline constexpr u32 ring_busy_seconds = 3;
inline constexpr float ring_growth_max_load = 85.f;
// A growth the allocator refused is not tried again for this long (each try costs a hard sync).
inline constexpr u64 ring_growth_retry_us = 30'000'000;

struct ring_rate
{
    u64 second_start_us = 0; // start of the current one-second bucket, 0 = no reclaim yet
    u32 reclaims = 0;        // reclaims in the current bucket
    u32 busy_seconds = 0;    // consecutive closed buckets with more than ring_busy_reclaims reclaims
};

// One reclaim of the ring at now_us. True when this reclaim makes the current second busy and the previous
// ring_busy_seconds - 1 seconds were busy too; the streak then starts over.
inline bool ring_rate_note(ring_rate& r, u64 now_us) noexcept
{
    if (!r.second_start_us || now_us < r.second_start_us || now_us - r.second_start_us >= 2'000'000)
    {
        // First reclaim, or a whole second without one: no streak.
        r.second_start_us = now_us ? now_us : 1;
        r.reclaims = 0;
        r.busy_seconds = 0;
    }
    else if (now_us - r.second_start_us >= 1'000'000)
    {
        r.busy_seconds = r.reclaims > ring_busy_reclaims ? r.busy_seconds + 1 : 0;
        r.second_start_us += 1'000'000;
        r.reclaims = 0;
    }

    if (++r.reclaims == ring_busy_reclaims + 1 && r.busy_seconds + 1 >= ring_busy_seconds)
    {
        r.busy_seconds = 0;
        r.reclaims = 0;
        r.second_start_us = now_us ? now_us : 1;
        return true;
    }

    return false;
}

// Size a busy ring grows to: twice its size, at most max_bytes; 0 when it is there already.
constexpr u64 ring_growth_target(u64 size, u64 max_bytes) noexcept
{
    return size >= max_bytes ? 0 : std::min(size * 2, max_bytes);
}

// The window admits a ring of new_size bytes (the old ring is released only after the new one exists). No meter
// (largest_hole 0) admits nothing.
inline bool ring_growth_admitted(u64 new_size, u64 largest_hole, float load_percent) noexcept
{
    return largest_hole >= new_size + admission_headroom && load_percent < ring_growth_max_load;
}

// Ring record of a title (util/ps4_adapt.hpp "<title>.rings"): one line of "key=MiB" tokens separated by spaces; the
// key is the ring's name with spaces as '_' ("transform_constants_buffer=16").
inline constexpr std::size_t ring_record_max_tokens = 16;

inline std::string ring_record_key(std::string_view ring_name)
{
    std::string key(ring_name);
    std::replace(key.begin(), key.end(), ' ', '_');
    return key;
}

namespace detail
{
constexpr bool ring_key_char(char c) noexcept
{
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

// Calls fn(key, mib) for each well-formed token ("key=N", key of [a-z0-9_], N plain decimal 1..65535).
template <typename F>
void ring_record_tokens(std::string_view record, F&& fn)
{
    std::size_t tokens = 0;
    while (!record.empty() && tokens < ring_record_max_tokens)
    {
        const std::size_t start = record.find_first_not_of(" \t\r\n");
        if (start == std::string_view::npos) break;
        record.remove_prefix(start);
        const std::size_t end = std::min(record.find_first_of(" \t\r\n"), record.size());
        const std::string_view token = record.substr(0, end);
        record.remove_prefix(end);
        tokens++;

        const std::size_t eq = token.find('=');
        if (eq == 0 || eq == std::string_view::npos || eq + 1 == token.size() || token.size() - eq - 1 > 5) continue;
        const std::string_view key = token.substr(0, eq);
        if (!std::all_of(key.begin(), key.end(), ring_key_char)) continue;
        u32 value = 0;
        bool digits = true;
        for (const char c : token.substr(eq + 1))
        {
            if (c < '0' || c > '9') { digits = false; break; }
            value = value * 10 + static_cast<u32>(c - '0');
        }
        if (!digits || !value || value > 65535) continue;
        fn(key, value);
    }
}
} // namespace detail

// MiB stored for ring_name, 0 when the record has no valid token for it.
inline u32 ring_record_mib(std::string_view record, std::string_view ring_name)
{
    const std::string key = ring_record_key(ring_name);
    u32 found = 0;
    detail::ring_record_tokens(record, [&](std::string_view k, u32 mib) { if (k == key) found = mib; });
    return found;
}

// The record with ring_name set to mib (0 removes it); other valid tokens are kept in order, invalid ones dropped.
inline std::string ring_record_set(std::string_view record, std::string_view ring_name, u32 mib)
{
    const std::string key = ring_record_key(ring_name);
    std::string out;
    bool placed = false;
    detail::ring_record_tokens(record, [&](std::string_view k, u32 v)
    {
        if (k == key)
        {
            if (placed || !mib) return;
            placed = true;
            v = mib;
        }
        if (!out.empty()) out += ' ';
        out += k;
        out += '=';
        out += std::to_string(v);
    });
    if (!placed && mib && std::all_of(key.begin(), key.end(), detail::ring_key_char))
    {
        if (!out.empty()) out += ' ';
        out += key;
        out += '=';
        out += std::to_string(mib);
    }
    return out;
}

// Emergency reserve: one raw 32 MiB device allocation held in the window from the renderer's start. Released only when
// a required allocation failed after the deep reclaim; the allocation is retried in its place and the game is stopped.
inline constexpr u64 emergency_reserve_bytes = 32 * mib;

// Published window state (RSX thread or any allocating thread writes, any thread reads; fields are independent).
struct window_state
{
    u64 time_us = 0;      // last meter sample, 0 = never
    u64 size = 0;         // window size (0: meter unavailable)
    u64 used = 0;         // bytes mapped in the window
    u64 largest_hole = 0; // largest free range
    u64 vma = 0;          // allocator blocks (all heaps)
    u64 reserve = 0;      // emergency reserve held
};

namespace detail
{
inline std::atomic<u64> g_time_us{0};
inline std::atomic<u64> g_size{0};
inline std::atomic<u64> g_used{0};
inline std::atomic<u64> g_hole{0};
inline std::atomic<u64> g_vma{0};
inline std::atomic<u64> g_reserve{0};
} // namespace detail

inline void publish_window(const window_state& s) noexcept
{
    detail::g_size.store(s.size, std::memory_order_relaxed);
    detail::g_used.store(s.used, std::memory_order_relaxed);
    detail::g_hole.store(s.largest_hole, std::memory_order_relaxed);
    detail::g_vma.store(s.vma, std::memory_order_relaxed);
    detail::g_reserve.store(s.reserve, std::memory_order_relaxed);
    detail::g_time_us.store(s.time_us, std::memory_order_release);
}

inline window_state current_window() noexcept
{
    window_state s;
    s.time_us = detail::g_time_us.load(std::memory_order_acquire);
    s.size = detail::g_size.load(std::memory_order_relaxed);
    s.used = detail::g_used.load(std::memory_order_relaxed);
    s.largest_hole = detail::g_hole.load(std::memory_order_relaxed);
    s.vma = detail::g_vma.load(std::memory_order_relaxed);
    s.reserve = detail::g_reserve.load(std::memory_order_relaxed);
    return s;
}

// Deep reclaim callbacks (RSX thread). Each frees what it can at 'level' (2 = severe, 3 = a required allocation failed)
// and returns an estimate of the bytes freed. At most 8; registering twice is a no-op.
using reclaim_fn = u64 (*)(int level) noexcept;
inline constexpr std::size_t max_reclaimers = 8;

namespace detail
{
inline std::atomic<reclaim_fn> g_reclaimers[max_reclaimers]{};
} // namespace detail

inline bool register_reclaimer(reclaim_fn fn) noexcept
{
    if (!fn) return false;
    for (auto& slot : detail::g_reclaimers)
    {
        if (slot.load(std::memory_order_acquire) == fn) return true;
    }
    for (auto& slot : detail::g_reclaimers)
    {
        reclaim_fn expected = nullptr;
        if (slot.compare_exchange_strong(expected, fn, std::memory_order_acq_rel)) return true;
    }
    return false;
}

inline void unregister_reclaimer(reclaim_fn fn) noexcept
{
    for (auto& slot : detail::g_reclaimers)
    {
        reclaim_fn expected = fn;
        slot.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
    }
}

inline u64 run_reclaimers(int level) noexcept
{
    u64 freed = 0;
    for (auto& slot : detail::g_reclaimers)
    {
        if (const reclaim_fn fn = slot.load(std::memory_order_acquire))
        {
            freed += fn(level);
        }
    }
    return freed;
}

// Stop request: a required allocation could not be served. The first request of a game wins; the frontend takes it
// once (take_stop_request), stops the game and shows stop_notice() in the XMB. The renderer of the next game clears it.
enum class stop_cause : u32
{
    none = 0,
    vram = 1,     // the GPU window
    cpu_heap = 2, // the emulator's CPU heap (an RSX container)
    gpu_hang = 3, // 0.39: the GPU stopped finishing work (util/ps4_gpu_health.hpp hang_confirmed)
    rsx_dead_fifo = 4, // 0.39: the RSX command FIFO died (only with /data/rpcs3/dead-fifo-stop.flag)
};

inline constexpr std::size_t stop_detail_size = 160;

namespace detail
{
inline std::atomic<u32> g_stop_cause{0};
inline std::atomic<bool> g_stop_ready{false};
inline std::atomic<bool> g_stop_taken{false};
inline std::atomic<u32> g_stop_requests{0};
inline char g_stop_detail[stop_detail_size]{};
} // namespace detail

// One XMB line in 'l' (the notice line holds about 110 characters; the frontend may append restart advice).
// Constant text (VRAM_ ids), allocation-free; empty for stop_cause::none.
inline const char* stop_text(stop_cause cause, i18n::lang l) noexcept
{
    switch (cause)
    {
    case stop_cause::vram: return i18n::tr(i18n::str::VRAM_MEMORIA_VIDEO_ESGOTADA_JOGO_FOI, l);
    case stop_cause::cpu_heap: return i18n::tr(i18n::str::VRAM_MEMORIA_EMULADOR_ESGOTADA_JOGO_FOI, l);
    case stop_cause::gpu_hang: return i18n::tr(i18n::str::VRAM_GPU_PAROU_RESPONDER_JOGO_FOI, l);
    case stop_cause::rsx_dead_fifo: return i18n::tr(i18n::str::VRAM_VIDEO_JOGO_PAROU_DEAD_FIFO, l);
    default: return "";
    }
}

// The plain notice the frontend passes to the XMB. The vram / cpu_heap texts stay pt-BR: the launcher compares their
// start until it decodes util/ps4_xmb_notice.hpp notices. The other causes are a message line in the interface
// language (at most 70 bytes in every language, so the restart advice never cuts them).
inline const char* stop_notice(stop_cause cause) noexcept
{
    const bool compared = cause == stop_cause::vram || cause == stop_cause::cpu_heap;
    return stop_text(cause, compared ? i18n::lang::pt_br : i18n::current());
}

// XMB notice kind of a stop: the two out-of-memory stops have their own dialog, the others are a message line.
inline xmb_notice::kind stop_notice_kind(stop_cause cause) noexcept
{
    switch (cause)
    {
    case stop_cause::vram: return xmb_notice::kind::vram_stop;
    case stop_cause::cpu_heap: return xmb_notice::kind::heap_stop;
    default: return xmb_notice::kind::text;
    }
}

// The stop notice in the encoded form of util/ps4_xmb_notice.hpp: kind, restart flag and, as detail, the text in the
// interface language. Empty for stop_cause::none.
inline std::string stop_notice_encoded(stop_cause cause, bool restart_advised = false)
{
    const char* const text = stop_text(cause, i18n::current());
    if (!*text) return {};
    return xmb_notice::encode(stop_notice_kind(cause), text, restart_advised);
}

// True for the first request since the last reset. Allocation-free, any thread.
inline bool request_stop(stop_cause cause, const char* why) noexcept
{
    detail::g_stop_requests.fetch_add(1, std::memory_order_relaxed);
    u32 expected = 0;
    if (cause == stop_cause::none ||
        !detail::g_stop_cause.compare_exchange_strong(expected, static_cast<u32>(cause), std::memory_order_acq_rel))
    {
        return false;
    }

    std::size_t n = 0;
    while (why && why[n] && n + 1 < stop_detail_size)
    {
        detail::g_stop_detail[n] = why[n];
        n++;
    }
    detail::g_stop_detail[n] = '\0';
    detail::g_stop_ready.store(true, std::memory_order_release);
    return true;
}

inline bool stop_requested() noexcept
{
    return detail::g_stop_ready.load(std::memory_order_acquire);
}

inline u32 stop_requests() noexcept
{
    return detail::g_stop_requests.load(std::memory_order_relaxed);
}

// Returns the cause once per request (none otherwise) and copies the detail. Frontend main loop.
inline stop_cause take_stop_request(char* out, std::size_t size) noexcept
{
    if (!detail::g_stop_ready.load(std::memory_order_acquire) || detail::g_stop_taken.exchange(true, std::memory_order_acq_rel))
    {
        return stop_cause::none;
    }

    if (out && size)
    {
        std::size_t n = 0;
        while (detail::g_stop_detail[n] && n + 1 < size)
        {
            out[n] = detail::g_stop_detail[n];
            n++;
        }
        out[n] = '\0';
    }

    return static_cast<stop_cause>(detail::g_stop_cause.load(std::memory_order_relaxed));
}

// A new renderer (the next game) starts without a request.
inline void reset_stop_request() noexcept
{
    detail::g_stop_ready.store(false, std::memory_order_relaxed);
    detail::g_stop_taken.store(false, std::memory_order_relaxed);
    detail::g_stop_detail[0] = '\0';
    detail::g_stop_cause.store(0, std::memory_order_release);
}
} // namespace utils::ps4::vram
