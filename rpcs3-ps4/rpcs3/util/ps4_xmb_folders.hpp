#pragma once

// Folders of the XMB 'Jogo' column (one level, as on the PS3): the model, its file format and the pure operations.
//
// File /data/rpcs3/xmb-folders.txt: UTF-8 without BOM, LF line ends, TAB-separated fields (a BOM and CR line ends are
// accepted on read). The first line is the header "RPCS3 XMB folders v1", then:
//     folder <id> <name>
//     game <folder id> <serial> <path> <title>
// - id: 1..999999, stable while the file lives (a new folder takes the highest id + 1);
// - name: 1..32 code points in the form clean_name() gives (no controls, no bidi embeddings, single spaces);
// - serial: empty or a TITLE_ID (four capital letters and five digits); anything else reads as empty;
// - path: portable form (portable_path: /mnt/usbN/x -> usb:/x, so a drive plugged into another port still matches),
//   absolute, at most 1024 bytes; title: the last title seen (texts about games not in the library), <= 128 bytes.
// Line order is the order of the folders and of each folder's games; a game line may come before its folder line.
// '#' lines and empty lines are ignored. Lines of an unknown kind and fields after the known ones are kept and written
// back, so a later version can add data that this one preserves. Limits: 200 folders, 5000 game lines, 2048 bytes per
// line, 1 MiB per file; a line that breaks a rule or a limit is left out and counted.
//
// Loading (choose): a missing main file means no folders (a backup alone is not used: the main file was never written
// or was removed on purpose). A header "v<N>" with N > 1 was written by a newer version: shown read only, never
// written, so going back to an older package cannot destroy it. Any other main file falls back to xmb-folders.txt.bak;
// before the next write the damaged file is renamed to xmb-folders.damaged.txt, so user data is never lost silently.
//
// Nothing here touches the file system: 'store' reads and writes only through the I/O object its caller passes, and
// only the three paths below. No operation deletes, moves or renames a game: folders hold references only.

#include "ps4_build_version.hpp"
#include "ps4_game_library.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace utils::ps4::xmb_folders
{
inline constexpr char file_path[] = "/data/rpcs3/xmb-folders.txt";
inline constexpr char backup_path[] = "/data/rpcs3/xmb-folders.txt.bak";
inline constexpr char damaged_path[] = "/data/rpcs3/xmb-folders.damaged.txt";
inline constexpr std::string_view header_v1 = "RPCS3 XMB folders v1";
inline constexpr std::string_view header_prefix = "RPCS3 XMB folders v";
inline constexpr std::size_t max_folders = 200, max_members = 5000, max_name_chars = 32, max_file_bytes = 1 << 20;
inline constexpr std::size_t max_line_bytes = 2048, max_path_bytes = 1024, max_title_bytes = 128;
inline constexpr std::uint32_t max_id = 999'999;

// Paths of the 'Jogo' column rows that stand for a folder (X opens it) and for the single row of an open folder that
// shows no game. They start with '@' like every row that is not a game.
inline constexpr std::string_view folder_row_prefix = "@folder:";
inline constexpr std::string_view empty_row_prefix = "@folder-empty:";

// 'extra': what follows the last known field of the line, starting with its TAB (empty when nothing does).
struct member
{
    std::string serial, path, title, extra;
    bool operator==(const member&) const = default;
};

struct folder
{
    std::uint32_t id = 0;
    std::string name, extra;
    std::vector<member> members;
    bool operator==(const folder&) const = default;
};

struct model
{
    std::vector<folder> folders;
    std::vector<std::string> unknown_lines; // written back after the known lines
    bool read_only = false;                 // written by a newer version: every operation is refused
    bool operator==(const model&) const = default;
};

enum class load_status { absent, ok, newer, damaged };

struct parsed
{
    model m;
    load_status status = load_status::absent;
    std::size_t dropped = 0;   // lines left out (a rule broken, a duplicate, over a limit)
    std::string first_problem; // "line N: why" of the first line left out, or why the file is not a folder file
    bool from_backup = false;  // choose(): the main file is damaged and the backup's folders are shown
};

// Why an operation changed nothing. A false result with 'none': the model already was as asked.
enum class refusal : std::uint8_t { none, read_only, folder_limit, member_limit, file_limit, empty_name, unknown_folder, not_a_game };

inline std::string_view refusal_text(refusal why)
{
    switch (why)
    {
    case refusal::none: return "none";
    case refusal::read_only: return "read only (written by a newer version)";
    case refusal::folder_limit: return "folder limit";
    case refusal::member_limit: return "game limit";
    case refusal::file_limit: return "file size limit";
    case refusal::empty_name: return "empty name";
    case refusal::unknown_folder: return "unknown folder";
    case refusal::not_a_game: return "not a game";
    }
    return "?";
}

namespace detail
{
inline void set(refusal* why, refusal value)
{
    if (why) *why = value;
}

inline bool refuse(refusal* why, refusal value)
{
    set(why, value);
    return false;
}

// Code point at 'at' (advanced past it), or -1 for a byte that does not start a valid sequence (one byte consumed).
// Overlong forms, surrogates and values above U+10FFFF are invalid.
inline std::int32_t next_code_point(std::string_view text, std::size_t& at)
{
    const auto byte = [&](std::size_t i) { return static_cast<unsigned>(static_cast<unsigned char>(text[i])); };
    const unsigned lead = byte(at);
    if (lead < 0x80)
    {
        ++at;
        return static_cast<std::int32_t>(lead);
    }
    std::size_t length = 0;
    std::uint32_t value = 0;
    std::uint32_t least = 0;
    if (lead >= 0xC2 && lead <= 0xDF)
    {
        length = 2;
        value = lead & 0x1F;
        least = 0x80;
    }
    else if (lead >= 0xE0 && lead <= 0xEF)
    {
        length = 3;
        value = lead & 0x0F;
        least = 0x800;
    }
    else if (lead >= 0xF0 && lead <= 0xF4)
    {
        length = 4;
        value = lead & 0x07;
        least = 0x10000;
    }
    if (!length || text.size() - at < length)
    {
        ++at;
        return -1;
    }
    for (std::size_t i = 1; i < length; ++i)
    {
        const unsigned next = byte(at + i);
        if ((next & 0xC0) != 0x80)
        {
            ++at;
            return -1;
        }
        value = (value << 6) | (next & 0x3F);
    }
    if (value < least || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF))
    {
        ++at;
        return -1;
    }
    at += length;
    return static_cast<std::int32_t>(value);
}

// Code points of 'text', or npos when it is not valid UTF-8.
inline std::size_t utf8_length(std::string_view text)
{
    std::size_t count = 0;
    for (std::size_t at = 0; at < text.size(); ++count)
        if (next_code_point(text, at) < 0) return std::string_view::npos;
    return count;
}

inline std::size_t utf8_size(std::uint32_t value)
{
    return value < 0x80 ? 1 : value < 0x800 ? 2 : value < 0x10000 ? 3 : 4;
}

inline void append_utf8(std::string& out, std::uint32_t value)
{
    const std::size_t size = utf8_size(value);
    if (size == 1)
    {
        out += static_cast<char>(value);
        return;
    }
    static constexpr unsigned lead[] = {0, 0, 0xC0, 0xE0, 0xF0};
    out += static_cast<char>(lead[size] | (value >> (6 * (size - 1))));
    for (std::size_t i = size - 1; i-- > 0;)
        out += static_cast<char>(0x80 | ((value >> (6 * i)) & 0x3F));
}

// Characters a folder name or a stored title never keeps: C0 and C1 controls (TAB, CR and LF included), DEL, the line
// and paragraph separators, and the bidi embeddings, overrides and isolates (they would reorder the rest of an XMB
// line). LRM and RLM stay: they only mark a direction.
inline bool dropped_char(std::uint32_t c)
{
    return c < 0x20 || (c >= 0x7F && c <= 0x9F) || c == 0x2028 || c == 0x2029 || (c >= 0x202A && c <= 0x202E) ||
        (c >= 0x2066 && c <= 0x2069);
}

// Valid UTF-8 without the dropped characters, with single spaces and no space at either end, cut on a code point
// boundary to at most 'max_chars' code points and 'max_bytes' bytes. Invalid bytes are left out. Applying it to its
// own result changes nothing.
inline std::string clean_text(std::string_view text, std::size_t max_chars, std::size_t max_bytes)
{
    std::string out;
    std::size_t chars = 0;
    bool space = false; // a space waits for the next kept character
    for (std::size_t at = 0; at < text.size();)
    {
        const std::int32_t c = next_code_point(text, at);
        if (c < 0 || dropped_char(static_cast<std::uint32_t>(c))) continue;
        if (c == ' ')
        {
            space = !out.empty();
            continue;
        }
        const std::size_t need_chars = space ? 2 : 1;
        const std::size_t need_bytes = (space ? 1 : 0) + utf8_size(static_cast<std::uint32_t>(c));
        if (chars + need_chars > max_chars || out.size() + need_bytes > max_bytes) break;
        if (space) out += ' ';
        append_utf8(out, static_cast<std::uint32_t>(c));
        chars += need_chars;
        space = false;
    }
    return out;
}

// Folder names are compared without ASCII case ("Luta" and "LUTA" are the same name).
inline bool same_name(std::string_view a, std::string_view b)
{
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        char x = a[i];
        char y = b[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x + ('a' - 'A'));
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y + ('a' - 'A'));
        if (x != y) return false;
    }
    return true;
}

