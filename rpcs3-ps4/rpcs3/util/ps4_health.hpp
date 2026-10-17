#pragma once

// 0.31: process health hooks for the PS4 frontend (header-only). ps4_main.cpp calls them at fixed points; each one
// only logs, notifies or trims memory, never stops the emulator, and silently does nothing when a query fails.
//
//   boot_checks()        once, after the RPCS3 log is ready: /data free space (statvfs; toast below 2 GiB),
//                        RLIMIT_NOFILE raised to its maximum, memory census "boot", first temperature reading.
//   periodic(now)        from the 60 s report: one "health:" line (heap with its musl small-heap share and the heap
//                        reserve, flexible memory, governor, disk, temperature, process CPU time), the VRAM line when
//                        the RSX thread published one, a memory census every 15 minutes (every 5 under heap
//                        pressure; orbis-compat lines are capped per hour).
//   on_stage_change()    when the boot progress text empties after the PPU/SPU compile stages: returns fully free
//                        heap carve-outs (orbis-compat built with ORBIS_MMAP_TRIM=1; otherwise a no-op), logs, and
//                        takes the governor's heap reserve once per process (governor::arm_reserve).
//   on_return_to_xmb()   every time the XMB opens again after a game (main thread). Clears the adapt title, so a
//                        failure while the XMB is shown is not recorded against the game that just ended; trims.
//                        0.38: first writes the game's memory record (adapt/<TITLE>.mem: heap it needed, JIT, guest
//                        and GPU window peaks), takes a spent heap reserve again and logs the "budget:" line.
//   0.38: boot_checks, periodic and on_stage_change also run the memory budget authority and log its "budget:" line
//   (governor::budget_line).
//   on_stall(...)        from the stall watchdog: one toast and one fatal note after 30 s, never while compiling.
//   trim_caches_before_boot(title)
//                        0.33: before a game boots (no game running): removes whole per-title cache folders, oldest
//                        first, while /data has less than 4 GiB free or /data/rpcs3/cache/ is above its budget.
//   remove_title_cache(title)
//                        0.33: removes one title's cache folder (menu action; no game running).
//
// Disk: below 2 GiB free on /data one toast; below 1 GiB cache_writes_allowed() turns false (back to true above
// 1.25 GiB). Code that writes optional caches checks it and skips the write; the PPU object cache is not optional
// (modules are linked from it), the SPU program cache is (SPUCommonRecompiler.cpp spu_cache::add).
// The numbers come from ::statvfs. Up to 0.32 the SDK libc.a statvfs() returned 0 without filling the struct, so every
// session logged "statvfs looks wrong" and these checks stayed off. 0.33 links orbis-compat's override, which translates
// the kernel's statfs; the sanity check below stays, so a missing override or a translation that does not match this
// kernel still turns the checks off instead of acting on wrong numbers.
//
// Cache budget (0.33): per-title caches live in /data/rpcs3/cache/cache/<TITLE ID>/ (PPU objects, SPU and shader
// caches). Only folders named like a title id are ever removed; 'mesa', 'xmb', 'vsh' (firmware modules shared by every
// game) and the title about to boot never are. The size limit is 8 GiB for all of /data/rpcs3/cache/, or the decimal
// GiB value in /data/rpcs3/cache-budget-gib.txt; it is not applied when the kept caches alone are above it (removing
// the others could not reach it). /data/rpcs3/no-cache-trim.flag turns the whole trim off. A removed cache costs one
// recompile the next time that game boots. Links are removed as names and never followed (lstat is stat on this SDK,
// so only the directory entry type tells them apart).
//
// Console safety monitor: read_thermal() only reads the CPU and SoC temperature sensors (sceKernelGetCpuTemperature,
// sceKernelGetSocSensorTemperature; both exported by libkernel but not declared in the OpenOrbis headers, so they
// are bound weakly). Nothing here changes fan, clock, power or firmware settings. thermal_state() samples at most
// once per second and is cheap enough for every main-loop pass; at the first warn level it shows one toast.
// pause_requested() tells the frontend to pause emulation while the state is critical (it shows the message).
// /data/rpcs3/no-thermal.flag turns the sensor reads off; /data/rpcs3/no-thermal-pause.flag keeps the readings and
// the warning but never requests a pause.
// 0.34: on every console measured so far both sensors return 0x80020002 (ENOENT: not visible to a sandboxed app), so
// no reading ever existed and the pause could never trigger. Each read is now classified (ok, ENOENT, EPERM, missing,
// implausible value, other error). While no sensor has ever answered they are read at boot, 60 s and 300 s later,
// then marked unavailable and never read again; startup.log says so once, and the health line reads "temperature
// unavailable (ENOENT)" until then (0.36: no temperature part after). The PS4 firmware's own fan control, warning and
// shutdown are what protect the console. A
// sensor that does answer is filtered (median of 3, jump rejection) before the warn/critical state machine.
//
// Test flag: /data/rpcs3/test-heap-cap-mib.txt (decimal MiB) lowers the real orbis-compat heap cap at boot, for the
// forced out-of-memory test (the governor's own test-governor-cap-mib.txt only changes what the governor sees).

#include "util/types.hpp"
#include "util/ps4_adapt.hpp"

#include <algorithm>
#include <atomic>
#include <string>
#include <string_view>
#include <vector>

#ifdef RPCS3_PS4
#include "util/ps4_gpu_health.hpp"
#include "util/ps4_i18n.hpp"
#include "util/ps4_memory_governor.hpp"
#include "Utilities/File.h"
#include "Utilities/StrFmt.h"

#include <orbis_mem.h>
#include <orbis_mmap.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fcntl.h>
#include <sys/resource.h> // getrusage (libkernel export; libc.a's getrusage.lo defines nothing)
#include <sys/statvfs.h>
#include <unistd.h>
#include <utility>

// Frontend objects (ps4_fatal.cpp), always linked; same declarations as ps4_fatal.h.
void ps4_notify(const char* message) noexcept;
void ps4_fatal_note(const char* prefix, const char* detail) noexcept;

extern "C"
{
// libkernel exports without an OpenOrbis prototype. Read-only queries; weak, so a missing export reads as "no
// sensor". The sensor index of the SoC query is 0.
int ps4_health_cpu_temperature(int* celsius) __asm__("sceKernelGetCpuTemperature") __attribute__((weak));
int ps4_health_soc_temperature(int sensor, int* celsius) __asm__("sceKernelGetSocSensorTemperature") __attribute__((weak));
// libkernel's rlimit calls take FreeBSD resource numbers (RLIMIT_NOFILE is 8), not the SDK musl header's (7), so
// they are bound by name here instead of through <sys/resource.h>. Layout: {cur, max}, 64-bit each.
int ps4_health_getrlimit(int resource, unsigned long long* cur_max) __asm__("getrlimit") __attribute__((weak));
int ps4_health_setrlimit(int resource, const unsigned long long* cur_max) __asm__("setrlimit") __attribute__((weak));
int ps4_health_getdtablesize() __asm__("getdtablesize") __attribute__((weak));
}
#endif

