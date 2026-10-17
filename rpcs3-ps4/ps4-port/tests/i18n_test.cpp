// Host test of the interface languages (rpcs3/util/ps4_i18n.hpp, ps4_i18n_*.inc, ps4_xmb_notice.hpp):
//  1 every id has pt-BR text; en/es/ar filled for the areas already translated (all areas with --release)
//  2 placeholders {0}..{9} identical in every language, no other braces, no printf specifiers
//  3 the " · " separators (dialog rows) keep their count
//  4 paths, file names and tokens (PS3, PPU, SPU, X, GoldHEN, ...) byte-identical; fragment edges (spaces) kept
//  5 valid UTF-8 and every glyph drawable by the XMB atlas (Latin-1, 6 symbols, the 129 Arabic forms after shaping);
//    no Persian letters or Arabic-Indic digits
//  6 Arabic shaping and bidi vectors (paragraph direction, bracket pairs, LRM/RLM/ALM, numbers, ellipsis)
//  7 string widths with the XMB font (DejaVu Sans, Latin and Arabic; --font <ttf> for the Latin glyphs, e.g. the PS3
//    firmware's SCE-PS3-RD-R-LATIN.TTF): footer hints, button labels, categories, row subtitles; long lines warned
//  8 ui-language.txt parser, PS4 language -> interface language, PS4 language -> game language, apply()
//  9 trf/trs: reordering, missing/extra arguments, integers, translated-id arguments, LRM isolation only when the Arabic
//    column served the template, UTF-8-safe truncation for every buffer size, NUL always written
// 10 fallback empty -> English -> pt-BR
// 11 XMB notice encode/decode round trip, plain text passthrough, malformed input
// 12 (the other host tests, run on their own: policy_test, xmb_preview, ... keep their pt-BR texts by default)
// 13 settings browser (ps4_i18n_cfg.inc, area "cfg" = every CFG*_ id but the tooltips, area "cfg-tooltips" = CFGTIP_*,
//    both complete in 4 languages; an English-only area may leave pt-BR empty): the English labels, value labels and tooltips are the PC dialog's
//    texts (tests/cfg-mocks/cfg_pc_text.inc, written by ps4-port/tools/gen_cfg_catalog.py), and every catalog row has its
//    texts; group 7 checks the browser slots (labels 560 px and values 360 px at 21 px, help lines at 17 px)
// 14 cheats (ps4_i18n_cheats.inc, area "CHT", complete in 4 languages): a problem text that takes the source line starts
//    with "Linha {0}" / "Line {0}" / "Línea {0}" / "السطر {0}" (cheats::problem_text); group 7 checks the list hint
//    in the footer help slot and the other texts as message lines
// Build and run from the repo root (llvm-mingw, as the other host tests):
//   clang++ -std=c++20 -O2 -static -Wall -Wextra -I rpcs3 -I 3rdparty/stblib/stb ps4-port/tests/i18n_test.cpp -o i18n_test.exe
//   i18n_test.exe [--release] [--verbose] [--font <ttf>]
// (without -I 3rdparty/stblib/stb group 7 is skipped; the font is read from bin/fonts/DejaVuSans.ttf next to the source)
#define PS4_I18N_ID_NAMES 1
#include "../../rpcs3/util/ps4_i18n.hpp"
#include "../../rpcs3/util/ps4_xmb_notice.hpp"
#include "../../rpcs3/util/ps4_cfg_catalog.hpp"

#include <array>
#include <climits>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

// Group 7 needs the XMB font (stb_truetype): built when 3rdparty/stblib/stb is on the include path
#if __has_include(<stb_truetype.h>)
#define I18N_TEST_WIDTHS 1
#define STB_TRUETYPE_IMPLEMENTATION // ps4_xmb.hpp includes stb_truetype.h once
#include "../../rpcs3/util/ps4_xmb.hpp"
#endif

namespace
{
namespace i18n = utils::ps4::i18n;
namespace notice = utils::ps4::xmb_notice;
namespace arabic = utils::ps4::arabic;
using i18n::lang;
using i18n::setting;
using i18n::str;

int g_failures = 0;
int g_checks = 0;
int g_warnings = 0;
bool g_release = false;
bool g_verbose = false;

#define CHECK(cond)                                                                     \
    do                                                                                  \
    {                                                                                   \
        ++g_checks;                                                                     \
        if (!(cond))                                                                    \
        {                                                                               \
            ++g_failures;                                                               \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                 \
        }                                                                               \
    } while (0)

// A table rule broken by one row: counted and printed with the id and the language
void row_fail(std::size_t id, int column, const std::string& what)
{
    ++g_checks;
    ++g_failures;
    static constexpr const char* names[] = {"pt", "en", "es", "ar"};
    std::printf("FAIL %s [%s]: %s\n", i18n::detail::names[id], column >= 0 ? names[column] : "-", what.c_str());
}

void row_warn(std::size_t id, int column, const std::string& what)
{
    ++g_warnings;
    if (!g_verbose) return;
    static constexpr const char* names[] = {"pt", "en", "es", "ar"};
    std::printf("warn %s [%s]: %s\n", i18n::detail::names[id], column >= 0 ? names[column] : "-", what.c_str());
}

std::string area_of(std::size_t id)
{
    const std::string_view name = i18n::detail::names[id];
    // The settings browser: CFGT_, CFGG_, CFGL_, CFGH_, CFGV_, CFGR_, CFGU_ form one area, the PC tooltips another
    if (name.starts_with("CFGTIP_")) return "cfg-tooltips";
    if (name.starts_with("CFG")) return "cfg";
    return std::string(name.substr(0, name.find('_')));
}

// Areas whose en/es/ar columns are complete; the others are counted until --release requires them
const std::set<std::string> g_translated_areas = {"COM", "LANG", "XMB", "DLG", "SET", "POL", "GL", "cfg", "cfg-tooltips", "CHT"};
// Areas that may stay English-only (empty pt-BR, es and ar columns fall back to English), even with --release; none
// today (the PC tooltips of the settings browser are translated)
const std::set<std::string> g_english_only_areas = {};

std::optional<std::u32string> decode_strict(std::string_view text)
{
    std::u32string out;
    for (std::size_t i = 0; i < text.size();)
    {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        if (c < 0x80) { out.push_back(c); ++i; continue; }
        const unsigned count = c >= 0xf0 && c <= 0xf4 ? 3 : c >= 0xe0 ? (c <= 0xef ? 2 : 0) : c >= 0xc2 ? 1 : 0;
        if (!count || i + count >= text.size()) return std::nullopt;
        char32_t value = c & ((1u << (6 - count)) - 1);
        for (unsigned k = 1; k <= count; ++k)
        {
            const unsigned char next = static_cast<unsigned char>(text[i + k]);
            if ((next & 0xc0) != 0x80) return std::nullopt;
            value = (value << 6) | (next & 0x3f);
        }
        if ((count == 2 && value < 0x800) || (count == 3 && value < 0x10000) || (value >= 0xd800 && value <= 0xdfff)) return std::nullopt;
        out.push_back(value);
        i += count + 1;
    }
    return out;
}

bool drawable(char32_t c)
{
    if (c >= 0x20 && c <= 0x7e) return true;
    if (c >= 0xa0 && c <= 0xff) return true;
    switch (c)
    {
    case 0x2122: case 0x2013: case 0x2014: case 0x2026: case 0x25b3: case 0x25cb: // ™ – — … △ ○
    case 0x060c: case 0x061b: case 0x061f: case 0x0640:                           // ، ؛ ؟ ـ
        return true;
    default: break;
    }
    return c >= 0xfe80 && c <= 0xfefc;
}

std::size_t count_of(std::string_view text, std::string_view what)
{
    std::size_t n = 0;
    for (std::size_t at = text.find(what); at != std::string_view::npos; at = text.find(what, at + what.size())) ++n;
    return n;
}

bool ascii_alnum(char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }

bool has_word(std::string_view text, std::string_view word)
{
    for (std::size_t at = text.find(word); at != std::string_view::npos; at = text.find(word, at + 1))
    {
        const bool left = at == 0 || !ascii_alnum(text[at - 1]);
        const bool right = at + word.size() >= text.size() || !ascii_alnum(text[at + word.size()]);
        if (left && right) return true;
    }
    return false;
}

// Placeholders of one text; false on a stray brace
bool placeholders(std::string_view text, std::set<int>& out, std::string& error)
{
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] == '{')
        {
            if (i + 2 < text.size() && text[i + 1] >= '0' && text[i + 1] <= '9' && text[i + 2] == '}')
            {
                out.insert(text[i + 1] - '0');
                i += 2;
                continue;
            }
            error = "stray '{' at byte " + std::to_string(i);
            return false;
        }
        if (text[i] == '}')
        {
            error = "stray '}' at byte " + std::to_string(i);
            return false;
        }
    }
    return true;
}

