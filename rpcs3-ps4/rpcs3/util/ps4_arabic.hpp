#pragma once

// Arabic text for the XMB font: contextual shaping into the Presentation Forms-B (U+FE70..U+FEFC, lam-alef
// ligatures included) and the bidirectional reordering of one line (UAX #9 subset without embeddings or isolates:
// paragraph direction given or from the first strong letter (P2), weak types W4-W7, paired brackets N0, neutrals
// N1/N2, levels I1/I2, L1, L2, mirroring L4; LRM/RLM/ALM are strong marks that are dropped after reordering). Pure; the
// XMB font calls visual_utf8() on every line that contains Arabic or a bidi mark.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace utils::ps4::arabic
{
inline bool is_arabic_letter(char32_t c)
{
    return (c >= 0x0600 && c <= 0x06ff && !(c >= 0x0660 && c <= 0x0669) && !(c >= 0x06f0 && c <= 0x06f9) && !(c >= 0x064b && c <= 0x065f) && c != 0x0670) ||
        (c >= 0xfb50 && c <= 0xfdff) || (c >= 0xfe70 && c <= 0xfefc);
}

// Harakat and other combining marks of the Arabic block (dropped: the XMB font has no mark positioning)
inline bool is_arabic_mark(char32_t c)
{
    return (c >= 0x064b && c <= 0x065f) || c == 0x0670 || (c >= 0x06d6 && c <= 0x06ed);
}

inline std::u32string decode(std::string_view text)
{
    std::u32string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();)
    {
        const unsigned char c = static_cast<unsigned char>(text[i++]);
        if (c < 0x80) { out.push_back(c); continue; }
        const unsigned count = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc2 ? 1 : 0;
        if (!count || i + count > text.size()) { out.push_back(U'?'); continue; }
        char32_t value = c & ((1u << (6 - count)) - 1);
        bool ok = true;
        for (unsigned k = 0; k < count; ++k)
        {
            const unsigned char next = static_cast<unsigned char>(text[i]);
            if ((next & 0xc0) != 0x80) { ok = false; break; }
            ++i;
            value = (value << 6) | (next & 0x3f);
        }
        out.push_back(ok ? value : U'?');
    }
    return out;
}

inline std::string encode(std::u32string_view text)
{
    std::string out;
    out.reserve(text.size() * 2);
    for (const char32_t c : text)
    {
        if (c < 0x80) out.push_back(char(c));
        else if (c < 0x800) { out.push_back(char(0xc0 | (c >> 6))); out.push_back(char(0x80 | (c & 0x3f))); }
        else if (c < 0x10000) { out.push_back(char(0xe0 | (c >> 12))); out.push_back(char(0x80 | ((c >> 6) & 0x3f))); out.push_back(char(0x80 | (c & 0x3f))); }
        else
        {
            out.push_back(char(0xf0 | (c >> 18))); out.push_back(char(0x80 | ((c >> 12) & 0x3f)));
            out.push_back(char(0x80 | ((c >> 6) & 0x3f))); out.push_back(char(0x80 | (c & 0x3f)));
        }
    }
    return out;
}

// Any Arabic letter (UTF-8 lead bytes 0xD8..0xDB, or a presentation form 0xEF 0xAD..0xBB)
inline bool has_arabic(std::string_view text)
{
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c >= 0xd8 && c <= 0xdb) return true;
        if (c == 0xef && i + 1 < text.size() && static_cast<unsigned char>(text[i + 1]) >= 0xad && static_cast<unsigned char>(text[i + 1]) <= 0xbb) return true;
    }
    return false;
}

// U+200E LRM or U+200F RLM (UTF-8 E2 80 8E / 8F); U+061C ALM counts as Arabic (has_arabic)
inline bool has_bidi_mark(std::string_view text)
{
    for (std::size_t i = 0; i + 2 < text.size(); ++i)
        if (static_cast<unsigned char>(text[i]) == 0xe2 && static_cast<unsigned char>(text[i + 1]) == 0x80 &&
            (static_cast<unsigned char>(text[i + 2]) == 0x8e || static_cast<unsigned char>(text[i + 2]) == 0x8f))
            return true;
    return false;
}

inline bool is_bidi_mark(char32_t c)
{
    return c == 0x200e || c == 0x200f || c == 0x061c;
}

