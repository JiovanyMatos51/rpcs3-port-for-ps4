// Host test of the 0.38 process memory budget (rpcs3/util/ps4_mem_budget.hpp): the reservation formula and the heap
// cap it gives, checked against the 0.37 console figures (FIFA 14 at its fatal, the XMB before a game) and the example
// budget line worked out by hand; the governor v2 levels with their hysteresis; compile admission; the per-title record.
// 0.39: the L1 rules (relative threshold, two-window slope outside compile stages) and the compile slot classes.
//
// Build and run from any scratch directory (llvm-mingw):
//   clang++ -std=c++20 -static -Wall -Wextra -I rpcs3 ps4-port/tests/mem_budget_test.cpp -o mem_budget_test.exe
//   mem_budget_test.exe
#include "util/ps4_mem_budget.hpp"
#include "util/ps4_memory_governor.hpp" // 0.39: host part only (thread cap file and levels)

#include <cstdio>
#include <cstring>
#include <string>

namespace mb = utils::ps4::mem_budget;

namespace
{
int g_failures = 0;

#define CHECK(expr) \
    do \
    { \
        if (!(expr)) \
        { \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #expr); \
            g_failures++; \
        } \
    } while (0)

constexpr mb::u64 MiB = mb::mib;
constexpr mb::u64 total = 4608 * MiB;
constexpr mb::u64 sys = 156 * MiB;

// A walked ledger for the given owners (MiB).
mb::owners walked(mb::u64 gpu, mb::u64 guest, mb::u64 jit, mb::u64 stacks, mb::u64 held, mb::u64 live)
{
    mb::owners o{};
    o.total = total;
    o.system_at_boot = sys;
    o.heap_held = held * MiB;
    o.heap_live = live * MiB;
    o.guest = guest * MiB;
    o.jit = jit * MiB;
    o.stacks = stacks * MiB;
    o.build_cap = 2048 * MiB;
    o.allocated = sys + gpu * MiB + o.guest + o.jit + o.stacks + o.heap_held;
    o.free_total = total - o.allocated;
    o.largest_run = o.free_total;
    return o;
}

void test_reservation()
{
    // FIFA 14 at the 0.37 fatal: GPU+other ~1110, guest 767, JIT 398, stacks 2, heap held 2176 (with the overdraft):
    // the pool was empty
    {
        const mb::owners o = walked(1109, 767, 398, 2, 2176, 1920);
        CHECK(o.free_total == 0);
        const mb::result r = mb::compute(o);
        CHECK(r.used_gpu == 1109 * MiB);
        CHECK(r.reserve == 625 * MiB); // 0 GPU + 257 guest + 242 JIT + 62 stacks + 64 floor
        // 4608 - 156 - 1110 - 1024 - 640 - 64 - 64 = 1550, rounded down to 32 MiB
        CHECK(r.cap_eff == 1536 * MiB);
        CHECK(r.room < 0);  // the heap held more than its share: level 3 and no new carve-out
        CHECK(r.headroom == 0);
        std::printf("FIFA 14 at the 0.37 fatal: reserve %llu MiB, cap %llu MiB (1550 before rounding), room %lld MiB\n",
            static_cast<unsigned long long>(r.reserve / MiB), static_cast<unsigned long long>(r.cap_eff / MiB), static_cast<long long>(r.room / static_cast<mb::s64>(MiB)));
    }
    // The same moment with the heap at its 0.38 share: the 13th carve-out (held 1664) is refused, the JIT keeps 242 MiB
    {
        const mb::owners o = walked(1110, 767, 398, 2, 1536, 1400);
        const mb::result r = mb::compute(o);
        CHECK(r.cap_eff == 1536 * MiB);
        CHECK(r.headroom < 128 * MiB);
        CHECK(o.free_total - 128 * MiB < r.reserve); // a 128 MiB carve-out would cut into the reservation
        CHECK(mb::sat_sub(mb::jit_ceiling, o.jit) == 242 * MiB);
    }
    // Example budget line: gpu+other 1046, guest 812, JIT 344, stacks 4, heap held 1408 live 812, free 838
    {
        mb::owners o = walked(1046, 812, 344, 4, 1408, 812);
        CHECK(o.free_total == 838 * MiB);
        const mb::result r = mb::compute(o);
        CHECK(r.reserve == 632 * MiB);
        CHECK(r.cap_eff == 1600 * MiB); // 1614 rounded down to 32 MiB
        CHECK(r.room == static_cast<mb::s64>(788 * MiB));
        CHECK(mb::raw_level(r.room, 0, false) == 0);
    }
    // The XMB before any game (only system and a few MiB of everything else): the heap cap lands in 1536..1696 MiB
    {
        const mb::owners o = walked(20, 0, 8, 2, 128, 115);
        const mb::result r = mb::compute(o);
        CHECK(r.cap_eff >= 1536 * MiB && r.cap_eff <= 1696 * MiB);
        std::printf("XMB before a game: reserve %llu MiB, cap %llu MiB, room %lld MiB\n", static_cast<unsigned long long>(r.reserve / MiB),
            static_cast<unsigned long long>(r.cap_eff / MiB), static_cast<long long>(r.room / static_cast<mb::s64>(MiB)));
    }
    // Owners past their ceilings never get a negative reservation term; the cap never passes the build cap
    {
        const mb::owners o = walked(1300, 1100, 700, 80, 300, 200);
        const mb::result r = mb::compute(o);
        CHECK(r.reserve == mb::floor_bytes);
        mb::owners small = walked(0, 0, 0, 0, 0, 0);
        small.build_cap = 1024 * MiB;
        CHECK(mb::compute(small).cap_eff == 1024 * MiB);
    }
    // Unknown build cap: no cap from the budget
    {
        mb::owners o = walked(100, 0, 0, 0, 128, 100);
        o.build_cap = 0;
        CHECK(mb::compute(o).cap_eff == 0);
    }
}

void test_estimated()
{
    // Without the kernel walk the heap sees free = total - boot - held - external, where external is what the budget
    // feeds; the GPU term is the window meter. The cap equals the walked one less the 64 MiB margin.
    const mb::owners w = walked(700, 800, 300, 4, 1024, 900);
    mb::owners e = w;
    e.estimated = true;
    e.gpu_window = 700 * MiB;
    const mb::u64 external = mb::external_bytes(e.guest, e.jit, e.stacks, e.gpu_window);
    CHECK(external == (800 + 300 + 4 + 700) * MiB);
    e.free_total = total - sys - e.heap_held - external;
    e.allocated = total - e.free_total;
    const mb::result rw = mb::compute(w);
    const mb::result re = mb::compute(e);
    CHECK(re.used_gpu == 700 * MiB);
    CHECK(re.reserve == rw.reserve + mb::estimated_margin);
    CHECK(re.cap_eff + mb::estimated_margin == rw.cap_eff);
    // The GPU term cancels out: a larger window gives the same cap (it is inside its ceiling)
    mb::owners e2 = e;
    e2.gpu_window = 900 * MiB;
    e2.free_total = total - sys - e2.heap_held - mb::external_bytes(e2.guest, e2.jit, e2.stacks, e2.gpu_window);
    CHECK(mb::compute(e2).cap_eff == re.cap_eff);
}

void test_levels()
{
    const mb::s64 M = static_cast<mb::s64>(MiB);
    CHECK(mb::raw_level(900 * M, 0, false) == 0);
    CHECK(mb::raw_level(700 * M, 0, false) == 1);
    CHECK(mb::raw_level(300 * M, 0, false) == 2);
    CHECK(mb::raw_level(100 * M, 0, false) == 3);
    CHECK(mb::raw_level(-50 * M, 0, false) == 3);
    CHECK(mb::raw_level(1500 * M, 0, true) == 3);
    // Slope: 900 MiB of room falling 60 MiB/min reaches L2 (384) in < 10 min; 30 MiB/min does not
    CHECK(mb::raw_level(900 * M, -60 * M, false) == 1);
    CHECK(mb::raw_level(900 * M, -30 * M, false) == 0);
    CHECK(mb::raw_level(900 * M, 100 * M, false) == 0);
    CHECK(mb::slope_per_min(1000 * M, 0, 970 * M, 30'000'000) == -60 * M);
    CHECK(mb::slope_per_min(1000 * M, 10, 970 * M, 10) == 0);

    // Rises at once; L3 -> L2 only after the refusal hold and above 192 MiB; L2 clears after 60 s above 512 MiB with
    // no fallback; L1 clears above 832 MiB.
    mb::level_state st{};
    mb::u64 t = 1'000'000;
    CHECK(mb::step_level(st, mb::raw_level(1000 * M, 0, false), 1000 * M, false, t) == 0);
    CHECK(mb::step_level(st, mb::raw_level(700 * M, 0, false), 700 * M, false, t += 1'000'000) == 1);
    CHECK(mb::step_level(st, mb::raw_level(800 * M, 0, false), 800 * M, false, t += 1'000'000) == 1); // inside the margin
    CHECK(mb::step_level(st, mb::raw_level(900 * M, 0, false), 900 * M, false, t += 1'000'000) == 0);
    CHECK(mb::step_level(st, mb::raw_level(900 * M, 0, true), 900 * M, false, t += 1'000'000) == 3); // a refusal
    CHECK(mb::step_level(st, mb::raw_level(150 * M, 0, false), 150 * M, false, t += 1'000'000) == 3); // below 192
    CHECK(mb::step_level(st, mb::raw_level(300 * M, 0, false), 300 * M, false, t += 1'000'000) == 2);
    CHECK(mb::step_level(st, mb::raw_level(600 * M, 0, false), 600 * M, true, t += 1'000'000) == 2); // fallback live
    CHECK(mb::step_level(st, mb::raw_level(600 * M, 0, false), 600 * M, false, t += 1'000'000) == 2); // calm starts
    CHECK(mb::step_level(st, mb::raw_level(600 * M, 0, false), 600 * M, false, t += 30'000'000) == 2);
    CHECK(mb::step_level(st, mb::raw_level(450 * M, 0, false), 450 * M, false, t += 1'000'000) == 2); // calm broken
    CHECK(mb::step_level(st, mb::raw_level(600 * M, 0, false), 600 * M, false, t += 1'000'000) == 2);
    CHECK(mb::step_level(st, mb::raw_level(600 * M, 0, false), 600 * M, false, t += 59'000'000) == 2);
    CHECK(mb::step_level(st, mb::raw_level(600 * M, 0, false), 600 * M, false, t += 1'000'000) == 1); // 60 s calm
    CHECK(mb::step_level(st, mb::raw_level(1200 * M, 0, false), 1200 * M, false, t += 1'000'000) == 0);
    // No sticky level: a long session that touched L3 once ends at L0
    CHECK(st.level == 0);
}

// 0.39 L1 rules and compile slot classes
void test_levels_v039()
{
    const mb::s64 M = static_cast<mb::s64>(MiB);
    CHECK(mb::l1_room_for(1536 * MiB) == 512 * MiB);
    CHECK(mb::l1_room_for(2560 * MiB) == 640 * MiB);
    CHECK(mb::l1_room_for(0) == 512 * MiB);
    const mb::u64 l1 = mb::l1_room_for(1600 * MiB);

    // PES 2013: room 1544 MiB, one -983 MiB/min window during the boot load: 0.38 level 1, 0.39 level 0
    CHECK(mb::raw_level(1544 * M, -983 * M, false) == 1);
    CHECK(mb::raw_level_v039(1544 * M, -983 * M, false, l1, 1, false) == 0);
    CHECK(mb::raw_level_v039(1544 * M, -983 * M, false, l1, 2, false) == 1); // two windows in a row
    CHECK(mb::raw_level_v039(1544 * M, -983 * M, false, l1, 2, true) == 0);  // never during a compile stage
    // The 2 h hold at room 813-821 with slope 0 (0.38 hysteresis band): level 0
    CHECK(mb::raw_level(816 * M, 0, false) == 0 && mb::raw_level_v039(816 * M, 0, false, l1, 0, false) == 0);
    CHECK(mb::raw_level(700 * M, 0, false) == 1);
    CHECK(mb::raw_level_v039(700 * M, 0, false, l1, 0, false) == 0);
    CHECK(mb::raw_level_v039(450 * M, 0, false, l1, 0, false) == 1);
    CHECK(mb::raw_level_v039(300 * M, 0, false, l1, 0, false) == 2);
    CHECK(mb::raw_level_v039(100 * M, 0, false, l1, 0, false) == 3);
    CHECK(mb::raw_level_v039(1500 * M, 0, true, l1, 0, false) == 3);

    // Streak: predicting windows count up, a window in a compile stage or a flat one resets it
    unsigned streak = 0;
    streak = mb::next_slope_streak(streak, 900 * M, -60 * M, false);
    CHECK(streak == 1);
    streak = mb::next_slope_streak(streak, 870 * M, -60 * M, false);
    CHECK(streak == 2);
    CHECK(mb::slope_rule_v039(870 * M, -60 * M, streak, false));
    CHECK(!mb::slope_rule_v039(870 * M, -60 * M, streak, true));
    CHECK(mb::next_slope_streak(streak, 870 * M, -60 * M, true) == 0);
    CHECK(mb::next_slope_streak(streak, 870 * M, 0, false) == 0);
    CHECK(mb::next_slope_streak(streak, 900 * M, -30 * M, false) == 0); // 10 min of -30 MiB/min stays above L2

    // L1 clears 32 MiB above its threshold
    mb::level_state st{};
    const mb::u64 clear = l1 + mb::l1_clear_margin_v039;
    mb::u64 t = 1'000'000;
    CHECK(mb::step_level(st, mb::raw_level_v039(500 * M, 0, false, l1, 0, false), 500 * M, false, t, clear) == 1);
    CHECK(mb::step_level(st, mb::raw_level_v039(530 * M, 0, false, l1, 0, false), 530 * M, false, t += 1'000'000, clear) == 1);
    CHECK(mb::step_level(st, mb::raw_level_v039(550 * M, 0, false, l1, 0, false), 550 * M, false, t += 1'000'000, clear) == 0);

    // Level 1 serializes PPU/SPU only below 384 MiB or on the slope rule
    CHECK(!mb::l1_serializes_cpu(450 * M, false));
    CHECK(mb::l1_serializes_cpu(380 * M, false));
    CHECK(mb::l1_serializes_cpu(900 * M, true));

    // Second console (base PS4, FW 9.00, Splinter Cell Blacklist history/2/startup.log)
    // :232 cap 1824, live 45, room 1778 MiB, slope -336 MiB/min after one window: 0.38 level 1 (as logged), 0.39 level 0
    {
        const mb::u64 l1_sc = mb::l1_room_for(1824 * MiB);
        CHECK(l1_sc == 512 * MiB);
        CHECK(mb::raw_level(1778 * M, -336 * M, false) == 1);
        const unsigned one = mb::next_slope_streak(0, 1778 * M, -336 * M, false);
        CHECK(one == 1);
        CHECK(mb::raw_level_v039(1778 * M, -336 * M, false, l1_sc, one, false) == 0);
    }
    // :288 cap 1600, room 929 MiB, slope -170 MiB/min over two windows inside the 97-min first boot's compile stage:
    // 0.38 level 1 (as logged), 0.39 level 0; the same two windows outside a compile stage: level 1 by design (the slope
    // reaches level 2 within l1_minutes), and PPU/SPU compiles serialize in gameplay at that slope
    {
        unsigned in_stage = mb::next_slope_streak(0, 950 * M, -170 * M, true);
        in_stage = mb::next_slope_streak(in_stage, 929 * M, -170 * M, true);
        CHECK(in_stage == 0);
        CHECK(mb::raw_level(929 * M, -170 * M, false) == 1);
        CHECK(mb::raw_level_v039(929 * M, -170 * M, false, l1, in_stage, true) == 0);
        CHECK(mb::raw_level_v039(929 * M, -170 * M, false, l1, 2, true) == 0);

        unsigned outside = mb::next_slope_streak(0, 950 * M, -170 * M, false);
        outside = mb::next_slope_streak(outside, 929 * M, -170 * M, false);
        CHECK(outside == 2);
        CHECK(mb::raw_level_v039(929 * M, -170 * M, false, l1, outside, false) == 1);
        CHECK(mb::l1_serializes_cpu(929 * M, mb::slope_rule_v039(929 * M, -170 * M, outside, false)));
    }
}

void test_slots()
{
    using K = mb::slot_kind;
    using S = mb::slot_mode;
    CHECK(mb::slot_mode_v039(0, true, false, false) == S::parallel);
    CHECK(mb::slot_mode_v039(1, false, false, false) == S::parallel);
    CHECK(mb::slot_mode_v039(1, true, false, false) == S::one_per_class);
    CHECK(mb::slot_mode_v039(0, false, true, false) == S::one_per_class);
    CHECK(mb::slot_mode_v039(2, false, false, false) == S::one_in_total);
    CHECK(mb::slot_mode_v039(3, false, false, false) == S::one_in_total);
    CHECK(mb::slot_mode_v039(0, false, false, true) == S::one_in_total);
    // "Modo frio" during a boot compile stage: two in total; outside it, during play or under pressure, as before
    CHECK(mb::slot_mode_v039(0, false, false, true, true) == S::two_in_total);
    CHECK(mb::slot_mode_v039(0, false, false, true, false) == S::one_in_total);
    CHECK(mb::slot_mode_v039(1, true, false, true, true) == S::one_per_class);
    CHECK(mb::slot_mode_v039(1, false, false, true, true) == S::two_in_total);
    CHECK(mb::slot_mode_v039(0, false, true, true, true) == S::one_per_class);
    CHECK(mb::slot_mode_v039(2, false, false, true, true) == S::one_in_total);
    CHECK(mb::slot_mode_v039(3, true, true, true, true) == S::one_in_total);
    CHECK(mb::slot_mode_v039(0, false, false, false, true) == S::parallel);
    CHECK(mb::slot_mode_v039(1, true, false, false, true) == S::one_per_class);

    const unsigned one_cpu = mb::slot_unit(K::cpu);
    const unsigned mixed = mb::slot_unit(K::cpu) + 2 * mb::slot_unit(K::rsx) + mb::slot_unit(K::interp);
    CHECK(mb::slots_total(mixed) == 4 && mb::slots_of(mixed, K::rsx) == 2 && mb::slots_of(mixed, K::interp) == 1);
    // Nothing held: every mode admits (work never stops)
    CHECK(mb::slot_admits(S::one_in_total, 0, K::cpu) && mb::slot_admits(S::one_per_class, 0, K::rsx));
    // Level 1 serialized: one cpu + one rsx + one interp
    CHECK(!mb::slot_admits(S::one_per_class, one_cpu, K::cpu));
    CHECK(mb::slot_admits(S::one_per_class, one_cpu, K::rsx));
    CHECK(mb::slot_admits(S::one_per_class, one_cpu + mb::slot_unit(K::rsx), K::interp));
    CHECK(!mb::slot_admits(S::one_per_class, one_cpu + mb::slot_unit(K::rsx), K::rsx));
    // Level 2: one in total
    CHECK(!mb::slot_admits(S::one_in_total, mb::slot_unit(K::interp), K::cpu));
    CHECK(mb::slot_admits(S::parallel, mixed, K::cpu));
    // "Modo frio" compile stage: two slots in the whole process, whatever their classes
    CHECK(mb::slot_admits(S::two_in_total, 0, K::cpu));
    CHECK(mb::slot_admits(S::two_in_total, one_cpu, K::cpu));
    CHECK(mb::slot_admits(S::two_in_total, one_cpu, K::rsx));
    CHECK(!mb::slot_admits(S::two_in_total, 2 * one_cpu, K::cpu));
    CHECK(!mb::slot_admits(S::two_in_total, 2 * one_cpu, K::interp));
    CHECK(!mb::slot_admits(S::two_in_total, one_cpu + mb::slot_unit(K::rsx), K::cpu));
    CHECK(!mb::slot_admits(S::two_in_total, mixed, K::rsx));
    // Only PPU/SPU wait for level 2 to fall (0.38: every class)
    CHECK(mb::slot_waits_for_level(K::cpu, 2, false));
    CHECK(!mb::slot_waits_for_level(K::rsx, 2, false) && !mb::slot_waits_for_level(K::interp, 3, false));
    CHECK(mb::slot_waits_for_level(K::rsx, 2, true));
    CHECK(!mb::slot_waits_for_level(K::cpu, 1, false));
}

// 0.39 thread levels on the console's thread cap and thread-cap.txt
void test_thread_cap()
{
    namespace gov = utils::ps4::governor;
    // Default cap 80: level 1 at 64 threads, level 2 at 72 (live + exited-not-joined)
    CHECK(gov::classify_thread_load(63, 80) == 0);
    CHECK(gov::classify_thread_load(64, 80) == 1);
    CHECK(gov::classify_thread_load(71, 80) == 1);
    CHECK(gov::classify_thread_load(72, 80) == 2);
    // The TTT2/SvR refusals: ~70-74 live + 8-9 unjoined -> level 2 before them
    CHECK(gov::classify_thread_load(70 + 8, 80) == 2);
    // Caps are clamped (a broken file never puts level 2 at a handful of threads)
    CHECK(gov::clamp_thread_cap(10) == gov::thread_cap_min && gov::clamp_thread_cap(100000) == gov::thread_cap_max);
    CHECK(gov::classify_thread_load(40, 1) == 0);

    CHECK(gov::firmware_key("13.020.001 (0x13020001)") == "13.020.001");
    CHECK(gov::firmware_key("  11.000.001") == "11.000.001");
    CHECK(gov::firmware_key("").empty() && gov::firmware_key("   ").empty());

    const std::string_view file = "# thread caps\n12.020.001 82\r\n13.020.001 78\n13.020.001  76 measured again\nbad line\n";
    CHECK(gov::parse_thread_cap(file, "13.020.001 (0x13020001)") == 76); // the last line wins
    CHECK(gov::parse_thread_cap(file, "12.020.001") == 82);
    CHECK(gov::parse_thread_cap(file, "11.000.001") == 0);
    CHECK(gov::parse_thread_cap(file, "") == 0);
    CHECK(gov::parse_thread_cap("13.520.001 12\n", "13.520.001") == gov::thread_cap_min);
    CHECK(gov::parse_thread_cap("13.520.001 x\n", "13.520.001") == 0);

    const std::string updated = gov::with_thread_cap(file, "13.020.001 (0x13020001)", 74);
    CHECK(updated == "# thread caps\n12.020.001 82\n13.020.001 74\nbad line\n");
    CHECK(gov::parse_thread_cap(updated, "13.020.001") == 74);
    const std::string added = gov::with_thread_cap(updated, "13.520.001", 80);
    CHECK(gov::parse_thread_cap(added, "13.520.001") == 80 && gov::parse_thread_cap(added, "12.020.001") == 82);
    CHECK(gov::with_thread_cap("", "13.520.001", 80) == "13.520.001 80\n");
    CHECK(gov::with_thread_cap("x 1\n", "", 80).empty());
}

void test_admission()
{
    const mb::s64 M = static_cast<mb::s64>(MiB);
    CHECK(mb::compile_admit(100 * M, 0, 200 * MiB, 3)); // the first compile always passes
    CHECK(!mb::compile_admit(1500 * M, 100 * MiB, 100 * MiB, 2));
    CHECK(mb::compile_admit(1500 * M, 100 * MiB, 200 * MiB, 0));
    CHECK(!mb::compile_admit(1500 * M, 200 * MiB, 200 * MiB, 0)); // over the 384 MiB budget
    CHECK(!mb::compile_admit(700 * M, 100 * MiB, 250 * MiB, 1));  // would leave < 384 MiB of room
    CHECK(mb::optional_workers_allowed(0, 1400 * M, 2, 0));
    CHECK(!mb::optional_workers_allowed(0, 1200 * M, 2, 0));
    CHECK(!mb::optional_workers_allowed(1, 3000 * M, 1, 0));
    CHECK(mb::optional_workers_allowed(0, 1400 * M, 2, 180 * MiB));
    CHECK(!mb::optional_workers_allowed(0, 1300 * M, 2, 180 * MiB));
}

void test_record()
{
    mb::record r{};
    r.heap = 1170 * MiB;
    r.jit = 345 * MiB;
    r.guest = 812 * MiB;
    r.window = 710 * MiB;
    r.boot = 130 * MiB;
    r.valid = true;
    char line[128];
    mb::format_record(r, line, sizeof(line));
    CHECK(std::strcmp(line, "mem heap=1170 jit=345 guest=812 window=710 boot=130") == 0);
    const mb::record p = mb::parse_record(line, std::strlen(line));
    CHECK(p.valid && p.heap == r.heap && p.jit == r.jit && p.guest == r.guest && p.window == r.window && p.boot == r.boot);

    const char text[] = "# comment\nmem heap=900 jit=100\r\ngarbage line\nmem jit=5\nmem heap=1000 window=x guest=700 extra=3\n";
    const mb::record q = mb::parse_record(text, sizeof(text) - 1);
    CHECK(q.valid && q.heap == 1000 * MiB && q.guest == 700 * MiB && q.jit == 0 && q.window == 0);
    CHECK(!mb::parse_record("", 0).valid);
    CHECK(!mb::parse_record("mem jit=4", 9).valid);

    mb::record small{};
    small.heap = 600 * MiB;
    small.jit = 400 * MiB;
    small.valid = true;
    const mb::record m = mb::merge_records(r, small);
    CHECK(m.heap == 1170 * MiB && m.jit == 400 * MiB && m.guest == 812 * MiB);
    CHECK(mb::merge_records(mb::record{}, small).heap == 600 * MiB);

    const mb::s64 M = static_cast<mb::s64>(MiB);
    CHECK(mb::boot_fits(true, 10 * M, r));             // a fresh process always boots
    CHECK(!mb::boot_fits(false, 1000 * M, r));         // 1170 needed
    CHECK(mb::boot_fits(false, 1200 * M, r));
    CHECK(!mb::boot_fits(false, 1100 * M, mb::record{})); // no record: ~1.2 GiB
    CHECK(mb::boot_fits(false, 1300 * M, mb::record{}));
}
} // namespace

int main()
{
    test_reservation();
    test_estimated();
    test_levels();
    test_levels_v039();
    test_slots();
    test_thread_cap();
    test_admission();
    test_record();
    if (g_failures)
    {
        std::printf("mem_budget_test: %d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("mem_budget_test: all passed\n");
    return 0;
}