// 1..6 decimal digits, value 1..max_id.
inline std::optional<std::uint32_t> parse_id(std::string_view text)
{
    if (text.empty() || text.size() > 6) return std::nullopt;
    std::uint32_t value = 0;
    for (const char c : text)
    {
        if (c < '0' || c > '9') return std::nullopt;
        value = value * 10 + static_cast<std::uint32_t>(c - '0');
    }
    if (value == 0 || value > max_id) return std::nullopt;
    return value;
}

// 1 for a version 1 header, N > 1 for a newer format, 0 for a line that is no folder file header.
inline std::uint32_t header_version(std::string_view line)
{
    if (!line.starts_with(header_prefix)) return 0;
    const std::string_view digits = line.substr(header_prefix.size());
    if (digits.empty() || digits.size() > 6) return 0;
    std::uint32_t value = 0;
    for (const char c : digits)
    {
        if (c < '0' || c > '9') return 0;
        value = value * 10 + static_cast<std::uint32_t>(c - '0');
    }
    return value;
}

// The first 'count' TAB-separated fields of 'line' (returns how many it has, at most 'count'); 'rest' receives what
// follows them, starting with its TAB.
inline std::size_t split_fields(std::string_view line, std::string_view* fields, std::size_t count, std::string_view& rest)
{
    std::size_t n = 0;
    std::size_t at = 0;
    rest = {};
    while (n < count)
    {
        const std::size_t tab = line.find('\t', at);
        fields[n++] = line.substr(at, tab == std::string_view::npos ? std::string_view::npos : tab - at);
        if (tab == std::string_view::npos) return n;
        at = tab + 1;
    }
    rest = line.substr(at - 1);
    return n;
}

// A field never holds TAB, CR or LF, and an extra part or a kept line never holds CR or LF: they would break the
// file's lines. Data read from the file cannot hold them; this keeps a model built in memory from doing so.
inline void put_field(std::string& out, std::string_view text)
{
    for (const char c : text) out += (c == '\t' || c == '\r' || c == '\n') ? ' ' : c;
}

inline void put_rest(std::string& out, std::string_view text)
{
    if (!text.empty() && text[0] != '\t') out += '\t';
    for (const char c : text) out += (c == '\r' || c == '\n') ? ' ' : c;
}

inline std::string without_cr(std::string_view text)
{
    std::string out(text);
    std::replace(out.begin(), out.end(), '\r', ' ');
    return out;
}