namespace detail
{
// U+0621..U+064A: first presentation form (isolated) and the number of forms (1 none, 2 isolated/final,
// 4 isolated/final/initial/medial). 0 = not shaped.
struct letter { std::uint16_t form; std::uint8_t count; };
inline constexpr std::array<letter, 0x064a - 0x0621 + 1> letters{{
    {0xfe80, 1}, {0xfe81, 2}, {0xfe83, 2}, {0xfe85, 2}, {0xfe87, 2}, {0xfe89, 4}, {0xfe8d, 2}, {0xfe8f, 4}, // 0621..0628
    {0xfe93, 2}, {0xfe95, 4}, {0xfe99, 4}, {0xfe9d, 4}, {0xfea1, 4}, {0xfea5, 4}, {0xfea9, 2}, {0xfeab, 2}, // 0629..0630
    {0xfead, 2}, {0xfeaf, 2}, {0xfeb1, 4}, {0xfeb5, 4}, {0xfeb9, 4}, {0xfebd, 4}, {0xfec1, 4}, {0xfec5, 4}, // 0631..0638
    {0xfec9, 4}, {0xfecd, 4}, {0, 0}, {0, 0}, {0, 0}, {0, 0}, {0, 0},                                      // 0639..063F
    {0, 0},                                                                                                 // 0640 tatweel
    {0xfed1, 4}, {0xfed5, 4}, {0xfed9, 4}, {0xfedd, 4}, {0xfee1, 4}, {0xfee5, 4}, {0xfee9, 4}, {0xfeed, 2}, // 0641..0648
    {0xfeef, 2}, {0xfef1, 4},                                                                               // 0649..064A
}};

inline const letter* find(char32_t c)
{
    if (c < 0x0621 || c > 0x064a) return nullptr;
    const letter& l = letters[c - 0x0621];
    return l.count ? &l : nullptr;
}

// Joins to the following letter (dual-joining letters and the tatweel)
inline bool joins_forward(char32_t c)
{
    if (c == 0x0640) return true;
    const letter* l = find(c);
    return l && l->count == 4;
}

// Accepts a join from the preceding letter (dual- and right-joining letters, tatweel)
inline bool joins_back(char32_t c)
{
    if (c == 0x0640) return true;
    const letter* l = find(c);
    return l && l->count >= 2;
}

inline char32_t lam_alef(char32_t alef)
{
    switch (alef)
    {
    case 0x0622: return 0xfef5;
    case 0x0623: return 0xfef7;
    case 0x0625: return 0xfef9;
    case 0x0627: return 0xfefb;
    default: return 0;
    }
}

enum class bidi : std::uint8_t { L, R, EN, AN, ES, ET, CS, WS, ON };

inline bidi classify(char32_t c)
{
    if (c == 0x200e) return bidi::L; // LRM
    if (c == 0x200f || c == 0x061c) return bidi::R; // RLM, ALM
    if (c >= '0' && c <= '9') return bidi::EN;
    if ((c >= 0x06f0 && c <= 0x06f9) || c == 0xb2 || c == 0xb3 || c == 0xb9) return bidi::EN;
    if ((c >= 0x0660 && c <= 0x0669) || c == 0x066b || c == 0x066c) return bidi::AN;
    if (c == 0x060c) return bidi::CS;
    if (is_arabic_letter(c) || (c >= 0x0590 && c <= 0x05ff)) return bidi::R;
    if (c == ' ' || c == '\t') return bidi::WS;
    if (c == '+' || c == '-') return bidi::ES;
    if (c == '#' || c == '$' || c == '%' || c == 0xb0 || c == 0xb1 || (c >= 0xa2 && c <= 0xa5) || c == 0x2030 || c == 0x066a) return bidi::ET;
    if (c == ',' || c == '.' || c == '/' || c == ':' || c == 0xa0) return bidi::CS;
    if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == 0xaa || c == 0xb5 || c == 0xba) return bidi::L;
    if (c >= 0xc0 && c <= 0x24f && c != 0xd7 && c != 0xf7) return bidi::L;
    if (c >= 0x0370 && c <= 0x058f) return bidi::L; // Greek, Cyrillic, Armenian
    return bidi::ON;
}

