#pragma once

// Catalog of the settings browser: one row per setting of the RPCS3 config tree (cfg_root, Emu/system_config.h) with
// where the PS4 browser shows it (tab and section, in the PC dialog's order), how far a player may change it (class),
// its texts (i18n ids of ps4_i18n_cfg.inc) and its editor hints. Data only, standard library + ps4_i18n.hpp, so host
// tests and tools can include it. The rows are in ps4_cfg_catalog.inc (see the format there);
// ps4-port/tools/gen_cfg_catalog.py --check and ps4-port/tests/cfg_browser_test.cpp keep them in step with the tree.

#include "util/ps4_i18n.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace utils::ps4::cfg_catalog
{
// The 9 tabs of the PC dialog that hold settings, then the port's own switches
enum class tab : std::uint8_t { cpu, gpu, audio, io, system, network, advanced, emulator, debug, ps4, count_ };
inline constexpr std::size_t tab_count = std::size_t(tab::count_);

// expose: edited freely (console limits still clamp); warn: the first change asks for a confirmation with 'reason';
// unavailable: shown greyed with 'reason', never edited; hidden: no meaning on this console, listed only with
// "Mostrar opções ocultas" (read-only)
enum class klass : std::uint8_t { expose, warn, unavailable, hidden };

// How the value is edited; auto_ = from the cfg type (toggle, enum list, stepper, text)
enum class editor : std::uint8_t
{
    auto_,
    zcull,          // 'ZCULL Accuracy': Precise / Approximate / Relaxed over the two ZCULL settings
    zcull_member,   // edited by the zcull row (Accurate ZCULL stats), no row of its own
    mfc_delay,      // 'Delay Each Odd MFC Command': on = 1, off = 0
    audio_formats,  // checkable list over the audio format bits
    time_offset,    // console time offset shown as the console date and time
    hex,            // keyboard in hexadecimal (64-bit bounds, PSID)
    text,           // keyboard, any text
    ipv4,           // keyboard, an IPv4 address
    swap_list,      // keyboard, 'ip=ip&&ip=ip'
    color,          // keyboard, '#RRGGBBAA'
    font,           // list of the installed fonts
    country,        // list of the PSN countries
    microphones,    // 4 microphone slots joined by '@@@'
    midi,           // 3 MIDI device slots
    libraries,      // Libraries Control: the firmware libraries with their LLE / HLE switch
    log_levels,     // the level of each log channel
    matrix,         // a 3x3 anaglyph matrix
    readonly,       // shown, never edited
};

// Where the PC dialog allows the setting: global only (disk cache) or per game only (game speed, 3D)
enum class scope : std::uint8_t { both, global_only, game_only };

enum flag : std::uint16_t
{
    none = 0,
    no_default = 1 << 0, // the default is random (System Name, Console PSID): never reset
    home_menu = 1 << 1,  // the in-game home menu (Options+Touchpad) edits it while a game runs
};

struct item
{
    std::string_view key;      // node names joined with " > "
    tab where;
    i18n::str section;         // CFGG_ caption
    klass kind;
    i18n::str label;           // CFGL_
    i18n::str help;            // CFGH_ (one XMB line)
    i18n::str reason;          // why the class is warn / unavailable / hidden (count_ = none)
    i18n::str tip;             // CFGTIP_: the PC tooltip (count_ = none)
    editor edit;
    double step, coarse;       // stepper steps (0 = 1 and 10)
    std::string_view unit;     // shown after a number
    // Value rules, '|' separated: "X" allowed, "~X" allowed with a warning, "-X" shown greyed, "#X" not offered; numbers
    // as "a..b". With a list, a value it does not name is greyed. Empty = every value of the type is allowed.
    std::string_view values;
    i18n::str value_reason;    // why a '~' or greyed value is (count_ = none)
    // Rows dimmed until every condition holds: "key=value[|value]" or "key!=value", '&' separated
    std::string_view depends;
    scope where_scope;
    std::uint16_t flags;
};

// A value's label: the PC dialog's text for a YAML spelling (an i18n id, or a text that is the same in every language)
struct value_label
{
    std::string_view key;
    std::string_view spelling;
    i18n::str label;           // count_ = use 'text'
    std::string_view text;
};

namespace detail
{
using enum tab;
using enum klass;
using enum editor;
using enum scope;
using i18n::str;
inline constexpr std::uint16_t F_NONE = flag::none, F_NO_DEFAULT = flag::no_default, F_HOME = flag::home_menu,
    F_NO_DEFAULT_HOME = flag::no_default | flag::home_menu;
inline constexpr klass E = expose, W = warn, U = unavailable, H = hidden;

inline constexpr item items[] = {
#define CFG_ITEM(key, tab_, section, kind, label, help, reason, tip, edit, step, coarse, unit, values, vreason, depends, scope_, flags) \
    {key, tab_, str::section, kind, str::label, str::help, str::reason, str::tip, edit, step, coarse, unit, values, str::vreason, depends, scope_, flags},
#define CFG_VALUE(key, spelling, label, text)
#include "ps4_cfg_catalog.inc"
#undef CFG_VALUE
#undef CFG_ITEM
};

inline constexpr value_label value_labels[] = {
#define CFG_ITEM(key, tab_, section, kind, label, help, reason, tip, edit, step, coarse, unit, values, vreason, depends, scope_, flags)
#define CFG_VALUE(key, spelling, label, text) {key, spelling, str::label, text},
#include "ps4_cfg_catalog.inc"
#undef CFG_VALUE
#undef CFG_ITEM
};
} // namespace detail

inline constexpr std::size_t item_count = std::size(detail::items);

inline constexpr std::span<const item> items() noexcept { return {detail::items, item_count}; }
inline constexpr std::span<const value_label> value_labels() noexcept { return {detail::value_labels, std::size(detail::value_labels)}; }

// Row positions sorted by key, built once (binary search by key)
inline const std::array<std::uint16_t, item_count>& sorted_index()
{
    static const std::array<std::uint16_t, item_count> index = []
    {
        std::array<std::uint16_t, item_count> out{};
        for (std::size_t i = 0; i < item_count; ++i) out[i] = std::uint16_t(i);
        std::sort(out.begin(), out.end(), [](std::uint16_t a, std::uint16_t b) { return detail::items[a].key < detail::items[b].key; });
        return out;
    }();
    return index;
}

// The catalog row of 'key', or null (a setting an upstream merge added)
inline const item* find(std::string_view key)
{
    const auto& index = sorted_index();
    const auto it = std::lower_bound(index.begin(), index.end(), key, [](std::uint16_t i, std::string_view k) { return detail::items[i].key < k; });
    return it != index.end() && detail::items[*it].key == key ? &detail::items[*it] : nullptr;
}

// The PC label of a value of 'key' (null when the spelling is shown as it is)
inline const value_label* find_value(std::string_view key, std::string_view spelling)
{
    for (const value_label& v : value_labels())
        if (v.key == key && v.spelling == spelling) return &v;
    return nullptr;
}

// Tab of a setting the catalog does not list yet, from its first node
inline tab default_tab(std::string_view key)
{
    const std::string_view node = key.substr(0, key.find(" > "));
    if (node == "Core") return tab::advanced;
    if (node == "Video") return tab::gpu;
    if (node == "Audio") return tab::audio;
    if (node == "Input/Output") return tab::io;
    if (node == "System" || node == "VFS") return tab::system;
    if (node == "Net") return tab::network;
    if (node == "Log") return tab::debug;
    return tab::emulator;
}

inline i18n::str tab_name(tab t)
{
    static constexpr i18n::str names[] = {i18n::str::CFGT_CPU, i18n::str::CFGT_GPU, i18n::str::CFGT_AUDIO, i18n::str::CFGT_IO,
        i18n::str::CFGT_SYSTEM, i18n::str::CFGT_NETWORK, i18n::str::CFGT_ADVANCED, i18n::str::CFGT_EMULATOR, i18n::str::CFGT_DEBUG,
        i18n::str::CFGT_PS4};
    return std::size_t(t) < std::size(names) ? names[std::size_t(t)] : i18n::str::count_;
}
} // namespace utils::ps4::cfg_catalog