// Keeps the first problem by line number (passes over the file do not run in line order).
struct problem_list
{
    std::size_t dropped = 0;
    std::size_t line = 0;
    std::string text;

    void drop(std::size_t at, std::string_view why)
    {
        ++dropped;
        if (!text.empty() && at >= line) return;
        line = at;
        text = "line " + std::to_string(at) + ": " + std::string(why);
    }
};

// Every line of a written file fits the limits a later load enforces.
inline bool text_fits(std::string_view text)
{
    if (text.size() > max_file_bytes) return false;
    for (std::size_t at = 0; at < text.size();)
    {
        const std::size_t lf = text.find('\n', at);
        const std::size_t end = lf == std::string_view::npos ? text.size() : lf;
        if (end - at > max_line_bytes) return false;
        at = end + 1;
    }
    return true;
}

inline bool folder_game(std::string_view path)
{
    return file_kind(path) == game_kind::none;
}
}

inline bool valid_serial(std::string_view serial)
{
    if (serial.size() != 9) return false;
    for (std::size_t i = 0; i < 9; ++i)
        if (i < 4 ? (serial[i] < 'A' || serial[i] > 'Z') : (serial[i] < '0' || serial[i] > '9')) return false;
    return true;
}

// /mnt/usbN/rest -> usb:/rest (N = 0..7, the drives the library reads); every other path unchanged.
inline std::string portable_path(std::string_view path)
{
    if (usb_drive_of(path) < 0) return std::string(path);
    constexpr std::size_t mount = std::string_view("/mnt/usb0").size();
    return "usb:/" + std::string(path.size() > mount + 1 ? path.substr(mount + 1) : std::string_view{});
}

inline bool on_usb(std::string_view portable)
{
    return portable.starts_with("usb:/");
}

// The path a portable USB path has on drive 'drive' (0..7); any other path unchanged.
inline std::string drive_path(std::string_view portable, unsigned drive)
{
    if (!on_usb(portable)) return std::string(portable);
    return usb_mount(drive) + std::string(portable.substr(4));
}

// A path a game line may hold: absolute, at most 1024 bytes, no TAB, CR, LF or NUL.
inline bool valid_member_path(std::string_view portable)
{
    if (portable.empty() || portable.size() > max_path_bytes) return false;
    if (!(portable[0] == '/' || (on_usb(portable) && portable.size() > 5))) return false;
    return portable.find_first_of(std::string_view("\t\r\n\0", 4)) == std::string_view::npos;
}

inline std::string clean_name(std::string_view typed)
{
    return detail::clean_text(typed, max_name_chars, 4 * max_name_chars);
}

inline std::string clean_title(std::string_view title)
{
    return detail::clean_text(title, max_title_bytes, max_title_bytes);
}

// For log lines: at most 'max_bytes' bytes, cut on a code point boundary.
inline std::string clip(std::string_view text, std::size_t max_bytes)
{
    if (text.size() <= max_bytes) return std::string(text);
    std::size_t end = max_bytes;
    while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) --end;
    return std::string(text.substr(0, end));
}

inline std::optional<std::size_t> find_folder(const model& m, std::uint32_t id)
{
    for (std::size_t i = 0; i < m.folders.size(); ++i)
        if (m.folders[i].id == id) return i;
    return std::nullopt;
}

// The folder that holds the game at 'portable' (the first one, should a hand edit list it twice).
inline std::optional<std::size_t> folder_of(const model& m, std::string_view portable)
{
    for (std::size_t i = 0; i < m.folders.size(); ++i)
        for (const auto& x : m.folders[i].members)
            if (x.path == portable) return i;
    return std::nullopt;
}

inline std::size_t member_count(const model& m)
{
    std::size_t count = 0;
    for (const auto& f : m.folders) count += f.members.size();
    return count;
}

inline std::string folder_row_path(std::uint32_t id)
{
    return std::string(folder_row_prefix) + std::to_string(id);
}

inline std::string empty_row_path(std::uint32_t id)
{
    return std::string(empty_row_prefix) + std::to_string(id);
}

inline std::optional<std::uint32_t> folder_row_id(std::string_view path)
{
    return path.starts_with(folder_row_prefix) ? detail::parse_id(path.substr(folder_row_prefix.size())) : std::nullopt;
}

inline std::optional<std::uint32_t> empty_row_id(std::string_view path)
{
    return path.starts_with(empty_row_prefix) ? detail::parse_id(path.substr(empty_row_prefix.size())) : std::nullopt;
}

// 'name' (already cleaned) unless another folder than 'except_id' has it; then "name (2)", "name (3)", ... with the
// name cut so the result keeps at most 32 code points. 'except_id' 0 excepts none (ids start at 1).
inline std::string unique_name(const model& m, std::string name, std::uint32_t except_id)
{
    const auto taken = [&](std::string_view candidate)
    {
        for (const auto& f : m.folders)
            if (f.id != except_id && detail::same_name(f.name, candidate)) return true;
        return false;
    };
    if (!taken(name)) return name;
    for (std::size_t n = 2; n <= max_folders + 1; ++n)
    {
        const std::string suffix = " (" + std::to_string(n) + ")";
        std::string candidate = detail::clean_text(name, max_name_chars - suffix.size(), 4 * max_name_chars) + suffix;
        if (!taken(candidate)) return candidate;
    }
    return name; // not reached: at most max_folders folders exist
}

