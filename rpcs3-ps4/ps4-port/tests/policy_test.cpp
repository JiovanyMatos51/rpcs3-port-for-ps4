// Host test for the PS4 boot policy (rpcs3/util/ps4_policy.hpp): the online switch against old full-dump per-game
// YAMLs, keep-user-settings.flag, the video memory cap and vram.flag, the console-safety values, the per-title
// rows, and the per-title adaptation (expiry after 3 boots, relaunch, threads=low keeps a game offline).
// 0.32: the console limits in every mode, the Demon's Souls Write Color Buffers row and the stale user-tuned marker.
// 0.33: the PC config database (reader, the database step in each mode, caps, row precedence, rejected entries and
// the God of War fallback rows), 'Use LLVM CPU' as a console limit and the per-game accurate-dfma marker.
// 0.34: 'Use native user interface' as a console limit, host_sleep_requested (an entry or a fallback row asking
// 'Sleep Timers Accuracy: As Host'), the 'Modo frio' pad interval and the Sonic / Silent Hill fallback rows; the
// vk-compile-3.flag third shader compile worker.
// 0.37: the bundled config database (/app0/config_database.dat) and its precedence against the /data copy, the PC
// defaults for Shader Mode and ZCULL with their escape files, the profile version stamp and the migration of values an
// older profile forced into a saved YAML, 'Disable SPU GETLLAR Spin Optimization' as a console limit, the Sonic row's
// Write Color Buffers and God of War: Ascension's Accurate ZCULL stats row value.
// 0.39: the 64 ms audio reserve and the profile 39 migrations, the OpenGL ZCULL pair (zcull-strict-gl.flag) and
// gl-async.flag, the Async Recompiler after a GPU hang (adapt gpu_hang), the PPU/SPU interpreter warning (user YAML or
// database entry) and the game language row (game-language.txt, per-game marker, automatic value, keep flag); the
// automatic configuration's boot toast only when the title's own fixes apply; the menu texts and the toasts come from
// util/ps4_i18n.hpp (pt-BR unless a test switches the language).
// Settings format 2: the 'PS4 user settings:' pins (parser, writer, keys, read_pins), their precedence at boot (profile,
// database entry, rows, migration, Modo frio vs every-mode rows, console / runtime / hardware limits, global vs game),
// the pins line of startup.log, the user-tuned marker with a format-2 file, and the four application console limits.
// 0.40: the 0.38 defaults behind their feature rows (34 ms reserve, strict OpenGL pair, English (US) as the automatic
// game language) with their enable flags, features.txt and kill flags, profile 40 and its migrations (stamp 39 back,
// pins kept), the 'Idioma do PS4' row value, Modo online without an RPCN account (rpcn.yml rule, offline boot, toast,
// online-without-account.flag), the per-game safe mode at boot (items per level and kind, toasts, the policy line,
// the level in every mode), config-database-off.flag and level 2 (the entry skipped, fallback rows), the database line
// with the entry's settings, and the boots-left count of the safe family. Also 0.40: the command-buffer split after a
// device loss (adapt cb_split: the stored renderer decision, the adapt line, the toast, 8 boots), a thread failure that
// keeps the database entry, custom_configs/<TITLE_ID>.features, the Resolution Scale cap, the platform step (OpenGL
// legacy recompiler without gl-async.flag), the disk cache bound from the free space, the 'settings:' line with the
// emulator pass merged and the once-per-build toast, and legacy_choices().
//
// The RPCS3 config tree and file system are replaced by tests/policy-mocks (same setting names, value spellings
// and bounds; /data/rpcs3 flag files live in memory). The adaptation files use their real I/O in a directory
// created next to the test, which the test removes again; so do the feature row files of util/ps4_safe_mode.hpp
// (its own stat/fopen reads, RPCS3_PS4_SAFE_MODE_DIR).
//
// Build and run from any scratch directory (llvm-mingw, already used for the other host tests):
//   clang++ -std=c++20 -static -Wall -Wextra -I ps4-port/tests/policy-mocks -I rpcs3 ps4-port/tests/policy_test.cpp -o policy_test.exe
//   policy_test.exe

#define RPCS3_PS4_ADAPT_DIR "policy-test-adapt"
#define RPCS3_PS4_SAFE_MODE_DIR "policy-test-safe"
#include "util/ps4_policy.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{
std::vector<std::string> g_checkpoints;
std::vector<std::string> g_toasts;
int g_failures = 0;

#define CHECK(expr) \
    do \
    { \
        if (!(expr)) \
        { \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
            g_failures++; \
        } \
    } while (0)

using namespace utils::ps4;

constexpr char minecraft[] = "BLUS31426";
constexpr char demons_souls[] = "BLUS30443";
constexpr char demons_souls_eu[] = "BLES00932";
constexpr char ben10[] = "BLUS30663";
constexpr char terraria[] = "BLUS31537";

struct change
{
    std::string name, old, now, why;
    change_kind kind;
};

policy_result boot(cfg_root& cfg, const std::string& title, std::vector<change>* out = nullptr)
{
    return apply_boot_policy(cfg, title, [&](const cfg::_base& setting, const std::string& old, std::string_view why, change_kind kind)
    {
        if (out) out->push_back({setting.get_name(), old, setting.to_string(), std::string(why), kind});
    });
}

unsigned count_named(const std::vector<change>& changes, std::string_view name)
{
    unsigned n = 0;
    for (const auto& c : changes) n += c.name == name;
    return n;
}

const change* find_named(const std::vector<change>& changes, std::string_view name)
{
    for (const auto& c : changes)
        if (c.name == name) return &c;
    return nullptr;
}

bool contains(const std::vector<std::string>& lines, std::string_view text)
{
    for (const auto& line : lines)
        if (line.find(text) != std::string::npos) return true;
    return false;
}

unsigned boot_lines(std::string_view title)
{
    const std::string text = adapt::read_text(title);
    unsigned n = 0;
    std::size_t at = 0;
    while ((at = text.find("boot\n", at)) != std::string::npos) n++, at += 5;
    return n;
}

// 0.40: a feature row file (enable or kill flag, features.txt, safe-mode.flag) of util/ps4_safe_mode.hpp, real I/O.
void safe_file(const char* name, bool on, std::string_view text = {})
{
    std::error_code ec;
    const std::filesystem::path path = std::filesystem::path(RPCS3_PS4_SAFE_MODE_DIR) / name;
    if (!on)
    {
        std::filesystem::remove(path, ec);
        return;
    }
    std::filesystem::create_directories(RPCS3_PS4_SAFE_MODE_DIR, ec);
    std::ofstream(path, std::ios::binary) << text;
}

// An rpcn.yml with an account (an NPID and a derived password), in the fs mock's config directory.
void give_rpcn_account(bool on = true)
{
    if (on)
        fs::test_files()[rpcn_config_path()] = "Version: 2\nHost: np.rpcs3.net\nNPID: Maestro_77\n"
                                               "Password: 0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF\nToken: \"\"\n";
    else fs::test_files().erase(rpcn_config_path());
}

void reset_state()
{
    fs::test_files().clear();
    config_db::forget();
    {
        std::error_code ec;
        std::filesystem::remove_all(RPCS3_PS4_SAFE_MODE_DIR, ec);
    }
    safe_mode::set_boot_enables(0);
    // cool-mode.flag is read through the real file system (util/ps4_cool_mode.hpp); the tests set its cached state.
    cool_mode::detail::state().store(0, std::memory_order_relaxed);
    g_checkpoints.clear();
    g_toasts.clear();
    adapt::clear_title();
    for (const char* title : {minecraft, demons_souls, demons_souls_eu, ben10, terraria}) adapt::remove(title);
}

// An old per-game YAML saved from the 0.28 menu: a full dump, offline, plus values a copied PC config carries.
constexpr char old_full_dump[] =
    "Core:\n"
    "  Max LLVM Compile Threads: 0\n"
    "Video:\n"
    "  Renderer: Vulkan\n"
    "  Vulkan:\n"
    "    VRAM allocation limit (MB): 2048\n"
    "VFS:\n"
    "  Enable /host_root/: true\n"
    "  Limit disk cache size: false\n"
    "  Disk cache maximum size (MB): 0\n"
    "Net:\n"
    "  Internet enabled: Disconnected\n"
    "  UPNP Enabled: true\n"
    "  PSN status: Disconnected\n";

void test_defaults_offline()
{
    reset_state();
    cfg_root cfg;
    std::vector<change> changes;
    const policy_result r = boot(cfg, minecraft, &changes);
    CHECK(r.mode == policy_mode::automatic);
    CHECK(!r.online && !r.online_skipped && r.adapt_keys.empty());
    CHECK(cfg.net.net_active.get() == np_internet_status::disabled);
    CHECK(cfg.net.psn_status.get() == np_psn_status::disabled);
    CHECK(!cfg.net.upnp_enabled.get());
    CHECK(!cfg.vfs.host_root.get());
    CHECK(cfg.vfs.limit_cache_size.get());
    CHECK(cfg.vfs.cache_max_size.get() == 4096);
    CHECK(cfg.video.vk.vram_allocation_limit.get() == 768);
    CHECK(cfg.video.renderer.get() == video_renderer::vulkan);
    CHECK(cfg.core.llvm_threads.get() == 2);
    CHECK(cfg.video.shader_compiler_threads_count.get() == 2);
    // limit_cache_size, cache_max_size and the VRAM cap; nothing else in the runtime section.
    CHECK(r.runtime_changes == 3);
    CHECK(r.adapt_changes == 0 && r.title_changes == 0);
    CHECK(r.console_changes == 0 && !r.stale_marker); // upstream defaults are the safe values
    CHECK(find_named(changes, "VRAM allocation limit (MB)") && find_named(changes, "VRAM allocation limit (MB)")->kind == change_kind::runtime);
    CHECK(contains(g_checkpoints, "Política PS4 para BLUS31426: offline, UPnP desligado · VRAM até 768 MB · cache de disco até 4096 MB"));
    CHECK(g_toasts.empty());
    CHECK(adapt::read_text(minecraft).empty()); // a game that never failed gets no adaptation file
}

void test_online_flag_with_old_yaml()
{
    for (const int mode : {0, 1, 2}) // automatic, user-tuned, automatic configuration off
    {
        reset_state();
        give_rpcn_account();
        CHECK(set_online_enabled(true));
        CHECK(online_enabled());
        if (mode == 1) CHECK(set_user_tuned(minecraft, true));
        if (mode == 2) CHECK(set_auto_config_enabled(false));
        cfg_root cfg;
        CHECK(cfg.from_string(old_full_dump));
        CHECK(cfg.net.net_active.get() == np_internet_status::disabled && cfg.vfs.host_root.get());
        const policy_result r = boot(cfg, minecraft);
        CHECK(r.mode == (mode == 0 ? policy_mode::automatic : mode == 1 ? policy_mode::user_tuned : policy_mode::auto_config_off));
        CHECK(r.online && !r.online_skipped);
        CHECK(cfg.net.net_active.get() == np_internet_status::enabled);
        CHECK(cfg.net.psn_status.get() == np_psn_status::psn_rpcn);
        CHECK(!cfg.net.upnp_enabled.get()); // the YAML said true, but UPnP needs its own switch
        CHECK(!cfg.vfs.host_root.get());
        CHECK(cfg.vfs.limit_cache_size.get() && cfg.vfs.cache_max_size.get() == 1024); // 0 would wipe hdd1 caches every boot
        CHECK(cfg.video.vk.vram_allocation_limit.get() == 768);
        CHECK(cfg.core.llvm_threads.get() == 2); // the profile in automatic mode, the console limit in the others
        CHECK(r.console_changes == (mode == 0 ? 0u : 2u)); // LLVM and shader threads (0 -> 2) outside automatic mode
        CHECK(contains(g_checkpoints, "online RPCN ligado, UPnP desligado"));
    }

    // UPnP: only with its switch and only while online.
    reset_state();
    give_rpcn_account();
    CHECK(set_online_enabled(true) && set_upnp_enabled(true) && upnp_enabled());
    {
        cfg_root cfg;
        boot(cfg, minecraft);
        CHECK(cfg.net.upnp_enabled.get());
    }
    CHECK(set_online_enabled(false) && !online_enabled());
    {
        cfg_root cfg;
        cfg.net.net_active.set(np_internet_status::enabled);
        cfg.net.psn_status.set(np_psn_status::psn_rpcn);
        cfg.net.upnp_enabled.set(true);
        const policy_result r = boot(cfg, minecraft);
        CHECK(!r.online);
        CHECK(cfg.net.net_active.get() == np_internet_status::disabled);
        CHECK(cfg.net.psn_status.get() == np_psn_status::disabled);
        CHECK(!cfg.net.upnp_enabled.get());
    }
    CHECK(set_upnp_enabled(false) && !upnp_enabled());
    CHECK(set_upnp_enabled(false)); // removing a missing flag is not an error
}

// 0.40: Modo online without an RPCN account boots offline (no NP sign-in can work; Minecraft waited for its world
// server forever with the network up and NP offline). The account rule reads rpcn.yml as text, never writes it.
void test_online_without_account()
{
    // The rule
    const std::string password(64, 'A');
    CHECK(rpcn_account_present_in("Version: 2\nNPID: Maestro_77\nPassword: " + password + "\n"));
    CHECK(rpcn_account_present_in("NPID: \"Maestro_77\"\r\nPassword: '" + std::string(64, 'f') + "'\r\n"));
    CHECK(!rpcn_account_present_in("Version: 2\nNPID: \"\"\nPassword: " + password + "\n"));          // no NPID
    CHECK(!rpcn_account_present_in("Version: 2\nNPID: Maestro_77\nPassword: \"\"\n"));               // no password
    CHECK(!rpcn_account_present_in("Version: 2\nNPID: Maestro_77\nPassword: hunter2\n"));            // a typed password
    CHECK(!rpcn_account_present_in("Version: 2\nNPID: Maestro_77\nPassword: " + std::string(64, 'G') + "\n"));
    CHECK(!rpcn_account_present_in("Version: 1\nNPID: Maestro_77\nPassword: " + password + "\n"));   // load() clears it
    CHECK(!rpcn_account_present_in("") && !rpcn_account_present_in("Host: np.rpcs3.net\n"));
    CHECK(!rpcn_account_present_in("Account:\n  NPID: Maestro_77\n  Password: " + password + "\n")); // top-level keys only

    // The switch on, no rpcn.yml: offline, one line, one toast per boot from the XMB, rpcn.yml never created
    reset_state();
    CHECK(set_online_enabled(true));
    {
        cfg_root cfg;
        std::vector<change> changes;
        const policy_result r = boot(cfg, minecraft, &changes);
        CHECK(!r.online && r.online_no_account && !r.online_skipped);
        CHECK(cfg.net.net_active.get() == np_internet_status::disabled && cfg.net.psn_status.get() == np_psn_status::disabled);
        const change* c = find_named(changes, "Internet enabled");
        CHECK(!c || c->why.find("no RPCN account") != std::string::npos);
        CHECK(contains(g_checkpoints, "Política PS4 para BLUS31426: online RPCN sem conta (offline neste boot), UPnP desligado"));
        CHECK(g_toasts.size() == 1 && contains(g_toasts, "RPCS3: Modo online sem conta RPCN: BLUS31426 abre offline"));
        CHECK(!fs::is_file(rpcn_config_path()));
        CHECK(!read_runtime_inputs(minecraft).rpcn_account);
    }
    // A YAML that says Connected (a PC copy) gets the same
    {
        cfg_root cfg;
        cfg.net.net_active.set(np_internet_status::enabled);
        cfg.net.psn_status.set(np_psn_status::psn_rpcn);
        const policy_result r = boot(cfg, minecraft);
        CHECK(!r.online && cfg.net.net_active.get() == np_internet_status::disabled);
    }
    // A relaunch of the running game: no second toast
    {
        CHECK(adapt::set_title(minecraft));
        g_toasts.clear();
        cfg_root cfg;
        boot(cfg, minecraft);
        CHECK(g_toasts.empty());
        adapt::clear_title();
    }
    // The preview: offline, no line, no toast
    {
        g_checkpoints.clear();
        g_toasts.clear();
        cfg_root cfg;
        const policy_result r = apply_boot_policy_preview(cfg, minecraft, [](const cfg::_base&, const std::string&, std::string_view) {});
        CHECK(r.online_no_account && cfg.net.net_active.get() == np_internet_status::disabled && g_checkpoints.empty() && g_toasts.empty());
    }
    // online-without-account.flag: the 0.39 state (the network up, NP offline at sign-in)
    safe_file("online-without-account.flag", true);
    {
        cfg_root cfg;
        const policy_result r = boot(cfg, minecraft);
        CHECK(r.online && !r.online_no_account && cfg.net.net_active.get() == np_internet_status::enabled);
    }
    safe_file("online-without-account.flag", false);
    // With an account: online
    give_rpcn_account();
    {
        g_toasts.clear();
        cfg_root cfg;
        const policy_result r = boot(cfg, minecraft);
        CHECK(r.online && !r.online_no_account && cfg.net.psn_status.get() == np_psn_status::psn_rpcn && g_toasts.empty());
        CHECK(read_runtime_inputs(minecraft).rpcn_account);
    }
    // The switch off: offline, no account question at all
    give_rpcn_account(false);
    CHECK(set_online_enabled(false));
    {
        g_toasts.clear();
        cfg_root cfg;
        const policy_result r = boot(cfg, minecraft);
        CHECK(!r.online && !r.online_no_account && g_toasts.empty() && read_runtime_inputs(minecraft).rpcn_account);
    }
    // A title on the lighter thread budget stays offline for that reason, not for the account
    CHECK(set_online_enabled(true));
    CHECK(adapt::append(minecraft, adapt::key_threads_low));
    {
        g_checkpoints.clear();
        cfg_root cfg;
        const policy_result r = boot(cfg, minecraft);
        CHECK(r.online_skipped && !r.online_no_account && contains(g_checkpoints, "online pulado (limite de threads)"));
    }
    reset_state();
}

// 0.40: the per-game safe mode at boot: the level and kind of the adapt file (util/ps4_adapt.hpp safe_status), the boot
// items of each level (util/ps4_safe_mode.hpp items_for), the policy line, the toast that replaces the adaptation
// toast, the level in every mode (for set_for_boot), the global safe mode, the expiry after 8 boots.
void test_safe_mode_boot()
{
    using safe_mode::fail_kind;
    i18n::apply(i18n::setting::automatic, -1);
    reset_state();
    give_rpcn_account();
    CHECK(set_online_enabled(true));
    const auto at_level = [](const char* title, const char* fail, const char* level)
    {
        adapt::remove(title);
        CHECK(adapt::append(title, fail) && adapt::append(title, level));
    };
    // The same title without a level: online, two compile threads, the interpreter
    cfg_root base;
    CHECK(boot(base, terraria).online && base.core.llvm_threads.get() == 2 && base.video.shadermode.get() == shader_mode::async_with_interpreter);

    // Level 1 after a crash: feature rows only (no boot item); the toast names the failure instead of the adjustments
    at_level(terraria, adapt::key_fail_crash, adapt::key_safe1);
    {
        g_checkpoints.clear();
        g_toasts.clear();
        cfg_root cfg;
        const policy_result r = boot(cfg, terraria);
        CHECK(r.safe_level == 1 && r.safe_kind == fail_kind::crash);
        CHECK(r.online && cfg.core.llvm_threads.get() == 2 && cfg.video.shader_compiler_threads_count.get() == 2);
        CHECK(cfg.video.shadermode.get() == shader_mode::async_with_interpreter);
        CHECK(contains(g_checkpoints, " · modo seguro nível 1 (erro fatal, este boot e mais 7)"));
        CHECK(contains(g_checkpoints, "adapt: BLUS31537 keys fail=crash,safe=1 (this boot and 7 more):"));
        CHECK(g_toasts.size() == 1 && g_toasts[0] == "RPCS3: BLUS31537 falhou da última vez (erro fatal): abrindo no modo seguro, nível 1 de 3");
        // the next boot counts down
        g_checkpoints.clear();
        cfg_root again;
        boot(again, terraria);
        CHECK(contains(g_checkpoints, " · modo seguro nível 1 (erro fatal, este boot e mais 6)"));
    }
    // Level 1 after a thread failure: the lighter thread budget (online off, one compile thread)
    at_level(terraria, adapt::key_fail_threads, adapt::key_safe1);
    {
        g_checkpoints.clear();
        g_toasts.clear();
        cfg_root cfg;
        cfg.net.net_active.set(np_internet_status::enabled); // a YAML that was online: the change names the reason
        std::vector<change> changes;
        const policy_result r = boot(cfg, terraria, &changes);
        CHECK(r.safe_level == 1 && r.safe_kind == fail_kind::threads && r.online_skipped && !r.online);
        CHECK(cfg.net.net_active.get() == np_internet_status::disabled);
        CHECK(cfg.core.llvm_threads.get() == 1 && cfg.video.shader_compiler_threads_count.get() == 1);
        const change* c = nullptr;
        for (const change& item : changes)
            if (item.name == "Max LLVM Compile Threads") c = &item; // the last change: after the profile's
        CHECK(c && c->kind == change_kind::adapt && c->why == "per-game safe mode level 1 (threads): one compile thread");
        c = find_named(changes, "Internet enabled");
        CHECK(c && c->kind == change_kind::adapt && c->why == "online skipped for this title: per-game safe mode level 1 (threads)");
        CHECK(contains(g_checkpoints, "Política PS4 para BLUS31537: online pulado (modo seguro nível 1), UPnP desligado"));
        CHECK(contains(g_checkpoints, " · modo seguro nível 1 (falta de threads, este boot e mais 7)"));
        CHECK(contains(g_checkpoints, "(this boot and 7 more): llvm_threads=1 shader_threads=1 online=skipped"));
        CHECK(g_toasts.size() == 1 && g_toasts[0] == "RPCS3: BLUS31537 falhou da última vez (falta de threads): abrindo no modo seguro, nível 1 de 3");
    }
    // Level 1 after a GPU failure: the Async Recompiler
    at_level(terraria, adapt::key_fail_gpu, adapt::key_safe1);
    {
        g_checkpoints.clear();
        cfg_root cfg;
        std::vector<change> changes;
        const policy_result r = boot(cfg, terraria, &changes);
        CHECK(r.online && cfg.core.llvm_threads.get() == 2 && cfg.video.shadermode.get() == shader_mode::async_recompiler);
        const change* c = find_named(changes, "Shader Mode");
        CHECK(c && c->kind == change_kind::adapt && c->why.find("(per-game safe mode level 1 (gpu)): async recompiler after a GPU hang") != std::string::npos);
        CHECK(contains(g_checkpoints, " · shader=Async Recompiler (multi-threaded) (async recompiler after a GPU hang: per-game safe mode level 1 (gpu))"));
    }
    // Level 2 after a stall: online off and one compile thread whatever the failure, the shader mode stays
    at_level(terraria, adapt::key_fail_stall, adapt::key_safe2);
    {
        g_checkpoints.clear();
        g_toasts.clear();
        cfg_root cfg;
        const policy_result r = boot(cfg, terraria);
        CHECK(r.safe_level == 2 && r.safe_kind == fail_kind::stall && r.online_skipped);
        CHECK(cfg.core.llvm_threads.get() == 1 && cfg.video.shadermode.get() == shader_mode::async_with_interpreter);
        CHECK(contains(g_checkpoints, "online pulado (modo seguro nível 2)") && contains(g_checkpoints, " · modo seguro nível 2 (travou sem progresso, "));
        CHECK(g_toasts.size() == 1 && g_toasts[0] == "RPCS3: BLUS31537 falhou da última vez (travou sem progresso): abrindo no modo seguro, nível 2 de 3");
    }
    // Level 3: also the Async Recompiler without the shader interpreter, whatever the failure
    at_level(terraria, adapt::key_fail_crash, adapt::key_safe3);
    {
        g_checkpoints.clear();
        cfg_root cfg;
        std::vector<change> changes;
        boot(cfg, terraria, &changes);
        CHECK(cfg.video.shadermode.get() == shader_mode::async_recompiler && cfg.core.llvm_threads.get() == 1);
        const change* c = find_named(changes, "Shader Mode");
        CHECK(c && c->why == "per-game safe mode level 3 (crash): async recompiler without the shader interpreter");
        CHECK(contains(g_checkpoints, "(async recompiler without the shader interpreter: per-game safe mode level 3 (crash))"));
    }
    // Level 3 failed again in a later session: the toast asks for the logs
    adapt::remove(terraria);
    CHECK(adapt::append(terraria, adapt::key_fail_crash) && adapt::append(terraria, adapt::key_safe3) && adapt::note_boot(terraria) &&
        adapt::append(terraria, adapt::key_fail_crash) && adapt::append(terraria, adapt::key_safe3));
    {
        g_checkpoints.clear();
        g_toasts.clear();
        cfg_root cfg;
        boot(cfg, terraria);
        CHECK(contains(g_checkpoints, ": o nível 3 falhou de novo"));
        CHECK(g_toasts.size() == 1 && g_toasts[0] == "RPCS3: BLUS31537 falhou também no modo seguro nível 3: envie startup.log e RPCS3.log");
    }
    // The player's level (the per-game row): its own toast; 'Desligado' (safe=0) forgets every earlier level
    adapt::remove(terraria);
    CHECK(adapt::set_user_level(terraria, 2));
    {
        g_toasts.clear();
        cfg_root cfg;
        const policy_result r = boot(cfg, terraria);
        CHECK(r.safe_level == 2 && r.safe_kind == fail_kind::user && !r.online);
        CHECK(g_toasts.size() == 1 && g_toasts[0] == "RPCS3: BLUS31537 abre no modo seguro, nível 2 de 3 (escolhido por você)");
    }
    CHECK(adapt::set_user_level(terraria, 0));
    {
        g_toasts.clear();
        cfg_root cfg;
        const policy_result r = boot(cfg, terraria);
        CHECK(r.safe_level == 0 && r.online && cfg.core.llvm_threads.get() == 2 && g_toasts.empty());
    }
    // English interface: the toast and the failure name follow it; the policy line stays in Portuguese
    i18n::apply(i18n::setting::en, -1);
    at_level(terraria, adapt::key_fail_gpu, adapt::key_safe2);
    {
        g_checkpoints.clear();
        g_toasts.clear();
        cfg_root cfg;
        boot(cfg, terraria);
        CHECK(g_toasts.size() == 1 && g_toasts[0] == "RPCS3: BLUS31537 failed last time (the GPU hung): starting in safe mode, level 2 of 3");
        CHECK(contains(g_checkpoints, " · modo seguro nível 2 (a GPU travou, este boot e mais 7)"));
    }
    i18n::apply(i18n::setting::automatic, -1);
    // The preview: the level, no line, no toast, no boot counted
    {
        g_checkpoints.clear();
        g_toasts.clear();
        const unsigned boots = boot_lines(terraria);
        cfg_root cfg;
        const policy_result r = apply_boot_policy_preview(cfg, terraria, [](const cfg::_base&, const std::string&, std::string_view) {});
        CHECK(r.safe_level == 2 && r.safe_kind == fail_kind::gpu && cfg.core.llvm_threads.get() == 1);
        CHECK(g_checkpoints.empty() && g_toasts.empty() && boot_lines(terraria) == boots);
    }
    // keep-user-settings.flag: no setting changes, the level still goes to set_for_boot (the feature rows are code paths)
    // and its boot counts toward the expiry
    fs::test_files()[keep_user_settings_flag] = "";
    {
        const unsigned boots = boot_lines(terraria);
        cfg_root cfg;
        const policy_result r = boot(cfg, terraria);
        CHECK(r.mode == policy_mode::keep_user_settings && r.safe_level == 2 && r.safe_kind == fail_kind::gpu && cfg.core.llvm_threads.get() == 0);
        CHECK(boot_lines(terraria) == boots + 1);
    }
    fs::test_files().erase(keep_user_settings_flag);
    // Expiry: 8 boots from the XMB at the level, then none (and the file goes)
    at_level(terraria, adapt::key_fail_crash, adapt::key_safe1);
    for (int i = 0; i < 8; i++)
    {
        cfg_root cfg;
        CHECK(boot(cfg, terraria).safe_level == 1);
    }
    {
        g_checkpoints.clear();
        g_toasts.clear();
        cfg_root cfg;
        const policy_result r = boot(cfg, terraria);
        CHECK(r.safe_level == 0 && g_toasts.empty() && contains(g_checkpoints, "adapt: BLUS31537: no active adjustment left, file removed"));
        CHECK(adapt::read_text(terraria).empty());
    }
    // The global safe mode: level 1 for every game, no boot item, the policy line says so
    safe_file("safe-mode.flag", true);
    {
        g_checkpoints.clear();
        g_toasts.clear();
        cfg_root cfg;
        const policy_result r = boot(cfg, terraria);
        CHECK(r.safe_level == 0 && r.online && cfg.core.llvm_threads.get() == 2 && g_toasts.empty());
        CHECK(contains(g_checkpoints, " · modo seguro global (safe-mode.flag)"));
        CHECK(read_runtime_inputs(terraria).global_safe && !feature_on(read_runtime_inputs(terraria), safe_mode::feature::live_cheats));
    }
    at_level(terraria, adapt::key_fail_threads, adapt::key_safe1);
    {
        g_checkpoints.clear();
        cfg_root cfg;
        const policy_result r = boot(cfg, terraria);
        CHECK(r.safe_level == 1 && r.online_skipped && contains(g_checkpoints, " · modo seguro nível 1 (falta de threads, este boot e mais 7) · modo seguro global"));
    }
    reset_state();
}

