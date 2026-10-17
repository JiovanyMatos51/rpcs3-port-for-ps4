#pragma once

// Settings browser: every setting of the RPCS3 config tree (cfg_root), browsed by the PC dialog's tabs, edited with the
// cfg parsers, with the PS4 policy's say on each value (util/ps4_policy.hpp) and the player's own choices (pins).
//
// The model never touches g_cfg and constructs no cfg_root: it edits the staged copy the settings menu owns, so this
// header belongs to the launcher TU only (never include it from ps4_policy.hpp or System.cpp; see the ODR note there).
//
//   saved     the values the settings files hold (captured by bind())
//   chosen    a value the player set in this session (step, pick, keyboard, resets)
//   staged    = boot policy preview of (saved, then chosen) with the pins: the value column shows what will run
//   pins      'PS4 user settings:' of ps4-settings.yml (global) and of the game's YAML; the bound scope's list gains
//             the keys chosen in this session whose value differs from the automatic dry run (pins_to_save)
//
// Rows: page(tab) lists the catalog rows of a tab (util/ps4_cfg_catalog.hpp) with section captions, the value that
// runs, a badge (who decides it) and one help line, all in the interface language and computed once per change.
// A setting the catalog lacks (an upstream merge added it) is still shown in its node's tab, section "Outras", class W.

#include "Emu/system_config.h"
#include "Emu/NP/rpcn_countries.h"
#include "util/logs.hpp"
#include "util/ps4_cfg_catalog.hpp"
#include "util/ps4_i18n.hpp"
#include "util/ps4_policy.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Emu/Cell/lv2/sys_prx.cpp: the firmware libraries the loader knows; 1 = HLE by default, 0 = LLE by default
extern const std::map<std::string_view, int> g_prx_list;

namespace utils::ps4::cfg_browser
{
namespace cat = cfg_catalog;
using cat::editor;
using cat::klass;
using cat::tab;

// Who decides the value a row shows (the last policy step that changed it, or the player)
enum class origin : std::uint8_t { saved, profile, title_row, database, console, runtime, switch_, adapt, hardware, migration, pinned };
enum class badge : std::uint8_t { none, automatic, game, pc, ps4, switch_, adapt, mine, this_game };
// The PS4 tab row that decides a setting (the launcher moves the cursor there)
enum class ps4_switch : std::uint8_t { none, online, upnp, game_language, cool };
// What the keyboard edits for a row
enum class keyboard_kind : std::uint8_t { none, number, hex, text, date };
// The sub-page a row opens
enum class sub_page : std::uint8_t { none, libraries, log_levels, matrix, audio_formats, microphones, midi };

// Node names joined with " > " (the same key as ps4_policy.hpp setting_key)
inline std::string key_of(const cfg::_base& leaf)
{
    std::string key = leaf.get_name();
    for (const cfg::_base* parent = leaf.get_parent(); parent; parent = parent->get_parent())
        if (!parent->get_name().empty()) key = parent->get_name() + " > " + key;
    return key;
}

namespace detail
{
inline void collect(const cfg::node& n, std::vector<cfg::_base*>& out)
{
    for (cfg::_base* e : n.get_nodes())
    {
        if (!e) continue;
        if (e->get_type() == cfg::type::node) collect(*static_cast<const cfg::node*>(e), out);
        else out.push_back(e);
    }
}

inline std::string_view trim(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r' || s.front() == '\n')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.remove_suffix(1);
    return s;
}

inline std::vector<std::string_view> split(std::string_view s, std::string_view sep)
{
    std::vector<std::string_view> out;
    while (true)
    {
        const auto at = s.find(sep);
        out.push_back(s.substr(0, at));
        if (at == std::string_view::npos) return out;
        s.remove_prefix(at + sep.size());
    }
}

// The value kinds the editors know; a cfg::_float reports type::_int, its to_list() items contain '.'
enum class vkind : std::uint8_t { boolean, enumeration, integer, unsigned_, floating, u128, text, set, map, log, device };

inline vkind kind_of(const cfg::_base& leaf)
{
    switch (leaf.get_type())
    {
    case cfg::type::_bool: return vkind::boolean;
    case cfg::type::_enum: return vkind::enumeration;
    case cfg::type::_int:
    {
        const auto range = leaf.to_list();
        return !range.empty() && range[0].find('.') != std::string::npos ? vkind::floating : vkind::integer;
    }
    case cfg::type::uint: return vkind::unsigned_;
    case cfg::type::uint128: return vkind::u128;
    case cfg::type::string: return vkind::text;
    case cfg::type::set: return vkind::set;
    case cfg::type::map:
    case cfg::type::node_map: return vkind::map;
    case cfg::type::log: return vkind::log;
    default: break;
    }
    return vkind::device;
}

inline std::optional<long long> parse_s64(std::string_view s)
{
    s = trim(s);
    long long v = 0;
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    if (r.ec != std::errc() || r.ptr != s.data() + s.size()) return std::nullopt;
    return v;
}

inline std::optional<unsigned long long> parse_u64(std::string_view s)
{
    s = trim(s);
    unsigned long long v = 0;
    int base = 10;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s.remove_prefix(2); }
    const auto r = std::from_chars(s.data(), s.data() + s.size(), v, base);
    if (s.empty() || r.ec != std::errc() || r.ptr != s.data() + s.size()) return std::nullopt;
    return v;
}

// Decimal with '.' or ',' (strtod, the parser cfg::_float uses)
inline std::optional<double> parse_double(std::string_view s)
{
    std::string t(trim(s));
    for (char& c : t)
        if (c == ',') c = '.';
    if (t.empty()) return std::nullopt;
    char* end = nullptr;
    const double v = std::strtod(t.c_str(), &end);
    if (end != t.c_str() + t.size()) return std::nullopt;
    return v;
}

// A float as cfg writes it is %.17g; shown with at most 3 decimals and no trailing zeros
inline std::string short_float(double v)
{
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.3f", v);
    std::string s = buf;
    if (s.find('.') != std::string::npos)
    {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    if (s == "-0") s = "0";
    return s;
}

// A float in the form cfg::_float::from_string accepts and to_string() would print back (%.17g)
inline std::string float_text(double v)
{
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%.17g", v);
    return buf;
}

inline bool ipv4(std::string_view s)
{
    const auto parts = split(s, ".");
    if (parts.size() != 4) return false;
    for (const std::string_view p : parts)
    {
        if (p.empty() || p.size() > 3) return false;
        const auto v = parse_s64(p);
        if (!v || *v < 0 || *v > 255) return false;
    }
    return true;
}

inline bool swap_list(std::string_view s)
{
    if (trim(s).empty()) return true;
    for (const std::string_view pair : split(s, "&&"))
    {
        const auto eq = pair.find('=');
        if (eq == std::string_view::npos || !ipv4(trim(pair.substr(0, eq))) || !ipv4(trim(pair.substr(eq + 1)))) return false;
    }
    return true;
}

inline bool color(std::string_view s)
{
    if (s.size() != 9 || s[0] != '#') return false;
    for (std::size_t i = 1; i < s.size(); ++i)
    {
        const char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}

// One entry of a catalog 'values' rule
struct rule
{
    char mark = 0;            // 0 allowed, '~' warning, '-' greyed, '#' not offered
    std::string_view text;    // a YAML spelling, or a number range
    bool range = false;
    double lo = 0, hi = 0;
};

inline std::vector<rule> parse_rules(std::string_view values)
{
    std::vector<rule> out;
    if (values.empty()) return out;
    for (std::string_view item : split(values, "|"))
    {
        rule r;
        if (!item.empty() && (item.front() == '~' || item.front() == '-' || item.front() == '#') && !(item.front() == '-' && item.size() > 1 && item[1] >= '0' && item[1] <= '9' && item.find("..") == std::string_view::npos))
        {
            r.mark = item.front();
            item.remove_prefix(1);
        }
        r.text = item;
        if (const auto dots = item.find(".."); dots != std::string_view::npos)
        {
            const auto lo = parse_double(item.substr(0, dots));
            const auto hi = parse_double(item.substr(dots + 2));
            if (lo && hi)
            {
                r.range = true;
                r.lo = *lo;
                r.hi = *hi;
            }
        }
        out.push_back(r);
    }
    return out;
}

// How a value fares against the rules
enum class verdict : std::uint8_t { allowed, warn, greyed, hidden };

inline verdict judge_text(const std::vector<rule>& rules, std::string_view spelling)
{
    if (rules.empty()) return verdict::allowed;
    for (const rule& r : rules)
    {
        if (r.range || r.text != spelling) continue;
        return r.mark == '~' ? verdict::warn : r.mark == '-' ? verdict::greyed : r.mark == '#' ? verdict::hidden : verdict::allowed;
    }
    return verdict::greyed;
}

inline verdict judge_number(const std::vector<rule>& rules, double v)
{
    bool any_number = false;
    for (const rule& r : rules)
    {
        if (r.range)
        {
            any_number = true;
            if (v >= r.lo && v <= r.hi) return r.mark == '~' ? verdict::warn : r.mark == '-' ? verdict::greyed : r.mark == '#' ? verdict::hidden : verdict::allowed;
            continue;
        }
        if (const auto n = parse_double(r.text))
        {
            any_number = true;
            if (*n == v) return r.mark == '~' ? verdict::warn : r.mark == '-' ? verdict::greyed : r.mark == '#' ? verdict::hidden : verdict::allowed;
        }
    }
    return any_number ? verdict::greyed : verdict::allowed;
}

// The rule list names single numbers only ("0|2|4|8|16"): the stepper walks them
inline std::vector<double> listed_numbers(const std::vector<rule>& rules)
{
    std::vector<double> out;
    for (const rule& r : rules)
    {
        if (r.range || r.mark == '#' || r.mark == '-') return {};
        const auto n = parse_double(r.text);
        if (!n) return {};
        out.push_back(*n);
    }
    return out;
}

// Log level spellings (fmt_class_string<logs::level>, util/logs.cpp) and their labels
inline constexpr std::array<std::string_view, 8> log_level_spellings = {"Nothing", "Fatal", "Error", "TODO", "Success", "Warning", "Notice", "Trace"};
inline constexpr std::array<i18n::str, 8> log_level_labels = {i18n::str::CFGU_LOG_LEVEL_ALWAYS, i18n::str::CFGU_LOG_LEVEL_FATAL,
    i18n::str::CFGU_LOG_LEVEL_ERROR, i18n::str::CFGU_LOG_LEVEL_TODO, i18n::str::CFGU_LOG_LEVEL_SUCCESS, i18n::str::CFGU_LOG_LEVEL_WARNING,
    i18n::str::CFGU_LOG_LEVEL_NOTICE, i18n::str::CFGU_LOG_LEVEL_TRACE};

// A double-quoted YAML scalar (escapes as yaml-cpp reads them), or the text as it is when nothing could be misread
inline std::string yaml_scalar(std::string_view s)
{
    bool plain = !s.empty() && s.size() < 128;
    for (const char c : s)
    {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ' ' || c == '.' || c == '_' || c == '(' ||
            c == ')' || c == '/' || c == '+' || c == '%' || c == ',';
        if (!ok) { plain = false; break; }
    }
    if (plain && (s.front() == ' ' || s.back() == ' ' || s.front() == '%' || s.front() == ',')) plain = false;
    if (plain && (s == "~" || s == "null" || s == "Null" || s == "NULL")) plain = false;
    if (plain) return std::string(s);
    std::string out = "\"";
    for (const char c : s)
    {
        const unsigned char u = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c == '\n') out += "\\n";
        else if (c == '\t') out += "\\t";
        else if (u < 0x20)
        {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\x%02X", u);
            out += buf;
        }
        else out += c;
    }
    return out + "\"";
}

// A leaf's value as one text: scalars as cfg prints them, a set as its items, a map as 'key\tvalue' lines, a log map as
// 'channel\tlevel' lines (the snapshots and the dry runs compare and restore these)
inline std::string canonical(const cfg::_base& leaf)
{
    switch (leaf.get_type())
    {
    case cfg::type::set:
    {
        std::string out;
        for (const std::string& item : leaf.to_list()) out += item + '\n';
        return out;
    }
    case cfg::type::map:
    case cfg::type::node_map:
    {
        std::string out;
        for (const auto& [k, v] : static_cast<const cfg::map_entry&>(leaf).get_map()) out += k + '\t' + v + '\n';
        return out;
    }
    case cfg::type::log:
    {
        std::string out;
        for (const auto& [k, v] : static_cast<const cfg::log_entry&>(leaf).get_map())
        {
            const auto i = static_cast<std::size_t>(v);
            out += k + '\t' + std::string(i < log_level_spellings.size() ? log_level_spellings[i] : "Notice") + '\n';
        }
        return out;
    }
    case cfg::type::device: return {};
    default: break;
    }
    return leaf.to_string();
}

inline bool restore(cfg::_base& leaf, std::string_view text)
{
    switch (leaf.get_type())
    {
    case cfg::type::set:
    {
        std::vector<std::string> items;
        for (const std::string_view line : split(text, "\n"))
            if (!line.empty()) items.emplace_back(line);
        return leaf.from_list(std::move(items));
    }
    case cfg::type::map:
    case cfg::type::node_map:
    {
        cfg::map_of_type<std::string> map;
        for (const std::string_view line : split(text, "\n"))
        {
            const auto tab_at = line.find('\t');
            if (line.empty() || tab_at == std::string_view::npos) continue;
            map.emplace(std::string(line.substr(0, tab_at)), std::string(line.substr(tab_at + 1)));
        }
        static_cast<cfg::map_entry&>(leaf).set_map(std::move(map));
        return true;
    }
    case cfg::type::log:
    {
        cfg::map_of_type<logs::level> map;
        for (const std::string_view line : split(text, "\n"))
        {
            const auto tab_at = line.find('\t');
            if (line.empty() || tab_at == std::string_view::npos) continue;
            const std::string_view level = line.substr(tab_at + 1);
            for (std::size_t i = 0; i < log_level_spellings.size(); ++i)
                if (log_level_spellings[i] == level) map.emplace(std::string(line.substr(0, tab_at)), static_cast<logs::level>(i));
        }
        static_cast<cfg::log_entry&>(leaf).set_map(std::move(map));
        return true;
    }
    case cfg::type::device: return true;
    default: break;
    }
    return leaf.from_string(text);
}

inline void emit_yaml(std::string& out, const cfg::_base& leaf, int indent)
{
    const std::string pad(std::size_t(indent), ' ');
    const std::string key = pad + yaml_scalar(leaf.get_name()) + ":";
    switch (leaf.get_type())
    {
    case cfg::type::set:
    {
        const auto items = leaf.to_list();
        if (items.empty()) { out += key + " []\n"; return; }
        out += key + "\n";
        for (const std::string& item : items) out += pad + "  - " + yaml_scalar(item) + "\n";
        return;
    }
    case cfg::type::map:
    case cfg::type::node_map:
    {
        const auto& map = static_cast<const cfg::map_entry&>(leaf).get_map();
        if (map.empty()) { out += key + " {}\n"; return; }
        out += key + "\n";
        for (const auto& [k, v] : map) out += pad + "  " + yaml_scalar(k) + ": " + yaml_scalar(v) + "\n";
        return;
    }
    case cfg::type::log:
    {
        std::string body;
        for (const auto& [k, v] : static_cast<const cfg::log_entry&>(leaf).get_map())
        {
            const auto i = static_cast<std::size_t>(v);
            if (v == logs::level::_default || i >= log_level_spellings.size()) continue;
            body += pad + "  " + yaml_scalar(k) + ": " + std::string(log_level_spellings[i]) + "\n";
        }
        if (body.empty()) { out += key + " {}\n"; return; }
        out += key + "\n" + body;
        return;
    }
    case cfg::type::device: return;
    default: out += key + " " + yaml_scalar(leaf.to_string()) + "\n"; return;
    }
}

inline void emit_selected(std::string& out, const cfg::node& n, const std::set<const cfg::_base*>& wanted, int indent)
{
    for (const cfg::_base* e : n.get_nodes())
    {
        if (!e) continue;
        if (e->get_type() == cfg::type::node)
        {
            std::string inner;
            emit_selected(inner, *static_cast<const cfg::node*>(e), wanted, indent + 2);
            if (!inner.empty()) out += std::string(std::size_t(indent), ' ') + yaml_scalar(e->get_name()) + ":\n" + inner;
            continue;
        }
        if (wanted.count(e)) emit_yaml(out, *e, indent);
    }
}

inline std::int64_t system_now()
{
    return static_cast<std::int64_t>(std::time(nullptr));
}
} // namespace detail

// The leaves of a tree in its order
inline std::vector<cfg::_base*> leaves_of(const cfg::node& root)
{
    std::vector<cfg::_base*> out;
    detail::collect(root, out);
    return out;
}

inline cfg::_base* find(const cfg::node& root, std::string_view key)
{
    for (cfg::_base* leaf : leaves_of(root))
        if (key_of(*leaf) == key) return leaf;
    return nullptr;
}

// YAML of the listed settings only, nested in their sections, in tree order (a per-game file of format 2 before the
// stamp keys; with_settings_stamp() adds them). Unknown keys are skipped.
inline std::string sparse_yaml_for(const cfg::node& root, const std::vector<std::string>& keys)
{
    std::set<const cfg::_base*> wanted;
    for (const std::string& key : keys)
        if (const cfg::_base* leaf = find(root, key)) wanted.insert(leaf);
    std::string out;
    detail::emit_selected(out, root, wanted, 0);
    return out;
}

// Text of every dynamic leaf ('key\tvalue' lines), taken when the in-game home menu opens
inline std::string dynamic_snapshot(const cfg::node& root)
{
    std::string out;
    for (const cfg::_base* leaf : leaves_of(root))
    {
        if (!leaf->get_is_dynamic()) continue;
        std::string value = detail::canonical(*leaf);
        for (char& c : value)
            if (c == '\n') c = '\x1f';
        out += key_of(*leaf) + '\t' + value + '\n';
    }
    return out;
}

// The dynamic settings whose value differs from a dynamic_snapshot(): the player's live edits in the home menu
inline std::vector<std::string> dynamic_changes(const cfg::node& root, std::string_view snapshot)
{
    std::map<std::string, std::string, std::less<>> before;
    for (const std::string_view line : detail::split(snapshot, "\n"))
    {
        const auto at = line.find('\t');
        if (at == std::string_view::npos) continue;
        before.emplace(std::string(line.substr(0, at)), std::string(line.substr(at + 1)));
    }
    std::vector<std::string> out;
    for (const cfg::_base* leaf : leaves_of(root))
    {
        if (!leaf->get_is_dynamic()) continue;
        const std::string key = key_of(*leaf);
        const auto it = before.find(key);
        if (it == before.end()) continue;
        std::string value = detail::canonical(*leaf);
        for (char& c : value)
            if (c == '\n') c = '\x1f';
        if (value != it->second) out.push_back(key);
    }
    return out;
}

// One line of a browser page
struct row
{
    const cat::item* meta = nullptr; // null: a setting the catalog lacks, or a section caption
    cfg::_base* leaf = nullptr;      // null for a section caption
    std::string key, label, value, help, note; // note: "muda durante o jogo" / "também no menu do jogo"
    std::string badge_text;
    badge mark = badge::none;
    origin from = origin::saved;
    klass kind = klass::expose;
    ps4_switch redirect = ps4_switch::none;
    keyboard_kind keyboard = keyboard_kind::none;
    sub_page page = sub_page::none;
    bool section = false;        // caption, not selectable
    bool pinned = false;         // the player's own choice (this scope's file or this session)
    bool differs_global = false; // per game: the value differs from the global one
    bool locked = false;         // unavailable, hidden or read-only: never edited
    bool dimmed = false;         // a dependency or the scope rule does not hold
    bool warn = false;           // class W: the first change asks for a confirmation
    bool dynamic = false;
    bool picker = false;         // X opens a list (enum, composite, provider)
};

// One value of a picker list
struct choice
{
    std::string spelling, label, reason;
    bool allowed = true, warn = false, current = false;
};

struct library_row
{
    std::string name;
    bool default_hle = false, checked = false, dev_only = false;
};

struct log_row
{
    std::string channel, level;
    i18n::str label = i18n::str::count_;
    bool changed = false; // not the default level
};

struct slot_row
{
    std::string type, device; // type: MIDI device type spelling (empty for a microphone)
};

struct self_check_result
{
    std::uint32_t settings = 0, uncatalogued = 0, round_trip_failures = 0;
    std::vector<std::string> problems;
};

class model
{
public:
    // --- binding and the policy preview ---

