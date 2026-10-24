#pragma once

// 0.31: compile and thread budget governor (header-only). PPU/SPU LLVM compile workers hold a compile_slot while
// they compile; under heap pressure, or when the process is close to the console's thread ceiling, only one
// compile runs at a time, so a heavy first boot gets slower instead of running out of memory or threads. Code that
// creates optional threads (RPCN, NP transaction workers, the XMB loader) checks thread_pressure() first and takes
// its synchronous or inline path at level 2.
//
// Levels: 0 = fine, 1 = low (serialize compiles), 2 = critical (also refuse optional threads).
// Every level change is written once to startup.log ("memory governor: ..."), at most 32 heap lines (the test cap
// line included; the change to heap level 2 is written even past that) and 32 thread lines per process.
//
// heap_pressure() reads the orbis-compat direct-memory heap (orbis::mmapDirectStats: cap, live bytes, critical
// flag, bytes that fell back to flexible memory) and the free flexible memory (thread stacks live there). It
// samples at most every 50 ms and reuses the last level in between, so polling it is cheap. The test cap file
// (/data/rpcs3/test-governor-cap-mib.txt, a decimal MiB value) lowers the cap the governor sees (not the real
// heap): a value below the heap's live size forces serialized compiles for the console test. The critical flag
// and the fallback byte count only ever grow, so once the heap had to fall back to flexible memory, compiles stay
// serialized for the rest of the process.
//
// 0.33: SPU LLVM compile budget. A runtime SPU LLVM compiler keeps the module and the loaded object of every program
// it built until it is destroyed (MCJIT), all in this heap, so SPUCommonRecompiler.cpp drops it after
// spu_recycle_limit_for() programs and builds a fresh one for the next program. The boot SPU cache build may start up
// to spu_cache_workers_max workers instead of the LLVM thread setting while nothing is under pressure
// (spu_cache_extra_workers_for()). Both decisions are pure functions of a heap sample, so they can be tested off the
// console.
// 0.38: the boot SPU cache build recycles its compilers with the same budget (it kept one compiler per worker for the
// whole build: FIFA 14, ~74k programs, grew the small heap by ~950 MiB and hit the cap), SPU modules are freed right
// after code emission (jit_compiler::ps4_add_and_free), a compiler is also dropped after spu_recycle_bytes of object
// code, and compile_room_short() stops the boot build / brakes runtime compiles before the cap instead of at it.
//
// 0.34: heap level 2 (the heap reached its cap or fell back to flexible memory) is reported to the player once per
// game boot: one toast to save and restart, restart_advised() turns true for the frontend, and when the level was
// first reached while that game was running, its adapt file gets heap=cap and llvm_threads=1 (lighter next boots).
// Level 2 is sticky for the process, so heap_pressure() checks this on every sample at level 2, and poll_heap()
// (main loop, once per second) takes that sample as soon as the heap counters say critical, even when nothing
// compiles.
// Heap reserve: at the first finished compile stage with enough room, arm_reserve() takes reserve_blocks blocks of
// one 16 MiB musl mapping each and installs a std::new_handler. Every call of that handler (an operator new that
// failed) frees one block and the allocation is retried; heap_pressure() also frees one while level 2 holds and the
// bytes that fell back to flexible memory keep growing (thread stacks live there). The handler uninstalls itself when
// the reserve is empty, so operator new then fails as before. Releases log through ps4_fatal_note (no allocation).
// "Modo frio" (util/ps4_cool_mode.hpp) also serializes compiles and keeps the boot SPU cache build at upstream's
// worker count. With the 0.39 rules, during a boot compile stage it allows two compiles in total and no optional
// workers (slot_mode::two_in_total; /data/rpcs3/cool-mode-serial-boot.flag: one, as in 0.38).
//
// 0.38 (governor v2): the budget authority and non-sticky levels. Once a second (budget_tick, from heap_pressure() and
// the main loop) the owners of the direct-memory pool are measured: the orbis-compat ledger (free total, largest run,
// what the system held at boot), the heap's carve-outs, the guest's shared memory and the private JIT commits
// (utils::ps4::memory), RPCS3's own thread stacks (Thread.cpp) and the GPU window meter (util/ps4_vram_policy.hpp).
// util/ps4_mem_budget.hpp turns them into the reservation of the other owners and the heap's cap, which orbis-compat
// enforces on every new carve-out (mmapDirectSetReserve, mmapDirectSetCap); while the ledger has no kernel walk the
// governor also feeds it the bytes it cannot see (directLedgerSetExternal). heap_sample::cap is therefore that cap.
// The memory level (memory_level(), 0..3) follows the room under it with hysteresis and falls again when memory comes
// back; heap_pressure() keeps its 0..2 range for its callers (3 reads as 2). Level 1 serializes compiles (as before);
// level 2 also makes every compile wait up to 2 s for the level to fall before it starts, runs the level-2 shedders
// (registered by the owners of caches: register_shedder) and trims empty heap carve-outs, one toast per boot; level 3
// (room below 128 MiB or an allocation the heap could not serve) runs every shedder, toasts "save now" and arms the
// clean-process rule of the frontend. orbis-compat calls the pressure hook installed here before it falls back to
// flexible memory: it gives the heap reserve (one 48 MiB block, re-armed every time the XMB opens) to the failing
// request. /data/rpcs3/no-mem-budget.flag keeps the build cap and no reservation (0.37 caps; the levels stay).
//
// 0.39 rules: memory level 1 from max(512 MiB, cap / 4) of room or a slope sustained over two 30 s windows outside
// boot compile stages (util/ps4_mem_budget.hpp); at level 1 PPU/SPU compiles serialize only below 384 MiB of room or
// on that slope. compile_slot has classes (slot_kind: cpu, rsx, interp): one per class while level 1 serializes, one
// in total from level 2 on, and only cpu slots wait for level 2 to fall. Thread levels from the count the console's
// cap applies to, with a cap learned from the first refused create.
// 0.40: they change compile and memory concurrency for every game, so the 0.38 rules (768 MiB, one-window slope, one
// shared slot, thread levels from the registry at 56/64) are the default; the 0.39 rules run with
// /data/rpcs3/governor-v039.flag or the governor_v039 line of features.txt (util/ps4_safe_mode.hpp; governor-v038.flag
// and the safe mode keep the 0.38 rules). The optional-thread rule (optional_thread_allowed) is independent of both
// (after a console refusal below the cap it counts against that load, optional_thread_cap), and so is the single paced
// retry of a refused HLE media create (is_hle_media_thread, media_retry_pace).
//
// Outside the PS4 build everything reports level 0 and compile_slot does nothing.

#include "util/types.hpp"
#include "util/ps4_mem_budget.hpp"

#include <atomic>
#include <cstdio>
#include <string>
#include <string_view>

#ifdef RPCS3_PS4
#include "Utilities/Thread.h" // ps4_thread_registry::get_counters()
#include "util/ps4_adapt.hpp"
#include "util/ps4_cool_mode.hpp"
#include "util/ps4_i18n.hpp"           // toasts (tr: constant tables, no allocation)
#include "util/ps4_safe_mode.hpp"      // feature switches (optional-thread rule)
#include "util/ps4_virtual_memory.hpp" // private (JIT, own stacks) and shared (guest) direct commits
#include "util/ps4_vram_policy.hpp"    // GPU window meter

#include <orbis_mmap.h> // orbis-compat heap counters

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <thread>
#include <fcntl.h>
#include <unistd.h>

// Frontend objects (ps4_fatal.cpp, ps4_launcher.cpp), always linked.
unsigned long long ps4_flexible_memory_free() noexcept;
void ps4_startup_checkpoint(const char* stage);
// Allocation-free (open/write/fsync and a system notification): usable when malloc has just failed.
void ps4_fatal_note(const char* prefix, const char* detail) noexcept;
void ps4_notify(const char* message) noexcept;
// Utilities/Thread.cpp: direct memory committed for RPCS3-owned thread stacks.
unsigned long long ps4_thread_stack_direct_bytes() noexcept;
#endif