inline char32_t mirror(char32_t c)
{
    switch (c)
    {
    case '(': return ')';
    case ')': return '(';
    case '[': return ']';
    case ']': return '[';
    case '{': return '}';
    case '}': return '{';
    case '<': return '>';
    case '>': return '<';
    case 0xab: return 0xbb;
    case 0xbb: return 0xab;
    case 0x2039: return 0x203a;
    case 0x203a: return 0x2039;
    default: return c;
    }
}
} // namespace detail

// Arabic letters (U+0621..U+064A) in logical order to their contextual presentation forms; lam + alef becomes
// one ligature; harakat are dropped. Other code points pass unchanged.
inline std::u32string shape(std::u32string_view text)
{
    using namespace detail;
    std::u32string in;
    in.reserve(text.size());
    for (const char32_t c : text) if (!is_arabic_mark(c)) in.push_back(c);
    std::u32string out;
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i)
    {
        const char32_t c = in[i];
        const bool prev_joins = i > 0 && joins_forward(in[i - 1]);
        if (c == 0x0644 && i + 1 < in.size())
            if (const char32_t ligature = lam_alef(in[i + 1]))
            {
                out.push_back(prev_joins ? ligature + 1 : ligature); // isolated or final form
                ++i;
                continue;
            }
        const letter* l = find(c);
        if (!l) { out.push_back(c); continue; }
        const bool back = prev_joins && l->count >= 2;
        const bool forward = l->count == 4 && i + 1 < in.size() && joins_back(in[i + 1]);
        unsigned form = 0;                    // isolated
        if (back && forward) form = 3;        // medial
        else if (back) form = 1;              // final
        else if (forward) form = 2;           // initial
        out.push_back(char32_t(l->form + form));
    }
    return out;
}

// Paragraph direction of a line: automatic = its first strong letter (UAX #9 P2; none: left to right)
enum class paragraph : std::uint8_t { automatic, ltr, rtl };

namespace detail
{
// The opening bracket a closing one pairs with (0: not a closing bracket)
inline char32_t opening_of(char32_t c)
{
    switch (c)
    {
    case ')': return '(';
    case ']': return '[';
    case '}': return '{';
    default: return 0;
    }
}

inline bool is_opening(char32_t c)
{
    return c == '(' || c == '[' || c == '{';
}
} // namespace detail