    // 'staged' holds the values the files give (config.yml, ps4-settings.yml, then the game's YAML); 'title' empty =
    // global scope; 'global_baseline_yaml' = the staged text before the game's YAML (per game: the "global" values).
    void bind(cfg_root& staged, std::string_view title, std::string_view global_baseline_yaml, const pin_set& pins,
        policy_mode mode = policy_mode::automatic)
    {
        m_cfg = &staged;
        m_title = std::string(title);
        m_mode = mode;
        m_states.clear();
        m_by_leaf.clear();
        m_page.clear();
        m_page_states.clear();
        m_confirmed.clear();
        m_pending_token.clear();
        m_confirm.reset();
        for (cfg::_base* leaf : leaves_of(staged))
        {
            state s;
            s.leaf = leaf;
            s.key = key_of(*leaf);
            s.meta = cat::find(s.key);
            s.saved = detail::canonical(*leaf);
            s.kind = detail::kind_of(*leaf);
            m_by_leaf.emplace(leaf, m_states.size());
            m_states.push_back(std::move(s));
        }
        // the global values: the baseline text loaded into the staged copy for a moment, then every leaf put back
        if (!global_baseline_yaml.empty() && staged.from_string(global_baseline_yaml))
            for (state& s : m_states) s.global = detail::canonical(*s.leaf);
        else
            for (state& s : m_states) s.global = s.saved;
        for (state& s : m_states)
            if (detail::canonical(*s.leaf) != s.saved) detail::restore(*s.leaf, s.saved);
        const auto mark = [&](const std::vector<std::string>& list, bool game)
        {
            for (const std::string& key : list)
                if (state* s = by_key(key)) (game ? s->pin_game : s->pin_global) = true;
        };
        mark(pins.global, false);
        mark(pins.game, true);
        m_dirty = true;
    }

    bool bound() const { return m_cfg != nullptr; }
    bool game_scope() const { return !m_title.empty(); }
    const std::string& title() const { return m_title; }
    policy_mode mode() const { return m_mode; }
    const runtime_inputs& inputs() const { return m_inputs; }

    // The boot policy preview on the saved values with this session's choices and the pins (apply_boot_policy_preview,
    // then the hardware limits, as the settings menu shows them): the staged copy then holds what will run.
    policy_result preview(const runtime_inputs& in)
    {
        m_inputs = in;
        if (!m_cfg) return {};
        const policy_result result = run_preview(true, nullptr);
        m_result = result;
        update_pin_candidates();
        m_dirty = true;
        return result;
    }

    const policy_result& last_preview() const { return m_result; }

    // The change callback the preview uses (ps4_policy.hpp OnChange, 4-argument form): keeps the last step per setting.
    void record(const cfg::_base& setting, const std::string& old, std::string_view why, change_kind kind)
    {
        const auto it = m_by_leaf.find(&setting);
        if (it == m_by_leaf.end()) return;
        state& s = m_states[it->second];
        if (!s.changed) s.before_policy = old;
        s.changed = true;
        s.why = std::string(why);
        s.from = origin_of(s, why, kind);
        if ((kind == change_kind::runtime || kind == change_kind::adapt) && !s.has_runtime_before)
        {
            s.has_runtime_before = true;
            s.runtime_before = old;
        }
    }

    // --- pages ---

