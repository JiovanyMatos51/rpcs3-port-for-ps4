#pragma once

// Entries of /data/rpcs3 the app cannot write: found at start and opened through the console's own FTP server.
//
// The GoldHEN FTP server creates what it uploads as uid 0 with mode 0755 (folders) / 0644 (files), and that uid gets no
// write access to the app's 0755 folders either. A folder made over FTP before the app first ran (or a whole rpcs3
// folder copied in) is therefore closed to the app, which cannot chmod it (EPERM: not the owner). The FTP server owns
// those entries, so it can open them: 'SITE CHMOD 777 <folder>' / 'SITE CHMOD 666 <file>' over the loopback (GoldHEN
// FTP v2.2 lists SITE CHMOD in FEAT). Only SITE CHMOD is sent, only for canonical paths inside /data/rpcs3 that a
// listing showed as real folders or files (no '.' or '..' component, no symbolic link, no control character): nothing
// is created, removed or renamed over FTP.
//
//   scan     write probe (create + remove a file) of the folders the emulator writes, missing ones created 0777, then
//            a breadth-first walk of /data/rpcs3 (entries and time capped) for entries of another owner the app cannot
//            write; the app's own entries without write access get its own chmod. Folders whose files the app only
//            reads (games, pkgs, raps, import, exdata) are walked one level deep and their files are left alone. Files
//            are stat'ed only in /data/rpcs3, the key folders and folders of another owner; the app's own folders
//            elsewhere (installed games) are walked for subfolders only (readdir types, no stat per file).
//   repair   one FTP control connection, SITE CHMOD per entry, parents first (commands, refusals and time capped); a
//            5xx 'not implemented' ends it. A folder the app could not list is walked again after its repair.
//   verify   the probes and the sent entries again: only what is writable afterwards counts as repaired.
// The first SITE CHMOD the server accepts is checked with a stat: 0777 goes on, 01411 (the digits read as decimal)
// switches to "511"/"438", any other mode is put back and ends the repair; the first path with a space is checked the
// same way. Cache folders still closed afterwards (e.g. the FTP server off) are renamed aside and made again; nothing
// else is ever moved. reopen_folder(): one folder at once, when a write there was refused.
// Generic part host-tested (ps4-port/tests/fs_repair_test.cpp) with stand-ins for the file system and the server.

