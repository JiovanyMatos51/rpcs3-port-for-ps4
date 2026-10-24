// Host test of the 0.38 process memory budget (rpcs3/util/ps4_mem_budget.hpp): the reservation formula and the heap
// cap it gives, checked against the 0.37 console figures (FIFA 14 at its fatal, the XMB before a game) and the example
// budget line worked out by hand; the governor v2 levels with their hysteresis; compile admission; the per-title record.
// 0.39: the L1 rules (relative threshold, two-window slope outside compile stages) and the compile slot classes.
// 0.40: the paced retry of a refused HLE media create and the reason text of serialized compiles; the optional-thread
// cap after a refusal, the group count of optional threads, the codec line rate and the opt-in boot compile stage rule.
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

// 0.40: the 0.38 rules are the default slot mode; the stage-aware level 1 decision of the 0.39 rules; the optional
// thread rule (the last 8 slots of the console's cap stay with the game)
void test_rules_040()
{
    using K = mb::slot_kind;
    using S = mb::slot_mode;
    namespace gov = utils::ps4::governor;
    const mb::s64 M = static_cast<mb::s64>(MiB);

    // 0.38 rules: any memory level, thread level 2 or "Modo frio" -> one compile in the whole process
    CHECK(mb::slot_mode_v038(0, false, false) == S::parallel);
    CHECK(mb::slot_mode_v038(1, false, false) == S::one_in_total);
    CHECK(mb::slot_mode_v038(2, false, false) == S::one_in_total);
    CHECK(mb::slot_mode_v038(3, false, false) == S::one_in_total);
    CHECK(mb::slot_mode_v038(0, true, false) == S::one_in_total);
    CHECK(mb::slot_mode_v038(0, false, true) == S::one_in_total);
    // ... every class waits for level 2 to fall, none at level 1, and one held slot of any class blocks the others
    for (const K k : {K::cpu, K::rsx, K::interp})
    {
        CHECK(mb::slot_waits_for_level(k, 2, true) && mb::slot_waits_for_level(k, 3, true));
        CHECK(!mb::slot_waits_for_level(k, 1, true));
        CHECK(!mb::slot_admits(S::one_in_total, mb::slot_unit(K::rsx), k));
        CHECK(mb::slot_admits(S::one_in_total, 0, k));
        CHECK(mb::slot_admits(S::parallel, mb::slot_unit(K::cpu) + mb::slot_unit(K::rsx), k));
    }
    // The 0.38 level machine keeps its default: level 1 below 768 MiB, cleared at 832 MiB
    {
        mb::level_state st{};
        CHECK(mb::step_level(st, mb::raw_level(700 * M, 0, false), 700 * M, false, 1'000'000) == 1);
        CHECK(mb::step_level(st, mb::raw_level(831 * M, 0, false), 831 * M, false, 2'000'000) == 1);
        CHECK(mb::step_level(st, mb::raw_level(832 * M, 0, false), 832 * M, false, 3'000'000) == 0);
    }

    // 0.39 rules: a slope-only level 1 never serializes PPU/SPU while a compile stage runs, even when the level was
    // sampled before the stage began; the room floor (below 384 MiB) serializes in any stage
    CHECK(mb::cpu_l1_in_stage(false, true, false));
    CHECK(!mb::cpu_l1_in_stage(false, true, true));
    CHECK(mb::cpu_l1_in_stage(true, false, true) && mb::cpu_l1_in_stage(true, true, true));
    CHECK(!mb::cpu_l1_in_stage(false, false, false));
    CHECK(mb::slot_mode_v039(1, mb::cpu_l1_in_stage(false, true, true), false, false) == S::parallel);
    CHECK(mb::slot_mode_v039(1, mb::cpu_l1_in_stage(false, true, false), false, false) == S::one_per_class);
    CHECK(mb::slot_mode_v039(1, mb::cpu_l1_in_stage(300 * M < static_cast<mb::s64>(mb::cpu_serialize_room), true, true), false, false) == S::one_per_class);
    // Split in two parts, the decision equals l1_serializes_cpu(room, slope_rule_v039(..., stage)) of the 0.39 governor
    for (const mb::s64 room : {300 * M, 450 * M, 900 * M})
        for (const unsigned streak : {0u, 1u, 2u, 5u})
            for (const mb::s64 slope : {mb::s64{0}, -60 * M, -170 * M})
                for (const bool stage : {false, true})
                {
                    const bool floor = room < static_cast<mb::s64>(mb::cpu_serialize_room);
                    const bool slope_any = mb::slope_rule_v039(room, slope, streak, false);
                    CHECK(mb::cpu_l1_in_stage(floor, slope_any, stage) == mb::l1_serializes_cpu(room, mb::slope_rule_v039(room, slope, streak, stage)));
                }

    // Optional threads (cap 80): one up to 71 threads, the three RPCN threads up to 69, a connect (none) up to 72
    CHECK(gov::optional_allowed(71, 1, 80) && !gov::optional_allowed(72, 1, 80));
    CHECK(gov::optional_allowed(69, 3, 80) && !gov::optional_allowed(70, 3, 80));
    CHECK(gov::optional_allowed(72, 0, 80) && !gov::optional_allowed(73, 0, 80));
    // Demon's Souls 0.39 started RPCN at 72 threads of cap 80: refused now
    CHECK(!gov::optional_allowed(72, 3, 80));
    // A file or learned cap is clamped (64 at least)
    CHECK(gov::optional_allowed(55, 1, 10) && !gov::optional_allowed(56, 1, 10));
    CHECK(gov::optional_allowed(1000, 1, 2000));
    // One optional thread is refused exactly where the 0.39 thread level 2 begins
    for (unsigned t = 40; t < 100; t++)
    {
        CHECK(gov::optional_allowed(t, 1, 80) == (gov::classify_thread_load(t, 80) < 2));
    }
    // optional-threads-free.flag: the rule is off and every optional thread may start
    CHECK(gov::optional_decision(false, 79, 3, 80));
    CHECK(!gov::optional_decision(true, 79, 3, 80));
    CHECK(gov::optional_decision(true, 10, 3, 80));

    // Host build: no rule, no reason text
    CHECK(gov::optional_thread_allowed("RPCN start", 3));
    char reason[8] = "x";
    gov::serialize_reason(reason, sizeof(reason));
    CHECK(reason[0] == '\0');
}