// Header check, then two passes (folders first, so a game line may come before its folder line), limits, and one
// line per game (the first folder in folder order keeps a game listed twice).
inline parsed parse(std::string_view text)
{
    parsed out;
    out.status = load_status::damaged;
    if (text.size() > max_file_bytes)
    {
        out.first_problem = "file larger than 1 MiB";
        return out;
    }
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);

    std::size_t at = 0;
    std::size_t number = 0;
    std::string_view line;
    const auto next_line = [&]()
    {
        if (at >= text.size()) return false;
        const std::size_t lf = text.find('\n', at);
        line = text.substr(at, lf == std::string_view::npos ? std::string_view::npos : lf - at);
        at = lf == std::string_view::npos ? text.size() : lf + 1;
        if (line.ends_with('\r')) line.remove_suffix(1);
        ++number;
        return true;
    };
    if (!next_line())
    {
        out.first_problem = "empty file";
        return out;
    }
    const std::uint32_t version = detail::header_version(line);
    if (!version)
    {
        out.first_problem = "line 1: not a folder file header";
        return out;
    }
    out.status = version == 1 ? load_status::ok : load_status::newer;
    out.m.read_only = version > 1;

    struct pending_game
    {
        std::size_t line;
        std::string_view id, serial, path, title, rest;
    };
    std::vector<pending_game> games;
    detail::problem_list problems;
    while (next_line())
    {
        if (line.size() > max_line_bytes)
        {
            problems.drop(number, "line longer than 2048 bytes");
            continue;
        }
        if (line.empty() || line[0] == '#') continue;
        const std::string_view kind = line.substr(0, line.find('\t'));
        std::string_view f[5];
        std::string_view rest;
        if (kind == "folder")
        {
            if (detail::split_fields(line, f, 3, rest) < 3)
            {
                problems.drop(number, "folder line without a name");
                continue;
            }
            const auto id = detail::parse_id(f[1]);
            if (!id)
            {
                problems.drop(number, "folder id not valid");
                continue;
            }
            if (find_folder(out.m, *id))
            {
                problems.drop(number, "folder id used twice");
                continue;
            }
            if (out.m.folders.size() >= max_folders)
            {
                problems.drop(number, "more than 200 folders");
                continue;
            }
            const std::size_t chars = detail::utf8_length(f[2]);
            std::string name = chars <= max_name_chars ? clean_name(f[2]) : std::string();
            if (name.empty())
            {
                problems.drop(number, chars == std::string_view::npos ? "folder name not valid UTF-8" : "folder name empty or longer than 32 characters");
                continue;
            }
            out.m.folders.push_back({*id, std::move(name), detail::without_cr(rest), {}});
        }
        else if (kind == "game")
        {
            const std::size_t n = detail::split_fields(line, f, 5, rest);
            if (n < 4)
            {
                problems.drop(number, "game line without a path");
                continue;
            }
            games.push_back({number, f[1], f[2], f[3], n > 4 ? f[4] : std::string_view{}, rest});
        }
        else out.m.unknown_lines.push_back(detail::without_cr(line));
    }

    std::vector<std::vector<std::size_t>> member_lines(out.m.folders.size());
    std::size_t members = 0;
    for (const auto& g : games)
    {
        const auto id = detail::parse_id(g.id);
        const auto index = id ? find_folder(out.m, *id) : std::nullopt;
        if (!index)
        {
            problems.drop(g.line, id ? "game of an unknown folder id" : "game folder id not valid");
            continue;
        }
        std::string path = portable_path(g.path);
        if (!valid_member_path(path))
        {
            problems.drop(g.line, "game path not valid");
            continue;
        }
        member x{valid_serial(g.serial) ? std::string(g.serial) : std::string(), std::move(path), clean_title(g.title), detail::without_cr(g.rest)};
        // The line as it is written back must fit too (a line without its title field gains one TAB).
        const std::size_t written = 4 + 1 + std::to_string(*id).size() + 1 + x.serial.size() + 1 + x.path.size() + 1 + x.title.size() + x.extra.size();
        if (written > max_line_bytes)
        {
            problems.drop(g.line, "line longer than 2048 bytes");
            continue;
        }
        if (members >= max_members)
        {
            problems.drop(g.line, "more than 5000 games");
            continue;
        }
        ++members;
        out.m.folders[*index].members.push_back(std::move(x));
        member_lines[*index].push_back(g.line);
    }

    // One line per game path, the first in folder order.
    struct reference
    {
        std::string_view path;
        std::size_t folder, index;
    };
    std::vector<reference> refs;
    refs.reserve(members);
    for (std::size_t fi = 0; fi < out.m.folders.size(); ++fi)
        for (std::size_t mi = 0; mi < out.m.folders[fi].members.size(); ++mi)
            refs.push_back({out.m.folders[fi].members[mi].path, fi, mi});
    std::stable_sort(refs.begin(), refs.end(), [](const reference& a, const reference& b) { return a.path < b.path; });
    std::vector<std::vector<char>> twice(out.m.folders.size());
    for (std::size_t fi = 0; fi < out.m.folders.size(); ++fi) twice[fi].assign(out.m.folders[fi].members.size(), 0);
    bool any_twice = false;
    for (std::size_t k = 1; k < refs.size(); ++k)
    {
        if (refs[k].path != refs[k - 1].path) continue;
        twice[refs[k].folder][refs[k].index] = 1;
        problems.drop(member_lines[refs[k].folder][refs[k].index], "the same game listed twice");
        any_twice = true;
    }
    refs.clear(); // the views end here, before the members move
    if (any_twice)
    {
        for (std::size_t fi = 0; fi < out.m.folders.size(); ++fi)
        {
            auto& list = out.m.folders[fi].members;
            std::vector<member> kept;
            kept.reserve(list.size());
            for (std::size_t mi = 0; mi < list.size(); ++mi)
                if (!twice[fi][mi]) kept.push_back(std::move(list[mi]));
            list = std::move(kept);
        }
    }
    out.dropped = problems.dropped;
    out.first_problem = std::move(problems.text);
    return out;
}

