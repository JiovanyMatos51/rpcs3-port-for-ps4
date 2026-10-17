#pragma once

// 0.33: game patches on the console, as 'Manage Game Patches' does on the PC RPCS3.
//
// The boot runs the original patch engine (Utilities/bin_patch.cpp): it reads /data/rpcs3/patches/patch.yml,
// imported_patch.yml and <TITLE_ID>_patch.yml and applies what /data/rpcs3/patch_config.yml enables for the game's
// serial and app version. This header gives the XMB the rest of the PC dialog:
//   - the patches of one game, chosen the way patch_engine::apply chooses them at boot, with their state, author
//     and notes; the switch writes patch_config.yml through patch_engine::save_config (same file and format as the
//     PC, a temporary file renamed over the old one) and reads it back;
//   - the import of patch files from USB, /data/rpcs3/import or /data/rpcs3, each validated by the engine before a file is
//     replaced (the old one is kept as .bak);
//   - the download of the latest patch.yml from rpcs3.net (the PC request), over TLS verified against the Mozilla
//     root certificates shipped in the package (certs/cacert.pem) plus the SHA-256 the server sends with the file.
//     Without that file the download is unavailable; verification is never turned off.
//
// Layout: the text and JSON helpers need only the standard library, the list logic only the patch_engine types
// (both host tested in ps4-port/tests/patches_test.cpp); file and network access is PS4 only.

#include "util/types.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Utilities/bin_patch.h"
#include "util/ps4_i18n.hpp"
#include "util/ps4_recommended_patches.hpp"

namespace utils::ps4::patches
{
// ---------------------------------------------------------------------------------------------------------------
// Text

inline std::string lower(std::string_view text)
{
    std::string out(text);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return out;
}

// Whitespace runs (line breaks of multi-line notes included) as one space, without leading or trailing space.
inline std::string flatten(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    bool space = false;
    for (const char c : text)
    {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
        {
            space = !out.empty();
            continue;
        }
        if (space) out += ' ';
        space = false;
        out += c;
    }
    return out;
}

// UTF-8 text cut to at most 'limit' bytes on a character boundary, "..." marking the cut.
inline std::string clip(std::string text, std::size_t limit)
{
    if (text.size() <= limit) return text;
    std::size_t cut = limit > 3 ? limit - 3 : 0;
    while (cut && (static_cast<unsigned char>(text[cut]) & 0xc0) == 0x80) --cut;
    text.resize(cut);
    text += "...";
    return text;
}

// The parts (strings, string views, literals) one after the other.
template <typename... Parts>
inline std::string concat(const Parts&... parts)
{
    std::string out;
    ((out += std::string_view(parts)), ...);
    return out;
}

// Page 'index' (wrapping around) of 'text' split into parts of at most 'width' bytes, broken at a space in the second
// half of a part when there is one and never inside a UTF-8 character. 'count' receives the number of pages (>= 1).
inline std::string page(std::string_view text, std::size_t index, std::size_t width, std::size_t& count)
{
    std::vector<std::string_view> pages;
    width = std::max<std::size_t>(width, 8);
    while (!text.empty())
    {
        std::size_t cut = std::min(text.size(), width);
        if (cut < text.size())
        {
            while (cut && (static_cast<unsigned char>(text[cut]) & 0xc0) == 0x80) --cut;
            // A part that already ends before a space keeps its last word
            if (text[cut] != ' ')
                if (const std::size_t space = text.substr(0, cut).rfind(' '); space != std::string_view::npos && space >= cut / 2) cut = space;
            if (!cut) cut = std::min(text.size(), width);
        }
        pages.push_back(text.substr(0, cut));
        text.remove_prefix(cut);
        while (!text.empty() && text.front() == ' ') text.remove_prefix(1);
    }
    count = std::max<std::size_t>(pages.size(), 1);
    return pages.empty() ? std::string{} : std::string(pages[index % pages.size()]);
}

// ---------------------------------------------------------------------------------------------------------------
// JSON (the rpcs3.net patch reply: one flat object)

namespace json_detail
{
inline void skip_space(std::string_view text, std::size_t& at)
{
    while (at < text.size() && (text[at] == ' ' || text[at] == '\t' || text[at] == '\r' || text[at] == '\n')) ++at;
}

inline void append_utf8(std::string& out, std::uint32_t code)
{
    if (code < 0x80)
    {
        out += static_cast<char>(code);
    }
    else if (code < 0x800)
    {
        out += static_cast<char>(0xc0 | (code >> 6));
        out += static_cast<char>(0x80 | (code & 0x3f));
    }
    else if (code < 0x10000)
    {
        out += static_cast<char>(0xe0 | (code >> 12));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (code & 0x3f));
    }
    else
    {
        out += static_cast<char>(0xf0 | (code >> 18));
        out += static_cast<char>(0x80 | ((code >> 12) & 0x3f));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (code & 0x3f));
    }
}

inline bool hex4(std::string_view text, std::size_t at, std::uint32_t& value)
{
    if (at > text.size() || text.size() - at < 4) return false;
    value = 0;
    for (std::size_t i = 0; i < 4; ++i)
    {
        const char c = text[at + i];
        value <<= 4;
        if (c >= '0' && c <= '9') value |= static_cast<std::uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f') value |= static_cast<std::uint32_t>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') value |= static_cast<std::uint32_t>(c - 'A' + 10);
        else return false;
    }
    return true;
}

// String whose opening quote is text[at]; 'at' ends after the closing quote. A surrogate pair becomes one character,
// a lone surrogate U+FFFD. 'out' may be null to skip the string.
inline bool string(std::string_view text, std::size_t& at, std::string* out)
{
    ++at;
    while (at < text.size())
    {
        const char c = text[at++];
        if (c == '"') return true;
        if (static_cast<unsigned char>(c) < 0x20) return false;
        if (c != '\\')
        {
            if (out) *out += c;
            continue;
        }
        if (at >= text.size()) return false;
        char plain = 0;
        switch (text[at++])
        {
        case '"': plain = '"'; break;
        case '\\': plain = '\\'; break;
        case '/': plain = '/'; break;
        case 'b': plain = '\b'; break;
        case 'f': plain = '\f'; break;
        case 'n': plain = '\n'; break;
        case 'r': plain = '\r'; break;
        case 't': plain = '\t'; break;
        case 'u':
        {
            std::uint32_t code = 0;
            if (!hex4(text, at, code)) return false;
            at += 4;
            if (code >= 0xd800 && code < 0xdc00)
            {
                std::uint32_t low = 0;
                if (text.size() - at >= 6 && text[at] == '\\' && text[at + 1] == 'u' && hex4(text, at + 2, low) && low >= 0xdc00 && low < 0xe000)
                {
                    code = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
                    at += 6;
                }
                else code = 0xfffd;
            }
            else if (code >= 0xdc00 && code < 0xe000) code = 0xfffd;
            if (out) append_utf8(*out, code);
            continue;
        }
        default: return false;
        }
        if (out) *out += plain;
    }
    return false;
}

// Object or array starting at text[at], skipped as a whole.
inline bool nested(std::string_view text, std::size_t& at)
{
    std::size_t depth = 0;
    while (at < text.size())
    {
        const char c = text[at];
        if (c == '"')
        {
            if (!depth || !string(text, at, nullptr)) return false;
            continue;
        }
        ++at;
        if (c == '{' || c == '[')
        {
            if (++depth > 64) return false;
        }
        else if (c == '}' || c == ']')
        {
            if (!depth) return false;
            if (--depth == 0) return true;
        }
    }
    return false;
}
} // namespace json_detail

