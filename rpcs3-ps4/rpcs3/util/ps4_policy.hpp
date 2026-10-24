#pragma once

// 0.29: 'Configuração automática' - the PS4 automatic settings profile.
// Shared by Emu/System.cpp (fixup_settings applies it to g_cfg on every boot) and the native settings menu
// (util/ps4_settings.hpp previews it, so the menu shows the values that actually run). Console-only and
// header-only: System.cpp includes it under RPCS3_PS4.
//
// 0.31: runtime limits, applied after the per-game YAML in every mode except keep-user-settings.flag:
// - the online switch (online-rpcn.flag) is the only thing that puts a game online, and UPnP needs its own switch;
// - console safety: no /host_root/, a bounded dev_hdd1 disk cache, a video memory cap inside the GPU window;
// - per-title adaptation: a game that died from a resource limit boots with a lighter configuration for its next
//   3 boots (util/ps4_adapt.hpp).
//
// 0.32: console limits (apply_console_limits), also in every mode except keep-user-settings.flag: options that are
// unsafe or do nothing on this console keep their upstream default, and the compile-thread counts stay at 1-2
// (OpenGL keeps 0 shader threads, which is one worker on this CPU). A user-tuned marker whose saved values run
// exactly like the automatic configuration no longer keeps the game out of it (saved_equals_automatic).
//
// 0.33: the PC per-game config database (util/ps4_config_db.hpp, /data/rpcs3/config_database.dat when the user
// copied it). Automatic mode: profile, then the title's database entry, then the per-title rows (a setting a row
// decides keeps the row's value), then the console limits and the runtime limits. 'Configuração automática:
// Desligada' applies the entry only to a game without its own YAML and a user-tuned game never gets it, as PC
// RPCS3 skips the database for a game with a custom config. 'Use LLVM CPU' is a console limit (native CPU in every
// mode but keep-user-settings.flag), and custom_configs/<TITLE_ID>.accurate-dfma turns PC-exact DFMA on for one title.
//
// 0.34: 'Use native user interface' is a console limit (the PS4 frontend has no desktop dialogs). A title whose PC
// database entry (or its fallback row) asks 'Sleep Timers Accuracy: As Host' explicitly reports host_sleep_requested,
// and System.cpp turns lv2's precise usleep loop off for it. 'Modo frio' (cool-mode.flag) slows the pad poll in the
// automatic configuration. vk-compile-3.flag (opt-in, runtime section) gives the Vulkan pipeline compiler a third worker.
//
// 0.37: the profile runs the PC defaults for Shader Mode (Async Recompiler with Shader Interpreter: a draw whose pipeline
// is still compiling is drawn by the interpreter instead of skipped) and for ZCULL (strict sync, accurate stats); the
// hand-made escape files shader-skip-draws.flag and zcull-relaxed.flag bring back the 0.36 values. The config database
// may also come from the package (util/ps4_config_db.hpp). The menu stamps every YAML it saves with the profile version
// ('PS4 automatic profile: 37'); outside the automatic mode, a saved YAML without that stamp gets the current profile
// value for each setting that still holds the value an older profile forced (apply_profile_migration). 'Disable SPU
// GETLLAR Spin Optimization' is a console limit (pure GETLLAR busy-waiting). Sonic & SEGA All-Stars Racing's row
// follows its PC entry (Write Color Buffers on), God of War: Ascension's adds its 'Accurate ZCULL stats: false'.
//
// 0.39: a 64 ms audio reserve (cellAudio waits for a late game block while the output holds more than 16 ms); on
// OpenGL the relaxed ZCULL pair (zcull-strict-gl.flag: strict) and, with gl-async.flag, the asynchronous recompiler; a
// title whose GPU stopped responding (adapt gpu_hang) boots the Async Recompiler without the shader interpreter; the
// game language row (Configurações > Idioma dos jogos, game-language.txt and custom_configs/<TITLE_ID>.game-language)
// in the runtime section; a startup.log line and a toast when a PPU or SPU interpreter boots; menu texts and the
// adaptation toast in the interface language (util/ps4_i18n.hpp); the boot toast of the automatic configuration only
// when the title's own fixes apply (automatic_config_toast). Profile version 39 (migrates the 34 ms reserve and
// the strict ZCULL pair that 0.37-0.38 saved into OpenGL YAMLs).
//
// Settings format 2 ('PS4 settings format: 2'): the files the settings browser saves list the player's own choices under
// 'PS4 user settings:' (pins). The automatic profile skips every pinned setting, the title's PC database entry and its
// automatic / fallback rows skip the game's pins, the migration skips all pins; every-mode rows, the console limits, the
// runtime section and the hardware limit still decide. Format-1 files (no format key) run exactly as before. Four more
// console limits: Exit RPCS3 when process finishes, Assume External Debugger, Initialize Directories and Automatically
// start games after boot keep their upstream defaults.
//
// 0.40: changes for every game run the 0.38 values unless their row of util/ps4_safe_mode.hpp is turned on: the 34 ms
// audio reserve (audio_reserve_64: 64 ms), the strict ZCULL pair on OpenGL (gl_relaxed_zcull: relaxed), English (US)
// as the automatic game language (game_language_ps4, or the row value 'Idioma do PS4': the PS4 language). Profile
// version 40 migrates the 64 ms reserve and the relaxed OpenGL pair that profile 39 (0.39) saved back. The per-game
// safe mode (util/ps4_adapt.hpp safe_status) adds its boot items: the lighter thread budget after a thread failure, the
// Async Recompiler after a GPU failure, from level 2 online off, one compile thread and the title's config database
// entry skipped (config-database-off.flag skips it for every title), at level 3 the Async Recompiler without the
// shader interpreter. Modo online without an RPCN account in rpcn.yml boots offline (online-without-account.flag:
// the network stays up). The boot toast names the level and the failure.
// Also in 0.40: a thread failure keeps the title's database entry at every level; Resolution Scale runs at most 200 %
// and Audio Renderer 'Null' runs as Cubeb (console limits); the platform step (apply_platform_limits) shows what the
// renderer would replace anyway (OpenGL without gl-async.flag: the legacy recompiler); the disk cache bound follows the
// free space of /data; after a device loss the title's next boots split command buffers (adapt cb_split); a game boot
// logs which values of the player's files run otherwise ('settings:'), and a game's own format-1 values the profile
// replaces get one toast per build pointing at the Converter; legacy_choices() offers the global layer's own values to
// the settings menu, which pins them only on the player's request.
//
// Never construct a cfg_root in this header: System.cpp is compiled without HAVE_VULKAN, and cfg_root's inline
// constructor (default renderer) may only be emitted by TUs that agree on it (system_config.cpp #error, ODR).
// Do not use HAVE_VULKAN here either, for the same reason (these inline functions are shared by both TUs).
//
// Rule for every forced value: it is the upstream default, the title's entry in the PC config database (which PC
// RPCS3 applies itself; 0.33), or an earlier port decision with its measured reason quoted next to it. Nothing here
// may reduce compatibility versus upstream defaults. Fixes that only some games need go to the per-title table,
// with the reason; no speculative entries.

#include "Emu/system_config.h"
#include "Emu/system_utils.hpp"
#include "Utilities/File.h"
#include "util/sysinfo.hpp"
#include "util/ps4_adapt.hpp"
#include "util/ps4_build_version.hpp"
#include "util/ps4_config_db.hpp"
#include "util/ps4_cool_mode.hpp"
#include "util/ps4_i18n.hpp"
#include "util/ps4_safe_mode.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#ifdef RPCS3_PS4
#include <sys/statvfs.h>

#include "util/logs.hpp"

// The RPCS3.log copy of the boot's 'settings:' line (testers often send RPCS3.log alone).
LOG_CHANNEL(ps4_policy_log, "PS4");
#endif

// PS4 frontend (ps4_fatal.cpp, ps4_launcher.cpp); the same declarations as ps4_fatal.h and ps4_launcher.h.
// Only the real boot path (apply_boot_policy) calls them, never the menu preview.
void ps4_notify(const char* message) noexcept;
void ps4_startup_checkpoint(const char* stage);

namespace utils::ps4
{
namespace policy_detail
{
// Small note file. The raw write overload returns a count (the typed fs::file::write overloads xfail on a
// short write, and the generic one would write a string_view's bytes, not its text).
inline bool write_note(const std::string& path, std::string_view text)
{
    fs::file file(path, fs::rewrite);
    return file && file.write(text.data(), text.size()) == text.size();
}

// Whole text of a small settings file; empty when it is missing or larger than 'limit' (no exception path).
inline std::string read_small(const std::string& path, u64 limit = 4096)
{
    const fs::file file(path);
    if (!file) return {};
    const u64 size = file.size();
    if (size == 0 || size > limit) return {};
    std::string text(static_cast<std::size_t>(size), '\0');
    text.resize(static_cast<std::size_t>(file.read(text.data(), size)));
    return text;
}

// Flag files: present = on.
inline bool set_flag(const char* path, bool on, std::string_view note)
{
    if (!on) return !fs::is_file(path) || fs::remove_file(path);
    return fs::is_file(path) || write_note(path, note);
}
} // namespace policy_detail

// 0.33: text of a setting for the change reports and the dry runs: a scalar as its YAML text, a set (Libraries
// Control, which the config database uses) as its items joined by ", ". cfg::set_entry has no to_string() and no
// from_string(); the policy never touches maps, logs or devices.
inline std::string setting_text(const cfg::_base& setting)
{
    if (setting.get_type() != cfg::type::set) return setting.to_string();
    std::string text;
    for (const std::string& item : setting.to_list())
    {
        if (!text.empty()) text += ", ";
        text += item;
    }
    return text;
}

// Puts back a setting_text() value.
inline bool restore_setting_text(cfg::_base& setting, const std::string& text)
{
    if (setting.get_type() != cfg::type::set) return setting.from_string(text);
    std::vector<std::string> items;
    for (std::string_view rest = text; !rest.empty();)
    {
        const auto comma = rest.find(", ");
        items.emplace_back(rest.substr(0, comma));
        rest.remove_prefix(comma == std::string_view::npos ? rest.size() : comma + 2);
    }
    return setting.from_list(std::move(items));
}

// 0.26 developer opt-out for A/B tests: nothing is forced, not even the per-title fixes. Created by hand (FTP).
inline constexpr char keep_user_settings_flag[] = "/data/rpcs3/keep-user-settings.flag";
// 0.29: the menu row 'Configuração automática: Desligada' creates it, 'Ligada' removes it (global switch).
inline constexpr char auto_config_off_flag[] = "/data/rpcs3/auto-config-off.flag";
// 0.25: opt back into software DFMA on Jaguar (no FMA3/FMA4): keeps a configured 'Use Accurate DFMA: true'.
inline constexpr char accurate_dfma_flag[] = "/data/rpcs3/accurate-dfma.flag";
// 0.31: online switch (PlayStation Network > Modo online). Present: every game boots with Internet 'Connected'
// and PSN 'RPCN'; absent: 'Disconnected'. It is the only switch, so neither an old per-game YAML (menu saves
// are full dumps that carry 'Disconnected') nor a copied PC config can turn it on unnoticed.
inline constexpr char online_rpcn_flag[] = "/data/rpcs3/online-rpcn.flag";
// 0.31: UPnP port mapping on the home router. Off unless switched on in the menu (and only while online).
inline constexpr char upnp_flag[] = "/data/rpcs3/upnp.flag";
// 0.31: video memory overrides for console A/B tests, created by hand: lines 'device_mb=N' (replaces the
// automatic device-local cap) and 'host_mb=N' (host-visible heaps, read by the Vulkan allocator).
inline constexpr char vram_flag[] = "/data/rpcs3/vram.flag";
// 0.34: opt-in third Vulkan pipeline compile worker (first-viewing shader backlogs drain faster). Not a default: every
// worker is one more thread on a console that runs out of them (adapt threads=low keeps one worker).
inline constexpr char vk_compile_3_flag[] = "/data/rpcs3/vk-compile-3.flag";
// 0.37: escape files for console A/B tests, created by hand. shader-skip-draws.flag: the Vulkan profile uses 'Async
// Recompiler' again (0.27-0.36: a draw whose pipeline is still compiling is skipped) instead of the PC default with the
// shader interpreter. zcull-relaxed.flag: the 0.26-0.36 ZCULL pair (Relaxed ZCULL Sync on, Accurate ZCULL stats off)
// instead of the PC defaults. Per-title rows and config database entries still decide over both.
inline constexpr char shader_skip_draws_flag[] = "/data/rpcs3/shader-skip-draws.flag";
inline constexpr char zcull_relaxed_flag[] = "/data/rpcs3/zcull-relaxed.flag";
// 0.39: OpenGL ran the relaxed ZCULL pair by default (through Zink every strict occlusion query is a blocking
// glGetQueryObjectiv round trip on the RSX thread); this escape file keeps the strict pair there. 0.40: the relaxed
// OpenGL pair is the opt-in row gl_relaxed_zcull (gl-relaxed-zcull.flag, util/ps4_safe_mode.hpp), whose kill flag
// this file is.
inline constexpr char zcull_strict_gl_flag[] = "/data/rpcs3/zcull-strict-gl.flag";
// 0.39: an asynchronous OpenGL compile worker on a shared EGL context is available (the GL renderer reads the same
// file, util/ps4_rsx_programs.hpp); without it the GL renderer falls back to the legacy recompiler.
inline constexpr char gl_async_flag[] = "/data/rpcs3/gl-async.flag";
// 0.37: the global settings file the native menu saves (System.cpp loads it after config.yml).
inline constexpr char global_settings_path[] = "/data/rpcs3/ps4-settings.yml";

// 0.37: the Shader Mode of the automatic profile on Vulkan.
inline shader_mode profile_vulkan_shader_mode()
{
    return fs::is_file(shader_skip_draws_flag) ? shader_mode::async_recompiler : shader_mode::async_with_interpreter;
}

// 0.39: the Shader Mode on OpenGL: the legacy recompiler unless gl-async.flag provides the asynchronous worker.
inline shader_mode profile_opengl_shader_mode()
{
    return fs::is_file(gl_async_flag) ? shader_mode::async_recompiler : shader_mode::recompiler;
}

// 0.37: the profile runs Relaxed ZCULL Sync on and Accurate ZCULL stats off (zcull-relaxed.flag).
inline bool profile_zcull_relaxed()
{
    return fs::is_file(zcull_relaxed_flag);
}

// 0.40: the feature rows (util/ps4_safe_mode.hpp) the policy decides with when the caller has no runtime_inputs: the
// flag files, features.txt, the global safe mode and the title's safe-mode level now (defined after
// read_runtime_inputs). An empty title gives the files at level 0.
inline u64 policy_features(std::string_view title_id = {});

// 0.39: the same for one renderer: OpenGL also runs the relaxed pair with the feature mask's gl_relaxed_zcull (0.40:
// off by default, the 0.38 strict pair; zcull-strict-gl.flag is its kill flag).
inline bool profile_zcull_relaxed(video_renderer renderer, u64 features)
{
    return profile_zcull_relaxed() || (renderer == video_renderer::opengl && safe_mode::enabled_in(features, safe_mode::feature::gl_relaxed_zcull));
}

inline bool profile_zcull_relaxed(video_renderer renderer)
{
    return profile_zcull_relaxed(renderer, policy_features());
}

// 0.37: version of the automatic profile. The settings menu appends 'PS4 automatic profile: N' to every YAML it saves
// (cfg skips unknown keys when it loads a config); a saved YAML without it was written by profile 36 or older.
// 0.39: 39 (0.37 and 0.38 wrote 37). 0.40: 40 (the 34 ms reserve and the strict OpenGL pair are the defaults again).
inline constexpr unsigned automatic_profile_version = 40;
inline constexpr std::string_view profile_stamp_key = "PS4 automatic profile";

namespace policy_detail
{
// The number of the top-level 'key: N' line of a YAML text (quotes, blanks and CR allowed around N); 0 when absent.
inline unsigned top_level_number(std::string_view yaml, std::string_view key)
{
    while (!yaml.empty())
    {
        const auto nl = yaml.find('\n');
        std::string_view line = yaml.substr(0, nl);
        yaml.remove_prefix(nl == std::string_view::npos ? yaml.size() : nl + 1);
        if (!line.starts_with(key) || line.substr(key.size(), 1) != ":") continue;
        line.remove_prefix(key.size() + 1);
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r')) line.remove_suffix(1);
        if (line.size() >= 2 && (line.front() == '"' || line.front() == '\'') && line.back() == line.front())
            line = line.substr(1, line.size() - 2);
        unsigned version = 0;
        bool ok = !line.empty();
        for (const char c : line)
        {
            if (c < '0' || c > '9' || version > 100000) { ok = false; break; }
            version = version * 10 + static_cast<unsigned>(c - '0');
        }
        if (ok) return version;
    }
    return 0;
}
} // namespace policy_detail

// The profile version a saved YAML text carries; 0 when it has no stamp.
inline unsigned saved_profile_version(std::string_view yaml)
{
    return policy_detail::top_level_number(yaml, profile_stamp_key);
}

// The text a menu save writes: the YAML plus the current stamp as its last top-level key.
inline std::string with_profile_stamp(std::string yaml)
{
    if (!yaml.empty() && yaml.back() != '\n') yaml += '\n';
    yaml += profile_stamp_key;
    yaml += ": " + std::to_string(automatic_profile_version) + "\n";
    return yaml;
}

// Settings file format. 2: the file lists the player's own choices under 'PS4 user settings:' (pins: the automatic
// configuration leaves those settings alone) and a per-game file holds only those choices (sparse; cfg loads it over the
// global file like a full dump, and so does PC RPCS3). Absent = 1: a full dump (older menu saves, PC copies, home-menu
// saves) with the user-tuned marker, the profile migration and the database skip of a game with its own YAML.
inline constexpr unsigned settings_format_version = 2;
inline constexpr std::string_view settings_format_key = "PS4 settings format";
inline constexpr std::string_view user_settings_key = "PS4 user settings";
// Bounds of the 'PS4 user settings:' list: keys kept and bytes of key text
inline constexpr std::size_t user_settings_max_keys = 1024;
inline constexpr std::size_t user_settings_max_bytes = 64 * 1024;
// Largest settings file read for the stamps and the list (the global file is a full dump)
inline constexpr u64 settings_text_limit = u64{1} << 20;

// The format of a settings YAML text: 1 without the format key.
inline unsigned saved_settings_format(std::string_view yaml)
{
    const unsigned format = policy_detail::top_level_number(yaml, settings_format_key);
    return format ? format : 1;
}

namespace policy_detail
{
inline std::string_view trim_blanks(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

// One YAML scalar at the start of 'text': double quoted (\" \\ \t \n \/ escapes), single quoted ('' = '), or plain (up to
// 'stop', a ' #' comment or the end). 'text' is advanced past it; false for an unterminated quote.
inline bool take_scalar(std::string_view& text, std::string& out, std::string_view stop)
{
    out.clear();
    text = trim_blanks(text);
    if (!text.empty() && text.front() == '"')
    {
        for (std::size_t i = 1; i < text.size(); i++)
        {
            const char c = text[i];
            if (c == '"') { text.remove_prefix(i + 1); return true; }
            if (c != '\\' || i + 1 == text.size()) { out += c; continue; }
            const char e = text[++i];
            out += e == 't' ? '\t' : e == 'n' ? '\n' : e;
        }
        return false;
    }
    if (!text.empty() && text.front() == '\'')
    {
        for (std::size_t i = 1; i < text.size(); i++)
        {
            if (text[i] != '\'') { out += text[i]; continue; }
            if (i + 1 < text.size() && text[i + 1] == '\'') { out += '\''; i++; continue; }
            text.remove_prefix(i + 1);
            return true;
        }
        return false;
    }
    std::size_t end = text.size();
    if (const auto hash = text.find(" #"); hash != std::string_view::npos) end = hash;
    if (const auto at = text.substr(0, end).find_first_of(stop); at != std::string_view::npos) end = at;
    out = std::string(trim_blanks(text.substr(0, end)));
    text.remove_prefix(end);
    return true;
}
} // namespace policy_detail

// The keys listed under the top-level 'PS4 user settings:' of a settings YAML text, in order and without duplicates:
// block items ('  - "Video > Write Color Buffers"', quoted or plain), '#' comment lines, trailing ' #' comments and CRLF;
// or a flow list on the key's line ('[]', '["a", "b"]'). The list ends at the first line that is not an item.
inline std::vector<std::string> parse_user_settings(std::string_view yaml)
{
    std::vector<std::string> keys;
    std::size_t bytes = 0;
    const auto keep = [&](std::string key)
    {
        if (key.empty() || key == "~" || key == "null" || keys.size() >= user_settings_max_keys || bytes + key.size() > user_settings_max_bytes) return;
        if (std::find(keys.begin(), keys.end(), key) != keys.end()) return;
        bytes += key.size();
        keys.push_back(std::move(key));
    };
    bool found = false;
    while (!yaml.empty())
    {
        const auto nl = yaml.find('\n');
        std::string_view line = yaml.substr(0, nl);
        yaml.remove_prefix(nl == std::string_view::npos ? yaml.size() : nl + 1);
        if (!found)
        {
            if (!line.starts_with(user_settings_key)) continue;
            std::string_view rest = policy_detail::trim_blanks(line.substr(user_settings_key.size()));
            if (rest.empty() || rest.front() != ':') continue;
            found = true;
            rest = policy_detail::trim_blanks(rest.substr(1));
            if (rest.empty() || rest.front() == '#') continue; // block items follow
            if (rest.front() != '[') return keys;              // '~', 'null' or a scalar: no list
            rest.remove_prefix(1);
            std::string key;
            while (true)
            {
                rest = policy_detail::trim_blanks(rest);
                if (rest.empty() || rest.front() == ']') return keys;
                if (!policy_detail::take_scalar(rest, key, ",]")) return keys;
                keep(key);
                rest = policy_detail::trim_blanks(rest);
                if (rest.empty() || rest.front() != ',') return keys;
                rest.remove_prefix(1);
            }
        }
        const std::string_view item = policy_detail::trim_blanks(line);
        if (item.empty() || item.front() == '#') continue;
        if (item != "-" && !item.starts_with("- ")) break;
        std::string_view rest = item.substr(1);
        std::string key;
        if (policy_detail::take_scalar(rest, key, {})) keep(std::move(key));
    }
    return keys;
}

namespace policy_detail
{
// 'yaml' without its top-level PS4 stamp keys (and the list items under 'PS4 user settings:'), so a save never writes
// them twice; an empty document ('{}', '~', 'null') becomes empty text.
inline std::string strip_ps4_keys(std::string_view yaml)
{
    const auto empty_document = [](std::string_view s)
    {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) s.remove_prefix(1);
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.remove_suffix(1);
        return s.empty() || s == "{}" || s == "~" || s == "null";
    };
    if (empty_document(yaml)) return {};
    std::string out;
    out.reserve(yaml.size());
    bool in_list = false;
    while (!yaml.empty())
    {
        const auto nl = yaml.find('\n');
        const std::string_view line = yaml.substr(0, nl);
        yaml.remove_prefix(nl == std::string_view::npos ? yaml.size() : nl + 1);
        const auto is_key = [&](std::string_view key)
        {
            return line.starts_with(key) && trim_blanks(line.substr(key.size())).starts_with(":");
        };
        if (in_list)
        {
            const std::string_view item = trim_blanks(line);
            if (item.empty() || item.front() == '#' || item == "-" || item.starts_with("- ")) continue;
            in_list = false;
        }
        if (is_key(user_settings_key))
        {
            const std::string_view rest = trim_blanks(trim_blanks(line.substr(user_settings_key.size())).substr(1));
            in_list = rest.empty() || rest.front() == '#';
            continue;
        }
        if (is_key(profile_stamp_key) || is_key(settings_format_key)) continue;
        out += line;
        out += '\n';
    }
    if (empty_document(out)) return {};
    return out;
}

// A key as a double-quoted YAML scalar
inline std::string quoted_key(std::string_view key)
{
    std::string out = "\"";
    for (const char c : key)
    {
        if (c == '"' || c == '\\') out += '\\';
        if (static_cast<unsigned char>(c) >= 0x20) out += c;
    }
    return out + "\"";
}
} // namespace policy_detail

// The text a settings save writes: the YAML, then the profile stamp, 'PS4 settings format: 2' and the player's choices
// ('PS4 user settings:', block list, '[]' when empty) as its last top-level keys. cfg and PC RPCS3 skip unknown keys when
// they load a config.
inline std::string with_settings_stamp(std::string yaml, const std::vector<std::string>& pins)
{
    std::string text = policy_detail::strip_ps4_keys(yaml);
    text = with_profile_stamp(std::move(text));
    text += settings_format_key;
    text += ": " + std::to_string(settings_format_version) + "\n";
    text += user_settings_key;
    if (pins.empty()) return text + ": []\n";
    text += ":\n";
    std::vector<std::string_view> written;
    for (const std::string& key : pins)
    {
        if (key.empty() || std::find(written.begin(), written.end(), std::string_view(key)) != written.end()) continue;
        written.emplace_back(key);
        text += "  - " + policy_detail::quoted_key(key) + "\n";
    }
    return text;
}

// The player's own choices: the keys of 'PS4 user settings:' in ps4-settings.yml (global) and in the game's
// custom_configs/config_<TITLE_ID>.yml, each read only from a format-2 file. A key is the setting's node names joined
// with " > " ("Video > Vulkan > VRAM allocation limit (MB)", setting_key).
struct pin_set
{
    std::vector<std::string> global, game;
    // Profile stamp of the file each list came from (unpin_on_profile); pins being edited count as current
    unsigned global_profile = automatic_profile_version, game_profile = automatic_profile_version;

