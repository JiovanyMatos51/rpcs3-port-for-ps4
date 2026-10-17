// Host test of the settings browser (rpcs3/util/ps4_cfg_browser.hpp, ps4_cfg_catalog.hpp/.inc, ps4_i18n_cfg.inc) on the
// REAL config tree (rpcs3/Emu/system_config.h over tests/cfg-mocks) and the real boot policy (util/ps4_policy.hpp over
// tests/policy-mocks: the /data/rpcs3 flag files live in memory):
//  1 coverage: every leaf has one catalog row and every row a leaf, unique keys, no " > " in a name, the key equals
//    ps4_policy.hpp setting_key, cfg-mocks/cfg_tree.inc matches the tree, texts and value labels exist
//  2 classes against the policy: the console limits (7 + the 4 new ones), the runtime section and the thread limits are
//    unavailable or value-limited; a pin never passes them
//  3 round trips per type (bool, enum, int, float, uint64, uint128, strings with validators, Libraries Control, log
//    levels, anaglyph matrix), then staged -> sparse YAML -> a fresh tree over the global values: every key equal
//  4 badges: automatic profile, the title's row, its PC database entry, console limit, the PS4 switches, the adaptation,
//    the player's pin, a per-game value that differs from the global one
//  5 resets: Square = the automatic value, RPCS3 default, the global value, System Name / PSID never reset
//  6 dependencies: OpenGL and Shader Mode / ZCULL / gl-async.flag, the FSR row, the 3D rows, the per-game-only rows
//  plus: pages and captions, the warn confirmations, pins_to_save, the home-menu snapshot, the self-check
// Build and run from the repo root (llvm-mingw, as the other host tests):
//   clang++ -std=c++20 -O1 -static -Wall -Wextra -I ps4-port/tests/cfg-mocks -I ps4-port/tests/policy-mocks -I rpcs3 ps4-port/tests/cfg_browser_test.cpp -o cfg_browser_test.exe
//   cfg_browser_test.exe [--verbose]

#define LLVM_AVAILABLE 1
#define RPCS3_PS4_ADAPT_DIR "cfg-browser-test-adapt"
#define PS4_I18N_ID_NAMES 1
#include "util/ps4_cfg_browser.hpp"

#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// The pieces of the emulator the browser links against
std::string cfg_root::node_sys::get_random_system_name() { return "RPCS3-HOST-TEST"; }
u128 cfg_root::node_sys::get_random_psid() { return (u128(0x0123456789ABCDEFull) << 64) | 0xFEDCBA9876543210ull; }
extern const std::map<std::string_view, int> g_prx_list{{"libadec.sprx", 1}, {"libfs.sprx", 0}, {"libpngdec.sprx", 0}, {"libsysutil_np.sprx", 1}};
namespace countries
{
const std::array<country_code, 72> g_countries{{{"Japan", "jp"}, {"United States", "us"}, {"Brazil", "br"}}};
}
void ps4_notify(const char*) noexcept {}
void ps4_startup_checkpoint(const char*) {}

namespace
{
using namespace utils::ps4;
namespace cb = utils::ps4::cfg_browser;
namespace cat = utils::ps4::cfg_catalog;
using i18n::str;

int g_failures = 0;
int g_checks = 0;
bool g_verbose = false;

#define CHECK(expr) \
    do \
    { \
        ++g_checks; \
        if (!(expr)) \
        { \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #expr); \
            g_failures++; \
        } \
    } while (0)

void fail(const std::string& what)
{
    ++g_checks;
    ++g_failures;
    std::fprintf(stderr, "FAIL %s\n", what.c_str());
}

constexpr char minecraft[] = "BLUS31426";
constexpr char demons_souls[] = "BLUS30443";
constexpr char database_game[] = "BLUS99999";

bool contains(std::string_view text, std::string_view part) { return text.find(part) != std::string_view::npos; }

std::string T(str id) { return i18n::tr(id); }

// A settings menu in miniature: the YAML stack, then the model bound to it and previewed
struct fixture
{
    cfg_root cfg;
    cb::model m;
    std::string baseline;

    void open(std::string_view title, std::string_view global_yaml = {}, std::string_view game_yaml = {}, const pin_set& pins = {}, const runtime_inputs& in = {})
    {
        cfg.from_default();
        if (!global_yaml.empty()) CHECK(cfg.from_string(global_yaml));
        baseline = cfg.to_string();
        if (!game_yaml.empty()) CHECK(cfg.from_string(game_yaml));
        m.bind(cfg, title, baseline, pins, get_policy_mode(title));
        m.preview(in);
    }

    // Row index of 'key' on its tab's page (hidden rows shown)
    std::size_t row(std::string_view key)
    {
        const cat::item* it = cat::find(key);
        const cat::tab t = it ? it->where : cat::default_tab(key);
        const auto rows = m.page(t, true);
        for (std::size_t i = 0; i < rows.size(); ++i)
            if (rows[i].key == key) return i;
        fail("no row for " + std::string(key));
        return SIZE_MAX;
    }

    const cb::row& get(std::string_view key)
    {
        static cb::row none;
        const std::size_t i = row(key);
        const cat::item* it = cat::find(key);
        const auto rows = m.page(it ? it->where : cat::default_tab(key), true);
        return i < rows.size() ? rows[i] : none;
    }

    std::string value(std::string_view key)
    {
        const cfg::_base* leaf = cb::find(cfg, key);
        return leaf ? leaf->to_string() : std::string{};
    }

    // The picker entry of a YAML spelling
    std::string pick(std::string_view key, std::string_view spelling)
    {
        const auto list = m.choices(row(key));
        for (std::size_t c = 0; c < list.size(); ++c)
            if (list[c].spelling == spelling) return m.pick(row(key), c);
        fail("no choice " + std::string(spelling) + " for " + std::string(key));
        return {};
    }

    // An edit that confirms the warning first when the row or the value asks for one
    std::string confirmed(std::string_view key, auto&& edit)
    {
        std::string message = edit(row(key));
        for (int n = 0; n < 2 && m.confirmation(); ++n)
        {
            m.confirm();
            message = edit(row(key));
        }
        return message;
    }
};

std::string json_escape(std::string_view text)
{
    std::string out;
    for (const char c : text)
    {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c == '\n') out += "\\n";
        else out += c;
    }
    return out;
}