    // The rows of a tab (section captions included), texts in the interface language; rebuilt after a change
    std::span<const row> page(tab t, bool show_hidden = false)
    {
        if (m_dirty || t != m_page_tab || show_hidden != m_page_hidden || m_page_lang != std::uint8_t(i18n::current()))
        {
            m_page_tab = t;
            m_page_hidden = show_hidden;
            m_page_lang = std::uint8_t(i18n::current());
            build_page();
            m_dirty = false;
        }
        return m_page;
    }

    // "26 opções · 4 automáticas · 2 limitadas no PS4 · 1 ajuste seu"
    std::string tab_summary(tab t)
    {
        std::uint32_t total = 0, automatic = 0, limited = 0, mine = 0;
        for (const state& s : m_states)
        {
            if (tab_of(s) != t || (s.meta && (s.meta->kind == klass::hidden || s.meta->edit == editor::zcull_member))) continue;
            total++;
            const origin o = s.from;
            if (o == origin::profile || o == origin::migration) automatic++;
            if ((s.meta && s.meta->kind == klass::unavailable) || o == origin::console || o == origin::runtime || o == origin::hardware) limited++;
            if (pinned(s)) mine++;
        }
        return i18n::trf(i18n::str::CFGU_TAB_SUMMARY, {total, automatic, limited, mine});
    }

    // --- edits (index = a row of the last page()) ---

    std::string step(std::size_t i, int direction, bool coarse = false) { return with_row(i, [&](state& s) { return do_step(s, direction, coarse); }); }
    // The keyboard's result (number, hex, text, date)
    std::string set_text(std::size_t i, std::string_view text) { return with_row(i, [&](state& s) { return do_set_text(s, text); }); }
    std::vector<choice> choices(std::size_t i) const
    {
        const state* s = state_of(i);
        return s ? choices_of(*s) : std::vector<choice>{};
    }
    std::string pick(std::size_t i, std::size_t c) { return with_row(i, [&](state& s) { return do_pick(s, c); }); }
    // Square: the value the boot runs when the player chooses nothing; removes the row's pin
    std::string reset_auto(std::size_t i) { return with_row(i, [&](state& s) { return do_reset_auto(s); }, false); }
    // Options > RPCS3 default (def_to_string); it becomes a pin when it differs from the automatic value
    std::string reset_upstream(std::size_t i)
    {
        if (const state* s = state_of(i); s && s->meta && (s->meta->flags & cat::flag::no_default)) return i18n::tr(i18n::str::CFGU_NO_DEFAULT);
        return with_row(i, [&](state& s) { return do_reset_upstream(s); });
    }
    // Options > Use the global value (per game): the game's choice is removed
    std::string use_global(std::size_t i) { return with_row(i, [&](state& s) { return do_use_global(s); }, false); }
    // Options > Keep this value / Unpin
    std::string toggle_pin(std::size_t i) { return with_row(i, [&](state& s) { return do_toggle_pin(s); }, false); }

    // The same edits by setting key (sub-pages, tests, the launcher's shortcuts)
    std::string step_key(std::string_view key, int direction, bool coarse = false) { return with_key(key, [&](state& s) { return do_step(s, direction, coarse); }); }
    std::string set_text_key(std::string_view key, std::string_view text) { return with_key(key, [&](state& s) { return do_set_text(s, text); }); }
    std::string reset_auto_key(std::string_view key) { return with_key(key, [&](state& s) { return do_reset_auto(s); }, false); }
    std::string reset_upstream_key(std::string_view key)
    {
        if (const state* s = by_key_const(key); s && s->meta && (s->meta->flags & cat::flag::no_default)) return i18n::tr(i18n::str::CFGU_NO_DEFAULT);
        return with_key(key, [&](state& s) { return do_reset_upstream(s); });
    }
    std::string use_global_key(std::string_view key) { return with_key(key, [&](state& s) { return do_use_global(s); }, false); }
    std::string toggle_pin_key(std::string_view key) { return with_key(key, [&](state& s) { return do_toggle_pin(s); }, false); }

    // A warn row, and a warned value, asks once per session: the edit that needs it changes nothing and returns the
    // warning; confirmation() then names the row (index of the last page(), SIZE_MAX for an edit by key) until the
    // launcher shows CFGU_WARN_TITLE / CFGU_WARN_CONFIRM and calls confirm() (or cancel_confirmation()), then repeats it
    std::optional<std::size_t> confirmation() const { return m_confirm; }
    void confirm()
    {
        if (!m_pending_token.empty()) m_confirmed.insert(m_pending_token);
        m_pending_token.clear();
        m_confirm.reset();
    }
    void cancel_confirmation()
    {
        m_pending_token.clear();
        m_confirm.reset();
    }

    // The keyboard a row opens: kind, the dialog title and the text it starts with
    keyboard_kind keyboard(std::size_t i, std::string& title_text, std::string& initial) const
    {
        const state* s = state_of(i);
        if (!s) return keyboard_kind::none;
        const keyboard_kind k = keyboard_of(*s);
        const std::string label = label_of(*s);
        switch (k)
        {
        case keyboard_kind::number: title_text = i18n::trf(i18n::str::CFGU_KB_TITLE_NUMBER, {i18n::raw(label)}); break;
        case keyboard_kind::hex: title_text = i18n::trf(i18n::str::CFGU_KB_TITLE_HEX, {i18n::raw(label)}); break;
        case keyboard_kind::text: title_text = i18n::trf(i18n::str::CFGU_KB_TITLE_TEXT, {i18n::raw(label)}); break;
        case keyboard_kind::date: title_text = i18n::tr(i18n::str::CFGU_KB_TITLE_DATE); break;
        case keyboard_kind::none: break;
        }
        if (k == keyboard_kind::date) initial = date_text(*s);
        else if (k == keyboard_kind::number && s->kind == detail::vkind::floating) initial = detail::short_float(number_of(*s));
        else if (k == keyboard_kind::hex) initial = hex_text(*s);
        else initial = s->leaf->to_string();
        return k;
    }

    // Options > Full help: the PC tooltip, else the help line; Options > YAML key
    std::string full_help(std::size_t i) const
    {
        const state* s = state_of(i);
        if (!s || !s->meta) return i18n::tr(i18n::str::CFGU_NEW_SETTING_HELP);
        return i18n::tr(s->meta->tip != i18n::str::count_ ? s->meta->tip : s->meta->help);
    }
    std::string yaml_key(std::size_t i) const
    {
        const state* s = state_of(i);
        return s ? s->key : std::string{};
    }

    // --- sub-pages ---

    // Libraries Control: the firmware libraries in two sections (LLE by default, then HLE by default)
    std::vector<library_row> libraries() const
    {
        std::vector<library_row> out;
        const state* s = by_key_const("Core > Libraries Control");
        if (!s) return out;
        const auto items = s->leaf->to_list();
        const std::set<std::string> set(items.begin(), items.end());
        for (const bool hle : {false, true})
            for (const auto& [name, def] : g_prx_list)
            {
                if ((def != 0) != hle) continue;
                const std::string n(name);
                const int res = int(set.count(n + ":hle")) - int(set.count(n + ":lle"));
                out.push_back({n, hle, res != 0 && res != (hle ? 1 : -1), n.starts_with("libsysutil")});
            }
        return out;
    }
    std::string toggle_library(std::string_view name)
    {
        return with_key("Core > Libraries Control", [&](state& s)
        {
            const auto it = g_prx_list.find(name);
            if (it == g_prx_list.end()) return std::string(i18n::tr(i18n::str::CFGU_BAD_NUMBER));
            const bool hle_default = it->second != 0;
            auto items = s.leaf->to_list();
            const std::string n(name);
            const bool was = std::find(items.begin(), items.end(), n + (hle_default ? ":lle" : ":hle")) != items.end();
            items.erase(std::remove_if(items.begin(), items.end(), [&](const std::string& e) { return e == n + ":hle" || e == n + ":lle"; }), items.end());
            if (!was) items.push_back(n + (hle_default ? ":lle" : ":hle"));
            s.leaf->from_list(std::move(items));
            return changed(s);
        });
    }
    std::string clear_libraries()
    {
        return with_key("Core > Libraries Control", [&](state& s)
        {
            s.leaf->from_list({});
            return changed(s);
        });
    }

    // Log levels: every channel the binary knows, with its level (default Notice)
    std::vector<log_row> log_levels() const
    {
        std::vector<log_row> out;
        const state* s = by_key_const("Log");
        if (!s) return out;
        const auto& map = static_cast<const cfg::log_entry&>(*s->leaf).get_map();
        for (const std::string& channel : logs::get_channels())
        {
            const auto it = map.find(channel);
            const logs::level l = it != map.end() ? it->second : logs::level::_default;
            const auto idx = static_cast<std::size_t>(l) < detail::log_level_spellings.size() ? static_cast<std::size_t>(l) : 6;
            out.push_back({channel, std::string(detail::log_level_spellings[idx]), detail::log_level_labels[idx], l != logs::level::_default});
        }
        return out;
    }
    std::string step_log(std::string_view channel, int direction)
    {
        return with_key("Log", [&](state& s)
        {
            auto map = static_cast<const cfg::log_entry&>(*s.leaf).get_map();
            const auto it = map.find(channel);
            const int now = it != map.end() ? static_cast<int>(it->second) : static_cast<int>(logs::level::_default);
            const int next = std::clamp(now + (direction < 0 ? -1 : 1), 0, int(detail::log_level_spellings.size()) - 1);
            if (next == static_cast<int>(logs::level::_default)) map.erase(std::string(channel));
            else map[std::string(channel)] = static_cast<logs::level>(next);
            static_cast<cfg::log_entry&>(*s.leaf).set_map(std::move(map));
            return changed(s);
        });
    }

    // An anaglyph matrix: 9 cells "00".."22" (row, column), shown as numbers
    std::array<double, 9> matrix(std::string_view key) const
    {
        std::array<double, 9> out{};
        const state* s = by_key_const(key);
        if (!s || s->kind != detail::vkind::map) return out;
        const auto& map = static_cast<const cfg::map_entry&>(*s->leaf).get_map();
        for (std::size_t c = 0; c < 9; ++c)
        {
            const auto it = map.find(cell_key(c));
            if (it != map.end())
                if (const auto v = detail::parse_double(it->second)) out[c] = *v;
        }
        return out;
    }
    std::string step_matrix(std::string_view key, std::size_t cell, int direction)
    {
        return with_key(key, [&](state& s) { return set_cell(s, cell, matrix(s.key)[cell % 9] + (direction < 0 ? -0.05 : 0.05)); });
    }
    std::string set_matrix_text(std::string_view key, std::size_t cell, std::string_view text)
    {
        return with_key(key, [&](state& s)
        {
            const auto v = detail::parse_double(text);
            if (!v) return std::string(i18n::tr(i18n::str::CFGU_BAD_NUMBER));
            return set_cell(s, cell, *v);
        });
    }

    // Audio Formats: the format bits (the first one is always on)
    std::vector<std::pair<std::uint32_t, bool>> audio_formats() const
    {
        std::vector<std::pair<std::uint32_t, bool>> out;
        const state* s = by_key_const("Audio > Audio Formats");
        const std::uint64_t value = s ? detail::parse_u64(s->leaf->to_string()).value_or(0) : 0;
        for (const cat::value_label& v : cat::value_labels())
        {
            if (v.key != "Audio > Audio Formats") continue;
            const auto bit = static_cast<std::uint32_t>(detail::parse_u64(v.spelling).value_or(0));
            out.emplace_back(bit, bit == 0 || (value & bit) != 0);
        }
        return out;
    }
    std::string toggle_audio_format(std::uint32_t bit)
    {
        return with_key("Audio > Audio Formats", [&](state& s)
        {
            if (!bit) return changed(s);
            const std::uint64_t value = detail::parse_u64(s.leaf->to_string()).value_or(0) ^ bit;
            if (!s.leaf->from_string(std::to_string(value))) return std::string(i18n::tr(i18n::str::CFGU_BAD_NUMBER));
            return changed(s);
        });
    }