namespace utils::ps4::health
{
inline constexpr u64 stall_notify_us = 30'000'000;        // on_stall: toast after this long without progress
inline constexpr u64 low_disk_bytes = 2048ull << 20;      // boot_checks: toast below this much free on /data
inline constexpr u64 cache_stop_bytes = 1024ull << 20;    // cache_writes_allowed() false below this...
inline constexpr u64 cache_resume_bytes = 1280ull << 20;  // ...and true again from this
// Carve-outs never trimmed. 0.38: 0 (0.37: 2, the first heap growth): with the small heap in carve-outs of its own,
// the first slots are not special, and a small-heap carve-out never empties while musl holds a step of it.
inline constexpr unsigned trim_keep = 0;

inline constexpr int thermal_min_c = 10;                  // readings outside [min, max] are discarded
inline constexpr int thermal_max_c = 120;
inline constexpr int thermal_warn_c = 85;
inline constexpr int thermal_critical_c = 92;
inline constexpr int thermal_clear_c = 80;                // warn and critical end only below this
inline constexpr u64 thermal_sustain_us = 10'000'000;     // critical needs this long at or above thermal_critical_c
inline constexpr u64 thermal_poll_us = 1'000'000;

inline constexpr char no_thermal_flag[] = "/data/rpcs3/no-thermal.flag";
inline constexpr char no_thermal_pause_flag[] = "/data/rpcs3/no-thermal-pause.flag";
inline constexpr char test_heap_cap_file[] = "/data/rpcs3/test-heap-cap-mib.txt";

inline constexpr u64 cache_free_target_bytes = 4096ull << 20; // trim while /data has less than this free...
inline constexpr u64 cache_budget_default_gib = 8;            // ...or /data/rpcs3/cache/ holds more than this
inline constexpr u64 cache_budget_max_gib = 4096;             // larger values in the budget file are clamped
inline constexpr u32 cache_walk_max_entries = 200'000;        // the size walk stops here (bounded boot delay)
inline constexpr u32 cache_walk_max_depth = 16;
inline constexpr char cache_root_dir[] = "/data/rpcs3/cache/";
inline constexpr char title_cache_dir[] = "/data/rpcs3/cache/cache/"; // rpcs3::utils::get_cache_dir()
inline constexpr char cache_budget_file[] = "/data/rpcs3/cache-budget-gib.txt";
inline constexpr char no_cache_trim_flag[] = "/data/rpcs3/no-cache-trim.flag";

// Per-title cache folder names are PS3 title ids: four upper-case letters and five digits (BLUS30268, NPUA80918).
// Anything else under cache/cache/ (vsh, iso_cache, playlists) is shared and never removed.
constexpr bool is_title_cache_name(std::string_view name) noexcept
{
    if (name.size() != 9) return false;
    for (usz i = 0; i < 4; i++)
        if (name[i] < 'A' || name[i] > 'Z') return false;
    for (usz i = 4; i < 9; i++)
        if (name[i] < '0' || name[i] > '9') return false;
    return true;
}

struct title_cache
{
    std::string name;
    u64 bytes = 0;  // logical size of every file in the folder
    s64 newest = 0; // newest mtime seen in the folder (seconds), i.e. when the game last wrote to its cache
};

// Removal order (indexes into titles), oldest last write first. 'keep' (the title about to boot) never appears; when
// it is empty or not a title id, the most recently written folder is kept instead.
inline std::vector<usz> cache_trim_order(const std::vector<title_cache>& titles, std::string_view keep)
{
    const bool keep_newest = !is_title_cache_name(keep);
    usz newest = titles.size();
    for (usz i = 0; keep_newest && i < titles.size(); i++)
    {
        if (is_title_cache_name(titles[i].name) && (newest == titles.size() || titles[i].newest > titles[newest].newest)) newest = i;
    }

    std::vector<usz> order;
    for (usz i = 0; i < titles.size(); i++)
    {
        if (!is_title_cache_name(titles[i].name) || i == newest || titles[i].name == keep) continue;
        order.push_back(i);
    }

    std::stable_sort(order.begin(), order.end(), [&](usz a, usz b) { return titles[a].newest < titles[b].newest; });
    return order;
}

// One step of the trim loop. kept: bytes that the trim can never remove (shared caches, the kept title). The size limit
// applies only when removing the other titles can reach it; free_bytes 0 means the free space is unknown.
constexpr bool cache_trim_needed(u64 total, u64 kept, u64 budget, u64 free_bytes, u64 free_target) noexcept
{
    return (kept <= budget && total > budget) || (free_bytes && free_bytes < free_target);
}

enum thermal_level : int
{
    thermal_ok = 0,
    thermal_warn = 1,
    thermal_critical = 2,
};

struct thermal
{
    int cpu_c = 0;      // CPU temperature in Celsius, 0 when unavailable or implausible
    int soc_c = 0;      // SoC sensor temperature in Celsius, 0 when unavailable or implausible
    bool valid = false; // at least one plausible reading
};

constexpr bool plausible_celsius(int c) noexcept
{
    return c >= thermal_min_c && c <= thermal_max_c;
}

// Turns two raw sensor values into a thermal sample (implausible values become 0).
constexpr thermal make_thermal(int cpu_c, int soc_c) noexcept
{
    thermal t{};
    t.cpu_c = plausible_celsius(cpu_c) ? cpu_c : 0;
    t.soc_c = plausible_celsius(soc_c) ? soc_c : 0;
    t.valid = t.cpu_c != 0 || t.soc_c != 0;
    return t;
}

// Pure thermal state machine (host-testable). Uses the hotter of the two readings:
// - warn at thermal_warn_c or above;
// - critical once readings stay at thermal_critical_c or above for thermal_sustain_us without a cooler sample;
// - warn and critical end only when the reading drops below thermal_clear_c, so a pause does not flap on and off
//   and the warning is not repeated while the temperature hovers around thermal_warn_c.
// An invalid sample changes nothing.
struct thermal_tracker
{
    int state = thermal_ok;
    u64 hot_since = 0; // time of the first critical-level sample of the current hot run
    bool hot = false;

    constexpr int update(const thermal& t, u64 now_us) noexcept
    {
        if (!t.valid) return state;
        const int hottest = t.cpu_c > t.soc_c ? t.cpu_c : t.soc_c;
        if (hottest >= thermal_critical_c)
        {
            if (!hot)
            {
                hot = true;
                hot_since = now_us;
            }
            if (now_us - hot_since >= thermal_sustain_us) state = thermal_critical;
            else if (state == thermal_ok) state = thermal_warn;
            return state;
        }
        hot = false;
        hot_since = 0;
        if (hottest < thermal_clear_c) state = thermal_ok;
        else if (state == thermal_ok && hottest >= thermal_warn_c) state = thermal_warn;
        return state;
    }
};

constexpr const char* thermal_name(int level) noexcept
{
    return level == thermal_critical ? "critical" : level == thermal_warn ? "warn" : "ok";
}

// 0.34: what one sensor call returned. The SCE codes are 0x80020000 | errno: ENOENT means the sensor is not visible
// to this (sandboxed) app, EPERM that the call was refused.
enum sensor_status : int
{
    sensor_ok = 0,
    sensor_enoent,      // 0x80020002
    sensor_eperm,       // 0x80020001
    sensor_missing,     // the export is not bound
    sensor_implausible, // rc 0, but the value is outside thermal_min_c..thermal_max_c
    sensor_error,       // any other return code
};

constexpr int classify_sensor(bool bound, int rc, int value) noexcept
{
    if (!bound) return sensor_missing;
    if (rc == 0) return plausible_celsius(value) ? sensor_ok : sensor_implausible;
    if (static_cast<unsigned>(rc) == 0x80020002u) return sensor_enoent;
    if (static_cast<unsigned>(rc) == 0x80020001u) return sensor_eperm;
    return sensor_error;
}

constexpr const char* sensor_status_name(int status) noexcept
{
    switch (status)
    {
    case sensor_ok: return "ok";
    case sensor_enoent: return "ENOENT";
    case sensor_eperm: return "EPERM";
    case sensor_missing: return "missing";
    case sensor_implausible: return "implausible value";
    default: return "error";
    }
}

// 0.34: while no sensor has ever answered, the sensors are read at boot and again this long after the first read;
// after the last retry they are marked unavailable and never read again in this process.
inline constexpr u64 thermal_retry_after_us[] = {60'000'000, 300'000'000};
inline constexpr u32 thermal_tries = 1 + sizeof(thermal_retry_after_us) / sizeof(thermal_retry_after_us[0]);
inline constexpr int thermal_jump_c = 15;           // a sample this far from the median is held back...
inline constexpr unsigned thermal_jump_confirm = 3; // ...unless it repeats this many times in a row

// Per-sensor filter for working sensors: median of the last 3 accepted samples (with 2 samples, the lower one), so
// one stray sample never crosses a threshold; a sample more than thermal_jump_c away from the median is held back
// until thermal_jump_confirm such samples arrive in a row, which then restart the window at the new level.
struct sensor_filter
{
    int samples[3]{};
    unsigned count = 0; // accepted samples in the window (at most 3)
    unsigned next = 0;  // slot of the next accepted sample
    unsigned held_back = 0;