void write_database(std::initializer_list<std::pair<std::string_view, std::string_view>> games);

// 0.40: config-database-off.flag, or the per-game safe mode from level 2, skips the title's PC config database entry:
// the title runs as without a database, its fallback rows included; every boot decides again.
void test_config_db_skip()
{
    constexpr char sonic[] = "BLES00750";
    reset_state();
    write_database({{sonic, "Video:\n  MSAA: Disabled\n  Write Color Buffers: true\n  Strict Rendering Mode: true"}});
    const auto strict = [&](database_use use, bool expect_strict)
    {
        g_checkpoints.clear();
        cfg_root cfg;
        std::vector<change> changes;
        const policy_result r = boot(cfg, sonic, &changes);
        CHECK(r.database.use == use && r.database.entry);
        CHECK(cfg.video.strict_rendering_mode.get() == expect_strict);
        // the fallback row holds MSAA and Write Color Buffers without the entry
        CHECK(cfg.video.antialiasing_level.get() == msaa_level::none && cfg.video.write_color_buffers.get());
        const change* c = find_named(changes, "MSAA");
        CHECK(c && (use == database_use::applied ? is_database_reason(c->why) : c->why.find("no usable database entry") != std::string::npos));
        return r;
    };
    strict(database_use::applied, true);
    CHECK(contains(g_checkpoints, "config database: applied BLES00750: MSAA=Disabled, Write Color Buffers=true, Strict Rendering Mode=true · 3 keys, 3 value(s) changed"));
    safe_file("config-database-off.flag", true);
    strict(database_use::skipped, false);
    CHECK(contains(g_checkpoints, "config database: BLES00750 entry skipped (config-database-off.flag): the title runs as without a database (its fallback rows included)"));
    CHECK(!config_db::has_entry(sonic));
    safe_file("config-database-off.flag", false);
    strict(database_use::applied, true);
    CHECK(config_db::has_entry(sonic));
    // Level 1 keeps the entry, level 2 skips it; the global safe mode is level 1
    CHECK(adapt::append(sonic, adapt::key_fail_crash) && adapt::append(sonic, adapt::key_safe1));
    strict(database_use::applied, true);
    CHECK(adapt::append(sonic, adapt::key_fail_stall) && adapt::append(sonic, adapt::key_safe2));
    strict(database_use::skipped, false);
    CHECK(contains(g_checkpoints, "config database: BLES00750 entry skipped (per-game safe mode level 2 (stall)): the title runs as without a database"));
    adapt::remove(sonic);
    safe_file("safe-mode.flag", true);
    strict(database_use::applied, true);
    safe_file("safe-mode.flag", false);
    // A user-tuned game never gets the entry; the line says why it is not used
    safe_file("config-database-off.flag", true);
    CHECK(set_user_tuned(sonic, true));
    {
        g_checkpoints.clear();
        cfg_root cfg;
        const policy_result r = boot(cfg, sonic);
        CHECK(r.database.use == database_use::skipped && contains(g_checkpoints, "config database: BLES00750 entry skipped (config-database-off.flag)"));
    }
    CHECK(set_user_tuned(sonic, false));
    // Automatic configuration off, a game without its own YAML: skipped too
    CHECK(set_auto_config_enabled(false));
    {
        cfg_root cfg;
        const policy_result r = boot(cfg, sonic);
        CHECK(r.database.use == database_use::skipped && !cfg.video.strict_rendering_mode.get());
    }
    CHECK(set_auto_config_enabled(true));
    safe_file("config-database-off.flag", false);

    // The settings an entry lists: sections by indentation, quotes, comments, a set's items
    const std::vector<policy_detail::entry_item> items = policy_detail::entry_items(
        "Core:\n  PPU Decoder: Interpreter (static) # slow\n  Libraries Control:\n    - libvdec.sprx:lle\n    - \"libfs.sprx:hle\"\nVideo:\n"
        "  Vulkan:\n    Force FIFO present mode: true\n  Resolution: '1280x720'\n# end\n");
    CHECK(items.size() == 4);
    if (items.size() == 4)
    {
        CHECK(items[0].key == "Core > PPU Decoder" && items[0].value == "Interpreter (static)");
        CHECK(items[1].key == "Core > Libraries Control" && items[1].value == "libvdec.sprx:lle, libfs.sprx:hle");
        CHECK(items[2].key == "Video > Vulkan > Force FIFO present mode" && items[2].value == "true");
        CHECK(items[3].key == "Video > Resolution" && items[3].value == "1280x720");
    }
    reset_state();
}

void test_keep_user_settings()
{
    reset_state();
    fs::test_files()[keep_user_settings_flag] = "";
    CHECK(set_online_enabled(true));
    CHECK(adapt::append(demons_souls, adapt::key_threads_low));
    cfg_root cfg;
    CHECK(cfg.from_string(old_full_dump));
    cfg.core.spu_accurate_reservations.set(false);
    const std::string before = cfg.to_string();
    unsigned calls = 0;
    const policy_result r = apply_boot_policy(cfg, demons_souls, [&](const cfg::_base&, const std::string&, std::string_view) { calls++; });
    CHECK(r.mode == policy_mode::keep_user_settings);
    CHECK(calls == 0 && cfg.to_string() == before);
    CHECK(r.adapt_keys.empty() && !r.online);
    CHECK(boot_lines(demons_souls) == 0); // a boot that applied nothing does not count toward the expiry
    CHECK(g_checkpoints.empty() && g_toasts.empty());
}

void test_vram()
{
    reset_state();
    runtime_inputs in;
    CHECK(vram_limit_for(65536, in) == 768);
    CHECK(vram_limit_for(512, in) == 512);
    CHECK(vram_limit_for(768, in) == 768);
    in.vram.device_mb = 900;
    CHECK(vram_limit_for(512, in) == 900); // the developer flag replaces the automatic cap
    in.vram.device_mb = 100;
    CHECK(vram_limit_for(65536, in) == 256); // clamped to the setting's bounds
    in.vram.device_mb = 0;
    in.adapt = {adapt::key_vram_low};
    CHECK(vram_limit_for(65536, in) == 448);
    CHECK(vram_limit_for(300, in) == 300);
    in.vram.device_mb = 900;
    CHECK(vram_limit_for(65536, in) == 448);

    // vram.flag parsing
    vram_override v = parse_vram_flag("# A/B test\r\ndevice_mb=576\r\n host_mb = 256 \r\n\r\nbogus=1\n");
    CHECK(v.device_mb == 576 && v.host_mb == 256);
    v = parse_vram_flag("device_mb=abc\nhost_mb=99999999999999999999\ndevice_mb=\n");
    CHECK(v.device_mb == 0 && v.host_mb == 0);
    v = parse_vram_flag("device_mb=65537\nhost_mb=65536");
    CHECK(v.device_mb == 0 && v.host_mb == 65536);
    CHECK(parse_vram_flag("").device_mb == 0);

    // Through the boot: the flag file, then an adaptation (reported as an adaptation, not as the cap).
    fs::test_files()[vram_flag] = "device_mb=900\n";
    {
        cfg_root cfg;
        boot(cfg, minecraft);
        CHECK(cfg.video.vk.vram_allocation_limit.get() == 900);
    }
    fs::test_files().erase(vram_flag);
    CHECK(adapt::append(minecraft, adapt::key_vram_low));
    {
        cfg_root cfg;
        std::vector<change> changes;
        const policy_result r = boot(cfg, minecraft, &changes);
        CHECK(cfg.video.vk.vram_allocation_limit.get() == 448);
        const change* c = find_named(changes, "VRAM allocation limit (MB)");
        CHECK(c && c->kind == change_kind::adapt && c->why.find("vram=low") != std::string::npos);
        CHECK(r.adapt_changes == 1);
        CHECK(contains(g_toasts, "memoria de video reduzida"));
    }
}

void test_adapt_expiry_and_relaunch()
{
    reset_state();
    CHECK(adapt::append(demons_souls, adapt::key_llvm_threads_1));
    for (unsigned n = 1; n <= 3; n++)
    {
        g_toasts.clear();
        cfg_root cfg;
        std::vector<change> changes;
        const policy_result r = boot(cfg, demons_souls, &changes);
        CHECK(adapt::has(r.adapt_keys, adapt::key_llvm_threads_1));
        CHECK(cfg.core.llvm_threads.get() == 1);
        CHECK(cfg.video.shader_compiler_threads_count.get() == 1);
        CHECK(r.adapt_changes == 2);
        // Adaptations count as per-title changes, plus Write Color Buffers from the automatic Demon's Souls row
        // (Accurate SPU Reservations is already true).
        CHECK(r.title_changes == 3);
        // The profile sets 2 (from 0), then the adaptation sets 1.
        CHECK(count_named(changes, "Max LLVM Compile Threads") == 2);
        CHECK(find_named(changes, "Max LLVM Compile Threads")->kind == change_kind::profile);
        const change* last = nullptr;
        for (const auto& c : changes)
            if (c.name == "Max LLVM Compile Threads") last = &c;
        CHECK(last && last->kind == change_kind::adapt && last->now == "1" && last->why.find("llvm_threads=1") != std::string::npos);
        CHECK(boot_lines(demons_souls) == n);
        CHECK(contains(g_toasts, "RPCS3: ajuste automatico para BLUS30443 apos falha anterior: compilacao com 1 thread"));
    }
    // The 4th boot runs the normal configuration and drops the file.
    {
        g_checkpoints.clear();
        cfg_root cfg;
        const policy_result r = boot(cfg, demons_souls);
        CHECK(r.adapt_keys.empty());
        CHECK(cfg.core.llvm_threads.get() == 2);
        CHECK(cfg.video.shader_compiler_threads_count.get() == 2);
        CHECK(adapt::read_text(demons_souls).empty());
        CHECK(contains(g_checkpoints, "adapt: BLUS30443: no active adjustment left, file removed"));
    }

    // A key written again during those boots restarts its count.
    reset_state();
    CHECK(adapt::append(demons_souls, adapt::key_llvm_threads_1));
    { cfg_root cfg; boot(cfg, demons_souls); }
    { cfg_root cfg; boot(cfg, demons_souls); }
    CHECK(adapt::append(demons_souls, adapt::key_llvm_threads_1));
    for (int n = 0; n < 3; n++)
    {
        cfg_root cfg;
        boot(cfg, demons_souls);
        CHECK(cfg.core.llvm_threads.get() == 1);
    }
    { cfg_root cfg; boot(cfg, demons_souls); CHECK(cfg.core.llvm_threads.get() == 2); }

    // A relaunch of the running game (same title still set) keeps the adaptation but is not a new boot.
    reset_state();
    CHECK(adapt::append(demons_souls, adapt::key_llvm_threads_1));
    { cfg_root cfg; boot(cfg, demons_souls); }
    CHECK(boot_lines(demons_souls) == 1);
    CHECK(adapt::set_title(demons_souls));
    g_toasts.clear();
    {
        cfg_root cfg;
        boot(cfg, demons_souls);
        CHECK(cfg.core.llvm_threads.get() == 1);
    }
    CHECK(boot_lines(demons_souls) == 1);
    CHECK(g_toasts.empty());
    adapt::clear_title();
    { cfg_root cfg; boot(cfg, demons_souls); }
    CHECK(boot_lines(demons_souls) == 2);

    // 0.39: gpu_hang expires like the others; on Vulkan its boots run the Async Recompiler without the shader
    // interpreter (one adaptation change, one toast, the policy line names it).
    reset_state();
    CHECK(adapt::append(minecraft, adapt::key_gpu_hang));
    {
        cfg_root cfg;
        std::vector<change> changes;
        const policy_result r = boot(cfg, minecraft, &changes);
        CHECK(r.adapt_changes == 1 && adapt::has(r.adapt_keys, adapt::key_gpu_hang));
        CHECK(cfg.video.shadermode.get() == shader_mode::async_recompiler);
        const change* c = nullptr;
        for (const auto& item : changes)
            if (item.name == "Shader Mode") c = &item;
        CHECK(c && c->kind == change_kind::adapt && c->why.find("async recompiler after a GPU hang") != std::string::npos);
        CHECK(contains(g_toasts, "RPCS3: ajuste automatico para BLUS31426 apos falha anterior: shaders sem interpretador (a GPU travou)"));
        CHECK(contains(g_checkpoints, "gpu_hang shader=async_recompiler"));
        CHECK(contains(g_checkpoints, " · shader=Async Recompiler (multi-threaded) (async recompiler after a GPU hang: adapt gpu_hang)"));
    }
    // A mode chosen otherwise stays (the legacy recompiler of a user-tuned save, OpenGL), and is only recorded.
    reset_state();
    CHECK(adapt::append(ben10, adapt::key_gpu_hang));
    {
        cfg_root cfg;
        const policy_result r = boot(cfg, ben10);
        CHECK(r.adapt_changes == 0 && cfg.video.renderer.get() == video_renderer::opengl && cfg.video.shadermode.get() == shader_mode::recompiler);
        CHECK(g_toasts.empty() && contains(g_checkpoints, "gpu_hang=recorded"));
    }
    // With several keys the toast parts are joined in the interface language.
    reset_state();
    CHECK(adapt::append(minecraft, adapt::key_gpu_hang));
    CHECK(adapt::append(minecraft, adapt::key_vram_low));
    {
        cfg_root cfg;
        boot(cfg, minecraft);
        CHECK(contains(g_toasts, "apos falha anterior: memoria de video reduzida; shaders sem interpretador (a GPU travou)"));
    }
    reset_state();
    CHECK(adapt::append(minecraft, adapt::key_gpu_hang));
    i18n::apply(i18n::setting::en, -1);
    {
        cfg_root cfg;
        boot(cfg, minecraft);
        CHECK(contains(g_toasts, "RPCS3: automatic adjustment for BLUS31426 after a previous failure: shaders without the interpreter (the GPU hung)"));
    }
    i18n::apply(i18n::setting::automatic, -1);
    reset_state();
}

void test_threads_low_skips_online()
{
    reset_state();
    give_rpcn_account(); // 0.40: without an account every title would stay offline for that reason
    CHECK(set_online_enabled(true));
    CHECK(adapt::append(demons_souls_eu, adapt::key_threads_low));
    {
        cfg_root cfg;
        std::vector<change> changes;
        const policy_result r = boot(cfg, demons_souls_eu, &changes);
        CHECK(r.online_skipped && !r.online);
        CHECK(cfg.net.net_active.get() == np_internet_status::disabled);
        CHECK(cfg.net.psn_status.get() == np_psn_status::disabled);
        CHECK(cfg.core.llvm_threads.get() == 1 && cfg.video.shader_compiler_threads_count.get() == 1);
        CHECK(cfg.core.spu_accurate_reservations.get()); // its every-mode row still applies
        CHECK(contains(g_toasts, "online desativado para este jogo: limite de threads"));
        CHECK(contains(g_checkpoints, "online pulado (limite de threads)"));
    }
    // Another title with the switch on goes online.
    {
        cfg_root cfg;
        const policy_result r = boot(cfg, minecraft);
        CHECK(r.online && cfg.net.net_active.get() == np_internet_status::enabled);
    }
    // A title that was online in its YAML loses it while the adjustment lasts, reported as an adaptation.
    {
        cfg_root cfg;
        cfg.net.net_active.set(np_internet_status::enabled);
        cfg.net.psn_status.set(np_psn_status::psn_rpcn);
        std::vector<change> changes;
        boot(cfg, demons_souls_eu, &changes);
        const change* c = find_named(changes, "Internet enabled");
        CHECK(c && c->kind == change_kind::adapt && c->why.find("thread budget") != std::string::npos);
    }
    // threads=low without the online switch: offline as usual, no online toast.
    CHECK(set_online_enabled(false));
    g_toasts.clear();
    {
        cfg_root cfg;
        const policy_result r = boot(cfg, demons_souls_eu);
        CHECK(!r.online_skipped);
        CHECK(!contains(g_toasts, "online desativado"));
        CHECK(contains(g_toasts, "compilacao com 1 thread"));
    }
}

void test_title_rows()
{
    {
        reset_state();
        cfg_root cfg; // Vulkan, async shaders, 720p
        std::vector<change> changes;
        const policy_result r = boot(cfg, ben10, &changes);
        CHECK(cfg.video.renderer.get() == video_renderer::opengl);
        CHECK(cfg.video.shadermode.get() == shader_mode::recompiler);
        CHECK(cfg.video.resolution.get() == video_resolution::_480i); // the output the console run used
        CHECK(cfg.video.shader_compiler_threads_count.get() == 0); // the Vulkan-only block did not run
        CHECK(count_named(changes, "Renderer") == 1);
        CHECK(count_named(changes, "Shader Mode") == 1);
        CHECK(count_named(changes, "Resolution") == 1);
        const change* c = find_named(changes, "Renderer");
        CHECK(c && c->kind == change_kind::title && c->why.find("verified on console") != std::string::npos);
        CHECK(r.title_changes == 3);
        CHECK(r.console_changes == 0); // 0 shader threads is left alone on OpenGL (one worker on this CPU)
    }
    // Terraria has no row: it only ever ran on Vulkan at 720p, which is what the automatic profile gives.
    {
        reset_state();
        cfg_root cfg;
        cfg.video.renderer.set(video_renderer::opengl);
        const policy_result r = boot(cfg, terraria);
        CHECK(cfg.video.renderer.get() == video_renderer::vulkan);
        CHECK(cfg.video.resolution.get() == video_resolution::_720p);
        CHECK(cfg.video.shadermode.get() == shader_mode::async_with_interpreter);
        CHECK(r.title_changes == 0);
        CHECK(!title_entry_for(cfg, terraria, cfg.video.renderer));
    }

    // A game saved from the menu (user-tuned) runs its YAML: the verified rows belong to the automatic profile.
    reset_state();
    CHECK(set_user_tuned(ben10, true));
    {
        cfg_root cfg;
        const policy_result r = boot(cfg, ben10);
        CHECK(r.mode == policy_mode::user_tuned);
        CHECK(cfg.video.renderer.get() == video_renderer::vulkan);
        CHECK(cfg.video.resolution.get() == video_resolution::_720p);
    }
    // Every-mode rows (Demon's Souls) hold for user-tuned games too.
    CHECK(set_user_tuned(demons_souls, true));
    {
        cfg_root cfg;
        cfg.core.spu_accurate_reservations.set(false);
        std::vector<change> changes;
        const policy_result r = boot(cfg, demons_souls, &changes);
        CHECK(r.mode == policy_mode::user_tuned && !r.stale_marker);
        CHECK(cfg.core.spu_accurate_reservations.get());
        // Write Color Buffers is an automatic-scope row: a game saved with it off keeps it off.
        CHECK(!cfg.video.write_color_buffers.get());
        CHECK(r.title_changes == 1 && r.profile_changes == 0);
        CHECK(r.console_changes == 2); // LLVM and shader threads 0 -> 2
    }
    // Any other game gets the automatic profile.
    {
        cfg_root cfg;
        cfg.video.renderer.set(video_renderer::opengl);
        cfg.video.resolution.set(video_resolution::_480i);
        boot(cfg, minecraft);
        CHECK(cfg.video.renderer.get() == video_renderer::vulkan);
        CHECK(cfg.video.resolution.get() == video_resolution::_720p);
        CHECK(cfg.video.shadermode.get() == shader_mode::async_with_interpreter);
    }
    // Lookup helpers used by the settings menu.
    {
        cfg_root cfg;
        const title_compat* e = title_entry_for(cfg, ben10, cfg.video.renderer);
        CHECK(e && e->scope == title_scope::automatic);
        CHECK(!title_entry_for(cfg, ben10, cfg.video.renderer, false));
        e = title_entry_for(cfg, demons_souls, cfg.core.spu_accurate_reservations, false);
        CHECK(e && e->scope == title_scope::every_mode);
        for (const char* id : {"BLUS30443", "BLES00932", "BCJS30022", "BCAS20071"})
        {
            e = title_entry_for(cfg, id, cfg.video.write_color_buffers);
            CHECK(e && e->scope == title_scope::automatic);
            CHECK(!title_entry_for(cfg, id, cfg.video.write_color_buffers, false));
            e = title_entry_for(cfg, id, cfg.core.spu_accurate_reservations, false);
            CHECK(e && e->scope == title_scope::every_mode);
        }
        CHECK(!title_entry_for(cfg, minecraft, cfg.video.write_color_buffers));
        CHECK(!title_entry_for(cfg, minecraft, cfg.video.renderer));
        CHECK(!title_entry_for(cfg, "", cfg.video.renderer));
    }
}

void test_preview_and_callbacks()
{
    reset_state();
    CHECK(set_online_enabled(true));
    CHECK(adapt::append(demons_souls_eu, adapt::key_threads_low));
    cfg_root booted, previewed;
    boot(booted, demons_souls_eu);
    const unsigned lines = boot_lines(demons_souls_eu);
    g_checkpoints.clear();
    g_toasts.clear();
    std::vector<change_kind> kinds;
    const policy_result r = apply_boot_policy_preview(previewed, demons_souls_eu,
        [&](const cfg::_base&, const std::string&, std::string_view, change_kind kind) { kinds.push_back(kind); });
    CHECK(previewed.to_string() == booted.to_string());
    CHECK(r.online_skipped && adapt::has(r.adapt_keys, adapt::key_threads_low));
    CHECK(boot_lines(demons_souls_eu) == lines); // opening the menu is not a boot
    CHECK(g_checkpoints.empty() && g_toasts.empty());
    CHECK(!kinds.empty());

    // Preview with inputs read by the caller (what the menu does), and the runtime section on its own.
    const runtime_inputs in = read_runtime_inputs(demons_souls_eu);
    CHECK(in.online && adapt::has(in.adapt, adapt::key_threads_low) && in.adapt_boots_left == 2);
    cfg_root again;
    apply_boot_policy_preview(again, demons_souls_eu, in, [](const cfg::_base&, const std::string&, std::string_view) {});
    CHECK(again.to_string() == booted.to_string());
    cfg_root runtime_only;
    u32 n = apply_runtime_limits(runtime_only, std::string_view(demons_souls_eu), [](const cfg::_base&, const std::string&, std::string_view) {});
    CHECK(n > 0 && runtime_only.core.llvm_threads.get() == 1 && runtime_only.net.net_active.get() == np_internet_status::disabled);

    // The System.cpp callback form (3 arguments): runtime changes carry a reason, profile changes do not.
    reset_state();
    cfg_root cfg;
    bool profile_seen = false, runtime_reason = false;
    apply_boot_policy(cfg, std::string(minecraft), [&](const cfg::_base& setting, const std::string&, std::string_view why)
    {
        if (setting.get_name() == "Max LLVM Compile Threads") profile_seen = why.empty();
        if (setting.get_name() == "VRAM allocation limit (MB)") runtime_reason = !why.empty();
    });
    CHECK(profile_seen && runtime_reason);

    // The global config (Emu.Init, no title) gets the runtime section but no per-title bookkeeping.
    reset_state();
    cfg_root global;
    const policy_result g = boot(global, "");
    CHECK(g.runtime_changes == 3 && g_checkpoints.empty());
}

void test_cache_bounds()
{
    reset_state();
    for (const auto& [configured, expected] : std::vector<std::pair<s64, s64>>{{0, 1024}, {512, 1024}, {2048, 2048}, {4096, 4096}, {8000, 4096}, {10240, 4096}})
    {
        cfg_root cfg;
        cfg.vfs.cache_max_size.set(configured);
        boot(cfg, "");
        CHECK(cfg.vfs.cache_max_size.get() == expected);
        CHECK(cfg.vfs.limit_cache_size.get());
    }
}

// Values a copied PC config or an old menu save can carry, all of them limited on the console.
void set_unsafe_values(cfg_root& cfg)
{
    cfg.video.multithreaded_rsx.set(true);
    cfg.video.vk.asynchronous_texture_streaming.set(true);
    cfg.video.vk.asynchronous_scheduler.set(vk_gpu_scheduler_mode::fast);
    cfg.video.disable_vulkan_mem_allocator.set(true);
    cfg.core.llvm_threads.set(0);
    cfg.video.shader_compiler_threads_count.set(5);
}

void test_console_limits()
{
    for (const int mode : {0, 1, 2}) // automatic, user-tuned, automatic configuration off
    {
        reset_state();
        if (mode == 1) CHECK(set_user_tuned(minecraft, true));
        if (mode == 2) CHECK(set_auto_config_enabled(false));
        cfg_root cfg;
        set_unsafe_values(cfg);
        std::vector<change> changes;
        const policy_result r = boot(cfg, minecraft, &changes);
        CHECK(r.mode == (mode == 0 ? policy_mode::automatic : mode == 1 ? policy_mode::user_tuned : policy_mode::auto_config_off));
        CHECK(!r.stale_marker);
        CHECK(!cfg.video.multithreaded_rsx.get());
        CHECK(!cfg.video.vk.asynchronous_texture_streaming.get());
        CHECK(cfg.video.vk.asynchronous_scheduler.get() == vk_gpu_scheduler_mode::safe);
        CHECK(!cfg.video.disable_vulkan_mem_allocator.get());
        CHECK(cfg.core.llvm_threads.get() == 2);
        CHECK(cfg.video.shader_compiler_threads_count.get() == 2);
        for (const char* name : {"Asynchronous Texture Streaming", "Asynchronous Queue Scheduler", "Disable Vulkan Memory Allocator"})
        {
            const change* c = find_named(changes, name);
            CHECK(c && c->kind == change_kind::console && !c->why.empty() && count_named(changes, name) == 1);
        }
        // The automatic profile already sets these three, so the console limits report them only in the other modes.
        const change_kind expected = mode == 0 ? change_kind::profile : change_kind::console;
        for (const char* name : {"Multithreaded RSX", "Max LLVM Compile Threads", "Shader Compiler Threads"})
        {
            const change* c = find_named(changes, name);
            CHECK(c && c->kind == expected && count_named(changes, name) == 1);
        }
        CHECK(r.console_changes == (mode == 0 ? 3u : 6u));
    }

    // 1 and 2 threads stay as saved (the adaptation may still lower them); OpenGL keeps 0 shader threads.
    reset_state();
    CHECK(set_user_tuned(minecraft, true));
    {
        cfg_root cfg;
        cfg.core.llvm_threads.set(1);
        cfg.video.shader_compiler_threads_count.set(1);
        const policy_result r = boot(cfg, minecraft);
        CHECK(r.mode == policy_mode::user_tuned && r.console_changes == 0);
        CHECK(cfg.core.llvm_threads.get() == 1 && cfg.video.shader_compiler_threads_count.get() == 1);
    }
    {
        cfg_root cfg;
        cfg.video.renderer.set(video_renderer::opengl);
        cfg.core.llvm_threads.set(2);
        boot(cfg, minecraft);
        CHECK(cfg.video.shader_compiler_threads_count.get() == 0);
        cfg.video.shader_compiler_threads_count.set(8);
        boot(cfg, minecraft);
        CHECK(cfg.video.shader_compiler_threads_count.get() == 2);
    }

    // keep-user-settings.flag: nothing is forced.
    reset_state();
    fs::test_files()[keep_user_settings_flag] = "";
    {
        cfg_root cfg;
        set_unsafe_values(cfg);
        const std::string before = cfg.to_string();
        unsigned calls = 0;
        const policy_result r = apply_boot_policy(cfg, minecraft, [&](const cfg::_base&, const std::string&, std::string_view, change_kind) { calls++; });
        CHECK(r.mode == policy_mode::keep_user_settings && r.console_changes == 0);
        CHECK(calls == 0 && cfg.to_string() == before);
    }

    // Helpers used by the settings menu.
    {
        cfg_root cfg;
        const unsafe_option* u = unsafe_option_for(cfg, cfg.video.multithreaded_rsx);
        CHECK(u && u->value == "false" && !u->why.empty() && *i18n::tr(u->menu));
        u = unsafe_option_for(cfg, cfg.video.vk.asynchronous_scheduler);
        CHECK(u && u->value == "Safe");
        CHECK(unsafe_option_for(cfg, cfg.video.vk.asynchronous_texture_streaming));
        CHECK(unsafe_option_for(cfg, cfg.video.disable_vulkan_mem_allocator));
        CHECK(!unsafe_option_for(cfg, cfg.video.write_color_buffers));
        CHECK(!unsafe_option_for(cfg, cfg.core.llvm_threads)); // a limit, not an unavailable option
        set_unsafe_values(cfg);
        const auto no_report = [](const cfg::_base&, const std::string&, std::string_view) {};
        CHECK(apply_console_limits(cfg, no_report) == 6);
        CHECK(apply_console_limits(cfg, no_report) == 0);
    }
}

