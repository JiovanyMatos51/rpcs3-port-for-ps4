#pragma once

// Games installed from a PS3 disc ("Instalar disco de PS3", ps4_disc_install.hpp) play only while a disc of that game
// is in the PS4 drive.
//
// - The install writes a lock record, /data/rpcs3/data/disc-lock/<TITLE_ID>.txt, before it renames the image into
//   place, so an installed image never exists without its record. The record names the full path of that image.
// - Only that exact image is bound: same full path, same non-zero size, a record that could be read. Any other ISO
//   (a Redump or decrypted ISO of the same game and size, a copy, a renamed or moved file) is an ordinary ISO and
//   boots without the disc. A record that cannot be parsed or read, an unknown size, or a record without an image path
//   locks nothing (fail open, logged); one the app may not open counts as absent, like a folder it cannot list.
//   Records of 0.37/0.38 have no path: their image is the one the install wrote, /data/rpcs3/games/<image>.
// - A bound game is locked unless the drive (ps4_disc_stream.hpp) saw a PS3 disc with that TITLE_ID in a check at
//   most two minutes old. Every copy of a pressing has the same sectors and the FTP server serves no per-copy id, so
//   "the disc" is a disc of that game: the identity (sectors 0-1) and the fingerprint (sectors 0-1, the PVD and
//   PARAM.SFO) only tell the exact pressing from another one with the same TITLE_ID. Both unlock; with
//   accept_other_pressing false only the exact pressing does.
// - The drive is checked when the game boots (X on the XMB, and before BootGame) and every 3 s while it runs: three
//   checks in a row over at least 10 s in which the server says the drive holds no disc (451 at LBA 0), or another
//   disc, are a removal; the disc seen again afterwards is its return (watch events). A check that fails without an
//   answer about the drive (FTP server off, time-out, 550) never counts. The record is a usage rule of this port, not
//   copy protection: deleting it unlocks the game.
// - Without a network GoldHEN's FTP server does not listen, so the drive cannot be checked at all: the screens say so
//   (the network is missing, not the FTP setting). With the offline grace (feature row disc_lock_grace, off unless the
//   player turns it on) such a game opens when a disc of it unlocked it at most grace_days ago and the console clock
//   can be trusted (classify_offline). The last sighting is <registry_dir><TITLE_ID>.seen, one line of unix seconds,
//   written at most once an hour (seen_book); the records themselves keep their format.
//
// Pure part (host-tested): record text, registry, binding, classification, the offline grace and its sightings, the X
// screen (gate) and its texts, the removal watch of a running game.
// PS4 part (RPCS3_PS4): the registry folder, the sightings, the token of a boot the XMB has just verified and the watch
// on the drive service. Log lines start with "disc lock:"; decisions also go to RPCS3.log (disc_stream::log_mirror).

#include "ps4_disc_stream.hpp"
#include "ps4_i18n.hpp"

#include <limits>
#include <map>
#include <set>

namespace utils::ps4::disc_lock
{
namespace ds = utils::ps4::disc_stream;

inline constexpr char registry_dir[] = "/data/rpcs3/data/disc-lock/";
inline constexpr char record_header[] = "rpcs3 disc lock 1";
inline constexpr std::size_t record_limit = 4096;         // bytes of one record file
inline constexpr std::size_t registry_limit = 512;        // record files read from the folder
inline constexpr std::size_t listing_limit = 4096;        // folder entries looked at
inline constexpr std::size_t title_limit = 128;           // bytes of a title kept in a record
inline constexpr std::size_t path_limit = 1024;           // bytes of the image path kept in a record
inline constexpr char legacy_games_dir[] = "/data/rpcs3/games/"; // where the 0.37/0.38 installs wrote every image
inline constexpr std::int64_t max_status_age_ms = 120000; // an older drive status says nothing about the drive now
inline constexpr bool accept_other_pressing = true;       // a disc with the same TITLE_ID and other sectors unlocks too

// The message line of a selected locked game, in the interface language
inline const char* row_hint() noexcept { return i18n::tr(i18n::str::DLOCK_PRECISA_DISCO_LEITOR_PS4_R1); }

// The same for the drive's last status: without a network the drive cannot be seen at all, and the line says so
inline const char* row_hint(const ds::disc_status& s) noexcept
{
    return ds::no_network(s) ? i18n::tr(i18n::str::DOFF_LOCK_SEM_REDE) : row_hint();
}

// Start of the plain XMB notice after a game closed because its disc left the drive (the launcher shows removed_text())
inline constexpr char removed_notice[] = "O disco foi removido.";
inline const char* removed_text() noexcept { return i18n::tr(i18n::str::DLOCK_DISCO_FOI_REMOVIDO); }

inline constexpr std::uint32_t colour_text = 0xffeeeeee, colour_accent = 0xff7ac7ff, colour_good = 0xff8be28b, colour_warn = 0xffffab7a;

namespace detail
{
inline std::string hex64(std::uint64_t value)
{
    char text[20];
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(value));
    return text;
}

inline bool number(std::string_view text, unsigned base, std::uint64_t& out)
{
    if (text.empty() || text.size() > (base == 16 ? 16u : 20u)) return false;
    std::uint64_t value = 0;
    for (const char c : text)
    {
        unsigned digit = 16;
        if (c >= '0' && c <= '9') digit = static_cast<unsigned>(c - '0');
        else if (base == 16 && c >= 'a' && c <= 'f') digit = static_cast<unsigned>(c - 'a' + 10);
        else if (base == 16 && c >= 'A' && c <= 'F') digit = static_cast<unsigned>(c - 'A' + 10);
        if (digit >= base || value > (std::numeric_limits<std::uint64_t>::max() - digit) / base) return false;
        value = value * base + digit;
    }
    out = value;
    return true;
}

// At most 'limit' bytes of 'text' without control characters, cut on a UTF-8 boundary
inline std::string clean(std::string_view text, std::size_t limit)
{
    std::string out;
    for (const char c : text)
        if (static_cast<unsigned char>(c) >= 0x20 && c != 0x7f) out += c;
    if (out.size() > limit)
    {
        std::size_t cut = limit;
        while (cut && (static_cast<unsigned char>(out[cut]) & 0xc0) == 0x80) --cut;
        out.resize(cut);
    }
    return out;
}

// 'text' cut to at most 'limit' bytes on a UTF-8 boundary, "..." marking the cut
inline std::string shorten(const std::string& text, std::size_t limit)
{
    if (text.size() <= limit || limit < 4) return text.substr(0, std::min(text.size(), limit));
    std::size_t cut = limit - 3;
    while (cut && (static_cast<unsigned char>(text[cut]) & 0xc0) == 0x80) --cut;
    return text.substr(0, cut) + "...";
}

// Splits 'text' at spaces into lines of at most 'width' bytes (a longer word stays whole)
inline std::vector<std::string> wrap(std::string_view text, std::size_t width)
{
    std::vector<std::string> out;
    std::string current;
    while (!text.empty())
    {
        const std::size_t space = text.find(' ');
        const std::string_view word = text.substr(0, space);
        text = space == std::string_view::npos ? std::string_view{} : text.substr(space + 1);
        if (!current.empty() && current.size() + 1 + word.size() > width) out.push_back(std::exchange(current, {}));
        if (!current.empty()) current += ' ';
        current += word;
    }
    if (!current.empty()) out.push_back(std::move(current));
    return out;
}
}

// One lock record: the game installed from a disc and what identifies that disc
struct record
{
    std::string title_id;          // "BLES01876"
    std::string title;             // PARAM.SFO TITLE
    std::string version, app_ver;  // PARAM.SFO VERSION and APP_VER (for the log)
    std::uint64_t size = 0;        // bytes of the installed image
    std::uint64_t identity = 0;    // FNV-1a of sectors 0-1 (disc_status::identity)
    std::uint64_t fingerprint = 0; // ds::disc_fingerprint, 0: not known
    std::string image;             // file name of the image when it was installed (for the log)
    std::string path;              // full path of the installed image: the only file bound to this record ("": none)
    bool valid = true;             // false: the file could not be read; it locks nothing
    std::string problem;           // why it is not valid
};