// Canonical text: header, one comment line, each folder line followed by its game lines, then the kept unknown lines.
// parse(serialize(m)).m == m for every model parse() returns (read_only aside: the header written is always v1).
inline std::string serialize(const model& m)
{
    std::string out;
    out.reserve(256 + 64 * m.folders.size() + 160 * member_count(m));
    out += header_v1;
    out += '\n';
    out += "# written by RPCS3 PORT FOR PS4 ";
    out += ps4_build_version;
    out += "; edit with care (tab-separated)\n";
    for (const auto& f : m.folders)
    {
        const std::string id = std::to_string(f.id);
        out += "folder\t";
        out += id;
        out += '\t';
        detail::put_field(out, f.name);
        detail::put_rest(out, f.extra);
        out += '\n';
        for (const auto& x : f.members)
        {
            out += "game\t";
            out += id;
            out += '\t';
            detail::put_field(out, x.serial);
            out += '\t';
            detail::put_field(out, x.path);
            out += '\t';
            detail::put_field(out, x.title);
            detail::put_rest(out, x.extra);
            out += '\n';
        }
    }
    for (const auto& line : m.unknown_lines)
    {
        for (const char c : line) out += (c == '\r' || c == '\n') ? ' ' : c;
        out += '\n';
    }
    return out;
}

// Which file the folders come from. 'main' nullopt: the file does not exist; a file that exists but cannot be read is
// passed as an empty text (damaged). 'backup' nullopt: absent or unreadable.
// - main absent: no folders, the backup is not used;
// - main version 1: its folders; main of a newer version: its folders, read only;
// - main damaged: the backup's folders when it is a folder file (from_backup; a newer backup stays read only), else
//   no folders. Either way the status stays 'damaged', and first_problem says why the main file was not used.
inline parsed choose(std::optional<std::string_view> main, std::optional<std::string_view> backup)
{
    if (!main) return {};
    parsed p = parse(*main);
    if (p.status != load_status::damaged) return p;
    if (backup)
    {
        parsed b = parse(*backup);
        if (b.status != load_status::damaged)
        {
            b.status = load_status::damaged;
            b.from_backup = true;
            b.first_problem = std::move(p.first_problem);
            return b;
        }
    }
    p.m = {};
    return p;
}

// One row of the 'Jogo' column as the library built it. Only games can be members: rows with a path that does not
// start with '@' (games, ISOs, PKG installers, ELF/SELF). 'folder_kind': the game is a folder, not a single file.
struct row
{
    std::string path, serial, title;
    bool is_game = false;
    bool folder_kind = false;
};

// The row of a column entry: a game when the path is not empty and does not start with '@', a folder game when the
// path does not end in a game file extension (.iso, .pkg, .elf, .self), the same test the library scan uses.
inline row make_row(std::string path, std::string serial, std::string title)
{
    const bool game = !path.empty() && path[0] != '@';
    const bool folder_kind = game && detail::folder_game(path);
    return {std::move(path), std::move(serial), std::move(title), game, folder_kind};
}

inline bool can_be_member(const row& r)
{
    return r.is_game && !r.path.empty() && r.path[0] != '@' && valid_member_path(portable_path(r.path));
}

struct assignment
{
    std::vector<int> folder_of_row;                     // per library row: folder index or -1
    std::vector<std::vector<std::size_t>> rows_of;      // per folder: library row indexes in member order
    std::vector<std::size_t> missing;                   // per folder: members with no row now
    bool changed = false;                               // adoptions or duplicate drops: save at the next change
    std::vector<std::uint32_t> ids;                     // per folder: its id
    std::vector<std::vector<std::size_t>> member_of;    // per folder: the member index of each rows_of entry
    std::vector<std::vector<std::string>> missing_paths; // per folder: portable paths of the members with no row now
    std::size_t adopted = 0;                            // members that followed their game to a new path
    std::size_t duplicates = 0;                         // members dropped: an earlier folder lists the same game
};

inline std::size_t missing_count(const assignment& a)
{
    std::size_t count = 0;
    for (const std::size_t n : a.missing) count += n;
    return count;
}

// Matches the members of 'm' with the library rows:
// 1. by portable path (two USB drives with the same relative path: the lowest drive index is the member);
// 2. a game listed by two folders stays in the first (in folder order); the later line is dropped from the model;
// 3. a member whose path matches no row is adopted by the row with the same TITLE_ID when exactly one row has it, that
//    row is in no folder, it is the same kind (folder or file) and storage_present(member path) says the storage of
//    the old path is readable and the game is not there (it was renamed or moved, e.g. over FTP). A game on a USB
//    drive that is only unplugged is never taken over by another copy. The new path is written at the next save;
// 4. members with no row are kept and counted: only forget_missing() removes them.
template <typename Present>
assignment assign(model& m, std::span<const row> rows, Present&& storage_present)
{
    assignment a;
    const std::size_t folders = m.folders.size();
    a.folder_of_row.assign(rows.size(), -1);
    a.rows_of.resize(folders);
    a.member_of.resize(folders);
    a.missing.assign(folders, 0);
    a.missing_paths.resize(folders);
    for (const auto& f : m.folders) a.ids.push_back(f.id);

    std::map<std::string, std::size_t, std::less<>> by_path;
    std::map<std::string, std::pair<std::size_t, std::size_t>, std::less<>> by_serial; // serial -> rows with it, last row
    for (std::size_t r = 0; r < rows.size(); ++r)
    {
        if (!can_be_member(rows[r])) continue;
        const auto [it, inserted] = by_path.try_emplace(portable_path(rows[r].path), r);
        if (!inserted && usb_drive_of(rows[r].path) < usb_drive_of(rows[it->second].path)) it->second = r;
        if (valid_serial(rows[r].serial))
        {
            auto& s = by_serial[rows[r].serial];
            ++s.first;
            s.second = r;
        }
    }

    std::vector<char> claimed(rows.size(), 0);
    for (auto& f : m.folders)
    {
        std::vector<member> kept;
        kept.reserve(f.members.size());
        for (auto& x : f.members)
        {
            if (const auto it = by_path.find(x.path); it != by_path.end())
            {
                if (claimed[it->second])
                {
                    ++a.duplicates;
                    continue;
                }
                claimed[it->second] = 1;
            }
            kept.push_back(std::move(x));
        }
        f.members = std::move(kept);
    }

    for (auto& f : m.folders)
        for (auto& x : f.members)
        {
            if (by_path.find(x.path) != by_path.end() || !valid_serial(x.serial)) continue;
            const auto s = by_serial.find(x.serial);
            if (s == by_serial.end() || s->second.first != 1) continue;
            const std::size_t r = s->second.second;
            if (claimed[r] || rows[r].folder_kind != detail::folder_game(x.path)) continue;
            std::string path = portable_path(rows[r].path);
            if (by_path.find(path)->second != r) continue; // another drive's copy is the row of that path
            if (!storage_present(std::string_view(x.path))) continue;
            x.path = std::move(path);
            if (std::string title = clean_title(rows[r].title); !title.empty()) x.title = std::move(title);
            claimed[r] = 1;
            ++a.adopted;
        }

    for (std::size_t fi = 0; fi < folders; ++fi)
    {
        const auto& list = m.folders[fi].members;
        for (std::size_t mi = 0; mi < list.size(); ++mi)
        {
            const auto it = by_path.find(list[mi].path);
            if (it == by_path.end())
            {
                ++a.missing[fi];
                a.missing_paths[fi].push_back(list[mi].path);
                continue;
            }
            a.folder_of_row[it->second] = static_cast<int>(fi);
            a.rows_of[fi].push_back(it->second);
            a.member_of[fi].push_back(mi);
        }
    }
    a.changed = a.adopted || a.duplicates;
    return a;
}

