#pragma once

// 0.33: the PC per-game config database (GuiConfigs/config_database.dat of a PC RPCS3 install). PC RPCS3 applies
// games[<serial>].config at boot to every title that has no custom config (System.cpp, cfg_mode::custom, with the
// Qt frontend's get_database_config). The PS4 build never downloads it and the PKG does not carry it (its licence
// was never checked): the user copies the file to /data/rpcs3/config_database.dat (FTP or the launcher's import).
// util/ps4_policy.hpp applies the entry of the booting title, in its own order (see apply_config_database there).
//
// The file is the server's JSON exactly as the PC wrote it: {"return_code": 0, "timestamp": N, "games":
// {"<serial>": {"config": "<YAML text>"}, ...}}. It is read with the small strict JSON reader below rather than
// yaml-cpp: it accepts exactly what Qt's QJsonDocument accepts on PC (yaml-cpp would also take YAML-only text),
// it builds no node tree for the ~300 KB document (only the serial -> text map is kept), and a damaged file can only
// fail the parse: the RPCS3 targets on PS4 are built with -fno-exceptions. Read once per process; read again only
// when the file's size or modification time changes (an import while the XMB is open). Header-only and
// exception-free; host-tested by ps4-port/tests/policy_test.cpp.
//
// 0.37: the package may carry a read-only copy at /app0/config_database.dat. The /data copy (the user's import) is
// used when it is valid, unless both copies carry a 'timestamp' and the bundled one is newer; without a usable /data
// copy the bundled one is used. Nothing changes when the package has no copy. file_info says which copy runs.
//
// 0.40: the boot policy can skip one title's entry (set_skipped: config-database-off.flag, or the per-game safe mode
// from level 2); the title then runs as without a database.

#include "Utilities/File.h"

#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

namespace utils::ps4::config_db
{
inline constexpr char path[] = "/data/rpcs3/config_database.dat";
// 0.37: the copy the package may carry (read-only).
inline constexpr char bundled_path[] = "/app0/config_database.dat";
// The 2026 database is 296 KB; anything this large is not a config database.
inline constexpr std::uint64_t max_size = std::uint64_t{16} << 20;

enum class file_state
{
    absent,     // no file: nothing is applied
    unreadable, // could not be opened or read
    too_large,  // above max_size
    invalid,    // not the PC JSON (parse error, return_code < 0, no 'games' object)
    loaded,
};

// 0.37: which copy a file_info describes.
enum class file_source
{
    data,    // /data/rpcs3/config_database.dat
    bundled, // /app0/config_database.dat
};

// 0.37: one copy as this process found it.
struct copy_info
{
    file_state state = file_state::absent;
    std::size_t titles = 0;
    long long timestamp = -1; // top-level 'timestamp' (seconds since 1970); -1 when it has none or was not read
    std::string error;
};

struct file_info
{
    file_state state = file_state::absent;
    std::size_t titles = 0; // entries with a 'config' string
    std::string error;      // why the file is not used (invalid, unreadable)
    // 0.37: the copy in use (state loaded). With no usable copy, the copy that 'state' and 'error' describe: /data,
    // or the bundled one when /data has no file at all.
    file_source source = file_source::data;
    long long timestamp = -1;
    copy_info data, bundled;
};

// One title's entry.
struct entry
{
    std::string config;    // YAML text, as PC RPCS3 applies it with cfg_root::from_string
    bool rejected = false; // the policy found a key or value this build does not know (see mark_rejected)
    bool skipped = false;  // 0.40: the boot policy does not apply it for this title (see set_skipped)

    bool usable() const { return !config.empty() && !rejected && !skipped; }
};

namespace detail
{
using game_map = std::map<std::string, std::string, std::less<>>;

// Strict JSON (RFC 8259) reader, the same acceptance as Qt's QJsonDocument::fromJson that PC RPCS3 uses: a UTF-8
// BOM is skipped, anything after the top-level value except whitespace is an error. No exceptions, bounded nesting.
class json_reader
{
    std::string_view m_text;
    std::size_t m_pos = 0;
    unsigned m_depth = 0;
    std::string m_error;

    static constexpr unsigned max_depth = 64;

    bool fail(std::string_view what)
    {
        if (m_error.empty()) m_error = std::string(what) + " at offset " + std::to_string(m_pos);
        return false;
    }