inline std::string file_name(std::string_view title_id)
{
    return std::string(title_id) + ".txt";
}

// "BLES01876.txt" -> "BLES01876"; "" for any other name (the install writes exactly this form)
inline std::string id_of_file(std::string_view name)
{
    if (name.size() != 13 || name.substr(9) != ".txt") return {};
    const std::string id = ds::canonical_title_id(name.substr(0, 9));
    return id == name.substr(0, 9) ? id : std::string{};
}

// An open() error after which a record file counts as absent: missing, or not the app's to read (the folder or the
// file closed to it). Any other failure makes an unreadable record, which is logged and locks nothing either.
inline bool absent_error(int error)
{
    return error == ENOENT || error == ENOTDIR || error == EACCES || error == EPERM;
}

// A record standing for a file of 'title_id' that could not be read
inline record unreadable_record(std::string title_id, std::string why)
{
    record out;
    out.title_id = std::move(title_id);
    out.valid = false;
    out.problem = std::move(why);
    return out;
}

inline std::string to_text(const record& r)
{
    std::string text = std::string(record_header) + "\ntitle_id " + r.title_id + "\n";
    const auto line = [&text](const char* key, const std::string& value)
    {
        if (!value.empty()) text += std::string(key) + ' ' + value + '\n';
    };
    line("title", detail::clean(r.title, title_limit));
    line("version", detail::clean(r.version, 16));
    line("app_ver", detail::clean(r.app_ver, 16));
    text += "size " + std::to_string(r.size) + "\nidentity " + detail::hex64(r.identity) + "\n";
    if (r.fingerprint) text += "fingerprint " + detail::hex64(r.fingerprint) + "\n";
    line("image", detail::clean(r.image, 255));
    line("path", detail::clean(r.path, path_limit));
    return text;
}

// The image path of a record read from a file: its 'path' (absolute), else, for a record of 0.37/0.38, the games
// folder those installs always wrote to and its 'image' file name; "" when neither is usable
inline std::string recorded_path(std::string_view path, std::string_view image)
{
    if (!path.empty()) return path.front() == '/' ? std::string(path) : std::string{};
    if (image.empty() || image == "." || image == ".." || image.find('/') != std::string_view::npos) return {};
    return std::string(legacy_games_dir) + std::string(image);
}

// A record file: the header line, then 'key value' lines (LF or CRLF; unknown keys ignored). title_id, size and
// identity are required. nullopt, with the reason in 'why', for anything else.
inline std::optional<record> parse(std::string_view text, std::string* why = nullptr)
{
    const auto fail = [why](const char* reason) -> std::optional<record>
    {
        if (why) *why = reason;
        return std::nullopt;
    };
    if (text.size() > record_limit) return fail("larger than 4 KiB");
    record out;
    bool first = true, header = false, has_size = false, has_identity = false;
    while (!text.empty())
    {
        const std::size_t eol = text.find('\n');
        std::string_view line = text.substr(0, eol);
        text.remove_prefix(eol == std::string_view::npos ? text.size() : eol + 1);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (std::exchange(first, false))
        {
            header = line == record_header;
            if (!header) break;
            continue;
        }
        const std::size_t space = line.find(' ');
        const std::string_view key = line.substr(0, space);
        const std::string_view value = space == std::string_view::npos ? std::string_view{} : line.substr(space + 1);
        if (key == "title_id") out.title_id = ds::canonical_title_id(value);
        else if (key == "title") out.title = detail::clean(value, title_limit);
        else if (key == "version") out.version = detail::clean(value, 16);
        else if (key == "app_ver") out.app_ver = detail::clean(value, 16);
        else if (key == "size") has_size = detail::number(value, 10, out.size) && out.size;
        else if (key == "identity") has_identity = detail::number(value, 16, out.identity);
        else if (key == "fingerprint" && !detail::number(value, 16, out.fingerprint)) out.fingerprint = 0;
        else if (key == "image") out.image = detail::clean(value, 255);
        else if (key == "path") out.path = detail::clean(value, path_limit);
    }
    if (!header) return fail("no 'rpcs3 disc lock 1' header");
    if (out.title_id.empty()) return fail("no valid title_id");
    if (!has_size) return fail("no valid size");
    if (!has_identity) return fail("no valid identity");
    out.path = recorded_path(out.path, out.image);
    return out;
}

// The records of the registry folder by TITLE_ID
struct registry
{
    std::map<std::string, record, std::less<>> by_id;
    std::map<std::string, std::string, std::less<>> by_path; // image path -> TITLE_ID of the valid record naming it
    std::string unreadable; // the folder could not be listed (errno text); "" when it was read or does not exist
    bool truncated = false; // more than registry_limit record files: the first ones by name were read
    std::vector<std::string> closed; // record files the app may not open (absent_error), with the error
    std::vector<std::string> unbound; // records that lock nothing, with why (for the log)
};

// list(dir, names, error): the entry names, at most listing_limit; false with 'error' (errno) when the folder cannot
// be listed. read(path, limit, error): the first 'limit' bytes of a file; nullopt with 'error' when it cannot be read.
using list_fn = std::function<bool(const std::string& dir, std::vector<std::string>& names, int& error)>;
using read_fn = std::function<std::optional<std::string>(const std::string& path, std::size_t limit, int& error)>;

inline registry load(const list_fn& list, const read_fn& read, const std::string& dir = registry_dir)
{
    registry out;
    std::vector<std::string> names;
    int error = 0;
    if (!list || !list(dir, names, error))
    {
        if (error != ENOENT) out.unreadable = ds::detail::errno_text(error);
        return out;
    }
    std::vector<std::string> ids;
    for (std::size_t i = 0; i < names.size() && i < listing_limit; ++i)
        if (std::string id = id_of_file(names[i]); !id.empty()) ids.push_back(std::move(id));
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    if (ids.size() > registry_limit)
    {
        ids.resize(registry_limit);
        out.truncated = true;
    }
    for (std::string& id : ids)
    {
        int read_error = 0;
        const std::optional<std::string> text = read ? read(dir + file_name(id), record_limit + 1, read_error) : std::nullopt;
        if (!text && absent_error(read_error))
        {
            out.closed.push_back(file_name(id) + " (" + ds::detail::errno_text(read_error) + ")");
            continue;
        }
        std::string why;
        std::optional<record> r = text ? parse(*text, &why) : std::nullopt;
        if (!text) why = "read: " + ds::detail::errno_text(read_error);
        if (r && r->title_id != id)
        {
            why = "it names " + r->title_id;
            r.reset();
        }
        record value = r ? std::move(*r) : unreadable_record(id, why);
        const std::string file = dir + file_name(id);
        if (!value.valid) out.unbound.push_back("record " + file + " unreadable (" + value.problem + "): nothing is locked for " + id);
        else if (value.path.empty()) out.unbound.push_back("record " + file + " has no image path: nothing is locked for " + id);
        else if (const auto [at, added] = out.by_path.try_emplace(value.path, id); !added)
            out.unbound.push_back("record " + file + " names the image '" + value.path + "' of " + at->second + ": nothing is locked for " + id);
        out.by_id.emplace(std::move(id), std::move(value));
    }
    return out;
}

// The valid record naming the image at 'iso_path' (any size), or null
inline const record* recorded_at(const registry& r, std::string_view iso_path)
{
    const auto at = r.by_path.find(iso_path);
    if (at == r.by_path.end()) return nullptr;
    const auto found = r.by_id.find(at->second);
    return found != r.by_id.end() && found->second.valid ? &found->second : nullptr;
}