// 0.40: one paced retry of a refused HLE media create (the guest create retry is off by default)
void test_media_pacing_040()
{
    namespace gov = utils::ps4::governor;

    // Only the emulator's four media threads, by exact name
    CHECK(gov::is_hle_media_thread("HLE Video Decoder"));
    CHECK(gov::is_hle_media_thread("HLE ATRAC3plus decoder"));
    CHECK(gov::is_hle_media_thread("HLE LPCM decoder"));
    CHECK(gov::is_hle_media_thread("HLE PAMF demuxer"));
    CHECK(!gov::is_hle_media_thread("HLE Video Decoder Reaper"));
    CHECK(!gov::is_hle_media_thread("HLE Video Decoder "));
    CHECK(!gov::is_hle_media_thread("hle video decoder"));
    CHECK(!gov::is_hle_media_thread("HLE LPCM"));
    CHECK(!gov::is_hle_media_thread("HLE SceNpBandwidthTest"));
    CHECK(!gov::is_hle_media_thread("_libsail-control"));
    CHECK(!gov::is_hle_media_thread("_cellVdecCtrlThread"));
    CHECK(!gov::is_hle_media_thread("MainThread"));
    CHECK(!gov::is_hle_media_thread(""));
    static_assert(gov::is_hle_media_thread("HLE PAMF demuxer") && !gov::is_hle_media_thread("HLE PAMF"));

    // Demon's Souls: refused at 78 counted while the cap in force stayed 80. A wait for "below the cap" would create
    // again at once; the wait for a slot below the load at the refusal does wait
    CHECK(78 < gov::thread_cap_default);
    CHECK(gov::media_retry_wait_ms(78, 78, 0) == gov::media_retry_step_ms);
    CHECK(gov::media_retry_wait_ms(79, 78, 30) == 10); // more threads since the refusal
    // A freed slot ends the wait (also before the first sleep)
    CHECK(gov::media_retry_wait_ms(77, 78, 40) == 0);
    CHECK(gov::media_retry_wait_ms(77, 78, 0) == 0);
    CHECK(gov::media_retry_wait_ms(60, 78, 120) == 0);
    // 250 ms ceiling: the last step is cut to what is left, then create once more
    CHECK(gov::media_retry_wait_ms(78, 78, 240) == 10);
    CHECK(gov::media_retry_wait_ms(78, 78, 245) == 5);
    CHECK(gov::media_retry_wait_ms(78, 78, 249) == 1);
    CHECK(gov::media_retry_wait_ms(78, 78, 250) == 0);
    CHECK(gov::media_retry_wait_ms(78, 78, 1000) == 0);
    CHECK(gov::media_retry_wait_ms(78, 78, 0xffffffffu) == 0);
    // Load not known (0): waits for the ceiling
    CHECK(gov::media_retry_wait_ms(5, 0, 0) == 10 && gov::media_retry_wait_ms(5, 0, 250) == 0);
    // The steps from 0 add up to the ceiling exactly
    {
        u32 waited = 0, steps = 0;
        while (const u32 step = gov::media_retry_wait_ms(78, 78, waited))
        {
            waited += step;
            steps++;
        }
        CHECK(waited == gov::media_retry_ceiling_ms && steps == 25);
    }
    static_assert(gov::media_retry_wait_ms(78, 78, 0) == 10 && gov::media_retry_wait_ms(77, 78, 0) == 0);

    // The wait loop on a fake clock: 'load' is the count after 'free_after_ms' (or forever when 0)
    struct fake
    {
        u64 t = 5'000'000; // steady clock, us
        u64 since = 5'000'000;
        u32 sleeps = 0;
        u32 oversleep_us = 0;
        bool clock_runs = true;
        u32 free_after_ms = 0;
        u32 stop_after_ms = 0;

        u32 run(u32 load_at_refusal)
        {
            return gov::media_retry_pace_with(
                since, load_at_refusal, [&]() noexcept { return t; },
                [&]() noexcept -> u32 { return free_after_ms && t - since >= u64{free_after_ms} * 1000 ? load_at_refusal - 1 : load_at_refusal; },
                [&](u32 ms) noexcept
                {
                    sleeps++;
                    if (clock_runs) t += u64{ms} * 1000 + oversleep_us;
                },
                [&]() noexcept { return stop_after_ms && t - since >= u64{stop_after_ms} * 1000; });
        }
    };
    {
        fake f; // nothing frees: the full ceiling, 25 sleeps of 10 ms
        CHECK(f.run(78) == 250 && f.sleeps == 25);
    }
    {
        fake f; // a join after 40 ms
        f.free_after_ms = 40;
        CHECK(f.run(78) == 40 && f.sleeps == 4);
    }
    {
        fake f; // the emulator stops after 20 ms
        f.stop_after_ms = 20;
        CHECK(f.run(78) == 20 && f.sleeps == 2);
    }
    {
        fake f; // stopped already: no sleep
        f.stop_after_ms = 1;
        f.t += 1000;
        CHECK(f.run(78) == 1 && f.sleeps == 0);
    }
    {
        fake f; // each sleep 3 ms longer than asked: still ends right after the ceiling
        f.oversleep_us = 3000;
        const u32 waited = f.run(78);
        CHECK(waited >= 250 && waited < 263 && f.sleeps == 20);
    }
    {
        fake f; // a clock that does not advance: bounded by the sleep count
        f.clock_runs = false;
        CHECK(f.run(78) == 0 && f.sleeps == 50);
    }
    {
        fake f; // 'since' ahead of the clock: counted from 0, still bounded
        f.since = f.t + 100'000;
        const u32 waited = f.run(78);
        CHECK(f.sleeps <= 50 && waited <= 250);
    }

    // Host build: no pacing
    CHECK(gov::media_retry_pace(0, 78, []() noexcept { return false; }) == 0);
}