    constexpr int median() const noexcept
    {
        if (count == 0) return 0;
        if (count == 1) return samples[0];
        if (count == 2) return samples[0] < samples[1] ? samples[0] : samples[1];
        const int a = samples[0], b = samples[1], c = samples[2];
        if ((a <= b && b <= c) || (c <= b && b <= a)) return b;
        if ((b <= a && a <= c) || (c <= a && a <= b)) return a;
        return c;
    }

    // Adds a plausible sample and returns the filtered value.
    constexpr int push(int celsius) noexcept
    {
        if (count)
        {
            const int m = median();
            const int distance = celsius > m ? celsius - m : m - celsius;
            if (distance > thermal_jump_c)
            {
                if (++held_back < thermal_jump_confirm) return m;
                count = 0;
                next = 0;
            }
        }
        held_back = 0;
        samples[next] = celsius;
        next = (next + 1) % 3;
        if (count < 3) count++;
        return median();
    }
};

// 0.36: when the GNM keepalive thread (ps4_main.cpp) calls sceGnmSubmitDone. Pure and host-testable: one step per
// 100 ms pass with the process time, orbis-drm's submission sequence and sceGnmAreSubmitsAllowed().
// - Nothing while no GPU submission was handed out yet (sequence 0: nothing to suspend).
// - While submits are not allowed (0: a suspend is pending), at most every gnm_pending_interval_us. A pending suspend
//   freezes the process within ~9 s, so that state lasting gnm_pending_give_up_us is something else and turns this
//   trigger off for good (the idle trigger stays).
// - Otherwise once no new submission has been handed out for gnm_idle_after_us, then again when the sequence moved or
//   gnm_idle_repeat_us after the previous call.
inline constexpr u64 gnm_idle_after_us = 500'000;
inline constexpr u64 gnm_idle_repeat_us = 1'000'000;
inline constexpr u64 gnm_pending_interval_us = 250'000;
inline constexpr u64 gnm_pending_give_up_us = 30'000'000;

enum class gnm_call : int
{
    none,
    idle,
    pending,
};

struct gnm_keepalive_policy
{
    u64 seen_seq = 0;
    u64 seq_changed_at = 0;   // process time the sequence last moved (the thread start before the first submission)
    u64 done_seq = 0;         // sequence at the last call
    u64 last_done_at = 0;     // process time the last call returned
    u64 zero_since = 0;       // process time submits were first seen not allowed in the current run
    bool called_once = false;
    bool zero = false;        // submits not allowed on the previous pass
    bool pending_off = false; // the not-allowed trigger gave up
    bool gave_up_now = false; // this step turned it off

    constexpr explicit gnm_keepalive_policy(u64 started_at = 0) noexcept
        : seq_changed_at(started_at)
    {
    }

    constexpr gnm_call step(u64 now, u64 seq, s32 allowed) noexcept
    {
        gave_up_now = false;
        if (seq != seen_seq)
        {
            seen_seq = seq;
            seq_changed_at = now;
        }
        if (!seq) return gnm_call::none;
        if (allowed == 0 && !pending_off)
        {
            if (!zero)
            {
                zero = true;
                zero_since = now;
            }
            if (now - zero_since >= gnm_pending_give_up_us)
            {
                pending_off = true;
                gave_up_now = true;
            }
            else if (!called_once || now - last_done_at >= gnm_pending_interval_us)
            {
                return gnm_call::pending;
            }
        }
        else if (allowed != 0)
        {
            zero = false;
        }
        if (now - seq_changed_at >= gnm_idle_after_us && (seq != done_seq || !called_once || now - last_done_at >= gnm_idle_repeat_us))
            return gnm_call::idle;
        return gnm_call::none;
    }