void test_demons_souls_graphics()
{
    for (const char* id : {"BLUS30443", "BLES00932", "BCJS30022", "BCAS20071"})
    {
        reset_state();
        cfg_root cfg;
        std::vector<change> changes;
        const policy_result r = boot(cfg, id, &changes);
        CHECK(r.mode == policy_mode::automatic);
        CHECK(cfg.video.write_color_buffers.get());
        CHECK(cfg.core.spu_accurate_reservations.get());
        const change* c = find_named(changes, "Write Color Buffers");
        CHECK(c && c->kind == change_kind::title && c->why.find("Write Color Buffers") != std::string::npos);
        CHECK(count_named(changes, "Write Color Buffers") == 1); // the profile skips a setting a row decides
        CHECK(r.title_changes == 1);
    }
    // Every other game keeps the profile's 'off'.
    {
        reset_state();
        cfg_root cfg;
        cfg.video.write_color_buffers.set(true);
        boot(cfg, minecraft);
        CHECK(!cfg.video.write_color_buffers.get());
    }
    // Automatic configuration off: the saved value runs; the every-mode row still holds.
    reset_state();
    CHECK(set_auto_config_enabled(false));
    {
        cfg_root cfg;
        cfg.core.spu_accurate_reservations.set(false);
        const policy_result r = boot(cfg, demons_souls);
        CHECK(r.mode == policy_mode::auto_config_off);
        CHECK(!cfg.video.write_color_buffers.get() && cfg.core.spu_accurate_reservations.get());
    }
}

// The console's Demon's Souls files after 0.31: BLUS30443.user-tuned plus a YAML saved from the menu with the
// automatic values, Write Color Buffers on, and the runtime-owned values from before the runtime section.
void test_stale_marker()
{
    reset_state();
    std::string saved;
    {
        cfg_root cfg;
        boot(cfg, demons_souls);
        cfg.vfs.limit_cache_size.set(false);
        cfg.vfs.cache_max_size.set(5120);
        cfg.video.vk.vram_allocation_limit.set(65536);
        saved = cfg.to_string();
    }
    CHECK(saved.find("Write Color Buffers: true") != std::string::npos);
    CHECK(set_user_tuned(demons_souls, true));
    {
        cfg_root cfg;
        CHECK(cfg.from_string(saved));
        CHECK(saved_equals_automatic(cfg, demons_souls));
        CHECK(cfg.to_string() == saved); // the check leaves the config as it was
        std::vector<change> changes;
        const policy_result r = boot(cfg, demons_souls, &changes);
        CHECK(r.mode == policy_mode::automatic && r.stale_marker);
        CHECK(r.profile_changes == 0 && r.title_changes == 0 && r.console_changes == 0);
        CHECK(r.runtime_changes == 3); // cache limit, cache size and the VRAM cap, as on every boot
        CHECK(cfg.video.write_color_buffers.get() && cfg.core.spu_accurate_reservations.get());
        CHECK(!cfg.video.multithreaded_rsx.get() && !cfg.video.vk.asynchronous_texture_streaming.get());
        CHECK(is_user_tuned(demons_souls)); // the boot does not delete the marker
        cfg_root preview; // what the settings menu shows
        CHECK(preview.from_string(saved));
        CHECK(apply_boot_policy_preview(preview, demons_souls, [](const cfg::_base&, const std::string&, std::string_view) {}).mode == policy_mode::automatic);
    }
    // Saved with Write Color Buffers off: a real manual setting, which the automatic-scope row leaves alone.
    {
        cfg_root cfg;
        CHECK(cfg.from_string(saved));
        cfg.video.write_color_buffers.set(false);
        CHECK(!saved_equals_automatic(cfg, demons_souls));
        CHECK(!cfg.video.write_color_buffers.get());
        const policy_result r = boot(cfg, demons_souls);
        CHECK(r.mode == policy_mode::user_tuned && !r.stale_marker);
        CHECK(!cfg.video.write_color_buffers.get());
    }
    // A profile value saved differently keeps the marker too.
    {
        cfg_root cfg;
        CHECK(cfg.from_string(saved));
        cfg.core.max_spurs_threads.set(4);
        const policy_result r = boot(cfg, demons_souls);
        CHECK(r.mode == policy_mode::user_tuned && cfg.core.max_spurs_threads.get() == 4);
    }
    // Values that only the console limits replace end the same on both paths: still stale.
    {
        cfg_root cfg;
        CHECK(cfg.from_string(saved));
        cfg.video.multithreaded_rsx.set(true);
        cfg.video.vk.asynchronous_texture_streaming.set(true);
        const policy_result r = boot(cfg, demons_souls);
        CHECK(r.mode == policy_mode::automatic && r.stale_marker);
        CHECK(!cfg.video.multithreaded_rsx.get() && !cfg.video.vk.asynchronous_texture_streaming.get());
    }
    // An active adaptation (the console still had vram=low for Demon's Souls) runs on both paths: still stale, and
    // the adaptation still applies.
    CHECK(adapt::append(demons_souls, adapt::key_vram_low));
    {
        cfg_root cfg;
        CHECK(cfg.from_string(saved));
        const policy_result r = boot(cfg, demons_souls);
        CHECK(r.mode == policy_mode::automatic && r.stale_marker);
        CHECK(cfg.video.vk.vram_allocation_limit.get() == 448 && r.adapt_changes == 1);
    }
    adapt::remove(demons_souls);

    // A check that finds a difference leaves the config as it was too, including the values the Vulkan-only block
    // of the profile changed after it switched the renderer.
    {
        cfg_root cfg;
        cfg.video.renderer.set(video_renderer::opengl);
        cfg.video.shadermode.set(shader_mode::recompiler);
        cfg.video.multithreaded_rsx.set(true);
        cfg.core.llvm_threads.set(0);
        const std::string before = cfg.to_string();
        CHECK(!saved_equals_automatic(cfg, minecraft));
        CHECK(cfg.to_string() == before);
    }
    // dry_run puts back a setting that one step changed twice (newest first) and reports its last value.
    {
        cfg_root cfg;
        const auto changes = policy_detail::dry_run([&](auto&& record)
        {
            for (const s64 value : {2, 1})
            {
                const std::string old = cfg.core.llvm_threads.to_string();
                cfg.core.llvm_threads.set(value);
                record(cfg.core.llvm_threads, old, std::string_view{}, change_kind::console);
            }
        });
        CHECK(changes.size() == 2 && cfg.core.llvm_threads.get() == 0);
        const std::string* last = policy_detail::final_value(changes, &cfg.core.llvm_threads);
        CHECK(last && *last == "1");
    }

    // Ben 10 saved with its verified OpenGL values is stale; saved with Vulkan it stays manual.
    reset_state();
    std::string ben10_saved;
    {
        cfg_root cfg;
        boot(cfg, ben10);
        ben10_saved = cfg.to_string();
    }
    CHECK(set_user_tuned(ben10, true));
    {
        cfg_root cfg;
        CHECK(cfg.from_string(ben10_saved));
        const policy_result r = boot(cfg, ben10);
        CHECK(r.mode == policy_mode::automatic && r.stale_marker);
        CHECK(cfg.video.renderer.get() == video_renderer::opengl);
    }
    {
        cfg_root cfg;
        CHECK(cfg.from_string(ben10_saved));
        cfg.video.renderer.set(video_renderer::vulkan);
        cfg.video.shadermode.set(shader_mode::async_with_interpreter);
        const policy_result r = boot(cfg, ben10);
        CHECK(r.mode == policy_mode::user_tuned && !r.stale_marker);
        CHECK(cfg.video.renderer.get() == video_renderer::vulkan);
    }
    // The other modes never look at the marker's contents.
    CHECK(set_auto_config_enabled(false));
    {
        cfg_root cfg;
        CHECK(cfg.from_string(ben10_saved));
        const policy_result r = boot(cfg, ben10);
        CHECK(r.mode == policy_mode::auto_config_off && !r.stale_marker);
    }
}

// 0.33: the PC config database. Entries below are the 2026-07-16 PC database texts where a real title is used.
constexpr char gow_ascension[] = "BCUS98232";
constexpr char gow_ascension_psn_eu[] = "NPEA00445";
constexpr char gow_3[] = "BCUS98111";
constexpr char gow_ascension_entry[] = "Core:\n  Accurate RSX reservation access: true\n  Sleep Timers Accuracy: As Host\nVideo:\n"
    "  Write Color Buffers: true\n  Accurate ZCULL stats: false\n  Relaxed ZCULL Sync: false";
constexpr char gow_3_entry[] = "Video:\n  Vulkan:\n    Asynchronous Texture Streaming: true\n  Write Color Buffers: true\nCore:\n  SPU Block Size: Mega";

std::string json_escape(std::string_view text)
{
    std::string out;
    for (const char c : text)
    {
        if (c == '\n') out += "\\n";
        else if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else out += c;
    }
    return out;
}

// config_database.dat as the PC writes it; timestamp < 0 leaves the key out.
std::string database_text(std::initializer_list<std::pair<std::string_view, std::string_view>> games, long long timestamp = 1784200630)
{
    std::string text = "{\"return_code\": 0, ";
    if (timestamp >= 0) text += "\"timestamp\": " + std::to_string(timestamp) + ", ";
    text += "\"games\": {";
    bool first = true;
    for (const auto& [serial, config] : games)
    {
        text += first ? "\n" : ",\n";
        first = false;
        text += "\"" + std::string(serial) + "\": {\"status\": \"Playable\", \"config\": \"" + json_escape(config) + "\"}";
    }
    text += "}}\n";
    return text;
}

// /data/rpcs3/config_database.dat.
void write_database(std::initializer_list<std::pair<std::string_view, std::string_view>> games)
{
    fs::test_files()[config_db::path] = database_text(games);
}

void test_config_db_reader()
{
    using config_db::detail::game_map;
    game_map games;
    std::string error;
    CHECK(config_db::detail::parse("\xEF\xBB\xBF {\"return_code\": 0, \"games\": {\"A\": {\"config\": \"Video:\\n  Write Color Buffers: true\"},"
        " \"B\": {\"status\": \"Ingame\"}, \"C\": 5, \"D\": {\"config\": 7}, \"E\": {\"config\": \"x\", \"list\": [1, -2.5e3, true, null, {}]}}} \r\n",
        games, error));
    CHECK(error.empty() && games.size() == 2 && games["A"] == "Video:\n  Write Color Buffers: true" && games["E"] == "x");
    // Escapes: é, a surrogate pair, a lone high surrogate (U+FFFD), \/ and \t.
    CHECK(config_db::detail::parse("{\"return_code\": 1, \"games\": {\"S\": {\"config\": \"\\u00e9\\ud83d\\ude00\\ud800x\\/\\t\"}}}", games, error));
    CHECK(games["S"] == "\xC3\xA9\xF0\x9F\x98\x80\xEF\xBF\xBDx/\t");
    // What the PC reader refuses.
    for (const char* bad : {
        "{\"return_code\": -1, \"games\": {}}",       // server error
        "{\"games\": {}}",                            // no return_code
        "{\"return_code\": 0.5, \"games\": {}}",      // not an integer
        "{\"return_code\": 0}",                       // no games
        "{\"return_code\": 0, \"games\": []}",        // games not an object
        "[]",                                         // not an object
        "{\"return_code\": 0, \"games\": {}} x",      // garbage after the document
        "{\"return_code\": 0, \"games\": {\"A\": {\"config\": \"a\nb\"}}}", // raw control character
        "{\"return_code\": 0, \"games\": {\"A\": {\"config\": \"abc}}}",    // unterminated string
        "{\"return_code\": 0, \"games\": {}",         // unterminated object
        "",
    })
    {
        CHECK(!config_db::detail::parse(bad, games, error));
        CHECK(games.empty() && !error.empty());
    }
    CHECK(!config_db::detail::parse("{\"return_code\": 0, \"games\": {}, \"x\": " + std::string(100, '[') + std::string(100, ']') + "}", games, error));
    CHECK(error.find("nesting") != std::string::npos);

    // File states, and a cache that follows the file.
    reset_state();
    CHECK(config_db::status().state == config_db::file_state::absent);
    CHECK(!config_db::has_entry(minecraft));
    fs::test_files()[config_db::path] = "<html>not a database</html>";
    CHECK(config_db::status().state == config_db::file_state::invalid && !config_db::status().error.empty());
    write_database({{minecraft, "Video:\n  Write Color Buffers: true"}, {terraria, "Video:\n  Write Color Buffers: true"}});
    const config_db::file_info info = config_db::status();
    CHECK(info.state == config_db::file_state::loaded && info.titles == 2);
    CHECK(config_db::has_entry(minecraft) && !config_db::has_entry(demons_souls) && !config_db::has_entry(""));
    config_db::mark_rejected(minecraft);
    CHECK(!config_db::has_entry(minecraft) && config_db::lookup(minecraft).rejected && !config_db::lookup(minecraft).config.empty());
    write_database({{minecraft, "Video:\n  Write Color Buffers: true"}}); // a new file: entries are checked again
    CHECK(config_db::has_entry(minecraft) && config_db::status().titles == 1);
    fs::test_files().erase(config_db::path);
    CHECK(config_db::status().state == config_db::file_state::absent && !config_db::has_entry(minecraft));
}

void test_config_db_automatic()
{
    // No database on the console: the God of War rows hold the PC entry's values.
    for (const char* id : {gow_ascension, gow_ascension_psn_eu})
    {
        reset_state();
        cfg_root cfg;
        std::vector<change> changes;
        const policy_result r = boot(cfg, id, &changes);
        CHECK(r.mode == policy_mode::automatic && r.database.use == database_use::none && !r.database.entry);
        CHECK(cfg.video.write_color_buffers.get() && cfg.core.rsx_accurate_res_access.get() && !cfg.video.relaxed_zcull_sync.get());
        CHECK(!cfg.video.precise_zpass_count.get()); // 0.37: the entry's Accurate ZCULL stats off is a row value
        const change* c = find_named(changes, "Write Color Buffers");
        CHECK(c && c->kind == change_kind::title && c->why.find("no usable database entry") != std::string::npos);
        CHECK(count_named(changes, "Relaxed ZCULL Sync") == 0); // the row's value is the default: the profile skips it
        c = find_named(changes, "Accurate ZCULL stats");
        CHECK(c && c->kind == change_kind::title && count_named(changes, "Accurate ZCULL stats") == 1);
        CHECK(r.title_changes == 3);
        CHECK(contains(g_checkpoints, "config database: /data/rpcs3/config_database.dat not present"));
        CHECK(!contains(g_checkpoints, "config database: applied"));
    }
    {
        reset_state();
        cfg_root cfg;
        boot(cfg, gow_3);
        CHECK(cfg.video.write_color_buffers.get() && cfg.core.spu_block_size.get() == spu_block_size_type::mega);
    }

    // The database lists the game: its entry decides, after the profile, reported with the database reason.
    reset_state();
    write_database({{gow_ascension, gow_ascension_entry}, {gow_3, gow_3_entry}});
    {
        cfg_root cfg;
        std::vector<change> changes;
        const policy_result r = boot(cfg, gow_ascension, &changes);
        CHECK(r.database.use == database_use::applied && r.database.entry && r.database.changes == 3 && r.database.capped.empty());
        CHECK(r.database.keys == 5);
        CHECK(cfg.video.write_color_buffers.get() && cfg.core.rsx_accurate_res_access.get() && !cfg.video.relaxed_zcull_sync.get());
        CHECK(!cfg.video.precise_zpass_count.get() && cfg.core.sleep_timers_accuracy.get() == sleep_timers_accuracy_level::_as_host);
        const change* c = find_named(changes, "Write Color Buffers");
        CHECK(c && c->kind == change_kind::title && is_database_reason(c->why) && c->why == database_reason);
        // 0.37: the profile runs strict ZCULL and accurate stats; the entry turns the stats off (the rows are inactive).
        CHECK(count_named(changes, "Relaxed ZCULL Sync") == 0);
        c = find_named(changes, "Accurate ZCULL stats");
        CHECK(c && is_database_reason(c->why) && count_named(changes, "Accurate ZCULL stats") == 1);
        CHECK(r.title_changes == 3);
        CHECK(contains(g_checkpoints, "config database: /data/rpcs3/config_database.dat (timestamp 1784200630, 2 titles); no bundled copy"));
        // 0.40: the entry's settings with what runs
        CHECK(contains(g_checkpoints, "config database: applied BCUS98232: Accurate RSX reservation access=true, Sleep Timers Accuracy=As Host, "
            "Write Color Buffers=true, Accurate ZCULL stats=false, Relaxed ZCULL Sync=false · 5 keys, 3 value(s) changed"));
    }
    // God of War III: Asynchronous Texture Streaming of its entry stays off (no transfer queue), the rest runs.
    {
        cfg_root cfg;
        std::vector<change> changes;
        const policy_result r = boot(cfg, gow_3, &changes);
        CHECK(r.database.use == database_use::applied);
        CHECK(cfg.video.write_color_buffers.get() && cfg.core.spu_block_size.get() == spu_block_size_type::mega);
        CHECK(!cfg.video.vk.asynchronous_texture_streaming.get());
        // The entry turns it on, then the console limit turns it off.
        std::vector<change_kind> kinds;
        for (const change& c : changes)
            if (c.name == "Asynchronous Texture Streaming") kinds.push_back(c.kind);
        CHECK(kinds.size() == 2 && kinds[0] == change_kind::title && kinds[1] == change_kind::console);
        CHECK(r.console_changes == 1);
    }
    // A database without the game: the fallback rows.
    {
        cfg_root cfg;
        const policy_result r = boot(cfg, gow_ascension_psn_eu);
        CHECK(r.database.use == database_use::none && !r.database.entry);
        CHECK(cfg.video.write_color_buffers.get() && !cfg.video.relaxed_zcull_sync.get());
        CHECK(contains(g_checkpoints, "config database: NPEA00445 has no entry"));
    }
    // Any other game without an entry keeps the profile.
    {
        cfg_root cfg;
        cfg.video.write_color_buffers.set(true);
        boot(cfg, minecraft);
        CHECK(!cfg.video.write_color_buffers.get() && !cfg.video.relaxed_zcull_sync.get() && cfg.video.precise_zpass_count.get());
    }

    // Console caps: output above 720p and an internal scale above 100 % come back; correctness values run.
    reset_state();
    write_database({{minecraft, "Video:\n  Resolution: 1920x1080\n  Resolution Scale: 150\n  Write Color Buffers: true\n"
        "  Strict Rendering Mode: true\n  Multithreaded RSX: true\nCore:\n  PPU Decoder: Interpreter (static)\n"
        "  SPU XFloat Accuracy: Accurate\n  Libraries Control:\n    - libvdec.sprx:lle"}});
    {
        cfg_root cfg;
        std::vector<change> changes;
        const policy_result r = boot(cfg, minecraft, &changes);
        CHECK(cfg.video.resolution.get() == video_resolution::_720p && cfg.video.resolution_scale_percent.get() == 100);
        CHECK(r.database.capped == "Resolution 1920x1080, Resolution Scale 150");
        CHECK(!find_named(changes, "Resolution") && !find_named(changes, "Resolution Scale")); // capped back: no change
        CHECK(cfg.video.write_color_buffers.get() && cfg.video.strict_rendering_mode.get());
        CHECK(cfg.core.ppu_decoder.get() == ppu_decoder_type::_static && cfg.core.spu_xfloat_accuracy.get() == xfloat_accuracy::accurate);
        CHECK(cfg.core.libraries_control.get_set().count("libvdec.sprx:lle") == 1);
        CHECK(!cfg.video.multithreaded_rsx.get()); // console limit after the entry
        // Write Color Buffers, Strict Rendering Mode, Multithreaded RSX, PPU Decoder, XFloat, Libraries Control.
        CHECK(r.database.changes == 6 && r.database.keys == 8);
        CHECK(contains(g_checkpoints, "config database: applied BLUS31426: Resolution=1920x1080 (capped), Resolution Scale=150 (capped), "
            "Write Color Buffers=true, Strict Rendering Mode=true, Multithreaded RSX=true (capped), PPU Decoder=Interpreter (static), "
            "SPU XFloat Accuracy=Accurate, Libraries Control=libvdec.sprx:lle · 8 keys, 6 value(s) changed (capped on PS4: Resolution 1920x1080, Resolution Scale 150)"));
        // The entry's Multithreaded RSX is an unavailable option: one startup.log line.
        CHECK(contains(g_checkpoints, "console limit: 'Multithreaded RSX' = 'false' (was 'true'): unsafe on PS4"));
        const change* lib = find_named(changes, "Libraries Control");
        CHECK(lib && lib->old.empty() && lib->why == database_reason);
    }

    // Rows keep precedence over the entry: Demon's Souls keeps Accurate SPU Reservations on, Ben 10 keeps OpenGL.
    reset_state();
    write_database({{demons_souls, "Core:\n  Accurate SPU Reservations: false\nVideo:\n  Write Color Buffers: true"},
        {ben10, "Video:\n  Renderer: Vulkan\n  Resolution: 1280x720"}});
    {
        cfg_root cfg;
        const policy_result r = boot(cfg, demons_souls);
        CHECK(cfg.core.spu_accurate_reservations.get() && cfg.video.write_color_buffers.get());
        CHECK(r.database.use == database_use::applied && r.database.changes == 0);
    }
    {
        cfg_root cfg;
        boot(cfg, ben10);
        CHECK(cfg.video.renderer.get() == video_renderer::opengl && cfg.video.resolution.get() == video_resolution::_480i);
        CHECK(cfg.video.shadermode.get() == shader_mode::recompiler);
    }

    // An entry that selects OpenGL gets the legacy shader recompiler, unless it names a shader mode itself.
    reset_state();
    write_database({{minecraft, "Video:\n  Renderer: OpenGL"}, {terraria, "Video:\n  Renderer: OpenGL\n  Shader Mode: Async Recompiler with Shader Interpreter"}});
    {
        cfg_root cfg;
        std::vector<change> changes;
        boot(cfg, minecraft, &changes);
        CHECK(cfg.video.renderer.get() == video_renderer::opengl && cfg.video.shadermode.get() == shader_mode::recompiler);
        const change* c = find_named(changes, "Shader Mode");
        CHECK(c && is_database_reason(c->why) && c->kind == change_kind::title);
        CHECK(cfg.video.shader_compiler_threads_count.get() <= 2);
    }
    {
        // 0.40: the entry's mode stays as asked, but without gl-async.flag the OpenGL renderer runs the legacy recompiler:
        // the boot's platform step says so (it ran at renderer start before)
        cfg_root cfg;
        std::vector<change> changes;
        boot(cfg, terraria, &changes);
        CHECK(cfg.video.renderer.get() == video_renderer::opengl && cfg.video.shadermode.get() == shader_mode::recompiler);
        const change* c = nullptr;
        for (const change& item : changes)
            if (item.name == "Shader Mode") c = &item;
        CHECK(c && c->kind == change_kind::console && c->old == "Async Recompiler with Shader Interpreter" && c->why.starts_with(platform_reason_prefix));
        fs::test_files()[gl_async_flag] = "";
        cfg_root with_flag;
        boot(with_flag, terraria);
        CHECK(with_flag.video.renderer.get() == video_renderer::opengl && with_flag.video.shadermode.get() == shader_mode::async_with_interpreter);
        fs::test_files().erase(gl_async_flag);
    }

    // An entry with a key or value this build does not know: none of it runs (PC drops it), the title is rejected
    // until the file changes, and God of War falls back to its rows.
    reset_state();
    write_database({{minecraft, "Video:\n  Write Color Buffers: true\n  Some Future Option: true"},
        {gow_ascension, "Video:\n  Write Color Buffers: true\n  Relaxed ZCULL Sync: maybe"}});
    {
        cfg_root cfg;
        std::vector<change> changes;
        const policy_result r = boot(cfg, minecraft, &changes);
        CHECK(r.database.use == database_use::rejected && r.database.entry);
        CHECK(!cfg.video.write_color_buffers.get() && !find_named(changes, "Write Color Buffers"));
        CHECK(contains(g_checkpoints, "BLUS31426 entry not valid for this build"));
        CHECK(config_db::lookup(minecraft).rejected);
    }
    for (int run = 0; run < 2; run++) // the first boot finds the error, the second starts from the rows
    {
        cfg_root cfg;
        std::vector<change> changes;
        const policy_result r = boot(cfg, gow_ascension, &changes);
        CHECK(r.database.use == database_use::rejected);
        CHECK(cfg.video.write_color_buffers.get() && cfg.core.rsx_accurate_res_access.get() && !cfg.video.relaxed_zcull_sync.get());
        const change* c = find_named(changes, "Accurate RSX reservation access");
        CHECK(c && c->why.find("no usable database entry") != std::string::npos);
    }
}