void write_database(std::string_view serial, std::string_view config)
{
    fs::test_files()[config_db::path] = "{\"return_code\": 0, \"timestamp\": 1784200630, \"games\": {\n\"" + std::string(serial) +
        "\": {\"status\": \"Playable\", \"config\": \"" + json_escape(config) + "\"}}}\n";
}

void reset_files()
{
    fs::test_files().clear();
}

// ---------------------------------------------------------------- 1 coverage

struct tree_leaf
{
    std::string_view key, type;
    double min, max;
    const char* def;
    bool dynamic;
};

#define CFG_LEAF(key, type, min, max, def, dynamic) {key, #type, double(min), double(max), def, dynamic},
const tree_leaf g_tree[] = {
#include "cfg-mocks/cfg_tree.inc"
};
#undef CFG_LEAF

std::string_view type_name(const cfg::_base& leaf)
{
    switch (leaf.get_type())
    {
    case cfg::type::_bool: return "bool";
    case cfg::type::_enum: return "enum";
    case cfg::type::_int:
    {
        const auto r = leaf.to_list();
        return !r.empty() && r[0].find('.') != std::string::npos ? "float" : "int";
    }
    case cfg::type::uint: return "uint";
    case cfg::type::uint128: return "uint128";
    case cfg::type::string: return "string";
    case cfg::type::set: return "set";
    case cfg::type::map: return "map";
    case cfg::type::node_map: return "node_map";
    case cfg::type::log: return "log";
    case cfg::type::device: return "device";
    default: break;
    }
    return "node";
}

void test_coverage()
{
    cfg_root c;
    const auto leaves = cb::leaves_of(c);
    CHECK(leaves.size() == cat::item_count);
    CHECK(leaves.size() == std::size(g_tree));
    std::set<std::string> keys;
    for (const cfg::_base* leaf : leaves)
    {
        const std::string key = cb::key_of(*leaf);
        CHECK(key == setting_key(*leaf));
        if (!keys.insert(key).second) fail("duplicate key " + key);
        for (const cfg::_base* p = leaf; p; p = p->get_parent())
            if (contains(p->get_name(), " > ")) fail("a node name contains ' > ': " + key);
        const cat::item* it = cat::find(key);
        if (!it) { fail("setting without a catalog row: " + key); continue; }
        // texts of every row (pt and en at least; i18n_test checks the 4 columns)
        if (std::string_view(i18n::tr(it->label, i18n::lang::pt_br)).empty() || std::string_view(i18n::tr(it->help, i18n::lang::pt_br)).empty())
            fail("row without label or help: " + key);
        // every enum spelling with words has a label (technical spellings such as 1280x720 or 16:9 are shown as they are)
        if (leaf->get_type() == cfg::type::_enum && it->kind != cat::klass::hidden)
            for (const std::string& spelling : leaf->to_list())
            {
                bool words = false;
                for (std::size_t i = 0; i + 1 < spelling.size(); ++i)
                    if (std::isalpha(static_cast<unsigned char>(spelling[i])) && std::isalpha(static_cast<unsigned char>(spelling[i + 1]))) words = true;
                if (words && !cat::find_value(key, spelling)) fail("enum value without a label: " + key + " = " + spelling);
            }
    }
    for (const cat::item& it : cat::items())
        if (!keys.count(std::string(it.key))) fail("catalog row without a setting: " + std::string(it.key));
    // sorted index: every key is found and unique
    std::set<std::string_view> catalog_keys;
    for (const cat::item& it : cat::items())
    {
        CHECK(cat::find(it.key) == &it);
        CHECK(catalog_keys.insert(it.key).second);
    }
    CHECK(cat::find("Video > No Such Setting") == nullptr);
    // the generated tree list is the tree
    std::size_t n = 0;
    for (const cfg::_base* leaf : leaves)
    {
        if (n >= std::size(g_tree)) break;
        const tree_leaf& t = g_tree[n++];
        if (t.key != cb::key_of(*leaf)) { fail("cfg_tree.inc order/key differs at " + std::string(t.key) + " (run gen_cfg_catalog.py)"); continue; }
        if (t.type != type_name(*leaf)) fail("cfg_tree.inc type of " + std::string(t.key) + ": " + std::string(t.type));
        if (t.dynamic != leaf->get_is_dynamic()) fail("cfg_tree.inc dynamic flag of " + std::string(t.key));
        if (t.def && leaf->def_to_string() != t.def) fail("cfg_tree.inc default of " + std::string(t.key) + ": " + t.def + " vs " + leaf->def_to_string());
        if (t.type == "int" || t.type == "uint")
        {
            const auto range = leaf->to_list();
            if (range.size() != 2 || cb::detail::parse_double(range[0]).value_or(-1) != t.min || cb::detail::parse_double(range[1]).value_or(-1) != t.max)
                fail("cfg_tree.inc range of " + std::string(t.key));
        }
    }
    // the random defaults are marked
    CHECK(cat::find("System > System Name")->flags & cat::flag::no_default);
    CHECK(cat::find("System > Console PSID")->flags & cat::flag::no_default);
    if (g_verbose) std::printf("coverage: %zu settings, %zu catalog rows, %zu value labels\n", leaves.size(), cat::item_count, cat::value_labels().size());
}

// ---------------------------------------------------------------- 2 classes

