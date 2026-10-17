#pragma once

// Interface language of the launcher, XMB, dialogs and toasts: pt-BR (the source text), English, Spanish, Arabic.
// Header-only, standard library only (host tests include it next to the policy, disc, PKG and patch headers).
// tr() and trs() are async-signal-safe: constant tables, relaxed atomics, no allocation.
//
// Tables: one row per id, I18N(ID, "pt-BR", "en", "es", "ar"), in the ps4_i18n_<area>.inc files (UTF-8, LF).
// An empty translation falls back to English, then to pt-BR. Placeholders {0}..{9} are numbered in the pt-BR order
// and may be reordered; no other '{' or '}' and no printf specifiers. " · " separates the parts of an XMB line (they
// become dialog rows): every language keeps the same count. Paths, file names and tokens (PS3, PPU, SPU, X, GoldHEN,
// ...) are byte-identical in every language. Numbers are formatted by the caller; '.' is the decimal point everywhere.

#include "ps4_arabic.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>

namespace utils::ps4::i18n
{
enum class lang : std::uint8_t { pt_br = 0, en = 1, es = 2, ar = 3 };
inline constexpr std::size_t lang_count = 4;

// Contents of ui-language.txt; automatic follows the PS4 system language.
enum class setting : std::uint8_t { automatic = 0, pt_br = 1, en = 2, es = 3, ar = 4 };
inline constexpr char setting_path[] = "/data/rpcs3/ui-language.txt";
inline constexpr std::size_t setting_max_bytes = 64;

enum class str : std::uint16_t
{
#define I18N(id, pt, en, es, ar) id,
#include "ps4_i18n_common.inc"
#include "ps4_i18n_xmb.inc"
#include "ps4_i18n_launcher.inc"
#include "ps4_i18n_main.inc"
#include "ps4_i18n_core.inc"
#include "ps4_i18n_overlay.inc"
#include "ps4_i18n_rsx.inc"
#include "ps4_i18n_cpu.inc"
#include "ps4_i18n_cfg.inc"
#include "ps4_i18n_cheats.inc"
#undef I18N
    count_
};
inline constexpr std::size_t id_count = std::size_t(str::count_);

// CELL_SYSUTIL_LANG_* values 0..19 (the PS3 game language), named by GL_NAME_0..GL_NAME_19
inline constexpr std::int32_t cell_lang_count = 20;
static_assert(std::size_t(str::GL_NAME_19) - std::size_t(str::GL_NAME_0) == std::size_t(cell_lang_count - 1), "GL_NAME_0..19 are contiguous");

namespace detail
{
struct row
{
    const char* text[lang_count];
};

inline constexpr row table[] = {
#define I18N(id, pt, en, es, ar) {{pt, en, es, ar}},
#include "ps4_i18n_common.inc"
#include "ps4_i18n_xmb.inc"
#include "ps4_i18n_launcher.inc"
#include "ps4_i18n_main.inc"
#include "ps4_i18n_core.inc"
#include "ps4_i18n_overlay.inc"
#include "ps4_i18n_rsx.inc"
#include "ps4_i18n_cpu.inc"
#include "ps4_i18n_cfg.inc"
#include "ps4_i18n_cheats.inc"
#undef I18N
};
static_assert(sizeof(table) / sizeof(table[0]) == id_count, "one table row per id");

#ifdef PS4_I18N_ID_NAMES
// Id spellings, for host tests and tools only (not in the PS4 build)
inline constexpr const char* names[] = {
#define I18N(id, pt, en, es, ar) #id,
#include "ps4_i18n_common.inc"
#include "ps4_i18n_xmb.inc"
#include "ps4_i18n_launcher.inc"
#include "ps4_i18n_main.inc"
#include "ps4_i18n_core.inc"
#include "ps4_i18n_overlay.inc"
#include "ps4_i18n_rsx.inc"
#include "ps4_i18n_cpu.inc"
#include "ps4_i18n_cfg.inc"
#include "ps4_i18n_cheats.inc"
#undef I18N
};
static_assert(sizeof(names) / sizeof(names[0]) == id_count, "one name per id");
#endif

inline std::atomic<std::uint8_t> g_lang{0};    // lang; pt_br until apply()
inline std::atomic<std::uint8_t> g_setting{0}; // setting
inline std::atomic<std::int32_t> g_system{-1}; // ORBIS_SYSTEM_PARAM_LANG_*, -1 unknown

// Column that serves 'l' in 'r': the requested one when filled, else English, else pt-BR
constexpr lang column(const row& r, lang l) noexcept
{
    const std::size_t i = std::size_t(l) < lang_count ? std::size_t(l) : 0;
    if (r.text[i] && r.text[i][0]) return lang(i);
    if (r.text[1] && r.text[1][0]) return lang::en;
    return lang::pt_br;
}

constexpr const char* text(const row& r, lang l) noexcept
{
    const char* t = r.text[std::size_t(column(r, l))];
    return t ? t : "";
}

inline const row* find(str id) noexcept
{
    const std::size_t i = std::size_t(id);
    return i < id_count ? &table[i] : nullptr;
}

// U+200E LEFT-TO-RIGHT MARK
inline constexpr std::string_view lrm = "\xE2\x80\x8E";
} // namespace detail

inline lang current() noexcept { return lang(detail::g_lang.load(std::memory_order_relaxed)); }
inline setting current_setting() noexcept { return setting(detail::g_setting.load(std::memory_order_relaxed)); }
inline std::int32_t system_language() noexcept { return detail::g_system.load(std::memory_order_relaxed); }
inline bool rtl() noexcept { return current() == lang::ar; }

// Text of 'id' in 'l' (empty -> English -> pt-BR). Static storage, never null.
inline const char* tr(str id, lang l) noexcept
{
    const detail::row* r = detail::find(id);
    return r ? detail::text(*r, l) : "";
}
inline const char* tr(str id) noexcept { return tr(id, current()); }

// Longest prefix of 'text' of at most 'max_bytes' bytes that does not cut a UTF-8 sequence
inline std::size_t utf8_fit(std::string_view text, std::size_t max_bytes) noexcept
{
    if (text.size() <= max_bytes) return text.size();
    std::size_t n = max_bytes;
    while (n > 0 && (static_cast<unsigned char>(text[n]) & 0xc0) == 0x80) --n;
    return n;
}

// One argument of trf/trs: a text (taken as it is), an integer (base 10, formatted in place) or a translated id.
// In Arabic a text or number argument is wrapped in U+200E LRM so "12.5 GB", paths, serials and "v1.05" stay one
// left-to-right run; raw() turns that off (translated text, Arabic names). Copyable; holds no pointer to itself.
class arg
{
public:
    arg(std::string_view t) noexcept : m_text(t) {}
    arg(const char* t) noexcept : m_text(t ? std::string_view(t) : std::string_view()) {}
    arg(const std::string& t) noexcept : m_text(t) {}
    // Translated text, resolved in the language of the template that receives it; never isolated
    arg(str id) noexcept : m_id(id), m_kind(kind::id) { isolate = false; }
    arg(int v) noexcept { set_signed(v); }
    arg(long v) noexcept { set_signed(v); }
    arg(long long v) noexcept { set_signed(v); }
    arg(unsigned v) noexcept { set_unsigned(v, false); }
    arg(unsigned long v) noexcept { set_unsigned(v, false); }
    arg(unsigned long long v) noexcept { set_unsigned(v, false); }
    // Ambiguous meaning: format the value explicitly (e.g. a "%.1f" string) or pass a text
    arg(bool) = delete;
    arg(char) = delete;
    arg(float) = delete;
    arg(double) = delete;
    arg(long double) = delete;

