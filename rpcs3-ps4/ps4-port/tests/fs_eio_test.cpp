// Host test of the stat EIO resolver (rpcs3/util/ps4_disc_probe.hpp, utils::ps4::stat_eio), the part of the file layer
// (Utilities/File.cpp, fs::get_stat) that decides what a stat() answering EIO really meant. A fake tree stands in for
// the console: its stat() answers EIO for a path under a missing folder, as the console does, open() says ENOENT.
//  1 the console case: a file under a missing folder is "not found" after one open(), without a stat or a listing
//  2 an existing file whose stat() failed is found through open() + fstat()
//  3 open() refused: the walk up to the deepest parent that stats, its listing, the parent answers
//  4 kept as EIO: a listed name (also in another case), a listing that failed or ran past its limit, other errors
//  5 never walked: device nodes, relative and empty paths, the root; the walk is bounded
//  6 random trees: an existing path is never reported missing, a found path always exists
// Build and run from the repo root (llvm-mingw, as the other host tests):
//   clang++ -std=c++23 -O2 -static -Wall -Wextra -I rpcs3 -I . ps4-port/tests/fs_eio_test.cpp -o fs_eio_test.exe
//   fs_eio_test.exe
#include "util/ps4_disc_probe.hpp"

#include <cerrno>
#include <cstdio>
#include <map>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace
{
namespace se = utils::ps4::stat_eio;
using se::answer;
using se::step;

int g_checks = 0;
int g_failures = 0;

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

struct fake_stat
{
    long long size = -1;
    bool dir = false;
};

std::string parent_of(const std::string& p)
{
    const std::size_t slash = p.rfind('/');
    return slash == 0 ? std::string("/") : p.substr(0, slash);
}

// A case-sensitive tree with the console's stat() quirk and injectable answers
struct fake_fs
{
    struct node
    {
        bool dir = false;
        long long size = 0;
    };
    std::map<std::string, node> nodes{{"/", {true, 0}}};
    std::map<std::string, int> stat_errors; // forced stat() answer of a path
    std::map<std::string, int> open_errors; // forced open() answer of a path
    std::map<std::string, int> list_errors; // forced listing answer of a folder
    bool eio_under_missing = true;          // stat() of a path whose parent is missing answers EIO (the console)
    int opens = 0, stats = 0, lists = 0;
    std::size_t names_listed = 0;

    void add_dir(const std::string& p)
    {
        if (p != "/") add_dir(parent_of(p));
        nodes[p].dir = true;
    }
    void add_file(const std::string& p, long long size)
    {
        add_dir(parent_of(p));
        nodes[p] = {false, size};
    }
    bool exists(const std::string& p) const { return nodes.count(p) != 0; }

    // What a stat() of a path that does not exist answers
    int missing_answer(const std::string& p) const
    {
        const std::string parent = parent_of(p);
        const auto found = nodes.find(parent);
        if (found != nodes.end()) return found->second.dir ? ENOENT : ENOTDIR;
        return eio_under_missing ? EIO : ENOENT;
    }

    int stat_raw(const std::string& p, bool& is_dir) const
    {
        if (const auto forced = stat_errors.find(p); forced != stat_errors.end()) return forced->second;
        const auto found = nodes.find(p);
        if (found == nodes.end()) return missing_answer(p);
        is_dir = found->second.dir;
        return 0;
    }

    int open_stat(const std::string& p, fake_stat& out)
    {
        ++opens;
        if (const auto forced = open_errors.find(p); forced != open_errors.end()) return forced->second;
        if (const auto found = nodes.find(p); found != nodes.end())
        {
            out.size = found->second.size;
            out.dir = found->second.dir;
            return 0;
        }
        // ENOTDIR when a parent on the way is a file
        for (std::string up = parent_of(p);; up = parent_of(up))
        {
            if (const auto found = nodes.find(up); found != nodes.end() && !found->second.dir) return ENOTDIR;
            if (up == "/") break;
        }
        return ENOENT;
    }

    int stat_kind(const std::string& p, bool& is_dir)
    {
        ++stats;
        return stat_raw(p, is_dir);
    }

    template <typename Visit>
    int list(const std::string& dir, Visit&& visit)
    {
        ++lists;
        if (const auto forced = list_errors.find(dir); forced != list_errors.end()) return forced->second;
        if (visit(std::string_view(".")) || visit(std::string_view(".."))) return 0;
        names_listed += 2;
        const std::string prefix = dir == "/" ? std::string("/") : dir + "/";
        for (auto it = nodes.lower_bound(prefix); it != nodes.end() && it->first.starts_with(prefix); ++it)
        {
            const std::string_view rest = std::string_view(it->first).substr(prefix.size());
            if (rest.empty() || rest.find('/') != std::string_view::npos) continue;
            ++names_listed;
            if (visit(rest)) return 0;
        }
        return 0;
    }
};

se::result run(fake_fs& fs, std::string_view path, fake_stat& out)
{
    fs.opens = fs.stats = fs.lists = 0;
    fs.names_listed = 0;
    return se::resolve(path, fs, out);
}

se::result run(fake_fs& fs, std::string_view path)
{
    fake_stat out;
    return run(fs, path, out);
}

fake_fs game_tree()
{
    fake_fs fs;
    fs.add_dir("/data/rpcs3/dev_hdd0/game/BCES01741/USRDIR/DCDS");
    fs.add_file("/data/rpcs3/dev_hdd0/game/BCES01741/USRDIR/EBOOT.BIN", 12345678);
    fs.add_file("/data/rpcs3/dev_hdd0/game/BCES01741/USRDIR/data.psarc", 777);
    fs.add_file("/data/rpcs3/dev_hdd0/game/BCES01741/PARAM.SFO", 1024);
    fs.add_dir("/mnt/usb0/Games");
    return fs;
}

// ---------------------------------------------------------------------------------------------------------------
void test_console_case()
{
    fake_fs fs = game_tree();
    const std::string path = "/data/rpcs3/dev_hdd0/game/BCES01741/USRDIR/DCDS/patch/INTRO/movie.pam";
    // the quirk the resolver answers: stat() of this path says EIO
    bool is_dir = false;
    CHECK(fs.stat_raw(path, is_dir) == EIO);
    const se::result r = run(fs, path);
    CHECK(r.what == answer::missing && r.how == step::open && r.folder.empty());
    CHECK(fs.opens == 1 && fs.stats == 0 && fs.lists == 0);

    // a missing top folder of the cache (dev_hdd1/caches/<id>/fios)
    fs.add_dir("/data/rpcs3/dev_hdd1/caches");
    CHECK(run(fs, "/data/rpcs3/dev_hdd1/caches/BCES01741_BCES01741/fios").what == answer::missing);
    // the same with trailing slashes
    const se::result slashed = run(fs, "/data/rpcs3/dev_hdd1/caches/BCES01741_BCES01741/fios//");
    CHECK(slashed.what == answer::missing && fs.opens == 1);
    // open() says ENOTDIR for a path through a file
    const se::result through = run(fs, "/data/rpcs3/dev_hdd0/game/BCES01741/USRDIR/EBOOT.BIN/x");
    CHECK(through.what == answer::not_dir && through.how == step::open);
}

void test_found()
{
    fake_fs fs = game_tree();
    const std::string path = "/data/rpcs3/dev_hdd0/game/BCES01741/USRDIR/EBOOT.BIN";
    fs.stat_errors[path] = EIO; // a stat() that failed although the file is there
    fake_stat out;
    const se::result r = run(fs, path, out);
    CHECK(r.what == answer::found && r.how == step::open);
    CHECK(out.size == 12345678 && !out.dir);
    CHECK(fs.opens == 1 && fs.stats == 0 && fs.lists == 0);
    // a folder that opens is found as a folder
    fake_stat dir_out;
    CHECK(run(fs, "/data/rpcs3/dev_hdd0/game/BCES01741/USRDIR", dir_out).what == answer::found && dir_out.dir);
}

void test_walk()
{
    // open() refused (EIO): the parent that answers decides
    {
        fake_fs fs = game_tree();
        const std::string path = "/data/rpcs3/dev_hdd0/game/BCES01741/USRDIR/a/b/c.dat";
        fs.open_errors[path] = EIO;
        // stat("/.../USRDIR/a/b") -> EIO (its parent 'a' is missing), stat("/.../USRDIR/a") -> ENOENT
        const se::result r = run(fs, path);
        CHECK(r.what == answer::missing && r.how == step::parent);
        CHECK(r.folder == "/data/rpcs3/dev_hdd0/game/BCES01741/USRDIR/a");
        CHECK(fs.opens == 1 && fs.stats == 2 && fs.lists == 0);
    }
    // every missing level answers EIO (another kernel state): the listing of the deepest existing folder decides
    {
        fake_fs fs = game_tree();
        const std::string path = "/data/rpcs3/dev_hdd0/game/BCES01741/USRDIR/a/b/c.dat";
        fs.open_errors[path] = EIO;
        fs.stat_errors["/data/rpcs3/dev_hdd0/game/BCES01741/USRDIR/a"] = EIO;
        const se::result r = run(fs, path);
        CHECK(r.what == answer::missing && r.how == step::listing);
        CHECK(r.folder == "/data/rpcs3/dev_hdd0/game/BCES01741/USRDIR");
        CHECK(fs.opens == 1 && fs.stats == 3 && fs.lists == 1);
    }
    // the direct parent exists and does not list the name
    {
        fake_fs fs = game_tree();
        const std::string path = "/data/rpcs3/dev_hdd0/game/BCES01741/USRDIR/missing.bin";
        fs.open_errors[path] = EIO;
        const se::result r = run(fs, path);
        CHECK(r.what == answer::missing && r.how == step::listing && r.folder == "/data/rpcs3/dev_hdd0/game/BCES01741/USRDIR");
        CHECK(fs.stats == 1 && fs.lists == 1);
    }
    // a child of the root
    {
        fake_fs fs = game_tree();
        fs.open_errors["/nothing"] = EIO;
        const se::result r = run(fs, "/nothing");
        CHECK(r.what == answer::missing && r.how == step::listing && r.folder == "/");
    }
    // a parent that is a file
    {
        fake_fs fs = game_tree();
        const std::string path = "/data/rpcs3/dev_hdd0/game/BCES01741/PARAM.SFO/x";
        fs.open_errors[path] = EIO;
        const se::result r = run(fs, path);
        CHECK(r.what == answer::not_dir && r.how == step::parent && r.folder == "/data/rpcs3/dev_hdd0/game/BCES01741/PARAM.SFO");
        CHECK(fs.lists == 0);
    }
    // a parent that answers ENOTDIR itself
    {
        fake_fs fs = game_tree();
        const std::string path = "/data/rpcs3/x/y/z";
        fs.open_errors[path] = EIO;
        fs.stat_errors["/data/rpcs3/x/y"] = ENOTDIR;
        const se::result r = run(fs, path);
        CHECK(r.what == answer::not_dir && r.how == step::parent && r.folder == "/data/rpcs3/x/y");
    }
}

void test_kept()
{
    const std::string usrdir = "/data/rpcs3/dev_hdd0/game/BCES01741/USRDIR";
    // a real read error of a listed file
    {
        fake_fs fs = game_tree();
        const std::string path = usrdir + "/data.psarc";
        fs.stat_errors[path] = EIO;
        fs.open_errors[path] = EIO;
        const se::result r = run(fs, path);
        CHECK(r.what == answer::kept && r.how == step::listing && r.folder == usrdir);
    }
    // listed in another case (a case-insensitive drive): never reported missing
    {
        fake_fs fs = game_tree();
        const std::string path = usrdir + "/DATA.PSARC";
        fs.open_errors[path] = EIO;
        CHECK(run(fs, path).what == answer::kept);
        fs.open_errors[usrdir + "/eboot.bin"] = EIO;
        CHECK(run(fs, usrdir + "/eboot.bin").what == answer::kept);
    }
    // the listing failed
    {
        fake_fs fs = game_tree();
        const std::string path = usrdir + "/missing.bin";
        fs.open_errors[path] = EIO;
        for (const int error : {EACCES, EIO, ENOMEM, EBADF})
        {
            fs.list_errors[usrdir] = error;
            const se::result r = run(fs, path);
            CHECK(r.what == answer::kept && r.how == step::listing);
        }
    }
    // a listing longer than the limit without the name: not known
    {
        fake_fs fs;
        fs.add_dir("/data/big");
        for (std::size_t i = 0; i < se::max_listing + 10; ++i) fs.add_file("/data/big/f" + std::to_string(100000 + i), 1);
        fs.open_errors["/data/big/zzz"] = EIO;
        const se::result r = run(fs, "/data/big/zzz");
        CHECK(r.what == answer::kept && r.how == step::listing);
        CHECK(fs.names_listed <= se::max_listing + 2);
        // inside the limit a name that is absent is missing
        fake_fs small;
        small.add_dir("/data/small");
        for (int i = 0; i < 100; ++i) small.add_file("/data/small/f" + std::to_string(i), 1);
        small.open_errors["/data/small/zzz"] = EIO;
        CHECK(run(small, "/data/small/zzz").what == answer::missing);
        // a name found early stops the listing
        small.open_errors["/data/small/f0"] = EIO;
        CHECK(run(small, "/data/small/f0").what == answer::kept && small.names_listed < 10);
    }
    // a parent that answers another error
    {
        fake_fs fs = game_tree();
        const std::string path = "/data/rpcs3/q/r/s";
        fs.open_errors[path] = EIO;
        fs.stat_errors["/data/rpcs3/q/r"] = EACCES;
        const se::result r = run(fs, path);
        CHECK(r.what == answer::kept && r.how == step::parent && r.folder == "/data/rpcs3/q/r");
    }
    // open() answers something else than ENOENT / ENOTDIR and the name is listed (a folder open() refuses)
    {
        fake_fs fs = game_tree();
        fs.open_errors[usrdir + "/DCDS"] = EISDIR;
        fs.stat_errors[usrdir + "/DCDS"] = EIO;
        CHECK(run(fs, usrdir + "/DCDS").what == answer::kept);
    }
}

void test_not_walked()
{
    fake_fs fs = game_tree();
    for (const std::string_view path : {"/dev/cd0", "/dev", "/dev/", "/dev/da0x0.crypt", "", "relative/path", "/", "//"})
    {
        const se::result r = run(fs, path);
        CHECK(r.what == answer::kept && r.how == step::none);
        CHECK(fs.opens == 0 && fs.stats == 0 && fs.lists == 0);
    }
    // a name that only starts like the device folder is an ordinary path
    CHECK(run(fs, "/devices/x").what == answer::missing && fs.opens == 1);
    // every parent answers EIO, the root too: kept, and the walk stops at the root
    {
        fake_fs eio;
        eio.add_dir("/a");
        const std::string path = "/a/b/c/d";
        eio.open_errors[path] = EIO;
        for (const char* p : {"/a/b/c", "/a/b", "/a", "/"}) eio.stat_errors[p] = EIO;
        const se::result r = run(eio, path);
        CHECK(r.what == answer::kept && r.how == step::parent && r.folder == "/");
        CHECK(eio.stats == 4 && eio.lists == 0);
    }
    // a very deep path: at most max_depth parents are asked
    {
        fake_fs deep;
        std::string path;
        for (int i = 0; i < 200; ++i) path += "/d" + std::to_string(i);
        deep.open_errors[path] = EIO;
        for (std::string up = parent_of(path);; up = parent_of(up))
        {
            deep.stat_errors[up] = EIO;
            if (up == "/") break;
        }
        const se::result r = run(deep, path);
        CHECK(r.what == answer::kept && r.how == step::none);
        CHECK(deep.stats == static_cast<int>(se::max_depth) && deep.lists == 0 && deep.opens == 1);
    }
    // name comparison
    static_assert(se::same_name("PS3_GAME", "ps3_game") && se::same_name("", ""));
    static_assert(!se::same_name("PS3_GAME", "PS3_GAM") && !se::same_name("a", "b"));
    CHECK(se::same_name("\xC3\xA9t\xC3\xA9", "\xC3\xA9T\xC3\xA9") && !se::same_name("\xC3\xA9", "\xC3\x89"));
}

// ---------------------------------------------------------------------------------------------------------------
std::string lower(std::string s)
{
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

void test_random_trees()
{
    std::mt19937 rng(0xe10);
    const std::vector<std::string> names = {"A", "b", "Usrdir", "USRDIR", "data", "x.bin", "X.BIN", "movie.pam", "c"};
    std::uniform_int_distribution<std::size_t> pick(0, names.size() - 1);
    std::uniform_int_distribution<int> depth(1, 6), coin(0, 3), errors(0, 5);
    int found = 0, missing = 0, kept = 0, not_dir = 0, missing_by_listing = 0, missing_case_variant = 0;
    for (int round = 0; round < 3000; ++round)
    {
        fake_fs fs;
        fs.eio_under_missing = coin(rng) != 0;
        std::vector<std::string> made;
        for (int n = 0; n < 12; ++n)
        {
            std::string p;
            const int d = depth(rng);
            for (int k = 0; k < d; ++k) p += "/" + names[pick(rng)];
            // a file in the middle of an existing folder path would need a different tree: skip such paths
            bool through_file = false;
            for (std::string up = parent_of(p); up != "/"; up = parent_of(up))
                if (fs.exists(up) && !fs.nodes[up].dir) through_file = true;
            if (through_file || (fs.exists(p) && fs.nodes[p].dir)) continue;
            if (coin(rng)) fs.add_file(p, static_cast<long long>(p.size()));
            else fs.add_dir(p);
            made.push_back(p);
        }
        std::set<std::string> lowered;
        for (const auto& [p, node] : fs.nodes) lowered.insert(lower(p));
        for (int q = 0; q < 20; ++q)
        {
            std::string path;
            const int d = depth(rng);
            for (int k = 0; k < d; ++k) path += "/" + names[pick(rng)];
            // random injected answers on the way
            if (errors(rng) == 0) fs.open_errors[path] = EIO;
            if (errors(rng) == 0) fs.stat_errors[parent_of(path)] = EIO;
            if (errors(rng) == 0) fs.list_errors[parent_of(path)] = EIO;
            fake_stat out;
            const se::result r = run(fs, path, out);
            const bool exists = fs.exists(path);
            const bool exists_any_case = lowered.count(lower(path)) != 0;
            switch (r.what)
            {
            case answer::found:
                ++found;
                CHECK(exists && out.dir == fs.nodes[path].dir);
                break;
            case answer::missing:
                ++missing;
                // never an existing path; a listing never misses a name that is there in another case
                CHECK(!exists);
                if (r.how == step::listing)
                {
                    const std::string prefix = r.folder == "/" ? std::string("/") : r.folder + "/";
                    CHECK(path.starts_with(prefix));
                    const std::string next = path.substr(prefix.size(), path.find('/', prefix.size()) - prefix.size());
                    bool listed_any_case = false;
                    for (const auto& [p, node] : fs.nodes)
                        listed_any_case |= p.size() > prefix.size() && p.starts_with(prefix) && p.find('/', prefix.size()) == std::string::npos &&
                            lower(p.substr(prefix.size())) == lower(next);
                    CHECK(!listed_any_case);
                    ++missing_by_listing;
                }
                if (exists_any_case) ++missing_case_variant;
                break;
            case answer::not_dir:
                ++not_dir;
                CHECK(!exists);
                break;
            case answer::kept: ++kept; break;
            }
            CHECK(fs.opens <= 1 && fs.lists <= 1 && fs.stats <= static_cast<int>(se::max_depth));
            fs.open_errors.clear();
            fs.stat_errors.clear();
            fs.list_errors.clear();
        }
    }
    // every answer was reached (a case variant is missing on this case-sensitive tree when open() or a parent says so)
    CHECK(found > 0 && missing > 0 && kept > 0 && not_dir > 0 && missing_by_listing > 0 && missing_case_variant > 0);
    std::printf("random trees: found %d, missing %d (%d by a listing, %d case variants), not a folder %d, kept %d\n", found, missing,
        missing_by_listing, missing_case_variant, not_dir, kept);
}
} // namespace

int main()
{
    test_console_case();
    test_found();
    test_walk();
    test_kept();
    test_not_walked();
    test_random_trees();
    std::printf("fs_eio_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