void test_classes()
{
    reset_files();
    cfg_root c;
    // every console limit of the policy is unavailable (shown greyed, never edited)
    for (const unsafe_option& u : unsafe_options)
    {
        const std::string key = cb::key_of(u.setting(c));
        const cat::item* it = cat::find(key);
        CHECK(it && it->kind == cat::klass::unavailable);
    }
    // the 4 settings the PS4 keeps at one value, with their reasons
    const std::pair<std::string_view, str> limits[] = {{"Miscellaneous > Exit RPCS3 when process finishes", str::CFGR_LIMIT_AUTOEXIT},
        {"Core > Assume External Debugger", str::CFGR_LIMIT_EXTERNAL_DEBUGGER}, {"VFS > Initialize Directories", str::CFGR_LIMIT_INIT_DIRS},
        {"Miscellaneous > Automatically start games after boot", str::CFGR_LIMIT_AUTOSTART}};
    for (const auto& [key, reason] : limits)
    {
        const cat::item* it = cat::find(key);
        CHECK(it && it->kind == cat::klass::unavailable && it->reason == reason);
    }
    // the runtime section: /host_root/ and the cache switch unavailable, the rest value-limited or redirected
    CHECK(cat::find("VFS > Enable /host_root/")->kind == cat::klass::unavailable);
    CHECK(cat::find("VFS > Limit disk cache size")->kind == cat::klass::unavailable);
    CHECK(cat::find("VFS > Disk cache maximum size (MB)")->values == "1024..4096");
    CHECK(cat::find("Video > Vulkan > VRAM allocation limit (MB)")->values == "256..768");
    CHECK(cat::find("Core > Max LLVM Compile Threads")->values == "1..2");
    CHECK(cat::find("Video > Shader Compiler Threads")->values == "1..2");

    // a pin never passes a console limit, the runtime section or the thread limit
    fixture f;
    pin_set pins;
    pins.global = {"Video > Multithreaded RSX", "Core > Max LLVM Compile Threads", "Video > Vulkan > VRAM allocation limit (MB)", "VFS > Enable /host_root/"};
    f.open({}, "Video:\n  Multithreaded RSX: true\n  Vulkan:\n    VRAM allocation limit (MB): 4096\nCore:\n  Max LLVM Compile Threads: 0\nVFS:\n  Enable /host_root/: true\n", {}, pins);
    CHECK(f.value("Video > Multithreaded RSX") == "false");
    CHECK(f.value("Core > Max LLVM Compile Threads") == "2");
    CHECK(f.value("Video > Vulkan > VRAM allocation limit (MB)") == "768");
    CHECK(f.value("VFS > Enable /host_root/") == "false");
    CHECK(f.get("Video > Multithreaded RSX").locked);
    CHECK(f.get("Video > Multithreaded RSX").mark == cb::badge::ps4);
    // an unavailable row refuses edits and pins
    const std::string refused = f.m.step(f.row("Video > Multithreaded RSX"), 1);
    CHECK(contains(refused, T(str::COM_INDISPONIVEL_PS4)));
    CHECK(f.value("Video > Multithreaded RSX") == "false");
    f.m.toggle_pin(f.row("Video > Multithreaded RSX"));
    const auto saved = f.m.pins_to_save();
    CHECK(std::find(saved.begin(), saved.end(), "Video > Multithreaded RSX") == saved.end());
    CHECK(std::find(saved.begin(), saved.end(), "VFS > Enable /host_root/") == saved.end());
    // the runtime cap shows its reason; the saved dump keeps the player's value (the boot caps it again)
    CHECK(f.get("Video > Vulkan > VRAM allocation limit (MB)").mark == cb::badge::ps4);
    CHECK(contains(f.m.save_dump(), "VRAM allocation limit (MB): 4096"));
    CHECK(f.value("Video > Vulkan > VRAM allocation limit (MB)") == "768");
}

// ---------------------------------------------------------------- 3 round trips

bool sparse_yaml_round_trip_quotes();

