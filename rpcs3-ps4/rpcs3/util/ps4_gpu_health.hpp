#pragma once

// 0.31: GPU health shared by the Vulkan backend and the frontend (header-only, no Vulkan headers).
// - The untimed fence wait calls set_suspect_hang(true) once a fence has not signalled for 10 s (it keeps waiting).
// - die_with_error() calls note_lost() on VK_ERROR_DEVICE_LOST before it throws; the frontend checks lost()
//   before it returns to the XMB, and leaves the app if the emulator cannot stop.
// - The RSX thread publishes a VRAM snapshot; the 60 s health report prints it with format_line().
// - The backend counts severe video memory pressure and the trims other threads had to hand to the RSX thread.
// - 0.34: also the fatal trims it backed off, the occlusion queries that timed out, and (in the snapshot) the CPU
//   heap held by descriptor-set scratch storage, the live descriptor pools and the shader text released.
// - 0.36: the severe trims skipped as futile, the readbacks rejected for impossible parameters, the age of the oldest
//   unfinished submission, the estimated GPU window of the descriptor pools and the pools released; plus the pure
//   rules behind them (ring room, trim back-off, pool window estimate), shared with the host tests.
// Flags are lock-free; the snapshot copy takes a short, bounded spin lock (one writer, rare readers).

#include "util/types.hpp"

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <string_view>
#include <thread>

namespace utils::ps4::gpu
{
inline constexpr u32 max_heaps = 16; // VK_MAX_MEMORY_HEAPS

// An untimed fence wait that has not signalled after this long is reported as a suspected GPU hang.
inline constexpr u64 fence_hang_us = 10'000'000;

// Optional allocator caps for A/B tests without a rebuild: lines "device_mb=N" (applied to
// vram_allocation_limit by the boot policy), "host_mb=N" (applied by the Vulkan allocator to host heaps) and
// "window_mb=N" (the window budget below, read by the Vulkan allocator).
inline constexpr char vram_flag_path[] = "/data/rpcs3/vram.flag";
// Same limits as the boot policy's reader: a larger file is ignored as a whole, and so is any value above the max.
inline constexpr std::size_t vram_flag_max_bytes = 4096;
inline constexpr u32 vram_flag_max_mib = 65536;
// Host heaps hold the upload rings (about 192 MiB) and the DMA mirror pool; a smaller cap fails at boot.
inline constexpr u32 host_cap_min_mib = 224;
inline constexpr u32 host_cap_max_mib = vram_flag_max_mib;
// "window_mb=N": budget for the bytes the allocator holds in all heaps of the ~991 MiB orbis-drm window (trims start
// against it). The default stays 640 MiB; the range keeps room for RADV's own objects in the window.
inline constexpr u32 window_budget_min_mib = 512;
inline constexpr u32 window_budget_max_mib = 832;

struct vram_snapshot
{
    u64 time_us = 0;              // when it was taken, 0 = never published
    u32 heap_count = 0;           // valid entries in the heap arrays
    u32 device_local_mask = 0;    // bit i set: heap i is DEVICE_LOCAL
    u64 heap_usage[max_heaps]{};  // bytes in use per memory heap
    u64 heap_budget[max_heaps]{}; // budget per memory heap
    u64 dma_pool = 0;             // host-visible DMA mirror pool
    u64 texture_cache = 0;        // texture cache bytes in use
    u64 surface_store = 0;        // render target / surface store bytes
    u32 device_cap_mib = 0;       // allocator cap for device-local heaps, 0 = none
    u32 host_cap_mib = 0;         // allocator cap for host heaps, 0 = none
    u32 descriptor_sets = 0;      // live descriptor sets with reserved scratch storage (CPU heap)
    u64 descriptor_storage = 0;   // bytes of that scratch storage
    u32 descriptor_pools = 0;     // live VkDescriptorPool objects
    u32 shaders_released = 0;     // shaders whose GLSL text and SPIR-V were freed after module creation (process)
    u64 shader_text_released = 0; // bytes freed that way
    u64 descriptor_pool_window = 0;   // estimated GPU window bytes of the live descriptor pools
    u32 descriptor_pools_released = 0; // pools of idle program tables released (process)
};

// 0.36: room in a ring buffer (rsx::data_heap): put is the start of the free space, get its end (get..put is in use).
// Largest single allocation that fits without moving get, by the rules of data_heap::can_alloc_impl.
inline u64 ring_contiguous_free(u64 put, u64 get, u64 size) noexcept
{
    if (put <= get) return get - put;
    const u64 tail = size - put;
    return tail > get ? tail : get;
}

// Room a ring keeps free before each draw: its guard size, at most a quarter of the ring, at least 1 MiB.
inline u64 ring_guard(u64 min_guard, u64 size) noexcept
{
    const u64 quarter = size / 4;
    const u64 guard = min_guard < quarter ? min_guard : quarter;
    return guard > 0x100000 ? guard : 0x100000;
}

// 0.36: back-off for severe trims that free nothing. A severe trim rated by the window load alone evicts textures and
// surfaces, which never shrinks the allocator's blocks; the load stays put and the next frame trims again. After
// streak_limit such trims in a row without the block total dropping below its value at the start of the streak, they
// are skipped for backoff_us. The streak survives the pause: one trim runs after it, and if the total is still not
// lower, the next check pauses again. RSX thread only.
struct futile_trim_backoff
{
    static constexpr u32 streak_limit = 64;
    static constexpr u64 backoff_us = 10'000'000;

