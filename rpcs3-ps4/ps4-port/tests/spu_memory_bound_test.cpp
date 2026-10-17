// Host test of the 0.38 SPU compile memory rules (rpcs3/util/ps4_memory_governor.hpp) and of the bound they give the
// boot SPU cache build, with a model of musl's small heap (held = high-water of the live small blocks, never returned;
// freed blocks are reused by later small allocations).
//
// Calibration (console log of the 0.37 test, FIFA 14 BLES01876, first boot): the boot build kept one LLVM compiler per
// worker for the whole build; 4 workers built ~74.5k programs (55928 by the three that returned) while the heap went
// from 971 to 1920 MiB: ~13 KiB kept per program, plus the program's spu_runtime item. The model reproduces that curve
// and shows the 0.38 one (compilers recycled after 256 programs or 8 MiB of code, IR and object bytes freed after
// emission), whose growth does not depend on the program count. It also checks the trampoline memory of the build:
// one rebuild per program (0.37) against one rebuild per bucket after the build (0.38), and the JIT private budget at
// which new SPU LLVM code stops (governor::jit_room_short).
//
// Build and run from any scratch directory (llvm-mingw):
//   clang++ -std=c++20 -static -Wall -Wextra -I rpcs3 ps4-port/tests/spu_memory_bound_test.cpp -o spu_memory_bound_test.exe
//   spu_memory_bound_test.exe
#include "util/ps4_memory_governor.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace gov = utils::ps4::governor;

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

constexpr u64 KiB = 1024;
constexpr u64 MiB = 1024 * KiB;

gov::heap_sample heap(u64 cap_mib, u64 live_mib, bool critical = false)
{
    gov::heap_sample s{};
    s.cap = cap_mib * MiB;
    s.live = live_mib * MiB;
    s.critical = critical;
    return s;
}

void test_thresholds()
{
    // The governor acts in this order while the room shrinks: recycle harder, serialize, stop / brake, then the cap
    CHECK(gov::heap_room_stop < gov::heap_room_floor);
    CHECK(gov::heap_room_floor < gov::spu_recycle_room);

    // Recycle budget
    CHECK(gov::spu_recycle_limit_for(heap(2048, 1000), gov::spu_recycle_programs) == 256);
    CHECK(gov::spu_recycle_limit_for(heap(2048, 1600), gov::spu_recycle_programs) == 16); // 448 MiB of room
    CHECK(gov::spu_recycle_limit_for(heap(2048, 1000, true), gov::spu_recycle_programs) == 16);
    CHECK(gov::spu_recycle_limit_for(heap(2048, 1600), 0) == 0);

    CHECK(!gov::spu_recycle_due(255, 0, 256));
    CHECK(gov::spu_recycle_due(256, 0, 256));
    CHECK(!gov::spu_recycle_due(3, gov::spu_recycle_bytes - 1, 256));
    CHECK(gov::spu_recycle_due(3, gov::spu_recycle_bytes, 256));
    CHECK(!gov::spu_recycle_due(1'000'000, 1ull << 40, 0)); // recycling off (spu-recycle-programs.txt = 0)
    CHECK(gov::spu_recycle_due(16, 0, gov::spu_recycle_limit_for(heap(2048, 1700), 256)));

    // Stop before the cap
    CHECK(!gov::compile_room_short(heap(2048, 2048 - 193)));
    CHECK(gov::compile_room_short(heap(2048, 2048 - 191)));
    CHECK(gov::compile_room_short(heap(2048, 2100)));               // past the cap (overdraft)
    CHECK(gov::compile_room_short(heap(2048, 2048 - 300, true)));    // critical: the floor applies
    CHECK(!gov::compile_room_short(heap(2048, 2048 - 400, true)));
    CHECK(!gov::compile_room_short(heap(0, 5000)));                  // cap unknown: never from the room alone
    // Brake hysteresis (margin 64 MiB while engaged)
    CHECK(gov::compile_room_short(heap(2048, 2048 - 200), 64 * MiB));
    CHECK(!gov::compile_room_short(heap(2048, 2048 - 260), 64 * MiB));

    // Serialization (level 1) starts above the stop
    CHECK(gov::classify_heap(heap(2048, 2048 - 383)) == 1);
    CHECK(gov::classify_heap(heap(2048, 2048 - 385)) == 0);
}