    void skip_ws()
    {
        while (m_pos < m_text.size() && (m_text[m_pos] == ' ' || m_text[m_pos] == '\t' || m_text[m_pos] == '\n' || m_text[m_pos] == '\r'))
            m_pos++;
    }

    bool eat(char c)
    {
        if (m_pos >= m_text.size() || m_text[m_pos] != c) return false;
        m_pos++;
        return true;
    }

    bool at(char c) const
    {
        return m_pos < m_text.size() && m_text[m_pos] == c;
    }

    static void append_utf8(std::string& out, std::uint32_t cp)
    {
        if (cp < 0x80) out += static_cast<char>(cp);
        else if (cp < 0x800)
        {
            out += static_cast<char>(0xc0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3f));
        }
        else if (cp < 0x10000)
        {
            out += static_cast<char>(0xe0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
            out += static_cast<char>(0x80 | (cp & 0x3f));
        }
        else
        {
            out += static_cast<char>(0xf0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
            out += static_cast<char>(0x80 | (cp & 0x3f));
        }
    }

    bool hex4(std::uint32_t& out)
    {
        if (m_text.size() - m_pos < 4) return fail("short \\u escape");
        out = 0;
        for (int i = 0; i < 4; i++)
        {
            const char c = m_text[m_pos++];
            out <<= 4;
            if (c >= '0' && c <= '9') out |= static_cast<std::uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') out |= static_cast<std::uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') out |= static_cast<std::uint32_t>(c - 'A' + 10);
            else return fail("bad \\u escape");
        }
        return true;
    }

    bool literal(std::string_view word)
    {
        if (m_text.substr(m_pos, word.size()) != word) return fail("invalid value");
        m_pos += word.size();
        return true;
    }

    template <typename Member>
    bool object(Member&& member)
    {
        if (!eat('{')) return fail("'{' expected");
        if (++m_depth > max_depth) return fail("nesting too deep");
        skip_ws();
        if (eat('}'))
        {
            m_depth--;
            return true;
        }
        while (true)
        {
            std::string key;
            skip_ws();
            if (!string(&key)) return false;
            skip_ws();
            if (!eat(':')) return fail("':' expected");
            skip_ws();
            if (!member(key)) return false;
            skip_ws();
            if (eat(',')) continue;
            if (eat('}')) break;
            return fail("',' or '}' expected");
        }
        m_depth--;
        return true;
    }

    bool array()
    {
        if (!eat('[')) return fail("'[' expected");
        if (++m_depth > max_depth) return fail("nesting too deep");
        skip_ws();
        if (eat(']'))
        {
            m_depth--;
            return true;
        }
        while (true)
        {
            skip_ws();
            if (!value()) return false;
            skip_ws();
            if (eat(',')) continue;
            if (eat(']')) break;
            return fail("',' or ']' expected");
        }
        m_depth--;
        return true;
    }

public:
    explicit json_reader(std::string_view text)
        : m_text(text)
    {
        if (m_text.starts_with("\xEF\xBB\xBF")) m_pos = 3;
    }

    const std::string& error() const { return m_error; }

    // A string; 'out' = null only checks and skips it.
    bool string(std::string* out)
    {
        if (!eat('"')) return fail("string expected");
        while (true)
        {
            if (m_pos >= m_text.size()) return fail("unterminated string");
            const char c = m_text[m_pos++];
            if (c == '"') return true;
            if (static_cast<unsigned char>(c) < 0x20) return fail("control character in string");
            if (c != '\\')
            {
                if (out) *out += c;
                continue;
            }
            if (m_pos >= m_text.size()) return fail("unterminated escape");
            const char e = m_text[m_pos++];
            char plain = 0;
            switch (e)
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
                std::uint32_t cp = 0;
                if (!hex4(cp)) return false;
                if (cp >= 0xd800 && cp <= 0xdbff && m_text.substr(m_pos, 2) == "\\u")
                {
                    const std::size_t back = m_pos;
                    m_pos += 2;
                    std::uint32_t low = 0;
                    if (!hex4(low)) return false;
                    if (low >= 0xdc00 && low <= 0xdfff) cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
                    else m_pos = back; // not a pair: the high half becomes U+FFFD, the next escape is read on its own
                }
                if (cp >= 0xd800 && cp <= 0xdfff) cp = 0xfffd;
                if (out) append_utf8(*out, cp);
                continue;
            }
            default: return fail("bad escape");
            }
            if (out) *out += plain;
        }
    }