void test_round_trips()
{
    reset_files();
    // every enum spelling of the tree parses back to itself
    {
        cfg_root c;
        for (cfg::_base* leaf : cb::leaves_of(c))
        {
            if (leaf->get_type() != cfg::type::_enum) continue;
            for (const std::string& spelling : leaf->to_list())
                if (!leaf->from_string(spelling) || leaf->to_string() != spelling) fail("enum spelling " + cb::key_of(*leaf) + " = " + spelling);
        }
    }
    fixture f;
    f.open(minecraft);
    // bool
    const std::string wcb = f.value("Video > Stretch To Display Area");
    f.m.step(f.row("Video > Stretch To Display Area"), 1);
    CHECK(f.value("Video > Stretch To Display Area") != wcb);
    // enum: Left/Right walk the allowed values
    CHECK(f.value("Video > Aspect ratio") == "16:9");
    f.m.step(f.row("Video > Aspect ratio"), 1);
    CHECK(f.value("Video > Aspect ratio") == "4:3");
    // int: step, coarse, clamp at the bounds; keyboard text out of range keeps the value
    CHECK(f.value("Audio > Master Volume") == "100");
    f.m.step(f.row("Audio > Master Volume"), 1);
    CHECK(f.value("Audio > Master Volume") == "105");
    f.m.step(f.row("Audio > Master Volume"), 1, true);
    CHECK(f.value("Audio > Master Volume") == "125");
    for (int n = 0; n < 20; ++n) f.m.step(f.row("Audio > Master Volume"), 1, true);
    CHECK(f.value("Audio > Master Volume") == "200");
    const std::string out_of_range = f.m.set_text(f.row("Audio > Master Volume"), "500");
    CHECK(contains(out_of_range, "0..200"));
    CHECK(f.value("Audio > Master Volume") == "200");
    CHECK(f.m.set_text(f.row("Audio > Master Volume"), "abc") == T(str::CFGU_BAD_NUMBER));
    f.m.set_text(f.row("Audio > Master Volume"), "42");
    CHECK(f.value("Audio > Master Volume") == "42");
    // int with a value rule: Max LLVM Compile Threads stays in 1..2, 0 and 3 refused with the reason
    f.m.step(f.row("Core > Max LLVM Compile Threads"), 1);
    CHECK(f.value("Core > Max LLVM Compile Threads") == "2");
    f.m.step(f.row("Core > Max LLVM Compile Threads"), -1);
    CHECK(f.value("Core > Max LLVM Compile Threads") == "1");
    f.m.step(f.row("Core > Max LLVM Compile Threads"), -1);
    CHECK(f.value("Core > Max LLVM Compile Threads") == "1");
    CHECK(contains(f.m.set_text(f.row("Core > Max LLVM Compile Threads"), "0"), T(str::CFGR_LIMIT_2_THREADS)));
    CHECK(f.value("Core > Max LLVM Compile Threads") == "1");
    // a listed number rule walks the list (Anisotropic Filter 0, 2, 4, 8, 16)
    f.m.step(f.row("Video > Anisotropic Filter Override"), 1);
    CHECK(f.value("Video > Anisotropic Filter Override") == "2");
    f.m.step(f.row("Video > Anisotropic Filter Override"), 1);
    CHECK(f.value("Video > Anisotropic Filter Override") == "4");
    // float: 0.25 steps, decimal comma from the keyboard, %.17g text, the value column trims zeros
    f.m.step(f.row("Video > Texture LOD Bias Addend"), 1);
    CHECK(f.value("Video > Texture LOD Bias Addend") == "0.25");
    f.m.set_text(f.row("Video > Texture LOD Bias Addend"), "0,5");
    CHECK(f.value("Video > Texture LOD Bias Addend") == "0.5");
    CHECK(f.get("Video > Texture LOD Bias Addend").value == "0.5");
    f.m.set_text(f.row("Video > Texture LOD Bias Addend"), "-1.75");
    CHECK(f.value("Video > Texture LOD Bias Addend") == "-1.75");
    CHECK(contains(f.m.set_text(f.row("Video > Texture LOD Bias Addend"), "40"), "-32..32"));
    // uint64 above 2^63 (hex keyboard), uint128 PSID
    f.confirmed("Core > SPU LLVM Upper Bound", [&](std::size_t i) { return f.m.set_text(i, "0xffffffffffffffff"); });
    CHECK(f.value("Core > SPU LLVM Upper Bound") == "18446744073709551615");
    CHECK(f.get("Core > SPU LLVM Upper Bound").value == "0xFFFFFFFFFFFFFFFF");
    f.confirmed("Core > SPU LLVM Lower Bound", [&](std::size_t i) { return f.m.set_text(i, "1F00"); });
    CHECK(f.value("Core > SPU LLVM Lower Bound") == "7936");
    f.confirmed("System > Console PSID", [&](std::size_t i) { return f.m.set_text(i, "0x00112233445566778899AABBCCDDEEFF"); });
    CHECK(f.value("System > Console PSID") == "0x00112233445566778899AABBCCDDEEFF");
    CHECK(f.m.set_text(f.row("System > Console PSID"), "xyz") == T(str::CFGU_BAD_NUMBER));
    // strings with validators
    f.m.set_text(f.row("Net > DNS address"), "8.8.4.4");
    CHECK(f.value("Net > DNS address") == "8.8.4.4");
    CHECK(contains(f.m.set_text(f.row("Net > DNS address"), "999.1.1.1"), T(str::CFGU_BAD_IPV4)));
    CHECK(f.value("Net > DNS address") == "8.8.4.4");
    f.m.set_text(f.row("Net > IP swap list"), "1.2.3.4=5.6.7.8&&9.9.9.9=1.1.1.1");
    CHECK(f.value("Net > IP swap list") == "1.2.3.4=5.6.7.8&&9.9.9.9=1.1.1.1");
    CHECK(contains(f.m.set_text(f.row("Net > IP swap list"), "1.2.3.4"), T(str::CFGU_BAD_SWAP_LIST)));
    f.m.step(f.row("Video > Performance Overlay > Enabled"), 1);
    f.m.set_text(f.row("Video > Performance Overlay > Body Color (hex)"), "#11223344");
    CHECK(f.value("Video > Performance Overlay > Body Color (hex)") == "#11223344");
    CHECK(contains(f.m.set_text(f.row("Video > Performance Overlay > Body Color (hex)"), "red"), T(str::CFGU_BAD_COLOR)));
    f.m.set_text(f.row("System > HDD Model Name"), "TOSHIBA \"MK\": 80GB");
    CHECK(f.value("System > HDD Model Name") == "TOSHIBA \"MK\": 80GB");
    // Libraries Control: an LLE library checked forces HLE and back; an HLE one forces LLE
    f.confirmed("Core > Libraries Control", [&](std::size_t) { return f.m.toggle_library("libfs.sprx"); });
    CHECK(f.cfg.core.libraries_control.get_set().count("libfs.sprx:hle"));
    f.m.toggle_library("libadec.sprx");
    CHECK(f.cfg.core.libraries_control.get_set().count("libadec.sprx:lle"));
    f.m.toggle_library("libfs.sprx");
    CHECK(!f.cfg.core.libraries_control.get_set().count("libfs.sprx:hle"));
    {
        const auto libs = f.m.libraries();
        CHECK(libs.size() == g_prx_list.size());
        bool adec = false, dev = false;
        for (const cb::library_row& l : libs)
        {
            if (l.name == "libadec.sprx") adec = l.checked && l.default_hle;
            if (l.name == "libsysutil_np.sprx") dev = l.dev_only;
        }
        CHECK(adec && dev);
    }
    CHECK(f.get("Core > Libraries Control").value == i18n::trf(str::CFGU_ITEMS, {1}));
    // log levels: Notice is the default and leaves no entry
    f.confirmed("Log", [&](std::size_t) { return f.m.step_log("RSX", 1); });
    CHECK(f.cfg.log.get_map().count("RSX") && f.cfg.log.get_map().at("RSX") == logs::level::trace);
    f.m.step_log("RSX", -1);
    CHECK(!f.cfg.log.get_map().count("RSX"));
    f.m.step_log("PPU", -1);
    CHECK(f.cfg.log.get_map().at("PPU") == logs::level::warning);
    CHECK(f.m.log_levels().size() == logs::get_channels().size());
    // anaglyph matrix (a node map of "%f" texts), shown with 3D on and the custom anaglyph
    CHECK(contains(f.m.step_matrix("Video > Custom Anaglyph Matrices > Matrix Left", 4, 1), T(str::CFGL_VIDEO_STEREO_ENABLED)));
    f.m.step(f.row("Video > 3D Display Enabled"), 1);
    f.pick("Video > 3D Display Mode", "Anaglyph Custom");
    CHECK(!f.get("Video > Custom Anaglyph Matrices > Matrix Left").dimmed);
    f.m.step_matrix("Video > Custom Anaglyph Matrices > Matrix Left", 4, 1);
    CHECK(f.cfg.video.custom_anaglyph_matrices.left.get_map().count("11") && f.cfg.video.custom_anaglyph_matrices.left.get_map().at("11") == "0.050000");
    f.m.set_matrix_text("Video > Custom Anaglyph Matrices > Matrix Left", 0, "0,25");
    CHECK(f.m.matrix("Video > Custom Anaglyph Matrices > Matrix Left")[0] == 0.25);
    // audio format bits (the first one is always on), with the Manual format
    f.pick("Audio > Audio Format", "Manual");
    f.m.toggle_audio_format(4);
    CHECK(f.value("Audio > Audio Formats") == "4");
    CHECK(f.m.audio_formats().size() == 5 && f.m.audio_formats()[0].second && f.m.audio_formats()[3].second);
    // microphone and MIDI slots
    f.m.set_microphone_devices({"USB Mic"});
    f.m.set_midi_devices({"USB MIDI keyboard"});
    f.confirmed("Audio > Microphone Type", [&](std::size_t i) { return f.m.step(i, 1); });
    f.confirmed("Audio > Microphone Devices", [&](std::size_t) { return f.m.step_slot("Audio > Microphone Devices", 0, 1, 1); });
    CHECK(f.value("Audio > Microphone Devices") == "USB Mic@@@@@@@@@@@@");
    const std::string midi_before = f.value("Input/Output > Emulated Midi devices");
    f.confirmed("Input/Output > Emulated Midi devices", [&](std::size_t) { return f.m.step_slot("Input/Output > Emulated Midi devices", 1, 0, 1); });
    if (f.m.slots("Input/Output > Emulated Midi devices")[1].type != "Drums")
        fail("MIDI slot type: '" + midi_before + "' -> '" + f.value("Input/Output > Emulated Midi devices") + "'");
    f.m.step_slot("Input/Output > Emulated Midi devices", 2, 1, 1);
    CHECK(f.m.slots("Input/Output > Emulated Midi devices")[2].device == "USB MIDI keyboard");
    // the country list
    f.m.pick(f.row("Net > PSN Country"), 2);
    CHECK(f.value("Net > PSN Country") == "br");
    CHECK(f.get("Net > PSN Country").value == "Brazil (br)");

    // staged -> sparse YAML -> a fresh tree over the global values: every key equal
    const std::vector<std::string> pins = f.m.pins_to_save();
    const std::string sparse = f.m.sparse_yaml();
    if (g_verbose) std::printf("sparse YAML (%zu pins):\n%s", pins.size(), sparse.c_str());
    cfg_root fresh;
    CHECK(fresh.from_string(f.baseline));
    CHECK(fresh.from_string(sparse));
    for (const std::string& key : pins)
    {
        const cfg::_base* a = cb::find(f.cfg, key);
        const cfg::_base* b = cb::find(fresh, key);
        if (!a || !b || cb::detail::canonical(*a) != cb::detail::canonical(*b)) fail("sparse YAML round trip of " + key);
    }
    // (SPU LLVM Upper Bound went back to its default, which is the automatic value: no pin)
    CHECK(std::find(pins.begin(), pins.end(), "Core > SPU LLVM Upper Bound") == pins.end());
    for (const std::string_view key : {"Video > Texture LOD Bias Addend", "Core > SPU LLVM Lower Bound", "System > Console PSID", "Net > DNS address", "Log",
             "Core > Libraries Control", "System > HDD Model Name", "Video > Custom Anaglyph Matrices > Matrix Left", "Input/Output > Emulated Midi devices"})
        if (std::find(pins.begin(), pins.end(), key) == pins.end()) fail("not a pin after the edits: " + std::string(key));
    // a key the player did not choose is not in the file and follows the global value
    CHECK(!contains(sparse, "Write Color Buffers"));
    CHECK(sparse_yaml_round_trip_quotes());
}

