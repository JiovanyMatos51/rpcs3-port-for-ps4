#pragma once

// Host-test stand-in for Utilities/Config.h (tests/cfg_browser_test.cpp): the real entry classes with the same
// constructors, types, ranges, defaults, dynamic flags and text forms, so the REAL Emu/system_config.h builds the whole
// tree (cfg-mocks/Emu/system_config.h). Differences from Utilities/Config.cpp, which needs the emulator's logging and
// yaml-cpp: enum spellings come from cfg_enum_names.inc (generated from the fmt_class_string tables), floats print with
// %.17g (the RPCS3_PS4 branch of try_to_string), and node::to_string()/from_string()/validate() use a small YAML
// emitter/parser for the block style cfg writes (maps, '- item' sequences, [] and {}, quoted scalars) with the real
// cfg::decode rules: incremental (a missing key keeps its value), unknown keys skipped unless strict, dynamic-only reload.

#include "util/logs.hpp"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s8 = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;
using u128 = __uint128_t;
using f64 = double;
using usz = std::size_t;
using ullong = unsigned long long;

// The YAML spellings of an enum: specialised by cfg_enum_names.inc
namespace cfg_mock
{
template <typename T>
struct enum_names;

// Spelling of value 'v', or empty
template <typename T>
std::string_view spelling(u64 v)
{
    for (const auto& [value, name] : enum_names<T>::values)
        if (static_cast<u64>(static_cast<std::underlying_type_t<T>>(value)) == v) return name;
    return {};
}

// A parsed YAML node of the block style cfg writes
struct ynode
{
    enum class kind : u8 { null, scalar, map, seq } k = kind::null;
    std::string text;
    std::vector<std::pair<std::string, ynode>> items; // map entries
    std::vector<ynode> list;                          // sequence items
};

inline std::string quote_scalar(std::string_view s)
{
    bool plain = !s.empty() && s.front() != ' ' && s.back() != ' ';
    if (plain && std::string_view("@#!&*-?{}[],|>%'\"`").find(s.front()) != std::string_view::npos) plain = false;
    if (plain && (s.find(": ") != std::string_view::npos || s.find(" #") != std::string_view::npos || s.back() == ':')) plain = false;
    for (const char c : s)
        if (static_cast<unsigned char>(c) < 0x20) plain = false;
    if (plain) return std::string(s);
    std::string out = "\"";
    for (const char c : s)
    {
        if (c == '"' || c == '\\') out += '\\';
        if (c == '\n') { out += "\\n"; continue; }
        out += c;
    }
    return out + "\"";
}

// One scalar at the start of 's' (quoted or plain up to a flow separator in flow context)
inline std::string take_scalar(std::string_view& s, bool flow)
{
    while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
    std::string out;
    if (!s.empty() && (s.front() == '"' || s.front() == '\''))
    {
        const char q = s.front();
        std::size_t i = 1;
        for (; i < s.size(); i++)
        {
            const char c = s[i];
            if (q == '"' && c == '\\' && i + 1 < s.size())
            {
                const char e = s[++i];
                out += e == 'n' ? '\n' : e == 't' ? '\t' : e;
                continue;
            }
            if (q == '\'' && c == '\'' && i + 1 < s.size() && s[i + 1] == '\'') { out += '\''; i++; continue; }
            if (c == q) { i++; break; }
            out += c;
        }
        s.remove_prefix(i);
        return out;
    }
    std::size_t end = s.size();
    if (const auto h = s.find(" #"); h != std::string_view::npos) end = h;
    if (flow)
        if (const auto at = s.substr(0, end).find_first_of(",]}"); at != std::string_view::npos) end = at;
    out = std::string(s.substr(0, end));
    while (!out.empty() && out.back() == ' ') out.pop_back();
    s.remove_prefix(end);
    return out;
}

inline ynode flow_value(std::string_view s)
{
    ynode n;
    while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
    if (s.starts_with("[") || s.starts_with("{"))
    {
        const bool seq = s.front() == '[';
        n.k = seq ? ynode::kind::seq : ynode::kind::map;
        s.remove_prefix(1);
        while (true)
        {
            while (!s.empty() && (s.front() == ' ' || s.front() == ',')) s.remove_prefix(1);
            if (s.empty() || s.front() == ']' || s.front() == '}') break;
            ynode item;
            item.k = ynode::kind::scalar;
            if (seq)
            {
                item.text = take_scalar(s, true);
                n.list.push_back(std::move(item));
            }
            else
            {
                const auto colon = s.find(':');
                if (colon == std::string_view::npos) break;
                std::string_view key = s.substr(0, colon);
                std::string k = take_scalar(key, true);
                s.remove_prefix(colon + 1);
                item.text = take_scalar(s, true);
                n.items.emplace_back(std::move(k), std::move(item));
            }
        }
        return n;
    }
    if (s.empty() || s == "~" || s == "null") return n;
    n.k = ynode::kind::scalar;
    n.text = take_scalar(s, false);
    return n;
}

struct yline
{
    int indent;
    std::string_view body;
};

// Block YAML (2-space indents as cfg writes, any indent accepted) into a node tree
inline ynode parse_block(const std::vector<yline>& lines, std::size_t& i, int indent)
{
    ynode n;
    if (i >= lines.size() || lines[i].indent < indent) return n;
    const int own = lines[i].indent;
    if (lines[i].body.starts_with("- ") || lines[i].body == "-")
    {
        n.k = ynode::kind::seq;
        while (i < lines.size() && lines[i].indent == own && (lines[i].body.starts_with("- ") || lines[i].body == "-"))
        {
            std::string_view rest = lines[i].body.substr(1);
            n.list.push_back(flow_value(rest));
            i++;
        }
        return n;
    }
    n.k = ynode::kind::map;
    while (i < lines.size() && lines[i].indent == own)
    {
        std::string_view body = lines[i].body;
        if (body.starts_with("- ")) break;
        std::string key;
        std::size_t colon;
        if (body.front() == '"' || body.front() == '\'')
        {
            std::string_view b = body;
            key = take_scalar(b, false);
            colon = body.size() - b.size();
            if (colon >= body.size() || body[colon] != ':') { i++; continue; }
        }
        else
        {
            colon = body.find(": ");
            if (colon == std::string_view::npos && body.back() == ':') colon = body.size() - 1;
            if (colon == std::string_view::npos) { i++; continue; }
            key = std::string(body.substr(0, colon));
        }
        std::string_view value = body.substr(colon + 1);
        while (!value.empty() && value.front() == ' ') value.remove_prefix(1);
        i++;
        if (value.empty() || value.front() == '#')
        {
            if (i < lines.size() && (lines[i].indent > own || (lines[i].indent == own && lines[i].body.starts_with("- "))))
                n.items.emplace_back(std::move(key), parse_block(lines, i, lines[i].indent));
            else
                n.items.emplace_back(std::move(key), ynode{});
            continue;
        }
        n.items.emplace_back(std::move(key), flow_value(value));
    }
    return n;
}

inline bool parse_yaml(std::string_view text, ynode& out)
{
    std::vector<yline> lines;
    while (!text.empty())
    {
        const auto nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text.remove_prefix(nl == std::string_view::npos ? text.size() : nl + 1);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
        int indent = 0;
        while (std::size_t(indent) < line.size() && line[indent] == ' ') indent++;
        line.remove_prefix(indent);
        if (line.empty() || line.front() == '#' || line == "---" || line == "...") continue;
        lines.push_back({indent, line});
    }
    if (lines.empty()) { out = ynode{}; out.k = ynode::kind::map; return true; }
    if (lines.size() == 1 && (lines[0].body == "{}" || lines[0].body == "~" || lines[0].body == "null")) { out = ynode{}; out.k = ynode::kind::map; return true; }
    std::size_t i = 0;
    out = parse_block(lines, i, lines[0].indent);
    return i == lines.size();
}
} // namespace cfg_mock