// Top-level fields of one JSON object: strings decoded to UTF-8, numbers and true/false/null as written. Nested
// objects and arrays are checked and skipped. False when 'text' is not exactly one well-formed object.
inline bool parse_json_fields(std::string_view text, std::map<std::string, std::string, std::less<>>& out)
{
    using namespace json_detail;
    out.clear();
    std::size_t at = 0;
    skip_space(text, at);
    if (at >= text.size() || text[at] != '{') return false;
    ++at;
    skip_space(text, at);
    if (at < text.size() && text[at] == '}')
    {
        ++at;
        skip_space(text, at);
        return at == text.size();
    }
    for (;;)
    {
        skip_space(text, at);
        if (at >= text.size() || text[at] != '"') return false;
        std::string key;
        if (!string(text, at, &key)) return false;
        skip_space(text, at);
        if (at >= text.size() || text[at] != ':') return false;
        ++at;
        skip_space(text, at);
        if (at >= text.size()) return false;
        if (text[at] == '"')
        {
            std::string value;
            if (!string(text, at, &value)) return false;
            out.insert_or_assign(std::move(key), std::move(value));
        }
        else if (text[at] == '{' || text[at] == '[')
        {
            if (!nested(text, at)) return false;
        }
        else
        {
            const std::size_t start = at;
            while (at < text.size())
            {
                const char c = text[at];
                const bool literal = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '+' || c == '.';
                if (!literal) break;
                ++at;
            }
            if (at == start) return false;
            out.insert_or_assign(std::move(key), std::string(text.substr(start, at - start)));
        }
        skip_space(text, at);
        if (at >= text.size()) return false;
        if (text[at] == ',')
        {
            ++at;
            continue;
        }
        if (text[at] != '}') return false;
        ++at;
        skip_space(text, at);
        return at == text.size();
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Import naming

struct import_target
{
    bool merge = false; // added to imported_patch.yml with patch_engine::import_patches, as the PC import does
    std::string name;   // file name in the patches folder when it is not merged
};

// patch.yml, imported_patch.yml and <TITLE_ID>_patch.yml are the files the boot reads: they keep their name (any
// letter case on the USB drive; the title ID is written in capitals). Other patch files are merged.
inline import_target import_target_for(std::string_view file_name)
{
    const std::string name = lower(file_name);
    if (name == "patch.yml" || name == "imported_patch.yml") return {false, name};
    constexpr std::string_view suffix = "_patch.yml";
    if (name.size() == 9 + suffix.size() && name.ends_with(suffix) &&
        std::all_of(name.begin(), name.begin() + 9, [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); }))
    {
        std::string serial = name.substr(0, 9);
        for (char& c : serial)
            if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        return {false, serial + std::string(suffix)};
    }
    return {true, {}};
}

// YAML files that share the import folders but are never patch files (cheats.yml: the PC cheat list, imported by
// the cheats list from USB:/cheats and USB:/rpcs3/cheats).
inline bool never_imported(std::string_view file_name)
{
    const std::string name = lower(file_name);
    return name == "rpcn.yml" || name == "patch_config.yml" || name == "config.yml" || name == "vfs.yml" ||
        name == "games.yml" || name == "ps4-settings.yml" || name == "cheats.yml";
}

// A patch file has a top-level "Version:" key (the engine version it was written for).
inline bool looks_like_patch_file(std::string_view content)
{
    if (content.starts_with("\xEF\xBB\xBF")) content.remove_prefix(3);
    while (!content.empty())
    {
        const std::size_t end = content.find('\n');
        std::string_view line = content.substr(0, end);
        if (line.starts_with("Version"))
        {
            line.remove_prefix(7);
            while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
            if (line.starts_with(':')) return true;
        }
        if (end == std::string_view::npos) break;
        content.remove_prefix(end + 1);
    }
    return false;
}

// A *.yml name of an import folder (USB:/rpcs3/patches, /data/rpcs3/import): not a dot file (macOS "._name.yml"
// metadata copies on USB drives) and not one of the never_imported files.
inline bool import_folder_candidate(std::string_view file_name)
{
    return file_name.size() >= 5 && file_name.front() != '.' && lower(file_name.substr(file_name.size() - 4)) == ".yml" &&
        !never_imported(file_name);
}

// /data/rpcs3 itself also holds rpcn.yml, config.yml, games.yml...: only the names the boot reads from the patches
// folder (patch.yml, imported_patch.yml, <TITLE_ID>_patch.yml) are imported from there.
inline bool data_root_candidate(std::string_view file_name)
{
    return import_folder_candidate(file_name) && !import_target_for(file_name).merge;
}

// ---------------------------------------------------------------------------------------------------------------
// Import result (one XMB line of at most 110 bytes, and the startup.log line)

enum class import_failure
{
    unreadable,   // I/O error or larger than the limit; 'detail' says which
    not_patch,    // no top-level "Version:" key
    rejected,     // the patch engine found no patch in it (a file still being copied by FTP looks like this)
    not_writable, // the patches folder refused the write (permission)
    write_failed, // other write or backup error; 'detail' has the error
};

struct import_problem
{
    std::string name;
    import_failure kind = import_failure::unreadable;
    std::string detail;
};

struct import_outcome
{
    std::vector<std::string> done;       // "patch.yml (5123 patches)", "extra.yml (3 novos)"
    std::vector<import_problem> problems;
    std::vector<std::string> warnings;   // files taken with some invalid entries
    std::vector<std::string> skipped;    // same target name as a file of an earlier source
    u32 unchanged = 0;                   // files already identical in the patches folder (or nothing new to merge)
    std::size_t ready_patches = 0;       // patches of the files already in the patches folder (checked in place)
    std::vector<std::string> ready_files;
};

inline constexpr std::size_t import_text_limit = 110;
// Byte bound of an XMB line in the interface language: an Arabic letter takes two bytes for about the width of a Latin one
inline std::size_t line_bytes(std::size_t latin_limit) noexcept
{
    return i18n::rtl() ? 2 * latin_limit : latin_limit;
}
inline constexpr i18n::str import_nothing_text = i18n::str::PATCH_NENHUM_PATCH_ACHADO_FTP_DATA;
inline constexpr i18n::str import_not_writable_text = i18n::str::PATCH_SEM_PERMISSAO_GRAVAR_DATA_RPCS3;

inline std::string import_problem_text(const import_problem& p)
{
    switch (p.kind)
    {
    case import_failure::unreadable: return i18n::trf(i18n::str::PATCH_UNREADABLE, {p.name, p.detail});
    case import_failure::not_patch: return i18n::trf(i18n::str::PATCH_NOT_PATCH_FILE, {p.name});
    case import_failure::rejected: return i18n::trf(i18n::str::PATCH_REJECTED, {p.name});
    case import_failure::not_writable: return i18n::tr(import_not_writable_text);
    case import_failure::write_failed: return i18n::trf(i18n::str::PATCH_WRITE_FAILED, {p.name, p.detail});
    }
    return p.name;
}

inline std::string import_result_text(const import_outcome& o)
{
    const auto join = [](const std::vector<std::string>& items)
    {
        std::string out;
        for (const auto& item : items) out += (out.empty() ? "" : ", ") + item;
        return out;
    };
    const std::string repeated = o.skipped.empty() ? std::string{} : i18n::trf(i18n::str::PATCH_SKIPPED_SAME_NAME, {o.skipped.size(), join(o.skipped)});
    const std::string ignored = o.warnings.empty() ? std::string{} : std::string(i18n::tr(i18n::str::PATCH_ENTRADAS_IGNORADAS_RPCS3_LOG));
    const std::size_t limit = line_bytes(import_text_limit);

    // A refused write explains every other failure of the same import
    for (const auto& p : o.problems)
        if (p.kind == import_failure::not_writable) return clip(i18n::tr(import_not_writable_text), limit);

    if (o.done.empty() && o.problems.size() == 1 && o.problems.front().name.empty())
        return clip(i18n::trf(i18n::str::PATCH_CREATE_FOLDER_FAILED, {o.problems.front().detail}), limit);
    if (o.done.empty() && o.problems.empty())
    {
        if (o.unchanged) return clip(i18n::tr(i18n::str::PATCH_PATCHES_JA_ESTAVAM_IMPORTADOS_NADA) + repeated, limit);
        if (!o.ready_files.empty())
            return clip(i18n::trf(o.ready_files.size() > 1 ? i18n::str::PATCH_READY_MANY : i18n::str::PATCH_READY_ONE, {join(o.ready_files), o.ready_patches}),
                limit);
        return clip(i18n::tr(import_nothing_text) + repeated, limit);
    }
    if (o.problems.empty())
        return clip(i18n::trf(i18n::str::PATCH_IMPORTED, {join(o.done)}) + ignored + repeated, limit);
    if (o.done.empty() && o.problems.size() == 1)
        return clip(import_problem_text(o.problems.front()), limit);

    std::vector<std::string> names;
    for (const auto& p : o.problems) names.push_back(p.name);
    std::string text = o.done.empty() ? std::string(i18n::tr(i18n::str::PATCH_NENHUM_PATCH_IMPORTADO)) : i18n::trf(i18n::str::PATCH_IMPORTED_SOME, {join(o.done)});
    text += i18n::trf(i18n::str::PATCH_INVALID_SUFFIX, {o.problems.size(), join(names)});
    return clip(text + ignored + repeated, limit);
}

// One place the import looked at: "<where> (absent | N file(s) | errno E[, note])".
struct import_scan
{
    std::string where;
    bool present = false; // the folder (or file) exists
    int error = 0;        // errno of a failed stat/open other than "absent"
    std::size_t files = 0;
    std::string note;
};

inline std::string import_scan_text(const std::vector<import_scan>& scans)
{
    std::string out;
    for (const auto& s : scans)
    {
        out += out.empty() ? "" : "; ";
        out += s.where + " (";
        if (s.error) out += "errno " + std::to_string(s.error);
        else if (!s.present) out += "absent";
        else out += std::to_string(s.files) + " file(s)";
        if (!s.note.empty()) out += ", " + s.note;
        out += ')';
    }
    return out;
}

inline std::string import_result_line(std::string_view text, std::string_view scanned)
{
    return concat("patches: import result: ", text, " | scanned: ", scanned);
}

// ---------------------------------------------------------------------------------------------------------------
// Download texts

// Cause of a curl result code in the interface language (CURLcode values; checked against curl.h where the download
// is built).
inline std::string curl_error_text(int code)
{
    using i18n::str;
    switch (code)
    {
    case 1: return i18n::tr(str::PATCH_SOCKET_NAO_FICOU_NAO_BLOQUEANTE);        // CURLE_UNSUPPORTED_PROTOCOL
    case 6: return i18n::tr(str::PATCH_ENDERECO_RPCS3_NET_NAO);                 // CURLE_COULDNT_RESOLVE_HOST
    case 7: return i18n::tr(str::PATCH_SEM_CONEXAO_RPCS3_NET);                  // CURLE_COULDNT_CONNECT
    case 23: return i18n::tr(str::PATCH_RESPOSTA_GRANDE_DEMAIS);                // CURLE_WRITE_ERROR
    case 27: return i18n::tr(str::PATCH_FALHA_INTERNA_CURL_RECURSOS_SISTEMA);   // CURLE_OUT_OF_MEMORY
    case 28: return i18n::tr(str::COM_TEMPO_ESGOTADO);                          // CURLE_OPERATION_TIMEDOUT
    case 35: return i18n::tr(str::PATCH_FALHA_CONEXAO_SEGURA_TLS);              // CURLE_SSL_CONNECT_ERROR
    case 42: return i18n::tr(str::PATCH_CANCELADO);                             // CURLE_ABORTED_BY_CALLBACK
    case 55: return i18n::tr(str::COM_CONEXAO_CAIU_MEIO);                       // CURLE_SEND_ERROR
    case 56: return i18n::tr(str::COM_CONEXAO_CAIU_MEIO);                       // CURLE_RECV_ERROR
    case 60: return i18n::tr(str::PATCH_CERTIFICADO_RPCS3_NET_NAO_FOI);         // CURLE_PEER_FAILED_VERIFICATION
    case 77: return i18n::tr(str::PATCH_CERTIFICADOS_RAIZ_INVALIDOS_PACOTE);    // CURLE_SSL_CACERT_BADFILE
    default: return i18n::trf(str::PATCH_ERRO_2, {code});
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Patch list of one game

using patch_map = patch_engine::patch_map;
using config_values = std::map<std::string, patch_engine::patch_config_value>;

// One hash a patch is split over (PPU executable, SPU programs, PRX segments) and the title it is listed under.
struct row_part
{
    std::string hash, title;
    bool operator==(const row_part&) const = default;
};

struct patch_row
{
    std::string description;
    std::string serial_key, version_key; // patch_config.yml path below each part: hash > description > title > these
    std::vector<row_part> parts;
    std::string author, notes, patch_version, group;
    std::size_t configurable = 0;  // configurable values (kept as configured; they are edited on the PC)
    std::size_t enabled_parts = 0; // parts patch_config.yml enables
    recommended::tier recommendation = recommended::tier::none; // util/ps4_recommended_patches.hpp
    bool auto_on = false;          // switched on at boot by the automatic configuration (not refused)
    std::string why;               // reason of the recommendation (interface language), shown with the notes

    bool on() const { return !parts.empty() && enabled_parts == parts.size(); }
    bool partial() const { return enabled_parts && enabled_parts < parts.size(); }
    bool global() const { return serial_key == patch_key::all; }
    // What the next boot runs: enabled in patch_config.yml or switched on as a recommended patch
    bool active() const { return on() || auto_on; }
};

// The patches of one game as the XMB lists them, with the patch_config.yml state the switches edit.
struct game_patches
{
    std::string path, title, serial, app_version; // app_version: "" when PARAM.SFO could not be read
    std::vector<patch_row> rows;
    patch_map config; // patch_config.yml as read (patch_engine::load_config) plus the switches made since
    std::map<std::pair<std::string, std::string>, config_values> defaults; // (hash, description) -> default values
    std::string files;     // patch files read, for the log
    std::size_t total = 0; // patches in those files
    u32 invalid_files = 0;
    bool recommend_auto = false; // automatic configuration without the game's opt-out: recommended rows start on
    std::string refusals;        // ps4_recommended_off.txt as read (plus the switches made since)
};

inline std::size_t count_patches(const patch_map& map)
{
    std::size_t count = 0;
    for (const auto& [hash, container] : map) count += container.patch_info_map.size();
    return count;
}

inline const patch_engine::patch_config_values* find_config(const patch_map& config, const std::string& hash, const std::string& description,
    const std::string& title, const std::string& serial, const std::string& version)
{
    const auto container = config.find(hash);
    if (container == config.end()) return nullptr;
    const auto info = container->second.patch_info_map.find(description);
    if (info == container->second.patch_info_map.end()) return nullptr;
    const auto titles = info->second.titles.find(title);
    if (titles == info->second.titles.end()) return nullptr;
    const auto serials = titles->second.find(serial);
    if (serials == titles->second.end()) return nullptr;
    const auto versions = serials->second.find(version);
    return versions == serials->second.end() ? nullptr : &versions->second;
}

// Rows for 'serial' at 'app_version', chosen as patch_engine::apply chooses at boot: per title the game's serial
// before "All", then the exact app version before "All". The same description over several hashes (and titles) is
// one row: its parts are switched together. With an unknown app version every version of the serial is listed.
// Order: patches of this game by name, then the patches for every game.
inline std::vector<patch_row> rows_for(const patch_map& catalog, const patch_map& config, const std::string& serial, const std::string& app_version)
{
    const std::string& all = patch_key::all;
    std::map<std::tuple<bool, std::string, std::string, std::string, std::string>, patch_row> rows;
    for (const auto& [hash, container] : catalog)
    {
        for (const auto& [description, info] : container.patch_info_map)
        {
            for (const auto& [title, serials] : info.titles)
            {
                auto found = serial.empty() ? serials.end() : serials.find(serial);
                if (found == serials.end()) found = serials.find(all);
                if (found == serials.end()) continue;
                const std::string& serial_key = found->first;
                const auto& versions = found->second;
                std::vector<std::string> version_keys;
                if (app_version.empty())
                {
                    for (const auto& [version, values] : versions) version_keys.push_back(version);
                    std::sort(version_keys.begin(), version_keys.end());
                }
                else if (versions.contains(app_version)) version_keys.push_back(app_version);
                else if (versions.contains(all)) version_keys.push_back(all);
                for (const auto& version : version_keys)
                {
                    patch_row& row = rows[{serial_key == all, lower(description), description, serial_key, version}];
                    const row_part part{hash, title};
                    if (std::find(row.parts.begin(), row.parts.end(), part) != row.parts.end()) continue;
                    if (row.parts.empty())
                    {
                        row.description = description;
                        row.serial_key = serial_key;
                        row.version_key = version;
                    }
                    row.parts.push_back(part);
                    if (row.author.empty()) row.author = info.author;
                    if (row.notes.empty()) row.notes = info.notes;
                    if (row.patch_version.empty()) row.patch_version = info.patch_version;
                    if (row.group.empty()) row.group = info.patch_group;
                    row.configurable = std::max(row.configurable, info.default_config_values.size());
                    if (const auto* values = find_config(config, hash, description, title, serial_key, version); values && values->enabled)
                        ++row.enabled_parts;
                }
            }
        }
    }
    std::vector<patch_row> out;
    out.reserve(rows.size());
    for (auto& [key, row] : rows) out.push_back(std::move(row));
    return out;
}

// Default configurable values of every patch the list or patch_config.yml names, from the patch files.
inline void collect_defaults(game_patches& game, const patch_map& catalog)
{
    const auto add = [&](const std::string& hash, const std::string& description)
    {
        if (game.defaults.contains({hash, description})) return;
        const auto container = catalog.find(hash);
        if (container == catalog.end()) return;
        const auto info = container->second.patch_info_map.find(description);
        if (info == container->second.patch_info_map.end()) return;
        game.defaults.emplace(std::pair{hash, description}, info->second.default_config_values);
    };
    for (const auto& row : game.rows)
        for (const auto& part : row.parts) add(part.hash, row.description);
    for (const auto& [hash, container] : game.config)
        for (const auto& [description, info] : container.patch_info_map) add(hash, description);
}

// Switches every part of row 'index' in game.config. Turning a row on turns off the rows of the same patch group for
// the same serial and version (one patch per group runs, as the PC dialog enforces). Returns the rows turned off.
inline std::vector<std::size_t> set_row(game_patches& game, std::size_t index, bool on)
{
    std::vector<std::size_t> off;
    if (index >= game.rows.size()) return off;
    const auto store = [&](patch_row& row, bool value)
    {
        for (const auto& part : row.parts)
            game.config[part.hash].patch_info_map[row.description].titles[part.title][row.serial_key][row.version_key].enabled = value;
        row.enabled_parts = value ? row.parts.size() : 0;
    };
    const patch_row& target = game.rows[index];
    if (on && !target.group.empty())
    {
        for (std::size_t i = 0; i < game.rows.size(); ++i)
        {
            patch_row& other = game.rows[i];
            if (i == index || !other.enabled_parts || other.group != target.group || other.serial_key != target.serial_key || other.version_key != target.version_key)
                continue;
            store(other, false);
            off.push_back(i);
        }
    }
    store(game.rows[index], on);
    return off;
}

// What patch_engine::save_config writes: every patch_config.yml entry (game.config) with the default configurable
// values of its patch, so save_config keeps or drops configured values exactly as on the PC. Entries of patches that
// are in no patch file now are kept as they are: their defaults are unknown, and a placeholder default that matches
// no stored set makes save_config write the stored values instead of dropping them.
inline patch_map save_map(const game_patches& game)
{
    patch_map out;
    for (const auto& [hash, container] : game.config)
    {
        for (const auto& [description, info] : container.patch_info_map)
        {
            auto& target = out[hash].patch_info_map[description];
            target.titles = info.titles;
            if (const auto defaults = game.defaults.find({hash, description}); defaults != game.defaults.end())
                target.default_config_values = defaults->second;
            else
                target.default_config_values[std::string{}] = {};
        }
    }
    return out;
}

// True when 'on_disk' (patch_config.yml read back) holds the state of every part of 'row'.
inline bool row_saved(const patch_map& on_disk, const patch_row& row)
{
    for (const auto& part : row.parts)
    {
        const auto* values = find_config(on_disk, part.hash, row.description, part.title, row.serial_key, row.version_key);
        if ((values && values->enabled) != row.on()) return false;
    }
    return true;
}

// Recommended patches (util/ps4_recommended_patches.hpp) in the list of one game. 'automatic': the game boots in the
// automatic configuration without its opt-out marker, so each recommended row runs at the next boot unless one of its
// parts is refused in game.refusals; suggested rows are only marked.
inline void mark_recommended(game_patches& game, bool automatic)
{
    game.recommend_auto = automatic;
    for (auto& row : game.rows)
    {
        row.recommendation = recommended::tier::none;
        row.why.clear();
        bool refused = false;
        for (const auto& part : row.parts)
        {
            const recommended::entry* e = recommended::find(part.hash, row.description, game.serial);
            if (!e) continue;
            if (row.recommendation != recommended::tier::automatic)
            {
                row.recommendation = e->level;
                row.why = i18n::tr(e->why);
            }
            if (e->level == recommended::tier::automatic && recommended::refused(game.refusals, game.serial, part.hash, row.description))
                refused = true;
        }
        row.auto_on = automatic && row.recommendation == recommended::tier::automatic && !refused;
    }
}

// A recommended row the switch handles through ps4_recommended_off.txt instead of patch_config.yml.
inline bool auto_managed(const game_patches& game, const patch_row& row)
{
    return game.recommend_auto && row.recommendation == recommended::tier::automatic;
}

// ps4_recommended_off.txt after switching row 'index' on (its lines removed) or off (one line per recommended part).
inline std::string refusals_after(const game_patches& game, std::size_t index, bool on)
{
    std::string content = game.refusals;
    if (index >= game.rows.size()) return content;
    const patch_row& row = game.rows[index];
    for (const auto& part : row.parts)
        if (recommended::tier_for(part.hash, row.description, game.serial) == recommended::tier::automatic)
            content = recommended::with_line(content, recommended::off_line(game.serial, part.hash, row.description), !on);
    return content;
}
} // namespace utils::ps4::patches

#ifdef RPCS3_PS4

#include "Crypto/utils.h"
#include "Emu/system_utils.hpp"
#include "Utilities/File.h"
#include "Utilities/StrFmt.h"
#include "util/ps4_net.hpp"
#include "util/ps4_policy.hpp"
#include "util/sysinfo.hpp"
#include "curl/curl/include/curl/curl.h"

#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <sys/stat.h>

namespace utils::ps4::patches
{
static_assert(CURLE_UNSUPPORTED_PROTOCOL == 1 && CURLE_COULDNT_RESOLVE_HOST == 6 && CURLE_COULDNT_CONNECT == 7 && CURLE_WRITE_ERROR == 23 &&
    CURLE_OUT_OF_MEMORY == 27 && CURLE_OPERATION_TIMEDOUT == 28 && CURLE_SSL_CONNECT_ERROR == 35 && CURLE_ABORTED_BY_CALLBACK == 42 &&
    CURLE_SEND_ERROR == 55 && CURLE_RECV_ERROR == 56 && CURLE_PEER_FAILED_VERIFICATION == 60 && CURLE_SSL_CACERT_BADFILE == 77,
    "curl_error_text uses these CURLcode values");

// 0.38: 8 MiB, as patch_engine::load on the PS4 (utils::ps4::hle_memory::patch_file_limit): yaml-cpp needs ~30 MiB of
// nodes per MB of file. The downloaded patch.yml is ~1.1 MB (2166 patches).
inline constexpr u64 file_limit = 8 << 20;           // largest patch file read
inline constexpr u64 reply_limit = 16 << 20;         // largest rpcs3.net reply (JSON-escaped patch.yml, ~1.1x the file)
inline constexpr char download_host[] = "rpcs3.net";
// Mozilla root certificates (148 roots, Mozilla data of 2024-07-02, wolfSSL's cacrt_all.pem) shipped with the
// package; nothing else is trusted.
inline constexpr char ca_bundle[] = "/app0/certs/cacert.pem";

// Reads a whole file of at most 'limit' bytes; a short read (I/O error, USB drive pulled) fails instead of ending
// in a fatal error.
inline bool read_file(const std::string& path, u64 limit, std::string& out)
{
    out.clear();
    const fs::file file(path);
    if (!file) return false;
    const u64 size = file.size();
    if (size > limit) return false;
    out.resize(static_cast<std::size_t>(size));
    return size == 0 || file.read(out.data(), size) == size;
}

// Writes 'content' through a temporary file renamed over 'path'.
inline bool write_file(const std::string& path, std::string_view content)
{
    fs::pending_file file(path);
    if (!file.file) return false;
    if (!content.empty() && file.file.write(content.data(), content.size()) != content.size()) return false;
    return file.commit();
}

// ---------------------------------------------------------------------------------------------------------------
// Patch files

// Every patch of the patch files without its data (only the list needs them), kept between list openings while the
// files are unchanged. Touched only by the code that runs the XMB loader jobs.
struct catalog
{
    patch_map map;
    std::string key;   // names, sizes and mtimes of the files read
    std::string files; // "patch.yml (5123), imported_patch.yml (2)"
    std::size_t total = 0;
    u32 invalid_files = 0;
    bool loaded = false;
};

// patch.yml, imported_patch.yml, then every <name>_patch.yml: the files and the order of the PC dialog.
inline std::vector<std::string> patch_files()
{
    std::vector<std::string> names{"patch.yml", "imported_patch.yml"};
    std::vector<std::string> titles;
    if (fs::dir folder{patch_engine::get_patches_path()})
    {
        for (const auto& entry : folder)
        {
            if (entry.is_directory || entry.name == "imported_patch.yml" || entry.name.size() <= 10 || !entry.name.ends_with("_patch.yml")) continue;
            titles.push_back(entry.name);
        }
    }
    std::sort(titles.begin(), titles.end());
    names.insert(names.end(), titles.begin(), titles.end());
    return names;
}

// Reads the patch files again when one was added, removed or changed. True when it read them.
inline bool refresh(catalog& c)
{
    const std::string dir = patch_engine::get_patches_path();
    std::vector<std::string> present;
    std::string key;
    for (const auto& name : patch_files())
    {
        fs::stat_t stat{};
        if (!fs::get_stat(dir + name, stat) || stat.is_directory) continue;
        key += fmt::format("%s:%u:%d;", name, stat.size, stat.mtime);
        present.push_back(name);
    }
    if (c.loaded && key == c.key) return false;
    catalog fresh;
    fresh.key = std::move(key);
    fresh.loaded = true;
    for (const auto& name : present)
    {
        const std::size_t before = count_patches(fresh.map);
        // importing = true: the switches come from patch_config.yml once, not once per file
        const bool clean = patch_engine::load(fresh.map, dir + name, "", true);
        fresh.invalid_files += clean ? 0 : 1;
        fresh.files += fmt::format("%s%s (%u%s)", fresh.files.empty() ? "" : ", ", name, count_patches(fresh.map) - before, clean ? "" : ", com erros");
    }
    for (auto& [hash, container] : fresh.map)
        for (auto& [description, info] : container.patch_info_map) std::vector<patch_engine::patch_data>{}.swap(info.data_list);
    fresh.total = count_patches(fresh.map);
    c = std::move(fresh);
    return true;
}

// The patch list of one game. 'app_version' is the version the boot will report ("" when unknown).
inline game_patches load_game(catalog& c, std::string path, std::string title, std::string serial, std::string app_version)
{
    const bool read = refresh(c);
    game_patches game;
    game.path = std::move(path);
    game.title = std::move(title);
    game.serial = std::move(serial);
    game.app_version = std::move(app_version);
    game.config = patch_engine::load_config();
    game.rows = rows_for(c.map, game.config, game.serial, game.app_version);
    collect_defaults(game, c.map);
    game.files = c.files.empty() ? std::string("nenhum arquivo de patch") : c.files;
    game.total = c.total;
    game.invalid_files = c.invalid_files;
    // Same conditions as the boot (patch_engine::append_global_patches): automatic configuration, no opt-out marker
    const bool automatic = !game.serial.empty() && get_policy_mode(game.serial) == policy_mode::automatic &&
        !fs::is_file(rpcs3::utils::get_custom_config_dir() + game.serial + std::string(recommended::game_off_suffix));
    if (!read_file(patch_engine::get_patches_path() + std::string(recommended::off_file_name), 1 << 20, game.refusals)) game.refusals.clear();
    mark_recommended(game, automatic);
    std::size_t recommended_rows = 0, recommended_on = 0;
    for (const auto& row : game.rows)
    {
        if (row.recommendation != recommended::tier::automatic) continue;
        ++recommended_rows;
        recommended_on += row.auto_on ? 1 : 0;
    }
    const std::string recommended_text = recommended_rows ? fmt::format("; recommended on PS4: %u row(s), %u on at the next boot%s", recommended_rows, recommended_on,
        automatic ? "" : " (not automatic configuration, or the game's no-recommended-patches marker)") : std::string{};
    ps4_startup_checkpoint(fmt::format("patches: list for %s v%s: %u row(s); %s%s%s", game.serial, game.app_version.empty() ? "?" : game.app_version,
        game.rows.size(), game.files, read ? "" : " (cached)", recommended_text).c_str());
    return game;
}

// Writes patch_config.yml with patch_engine::save_config and reads it back: true when the file now holds the state of
// 'row'. save_config replaces the file through a temporary file, so a failure leaves the previous file.
inline bool save(const game_patches& game, const patch_row& row)
{
    patch_engine::save_config(save_map(game));
    return row_saved(patch_engine::load_config(), row);
}

// Switches a recommended row of a game in the automatic configuration (auto_managed): its lines leave (on) or enter
// (off) ps4_recommended_off.txt, written through a temporary file and read back. Switching off also clears a
// patch_config.yml enable of the row; switching on never writes patch_config.yml. True when the files hold the new state.
inline bool set_recommended(game_patches& game, std::size_t index, bool on)
{
    if (index >= game.rows.size()) return false;
    const std::string dir = patch_engine::get_patches_path();
    const std::string path = dir + std::string(recommended::off_file_name);
    const std::string content = refusals_after(game, index, on);
    std::string back;
    if (!fs::create_path(dir) || !write_file(path, content) || !read_file(path, 1 << 20, back) || back != content) return false;
    game.refusals = content;
    patch_row& row = game.rows[index];
    row.auto_on = on;
    if (on || !row.enabled_parts) return true;
    set_row(game, index, false);
    return save(game, row);
}

// ---------------------------------------------------------------------------------------------------------------
// Import

struct import_report
{
    u32 replaced = 0, merged = 0, unchanged = 0, invalid = 0;
    std::string text;     // one XMB line (interface language, at most 110 bytes)
    std::string scanned;  // every place looked at: "<where> (absent | N file(s) | errno E[, note]); ..."
    std::string log_line; // "patches: import result: <text> | scanned: <scanned>", already in startup.log
};

inline constexpr unsigned import_usb_drives = 8; // /mnt/usb0 .. /mnt/usb7, as the game library walks them

// The *.yml names 'accept' takes from 'folder', sorted, with the scan result. A folder that cannot be listed is
// reported with its errno; "absent" only for ENOENT/ENOTDIR.
template <typename Accept>
inline std::vector<std::string> import_list(const std::string& folder, std::string where, Accept&& accept, std::vector<import_scan>& scans)
{
    import_scan scan{std::move(where), false, 0, 0, {}};
    std::vector<std::string> names;
    struct stat st{};
    if (::stat(folder.c_str(), &st) != 0)
    {
        scan.error = errno == ENOENT || errno == ENOTDIR ? 0 : errno;
    }
    else if (fs::dir dir{folder})
    {
        scan.present = true;
        for (const auto& entry : dir)
            if (!entry.is_directory && accept(entry.name)) names.push_back(entry.name);
        std::sort(names.begin(), names.end());
        scan.files = names.size();
    }
    else
    {
        scan.error = errno ? errno : -1;
    }
    scans.push_back(std::move(scan));
    return names;
}

// Reads a source file; on failure 'why' gets the reason for the XMB ("maior que 8 MB", the file error).
inline bool read_import_source(const std::string& path, std::string& content, std::string& why)
{
    content.clear();
    const fs::file file(path);
    if (!file)
    {
        why = i18n::trf(i18n::str::PATCH_ERRO_2, {fmt::format("%s", fs::g_tls_error)});
        return false;
    }
    const u64 size = file.size();
    if (size > file_limit)
    {
        why = i18n::trf(i18n::str::PATCH_MAIOR_QUE_MB, {file_limit >> 20});
        return false;
    }
    content.resize(static_cast<std::size_t>(size));
    if (size && file.read(content.data(), size) != size)
    {
        why = i18n::trf(i18n::str::PATCH_LIDOS_MENOS_BYTES, {size});
        return false;
    }
    return true;
}

// Copies the patch files of the import sources into /data/rpcs3/patches. Sources, in this order:
//   USB:/rpcs3/patches/*.yml (drives 0-7), /data/rpcs3/import/*.yml, /data/rpcs3/{patch.yml, imported_patch.yml,
//   <TITLE_ID>_patch.yml} (only those names: /data/rpcs3 also holds rpcn.yml, config.yml...), USB:/patch.yml.
// Each file is parsed by the patch engine first and must hold at least one patch; a replaced file is kept as
// <name>.bak; sources stay where they are. Files the engine reads with some invalid entries are taken (the boot skips
// the same entries) and named in the result. /data/rpcs3/patches itself is only checked in place: a file put there
// by FTP is already what the boot reads, so when no source has anything it is parsed and reported, never copied.
// Every return writes "patches: import result: ..." to startup.log (report.log_line); callers show report.text.
inline import_report import_files()
{
    import_report report;
    import_outcome outcome;
    std::vector<import_scan> scans;
    const std::string dir = patch_engine::get_patches_path();

    const auto finish = [&]() -> import_report
    {
        report.text = import_result_text(outcome);
        report.scanned = import_scan_text(scans);
        report.log_line = import_result_line(report.text, report.scanned);
        ps4_startup_checkpoint(report.log_line.c_str());
        return report;
    };
    // Write errors: a refused permission gets its own text (startup.log 'folders:' says which folder is not open)
    const auto write_problem = [&](const std::string& name) -> import_problem
    {
        if (fs::g_tls_error == fs::error::acces) return {name, import_failure::not_writable, {}};
        return {name, import_failure::write_failed, i18n::trf(i18n::str::PATCH_ERRO_2, {fmt::format("%s", fs::g_tls_error)})};
    };

    if (!fs::create_path(dir))
    {
        // An empty name: the folder itself could not be created
        ++report.invalid;
        outcome.problems.push_back(write_problem({}));
        scans.push_back({"/data/rpcs3/patches", false, errno ? errno : -1, 0, "cannot create"});
        return finish();
    }

    std::vector<std::string> sources;
    std::vector<std::string> usb_roots;
    for (unsigned i = 0; i < import_usb_drives; ++i)
    {
        const std::string mount = "/mnt/usb" + std::to_string(i);
        if (!fs::is_dir(mount)) continue;
        usb_roots.push_back(mount);
        const std::string folder = mount + "/rpcs3/patches/";
        for (const auto& name : import_list(folder, fmt::format("usb%u:/rpcs3/patches", i), import_folder_candidate, scans))
            sources.push_back(folder + name);
    }
    if (usb_roots.empty()) scans.push_back({"USB", false, 0, 0, "no drive"});
    for (const auto& name : import_list("/data/rpcs3/import/", "/data/rpcs3/import", import_folder_candidate, scans))
        sources.push_back("/data/rpcs3/import/" + name);
    for (const auto& name : import_list("/data/rpcs3/", "/data/rpcs3/*patch.yml", data_root_candidate, scans))
        sources.push_back("/data/rpcs3/" + name);
    for (const auto& mount : usb_roots)
    {
        import_scan scan{mount.substr(5) + ":/patch.yml", false, 0, 0, {}};
        if (fs::is_file(mount + "/patch.yml"))
        {
            scan.present = true;
            scan.files = 1;
            sources.push_back(mount + "/patch.yml");
        }
        scans.push_back(std::move(scan));
    }
    // Files that keep their name go first: an imported_patch.yml among the sources is then written before the other
    // files are merged into it, instead of replacing those merges later in the same import.
    std::stable_partition(sources.begin(), sources.end(),
        [](const std::string& source) { return !import_target_for(source.substr(source.find_last_of('/') + 1)).merge; });

    bool imported_backed_up = false;
    // Files of the patches folder this import already wrote or found identical. A second source for the same file
    // (USB:/rpcs3/patches/patch.yml and USB:/patch.yml, or two drives) is skipped: it would replace the first one
    // and overwrite the .bak of the file that was there before the import.
    std::set<std::string> targets;
    for (const auto& source : sources)
    {
        const std::string name = source.substr(source.find_last_of('/') + 1);
        const import_target target = import_target_for(name);
        if (!target.merge && targets.contains(target.name))
        {
            outcome.skipped.push_back(name);
            ps4_startup_checkpoint(fmt::format("patches: import: '%s' skipped; '%s' already came from another source in this import", source, target.name).c_str());
            continue;
        }
        std::string content, why;
        if (!read_import_source(source, content, why))
        {
            ++report.invalid;
            outcome.problems.push_back({name, import_failure::unreadable, why});
            ps4_startup_checkpoint(fmt::format("patches: import: cannot read '%s' (%s)", source, why).c_str());
            continue;
        }
        if (!looks_like_patch_file(content))
        {
            ++report.invalid;
            outcome.problems.push_back({name, import_failure::not_patch, {}});
            ps4_startup_checkpoint(fmt::format("patches: import: '%s' has no 'Version:' key; not a patch file", source).c_str());
            continue;
        }
        patch_map parsed;
        const bool clean = patch_engine::load(parsed, source, content, true);
        const std::size_t patches = count_patches(parsed);
        if (!patches)
        {
            ++report.invalid;
            outcome.problems.push_back({name, import_failure::rejected, {}});
            ps4_startup_checkpoint(fmt::format("patches: import: '%s' rejected by the patch engine (see RPCS3.log; %u bytes)", source, content.size()).c_str());
            continue;
        }
        if (!clean) outcome.warnings.push_back(name);
        if (target.merge)
        {
            const std::string imported = patch_engine::get_imported_patch_path();
            if (!imported_backed_up && fs::is_file(imported))
            {
                if (!fs::copy_file(imported, imported + ".bak", true))
                {
                    ++report.invalid;
                    outcome.problems.push_back(write_problem(name));
                    ps4_startup_checkpoint(fmt::format("patches: import: cannot back up '%s': %s", imported, fs::g_tls_error).c_str());
                    continue;
                }
                imported_backed_up = true;
            }
            usz count = 0, total = 0;
            if (!patch_engine::import_patches(parsed, imported, count, total))
            {
                ++report.invalid;
                outcome.problems.push_back({name, import_failure::write_failed, "RPCS3.log"});
                ps4_startup_checkpoint(fmt::format("patches: import: merging '%s' into imported_patch.yml failed (see RPCS3.log)", source).c_str());
                continue;
            }
            if (!count)
            {
                ++report.unchanged;
                ++outcome.unchanged;
                continue;
            }
            ++report.merged;
            outcome.done.push_back(i18n::trf(i18n::str::PATCH_DONE_NEW, {name, count}));
            ps4_startup_checkpoint(fmt::format("patches: import: '%s' merged into imported_patch.yml (%u of %u patches new or newer)", source, count, total).c_str());
            continue;
        }
        const std::string destination = dir + target.name;
        if (std::string current; read_file(destination, file_limit, current) && current == content)
        {
            targets.insert(target.name);
            ++report.unchanged;
            ++outcome.unchanged;
            continue;
        }
        if (fs::is_file(destination) && !fs::copy_file(destination, destination + ".bak", true))
        {
            ++report.invalid;
            outcome.problems.push_back(write_problem(name));
            ps4_startup_checkpoint(fmt::format("patches: import: cannot back up '%s': %s", destination, fs::g_tls_error).c_str());
            continue;
        }
        if (!write_file(destination, content))
        {
            ++report.invalid;
            outcome.problems.push_back(write_problem(name));
            ps4_startup_checkpoint(fmt::format("patches: import: cannot write '%s': %s", destination, fs::g_tls_error).c_str());
            continue;
        }
        targets.insert(target.name);
        // Its .bak already holds the file from before this import; the merges below must not overwrite it
        if (destination == patch_engine::get_imported_patch_path()) imported_backed_up = true;
        ++report.replaced;
        outcome.done.push_back(i18n::trf(i18n::str::PATCH_DONE_PATCHES, {target.name, patches}));
        ps4_startup_checkpoint(fmt::format("patches: import: '%s' -> '%s' (%u patches%s)", source, destination, patches, clean ? "" : ", some entries invalid").c_str());
    }

    // The patches folder, checked in place
    import_scan here{"/data/rpcs3/patches", true, 0, 0, {}};
    std::vector<std::string> present;
    for (const auto& name : patch_files())
        if (fs::is_file(dir + name)) present.push_back(name);
    here.files = present.size();
    here.note = "check-only";
    if (outcome.done.empty() && outcome.problems.empty() && !outcome.unchanged)
    {
        for (const auto& name : present)
        {
            patch_map parsed;
            patch_engine::load(parsed, dir + name, "", true);
            const std::size_t patches = count_patches(parsed);
            if (patches)
            {
                outcome.ready_patches += patches;
                outcome.ready_files.push_back(name);
            }
            else
            {
                outcome.problems.push_back({name, import_failure::rejected, {}});
                ps4_startup_checkpoint(fmt::format("patches: import: '%s' in the patches folder has no valid patch (see RPCS3.log)", dir + name).c_str());
            }
        }
        if (!present.empty()) here.note += fmt::format(", %u patches", outcome.ready_patches);
    }
    scans.push_back(std::move(here));
    return finish();
}

// ---------------------------------------------------------------------------------------------------------------
// Download

// What a finished download reports, for callers that react to a kind of result: the text is translated, so it is never
// compared.
enum class result_kind : u8
{
    none,          // not finished
    updated,       // patch.yml replaced
    up_to_date,    // the server has the same patch.yml
    unavailable,   // no root certificates in the package
    cancelled,     // X on the row or the XMB closing
    network,       // DNS, curl, HTTP or a server return code
    bad_reply,     // reply or patch.yml rejected (format, version, SHA-256, patch engine)
    write_error,   // creating /data/rpcs3/patches, backing up or writing patch.yml failed (the folder may refuse writes)
};

// Shared between the XMB and the download thread. Everything but the atomics is written by the thread before 'done'
// is set (release) and read by the XMB after it sees 'done' (acquire).
struct download_state
{
    std::atomic<bool> stop{false}; // set by the XMB: cancel at the next progress callback
    std::atomic<bool> done{false};
    std::atomic<int> stage{0};     // 0 resolving, 1 connecting/receiving, 2 checking, 3 writing
    std::atomic<u64> received{0}, expected{0};
    bool ok = false;
    bool changed = false; // patch.yml was replaced
    result_kind kind = result_kind::none;
    std::string text;     // result line for the XMB (interface language)
};

inline bool can_download()
{
    return fs::is_file(ca_bundle);
}

namespace download_detail
{
struct transfer
{
    download_state* state = nullptr;
    std::string body;
    bool too_large = false;
};

inline std::size_t write(char* data, std::size_t size, std::size_t count, void* user)
{
    auto& t = *static_cast<transfer*>(user);
    const std::size_t bytes = size * count;
    if (t.body.size() + bytes > reply_limit)
    {
        t.too_large = true;
        return 0;
    }
    t.body.append(data, bytes);
    t.state->received.store(t.body.size(), std::memory_order_relaxed);
    return bytes;
}

// Called about once per second during the whole transfer (connection and TLS handshake included).
inline int progress(void* user, curl_off_t total, curl_off_t, curl_off_t, curl_off_t)
{
    auto& t = *static_cast<transfer*>(user);
    if (total > 0) t.state->expected.store(static_cast<u64>(total), std::memory_order_relaxed);
    return t.state->stop.load(std::memory_order_relaxed) ? 1 : 0;
}

inline std::string curl_text(CURLcode code)
{
    return curl_error_text(static_cast<int>(code));
}

// "libcurl 8.22.0-DEV wolfSSL/5.9.4" of the linked library
inline std::string curl_versions()
{
    const curl_version_info_data* info = curl_version_info(CURLVERSION_NOW);
    if (!info) return "libcurl ?";
    return fmt::format("libcurl %s %s", info->version ? info->version : "?", info->ssl_version ? info->ssl_version : "(no TLS backend)");
}
} // namespace download_detail

// Downloads the latest patch.yml as the PC patch manager does (rpcs3.net, patch engine version, SHA-256 of the file
// already here so the server can answer "up to date"), checks the reply's SHA-256 and the engine's parse, keeps the
// old file as patch.yml.old and replaces patch.yml through a temporary file. Runs on its own thread; sets 'done'.
inline void download(download_state& state)
{
    using i18n::str;
    const auto finish = [&](result_kind kind, std::string text, std::string log)
    {
        ps4_startup_checkpoint(("patches: download: " + log).c_str());
        state.ok = kind == result_kind::updated || kind == result_kind::up_to_date;
        state.kind = kind;
        state.text = clip(std::move(text), line_bytes(110));
        state.done.store(true, std::memory_order_release);
    };

    std::string certificates;
    if (!read_file(ca_bundle, 4 << 20, certificates) || certificates.find("-----BEGIN CERTIFICATE-----") == std::string::npos)
        return finish(result_kind::unavailable, i18n::tr(str::PATCH_DOWNLOAD_INDISPONIVEL_PACOTE_NAO_TEM), "no CA bundle at /app0/certs/cacert.pem");

    std::string how;
    const u32 address = utils::ps4::net::resolve_ipv4(download_host, how);
    if (!address)
        return finish(result_kind::network, i18n::tr(str::PATCH_SEM_DNS_RPCS3_NET_CONFIRA), "DNS failed: " + how);
    std::array<u8, 4> ip{};
    std::memcpy(ip.data(), &address, sizeof(address));
    const std::string resolve_line = fmt::format("%s:443:%u.%u.%u.%u", download_host, ip[0], ip[1], ip[2], ip[3]);

    const std::string path = patch_engine::get_patches_path() + "patch.yml";
    std::string url = fmt::format("https://%s/compatibility?patch&api=v1&v=%s", download_host, patch_engine_version);
    std::string current;
    const bool have_current = read_file(path, file_limit, current) && !current.empty();
    if (have_current) url += "&sha256=" + sha256_get_hash(current.data(), current.size(), true);

    CURL* curl = curl_easy_init();
    if (!curl) return finish(result_kind::network, i18n::tr(str::PATCH_FALHA_INICIAR_DOWNLOAD_CURL), "curl_easy_init failed");
    curl_slist* resolve = curl_slist_append(nullptr, resolve_line.c_str());
    download_detail::transfer transfer;
    transfer.state = &state;
    std::array<char, CURL_ERROR_SIZE> error{};
    curl_blob blob{certificates.data(), certificates.size(), CURL_BLOB_COPY};
    const std::string agent = utils::get_user_agent();
    state.stage = 1;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_RESOLVE, resolve);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_SSLVERSION, static_cast<long>(CURL_SSLVERSION_TLSv1_2));
    curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &blob);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 64L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 600L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, agent.c_str());
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error.data());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &download_detail::write);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &transfer);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &download_detail::progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &transfer);
    const CURLcode result = curl_easy_perform(curl);
    long http = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
    curl_slist_free_all(resolve);
    curl_easy_cleanup(curl);

    const std::string where = fmt::format("%s via %s, HTTP %d, %u bytes", resolve_line, how, http, transfer.body.size());
    if (state.stop.load())
        return finish(result_kind::cancelled, i18n::tr(str::PATCH_DOWNLOAD_PATCHES_CANCELADO), "cancelled (" + where + ")");
    if (result != CURLE_OK)
        return finish(result_kind::network, i18n::trf(str::PATCH_DOWNLOAD_FAILED, {i18n::raw(download_detail::curl_text(result))}),
            fmt::format("curl %d '%s' [%s] (%s; %s)", static_cast<int>(result), error.data(), curl_easy_strerror(result), where, download_detail::curl_versions()));
    if (http != 200)
        return finish(result_kind::network, i18n::trf(str::PATCH_RPCS3_NET_RESPONDEU_HTTP_TENTE, {http}), "HTTP error (" + where + ")");

    state.stage = 2;
    std::map<std::string, std::string, std::less<>> reply;
    if (!parse_json_fields(transfer.body, reply))
        return finish(result_kind::bad_reply, i18n::tr(str::PATCH_RESPOSTA_INVALIDA_RPCS3_NET), "reply is not one JSON object (" + where + ")");
    int code = -255;
    if (const auto it = reply.find("return_code"); it != reply.end())
    {
        int value = 0;
        if (const auto [end, ec] = std::from_chars(it->second.data(), it->second.data() + it->second.size(), value); ec == std::errc{} && end == it->second.data() + it->second.size())
            code = value;
    }
    if (code == 1)
        return finish(result_kind::up_to_date, i18n::tr(str::PATCH_PATCHES_JA_ESTAO_ATUALIZADOS_PATCH), "up to date (" + where + ")");
    if (code != 0)
    {
        const str reason = code == -1 ? str::PATCH_NENHUM_PATCH_ESTA_VERSAO : code == -2 ? str::PATCH_SERVIDOR_MANUTENCAO : str::PATCH_ERRO_SERVIDOR;
        return finish(result_kind::network, i18n::trf(str::PATCH_RPCS3_NET_CODIGO, {reason, code}), fmt::format("return_code %d (%s)", code, where));
    }
    const auto version = reply.find("version");
    const auto hash = reply.find("sha256");
    const auto patch = reply.find("patch");
    if (version == reply.end() || version->second != patch_engine_version)
        return finish(result_kind::bad_reply, i18n::tr(str::PATCH_RPCS3_NET_MANDOU_PATCHES_OUTRA), "wrong or missing version (" + where + ")");
    if (hash == reply.end() || patch == reply.end() || patch->second.empty())
        return finish(result_kind::bad_reply, i18n::tr(str::PATCH_RESPOSTA_INCOMPLETA_RPCS3_NET), "missing sha256 or patch (" + where + ")");
    const std::string& content = patch->second;
    if (lower(hash->second) != sha256_get_hash(content.data(), content.size(), true))
        return finish(result_kind::bad_reply, i18n::tr(str::PATCH_PATCH_YML_BAIXADO_NAO_CONFERE), "checksum mismatch (" + where + ")");
    patch_map parsed;
    if (!patch_engine::load(parsed, "From Download", content, true) || !count_patches(parsed))
        return finish(result_kind::bad_reply, i18n::tr(str::PATCH_PATCH_YML_BAIXADO_NAO_PASSOU), "patch engine rejected the file (see RPCS3.log)");

    // X on the row (or the XMB closing) while the reply was checked: nothing is replaced
    if (state.stop.load())
        return finish(result_kind::cancelled, i18n::tr(str::PATCH_DOWNLOAD_PATCHES_CANCELADO_PATCH_YML), "cancelled before writing (" + where + ")");
    state.stage = 3;
    if (!fs::create_path(patch_engine::get_patches_path()))
        return finish(result_kind::write_error, i18n::tr(str::PATCH_ERRO_CRIAR_DATA_RPCS3), "cannot create the patches folder");
    if (have_current && !fs::copy_file(path, path + ".old", true))
        return finish(result_kind::write_error, i18n::tr(str::PATCH_ERRO_GUARDAR_PATCH_YML_ANTERIOR), fmt::format("cannot back up patch.yml: %s", fs::g_tls_error));
    if (!write_file(path, content))
        return finish(result_kind::write_error, i18n::tr(str::PATCH_ERRO_GRAVAR_DATA_RPCS3_PATCHES), fmt::format("cannot write patch.yml: %s", fs::g_tls_error));
    state.changed = true;
    const std::size_t total = count_patches(parsed);
    finish(result_kind::updated, i18n::trf(str::PATCH_PATCH_YML_ATUALIZADO_PATCHES_R1, {total}),
        fmt::format("patch.yml replaced, %u patches, sha256 %s (%s)", total, hash->second, where));
}
} // namespace utils::ps4::patches

#endif // RPCS3_PS4