    // A number. 'integer' receives its value when it has no fraction or exponent and fits; 'exact' tells which.
    bool number(long long* integer = nullptr, bool* exact = nullptr)
    {
        const std::size_t start = m_pos;
        bool negative = eat('-');
        bool fits = true;
        unsigned long long magnitude = 0;
        if (eat('0')) {}
        else if (m_pos < m_text.size() && m_text[m_pos] >= '1' && m_text[m_pos] <= '9')
        {
            while (m_pos < m_text.size() && m_text[m_pos] >= '0' && m_text[m_pos] <= '9')
            {
                const unsigned d = static_cast<unsigned>(m_text[m_pos++] - '0');
                if (magnitude > (~0ull >> 1) / 10) fits = false;
                else magnitude = magnitude * 10 + d;
            }
        }
        else return fail("invalid value");
        bool integral = true;
        if (eat('.'))
        {
            integral = false;
            if (m_pos >= m_text.size() || m_text[m_pos] < '0' || m_text[m_pos] > '9') return fail("digit expected");
            while (m_pos < m_text.size() && m_text[m_pos] >= '0' && m_text[m_pos] <= '9') m_pos++;
        }
        if (eat('e') || eat('E'))
        {
            integral = false;
            if (!eat('+')) eat('-');
            if (m_pos >= m_text.size() || m_text[m_pos] < '0' || m_text[m_pos] > '9') return fail("digit expected");
            while (m_pos < m_text.size() && m_text[m_pos] >= '0' && m_text[m_pos] <= '9') m_pos++;
        }
        if (m_pos == start) return fail("invalid value");
        const bool ok = integral && fits && magnitude <= static_cast<unsigned long long>(~0ull >> 1);
        if (exact) *exact = ok;
        if (integer && ok) *integer = negative ? -static_cast<long long>(magnitude) : static_cast<long long>(magnitude);
        return true;
    }

    // Any value, skipped.
    bool value()
    {
        if (m_pos >= m_text.size()) return fail("value expected");
        switch (m_text[m_pos])
        {
        case '{': return object([this](const std::string&) { return value(); });
        case '[': return array();
        case '"': return string(nullptr);
        case 't': return literal("true");
        case 'f': return literal("false");
        case 'n': return literal("null");
        default: return number();
        }
    }

    // Reads the whole document into 'games' (serial -> config text). False with error() set when the file is not
    // usable, mirroring config_database::read_json: not an object, return_code missing or below 0, or no 'games'
    // object. A game whose value is not an object with a 'config' string is skipped, as on PC.
    // 0.37: 'timestamp' receives the top-level 'timestamp' (-1 when it is missing, negative or not an integer).
    bool read_database(game_map& games, long long* timestamp = nullptr)
    {
        long long return_code = -255; // QJsonValue::toInt(-255) for a missing or non-integral value
        long long stamp = -1;
        bool have_games = false;
        skip_ws();
        if (!at('{')) return fail("not a JSON object");
        const bool ok = object([&](const std::string& key)
        {
            if (key == "return_code" || key == "timestamp")
            {
                long long& target = key == "return_code" ? return_code : stamp;
                const long long missing = key == "return_code" ? -255 : -1;
                if (!(at('-') || (m_pos < m_text.size() && m_text[m_pos] >= '0' && m_text[m_pos] <= '9')))
                {
                    target = missing;
                    return value();
                }
                long long number_value = 0;
                bool exact = false;
                if (!number(&number_value, &exact)) return false;
                target = exact ? number_value : missing;
                return true;
            }
            if (key != "games") return value();
            if (!at('{'))
            {
                have_games = false;
                return value();
            }
            have_games = true;
            games.clear();
            return object([&](const std::string& serial)
            {
                if (!at('{')) return value();
                std::string config;
                bool have_config = false;
                const bool game_ok = object([&](const std::string& field)
                {
                    if (field != "config" || !at('"')) return value();
                    config.clear();
                    have_config = true;
                    return string(&config);
                });
                if (game_ok && have_config) games[serial] = std::move(config);
                return game_ok;
            });
        });
        if (!ok) return false;
        skip_ws();
        if (m_pos != m_text.size()) return fail("garbage after the document");
        if (return_code < 0) return fail("return_code " + std::to_string(return_code));
        if (!have_games) return fail("no 'games' object");
        if (timestamp) *timestamp = stamp < 0 ? -1 : stamp;
        return true;
    }
};

