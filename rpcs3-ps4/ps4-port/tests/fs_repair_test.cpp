// Host test of the /data/rpcs3 permission repair (rpcs3/util/ps4_fs_repair.hpp): an in-memory file system with owners
// and modes (the app's uid, the FTP server's uid 0, neither privileged) and a stand-in for the GoldHEN FTP server behind
// net_ops that applies SITE CHMOD with owner rules. Covers the field case (folders made over FTP), the path guard,
// symbolic links, the walk caps, the re-probe, unlistable folders and every fallback (server off, SITE CHMOD not
// implemented, refusals, a server that answers 200 without changing anything, a lost connection).
// Build (from the repo root):
//   clang++ -std=c++20 -O2 -static -Wall -Wextra -I rpcs3 ps4-port/tests/fs_repair_test.cpp -o fs_repair_test.exe
#include "../../rpcs3/util/ps4_fs_repair.hpp"

#include <cstdio>
#include <map>

namespace
{
namespace fr = utils::ps4::fs_repair;
namespace ds = utils::ps4::disc_stream;

int g_failures = 0;
int g_checks = 0;

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

constexpr std::uint32_t app_uid = 1000;
constexpr std::uint32_t ftp_uid = 0;

struct node
{
    fr::kind type = fr::kind::directory;
    std::uint32_t uid = 0;
    std::uint32_t mode = 0755;
    std::string target; // symlink
};

struct world
{
    std::map<std::string, node> nodes;
    std::int64_t clock = 0;
    std::int64_t list_cost = 0; // ms per listed entry
    std::uint32_t lists = 0;
    std::uint32_t file_stats = 0; // files listed with their uid and mode
    bool pending_mark = false;

    // FTP server
    bool server_on = true;
    // decimal: the mode digits read as decimal; fixed: every SITE CHMOD sets fixed_mode; space_lie: 200 without a change
    // for a path with a space
    enum class chmod_mode { normal, unsupported, refuse, lie, decimal, fixed, space_lie } site = chmod_mode::normal;
    std::uint32_t fixed_mode = 0700;
    bool can_rename = true;
    int drop_after = -1; // closes the control connection after this many SITE commands (once)
    bool dropped = false;
    std::vector<std::string> commands;
    std::string in, out;
    bool closed = false;
    int site_count = 0;
    int connects = 0;

    void dir(const std::string& path, std::uint32_t uid, std::uint32_t mode) { nodes[path] = {fr::kind::directory, uid, mode, {}}; }
    void file(const std::string& path, std::uint32_t uid, std::uint32_t mode) { nodes[path] = {fr::kind::file, uid, mode, {}}; }
    void link(const std::string& path, const std::string& target) { nodes[path] = {fr::kind::symlink, 0, 0777, target}; }

    const node* find(const std::string& path, bool follow = true) const
    {
        auto it = nodes.find(path);
        if (it == nodes.end()) return nullptr;
        if (follow && it->second.type == fr::kind::symlink) return find(it->second.target, true);
        return &it->second;
    }

    node* find_mut(const std::string& path)
    {
        auto it = nodes.find(path);
        if (it == nodes.end()) return nullptr;
        if (it->second.type == fr::kind::symlink) return find_mut(it->second.target);
        return &it->second;
    }

    static bool allows(const node& n, std::uint32_t uid, std::uint32_t bits_owner, std::uint32_t bits_other)
    {
        return uid == n.uid ? (n.mode & bits_owner) == bits_owner : (n.mode & bits_other) == bits_other;
    }

    static std::string parent(const std::string& path) { return path.substr(0, path.rfind('/')); }

    fr::fs_ops ops()
    {
        fr::fs_ops f;
        f.stat = [this](const std::string& path) -> fr::entry_info
        {
            const node* n = find(path);
            if (!n) return {};
            return {n->type, n->uid, n->mode};
        };
        f.list = [this](const std::string& path, std::vector<fr::dir_entry>& out_entries, int& error, std::size_t max, bool stat_files) -> bool
        {
            ++lists;
            const node* n = find(path);
            if (!n || n->type != fr::kind::directory)
            {
                error = ENOENT;
                return false;
            }
            if (!allows(*n, app_uid, 0500, 05))
            {
                error = EACCES;
                return false;
            }
            const std::string prefix = path + "/";
            for (auto it = nodes.lower_bound(prefix); it != nodes.end() && it->first.compare(0, prefix.size(), prefix) == 0; ++it)
            {
                const std::string name = it->first.substr(prefix.size());
                if (name.find('/') != std::string::npos) continue;
                if (out_entries.size() >= max) break;
                clock += list_cost;
                fr::dir_entry e{name, {}, true};
                if (it->second.type == fr::kind::symlink) e.info.type = fr::kind::symlink;
                else if (it->second.type == fr::kind::file && !stat_files)
                {
                    e.info.type = fr::kind::file;
                    e.details = false;
                }
                else
                {
                    e.info = {it->second.type, it->second.uid, it->second.mode};
                    if (it->second.type == fr::kind::file) ++file_stats;
                }
                out_entries.push_back(std::move(e));
            }
            return true;
        };
        f.probe = [this](const std::string& folder, std::uint32_t& owner) -> int
        {
            const node* n = find(folder);
            if (!n) return ENOENT;
            if (n->type != fr::kind::directory) return ENOTDIR;
            if (!allows(*n, app_uid, 0300, 03)) return EACCES;
            owner = app_uid;
            return 0;
        };
        f.make_dir = [this](const std::string& path, std::uint32_t mode) -> int
        {
            if (find(path)) return 0;
            const node* p = find(parent(path));
            if (!p) return ENOENT;
            if (!allows(*p, app_uid, 0300, 03)) return EACCES;
            dir(path, app_uid, mode);
            return 0;
        };
        f.chmod = [this](const std::string& path, std::uint32_t mode) -> int
        {
            node* n = find_mut(path);
            if (!n) return ENOENT;
            if (n->uid != app_uid) return EPERM;
            n->mode = mode;
            return 0;
        };
        f.rename = [this](const std::string& from, const std::string& to) -> int
        {
            if (!can_rename) return EXDEV;
            const node* p = find(parent(from));
            if (!p || !find(from, false)) return ENOENT;
            if (parent(from) != parent(to) || !allows(*p, app_uid, 0300, 03)) return EACCES;
            std::map<std::string, node> moved;
            for (auto it = nodes.begin(); it != nodes.end();)
            {
                if (it->first == from || it->first.compare(0, from.size() + 1, from + "/") == 0)
                {
                    moved[to + it->first.substr(from.size())] = it->second;
                    it = nodes.erase(it);
                }
                else ++it;
            }
            nodes.insert(moved.begin(), moved.end());
            return 0;
        };
        f.unix_time = [] { return std::int64_t{1792108800}; };
        f.now_ms = [this] { return clock; };
        f.pending = [this] { return pending_mark; };
        f.set_pending = [this](bool on) { pending_mark = on; };
        return f;
    }