void test_config_db_modes()
{
    // Automatic configuration off: the entry runs for a game without its own YAML (PC: no custom config), before
    // the every-mode rows; a game with its own YAML runs it as saved.
    reset_state();
    write_database({{minecraft, "Video:\n  Write Color Buffers: true\n  Relaxed ZCULL Sync: false"},
        {demons_souls, "Core:\n  Accurate SPU Reservations: false"}});
    CHECK(set_auto_config_enabled(false));
    {
        cfg_root cfg;
        std::vector<change> changes;
        const policy_result r = boot(cfg, minecraft, &changes);
        CHECK(r.mode == policy_mode::auto_config_off && r.database.use == database_use::applied && r.profile_changes == 0);
        CHECK(cfg.video.write_color_buffers.get() && !cfg.video.relaxed_zcull_sync.get());
    }
    {
        cfg_root cfg;
        cfg.core.spu_accurate_reservations.set(false);
        boot(cfg, demons_souls);
        CHECK(cfg.core.spu_accurate_reservations.get()); // the every-mode row still wins
    }
    fs::test_files()[rpcs3::utils::get_custom_config_path(minecraft)] = "Video:\n  Write Color Buffers: false\n";
    CHECK(has_custom_config(minecraft) && !has_custom_config(terraria) && !has_custom_config(""));
    {
        cfg_root cfg;
        const policy_result r = boot(cfg, minecraft);
        CHECK(r.database.use == database_use::custom_config && r.database.entry);
        CHECK(!cfg.video.write_color_buffers.get() && cfg.video.relaxed_zcull_sync.get() == false);
        CHECK(contains(g_checkpoints, "automatic configuration off and the game has its own config (as on PC)"));
    }
    CHECK(set_auto_config_enabled(true));

    // User-tuned: the saved YAML runs as saved, without the entry. A save that equals automatic (entry included)
    // is a stale marker.
    reset_state();
    write_database({{minecraft, "Video:\n  Write Color Buffers: true"}});
    std::string saved;
    {
        cfg_root cfg;
        boot(cfg, minecraft);
        CHECK(cfg.video.write_color_buffers.get());
        saved = cfg.to_string();
    }
    CHECK(set_user_tuned(minecraft, true));
    {
        cfg_root cfg;
        CHECK(cfg.from_string(saved));
        CHECK(saved_equals_automatic(cfg, minecraft));
        CHECK(cfg.to_string() == saved);
        const policy_result r = boot(cfg, minecraft);
        CHECK(r.mode == policy_mode::automatic && r.stale_marker && r.database.use == database_use::applied);
        CHECK(cfg.video.write_color_buffers.get());
    }
    {
        cfg_root cfg;
        CHECK(cfg.from_string(saved));
        cfg.video.write_color_buffers.set(false);
        CHECK(!saved_equals_automatic(cfg, minecraft));
        std::vector<change> changes;
        const policy_result r = boot(cfg, minecraft, &changes);
        CHECK(r.mode == policy_mode::user_tuned && r.database.use == database_use::manual && r.database.entry);
        CHECK(!cfg.video.write_color_buffers.get() && !find_named(changes, "Write Color Buffers"));
        CHECK(contains(g_checkpoints, "BLUS31426 entry not applied: settings saved from the per-game menu run as saved"));
    }
    // A set in the saved values (Libraries Control) is compared and put back like any other setting.
    reset_state();
    write_database({{terraria, "Core:\n  Libraries Control:\n    - libvdec.sprx:lle"}});
    {
        cfg_root cfg;
        boot(cfg, terraria);
        CHECK(cfg.core.libraries_control.get_set().count("libvdec.sprx:lle") == 1);
        cfg_root again;
        CHECK(again.from_string(cfg.to_string()));
        CHECK(saved_equals_automatic(again, terraria));
        again.core.libraries_control.from_list({});
        const std::string before = again.to_string();
        CHECK(!saved_equals_automatic(again, terraria)); // the entry would add libvdec LLE
        CHECK(again.to_string() == before && again.core.libraries_control.get_set().empty()); // put back
    }

    // keep-user-settings.flag: nothing at all, not even the entry.
    reset_state();
    write_database({{minecraft, "Video:\n  Write Color Buffers: true"}});
    fs::test_files()[keep_user_settings_flag] = "1";
    {
        cfg_root cfg;
        cfg.core.llvm_cpu.set("haswell");
        const policy_result r = boot(cfg, minecraft);
        CHECK(r.mode == policy_mode::keep_user_settings && r.database.use == database_use::none);
        CHECK(!cfg.video.write_color_buffers.get() && cfg.core.llvm_cpu.get() == "haswell");
    }
}

// 'Use LLVM CPU' is a console limit: the native CPU in every mode but keep-user-settings.flag.
void test_llvm_cpu_limit()
{
    for (const int mode : {0, 1, 2})
    {
        reset_state();
        if (mode == 1) CHECK(set_user_tuned(terraria, true));
        if (mode == 2) CHECK(set_auto_config_enabled(false));
        cfg_root cfg;
        cfg.core.llvm_cpu.set("haswell");
        cfg.core.max_spurs_threads.set(4); // a real manual value, so user-tuned stays user-tuned
        std::vector<change> changes;
        const policy_result r = boot(cfg, terraria, &changes);
        CHECK(r.mode == (mode == 0 ? policy_mode::automatic : mode == 1 ? policy_mode::user_tuned : policy_mode::auto_config_off));
        CHECK(cfg.core.llvm_cpu.get().empty());
        const change* c = find_named(changes, "Use LLVM CPU");
        CHECK(c && c->old == "haswell");
        CHECK(c && c->kind == (mode == 0 ? change_kind::profile : change_kind::console));
    }
    cfg_root cfg;
    CHECK(unsafe_option_for(cfg, cfg.core.llvm_cpu) != nullptr);
}

// Per-game accurate DFMA (custom_configs/<TITLE_ID>.accurate-dfma).
void test_accurate_dfma_marker()
{
    reset_state();
    const auto run = [](cfg_root& cfg, std::string_view title)
    {
        u32 n = 0;
        apply_hardware_limits(cfg, title, [&](const cfg::_base&, const std::string&, std::string_view) { n++; });
        return n;
    };
    {
        cfg_root cfg; // 'Use Accurate DFMA: true' (PC default) and no marker: off on Jaguar
        CHECK(run(cfg, minecraft) == 1 && !cfg.core.use_accurate_dfma.get());
    }
    CHECK(accurate_dfma_marker(minecraft) == "/data/rpcs3/config/custom_configs/BLUS31426.accurate-dfma" && accurate_dfma_marker("").empty());
    CHECK(set_title_accurate_dfma(minecraft, true) && title_accurate_dfma(minecraft) && !title_accurate_dfma(terraria));
    {
        cfg_root cfg;
        cfg.core.use_accurate_dfma.set(false); // the marker wins over the YAML
        CHECK(run(cfg, minecraft) == 1 && cfg.core.use_accurate_dfma.get());
        CHECK(run(cfg, minecraft) == 0 && cfg.core.use_accurate_dfma.get());
        CHECK(run(cfg, terraria) == 1 && !cfg.core.use_accurate_dfma.get()); // only for its own game
    }
    {
        cfg_root cfg; // the global form never reads a marker
        apply_hardware_limits(cfg, [](const cfg::_base&, const std::string&, std::string_view) {});
        CHECK(!cfg.core.use_accurate_dfma.get());
    }
    fs::test_files()[accurate_dfma_flag] = "1";
    {
        cfg_root cfg;
        CHECK(run(cfg, terraria) == 0 && cfg.core.use_accurate_dfma.get());
    }
    CHECK(set_title_accurate_dfma(minecraft, false) && !title_accurate_dfma(minecraft));
    CHECK(!set_title_accurate_dfma("", true));
}

// 0.34: 'Use native user interface' is a console limit: a saved YAML or a copied PC config with it off runs with it on.
void test_native_interface()
{
    for (const int mode : {0, 1, 2})
    {
        reset_state();
        if (mode == 1) CHECK(set_user_tuned(terraria, true));
        if (mode == 2) CHECK(set_auto_config_enabled(false));
        cfg_root cfg;
        CHECK(cfg.from_string("Miscellaneous:\n  Use native user interface: false\n"));
        CHECK(!cfg.misc.use_native_interface.get());
        cfg.core.max_spurs_threads.set(4); // a real manual value, so user-tuned stays user-tuned
        std::vector<change> changes;
        const policy_result r = boot(cfg, terraria, &changes);
        CHECK(r.mode == (mode == 0 ? policy_mode::automatic : mode == 1 ? policy_mode::user_tuned : policy_mode::auto_config_off));
        CHECK(cfg.misc.use_native_interface.get());
        const change* c = find_named(changes, "Use native user interface");
        CHECK(c && c->kind == change_kind::console && c->old == "false" && c->now == "true" && !c->why.empty());
        CHECK(count_named(changes, "Use native user interface") == 1);
    }
    // Already on (upstream default): nothing to report.
    {
        reset_state();
        cfg_root cfg;
        std::vector<change> changes;
        boot(cfg, terraria, &changes);
        CHECK(cfg.misc.use_native_interface.get() && !find_named(changes, "Use native user interface"));
    }
    // keep-user-settings.flag: the policy forces nothing (System.cpp forces this one itself).
    {
        reset_state();
        fs::test_files()[keep_user_settings_flag] = "";
        cfg_root cfg;
        cfg.misc.use_native_interface.set(false);
        boot(cfg, terraria);
        CHECK(!cfg.misc.use_native_interface.get());
    }
    cfg_root cfg;
    const unsafe_option* u = unsafe_option_for(cfg, cfg.misc.use_native_interface);
    CHECK(u && u->value == "true" && !u->why.empty() && *i18n::tr(u->menu));
}

// 0.34: host_sleep_requested: the title's PC entry asks 'Sleep Timers Accuracy: As Host' explicitly, from the applied
// database entry or from a God of War fallback row.
void test_host_sleep_request()
{
    // The text check itself.
    using policy_detail::entry_writes;
    CHECK(entry_writes(gow_ascension_entry, "Sleep Timers Accuracy", "As Host"));
    CHECK(entry_writes("Core:\r\n  Sleep Timers Accuracy: \"As Host\"\r\n", "Sleep Timers Accuracy", "As Host"));
    CHECK(entry_writes("Core:\n    Sleep Timers Accuracy: 'As Host'  # PC default\n", "Sleep Timers Accuracy", "As Host"));
    CHECK(entry_writes("Video:\n  Write Color Buffers: true\nCore:\n  Sleep Timers Accuracy: As Host", "Sleep Timers Accuracy", "As Host"));
    CHECK(!entry_writes("Core:\n  Sleep Timers Accuracy: Usleep Only\n", "Sleep Timers Accuracy", "As Host"));
    CHECK(!entry_writes("Core:\n  Sleep Timers Accuracy2: As Host\n", "Sleep Timers Accuracy", "As Host"));
    CHECK(!entry_writes("Core:\n  Sleep Timers Accuracy As Host\n", "Sleep Timers Accuracy", "As Host"));
    CHECK(!entry_writes("Video:\n  Write Color Buffers: true", "Sleep Timers Accuracy", "As Host"));
    CHECK(!entry_writes("", "Sleep Timers Accuracy", "As Host"));

    // No database on the console: the six God of War: Ascension rows carry it; God of War III and others do not.
    for (const char* id : {"BCUS98232", "BCES01741", "BCES01742", "NPUA80918", "NPEA00445", "NPHA80258"})
    {
        reset_state();
        cfg_root cfg;
        const policy_result r = boot(cfg, id);
        CHECK(r.host_sleep_requested);
        CHECK(cfg.core.sleep_timers_accuracy.get() == sleep_timers_accuracy_level::_as_host);
    }
    for (const char* id : {gow_3, minecraft, demons_souls, ""})
    {
        reset_state();
        cfg_root cfg;
        CHECK(!boot(cfg, id).host_sleep_requested);
    }

    // The database lists the title: its entry text decides.
    reset_state();
    write_database({{gow_ascension, gow_ascension_entry}, {minecraft, "Video:\n  Write Color Buffers: true"},
        {terraria, "Core:\n  Sleep Timers Accuracy: As Host\n  SPU Block Size: Mega"},
        {ben10, "Core:\n  Sleep Timers Accuracy: Usleep Only"}});
    {
        cfg_root cfg;
        const policy_result r = boot(cfg, gow_ascension);
        CHECK(r.database.use == database_use::applied && r.database.host_sleep && r.host_sleep_requested);
    }
    {
        cfg_root cfg;
        const policy_result r = boot(cfg, minecraft);
        CHECK(r.database.use == database_use::applied && !r.database.host_sleep && !r.host_sleep_requested);
    }
    {
        cfg_root cfg; // any title whose entry writes it
        const policy_result r = boot(cfg, terraria);
        CHECK(r.database.host_sleep && r.host_sleep_requested);
        CHECK(cfg.core.spu_block_size.get() == spu_block_size_type::mega);
    }
    {
        cfg_root cfg; // another value is not 'As Host'
        const policy_result r = boot(cfg, ben10);
        CHECK(!r.host_sleep_requested);
    }
    // The database exists but lacks this God of War serial: the row asks it.
    {
        cfg_root cfg;
        CHECK(boot(cfg, gow_ascension_psn_eu).host_sleep_requested);
    }

    // A rejected entry does not count; God of War's fallback rows still ask it (first boot finds the error, the second
    // starts from the rows).
    reset_state();
    write_database({{minecraft, "Core:\n  Sleep Timers Accuracy: As Host\n  Some Future Option: true"},
        {gow_ascension, "Core:\n  Sleep Timers Accuracy: As Host\nVideo:\n  Relaxed ZCULL Sync: maybe"}});
    for (int run = 0; run < 2; run++)
    {
        cfg_root cfg;
        const policy_result r = boot(cfg, minecraft);
        CHECK(r.database.use == database_use::rejected && !r.host_sleep_requested);
        cfg_root gow;
        const policy_result g = boot(gow, gow_ascension);
        CHECK(g.database.use == database_use::rejected && g.host_sleep_requested);
    }

    // Other modes: user-tuned games run their YAML (no entry, no rows); automatic configuration off applies the entry
    // to a game without its own YAML only; keep-user-settings.flag: nothing.
    reset_state();
    write_database({{terraria, "Core:\n  Sleep Timers Accuracy: As Host"}});
    CHECK(set_user_tuned(gow_ascension, true));
    {
        cfg_root cfg;
        cfg.core.max_spurs_threads.set(4);
        const policy_result r = boot(cfg, gow_ascension);
        CHECK(r.mode == policy_mode::user_tuned && !r.host_sleep_requested);
    }
    CHECK(set_auto_config_enabled(false));
    {
        cfg_root cfg;
        const policy_result r = boot(cfg, terraria);
        CHECK(r.mode == policy_mode::auto_config_off && r.database.use == database_use::applied && r.host_sleep_requested);
        cfg_root gow;
        CHECK(!boot(gow, gow_ascension_psn_eu).host_sleep_requested); // fallback rows are automatic-only
    }
    fs::test_files()[rpcs3::utils::get_custom_config_path(terraria)] = "Video:\n  Write Color Buffers: false\n";
    {
        cfg_root cfg;
        const policy_result r = boot(cfg, terraria);
        CHECK(r.database.use == database_use::custom_config && !r.host_sleep_requested);
    }
    reset_state();
    fs::test_files()[keep_user_settings_flag] = "";
    {
        cfg_root cfg;
        const policy_result r = boot(cfg, gow_ascension);
        CHECK(r.mode == policy_mode::keep_user_settings && !r.host_sleep_requested);
    }

    // The preview (settings menu) reports the same as the boot.
    reset_state();
    {
        cfg_root cfg;
        CHECK(apply_boot_policy_preview(cfg, gow_ascension, [](const cfg::_base&, const std::string&, std::string_view) {}).host_sleep_requested);
    }
}

// 0.34: 'Modo frio' (cool-mode.flag): the automatic configuration polls the pads every 8 ms instead of the upstream 1 ms.
void test_cool_mode()
{
    reset_state();
    {
        cfg_root cfg; // off: the upstream value runs
        const policy_result r = boot(cfg, minecraft);
        CHECK(cfg.io.pad_sleep.get() == 1000 && r.runtime_changes == 3);
        CHECK(!read_runtime_inputs(minecraft).cool);
    }
    cool_mode::detail::state().store(1, std::memory_order_relaxed);
    CHECK(read_runtime_inputs(minecraft).cool && read_runtime_inputs("").cool);
    {
        cfg_root cfg;
        std::vector<change> changes;
        const policy_result r = boot(cfg, minecraft, &changes);
        CHECK(r.mode == policy_mode::automatic && cfg.io.pad_sleep.get() == 8000);
        const change* c = find_named(changes, "Pad handler sleep (microseconds)");
        CHECK(c && c->kind == change_kind::runtime && c->old == "1000" && c->now == "8000" && c->why.find("Modo frio") != std::string::npos);
        CHECK(r.runtime_changes == 4);
    }
    {
        cfg_root cfg; // a value the player chose stays
        cfg.io.pad_sleep.set(2000);
        boot(cfg, minecraft);
        CHECK(cfg.io.pad_sleep.get() == 2000);
    }
    {
        cfg_root cfg; // the global config too (Emu.Init)
        boot(cfg, "");
        CHECK(cfg.io.pad_sleep.get() == 8000);
    }
    // Only the automatic configuration: a game saved from the menu or the switch off runs its value.
    CHECK(set_user_tuned(terraria, true));
    {
        cfg_root cfg;
        cfg.core.max_spurs_threads.set(4);
        const policy_result r = boot(cfg, terraria);
        CHECK(r.mode == policy_mode::user_tuned && cfg.io.pad_sleep.get() == 1000);
    }
    CHECK(set_auto_config_enabled(false));
    {
        cfg_root cfg;
        boot(cfg, minecraft);
        CHECK(cfg.io.pad_sleep.get() == 1000);
    }
    CHECK(set_auto_config_enabled(true));
    fs::test_files()[keep_user_settings_flag] = "";
    {
        cfg_root cfg;
        boot(cfg, minecraft);
        CHECK(cfg.io.pad_sleep.get() == 1000);
    }
    fs::test_files().erase(keep_user_settings_flag);
    // The menu's preview with inputs it read itself.
    {
        runtime_inputs in;
        in.cool = true;
        cfg_root cfg;
        std::vector<change_kind> kinds;
        apply_boot_policy_preview(cfg, minecraft, in, [&](const cfg::_base& setting, const std::string&, std::string_view, change_kind kind)
        {
            if (setting.get_name() == "Pad handler sleep (microseconds)") kinds.push_back(kind);
        });
        CHECK(cfg.io.pad_sleep.get() == 8000 && kinds.size() == 1 && kinds[0] == change_kind::runtime);
        in.cool = false;
        cfg_root off;
        apply_boot_policy_preview(off, minecraft, in, [](const cfg::_base&, const std::string&, std::string_view) {});
        CHECK(off.io.pad_sleep.get() == 1000);
    }
    // A saved YAML that carries the cool value is not a manual setting by that value alone (the comparison of the
    // marker check leaves runtime values out), and it is never raised again from 8000.
    {
        std::string saved;
        {
            cfg_root cfg;
            boot(cfg, minecraft);
            saved = cfg.to_string();
        }
        CHECK(saved.find("Pad handler sleep (microseconds): 8000") != std::string::npos);
        CHECK(set_user_tuned(minecraft, true));
        cfg_root cfg;
        CHECK(cfg.from_string(saved));
        CHECK(saved_equals_automatic(cfg, minecraft));
        const policy_result r = boot(cfg, minecraft);
        CHECK(r.mode == policy_mode::automatic && r.stale_marker && cfg.io.pad_sleep.get() == 8000);
        CHECK(set_user_tuned(minecraft, false));
    }
    reset_state();
    CHECK(!read_runtime_inputs(minecraft).cool);
}

// 0.34: vk-compile-3.flag: a third Vulkan pipeline compile worker, opt-in, never with an adaptation to fewer threads.
void test_vk_compile_3()
{
    reset_state();
    {
        cfg_root cfg; // absent: the automatic 2 workers, no note in the policy line
        const policy_result r = boot(cfg, minecraft);
        CHECK(!read_runtime_inputs(minecraft).compile_3);
        CHECK(cfg.video.shader_compiler_threads_count.get() == 2 && r.runtime_changes == 3);
        CHECK(!contains(g_checkpoints, "vk-compile-3.flag"));
    }
    fs::test_files()[vk_compile_3_flag] = "";
    CHECK(read_runtime_inputs(minecraft).compile_3 && read_runtime_inputs("").compile_3);
    {
        g_checkpoints.clear();
        cfg_root cfg;
        std::vector<change> changes;
        const policy_result r = boot(cfg, minecraft, &changes);
        CHECK(r.mode == policy_mode::automatic && cfg.video.shader_compiler_threads_count.get() == 3);
        CHECK(r.runtime_changes == 4 && r.adapt_changes == 0 && r.console_changes == 0);
        const change* last = nullptr;
        for (const auto& c : changes)
            if (c.name == "Shader Compiler Threads") last = &c;
        CHECK(last && last->kind == change_kind::runtime && last->old == "2" && last->now == "3" &&
            last->why.find("vk-compile-3.flag") != std::string::npos);
        CHECK(contains(g_checkpoints, "Política PS4 para BLUS31426: offline, UPnP desligado · VRAM até 768 MB · cache de disco até 4096 MB"
            " · shaders Vulkan com 3 threads (vk-compile-3.flag)"));
    }
    {
        cfg_root cfg; // the global config too (Emu.Init)
        boot(cfg, "");
        CHECK(cfg.video.shader_compiler_threads_count.get() == 3);
    }
    // An adaptation to fewer threads wins: one worker, and the line says the flag had no effect.
    for (const char* key : {adapt::key_threads_low, adapt::key_llvm_threads_1})
    {
        reset_state();
        fs::test_files()[vk_compile_3_flag] = "";
        CHECK(adapt::append(demons_souls, key));
        cfg_root cfg;
        std::vector<change> changes;
        boot(cfg, demons_souls, &changes);
        CHECK(cfg.video.shader_compiler_threads_count.get() == 1 && cfg.core.llvm_threads.get() == 1);
        for (const auto& c : changes) CHECK(c.why.find("vk-compile-3.flag") == std::string::npos);
        CHECK(contains(g_checkpoints, "vk-compile-3.flag sem efeito (ajuste automático de threads ativo)"));
    }
    reset_state();
    fs::test_files()[vk_compile_3_flag] = "";
    // User-tuned: a per-game 1 stays; a saved 2 (or a capped 8) becomes 3; OpenGL is never touched.
    CHECK(set_user_tuned(terraria, true));
    {
        cfg_root cfg;
        cfg.core.max_spurs_threads.set(4);
        cfg.video.shader_compiler_threads_count.set(1);
        const policy_result r = boot(cfg, terraria);
        CHECK(r.mode == policy_mode::user_tuned && cfg.video.shader_compiler_threads_count.get() == 1);
        CHECK(contains(g_checkpoints, "vk-compile-3.flag sem efeito (valor do jogo mantido)"));
    }
    {
        cfg_root cfg;
        cfg.core.max_spurs_threads.set(4);
        cfg.video.shader_compiler_threads_count.set(8);
        std::vector<change> changes;
        boot(cfg, terraria, &changes);
        CHECK(cfg.video.shader_compiler_threads_count.get() == 3);
        CHECK(count_named(changes, "Shader Compiler Threads") == 2); // console limit 8 -> 2, then the flag 2 -> 3
    }
    {
        g_checkpoints.clear();
        cfg_root cfg;
        cfg.core.max_spurs_threads.set(4);
        cfg.video.renderer.set(video_renderer::opengl);
        boot(cfg, terraria);
        CHECK(cfg.video.renderer.get() == video_renderer::opengl && cfg.video.shader_compiler_threads_count.get() == 0);
        CHECK(contains(g_checkpoints, "vk-compile-3.flag sem efeito (renderizador não é Vulkan)"));
    }
    CHECK(set_user_tuned(terraria, false));
    // keep-user-settings.flag: nothing is forced.
    fs::test_files()[keep_user_settings_flag] = "";
    {
        cfg_root cfg;
        boot(cfg, minecraft);
        CHECK(cfg.video.shader_compiler_threads_count.get() == 0);
    }
    fs::test_files().erase(keep_user_settings_flag);
    // The menu's preview with inputs it read itself reports it as a runtime value (shown, never saved).
    {
        runtime_inputs in;
        in.compile_3 = true;
        cfg_root cfg;
        std::vector<change_kind> kinds;
        apply_boot_policy_preview(cfg, minecraft, in, [&](const cfg::_base& setting, const std::string&, std::string_view, change_kind kind)
        {
            if (setting.get_name() == "Shader Compiler Threads") kinds.push_back(kind);
        });
        CHECK(cfg.video.shader_compiler_threads_count.get() == 3 && kinds.size() == 2);
        CHECK(kinds.size() == 2 && kinds[0] == change_kind::profile && kinds[1] == change_kind::runtime);
    }
    // A YAML saved while the flag was on carries 3: it is not a manual setting by that value alone, and without the
    // flag the console limit brings it back to 2.
    {
        std::string saved;
        {
            cfg_root cfg;
            boot(cfg, minecraft);
            saved = cfg.to_string();
        }
        CHECK(saved.find("Shader Compiler Threads: 3") != std::string::npos);
        fs::test_files().erase(vk_compile_3_flag);
        CHECK(set_user_tuned(minecraft, true));
        cfg_root cfg;
        CHECK(cfg.from_string(saved));
        CHECK(saved_equals_automatic(cfg, minecraft));
        const policy_result r = boot(cfg, minecraft);
        CHECK(r.mode == policy_mode::automatic && r.stale_marker && cfg.video.shader_compiler_threads_count.get() == 2);
        CHECK(set_user_tuned(minecraft, false));
    }
    reset_state();
    CHECK(!read_runtime_inputs(minecraft).compile_3);
}

// 0.34: Sonic & SEGA All-Stars Racing and Silent Hill: Downpour fallback rows (their PC config_database.dat entries).
void test_fallback_rows_034()
{
    constexpr char sonic[] = "BLES00750";
    constexpr char silent_hill[] = "BLUS30565";
    reset_state();
    {
        cfg_root cfg;
        std::vector<change> changes;
        const policy_result r = boot(cfg, sonic, &changes);
        CHECK(cfg.video.antialiasing_level.get() == msaa_level::none);
        CHECK(cfg.video.write_color_buffers.get()); // 0.37: the entry's Write Color Buffers too, as on PC
        const change* c = find_named(changes, "MSAA");
        CHECK(c && c->kind == change_kind::title && c->old == "Auto" && c->now == "Disabled" && c->why.find("no usable database entry") != std::string::npos);
        c = find_named(changes, "Write Color Buffers");
        CHECK(c && c->kind == change_kind::title && count_named(changes, "Write Color Buffers") == 1);
        CHECK(r.title_changes == 2 && !r.host_sleep_requested);
    }
    {
        cfg_root cfg;
        std::vector<change> changes;
        const policy_result r = boot(cfg, silent_hill, &changes);
        CHECK(cfg.video.disable_vertex_cache.get());
        const change* c = find_named(changes, "Disable Vertex Cache");
        CHECK(c && c->kind == change_kind::title && r.title_changes == 1);
    }
    {
        cfg_root cfg; // other titles keep the defaults
        boot(cfg, minecraft);
        CHECK(cfg.video.antialiasing_level.get() == msaa_level::_auto && !cfg.video.disable_vertex_cache.get());
    }
    // The database lists Sonic: its entry runs as on PC (Write Color Buffers included); the row steps aside.
    write_database({{sonic, "Video:\n  MSAA: Disabled\n  Write Color Buffers: true"}});
    {
        cfg_root cfg;
        std::vector<change> changes;
        const policy_result r = boot(cfg, sonic, &changes);
        CHECK(r.database.use == database_use::applied && r.database.changes == 2);
        CHECK(cfg.video.antialiasing_level.get() == msaa_level::none && cfg.video.write_color_buffers.get());
        const change* c = find_named(changes, "MSAA");
        CHECK(c && is_database_reason(c->why));
    }
    // A database without Silent Hill: the row.
    {
        cfg_root cfg;
        boot(cfg, silent_hill);
        CHECK(cfg.video.disable_vertex_cache.get());
    }
    // Automatic configuration off / user-tuned: fallback rows do not run.
    reset_state();
    CHECK(set_auto_config_enabled(false));
    {
        cfg_root cfg;
        boot(cfg, sonic);
        CHECK(cfg.video.antialiasing_level.get() == msaa_level::_auto);
    }
    {
        cfg_root cfg;
        const title_compat* e = title_entry_for(cfg, silent_hill, cfg.video.disable_vertex_cache);
        CHECK(e && e->scope == title_scope::database_fallback && !e->host_sleep);
        CHECK(!title_entry_for(cfg, silent_hill, cfg.video.disable_vertex_cache, false));
    }
}