// Parses a whole config_database.dat text. On failure 'games' is empty and 'error' says why.
// 0.37: 'timestamp' (optional) receives the document's timestamp, -1 when it has none or the parse failed.
inline bool parse(std::string_view text, game_map& games, std::string& error, long long* timestamp = nullptr)
{
    games.clear();
    if (timestamp) *timestamp = -1;
    json_reader reader(text);
    if (reader.read_database(games, timestamp))
    {
        error.clear();
        return true;
    }
    games.clear();
    if (timestamp) *timestamp = -1;
    error = reader.error();
    return false;
}

// 0.37: size and modification time of one copy when it was last read.
struct file_stamp
{
    bool present = false;
    std::uint64_t size = 0;
    std::int64_t mtime = 0;

    bool operator==(const file_stamp&) const = default;
};

struct cache
{
    std::mutex mutex;
    bool checked = false;    // the files were looked at at least once
    file_stamp data, bundled; // 0.37: both copies as they were then
    file_info info;
    game_map games;          // entries of the copy in use
    std::map<std::string, bool, std::less<>> rejected; // titles whose entry failed validation for this file
    // 0.40: titles whose entry the boot policy skips; not tied to a file, so a re-read keeps them
    std::map<std::string, bool, std::less<>> skipped;
};

inline cache& get_cache()
{
    static cache c;
    return c;
}

inline file_stamp stamp_of(const char* file)
{
    fs::stat_t st{};
    file_stamp out;
    out.present = fs::get_stat(file, st) && !st.is_directory;
    if (out.present)
    {
        out.size = st.size;
        out.mtime = st.mtime;
    }
    return out;
}

// Reads one copy; 'games' receives its entries when it is usable.
inline copy_info read_copy(const char* file_path, const file_stamp& stamp, game_map& games)
{
    copy_info out;
    games.clear();
    if (!stamp.present) return out;
    if (stamp.size > max_size)
    {
        out.state = file_state::too_large;
        out.error = "file larger than 16 MiB";
        return out;
    }
    const fs::file file(file_path);
    if (!file)
    {
        out.state = file_state::unreadable;
        out.error = "could not open the file";
        return out;
    }
    const std::uint64_t size = file.size();
    if (size > max_size)
    {
        out.state = file_state::too_large;
        out.error = "file larger than 16 MiB";
        return out;
    }
    std::string text(static_cast<std::size_t>(size), '\0');
    if (size && file.read(text.data(), size) != size)
    {
        out.state = file_state::unreadable;
        out.error = "short read";
        return out;
    }
    if (!parse(text, games, out.error, &out.timestamp))
    {
        out.state = file_state::invalid;
        return out;
    }
    out.state = file_state::loaded;
    out.titles = games.size();
    return out;
}

// Brings the cache up to date with both copies (caller holds the mutex). Two stats when nothing changed.
inline void refresh(cache& c)
{
    const file_stamp data = stamp_of(path);
    const file_stamp bundled = stamp_of(bundled_path);
    if (c.checked && data == c.data && bundled == c.bundled) return;

    c.checked = true;
    c.data = data;
    c.bundled = bundled;
    c.info = {};
    c.games.clear();
    c.rejected.clear();

    game_map data_games, bundled_games;
    c.info.data = read_copy(path, data, data_games);
    c.info.bundled = read_copy(bundled_path, bundled, bundled_games);
    const bool data_ok = c.info.data.state == file_state::loaded;
    const bool bundled_ok = c.info.bundled.state == file_state::loaded;
    // The user's import wins, unless both copies are dated and the package's is newer.
    const bool use_bundled = bundled_ok &&
        (!data_ok || (c.info.data.timestamp >= 0 && c.info.bundled.timestamp > c.info.data.timestamp));
    if (use_bundled || data_ok)
    {
        const copy_info& used = use_bundled ? c.info.bundled : c.info.data;
        c.info.state = file_state::loaded;
        c.info.titles = used.titles;
        c.info.timestamp = used.timestamp;
        c.info.source = use_bundled ? file_source::bundled : file_source::data;
        c.games = std::move(use_bundled ? bundled_games : data_games);
        return;
    }
    const bool describe_bundled = c.info.data.state == file_state::absent && c.info.bundled.state != file_state::absent;
    const copy_info& shown = describe_bundled ? c.info.bundled : c.info.data;
    c.info.state = shown.state;
    c.info.error = shown.error;
    c.info.source = describe_bundled ? file_source::bundled : file_source::data;
}
} // namespace detail