    void serve_line(const std::string& line)
    {
        commands.push_back(line);
        if (line.rfind("USER ", 0) == 0) out += "230 User logged in, proceed.\r\n";
        else if (line.rfind("PASS ", 0) == 0) out += "230 ok\r\n";
        else if (line == "QUIT") out += "221 Goodbye.\r\n";
        else if (line.rfind("SITE ", 0) == 0)
        {
            ++site_count;
            if (drop_after >= 0 && !dropped && site_count > drop_after)
            {
                dropped = true;
                closed = true;
                return;
            }
            if (site == chmod_mode::unsupported)
            {
                out += "502 Command not implemented.\r\n";
                return;
            }
            if (site == chmod_mode::refuse)
            {
                out += "550 Requested action not taken.\r\n";
                return;
            }
            // "SITE CHMOD <mode> <path>"
            const std::string args = line.substr(11);
            const std::size_t space = args.find(' ');
            std::uint32_t mode = static_cast<std::uint32_t>(std::stoul(args.substr(0, space), nullptr, site == chmod_mode::decimal ? 10 : 8));
            if (site == chmod_mode::fixed) mode = fixed_mode;
            const std::string path = args.substr(space + 1);
            node* n = find_mut(path);
            if (site == chmod_mode::lie || (site == chmod_mode::space_lie && path.find(' ') != std::string::npos))
            {
                out += "200 OK\r\n";
                return;
            }
            if (!n || n->uid != ftp_uid)
            {
                out += "550 Operation not permitted.\r\n";
                return;
            }
            n->mode = mode;
            out += "200 SITE CHMOD command ok.\r\n";
        }
        else out += "502 Command not implemented.\r\n";
    }

    ds::net_ops net()
    {
        ds::net_ops n;
        n.connect = [this](const ds::endpoint& to, int, int, int& error) -> int
        {
            ++connects;
            if (!server_on || to.ipv4 != ds::loopback_ipv4 || to.port != ds::default_port)
            {
                error = ECONNREFUSED;
                return -1;
            }
            in.clear();
            out = "220 GoldHEN FTP server v2.2\r\n";
            closed = false;
            return 7;
        };
        n.send = [this](int, const void* data, std::size_t size, int, int& error) -> std::int64_t
        {
            if (closed)
            {
                error = EPIPE;
                return -1;
            }
            in.append(static_cast<const char*>(data), size);
            for (std::size_t eol; (eol = in.find("\r\n")) != std::string::npos;)
            {
                const std::string line = in.substr(0, eol);
                in.erase(0, eol + 2);
                serve_line(line);
            }
            return static_cast<std::int64_t>(size);
        };
        n.recv = [this](int, void* data, std::size_t size, int timeout_ms, int& error) -> std::int64_t
        {
            if (out.empty())
            {
                if (closed) return 0;
                clock += timeout_ms;
                error = ETIMEDOUT;
                return -1;
            }
            const std::size_t take = std::min(size, out.size());
            std::memcpy(data, out.data(), take);
            out.erase(0, take);
            return static_cast<std::int64_t>(take);
        };
        n.close = [](int) {};
        return n;
    }

    std::vector<std::string> sites() const
    {
        std::vector<std::string> list;
        for (const std::string& c : commands)
            if (c.rfind("SITE ", 0) == 0) list.push_back(c);
        return list;
    }