// 0.37: the profile's Shader Mode and ZCULL values are the PC defaults; the escape files bring back the 0.36 values.
void test_profile_defaults_037()
{
    reset_state();
    CHECK(profile_vulkan_shader_mode() == shader_mode::async_with_interpreter && !profile_zcull_relaxed());
    {
        cfg_root cfg; // upstream defaults: nothing to force for these three
        std::vector<change> changes;
        boot(cfg, minecraft, &changes);
        CHECK(cfg.video.shadermode.get() == shader_mode::async_with_interpreter);
        CHECK(!cfg.video.relaxed_zcull_sync.get() && cfg.video.precise_zpass_count.get());
        CHECK(cfg.video.shader_compiler_threads_count.get() == 2);
        CHECK(!find_named(changes, "Shader Mode") && !find_named(changes, "Relaxed ZCULL Sync") && !find_named(changes, "Accurate ZCULL stats"));
    }
    {
        cfg_root cfg; // the 0.36 values a saved YAML carries: the automatic profile puts the PC values back
        cfg.video.shadermode.set(shader_mode::async_recompiler);
        cfg.video.relaxed_zcull_sync.set(true);
        cfg.video.precise_zpass_count.set(false);
        std::vector<change> changes;
        const policy_result r = boot(cfg, minecraft, &changes);
        CHECK(cfg.video.shadermode.get() == shader_mode::async_with_interpreter);
        CHECK(!cfg.video.relaxed_zcull_sync.get() && cfg.video.precise_zpass_count.get());
        for (const char* name : {"Shader Mode", "Relaxed ZCULL Sync", "Accurate ZCULL stats"})
        {
            const change* c = find_named(changes, name);
            CHECK(c && c->kind == change_kind::profile && count_named(changes, name) == 1);
        }
        CHECK(r.migrated_changes == 0 && !contains(g_checkpoints, "profile: '")); // the automatic mode needs no migration
    }
    // shader-skip-draws.flag: 'Async Recompiler' on Vulkan again; the ZCULL values stay.
    fs::test_files()[shader_skip_draws_flag] = "";
    {
        cfg_root cfg;
        std::vector<change> changes;
        boot(cfg, minecraft, &changes);
        CHECK(cfg.video.shadermode.get() == shader_mode::async_recompiler);
        const change* c = find_named(changes, "Shader Mode");
        CHECK(c && c->kind == change_kind::profile && c->old == "Async Recompiler with Shader Interpreter");
        CHECK(!cfg.video.relaxed_zcull_sync.get() && cfg.video.precise_zpass_count.get());
    }
    {
        cfg_root cfg; // OpenGL keeps the legacy recompiler of its row
        boot(cfg, ben10);
        CHECK(cfg.video.renderer.get() == video_renderer::opengl && cfg.video.shadermode.get() == shader_mode::recompiler);
    }
    fs::test_files().erase(shader_skip_draws_flag);
    // zcull-relaxed.flag: the 0.36 pair for every title without a row or an entry that decides it.
    fs::test_files()[zcull_relaxed_flag] = "";
    {
        cfg_root cfg;
        boot(cfg, minecraft);
        CHECK(cfg.video.relaxed_zcull_sync.get() && !cfg.video.precise_zpass_count.get());
        CHECK(cfg.video.shadermode.get() == shader_mode::async_with_interpreter);
    }
    {
        cfg_root cfg; // God of War: Ascension's row keeps its PC entry's strict sync
        boot(cfg, gow_ascension);
        CHECK(!cfg.video.relaxed_zcull_sync.get() && !cfg.video.precise_zpass_count.get());
    }
    write_database({{minecraft, "Video:\n  Relaxed ZCULL Sync: false\n  Accurate ZCULL stats: true"}});
    {
        cfg_root cfg; // and an entry still decides
        boot(cfg, minecraft);
        CHECK(!cfg.video.relaxed_zcull_sync.get() && cfg.video.precise_zpass_count.get());
    }
    reset_state();
    // Demon's Souls, Wolverine, inFamous: an entry asking the relaxed pair gets it over the strict profile.
    write_database({{minecraft, "Video:\n  Relaxed ZCULL Sync: true\n  Accurate ZCULL stats: false"}});
    {
        cfg_root cfg;
        const policy_result r = boot(cfg, minecraft);
        CHECK(cfg.video.relaxed_zcull_sync.get() && !cfg.video.precise_zpass_count.get() && r.database.changes == 2);
    }
    // An OpenGL entry that names the PC default Shader Mode keeps it (it named one); 0.40: it runs as named with
    // gl-async.flag, else the platform step gives the legacy recompiler the renderer would switch to
    reset_state();
    write_database({{minecraft, "Video:\n  Renderer: OpenGL\n  Shader Mode: Async Recompiler with Shader Interpreter"}});
    {
        cfg_root cfg;
        boot(cfg, minecraft);
        CHECK(cfg.video.renderer.get() == video_renderer::opengl && cfg.video.shadermode.get() == shader_mode::recompiler);
        fs::test_files()[gl_async_flag] = "";
        cfg_root with_flag;
        boot(with_flag, minecraft);
        CHECK(with_flag.video.renderer.get() == video_renderer::opengl && with_flag.video.shadermode.get() == shader_mode::async_with_interpreter);
        fs::test_files().erase(gl_async_flag);
    }
}

// 0.37: the profile version stamp and the migration of values an older profile forced into a saved YAML.
void test_profile_migration()
{
    constexpr char mk[] = "BLUS30902";
    // The stamp in a YAML text.
    CHECK(saved_profile_version("") == 0);
    CHECK(saved_profile_version("Core:\n  PPU Threads: 2\n") == 0);
    CHECK(saved_profile_version("Core:\n  PPU Threads: 2\nPS4 automatic profile: 37\n") == 37);
    CHECK(saved_profile_version("PS4 automatic profile: \"38\"\r\nVideo:\r\n") == 38);
    CHECK(saved_profile_version("PS4 automatic profile2: 37\nPS4 automatic profile: x\n") == 0);
    CHECK(saved_profile_version("PS4 automatic profile 37\n") == 0);
    CHECK(with_profile_stamp("Core:\n  PPU Threads: 2") == "Core:\n  PPU Threads: 2\nPS4 automatic profile: 40\n");
    CHECK(with_profile_stamp("Video:\n") == "Video:\nPS4 automatic profile: 40\n");
    CHECK(saved_profile_version(with_profile_stamp("Video:\n  Renderer: Vulkan")) == automatic_profile_version);
    {
        cfg_root cfg; // a config load skips the stamp like any unknown key
        CHECK(cfg.from_string(with_profile_stamp(cfg.to_string())));
    }

    // Mortal Kombat as saved from the 0.3x menu: the profile's 0.36 values plus the player's Write Color Buffers.
    const auto saved_by_036 = [](const char* title, bool wcb)
    {
        cfg_root cfg;
        boot(cfg, title);
        cfg.video.shadermode.set(shader_mode::async_recompiler);
        cfg.video.relaxed_zcull_sync.set(true);
        cfg.video.precise_zpass_count.set(false);
        if (wcb) cfg.video.write_color_buffers.set(true);
        return cfg.to_string();
    };
    reset_state();
    const std::string mk_yaml = saved_by_036(mk, true);
    reset_state();
    CHECK(set_user_tuned(mk, true));
    fs::test_files()[rpcs3::utils::get_custom_config_path(mk)] = mk_yaml;
    {
        cfg_root cfg;
        CHECK(cfg.from_string(mk_yaml));
        std::vector<change> changes;
        const policy_result r = boot(cfg, mk, &changes);
        CHECK(r.mode == policy_mode::user_tuned && !r.stale_marker);
        CHECK(r.migrated_changes == 3 && r.profile_changes == 0 && r.title_changes == 0);
        CHECK(cfg.video.shadermode.get() == shader_mode::async_with_interpreter);
        CHECK(!cfg.video.relaxed_zcull_sync.get() && cfg.video.precise_zpass_count.get());
        CHECK(cfg.video.write_color_buffers.get()); // the player's choice stays
        CHECK(contains(g_checkpoints, "profile: 'Shader Mode' saved by automatic profile <=36 ('Async Recompiler (multi-threaded)') -> 40 "
            "('Async Recompiler with Shader Interpreter')"));
        CHECK(contains(g_checkpoints, "profile: 'Relaxed ZCULL Sync' saved by automatic profile <=36 ('true') -> 40 ('false')"));
        CHECK(contains(g_checkpoints, "profile: 'Accurate ZCULL stats' saved by automatic profile <=36 ('false') -> 40 ('true')"));
        const change* c = find_named(changes, "Shader Mode");
        CHECK(c && c->kind == change_kind::migration && c->why.find("automatic profile <=36") != std::string::npos);
    }
    // The same YAML stamped by a 0.37 save runs as saved.
    fs::test_files()[rpcs3::utils::get_custom_config_path(mk)] = with_profile_stamp(mk_yaml);
    {
        g_checkpoints.clear();
        cfg_root cfg;
        CHECK(cfg.from_string(mk_yaml));
        const policy_result r = boot(cfg, mk);
        CHECK(r.mode == policy_mode::user_tuned && r.migrated_changes == 0);
        CHECK(cfg.video.shadermode.get() == shader_mode::async_recompiler && cfg.video.relaxed_zcull_sync.get());
        CHECK(!contains(g_checkpoints, "profile: '"));
    }
    // Values the player chose away from the old forced ones stay, without a stamp too.
    fs::test_files()[rpcs3::utils::get_custom_config_path(mk)] = mk_yaml;
    {
        cfg_root cfg;
        CHECK(cfg.from_string(mk_yaml));
        cfg.video.shadermode.set(shader_mode::recompiler);
        cfg.video.relaxed_zcull_sync.set(false);
        cfg.video.precise_zpass_count.set(true);
        const policy_result r = boot(cfg, mk);
        CHECK(r.migrated_changes == 0 && cfg.video.shadermode.get() == shader_mode::recompiler);
    }
    // OpenGL: the old profile never forced the asynchronous mode there, so it does not migrate (0.40: without
    // gl-async.flag the platform step runs the legacy recompiler, as the renderer would at start); 0.40: the strict
    // pair is the OpenGL value again (0.38), so the old relaxed pair migrates as on Vulkan, also with
    // zcull-strict-gl.flag; with gl-relaxed-zcull.flag (the 0.39 behaviour) the relaxed pair is the OpenGL value and
    // nothing migrates.
    for (const char* file : {static_cast<const char*>(nullptr), "zcull-strict-gl.flag"})
    {
        if (file) safe_file(file, true);
        cfg_root cfg;
        CHECK(cfg.from_string(mk_yaml));
        cfg.video.renderer.set(video_renderer::opengl);
        const policy_result r = boot(cfg, mk);
        CHECK(r.migrated_changes == 2 && cfg.video.shadermode.get() == shader_mode::recompiler);
        CHECK(!cfg.video.relaxed_zcull_sync.get() && cfg.video.precise_zpass_count.get());
        if (file) safe_file(file, false);
    }
    safe_file("gl-relaxed-zcull.flag", true);
    fs::test_files()[gl_async_flag] = "";
    {
        cfg_root cfg;
        CHECK(cfg.from_string(mk_yaml));
        cfg.video.renderer.set(video_renderer::opengl);
        const policy_result r = boot(cfg, mk);
        CHECK(r.migrated_changes == 0 && cfg.video.shadermode.get() == shader_mode::async_recompiler);
        CHECK(cfg.video.relaxed_zcull_sync.get() && !cfg.video.precise_zpass_count.get());
    }
    fs::test_files().erase(gl_async_flag);
    safe_file("gl-relaxed-zcull.flag", false);
    // The escape files: their value is the current profile value, so nothing migrates for them.
    fs::test_files()[zcull_relaxed_flag] = "";
    {
        cfg_root cfg;
        CHECK(cfg.from_string(mk_yaml));
        const policy_result r = boot(cfg, mk);
        CHECK(r.migrated_changes == 1 && cfg.video.relaxed_zcull_sync.get() && !cfg.video.precise_zpass_count.get());
        CHECK(cfg.video.shadermode.get() == shader_mode::async_with_interpreter);
    }
    fs::test_files()[shader_skip_draws_flag] = "";
    {
        cfg_root cfg;
        CHECK(cfg.from_string(mk_yaml));
        CHECK(boot(cfg, mk).migrated_changes == 0);
    }
    fs::test_files().erase(zcull_relaxed_flag);
    fs::test_files().erase(shader_skip_draws_flag);
    // The settings menu's preview migrates too (it saves the current values), without startup.log lines.
    {
        g_checkpoints.clear();
        cfg_root cfg;
        CHECK(cfg.from_string(mk_yaml));
        std::vector<change_kind> kinds;
        const policy_result r = apply_boot_policy_preview(cfg, mk, [&](const cfg::_base&, const std::string&, std::string_view, change_kind kind)
        { if (kind == change_kind::migration) kinds.push_back(kind); });
        CHECK(kinds.size() == 3 && r.migrated_changes == 3 && g_checkpoints.empty());
        CHECK(!cfg.video.relaxed_zcull_sync.get());
    }
    // A marker without any saved YAML: nothing to migrate.
    fs::test_files().erase(rpcs3::utils::get_custom_config_path(mk));
    {
        cfg_root cfg;
        CHECK(cfg.from_string(mk_yaml));
        CHECK(boot(cfg, mk).migrated_changes == 0 && cfg.video.relaxed_zcull_sync.get());
    }
    // keep-user-settings.flag: nothing.
    fs::test_files()[rpcs3::utils::get_custom_config_path(mk)] = mk_yaml;
    fs::test_files()[keep_user_settings_flag] = "";
    {
        cfg_root cfg;
        CHECK(cfg.from_string(mk_yaml));
        const policy_result r = boot(cfg, mk);
        CHECK(r.mode == policy_mode::keep_user_settings && r.migrated_changes == 0 && cfg.video.relaxed_zcull_sync.get());
    }
    fs::test_files().erase(keep_user_settings_flag);

    // Demon's Souls saved in 0.31-0.36 with Write Color Buffers on (its automatic row) and the old profile values: once
    // those are migrated, the save runs exactly like the automatic configuration (stale marker).
    reset_state();
    const std::string ds_yaml = saved_by_036(demons_souls, false);
    reset_state();
    CHECK(ds_yaml.find("Write Color Buffers: true") != std::string::npos);
    CHECK(set_user_tuned(demons_souls, true));
    fs::test_files()[rpcs3::utils::get_custom_config_path(demons_souls)] = ds_yaml;
    {
        cfg_root cfg;
        CHECK(cfg.from_string(ds_yaml));
        const policy_result r = boot(cfg, demons_souls);
        CHECK(r.mode == policy_mode::automatic && r.stale_marker && r.migrated_changes == 3);
        CHECK(cfg.video.write_color_buffers.get() && !cfg.video.relaxed_zcull_sync.get());
    }

    // God of War: Ascension saved in 0.33-0.36 with its rows' values: 'Accurate ZCULL stats: false' is what its row and
    // its PC entry ask, not only the old profile's value, so it stays; the save then runs like the automatic
    // configuration (stale marker). Without and with the database.
    for (const bool database : {false, true})
    {
        reset_state();
        if (database) write_database({{gow_ascension, gow_ascension_entry}});
        std::string gow_yaml;
        {
            cfg_root cfg;
            boot(cfg, gow_ascension);
            cfg.video.shadermode.set(shader_mode::async_recompiler);
            gow_yaml = cfg.to_string();
        }
        g_checkpoints.clear();
        CHECK(set_user_tuned(gow_ascension, true));
        fs::test_files()[rpcs3::utils::get_custom_config_path(gow_ascension)] = gow_yaml;
        cfg_root cfg;
        CHECK(cfg.from_string(gow_yaml));
        const policy_result r = boot(cfg, gow_ascension);
        CHECK(r.mode == policy_mode::automatic && r.stale_marker && r.migrated_changes == 1);
        CHECK(!cfg.video.precise_zpass_count.get() && !cfg.video.relaxed_zcull_sync.get() && cfg.video.write_color_buffers.get());
        CHECK(cfg.video.shadermode.get() == shader_mode::async_with_interpreter);
        CHECK(!contains(g_checkpoints, "profile: 'Accurate ZCULL stats'"));
    }

    // Automatic configuration off: the global ps4-settings.yml decides for a game without its own YAML.
    reset_state();
    const std::string global_yaml = saved_by_036(terraria, false);
    reset_state();
    CHECK(set_auto_config_enabled(false));
    fs::test_files()[global_settings_path] = global_yaml;
    {
        cfg_root cfg;
        CHECK(cfg.from_string(global_yaml));
        const policy_result r = boot(cfg, terraria);
        CHECK(r.mode == policy_mode::auto_config_off && r.migrated_changes == 3);
        CHECK(cfg.video.shadermode.get() == shader_mode::async_with_interpreter && !cfg.video.relaxed_zcull_sync.get());
        CHECK(contains(g_checkpoints, "profile: 'Relaxed ZCULL Sync' saved by automatic profile <=36 ('true') -> 40 ('false')"));
    }
    {
        g_checkpoints.clear();
        cfg_root cfg; // the global pass (Emu.Init) migrates as well, without startup.log lines
        CHECK(cfg.from_string(global_yaml));
        CHECK(boot(cfg, "").migrated_changes == 3 && g_checkpoints.empty());
    }
    // A game with its own stamped YAML: that file decides.
    fs::test_files()[rpcs3::utils::get_custom_config_path(terraria)] = with_profile_stamp(global_yaml);
    {
        cfg_root cfg;
        CHECK(cfg.from_string(global_yaml));
        CHECK(boot(cfg, terraria).migrated_changes == 0 && cfg.video.relaxed_zcull_sync.get());
    }
    reset_state();
}

// 0.37: 'Disable SPU GETLLAR Spin Optimization' (22 PC database entries) is a console limit.
void test_getllar_spin_limit()
{
    constexpr std::string_view line = "console limit: 'Disable SPU GETLLAR Spin Optimization' = 'false' (was 'true'): unsafe on PS4: "
        "pure GETLLAR busy-waiting starves Orbis' fixed-priority cores";
    reset_state();
    write_database({{minecraft, "Core:\n  Disable SPU GETLLAR Spin Optimization: true\nVideo:\n  Write Color Buffers: true"}});
    {
        cfg_root cfg;
        std::vector<change> changes;
        const policy_result r = boot(cfg, minecraft, &changes);
        CHECK(r.database.use == database_use::applied && cfg.video.write_color_buffers.get());
        CHECK(!cfg.core.spu_getllar_spin_optimization_disabled.get());
        std::vector<change_kind> kinds;
        for (const change& c : changes)
            if (c.name == "Disable SPU GETLLAR Spin Optimization") kinds.push_back(c.kind);
        CHECK(kinds.size() == 2 && kinds[0] == change_kind::title && kinds[1] == change_kind::console);
        CHECK(r.console_changes == 1);
        CHECK(contains(g_checkpoints, line));
    }
    for (const int mode : {1, 2}) // a saved YAML: user-tuned, automatic configuration off
    {
        reset_state();
        if (mode == 1) CHECK(set_user_tuned(terraria, true));
        if (mode == 2) CHECK(set_auto_config_enabled(false));
        cfg_root cfg;
        cfg.core.max_spurs_threads.set(4);
        cfg.core.spu_getllar_spin_optimization_disabled.set(true);
        std::vector<change> changes;
        boot(cfg, terraria, &changes);
        CHECK(!cfg.core.spu_getllar_spin_optimization_disabled.get());
        const change* c = find_named(changes, "Disable SPU GETLLAR Spin Optimization");
        CHECK(c && c->kind == change_kind::console && !c->why.empty());
        CHECK(contains(g_checkpoints, line));
    }
    {
        reset_state();
        fs::test_files()[keep_user_settings_flag] = "";
        cfg_root cfg;
        cfg.core.spu_getllar_spin_optimization_disabled.set(true);
        boot(cfg, minecraft);
        CHECK(cfg.core.spu_getllar_spin_optimization_disabled.get());
    }
    reset_state();
    {
        cfg_root cfg; // the menu preview: same value, no startup.log line
        cfg.core.spu_getllar_spin_optimization_disabled.set(true);
        apply_boot_policy_preview(cfg, minecraft, [](const cfg::_base&, const std::string&, std::string_view) {});
        CHECK(!cfg.core.spu_getllar_spin_optimization_disabled.get() && g_checkpoints.empty());
        const unsafe_option* u = unsafe_option_for(cfg, cfg.core.spu_getllar_spin_optimization_disabled);
        CHECK(u && u->value == "false" && !u->why.empty() && std::string_view(i18n::tr(u->menu)) == "espera ativa pura do SPU trava o PS4");
    }
    {
        cfg_root cfg; // nothing to say when the option is off already
        boot(cfg, minecraft);
        CHECK(!contains(g_checkpoints, "console limit:"));
    }
}

// 0.37: the config database the package carries (/app0/config_database.dat) and its precedence against /data.
void test_bundled_database()
{
    using config_db::detail::game_map;
    game_map games;
    std::string error;
    long long stamp = 0;
    CHECK(config_db::detail::parse(database_text({{minecraft, "Video:\n  Write Color Buffers: true"}}), games, error, &stamp) && stamp == 1784200630);
    CHECK(config_db::detail::parse(database_text({{minecraft, "x"}}, -1), games, error, &stamp) && stamp == -1);
    CHECK(config_db::detail::parse("{\"return_code\": 0, \"timestamp\": 1.5, \"games\": {}}", games, error, &stamp) && stamp == -1);
    CHECK(config_db::detail::parse("{\"timestamp\": -5, \"return_code\": 0, \"games\": {}}", games, error, &stamp) && stamp == -1);
    CHECK(config_db::detail::parse("{\"timestamp\": \"x\", \"return_code\": 0, \"games\": {}}", games, error, &stamp) && stamp == -1);
    stamp = 0;
    CHECK(!config_db::detail::parse("{\"return_code\": -1, \"timestamp\": 5, \"games\": {}}", games, error, &stamp) && stamp == -1);
    CHECK(config_db::detail::parse("{\"return_code\": 0, \"games\": {}}", games, error)); // the 0.33 form

    const std::string bundled = config_db::bundled_path;
    CHECK(bundled == "/app0/config_database.dat");
    const std::string gow_and_minecraft = database_text({{gow_ascension, gow_ascension_entry}, {minecraft, "Video:\n  Write Color Buffers: true"}});

    // Only the package's copy.
    reset_state();
    fs::test_files()[bundled] = gow_and_minecraft;
    {
        const config_db::file_info info = config_db::status();
        CHECK(info.state == config_db::file_state::loaded && info.source == config_db::file_source::bundled && info.titles == 2);
        CHECK(info.timestamp == 1784200630 && info.data.state == config_db::file_state::absent && info.bundled.state == config_db::file_state::loaded);
        cfg_root cfg;
        std::vector<change> changes;
        const policy_result r = boot(cfg, gow_ascension, &changes);
        CHECK(r.database.use == database_use::applied && r.database.changes == 3);
        const change* c = find_named(changes, "Write Color Buffers");
        CHECK(c && is_database_reason(c->why)); // the entry, not the fallback row
        CHECK(contains(g_checkpoints, "config database: bundled /app0/config_database.dat (timestamp 1784200630, 2 titles); /data copy absent"));
        CHECK(contains(g_checkpoints, "Relaxed ZCULL Sync=false · 5 keys, 3 value(s) changed"));
        CHECK(!contains(g_checkpoints, "not present"));
    }
    {
        cfg_root cfg; // a serial the bundle lacks: its fallback row
        const policy_result r = boot(cfg, gow_ascension_psn_eu);
        CHECK(r.database.use == database_use::none && cfg.video.write_color_buffers.get() && !cfg.video.precise_zpass_count.get());
    }

    // An older /data copy: the bundled one runs.
    fs::test_files()[config_db::path] = database_text({{minecraft, "Video:\n  Strict Rendering Mode: true"}}, 1700000000);
    {
        g_checkpoints.clear();
        const config_db::file_info info = config_db::status();
        CHECK(info.source == config_db::file_source::bundled && info.data.state == config_db::file_state::loaded && info.data.timestamp == 1700000000);
        cfg_root cfg;
        boot(cfg, minecraft);
        CHECK(cfg.video.write_color_buffers.get() && !cfg.video.strict_rendering_mode.get());
        CHECK(contains(g_checkpoints, "config database: bundled /app0/config_database.dat (timestamp 1784200630, 2 titles); /data copy older (timestamp 1700000000)"));
    }
    // A /data copy as new as the bundle (an import): it runs.
    for (const long long t : {1784200630LL, 1790000000LL})
    {
        g_checkpoints.clear();
        fs::test_files()[config_db::path] = database_text({{minecraft, "Video:\n  Strict Rendering Mode: true"}}, t);
        const config_db::file_info info = config_db::status();
        CHECK(info.source == config_db::file_source::data && info.titles == 1 && info.timestamp == t);
        cfg_root cfg;
        boot(cfg, minecraft);
        CHECK(!cfg.video.write_color_buffers.get() && cfg.video.strict_rendering_mode.get());
        CHECK(contains(g_checkpoints, "config database: /data/rpcs3/config_database.dat (timestamp " + std::to_string(t) +
            ", 1 titles); bundled copy not newer (timestamp 1784200630)"));
        CHECK(!config_db::has_entry(gow_ascension)); // the copy in use lacks it
    }
    // A /data copy without a timestamp cannot be compared: the user's copy runs.
    fs::test_files()[config_db::path] = database_text({{minecraft, "Video:\n  Strict Rendering Mode: true"}}, -1);
    {
        g_checkpoints.clear();
        CHECK(config_db::status().source == config_db::file_source::data);
        cfg_root cfg;
        boot(cfg, minecraft);
        CHECK(contains(g_checkpoints, "config database: /data/rpcs3/config_database.dat (no timestamp, 1 titles); bundled copy not used (timestamp 1784200630"));
    }
    // An unusable /data copy: the bundled one runs.
    fs::test_files()[config_db::path] = "<html>not a database</html>";
    {
        g_checkpoints.clear();
        const config_db::file_info info = config_db::status();
        CHECK(info.state == config_db::file_state::loaded && info.source == config_db::file_source::bundled);
        CHECK(info.data.state == config_db::file_state::invalid && !info.data.error.empty());
        cfg_root cfg;
        boot(cfg, minecraft);
        CHECK(cfg.video.write_color_buffers.get());
        CHECK(contains(g_checkpoints, "config database: bundled /app0/config_database.dat (timestamp 1784200630, 2 titles); /data copy not used: "));
    }
    // An unusable bundle and a usable /data copy.
    write_database({{minecraft, "Video:\n  Strict Rendering Mode: true"}});
    fs::test_files()[bundled] = "{\"return_code\": -1}";
    {
        g_checkpoints.clear();
        CHECK(config_db::status().source == config_db::file_source::data);
        cfg_root cfg;
        boot(cfg, minecraft);
        CHECK(cfg.video.strict_rendering_mode.get());
        CHECK(contains(g_checkpoints, "config database: /data/rpcs3/config_database.dat (timestamp 1784200630, 1 titles); bundled copy not used: "));
    }
    // Nothing usable.
    fs::test_files().erase(config_db::path);
    {
        g_checkpoints.clear();
        const config_db::file_info info = config_db::status();
        CHECK(info.state == config_db::file_state::invalid && info.source == config_db::file_source::bundled && !info.error.empty());
        cfg_root cfg;
        const policy_result r = boot(cfg, gow_ascension);
        CHECK(r.database.use == database_use::none && cfg.video.write_color_buffers.get()); // the row
        CHECK(contains(g_checkpoints, "config database: bundled /app0/config_database.dat not used: "));
        CHECK(contains(g_checkpoints, "; /data copy absent"));
        CHECK(!contains(g_checkpoints, "config database: applied") && !contains(g_checkpoints, "has no entry"));
    }
    fs::test_files()[config_db::path] = "<html>";
    {
        g_checkpoints.clear();
        CHECK(config_db::status().source == config_db::file_source::data);
        cfg_root cfg;
        boot(cfg, minecraft);
        CHECK(contains(g_checkpoints, "config database: /data/rpcs3/config_database.dat not used: "));
        CHECK(contains(g_checkpoints, "; bundled copy not used: return_code -1"));
    }
    // A rejected entry is forgotten when the copy in use changes.
    reset_state();
    fs::test_files()[bundled] = database_text({{minecraft, "Video:\n  Some Future Option: true"}});
    {
        cfg_root cfg;
        CHECK(boot(cfg, minecraft).database.use == database_use::rejected && config_db::lookup(minecraft).rejected);
    }
    fs::test_files()[bundled] = database_text({{minecraft, "Video:\n  Write Color Buffers: true"}});
    CHECK(config_db::has_entry(minecraft) && !config_db::lookup(minecraft).rejected);
    // No copy at all: the 0.36 line.
    reset_state();
    {
        cfg_root cfg;
        boot(cfg, minecraft);
        CHECK(contains(g_checkpoints, "config database: /data/rpcs3/config_database.dat not present (PC per-game settings not used"));
        CHECK(config_db::status().source == config_db::file_source::data && config_db::status().bundled.state == config_db::file_state::absent);
    }
}