// A printf conversion ("%s", "%u", "%.1f", "%llu", "%%"); "45%" or "(%)" are plain text
bool has_printf(std::string_view text)
{
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] != '%') continue;
        std::size_t k = i + 1;
        while (k < text.size() && (text[k] == '-' || text[k] == '+' || text[k] == '#' || text[k] == '0')) ++k;
        while (k < text.size() && text[k] >= '0' && text[k] <= '9') ++k;
        if (k < text.size() && text[k] == '.')
        {
            ++k;
            while (k < text.size() && text[k] >= '0' && text[k] <= '9') ++k;
        }
        while (k < text.size() && (text[k] == 'h' || text[k] == 'l' || text[k] == 'z' || text[k] == 'j' || text[k] == 't' || text[k] == 'L')) ++k;
        if (k < text.size() && std::string_view("diouxXeEfFgGaAcspn%").find(text[k]) != std::string_view::npos) return true;
    }
    return false;
}

// Paths (/data/..., /app0/..., USB:/...) and file names with a known extension, as they appear in pt-BR
std::vector<std::string> path_tokens(std::string_view text)
{
    std::vector<std::string> out;
    const auto trim = [](std::string_view t)
    {
        while (!t.empty() && std::string_view(".,;:)?!'\"").find(t.back()) != std::string_view::npos) t.remove_suffix(1);
        return t;
    };
    std::size_t i = 0;
    while (i < text.size())
    {
        while (i < text.size() && (text[i] == ' ' || text[i] == '(' || text[i] == '\'' || text[i] == '"')) ++i;
        std::size_t end = i;
        while (end < text.size() && text[end] != ' ' && text[end] != '(' && text[end] != '\'' && text[end] != '"') ++end;
        const std::string_view word = trim(text.substr(i, end - i));
        if (word.starts_with("/data") || word.starts_with("/app0") || word.starts_with("/mnt") || word.starts_with("USB:/"))
            out.emplace_back(word);
        i = end;
    }
    // file names: runs of [A-Za-z0-9_.-*<>] whose last extension is known
    static const std::set<std::string> extensions = {"dkey", "ird", "rap", "yml", "log", "flag", "txt", "dat", "sfo", "sfb", "bin", "pup", "pkg", "iso", "json", "ttf"};
    const auto name_char = [](char c) { return ascii_alnum(c) || c == '_' || c == '.' || c == '-' || c == '*' || c == '<' || c == '>'; };
    for (std::size_t k = 0; k < text.size();)
    {
        if (!name_char(text[k])) { ++k; continue; }
        std::size_t end = k;
        while (end < text.size() && name_char(text[end])) ++end;
        std::string_view run = text.substr(k, end - k);
        while (!run.empty() && (run.back() == '.' || run.back() == '-')) run.remove_suffix(1);
        if (const std::size_t dot = run.rfind('.'); dot != std::string_view::npos && dot + 1 < run.size())
        {
            std::string ext(run.substr(dot + 1));
            for (char& c : ext) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
            if (extensions.contains(ext)) out.emplace_back(run);
        }
        k = end;
    }
    return out;
}

const std::vector<std::string_view> g_word_tokens = {"PPU", "SPU", "X", "R1", "L1", "PS", "GoldHEN", "RPCN", "FTP", "USB", "PS3", "PS4", "DLC",
    "PKG", "ISO", "IRD", "TLS", "DNS", "HTTP", "VRAM", "GB", "MB", "MiB", "KiB", "RPCS3", "LLVM", "Vulkan", "OpenGL"};

std::size_t leading_spaces(std::string_view t)
{
    std::size_t n = 0;
    while (n < t.size() && t[n] == ' ') ++n;
    return n;
}

std::size_t trailing_spaces(std::string_view t)
{
    std::size_t n = 0;
    while (n < t.size() && t[t.size() - 1 - n] == ' ') ++n;
    return n;
}