    bool empty() const { return global.empty() && game.empty(); }
};

// A pin the automatic configuration stops honouring: when a profile version finds a value unsafe for one setting, it
// lists it here, and a file stamped at or below 'until' that pins 'key' holding 'value' runs the automatic value again
// (startup.log names it). Empty while no profile has needed it.
struct pin_revoke
{
    std::string_view key, value;
    unsigned until;
};

inline constexpr std::array<pin_revoke, 0> unpin_on_profile{};

// The node names of a setting joined with " > ", without the root's (empty) name: the key of the settings browser, the
// pins and the startup.log lines.
inline std::string setting_key(const cfg::_base& setting)
{
    std::string key = setting.get_name();
    for (const cfg::_base* parent = setting.get_parent(); parent; parent = parent->get_parent())
        if (!parent->get_name().empty()) key = parent->get_name() + " > " + key;
    return key;
}

// The pins of a settings YAML text (none below format 2).
inline std::vector<std::string> pins_of(std::string_view yaml)
{
    if (yaml.empty() || saved_settings_format(yaml) < settings_format_version) return {};
    return parse_user_settings(yaml);
}

inline pin_set read_pins(std::string_view title_id)
{
    pin_set pins;
    const std::string global = policy_detail::read_small(global_settings_path, settings_text_limit);
    pins.global = pins_of(global);
    pins.global_profile = saved_profile_version(global);
    if (!title_id.empty())
    {
        const std::string game = policy_detail::read_small(rpcs3::utils::get_custom_config_path(std::string(title_id)), settings_text_limit);
        pins.game = pins_of(game);
        pins.game_profile = saved_profile_version(game);
    }
    return pins;
}

// Pins resolved against one cfg tree (pointers into it): what the policy steps test. 'unknown' = listed keys that name no
// setting of this build (ignored; one startup.log line per game boot).
struct pinned_settings
{
    std::vector<const cfg::_base*> global, game;
    std::vector<std::string> unknown;
    std::vector<std::string> revoked; // "key=value" pins unpin_on_profile no longer honours