// 0.37: "timestamp T" or "no timestamp".
inline std::string timestamp_text(long long timestamp)
{
    return timestamp >= 0 ? "timestamp " + std::to_string(timestamp) : std::string("no timestamp");
}

// 0.37: the startup.log line of the copies (which one runs, and why the other does not).
inline std::string describe(const file_info& info)
{
    const bool bundled = info.source == file_source::bundled;
    std::string line = "config database: ";
    line += bundled ? std::string("bundled ") + bundled_path : std::string(path);
    if (info.state == file_state::loaded)
    {
        line += " (" + timestamp_text(info.timestamp) + ", " + std::to_string(info.titles) + " titles); ";
        const copy_info& other = bundled ? info.data : info.bundled;
        switch (other.state)
        {
        case file_state::absent: line += bundled ? "/data copy absent" : "no bundled copy"; break;
        case file_state::loaded:
            if (bundled) line += "/data copy older (" + timestamp_text(other.timestamp) + ")";
            else if (info.timestamp >= 0 && other.timestamp >= 0) line += "bundled copy not newer (" + timestamp_text(other.timestamp) + ")";
            else line += "bundled copy not used (" + timestamp_text(other.timestamp) + "; the /data copy wins unless both are dated)";
            break;
        default: line += std::string(bundled ? "/data copy" : "bundled copy") + " not used: " + other.error; break;
        }
        return line;
    }
    if (info.state == file_state::absent)
        return line + " not present (PC per-game settings not used; copy GuiConfigs/config_database.dat from a PC RPCS3 to use them)";
    line += " not used: " + info.error;
    if (bundled) line += "; /data copy absent";
    else if (info.bundled.state != file_state::absent) line += "; bundled copy not used: " + info.bundled.error;
    return line;
}

// The state of the config database (both copies read when one changed since the last call).
inline file_info status()
{
    detail::cache& c = detail::get_cache();
    const std::lock_guard lock(c.mutex);
    detail::refresh(c);
    return c.info;
}

// The entry of 'title_id'; empty config when the file, or the title in it, is missing.
inline entry lookup(std::string_view title_id)
{
    if (title_id.empty()) return {};
    detail::cache& c = detail::get_cache();
    const std::lock_guard lock(c.mutex);
    detail::refresh(c);
    entry out;
    if (const auto it = c.games.find(title_id); it != c.games.end())
    {
        out.config = it->second;
        out.rejected = c.rejected.find(title_id) != c.rejected.end();
        out.skipped = c.skipped.find(title_id) != c.skipped.end();
    }
    return out;
}

// True when the database has an entry for the title that this build accepted (or has not checked yet).
inline bool has_entry(std::string_view title_id)
{
    return lookup(title_id).usable();
}

// Records that the title's entry is not valid for this build (unknown key or value): PC RPCS3 drops such an entry
// when it reads the database, so the policy stops using it until the file changes.
inline void mark_rejected(std::string_view title_id)
{
    if (title_id.empty()) return;
    detail::cache& c = detail::get_cache();
    const std::lock_guard lock(c.mutex);
    if (c.games.find(title_id) != c.games.end()) c.rejected[std::string(title_id)] = true;
}

// 0.40: the boot policy's decision for one title (config-database-off.flag, or the per-game safe mode from level 2):
// while skipped, lookup() marks the entry skipped and has_entry() is false, so the title runs as on a console without
// the database, its fallback rows included. Every policy run of the title sets it again.
inline void set_skipped(std::string_view title_id, bool skip)
{
    if (title_id.empty()) return;
    detail::cache& c = detail::get_cache();
    const std::lock_guard lock(c.mutex);
    if (skip) c.skipped[std::string(title_id)] = true;
    else if (const auto it = c.skipped.find(title_id); it != c.skipped.end()) c.skipped.erase(it);
}

// Drops the cached copy, so the next call reads the file again even if its size and time did not change (for an
// import that replaced it within the same second).
inline void forget()
{
    detail::cache& c = detail::get_cache();
    const std::lock_guard lock(c.mutex);
    c.checked = false;
}
} // namespace utils::ps4::config_db