namespace utils::ps4::governor
{
inline constexpr u64 heap_room_floor = 384ull << 20; // free heap budget below this: level 1
inline constexpr u64 flexible_floor = 96ull << 20;   // free flexible memory below this: level 1
inline constexpr u32 thread_warn = 56;               // live RPCS3 threads: level 1
inline constexpr u32 thread_high = 64;               // level 2 (the console refused threads at about 80 stacks)
inline constexpr char test_cap_file[] = "/data/rpcs3/test-governor-cap-mib.txt";
inline constexpr char v038_flag[] = "/data/rpcs3/governor-v038.flag";
// "Modo frio" serializes boot compile stages too (one compile in the whole process, as in 0.38).
inline constexpr char cool_serial_boot_flag[] = "/data/rpcs3/cool-mode-serial-boot.flag";

// 0.39: compile slot classes (contract with the RSX pipeline compiler).
using slot_kind = ::utils::ps4::mem_budget::slot_kind;

struct heap_sample
{
    u64 cap = 0;           // heap budget in bytes, 0 = unknown (0.38: the budget authority's cap)
    u64 live = 0;          // heap bytes in use (only meaningful with a cap)
    u64 flexible_free = 0; // free flexible memory, 0 = unknown
    // The heap hit its cap or had to fall back to flexible memory. 0.38: no longer sticky: the flag is cleared when
    // the memory level falls below 2, and only fallback bytes still mapped count.
    bool critical = false;
    u64 fallback_bytes = 0; // bytes that fell back to flexible memory so far (never shrinks)
    u64 fallback_live = 0;  // 0.38: of which still mapped
    u64 refusals = 0;       // 0.38: refused carve-outs plus requests no carve-out could serve, cumulative
};

constexpr int classify_heap(const heap_sample& s) noexcept
{
    if (s.critical) return 2;
    const u64 room = s.cap > s.live ? s.cap - s.live : 0;
    if (s.cap && room < heap_room_floor) return 1;
    if (s.flexible_free && s.flexible_free < flexible_floor) return 1;
    return 0;
}

constexpr int classify_threads(u32 live) noexcept
{
    return live >= thread_high ? 2 : live >= thread_warn ? 1 : 0;
}

// 0.39: thread levels on the console's per-process thread cap and the count it applies to (Thread.h
// ps4_thread_load: threads that have not exited plus exited threads not joined yet). Level 1 at cap - 16 (no optional
// workers), level 2 at cap - 8 (optional threads refused or run inline, compiles one per class). The cap is 80 unless
// /data/rpcs3/thread-cap.txt has a line for this firmware ("13.020.001 80"; written by set_thread_cap); the first
// refused create of a process may lower it for that process only. governor-v038.flag: the 0.38 registry count at 56/64.
inline constexpr u32 thread_cap_default = 80;
inline constexpr u32 thread_cap_min = 64;
inline constexpr u32 thread_cap_max = 2048;
inline constexpr u32 thread_l1_margin = 16;
inline constexpr u32 thread_l2_margin = 8;
inline constexpr char thread_cap_file[] = "/data/rpcs3/thread-cap.txt";

constexpr u32 clamp_thread_cap(u32 cap) noexcept
{
    return cap < thread_cap_min ? thread_cap_min : cap > thread_cap_max ? thread_cap_max : cap;
}

constexpr int classify_thread_load(u32 total, u32 cap) noexcept
{
    const u32 c = clamp_thread_cap(cap);
    return total + thread_l2_margin >= c ? 2 : total + thread_l1_margin >= c ? 1 : 0;
}

// 0.40: an optional emulator thread group of 'n' threads may start while the count the console's cap applies to
// ('total': orbis live + exited not joined), with those threads added, leaves the last thread_l2_margin slots free:
// those belong to the game, whose creates fail with CELL_EAGAIN when the cap is reached.
constexpr bool optional_allowed(u32 total, u32 n, u32 cap) noexcept
{
    return total + n + thread_l2_margin <= clamp_thread_cap(cap);
}

// optional_thread_allowed() with its inputs: 'rule_on' is the optional_thread_rule row (optional-threads-free.flag
// turns it off: every optional thread may start, as in 0.39).
constexpr bool optional_decision(bool rule_on, u32 total, u32 n, u32 cap) noexcept
{
    return !rule_on || optional_allowed(total, n, cap);
}

// 0.40: how many of 'wanted' optional threads may start now: the largest n <= wanted with optional_decision() true
// (all of them with the rule off).
constexpr u32 optional_allowed_count(bool rule_on, u32 total, u32 wanted, u32 cap) noexcept
{
    if (!rule_on) return wanted;
    const u32 c = clamp_thread_cap(cap);
    const u32 used = total + thread_l2_margin;
    const u32 room = c > used ? c - used : 0;
    return wanted < room ? wanted : room;
}

// 0.40: the cap the optional-thread rule counts against. Once the console refused a create while fewer threads counted
// than the cap in force ('refused_at': the count right after the first refused create of the process, 0 = none), that
// count, for the rest of the process and under either governor rule set: with a cap of 80 and a refusal at 78 the last
// 8 slots below 78 stay with the game, not 6. The 0.38 thread levels keep their registry count; the 0.39 rules learn
// the same cap (note_thread_refusal). A count below thread_cap_min is not taken as a cap (such a refusal had another
// cause, such as no memory for a stack).
constexpr u32 optional_cap(u32 cap, u32 refused_at) noexcept
{
    return refused_at >= thread_cap_min && refused_at < clamp_thread_cap(cap) ? refused_at : cap;
}

// 0.40: the threads the emulator itself creates through sys_ppu_thread_create when a game opens an HLE codec
// (cellVdec, cellAtracXdec, cellAdec, cellDmuxPamf). With the guest create retry off, a refused one of these fails at
// once and libsail reopens the decoder within a fraction of a millisecond, so the refused create waits for a slot
// once (media_retry_pace). Exact names only: the vdec reaper and every game-created thread keep the immediate
// CELL_EAGAIN.
constexpr bool is_hle_media_thread(std::string_view name) noexcept
{
    return name == "HLE Video Decoder" || name == "HLE ATRAC3plus decoder" || name == "HLE LPCM decoder" || name == "HLE PAMF demuxer";
}

inline constexpr u32 media_retry_ceiling_ms = 250; // 0.39 spent ~230 ms per refused vdec open in its retries
inline constexpr u32 media_retry_step_ms = 10;

// Next sleep in ms before an HLE media create is tried once more, 0 = create now: a slot came back (fewer threads than
// right after the refusal, 'load_at_refusal'; the cap in force can sit above the count the console refused at, so it
// is not the condition), or 'waited_ms' reached the ceiling. A load of 0 (not known) waits for the ceiling.
constexpr u32 media_retry_wait_ms(u32 total_now, u32 load_at_refusal, u32 waited_ms) noexcept
{
    if (total_now < load_at_refusal || waited_ms >= media_retry_ceiling_ms) return 0;
    const u32 left = media_retry_ceiling_ms - waited_ms;
    return left < media_retry_step_ms ? left : media_retry_step_ms;
}

// The wait of media_retry_pace with its clock, thread count and sleep passed in (host tests use fakes): sleeps in
// media_retry_wait_ms steps from 'since_us' until a slot comes back, the ceiling passes or 'stopped()' is true, and
// returns the ms waited since 'since_us'. Bounded by a sleep count as well, so a clock that does not advance cannot
// keep it waiting.
template <typename Now, typename Total, typename Sleep, typename Stop>
inline u32 media_retry_pace_with(u64 since_us, u32 load_at_refusal, Now&& now_us, Total&& total_now, Sleep&& sleep_ms, Stop&& stopped) noexcept
{
    const auto elapsed_ms = [&]() noexcept -> u32
    {
        const u64 now = now_us();
        const u64 ms = now > since_us ? (now - since_us) / 1000 : 0;
        return ms > 0xffffffffull ? 0xffffffffu : static_cast<u32>(ms);
    };
    for (u32 sleeps = 0; sleeps < 2 * (media_retry_ceiling_ms / media_retry_step_ms); sleeps++)
    {
        if (stopped()) break;
        const u32 step = media_retry_wait_ms(total_now(), load_at_refusal, elapsed_ms());
        if (!step) break;
        sleep_ms(step);
    }
    return elapsed_ms();
}

// 0.40: rate of the failure lines of the HLE codec opens and of the decoder diagnostics: a game may reopen a refused
// decoder in a tight loop, so the first 8 of a counter are logged, then one in 100.
constexpr bool media_log_due(u32 number) noexcept
{
    return number <= 8 || number % 100 == 0;
}

// Counts one more in 'count'; 'number' receives its 1-based count. True when its line is due. Any thread, lock-free.
inline bool media_log_due(std::atomic<u32>& count, u32& number) noexcept
{
    number = count.fetch_add(1, std::memory_order_relaxed) + 1;
    return media_log_due(number);
}

// 0.40 opt-in (row ppu_boot_workers, ppu-boot-workers.flag or features.txt): the boot compile stage of the PPU modules
// (the main thread's initialize: no game code has run and no other guest thread exists) uses up to boot_workers_total
// compile threads, the 2 LLVM threads plus both optional workers, while nothing presses: memory and thread level 0, at
// most 48 RPCS3 threads, at least 1 GiB of heap room, and no adapt key of the title from an out-of-memory compile, a
// refused thread or the heap cap. Its compiles are admitted against boot_compile_budget (boot_compile_admit) while the
// memory level stays 0: any level change brings the normal rules back for the next module.
inline constexpr u32 boot_workers_total = 4;
inline constexpr u32 boot_workers_base = 2; // the PS4 policy's LLVM thread count
inline constexpr u32 boot_workers_max_threads = 48;
inline constexpr u64 boot_workers_min_room = 1ull << 30;
inline constexpr u64 boot_compile_budget = 2 * ::utils::ps4::mem_budget::compile_budget;

struct boot_workers_state
{
    bool row_on = false;     // the ppu_boot_workers row
    bool boot_stage = false; // the main thread's boot compile with no other guest thread
    u32 llvm_threads = 0;    // LLVM threads in force
    u32 modules = 0;         // modules of this workload
    int memory_level = 0;
    int thread_level = 0;
    u32 threads = 0;         // live RPCS3 threads
    bool room_known = false;
    s64 room = 0;            // heap room in bytes
    bool adapt_key = false;  // an adapt key of the title that asks for fewer compile threads
};

// nullptr: the boot rule applies; else the first condition that keeps the normal workers (for the startup line).
constexpr const char* boot_workers_block(const boot_workers_state& s) noexcept
{
    if (!s.row_on) return "ppu_boot_workers off";
    if (!s.boot_stage) return "not the boot compile stage";
    if (s.llvm_threads != boot_workers_base) return "LLVM threads not 2";
    if (s.modules <= boot_workers_base) return "2 modules or fewer";
    if (s.memory_level != 0) return "memory level above 0";
    if (s.thread_level != 0) return "thread level above 0";
    if (s.threads > boot_workers_max_threads) return "more than 48 RPCS3 threads";
    if (!s.room_known || s.room < static_cast<s64>(boot_workers_min_room)) return "heap room below 1 GiB";
    if (s.adapt_key) return "adapt key of this title (llvm_threads=1, threads=low or heap_cap)";
    return nullptr;
}

// Optional workers on top of the 2 LLVM threads under the boot rule (0 when it does not apply), at most
// 'optional_max' (the permits of the optional workers) and never more than the modules left for them.
constexpr u32 boot_workers_optional(const boot_workers_state& s, u32 optional_max) noexcept
{
    if (boot_workers_block(s)) return 0;
    const u32 extra = boot_workers_total - boot_workers_base;
    const u32 n = extra < optional_max ? extra : optional_max;
    const u32 left = s.modules - boot_workers_base;
    return n < left ? n : left;
}

// Compile admission of a boot-rule worker: the normal rule (mem_budget::compile_admit) at any memory level above 0;
// at level 0 the larger in-flight budget, with the same room guard (the compiles in flight leave the level 2 room
// free). The first compile always passes.
constexpr bool boot_compile_admit(s64 room, u64 in_flight, u64 bytes, int level) noexcept
{
    if (level != 0) return ::utils::ps4::mem_budget::compile_admit(room, in_flight, bytes, level);
    if (!in_flight) return true;
    if (in_flight + bytes > boot_compile_budget) return false;
    return room - static_cast<s64>(in_flight + bytes) >= static_cast<s64>(::utils::ps4::mem_budget::l2_room);
}

// First word of a firmware text ("13.020.001 (0x13020001)" -> "13.020.001"), leading blanks skipped.
constexpr std::string_view firmware_key(std::string_view text) noexcept
{
    const usz first = text.find_first_not_of(" \t");
    if (first == std::string_view::npos) return {};
    text.remove_prefix(first);
    const usz end = text.find_first_of(" \t\r\n");
    return end == std::string_view::npos ? text : text.substr(0, end);
}

// thread-cap.txt: one "<firmware> <cap>" line per firmware; '#' starts a comment line. The cap of 'firmware' (its
// last line wins), 0 when there is none. Values are clamped (clamp_thread_cap).
constexpr u32 parse_thread_cap(std::string_view text, std::string_view firmware) noexcept
{
    const std::string_view key = firmware_key(firmware);
    u32 found = 0;
    while (!key.empty() && !text.empty())
    {
        const usz end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        const std::string_view word = firmware_key(line);
        if (word.empty() || word[0] == '#' || word != key) continue;
        line.remove_prefix(line.find(word) + word.size());
        const usz digit = line.find_first_not_of(" \t");
        if (digit == std::string_view::npos || line[digit] < '0' || line[digit] > '9') continue;
        u32 value = 0;
        for (usz i = digit; i < line.size() && line[i] >= '0' && line[i] <= '9' && value < 100000; i++)
        {
            value = value * 10 + static_cast<u32>(line[i] - '0');
        }
        found = clamp_thread_cap(value);
    }
    return found;
}

// The file text with the line of 'firmware' set to 'cap' (every other line kept in order; the line appended when
// missing). "" when the firmware key is empty.
inline std::string with_thread_cap(std::string_view text, std::string_view firmware, u32 cap)
{
    const std::string_view key = firmware_key(firmware);
    if (key.empty()) return {};
    std::string out;
    bool replaced = false;
    while (!text.empty())
    {
        const usz end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty()) continue;
        if (firmware_key(line) == key)
        {
            if (replaced) continue;
            replaced = true;
            out.append(key).append(" ").append(std::to_string(clamp_thread_cap(cap))).append("\n");
            continue;
        }
        out.append(line).append("\n");
    }
    if (!replaced) out.append(key).append(" ").append(std::to_string(clamp_thread_cap(cap))).append("\n");
    return out;
}

// Free heap budget (cap - live), 0 once the heap is critical. Only meaningful with a known cap (s.cap != 0).
constexpr u64 heap_room(const heap_sample& s) noexcept
{
    return s.critical || s.cap <= s.live ? 0 : s.cap - s.live;
}

// Runtime SPU LLVM compilers (one per SPUW worker, or the inline one on the 'SPU LLVM' thread) are dropped after
// spu_recycle_programs programs, or after spu_recycle_programs_short while the heap has less than spu_recycle_room
// of room or is critical. Only the heap counts here: low flexible memory (thread stacks) is not something a
// compiler reset gives back, and it stays low for whole sessions (Demon's Souls gameplay sits at 84-86 MiB).
inline constexpr u32 spu_recycle_programs = 256;
inline constexpr u32 spu_recycle_programs_short = 16;
inline constexpr u64 spu_recycle_room = 512ull << 20;
inline constexpr u64 spu_recycle_programs_max = 1'000'000;
// Decimal program count that replaces spu_recycle_programs (the short limit never exceeds it); 0 = never recycle.
inline constexpr char spu_recycle_file[] = "/data/rpcs3/spu-recycle-programs.txt";

// Programs a runtime compiler may build before it is dropped, for the heap state 's'; 0 = never. 'limit' is the
// normal limit (spu_recycle_programs or the file value).
constexpr u32 spu_recycle_limit_for(const heap_sample& s, u32 limit) noexcept
{
    if (!limit) return 0;
    const bool short_heap = s.critical || (s.cap && heap_room(s) < spu_recycle_room);
    return short_heap && limit > spu_recycle_programs_short ? spu_recycle_programs_short : limit;
}

// 0.38: the same budget in object bytes. A compiler that builds few but huge programs (giga blocks) grows its LLVM
// context and RuntimeDyld tables with the code size, not the program count: it is dropped once it emitted this much.
inline constexpr u64 spu_recycle_bytes = 8ull << 20;

// True when a compiler that built 'programs' programs and emitted 'emitted' object bytes must be dropped now; 'limit'
// is spu_recycle_limit_for() (0 = never recycle, the bytes included).
constexpr bool spu_recycle_due(u32 programs, u64 emitted, u32 limit) noexcept
{
    return limit && (programs >= limit || emitted >= spu_recycle_bytes);
}

// 0.38: last stop before the cap. With less heap room than this (or at level 2 with less than heap_room_floor), the
// boot SPU cache build takes no new program (the rest compile while the game runs, after their spu_fast code) and the
// runtime dispatcher hands out no block: a compile needs a few MiB of small-heap growth, which musl never returns, and
// the game's own large allocations need the rest. 'margin' is the hysteresis of a brake that is engaged.
inline constexpr u64 heap_room_stop = 192ull << 20;

constexpr bool compile_room_short(const heap_sample& s, u64 margin = 0) noexcept
{
    if (!s.cap) return false;
    const u64 room = s.cap > s.live ? s.cap - s.live : 0;
    return room < heap_room_stop + margin || (s.critical && room < heap_room_floor + margin);
}

// 0.38: JIT private memory budget: the private direct commits of ps4_virtual_memory (jit_runtime code and data, PPU
// link arenas, PPU executable tables; 113-398 MiB in the 0.37 sessions). New SPU LLVM code stops jit_room_stop before
// it (boot build ends, runtime compiles wait on the fast path), so spu_fast stubs, trampolines and PPU code still find
// pages. JIT memory is only returned at the stop, so an engaged brake stays engaged for the rest of the game.
inline constexpr u64 jit_budget = 640ull << 20;
inline constexpr u64 jit_room_stop = 32ull << 20;

constexpr bool jit_room_short(u64 committed, u64 margin = 0) noexcept
{
    return committed + jit_room_stop + margin >= jit_budget;
}

// Boot SPU cache build. 'workers' is upstream's count, min(LLVM thread setting, programs): 2 with the PS4 profile.
// Extra workers (up to spu_cache_workers_max in total) are tried only when that count is at least 2 (1 comes from
// the per-title adaptation after a refused thread or an out-of-memory compile, and must stay 1), compiles are not
// serialized, the heap cap is known with spu_cache_workers_room of room, and fewer than spu_cache_workers_threads
// threads are live. The game code has not started at that point, and every worker frees its compiler when the build
// ends.
inline constexpr u32 spu_cache_workers_max = 4;
inline constexpr u64 spu_cache_workers_room = 512ull << 20;
inline constexpr u32 spu_cache_workers_threads = 48;
inline constexpr char no_extra_spu_workers_flag[] = "/data/rpcs3/no-extra-spu-workers.flag";

// Heap reserve. 64 bytes below the block size: musl's malloc adds its chunk header and rounds the mapping up to the
// page size, so each block is exactly one mapping, and a released block can serve small-heap growth steps as well as
// large allocations.
// 0.38: one contiguous 48 MiB block (0.37: 3 x 16 MiB, and FIFA's second boot burned all three in 60 ms on one 71 MiB
// request), released whole and taken again every time the XMB opens.
inline constexpr u32 reserve_blocks = 1;
inline constexpr u64 reserve_block_bytes = 48ull << 20;
inline constexpr u64 reserve_request_bytes = reserve_block_bytes - 64;
// The reserve is taken only while the heap keeps more than this much room afterwards, so taking it never raises the
// governor level by itself.
inline constexpr u64 reserve_min_room = heap_room_floor + reserve_blocks * reserve_block_bytes;

// Number of extra (optional) SPU cache workers to try on top of 'workers'; 0 = none.
constexpr u32 spu_cache_extra_workers_for(u32 workers, u64 programs, const heap_sample& s, u32 live_threads, bool serialized) noexcept
{
    if (workers < 2 || workers >= spu_cache_workers_max || programs <= workers || serialized) return 0;
    if (!s.cap || heap_room(s) < spu_cache_workers_room) return 0;
    if (live_threads >= spu_cache_workers_threads) return 0;
    const u64 total = programs < spu_cache_workers_max ? programs : spu_cache_workers_max;
    return static_cast<u32>(total) - workers;
}