// The record the image at 'iso_path' with 'iso_size' bytes is bound to, or null: only the exact image an install wrote
// (the full path of its record, the recorded non-zero size). Another path, another size, an unknown size (0) and a
// record that could not be read bind nothing.
inline const record* bound(const registry& r, std::string_view iso_path, std::uint64_t iso_size)
{
    if (!iso_size) return nullptr;
    const record* found = recorded_at(r, iso_path);
    return found && found->size == iso_size ? found : nullptr;
}

// Why an ISO of 'serial' at 'iso_path' with 'iso_size' bytes is not bound although a record of its TITLE_ID exists
// (for one log line); "" when it is bound or no record of the TITLE_ID can be read
inline std::string unbound_reason(const registry& r, std::string_view iso_path, std::string_view serial, std::uint64_t iso_size)
{
    const auto found = r.by_id.find(ds::canonical_title_id(serial));
    if (found == r.by_id.end() || !found->second.valid || found->second.path.empty() || bound(r, iso_path, iso_size)) return {};
    const record& rec = found->second;
    const std::string head = "'" + std::string(iso_path) + "' (" + rec.title_id + ")";
    if (rec.path != iso_path) return head + " is an ordinary ISO (the image installed from the disc is '" + rec.path + "'): not locked";
    if (!iso_size) return head + ": size not known now: not locked";
    return head + ": " + std::to_string(iso_size) + " bytes, its record says " + std::to_string(rec.size) + ": not locked";
}

// The TITLE_ID of the disc of a status: the header's, else the one of its PARAM.SFO
inline std::string disc_id(const ds::disc_status& s)
{
    std::string id = ds::canonical_title_id(s.title_id);
    return id.empty() ? ds::canonical_title_id(s.sfo_title_id) : id;
}

// What the drive's status means for a game
enum class match
{
    unknown,    // not checked yet
    stale,      // the last check is older than max_status_age_ms
    no_server,  // GoldHEN FTP off
    no_disc,
    not_ps3,
    other_disc, // a PS3 disc of another game
    same_title, // a disc of this game, another pressing (or the record is unreadable)
    exact,
};

inline const char* to_string(match m)
{
    switch (m)
    {
    case match::unknown: return "unknown";
    case match::stale: return "stale";
    case match::no_server: return "no FTP server";
    case match::no_disc: return "no disc";
    case match::not_ps3: return "not a PS3 disc";
    case match::other_disc: return "another PS3 disc";
    case match::same_title: return "same TITLE_ID";
    case match::exact: return "exact";
    }
    return "?";
}

inline match classify(const record& r, const ds::disc_status& s, std::int64_t now_ms, std::int64_t max_age_ms = max_status_age_ms)
{
    if (s.checked_ms <= 0) return match::unknown;
    if (now_ms - s.checked_ms > max_age_ms) return match::stale;
    switch (s.state)
    {
    case ds::disc_state::unknown: return match::unknown;
    case ds::disc_state::no_server: return match::no_server;
    case ds::disc_state::no_disc: return match::no_disc;
    case ds::disc_state::not_ps3: return match::not_ps3;
    case ds::disc_state::ps3_disc: break;
    }
    if (r.title_id.empty() || disc_id(s) != r.title_id) return match::other_disc;
    if (!r.valid) return match::same_title;
    if (r.identity && s.identity && r.identity != s.identity) return match::same_title;
    if (r.fingerprint && s.fingerprint && r.fingerprint != s.fingerprint) return match::same_title;
    return match::exact;
}

inline bool unlocks(match m, bool other_pressing = accept_other_pressing)
{
    return m == match::exact || (m == match::same_title && other_pressing);
}

// Why, for the log: "no disc in the drive", "disc in the drive: exact", ...
inline std::string describe(match m, const record& r, const ds::disc_status& s)
{
    switch (m)
    {
    case match::unknown: return "drive not checked yet";
    case match::stale: return "the last drive check is too old";
    case match::no_server: return ds::no_network(s) ? "drive not visible: no network" : "FTP server off";
    case match::no_disc: return "no disc in the drive";
    case match::not_ps3: return "a disc that is not a PS3 game";
    case match::other_disc: return "another PS3 disc: " + (disc_id(s).empty() ? std::string("?") : disc_id(s));
    case match::same_title:
        if (!r.valid) return "same TITLE_ID; the record is unreadable";
        if (r.identity && s.identity && r.identity != s.identity)
            return "same TITLE_ID, another pressing: identity " + detail::hex64(r.identity) + " != " + detail::hex64(s.identity);
        return "same TITLE_ID, another pressing: fingerprint " + detail::hex64(r.fingerprint) + " != " + detail::hex64(s.fingerprint);
    case match::exact: return "disc in the drive: exact";
    }
    return "?";
}

// "BLES01876 'FIFA 14': locked (no disc in the drive)"
inline std::string row_log_line(const record& r, match m, const ds::disc_status& s, bool other_pressing = accept_other_pressing)
{
    return r.title_id + (r.title.empty() ? std::string{} : " '" + r.title + "'") + ": " + (unlocks(m, other_pressing) ? "unlocked (" : "locked (") +
        describe(m, r, s) + ")";
}

// "FIFA 14 (BLES01876)", or the TITLE_ID alone
inline std::string game_name(const record& r)
{
    const std::string title = detail::clean(r.title, title_limit);
    return title.empty() ? r.title_id : title + " (" + r.title_id + ")";
}

// The XMB line after the disc screen was closed without the disc, and the refusal before BootGame (pt-BR <= 116 bytes)
inline std::string message_text(const record& r)
{
    std::string title = detail::clean(r.title, title_limit);
    for (char& c : title)
        if (c == '"') c = '\'';
    title = detail::shorten(title, 60);
    if (title.empty()) return i18n::trf(i18n::str::DLOCK_INSIRA_DISCO_JOGO_ID, {r.title_id});
    return i18n::trf(i18n::str::DLOCK_INSIRA_DISCO_JOGO_TITULO, {title});
}

// How often the XMB asks the drive for a check while a locked game is selected (0: the drive's own cadence is
// enough). Another PS3 disc (checked every 60 s otherwise) or a disc that is not one is read every 10 s, and only while
// the player is active (a press in the last minute): nothing keeps the drive spinning for a menu left alone.
inline std::int64_t forced_check_ms(bool visible, bool selected_locked, match m, std::int64_t idle_ms)
{
    if (!visible || !selected_locked || idle_ms >= 60000) return 0;
    return m == match::other_disc || m == match::not_ps3 ? 10000 : 0;
}

// ---------------------------------------------------------------------------------------------------------------
// The drive without a network, and the offline grace

inline constexpr char seen_suffix[] = ".seen";       // <registry_dir><TITLE_ID>.seen: the last sighting of the game's disc
inline constexpr std::int64_t grace_days = 7;        // a disc seen at most this long ago opens its game offline
inline constexpr std::int64_t seen_rewrite_s = 3600; // a sighting is written again at most once an hour
inline constexpr std::size_t seen_limit = 64;        // bytes of a .seen file read

inline std::string seen_file_name(std::string_view title_id)
{
    return std::string(title_id) + seen_suffix;
}

inline std::string seen_text(std::int64_t unix_seconds)
{
    return std::to_string(unix_seconds) + "\n";
}

// The time of a .seen file: decimal unix seconds, blanks and a line end around them; -1 for anything else
inline std::int64_t parse_seen(std::string_view text)
{
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    std::uint64_t value = 0;
    if (text.size() > 12 || !detail::number(text, 10, value)) return -1;
    return static_cast<std::int64_t>(value);
}