// 0.40: the reason text of a serialized compile (the PPU and SPU "one thread only" lines)
void test_serialize_reason_040()
{
    namespace gov = utils::ps4::governor;
    using S = mb::slot_mode;
    const mb::s64 M = static_cast<mb::s64>(MiB);
    char text[192];

    const auto reason = [&](const gov::serialize_state& st) -> std::string
    {
        const auto n = gov::format_serialize_reason(text, sizeof(text), st);
        CHECK(n == std::strlen(text));
        return text;
    };

    gov::serialize_state st{};
    st.v038 = false;
    st.threads = 74;
    st.cap = 80;
    CHECK(reason(st) == "thread level 2, 74 of 80");
    st.threads = 71;
    CHECK(reason(st) == "none now");
    st.memory_level = 2;
    st.room_known = true;
    st.room = 300 * M;
    CHECK(reason(st) == "memory level 2, room 300 MiB");
    // 0.39 level 1 serializes PPU/SPU only with cpu_l1
    st.memory_level = 1;
    st.room = 700 * M;
    CHECK(reason(st) == "none now");
    st.cpu_l1 = true;
    st.room = 350 * M;
    CHECK(reason(st) == "memory level 1, room 350 MiB (below 384 MiB of room, low flexible memory or a sustained slope)");
    st.threads = 78;
    st.cool = true;
    CHECK(reason(st) == "memory level 1, room 350 MiB (below 384 MiB of room, low flexible memory or a sustained slope); thread level 2, 78 of 80; cool mode");
    // "Modo frio" in a boot compile stage keeps two compiles: not a cause of its own
    st = {};
    st.v038 = false;
    st.cap = 80;
    st.cool = true;
    CHECK(reason(st) == "cool mode");
    st.cool_stage = true;
    CHECK(reason(st) == "none now");
    // A learned or file cap
    st.cool = false;
    st.threads = 70;
    st.cap = 78;
    CHECK(reason(st) == "thread level 2, 70 of 78");

    // 0.38 rules: any memory level, registry threads at 56/64, "Modo frio"
    st = {};
    st.memory_level = 1;
    st.room_known = true;
    st.room = 700 * M;
    CHECK(reason(st) == "memory level 1, room 700 MiB");
    st.memory_level = 0;
    st.threads = 66;
    CHECK(reason(st) == "thread level 2, 66 live RPCS3 threads (levels at 56/64)");
    st.memory_level = 3;
    st.room = -50 * M;
    st.cool = true;
    CHECK(reason(st) == "memory level 3, room -50 MiB; thread level 2, 66 live RPCS3 threads (levels at 56/64); cool mode");
    st = {};
    st.memory_level = 3;
    CHECK(reason(st) == "memory level 3"); // no cap known
    st.memory_level = 9;
    CHECK(reason(st) == "memory level 3");

    // Truncated to fit, always NUL-terminated
    st = {};
    st.v038 = false;
    st.threads = 74;
    st.cap = 80;
    char small[10];
    CHECK(gov::format_serialize_reason(small, sizeof(small), st) == 9 && std::strcmp(small, "thread le") == 0);
    char one[1] = {'x'};
    CHECK(gov::format_serialize_reason(one, sizeof(one), st) == 0 && one[0] == '\0');
    CHECK(gov::format_serialize_reason(nullptr, 16, st) == 0);

    // A cause is named exactly when the slot mode of the same inputs serializes (one per class or one in total)
    for (const bool v038 : {false, true})
        for (int level = 0; level <= 3; level++)
            for (const bool cpu_l1 : {false, true})
                for (const unsigned threads : {50u, 60u, 66u, 71u, 74u})
                    for (const bool cool : {false, true})
                        for (const bool stage : {false, true})
                        {
                            gov::serialize_state s{};
                            s.v038 = v038;
                            s.memory_level = level;
                            s.room_known = true;
                            s.room = 400 * M;
                            s.cpu_l1 = cpu_l1;
                            s.threads = threads;
                            s.cap = 80;
                            s.cool = cool;
                            s.cool_stage = stage;
                            const int heap = level > 2 ? 2 : level;
                            const S mode = v038 ? mb::slot_mode_v038(heap, gov::classify_threads(threads) >= 2, cool)
                                                : mb::slot_mode_v039(heap, cpu_l1, gov::classify_thread_load(threads, 80) >= 2, cool, cool && stage);
                            const bool serialized = mode == S::one_per_class || mode == S::one_in_total;
                            CHECK((reason(s) != "none now") == serialized);
                        }

    // Host build: no reason text
    CHECK(gov::serialize_reason().empty());
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

// 0.40: optional threads after a console refusal below the cap (the refusal load is the cap for them, under either
// rule set), the group count the SPU LLVM workers ask for, and the rate of the codec failure lines.
void test_optional_cap_040()
{
    namespace gov = utils::ps4::governor;

    // No refusal yet: the cap in force
    CHECK(gov::optional_cap(80, 0) == 80);
    // DeS: refused at 78 with the default cap 80 -> optional threads count against 78 (one more may start at 69)
    CHECK(gov::optional_cap(80, 78) == 78);
    CHECK(gov::optional_allowed(69, 1, gov::optional_cap(80, 78)) && !gov::optional_allowed(70, 1, gov::optional_cap(80, 78)));
    // Before: against 80 one could still start at 70 and 71 (only 6 slots below 78 kept for the game)
    CHECK(gov::optional_allowed(70, 1, 80) && gov::optional_allowed(71, 1, 80) && !gov::optional_allowed(72, 1, 80));
    // A refusal at or above the cap is not a lower cap; below thread_cap_min it is not a cap at all
    CHECK(gov::optional_cap(80, 80) == 80);
    CHECK(gov::optional_cap(80, 85) == 80);
    CHECK(gov::optional_cap(80, 64) == 64);
    CHECK(gov::optional_cap(80, 63) == 80);
    CHECK(gov::optional_cap(80, 20) == 80);
    // A learned (0.39 rules) or probed cap below the refusal keeps the smaller one
    CHECK(gov::optional_cap(76, 78) == 76);
    CHECK(gov::optional_cap(100, 90) == 90);
    // An out-of-range cap is clamped like the levels clamp it
    CHECK(gov::optional_cap(10, 63) == 10 && gov::optional_cap(10, 64) == 10);
    static_assert(gov::optional_cap(80, 78) == 78);

    // Group count: the largest n <= wanted with optional_decision() true, for every load
    for (u32 cap : {64u, 78u, 80u, 100u})
    {
        for (u32 t = 0; t <= cap + 4; t++)
        {
            for (u32 wanted = 0; wanted <= 4; wanted++)
            {
                const u32 n = gov::optional_allowed_count(true, t, wanted, cap);
                bool ok = n <= wanted && (n == 0 || gov::optional_decision(true, t, n, cap));
                ok = ok && (n == wanted || !gov::optional_decision(true, t, n + 1, cap));
                CHECK(ok);
                CHECK(gov::optional_allowed_count(false, t, wanted, cap) == wanted);
            }
        }
    }
    // GT6: two SPUW workers wanted at 70 of 80 -> both; at 71 -> one; at 72 and above -> none (the SPU LLVM thread
    // compiles inline then)
    CHECK(gov::optional_allowed_count(true, 70, 2, 80) == 2);
    CHECK(gov::optional_allowed_count(true, 71, 2, 80) == 1);
    CHECK(gov::optional_allowed_count(true, 72, 2, 80) == 0);
    CHECK(gov::optional_allowed_count(true, 200, 2, 80) == 0);
    // With the refusal cap of 78: one fewer at each load
    CHECK(gov::optional_allowed_count(true, 69, 2, gov::optional_cap(80, 78)) == 1);
    // Host stubs: every optional thread may start
    CHECK(gov::optional_threads_allowed("SPU LLVM worker", 2) == 2 && gov::optional_thread_cap() == gov::thread_cap_default);

    // Codec failure lines: the first 8, then every 100th
    std::atomic<u32> count{0};
    u32 lines = 0, last = 0;
    for (int i = 0; i < 1000; i++)
    {
        u32 number = 0;
        if (gov::media_log_due(count, number))
        {
            lines++;
            last = number;
        }
    }
    CHECK(lines == 8 + 10 && last == 1000 && count.load() == 1000);
    CHECK(gov::media_log_due(1u) && gov::media_log_due(8u) && !gov::media_log_due(9u) && !gov::media_log_due(99u) && gov::media_log_due(100u) &&
        !gov::media_log_due(101u) && gov::media_log_due(200u));
}

// 0.40 opt-in boot compile stage rule (row ppu_boot_workers): up to 4 PPU compile threads, the normal rules back at
// any level change.
void test_boot_workers_040()
{
    namespace gov = utils::ps4::governor;
    const mb::s64 M = static_cast<mb::s64>(MiB);

    gov::boot_workers_state s{};
    s.row_on = true;
    s.boot_stage = true;
    s.llvm_threads = 2;
    s.modules = 211; // The Darkness II first boot
    s.threads = 25;
    s.room_known = true;
    s.room = 1232 * M;
    CHECK(gov::boot_workers_block(s) == nullptr);
    CHECK(gov::boot_workers_optional(s, 2) == 2); // 2 LLVM threads + 2 = 4 compile threads
    CHECK(gov::boot_workers_optional(s, 1) == 1); // never more than the optional permits
    CHECK(gov::boot_workers_total == 4 && gov::boot_workers_base == 2);

    // Each condition alone keeps the normal workers (0 extra under this rule)
    const auto blocked = [&](auto change)
    {
        gov::boot_workers_state t = s;
        change(t);
        return gov::boot_workers_block(t) != nullptr && gov::boot_workers_optional(t, 2) == 0;
    };
    CHECK(blocked([](gov::boot_workers_state& t) { t.row_on = false; }));                      // the row is off by default
    CHECK(blocked([](gov::boot_workers_state& t) { t.boot_stage = false; }));                  // a PRX loaded while the game runs
    CHECK(blocked([](gov::boot_workers_state& t) { t.llvm_threads = 1; }));                    // the player's or the adapt's 1 thread
    CHECK(blocked([](gov::boot_workers_state& t) { t.modules = 2; }));
    CHECK(blocked([](gov::boot_workers_state& t) { t.memory_level = 1; }));
    CHECK(blocked([](gov::boot_workers_state& t) { t.thread_level = 1; }));
    CHECK(blocked([](gov::boot_workers_state& t) { t.threads = 49; }));
    CHECK(blocked([&](gov::boot_workers_state& t) { t.room = 1023 * M; }));
    CHECK(blocked([](gov::boot_workers_state& t) { t.room_known = false; }));
    CHECK(blocked([](gov::boot_workers_state& t) { t.adapt_key = true; }));
    // The edges that still pass: 48 threads, exactly 1 GiB, 3 modules (one optional worker has work)
    {
        gov::boot_workers_state t = s;
        t.threads = 48;
        t.room = 1024 * M;
        CHECK(gov::boot_workers_block(t) == nullptr && gov::boot_workers_optional(t, 2) == 2);
        t.modules = 3;
        CHECK(gov::boot_workers_optional(t, 2) == 1);
    }
    CHECK(std::strcmp(gov::boot_workers_block(gov::boot_workers_state{}), "ppu_boot_workers off") == 0);

    // Admission in that stage: four mid-size modules (140 MiB each) fit at level 0 with 1.2 GiB of room; the normal
    // budget admits two
    const mb::u64 mod = 140 * MiB;
    CHECK(gov::boot_compile_budget == 768 * MiB);
    CHECK(gov::boot_compile_admit(1232 * M, 0, mod, 0));
    CHECK(gov::boot_compile_admit(1232 * M, mod, mod, 0));
    CHECK(gov::boot_compile_admit(1232 * M, 2 * mod, mod, 0));
    CHECK(gov::boot_compile_admit(1232 * M, 3 * mod, mod, 0) == (1232 * M - static_cast<mb::s64>(4 * mod) >= static_cast<mb::s64>(mb::l2_room)));
    CHECK(mb::compile_admit(1232 * M, mod, mod, 0) && !mb::compile_admit(1232 * M, 2 * mod, mod, 0));
    // The in-flight budget still bounds it, and the level 2 room stays free
    CHECK(!gov::boot_compile_admit(4000 * M, 700 * MiB, 100 * MiB, 0));
    CHECK(!gov::boot_compile_admit(900 * M, 400 * MiB, 140 * MiB, 0)); // 900 - 540 < 384
    // Any memory level above 0: exactly the normal rule
    for (int level = 1; level <= 3; level++)
    {
        for (mb::u64 in : {0ull, 100ull, 200ull, 300ull})
        {
            for (mb::s64 room : {500ll, 900ll, 1500ll})
            {
                CHECK(gov::boot_compile_admit(room * M, in * MiB, 100 * MiB, level) == mb::compile_admit(room * M, in * MiB, 100 * MiB, level));
            }
        }
    }
    // The first compile always passes
    CHECK(gov::boot_compile_admit(10 * M, 0, 384 * MiB, 0));
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
    test_rules_040();
    test_media_pacing_040();
    test_serialize_reason_040();
    test_optional_cap_040();
    test_boot_workers_040();
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