    fr::report run(const fr::limits& lim = {}, std::size_t* announced = nullptr)
    {
        const fr::fs_ops f = ops();
        const ds::net_ops n = net();
        return fr::run(f, n, {ds::loopback_ipv4, ds::default_port}, lim, [&](std::size_t count) { if (announced) *announced = count; });
    }
};

bool contains(const std::vector<std::string>& list, const std::string& item)
{
    return std::find(list.begin(), list.end(), item) != list.end();
}

std::size_t index_of(const std::vector<std::string>& list, const std::string& item)
{
    return static_cast<std::size_t>(std::find(list.begin(), list.end(), item) - list.begin());
}

// The user's case: /data/rpcs3 made by the app, several folders and a copied game made over FTP
world field_world()
{
    world w;
    w.dir("/data", 0, 0777);
    w.dir("/data/rpcs3", app_uid, 0777);
    w.file("/data/rpcs3/config.yml", ftp_uid, 0644);
    w.file("/data/rpcs3/games.yml", app_uid, 0644);
    w.dir("/data/rpcs3/games", ftp_uid, 0755);
    w.file("/data/rpcs3/games/Game.pkg", ftp_uid, 0644);
    w.dir("/data/rpcs3/games/BLES00001", ftp_uid, 0755);
    w.dir("/data/rpcs3/games/BLES00001/PS3_GAME", ftp_uid, 0755);
    w.file("/data/rpcs3/games/BLES00001/PS3_GAME/PARAM.SFO", ftp_uid, 0644);
    w.dir("/data/rpcs3/patches", ftp_uid, 0755);
    w.dir("/data/rpcs3/raps", ftp_uid, 0755);
    w.file("/data/rpcs3/raps/JP0177-NPJB00435_00-PJDF2MOMJTNSDAYO.rap", ftp_uid, 0644);
    w.dir("/data/rpcs3/cache", app_uid, 0755);
    w.dir("/data/rpcs3/cache/xmb", ftp_uid, 0755);
    w.dir("/data/rpcs3/cache/xmb/icons", ftp_uid, 0755);
    w.dir("/data/rpcs3/dev_hdd0", app_uid, 0755);
    w.dir("/data/rpcs3/dev_hdd0/game", ftp_uid, 0755);
    w.dir("/data/rpcs3/dev_hdd0/game/NPJB00435", ftp_uid, 0755);
    w.dir("/data/rpcs3/dev_hdd0/game/NPJB00435/USRDIR", ftp_uid, 0755);
    w.file("/data/rpcs3/dev_hdd0/game/NPJB00435/USRDIR/EBOOT.BIN", ftp_uid, 0644);
    w.file("/data/rpcs3/dev_hdd0/game/NPJB00435/PARAM.SFO", ftp_uid, 0644);
    w.dir("/data/rpcs3/dev_hdd0/home", app_uid, 0755);
    w.dir("/data/rpcs3/log", app_uid, 0755);
    w.file("/data/rpcs3/log/RPCS3.log", app_uid, 0644);
    w.dir("/data/rpcs3/dev_flash", app_uid, 0755);
    return w;
}

void test_path_guard()
{
    CHECK(fr::allowed_path("/data/rpcs3"));
    CHECK(fr::allowed_path("/data/rpcs3/games"));
    CHECK(fr::allowed_path("/data/rpcs3/dev_hdd0/game/NPJB00435/USRDIR/EBOOT.BIN"));
    CHECK(fr::allowed_path("/data/rpcs3/games/My Game (USA)"));
    CHECK(fr::allowed_path("/data/rpcs3/games/Jogo \xc3\xa9"));
    CHECK(!fr::allowed_path("/data/rpcs3/"));
    CHECK(!fr::allowed_path("/data/rpcs3x"));
    CHECK(!fr::allowed_path("/data/rpcs3x/games"));
    CHECK(!fr::allowed_path("/data/rpcs"));
    CHECK(!fr::allowed_path("/data"));
    CHECK(!fr::allowed_path("/"));
    CHECK(!fr::allowed_path(""));
    CHECK(!fr::allowed_path("data/rpcs3/games"));
    CHECK(!fr::allowed_path("/data/rpcs3/.."));
    CHECK(!fr::allowed_path("/data/rpcs3/../../system"));
    CHECK(!fr::allowed_path("/data/rpcs3/games/../../../system_ex"));
    CHECK(!fr::allowed_path("/data/rpcs3/./games"));
    CHECK(!fr::allowed_path("/data/rpcs3/games/."));
    CHECK(!fr::allowed_path("/data/rpcs3//games"));
    CHECK(!fr::allowed_path("/data/rpcs3/games/"));
    CHECK(!fr::allowed_path("/data/rpcs3/games\r\nDELE /data/rpcs3/config.yml"));
    CHECK(!fr::allowed_path("/data/rpcs3/games\nx"));
    CHECK(!fr::allowed_path(std::string("/data/rpcs3/a\0b", 16)));
    CHECK(!fr::allowed_path("/data/rpcs3/a\x7f"));
    CHECK(!fr::allowed_path("/data/rpcs3/a\\..\\..\\system"));
    CHECK(!fr::allowed_path("/user/data/rpcs3/games"));
    CHECK(!fr::allowed_path("/data/rpcs3/" + std::string(1100, 'a')));
}

void test_helpers()
{
    CHECK(fr::parent_folder("/data/rpcs3/dev_hdd0/game/NPJB00435/") == "/data/rpcs3/dev_hdd0/game");
    CHECK(fr::parent_folder("/data/rpcs3/dev_hdd0/game/NPJB00435/USRDIR/EBOOT.BIN") == "/data/rpcs3/dev_hdd0/game/NPJB00435/USRDIR");
    CHECK(fr::no_write_text("/data/rpcs3/dev_hdd0/game") ==
        "Sem permissão de gravação em /data/rpcs3/dev_hdd0/game · ligue o FTP do GoldHEN e reabra o RPCS3");
    CHECK(fr::no_write_text("/data/rpcs3/dev_hdd0/game").size() <= 116);
    CHECK(fr::permission_detail("open /data/rpcs3/games/BLES01876.iso.part: errno 13 (EACCES)"));
    CHECK(fr::permission_detail("rename x: errno 1 (EPERM)"));
    CHECK(!fr::permission_detail("open x: errno 28"));
    CHECK(fr::folder_in_detail("open /data/rpcs3/games/BLES01876.iso.part: errno 13 (EACCES)", "/data/rpcs3/games") == "/data/rpcs3/games");
    CHECK(fr::folder_in_detail("lock record /data/rpcs3/data/disc-lock/BLES01876.txt: errno 13 (EACCES)", "x") == "/data/rpcs3/data/disc-lock");
    CHECK(fr::folder_in_detail("no path: errno 13 (EACCES)", "/data/rpcs3/games") == "/data/rpcs3/games");
    CHECK(fr::folder_in_detail("open /data/rpcs3x/a: errno 13 (EACCES)", "/data/rpcs3/games") == "/data/rpcs3/games");
    fr::entry_info d{fr::kind::directory, ftp_uid, 0755};
    CHECK(fr::judge(d, app_uid) == fr::verdict::foreign);
    d.mode = 0777;
    CHECK(fr::judge(d, app_uid) == fr::verdict::fine);
    fr::entry_info f{fr::kind::file, ftp_uid, 0644};
    CHECK(fr::judge(f, app_uid) == fr::verdict::foreign);
    f.mode = 0666;
    CHECK(fr::judge(f, app_uid) == fr::verdict::fine);
    f = {fr::kind::file, app_uid, 0444};
    CHECK(fr::judge(f, app_uid) == fr::verdict::own);
    f = {fr::kind::file, app_uid, 0644};
    CHECK(fr::judge(f, app_uid) == fr::verdict::fine);
    CHECK(fr::judge({fr::kind::symlink, 0, 0}, app_uid) == fr::verdict::fine);
    CHECK(fr::judge({fr::kind::directory, app_uid, 0755}, std::nullopt) == fr::verdict::foreign);
}

void test_all_writable()
{
    world w;
    w.dir("/data", 0, 0777);
    w.dir("/data/rpcs3", app_uid, 0777);
    for (const char* rel : fr::key_folders)
        if (*rel) w.dir(fr::full_path(rel), app_uid, 0777);
    w.file("/data/rpcs3/config.yml", app_uid, 0644);
    w.dir("/data/rpcs3/dev_hdd0/game/BLUS00001", app_uid, 0755);
    const fr::report r = w.run();
    CHECK(w.connects == 0);
    CHECK(w.commands.empty());
    CHECK(!r.blocked());
    CHECK(r.ftp == fr::ftp_state::not_needed);
    CHECK(r.key_blocked.empty());
    CHECK(r.created.empty());
    CHECK(r.app_uid && *r.app_uid == app_uid);
    const auto lines = r.log_lines();
    CHECK(lines.size() == 1);
    CHECK(lines[0].find("all writable") != std::string::npos);
}

void test_field_case()
{
    world w = field_world();
    std::size_t announced = 0;
    const fr::report r = w.run({}, &announced);
    const auto sites = w.sites();
    for (const std::string& line : r.log_lines()) std::printf("  %s\n", line.c_str());
    // Every command is a SITE CHMOD of an allowed path
    for (const std::string& c : sites)
    {
        const bool dir_cmd = c.rfind("SITE CHMOD 777 ", 0) == 0, file_cmd = c.rfind("SITE CHMOD 666 ", 0) == 0;
        CHECK(dir_cmd || file_cmd);
        CHECK(fr::allowed_path(c.substr(15)));
    }
    for (const std::string& c : w.commands) CHECK(c.rfind("SITE CHMOD ", 0) == 0 || c == "USER anonymous" || c == "QUIT");
    CHECK(contains(sites, "SITE CHMOD 777 /data/rpcs3/games"));
    CHECK(contains(sites, "SITE CHMOD 777 /data/rpcs3/patches"));
    CHECK(contains(sites, "SITE CHMOD 777 /data/rpcs3/raps"));
    CHECK(contains(sites, "SITE CHMOD 777 /data/rpcs3/cache/xmb"));
    CHECK(contains(sites, "SITE CHMOD 777 /data/rpcs3/cache/xmb/icons"));
    CHECK(contains(sites, "SITE CHMOD 777 /data/rpcs3/dev_hdd0/game"));
    CHECK(contains(sites, "SITE CHMOD 777 /data/rpcs3/dev_hdd0/game/NPJB00435/USRDIR"));
    CHECK(contains(sites, "SITE CHMOD 666 /data/rpcs3/dev_hdd0/game/NPJB00435/USRDIR/EBOOT.BIN"));
    CHECK(contains(sites, "SITE CHMOD 666 /data/rpcs3/config.yml"));
    // Folders the app only reads: one level of folders, no file
    CHECK(contains(sites, "SITE CHMOD 777 /data/rpcs3/games/BLES00001"));
    CHECK(!contains(sites, "SITE CHMOD 777 /data/rpcs3/games/BLES00001/PS3_GAME"));
    CHECK(!contains(sites, "SITE CHMOD 666 /data/rpcs3/games/Game.pkg"));
    CHECK(!contains(sites, "SITE CHMOD 666 /data/rpcs3/raps/JP0177-NPJB00435_00-PJDF2MOMJTNSDAYO.rap"));
    // The app's own entries are never sent
    CHECK(!contains(sites, "SITE CHMOD 777 /data/rpcs3/cache"));
    CHECK(!contains(sites, "SITE CHMOD 777 /data/rpcs3"));
    CHECK(!contains(sites, "SITE CHMOD 666 /data/rpcs3/games.yml"));
    // Parents first
    CHECK(index_of(sites, "SITE CHMOD 777 /data/rpcs3/dev_hdd0/game") < index_of(sites, "SITE CHMOD 777 /data/rpcs3/dev_hdd0/game/NPJB00435"));
    CHECK(index_of(sites, "SITE CHMOD 777 /data/rpcs3/dev_hdd0/game/NPJB00435") < index_of(sites, "SITE CHMOD 777 /data/rpcs3/dev_hdd0/game/NPJB00435/USRDIR"));
    CHECK(index_of(sites, "SITE CHMOD 777 /data/rpcs3/cache/xmb") < index_of(sites, "SITE CHMOD 777 /data/rpcs3/cache/xmb/icons"));
    // Result
    CHECK(r.ftp == fr::ftp_state::done);
    CHECK(r.banner == "GoldHEN FTP server v2.2");
    CHECK(r.sent == sites.size());
    CHECK(r.accepted == r.sent);
    CHECK(r.repaired == r.sent);
    CHECK(!r.blocked());
    CHECK(announced == sites.size());
    CHECK(contains(r.key_blocked, "games") && contains(r.key_blocked, "patches") && contains(r.key_blocked, "cache/xmb") && contains(r.key_blocked, "dev_hdd0/game"));
    CHECK(r.owners == std::set<std::uint32_t>{ftp_uid});
    CHECK(w.find("/data/rpcs3/games")->mode == 0777);
    CHECK(w.find("/data/rpcs3/config.yml")->mode == 0666);
    CHECK(w.find("/data/rpcs3/dev_hdd0/game/NPJB00435/PARAM.SFO")->mode == 0666);
    CHECK(w.find("/data/rpcs3/games/Game.pkg")->mode == 0644);
    // Prevention: missing key folders created 0777 by the app, the app's own key folders opened to 0777
    CHECK(w.find("/data/rpcs3/pkgs") && w.find("/data/rpcs3/pkgs")->uid == app_uid && w.find("/data/rpcs3/pkgs")->mode == 0777);
    CHECK(w.find("/data/rpcs3/data/redump") && w.find("/data/rpcs3/data/disc-lock") && w.find("/data/rpcs3/dev_hdd1"));
    CHECK(w.find("/data/rpcs3/cache")->mode == 0777);
    CHECK(w.find("/data/rpcs3/log")->mode == 0777);
    CHECK(w.find("/data/rpcs3/dev_hdd0")->mode == 0777);
    CHECK(w.find("/data/rpcs3/dev_flash")->mode == 0755); // optional folders are only probed
    CHECK(!w.find("/data/rpcs3/dev_flash2"));
    CHECK(contains(r.created, "pkgs"));
    const auto lines = r.log_lines();
    CHECK(lines.size() == 3);
    CHECK(lines[0].find("owner uid 0") != std::string::npos);
    CHECK(lines[1].find("repaired " + std::to_string(r.sent) + " via FTP") != std::string::npos);
    CHECK(lines[2] == "fs perms: after the repair every checked entry is writable");

    // A second start finds nothing
    w.commands.clear();
    w.connects = 0;
    const fr::report again = w.run();
    CHECK(w.connects == 0);
    CHECK(!again.blocked());
    CHECK(again.ftp == fr::ftp_state::not_needed);
}

void test_root_made_over_ftp()
{
    // The user made /data/rpcs3 itself with FileZilla before the first start: nothing is writable, the app uid is unknown
    world w;
    w.dir("/data", 0, 0777);
    w.dir("/data/rpcs3", ftp_uid, 0755);
    w.dir("/data/rpcs3/games", ftp_uid, 0755);
    w.file("/data/rpcs3/games/a.pkg", ftp_uid, 0644);
    const fr::report r = w.run();
    const auto sites = w.sites();
    CHECK(!sites.empty() && sites.front() == "SITE CHMOD 777 /data/rpcs3");
    CHECK(contains(sites, "SITE CHMOD 777 /data/rpcs3/games"));
    CHECK(!r.blocked());
    CHECK(r.app_uid && *r.app_uid == app_uid); // learnt by the re-probe
    CHECK(w.find("/data/rpcs3/dev_hdd0/game") && w.find("/data/rpcs3/dev_hdd0/game")->uid == app_uid);
    CHECK(contains(r.created, "dev_hdd0") && contains(r.created, "cache/xmb"));
}

void test_server_off()
{
    world w = field_world();
    w.server_on = false;
    const fr::report r = w.run();
    CHECK(w.commands.empty());
    CHECK(r.ftp == fr::ftp_state::unreachable);
    CHECK(!r.ftp_answered());
    CHECK(r.blocked());
    CHECK(r.repaired == 0);
    const auto folders = r.blocked_folders(3);
    CHECK(folders.size() == 3);
    CHECK(contains(folders, "dev_hdd0/game") || contains(folders, "games"));
    // Key folders lead the list
    CHECK(!r.still_blocked.empty() && fr::relative(r.still_blocked.front().path) == "dev_hdd0/game");
    const auto lines = r.log_lines();
    CHECK(lines.size() == 4);
    CHECK(lines[1].find("connect failed") != std::string::npos && lines[1].find("GoldHEN FTP server off?") != std::string::npos);
    // Without the server only the cache is helped: its closed folder moved aside and made again by the app
    CHECK(lines[2] == "fs perms: cache moved aside: cache/xmb -> cache/xmb.blocked-1792108800 (made by the FTP server, not writable): a new cache is used");
    CHECK(lines[3].rfind("fs perms: still blocked (", 0) == 0);
    CHECK(lines[3].find("/data/rpcs3/dev_hdd0/game (mode 0755 uid 0)") != std::string::npos && lines[3].find("cache/xmb") == std::string::npos);
    for (const std::string& line : lines) std::printf("  %s\n", line.c_str());
    // Nothing changed on the file system except what the app may do itself
    CHECK(w.find("/data/rpcs3/games")->mode == 0755);
    CHECK(w.find("/data/rpcs3/cache/xmb")->uid == app_uid && w.find("/data/rpcs3/cache/xmb")->mode == 0777);
    CHECK(w.find("/data/rpcs3/cache/xmb.blocked-1792108800/icons") && w.find("/data/rpcs3/cache/xmb.blocked-1792108800")->uid == ftp_uid);
    // The next start leaves the moved folder alone
    const fr::report again = w.run();
    CHECK(again.moved_aside.empty() && again.foreign_dirs == r.foreign_dirs - 2);
    for (const fr::blocked_entry& e : again.still_blocked) CHECK(e.path.find(".blocked-") == std::string::npos);
}

// The server reads the SITE CHMOD digits as decimal (777 -> 01411): learnt on the first entry, 511 / 438 from then on
void test_decimal_server()
{
    world w = field_world();
    w.site = world::chmod_mode::decimal;
    const fr::report r = w.run();
    const auto sites = w.sites();
    // The first entry is /data/rpcs3/config.yml (a file): 666 -> 01232, then 438
    CHECK(sites.size() >= 3 && sites[0] == "SITE CHMOD 666 /data/rpcs3/config.yml" && sites[1] == "SITE CHMOD 438 /data/rpcs3/config.yml");
    for (std::size_t i = 2; i < sites.size(); ++i) CHECK(sites[i].rfind("SITE CHMOD 511 ", 0) == 0 || sites[i].rfind("SITE CHMOD 438 ", 0) == 0);
    CHECK(r.form == fr::chmod_form::decimal && r.ftp == fr::ftp_state::done && !r.blocked());
    CHECK(w.find("/data/rpcs3/games")->mode == 0777 && w.find("/data/rpcs3/config.yml")->mode == 0666);
    CHECK(r.log_lines()[1].find("the server reads the mode as decimal: 511/438 sent") != std::string::npos);
}

// A server that sets another mode: the entry is put back and nothing more is sent
void test_wrong_mode_server()
{
    world w = field_world();
    w.site = world::chmod_mode::fixed;
    w.fixed_mode = 0700;
    const fr::report r = w.run();
    const auto sites = w.sites();
    CHECK(sites.size() == 2 && sites[0] == "SITE CHMOD 666 /data/rpcs3/config.yml" && sites[1] == "SITE CHMOD 644 /data/rpcs3/config.yml");
    CHECK(r.ftp == fr::ftp_state::wrong_mode && r.ftp_answered() && r.blocked());
    const auto lines = r.log_lines();
    CHECK(std::find(lines.begin(), lines.end(), "fs perms: SITE CHMOD changed the mode to 0700, not 0666: stopped") != lines.end());
    // A server that puts the mode back as asked
    world back = field_world();
    back.site = world::chmod_mode::fixed;
    back.fixed_mode = 0640;
    back.run();
    CHECK(back.sites().size() == 2 && back.find("/data/rpcs3/config.yml")->mode == 0640); // the fake applies 0640 to any mode
}

// A server that answers 200 for a path with a space without applying it: the rest of such paths are left out
void test_space_paths()
{
    world w = field_world();
    w.site = world::chmod_mode::space_lie;
    w.dir("/data/rpcs3/dev_hdd0/game/A B", ftp_uid, 0755);
    w.dir("/data/rpcs3/dev_hdd0/game/C D", ftp_uid, 0755);
    w.file("/data/rpcs3/config one.yml", ftp_uid, 0644);
    const fr::report r = w.run();
    const auto sites = w.sites();
    std::size_t spaced = 0;
    for (const std::string& c : sites) spaced += c.find(' ', 15) != std::string::npos;
    CHECK(spaced == 1 && r.space_skipped == 2);
    CHECK(r.form == fr::chmod_form::octal && r.ftp == fr::ftp_state::done);
    CHECK(w.find("/data/rpcs3/games")->mode == 0777);
    const auto lines = r.log_lines();
    CHECK(std::find(lines.begin(), lines.end(), "fs perms: 2 path(s) with a space left out (the server did not apply SITE CHMOD to such a path)") != lines.end());
    // A server that applies them goes on with every one
    world ok = field_world();
    ok.dir("/data/rpcs3/dev_hdd0/game/A B", ftp_uid, 0755);
    ok.dir("/data/rpcs3/dev_hdd0/game/C D", ftp_uid, 0755);
    const fr::report all = ok.run();
    CHECK(all.space_skipped == 0 && !all.blocked() && ok.find("/data/rpcs3/dev_hdd0/game/C D")->mode == 0777);
}

// The whole cache made over FTP and the server off: /data/rpcs3/cache moved aside and made again
void test_cache_moved_aside()
{
    world w;
    w.dir("/data", 0, 0777);
    w.dir("/data/rpcs3", app_uid, 0777);
    w.dir("/data/rpcs3/cache", ftp_uid, 0755);
    w.dir("/data/rpcs3/cache/cache", ftp_uid, 0755);
    w.dir("/data/rpcs3/cache/cache/BLES00229", ftp_uid, 0755);
    w.file("/data/rpcs3/cache/cache/BLES00229/x.obj", ftp_uid, 0644);
    w.dir("/data/rpcs3/games", ftp_uid, 0755);
    w.server_on = false;
    const fr::report r = w.run();
    CHECK(r.moved_aside == std::vector<std::string>{"cache -> cache.blocked-1792108800"});
    CHECK(w.find("/data/rpcs3/cache")->uid == app_uid && w.find("/data/rpcs3/cache/xmb") && w.find("/data/rpcs3/cache.blocked-1792108800/cache/BLES00229/x.obj"));
    CHECK(w.find("/data/rpcs3/games")->uid == ftp_uid); // never moved
    for (const fr::blocked_entry& e : r.still_blocked) CHECK(e.path.find("/cache") == std::string::npos);
    CHECK(r.blocked_folders(8) == std::vector<std::string>{"games"});
    // Without a rename nothing is moved
    world n;
    n.dir("/data", 0, 0777);
    n.dir("/data/rpcs3", app_uid, 0777);
    n.dir("/data/rpcs3/cache", ftp_uid, 0755);
    n.server_on = false;
    n.can_rename = false;
    const fr::report nr = n.run();
    CHECK(nr.moved_aside.empty() && nr.move_failed == 1 && nr.blocked());
}

// One folder at once (a refused write): FTP when another owner made it, the app's chmod for its own, nothing through a
// link or outside /data/rpcs3
void test_reopen_folder()
{
    world w = field_world();
    const fr::fs_ops f = w.ops();
    const ds::net_ops n = w.net();
    const ds::endpoint server{ds::loopback_ipv4, ds::default_port};
    std::string log;
    CHECK(fr::reopen_folder(f, n, server, "/data/rpcs3/dev_hdd0/game", log) && w.find("/data/rpcs3/dev_hdd0/game")->mode == 0777);
    CHECK(log == "fs perms: /data/rpcs3/dev_hdd0/game: SITE CHMOD 777 (reply 200): writable now");
    CHECK(fr::reopen_folder(f, n, server, "/data/rpcs3/dev_hdd0/game", log) && log == "fs perms: /data/rpcs3/dev_hdd0/game: writable");
    w.dir("/data/rpcs3/log", app_uid, 0555);
    CHECK(fr::reopen_folder(f, n, server, "/data/rpcs3/log", log) && log.find("the app's chmod") != std::string::npos);
    w.link("/data/rpcs3/linked", "/data/rpcs3/games");
    CHECK(!fr::reopen_folder(f, n, server, "/data/rpcs3/linked", log) && log.find("not a real folder") != std::string::npos);
    CHECK(!fr::reopen_folder(f, n, server, "/system", log) && !fr::reopen_folder(f, n, server, "/data/rpcs3", log));
    w.server_on = false;
    CHECK(!fr::reopen_folder(f, n, server, "/data/rpcs3/patches", log) && log.find("still closed") != std::string::npos);
    w.server_on = true;
    w.site = world::chmod_mode::decimal;
    CHECK(fr::reopen_folder(f, n, server, "/data/rpcs3/patches", log) && log.find("then 511 (decimal)") != std::string::npos);
}

// The port's user folder and its licence folder are key folders (probed and repaired by name); save files are stat'ed
void test_user_folders()
{
    const auto at = [](std::string_view name) { return static_cast<std::size_t>(std::find_if(fr::key_folders.begin(), fr::key_folders.end(), [&](const char* k) { return name == k; }) - fr::key_folders.begin()); };
    CHECK(fr::key_folders.size() == 24 && at("dev_hdd0/home") < at("dev_hdd0/home/00000001") && at("dev_hdd0/home/00000001") < at("dev_hdd0/home/00000001/exdata"));
    CHECK(at("keys") < fr::key_folders.size());
    CHECK(at("cheats") < fr::key_folders.size());
    CHECK(at("dev_hdd0/home/00000001/exdata") < fr::key_folders.size());
    CHECK(fr::in_savedata("dev_hdd0/home/00000001/savedata") && fr::in_savedata("dev_hdd0/home/00000001/savedata/BLUS30443-SAVE") && !fr::in_savedata("dev_hdd0/home/00000001"));
    CHECK(!fr::in_savedata("dev_hdd0/home//savedata") && !fr::in_savedata("dev_hdd0/game/savedata"));
    world w;
    w.dir("/data", 0, 0777);
    w.dir("/data/rpcs3", app_uid, 0777);
    for (const char* rel : fr::key_folders)
        if (*rel) w.dir(fr::full_path(rel), app_uid, 0777);
    w.dir("/data/rpcs3/dev_hdd0/home/00000001", ftp_uid, 0755); // made over FTP: no user name can be written
    w.file("/data/rpcs3/dev_hdd0/home/00000001/localusername", ftp_uid, 0644);
    w.dir("/data/rpcs3/dev_hdd0/home/00000001/savedata", app_uid, 0777);
    w.dir("/data/rpcs3/dev_hdd0/home/00000001/savedata/BLUS30443-SAVE", app_uid, 0777);
    w.file("/data/rpcs3/dev_hdd0/home/00000001/savedata/BLUS30443-SAVE/USR-DATA", ftp_uid, 0644);
    const fr::report r = w.run();
    const auto sites = w.sites();
    CHECK(contains(r.key_blocked, "dev_hdd0/home/00000001"));
    CHECK(contains(sites, "SITE CHMOD 777 /data/rpcs3/dev_hdd0/home/00000001") && contains(sites, "SITE CHMOD 666 /data/rpcs3/dev_hdd0/home/00000001/localusername"));
    CHECK(contains(sites, "SITE CHMOD 666 /data/rpcs3/dev_hdd0/home/00000001/savedata/BLUS30443-SAVE/USR-DATA"));
    CHECK(!r.blocked());
}

// The disc key inbox /data/rpcs3/keys: made 0777 by the app when missing (FTP uploads land in it); made over FTP it is
// opened like any key folder, but nothing below it (the player's key files, lists, IRDs and their folders) is ever sent
// a SITE CHMOD or chmod'ed by the app
void test_key_inbox()
{
    CHECK(fr::in_inbox("keys/a.txt") && fr::in_inbox("keys/sub/x.dkey") && !fr::in_inbox("keys") && !fr::in_inbox("keysx/a") && !fr::in_inbox("data/keys/a"));
    world fresh;
    fresh.dir("/data", 0, 0777);
    fresh.dir("/data/rpcs3", app_uid, 0777);
    const fr::report made = fresh.run();
    CHECK(contains(made.created, "keys") && fresh.find("/data/rpcs3/keys") && fresh.find("/data/rpcs3/keys")->uid == app_uid &&
        fresh.find("/data/rpcs3/keys")->mode == 0777);

    world w;
    w.dir("/data", 0, 0777);
    w.dir("/data/rpcs3", app_uid, 0777);
    for (const char* rel : fr::key_folders)
        if (*rel) w.dir(fr::full_path(rel), app_uid, 0777);
    w.dir("/data/rpcs3/keys", ftp_uid, 0755);
    w.file("/data/rpcs3/keys/meus-discos.txt", ftp_uid, 0644);
    w.file("/data/rpcs3/keys/BLES01876.dkey", ftp_uid, 0644);
    w.dir("/data/rpcs3/keys/ird", ftp_uid, 0755);
    w.file("/data/rpcs3/keys/ird/FIFA 14.ird", ftp_uid, 0644);
    const fr::report r = w.run();
    const auto sites = w.sites();
    CHECK(contains(sites, "SITE CHMOD 777 /data/rpcs3/keys") && contains(r.key_blocked, "keys"));
    for (const std::string& c : sites) CHECK(c.find("/data/rpcs3/keys/") == std::string::npos);
    CHECK(w.find("/data/rpcs3/keys/meus-discos.txt")->mode == 0644 && w.find("/data/rpcs3/keys/ird")->mode == 0755 &&
        w.find("/data/rpcs3/keys/ird/FIFA 14.ird")->mode == 0644);
    CHECK(!r.blocked());
}

void test_unsupported()
{
    world w = field_world();
    w.site = world::chmod_mode::unsupported;
    const fr::report r = w.run();
    CHECK(w.sites().size() == 1);
    CHECK(r.ftp == fr::ftp_state::unsupported);
    CHECK(r.ftp_answered());
    CHECK(r.blocked());
    CHECK(r.log_lines()[1].find("SITE CHMOD not supported ('502 Command not implemented.')") != std::string::npos);
}

void test_refusals()
{
    world w = field_world();
    w.site = world::chmod_mode::refuse;
    fr::limits lim;
    lim.max_refusals = 5;
    const fr::report r = w.run(lim);
    CHECK(w.sites().size() == 5);
    CHECK(r.ftp == fr::ftp_state::refused);
    CHECK(r.refused == 5);
    CHECK(r.blocked());
}

void test_lying_server()
{
    // 200 without a change: the stat after the first one stops the repair (nothing to put back); the re-probe decides
    world w = field_world();
    w.site = world::chmod_mode::lie;
    const fr::report r = w.run();
    CHECK(r.accepted == r.sent && r.sent == 1);
    CHECK(r.ftp == fr::ftp_state::wrong_mode && r.wrong_mode == 0644 && r.wanted_mode == 0666);
    CHECK(r.repaired == 0);
    CHECK(r.blocked());
    CHECK(r.still_blocked_count >= r.sent);
}

void test_symlinks()
{
    world w = field_world();
    w.dir("/system", 0, 0755);
    w.file("/system/secret", 0, 0600);
    w.link("/data/rpcs3/evil", "/system");
    w.link("/data/rpcs3/dev_hdd0/game/link", "/system");
    const fr::report r = w.run();
    for (const std::string& c : w.commands)
    {
        CHECK(c.find("evil") == std::string::npos);
        CHECK(c.find("/system") == std::string::npos);
        CHECK(c.find("/link") == std::string::npos);
    }
    CHECK(w.find("/system")->mode == 0755);
    CHECK(!r.blocked());

    // /data/rpcs3 itself a link: nothing is sent
    world l;
    l.dir("/data", 0, 0777);
    l.dir("/elsewhere", ftp_uid, 0755);
    l.dir("/elsewhere/games", ftp_uid, 0755);
    l.link("/data/rpcs3", "/elsewhere");
    const fr::report lr = l.run();
    CHECK(lr.root_link);
    CHECK(l.commands.empty());
    CHECK(l.find("/elsewhere")->mode == 0755);
    CHECK(lr.log_lines()[0].find("symbolic link") != std::string::npos);
}

void test_hostile_names()
{
    world w = field_world();
    w.dir("/data/rpcs3/dev_hdd0/game/A\r\nDELE x", ftp_uid, 0755);
    w.file("/data/rpcs3/dev_hdd0/game/A\r\nDELE x/f", ftp_uid, 0644);
    w.dir("/data/rpcs3/dev_hdd0/game/B\\..", ftp_uid, 0755);
    w.run();
    for (const std::string& c : w.commands)
    {
        CHECK(c.find("DELE") == std::string::npos);
        CHECK(c.find('\\') == std::string::npos);
    }
}

void test_entry_cap()
{
    world w = field_world();
    w.dir("/data/rpcs3/dev_hdd0/game/BIG", ftp_uid, 0755);
    for (int i = 0; i < 3000; ++i) w.file("/data/rpcs3/dev_hdd0/game/BIG/f" + std::to_string(i), ftp_uid, 0644);
    fr::limits lim;
    lim.max_entries = 500;
    // While the server repairs, the walk goes on where the limit stopped it: everything in one start
    const fr::report r = w.run(lim);
    CHECK(!r.capped_entries);
    CHECK(r.rounds > 1);
    CHECK(r.sent == 3012 + 1); // the field case's 12 entries, BIG and its 3000 files
    CHECK(!r.blocked());
    for (const std::string& line : r.log_lines()) std::printf("  %s\n", line.c_str());

    // Without a server the walk stops at the limit and says so
    world off = field_world();
    off.dir("/data/rpcs3/dev_hdd0/game/BIG", ftp_uid, 0755);
    for (int i = 0; i < 3000; ++i) off.file("/data/rpcs3/dev_hdd0/game/BIG/f" + std::to_string(i), ftp_uid, 0644);
    off.server_on = false;
    const fr::report o = off.run(lim);
    CHECK(o.capped_entries);
    CHECK(o.entries == 500);
    CHECK(o.blocked());
    CHECK(o.log_lines()[0].find("stopped at the entry limit") != std::string::npos);

    // The command limit: later starts go on (repaired entries are found fine and the walk continues past them)
    world slow = field_world();
    slow.dir("/data/rpcs3/dev_hdd0/game/BIG", ftp_uid, 0755);
    for (int i = 0; i < 3000; ++i) slow.file("/data/rpcs3/dev_hdd0/game/BIG/f" + std::to_string(i), ftp_uid, 0644);
    fr::limits few = lim;
    few.max_commands = 400;
    std::uint32_t total = 0;
    int starts = 0;
    for (; starts < 20; ++starts)
    {
        const fr::report s = slow.run(few);
        total += s.sent;
        if (!s.blocked()) break;
    }
    CHECK(total == 3013);
    CHECK(starts <= 9);
    CHECK(!slow.pending_mark);

    // A big library of the app's own, nothing to repair: the walk stops at its limit every start and leaves no mark
    world big;
    big.dir("/data", 0, 0777);
    big.dir("/data/rpcs3", app_uid, 0777);
    for (const char* rel : fr::key_folders)
        if (*rel) big.dir(fr::full_path(rel), app_uid, 0777);
    for (int i = 0; i < 700; ++i) big.dir("/data/rpcs3/dev_hdd0/game/G" + std::to_string(i), app_uid, 0755);
    const fr::report b = big.run(lim);
    CHECK(b.capped_entries);
    CHECK(!big.pending_mark);
    CHECK(big.connects == 0);
}

void test_time_cap()
{
    world w = field_world();
    w.dir("/data/rpcs3/dev_hdd0/game/BIG", ftp_uid, 0755);
    for (int i = 0; i < 3000; ++i) w.file("/data/rpcs3/dev_hdd0/game/BIG/f" + std::to_string(i), ftp_uid, 0644);
    w.list_cost = 1;
    w.server_on = false;
    fr::limits lim;
    lim.scan_ms = 200;
    const fr::report r = w.run(lim);
    CHECK(r.capped_time);
    CHECK(r.scan_ms < 200 + 3000); // one listing may finish past the limit
    CHECK(r.log_lines()[0].find("stopped at the time limit") != std::string::npos);
    CHECK(w.pending_mark); // entries to repair and an unfinished walk: the next start goes on

    // With the server on, the walk goes on after each repair within the repair's time budget
    w.server_on = true;
    const fr::report on = w.run(lim);
    CHECK(on.resumed);
    CHECK(!on.capped_time);
    CHECK(!on.blocked());
    CHECK(!w.pending_mark);
}

void test_command_cap()
{
    world w = field_world();
    fr::limits lim;
    lim.max_commands = 3;
    const fr::report r = w.run(lim);
    CHECK(w.sites().size() == 3);
    CHECK(r.ftp == fr::ftp_state::partial);
    CHECK(r.blocked());
    CHECK(r.log_lines()[1].find("command limit") != std::string::npos);
}

void test_connection_lost()
{
    world w = field_world();
    w.drop_after = 2;
    const fr::report r = w.run();
    CHECK(w.connects == 2); // one reconnection
    CHECK(r.ftp == fr::ftp_state::done);
    CHECK(!r.blocked());

    // The reconnection is refused: partial
    world e = field_world();
    e.drop_after = 1;
    fr::fs_ops f = e.ops();
    ds::net_ops n = e.net();
    auto connect = n.connect;
    int connects = 0;
    n.connect = [&](const ds::endpoint& to, int t, int b, int& error) { return ++connects > 1 ? (error = ECONNREFUSED, -1) : connect(to, t, b, error); };
    const fr::report p = fr::run(f, n, {ds::loopback_ipv4, ds::default_port});
    CHECK(p.ftp == fr::ftp_state::partial);
    CHECK(p.ftp_why == "the connection was lost");
    CHECK(p.blocked());
}

void test_unlistable_folder()
{
    world w = field_world();
    w.dir("/data/rpcs3/dev_hdd0/game/SECRET", ftp_uid, 0700);
    w.dir("/data/rpcs3/dev_hdd0/game/SECRET/USRDIR", ftp_uid, 0700);
    w.file("/data/rpcs3/dev_hdd0/game/SECRET/USRDIR/save.dat", ftp_uid, 0600);
    const fr::report r = w.run();
    const auto sites = w.sites();
    CHECK(contains(sites, "SITE CHMOD 777 /data/rpcs3/dev_hdd0/game/SECRET"));
    CHECK(contains(sites, "SITE CHMOD 777 /data/rpcs3/dev_hdd0/game/SECRET/USRDIR"));
    CHECK(contains(sites, "SITE CHMOD 666 /data/rpcs3/dev_hdd0/game/SECRET/USRDIR/save.dat"));
    CHECK(r.rounds >= 2);
    CHECK(!r.blocked());
}

void test_own_entries()
{
    world w;
    w.dir("/data", 0, 0777);
    w.dir("/data/rpcs3", app_uid, 0777);
    for (const char* rel : fr::key_folders)
        if (*rel) w.dir(fr::full_path(rel), app_uid, 0777);
    w.file("/data/rpcs3/custom_configs/config_BLES00001.yml", app_uid, 0444);
    w.dir("/data/rpcs3/dev_hdd0/home/00000001", app_uid, 0555);
    const fr::report r = w.run();
    CHECK(w.connects == 0);
    CHECK(r.own_fixed == 2);
    CHECK(w.find("/data/rpcs3/custom_configs/config_BLES00001.yml")->mode == 0666);
    CHECK(w.find("/data/rpcs3/dev_hdd0/home/00000001")->mode == 0777);
}

void test_cheap_walk()
{
    // An installed game of the app: walked for folders only (no stat per file); files the FTP server uploaded into a key
    // folder (patch.yml, a per-game config) or into its own folders are found
    world w;
    w.dir("/data", 0, 0777);
    w.dir("/data/rpcs3", app_uid, 0777);
    for (const char* rel : fr::key_folders)
        if (*rel) w.dir(fr::full_path(rel), app_uid, 0777);
    w.dir("/data/rpcs3/dev_hdd0/game/BLUS00001", app_uid, 0755);
    w.dir("/data/rpcs3/dev_hdd0/game/BLUS00001/USRDIR", app_uid, 0755);
    for (int i = 0; i < 1000; ++i) w.file("/data/rpcs3/dev_hdd0/game/BLUS00001/USRDIR/f" + std::to_string(i), app_uid, 0644);
    w.file("/data/rpcs3/patches/patch.yml", ftp_uid, 0644);
    w.file("/data/rpcs3/custom_configs/config_BLES00001.yml", ftp_uid, 0644);
    w.dir("/data/rpcs3/dev_hdd0/game/BLUS00002", ftp_uid, 0777); // made over FTP, already opened
    w.file("/data/rpcs3/dev_hdd0/game/BLUS00002/EBOOT.BIN", ftp_uid, 0644);
    const fr::report r = w.run();
    const auto sites = w.sites();
    CHECK(w.file_stats < 20);
    CHECK(contains(sites, "SITE CHMOD 666 /data/rpcs3/patches/patch.yml"));
    CHECK(contains(sites, "SITE CHMOD 666 /data/rpcs3/custom_configs/config_BLES00001.yml"));
    CHECK(contains(sites, "SITE CHMOD 666 /data/rpcs3/dev_hdd0/game/BLUS00002/EBOOT.BIN"));
    CHECK(!contains(sites, "SITE CHMOD 777 /data/rpcs3/dev_hdd0/game/BLUS00002"));
    CHECK(sites.size() == 3);
    CHECK(!r.blocked());
}

void test_other_port_or_host()
{
    // The repair only talks to the endpoint it is given; sys::server() never names another host (not exercised here)
    world w = field_world();
    const fr::fs_ops f = w.ops();
    const ds::net_ops n = w.net();
    const fr::report r = fr::run(f, n, {0xc0a80121, 2121});
    CHECK(r.ftp == fr::ftp_state::unreachable);
    CHECK(w.commands.empty());
}
}

int main()
{
    test_path_guard();
    test_helpers();
    test_all_writable();
    test_field_case();
    test_root_made_over_ftp();
    test_server_off();
    test_unsupported();
    test_refusals();
    test_lying_server();
    test_symlinks();
    test_hostile_names();
    test_entry_cap();
    test_time_cap();
    test_command_cap();
    test_connection_lost();
    test_unlistable_folder();
    test_own_entries();
    test_cheap_walk();
    test_other_port_or_host();
    test_decimal_server();
    test_wrong_mode_server();
    test_space_paths();
    test_cache_moved_aside();
    test_reopen_folder();
    test_user_folders();
    test_key_inbox();
    std::printf("%s: %d checks, %d failures\n", g_failures ? "FAIL" : "PASS", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