// 0.40: the inputs of the compile serialization decision, for the text that names why a build runs on one thread
// (serialize_reason).
struct serialize_state
{
    bool v038 = true;        // the rules in force (the 0.38 rules are the default)
    int memory_level = 0;    // 0..3
    bool room_known = false; // the heap cap is known
    s64 room = 0;            // heap room under the cap, bytes
    bool cpu_l1 = false;     // 0.39 rules: memory level 1 also serializes PPU/SPU compiles
    u32 threads = 0;         // 0.38 rules: live RPCS3 threads (registry); 0.39 rules: live + exited not joined
    u32 cap = 0;             // 0.39 rules: the thread cap in force
    bool cool = false;       // "Modo frio"
    bool cool_stage = false; // a boot compile stage runs (0.39 rules: "Modo frio" then allows two compiles in total)
};

// The causes that serialize compiles under the rules in force, in the order the slot mode looks at them
// (mem_budget::slot_mode_v038 / slot_mode_v039), each one enough on its own, joined by "; ":
//   "memory level 2, room 300 MiB"
//   "thread level 2, 74 of 80"                                 (0.39 rules: live + exited not joined of the cap)
//   "thread level 2, 66 live RPCS3 threads (levels at 56/64)"  (0.38 rules: the thread registry)
//   "cool mode"
// and "none now" once none holds any more (the levels can fall between the decision and this text). NUL-terminated,
// truncated to fit; returns the length. Allocation-free.
inline usz format_serialize_reason(char* out, usz size, const serialize_state& st) noexcept
{
    if (!out || !size) return 0;
    out[0] = '\0';
    usz len = 0;
    u32 causes = 0;
    const auto append = [&](const char* text) noexcept
    {
        for (const char* p = causes ? "; " : ""; *p && len + 1 < size; ++p) out[len++] = *p;
        for (const char* p = text; *p && len + 1 < size; ++p) out[len++] = *p;
        out[len] = '\0';
        causes++;
    };

    char part[128];
    const int level = st.memory_level < 0 ? 0 : st.memory_level > 3 ? 3 : st.memory_level;
    const auto memory = [&](const char* why) noexcept
    {
        if (st.room_known)
            std::snprintf(part, sizeof(part), "memory level %d, room %lld MiB%s", level, static_cast<long long>(st.room / (s64{1} << 20)), why);
        else
            std::snprintf(part, sizeof(part), "memory level %d%s", level, why);
        append(part);
    };

    if (st.v038)
    {
        if (level > 0) memory("");
        if (classify_threads(st.threads) >= 2)
        {
            std::snprintf(part, sizeof(part), "thread level 2, %u live RPCS3 threads (levels at %u/%u)", st.threads, thread_warn, thread_high);
            append(part);
        }
        if (st.cool) append("cool mode");
    }
    else
    {
        // Memory level 1 serializes PPU/SPU only on its floor (room, flexible memory) or a sustained slope
        if (level >= 2) memory("");
        else if (level == 1 && st.cpu_l1) memory(" (below 384 MiB of room, low flexible memory or a sustained slope)");
        if (classify_thread_load(st.threads, st.cap) >= 2)
        {
            std::snprintf(part, sizeof(part), "thread level 2, %u of %u", st.threads, clamp_thread_cap(st.cap));
            append(part);
        }
        // A boot compile stage keeps two compiles in "Modo frio": not serialized by it
        if (st.cool && !st.cool_stage) append("cool mode");
    }
    if (!causes) append("none now");
    return len;
}