    // Microphone (4) and MIDI (3) device slots; the device lists come from the launcher (OpenAL capture, USB MIDI)
    void set_microphone_devices(std::vector<std::string> names) { m_microphones = std::move(names); m_dirty = true; }
    void set_midi_devices(std::vector<std::string> names) { m_midi = std::move(names); m_dirty = true; }
    void set_fonts(std::vector<std::string> names) { m_fonts = std::move(names); m_dirty = true; }

    std::vector<slot_row> slots(std::string_view key) const
    {
        std::vector<slot_row> out;
        const state* s = by_key_const(key);
        if (!s) return out;
        const bool midi = s->meta && s->meta->edit == editor::midi;
        const std::size_t count = midi ? 3 : 4;
        const std::string value = s->leaf->to_string();
        const auto parts = detail::split(value, "@@@");
        for (std::size_t n = 0; n < count; ++n)
        {
            const std::string_view part = n < parts.size() ? parts[n] : std::string_view{};
            slot_row r;
            if (midi)
            {
                const auto at = part.find("\xC3\x9F\xC3\x9F\xC3\x9F"); // ßßß
                r.type = std::string(part.substr(0, at));
                if (at != std::string_view::npos) r.device = std::string(part.substr(at + 6));
            }
            else r.device = std::string(part);
            out.push_back(std::move(r));
        }
        return out;
    }
    // Left/Right on a slot: the next device (MIDI: field 0 = type, 1 = device)
    std::string step_slot(std::string_view key, std::size_t slot, int field, int direction)
    {
        return with_key(key, [&](state& s)
        {
            auto list = slots(s.key);
            if (slot >= list.size()) return std::string{};
            const bool midi = s.meta && s.meta->edit == editor::midi;
            const auto cycle = [&](const std::vector<std::string>& values, std::string& now)
            {
                if (values.empty()) return;
                auto it = std::find(values.begin(), values.end(), now);
                const int idx = it == values.end() ? -1 : int(it - values.begin());
                const int n = int(values.size());
                now = values[std::size_t(((idx + (direction < 0 ? -1 : 1)) % n + n) % n)];
            };
            if (midi && field == 0)
            {
                std::vector<std::string> types;
                for (const cat::value_label& v : cat::value_labels())
                    if (v.key == s.key) types.emplace_back(v.spelling);
                cycle(types, list[slot].type);
            }
            else
            {
                std::vector<std::string> devices{""};
                for (const std::string& d : midi ? m_midi : m_microphones) devices.push_back(d);
                cycle(devices, list[slot].device);
            }
            std::string text;
            for (const slot_row& r : list) text += (midi ? r.type + "\xC3\x9F\xC3\x9F\xC3\x9F" + r.device : r.device) + "@@@";
            if (!s.leaf->from_string(text)) return std::string(i18n::tr(i18n::str::CFGU_BAD_NUMBER));
            return changed(s);
        });
    }

    // --- saving ---

    // The keys of the bound scope's 'PS4 user settings:': the file's pins (minus the ones reset or unpinned in this
    // session), the keys pinned with Options > Keep this value, and the keys chosen in this session whose value differs
    // from the automatic dry run. Settings the PS4 keeps at one value are never pinned.
    std::vector<std::string> pins_to_save()
    {
        std::vector<std::string> out;
        if (!m_cfg) return out;
        update_pin_candidates();
        for (const state& s : m_states)
        {
            if (s.meta && (s.meta->kind == klass::unavailable || s.meta->kind == klass::hidden)) continue;
            const bool file_pin = game_scope() ? s.pin_game : s.pin_global;
            const bool keep = s.force_pin || (file_pin && !s.unpinned && !s.reset) || (s.edited && !s.reset && s.would_pin);
            if (keep) out.push_back(s.key);
        }
        return out;
    }

    // Both scopes' pins as the policy reads them (the files' lists with this session's changes); the preview uses it
    pin_set current_pins() const
    {
        pin_set pins;
        for (const state& s : m_states)
        {
            const bool session = (s.edited && !s.reset) || s.force_pin;
            const bool drop = s.unpinned || s.reset;
            const bool global = (s.pin_global && !(drop && !game_scope())) || (!game_scope() && session);
            const bool game = (s.pin_game && !(drop && game_scope())) || (game_scope() && session);
            if (global) pins.global.push_back(s.key);
            if (game) pins.game.push_back(s.key);
        }
        return pins;
    }

    // The full dump the global file gets, with the values the runtime section forced put back (the boot forces them
    // again; an adaptation or the online switch must not be frozen into the file)
    std::string save_dump()
    {
        if (!m_cfg) return {};
        const auto shown = snapshot();
        restore_runtime_values();
        std::string text = m_cfg->to_string();
        restore_snapshot(shown);
        return text;
    }

    // Per game: only the pinned settings (pins_to_save), with their chosen values (format 2 body, before the stamp)
    std::string sparse_yaml()
    {
        if (!m_cfg) return {};
        const std::vector<std::string> keys = pins_to_save();
        const auto shown = snapshot();
        restore_runtime_values();
        std::string text = sparse_yaml_for(*m_cfg, keys);
        restore_snapshot(shown);
        return text;
    }

    // Number of settings chosen or reset in this session
    std::size_t pending() const
    {
        std::size_t n = 0;
        for (const state& s : m_states) n += s.edited || s.reset || s.force_pin || s.unpinned;
        return n;
    }

    // top > Restaurar valores padrão: every editable setting of the scope back to its automatic value, no pins
    void reset_all()
    {
        for (state& s : m_states)
        {
            if (is_locked(s)) continue;
            s.chosen = baseline_value(s);
            s.edited = true;
            s.reset = true;
            s.force_pin = false;
            s.unpinned = false;
        }
        refresh();
    }

    // Per game, converting a format-1 file: the settings whose saved value differs from what the automatic configuration
    // runs for the game (one dry run with every setting at its global value); the player confirms the list, then
    // pin_keys() makes them this game's choices
    std::vector<std::string> differing_from_automatic()
    {
        std::vector<std::string> out;
        if (!m_cfg) return out;
        const auto staged = snapshot();
        std::vector<std::optional<std::string>> chosen;
        chosen.reserve(m_states.size());
        for (state& s : m_states)
        {
            chosen.push_back(s.chosen);
            if (!is_locked(s)) s.chosen = baseline_value(s);
        }
        pin_set pins = current_pins();
        (game_scope() ? pins.game : pins.global).clear();
        run_preview(false, &pins);
        const auto automatic = snapshot();
        for (std::size_t i = 0; i < m_states.size(); ++i)
        {
            state& s = m_states[i];
            s.chosen = chosen[i];
            if (is_locked(s)) continue;
            if ((s.chosen ? *s.chosen : s.saved) != automatic[i]) out.push_back(s.key);
        }
        restore_snapshot(staged);
        return out;
    }

    void pin_keys(const std::vector<std::string>& keys)
    {
        for (const std::string& key : keys)
        {
            state* s = by_key(key);
            if (!s || is_locked(*s)) continue;
            if (!s->chosen) s->chosen = s->saved;
            s->edited = true;
            s->force_pin = true;
            s->reset = false;
            s->unpinned = false;
        }
        refresh();
    }

    // Every pin of the bound scope removed, every choice of this session dropped (top > Voltar para automático)
    void clear_scope_choices()
    {
        for (state& s : m_states)
        {
            const bool file_pin = game_scope() ? s.pin_game : s.pin_global;
            s.force_pin = false;
            s.unpinned = file_pin;
            s.edited = file_pin;
            s.reset = file_pin;
            if (file_pin) s.chosen = baseline_value(s);
            else s.chosen.reset();
        }
        refresh();
    }

    // Console self-check (startup.log): every leaf has a catalog row, and its value survives to_string/from_string
    static self_check_result self_check(cfg::node& root)
    {
        self_check_result r;
        for (cfg::_base* leaf : leaves_of(root))
        {
            r.settings++;
            const std::string key = key_of(*leaf);
            if (!cat::find(key))
            {
                r.uncatalogued++;
                r.problems.push_back("no catalog row: " + key);
            }
            const std::string before = detail::canonical(*leaf);
            if (leaf->get_type() == cfg::type::set || leaf->get_type() == cfg::type::map || leaf->get_type() == cfg::type::node_map ||
                leaf->get_type() == cfg::type::log || leaf->get_type() == cfg::type::device)
                continue;
            if (!leaf->from_string(leaf->to_string()) || detail::canonical(*leaf) != before)
            {
                r.round_trip_failures++;
                r.problems.push_back("value does not survive a round trip: " + key + " = " + before);
                detail::restore(*leaf, before);
            }
            if (leaf->get_type() == cfg::type::_enum)
            {
                for (const std::string& spelling : leaf->to_list())
                {
                    if (!leaf->from_string(spelling) || leaf->to_string() != spelling)
                    {
                        r.round_trip_failures++;
                        r.problems.push_back("enum spelling not accepted: " + key + " = " + spelling);
                    }
                }
                detail::restore(*leaf, before);
            }
        }
        return r;
    }

    // Clock of the console time row (host tests set a fixed one)
    void set_clock(std::int64_t (*now)()) { m_now = now ? now : &detail::system_now; m_dirty = true; }

private:
    struct state
    {
        cfg::_base* leaf = nullptr;
        const cat::item* meta = nullptr;
        std::string key;
        detail::vkind kind = detail::vkind::text;
        std::string saved;                 // the files' value (canonical text)
        std::string global;                // the global value (per game: before the game's YAML)
        std::optional<std::string> chosen; // this session's value
        bool edited = false;               // chosen in this session
        bool reset = false;                // Square / Use the global value: not a pin
        bool force_pin = false;            // Options > Keep this value
        bool unpinned = false;             // Options > Unpin of a file pin
        bool pin_global = false, pin_game = false; // the files' pins
        bool would_pin = false;            // chosen, and different from the automatic dry run (the last refresh)
        // the last preview
        origin from = origin::saved;
        std::string why, before_policy, runtime_before;
        bool changed = false, has_runtime_before = false;
    };

    cfg_root* m_cfg = nullptr;
    std::string m_title;
    policy_mode m_mode = policy_mode::automatic;
    runtime_inputs m_inputs{};
    policy_result m_result{};
    std::vector<state> m_states;
    std::map<const cfg::_base*, std::size_t> m_by_leaf;
    std::vector<row> m_page;
    std::vector<std::size_t> m_page_states; // state index per page row (npos for captions)
    tab m_page_tab = tab::count_;
    bool m_page_hidden = false;
    std::uint8_t m_page_lang = 0xff;
    bool m_dirty = true;
    std::set<std::string> m_confirmed;     // rows ("key") and warned values ("key=value", "key=~") confirmed this session
    std::string m_pending_token;
    std::optional<std::size_t> m_confirm;
    std::size_t m_last_row = SIZE_MAX;
    std::vector<std::string> m_fonts, m_microphones, m_midi;
    std::int64_t (*m_now)() = &detail::system_now;

    static constexpr std::size_t npos = SIZE_MAX;

    state* by_key(std::string_view key)
    {
        for (state& s : m_states)
            if (s.key == key) return &s;
        return nullptr;
    }
    const state* by_key_const(std::string_view key) const
    {
        for (const state& s : m_states)
            if (s.key == key) return &s;
        return nullptr;
    }

    const state* state_of(std::size_t i) const
    {
        return i < m_page_states.size() && m_page_states[i] != npos ? &m_states[m_page_states[i]] : nullptr;
    }

    std::size_t index_of(const state& s) const { return std::size_t(&s - m_states.data()); }

    static std::string cell_key(std::size_t c)
    {
        return std::string(1, char('0' + (c % 9) / 3)) + char('0' + (c % 9) % 3);
    }

    std::string set_cell(state& s, std::size_t cell, double v)
    {
        auto map = static_cast<const cfg::map_entry&>(*s.leaf).get_map();
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%f", std::clamp(v, -16.0, 16.0)); // stereo_config::convert_matrix writes "%f"
        map[cell_key(cell)] = buf;
        static_cast<cfg::map_entry&>(*s.leaf).set_map(std::move(map));
        return changed(s);
    }