namespace cfg
{
enum class type : unsigned
{
    node = 0,
    _bool,
    _enum,
    _int,
    uint,
    uint128,
    string,
    set,
    map,
    node_map,
    log,
    device,
};

class node;

class _base
{
    const type m_type{};

protected:
    _base* m_parent = nullptr;
    bool m_dynamic = true;
    const std::string m_name{};

    explicit _base(type t) : m_type(t) {}
    _base(type t, node* owner, std::string name, bool dynamic);

public:
    _base(const _base&) = delete;
    _base& operator=(const _base&) = delete;
    virtual ~_base() = default;

    _base* get_parent() const { return m_parent; }
    type get_type() const { return m_type; }
    const std::string& get_name() const { return m_name; }
    bool get_is_dynamic() const { return m_dynamic; }
    virtual void from_default() = 0;
    virtual void restore_defaults() = 0;
    virtual std::string to_string() const { return {}; }
    virtual std::string def_to_string() const { return {}; }
    virtual bool from_string(std::string_view, bool /*dynamic*/ = false) { return false; }
    virtual std::vector<std::string> to_list() const { return {}; }
    virtual bool from_list(std::vector<std::string>&&) { return false; }
};

class node : public _base
{
    std::vector<_base*> m_nodes{};
    friend class _base;

public:
    node() : _base(type::node) {}
    node(node* owner, std::string name, bool dynamic = true) : _base(type::node, owner, std::move(name), dynamic) {}