#ifdef RPCS3_PS4
namespace detail
{
inline std::atomic<u32> g_active{0};       // compile slots currently held (0.39: packed per class, mem_budget::slots_of)
inline std::atomic<u64> g_waits{0};        // compile slots that had to wait
inline std::atomic<int> g_heap_level{-1};  // last reported level, -1 = not sampled yet
inline std::atomic<int> g_thread_level{-1};
// Line budgets. The thread count can hover around a threshold while workers come and go, so thread changes have
// their own budget and can never use up the one that reports the heap.
inline std::atomic<u32> g_lines{0};
inline std::atomic<u32> g_thread_lines{0};
inline std::atomic<u64> g_heap_sampled_us{0}; // steady-clock time of the last heap sample
inline constexpr u64 heap_sample_interval_us = 50'000;
inline std::atomic<u64> g_heap_polled_us{0};  // poll_heap(): time of the last forced sample
inline constexpr u64 heap_poll_interval_us = 1'000'000;

// Heap level 2 bookkeeping. Title generations are adapt::g_title_seq values (even while a title is set).
inline constexpr u32 no_generation = 0xffffffffu;
inline constexpr u32 no_title_generation = 0xfffffffdu;
inline std::atomic<bool> g_restart_advised{false};
inline std::atomic<u32> g_critical_generation{no_generation}; // generation that first saw level 2
inline std::atomic<u32> g_notified_generation{no_generation}; // last generation that got the save-now notice
inline std::atomic<u64> g_fallback_seen{0};                   // fallback bytes at the last reserve check

// Heap reserve (arm_reserve). Each slot holds one block or null.
inline std::atomic<void*> g_reserve[reserve_blocks]{};
inline std::atomic<u32> g_reserve_left{0};
inline std::atomic<u32> g_reserve_released{0};
inline std::atomic<bool> g_reserve_armed{false};    // taken (or given up for good) once per process
inline std::atomic<bool> g_reserve_skip_logged{false};

inline std::atomic<bool> g_cool_logged{false};
inline std::atomic<bool> g_cool_stage_logged{false}; // "Modo frio" compile stage on two slots, logged once per process

// 0.38: budget authority (budget_tick; one thread at a time through g_budget_busy).
inline constexpr u64 heap_build_cap = 2048ull << 20; // ORBIS_MMAP_DIRECT_CAP_MIB of ps4-port/dependencies.cmake
inline constexpr u64 budget_tick_interval_us = 1'000'000;
inline constexpr char no_budget_flag[] = "/data/rpcs3/no-mem-budget.flag";
inline std::atomic<u64> g_budget_tick_us{0};
inline std::atomic<bool> g_budget_busy{false};
inline std::atomic<u32> g_budget_ticks{0};
inline std::atomic<u64> g_cap_eff{0};
inline std::atomic<u64> g_reserve_set{0};
inline std::atomic<u64> g_free_total{0};
inline std::atomic<u64> g_largest_run{0};
inline std::atomic<u64> g_allocated{0};
inline std::atomic<u64> g_total{0};
inline std::atomic<u64> g_system{0};
inline std::atomic<u64> g_used_gpu{0};
inline std::atomic<u64> g_guest{0};
inline std::atomic<u64> g_jit{0};
inline std::atomic<u64> g_stacks{0};
inline std::atomic<bool> g_estimated{false};
inline std::atomic<u64> g_test_heap_cap{0};               // health: test-heap-cap-mib.txt
inline std::atomic<u64 (*)() noexcept> g_jit_source{nullptr}; // set_jit_source
inline std::atomic<u32> g_budget_lines{0};

// Governor v2 levels. g_level_state, g_slope_room and g_slope_us belong to the heap_pressure() sampler (one thread at
// a time, through g_heap_sampled_us).
inline std::atomic<int> g_level{0};
inline ::utils::ps4::mem_budget::level_state g_level_state{};
inline std::atomic<s64> g_slope{0};
inline s64 g_slope_room = 0;
inline u64 g_slope_us = 0;
inline unsigned g_slope_streak = 0;            // 0.39: closed windows in a row that predicted level 2
inline std::atomic<bool> g_cpu_l1{false};      // 0.39: level 1 also serializes PPU/SPU compiles
inline std::atomic<bool> g_cpu_l1_floor{false}; // the 0.38 rules, flexible floor or room < 384 MiB (any stage)
inline std::atomic<bool> g_cpu_l1_slope{false}; // the slope rule at the last sample, the compile stage not considered
inline std::atomic<u32> g_compile_stage{0};    // 0.39: boot compile stages in progress (compile_stage)
inline std::atomic<u64> g_refusals_seen{0};
inline std::atomic<u64> g_refusal_us{0};       // last refused or unserved allocation (counters, hook, new_handler)
inline std::atomic<bool> g_l3_seen{false};      // level 3 reached in this process
inline std::atomic<u32> g_low_generation{no_generation}; // last title generation that got the level-2 toast
inline std::atomic<u64> g_level_waits{0};       // compile slots that waited at level 2 or above
inline std::atomic<u64> g_pressure_calls{0};    // pressure hook calls
inline std::atomic<s64> g_compile_in_flight{0}; // try_admit_compile bytes
inline std::atomic<u64> g_admission_waits{0};   // compile_admission objects that had to wait

// Shedders (register_shedder). Slots are filled once (main thread, before the game) and run by budget_tick.
inline constexpr u32 max_shedders = 16;
inline constexpr u64 shed_repeat_us = 30'000'000;
struct shedder_slot
{
    std::atomic<u64 (*)(int) noexcept> fn{nullptr};
    const char* name = "";
    int level = 2;
    u64 last_us = 0;  // budget_tick only
    bool active = false;
};
inline shedder_slot g_shedders[max_shedders];
inline std::atomic<u32> g_shedder_count{0};
inline std::atomic<u64> g_shed_runs{0};
inline std::atomic<u64> g_shed_bytes{0};
inline std::atomic<u32> g_shed_lines{0};

// Session of the running title (begin_session .. session_record), for its memory record.
inline std::atomic<bool> g_session{false};
inline std::atomic<u64> g_session_boot_live{0};
inline std::atomic<u64> g_session_peak_live{0};
inline std::atomic<u64> g_session_peak_jit{0};
inline std::atomic<u64> g_session_peak_guest{0};
inline std::atomic<u64> g_session_peak_window{0};
inline std::atomic<int> g_session_max_level{0};

inline u64 now_us() noexcept
{
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

inline void log_line(const char* text, std::atomic<u32>& budget = g_lines, bool always = false) noexcept
{
    if (budget.fetch_add(1, std::memory_order_relaxed) < 32 || always) ps4_startup_checkpoint(text);
}

// 0.39: level transitions also go to RPCS3.log through this writer (set_rpcs3_log; null = startup.log only). Not for
// level 3 (near heap exhaustion: the RPCS3 logger formats and may allocate); 64 lines per process.
inline std::atomic<void (*)(const char*) noexcept> g_rpcs3_log{nullptr};
inline std::atomic<u32> g_rpcs3_log_lines{0};

// Logs a change of level; the first sample is logged only when it is not 0. 'effect' says what the new level does.
// 'always': logged even when the budget is spent (the heap reaches level 2 at most once per process).
inline void log_transition(const char* what, int from, int to, const char* numbers, const char* effect,
    std::atomic<u32>& budget = g_lines, bool always = false) noexcept
{
    if (from == to || (from < 0 && to == 0)) return;
    char line[448];
    std::snprintf(line, sizeof(line), "memory governor: %s pressure %d -> %d (%s): %s", what, from < 0 ? 0 : from, to,
        numbers, effect);
    log_line(line, budget, always);
    if (to <= 2)
    {
        const auto writer = g_rpcs3_log.load(std::memory_order_acquire);
        if (writer && g_rpcs3_log_lines.fetch_add(1, std::memory_order_relaxed) < 64) writer(line);
    }
}

// Any memory level above 0 serializes compiles. 0.38: level 2 and 3 do more (see the top of this file).
constexpr const char* level_effect(int to) noexcept
{
    return to >= 3 ? "every shedder, heap reserve for a failing request, save-now notice; the next boot needs a clean process"
        : to == 2 ? "compiles wait between programs, level-2 shedders and heap trim, save notice"
        : to == 1 ? "compiles serialized, no optional workers" : "compiles parallel again";
}

// Thread level 1 is only a warning; level 2 serializes compiles and makes optional threads run inline.
constexpr const char* thread_effect(int from, int to) noexcept
{
    if (to >= 2) return "compiles serialized, optional threads run inline";
    if (to == 1) return from >= 2 ? "optional threads allowed again" : "warning only, nothing restricted";
    return "back to normal";
}

// Reads a decimal number from a small text file (leading blanks allowed), clamped to 'max' (at most 2^60).
// false when the file cannot be read or does not start with a digit; 'value' is then left unchanged.
inline bool read_decimal_file(const char* path, u64 max, u64& value) noexcept
{
    const int fd = ::open(path, O_RDONLY);
    if (fd < 0) return false;
    char text[32]{};
    const auto n = ::read(fd, text, sizeof(text) - 1);
    ::close(fd);
    long i = 0;
    while (i < n && (text[i] == ' ' || text[i] == '\t' || text[i] == '\r' || text[i] == '\n')) i++;
    if (i >= n || text[i] < '0' || text[i] > '9') return false;
    u64 v = 0;
    for (; i < n && text[i] >= '0' && text[i] <= '9'; i++)
    {
        v = v * 10 + static_cast<u64>(text[i] - '0');
        if (v >= max)
        {
            v = max;
            break;
        }
    }
    value = v;
    return true;
}

// Reads the test cap once. Returns bytes, 0 when the file is absent or not a positive number.
inline u64 read_test_cap() noexcept
{
    u64 mib = 0;
    if (!read_decimal_file(test_cap_file, 1ull << 30, mib) || !mib) return 0;
    char line[160];
    std::snprintf(line, sizeof(line), "memory governor: test cap %llu MiB from %s", static_cast<unsigned long long>(mib), test_cap_file);
    log_line(line);
    return mib << 20;
}
} // namespace detail

// Heap cap from the test file, in bytes (0 = none). Read once per process.
inline u64 test_cap_bytes() noexcept
{
    static const u64 cap = detail::read_test_cap();
    return cap;
}

// One fresh sample (no rate limit): a flexible-memory query plus relaxed loads of the heap counters. With
// 'flexible' false the query (a syscall) is skipped and flexible_free stays 0 (unknown): cheap enough to call after
// every compile.
inline heap_sample sample_heap(bool flexible = true) noexcept
{
    heap_sample s{};
    if (flexible) s.flexible_free = ps4_flexible_memory_free();
    orbis::MmapDirectStats st{};
    orbis::mmapDirectStats(&st);
    const u64 test_cap = test_cap_bytes();
    s.cap = test_cap && (!st.cap || test_cap < st.cap) ? test_cap : st.cap;
    s.live = st.live;
    // A heap that reached its cap or has requests served from flexible memory has no room left that the governor can
    // count on. 0.38: the flag is cleared once the level falls below 2 (update_level), and only fallback bytes still
    // mapped count (0.37: every byte that ever fell back, so the state never ended).
    s.critical = st.critical != 0 || st.fallback_live != 0;
    s.fallback_bytes = st.fallback_bytes;
    s.fallback_live = st.fallback_live;
    s.refusals = st.refusals + st.pressure_events;
    return s;
}

namespace detail
{
// Level 2 seen (any thread, allocation-free). Once per boot of a title: one toast to save and restart; the adapt
// keys only when this boot is the one during which the heap first became critical (a game booted into a heap that
// was already critical did not cause it). Without a running title only restart_advised() changes.
inline void on_critical() noexcept
{
    g_restart_advised.store(true, std::memory_order_relaxed);
    const u32 generation = ::utils::ps4::adapt::g_title_seq.load(std::memory_order_acquire);
    char title[::utils::ps4::adapt::title_size];
    const bool running = ::utils::ps4::adapt::current_title(title) && !(generation & 1);
    u32 expected = no_generation;
    g_critical_generation.compare_exchange_strong(expected, running ? generation : no_title_generation, std::memory_order_acq_rel);
    if (!running) return;

    u32 last = g_notified_generation.load(std::memory_order_acquire);
    if (last == generation || !g_notified_generation.compare_exchange_strong(last, generation, std::memory_order_acq_rel)) return;

    ps4_notify(::utils::ps4::i18n::tr(::utils::ps4::i18n::str::MEMGOV_MEMORIA_EMULADOR_QUASE_ESGOTADA_SALVE));
    const bool caused_here = g_critical_generation.load(std::memory_order_acquire) == generation;
    if (caused_here)
    {
        ::utils::ps4::adapt::note(::utils::ps4::adapt::key_heap_cap);
        ::utils::ps4::adapt::note(::utils::ps4::adapt::key_llvm_threads_1);
    }
    char line[192];
    std::snprintf(line, sizeof(line), "heap critical while %s runs: save-now notice shown, restart advised%s", title,
        caused_here ? "; adapt keys heap=cap and llvm_threads=1 noted for its next boots" : " (the heap was already critical when it booted)");
    ps4_fatal_note("memory governor: ", line);
}

inline void reserve_new_handler();

// Removes the reserve's new_handler (only if it is still the installed one).
inline void uninstall_reserve_handler() noexcept
{
    if (std::get_new_handler() == &reserve_new_handler) std::set_new_handler(nullptr);
}

// Frees one reserve block; false when none is left. Any thread, allocation-free (free() of a mapped chunk only
// unmaps it). 'why' ends the log line.
inline bool release_reserve_block(const char* why) noexcept
{
    // 0.38: a released block means an allocation could not be served: level 3 for the next samples.
    g_refusal_us.store(now_us() | 1, std::memory_order_relaxed);
    for (auto& slot : g_reserve)
    {
        void* const block = slot.exchange(nullptr, std::memory_order_acq_rel);
        if (!block) continue;
        std::free(block);
        const u32 left = g_reserve_left.fetch_sub(1, std::memory_order_acq_rel) - 1;
        g_reserve_released.fetch_add(1, std::memory_order_relaxed);
        if (!left) uninstall_reserve_handler();
        char line[192];
        std::snprintf(line, sizeof(line), "released %llu MiB (%u left): %s", static_cast<unsigned long long>(reserve_block_bytes >> 20), left, why);
        ps4_fatal_note("heap reserve: ", line);
        on_critical();
        return true;
    }
    uninstall_reserve_handler();
    return false;
}

// std::new_handler: operator new calls it after malloc returned null and retries the allocation when it returns.
// With nothing left to free it uninstalls itself, so the next failure throws std::bad_alloc as without a reserve.
inline void reserve_new_handler()
{
    release_reserve_block("an allocation failed; it is retried");
}

// Level 2 with more bytes in flexible memory than at the last check: give one block back to the heap, so the next
// large requests are served from the carve-outs instead of the pool the thread stacks need.
inline void release_on_fallback(u64 fallback_bytes) noexcept
{
    u64 seen = g_fallback_seen.load(std::memory_order_relaxed);
    if (fallback_bytes <= seen || !g_fallback_seen.compare_exchange_strong(seen, fallback_bytes, std::memory_order_relaxed)) return;
    if (g_reserve_left.load(std::memory_order_acquire)) release_reserve_block("heap at level 3 and more memory fell back to flexible memory");
}

// 0.38: level 2 seen while a title runs: one toast per boot of the title (allocation-free).
inline void on_low() noexcept
{
    const u32 generation = ::utils::ps4::adapt::g_title_seq.load(std::memory_order_acquire);
    char title[::utils::ps4::adapt::title_size];
    if (!::utils::ps4::adapt::current_title(title) || (generation & 1)) return;
    u32 last = g_low_generation.load(std::memory_order_acquire);
    if (last == generation || !g_low_generation.compare_exchange_strong(last, generation, std::memory_order_acq_rel)) return;
    ps4_notify(::utils::ps4::i18n::tr(::utils::ps4::i18n::str::MEMGOV_RPCS3_MEMORIA_EMULADOR_BAIXA_SALVE));
    char line[160];
    std::snprintf(line, sizeof(line), "memory level 2 while %s runs: save notice shown, compiles wait, caches shed", title);
    ps4_fatal_note("memory governor: ", line);
}

// 0.38: orbis-compat's pressure hook (orbis::setPressureHook): a request no carve-out could serve, before the
// flexible-memory fallback. Runs on the allocating thread without the allocator lock, possibly holding musl's heap
// lock (MmapPressureSmallHeap): it only frees the reserve block (free() of a mapped chunk returns it to its carve-out
// for the retry), marks level 3 and writes an allocation-free note. Never waits.
inline void pressure_hook(unsigned long long bytes, unsigned why) noexcept
{
    g_refusal_us.store(now_us() | 1, std::memory_order_relaxed);
    const u64 n = g_pressure_calls.fetch_add(1, std::memory_order_relaxed) + 1;
    const bool released = g_reserve_left.load(std::memory_order_acquire) && release_reserve_block("a heap request found no carve-out room; it is retried");
    if (n <= 8 || !(n & (n - 1)))
    {
        char line[224];
        std::snprintf(line, sizeof(line), "request of %llu KiB found no heap room (%s%s%s%s), #%llu; %s", bytes >> 10,
            (why & orbis::MmapPressureSmallHeap) ? "small heap " : "", (why & orbis::MmapPressureReserve) ? "reservation " : "",
            (why & orbis::MmapPressureCap) ? "cap " : "", (why & orbis::MmapPressureKernel) ? "kernel" : "", static_cast<unsigned long long>(n),
            released ? "heap reserve released for it" : "no heap reserve left: flexible memory next");
        ps4_fatal_note("memory governor: ", line);
    }
}

// Level 3 or 2 -> below 2: the heap's critical flag goes (the counters stay).
inline void level_fell_below_2() noexcept
{
    orbis::mmapDirectClearCritical();
}

// The rules in force, written to startup.log at the first call and whenever they change (a game boot can switch them).
inline void log_rules(bool v038) noexcept
{
    if (!v038)
    {
        ps4_startup_checkpoint("memory governor: 0.39 rules (governor-v039.flag or features.txt): level 1 below max(512 MiB, cap/4) of room or a slope over two 30 s windows outside compile stages; level 1 serializes PPU/SPU only below 384 MiB or on that slope; slots one per class (cpu/rsx/interp) at level 1, one in total from level 2; thread levels at cap - 16 / cap - 8");
        return;
    }
    const bool killed = ::access(v038_flag, F_OK) == 0;
    const bool safe = ::utils::ps4::safe_mode::g_level.load(std::memory_order_relaxed) != 0;
    ps4_startup_checkpoint(killed
        ? "memory governor: 0.38 rules (governor-v038.flag): level 1 below 768 MiB of room or one slope window, every compile shares one slot from level 1, thread levels from the registry at 56/64"
        : safe ? "memory governor: 0.38 rules (safe mode): level 1 below 768 MiB of room or one slope window, every compile shares one slot from level 1, thread levels from the registry at 56/64"
        : "memory governor: 0.38 rules (default; governor-v039.flag: 0.39 rules): level 1 below 768 MiB of room or one slope window, every compile shares one slot from level 1, thread levels from the registry at 56/64");
}

// The 0.38 rules unless the governor_v039 row is on (util/ps4_safe_mode.hpp). Read at every decision, so a game's
// safe-mode level or a changed flag file applies from its boot on. One relaxed load while nothing changes.
inline bool governor_v038() noexcept
{
    static std::atomic<int> s_logged{-1};
    const bool v038 = !::utils::ps4::safe_mode::enabled(::utils::ps4::safe_mode::feature::governor_v039);
    const int value = v038 ? 1 : 0;
    if (s_logged.load(std::memory_order_relaxed) != value && s_logged.exchange(value, std::memory_order_relaxed) != value)
    {
        log_rules(v038);
    }
    return v038;
}

inline bool compile_stage_active() noexcept
{
    return g_compile_stage.load(std::memory_order_relaxed) != 0;
}

// Whether level 1 serializes PPU/SPU compiles now (0.39 rules). The slope part never counts while a compile stage
// runs: g_cpu_l1 is the value of the last heap sample, which may predate the stage (Demon's Souls 0.39 built its SPU
// cache on 1 worker, 92 ms before the sample that would have cleared it).
inline bool cpu_l1_now() noexcept
{
    return ::utils::ps4::mem_budget::cpu_l1_in_stage(g_cpu_l1_floor.load(std::memory_order_relaxed),
        g_cpu_l1_slope.load(std::memory_order_relaxed), compile_stage_active());
}

// Effect text of a heap level change under the rules in force.
inline const char* level_effect_now(int to) noexcept
{
    if (to == 1 && !governor_v038())
    {
        return cpu_l1_now() ? "PPU/SPU compiles serialized (room below 384 MiB or a sustained slope), no optional workers"
                            : "no optional workers, compiles stay parallel";
    }
    return level_effect(to);
}

// The v2 level for a fresh sample (see util/ps4_mem_budget.hpp). heap_pressure() sampler only.
inline int update_level(const heap_sample& s) noexcept
{
    namespace mb = ::utils::ps4::mem_budget;
    const u64 now = now_us();
    if (s.refusals > g_refusals_seen.load(std::memory_order_relaxed))
    {
        g_refusals_seen.store(s.refusals, std::memory_order_relaxed);
        g_refusal_us.store(now | 1, std::memory_order_relaxed);
    }
    const u64 refused_at = g_refusal_us.load(std::memory_order_relaxed);
    const bool refused = refused_at && now - refused_at < mb::refusal_hold_us;
    const s64 room = s.cap ? static_cast<s64>(s.cap) - static_cast<s64>(s.live) : (s64{1} << 40);
    const bool v038 = governor_v038();
    const bool stage = compile_stage_active();
    if (!g_slope_us)
    {
        g_slope_us = now;
        g_slope_room = room;
    }
    else if (now - g_slope_us >= mb::slope_window_us)
    {
        const s64 slope = mb::slope_per_min(g_slope_room, g_slope_us, room, now);
        g_slope.store(slope, std::memory_order_relaxed);
        g_slope_streak = mb::next_slope_streak(g_slope_streak, room, slope, stage);
        g_slope_us = now;
        g_slope_room = room;
    }
    const s64 slope = g_slope.load(std::memory_order_relaxed);
    // 0.39: L1 threshold relative to the cap, slope over two windows outside compile stages (governor-v038.flag: 0.38)
    const u64 l1_threshold = v038 ? mb::l1_room : mb::l1_room_for(s.cap);
    const u64 l1_clear_room = v038 ? mb::l1_room + mb::l1_clear_margin : l1_threshold + mb::l1_clear_margin_v039;
    int raw = v038 ? mb::raw_level(room, slope, refused) : mb::raw_level_v039(room, slope, refused, l1_threshold, g_slope_streak, stage);
    // Thread stacks live in flexible memory: below the floor compiles are serialized, as in 0.37.
    const bool flexible_low = s.flexible_free && s.flexible_free < flexible_floor;
    if (flexible_low && raw < 1) raw = 1;
    // Same value as l1_serializes_cpu(room, slope_rule_v039(..., stage)), kept in two parts for cpu_l1_now()
    const bool cpu_floor = v038 || flexible_low || room < static_cast<s64>(mb::cpu_serialize_room);
    const bool slope_cpu = !v038 && mb::slope_rule_v039(room, slope, g_slope_streak, false);
    g_cpu_l1_floor.store(cpu_floor, std::memory_order_relaxed);
    g_cpu_l1_slope.store(slope_cpu, std::memory_order_relaxed);
    g_cpu_l1.store(mb::cpu_l1_in_stage(cpu_floor, slope_cpu, stage), std::memory_order_relaxed);
    const int level = mb::step_level(g_level_state, raw, room, s.fallback_live != 0, now, l1_clear_room);
    const int previous = g_level.exchange(level, std::memory_order_relaxed);
    if (previous >= 2 && level < 2) level_fell_below_2();
    if (level >= 3) g_l3_seen.store(true, std::memory_order_relaxed);
    if (g_session.load(std::memory_order_relaxed))
    {
        int seen = g_session_max_level.load(std::memory_order_relaxed);
        while (level > seen && !g_session_max_level.compare_exchange_weak(seen, level, std::memory_order_relaxed))
        {
        }
    }
    return level;
}

inline void store_max(std::atomic<u64>& v, u64 x) noexcept
{
    u64 seen = v.load(std::memory_order_relaxed);
    while (x > seen && !v.compare_exchange_weak(seen, x, std::memory_order_relaxed))
    {
    }
}

inline bool budget_disabled() noexcept
{
    static const bool off = []() noexcept
    {
        const bool present = ::access(no_budget_flag, F_OK) == 0;
        if (present) ps4_startup_checkpoint("memory budget: OFF (no-mem-budget.flag): build heap cap, no reservation for the other owners");
        return present;
    }();
    return off;
}

// Runs the shedders for 'level' (budget_tick only). A shedder at or below the level runs when the level reaches it
// and again every shed_repeat_us while it holds; once the level is below it, it is called once with 0 (restore).
inline void run_shedders(int level, u64 now) noexcept
{
    const u32 count = std::min<u32>(g_shedder_count.load(std::memory_order_acquire), max_shedders);
    for (u32 i = 0; i < count; i++)
    {
        shedder_slot& s = g_shedders[i];
        const auto fn = s.fn.load(std::memory_order_acquire);
        if (!fn) continue;
        char line[192];
        if (level >= s.level)
        {
            if (s.active && now - s.last_us < shed_repeat_us) continue;
            s.active = true;
            s.last_us = now;
            const u64 freed = fn(level);
            g_shed_runs.fetch_add(1, std::memory_order_relaxed);
            g_shed_bytes.fetch_add(freed, std::memory_order_relaxed);
            const u32 n = g_shed_lines.fetch_add(1, std::memory_order_relaxed);
            if (n < 32 || !(n & 63))
            {
                if (freed) std::snprintf(line, sizeof(line), "shed: %s freed %llu MiB at level %d", s.name, static_cast<unsigned long long>(freed >> 20), level);
                else std::snprintf(line, sizeof(line), "shed: %s asked to free memory at level %d", s.name, level);
                ps4_startup_checkpoint(line);
            }
        }
        else if (s.active)
        {
            s.active = false;
            fn(0);
            if (g_shed_lines.fetch_add(1, std::memory_order_relaxed) < 32)
            {
                std::snprintf(line, sizeof(line), "shed: %s back to normal (level %d)", s.name, level);
                ps4_startup_checkpoint(line);
            }
        }
    }
}

inline u64 heap_trim_shedder(int level) noexcept
{
    return level ? orbis::mmapDirectTrim(0) : 0;
}
} // namespace detail

// 0.38: memory level 0..3 (governor v2, see the top of this file), as of the last heap_pressure() sample. Any thread.
inline int memory_level() noexcept
{
    return detail::g_level.load(std::memory_order_relaxed);
}

// 0.38: true once level 3 was reached in this process (an allocation could not be served, or room fell below
// 128 MiB): the frontend's clean-process rule. Any thread.
inline bool level3_seen() noexcept
{
    return detail::g_l3_seen.load(std::memory_order_relaxed);
}

// 0.38: memory shedders. 'fn(level)' frees what it can at 'level' (>= its own level) and returns the bytes freed (0 =
// unknown or asynchronous: a request the owner serves at its next safe point); fn(0) restores normal limits once the
// level fell below it. It runs on whichever thread runs the budget tick (any thread that samples the heap): it must be
// thread-safe, allocation-free and quick (set a cap, raise a request flag). 'name' must outlive the process (a
// literal). At most 16; false when full or fn is null. Register before the game starts.
inline bool register_shedder(const char* name, int level, u64 (*fn)(int level) noexcept) noexcept
{
    if (!fn) return false;
    const u32 index = detail::g_shedder_count.fetch_add(1, std::memory_order_acq_rel);
    if (index >= detail::max_shedders) return false;
    detail::shedder_slot& s = detail::g_shedders[index];
    s.name = name ? name : "?";
    s.level = level < 1 ? 1 : level > 3 ? 3 : level;
    s.fn.store(fn, std::memory_order_release);
    return true;
}

// 0.38: source of the JIT figure of the budget (bytes committed for JIT code, data, PPU link arenas and executable
// tables). Unset: utils::ps4::memory private commits less the own-stack commits. A dedicated JIT ledger installs its
// counter here (any thread; allocation-free, lock-free function).
inline void set_jit_source(u64 (*source)() noexcept) noexcept
{
    detail::g_jit_source.store(source, std::memory_order_release);
}

// 0.39: writer of the RPCS3.log copy of the level transitions (levels 0..2, 64 lines per process). Called on the
// sampling thread after the startup.log line; never from the new_handler or the pressure hook.
inline void set_rpcs3_log(void (*writer)(const char*) noexcept) noexcept
{
    detail::g_rpcs3_log.store(writer, std::memory_order_release);
}

// JIT bytes as the budget counts them. Any thread.
inline u64 jit_committed() noexcept
{
    if (const auto source = detail::g_jit_source.load(std::memory_order_acquire)) return source();
    const u64 priv = ::utils::ps4::memory::get_statistics().direct_bytes;
    const u64 stacks = ps4_thread_stack_direct_bytes();
    return priv > stacks ? priv - stacks : 0;
}

// 0.38: test-heap-cap-mib.txt (health): the budget's cap never goes above it.
inline void set_test_heap_cap(u64 bytes) noexcept
{
    detail::g_test_heap_cap.store(bytes, std::memory_order_relaxed);
}

// 0.38: one "budget:" line (allocation-free: snprintf, relaxed loads, lock-free counter reads; safe on a fatal path).
// Owners in MiB: system at boot, heap held/cap (live: small, large; fallback), guest/ceiling, JIT/ceiling,
// stacks/ceiling, GPU and other; pool free (largest run) and reservation; flexible free; GPU window; level, room, slope.
inline void budget_line(char* out, usz size) noexcept
{
    namespace mb = ::utils::ps4::mem_budget;
    orbis::MmapDirectStats st{};
    orbis::mmapDirectStats(&st);
    const auto w = ::utils::ps4::vram::current_window();
    const u64 small = st.small_heap;
    const u64 large = st.live > small ? st.live - small : 0;
    const s64 room = st.cap ? static_cast<s64>(st.cap) - static_cast<s64>(st.live) : 0;
    const auto m = [](u64 v) { return static_cast<unsigned long long>(v >> 20); };
    char window[96];
    if (w.size) std::snprintf(window, sizeof(window), "window %llu/%llu (vma %llu, hole %llu)", m(w.used), m(w.size), m(w.vma), m(w.largest_hole));
    else std::snprintf(window, sizeof(window), "window off");
    std::snprintf(out, size,
        "budget: direct %llu = sys %llu + heap %llu/%llu (live %llu: small %llu, large %llu; fb %llu) + guest %llu/%llu + jit %llu/%llu + stacks %llu/%llu"
        " + gpu+other %llu | free %llu (largest %llu) reserve %llu%s | flex %llu | %s | level %d room %lld slope %lld/min | shed %llu (%llu MiB)"
        " | compiles admitted %llu MiB (%llu waited)%s",
        m(detail::g_total.load(std::memory_order_relaxed)), m(detail::g_system.load(std::memory_order_relaxed)), m(st.held), m(st.cap), m(st.live), m(small), m(large),
        m(st.fallback_live), m(detail::g_guest.load(std::memory_order_relaxed)), m(mb::guest_ceiling), m(detail::g_jit.load(std::memory_order_relaxed)),
        m(mb::jit_ceiling), m(detail::g_stacks.load(std::memory_order_relaxed)), m(mb::stacks_ceiling), m(detail::g_used_gpu.load(std::memory_order_relaxed)),
        m(detail::g_free_total.load(std::memory_order_relaxed)), m(detail::g_largest_run.load(std::memory_order_relaxed)),
        m(detail::g_reserve_set.load(std::memory_order_relaxed)), detail::g_estimated.load(std::memory_order_relaxed) ? " (ledger estimated)" : "",
        m(ps4_flexible_memory_free()), window, memory_level(), static_cast<long long>(room / (s64{1} << 20)),
        static_cast<long long>(detail::g_slope.load(std::memory_order_relaxed) / (s64{1} << 20)),
        static_cast<unsigned long long>(detail::g_shed_runs.load(std::memory_order_relaxed)), m(detail::g_shed_bytes.load(std::memory_order_relaxed)),
        m(static_cast<u64>(std::max<s64>(detail::g_compile_in_flight.load(std::memory_order_relaxed), 0))),
        static_cast<unsigned long long>(detail::g_admission_waits.load(std::memory_order_relaxed)), detail::budget_disabled() ? " | budget OFF (no-mem-budget.flag)" : "");
}

// 0.38: the budget authority, once a second at most (any thread; one at a time): measures the owners, sets the
// reservation and the heap cap in orbis-compat, feeds the estimated ledger, records the running title's peaks and
// runs the shedders of the current level. 'force': now, whatever the interval (stage changes, the XMB, a boot check).
inline void budget_tick(bool force = false) noexcept
{
    namespace mb = ::utils::ps4::mem_budget;
    const u64 now = detail::now_us();
    u64 last = detail::g_budget_tick_us.load(std::memory_order_relaxed);
    if (!force && (now - last < detail::budget_tick_interval_us || !detail::g_budget_tick_us.compare_exchange_strong(last, now, std::memory_order_relaxed)))
    {
        return;
    }
    if (detail::g_budget_busy.exchange(true, std::memory_order_acquire)) return;
    detail::g_budget_tick_us.store(now, std::memory_order_relaxed);
    const u32 tick = detail::g_budget_ticks.fetch_add(1, std::memory_order_relaxed);
    if (!tick)
    {
        orbis::setPressureHook(&detail::pressure_hook);
        register_shedder("heap trim (empty carve-outs back to the pool)", 2, &detail::heap_trim_shedder);
    }

    orbis::MmapDirectStats st{};
    orbis::mmapDirectStats(&st);
    const auto memory = ::utils::ps4::memory::get_statistics();
    const u64 stacks = ps4_thread_stack_direct_bytes();
    const u64 jit = jit_committed();
    const u64 guest = memory.shared_direct_bytes;
    const auto window = ::utils::ps4::vram::current_window();
    const u64 gpu_window = window.size ? window.used : 0;

    orbis::DirectLedger ledger{};
    orbis::directLedger(&ledger, 250);
    if (ledger.estimated)
    {
        orbis::directLedgerSetExternal(mb::external_bytes(guest, jit, stacks, gpu_window));
        orbis::directLedger(&ledger, 0);
    }

    mb::owners o{};
    o.total = ledger.total;
    o.allocated = ledger.allocated;
    o.free_total = ledger.free_total;
    o.largest_run = ledger.largest_run;
    o.system_at_boot = ledger.boot_allocated;
    o.heap_held = st.held;
    o.heap_live = st.live;
    o.build_cap = detail::heap_build_cap;
    o.guest = guest;
    o.jit = jit;
    o.stacks = stacks;
    o.gpu_window = gpu_window;
    o.estimated = ledger.estimated != 0;
    const mb::result r = mb::compute(o);
    u64 cap = r.cap_eff;
    if (const u64 test = detail::g_test_heap_cap.load(std::memory_order_relaxed); test && test < cap) cap = test;

    const bool off = detail::budget_disabled();
    if (!off && ledger.total)
    {
        orbis::mmapDirectSetReserve(r.reserve);
        orbis::mmapDirectSetCap(cap ? cap : 1); // 0 would restore the build cap; 1 stops new carve-outs
    }

    detail::g_total.store(ledger.total, std::memory_order_relaxed);
    detail::g_system.store(ledger.boot_allocated, std::memory_order_relaxed);
    detail::g_allocated.store(ledger.allocated, std::memory_order_relaxed);
    detail::g_free_total.store(ledger.free_total, std::memory_order_relaxed);
    detail::g_largest_run.store(ledger.largest_run, std::memory_order_relaxed);
    detail::g_used_gpu.store(r.used_gpu, std::memory_order_relaxed);
    detail::g_guest.store(guest, std::memory_order_relaxed);
    detail::g_jit.store(jit, std::memory_order_relaxed);
    detail::g_stacks.store(stacks, std::memory_order_relaxed);
    detail::g_estimated.store(o.estimated, std::memory_order_relaxed);
    detail::g_reserve_set.store(off ? 0 : r.reserve, std::memory_order_relaxed);
    detail::g_cap_eff.store(off ? 0 : cap, std::memory_order_relaxed);

    if (detail::g_session.load(std::memory_order_relaxed))
    {
        detail::store_max(detail::g_session_peak_live, st.live);
        detail::store_max(detail::g_session_peak_jit, jit);
        detail::store_max(detail::g_session_peak_guest, guest);
        detail::store_max(detail::g_session_peak_window, gpu_window);
    }

    const int level = memory_level();
    detail::run_shedders(level, now);

    // The first tick, and every level change (the 60 s line comes from health::periodic).
    static std::atomic<int> s_logged_level{-1};
    if (s_logged_level.exchange(level, std::memory_order_relaxed) != level && detail::g_budget_lines.fetch_add(1, std::memory_order_relaxed) < 64)
    {
        char line[768];
        budget_line(line, sizeof(line));
        if (level >= 3) ps4_fatal_note("", line); // durable: the line before a possible fatal
        else ps4_startup_checkpoint(line);
    }
    detail::g_budget_busy.store(false, std::memory_order_release);
}

// 0 / 1 / 2, see the top of this file. Any thread. Samples at most every 50 ms (one thread at a time); other
// calls return the last level. 0.38: g_heap_level holds the v2 level (0..3), returned here as at most 2.
inline int heap_pressure() noexcept
{
    const int cached = detail::g_heap_level.load(std::memory_order_relaxed);
    {
        const u64 now = detail::now_us();
        u64 last = detail::g_heap_sampled_us.load(std::memory_order_relaxed);
        if ((cached >= 0 && now - last < detail::heap_sample_interval_us) ||
            !detail::g_heap_sampled_us.compare_exchange_strong(last, now, std::memory_order_relaxed))
        {
            return cached < 0 ? 0 : cached > 2 ? 2 : cached;
        }
    }

    const heap_sample s = sample_heap();
    // 0.38: the v2 level (0..3) on the budget's cap; callers of this function keep 0..2 (3 reads as 2).
    const int v2 = detail::update_level(s);
    const int level = v2 > 2 ? 2 : v2;
    const int old = detail::g_heap_level.exchange(v2, std::memory_order_relaxed);
    if (old != v2)
    {
        // 0.39: a refusal younger than 120 s is named (it holds level 3 for 30 s, then level 2 for 60 s)
        char refusal[48]{};
        if (const u64 refused_at = detail::g_refusal_us.load(std::memory_order_relaxed))
        {
            const u64 now = detail::now_us();
            if (now >= refused_at && now - refused_at < 120'000'000)
                std::snprintf(refusal, sizeof(refusal), ", last refusal %llu s ago", static_cast<unsigned long long>((now - refused_at) / 1'000'000));
        }
        char text[272];
        std::snprintf(text, sizeof(text), "flexible free %llu MiB, heap cap %llu MiB, live %llu MiB, room %lld MiB, slope %lld MiB/min%s%s",
            static_cast<unsigned long long>(s.flexible_free >> 20), static_cast<unsigned long long>(s.cap >> 20),
            static_cast<unsigned long long>(s.live >> 20), static_cast<long long>((static_cast<s64>(s.cap) - static_cast<s64>(s.live)) / (s64{1} << 20)),
            static_cast<long long>(detail::g_slope.load(std::memory_order_relaxed) / (s64{1} << 20)),
            s.critical ? ", an allocation found no heap room or fell back to flexible memory" : "", refusal);
        detail::log_transition("heap", old, v2, text, detail::level_effect_now(v2), detail::g_lines, v2 >= 2);
    }
    if (v2 >= 3)
    {
        detail::on_critical();
        detail::release_on_fallback(s.fallback_bytes);
    }
    else if (v2 == 2)
    {
        detail::on_low();
    }
    budget_tick();
    return level;
}

// Main loop, every pass: once per second a heap sample (level changes, the save notices) and the budget tick, also
// while nothing compiles. Cheap: one clock read between samples.
inline void poll_heap() noexcept
{
    const u64 now = detail::now_us();
    u64 last = detail::g_heap_polled_us.load(std::memory_order_relaxed);
    if (now - last < detail::heap_poll_interval_us ||
        !detail::g_heap_polled_us.compare_exchange_strong(last, now, std::memory_order_relaxed))
    {
        return;
    }
    static_cast<void>(heap_pressure());
    budget_tick();
}

// True once the heap reached level 3 in this process, or while it is critical or has bytes in flexible memory: the
// frontend advises restarting the application before the next game. Any thread; relaxed loads only.
inline bool restart_advised() noexcept
{
    if (detail::g_restart_advised.load(std::memory_order_relaxed)) return true;
    orbis::MmapDirectStats st{};
    orbis::mmapDirectStats(&st);
    return st.critical != 0 || st.fallback_live != 0;
}

// 0.38: heap room under the budget's cap right now (negative when the other owners took the heap's share), after a
// fresh budget tick. Main thread (the frontend's pre-boot check).
inline s64 room_now() noexcept
{
    budget_tick(true);
    static_cast<void>(heap_pressure());
    const heap_sample s = sample_heap(false);
    return s.cap ? static_cast<s64>(s.cap) - static_cast<s64>(s.live) : (s64{1} << 40);
}

namespace detail
{
// 0.40: this thread is a PPU compile worker of the boot compile stage under the ppu_boot_workers rule
// (boot_compile_scope): its admissions use boot_compile_admit.
inline thread_local bool t_boot_compile = false;
} // namespace detail

// 0.38: compile admission for a working set of 'bytes' (PPU module, SPU program, RSX pipeline), see
// mem_budget::compile_admit. True: admitted, and release_compile(bytes) must follow when the compile ends.
inline bool try_admit_compile(u64 bytes) noexcept
{
    const heap_sample s = sample_heap(false);
    const s64 room = s.cap ? static_cast<s64>(s.cap) - static_cast<s64>(s.live) : (s64{1} << 40);
    const bool boot = detail::t_boot_compile;
    s64 in = detail::g_compile_in_flight.load(std::memory_order_relaxed);
    for (;;)
    {
        const u64 in_flight = static_cast<u64>(in > 0 ? in : 0);
        const int level = memory_level();
        if (!(boot ? boot_compile_admit(room, in_flight, bytes, level) : ::utils::ps4::mem_budget::compile_admit(room, in_flight, bytes, level))) return false;
        if (detail::g_compile_in_flight.compare_exchange_weak(in, in + static_cast<s64>(bytes), std::memory_order_acq_rel)) return true;
    }
}

// 0.40: held by a PPU compile worker of the boot compile stage while the ppu_boot_workers rule applies to its
// workload ('on'): its compile admissions use the boot budget (boot_compile_admit). Nested scopes keep the outer one.
class boot_compile_scope
{
public:
    explicit boot_compile_scope(bool on) noexcept
        : m_prev(detail::t_boot_compile)
    {
        detail::t_boot_compile = on || m_prev;
    }

