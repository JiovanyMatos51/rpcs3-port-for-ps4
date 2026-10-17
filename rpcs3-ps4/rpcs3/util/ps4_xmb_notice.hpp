#pragma once

// Notices from the emulator core to the XMB (the text passed to ps4_select_game): a kind, flags and a detail, so the
// launcher builds the dialog in the interface language instead of comparing pt-BR text.
//   encoded form: '\x1d' <kind> [flags] '\x1e' <detail>      flags: 'r' = restarting the application is advised
// Text without the leading '\x1d' decodes as kind::text with the whole text as detail (older producers keep working).

#include <cstddef>
#include <string>
#include <string_view>

namespace utils::ps4::xmb_notice
{
enum class kind : char
{
    text = 'T',             // detail = the message line, already in the interface language
    disc_removed = 'D',     // the game's PS3 disc left the drive
    emulator_error = 'E',   // a thread ended in a fatal error; detail = boot-next note
    emulator_restart = 'R', // the emulator must restart after the previous error; detail = boot-next note
    clean_process = 'C',    // the clean-process rule refused a boot; detail = refusal " · " boot-next note
    vram_stop = 'V',        // the game was stopped: GPU window exhausted
    heap_stop = 'H',        // the game was stopped: CPU heap exhausted
    boot_next = 'N',        // reopened to boot the game of boot-next.txt; detail = game name
    firmware_missing = 'F', // boot failed: PS3 firmware not installed
    firmware_old = 'O',     // boot failed: PS3 firmware too old
    previous_fatal = 'P',   // the previous session ended in a fatal error; detail = its reason
};

inline constexpr char marker = '\x1d';
inline constexpr char separator = '\x1e';
inline constexpr char flag_restart = 'r';

inline bool valid_kind(char c) noexcept
{
    switch (kind(c))
    {
    case kind::text:
    case kind::disc_removed:
    case kind::emulator_error:
    case kind::emulator_restart:
    case kind::clean_process:
    case kind::vram_stop:
    case kind::heap_stop:
    case kind::boot_next:
    case kind::firmware_missing:
    case kind::firmware_old:
    case kind::previous_fatal: return true;
    }
    return false;
}

inline const char* kind_name(kind k) noexcept
{
    switch (k)
    {
    case kind::text: return "text";
    case kind::disc_removed: return "disc_removed";
    case kind::emulator_error: return "emulator_error";
    case kind::emulator_restart: return "emulator_restart";
    case kind::clean_process: return "clean_process";
    case kind::vram_stop: return "vram_stop";
    case kind::heap_stop: return "heap_stop";
    case kind::boot_next: return "boot_next";
    case kind::firmware_missing: return "firmware_missing";
    case kind::firmware_old: return "firmware_old";
    case kind::previous_fatal: return "previous_fatal";
    }
    return "unknown";
}

inline bool is_encoded(std::string_view text) noexcept { return !text.empty() && text.front() == marker; }

// The marker and separator bytes are dropped from the detail, so a detail can never forge a second notice
inline std::string encode(kind k, std::string_view detail = {}, bool restart_advised = false)
{
    std::string out;
    out.reserve(detail.size() + 4);
    out += marker;
    out += valid_kind(char(k)) ? char(k) : char(kind::text);
    if (restart_advised) out += flag_restart;
    out += separator;
    for (const char c : detail)
        if (c != marker && c != separator) out += c;
    return out;
}

struct decoded
{
    kind k = kind::text;
    bool restart_advised = false;
    std::string detail;
};

// Unknown kinds decode as text (detail kept); unknown flags are ignored. A marker without a separator is text with
// the control bytes removed.
inline decoded decode(std::string_view text)
{
    decoded out;
    if (!is_encoded(text))
    {
        out.detail = std::string(text);
        return out;
    }
    const std::size_t sep = text.find(separator, 1);
    if (sep == std::string_view::npos || sep < 2)
    {
        for (const char c : text)
            if (c != marker && c != separator) out.detail += c;
        return out;
    }
    if (valid_kind(text[1])) out.k = kind(text[1]);
    for (const char f : text.substr(2, sep - 2))
        if (f == flag_restart) out.restart_advised = true;
    out.detail = std::string(text.substr(sep + 1));
    return out;
}

// For startup.log lines: "[vram_stop, restart advised] detail" (plain text unchanged)
inline std::string describe(std::string_view text)
{
    if (!is_encoded(text)) return std::string(text);
    const decoded d = decode(text);
    std::string out = "[";
    out += kind_name(d.k);
    if (d.restart_advised) out += ", restart advised";
    out += "] ";
    out += d.detail;
    return out;
}
} // namespace utils::ps4::xmb_notice
