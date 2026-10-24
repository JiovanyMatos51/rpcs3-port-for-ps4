#pragma once

// 0.40: one table of the PS4 port's behaviour switches, and the per-game automatic safe mode (header-only).
//
// Rule: a change that alters what every game sees (timing, thread order, audio clock, compile or memory concurrency,
// memory visibility, language) ships off, i.e. as the 0.38 code path, until console logs show it working. Such a row
// has default_on = false and an enable flag. Guards (they act only after a failure, near a hard limit, or have no
// guest-visible effect) and scoped changes ship on; each may name a kill flag and a safe-mode level that turns it off.
// A guard that relaxes a frontend rule the player relies on (the disc lock's offline grace) ships off as well, with an
// enable flag and a kill flag; no level reverts it, because it decides whether a game may start, not how it runs.
//
// Precedence, per row (compute(), pure and host-tested):
//   1. the row's kill flag (/data/rpcs3/<kill>.flag) forces the 0.38 path;
//   2. a safe-mode level turns off every row whose reverted_at is set and <= the level, opt-ins included. The global
//      safe mode (/data/rpcs3/safe-mode.flag) is level 1 for every game and ignores enable flags and features.txt; a
//      title's own level (adapt file, util/ps4_adapt.hpp) applies on top of it (the higher level wins). A title's level
//      after a thread failure keeps the rows kept_for_kind() names (they would cost it a thread or its database fixes);
//   3. the enable flag (/data/rpcs3/<enable>.flag), or the row's name on a line of /data/rpcs3/features.txt, turns an
//      opt-in on ('all' = every row); /data/rpcs3/custom_configs/<TITLE_ID>.features (same syntax) does so for that
//      title's boots only (not the process rows nor a frontend rule relaxation, see per_title_row);
//   4. the row's default.
//
// Reading: enabled(feature) is one relaxed atomic load, so it is valid on any thread and in signal handlers. Callers
// read it at their own point (per call, per boot, per renderer) instead of caching a flag file in a function-local
// static, so a per-game level can switch it between games. Rows with timing_process (the Mesa disk cache, set up in
// the environment before the first renderer; the stat EIO probe of the file layer; the eviction order of the disk
// cache, which runs in Emulator::Init before the title is known) keep the value of init_process() for the life of the
// process.
//
// Lifecycle: init_process() at app start (frontend, after the i18n init); set_for_boot() once per game boot, after the
// boot policy computed the title's level (System.cpp). Before init_process() the defaults apply.
//
// Failures: note_failure(kind) records "fail=<kind>" and the next level ("safe=<level + 1>", at most 3) in the
// running title's adapt file. It is allocation-free and async-signal-safe (adapt::note: open/write/fsync/close), so the
// fatal and signal paths may call it. One failure per session: the first kind wins, except that "threads" replaces
// "crash" (a guest thread refusal is the cause when both happen). note_ok() records "safe_ok" after a clean session
// at a level, which keeps that level for another 8 boots.

#include "util/types.hpp"
#include "util/ps4_adapt.hpp"

#include <atomic>
#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>

#include <sys/stat.h>

#ifndef RPCS3_PS4_SAFE_MODE_DIR
#define RPCS3_PS4_SAFE_MODE_DIR "/data/rpcs3"
#endif

// PS4 frontend (ps4_launcher.cpp): durable startup.log line. Host tests define their own.
void ps4_startup_checkpoint(const char* stage);