    u64 reference = 0;     // block total at the start of the streak
    u32 streak = 0;        // window-only severe trims since then
    u64 skip_until_us = 0; // 0 = not paused
    u32 pauses = 0;

    // Called for each window-only severe trim about to run. True: skip it.
    bool should_skip(u64 now_us, u64 block_total) noexcept
    {
        if (skip_until_us)
        {
            if (now_us < skip_until_us) return true;
            skip_until_us = 0;
            streak = streak_limit - 1;
        }

        if (!streak || block_total < reference)
        {
            reference = block_total;
            streak = 1;
            return false;
        }

        if (++streak > streak_limit)
        {
            skip_until_us = now_us + backoff_us;
            pauses++;
            return true;
        }

        return false;
    }
};

// 0.36: why the render pass this thread closes next ends, as an rsx::ps4_rp_close_cause value (0 = untagged). Set
// by rp_close_cause_scope around the code that may close it; vk::end_renderpass counts the close under it. The
// outermost scope wins, so a barrier inside a texture cache upload counts as the upload.
inline thread_local u32 t_rp_close_cause = 0;

class rp_close_cause_scope
{
public:
    explicit rp_close_cause_scope(u32 cause) noexcept
        : m_previous(t_rp_close_cause)
    {
        if (!m_previous) t_rp_close_cause = cause;
    }

    ~rp_close_cause_scope()
    {
        t_rp_close_cause = m_previous;
    }

    rp_close_cause_scope(const rp_close_cause_scope&) = delete;
    rp_close_cause_scope& operator=(const rp_close_cause_scope&) = delete;

private:
    u32 m_previous;
};

// 0.36: GPU window bytes of one VkDescriptorPool on this console, estimated from RADV's pool code: a descriptor buffer
// of `descriptor_bytes` (16 B per buffer, texel buffer or sampler descriptor, 64 B per image or input attachment, 96 B
// per combined image sampler) plus a 16 B alignment reserve per set for the 16-byte descriptors, in 16 KiB pages, then
// RADV's guard page and the orbis-drm gap after every VA range (16 KiB each).
inline u64 descriptor_pool_window_bytes(u64 descriptor_bytes, u64 descriptors_16, u64 max_sets) noexcept
{
    constexpr u64 page = 0x4000;
    const u64 reserve = 16 * (descriptors_16 < max_sets ? descriptors_16 : max_sets);
    const u64 bo = (descriptor_bytes + reserve + page - 1) / page * page;
    return (bo ? bo : page) + 2 * page;
}

namespace detail
{
inline std::atomic<bool> g_suspect_hang{false};
inline std::atomic<u32> g_hang_reports{0};
inline std::atomic<bool> g_lost{false};
inline std::atomic<u32> g_pressure_trims{0};
inline std::atomic<u32> g_deferred_trims{0};
inline std::atomic<u32> g_trim_backoffs{0};
inline std::atomic<u32> g_query_timeouts{0};
inline std::atomic<u32> g_futile_trim_skips{0};
inline std::atomic<u32> g_rejected_readbacks{0};
inline std::atomic<u32> g_unfinished_submission_ms{0};
inline std::atomic<bool> g_snapshot_busy{false};
inline vram_snapshot g_snapshot{};

// The holder only copies a few hundred bytes, so the wait is bounded: if the lock is still taken after a few
// thousand yields (the holder was preempted or interrupted by a signal), the caller gives up instead of stalling
// the RSX thread or a fatal report.
class snapshot_guard
{
public:
    snapshot_guard() noexcept
    {
        for (u32 i = 0; i < 4096; i++)
        {
            if (!g_snapshot_busy.load(std::memory_order_relaxed) && !g_snapshot_busy.exchange(true, std::memory_order_acquire))
            {
                m_owned = true;
                return;
            }
            std::this_thread::yield();
        }
    }