    tab tab_of(const state& s) const { return s.meta ? s.meta->where : cat::default_tab(s.key); }

    // The player's own choice: pinned with Options, chosen with a value the automatic configuration would not run, or a
    // pin of the files nobody removed
    bool pinned(const state& s) const
    {
        if (s.force_pin || (s.would_pin && s.edited && !s.reset)) return true;
        if (s.unpinned || s.reset) return false;
        return s.pin_game || s.pin_global;
    }

    // would_pin of every chosen setting (one dry run each; a session holds few)
    void update_pin_candidates()
    {
        for (std::size_t i = 0; i < m_states.size(); ++i)
        {
            state& s = m_states[i];
            s.would_pin = false;
            if (!s.edited || s.reset || s.force_pin || is_locked(s)) continue;
            s.would_pin = detail::canonical(*s.leaf) != automatic_value(i);
        }
    }

    origin origin_of(const state& s, std::string_view why, change_kind kind) const
    {
        const cfg_root& c = *m_cfg;
        switch (kind)
        {
        case change_kind::profile: return origin::profile;
        case change_kind::migration: return origin::migration;
        case change_kind::title: return is_database_reason(why) ? origin::database : origin::title_row;
        case change_kind::adapt: return origin::adapt;
        case change_kind::console: return origin::console;
        case change_kind::runtime:
            if (s.leaf == &c.core.use_accurate_dfma) return origin::hardware;
            if (s.leaf == &c.net.net_active || s.leaf == &c.net.psn_status || s.leaf == &c.net.upnp_enabled || s.leaf == &c.sys.language ||
                s.leaf == &c.io.pad_sleep)
                return origin::switch_;
            return origin::runtime;
        }
        return origin::saved;
    }

    ps4_switch redirect_of(const state& s) const
    {
        if (!m_cfg || m_mode == policy_mode::keep_user_settings) return ps4_switch::none;
        const cfg_root& c = *m_cfg;
        if (s.leaf == &c.net.net_active || s.leaf == &c.net.psn_status) return ps4_switch::online;
        if (s.leaf == &c.net.upnp_enabled) return ps4_switch::upnp;
        if (s.leaf == &c.sys.language) return ps4_switch::game_language;
        return ps4_switch::none;
    }

    static i18n::str switch_name(ps4_switch w)
    {
        switch (w)
        {
        case ps4_switch::online: return i18n::str::CFGU_SWITCH_ONLINE;
        case ps4_switch::upnp: return i18n::str::CFGU_SWITCH_UPNP;
        case ps4_switch::game_language: return i18n::str::CFGU_SWITCH_GAME_LANGUAGE;
        case ps4_switch::cool: return i18n::str::CFGU_SWITCH_COOL;
        case ps4_switch::none: break;
        }
        return i18n::str::count_;
    }

    std::string label_of(const state& s) const
    {
        return s.meta ? std::string(i18n::tr(s.meta->label)) : s.leaf->get_name();
    }

    static bool is_locked(const state& s)
    {
        return s.meta && (s.meta->kind == klass::unavailable || s.meta->kind == klass::hidden || s.meta->edit == editor::readonly);
    }

    // The first dependency of the row that does not hold (its key), or empty
    std::string failing_dependency(const state& s) const
    {
        if (!s.meta || s.meta->depends.empty()) return {};
        for (const std::string_view cond : detail::split(s.meta->depends, "&"))
        {
            const bool negate = cond.find("!=") != std::string_view::npos;
            const auto op = negate ? cond.find("!=") : cond.find('=');
            if (op == std::string_view::npos) continue;
            const std::string_view key = cond.substr(0, op);
            const std::string_view want = cond.substr(op + (negate ? 2 : 1));
            const state* other = by_key_const(key);
            if (!other) continue;
            const std::string now = other->leaf->to_string();
            bool match = false;
            for (const std::string_view v : detail::split(want, "|"))
                if (now == v) match = true;
            if (match == negate) return std::string(key);
        }
        return {};
    }

    bool scope_blocked(const state& s) const
    {
        if (!s.meta) return false;
        return (s.meta->where_scope == cat::scope::game_only && !game_scope()) || (s.meta->where_scope == cat::scope::global_only && game_scope());
    }

    keyboard_kind keyboard_of(const state& s) const
    {
        if (is_locked(s)) return keyboard_kind::none;
        const editor e = s.meta ? s.meta->edit : editor::auto_;
        switch (e)
        {
        case editor::hex: return keyboard_kind::hex;
        case editor::text:
        case editor::ipv4:
        case editor::swap_list:
        case editor::color: return keyboard_kind::text;
        case editor::time_offset: return keyboard_kind::date;
        case editor::auto_: break;
        default: return keyboard_kind::none;
        }
        switch (s.kind)
        {
        case detail::vkind::integer:
        case detail::vkind::unsigned_:
        case detail::vkind::floating: return keyboard_kind::number;
        case detail::vkind::u128: return keyboard_kind::hex;
        case detail::vkind::text: return keyboard_kind::text;
        default: break;
        }
        return keyboard_kind::none;
    }

    sub_page page_of(const state& s) const
    {
        if (!s.meta) return sub_page::none;
        switch (s.meta->edit)
        {
        case editor::libraries: return sub_page::libraries;
        case editor::log_levels: return sub_page::log_levels;
        case editor::matrix: return sub_page::matrix;
        case editor::audio_formats: return sub_page::audio_formats;
        case editor::microphones: return sub_page::microphones;
        case editor::midi: return sub_page::midi;
        default: break;
        }
        return sub_page::none;
    }

    // --- values ---

    double number_of(const state& s) const
    {
        return detail::parse_double(s.leaf->to_string()).value_or(0);
    }

    std::pair<double, double> bounds_of(const state& s) const
    {
        const auto range = s.leaf->to_list();
        if (range.size() != 2) return {0, 0};
        return {detail::parse_double(range[0]).value_or(0), detail::parse_double(range[1]).value_or(0)};
    }

    std::vector<detail::rule> rules_of(const state& s) const
    {
        auto rules = detail::parse_rules(s.meta ? s.meta->values : std::string_view{});
        if (!m_cfg) return rules;
        // Shader compile threads: OpenGL keeps 0 (one worker on this CPU); vk-compile-3.flag allows a third on Vulkan
        if (s.leaf == &m_cfg->video.shader_compiler_threads_count)
        {
            if (m_cfg->video.renderer != video_renderer::vulkan) rules = detail::parse_rules("0..2");
            else if (m_inputs.compile_3) rules = detail::parse_rules("1..3");
        }
        return rules;
    }

    // A spelling against the row's rules and the PS4's dynamic rules; 'reason' gets why it is warned or refused
    detail::verdict judge(const state& s, std::string_view spelling, i18n::str& reason) const
    {
        reason = s.meta ? s.meta->value_reason : i18n::str::count_;
        detail::verdict v = detail::judge_text(rules_of(s), spelling);
        if (!m_cfg) return v;
        // OpenGL: the asynchronous modes only with gl-async.flag (the GL renderer falls back to the legacy one)
        if (s.leaf == &m_cfg->video.shadermode && m_cfg->video.renderer == video_renderer::opengl && !fs::is_file(gl_async_flag) &&
            (spelling == "Async Recompiler (multi-threaded)" || spelling == "Async Recompiler with Shader Interpreter"))
        {
            reason = i18n::str::CFGR_VALUE_GL_ASYNC;
            return detail::verdict::greyed;
        }
        // PSN 'Simulated' only per game (PC removes it from the global dialog)
        if (s.leaf == &m_cfg->net.psn_status && spelling == "Simulated" && !game_scope())
        {
            reason = i18n::str::CFGR_VALUE_PSN_SIMULATED;
            return detail::verdict::greyed;
        }
        return v;
    }

    std::string value_label(const state& s, std::string_view spelling) const
    {
        if (const cat::value_label* v = cat::find_value(s.key, spelling))
            return v->label != i18n::str::count_ ? std::string(i18n::tr(v->label)) : std::string(v->text);
        return std::string(spelling);
    }

    std::string with_unit(const state& s, std::string number) const
    {
        const std::string_view unit = s.meta ? s.meta->unit : std::string_view{};
        if (unit.empty()) return number;
        if (unit == "x") return number + "x";
        return number + " " + std::string(unit);
    }

    std::string date_text(const state& s) const
    {
        const std::int64_t offset = static_cast<std::int64_t>(detail::parse_s64(s.leaf->to_string()).value_or(0));
        const std::time_t t = static_cast<std::time_t>(m_now() + offset);
        const std::tm* tm = std::localtime(&t);
        if (!tm) return s.leaf->to_string();
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d", tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday, tm->tm_hour, tm->tm_min);
        return buf;
    }

    // A 64-bit bound in hexadecimal (cfg prints it in decimal and reads '0x' too)
    std::string hex_text(const state& s) const
    {
        if (s.kind != detail::vkind::unsigned_) return s.leaf->to_string();
        char buf[24];
        std::snprintf(buf, sizeof(buf), "0x%llX", detail::parse_u64(s.leaf->to_string()).value_or(0));
        return buf;
    }

    std::string zcull_text() const
    {
        const auto& v = m_cfg->video;
        if (v.relaxed_zcull_sync) return i18n::tr(i18n::str::CFGU_ZCULL_RELAXED);
        if (v.precise_zpass_count) return i18n::tr(i18n::str::CFGU_ZCULL_PRECISE);
        return i18n::tr(i18n::str::CFGU_ZCULL_APPROXIMATE);
    }

    // The value column
    std::string value_text(const state& s) const
    {
        const editor e = s.meta ? s.meta->edit : editor::auto_;
        switch (e)
        {
        case editor::zcull: return zcull_text();
        case editor::mfc_delay: return i18n::tr(s.leaf->to_string() == "0" ? i18n::str::COM_DESLIGADO : i18n::str::COM_LIGADO);
        case editor::time_offset:
        {
            const long long offset = detail::parse_s64(s.leaf->to_string()).value_or(0);
            return i18n::trf(i18n::str::CFGU_HOURS_OFFSET, {date_text(s), offset / 3600});
        }
        case editor::audio_formats:
        {
            unsigned n = 0;
            for (const auto& [bit, on] : audio_formats()) n += on;
            return i18n::trf(i18n::str::CFGU_AUDIO_FORMATS_N, {n});
        }
        case editor::microphones:
        case editor::midi:
        {
            unsigned used = 0;
            const auto list = slots(s.key);
            for (const slot_row& r : list) used += !r.device.empty() || (e == editor::midi && !r.type.empty());
            return i18n::trf(i18n::str::CFGU_SLOTS_N, {used, unsigned(list.size())});
        }
        case editor::country:
        {
            const std::string code = s.leaf->to_string();
            for (const countries::country_code& c : countries::g_countries)
                if (!c.ccode.empty() && c.ccode == code) return std::string(c.name) + " (" + code + ")";
            return code;
        }
        default: break;
        }
        switch (s.kind)
        {
        case detail::vkind::boolean: return i18n::tr(s.leaf->to_string() == "true" ? i18n::str::COM_LIGADO : i18n::str::COM_DESLIGADO);
        case detail::vkind::enumeration: return value_label(s, s.leaf->to_string());
        case detail::vkind::integer:
        case detail::vkind::unsigned_:
            if (e == editor::hex) return hex_text(s);
            return with_unit(s, s.leaf->to_string());
        case detail::vkind::floating: return with_unit(s, detail::short_float(number_of(s)));
        case detail::vkind::u128:
        {
            const std::string t = s.leaf->to_string();
            return t.size() > 12 ? t.substr(0, 6) + "\xE2\x80\xA6" + t.substr(t.size() - 4) : t;
        }
        case detail::vkind::text:
        {
            const std::string t = s.leaf->to_string();
            return t.empty() ? std::string(i18n::tr(i18n::str::CFGU_EMPTY)) : t;
        }
        case detail::vkind::set: return i18n::trf(i18n::str::CFGU_ITEMS, {unsigned(s.leaf->to_list().size())});
        case detail::vkind::map: return i18n::trf(i18n::str::CFGU_ITEMS, {unsigned(static_cast<const cfg::map_entry&>(*s.leaf).get_map().size())});
        case detail::vkind::log:
        {
            unsigned n = 0;
            for (const auto& [k, v] : static_cast<const cfg::log_entry&>(*s.leaf).get_map()) n += v != logs::level::_default;
            return i18n::trf(i18n::str::CFGU_CHANNELS, {n});
        }
        case detail::vkind::device: break;
        }
        return {};
    }