// One line in logical order to its visual (left-to-right) order with the given paragraph direction. Runs of Latin
// text and numbers keep their left-to-right order inside a right-to-left paragraph and Arabic runs read right to left
// inside a left-to-right one; a bracket pair takes the direction of its content (N0), mirrored characters are swapped
// on right-to-left levels, and LRM/RLM/ALM are removed from the result.
inline std::u32string reorder(std::u32string_view text, paragraph direction)
{
    using namespace detail;
    const std::size_t n = text.size();
    std::vector<bidi> type(n, bidi::ON);
    bool any_r = false, any_mark = false;
    bidi first_strong = bidi::ON;
    for (std::size_t i = 0; i < n; ++i)
    {
        type[i] = classify(text[i]);
        any_r |= type[i] == bidi::R;
        any_mark |= is_bidi_mark(text[i]);
        if (first_strong == bidi::ON && (type[i] == bidi::L || type[i] == bidi::R)) first_strong = type[i];
    }
    const bool rtl = direction == paragraph::rtl || (direction == paragraph::automatic && first_strong == bidi::R);
    const bidi base = rtl ? bidi::R : bidi::L;
    const auto strip = [&](std::u32string s)
    {
        if (any_mark) std::erase_if(s, [](char32_t c) { return is_bidi_mark(c); });
        return s;
    };
    // A left-to-right paragraph without any right-to-left character keeps its order
    if (!rtl && !any_r) return strip(std::u32string(text));

    // W4: one separator between two numbers of the same kind joins them (1.5, 10:30, 2/4)
    for (std::size_t i = 1; i + 1 < n; ++i)
    {
        if (type[i] == bidi::ES && type[i - 1] == bidi::EN && type[i + 1] == bidi::EN) type[i] = bidi::EN;
        else if (type[i] == bidi::CS && type[i - 1] == type[i + 1] && (type[i - 1] == bidi::EN || type[i - 1] == bidi::AN)) type[i] = type[i - 1];
    }
    // W5: terminators next to a European number belong to it (50%, $3)
    for (std::size_t i = 0; i < n; ++i)
    {
        if (type[i] != bidi::ET) continue;
        std::size_t end = i;
        while (end < n && type[end] == bidi::ET) ++end;
        if ((i > 0 && type[i - 1] == bidi::EN) || (end < n && type[end] == bidi::EN))
            for (std::size_t k = i; k < end; ++k) type[k] = bidi::EN;
        i = end - 1;
    }
    // W6: the remaining separators and terminators are neutral
    for (auto& t : type) if (t == bidi::ES || t == bidi::ET || t == bidi::CS) t = bidi::ON;
    // W7: a European number after Latin text (or at the start of a left-to-right paragraph) is Latin
    bidi last_strong = base;
    for (std::size_t i = 0; i < n; ++i)
    {
        if (type[i] == bidi::L || type[i] == bidi::R) last_strong = type[i];
        else if (type[i] == bidi::EN && last_strong == bidi::L) type[i] = bidi::L;
    }
    // Numbers count as right-to-left for the neutral rules
    const auto strong = [](bidi t) { return t == bidi::L ? bidi::L : bidi::R; };
    const auto is_strong = [](bidi t) { return t == bidi::L || t == bidi::R || t == bidi::EN || t == bidi::AN; };
    // N0: bracket pairs (BD16, at most 63 open brackets), in the order of their opening brackets
    {
        struct pair_at { std::size_t open, close; };
        std::vector<pair_at> pairs;
        std::vector<std::size_t> stack;
        for (std::size_t i = 0; i < n; ++i)
        {
            if (type[i] != bidi::ON) continue;
            if (is_opening(text[i]))
            {
                if (stack.size() == 63) break;
                stack.push_back(i);
            }
            else if (const char32_t open = opening_of(text[i]))
            {
                for (std::size_t k = stack.size(); k > 0; --k)
                {
                    if (text[stack[k - 1]] != open) continue;
                    pairs.push_back({stack[k - 1], i});
                    stack.resize(k - 1);
                    break;
                }
            }
        }
        std::sort(pairs.begin(), pairs.end(), [](const pair_at& a, const pair_at& b) { return a.open < b.open; });
        for (const pair_at& p : pairs)
        {
            bool same = false, opposite = false;
            for (std::size_t k = p.open + 1; k < p.close; ++k)
            {
                if (!is_strong(type[k])) continue;
                (strong(type[k]) == base ? same : opposite) = true;
            }
            bidi resolved = bidi::ON;
            if (same) resolved = base;
            else if (opposite)
            {
                bidi before = base;
                for (std::size_t k = p.open; k > 0; --k)
                    if (is_strong(type[k - 1])) { before = strong(type[k - 1]); break; }
                resolved = before != base ? before : base;
            }
            if (resolved != bidi::ON) type[p.open] = type[p.close] = resolved;
        }
    }
    // N1/N2: neutrals between two runs of the same direction take it, the others the paragraph direction
    for (std::size_t i = 0; i < n; ++i)
    {
        if (type[i] != bidi::ON && type[i] != bidi::WS) continue;
        std::size_t end = i;
        while (end < n && (type[end] == bidi::ON || type[end] == bidi::WS)) ++end;
        const bidi before = i > 0 ? strong(type[i - 1]) : base;
        const bidi after = end < n ? strong(type[end]) : base;
        const bidi resolved = before == after ? before : base;
        for (std::size_t k = i; k < end; ++k) type[k] = resolved;
        i = end - 1;
    }
    // I1/I2: paragraph level 0 (L 0, R 1, numbers 2) or 1 (R 1, L and numbers 2)
    const std::uint8_t para = rtl ? 1 : 0;
    std::vector<std::uint8_t> level(n, para);
    for (std::size_t i = 0; i < n; ++i)
    {
        const bool number = type[i] == bidi::EN || type[i] == bidi::AN;
        if (rtl) level[i] = type[i] == bidi::R ? 1 : 2;
        else level[i] = type[i] == bidi::L ? 0 : number ? 2 : 1;
    }
    // L1: trailing whitespace takes the paragraph level
    for (std::size_t i = n; i > 0 && (text[i - 1] == ' ' || text[i - 1] == '\t'); --i) level[i - 1] = para;
    // L4 before L2 (positions are still logical): mirrored glyphs on the right-to-left (odd) levels
    std::u32string out(text);
    for (std::size_t i = 0; i < n; ++i) if (level[i] & 1) out[i] = mirror(out[i]);
    // L2: from the highest level down to the lowest odd one, reverse every run at that level or above
    const std::uint8_t highest = n ? *std::max_element(level.begin(), level.end()) : 0;
    for (std::uint8_t l = highest; l >= 1; --l)
    {
        for (std::size_t i = 0; i < n; ++i)
        {
            if (level[i] < l) continue;
            std::size_t end = i;
            while (end < n && level[end] >= l) ++end;
            std::reverse(out.begin() + std::ptrdiff_t(i), out.begin() + std::ptrdiff_t(end));
            std::reverse(level.begin() + std::ptrdiff_t(i), level.begin() + std::ptrdiff_t(end));
            i = end - 1;
        }
    }
    return strip(std::move(out));
}