    ~snapshot_guard()
    {
        if (m_owned) g_snapshot_busy.store(false, std::memory_order_release);
    }

    bool owned() const noexcept
    {
        return m_owned;
    }

    snapshot_guard(const snapshot_guard&) = delete;
    snapshot_guard& operator=(const snapshot_guard&) = delete;

private:
    bool m_owned = false;
};
} // namespace detail

// True after a fence wait exceeded 10 s and until the backend clears it.
inline bool suspect_hang() noexcept
{
    return detail::g_suspect_hang.load(std::memory_order_relaxed);
}

// Returns true when this call raised the flag (the caller shows a notification only then).
inline bool set_suspect_hang(bool hang) noexcept
{
    if (!hang)
    {
        detail::g_suspect_hang.store(false, std::memory_order_relaxed);
        return false;
    }
    if (detail::g_suspect_hang.exchange(true, std::memory_order_relaxed)) return false;
    detail::g_hang_reports.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// How many times the hang flag was raised in this process.
inline u32 hang_reports() noexcept
{
    return detail::g_hang_reports.load(std::memory_order_relaxed);
}

// 0.39: a hang confirmed for hang_stop_us stops the game and returns to the XMB instead of waiting forever: an untimed
// fence wait that long (its clock skips time in the background), or the oldest submission unfinished that long while
// the game stayed in the foreground. Off while the hang_stop feature is off (no-hang-stop.flag, safe-mode level 2; set
// when a renderer starts).
inline constexpr u64 hang_stop_us = 10'000'000;

namespace detail
{
inline std::atomic<bool> g_hang_stop{true};
} // namespace detail

inline void set_hang_stop(bool on) noexcept
{
    detail::g_hang_stop.store(on, std::memory_order_relaxed);
}

inline bool hang_stop() noexcept
{
    return detail::g_hang_stop.load(std::memory_order_relaxed);
}

// Liveness rule: the oldest submission unfinished for unfinished_us while the game has been in the foreground for
// foreground_us.
constexpr bool hang_confirmed(u64 unfinished_us, u64 foreground_us) noexcept
{
    return unfinished_us >= hang_stop_us && foreground_us >= hang_stop_us;
}

// True once the device was lost; it stays set for the rest of the process.
inline bool lost() noexcept
{
    return detail::g_lost.load(std::memory_order_acquire);
}

// Marks the device as lost. Returns true for the first call only. Allocation-free, any thread.
inline bool note_lost() noexcept
{
    return !detail::g_lost.exchange(true, std::memory_order_acq_rel);
}

// Counts one severe video memory event (a trim at severity severe or fatal: high usage, or an allocation that ran out
// of memory). Returns the new count. Any thread.
inline u32 note_pressure_trim() noexcept
{
    return detail::g_pressure_trims.fetch_add(1, std::memory_order_relaxed) + 1;
}

inline u32 pressure_trims() noexcept
{
    return detail::g_pressure_trims.load(std::memory_order_relaxed);
}

// Counts one trim requested by a thread that cannot run it (only the RSX thread may evict caches). Returns the new
// count. Any thread, allocation-free.
inline u32 note_deferred_trim() noexcept
{
    return detail::g_deferred_trims.fetch_add(1, std::memory_order_relaxed) + 1;
}

inline u32 deferred_trims() noexcept
{
    return detail::g_deferred_trims.load(std::memory_order_relaxed);
}

// Counts one window-load trim that ran at severe instead of fatal because the previous fatal trim, less than 500 ms
// earlier, freed nothing. Returns the new count. RSX thread.
inline u32 note_trim_backoff() noexcept
{
    return detail::g_trim_backoffs.fetch_add(1, std::memory_order_relaxed) + 1;
}

inline u32 trim_backoffs() noexcept
{
    return detail::g_trim_backoffs.load(std::memory_order_relaxed);
}

// Counts one occlusion query whose result was still unavailable after 2 s (it then counts as visible). Returns the
// new count. Any thread.
inline u32 note_query_timeout() noexcept
{
    return detail::g_query_timeouts.fetch_add(1, std::memory_order_relaxed) + 1;
}

inline u32 query_timeouts() noexcept
{
    return detail::g_query_timeouts.load(std::memory_order_relaxed);
}

// 0.36: counts one window-only severe trim skipped by futile_trim_backoff. Returns the new count. RSX thread.
inline u32 note_futile_trim_skip() noexcept
{
    return detail::g_futile_trim_skips.fetch_add(1, std::memory_order_relaxed) + 1;
}

inline u32 futile_trim_skips() noexcept
{
    return detail::g_futile_trim_skips.load(std::memory_order_relaxed);
}

// 0.36: counts one GPU readback skipped because its parameters were impossible (the CPU copy was kept). Returns the
// new count. Any thread.
inline u32 note_rejected_readback() noexcept
{
    return detail::g_rejected_readbacks.fetch_add(1, std::memory_order_relaxed) + 1;
}

inline u32 rejected_readbacks() noexcept
{
    return detail::g_rejected_readbacks.load(std::memory_order_relaxed);
}

// 0.36: age of the oldest submitted command buffer whose fence has not signalled, when it is at least
// unfinished_report_ms old; 0 clears it. Written by the RSX thread's liveness scan about once per second.
inline constexpr u32 unfinished_report_ms = 3000;

inline void note_unfinished_submission(u32 age_ms) noexcept
{
    detail::g_unfinished_submission_ms.store(age_ms >= unfinished_report_ms ? age_ms : 0, std::memory_order_relaxed);
}

inline u32 unfinished_submission_ms() noexcept
{
    return detail::g_unfinished_submission_ms.load(std::memory_order_relaxed);
}

// Value of "key=N" in vram.flag text: one "key=value" per line, '#' comments, blanks, tabs and CRLF allowed. Lines
// whose value is not a plain decimal number up to vram_flag_max_mib are skipped (the boot policy reads the same
// file the same way); among the valid ones the last wins. Returns 0 when there is none.
inline u32 parse_flag_mib(std::string_view text, std::string_view key) noexcept
{
    u32 result = 0;
    while (!text.empty())
    {
        const std::size_t nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text.remove_prefix(nl == std::string_view::npos ? text.size() : nl + 1);
        auto trim = [](std::string_view s)
        {
            while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
            while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
            return s;
        };
        line = trim(line);
        if (line.empty() || line.front() == '#') continue;
        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos || trim(line.substr(0, eq)) != key) continue;
        const std::string_view value = trim(line.substr(eq + 1));
        u64 v = 0;
        bool ok = !value.empty();
        for (const char c : value)
        {
            // Stop as soon as the value is out of range, so long digit strings cannot overflow.
            if (c < '0' || c > '9' || v > vram_flag_max_mib)
            {
                ok = false;
                break;
            }
            v = v * 10 + static_cast<u64>(c - '0');
        }
        if (ok && v <= vram_flag_max_mib) result = static_cast<u32>(v);
    }
    return result;
}

// Stores a new snapshot. If a reader holds the lock for too long, this update is dropped (the next one lands).
inline void publish(const vram_snapshot& s) noexcept
{
    detail::snapshot_guard guard;
    if (guard.owned()) detail::g_snapshot = s;
}

// Copy of the last published snapshot (time_us == 0 when none was published, or when the lock could not be taken).
inline vram_snapshot snapshot() noexcept
{
    detail::snapshot_guard guard;
    if (!guard.owned()) return {};
    return detail::g_snapshot;
}

// One log line, e.g. "PS4 VRAM: heap0 dev 412/614 MiB, heap1 host 96/528 MiB; dma 64 MiB, textures 210 MiB,
// surfaces 88 MiB; caps device 768 MiB host none; pressure trims 0 (deferred 0), fatal trims backed off 0, futile
// severe trims skipped 0; descriptor storage 412 sets, 24720 KiB, descriptor pools 530 (~25 MiB window), released 0;
// shader text released 830 shaders, 41200 KiB; occlusion query timeouts 0; fence hangs 0; device lost no", with
// " GPU oldest unfinished submission N ms;" and " DMA: rejected readbacks N;" right after "PS4 VRAM:" when non-zero.
// Heap numbers are usage/budget as the allocator reports them. Returns the length.
inline std::size_t format_line(const vram_snapshot& s, char* out, std::size_t size) noexcept
{
    if (!out || !size) return 0;
    std::size_t n = 0;
    auto add = [&](int written)
    {
        if (written > 0) n += static_cast<std::size_t>(written);
        if (n >= size) n = size - 1;
    };
    add(std::snprintf(out, size, "PS4 VRAM:"));
    // Rare fields first: the caller's buffer is 512 bytes and a long line loses its end.
    if (const u32 unfinished = unfinished_submission_ms(); unfinished && n + 1 < size)
        add(std::snprintf(out + n, size - n, " GPU oldest unfinished submission %u ms;", unfinished));
    if (const u32 rejected = rejected_readbacks(); rejected && n + 1 < size)
        add(std::snprintf(out + n, size - n, " DMA: rejected readbacks %u;", rejected));
    if (!s.time_us) add(std::snprintf(out + n, size - n, " no snapshot"));
    const u32 heaps = s.heap_count < max_heaps ? s.heap_count : max_heaps;
    for (u32 i = 0; i < heaps && n + 1 < size; i++)
    {
        add(std::snprintf(out + n, size - n, "%s heap%u %s %llu/%llu MiB", i ? "," : "", i,
            (s.device_local_mask >> i) & 1 ? "dev" : "host", static_cast<unsigned long long>(s.heap_usage[i] >> 20),
            static_cast<unsigned long long>(s.heap_budget[i] >> 20)));
    }
    if (n + 1 < size)
    {
        add(std::snprintf(out + n, size - n, "; dma %llu MiB, textures %llu MiB, surfaces %llu MiB; caps device ",
            static_cast<unsigned long long>(s.dma_pool >> 20), static_cast<unsigned long long>(s.texture_cache >> 20),
            static_cast<unsigned long long>(s.surface_store >> 20)));
    }
    if (n + 1 < size)
    {
        if (s.device_cap_mib) add(std::snprintf(out + n, size - n, "%u MiB", s.device_cap_mib));
        else add(std::snprintf(out + n, size - n, "none"));
    }
    if (n + 1 < size)
    {
        if (s.host_cap_mib) add(std::snprintf(out + n, size - n, " host %u MiB", s.host_cap_mib));
        else add(std::snprintf(out + n, size - n, " host none"));
    }
    if (n + 1 < size)
        add(std::snprintf(out + n, size - n, "; pressure trims %u (deferred %u), fatal trims backed off %u, futile severe trims skipped %u",
            pressure_trims(), deferred_trims(), trim_backoffs(), futile_trim_skips()));
    if (n + 1 < size)
    {
        add(std::snprintf(out + n, size - n, "; descriptor storage %u sets, %llu KiB, descriptor pools %u (~%llu MiB window), released %u",
            s.descriptor_sets, static_cast<unsigned long long>(s.descriptor_storage >> 10), s.descriptor_pools,
            static_cast<unsigned long long>(s.descriptor_pool_window >> 20), s.descriptor_pools_released));
    }
    if (n + 1 < size)
    {
        add(std::snprintf(out + n, size - n, "; shader text released %u shaders, %llu KiB", s.shaders_released,
            static_cast<unsigned long long>(s.shader_text_released >> 10)));
    }
    if (n + 1 < size)
        add(std::snprintf(out + n, size - n, "; occlusion query timeouts %u", query_timeouts()));
    if (n + 1 < size)
        add(std::snprintf(out + n, size - n, "; fence hangs %u; device lost %s", hang_reports(), lost() ? "yes" : "no"));
    out[n] = '\0';
    return n;
}
} // namespace utils::ps4::gpu