    // A canonical value as the value column shows it (the help line's "seu valor salvo")
    std::string shown_value(const state& s, const std::string& text) const
    {
        const std::string now = detail::canonical(*s.leaf);
        if (now == text) return value_text(s);
        detail::restore(*s.leaf, text);
        std::string out = value_text(s);
        detail::restore(*s.leaf, now);
        return out;
    }

    // The help line (provenance first, then the row's reason, then its help)
    std::string help_of(const state& s, badge& mark) const
    {
        using i18n::str;
        mark = badge::none;
        if (s.meta && s.meta->kind == klass::unavailable)
        {
            mark = badge::ps4;
            return std::string(i18n::tr(str::COM_INDISPONIVEL_PS4)) + (s.meta->reason != str::count_ ? i18n::tr(s.meta->reason) : "");
        }
        if (s.meta && s.meta->kind == klass::hidden) return i18n::trf(str::CFGU_HIDDEN_ROW, {str(s.meta->reason)});
        if (scope_blocked(s)) return i18n::tr(s.meta->where_scope == cat::scope::game_only ? str::CFGU_NOTE_GAME_ONLY : str::CFGU_NOTE_GLOBAL_ONLY);
        if (const std::string dep = failing_dependency(s); !dep.empty())
        {
            const state* other = by_key_const(dep);
            return i18n::trf(str::CFGU_NOTE_DEPENDS, {i18n::raw(other ? label_of(*other) : dep)});
        }
        if (s.changed && m_mode != policy_mode::keep_user_settings)
        {
            switch (s.from)
            {
            case origin::profile:
            case origin::migration:
                mark = badge::automatic;
                return i18n::trf(str::CFGU_NOTE_AUTO, {i18n::raw(value_text(s)), i18n::raw(shown_value(s, s.chosen ? *s.chosen : s.saved))});
            case origin::title_row: mark = badge::game; return i18n::tr(str::SET_CORRECAO_DESTE_JOGO_BOOT_MANTEM);
            case origin::database: mark = badge::pc; return i18n::tr(str::CFGU_NOTE_DATABASE);
            case origin::console:
                mark = badge::ps4;
                if (const unsafe_option* u = unsafe_option_for(*m_cfg, *s.leaf)) return std::string(i18n::tr(str::COM_INDISPONIVEL_PS4)) + i18n::tr(u->menu);
                return i18n::tr(str::SET_LIMITE_PS4_BOOT_USA_MAXIMO);
            case origin::switch_:
                mark = badge::switch_;
                if (s.leaf == &m_cfg->io.pad_sleep) return i18n::trf(str::CFGU_FOLLOWS_SWITCH, {str::CFGU_SWITCH_COOL});
                return i18n::trf(str::CFGU_FOLLOWS_SWITCH, {switch_name(redirect_of(s) != ps4_switch::none ? redirect_of(s) : ps4_switch::online)});
            case origin::adapt:
                mark = badge::adapt;
                if (s.leaf == &m_cfg->video.vk.vram_allocation_limit) return i18n::trf(str::SET_AJUSTE_AUTOMATICO_DESTE_JOGO_APOS, {vram_low_cap_mb});
                if (s.leaf == &m_cfg->video.shadermode) return i18n::tr(str::SET_AJUSTE_AUTOMATICO_SHADERS);
                if (s.leaf == &m_cfg->net.net_active || s.leaf == &m_cfg->net.psn_status) return i18n::tr(str::SET_ONLINE_PULADO_NESTE_JOGO_APOS);
                return i18n::tr(str::SET_AJUSTE_AUTOMATICO_DESTE_JOGO_APOS_2);
            case origin::runtime:
                mark = badge::ps4;
                if (s.leaf == &m_cfg->video.vk.vram_allocation_limit) return i18n::trf(str::SET_BOOT_LIMITA_VRAM_MB_VRAM, {vram_cap_mb});
                if (s.leaf == &m_cfg->vfs.host_root) return i18n::tr(str::SET_SEMPRE_DESLIGADO_PS4_JOGO_NAO);
                if (s.leaf == &m_cfg->vfs.limit_cache_size || s.leaf == &m_cfg->vfs.cache_max_size)
                    return i18n::trf(str::SET_BOOT_MANTEM_LIMITE_CACHE_ENTRE, {disk_cache_floor_mb, disk_cache_max_mb});
                if (s.leaf == &m_cfg->video.shader_compiler_threads_count) return i18n::tr(str::CFGR_LIMIT_2_THREADS);
                return i18n::tr(str::SET_BOOT_APLICA_LIMITE_PS4);
            case origin::hardware:
                mark = badge::ps4;
                return i18n::tr(game_scope() ? str::SET_SEM_FMA_JAGUAR_SALVAR_LIGA : str::SET_SEM_FMA_JAGUAR_DESLIGADO_SEM);
            case origin::saved:
            case origin::pinned: break;
            }
        }
        // Per game, DFMA turned on here: Salvar writes the game's marker (software fma, slower on Jaguar)
        if (m_mode != policy_mode::keep_user_settings && s.leaf == &m_cfg->core.use_accurate_dfma && game_scope() && s.edited && !s.reset &&
            m_cfg->core.use_accurate_dfma.get() && !utils::has_fma3())
        {
            mark = badge::mine;
            return i18n::tr(str::SET_SEM_FMA_JAGUAR_SALVAR_LIGA);
        }
        if (pinned(s))
        {
            mark = badge::mine;
            return i18n::tr(str::CFGU_NOTE_MINE);
        }
        if (game_scope() && differs_global(s))
        {
            mark = badge::this_game;
            return i18n::trf(str::CFGU_NOTE_GLOBAL, {i18n::raw(shown_value(s, s.global))});
        }
        if (!s.meta) return i18n::tr(str::CFGU_NEW_SETTING_HELP);
        return i18n::tr(s.meta->help);
    }

    // Per game: a value no policy step replaced that differs from the global one
    bool differs_global(const state& s) const
    {
        return game_scope() && !s.changed && detail::canonical(*s.leaf) != s.global;
    }

    static i18n::str badge_name(badge b)
    {
        using i18n::str;
        switch (b)
        {
        case badge::automatic: return str::CFGU_BADGE_AUTO;
        case badge::game: return str::CFGU_BADGE_GAME;
        case badge::pc: return str::CFGU_BADGE_PC;
        case badge::ps4: return str::CFGU_BADGE_PS4;
        case badge::switch_: return str::CFGU_BADGE_SWITCH;
        case badge::adapt: return str::CFGU_BADGE_ADAPT;
        case badge::mine: return str::CFGU_BADGE_MINE;
        case badge::this_game: return str::CFGU_BADGE_THIS_GAME;
        case badge::none: break;
        }
        return str::count_;
    }

    void build_page()
    {
        m_page.clear();
        m_page_states.clear();
        const auto caption = [&](i18n::str section)
        {
            row r;
            r.section = true;
            r.label = i18n::tr(section);
            m_page.push_back(std::move(r));
            m_page_states.push_back(npos);
        };
        const auto add = [&](std::size_t idx)
        {
            const state& s = m_states[idx];
            row r;
            r.meta = s.meta;
            r.leaf = s.leaf;
            r.key = s.key;
            r.label = label_of(s);
            r.value = value_text(s);
            r.help = help_of(s, r.mark);
            r.badge_text = r.mark != badge::none ? i18n::tr(badge_name(r.mark)) : "";
            r.from = s.changed ? s.from : pinned(s) ? origin::pinned : origin::saved;
            r.kind = s.meta ? s.meta->kind : klass::warn;
            r.redirect = redirect_of(s);
            r.keyboard = keyboard_of(s);
            r.page = page_of(s);
            r.pinned = pinned(s);
            r.differs_global = differs_global(s);
            r.locked = is_locked(s);
            r.dimmed = scope_blocked(s) || !failing_dependency(s).empty();
            r.warn = r.kind == klass::warn;
            r.dynamic = s.leaf->get_is_dynamic();
            if (s.meta && (s.meta->flags & cat::flag::home_menu)) r.note = i18n::tr(i18n::str::CFGU_NOTE_HOME_MENU);
            else if (r.dynamic) r.note = i18n::tr(i18n::str::CFGU_NOTE_DYNAMIC);
            const editor e = s.meta ? s.meta->edit : editor::auto_;
            r.picker = !r.locked && (s.kind == detail::vkind::enumeration || e == editor::zcull || e == editor::font || e == editor::country);
            m_page.push_back(std::move(r));
            m_page_states.push_back(idx);
        };
        i18n::str last = i18n::str::count_;
        for (const cat::item& item : cat::items())
        {
            if (item.where != m_page_tab || item.edit == editor::zcull_member) continue;
            if (item.kind == klass::hidden && !m_page_hidden) continue;
            const state* s = by_key_const(item.key);
            if (!s) continue;
            if (item.section != last)
            {
                caption(item.section);
                last = item.section;
            }
            add(index_of(*s));
        }
        bool other = false;
        for (std::size_t idx = 0; idx < m_states.size(); ++idx)
        {
            const state& s = m_states[idx];
            if (s.meta || cat::default_tab(s.key) != m_page_tab) continue;
            if (!other)
            {
                caption(i18n::str::CFGG_OTHER);
                other = true;
            }
            add(idx);
        }
    }

    // --- edits ---

    template <typename F>
    std::string with_row(std::size_t i, F&& f, bool is_change = true)
    {
        if (!m_cfg || i >= m_page_states.size() || m_page_states[i] == npos) return {};
        m_last_row = i;
        return apply(m_states[m_page_states[i]], f, is_change);
    }

    template <typename F>
    std::string with_key(std::string_view key, F&& f, bool is_change = true)
    {
        state* s = by_key(key);
        if (!m_cfg || !s) return {};
        m_last_row = SIZE_MAX;
        for (std::size_t i = 0; i < m_page_states.size(); ++i)
            if (m_page_states[i] == index_of(*s)) m_last_row = i;
        return apply(*s, f, is_change);
    }

    template <typename F>
    std::string apply(state& s, F& f, bool is_change)
    {
        using i18n::str;
        if (s.meta && s.meta->kind == klass::unavailable)
            return std::string(i18n::tr(str::COM_INDISPONIVEL_PS4)) + (s.meta->reason != str::count_ ? i18n::tr(s.meta->reason) : "");
        if (s.meta && s.meta->kind == klass::hidden) return i18n::trf(str::CFGU_HIDDEN_ROW, {str(s.meta->reason)});
        if (s.meta && s.meta->edit == editor::readonly) return i18n::tr(str::CFGU_READONLY);
        if (const ps4_switch w = redirect_of(s); w != ps4_switch::none && is_change) return i18n::trf(str::CFGU_FOLLOWS_SWITCH, {switch_name(w)});
        if (is_change && scope_blocked(s)) return i18n::tr(s.meta->where_scope == cat::scope::game_only ? str::CFGU_NOTE_GAME_ONLY : str::CFGU_NOTE_GLOBAL_ONLY);
        if (is_change)
            if (const std::string dep = failing_dependency(s); !dep.empty())
            {
                const state* other = by_key_const(dep);
                return i18n::trf(str::CFGU_NOTE_DEPENDS, {i18n::raw(other ? label_of(*other) : dep)});
            }
        // A warn row (and a setting the catalog lacks) asks once per session
        if (is_change && (!s.meta || s.meta->kind == klass::warn) && !m_confirmed.count(s.key))
            return ask(s.key, s.meta ? s.meta->reason : str::CFGU_NEW_SETTING_HELP);
        return f(s);
    }