// Quoted scalars of the sparse writer read back by cfg (values with '@', '#', '"', ': ' and an empty one)
bool sparse_yaml_round_trip_quotes()
{
    cfg_root a;
    a.audio.audio_device.from_string("@@@default@@@");
    a.video.perf_overlay.color_body.from_string("#FFE138FF");
    a.misc.title_format.from_string("FPS: %F | \"x\" # y");
    a.net.swap_list.from_string("");
    const std::string text = cb::sparse_yaml_for(a, {"Audio > Audio Device", "Video > Performance Overlay > Body Color (hex)", "Miscellaneous > Window Title Format",
        "Net > IP swap list"});
    cfg_root b;
    b.audio.audio_device.from_string("x");
    b.net.swap_list.from_string("y");
    if (!b.from_string(text)) return false;
    return b.audio.audio_device.to_string() == "@@@default@@@" && b.video.perf_overlay.color_body.to_string() == "#FFE138FF" &&
        b.misc.title_format.to_string() == "FPS: %F | \"x\" # y" && b.net.swap_list.to_string().empty();
}

// ---------------------------------------------------------------- 4 badges

void test_badges()
{
    reset_files();
    // the automatic profile (global scope): a saved 34 ms reserve runs as 64
    {
        fixture f;
        f.open({}, "Audio:\n  Desired Audio Buffer Duration: 34\n");
        CHECK(f.value("Audio > Desired Audio Buffer Duration") == "64");
        const cb::row& r = f.get("Audio > Desired Audio Buffer Duration");
        CHECK(r.mark == cb::badge::automatic && r.badge_text == T(str::CFGU_BADGE_AUTO));
        CHECK(contains(r.help, "64 ms") && contains(r.help, "34 ms"));
        // Left/Right changes and keeps the player's value (a pin at save)
        f.m.step(f.row("Audio > Desired Audio Buffer Duration"), 1);
        CHECK(f.value("Audio > Desired Audio Buffer Duration") == "66");
        CHECK(f.get("Audio > Desired Audio Buffer Duration").mark == cb::badge::mine);
        const auto pins = f.m.pins_to_save();
        CHECK(std::find(pins.begin(), pins.end(), "Audio > Desired Audio Buffer Duration") != pins.end());
    }
    // the title's row (Demon's Souls Write Color Buffers)
    {
        fixture f;
        f.open(demons_souls);
        CHECK(f.value("Video > Write Color Buffers") == "true");
        CHECK(f.get("Video > Write Color Buffers").mark == cb::badge::game);
    }
    // the title's PC config database entry
    {
        write_database(database_game, "Video:\n  Strict Rendering Mode: true\n");
        fixture f;
        f.open(database_game);
        CHECK(f.value("Video > Strict Rendering Mode") == "true");
        CHECK(f.get("Video > Strict Rendering Mode").mark == cb::badge::pc);
        reset_files();
    }
    // the PS4 switches: online on -> Internet connected, the row follows the switch and redirects edits
    {
        fixture f;
        runtime_inputs in;
        in.online = true;
        f.open(minecraft, {}, {}, {}, in);
        CHECK(f.value("Net > Internet enabled") == "Connected");
        const cb::row& r = f.get("Net > Internet enabled");
        CHECK(r.mark == cb::badge::switch_ && r.redirect == cb::ps4_switch::online);
        CHECK(contains(f.m.step(f.row("Net > Internet enabled"), 1), T(str::CFGU_SWITCH_ONLINE)));
        CHECK(f.value("Net > Internet enabled") == "Connected");
        CHECK(f.get("System > Language").redirect == cb::ps4_switch::game_language);
    }
    // the adaptation after a thread refusal
    {
        fixture f;
        runtime_inputs in;
        in.adapt = {std::string(adapt::key_threads_low)};
        f.open(minecraft, {}, {}, {}, in);
        CHECK(f.value("Core > Max LLVM Compile Threads") == "1");
        CHECK(f.get("Core > Max LLVM Compile Threads").mark == cb::badge::adapt);
    }
    // the player's pin keeps a profile value off the profile; a value of this game that differs from the global one
    {
        fixture f;
        pin_set pins;
        pins.game = {"Video > Write Color Buffers"};
        f.open(minecraft, {}, "Video:\n  Write Color Buffers: true\nMiscellaneous:\n  Show trophy popups: false\n", pins);
        CHECK(f.value("Video > Write Color Buffers") == "true");
        CHECK(f.get("Video > Write Color Buffers").mark == cb::badge::mine);
        const cb::row& t = f.get("Miscellaneous > Show trophy popups");
        CHECK(t.mark == cb::badge::this_game && t.differs_global);
        CHECK(contains(t.help, T(str::COM_LIGADO)));
        // a format-1 value nobody pinned is not written as a pin; the file's pin stays
        const auto saved = f.m.pins_to_save();
        CHECK(saved.size() == 1 && saved[0] == "Video > Write Color Buffers");
    }
    // without the pin the profile wins (badge automatic)
    {
        fixture f;
        f.open(minecraft, {}, "Video:\n  Write Color Buffers: true\n");
        CHECK(f.value("Video > Write Color Buffers") == "false");
        CHECK(f.get("Video > Write Color Buffers").mark == cb::badge::automatic);
    }
    // DFMA (no FMA on Jaguar): forced off in the global menu; per game a chosen 'on' runs (Salvar writes the marker)
    {
        fixture g;
        g.open({});
        g.confirmed("Core > Use Accurate DFMA", [&](std::size_t i) { return g.m.step(i, 1); });
        CHECK(g.value("Core > Use Accurate DFMA") == "false");
        CHECK(g.get("Core > Use Accurate DFMA").mark == cb::badge::ps4);
        fixture f;
        f.open(minecraft);
        f.confirmed("Core > Use Accurate DFMA", [&](std::size_t i) { return f.m.step(i, 1); });
        CHECK(f.value("Core > Use Accurate DFMA") == "true");
        CHECK(f.get("Core > Use Accurate DFMA").help == T(str::SET_SEM_FMA_JAGUAR_SALVAR_LIGA));
        const auto pins = f.m.pins_to_save();
        CHECK(std::find(pins.begin(), pins.end(), "Core > Use Accurate DFMA") != pins.end());
    }
    // keep-user-settings.flag: nothing forced; the game's value only differs from the global one
    {
        fs::test_files()[keep_user_settings_flag] = "";
        fixture f;
        f.open(minecraft, {}, "Video:\n  Write Color Buffers: true\n");
        CHECK(f.value("Video > Write Color Buffers") == "true");
        CHECK(f.get("Video > Write Color Buffers").mark == cb::badge::this_game);
        CHECK(f.get("Video > Strict Rendering Mode").mark == cb::badge::none);
        reset_files();
    }
}