    const std::vector<_base*>& get_nodes() const { return m_nodes; }
    std::string to_string() const override;
    bool from_string(std::string_view value, bool dynamic = false) override;
    void from_default() override
    {
        for (_base* n : m_nodes) n->from_default();
    }
    void restore_defaults() override
    {
        for (_base* n : m_nodes) n->restore_defaults();
    }
    bool validate(std::string_view value);
};

inline _base::_base(type t, node* owner, std::string name, bool dynamic)
    : m_type(t), m_parent(owner), m_dynamic(dynamic), m_name(std::move(name))
{
    for (const _base* n : owner->m_nodes)
        if (n->get_name() == m_name) throw std::logic_error("Node already exists: " + m_name);
    owner->m_nodes.push_back(this);
}

namespace detail
{
inline bool to_int64(s64* out, std::string_view value, s64 min, s64 max)
{
    if (value.empty()) return false;
    const char* start = value.data();
    const char* end = start + value.size();
    int base = 10;
    int sign = 1;
    if (start[0] == '-') { sign = -1; start++; }
    if (start < end && start[0] == '0' && end - start >= 2 && (start[1] == 'x' || start[1] == 'X')) { base = 16; start += 2; }
    s64 result = 0;
    const auto r = std::from_chars(start, end, result, base);
    if (r.ec != std::errc() || r.ptr != end || (start < end && start[0] == '-' && sign < 0)) return false;
    result *= sign;
    if (result < min || result > max) return false;
    if (out) *out = result;
    return true;
}

inline bool to_uint64(u64* out, std::string_view value, u64 min, u64 max)
{
    if (value.empty()) return false;
    const char* start = value.data();
    const char* end = start + value.size();
    int base = 10;
    if (start[0] == '0' && value.size() >= 2 && (start[1] == 'x' || start[1] == 'X')) { base = 16; start += 2; }
    u64 result = 0;
    const auto r = std::from_chars(start, end, result, base);
    if (r.ec != std::errc() || r.ptr != end) return false;
    if (result < min || result > max) return false;
    if (out) *out = result;
    return true;
}

inline bool to_float(f64* out, std::string_view value, f64 min, f64 max)
{
    if (value.empty()) return false;
    const std::string s(value);
    char* end = nullptr;
    const double result = std::strtod(s.c_str(), &end);
    if (end != s.c_str() + s.size()) return false;
    if (result < min || result > max) return false;
    if (out) *out = result;
    return true;
}

inline std::string float_text(f64 v)
{
    char buf[32];
    const int n = std::snprintf(buf, sizeof(buf), "%.17g", v);
    return n > 0 ? std::string(buf, std::size_t(n)) : "0.0";
}
} // namespace detail

class _bool final : public _base
{
    bool m_value;
    bool original_def;

public:
    bool def;

    _bool(node* owner, std::string name, bool def = false, bool dynamic = false)
        : _base(type::_bool, owner, std::move(name), dynamic), m_value(def), original_def(def), def(def)
    {
    }
    explicit operator bool() const { return m_value; }
    bool get() const { return m_value; }
    void from_default() override { m_value = def; }
    void restore_defaults() override { def = original_def; }
    std::string to_string() const override { return m_value ? "true" : "false"; }
    std::string def_to_string() const override { return def ? "true" : "false"; }
    bool from_string(std::string_view value, bool = false) override
    {
        if (value.size() != 4 && value.size() != 5) return false;
        std::string lower(value);
        for (char& c : lower) c = char(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        if (lower == "true") m_value = true;
        else if (lower == "false") m_value = false;
        else return false;
        return true;
    }
    void set(const bool& value) { m_value = value; }
};

template <typename T>
class _enum : public _base
{
    T m_value;
    T original_def;