    ~boot_compile_scope()
    {
        detail::t_boot_compile = m_prev;
    }

    boot_compile_scope(const boot_compile_scope&) = delete;
    boot_compile_scope& operator=(const boot_compile_scope&) = delete;

private:
    bool m_prev;
};

inline void release_compile(u64 bytes) noexcept
{
    detail::g_compile_in_flight.fetch_sub(static_cast<s64>(bytes), std::memory_order_acq_rel);
}

// Same as try_admit_compile (the name the compile owners use).
inline bool admit_compile(u64 bytes) noexcept
{
    return try_admit_compile(bytes);
}

// Working-set bytes of the admitted compiles in flight, and admissions that had to wait.
inline u64 compile_in_flight() noexcept
{
    const s64 in = detail::g_compile_in_flight.load(std::memory_order_relaxed);
    return in > 0 ? static_cast<u64>(in) : 0;
}

inline u64 admission_waits() noexcept
{
    return detail::g_admission_waits.load(std::memory_order_relaxed);
}

// 0.38: blocking compile admission, held for one compile of an estimated working set of 'bytes' (at least 1 MiB is
// counted). Waits (10 ms steps, the level resampled) until try_admit_compile passes: the compiles in flight stay under
// mem_budget::compile_budget and leave room above level 2; at level 2 or 3 only one admitted compile runs at a time.
// The first compile always passes, so work never stops. 'stopped' (noexcept, polled while waiting) returning true
// ends the wait without admission (admitted() false, nothing to release); the caller checks for stop itself. Not
// reentrant (a thread holds at most one). Order with compile_slot: take the admission first, then the slot.
class compile_admission
{
public:
    static constexpr u64 min_bytes = 1ull << 20;