// ---------------------------------------------------------------- 5 resets

void test_resets()
{
    reset_files();
    // Square: the automatic value (2 compile threads, not the RPCS3 default 0) and no pin
    {
        fixture f;
        f.open({});
        f.m.step(f.row("Core > Max LLVM Compile Threads"), -1);
        CHECK(f.value("Core > Max LLVM Compile Threads") == "1");
        f.m.reset_auto(f.row("Core > Max LLVM Compile Threads"));
        CHECK(f.value("Core > Max LLVM Compile Threads") == "2");
        const auto pins = f.m.pins_to_save();
        CHECK(std::find(pins.begin(), pins.end(), "Core > Max LLVM Compile Threads") == pins.end());
        // a value equal to the automatic one is no pin either
        f.m.step(f.row("Audio > Master Volume"), 1);
        f.m.step(f.row("Audio > Master Volume"), -1);
        const auto pins2 = f.m.pins_to_save();
        CHECK(std::find(pins2.begin(), pins2.end(), "Audio > Master Volume") == pins2.end());
        // Options > Keep this value pins it anyway; Unpin drops it
        f.m.toggle_pin(f.row("Audio > Master Volume"));
        const auto pins3 = f.m.pins_to_save();
        CHECK(std::find(pins3.begin(), pins3.end(), "Audio > Master Volume") != pins3.end());
        f.m.toggle_pin(f.row("Audio > Master Volume"));
        const auto pins4 = f.m.pins_to_save();
        CHECK(std::find(pins4.begin(), pins4.end(), "Audio > Master Volume") == pins4.end());
    }
    // RPCS3 default; the random identities never reset
    {
        fixture f;
        f.open({}, "Miscellaneous:\n  Show trophy popups: false\nSystem:\n  System Name: MyPS3\n");
        f.m.reset_upstream(f.row("Miscellaneous > Show trophy popups"));
        CHECK(f.value("Miscellaneous > Show trophy popups") == "true");
        CHECK(f.m.reset_upstream(f.row("System > System Name")) == T(str::CFGU_NO_DEFAULT));
        CHECK(f.value("System > System Name") == "MyPS3");
        f.m.reset_auto(f.row("System > System Name"));
        CHECK(f.value("System > System Name") == "MyPS3");
        const std::string psid = f.value("System > Console PSID");
        CHECK(f.m.reset_upstream(f.row("System > Console PSID")) == T(str::CFGU_NO_DEFAULT));
        CHECK(f.value("System > Console PSID") == psid);
    }
    // per game: the global value, and Square = the global value with the pin removed
    {
        fixture f;
        pin_set pins;
        pins.game = {"Miscellaneous > Show trophy popups", "Audio > Master Volume"};
        f.open(minecraft, "Audio:\n  Master Volume: 80\n", "Miscellaneous:\n  Show trophy popups: false\nAudio:\n  Master Volume: 50\n", pins);
        CHECK(f.value("Audio > Master Volume") == "50");
        f.m.use_global(f.row("Audio > Master Volume"));
        CHECK(f.value("Audio > Master Volume") == "80");
        f.m.reset_auto(f.row("Miscellaneous > Show trophy popups"));
        CHECK(f.value("Miscellaneous > Show trophy popups") == "true");
        CHECK(f.m.pins_to_save().empty());
        CHECK(f.m.sparse_yaml().empty());
    }
    // a format-1 per-game file converted: the values that differ from the automatic configuration become pins
    {
        fixture f;
        f.open(minecraft, {}, "Video:\n  Write Color Buffers: true\n  Strict Rendering Mode: false\nAudio:\n  Master Volume: 90\n  Desired Audio Buffer Duration: 34\n");
        const auto differ = f.m.differing_from_automatic();
        const auto has = [&](std::string_view key) { return std::find(differ.begin(), differ.end(), key) != differ.end(); };
        CHECK(has("Video > Write Color Buffers") && has("Audio > Master Volume") && has("Audio > Desired Audio Buffer Duration"));
        CHECK(!has("Video > Strict Rendering Mode") && !has("Video > Renderer"));
        CHECK(f.value("Video > Write Color Buffers") == "false");
        f.m.pin_keys({"Video > Write Color Buffers", "Audio > Master Volume"});
        CHECK(f.value("Video > Write Color Buffers") == "true");
        CHECK(f.value("Audio > Master Volume") == "90");
        CHECK(f.value("Audio > Desired Audio Buffer Duration") == "64");
        const auto pins = f.m.pins_to_save();
        CHECK(pins.size() == 2);
        // Voltar para automático: the pins go, the profile applies again
        f.m.clear_scope_choices();
        CHECK(f.value("Video > Write Color Buffers") == "false");
        // Restaurar valores padrão: every setting automatic, nothing pinned
        f.m.reset_all();
        CHECK(f.value("Audio > Master Volume") == "100");
        CHECK(f.m.pins_to_save().empty());
    }
}