void test_tables()
{
    std::map<std::string, std::array<int, 4>> pending;
    for (std::size_t id = 0; id < i18n::id_count; ++id)
    {
        const auto& row = i18n::detail::table[id];
        const std::string area = area_of(id);
        const bool english_only = g_english_only_areas.contains(area);
        // An English-only row may leave pt-BR empty: the other columns are then compared with the English one
        const int base_column = english_only && !(row.text[0] && row.text[0][0]) ? 1 : 0;
        const std::string_view pt = row.text[base_column] ? row.text[base_column] : "";
        // 1
        if (pt.empty()) row_fail(id, 0, english_only ? "English text is empty" : "pt-BR text is empty");
        for (int l = 1; l < 4; ++l)
        {
            if (row.text[l] && row.text[l][0]) continue;
            if (!english_only && (g_release || g_translated_areas.contains(area))) row_fail(id, l, "not translated");
            else pending[area][l]++;
        }
        if (english_only && base_column == 1) pending[area][0]++;
        std::set<int> pt_marks;
        std::string error;
        for (int l = base_column; l < 4; ++l)
        {
            const std::string_view t = row.text[l] ? row.text[l] : "";
            if (t.empty()) continue;
            ++g_checks;
            // 5: UTF-8, control bytes, glyphs
            const auto decoded = decode_strict(t);
            if (!decoded) { row_fail(id, l, "invalid UTF-8"); continue; }
            for (const char32_t c : *decoded)
            {
                if (c < 0x20 || c == 0x7f) { row_fail(id, l, "control character"); break; }
                if (c == 0x06cc || c == 0x06a9 || (c >= 0x0660 && c <= 0x0669) || (c >= 0x06f0 && c <= 0x06f9))
                {
                    char hex[16];
                    std::snprintf(hex, sizeof(hex), "U+%04X", unsigned(c));
                    row_fail(id, l, std::string("Persian letter or Arabic-Indic digit ") + hex);
                    break;
                }
                if (c == 0x200e || c == 0x200f || c == 0x061c) { row_fail(id, l, "bidi mark in a table (trf adds LRM itself)"); break; }
            }
            for (const char32_t c : arabic::shape(*decoded))
                if (!drawable(c))
                {
                    char hex[16];
                    std::snprintf(hex, sizeof(hex), "U+%04X", unsigned(c));
                    row_fail(id, l, std::string("no XMB glyph for ") + hex);
                    break;
                }
            for (std::size_t k = 0; k < decoded->size(); ++k)
            {
                const char32_t c = (*decoded)[k];
                if (c != U'?' && c != U',') continue;
                const auto near = [&](std::size_t at) { return at < decoded->size() && arabic::is_arabic_letter((*decoded)[at]); };
                if (near(k - 1) || near(k + 1) || (k >= 2 && (*decoded)[k - 1] == U' ' && near(k - 2)) || near(k + 2))
                    row_warn(id, l, "ASCII '?' or ',' next to Arabic letters (use ؟ / ،)");
            }
            // 2: placeholders, braces, printf
            std::set<int> marks;
            if (!placeholders(t, marks, error)) row_fail(id, l, error);
            if (has_printf(t)) row_fail(id, l, "printf specifier");
            if (l == base_column) { pt_marks = marks; continue; }
            if (marks != pt_marks) row_fail(id, l, "placeholders differ from pt-BR");
            // 3: separators
            if (count_of(t, " \xC2\xB7 ") != count_of(pt, " \xC2\xB7 ")) row_fail(id, l, "count of ' · ' differs from pt-BR");
            // 4: paths, file names, tokens, fragment edges
            for (const std::string& token : path_tokens(pt))
                if (t.find(token) == std::string_view::npos) row_fail(id, l, "missing path/file name '" + token + "'");
            for (const std::string_view word : g_word_tokens)
                if (has_word(pt, word) && !has_word(t, word)) row_fail(id, l, "missing token '" + std::string(word) + "'");
            if (leading_spaces(t) != leading_spaces(pt) || trailing_spaces(t) != trailing_spaces(pt))
                row_fail(id, l, "leading/trailing spaces differ from pt-BR (fragment edges)");
            const std::string_view pt_body = pt.substr(leading_spaces(pt));
            const std::string_view t_body = t.substr(leading_spaces(t));
            for (const std::string_view lead : {":", "+", "\xC2\xB7"})
                if (pt_body.starts_with(lead) && !t_body.starts_with(lead)) row_fail(id, l, "fragment must start with '" + std::string(lead) + "'");
            if (l == 3 && !arabic::has_arabic(t) && area != "LANG") row_warn(id, l, "Arabic column without Arabic letters");
        }
    }
    std::printf("i18n: %zu ids\n", i18n::id_count);
    for (const auto& [area, n] : pending)
    {
        if (n[0]) std::printf("i18n: area %s pending translations: pt %d, en %d, es %d, ar %d (English-only allowed)\n", area.c_str(), n[0], n[1], n[2], n[3]);
        else std::printf("i18n: area %s pending translations: en %d, es %d, ar %d\n", area.c_str(), n[1], n[2], n[3]);
    }
}

void test_defaults()
{
    // Before apply(): pt-BR, so every host test keeps its texts
    CHECK(i18n::current() == lang::pt_br);
    CHECK(i18n::current_setting() == setting::automatic);
    CHECK(i18n::system_language() == -1);
    CHECK(!i18n::rtl());
    CHECK(std::string_view(i18n::tr(str::COM_SIM)) == "Sim");
    CHECK(std::string_view(i18n::tr(str::count_)) == "");
    CHECK(std::string_view(i18n::tr(str::COM_SIM, lang::en)) == "Yes");
    CHECK(std::string_view(i18n::tr(str::COM_SIM, lang::es)) == "S\xC3\xAD");
    CHECK(std::string_view(i18n::tr(str::COM_SIM, lang(9))) == "Sim");
    CHECK(i18n::trf(str::count_, {1}).empty());
}

// A visual line as text: ASCII as it is, everything else as <XXXX>
std::string spell(std::string_view utf8)
{
    std::string out;
    for (const char32_t c : arabic::decode(utf8))
    {
        if (c < 0x80) { out += char(c); continue; }
        char hex[12];
        std::snprintf(hex, sizeof(hex), "<%04X>", unsigned(c));
        out += hex;
    }
    return out;
}

void test_arabic_vectors()
{
    const auto shaped = [](std::string_view s) { return arabic::shape(arabic::decode(s)); };
    CHECK(shaped("\xD9\x84\xD8\xA7") == std::u32string({0xfefb}));                                        // لا
    CHECK(shaped("\xD8\xB3\xD9\x84\xD8\xA7\xD9\x85") == std::u32string({0xfeb3, 0xfefc, 0xfee1}));       // سلام
    CHECK(shaped("\xD8\xA8\xD9\x8A\xD8\xAA") == std::u32string({0xfe91, 0xfef4, 0xfe96}));               // بيت

    // Bidi (UAX #9): paragraph direction, N0 bracket pairs, LRM/RLM/ALM, numbers, the fitted ellipsis
    const std::string lugha = "\xD8\xA7\xD9\x84\xD9\x84\xD8\xBA\xD8\xA9";       // اللغة
    const std::string lugha_visual = "<FE94><FED0><FEE0><FEDF><FE8D>";          // shaped, right to left
    const std::string tathbit = "\xD8\xAA\xD8\xAB\xD8\xA8\xD9\x8A\xD8\xAA";     // تثبيت
    const std::string tathbit_visual = "<FE96><FEF4><FE92><FE9C><FE97>";
    const std::string lrm = "\xE2\x80\x8E", rlm = "\xE2\x80\x8F";
    // P2: a line that starts in Latin is left to right in a left-to-right interface; the Arabic word reads right to left
    CHECK(spell(arabic::visual_utf8("Idioma \xC2\xB7 Language \xC2\xB7 " + lugha, false)) == "Idioma <00B7> Language <00B7> " + lugha_visual);
    // In a right-to-left interface the same line is a right-to-left paragraph: the Latin run moves to the right end
    CHECK(spell(arabic::visual_utf8("Idioma \xC2\xB7 Language \xC2\xB7 " + lugha, true)) == lugha_visual + " <00B7> Idioma <00B7> Language");
    // N0: the brackets of a Latin title inside an Arabic line stay with it (no stray mirrored bracket)
    CHECK(spell(arabic::visual_utf8(tathbit + " Demon's Souls (BLUS30443)", true)) == "Demon's Souls (BLUS30443) " + tathbit_visual);
    // N0: brackets around Arabic inside a Latin line (left to right): mirrored as a pair, in place
    CHECK(spell(arabic::visual_utf8("Jogo (\xD8\xB9\xD8\xB1\xD8\xA8\xD9\x8A) fim", false)) == "Jogo (<FEF2><FE91><FEAE><FECB>) fim");
    // Brackets around Arabic in a right-to-left line: mirrored glyphs on the right-to-left level
    CHECK(spell(arabic::visual_utf8(tathbit + " (" + lugha + ")", true)) == "(" + lugha_visual + ") " + tathbit_visual);
    // LRM isolation (trf adds it around arguments): "12.5 GB" stays one run, the marks are removed
    const std::string naskh = "\xD9\x86\xD8\xB3\xD8\xAE", min = "\xD9\x85\xD9\x86", qurs = "\xD8\xA7\xD9\x84\xD9\x82\xD8\xB1\xD8\xB5";
    const std::string with_lrm = arabic::visual_utf8(naskh + " " + lrm + "12.5 GB" + lrm + " " + min + " " + qurs, true);
    CHECK(spell(with_lrm) == "<FEB9><FEAE><FED8><FEDF><FE8D> <FEE6><FEE3> 12.5 GB <FEA6><FEB4><FEE7>");
    CHECK(with_lrm.find(lrm) == std::string::npos);
    // A path written in a table text (no LRM from trf): its leading '/' stays with it, the ':' after it follows the
    // Arabic reading order (left of the path)
    const std::string taadhar = "\xD8\xAA\xD8\xB9\xD8\xB0\xD8\xB1", tabqa = "\xD8\xAA\xD8\xA8\xD9\x82\xD9\x89";
    const std::string path_line = spell(arabic::visual_utf8(taadhar + " /data/rpcs3/ui-language.txt: " + tabqa, true));
    CHECK(path_line.find(" :/data/rpcs3/ui-language.txt ") != std::string::npos);
    CHECK(spell(arabic::visual_utf8(tathbit + " v1.05-beta.", true)) == ".v1.05-beta " + tathbit_visual);
    // Numbers keep their order and their terminators (10:30, 45%)
    CHECK(spell(arabic::visual_utf8("\xD8\xA7\xD9\x84\xD9\x88\xD9\x82\xD8\xAA 10:30 \xD9\x88 45%", true)) == "45% <FEED> 10:30 <FE96><FED7><FEEE><FEDF><FE8D>");
    // The ellipsis fit() appends at the logical end is on the left of an Arabic line
    CHECK(spell(arabic::visual_utf8(tathbit + " " + qurs + "\xE2\x80\xA6", true)).starts_with("<2026><FEB9>"));
    // A line without Arabic keeps its order in every interface; its bidi marks are removed
    CHECK(arabic::visual_utf8("Demon's Souls (BLUS30443)", true) == "Demon's Souls (BLUS30443)");
    CHECK(arabic::visual_utf8(lrm + "v1.05" + lrm, true) == "v1.05");
    CHECK(arabic::visual_utf8(rlm + "abc", false) == "abc");
    // RLM and ALM are strong right-to-left marks for the paragraph rule and are removed
    CHECK(spell(arabic::visual_utf8("\xD8\x9C" "abc " + lugha, false)) == lugha_visual + " abc");
    // reorder() with an explicit direction, and the 0.38 form
    const std::u32string latin = U"abc 12";
    CHECK(arabic::reorder(latin, arabic::paragraph::rtl) == latin);
    CHECK(arabic::reorder(latin, arabic::paragraph::ltr) == latin);
    CHECK(arabic::reorder(latin) == latin);
    CHECK(arabic::reorder(arabic::shape(arabic::decode(lugha))) == arabic::reorder(arabic::shape(arabic::decode(lugha)), arabic::paragraph::rtl));
    CHECK(arabic::visual_utf8(lugha) == arabic::visual_utf8(lugha, true));
    // Deep and unbalanced brackets never break the line (stack of 63)
    std::string deep(80, '(');
    deep += lugha + std::string(80, ')') + ")]}";
    CHECK(arabic::decode(arabic::visual_utf8(deep, true)).size() == arabic::shape(arabic::decode(deep)).size());
    CHECK(arabic::has_bidi_mark(lrm) && arabic::has_bidi_mark(rlm) && !arabic::has_bidi_mark("\xE2\x80\xA6"));
}