// The drive could not be checked because the console has no network: no FTP server answered while it had no IP
// address (also a check too old, or none counted yet, whose last answer was that). An FTP server switched off on a
// console with a network is not this case: the player can turn it on.
inline bool unseen_offline(match m, const ds::disc_status& s) noexcept
{
    const bool no_server = m == match::no_server || ((m == match::unknown || m == match::stale) && s.state == ds::disc_state::no_server);
    return no_server && ds::no_network(s);
}

enum class offline_verdict
{
    locked,        // no grace: the drive was checked, the grace is off, no sighting is on record or the clock is not trusted
    grace,         // the game opens
    grace_expired, // its disc was last seen more than the grace ago
};

struct offline_decision
{
    offline_verdict verdict = offline_verdict::locked;
    bool offline = false;  // the drive could not be checked for want of a network (unseen_offline)
    std::int64_t days = 0; // grace, grace_expired: whole days since the last sighting
    std::string why;       // for the log; "" when the drive was checked
};

// The offline grace of a bound game whose drive status gave 'm'. 'seen_unix': the last sighting of its disc (-1 none);
// 'now_unix': the console clock (-1 unknown); 'build_date': "YYYY-MM-DD" of this build (a clock before it is wrong);
// 'grace_on': the feature row. Only a sighting at most 'days' old under a clock that is plausible and not earlier than
// the sighting opens the game: a clock moved back or reset gives no grace.
inline offline_decision classify_offline(match m, const ds::disc_status& s, std::int64_t seen_unix, std::int64_t now_unix, std::string_view build_date, bool grace_on,
    std::int64_t days = grace_days)
{
    offline_decision out;
    if (!unseen_offline(m, s)) return out;
    out.offline = true;
    if (!grace_on) out.why = "offline grace off";
    else if (seen_unix < 0) out.why = "no sighting of its disc on record";
    else if (now_unix < 0 || !ds::clock_plausible(now_unix, build_date)) out.why = "console clock " + ds::detail::utc_text(now_unix) + " cannot be trusted";
    else if (now_unix < seen_unix) out.why = "the console clock is earlier than the last sighting (" + ds::detail::utc_text(seen_unix) + ")";
    else
    {
        out.days = (now_unix - seen_unix) / 86400;
        out.verdict = now_unix - seen_unix <= days * 86400 ? offline_verdict::grace : offline_verdict::grace_expired;
        out.why = "disc seen " + std::to_string(out.days) + " day(s) ago, grace " + std::to_string(days) + " days" +
            (out.verdict == offline_verdict::grace_expired ? ": expired" : "");
    }
    return out;
}

// "BLES01876 'FIFA 14': unlocked offline (drive not visible: no network; disc seen 2 day(s) ago, grace 7 days)"
inline std::string offline_log_line(const record& r, const offline_decision& d)
{
    const std::string name = r.title_id + (r.title.empty() ? std::string{} : " '" + r.title + "'");
    if (!d.offline) return name + ": the drive was checked (no offline grace)";
    return name + (d.verdict == offline_verdict::grace ? ": unlocked offline (" : ": locked (") + "drive not visible: no network; " + d.why + ")";
}

// The screen text of an offline grace: "... visto há 2 dia(s): o jogo abre (até 7 dias sem conferir)"; "" otherwise
inline std::string grace_text(const offline_decision& d, std::int64_t days = grace_days)
{
    if (d.verdict != offline_verdict::grace) return {};
    return i18n::trf(i18n::str::DOFF_LOCK_GRACE, {d.days, days});
}

// When a disc of each game last unlocked it (<dir><TITLE_ID>.seen): read once per title and kept; written when none is
// on record, when the one on record is seen_rewrite_s old or more (or later than now: a clock set right again), and
// only under a plausible clock. A refused write is not tried again within the hour either. Not thread-safe.
class seen_book
{
public:
    using read_text_fn = std::function<std::optional<std::string>(const std::string& path)>;
    using write_text_fn = std::function<bool(const std::string& path, const std::string& text)>;

    seen_book(read_text_fn read, write_text_fn write, std::string dir = registry_dir)
        : m_read(std::move(read)), m_write(std::move(write)), m_dir(std::move(dir))
    {
    }

    // The last sighting of a disc of 'title_id' (unix seconds), -1 when none is on record
    std::int64_t seen(std::string_view title_id)
    {
        const std::string id = ds::canonical_title_id(title_id);
        if (id.empty()) return -1;
        if (const auto it = m_known.find(id); it != m_known.end()) return it->second;
        std::int64_t value = -1;
        if (m_read)
            if (const std::optional<std::string> text = m_read(m_dir + seen_file_name(id))) value = parse_seen(*text);
        m_known.emplace(id, value);
        return value;
    }

    // A disc of 'title_id' unlocked its game at 'now_unix'. True when the sighting was written.
    bool note(std::string_view title_id, std::int64_t now_unix, std::string_view build_date)
    {
        const std::string id = ds::canonical_title_id(title_id);
        if (id.empty() || !m_write || now_unix < 0 || !ds::clock_plausible(now_unix, build_date)) return false;
        const std::int64_t stored = seen(id);
        if (stored >= 0 && now_unix >= stored && now_unix - stored < seen_rewrite_s) return false;
        if (const auto it = m_refused.find(id); it != m_refused.end() && now_unix >= it->second && now_unix - it->second < seen_rewrite_s) return false;
        if (!m_write(m_dir + seen_file_name(id), seen_text(now_unix)))
        {
            m_refused[id] = now_unix;
            return false;
        }
        m_refused.erase(id);
        m_known[id] = now_unix;
        ++m_writes;
        return true;
    }

    unsigned writes() const { return m_writes; }

private:
    read_text_fn m_read;
    write_text_fn m_write;
    std::string m_dir;
    std::map<std::string, std::int64_t, std::less<>> m_known;   // title id -> sighting on record (-1 none)
    std::map<std::string, std::int64_t, std::less<>> m_refused; // title id -> time of a write that failed
    unsigned m_writes = 0;
};

// One line of the full-screen disc screen (pixels from the top, like the install question)
struct screen_line
{
    int y = 0;
    std::string text;
    std::uint32_t colour = colour_text;
    bool operator==(const screen_line&) const = default;
};

enum class step
{
    verifying,  // waiting for a drive check newer than X
    missing,    // the disc of the game is not in the drive: the screen asks for it
    recognized, // it went in: X plays
    boot,
    back,
};

struct gate_config
{
    std::int64_t grace_ms = 3000;        // a check this much older than X still decides at once
    std::int64_t verify_retry_ms = 1000; // the check is asked again while none newer than X has come
    std::int64_t slow_ms = 8000;         // then the screen says the drive is slow
    std::int64_t give_up_ms = 25000;     // still no answer (and the drive not busy): the missing screen ("O leitor não respondeu")
    std::int64_t screen_check_ms = 2000; // the missing and recognized screens ask for a check this often
    std::int64_t max_age_ms = max_status_age_ms;
    bool other_pressing = accept_other_pressing;
    // The offline grace (classify_offline): the feature row, the last sighting of the game's disc (-1 none), the console
    // clock when X was pressed (unix seconds, -1 unknown) and this build's date. The grace text stays up this long before
    // the boot.
    bool offline_grace = false;
    std::int64_t seen_unix = -1;
    std::int64_t clock_unix = -1;
    std::string build_date;
    std::int64_t grace_notice_ms = 2000;
};

// X on a locked game. The adapter feeds the drive's last status and the pressed buttons; it asks the drive for a
// check when want_check() is set, draws lines() and boots on step::boot. Only a check that started after X (or at most
// grace_ms before it) decides, so a disc removed just before X is never taken for present.
class gate
{
public:
    gate(record r, std::string subject, std::int64_t pressed_ms, gate_config cfg = {})
        : m_record(std::move(r)), m_subject(std::move(subject)), m_cfg(cfg), m_pressed(pressed_ms), m_now(pressed_ms), m_entered(pressed_ms),
          m_next_ask(pressed_ms)
    {
    }