// JIT private memory budget (ps4_virtual_memory private commits): new SPU LLVM code stops 32 MiB before 640 MiB
void test_jit_budget()
{
    CHECK(gov::jit_room_stop < gov::jit_budget);
    CHECK(!gov::jit_room_short(gov::jit_budget - gov::jit_room_stop - 1));
    CHECK(gov::jit_room_short(gov::jit_budget - gov::jit_room_stop));
    CHECK(gov::jit_room_short(gov::jit_budget + 100 * MiB)); // past the budget (PPU link arenas are not braked here)

    // Brake hysteresis (64 MiB while engaged): JIT memory is only returned at the stop, so an engaged brake holds
    CHECK(gov::jit_room_short(gov::jit_budget - gov::jit_room_stop - 64 * MiB, 64 * MiB));
    CHECK(!gov::jit_room_short(gov::jit_budget - gov::jit_room_stop - 64 * MiB - 1, 64 * MiB));

    // The 0.37 sessions never reach it: start 113 MiB, FIFA peak 388 MiB (with the per-program trampolines), 398 MiB
    // the largest private figure of the 0.37 logs, also with the engaged margin
    for (const u64 mib : {113ull, 388ull, 398ull})
    {
        CHECK(!gov::jit_room_short(mib * MiB));
        CHECK(!gov::jit_room_short(mib * MiB, 64 * MiB));
    }

    // What stays for the game after the stop: spu_fast stubs of a 4096-instruction block are 22 + 1 + 9 + 4096 * 32 +
    // 36 + 47 bytes (spu_fast::compile), a full bucket trampoline 22 * n + 14; 32 MiB holds ~250 of the largest stubs
    CHECK(gov::jit_room_stop / (22 + 1 + 9 + 4096 * 32 + 36 + 47) >= 250);
}

// One boot build: 'workers' compile 'programs' programs in turn. A compiler keeps 'kept' bytes per program until it is
// dropped; 'base' is a fresh compiler, 'transient' the working set of one compile (freed when it ends), 'item' the
// spu_runtime entry every program keeps. Returns the small-heap high-water growth over the build; 'curve' gets the
// live growth after every tenth of the build.
struct build_model
{
    u32 workers = 4;
    u64 programs = 0;
    u32 recycle = 0;         // programs per compiler, 0 = one compiler per worker for the whole build (0.37)
    u64 recycle_bytes = 0;   // object bytes per compiler, 0 = no byte budget
    u64 kept = 13 * KiB;
    u64 object = 4 * KiB;
    u64 base = 6 * MiB;
    u64 transient = 16 * MiB;
    u64 item = 256;

    u64 run(std::vector<u64>* curve = nullptr) const
    {
        std::vector<u64> kept_now(workers, 0), obj_now(workers, 0), count(workers, 0);
        u64 live = workers * base;
        u64 high = live;
        const u64 start = live;

        for (u64 i = 0; i < programs; i++)
        {
            const u32 w = static_cast<u32>(i % workers);

            // Worst case: every worker sits at its compile peak at the same time
            high = std::max(high, live + workers * transient);

            kept_now[w] += kept;
            obj_now[w] += object;
            count[w]++;
            live += kept + item;
            high = std::max(high, live);

            if ((recycle && count[w] >= recycle) || (recycle_bytes && obj_now[w] >= recycle_bytes))
            {
                // Dropped and rebuilt at once: its blocks go back to musl's free lists and are reused
                live -= kept_now[w];
                kept_now[w] = 0;
                obj_now[w] = 0;
                count[w] = 0;
            }

            if (curve && programs >= 10 && (i + 1) % (programs / 10) == 0)
            {
                curve->push_back(high - start);
            }
        }

        return high - start;
    }
};