#ifdef I18N_TEST_WIDTHS
std::vector<std::uint8_t> read_file(const std::string& path)
{
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

// 7: widths in every language with the XMB font: fixed slots fail, long free lines are warnings (fit() cuts them)
void test_widths(const std::string& latin_path)
{
    namespace xmb = utils::ps4::xmb;
    const std::string here = __FILE__;
    const std::string dir = here.substr(0, here.find_last_of("/\\") + 1);
    const std::vector<std::uint8_t> dejavu = read_file(dir + "../../bin/fonts/DejaVuSans.ttf");
    if (dejavu.empty())
    {
        std::printf("i18n: bin/fonts/DejaVuSans.ttf not found: width checks skipped\n");
        return;
    }
    const std::vector<std::uint8_t> latin = latin_path.empty() ? dejavu : read_file(latin_path);
    xmb::font f;
    CHECK(f.load(latin, dejavu) && f.exact_sizes() && f.arabic());
    struct slot
    {
        std::vector<str> ids;
        float size, max;
        const char* what;
    };
    using S = str;
    const std::vector<slot> slots = {
        {{S::COM_SELECIONAR, S::COM_VOLTAR, S::COM_RESTAURAR, S::COM_ATUALIZAR}, 18, 124, "footer hint"},
        {{S::XMB_ESQ_DIR_ALTERAR_R1_ACELERAR, S::XMB_QUADRADO_CONFIGURAR_JOGO_BOTAO_PS}, 18, 736, "footer help"},
        {{S::COM_SIM, S::COM_NAO, S::COM_OK, S::COM_VOLTAR, S::COM_CANCELAR, S::COM_INSTALAR, S::COM_APAGAR, S::COM_CONTINUAR, S::COM_JOGAR, S::COM_SAIR},
            20, 220, "button label"},
        {{S::XMB_USUARIOS, S::XMB_CONFIGURACOES, S::XMB_FOTO, S::XMB_MUSICA, S::XMB_VIDEO, S::XMB_TV_SERVICOS_VIDEO, S::XMB_JOGO, S::XMB_REDE, S::XMB_AMIGOS},
            21, 260, "category"},
        {{S::SET_TODOS_JOGOS_CONECTAM_RPCN_CONTA, S::SET_JOGOS_FICAM_OFFLINE_LIGA_RPCN, S::SET_VALE_QUANDO_MODO_ONLINE_ESTIVER, S::SET_SO_FUNCIONA_MODO_ONLINE,
             S::SET_ROTEADOR_ABRE_PORTAS_JOGOS_ONLINE, S::SET_PORTAS_ROTEADOR_FECHADAS_PADRAO_LIGA, S::SET_MENOS_CPU_GIRANDO_TOA_MENOS,
             S::SET_CENA_VISTA_PRIMEIRA_VEZ_SEGURA, S::SET_AUDIO_CONTINUO_HELP, S::GL_HELP_GLOBAL, S::LANG_ROW_HELP, S::SET_KEEP_USER_SETTINGS_FLAG_PRESENTE,
             S::SET_CADA_JOGO_INICIA_AJUSTES_RECOMENDADOS, S::SET_JOGOS_USAM_VALORES_SALVOS_LIGA, S::SET_MARCADOR_ANTIGO_SEM_EFEITO_VALORES,
             S::SET_LLVM_VULKAN_720P_SHADERS_ASSINCRONOS, S::SET_VALORES_AUTOMATICOS_PS4_OPCOES_DESTE, S::SET_ABRIR, S::GL_TITLE_GLOBAL, S::GL_TITLE_GAME,
             S::SET_AUDIO_CONTINUO_AUTOMATICO, S::SET_AUDIO_CONTINUO_DESLIGADO, S::LANG_ROW_TITLE},
            // the row text starts at x 450 and the font stops drawing once a glyph starts past x 1256
            17, 800, "row subtitle"},
        // 13: the settings browser page (footer, the save question's buttons)
        {{S::CFGU_FOOTER_CHANGE, S::CFGU_FOOTER_OPTIONS, S::CFGU_FOOTER_AUTO}, 18, 124, "footer hint"},
        {{S::CFGU_FOOTER_HELP}, 18, 736, "footer help"},
        // 14: the cheat list's footer
        {{S::CHT_DICA_LISTA}, 18, 736, "footer help"},
        {{S::CFGU_SAVE, S::CFGU_DISCARD, S::CFGU_KEEP_EDITING}, 20, 220, "button label"},
    };
    // 13: the settings browser's slots: labels from x 170 fitted to 560 px and values to 360 px (21 px; one English PC
    // value text is 379 px wide: a warning up to 400 px), help lines of two 1100 px lines at 17 px (one line expected),
    // section captions at 18 px, badges at 15 px
    struct cfg_slot
    {
        std::string_view prefix;
        float size, max, warn;
        const char* what;
    };
    static constexpr cfg_slot cfg_slots[] = {
        {"CFGL_", 21, 560, 560, "settings label"},
        {"CFGV_", 21, 400, 360, "settings value"},
        {"CFGT_", 21, 400, 400, "settings tab"},
        {"CFGG_", 18, 1000, 900, "settings section"},
        {"CFGH_", 17, 2200, 1100, "settings help"},
        {"CFGR_", 17, 1700, 900, "settings reason (after 'Indisponível no PS4: ')"},
        {"CFGU_BADGE_", 15, 170, 170, "settings badge"},
    };
    int cfg_long = 0;
    static constexpr setting settings[] = {setting::pt_br, setting::en, setting::es, setting::ar};
    static constexpr const char* names[] = {"pt", "en", "es", "ar"};
    int long_lines = 0;
    for (int l = 0; l < 4; ++l)
    {
        i18n::apply(settings[l], -1);
        const auto sample = [](str id) { return i18n::trf(id, {"Demon's Souls (BLUS30443)", 4096, 4096, "v01.12"}); };
        float widest_hint = 0;
        for (const slot& s : slots)
            for (const str id : s.ids)
            {
                const float w = f.measure(sample(id), s.size);
                if (std::string_view(s.what) == "footer hint") widest_hint = std::max(widest_hint, w);
                ++g_checks;
                if (w > s.max)
                {
                    ++g_failures;
                    std::printf("FAIL %s [%s]: %s %.0f px > %.0f px at %.0f px\n", i18n::detail::names[std::size_t(id)], names[l], s.what, w, s.max, s.size);
                }
            }
        // Every other text of the XMB tables as a message line (20 px, fitted to 1160 px)
        for (std::size_t id = 0; id < i18n::id_count; ++id)
        {
            const std::string area = area_of(id);
            if (area != "SET" && area != "POL" && area != "GL" && area != "LANG" && area != "COM" && area != "XMB" && area != "DLG" && area != "CHT") continue;
            const float w = f.measure(sample(str(id)), 20);
            if (w > 1160)
            {
                ++long_lines;
                row_warn(id, l, "wider than the message line (" + std::to_string(int(w)) + " px at 20 px): fit() ends it with an ellipsis");
            }
        }
        for (std::size_t id = 0; id < i18n::id_count; ++id)
        {
            const std::string_view name = i18n::detail::names[id];
            for (const cfg_slot& s : cfg_slots)
            {
                if (!name.starts_with(s.prefix)) continue;
                const float w = f.measure(sample(str(id)), s.size);
                ++g_checks;
                if (w > s.max)
                {
                    ++g_failures;
                    std::printf("FAIL %s [%s]: %s %.0f px > %.0f px at %.0f px\n", i18n::detail::names[id], names[l], s.what, w, s.max, s.size);
                }
                else if (w > s.warn)
                {
                    ++cfg_long;
                    row_warn(id, l, std::string(s.what) + " " + std::to_string(int(w)) + " px > " + std::to_string(int(s.warn)) + " px: wraps or ends with an ellipsis");
                }
                break;
            }
        }
        if (g_verbose) std::printf("i18n: widest footer hint in %s: %.0f px\n", names[l], widest_hint);
        // fit(): the result fits and is a prefix of the logical text plus the ellipsis
        const std::string long_text = sample(str::COM_PROCURANDO_PS3UPDAT_PUP_PENDRIVE_PS3) + " " + sample(str::LANG_SAVE_FAILED);
        const std::string fitted = f.fit(long_text, 20, 600, l == 3);
        CHECK(f.measure(fitted, 20) <= 600.5f);
        CHECK(fitted.ends_with("\xE2\x80\xA6") && long_text.starts_with(fitted.substr(0, fitted.size() - 3)));
        CHECK(f.fit("OK", 20, 600, l == 3) == "OK");
        // The drawn glyphs: nothing of a translated line maps to '?' (the 129 Arabic forms are in the atlas)
        for (const slot& s : slots)
            for (const str id : s.ids)
            {
                const std::string shown = f.visual(sample(id), l == 3);
                for (const char32_t c : arabic::decode(shown)) CHECK(c != U'?' || std::string_view(i18n::tr(id, lang::pt_br)).find('?') != std::string_view::npos);
            }
    }
    i18n::apply(setting::automatic, -1);
    std::printf("i18n: widths checked with %s + DejaVu Sans Arabic; %d long message line(s) end with an ellipsis, %d long settings text(s)%s\n",
        latin_path.empty() ? "DejaVu Sans" : latin_path.c_str(), long_lines, cfg_long, g_verbose ? "" : " (--verbose lists them)");
}
#endif

// 13: the settings browser's texts against the PC dialog and the catalog
void test_cfg_texts()
{
    namespace cat = utils::ps4::cfg_catalog;
    struct pc_label
    {
        std::string_view key, tab, text;
    };
    struct pc_value
    {
        std::string_view key, spelling, text;
    };
    struct pc_tip
    {
        std::string_view key, text;
    };
    static constexpr pc_label labels[] = {
#define CFG_PC_LABEL(key, tab, text) {key, tab, text},
#define CFG_PC_VALUE(key, spelling, text)
#define CFG_PC_TIP(key, text)
#define CFG_HOME_MENU(key)
#include "cfg-mocks/cfg_pc_text.inc"
#undef CFG_HOME_MENU
#undef CFG_PC_TIP
#undef CFG_PC_VALUE
#undef CFG_PC_LABEL
    };
    static constexpr pc_value values[] = {
#define CFG_PC_LABEL(key, tab, text)
#define CFG_PC_VALUE(key, spelling, text) {key, spelling, text},
#define CFG_PC_TIP(key, text)
#define CFG_HOME_MENU(key)
#include "cfg-mocks/cfg_pc_text.inc"
#undef CFG_HOME_MENU
#undef CFG_PC_TIP
#undef CFG_PC_VALUE
#undef CFG_PC_LABEL
    };
    static constexpr pc_tip tips[] = {
#define CFG_PC_LABEL(key, tab, text)
#define CFG_PC_VALUE(key, spelling, text)
#define CFG_PC_TIP(key, text) {key, text},
#define CFG_HOME_MENU(key)
#include "cfg-mocks/cfg_pc_text.inc"
#undef CFG_HOME_MENU
#undef CFG_PC_TIP
#undef CFG_PC_VALUE
#undef CFG_PC_LABEL
    };
    static constexpr std::string_view home[] = {
#define CFG_PC_LABEL(key, tab, text)
#define CFG_PC_VALUE(key, spelling, text)
#define CFG_PC_TIP(key, text)
#define CFG_HOME_MENU(key) key,
#include "cfg-mocks/cfg_pc_text.inc"
#undef CFG_HOME_MENU
#undef CFG_PC_TIP
#undef CFG_PC_VALUE
#undef CFG_PC_LABEL
    };
    const auto fail = [](std::string_view key, const std::string& what)
    {
        ++g_checks;
        ++g_failures;
        std::printf("FAIL cfg '%.*s': %s\n", int(key.size()), key.data(), what.c_str());
    };
    const auto name_of = [](str id) { return std::string_view(id == str::count_ ? "count_" : i18n::detail::names[std::size_t(id)]); };
    static constexpr std::pair<std::string_view, cat::tab> pc_tabs[] = {{"CPU", cat::tab::cpu}, {"GPU", cat::tab::gpu}, {"Audio", cat::tab::audio},
        {"I/O", cat::tab::io}, {"System", cat::tab::system}, {"Network", cat::tab::network}, {"Advanced", cat::tab::advanced},
        {"Emulator", cat::tab::emulator}, {"Debug", cat::tab::debug}};
    // Every catalog row has its texts; the English label is the PC text; the PC tab is kept (a setting only config.yml has
    // goes to the tab of its subject)
    CHECK(cat::item_count == std::size(labels));
    for (const cat::item& it : cat::items())
    {
        const pc_label* pc = nullptr;
        for (const pc_label& l : labels)
            if (l.key == it.key) pc = &l;
        if (!pc) { fail(it.key, "no CFG_PC_LABEL (run gen_cfg_catalog.py)"); continue; }
        ++g_checks;
        if (std::string_view(i18n::tr(it.label, lang::en)) != pc->text)
            fail(it.key, "English label '" + std::string(i18n::tr(it.label, lang::en)) + "' is not the PC text '" + std::string(pc->text) + "'");
        for (const auto& [tab_name, t] : pc_tabs)
            if (pc->tab == tab_name && it.where != t) fail(it.key, "tab differs from the PC tab " + std::string(tab_name));
        ++g_checks;
        if (!name_of(it.label).starts_with("CFGL_") || !name_of(it.help).starts_with("CFGH_") || !name_of(it.section).starts_with("CFGG_"))
            fail(it.key, "label / help / section ids");
        if (it.kind != cat::klass::expose && it.reason == str::count_) fail(it.key, "class W / U / H without a reason");
        const pc_tip* tip = nullptr;
        for (const pc_tip& t : tips)
            if (t.key == it.key) tip = &t;
        ++g_checks;
        if (tip && (it.tip == str::count_ || std::string_view(i18n::tr(it.tip, lang::en)) != tip->text)) fail(it.key, "tooltip is not the PC text");
        if (!tip && it.tip != str::count_) fail(it.key, "tooltip without a PC tooltip");
        const bool in_home = std::find(std::begin(home), std::end(home), it.key) != std::end(home);
        ++g_checks;
        if (in_home != ((it.flags & cat::flag::home_menu) != 0)) fail(it.key, "F_HOME differs from the in-game menu (run gen_cfg_catalog.py)");
    }
    // Every PC value text is a catalog value label with the same English text
    for (const pc_value& v : values)
    {
        const cat::value_label* label = cat::find_value(v.key, v.spelling);
        ++g_checks;
        if (!label) { fail(v.key, "no CFG_VALUE for '" + std::string(v.spelling) + "'"); continue; }
        const std::string_view en = label->label != str::count_ ? std::string_view(i18n::tr(label->label, lang::en)) : label->text;
        if (en != v.text) fail(v.key, "value '" + std::string(v.spelling) + "' reads '" + std::string(en) + "', PC: '" + std::string(v.text) + "'");
    }
    // Value labels name existing ids or carry a text
    for (const cat::value_label& v : cat::value_labels())
    {
        ++g_checks;
        if (v.label == str::count_ && v.text.empty()) fail(v.key, "value '" + std::string(v.spelling) + "' without a label");
        if (!cat::find(v.key)) fail(v.key, "value label of an unknown setting");
    }
    for (std::size_t t = 0; t < cat::tab_count; ++t) CHECK(cat::tab_name(cat::tab(t)) != str::count_);
}

void test_settings()
{
    using i18n::parse_setting;
    CHECK(parse_setting("auto") == setting::automatic);
    CHECK(parse_setting("AUTO\n") == setting::automatic);
    CHECK(parse_setting("Autom\xC3\xA1tico") == setting::automatic);
    CHECK(parse_setting(" pt-BR \r\n") == setting::pt_br);
    CHECK(parse_setting("pt_br") == setting::pt_br);
    CHECK(parse_setting("PTBR") == setting::pt_br);
    CHECK(parse_setting("por") == setting::pt_br);
    CHECK(parse_setting("Portugu\xC3\xAAs") == setting::pt_br);
    CHECK(parse_setting("PORTUGU\xC3\x8AS") == setting::pt_br);
    CHECK(parse_setting("english") == setting::en);
    CHECK(parse_setting("EN-US") == setting::en);
    CHECK(parse_setting("en_gb") == setting::en);
    CHECK(parse_setting("es-419") == setting::es);
    CHECK(parse_setting("Espa\xC3\xB1ol") == setting::es);
    CHECK(parse_setting("ESPA\xC3\x91OL") == setting::es);
    CHECK(parse_setting("spanish") == setting::es);
    CHECK(parse_setting("ar") == setting::ar);
    CHECK(parse_setting("Arabic") == setting::ar);
    CHECK(parse_setting("\xD8\xA7\xD9\x84\xD8\xB9\xD8\xB1\xD8\xA8\xD9\x8A\xD8\xA9") == setting::ar);
    CHECK(parse_setting("\xEF\xBB\xBF" "en\n") == setting::en);
    CHECK(!parse_setting(""));
    CHECK(!parse_setting("  \n"));
    CHECK(!parse_setting("fr"));
    CHECK(!parse_setting("e n"));
    CHECK(!parse_setting(std::string(65, 'a')));
    CHECK(!parse_setting(std::string(60, ' ') + "en  " + std::string(2, ' ')));
    for (const setting s : {setting::automatic, setting::pt_br, setting::en, setting::es, setting::ar})
        CHECK(parse_setting(i18n::setting_token(s)) == s);

    using i18n::from_system;
    CHECK(from_system(17) == lang::pt_br);
    CHECK(from_system(7) == lang::pt_br);
    CHECK(from_system(1) == lang::en);
    CHECK(from_system(18) == lang::en);
    CHECK(from_system(3) == lang::es);
    CHECK(from_system(20) == lang::es);
    CHECK(from_system(21) == lang::ar);
    CHECK(from_system(2) == lang::en);
    CHECK(from_system(0) == lang::en);
    CHECK(from_system(29) == lang::en);
    CHECK(from_system(30) == lang::en);
    CHECK(from_system(-1) == lang::pt_br);

    for (std::int32_t v = 0; v < 20; ++v) CHECK(i18n::cell_lang_from_ps4(v) == v);
    CHECK(i18n::cell_lang_from_ps4(20) == 3);
    CHECK(i18n::cell_lang_from_ps4(21) == 1);
    CHECK(i18n::cell_lang_from_ps4(22) == 2);
    for (std::int32_t v = 23; v <= 29; ++v) CHECK(i18n::cell_lang_from_ps4(v) == 1);
    CHECK(i18n::cell_lang_from_ps4(-1) == 1);
    CHECK(i18n::cell_lang_for(lang::pt_br) == 17);
    CHECK(i18n::cell_lang_for(lang::en) == 1);
    CHECK(i18n::cell_lang_for(lang::es) == 3);
    CHECK(i18n::cell_lang_for(lang::ar) == 1);

    i18n::apply(setting::automatic, 17);
    CHECK(i18n::current() == lang::pt_br && !i18n::rtl() && i18n::system_language() == 17 && i18n::game_language_auto() == 17);
    i18n::apply(setting::automatic, 21);
    CHECK(i18n::current() == lang::ar && i18n::rtl() && i18n::game_language_auto() == 1);
    i18n::apply(setting::automatic, 2);
    CHECK(i18n::current() == lang::en && i18n::game_language_auto() == 2);
    i18n::apply(setting::es, 17);
    CHECK(i18n::current() == lang::es && i18n::current_setting() == setting::es && i18n::game_language_auto() == 3);
    i18n::apply(setting::pt_br, 1);
    CHECK(i18n::current() == lang::pt_br && i18n::game_language_auto() == 17);
    i18n::apply(setting::automatic, -1);
    CHECK(i18n::current() == lang::pt_br && i18n::game_language_auto() == 1);
    i18n::apply(setting(9), 3);
    CHECK(i18n::current_setting() == setting::automatic && i18n::current() == lang::es);

    CHECK(std::string_view(i18n::endonym(lang::pt_br)) == "Portugu\xC3\xAAs (Brasil)");
    CHECK(std::string_view(i18n::endonym(lang::ar)) == "\xD8\xA7\xD9\x84\xD8\xB9\xD8\xB1\xD8\xA8\xD9\x8A\xD8\xA9");
    i18n::apply(setting::automatic, 1);
    CHECK(i18n::setting_label(setting::automatic) == "Automatic (English)");
    CHECK(i18n::setting_label(setting::es) == "Espa\xC3\xB1ol");
    i18n::apply(setting::automatic, 17);
    CHECK(i18n::setting_label(setting::automatic) == "Autom\xC3\xA1tico (Portugu\xC3\xAAs (Brasil))");
    i18n::apply(setting::automatic, 21);
    // translated text argument: no LRM
    std::string expected = i18n::tr(str::LANG_AUTO_FMT, lang::ar);
    expected.replace(expected.find("{0}"), 3, i18n::endonym(lang::ar));
    CHECK(i18n::setting_label(setting::automatic) == expected);
    CHECK(std::string_view(i18n::tr(i18n::game_language_name(17), lang::pt_br)) == "Portugu\xC3\xAAs (Brasil)");
    CHECK(i18n::game_language_name(-5) == str::GL_NAME_1);
    CHECK(i18n::game_language_name(20) == str::GL_NAME_1);
    CHECK(i18n::game_language_name(0) == str::GL_NAME_0);
    i18n::apply(setting::automatic, -1);
}

std::string run(std::string_view pattern, std::initializer_list<i18n::arg> args, lang l = lang::pt_br, bool isolate = false)
{
    std::string out;
    i18n::detail::format(pattern, args.begin(), args.size(), l, isolate, [&](std::string_view piece)
    {
        out.append(piece);
        return true;
    });
    return out;
}

void test_format()
{
    const std::string lrm = "\xE2\x80\x8E";
    using i18n::arg;
    using i18n::raw;
    CHECK(run("{1} {0}", {"a", "b"}) == "b a");
    CHECK(run("{{0}} {0}", {"x"}) == "{0}} x");
    CHECK(run("{0}-{3}", {"x"}) == "x-");
    CHECK(run("{0}", {"x", "y", "z"}) == "x");
    CHECK(run("{0", {"x"}) == "{0");
    CHECK(run("{a} {", {"x"}) == "{a} {");
    CHECK(run("", {"x"}).empty());
    CHECK(run("{0}{0}", {"ab"}) == "abab");
    CHECK(run("{0} {1} {2} {3}", {-42, 0, 18446744073709551615ull, LLONG_MIN}) == "-42 0 18446744073709551615 -9223372036854775808");
    CHECK(run("{0} {1}", {4294967295u, 2147483647L}) == "4294967295 2147483647");
    CHECK(run("{0}", {std::string("str")}) == "str");
    CHECK(run("{0}", {static_cast<const char*>(nullptr)}).empty());
    // translated id arguments follow the template's language
    CHECK(run("{0}", {str::COM_SIM}, lang::en) == "Yes");
    CHECK(run("{0}", {str::COM_SIM}, lang::pt_br) == "Sim");
    // copies keep their own number
    const arg a(123);
    const arg b = a;
    CHECK(b.view(lang::pt_br) == "123");
    // isolation: text and numbers wrapped, raw() and Arabic text not, empty arguments dropped
    const std::string ar_word = "\xD9\x86\xD8\xB9\xD9\x85";
    CHECK(run("[{0}]", {"12.5 GB"}, lang::ar, true) == "[" + lrm + "12.5 GB" + lrm + "]");
    CHECK(run("[{0}]", {7}, lang::ar, true) == "[" + lrm + "7" + lrm + "]");
    CHECK(run("[{0}]", {raw("12.5 GB")}, lang::ar, true) == "[12.5 GB]");
    CHECK(run("[{0}]", {ar_word}, lang::ar, true) == "[" + ar_word + "]");
    CHECK(run("[{0}]", {""}, lang::ar, true) == "[]");
    CHECK(run("[{0}]", {"12.5 GB"}, lang::ar, false) == "[12.5 GB]");

    // trf on real rows
    CHECK(i18n::trf(lang::pt_br, str::SET_CONTROLES_CONECTADOS_4, {3}) == "Controles conectados: 3 de 4");
    CHECK(i18n::trf(lang::en, str::SET_CONTROLES_CONECTADOS_4, {3}) == "Connected controllers: 3 of 4");
    CHECK(i18n::trf(lang::ar, str::SET_CONTROLES_CONECTADOS_4, {3}).find(lrm + "3" + lrm) != std::string::npos);
    CHECK(i18n::trf(lang::ar, str::SET_CONTROLES_CONECTADOS_4, {raw("3")}).find(lrm) == std::string::npos);
    CHECK(i18n::trf(lang::pt_br, str::SET_BOOT_MANTEM_LIMITE_CACHE_ENTRE, {512, 4096}) == "o boot mant\xC3\xA9m o limite de cache entre 512 e 4096 MB");
    CHECK(i18n::trf(lang::pt_br, str::POL_ADAPT_TOAST, {"BLUS30443", str::POL_COMPILACAO_1_THREAD}) ==
        "RPCS3: ajuste automatico para BLUS30443 apos falha anterior: compilacao com 1 thread");
    CHECK(i18n::trf(lang::pt_br, str::COM_SIM, {}) == "Sim");
    // the current language is used by the short forms
    i18n::apply(setting::en, -1);
    CHECK(i18n::trf(str::SET_CONTROLES_CONECTADOS_4, {2}) == "Connected controllers: 2 of 4");
    i18n::apply(setting::automatic, -1);
    CHECK(i18n::trf(str::SET_CONTROLES_CONECTADOS_4, {2}) == "Controles conectados: 2 de 4");

    // a template served by the English fallback in Arabic: no LRM (the line has no Arabic for the visual transform)
    bool found_fallback = false;
    for (std::size_t id = 0; id < i18n::id_count && !found_fallback; ++id)
    {
        const auto& row = i18n::detail::table[id];
        if ((row.text[3] && row.text[3][0]) || std::string_view(row.text[0]).find("{0}") == std::string_view::npos) continue;
        found_fallback = true;
        const std::string out = i18n::trf(lang::ar, str(id), {"v1.05", "x", "y", "z", "w"});
        CHECK(out.find(lrm) == std::string::npos);
        CHECK(out == i18n::trf(i18n::detail::column(row, lang::ar), str(id), {"v1.05", "x", "y", "z", "w"}));
    }
    if (!found_fallback) std::printf("i18n: every id is translated to Arabic: fallback isolation check skipped\n");

    // trs: identical to trf when it fits; for every buffer size a NUL-terminated prefix cut on a UTF-8 boundary
    struct sample { lang l; str id; };
    for (const sample s : {sample{lang::ar, str::SET_CONTROLES_CONECTADOS_4}, sample{lang::es, str::COM_SIM}, sample{lang::ar, str::LANG_SAVE_FAILED},
             sample{lang::pt_br, str::COM_DESEJA_CANCELAR_INSTALACAO_DISCO}})
    {
        const std::string full = i18n::trf(s.l, s.id, {3});
        for (std::size_t size = 1; size <= full.size() + 2; ++size)
        {
            char buffer[512];
            std::memset(buffer, 'Z', sizeof(buffer));
            const std::size_t length = i18n::trs(buffer, size, s.l, s.id, {3});
            CHECK(length < size);
            CHECK(buffer[length] == '\0');
            CHECK(length == i18n::utf8_fit(full, size - 1));
            CHECK(std::string_view(buffer, length) == std::string_view(full).substr(0, length));
            CHECK(decode_strict(std::string_view(buffer, length)).has_value());
            CHECK(buffer[size] == 'Z');
        }
    }
    char untouched[4] = {'a', 'b', 'c', 'd'};
    CHECK(i18n::trs(untouched, 0, lang::pt_br, str::COM_SIM, {}) == 0 && untouched[0] == 'a');
    CHECK(i18n::trs(nullptr, 16, lang::pt_br, str::COM_SIM, {}) == 0);
    char one[1];
    CHECK(i18n::trs(one, 1, lang::pt_br, str::COM_SIM, {}) == 0 && one[0] == '\0');
    char small[3];
    CHECK(i18n::trs(small, sizeof(small), lang::es, str::COM_SIM, {}) == 1 && std::string_view(small) == "S");
    char fits[4];
    CHECK(i18n::trs(fits, sizeof(fits), lang::es, str::COM_SIM, {}) == 3 && std::string_view(fits) == "S\xC3\xAD");
    char array_form[64];
    CHECK(i18n::trs(array_form, str::COM_SIM, {}) == 3 && std::string_view(array_form) == "Sim");
    char bad_id[8] = "xxxxxxx";
    CHECK(i18n::trs(bad_id, sizeof(bad_id), lang::pt_br, str::count_, {}) == 0 && bad_id[0] == '\0');

    CHECK(i18n::utf8_fit("abc", 5) == 3);
    CHECK(i18n::utf8_fit("a\xC3\xADz", 2) == 1);
    CHECK(i18n::utf8_fit("a\xC3\xADz", 3) == 3);
    CHECK(i18n::utf8_fit("\xE2\x80\x8E", 2) == 0);
    CHECK(i18n::utf8_fit("abc", 0) == 0);
}

void test_fallback()
{
    using i18n::detail::column;
    using i18n::detail::row;
    using i18n::detail::text;
    constexpr row full{{"pt", "en", "es", "ar"}};
    constexpr row no_ar{{"pt", "en", "es", ""}};
    constexpr row pt_only{{"pt", "", "", ""}};
    constexpr row nulls{{"pt", nullptr, nullptr, nullptr}};
    static_assert(column(full, lang::ar) == lang::ar);
    static_assert(column(no_ar, lang::ar) == lang::en);
    static_assert(column(pt_only, lang::es) == lang::pt_br);
    CHECK(std::string_view(text(no_ar, lang::ar)) == "en");
    CHECK(std::string_view(text(no_ar, lang::es)) == "es");
    CHECK(std::string_view(text(pt_only, lang::ar)) == "pt");
    CHECK(std::string_view(text(pt_only, lang::en)) == "pt");
    CHECK(std::string_view(text(nulls, lang::ar)) == "pt");
    CHECK(std::string_view(text(full, lang(7))) == "pt");
}

void test_notice()
{
    using notice::kind;
    const kind kinds[] = {kind::text, kind::disc_removed, kind::emulator_error, kind::emulator_restart, kind::clean_process, kind::vram_stop,
        kind::heap_stop, kind::boot_next, kind::firmware_missing, kind::firmware_old, kind::previous_fatal};
    std::set<std::string> names;
    for (const kind k : kinds)
    {
        names.insert(notice::kind_name(k));
        for (const bool restart : {false, true})
        {
            const std::string detail = "Demon's Souls \xC2\xB7 detalhe";
            const std::string e = notice::encode(k, detail, restart);
            CHECK(notice::is_encoded(e));
            const notice::decoded d = notice::decode(e);
            CHECK(d.k == k);
            CHECK(d.restart_advised == restart);
            CHECK(d.detail == detail);
            CHECK(notice::decode(notice::encode(k, {}, restart)).detail.empty());
        }
    }
    CHECK(names.size() == std::size(kinds) && !names.contains("unknown"));
    const notice::decoded plain = notice::decode("O disco foi removido.");
    CHECK(plain.k == kind::text && !plain.restart_advised && plain.detail == "O disco foi removido.");
    CHECK(!notice::is_encoded("O disco foi removido."));
    CHECK(notice::decode("").k == kind::text && notice::decode("").detail.empty());
    // control bytes never reach the detail
    const notice::decoded forged = notice::decode(notice::encode(kind::text, "a\x1d" "D\x1e" "b"));
    CHECK(forged.k == kind::text && forged.detail == "aDb");
    // malformed and unknown kinds degrade to text
    const notice::decoded no_separator = notice::decode("\x1dVr detail");
    CHECK(no_separator.k == kind::text && no_separator.detail == "Vr detail");
    const notice::decoded unknown = notice::decode("\x1dZrq\x1e" "abc");
    CHECK(unknown.k == kind::text && unknown.restart_advised && unknown.detail == "abc");
    CHECK(notice::encode(kind('Z'), "x")[1] == 'T');
    CHECK(notice::describe(notice::encode(kind::vram_stop, "x", true)) == "[vram_stop, restart advised] x");
    CHECK(notice::describe(notice::encode(kind::boot_next, "Folklore")) == "[boot_next] Folklore");
    CHECK(notice::describe("plain") == "plain");
}
} // namespace