    bool empty() const { return global.empty() && game.empty(); }
    bool for_game(const cfg::_base& setting) const { return std::find(game.begin(), game.end(), &setting) != game.end(); }
    bool any(const cfg::_base& setting) const
    {
        return for_game(setting) || std::find(global.begin(), global.end(), &setting) != global.end();
    }
};

namespace policy_detail
{
inline void collect_leaves(const std::vector<cfg::_base*>& nodes, std::vector<const cfg::_base*>& out)
{
    for (const cfg::_base* node : nodes)
    {
        if (!node) continue;
        if (node->get_type() == cfg::type::node) collect_leaves(static_cast<const cfg::node*>(node)->get_nodes(), out);
        else out.push_back(node);
    }
}

// Null when nothing is pinned, so the policy steps run exactly as for a format-1 file.
inline const pinned_settings* pins_or_null(const pinned_settings& pins)
{
    return pins.empty() ? nullptr : &pins;
}
} // namespace policy_detail

// 'revokes' = unpin_on_profile (a test passes its own); 'cfg' holds the loaded values the revokes compare.
inline pinned_settings resolve_pins(const cfg_root& cfg, const pin_set& pins, std::span<const pin_revoke> revokes = unpin_on_profile)
{
    pinned_settings out;
    if (pins.empty()) return out;
    std::vector<const cfg::_base*> leaves;
    policy_detail::collect_leaves(cfg.get_nodes(), leaves);
    std::vector<std::string> keys;
    keys.reserve(leaves.size());
    for (const cfg::_base* leaf : leaves) keys.push_back(setting_key(*leaf));
    const auto resolve = [&](const std::vector<std::string>& list, unsigned profile, std::vector<const cfg::_base*>& into)
    {
        for (const std::string& key : list)
        {
            const auto it = std::find(keys.begin(), keys.end(), key);
            if (it == keys.end())
            {
                if (std::find(out.unknown.begin(), out.unknown.end(), key) == out.unknown.end()) out.unknown.push_back(key);
                continue;
            }
            const cfg::_base* setting = leaves[static_cast<std::size_t>(it - keys.begin())];
            const auto revoked = std::find_if(revokes.begin(), revokes.end(), [&](const pin_revoke& r)
            { return r.key == key && profile <= r.until && setting_text(*setting) == r.value; });
            if (revoked != revokes.end())
            {
                out.revoked.push_back(key + "=" + std::string(revoked->value));
                continue;
            }
            if (std::find(into.begin(), into.end(), setting) == into.end()) into.push_back(setting);
        }
    };
    resolve(pins.global, pins.global_profile, out.global);
    resolve(pins.game, pins.game_profile, out.game);
    return out;
}

// 0.31: limits of the runtime section (see apply_runtime_limits).
// The orbis-drm GPU window is 991 MiB; RADV keeps its own objects (pipelines, descriptor pools, fences) in the
// same window, so RPCS3's device-local allocations stop well before it.
inline constexpr u32 vram_cap_mb = 768;
inline constexpr u32 vram_low_cap_mb = 448;        // after a game ran out of video or direct memory
inline constexpr u32 vram_min_mb = 256, vram_max_mb = 65536; // 'VRAM allocation limit (MB)' bounds
// dev_hdd1/caches holds the disc data games cache for themselves (a real PS3 has about 2 GB for it). The boot
// trims it oldest-first above the limit, so it can never fill the console's drive. Below the floor, a game
// would lose its cache on every boot and copy it again.
inline constexpr s64 disk_cache_max_mb = 4096;
inline constexpr s64 disk_cache_floor_mb = 1024;
// 0.40: the upper bound follows the free space of /data (row disk_cache_order; disk-cache-upstream.flag keeps 4096 MB):
// 10 % of it, at least 4096 MB and at most 16384 MB, so first-boot PPU builds (10-59 min each) survive longer on a
// console with room. Disk use is not guest-visible. The setting itself holds at most disk_cache_setting_max_mb.
inline constexpr s64 disk_cache_cap_max_mb = 16384;
inline constexpr s64 disk_cache_setting_max_mb = 10240;

// The cap for 'data_free_mb' MB free on /data (0 = unknown: 4096 MB)
constexpr s64 disk_cache_cap_mb(u64 data_free_mb) noexcept
{
    const u64 tenth = data_free_mb / 10;
    if (tenth <= static_cast<u64>(disk_cache_max_mb)) return disk_cache_max_mb;
    return tenth >= static_cast<u64>(disk_cache_cap_max_mb) ? disk_cache_cap_max_mb : static_cast<s64>(tenth);
}

// Free MB on /data now, 0 when unknown (statvfs refused, or numbers that cannot be right: the same plausibility rules
// as the health checks). Host builds always answer 0.
inline u64 data_free_mb() noexcept
{
#ifdef RPCS3_PS4
    struct ::statvfs st{};
    if (::statvfs("/data/rpcs3/", &st) != 0) return 0;
    const u64 unit = st.f_frsize ? static_cast<u64>(st.f_frsize) : static_cast<u64>(st.f_bsize);
    const u64 blocks = static_cast<u64>(st.f_blocks), avail = static_cast<u64>(st.f_bavail);
    if (!unit || (unit & (unit - 1)) || blocks > ~u64{0} / unit || unit * blocks < (u64{1} << 30) || avail > blocks) return 0;
    return (unit * avail) >> 20;
#else
    return 0;
#endif
}

inline bool online_enabled()
{
    return fs::is_file(online_rpcn_flag);
}

// Creates or deletes online-rpcn.flag. Returns false if the file system refused.
inline bool set_online_enabled(bool enabled)
{
    return policy_detail::set_flag(online_rpcn_flag, enabled, "Created by PlayStation Network > Modo online (RPCN): games boot "
        "with Internet 'Connected' and PSN 'RPCN'. Delete it, or switch it off in the menu, to stay offline.\n");
}

inline bool upnp_enabled()
{
    return fs::is_file(upnp_flag);
}

// Creates or deletes upnp.flag. Returns false if the file system refused.
inline bool set_upnp_enabled(bool enabled)
{
    return policy_detail::set_flag(upnp_flag, enabled, "Created by PlayStation Network > UPnP: while online, RPCS3 asks the "
        "router to open the game's ports. Delete it, or switch it off in the menu, to stop that.\n");
}

struct vram_override
{
    u32 device_mb = 0; // 0 = automatic
    u32 host_mb = 0;   // 0 = no cap on host-visible heaps
};

// Parses vram.flag text: 'device_mb=N' and 'host_mb=N' lines; blanks, '#' comments, CRLF and anything else are
// ignored. Values above 65536 are treated as absent.
inline vram_override parse_vram_flag(std::string_view text)
{
    vram_override out;
    while (!text.empty())
    {
        const auto nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text.remove_prefix(nl == std::string_view::npos ? text.size() : nl + 1);
        const auto trim = [](std::string_view s)
        {
            while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
            while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
            return s;
        };
        line = trim(line);
        const auto eq = line.find('=');
        if (line.empty() || line.front() == '#' || eq == std::string_view::npos) continue;
        const std::string_view key = trim(line.substr(0, eq));
        const std::string_view value = trim(line.substr(eq + 1));
        u64 number = 0;
        bool ok = !value.empty();
        for (const char c : value)
        {
            if (c < '0' || c > '9' || number > vram_max_mb) { ok = false; break; }
            number = number * 10 + static_cast<u64>(c - '0');
        }
        if (!ok || number > vram_max_mb) continue;
        if (key == "device_mb") out.device_mb = static_cast<u32>(number);
        else if (key == "host_mb") out.host_mb = static_cast<u32>(number);
    }
    return out;
}

inline vram_override read_vram_flag()
{
    return parse_vram_flag(policy_detail::read_small(vram_flag));
}

// 0.29: per-game 'user-tuned' marker (custom_configs/<TITLE_ID>.user-tuned). The native menu writes it only when
// a per-game config is saved whose values differ from the automatic profile; while it exists, that game's
// YAML runs as saved. 'Voltar para automático' deletes it. 0.32: a marker whose YAML runs exactly like the
// automatic configuration is ignored at boot (saved_equals_automatic).
inline std::string user_tuned_marker(std::string_view title_id)
{
    if (title_id.empty()) return {};
    return rpcs3::utils::get_custom_config_dir() + std::string(title_id) + ".user-tuned";
}

inline bool is_user_tuned(std::string_view title_id)
{
    return !title_id.empty() && fs::is_file(user_tuned_marker(title_id));
}

// Creates or deletes the marker. Returns false if the file system refused.
inline bool set_user_tuned(std::string_view title_id, bool tuned)
{
    const std::string marker = user_tuned_marker(title_id);
    if (marker.empty()) return false;
    if (!tuned) return !fs::is_file(marker) || fs::remove_file(marker);
    if (fs::is_file(marker)) return true;
    fs::create_path(rpcs3::utils::get_custom_config_dir());
    static constexpr std::string_view text = "Saved from the PS4 settings menu: config_<TITLE_ID>.yml runs as saved (automatic "
        "configuration off for this game). Delete this file, or use 'Voltar para automatico', to return to automatic.\n";
    return policy_detail::write_note(marker, text);
}

// 0.33: per-game opt-in to PC-exact DFMA (custom_configs/<TITLE_ID>.accurate-dfma). Jaguar has no FMA, so the boot
// keeps 'Use Accurate DFMA' off (unfused PPU fmadd* / SPU DFMA*, two roundings); a title that needs the fused result
// runs with it on, through software fma(), without slowing every game as accurate-dfma.flag would. Written by a
// per-game menu save with the option on, or by hand.
inline std::string accurate_dfma_marker(std::string_view title_id)
{
    if (title_id.empty()) return {};
    return rpcs3::utils::get_custom_config_dir() + std::string(title_id) + ".accurate-dfma";
}

inline bool title_accurate_dfma(std::string_view title_id)
{
    return !title_id.empty() && fs::is_file(accurate_dfma_marker(title_id));
}

// Creates or deletes the marker. Returns false if the file system refused.
inline bool set_title_accurate_dfma(std::string_view title_id, bool on)
{
    const std::string marker = accurate_dfma_marker(title_id);
    if (marker.empty()) return false;
    if (!on) return !fs::is_file(marker) || fs::remove_file(marker);
    if (fs::is_file(marker)) return true;
    fs::create_path(rpcs3::utils::get_custom_config_dir());
    static constexpr std::string_view text = "Saved from the PS4 settings menu: 'Use Accurate DFMA' runs on for this game (PC-exact "
        "fused multiply-add through software fma(), slower on Jaguar). Delete this file, or switch it off in the game's menu.\n";
    return policy_detail::write_note(marker, text);
}

inline bool auto_config_enabled()
{
    return !fs::is_file(auto_config_off_flag);
}

// Creates or deletes auto-config-off.flag. Returns false if the file system refused.
inline bool set_auto_config_enabled(bool enabled)
{
    if (enabled) return !fs::is_file(auto_config_off_flag) || fs::remove_file(auto_config_off_flag);
    static constexpr std::string_view text = "Created by the PS4 settings menu ('Configuracao automatica: Desligada'): "
        "saved settings run as saved. Delete it or switch it back on in the menu.\n";
    return fs::is_file(auto_config_off_flag) || policy_detail::write_note(auto_config_off_flag, text);
}

enum class policy_mode
{
    automatic,          // automatic profile + per-title fixes (+ console limits from 0.32)
    user_tuned,         // this game's YAML as saved from the menu + per-title fixes (+ console limits)
    auto_config_off,    // global switch off: saved YAMLs as saved + per-title fixes (+ console limits)
    keep_user_settings, // developer flag: nothing forced at all
};

inline policy_mode get_policy_mode(std::string_view title_id)
{
    if (fs::is_file(keep_user_settings_flag)) return policy_mode::keep_user_settings;
    if (!auto_config_enabled()) return policy_mode::auto_config_off;
    if (is_user_tuned(title_id)) return policy_mode::user_tuned;
    return policy_mode::automatic;
}

// Short status for the menu, in the interface language (static text).
inline const char* policy_mode_text(policy_mode mode)
{
    using i18n::str;
    switch (mode)
    {
    case policy_mode::automatic: return i18n::tr(str::COM_CONFIGURACAO_AUTOMATICA);
    case policy_mode::user_tuned: return i18n::tr(str::POL_AJUSTE_MANUAL_SALVO);
    case policy_mode::auto_config_off: return i18n::tr(str::POL_CONFIGURACAO_AUTOMATICA_DESLIGADA);
    case policy_mode::keep_user_settings: return i18n::tr(str::POL_KEEP_USER_SETTINGS_FLAG);
    }
    return "";
}

// 0.31: where a forced value comes from. Callbacks may take it as a 4th argument; the 3-argument form
// (setting, old value, why) still works, with 'why' empty only for the automatic profile.
enum class change_kind
{
    profile, // automatic profile (automatic mode only)
    title,   // per-title table; 0.33: also the title's PC config database entry (reason: is_database_reason)
    adapt,   // per-title adaptation after a resource failure (util/ps4_adapt.hpp)
    runtime, // online switch, video memory cap, console safety
    console, // 0.32: option unavailable on PS4, or a compile-thread limit (apply_console_limits)
    migration, // 0.37: a saved YAML's value that an older automatic profile forced (apply_profile_migration)
};

namespace policy_detail
{
template <typename OnChange>
void report(OnChange& on_change, const cfg::_base& setting, const std::string& old, std::string_view why, change_kind kind)
{
    if constexpr (std::is_invocable_v<OnChange&, const cfg::_base&, const std::string&, std::string_view, change_kind>)
        on_change(setting, old, why, kind);
    else
        on_change(setting, old, why);
}
} // namespace policy_detail

// 0.29: per-title compatibility table. Each row: title id -> overrides (setting + value in its YAML text form)
// + the reason + its scope. Only breakage seen on this port (or the same game in another region), or settings
// verified on the console for that game, belong here; no speculative entries. The menu previews them too, so
// it shows what runs. To add a title: an accessor in policy_detail, an override list, and one row per region.
struct title_override
{
    cfg::_base& (*setting)(cfg_root&);
    std::string_view value;
};

enum class title_scope
{
    // Crash fix: applied in every mode except keep-user-settings.flag, so it also holds for games saved from the
    // menu and with the automatic profile off.
    every_mode,
    // 0.31: settings verified for this game, used instead of the automatic profile's values for it. A per-game
    // save from the menu (user-tuned) or the switch 'Configuração automática: Desligada' runs the YAML as saved.
    automatic,
    // 0.33: the values of the title's PC config_database.dat entry, kept here for a console without that file (or
    // with a copy that lacks the title, or an entry this build rejects). Automatic mode only, and only while
    // the config database in use (0.37: the /data copy or the bundled one) has no usable entry for the title: a newer
    // database decides instead.
    database_fallback,
};

struct title_compat
{
    std::string_view title_id;
    std::span<const title_override> overrides;
    std::string_view reason;
    title_scope scope = title_scope::every_mode;
    // 0.34: the PC entry this row copies writes 'Sleep Timers Accuracy: As Host'. The value equals the profile's, so
    // the row cannot carry it as an override; while the row runs, lv2's precise usleep loop stays off for the title.
    bool host_sleep = false;
};

namespace policy_detail
{
inline cfg::_base& spu_accurate_reservations(cfg_root& cfg) { return cfg.core.spu_accurate_reservations; }
inline cfg::_base& renderer_of(cfg_root& cfg) { return cfg.video.renderer; }
inline cfg::_base& shader_mode_of(cfg_root& cfg) { return cfg.video.shadermode; }
inline cfg::_base& resolution_of(cfg_root& cfg) { return cfg.video.resolution; }
inline cfg::_base& write_color_buffers_of(cfg_root& cfg) { return cfg.video.write_color_buffers; }

// Demon's Souls: with Accurate SPU Reservations off (0.26 forced it off) its PPU/SPU-shared sync objects broke
// right after the _synth2 SPU group started (sys_mutex_lock EDEADLK, _sys_lwmutex_lock ESRCH on several
// threads). Off also switches 10 cellSpurs workload calls to forced HLE (cellSpurs.cpp:5578-5592).
inline constexpr title_override demons_souls[] = {{spu_accurate_reservations, "true"}};

// 0.32: Demon's Souls reads its color buffers back from the CPU (access-violation flushes from its 'HighGraphics'
// PPU thread). On 0.31 (Vulkan, 720p) it reached gameplay at ~30 FPS with black/corrupted graphics while Write
// Color Buffers was off (automatic profile), and rendered correctly with it on (per-game save). Automatic scope, not
// every mode: the write-back runs the GPU readback path, so a player can still save it off for this game.
inline constexpr title_override demons_souls_graphics[] = {{write_color_buffers_of, "true"}};

// 0.31: Ben 10 played on this console in 0.28 with OpenGL (Zink), the legacy single-threaded shader recompiler
// and 720x480i output (26-60 FPS, no error). Its earlier Vulkan runs flickered or fell to 6 FPS with an RSX
// backlog, and the automatic profile (Vulkan, 720p, async shaders) never ran with it. The legacy recompiler stays
// because asynchronous GL compilation needs extra shared EGL contexts. Only add a game here with a console run
// of exactly these values (Terraria, for example, only ever ran on Vulkan at 720p).
inline constexpr title_override opengl_verified[] = {
    {renderer_of, "OpenGL"},
    {shader_mode_of, "Legacy Recompiler (single-threaded)"},
    {resolution_of, "720x480i"},
};

inline cfg::_base& rsx_accurate_res_access_of(cfg_root& cfg) { return cfg.core.rsx_accurate_res_access; }
inline cfg::_base& relaxed_zcull_sync_of(cfg_root& cfg) { return cfg.video.relaxed_zcull_sync; }
inline cfg::_base& spu_block_size_of(cfg_root& cfg) { return cfg.core.spu_block_size; }

inline cfg::_base& precise_zpass_count_of(cfg_root& cfg) { return cfg.video.precise_zpass_count; }

// 0.33: God of War: Ascension, its PC config_database.dat entry (2026-07-16 copy, the same for all six serials):
// Write Color Buffers on, Accurate RSX reservation access on (read by the SPU LLVM reservation path) and Relaxed ZCULL
// Sync off. Sleep Timers 'As Host' is the profile's value, but on this port the profile still serves guest usleeps
// <= 100 us with a yield loop (lv2 precise sleep); the entry's explicit 'As Host' turns that loop off for the title
// (0.34, the rows' host_sleep flag): its post-process waits spun about 0.9 of a core in usleep(30) polls. 0.37: the
// profile runs accurate ZCULL stats (PC default), so the entry's 'Accurate ZCULL stats: false' is a row value now.
// Not run on this console yet (database_fallback scope).
inline constexpr title_override gow_ascension[] = {
    {write_color_buffers_of, "true"},
    {rsx_accurate_res_access_of, "true"},
    {relaxed_zcull_sync_of, "false"},
    {precise_zpass_count_of, "false"},
};

// 0.33: God of War III, its PC config_database.dat entry: Write Color Buffers on, SPU Block Size Mega. Its
// asynchronous texture streaming is unavailable on PS4 (console limit).
inline constexpr title_override gow_3[] = {
    {write_color_buffers_of, "true"},
    {spu_block_size_of, "Mega"},
};

inline cfg::_base& msaa_of(cfg_root& cfg) { return cfg.video.antialiasing_level; }
inline cfg::_base& disable_vertex_cache_of(cfg_root& cfg) { return cfg.video.disable_vertex_cache; }

// 0.34: Sonic & SEGA All-Stars Racing, its PC config_database.dat entry: MSAA Disabled (a console run used 119 % of the
// video memory window with MSAA on). 0.37: and the entry's Write Color Buffers on, as PC runs it (the readbacks may cost
// frames on this port).
inline constexpr title_override sonic_allstars_racing[] = {
    {msaa_of, "Disabled"},
    {write_color_buffers_of, "true"},
};

// 0.34: Silent Hill: Downpour, its PC config_database.dat entry: Disable Vertex Cache on.
inline constexpr title_override silent_hill_downpour[] = {{disable_vertex_cache_of, "true"}};
} // namespace policy_detail

inline constexpr title_compat title_compat_table[] =
{
    {"BLUS30443", policy_detail::demons_souls, "Demon's Souls: sync objects broke with Accurate SPU Reservations off (0.26, verified)"},
    {"BLES00932", policy_detail::demons_souls, "Demon's Souls EU (region ID not checked on the console): same fix as BLUS30443"},
    {"BCJS30022", policy_detail::demons_souls, "Demon's Souls JP (region ID not checked on the console): same fix as BLUS30443"},
    {"BCAS20071", policy_detail::demons_souls, "Demon's Souls Asia (region ID not checked on the console): same fix as BLUS30443"},
    {"BLUS30443", policy_detail::demons_souls_graphics, "Demon's Souls: Write Color Buffers on (black graphics with it off, correct with it on; 0.31 console runs)", title_scope::automatic},
    {"BLES00932", policy_detail::demons_souls_graphics, "Demon's Souls EU (region ID not checked on the console): Write Color Buffers on, as for BLUS30443", title_scope::automatic},
    {"BCJS30022", policy_detail::demons_souls_graphics, "Demon's Souls JP (region ID not checked on the console): Write Color Buffers on, as for BLUS30443", title_scope::automatic},
    {"BCAS20071", policy_detail::demons_souls_graphics, "Demon's Souls Asia (region ID not checked on the console): Write Color Buffers on, as for BLUS30443", title_scope::automatic},
    {"BLUS30663", policy_detail::opengl_verified, "Ben 10 Ultimate Alien: Cosmic Destruction: OpenGL, legacy shaders, 720x480i (verified on console in 0.28)", title_scope::automatic},
    // 0.33: only serials that the PC database itself lists (no region guesses).
    {"BCUS98232", policy_detail::gow_ascension, "God of War: Ascension: its PC config_database.dat values (no usable database entry on the console)", title_scope::database_fallback, true},
    {"BCES01741", policy_detail::gow_ascension, "God of War: Ascension EU: its PC config_database.dat values (no usable database entry on the console)", title_scope::database_fallback, true},
    {"BCES01742", policy_detail::gow_ascension, "God of War: Ascension EU: its PC config_database.dat values (no usable database entry on the console)", title_scope::database_fallback, true},
    {"NPUA80918", policy_detail::gow_ascension, "God of War: Ascension (PSN US): its PC config_database.dat values (no usable database entry on the console)", title_scope::database_fallback, true},
    {"NPEA00445", policy_detail::gow_ascension, "God of War: Ascension (PSN EU): its PC config_database.dat values (no usable database entry on the console)", title_scope::database_fallback, true},
    {"NPHA80258", policy_detail::gow_ascension, "God of War: Ascension (PSN Asia): its PC config_database.dat values (no usable database entry on the console)", title_scope::database_fallback, true},
    {"BCUS98111", policy_detail::gow_3, "God of War III: its PC config_database.dat values (no usable database entry on the console)", title_scope::database_fallback},
    {"BCES00510", policy_detail::gow_3, "God of War III EU: its PC config_database.dat values (no usable database entry on the console)", title_scope::database_fallback},
    {"BCES00799", policy_detail::gow_3, "God of War III EU: its PC config_database.dat values (no usable database entry on the console)", title_scope::database_fallback},
    {"BCAS25003", policy_detail::gow_3, "God of War III Asia: its PC config_database.dat values (no usable database entry on the console)", title_scope::database_fallback},
    {"BCKS15003", policy_detail::gow_3, "God of War III Korea: its PC config_database.dat values (no usable database entry on the console)", title_scope::database_fallback},
    {"BLES00750", policy_detail::sonic_allstars_racing, "Sonic & SEGA All-Stars Racing: its PC config_database.dat values (no usable database entry on the console)", title_scope::database_fallback},
    {"BLUS30565", policy_detail::silent_hill_downpour, "Silent Hill: Downpour: its PC config_database.dat values (no usable database entry on the console)", title_scope::database_fallback},
};

// Whether a row of the table runs. automatic = false (user-tuned or automatic configuration off): only every-mode
// rows. A database_fallback row asks the config database, only for its own title.
inline bool title_row_active(const title_compat& entry, bool automatic)
{
    switch (entry.scope)
    {
    case title_scope::every_mode: return true;
    case title_scope::automatic: return automatic;
    case title_scope::database_fallback: return automatic && !config_db::has_entry(entry.title_id);
    }
    return false;
}

// A row that replaces the player's choice: an every-mode row (crash fix) always does; the automatic and database
// fallback rows leave a setting the player pinned for this game alone.
inline bool title_row_applies(const title_compat& entry, const cfg::_base& setting, const pinned_settings* pins)
{
    return !pins || entry.scope == title_scope::every_mode || !pins->for_game(setting);
}

// The table row that sets 'setting' for 'title_id', or nullptr. With automatic = false only every-mode rows count.
inline const title_compat* title_entry_for(cfg_root& cfg, std::string_view title_id, const cfg::_base& setting, bool automatic = true,
    const pinned_settings* pins = nullptr)
{
    if (title_id.empty()) return nullptr;
    for (const title_compat& entry : title_compat_table)
    {
        if (entry.title_id != title_id) continue;
        for (const title_override& item : entry.overrides)
            if (&item.setting(cfg) == &setting && title_row_active(entry, automatic) && title_row_applies(entry, setting, pins)) return &entry;
    }
    return nullptr;
}

// 0.34: an active row of title_id carries the PC entry's explicit 'Sleep Timers Accuracy: As Host' (host_sleep). A
// fallback row does not when the player pinned Sleep Timers Accuracy for the game.
inline bool title_rows_ask_host_sleep(std::string_view title_id, bool automatic, cfg_root* cfg = nullptr, const pinned_settings* pins = nullptr)
{
    if (title_id.empty()) return false;
    for (const title_compat& entry : title_compat_table)
        if (entry.title_id == title_id && entry.host_sleep && title_row_active(entry, automatic) &&
            (!cfg || title_row_applies(entry, cfg->core.sleep_timers_accuracy, pins)))
            return true;
    return false;
}

namespace policy_detail
{
// The rows of title_id that 'use' selects, reported as change_kind::title.
template <typename OnChange, typename Use>
u32 apply_title_rows(cfg_root& cfg, std::string_view title_id, OnChange& on_change, Use&& use, const pinned_settings* pins = nullptr)
{
    u32 changed = 0;
    if (title_id.empty()) return 0;
    for (const title_compat& entry : title_compat_table)
    {
        if (entry.title_id != title_id || !use(entry)) continue;
        for (const title_override& item : entry.overrides)
        {
            cfg::_base& setting = item.setting(cfg);
            if (!title_row_applies(entry, setting, pins)) continue;
            const std::string old = setting.to_string();
            if (old == item.value || !setting.from_string(item.value)) continue;
            report(on_change, static_cast<const cfg::_base&>(setting), old, entry.reason, change_kind::title);
            changed++;
        }
    }
    return changed;
}
} // namespace policy_detail

// OnChange: void(const cfg::_base& setting, const std::string& old_value, std::string_view why[, change_kind]).
// 'why' holds the table reason. automatic = false (user-tuned or automatic configuration off) applies only the
// every-mode rows.
template <typename OnChange>
u32 apply_title_compat(cfg_root& cfg, std::string_view title_id, OnChange&& on_change, bool automatic = true, const pinned_settings* pins = nullptr)
{
    return policy_detail::apply_title_rows(cfg, title_id, on_change, [&](const title_compat& entry) { return title_row_active(entry, automatic); }, pins);
}

// Hardware limits that hold in every mode. System.cpp keeps its own DFMA block (same rule, its own log line);
// the menu calls this so it shows the value that runs. title_id selects the per-game DFMA marker (empty = global).
template <typename OnChange>
u32 apply_hardware_limits(cfg_root& cfg, std::string_view title_id, OnChange&& on_change)
{
    if (utils::has_fma3()) return 0;
    // 0.33: the per-game marker turns it on for that title (PC default), whatever the YAML says.
    if (title_accurate_dfma(title_id))
    {
        if (cfg.core.use_accurate_dfma) return 0;
        const std::string old = cfg.core.use_accurate_dfma.to_string();
        cfg.core.use_accurate_dfma.set(true);
        policy_detail::report(on_change, static_cast<const cfg::_base&>(cfg.core.use_accurate_dfma), old,
            std::string_view{"per-game accurate-dfma marker: PC-exact fused DFMA through software fma()"}, change_kind::runtime);
        return 1;
    }
    // Jaguar has no FMA3/FMA4: accurate DFMA turns each PPU fmadd and SPU DFMA into a libm fma() call (0.25).
    if (!cfg.core.use_accurate_dfma || fs::is_file(accurate_dfma_flag)) return 0;
    const std::string old = cfg.core.use_accurate_dfma.to_string();
    cfg.core.use_accurate_dfma.set(false);
    policy_detail::report(on_change, static_cast<const cfg::_base&>(cfg.core.use_accurate_dfma), old,
        std::string_view{"no FMA on Jaguar; accurate-dfma.flag or the game's accurate-dfma marker keeps it"}, change_kind::runtime);
    return 1;
}

// The global form (no per-game marker).
template <typename OnChange>
u32 apply_hardware_limits(cfg_root& cfg, OnChange&& on_change)
{
    return apply_hardware_limits(cfg, std::string_view{}, on_change);
}

// 0.32: options that are unsafe or do nothing on this console. Every mode except keep-user-settings.flag forces the
// safe value after the per-game YAML, the automatic profile and the per-title rows, so neither a menu save, a copied
// PC config nor 'Configuração automática: Desligada' can turn them on. Each safe value is the upstream default. The
// menu shows the ones it lists as 'Indisponível no PS4' and never changes them.
struct unsafe_option
{
    cfg::_base& (*setting)(cfg_root&);
    std::string_view value; // YAML text of the safe value
    std::string_view why;   // RPCS3.log
    i18n::str menu;         // menu line (XMB font), translated
};

namespace policy_detail
{
inline cfg::_base& multithreaded_rsx_of(cfg_root& cfg) { return cfg.video.multithreaded_rsx; }
inline cfg::_base& async_texture_streaming_of(cfg_root& cfg) { return cfg.video.vk.asynchronous_texture_streaming; }
inline cfg::_base& async_scheduler_of(cfg_root& cfg) { return cfg.video.vk.asynchronous_scheduler; }
inline cfg::_base& vma_disabled_of(cfg_root& cfg) { return cfg.video.disable_vulkan_mem_allocator; }
inline cfg::_base& llvm_cpu_of(cfg_root& cfg) { return cfg.core.llvm_cpu; }
inline cfg::_base& use_native_interface_of(cfg_root& cfg) { return cfg.misc.use_native_interface; }
inline cfg::_base& getllar_spin_disabled_of(cfg_root& cfg) { return cfg.core.spu_getllar_spin_optimization_disabled; }
inline cfg::_base& autoexit_of(cfg_root& cfg) { return cfg.misc.autoexit; }
inline cfg::_base& external_debugger_of(cfg_root& cfg) { return cfg.core.external_debugger; }
inline cfg::_base& init_dirs_of(cfg_root& cfg) { return cfg.vfs.init_dirs; }
inline cfg::_base& autostart_of(cfg_root& cfg) { return cfg.misc.autostart; }
} // namespace policy_detail

inline constexpr unsafe_option unsafe_options[] =
{
    // VKCommandStream.cpp queue_submit hands primary submissions to the RSX Offloader when this is on, while the
    // access-violation drain (VKGSRender::on_access_violation) submits its fence directly, so that fence may not
    // cover the copies it waits for. It is also one more thread on a console that dies from thread exhaustion.
    {policy_detail::multithreaded_rsx_of, "false", "unsafe on PS4: the DMA queue drain does not cover submissions deferred to the RSX Offloader",
        i18n::str::POL_ESVAZIAMENTO_DMA_PS4_NAO_COBRE},
    // VKGSRender.cpp needs a transfer queue separate from the graphics queue; this GPU has one, and RPCS3 turns
    // async uploads off itself ('Cannot run graphics and async transfer in the same queue').
    {policy_detail::async_texture_streaming_of, "false", "no effect on PS4: the GPU has no separate transfer queue",
        i18n::str::POL_SEM_FILA_TRANSFERENCIA_SEPARADA_GPU},
    // Read only while asynchronous streaming runs; 'Fast' relies on GPU-signalled vk::events.
    {policy_detail::async_scheduler_of, "Safe", "only used by asynchronous texture streaming (unavailable on PS4)",
        i18n::str::POL_SO_VALE_STREAMING_ASSINCRONO_INDISPONIVEL},
    // vkutils/device.cpp swaps VMA for the plain allocator, and the PS4 video memory caps are VMA heap limits
    // (vkutils/memory.cpp). Not in the menu, but a copied PC config can carry it.
    {policy_detail::vma_disabled_of, "false", "the PS4 video memory caps need the Vulkan Memory Allocator",
        i18n::str::POL_LIMITES_VRAM_PS4_DEPENDEM_VMA},
    // 0.33: a PC CPU name ('haswell', 'znver4', ...) makes the JIT emit BMI2/FMA/XOP or AVX-512 code and the compile
    // threads run AVX-512/GFNI folding (CPUTranslator.cpp picks those from the name): SIGILL on Jaguar. Empty = the
    // host CPU (btver2), the upstream default. The automatic profile already forced it; this covers the other modes.
    // Not in the menu, but a copied PC config can carry it.
    {policy_detail::llvm_cpu_of, "", "a PC CPU name enables instructions the PS4 CPU lacks (crash)",
        i18n::str::POL_PS4_USA_SEMPRE_CPU},
    // 0.34: the PS4 frontend registers no desktop dialogs (ps4_main.cpp returns none for the save list, message,
    // keyboard and NP message dialogs), so with this off cellSaveData cancels, cellMsgDialog and the on-screen keyboard
    // report BUSY and the NP message dialogs would call a null dialog. Upstream default: true. System.cpp also forces it
    // with keep-user-settings.flag. Not in the menu, but a copied PC config can carry it.
    {policy_detail::use_native_interface_of, "true", "the PS4 frontend only has the native dialogs (save list, messages, keyboard)",
        i18n::str::POL_PS4_SO_TEM_INTERFACE},
    // 0.37: true makes SPUThread.cpp busy-wait every GETLLAR poll for good (getllar_busy_waiting_switch = 1), whatever
    // the busy-waiting percentage says; long guest spin loops starved the fixed-priority cores in 0.25/0.26. 22 entries
    // of the PC config database ask it. Upstream default: false. Not in the menu.
    {policy_detail::getllar_spin_disabled_of, "false", "unsafe on PS4: pure GETLLAR busy-waiting starves Orbis' fixed-priority cores",
        i18n::str::POL_ESPERA_ATIVA_PURA_SPU},
    // true makes Emulator::Kill quit the application when the game ends (Quit -> the frontend's exit request) instead
    // of returning to the XMB; an application that leaves by itself risks CE-34878-0. Upstream default: false.
    {policy_detail::autoexit_of, "false", "the game returns to the XMB on PS4; quitting the application by itself risks CE-34878-0",
        i18n::str::CFGR_LIMIT_AUTOEXIT},
    // Utilities/Thread.cpp: with it on, the fault and assertion paths break into an attached debugger, and there is none
    // on the console. Upstream default: false.
    {policy_detail::external_debugger_of, "false", "no debugger on PS4: fault paths would break into one that does not exist",
        i18n::str::CFGR_LIMIT_EXTERNAL_DEBUGGER},
    // Emulator::Init creates the dev_hdd0 / dev_hdd1 tree only with it on; the frontend and the games need it. Upstream
    // default: true.
    {policy_detail::init_dirs_of, "true", "the frontend and the games need the dev_hdd0 folders Emulator::Init creates",
        i18n::str::CFGR_LIMIT_INIT_DIRS},
    // The launcher always force-boots the chosen game; off would leave a savestate boot paused with no resume control.
    // Upstream default: true.
    {policy_detail::autostart_of, "true", "the PS4 launcher always starts the chosen game (no resume control)",
        i18n::str::CFGR_LIMIT_AUTOSTART},
};

// The unsafe_options row for 'setting', or nullptr.
inline const unsafe_option* unsafe_option_for(cfg_root& cfg, const cfg::_base& setting)
{
    for (const unsafe_option& item : unsafe_options)
        if (&item.setting(cfg) == &setting) return &item;
    return nullptr;
}

// 0.40: one value with no meaning on PS4 that a saved file can still hold, for a setting whose other values stay as
// chosen: the boot replaces only that value, in every mode but keep-user-settings.flag. Found by setting key, so a
// config tree without the setting skips the row.
struct unsafe_value
{
    std::string_view key;   // setting_key()
    std::string_view value; // the YAML text it replaces
    std::string_view safe;  // the YAML text that runs instead (the upstream default)
    std::string_view why;   // RPCS3.log and the startup.log console-limit line
    i18n::str menu;         // the settings browser's reason
};

inline constexpr unsafe_value unsafe_values[] =
{
    // The PS4 frontend always opens its own AudioOut backend, so 'Null' never muted anything: cellAudio only turned its
    // buffering off for it, and the unbuffered period clock then fired whole catch-up bursts after the boot wait and
    // after pauses (thousands of dropped blocks per boot). Cubeb is the upstream default; Master Volume mutes.
    {"Audio > Renderer", "Null", "Cubeb",
        "the PS4 has one sound output: Null did not mute, it only turned buffering off and the sound stuttered (use Master Volume)",
        i18n::str::CFGR_AUDIO_NULL_PS4},
};

// The unsafe_values row for 'setting', or nullptr.
inline const unsafe_value* unsafe_value_for(const cfg::_base& setting)
{
    const std::string key = setting_key(setting);
    for (const unsafe_value& item : unsafe_values)
        if (item.key == key) return &item;
    return nullptr;
}

// 0.40: Resolution Scale above this runs at it (a pin, a saved file, the in-game menu's last value): render targets
// grow with the square of the scale, and a live 650 % ended the RSX in VK_ERROR_OUT_OF_DEVICE_MEMORY within a frame.
// A hard-limit guard derived from the 768 MB device-local cap (200 % = 4x the surface memory of 100 %); the in-game
// slider stops at resolution_scale_slider_max_percent. The PC config database cap of 100 % stays in
// apply_config_database.
inline constexpr u64 resolution_scale_cap_percent = 200;
inline constexpr u64 resolution_scale_slider_max_percent = 150;

namespace policy_detail
{
// A leaf of 'cfg' by setting key, nullptr when this tree has none
inline cfg::_base* find_leaf(cfg_root& cfg, std::string_view key)
{
    std::vector<const cfg::_base*> leaves;
    collect_leaves(cfg.get_nodes(), leaves);
    for (const cfg::_base* leaf : leaves)
        if (setting_key(*leaf) == key) return const_cast<cfg::_base*>(leaf); // a member of the mutable tree 'cfg'
    return nullptr;
}
} // namespace policy_detail

// 0.32: the unsafe options and the compile-thread limits (change_kind::console), after the automatic profile or the
// per-title rows and before the runtime section (whose adaptation may still lower the thread counts to 1).
template <typename OnChange>
u32 apply_console_limits(cfg_root& cfg, OnChange&& on_change)
{
    u32 changed = 0;
    for (const unsafe_option& item : unsafe_options)
    {
        cfg::_base& setting = item.setting(cfg);
        const std::string old = setting.to_string();
        if (old == item.value || !setting.from_string(item.value)) continue;
        policy_detail::report(on_change, static_cast<const cfg::_base&>(setting), old, item.why, change_kind::console);
        changed++;
    }
    const auto limit = [&](auto& setting, s64 lowest, std::string_view why)
    {
        if (setting.get() >= lowest && setting.get() <= 2) return;
        const std::string old = setting.to_string();
        setting.set(s64{2});
        policy_detail::report(on_change, static_cast<const cfg::_base&>(setting), old, why, change_kind::console);
        changed++;
    };
    // 0 = one LLVM worker per core: 6 workers with their heaps at boot. Demon's Souls died from thread exhaustion in
    // 0.27/0.28; the profile sets 2 and the adaptation 1, and a saved YAML now stays within 1-2 as well.
    limit(cfg.core.llvm_threads, 1, "at most 2 compile threads on PS4 (0 = 6 workers; thread exhaustion)");
    // On Vulkan 0 means up to 6 extra 2 MiB threads. OpenGL picks 1 worker on this CPU for 0 (GLPipelineCompiler.cpp)
    // and its legacy mode does not read the value, so 0 stays there.
    limit(cfg.video.shader_compiler_threads_count, cfg.video.renderer == video_renderer::vulkan ? 1 : 0,
        "at most 2 shader compile threads on PS4 (0 = up to 6 extra threads on Vulkan)");
    // 0.40: single values without a PS4 meaning (unsafe_values) and the Resolution Scale cap
    for (const unsafe_value& item : unsafe_values)
    {
        cfg::_base* setting = policy_detail::find_leaf(cfg, item.key);
        if (!setting || setting->to_string() != item.value || !setting->from_string(item.safe)) continue;
        policy_detail::report(on_change, static_cast<const cfg::_base&>(*setting), std::string(item.value), item.why, change_kind::console);
        changed++;
    }
    if (cfg.video.resolution_scale_percent.get() > resolution_scale_cap_percent)
    {
        const std::string old = cfg.video.resolution_scale_percent.to_string();
        cfg.video.resolution_scale_percent.set(u64{resolution_scale_cap_percent});
        policy_detail::report(on_change, static_cast<const cfg::_base&>(cfg.video.resolution_scale_percent), old,
            std::string_view{"above 200 % the render targets outgrow the 768 MB video memory cap of the PS4 (out of device memory)"},
            change_kind::console);
        changed++;
    }
    return changed;
}

// 0.40: platform decisions: values the renderer or the backends of this console replace or ignore whatever a file says.
// One place for the boot (run_boot_policy), the settings browser's preview and the renderers, so a row shows what runs.
// - OpenGL Shader Mode: without gl-async.flag (no validated asynchronous worker on a shared EGL context) every mode is
//   the legacy recompiler; GLGSRender keeps the same rule for its own start.
// - VSync on Vulkan: the console's surface presents with FIFO whatever the mode (every logged swapchain used it); OpenGL
//   goes through Zink and is not proven, so its row stays editable.
// - Enable Buffering off: cellAudio's unbuffered clock skips sound after loads and pauses (a warned value, kept).
constexpr shader_mode platform_gl_shader_mode(shader_mode wanted, bool gl_async) noexcept
{
    return gl_async ? wanted : shader_mode::recompiler;
}

constexpr bool platform_vsync_ignored(video_renderer renderer) noexcept
{
    return renderer == video_renderer::vulkan;
}

constexpr bool platform_buffering_warned(bool enable_buffering) noexcept
{
    return !enable_buffering;
}

inline constexpr std::string_view platform_reason_prefix = "platform: ";
inline constexpr std::string_view platform_gl_shader_why =
    "platform: OpenGL compiles shaders with the legacy recompiler without /data/rpcs3/gl-async.flag (the renderer switches at start)";

// Reported as change_kind::console with a reason that starts with "platform: ".
template <typename OnChange>
u32 apply_platform_limits(cfg_root& cfg, OnChange&& on_change)
{
    u32 changed = 0;
    if (cfg.video.renderer == video_renderer::opengl)
    {
        const shader_mode runs = platform_gl_shader_mode(cfg.video.shadermode.get(), fs::is_file(gl_async_flag));
        if (runs != cfg.video.shadermode.get())
        {
            const std::string old = cfg.video.shadermode.to_string();
            cfg.video.shadermode.set(runs);
            policy_detail::report(on_change, static_cast<const cfg::_base&>(cfg.video.shadermode), old, platform_gl_shader_why, change_kind::console);
            changed++;
        }
    }
    return changed;
}

// 0.33: what the config database step did for one boot (startup.log line).
enum class database_use
{
    none,          // no file, or no entry for the title
    applied,       // the entry ran
    rejected,      // the entry has a key or value this build does not know: none of it runs (PC drops it too)
    manual,        // user-tuned game: its saved YAML runs as saved
    custom_config, // automatic configuration off and the game has its own YAML (PC: a custom config wins)
    skipped,       // 0.40: not applied for this title (config-database-off.flag, or the per-game safe mode level 2)
};

struct database_result
{
    database_use use = database_use::none;
    bool entry = false;  // the database lists the title
    u32 changes = 0;     // values the entry changed (also counted as per-title changes)
    std::string capped;  // what it asked above the console's caps ("Resolution 1920x1080")
    bool host_sleep = false; // 0.34: the applied entry writes 'Sleep Timers Accuracy: As Host' explicitly
    u32 keys = 0;        // 0.37: settings the applied entry lists (entry_keys)
    // 0.40: what the applied entry lists, as 'Name=value' with the final value's fate ("Keyboard=Basic, Multithreaded
    // RSX=true (capped)"), filled by the boot policy once every step ran (database_items)
    std::string items;
};

// Every reason the database step reports starts with this, so the settings menu can tell its values from the rows'.
inline constexpr std::string_view database_reason = "PC config_database.dat entry of this title (PC RPCS3 applies it too)";
inline constexpr std::string_view database_reason_prefix = "PC config_database.dat";

inline bool is_database_reason(std::string_view why)
{
    return why.starts_with(database_reason_prefix);
}

// 0.33: the game's own YAML (custom_configs/config_<TITLE_ID>.yml), which System.cpp applies before the policy.
inline bool has_custom_config(std::string_view title_id)
{
    return !title_id.empty() && fs::is_file(rpcs3::utils::get_custom_config_path(std::string(title_id)));
}

namespace policy_detail
{
struct setting_value
{
    cfg::_base* setting;
    std::string text;
};

// Every scalar and set setting under 'nodes', with its text. Maps, logs and devices are left out: the database
// never sets them.
inline void collect_settings(const std::vector<cfg::_base*>& nodes, std::vector<setting_value>& out)
{
    for (cfg::_base* node : nodes)
    {
        if (!node) continue;
        switch (node->get_type())
        {
        case cfg::type::node: collect_settings(static_cast<const cfg::node*>(node)->get_nodes(), out); break;
        case cfg::type::map:
        case cfg::type::node_map:
        case cfg::type::log:
        case cfg::type::device: break;
        default: out.push_back({node, setting_text(*node)});
        }
    }
}

// Lines of an output mode text ("1920x1080i" -> 1080); 0 when it has no 'x'.
inline unsigned resolution_lines(std::string_view text)
{
    const auto x = text.find('x');
    if (x == std::string_view::npos) return 0;
    unsigned lines = 0;
    for (std::size_t i = x + 1; i < text.size() && text[i] >= '0' && text[i] <= '9' && lines < 100000; i++)
        lines = lines * 10 + static_cast<unsigned>(text[i] - '0');
    return lines;
}

// 0.34: whether a database entry's YAML text writes 'name: value' itself (a value equal to the default leaves no trace
// in cfg). Plain block YAML as the PC database stores it: any indentation, CRLF, a quoted value, a trailing comment.
inline bool entry_writes(std::string_view yaml, std::string_view name, std::string_view value)
{
    const auto trim = [](std::string_view s)
    {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
        return s;
    };
    while (!yaml.empty())
    {
        const auto nl = yaml.find('\n');
        std::string_view line = trim(yaml.substr(0, nl));
        yaml.remove_prefix(nl == std::string_view::npos ? yaml.size() : nl + 1);
        if (!line.starts_with(name) || line.substr(name.size(), 1) != ":") continue;
        std::string_view text = line.substr(name.size() + 1);
        if (const auto hash = text.find(" #"); hash != std::string_view::npos) text = text.substr(0, hash);
        text = trim(text);
        if (text.size() >= 2 && (text.front() == '"' || text.front() == '\'') && text.back() == text.front())
            text = text.substr(1, text.size() - 2);
        if (text == value) return true;
    }
    return false;
}

// 0.37: the settings a database entry's YAML text lists: 'name: value' lines, and 'name:' lines whose next line is a
// '- item' list (a set such as Libraries Control). Section headers ('Video:', 'Vulkan:') and list items do not count.
inline u32 entry_keys(std::string_view yaml)
{
    const auto trim = [](std::string_view s)
    {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
        return s;
    };
    u32 keys = 0;
    bool open_header = false; // the previous line was 'name:' with nothing after it
    while (!yaml.empty())
    {
        const auto nl = yaml.find('\n');
        std::string_view line = trim(yaml.substr(0, nl));
        yaml.remove_prefix(nl == std::string_view::npos ? yaml.size() : nl + 1);
        if (line.empty() || line.front() == '#') continue;
        if (line.starts_with("- ") || line == "-")
        {
            if (open_header) keys++;
            open_header = false;
            continue;
        }
        open_header = false;
        if (const auto hash = line.find(" #"); hash != std::string_view::npos) line = trim(line.substr(0, hash));
        const auto colon = line.find(':');
        if (colon == std::string_view::npos) continue;
        if (trim(line.substr(colon + 1)).empty()) open_header = true;
        else keys++;
    }
    return keys;
}

// 0.40: the settings a database entry's YAML text lists, with their setting key ("Video > Multithreaded RSX", the
// sections by indentation) and the value it asks (a set's '- item' lines joined by ", ", as setting_text writes a set).
struct entry_item
{
    std::string key, value;
};

inline std::vector<entry_item> entry_items(std::string_view yaml)
{
    const auto trim = [](std::string_view s)
    {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
        return s;
    };
    const auto unquote = [](std::string_view s)
    {
        if (s.size() >= 2 && (s.front() == '"' || s.front() == '\'') && s.back() == s.front()) return s.substr(1, s.size() - 2);
        return s;
    };
    std::vector<entry_item> out;
    std::vector<std::pair<std::size_t, std::string>> sections; // indentation, name of the open 'name:' headers
    const auto path = [&](std::string_view leaf)
    {
        std::string key;
        for (const auto& section : sections) key += section.second + " > ";
        if (leaf.empty() && !key.empty()) key.resize(key.size() - 3);
        return key + std::string(leaf);
    };
    bool in_set = false; // the last entry of 'out' is the set the following '- item' lines belong to
    while (!yaml.empty())
    {
        const auto nl = yaml.find('\n');
        std::string_view line = yaml.substr(0, nl);
        yaml.remove_prefix(nl == std::string_view::npos ? yaml.size() : nl + 1);
        std::size_t indent = 0;
        while (indent < line.size() && line[indent] == ' ') indent++;
        line = trim(line);
        if (line.empty() || line.front() == '#') continue;
        if (line.starts_with("- ") || line == "-")
        {
            // The 'name:' header above the items names a set (it stays open until a line at its indentation)
            if (sections.empty()) continue;
            if (!in_set) out.push_back({path({}), {}});
            in_set = true;
            const std::string_view item = unquote(trim(line.substr(1)));
            if (!out.back().value.empty()) out.back().value += ", ";
            out.back().value += item;
            continue;
        }
        in_set = false;
        if (const auto hash = line.find(" #"); hash != std::string_view::npos) line = trim(line.substr(0, hash));
        const auto colon = line.find(':');
        if (colon == std::string_view::npos) continue;
        while (!sections.empty() && sections.back().first >= indent) sections.pop_back();
        const std::string_view name = trim(line.substr(0, colon));
        const std::string_view value = unquote(trim(line.substr(colon + 1)));
        if (value.empty()) sections.push_back({indent, std::string(name)});
        else out.push_back({path(name), std::string(value)});
    }
    return out;
}
} // namespace policy_detail

// 0.33: the title's PC config database entry (util/ps4_config_db.hpp) over cfg, after the automatic profile, like PC
// RPCS3's g_cfg.from_string(database config). Reported as change_kind::title with a database reason.
// - The per-title rows keep precedence: a value of the entry for a setting a row in use decides (automatic = false:
//   only the every-mode rows) is put back, so console-verified values win.
// - Strict, as PC validates each entry when it reads the database: with one key or value this build does not know,
//   none of the entry runs, the title is marked rejected until the file changes and its fallback rows run instead.
// - Console caps: output above 1280x720 and Resolution Scale above 100 % come back to those (VRAM: 0.32 Demon's Souls
//   peaked at 83 % of the 640 MiB GPU window at 720p). Every correctness value (buffer read/write, ZCULL, RSX
//   reservations, SPU block size, xfloat, decoders, Libraries Control) runs as listed.
// - An entry that selects OpenGL gets the legacy shader recompiler, as the menu's renderer switch does.
// - A setting the player pinned for this game is put back like a row's (a global pin is not: PC RPCS3 also puts the
//   database over its global config).
// The console limits and the runtime limits still run after this step (e.g. 'Multithreaded RSX: true' of an entry).
// 0.40: with the row config_db_entry off in 'features' (config-database-off.flag, or the per-game safe mode level 2) the
// entry is skipped: nothing of it runs and the title's fallback rows apply, as without a database.
template <typename OnChange>
u32 apply_config_database(cfg_root& cfg, std::string_view title_id, OnChange&& on_change, bool automatic, database_result* out = nullptr,
    const pinned_settings* pins = nullptr, std::optional<u64> features = std::nullopt)
{
    const u64 mask = features ? *features : policy_features(title_id);
    config_db::set_skipped(title_id, !safe_mode::enabled_in(mask, safe_mode::feature::config_db_entry));
    const auto game_pinned = [&](const cfg::_base& setting) { return pins && pins->for_game(setting); };
    database_result result;
    const config_db::entry entry = config_db::lookup(title_id);
    result.entry = !entry.config.empty();
    if (entry.rejected) result.use = database_use::rejected;
    if (entry.skipped && result.entry) result.use = database_use::skipped;
    if (!entry.usable())
    {
        if (out) *out = std::move(result);
        return 0;
    }

    std::vector<policy_detail::setting_value> before;
    policy_detail::collect_settings(cfg.get_nodes(), before);
    const bool valid = cfg.validate(entry.config);

    u32 changed = 0;
    bool renderer_set = false, shader_mode_set = false;
    for (const policy_detail::setting_value& item : before)
    {
        cfg::_base& setting = *item.setting;
        if (setting_text(setting) == item.text) continue;
        if (!valid || title_entry_for(cfg, title_id, setting, automatic, pins) || game_pinned(setting))
        {
            restore_setting_text(setting, item.text);
            continue;
        }
        std::string why(database_reason);
        if (&setting == &cfg.video.resolution && policy_detail::resolution_lines(setting.to_string()) > 720)
        {
            const std::string asked = setting.to_string();
            setting.from_string("1280x720");
            result.capped += (result.capped.empty() ? "" : ", ") + setting.get_name() + " " + asked;
            why = std::string(database_reason_prefix) + " asks for " + asked + "; capped at 1280x720 on PS4 (video memory)";
        }
        else if (&setting == &cfg.video.resolution_scale_percent && cfg.video.resolution_scale_percent.get() > 100)
        {
            const std::string asked = setting.to_string();
            cfg.video.resolution_scale_percent.set(u64{100});
            result.capped += (result.capped.empty() ? "" : ", ") + setting.get_name() + " " + asked;
            why = std::string(database_reason_prefix) + " asks for " + asked + " %; capped at 100 % on PS4 (video memory)";
        }
        // Capped back to the value it had: nothing changed (the cap is still in the startup.log line).
        if (setting_text(setting) == item.text) continue;
        if (&setting == &cfg.video.renderer) renderer_set = true;
        if (&setting == &cfg.video.shadermode) shader_mode_set = true;
        policy_detail::report(on_change, static_cast<const cfg::_base&>(setting), item.text, why, change_kind::title);
        changed++;
    }

    if (!valid)
    {
        config_db::mark_rejected(title_id);
        result.use = database_use::rejected;
        if (out) *out = std::move(result);
        // The profile did not leave the fallback rows' settings alone (the entry looked usable), so they run here.
        if (!automatic) return 0;
        return policy_detail::apply_title_rows(cfg, title_id, on_change, [](const title_compat& row) { return row.scope == title_scope::database_fallback; }, pins);
    }

    // OpenGL from the entry: the profile's Vulkan block chose the asynchronous recompiler, which on OpenGL needs shared
    // EGL contexts never run on the console (Zink).
    // 0.37: an entry that names the mode the profile already set (now the PC default) changed nothing, but chose it.
    if (!shader_mode_set && policy_detail::entry_writes(entry.config, cfg.video.shadermode.get_name(), cfg.video.shadermode.to_string()))
        shader_mode_set = true;
    // 0.39: with gl-async.flag the OpenGL profile mode is the asynchronous recompiler (one worker on a shared EGL context).
    const shader_mode gl_mode = profile_opengl_shader_mode();
    if (renderer_set && !shader_mode_set && cfg.video.renderer == video_renderer::opengl && cfg.video.shadermode != gl_mode &&
        !title_entry_for(cfg, title_id, cfg.video.shadermode, automatic, pins) && !game_pinned(cfg.video.shadermode))
    {
        const std::string old = cfg.video.shadermode.to_string();
        cfg.video.shadermode.set(gl_mode);
        policy_detail::report(on_change, static_cast<const cfg::_base&>(cfg.video.shadermode), old,
            gl_mode == shader_mode::recompiler
                ? std::string_view{"PC config_database.dat selects OpenGL: legacy shader recompiler (asynchronous GL compilation needs gl-async.flag)"}
                : std::string_view{"PC config_database.dat selects OpenGL: asynchronous recompiler (gl-async.flag)"},
            change_kind::title);
        changed++;
    }
    // 0.39: and the OpenGL ZCULL pair of the profile (relaxed unless zcull-strict-gl.flag) for each of the two settings
    // that neither the entry writes nor a row decides. 0.40: only with gl_relaxed_zcull.
    if (renderer_set && cfg.video.renderer == video_renderer::opengl && profile_zcull_relaxed(video_renderer::opengl, mask))
    {
        const auto relax = [&](cfg::_bool& setting, bool value)
        {
            if (setting.get() == value || policy_detail::entry_writes(entry.config, setting.get_name(), "true") ||
                policy_detail::entry_writes(entry.config, setting.get_name(), "false") || title_entry_for(cfg, title_id, setting, automatic, pins) ||
                game_pinned(setting))
                return;
            const std::string old = setting.to_string();
            setting.set(value);
            policy_detail::report(on_change, static_cast<const cfg::_base&>(setting), old,
                std::string_view{"PC config_database.dat selects OpenGL: relaxed ZCULL pair of the OpenGL profile (zcull-strict-gl.flag keeps it strict)"},
                change_kind::title);
            changed++;
        };
        relax(cfg.video.precise_zpass_count, false);
        relax(cfg.video.relaxed_zcull_sync, true);
    }
    result.use = database_use::applied;
    result.changes = changed;
    result.keys = policy_detail::entry_keys(entry.config);
    result.host_sleep = !game_pinned(cfg.core.sleep_timers_accuracy) &&
        policy_detail::entry_writes(entry.config, cfg.core.sleep_timers_accuracy.get_name(), "As Host");
    if (out) *out = std::move(result);
    return changed;
}

// The per-title rows for title_id (empty = global config, no rows), then the automatic profile for every setting
// those rows do not decide, then (0.33) the title's PC config database entry, whose values the rows still override.
// Returns the number of values changed; 'database' receives what the database step did.
// 'pins': the profile leaves every pinned setting (global or this game's) alone; the rows and the database entry only
// the game's pins (see apply_title_compat, apply_config_database).
// 0.40: 'features' is the feature mask of util/ps4_safe_mode.hpp the boot decides with (runtime_inputs::features);
// without it, policy_features(title_id) reads the files and the title's level.
template <typename OnChange>
u32 apply_performance_policy(cfg_root& cfg, const std::string& title_id, OnChange&& on_change, database_result* database = nullptr,
    const pinned_settings* pins = nullptr, std::optional<u64> features = std::nullopt)
{
    const u64 mask = features ? *features : policy_features(title_id);
    // Before the rows: a skipped database entry makes the title's fallback rows run (title_row_active)
    config_db::set_skipped(title_id, !safe_mode::enabled_in(mask, safe_mode::feature::config_db_entry));
    // 0.31: per-title rows first and the profile skips their settings, so a row like 'OpenGL' is neither
    // overwritten nor reported twice (Vulkan, then OpenGL again), and the Vulkan-only block below sees the
    // renderer that will run.
    u32 changed = apply_title_compat(cfg, title_id, on_change, true, pins);
    const auto force = [&](auto& setting, auto value)
    {
        if (setting.get() == value) return;
        if (pins && pins->any(setting)) return;
        if (title_entry_for(cfg, title_id, setting, true, pins)) return;
        const std::string old = setting.to_string();
        setting.set(value);
        policy_detail::report(on_change, static_cast<const cfg::_base&>(setting), old, std::string_view{}, change_kind::profile);
        changed++;
    };

    // --- CPU ---
#ifdef LLVM_AVAILABLE
    // The console's config.yml still says 'Interpreter (static)' / 'Recompiler (ASMJIT)' (written by the pre-LLVM
    // builds), so a game without its own YAML could boot interpreted. LLVM is the upstream default for both and is
    // what 0.27/0.28 ran (Minecraft, Demon's Souls). fixup_settings runs the FPU rounding self-test after this,
    // so a console where rounding control fails still gets the SPU interpreter.
    force(cfg.core.ppu_decoder, ppu_decoder_type::llvm);
    force(cfg.core.spu_decoder, spu_decoder_type::llvm);
#endif
    // Upstream: 'must be 2' (system_config.h). lv2 runs at most this many guest PPU threads at once.
    force(cfg.core.ppu_threads, s64{2});
    // Boot compile workers: 0 = all 6 cores (system_utils.cpp:24-30) = 6 SPU-cache + 6 PPU JIT threads and their
    // LLVM heaps at boot, on a console that dies from thread/stack exhaustion (Demon's Souls 0.27/0.28).
    if (cfg.core.llvm_threads == 0 || cfg.core.llvm_threads > 2)
    {
        force(cfg.core.llvm_threads, s64{2});
    }
    // A PC value (e.g. 'haswell') turns on m_use_fma/m_use_avx (CPUTranslator.cpp:129-148) while JITLLVM strips
    // FMA for Jaguar: every fmuladd would become a libm call. Empty = host = btver2 (upstream default).
    force(cfg.core.llvm_cpu, std::string{});
    // Upstream defaults: PPU cache prepared before the game runs, SPU programs cached between boots.
    force(cfg.core.llvm_precompilation, true);
    force(cfg.core.spu_cache, true);
    // Desktop debugging/profiling switches a copied PC config can carry (upstream defaults: off). Never useful
    // here, and they cost CPU or extra threads (the CPU Profiler only idles when both profilers are off).
    force(cfg.core.ppu_debug, false);
    force(cfg.core.spu_debug, false);
    force(cfg.core.mfc_debug, false);
    force(cfg.core.ppu_prof, false);
    force(cfg.core.spu_prof, false);
    force(cfg.core.llvm_logs, false);
    // 0.26: lv2.cpp 'Usleep Only'/'All Timers' turn every guest sleep remainder below 500 us into a yield() spin
    // (no mwaitx on Jaguar). Minecraft: 23.8k sys_timer_usleep(30 us)/s = one whole core spinning. ('As Host' is
    // also this build's default: system_config.h, __ORBIS__.)
    force(cfg.core.sleep_timers_accuracy, sleep_timers_accuracy_level::_as_host);
    // 0.26: GETLLAR busy_wait loops up to ~0.25 ms per episode; 0 % means always sleep on the notifier.
    force(cfg.core.spu_getllar_busy_waiting_percentage, u64{0});
    force(cfg.core.spu_reservation_busy_waiting_enabled, false);
    // 0.27: 'Accurate SPU Reservations' is not forced (upstream true is kept; Demon's Souls broke with it off).
    // Upstream default; the RPCS3 scheduler modes were never validated on Orbis.
    force(cfg.core.thread_scheduler, thread_scheduler_mode::os);
    // Upstream defaults. Max SPURS / Preferred SPU threads cannot help here (2-2.5 of 6 cores sit idle in the
    // measured games) and upstream warns that limiting threads causes crashes. SPU Block Size
    // Safe and XFloat Approximate are upstream's compatibility choices (Mega/Relaxed break some titles).
    force(cfg.core.max_spurs_threads, s64{6});
    force(cfg.core.preferred_spu_threads, s64{0});
    force(cfg.core.spu_block_size, spu_block_size_type::safe);
    force(cfg.core.spu_xfloat_accuracy, xfloat_accuracy::approximate);

    // --- GPU ---
    // Vulkan (native RADV) is what 0.28 runs best (Minecraft menu 60 FPS); OpenGL goes through Zink on the same
    // RADV with extra work and threads. A game that needs OpenGL is set per game in the menu (user-tuned marker)
    // or, when verified on the console, in the per-title table.
    force(cfg.video.renderer, video_renderer::vulkan);
    // Upstream defaults: 1280x720 output at 100 % internal scale (a few saved configs carried 720x480i).
    force(cfg.video.resolution, video_resolution::_720p);
    force(cfg.video.resolution_scale_percent, u64{100});
    force(cfg.video.frame_limit, frame_limit_type::_auto);
    if (cfg.video.renderer == video_renderer::vulkan)
    {
        // 0.27: on Vulkan the legacy single-threaded recompiler builds every new pipeline inside the RSX thread
        // (RADV compiles on the Jaguar): Minecraft's menu stuttered and entering the world dropped to 7
        // submissions in 8 s. Compile asynchronously on 2 workers instead. OpenGL keeps the legacy mode:
        // async GL compilation needs extra shared EGL contexts that were never validated on the console.
        // 0.37: with the PC default, VKGSRender draws geometry whose pipeline is still compiling with the shader
        // interpreter; 'Async Recompiler' (0.27-0.36, shader-skip-draws.flag) skips those draws, which loses one-off
        // render-to-texture passes, CPU/SPU-read surfaces and occlusion results, not only a frame.
        force(cfg.video.shadermode, profile_vulkan_shader_mode());
        // 0.29: exactly 2 workers (0.27-0.28 kept 1 and 3 as saved). 0 = auto = up to 6 extra 2 MiB threads at
        // the guests' priority (the PS4 ignores lowering it), on a console that dies from thread exhaustion;
        // 1 serializes compiles (~200 ms each) behind each other.
        force(cfg.video.shader_compiler_threads_count, s64{2});
        // 0.29: the 0.28 DMA drain (VKGSRender::on_access_violation) assumes primary submissions are not deferred
        // to the offloader thread (it would read stale data otherwise), and the RSX thread is not the bottleneck
        // (FIFO load 34-45 % in Minecraft's world).
        // Upstream default: off. 0.32: apply_console_limits also forces it off in the other modes and renderers.
        force(cfg.video.multithreaded_rsx, false);
    }
    // OpenGL that a row (which sets its own Shader Mode) or a pinned Renderer chose: the OpenGL mode of the menu's renderer
    // switch (the legacy recompiler unless gl-async.flag).
    else if (cfg.video.renderer == video_renderer::opengl)
    {
        force(cfg.video.shadermode, profile_opengl_shader_mode());
    }
    // Upstream defaults (off): surface write-back/readback and strict modes are per-game accuracy hacks that cost
    // GPU<->CPU round trips; a game that needs one gets a per-title entry, its config database entry (0.33) or a
    // per-game menu save.
    force(cfg.video.write_color_buffers, false);
    force(cfg.video.write_depth_buffer, false);
    force(cfg.video.read_color_buffers, false);
    force(cfg.video.read_depth_buffer, false);
    force(cfg.video.strict_rendering_mode, false);
    force(cfg.video.strict_texture_flushing, false);
    // 0.37: upstream defaults (strict ZCULL sync, accurate stats). Relaxed sync can publish a label or semaphore before
    // the report it covers, and inaccurate stats turn visible-pixel counts into 0/0xFFFF; titles that read reports on
    // the CPU (Demon's Souls, Wolverine, DMC4) then see stale values. The config database restores the per-title
    // relaxations PC uses. zcull-relaxed.flag brings back the 0.26-0.36 pair (no blocking GPU round trip per occlusion
    // query; hard ZCULL syncs were a candidate for the 0.25 Minecraft RSX freeze). Forced, not left alone, because the
    // YAMLs the 0.29-0.36 menu saved carry that pair.
    // 0.39: OpenGL (a per-title row) runs the relaxed pair unless zcull-strict-gl.flag exists: through Zink each strict
    // query result is a blocking glGetQueryObjectiv on the RSX thread. 0.40: only with gl_relaxed_zcull (opt-in).
    const bool zcull_relaxed = profile_zcull_relaxed(cfg.video.renderer, mask);
    force(cfg.video.precise_zpass_count, !zcull_relaxed);
    force(cfg.video.relaxed_zcull_sync, zcull_relaxed);
    // Desktop debugging switches (upstream defaults: off): Vulkan validation output and per-program dumps to disk.
    force(cfg.video.debug_output, false);
    force(cfg.video.log_programs, false);

    // --- Audio (upstream defaults) ---
    // Buffered output; time stretching off as a setting (SoundTouch costs CPU on Jaguar; 0.39: cellAudio engages its
    // automatic continuity by itself when the game falls behind, 0.40: only with audio_continuity); float samples (the
    // PS4 AudioOut backend takes both formats).
    // 0.39: a 64 ms reserve instead of the upstream 34 ms: cellAudio waits for a late game block while the output still
    // holds more than 16 ms, so the deeper ring turns late blocks into waits instead of skips and silence (+30 ms latency).
    // 0.40: the upstream 34 ms of 0.29-0.38 unless audio_reserve_64 (audio-reserve-64.flag) is on.
    force(cfg.audio.enable_buffering, true);
    force(cfg.audio.desired_buffer_duration, safe_mode::enabled_in(mask, safe_mode::feature::audio_reserve_64) ? s64{64} : s64{34});
    force(cfg.audio.enable_time_stretching, false);
    force(cfg.audio.convert_to_s16, false);
    force(cfg.audio.disable_sampling_skip, false);

    // --- 0.33: the title's PC config database entry ---
    // After the profile, so its per-title values (e.g. Write Color Buffers, Relaxed ZCULL Sync off, the PPU
    // interpreter) replace the profile's global ones exactly as on PC; the rows above keep precedence over it.
    changed += apply_config_database(cfg, title_id, on_change, true, database, pins, mask);
    return changed;
}

// 0.39: the language PS3 games get from the system (Configurações > Idioma dos jogos -> 'Language', CELL_SYSUTIL_LANG_*
// 0..19). game-language.txt holds the global choice, custom_configs/<TITLE_ID>.game-language one game's; each holds
// 'auto' or a number. Absent global file = automatic; absent marker = the global choice.
// 0.40: 'Automático' is English (US) again, as in 0.25-0.38 (0.39 followed the PS4 language, and God of War: Ascension
// then looked for BR_PORTUGUESE.PSARC, which its disc does not have, and mounted no language pack); features.txt
// game_language_ps4 brings back the 0.39 automatic value. The value 'ps4' ('Idioma do PS4') asks for that value for
// the boots of its scope: it turns the row game_language_ps4 on like an enable flag, so the safe mode reverts it. A
// language number stays absolute.
inline constexpr char game_language_path[] = "/data/rpcs3/game-language.txt";
inline constexpr std::int32_t game_language_auto = -1;
inline constexpr std::int32_t game_language_system = -2; // 'ps4': the PS4 language (the 0.39 automatic value)
inline constexpr std::int32_t game_language_count = i18n::cell_lang_count;

inline std::string game_language_marker(std::string_view title_id)
{
    if (title_id.empty()) return {};
    return rpcs3::utils::get_custom_config_dir() + std::string(title_id) + ".game-language";
}

// "auto" -> game_language_auto, "ps4" -> game_language_system, "0".."19" -> that language; nullopt for anything else
// (blanks and CR/LF trimmed)
inline std::optional<std::int32_t> parse_game_language(std::string_view text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r' || text.front() == '\n')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' || text.back() == '\n')) text.remove_suffix(1);
    if (text == "auto") return game_language_auto;
    if (text == "ps4") return game_language_system;
    if (text.empty() || text.size() > 2) return std::nullopt;
    std::int32_t value = 0;
    for (const char c : text)
    {
        if (c < '0' || c > '9') return std::nullopt;
        value = value * 10 + (c - '0');
    }
    if (value >= game_language_count) return std::nullopt;
    return value;
}