    std::string ask(std::string token, i18n::str reason)
    {
        m_pending_token = std::move(token);
        m_confirm = m_last_row;
        return std::string(i18n::tr(i18n::str::CFGU_WARN_TITLE)) + ": " + (reason != i18n::str::count_ ? i18n::tr(reason) : "");
    }

    // After a value changed: remember it as this session's choice, run the dependent rules and the preview, and say what
    // the boot will run
    std::string changed(state& s)
    {
        s.chosen = detail::canonical(*s.leaf);
        s.edited = true;
        s.reset = false;
        s.unpinned = false;
        std::string extra = dependent_rules(s);
        refresh();
        std::string message = i18n::trf(i18n::str::CFGU_CHANGED, {i18n::raw(label_of(s)), i18n::raw(value_text(s))});
        if (!extra.empty()) message += " \xC2\xB7 " + extra;
        if (const std::string note = boot_note(s); !note.empty()) message += " \xC2\xB7 " + note;
        return message;
    }

    // What the boot does to a chosen value (empty when it runs as chosen)
    std::string boot_note(const state& s) const
    {
        if (!s.changed || m_mode == policy_mode::keep_user_settings) return {};
        switch (s.from)
        {
        case origin::console:
        case origin::runtime:
        case origin::switch_:
        case origin::adapt:
        case origin::hardware:
        {
            badge b;
            return help_of(s, b);
        }
        default: break;
        }
        return {};
    }

    // Renderer -> Shader Mode and the ZCULL pair (the settings menu's rule since 0.29); returns what changed
    std::string dependent_rules(state& s)
    {
        if (!m_cfg || s.leaf != &m_cfg->video.renderer) return {};
        auto& v = m_cfg->video;
        std::string note;
        const auto mark = [&](cfg::_base& leaf)
        {
            const auto it = m_by_leaf.find(&leaf);
            if (it == m_by_leaf.end()) return;
            state& other = m_states[it->second];
            other.chosen = detail::canonical(leaf);
            other.edited = true;
            other.reset = false;
        };
        const bool vulkan = v.renderer == video_renderer::vulkan;
        if (vulkan && v.shadermode == shader_mode::recompiler)
        {
            v.shadermode.set(profile_vulkan_shader_mode());
            mark(v.shadermode);
            note = i18n::tr(i18n::str::SET_SHADERS_ASSINCRONOS);
        }
        else if (!vulkan && v.shadermode != profile_opengl_shader_mode())
        {
            v.shadermode.set(profile_opengl_shader_mode());
            mark(v.shadermode);
            note = i18n::tr(v.shadermode == shader_mode::recompiler ? i18n::str::SET_SHADERS_SINCRONOS_GL_ASSINCRONO_NAO : i18n::str::SET_SHADERS_ASSINCRONOS_GL_FLAG);
        }
        const bool relaxed_now = profile_zcull_relaxed(v.renderer);
        const bool relaxed_before = profile_zcull_relaxed(vulkan ? video_renderer::opengl : video_renderer::vulkan);
        if (relaxed_now != relaxed_before && v.relaxed_zcull_sync.get() == relaxed_before && v.precise_zpass_count.get() == !relaxed_before &&
            !title_entry_for(*m_cfg, m_title, v.relaxed_zcull_sync) && !title_entry_for(*m_cfg, m_title, v.precise_zpass_count))
        {
            v.relaxed_zcull_sync.set(relaxed_now);
            v.precise_zpass_count.set(!relaxed_now);
            mark(v.relaxed_zcull_sync);
            mark(v.precise_zpass_count);
            if (!note.empty()) note += " \xC2\xB7 ";
            note += i18n::tr(relaxed_now ? i18n::str::SET_ZCULL_RELAXADO_OPENGL : i18n::str::SET_ZCULL_ESTRITO_VULKAN);
        }
        return note;
    }

    // A value about to be set from a step / pick / keyboard: the row's rules (greyed = refused, warn = asks once)
    std::string admit(state& s, std::string_view spelling, bool& ok)
    {
        ok = false;
        i18n::str reason;
        const detail::verdict v = judge(s, spelling, reason);
        if (v == detail::verdict::greyed || v == detail::verdict::hidden)
            return i18n::trf(i18n::str::CFGU_VALUE_REFUSED, {i18n::raw(value_label(s, spelling)), reason != i18n::str::count_ ? reason : i18n::str::CFGU_READONLY});
        if (v == detail::verdict::warn && !m_confirmed.count(s.key + "=" + std::string(spelling))) return ask(s.key + "=" + std::string(spelling), reason);
        ok = true;
        return {};
    }

    std::string admit_number(state& s, double value, bool& ok)
    {
        ok = false;
        const auto [lo, hi] = bounds_of(s);
        if (value < lo || value > hi)
            return i18n::trf(i18n::str::CFGU_OUT_OF_RANGE, {s.kind == detail::vkind::floating ? detail::short_float(lo) : std::to_string(static_cast<long long>(lo)),
                s.kind == detail::vkind::floating ? detail::short_float(hi) : s.kind == detail::vkind::unsigned_ ? std::to_string(static_cast<unsigned long long>(hi)) : std::to_string(static_cast<long long>(hi))});
        i18n::str reason = s.meta ? s.meta->value_reason : i18n::str::count_;
        const detail::verdict v = detail::judge_number(rules_of(s), value);
        if (v == detail::verdict::greyed || v == detail::verdict::hidden)
            return i18n::trf(i18n::str::CFGU_VALUE_REFUSED, {s.kind == detail::vkind::floating ? detail::short_float(value) : std::to_string(static_cast<long long>(value)),
                reason != i18n::str::count_ ? reason : i18n::str::CFGU_READONLY});
        if (v == detail::verdict::warn && !m_confirmed.count(s.key + "=~")) return ask(s.key + "=~", reason);
        ok = true;
        return {};
    }

    std::string set_number(state& s, double value)
    {
        bool ok = false;
        std::string refused = admit_number(s, value, ok);
        if (!ok) return refused;
        std::string text;
        if (s.kind == detail::vkind::floating) text = detail::float_text(value);
        else if (s.kind == detail::vkind::unsigned_) text = std::to_string(static_cast<unsigned long long>(value));
        else text = std::to_string(static_cast<long long>(value));
        if (!s.leaf->from_string(text)) return i18n::tr(i18n::str::CFGU_BAD_NUMBER);
        return changed(s);
    }

    std::vector<std::string> allowed_spellings(const state& s) const
    {
        std::vector<std::string> out;
        for (const std::string& spelling : s.leaf->to_list())
        {
            i18n::str reason;
            const detail::verdict v = judge(s, spelling, reason);
            if (v == detail::verdict::allowed || v == detail::verdict::warn) out.push_back(spelling);
        }
        return out;
    }

    std::string do_step(state& s, int direction, bool coarse)
    {
        const int dir = direction < 0 ? -1 : 1;
        const editor e = s.meta ? s.meta->edit : editor::auto_;
        switch (e)
        {
        case editor::zcull:
        {
            auto& v = m_cfg->video;
            // Precise -> Approximate -> Relaxed
            const int now = v.relaxed_zcull_sync ? 2 : v.precise_zpass_count ? 0 : 1;
            const int next = ((now + dir) % 3 + 3) % 3;
            return set_zcull(s, next);
        }
        case editor::mfc_delay:
            if (!s.leaf->from_string(s.leaf->to_string() == "0" ? "1" : "0")) return {};
            return changed(s);
        case editor::time_offset:
        {
            const long long now = detail::parse_s64(s.leaf->to_string()).value_or(0);
            return set_number(s, double(now + dir * (coarse ? 86400LL : 3600LL)));
        }
        case editor::font:
        case editor::country:
        {
            const auto list = choices_of(s);
            if (list.empty()) return {};
            std::size_t idx = 0;
            for (std::size_t c = 0; c < list.size(); ++c)
                if (list[c].current) idx = c;
            for (std::size_t n = 1; n <= list.size(); ++n)
            {
                const std::size_t c = (idx + list.size() + std::size_t(dir) * n) % list.size();
                if (list[c].allowed) return do_pick(s, c);
            }
            return {};
        }
        case editor::auto_: break;
        default: return {};
        }
        switch (s.kind)
        {
        case detail::vkind::boolean:
            if (!s.leaf->from_string(s.leaf->to_string() == "true" ? "false" : "true")) return {};
            return changed(s);
        case detail::vkind::enumeration:
        {
            const auto list = allowed_spellings(s);
            if (list.empty()) return {};
            const std::string now = s.leaf->to_string();
            const auto it = std::find(list.begin(), list.end(), now);
            const int idx = it == list.end() ? (dir > 0 ? -1 : 0) : int(it - list.begin());
            const int n = int(list.size());
            const std::string next = list[std::size_t(((idx + dir) % n + n) % n)];
            bool ok = false;
            std::string refused = admit(s, next, ok);
            if (!ok) return refused;
            if (!s.leaf->from_string(next)) return {};
            return changed(s);
        }
        case detail::vkind::integer:
        case detail::vkind::unsigned_:
        case detail::vkind::floating:
        {
            const auto rules = rules_of(s);
            const double now = number_of(s);
            if (const auto listed = detail::listed_numbers(rules); !listed.empty())
            {
                auto it = std::find(listed.begin(), listed.end(), now);
                const int idx = it == listed.end() ? (dir > 0 ? -1 : 0) : int(it - listed.begin());
                const int n = int(listed.size());
                return set_number(s, listed[std::size_t(((idx + dir) % n + n) % n)]);
            }
            const double base = s.meta && s.meta->step > 0 ? s.meta->step : 1;
            const double big = s.meta && s.meta->coarse > 0 ? s.meta->coarse : base * 10;
            const auto [lo, hi] = bounds_of(s);
            double lo_rule = lo, hi_rule = hi;
            // Stay inside the allowed (or warned) ranges of the rules
            bool any = false;
            for (const detail::rule& r : rules)
            {
                if (!r.range || r.mark == '-' || r.mark == '#') continue;
                lo_rule = any ? std::min(lo_rule, r.lo) : r.lo;
                hi_rule = any ? std::max(hi_rule, r.hi) : r.hi;
                any = true;
            }
            if (!any) { lo_rule = lo; hi_rule = hi; }
            double next = now + dir * (coarse ? big : base);
            // snap to the step grid (25 % steps land on 25, 50, ...)
            if (s.kind != detail::vkind::floating && base >= 1)
            {
                const double grid = coarse ? big : base;
                next = dir > 0 ? std::floor((now + grid) / grid) * grid : std::ceil((now - grid) / grid) * grid;
            }
            next = std::clamp(next, std::max(lo, lo_rule), std::min(hi, hi_rule));
            if (next == now) return i18n::trf(i18n::str::CFGU_CHANGED, {i18n::raw(label_of(s)), i18n::raw(value_text(s))});
            return set_number(s, next);
        }
        default: break;
        }
        return {};
    }

    std::string set_zcull(state& s, int mode)
    {
        auto& v = m_cfg->video;
        v.relaxed_zcull_sync.set(mode == 2);
        v.precise_zpass_count.set(mode == 0);
        if (const auto it = m_by_leaf.find(&v.precise_zpass_count); it != m_by_leaf.end())
        {
            state& member = m_states[it->second];
            member.chosen = detail::canonical(*member.leaf);
            member.edited = true;
            member.reset = false;
            member.unpinned = false;
        }
        return changed(s);
    }