namespace utils::ps4::safe_mode
{
enum class feature : u8
{
    // Changes for every game: off by default (the 0.38 path), on with the enable flag or features.txt
    mesa_cache,
    ppu_start_nowait,
    thread_retry,
    join_prestart,
    lazy_services,
    governor_v039,
    precise_sleep_hr30,
    audio_hr_waits,
    audio_wait_policy,
    audio_continuity,
    audio_reserve_64,
    rsx_label_sudo,
    zcull_av_early,
    vma_release,
    ring_growth,
    interp_lane,
    gl_relaxed_zcull,
    game_language_ps4,
    pipelines_first,
    ppu_boot_workers,
    // Frontend rule relaxation: off by default (the strict rule), on with the enable flag or features.txt, never reverted
    disc_lock_grace,
    // Guards and scoped changes: on; some are turned off by a safe-mode level
    idle_services_skip,
    optional_thread_rule,
    refusal_notes,
    pause_thread_refusable,
    interp_step_bound,
    hang_stop,
    av_livelock_net,
    hr_timeout_keep,
    music_lazy,
    copy_worker_lazy,
    optional_ppu_workers_off,
    netstart_np,
    online_needs_account,
    home_menu_apply,
    disc_pause,
    live_cheats,
    stall_activity,
    config_db_entry,
    audio_out_parked,
    audio_unbuffered_rebase,
    media_open_pacing,
    zcull_skip_visible,
    fs_eio_probe,
    cb_split_after_loss,
    gl_vram_guard,
    disk_cache_order,
    iso_probe_wide,
    disc_keys_offline,
    // Paths that differ from 0.36 and older (HR timer waits, RSX copy worker): turned off at level 3 only
    hr_wait,
    rsx_copy_worker,
    count_
};

static_assert(static_cast<unsigned>(feature::count_) <= 64, "the feature mask is one u64");

enum class fail_kind : u8
{
    none,
    crash,   // fatal report, thread fatal, frozen emulation after a guest access violation
    threads, // a guest thread create ended in CELL_EAGAIN, or a must-succeed thread start was refused
    stall,   // no progress for minutes, or a stop that never finished
    gpu,     // confirmed GPU hang, video memory stop, dead FIFO stop
    user,    // the player chose a level in the per-game row
};

// When a consumer reads its row (documentation and the startup line; only timing_process changes behaviour here).
inline constexpr u8 timing_call = 0;     // at every decision
inline constexpr u8 timing_boot = 1;     // once per game boot
inline constexpr u8 timing_renderer = 2; // when a renderer is created
inline constexpr u8 timing_process = 3;  // at app start (init_process); a change applies at the next RPCS3 start

struct info
{
    feature f;
    std::string_view name;  // features.txt and log name
    const char* enable_flag; // file name under /data/rpcs3 that turns the row on, nullptr = none
    const char* kill_flag;   // file name under /data/rpcs3 that forces the 0.38 path, nullptr = none
    bool default_on;
    u8 reverted_at;          // safe-mode level that turns the row off, 0 = never
    u8 timing;               // timing_call / timing_boot / timing_renderer / timing_process
};

inline constexpr info table[] = {
    {feature::mesa_cache, "mesa_cache", "mesa-cache.flag", "mesa-cache-off.flag", false, 1, timing_process},
    {feature::ppu_start_nowait, "ppu_start_nowait", "ppu-start-nowait.flag", "ppu-start-wait.flag", false, 1, timing_call},
    {feature::thread_retry, "thread_retry", "thread-retry.flag", "thread-no-retry.flag", false, 1, timing_call},
    {feature::join_prestart, "join_prestart", "join-prestart.flag", nullptr, false, 1, timing_boot},
    {feature::lazy_services, "lazy_services", "lazy-service-threads.flag", "keep-service-threads.flag", false, 1, timing_boot},
    {feature::governor_v039, "governor_v039", "governor-v039.flag", "governor-v038.flag", false, 1, timing_call},
    {feature::precise_sleep_hr30, "precise_sleep_hr30", "precise-sleep-hr30.flag", "precise-sleep-spin.flag", false, 1, timing_call},
    {feature::audio_hr_waits, "audio_hr_waits", "audio-hr-waits.flag", "audio-poll-2ms.flag", false, 1, timing_boot},
    {feature::audio_wait_policy, "audio_wait_policy", "audio-wait-policy.flag", "audio-upstream-skip.flag", false, 1, timing_boot},
    {feature::audio_continuity, "audio_continuity", "audio-continuity.flag", "audio-no-stretch.flag", false, 1, timing_boot},
    {feature::audio_reserve_64, "audio_reserve_64", "audio-reserve-64.flag", nullptr, false, 1, timing_boot},
    {feature::rsx_label_sudo, "rsx_label_sudo", "rsx-label-sudo.flag", "rsx-label-direct.flag", false, 1, timing_renderer},
    {feature::zcull_av_early, "zcull_av_early", "zcull-av-early.flag", "zcull-av-upstream.flag", false, 1, timing_renderer},
    {feature::vma_release, "vma_release", "vma-release-empty.flag", "vma-keep-empty.flag", false, 1, timing_renderer},
    {feature::ring_growth, "ring_growth", "rings-grow.flag", "rings-no-grow.flag", false, 1, timing_renderer},
    {feature::interp_lane, "interp_lane", "interp-lane.flag", "interp-no-lane.flag", false, 1, timing_renderer},
    {feature::gl_relaxed_zcull, "gl_relaxed_zcull", "gl-relaxed-zcull.flag", "zcull-strict-gl.flag", false, 1, timing_boot},
    // Turned on by the game-language row value "PS4 language" (game-language.txt = ps4) or features.txt
    {feature::game_language_ps4, "game_language_ps4", nullptr, nullptr, false, 1, timing_boot},
    // One shared queue for the pipeline compile workers: game pipelines before shader interpreter links
    {feature::pipelines_first, "pipelines_first", "pipelines-first.flag", nullptr, false, 1, timing_renderer},
    // Up to 4 PPU module compiles in the boot compile stage, while the game has no thread yet
    {feature::ppu_boot_workers, "ppu_boot_workers", "ppu-boot-workers.flag", nullptr, false, 1, timing_call},
    // A game installed from a disc opens when the drive cannot be checked at all and a disc of that game was seen
    // recently; off keeps the strict lock (a level never reverts it: it decides whether a game starts)
    {feature::disc_lock_grace, "disc_lock_grace", "disc-lock-grace.flag", "disc-lock-strict.flag", false, 0, timing_call},

    // Service threads whose body would return at once are not started; this includes NP Handler Thread while the PSN
    // status is not RPCN
    {feature::idle_services_skip, "idle_services_skip", nullptr, "keep-service-threads.flag", true, 0, timing_boot},
    // Optional emulator threads (extra workers, SPU LLVM compile workers) leave the last slots below the console's
    // thread cap to the game; after a console refusal below the cap, the load at that refusal counts as the cap
    {feature::optional_thread_rule, "optional_thread_rule", nullptr, "optional-threads-free.flag", true, 0, timing_call},
    {feature::refusal_notes, "refusal_notes", nullptr, nullptr, true, 0, timing_call},
    // Pause Message Thread and the two savestate threads (Savestate Prepare, Emu State Capture) start refusably: a
    // refusal skips the refresh or cancels the savestate instead of ending the application
    {feature::pause_thread_refusable, "pause_thread_refusable", nullptr, nullptr, true, 0, timing_call},
    {feature::interp_step_bound, "interp_step_bound", nullptr, "interpreter-unbounded.flag", true, 0, timing_renderer},
    {feature::hang_stop, "hang_stop", nullptr, "no-hang-stop.flag", true, 2, timing_renderer},
    {feature::av_livelock_net, "av_livelock_net", nullptr, "no-av-livelock-net.flag", true, 1, timing_call},
    {feature::hr_timeout_keep, "hr_timeout_keep", nullptr, "no-hr-wait.flag", true, 1, timing_call},
    {feature::music_lazy, "music_lazy", nullptr, "keep-service-threads.flag", true, 1, timing_boot},
    {feature::copy_worker_lazy, "copy_worker_lazy", nullptr, "rsx-copy-worker-off.flag", true, 0, timing_renderer},
    {feature::optional_ppu_workers_off, "optional_ppu_workers_off", nullptr, nullptr, true, 0, timing_call},
    {feature::netstart_np, "netstart_np", nullptr, "netstart-upstream.flag", true, 2, timing_call},
    {feature::online_needs_account, "online_needs_account", nullptr, "online-without-account.flag", true, 0, timing_boot},
    // In-game audio and overlay changes apply at once. It acts only on the player's own change, and the titles that
    // reach a level (those near the thread cap, whose sound stutters) are the ones that need it, so no level reverts it
    {feature::home_menu_apply, "home_menu_apply", nullptr, nullptr, true, 0, timing_call},
    {feature::disc_pause, "disc_pause", nullptr, nullptr, true, 1, timing_call},
    {feature::live_cheats, "live_cheats", nullptr, nullptr, true, 1, timing_boot},
    {feature::stall_activity, "stall_activity", nullptr, "stall-strict.flag", true, 1, timing_call},
    {feature::config_db_entry, "config_db_entry", nullptr, "config-database-off.flag", true, 2, timing_boot},
    // The AudioOut output thread is created when the port opens and parked between pauses, so a resume near the thread
    // cap needs no new thread (kill: one thread per play)
    {feature::audio_out_parked, "audio_out_parked", nullptr, "audio-thread-per-play.flag", true, 0, timing_boot},
    // Unbuffered cellAudio rebases its period clock after a wait or a pause instead of firing the whole gap at once
    {feature::audio_unbuffered_rebase, "audio_unbuffered_rebase", nullptr, "audio-unbuffered-catchup.flag", true, 0, timing_boot},
    // After a console refusal, a thread create of the emulator's HLE media modules waits (bounded) for a slot to come
    // back, then tries once more
    {feature::media_open_pacing, "media_open_pacing", nullptr, "media-open-nopace.flag", true, 0, timing_call},
    // A draw the emulator skipped (pipeline still compiling) never makes an occlusion query or a conditional render
    // read as hidden
    {feature::zcull_skip_visible, "zcull_skip_visible", nullptr, "zcull-skip-upstream.flag", true, 2, timing_renderer},
    // A stat that answers EIO because a parent folder is missing reports "not found", as an open of the same path does
    {feature::fs_eio_probe, "fs_eio_probe", nullptr, "fs-eio-raw.flag", true, 0, timing_process},
    // After a GPU device loss of a title, its next boots split command buffers (adapt key cb_split)
    {feature::cb_split_after_loss, "cb_split_after_loss", nullptr, "cb-split-off.flag", true, 0, timing_renderer},
    // OpenGL out of video memory: purge the texture caches once, then a clear stop instead of a broken format fatal
    {feature::gl_vram_guard, "gl_vram_guard", nullptr, "gl-vram-guard-off.flag", true, 0, timing_renderer},
    // Disk cache eviction removes shader and SPU caches before PPU objects and keeps the newest titles
    {feature::disk_cache_order, "disk_cache_order", nullptr, "disk-cache-upstream.flag", true, 0, timing_process},
    // ISO images without a PS3 disc header: a wider PARAM.SFO probe and a refusal that names the image kind
    {feature::iso_probe_wide, "iso_probe_wide", nullptr, "iso-probe-strict.flag", true, 0, timing_call},
    // Disc keys without a network: per-pressing copies, the key inbox and offline-aware lookup order; consulted only
    // when the key in place is missing or fails
    {feature::disc_keys_offline, "disc_keys_offline", nullptr, "disc-keys-039.flag", true, 0, timing_call},

    {feature::hr_wait, "hr_wait", nullptr, "no-hr-wait.flag", true, 3, timing_call},
    {feature::rsx_copy_worker, "rsx_copy_worker", nullptr, "rsx-copy-worker-off.flag", true, 3, timing_renderer},
};

// Older enable flags that keep turning a row on.
struct alias
{
    feature f;
    const char* enable_flag;
};

inline constexpr alias aliases[] = {
    {feature::audio_continuity, "audio-auto-stretch.flag"}, // the 0.34-0.38 opt-in of automatic stretching
};

inline constexpr char features_file_name[] = "features.txt";
inline constexpr char global_flag_name[] = "safe-mode.flag";
inline constexpr u8 max_level = 3;

static_assert(std::size(table) == static_cast<std::size_t>(feature::count_), "one table row per feature");

constexpr bool table_in_order() noexcept
{
    for (std::size_t i = 0; i < std::size(table); i++)
        if (static_cast<std::size_t>(table[i].f) != i) return false;
    return true;
}
static_assert(table_in_order(), "table rows follow the enum order");

constexpr u64 bit(feature f) noexcept
{
    return u64{1} << static_cast<unsigned>(f);
}

constexpr const info& row(feature f) noexcept
{
    return table[static_cast<std::size_t>(f)];
}

constexpr bool enabled_in(u64 mask, feature f) noexcept
{
    return (mask & bit(f)) != 0;
}

constexpr std::string_view kind_name(fail_kind k) noexcept
{
    switch (k)
    {
    case fail_kind::crash: return "crash";
    case fail_kind::threads: return "threads";
    case fail_kind::stall: return "stall";
    case fail_kind::gpu: return "gpu";
    case fail_kind::user: return "user";
    case fail_kind::none: break;
    }
    return "none";
}

// "crash" -> fail_kind::crash; none for anything else.
constexpr fail_kind kind_from_name(std::string_view name) noexcept
{
    for (const fail_kind k : {fail_kind::crash, fail_kind::threads, fail_kind::stall, fail_kind::gpu, fail_kind::user})
        if (kind_name(k) == name) return k;
    return fail_kind::none;
}

// Adapt key of a failure kind (util/ps4_adapt.hpp), nullptr for none.
constexpr const char* fail_key(fail_kind k) noexcept
{
    switch (k)
    {
    case fail_kind::crash: return adapt::key_fail_crash;
    case fail_kind::threads: return adapt::key_fail_threads;
    case fail_kind::stall: return adapt::key_fail_stall;
    case fail_kind::gpu: return adapt::key_fail_gpu;
    case fail_kind::user: return adapt::key_fail_user;
    case fail_kind::none: break;
    }
    return nullptr;
}

// Adapt key of a level 1..3, nullptr otherwise.
constexpr const char* level_key(u8 level) noexcept
{
    return level == 1 ? adapt::key_safe1 : level == 2 ? adapt::key_safe2 : level == 3 ? adapt::key_safe3 : nullptr;
}

// One failed session moves the title one level up; level 3 stays 3.
constexpr u8 next_level(u8 level) noexcept
{
    return level >= max_level ? max_level : static_cast<u8>(level + 1);
}

// What compute() reads. Bits are feature bits.
struct inputs
{
    u64 enable_files = 0;      // the row's enable flag (or an alias) exists
    u64 kill_files = 0;        // the row's kill flag exists
    bool features_all = false; // features.txt has an 'all' line
    u64 features_named = 0;    // rows named in features.txt
    bool global_safe = false;  // safe-mode.flag exists
    u8 title_level = 0;        // this title's safe-mode level (0..3)
    fail_kind kind = fail_kind::none; // why the title is at that level
    u64 title_named = 0;       // rows named in the booted title's custom_configs/<TITLE_ID>.features (per_title_rows only)
    bool title_all = false;    // that file has an 'all' line (every per-title row)
};

// Rows a title's own .features file may turn on: not the process rows (fixed at app start, before any title is known)
// and not a frontend rule relaxation (read at the XMB, where no title is booted).
constexpr bool per_title_row(const info& r) noexcept
{
    return r.timing != timing_process && (r.default_on || r.reverted_at != 0);
}

constexpr u64 per_title_rows() noexcept
{
    u64 mask = 0;
    for (const info& r : table)
        if (per_title_row(r)) mask |= u64{1} << static_cast<unsigned>(r.f);
    return mask;
}

// The level this boot runs: the title's level, at least 1 with the global safe mode.
constexpr u8 effective_level(const inputs& in) noexcept
{
    const u8 title = in.title_level > max_level ? max_level : in.title_level;
    return in.global_safe && title < 1 ? u8{1} : title;
}

// Rows a title's own level keeps when its failure was a thread refusal: reverting the lazy cellMusic start costs the
// title one more thread at boot, and skipping its PC database entry drops correctness fixes (write-back, RSX
// reservations) that have nothing to do with threads. The global safe mode still reverts them.
constexpr bool kept_for_kind(fail_kind kind, feature f) noexcept
{
    return kind == fail_kind::threads && (f == feature::music_lazy || f == feature::config_db_entry);
}

// Why a row has its value (the startup line names it).
enum class reason : u8
{
    by_default,
    kill_flag,
    safe_level,
    enable_flag,
    features_name,
    features_all,
    kind_kept,      // the title's level would revert it, but its failure kind keeps it (kept_for_kind)
    title_features, // named in the title's custom_configs/<TITLE_ID>.features (or 'all' there)
};

constexpr reason why(const inputs& in, const info& r) noexcept
{
    const u64 b = bit(r.f);
    if (in.kill_files & b) return reason::kill_flag;
    if (r.reverted_at && effective_level(in) >= r.reverted_at)
    {
        // Only the level that comes from the title is overridden; the global safe mode alone is level 1
        const bool global_reverts = in.global_safe && r.reverted_at <= 1;
        if (!kept_for_kind(in.kind, r.f) || global_reverts) return reason::safe_level;
        return reason::kind_kept;
    }
    if (!in.global_safe)
    {
        if (in.enable_files & b) return reason::enable_flag;
        if (in.features_named & b) return reason::features_name;
        if (in.features_all) return reason::features_all;
        if (per_title_row(r) && ((in.title_named & b) || in.title_all)) return reason::title_features;
    }
    return reason::by_default;
}

constexpr bool on_for(reason why_on, const info& r) noexcept
{
    switch (why_on)
    {
    case reason::enable_flag:
    case reason::features_name:
    case reason::features_all:
    case reason::title_features: return true;
    case reason::by_default:
    case reason::kind_kept: return r.default_on;
    case reason::kill_flag:
    case reason::safe_level: break;
    }
    return false;
}

// Pure: the feature mask for these inputs (bit set = the row's newer behaviour runs).
constexpr u64 compute(const inputs& in) noexcept
{
    u64 mask = 0;
    for (const info& r : table)
        if (on_for(why(in, r), r)) mask |= bit(r.f);
    return mask;
}

inline constexpr u64 default_mask = compute(inputs{});

constexpr u64 process_rows() noexcept
{
    u64 mask = 0;
    for (const info& r : table)
        if (r.timing == timing_process) mask |= bit(r.f);
    return mask;
}

// Boot-policy degradations of a level (not feature rows): applied by the boot policy before the game starts.
enum class item : u8
{
    threads_low,         // the lighter thread budget: online off, one LLVM and one pipeline compile worker
    gpu_async,           // Async Recompiler after a GPU failure
    online_off,          // RPCN/NP off for the title
    compile_threads_1,   // one compile thread
    config_db_skip,      // the title's PC config database entry is not applied (same as the config_db_entry row)
    async_no_interpreter, // Async Recompiler without the shader interpreter
    count_
};

constexpr u32 item_bit(item i) noexcept
{
    return u32{1} << static_cast<unsigned>(i);
}

// Pure: the policy items of a level; the kind adds its own remedy from level 1 on. A thread failure keeps the title's
// PC database entry (kept_for_kind).
constexpr u32 items_for(u8 level, fail_kind kind) noexcept
{
    u32 items = 0;
    if (level >= 1 && kind == fail_kind::threads) items |= item_bit(item::threads_low);
    if (level >= 1 && kind == fail_kind::gpu) items |= item_bit(item::gpu_async);
    if (level >= 2) items |= item_bit(item::online_off) | item_bit(item::compile_threads_1);
    if (level >= 2 && !kept_for_kind(kind, feature::config_db_entry)) items |= item_bit(item::config_db_skip);
    if (level >= 3) items |= item_bit(item::async_no_interpreter);
    return items;
}

// State of this process. Relaxed loads: every reader tolerates a value from just before a boot switched it.
inline std::atomic<u64> g_on{default_mask}; // bit set = the newer behaviour of that row runs
inline std::atomic<u8> g_level{0};          // this boot's level (the title's, at least 1 with the global safe mode)
inline std::atomic<u8> g_kind{0};           // fail_kind that put the title at its level

inline bool enabled(feature f) noexcept
{
    return (g_on.load(std::memory_order_relaxed) >> static_cast<unsigned>(f)) & 1;
}

namespace detail
{
inline std::atomic<u8> g_title_level{0};   // this boot's title level (note_ok)
inline std::atomic<u8> g_session_kind{0};  // first failure of this session (note_failure)
inline std::atomic<bool> g_boot_global{false};
inline std::atomic<u64> g_boot_enables{0}; // rows a settings row turned on for the next boot (set_boot_enables)
inline inputs g_process_inputs{};          // what init_process read (main thread, before any boot)
inline u64 g_process_mask = default_mask;  // rows fixed at app start take their bit from here

inline constexpr char dir[] = RPCS3_PS4_SAFE_MODE_DIR;

constexpr char lower(char c) noexcept
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

// Case-insensitive, '-' equal to '_': "Audio-Continuity" names audio_continuity.
constexpr bool same_name(std::string_view token, std::string_view name) noexcept
{
    if (token.size() != name.size()) return false;
    for (std::size_t i = 0; i < token.size(); i++)
    {
        const char c = lower(token[i]);
        if ((c == '-' ? '_' : c) != name[i]) return false;
    }
    return true;
}

constexpr bool same_file(std::string_view token, const char* file) noexcept
{
    if (!file) return false;
    const std::string_view f(file);
    if (token.size() != f.size()) return false;
    for (std::size_t i = 0; i < token.size(); i++)
        if (lower(token[i]) != f[i]) return false;
    return true;
}

// Row index named by a features.txt token (its name, or its enable flag's file name), -1 when none.
constexpr int find_row(std::string_view token) noexcept
{
    for (const info& r : table)
        if (same_name(token, r.name) || same_file(token, r.enable_flag)) return static_cast<int>(r.f);
    for (const alias& a : aliases)
        if (same_file(token, a.enable_flag)) return static_cast<int>(a.f);
    return -1;
}

constexpr bool separator(char c) noexcept
{
    return c == ' ' || c == '\t' || c == '\r' || c == ',' || c == ';';
}

// Calls fn(token) for each token of one line ('#' starts a comment).
template <typename Fn>
constexpr void for_each_token(std::string_view line, Fn&& fn)
{
    if (const auto hash = line.find('#'); hash != std::string_view::npos) line = line.substr(0, hash);
    std::size_t i = 0;
    while (i < line.size())
    {
        while (i < line.size() && separator(line[i])) i++;
        const std::size_t start = i;
        while (i < line.size() && !separator(line[i])) i++;
        if (i > start) fn(line.substr(start, i - start));
    }
}

template <typename Fn>
constexpr void for_each_line(std::string_view text, Fn&& fn)
{
    while (!text.empty())
    {
        const auto nl = text.find('\n');
        fn(text.substr(0, nl));
        text.remove_prefix(nl == std::string_view::npos ? text.size() : nl + 1);
    }
}

inline bool make_path(const char* name, char* out, std::size_t size) noexcept
{
    if (!name || !out || !size) return false;
    const int n = std::snprintf(out, size, "%s/%s", dir, name);
    return n > 0 && static_cast<std::size_t>(n) < size;
}

inline bool file_exists(const char* name) noexcept
{
    char path[256];
    struct stat st;
    return make_path(name, path, sizeof(path)) && ::stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

// Whole small file into 'out' (at most size - 1 bytes, NUL-terminated); 0 when missing or empty.
inline std::size_t read_small(const char* name, char* out, std::size_t size) noexcept
{
    char path[256];
    if (!size || !make_path(name, path, sizeof(path))) return 0;
    std::FILE* file = std::fopen(path, "rb");
    if (!file) return 0;
    const std::size_t got = std::fread(out, 1, size - 1, file);
    std::fclose(file);
    out[got] = '\0';
    return got;
}

// "custom_configs/<TITLE_ID>.features" (relative to the directory) for a title of ASCII letters and digits, at most 16;
// false for anything else (no path is ever built from other characters)
inline bool title_file_name(std::string_view title, char* out, std::size_t size) noexcept
{
    if (title.empty() || title.size() > 16 || !out || !size) return false;
    for (const char c : title)
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) return false;
    const int n = std::snprintf(out, size, "custom_configs/%.*s.features", static_cast<int>(title.size()), title.data());
    return n > 0 && static_cast<std::size_t>(n) < size;
}

inline void log(const std::string& line) noexcept
{
    ps4_startup_checkpoint(line.c_str());
}
} // namespace detail

// Pure: folds the text of features.txt into 'in'. Unknown tokens are appended to 'unknown' (", "-separated).
inline void parse_features_text(std::string_view text, inputs& in, std::string* unknown = nullptr)
{
    detail::for_each_line(text, [&](std::string_view line)
    {
        detail::for_each_token(line, [&](std::string_view token)
        {
            if (detail::same_name(token, "all"))
            {
                in.features_all = true;
                return;
            }
            if (const int r = detail::find_row(token); r >= 0)
            {
                in.features_named |= u64{1} << static_cast<unsigned>(r);
                return;
            }
            if (unknown)
            {
                if (!unknown->empty()) unknown->append(", ");
                unknown->append(token.substr(0, 48));
            }
        });
    });
}

// Pure: folds the text of a title's custom_configs/<TITLE_ID>.features into 'in' (the syntax of features.txt; it counts
// only for that title, after the kill flags, the levels and the global safe mode). A row that cannot be turned on per
// game is appended to 'unknown' as "<name> (features.txt only)".
inline void parse_title_features_text(std::string_view text, inputs& in, std::string* unknown = nullptr)
{
    inputs file;
    parse_features_text(text, file, unknown);
    in.title_all = in.title_all || file.features_all;
    in.title_named |= file.features_named & per_title_rows();
    if (!unknown) return;
    for (const info& r : table)
    {
        if (!(file.features_named & bit(r.f)) || per_title_row(r)) continue;
        if (!unknown->empty()) unknown->append(", ");
        unknown->append(r.name).append(" (features.txt only)");
    }
}

// Reads every row's enable/kill flag, features.txt and safe-mode.flag (title_level 0, kind none); with a title also its
// custom_configs/<TITLE_ID>.features ('title_unknown' gets that file's unknown names).
inline inputs read_inputs(std::string* unknown = nullptr, std::string_view title = {}, std::string* title_unknown = nullptr)
{
    inputs in;
    for (const info& r : table)
    {
        if (r.enable_flag && detail::file_exists(r.enable_flag)) in.enable_files |= bit(r.f);
        if (r.kill_flag && detail::file_exists(r.kill_flag)) in.kill_files |= bit(r.f);
    }
    for (const alias& a : aliases)
        if (detail::file_exists(a.enable_flag)) in.enable_files |= bit(a.f);
    char text[8192];
    if (const std::size_t got = detail::read_small(features_file_name, text, sizeof(text)))
        parse_features_text(std::string_view(text, got), in, unknown);
    char name[64];
    if (detail::title_file_name(title, name, sizeof(name)))
        if (const std::size_t got = detail::read_small(name, text, sizeof(text)))
            parse_title_features_text(std::string_view(text, got), in, title_unknown);
    in.global_safe = detail::file_exists(global_flag_name);
    return in;
}

// The mask a boot at 'title_level' would run, without storing it (for the boot policy, which runs before
// set_for_boot). Process rows keep their app-start value.
inline u64 preview(u8 title_level, fail_kind kind = fail_kind::none, std::string_view title = {})
{
    inputs in = read_inputs(nullptr, title);
    in.title_level = title_level;
    in.kind = kind;
    const u64 fixed = process_rows();
    return (compute(in) & ~fixed) | (detail::g_process_mask & fixed);
}

namespace detail
{
inline std::string reason_text(const inputs& in, const info& r, reason why_on, std::string_view title = {})
{
    switch (why_on)
    {
    case reason::by_default: return "default";
    case reason::kill_flag: return r.kill_flag ? r.kill_flag : "kill flag";
    case reason::safe_level:
    {
        const u8 level = effective_level(in);
        if (in.global_safe && in.title_level < level) return "global safe mode";
        return "safe level " + std::to_string(level);
    }
    case reason::enable_flag:
    {
        // A row without an enable file is turned on by a settings row value (set_boot_enables)
        std::string text = r.enable_flag ? r.enable_flag : "settings row";
        for (const alias& a : aliases)
            if (a.f == r.f) text.append(" or ").append(a.enable_flag);
        return text;
    }
    case reason::features_name: return "features.txt";
    case reason::features_all: return "features.txt: all";
    case reason::kind_kept: return "kept: a thread failure";
    case reason::title_features: return (title.empty() ? std::string("game") : std::string(title)) + ".features";
    }
    return "default";
}

// "a, b (why1), c (why2)": names grouped by their reason, in table order of each reason's first row.
struct named_reason
{
    std::string_view name;
    std::string why;
};

inline std::string grouped(const named_reason* items, std::size_t count, bool with_reasons)
{
    if (!count) return "none";
    std::string out;
    bool done[64]{};
    for (std::size_t i = 0; i < count; i++)
    {
        if (done[i]) continue;
        for (std::size_t j = i; j < count; j++)
        {
            if (done[j] || items[j].why != items[i].why) continue;
            done[j] = true;
            if (!out.empty()) out += ", ";
            out += items[j].name;
            // the reason follows the last name of its group
            bool more = false;
            for (std::size_t k = j + 1; k < count && !more; k++) more = !done[k] && items[k].why == items[i].why;
            if (!more && with_reasons && !items[i].why.empty()) out += " (" + items[i].why + ")";
        }
    }
    return out;
}
} // namespace detail

// The 'features:' line: 0.38 behaviour (rows off by default that are off, with the reason) | 0.39 behaviour (those
// that are on, with the file that turned them on) | guards on | guards off (only when some are) | safe mode.
// 'process_in': what init_process read (process rows take their reason from it); 'next_start_mask': the mask the files
// give now at title level 0, to say when a process row changes at the next RPCS3 start.
inline std::string features_line(const inputs& in, u64 mask, std::string_view title, const inputs& process_in, u64 next_start_mask)
{
    detail::named_reason old_rows[64], new_rows[64], guards_on[64], guards_off[64];
    std::size_t n_old = 0, n_new = 0, n_on = 0, n_off = 0;
    for (const info& r : table)
    {
        const bool process = r.timing == timing_process;
        const inputs& source = process ? process_in : in;
        const bool on = enabled_in(mask, r.f);
        const reason why_on = why(source, r);
        std::string why_text = detail::reason_text(source, r, why_on, title);
        const bool changes_next_start = process && enabled_in(next_start_mask, r.f) != on;
        if (changes_next_start)
            why_text += std::string("; next RPCS3 start: ") + (on ? "off" : "on") + " (vale no próximo início do RPCS3)";
        if (!r.default_on)
        {
            if (on) new_rows[n_new++] = {r.name, std::move(why_text)};
            else old_rows[n_old++] = {r.name, std::move(why_text)};
        }
        else
        {
            // A guard that is on shows no reason, except a process row whose kill flag applies at the next start and a
            // row the title's level keeps for its failure kind
            const bool show = changes_next_start || why_on == reason::kind_kept;
            if (on) guards_on[n_on++] = {r.name, show ? std::move(why_text) : std::string()};
            else guards_off[n_off++] = {r.name, std::move(why_text)};
        }
    }
    std::string line = "features: 0.38 behaviour: " + detail::grouped(old_rows, n_old, true);
    line += " | 0.39 behaviour: " + detail::grouped(new_rows, n_new, true);
    line += " | guards on: " + detail::grouped(guards_on, n_on, true);
    if (n_off) line += " | guards off: " + detail::grouped(guards_off, n_off, true);
    line += " | safe mode: global ";
    line += in.global_safe ? "on (safe-mode.flag)" : "off";
    line += ", ";
    line += title.empty() ? std::string_view("no game") : title;
    line += " level " + std::to_string(in.title_level > max_level ? max_level : in.title_level);
    if (in.title_level && in.kind != fail_kind::none) line += " (" + std::string(kind_name(in.kind)) + ")";
    if (effective_level(in) != (in.title_level > max_level ? max_level : in.title_level))
        line += ", running level " + std::to_string(effective_level(in));
    return line;
}

// App start (frontend main thread, before the first renderer): reads the files, fixes the process rows, logs
// 'features (process): ...'. Reads before the first boot see this mask.
inline void init_process() noexcept
{
    std::string unknown;
    const inputs in = read_inputs(&unknown);
    const u64 mask = compute(in);
    detail::g_process_inputs = in;
    detail::g_process_mask = mask;
    detail::g_title_level.store(0, std::memory_order_relaxed);
    detail::g_session_kind.store(0, std::memory_order_relaxed);
    detail::g_boot_global.store(in.global_safe, std::memory_order_relaxed);
    g_level.store(effective_level(in), std::memory_order_relaxed);
    g_kind.store(0, std::memory_order_relaxed);
    g_on.store(mask, std::memory_order_relaxed);
    if (!unknown.empty()) detail::log("features.txt: not a feature name: " + unknown + " (ignored)");
    std::string line = features_line(in, mask, {}, in, mask);
    line.replace(0, std::string_view("features:").size(), "features (process):");
    detail::log(line);
}

// Rows a settings value turns on for the next boot like an enable flag (the game language row's PS4 language value
// for game_language_ps4): the boot policy stores them before set_for_boot, which folds them into the enable files, so
// the same precedence applies (kill flag, safe level and the global safe mode still turn them off).
inline void set_boot_enables(u64 rows) noexcept
{
    detail::g_boot_enables.store(rows & ~process_rows(), std::memory_order_relaxed);
}

// One game boot (System.cpp, after the boot policy): 'level' and 'kind' are the title's safe-mode state from its adapt
// file. Re-reads the files (a settings row may have changed them since the last game), stores the mask and logs one
// 'features:' line.
inline void set_for_boot(std::string_view title, u8 level, fail_kind kind) noexcept
{
    std::string unknown, title_unknown;
    inputs in = read_inputs(&unknown, title, &title_unknown);
    in.enable_files |= detail::g_boot_enables.load(std::memory_order_relaxed);
    const u64 next_start = compute(in);
    in.title_level = level > max_level ? max_level : level;
    in.kind = in.title_level ? kind : fail_kind::none;
    const u64 fixed = process_rows();
    const u64 mask = (compute(in) & ~fixed) | (detail::g_process_mask & fixed);
    detail::g_title_level.store(in.title_level, std::memory_order_relaxed);
    detail::g_session_kind.store(0, std::memory_order_relaxed);
    detail::g_boot_global.store(in.global_safe, std::memory_order_relaxed);
    g_level.store(effective_level(in), std::memory_order_relaxed);
    g_kind.store(static_cast<u8>(in.kind), std::memory_order_relaxed);
    g_on.store(mask, std::memory_order_relaxed);
    if (!unknown.empty()) detail::log("features.txt: not a feature name: " + unknown + " (ignored)");
    if (!title_unknown.empty()) detail::log(std::string(title) + ".features: not a feature name: " + title_unknown + " (ignored)");
    detail::log(features_line(in, mask, title, detail::g_process_inputs, next_start));
}

// A failure of the running game (any thread, fatal and signal paths): allocation-free, lock-free. Records the kind and
// the next level in the title's adapt file once per session; without a running title nothing is written.
inline void note_failure(fail_kind k) noexcept
{
    if (k == fail_kind::none || static_cast<u8>(k) > static_cast<u8>(fail_kind::user)) return;
    u8 current = detail::g_session_kind.load(std::memory_order_acquire);
    for (;;)
    {
        const bool replaces = current == static_cast<u8>(fail_kind::crash) && k == fail_kind::threads;
        if (current != static_cast<u8>(fail_kind::none) && !replaces) return;
        if (detail::g_session_kind.compare_exchange_weak(current, static_cast<u8>(k), std::memory_order_acq_rel)) break;
    }
    // The kind first: the boot side takes the last fail= line before or after the level line of a session.
    adapt::note(fail_key(k));
    adapt::note(level_key(next_level(g_level.load(std::memory_order_relaxed))));
}

// A clean session at a title level (>= 10 min running, frames with draws, no failure): the level stays for another
// 8 boots. Allocation-free.
inline void note_ok() noexcept
{
    if (!detail::g_title_level.load(std::memory_order_relaxed) || detail::g_session_kind.load(std::memory_order_acquire)) return;
    adapt::note(adapt::key_safe_ok);
}

// The global safe mode as the file says now (XMB, settings); a boot uses the value it read at set_for_boot.
inline bool global_on() noexcept
{
    return detail::file_exists(global_flag_name);
}

// The global safe mode of the current (or last) boot.
inline bool global_this_boot() noexcept
{
    return detail::g_boot_global.load(std::memory_order_relaxed);
}

// Writes or removes /data/rpcs3/safe-mode.flag (the global row). Applies from the next game boot.
inline bool set_global(bool on) noexcept
{
    char path[256];
    if (!detail::make_path(global_flag_name, path, sizeof(path))) return false;
    if (!on) return std::remove(path) == 0 || !detail::file_exists(global_flag_name);
    std::FILE* file = std::fopen(path, "wb");
    if (!file) return false;
    return std::fclose(file) == 0;
}

namespace features_file
{
// Pure: 'text' with every token naming 'name' removed (a line left without tokens is dropped) and, when 'on', a line
// with the row's name appended. Other lines and comments are kept.
inline std::string with(std::string_view text, std::string_view name, bool on)
{
    std::string out;
    detail::for_each_line(text, [&](std::string_view line)
    {
        while (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        const auto hash = line.find('#');
        const std::string_view comment = hash == std::string_view::npos ? std::string_view() : line.substr(hash);
        std::string kept;
        bool removed = false;
        bool any = false;
        detail::for_each_token(line, [&](std::string_view token)
        {
            any = true;
            const int r = detail::find_row(token);
            const bool match = detail::same_name(token, name) || (r >= 0 && table[r].name == name);
            if (match)
            {
                removed = true;
                return;
            }
            if (!kept.empty()) kept += ' ';
            kept.append(token);
        });
        if (!removed)
        {
            out.append(line).append("\n");
            return;
        }
        if (!any || (kept.empty() && comment.empty())) return;
        out += kept;
        if (!comment.empty()) out.append(kept.empty() ? "" : " ").append(comment);
        out += '\n';
    });
    if (on) out.append(name).append("\n");
    return out;
}

// Turns a row (or 'all') on or off in /data/rpcs3/features.txt for the next boot. False for an unknown name or when
// the file could not be written. The file is removed once it holds nothing.
inline bool set(std::string_view name, bool on)
{
    if (!detail::same_name(name, "all") && detail::find_row(name) < 0) return false;
    const std::string_view canonical = detail::same_name(name, "all") ? std::string_view("all") : table[detail::find_row(name)].name;
    char text[8192];
    const std::size_t got = detail::read_small(features_file_name, text, sizeof(text));
    const std::string updated = with(std::string_view(text, got), canonical, on);
    char path[256];
    if (!detail::make_path(features_file_name, path, sizeof(path))) return false;
    if (updated.find_first_not_of(" \t\r\n") == std::string::npos)
        return std::remove(path) == 0 || !detail::file_exists(features_file_name);
    std::FILE* file = std::fopen(path, "wb");
    if (!file) return false;
    const bool written = std::fwrite(updated.data(), 1, updated.size(), file) == updated.size();
    return std::fclose(file) == 0 && written;
}
} // namespace features_file

// A title's custom_configs/<TITLE_ID>.features, edited by the per-game settings row (the same text rules as
// features_file). The caller creates the custom_configs folder before a write.
namespace title_features_file
{
// The per-title rows the file turns on now ('all' = every per-title row)
inline u64 named(std::string_view title)
{
    char name[64], text[8192];
    if (!detail::title_file_name(title, name, sizeof(name))) return 0;
    inputs in;
    if (const std::size_t got = detail::read_small(name, text, sizeof(text))) parse_title_features_text(std::string_view(text, got), in);
    return in.title_all ? per_title_rows() : in.title_named;
}

// Names (on) or removes (off) every per-title row of 'rows' in the title's file; other lines and comments stay, and the
// file is removed once it holds nothing. A row named by 'all' stays on. False for a refused title or a failed write.
inline bool set(std::string_view title, u64 rows, bool on)
{
    char name[64], text[8192], path[256];
    if (!detail::title_file_name(title, name, sizeof(name)) || !detail::make_path(name, path, sizeof(path))) return false;
    const std::size_t got = detail::read_small(name, text, sizeof(text));
    std::string updated(text, got);
    for (const info& r : table)
        if ((rows & bit(r.f)) && per_title_row(r)) updated = features_file::with(updated, r.name, on);
    if (updated.find_first_not_of(" \t\r\n") == std::string::npos)
        return std::remove(path) == 0 || !detail::file_exists(name);
    std::FILE* file = std::fopen(path, "wb");
    if (!file) return false;
    const bool written = std::fwrite(updated.data(), 1, updated.size(), file) == updated.size();
    return std::fclose(file) == 0 && written;
}
} // namespace title_features_file
} // namespace utils::ps4::safe_mode