    static u64 max_index()
    {
        u64 i = 0;
        while (!cfg_mock::spelling<T>(i).empty()) i++;
        return i;
    }

public:
    T def;

    _enum(node* owner, const std::string& name, T def = {}, bool dynamic = false)
        : _base(type::_enum, owner, name, dynamic), m_value(def), original_def(def), def(def)
    {
    }
    operator T() const { return m_value; }
    T get() const { return m_value; }
    T get_default() const { return def; }
    void set(T value) { m_value = value; }
    void from_default() override { m_value = def; }
    void restore_defaults() override { def = original_def; }
    static std::string text(T v)
    {
        const u64 raw = static_cast<u64>(static_cast<std::underlying_type_t<T>>(v));
        const std::string_view s = cfg_mock::spelling<T>(raw);
        if (!s.empty()) return std::string(s);
        char buf[24];
        std::snprintf(buf, sizeof(buf), "0x%llx", static_cast<unsigned long long>(raw));
        return buf;
    }
    std::string to_string() const override { return text(m_value); }
    std::string def_to_string() const override { return text(def); }
    // Like cfg::try_to_enum_value: the spellings of 0, 1, ... up to the first value without one, then a number
    bool from_string(std::string_view value, bool = false) override
    {
        const u64 count = max_index();
        for (u64 i = 0; i < count; i++)
        {
            if (cfg_mock::spelling<T>(i) == value)
            {
                m_value = static_cast<T>(static_cast<std::underlying_type_t<T>>(i));
                return true;
            }
        }
        u64 n = 0;
        if (!detail::to_uint64(&n, value, 0, count ? count - 1 : 0) || !count) return false;
        m_value = static_cast<T>(static_cast<std::underlying_type_t<T>>(n));
        return true;
    }
    std::vector<std::string> to_list() const override
    {
        std::vector<std::string> out;
        for (u64 i = 0; !cfg_mock::spelling<T>(i).empty(); i++) out.emplace_back(cfg_mock::spelling<T>(i));
        return out;
    }
};

template <s64 Min, s64 Max>
class _int final : public _base
{
    static_assert(Min < Max, "Invalid cfg::_int range");
    using int_type = std::conditional_t<Min >= INT32_MIN && Max <= INT32_MAX, s32, s64>;
    int_type m_value;
    int_type original_def;

public:
    int_type def;
    static constexpr s64 max = Max;
    static constexpr s64 min = Min;

    _int(node* owner, const std::string& name, int_type def = std::min<int_type>(Max, std::max<int_type>(Min, 0)), bool dynamic = false)
        : _base(type::_int, owner, name, dynamic), m_value(def), original_def(def), def(def)
    {
    }
    operator int_type() const { return m_value; }
    int_type get() const { return m_value; }
    void from_default() override { m_value = def; }
    void restore_defaults() override { def = original_def; }
    std::string to_string() const override { return std::to_string(m_value); }
    std::string def_to_string() const override { return std::to_string(def); }
    bool from_string(std::string_view value, bool = false) override
    {
        s64 result;
        if (!detail::to_int64(&result, value, Min, Max)) return false;
        m_value = static_cast<int_type>(result);
        return true;
    }
    void set(const s64& value)
    {
        if (value < Min || value > Max) throw std::out_of_range("'" + m_name + "': value out of bounds");
        m_value = static_cast<int_type>(value);
    }
    std::vector<std::string> to_list() const override { return {std::to_string(Min), std::to_string(Max)}; }
};

template <s32 Min, s32 Max>
class _float final : public _base
{
    static_assert(Min < Max, "Invalid cfg::_float range");
    f64 m_value;
    f64 original_def;

public:
    f64 def;
    static constexpr f64 max = Max;
    static constexpr f64 min = Min;