inline const char* game_language_token(std::int32_t value, char (&buffer)[4])
{
    if (value == game_language_system) return "ps4";
    if (value < 0 || value >= game_language_count) return "auto";
    buffer[0] = char('0' + value / 10);
    buffer[1] = char('0' + value % 10);
    buffer[2] = '\0';
    return value < 10 ? buffer + 1 : buffer;
}

// Where a boot's game language comes from (startup.log and the menu)
enum class game_language_source : std::uint8_t { automatic, global, game };

struct game_language_choice
{
    std::int32_t value = game_language_auto; // a CELL_SYSUTIL_LANG_* value, game_language_auto or game_language_system
    game_language_source source = game_language_source::automatic;
};

// The global choice ('global' = true) or what one game boots with (its marker, else the global choice). A file that
// does not parse counts as absent.
inline game_language_choice read_game_language(std::string_view title_id, bool global = false)
{
    game_language_choice choice;
    if (!global && !title_id.empty())
        if (const auto value = parse_game_language(policy_detail::read_small(game_language_marker(title_id), 64)))
        {
            choice.value = *value;
            choice.source = game_language_source::game;
            return choice;
        }
    if (const auto value = parse_game_language(policy_detail::read_small(game_language_path, 64)); value && *value != game_language_auto)
    {
        choice.value = *value;
        choice.source = game_language_source::global;
    }
    return choice;
}

