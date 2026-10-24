// Host test of the PS4 feature table and the per-game safe mode (rpcs3/util/ps4_safe_mode.hpp) and of the safe-mode
// records of the adapt files (rpcs3/util/ps4_adapt.hpp):
// - the table: one row per feature, names and flag files well formed, opt-ins reverted at level 1 (the disc lock's
//   offline grace: off by default and never reverted), process rows;
// - compute(): defaults, enable flags, features.txt names and 'all', kill flags win, the global safe mode, title levels,
//   the policy items of each level and kind; one truth table per row added in 0.40 (default, its flag, every level,
//   the global safe mode);
// - the cb_split adapt key and its 8-boot expiry;
// - a title level after a thread failure keeps music_lazy and config_db_entry (kept_for_kind); a title's own
//   custom_configs/<TITLE_ID>.features (per-title rows only, kill flags, levels and the global safe mode still win);
// - the files: init_process / set_for_boot / features_file::set / the global flag, and the one 'features:' line;
// - escalation 0 -> 1 -> 2 -> 3 (stays 3, exhausted), the 8-boot expiry, safe_ok, the player's choice, the first
//   failure kind of a session (threads replaces crash);
// - the flag inventory: every "/data/rpcs3/<name>.flag" string literal and every bare "<name>.flag" literal (a name
//   joined to the folder at run time, a name check, a log text) in the eboot's sources (rpcs3/, Utilities/,
//   ps4-port/runtime, compat, probe) is a row's enable or kill flag or is on one of the allow-lists below, and every
//   allow-listed name is still in that tree.
//   A new switch must become a table row (new behaviour for every game: default off) or be added to the right
//   allow-list group here; a switch whose last literal goes leaves its list in the same change.
//
// Build (llvm-mingw), from the repo root:
//   clang++ -std=c++20 -static -Wall -Wextra -I rpcs3 ps4-port/tests/safe_mode_test.cpp -o safe_mode_test.exe
// Run from the repo root, or pass the repo root as the first argument (the inventory needs the tree). The file tests
// run in a fresh directory under the system temp directory, which is removed at the end.

#define RPCS3_PS4_ADAPT_DIR "safe-mode-test-adapt"
#define RPCS3_PS4_SAFE_MODE_DIR "safe-mode-test-data"

#include "util/ps4_safe_mode.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace sm = utils::ps4::safe_mode;
namespace adapt = utils::ps4::adapt;
namespace stdfs = std::filesystem;
using sm::fail_kind;
using sm::feature;