#include "ps4_disc_stream.hpp"
#include "ps4_i18n.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace utils::ps4::fs_repair
{
namespace ds = utils::ps4::disc_stream;

inline constexpr std::string_view root = "/data/rpcs3";
inline constexpr char probe_name[] = ".rpcs3-write-probe";

// Folders the emulator writes, parents first ("" is /data/rpcs3): probed for real, created 0777 when missing, opened
// to 0777 when the app owns them (so the user's later FTP uploads into them work). The port has one user, 00000001:
// its folder holds the user name every game reads and exdata the imported licences.
inline constexpr std::array<const char*, 23> key_folders{"", "dev_hdd0", "dev_hdd0/game", "dev_hdd0/home", "dev_hdd0/home/00000001",
    "dev_hdd0/home/00000001/exdata", "dev_hdd1", "cache", "cache/xmb", "games", "pkgs", "raps", "patches", "cheats", "import", "data",
    "data/redump", "data/disc-lock", "log", "savestates", "custom_configs", "adapt", "exdata"};
// A cache folder that stays closed is renamed "<name><blocked_suffix><unix time>" in its parent and made again (never
// any other folder); such folders are not walked again
inline constexpr std::string_view blocked_suffix = ".blocked-";
// Probed when present, never created or opened by the app (the firmware installer owns them)
inline constexpr std::array<const char*, 3> optional_folders{"dev_flash", "dev_flash2", "dev_flash3"};
// The app only reads the files in these (games, packages, licenses to import)
inline constexpr std::array<const char*, 5> source_folders{"games", "pkgs", "raps", "import", "exdata"};

enum class kind : std::uint8_t
{
    missing,
    directory,
    file,
    symlink,
    other,
};

struct entry_info
{
    kind type = kind::missing;
    std::uint32_t uid = 0;
    std::uint32_t mode = 0; // permission bits (07777)
};

struct dir_entry
{
    std::string name;
    entry_info info;      // symlink when the listing says so (never followed)
    bool details = true;  // uid and mode read (a file listed without 'stat_files' has only its type)
};

// The file system calls, so that a test can stand in for the console
struct fs_ops
{
    std::function<entry_info(const std::string& path)> stat;
    // The entries of a folder without "." and "..", at most 'max'; false with 'error' when it cannot be listed.
    // stat_files false: regular files may come without uid and mode (no stat per file).
    std::function<bool(const std::string& path, std::vector<dir_entry>& out, int& error, std::size_t max, bool stat_files)> list;
    // 0 when a file could be created and removed in 'folder' ('owner': the uid it got), else the errno
    std::function<int(const std::string& folder, std::uint32_t& owner)> probe;
    std::function<int(const std::string& path, std::uint32_t mode)> make_dir; // the app's mkdir + chmod: 0 or errno
    std::function<int(const std::string& path, std::uint32_t mode)> chmod;    // the app's own chmod: 0 or errno
    // Optional: the app's rename inside one folder (0 or errno), and the wall clock (unix seconds) for its new name.
    // Without them no cache folder is moved aside.
    std::function<int(const std::string& from, const std::string& to)> rename;
    std::function<std::int64_t()> unix_time;
    std::function<std::int64_t()> now_ms;
    // Optional: a mark that an earlier start left the walk unfinished with entries to repair (the walk then goes on
    // past its limits in the next start, within the repair's time budget)
    std::function<bool()> pending;
    std::function<void(bool)> set_pending;
};

struct limits
{
    std::uint32_t max_entries = 20000;  // entries the walk looks at per round
    std::int64_t scan_ms = 3000;        // per round
    std::uint32_t max_commands = 20000; // SITE CHMOD per start
    std::int64_t repair_ms = 60000;
    int connect_ms = 2000;
    int reply_ms = 4000;
    std::uint32_t max_refusals = 64; // refusals in a row that end the repair
};

// A path the repair may name: "/data/rpcs3" or "/data/rpcs3/<a>/<b>..." without an empty, "." or ".." component, a
// backslash or a byte below 0x20 or 0x7f (a CR/LF would end the FTP command); at most 1024 bytes
inline bool allowed_path(std::string_view path)
{
    if (path.size() > 1024) return false;
    if (path == root) return true;
    if (path.size() <= root.size() + 1 || path.substr(0, root.size()) != root || path[root.size()] != '/') return false;
    std::string_view rest = path.substr(root.size() + 1);
    for (;;)
    {
        const std::size_t slash = rest.find('/');
        const std::string_view part = rest.substr(0, slash);
        if (part.empty() || part == "." || part == "..") return false;
        for (const char c : part)
        {
            const auto byte = static_cast<unsigned char>(c);
            if (byte < 0x20 || byte == 0x7f || c == '\\') return false;
        }
        if (slash == std::string_view::npos) return true;
        rest.remove_prefix(slash + 1);
    }
}

inline std::string full_path(std::string_view rel)
{
    return rel.empty() ? std::string(root) : std::string(root) + "/" + std::string(rel);
}

// "games/x" for "/data/rpcs3/games/x"; "" for /data/rpcs3 itself
inline std::string_view relative(std::string_view path)
{
    return path.size() > root.size() + 1 ? path.substr(root.size() + 1) : std::string_view{};
}

inline std::size_t depth(std::string_view rel)
{
    return rel.empty() ? 0 : 1 + static_cast<std::size_t>(std::count(rel.begin(), rel.end(), '/'));
}

inline bool in_source_folder(std::string_view rel)
{
    const std::string_view top = rel.substr(0, rel.find('/'));
    return std::any_of(source_folders.begin(), source_folders.end(), [&](const char* name) { return top == name; });
}

inline bool is_key_folder(std::string_view rel)
{
    return std::any_of(key_folders.begin(), key_folders.end(), [&](const char* name) { return rel == name; });
}

// "dev_hdd0/home/<user>/savedata" and the folders below it: a save file uploaded over FTP into an app's save folder is
// found only by a stat of each file there
inline bool in_savedata(std::string_view rel)
{
    constexpr std::string_view head = "dev_hdd0/home/";
    if (rel.substr(0, head.size()) != head) return false;
    const std::string_view rest = rel.substr(head.size());
    const std::size_t slash = rest.find('/');
    if (slash == std::string_view::npos || slash == 0) return false;
    const std::string_view below = rest.substr(slash + 1);
    return below == "savedata" || below.substr(0, 9) == "savedata/";
}

// The cache tree (relative names): the only folders a move-aside may touch
inline bool in_cache_tree(std::string_view rel)
{
    return rel == "cache" || rel.substr(0, 6) == "cache/";
}

// A name a move-aside made: "<name>.blocked-<digits>" inside the cache tree, or "cache.blocked-<digits>" in /data/rpcs3
inline bool moved_aside_name(std::string_view parent_rel, std::string_view name)
{
    const std::size_t at = name.find(blocked_suffix);
    if (at == std::string_view::npos || at == 0) return false;
    const std::string_view digits = name.substr(at + blocked_suffix.size());
    if (digits.empty() || !std::all_of(digits.begin(), digits.end(), [](char c) { return c >= '0' && c <= '9'; })) return false;
    return (parent_rel.empty() && name.substr(0, at) == "cache") || in_cache_tree(parent_rel);
}

inline bool permission_error(int error)
{
    return error == EACCES || error == EPERM;
}

enum class verdict
{
    fine,
    foreign, // another owner and no write access for the app: only the owner (the FTP server) can open it
    own,     // the app's own entry without owner write access: its own chmod opens it
};

// Folders need rwx and files rw for the app. Without a known app uid (no folder writable) every entry lacking them
// for 'others' counts as foreign.
inline verdict judge(const entry_info& e, std::optional<std::uint32_t> app_uid)
{
    if (e.type != kind::directory && e.type != kind::file) return verdict::fine;
    const bool folder = e.type == kind::directory;
    if (app_uid && e.uid == *app_uid)
    {
        const std::uint32_t need = folder ? 0700u : 0600u;
        return (e.mode & need) == need ? verdict::fine : verdict::own;
    }
    const std::uint32_t need = folder ? 07u : 06u;
    return (e.mode & need) == need ? verdict::fine : verdict::foreign;
}

inline std::string mode_text(std::uint32_t mode)
{
    char text[16];
    std::snprintf(text, sizeof(text), "0%o", static_cast<unsigned>(mode & 07777));
    return text;
}

// The folder a write failure is about: the parent of 'path' ("/data/rpcs3/dev_hdd0/game/NPJB00435/" ->
// "/data/rpcs3/dev_hdd0/game")
inline std::string parent_folder(std::string_view path)
{
    while (path.size() > 1 && path.back() == '/') path.remove_suffix(1);
    const std::size_t slash = path.rfind('/');
    if (slash == std::string_view::npos || slash == 0) return std::string(path);
    return std::string(path.substr(0, slash));
}

// Text of a write the console refused (EACCES/EPERM) in 'folder', in the interface language (pt-BR fits the 116-byte
// XMB line); " · " separates the dialog notes. The next start repairs the folder when the GoldHEN FTP server is on.
inline std::string no_write_text(std::string_view folder)
{
    return i18n::trf(i18n::str::FSR_SEM_PERMISSAO_GRAVACAO, {folder});
}

// An English failure detail that names a refused permission ("... errno 13 (EACCES)")
inline bool permission_detail(std::string_view detail)
{
    return detail.find("(EACCES)") != std::string_view::npos || detail.find("(EPERM)") != std::string_view::npos;
}

// The folder of the first /data/rpcs3 path in such a detail ("open /data/rpcs3/games/X.iso.part: errno 13 (EACCES)"
// -> "/data/rpcs3/games"); 'fallback' when it names none
inline std::string folder_in_detail(std::string_view detail, std::string_view fallback)
{
    std::size_t at = detail.find(root);
    while (at != std::string_view::npos && at + root.size() < detail.size() && detail[at + root.size()] != '/' && detail[at + root.size()] != ':' && detail[at + root.size()] != ' ')
        at = detail.find(root, at + 1);
    if (at == std::string_view::npos) return std::string(fallback);
    std::string_view path = detail.substr(at);
    if (const std::size_t end = path.find(": "); end != std::string_view::npos) path = path.substr(0, end);
    while (!path.empty() && (path.back() == ' ' || path.back() == '\'' || path.back() == ')' || path.back() == ':')) path.remove_suffix(1);
    std::string folder = parent_folder(path);
    return folder.size() >= root.size() && std::string_view(folder).substr(0, root.size()) == root ? folder : std::string(fallback);
}

// One FTP control connection: greeting, anonymous login, single commands. No data connection is ever opened.
class ftp_control
{
public:
    ftp_control(const ds::net_ops& net, std::function<std::int64_t()> now_ms, int reply_ms)
        : m_net(net), m_now(std::move(now_ms)), m_reply_ms(reply_ms)
    {
    }

    ~ftp_control() { close(); }

    ftp_control(const ftp_control&) = delete;
    ftp_control& operator=(const ftp_control&) = delete;

    bool connected() const noexcept { return m_socket >= 0; }
    const std::string& banner() const noexcept { return m_banner; }

    bool open(const ds::endpoint& to, int connect_ms, std::string& why)
    {
        drop();
        int error = 0;
        m_socket = m_net.connect(to, connect_ms, 0, error);
        if (m_socket < 0)
        {
            m_socket = -1;
            why = "connect failed: " + ds::detail::errno_text(error);
            return false;
        }
        ds::reply r = read_reply();
        if (r.code != 220)
        {
            why = r.code ? "unexpected greeting '" + r.text + "'" : std::string("no greeting");
            drop();
            return false;
        }
        m_banner = r.text.size() > 4 ? r.text.substr(4) : r.text;
        r = command("USER anonymous");
        if (r.code == 331) r = command("PASS anonymous@");
        if (r.code != 230 && r.code != 202)
        {
            why = r.code ? "login refused: '" + r.text + "'" : std::string("no reply to the login");
            drop();
            return false;
        }
        return true;
    }

    // The reply (code 0: none came, the connection is closed)
    ds::reply command(const std::string& line)
    {
        if (m_socket < 0) return {};
        const std::string data = line + "\r\n";
        int error = 0;
        if (m_net.send(m_socket, data.data(), data.size(), m_reply_ms, error) != static_cast<std::int64_t>(data.size()))
        {
            drop();
            return {};
        }
        ds::reply r = read_reply();
        if (r.code <= 0) drop();
        return r;
    }

    void close()
    {
        if (m_socket < 0) return;
        int error = 0;
        m_net.send(m_socket, "QUIT\r\n", 6, 500, error);
        drop();
    }

private:
    const ds::net_ops& m_net;
    std::function<std::int64_t()> m_now;
    int m_reply_ms;
    int m_socket = -1;
    std::string m_buffer;
    std::string m_banner;

    void drop()
    {
        if (m_socket >= 0) m_net.close(m_socket);
        m_socket = -1;
        m_buffer.clear();
    }

    ds::reply read_reply()
    {
        const std::int64_t end = m_now() + m_reply_ms;
        char chunk[512];
        for (;;)
        {
            if (auto r = ds::take_reply(m_buffer)) return *r;
            if (m_buffer.size() > 16384) return {-1, "reply too long"};
            const std::int64_t left = end - m_now();
            if (left <= 0) return {};
            int error = 0;
            const std::int64_t got = m_net.recv(m_socket, chunk, sizeof(chunk), static_cast<int>(left), error);
            if (got <= 0) return {};
            m_buffer.append(chunk, static_cast<std::size_t>(got));
        }
    }
};

enum class ftp_state
{
    not_needed,   // nothing for the FTP server
    unreachable,  // no connection (server off)
    login_failed,
    unsupported,  // SITE CHMOD answered 500/502/504
    refused,      // max_refusals refusals in a row
    done,         // every entry sent
    partial,      // command/time limit or the connection was lost
    wrong_mode,   // the first SITE CHMOD it accepted set another mode (in octal and in decimal): nothing more is sent
};

inline const char* to_string(ftp_state state)
{
    switch (state)
    {
    case ftp_state::not_needed: return "not needed";
    case ftp_state::unreachable: return "unreachable";
    case ftp_state::login_failed: return "login failed";
    case ftp_state::unsupported: return "SITE CHMOD not supported";
    case ftp_state::refused: return "refused";
    case ftp_state::done: return "done";
    case ftp_state::partial: return "partial";
    case ftp_state::wrong_mode: return "SITE CHMOD sets another mode";
    }
    return "?";
}

// How the server reads the mode of SITE CHMOD, learnt from the first entry it accepted (stat afterwards)
enum class chmod_form : std::uint8_t
{
    unknown,
    octal,   // "777" / "666"
    decimal, // "777" became 01411: "511" / "438" are sent
};

// The mode argument for a folder or a file in that form
inline const char* chmod_argument(bool folder, chmod_form form)
{
    return form == chmod_form::decimal ? (folder ? "511" : "438") : (folder ? "777" : "666");
}

struct blocked_entry
{
    std::string path;
    entry_info info;
};

struct report
{
    std::optional<std::uint32_t> app_uid;
    bool root_link = false; // /data/rpcs3 is a symbolic link: nothing is sent
    std::uint64_t entries = 0;
    bool capped_entries = false, capped_time = false;
    bool resumed = false; // an earlier start left the walk unfinished
    std::int64_t scan_ms = 0;
    std::vector<std::string> key_blocked; // relative names whose probe failed before the repair ("" = /data/rpcs3)
    std::vector<std::string> created;     // key folders created now
    std::uint32_t foreign_dirs = 0, foreign_files = 0;
    std::set<std::uint32_t> owners; // owners of those
    std::uint32_t own_fixed = 0, own_failed = 0;
    std::uint32_t rounds = 0;
    ftp_state ftp = ftp_state::not_needed;
    std::string server, banner, ftp_why;
    std::uint32_t sent = 0, accepted = 0, refused = 0;
    std::int64_t repair_ms = 0;
    std::uint32_t repaired = 0;                // entries writable after their SITE CHMOD
    std::vector<blocked_entry> still_blocked;  // after the repair, at most 64 (key folders first)
    std::uint32_t still_blocked_count = 0;
    chmod_form form = chmod_form::unknown;
    std::uint32_t wrong_mode = 0;              // the mode the first accepted SITE CHMOD left (ftp_state::wrong_mode)
    std::uint32_t wanted_mode = 0;             // and the one it should have set
    std::uint32_t space_skipped = 0;           // paths with a space left out: the server did not apply such a path
    std::vector<std::string> moved_aside;      // "cache -> cache.blocked-<t>" (relative names)
    std::uint32_t move_failed = 0;

    bool blocked() const noexcept { return still_blocked_count != 0; }
    bool ftp_answered() const noexcept { return ftp != ftp_state::not_needed && ftp != ftp_state::unreachable; }

    // Relative names ("games", "cache/xmb"; "/data/rpcs3" for the root) of the folders still blocked, at most 'max'
    std::vector<std::string> blocked_folders(std::size_t max) const
    {
        std::vector<std::string> out;
        for (const blocked_entry& e : still_blocked)
        {
            const std::string folder = e.info.type == kind::file ? parent_folder(e.path) : e.path;
            const std::string name = folder == root ? std::string(root) : std::string(relative(folder));
            if (std::find(out.begin(), out.end(), name) != out.end()) continue;
            if (out.size() == max) break;
            out.push_back(name);
        }
        return out;
    }

    std::vector<std::string> log_lines() const
    {
        const auto join = [](const std::vector<std::string>& names)
        {
            std::string text;
            for (const std::string& name : names) text += (text.empty() ? "" : " ") + (name.empty() ? std::string(root) : name);
            return text;
        };
        std::vector<std::string> lines;
        std::string line = "fs perms: " + std::to_string(entries) + " entries checked in " + std::to_string(scan_ms) + " ms";
        if (resumed) line += " (going on with the walk an earlier start left unfinished)";
        if (capped_entries) line += " (stopped at the entry limit: the rest next start)";
        else if (capped_time) line += " (stopped at the time limit: the rest next start)";
        line += app_uid ? "; app uid " + std::to_string(*app_uid) : std::string("; app uid unknown (no folder writable)");
        if (root_link) line += "; /data/rpcs3 is a symbolic link: nothing changed";
        if (!created.empty()) line += "; created 0777: " + join(created);
        if (own_fixed || own_failed) line += "; app chmod opened " + std::to_string(own_fixed) + " of its own entries" + (own_failed ? " (" + std::to_string(own_failed) + " refused)" : std::string());
        if (key_blocked.empty() && !foreign_dirs && !foreign_files)
            line += "; all writable";
        else
        {
            std::string owner_text;
            for (const std::uint32_t uid : owners) owner_text += (owner_text.empty() ? "uid " : ", uid ") + std::to_string(uid);
            line += "; " + std::to_string(foreign_dirs) + " dirs and " + std::to_string(foreign_files) + " files not writable" + (owner_text.empty() ? std::string() : " (owner " + owner_text + ")");
            if (!key_blocked.empty()) line += "; key folders blocked: " + join(key_blocked);
        }
        lines.push_back(std::move(line));
        if (ftp != ftp_state::not_needed)
        {
            std::string f = "fs perms: FTP " + server + (banner.empty() ? std::string() : " ('" + banner + "')");
            if (ftp == ftp_state::unreachable || ftp == ftp_state::login_failed)
                f += ": " + ftp_why + (ftp == ftp_state::unreachable ? " (GoldHEN FTP server off?)" : "");
            else
            {
                f += ": SITE CHMOD sent " + std::to_string(sent) + ", accepted " + std::to_string(accepted) + ", refused " + std::to_string(refused) + " in " + std::to_string(repair_ms) + " ms";
                if (ftp == ftp_state::unsupported) f += "; SITE CHMOD not supported ('" + ftp_why + "')";
                else if (ftp == ftp_state::refused) f += "; stopped after refusals in a row ('" + ftp_why + "')";
                else if (ftp == ftp_state::partial) f += "; stopped: " + ftp_why + " (the rest next start)";
                if (form == chmod_form::decimal) f += "; the server reads the mode as decimal: 511/438 sent";
            }
            f += "; repaired " + std::to_string(repaired) + " via FTP";
            lines.push_back(std::move(f));
            if (ftp == ftp_state::wrong_mode)
                lines.push_back("fs perms: SITE CHMOD changed the mode to " + mode_text(wrong_mode) + ", not " + mode_text(wanted_mode) + ": stopped");
            if (space_skipped)
                lines.push_back("fs perms: " + std::to_string(space_skipped) + " path(s) with a space left out (the server did not apply SITE CHMOD to such a path)");
        }
        for (const std::string& move : moved_aside)
            lines.push_back("fs perms: cache moved aside: " + move + " (made by the FTP server, not writable): a new cache is used");
        if (move_failed) lines.push_back("fs perms: " + std::to_string(move_failed) + " blocked cache folder(s) could not be moved aside");
        if (still_blocked_count)
        {
            std::string b = "fs perms: still blocked (" + std::to_string(still_blocked_count) + "):";
            std::size_t shown = 0;
            for (const blocked_entry& e : still_blocked)
            {
                if (shown++ == 12) break;
                b += " " + e.path + (e.info.type == kind::missing ? std::string(" (missing)") : " (mode " + mode_text(e.info.mode) + " uid " + std::to_string(e.info.uid) + ")");
            }
            if (still_blocked_count > 12) b += " (+" + std::to_string(still_blocked_count - 12) + ")";
            lines.push_back(std::move(b));
        }
        else if (ftp != ftp_state::not_needed || !key_blocked.empty())
        {
            lines.emplace_back(std::string("fs perms: after the repair every checked entry is writable") + (capped_entries || capped_time ? " (the rest of the walk next start)" : ""));
        }
        return lines;
    }
};

namespace detail
{
struct candidate
{
    std::string path;
    bool folder = false;
    entry_info info;
};

class repairer
{
public:
    repairer(const fs_ops& fs, const ds::net_ops& net, const ds::endpoint& server, const limits& lim, report& out)
        : m_fs(fs), m_net(net), m_server(server), m_lim(lim), m_out(out)
    {
    }

    void run(const std::function<void(std::size_t)>& on_repair)
    {
        // stat() follows links, so whether /data/rpcs3 itself is one is read from the listing of /data
        {
            std::vector<dir_entry> top;
            int error = 0;
            if (m_fs.list("/data", top, error, 65536, false))
                for (const dir_entry& e : top)
                    if (e.name == "rpcs3" && e.info.type == kind::symlink) m_out.root_link = true;
        }
        m_out.key_blocked = probe_keys(&m_out.created);
        const bool pending = m_fs.pending && m_fs.pending();
        m_out.resumed = pending;
        work queue;
        for (std::uint32_t round = 0; round < max_rounds && !m_out.root_link; ++round)
        {
            m_out.rounds = round + 1;
            const std::vector<candidate> found = scan(std::move(queue), round == 0);
            queue.clear();
            const std::size_t count = static_cast<std::size_t>(std::count_if(found.begin(), found.end(), [&](const candidate& c) { return sendable(c); }));
            if (count)
            {
                if (on_repair) on_repair(count);
                repair(found);
            }
            // Only while this start repairs through a working server (or finishes what an earlier one left): the walk
            // goes on where a limit stopped it, within the repair's time budget; a later start would stop at the same
            // place
            if (m_out.ftp != ftp_state::done && !(pending && m_out.ftp == ftp_state::not_needed)) break;
            if (!m_leftover.empty() && m_out.scan_ms + m_out.repair_ms < m_lim.repair_ms)
            {
                queue = std::move(m_leftover);
                m_leftover.clear();
            }
            // A folder the app could not list: its entries are walked once its repair went through
            for (const std::string& path : m_unlistable)
                if (m_accepted.count(path) && m_walked_after.insert(path).second) queue.push_back({path, true, 0});
            if (queue.empty()) break;
        }
        if (!queue.empty()) m_leftover = std::move(queue); // the round limit
        m_out.capped_entries = !m_leftover.empty() && m_last_cap == cap::entries;
        m_out.capped_time = !m_leftover.empty() && m_last_cap == cap::time;
        move_aside();
        verify();
        // An unfinished walk of a tree known to hold entries to repair is taken up by the next start
        const bool unfinished = !m_leftover.empty() && (pending || !m_all.empty());
        if (m_fs.set_pending && unfinished != pending) m_fs.set_pending(unfinished);
    }

private:
    // The walk from /data/rpcs3, then its continuations and the folders the app could only list after their repair
    static constexpr std::uint32_t max_rounds = 16;

    // A folder still to list: whether its files are stat'ed, how many of its entries an earlier round already took
    struct work_item
    {
        std::string path;
        bool files = true;
        std::size_t skip = 0;
    };
    using work = std::deque<work_item>;
    enum class cap
    {
        none,
        entries,
        time,
    };

    const fs_ops& m_fs;
    const ds::net_ops& m_net;
    const ds::endpoint& m_server;
    const limits& m_lim;
    report& m_out;
    std::set<std::string> m_real;       // listed as real folders (never through a link)
    std::set<std::string> m_seen;       // candidates already found
    std::vector<candidate> m_all;       // every candidate, in order
    std::set<std::string> m_attempted;  // a SITE CHMOD went out for these
    std::set<std::string> m_accepted;   // and was answered 2xx
    std::set<std::string> m_unlistable;
    std::set<std::string> m_walked_after;
    std::set<std::string> m_queued; // folders put in a walk queue once
    work m_leftover;                // where the last capped walk stopped
    cap m_last_cap = cap::none;
    bool m_space_known = false;     // the first accepted path with a space was stat'ed
    bool m_space_refused = false;   // and its mode did not change: the rest of such paths are left out
    std::set<std::string> m_moved;  // cache folders moved aside (their old paths)

    // After the first SITE CHMOD the server accepted (and the first one of a path with a space): did it set the mode it
    // should? The first one teaches the form: 0777/0666 -> octal; 01411/01232 (the digits read as decimal) -> the
    // decimal argument is sent again and used from then on. Any other mode: the entry is put back as it was and false
    // ends the repair. A path with a space whose mode did not change: such paths are left out.
    template <typename Send>
    bool learn(const candidate& c, bool spaced, Send&& send)
    {
        const bool form_known = m_out.form != chmod_form::unknown;
        if (form_known && (!spaced || m_space_known)) return true;
        const std::uint32_t want = c.folder ? 0777u : 0666u;
        const entry_info now = m_fs.stat(c.path);
        if (now.type == kind::missing) return true; // nothing to tell: the verify pass decides
        std::uint32_t mode = now.mode & 07777u;
        if (form_known)
        {
            m_space_known = true;
            m_space_refused = mode != want;
            return true;
        }
        if (mode == want)
        {
            m_out.form = chmod_form::octal;
            m_space_known = m_space_known || spaced;
            return true;
        }
        if (mode == (c.folder ? 01411u : 01232u))
        {
            const ds::reply r = send(c, chmod_argument(c.folder, chmod_form::decimal));
            mode = m_fs.stat(c.path).mode & 07777u;
            if (r.code >= 200 && r.code < 300 && mode == want)
            {
                m_out.form = chmod_form::decimal;
                m_space_known = m_space_known || spaced;
                return true;
            }
        }
        if (mode != (c.info.mode & 07777u))
        {
            char back[8];
            std::snprintf(back, sizeof(back), "%o", static_cast<unsigned>(c.info.mode & 07777u));
            send(c, back);
        }
        m_out.wrong_mode = mode;
        m_out.wanted_mode = want;
        return false;
    }

    // The key folders that refuse a write (relative names); missing ones are created first
    std::vector<std::string> probe_keys(std::vector<std::string>* created)
    {
        std::vector<std::string> blocked;
        std::vector<std::pair<std::string, entry_info>> writable;
        const auto check = [&](std::string_view rel, bool key)
        {
            const std::string path = full_path(rel);
            const entry_info info = m_fs.stat(path);
            if (info.type == kind::missing)
            {
                if (!key) return;
                if (m_fs.make_dir(path, 0777) == 0)
                {
                    if (created) created->emplace_back(rel);
                }
                else
                {
                    blocked.emplace_back(rel);
                }
                return;
            }
            if (info.type != kind::directory)
            {
                blocked.emplace_back(rel);
                return;
            }
            std::uint32_t owner = 0;
            if (m_fs.probe(path, owner) != 0)
            {
                blocked.emplace_back(rel);
                return;
            }
            if (!m_out.app_uid) m_out.app_uid = owner;
            if (key) writable.emplace_back(path, info);
        };
        for (const char* rel : key_folders) check(rel, true);
        for (const char* rel : optional_folders) check(rel, false);
        // The app's own key folders open to 0777: the FTP server's uid can then upload into them
        for (const auto& [path, info] : writable)
        {
            if (!m_out.app_uid || info.uid != *m_out.app_uid || (info.mode & 0777) == 0777) continue;
            if (m_fs.chmod(path, 0777) == 0) ++m_out.own_fixed;
            else ++m_out.own_failed;
        }
        return blocked;
    }

    bool add(const std::string& path, const entry_info& info, std::vector<candidate>& found, bool count)
    {
        const verdict v = judge(info, m_out.app_uid);
        if (v == verdict::fine) return false;
        const bool folder = info.type == kind::directory;
        if (v == verdict::own)
        {
            if (m_fs.chmod(path, folder ? 0777 : 0666) == 0) ++m_out.own_fixed;
            else ++m_out.own_failed;
            return false;
        }
        if (!m_seen.insert(path).second) return false;
        if (count)
        {
            if (folder) ++m_out.foreign_dirs;
            else ++m_out.foreign_files;
            m_out.owners.insert(info.uid);
        }
        found.push_back({path, folder, info});
        m_all.push_back(found.back());
        return true;
    }

    // Breadth-first walk of 'queue' (and of /data/rpcs3 itself in the first round): candidates parents first. A walk a
    // limit stopped leaves the folders still to list in m_leftover. 'first': the round that starts at /data/rpcs3.
    std::vector<candidate> scan(work queue, bool first)
    {
        std::vector<candidate> found;
        const std::int64_t started = m_fs.now_ms();
        const std::string top(root);
        std::uint64_t entries = 0, visited = 0;
        // Files are stat'ed where FTP-made ones matter: /data/rpcs3 itself, the key folders, folders of another owner
        // (what the FTP server made holds what it uploaded). The app's own folders elsewhere (installed games) are
        // walked for their subfolders only, which keeps the walk cheap on every start; their files do not count
        // against the entry limit.
        const auto stat_files = [&](std::string_view rel, const entry_info& info)
        {
            return rel.empty() || is_key_folder(rel) || in_savedata(rel) || !m_out.app_uid || info.uid != *m_out.app_uid;
        };
        if (first)
        {
            const entry_info info = m_fs.stat(top);
            if (info.type == kind::directory)
            {
                m_real.insert(top);
                add(top, info, found, true);
                if (m_queued.insert(top).second) queue.push_front({top, true, 0});
            }
        }
        cap stopped = cap::none;
        std::vector<dir_entry> children;
        while (!queue.empty() && stopped == cap::none)
        {
            if (m_fs.now_ms() - started >= m_lim.scan_ms)
            {
                stopped = cap::time;
                break;
            }
            work_item item = std::move(queue.front());
            queue.pop_front();
            const std::string& folder = item.path;
            const bool with_files = item.files;
            if (!m_real.count(folder)) continue;
            children.clear();
            int error = 0;
            const std::size_t max = with_files ? item.skip + static_cast<std::size_t>(m_lim.max_entries - std::min<std::uint64_t>(entries, m_lim.max_entries)) + 1 : std::size_t{1} << 20;
            if (!m_fs.list(folder, children, error, max, with_files))
            {
                m_unlistable.insert(folder);
                continue;
            }
            bool partial = false;
            std::size_t index = 0;
            for (; index < children.size(); ++index)
            {
                const dir_entry& child = children[index];
                if (index < item.skip) continue;
                if (child.name.empty() || child.name == "." || child.name == ".." || child.name == probe_name) continue;
                if (moved_aside_name(relative(folder), child.name)) continue; // a cache folder moved aside: left as it is
                const bool counted = child.info.type != kind::file || child.details;
                if (counted && entries >= m_lim.max_entries)
                {
                    stopped = cap::entries;
                    partial = true;
                    break;
                }
                if ((++visited & 255) == 0 && m_fs.now_ms() - started >= m_lim.scan_ms)
                {
                    stopped = cap::time;
                    partial = true;
                    break;
                }
                if (counted) ++entries;
                if (child.info.type != kind::directory && child.info.type != kind::file) continue; // links never followed
                const std::string path = folder + "/" + child.name;
                if (!allowed_path(path)) continue;
                const std::string_view rel = relative(path);
                if (child.info.type == kind::directory)
                {
                    m_real.insert(path);
                    add(path, child.info, found, true);
                    if ((!in_source_folder(rel) || depth(rel) < 2) && m_queued.insert(path).second) queue.push_back({path, stat_files(rel, child.info), 0});
                }
                else if (child.details && !in_source_folder(rel))
                {
                    add(path, child.info, found, true);
                }
            }
            // Listed again when the walk goes on, from the entry it stopped at (readdir keeps its order)
            if (partial)
            {
                item.skip = index;
                queue.push_front(std::move(item));
            }
        }
        if (stopped != cap::none)
        {
            m_leftover = std::move(queue);
            m_last_cap = stopped;
        }
        // A key folder whose probe failed while its mode looks open (another cause) is sent as well
        for (const std::string& rel : m_out.key_blocked)
        {
            const std::string path = full_path(rel);
            if (!m_real.count(path) || m_seen.count(path)) continue;
            if (std::uint32_t owner = 0; m_fs.probe(path, owner) == 0) continue; // opened meanwhile (the app's own chmod above)
            const entry_info info = m_fs.stat(path);
            if (info.type != kind::directory || !m_seen.insert(path).second) continue;
            found.push_back({path, true, info});
            m_all.push_back(found.back());
        }
        std::stable_sort(found.begin(), found.end(), [](const candidate& a, const candidate& b) { return depth(relative(a.path)) < depth(relative(b.path)); });
        m_out.entries += entries;
        m_out.scan_ms += m_fs.now_ms() - started;
        return found;
    }

    // Only entries a listing showed (their folder chain real), inside /data/rpcs3
    bool sendable(const candidate& c) const
    {
        if (!allowed_path(c.path) || m_out.root_link) return false;
        return c.folder ? m_real.count(c.path) != 0 : m_real.count(parent_folder(c.path)) != 0;
    }

    void repair(const std::vector<candidate>& found)
    {
        const std::int64_t started = m_fs.now_ms();
        m_out.server = ds::to_string(m_server);
        ftp_control ftp(m_net, m_fs.now_ms, m_lim.reply_ms);
        std::string why;
        if (!ftp.open(m_server, m_lim.connect_ms, why))
        {
            m_out.ftp = why.rfind("connect failed", 0) == 0 ? ftp_state::unreachable : ftp_state::login_failed;
            m_out.ftp_why = why;
            m_out.repair_ms += m_fs.now_ms() - started;
            return;
        }
        m_out.banner = ftp.banner();
        m_out.ftp = ftp_state::done;
        std::uint32_t in_a_row = 0;
        bool reconnected = false;
        // The mode form is learnt on a path without a space; paths with one wait until it is known
        std::vector<const candidate*> order, spaced;
        for (const candidate& c : found) (c.path.find(' ') != std::string::npos ? spaced : order).push_back(&c);
        order.insert(order.end(), spaced.begin(), spaced.end());
        const auto send = [&](const candidate& c, const char* argument) -> ds::reply
        {
            const std::string line = std::string("SITE CHMOD ") + argument + " " + c.path;
            ++m_out.sent;
            ds::reply r = ftp.command(line);
            if (r.code <= 0 && !reconnected)
            {
                reconnected = true;
                if (ftp.open(m_server, m_lim.connect_ms, why)) r = ftp.command(line);
            }
            return r;
        };
        for (const candidate* entry : order)
        {
            const candidate& c = *entry;
            if (m_attempted.count(c.path) || !sendable(c)) continue;
            const bool spaced_path = c.path.find(' ') != std::string::npos;
            if (spaced_path && m_space_refused)
            {
                ++m_out.space_skipped;
                continue;
            }
            if (m_out.sent >= m_lim.max_commands || m_out.repair_ms + (m_fs.now_ms() - started) >= m_lim.repair_ms)
            {
                m_out.ftp = ftp_state::partial;
                m_out.ftp_why = m_out.sent >= m_lim.max_commands ? "command limit" : "time limit";
                break;
            }
            m_attempted.insert(c.path);
            ds::reply r = send(c, chmod_argument(c.folder, m_out.form));
            if (r.code >= 200 && r.code < 300)
            {
                ++m_out.accepted;
                if (!learn(c, spaced_path, send))
                {
                    m_out.ftp = ftp_state::wrong_mode;
                    break;
                }
                m_accepted.insert(c.path);
                in_a_row = 0;
                continue;
            }
            if (r.code == 500 || r.code == 502 || r.code == 504)
            {
                m_out.ftp = ftp_state::unsupported;
                m_out.ftp_why = r.text;
                break;
            }
            if (r.code <= 0)
            {
                m_out.ftp = ftp_state::partial;
                m_out.ftp_why = "the connection was lost";
                break;
            }
            ++m_out.refused;
            if (++in_a_row >= m_lim.max_refusals)
            {
                m_out.ftp = ftp_state::refused;
                m_out.ftp_why = r.text;
                break;
            }
        }
        ftp.close();
        m_out.repair_ms += m_fs.now_ms() - started;
    }

    // Cache folders still closed (the FTP server off, or it could not open them): each topmost one is renamed
    // "<name>.blocked-<unix time>" in its parent and made again 0777 by the app, which a rename inside one writable
    // folder allows. Only the cache tree, at most 16 folders; games, packages, licences, patches and dev_hdd0 never.
    void move_aside()
    {
        if (!m_fs.rename || !m_fs.unix_time || m_out.root_link) return;
        std::vector<std::string> closed;
        for (const candidate& c : m_all)
            if (c.folder && in_cache_tree(relative(c.path)) && m_real.count(c.path)) closed.push_back(c.path);
        std::stable_sort(closed.begin(), closed.end(), [](const std::string& a, const std::string& b) { return depth(relative(a)) < depth(relative(b)); });
        std::vector<std::string> targets;
        for (const std::string& path : closed)
        {
            if (std::any_of(targets.begin(), targets.end(), [&](const std::string& t) { return path.size() > t.size() && path.compare(0, t.size() + 1, t + "/") == 0; }))
                continue;
            const entry_info now = m_fs.stat(path);
            if (now.type == kind::directory && judge(now, m_out.app_uid) != verdict::fine) targets.push_back(path);
        }
        const std::string stamp = std::to_string(m_fs.unix_time());
        for (const std::string& path : targets)
        {
            if (m_out.moved_aside.size() >= 16) break;
            std::uint32_t owner = 0;
            const std::string to = path + std::string(blocked_suffix) + stamp;
            if (m_fs.probe(parent_folder(path), owner) != 0 || m_fs.rename(path, to) != 0)
            {
                ++m_out.move_failed;
                continue;
            }
            if (m_fs.make_dir(path, 0777) != 0) ++m_out.move_failed; // made again by the next start's probe
            m_moved.insert(path);
            m_out.moved_aside.push_back(std::string(relative(path)) + " -> " + std::string(relative(to)));
        }
    }

    bool moved(const std::string& path) const
    {
        for (const std::string& from : m_moved)
            if (path == from || (path.size() > from.size() && path.compare(0, from.size() + 1, from + "/") == 0)) return true;
        return false;
    }

    void verify()
    {
        std::vector<blocked_entry> blocked;
        std::uint32_t count = 0;
        const auto note = [&](std::string path, const entry_info& info)
        {
            ++count;
            if (blocked.size() < 64) blocked.push_back({std::move(path), info});
        };
        std::set<std::string> noted;
        // Key folders a blocked parent kept from being created are created now
        const std::vector<std::string> keys = m_out.key_blocked.empty() && m_all.empty() ? std::vector<std::string>{} : probe_keys(&m_out.created);
        for (const std::string& rel : keys)
        {
            const std::string path = full_path(rel);
            noted.insert(path);
            note(path, m_fs.stat(path));
        }
        for (const candidate& c : m_all)
        {
            if (noted.count(c.path) || moved(c.path)) continue;
            if (!m_attempted.count(c.path))
            {
                note(c.path, c.info);
                continue;
            }
            const entry_info now = m_fs.stat(c.path);
            if (now.type == kind::missing || judge(now, m_out.app_uid) == verdict::fine) ++m_out.repaired;
            else note(c.path, now);
        }
        // Key folders first, then folders, then files
        std::stable_sort(blocked.begin(), blocked.end(), [&](const blocked_entry& a, const blocked_entry& b)
        {
            const auto rank = [&](const blocked_entry& e) { return noted.count(e.path) ? 0 : e.info.type == kind::file ? 2 : 1; };
            return rank(a) < rank(b);
        });
        m_out.still_blocked = std::move(blocked);
        m_out.still_blocked_count = count;
    }
};
}

// Probe, walk, repair through the FTP server at 'server' and verify. 'on_repair' hears the number of entries before
// the first SITE CHMOD (a notice while a long repair runs).
inline report run(const fs_ops& fs, const ds::net_ops& net, const ds::endpoint& server, const limits& lim = {},
    const std::function<void(std::size_t)>& on_repair = {})
{
    report out;
    detail::repairer(fs, net, server, lim, out).run(on_repair);
    return out;
}

// A folder that refused a write just now, opened at once instead of at the next start: the app's own chmod 0777, else
// 'SITE CHMOD 777' through the FTP server ("511" again when "777" came out as 01411; another mode is put back). Only a
// folder inside /data/rpcs3 whose every component a listing shows as a real folder (never through a link). True when a
// write probe passes afterwards; 'log' gets one line.
inline bool reopen_folder(const fs_ops& fs, const ds::net_ops& net, const ds::endpoint& server, const std::string& folder, std::string& log,
    const limits& lim = {})
{
    const auto done = [&](bool ok, const std::string& what)
    {
        log = "fs perms: " + folder + ": " + what;
        return ok;
    };
    if (!allowed_path(folder) || folder == root) return done(false, "not a folder inside /data/rpcs3: nothing done");
    std::uint32_t owner = 0;
    if (fs.probe(folder, owner) == 0) return done(true, "writable");
    std::string at(root);
    std::string_view rest = relative(folder);
    while (!rest.empty())
    {
        const std::size_t slash = rest.find('/');
        const std::string name(rest.substr(0, slash));
        std::vector<dir_entry> entries;
        int error = 0;
        if (!fs.list(at, entries, error, 65536, false)) return done(false, at + " cannot be listed (" + ds::detail::errno_text(error) + "): nothing done");
        const auto it = std::find_if(entries.begin(), entries.end(), [&](const dir_entry& e) { return e.name == name; });
        if (it == entries.end() || it->info.type != kind::directory) return done(false, "not a real folder: nothing done");
        at += "/" + name;
        rest = slash == std::string_view::npos ? std::string_view{} : rest.substr(slash + 1);
    }
    const entry_info before = fs.stat(folder);
    if (fs.chmod(folder, 0777) == 0 && fs.probe(folder, owner) == 0) return done(true, "opened with the app's chmod: writable now");
    ftp_control ftp(net, fs.now_ms, lim.reply_ms);
    std::string why;
    if (!ftp.open(server, lim.connect_ms, why)) return done(false, "FTP " + ds::to_string(server) + ": " + why + ": still closed");
    ds::reply r = ftp.command("SITE CHMOD 777 " + folder);
    std::string sent = "SITE CHMOD 777";
    if (r.code >= 200 && r.code < 300)
    {
        const std::uint32_t mode = fs.stat(folder).mode & 07777u;
        if (mode == 01411u)
        {
            r = ftp.command("SITE CHMOD 511 " + folder);
            sent += ", then 511 (decimal)";
        }
        else if (mode != 0777u && mode != (before.mode & 07777u))
        {
            char back[8];
            std::snprintf(back, sizeof(back), "%o", static_cast<unsigned>(before.mode & 07777u));
            ftp.command(std::string("SITE CHMOD ") + back + " " + folder);
            sent += " set " + mode_text(mode) + ", put back";
        }
    }
    ftp.close();
    const bool ok = fs.probe(folder, owner) == 0;
    return done(ok, sent + " (reply " + std::to_string(r.code) + ")" + (ok ? ": writable now" : ": still closed"));
}
}

