#pragma once

// 0.31: system on-screen keyboard (libSceImeDialog) for the XMB launcher (ps4_ime.cpp, frontend target).
// The dialog is drawn by the system over the app, so the launcher must keep presenting its own VideoOut frames
// while active() is true. No SCE headers here and nothing throws. Text is UTF-8 on this side; the conversion to
// the system's 16-bit text is done inside ps4_ime.cpp.

#include <cstddef>
#include <cstdint>
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
    waiting,    // a request() waits until the system allows the dialog
    not_opened, // a request() could not be opened (refused, or still not allowed after the time limit)
};

// When a queued dialog (request()) may be opened. The system refuses a dialog opened while the previous one is still
// closing: its overlay stays in front for about 0.3 s after a close, and sceImeDialogInit answers 0x80bc0030 or
// 0x80bc0008 meanwhile (seen on consoles when a form opened its next field from the close of the previous one). A
// queued dialog therefore waits until no dialog is open, the system UI is not in front and settle_us passed since the
// last close; a refusal with one of those two codes waits once more; a request still not open after timeout_us of
// waiting ends as not_opened. Pure, so the host can test the rule.
namespace deferred
{
inline constexpr std::uint64_t settle_us = 150'000;
inline constexpr std::uint64_t timeout_us = 3'000'000;
// Only time with the launcher in front counts: a pass that comes after the loop waited behind the system UI adds at
// most this much (the overlay of the dialog that just closed keeps the launcher there for a moment)
inline constexpr std::uint64_t step_cap_us = 100'000;
inline constexpr unsigned max_retries = 1;

// The two refusals of a dialog opened while the previous one is still closing
constexpr bool closing_refusal(std::int32_t code) noexcept
{
    const auto c = static_cast<std::uint32_t>(code);
    return c == 0x80bc0030u || c == 0x80bc0008u;
}

// The refusal that stops the option fallback at once: fewer options cannot help while the system is not ready, and a
// dialog that opens on a later fallback step would lose its options (a password would be visible while typing)
constexpr bool not_ready_refusal(std::int32_t code) noexcept
{
    return static_cast<std::uint32_t>(code) == 0x80bc0008u;
}

enum class step
{
    wait,    // not now: a dialog is open, the system UI is in front, or the last close is too recent
    open,    // try sceImeDialogInit now
    give_up, // timeout_us of waiting passed
};

struct gate
{
    std::uint64_t waited_us = 0;      // time waited with the launcher in front
    std::uint64_t last_pass_us = 0;   // the previous pass (0: none yet)
    std::uint64_t settle_from_us = 0; // the last close or the last refusal with a closing code (0: none)
    unsigned retries = 0;

    // A new request; 'closed_at_us' is the end of the last dialog (0: none closed yet)
    void start(std::uint64_t closed_at_us) noexcept
    {
        *this = {};
        settle_from_us = closed_at_us;
    }

    // One pass of the launcher loop at 'now_us'; 'busy': a dialog is open or the system UI is in front
    step next(std::uint64_t now_us, bool busy) noexcept
    {
        if (last_pass_us && now_us > last_pass_us)
        {
            const std::uint64_t gap = now_us - last_pass_us;
            waited_us += gap < step_cap_us ? gap : step_cap_us;
        }
        last_pass_us = now_us;
        if (waited_us >= timeout_us) return step::give_up;
        if (busy) return step::wait;
        if (settle_from_us && now_us >= settle_from_us && now_us - settle_from_us < settle_us) return step::wait;
        return step::open;
    }

    // sceImeDialogInit refused the attempt at 'now_us' with 'code': true = wait settle_us and try once more
    bool refused(std::uint64_t now_us, std::int32_t code) noexcept
    {
        if (!closing_refusal(code) || retries >= max_retries) return false;
        retries++;
        settle_from_us = now_us;
        return true;
    }
};
} // namespace deferred

// Opens the dialog for the initial user. 'title' is UTF-8 (cut at 127 UTF-16 units), 'max_len' the maximum number
// of characters (0 = the default of the kind; username and token are capped at 16, password at 128, email at
// 254, number at 24, hex at 40). 'initial' is the text the dialog starts with (UTF-8, cut at max_len characters;
// never pass a password). Returns false (and logs why) when a dialog is already open or the system refused to open one.
bool open(kind type, std::string_view title, unsigned max_len, std::string_view initial = {});

// The same dialog, queued instead of opened now: poll() opens it when the system allows it (the rule of 'deferred'
// above) and reports waiting until then, running once it is open, and not_opened when it was refused or the time
// limit passed. For a dialog that follows another one (a form's next field, the same field again after a typing
// error): opened at once from the close of the previous dialog, the system refuses it. Returns false (and logs why)
// when a dialog is open or another request waits. Launcher thread only, like open() and poll().
bool request(kind type, std::string_view title, unsigned max_len, std::string_view initial = {});

// True from a successful request() until poll() has opened it or reported not_opened.
bool pending();

// Drops a waiting request; nothing is reported for it.
void cancel_request();

// Call once per frame while active() or pending(). done, cancelled, failed and not_opened are reported once, after
// which poll() returns idle. 'out' is written only for done. For kind::password 'out' holds the plain text: hand it
// straight to ps4_rpcn::start with std::move, never log it or keep it in a member.
state poll(std::string& out);

// True from a successful open() (or from the moment poll() opened a request) until poll() has reported the result.
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