// One row of the projected 'Jogo' column: a library row (>= 0), folder k (-1 - k) or the row of an open folder that
// shows no game (empty_row).
inline constexpr int empty_row = std::numeric_limits<int>::min();

struct slot
{
    int value = 0;
    bool is_row() const { return value >= 0; }
    bool is_folder() const { return value < 0 && value != empty_row; }
    bool is_empty_row() const { return value == empty_row; }
    std::size_t row_index() const { return static_cast<std::size_t>(value); }
    std::size_t folder_index() const { return static_cast<std::size_t>(-1 - static_cast<long long>(value)); }
    bool operator==(const slot&) const = default;
};

inline slot row_slot(std::size_t r)
{
    return {static_cast<int>(r)};
}

inline slot folder_slot(std::size_t k)
{
    return {-1 - static_cast<int>(k)};
}

// Top level ('open_folder' nullopt, or an index that no longer exists): the leading '@' rows (saved data, licenses;
// the PS3 disc row, inserted later at index 2, lands right after them), the folders in their order, then every row
// in no folder in library order (games, then the notes). With no folder this is the library column unchanged.
// Inside folder k: its games in member order, or one empty_row when none is in the library now.
inline std::vector<slot> project(const assignment& a, std::span<const row> rows, std::optional<std::size_t> open_folder)
{
    std::vector<slot> out;
    if (open_folder && *open_folder < a.rows_of.size())
    {
        for (const std::size_t r : a.rows_of[*open_folder]) out.push_back(row_slot(r));
        if (out.empty()) out.push_back({empty_row});
        return out;
    }
    out.reserve(rows.size() + a.rows_of.size());
    std::size_t r = 0;
    for (; r < rows.size() && rows[r].path.starts_with('@'); ++r) out.push_back(row_slot(r));
    for (std::size_t k = 0; k < a.rows_of.size(); ++k) out.push_back(folder_slot(k));
    for (; r < rows.size(); ++r)
        if (r >= a.folder_of_row.size() || a.folder_of_row[r] < 0) out.push_back(row_slot(r));
    return out;
}

// Operations. Each returns true when it changed the model (save it then); false leaves the model as it was, with the
// reason in 'why' (refusal::none: it already was as asked). None touches a file.
namespace detail
{
inline std::uint32_t next_id(const model& m)
{
    std::uint32_t highest = 0;
    for (const auto& f : m.folders) highest = std::max(highest, f.id);
    if (highest < max_id) return highest + 1;
    for (std::uint32_t id = 1; id <= max_id; ++id) // every id up to the highest was handed out once: reuse a free one
        if (!find_folder(m, id)) return id;
    return 0;
}

inline member member_of_row(const row& game)
{
    return {valid_serial(game.serial) ? game.serial : std::string(), portable_path(game.path), clean_title(game.title), {}};
}

// The member taken out of the folders keeps its extra fields; its serial and title follow the library row.
inline void refresh(member& x, const row& game)
{
    if (valid_serial(game.serial)) x.serial = game.serial;
    if (std::string title = clean_title(game.title); !title.empty()) x.title = std::move(title);
}

// 'index' < list.size(): moves the element by 'step' places, clamped to the first and the last place.
template <typename List>
bool move_within(List& list, std::size_t index, int step)
{
    const long long last = static_cast<long long>(list.size()) - 1;
    const long long target = std::clamp(static_cast<long long>(index) + step, 0LL, last);
    const auto from = static_cast<std::ptrdiff_t>(index);
    const auto to = static_cast<std::ptrdiff_t>(target);
    if (to == from) return false;
    const auto begin = list.begin();
    if (to < from) std::rotate(begin + to, begin + from, begin + from + 1);
    else std::rotate(begin + from, begin + from + 1, begin + to + 1);
    return true;
}
}

// A new folder after the last one, named 'name' (cleaned, made unique); 'id' receives its id.
inline bool create(model& m, std::string name, std::uint32_t& id, refusal* why = nullptr)
{
    detail::set(why, refusal::none);
    if (m.read_only) return detail::refuse(why, refusal::read_only);
    if (m.folders.size() >= max_folders) return detail::refuse(why, refusal::folder_limit);
    name = clean_name(name);
    if (name.empty()) return detail::refuse(why, refusal::empty_name);
    const std::uint32_t next = detail::next_id(m);
    if (!next) return detail::refuse(why, refusal::folder_limit);
    id = next;
    m.folders.push_back({next, unique_name(m, std::move(name), 0), {}, {}});
    return true;
}

inline bool rename(model& m, std::uint32_t id, std::string name, refusal* why = nullptr)
{
    detail::set(why, refusal::none);
    if (m.read_only) return detail::refuse(why, refusal::read_only);
    const auto at = find_folder(m, id);
    if (!at) return detail::refuse(why, refusal::unknown_folder);
    name = clean_name(name);
    if (name.empty()) return detail::refuse(why, refusal::empty_name);
    name = unique_name(m, std::move(name), id);
    if (m.folders[*at].name == name) return false;
    m.folders[*at].name = std::move(name);
    return true;
}