    // drive_busy: another reader of the shared disc reader is working (the disc key search). Its read of a block past a
    // 2 GiB boundary the server cannot start a transfer at goes forward for up to 2 GiB and the check waits behind it,
    // so verifying does not give up while it runs.
    step update(const ds::disc_status& s, std::int64_t now_ms, bool cross, bool circle, bool drive_busy = false)
    {
        m_now = now_ms;
        m_want = false;
        if (m_step == step::boot || m_step == step::back) return m_step;
        if (s.checked_ms > 0 && s.checked_ms + m_cfg.grace_ms >= m_pressed)
        {
            m_match = classify(m_record, s, now_ms, m_cfg.max_age_ms);
            m_status = s;
            m_counted = true;
            m_offline = classify_offline(m_match, s, m_cfg.seen_unix, m_cfg.clock_unix, m_cfg.build_date, m_cfg.offline_grace);
        }
        const bool open = m_counted && unlocks(m_match, m_cfg.other_pressing);
        if (circle)
        {
            log("'" + m_subject + "': screen closed with Circle (" + reason() + ")");
            m_step = step::back;
            return m_step;
        }
        switch (m_step)
        {
        case step::verifying:
            if (open)
            {
                log("'" + m_subject + "': disc " + m_record.title_id + " present (" + to_string(m_match) + ", checked in " + std::to_string(now_ms - m_pressed) +
                    " ms): booting");
                m_step = step::boot;
            }
            else if (m_counted && m_offline.verdict == offline_verdict::grace)
            {
                // The drive cannot be seen without a network and a disc of the game unlocked it lately: the grace text,
                // then the boot
                if (!m_grace_shown)
                {
                    m_grace_shown = true;
                    m_grace_at = now_ms;
                    log("'" + m_subject + "': " + offline_log_line(m_record, m_offline));
                }
                if (now_ms - m_grace_at >= m_cfg.grace_notice_ms) m_step = step::boot;
            }
            else if (m_counted)
            {
                log("'" + m_subject + "' not booted: " + reason() + " (state " + ds::to_string(m_status.state) +
                    (m_status.detail.empty() ? std::string{} : ", " + m_status.detail) + ")" + (m_offline.offline ? "; " + m_offline.why : std::string{}));
                enter(step::missing);
            }
            else if (now_ms - m_pressed >= m_cfg.give_up_ms && !drive_busy)
            {
                log("'" + m_subject + "' not booted: no drive check came in " + std::to_string((now_ms - m_pressed) / 1000) + " s");
                enter(step::missing);
            }
            else
            {
                if (now_ms - m_pressed >= m_cfg.give_up_ms && !std::exchange(m_busy_logged, true))
                    log("'" + m_subject + "': the drive is busy with the disc key search: waiting for it");
                ask(m_cfg.verify_retry_ms);
            }
            break;
        case step::missing:
            if (open)
            {
                log("disc " + m_record.title_id + " inserted while the screen was shown (after " + std::to_string((now_ms - m_entered) / 1000) + " s, " +
                    to_string(m_match) + ")");
                enter(step::recognized);
            }
            else ask(m_cfg.screen_check_ms);
            break;
        case step::recognized:
            if (!open)
            {
                log("disc " + m_record.title_id + " no longer seen while the screen was shown (" + reason() + ")");
                enter(step::missing);
            }
            else if (cross)
            {
                log("'" + m_subject + "': disc " + m_record.title_id + " present (" + to_string(m_match) + ", X on the screen): booting");
                m_step = step::boot;
            }
            else ask(m_cfg.screen_check_ms);
            break;
        case step::boot:
        case step::back:
            break;
        }
        return m_step;
    }

    step current() const { return m_step; }
    bool want_check() const { return m_want; }
    match last_match() const { return m_counted ? m_match : match::unknown; }
    const record& game() const { return m_record; }
    // The offline grace of the last counted check (verdict locked and offline false while the drive answers)
    const offline_decision& offline() const { return m_offline; }

    // While the grace text is up before an offline boot: that text (interface language); "" otherwise
    std::string grace_note() const
    {
        return m_grace_shown && (m_step == step::verifying || m_step == step::boot) ? grace_text(m_offline) : std::string{};
    }

    // A line for startup.log (without the "disc lock: " prefix) when the last update had one, else ""
    std::string take_log() { return std::exchange(m_log, {}); }

    std::vector<screen_line> lines() const
    {
        std::vector<screen_line> out;
        int y = 210;
        const auto add = [&](std::string text, std::uint32_t colour = colour_text)
        {
            out.push_back({y, std::move(text), colour});
            y += 45;
        };
        using i18n::str;
        const std::string name = detail::shorten(game_name(m_record), 70);
        switch (m_step)
        {
        case step::verifying:
        case step::boot:
        case step::back:
            out.push_back({80, i18n::tr(str::COM_VERIFICANDO_DISCO), colour_accent});
            out.push_back({140, name});
            if (const std::string note = grace_note(); !note.empty())
                for (std::string& line : detail::wrap(note, 70)) add(std::move(line), colour_good);
            else add(i18n::tr(m_now - m_pressed >= m_cfg.slow_ms ? str::COM_LEITOR_ESTA_DEMORANDO_RESPONDER_AGUARDE : str::COM_PROCURANDO_DISCO_DESTE_JOGO_LEITOR));
            y += 20;
            add(i18n::tr(str::DLOCK_CIRCULO_VOLTAR), colour_accent);
            break;
        case step::missing:
            out.push_back({80, i18n::tr(str::DLOCK_COLOQUE_DISCO_JOGO_PS4), colour_warn});
            out.push_back({140, name});
            add(i18n::tr(str::DLOCK_ESTE_JOGO_FOI_INSTALADO_PARTIR));
            add(i18n::tr(str::DLOCK_JOGAR_DISCO_PRECISA_ESTAR_LEITOR));
            for (std::string& text : reason_lines()) add(std::move(text), colour_warn);
            y += 20;
            add(i18n::tr(str::DLOCK_COLOQUE_DISCO_ESTA_TELA_MUDA), colour_accent);
            break;
        case step::recognized:
            out.push_back({80, i18n::tr(str::DLOCK_DISCO_RECONHECIDO), colour_good});
            out.push_back({140, name});
            add(i18n::tr(str::DLOCK_CADEADO_SAIU_JA_JOGAR));
            y += 20;
            add(i18n::tr(str::DLOCK_JOGAR_CIRCULO_VOLTAR), colour_accent);
            break;
        }
        return out;
    }