// Writes the global choice (automatic removes the file) or one game's (nullopt removes the marker: the game follows the
// global choice). Returns false if the file system refused.
inline bool set_game_language(std::string_view title_id, std::optional<std::int32_t> value)
{
    char buffer[4];
    if (title_id.empty())
    {
        if (!value || *value == game_language_auto) return !fs::is_file(game_language_path) || fs::remove_file(game_language_path);
        return policy_detail::write_note(game_language_path, std::string(game_language_token(*value, buffer)) + "\n");
    }
    const std::string marker = game_language_marker(title_id);
    if (marker.empty()) return false;
    if (!value) return !fs::is_file(marker) || fs::remove_file(marker);
    fs::create_path(rpcs3::utils::get_custom_config_dir());
    return policy_detail::write_note(marker, std::string(game_language_token(*value, buffer)) + "\n");
}

// 0.40: rpcn.yml (beside config.yml, where cfg_rpcn and the PS4 account service keep it) holds an RPCN account: an NPID
// and a stored password in the derived form, 64 hex digits (the rule of util/ps4_rpcn_account.hpp is_derived_password;
// that header needs the wolfSSL ABI of the NP target, so it cannot be included here). Read as text, not through
// cfg_rpcn: its load() rewrites a version-1 file and get_npid() invents and saves an NPID, and a menu preview must
// write nothing. A version-1 file counts as no account (load() clears its password).
inline std::string rpcn_config_path()
{
    return fs::get_config_dir(true) + "rpcn.yml";
}