    std::string_view view(lang l) const noexcept
    {
        switch (m_kind)
        {
        case kind::number: return std::string_view(m_buffer, m_length);
        case kind::id: return tr(m_id, l);
        case kind::text: break;
        }
        return m_text;
    }

    bool isolate = true;

private:
    enum class kind : std::uint8_t { text, number, id };

    void set_unsigned(unsigned long long v, bool negative) noexcept
    {
        char digits[20];
        std::size_t n = 0;
        do
        {
            digits[n++] = char('0' + v % 10);
            v /= 10;
        } while (v);
        std::size_t length = 0;
        if (negative) m_buffer[length++] = '-';
        while (n) m_buffer[length++] = digits[--n];
        m_length = std::uint8_t(length);
        m_kind = kind::number;
    }
    void set_signed(long long v) noexcept
    {
        if (v < 0) set_unsigned(0ull - static_cast<unsigned long long>(v), true);
        else set_unsigned(static_cast<unsigned long long>(v), false);
    }

    std::string_view m_text;
    str m_id = str::count_;
    kind m_kind = kind::text;
    std::uint8_t m_length = 0;
    char m_buffer[24]{};
};

inline arg raw(std::string_view t) noexcept
{
    arg a(t);
    a.isolate = false;
    return a;
}

namespace detail
{
// Calls sink(piece) for each piece of 'pattern' with "{0}".."{9}" replaced by the arguments (missing -> nothing,
// extra ignored, "{{" -> '{'); 'isolate' wraps text/number arguments without Arabic letters in LRM. Stops when
// sink returns false. Allocation-free.
template <typename Sink>
void format(std::string_view pattern, const arg* args, std::size_t count, lang l, bool isolate, Sink&& sink) noexcept
{
    std::size_t i = 0;
    while (i < pattern.size())
    {
        const std::size_t brace = pattern.find('{', i);
        const std::size_t end = brace == std::string_view::npos ? pattern.size() : brace;
        if (end > i && !sink(pattern.substr(i, end - i))) return;
        if (end == pattern.size()) return;
        i = end;
        if (i + 1 < pattern.size() && pattern[i + 1] == '{')
        {
            if (!sink(pattern.substr(i, 1))) return;
            i += 2;
            continue;
        }
        if (i + 2 < pattern.size() && pattern[i + 1] >= '0' && pattern[i + 1] <= '9' && pattern[i + 2] == '}')
        {
            const std::size_t n = std::size_t(pattern[i + 1] - '0');
            i += 3;
            if (n >= count) continue;
            const std::string_view value = args[n].view(l);
            if (value.empty()) continue;
            const bool wrap = isolate && args[n].isolate && !arabic::has_arabic(value);
            if (wrap && !sink(lrm)) return;
            if (!sink(value)) return;
            if (wrap && !sink(lrm)) return;
            continue;
        }
        if (!sink(pattern.substr(i, 1))) return;
        ++i;
    }
}
} // namespace detail

// Template of 'id' in 'l' with the arguments substituted (see arg). Arguments are isolated only when the Arabic
// column served the template (a fallback to English stays plain).
inline std::string trf(lang l, str id, std::initializer_list<arg> args)
{
    const detail::row* r = detail::find(id);
    if (!r) return {};
    const lang used = detail::column(*r, l);
    const std::string_view pattern = detail::text(*r, l);
    std::string out;
    out.reserve(pattern.size() * 2);
    detail::format(pattern, args.begin(), args.size(), used, used == lang::ar, [&](std::string_view piece)
    {
        out.append(piece);
        return true;
    });
    return out;
}
inline std::string trf(str id, std::initializer_list<arg> args) { return trf(current(), id, args); }

// Allocation-free trf (fatal and signal paths, health thread, pads): writes at most size-1 bytes, never cuts a UTF-8
// sequence, always NUL-terminates when size > 0; returns the length written.
inline std::size_t trs(char* out, std::size_t size, lang l, str id, std::initializer_list<arg> args) noexcept
{
    if (!out || !size) return 0;
    const detail::row* r = detail::find(id);
    std::size_t length = 0;
    if (r)
    {
        const std::size_t cap = size - 1;
        const lang used = detail::column(*r, l);
        detail::format(detail::text(*r, l), args.begin(), args.size(), used, used == lang::ar, [&](std::string_view piece) noexcept
        {
            const std::size_t room = cap - length;
            const std::size_t n = piece.size() <= room ? piece.size() : utf8_fit(piece, room);
            if (n) std::memcpy(out + length, piece.data(), n);
            length += n;
            return n == piece.size();
        });
    }
    out[length] = '\0';
    return length;
}
inline std::size_t trs(char* out, std::size_t size, str id, std::initializer_list<arg> args) noexcept
{
    return trs(out, size, current(), id, args);
}
template <std::size_t N>
inline std::size_t trs(char (&out)[N], str id, std::initializer_list<arg> args) noexcept
{
    return trs(out, N, current(), id, args);
}

// --- interface setting, PS4 system language, game language (pure: the caller reads/writes the file and the system
// parameter, then calls apply() once before the first toast or XMB frame) ---

inline const char* lang_token(lang l) noexcept
{
    switch (l)
    {
    case lang::en: return "en";
    case lang::es: return "es";
    case lang::ar: return "ar";
    case lang::pt_br: break;
    }
    return "pt-BR";
}

// What ui-language.txt gets
inline const char* setting_token(setting s) noexcept
{
    switch (s)
    {
    case setting::pt_br: return "pt-BR";
    case setting::en: return "en";
    case setting::es: return "es";
    case setting::ar: return "ar";
    case setting::automatic: break;
    }
    return "auto";
}

// "auto", "pt-BR", "en", "es", "ar" and aliases; trims spaces, line ends and a UTF-8 BOM, ignores case and '_'/'-'.
// nullopt for anything else or more than setting_max_bytes (the caller then uses automatic and logs it).
inline std::optional<setting> parse_setting(std::string_view text) noexcept
{
    if (text.size() > setting_max_bytes) return std::nullopt;
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
    const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f'; };
    while (!text.empty() && space(text.front())) text.remove_prefix(1);
    while (!text.empty() && space(text.back())) text.remove_suffix(1);
    char folded[setting_max_bytes];
    std::size_t n = 0;
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        char c = text[i];
        if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
        else if (c == '_') c = '-';
        // Latin-1 capitals in UTF-8 (C3 80..C3 9E, not C3 97) to their small letters
        else if (static_cast<unsigned char>(c) >= 0x80 && static_cast<unsigned char>(c) <= 0x9e && static_cast<unsigned char>(c) != 0x97 && i > 0 &&
            static_cast<unsigned char>(text[i - 1]) == 0xc3)
            c = char(static_cast<unsigned char>(c) + 0x20);
        folded[n++] = c;
    }
    const std::string_view t(folded, n);
    struct alias
    {
        std::string_view name;
        setting value;
    };
    static constexpr alias aliases[] = {
        {"auto", setting::automatic}, {"automatic", setting::automatic}, {"automatico", setting::automatic},
        {"autom\xC3\xA1tico", setting::automatic}, {"system", setting::automatic}, {"sistema", setting::automatic},
        {"pt-br", setting::pt_br}, {"pt", setting::pt_br}, {"ptbr", setting::pt_br}, {"pt-pt", setting::pt_br}, {"por", setting::pt_br},
        {"portugues", setting::pt_br}, {"portugu\xC3\xAAs", setting::pt_br}, {"portuguese", setting::pt_br},
        {"en", setting::en}, {"en-us", setting::en}, {"en-gb", setting::en}, {"eng", setting::en}, {"english", setting::en},
        {"ingles", setting::en}, {"ingl\xC3\xAAs", setting::en}, {"ingl\xC3\xA9s", setting::en},
        {"es", setting::es}, {"es-es", setting::es}, {"es-419", setting::es}, {"es-la", setting::es}, {"es-mx", setting::es},
        {"spa", setting::es}, {"spanish", setting::es}, {"espanol", setting::es}, {"espa\xC3\xB1ol", setting::es},
        {"espanhol", setting::es},
        {"ar", setting::ar}, {"ara", setting::ar}, {"arabic", setting::ar}, {"arabe", setting::ar}, {"\xC3\xA1rabe", setting::ar},
        {"\xD8\xA7\xD9\x84\xD8\xB9\xD8\xB1\xD8\xA8\xD9\x8A\xD8\xA9", setting::ar},
    };
    for (const alias& a : aliases)
        if (a.name == t) return a.value;
    return std::nullopt;
}