    // What the missing screen says about the drive (interface language, at most 70 bytes per line)
    std::vector<std::string> reason_lines() const
    {
        using i18n::str;
        std::vector<std::string> out;
        const auto add = [&out](const std::string& text)
        {
            for (std::string& line : detail::wrap(text, 70)) out.push_back(std::move(line));
        };
        switch (last_match())
        {
        case match::no_disc: add(i18n::tr(str::DLOCK_AGORA_NAO_HA_NENHUM)); break;
        case match::other_disc:
        {
            std::string title = detail::clean(m_status.title, title_limit);
            for (char& c : title)
                if (c == '"') c = '\'';
            const std::string id = disc_id(m_status);
            add(i18n::tr(str::DLOCK_DISCO_LEITOR_OUTRO_JOGO));
            if (!title.empty()) add("\"" + detail::shorten(title, 40) + "\"" + (id.empty() ? std::string{} : " (" + id + ")") + ".");
            else if (!id.empty()) add(id + ".");
            add(i18n::tr(str::DLOCK_TROQUE_DISCO_DESTE_JOGO));
            break;
        }
        case match::not_ps3: add(i18n::tr(str::DLOCK_DISCO_LEITOR_NAO_DISCO)); break;
        case match::no_server:
            // Without an IP address GoldHEN's FTP server does not listen: the network is missing, not the FTP setting
            if (ds::no_network(m_status))
            {
                add(i18n::tr(str::DOFF_LOCK_SEM_REDE));
                if (m_offline.verdict == offline_verdict::grace_expired) add(i18n::trf(str::DOFF_LOCK_GRACE_FIM, {grace_days}));
                else add(i18n::tr(str::DOFF_LOCK_CONECTE_REDE));
                break;
            }
            add(i18n::tr(str::DLOCK_APP_NAO_CONSEGUE_VER_LEITOR));
            add(i18n::tr(str::DLOCK_LIGUE_FTP_CONFIGURACOES_GOLDHEN_SERVER));
            add(i18n::tr(str::DLOCK_APP_LE_DISCO_ELE));
            break;
        case match::unknown:
        case match::stale:
        case match::same_title:
        case match::exact:
            add(i18n::tr(str::DLOCK_LEITOR_NAO_RESPONDEU_TIRE_DISCO));
            break;
        }
        return out;
    }

private:
    record m_record;
    std::string m_subject;
    gate_config m_cfg;
    std::int64_t m_pressed = 0, m_now = 0, m_entered = 0, m_next_ask = 0;
    step m_step = step::verifying;
    match m_match = match::unknown;
    ds::disc_status m_status;
    offline_decision m_offline;
    std::int64_t m_grace_at = 0;
    bool m_counted = false, m_want = false, m_busy_logged = false, m_grace_shown = false;
    std::string m_log;

    std::string reason() const { return describe(last_match(), m_record, m_status); }

    void log(std::string line) { m_log = std::move(line); }

    void enter(step next)
    {
        m_step = next;
        m_entered = m_now;
        m_next_ask = m_now + m_cfg.screen_check_ms;
    }

    void ask(std::int64_t interval_ms)
    {
        if (m_now < m_next_ask) return;
        m_want = true;
        m_next_ask = m_now + interval_ms;
    }
};

// The boot the XMB has just verified, so the check before BootGame does not read the drive again. One use.
class verified_boots
{
public:
    void remember(std::string path, std::int64_t now_ms)
    {
        m_path = std::move(path);
        m_at = now_ms;
    }

    // True once for the path remembered at most window_ms ago
    bool take(const std::string& path, std::int64_t now_ms, std::int64_t window_ms = 30000)
    {
        if (m_path.empty() || path != m_path || now_ms < m_at || now_ms - m_at > window_ms) return false;
        m_path.clear();
        return true;
    }

private:
    std::string m_path;
    std::int64_t m_at = 0;
};

// What one drive check says about the disc of a running game. Only an answer of the server about the drive is a
// miss: no disc (451 or an early end at LBA 0, ds::drive_answered_empty), a disc that is not a PS3 game, the disc of
// another game. FTP server off, time-outs, dropped connections and a refused RETR (550) say nothing about the disc.
enum class sighting
{
    present, // a disc that unlocks the game
    missing,
    unknown, // the check failed without an answer about the drive: never counted
};

inline sighting sight(const record& r, const ds::disc_status& s, bool other_pressing = accept_other_pressing)
{
    if (s.checked_ms <= 0) return sighting::unknown;
    switch (s.state)
    {
    case ds::disc_state::unknown:
    case ds::disc_state::no_server: return sighting::unknown;
    case ds::disc_state::no_disc: return ds::drive_answered_empty(s) ? sighting::missing : sighting::unknown;
    case ds::disc_state::not_ps3: return sighting::missing;
    case ds::disc_state::ps3_disc: break;
    }
    return unlocks(classify(r, s, s.checked_ms), other_pressing) ? sighting::present : sighting::missing;
}

struct watch_config
{
    std::int64_t check_ms = 3000;      // a check is asked this often (sectors 0-1, a drive cache hit: about 4 ms)
    unsigned confirm_misses = 3;       // checks in a row without the disc that make a removal
    std::int64_t confirm_ms = 10000;   // ... spanning at least this long from the first of them to the last
    std::int64_t answer_ms = 15000;    // a request still unanswered after this is made again
    std::int64_t silent_ms = 30000;    // no check for this long is logged (no drive worker, e.g. at the thread limit)
    std::int64_t error_log_ms = 60000; // checks without an answer: the first one logged, then at most one line a minute
    unsigned change_lines = 20;        // misses and sightings after a miss: this many lines, then at most one a minute
    bool other_pressing = accept_other_pressing;
};

// What one step of the removal watch found
enum class watch_event
{
    none,
    removed, // the disc left the drive
    back,    // after a removal: a disc of the game is in the drive again
};

// The disc of a running game installed from a disc. The adapter feeds the drive's last status (any age: each check
// that started after the watch began counts once) and asks the drive for a check when want_check() is set. step()
// returns removed once the disc was missing in confirm_misses checks in a row spanning at least confirm_ms, and back
// once a disc of the game is seen again after that; the watch goes on after either. A check without an answer about
// the drive is neither a miss nor a sighting; a sighting of the disc forgets the misses. A check is asked check_ms after
// the previous request once that one was answered, so a slow drive is never asked twice.
class removal_watch
{
public:
    removal_watch(record r, std::int64_t started_ms, watch_config cfg = {})
        : m_record(std::move(r)), m_cfg(cfg), m_started(started_ms), m_next_ask(started_ms + cfg.check_ms)
    {
        m_cfg.confirm_misses = std::max(1u, m_cfg.confirm_misses);
    }

    // True once per removal, when the disc left the drive
    bool update(const ds::disc_status& s, std::int64_t now_ms) { return step(s, now_ms) == watch_event::removed; }

    watch_event step(const ds::disc_status& s, std::int64_t now_ms)
    {
        m_want = false;
        watch_event event = watch_event::none;
        if (s.checked_ms > m_seen && s.checked_ms >= m_started)
        {
            if (std::exchange(m_silent, false))
                log("the drive answers again after " + std::to_string((s.checked_ms - std::max(m_seen, m_started)) / 1000) + " s without a check");
            m_seen = s.checked_ms;
            if (s.checked_ms >= m_asked) m_answered = true;
            ++m_checks;
            event = count(s, now_ms);
        }
        const std::int64_t last = std::max(m_seen, m_started);
        if (!m_silent && now_ms - last >= m_cfg.silent_ms)
        {
            m_silent = true;
            log("no drive check came in " + std::to_string((now_ms - last) / 1000) + " s: the disc of " + m_record.title_id +
                " is not watched until the drive answers");
        }
        if (now_ms >= m_next_ask && (m_answered || now_ms - m_asked >= m_cfg.answer_ms))
        {
            m_want = true;
            m_answered = false;
            m_asked = now_ms;
            m_next_ask = now_ms + m_cfg.check_ms;
        }
        return event;
    }

    bool want_check() const { return m_want; }
    bool removed() const { return m_removed; } // the disc is away now (removed, not back yet)
    const record& game() const { return m_record; }
    const watch_config& config() const { return m_cfg; }
    unsigned checks() const { return m_checks; }
    unsigned misses() const { return m_misses; } // in a row, now
    unsigned unanswered() const { return m_unanswered; }
    unsigned removals() const { return m_removals; }

    // Lines for startup.log (without the "disc lock: " prefix) since the last call
    std::vector<std::string> take_log() { return std::exchange(m_log, {}); }

