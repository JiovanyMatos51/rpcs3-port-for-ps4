#pragma once

// 0.31: system on-screen keyboard (libSceImeDialog) for the XMB launcher (ps4_ime.cpp, frontend target).
// The dialog is drawn by the system over the app, so the launcher must keep presenting its own VideoOut frames
// while active() is true. No SCE headers here and nothing throws. Text is UTF-8 on this side; the conversion to
// the system's 16-bit text is done inside ps4_ime.cpp.

#include <cstddef>
#include <string>
#include <string_view>

namespace ps4_ime
{
enum class kind : unsigned
{
    text,     // general text
    username, // RPCN user name: Latin letters, digits, '-' and '_'
    password, // hidden input; the text buffer is wiped after use
    email,
    token,    // 16-character RPCN email token
    number,   // digits only (the system's number keyboard; falls back to Latin letters and digits when refused)
    hex,      // Latin letters and digits: hexadecimal values, numbers with a sign or decimals, dates
};

enum class state : unsigned
{
    idle,      // no dialog open
    running,   // the dialog is on screen
    done,      // confirmed; poll() filled 'out'
    cancelled, // closed by the player
    failed,    // the dialog could not be opened or ended with an error
};

// Opens the dialog for the initial user. 'title' is UTF-8 (cut at 127 UTF-16 units), 'max_len' the maximum number
// of characters (0 = the default of the kind; username and token are capped at 16, password at 128, email at
// 254, number at 24, hex at 40). 'initial' is the text the dialog starts with (UTF-8, cut at max_len characters;
// never pass a password). Returns false (and logs why) when a dialog is already open or the system refused to open one.
bool open(kind type, std::string_view title, unsigned max_len, std::string_view initial = {});

// Call once per frame while active(). done, cancelled and failed are reported once, after which the dialog is
// closed and poll() returns idle. 'out' is written only for done. For kind::password 'out' holds the plain text:
// hand it straight to ps4_rpcn::start with std::move, never log it or keep it in a member.
state poll(std::string& out);

// True from a successful open() until poll() has reported the result.
bool active();

// UTF-8 <-> UTF-16 for the dialog's text. Pure and never throwing (std::wstring_convert throws on bad input, and
// the frontend is built without exceptions); the host test uses them too.
namespace utf
{
// Writes 'in' as UTF-16 to out[0..cap-1] and terminates it. Invalid UTF-8 bytes become U+FFFD; a character that
// does not fit whole is left out, as is everything after it. Returns the units written, terminator excluded.
// Writes nothing when cap is 0.
inline std::size_t to_utf16(std::string_view in, char16_t* out, std::size_t cap) noexcept
{
    if (!cap) return 0;
    std::size_t n = 0;
    std::size_t i = 0;
    while (i < in.size())
    {
        const unsigned char lead = static_cast<unsigned char>(in[i]);
        char32_t cp = 0xFFFD;
        std::size_t len = 1;
        if (lead < 0x80)
        {
            cp = lead;
        }
        else
        {
            char32_t min = 0;
            if (lead >= 0xC2 && lead <= 0xDF)
            {
                len = 2;
                min = 0x80;
                cp = lead & 0x1Fu;
            }
            else if (lead >= 0xE0 && lead <= 0xEF)
            {
                len = 3;
                min = 0x800;
                cp = lead & 0x0Fu;
            }
            else if (lead >= 0xF0 && lead <= 0xF4)
            {
                len = 4;
                min = 0x10000;
                cp = lead & 0x07u;
            }
            bool ok = len > 1 && i + len <= in.size();
            for (std::size_t k = 1; ok && k < len; k++)
            {
                const unsigned char next = static_cast<unsigned char>(in[i + k]);
                ok = (next & 0xC0u) == 0x80u;
                cp = (cp << 6) | (next & 0x3Fu);
            }
            if (!ok || cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
            {
                cp = 0xFFFD; // skip only the lead byte; a stray continuation byte gets its own U+FFFD
                len = 1;
            }
        }

        const std::size_t units = cp >= 0x10000 ? 2 : 1;
        if (n + units > cap - 1) break;
        if (units == 2)
        {
            const char32_t v = cp - 0x10000;
            out[n++] = static_cast<char16_t>(0xD800 + (v >> 10));
            out[n++] = static_cast<char16_t>(0xDC00 + (v & 0x3FF));
        }
        else
        {
            out[n++] = static_cast<char16_t>(cp);
        }
        i += len;
    }
    out[n] = 0;
    return n;
}

// Appends up to 'len' units of 'in' (stopping at a 0 unit) to 'out' as UTF-8; unpaired surrogates become U+FFFD.
inline void to_utf8(const char16_t* in, std::size_t len, std::string& out)
{
    for (std::size_t i = 0; i < len && in[i]; i++)
    {
        char32_t cp = in[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < len && in[i + 1] >= 0xDC00 && in[i + 1] <= 0xDFFF)
        {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (static_cast<char32_t>(in[i + 1]) - 0xDC00);
            i++;
        }
        else if (cp >= 0xD800 && cp <= 0xDFFF)
        {
            cp = 0xFFFD;
        }

        if (cp < 0x80)
        {
            out += static_cast<char>(cp);
        }
        else if (cp < 0x800)
        {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
        else if (cp < 0x10000)
        {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
        else
        {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
}
} // namespace utf
} // namespace ps4_ime