// 0.38 form: a right-to-left paragraph when the line holds a right-to-left character, else the text as it is
inline std::u32string reorder(std::u32string_view text)
{
    for (const char32_t c : text)
        if (detail::classify(c) == detail::bidi::R) return reorder(text, paragraph::rtl);
    return std::u32string(text);
}

// In a line with Arabic, each run of non-Arabic text between spaces that holds a Latin letter (a path, a file name, a
// word of a title) is isolated with LRM, without its leading brackets and quotes and its trailing punctuation: the
// neutral characters at its edges (the '/' of "/data/...", a '-', a '.' inside "v1.05") then stay with it instead of
// joining the Arabic text around it. Table texts carry paths without the LRM that trf() puts around arguments.
inline std::u32string isolate_latin_words(std::u32string_view text)
{
    using namespace detail;
    const auto space = [](char32_t c) { return c == ' ' || c == '\t' || c == 0xa0; };
    const auto leading = [](char32_t c) { return c == '(' || c == '[' || c == '{' || c == '"' || c == '\'' || c == 0xab; };
    const auto trailing = [](char32_t c)
    {
        return c == '.' || c == ',' || c == ':' || c == ';' || c == '!' || c == '?' || c == ')' || c == ']' || c == '}' || c == '"' ||
            c == '\'' || c == 0xbb || c == 0x2026 || c == 0x060c || c == 0x061b || c == 0x061f;
    };
    std::u32string out;
    out.reserve(text.size() + 16);
    std::size_t i = 0;
    while (i < text.size())
    {
        if (space(text[i]) || is_arabic_letter(text[i]))
        {
            out.push_back(text[i++]);
            continue;
        }
        std::size_t end = i;
        bool letter = false;
        while (end < text.size() && !space(text[end]) && !is_arabic_letter(text[end]))
        {
            letter |= classify(text[end]) == bidi::L && !is_bidi_mark(text[end]);
            ++end;
        }
        std::size_t a = i, b = end;
        while (a < b && leading(text[a])) ++a;
        while (b > a && trailing(text[b - 1])) --b;
        if (!letter || a == b)
        {
            out.append(text.substr(i, end - i));
            i = end;
            continue;
        }
        out.append(text.substr(i, a - i));
        out.push_back(0x200e);
        out.append(text.substr(a, b - a));
        out.push_back(0x200e);
        out.append(text.substr(b, end - b));
        i = end;
    }
    return out;
}

// Shaping and reordering of one UTF-8 line. A line without an Arabic letter keeps its order in every interface
// language (game titles, paths, Latin text); only its bidi marks are removed. A line with Arabic is a right-to-left
// paragraph in a right-to-left interface, else its direction follows its first strong letter.
inline std::string visual_utf8(std::string_view text, bool ui_rtl)
{
    if (!has_arabic(text))
    {
        if (!has_bidi_mark(text)) return std::string(text);
        std::u32string chars = decode(text);
        std::erase_if(chars, [](char32_t c) { return is_bidi_mark(c); });
        return encode(chars);
    }
    return encode(reorder(isolate_latin_words(shape(decode(text))), ui_rtl ? paragraph::rtl : paragraph::automatic));
}

// 0.38 form: a line with Arabic is always a right-to-left paragraph
inline std::string visual_utf8(std::string_view text)
{
    return visual_utf8(text, true);
}
} // namespace utils::ps4::arabic
