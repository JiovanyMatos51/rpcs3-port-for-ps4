#pragma once

// 0.38: process memory budget (header-only and pure: no RPCS3, PS4 or orbis-compat include; host test
// ps4-port/tests/mem_budget_test.cpp). util/ps4_memory_governor.hpp feeds it every second and applies the result.
//
// The console's direct memory (4608 MiB, one pool) is shared by the system, the GPU driver (Mesa's 991 MiB VA window
// and whatever else it maps), the guest's shared memory, the JIT, RPCS3's own thread stacks and the CPU heap's
// carve-outs, all taken first fit from the same pool. Every owner that can grow has a ceiling, and what it has not used
// of its ceiling yet is a reservation the heap may not take; the heap gets what is left, computed at run time:
//
//   used_gpu = allocated - heap_held - guest - jit - stacks - system_at_boot     (GPU, Mesa and everything else)
//   reserve  = (991 - used_gpu)+ + (1024 - guest)+ + (640 - jit)+ + (64 - stacks)+ + 64 floor (+ 64 if estimated)
//   cap_eff  = min(build cap, heap_held + free_total - reserve), rounded down to 32 MiB
//   room     = cap_eff - heap_live
//
// orbis-compat refuses a new carve-out that would cut into the reservation (mmapDirectSetReserve) and stops at cap_eff
// (mmapDirectSetCap). When the pool's own walk is not available (estimated ledger), the GPU term is the window meter's
// mapped bytes and the reserve gets a 64 MiB margin for what neither can see.
//
// Levels (governor v2), on room:
//   L0  room >= 768 MiB and more than 10 min before L2 at the current slope
//   L1  room < 768 MiB, or L2 within 10 min: compiles serialized, no optional workers, soft caches
//   L2  room < 384 MiB: compiles wait between programs, level-2 shedders, heap trim, one "save" toast per boot
//   L3  room < 128 MiB, or an allocation was refused in the last 30 s: every shedder, the heap reserve goes to the
//       failing request, "save now" toast, the next boot needs a clean process
// A level rises at once. L3 falls to L2 once nothing was refused for 30 s and room is above the L3 threshold plus 64
// MiB; L2 falls once room stayed 128 MiB above the L2 threshold for 60 s with no heap bytes in flexible memory; L1
// falls once room is 64 MiB above the L1 threshold.
//
// 0.39 L1 rules (the 0.38 rules above stay available, /data/rpcs3/governor-v038.flag): L1 below max(512 MiB, cap / 4)
// of room, or when two consecutive 30 s windows each predicted L2 within 10 min (a window that closes during a boot
// compile stage breaks the streak, and the slope rule never applies during one); L1 clears 32 MiB above its threshold.
// At L1 PPU/SPU compiles run one at a time only below 384 MiB of room or while the slope rule holds; otherwise L1 only
// stops optional workers. Compile slots have classes (cpu = PPU/SPU, rsx = RSX pipelines, interp = interpreter
// links): one per class while L1 serializes, one in total from L2 on.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace utils::ps4::mem_budget
{
using u64 = std::uint64_t;
using s64 = std::int64_t;

inline constexpr u64 mib = 1ull << 20;

// Ceilings and reserve terms (see the formulas at the top of this file).
inline constexpr u64 gpu_ceiling = 991 * mib;    // orbis-drm GPU VA window
inline constexpr u64 guest_ceiling = 1024 * mib; // PS3 main + local memory and mappings
inline constexpr u64 jit_ceiling = 640 * mib;    // jit_runtime, PPU link arenas, PPU executable tables
inline constexpr u64 stacks_ceiling = 64 * mib;  // RPCS3-owned thread stacks (32 live x 2 MiB)
inline constexpr u64 floor_bytes = 64 * mib;     // never handed out
inline constexpr u64 estimated_margin = 64 * mib;
inline constexpr u64 cap_step = 32 * mib;

// Levels.
inline constexpr u64 l1_room = 768 * mib;
inline constexpr u64 l2_room = 384 * mib;
inline constexpr u64 l3_room = 128 * mib;
inline constexpr s64 l1_minutes = 10;
inline constexpr u64 l1_clear_margin = 64 * mib;
inline constexpr u64 l2_clear_margin = 128 * mib;
inline constexpr u64 l3_clear_margin = 64 * mib;
inline constexpr u64 l2_clear_hold_us = 60'000'000;
inline constexpr u64 refusal_hold_us = 30'000'000;
inline constexpr u64 slope_window_us = 30'000'000;

constexpr u64 sat_sub(u64 a, u64 b) noexcept
{
    return a > b ? a - b : 0;
}

struct owners
{
    u64 total = 0;          // direct memory of the process (ledger total)
    u64 allocated = 0;      // ledger: allocated bytes in the pool
    u64 free_total = 0;     // ledger: free bytes in the pool
    u64 largest_run = 0;    // ledger: largest free run
    u64 system_at_boot = 0; // ledger: allocated at the probe, the heap's carve-outs excluded
    u64 heap_held = 0;      // carve-outs held by the heap
    u64 heap_live = 0;      // heap bytes in use (the musl small heap counts whole)
    u64 build_cap = 0;      // the heap's build cap (2048 MiB); 0 = unknown, no cap from the budget
    u64 guest = 0;          // guest shared memory
    u64 jit = 0;            // JIT private commits (own stacks excluded)
    u64 stacks = 0;         // RPCS3-owned stacks
    u64 gpu_window = 0;     // window meter: bytes mapped in the GPU window (only used while estimated)
    bool estimated = false; // the ledger has no kernel walk
};

struct result
{
    u64 used_gpu = 0; // GPU and every other owner the budget does not name
    u64 reserve = 0;
    u64 cap_eff = 0;  // heap cap, 0 = keep the build cap (unknown)
    s64 room = 0;     // cap_eff - heap_live (negative when the other owners took the heap's share)
    u64 headroom = 0; // free_total - reserve: what a new carve-out may take now
};

// Bytes to give directLedgerSetExternal while the ledger is estimated: what the heap cannot see.
constexpr u64 external_bytes(u64 guest, u64 jit, u64 stacks, u64 gpu_window) noexcept
{
    return guest + jit + stacks + gpu_window;
}

constexpr u64 round_down(u64 v, u64 step) noexcept
{
    return v / step * step;
}

constexpr result compute(const owners& o) noexcept
{
    result r{};
    if (o.estimated)
    {
        r.used_gpu = o.gpu_window;
    }
    else
    {
        r.used_gpu = sat_sub(sat_sub(sat_sub(sat_sub(sat_sub(o.allocated, o.heap_held), o.guest), o.jit), o.stacks), o.system_at_boot);
    }
    r.reserve = sat_sub(gpu_ceiling, r.used_gpu) + sat_sub(guest_ceiling, o.guest) + sat_sub(jit_ceiling, o.jit) +
        sat_sub(stacks_ceiling, o.stacks) + floor_bytes + (o.estimated ? estimated_margin : 0);
    r.headroom = sat_sub(o.free_total, r.reserve);
    if (o.build_cap)
    {
        const u64 allowed = sat_sub(o.heap_held + o.free_total, r.reserve);
        r.cap_eff = round_down(allowed < o.build_cap ? allowed : o.build_cap, cap_step);
        r.room = static_cast<s64>(r.cap_eff) - static_cast<s64>(o.heap_live);
    }
    return r;
}

// Room change per minute between two samples (bytes per minute; 0 without a usable interval).
constexpr s64 slope_per_min(s64 room0, u64 t0_us, s64 room1, u64 t1_us) noexcept
{
    if (t1_us <= t0_us) return 0;
    const s64 dt = static_cast<s64>(t1_us - t0_us);
    return (room1 - room0) * 60'000'000 / dt;
}

// The level the thresholds give (no hysteresis). 'slope' in bytes per minute.
constexpr int raw_level(s64 room, s64 slope, bool refused_recently) noexcept
{
    if (refused_recently || room < static_cast<s64>(l3_room)) return 3;
    if (room < static_cast<s64>(l2_room)) return 2;
    if (room < static_cast<s64>(l1_room)) return 1;
    if (slope < 0 && (room - static_cast<s64>(l2_room)) < -slope * l1_minutes) return 1;
    return 0;
}

struct level_state
{
    int level = 0;
    u64 calm_since_us = 0; // L2: since when room has been above its clear threshold (0 = not)
};

// One step of the level machine (see the top of this file). Returns the new level. 'l1_clear_room': room at which L1
// falls (0.39: its threshold + l1_clear_margin_v039).
constexpr int step_level(level_state& st, int raw, s64 room, bool fallback_live, u64 now_us, u64 l1_clear_room = l1_room + l1_clear_margin) noexcept
{
    if (raw >= st.level)
    {
        st.level = raw;
        st.calm_since_us = 0;
        return st.level;
    }
    if (st.level == 3)
    {
        st.calm_since_us = 0;
        if (room >= static_cast<s64>(l3_room + l3_clear_margin)) st.level = 2;
        return st.level;
    }
    if (st.level == 2)
    {
        if (room >= static_cast<s64>(l2_room + l2_clear_margin) && !fallback_live)
        {
            if (!st.calm_since_us) st.calm_since_us = now_us ? now_us : 1;
            else if (now_us - st.calm_since_us >= l2_clear_hold_us)
            {
                st.level = raw;
                st.calm_since_us = 0;
            }
        }
        else
        {
            st.calm_since_us = 0;
        }
        return st.level;
    }
    // st.level == 1, raw == 0
    if (room >= static_cast<s64>(l1_clear_room)) st.level = 0;
    return st.level;
}

// 0.39 L1 rules (see the top of this file).
inline constexpr u64 l1_room_min_v039 = 512 * mib;
inline constexpr u64 l1_clear_margin_v039 = 32 * mib;
inline constexpr unsigned slope_windows_v039 = 2;
inline constexpr u64 cpu_serialize_room = 384 * mib; // below it L1 serializes PPU/SPU compiles (the 0.37 floor)

constexpr u64 l1_room_for(u64 cap_eff) noexcept
{
    return cap_eff / 4 > l1_room_min_v039 ? cap_eff / 4 : l1_room_min_v039;
}

// The slope (bytes per minute) brings room down to the L2 threshold within l1_minutes.
constexpr bool slope_predicts_l2(s64 room, s64 slope) noexcept
{
    return slope < 0 && (room - static_cast<s64>(l2_room)) < -slope * l1_minutes;
}

// Consecutive closed 30 s windows that predicted L2; a window that closes during a boot compile stage breaks it.
constexpr unsigned next_slope_streak(unsigned streak, s64 room, s64 slope, bool compile_stage) noexcept
{
    if (compile_stage || !slope_predicts_l2(room, slope)) return 0;
    return streak < 1000 ? streak + 1 : streak;
}

// The slope rule of the 0.39 L1: enough predicting windows, the current room still predicts L2, no compile stage.
constexpr bool slope_rule_v039(s64 room, s64 slope, unsigned streak, bool compile_stage) noexcept
{
    return !compile_stage && streak >= slope_windows_v039 && slope_predicts_l2(room, slope);
}

constexpr int raw_level_v039(s64 room, s64 slope, bool refused_recently, u64 l1_threshold, unsigned streak, bool compile_stage) noexcept
{
    if (refused_recently || room < static_cast<s64>(l3_room)) return 3;
    if (room < static_cast<s64>(l2_room)) return 2;
    if (room < static_cast<s64>(l1_threshold)) return 1;
    return slope_rule_v039(room, slope, streak, compile_stage) ? 1 : 0;
}

// Whether L1 also serializes PPU/SPU compiles (otherwise it only stops optional workers).
constexpr bool l1_serializes_cpu(s64 room, bool slope_rule) noexcept
{
    return room < static_cast<s64>(cpu_serialize_room) || slope_rule;
}

// The same decision at the time of use: 'floor' (room below cpu_serialize_room, flexible memory low, or the 0.38 rules)
// holds in any stage; the slope rule, sampled without regard to the stage, never counts while a compile stage runs.
constexpr bool cpu_l1_in_stage(bool floor, bool slope_rule_any_stage, bool compile_stage) noexcept
{
    return floor || (slope_rule_any_stage && !compile_stage);
}

// Compile slot classes (util/ps4_memory_governor.hpp compile_slot). The slots held are packed in one word, 10 bits
// per class, so the class and total checks are one compare-and-swap.
enum class slot_kind : unsigned char
{
    cpu = 0,    // PPU modules, SPU programs (the default)
    rsx = 1,    // RSX pipeline jobs
    interp = 2, // shader interpreter variant links
};

enum class slot_mode : unsigned char
{
    parallel,      // no limit
    one_per_class, // one slot per class
    one_in_total,  // one slot in the whole process
    two_in_total,  // "Modo frio" during a boot compile stage: two slots in the whole process
};

inline constexpr unsigned slot_bits = 10;
inline constexpr unsigned slot_mask = (1u << slot_bits) - 1;

constexpr unsigned slot_shift(slot_kind k) noexcept
{
    return static_cast<unsigned>(k) * slot_bits;
}

constexpr unsigned slot_unit(slot_kind k) noexcept
{
    return 1u << slot_shift(k);
}

constexpr unsigned slots_of(unsigned packed, slot_kind k) noexcept
{
    return (packed >> slot_shift(k)) & slot_mask;
}

constexpr unsigned slots_total(unsigned packed) noexcept
{
    return slots_of(packed, slot_kind::cpu) + slots_of(packed, slot_kind::rsx) + slots_of(packed, slot_kind::interp);
}

// A process with no slot held always admits (work never stops).
constexpr bool slot_admits(slot_mode mode, unsigned packed, slot_kind k) noexcept
{
    if (!slots_total(packed)) return true;
    switch (mode)
    {
    case slot_mode::parallel: return slots_of(packed, k) < slot_mask;
    case slot_mode::one_per_class: return slots_of(packed, k) == 0;
    case slot_mode::one_in_total: return false;
    case slot_mode::two_in_total: return slots_total(packed) < 2;
    }
    return false;
}

// 0.39 slot mode. 'level': memory level 0..3; 'cpu_l1': L1 serializes PPU/SPU (l1_serializes_cpu); 'threads_high':
// thread level 2; 'cool': "Modo frio"; 'cool_stage': a boot compile stage runs (no game code yet), so "Modo frio"
// allows two compiles in total instead of one. Memory and thread pressure still win.
constexpr slot_mode slot_mode_v039(int level, bool cpu_l1, bool threads_high, bool cool, bool cool_stage = false) noexcept
{
    if (level >= 2 || (cool && !cool_stage)) return slot_mode::one_in_total;
    if ((level == 1 && cpu_l1) || threads_high) return slot_mode::one_per_class;
    if (cool) return slot_mode::two_in_total;
    return slot_mode::parallel;
}

// 0.38 slot mode (the default rules): every compile shares one slot from memory level 1, at thread level 2 and in
// "Modo frio"; otherwise no limit.
constexpr slot_mode slot_mode_v038(int level, bool threads_high, bool cool) noexcept
{
    return level > 0 || threads_high || cool ? slot_mode::one_in_total : slot_mode::parallel;
}

// Only PPU/SPU compiles wait up to 2 s for the level to fall at L2 (0.38: every compile); RSX jobs never do, the RSX
// already draws through the interpreter or skips draws while a job waits.
constexpr bool slot_waits_for_level(slot_kind k, int level, bool v038) noexcept
{
    return level >= 2 && (v038 || k == slot_kind::cpu);
}

// Compile admission (PPU modules, SPU programs, RSX pipelines): the working set of the compiles in flight stays below
// compile_budget and leaves room above the L2 threshold. The first compile always passes, so work never stops.
inline constexpr u64 compile_budget = 384 * mib;

constexpr bool compile_admit(s64 room, u64 in_flight, u64 bytes, int level) noexcept
{
    if (!in_flight) return true;
    if (level >= 2) return false;
    if (in_flight + bytes > compile_budget) return false;
    return room - static_cast<s64>(in_flight + bytes) >= static_cast<s64>(l2_room);
}

// Optional compile workers (beyond the base count): only at L0, with 1 GiB of room plus the recorded per-worker peak
// for each extra worker (140 MiB when nothing was recorded).
inline constexpr u64 optional_worker_base_room = 1024 * mib;
inline constexpr u64 optional_worker_default_peak = 140 * mib;

constexpr bool optional_workers_allowed(int level, s64 room, unsigned extra, u64 per_worker_peak) noexcept
{
    if (level != 0) return false;
    const u64 peak = per_worker_peak ? per_worker_peak : optional_worker_default_peak;
    return room >= static_cast<s64>(optional_worker_base_room + extra * peak);
}

// ---------------------------------------------------------------------------------------------------------------
// Per-title memory record, written at every stop of a game (adapt folder, <TITLE>.mem): the heap it needed above the
// heap live at its boot, and the peaks of the other owners, in MiB. The frontend compares it with the room before a
// game boots in a process that already ran another game.
inline constexpr u64 default_heap_need = 1229 * mib; // ~1.2 GiB for a title without a record

struct record
{
    u64 heap = 0;   // peak heap live - heap live at boot
    u64 jit = 0;
    u64 guest = 0;
    u64 window = 0;
    u64 boot = 0;   // heap live at boot
    bool valid = false;
};

// "mem heap=1170 jit=345 guest=812 window=710 boot=130" (MiB). 'out' >= 96 bytes. Allocation-free.
inline void format_record(const record& r, char* out, std::size_t size) noexcept
{
    std::snprintf(out, size, "mem heap=%llu jit=%llu guest=%llu window=%llu boot=%llu", static_cast<unsigned long long>(r.heap / mib),
        static_cast<unsigned long long>(r.jit / mib), static_cast<unsigned long long>(r.guest / mib), static_cast<unsigned long long>(r.window / mib),
        static_cast<unsigned long long>(r.boot / mib));
}

// Parses the text of a record file (the last "mem " line wins; unknown keys ignored). valid only with a heap value.
inline record parse_record(const char* text, std::size_t size) noexcept
{
    record out{};
    std::size_t i = 0;
    while (i < size)
    {
        std::size_t end = i;
        while (end < size && text[end] != '\n') end++;
        if (end - i >= 4 && std::memcmp(text + i, "mem ", 4) == 0)
        {
            record r{};
            bool heap_seen = false;
            std::size_t p = i + 4;
            while (p < end)
            {
                while (p < end && text[p] == ' ') p++;
                const std::size_t key = p;
                while (p < end && text[p] != '=' && text[p] != ' ') p++;
                if (p >= end || text[p] != '=') break;
                const std::size_t key_len = p - key;
                p++;
                u64 v = 0;
                bool digits = false;
                while (p < end && text[p] >= '0' && text[p] <= '9' && v < (1ull << 40))
                {
                    v = v * 10 + static_cast<u64>(text[p] - '0');
                    p++;
                    digits = true;
                }
                while (p < end && text[p] != ' ') p++;
                if (!digits) continue;
                const auto is = [&](const char* name) { return std::strlen(name) == key_len && std::memcmp(text + key, name, key_len) == 0; };
                if (is("heap"))
                {
                    r.heap = v * mib;
                    heap_seen = true;
                }
                else if (is("jit")) r.jit = v * mib;
                else if (is("guest")) r.guest = v * mib;
                else if (is("window")) r.window = v * mib;
                else if (is("boot")) r.boot = v * mib;
            }
            if (heap_seen)
            {
                r.valid = true;
                out = r;
            }
        }
        i = end + 1;
    }
    return out;
}

// Record of the session that just ended, merged with the previous one (each field keeps its larger value: a short
// session to the menu must not hide what a long one needed).
constexpr record merge_records(const record& previous, const record& now) noexcept
{
    if (!previous.valid) return now;
    record r = now;
    r.heap = previous.heap > now.heap ? previous.heap : now.heap;
    r.jit = previous.jit > now.jit ? previous.jit : now.jit;
    r.guest = previous.guest > now.guest ? previous.guest : now.guest;
    r.window = previous.window > now.window ? previous.window : now.window;
    r.valid = true;
    return r;
}

// Heap room a boot needs: the record's heap need, else default_heap_need.
constexpr u64 heap_need(const record& r) noexcept
{
    return r.valid && r.heap ? r.heap : default_heap_need;
}

// Pre-boot check. A process that has not run a game yet always boots (nothing better can happen by restarting it).
constexpr bool boot_fits(bool fresh_process, s64 room, const record& r) noexcept
{
    return fresh_process || room >= static_cast<s64>(heap_need(r));
}
} // namespace utils::ps4::mem_budget