    _float(node* owner, const std::string& name, f64 def = std::min<f64>(Max, std::max<f64>(Min, 0)), bool dynamic = false)
        : _base(type::_int, owner, name, dynamic), m_value(def), original_def(def), def(def)
    {
    }
    operator f64() const { return m_value; }
    f64 get() const { return m_value; }
    void from_default() override { m_value = def; }
    void restore_defaults() override { def = original_def; }
    std::string to_string() const override { return detail::float_text(m_value); }
    std::string def_to_string() const override { return detail::float_text(def); }
    bool from_string(std::string_view value, bool = false) override
    {
        f64 result;
        if (!detail::to_float(&result, value, Min, Max)) return false;
        m_value = result;
        return true;
    }
    void set(const f64& value)
    {
        if (value < Min || value > Max) throw std::out_of_range("'" + m_name + "': value out of bounds");
        m_value = value;
    }
    // Like cfg::make_float_range: std::to_string, so the items contain a '.'
    std::vector<std::string> to_list() const override { return {std::to_string(f64(Min)), std::to_string(f64(Max))}; }
};

using int32 = _int<INT32_MIN, INT32_MAX>;
using int64 = _int<INT64_MIN, INT64_MAX>;

template <u64 Min, u64 Max>
class uint final : public _base
{
    static_assert(Min < Max, "Invalid cfg::uint range");
    using int_type = std::conditional_t<Max <= UINT32_MAX, u32, u64>;
    int_type m_value;
    int_type original_def;

public:
    int_type def;
    static constexpr u64 max = Max;
    static constexpr u64 min = Min;

    uint(node* owner, const std::string& name, int_type def = std::max<int_type>(Min, 0), bool dynamic = false)
        : _base(type::uint, owner, name, dynamic), m_value(def), original_def(def), def(def)
    {
    }
    operator int_type() const { return m_value; }
    int_type get() const { return m_value; }
    void from_default() override { m_value = def; }
    void restore_defaults() override { def = original_def; }
    std::string to_string() const override { return std::to_string(m_value); }
    std::string def_to_string() const override { return std::to_string(def); }
    bool from_string(std::string_view value, bool = false) override
    {
        u64 result;
        if (!detail::to_uint64(&result, value, Min, Max)) return false;
        m_value = static_cast<int_type>(result);
        return true;
    }
    void set(const u64& value)
    {
        if (value < Min || value > Max) throw std::out_of_range("'" + m_name + "': value out of bounds");
        m_value = static_cast<int_type>(value);
    }
    std::vector<std::string> to_list() const override { return {std::to_string(Min), std::to_string(Max)}; }
};

using uint32 = uint<0, UINT32_MAX>;
using uint64 = uint<0, UINT64_MAX>;

class uint128 final : public _base
{
    u128 m_value{};
    u128 original_def = 0;

public:
    u128 def;

    uint128(node* owner, const std::string& name, u128 def = 0, bool dynamic = false)
        : _base(type::uint128, owner, name, dynamic), m_value(def), original_def(def), def(def)
    {
    }
    operator u128() const { return m_value; }
    u128 get() const { return m_value; }
    void from_default() override { m_value = def; }
    void restore_defaults() override { def = original_def; }
    static std::string to_string(u128 value)
    {
        std::string result = "0x";
        result.resize(result.size() + 32);
        for (u32 i = 0; i < 32; i++) result[result.size() - 1 - i] = "0123456789ABCDEF"[static_cast<u64>(value >> (i * 4)) % 16];
        return result;
    }
    std::string to_string() const override { return to_string(m_value); }
    std::string def_to_string() const override { return to_string(def); }
    // Like cfg::try_to_uint128: hexadecimal only, '0x' optional, at most 32 digits
    bool from_string(std::string_view value, bool = false) override
    {
        if (value.empty()) return false;
        if (value.size() >= 2 && value[0] == '0' && (value[1] == 'x' || value[1] == 'X')) value.remove_prefix(2);
        if (value.empty() || value.size() > 32) return false;
        u128 result = 0;
        for (const char c : value)
        {
            int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (d < 0) return false;
            result = (result << 4) | u128(d);
        }
        m_value = result;
        return true;
    }
    void set(u128 value) { m_value = value; }
    std::vector<std::string> to_list() const override { return {"0", "1"}; }
};

class string : public _base
{
    std::string m_value;
    std::string original_def;

public:
    std::string def;