namespace policy_detail
{
// The top-level 'key: value' scalar of a YAML text (plain, single or double quoted); nullopt when the key is absent.
inline std::optional<std::string> top_level_text(std::string_view yaml, std::string_view key)
{
    while (!yaml.empty())
    {
        const auto nl = yaml.find('\n');
        const std::string_view line = yaml.substr(0, nl);
        yaml.remove_prefix(nl == std::string_view::npos ? yaml.size() : nl + 1);
        if (!line.starts_with(key) || line.substr(key.size(), 1) != ":") continue;
        std::string_view rest = line.substr(key.size() + 1);
        std::string value;
        if (!take_scalar(rest, value, {})) return std::nullopt;
        return value;
    }
    return std::nullopt;
}

constexpr bool derived_password(std::string_view value) noexcept
{
    if (value.size() != 64) return false;
    for (const char c : value)
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

// Overwrites a string that held a stored password before it is released.
inline void wipe(std::string& s) noexcept
{
    volatile char* p = s.data();
    for (std::size_t i = 0; i < s.size(); i++) p[i] = 0;
    s.clear();
}
} // namespace policy_detail

// The rule on the text of an rpcn.yml (host tests).
inline bool rpcn_account_present_in(std::string_view yaml)
{
    std::optional<std::string> npid = policy_detail::top_level_text(yaml, "NPID");
    std::optional<std::string> password = policy_detail::top_level_text(yaml, "Password");
    const bool present = npid && !npid->empty() && password && policy_detail::derived_password(*password) &&
        policy_detail::top_level_number(yaml, "Version") != 1;
    if (password) policy_detail::wipe(*password);
    return present;
}

inline bool rpcn_account_present()
{
    std::string text = policy_detail::read_small(rpcn_config_path(), 64 * 1024);
    const bool present = rpcn_account_present_in(text);
    policy_detail::wipe(text);
    return present;
}

// 0.31: what the runtime section reads from /data/rpcs3 for one boot. read_runtime_inputs() reads the files;
// the settings menu keeps a copy, and host tests build it by hand.
struct runtime_inputs
{
    bool online = false;            // online-rpcn.flag
    bool upnp = false;              // upnp.flag
    vram_override vram;             // vram.flag
    std::vector<std::string> adapt; // active adaptation keys of the title (util/ps4_adapt.hpp)
    unsigned adapt_boots_left = 0;  // boots, the next one included, before the last active key expires
    bool cool = false;              // 0.34: 'Modo frio' (cool-mode.flag; System.cpp re-reads it on every boot)
    bool compile_3 = false;         // 0.34: vk-compile-3.flag
    // 0.39: game language: the choice of the menu row
    game_language_choice game_language;
    // 0.40: what 'Automático' runs: English (US), or with features.txt game_language_ps4 the 0.39 automatic value (the
    // PS4 system language while the interface follows it, else the interface language)
    std::int32_t game_language_auto = 1;
    // 0.40: what 'Idioma do PS4' runs (the 0.39 automatic value), and whether this boot honours it: the per-game and the
    // global safe mode turn the row value off like an enable flag
    std::int32_t game_language_system = 1;
    bool game_language_system_on = true;
    std::int32_t system_language = -1; // ORBIS_SYSTEM_PARAM_LANG_* the automatic value came from (-1 unknown)
    // 0.40: rpcn.yml holds an RPCN account (read only while the online switch is on)
    bool rpcn_account = true;
    // 0.40: the feature rows of util/ps4_safe_mode.hpp the policy decides with for this boot, and the inputs they came
    // from (the reasons of the startup.log lines)
    safe_mode::inputs features_in{};
    u64 features = safe_mode::default_mask;
    // 0.40: the title's per-game safe mode (util/ps4_adapt.hpp safe_status): its level 0..3, the failure kind, the boots
    // it still applies (the next one included), level 3 failed again; and the global safe mode (safe-mode.flag)
    u8 safe_level = 0;
    safe_mode::fail_kind safe_kind = safe_mode::fail_kind::none;
    unsigned safe_boots_left = 0;
    bool safe_exhausted = false;
    bool global_safe = false;
    // 0.40: free MB on /data (data_free_mb, 0 = unknown), for the disk cache cap
    u64 data_free_mb = 0;
};

namespace policy_detail
{
// The per-game safe-mode state of an adapt file's text into 'in'.
inline void set_safe_level(runtime_inputs& in, std::string_view adapt_text)
{
    const adapt::safe_state safe = adapt::safe_status(adapt_text);
    in.safe_level = static_cast<u8>(std::min<unsigned>(safe.level, safe_mode::max_level));
    in.safe_kind = in.safe_level ? safe_mode::kind_from_name(safe.kind) : safe_mode::fail_kind::none;
    in.safe_boots_left = in.safe_level ? safe.boots_left : 0;
    in.safe_exhausted = in.safe_level && safe.exhausted;
}

// The feature mask of 'in' from the files safe_mode::read_inputs() found, at the title's level. The game language row
// value 'Idioma do PS4' counts as the enable of game_language_ps4 for that value only; 'Automático' follows the files.
inline void set_features(runtime_inputs& in, safe_mode::inputs files)
{
    using safe_mode::feature;
    files.title_level = in.safe_level;
    files.kind = in.safe_kind;
    in.global_safe = files.global_safe;
    const bool automatic_ps4 = safe_mode::enabled_in(safe_mode::compute(files), feature::game_language_ps4);
    if (in.game_language.value == game_language_system) files.enable_files |= safe_mode::bit(feature::game_language_ps4);
    in.features_in = files;
    in.features = safe_mode::compute(files);
    in.game_language_system = i18n::game_language_auto();
    in.game_language_system_on = safe_mode::enabled_in(in.features, feature::game_language_ps4);
    in.game_language_auto = automatic_ps4 ? in.game_language_system : 1;
}
} // namespace policy_detail

inline runtime_inputs read_runtime_inputs(std::string_view title_id)
{
    runtime_inputs in;
    in.online = online_enabled();
    in.upnp = upnp_enabled();
    in.vram = read_vram_flag();
    in.cool = cool_mode::enabled();
    in.compile_3 = fs::is_file(vk_compile_3_flag);
    in.game_language = read_game_language(title_id);
    in.system_language = i18n::system_language();
    in.rpcn_account = !in.online || rpcn_account_present();
    in.data_free_mb = data_free_mb();
    const std::string adapt_text = title_id.empty() ? std::string() : adapt::read_text(title_id);
    policy_detail::set_safe_level(in, adapt_text);
    // 0.40: with the title's own custom_configs/<TITLE_ID>.features (the rows it turns on for its boots only)
    policy_detail::set_features(in, safe_mode::read_inputs(nullptr, title_id));
    if (title_id.empty()) return in;
    for (adapt::entry& e : adapt::parse(adapt_text))
    {
        if (!adapt::is_active(e)) continue;
        // boots_left: the safe family lives 8 boots, so 3 - boots_since would wrap around
        in.adapt_boots_left = std::max(in.adapt_boots_left, adapt::boots_left(e));
        in.adapt.push_back(std::move(e.key));
    }
    return in;
}

inline u64 policy_features(std::string_view title_id)
{
    runtime_inputs in;
    in.game_language = read_game_language(title_id);
    if (!title_id.empty()) policy_detail::set_safe_level(in, adapt::read_text(title_id));
    policy_detail::set_features(in, safe_mode::read_inputs(nullptr, title_id));
    return in.features;
}

inline bool feature_on(const runtime_inputs& in, safe_mode::feature f)
{
    return safe_mode::enabled_in(in.features, f);
}

// 0.40: the boot items of the per-game safe mode (util/ps4_safe_mode.hpp items_for) at the level this boot runs (the
// title's, at least 1 with the global safe mode, which adds no item of its own: it has no failure kind).
inline bool safe_item(const runtime_inputs& in, safe_mode::item i)
{
    const u8 level = in.global_safe && in.safe_level < 1 ? u8{1} : in.safe_level;
    return (safe_mode::items_for(level, in.safe_kind) & safe_mode::item_bit(i)) != 0;
}

// "per-game safe mode level 2 (threads)", "global safe mode (safe-mode.flag)" or empty, for the change reasons.
inline std::string safe_reason(const runtime_inputs& in)
{
    if (!in.safe_level) return in.global_safe ? "global safe mode (safe-mode.flag)" : std::string();
    std::string text = "per-game safe mode level " + std::to_string(in.safe_level);
    if (in.safe_kind != safe_mode::fail_kind::none) text += " (" + std::string(safe_mode::kind_name(in.safe_kind)) + ")";
    return text;
}

// 0.40: the interface text of a failure kind (the boot toast, the settings rows); none reads as the player's choice.
inline i18n::str fail_text(safe_mode::fail_kind kind)
{
    using i18n::str;
    switch (kind)
    {
    case safe_mode::fail_kind::crash: return str::POL_FAIL_CRASH;
    case safe_mode::fail_kind::threads: return str::POL_FAIL_THREADS;
    case safe_mode::fail_kind::stall: return str::POL_FAIL_STALL;
    case safe_mode::fail_kind::gpu: return str::POL_FAIL_GPU;
    case safe_mode::fail_kind::user:
    case safe_mode::fail_kind::none: break;
    }
    return str::POL_FAIL_USER;
}

inline bool adapt_has(const runtime_inputs& in, std::string_view key)
{
    return adapt::has(in.adapt, key);
}

// A thread was refused, or a compile ran out of memory: compile on one thread. 0.40: also while the per-game safe mode
// runs the lighter thread budget (a thread failure) or one compile thread (level 2 and up).
inline bool adapt_fewer_threads(const runtime_inputs& in)
{
    return adapt_has(in, adapt::key_threads_low) || adapt_has(in, adapt::key_llvm_threads_1) || safe_item(in, safe_mode::item::threads_low) ||
        safe_item(in, safe_mode::item::compile_threads_1);
}

// The online switch is on, but this title last died from thread exhaustion. Online adds the RPCN client threads
// and NP workers, so the title stays offline until its adjustment expires. 0.40: also while its per-game safe mode
// runs the lighter thread budget or is at level 2 or more.
inline bool online_blocked(const runtime_inputs& in)
{
    return in.online && (adapt_has(in, adapt::key_threads_low) || safe_item(in, safe_mode::item::threads_low) || safe_item(in, safe_mode::item::online_off));
}

// 0.40: the online switch is on, but rpcn.yml holds no RPCN account: the boot stays offline (no NP sign-in can work, and
// a game that sees the network up and NP offline may wait for a server forever). online-without-account.flag (kill of
// online_needs_account) keeps the 0.39 state: the network up, NP offline.
inline bool online_no_account(const runtime_inputs& in)
{
    return in.online && !online_blocked(in) && !in.rpcn_account && feature_on(in, safe_mode::feature::online_needs_account);
}

// Device-local video memory limit without the adaptation: vram.flag device_mb= when present, else the configured
// value capped at 768 MB.
inline u64 vram_base_limit(u64 configured, const runtime_inputs& in)
{
    const u64 limit = in.vram.device_mb ? u64{in.vram.device_mb} : std::min<u64>(configured, vram_cap_mb);
    return std::clamp<u64>(limit, vram_min_mb, vram_max_mb);
}

// The limit this boot runs with: a title that ran out of video or direct memory gets at most 448 MB.
inline u64 vram_limit_for(u64 configured, const runtime_inputs& in)
{
    const u64 limit = vram_base_limit(configured, in);
    return adapt_has(in, adapt::key_vram_low) ? std::min<u64>(limit, vram_low_cap_mb) : limit;
}

// 0.40: the largest disk cache limit a boot keeps: the free-space cap (disk_cache_cap_mb) with the row
// disk_cache_order, else 4096 MB; never above what the setting holds.
inline s64 disk_cache_limit_max_mb(const runtime_inputs& in)
{
    if (!feature_on(in, safe_mode::feature::disk_cache_order)) return disk_cache_max_mb;
    return std::min(disk_cache_cap_mb(in.data_free_mb), disk_cache_setting_max_mb);
}

// 0.39: the game language a boot runs with: the menu row's choice, else the automatic value while the YAML holds the
// upstream default English (US); -1 = the YAML's value stays. 0.40: 'Idioma do PS4' runs the PS4 language while the
// boot honours it, else the automatic value.
inline std::int32_t game_language_for(const runtime_inputs& in, std::int32_t current)
{
    if (in.game_language.value >= 0 && in.game_language.value < game_language_count) return in.game_language.value;
    const std::int32_t automatic = in.game_language.value == game_language_system && in.game_language_system_on ? in.game_language_system
                                                                                                              : in.game_language_auto;
    if (current != 1 || automatic < 0 || automatic >= game_language_count) return -1;
    return automatic;
}

// 0.39: the GPU stopped responding in an earlier session of the title (adapt gpu_hang): on Vulkan its next boots run
// the Async Recompiler, which skips a draw whose pipeline is still compiling instead of drawing it with the shader
// interpreter (shader-skip-draws.flag does the same for every title). 0.40: also while the per-game safe mode follows a
// GPU failure, and at level 3 whatever the failure was.
inline bool adapt_no_shader_interpreter(const runtime_inputs& in)
{
    return adapt_has(in, adapt::key_gpu_hang) || safe_item(in, safe_mode::item::gpu_async) || safe_item(in, safe_mode::item::async_no_interpreter);
}

// 0.40: the reason of that switch: a GPU failure ('async recompiler after a GPU hang') or the safe-mode level 3.
inline std::string no_interpreter_reason(const runtime_inputs& in)
{
    if (adapt_has(in, adapt::key_gpu_hang)) return "the GPU stopped responding in an earlier session (adapt gpu_hang): async recompiler after a GPU hang";
    if (in.safe_kind == safe_mode::fail_kind::gpu)
        return "the GPU stopped responding in an earlier session (" + safe_reason(in) + "): async recompiler after a GPU hang";
    return safe_reason(in) + ": async recompiler without the shader interpreter";
}

// 0.40: the command-buffer split of the Vulkan renderer for the booted title (adapt key cb_split, written after a GPU
// device loss; row cb_split_after_loss). The decision belongs to the boot, which reads the adapt file before it appends
// this boot's "boot" line, so the renderer reads the stored decision instead of the file. Lock-free, any thread; false
// while no boot stored a decision.
namespace policy
{
inline std::atomic<bool> g_cb_split_boot{false};

inline bool cb_split_for_title() noexcept
{
    return g_cb_split_boot.load(std::memory_order_relaxed) && safe_mode::enabled(safe_mode::feature::cb_split_after_loss);
}
} // namespace policy

// 0.31: limits that hold in every mode except keep-user-settings.flag, applied after the automatic profile or the
// per-title rows (so after the per-game YAML too). OnChange gets change_kind::runtime, or change_kind::adapt for
// the per-title adaptation (including online skipped for the title).
template <typename OnChange>
u32 apply_runtime_limits(cfg_root& cfg, const runtime_inputs& in, OnChange&& on_change)
{
    u32 changed = 0;
    const auto force = [&](auto& setting, auto value, std::string_view why, change_kind kind)
    {
        if (setting.get() == value) return;
        const std::string old = setting.to_string();
        setting.set(value);
        policy_detail::report(on_change, static_cast<const cfg::_base&>(setting), old, why, kind);
        changed++;
    };

    // --- Console safety ---
    // /host_root/ exposes the console's own '/' to the guest. No game needs it, and a copied PC config must not
    // turn it on.
    force(cfg.vfs.host_root, false, "the guest must never reach the console's file system", change_kind::runtime);
    // The boot trims dev_hdd1/caches above this size (cache_utils.cpp), oldest first.
    force(cfg.vfs.limit_cache_size, true, "disk cache limited so it cannot fill the console's drive", change_kind::runtime);
    // 0.40: the upper bound follows the free space of /data (disk_cache_limit_max_mb); the player's value inside the
    // bounds runs as chosen
    const s64 cache_max = disk_cache_limit_max_mb(in);
    const std::string cache_why = cache_max > disk_cache_max_mb
        ? "disk cache limit kept between 1024 and " + std::to_string(cache_max) + " MB (10 % of the free space of /data)"
        : !feature_on(in, safe_mode::feature::disk_cache_order) ? std::string("disk cache limit kept between 1024 and 4096 MB (disk-cache-upstream.flag)")
        : std::string("disk cache limit kept between 1024 and 4096 MB");
    force(cfg.vfs.cache_max_size, std::clamp<s64>(cfg.vfs.cache_max_size.get(), disk_cache_floor_mb, cache_max), cache_why, change_kind::runtime);

    // --- Network: online-rpcn.flag decides ---
    // 0.40: a title on the lighter thread budget or at safe-mode level 2 or more stays offline, and so does every boot
    // while rpcn.yml holds no RPCN account (online_no_account).
    const bool blocked = online_blocked(in);
    const bool no_account = online_no_account(in);
    const bool online = in.online && !blocked && !no_account;
    const std::string blocked_why = adapt_has(in, adapt::key_threads_low) ? std::string("online skipped for this title: thread budget (adapt threads=low)")
                                                                         : "online skipped for this title: " + safe_reason(in);
    const std::string_view net_why = online ? std::string_view("online switch on (PlayStation Network > Modo online)")
        : blocked ? std::string_view(blocked_why)
        : no_account ? std::string_view("online switch on, but rpcn.yml holds no RPCN account: this boot is offline (PlayStation Network > Entrar com "
                                        "uma conta RPCN; online-without-account.flag keeps the network up)")
        : std::string_view("online switch off (PlayStation Network > Modo online)");
    const change_kind net_kind = blocked ? change_kind::adapt : change_kind::runtime;
    force(cfg.net.net_active, online ? np_internet_status::enabled : np_internet_status::disabled, net_why, net_kind);
    force(cfg.net.psn_status, online ? np_psn_status::psn_rpcn : np_psn_status::disabled, net_why, net_kind);
    // UPnP opens ports on the home router and runs a 2 s router discovery on every boot: only on request.
    const bool upnp = online && in.upnp;
    force(cfg.net.upnp_enabled, upnp, upnp ? "UPnP switched on (PlayStation Network > UPnP)"
        : "UPnP only when switched on in PlayStation Network, while online", change_kind::runtime);

    // --- Video memory (device-local heaps of the Vulkan allocator) ---
    const u64 configured = cfg.video.vk.vram_allocation_limit.get();
    const u64 base = vram_base_limit(configured, in);
    const u64 vram = vram_limit_for(configured, in);
    force(cfg.video.vk.vram_allocation_limit, vram,
        vram < base ? "video memory ran out in an earlier session (adapt vram=low)"
        : in.vram.device_mb ? "vram.flag device_mb override"
        : "device-local video memory capped inside the 991 MiB GPU window",
        vram < base ? change_kind::adapt : change_kind::runtime);

    // --- 0.39: game language (Configurações > Idioma dos jogos) ---
    // A language chosen in the menu runs whatever the YAML says; the automatic value only replaces the upstream default
    // English (US), so a language written into a YAML by hand stays.
    if (const std::int32_t lang = game_language_for(in, cfg.sys.language.get()); lang >= 0)
    {
        const std::string_view why = in.game_language.value >= 0 ? std::string_view("game language: Configurações > Idioma dos jogos")
            : in.game_language.value == game_language_system && in.game_language_system_on
                ? std::string_view("game language: the PS4 language (Configurações > Idioma dos jogos: Idioma do PS4)")
                : std::string_view("game language: automatic (Configurações > Idioma dos jogos)");
        force(cfg.sys.language, static_cast<CellSysutilLang>(lang), why, change_kind::runtime);
    }

    // --- Per-title adaptation ---
    if (adapt_fewer_threads(in))
    {
        // One LLVM worker also gives the SPU cache build one worker (rpcs3::utils::get_max_threads).
        const std::string why = adapt_has(in, adapt::key_threads_low) ? std::string("a thread was refused in an earlier session (adapt threads=low)")
            : adapt_has(in, adapt::key_llvm_threads_1) ? std::string("a compile ran out of memory in an earlier session (adapt llvm_threads=1)")
            : safe_reason(in) + ": one compile thread";
        force(cfg.core.llvm_threads, s64{1}, why, change_kind::adapt);
        force(cfg.video.shader_compiler_threads_count, s64{1}, why, change_kind::adapt);
    }
    // 0.34: vk-compile-3.flag (opt-in): a third Vulkan pipeline compile worker. Never for a title adapted to fewer
    // threads (above), and only in place of the 2 the console limit allows: a per-game 1 stays.
    else if (in.compile_3 && cfg.video.renderer == video_renderer::vulkan && cfg.video.shader_compiler_threads_count.get() == 2)
    {
        force(cfg.video.shader_compiler_threads_count, s64{3}, "vk-compile-3.flag: a third Vulkan pipeline compile worker (opt-in)",
            change_kind::runtime);
    }
    // 0.39: a title whose GPU stopped responding boots without the shader interpreter (a mode chosen otherwise stays).
    if (adapt_no_shader_interpreter(in) && cfg.video.renderer == video_renderer::vulkan && cfg.video.shadermode == shader_mode::async_with_interpreter)
    {
        force(cfg.video.shadermode, shader_mode::async_recompiler, no_interpreter_reason(in), change_kind::adapt);
    }
    return changed;
}

// Same, reading the flags and the adaptation file of 'title_id'.
template <typename OnChange>
u32 apply_runtime_limits(cfg_root& cfg, std::string_view title_id, OnChange&& on_change)
{
    return apply_runtime_limits(cfg, read_runtime_inputs(title_id), on_change);
}

// 0.34: the vk-compile-3.flag part of a game boot's policy line in startup.log ("" when the flag is absent).
inline std::string compile_3_note(const cfg_root& cfg, const runtime_inputs& in)
{
    if (!in.compile_3) return {};
    if (cfg.video.shader_compiler_threads_count.get() == 3) return " · shaders Vulkan com 3 threads (vk-compile-3.flag)";
    const char* why = adapt_fewer_threads(in) ? "ajuste automático de threads ativo"
        : cfg.video.renderer != video_renderer::vulkan ? "renderizador não é Vulkan"
        : "valor do jogo mantido";
    return std::string(" · vk-compile-3.flag sem efeito (") + why + ")";
}

// 0.34: 'Modo frio' values of the automatic configuration (run_boot_policy calls this in automatic mode only).
// The pad thread polls every pad_sleep us (pad_thread.cpp; about 6 % of a core at the upstream 1000 us); a value the
// player changed is kept. Reported as change_kind::runtime, so the settings menu shows it without saving it.
inline constexpr u64 pad_sleep_default_us = 1000;
inline constexpr u64 cool_pad_sleep_us = 8000;

// A pinned pad interval is the player's (also when it holds 1000 us).
template <typename OnChange>
u32 apply_cool_mode(cfg_root& cfg, const runtime_inputs& in, OnChange&& on_change, const pinned_settings* pins = nullptr)
{
    if (!in.cool || cfg.io.pad_sleep.get() != pad_sleep_default_us || (pins && pins->any(cfg.io.pad_sleep))) return 0;
    const std::string old = cfg.io.pad_sleep.to_string();
    cfg.io.pad_sleep.set(cool_pad_sleep_us);
    policy_detail::report(on_change, static_cast<const cfg::_base&>(cfg.io.pad_sleep), old,
        std::string_view{"Modo frio (cool-mode.flag): pads polled every 8 ms instead of 1 ms"}, change_kind::runtime);
    return 1;
}

namespace policy_detail
{
// One value a policy step changed: the setting and its text before and after.
struct recorded_change
{
    cfg::_base* setting;
    std::string old, now;
};

// Runs 'step' (called with a change callback) on cfg, then puts every changed value back, newest first, so cfg ends
// as it started. Returns what the step changed.
template <typename Step>
std::vector<recorded_change> dry_run(Step&& step)
{
    std::vector<recorded_change> changes;
    step([&](const cfg::_base& setting, const std::string& old, std::string_view, change_kind)
    {
        // Every reported setting is a member of the cfg_root the step was given, which the caller holds mutable.
        changes.push_back({const_cast<cfg::_base*>(&setting), old, setting_text(setting)});
    });
    // 0.33: through setting_text, because the database step can report a set (Libraries Control).
    for (auto it = changes.rbegin(); it != changes.rend(); ++it) restore_setting_text(*it->setting, it->old);
    return changes;
}

// The value 'setting' ends with after 'changes', or nullptr when they did not touch it.
inline const std::string* final_value(const std::vector<recorded_change>& changes, const cfg::_base* setting)
{
    const std::string* value = nullptr;
    for (const recorded_change& c : changes)
        if (c.setting == setting) value = &c.now;
    return value;
}
} // namespace policy_detail

// 0.32: true when this game's saved values (cfg = its YAML) run exactly like the automatic configuration, i.e. the
// user-tuned path (every-mode rows) and the automatic path (all rows + profile) end with the same values, console
// limits included. The runtime section and the hardware limits are the same on both paths. Only the values a step
// reports are compared and put back (no YAML round trip of g_cfg); cfg is left unchanged.
// 0.33: the automatic path includes the title's config database entry, so a save that only repeats it stays automatic.
// The automatic path honours the pins, as the automatic boot would.
// 0.40: 'features' as for apply_performance_policy.
inline bool saved_equals_automatic(cfg_root& cfg, const std::string& title_id, const pinned_settings* pins = nullptr,
    std::optional<u64> features = std::nullopt)
{
    using policy_detail::recorded_change;
    const u64 mask = features ? *features : policy_features(title_id);
    const std::vector<recorded_change> automatic = policy_detail::dry_run([&](auto&& record)
    {
        apply_performance_policy(cfg, title_id, record, nullptr, pins, mask);
        apply_console_limits(cfg, record);
    });
    const std::vector<recorded_change> tuned = policy_detail::dry_run([&](auto&& record)
    {
        apply_title_compat(cfg, title_id, record, false);
        apply_console_limits(cfg, record);
    });
    const auto same = [](const std::vector<recorded_change>& a, const std::vector<recorded_change>& b)
    {
        for (const recorded_change& c : a)
        {
            // cfg holds the saved value again, which is what a path that did not touch the setting runs.
            const std::string saved = setting_text(*c.setting);
            const std::string* x = policy_detail::final_value(a, c.setting);
            const std::string* y = policy_detail::final_value(b, c.setting);
            if ((x ? *x : saved) != (y ? *y : saved)) return false;
        }
        return true;
    };
    return same(automatic, tuned) && same(tuned, automatic);
}

// 0.37: the saved YAML whose values a boot outside the automatic mode runs: the game's own YAML when it has one, else
// the global ps4-settings.yml. Empty when there is none (or it is above 1 MiB). A format-2 game file holds only the
// player's pinned choices, which no migration touches, so the other values come from the global file.
inline std::string saved_settings_text(std::string_view title_id)
{
    if (has_custom_config(title_id))
    {
        std::string text = policy_detail::read_small(rpcs3::utils::get_custom_config_path(std::string(title_id)), settings_text_limit);
        if (saved_settings_format(text) < settings_format_version) return text;
    }
    return policy_detail::read_small(global_settings_path, settings_text_limit);
}

// 0.37: values that older automatic profiles forced, so the 0.29-0.36 menu wrote them into every YAML it saved (its
// preview shows the profile's values, and a save is a full dump). For a saved YAML stamped at or below the last
// version that forced a value (no stamp = 0), a setting that still holds that value gets the value the automatic
// configuration runs for this title now (profile, rows, PC database entry); a different value is the player's choice
// and stays, and so does a value the title's own row or database entry asks for (e.g. God of War: Ascension's
// 'Accurate ZCULL stats: false'). Reported as change_kind::migration; 'lines' receives the startup.log text. Returns
// the number of values changed. A pinned setting is the player's choice and never migrates.
// 0.40: a rule may also start at a version (forced_from): the values only profile 39 forced (the 64 ms reserve, the
// relaxed OpenGL pair) go back for a YAML stamped 39 while their rows are off. 'features' as for
// apply_performance_policy.
template <typename OnChange>
u32 apply_profile_migration(cfg_root& cfg, const std::string& title_id, std::string_view saved_yaml, OnChange&& on_change, std::vector<std::string>* lines = nullptr,
    const pinned_settings* pins = nullptr, std::optional<u64> features = std::nullopt)
{
    if (saved_yaml.empty()) return 0;
    const unsigned saved = saved_profile_version(saved_yaml);
    const u64 mask = features ? *features : policy_features(title_id);
    u32 changed = 0;
    // The automatic configuration's changes to the saved values, computed once before any migration
    std::vector<policy_detail::recorded_change> automatic;
    bool automatic_known = false;
    const auto migrate = [&](auto& setting, auto old_value, unsigned forced_from, unsigned forced_until)
    {
        if (saved < forced_from || saved > forced_until || setting.get() != old_value || (pins && pins->any(setting))) return;
        if (!automatic_known)
        {
            automatic = policy_detail::dry_run([&](auto&& record) { apply_performance_policy(cfg, title_id, record, nullptr, pins, mask); });
            automatic_known = true;
        }
        // Not changed by the automatic configuration: an escape file or the title's row or entry keeps the old value
        const std::string* value = policy_detail::final_value(automatic, &setting);
        const std::string old = setting.to_string();
        if (!value || *value == old || !setting.from_string(*value)) return;
        const std::string versions = forced_from == 0 ? "<=" + std::to_string(forced_until)
            : forced_from == forced_until ? std::to_string(forced_until) : std::to_string(forced_from) + "-" + std::to_string(forced_until);
        const std::string reason = "saved by automatic profile " + versions + ", which forced this value; automatic profile " +
            std::to_string(automatic_profile_version) + " value";
        policy_detail::report(on_change, static_cast<const cfg::_base&>(setting), old, std::string_view{reason}, change_kind::migration);
        if (lines)
        {
            lines->push_back("profile: '" + setting.get_name() + "' saved by automatic profile " + versions + " ('" + old +
                "') -> " + std::to_string(automatic_profile_version) + " ('" + setting.to_string() + "')");
        }
        changed++;
    };
    // 0.27-0.36 forced 'Async Recompiler' on Vulkan only; 0.26-0.36 forced the relaxed ZCULL pair.
    if (cfg.video.renderer == video_renderer::vulkan)
        migrate(cfg.video.shadermode, shader_mode::async_recompiler, 0, 36);
    // 0.39: on OpenGL the relaxed pair is the current profile value (nothing to migrate from <=36); the strict pair that
    // profile 37 (0.37-0.38) forced into OpenGL saves becomes the relaxed one, unless zcull-strict-gl.flag, the title's
    // row or its database entry decides the setting. 0.40: that is the opt-in gl_relaxed_zcull; without it OpenGL runs
    // the strict pair as in 0.38, and the relaxed pair profile 39 forced into OpenGL saves goes back to strict.
    const bool opengl = cfg.video.renderer == video_renderer::opengl;
    const bool gl_relaxed = opengl && profile_zcull_relaxed(video_renderer::opengl, mask);
    if (!gl_relaxed)
    {
        migrate(cfg.video.relaxed_zcull_sync, true, 0, 36);
        migrate(cfg.video.precise_zpass_count, false, 0, 36);
        if (opengl)
        {
            migrate(cfg.video.relaxed_zcull_sync, true, 39, 39);
            migrate(cfg.video.precise_zpass_count, false, 39, 39);
        }
    }
    else if (saved == 37)
    {
        const config_db::entry entry = config_db::lookup(title_id);
        const auto relax = [&](cfg::_bool& setting, bool strict_value)
        {
            if (setting.get() != strict_value || (pins && pins->any(setting)) || title_entry_for(cfg, title_id, setting, true, pins) ||
                (entry.usable() && (policy_detail::entry_writes(entry.config, setting.get_name(), "true") ||
                    policy_detail::entry_writes(entry.config, setting.get_name(), "false"))))
                return;
            const std::string old = setting.to_string();
            setting.set(!strict_value);
            policy_detail::report(on_change, static_cast<const cfg::_base&>(setting), old,
                std::string_view{"saved by automatic profile <=37, which forced the strict ZCULL pair; OpenGL value of gl_relaxed_zcull (zcull-strict-gl.flag keeps it strict)"},
                change_kind::migration);
            if (lines)
            {
                lines->push_back("profile: '" + setting.get_name() + "' saved by automatic profile <=37 ('" + old + "') -> " +
                    std::to_string(automatic_profile_version) + " OpenGL ('" + setting.to_string() + "')");
            }
            changed++;
        };
        relax(cfg.video.relaxed_zcull_sync, false);
        relax(cfg.video.precise_zpass_count, true);
    }
    // 0.29-0.38 forced the upstream 34 ms audio reserve; 0.39 ran 64 ms. 0.40: 34 ms again unless audio_reserve_64, so the
    // first rule changes nothing by default, and the 64 ms profile 39 forced goes back to 34 ms.
    migrate(cfg.audio.desired_buffer_duration, s64{34}, 0, 37);
    migrate(cfg.audio.desired_buffer_duration, s64{64}, 39, 39);
    return changed;
}

// The player's choices in a format-1 per-game YAML, for its conversion to format 2 (settings menu, 'Modo deste jogo'):
// 'saved' holds the game's stack (config.yml, ps4-settings.yml, then the game's YAML) and 'automatic' the same stack
// without the game's YAML. A candidate is a setting the game's YAML gives another value than the global files. 'saved'
// then runs as the user-tuned boot runs it (older profile values migrated, every-mode rows), 'automatic' as the automatic
// boot (profile, rows, the PC database entry, the global pins; not Modo frio, whose pad interval nobody chose); both get
// the console limits, the runtime section and the hardware limit. The candidates whose final values still differ are the
// choices ('saved' then holds the values to write). Maps, logs and devices are not compared (the policy never sets
// them). Both trees are changed.
inline std::vector<std::string> conversion_pins(cfg_root& saved, cfg_root& automatic, const std::string& title_id, const runtime_inputs& in,
    std::string_view saved_yaml, const std::vector<std::string>& global_pins = {})
{
    std::vector<const cfg::_base*> a, s;
    policy_detail::collect_leaves(automatic.get_nodes(), a);
    policy_detail::collect_leaves(saved.get_nodes(), s);
    const auto comparable = [](const cfg::_base& leaf)
    {
        switch (leaf.get_type())
        {
        case cfg::type::map:
        case cfg::type::node_map:
        case cfg::type::log:
        case cfg::type::device: return false;
        default: return true;
        }
    };
    std::vector<std::size_t> candidates;
    for (std::size_t i = 0; i < a.size() && i < s.size(); i++)
        if (comparable(*s[i]) && setting_text(*a[i]) != setting_text(*s[i])) candidates.push_back(i);
    if (candidates.empty()) return {};

    const auto none = [](const cfg::_base&, const std::string&, std::string_view, change_kind) {};
    pin_set global;
    global.global = global_pins;
    const pinned_settings pins = resolve_pins(automatic, global);
    apply_performance_policy(automatic, title_id, none, nullptr, policy_detail::pins_or_null(pins), in.features);
    apply_profile_migration(saved, title_id, saved_yaml, none, nullptr, nullptr, in.features);
    apply_title_compat(saved, title_id, none, false);
    for (cfg_root* side : {&automatic, &saved})
    {
        apply_console_limits(*side, none);
        apply_runtime_limits(*side, in, none);
        apply_hardware_limits(*side, title_id, none);
    }
    std::vector<std::string> keys;
    for (const std::size_t i : candidates)
        if (setting_text(*a[i]) != setting_text(*s[i])) keys.push_back(setting_key(*s[i]));
    return keys;
}

namespace policy_detail
{
inline std::string default_text(cfg::_base& setting);
} // namespace policy_detail

// 0.40: the player's own values in the global layer while it is of format 1 (config.yml, or a ps4-settings.yml without
// the format key): settings whose saved value is not the upstream default and that the automatic configuration still
// replaces after the profile migration and the limits, the global pins left out (they run already). Nothing here runs
// on its own: the settings menu lists them and pins them with the saved values only on the player's X ('Usar meus
// valores salvos'). Both trees hold the global files' values and are changed. A format-2 global file is a full dump of
// what the menu showed (the profile's values with the pins), so it has no other choices to offer.
inline std::vector<std::string> legacy_choices(cfg_root& saved, cfg_root& automatic, const runtime_inputs& in, std::string_view global_yaml,
    const std::vector<std::string>& global_pins = {})
{
    if (!global_yaml.empty() && saved_settings_format(global_yaml) >= settings_format_version) return {};
    std::vector<const cfg::_base*> a, s;
    policy_detail::collect_leaves(automatic.get_nodes(), a);
    policy_detail::collect_leaves(saved.get_nodes(), s);
    std::vector<std::size_t> candidates;
    for (std::size_t i = 0; i < a.size() && i < s.size(); i++)
    {
        switch (s[i]->get_type())
        {
        case cfg::type::map:
        case cfg::type::node_map:
        case cfg::type::log:
        case cfg::type::device: continue;
        default: break;
        }
        const std::string key = setting_key(*s[i]);
        if (std::find(global_pins.begin(), global_pins.end(), key) != global_pins.end()) continue;
        cfg::_base& leaf = const_cast<cfg::_base&>(*s[i]); // a member of the mutable tree 'saved'
        if (setting_text(leaf) != policy_detail::default_text(leaf)) candidates.push_back(i);
    }
    if (candidates.empty()) return {};

    const auto none = [](const cfg::_base&, const std::string&, std::string_view, change_kind) {};
    pin_set pins;
    pins.global = global_pins;
    const pinned_settings resolved = resolve_pins(automatic, pins);
    const std::string no_title;
    apply_performance_policy(automatic, no_title, none, nullptr, policy_detail::pins_or_null(resolved), in.features);
    apply_profile_migration(saved, no_title, global_yaml, none, nullptr, nullptr, in.features);
    for (cfg_root* side : {&automatic, &saved})
    {
        apply_console_limits(*side, none);
        apply_platform_limits(*side, none);
        apply_runtime_limits(*side, in, none);
        apply_hardware_limits(*side, none);
    }
    std::vector<std::string> keys;
    for (const std::size_t i : candidates)
        if (setting_text(*a[i]) != setting_text(*s[i])) keys.push_back(setting_key(*s[i]));
    return keys;
}

// 0.40: one setting a game boot runs with another value than the player's settings files hold (the 'settings:' line
// of startup.log and RPCS3.log): the files' value, the value that runs, and who replaced it.
struct settings_diff
{
    std::string key;      // setting_key()
    std::string name;     // the setting's own name, for the line
    std::string file;     // the value the settings files gave
    std::string now;      // the value the boot runs
    std::string_view who; // automático, limite do PS4, após falha, base do PC, correção do jogo, interruptor, plataforma, perfil antigo
    change_kind kind = change_kind::profile; // the last step that changed it
};

struct policy_result
{
    policy_mode mode = policy_mode::automatic;
    u32 profile_changes = 0; // automatic profile
    u32 title_changes = 0;   // per-title rows and per-title adaptation
    u32 adapt_changes = 0;   // per-title adaptation only (also counted in title_changes)
    u32 runtime_changes = 0; // online switch, video memory cap, console safety
    u32 console_changes = 0; // 0.32: options unavailable on PS4 and compile-thread limits
    bool stale_marker = false;   // 0.32: a user-tuned marker ignored because the saved values equal the automatic ones
    bool online = false;         // this boot goes online (RPCN)
    bool online_skipped = false; // the online switch is on, but this title stays offline (thread budget; 0.40: safe mode)
    bool online_no_account = false; // 0.40: the online switch is on, but rpcn.yml holds no RPCN account: offline
    // 0.40: the title's per-game safe mode (util/ps4_adapt.hpp safe_status), for safe_mode::set_for_boot in System.cpp;
    // set in every mode, keep-user-settings.flag included (the feature rows are code paths, not settings)
    u8 safe_level = 0;
    safe_mode::fail_kind safe_kind = safe_mode::fail_kind::none;
    std::vector<std::string> adapt_keys; // active adaptation keys of the title
    database_result database;    // 0.33: the PC config database step (its changes are also in title_changes)
    // 0.34: the title's PC database entry, applied or held by its fallback rows, asks 'Sleep Timers Accuracy: As Host'
    // explicitly; System.cpp then turns lv2's precise usleep loop off for this boot (lv2_obj::ps4_set_precise_sleep_title).
    bool host_sleep_requested = false;
    u32 migrated_changes = 0;          // 0.37: saved values an older profile forced (apply_profile_migration)
    std::vector<std::string> notes;    // 0.37: startup.log lines of a game boot (profile migration, unsafe options limited)
    // 0.39: the PPU / SPU decoder this boot runs was set by the title's PC config database entry (else, when it is an
    // interpreter, it is the player's YAML)
    bool ppu_decoder_from_database = false;
    bool spu_decoder_from_database = false;
    // The player's own choices in effect: settings pinned in the game's YAML and in ps4-settings.yml
    u32 pinned_game = 0, pinned_global = 0;
    // 0.40 (boots only): the values of the player's files this boot replaced, in the order the steps changed them; and
    // how many of them the game's own format-1 YAML holds and the automatic profile replaced (the toast that points at
    // the per-game Converter)
    std::vector<settings_diff> settings_diffs;
    u32 legacy_game_values = 0;
};

namespace policy_detail
{
// 0.40: who replaced a value, in the words of the 'settings:' line
inline std::string_view diff_who(const cfg_root& cfg, const cfg::_base& setting, std::string_view why, change_kind kind)
{
    switch (kind)
    {
    case change_kind::profile: return "automático";
    case change_kind::migration: return "perfil antigo";
    case change_kind::title: return is_database_reason(why) ? "base do PC" : "correção do jogo";
    case change_kind::adapt: return "após falha";
    case change_kind::console: return why.starts_with(platform_reason_prefix) ? "plataforma" : "limite do PS4";
    case change_kind::runtime:
        if (&setting == &cfg.core.use_accurate_dfma) return "plataforma";
        if (&setting == &cfg.net.net_active || &setting == &cfg.net.psn_status || &setting == &cfg.net.upnp_enabled || &setting == &cfg.sys.language ||
            &setting == &cfg.io.pad_sleep)
            return "interruptor";
        return "limite do PS4";
    }
    return "limite do PS4";
}

// A change a boot's policy steps reported, per setting: the value before the first step and who changed it last
struct diff_note
{
    const cfg::_base* setting;
    std::string old;
    std::string_view who;
    change_kind kind;
};

inline void note_diff(std::vector<diff_note>& notes, const cfg_root& cfg, const cfg::_base& setting, const std::string& old, std::string_view why,
    change_kind kind)
{
    const std::string_view who = diff_who(cfg, setting, why, kind);
    for (diff_note& n : notes)
        if (n.setting == &setting)
        {
            n.who = who;
            n.kind = kind;
            return;
        }
    notes.push_back({&setting, old, who, kind});
}

// The upstream default of a leaf as setting_text writes it (the leaf keeps its value)
inline std::string default_text(cfg::_base& setting)
{
    const std::string now = setting_text(setting);
    setting.from_default();
    std::string def = setting_text(setting);
    if (setting_text(setting) != now) restore_setting_text(setting, now);
    return def;
}

// The emulator's own pass of the current boot (no title): its replacements of the global files' values, which the game
// pass merges (the game's YAML is loaded over that pass's result, so it starts from the replaced values). Boot thread
// only; every emulator pass replaces the list.
inline std::vector<settings_diff> g_emulator_pass_diffs;

// The diffs of one boot pass once every step ran ('cfg' holds the final values): the emulator pass keeps its own for
// the game pass and returns none; the game pass adds the emulator pass's replacements its YAML did not override. A
// value equal to its upstream default is not the player's (a generated config.yml holds them all) and is left out.
inline std::vector<settings_diff> finish_diffs(cfg_root& cfg, std::string_view title_id, const std::vector<diff_note>& notes)
{
    std::vector<settings_diff> out;
    for (const diff_note& n : notes)
    {
        const cfg::_base& setting = *n.setting;
        out.push_back({setting_key(setting), setting.get_name(), n.old, setting_text(setting), n.who, n.kind});
    }
    if (title_id.empty())
    {
        g_emulator_pass_diffs = std::move(out);
        return {};
    }
    for (const settings_diff& first : g_emulator_pass_diffs)
    {
        const auto it = std::find_if(out.begin(), out.end(), [&](const settings_diff& d) { return d.key == first.key; });
        if (it != out.end())
        {
            // Changed again by this pass: when it started from the emulator pass's value, the files held the earlier one
            if (it->file == first.now) it->file = first.file;
            continue;
        }
        cfg::_base* leaf = find_leaf(cfg, first.key);
        // The game's YAML gave it another value after that pass, or the setting is unknown to this tree
        if (!leaf || setting_text(*leaf) != first.now) continue;
        out.push_back(first);
    }
    std::erase_if(out, [&](const settings_diff& d)
    {
        if (d.file == d.now) return true;
        cfg::_base* leaf = find_leaf(cfg, d.key);
        return !leaf || default_text(*leaf) == d.file;
    });
    return out;
}

// A value for the line: at most 40 bytes, never cut inside a UTF-8 sequence
inline std::string diff_value(std::string_view text)
{
    if (text.size() <= 40) return text.empty() ? std::string("\"\"") : std::string(text);
    std::size_t cut = 37;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xC0) == 0x80) cut--;
    return std::string(text.substr(0, cut)) + "...";
}