// PS4 system language (ORBIS_SYSTEM_PARAM_LANG_*) to the interface language: Portuguese -> pt-BR, Spanish -> es,
// Arabic -> ar, any other known language -> English; unknown (-1, the call failed) -> pt-BR as before 0.39
inline lang from_system(std::int32_t ps4_lang) noexcept
{
    switch (ps4_lang)
    {
    case 7: case 17: return lang::pt_br;
    case 3: case 20: return lang::es;
    case 21: return lang::ar;
    default: break;
    }
    return ps4_lang < 0 ? lang::pt_br : lang::en;
}

inline lang resolve(setting s, std::int32_t ps4_lang) noexcept
{
    switch (s)
    {
    case setting::pt_br: return lang::pt_br;
    case setting::en: return lang::en;
    case setting::es: return lang::es;
    case setting::ar: return lang::ar;
    case setting::automatic: break;
    }
    return from_system(ps4_lang);
}

inline void apply(setting s, std::int32_t ps4_lang) noexcept
{
    if (std::uint8_t(s) > std::uint8_t(setting::ar)) s = setting::automatic;
    detail::g_setting.store(std::uint8_t(s), std::memory_order_relaxed);
    detail::g_system.store(ps4_lang, std::memory_order_relaxed);
    detail::g_lang.store(std::uint8_t(resolve(s, ps4_lang)), std::memory_order_relaxed);
}