    string(node* owner, std::string name, std::string def = {}, bool dynamic = false)
        : _base(type::string, owner, std::move(name), dynamic), m_value(def), original_def(def), def(std::move(def))
    {
    }
    operator std::string() const { return m_value; }
    void from_default() override { m_value = def; }
    void restore_defaults() override { def = original_def; }
    std::string to_string() const override { return m_value; }
    std::string get() const { return m_value; }
    std::string def_to_string() const override { return def; }
    bool from_string(std::string_view value, bool = false) override
    {
        m_value = std::string(value);
        return true;
    }
    void set(std::string_view value) { m_value = std::string(value); }
};

class set_entry final : public _base
{
    std::set<std::string> m_set{};

public:
    set_entry(node* owner, const std::string& name) : _base(type::set, owner, name, false) {}
    const std::set<std::string>& get_set() const { return m_set; }
    void set_set(std::set<std::string>&& set) { m_set = std::move(set); }
    void from_default() override { m_set = {}; }
    void restore_defaults() override {}
    std::vector<std::string> to_list() const override { return {m_set.begin(), m_set.end()}; }
    bool from_list(std::vector<std::string>&& list) override
    {
        m_set = {std::make_move_iterator(list.begin()), std::make_move_iterator(list.end())};
        return true;
    }
};

template <typename T>
using map_of_type = std::map<std::string, T, std::less<>>;

class map_entry : public _base
{
    map_of_type<std::string> m_map{};

public:
    map_entry(node* owner, const std::string& name, type t = type::map) : _base(t, owner, name, true) {}
    const map_of_type<std::string>& get_map() const { return m_map; }
    std::string get_value(std::string_view key)
    {
        const auto it = m_map.find(key);
        return it != m_map.end() ? it->second : std::string{};
    }
    void set_value(std::string key, std::string value) { m_map[std::move(key)] = std::move(value); }
    void set_map(map_of_type<std::string>&& map) { m_map = std::move(map); }
    void erase(std::string_view key)
    {
        if (const auto it = m_map.find(key); it != m_map.end()) m_map.erase(it);
    }
    void from_default() override { m_map = {}; }
    void restore_defaults() override {}
};

class node_map_entry final : public map_entry
{
public:
    node_map_entry(node* owner, const std::string& name) : map_entry(owner, name, type::node_map) {}
};

class log_entry final : public _base
{
    map_of_type<logs::level> m_map{};

public:
    log_entry(node* owner, const std::string& name) : _base(type::log, owner, name, true) {}
    const map_of_type<logs::level>& get_map() const { return m_map; }
    void set_map(map_of_type<logs::level>&& map) { m_map = std::move(map); }
    void from_default() override { m_map = {}; }
    void restore_defaults() override {}
};

struct device_info
{
    std::string path, serial, vid, pid;
};

class device_entry final : public _base
{
    map_of_type<device_info> m_map{};
    map_of_type<device_info> m_default{};

public:
    device_entry(node* owner, const std::string& name, map_of_type<device_info> def = {}) : _base(type::device, owner, name, true), m_map(std::move(def))
    {
        m_default = m_map;
    }
    const map_of_type<device_info>& get_map() const { return m_map; }
    const map_of_type<device_info>& get_default() const { return m_default; }
    void set_map(map_of_type<device_info>&& map) { m_map = std::move(map); }
    void from_default() override { m_map = m_default; }
    void restore_defaults() override {}
};

namespace detail
{
inline void emit(std::string& out, const _base& e, int indent)
{
    const std::string pad(std::size_t(indent), ' ');
    const std::string key = pad + cfg_mock::quote_scalar(e.get_name()) + ":";
    switch (e.get_type())
    {
    case type::node:
    {
        const auto& nodes = static_cast<const node&>(e).get_nodes();
        if (nodes.empty()) { out += key + " {}\n"; return; }
        out += key + "\n";
        for (const _base* n : nodes) emit(out, *n, indent + 2);
        return;
    }
    case type::set:
    {
        const auto items = e.to_list();
        if (items.empty()) { out += key + " []\n"; return; }
        out += key + "\n";
        for (const std::string& item : items) out += pad + "  - " + cfg_mock::quote_scalar(item) + "\n";
        return;
    }
    case type::map:
    case type::node_map:
    {
        const auto& map = static_cast<const map_entry&>(e).get_map();
        if (map.empty()) { out += key + " {}\n"; return; }
        out += key + "\n";
        for (const auto& [k, v] : map) out += pad + "  " + cfg_mock::quote_scalar(k) + ": " + cfg_mock::quote_scalar(v) + "\n";
        return;
    }
    case type::log:
    {
        std::string body;
        for (const auto& [k, v] : static_cast<const log_entry&>(e).get_map())
            if (v != logs::level::_default) body += pad + "  " + cfg_mock::quote_scalar(k) + ": " + std::string(cfg_mock::log_level_name(v)) + "\n";
        if (body.empty()) { out += key + " {}\n"; return; }
        out += key + "\n" + body;
        return;
    }
    case type::device:
    {
        const auto& map = static_cast<const device_entry&>(e).get_map();
        if (map.empty()) { out += key + " {}\n"; return; }
        out += key + "\n";
        for (const auto& [k, info] : map)
        {
            out += pad + "  " + cfg_mock::quote_scalar(k) + ":\n";
            out += pad + "    Path: " + cfg_mock::quote_scalar(info.path) + "\n";
            out += pad + "    Serial: " + cfg_mock::quote_scalar(info.serial) + "\n";
            out += pad + "    VID: " + cfg_mock::quote_scalar(info.vid) + "\n";
            out += pad + "    PID: " + cfg_mock::quote_scalar(info.pid) + "\n";
        }
        return;
    }
    default: out += key + " " + cfg_mock::quote_scalar(e.to_string()) + "\n"; return;
    }
}

// cfg::decode of Utilities/Config.cpp
inline bool decode(const cfg_mock::ynode& data, _base& rhs, bool dynamic, bool strict)
{
    using cfg_mock::ynode;
    if (dynamic && !rhs.get_is_dynamic()) return true;
    switch (rhs.get_type())
    {
    case type::node:
    {
        if (data.k != ynode::kind::map) return false;
        bool success = true;
        for (const auto& [key, value] : data.items)
        {
            const auto& nodes = static_cast<node&>(rhs).get_nodes();
            const auto it = std::find_if(nodes.begin(), nodes.end(), [&](const _base* n) { return n->get_name() == key; });
            if (it == nodes.end())
            {
                if (strict) success = false;
                continue;
            }
            if (!decode(value, **it, dynamic, strict) && strict) success = false;
        }
        return success;
    }
    case type::set:
    {
        if (data.k != ynode::kind::seq) break;
        std::vector<std::string> values;
        for (const ynode& item : data.list)
            if (item.k == ynode::kind::scalar) values.push_back(item.text);
        if (!rhs.from_list(std::move(values)) && strict) return false;
        break;
    }
    case type::map:
    case type::node_map:
    {
        if (data.k != ynode::kind::map) return false;
        map_of_type<std::string> values;
        for (const auto& [key, value] : data.items)
            if (value.k == ynode::kind::scalar) values.emplace(key, value.text);
        static_cast<map_entry&>(rhs).set_map(std::move(values));
        break;
    }
    case type::log:
    {
        if (data.k != ynode::kind::map) return false;
        map_of_type<logs::level> values;
        for (const auto& [key, value] : data.items)
        {
            if (value.k != ynode::kind::scalar)
            {
                if (strict) return false;
                continue;
            }
            const auto level = cfg_mock::log_level_from_name(value.text);
            if (!level)
            {
                if (strict) return false;
                continue;
            }
            values.emplace(key, *level);
        }
        static_cast<log_entry&>(rhs).set_map(std::move(values));
        break;
    }
    case type::device: break;
    default:
    {
        if (data.k != ynode::kind::scalar) break;
        if (!rhs.from_string(data.text, dynamic) && strict) return false;
        break;
    }
    }
    return true;
}
} // namespace detail

inline std::string node::to_string() const
{
    std::string out;
    for (const _base* n : m_nodes) detail::emit(out, *n, 0);
    return out.empty() ? "{}\n" : out;
}

inline bool node::from_string(std::string_view value, bool dynamic)
{
    cfg_mock::ynode data;
    if (!cfg_mock::parse_yaml(value, data)) return false;
    return detail::decode(data, *this, dynamic, false);
}

inline bool node::validate(std::string_view value)
{
    cfg_mock::ynode data;
    if (!cfg_mock::parse_yaml(value, data)) return false;
    return detail::decode(data, *this, false, true);
}
} // namespace cfg