    // "312 checks, 1 without the disc, 2 without an answer" (+ ", 1 removal(s)")
    std::string summary() const
    {
        return std::to_string(m_checks) + " checks, " + std::to_string(m_missing) + " without the disc, " + std::to_string(m_unanswered) +
            " without an answer" + (m_removals ? ", " + std::to_string(m_removals) + " removal(s)" : std::string{});
    }

private:
    record m_record;
    watch_config m_cfg;
    std::int64_t m_started = 0, m_next_ask = 0, m_asked = 0, m_seen = 0, m_first_miss = 0, m_removed_at = 0, m_unanswered_logged = 0, m_change_logged = 0;
    unsigned m_checks = 0, m_misses = 0, m_missing = 0, m_unanswered = 0, m_unanswered_quiet = 0, m_changes = 0, m_changes_quiet = 0, m_removals = 0;
    bool m_want = false, m_answered = true, m_removed = false, m_silent = false, m_unanswered_logged_once = false;
    std::vector<std::string> m_log;

    void log(std::string line)
    {
        if (m_log.size() < 16) m_log.push_back(std::move(line));
    }

    // A miss or a sighting after a miss: a disc that keeps coming and going is not logged on every check
    void log_change(std::string line, std::int64_t now_ms)
    {
        if (++m_changes > m_cfg.change_lines && now_ms - m_change_logged < m_cfg.error_log_ms)
        {
            ++m_changes_quiet;
            return;
        }
        if (m_changes_quiet) line += " (" + std::to_string(m_changes_quiet) + " such lines left out)";
        m_changes_quiet = 0;
        m_change_logged = now_ms;
        log(std::move(line));
    }

    // The reason of a check, for the log
    std::string why(const ds::disc_status& s, sighting v) const
    {
        const std::string detail = s.detail.empty() ? std::string{} : ": " + s.detail;
        if (v == sighting::missing) return describe(classify(m_record, s, s.checked_ms), m_record, s) + detail;
        if (s.state == ds::disc_state::no_server) return (ds::no_network(s) ? "no network" : "FTP server off") + detail;
        if (s.state == ds::disc_state::unknown) return "the drive was not read";
        return "no answer about the drive" + detail;
    }

    watch_event count(const ds::disc_status& s, std::int64_t now_ms)
    {
        const sighting v = sight(m_record, s, m_cfg.other_pressing);
        switch (v)
        {
        case sighting::present:
            if (m_removed)
            {
                m_removed = false;
                m_misses = 0;
                log(game_name(m_record) + ": in the drive again " + std::to_string((s.checked_ms - m_removed_at) / 1000) + " s after the removal (" +
                    to_string(classify(m_record, s, s.checked_ms)) + "): the game can go on");
                return watch_event::back;
            }
            if (m_misses)
                log_change(m_record.title_id + " is in the drive again " + std::to_string(s.checked_ms - m_first_miss) + " ms after the check without it: the game goes on",
                    now_ms);
            m_misses = 0;
            return watch_event::none;
        case sighting::missing:
            ++m_missing;
            if (m_removed) return watch_event::none; // still away
            if (!m_misses)
            {
                m_misses = 1;
                m_first_miss = s.checked_ms;
                log_change(m_record.title_id + " not in the drive (" + why(s, v) + "): " + std::to_string(m_cfg.confirm_misses) + " checks over " +
                    std::to_string(m_cfg.confirm_ms / 1000) + " s decide", now_ms);
                if (m_cfg.confirm_misses > 1 || m_cfg.confirm_ms > 0) return watch_event::none;
            }
            else ++m_misses;
            if (m_misses < m_cfg.confirm_misses || s.checked_ms - m_first_miss < m_cfg.confirm_ms) return watch_event::none;
            m_removed = true;
            m_removed_at = s.checked_ms;
            ++m_removals;
            log(game_name(m_record) + ": not in the drive (" + why(s, v) + "; " + std::to_string(m_misses) + " checks over " +
                std::to_string((s.checked_ms - m_first_miss) / 1000) + " s): disc removed during the game");
            return watch_event::removed;
        case sighting::unknown:
            ++m_unanswered;
            if (m_unanswered_logged_once && now_ms - m_unanswered_logged < m_cfg.error_log_ms)
            {
                ++m_unanswered_quiet;
                return watch_event::none;
            }
            log("drive check without an answer about the disc (" + why(s, v) + "): not counted" +
                (m_unanswered_quiet ? " (" + std::to_string(m_unanswered_quiet) + " more since the last line)" : std::string{}));
            m_unanswered_logged_once = true;
            m_unanswered_logged = now_ms;
            m_unanswered_quiet = 0;
            return watch_event::none;
        }
        return watch_event::none;
    }
};
}

#ifdef RPCS3_PS4
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wold-style-cast"
#endif
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __clang__
#pragma clang diagnostic pop
#endif

#include <ctime>

#include "util/ps4_build_version.hpp"
#include "util/ps4_safe_mode.hpp"