// Removes the folder and its game lines (the games go back to the main list; no game file is touched). Returns how
// many game lines it held, present in the library or not (the shown count is the assignment's rows_of size); a
// refusal returns 0 with the reason in 'why'.
inline std::size_t remove_folder(model& m, std::uint32_t id, refusal* why = nullptr)
{
    detail::set(why, refusal::none);
    const auto at = m.read_only ? std::nullopt : find_folder(m, id);
    if (!at)
    {
        detail::set(why, m.read_only ? refusal::read_only : refusal::unknown_folder);
        return 0;
    }
    const std::size_t count = m.folders[*at].members.size();
    m.folders.erase(m.folders.begin() + static_cast<std::ptrdiff_t>(*at));
    return count;
}

// Puts 'game' at the end of folder 'folder' (out of any other one), or out of every folder for nullopt.
inline bool move_to(model& m, const row& game, std::optional<std::uint32_t> folder, refusal* why = nullptr)
{
    detail::set(why, refusal::none);
    if (m.read_only) return detail::refuse(why, refusal::read_only);
    if (!can_be_member(game)) return detail::refuse(why, refusal::not_a_game);
    std::optional<std::size_t> target;
    if (folder)
    {
        target = find_folder(m, *folder);
        if (!target) return detail::refuse(why, refusal::unknown_folder);
    }
    const std::string path = portable_path(game.path);
    const auto current = folder_of(m, path);
    std::size_t lines = 0;
    for (const auto& f : m.folders)
        for (const auto& x : f.members)
            if (x.path == path) ++lines;
    // Already where it was asked to be, listed once: nothing changes and its position is kept.
    if (current == target && lines <= 1) return false;
    if (!current && member_count(m) >= max_members) return detail::refuse(why, refusal::member_limit);
    member moved = detail::member_of_row(game);
    if (current)
    {
        for (const auto& x : m.folders[*current].members)
            if (x.path == path)
            {
                moved = x;
                break;
            }
        detail::refresh(moved, game);
    }
    for (auto& f : m.folders) std::erase_if(f.members, [&](const member& x) { return x.path == path; });
    if (target) m.folders[*target].members.push_back(std::move(moved));
    return true;
}

// Result of the add-games checklist of folder 'id': 'listed' are the games the list showed (the library),
// 'checked' the ones marked. Marked games are members afterwards (taken out of other folders, appended in the list's
// order); unmarked listed games leave the folder; members that were not listed (not in the library now) stay.
inline bool set_members(model& m, std::uint32_t id, std::span<const row> checked, std::span<const row> listed, refusal* why = nullptr)
{
    detail::set(why, refusal::none);
    if (m.read_only) return detail::refuse(why, refusal::read_only);
    const auto target = find_folder(m, id);
    if (!target) return detail::refuse(why, refusal::unknown_folder);
    std::set<std::string, std::less<>> shown;
    std::set<std::string, std::less<>> marked;
    for (const row& r : listed)
        if (can_be_member(r)) shown.insert(portable_path(r.path));
    for (const row& r : checked)
        if (can_be_member(r)) marked.insert(portable_path(r.path));

    model next = m;
    std::vector<member> kept;
    std::set<std::string, std::less<>> in_folder;
    for (const auto& x : next.folders[*target].members)
        if ((!shown.contains(x.path) || marked.contains(x.path)) && in_folder.insert(x.path).second) kept.push_back(x);

    std::map<std::string, member, std::less<>> elsewhere; // first line of each game in the other folders
    for (std::size_t fi = 0; fi < next.folders.size(); ++fi)
        if (fi != *target)
            for (const auto& x : next.folders[fi].members) elsewhere.try_emplace(x.path, x);
    std::set<std::string, std::less<>> taken;
    for (const row& r : checked)
    {
        if (!can_be_member(r)) continue;
        std::string path = portable_path(r.path);
        if (in_folder.contains(path)) continue;
        member x = detail::member_of_row(r);
        if (const auto it = elsewhere.find(path); it != elsewhere.end())
        {
            x = it->second;
            detail::refresh(x, r);
            taken.insert(path);
        }
        in_folder.insert(path);
        kept.push_back(std::move(x));
    }
    next.folders[*target].members = std::move(kept);
    for (std::size_t fi = 0; fi < next.folders.size(); ++fi)
        if (fi != *target) std::erase_if(next.folders[fi].members, [&](const member& x) { return taken.contains(x.path); });
    if (member_count(next) > max_members) return detail::refuse(why, refusal::member_limit);
    if (next == m) return false;
    m = std::move(next);
    return true;
}

// Moves folder 'index' by 'step' places (negative: up), stopping at the first and the last place.
inline bool move_folder(model& m, std::size_t index, int step, refusal* why = nullptr)
{
    detail::set(why, refusal::none);
    if (m.read_only) return detail::refuse(why, refusal::read_only);
    if (index >= m.folders.size()) return detail::refuse(why, refusal::unknown_folder);
    return detail::move_within(m.folders, index, step);
}

// Moves member 'index' of folder 'id' (an index into its members: assignment::member_of maps shown rows) by 'step'.
inline bool move_member(model& m, std::uint32_t id, std::size_t index, int step, refusal* why = nullptr)
{
    detail::set(why, refusal::none);
    if (m.read_only) return detail::refuse(why, refusal::read_only);
    const auto at = find_folder(m, id);
    if (!at || index >= m.folders[*at].members.size()) return detail::refuse(why, refusal::unknown_folder);
    return detail::move_within(m.folders[*at].members, index, step);
}