    // After the call returned
    constexpr void called(u64 seq, u64 returned_at) noexcept
    {
        done_seq = seq;
        last_done_at = returned_at;
        called_once = true;
    }
};

#ifdef RPCS3_PS4
namespace detail
{
inline u64 now_us() noexcept
{
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

inline void log(const char* text) noexcept
{
    ps4_startup_checkpoint(text);
}

inline bool flag_present(const char* path) noexcept
{
    return ::access(path, F_OK) == 0;
}

// Decimal value from a small text file (the file name gives the unit); 0 when absent or not a positive number.
inline u64 read_mib_file(const char* path) noexcept
{
    const int fd = ::open(path, O_RDONLY);
    if (fd < 0) return 0;
    char text[32]{};
    const auto n = ::read(fd, text, sizeof(text) - 1);
    ::close(fd);
    long i = 0;
    while (i < n && (text[i] == ' ' || text[i] == '\t' || text[i] == '\r' || text[i] == '\n')) i++;
    u64 mib = 0;
    for (; i < n && text[i] >= '0' && text[i] <= '9' && mib < (1ull << 30); i++) mib = mib * 10 + static_cast<u64>(text[i] - '0');
    return mib;
}

// ---------------------------------------------------------------- disk

struct disk_state
{
    std::atomic<u64> free_bytes{0};  // last plausible reading, 0 = unknown
    std::atomic<u64> total_bytes{0};
    std::atomic<u64> logged_free{0}; // value printed by the last "/data free" line
    std::atomic<bool> low_notified{false};
    std::atomic<bool> blocked_notified{false};
    std::atomic<bool> failure_logged{false};
};

inline disk_state g_disk;
inline std::atomic<bool> g_cache_writes_blocked{false};

inline void update_disk(bool boot) noexcept
{
    char line[256];
    struct ::statvfs st{};
    if (::statvfs("/data/rpcs3/", &st) != 0)
    {
        const int error = errno;
        if (!g_disk.failure_logged.exchange(true))
        {
            std::snprintf(line, sizeof(line), "health: /data free space unknown (statvfs failed, errno %d); disk checks off", error);
            log(line);
        }
        return;
    }

    const u64 unit = st.f_frsize ? st.f_frsize : st.f_bsize;
    const u64 blocks = static_cast<u64>(st.f_blocks);
    const u64 total = unit * blocks;
    const u64 avail = unit * static_cast<u64>(st.f_bavail);

    // statvfs reaches the kernel through a translation of FreeBSD's struct statfs (orbis-compat; before 0.33 the SDK
    // libc left the struct untouched); do not act on numbers that cannot be right.
    if (!unit || (unit & (unit - 1)) || blocks > ~0ull / unit || total < (1ull << 30) || avail > total)
    {
        if (!g_disk.failure_logged.exchange(true))
        {
            std::snprintf(line, sizeof(line), "health: /data statvfs looks wrong (bsize %lu, frsize %lu, blocks %llu, bavail %llu); disk checks off",
                static_cast<unsigned long>(st.f_bsize), static_cast<unsigned long>(st.f_frsize),
                static_cast<unsigned long long>(st.f_blocks), static_cast<unsigned long long>(st.f_bavail));
            log(line);
        }
        return;
    }

    g_disk.free_bytes.store(avail, std::memory_order_relaxed);
    g_disk.total_bytes.store(total, std::memory_order_relaxed);

    const u64 logged = g_disk.logged_free.load(std::memory_order_relaxed);
    const u64 moved = logged > avail ? logged - avail : avail - logged;
    if (boot || !logged || moved >= (256ull << 20))
    {
        std::snprintf(line, sizeof(line), "health: /data free %llu MiB of %llu MiB", static_cast<unsigned long long>(avail >> 20),
            static_cast<unsigned long long>(total >> 20));
        log(line);
        g_disk.logged_free.store(avail, std::memory_order_relaxed);
    }

    if (avail < low_disk_bytes)
    {
        if (!g_disk.low_notified.exchange(true))
        {
            i18n::trs(line, i18n::str::HEALTH_RPCS3_POUCO_ESPACO_LIVRE_HDD, {static_cast<unsigned long long>(avail >> 20)});
            ps4_notify(line);
        }
    }
    else if (avail >= low_disk_bytes + (512ull << 20))
    {
        g_disk.low_notified.store(false, std::memory_order_relaxed);
    }

    if (avail < cache_stop_bytes)
    {
        if (!g_cache_writes_blocked.exchange(true))
        {
            std::snprintf(line, sizeof(line), "health: /data free %llu MiB (below %llu MiB): new optional cache writes paused",
                static_cast<unsigned long long>(avail >> 20), static_cast<unsigned long long>(cache_stop_bytes >> 20));
            log(line);
            if (!g_disk.blocked_notified.exchange(true))
            {
                ps4_notify(i18n::tr(i18n::str::HEALTH_RPCS3_HDD_QUASE_CHEIO_NOVOS));
            }
        }
    }
    else if (avail >= cache_resume_bytes && g_cache_writes_blocked.exchange(false))
    {
        std::snprintf(line, sizeof(line), "health: /data free %llu MiB: optional cache writes resumed", static_cast<unsigned long long>(avail >> 20));
        log(line);
    }
}

// ---------------------------------------------------------------- cache budget

// Rewritten at every boot of a title, so "last written" means "last booted" even for a game that only reads its cache.
inline constexpr char last_boot_marker[] = ".rpcs3-ps4-last-boot";

struct tree_size
{
    u64 bytes = 0;
    s64 newest = 0;
    u32 entries = 0;
};

struct tree_walk
{
    u32 budget = cache_walk_max_entries; // entries the walk may still visit
    bool complete = true;                // false: an entry limit, the depth limit or an unreadable directory was hit
};

// Sums the files below 'path' (ends with '/'). Links are counted as entries but never followed. With 'titles', the
// title-id folders directly below title_cache_dir are also measured one by one (and still added to 'out').
inline void measure_tree(const std::string& path, u32 depth, tree_size& out, tree_walk& walk, std::vector<title_cache>* titles) noexcept
{
    fs::dir dir;
    if (!dir.open(path))
    {
        walk.complete = false;
        return;
    }

    fs::dir_entry entry;
    while (dir.read(entry))
    {
        if (entry.name == "." || entry.name == "..") continue;
        if (!walk.budget)
        {
            walk.complete = false;
            return;
        }

        walk.budget--;
        out.entries++;
        out.newest = std::max(out.newest, entry.mtime);
        if (entry.is_symlink) continue;
        if (!entry.is_directory)
        {
            out.bytes += entry.size;
            continue;
        }

        if (depth >= cache_walk_max_depth)
        {
            walk.complete = false;
            continue;
        }

        const std::string child = path + entry.name + '/';
        if (titles && path == title_cache_dir && is_title_cache_name(entry.name))
        {
            tree_size own{};
            measure_tree(child, depth + 1, own, walk, nullptr);
            titles->push_back({entry.name, own.bytes, own.newest});
            out.bytes += own.bytes;
            out.entries += own.entries;
            out.newest = std::max(out.newest, own.newest);
            continue;
        }

        measure_tree(child, depth + 1, out, walk, titles);
    }
}

// Removes 'path' (no trailing '/') and everything below it, bottom-up. A link is unlinked as a name and never followed.
// The names are read first, so no directory is modified while it is being listed.
inline bool remove_tree(const std::string& path, u32 depth) noexcept
{
    std::vector<std::pair<std::string, bool>> children; // name, real directory
    {
        fs::dir dir;
        if (!dir.open(path)) return false;
        fs::dir_entry entry;
        while (dir.read(entry))
        {
            if (entry.name == "." || entry.name == "..") continue;
            children.emplace_back(entry.name, entry.is_directory && !entry.is_symlink);
        }
    }

    for (const auto& [name, is_dir] : children)
    {
        const std::string child = path + '/' + name;
        if (is_dir ? depth >= cache_walk_max_depth || !remove_tree(child, depth + 1) : !fs::remove_file(child)) return false;
    }

    return fs::remove_dir(path);
}

// True when title_cache_dir/<name> is a real directory, not a link to one (a link may lead outside /data/rpcs3).
inline bool title_folder_present(std::string_view name) noexcept
{
    fs::dir dir;
    if (!dir.open(title_cache_dir)) return false;
    fs::dir_entry entry;
    while (dir.read(entry))
    {
        if (entry.name == name) return entry.is_directory && !entry.is_symlink;
    }
    return false;
}

inline void note_title_boot(std::string_view title) noexcept
{
    if (!is_title_cache_name(title) || !title_folder_present(title)) return; // the game creates the folder when it first caches
    const std::string path = std::string(title_cache_dir) + std::string(title) + '/' + last_boot_marker;
    if (fs::file marker{path, fs::rewrite})
    {
        char text[32];
        const int n = std::snprintf(text, sizeof(text), "%lld\n", static_cast<long long>(std::time(nullptr)));
        if (n > 0) marker.write(text, static_cast<u64>(n));
    }
}

inline u64 cache_budget_bytes() noexcept
{
    u64 gib = read_mib_file(cache_budget_file);
    if (!gib) gib = cache_budget_default_gib;
    return std::min(gib, cache_budget_max_gib) << 30;
}

inline void trim_caches(std::string_view keep) noexcept
{
    char line[384];
    if (flag_present(no_cache_trim_flag))
    {
        log("cache: no-cache-trim.flag present; no cache is removed");
        return;
    }

    note_title_boot(keep);
    update_disk(false);
    u64 free_bytes = g_disk.free_bytes.load(std::memory_order_relaxed); // 0: unknown, only the size limit applies
    const u64 budget = cache_budget_bytes();

    const u64 started = now_us();
    std::vector<title_cache> titles;
    tree_size all{};
    tree_walk walk{};
    measure_tree(cache_root_dir, 0, all, walk, &titles);
    const u64 walk_ms = (now_us() - started) / 1000;

    const std::vector<usz> order = cache_trim_order(titles, keep);
    u64 removable = 0;
    for (const usz i : order) removable += titles[i].bytes;
    const u64 kept = all.bytes > removable ? all.bytes - removable : 0;
    u64 total = all.bytes;

    char free_text[48];
    if (free_bytes) std::snprintf(free_text, sizeof(free_text), "%llu MiB", static_cast<unsigned long long>(free_bytes >> 20));
    else std::snprintf(free_text, sizeof(free_text), "unknown");
    const std::string keep_text = is_title_cache_name(keep) ? std::string(keep) : std::string("no title id (newest cache kept)");
    std::snprintf(line, sizeof(line), "cache: %s holds %llu MiB, %zu title folder(s), %llu MiB removable for this boot (%s); limit %llu MiB, /data free %s; "
        "%u entries read in %llu ms%s",
        cache_root_dir, static_cast<unsigned long long>(total >> 20), titles.size(), static_cast<unsigned long long>(removable >> 20), keep_text.c_str(),
        static_cast<unsigned long long>(budget >> 20), free_text, all.entries, static_cast<unsigned long long>(walk_ms),
        walk.complete ? "" : " (walk incomplete: entry or depth limit, or an unreadable folder)");
    log(line);

    if (!cache_trim_needed(total, kept, budget, free_bytes, cache_free_target_bytes)) return;
    if (kept > budget)
    {
        std::snprintf(line, sizeof(line), "cache: the caches that are always kept already hold %llu MiB, above the %llu MiB limit; only low free space on /data removes caches",
            static_cast<unsigned long long>(kept >> 20), static_cast<unsigned long long>(budget >> 20));
        log(line);
    }

    const s64 now = static_cast<s64>(std::time(nullptr));
    u32 removed = 0;
    u64 freed = 0;
    for (const usz i : order)
    {
        if (!cache_trim_needed(total, kept, budget, free_bytes, cache_free_target_bytes)) break;
        const title_cache& t = titles[i];
        char age[48];
        if (t.newest > 0 && now >= t.newest)
            std::snprintf(age, sizeof(age), "last used %lld day(s) ago", static_cast<long long>((now - t.newest) / 86400));
        else
            std::snprintf(age, sizeof(age), "last use unknown");
        if (!remove_tree(std::string(title_cache_dir) + t.name, 0))
        {
            std::snprintf(line, sizeof(line), "cache: could not remove all of %s (%s); the rest is left as it is", t.name.c_str(),
                fmt::format("%s", fs::g_tls_error).c_str());
            log(line);
            continue;
        }

        removed++;
        freed += t.bytes;
        total -= std::min(total, t.bytes);
        if (free_bytes) free_bytes += t.bytes;
        std::snprintf(line, sizeof(line), "cache: removed %s (%llu MiB; %s)", t.name.c_str(), static_cast<unsigned long long>(t.bytes >> 20), age);
        log(line);
    }

    update_disk(false);
    const bool still = cache_trim_needed(total, kept, budget, g_disk.free_bytes.load(std::memory_order_relaxed), cache_free_target_bytes);
    std::snprintf(line, sizeof(line), "cache: %u title cache(s) removed, %llu MiB freed; cache now %llu MiB%s", removed,
        static_cast<unsigned long long>(freed >> 20), static_cast<unsigned long long>(total >> 20),
        still ? "; still over the limit or low on space (nothing else may be removed)" : "");
    log(line);

    if (removed)
    {
        i18n::trs(line, i18n::str::HEALTH_RPCS3_CACHE_JOGO_ANTIGO_APAGADO, {removed, static_cast<unsigned long long>(freed >> 20)});
        ps4_notify(line);
    }
}

// ---------------------------------------------------------------- open files

inline void raise_fd_limit() noexcept
{
    constexpr int rlimit_nofile = 8; // FreeBSD numbering
    char line[256];
    if (!ps4_health_getrlimit)
    {
        log("health: getrlimit unavailable; open-file limit unchanged");
        return;
    }

    unsigned long long lim[2]{};
    if (ps4_health_getrlimit(rlimit_nofile, lim) != 0)
    {
        std::snprintf(line, sizeof(line), "health: getrlimit(NOFILE) failed (errno %d); open-file limit unchanged", errno);
        log(line);
        return;
    }

    const unsigned long long before = lim[0];
    int set_error = 0;
    if (lim[0] < lim[1] && ps4_health_setrlimit)
    {
        // Within the hard limit: process-local, nothing persists.
        const unsigned long long want[2] = {lim[1], lim[1]};
        if (ps4_health_setrlimit(rlimit_nofile, want) != 0) set_error = errno ? errno : -1;
        if (ps4_health_getrlimit(rlimit_nofile, lim) != 0) lim[0] = before;
    }

    const int table = ps4_health_getdtablesize ? ps4_health_getdtablesize() : -1;
    if (set_error)
        std::snprintf(line, sizeof(line), "health: open-file limit %llu (max %llu, was %llu, setrlimit failed errno %d); descriptor table %d", lim[0], lim[1],
            before, set_error, table);
    else
        std::snprintf(line, sizeof(line), "health: open-file limit %llu (max %llu, was %llu); descriptor table %d", lim[0], lim[1], before, table);
    log(line);
}

inline void apply_test_heap_cap() noexcept
{
    const u64 mib = read_mib_file(test_heap_cap_file);
    if (!mib) return;
    char line[160];
    std::snprintf(line, sizeof(line), "health: TEST heap cap %llu MiB from %s", static_cast<unsigned long long>(mib), test_heap_cap_file);
    log(line);
    ::orbis::mmapDirectSetCap(mib << 20);
    // 0.38: the budget authority moves the cap every second; it never goes above this one.
    governor::set_test_heap_cap(mib << 20);
}

// 0.38: the "budget:" line (governor::budget_line) into startup.log.
inline void log_budget() noexcept
{
    char line[768];
    governor::budget_line(line, sizeof(line));
    log(line);
}

// ---------------------------------------------------------------- temperature

struct thermal_monitor
{
    std::atomic<bool> busy{false};
    std::atomic<int> level{thermal_ok};
    std::atomic<int> cpu_c{0};
    std::atomic<int> soc_c{0};
    std::atomic<bool> valid{false};
    std::atomic<bool> raw_logged{false};
    // 0.34: status of the last read of each sensor (-1 = not read yet) and the final verdict when no sensor ever
    // answered: no more reads in this process.
    std::atomic<int> cpu_status{-1};
    std::atomic<int> soc_status{-1};
    std::atomic<int> cpu_rc{-1};
    std::atomic<int> soc_rc{-1};
    std::atomic<bool> ever_ok{false};
    std::atomic<bool> unavailable{false};
    // Owned by the thread that holds 'busy':
    bool polled = false;
    u64 last_poll = 0;
    bool warned = false;
    thermal_tracker tracker{};
    u32 failed_tries = 0; // reads without any working sensor, while none has ever worked
    u64 first_try = 0;
    sensor_filter cpu_filter{};
    sensor_filter soc_filter{};
};

// Raw result of one read of both sensors.
struct sensor_read
{
    int cpu_rc = -1;
    int soc_rc = -1;
    int cpu = 0;
    int soc = 0;
    int cpu_status = sensor_missing;
    int soc_status = sensor_missing;
};

// "ENOENT" when both sensors agree, else "cpu ENOENT, soc EPERM"; an error carries its return code.
inline void sensor_status_text(const sensor_read& r, char* out, usz size) noexcept
{
    const auto put = [](char* buffer, usz n, int status, int rc) noexcept
    {
        if (status == sensor_error) std::snprintf(buffer, n, "error 0x%x", static_cast<unsigned>(rc));
        else std::snprintf(buffer, n, "%s", sensor_status_name(status));
    };
    char cpu[40];
    char soc[40];
    put(cpu, sizeof(cpu), r.cpu_status, r.cpu_rc);
    put(soc, sizeof(soc), r.soc_status, r.soc_rc);
    if (r.cpu_status == r.soc_status && (r.cpu_status != sensor_error || r.cpu_rc == r.soc_rc)) std::snprintf(out, size, "%s", cpu);
    else std::snprintf(out, size, "cpu %s, soc %s", cpu, soc);
}

inline thermal_monitor g_thermal;

inline bool thermal_disabled() noexcept
{
    static const bool disabled = []() noexcept
    {
        const bool off = flag_present(no_thermal_flag);
        if (off) log("health: no-thermal.flag present; temperature sensors not read");
        return off;
    }();
    return disabled;
}

inline bool thermal_pause_disabled() noexcept
{
    static const bool disabled = []() noexcept
    {
        const bool off = flag_present(no_thermal_pause_flag);
        if (off) log("health: no-thermal-pause.flag present; a critical temperature is reported but never pauses the game");
        return off;
    }();
    return disabled;
}

inline void thermal_transition(int from, int to, const thermal& t) noexcept
{
    const int hottest = t.cpu_c > t.soc_c ? t.cpu_c : t.soc_c;
    char line[256];
    std::snprintf(line, sizeof(line), "health: temperature %s -> %s (cpu %d C, soc %d C; warn %d, critical %d for %llu s, clear below %d)",
        thermal_name(from), thermal_name(to), t.cpu_c, t.soc_c, thermal_warn_c, thermal_critical_c,
        static_cast<unsigned long long>(thermal_sustain_us / 1'000'000), thermal_clear_c);
    log(line);

    if (to == thermal_ok)
    {
        g_thermal.warned = false;
        return;
    }

    if (!g_thermal.warned)
    {
        g_thermal.warned = true;
        i18n::trs(line, i18n::str::HEALTH_RPCS3_CONSOLE_ESTA_QUENTE_C, {hottest, thermal_critical_c});
        ps4_notify(line);
    }

    if (to == thermal_critical)
    {
        std::snprintf(line, sizeof(line), "%d C for %llu s (cpu %d, soc %d)%s", hottest, static_cast<unsigned long long>(thermal_sustain_us / 1'000'000),
            t.cpu_c, t.soc_c, thermal_pause_disabled() ? "; pause off by no-thermal-pause.flag" : "; emulation pause requested");
        ps4_fatal_note("THERMAL: console too hot: ", line);
        if (thermal_pause_disabled())
        {
            ps4_notify(i18n::tr(i18n::str::HEALTH_RPCS3_CONSOLE_MUITO_QUENTE_PAUSA));
        }
    }
}

// One read-only read of both sensors, classified. The first call logs the raw return codes and values.
inline sensor_read read_sensors() noexcept
{
    // The prototypes of both exports are not in any SDK header. Say so before the very first call, so a session
    // that ends right here points at the opt-out flag.
    const bool first = !g_thermal.raw_logged.exchange(true);
    if (first) ps4_fatal_note("health: ", "reading the temperature sensors (read-only); if the app stops right here, create /data/rpcs3/no-thermal.flag");

    // Each call writes into its own zeroed 16-byte buffer and only the first int is used, so an export that
    // stores a wider value than the assumed int cannot overwrite the neighbouring stack.
    alignas(8) int cpu_out[4]{};
    alignas(8) int soc_out[4]{};
    sensor_read r{};
    if (ps4_health_cpu_temperature) r.cpu_rc = ps4_health_cpu_temperature(cpu_out);
    if (ps4_health_soc_temperature) r.soc_rc = ps4_health_soc_temperature(0, soc_out);
    r.cpu = cpu_out[0];
    r.soc = soc_out[0];
    r.cpu_status = classify_sensor(ps4_health_cpu_temperature != nullptr, r.cpu_rc, r.cpu);
    r.soc_status = classify_sensor(ps4_health_soc_temperature != nullptr, r.soc_rc, r.soc);
    g_thermal.cpu_rc.store(r.cpu_rc, std::memory_order_relaxed);
    g_thermal.soc_rc.store(r.soc_rc, std::memory_order_relaxed);
    g_thermal.cpu_status.store(r.cpu_status, std::memory_order_relaxed);
    g_thermal.soc_status.store(r.soc_status, std::memory_order_relaxed);

    if (first)
    {
        char line[256];
        std::snprintf(line, sizeof(line), "health: temperature sensors: cpu %s rc 0x%x value %d, soc %s rc 0x%x value %d (used only in %d..%d C)",
            ps4_health_cpu_temperature ? "bound" : "missing", static_cast<unsigned>(r.cpu_rc), r.cpu,
            ps4_health_soc_temperature ? "bound" : "missing", static_cast<unsigned>(r.soc_rc), r.soc, thermal_min_c, thermal_max_c);
        log(line);
    }
    return r;
}

// A read in which no sensor worked, while none has ever worked: logged, and after the last try the sensors are
// marked unavailable (never read again in this process). Owner of g_thermal.busy only.
inline void note_failed_read(const sensor_read& r, u64 now) noexcept
{
    auto& m = g_thermal;
    if (++m.failed_tries == 1) m.first_try = now;
    char status[96];
    sensor_status_text(r, status, sizeof(status));
    char line[256];
    if (m.failed_tries < thermal_tries)
    {
        std::snprintf(line, sizeof(line), "thermal: no reading (%s), try %u of %u; next try %llu s after the first", status, m.failed_tries, thermal_tries,
            static_cast<unsigned long long>(thermal_retry_after_us[m.failed_tries - 1] / 1'000'000));
        log(line);
        return;
    }
    m.unavailable.store(true, std::memory_order_release);
    std::snprintf(line, sizeof(line), "thermal: sensor unavailable (%s); the emulator cannot pause on heat, the PS4 firmware protects the console", status);
    log(line);
}

// True when the sensors are due for a read at 'now'. Owner of g_thermal.busy only.
inline bool sensors_due(u64 now) noexcept
{
    const auto& m = g_thermal;
    if (!m.polled) return true;
    if (m.ever_ok.load(std::memory_order_relaxed) || !m.failed_tries) return now - m.last_poll >= thermal_poll_us;
    return now - m.first_try >= thermal_retry_after_us[m.failed_tries - 1];
}
} // namespace detail

// One read-only sample of both temperature sensors (unfiltered). The first call logs the raw return codes and values.
inline thermal read_thermal() noexcept
{
    if (detail::thermal_disabled()) return {};
    const detail::sensor_read r = detail::read_sensors();
    return make_thermal(r.cpu_status == sensor_ok ? r.cpu : 0, r.soc_status == sensor_ok ? r.soc : 0);
}

// thermal_ok / thermal_warn / thermal_critical. Reads the sensors at most once per second (whichever thread calls
// first; the others get the last level). Shows one toast when the console first gets hot.
// 0.34: while no sensor has ever answered, only at the first call and thermal_retry_after_us after it; then the
// sensors are marked unavailable and this returns thermal_ok without any call. Working sensors go through
// sensor_filter (median of 3, jump rejection) before the state machine.
inline int thermal_state() noexcept
{
    auto& m = detail::g_thermal;
    if (m.unavailable.load(std::memory_order_acquire) || detail::thermal_disabled()) return m.level.load(std::memory_order_acquire);
    if (!m.busy.exchange(true, std::memory_order_acquire))
    {
        const u64 now = detail::now_us();
        if (detail::sensors_due(now))
        {
            m.polled = true;
            m.last_poll = now;
            const detail::sensor_read r = detail::read_sensors();
            const bool cpu_ok = r.cpu_status == sensor_ok;
            const bool soc_ok = r.soc_status == sensor_ok;
            if (cpu_ok || soc_ok)
            {
                if (!m.ever_ok.exchange(true, std::memory_order_relaxed))
                {
                    char line[160];
                    std::snprintf(line, sizeof(line), "thermal: sensor reading works (cpu %d C %s, soc %d C %s); read every second", r.cpu,
                        sensor_status_name(r.cpu_status), r.soc, sensor_status_name(r.soc_status));
                    detail::log(line);
                }
                // A sensor that fails on this read keeps its last filtered value.
                const int cpu = cpu_ok ? m.cpu_filter.push(r.cpu) : m.cpu_filter.median();
                const int soc = soc_ok ? m.soc_filter.push(r.soc) : m.soc_filter.median();
                const thermal t = make_thermal(cpu, soc);
                const int old = m.tracker.state;
                const int level = m.tracker.update(t, now);
                m.cpu_c.store(t.cpu_c, std::memory_order_relaxed);
                m.soc_c.store(t.soc_c, std::memory_order_relaxed);
                m.valid.store(t.valid, std::memory_order_relaxed);
                m.level.store(level, std::memory_order_release);
                if (level != old) detail::thermal_transition(old, level, t);
            }
            else if (!m.ever_ok.load(std::memory_order_relaxed))
            {
                detail::note_failed_read(r, now);
            }
        }
        m.busy.store(false, std::memory_order_release);
    }
    return m.level.load(std::memory_order_acquire);
}

// True while emulation should stay paused to let the console cool down (critical until below thermal_clear_c).
// 0.34: also forces the governor's heap sample once per second (governor::poll_heap), so the save-now notice of
// heap level 2 follows within a second; this is the only hook the main loop calls on every pass.
inline bool pause_requested() noexcept
{
    governor::poll_heap();
    return thermal_state() == thermal_critical && !detail::thermal_pause_disabled();
}

// False while /data is nearly full (below cache_stop_bytes at the last check). Any thread; one relaxed load.
inline bool cache_writes_allowed() noexcept
{
    return !detail::g_cache_writes_blocked.load(std::memory_order_relaxed);
}

// Free bytes on /data at the last check, 0 = unknown.
inline u64 disk_free_bytes() noexcept
{
    return detail::g_disk.free_bytes.load(std::memory_order_relaxed);
}

// 0.33: main thread, no game running, right before a boot. title_id: the game about to boot (empty when unknown: the
// most recently booted cache is kept instead). Marks that title as just used, then removes other titles' cache
// folders, oldest first, while /data has less than cache_free_target_bytes free or the cache is above its limit.
// Logs one summary line per call, one line per removed folder, and shows one toast when something was removed.
inline void trim_caches_before_boot(std::string_view title_id) noexcept
{
    detail::trim_caches(title_id);
}

// 0.33: removes /data/rpcs3/cache/cache/<title_id>/ (menu action); never while that game runs. False for an id that
// is not a title id or when a file could not be removed (the rest is left). No folder: nothing to do, true.
inline bool remove_title_cache(std::string_view title_id) noexcept
{
    if (!is_title_cache_name(title_id)) return false;
    char line[256];
    const std::string path = std::string(title_cache_dir) + std::string(title_id);
    if (!detail::title_folder_present(title_id))
    {
        std::snprintf(line, sizeof(line), "cache: %s has no cache folder (or it is a link); nothing removed", path.c_str());
        detail::log(line);
        return true;
    }

    detail::tree_size size{};
    detail::tree_walk walk{};
    detail::measure_tree(path + '/', 0, size, walk, nullptr);
    const bool ok = detail::remove_tree(path, 0);
    const std::string title(title_id);
    if (ok)
        std::snprintf(line, sizeof(line), "cache: removed %s on request (%llu MiB)", title.c_str(), static_cast<unsigned long long>(size.bytes >> 20));
    else
        std::snprintf(line, sizeof(line), "cache: could not remove all of %s on request (%llu MiB before; %s); the rest is left as it is", title.c_str(),
            static_cast<unsigned long long>(size.bytes >> 20), fmt::format("%s", fs::g_tls_error).c_str());
    detail::log(line);
    detail::update_disk(false);
    return ok;
}

inline void boot_checks() noexcept
{
    detail::update_disk(true);
    detail::raise_fd_limit();
    detail::apply_test_heap_cap();
    ::orbis::memCensus("boot");
    thermal_state();
    // First governor samples: they log only a level above 0.
    static_cast<void>(governor::heap_pressure());
    static_cast<void>(governor::thread_pressure());
    // 0.38: the first budget tick (pressure hook, reservation and cap) and its line.
    governor::budget_tick(true);
    detail::log_budget();
}

// now_us: sceKernelGetProcessTime() of the caller's report (every 60 s).
inline void periodic(u64 now_us) noexcept
{
    detail::update_disk(false);
    const int temp = thermal_state();

    ::orbis::MmapDirectStats st{};
    ::orbis::mmapDirectStats(&st);
    const int heap = governor::heap_pressure();
    const int threads = governor::thread_pressure();
    const u32 live_threads = ::ps4_thread_registry::get_counters().live;

    char disk[64];
    if (const u64 free_bytes = disk_free_bytes())
        std::snprintf(disk, sizeof(disk), "%llu MiB%s", static_cast<unsigned long long>(free_bytes >> 20), cache_writes_allowed() ? "" : " (cache writes paused)");
    else
        std::snprintf(disk, sizeof(disk), "unknown");

    const auto& sensors = detail::g_thermal;
    char temperature[128];
    // 0.36: once the sensors are marked unavailable, the 'thermal: sensor unavailable' line said so; the health
    // lines leave the temperature out from then on.
    const bool temperature_shown = sensors.valid.load(std::memory_order_relaxed) || !sensors.unavailable.load(std::memory_order_acquire);
    if (sensors.valid.load(std::memory_order_relaxed))
    {
        std::snprintf(temperature, sizeof(temperature), "cpu %d C soc %d C (%s)", sensors.cpu_c.load(std::memory_order_relaxed),
            sensors.soc_c.load(std::memory_order_relaxed), thermal_name(temp));
    }
    else if (detail::thermal_disabled())
    {
        std::snprintf(temperature, sizeof(temperature), "off (no-thermal.flag)");
    }
    else if (sensors.cpu_status.load(std::memory_order_relaxed) >= 0 && !sensors.ever_ok.load(std::memory_order_relaxed))
    {
        // 0.34: the reason no reading exists (ENOENT on every console so far: the sensor is not visible to apps).
        detail::sensor_read r{};
        r.cpu_rc = sensors.cpu_rc.load(std::memory_order_relaxed);
        r.soc_rc = sensors.soc_rc.load(std::memory_order_relaxed);
        r.cpu_status = sensors.cpu_status.load(std::memory_order_relaxed);
        r.soc_status = sensors.soc_status.load(std::memory_order_relaxed);
        char status[96];
        detail::sensor_status_text(r, status, sizeof(status));
        std::snprintf(temperature, sizeof(temperature), "unavailable (%s)", status);
    }
    else
    {
        std::snprintf(temperature, sizeof(temperature), "n/a");
    }

    // 0.34: CPU time of the whole process (all threads) since the previous report, against 6 cores.
    char cpu[64];
    {
        static std::atomic<u64> s_last_cpu_us{0};
        static std::atomic<u64> s_last_wall_us{0};
        struct ::rusage ru{};
        if (::getrusage(RUSAGE_SELF, &ru) == 0)
        {
            const u64 cpu_us = (static_cast<u64>(ru.ru_utime.tv_sec) + static_cast<u64>(ru.ru_stime.tv_sec)) * 1'000'000 +
                static_cast<u64>(ru.ru_utime.tv_usec) + static_cast<u64>(ru.ru_stime.tv_usec);
            const u64 last_cpu = s_last_cpu_us.exchange(cpu_us, std::memory_order_relaxed);
            const u64 last_wall = s_last_wall_us.exchange(now_us, std::memory_order_relaxed);
            if (now_us > last_wall && cpu_us >= last_cpu)
                std::snprintf(cpu, sizeof(cpu), "process %llu%% of 600%%", static_cast<unsigned long long>((cpu_us - last_cpu) * 100 / (now_us - last_wall)));
            else
                std::snprintf(cpu, sizeof(cpu), "process n/a");
        }
        else
        {
            std::snprintf(cpu, sizeof(cpu), "process n/a (getrusage errno %d)", errno);
        }
    }

    // 0.34: the musl small heap (never returned) apart from the large blocks, and the heap reserve.
    const u64 small_heap = st.small_heap;
    const u64 large_live = st.live > small_heap ? st.live - small_heap : 0;
    char reserve[64];
    if (!governor::reserve_armed())
        std::snprintf(reserve, sizeof(reserve), "heap reserve not taken");
    else
        std::snprintf(reserve, sizeof(reserve), "heap reserve %llu MiB (%u released)",
            static_cast<unsigned long long>((governor::reserve_left() * governor::reserve_block_bytes) >> 20), governor::reserve_released());

    char line[768];
    std::snprintf(line, sizeof(line),
        "health: heap held %llu of %llu MiB, live %llu, peak %llu, %u carve-out(s)%s, small heap %llu MiB, large live %llu MiB, %llu MiB fell back to flexible; %s; "
        "flexible free %llu MiB; governor heap %d threads %d (%u live), %u compile(s) active, %llu waited; /data free %s%s%s; cpu: %s",
        static_cast<unsigned long long>(st.held >> 20), static_cast<unsigned long long>(st.cap >> 20),
        static_cast<unsigned long long>(st.live >> 20), static_cast<unsigned long long>(st.peak >> 20), st.carves,
        st.critical ? " AT CAP" : "", static_cast<unsigned long long>(small_heap >> 20), static_cast<unsigned long long>(large_live >> 20),
        static_cast<unsigned long long>(st.fallback_bytes >> 20), reserve,
        static_cast<unsigned long long>(ps4_flexible_memory_free() >> 20), governor::memory_level(), threads, live_threads,
        governor::active_compiles(), static_cast<unsigned long long>(governor::compile_waits()), disk,
        temperature_shown ? "; temperature " : "", temperature_shown ? temperature : "", cpu);
    detail::log(line);
    // 0.38: the process memory budget every 60 s (also at stage changes, level changes and the XMB).
    detail::log_budget();

    if (const gpu::vram_snapshot vram = gpu::snapshot(); vram.time_us)
    {
        char vram_line[512];
        gpu::format_line(vram, vram_line, sizeof(vram_line));
        detail::log(vram_line);
    }

    // The orbis-compat log sink keeps a limited number of lines per session, so the full census (direct memory
    // still free in the system pool, flexible low-water) runs every 15 minutes, every 5 under heap pressure.
    static std::atomic<u64> s_next_census{0};
    const u64 next = s_next_census.load(std::memory_order_relaxed);
    if (!next)
    {
        s_next_census.store(now_us + 900'000'000, std::memory_order_relaxed);
    }
    else if (now_us >= next)
    {
        s_next_census.store(now_us + (heap ? 300'000'000 : 900'000'000), std::memory_order_relaxed);
        ::orbis::memCensus("periodic");
    }
}

inline void on_stage_change() noexcept
{
    const u64 released = ::orbis::mmapDirectTrim(trim_keep);
    ::orbis::MmapDirectStats st{};
    ::orbis::mmapDirectStats(&st);
    char line[256];
    std::snprintf(line, sizeof(line), "health: compile stage finished: heap held %llu MiB, live %llu MiB, peak %llu MiB; trim returned %llu MiB",
        static_cast<unsigned long long>(st.held >> 20), static_cast<unsigned long long>(st.live >> 20),
        static_cast<unsigned long long>(st.peak >> 20), static_cast<unsigned long long>(released >> 20));
    detail::log(line);
    static std::atomic<u32> s_census{0};
    if (s_census.fetch_add(1, std::memory_order_relaxed) < 4) ::orbis::memCensus("compile done");
    // 0.34: the heap reserve, once per process, after the compile workers have given their memory back.
    governor::arm_reserve();
    // 0.38: the budget with the trimmed heap and the reserve taken
    governor::budget_tick(true);
    detail::log_budget();
}

namespace detail
{
struct stall_state
{
    std::atomic<bool> armed{true};
    std::atomic<u32> notices{0};
};

inline stall_state g_stall;
} // namespace detail

namespace detail
{
// 0.38: the memory record of the title whose session just ended (adapt folder, <TITLE>.mem), merged with its previous
// record. Main thread, before the title is cleared.
inline void write_session_record() noexcept
{
    namespace mb = utils::ps4::mem_budget;
    mb::record now{};
    int max_level = 0;
    if (!governor::end_session(now, max_level)) return;
    char title[utils::ps4::adapt::title_size];
    if (!utils::ps4::adapt::current_title(title)) return;
    const std::string previous_text = utils::ps4::adapt::read_mem_record(title);
    const mb::record previous = mb::parse_record(previous_text.data(), previous_text.size());
    const mb::record merged = mb::merge_records(previous, now);
    char record_line[128];
    mb::format_record(merged, record_line, sizeof(record_line));
    const bool written = utils::ps4::adapt::write_mem_record(title, record_line);
    char line[384];
    std::snprintf(line, sizeof(line), "memory record of %s: this session heap +%llu MiB over %llu at boot, jit %llu, guest %llu, window %llu, max level %d; "
        "kept '%s'%s", title, static_cast<unsigned long long>(now.heap >> 20), static_cast<unsigned long long>(now.boot >> 20),
        static_cast<unsigned long long>(now.jit >> 20), static_cast<unsigned long long>(now.guest >> 20), static_cast<unsigned long long>(now.window >> 20),
        max_level, record_line, written ? "" : " (NOT written)");
    log(line);
}
} // namespace detail

// Main thread only (adapt::set_title's rule). Keep the clear_title() call.
inline void on_return_to_xmb() noexcept
{
    detail::write_session_record();
    utils::ps4::adapt::clear_title();
    detail::g_stall.armed.store(true, std::memory_order_relaxed);
    const u64 released = ::orbis::mmapDirectTrim(trim_keep);
    ::orbis::MmapDirectStats st{};
    ::orbis::mmapDirectStats(&st);
    char line[256];
    std::snprintf(line, sizeof(line), "health: back in the XMB: heap held %llu MiB, live %llu MiB; trim returned %llu MiB; flexible free %llu MiB",
        static_cast<unsigned long long>(st.held >> 20), static_cast<unsigned long long>(st.live >> 20),
        static_cast<unsigned long long>(released >> 20), static_cast<unsigned long long>(ps4_flexible_memory_free() >> 20));
    detail::log(line);
    static std::atomic<u32> s_census{0};
    if (s_census.fetch_add(1, std::memory_order_relaxed) < 4) ::orbis::memCensus("xmb");
    // 0.38: the heap reserve taken again if it was spent, then the budget of the process as the next boot finds it.
    governor::rearm_reserve();
    governor::budget_tick(true);
    detail::log_budget();
}

// stalled_us: time without guest flips or progress; running: Emu.IsRunning(); background: ps4_in_background();
// compiling: true while the boot progress text is not empty (PPU/SPU compile or cache build in progress), i.e. the
// caller passes !progress_text_empty. No stall notice is shown while it is true. One notice per stall, at most 3
// per session; the next stall is reported only after the game made progress again.
inline void on_stall(u64 stalled_us, bool running, bool background, bool compiling) noexcept
{
    auto& s = detail::g_stall;
    if (!running || stalled_us < stall_notify_us)
    {
        s.armed.store(true, std::memory_order_relaxed);
        return;
    }
    if (background || compiling || !s.armed.load(std::memory_order_relaxed)) return;
    if (s.notices.load(std::memory_order_relaxed) >= 3) return;

    s.armed.store(false, std::memory_order_relaxed);
    s.notices.fetch_add(1, std::memory_order_relaxed);
    const unsigned long long seconds = stalled_us / 1'000'000;
    char text[192];
    std::snprintf(text, sizeof(text), "no guest flip or progress for %llu s (game running, in foreground, not compiling)", seconds);
    ps4_fatal_note("STALL: game looks frozen: ", text);
    char notice[256];
    i18n::trs(notice, i18n::str::HEALTH_RPCS3_JOGO_PARECE_TRAVADO_HA, {seconds});
    ps4_notify(notice);
}
#else
inline thermal read_thermal() noexcept { return {}; }
inline int thermal_state() noexcept { return thermal_ok; }
inline bool pause_requested() noexcept { return false; }
inline bool cache_writes_allowed() noexcept { return true; }
inline u64 disk_free_bytes() noexcept { return 0; }
inline void trim_caches_before_boot(std::string_view) noexcept {}
inline bool remove_title_cache(std::string_view) noexcept { return false; }
inline void boot_checks() noexcept {}
inline void periodic(u64) noexcept {}
inline void on_stage_change() noexcept {}

// Keep the clear_title() call.
inline void on_return_to_xmb() noexcept
{
    utils::ps4::adapt::clear_title();
}

inline void on_stall(u64, bool, bool, bool) noexcept {}
#endif
} // namespace utils::ps4::health
