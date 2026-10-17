#pragma once

// Cheat codes for the games: NetCheat / Artemis code lines ("T AAAAAAAA VVVVVVVV"), .ncl lists, PC RPCS3 cheats.yml
// entries and clipboard strings ("game@@@name@@@type@@@offset@@@script@@@"), each cheat turned into a small op
// program over guest addresses (a PS3 effective address is the RPCS3 vm address; nothing in a file names a host
// address).
//
// /data/rpcs3/cheats/<TITLE_ID>.txt, .ncl   imported Artemis / NetCheat text
//                    <TITLE_ID>.yml          imported part of a PC cheats.yml (or a per-game PC file)
//                    <TITLE_ID>_user.txt     cheats made on the console (the same text format, readable by a PC)
//                    <TITLE_ID>.state        switches, modes, periods and typed values
//                    <TITLE_ID>.seen         executables booted for the title (hash, version, code and data ranges)
//
// The parser, converter, interpreter, state/seen formats and row texts need only the standard library (host tested
// in ps4-port/tests/cheats_test.cpp); the file part (load, edit, import) is PS4 only. Nothing here throws (no
// std::sto*), every loop is bounded by the caps below and every problem has an i18n id.
//
// Code types 0 (write), 1 (text), 2 (float) and 4 (condensed series) run. 6 (pointer), A/F (copy), B (find and
// replace) and D/E (conditionals) are parsed with the provisional layouts of type_table() and refused with
// CHT_ERR_TIPO_NAO_SUPORTADO until sample files pin them (the interpreter already runs every op kind).