// Name of a language in itself ("Português (Brasil)", "English", "Español", "العربية")
inline str endonym_id(lang l) noexcept
{
    switch (l)
    {
    case lang::en: return str::LANG_NAME_EN;
    case lang::es: return str::LANG_NAME_ES;
    case lang::ar: return str::LANG_NAME_AR;
    case lang::pt_br: break;
    }
    return str::LANG_NAME_PT;
}
inline const char* endonym(lang l) noexcept { return tr(endonym_id(l), lang::pt_br); }

// Value label of the language row in the current language: "Automático (Português (Brasil))", "English", ...
inline std::string setting_label(setting s)
{
    if (s == setting::automatic) return trf(str::LANG_AUTO_FMT, {raw(endonym(resolve(s, system_language())))});
    return endonym(resolve(s, -1));
}

// PS4 system language to the PS3 game language (CELL_SYSUTIL_LANG_*): 0..19 are the same languages, es-LA -> Spanish,
// fr-CA -> French, Arabic and languages the PS3 does not have -> English (US); unknown -> English (US)
inline std::int32_t cell_lang_from_ps4(std::int32_t ps4_lang) noexcept
{
    if (ps4_lang >= 0 && ps4_lang < cell_lang_count) return ps4_lang;
    if (ps4_lang == 20) return 3;
    if (ps4_lang == 22) return 2;
    return 1;
}

// Game language for an interface language chosen explicitly
inline std::int32_t cell_lang_for(lang l) noexcept
{
    switch (l)
    {
    case lang::pt_br: return 17;
    case lang::es: return 3;
    case lang::en:
    case lang::ar: break;
    }
    return 1;
}

// Automatic game language: the PS4 language while the interface follows it, else the interface language
inline std::int32_t game_language_auto() noexcept
{
    if (current_setting() == setting::automatic) return cell_lang_from_ps4(system_language());
    return cell_lang_for(current());
}

// GL_NAME_<n> of a CELL_SYSUTIL_LANG_* value (out of range -> English (US))
inline str game_language_name(std::int32_t cell_lang) noexcept
{
    if (cell_lang < 0 || cell_lang >= cell_lang_count) cell_lang = 1;
    return str(std::size_t(str::GL_NAME_0) + std::size_t(cell_lang));
}
} // namespace utils::ps4::i18n