    template <typename Stopped>
    compile_admission(u64 bytes, Stopped&& stopped) noexcept
        : m_bytes(bytes < min_bytes ? min_bytes : bytes)
    {
        while (!try_admit_compile(m_bytes))
        {
            if (stopped()) return;
            if (!m_waited)
            {
                m_waited = true;
                detail::g_admission_waits.fetch_add(1, std::memory_order_relaxed);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            static_cast<void>(heap_pressure());
        }
        m_admitted = true;
    }

    ~compile_admission()
    {
        if (m_admitted) release_compile(m_bytes);
    }

    bool admitted() const noexcept { return m_admitted; }
    bool waited() const noexcept { return m_waited; }

    compile_admission(const compile_admission&) = delete;
    compile_admission& operator=(const compile_admission&) = delete;

private:
    u64 m_bytes = 0;
    bool m_admitted = false;
    bool m_waited = false;
};

// 0.38: optional compile workers beyond the base count (mem_budget::optional_workers_allowed); 'per_worker_peak' 0 =
// the default estimate. With the 0.39 rules "Modo frio" never adds them (its boot compile stage runs on the two base
// slots); with the 0.38 rules it serializes the compile instead.
inline bool optional_workers_allowed(u32 extra, u64 per_worker_peak) noexcept
{
    if (!detail::governor_v038() && ::utils::ps4::cool_mode::enabled()) return false;
    const heap_sample s = sample_heap(false);
    const s64 room = s.cap ? static_cast<s64>(s.cap) - static_cast<s64>(s.live) : (s64{1} << 40);
    return ::utils::ps4::mem_budget::optional_workers_allowed(memory_level(), room, extra, per_worker_peak);
}

// 0.38: the running title's session (main thread): begin after a successful boot, record before the XMB clears the
// title. The record: heap peak above the heap live at its boot, and the peaks of the other owners.
inline void begin_session() noexcept
{
    const heap_sample s = sample_heap(false);
    detail::g_session_boot_live.store(s.live, std::memory_order_relaxed);
    detail::g_session_peak_live.store(s.live, std::memory_order_relaxed);
    detail::g_session_peak_jit.store(0, std::memory_order_relaxed);
    detail::g_session_peak_guest.store(0, std::memory_order_relaxed);
    detail::g_session_peak_window.store(0, std::memory_order_relaxed);
    detail::g_session_max_level.store(memory_level(), std::memory_order_relaxed);
    detail::g_session.store(true, std::memory_order_release);
}

// False when no session ran since the last call. 'max_level': the highest memory level of the session.
inline bool end_session(::utils::ps4::mem_budget::record& out, int& max_level) noexcept
{
    if (!detail::g_session.exchange(false, std::memory_order_acq_rel)) return false;
    budget_tick(true);
    const heap_sample s = sample_heap(false);
    detail::store_max(detail::g_session_peak_live, s.live);
    const u64 boot = detail::g_session_boot_live.load(std::memory_order_relaxed);
    const u64 peak = detail::g_session_peak_live.load(std::memory_order_relaxed);
    out = {};
    out.heap = peak > boot ? peak - boot : 0;
    out.jit = detail::g_session_peak_jit.load(std::memory_order_relaxed);
    out.guest = detail::g_session_peak_guest.load(std::memory_order_relaxed);
    out.window = detail::g_session_peak_window.load(std::memory_order_relaxed);
    out.boot = boot;
    out.valid = true;
    max_level = detail::g_session_max_level.load(std::memory_order_relaxed);
    return true;
}

// Takes the heap reserve and installs its new_handler (see the top of this file). Main thread, after a compile stage
// finished (health::on_stage_change), and (0.38) every time the XMB opens once a released reserve can be taken again
// (rearm_reserve). Skipped (and tried again at the next of those points) while the heap has less than
// reserve_min_room of room.
inline void arm_reserve() noexcept
{
    if (detail::g_reserve_armed.load(std::memory_order_acquire)) return;
    char line[256];
    const heap_sample before = sample_heap(false);
    if (!before.cap || before.critical || heap_room(before) < reserve_min_room)
    {
        if (!detail::g_reserve_skip_logged.exchange(true, std::memory_order_relaxed))
        {
            std::snprintf(line, sizeof(line), "heap reserve: not taken yet (heap cap %llu MiB, room %llu MiB, needs %llu MiB%s); tried again after the next compile stage",
                static_cast<unsigned long long>(before.cap >> 20), static_cast<unsigned long long>(heap_room(before) >> 20),
                static_cast<unsigned long long>(reserve_min_room >> 20), before.critical ? ", heap critical" : "");
            ps4_startup_checkpoint(line);
        }
        return;
    }

    detail::g_reserve_armed.store(true, std::memory_order_release);
    if (std::get_new_handler())
    {
        ps4_startup_checkpoint("heap reserve: not taken (another std::new_handler is installed)");
        return;
    }

    u32 taken = 0;
    for (auto& slot : detail::g_reserve)
    {
        void* const block = std::malloc(reserve_request_bytes);
        if (!block) break;
        slot.store(block, std::memory_order_release);
        taken++;
    }
    detail::g_reserve_left.store(taken, std::memory_order_release);
    if (taken) std::set_new_handler(&detail::reserve_new_handler);

    const heap_sample after = sample_heap(false);
    std::snprintf(line, sizeof(line), "heap reserve: %u x %llu MiB taken (heap room %llu MiB now); released when an allocation fails, when a heap request finds no carve-out room, or when level 3 falls back to flexible memory",
        taken, static_cast<unsigned long long>(reserve_block_bytes >> 20), static_cast<unsigned long long>(heap_room(after) >> 20));
    ps4_startup_checkpoint(line);

    // The blocks needed a carve-out the system could not give (the heap had to fall back): hand them back at once.
    if (after.critical)
    {
        while (detail::release_reserve_block("the heap became critical while the reserve was taken"))
        {
        }
    }
}

// 0.38: the XMB opened again (main thread, no game running): a reserve that was released is taken again, if the heap
// has the room.
inline void rearm_reserve() noexcept
{
    if (detail::g_reserve_armed.load(std::memory_order_acquire) && !detail::g_reserve_left.load(std::memory_order_acquire) &&
        std::get_new_handler() == nullptr)
    {
        detail::g_reserve_armed.store(false, std::memory_order_release);
        detail::g_reserve_skip_logged.store(false, std::memory_order_relaxed);
    }
    arm_reserve();
}

// Reserve blocks still held, and blocks released so far.
inline u32 reserve_left() noexcept
{
    return detail::g_reserve_left.load(std::memory_order_relaxed);
}

inline u32 reserve_released() noexcept
{
    return detail::g_reserve_released.load(std::memory_order_relaxed);
}

inline bool reserve_armed() noexcept
{
    return detail::g_reserve_armed.load(std::memory_order_relaxed);
}

namespace detail
{
inline std::atomic<u32> g_thread_cap{0};          // 0.39: cap in force, 0 = not loaded yet
inline std::atomic<bool> g_thread_cap_learned{false};

// Reads a small text file (at most size - 1 bytes); false when it cannot be opened.
inline bool read_small_file(const char* path, char* out, usz size, usz& got) noexcept
{
    got = 0;
    const int fd = ::open(path, O_RDONLY);
    if (fd < 0) return false;
    while (got + 1 < size)
    {
        const auto n = ::read(fd, out + got, size - 1 - got);
        if (n <= 0) break;
        got += static_cast<usz>(n);
    }
    ::close(fd);
    return true;
}

inline bool write_small_file(const char* path, std::string_view text) noexcept
{
    const int fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) return false;
    usz done = 0;
    while (done < text.size())
    {
        const auto n = ::write(fd, text.data() + done, text.size() - done);
        if (n <= 0) break;
        done += static_cast<usz>(n);
    }
    ::close(fd);
    return done == text.size();
}

// thread-cap.txt with the line of 'firmware' set to 'cap'.
inline bool store_thread_cap(std::string_view firmware, u32 cap) noexcept
{
    char text[2048];
    usz got = 0;
    if (!read_small_file(thread_cap_file, text, sizeof(text), got)) got = 0;
    const std::string updated = with_thread_cap(std::string_view(text, got), firmware, cap);
    return !updated.empty() && write_small_file(thread_cap_file, updated);
}

inline u32 load_thread_cap() noexcept
{
    const char* const firmware = ::ps4_firmware_key();
    char text[2048];
    usz got = 0;
    const u32 from_file = firmware[0] && read_small_file(thread_cap_file, text, sizeof(text), got) ? parse_thread_cap(std::string_view(text, got), firmware) : 0;
    const u32 cap = from_file ? from_file : thread_cap_default;
    char line[256];
    std::snprintf(line, sizeof(line), "memory governor: thread cap %u (%s) for firmware %s; optional threads leave the last %u slots free; 0.39 thread levels at %u/%u of live + exited-not-joined threads",
        cap, from_file ? "thread-cap.txt" : "default", firmware[0] ? firmware : "unknown", thread_l2_margin, cap - thread_l1_margin, cap - thread_l2_margin);
    log_line(line, g_thread_lines, true);
    return cap;
}

constexpr const char* thread_effect_v039(int from, int to) noexcept
{
    if (to >= 2) return "optional threads refused or run inline, compiles one per class";
    if (to == 1) return from >= 2 ? "optional threads allowed again, no optional workers" : "no optional workers";
    return "back to normal";
}
} // namespace detail

// 0.39: the thread cap in force (thread-cap.txt line of this firmware, else 80). Any thread.
inline u32 thread_cap() noexcept
{
    if (const u32 cap = detail::g_thread_cap.load(std::memory_order_relaxed)) return cap;
    static const u32 loaded = detail::load_thread_cap();
    u32 expected = 0;
    detail::g_thread_cap.compare_exchange_strong(expected, loaded, std::memory_order_relaxed);
    return detail::g_thread_cap.load(std::memory_order_relaxed);
}

// 0.39 (contract with the frontend's thread-limit probe): stores 'cap' for 'firmware' (its first word) in
// thread-cap.txt and applies it at once when it is this console's firmware. Main thread; false when nothing was
// written.
inline bool set_thread_cap(u32 cap, std::string_view firmware) noexcept
{
    const std::string_view key = firmware_key(firmware);
    if (!cap || key.empty()) return false;
    cap = clamp_thread_cap(cap);
    const bool stored = detail::store_thread_cap(key, cap);
    const char* const own = ::ps4_firmware_key();
    const bool applied = !own[0] || key == own;
    if (applied)
    {
        static_cast<void>(thread_cap());
        detail::g_thread_cap.store(cap, std::memory_order_relaxed);
    }
    char line[224];
    std::snprintf(line, sizeof(line), "memory governor: thread cap %u for firmware %.*s %s thread-cap.txt%s", cap, static_cast<int>(std::min<usz>(key.size(), 32)), key.data(),
        stored ? "written to" : "NOT written to", applied ? "; applied now" : "");
    detail::log_line(line, detail::g_thread_lines, true);
    return stored;
}

// 0.39: the first refused thread create of the process (Utilities/Thread.cpp ps4_thread_first_refusal, not test
// refusals), 'load' taken at the refusal: when fewer threads were alive than the cap says, that count is the cap for
// the rest of this process. Not written to thread-cap.txt: a refusal for another reason (flexible memory for stacks)
// would lower the levels of every later session; only the thread-limit probe writes that file. Once per process.
inline void note_thread_refusal(const ps4_thread_load& load) noexcept
{
    if (detail::governor_v038() || detail::g_thread_cap_learned.exchange(true, std::memory_order_relaxed)) return;
    const u32 current = thread_cap();
    const char* const firmware = ::ps4_firmware_key();
    char line[256];
    if (load.total < thread_cap_min || load.total >= current)
    {
        std::snprintf(line, sizeof(line), "memory governor: first refused thread create at %u threads (live %u + exited not joined %u); cap %u kept",
            load.total, load.live, load.unjoined, current);
    }
    else
    {
        detail::g_thread_cap.store(load.total, std::memory_order_relaxed);
        std::snprintf(line, sizeof(line), "memory governor: thread cap %u learned from the first refused create (live %u + exited not joined %u) for firmware %s, this process only (thread-cap.txt unchanged); levels at %u/%u",
            load.total, load.live, load.unjoined, firmware[0] ? firmware : "unknown", load.total - thread_l1_margin, load.total - thread_l2_margin);
    }
    detail::log_line(line, detail::g_thread_lines, true);
}

// 0 / 1 / 2: the 0.38 rules (the default): from the number of live RPCS3 threads (thread registry) at 56/64; the 0.39
// rules: from the threads the console's cap counts (see thread_cap). Any thread; a few relaxed loads.
inline int thread_pressure() noexcept
{
    if (detail::governor_v038())
    {
        const u32 live = ::ps4_thread_registry::get_counters().live;
        const int level = classify_threads(live);
        const int old = detail::g_thread_level.exchange(level, std::memory_order_relaxed);
        if (old != level)
        {
            char text[96];
            std::snprintf(text, sizeof(text), "live RPCS3 threads %u, levels at %u/%u", live, thread_warn, thread_high);
            detail::log_transition("thread", old, level, text, detail::thread_effect(old, level), detail::g_thread_lines);
            // Who holds the slots, printed by the frontend's next 1 s pass
            if (old >= 0 || level != 0) ::ps4_census_request();
        }
        return level;
    }

    if (ps4_thread_load at{}; !detail::g_thread_cap_learned.load(std::memory_order_relaxed) && ::ps4_thread_first_refusal(at))
    {
        note_thread_refusal(at);
    }

    const ps4_thread_load load = ::ps4_thread_load_now();
    const u32 cap = thread_cap();
    const int level = classify_thread_load(load.total, cap);
    const int old = detail::g_thread_level.exchange(level, std::memory_order_relaxed);
    if (old != level)
    {
        char text[160];
        std::snprintf(text, sizeof(text), "threads %u (orbis live %u + exited not joined %u) of cap %u, levels at %u/%u", load.total, load.live, load.unjoined,
            cap, clamp_thread_cap(cap) - thread_l1_margin, clamp_thread_cap(cap) - thread_l2_margin);
        detail::log_transition("thread", old, level, text, detail::thread_effect_v039(old, level), detail::g_thread_lines);
        if (old >= 0 || level != 0) ::ps4_census_request();
    }
    return level;
}

namespace detail
{
// The retry of refused guest creates never recovered a thread on the console and held the creating guest thread up to
// 1.1 s per refusal, so it is off by default (CELL_EAGAIN at once, as in 0.38); the thread_retry row
// (thread-retry.flag or features.txt; thread-no-retry.flag still wins) turns it on. Read per call; the rule in force
// is written to startup.log at its first use and whenever it changes.
inline bool thread_retry_disabled() noexcept
{
    static std::atomic<int> s_logged{-1};
    const bool off = !::utils::ps4::safe_mode::enabled(::utils::ps4::safe_mode::feature::thread_retry);
    const int value = off ? 1 : 0;
    if (s_logged.load(std::memory_order_relaxed) != value && s_logged.exchange(value, std::memory_order_relaxed) != value)
    {
        ps4_startup_checkpoint(off ? "PS4 threads: a refused guest thread create fails at once (0.38 default; thread-retry.flag: retried)"
                                   : "PS4 threads: a refused guest thread create is retried (up to 5 tries in 5 s, waiting up to 50 ms for a slot; thread-retry.flag or features.txt)");
    }
    return off;
}
} // namespace detail

// 0.39 (opt-in in 0.40, see thread_retry_disabled): a guest thread create the console refused gets more tries. Call
// after the 'tries'-th refused create, started at 'since_us' (steady clock, detail::now_us): waits up to 50 ms (5 ms
// steps) for a thread slot to come back (a join, or fewer threads than now) and returns true when the caller should
// create again. false after 'max_tries' creates or 5 s, while the retry is off, or once 'stopped()' is true. Never call
// it while holding a lock that other creates or joins need (idm, an SPU group mutex).
template <typename Stopped>
inline bool retry_refused_create(u32 tries, u32 max_tries, u64 since_us, Stopped&& stopped) noexcept
{
    constexpr u64 window_us = 5'000'000;
    if (tries >= max_tries || detail::thread_retry_disabled()) return false;
    const ps4_thread_load before = ::ps4_thread_load_now();
    for (u32 waited_ms = 0; waited_ms < 50; waited_ms += 5)
    {
        if (stopped() || detail::now_us() - since_us >= window_us) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        const ps4_thread_load now = ::ps4_thread_load_now();
        if (now.joined != before.joined || now.total < before.total) break;
    }
    return !stopped() && detail::now_us() - since_us < window_us;
}

// 0.40: an HLE media create (is_hle_media_thread) the console refused while the guest create retry is off: waits in
// media_retry_step_ms sleeps, at most media_retry_ceiling_ms after 'since_us' (steady clock, detail::now_us), until
// fewer threads than 'load_at_refusal' (ps4_thread_load_now().total taken right after the refusal) count against the
// console's cap, and returns the ms waited since 'since_us'; the caller then creates once more. Ends early once
// 'stopped()' is true (the caller checks for stop itself before creating). Holds no lock; never call it while holding
// one that other creates or joins need (idm, an SPU group mutex).
template <typename Stop>
inline u32 media_retry_pace(u64 since_us, u32 load_at_refusal, Stop&& stopped) noexcept
{
    return media_retry_pace_with(since_us, load_at_refusal, []() noexcept { return detail::now_us(); },
        []() noexcept { return ::ps4_thread_load_now().total; },
        [](u32 ms) noexcept { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }, stopped);
}

// 0.39: rate limit of the per-name refusal lines: the first 4 of a name, then every power of two. Returns the count
// of this name, 0 when the line is not due. Any thread, lock-free.
inline u32 refusal_line_due(std::string_view name) noexcept
{
    struct entry
    {
        std::atomic<u64> hash{0};
        std::atomic<u32> count{0};
    };
    static entry s_names[64];
    static std::atomic<u32> s_other{0};

    u64 hash = 1469598103934665603ull;
    for (const char ch : name)
    {
        hash = (hash ^ static_cast<u8>(ch)) * 1099511628211ull;
    }
    hash |= 1;

    std::atomic<u32>* counter = &s_other;
    for (entry& e : s_names)
    {
        u64 seen = e.hash.load(std::memory_order_acquire);
        if (!seen && !e.hash.compare_exchange_strong(seen, hash, std::memory_order_acq_rel) && seen != hash) continue;
        if (seen && seen != hash) continue;
        counter = &e.count;
        break;
    }
    const u32 n = counter->fetch_add(1, std::memory_order_relaxed) + 1;
    return n <= 4 || !(n & (n - 1)) ? n : 0;
}

namespace detail
{
inline std::atomic<u32> g_optional_refusal_cap{0};      // 0.40: the count at the first refused create taken as a cap
inline std::atomic<bool> g_optional_refusal_seen{false};
} // namespace detail

// 0.40: the cap optional threads count against (optional_cap): thread_cap() until the console refuses a create at
// fewer threads, then that count for the rest of this process, with either governor rule set. A guard: it changes
// nothing before a refusal, and a refusal for another reason only keeps optional threads off sooner. One startup.log
// line when it takes effect under the 0.38 rules (the 0.39 rules write their own learned-cap line). Any thread.
inline u32 optional_thread_cap() noexcept
{
    const u32 cap = thread_cap();
    if (!detail::g_optional_refusal_seen.load(std::memory_order_acquire))
    {
        ps4_thread_load at{};
        if (!::ps4_thread_first_refusal(at)) return cap;
        // The cap first, then 'seen': a reader that sees 'seen' also sees the cap
        if (u32 none = 0; optional_cap(cap, at.total) != cap &&
            detail::g_optional_refusal_cap.compare_exchange_strong(none, at.total, std::memory_order_acq_rel) && detail::governor_v038())
        {
            char line[224];
            std::snprintf(line, sizeof(line), "PS4 threads: first refused create at %u threads (live %u + exited not joined %u, cap %u): optional threads keep the last %u slots below %u for the game, this process only",
                at.total, at.live, at.unjoined, clamp_thread_cap(cap), thread_l2_margin, at.total);
            detail::log_line(line, detail::g_thread_lines, true);
        }
        detail::g_optional_refusal_seen.store(true, std::memory_order_release);
    }
    return optional_cap(cap, detail::g_optional_refusal_cap.load(std::memory_order_acquire));
}

// 0.40: call before starting an optional emulator thread (online client, request workers, launcher helpers, a lazily
// started worker) with the number of threads it adds. False while those threads would take one of the last
// thread_l2_margin slots of the console's cap (see optional_allowed; the cap is optional_thread_cap); the caller then
// takes its offline, inline or later path. Independent of the governor rules in force. One startup.log line per name
// and process. Any thread, allocation-free. optional-threads-free.flag turns the rule off (always true).
inline bool optional_thread_allowed(std::string_view name, u32 n = 1) noexcept
{
    if (!::utils::ps4::safe_mode::enabled(::utils::ps4::safe_mode::feature::optional_thread_rule)) return true;
    const ps4_thread_load load = ::ps4_thread_load_now();
    const u32 cap = optional_thread_cap();
    if (optional_decision(true, load.total, n, cap)) return true;

    // Own key space, so the refusal lines of real creates with the same name keep their own count
    char key[96] = "optional:";
    usz len = 9;
    for (const char ch : name)
    {
        if (len + 1 >= sizeof(key)) break;
        key[len++] = ch;
    }
    if (refusal_line_due(std::string_view(key, len)) == 1)
    {
        char line[256];
        std::snprintf(line, sizeof(line), "PS4 threads: optional '%.*s' not started: thread budget (%u of cap %u; the last %u slots are kept for the game)",
            static_cast<int>(name.size() > 64 ? 64 : name.size()), name.data(), load.total, clamp_thread_cap(cap), thread_l2_margin);
        detail::log_line(line, detail::g_thread_lines, true);
    }
    return false;
}

// 0.40: optional_thread_allowed for a group of 'wanted' threads: how many of them may start now (optional_allowed_count;
// all of them with the rule off). When not all may, the refusal is logged as optional_thread_allowed logs it.
inline u32 optional_threads_allowed(std::string_view name, u32 wanted) noexcept
{
    if (!wanted || !::utils::ps4::safe_mode::enabled(::utils::ps4::safe_mode::feature::optional_thread_rule)) return wanted;
    const u32 n = optional_allowed_count(true, ::ps4_thread_load_now().total, wanted, optional_thread_cap());
    if (n < wanted)
    {
        static_cast<void>(optional_thread_allowed(name, n + 1));
    }
    return n;
}

namespace detail
{
inline bool cool_serializes() noexcept
{
    if (!::utils::ps4::cool_mode::enabled()) return false;
    if (!g_cool_logged.exchange(true, std::memory_order_relaxed)) ps4_startup_checkpoint("cool mode: compiles serialized");
    return true;
}

// cool-mode-serial-boot.flag, read once (at the first "Modo frio" compile stage).
inline bool cool_serial_boot() noexcept
{
    static const bool on = []() noexcept
    {
        const bool present = ::access(cool_serial_boot_flag, F_OK) == 0;
        if (present) ps4_startup_checkpoint("cool mode: compile stages serialized too (cool-mode-serial-boot.flag)");
        return present;
    }();
    return on;
}

// Slot mode of the rules in force. 0.38 rules (the default): one shared slot whenever compiles serialize. 0.39 rules:
// "Modo frio" keeps one compile in the whole process during play; a boot compile stage (PPU modules, SPU cache: no game
// code runs yet) gets two, unless memory or thread pressure asks for less or cool-mode-serial-boot.flag is present.
inline ::utils::ps4::mem_budget::slot_mode slot_mode_now() noexcept
{
    namespace mb = ::utils::ps4::mem_budget;
    const int heap = heap_pressure();
    if (governor_v038())
    {
        // The 0.38 order: the thread level and "Modo frio" are looked at only while the heap level is 0
        const bool threads_high = heap == 0 && thread_pressure() >= 2;
        const bool cool = heap == 0 && !threads_high && cool_serializes();
        return mb::slot_mode_v038(heap, threads_high, cool);
    }
    const bool cool = ::utils::ps4::cool_mode::enabled();
    const bool cool_stage = cool && compile_stage_active() && !cool_serial_boot();
    const mb::slot_mode mode = mb::slot_mode_v039(heap, cpu_l1_now(), thread_pressure() >= 2, cool, cool_stage);
    if (mode == mb::slot_mode::two_in_total)
    {
        if (!g_cool_stage_logged.exchange(true, std::memory_order_relaxed))
        {
            ps4_startup_checkpoint("cool mode: compile stage (PPU modules, SPU cache) runs on at most 2 compile threads, no optional workers; compiles during play stay serialized");
        }
    }
    else if (mode == mb::slot_mode::one_in_total && cool && heap < 2)
    {
        // One in total because of "Modo frio" (level 2 and above serialize on their own)
        static_cast<void>(cool_serializes());
    }
    return mode;
}
} // namespace detail

// True while a second concurrent PPU/SPU compile should wait: memory or thread pressure, or "Modo frio" (fewer busy
// cores). 0.39: at memory level 1 only below 384 MiB of room or on a sustained slope. The two slots of a "Modo frio"
// compile stage keep the base workers (two compile threads), like parallel.
inline bool serialize_compiles() noexcept
{
    namespace mb = ::utils::ps4::mem_budget;
    const mb::slot_mode mode = detail::slot_mode_now();
    return mode == mb::slot_mode::one_per_class || mode == mb::slot_mode::one_in_total;
}

// What serialize_compiles() last looked at, for the lines that say why a build runs on fewer workers:
// "memory level L, cpu_l1 X, thread level T, stage S, cool mode C, 0.38|0.39 rules". Reads the stored levels (no new
// sample, no log line). Any thread, allocation-free.
inline void serialize_reason(char* out, usz size) noexcept
{
    if (!out || !size) return;
    const bool v038 = detail::governor_v038();
    const int thread_level = detail::g_thread_level.load(std::memory_order_relaxed);
    std::snprintf(out, size, "memory level %d, cpu_l1 %d, thread level %d, stage %d, cool mode %d, %s rules", memory_level(),
        detail::cpu_l1_now() ? 1 : 0, thread_level < 0 ? 0 : thread_level, detail::compile_stage_active() ? 1 : 0,
        ::utils::ps4::cool_mode::enabled() ? 1 : 0, v038 ? "0.38" : "0.39");
}

// 0.40: the causes behind serialize_compiles() now (format_serialize_reason), e.g. "thread level 2, 74 of 80", for the
// lines that say why a build runs on one thread. Relaxed reads of the stored levels and the thread counters and a heap
// sample without the flexible-memory query: no level change, no log line. The view points into a buffer of the calling
// thread and stays valid until that thread calls again. Any thread, allocation-free.
inline std::string_view serialize_reason() noexcept
{
    static thread_local char t_text[192];
    serialize_state st{};
    st.v038 = detail::governor_v038();
    st.memory_level = memory_level();
    const heap_sample s = sample_heap(false);
    st.room_known = s.cap != 0;
    st.room = s.cap ? static_cast<s64>(s.cap) - static_cast<s64>(s.live) : 0;
    st.cpu_l1 = detail::cpu_l1_now();
    if (st.v038)
    {
        st.threads = ::ps4_thread_registry::get_counters().live;
    }
    else
    {
        st.threads = ::ps4_thread_load_now().total;
        st.cap = thread_cap();
    }
    st.cool = ::utils::ps4::cool_mode::enabled();
    st.cool_stage = st.cool && detail::compile_stage_active() && !detail::cool_serial_boot();
    return std::string_view(t_text, format_serialize_reason(t_text, sizeof(t_text), st));
}

// Live RPCS3 threads (thread registry), without touching the thread level.
inline u32 live_threads() noexcept
{
    return ::ps4_thread_registry::get_counters().live;
}

// Normal runtime SPU compiler recycle limit: spu_recycle_programs, or the value in spu_recycle_file (0 = never).
// Read once per process; a file value is written to startup.log.
inline u32 spu_recycle_programs_setting() noexcept
{
    static const u32 limit = []() noexcept -> u32
    {
        u64 value = 0;
        if (!detail::read_decimal_file(spu_recycle_file, spu_recycle_programs_max, value)) return spu_recycle_programs;
        char line[192];
        if (value)
        {
            std::snprintf(line, sizeof(line), "memory governor: SPU LLVM compilers recycled after %llu program(s) (%u below %llu MiB heap room), from %s",
                static_cast<unsigned long long>(value), value < spu_recycle_programs_short ? static_cast<u32>(value) : spu_recycle_programs_short,
                static_cast<unsigned long long>(spu_recycle_room >> 20), spu_recycle_file);
        }
        else
        {
            std::snprintf(line, sizeof(line), "memory governor: SPU LLVM compiler recycling off (0 in %s)", spu_recycle_file);
        }
        detail::log_line(line);
        return static_cast<u32>(value);
    }();
    return limit;
}

// no-extra-spu-workers.flag or "Modo frio": the boot SPU cache build keeps upstream's worker count.
inline bool extra_spu_workers_disabled() noexcept
{
    return ::utils::ps4::cool_mode::enabled() || ::access(no_extra_spu_workers_flag, F_OK) == 0;
}

inline u32 active_compiles() noexcept
{
    return ::utils::ps4::mem_budget::slots_total(detail::g_active.load(std::memory_order_relaxed));
}

// 0.39: compile slots held by one class.
inline u32 active_compiles(slot_kind kind) noexcept
{
    return ::utils::ps4::mem_budget::slots_of(detail::g_active.load(std::memory_order_relaxed), kind);
}

inline u64 compile_waits() noexcept
{
    return detail::g_waits.load(std::memory_order_relaxed);
}

// 0.38: compile slots that waited at memory level 2 or above.
inline u64 level_waits() noexcept
{
    return detail::g_level_waits.load(std::memory_order_relaxed);
}

// Held for the duration of one compile. The first worker always gets through; others wait (10 ms steps) while
// serialize_compiles() is true (and a third one during a "Modo frio" compile stage). 'stopped' is polled while waiting and must return true once the emulator or the
// worker is stopping, so a waiting worker can never block shutdown; it must not throw (the constructor is
// noexcept). A waiter that sees 'stopped' still takes the slot, so the caller checks for stop itself after the
// constructor and before compiling. Not reentrant: a thread that holds a slot must
// not take a second one. Do not take a slot on a thread that the other compile workers depend on (e.g. an inline
// compile on the RSX or SPU thread).
// 0.39: 'kind' is the slot class (the default cpu is the 0.38 behaviour for PPU/SPU callers); see the top of this file.
class compile_slot
{
public:
    static constexpr u64 level_wait_us = 2'000'000;