void test_build_bound()
{
    // 0.37 calibration: ~74.5k programs, 971 -> 1920 MiB (949 MiB)
    build_model old37;
    old37.programs = 74'500;
    const u64 growth37 = old37.run() - old37.workers * old37.transient;
    std::printf("0.37 model, FIFA 14 boot build (74.5k programs): small heap +%llu MiB (console: +949 MiB, fatal at the cap)\n",
        static_cast<unsigned long long>(growth37 / MiB));
    CHECK(growth37 > 900 * MiB && growth37 < 1000 * MiB);

    // 0.38: recycled after 256 programs or 8 MiB of code. 'kept' stays at the 0.37 value on purpose: the bound must hold
    // even if freeing the IR and the object bytes after emission saved nothing.
    build_model new38 = old37;
    new38.recycle = gov::spu_recycle_programs;
    new38.recycle_bytes = gov::spu_recycle_bytes;

    const u64 compilers_bound = new38.workers * (new38.base + new38.recycle * new38.kept + new38.transient);

    for (const u64 programs : {10'000ull, 74'500ull, 300'000ull, 1'000'000ull})
    {
        new38.programs = programs;
        const u64 growth = new38.run();
        const u64 items = programs * new38.item;
        std::printf("0.38 model, %7llu programs: small heap +%llu MiB (compilers <= %llu MiB, spu_runtime items %llu MiB)\n",
            static_cast<unsigned long long>(programs), static_cast<unsigned long long>(growth / MiB),
            static_cast<unsigned long long>(compilers_bound / MiB), static_cast<unsigned long long>(items / MiB));
        CHECK(growth <= compilers_bound + items);
    }

    // Under pressure (room < 512 MiB) the limit is 16 programs
    new38.programs = 74'500;
    new38.recycle = gov::spu_recycle_programs_short;
    CHECK(new38.run() <= new38.workers * (new38.base + 16 * new38.kept + new38.transient) + new38.programs * new38.item);

    // Huge programs: the byte budget drops the compiler after 4 programs of 2 MiB of code
    build_model huge = old37;
    huge.programs = 1000;
    huge.recycle = gov::spu_recycle_programs;
    huge.recycle_bytes = gov::spu_recycle_bytes;
    huge.object = 2 * MiB;
    huge.kept = 3 * MiB;
    CHECK(huge.run() <= huge.workers * (huge.base + 4 * huge.kept + huge.transient) + huge.programs * huge.item);

    // Expected curves of the FIFA build (live growth after each tenth, compile peaks included)
    std::vector<u64> c37, c38;
    old37.run(&c37);
    new38 = old37;
    new38.recycle = gov::spu_recycle_programs;
    new38.recycle_bytes = gov::spu_recycle_bytes;
    new38.run(&c38);
    std::printf("expected curve, small heap growth over 971 MiB at each tenth of the build (MiB):\n  0.37:");
    for (const u64 v : c37) std::printf(" %llu", static_cast<unsigned long long>(v / MiB));
    std::printf("\n  0.38:");
    for (const u64 v : c38) std::printf(" %llu", static_cast<unsigned long long>(v / MiB));
    std::printf("\n");
    CHECK(c38.size() == 10 && c37.size() == 10);
    CHECK(c38.back() - c38.front() < 32 * MiB); // only the spu_runtime items grow after the first tenth
}

// The build stops taking programs before the cap: whatever the start, live never passes cap - heap_room_stop by more
// than one compile's growth per worker.
void test_stop_before_cap()
{
    constexpr u64 cap = 2048 * MiB;
    constexpr u64 per_compile = 16 * MiB;

    for (const u64 start_mib : {900ull, 1500ull, 1800ull, 1840ull, 1900ull})
    {
        u64 live = start_mib * MiB;
        u64 built = 0;

        for (u64 i = 0; i < 100'000; i++)
        {
            gov::heap_sample s{};
            s.cap = cap;
            s.live = live;

            if (gov::compile_room_short(s))
            {
                break;
            }

            // The pessimistic case: each compile leaves 64 KiB behind (no recycling at all)
            live += 64 * KiB;
            built++;
        }

        CHECK(live + 4 * per_compile < cap);
        std::printf("stop model: start %llu MiB -> stops at %llu MiB after %llu programs (room %llu MiB)\n",
            static_cast<unsigned long long>(start_mib), static_cast<unsigned long long>(live / MiB),
            static_cast<unsigned long long>(built), static_cast<unsigned long long>((cap - live) / MiB));
    }
}

// Ubertrampolines: x86 bytes of one bucket of n programs are n * 22 + 14 (rebuild_ubertrampoline), never freed. The
// real spread of first instructions is not in the logs (the 0.37 build used 275 MiB of JIT code and data for code and
// trampolines together), so a few Zipf-like spreads of 74.5k programs over 16384 buckets are shown.
void test_trampolines()
{
    constexpr u32 buckets = 16384;
    constexpr u64 programs = 74'500;

    for (const double skew : {0.6, 0.8, 1.0})
    {
        std::vector<u64> size(buckets);
        double norm = 0;

        for (u32 b = 0; b < buckets; b++)
        {
            norm += 1.0 / std::pow(b + 1, skew);
        }

        u64 placed = 0;

        for (u32 b = 0; b < buckets; b++)
        {
            size[b] = static_cast<u64>(programs * (1.0 / std::pow(b + 1, skew)) / norm);
            placed += size[b];
        }

        // Rounding remainder: one program each to the first buckets
        for (u32 b = 0; placed < programs; b = (b + 1) % buckets, placed++)
        {
            size[b]++;
        }

        u64 per_program = 0, deferred = 0, used = 0;

        for (const u64 n : size)
        {
            if (!n) continue;
            used++;
            // 0.37: one rebuild per compiled program of the bucket (sizes 1..n; a single program needs none)
            for (u64 k = 2; k <= n; k++) per_program += k * 22 + 14;
            // 0.38: one rebuild after the build
            deferred += n > 1 ? n * 22 + 14 : 0;
        }

        std::printf("trampolines, skew %.1f, 74.5k programs in %llu buckets (largest %llu): per-program rebuilds %llu MiB, one rebuild per bucket %llu KiB\n",
            skew, static_cast<unsigned long long>(used), static_cast<unsigned long long>(size[0]),
            static_cast<unsigned long long>(per_program / MiB), static_cast<unsigned long long>(deferred / KiB));
        CHECK(deferred <= programs * 22 + used * 14);
        CHECK(per_program >= deferred);
    }
}
}

int main()
{
    test_thresholds();
    test_jit_budget();
    test_build_bound();
    test_stop_before_cap();
    test_trampolines();

    if (g_failures)
    {
        std::printf("spu_memory_bound_test: %d failure(s)\n", g_failures);
        return 1;
    }

    std::printf("spu_memory_bound_test: all passed\n");
    return 0;
}