#ifdef __ORBIS__
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

namespace utils::ps4::fs_repair::sys
{
inline constexpr char pending_file[] = "/data/rpcs3/cache/.fs-repair-pending";

// lstat: a link is reported as one (never as its target), also on a file system without d_type
inline entry_info stat_entry(const std::string& path)
{
    struct ::stat st{};
    if (::lstat(path.c_str(), &st) != 0) return {};
    entry_info e;
    e.type = S_ISDIR(st.st_mode) ? kind::directory : S_ISREG(st.st_mode) ? kind::file : S_ISLNK(st.st_mode) ? kind::symlink : kind::other;
    e.uid = static_cast<std::uint32_t>(st.st_uid);
    e.mode = static_cast<std::uint32_t>(st.st_mode) & 07777u;
    return e;
}

// opendir/readdir (getdents) and stat, as fs::dir uses them; a DT_LNK entry is reported as a link and never stat'ed,
// a DT_REG entry is stat'ed only with 'stat_files'
inline fs_ops fs()
{
    fs_ops ops;
    ops.stat = stat_entry;
    ops.list = [](const std::string& path, std::vector<dir_entry>& out, int& error, std::size_t max, bool stat_files) -> bool
    {
        ::DIR* const dir = ::opendir(path.c_str());
        if (!dir)
        {
            error = errno ? errno : EIO;
            return false;
        }
        while (out.size() < max)
        {
            const ::dirent* const item = ::readdir(dir);
            if (!item) break;
            const std::string_view name = item->d_name;
            if (name.empty() || name == "." || name == "..") continue;
            dir_entry e{std::string(name), {}, true};
            if (item->d_type == DT_LNK) e.info.type = kind::symlink;
            else if (item->d_type == DT_REG && !stat_files)
            {
                e.info.type = kind::file;
                e.details = false;
            }
            else e.info = stat_entry(path + "/" + e.name);
            out.push_back(std::move(e));
        }
        ::closedir(dir);
        return true;
    };
    ops.probe = [](const std::string& folder, std::uint32_t& owner) -> int
    {
        const std::string path = folder + "/" + probe_name;
        const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
        if (fd < 0) return errno ? errno : EIO;
        struct ::stat st{};
        if (::fstat(fd, &st) == 0) owner = static_cast<std::uint32_t>(st.st_uid);
        ::close(fd);
        if (::unlink(path.c_str()) != 0) return errno ? errno : EIO;
        return 0;
    };
    ops.make_dir = [](const std::string& path, std::uint32_t mode) -> int
    {
        if (::mkdir(path.c_str(), static_cast<mode_t>(mode)) != 0)
        {
            const int error = errno;
            if (error != EEXIST) return error ? error : EIO;
        }
        ::chmod(path.c_str(), static_cast<mode_t>(mode)); // mkdir applied the umask
        return 0;
    };
    ops.chmod = [](const std::string& path, std::uint32_t mode) -> int
    {
        return ::chmod(path.c_str(), static_cast<mode_t>(mode)) == 0 ? 0 : (errno ? errno : EIO);
    };
    ops.rename = [](const std::string& from, const std::string& to) -> int
    {
        return ::rename(from.c_str(), to.c_str()) == 0 ? 0 : (errno ? errno : EIO);
    };
    ops.unix_time = [] { return static_cast<std::int64_t>(std::time(nullptr)); };
    ops.now_ms = [] { return ds::detail::now_ms(); };
    ops.pending = []
    {
        struct ::stat st{};
        return ::stat(pending_file, &st) == 0;
    };
    ops.set_pending = [](bool on)
    {
        if (!on)
        {
            ::unlink(pending_file);
            return;
        }
        const int fd = ::open(pending_file, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
        if (fd >= 0) ::close(fd);
    };
    return ops;
}

// The console's own FTP server: 127.0.0.1, at the port disc-ftp.txt names when it names the loopback, else 2121.
// Never another host: the repair is for this console's /data/rpcs3 only.
inline ds::endpoint server()
{
    if (const auto configured = ds::sys::configured_server(); configured && configured->ipv4 == ds::loopback_ipv4) return *configured;
    return {ds::loopback_ipv4, ds::default_port};
}
}
#endif