    template <typename Stopped>
    explicit compile_slot(Stopped&& stopped, slot_kind kind = slot_kind::cpu) noexcept
        : m_unit(::utils::ps4::mem_budget::slot_unit(kind))
    {
        namespace mb = ::utils::ps4::mem_budget;
        bool waited = false;
        // 0.38: at memory level 2 or above every compile, the first one included, waits for the level to fall (at
        // most level_wait_us, so a boot never stops for good), then goes on serialized below. 0.39: PPU/SPU only.
        if (mb::slot_waits_for_level(kind, memory_level(), detail::governor_v038()))
        {
            const u64 until = detail::now_us() + level_wait_us;
            detail::g_level_waits.fetch_add(1, std::memory_order_relaxed);
            while (!stopped() && detail::now_us() < until)
            {
                static_cast<void>(heap_pressure());
                if (memory_level() < 2) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
        for (;;)
        {
            u32 n = detail::g_active.load(std::memory_order_relaxed);
            if (mb::slots_total(n) == 0 || mb::slot_admits(detail::slot_mode_now(), n, kind))
            {
                if (detail::g_active.compare_exchange_weak(n, n + m_unit, std::memory_order_acq_rel)) return;
                continue;
            }
            if (stopped())
            {
                detail::g_active.fetch_add(m_unit, std::memory_order_acq_rel);
                return;
            }
            if (!waited)
            {
                waited = true;
                detail::g_waits.fetch_add(1, std::memory_order_relaxed);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }

    ~compile_slot()
    {
        detail::g_active.fetch_sub(m_unit, std::memory_order_acq_rel);
    }

    compile_slot(const compile_slot&) = delete;
    compile_slot& operator=(const compile_slot&) = delete;

private:
    u32 m_unit;
};

// 0.39: held while a boot compile stage runs (PPU modules, SPU cache build): the slope of the heap room is not a
// reason for level 1 then (the compile working set returns when the stage ends).
class compile_stage
{
public:
    compile_stage() noexcept
    {
        detail::g_compile_stage.fetch_add(1, std::memory_order_relaxed);
    }

    ~compile_stage()
    {
        detail::g_compile_stage.fetch_sub(1, std::memory_order_relaxed);
    }

    compile_stage(const compile_stage&) = delete;
    compile_stage& operator=(const compile_stage&) = delete;
};
#else
inline u64 test_cap_bytes() noexcept { return 0; }
inline heap_sample sample_heap(bool = true) noexcept { return {}; }
inline int heap_pressure() noexcept { return 0; }
inline void poll_heap() noexcept {}
inline bool restart_advised() noexcept { return false; }
inline void arm_reserve() noexcept {}
inline u32 reserve_left() noexcept { return 0; }
inline u32 reserve_released() noexcept { return 0; }
inline bool reserve_armed() noexcept { return false; }
inline int thread_pressure() noexcept { return 0; }
inline u32 optional_thread_cap() noexcept { return thread_cap_default; }
inline bool optional_thread_allowed(std::string_view, u32 = 1) noexcept { return true; }
inline u32 optional_threads_allowed(std::string_view, u32 wanted) noexcept { return wanted; }
inline bool serialize_compiles() noexcept { return false; }
inline void serialize_reason(char* out, usz size) noexcept { if (out && size) out[0] = '\0'; }
inline std::string_view serialize_reason() noexcept { return {}; }
template <typename Stop>
inline u32 media_retry_pace(u64, u32, Stop&&) noexcept { return 0; }
inline u32 live_threads() noexcept { return 0; }
inline u32 spu_recycle_programs_setting() noexcept { return 0; }
inline bool extra_spu_workers_disabled() noexcept { return true; }
inline u32 active_compiles() noexcept { return 0; }
inline u32 active_compiles(slot_kind) noexcept { return 0; }
inline u64 compile_waits() noexcept { return 0; }
inline u64 level_waits() noexcept { return 0; }
inline int memory_level() noexcept { return 0; }
inline bool level3_seen() noexcept { return false; }
inline bool register_shedder(const char*, int, u64 (*)(int) noexcept) noexcept { return false; }
inline void set_jit_source(u64 (*)() noexcept) noexcept {}
inline void set_rpcs3_log(void (*)(const char*) noexcept) noexcept {}
inline u64 jit_committed() noexcept { return 0; }
inline void set_test_heap_cap(u64) noexcept {}
inline void budget_line(char* out, usz size) noexcept { if (out && size) out[0] = '\0'; }
inline void budget_tick(bool = false) noexcept {}
inline s64 room_now() noexcept { return s64{1} << 40; }
inline bool try_admit_compile(u64) noexcept { return true; }
inline void release_compile(u64) noexcept {}
inline bool admit_compile(u64) noexcept { return true; }
inline u64 compile_in_flight() noexcept { return 0; }
inline u64 admission_waits() noexcept { return 0; }

class compile_admission
{
public:
    template <typename Stopped>
    compile_admission(u64, Stopped&&) noexcept {}

    bool admitted() const noexcept { return true; }
    bool waited() const noexcept { return false; }

    compile_admission(const compile_admission&) = delete;
    compile_admission& operator=(const compile_admission&) = delete;
};
inline bool optional_workers_allowed(u32, u64) noexcept { return true; }

class boot_compile_scope
{
public:
    explicit boot_compile_scope(bool) noexcept {}

    boot_compile_scope(const boot_compile_scope&) = delete;
    boot_compile_scope& operator=(const boot_compile_scope&) = delete;
};
inline void begin_session() noexcept {}
inline bool end_session(::utils::ps4::mem_budget::record&, int&) noexcept { return false; }
inline void rearm_reserve() noexcept {}

class compile_slot
{
public:
    template <typename Stopped>
    explicit compile_slot(Stopped&&, slot_kind = slot_kind::cpu) noexcept {}

    compile_slot(const compile_slot&) = delete;
    compile_slot& operator=(const compile_slot&) = delete;
};

class compile_stage
{
public:
    compile_stage() noexcept {}

    compile_stage(const compile_stage&) = delete;
    compile_stage& operator=(const compile_stage&) = delete;
};
#endif
} // namespace utils::ps4::governor