namespace utils::ps4::disc_lock::sys
{
inline void note(const std::string& line)
{
    ps4_startup_checkpoint(("disc lock: " + line).c_str());
}

// A decision about a game's disc (booted, refused, unlocked offline, removed during the game): startup.log and
// RPCS3.log, the log testers send most
inline void decision(const std::string& line)
{
    note(line);
    ds::mirror("disc lock: " + line);
}

inline bool list_folder(const std::string& dir, std::vector<std::string>& names, int& error)
{
    DIR* folder = ::opendir(dir.c_str());
    if (!folder)
    {
        error = errno ? errno : EIO;
        return false;
    }
    while (const dirent* entry = ::readdir(folder))
    {
        if (names.size() >= listing_limit) break;
        const std::string_view name = entry->d_name;
        if (name != "." && name != "..") names.emplace_back(name);
    }
    ::closedir(folder);
    return true;
}

// The first 'limit' bytes of a file; 'opened' tells an open failure from a read failure
inline std::optional<std::string> read_file(const std::string& path, std::size_t limit, int& error, bool* opened = nullptr)
{
    if (opened) *opened = false;
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
    {
        error = errno ? errno : EIO;
        return std::nullopt;
    }
    if (opened) *opened = true;
    std::string text(limit, '\0');
    std::size_t done = 0;
    while (done < limit)
    {
        const ssize_t got = ::read(fd, text.data() + done, limit - done);
        if (got > 0) done += static_cast<std::size_t>(got);
        else if (got < 0 && errno == EINTR) continue;
        else if (got < 0)
        {
            error = errno ? errno : EIO;
            ::close(fd);
            return std::nullopt;
        }
        else break;
    }
    ::close(fd);
    text.resize(done);
    return text;
}

// The registry of the console: one listing of the folder and one read per record (at most 512 x 4 KiB). Logged when
// the records or the folder error change.
inline registry load_registry()
{
    registry r = load(list_folder, [](const std::string& path, std::size_t limit, int& error) { return read_file(path, limit, error); });
    std::string summary;
    std::vector<std::string> problems;
    if (!r.unreadable.empty()) summary = std::string("registry folder ") + registry_dir + " unreadable (" + r.unreadable + "): no game is locked";
    else
    {
        summary = "registry: " + std::to_string(r.by_id.size()) + " game(s) installed from disc";
        std::string list;
        std::size_t shown = 0;
        for (const auto& [id, rec] : r.by_id)
        {
            if (shown++ < 8)
                list += (list.empty() ? "" : ", ") + id + (!rec.valid ? std::string(" unreadable") : rec.path.empty() ? std::string(" without an image path")
                    : " at '" + rec.path + "' " + std::to_string(rec.size >> 20) + " MiB");
        }
        if (shown > 8) list += ", +" + std::to_string(shown - 8) + " more";
        if (!list.empty()) summary += " (" + list + ")";
        if (r.truncated) summary += "; only the first " + std::to_string(registry_limit) + " records are read";
        problems = r.unbound;
        for (const std::string& closed : r.closed) problems.push_back("record " + std::string(registry_dir) + closed + " cannot be opened: not locked");
        if (!r.closed.empty()) summary += "; " + std::to_string(r.closed.size()) + " record file(s) cannot be opened";
    }
    static std::mutex mutex;
    static std::string last;
    std::lock_guard lock(mutex);
    if (summary != last)
    {
        last = summary;
        note(summary);
        for (const std::string& line : problems) note(line);
    }
    return r;
}

// A line logged once per process (at most 256 different lines, then nothing more)
inline void note_once(const std::string& line)
{
    static std::mutex mutex;
    static std::set<std::string>* const seen = new std::set<std::string>(); // never destroyed
    std::lock_guard lock(mutex);
    if (seen->size() >= 256 || !seen->insert(line).second) return;
    note(line);
}

// The lock record the image at 'path' with 'iso_size' bytes is bound to, read now (the folder listing and at most 512
// records of 4 KiB, as load_registry), or none: only the exact image of a readable record with that non-zero size is
// bound (bound()). For an ISO of a TITLE_ID that has a record but is not that image, 'title_id' names it in one line.
inline std::optional<record> record_for_path(const std::string& path, std::uint64_t iso_size, std::string_view title_id = {})
{
    if (path.empty()) return std::nullopt;
    const registry all = load_registry();
    if (const record* found = bound(all, path, iso_size)) return *found;
    if (const std::string why = unbound_reason(all, path, title_id, iso_size); !why.empty()) note_once(why);
    return std::nullopt;
}

// A small file of the registry folder replaced at once (written as "<path>.tmp", then renamed over it); the folder is
// made 0777 when it is missing. Only below registry_dir.
inline bool write_small(const std::string& path, const std::string& text)
{
    if (!path.starts_with(registry_dir) || path.find("/../") != std::string::npos) return false;
    const std::string temp = path + ".tmp";
    int fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    if (fd < 0 && errno == ENOENT)
    {
        const std::string folder(registry_dir, sizeof(registry_dir) - 2);
        ::mkdir(folder.c_str(), 0777);
        fd = ::open(temp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
    }
    if (fd < 0) return false;
    bool ok = ::write(fd, text.data(), text.size()) == static_cast<ssize_t>(text.size());
    ok = ::close(fd) == 0 && ok;
    if (ok && ::rename(temp.c_str(), path.c_str()) == 0) return true;
    ::unlink(temp.c_str());
    return false;
}

struct seen_state
{
    std::mutex mutex;
    seen_book book{[](const std::string& path) -> std::optional<std::string>
                   {
                       int error = 0;
                       return read_file(path, seen_limit, error);
                   },
        write_small};
};

inline seen_state& sightings()
{
    static seen_state* const s = new seen_state(); // never destroyed: read on the way out of the XMB
    return *s;
}

inline std::int64_t clock_now()
{
    return static_cast<std::int64_t>(std::time(nullptr));
}

// A disc of 'title_id' unlocked its game at 'now_unix' (the XMB's padlock rows, X, the check before BootGame): kept as
// its last sighting, written at most once an hour. Cheap enough for every frame (a map lookup) after the first read.
inline void note_seen(const std::string& title_id, std::int64_t now_unix = clock_now())
{
    seen_state& s = sightings();
    std::lock_guard lock(s.mutex);
    if (s.book.note(title_id, now_unix, ps4_build_date)) note(title_id + ": disc seen, kept in " + registry_dir + seen_file_name(title_id));
}

// The last sighting of a disc of 'title_id' (unix seconds), -1 when none is on record
inline std::int64_t last_seen(const std::string& title_id)
{
    seen_state& s = sightings();
    std::lock_guard lock(s.mutex);
    return s.book.seen(title_id);
}

// The offline grace now for a game whose drive status gave 'm' (the feature row disc_lock_grace, its last sighting,
// the console clock, this build's date)
inline offline_decision decide_offline(const record& r, match m, const ds::disc_status& s)
{
    if (!unseen_offline(m, s)) return {};
    const bool on = utils::ps4::safe_mode::enabled(utils::ps4::safe_mode::feature::disc_lock_grace);
    return classify_offline(m, s, last_seen(r.title_id), clock_now(), ps4_build_date, on);
}

// The X screen's settings for a record: the defaults and the offline grace of this console now
inline gate_config gate_settings(const record& r)
{
    gate_config cfg;
    cfg.offline_grace = utils::ps4::safe_mode::enabled(utils::ps4::safe_mode::feature::disc_lock_grace);
    cfg.seen_unix = last_seen(r.title_id);
    cfg.clock_unix = clock_now();
    cfg.build_date = ps4_build_date;
    return cfg;
}

struct token_state
{
    std::mutex mutex;
    verified_boots boots;
};

inline token_state& tokens()
{
    static token_state* const t = new token_state(); // never destroyed: read on the way out of the XMB
    return *t;
}

// The XMB verified the disc for 'path' just now: the check before BootGame trusts it for 30 s
inline void remember_verified(const std::string& path)
{
    token_state& t = tokens();
    std::lock_guard lock(t.mutex);
    t.boots.remember(path, ds::detail::now_ms());
}

inline bool take_verified(const std::string& path)
{
    token_state& t = tokens();
    std::lock_guard lock(t.mutex);
    return t.boots.take(path, ds::detail::now_ms());
}

// The disc of the game running now, when it was installed from a disc (removal_watch). The drive is read by the
// drive service's worker thread, the one the XMB polls; tick_event() only takes the service's mutex to read its last status
// and to ask for the next check, so it never waits for the drive or the network. Main thread.
class game_watch
{
public:
    static constexpr std::int64_t tick_ms = 250;

    // Watches the game booted from 'path' when it is the exact image of a lock record with 'iso_size' bytes
    // (record_for_path; the drive's own image is never watched). 'title_id' only names an ISO of a recorded game that
    // is not that image in the log. Ends an earlier watch.
    void start(const std::string& path, std::string_view title_id, std::uint64_t iso_size)
    {
        stop("another boot");
        if (path.empty() || path.starts_with(ds::device_prefix)) return;
        std::optional<record> r = record_for_path(path, iso_size, title_id);
        if (!r) return;
        const std::int64_t now = ds::detail::now_ms();
        m_watch.emplace(std::move(*r), now);
        m_next = now;
        const watch_config& cfg = m_watch->config();
        decision("watching the drive while '" + path + "' runs (" + game_name(m_watch->game()) + "): a check every " + std::to_string(cfg.check_ms / 1000) +
            " s; " + std::to_string(cfg.confirm_misses) + " checks without its disc over " + std::to_string(cfg.confirm_ms / 1000) + " s are a removal");
    }

    void stop(const char* why)
    {
        if (!m_watch) return;
        decision("watch of " + m_watch->game().title_id + " ended (" + why + "; " + m_watch->summary() + ")");
        m_watch.reset();
    }

    bool active() const { return m_watch.has_value(); }

    // The disc is away now (a removal without its return yet)
    bool removed() const { return m_watch && m_watch->removed(); }

    // tick_event() would look at the drive's status now (at most every tick_ms)
    bool due() const { return m_watch && ds::detail::now_ms() >= m_next; }

    // removed once when the disc left the drive, back once when a disc of the game is in it again; the watch goes on
    // (the caller pauses and resumes the game). start_worker false (close to the thread limit): a stopped drive worker
    // is not started again; a running one is kept alive.
    watch_event tick_event(bool start_worker)
    {
        if (!m_watch) return watch_event::none;
        const std::int64_t now = ds::detail::now_ms();
        if (now < m_next) return watch_event::none;
        m_next = now + tick_ms;
        ds::service& drive = ds::service::instance();
        const watch_event event = m_watch->step(drive.poll(start_worker), now);
        if (m_watch->want_check()) drive.request_check();
        // Bounded by the watch itself (16 lines per step, misses and unanswered checks rate-limited)
        for (const std::string& line : m_watch->take_log()) decision(line);
        return event;
    }

private:
    std::optional<removal_watch> m_watch;
    std::int64_t m_next = 0;
};
}
#endif