// The forget-games-not-in-the-library action: removes the members of folder 'id' that 'a' found without a row (by
// path, so a model read again from the file loses only those). Returns how many lines it removed.
inline std::size_t forget_missing(model& m, std::uint32_t id, const assignment& a, refusal* why = nullptr)
{
    detail::set(why, refusal::none);
    const auto at = m.read_only ? std::nullopt : find_folder(m, id);
    if (!at)
    {
        detail::set(why, m.read_only ? refusal::read_only : refusal::unknown_folder);
        return 0;
    }
    const auto k = std::find(a.ids.begin(), a.ids.end(), id);
    if (k == a.ids.end() || static_cast<std::size_t>(k - a.ids.begin()) >= a.missing_paths.size()) return 0;
    const auto& gone = a.missing_paths[static_cast<std::size_t>(k - a.ids.begin())];
    auto& list = m.folders[*at].members;
    const std::size_t before = list.size();
    std::erase_if(list, [&](const member& x) { return std::find(gone.begin(), gone.end(), x.path) != gone.end(); });
    return before - list.size();
}

// Loading and saving through the launcher's I/O. The I/O object supplies:
//   file_read read(std::string_view path, std::size_t limit)   the whole file (larger than 'limit': unreadable)
//   stamp stat(std::string_view path)                          a default stamp when the file does not exist
//   bool write(std::string_view path, std::string_view text)   replaces the file atomically (temporary file + rename)
//   bool rename(std::string_view from, std::string_view to)    replaces 'to'
// Only file_path, backup_path and damaged_path are passed to it.
struct stamp
{
    bool exists = false;
    std::uint64_t size = 0;
    std::int64_t mtime = 0;
    bool operator==(const stamp&) const = default;
};

enum class read_status { absent, unreadable, ok };

struct file_read
{
    read_status status = read_status::absent;
    std::string text;
    stamp at;
};

enum class save_status { saved, unchanged, refused, failed };

struct save_result
{
    save_status status = save_status::unchanged;
    refusal why = refusal::none;  // refused: the reason
    bool reloaded = false;        // the main file had changed outside the emulator: read again, the operation redone
    std::string_view failed_step; // failed: "backup", "damaged copy" or "main file" (the change stays in memory)
    std::size_t bytes = 0;        // saved: size of the main file
};

struct store
{
    // The folders shown, and how the last load went.
    model m;
    load_status status = load_status::absent;
    bool from_backup = false;
    std::size_t dropped = 0;
    std::string first_problem;

    // What the next save needs.
    stamp main_at;                        // the main file as last read or written (a default stamp: it did not exist)
    std::optional<std::string> good_text; // the last good main text: copied to the backup before the main file changes
    bool backup_current = false;          // the backup already holds good_text
    bool keep_damaged = false;            // the main file is damaged: rename it to damaged_path before writing

    // Reads the main file; the backup only when the main file exists and is no folder file. A user who never created a
    // folder costs one failed read.
    template <typename Io>
    void load(Io& io)
    {
        const file_read main = io.read(file_path, max_file_bytes);
        std::optional<file_read> backup;
        parsed p;
        if (main.status != read_status::absent)
        {
            const std::string_view main_text = main.status == read_status::ok ? std::string_view(main.text) : std::string_view();
            p = parse(main_text);
            if (p.status == load_status::damaged)
            {
                backup = io.read(backup_path, max_file_bytes);
                p = choose(main_text, backup->status == read_status::ok ? std::optional<std::string_view>(backup->text) : std::nullopt);
                if (main.status == read_status::unreadable) p.first_problem = "the file could not be read";
            }
        }
        m = std::move(p.m);
        status = p.status;
        from_backup = p.from_backup;
        dropped = p.dropped;
        first_problem = std::move(p.first_problem);
        main_at = main.status == read_status::absent ? stamp{} : main.at;
        keep_damaged = status == load_status::damaged;
        backup_current = false;
        good_text.reset();
        if (status == load_status::ok || status == load_status::newer) good_text = main.text;
        else if (from_backup)
        {
            good_text = backup->text;
            backup_current = true;
        }
    }

    // Applies 'op' (bool(model&, refusal&), one of the operations above) and writes the result: the backup first (the
    // previous good text), then the damaged main file kept as damaged_path, then the main file. When the main file
    // changed outside the emulator since it was read (FTP), it is read again and 'op' runs on that model. Nothing is
    // read or written for a read-only model, a refusal or no change. After a failed write the change stays in memory.
    // The caller assigns and projects again afterwards (indexes of the old model no longer apply).
    template <typename Io, typename Op>
    save_result apply(Io& io, Op&& op)
    {
        save_result r;
        if (m.read_only)
        {
            r.status = save_status::refused;
            r.why = refusal::read_only;
            return r;
        }
        if (!(io.stat(file_path) == main_at))
        {
            load(io);
            r.reloaded = true;
            if (m.read_only)
            {
                r.status = save_status::refused;
                r.why = refusal::read_only;
                return r;
            }
        }
        model next = m;
        refusal why = refusal::none;
        if (!op(next, why))
        {
            r.status = why == refusal::none ? save_status::unchanged : save_status::refused;
            r.why = why;
            return r;
        }
        std::string text = serialize(next);
        if (!detail::text_fits(text))
        {
            r.status = save_status::refused;
            r.why = refusal::file_limit;
            return r;
        }
        m = std::move(next);
        r.status = save_status::failed;
        if (good_text && !backup_current)
        {
            if (!io.write(backup_path, *good_text))
            {
                r.failed_step = "backup";
                return r;
            }
            backup_current = true;
        }
        const bool keep = keep_damaged && main_at.exists;
        if (keep && !io.rename(file_path, damaged_path))
        {
            r.failed_step = "damaged copy";
            return r;
        }
        if (!io.write(file_path, text))
        {
            // The damaged file goes back to its name, so the next start still shows the backup's folders.
            if (keep) io.rename(damaged_path, file_path);
            r.failed_step = "main file";
            return r;
        }
        keep_damaged = false;
        r.status = save_status::saved;
        r.bytes = text.size();
        good_text = std::move(text);
        backup_current = false;
        main_at = io.stat(file_path);
        return r;
    }
};
}