// 0.37: the keys of a database entry, as the applied line counts them.
void test_entry_keys()
{
    using policy_detail::entry_keys;
    CHECK(entry_keys("") == 0);
    CHECK(entry_keys(gow_ascension_entry) == 5);
    CHECK(entry_keys(gow_3_entry) == 3);
    CHECK(entry_keys("Core:\r\n  Libraries Control:\r\n    - libvdec.sprx:lle\r\n    - libfs.sprx:lle\r\nVideo:\r\n  Write Color Buffers: true # PC\r\n") == 2);
    CHECK(entry_keys("Core:\n  Libraries Control: []\n# comment: x\n") == 1);
    CHECK(entry_keys("Video:\n  Vulkan:\n") == 0);
}
} // namespace

void ps4_notify(const char* message) noexcept
{
    g_toasts.emplace_back(message ? message : "");
}

void ps4_startup_checkpoint(const char* stage)
{
    g_checkpoints.emplace_back(stage ? stage : "");
}

// 0.39: the OpenGL ZCULL pair and gl-async.flag, the interpreter warning and the game language row.
// 0.40: the audio reserve is 34 ms again unless audio_reserve_64 (audio-reserve-64.flag, features.txt); a title at a
// safe-mode level and the global safe mode run 34 ms whatever the opt-in says. Profile 40 migrates the 64 ms that
// profile 39 saved back to 34 ms, never a pinned value; with the opt-in, the 0.39 migration of 34 ms saves runs.
void test_audio_reserve_040()
{
    reset_state();
    const auto reserve = [](const char* title)
    {
        cfg_root cfg;
        boot(cfg, title);
        return cfg.audio.desired_buffer_duration.get();
    };
    {
        cfg_root cfg; // the profile: 34 ms, the upstream value of 0.29-0.38
        std::vector<change> changes;
        boot(cfg, minecraft, &changes);
        CHECK(cfg.audio.desired_buffer_duration.get() == 34);
        const change* c = find_named(changes, "Desired Audio Buffer Duration");
        CHECK(c && c->kind == change_kind::profile && c->old == "100" && c->now == "34");
    }
    safe_file("audio-reserve-64.flag", true);
    CHECK(reserve(minecraft) == 64);
    // A safe-mode level of the title, or the global safe mode, turns the opt-in off
    CHECK(adapt::append(minecraft, adapt::key_fail_crash) && adapt::append(minecraft, adapt::key_safe1));
    CHECK(reserve(minecraft) == 34 && reserve(terraria) == 64);
    adapt::remove(minecraft);
    safe_file("safe-mode.flag", true);
    CHECK(reserve(minecraft) == 34);
    safe_file("safe-mode.flag", false);
    safe_file("audio-reserve-64.flag", false);
    for (const char* text : {"audio_reserve_64\n", "# test\r\nAudio-Reserve-64\r\n", "all\n", "audio-reserve-64.flag"})
    {
        safe_file("features.txt", true, text);
        CHECK(reserve(minecraft) == 64);
    }
    safe_file("features.txt", true, "audio_continuity\n");
    CHECK(reserve(minecraft) == 34);
    safe_file("features.txt", false);

    // A save from 0.29-0.38 (unstamped or stamped 37) with the 34 ms those profiles forced: runs as saved (0.38); with
    // the opt-in, 64 ms as in 0.39, and a later save keeps the player's 34 ms.
    for (const bool opt_in : {false, true})
        for (const bool stamped : {false, true})
        {
            reset_state();
            if (opt_in) safe_file("audio-reserve-64.flag", true);
            cfg_root saved;
            boot(saved, terraria);
            saved.audio.desired_buffer_duration.set(34);
            saved.video.write_color_buffers.set(true); // the player's choice keeps the marker meaningful
            std::string yaml = saved.to_string();
            if (stamped) yaml += "PS4 automatic profile: 37\n";
            CHECK(set_user_tuned(terraria, true));
            fs::test_files()[rpcs3::utils::get_custom_config_path(terraria)] = yaml;
            cfg_root cfg;
            CHECK(cfg.from_string(yaml));
            const policy_result r = boot(cfg, terraria);
            CHECK(r.mode == policy_mode::user_tuned && r.migrated_changes == (opt_in ? 1u : 0u));
            CHECK(cfg.audio.desired_buffer_duration.get() == (opt_in ? 64 : 34));
            CHECK(contains(g_checkpoints, "profile: 'Desired Audio Buffer Duration' saved by automatic profile <=37 ('34') -> 40 ('64')") == opt_in);
            fs::test_files()[rpcs3::utils::get_custom_config_path(terraria)] = with_profile_stamp(saved.to_string());
            cfg_root again;
            CHECK(again.from_string(saved.to_string()));
            CHECK(boot(again, terraria).migrated_changes == 0 && again.audio.desired_buffer_duration.get() == 34);
        }

    // Profile 40: a YAML stamped 39 holding the 64 ms that profile forced goes back to 34 ms; stamped 40 (a 0.40 save) it
    // is the player's value; with the opt-in it stays.
    for (const unsigned stamp : {39u, 40u})
        for (const bool opt_in : {false, true})
        {
            reset_state();
            if (opt_in) safe_file("audio-reserve-64.flag", true);
            cfg_root saved;
            boot(saved, terraria);
            saved.audio.desired_buffer_duration.set(64);
            saved.video.write_color_buffers.set(true);
            const std::string yaml = saved.to_string() + "PS4 automatic profile: " + std::to_string(stamp) + "\n";
            CHECK(set_user_tuned(terraria, true));
            fs::test_files()[rpcs3::utils::get_custom_config_path(terraria)] = yaml;
            cfg_root cfg;
            CHECK(cfg.from_string(yaml));
            const policy_result r = boot(cfg, terraria);
            const bool back = stamp == 39 && !opt_in;
            CHECK(r.mode == policy_mode::user_tuned && r.migrated_changes == (back ? 1u : 0u));
            CHECK(cfg.audio.desired_buffer_duration.get() == (back ? 34 : 64));
            CHECK(contains(g_checkpoints, "profile: 'Desired Audio Buffer Duration' saved by automatic profile 39 ('64') -> 40 ('34')") == back);
        }

    // Never a pinned value: the global ps4-settings.yml of format 2 stamped 39 that pins the reserve keeps its 64 ms
    // (automatic configuration off, a game without its own YAML); the same file without the pin migrates.
    for (const bool pinned : {true, false})
    {
        reset_state();
        cfg_root saved;
        boot(saved, "");
        saved.audio.desired_buffer_duration.set(64);
        std::string yaml = policy_detail::strip_ps4_keys(saved.to_string());
        yaml += "PS4 automatic profile: 39\nPS4 settings format: 2\n";
        yaml += pinned ? "PS4 user settings:\n  - \"Audio > Desired Audio Buffer Duration\"\n" : "PS4 user settings: []\n";
        CHECK(set_auto_config_enabled(false));
        fs::test_files()[global_settings_path] = yaml;
        cfg_root cfg;
        CHECK(cfg.from_string(yaml));
        const policy_result r = boot(cfg, terraria);
        CHECK(r.mode == policy_mode::auto_config_off && r.migrated_changes == (pinned ? 0u : 1u));
        CHECK(cfg.audio.desired_buffer_duration.get() == (pinned ? 64 : 34));
    }
    reset_state();
}

// 0.40: OpenGL runs the strict ZCULL pair of 0.38 unless gl_relaxed_zcull (gl-relaxed-zcull.flag; zcull-strict-gl.flag
// is its kill flag); profile 40 puts the relaxed pair profile 39 saved into OpenGL YAMLs back to strict.
void test_opengl_profile_040()
{
    reset_state();
    {
        cfg_root cfg; // Ben 10's OpenGL row: the strict pair (0.38), the legacy recompiler of its row
        std::vector<change> changes;
        boot(cfg, ben10, &changes);
        CHECK(cfg.video.renderer.get() == video_renderer::opengl && cfg.video.shadermode.get() == shader_mode::recompiler);
        CHECK(!cfg.video.relaxed_zcull_sync.get() && cfg.video.precise_zpass_count.get());
        CHECK(!find_named(changes, "Relaxed ZCULL Sync"));
        CHECK(contains(g_checkpoints, " · OpenGL ZCULL strict · ") && !contains(g_checkpoints, "(gl_relaxed_zcull)"));
    }
    safe_file("gl-relaxed-zcull.flag", true);
    {
        g_checkpoints.clear();
        cfg_root cfg; // the 0.39 behaviour: the relaxed pair
        std::vector<change> changes;
        boot(cfg, ben10, &changes);
        CHECK(cfg.video.relaxed_zcull_sync.get() && !cfg.video.precise_zpass_count.get());
        const change* c = find_named(changes, "Relaxed ZCULL Sync");
        CHECK(c && c->kind == change_kind::profile);
        CHECK(contains(g_checkpoints, " · OpenGL ZCULL relaxed (gl_relaxed_zcull)"));
    }
    {
        g_checkpoints.clear();
        cfg_root cfg; // Vulkan keeps the strict pair
        boot(cfg, minecraft);
        CHECK(!cfg.video.relaxed_zcull_sync.get() && cfg.video.precise_zpass_count.get());
        CHECK(!contains(g_checkpoints, "OpenGL ZCULL"));
    }
    safe_file("zcull-strict-gl.flag", true);
    {
        g_checkpoints.clear();
        cfg_root cfg; // the kill flag wins over the opt-in
        boot(cfg, ben10);
        CHECK(!cfg.video.relaxed_zcull_sync.get() && cfg.video.precise_zpass_count.get());
        CHECK(contains(g_checkpoints, " · OpenGL ZCULL strict (zcull-strict-gl.flag)"));
    }
    reset_state();
    // A database entry that selects OpenGL: with the opt-in, the relaxed OpenGL pair for each ZCULL setting the entry
    // does not write; without it, the strict pair
    for (const bool opt_in : {false, true})
    {
        reset_state();
        if (opt_in) safe_file("gl-relaxed-zcull.flag", true);
        write_database({{minecraft, "Video:\n  Renderer: OpenGL"}, {terraria, "Video:\n  Renderer: OpenGL\n  Relaxed ZCULL Sync: false"}});
        {
            cfg_root cfg;
            std::vector<change> changes;
            boot(cfg, minecraft, &changes);
            CHECK(cfg.video.renderer.get() == video_renderer::opengl && cfg.video.shadermode.get() == shader_mode::recompiler);
            CHECK(cfg.video.relaxed_zcull_sync.get() == opt_in && cfg.video.precise_zpass_count.get() == !opt_in);
            const change* c = nullptr;
            for (const auto& item : changes)
                if (item.name == "Relaxed ZCULL Sync") c = &item;
            CHECK(opt_in ? (c && is_database_reason(c->why) && c->kind == change_kind::title) : !c);
        }
        {
            cfg_root cfg;
            boot(cfg, terraria);
            CHECK(!cfg.video.relaxed_zcull_sync.get() && cfg.video.precise_zpass_count.get() == !opt_in);
        }
    }
    // gl-async.flag: such an entry gets the asynchronous recompiler
    fs::test_files()[gl_async_flag] = "";
    {
        cfg_root cfg;
        std::vector<change> changes;
        boot(cfg, minecraft, &changes);
        CHECK(cfg.video.shadermode.get() == shader_mode::async_recompiler);
        const change* c = find_named(changes, "Shader Mode");
        CHECK(c && c->why.find("gl-async.flag") != std::string::npos);
        CHECK(profile_opengl_shader_mode() == shader_mode::async_recompiler);
    }
    fs::test_files().erase(gl_async_flag);
    CHECK(profile_opengl_shader_mode() == shader_mode::recompiler);
    CHECK(profile_zcull_relaxed(video_renderer::opengl) && !profile_zcull_relaxed(video_renderer::vulkan)); // the opt-in is on
    safe_file("gl-relaxed-zcull.flag", false);
    CHECK(!profile_zcull_relaxed(video_renderer::opengl) && !profile_zcull_relaxed(video_renderer::vulkan));
    CHECK(profile_zcull_relaxed(video_renderer::opengl, safe_mode::bit(safe_mode::feature::gl_relaxed_zcull)));
    // An OpenGL save that profile 37 stamped with the strict pair it forced: runs as saved (0.38); with the opt-in, the
    // relaxed OpenGL pair (0.39); a Vulkan one keeps it; the kill flag keeps it too.
    for (const bool opt_in : {false, true})
        for (const bool gl : {true, false})
        {
            reset_state();
            if (opt_in) safe_file("gl-relaxed-zcull.flag", true);
            cfg_root saved;
            boot(saved, terraria);
            saved.video.renderer.set(gl ? video_renderer::opengl : video_renderer::vulkan);
            saved.video.relaxed_zcull_sync.set(false);
            saved.video.precise_zpass_count.set(true);
            const std::string yaml = saved.to_string() + "PS4 automatic profile: 37\n";
            CHECK(set_user_tuned(terraria, true));
            fs::test_files()[rpcs3::utils::get_custom_config_path(terraria)] = yaml;
            cfg_root cfg;
            CHECK(cfg.from_string(yaml));
            const policy_result r = boot(cfg, terraria);
            const bool relax = gl && opt_in;
            CHECK(r.migrated_changes == (relax ? 2u : 0u));
            CHECK(cfg.video.relaxed_zcull_sync.get() == relax && cfg.video.precise_zpass_count.get() == !relax);
            CHECK(contains(g_checkpoints, "profile: 'Relaxed ZCULL Sync' saved by automatic profile <=37 ('false') -> 40 OpenGL ('true')") == relax);
            safe_file("zcull-strict-gl.flag", true);
            cfg_root strict;
            CHECK(strict.from_string(yaml));
            CHECK(boot(strict, terraria).migrated_changes == 0 && !strict.video.relaxed_zcull_sync.get());
            safe_file("zcull-strict-gl.flag", false);
        }
    // Profile 40: the relaxed pair profile 39 forced into an OpenGL save goes back to strict; a Vulkan save stamped 39
    // with that pair is the player's choice, and the opt-in keeps the OpenGL one.
    for (const bool opt_in : {false, true})
        for (const bool gl : {true, false})
        {
            reset_state();
            if (opt_in) safe_file("gl-relaxed-zcull.flag", true);
            cfg_root saved;
            boot(saved, terraria);
            saved.video.renderer.set(gl ? video_renderer::opengl : video_renderer::vulkan);
            saved.video.relaxed_zcull_sync.set(true);
            saved.video.precise_zpass_count.set(false);
            const std::string yaml = saved.to_string() + "PS4 automatic profile: 39\n";
            CHECK(set_user_tuned(terraria, true));
            fs::test_files()[rpcs3::utils::get_custom_config_path(terraria)] = yaml;
            cfg_root cfg;
            CHECK(cfg.from_string(yaml));
            const policy_result r = boot(cfg, terraria);
            const bool back = gl && !opt_in;
            CHECK(r.migrated_changes == (back ? 2u : 0u));
            CHECK(cfg.video.relaxed_zcull_sync.get() == !back && cfg.video.precise_zpass_count.get() == back);
            CHECK(contains(g_checkpoints, "profile: 'Relaxed ZCULL Sync' saved by automatic profile 39 ('true') -> 40 ('false')") == back);
            CHECK(contains(g_checkpoints, "profile: 'Accurate ZCULL stats' saved by automatic profile 39 ('false') -> 40 ('true')") == back);
        }
    reset_state();
}

void test_interpreter_warning_039()
{
    constexpr std::string_view ppu_line = "Política PS4 para BLUS31029: PPU decoder = interpreter (user setting): 10-50x slower than LLVM on this CPU";
    constexpr char pes[] = "BLUS31029";
    reset_state();
    {
        cfg_root saved;
        boot(saved, pes);
        saved.core.ppu_decoder.set(ppu_decoder_type::_static);
        // (this host build has no LLVM, so the profile leaves the decoder alone: a second choice keeps the save manual)
        saved.video.write_color_buffers.set(true);
        CHECK(set_user_tuned(pes, true));
        fs::test_files()[rpcs3::utils::get_custom_config_path(pes)] = with_profile_stamp(saved.to_string());
        g_checkpoints.clear();
        g_toasts.clear();
        cfg_root cfg;
        CHECK(cfg.from_string(saved.to_string()));
        const policy_result r = boot(cfg, pes);
        CHECK(r.mode == policy_mode::user_tuned && !r.ppu_decoder_from_database);
        CHECK(contains(g_checkpoints, ppu_line));
        CHECK(!contains(g_checkpoints, "SPU decoder = interpreter"));
        CHECK(g_toasts.size() == 1 && contains(g_toasts, "RPCS3: BLUS31029 usa o interpretador de PPU (escolhido por você): 10 a 50 vezes mais lento"));
        // A relaunch of the running game: the line again, no second toast
        CHECK(adapt::set_title(pes));
        g_toasts.clear();
        cfg_root relaunch;
        CHECK(relaunch.from_string(saved.to_string()));
        boot(relaunch, pes);
        CHECK(g_toasts.empty());
        adapt::clear_title();
        // Both decoders interpreted
        saved.core.spu_decoder.set(spu_decoder_type::dynamic);
        fs::test_files()[rpcs3::utils::get_custom_config_path(pes)] = with_profile_stamp(saved.to_string());
        g_toasts.clear();
        cfg_root both;
        CHECK(both.from_string(saved.to_string()));
        boot(both, pes);
        CHECK(contains(g_toasts, "RPCS3: BLUS31029 usa o interpretador de PPU e SPU (escolhido por você): 10 a 50 vezes mais lento"));
        // The preview (settings menu) neither logs nor toasts
        g_checkpoints.clear();
        g_toasts.clear();
        cfg_root preview;
        CHECK(preview.from_string(saved.to_string()));
        apply_boot_policy_preview(preview, pes, [](const cfg::_base&, const std::string&, std::string_view, change_kind) {});
        CHECK(g_checkpoints.empty() && g_toasts.empty());
    }
    // The title's PC database entry asks for the SPU interpreter (Sonic Generations): its source is named
    reset_state();
    write_database({{"BLUS30612", "Core:\n  SPU Decoder: Interpreter (static)"}});
    {
        cfg_root cfg;
        const policy_result r = boot(cfg, "BLUS30612");
        CHECK(r.spu_decoder_from_database && cfg.core.spu_decoder.get() == spu_decoder_type::_static);
        CHECK(contains(g_checkpoints, "Política PS4 para BLUS30612: SPU decoder = interpreter (PC database entry): 10-50x slower than LLVM on this CPU"));
        CHECK(contains(g_toasts, "RPCS3: BLUS30612 usa o interpretador de SPU (pedido pela base do PC): 10 a 50 vezes mais lento"));
    }
    // LLVM: nothing
    reset_state();
    {
        cfg_root cfg;
        boot(cfg, minecraft);
        CHECK(!contains(g_checkpoints, "decoder = interpreter") && g_toasts.empty());
    }
    reset_state();
}

void test_game_language_039()
{
    // The files
    CHECK(parse_game_language("auto") == game_language_auto);
    CHECK(parse_game_language(" 17\r\n") == 17);
    CHECK(parse_game_language("0") == 0 && parse_game_language("19") == 19);
    CHECK(!parse_game_language("20") && !parse_game_language("-1") && !parse_game_language("pt") && !parse_game_language("") && !parse_game_language("123"));
    reset_state();
    CHECK(read_game_language(minecraft).value == game_language_auto && read_game_language(minecraft).source == game_language_source::automatic);
    CHECK(set_game_language("", 2) && fs::test_files()[game_language_path] == "2\n");
    CHECK(read_game_language(minecraft).value == 2 && read_game_language(minecraft).source == game_language_source::global);
    CHECK(set_game_language(minecraft, game_language_auto) && fs::test_files()[game_language_marker(minecraft)] == "auto\n");
    CHECK(read_game_language(minecraft).value == game_language_auto && read_game_language(minecraft).source == game_language_source::game);
    CHECK(read_game_language(minecraft, true).value == 2);
    CHECK(set_game_language(minecraft, std::nullopt) && !fs::is_file(game_language_marker(minecraft)));
    CHECK(set_game_language("", game_language_auto) && !fs::is_file(game_language_path));
    CHECK(set_game_language(terraria, 17) && read_game_language(terraria).value == 17);

    // 0.40: 'ps4' (Idioma do PS4)
    CHECK(parse_game_language("ps4\n") == game_language_system && !parse_game_language("PS4"));
    CHECK(set_game_language("", game_language_system) && fs::test_files()[game_language_path] == "ps4\n");
    CHECK(read_game_language(minecraft).value == game_language_system && read_game_language(minecraft).source == game_language_source::global);
    CHECK(set_game_language(minecraft, game_language_system) && fs::test_files()[game_language_marker(minecraft)] == "ps4\n");
    CHECK(set_game_language("", game_language_auto) && !fs::is_file(game_language_path));

    // Automatic before the interface language is known: English (US), the upstream default (nothing changes)
    reset_state();
    {
        cfg_root cfg;
        std::vector<change> changes;
        boot(cfg, minecraft, &changes);
        CHECK(cfg.sys.language.get() == CellSysutilLang{1} && !find_named(changes, "Language"));
        CHECK(contains(g_checkpoints, " · idioma do jogo English (US) (auto)"));
    }
    // 0.40: a Portuguese (Brazil) PS4 with the interface automatic: 'Automático' is still English (US), as in 0.25-0.38
    i18n::apply(i18n::setting::automatic, 17);
    {
        cfg_root cfg;
        std::vector<change> changes;
        boot(cfg, minecraft, &changes);
        CHECK(cfg.sys.language.get() == CellSysutilLang{1} && !find_named(changes, "Language"));
        CHECK(contains(g_checkpoints, " · idioma do jogo English (US) (auto)"));
        CHECK(read_runtime_inputs(minecraft).game_language_auto == 1 && read_runtime_inputs(minecraft).game_language_system == 17);
    }
    // features.txt game_language_ps4: the 0.39 automatic value (the PS4 language), as a runtime change
    safe_file("features.txt", true, "game_language_ps4\n");
    {
        cfg_root cfg;
        std::vector<change> changes;
        const policy_result r = boot(cfg, minecraft, &changes);
        CHECK(cfg.sys.language.get() == CellSysutilLang{17});
        const change* c = find_named(changes, "Language");
        CHECK(c && c->kind == change_kind::runtime && c->old == "English (US)" && c->now == "Portuguese (Brazil)");
        CHECK(r.runtime_changes >= 1);
        CHECK(contains(g_checkpoints, " · idioma do jogo Portuguese (Brazil) (auto, PS4 17: game_language_ps4)"));
    }
    {
        cfg_root cfg; // a language written into the YAML by hand stays with the automatic value
        cfg.sys.language.set(CellSysutilLang{2});
        boot(cfg, minecraft);
        CHECK(cfg.sys.language.get() == CellSysutilLang{2});
        CHECK(contains(g_checkpoints, " · idioma do jogo French (YAML)"));
    }
    safe_file("features.txt", false);
    // The row value 'Idioma do PS4' (global or this game's): the PS4 language; it turns game_language_ps4 on for the
    // boot (the features line of set_for_boot names it); a safe-mode level, or the global safe mode, turns it off
    for (const bool global : {true, false})
    {
        CHECK(set_game_language(global ? std::string_view{} : std::string_view{minecraft}, game_language_system));
        cfg_root cfg;
        std::vector<change> changes;
        boot(cfg, minecraft, &changes);
        CHECK(cfg.sys.language.get() == CellSysutilLang{17});
        const change* c = find_named(changes, "Language");
        CHECK(c && c->why.find("Idioma do PS4") != std::string::npos);
        CHECK(contains(g_checkpoints, " · idioma do jogo Portuguese (Brazil) (Idioma do PS4, PS4 17)"));
        CHECK(safe_mode::detail::g_boot_enables.load() == safe_mode::bit(safe_mode::feature::game_language_ps4));
        // the preview leaves the boot's enables alone
        safe_mode::set_boot_enables(0);
        cfg_root preview;
        apply_boot_policy_preview(preview, minecraft, [](const cfg::_base&, const std::string&, std::string_view) {});
        CHECK(preview.sys.language.get() == CellSysutilLang{17} && safe_mode::detail::g_boot_enables.load() == 0);
        CHECK(set_game_language(global ? std::string_view{} : std::string_view{minecraft}, global ? std::optional<std::int32_t>(game_language_auto) : std::nullopt));
    }
    CHECK(set_game_language(minecraft, game_language_system));
    CHECK(adapt::append(minecraft, adapt::key_fail_gpu) && adapt::append(minecraft, adapt::key_safe1));
    {
        g_checkpoints.clear();
        cfg_root cfg;
        boot(cfg, minecraft);
        CHECK(cfg.sys.language.get() == CellSysutilLang{1});
        CHECK(contains(g_checkpoints, " · idioma do jogo English (US) (auto: Idioma do PS4 desligado pelo modo seguro)"));
        CHECK(!read_runtime_inputs(minecraft).game_language_system_on);
    }
    adapt::remove(minecraft);
    safe_file("safe-mode.flag", true);
    {
        cfg_root cfg;
        boot(cfg, minecraft);
        CHECK(cfg.sys.language.get() == CellSysutilLang{1});
    }
    safe_file("safe-mode.flag", false);
    CHECK(set_game_language(minecraft, std::nullopt));
    // The global choice wins over the YAML, the game's marker over the global choice
    CHECK(set_game_language("", 4));
    {
        cfg_root cfg;
        cfg.sys.language.set(CellSysutilLang{2});
        boot(cfg, minecraft);
        CHECK(cfg.sys.language.get() == CellSysutilLang{4});
        CHECK(contains(g_checkpoints, " · idioma do jogo German (Configurações)"));
    }
    CHECK(set_game_language(minecraft, 18));
    {
        cfg_root cfg;
        boot(cfg, minecraft);
        CHECK(cfg.sys.language.get() == CellSysutilLang{18});
        CHECK(contains(g_checkpoints, " · idioma do jogo English (UK) (este jogo)"));
    }
    CHECK(set_game_language(minecraft, game_language_auto));
    {
        cfg_root cfg; // the game's 'auto' over a global choice: English (US) (0.40)
        boot(cfg, minecraft);
        CHECK(cfg.sys.language.get() == CellSysutilLang{1});
    }
    // An interface language chosen explicitly decides the 0.39 automatic value (Spanish -> Spanish): 'Idioma do PS4'
    CHECK(set_game_language(minecraft, std::nullopt) && set_game_language("", game_language_system));
    i18n::apply(i18n::setting::es, 17);
    {
        cfg_root cfg;
        boot(cfg, minecraft);
        CHECK(cfg.sys.language.get() == CellSysutilLang{3});
    }
    CHECK(set_game_language("", game_language_auto));
    {
        cfg_root cfg;
        boot(cfg, minecraft);
        CHECK(cfg.sys.language.get() == CellSysutilLang{1});
    }
    // keep-user-settings.flag: nothing
    fs::test_files()[keep_user_settings_flag] = "";
    CHECK(set_game_language("", 4));
    {
        cfg_root cfg;
        boot(cfg, minecraft);
        CHECK(cfg.sys.language.get() == CellSysutilLang{1});
    }
    fs::test_files().erase(keep_user_settings_flag);
    // The global pass (Emu.Init) gets the global choice too
    {
        cfg_root cfg;
        boot(cfg, "");
        CHECK(cfg.sys.language.get() == CellSysutilLang{4});
    }
    i18n::apply(i18n::setting::automatic, -1);
    reset_state();
}