#include "util/ps4_i18n.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace utils::ps4::cheats
{
using u8 = std::uint8_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

inline constexpr std::size_t file_limit = 1u << 20;     // one cheat file
inline constexpr std::size_t max_cheats = 512;          // per game
inline constexpr std::size_t max_code_lines = 4096;     // per cheat
inline constexpr std::size_t max_write = 4096;          // bytes of one write (and of one compare value)
inline constexpr std::size_t max_series = 64u << 10;    // bytes of a condensed series or a copy
inline constexpr std::size_t max_find_range = 16u << 20;
inline constexpr unsigned max_pointer_depth = 8;
inline constexpr std::size_t max_name_bytes = 120;
inline constexpr u32 period_default_ms = 100;
inline constexpr u32 period_min_ms = 16;
inline constexpr u32 period_max_ms = 1000;
inline constexpr std::array<u32, 6> periods_ms{16, 50, 100, 250, 500, 1000};
// Guest memory a cheat may touch: vm::main and the user areas. RSX local memory (0xC0000000), thread stacks
// (0xD0000000) and SPU (0xE0000000) are refused; the RSX context block is refused at run time.
inline constexpr u32 guest_begin = 0x00010000;
inline constexpr u32 guest_end = 0xC0000000;
inline constexpr u32 once_delay_ms = 10000;    // a "once" cheat is written 10 s after the game starts running
inline constexpr u32 unmapped_off_ms = 60000;  // a target never writable for this long turns the cheat off
inline constexpr std::string_view state_header = "# RPCS3 PS4 cheats state 1";
inline constexpr std::string_view seen_header = "# RPCS3 PS4 cheats seen 1";
inline constexpr std::size_t max_seen_lines = 8;
inline constexpr std::size_t max_notes = 64;   // problems kept per file and per cheat (a garbage file adds no more)

namespace i18n = utils::ps4::i18n;

// ---------------------------------------------------------------------------------------------------------------
// Text helpers

namespace detail
{
constexpr bool space(char c) noexcept { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f'; }

inline std::string_view trim(std::string_view t) noexcept
{
    while (!t.empty() && space(t.front())) t.remove_prefix(1);
    while (!t.empty() && space(t.back())) t.remove_suffix(1);
    return t;
}

inline std::string_view ltrim(std::string_view t) noexcept
{
    while (!t.empty() && space(t.front())) t.remove_prefix(1);
    return t;
}

constexpr int hex_value(char c) noexcept
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

constexpr char upper(char c) noexcept { return c >= 'a' && c <= 'z' ? char(c - 'a' + 'A') : c; }
constexpr char lower(char c) noexcept { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; }
constexpr bool alnum(char c) noexcept { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

inline std::string upper_ascii(std::string_view t)
{
    std::string out(t);
    for (char& c : out) c = upper(c);
    return out;
}

inline std::string lower_ascii(std::string_view t)
{
    std::string out(t);
    for (char& c : out) c = lower(c);
    return out;
}

inline bool iequals(std::string_view a, std::string_view b) noexcept
{
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (lower(a[i]) != lower(b[i])) return false;
    return true;
}

inline bool all_hex(std::string_view t) noexcept
{
    if (t.empty()) return false;
    for (const char c : t)
        if (hex_value(c) < 0) return false;
    return true;
}

inline u32 fnv1a(std::string_view t, u32 h = 2166136261u) noexcept
{
    for (const char c : t)
    {
        h ^= static_cast<u8>(c);
        h *= 16777619u;
    }
    return h;
}

inline std::string hex_upper(u64 v, unsigned digits)
{
    std::string out(digits, '0');
    for (unsigned i = 0; i < digits; ++i) out[digits - 1 - i] = "0123456789ABCDEF"[(v >> (4 * i)) & 15];
    return out;
}

// "0x0031A2B0", as the messages show addresses
inline std::string address_text(u64 v) { return "0x" + hex_upper(v, v > 0xffffffffu ? 16 : 8); }

inline std::string hex_bytes(std::string_view bytes)
{
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const char c : bytes)
    {
        out += "0123456789abcdef"[static_cast<u8>(c) >> 4];
        out += "0123456789abcdef"[static_cast<u8>(c) & 15];
    }
    return out;
}

// 1..16 hex digits
inline bool parse_hex(std::string_view t, u64& out) noexcept
{
    if (t.empty() || t.size() > 16 || !all_hex(t)) return false;
    out = 0;
    for (const char c : t) out = out << 4 | u64(hex_value(c));
    return true;
}

// Decimal digits only, at most 'max' (no sign, no overflow)
inline bool parse_decimal(std::string_view t, u64& out, u64 max) noexcept
{
    if (t.empty() || t.size() > 20) return false;
    out = 0;
    for (const char c : t)
    {
        if (c < '0' || c > '9') return false;
        const u64 d = u64(c - '0');
        if (d > max || out > (max - d) / 10) return false;
        out = out * 10 + d;
    }
    return true;
}

// Strict decimal float, '.' as the point: [-+]digits[.digits][e[-+]digits] (or .digits); no locale, no exceptions
inline bool parse_float(std::string_view t, float& out) noexcept
{
    std::size_t i = 0;
    bool negative = false;
    if (i < t.size() && (t[i] == '-' || t[i] == '+')) negative = t[i++] == '-';
    u64 mantissa = 0;
    int exponent = 0;
    int digits = 0;
    bool any = false;
    for (; i < t.size() && t[i] >= '0' && t[i] <= '9'; ++i, any = true)
    {
        if (digits < 19) mantissa = mantissa * 10 + u64(t[i] - '0'), digits += mantissa != 0;
        else ++exponent;
    }
    if (i < t.size() && t[i] == '.')
    {
        for (++i; i < t.size() && t[i] >= '0' && t[i] <= '9'; ++i, any = true)
        {
            if (digits < 19)
            {
                mantissa = mantissa * 10 + u64(t[i] - '0');
                digits += mantissa != 0;
                --exponent;
            }
        }
    }
    if (!any) return false;
    if (i < t.size() && (t[i] == 'e' || t[i] == 'E'))
    {
        ++i;
        bool exp_negative = false;
        if (i < t.size() && (t[i] == '-' || t[i] == '+')) exp_negative = t[i++] == '-';
        int e = 0;
        bool exp_any = false;
        for (; i < t.size() && t[i] >= '0' && t[i] <= '9'; ++i, exp_any = true) e = std::min(e * 10 + (t[i] - '0'), 1000);
        if (!exp_any) return false;
        exponent += exp_negative ? -e : e;
    }
    if (i != t.size()) return false;
    // double is exact enough for a float result
    double value = static_cast<double>(mantissa);
    if (mantissa)
    {
        if (exponent > 60 || exponent < -80) return false;
        for (int k = 0; k < exponent; ++k) value *= 10.0;
        for (int k = 0; k > exponent; --k) value /= 10.0;
    }
    if (value > static_cast<double>(std::numeric_limits<float>::max())) return false;
    out = static_cast<float>(negative ? -value : value);
    return true;
}

inline void append_be(std::string& out, u64 v, unsigned bytes)
{
    for (unsigned i = bytes; i-- > 0;) out += static_cast<char>((v >> (8 * i)) & 0xff);
}

inline u64 read_be(const char* p, unsigned bytes) noexcept
{
    u64 v = 0;
    for (unsigned i = 0; i < bytes; ++i) v = v << 8 | static_cast<u8>(p[i]);
    return v;
}

inline std::string float_bytes(float f)
{
    u32 bits = 0;
    std::memcpy(&bits, &f, sizeof(bits));
    std::string out;
    append_be(out, bits, 4);
    return out;
}

// Hex digits to bytes in memory order; an odd count gets one leading 0 ('odd' set)
inline bool hex_to_bytes(std::string_view digits, std::string& out, bool& odd)
{
    out.clear();
    odd = digits.size() % 2 != 0;
    if (!all_hex(digits)) return false;
    out.reserve(digits.size() / 2 + 1);
    std::size_t i = 0;
    if (odd) out += static_cast<char>(hex_value(digits[i++]));
    // an even count of digits is left
    for (; i < digits.size(); i += 2) out += static_cast<char>(hex_value(digits[i]) << 4 | hex_value(digits[i + 1]));
    return true;
}

inline std::string clip_name(std::string_view t)
{
    t = trim(t);
    std::string out(t.substr(0, i18n::utf8_fit(t, max_name_bytes)));
    for (char& c : out)
        if (static_cast<u8>(c) < 0x20) c = ' ';
    return out;
}

// A PS3 title ID: 4 letters and 5 digits (BLUS30443, NPEB00001)
inline bool serial_shape(std::string_view t) noexcept
{
    if (t.size() != 9) return false;
    for (std::size_t i = 0; i < 4; ++i)
        if (!((t[i] >= 'A' && t[i] <= 'Z') || (t[i] >= 'a' && t[i] <= 'z'))) return false;
    for (std::size_t i = 4; i < 9; ++i)
        if (t[i] < '0' || t[i] > '9') return false;
    return true;
}

// First serial-shaped word of a text ("BLUS30443", "[BLUS-30443]" is not taken)
inline std::string find_serial(std::string_view t)
{
    for (std::size_t i = 0; i + 9 <= t.size(); ++i)
    {
        const bool left = i == 0 || !alnum(t[i - 1]);
        const bool right = i + 9 == t.size() || !alnum(t[i + 9]);
        if (left && right && serial_shape(t.substr(i, 9))) return upper_ascii(t.substr(i, 9));
    }
    return {};
}

// Titles compared case- and punctuation-insensitively (ASCII letters and digits; other bytes kept)
inline std::string fold_title(std::string_view t)
{
    std::string out;
    for (const char c : t)
    {
        if (static_cast<unsigned char>(c) >= 0x80) out += c;
        else if (alnum(c)) out += lower(c);
    }
    return out;
}

// Version strings compared as numbers per part: "01.02" == "1.02"
inline std::string fold_version(std::string_view t)
{
    std::string out;
    bool lead = true;
    for (const char c : trim(t))
    {
        if (c == '.') { out += '.'; lead = true; continue; }
        if (lead && c == '0') continue;
        lead = false;
        out += lower(c);
    }
    return out;
}
} // namespace detail

// A translated text as a string (i18n::tr gives a pointer to the table)
inline std::string text(i18n::str id) { return i18n::tr(id); }

// ---------------------------------------------------------------------------------------------------------------
// Problems

struct problem
{
    u32 line = 0;                // 1-based source line; 0 = the whole file or cheat
    i18n::str id = i18n::str::CHT_ERR_FORMATO;
    std::string arg1, arg2;
    bool fatal = true;           // the cheat cannot be turned on
};

// Adds a problem while the list holds fewer than max_notes (each one is also an RPCS3.log line at boot)
inline bool add_note(std::vector<problem>& list, problem p)
{
    if (list.size() >= max_notes) return false;
    list.push_back(std::move(p));
    return true;
}

// The text of a problem in the interface language; an id whose pt-BR text starts with "Linha {0}" gets the line
inline std::string problem_text(const problem& p)
{
    const std::string_view pattern = i18n::tr(p.id, i18n::lang::pt_br);
    if (pattern.starts_with("Linha {0}")) return i18n::trf(p.id, {p.line, p.arg1, p.arg2});
    return i18n::trf(p.id, {p.arg1, p.arg2});
}

// ---------------------------------------------------------------------------------------------------------------
// Op program

enum class op_kind : u8
{
    write,        // bytes at addr
    write_series, // count stores of 'width' bytes: addr += addr_step, value += value_step (mod the width)
    pointer,      // base = be32 at addr (+ base when relative) + aux; a null or unmapped pointer skips the pass
    if_equal,     // the next 'count' ops run only when memory at addr equals bytes
    if_mask,      // the next 'count' ops run only when (memory & bytes) == bytes
    copy,         // aux bytes from addr to aux2
    find_replace, // first occurrence of bytes in [addr, aux) replaced by bytes2
    script,       // base = PC redirection script with '$' = addr
};

struct op
{
    op_kind kind = op_kind::write;
    bool relative = false;   // addr (and aux2 for a copy) is an offset from the base of the last pointer/script op
    u8 width = 0;            // write_series: bytes per store (1..8)
    u32 addr = 0;
    u32 count = 0;           // write_series: stores; if_*: guarded ops
    u32 addr_step = 0;
    u64 value_step = 0;
    u32 aux = 0;
    u32 aux2 = 0;
    u32 line = 0;            // 1-based source line
    std::string bytes;       // value / compare value / mask / find original, in memory order
    std::string bytes2;      // find_replace replacement
    std::string script;
};

struct program
{
    std::vector<op> ops;
    bool plain = true;   // only absolute write / write_series ops: every target is known before the game runs
    bool find = false;   // has a find_replace op: runs once
};

// Bytes [begin, end) a plain op touches
inline std::pair<u64, u64> op_span(const op& o) noexcept
{
    if (o.kind == op_kind::write_series)
    {
        if (!o.count) return {o.addr, o.addr};
        return {o.addr, u64(o.addr) + u64(o.count - 1) * o.addr_step + o.width};
    }
    return {o.addr, u64(o.addr) + o.bytes.size()};
}

// Code types: the running ones and the provisional layouts of the others (one row each, so pinning is local)
struct type_info
{
    char type;
    bool runs;
    const char* layout;
};

inline constexpr std::array<type_info, 10> type_table{{
    {'0', true, "0 AAAAAAAA VV..: write the value bytes"},
    {'1', true, "1 AAAAAAAA text: write the text bytes (quotes optional)"},
    {'2', true, "2 AAAAAAAA F: write F as a big-endian float (8 hex digits without '.' = raw bits)"},
    {'4', true, "4 AAAAAAAA VV.. + 4 NNNNIIII TTTT..: write V at A, N times, A += I, V += T"},
    {'6', false, "6 AAAAAAAA OOOOOOOO: base = be32 at A + O (more 6 lines go deeper); the next line is relative"},
    {'A', false, "A SSSSSSSS LLLLLLLL + A DDDDDDDD 00000000: copy L bytes from S to D"},
    {'B', false, "B SSSSSSSS EEEEEEEE + B OO.. RR..: replace the first O in [S, E) by R"},
    {'D', false, "D AAAAAAAA VV..: the next line runs only when memory at A equals V"},
    {'E', false, "E AAAAAAAA MM..: the next line runs only when (memory at A & M) == M"},
    {'F', false, "F SSSSSSSS LLLLLLLL + F DDDDDDDD 00000000: copy (as A)"},
}};

inline const type_info* find_type(char t) noexcept
{
    t = detail::upper(t);
    for (const type_info& i : type_table)
        if (i.type == t) return &i;
    return nullptr;
}

// ---------------------------------------------------------------------------------------------------------------
// Cheats as read from a file

enum class mode : u8 { automatic, boot, once, constant };

inline std::string_view mode_token(mode m) noexcept
{
    switch (m)
    {
    case mode::boot: return "boot";
    case mode::once: return "once";
    case mode::constant: return "constant";
    case mode::automatic: break;
    }
    return "auto";
}

inline bool parse_mode(std::string_view t, mode& out) noexcept
{
    if (detail::iequals(t, "auto") || detail::iequals(t, "automatic")) out = mode::automatic;
    else if (detail::iequals(t, "boot") || detail::iequals(t, "start")) out = mode::boot;
    else if (detail::iequals(t, "once")) out = mode::once;
    else if (detail::iequals(t, "constant")) out = mode::constant;
    else return false;
    return true;
}

inline u32 clamp_period(u64 ms) noexcept
{
    if (!ms) return period_default_ms;
    return static_cast<u32>(std::clamp<u64>(ms, period_min_ms, period_max_ms));
}

// PC RPCS3 cheat types (cheat_info.h order, the names its cheats.yml writes)
enum class pc_type : u8 { u8_, u16_, u32_, u64_, s8_, s16_, s32_, s64_, f32_ };
inline constexpr std::array<std::string_view, 9> pc_type_names{"Unsigned 8 bits", "Unsigned 16 bits", "Unsigned 32 bits", "Unsigned 64 bits",
    "Signed 8 bits", "Signed 16 bits", "Signed 32 bits", "Signed 64 bits", "Float 32 bits"};

inline unsigned pc_width(pc_type t) noexcept
{
    switch (t)
    {
    case pc_type::u8_: case pc_type::s8_: return 1;
    case pc_type::u16_: case pc_type::s16_: return 2;
    case pc_type::u64_: case pc_type::s64_: return 8;
    default: break;
    }
    return 4;
}

inline bool parse_pc_type(std::string_view t, pc_type& out) noexcept
{
    t = detail::trim(t);
    for (std::size_t i = 0; i < pc_type_names.size(); ++i)
        if (detail::iequals(t, pc_type_names[i]))
        {
            out = pc_type(i);
            return true;
        }
    return false;
}

struct pc_entry
{
    std::string game;      // the PC title key
    pc_type type = pc_type::u32_;
    u32 offset = 0;
    std::string script;    // redirection script, empty = write at the offset
};

struct cheat
{
    std::string name, author, notes;
    std::vector<std::string> lines;      // code lines as shown ("Ver linhas")
    std::vector<std::string> code_text;  // the lines as a text file holds them (PC entries: the clipboard form)
    program prog;
    std::vector<problem> problems;
    mode wanted = mode::automatic;       // "; mode:" directive or the .ncl constant flag
    u32 period_ms = 0;                   // 0 = default
    std::string hash, version;           // file directives in effect
    u32 key = 0;
    u32 line = 0;                        // title line
    bool pc = false;
    pc_entry pc_info;
    bool unsupported = false;

    bool fatal() const
    {
        return std::any_of(problems.begin(), problems.end(), [](const problem& p) { return p.fatal; });
    }
};

struct parsed_file
{
    std::vector<cheat> cheats;
    std::vector<problem> notes;          // file-level notes (caps, ignored lines, dropped cheats)
    std::string serial, version, game;   // first file-level values seen (import naming)
    std::vector<u32> ignored;            // first 16 lines the parser could not use
    u32 ignored_count = 0;
};

// ---------------------------------------------------------------------------------------------------------------
// PC redirection scripts: '$' = the cheat offset, [expr] = the big-endian u32 at expr, '+', '-', spaces, decimal
// numbers; the value starts at 0 and each operand applies the pending operator (initially '='), as the PC
// cheat_engine::resolve_script does. u64 arithmetic: a result outside 0..0xFFFFFFFF fails instead of wrapping.

namespace detail
{
inline bool script_close(std::string_view s, std::size_t open, std::size_t& close) noexcept
{
    int level = 0;
    for (std::size_t i = open; i < s.size(); ++i)
    {
        if (s[i] == '[') ++level;
        else if (s[i] == ']' && --level == 0)
        {
            close = i;
            return true;
        }
    }
    return false;
}

template <typename Read>
bool eval_script(std::string_view s, u32 offset, Read&& read, u64& out, unsigned depth) noexcept
{
    if (depth > max_pointer_depth) return false;
    u64 value = 0;
    char op = '=';
    const auto apply = [&](u64 operand) -> bool
    {
        if (op == '=') value = operand;
        else if (op == '+') value += operand;
        else
        {
            if (operand > value) return false;
            value -= operand;
        }
        return value <= 0xffffffffu;
    };
    std::size_t i = 0;
    while (i < s.size())
    {
        const char c = s[i];
        if (c >= '0' && c <= '9')
        {
            std::size_t end = i;
            while (end < s.size() && s[end] >= '0' && s[end] <= '9') ++end;
            u64 number = 0;
            if (!parse_decimal(s.substr(i, end - i), number, 0xffffffffu) || !apply(number)) return false;
            i = end;
            continue;
        }
        switch (c)
        {
        case '$':
            if (!apply(offset)) return false;
            ++i;
            break;
        case '[':
        {
            std::size_t close = 0;
            if (!script_close(s, i, close)) return false;
            u64 address = 0;
            if (!eval_script(s.substr(i + 1, close - i - 1), offset, read, address, depth + 1)) return false;
            u32 pointer = 0;
            if (!read(static_cast<u32>(address), pointer) || !apply(pointer)) return false;
            i = close + 1;
            break;
        }
        case '+': op = '+'; ++i; break;
        case '-': op = '-'; ++i; break;
        case ' ': ++i; break;
        default: return false;
        }
    }
    out = value;
    return true;
}
} // namespace detail

// Structure of a script (characters, brackets, numbers, depth), without memory: every read gives 0x10000
inline bool check_script(std::string_view s) noexcept
{
    u64 out = 0;
    return detail::eval_script(s, 0x10000, [](u32, u32& v) { v = 0x10000; return true; }, out, 0) || [&]
    {
        // A result out of range with the stand-in reads is still a valid script: check the shape only
        for (const char c : s)
            if (!((c >= '0' && c <= '9') || c == '$' || c == '[' || c == ']' || c == '+' || c == '-' || c == ' ')) return false;
        int level = 0, deepest = 0;
        for (const char c : s)
        {
            if (c == '[') deepest = std::max(deepest, ++level);
            if (c == ']' && --level < 0) return false;
        }
        if (level || deepest > int(max_pointer_depth)) return false;
        for (std::size_t i = 0; i < s.size();)
        {
            if (s[i] < '0' || s[i] > '9') { ++i; continue; }
            std::size_t end = i;
            while (end < s.size() && s[end] >= '0' && s[end] <= '9') ++end;
            u64 n = 0;
            if (!detail::parse_decimal(s.substr(i, end - i), n, 0xffffffffu)) return false;
            i = end;
        }
        return true;
    }();
}

// ---------------------------------------------------------------------------------------------------------------
// Code line parser

namespace detail
{
struct code_line
{
    u32 line = 0;
    char type = 0;
    std::string_view addr, value, raw;
};

// "T ADDR VALUE": T one hex digit, ADDR 1-16 hex digits. Short addresses (under 6 digits) need a value that fits
// the type, so a title such as "A Bad Day" or "1 Hit Kill" stays a title.
inline bool split_code_line(std::string_view t, u32 line, code_line& out) noexcept
{
    if (t.size() < 3 || hex_value(t[0]) < 0 || !space(t[1])) return false;
    std::string_view rest = ltrim(t.substr(1));
    std::size_t end = 0;
    while (end < rest.size() && !space(rest[end])) ++end;
    const std::string_view addr = rest.substr(0, end);
    if (!all_hex(addr)) return false;
    std::string_view value = ltrim(rest.substr(end));
    const char type = upper(t[0]);
    if (type != '1')
    {
        // trailing comments of a numeric line: "0 00A1B2C4 0000270F // money"
        for (std::size_t i = 0; i < value.size(); ++i)
            if ((value[i] == '/' && i + 1 < value.size() && value[i + 1] == '/') || value[i] == ';' || value[i] == '#')
            {
                if (i == 0 || space(value[i - 1]))
                {
                    value = value.substr(0, i);
                    break;
                }
            }
        value = trim(value);
    }
    else
    {
        while (!value.empty() && space(value.back())) value.remove_suffix(1);
    }
    if (addr.size() < 6)
    {
        if (value.empty()) return false;
        if (type == '1')
        {
            if (value.front() != '"') return false;
        }
        else if (type == '2')
        {
            float f = 0;
            if (!parse_float(value, f) && !all_hex(value)) return false;
        }
        else
        {
            std::size_t v = 0;
            while (v < value.size() && !space(value[v])) ++v;
            if (!all_hex(value.substr(0, v))) return false;
        }
    }
    out = {line, type, addr, value, t};
    return true;
}

// Normalized text of one code line for the cheat key: upper-case type, address and hex value, single spaces
inline std::string key_line(const code_line& l)
{
    std::string out(1, l.type);
    out += ' ';
    u64 a = 0;
    if (parse_hex(l.addr, a) && a <= 0xffffffffu) out += hex_upper(a, 8);
    else out += upper_ascii(l.addr);
    out += ' ';
    if (l.type == '1') out += l.value;
    else
        for (const char c : l.value)
            if (!space(c)) out += upper(c);
    return out;
}

// Type 1 value: "quoted text" with \" \\ \n \t \0 or the raw rest of the line
inline bool decode_text(std::string_view v, std::string& out)
{
    out.clear();
    if (v.empty() || v.front() != '"')
    {
        out = std::string(v);
        return !out.empty();
    }
    std::size_t i = 1;
    for (; i < v.size(); ++i)
    {
        const char c = v[i];
        if (c == '"') break;
        if (c != '\\')
        {
            out += c;
            continue;
        }
        if (++i >= v.size()) return false;
        switch (v[i])
        {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case '0': out += '\0'; break;
        default: return false;
        }
    }
    if (i >= v.size()) return false;
    return trim(v.substr(i + 1)).empty() && !out.empty();
}

inline bool in_guest(u64 begin, u64 end) noexcept
{
    return begin >= guest_begin && end <= guest_end && begin <= end;
}

struct converter
{
    cheat& c;
    std::string key_text = "A";

    void add(u32 line, i18n::str id, std::string a1 = {}, std::string a2 = {}, bool fatal = true)
    {
        c.problems.push_back({line, id, std::move(a1), std::move(a2), fatal});
    }

    // 1-8 hex digits (or 9-16 with a value that fits 32 bits); more is a lv2 kernel address
    bool address(const code_line& l, u32& out)
    {
        u64 v = 0;
        if (!parse_hex(l.addr, v))
        {
            add(l.line, i18n::str::CHT_ERR_HEX, std::string(l.addr));
            return false;
        }
        if (v > 0xffffffffu)
        {
            add(l.line, i18n::str::CHT_ERR_KERNEL, std::string(l.addr));
            return false;
        }
        out = static_cast<u32>(v);
        return true;
    }

    bool region(const code_line& l, u64 begin, u64 end)
    {
        if (in_guest(begin, end)) return true;
        add(l.line, i18n::str::CHT_ERR_REGIAO, address_text(begin));
        return false;
    }

    bool value_bytes(const code_line& l, std::string_view digits, std::string& out, std::size_t limit)
    {
        std::string joined;
        for (const char ch : digits)
            if (!space(ch)) joined += ch;
        if (joined.empty())
        {
            add(l.line, i18n::str::CHT_ERR_FORMATO);
            return false;
        }
        if (joined.size() > 2 * limit)
        {
            add(l.line, i18n::str::CHT_ERR_GRANDE, std::to_string(limit));
            return false;
        }
        bool odd = false;
        if (!hex_to_bytes(joined, out, odd))
        {
            add(l.line, i18n::str::CHT_ERR_HEX, joined.substr(0, 24));
            return false;
        }
        if (odd) add(l.line, i18n::str::CHT_NOTA_HEX_IMPAR, {}, {}, false);
        return true;
    }

    bool single_token(const code_line& l)
    {
        for (const char ch : l.value)
            if (space(ch))
            {
                add(l.line, i18n::str::CHT_ERR_FORMATO);
                return false;
            }
        return true;
    }

    void run(const std::vector<code_line>& codes)
    {
        unsigned depth = 0;
        bool relative = false;
        for (std::size_t i = 0; i < codes.size(); ++i)
        {
            const code_line& l = codes[i];
            c.lines.emplace_back(l.raw);
            c.code_text.emplace_back(l.raw);
            key_text += '\n';
            key_text += key_line(l);
            const type_info* type = find_type(l.type);
            if (!type)
            {
                add(l.line, i18n::str::CHT_ERR_TIPO_DESCONHECIDO, std::string(1, l.type));
                continue;
            }
            if (!type->runs && !c.unsupported)
            {
                c.unsupported = true;
                add(l.line, i18n::str::CHT_ERR_TIPO_NAO_SUPORTADO, std::string(1, type->type));
            }
            // The second line of a two-line code, consumed with the first
            const auto second = [&](char t) -> const code_line*
            {
                if (i + 1 < codes.size() && codes[i + 1].type == t)
                {
                    ++i;
                    c.lines.emplace_back(codes[i].raw);
                    c.code_text.emplace_back(codes[i].raw);
                    key_text += '\n';
                    key_text += key_line(codes[i]);
                    return &codes[i];
                }
                add(l.line, i18n::str::CHT_ERR_LINHA_FALTANDO, std::string(1, t));
                return nullptr;
            };
            u32 addr = 0;
            if (!address(l, addr))
            {
                // keep the pairs together
                if ((type->type == '4' || type->type == 'A' || type->type == 'B' || type->type == 'F') && i + 1 < codes.size() && codes[i + 1].type == type->type)
                {
                    ++i;
                    c.lines.emplace_back(codes[i].raw);
                    c.code_text.emplace_back(codes[i].raw);
                }
                continue;
            }
            op o;
            o.line = l.line;
            o.addr = addr;
            o.relative = relative && type->type != '6';
            switch (type->type)
            {
            case '0':
            {
                if (!value_bytes(l, l.value, o.bytes, max_write)) break;
                if (!o.relative && !region(l, addr, u64(addr) + o.bytes.size())) break;
                c.prog.ops.push_back(std::move(o));
                break;
            }
            case '1':
            {
                if (l.value.empty())
                {
                    add(l.line, i18n::str::CHT_ERR_FORMATO);
                    break;
                }
                if (!decode_text(l.value, o.bytes))
                {
                    add(l.line, i18n::str::CHT_ERR_TEXTO);
                    break;
                }
                if (o.bytes.size() > max_write)
                {
                    add(l.line, i18n::str::CHT_ERR_GRANDE, std::to_string(max_write));
                    break;
                }
                if (!o.relative && !region(l, addr, u64(addr) + o.bytes.size())) break;
                c.prog.ops.push_back(std::move(o));
                break;
            }
            case '2':
            {
                if (l.value.empty() || !single_token(l))
                {
                    if (l.value.empty()) add(l.line, i18n::str::CHT_ERR_FORMATO);
                    break;
                }
                float f = 0;
                const bool raw_bits = l.value.size() == 8 && all_hex(l.value) && l.value.find('.') == std::string_view::npos;
                if (raw_bits)
                {
                    bool odd = false;
                    hex_to_bytes(l.value, o.bytes, odd);
                }
                else if (parse_float(l.value, f)) o.bytes = float_bytes(f);
                else
                {
                    add(l.line, i18n::str::CHT_ERR_FLOAT, std::string(l.value.substr(0, 24)));
                    break;
                }
                if (!o.relative && !region(l, addr, u64(addr) + 4)) break;
                c.prog.ops.push_back(std::move(o));
                break;
            }
            case '4':
            {
                std::string first;
                const code_line* two = second('4');
                if (!value_bytes(l, l.value, first, 8) || !two) break;
                u64 field = 0, step = 0;
                if (!parse_hex(two->addr, field) || field > 0xffffffffu)
                {
                    add(two->line, i18n::str::CHT_ERR_HEX, std::string(two->addr));
                    break;
                }
                if (!parse_hex(two->value, step))
                {
                    add(two->line, i18n::str::CHT_ERR_HEX, std::string(two->value.substr(0, 24)));
                    break;
                }
                o.kind = op_kind::write_series;
                o.width = static_cast<u8>(first.size());
                o.bytes = std::move(first);
                o.count = static_cast<u32>(field >> 16);
                o.addr_step = static_cast<u32>(field & 0xffff);
                o.value_step = step;
                if (u64(o.count) * o.width > max_series)
                {
                    add(l.line, i18n::str::CHT_ERR_GRANDE, std::to_string(max_series));
                    break;
                }
                const auto [begin, end] = op_span(o);
                if (!o.relative && !region(l, begin, std::max(end, begin + 1))) break;
                c.prog.ops.push_back(std::move(o));
                break;
            }
            case '6':
            {
                u64 offset = 0;
                if (!single_token(l)) break;
                if (!parse_hex(l.value, offset) || offset > 0xffffffffu)
                {
                    add(l.line, l.value.empty() ? i18n::str::CHT_ERR_FORMATO : i18n::str::CHT_ERR_HEX, std::string(l.value.substr(0, 24)));
                    break;
                }
                if (depth >= max_pointer_depth)
                {
                    add(l.line, i18n::str::CHT_ERR_PONTEIRO_FUNDO, std::to_string(max_pointer_depth));
                    break;
                }
                o.kind = op_kind::pointer;
                o.relative = depth > 0;
                o.aux = static_cast<u32>(offset);
                if (!o.relative && !region(l, addr, u64(addr) + 4)) break;
                c.prog.ops.push_back(std::move(o));
                ++depth;
                relative = true;
                continue;
            }
            case 'D':
            case 'E':
            {
                if (!value_bytes(l, l.value, o.bytes, max_write)) break;
                o.kind = type->type == 'D' ? op_kind::if_equal : op_kind::if_mask;
                o.count = 1;
                if (!o.relative && !region(l, addr, u64(addr) + o.bytes.size())) break;
                c.prog.ops.push_back(std::move(o));
                // the guarded line keeps the pointer base
                continue;
            }
            case 'A':
            case 'F':
            {
                u64 length = 0;
                const code_line* two = second(type->type);
                if (!single_token(l) || !two) break;
                if (!parse_hex(l.value, length) || length > 0xffffffffu)
                {
                    add(l.line, i18n::str::CHT_ERR_HEX, std::string(l.value.substr(0, 24)));
                    break;
                }
                if (length > max_series)
                {
                    add(l.line, i18n::str::CHT_ERR_GRANDE, std::to_string(max_series));
                    break;
                }
                u32 destination = 0;
                if (!address(*two, destination)) break;
                o.kind = op_kind::copy;
                o.aux = static_cast<u32>(length);
                o.aux2 = destination;
                if (!o.relative && (!region(l, addr, u64(addr) + length) || !region(*two, destination, u64(destination) + length))) break;
                c.prog.ops.push_back(std::move(o));
                break;
            }
            case 'B':
            {
                u64 end = 0;
                const code_line* two = second('B');
                if (!single_token(l) || !two) break;
                if (!parse_hex(l.value, end) || end > 0xffffffffu)
                {
                    add(l.line, i18n::str::CHT_ERR_HEX, std::string(l.value.substr(0, 24)));
                    break;
                }
                if (end <= addr || end - addr > max_find_range)
                {
                    add(l.line, i18n::str::CHT_ERR_GRANDE, std::to_string(max_find_range));
                    break;
                }
                if (!value_bytes(*two, two->addr, o.bytes, max_write) || !value_bytes(*two, two->value, o.bytes2, max_write)) break;
                if (o.bytes.size() != o.bytes2.size())
                {
                    add(two->line, i18n::str::CHT_ERR_FORMATO);
                    break;
                }
                o.kind = op_kind::find_replace;
                o.aux = static_cast<u32>(end);
                if (!o.relative && !region(l, addr, end)) break;
                c.prog.ops.push_back(std::move(o));
                c.prog.find = true;
                break;
            }
            default: break;
            }
            // The op after a pointer chain is relative to its base; the chain ends there
            depth = 0;
            relative = false;
        }
        c.prog.plain = std::all_of(c.prog.ops.begin(), c.prog.ops.end(), [](const op& o)
        {
            return (o.kind == op_kind::write || o.kind == op_kind::write_series) && !o.relative;
        });
        c.key = fnv1a(key_text);
    }
};
} // namespace detail

// ---------------------------------------------------------------------------------------------------------------
// PC entries (cheats.yml and the clipboard string)

inline u32 pc_key(const pc_entry& e)
{
    return detail::fnv1a("P\n" + detail::hex_upper(e.offset, 8) + '\n' + std::string(pc_type_names[std::size_t(e.type)]) + '\n' + e.script);
}

// "game@@@description@@@type@@@offset@@@script@@@", as cheat_info::to_str writes it
inline std::string pc_clipboard(const pc_entry& e, std::string_view description)
{
    return e.game + "@@@" + std::string(description) + "@@@" + std::to_string(unsigned(e.type)) + "@@@" + std::to_string(e.offset) + "@@@" + e.script + "@@@";
}

inline cheat make_pc_cheat(pc_entry e, std::string_view description, u32 line, std::string code_text = {})
{
    cheat c;
    c.pc = true;
    c.line = line;
    c.name = detail::clip_name(description);
    if (c.name.empty()) c.name = detail::address_text(e.offset);
    std::string shown = "0x" + detail::lower_ascii(detail::hex_upper(e.offset, 8)) + ' ' + std::string(pc_type_names[std::size_t(e.type)]);
    if (!e.script.empty()) shown += ' ' + e.script;
    c.lines.push_back(std::move(shown));
    c.code_text.push_back(code_text.empty() ? pc_clipboard(e, c.name) : std::move(code_text));
    if (!e.script.empty())
    {
        if (!check_script(e.script)) c.problems.push_back({line, i18n::str::CHT_ERR_SCRIPT, e.script.substr(0, 40), {}, true});
    }
    else if (!detail::in_guest(e.offset, u64(e.offset) + pc_width(e.type)))
    {
        c.problems.push_back({line, i18n::str::CHT_ERR_REGIAO, detail::address_text(e.offset), {}, true});
    }
    c.prog.plain = e.script.empty();
    c.key = pc_key(e);
    c.pc_info = std::move(e);
    return c;
}

// A typed value for a PC entry: unsigned (decimal or 0x hex), signed (decimal), float (decimal, '.')
inline bool parse_pc_value(pc_type t, std::string_view typed, std::string& bytes, std::string& canonical)
{
    typed = detail::trim(typed);
    const unsigned width = pc_width(t);
    const u64 max = width == 8 ? ~u64{0} : (u64{1} << (8 * width)) - 1;
    bytes.clear();
    if (t == pc_type::f32_)
    {
        float f = 0;
        if (!detail::parse_float(typed, f)) return false;
        bytes = detail::float_bytes(f);
        canonical = std::string(typed);
        return true;
    }
    const bool is_signed = t >= pc_type::s8_ && t <= pc_type::s64_;
    bool negative = false;
    std::string_view digits = typed;
    if (!digits.empty() && (digits.front() == '-' || digits.front() == '+'))
    {
        negative = digits.front() == '-';
        digits.remove_prefix(1);
    }
    u64 v = 0;
    bool hex = false;
    if (digits.size() > 2 && digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X'))
    {
        hex = true;
        if (!detail::parse_hex(digits.substr(2), v)) return false;
    }
    else if (!detail::parse_decimal(digits, v, ~u64{0})) return false;
    if (negative)
    {
        if (!is_signed || hex) return false;
        const u64 limit = width == 8 ? u64{1} << 63 : u64{1} << (8 * width - 1);
        if (v > limit) return false;
        v = (~v + 1) & max;
    }
    else
    {
        const u64 limit = is_signed && !hex ? (width == 8 ? (u64{1} << 63) - 1 : (u64{1} << (8 * width - 1)) - 1) : max;
        if (v > limit) return false;
    }
    detail::append_be(bytes, v, width);
    canonical = std::string(typed);
    return true;
}

// The program of a PC entry with its value: a write at the offset, or the script then a write at its result
inline program pc_program(const pc_entry& e, std::string_view bytes, u32 line)
{
    program p;
    if (!e.script.empty())
    {
        op s;
        s.kind = op_kind::script;
        s.addr = e.offset;
        s.script = e.script;
        s.line = line;
        p.ops.push_back(std::move(s));
    }
    op w;
    w.kind = op_kind::write;
    w.relative = !e.script.empty();
    w.addr = e.script.empty() ? e.offset : 0;
    w.bytes = std::string(bytes);
    w.line = line;
    p.ops.push_back(std::move(w));
    p.plain = e.script.empty();
    return p;
}

// Clipboard entries joined by "^^^"; garbage gives problems, never an exception. One entry past max_cheats is kept so
// the caller reports the cap.
inline void parse_clipboard(std::string_view text, u32 line, std::vector<cheat>& out, std::vector<problem>& notes)
{
    while (!text.empty() && out.size() <= max_cheats)
    {
        const std::size_t sep = text.find("^^^");
        std::string_view entry = detail::trim(text.substr(0, sep));
        text = sep == std::string_view::npos ? std::string_view{} : text.substr(sep + 3);
        if (entry.empty()) continue;
        std::vector<std::string_view> fields;
        while (fields.size() < 6)
        {
            const std::size_t at = entry.find("@@@");
            fields.push_back(entry.substr(0, at));
            if (at == std::string_view::npos) break;
            entry = entry.substr(at + 3);
        }
        u64 type = 0, offset = 0;
        if (fields.size() < 5 || !detail::parse_decimal(detail::trim(fields[2]), type, pc_type_names.size() - 1))
        {
            add_note(notes, {line, i18n::str::CHT_ERR_FORMATO, {}, {}, false});
            continue;
        }
        if (!detail::parse_decimal(detail::trim(fields[3]), offset, 0xffffffffu))
        {
            add_note(notes, {line, i18n::str::CHT_ERR_ENDERECO, std::string(detail::trim(fields[3]).substr(0, 24)), {}, false});
            continue;
        }
        pc_entry e;
        e.game = std::string(fields[0]);
        e.type = pc_type(type);
        e.offset = static_cast<u32>(offset);
        e.script = std::string(detail::trim(fields[4]));
        const std::string code = std::string(fields[0]) + "@@@" + std::string(fields[1]) + "@@@" + std::string(detail::trim(fields[2])) + "@@@" +
            std::string(detail::trim(fields[3])) + "@@@" + e.script + "@@@";
        out.push_back(make_pc_cheat(std::move(e), fields[1], line, code));
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Artemis / NetCheat / console text (UTF-8, BOM and CRLF accepted). Per line, after trimming:
//   empty                        separator (a cheat goes on after it)
//   // ...                       comment
//   ; key: value                 directive: serial, version, hash, file (file level; "file" resets them), mode,
//                                author, notes (the current cheat); any other ';' line is a comment
//   { or }                       ignored (.ncl blocks)
//   [Title] or #Title            a new cheat
//   ...@@@...                    PC clipboard cheat(s)
//   0 or 1 after code lines      .ncl constant flag of the current cheat (1 constant, 0 once)
//   T ADDR VALUE                 code line
//   Key: value                   metadata: Author/Autor, Notes/Notas, Game/Jogo, Title ID/Serial, Version/Versão
//   anything else                a new cheat with that title; a serial-shaped first word before any code is the
//                                file's game header ("BLUS30443 v1.02")
// Codes before any title make "Cheat 1"; a title without codes is dropped with a note.

inline parsed_file parse_text(std::string_view content, std::string_view source = {})
{
    parsed_file f;
    if (content.size() > file_limit)
    {
        f.notes.push_back({0, i18n::str::CHT_ERR_ARQUIVO_GRANDE, std::string(source), std::to_string(file_limit >> 10), true});
        return f;
    }
    if (content.starts_with("\xEF\xBB\xBF")) content.remove_prefix(3);

    struct pending
    {
        cheat c;
        std::vector<detail::code_line> codes;
        bool lines_capped = false;
    };
    std::vector<pending> list;
    std::string hash, version, author;
    bool capped = false;
    u32 unnamed = 0;
    const auto ignore = [&](u32 line)
    {
        if (f.ignored.size() < 16) f.ignored.push_back(line);
        ++f.ignored_count;
    };
    const auto start = [&](std::string_view name, u32 line) -> bool
    {
        if (list.size() >= max_cheats)
        {
            if (!capped) f.notes.push_back({line, i18n::str::CHT_ERR_MUITOS, std::string(source), std::to_string(max_cheats), false});
            capped = true;
            return false;
        }
        list.emplace_back();
        cheat& c = list.back().c;
        c.name = detail::clip_name(name);
        if (c.name.empty()) c.name = i18n::trf(i18n::str::CHT_SEM_NOME, {++unnamed});
        c.line = line;
        c.hash = hash;
        c.version = version;
        c.author = author;
        return true;
    };
    const auto metadata_key = [](std::string_view key, std::string_view& kind) -> bool
    {
        static constexpr std::pair<std::string_view, std::string_view> keys[] = {{"author", "author"}, {"autor", "author"}, {"by", "author"},
            {"creator", "author"}, {"notes", "notes"}, {"note", "notes"}, {"notas", "notes"}, {"nota", "notes"}, {"game", "game"},
            {"jogo", "game"}, {"title id", "serial"}, {"titleid", "serial"}, {"title_id", "serial"}, {"serial", "serial"}, {"game id", "serial"},
            {"version", "version"}, {"versão", "version"}, {"versao", "version"}, {"app version", "version"}, {"app_ver", "version"}};
        const std::string k = detail::lower_ascii(detail::trim(key));
        for (const auto& [name, what] : keys)
            if (k == name)
            {
                kind = what;
                return true;
            }
        return false;
    };

    u32 line_no = 0;
    std::size_t at = 0;
    while (at < content.size() && !capped)
    {
        const std::size_t end = content.find('\n', at);
        const std::string_view raw = content.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at);
        at = end == std::string_view::npos ? content.size() : end + 1;
        ++line_no;
        const std::string_view t = detail::trim(raw);
        if (t.empty() || t.starts_with("//")) continue;
        if (t.front() == ';')
        {
            const std::string_view body = detail::trim(t.substr(1));
            const std::size_t colon = body.find(':');
            if (colon == std::string_view::npos) continue;
            const std::string key = detail::lower_ascii(detail::trim(body.substr(0, colon)));
            const std::string_view value = detail::trim(body.substr(colon + 1));
            if (key == "serial")
            {
                const std::string s = detail::find_serial(value);
                if (f.serial.empty() && !s.empty()) f.serial = s;
            }
            else if (key == "version")
            {
                version = std::string(value.substr(0, 16));
                if (f.version.empty()) f.version = version;
            }
            else if (key == "hash")
            {
                // the engine's spelling: "PPU-" and 40 lower-case hex digits
                if (value.size() == 44 && detail::iequals(value.substr(0, 4), "PPU-") && detail::all_hex(value.substr(4)))
                    hash = "PPU-" + detail::lower_ascii(value.substr(4));
                else add_note(f.notes, {line_no, i18n::str::CHT_ERR_HASH, {}, {}, false});
            }
            else if (key == "file")
            {
                hash.clear();
                version.clear();
                author.clear();
            }
            else if (key == "mode" || key == "author" || key == "notes")
            {
                if (list.empty())
                {
                    add_note(f.notes, {line_no, i18n::str::CHT_NOTA_DIRETIVA, "; " + key + ":", {}, false});
                    continue;
                }
                cheat& c = list.back().c;
                if (key == "author") c.author = detail::clip_name(value);
                else if (key == "notes") c.notes = std::string(value.substr(0, 512));
                else
                {
                    std::string_view rest = value;
                    std::size_t sp = 0;
                    while (sp < rest.size() && !detail::space(rest[sp])) ++sp;
                    mode m{};
                    if (!parse_mode(rest.substr(0, sp), m))
                    {
                        add_note(c.problems, {line_no, i18n::str::CHT_ERR_MODO, std::string(rest.substr(0, std::min<std::size_t>(sp, 16))), {}, false});
                        continue;
                    }
                    c.wanted = m;
                    u64 period = 0;
                    if (detail::parse_decimal(detail::trim(rest.substr(sp)), period, 1000000)) c.period_ms = clamp_period(period);
                }
            }
            continue;
        }
        if (t.find_first_not_of("{} \t") == std::string_view::npos) continue;
        if ((t.front() == '[' && t.back() == ']' && t.size() >= 2) || t.front() == '#')
        {
            std::string_view title = t;
            if (title.front() == '[') title = title.substr(1, title.size() - 2);
            else
                while (!title.empty() && title.front() == '#') title.remove_prefix(1);
            start(detail::trim(title), line_no);
            continue;
        }
        if (t.find("@@@") != std::string_view::npos)
        {
            std::vector<cheat> pc;
            parse_clipboard(t, line_no, pc, f.notes);
            for (cheat& c : pc)
            {
                if (list.size() >= max_cheats)
                {
                    if (!capped) f.notes.push_back({line_no, i18n::str::CHT_ERR_MUITOS, std::string(source), std::to_string(max_cheats), false});
                    capped = true;
                    break;
                }
                c.hash = hash;
                c.version = version;
                list.emplace_back();
                list.back().c = std::move(c);
            }
            continue;
        }
        if ((t == "0" || t == "1") && !list.empty() && !list.back().c.pc && !list.back().codes.empty())
        {
            cheat& c = list.back().c;
            if (c.wanted == mode::automatic) c.wanted = t == "1" ? mode::constant : mode::once;
            continue;
        }
        detail::code_line code;
        if (detail::split_code_line(t, line_no, code))
        {
            if (list.empty() || list.back().c.pc)
            {
                if (!start({}, line_no)) break;
            }
            pending& p = list.back();
            if (p.codes.size() >= max_code_lines)
            {
                if (!p.lines_capped) p.c.problems.push_back({line_no, i18n::str::CHT_ERR_LINHAS_DEMAIS, p.c.name, std::to_string(max_code_lines), false});
                p.lines_capped = true;
                ignore(line_no);
                continue;
            }
            p.codes.push_back(code);
            continue;
        }
        if (const std::size_t colon = t.find(':'); colon != std::string_view::npos && colon > 0)
        {
            std::string_view kind;
            if (metadata_key(t.substr(0, colon), kind))
            {
                const std::string_view value = detail::trim(t.substr(colon + 1));
                // after a title (before its codes) it is that cheat's; anywhere else it holds for the next cheats
                const bool cheat_level = !list.empty() && list.back().codes.empty() && !list.back().c.pc;
                if (kind == "author")
                {
                    if (cheat_level) list.back().c.author = detail::clip_name(value);
                    else author = detail::clip_name(value);
                }
                else if (kind == "notes")
                {
                    // notes belong to the cheat they follow, before or after its codes
                    if (!list.empty()) list.back().c.notes = std::string(value.substr(0, 512));
                }
                else if (kind == "game")
                {
                    if (f.game.empty()) f.game = detail::clip_name(value);
                    if (f.serial.empty()) f.serial = detail::find_serial(value);
                }
                else if (kind == "serial")
                {
                    if (f.serial.empty()) f.serial = detail::find_serial(value);
                }
                else if (kind == "version")
                {
                    version = std::string(value.substr(0, 16));
                    if (f.version.empty()) f.version = version;
                }
                continue;
            }
        }
        // A game header before any code: "BLUS30443 v1.02"
        if (std::none_of(list.begin(), list.end(), [](const pending& p) { return !p.codes.empty() || p.c.pc; }))
        {
            std::size_t sp = 0;
            while (sp < t.size() && !detail::space(t[sp])) ++sp;
            if (detail::serial_shape(t.substr(0, sp)))
            {
                if (f.serial.empty()) f.serial = detail::upper_ascii(t.substr(0, sp));
                std::string_view v = detail::trim(t.substr(sp));
                if (!v.empty() && (v.front() == 'v' || v.front() == 'V')) v.remove_prefix(1);
                if (!v.empty() && v.front() >= '0' && v.front() <= '9' && v.size() <= 8)
                {
                    version = std::string(v);
                    if (f.version.empty()) f.version = version;
                }
                list.clear();
                unnamed = 0;
                continue;
            }
        }
        start(t, line_no);
    }

    for (pending& p : list)
    {
        if (p.c.pc)
        {
            f.cheats.push_back(std::move(p.c));
            continue;
        }
        if (p.codes.empty())
        {
            f.notes.push_back({p.c.line, i18n::str::CHT_ERR_SEM_CODIGO, p.c.name, {}, false});
            continue;
        }
        detail::converter conv{p.c};
        conv.run(p.codes);
        if (p.c.prog.ops.empty() && !p.c.fatal()) p.c.problems.push_back({p.c.line, i18n::str::CHT_ERR_SEM_CODIGO, p.c.name, {}, true});
        f.cheats.push_back(std::move(p.c));
    }
    if (f.ignored_count)
    {
        std::string lines;
        for (const u32 l : f.ignored) lines += (lines.empty() ? "" : " ") + std::to_string(l);
        if (f.ignored_count > f.ignored.size()) lines += " ...";
        f.notes.push_back({0, i18n::str::CHT_NOTA_LINHAS_IGNORADAS, std::string(source), lines, false});
    }
    return f;
}

// ---------------------------------------------------------------------------------------------------------------
// PC cheats.yml: the subset the PC dialog writes and people edit by hand. A top-level map of game titles; each holds
// a map of offsets (0x hex or decimal) to [description, type name, redirection script], as a block sequence
// ("- item" lines) or a flow sequence; plain, 'single' and "double" quoted scalars; '#' comments. Values are
// never stored there (the player types them on the console).

namespace detail
{
// A scalar at the start of 't': quoted (with its escapes) or plain up to one of 'stops' (outside quotes)
inline bool yml_scalar(std::string_view& t, std::string& out, std::string_view stops)
{
    out.clear();
    t = ltrim(t);
    if (!t.empty() && (t.front() == '"' || t.front() == '\''))
    {
        const char q = t.front();
        std::size_t i = 1;
        for (; i < t.size(); ++i)
        {
            const char c = t[i];
            if (q == '\'' && c == '\'')
            {
                if (i + 1 < t.size() && t[i + 1] == '\'')
                {
                    out += '\'';
                    ++i;
                    continue;
                }
                break;
            }
            if (q == '"' && c == '"') break;
            if (q == '"' && c == '\\' && i + 1 < t.size())
            {
                const char e = t[++i];
                out += e == 'n' ? '\n' : e == 't' ? '\t' : e == '0' ? '\0' : e;
                continue;
            }
            out += c;
        }
        if (i >= t.size()) return false;
        t = t.substr(i + 1);
        return true;
    }
    // a plain scalar ends at a stop character; a ':' stops it only when a space or the line end follows
    std::size_t i = 0;
    for (; i < t.size(); ++i)
    {
        const char c = t[i];
        if (stops.find(c) == std::string_view::npos) continue;
        if (c != ':' || i + 1 == t.size() || space(t[i + 1])) break;
    }
    out = std::string(trim(t.substr(0, i)));
    t = t.substr(i);
    return true;
}

// Removes a '#' comment that starts outside quotes after a space (or at the start)
inline std::string_view yml_strip_comment(std::string_view t) noexcept
{
    char q = 0;
    for (std::size_t i = 0; i < t.size(); ++i)
    {
        const char c = t[i];
        if (q)
        {
            if (c == q && !(q == '"' && i > 0 && t[i - 1] == '\\')) q = 0;
            continue;
        }
        if (c == '"' || c == '\'') q = c;
        else if (c == '#' && (i == 0 || space(t[i - 1]))) return t.substr(0, i);
    }
    return t;
}

inline bool yml_flow(std::string_view t, std::vector<std::string>& items)
{
    t = trim(t);
    if (t.size() < 2 || t.front() != '[' || t.back() != ']') return false;
    t = t.substr(1, t.size() - 2);
    items.clear();
    if (trim(t).empty()) return true;
    while (items.size() < 8)
    {
        std::string item;
        if (!yml_scalar(t, item, ",")) return false;
        items.push_back(std::move(item));
        t = ltrim(t);
        if (t.empty()) return true;
        if (t.front() != ',') return false;
        t.remove_prefix(1);
    }
    return false;
}

inline bool yml_offset(std::string_view t, u32& out) noexcept
{
    u64 v = 0;
    t = trim(t);
    const bool ok = t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X') ? parse_hex(t.substr(2), v) : parse_decimal(t, v, 0xffffffffu);
    if (!ok || v > 0xffffffffu) return false;
    out = static_cast<u32>(v);
    return true;
}
} // namespace detail

inline parsed_file parse_cheats_yml(std::string_view content, std::string_view source = {})
{
    parsed_file f;
    if (content.size() > file_limit)
    {
        f.notes.push_back({0, i18n::str::CHT_ERR_ARQUIVO_GRANDE, std::string(source), std::to_string(file_limit >> 10), true});
        return f;
    }
    if (content.starts_with("\xEF\xBB\xBF")) content.remove_prefix(3);
    std::string game;
    bool in_game = false;
    std::size_t offset_indent = std::string_view::npos;
    struct entry_state
    {
        bool open = false;
        u32 offset = 0, line = 0;
        std::vector<std::string> items;
    } entry;
    u32 unnoted = 0; // bad lines past max_notes: summed up in one note at the end
    const auto bad = [&](u32 line, i18n::str id, std::string arg = {})
    {
        if (!add_note(f.notes, {line, id, std::move(arg), {}, false})) ++unnoted;
        if (f.ignored.size() < 16) f.ignored.push_back(line);
        ++f.ignored_count;
    };
    const auto finish = [&]()
    {
        if (!entry.open) return;
        entry.open = false;
        if (entry.items.size() != 3)
        {
            bad(entry.line, i18n::str::CHT_ERR_YML_CAMPOS);
            return;
        }
        pc_entry e;
        if (!parse_pc_type(entry.items[1], e.type))
        {
            bad(entry.line, i18n::str::CHT_ERR_YML_TIPO, entry.items[1].substr(0, 32));
            return;
        }
        if (f.cheats.size() >= max_cheats)
        {
            if (f.notes.empty() || f.notes.back().id != i18n::str::CHT_ERR_MUITOS)
                f.notes.push_back({entry.line, i18n::str::CHT_ERR_MUITOS, std::string(source), std::to_string(max_cheats), false});
            return;
        }
        e.game = game;
        e.offset = entry.offset;
        e.script = std::string(detail::trim(entry.items[2]));
        f.cheats.push_back(make_pc_cheat(std::move(e), entry.items[0], entry.line));
        if (f.game.empty()) f.game = game;
    };

    u32 line_no = 0;
    std::size_t at = 0;
    while (at < content.size())
    {
        const std::size_t end = content.find('\n', at);
        std::string_view raw = content.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at);
        at = end == std::string_view::npos ? content.size() : end + 1;
        ++line_no;
        if (!raw.empty() && raw.back() == '\r') raw.remove_suffix(1);
        std::size_t indent = 0;
        while (indent < raw.size() && raw[indent] == ' ') ++indent;
        std::string_view t = detail::trim(detail::yml_strip_comment(raw));
        if (t.empty() || t == "---" || t == "...") continue;
        if (indent < raw.size() && raw[indent] == '\t')
        {
            bad(line_no, i18n::str::CHT_ERR_YML_LINHA);
            continue;
        }
        // a block sequence item of the open entry (at the entry's indent or deeper)
        if (entry.open && t.starts_with("-") && (t.size() == 1 || t[1] == ' ') && indent >= offset_indent)
        {
            std::string_view item_text = t.substr(1);
            std::string item;
            if (!detail::yml_scalar(item_text, item, "") || !detail::trim(item_text).empty())
            {
                bad(line_no, i18n::str::CHT_ERR_YML_LINHA);
                continue;
            }
            if (entry.items.size() < 8) entry.items.push_back(std::move(item));
            continue;
        }
        if (indent == 0)
        {
            finish();
            std::string key;
            std::string_view rest = t;
            if (!detail::yml_scalar(rest, key, ":") || (rest = detail::ltrim(rest)).empty() || rest.front() != ':')
            {
                in_game = false;
                bad(line_no, i18n::str::CHT_ERR_YML_LINHA);
                continue;
            }
            rest = detail::trim(rest.substr(1));
            game = detail::clip_name(key);
            in_game = rest.empty() || rest == "{}";
            offset_indent = std::string_view::npos;
            if (!in_game) bad(line_no, i18n::str::CHT_ERR_YML_LINHA);
            continue;
        }
        if (!in_game || (offset_indent != std::string_view::npos && indent != offset_indent))
        {
            bad(line_no, i18n::str::CHT_ERR_YML_LINHA);
            continue;
        }
        finish();
        offset_indent = indent;
        std::string key;
        std::string_view rest = t;
        if (!detail::yml_scalar(rest, key, ":") || (rest = detail::ltrim(rest)).empty() || rest.front() != ':')
        {
            bad(line_no, i18n::str::CHT_ERR_YML_LINHA);
            continue;
        }
        rest = detail::trim(rest.substr(1));
        u32 offset = 0;
        if (!detail::yml_offset(key, offset))
        {
            bad(line_no, i18n::str::CHT_ERR_ENDERECO, key.substr(0, 24));
            continue;
        }
        entry = {};
        entry.open = true;
        entry.offset = offset;
        entry.line = line_no;
        if (!rest.empty())
        {
            if (!detail::yml_flow(rest, entry.items)) entry.items.clear(), entry.items.resize(1);
            finish();
        }
    }
    finish();
    if (unnoted)
    {
        std::string lines;
        for (const u32 l : f.ignored) lines += (lines.empty() ? "" : " ") + std::to_string(l);
        if (f.ignored_count > f.ignored.size()) lines += " ...";
        f.notes.push_back({0, i18n::str::CHT_NOTA_LINHAS_IGNORADAS, std::string(source), lines, false});
    }
    return f;
}

// PC cheats.yml text of some entries (one game), readable by the PC RPCS3 and by parse_cheats_yml
inline std::string cheats_yml_text(std::string_view title, const std::vector<const cheat*>& entries)
{
    const auto quote = [](std::string_view t)
    {
        std::string out = "\"";
        for (const char c : t)
        {
            if (c == '"' || c == '\\') out += '\\';
            if (c == '\n') { out += "\\n"; continue; }
            out += c;
        }
        return out + '"';
    };
    std::string out = quote(title) + ":\n";
    for (const cheat* c : entries)
    {
        if (!c || !c->pc) continue;
        out += "  0x" + detail::lower_ascii(detail::hex_upper(c->pc_info.offset, 8)) + ": [" + quote(c->name) + ", " +
            quote(pc_type_names[std::size_t(c->pc_info.type)]) + ", " + quote(c->pc_info.script) + "]\n";
    }
    return out;
}

// ---------------------------------------------------------------------------------------------------------------
// Interpreter: runs a program through a memory port. The console port is vm::try_access (range-locked, page
// checked) plus vm::ps4_touch_reservations; the host test port is a sparse byte map.
//   Port: access read(u32 addr, void* out, u32 n); access write(u32 addr, const void* in, u32 n)
//   Budget: bool take() - false once this pass may not do one more memory access

enum class access : u8
{
    ok,
    unmapped,   // not mapped (or not readable)
    read_only,  // mapped, not writable
    code,       // executable page: never written live under PPU LLVM
    refused,    // outside the allowed areas (RSX context, video, stack, SPU)
    later,      // not now (rate limit of a privileged write)
};

enum class run_status : u8
{
    done,       // the program ran to its end
    budget,     // stopped by the budget; the cursor resumes it
    not_ready,  // a pointer was null or unmapped, a read failed: skip this pass
    failed,     // a write target was unmapped or read-only
    code,       // a write target is game code
    refused,    // a write target is outside the allowed areas
};

struct cursor
{
    u32 pc = 0;       // next op
    u32 sub = 0;      // stores of a series / bytes of a find range already done
    u64 base = 0;     // pointer base
    bool base_ok = false;
    u32 skip = 0;     // ops still skipped by a false condition
};

struct run_result
{
    run_status status = run_status::done;
    u32 addr = 0;     // the address that stopped the pass
    u32 line = 0;     // its source line
    u32 writes = 0;
};

namespace detail
{
inline run_status write_status(access a) noexcept
{
    switch (a)
    {
    case access::ok: return run_status::done;
    case access::code: return run_status::code;
    case access::refused: return run_status::refused;
    case access::later: return run_status::not_ready;
    case access::unmapped:
    case access::read_only: break;
    }
    return run_status::failed;
}
} // namespace detail

template <typename Port, typename Budget>
run_result run(const program& p, cursor& c, Port& port, Budget& budget)
{
    run_result r;
    const auto stop = [&](run_status s, u64 addr) -> run_result
    {
        r.status = s;
        r.addr = static_cast<u32>(std::min<u64>(addr, 0xffffffffu));
        if (s != run_status::budget) c = {};
        return r;
    };
    const auto fits = [](u64 addr, u64 n) { return n && addr + n <= u64{1} << 32; };
    while (c.pc < p.ops.size())
    {
        const op& o = p.ops[c.pc];
        r.line = o.line;
        if (c.skip)
        {
            --c.skip;
            ++c.pc;
            c.sub = 0;
            continue;
        }
        u64 ea = o.addr;
        if (o.relative)
        {
            if (!c.base_ok) return stop(run_status::not_ready, 0);
            ea = c.base + o.addr;
        }
        switch (o.kind)
        {
        case op_kind::write:
        {
            if (!fits(ea, o.bytes.size())) return stop(run_status::not_ready, ea);
            if (!budget.take()) return stop(run_status::budget, ea);
            const access a = port.write(static_cast<u32>(ea), o.bytes.data(), static_cast<u32>(o.bytes.size()));
            if (a != access::ok) return stop(detail::write_status(a), ea);
            ++r.writes;
            break;
        }
        case op_kind::write_series:
        {
            const unsigned width = o.width;
            if (!width || width > 8) break;
            const u64 mask = width == 8 ? ~u64{0} : (u64{1} << (8 * width)) - 1;
            const u64 first = detail::read_be(o.bytes.data(), width);
            for (; c.sub < o.count; ++c.sub)
            {
                const u64 at = ea + u64(c.sub) * o.addr_step;
                if (!fits(at, width)) return stop(run_status::not_ready, at);
                if (!budget.take()) return stop(run_status::budget, at);
                std::string value;
                detail::append_be(value, (first + u64(c.sub) * o.value_step) & mask, width);
                const access a = port.write(static_cast<u32>(at), value.data(), width);
                if (a != access::ok) return stop(detail::write_status(a), at);
                ++r.writes;
            }
            break;
        }
        case op_kind::pointer:
        {
            if (!fits(ea, 4)) return stop(run_status::not_ready, ea);
            if (!budget.take()) return stop(run_status::budget, ea);
            char buf[4];
            if (port.read(static_cast<u32>(ea), buf, 4) != access::ok) return stop(run_status::not_ready, ea);
            const u64 pointer = detail::read_be(buf, 4);
            if (!pointer || pointer + o.aux > 0xffffffffu) return stop(run_status::not_ready, ea);
            c.base = pointer + o.aux;
            c.base_ok = true;
            break;
        }
        case op_kind::script:
        {
            if (!budget.take()) return stop(run_status::budget, ea);
            u32 reads = 0;
            u64 result = 0;
            const bool ok = detail::eval_script(o.script, o.addr, [&](u32 addr, u32& out) -> bool
            {
                char buf[4];
                if (++reads > 64 || port.read(addr, buf, 4) != access::ok) return false;
                out = static_cast<u32>(detail::read_be(buf, 4));
                return true;
            }, result, 0);
            if (!ok) return stop(run_status::not_ready, o.addr);
            c.base = result;
            c.base_ok = true;
            break;
        }
        case op_kind::if_equal:
        case op_kind::if_mask:
        {
            const u32 n = static_cast<u32>(o.bytes.size());
            if (!fits(ea, n)) return stop(run_status::not_ready, ea);
            if (!budget.take()) return stop(run_status::budget, ea);
            std::string memory(n, '\0');
            if (port.read(static_cast<u32>(ea), memory.data(), n) != access::ok) return stop(run_status::not_ready, ea);
            bool match = true;
            for (u32 i = 0; i < n && match; ++i)
            {
                const u8 m = static_cast<u8>(memory[i]), v = static_cast<u8>(o.bytes[i]);
                match = o.kind == op_kind::if_equal ? m == v : (m & v) == v;
            }
            if (!match) c.skip = o.count;
            break;
        }
        case op_kind::copy:
        {
            const u64 dst = o.relative ? c.base + o.aux2 : o.aux2;
            if (!fits(ea, o.aux) || !fits(dst, o.aux)) return stop(run_status::not_ready, ea);
            // one memory access per 4 KiB each way; the copy itself is never split (overlapping ranges stay exact)
            for (u32 k = 0; k < 2 * (o.aux / 4096 + 1); ++k)
                if (!budget.take()) return stop(run_status::budget, ea);
            std::string buf(o.aux, '\0');
            if (port.read(static_cast<u32>(ea), buf.data(), o.aux) != access::ok) return stop(run_status::not_ready, ea);
            const access a = port.write(static_cast<u32>(dst), buf.data(), o.aux);
            if (a != access::ok) return stop(detail::write_status(a), dst);
            ++r.writes;
            break;
        }
        case op_kind::find_replace:
        {
            const u64 end = o.relative ? c.base + o.aux : o.aux;
            const u32 n = static_cast<u32>(o.bytes.size());
            if (!n || end <= ea || end > u64{1} << 32) break;
            constexpr u32 chunk = 4096;
            bool replaced = false;
            while (!replaced && ea + c.sub + n <= end)
            {
                if (!budget.take()) return stop(run_status::budget, ea + c.sub);
                const u64 from = ea + c.sub;
                const u32 len = static_cast<u32>(std::min<u64>(u64{chunk} + n - 1, end - from));
                std::string buf(len, '\0');
                if (port.read(static_cast<u32>(from), buf.data(), len) != access::ok) return stop(run_status::not_ready, from);
                const std::size_t pos = buf.find(o.bytes);
                if (pos != std::string::npos)
                {
                    const access a = port.write(static_cast<u32>(from + pos), o.bytes2.data(), n);
                    if (a != access::ok) return stop(detail::write_status(a), from + pos);
                    ++r.writes;
                    replaced = true;
                }
                c.sub += chunk;
            }
            break;
        }
        }
        ++c.pc;
        c.sub = 0;
    }
    c = {};
    r.status = run_status::done;
    return r;
}

// ---------------------------------------------------------------------------------------------------------------
// Modes: which ops go into the module at load and which run live

// A LOAD segment of the main executable, [begin, end)
struct segment
{
    u32 begin = 0;
    u32 end = 0;
    bool exec = false;
};

enum class target : u8 { code, data, outside };

// code: touches an executable segment; data: wholly inside non-executable segments; outside: anything else
inline target classify(u64 begin, u64 end, const std::vector<segment>& segs) noexcept
{
    u64 covered = 0;
    for (const segment& s : segs)
    {
        const u64 b = std::max<u64>(begin, s.begin), e = std::min<u64>(end, s.end);
        if (b >= e) continue;
        if (s.exec) return target::code;
        covered += e - b;
    }
    return end > begin && covered == end - begin ? target::data : target::outside;
}

struct plan
{
    mode resolved = mode::automatic;   // automatic only while the segments are unknown
    std::vector<op> boot;              // absolute writes into the module at load
    program live;                      // ops of the tick
    std::vector<problem> problems;
    bool code_part = false;            // some ops change the game's code (written at load only)
};

// wanted: the player's (or the file's) choice. Code ops are always written at load (a live code write does nothing
// under PPU LLVM); data ops follow the mode; pointers, conditions, copies and find-replace run live only.
inline plan make_plan(const program& prog, mode wanted, const std::vector<segment>* segs)
{
    plan pl;
    if (!prog.plain)
    {
        pl.live = prog;
        pl.resolved = prog.find ? mode::once : wanted == mode::once ? mode::once : mode::constant;
        if (wanted == mode::boot) pl.problems.push_back({0, i18n::str::CHT_ERR_INICIO_DINAMICO, {}, {}, false});
        if (segs)
        {
            for (const op& o : prog.ops)
            {
                if (o.relative || o.kind == op_kind::pointer || o.kind == op_kind::script || o.kind == op_kind::if_equal || o.kind == op_kind::if_mask) continue;
                const u64 b = o.kind == op_kind::copy ? o.aux2 : o.addr;
                const u64 e = o.kind == op_kind::copy ? u64(o.aux2) + o.aux : o.kind == op_kind::find_replace ? u64(o.aux) : op_span(o).second;
                if (classify(b, std::max(e, b + 1), *segs) == target::code)
                    pl.problems.push_back({o.line, i18n::str::CHT_ERR_CODIGO_DINAMICO, {}, {}, true});
            }
        }
        return pl;
    }
    // before the first boot (or a PC entry without its value) nothing can be placed yet
    if (!segs || prog.ops.empty())
    {
        pl.resolved = wanted;
        return pl;
    }
    pl.live.plain = true;
    const auto place = [&](op o, target t)
    {
        if (t == target::code)
        {
            pl.code_part = true;
            pl.boot.push_back(std::move(o));
        }
        else if (wanted == mode::boot)
        {
            if (t == target::data) pl.boot.push_back(std::move(o));
            else pl.problems.push_back({o.line, i18n::str::CHT_ERR_ENDERECO_FORA_EXEC, detail::address_text(o.addr), {}, false});
        }
        else pl.live.ops.push_back(std::move(o));
    };
    for (const op& o : prog.ops)
    {
        if (o.kind == op_kind::write)
        {
            const auto [b, e] = op_span(o);
            if (e > b) place(o, classify(b, e, *segs));
            continue;
        }
        if (!o.count || !o.width || o.width > 8) continue;
        // a series stays one op when every store has the same target, else one write per store
        const auto store = [&](u32 k)
        {
            const u64 at = u64(o.addr) + u64(k) * o.addr_step;
            return classify(at, at + o.width, *segs);
        };
        const target first_target = store(0);
        bool same = true;
        for (u32 k = 1; k < o.count && same; ++k) same = store(k) == first_target;
        if (same)
        {
            place(o, first_target);
            continue;
        }
        const u64 mask = o.width == 8 ? ~u64{0} : (u64{1} << (8 * o.width)) - 1;
        const u64 first = detail::read_be(o.bytes.data(), o.width);
        for (u32 k = 0; k < o.count; ++k)
        {
            op w;
            w.kind = op_kind::write;
            w.line = o.line;
            w.addr = static_cast<u32>(u64(o.addr) + u64(k) * o.addr_step);
            detail::append_be(w.bytes, (first + u64(k) * o.value_step) & mask, o.width);
            const target t = store(k);
            place(std::move(w), t);
        }
    }
    if (wanted == mode::boot) pl.resolved = mode::boot;
    else if (pl.live.ops.empty()) pl.resolved = mode::boot;
    else pl.resolved = wanted == mode::once ? mode::once : mode::constant;
    return pl;
}

// ---------------------------------------------------------------------------------------------------------------
// .seen: one line per executable booted for the title, most recent first:
//   PPU-<40 hex> <app_ver|-> exec=00010000-0034ffff,... data=00350000-003fffff,...   (inclusive ends)

struct seen_line
{
    std::string hash, app_version;
    std::vector<segment> segs;
};

struct seen_info
{
    std::vector<seen_line> lines;
};

inline seen_info parse_seen(std::string_view text)
{
    seen_info s;
    std::size_t at = 0;
    while (at < text.size() && s.lines.size() < max_seen_lines)
    {
        const std::size_t end = text.find('\n', at);
        const std::string_view line = detail::trim(text.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at));
        at = end == std::string_view::npos ? text.size() : end + 1;
        if (line.empty() || line.front() == '#' || !line.starts_with("PPU-")) continue;
        seen_line l;
        std::string_view rest = line;
        const auto token = [&]()
        {
            rest = detail::ltrim(rest);
            std::size_t n = 0;
            while (n < rest.size() && !detail::space(rest[n])) ++n;
            const std::string_view t = rest.substr(0, n);
            rest = rest.substr(n);
            return t;
        };
        l.hash = std::string(token());
        const std::string_view version = token();
        l.app_version = version == "-" ? std::string() : std::string(version.substr(0, 16));
        bool ok = l.hash.size() == 44 && detail::all_hex(std::string_view(l.hash).substr(4));
        for (std::string_view t = token(); ok && !t.empty(); t = token())
        {
            const bool exec = t.starts_with("exec=");
            if (!exec && !t.starts_with("data=")) continue;
            t.remove_prefix(5);
            while (!t.empty() && l.segs.size() < 64)
            {
                const std::size_t comma = t.find(',');
                const std::string_view range = t.substr(0, comma);
                t = comma == std::string_view::npos ? std::string_view{} : t.substr(comma + 1);
                const std::size_t dash = range.find('-');
                u64 b = 0, e = 0;
                if (dash == std::string_view::npos || !detail::parse_hex(range.substr(0, dash), b) || !detail::parse_hex(range.substr(dash + 1), e) ||
                    b > e || e > 0xffffffffu)
                {
                    ok = false;
                    break;
                }
                l.segs.push_back({static_cast<u32>(b), static_cast<u32>(std::min<u64>(e + 1, 0xffffffffu)), exec});
            }
        }
        if (ok) s.lines.push_back(std::move(l));
    }
    return s;
}

inline std::string seen_text(const seen_info& s)
{
    std::string out(seen_header);
    out += '\n';
    for (const seen_line& l : s.lines)
    {
        out += l.hash + ' ' + (l.app_version.empty() ? "-" : l.app_version);
        for (const bool exec : {true, false})
        {
            std::string list;
            for (const segment& g : l.segs)
                if (g.exec == exec && g.end > g.begin)
                    list += (list.empty() ? "" : ",") + detail::lower_ascii(detail::hex_upper(g.begin, 8)) + '-' + detail::lower_ascii(detail::hex_upper(g.end - 1, 8));
            if (!list.empty()) out += (exec ? " exec=" : " data=") + list;
        }
        out += '\n';
    }
    return out;
}

// Puts 'line' first (replacing the line of the same hash); true when the text changes
inline bool seen_update(seen_info& s, seen_line line)
{
    const auto same = std::find_if(s.lines.begin(), s.lines.end(), [&](const seen_line& l) { return l.hash == line.hash; });
    if (same == s.lines.begin() && same != s.lines.end() && same->app_version == line.app_version && same->segs.size() == line.segs.size() &&
        std::equal(same->segs.begin(), same->segs.end(), line.segs.begin(), [](const segment& a, const segment& b) { return a.begin == b.begin && a.end == b.end && a.exec == b.exec; }))
        return false;
    if (same != s.lines.end()) s.lines.erase(same);
    s.lines.insert(s.lines.begin(), std::move(line));
    if (s.lines.size() > max_seen_lines) s.lines.resize(max_seen_lines);
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// .state: "on|off <auto|boot|once|constant> <period ms> <key, 8 hex> <name>[ = <typed value>]"

struct state_entry
{
    bool on = false;
    mode wanted = mode::automatic;
    u32 period_ms = period_default_ms;
    u32 key = 0;
    std::string rest;   // name, then " = value" for PC entries
};

inline std::vector<state_entry> parse_state(std::string_view text)
{
    std::vector<state_entry> out;
    std::size_t at = 0;
    while (at < text.size() && out.size() < 4 * max_cheats)
    {
        const std::size_t end = text.find('\n', at);
        const std::string_view line = detail::trim(text.substr(at, end == std::string_view::npos ? std::string_view::npos : end - at));
        at = end == std::string_view::npos ? text.size() : end + 1;
        if (line.empty() || line.front() == '#') continue;
        std::string_view rest = line;
        const auto token = [&]()
        {
            rest = detail::ltrim(rest);
            std::size_t n = 0;
            while (n < rest.size() && !detail::space(rest[n])) ++n;
            const std::string_view t = rest.substr(0, n);
            rest = rest.substr(n);
            return t;
        };
        state_entry e;
        const std::string_view sw = token(), m = token(), period = token(), key = token();
        u64 p = 0, k = 0;
        if ((sw != "on" && sw != "off") || !parse_mode(m, e.wanted) || !detail::parse_decimal(period, p, 1000000) || key.size() != 8 || !detail::parse_hex(key, k))
            continue;
        e.on = sw == "on";
        e.period_ms = clamp_period(p);
        e.key = static_cast<u32>(k);
        e.rest = std::string(detail::trim(rest));
        out.push_back(std::move(e));
    }
    return out;
}

// The typed value of a PC entry's state line (after the last " = ")
inline bool state_value(std::string_view rest, std::string& value)
{
    const std::size_t at = rest.rfind(" = ");
    if (at == std::string_view::npos) return false;
    value = std::string(detail::trim(rest.substr(at + 3)));
    return !value.empty();
}

// ---------------------------------------------------------------------------------------------------------------
// One game's cheats as the XMB lists them

struct row
{
    std::string name, author, notes, source;   // source = file name
    u32 key = 0;
    bool on = false;
    mode wanted = mode::automatic, resolved = mode::automatic;
    u32 period_ms = period_default_ms;
    bool user = false, needs_value = false, dynamic = false, valid = true;
    std::string value;                         // PC entries: the typed value
    std::vector<std::string> lines;
    std::vector<problem> problems;
    // engine side
    program prog;
    std::vector<std::string> code_text;
    std::string hash, version;
    bool pc = false;
    pc_entry pc_info;
    bool code_part = false;
    u32 line = 0;
};

struct game_cheats
{
    std::string serial, title, app_version;
    std::vector<row> rows;
    std::vector<problem> notes;
    seen_info seen;
    std::vector<std::string> files;     // files read
};

struct source_file
{
    std::string name;     // file name in the cheats folder
    std::string content;
    bool user = false;    // <TITLE_ID>_user.txt
    bool yml = false;     // a PC cheats.yml
};

inline bool titles_match(std::string_view a, std::string_view b)
{
    if (a == b) return true;
    const std::string fa = detail::fold_title(a), fb = detail::fold_title(b);
    return !fa.empty() && fa == fb;
}

// Segments of the most recent executable seen, or null before the first boot
inline const std::vector<segment>* seen_segments(const game_cheats& g) noexcept
{
    return g.seen.lines.empty() ? nullptr : &g.seen.lines.front().segs;
}

inline void resolve_row(row& r, const game_cheats& g)
{
    const plan pl = make_plan(r.prog, r.wanted, seen_segments(g));
    r.resolved = pl.resolved;
    r.code_part = pl.code_part && pl.resolved != mode::boot;
    // dynamic ops on code found with the seen segments make the cheat invalid
    r.problems.erase(std::remove_if(r.problems.begin(), r.problems.end(), [](const problem& p) { return p.id == i18n::str::CHT_ERR_CODIGO_DINAMICO; }), r.problems.end());
    for (const problem& p : pl.problems)
        if (p.id == i18n::str::CHT_ERR_CODIGO_DINAMICO) r.problems.push_back(p);
    r.valid = std::none_of(r.problems.begin(), r.problems.end(), [](const problem& p) { return p.fatal; });
    if (!r.valid) r.on = false;
}

// Rebuilds a PC entry's program from its typed value (none = needs a value)
inline bool apply_pc_value(row& r, std::string_view typed)
{
    std::string bytes, canonical;
    if (!r.pc || !parse_pc_value(r.pc_info.type, typed, bytes, canonical)) return false;
    r.value = canonical;
    r.prog = pc_program(r.pc_info, bytes, r.line);
    return true;
}

inline row make_row(cheat&& c, const source_file& f)
{
    row r;
    r.name = std::move(c.name);
    r.author = std::move(c.author);
    r.notes = std::move(c.notes);
    r.source = f.name;
    r.key = c.key;
    r.wanted = c.wanted;
    r.period_ms = c.period_ms ? c.period_ms : period_default_ms;
    r.user = f.user;
    r.pc = c.pc;
    r.needs_value = c.pc;
    r.dynamic = !c.prog.plain;
    r.lines = std::move(c.lines);
    r.code_text = std::move(c.code_text);
    r.problems = std::move(c.problems);
    r.prog = std::move(c.prog);
    r.hash = std::move(c.hash);
    r.version = std::move(c.version);
    r.pc_info = std::move(c.pc_info);
    r.line = c.line;
    r.valid = std::none_of(r.problems.begin(), r.problems.end(), [](const problem& p) { return p.fatal; });
    return r;
}

// Builds the list from the file contents (user file first, so a copied cheat replaces its import), the state file
// and the seen file. Cheats are never turned on here: only a state line saying "on" does that.
inline game_cheats build_game(std::string serial, std::string title, std::string app_version, const std::vector<source_file>& files,
    std::string_view state, std::string_view seen)
{
    game_cheats g;
    g.serial = std::move(serial);
    g.title = std::move(title);
    g.app_version = std::move(app_version);
    g.seen = parse_seen(seen);
    std::vector<u32> keys;
    bool capped = false;
    for (const source_file& f : files)
    {
        g.files.push_back(f.name);
        parsed_file pf = f.yml ? parse_cheats_yml(f.content, f.name) : parse_text(f.content, f.name);
        for (problem& p : pf.notes) g.notes.push_back(std::move(p));
        if (f.yml)
        {
            // the entries of this game's title; a file with a single other title is taken as this game's
            std::vector<std::string> titles;
            for (const cheat& c : pf.cheats)
                if (std::find(titles.begin(), titles.end(), c.pc_info.game) == titles.end()) titles.push_back(c.pc_info.game);
            const bool any = std::any_of(titles.begin(), titles.end(), [&](const std::string& t) { return titles_match(t, g.title); });
            if (!any && titles.size() == 1 && !g.title.empty())
                g.notes.push_back({0, i18n::str::CHT_NOTA_TITULO_DIFERENTE, f.name, titles.front(), false});
            pf.cheats.erase(std::remove_if(pf.cheats.begin(), pf.cheats.end(), [&](const cheat& c)
            {
                return any ? !titles_match(c.pc_info.game, g.title) : titles.size() != 1;
            }), pf.cheats.end());
        }
        for (cheat& c : pf.cheats)
        {
            if (std::find(keys.begin(), keys.end(), c.key) != keys.end())
            {
                g.notes.push_back({c.line, i18n::str::CHT_NOTA_DUPLICADO, c.name, {}, false});
                continue;
            }
            if (g.rows.size() >= max_cheats)
            {
                if (!capped) g.notes.push_back({0, i18n::str::CHT_ERR_MUITOS, f.name, std::to_string(max_cheats), false});
                capped = true;
                break;
            }
            keys.push_back(c.key);
            g.rows.push_back(make_row(std::move(c), f));
        }
    }
    for (const state_entry& e : parse_state(state))
    {
        for (row& r : g.rows)
        {
            if (r.key != e.key) continue;
            r.wanted = e.wanted;
            r.period_ms = e.period_ms;
            std::string value;
            if (r.pc && state_value(e.rest, value)) apply_pc_value(r, value);
            r.on = e.on && r.valid && (!r.pc || !r.value.empty());
            break;
        }
    }
    for (row& r : g.rows) resolve_row(r, g);
    return g;
}

inline std::string state_text(const game_cheats& g)
{
    std::string out(state_header);
    out += '\n';
    for (const row& r : g.rows)
    {
        out += r.on ? "on " : "off ";
        out += mode_token(r.wanted);
        out += ' ' + std::to_string(r.period_ms) + ' ' + detail::lower_ascii(detail::hex_upper(r.key, 8)) + ' ' + r.name;
        if (r.pc && !r.value.empty()) out += " = " + r.value;
        out += '\n';
    }
    return out;
}

// <TITLE_ID>_user.txt: the console's own cheats, in the text format above
inline std::string user_text(const game_cheats& g)
{
    std::string out = "; RPCS3 PS4 cheats (Artemis/NetCheat code lines)\n; serial: " + g.serial + "\n";
    for (const row& r : g.rows)
    {
        if (!r.user) continue;
        out += "\n; file: " + r.source + "\n";
        if (!r.hash.empty()) out += "; hash: " + r.hash + "\n";
        if (!r.version.empty()) out += "; version: " + r.version + "\n";
        // the cheat-level directives follow the line that starts the cheat (the title, or a PC clipboard line)
        if (!r.pc) out += "[" + r.name + "]\n";
        else
            for (const std::string& l : r.code_text) out += l + "\n";
        if (!r.author.empty()) out += "; author: " + r.author + "\n";
        if (!r.notes.empty()) out += "; notes: " + r.notes + "\n";
        if (!r.pc)
            for (const std::string& l : r.code_text) out += l + "\n";
    }
    return out;
}

// ---------------------------------------------------------------------------------------------------------------
// XMB texts (interface language)

inline i18n::str mode_name(mode m) noexcept
{
    switch (m)
    {
    case mode::boot: return i18n::str::CHT_MODO_INICIO;
    case mode::once: return i18n::str::CHT_MODO_UMA_VEZ;
    case mode::constant: return i18n::str::CHT_MODO_CONSTANTE;
    case mode::automatic: break;
    }
    return i18n::str::CHT_MODO_AUTOMATICO;
}

inline const problem* first_fatal(const row& r) noexcept
{
    for (const problem& p : r.problems)
        if (p.fatal) return &p;
    return nullptr;
}

// "Ligado · reescrito a cada 100 ms · 1 linha(s) · Autor: X" / "Inválido: Linha 4: ..."
inline std::string row_subtitle(const row& r, const game_cheats& /*g*/)
{
    if (const problem* p = first_fatal(r)) return i18n::trf(i18n::str::CHT_INVALIDO, {i18n::raw(problem_text(*p))});
    std::string s;
    switch (r.resolved)
    {
    case mode::constant: s = i18n::trf(r.on ? i18n::str::CHT_LIGADO_CONSTANTE : i18n::str::CHT_DESLIGADO_CONSTANTE, {r.period_ms}); break;
    case mode::once: s = text(r.on ? i18n::str::CHT_LIGADO_UMA_VEZ : i18n::str::CHT_DESLIGADO_UMA_VEZ); break;
    case mode::boot: s = text(r.on ? i18n::str::CHT_LIGADO_INICIO : i18n::str::CHT_DESLIGADO_INICIO); break;
    case mode::automatic: s = text(r.on ? i18n::str::CHT_LIGADO_AUTOMATICO : i18n::str::CHT_DESLIGADO_AUTOMATICO); break;
    }
    if (r.needs_value) s += r.value.empty() ? text(i18n::str::CHT_VALOR_DIGITE) : i18n::trf(i18n::str::CHT_VALOR, {r.value});
    if (r.code_part) s += text(i18n::str::CHT_PARTE_INICIO);
    s += i18n::trf(i18n::str::CHT_N_LINHAS, {static_cast<unsigned>(r.lines.size())});
    if (r.dynamic) s += text(i18n::str::CHT_PONTEIRO);
    if (!r.author.empty()) s += i18n::trf(i18n::str::CHT_AUTOR, {r.author});
    if (r.pc) s += i18n::trf(i18n::str::CHT_PC_TIPO, {pc_type_names[std::size_t(r.pc_info.type)]});
    return s;
}

// Lines under the list title: version mismatch, executables seen, the RPCN warning (cheats are warned about online,
// never blocked)
inline std::vector<std::string> header_lines(const game_cheats& g, bool rpcn_online)
{
    std::vector<std::string> out;
    const std::string installed = detail::fold_version(g.app_version);
    for (const row& r : g.rows)
    {
        if (r.version.empty() || installed.empty() || detail::fold_version(r.version) == installed) continue;
        out.push_back(i18n::trf(i18n::str::CHT_VERSAO_DIFERENTE, {r.version, g.app_version}));
        break;
    }
    if (g.seen.lines.size() > 1) out.push_back(i18n::trf(i18n::str::CHT_EXECUTAVEIS_VISTOS, {static_cast<unsigned>(g.seen.lines.size())}));
    if (rpcn_online && !g.rows.empty()) out.push_back(text(i18n::str::CHT_AVISO_ONLINE));
    return out;
}

// "Ver linhas": the code lines, every problem, the executable restriction and the current executable
inline std::vector<std::string> row_details(const row& r, const game_cheats& g)
{
    std::vector<std::string> out = r.lines;
    for (const problem& p : r.problems) out.push_back(problem_text(p));
    if (!r.notes.empty()) out.push_back(r.notes);
    if (!r.hash.empty()) out.push_back(i18n::trf(i18n::str::CHT_HASH_SO, {r.hash}));
    if (!g.seen.lines.empty()) out.push_back(i18n::trf(i18n::str::CHT_HASH_ATUAL, {g.seen.lines.front().hash}));
    return out;
}

inline std::size_t count_on(const game_cheats& g) noexcept
{
    return static_cast<std::size_t>(std::count_if(g.rows.begin(), g.rows.end(), [](const row& r) { return r.on; }));
}

// ---------------------------------------------------------------------------------------------------------------
// Edits of the model (the PS4 part writes the files they name)

struct edit_files
{
    bool state = false;
    bool user = false;
};

inline bool edit_on(game_cheats& g, std::size_t i, bool on, std::string& message, edit_files& files)
{
    if (i >= g.rows.size()) return false;
    row& r = g.rows[i];
    if (on && !r.valid)
    {
        const problem* p = first_fatal(r);
        message = i18n::trf(i18n::str::CHT_ERR_NAO_LIGA, {i18n::raw(p ? problem_text(*p) : std::string())});
        return false;
    }
    if (on && r.needs_value && r.value.empty())
    {
        message = text(i18n::str::CHT_ERR_PRECISA_VALOR);
        return false;
    }
    r.on = on;
    files.state = true;
    message = text(i18n::str::CHT_SALVO_PROXIMO_INICIO);
    return true;
}

inline bool edit_mode(game_cheats& g, std::size_t i, mode m, u32 period_ms, std::string& message, edit_files& files)
{
    if (i >= g.rows.size()) return false;
    if (period_ms && (period_ms < period_min_ms || period_ms > period_max_ms))
    {
        message = i18n::trf(i18n::str::CHT_ERR_INTERVALO, {period_min_ms, period_max_ms});
        return false;
    }
    row& r = g.rows[i];
    r.wanted = m;
    if (period_ms) r.period_ms = period_ms;
    resolve_row(r, g);
    files.state = true;
    message = text(i18n::str::CHT_SALVO_PROXIMO_INICIO);
    return true;
}

inline bool edit_value(game_cheats& g, std::size_t i, std::string_view typed, std::string& message, edit_files& files)
{
    if (i >= g.rows.size() || !g.rows[i].pc) return false;
    row& r = g.rows[i];
    if (!apply_pc_value(r, typed))
    {
        message = i18n::trf(i18n::str::CHT_ERR_VALOR, {detail::trim(typed).substr(0, 32), pc_type_names[std::size_t(r.pc_info.type)]});
        return false;
    }
    resolve_row(r, g);
    files.state = true;
    message = text(i18n::str::CHT_SALVO_PROXIMO_INICIO);
    return true;
}

namespace detail
{
// Codes typed on the keyboard: lines separated by ';' (outside double quotes) or new lines
inline std::string typed_lines(std::string_view codes)
{
    std::string out;
    bool quoted = false;
    for (std::size_t i = 0; i < codes.size(); ++i)
    {
        const char c = codes[i];
        if (c == '"' && (i == 0 || codes[i - 1] != '\\')) quoted = !quoted;
        if (!quoted && (c == ';' || c == '\r')) out += '\n';
        else out += c;
    }
    return out;
}

// One cheat from typed codes, problems with the typed line numbers (the title is line 1)
inline bool typed_cheat(std::string_view name, std::string_view codes, cheat& out, std::string& message)
{
    parsed_file pf = parse_text("[" + std::string(name) + "]\n" + typed_lines(codes));
    if (pf.cheats.size() > 1)
    {
        message = problem_text({pf.cheats[1].line > 1 ? pf.cheats[1].line - 1 : 1, i18n::str::CHT_ERR_FORMATO, {}, {}, true});
        return false;
    }
    if (pf.cheats.size() != 1 || pf.cheats.front().lines.empty())
    {
        message = i18n::trf(i18n::str::CHT_ERR_SEM_CODIGO, {std::string(name)});
        return false;
    }
    // a typed line that is neither a code nor a comment would start another cheat: report it as a format error
    for (const problem& n : pf.notes)
        if (n.id == i18n::str::CHT_ERR_SEM_CODIGO && n.line > 1)
        {
            message = problem_text({n.line - 1, i18n::str::CHT_ERR_FORMATO, {}, {}, true});
            return false;
        }
    cheat& c = pf.cheats.front();
    for (problem& p : c.problems)
        if (p.line) --p.line;
    if (const auto bad = std::find_if(c.problems.begin(), c.problems.end(), [](const problem& p) { return p.fatal; }); bad != c.problems.end())
    {
        message = problem_text(*bad);
        return false;
    }
    if (pf.ignored_count)
    {
        message = problem_text({pf.ignored.front() - 1, i18n::str::CHT_ERR_FORMATO, {}, {}, true});
        return false;
    }
    // a typed clipboard line takes the typed name (the key does not depend on it)
    if (c.pc)
    {
        c.name = std::string(name);
        c.code_text = {pc_clipboard(c.pc_info, c.name)};
    }
    out = std::move(c);
    return true;
}

inline std::string user_file_name(const game_cheats& g) { return g.serial + "_user.txt"; }
} // namespace detail

inline bool edit_add_user(game_cheats& g, std::string_view name, std::string_view codes, std::string& message, edit_files& files)
{
    const std::string clean = detail::clip_name(name);
    if (clean.empty())
    {
        message = text(i18n::str::CHT_ERR_NOME_VAZIO);
        return false;
    }
    if (g.rows.size() >= max_cheats)
    {
        message = i18n::trf(i18n::str::CHT_ERR_MUITOS, {detail::user_file_name(g), static_cast<unsigned>(max_cheats)});
        return false;
    }
    cheat c;
    if (!detail::typed_cheat(clean, codes, c, message)) return false;
    if (std::any_of(g.rows.begin(), g.rows.end(), [&](const row& r) { return r.key == c.key; }))
    {
        message = i18n::trf(i18n::str::CHT_NOTA_DUPLICADO, {clean});
        return false;
    }
    source_file f{detail::user_file_name(g), {}, true, false};
    row r = make_row(std::move(c), f);
    resolve_row(r, g);
    g.rows.push_back(std::move(r));
    files.user = files.state = true;
    message = text(i18n::str::CHT_SALVO);
    return true;
}

inline bool edit_codes(game_cheats& g, std::size_t i, std::string_view codes, std::string& message, edit_files& files)
{
    if (i >= g.rows.size()) return false;
    row& old = g.rows[i];
    if (!old.user)
    {
        message = text(i18n::str::CHT_ERR_SO_MEUS);
        return false;
    }
    cheat c;
    if (!detail::typed_cheat(old.name, codes, c, message)) return false;
    for (std::size_t k = 0; k < g.rows.size(); ++k)
        if (k != i && g.rows[k].key == c.key)
        {
            message = i18n::trf(i18n::str::CHT_NOTA_DUPLICADO, {old.name});
            return false;
        }
    c.author = old.author;
    c.notes = old.notes;
    c.hash = old.hash;
    c.version = old.version;
    source_file f{old.source, {}, true, false};
    row r = make_row(std::move(c), f);
    // changed codes get a new key and start off; the mode choice stays
    r.wanted = old.wanted;
    r.period_ms = old.period_ms;
    resolve_row(r, g);
    old = std::move(r);
    files.user = files.state = true;
    message = text(i18n::str::CHT_SALVO);
    return true;
}

inline bool edit_rename(game_cheats& g, std::size_t i, std::string_view name, std::string& message, edit_files& files)
{
    if (i >= g.rows.size()) return false;
    row& r = g.rows[i];
    if (!r.user)
    {
        message = text(i18n::str::CHT_ERR_SO_MEUS);
        return false;
    }
    const std::string clean = detail::clip_name(name);
    if (clean.empty())
    {
        message = text(i18n::str::CHT_ERR_NOME_VAZIO);
        return false;
    }
    r.name = clean;
    if (r.pc && !r.code_text.empty()) r.code_text.front() = pc_clipboard(r.pc_info, clean);
    files.user = files.state = true;
    message = text(i18n::str::CHT_SALVO);
    return true;
}

inline bool edit_delete(game_cheats& g, std::size_t i, std::string& message, edit_files& files)
{
    if (i >= g.rows.size()) return false;
    if (!g.rows[i].user)
    {
        message = text(i18n::str::CHT_ERR_SO_MEUS);
        return false;
    }
    message = i18n::trf(i18n::str::CHT_APAGADO, {g.rows[i].name});
    g.rows.erase(g.rows.begin() + static_cast<std::ptrdiff_t>(i));
    files.user = files.state = true;
    return true;
}

// An imported cheat becomes one of the console's own (same codes, same key: its switch and mode stay)
inline bool edit_copy(game_cheats& g, std::size_t i, std::string& message, edit_files& files)
{
    if (i >= g.rows.size()) return false;
    row& r = g.rows[i];
    if (!r.user)
    {
        r.user = true;
        r.source = detail::user_file_name(g);
        if (r.pc) r.code_text = {pc_clipboard(r.pc_info, r.name)};
    }
    files.user = files.state = true;
    message = i18n::trf(i18n::str::CHT_COPIADO, {r.name});
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// Import naming

// Serial at the start of a file name: "BLUS30443.txt", "blus30443_cheats.ncl"
inline std::string serial_from_name(std::string_view name)
{
    if (name.size() < 9 || !detail::serial_shape(name.substr(0, 9))) return {};
    if (name.size() > 9 && detail::alnum(name[9])) return {};
    return detail::upper_ascii(name.substr(0, 9));
}

inline std::string extension_of(std::string_view name)
{
    const std::size_t dot = name.rfind('.');
    return dot == std::string_view::npos ? std::string() : detail::lower_ascii(name.substr(dot));
}

inline bool import_candidate(std::string_view name)
{
    if (name.empty() || name.front() == '.') return false;
    const std::string ext = extension_of(name);
    return ext == ".txt" || ext == ".ncl" || ext == ".yml";
}

// Valid cheats and lines with problems of a parsed file (the import result counts)
inline std::pair<u32, u32> file_counts(const parsed_file& pf)
{
    u32 good = 0, bad = 0;
    for (const cheat& c : pf.cheats)
    {
        good += !c.fatal();
        bad += static_cast<u32>(std::count_if(c.problems.begin(), c.problems.end(), [](const problem& p) { return p.fatal && p.line; }));
    }
    bad += pf.ignored_count;
    return {good, bad};
}
} // namespace utils::ps4::cheats

#ifdef RPCS3_PS4

#include "Utilities/File.h"
#include "Utilities/StrFmt.h"

#include <cerrno>
#include <map>
#include <sys/stat.h>

void ps4_startup_checkpoint(const char* stage);

namespace utils::ps4::cheats
{
inline constexpr char folder[] = "/data/rpcs3/cheats/";
inline constexpr unsigned import_usb_drives = 8; // /mnt/usb0 .. /mnt/usb7

// A title ID usable in a file name: letters and digits only (nothing in it can leave the cheats folder)
inline bool valid_serial(std::string_view s) noexcept
{
    if (s.empty() || s.size() > 16) return false;
    return std::all_of(s.begin(), s.end(), [](char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); });
}

namespace detail
{
// Reads a whole file of at most 'limit' bytes; 'missing' when it does not exist
inline bool read_file(const std::string& path, std::size_t limit, std::string& out, bool& too_large, bool& missing)
{
    out.clear();
    too_large = false;
    missing = false;
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0)
    {
        missing = errno == ENOENT || errno == ENOTDIR;
        return false;
    }
    const fs::file file(path);
    if (!file) return false;
    const u64 size = file.size();
    if (size > limit)
    {
        too_large = true;
        return false;
    }
    out.resize(static_cast<std::size_t>(size));
    return size == 0 || file.read(out.data(), size) == size;
}

// Writes through a temporary file renamed over 'path' (the folder is created first)
inline bool write_file(const std::string& path, std::string_view content)
{
    if (!fs::create_path(folder)) return false;
    fs::pending_file file(path);
    if (!file.file) return false;
    if (!content.empty() && file.file.write(content.data(), content.size()) != content.size()) return false;
    return file.commit();
}

inline std::string error_text() { return fmt::format("%s", fs::g_tls_error); }
} // namespace detail

inline std::string path_for(std::string_view serial, std::string_view suffix)
{
    return std::string(folder) + std::string(serial) + std::string(suffix);
}

// Reads every file of a game ("" title: the PC cheats.yml entries of a single-title file are still taken)
inline game_cheats load_game(std::string serial, std::string title, std::string app_version)
{
    if (!valid_serial(serial)) return build_game(std::move(serial), std::move(title), std::move(app_version), {}, {}, {});
    std::vector<source_file> files;
    std::vector<problem> notes;
    const auto read = [&](std::string_view suffix, bool user, bool yml, std::string* out_text)
    {
        const std::string name = serial + std::string(suffix);
        std::string content;
        bool too_large = false, missing = false;
        if (!detail::read_file(std::string(folder) + name, file_limit, content, too_large, missing))
        {
            if (too_large) notes.push_back({0, i18n::str::CHT_ERR_ARQUIVO_GRANDE, name, std::to_string(file_limit >> 10), false});
            else if (!missing) notes.push_back({0, i18n::str::CHT_ERRO_LER, name, detail::error_text(), false});
            return;
        }
        if (out_text) *out_text = std::move(content);
        else files.push_back({name, std::move(content), user, yml});
    };
    read("_user.txt", true, false, nullptr);
    read(".txt", false, false, nullptr);
    read(".ncl", false, false, nullptr);
    read(".yml", false, true, nullptr);
    std::string state, seen;
    read(".state", false, false, &state);
    read(".seen", false, false, &seen);
    game_cheats g = build_game(std::move(serial), std::move(title), std::move(app_version), files, state, seen);
    g.notes.insert(g.notes.begin(), notes.begin(), notes.end());
    return g;
}

inline bool save_files(const game_cheats& g, const edit_files& files, std::string& message)
{
    if (!valid_serial(g.serial)) return false;
    if (files.user)
    {
        // with no cheat left the file keeps only its header
        if (!detail::write_file(path_for(g.serial, "_user.txt"), user_text(g)))
        {
            message = i18n::trf(i18n::str::CHT_ERRO_GRAVAR, {g.serial + "_user.txt", detail::error_text()});
            return false;
        }
    }
    if (files.state && !detail::write_file(path_for(g.serial, ".state"), state_text(g)))
    {
        message = i18n::trf(i18n::str::CHT_ERRO_GRAVAR, {g.serial + ".state", detail::error_text()});
        return false;
    }
    return true;
}

namespace detail
{
// Runs a model edit; on a write failure the model goes back to what it was
template <typename Edit>
bool edit_and_save(game_cheats& g, std::string& message, Edit&& edit)
{
    game_cheats before = g;
    edit_files files;
    if (!edit(files)) return false;
    if (save_files(g, files, message)) return true;
    g = std::move(before);
    return false;
}
} // namespace detail

inline bool set_on(game_cheats& g, std::size_t i, bool on, std::string& message)
{
    return detail::edit_and_save(g, message, [&](edit_files& f) { return edit_on(g, i, on, message, f); });
}
inline bool set_mode(game_cheats& g, std::size_t i, mode m, u32 period_ms, std::string& message)
{
    return detail::edit_and_save(g, message, [&](edit_files& f) { return edit_mode(g, i, m, period_ms, message, f); });
}
inline bool set_value(game_cheats& g, std::size_t i, std::string_view typed, std::string& message)
{
    return detail::edit_and_save(g, message, [&](edit_files& f) { return edit_value(g, i, typed, message, f); });
}
inline bool add_user(game_cheats& g, std::string_view name, std::string_view codes, std::string& message)
{
    return detail::edit_and_save(g, message, [&](edit_files& f) { return edit_add_user(g, name, codes, message, f); });
}
inline bool edit_user(game_cheats& g, std::size_t i, std::string_view codes, std::string& message)
{
    return detail::edit_and_save(g, message, [&](edit_files& f) { return edit_codes(g, i, codes, message, f); });
}
inline bool rename_user(game_cheats& g, std::size_t i, std::string_view name, std::string& message)
{
    return detail::edit_and_save(g, message, [&](edit_files& f) { return edit_rename(g, i, name, message, f); });
}
inline bool delete_user(game_cheats& g, std::size_t i, std::string& message)
{
    return detail::edit_and_save(g, message, [&](edit_files& f) { return edit_delete(g, i, message, f); });
}
inline bool copy_to_user(game_cheats& g, std::size_t i, std::string& message)
{
    return detail::edit_and_save(g, message, [&](edit_files& f) { return edit_copy(g, i, message, f); });
}

// The engine's record of the executable just booted (only written when it changed)
inline void write_seen(const std::string& serial, seen_line line)
{
    if (!valid_serial(serial)) return;
    const std::string path = path_for(serial, ".seen");
    std::string text;
    bool too_large = false, missing = false;
    detail::read_file(path, 64u << 10, text, too_large, missing);
    seen_info s = parse_seen(text);
    if (!seen_update(s, std::move(line))) return;
    if (!detail::write_file(path, seen_text(s)))
        ps4_startup_checkpoint(fmt::format("cheats: cannot write '%s': %s", path, fs::g_tls_error).c_str());
}

// ---------------------------------------------------------------------------------------------------------------
// Import from USB:/cheats, USB:/rpcs3/cheats (drives 0-7) and /data/rpcs3/import/cheats (the sources stay).
// only_serial: the game whose list asked ("" = every game). A text file belongs to the serial at the start of its
// name or in its "; serial:" / "Title ID:" / header line; in a game's list a file that names no serial is that
// game's. A PC cheats.yml is split by title against the library (exact, then case/punctuation-insensitive). A file
// with no valid cheat is refused; the files of one game and format are joined into /data/rpcs3/cheats/<ID>.txt,
// .ncl or .yml (the old file kept as .bak). Cheats are never turned on by an import.

struct import_report
{
    std::string text, log_line;
    u32 files = 0, cheats = 0, bad_lines = 0;
};

inline import_report import_files(std::string_view only_serial, const std::vector<std::pair<std::string, std::string>>& library_title_serial)
{
    import_report report;
    std::vector<std::string> sources;
    std::string scanned;
    const auto scan = [&](const std::string& dir, const std::string& where)
    {
        struct stat st{};
        if (::stat(dir.c_str(), &st) != 0) return;
        std::vector<std::string> names;
        if (fs::dir d{dir})
            for (const auto& entry : d)
                if (!entry.is_directory && import_candidate(entry.name) && names.size() < 256) names.push_back(entry.name);
        std::sort(names.begin(), names.end());
        scanned += (scanned.empty() ? "" : "; ") + where + " (" + std::to_string(names.size()) + ")";
        for (const std::string& n : names) sources.push_back(dir + n);
    };
    for (unsigned i = 0; i < import_usb_drives; ++i)
    {
        const std::string mount = "/mnt/usb" + std::to_string(i);
        if (!fs::is_dir(mount)) continue;
        scan(mount + "/cheats/", "usb" + std::to_string(i) + ":/cheats");
        scan(mount + "/rpcs3/cheats/", "usb" + std::to_string(i) + ":/rpcs3/cheats");
    }
    scan("/data/rpcs3/import/cheats/", "/data/rpcs3/import/cheats");
    const auto finish = [&]() -> import_report
    {
        report.log_line = "cheats: import result: " + report.text + " | scanned: " + (scanned.empty() ? std::string("nothing") : scanned);
        ps4_startup_checkpoint(report.log_line.c_str());
        return report;
    };
    if (sources.empty())
    {
        report.text = text(i18n::str::CHT_IMPORT_NADA);
        return finish();
    }
    std::string game_title;
    if (!only_serial.empty())
        for (const auto& [title, serial] : library_title_serial)
            if (detail::iequals(serial, only_serial)) game_title = title;
    const auto library_serial = [&](std::string_view title) -> std::string
    {
        for (const auto& [t, s] : library_title_serial)
            if (t == title) return s;
        for (const auto& [t, s] : library_title_serial)
            if (titles_match(t, title)) return s;
        return {};
    };
    const auto library_title = [&](std::string_view serial) -> std::string
    {
        for (const auto& [t, s] : library_title_serial)
            if (detail::iequals(s, serial)) return t;
        return {};
    };

    struct target
    {
        std::string text;                     // joined text files
        std::vector<std::string> yml_parts;   // cheats.yml text per title
    };
    std::map<std::string, target> targets;    // file name in the cheats folder -> content
    std::vector<std::string> refused, no_game, no_serial, not_written;
    const auto add_name = [](std::vector<std::string>& list, std::string name)
    {
        if (std::find(list.begin(), list.end(), name) == list.end()) list.push_back(std::move(name));
    };
    for (const std::string& path : sources)
    {
        const std::string name = path.substr(path.find_last_of('/') + 1);
        std::string content;
        bool too_large = false, missing = false;
        if (!detail::read_file(path, file_limit, content, too_large, missing))
        {
            add_name(refused, name);
            ps4_startup_checkpoint(fmt::format("cheats: import: cannot read '%s' (%s)", path, too_large ? std::string("larger than 1 MiB") : detail::error_text()).c_str());
            continue;
        }
        if (extension_of(name) == ".yml")
        {
            const parsed_file pf = parse_cheats_yml(content, name);
            const auto [good, bad] = file_counts(pf);
            if (!good)
            {
                add_name(refused, name);
                ps4_startup_checkpoint(fmt::format("cheats: import: '%s' has no valid cheats.yml entry", path).c_str());
                continue;
            }
            std::vector<std::string> titles;
            for (const cheat& c : pf.cheats)
                if (std::find(titles.begin(), titles.end(), c.pc_info.game) == titles.end()) titles.push_back(c.pc_info.game);
            for (const std::string& title : titles)
            {
                std::string serial;
                if (!only_serial.empty())
                {
                    if (titles_match(title, game_title) || titles.size() == 1) serial = std::string(only_serial);
                }
                else serial = library_serial(title);
                if (serial.empty())
                {
                    if (only_serial.empty()) add_name(no_game, title);
                    continue;
                }
                if (!valid_serial(serial)) continue;
                serial = detail::upper_ascii(serial);
                std::vector<const cheat*> entries;
                u32 taken = 0;
                for (const cheat& c : pf.cheats)
                    if (c.pc_info.game == title && !c.fatal()) entries.push_back(&c), ++taken;
                // keyed by the library title, so the boot (Emu.GetTitle()) finds them
                const std::string key = library_title(serial);
                targets[serial + ".yml"].yml_parts.push_back(cheats_yml_text(key.empty() ? title : key, entries));
                report.cheats += taken;
            }
            report.bad_lines += bad;
            continue;
        }
        const parsed_file pf = parse_text(content, name);
        const auto [good, bad] = file_counts(pf);
        if (!good)
        {
            add_name(refused, name);
            ps4_startup_checkpoint(fmt::format("cheats: import: '%s' has no valid cheat", path).c_str());
            continue;
        }
        std::string serial = serial_from_name(name);
        if (serial.empty()) serial = pf.serial;
        if (!only_serial.empty())
        {
            if (!serial.empty() && !detail::iequals(serial, only_serial)) continue;
            serial = detail::upper_ascii(only_serial);
        }
        if (serial.empty() || !valid_serial(serial))
        {
            add_name(no_serial, name);
            continue;
        }
        target& t = targets[serial + extension_of(name)];
        t.text += (t.text.empty() ? "" : "\n") + std::string("; file: ") + name + "\n" + content;
        if (!t.text.empty() && t.text.back() != '\n') t.text += '\n';
        report.cheats += good;
        report.bad_lines += bad;
    }

    u32 unchanged = 0;
    for (const auto& [file, t] : targets)
    {
        std::string text = t.text;
        for (const std::string& part : t.yml_parts) text += part;
        const std::string path = std::string(folder) + file;
        std::string current;
        bool too_large = false, missing = false;
        const bool read = detail::read_file(path, 4 * file_limit, current, too_large, missing);
        const bool exists = read || !missing;
        if (read && current == text)
        {
            ++unchanged;
            continue;
        }
        if (exists && !fs::copy_file(path, path + ".bak", true))
        {
            add_name(not_written, file);
            ps4_startup_checkpoint(fmt::format("cheats: import: cannot back up '%s': %s", path, fs::g_tls_error).c_str());
            continue;
        }
        if (!detail::write_file(path, text))
        {
            add_name(not_written, file);
            ps4_startup_checkpoint(fmt::format("cheats: import: cannot write '%s': %s", path, fs::g_tls_error).c_str());
            continue;
        }
        ++report.files;
        ps4_startup_checkpoint(fmt::format("cheats: import: wrote '%s' (%u bytes)", path, text.size()).c_str());
    }
    const auto names = [](const std::vector<std::string>& list)
    {
        std::string out;
        for (std::size_t i = 0; i < list.size() && i < 3; ++i) out += (i ? ", " : "") + list[i];
        if (list.size() > 3) out += ", ...";
        return out;
    };
    const bool clean = refused.empty() && no_game.empty() && no_serial.empty() && not_written.empty();
    if (!report.files && unchanged && clean)
        report.text = text(i18n::str::CHT_IMPORT_IGUAL);
    else if (targets.empty() && clean)
        report.text = text(i18n::str::CHT_IMPORT_NADA); // files of other games only
    else if (report.bad_lines)
        report.text = i18n::trf(i18n::str::CHT_IMPORT_RESULTADO, {report.files, report.cheats, report.bad_lines});
    else
        report.text = i18n::trf(i18n::str::CHT_IMPORT_RESULTADO_OK, {report.files, report.cheats});
    if (!refused.empty()) report.text += i18n::trf(i18n::str::CHT_IMPORT_RECUSADO, {names(refused)});
    if (!no_game.empty()) report.text += i18n::trf(i18n::str::CHT_IMPORT_SEM_JOGO, {names(no_game)});
    if (!no_serial.empty()) report.text += i18n::trf(i18n::str::CHT_IMPORT_SEM_SERIAL, {names(no_serial)});
    if (!not_written.empty()) report.text += i18n::trf(i18n::str::CHT_IMPORT_ERRO_GRAVAR, {names(not_written)});
    return finish();
}
} // namespace utils::ps4::cheats

#endif // RPCS3_PS4