namespace
{
int g_failures = 0;
int g_checks = 0;

#define CHECK(cond)                                                                \
    do                                                                             \
    {                                                                              \
        g_checks++;                                                                \
        if (!(cond))                                                               \
        {                                                                          \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
            g_failures++;                                                          \
        }                                                                          \
    } while (0)

std::vector<std::string> g_lines;

std::size_t count_prefix(std::size_t from, std::string_view prefix)
{
    std::size_t n = 0;
    for (std::size_t i = from; i < g_lines.size(); i++)
        if (std::string_view(g_lines[i]).substr(0, prefix.size()) == prefix) n++;
    return n;
}

const std::string* last_with(std::string_view prefix)
{
    for (std::size_t i = g_lines.size(); i-- > 0;)
        if (std::string_view(g_lines[i]).substr(0, prefix.size()) == prefix) return &g_lines[i];
    return nullptr;
}

bool contains(const std::string* s, std::string_view part)
{
    return s && s->find(part) != std::string::npos;
}

void touch(const char* name)
{
    std::ofstream(std::string(RPCS3_PS4_SAFE_MODE_DIR) + "/" + name, std::ios::binary).flush();
}

void unlink_data(const char* name)
{
    std::error_code ec;
    stdfs::remove(std::string(RPCS3_PS4_SAFE_MODE_DIR) + "/" + name, ec);
}

void write_data(const char* name, std::string_view text)
{
    std::ofstream out(std::string(RPCS3_PS4_SAFE_MODE_DIR) + "/" + name, std::ios::binary);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

std::string read_data(const char* name)
{
    std::ifstream in(std::string(RPCS3_PS4_SAFE_MODE_DIR) + "/" + name, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

constexpr u64 bits(std::initializer_list<feature> list)
{
    u64 m = 0;
    for (const feature f : list) m |= sm::bit(f);
    return m;
}

// The class R rows: off by default, the 0.38 path
constexpr u64 opt_in_rows = bits({feature::mesa_cache, feature::ppu_start_nowait, feature::thread_retry, feature::join_prestart,
    feature::lazy_services, feature::governor_v039, feature::precise_sleep_hr30, feature::audio_hr_waits,
    feature::audio_wait_policy, feature::audio_continuity, feature::audio_reserve_64, feature::rsx_label_sudo,
    feature::zcull_av_early, feature::vma_release, feature::ring_growth, feature::interp_lane, feature::gl_relaxed_zcull,
    feature::game_language_ps4, feature::pipelines_first, feature::ppu_boot_workers});

// Every row that is off by default: the R rows and the frontend rule relaxation (the strict disc lock stays the default)
constexpr u64 default_off_rows = opt_in_rows | sm::bit(feature::disc_lock_grace);

// The guards added in 0.40: on by default, a kill flag each, never reverted by a level but zcull_skip_visible (level 2)
constexpr feature new_guards[] = {feature::audio_out_parked, feature::audio_unbuffered_rebase, feature::media_open_pacing,
    feature::zcull_skip_visible, feature::fs_eio_probe, feature::cb_split_after_loss, feature::gl_vram_guard,
    feature::disk_cache_order, feature::iso_probe_wide, feature::disc_keys_offline};

constexpr u64 all_rows = sm::bit(feature::count_) - 1;

// ---------------------------------------------------------------------------------------------------------------
void test_table()
{
    // The documented feature names (features.txt, logs), in enum order
    constexpr std::string_view expected[] = {"mesa_cache", "ppu_start_nowait", "thread_retry", "join_prestart",
        "lazy_services", "governor_v039", "precise_sleep_hr30", "audio_hr_waits", "audio_wait_policy", "audio_continuity",
        "audio_reserve_64", "rsx_label_sudo", "zcull_av_early", "vma_release", "ring_growth", "interp_lane",
        "gl_relaxed_zcull", "game_language_ps4", "pipelines_first", "ppu_boot_workers", "disc_lock_grace",
        "idle_services_skip", "optional_thread_rule", "refusal_notes", "pause_thread_refusable", "interp_step_bound",
        "hang_stop", "av_livelock_net", "hr_timeout_keep", "music_lazy", "copy_worker_lazy", "optional_ppu_workers_off",
        "netstart_np", "online_needs_account", "home_menu_apply", "disc_pause", "live_cheats", "stall_activity",
        "config_db_entry", "audio_out_parked", "audio_unbuffered_rebase", "media_open_pacing", "zcull_skip_visible",
        "fs_eio_probe", "cb_split_after_loss", "gl_vram_guard", "disk_cache_order", "iso_probe_wide", "disc_keys_offline",
        "hr_wait", "rsx_copy_worker"};
    CHECK(std::size(expected) == std::size(sm::table));
    CHECK(std::size(sm::table) == 51);
    for (std::size_t i = 0; i < std::size(expected) && i < std::size(sm::table); i++) CHECK(sm::table[i].name == expected[i]);

    std::set<std::string_view> names, enables;
    for (const sm::info& r : sm::table)
    {
        CHECK(names.insert(r.name).second);
        for (const char c : r.name) CHECK((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_');
        CHECK(r.reverted_at <= sm::max_level);
        CHECK(r.timing <= sm::timing_process);
        for (const char* flag : {r.enable_flag, r.kill_flag})
        {
            if (!flag) continue;
            const std::string_view f(flag);
            CHECK(f.size() > 5 && f.substr(f.size() - 5) == ".flag");
            CHECK(f.find('/') == std::string_view::npos);
            for (const char c : f) CHECK((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.');
        }
        if (r.enable_flag) CHECK(enables.insert(r.enable_flag).second);
        // A behaviour for every game is off by default and off from level 1 on, opt-ins included; the disc lock's
        // offline grace is off by default too, and no level reverts it (it decides whether a game starts)
        if (!r.default_on) CHECK(r.reverted_at == (r.f == feature::disc_lock_grace ? 0 : 1));
        CHECK(sm::enabled_in(default_off_rows, r.f) == !r.default_on);
        // The process rows: the Mesa disk cache, the stat EIO probe of the file layer, the disk cache eviction order
        CHECK((r.timing == sm::timing_process) ==
              (r.f == feature::mesa_cache || r.f == feature::fs_eio_probe || r.f == feature::disk_cache_order));
        // Every opt-in but the language row (its menu value turns it on) has an enable flag
        if (!r.default_on && r.f != feature::game_language_ps4) CHECK(r.enable_flag != nullptr);
        if (r.default_on) CHECK(r.enable_flag == nullptr);
    }
    for (const sm::alias& a : sm::aliases)
    {
        CHECK(enables.insert(a.enable_flag).second);
        CHECK(!sm::row(a.f).default_on);
    }
    // An enable flag is never also a kill flag
    for (const sm::info& r : sm::table)
        if (r.kill_flag) CHECK(!enables.count(r.kill_flag));

    CHECK(sm::default_mask == (all_rows & ~default_off_rows));
    CHECK(sm::process_rows() == (sm::bit(feature::mesa_cache) | sm::bit(feature::fs_eio_probe) | sm::bit(feature::disk_cache_order)));

    // Levels of the guards
    CHECK(sm::row(feature::hang_stop).reverted_at == 2);
    CHECK(sm::row(feature::netstart_np).reverted_at == 2);
    CHECK(sm::row(feature::config_db_entry).reverted_at == 2);
    CHECK(sm::row(feature::zcull_skip_visible).reverted_at == 2);
    CHECK(sm::row(feature::hr_wait).reverted_at == 3);
    CHECK(sm::row(feature::rsx_copy_worker).reverted_at == 3);
    for (const feature f : {feature::av_livelock_net, feature::hr_timeout_keep, feature::music_lazy, feature::disc_pause,
             feature::live_cheats, feature::stall_activity})
        CHECK(sm::row(f).reverted_at == 1);
    // home_menu_apply acts only on the player's own change: never reverted since 0.40
    for (const feature f : {feature::idle_services_skip, feature::optional_thread_rule, feature::refusal_notes,
             feature::pause_thread_refusable, feature::interp_step_bound, feature::copy_worker_lazy,
             feature::optional_ppu_workers_off, feature::online_needs_account, feature::home_menu_apply})
        CHECK(sm::row(f).reverted_at == 0);
    for (const feature f : new_guards)
    {
        CHECK(sm::row(f).default_on);
        CHECK(sm::row(f).enable_flag == nullptr);
        CHECK(sm::row(f).kill_flag != nullptr);
        CHECK(sm::row(f).reverted_at == (f == feature::zcull_skip_visible ? 2 : 0));
    }

    // Flag names the tester documentation gives
    CHECK(std::string_view(sm::row(feature::mesa_cache).enable_flag) == "mesa-cache.flag");
    CHECK(std::string_view(sm::row(feature::mesa_cache).kill_flag) == "mesa-cache-off.flag");
    CHECK(std::string_view(sm::row(feature::lazy_services).enable_flag) == "lazy-service-threads.flag");
    CHECK(std::string_view(sm::row(feature::governor_v039).enable_flag) == "governor-v039.flag");
    CHECK(std::string_view(sm::row(feature::governor_v039).kill_flag) == "governor-v038.flag");
    CHECK(std::string_view(sm::row(feature::audio_continuity).enable_flag) == "audio-continuity.flag");
    CHECK(std::string_view(sm::row(feature::optional_thread_rule).kill_flag) == "optional-threads-free.flag");
    CHECK(std::string_view(sm::row(feature::config_db_entry).kill_flag) == "config-database-off.flag");
    CHECK(std::string_view(sm::row(feature::interp_step_bound).kill_flag) == "interpreter-unbounded.flag");

    // The 0.40 rows: every flag name, default, level and timing as the tester documentation gives them
    struct expected_row
    {
        feature f;
        const char* enable;
        const char* kill;
        bool on;
        u8 reverted_at;
        u8 timing;
    };
    constexpr expected_row rows_040[] = {
        {feature::audio_out_parked, nullptr, "audio-thread-per-play.flag", true, 0, sm::timing_boot},
        {feature::audio_unbuffered_rebase, nullptr, "audio-unbuffered-catchup.flag", true, 0, sm::timing_boot},
        {feature::media_open_pacing, nullptr, "media-open-nopace.flag", true, 0, sm::timing_call},
        {feature::zcull_skip_visible, nullptr, "zcull-skip-upstream.flag", true, 2, sm::timing_renderer},
        {feature::fs_eio_probe, nullptr, "fs-eio-raw.flag", true, 0, sm::timing_process},
        {feature::pipelines_first, "pipelines-first.flag", nullptr, false, 1, sm::timing_renderer},
        {feature::cb_split_after_loss, nullptr, "cb-split-off.flag", true, 0, sm::timing_renderer},
        {feature::gl_vram_guard, nullptr, "gl-vram-guard-off.flag", true, 0, sm::timing_renderer},
        {feature::ppu_boot_workers, "ppu-boot-workers.flag", nullptr, false, 1, sm::timing_call},
        {feature::disk_cache_order, nullptr, "disk-cache-upstream.flag", true, 0, sm::timing_process},
        {feature::iso_probe_wide, nullptr, "iso-probe-strict.flag", true, 0, sm::timing_call},
        {feature::disc_keys_offline, nullptr, "disc-keys-039.flag", true, 0, sm::timing_call},
        {feature::disc_lock_grace, "disc-lock-grace.flag", "disc-lock-strict.flag", false, 0, sm::timing_call},
    };
    const auto same_flag = [](const char* a, const char* b) { return a && b ? std::string_view(a) == b : a == b; };
    for (const expected_row& e : rows_040)
    {
        const sm::info& r = sm::row(e.f);
        CHECK(same_flag(r.enable_flag, e.enable));
        CHECK(same_flag(r.kill_flag, e.kill));
        CHECK(r.default_on == e.on);
        CHECK(r.reverted_at == e.reverted_at);
        CHECK(r.timing == e.timing);
        CHECK(sm::enabled_in(sm::default_mask, e.f) == e.on);
    }
    CHECK(sm::row(feature::home_menu_apply).enable_flag == nullptr && sm::row(feature::home_menu_apply).kill_flag == nullptr);

    // Kinds and keys
    for (const fail_kind k : {fail_kind::crash, fail_kind::threads, fail_kind::stall, fail_kind::gpu, fail_kind::user})
    {
        CHECK(sm::kind_from_name(sm::kind_name(k)) == k);
        CHECK(std::string_view(sm::fail_key(k)) == "fail=" + std::string(sm::kind_name(k)));
    }
    CHECK(sm::kind_from_name("nope") == fail_kind::none);
    CHECK(sm::fail_key(fail_kind::none) == nullptr);
    CHECK(sm::level_key(0) == nullptr && sm::level_key(4) == nullptr);
    CHECK(std::string_view(sm::level_key(2)) == "safe=2");
    CHECK(sm::next_level(0) == 1 && sm::next_level(1) == 2 && sm::next_level(2) == 3 && sm::next_level(3) == 3);
}

// ---------------------------------------------------------------------------------------------------------------
void test_compute()
{
    sm::inputs in;
    CHECK(sm::compute(in) == sm::default_mask);
    CHECK(!sm::enabled_in(sm::compute(in), feature::governor_v039));
    CHECK(sm::enabled_in(sm::compute(in), feature::idle_services_skip));

    // Enable flag
    in.enable_files = sm::bit(feature::audio_continuity);
    CHECK(sm::compute(in) == (sm::default_mask | sm::bit(feature::audio_continuity)));
    CHECK(sm::why(in, sm::row(feature::audio_continuity)) == sm::reason::enable_flag);

    // features.txt names and 'all'
    in = {};
    in.features_named = sm::bit(feature::governor_v039) | sm::bit(feature::mesa_cache);
    CHECK(sm::compute(in) == (sm::default_mask | in.features_named));
    CHECK(sm::why(in, sm::row(feature::governor_v039)) == sm::reason::features_name);
    in = {};
    in.features_all = true;
    CHECK(sm::compute(in) == all_rows);
    CHECK(sm::why(in, sm::row(feature::ring_growth)) == sm::reason::features_all);

    // A kill flag wins over the enable flag, features.txt, 'all' and the default
    in = {};
    in.features_all = true;
    in.enable_files = sm::bit(feature::governor_v039);
    in.features_named = sm::bit(feature::governor_v039);
    in.kill_files = sm::bit(feature::governor_v039) | sm::bit(feature::hang_stop);
    CHECK(!sm::enabled_in(sm::compute(in), feature::governor_v039));
    CHECK(!sm::enabled_in(sm::compute(in), feature::hang_stop));
    CHECK(sm::enabled_in(sm::compute(in), feature::ring_growth));
    CHECK(sm::why(in, sm::row(feature::governor_v039)) == sm::reason::kill_flag);

    // Global safe mode: level 1 for every game, enable flags and features.txt ignored
    in = {};
    in.global_safe = true;
    in.features_all = true;
    in.enable_files = opt_in_rows;
    CHECK(sm::effective_level(in) == 1);
    {
        const u64 m = sm::compute(in);
        CHECK((m & opt_in_rows) == 0);
        for (const sm::info& r : sm::table)
            if (r.default_on) CHECK(sm::enabled_in(m, r.f) == (r.reverted_at == 0 || r.reverted_at > 1));
        CHECK(sm::enabled_in(m, feature::hang_stop));
        CHECK(!sm::enabled_in(m, feature::av_livelock_net));
        CHECK(!sm::enabled_in(m, feature::stall_activity));
        CHECK(sm::enabled_in(m, feature::idle_services_skip));
        CHECK(sm::why(in, sm::row(feature::ring_growth)) == sm::reason::safe_level);
        CHECK(sm::why(in, sm::row(feature::hang_stop)) == sm::reason::by_default);
    }
    // The global safe mode ignores enable flags also for rows a level does not revert: no such row exists today;
    // a default-on row has no enable flag, so the result equals the defaults with the level applied.

    // Title levels: every row with reverted_at <= level is off, opt-ins included
    for (u8 level = 0; level <= 3; level++)
    {
        in = {};
        in.title_level = level;
        in.features_all = true;
        const u64 m = sm::compute(in);
        for (const sm::info& r : sm::table)
        {
            const bool reverted = r.reverted_at && r.reverted_at <= level;
            CHECK(sm::enabled_in(m, r.f) == (!reverted));
        }
    }
    in = {};
    in.title_level = 2;
    CHECK(!sm::enabled_in(sm::compute(in), feature::hang_stop));
    CHECK(!sm::enabled_in(sm::compute(in), feature::netstart_np));
    CHECK(!sm::enabled_in(sm::compute(in), feature::config_db_entry));
    CHECK(sm::enabled_in(sm::compute(in), feature::hr_wait));
    in.title_level = 3;
    CHECK(!sm::enabled_in(sm::compute(in), feature::hr_wait));
    CHECK(!sm::enabled_in(sm::compute(in), feature::rsx_copy_worker));
    CHECK(sm::enabled_in(sm::compute(in), feature::interp_step_bound));
    CHECK(sm::enabled_in(sm::compute(in), feature::optional_thread_rule));
    in.title_level = 9; // clamped
    CHECK(sm::effective_level(in) == 3);
    // Global plus a title level: the higher one
    in = {};
    in.global_safe = true;
    in.title_level = 2;
    CHECK(sm::effective_level(in) == 2);
    CHECK(!sm::enabled_in(sm::compute(in), feature::hang_stop));
    in.title_level = 0;
    CHECK(sm::effective_level(in) == 1);

    // Policy items per level and kind
    using sm::item;
    using sm::item_bit;
    CHECK(sm::items_for(0, fail_kind::threads) == 0);
    CHECK(sm::items_for(0, fail_kind::gpu) == 0);
    CHECK(sm::items_for(1, fail_kind::threads) == item_bit(item::threads_low));
    CHECK(sm::items_for(1, fail_kind::gpu) == item_bit(item::gpu_async));
    CHECK(sm::items_for(1, fail_kind::crash) == 0);
    CHECK(sm::items_for(1, fail_kind::none) == 0);
    constexpr u32 level2 = item_bit(item::online_off) | item_bit(item::compile_threads_1) | item_bit(item::config_db_skip);
    CHECK(sm::items_for(2, fail_kind::stall) == level2);
    // A thread failure keeps the title's PC database entry at every level
    constexpr u32 level2_threads = (level2 & ~item_bit(item::config_db_skip)) | item_bit(item::threads_low);
    CHECK(sm::items_for(2, fail_kind::threads) == level2_threads);
    CHECK(sm::items_for(3, fail_kind::threads) == (level2_threads | item_bit(item::async_no_interpreter)));
    CHECK(sm::items_for(2, fail_kind::crash) == level2 && sm::items_for(2, fail_kind::gpu) == (level2 | item_bit(item::gpu_async)));
    CHECK(sm::items_for(3, fail_kind::gpu) == (level2 | item_bit(item::gpu_async) | item_bit(item::async_no_interpreter)));
    CHECK(sm::items_for(3, fail_kind::user) == (level2 | item_bit(item::async_no_interpreter)));
}

// ---------------------------------------------------------------------------------------------------------------
// A title's level after a thread failure keeps music_lazy and config_db_entry (it would cost a thread at boot and the
// PC database fixes); every other kind reverts them as before, and so does the global safe mode (level 1).
void test_kind_kept()
{
    using sm::reason;
    for (const fail_kind k : {fail_kind::crash, fail_kind::threads, fail_kind::stall, fail_kind::gpu, fail_kind::user})
    {
        CHECK(sm::kept_for_kind(k, feature::music_lazy) == (k == fail_kind::threads));
        CHECK(sm::kept_for_kind(k, feature::config_db_entry) == (k == fail_kind::threads));
        for (const sm::info& r : sm::table)
            if (r.f != feature::music_lazy && r.f != feature::config_db_entry) CHECK(!sm::kept_for_kind(k, r.f));
    }
    CHECK(!sm::kept_for_kind(fail_kind::none, feature::music_lazy));
    for (u8 level = 1; level <= 3; level++)
    {
        sm::inputs in;
        in.title_level = level;
        in.kind = fail_kind::threads;
        const u64 m = sm::compute(in);
        CHECK(sm::enabled_in(m, feature::music_lazy) && sm::enabled_in(m, feature::config_db_entry));
        CHECK(sm::why(in, sm::row(feature::music_lazy)) == reason::kind_kept);
        CHECK(sm::why(in, sm::row(feature::config_db_entry)) == (level >= 2 ? reason::kind_kept : reason::by_default));
        // every other row as for any kind
        sm::inputs crash = in;
        crash.kind = fail_kind::crash;
        CHECK((sm::compute(crash) | sm::bit(feature::music_lazy) | (level >= 2 ? sm::bit(feature::config_db_entry) : 0)) == m);
        CHECK(!sm::enabled_in(sm::compute(crash), feature::music_lazy));
        CHECK(sm::enabled_in(sm::compute(crash), feature::config_db_entry) == (level < 2));
        // the kill flags still win
        in.kill_files = sm::bit(feature::music_lazy) | sm::bit(feature::config_db_entry);
        CHECK(!sm::enabled_in(sm::compute(in), feature::music_lazy) && !sm::enabled_in(sm::compute(in), feature::config_db_entry));
        CHECK(sm::why(in, sm::row(feature::music_lazy)) == reason::kill_flag);
        // with the global safe mode, level 1 comes from it too: music_lazy is reverted, the database entry only by level 2
        in.kill_files = 0;
        in.global_safe = true;
        CHECK(!sm::enabled_in(sm::compute(in), feature::music_lazy) && sm::enabled_in(sm::compute(in), feature::config_db_entry));
        CHECK(sm::why(in, sm::row(feature::music_lazy)) == reason::safe_level);
    }
    // The global safe mode alone (no title level, kind none) reverts music_lazy
    sm::inputs global;
    global.global_safe = true;
    CHECK(!sm::enabled_in(sm::compute(global), feature::music_lazy) && sm::enabled_in(sm::compute(global), feature::config_db_entry));
    // The features: line names the reason after the kept guards
    sm::inputs in;
    in.title_level = 2;
    in.kind = fail_kind::threads;
    const std::string line = sm::features_line(in, sm::compute(in), "BLUS31029", sm::inputs{}, sm::default_mask);
    CHECK(line.find("rsx_copy_worker, music_lazy, config_db_entry (kept: a thread failure) | guards off: ") != std::string::npos);
    CHECK(line.find("| guards off: hang_stop, av_livelock_net, hr_timeout_keep, netstart_np, disc_pause, live_cheats, stall_activity, "
                    "zcull_skip_visible (safe level 2) | safe mode: global off, BLUS31029 level 2 (threads)") != std::string::npos);
}

// ---------------------------------------------------------------------------------------------------------------
// Truth tables of the rows added or changed in 0.40: the default, the row's own flag, features.txt, every title level,
// the global safe mode, and the kill flag over all of them.
bool on_with(feature f, const sm::inputs& in)
{
    return sm::enabled_in(sm::compute(in), f);
}

sm::inputs at_level(u8 level, bool global = false)
{
    sm::inputs in;
    in.title_level = level;
    in.global_safe = global;
    in.kind = level ? fail_kind::crash : fail_kind::none;
    return in;
}

void test_rows_040()
{
    using sm::reason;

    // The new guards: on; the kill flag turns each off at every level; a level reverts only zcull_skip_visible (from 2)
    for (const feature f : new_guards)
    {
        const u64 b = sm::bit(f);
        const u8 revert = f == feature::zcull_skip_visible ? 2 : 0;
        sm::inputs in;
        CHECK(on_with(f, in));
        CHECK(sm::why(in, sm::row(f)) == reason::by_default);
        in.kill_files = b;
        CHECK(!on_with(f, in));
        CHECK(sm::why(in, sm::row(f)) == reason::kill_flag);
        // The kill flag of one row changes no other row
        CHECK(sm::compute(in) == (sm::default_mask & ~b));
        for (u8 level = 0; level <= sm::max_level; level++)
        {
            for (const bool global : {false, true})
            {
                sm::inputs l = at_level(level, global);
                const u8 running = global && level < 1 ? u8{1} : level;
                const bool expect = !(revert && running >= revert);
                CHECK(on_with(f, l) == expect);
                CHECK(sm::why(l, sm::row(f)) == (expect ? reason::by_default : reason::safe_level));
                // features.txt cannot keep a reverted guard on
                l.features_all = true;
                l.features_named = b;
                CHECK(on_with(f, l) == expect);
                // and the kill flag wins over everything
                l.kill_files = b;
                CHECK(!on_with(f, l));
                CHECK(sm::why(l, sm::row(f)) == reason::kill_flag);
            }
        }
    }
    CHECK(on_with(feature::zcull_skip_visible, at_level(1)));
    CHECK(on_with(feature::zcull_skip_visible, at_level(0, true)));
    CHECK(!on_with(feature::zcull_skip_visible, at_level(2)));
    CHECK(!on_with(feature::zcull_skip_visible, at_level(3, true)));

    // The new opt-ins: off; on with their enable flag, their features.txt name or 'all'; off from level 1 on and with
    // the global safe mode whatever turned them on; no kill flag
    for (const feature f : {feature::pipelines_first, feature::ppu_boot_workers})
    {
        const u64 b = sm::bit(f);
        sm::inputs in;
        CHECK(!on_with(f, in));
        CHECK(sm::why(in, sm::row(f)) == reason::by_default);
        in.enable_files = b;
        CHECK(on_with(f, in));
        CHECK(sm::why(in, sm::row(f)) == reason::enable_flag);
        CHECK(sm::compute(in) == (sm::default_mask | b));
        in = {};
        in.features_named = b;
        CHECK(on_with(f, in));
        CHECK(sm::why(in, sm::row(f)) == reason::features_name);
        in = {};
        in.features_all = true;
        CHECK(on_with(f, in));
        CHECK(sm::why(in, sm::row(f)) == reason::features_all);
        for (u8 level = 1; level <= sm::max_level; level++)
        {
            in = at_level(level);
            in.enable_files = b;
            in.features_named = b;
            in.features_all = true;
            CHECK(!on_with(f, in));
            CHECK(sm::why(in, sm::row(f)) == reason::safe_level);
        }
        in = at_level(0, true);
        in.enable_files = b;
        in.features_named = b;
        in.features_all = true;
        CHECK(!on_with(f, in));
        CHECK(sm::why(in, sm::row(f)) == reason::safe_level);
        CHECK(sm::row(f).kill_flag == nullptr);
    }

    // The disc lock's offline grace: off (the strict lock); on with disc-lock-grace.flag, its features.txt name or
    // 'all'; disc-lock-strict.flag wins; no title level reverts it; the global safe mode ignores the enable flag and
    // features.txt, so it keeps the strict lock
    {
        const feature f = feature::disc_lock_grace;
        const u64 b = sm::bit(f);
        sm::inputs in;
        CHECK(!on_with(f, in));
        CHECK(sm::why(in, sm::row(f)) == reason::by_default);
        in.enable_files = b;
        CHECK(on_with(f, in));
        CHECK(sm::why(in, sm::row(f)) == reason::enable_flag);
        CHECK(sm::compute(in) == (sm::default_mask | b));
        in.kill_files = b;
        CHECK(!on_with(f, in));
        CHECK(sm::why(in, sm::row(f)) == reason::kill_flag);
        in = {};
        in.features_named = b;
        CHECK(on_with(f, in));
        in = {};
        in.features_all = true;
        CHECK(on_with(f, in));
        in.kill_files = b;
        CHECK(!on_with(f, in));
        in = {};
        in.kill_files = b; // the kill flag alone: still the strict lock, by the kill flag
        CHECK(!on_with(f, in));
        CHECK(sm::why(in, sm::row(f)) == reason::kill_flag);
        for (u8 level = 1; level <= sm::max_level; level++)
        {
            in = at_level(level);
            CHECK(!on_with(f, in));
            in.enable_files = b;
            CHECK(on_with(f, in));
            CHECK(sm::why(in, sm::row(f)) == reason::enable_flag);
            in.kill_files = b;
            CHECK(!on_with(f, in));
        }
        for (const u8 level : {u8{0}, u8{2}})
        {
            in = at_level(level, true);
            in.enable_files = b;
            in.features_named = b;
            in.features_all = true;
            CHECK(!on_with(f, in));
            CHECK(sm::why(in, sm::row(f)) == reason::by_default);
        }
    }

    // home_menu_apply: on at every level and with the global safe mode (it has no kill flag file, test_table)
    for (u8 level = 0; level <= sm::max_level; level++)
        for (const bool global : {false, true})
        {
            sm::inputs in = at_level(level, global);
            CHECK(on_with(feature::home_menu_apply, in));
            in.features_all = true;
            CHECK(on_with(feature::home_menu_apply, in));
        }

    // The rows whose coverage grew in 0.40 keep their switches: never reverted, the same kill flags
    for (const feature f : {feature::idle_services_skip, feature::optional_thread_rule, feature::pause_thread_refusable})
        for (u8 level = 0; level <= sm::max_level; level++)
        {
            CHECK(on_with(f, at_level(level)));
            CHECK(on_with(f, at_level(level, true)));
        }
    CHECK(std::string_view(sm::row(feature::idle_services_skip).kill_flag) == "keep-service-threads.flag");
    CHECK(sm::row(feature::pause_thread_refusable).kill_flag == nullptr);

    // features.txt: the new rows by name or by enable flag file; a kill flag file name is not a feature name
    sm::inputs t;
    std::string unknown;
    sm::parse_features_text("pipelines-first.flag\nPPU-Boot-Workers # boot stage\ndisc-lock-grace.flag\n"
                            "disc-lock-strict.flag cb-split-off.flag\nzcull_skip_visible\n", t, &unknown);
    CHECK(t.features_named == bits({feature::pipelines_first, feature::ppu_boot_workers, feature::disc_lock_grace,
                                    feature::zcull_skip_visible}));
    CHECK(unknown == "disc-lock-strict.flag, cb-split-off.flag");
    CHECK(sm::features_file::with("", "disc_lock_grace", true) == "disc_lock_grace\n");
    CHECK(sm::features_file::with("disc-lock-grace.flag\n", "disc_lock_grace", false).empty());
}

// ---------------------------------------------------------------------------------------------------------------
void test_features_text()
{
    sm::inputs in;
    std::string unknown;
    sm::parse_features_text("mesa_cache\r\n# a comment: governor_v039\n  Audio-Continuity , governor-v039.flag\n"
                            "audio-auto-stretch.flag\nfoo bar # x\n\n", in, &unknown);
    CHECK(in.features_named == (sm::bit(feature::mesa_cache) | sm::bit(feature::audio_continuity) | sm::bit(feature::governor_v039)));
    CHECK(!in.features_all);
    CHECK(unknown == "foo, bar");
    sm::parse_features_text("ALL\n", in, &unknown);
    CHECK(in.features_all);

    namespace ff = sm::features_file;
    CHECK(ff::with("", "mesa_cache", true) == "mesa_cache\n");
    CHECK(ff::with("mesa_cache\n", "mesa_cache", true) == "mesa_cache\n");
    CHECK(ff::with("mesa_cache\n", "mesa_cache", false).empty());
    CHECK(ff::with("# keep\nmesa-cache governor_v039 # c\n", "mesa_cache", false) == "# keep\ngovernor_v039 # c\n");
    CHECK(ff::with("mesa-cache.flag # why\r\nring_growth\n", "mesa_cache", false) == "# why\nring_growth\n");
    CHECK(ff::with("all\n", "all", false).empty());
}

// ---------------------------------------------------------------------------------------------------------------
std::string default_line(std::string_view safe_part)
{
    std::string line = "features: 0.38 behaviour: mesa_cache, ppu_start_nowait, thread_retry, join_prestart, lazy_services, "
                       "governor_v039, precise_sleep_hr30, audio_hr_waits, audio_wait_policy, audio_continuity, audio_reserve_64, "
                       "rsx_label_sudo, zcull_av_early, vma_release, ring_growth, interp_lane, gl_relaxed_zcull, "
                       "game_language_ps4, pipelines_first, ppu_boot_workers, disc_lock_grace (default) | 0.39 behaviour: none | "
                       "guards on: idle_services_skip, optional_thread_rule, refusal_notes, pause_thread_refusable, "
                       "interp_step_bound, hang_stop, av_livelock_net, hr_timeout_keep, music_lazy, copy_worker_lazy, "
                       "optional_ppu_workers_off, netstart_np, online_needs_account, home_menu_apply, disc_pause, live_cheats, "
                       "stall_activity, config_db_entry, audio_out_parked, audio_unbuffered_rebase, media_open_pacing, "
                       "zcull_skip_visible, fs_eio_probe, cb_split_after_loss, gl_vram_guard, disk_cache_order, iso_probe_wide, "
                       "disc_keys_offline, hr_wait, rsx_copy_worker | safe mode: ";
    line += safe_part;
    return line;
}

void test_files()
{
    stdfs::create_directories(RPCS3_PS4_SAFE_MODE_DIR);

    // Defaults: nothing on disk
    std::size_t mark = g_lines.size();
    sm::init_process();
    CHECK(sm::g_on.load() == sm::default_mask);
    CHECK(count_prefix(mark, "features (process): ") == 1);
    CHECK(count_prefix(mark, "features:") == 0);
    mark = g_lines.size();
    sm::set_for_boot("BLUS30443", 0, fail_kind::none);
    CHECK(count_prefix(mark, "features:") == 1);
    CHECK(g_lines.size() == mark + 1);
    if (const std::string* line = last_with("features:"))
    {
        CHECK(*line == default_line("global off, BLUS30443 level 0"));
        if (*line != default_line("global off, BLUS30443 level 0")) std::printf("  got: %s\n", line->c_str());
    }
    CHECK(sm::g_level.load() == 0);

    // Enable flags, an alias, a kill flag, features.txt with an unknown name
    touch("audio-auto-stretch.flag");
    touch("governor-v039.flag");
    touch("governor-v038.flag");
    touch("no-hang-stop.flag");
    touch("mesa-cache.flag");
    write_data("features.txt", "ring_growth\nnope\n");
    mark = g_lines.size();
    sm::init_process();
    CHECK(sm::enabled(feature::audio_continuity));
    CHECK(sm::enabled(feature::ring_growth));
    CHECK(sm::enabled(feature::mesa_cache));
    CHECK(!sm::enabled(feature::governor_v039));
    CHECK(!sm::enabled(feature::hang_stop));
    CHECK(sm::enabled(feature::idle_services_skip));
    CHECK(count_prefix(mark, "features.txt: not a feature name: nope (ignored)") == 1);
    CHECK(count_prefix(mark, "features") == 2);
    const std::string* process_line = last_with("features (process): ");
    CHECK(contains(process_line, "audio_continuity (audio-continuity.flag or audio-auto-stretch.flag)"));
    CHECK(contains(process_line, "ring_growth (features.txt)"));
    CHECK(contains(process_line, "governor_v039 (governor-v038.flag)"));
    CHECK(contains(process_line, "guards off: hang_stop (no-hang-stop.flag)"));
    CHECK(contains(process_line, "safe mode: global off, no game level 0"));

    // A settings row adds a name; the next boot sees it. The process row keeps its app-start value.
    CHECK(sm::features_file::set("interp_lane", true));
    CHECK(read_data("features.txt") == "ring_growth\nnope\ninterp_lane\n");
    CHECK(!sm::features_file::set("not_a_feature", true));
    unlink_data("mesa-cache.flag");
    mark = g_lines.size();
    sm::set_for_boot("BLUS30443", 0, fail_kind::none);
    CHECK(sm::enabled(feature::interp_lane));
    CHECK(sm::enabled(feature::mesa_cache)); // fixed at app start
    const std::string* boot_line = last_with("features:");
    CHECK(count_prefix(mark, "features: 0.38 behaviour: ") == 1);
    CHECK(count_prefix(mark, "features:") == 1); // one line per boot, the unknown name on its own features.txt line
    CHECK(count_prefix(mark, "features.txt: not a feature name: nope") == 1);
    CHECK(contains(boot_line, "mesa_cache (mesa-cache.flag; next RPCS3 start: off (vale no próximo início do RPCS3))"));
    CHECK(contains(boot_line, "interp_lane (features.txt)"));
    CHECK(sm::features_file::set("interp_lane", false));
    CHECK(sm::features_file::set("ring_growth", false));
    CHECK(read_data("features.txt") == "nope\n");

    // A title level turns the rows off (the process row stays as it was at app start)
    sm::set_for_boot("BLUS30443", 2, fail_kind::threads);
    CHECK(sm::g_level.load() == 2);
    CHECK(sm::g_kind.load() == static_cast<u8>(fail_kind::threads));
    CHECK(!sm::enabled(feature::audio_continuity));
    CHECK(!sm::enabled(feature::netstart_np));
    CHECK(sm::enabled(feature::mesa_cache));
    CHECK(contains(last_with("features:"), "safe mode: global off, BLUS30443 level 2 (threads)"));
    // One group per reason (a kill flag before the level), the reason after its last name
    CHECK(contains(last_with("features:"), "features: 0.38 behaviour: ppu_start_nowait, thread_retry, join_prestart, lazy_services, "
                                           "precise_sleep_hr30, audio_hr_waits, audio_wait_policy, audio_continuity, audio_reserve_64, "
                                           "rsx_label_sudo, zcull_av_early, vma_release, ring_growth, interp_lane, gl_relaxed_zcull, "
                                           "game_language_ps4, pipelines_first, ppu_boot_workers (safe level 2), governor_v039 "
                                           "(governor-v038.flag), disc_lock_grace (default) | 0.39 behaviour: "
                                           "mesa_cache (mesa-cache.flag; next RPCS3 start: off (vale no próximo início do RPCS3)) | guards on: "));
    // home_menu_apply stays on at every level since 0.40; zcull_skip_visible is reverted from level 2; a thread failure
    // keeps music_lazy and config_db_entry
    CHECK(contains(last_with("features:"), "| guards off: hang_stop (no-hang-stop.flag), av_livelock_net, hr_timeout_keep, "
                                           "netstart_np, disc_pause, live_cheats, stall_activity, zcull_skip_visible "
                                           "(safe level 2) | safe mode: "));
    CHECK(contains(last_with("features:"), "rsx_copy_worker, music_lazy, config_db_entry (kept: a thread failure) | guards off: "));
    CHECK(sm::enabled(feature::music_lazy) && sm::enabled(feature::config_db_entry));
    CHECK(contains(last_with("features:"), "home_menu_apply"));
    CHECK(!contains(last_with("features:"), "home_menu_apply (safe level 2)"));
    // the same level after a crash reverts both
    sm::set_for_boot("BLUS30443", 2, fail_kind::crash);
    CHECK(!sm::enabled(feature::music_lazy) && !sm::enabled(feature::config_db_entry));
    CHECK(contains(last_with("features:"), "hr_timeout_keep, music_lazy, netstart_np, disc_pause, live_cheats, stall_activity, config_db_entry, "
                                           "zcull_skip_visible (safe level 2) | safe mode: global off, BLUS30443 level 2 (crash)"));

    // A settings row value turns a row on for the boots that follow, like an enable flag (the game language row's PS4
    // language: game_language_ps4); the safe level still wins; a process row cannot be turned on this way
    sm::set_boot_enables(sm::bit(feature::game_language_ps4) | sm::bit(feature::mesa_cache));
    sm::set_for_boot("BLUS31426", 0, fail_kind::none);
    CHECK(sm::enabled(feature::game_language_ps4));
    CHECK(contains(last_with("features:"), "game_language_ps4 (settings row)"));
    sm::set_for_boot("BLUS31426", 1, fail_kind::crash);
    CHECK(!sm::enabled(feature::game_language_ps4));
    CHECK(contains(last_with("features:"), "game_language_ps4, pipelines_first, ppu_boot_workers (safe level 1)"));
    sm::set_boot_enables(0);
    sm::set_for_boot("BLUS31426", 0, fail_kind::none);
    CHECK(!sm::enabled(feature::game_language_ps4));

    // Global safe mode
    CHECK(!sm::global_on());
    CHECK(sm::set_global(true));
    CHECK(sm::global_on());
    sm::set_for_boot("BLUS31426", 0, fail_kind::none);
    CHECK(sm::global_this_boot());
    CHECK(sm::g_level.load() == 1);
    CHECK(!sm::enabled(feature::audio_continuity));
    CHECK(!sm::enabled(feature::av_livelock_net));
    CHECK(sm::enabled(feature::hang_stop) == false); // killed by no-hang-stop.flag
    CHECK(contains(last_with("features:"), "safe mode: global on (safe-mode.flag), BLUS31426 level 0, running level 1"));
    CHECK(contains(last_with("features:"), "game_language_ps4, pipelines_first, ppu_boot_workers (global safe mode)"));
    // The global safe mode ignores enable flags, so the grace keeps the strict lock by its default
    CHECK(contains(last_with("features:"), "disc_lock_grace (default)"));
    CHECK(contains(last_with("features:"), "stall_activity (global safe mode)"));
    CHECK(sm::set_global(false));
    CHECK(!sm::global_on());

    // preview() for the boot policy: no store
    const u64 before = sm::g_on.load();
    CHECK(!sm::enabled_in(sm::preview(1), feature::audio_continuity));
    CHECK(sm::enabled_in(sm::preview(0), feature::audio_continuity));
    CHECK(sm::g_on.load() == before);

    for (const char* f : {"audio-auto-stretch.flag", "governor-v039.flag", "governor-v038.flag", "no-hang-stop.flag", "features.txt"})
        unlink_data(f);
    CHECK(sm::features_file::set("all", true));
    CHECK(sm::features_file::set("all", false));
    CHECK(!stdfs::exists(std::string(RPCS3_PS4_SAFE_MODE_DIR) + "/features.txt"));
}

// ---------------------------------------------------------------------------------------------------------------
// custom_configs/<TITLE_ID>.features: the syntax of features.txt for one title's boots; kill flags, levels and the global
// safe mode still win; process rows and the disc lock's grace are not per title; the file names its reason.
void test_title_features()
{
    using sm::reason;
    constexpr u64 audio_039 = bits({feature::audio_wait_policy, feature::audio_hr_waits, feature::audio_reserve_64});
    // Which rows a title may turn on
    CHECK(!sm::per_title_row(sm::row(feature::mesa_cache)) && !sm::per_title_row(sm::row(feature::fs_eio_probe)) &&
        !sm::per_title_row(sm::row(feature::disk_cache_order)));
    CHECK(!sm::per_title_row(sm::row(feature::disc_lock_grace)));
    CHECK((sm::per_title_rows() & audio_039) == audio_039);
    CHECK(sm::per_title_row(sm::row(feature::pipelines_first)) && sm::per_title_row(sm::row(feature::ppu_boot_workers)));
    CHECK(sm::per_title_rows() == (all_rows & ~sm::process_rows() & ~sm::bit(feature::disc_lock_grace)));

    // The pure parser
    {
        sm::inputs in;
        std::string unknown;
        sm::parse_title_features_text("audio_wait_policy audio-hr-waits.flag\r\n# c\naudio_reserve_64 mesa_cache disc-lock-grace.flag nope\n", in, &unknown);
        CHECK(in.title_named == audio_039 && !in.title_all && in.features_named == 0);
        CHECK(unknown == "nope, mesa_cache (features.txt only), disc_lock_grace (features.txt only)");
        CHECK(sm::compute(in) == (sm::default_mask | audio_039));
        CHECK(sm::why(in, sm::row(feature::audio_reserve_64)) == reason::title_features);
        CHECK(sm::why(in, sm::row(feature::mesa_cache)) == reason::by_default);
        // kill flag and levels still win, the global safe mode ignores it
        in.kill_files = sm::bit(feature::audio_hr_waits);
        CHECK(sm::compute(in) == (sm::default_mask | sm::bit(feature::audio_wait_policy) | sm::bit(feature::audio_reserve_64)));
        in.kill_files = 0;
        for (u8 level = 1; level <= 3; level++)
        {
            sm::inputs at = in;
            at.title_level = level;
            at.kind = fail_kind::crash;
            CHECK((sm::compute(at) & audio_039) == 0);
            CHECK(sm::why(at, sm::row(feature::audio_wait_policy)) == reason::safe_level);
        }
        sm::inputs global = in;
        global.global_safe = true;
        CHECK((sm::compute(global) & audio_039) == 0);
        // features.txt names it first: its reason wins
        sm::inputs both = in;
        both.features_named = sm::bit(feature::audio_reserve_64);
        CHECK(sm::why(both, sm::row(feature::audio_reserve_64)) == reason::features_name);
        // 'all' in the title's file: every per-title row, never a process row or the grace
        sm::inputs all;
        sm::parse_title_features_text("all\n", all);
        CHECK(all.title_all && (sm::compute(all) & ~sm::default_mask) == (sm::per_title_rows() & ~sm::default_mask));
        CHECK(!sm::enabled_in(sm::compute(all), feature::mesa_cache) && !sm::enabled_in(sm::compute(all), feature::disc_lock_grace));
    }
    // The file name: letters and digits only, at most 16
    {
        char name[64];
        CHECK(sm::detail::title_file_name("BLUS30678", name, sizeof(name)) && std::string_view(name) == "custom_configs/BLUS30678.features");
        CHECK(!sm::detail::title_file_name("", name, sizeof(name)));
        CHECK(!sm::detail::title_file_name("../BLUS30678", name, sizeof(name)));
        CHECK(!sm::detail::title_file_name("BLUS3067/", name, sizeof(name)));
        CHECK(!sm::detail::title_file_name("ABCDEFGHIJKLMNOPQ", name, sizeof(name)));
        CHECK(!sm::detail::title_file_name("BLUS30678", name, 20));
    }

    // Files: one title's boots only
    stdfs::create_directories(std::string(RPCS3_PS4_SAFE_MODE_DIR) + "/custom_configs");
    sm::init_process();
    CHECK(sm::title_features_file::named("BLUS30678") == 0);
    CHECK(sm::title_features_file::set("BLUS30678", audio_039, true));
    CHECK(read_data("custom_configs/BLUS30678.features") == "audio_hr_waits\naudio_wait_policy\naudio_reserve_64\n");
    CHECK(sm::title_features_file::named("BLUS30678") == audio_039);
    CHECK(!sm::title_features_file::set("../x", audio_039, true));
    std::size_t mark = g_lines.size();
    sm::set_for_boot("BLUS30678", 0, fail_kind::none);
    CHECK((sm::g_on.load() & audio_039) == audio_039);
    CHECK(contains(last_with("features:"), "| 0.39 behaviour: audio_hr_waits, audio_wait_policy, audio_reserve_64 (BLUS30678.features) | "));
    CHECK(count_prefix(mark, "BLUS30678.features: not a feature name") == 0);
    // another title does not see it
    sm::set_for_boot("BLUS31426", 0, fail_kind::none);
    CHECK((sm::g_on.load() & audio_039) == 0);
    CHECK(contains(last_with("features:"), "| 0.39 behaviour: none | "));
    // a level still wins; the kill flag too
    sm::set_for_boot("BLUS30678", 1, fail_kind::crash);
    CHECK((sm::g_on.load() & audio_039) == 0);
    touch("audio-poll-2ms.flag");
    sm::set_for_boot("BLUS30678", 0, fail_kind::none);
    CHECK(!sm::enabled(feature::audio_hr_waits) && sm::enabled(feature::audio_wait_policy));
    unlink_data("audio-poll-2ms.flag");
    // the preview with a title reads it too
    CHECK((sm::preview(0, fail_kind::none, "BLUS30678") & audio_039) == audio_039 && (sm::preview(0) & audio_039) == 0);
    // unknown names, a process row: one line, ignored
    write_data("custom_configs/BLUS30678.features", "audio_reserve_64 fs_eio_probe wat\n");
    mark = g_lines.size();
    sm::set_for_boot("BLUS30678", 0, fail_kind::none);
    CHECK(count_prefix(mark, "BLUS30678.features: not a feature name: wat, fs_eio_probe (features.txt only) (ignored)") == 1);
    CHECK(sm::enabled(feature::audio_reserve_64) && !sm::enabled(feature::audio_wait_policy));
    // the row's write keeps other names and removes the file once empty
    CHECK(sm::title_features_file::set("BLUS30678", audio_039, false));
    CHECK(read_data("custom_configs/BLUS30678.features") == "fs_eio_probe wat\n");
    write_data("custom_configs/BLUS30678.features", "# A/B\npipelines_first\n");
    CHECK(sm::title_features_file::set("BLUS30678", audio_039, true));
    CHECK(sm::title_features_file::named("BLUS30678") == (audio_039 | sm::bit(feature::pipelines_first)));
    CHECK(sm::title_features_file::set("BLUS30678", audio_039 | sm::bit(feature::pipelines_first), false));
    CHECK(read_data("custom_configs/BLUS30678.features") == "# A/B\n");
    write_data("custom_configs/BLUS30678.features", "audio_reserve_64\n");
    CHECK(sm::title_features_file::set("BLUS30678", audio_039, false));
    CHECK(!stdfs::exists(std::string(RPCS3_PS4_SAFE_MODE_DIR) + "/custom_configs/BLUS30678.features"));
    sm::set_for_boot("BLUS30678", 0, fail_kind::none);
    CHECK((sm::g_on.load() & audio_039) == 0);
    std::error_code ec;
    stdfs::remove_all(std::string(RPCS3_PS4_SAFE_MODE_DIR) + "/custom_configs", ec);
}

// ---------------------------------------------------------------------------------------------------------------
std::string boots(unsigned n)
{
    std::string s;
    for (unsigned i = 0; i < n; i++) s += "boot\n";
    return s;
}

void test_safe_status()
{
    CHECK(adapt::safe_status("").level == 0);
    CHECK(adapt::safe_status("threads=low\nboot\n").level == 0);

    std::string t = "fail=threads\nsafe=1\n";
    adapt::safe_state s = adapt::safe_status(t);
    CHECK(s.level == 1 && s.kind == "threads" && s.boots_since == 0 && s.boots_left == 8 && !s.exhausted);
    s = adapt::safe_status(t + boots(7));
    CHECK(s.level == 1 && s.boots_left == 1);
    s = adapt::safe_status(t + boots(8));
    CHECK(s.level == 0 && s.kind.empty());

    // One step per failed session
    t = "fail=crash\nsafe=1\nboot\nfail=crash\nsafe=2\n";
    CHECK(adapt::safe_status(t).level == 2);
    t += "boot\nfail=gpu\nsafe=3\n";
    s = adapt::safe_status(t);
    CHECK(s.level == 3 && s.kind == "gpu" && !s.exhausted);
    // Level 3 failed again: stays 3, exhausted
    t += "boot\nfail=stall\nsafe=3\n";
    s = adapt::safe_status(t);
    CHECK(s.level == 3 && s.kind == "stall" && s.exhausted);
    // A clean session at level 3 clears 'exhausted' and keeps the level
    s = adapt::safe_status(t + boots(5) + "safe_ok\n" + boots(5));
    CHECK(s.level == 3 && s.boots_since == 5 && !s.exhausted);
    // A level-3 session that wrote safe_ok and failed later on: level 3 failed again
    s = adapt::safe_status("fail=gpu\nsafe=3\nboot\nsafe_ok\nfail=crash\nsafe=3\n");
    CHECK(s.level == 3 && s.kind == "crash" && s.exhausted);
    // Expired: older lower levels are older still
    CHECK(adapt::safe_status(t + boots(8)).level == 0);

    // safe_ok counts as an occurrence of the level in force
    t = "fail=threads\nsafe=1\n" + boots(6) + "safe_ok\n" + boots(6);
    s = adapt::safe_status(t);
    CHECK(s.level == 1 && s.boots_since == 6 && s.boots_left == 2);
    CHECK(adapt::safe_status(boots(3) + "safe_ok\n").level == 0); // no level: safe_ok alone means nothing

    // The player's choice: safe=0 forgets the levels before it
    t = "fail=crash\nsafe=3\nboot\nsafe=0\nfail=user\nsafe=1\n";
    s = adapt::safe_status(t);
    CHECK(s.level == 1 && s.kind == "user" && !s.exhausted);
    CHECK(adapt::safe_status("fail=crash\nsafe=2\nsafe=0\n").level == 0);
    // The player choosing level 3 twice is not 'exhausted'
    s = adapt::safe_status("safe=0\nfail=user\nsafe=3\nboot\nsafe=0\nfail=user\nsafe=3\n");
    CHECK(s.level == 3 && !s.exhausted);

    // The generic keys: the safe family lives 8 boots, the others 3
    t = "fail=threads\nsafe=1\nthreads=low\n";
    auto keys = adapt::active_keys(t + boots(3));
    CHECK(keys.size() == 2 && adapt::has(keys, "fail=threads") && adapt::has(keys, "safe=1") && !adapt::has(keys, "threads=low"));
    CHECK(adapt::active_keys(t + boots(2)).size() == 3);
    CHECK(adapt::active_keys(t + boots(8)).empty());
    for (const adapt::entry& e : adapt::parse(t + boots(4)))
        CHECK(adapt::boots_left(e) == (adapt::safe_family(e.key) ? 4u : 0u));
    CHECK(adapt::expiry_for("safe_ok") == 8 && adapt::expiry_for("gpu_hang") == 3 && adapt::expiry_for("safe=2", 10) == 10);
}

// ---------------------------------------------------------------------------------------------------------------
std::string adapt_text(const char* title)
{
    return adapt::read_text(title);
}

void boot_title(const char* title)
{
    // As the boot policy does: read the level, note the boot, then the frontend sets the title after the boot
    const adapt::safe_state s = adapt::safe_status(adapt_text(title));
    adapt::note_boot(title);
    sm::set_for_boot(title, static_cast<u8>(s.level), sm::kind_from_name(s.kind));
    adapt::set_title(title);
}

void test_escalation()
{
    constexpr const char* des = "BLUS30443";
    adapt::remove(des);

    // No running title: nothing is written
    adapt::clear_title();
    sm::set_for_boot(des, 0, fail_kind::none);
    sm::note_failure(fail_kind::crash);
    CHECK(adapt_text(des).empty());

    // Session 1 at level 0: crash first, then a guest refusal: threads replaces crash; the level is written once
    boot_title(des);
    sm::note_failure(fail_kind::crash);
    sm::note_failure(fail_kind::stall); // not a second failure of the session
    sm::note_failure(fail_kind::threads);
    sm::note_failure(fail_kind::crash);
    CHECK(adapt_text(des) == "fail=crash\nsafe=1\nfail=threads\n");
    adapt::safe_state s = adapt::safe_status(adapt_text(des));
    CHECK(s.level == 1 && s.kind == "threads");

    // Session 2 at level 1: threads first, crash later does not replace it
    adapt::clear_title();
    boot_title(des);
    CHECK(sm::g_level.load() == 1 && sm::g_kind.load() == static_cast<u8>(fail_kind::threads));
    CHECK(!sm::enabled(feature::av_livelock_net));
    sm::note_failure(fail_kind::threads);
    sm::note_failure(fail_kind::crash);
    s = adapt::safe_status(adapt_text(des));
    CHECK(s.level == 2 && s.kind == "threads");

    // Session 3 at level 2: gpu -> 3; session 4 at 3: stall -> stays 3, exhausted
    adapt::clear_title();
    boot_title(des);
    CHECK(sm::g_level.load() == 2);
    sm::note_failure(fail_kind::gpu);
    s = adapt::safe_status(adapt_text(des));
    CHECK(s.level == 3 && s.kind == "gpu" && !s.exhausted);
    adapt::clear_title();
    boot_title(des);
    CHECK(sm::g_level.load() == 3);
    CHECK(!sm::enabled(feature::hr_wait));
    sm::note_failure(fail_kind::stall);
    s = adapt::safe_status(adapt_text(des));
    CHECK(s.level == 3 && s.exhausted);

    // Session 5 at 3, clean: safe_ok keeps level 3 and clears 'exhausted'; note_ok after a failure writes nothing
    adapt::clear_title();
    boot_title(des);
    sm::note_ok();
    s = adapt::safe_status(adapt_text(des));
    CHECK(s.level == 3 && !s.exhausted && s.boots_since == 0);

    // Eight boots without a failure or a clean long session: back to level 0
    for (int i = 0; i < 8; i++)
    {
        adapt::clear_title();
        boot_title(des);
    }
    CHECK(adapt::safe_status(adapt_text(des)).level == 0);
    CHECK(sm::g_level.load() == 0 || sm::g_level.load() == 3); // the 8th boot read level 3 still (1 boot left)
    adapt::clear_title();
    boot_title(des);
    CHECK(sm::g_level.load() == 0);
    // note_ok at level 0 writes nothing
    const std::string before = adapt_text(des);
    sm::note_ok();
    CHECK(adapt_text(des) == before);

    // The global safe mode is level 1: a failure escalates from the running level
    constexpr const char* mc = "BLUS31426";
    adapt::remove(mc);
    adapt::clear_title();
    CHECK(sm::set_global(true));
    boot_title(mc);
    CHECK(sm::g_level.load() == 1);
    sm::note_failure(fail_kind::crash);
    CHECK(adapt::safe_status(adapt_text(mc)).level == 2);
    CHECK(sm::set_global(false));

    // The player's choice in the per-game row
    CHECK(adapt::set_user_level(mc, 1));
    s = adapt::safe_status(adapt_text(mc));
    CHECK(s.level == 1 && s.kind == "user");
    CHECK(adapt::set_user_level(mc, 0));
    CHECK(adapt::safe_status(adapt_text(mc)).level == 0);
    CHECK(!adapt::set_user_level(mc, 4));

    adapt::clear_title();
    adapt::remove(des);
    adapt::remove(mc);
}

// ---------------------------------------------------------------------------------------------------------------
// The 0.40 rows through the files: the process rows keep their app-start value, the frontend relaxation and a boot
// guard follow the files of each boot, and the 'features:' line names every reason.
void test_rows_040_files()
{
    // Nothing on disk (test_files removed its files)
    sm::init_process();
    CHECK(sm::enabled(feature::fs_eio_probe) && sm::enabled(feature::disk_cache_order) && !sm::enabled(feature::disc_lock_grace));

    // Kill flags of two process guards and a boot guard, and the grace's enable flag, created after app start
    touch("fs-eio-raw.flag");
    touch("disk-cache-upstream.flag");
    touch("audio-thread-per-play.flag");
    touch("disc-lock-grace.flag");
    sm::set_for_boot("BLES01128", 0, fail_kind::none);
    CHECK(sm::enabled(feature::fs_eio_probe));     // fixed at app start
    CHECK(sm::enabled(feature::disk_cache_order)); // fixed at app start
    CHECK(!sm::enabled(feature::audio_out_parked));
    CHECK(sm::enabled(feature::disc_lock_grace));
    const std::string* line = last_with("features:");
    CHECK(contains(line, "| 0.39 behaviour: disc_lock_grace (disc-lock-grace.flag) |"));
    CHECK(contains(line, "fs_eio_probe, disk_cache_order (default; next RPCS3 start: off (vale no próximo início do RPCS3))"));
    CHECK(contains(line, "| guards off: audio_out_parked (audio-thread-per-play.flag) |"));
    if (!contains(line, "fs_eio_probe, disk_cache_order (default; next RPCS3 start: off") && line) std::printf("  got: %s\n", line->c_str());

    // The next RPCS3 start reads them: both process guards off by their kill flags
    sm::init_process();
    CHECK(!sm::enabled(feature::fs_eio_probe) && !sm::enabled(feature::disk_cache_order));
    CHECK(contains(last_with("features (process): "), "| guards off: audio_out_parked (audio-thread-per-play.flag), "
                                                      "fs_eio_probe (fs-eio-raw.flag), disk_cache_order (disk-cache-upstream.flag) |"));
    CHECK(!contains(last_with("features (process): "), "next RPCS3 start"));

    // The strict flag wins over the grace's enable flag; a title level never reverts the grace
    sm::set_for_boot("BLES01128", 3, fail_kind::gpu);
    CHECK(sm::enabled(feature::disc_lock_grace));
    CHECK(!sm::enabled(feature::zcull_skip_visible));
    CHECK(sm::enabled(feature::cb_split_after_loss));
    touch("disc-lock-strict.flag");
    sm::set_for_boot("BLES01128", 0, fail_kind::none);
    CHECK(!sm::enabled(feature::disc_lock_grace));
    CHECK(contains(last_with("features:"), "disc_lock_grace (disc-lock-strict.flag)"));
    unlink_data("disc-lock-strict.flag");

    // The global safe mode keeps the strict lock even with the grace's enable flag
    CHECK(sm::set_global(true));
    sm::set_for_boot("BLES01128", 0, fail_kind::none);
    CHECK(!sm::enabled(feature::disc_lock_grace));
    CHECK(sm::enabled(feature::home_menu_apply));
    CHECK(sm::enabled(feature::disc_keys_offline));
    CHECK(sm::set_global(false));

    for (const char* f : {"fs-eio-raw.flag", "disk-cache-upstream.flag", "audio-thread-per-play.flag", "disc-lock-grace.flag"})
        unlink_data(f);
    sm::init_process();
    CHECK(sm::g_on.load() == sm::default_mask);
}

// ---------------------------------------------------------------------------------------------------------------
// The cb_split adapt key: written by the device-loss note next to gpu_hang, it applies 8 boots (gpu_hang 3).
void test_cb_split_key()
{
    CHECK(std::string_view(adapt::key_cb_split) == "cb_split");
    CHECK(adapt::detail::valid_key(adapt::key_cb_split));
    CHECK(adapt::long_lived(adapt::key_cb_split) && adapt::long_lived("safe=1") && adapt::long_lived("safe_ok"));
    CHECK(!adapt::long_lived(adapt::key_gpu_hang) && !adapt::long_lived("cb_split=1") && !adapt::long_lived("cb_spli"));
    CHECK(!adapt::safe_family(adapt::key_cb_split)); // it never sets a safe-mode level
    CHECK(adapt::cb_split_expiry_boots == 8);
    CHECK(adapt::expiry_for(adapt::key_cb_split) == 8 && adapt::expiry_for(adapt::key_cb_split, 10) == 10);
    CHECK(adapt::expiry_for(adapt::key_gpu_hang) == 3);

    const std::string t = "gpu_hang\ncb_split\n";
    CHECK(adapt::active_keys(t + boots(2)).size() == 2);
    auto keys = adapt::active_keys(t + boots(3));
    CHECK(keys.size() == 1 && adapt::has(keys, adapt::key_cb_split));
    CHECK(adapt::has(adapt::active_keys(t + boots(7)), adapt::key_cb_split));
    CHECK(adapt::active_keys(t + boots(8)).empty());
    for (const adapt::entry& e : adapt::parse(t + boots(5)))
        CHECK(adapt::boots_left(e) == (e.key == adapt::key_cb_split ? 3u : 0u));
    // Another loss renews it
    CHECK(adapt::has(adapt::active_keys(t + boots(7) + "cb_split\n" + boots(7)), adapt::key_cb_split));
    // A reader after the boot policy appended this boot's "boot" line has one boot more of reach
    CHECK(adapt::has(adapt::active_keys(t + boots(8), adapt::cb_split_expiry_boots + 1), adapt::key_cb_split));
    CHECK(adapt::active_keys(t + boots(9), adapt::cb_split_expiry_boots + 1).empty());
    // It is not part of the safe-mode state
    CHECK(adapt::safe_status(t).level == 0);
    CHECK(adapt::safe_status("fail=gpu\nsafe=1\ncb_split\n").level == 1);

    // Through the file: once per session for the running title, then 8 boots
    constexpr const char* gta = "BLES01128";
    adapt::remove(gta);
    adapt::clear_title();
    CHECK(!adapt::note(adapt::key_cb_split)); // no running title: nothing is written
    CHECK(adapt::read_text(gta).empty());
    adapt::set_title(gta);
    CHECK(adapt::note(adapt::key_gpu_hang) && adapt::note(adapt::key_cb_split) && adapt::note(adapt::key_cb_split));
    CHECK(adapt::read_text(gta) == "gpu_hang\ncb_split\n");
    adapt::clear_title();
    for (int i = 0; i < 7; i++) CHECK(adapt::note_boot(gta));
    CHECK(adapt::keys(gta) == std::vector<std::string>{"cb_split"});
    CHECK(adapt::note_boot(gta));
    CHECK(adapt::keys(gta).empty());
    adapt::remove(gta);
}

// ---------------------------------------------------------------------------------------------------------------
// Flag inventory. Every allow-list entry is a file name under /data/rpcs3 that is not a row of the feature table.

// Switches present in the 0.38 build (its eboot strings): each selects 0.38-or-older behaviour or an older opt-in, so
// it is part of the 0.38 state the defaults must equal, not a change made after 0.38.
constexpr std::string_view allow_038_switches[] = {
    "audio-fine-poll.flag", "audio-high-priority.flag", "audio-no-conceal.flag", "dma-age-out-off.flag", "dma.flag",
    "font-pages-unbounded.flag", "interpreter-no-lru.flag", "interpreter-sync-link.flag", "large-pages.flag",
    "no-bg-pause.flag", "no-cache-trim.flag", "no-extra-spu-workers.flag", "no-gnm-keepalive.flag", "no-lightbar.flag",
    "no-loadexec.flag", "no-lock-release.flag", "no-mem-budget.flag", "no-pipeline-lru.flag", "no-pool-release.flag",
    "no-precise-sleep.flag", "no-rr.flag", "no-spu-object-cache.flag", "no-stack-rescue.flag", "no-thermal-pause.flag",
    "no-thermal.flag", "no-tsc-fallback.flag", "own-stacks.flag", "pad-keep-open.flag", "pad-no-motion.flag",
    "patch-extract-off.flag", "ppu-check-full.flag", "ppu-keep-objects.flag", "ppu-priority.flag", "ring-reclaim-off.flag",
    "rings-full.flag", "rsx-compile-no-slot.flag", "rsx-no-report-drain.flag", "rsx-normal-priority.flag",
    "rsx-plain-copy.flag", "rsx-sema-spin.flag", "rsx-sema-yield.flag", "savedata-preload.flag",
    "shader-interpreter-lazy.flag", "shader-interpreter-preload.flag", "shader-preload-all.flag", "shader-skip-draws.flag",
    "spu-compile-push.flag", "spu-fast-fma.flag", "spu-objcache-off.flag", "spu-objcache-readonly.flag",
    "spu-objcache-reset.flag", "spu-timebase-baked.flag", "spu-wait-escalation.flag", "texture-age-evict-off.flag",
    "upload-staging-off.flag", "vdec-full-quality.flag", "vdec-queue-60.flag", "vdec-release-slot.flag",
    "vdec-slice-threads.flag", "vdec-sws-convert.flag", "vk-app-pipeline-cache.flag", "vk-fast-pipelines.flag",
    "vram-meter-off.flag", "vram-reserve-off.flag", "vram-trim-every-frame.flag", "zcull-relaxed.flag",
    "zcull-strict-on-read.flag",
};

// Player settings and state files the menus or the emulator write (0.38 and older; 0.40: the XMB's own background and
// music switch, frontend only).
constexpr std::string_view allow_settings_state[] = {
    "accurate-dfma.flag", "auto-config-off.flag", "boot-dirty.flag", "cool-mode.flag", "keep-user-settings.flag",
    "online-rpcn.flag", "upnp.flag", "vk-compile-3.flag", "vram.flag", "xmb-media-off.flag",
};

// Diagnostics, probes and fault injection for console tests: logs, dumps, profiles, refusals on purpose (0.40: the
// kill switch of the repeated-line limiter of the live log).
constexpr std::string_view allow_diagnostics[] = {
    "coredump.flag", "diagnostics.flag", "log-flood-off.flag", "no-abi-probe.flag", "no-profile.flag", "pad-motion-log.flag",
    "profile.flag", "refuse-threads.flag", "rsx-profiler-off.flag", "spu-objcache-verify.flag", "stall-sample.flag",
    "test-io-fail.flag", "test-refuse-hle-thread.flag", "test-refuse-np-thread.flag", "test-refuse-rpcn-account.flag",
    "thread-limit-probe.flag",
};

// Already opt-in since they were added (absent = the 0.38 behaviour).
constexpr std::string_view allow_opt_in[] = {
    "dead-fifo-stop.flag", "gl-async.flag", "shader-translate-async.flag",
};

// Boot gates, disc checks and the filesystem repair: they act before the game runs and are not in the safe mode.
constexpr std::string_view allow_gates[] = {
    "dev-hdd0-gate-off.flag", "iso-size-gate-off.flag", "iso-strict-header.flag", "no-fs-repair.flag",
};

// Kill switch of a proven fix (a frozen emulation returns to the XMB), and a sub-switch inside a table row (the
// Modo frio compile slots of governor_v039).
constexpr std::string_view allow_proven_and_sub[] = {
    "cool-mode-serial-boot.flag", "frozen-stays.flag",
};

// The safe mode's own file.
constexpr std::string_view allow_safe_mode[] = {
    "safe-mode.flag",
};

struct literal
{
    std::string name;
    std::string where;
    bool bare = false; // "<name>.flag" without the folder
};

constexpr bool flag_name_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '+';
}

// The sources of the eboot and of the standalone probe. The tests (made-up names) and the third-party trees are not
// scanned.
constexpr const char* scanned_dirs[] = {"rpcs3", "Utilities", "ps4-port/runtime", "ps4-port/compat", "ps4-port/probe"};

std::vector<literal> scan_tree(const stdfs::path& root)
{
    std::vector<literal> out;
    const std::string needle = "\"/data/rpcs3/";
    const std::string suffix = ".flag\"";
    for (const char* sub : scanned_dirs)
    {
        std::error_code ec;
        for (stdfs::recursive_directory_iterator it(root / sub, stdfs::directory_options::skip_permission_denied, ec), end; it != end;
             it.increment(ec))
        {
            if (ec) break;
            if (!it->is_regular_file(ec)) continue;
            const std::string ext = it->path().extension().string();
            if (ext != ".cpp" && ext != ".h" && ext != ".hpp" && ext != ".inc" && ext != ".c" && ext != ".cc" && ext != ".mm") continue;
            std::ifstream in(it->path(), std::ios::binary);
            std::stringstream s;
            s << in.rdbuf();
            const std::string text = s.str();
            for (std::size_t pos = text.find(needle); pos != std::string::npos; pos = text.find(needle, pos + 1))
            {
                std::size_t end_pos = pos + needle.size();
                while (end_pos < text.size())
                {
                    if (!flag_name_char(text[end_pos])) break;
                    end_pos++;
                }
                if (end_pos >= text.size() || text[end_pos] != '"') continue;
                const std::string name = text.substr(pos + needle.size(), end_pos - pos - needle.size());
                if (name.size() <= 5 || name.substr(name.size() - 5) != ".flag") continue;
                const std::size_t line = static_cast<std::size_t>(std::count(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(pos), '\n')) + 1;
                out.push_back({name, stdfs::relative(it->path(), root, ec).generic_string() + ":" + std::to_string(line)});
            }
            // A quoted file name alone names a flag too: the safe mode's own file is joined to the folder at run time
            // (no full path literal exists), name checks and log lines quote the bare name.
            for (std::size_t pos = text.find(suffix); pos != std::string::npos; pos = text.find(suffix, pos + 1))
            {
                std::size_t start = pos;
                while (start > 0 && flag_name_char(text[start - 1])) start--;
                if (start == pos || start == 0 || text[start - 1] != '"') continue;
                const std::string name = text.substr(start, pos + suffix.size() - 1 - start);
                const std::size_t line = static_cast<std::size_t>(std::count(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(pos), '\n')) + 1;
                out.push_back({name, stdfs::relative(it->path(), root, ec).generic_string() + ":" + std::to_string(line), true});
            }
        }
    }
    return out;
}

void test_inventory(const stdfs::path& root)
{
    if (!stdfs::exists(root / "rpcs3" / "util" / "ps4_safe_mode.hpp"))
    {
        std::printf("FAIL inventory: the repo tree was not found at '%s' (run from the repo root or pass it as the first argument)\n",
            root.generic_string().c_str());
        g_failures++;
        return;
    }

    std::set<std::string> rows, allowed;
    for (const sm::info& r : sm::table)
    {
        if (r.enable_flag) rows.insert(r.enable_flag);
        if (r.kill_flag) rows.insert(r.kill_flag);
    }
    for (const sm::alias& a : sm::aliases) rows.insert(a.enable_flag);
    const auto add = [&](auto& list)
    {
        for (const std::string_view name : list)
        {
            CHECK(!rows.count(std::string(name)));         // a row flag is never on an allow-list too
            CHECK(allowed.insert(std::string(name)).second); // listed once
        }
    };
    add(allow_038_switches);
    add(allow_settings_state);
    add(allow_diagnostics);
    add(allow_opt_in);
    add(allow_gates);
    add(allow_proven_and_sub);
    add(allow_safe_mode);
    CHECK(!allowed.count(sm::features_file_name));

    const std::vector<literal> found = scan_tree(root);
    std::set<std::string> seen;
    std::size_t unlisted = 0, full = 0;
    for (const literal& l : found)
    {
        seen.insert(l.name);
        if (!l.bare) full++;
        if (rows.count(l.name) || allowed.count(l.name)) continue;
        std::printf("FAIL inventory: %s%s (%s) is no feature table row and on no allow-list\n", l.bare ? "" : "/data/rpcs3/", l.name.c_str(),
            l.where.c_str());
        unlisted++;
    }
    g_checks++;
    if (unlisted) g_failures++;
    CHECK(full > 100);                       // the scan really ran over the tree
    CHECK(seen.count(sm::global_flag_name)); // and its bare-name pass too (the safe mode's own file has no path literal)
    // An allow-list entry without a literal hides nothing today but would let a switch come back unreviewed.
    std::size_t stale = 0;
    for (const std::string& name : allowed)
    {
        if (seen.count(name)) continue;
        std::printf("FAIL inventory: allow-listed %s is no longer in the tree (remove it from its list)\n", name.c_str());
        stale++;
    }
    g_checks++;
    if (stale) g_failures++;
    std::printf("inventory: %zu literals (%zu paths, %zu bare names), %zu distinct names; %zu row flags, %zu allow-listed (%zu not in the tree now)\n",
        found.size(), full, found.size() - full, seen.size(), rows.size(), allowed.size(), stale);
}
} // namespace

void ps4_startup_checkpoint(const char* stage)
{
    g_lines.emplace_back(stage ? stage : "");
}

int main(int argc, char** argv)
{
    std::error_code ec;
    const stdfs::path root = stdfs::absolute(argc > 1 ? stdfs::path(argv[1]) : stdfs::current_path(), ec);
    const stdfs::path work = stdfs::temp_directory_path(ec) / ("safe_mode_test_" + std::to_string(static_cast<unsigned long long>(
        std::chrono::steady_clock::now().time_since_epoch().count() & 0xffffffffu)));
    stdfs::create_directories(work, ec);
    stdfs::current_path(work, ec);
    if (ec)
    {
        std::printf("FAIL: cannot work in %s\n", work.generic_string().c_str());
        return 1;
    }

    test_table();
    test_compute();
    test_kind_kept();
    test_rows_040();
    test_features_text();
    test_files();
    test_title_features();
    test_safe_status();
    test_escalation();
    test_rows_040_files();
    test_cb_split_key();
    test_inventory(root);

    stdfs::current_path(root, ec);
    stdfs::remove_all(work, ec);
    std::printf("%s: %d checks, %d failures\n", g_failures ? "safe_mode_test FAILED" : "safe_mode_test OK", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