// The automatic configuration's boot toast (System.cpp): only when the title's own fixes (per-title rows, its PC
// database entry) change a value; never for the profile alone ("0 ajuste(s)"), the adaptation (it has its own toast),
// the global pass or the other modes.
void test_automatic_config_toast()
{
    reset_state();
    {
        cfg_root cfg; // the profile changes values, the title has no fix of its own
        const policy_result r = boot(cfg, minecraft);
        CHECK(r.mode == policy_mode::automatic && r.title_changes == 0);
        CHECK(automatic_config_toast(minecraft, r).empty());
        CHECK(g_toasts.empty());
    }
    {
        cfg_root cfg; // a per-title row (Demon's Souls: Write Color Buffers)
        const policy_result r = boot(cfg, demons_souls);
        CHECK(r.title_changes == 1);
        CHECK(automatic_config_toast(demons_souls, r) == "RPCS3: configuração automática ativa para BLUS30443 (1 ajuste(s) específico(s) para este jogo)");
        CHECK(automatic_config_toast("", r).empty()); // the global pass (Emu.Init) stays silent
    }
    // The adaptation alone has its own toast; with a row, only the row counts
    reset_state();
    CHECK(adapt::append(minecraft, adapt::key_gpu_hang));
    {
        cfg_root cfg;
        const policy_result r = boot(cfg, minecraft);
        CHECK(r.adapt_changes == 1 && r.title_changes == 1);
        CHECK(automatic_config_toast(minecraft, r).empty());
    }
    reset_state();
    CHECK(adapt::append(demons_souls, adapt::key_llvm_threads_1));
    {
        cfg_root cfg;
        const policy_result r = boot(cfg, demons_souls);
        CHECK(r.adapt_changes == 2 && r.title_changes == 3);
        CHECK(automatic_config_toast(demons_souls, r) == "RPCS3: configuração automática ativa para BLUS30443 (1 ajuste(s) específico(s) para este jogo)");
    }
    // The title's PC database entry
    reset_state();
    write_database({{minecraft, "Video:\n  Write Color Buffers: true\n  Strict Rendering Mode: true"}});
    {
        cfg_root cfg;
        const policy_result r = boot(cfg, minecraft);
        CHECK(r.database.use == database_use::applied && r.title_changes == 2);
        CHECK(automatic_config_toast(minecraft, r) == "RPCS3: configuração automática ativa para BLUS31426 (2 ajuste(s) específico(s) para este jogo)");
        i18n::apply(i18n::setting::en, -1);
        CHECK(automatic_config_toast(minecraft, r) == "RPCS3: automatic configuration active for BLUS31426 (2 fix(es) specific to this game)");
        i18n::apply(i18n::setting::es, -1);
        CHECK(automatic_config_toast(minecraft, r) == "RPCS3: configuración automática activa para BLUS31426 (2 ajuste(s) específico(s) para este juego)");
        i18n::apply(i18n::setting::automatic, -1);
    }
    // Outside the automatic mode no toast, even when an every-mode row (Accurate SPU Reservations) changes a value
    for (const bool tuned : {true, false})
    {
        reset_state();
        CHECK(tuned ? set_user_tuned(demons_souls, true) : set_auto_config_enabled(false));
        cfg_root cfg; // saved without Write Color Buffers: differs from the automatic values, so the marker holds
        cfg.core.spu_accurate_reservations.set(false);
        const policy_result r = boot(cfg, demons_souls);
        CHECK(r.mode == (tuned ? policy_mode::user_tuned : policy_mode::auto_config_off) && r.title_changes == 1);
        CHECK(automatic_config_toast(demons_souls, r).empty());
    }
    reset_state();
}

// Settings format 2: the 'PS4 user settings:' list (pins), its parser and writer, and the keys of the cfg tree.
void test_pin_format()
{
    using keys = std::vector<std::string>;
    CHECK(parse_user_settings("").empty());
    CHECK(parse_user_settings("PS4 user settings: []\n").empty());
    CHECK(parse_user_settings("PS4 user settings: ~\n").empty());
    CHECK(parse_user_settings("PS4 user settings:\n").empty());
    // Block items: quotes, comments, blank lines, CRLF, duplicates once; the list ends at the next key.
    CHECK(parse_user_settings("Core:\n  PPU Threads: 2\nPS4 user settings:\r\n  - \"Video > Write Color Buffers\"\r\n  # a comment\r\n\r\n"
                              "  - 'Core > SPU Block Size' # why\r\n  - Audio > Desired Audio Buffer Duration\r\n  - \"Video > Write Color Buffers\"\r\n"
                              "PS4 settings format: 2\r\n  - \"Video > Strict Rendering Mode\"\n") ==
        keys({"Video > Write Color Buffers", "Core > SPU Block Size", "Audio > Desired Audio Buffer Duration"}));
    CHECK(parse_user_settings("PS4 user settings:\n- \"Video > Renderer\"\n-\n- \"\"\n") == keys{"Video > Renderer"});
    // Flow list on the key's line.
    CHECK(parse_user_settings("PS4 user settings: [\"Video > Renderer\", 'Core > PPU Threads', Net > UPNP Enabled]\n") ==
        keys({"Video > Renderer", "Core > PPU Threads", "Net > UPNP Enabled"}));
    // Escapes inside quotes ('it''s' and \" \\).
    CHECK(parse_user_settings("PS4 user settings:\n  - \"A \\\"q\\\" \\\\ b\"\n  - 'it''s'\n") == keys({"A \"q\" \\ b", "it's"}));
    // Only the top-level key, spelled exactly; an unterminated quote is skipped.
    CHECK(parse_user_settings("Video:\n  PS4 user settings:\n  - \"x\"\n").empty());
    CHECK(parse_user_settings("PS4 user settings2:\n  - \"x\"\n").empty());
    CHECK(parse_user_settings("PS4 user settings:\n  - \"open\n  - \"Video > Renderer\"\n") == keys{"Video > Renderer"});
    {
        std::string many = "PS4 user settings:\n";
        for (int i = 0; i < 2000; i++) many += "  - \"Key " + std::to_string(i) + "\"\n";
        CHECK(parse_user_settings(many).size() == user_settings_max_keys);
        std::string long_keys = "PS4 user settings:\n";
        for (int i = 0; i < 100; i++) long_keys += "  - \"" + std::to_string(i) + std::string(1000, 'k') + "\"\n";
        CHECK(parse_user_settings(long_keys).size() == 65); // 64 KiB of key text
    }

    // Format: absent = 1; the pins of a text count only in format 2.
    CHECK(saved_settings_format("") == 1 && saved_settings_format("Video:\n  Renderer: Vulkan\n") == 1);
    CHECK(saved_settings_format("PS4 settings format: 2\n") == 2 && saved_settings_format("PS4 settings format: \"3\"\r\n") == 3);
    CHECK(pins_of("PS4 user settings:\n  - \"Video > Renderer\"\n").empty());
    CHECK(pins_of("PS4 settings format: 2\nPS4 user settings:\n  - \"Video > Renderer\"\n") == keys{"Video > Renderer"});

    // Writer: the stamp keys last, the list deduplicated; a second save replaces them; an empty document is dropped.
    const std::string stamped = with_settings_stamp("Video:\n  Write Color Buffers: true\n",
        {"Video > Write Color Buffers", "Video > Write Color Buffers", "Core > SPU Block Size", ""});
    CHECK(stamped == "Video:\n  Write Color Buffers: true\nPS4 automatic profile: 40\nPS4 settings format: 2\nPS4 user settings:\n"
                     "  - \"Video > Write Color Buffers\"\n  - \"Core > SPU Block Size\"\n");
    CHECK(saved_profile_version(stamped) == automatic_profile_version && saved_settings_format(stamped) == 2);
    CHECK(pins_of(stamped) == keys({"Video > Write Color Buffers", "Core > SPU Block Size"}));
    CHECK(with_settings_stamp(stamped, {"Audio > Enable Buffering"}) ==
        "Video:\n  Write Color Buffers: true\nPS4 automatic profile: 40\nPS4 settings format: 2\nPS4 user settings:\n  - \"Audio > Enable Buffering\"\n");
    CHECK(with_settings_stamp(with_profile_stamp("Video:\n"), {}) == "Video:\nPS4 automatic profile: 40\nPS4 settings format: 2\nPS4 user settings: []\n");
    CHECK(with_settings_stamp("{}\n", {}) == "PS4 automatic profile: 40\nPS4 settings format: 2\nPS4 user settings: []\n");
    CHECK(with_settings_stamp("", {"Video > Renderer"}) == "PS4 automatic profile: 40\nPS4 settings format: 2\nPS4 user settings:\n  - \"Video > Renderer\"\n");
    CHECK(pins_of(with_settings_stamp("", {"A \"b\" \\c"})) == keys{"A \"b\" \\c"});
    {
        cfg_root cfg; // a config load skips the three keys like any unknown key
        CHECK(cfg.from_string(stamped) && cfg.video.write_color_buffers.get());
    }

    // Keys: node names joined with " > " (the real tree's sections in the mock).
    {
        cfg_root cfg;
        CHECK(setting_key(cfg.video.vk.vram_allocation_limit) == "Video > Vulkan > VRAM allocation limit (MB)");
        CHECK(setting_key(cfg.core.spu_block_size) == "Core > SPU Block Size");
        CHECK(setting_key(cfg.audio.enable_buffering) == "Audio > Enable Buffering");
        CHECK(setting_key(cfg.io.pad_sleep) == "Input/Output > Pad handler sleep (microseconds)");
        CHECK(setting_key(cfg.misc.autoexit) == "Miscellaneous > Exit RPCS3 when process finishes");
        const pinned_settings pins = resolve_pins(cfg, {{"Video > Renderer", "Video > Renderer", "Video > Future Option"},
            {"Core > SPU Block Size", "Renderer", "Video > Future Option"}});
        CHECK(pins.global.size() == 1 && pins.global[0] == &cfg.video.renderer);
        CHECK(pins.game.size() == 1 && pins.game[0] == &cfg.core.spu_block_size);
        CHECK(pins.unknown == keys({"Video > Future Option", "Renderer"}));
        CHECK(pins.any(cfg.video.renderer) && !pins.for_game(cfg.video.renderer) && pins.for_game(cfg.core.spu_block_size));
        CHECK(resolve_pins(cfg, {}).empty());
        // A revoked value of a file stamped at or below the revoking version is no pin; a newer stamp or another value is.
        static constexpr pin_revoke revokes[] = {{"Core > SPU Block Size", "Mega", 39}};
        cfg.core.spu_block_size.set(spu_block_size_type::mega);
        pin_set old_file{{}, {"Core > SPU Block Size"}};
        old_file.game_profile = 39;
        const pinned_settings revoked = resolve_pins(cfg, old_file, revokes);
        CHECK(revoked.game.empty() && revoked.revoked == keys{"Core > SPU Block Size=Mega"});
        CHECK(policy_detail::pins_line("BLUS31426", revoked).find("de volta ao automático (perfil 40): Core > SPU Block Size=Mega") != std::string::npos);
        old_file.game_profile = 40;
        CHECK(resolve_pins(cfg, old_file, revokes).game.size() == 1);
        old_file.game_profile = 39;
        cfg.core.spu_block_size.set(spu_block_size_type::giga);
        CHECK(resolve_pins(cfg, old_file, revokes).game.size() == 1);
        CHECK(unpin_on_profile.empty());
    }

    // read_pins: ps4-settings.yml and the game's YAML, format 2 only.
    reset_state();
    CHECK(read_pins(minecraft).empty());
    fs::test_files()[global_settings_path] = with_settings_stamp("Video:\n  Renderer: Vulkan\n", {"Video > Renderer"});
    fs::test_files()[rpcs3::utils::get_custom_config_path(minecraft)] = with_profile_stamp("PS4 user settings:\n  - \"Core > SPU Block Size\"\n");
    {
        const pin_set pins = read_pins(minecraft);
        CHECK(pins.global == keys{"Video > Renderer"} && pins.game.empty()); // the game file is format 1
    }
    fs::test_files()[rpcs3::utils::get_custom_config_path(minecraft)] = with_settings_stamp("", {"Core > SPU Block Size"});
    CHECK(read_pins(minecraft).game == keys{"Core > SPU Block Size"});
    CHECK(read_pins("").game.empty() && read_pins("").global == keys{"Video > Renderer"});
    reset_state();
}

// The player's pins at boot: a pinned setting survives the profile, the game's PC database entry and the automatic /
// fallback rows; every-mode rows, console limits, the runtime section and the hardware limit still win; a global pin
// beats the profile but not the database entry (as PC puts the database over the global config).
void test_pins_boot()
{
    const std::string game_path = rpcs3::utils::get_custom_config_path(minecraft);
    const auto game_file = [](std::string_view title, std::string yaml, const std::vector<std::string>& pins)
    {
        const std::string text = with_settings_stamp(std::move(yaml), pins);
        fs::test_files()[rpcs3::utils::get_custom_config_path(std::string(title))] = text;
        return text;
    };

    // Profile: pinned values stay, the other values of the same file still follow the profile.
    reset_state();
    {
        const std::string text = game_file(minecraft, "Core:\n  SPU Block Size: Mega\nVideo:\n  Write Color Buffers: true\n  Strict Rendering Mode: true\n",
            {"Video > Write Color Buffers", "Core > SPU Block Size"});
        cfg_root cfg;
        CHECK(cfg.from_string(text));
        std::vector<change> changes;
        const policy_result r = boot(cfg, minecraft, &changes);
        CHECK(r.mode == policy_mode::automatic && r.pinned_game == 2 && r.pinned_global == 0);
        CHECK(cfg.video.write_color_buffers.get() && cfg.core.spu_block_size.get() == spu_block_size_type::mega);
        CHECK(!cfg.video.strict_rendering_mode.get()); // not pinned: the profile's value
        CHECK(!find_named(changes, "Write Color Buffers") && !find_named(changes, "SPU Block Size"));
        CHECK(contains(g_checkpoints, "Política PS4 para BLUS31426: 2 ajuste(s) seu(s): Video > Write Color Buffers=true, Core > SPU Block Size=Mega · ajustes globais: 0"));
    }
    // The same YAML without the format key (an older menu save): the profile decides, no pins line.
    fs::test_files()[game_path] = with_profile_stamp("Video:\n  Write Color Buffers: true\nPS4 user settings:\n  - \"Video > Write Color Buffers\"\n");
    {
        g_checkpoints.clear();
        cfg_root cfg;
        cfg.video.write_color_buffers.set(true);
        const policy_result r = boot(cfg, minecraft);
        CHECK(r.pinned_game == 0 && !cfg.video.write_color_buffers.get());
        CHECK(!contains(g_checkpoints, "ajuste(s) seu(s)"));
    }

    // The game's PC database entry: a game pin is put back like a row, the entry's other values run.
    reset_state();
    write_database({{minecraft, "Video:\n  Write Color Buffers: true\n  Strict Rendering Mode: true"}});
    {
        const std::string text = game_file(minecraft, "Video:\n  Write Color Buffers: false\n", {"Video > Write Color Buffers"});
        cfg_root cfg;
        CHECK(cfg.from_string(text));
        const policy_result r = boot(cfg, minecraft);
        CHECK(r.database.use == database_use::applied && r.database.changes == 1);
        CHECK(!cfg.video.write_color_buffers.get() && cfg.video.strict_rendering_mode.get());
    }
    // A global pin: it beats the profile, the database entry beats it.
    reset_state();
    write_database({{minecraft, "Video:\n  Write Color Buffers: true"}});
    fs::test_files()[global_settings_path] = with_settings_stamp("Video:\n  Write Color Buffers: false\n  Strict Rendering Mode: true\n",
        {"Video > Write Color Buffers", "Video > Strict Rendering Mode"});
    {
        cfg_root cfg;
        CHECK(cfg.from_string(fs::test_files()[global_settings_path]));
        const policy_result r = boot(cfg, minecraft);
        CHECK(r.pinned_global == 2 && r.pinned_game == 0);
        CHECK(cfg.video.write_color_buffers.get() && cfg.video.strict_rendering_mode.get());
        CHECK(contains(g_checkpoints, "Política PS4 para BLUS31426: 0 ajuste(s) seu(s) · ajustes globais: 2"));
    }
    {
        cfg_root cfg; // the global pass (Emu.Init) keeps the global pins too
        CHECK(cfg.from_string(fs::test_files()[global_settings_path]));
        boot(cfg, "");
        CHECK(cfg.video.strict_rendering_mode.get() && !cfg.video.write_color_buffers.get());
    }

    // Rows: Demon's Souls' automatic row (Write Color Buffers on) leaves a game pin alone; its every-mode crash fix
    // (Accurate SPU Reservations) still wins over one.
    reset_state();
    {
        const std::string text = game_file(demons_souls, "Core:\n  Accurate SPU Reservations: false\nVideo:\n  Write Color Buffers: false\n",
            {"Video > Write Color Buffers", "Core > Accurate SPU Reservations"});
        cfg_root cfg;
        CHECK(cfg.from_string(text));
        const policy_result r = boot(cfg, demons_souls);
        CHECK(!cfg.video.write_color_buffers.get() && cfg.core.spu_accurate_reservations.get());
        CHECK(r.title_changes == 1);
        CHECK(title_entry_for(cfg, demons_souls, cfg.video.write_color_buffers) != nullptr);
        const pinned_settings pins = resolve_pins(cfg, read_pins(demons_souls));
        CHECK(title_entry_for(cfg, demons_souls, cfg.video.write_color_buffers, true, &pins) == nullptr);
        CHECK(title_entry_for(cfg, demons_souls, cfg.core.spu_accurate_reservations, true, &pins) != nullptr);
    }
    // The God of War: Ascension fallback rows (no database) leave its pinned settings alone, and with Sleep Timers
    // Accuracy pinned the row no longer asks for host sleeps.
    reset_state();
    {
        const std::string text = game_file(gow_ascension, "Video:\n  Write Color Buffers: false\n",
            {"Video > Write Color Buffers", "Core > Sleep Timers Accuracy"});
        cfg_root cfg;
        CHECK(cfg.from_string(text));
        const policy_result r = boot(cfg, gow_ascension);
        CHECK(!cfg.video.write_color_buffers.get() && cfg.core.rsx_accurate_res_access.get());
        CHECK(!r.host_sleep_requested);
    }
    // and with its database entry (which also writes 'As Host')
    reset_state();
    write_database({{gow_ascension, gow_ascension_entry}});
    {
        const std::string text = game_file(gow_ascension, "Core:\n  Sleep Timers Accuracy: As Host\n", {"Core > Sleep Timers Accuracy"});
        cfg_root cfg;
        CHECK(cfg.from_string(text));
        const policy_result r = boot(cfg, gow_ascension);
        CHECK(r.database.use == database_use::applied && !r.host_sleep_requested && cfg.video.write_color_buffers.get());
    }

    // Never pinnable: console limits, the runtime section and the hardware limit replace a pinned value.
    reset_state();
    {
        const std::string text = game_file(minecraft,
            "Core:\n  Max LLVM Compile Threads: 6\n  Use LLVM CPU: znver4\n  Use Accurate DFMA: true\nVideo:\n  Multithreaded RSX: true\n  Vulkan:\n"
            "    VRAM allocation limit (MB): 2048\nVFS:\n  Enable /host_root/: true\nNet:\n  Internet enabled: Connected\n  PSN status: RPCN\n",
            {"Core > Max LLVM Compile Threads", "Core > Use LLVM CPU", "Core > Use Accurate DFMA", "Video > Multithreaded RSX",
             "Video > Vulkan > VRAM allocation limit (MB)", "VFS > Enable /host_root/", "Net > Internet enabled", "Net > PSN status"});
        cfg_root cfg;
        CHECK(cfg.from_string(text));
        const policy_result r = boot(cfg, minecraft);
        CHECK(r.pinned_game == 8);
        CHECK(cfg.core.llvm_threads.get() == 2 && cfg.core.llvm_cpu.get().empty() && !cfg.video.multithreaded_rsx.get());
        CHECK(cfg.video.vk.vram_allocation_limit.get() == 768 && !cfg.vfs.host_root.get());
        CHECK(cfg.net.net_active.get() == np_internet_status::disabled && cfg.net.psn_status.get() == np_psn_status::disabled);
        CHECK(r.console_changes == 3);
        apply_hardware_limits(cfg, minecraft, [](const cfg::_base&, const std::string&, std::string_view) {});
        CHECK(!cfg.core.use_accurate_dfma.get());
    }

    // 'Modo frio' leaves a pinned pad interval alone, even at the upstream 1000 us.
    reset_state();
    cool_mode::detail::state().store(1, std::memory_order_relaxed);
    {
        cfg_root cfg; // unpinned: 8 ms
        boot(cfg, minecraft);
        CHECK(cfg.io.pad_sleep.get() == 8000);
    }
    game_file(minecraft, "Input/Output:\n  Pad handler sleep (microseconds): 1000\n", {"Input/Output > Pad handler sleep (microseconds)"});
    {
        cfg_root cfg;
        const policy_result r = boot(cfg, minecraft);
        CHECK(cfg.io.pad_sleep.get() == 1000 && r.runtime_changes == 3);
    }

    // A pinned OpenGL renderer: the OpenGL shader mode of the profile (legacy, or async with gl-async.flag) and its ZCULL pair.
    reset_state();
    {
        const std::string text = game_file(minecraft, "Video:\n  Renderer: OpenGL\n", {"Video > Renderer"});
        for (const bool gl_async : {false, true})
        {
            if (gl_async) fs::test_files()[gl_async_flag] = "";
            cfg_root cfg;
            CHECK(cfg.from_string(text));
            boot(cfg, minecraft);
            CHECK(cfg.video.renderer.get() == video_renderer::opengl);
            CHECK(cfg.video.shadermode.get() == (gl_async ? shader_mode::async_recompiler : shader_mode::recompiler));
            CHECK(!cfg.video.relaxed_zcull_sync.get() && cfg.video.precise_zpass_count.get()); // 0.40: the strict pair
        }
        safe_file("gl-relaxed-zcull.flag", true);
        cfg_root cfg;
        CHECK(cfg.from_string(text));
        boot(cfg, minecraft);
        CHECK(cfg.video.relaxed_zcull_sync.get() && !cfg.video.precise_zpass_count.get());
        safe_file("gl-relaxed-zcull.flag", false);
    }
    // ... and a pinned Shader Mode next to it stays.
    {
        const std::string text = game_file(minecraft, "Video:\n  Renderer: OpenGL\n  Shader Mode: Shader Interpreter only\n", {"Video > Renderer", "Video > Shader Mode"});
        cfg_root cfg;
        CHECK(cfg.from_string(text));
        boot(cfg, minecraft);
        CHECK(cfg.video.shadermode.get() == shader_mode::interpreter_only);
    }

    // Keys this build does not know are ignored and named in the pins line.
    reset_state();
    {
        const std::string text = game_file(minecraft, "Video:\n  Write Color Buffers: true\n", {"Video > Write Color Buffers", "Video > Future Option"});
        cfg_root cfg;
        CHECK(cfg.from_string(text));
        const policy_result r = boot(cfg, minecraft);
        CHECK(r.pinned_game == 1 && cfg.video.write_color_buffers.get());
        CHECK(contains(g_checkpoints, "Política PS4 para BLUS31426: 1 ajuste(s) seu(s): Video > Write Color Buffers=true · ajustes globais: 0 · "
                                      "sem opção nesta versão (ignorados): Video > Future Option"));
    }
    // More than 8 game pins: the first 8 with their values, then the count.
    {
        const std::vector<std::string> many = {"Core > PPU Debug", "Core > SPU Debug", "Core > MFC Debug", "Core > PPU Profiler", "Core > SPU Profiler",
            "Core > Save LLVM logs", "Video > Debug output", "Video > Log shader programs", "Video > Read Color Buffers", "Video > Read Depth Buffer"};
        game_file(minecraft, "", many);
        g_checkpoints.clear();
        cfg_root cfg;
        boot(cfg, minecraft);
        CHECK(contains(g_checkpoints, "10 ajuste(s) seu(s): Core > PPU Debug=false, ") && contains(g_checkpoints, "Video > Log shader programs=false (+2) · ajustes globais: 0"));
    }

    // The preview takes the pins the menu is editing instead of the files; an empty set means none.
    reset_state();
    {
        cfg_root cfg;
        cfg.video.write_color_buffers.set(true);
        const pin_set editing{{}, {"Video > Write Color Buffers"}};
        const policy_result r = apply_boot_policy_preview(cfg, minecraft, [](const cfg::_base&, const std::string&, std::string_view, change_kind) {}, &editing);
        CHECK(r.pinned_game == 1 && cfg.video.write_color_buffers.get() && g_checkpoints.empty());
    }
    game_file(minecraft, "Video:\n  Write Color Buffers: true\n", {"Video > Write Color Buffers"});
    {
        cfg_root cfg;
        cfg.video.write_color_buffers.set(true);
        const pin_set none;
        const runtime_inputs in = read_runtime_inputs(minecraft);
        apply_boot_policy_preview(cfg, minecraft, in, [](const cfg::_base&, const std::string&, std::string_view, change_kind) {}, &none);
        CHECK(!cfg.video.write_color_buffers.get());
        cfg.video.write_color_buffers.set(true);
        apply_boot_policy_preview(cfg, minecraft, in, [](const cfg::_base&, const std::string&, std::string_view, change_kind) {});
        CHECK(cfg.video.write_color_buffers.get()); // null: the files' pins
    }
    reset_state();
}

// Format 2 outside the automatic mode: the user-tuned marker still runs everything as saved; the migration of values an
// older profile forced skips pinned settings and reads the global file for the others (a game file holds only pins).
void test_pins_modes()
{
    constexpr char mk[] = "BLUS30902";
    const std::string game_path = rpcs3::utils::get_custom_config_path(mk);

    // Marker + format 2 = manual: the global baseline's unpinned values run as saved.
    reset_state();
    CHECK(set_user_tuned(mk, true));
    fs::test_files()[game_path] = with_settings_stamp("Video:\n  Write Color Buffers: true\n", {"Video > Write Color Buffers"});
    {
        cfg_root cfg;
        cfg.video.strict_rendering_mode.set(true); // from ps4-settings.yml
        cfg.video.write_color_buffers.set(true);
        const policy_result r = boot(cfg, mk);
        CHECK(r.mode == policy_mode::user_tuned && !r.stale_marker && r.profile_changes == 0 && r.pinned_game == 1);
        CHECK(cfg.video.strict_rendering_mode.get() && cfg.video.write_color_buffers.get());
    }
    // A marker whose values run exactly like the automatic configuration with the pins is stale (same values either way).
    {
        cfg_root cfg;
        cfg.video.write_color_buffers.set(true);
        boot(cfg, minecraft); // the profile's values for everything else
        cfg.video.write_color_buffers.set(true);
        const policy_result r = boot(cfg, mk);
        CHECK(r.mode == policy_mode::automatic && r.stale_marker && cfg.video.write_color_buffers.get());
    }

    // Migration: automatic configuration off, an unstamped full-dump ps4-settings.yml from 0.36 and a format-2 game file.
    reset_state();
    CHECK(set_auto_config_enabled(false));
    const std::string global_036 = "Video:\n  Shader Mode: Async Recompiler (multi-threaded)\n  Relaxed ZCULL Sync: true\n  Accurate ZCULL stats: false\n";
    fs::test_files()[global_settings_path] = global_036;
    fs::test_files()[game_path] = with_settings_stamp("Video:\n  Shader Mode: Async Recompiler (multi-threaded)\n", {"Video > Shader Mode"});
    CHECK(saved_settings_text(mk) == global_036);
    {
        cfg_root cfg;
        CHECK(cfg.from_string(global_036) && cfg.from_string(fs::test_files()[game_path]));
        const policy_result r = boot(cfg, mk);
        CHECK(r.mode == policy_mode::auto_config_off && r.migrated_changes == 2);
        CHECK(cfg.video.shadermode.get() == shader_mode::async_recompiler); // pinned: the player's
        CHECK(!cfg.video.relaxed_zcull_sync.get() && cfg.video.precise_zpass_count.get());
        CHECK(!contains(g_checkpoints, "profile: 'Shader Mode'"));
    }
    // Conversion of a 0.36 full dump to format 2: the old profile values migrate away, values the boot forces anyway are no
    // choice, the rest that differs from the automatic configuration (over the global values) are the pins.
    reset_state();
    cool_mode::detail::state().store(1, std::memory_order_relaxed);
    CHECK(set_online_enabled(true));
    {
        cfg_root global_values;
        const std::string global_text = global_values.to_string();
        cfg_root saved, automatic;
        CHECK(saved.from_string(global_text) && automatic.from_string(global_text));
        const std::string game_yaml = "Core:\n  Accurate RSX reservation access: true\n  Max LLVM Compile Threads: 0\nVideo:\n"
            "  Write Color Buffers: true\n  Shader Mode: Async Recompiler (multi-threaded)\n  Relaxed ZCULL Sync: true\n  Accurate ZCULL stats: false\n"
            "  Vulkan:\n    VRAM allocation limit (MB): 512\nAudio:\n  Desired Audio Buffer Duration: 34\nNet:\n  Internet enabled: Disconnected\n";
        CHECK(saved.from_string(game_yaml));
        const std::vector<std::string> pins = conversion_pins(saved, automatic, mk, read_runtime_inputs(mk), game_yaml);
        CHECK(pins == std::vector<std::string>({"Core > Accurate RSX reservation access", "Video > Write Color Buffers",
            "Video > Vulkan > VRAM allocation limit (MB)"}));
        CHECK(saved.video.vk.vram_allocation_limit.get() == 512 && saved.video.write_color_buffers.get());
        // A global pin keeps the automatic side at the global value, so the same value in the game file is no choice.
        cfg_root saved2, automatic2;
        cfg_root pinned_global;
        pinned_global.video.write_color_buffers.set(true);
        CHECK(saved2.from_string(pinned_global.to_string()) && automatic2.from_string(pinned_global.to_string()) && saved2.from_string(game_yaml));
        const std::vector<std::string> pins2 = conversion_pins(saved2, automatic2, mk, read_runtime_inputs(mk), game_yaml, {"Video > Write Color Buffers"});
        CHECK(std::find(pins2.begin(), pins2.end(), "Video > Write Color Buffers") == pins2.end() && pins2.size() == 2);
    }

    // A format-1 game file is still the migrated text, as before.
    fs::test_files()[game_path] = global_036;
    CHECK(saved_settings_text(mk) == global_036);
    fs::test_files()[game_path] = with_profile_stamp(global_036);
    CHECK(saved_settings_text(mk) == with_profile_stamp(global_036));
    reset_state();
}