inline constexpr std::size_t settings_diff_shown = 12;

// "settings: BLUS30645: 2 valor(es) rodando diferente dos seus arquivos: Frame limit Off->Auto (automático), Resolution
// Scale 900->200 (limite do PS4)": at most 12, then '(+N)'; empty when nothing differs.
inline std::string settings_diff_line(std::string_view title_id, const std::vector<settings_diff>& diffs)
{
    if (diffs.empty()) return {};
    std::string line = "settings: " + std::string(title_id) + ": " + std::to_string(diffs.size()) + " valor(es) rodando diferente dos seus arquivos:";
    for (std::size_t i = 0; i < diffs.size() && i < settings_diff_shown; i++)
        line += (i ? ", " : " ") + diffs[i].name + " " + diff_value(diffs[i].file) + "->" + diff_value(diffs[i].now) + " (" + std::string(diffs[i].who) + ")";
    if (diffs.size() > settings_diff_shown) line += " (+" + std::to_string(diffs.size() - settings_diff_shown) + ")";
    return line;
}

// How many diffs are values of the game's own format-1 YAML ('yaml') that the automatic profile replaced
inline u32 legacy_game_values(const std::vector<settings_diff>& diffs, std::string_view yaml)
{
    if (yaml.empty() || saved_settings_format(yaml) >= settings_format_version) return 0;
    const std::vector<entry_item> items = entry_items(yaml);
    u32 n = 0;
    for (const settings_diff& d : diffs)
        if (d.kind == change_kind::profile &&
            std::any_of(items.begin(), items.end(), [&](const entry_item& item) { return item.key == d.key && item.value == d.file; }))
            n++;
    return n;
}

// The startup.log line of the player's own choices for a game boot (empty when there are none): the game's pins with the
// value they run with (at most 8, then '+N'), the count of global pins, and the listed keys this build does not know.
inline std::string pins_line(const std::string& title_id, const pinned_settings& pins)
{
    if (pins.empty() && pins.unknown.empty() && pins.revoked.empty()) return {};
    std::string line = "Política PS4 para " + title_id + ": " + std::to_string(pins.game.size()) + " ajuste(s) seu(s)";
    constexpr std::size_t shown = 8;
    for (std::size_t i = 0; i < pins.game.size() && i < shown; i++)
        line += (i ? ", " : ": ") + setting_key(*pins.game[i]) + "=" + setting_text(*pins.game[i]);
    if (pins.game.size() > shown) line += " (+" + std::to_string(pins.game.size() - shown) + ")";
    line += " · ajustes globais: " + std::to_string(pins.global.size());
    if (!pins.unknown.empty())
    {
        line += " · sem opção nesta versão (ignorados):";
        for (std::size_t i = 0; i < pins.unknown.size() && i < shown; i++) line += (i ? ", " : " ") + pins.unknown[i];
        if (pins.unknown.size() > shown) line += " (+" + std::to_string(pins.unknown.size() - shown) + ")";
    }
    if (!pins.revoked.empty())
    {
        line += " · de volta ao automático (perfil " + std::to_string(automatic_profile_version) + "):";
        for (std::size_t i = 0; i < pins.revoked.size(); i++) line += (i ? ", " : " ") + pins.revoked[i];
    }
    return line;
}

// 0.40: the applied entry's settings for its startup.log line, once every policy step ran: 'Name=value' as the entry
// asks it, then what became of it: '(row)' a per-title row decides the setting, '(pinned)' the player pinned it for
// this game, '(capped)' a later step (a console cap or limit, the runtime section) runs another value; '(unknown)'
// names no setting of this build. At most 16 items, then '+N'.
inline std::string database_items(cfg_root& cfg, std::string_view title_id, std::string_view yaml, bool automatic, const pinned_settings* pins)
{
    std::vector<const cfg::_base*> leaves;
    collect_leaves(cfg.get_nodes(), leaves);
    const auto same = [](const cfg::_base& setting, const std::string& asked)
    {
        if (setting.get_type() != cfg::type::set) return setting.to_string() == asked;
        // A set: the same items in any order
        std::vector<std::string> want;
        for (std::string_view rest = asked; !rest.empty();)
        {
            const auto comma = rest.find(", ");
            want.emplace_back(rest.substr(0, comma));
            rest.remove_prefix(comma == std::string_view::npos ? rest.size() : comma + 2);
        }
        std::vector<std::string> have = setting.to_list();
        std::sort(want.begin(), want.end());
        std::sort(have.begin(), have.end());
        return want == have;
    };
    constexpr std::size_t shown_max = 16;
    std::string out;
    std::size_t shown = 0, total = 0;
    for (const entry_item& item : entry_items(yaml))
    {
        total++;
        if (shown >= shown_max) continue;
        const auto gt = item.key.rfind(" > ");
        std::string text = item.key.substr(gt == std::string::npos ? 0 : gt + 3) + "=" + item.value;
        const auto it = std::find_if(leaves.begin(), leaves.end(), [&](const cfg::_base* leaf) { return setting_key(*leaf) == item.key; });
        if (it == leaves.end()) text += " (unknown)";
        else if (title_entry_for(cfg, title_id, **it, automatic, pins)) text += " (row)";
        else if (pins && pins->for_game(**it)) text += " (pinned)";
        else if (!same(**it, item.value)) text += " (capped)";
        out += (shown++ ? ", " : "") + text;
    }
    if (total > shown) out += " (+" + std::to_string(total - shown) + ")";
    return out;
}

// 0.33: the startup.log line of the config database step for a game boot. 0.37: the copies' line
// (config_db::describe) comes first; this one says what the title's entry did and is empty when no copy is usable.
// 0.40: an applied entry lists its settings (database_result::items); 'skip_reason' says why an entry was skipped.
inline std::string database_line(const std::string& title_id, const database_result& db, const config_db::file_info& file, std::string_view skip_reason = {})
{
    if (file.state != config_db::file_state::loaded) return {};
    std::string line = "config database: ";
    switch (db.use)
    {
    case database_use::none: line += title_id + (db.entry ? " entry not used" : " has no entry"); break;
    case database_use::applied:
        line += "applied " + title_id;
        if (!db.items.empty()) line += ": " + db.items;
        line += " · " + std::to_string(db.keys) + " keys, " + std::to_string(db.changes) + " value(s) changed";
        if (!db.capped.empty()) line += " (capped on PS4: " + db.capped + ")";
        break;
    case database_use::rejected: line += title_id + " entry not valid for this build (unknown key or value), not applied"; break;
    case database_use::manual: line += title_id + " entry not applied: settings saved from the per-game menu run as saved"; break;
    case database_use::custom_config: line += title_id + " entry not applied: automatic configuration off and the game has its own config (as on PC)"; break;
    case database_use::skipped:
        line += title_id + " entry skipped";
        if (!skip_reason.empty()) line += " (" + std::string(skip_reason) + ")";
        line += ": the title runs as without a database (its fallback rows included)";
        break;
    }
    return line;
}

// 0.40: why config_db_entry is off for a boot: its kill flag or the safe-mode level.
inline std::string database_skip_reason(const runtime_inputs& in)
{
    const safe_mode::info& row = safe_mode::row(safe_mode::feature::config_db_entry);
    switch (safe_mode::why(in.features_in, row))
    {
    case safe_mode::reason::kill_flag: return row.kill_flag;
    case safe_mode::reason::safe_level: return safe_reason(in);
    default: break;
    }
    return {};
}