// 14: problem texts that take the source line keep it first in every language
void test_cheat_texts()
{
    static constexpr std::array<std::string_view, 4> line_first = {"Linha {0}", "Line {0}", "Línea {0}", "السطر {0}"};
    int with_line = 0;
    for (std::size_t id = 0; id < i18n::id_count; ++id)
    {
        if (area_of(id) != "CHT") continue;
        const auto& row = i18n::detail::table[id];
        if (!std::string_view(row.text[0] ? row.text[0] : "").starts_with(line_first[0])) continue;
        ++with_line;
        for (int l = 1; l < 4; ++l)
        {
            ++g_checks;
            if (!std::string_view(row.text[l] ? row.text[l] : "").starts_with(line_first[std::size_t(l)])) row_fail(id, l, "a line problem must start with its line");
        }
    }
    CHECK(with_line >= 20);
}

int main(int argc, char** argv)
{
    std::string latin_font;
    for (int i = 1; i < argc; ++i)
    {
        const std::string_view a = argv[i];
        if (a == "--release") g_release = true;
        else if (a == "--verbose") g_verbose = true;
        else if (a == "--font" && i + 1 < argc) latin_font = argv[++i];
    }
    test_defaults();
    test_tables();
    test_arabic_vectors();
#ifdef I18N_TEST_WIDTHS
    test_widths(latin_font);
#else
    std::printf("i18n: string width checks skipped (build with -I 3rdparty/stblib/stb)\n");
#endif
    test_settings();
    test_format();
    test_fallback();
    test_notice();
    test_cfg_texts();
    test_cheat_texts();
    std::printf("i18n_test: %d checks, %d failures, %d warnings%s\n", g_checks, g_failures, g_warnings, g_verbose ? "" : " (--verbose lists them)");
    return g_failures ? 1 : 0;
}