// Exit RPCS3 when process finishes, Assume External Debugger, Initialize Directories and Automatically start games after
// boot are console limits: a copied PC config (or a pin) never changes them, in every mode but keep-user-settings.flag.
void test_app_console_limits()
{
    constexpr char copied[] = "Core:\n  Assume External Debugger: true\nVFS:\n  Initialize Directories: false\nMiscellaneous:\n"
        "  Automatically start games after boot: false\n  Exit RPCS3 when process finishes: true\n";
    for (const int mode : {0, 1, 2, 3}) // automatic, user-tuned, automatic configuration off, pinned in a format-2 file
    {
        reset_state();
        if (mode == 1) CHECK(set_user_tuned(minecraft, true));
        if (mode == 2) CHECK(set_auto_config_enabled(false));
        if (mode == 3)
            fs::test_files()[rpcs3::utils::get_custom_config_path(minecraft)] = with_settings_stamp(copied, {"Core > Assume External Debugger",
                "VFS > Initialize Directories", "Miscellaneous > Automatically start games after boot", "Miscellaneous > Exit RPCS3 when process finishes"});
        cfg_root cfg;
        CHECK(cfg.from_string(copied));
        CHECK(cfg.misc.autoexit.get() && cfg.core.external_debugger.get() && !cfg.vfs.init_dirs.get() && !cfg.misc.autostart.get());
        const policy_result r = boot(cfg, minecraft);
        CHECK(!cfg.misc.autoexit.get() && !cfg.core.external_debugger.get() && cfg.vfs.init_dirs.get() && cfg.misc.autostart.get());
        CHECK(r.console_changes == 4u + (mode == 1 || mode == 2 ? 2u : 0u)); // + the two thread limits outside automatic mode
        CHECK(contains(r.notes, "console limit: 'Exit RPCS3 when process finishes' = 'false' (was 'true'): the game returns to the XMB on PS4"));
        CHECK(contains(r.notes, "console limit: 'Initialize Directories' = 'true' (was 'false')"));
        CHECK(unsafe_option_for(cfg, cfg.misc.autostart) && unsafe_option_for(cfg, cfg.core.external_debugger));
    }
    // The upstream defaults are the safe values; keep-user-settings.flag forces nothing.
    reset_state();
    {
        cfg_root cfg;
        CHECK(boot(cfg, minecraft).console_changes == 0);
    }
    fs::test_files()[keep_user_settings_flag] = "";
    {
        cfg_root cfg;
        CHECK(cfg.from_string(copied));
        boot(cfg, minecraft);
        CHECK(cfg.misc.autoexit.get() && !cfg.vfs.init_dirs.get());
    }
    reset_state();
    {
        cfg_root cfg; // the menu line of the row, in the interface language
        const unsafe_option* item = unsafe_option_for(cfg, cfg.misc.autoexit);
        CHECK(item && item->menu == i18n::str::CFGR_LIMIT_AUTOEXIT && std::string(i18n::tr(item->menu)).find("XMB") != std::string::npos);
    }
}

// 0.40: the renderer's reader of the command-buffer split: false until a boot stores its decision, and the row
// cb_split_after_loss (kill cb-split-off.flag) turns it off.
void test_cb_split_reader()
{
    reset_state();
    const u64 saved = safe_mode::g_on.load();
    policy::g_cb_split_boot.store(false);
    CHECK(!policy::cb_split_for_title());
    policy::g_cb_split_boot.store(true);
    safe_mode::g_on.store(safe_mode::default_mask);
    CHECK(policy::cb_split_for_title());
    safe_mode::g_on.store(safe_mode::default_mask & ~safe_mode::bit(safe_mode::feature::cb_split_after_loss));
    CHECK(!policy::cb_split_for_title());
    safe_file("cb-split-off.flag", true);
    CHECK(!safe_mode::enabled_in(safe_mode::compute(safe_mode::read_inputs()), safe_mode::feature::cb_split_after_loss));
    safe_file("cb-split-off.flag", false);
    CHECK(safe_mode::enabled_in(safe_mode::compute(safe_mode::read_inputs()), safe_mode::feature::cb_split_after_loss));
    policy::g_cb_split_boot.store(false);
    safe_mode::g_on.store(saved);
    CHECK(!policy::cb_split_for_title());
}

// 0.40: after a device loss (adapt cb_split, 8 boots) the boot stores the renderer's decision for that title only; the
// adapt line names it, the toast says so on Vulkan, cb-split-off.flag keeps the key but no split.
void test_cb_split_boot()
{
    reset_state();
    CHECK(adapt::append(terraria, adapt::key_cb_split));
    {
        cfg_root cfg;
        boot(cfg, terraria);
        CHECK(policy::g_cb_split_boot.load());
        CHECK(contains(g_checkpoints, "adapt: BLUS31537 keys cb_split (this boot and 7 more): cb_split=384_draws"));
        CHECK(g_toasts.size() == 1 && g_toasts[0] == "RPCS3: ajuste automatico para BLUS31537 apos falha anterior: comandos da GPU divididos (a GPU travou)");
    }
    // the emulator's own pass and another title: no split
    {
        cfg_root cfg;
        boot(cfg, "");
        CHECK(!policy::g_cb_split_boot.load());
        cfg_root other;
        boot(other, minecraft);
        CHECK(!policy::g_cb_split_boot.load());
    }
    // the kill flag: the key stays, the split does not run, no toast item
    safe_file("cb-split-off.flag", true);
    {
        g_checkpoints.clear();
        g_toasts.clear();
        cfg_root cfg;
        boot(cfg, terraria);
        CHECK(policy::g_cb_split_boot.load()); // the stored decision; cb_split_for_title() also reads the row
        CHECK(contains(g_checkpoints, "cb_split=off(cb-split-off.flag)") && g_toasts.empty());
    }
    safe_file("cb-split-off.flag", false);
    // OpenGL: the split is a Vulkan renderer path
    {
        g_checkpoints.clear();
        g_toasts.clear();
        cfg_root cfg;
        CHECK(set_user_tuned(terraria, true));
        cfg.video.renderer.set(video_renderer::opengl);
        boot(cfg, terraria);
        CHECK(contains(g_checkpoints, "cb_split=vulkan_only") && g_toasts.empty());
        CHECK(set_user_tuned(terraria, false));
    }
    // 8 boots from the XMB in all (3 above), then the key expires and the file goes
    for (int i = 0; i < 5; i++)
    {
        cfg_root cfg;
        boot(cfg, terraria);
        CHECK(policy::g_cb_split_boot.load());
    }
    {
        g_checkpoints.clear();
        cfg_root cfg;
        boot(cfg, terraria);
        CHECK(!policy::g_cb_split_boot.load() && contains(g_checkpoints, "adapt: BLUS31537: no active adjustment left, file removed"));
    }
    // keep-user-settings.flag: nothing is forced, but the renderer path still follows the key
    CHECK(adapt::append(terraria, adapt::key_cb_split));
    fs::test_files()[keep_user_settings_flag] = "";
    {
        cfg_root cfg;
        boot(cfg, terraria);
        CHECK(policy::g_cb_split_boot.load());
    }
    fs::test_files().erase(keep_user_settings_flag);
    policy::g_cb_split_boot.store(false);
    reset_state();
}

// 0.40: a title level after a thread failure keeps the title's PC config database entry (and the lazy cellMusic row);
// any other failure skips it from level 2 as before.
void test_kind_kept_boot()
{
    constexpr char sonic[] = "BLES00750";
    reset_state();
    write_database({{sonic, "Video:\n  MSAA: Disabled\n  Write Color Buffers: true\n  Strict Rendering Mode: true"}});
    for (const char* fail : {adapt::key_fail_threads, adapt::key_fail_crash})
        for (const char* level : {adapt::key_safe2, adapt::key_safe3})
        {
            adapt::remove(sonic);
            CHECK(adapt::append(sonic, fail) && adapt::append(sonic, level));
            g_checkpoints.clear();
            cfg_root cfg;
            const policy_result r = boot(cfg, sonic);
            const bool threads = std::string_view(fail) == adapt::key_fail_threads;
            CHECK(r.database.use == (threads ? database_use::applied : database_use::skipped));
            CHECK(cfg.video.strict_rendering_mode.get() == threads);
            const runtime_inputs in = read_runtime_inputs(sonic);
            CHECK(feature_on(in, safe_mode::feature::config_db_entry) == threads && feature_on(in, safe_mode::feature::music_lazy) == threads);
            CHECK(!safe_item(in, safe_mode::item::config_db_skip) == threads);
            // the other level items still apply: online off, one compile thread
            CHECK(cfg.core.llvm_threads.get() == 1);
        }
    adapt::remove(sonic);
    reset_state();
}

// 0.40: custom_configs/<TITLE_ID>.features turns rows on for one title's boots (the 0.39 audio rules per game)
void test_title_features_boot()
{
    reset_state();
    const std::filesystem::path file = std::filesystem::path(RPCS3_PS4_SAFE_MODE_DIR) / "custom_configs" / (std::string(terraria) + ".features");
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    std::ofstream(file, std::ios::binary) << "audio_reserve_64\n";
    {
        cfg_root cfg;
        boot(cfg, terraria);
        CHECK(cfg.audio.desired_buffer_duration.get() == 64);
        CHECK(feature_on(read_runtime_inputs(terraria), safe_mode::feature::audio_reserve_64));
        CHECK(safe_mode::enabled_in(policy_features(terraria), safe_mode::feature::audio_reserve_64));
        cfg_root other;
        boot(other, minecraft);
        CHECK(other.audio.desired_buffer_duration.get() == 34);
        CHECK(!safe_mode::enabled_in(policy_features(minecraft), safe_mode::feature::audio_reserve_64));
    }
    // a level and the global safe mode still win
    CHECK(adapt::append(terraria, adapt::key_fail_crash) && adapt::append(terraria, adapt::key_safe1));
    {
        cfg_root cfg;
        boot(cfg, terraria);
        CHECK(cfg.audio.desired_buffer_duration.get() == 34);
    }
    adapt::remove(terraria);
    safe_file("safe-mode.flag", true);
    {
        cfg_root cfg;
        boot(cfg, terraria);
        CHECK(cfg.audio.desired_buffer_duration.get() == 34);
    }
    safe_file("safe-mode.flag", false);
    std::filesystem::remove(file, ec);
    {
        cfg_root cfg;
        boot(cfg, terraria);
        CHECK(cfg.audio.desired_buffer_duration.get() == 34);
    }
    reset_state();
}

// 0.40: Resolution Scale above 200 % runs at 200 % in every mode (a pin, a saved file); 150 stays; the PC database cap
// of 100 % stays.
void test_resolution_scale_cap()
{
    for (const int mode : {0, 1, 2}) // automatic with a global pin, user-tuned, automatic configuration off
    {
        reset_state();
        if (mode == 1) CHECK(set_user_tuned(minecraft, true));
        if (mode == 2) CHECK(set_auto_config_enabled(false));
        for (const u64 asked : {u64{775}, u64{201}, u64{200}, u64{150}})
        {
            g_checkpoints.clear();
            cfg_root cfg;
            cfg.video.resolution_scale_percent.set(asked);
            pin_set pins;
            pins.global = {"Video > Resolution Scale"};
            std::vector<change> changes;
            apply_boot_policy(cfg, minecraft, [&](const cfg::_base& setting, const std::string& old, std::string_view why, change_kind kind)
            {
                changes.push_back({setting.get_name(), old, setting.to_string(), std::string(why), kind});
            }, mode == 0 ? &pins : nullptr);
            const u64 expected = std::min<u64>(asked, resolution_scale_cap_percent);
            CHECK(cfg.video.resolution_scale_percent.get() == expected);
            const change* c = find_named(changes, "Resolution Scale");
            if (asked > 200)
            {
                CHECK(c && c->kind == change_kind::console && c->now == "200" && c->why.find("768 MB") != std::string::npos);
                CHECK(contains(g_checkpoints, "console limit: 'Resolution Scale' = '200' (was '" + std::to_string(asked) + "'): above 200 %"));
                CHECK(contains(g_checkpoints, "Resolution Scale " + std::to_string(asked) + "->200 (limite do PS4)"));
            }
            else CHECK(!c);
        }
    }
    // the database cap stays 100 %
    reset_state();
    write_database({{minecraft, "Video:\n  Resolution Scale: 300"}});
    {
        cfg_root cfg;
        boot(cfg, minecraft);
        CHECK(cfg.video.resolution_scale_percent.get() == 100);
    }
    reset_state();
}

// 0.40: the platform step: OpenGL runs the legacy recompiler without gl-async.flag (Desligada and pins included); the
// pure rules the renderers share
void test_platform_limits()
{
    CHECK(platform_gl_shader_mode(shader_mode::async_with_interpreter, false) == shader_mode::recompiler);
    CHECK(platform_gl_shader_mode(shader_mode::interpreter_only, false) == shader_mode::recompiler);
    CHECK(platform_gl_shader_mode(shader_mode::recompiler, false) == shader_mode::recompiler);
    CHECK(platform_gl_shader_mode(shader_mode::async_recompiler, true) == shader_mode::async_recompiler);
    CHECK(platform_vsync_ignored(video_renderer::vulkan) && !platform_vsync_ignored(video_renderer::opengl));
    CHECK(platform_buffering_warned(false) && !platform_buffering_warned(true));
    reset_state();
    CHECK(set_auto_config_enabled(false));
    {
        g_checkpoints.clear();
        cfg_root cfg;
        cfg.video.renderer.set(video_renderer::opengl);
        cfg.video.shadermode.set(shader_mode::interpreter_only);
        std::vector<change> changes;
        const policy_result r = boot(cfg, minecraft, &changes);
        CHECK(cfg.video.shadermode.get() == shader_mode::recompiler && r.console_changes >= 1);
        const change* c = find_named(changes, "Shader Mode");
        CHECK(c && c->kind == change_kind::console && c->why.starts_with(platform_reason_prefix));
        CHECK(contains(g_checkpoints, "platform: 'Shader Mode' = 'Legacy Recompiler (single-threaded)' (was 'Shader Interpreter only'): OpenGL compiles"));
        CHECK(contains(g_checkpoints, "Shader Mode Shader Interpreter only->Legacy Recompiler (single-threaded) (plataforma)"));
    }
    fs::test_files()[gl_async_flag] = "";
    {
        cfg_root cfg;
        cfg.video.renderer.set(video_renderer::opengl);
        cfg.video.shadermode.set(shader_mode::interpreter_only);
        boot(cfg, minecraft);
        CHECK(cfg.video.shadermode.get() == shader_mode::interpreter_only);
    }
    fs::test_files().erase(gl_async_flag);
    {
        cfg_root cfg;
        cfg.video.shadermode.set(shader_mode::interpreter_only); // Vulkan: not a platform decision
        boot(cfg, minecraft);
        CHECK(cfg.video.shadermode.get() == shader_mode::interpreter_only);
    }
    reset_state();
}

// 0.40: the disk cache cap follows the free space of /data (10 %, 4096..16384 MB; the setting holds at most 10240);
// disk-cache-upstream.flag keeps 4096 MB; unknown free space keeps 4096 MB.
void test_disk_cache_cap()
{
    CHECK(disk_cache_cap_mb(0) == 4096 && disk_cache_cap_mb(40960) == 4096 && disk_cache_cap_mb(41000) == 4100);
    CHECK(disk_cache_cap_mb(100000) == 10000 && disk_cache_cap_mb(163840) == 16384 && disk_cache_cap_mb(900000) == 16384);
    CHECK(data_free_mb() == 0); // host builds: unknown
    reset_state();
    const auto run = [](s64 configured, u64 free_mb, std::vector<change>* changes = nullptr)
    {
        cfg_root cfg;
        cfg.vfs.cache_max_size.set(configured);
        runtime_inputs in = read_runtime_inputs(minecraft);
        in.data_free_mb = free_mb;
        apply_runtime_limits(cfg, in, [&](const cfg::_base& setting, const std::string& old, std::string_view why, change_kind kind)
        {
            if (changes) changes->push_back({setting.get_name(), old, setting.to_string(), std::string(why), kind});
        });
        return cfg.vfs.cache_max_size.get();
    };
    CHECK(run(5120, 0) == 4096);         // unknown free space: the 0.31-0.39 cap
    CHECK(run(5120, 100000) == 5120);    // the player's (default) value inside the cap runs as it is
    CHECK(run(10240, 100000) == 10000);  // 10 % of 100000 MB free
    CHECK(run(10240, 200000) == 10240);  // the setting's own maximum
    CHECK(run(10240, 30000) == 4096);    // little room: at least 4096
    CHECK(run(0, 100000) == 1024);       // the floor
    {
        std::vector<change> changes;
        run(10240, 100000, &changes);
        const change* c = find_named(changes, "Disk cache maximum size (MB)");
        CHECK(c && c->why == "disk cache limit kept between 1024 and 10000 MB (10 % of the free space of /data)");
    }
    safe_file("disk-cache-upstream.flag", true);
    {
        std::vector<change> changes;
        CHECK(run(10240, 100000, &changes) == 4096);
        const change* c = find_named(changes, "Disk cache maximum size (MB)");
        CHECK(c && c->why == "disk cache limit kept between 1024 and 4096 MB (disk-cache-upstream.flag)");
    }
    safe_file("disk-cache-upstream.flag", false);
    runtime_inputs in;
    in.data_free_mb = 60000;
    CHECK(disk_cache_limit_max_mb(in) == 6000);
    reset_state();
}

// 0.40: the 'settings:' line: what the boot runs differently from the player's files and who replaced it, the emulator
// pass's replacements of the global files merged into the game's line, upstream defaults left out; the toast about a
// game's own format-1 values once per build version.
void test_settings_diff()
{
    using policy_detail::settings_diff_line;
    CHECK(settings_diff_line(minecraft, {}).empty());
    {
        std::vector<settings_diff> d(14);
        for (std::size_t i = 0; i < d.size(); i++) d[i] = {"Video > X", "X" + std::to_string(i), "1", "2", "automático", change_kind::profile};
        const std::string line = settings_diff_line(minecraft, d);
        CHECK(line.starts_with("settings: BLUS31426: 14 valor(es) rodando diferente dos seus arquivos: X0 1->2 (automático), X1 1->2 (automático)"));
        CHECK(line.find("X11 1->2 (automático) (+2)") != std::string::npos && line.find("X12") == std::string::npos);
        CHECK(policy_detail::diff_value(std::string(60, 'a')) == std::string(37, 'a') + "..." && policy_detail::diff_value("") == "\"\"");
    }
    reset_state();
    // One boot as Emulator::Init and Emulator::Load run it on one tree: the global files, the emulator pass, the game's
    // YAML over it, the game pass
    const auto two_passes = [](cfg_root& cfg, std::string_view global, std::string_view game)
    {
        CHECK(cfg.from_string(global));
        boot(cfg, "");
        if (!game.empty()) CHECK(cfg.from_string(game));
        return boot(cfg, minecraft);
    };
    {
        g_checkpoints.clear();
        cfg_root cfg;
        const policy_result r = two_passes(cfg, "Video:\n  Renderer: OpenGL\n  Frame limit: 30\nCore:\n  Max LLVM Compile Threads: 0\n", "Video:\n  Resolution Scale: 150\n");
        // Renderer and Frame limit from the global file (replaced by the emulator pass), Resolution Scale from the game's
        // YAML (the game pass); Max LLVM Compile Threads 0 is the upstream default: not the player's
        CHECK(r.settings_diffs.size() == 3);
        CHECK(contains(g_checkpoints, "settings: BLUS31426: 3 valor(es) rodando diferente dos seus arquivos: "));
        CHECK(contains(g_checkpoints, "Resolution Scale 150->100 (automático)"));
        CHECK(contains(g_checkpoints, "Renderer OpenGL->Vulkan (automático)"));
        CHECK(contains(g_checkpoints, "Frame limit 30->Auto (automático)"));
        CHECK(!contains(g_checkpoints, "Max LLVM Compile Threads"));
        CHECK(r.legacy_game_values == 0); // no own YAML on disk
    }
    // The game's YAML sets its own value over the replaced global one: the game's value is the file value
    {
        g_checkpoints.clear();
        cfg_root cfg;
        two_passes(cfg, "Video:\n  Frame limit: 30\n", "Video:\n  Frame limit: 60\n");
        CHECK(contains(g_checkpoints, "Frame limit 60->Auto (automático)") && !contains(g_checkpoints, "Frame limit 30->"));
    }
    // A value no step replaced is not listed; a console limit and a runtime switch name themselves
    {
        g_checkpoints.clear();
        cfg_root cfg;
        CHECK(set_auto_config_enabled(false));
        two_passes(cfg, "Video:\n  Frame limit: 30\n  Multithreaded RSX: true\nNet:\n  Internet enabled: Connected\n", {});
        CHECK(contains(g_checkpoints, "Multithreaded RSX true->false (limite do PS4)"));
        CHECK(contains(g_checkpoints, "Internet enabled Connected->Disconnected (interruptor)"));
        CHECK(!contains(g_checkpoints, "Frame limit"));
        CHECK(set_auto_config_enabled(true));
    }
    // Nothing differs: no line; the menu's preview never prints one
    {
        g_checkpoints.clear();
        cfg_root cfg;
        two_passes(cfg, {}, {});
        CHECK(!contains(g_checkpoints, "settings: "));
        cfg_root preview;
        CHECK(preview.from_string("Video:\n  Frame limit: 30\n"));
        g_checkpoints.clear();
        const policy_result r = apply_boot_policy_preview(preview, minecraft, [](const cfg::_base&, const std::string&, std::string_view) {});
        CHECK(r.settings_diffs.empty() && g_checkpoints.empty());
    }
    // A game's own format-1 YAML whose values the automatic profile replaced: one toast per title and build version
    const std::string game_yaml = "Video:\n  Frame limit: 30\n  Renderer: Vulkan\n";
    fs::test_files()[rpcs3::utils::get_custom_config_path(minecraft)] = game_yaml;
    for (int n = 0; n < 2; n++)
    {
        g_toasts.clear();
        cfg_root cfg;
        const policy_result r = two_passes(cfg, {}, game_yaml);
        CHECK(r.legacy_game_values == 1);
        CHECK(n == 0 ? g_toasts.size() == 1 && g_toasts[0] == "1 ajuste(s) do arquivo do jogo sem efeito · Quadrado no jogo > Converter" : g_toasts.empty());
        adapt::clear_title();
    }
    CHECK(fs::test_files()[policy_detail::legacy_notice_marker(minecraft)] == std::string(ps4_build_version) + "\n");
    // another build version shows it again; a format-2 file holds only pins and never counts
    fs::test_files()[policy_detail::legacy_notice_marker(minecraft)] = "0.39\n";
    {
        g_toasts.clear();
        cfg_root cfg;
        two_passes(cfg, {}, game_yaml);
        CHECK(g_toasts.size() == 1);
    }
    CHECK(policy_detail::legacy_game_values({{"Video > Frame limit", "Frame limit", "30", "Auto", "automático", change_kind::profile}},
        "Video:\n  Frame limit: 30\nPS4 settings format: 2\n") == 0);
    CHECK(policy_detail::legacy_game_values({{"Video > Frame limit", "Frame limit", "30", "Auto", "limite do PS4", change_kind::console}}, game_yaml) == 0);
    reset_state();
}

// 0.40: the global layer's own values (format 1) that the automatic configuration replaces, for 'Usar meus valores
// salvos': upstream defaults, pins, old profile values and console limits left out; a format-2 file has none.
void test_legacy_choices()
{
    reset_state();
    const runtime_inputs in = read_runtime_inputs({});
    const auto choices = [&](std::string_view yaml, const std::vector<std::string>& pins = {})
    {
        cfg_root saved, automatic;
        CHECK(saved.from_string(yaml) && automatic.from_string(yaml));
        return legacy_choices(saved, automatic, in, yaml, pins);
    };
    const std::string yaml = "Video:\n  Renderer: OpenGL\n  Frame limit: 30\n  Multithreaded RSX: true\nCore:\n  Max LLVM Compile Threads: 0\n";
    {
        const std::vector<std::string> keys = choices(yaml);
        CHECK(keys.size() == 2);
        CHECK(std::find(keys.begin(), keys.end(), "Video > Renderer") != keys.end());
        CHECK(std::find(keys.begin(), keys.end(), "Video > Frame limit") != keys.end());
    }
    CHECK(choices(yaml, {"Video > Frame limit"}) == std::vector<std::string>{"Video > Renderer"});
    // a value an older profile froze migrates (Async Recompiler on Vulkan, stamp <= 36): not a choice
    CHECK(choices("Video:\n  Shader Mode: Async Recompiler (multi-threaded)\n").empty());
    CHECK(choices("Video:\n  Shader Mode: Async Recompiler (multi-threaded)\nPS4 automatic profile: 39\n") ==
        std::vector<std::string>{"Video > Shader Mode"});
    // a format-2 global file: its other values are what the menu showed
    CHECK(choices(yaml + "PS4 settings format: 2\nPS4 user settings: []\n").empty());
    CHECK(choices({}).empty());
    reset_state();
}

int main()
{
    std::error_code ec;
    std::filesystem::remove_all(adapt::adapt_dir, ec);

    test_defaults_offline();
    test_online_flag_with_old_yaml();
    test_keep_user_settings();
    test_vram();
    test_adapt_expiry_and_relaunch();
    test_threads_low_skips_online();
    test_title_rows();
    test_preview_and_callbacks();
    test_cache_bounds();
    test_console_limits();
    test_demons_souls_graphics();
    test_stale_marker();
    test_config_db_reader();
    test_config_db_automatic();
    test_config_db_modes();
    test_llvm_cpu_limit();
    test_accurate_dfma_marker();
    test_native_interface();
    test_host_sleep_request();
    test_cool_mode();
    test_vk_compile_3();
    test_fallback_rows_034();
    test_profile_defaults_037();
    test_profile_migration();
    test_getllar_spin_limit();
    test_bundled_database();
    test_entry_keys();
    test_audio_reserve_040();
    test_opengl_profile_040();
    test_interpreter_warning_039();
    test_game_language_039();
    test_automatic_config_toast();
    test_pin_format();
    test_pins_boot();
    test_pins_modes();
    test_app_console_limits();
    test_online_without_account();
    test_safe_mode_boot();
    test_config_db_skip();
    test_cb_split_reader();
    test_cb_split_boot();
    test_kind_kept_boot();
    test_title_features_boot();
    test_resolution_scale_cap();
    test_platform_limits();
    test_disk_cache_cap();
    test_settings_diff();
    test_legacy_choices();

    reset_state();
    std::filesystem::remove_all(adapt::adapt_dir, ec);
    std::filesystem::remove_all(RPCS3_PS4_SAFE_MODE_DIR, ec);
    if (g_failures)
    {
        std::fprintf(stderr, "policy test FAILED: %d check(s)\n", g_failures);
        return EXIT_FAILURE;
    }
    std::puts("policy test passed");
    return EXIT_SUCCESS;
}