// ---------------------------------------------------------------- 6 dependencies

void test_dependencies()
{
    reset_files();
    fixture f;
    f.open(minecraft);
    // OpenGL: Shader Mode legacy without gl-async.flag, the relaxed ZCULL pair
    f.m.pick(f.row("Video > Renderer"), 1); // Null, OpenGL, Vulkan
    CHECK(f.value("Video > Renderer") == "OpenGL");
    CHECK(f.value("Video > Shader Mode") == "Legacy Recompiler (single-threaded)");
    CHECK(f.value("Video > Relaxed ZCULL Sync") == "true" && f.value("Video > Accurate ZCULL stats") == "false");
    CHECK(f.get("Video > Relaxed ZCULL Sync").value == T(str::CFGU_ZCULL_RELAXED));
    {
        const auto list = f.m.choices(f.row("Video > Shader Mode"));
        bool async_greyed = false;
        for (const cb::choice& c : list)
            if (c.spelling == "Async Recompiler (multi-threaded)") async_greyed = !c.allowed && c.reason == T(str::CFGR_VALUE_GL_ASYNC);
        CHECK(async_greyed);
    }
    fs::test_files()[gl_async_flag] = "";
    {
        const auto list = f.m.choices(f.row("Video > Shader Mode"));
        bool async_allowed = false;
        for (const cb::choice& c : list)
            if (c.spelling == "Async Recompiler (multi-threaded)") async_allowed = c.allowed;
        CHECK(async_allowed);
    }
    fs::test_files().erase(gl_async_flag);
    // the ZCULL composite writes both settings
    f.m.pick(f.row("Video > Relaxed ZCULL Sync"), 0);
    CHECK(f.value("Video > Relaxed ZCULL Sync") == "false" && f.value("Video > Accurate ZCULL stats") == "true");
    // back on Vulkan: the profile's shader mode
    f.m.pick(f.row("Video > Renderer"), 2);
    CHECK(f.value("Video > Shader Mode") == "Async Recompiler with Shader Interpreter");
    // the FSR sharpening row is dimmed until FidelityFX Super Resolution is chosen
    CHECK(f.get("Video > FidelityFX CAS Sharpening Intensity").dimmed);
    CHECK(contains(f.m.step(f.row("Video > FidelityFX CAS Sharpening Intensity"), 1), T(str::CFGL_VIDEO_OUTPUT_SCALING)));
    f.m.pick(f.row("Video > Output Scaling Mode"), 2);
    CHECK(f.value("Video > Output Scaling Mode") == "FidelityFX Super Resolution");
    CHECK(!f.get("Video > FidelityFX CAS Sharpening Intensity").dimmed);
    // 3D rows: per game and only at 1280x720
    CHECK(!f.get("Video > 3D Display Enabled").dimmed);
    f.confirmed("Video > Resolution", [&](std::size_t i) { return f.m.pick(i, 0); }); // 1920x1080: a warned value
    CHECK(f.value("Video > Resolution") == "1920x1080");
    CHECK(f.get("Video > 3D Display Enabled").dimmed);
    // per-game-only rows are dimmed in the global menu
    fixture g;
    g.open({});
    CHECK(g.get("Video > Vblank Rate").dimmed);
    CHECK(g.get("Video > Vblank Rate").help == T(str::CFGU_NOTE_GAME_ONLY));
    CHECK(g.m.step(g.row("Video > Vblank Rate"), 1) == T(str::CFGU_NOTE_GAME_ONLY));
    CHECK(g.get("Video > 3D Display Enabled").dimmed);
    // and the global-only disk cache is dimmed per game
    CHECK(f.get("VFS > Disk cache maximum size (MB)").dimmed);
    CHECK(!g.get("VFS > Disk cache maximum size (MB)").dimmed);
    // PSN 'Simulated' only per game
    {
        bool greyed = false;
        for (const cb::choice& c : g.m.choices(g.row("Net > PSN status")))
            if (c.spelling == "Simulated") greyed = !c.allowed;
        CHECK(greyed);
    }
}

// ---------------------------------------------------------------- pages, confirmations, snapshots, self-check