    std::vector<choice> choices_of(const state& s) const
    {
        std::vector<choice> out;
        if (!s.leaf) return out;
        const editor e = s.meta ? s.meta->edit : editor::auto_;
        const std::string now = s.leaf->to_string();
        if (e == editor::zcull)
        {
            const int mode = m_cfg->video.relaxed_zcull_sync ? 2 : m_cfg->video.precise_zpass_count ? 0 : 1;
            const i18n::str labels[] = {i18n::str::CFGU_ZCULL_PRECISE, i18n::str::CFGU_ZCULL_APPROXIMATE, i18n::str::CFGU_ZCULL_RELAXED};
            for (int m = 0; m < 3; ++m) out.push_back({std::to_string(m), i18n::tr(labels[m]), {}, true, false, m == mode});
            return out;
        }
        if (e == editor::font)
        {
            std::vector<std::string> names = m_fonts;
            if (!now.empty() && std::find(names.begin(), names.end(), now) == names.end()) names.insert(names.begin(), now);
            for (const std::string& n : names) out.push_back({n, n, {}, true, false, n == now});
            return out;
        }
        if (e == editor::country)
        {
            for (const countries::country_code& c : countries::g_countries)
            {
                if (c.ccode.empty()) continue;
                out.push_back({std::string(c.ccode), std::string(c.name) + " (" + std::string(c.ccode) + ")", {}, true, false, c.ccode == now});
            }
            return out;
        }
        if (s.kind != detail::vkind::enumeration) return out;
        for (const std::string& spelling : s.leaf->to_list())
        {
            i18n::str reason;
            const detail::verdict v = judge(s, spelling, reason);
            if (v == detail::verdict::hidden) continue;
            choice c;
            c.spelling = spelling;
            c.label = value_label(s, spelling);
            c.allowed = v != detail::verdict::greyed;
            c.warn = v == detail::verdict::warn;
            c.current = spelling == now;
            if ((!c.allowed || c.warn) && reason != i18n::str::count_) c.reason = i18n::tr(reason);
            out.push_back(std::move(c));
        }
        return out;
    }

    std::string do_pick(state& s, std::size_t c)
    {
        const auto list = choices_of(s);
        if (c >= list.size()) return {};
        const editor e = s.meta ? s.meta->edit : editor::auto_;
        if (e == editor::zcull) return set_zcull(s, int(c));
        if (!list[c].allowed) return i18n::trf(i18n::str::CFGU_VALUE_REFUSED, {i18n::raw(list[c].label), i18n::raw(list[c].reason)});
        if (s.kind == detail::vkind::enumeration)
        {
            bool ok = false;
            std::string refused = admit(s, list[c].spelling, ok);
            if (!ok) return refused;
        }
        if (!s.leaf->from_string(list[c].spelling)) return i18n::trf(i18n::str::CFGU_BAD_TEXT, {list[c].spelling});
        return changed(s);
    }

    std::string do_set_text(state& s, std::string_view input)
    {
        using i18n::str;
        const std::string text(detail::trim(input));
        const editor e = s.meta ? s.meta->edit : editor::auto_;
        switch (keyboard_of(s))
        {
        case keyboard_kind::number:
        {
            const auto v = detail::parse_double(text);
            if (!v) return i18n::tr(str::CFGU_BAD_NUMBER);
            if (s.kind != detail::vkind::floating && *v != static_cast<double>(static_cast<long long>(*v))) return i18n::tr(str::CFGU_BAD_NUMBER);
            if (s.kind == detail::vkind::unsigned_)
            {
                // above 2^53 a double loses digits: parse as an integer
                if (const auto u = detail::parse_u64(text))
                {
                    const auto [lo, hi] = bounds_of(s);
                    if (double(*u) < lo || double(*u) > hi) return set_number(s, double(*u));
                    bool ok = false;
                    std::string refused = admit_number(s, double(*u), ok);
                    if (!ok) return refused;
                    if (!s.leaf->from_string(std::to_string(*u))) return i18n::tr(str::CFGU_BAD_NUMBER);
                    return changed(s);
                }
            }
            return set_number(s, *v);
        }
        case keyboard_kind::hex:
        {
            std::string hex = text;
            if (hex.size() >= 2 && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) hex = hex.substr(2);
            if (hex.empty() || hex.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) return i18n::tr(str::CFGU_BAD_NUMBER);
            if (!s.leaf->from_string("0x" + hex)) return i18n::tr(str::CFGU_BAD_NUMBER);
            return changed(s);
        }
        case keyboard_kind::date:
        {
            int y = 0, mo = 0, d = 0, h = 0, mi = 0;
            if (std::sscanf(text.c_str(), "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) != 5 || mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 || mi < 0 || mi > 59)
                return i18n::trf(str::CFGU_BAD_TEXT, {str::CFGU_BAD_DATE});
            std::tm tm{};
            tm.tm_year = y - 1900;
            tm.tm_mon = mo - 1;
            tm.tm_mday = d;
            tm.tm_hour = h;
            tm.tm_min = mi;
            tm.tm_isdst = -1;
            const std::time_t target = std::mktime(&tm);
            if (target == static_cast<std::time_t>(-1)) return i18n::trf(str::CFGU_BAD_TEXT, {str::CFGU_BAD_DATE});
            return set_number(s, double(static_cast<long long>(target) - static_cast<long long>(m_now())));
        }
        case keyboard_kind::text:
        {
            if (e == editor::ipv4 && !detail::ipv4(text)) return i18n::trf(str::CFGU_BAD_TEXT, {str::CFGU_BAD_IPV4});
            if (e == editor::swap_list && !detail::swap_list(text)) return i18n::trf(str::CFGU_BAD_TEXT, {str::CFGU_BAD_SWAP_LIST});
            if (e == editor::color && !detail::color(text)) return i18n::trf(str::CFGU_BAD_TEXT, {str::CFGU_BAD_COLOR});
            for (const char c : text)
                if (static_cast<unsigned char>(c) < 0x20) return i18n::trf(str::CFGU_BAD_TEXT, {text});
            if (!s.leaf->from_string(text)) return i18n::trf(str::CFGU_BAD_TEXT, {text});
            return changed(s);
        }
        case keyboard_kind::none: break;
        }
        return {};
    }

    // The value a key starts from when the player chose nothing: per game the global value, else the RPCS3 default
    // (a random default such as System Name keeps its saved value)
    std::string baseline_value(const state& s) const
    {
        if (game_scope()) return s.global;
        if (s.meta && (s.meta->flags & cat::flag::no_default)) return s.saved;
        if (s.kind == detail::vkind::set || s.kind == detail::vkind::map || s.kind == detail::vkind::log) return {};
        return s.leaf->def_to_string();
    }

    std::string do_reset_auto(state& s)
    {
        s.chosen = baseline_value(s);
        s.edited = true;
        s.reset = true;
        s.force_pin = false;
        s.unpinned = false;
        detail::restore(*s.leaf, *s.chosen);
        dependent_rules(s);
        refresh();
        return i18n::trf(i18n::str::CFGU_RESET_AUTO_DONE, {i18n::raw(label_of(s)), i18n::raw(value_text(s))});
    }

    std::string do_reset_upstream(state& s)
    {
        if (s.meta && (s.meta->flags & cat::flag::no_default)) return i18n::tr(i18n::str::CFGU_NO_DEFAULT);
        const std::string def = s.kind == detail::vkind::set || s.kind == detail::vkind::map || s.kind == detail::vkind::log ? std::string{} : s.leaf->def_to_string();
        if (!detail::restore(*s.leaf, def)) return i18n::tr(i18n::str::CFGU_BAD_NUMBER);
        changed(s);
        return i18n::trf(i18n::str::CFGU_RESET_UPSTREAM_DONE, {i18n::raw(label_of(s)), i18n::raw(value_text(s))});
    }

    std::string do_use_global(state& s)
    {
        if (!game_scope()) return i18n::tr(i18n::str::CFGU_NOTE_GLOBAL_ONLY);
        s.chosen = s.global;
        s.edited = true;
        s.reset = true;
        s.force_pin = false;
        s.unpinned = false;
        detail::restore(*s.leaf, s.global);
        refresh();
        return i18n::trf(i18n::str::CFGU_USE_GLOBAL_DONE, {i18n::raw(label_of(s)), i18n::raw(value_text(s))});
    }

    std::string do_toggle_pin(state& s)
    {
        if (pinned(s))
        {
            s.force_pin = false;
            if (s.edited && !s.reset)
            {
                // a value chosen in this session stays, but the automatic configuration may change it again
                s.edited = false;
                s.chosen.reset();
                s.saved = detail::canonical(*s.leaf);
            }
            s.unpinned = true;
            refresh();
            return i18n::trf(i18n::str::CFGU_UNPINNED, {i18n::raw(label_of(s))});
        }
        s.force_pin = true;
        s.unpinned = false;
        s.reset = false;
        if (!s.chosen) s.chosen = detail::canonical(*s.leaf);
        s.edited = true;
        refresh();
        return i18n::trf(i18n::str::CFGU_PINNED, {i18n::raw(label_of(s))});
    }

    // --- the preview ---

    bool dfma_chosen_for_game() const
    {
        if (!game_scope() || !m_cfg) return false;
        const auto it = m_by_leaf.find(&m_cfg->core.use_accurate_dfma);
        return it != m_by_leaf.end() && m_states[it->second].edited && !m_states[it->second].reset;
    }

    // Every leaf back to its saved value, or to this session's choice
    void apply_base()
    {
        for (state& s : m_states)
        {
            const std::string& want = s.chosen ? *s.chosen : s.saved;
            if (detail::canonical(*s.leaf) != want) detail::restore(*s.leaf, want);
        }
    }

    policy_result run_preview(bool record_changes, const pin_set* pins_override)
    {
        apply_base();
        if (record_changes)
            for (state& s : m_states)
            {
                s.changed = false;
                s.has_runtime_before = false;
                s.from = origin::saved;
                s.why.clear();
            }
        const pin_set pins = pins_override ? *pins_override : current_pins();
        const auto recorder = [&](const cfg::_base& setting, const std::string& old, std::string_view why, change_kind kind)
        {
            if (record_changes) record(setting, old, why, kind);
        };
        const policy_result result = apply_boot_policy_preview(*m_cfg, m_title, m_inputs, recorder, &pins);
        // Per game, a DFMA value chosen here runs as chosen: Salvar writes the game's accurate-dfma marker from it
        if (!dfma_chosen_for_game())
        {
            apply_hardware_limits(*m_cfg, m_title, [&](const cfg::_base& setting, const std::string& old, std::string_view why)
            {
                if (record_changes) record(setting, old, why, change_kind::runtime);
            });
        }
        return result;
    }

    void refresh()
    {
        if (!m_cfg) return;
        m_result = run_preview(true, nullptr);
        update_pin_candidates();
        m_dirty = true;
    }

    // The value the boot runs for state 'i' when the player chose nothing for it in the bound scope (dry run; the staged
    // copy is put back)
    std::string automatic_value(std::size_t i)
    {
        state& s = m_states[i];
        const auto staged = snapshot();
        const std::optional<std::string> chosen = s.chosen;
        s.chosen = baseline_value(s);
        pin_set pins = current_pins();
        auto& list = game_scope() ? pins.game : pins.global;
        list.erase(std::remove(list.begin(), list.end(), s.key), list.end());
        run_preview(false, &pins);
        const std::string value = detail::canonical(*s.leaf);
        s.chosen = chosen;
        restore_snapshot(staged);
        return value;
    }

    // Every leaf's value, and the staged copy put back to it (cheaper than a YAML round trip of the whole tree)
    std::vector<std::string> snapshot() const
    {
        std::vector<std::string> out;
        out.reserve(m_states.size());
        for (const state& s : m_states) out.push_back(detail::canonical(*s.leaf));
        return out;
    }

    void restore_snapshot(const std::vector<std::string>& values)
    {
        for (std::size_t i = 0; i < m_states.size() && i < values.size(); ++i)
            if (detail::canonical(*m_states[i].leaf) != values[i]) detail::restore(*m_states[i].leaf, values[i]);
    }

    // The values the runtime section or the adaptation replaced, put back (they are forced again at every boot)
    void restore_runtime_values()
    {
        for (state& s : m_states)
            if (s.has_runtime_before && (s.from == origin::runtime || s.from == origin::switch_ || s.from == origin::adapt))
                detail::restore(*s.leaf, s.runtime_before);
    }
};
} // namespace utils::ps4::cfg_browser