// 0.40: custom_configs/<TITLE_ID>.settings-notice holds the build version whose boot toast about the game's replaced
// legacy values was shown; true (and the version written) when this build has not shown it yet.
inline std::string legacy_notice_marker(std::string_view title_id)
{
    if (title_id.empty()) return {};
    return rpcs3::utils::get_custom_config_dir() + std::string(title_id) + ".settings-notice";
}

inline bool legacy_notice_due(std::string_view title_id)
{
    const std::string marker = legacy_notice_marker(title_id);
    if (marker.empty()) return false;
    std::string shown = policy_detail::read_small(marker, 64);
    while (!shown.empty() && (shown.back() == '\n' || shown.back() == '\r' || shown.back() == ' ')) shown.pop_back();
    if (shown == ps4_build_version) return false;
    fs::create_path(rpcs3::utils::get_custom_config_dir());
    // Shown also when the marker cannot be written: the next boot then shows it again
    policy_detail::write_note(marker, std::string(ps4_build_version) + "\n");
    return true;
}

// After the policy ran for a game boot: a startup.log line with the runtime state, the adaptation bookkeeping
// (one "boot" line, or the file removed once nothing is active) and one toast when the game was adapted.
inline void record_boot(const std::string& title_id, const cfg_root& cfg, const runtime_inputs& in, const policy_result& result,
    const pinned_settings& pins = {})
{
    if (title_id.empty()) return;
    std::string line = "Política PS4 para " + title_id + ": ";
    line += result.online ? "online RPCN ligado"
        : result.online_skipped ? (adapt_has(in, adapt::key_threads_low) ? "online pulado (limite de threads)" : "online pulado (modo seguro nível " +
                                       std::to_string(std::max<unsigned>(in.safe_level, in.global_safe ? 1u : 0u)) + ")")
        : result.online_no_account ? "online RPCN sem conta (offline neste boot)"
        : "offline";
    line += cfg.net.upnp_enabled ? ", UPnP ligado" : ", UPnP desligado";
    line += " · VRAM até " + std::to_string(cfg.video.vk.vram_allocation_limit.get()) + " MB";
    line += " · cache de disco até " + std::to_string(cfg.vfs.cache_max_size.get()) + " MB";
    line += compile_3_note(cfg, in);
    // 0.39: the shader mode (and why, after a GPU hang), the OpenGL ZCULL pair and the game language with its source
    line += " · shader=" + cfg.video.shadermode.to_string();
    if (adapt_no_shader_interpreter(in) && cfg.video.renderer == video_renderer::vulkan && cfg.video.shadermode == shader_mode::async_recompiler)
    {
        line += adapt_has(in, adapt::key_gpu_hang) ? std::string(" (async recompiler after a GPU hang: adapt gpu_hang)")
            : in.safe_kind == safe_mode::fail_kind::gpu ? " (async recompiler after a GPU hang: " + safe_reason(in) + ")"
            : " (async recompiler without the shader interpreter: " + safe_reason(in) + ")";
    }
    if (cfg.video.renderer == video_renderer::opengl)
    {
        // 0.40: the strict pair unless gl_relaxed_zcull (its file, or features.txt); zcull-strict-gl.flag is its kill flag
        line += std::string(" · OpenGL ZCULL ") + (cfg.video.relaxed_zcull_sync.get() ? "relaxed" : "strict");
        const safe_mode::info& row = safe_mode::row(safe_mode::feature::gl_relaxed_zcull);
        if (feature_on(in, row.f)) line += " (gl_relaxed_zcull)";
        else if (safe_mode::why(in.features_in, row) == safe_mode::reason::kill_flag) line += " (zcull-strict-gl.flag)";
    }
    // 0.40: 'auto' is English (US); the PS4 language comes from 'Idioma do PS4' or features.txt game_language_ps4
    line += " · idioma do jogo " + cfg.sys.language.to_string();
    const std::int32_t running_language = static_cast<std::int32_t>(cfg.sys.language.get());
    if (in.game_language.value >= 0)
        line += in.game_language.source == game_language_source::game ? " (este jogo)" : " (Configurações)";
    else if (in.game_language.value == game_language_system && in.game_language_system_on && running_language == in.game_language_system)
        line += " (Idioma do PS4, PS4 " + std::to_string(in.system_language) + ")";
    else if (running_language == in.game_language_auto)
    {
        if (in.game_language.value == game_language_system) line += " (auto: Idioma do PS4 desligado pelo modo seguro)";
        else if (feature_on(in, safe_mode::feature::game_language_ps4)) line += " (auto, PS4 " + std::to_string(in.system_language) + ": game_language_ps4)";
        else line += " (auto)";
    }
    else line += " (YAML)";
    // 0.40: the per-game safe mode (the boot items above) and the global one
    if (in.safe_level)
    {
        line += " · modo seguro nível " + std::to_string(in.safe_level) + " (" + i18n::tr(fail_text(in.safe_kind), i18n::lang::pt_br) + ", este boot e mais " +
            std::to_string(in.safe_boots_left ? in.safe_boots_left - 1 : 0) + ")";
        if (in.safe_exhausted) line += ": o nível 3 falhou de novo";
        if (in.global_safe) line += " · modo seguro global";
    }
    else if (in.global_safe)
        line += " · modo seguro global (safe-mode.flag)";
    ps4_startup_checkpoint(line.c_str());
    if (const std::string own = pins_line(title_id, pins); !own.empty()) ps4_startup_checkpoint(own.c_str());
    // 0.40: the values of the player's files that run otherwise, and who replaced them (also in RPCS3.log)
    if (const std::string diff = settings_diff_line(title_id, result.settings_diffs); !diff.empty())
    {
        ps4_startup_checkpoint(diff.c_str());
#ifdef RPCS3_PS4
        ps4_policy_log.notice("%s", diff);
#endif
    }
    for (const std::string& note : result.notes) ps4_startup_checkpoint(note.c_str());
    const config_db::file_info database_file = config_db::status();
    ps4_startup_checkpoint(config_db::describe(database_file).c_str());
    if (const std::string db_line = database_line(title_id, result.database, database_file, database_skip_reason(in)); !db_line.empty())
        ps4_startup_checkpoint(db_line.c_str());

    // A game that starts another of its executables, or a restart from the home menu, is still the same session:
    // only a boot from the XMB (the title was cleared on the way back) counts toward the expiry.
    char running[adapt::title_size];
    const bool same_session = adapt::current_title(running) && title_id == running;

    // 0.39: a PPU or SPU interpreter (the player's YAML or the title's PC database entry) runs 10-50x slower than LLVM
    // here: one line per decoder, one toast per boot from the XMB.
    {
        const bool ppu = cfg.core.ppu_decoder == ppu_decoder_type::_static;
        const bool spu = cfg.core.spu_decoder == spu_decoder_type::_static || cfg.core.spu_decoder == spu_decoder_type::dynamic;
        const auto source_log = [](bool database) { return database ? "PC database entry" : "user setting"; };
        if (ppu)
            ps4_startup_checkpoint(("Política PS4 para " + title_id + ": PPU decoder = interpreter (" + source_log(result.ppu_decoder_from_database) +
                "): 10-50x slower than LLVM on this CPU").c_str());
        if (spu)
            ps4_startup_checkpoint(("Política PS4 para " + title_id + ": SPU decoder = interpreter (" + source_log(result.spu_decoder_from_database) +
                "): 10-50x slower than LLVM on this CPU").c_str());
        if ((ppu || spu) && !same_session)
        {
            using i18n::str;
            const bool database = (!ppu || result.ppu_decoder_from_database) && (!spu || result.spu_decoder_from_database);
            const bool user = (!ppu || !result.ppu_decoder_from_database) && (!spu || !result.spu_decoder_from_database);
            const str source = database ? str::POL_PEDIDO_BASE_PC : user ? str::POL_ESCOLHIDO_POR_VOCE : str::POL_ESCOLHA_VOCE_BASE_PC;
            const std::string toast = ppu && spu ? i18n::trf(str::POL_INTERPRETER_TOAST, {title_id, str::POL_PPU_E_SPU, source})
                                                 : i18n::trf(str::POL_INTERPRETER_TOAST, {title_id, ppu ? "PPU" : "SPU", source});
            ps4_notify(toast.c_str());
        }
    }
    if (same_session) return;

    // 0.40: Modo online without an RPCN account: one toast per boot from the XMB
    if (result.online_no_account) ps4_notify(i18n::trf(i18n::str::POL_ONLINE_SEM_CONTA, {title_id}).c_str());
    // 0.40: values of the game's own format-1 YAML the automatic configuration replaced: one toast per title and build
    // version, pointing at the per-game Converter (which turns them into the player's pins on request)
    if (result.legacy_game_values && legacy_notice_due(title_id))
        ps4_notify(i18n::trf(i18n::str::POL_AJUSTES_ARQUIVO_SEM_EFEITO, {result.legacy_game_values}).c_str());

    if (in.adapt.empty())
    {
        // Same I/O as the adaptation file itself (util/ps4_adapt.hpp); empty = no file.
        if (adapt::read_text(title_id).empty()) return;
        const bool removed = adapt::remove(title_id);
        line = "adapt: " + title_id + ": no active adjustment left, " + (removed ? "file removed" : "file could not be removed");
        ps4_startup_checkpoint(line.c_str());
        return;
    }

    adapt::note_boot(title_id);
    line = "adapt: " + title_id + " keys";
    for (std::size_t i = 0; i < in.adapt.size(); i++) line += (i ? "," : " ") + in.adapt[i];
    line += " (this boot and " + std::to_string(in.adapt_boots_left ? in.adapt_boots_left - 1 : 0) + " more):";
    // The toast lists what changed, in the interface language
    using i18n::str;
    std::string toast;
    const auto add = [&](std::string_view log, str text)
    {
        line += " ";
        line += log;
        if (text == str::count_) return;
        if (!toast.empty()) toast += i18n::tr(str::POL_LIST_SEPARATOR);
        toast += i18n::tr(text);
    };
    if (adapt_fewer_threads(in)) add("llvm_threads=1 shader_threads=1", str::POL_COMPILACAO_1_THREAD);
    if (adapt_has(in, adapt::key_vram_low)) add("vram=" + std::to_string(cfg.video.vk.vram_allocation_limit.get()) + "MB", str::POL_MEMORIA_VIDEO_REDUZIDA);
    if (result.online_skipped) add("online=skipped", str::POL_ONLINE_DESATIVADO_ESTE_JOGO_LIMITE);
    if (adapt_has(in, adapt::key_gpu_hang))
    {
        if (cfg.video.renderer == video_renderer::vulkan && cfg.video.shadermode == shader_mode::async_recompiler) add("gpu_hang shader=async_recompiler", str::POL_SHADERS_SEM_INTERPRETADOR);
        else add("gpu_hang=recorded", str::count_);
    }
    // 0.40: after a device loss the Vulkan renderer splits command buffers (row cb_split_after_loss, kill cb-split-off.flag)
    if (adapt_has(in, adapt::key_cb_split))
    {
        if (!feature_on(in, safe_mode::feature::cb_split_after_loss)) add("cb_split=off(cb-split-off.flag)", str::count_);
        else if (cfg.video.renderer == video_renderer::vulkan) add("cb_split=384_draws", str::POL_COMANDOS_GPU_DIVIDIDOS);
        else add("cb_split=vulkan_only", str::count_);
    }
    ps4_startup_checkpoint(line.c_str());
    // 0.40: a safe-mode level names the level and the failure instead of the adjustments: level 3 that failed again
    // asks for the logs, a level the player chose says so
    if (in.safe_level)
    {
        const std::string safe_toast = in.safe_exhausted ? i18n::trf(str::POL_SAFE_MODE_EXHAUSTED, {title_id})
            : in.safe_kind == safe_mode::fail_kind::user || in.safe_kind == safe_mode::fail_kind::none
                ? i18n::trf(str::POL_SAFE_MODE_USER_TOAST, {title_id, unsigned{in.safe_level}})
                : i18n::trf(str::POL_SAFE_MODE_TOAST, {title_id, fail_text(in.safe_kind), unsigned{in.safe_level}});
        ps4_notify(safe_toast.c_str());
        return;
    }
    if (!toast.empty())
    {
        toast = i18n::trf(str::POL_ADAPT_TOAST, {title_id, i18n::raw(toast)});
        ps4_notify(toast.c_str());
    }
}

// 'given' = null reads the flags and the adaptation file; 'boot' = false is the side-effect-free preview. 'given_pins' =
// null reads the pins of ps4-settings.yml and of the game's YAML (read_pins); the menu passes the ones it is editing.
template <typename OnChange>
policy_result run_boot_policy(cfg_root& cfg, const std::string& title_id, const runtime_inputs* given, bool boot, OnChange& on_change,
    const pin_set* given_pins = nullptr)
{
    policy_result result;
    result.mode = get_policy_mode(title_id);
    const runtime_inputs in = given ? *given : read_runtime_inputs(title_id);
    // 0.40: the title's safe-mode level for set_for_boot, in every mode; the game language row's 'Idioma do PS4' turns
    // game_language_ps4 on for that boot (nothing is forced with keep-user-settings.flag, so neither is the language)
    result.safe_level = in.safe_level;
    result.safe_kind = in.safe_kind;
    if (boot)
    {
        const bool language_row = result.mode != policy_mode::keep_user_settings && in.game_language.value == game_language_system;
        safe_mode::set_boot_enables(language_row ? safe_mode::bit(safe_mode::feature::game_language_ps4) : 0);
        // 0.40: the command-buffer split after a device loss of this title (adapt cb_split, 8 boots), for the renderer of
        // this boot; read here, before record_boot counts the boot, in every mode (a renderer code path, not a setting).
        // The emulator's own pass (no title) clears the previous game's decision.
        policy::g_cb_split_boot.store(!title_id.empty() && adapt_has(in, adapt::key_cb_split), std::memory_order_relaxed);
    }
    if (result.mode == policy_mode::keep_user_settings)
    {
        // The level's feature rows run in this mode too, so its boots from the XMB count toward its 8-boot expiry
        // (record_boot, which counts them otherwise, does not run: nothing is forced)
        char running[adapt::title_size];
        if (boot && in.safe_level && !title_id.empty() && !(adapt::current_title(running) && title_id == running)) adapt::note_boot(title_id);
        if (boot && title_id.empty()) g_emulator_pass_diffs.clear();
        return result;
    }
    // 0.40: what the steps of a boot replace, for the 'settings:' line (none for the menu's preview)
    std::vector<diff_note> diff_notes;
    // Before any step that reads the database: a skipped entry makes the title's fallback rows run (title_row_active)
    config_db::set_skipped(title_id, !feature_on(in, safe_mode::feature::config_db_entry));
    const pinned_settings pinned = resolve_pins(cfg, given_pins ? *given_pins : read_pins(title_id));
    const pinned_settings* pins = pins_or_null(pinned);
    result.pinned_game = static_cast<u32>(pinned.game.size());
    result.pinned_global = static_cast<u32>(pinned.global.size());
    const auto counted = [&](const cfg::_base& setting, const std::string& old, std::string_view why, change_kind kind)
    {
        switch (kind)
        {
        case change_kind::profile: result.profile_changes++; break;
        case change_kind::title:
            result.title_changes++;
            // 0.39: the decoders the database entry sets (the interpreter warning names its source)
            if (is_database_reason(why))
            {
                if (&setting == &cfg.core.ppu_decoder) result.ppu_decoder_from_database = true;
                if (&setting == &cfg.core.spu_decoder) result.spu_decoder_from_database = true;
            }
            break;
        case change_kind::adapt: result.title_changes++; result.adapt_changes++; break;
        case change_kind::runtime: result.runtime_changes++; break;
        case change_kind::console:
            result.console_changes++;
            // 0.37: an unavailable option replaced (a copied config, a database entry) gets a startup.log line. 0.40: so does
            // a single unsafe value and the Resolution Scale cap.
            if (unsafe_option_for(cfg, setting) || unsafe_value_for(setting) || &setting == &cfg.video.resolution_scale_percent)
                result.notes.push_back("console limit: '" + setting.get_name() + "' = '" + setting_text(setting) + "' (was '" + old + "'): " + std::string(why));
            else if (why.starts_with(platform_reason_prefix))
                result.notes.push_back("platform: '" + setting.get_name() + "' = '" + setting_text(setting) + "' (was '" + old + "'): " +
                    std::string(why.substr(platform_reason_prefix.size())));
            break;
        case change_kind::migration: result.migrated_changes++; break;
        }
        if (boot) note_diff(diff_notes, cfg, setting, old, why, kind);
        report(on_change, setting, old, why, kind);
    };
    // 0.37: values of a saved YAML that an older profile forced get the current profile's values. Before the marker
    // check, so a save that differs from the automatic configuration only in such values is a stale marker too.
    if (result.mode != policy_mode::automatic)
    {
        std::vector<std::string> lines;
        apply_profile_migration(cfg, title_id, saved_settings_text(title_id), counted, &lines, pins, in.features);
        result.notes.insert(result.notes.end(), lines.begin(), lines.end());
    }
    // 0.32: a marker whose saved YAML runs exactly like the automatic configuration (e.g. Demon's Souls saved with
    // Write Color Buffers on, which its automatic row now sets) no longer keeps the game out of it. The marker file
    // stays until the next menu save or 'Voltar para automático'.
    if (result.mode == policy_mode::user_tuned && saved_equals_automatic(cfg, title_id, pins, in.features))
    {
        result.mode = policy_mode::automatic;
        result.stale_marker = true;
    }
    if (result.mode == policy_mode::automatic) apply_performance_policy(cfg, title_id, counted, &result.database, pins, in.features);
    else
    {
        // 0.33: PC RPCS3 adds the database entry only to a game without a custom config. User-tuned games never get
        // it; with the automatic configuration off, only a game without its own YAML does (before its every-mode rows,
        // which keep precedence).
        if (result.mode == policy_mode::auto_config_off && !has_custom_config(title_id))
            apply_config_database(cfg, title_id, counted, false, &result.database, pins, in.features);
        else if (const config_db::entry entry = config_db::lookup(title_id); !entry.config.empty())
        {
            result.database.entry = true;
            result.database.use = entry.skipped ? database_use::skipped
                : result.mode == policy_mode::user_tuned ? database_use::manual : database_use::custom_config;
        }
        apply_title_compat(cfg, title_id, counted, false, pins);
    }
    apply_console_limits(cfg, counted);
    // 0.40: what the renderer would replace at start anyway, so the preview and the logs show it
    apply_platform_limits(cfg, counted);
    apply_runtime_limits(cfg, in, counted);
    if (result.mode == policy_mode::automatic) apply_cool_mode(cfg, in, counted, pins);
    // After the database step: a rejected entry no longer counts, and its fallback rows are then active.
    result.host_sleep_requested = (result.database.use == database_use::applied && result.database.host_sleep) ||
        title_rows_ask_host_sleep(title_id, result.mode == policy_mode::automatic, &cfg, pins);
    result.online_skipped = online_blocked(in);
    result.online_no_account = online_no_account(in);
    result.online = in.online && !result.online_skipped && !result.online_no_account;
    result.adapt_keys = in.adapt;
    // 0.40: what the applied database entry asked and what runs now (the startup.log line), after every step
    if (boot && result.database.use == database_use::applied)
        result.database.items = database_items(cfg, title_id, config_db::lookup(title_id).config, result.mode == policy_mode::automatic, pins);
    if (boot)
    {
        result.settings_diffs = finish_diffs(cfg, title_id, diff_notes);
        if (result.mode == policy_mode::automatic && has_custom_config(title_id))
            result.legacy_game_values = legacy_game_values(result.settings_diffs,
                read_small(rpcs3::utils::get_custom_config_path(title_id), settings_text_limit));
    }
    if (boot) record_boot(title_id, cfg, in, result, pinned);
    return result;
}
} // namespace policy_detail

// What fixup_settings does on every boot: decide the mode, apply the automatic profile and the title's config
// database entry (automatic) or only the every-mode per-title rows (user-tuned, switched off; the database entry too
// for a switched-off game without its own YAML), then the console limits and the runtime limits; nothing with the
// keep flag.
// For a game it also counts the boot in its adaptation file and reports what was adapted (startup.log, toast).
// OnChange: void(const cfg::_base& setting, const std::string& old_value, std::string_view why[, change_kind]).
// 'pins' = null reads the player's pins from the settings files (read_pins).
template <typename OnChange>
policy_result apply_boot_policy(cfg_root& cfg, const std::string& title_id, OnChange&& on_change, const pin_set* pins = nullptr)
{
    return policy_detail::run_boot_policy(cfg, title_id, nullptr, true, on_change, pins);
}

// Boot toast of the automatic configuration, in the interface language; empty = no toast. Shown only when the title's
// own fixes (per-title rows, its PC config database entry) changed a value: the profile values and console limits are
// the same for every game, and the adaptation after a failure has its own toast (record_boot), so a count of those
// ("0 ajuste(s)" included) read like an error. The startup.log line keeps every count.
inline std::string automatic_config_toast(const std::string& title_id, const policy_result& result)
{
    if (title_id.empty() || result.mode != policy_mode::automatic) return {};
    const u32 own = result.title_changes - std::min(result.adapt_changes, result.title_changes);
    if (!own) return {};
    return i18n::trf(i18n::str::POL_AUTO_CONFIG_TOAST, {title_id, own});
}

// 0.31: the same values as apply_boot_policy without any side effect (no adaptation bookkeeping, no log line, no
// toast), for the settings menu: opening the menu must not count as a boot.
template <typename OnChange>
policy_result apply_boot_policy_preview(cfg_root& cfg, const std::string& title_id, OnChange&& on_change, const pin_set* pins = nullptr)
{
    return policy_detail::run_boot_policy(cfg, title_id, nullptr, false, on_change, pins);
}

// Preview with inputs the caller already read (read_runtime_inputs) and, optionally, the pins it is editing.
template <typename OnChange>
policy_result apply_boot_policy_preview(cfg_root& cfg, const std::string& title_id, const runtime_inputs& inputs, OnChange&& on_change,
    const pin_set* pins = nullptr)
{
    return policy_detail::run_boot_policy(cfg, title_id, &inputs, false, on_change, pins);
}
} // namespace utils::ps4