void test_pages()
{
    reset_files();
    fixture f;
    f.open(minecraft);
    std::size_t visible = 0, hidden = 0;
    for (std::size_t t = 0; t + 1 < cat::tab_count; ++t)
    {
        const auto rows = f.m.page(cat::tab(t));
        CHECK(!rows.empty() && rows[0].section);
        for (const cb::row& r : rows)
        {
            if (r.section) continue;
            visible++;
            CHECK(!r.label.empty() && !r.help.empty());
            CHECK(r.kind != cat::klass::hidden);
        }
        for (const cb::row& r : f.m.page(cat::tab(t), true))
            if (!r.section && r.kind == cat::klass::hidden) hidden++;
        CHECK(!f.m.tab_summary(cat::tab(t)).empty());
    }
    // the ZCULL member has no row of its own
    std::size_t members = 0;
    for (const cat::item& it : cat::items()) members += it.edit == cat::editor::zcull_member;
    CHECK(members == 1);
    CHECK(visible + hidden + members == cat::item_count);
    if (g_verbose) std::printf("pages: %zu visible rows, %zu hidden rows\n", visible, hidden);
    CHECK(f.m.page(cat::tab::ps4).empty());
    // a hidden row is read-only with its reason
    const cb::row& h = f.get("Miscellaneous > Enable GameMode");
    CHECK(h.locked && contains(h.help, T(str::CFGR_DESKTOP)));
    CHECK(contains(f.m.step(f.row("Miscellaneous > Enable GameMode"), 1), T(str::CFGR_DESKTOP)));
    // the notes of the dynamic settings
    CHECK(f.get("Audio > Master Volume").note == T(str::CFGU_NOTE_HOME_MENU));
    CHECK(f.get("Video > Handle RSX Memory Tiling").note == T(str::CFGU_NOTE_HOME_MENU));
    CHECK(f.get("Video > Renderer").note.empty());
    // keyboards
    std::string title, initial;
    CHECK(f.m.keyboard(f.row("Net > DNS address"), title, initial) == cb::keyboard_kind::text && initial == "8.8.8.8");
    CHECK(f.m.keyboard(f.row("Video > Texture LOD Bias Addend"), title, initial) == cb::keyboard_kind::number && initial == "0");
    CHECK(f.m.keyboard(f.row("System > Console PSID"), title, initial) == cb::keyboard_kind::hex);
    CHECK(f.m.keyboard(f.row("System > Console time offset (s)"), title, initial) == cb::keyboard_kind::date && initial.size() == 16);
    CHECK(f.m.full_help(f.row("Video > Write Color Buffers")) == T(str::CFGTIP_VIDEO_WRITE_COLOR_BUFFERS));
    CHECK(f.m.yaml_key(f.row("Video > Write Color Buffers")) == "Video > Write Color Buffers");

    // a warn row asks once per session
    const std::string asked = f.m.step(f.row("Core > Accurate SPU Reservations"), 1);
    CHECK(contains(asked, T(str::CFGU_WARN_TITLE)) && contains(asked, T(str::CFGR_ACCURATE_RES)));
    CHECK(f.m.confirmation().has_value());
    CHECK(f.value("Core > Accurate SPU Reservations") == "true");
    f.m.confirm();
    f.m.step(f.row("Core > Accurate SPU Reservations"), 1);
    CHECK(f.value("Core > Accurate SPU Reservations") == "false");
    CHECK(!f.m.confirmation());
    f.m.step(f.row("Core > Accurate SPU Reservations"), 1);
    CHECK(f.value("Core > Accurate SPU Reservations") == "true");
    // a warned value asks too (the PPU interpreter)
    std::size_t interpreter = 0;
    const auto ppu = f.m.choices(f.row("Core > PPU Decoder"));
    for (std::size_t c = 0; c < ppu.size(); ++c)
        if (ppu[c].spelling == "Interpreter (static)") interpreter = c;
    CHECK(ppu[interpreter].warn && ppu[interpreter].reason == T(str::CFGR_VALUE_INTERPRETER));
    CHECK(contains(f.m.pick(f.row("Core > PPU Decoder"), interpreter), T(str::CFGR_VALUE_INTERPRETER)));
    CHECK(f.value("Core > PPU Decoder") == "Recompiler (LLVM)");
    f.m.confirm();
    f.m.pick(f.row("Core > PPU Decoder"), interpreter);
    CHECK(f.value("Core > PPU Decoder") == "Interpreter (static)");
    // the hidden SPU interpreter is not offered
    for (const cb::choice& c : f.m.choices(f.row("Core > SPU Decoder"))) CHECK(c.spelling != "Interpreter (dynamic)");
    CHECK(f.m.pending() > 0);

    // the home menu: the dynamic settings that changed while it was open
    cfg_root live;
    const std::string snapshot = cb::dynamic_snapshot(live);
    live.audio.volume.set(70);
    live.video.renderer.set(video_renderer::opengl); // not dynamic: ignored
    live.video.perf_overlay.margin_x.set(1.5);
    const auto changes = cb::dynamic_changes(live, snapshot);
    CHECK(changes.size() == 2 && std::find(changes.begin(), changes.end(), "Audio > Master Volume") != changes.end());
    const std::string merged = cb::sparse_yaml_for(live, changes);
    CHECK(contains(merged, "Master Volume: 70") && !contains(merged, "Renderer"));

    // the console self-check
    cfg_root c;
    const cb::self_check_result check = cb::model::self_check(c);
    CHECK(check.settings == cat::item_count && check.uncatalogued == 0 && check.round_trip_failures == 0);
    for (const std::string& p : check.problems) fail(p);
}

// The page in every interface language: texts change with the language, the Arabic rows hold Arabic letters
void test_languages()
{
    reset_files();
    fixture f;
    f.open({});
    const std::string pt = f.get("Video > Write Color Buffers").label;
    i18n::apply(i18n::setting::en, -1);
    CHECK(f.get("Video > Write Color Buffers").label == "Write Color Buffers");
    CHECK(f.get("Video > Renderer").value == "Vulkan");
    i18n::apply(i18n::setting::ar, -1);
    CHECK(utils::ps4::arabic::has_arabic(f.get("Video > Write Color Buffers").label));
    i18n::apply(i18n::setting::automatic, -1);
    CHECK(f.get("Video > Write Color Buffers").label == pt);
}
} // namespace

int main(int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
        if (std::string_view(argv[i]) == "--verbose") g_verbose = true;
    test_coverage();
    test_classes();
    test_round_trips();
    test_badges();
    test_resets();
    test_dependencies();
    test_pages();
    test_languages();
    std::printf("cfg_browser_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
