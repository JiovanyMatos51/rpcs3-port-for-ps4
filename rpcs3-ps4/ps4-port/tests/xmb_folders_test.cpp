// Host test of the XMB folders model (rpcs3/util/ps4_xmb_folders.hpp): file round trip, tolerated input, the load
// decision (main / backup / damaged / newer version), parser rules and limits, unknown data kept, portable paths,
// matching with the library (adoption by TITLE_ID), the projected 'Jogo' column, the operations, name cleaning, an
// outside edit before a save, and a recording I/O proving that only the three folder files are ever written or
// renamed (no game file is touched). Ends with random mutations of a folder file (parser robustness and round trip).
// Build (from the repo root):
//   clang++ -std=c++23 -O1 -static -Wall -Wextra -I rpcs3 -I . ps4-port/tests/xmb_folders_test.cpp -o xmb_folders_test.exe
#include "../../rpcs3/util/ps4_xmb_folders.hpp"

#include <cstdio>
#include <functional>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

namespace
{
namespace xf = utils::ps4::xmb_folders;

int failures = 0;
int checks = 0;
#define CHECK(condition) \
    do \
    { \
        ++checks; \
        if (!(condition)) \
        { \
            std::fprintf(stderr, "FAIL at line %d: %s\n", __LINE__, #condition); \
            ++failures; \
        } \
    } while (false)

std::string comment_line()
{
    return std::string("# written by RPCS3 PORT FOR PS4 ") + ps4_build_version + "; edit with care (tab-separated)\n";
}

// The example file of the format description, in canonical form.
std::string canonical()
{
    return std::string("RPCS3 XMB folders v1\n") + comment_line() +
        "folder\t1\tCorrida\n"
        "game\t1\tBLUS30066\t/data/rpcs3/games/Burnout Paradise.iso\tBurnout Paradise\n"
        "game\t1\tBCUS98114\tusb:/PS3ISO/Gran Turismo 5.iso\tGran Turismo 5\n"
        "folder\t2\tLuta\n"
        "game\t2\t\t/data/rpcs3/pkgs/Tekken 6.pkg\tTekken 6 (pacote)\n"
        "folder\t3\tFavoritos\n";
}

xf::model canonical_model()
{
    xf::model m;
    m.folders = {
        {1, "Corrida", "", {{"BLUS30066", "/data/rpcs3/games/Burnout Paradise.iso", "Burnout Paradise", ""},
                               {"BCUS98114", "usb:/PS3ISO/Gran Turismo 5.iso", "Gran Turismo 5", ""}}},
        {2, "Luta", "", {{"", "/data/rpcs3/pkgs/Tekken 6.pkg", "Tekken 6 (pacote)", ""}}},
        {3, "Favoritos", "", {}},
    };
    return m;
}

xf::row game(std::string path, std::string serial, std::string title, bool folder_kind = false)
{
    return {std::move(path), std::move(serial), std::move(title), true, folder_kind};
}

xf::row utility(std::string path, std::string title)
{
    return {std::move(path), "", std::move(title), false, false};
}

// The 'Jogo' column as the library builds it: two utility rows, the games, a USB note.
std::vector<xf::row> library()
{
    return {
        utility("@savedata", "Utilitario de dados guardados"),
        utility("@licenses", "Importar licencas"),
        game("/data/rpcs3/games/Burnout Paradise.iso", "BLUS30066", "Burnout Paradise"),
        game("/data/rpcs3/games/Demon's Souls.iso", "BLUS30443", "Demon's Souls"),
        game("/mnt/usb1/PS3ISO/Gran Turismo 5.iso", "BCUS98114", "Gran Turismo 5"),
        game("/data/rpcs3/games/Folder Game", "BLES00001", "Folder Game", true),
        utility("@usb:empty:0", "USB 1: nenhum jogo"),
    };
}

std::string repeat(std::string_view piece, std::size_t count)
{
    std::string out;
    for (std::size_t i = 0; i < count; ++i) out += piece;
    return out;
}

// In-memory files with modification times; records every path read, stat'ed, written or renamed.
struct fake_io
{
    std::map<std::string, std::string, std::less<>> files;
    std::map<std::string, std::int64_t, std::less<>> mtimes;
    std::set<std::string, std::less<>> unreadable, fail_write, fail_rename;
    std::int64_t clock = 100;
    std::vector<std::string> reads, stats, writes, renames;
    std::set<std::string, std::less<>> touched; // every path written or renamed (both names of a rename)

    xf::stamp stamp_of(std::string_view path) const
    {
        const auto it = files.find(path);
        if (it == files.end()) return {};
        return {true, it->second.size(), mtimes.find(path)->second};
    }

    xf::file_read read(std::string_view path, std::size_t limit)
    {
        reads.emplace_back(path);
        xf::file_read out;
        const auto it = files.find(path);
        if (it == files.end()) return out;
        out.at = stamp_of(path);
        if (unreadable.contains(std::string(path)) || it->second.size() > limit)
        {
            out.status = xf::read_status::unreadable;
            return out;
        }
        out.status = xf::read_status::ok;
        out.text = it->second;
        return out;
    }

    xf::stamp stat(std::string_view path)
    {
        stats.emplace_back(path);
        return stamp_of(path);
    }

    bool write(std::string_view path, std::string_view text)
    {
        writes.emplace_back(path);
        touched.insert(std::string(path));
        if (fail_write.contains(std::string(path))) return false;
        put(path, text);
        return true;
    }

    bool rename(std::string_view from, std::string_view to)
    {
        renames.push_back(std::string(from) + " -> " + std::string(to));
        touched.insert(std::string(from));
        touched.insert(std::string(to));
        if (fail_rename.contains(std::string(from))) return false;
        const auto it = files.find(from);
        if (it == files.end()) return false;
        const std::string text = it->second;
        const std::int64_t mtime = mtimes.find(from)->second;
        files.erase(it);
        mtimes.erase(mtimes.find(from));
        files[std::string(to)] = text;
        mtimes[std::string(to)] = mtime; // a rename keeps the modification time
        return true;
    }

    // A file created or changed by somebody else (FTP): not recorded.
    void put(std::string_view path, std::string_view text)
    {
        files[std::string(path)] = std::string(text);
        mtimes[std::string(path)] = ++clock;
    }

    std::string text(std::string_view path) const
    {
        const auto it = files.find(path);
        return it == files.end() ? std::string() : it->second;
    }

    bool has(std::string_view path) const { return files.find(path) != files.end(); }

    void clear_records()
    {
        reads.clear();
        stats.clear();
        writes.clear();
        renames.clear();
    }
};

// Nothing but the folder files was read, stat'ed, written or renamed.
bool only_folder_files(const fake_io& io)
{
    const std::set<std::string> allowed{xf::file_path, xf::backup_path, xf::damaged_path};
    for (const auto& path : io.touched)
        if (!allowed.contains(path)) return false;
    for (const auto& path : io.reads)
        if (path != xf::file_path && path != xf::backup_path) return false;
    for (const auto& path : io.stats)
        if (path != xf::file_path) return false;
    return true;
}

// Game files a library holds: they must come out of every folder operation byte-identical.
void put_game_files(fake_io& io)
{
    io.put("/data/rpcs3/games/Burnout Paradise.iso", "iso bytes");
    io.put("/data/rpcs3/games/Demon's Souls.iso", "iso bytes 2");
    io.put("/mnt/usb1/PS3ISO/Gran Turismo 5.iso", "usb iso bytes");
    io.put("/data/rpcs3/games/Folder Game/PS3_GAME/USRDIR/EBOOT.BIN", "eboot");
}

std::map<std::string, std::string, std::less<>> game_files(const fake_io& io)
{
    std::map<std::string, std::string, std::less<>> out;
    for (const auto& [path, text] : io.files)
        if (!path.starts_with("/data/rpcs3/xmb-folders")) out[path] = text;
    return out;
}

bool is_folder_text(std::string_view text, const xf::model& m)
{
    const xf::parsed p = xf::parse(text);
    return p.status == xf::load_status::ok && p.dropped == 0 && p.m == m;
}

auto always = [](std::string_view) { return true; };

// canonical text round trip, model round trip.
void test_round_trip()
{
    const xf::parsed p = xf::parse(canonical());
    CHECK(p.status == xf::load_status::ok);
    CHECK(p.dropped == 0 && p.first_problem.empty() && !p.from_backup && !p.m.read_only);
    CHECK(p.m == canonical_model());
    CHECK(xf::serialize(p.m) == canonical());

    xf::model m = canonical_model();
    m.folders.push_back({42, "سباق \xE2\x80\x8E" "2", "\tcolor=blue", {{"BLES01234", "usb:/GAMES/Jogo Á", "Título ação", "\tplayed=3\tx"}}});
    m.unknown_lines = {"style\t42\tblue", "a later line"};
    const std::string text = xf::serialize(m);
    const xf::parsed back = xf::parse(text);
    CHECK(back.status == xf::load_status::ok && back.dropped == 0);
    CHECK(back.m == m);
    CHECK(xf::serialize(back.m) == text);
    CHECK(text.starts_with("RPCS3 XMB folders v1\n# written by RPCS3 PORT FOR PS4 "));
    CHECK(text.find('\r') == std::string::npos && !text.starts_with("\xEF\xBB\xBF"));
    CHECK(xf::serialize(xf::model{}) == std::string("RPCS3 XMB folders v1\n") + comment_line());
}

// CRLF, BOM, comment and empty lines, a game line before its folder line.
void test_tolerated_input()
{
    std::string crlf;
    for (const char c : canonical())
    {
        if (c == '\n') crlf += '\r';
        crlf += c;
    }
    const std::string variants[] = {
        crlf,
        "\xEF\xBB\xBF" + canonical(),
        "\xEF\xBB\xBF" + crlf,
        std::string("RPCS3 XMB folders v1\n") +
            "# a comment\n"
            "\n"
            "game\t1\tBLUS30066\t/data/rpcs3/games/Burnout Paradise.iso\tBurnout Paradise\n"
            "folder\t1\tCorrida\n"
            "\r\n"
            "game\t2\t\t/data/rpcs3/pkgs/Tekken 6.pkg\tTekken 6 (pacote)\n"
            "#game\t1\tBLUS99999\t/data/x.iso\tcommented out\n"
            "game\t1\tBCUS98114\tusb:/PS3ISO/Gran Turismo 5.iso\tGran Turismo 5\n"
            "folder\t2\tLuta\n"
            "folder\t3\tFavoritos",
        // A non-portable USB path written by hand is stored in portable form.
        std::string("RPCS3 XMB folders v1\n") +
            "folder\t1\tCorrida\n"
            "game\t1\tBLUS30066\t/data/rpcs3/games/Burnout Paradise.iso\tBurnout Paradise\n"
            "game\t1\tBCUS98114\t/mnt/usb5/PS3ISO/Gran Turismo 5.iso\tGran Turismo 5\n"
            "folder\t2\tLuta\n"
            "game\t2\t\t/data/rpcs3/pkgs/Tekken 6.pkg\tTekken 6 (pacote)\n"
            "folder\t003\tFavoritos\n",
    };
    for (const auto& text : variants)
    {
        const xf::parsed p = xf::parse(text);
        CHECK(p.status == xf::load_status::ok);
        CHECK(p.dropped == 0);
        CHECK(p.m == canonical_model());
        CHECK(xf::serialize(p.m) == canonical());
    }
}

// which file the folders come from.
void test_choose()
{
    const std::string good = canonical();
    const std::string other = "RPCS3 XMB folders v1\nfolder\t9\tBackup\n";
    xf::parsed p = xf::choose(good, other);
    CHECK(p.status == xf::load_status::ok && !p.from_backup && p.m == canonical_model());

    p = xf::choose(std::string_view("not a folder file\nfolder\t1\tX\n"), std::string_view(other));
    CHECK(p.status == xf::load_status::damaged && p.from_backup);
    CHECK(p.m.folders.size() == 1 && p.m.folders[0].name == "Backup" && !p.m.read_only);
    CHECK(p.first_problem == "line 1: not a folder file header");

    p = xf::choose(std::string_view(""), std::string_view(other)); // exists, could not be read
    CHECK(p.status == xf::load_status::damaged && p.from_backup && p.m.folders.size() == 1);

    p = xf::choose(std::string_view("garbage"), std::string_view("more garbage"));
    CHECK(p.status == xf::load_status::damaged && !p.from_backup && p.m == xf::model{});
    p = xf::choose(std::string_view("garbage"), std::nullopt);
    CHECK(p.status == xf::load_status::damaged && !p.from_backup && p.m == xf::model{});

    // A missing main file: the user never created a folder or removed the file on purpose. The backup is not used.
    p = xf::choose(std::nullopt, std::string_view(other));
    CHECK(p.status == xf::load_status::absent && !p.from_backup && p.m == xf::model{});
    CHECK(xf::choose(std::nullopt, std::nullopt).status == xf::load_status::absent);

    // A newer main file wins over the backup (read only); a newer backup of a damaged main stays read only.
    p = xf::choose(std::string_view("RPCS3 XMB folders v2\nfolder\t5\tNova\n"), std::string_view(other));
    CHECK(p.status == xf::load_status::newer && p.m.read_only && !p.from_backup && p.m.folders.size() == 1 && p.m.folders[0].id == 5);
    p = xf::choose(std::string_view("garbage"), std::string_view("RPCS3 XMB folders v3\nfolder\t5\tNova\n"));
    CHECK(p.status == xf::load_status::damaged && p.from_backup && p.m.read_only);

    // The store reads the backup only when the main file exists and is damaged.
    fake_io io;
    xf::store s;
    s.load(io);
    CHECK(s.status == xf::load_status::absent && s.m == xf::model{});
    CHECK(io.reads == std::vector<std::string>{xf::file_path} && io.writes.empty() && io.renames.empty());

    io.put(xf::backup_path, other);
    io.clear_records();
    s.load(io);
    CHECK(s.status == xf::load_status::absent && s.m == xf::model{});
    CHECK(io.reads == std::vector<std::string>{xf::file_path});

    io.put(xf::file_path, good);
    io.clear_records();
    s.load(io);
    CHECK(s.status == xf::load_status::ok && s.m == canonical_model() && io.reads.size() == 1);

    io.put(xf::file_path, "damaged");
    io.clear_records();
    s.load(io);
    CHECK(s.status == xf::load_status::damaged && s.from_backup && s.m.folders.size() == 1);
    CHECK((io.reads == std::vector<std::string>{xf::file_path, xf::backup_path}));

    io.put(xf::file_path, good);
    io.unreadable.insert(xf::file_path);
    s.load(io);
    CHECK(s.status == xf::load_status::damaged && s.from_backup && s.first_problem == "the file could not be read");
    io.unreadable.clear();

    // Larger than 1 MiB: not read (unreadable), the backup is shown.
    io.put(xf::file_path, good + repeat("#\n", (xf::max_file_bytes / 2) + 1));
    s.load(io);
    CHECK(s.status == xf::load_status::damaged && s.from_backup);
    CHECK(xf::parse(io.text(xf::file_path)).first_problem == "file larger than 1 MiB");
    CHECK(io.writes.empty() && io.renames.empty());
}

// a file of a newer version is read only: shown, never written, every operation refused.
void test_newer_version()
{
    const std::string v2 = "RPCS3 XMB folders v2\nfolder\t1\tCorrida\ngame\t1\tBLUS30066\t/data/rpcs3/games/Burnout Paradise.iso\tBurnout Paradise\n"
                           "group\t1\t7\nfolder\t2\tLuta\tnew field\n";
    const xf::parsed p = xf::parse(v2);
    CHECK(p.status == xf::load_status::newer && p.m.read_only);
    CHECK(p.m.folders.size() == 2 && p.m.folders[0].members.size() == 1);
    CHECK(xf::parse("RPCS3 XMB folders v10\n").status == xf::load_status::newer);
    CHECK(xf::parse("RPCS3 XMB folders v01\n").status == xf::load_status::ok);
    for (const char* bad : {"RPCS3 XMB folders v0\n", "RPCS3 XMB folders v1x\n", "RPCS3 XMB folders\n", "RPCS3 XMB folders v\n",
             "RPCS3 XMB folders v1234567\n", "rpcs3 xmb folders v1\n", " RPCS3 XMB folders v1\n", "\n"})
        CHECK(xf::parse(bad).status == xf::load_status::damaged);

    xf::model m = p.m;
    const xf::model before = m;
    const auto rows = library();
    xf::refusal why = xf::refusal::none;
    std::uint32_t id = 0;
    CHECK(!xf::create(m, "Nova", id, &why) && why == xf::refusal::read_only);
    CHECK(!xf::rename(m, 1, "Outra", &why) && why == xf::refusal::read_only);
    CHECK(xf::remove_folder(m, 1, &why) == 0 && why == xf::refusal::read_only);
    CHECK(!xf::move_to(m, rows[3], 1u, &why) && why == xf::refusal::read_only);
    CHECK(!xf::move_to(m, rows[2], std::nullopt, &why) && why == xf::refusal::read_only);
    CHECK(!xf::set_members(m, 1, std::span(rows).subspan(3, 1), rows, &why) && why == xf::refusal::read_only);
    CHECK(!xf::move_folder(m, 0, 1, &why) && why == xf::refusal::read_only);
    CHECK(!xf::move_member(m, 1, 0, 1, &why) && why == xf::refusal::read_only);
    const xf::assignment a = xf::assign(m, rows, always);
    CHECK(xf::forget_missing(m, 1, a, &why) == 0 && why == xf::refusal::read_only);
    CHECK(m == before);

    fake_io io;
    io.put(xf::file_path, v2);
    xf::store s;
    s.load(io);
    CHECK(s.status == xf::load_status::newer && s.m.read_only);
    io.clear_records();
    const xf::save_result r = s.apply(io, [&](xf::model& x, xf::refusal& w) { return xf::create(x, "Nova", id, &w); });
    CHECK(r.status == xf::save_status::refused && r.why == xf::refusal::read_only);
    CHECK(io.reads.empty() && io.stats.empty() && io.writes.empty() && io.renames.empty());
    CHECK(io.text(xf::file_path) == v2 && !io.has(xf::backup_path));
}

// lines that break a rule are left out and counted; the first problem by line number is reported.
void test_bad_lines()
{
    const std::string name32 = repeat("n", 32);
    const std::string arabic32 = repeat("\xD8\xB3", 32); // 32 code points, 64 bytes
    const std::string text = std::string("RPCS3 XMB folders v1\n") +
        "folder\t0\tZero\n"                                   // 2: id 0
        "folder\tabc\tLetters\n"                              // 3
        "folder\t1234567\tBig\n"                              // 4
        "folder\t\tNo id\n"                                   // 5
        "folder\t-5\tMinus\n"                                 // 6
        "folder\t5\tOk\n"                                     // 7: kept
        "folder\t5\tSame id\n"                                // 8: id used twice
        "folder\t6\n"                                         // 9: no name
        "folder\t7\t" + name32 + "x\n" +                      // 10: 33 code points
        "folder\t8\tA\xFF" "B\n"                              // 11: invalid UTF-8
        "folder\t9\t\t\n"                                     // 12: empty name (tabs)
        "folder\t10\t  \x01 \n"                               // 13: nothing left after cleaning
        "game\t99\t\t/data/x.iso\tX\n"                        // 14: unknown folder id
        "game\tx\t\t/data/y.iso\tY\n"                         // 15: folder id not valid
        "game\t5\t\trelative/path.iso\tZ\n"                   // 16: path not absolute
        "game\t5\t\t/data/ok.iso\tOk\n"                       // 17: kept
        "game\t5\n"                                           // 18: no path
        "game\t5\t\t" + repeat("/", 2048) + "\tlong\n" +      // 19: longer than 2048 bytes
        "folder\t11\t" + name32 + "\n" +                      // 20: kept (32 code points)
        "folder\t12\t" + arabic32 + "\n" +                    // 21: kept
        "game\t5\tbadserial\t/data/s.iso\tS\n"                // 22: kept, serial read as empty
        "game\t5\tBLUS12345\t/data/ok.iso\tOk again\n"        // 23: the same game twice
        "game\t11\tBLUS12345\t/data/ok.iso\tOk elsewhere\n"   // 24: the same game in another folder
        "game\t11\tblus12345\t" + std::string("/data/") + repeat("p", 1019) + "\tT\n" + // 25: path of 1025 bytes
        "game\t11\t\t/data/a\tb\x01" "c\n"                    // 26: kept, the control left out of the title
        "folder\t13\tComma\tand\textra\n";                    // 27: kept, extra fields
    const xf::parsed p = xf::parse(text);
    CHECK(p.status == xf::load_status::ok);
    CHECK(p.first_problem == "line 2: folder id not valid");
    CHECK(p.dropped == 19);
    CHECK(p.m.folders.size() == 4);
    if (p.m.folders.size() == 4)
    {
        CHECK(p.m.folders[0].id == 5 && p.m.folders[0].name == "Ok");
        CHECK(p.m.folders[1].id == 11 && p.m.folders[1].name == name32);
        CHECK(p.m.folders[2].id == 12 && p.m.folders[2].name == arabic32);
        CHECK(p.m.folders[3].id == 13 && p.m.folders[3].name == "Comma" && p.m.folders[3].extra == "\tand\textra");
        const auto& ok = p.m.folders[0].members;
        CHECK(ok.size() == 2);
        if (ok.size() == 2)
        {
            CHECK(ok[0].path == "/data/ok.iso" && ok[0].title == "Ok" && ok[0].serial.empty());
            CHECK(ok[1].path == "/data/s.iso" && ok[1].serial.empty());
        }
        CHECK(p.m.folders[1].members.size() == 1 && p.m.folders[1].members[0].path == "/data/a" && p.m.folders[1].members[0].title == "bc");
    }
    // The problem of the lowest line wins even when a later pass finds it.
    const xf::parsed q = xf::parse("RPCS3 XMB folders v1\ngame\t4\t\t/data/x\tX\nfolder\t1\tA\nfolder\t1\tB\n");
    CHECK(q.dropped == 2 && q.first_problem == "line 2: game of an unknown folder id");
    // A file of only comments is a folder file without folders.
    const xf::parsed r = xf::parse("RPCS3 XMB folders v1\n# nothing\n");
    CHECK(r.status == xf::load_status::ok && r.m == xf::model{} && r.dropped == 0);
}

// limits of the file and of the operations.
void test_limits()
{
    std::string text = "RPCS3 XMB folders v1\n";
    for (int i = 1; i <= 201; ++i) text += "folder\t" + std::to_string(i) + "\tF" + std::to_string(i) + "\n";
    xf::parsed p = xf::parse(text);
    CHECK(p.m.folders.size() == xf::max_folders && p.dropped == 1 && p.first_problem == "line 202: more than 200 folders");

    text = "RPCS3 XMB folders v1\nfolder\t1\tA\nfolder\t2\tB\n";
    for (int i = 0; i < 5001; ++i) text += "game\t" + std::string(i < 2500 ? "1" : "2") + "\t\t/data/g" + std::to_string(i) + ".iso\tG\n";
    p = xf::parse(text);
    CHECK(xf::member_count(p.m) == xf::max_members && p.dropped == 1 && p.first_problem == "line 5004: more than 5000 games");

    p = xf::parse("RPCS3 XMB folders v1\n" + repeat("#\n", xf::max_file_bytes / 2));
    CHECK(p.status == xf::load_status::damaged && p.first_problem == "file larger than 1 MiB");
    CHECK(xf::parse(("RPCS3 XMB folders v1\n" + repeat("#\n", xf::max_file_bytes / 2)).substr(0, xf::max_file_bytes)).status == xf::load_status::ok);

    // Operations refuse to pass the limits.
    xf::model m;
    xf::refusal why = xf::refusal::none;
    std::uint32_t id = 0;
    for (std::size_t i = 0; i < xf::max_folders; ++i) CHECK(xf::create(m, "Pasta", id, &why));
    CHECK(m.folders.size() == xf::max_folders && m.folders.back().name == "Pasta (200)");
    const xf::model full = m;
    CHECK(!xf::create(m, "Mais", id, &why) && why == xf::refusal::folder_limit && m == full);

    xf::model many;
    many.folders.push_back({1, "A", "", {}});
    for (std::size_t i = 0; i < xf::max_members; ++i) many.folders[0].members.push_back({"", "/data/g" + std::to_string(i) + ".iso", "G", ""});
    const xf::model many_before = many;
    CHECK(!xf::move_to(many, game("/data/new.iso", "", "New"), 1u, &why) && why == xf::refusal::member_limit && many == many_before);
    CHECK(xf::move_to(many, game("/data/g7.iso", "", "G"), std::nullopt, &why) && xf::member_count(many) == xf::max_members - 1);
    many.folders.push_back({2, "B", "", {}});
    CHECK(xf::move_to(many, game("/data/g8.iso", "", "G"), 2u, &why) && xf::member_count(many) == xf::max_members - 1);
    CHECK(xf::move_to(many, game("/data/n1.iso", "", "N"), 2u, &why) && xf::member_count(many) == xf::max_members);
    const xf::model many_full = many;
    const std::vector<xf::row> two{game("/data/n2.iso", "", "N"), game("/data/n3.iso", "", "N")};
    CHECK(!xf::set_members(many, 2, two, two, &why) && why == xf::refusal::member_limit && many == many_full);

    // A change whose file would pass 1 MiB is refused before anything is written.
    fake_io io;
    xf::store s;
    s.load(io);
    s.m.folders.push_back({1, "Longos", "", {}});
    for (std::size_t i = 0; i < 1100; ++i)
        s.m.folders[0].members.push_back({"", "/data/rpcs3/games/" + std::to_string(i) + repeat("x", 990) + ".iso", "T", ""});
    const xf::model big = s.m;
    io.clear_records();
    const xf::save_result r = s.apply(io, [&](xf::model& x, xf::refusal& w) { return xf::create(x, "Mais uma", id, &w); });
    CHECK(r.status == xf::save_status::refused && r.why == xf::refusal::file_limit);
    CHECK(io.writes.empty() && io.renames.empty() && s.m == big);
}

// unknown line kinds and trailing fields survive a rewrite.
void test_unknown_data_kept()
{
    const std::string text = std::string("RPCS3 XMB folders v1\n") +
        "style\t1\tblue\n"
        "folder\t1\tA\tcolor=red\tx\n"
        "game\t1\tBLUS30001\t/data/a.iso\tA game\tplayed=3\n"
        "game\t1\tBLUS30002\t/data/b.iso\tB game\t\n"
        "future line without tabs\n"
        "game2\t1\tlike a game line\n";
    const xf::parsed p = xf::parse(text);
    CHECK(p.status == xf::load_status::ok && p.dropped == 0);
    CHECK((p.m.unknown_lines == std::vector<std::string>{"style\t1\tblue", "future line without tabs", "game2\t1\tlike a game line"}));
    CHECK(p.m.folders.size() == 1 && p.m.folders[0].extra == "\tcolor=red\tx");
    CHECK(p.m.folders[0].members.size() == 2 && p.m.folders[0].members[0].extra == "\tplayed=3" && p.m.folders[0].members[1].extra == "\t");
    const std::string out = xf::serialize(p.m);
    for (const char* line : {"\nstyle\t1\tblue\n", "\nfolder\t1\tA\tcolor=red\tx\n", "\ngame\t1\tBLUS30001\t/data/a.iso\tA game\tplayed=3\n",
             "\ngame\t1\tBLUS30002\t/data/b.iso\tB game\t\n", "\nfuture line without tabs\n", "\ngame2\t1\tlike a game line\n"})
        CHECK(out.find(line) != std::string::npos);
    CHECK(xf::parse(out).m == p.m);

    // An operation through the store keeps them, and a member moved to another folder keeps its own fields.
    fake_io io;
    io.put(xf::file_path, text);
    xf::store s;
    s.load(io);
    std::uint32_t id = 0;
    CHECK(s.apply(io, [&](xf::model& x, xf::refusal& w) { return xf::create(x, "B", id, &w); }).status == xf::save_status::saved);
    CHECK(s.apply(io, [&](xf::model& x, xf::refusal& w) { return xf::move_to(x, game("/data/a.iso", "BLUS30001", "A game"), id, &w); }).status ==
        xf::save_status::saved);
    const std::string saved = io.text(xf::file_path);
    CHECK(saved.find("\nstyle\t1\tblue\n") != std::string::npos && saved.find("\nfuture line without tabs\n") != std::string::npos);
    CHECK(saved.find("\nfolder\t1\tA\tcolor=red\tx\n") != std::string::npos);
    CHECK(saved.find("\ngame\t" + std::to_string(id) + "\tBLUS30001\t/data/a.iso\tA game\tplayed=3\n") != std::string::npos);
    CHECK(io.text(xf::backup_path).find("\nfolder\t" + std::to_string(id) + "\tB\n") != std::string::npos); // the version after the create
}

// portable paths.
void test_portable_path()
{
    for (unsigned n = 0; n < 8; ++n)
    {
        const std::string mount = "/mnt/usb" + std::to_string(n);
        CHECK(xf::portable_path(mount + "/PS3ISO/Game.iso") == "usb:/PS3ISO/Game.iso");
        CHECK(xf::drive_path("usb:/PS3ISO/Game.iso", n) == mount + "/PS3ISO/Game.iso");
    }
    CHECK(xf::portable_path("/mnt/usb8/PS3ISO/Game.iso") == "/mnt/usb8/PS3ISO/Game.iso");
    CHECK(xf::portable_path("/mnt/usb10/PS3ISO/Game.iso") == "/mnt/usb10/PS3ISO/Game.iso");
    CHECK(xf::portable_path("/mnt/usb0x/Game.iso") == "/mnt/usb0x/Game.iso");
    CHECK(xf::portable_path("/data/rpcs3/games/Game.iso") == "/data/rpcs3/games/Game.iso");
    CHECK(xf::portable_path("/mnt/usb3") == "usb:/" && xf::portable_path("/mnt/usb3/") == "usb:/");
    CHECK(xf::portable_path("usb:/x") == "usb:/x");
    CHECK(xf::drive_path("/data/rpcs3/games/Game.iso", 2) == "/data/rpcs3/games/Game.iso");
    CHECK(xf::on_usb("usb:/a") && !xf::on_usb("/mnt/usb0/a") && !xf::on_usb("usb:a"));
    CHECK(xf::valid_member_path("usb:/a") && xf::valid_member_path("/data/a") && xf::valid_member_path("/" + repeat("a", 1023)));
    CHECK(!xf::valid_member_path("usb:/") && !xf::valid_member_path("") && !xf::valid_member_path("relative") && !xf::valid_member_path("@savedata"));
    CHECK(!xf::valid_member_path("/" + repeat("a", 1024)) && !xf::valid_member_path("/a\tb") && !xf::valid_member_path("/a\nb") &&
        !xf::valid_member_path("/a\rb") && !xf::valid_member_path(std::string("/a\0b", 4)));
    CHECK(xf::valid_serial("BLUS30443") && !xf::valid_serial("blus30443") && !xf::valid_serial("BLUS3044") && !xf::valid_serial("BLUS304431") &&
        !xf::valid_serial("BLU530443") && !xf::valid_serial(""));
}

// matching members with the library rows.
void test_assign()
{
    const auto rows = library();
    {
        // Exact paths, a USB drive in another port, a member not in the library now.
        xf::model m = canonical_model();
        const xf::assignment a = xf::assign(m, rows, always);
        CHECK(a.folder_of_row == (std::vector<int>{-1, -1, 0, -1, 0, -1, -1}));
        CHECK(a.rows_of == (std::vector<std::vector<std::size_t>>{{2, 4}, {}, {}}));
        CHECK(a.member_of == (std::vector<std::vector<std::size_t>>{{0, 1}, {}, {}}));
        CHECK(a.missing == (std::vector<std::size_t>{0, 1, 0}) && xf::missing_count(a) == 1);
        CHECK(a.missing_paths[1] == std::vector<std::string>{"/data/rpcs3/pkgs/Tekken 6.pkg"});
        CHECK(a.ids == (std::vector<std::uint32_t>{1, 2, 3}));
        CHECK(!a.changed && a.adopted == 0 && a.duplicates == 0 && m == canonical_model());
    }
    {
        // The same game in two folders stays in the first; the later line leaves the model.
        xf::model m = canonical_model();
        m.folders[2].members.push_back({"BLUS30066", "/data/rpcs3/games/Burnout Paradise.iso", "Burnout Paradise", ""});
        m.folders[0].members.push_back({"BLUS30066", "/data/rpcs3/games/Burnout Paradise.iso", "again", ""});
        const xf::assignment a = xf::assign(m, rows, always);
        CHECK(a.duplicates == 2 && a.changed);
        CHECK(a.rows_of[0] == (std::vector<std::size_t>{2, 4}) && a.rows_of[2].empty());
        CHECK(m.folders[0].members.size() == 2 && m.folders[2].members.empty());
    }
    {
        // Two drives with the same relative path: the lowest drive index is the member, the other row is in no folder.
        std::vector<xf::row> two = rows;
        two.push_back(game("/mnt/usb3/PS3ISO/X.iso", "BLES00002", "X 3"));
        two.push_back(game("/mnt/usb0/PS3ISO/X.iso", "BLES00002", "X 0"));
        xf::model m;
        m.folders.push_back({1, "A", "", {{"BLES00002", "usb:/PS3ISO/X.iso", "X", ""}}});
        const xf::assignment a = xf::assign(m, two, always);
        CHECK(a.rows_of[0] == std::vector<std::size_t>{8} && a.folder_of_row[7] == -1 && a.folder_of_row[8] == 0);
    }
    {
        // Adoption: a game renamed inside /data (FTP) follows into its folder by TITLE_ID.
        xf::model m;
        m.folders.push_back({1, "A", "", {{"BLUS30443", "/data/rpcs3/games/Old Name.iso", "Old", "\tkeep"}}});
        std::vector<std::string> asked;
        const xf::assignment a = xf::assign(m, rows, [&](std::string_view path) { asked.emplace_back(path); return true; });
        CHECK(asked == std::vector<std::string>{"/data/rpcs3/games/Old Name.iso"});
        CHECK(a.adopted == 1 && a.changed && a.rows_of[0] == std::vector<std::size_t>{3} && a.missing[0] == 0);
        CHECK(m.folders[0].members[0].path == "/data/rpcs3/games/Demon's Souls.iso" && m.folders[0].members[0].title == "Demon's Souls");
        CHECK(m.folders[0].members[0].extra == "\tkeep");
        const std::string text = xf::serialize(m);
        CHECK(text.find("Demon's Souls.iso") != std::string::npos && text.find("Old Name.iso") == std::string::npos);
    }
    {
        // No adoption: two rows with the serial, storage not present (USB unplugged), another kind, a row in a folder,
        // no serial.
        std::vector<xf::row> twice = rows;
        twice.push_back(game("/data/rpcs3/games/Demon's Souls copy.iso", "BLUS30443", "Demon's Souls"));
        xf::model m;
        m.folders.push_back({1, "A", "", {{"BLUS30443", "/data/rpcs3/games/Old Name.iso", "Old", ""}}});
        xf::assignment a = xf::assign(m, twice, always);
        CHECK(a.adopted == 0 && a.missing[0] == 1 && m.folders[0].members[0].path == "/data/rpcs3/games/Old Name.iso");

        xf::model usb;
        usb.folders.push_back({1, "A", "", {{"BCUS98114", "usb:/Old/GT5.iso", "GT5", ""}}});
        a = xf::assign(usb, rows, [](std::string_view path) { return !xf::on_usb(path); });
        CHECK(a.adopted == 0 && a.missing[0] == 1 && usb.folders[0].members[0].path == "usb:/Old/GT5.iso");
        a = xf::assign(usb, rows, always); // a drive is connected and none has the old path: the game moved
        CHECK(a.adopted == 1 && usb.folders[0].members[0].path == "usb:/PS3ISO/Gran Turismo 5.iso");

        xf::model kind;
        kind.folders.push_back({1, "A", "", {{"BLES00001", "/data/rpcs3/games/Folder Game.iso", "FG", ""}}});
        a = xf::assign(kind, rows, always);
        CHECK(a.adopted == 0 && a.missing[0] == 1);
        kind.folders[0].members[0].path = "/data/rpcs3/games/Old Folder Game";
        a = xf::assign(kind, rows, always);
        CHECK(a.adopted == 1 && a.rows_of[0] == std::vector<std::size_t>{5});

        xf::model taken;
        taken.folders.push_back({1, "A", "", {{"BLUS30066", "/data/rpcs3/games/Burnout Paradise.iso", "BP", ""}}});
        taken.folders.push_back({2, "B", "", {{"BLUS30066", "/data/rpcs3/games/Old Burnout.iso", "BP", ""}}});
        a = xf::assign(taken, rows, always);
        CHECK(a.adopted == 0 && a.missing[1] == 1 && a.rows_of[0] == std::vector<std::size_t>{2});

        xf::model none;
        none.folders.push_back({1, "A", "", {{"", "/data/rpcs3/games/Old.iso", "x", ""}}});
        a = xf::assign(none, rows, always);
        CHECK(a.adopted == 0 && a.missing[0] == 1);
    }
    {
        // Rows that are not games never become members, even with a matching path.
        xf::model m;
        m.folders.push_back({1, "A", "", {{"", "@savedata", "x", ""}}});
        std::vector<xf::row> odd = rows;
        odd.push_back({"/data/rpcs3/games/Note", "", "a note", false, false});
        m.folders[0].members.push_back({"", "/data/rpcs3/games/Note", "x", ""});
        const xf::assignment a = xf::assign(m, odd, always);
        CHECK(a.rows_of[0].empty() && a.missing[0] == 2);
    }
    {
        // Rows built from column entries: games by path, the kind from the file extension.
        const xf::row iso = xf::make_row("/mnt/usb2/PS3ISO/A.ISO", "BLUS30001", "A");
        CHECK(iso.is_game && !iso.folder_kind && iso.serial == "BLUS30001" && iso.title == "A" && xf::can_be_member(iso));
        const xf::row dir = xf::make_row("/data/rpcs3/dev_hdd0/game/NPUB30001", "NPUB30001", "B");
        CHECK(dir.is_game && dir.folder_kind && xf::can_be_member(dir));
        CHECK(xf::make_row("/data/rpcs3/games/Game v1.02", "", "C").folder_kind);
        CHECK(!xf::make_row("/data/rpcs3/pkgs/X.pkg", "", "").folder_kind && !xf::make_row("/data/x/EBOOT.self", "", "").folder_kind);
        for (const char* path : {"@savedata", "@licenses", "@folder:1", "@folder-empty:1", ""})
        {
            const xf::row r = xf::make_row(path, "", "x");
            CHECK(!r.is_game && !r.folder_kind && !xf::can_be_member(r));
        }
        // The same rows through make_row match as the hand-built ones.
        std::vector<xf::row> built;
        for (const auto& r : rows) built.push_back(xf::make_row(r.path, r.serial, r.title));
        for (std::size_t i = 0; i < rows.size(); ++i) CHECK(built[i].is_game == rows[i].is_game && built[i].folder_kind == rows[i].folder_kind);
    }
}

std::vector<int> values(const std::vector<xf::slot>& slots)
{
    std::vector<int> out;
    for (const auto& s : slots) out.push_back(s.value);
    return out;
}

// the projected 'Jogo' column.
void test_project()
{
    const auto rows = library();
    xf::model m = canonical_model();
    const xf::assignment a = xf::assign(m, rows, always);
    // Utility rows, folders in order, games in no folder in library order, the note.
    CHECK(values(xf::project(a, rows, std::nullopt)) == (std::vector<int>{0, 1, -1, -2, -3, 3, 5, 6}));
    // Inside a folder: member order; a folder whose games are not in the library or that has none: one empty row.
    CHECK(values(xf::project(a, rows, 0)) == (std::vector<int>{2, 4}));
    CHECK(values(xf::project(a, rows, 1)) == std::vector<int>{xf::empty_row});
    CHECK(values(xf::project(a, rows, 2)) == std::vector<int>{xf::empty_row});
    CHECK(values(xf::project(a, rows, 3)) == values(xf::project(a, rows, std::nullopt))); // gone: top level

    // The PS3 disc row is inserted at index 2 after the projection: it lands after the utility rows, before the folders.
    std::vector<xf::slot> top = xf::project(a, rows, std::nullopt);
    top.insert(top.begin() + 2, xf::row_slot(99));
    CHECK(top[1].value == 1 && top[2].value == 99 && top[3].is_folder() && top[3].folder_index() == 0);
    // A disc row already in the rows is one of the leading '@' rows.
    std::vector<xf::row> with_disc = rows;
    with_disc.insert(with_disc.begin() + 2, utility("@disc:install", "Disco"));
    xf::model md = canonical_model();
    const xf::assignment ad = xf::assign(md, with_disc, always);
    CHECK(values(xf::project(ad, with_disc, std::nullopt)) == (std::vector<int>{0, 1, 2, -1, -2, -3, 4, 6, 7}));

    // No folders: the library column unchanged.
    xf::model empty;
    const xf::assignment none = xf::assign(empty, rows, always);
    CHECK(values(xf::project(none, rows, std::nullopt)) == (std::vector<int>{0, 1, 2, 3, 4, 5, 6}));

    // The 'Carregando' placeholder (no scan yet): folders shown, every member counted missing, open folders empty.
    const std::vector<xf::row> loading{utility("@savedata", "u"), utility("@licenses", "l"), utility("", "Carregando biblioteca")};
    xf::model ml = canonical_model();
    const xf::assignment al = xf::assign(ml, loading, always);
    CHECK(values(xf::project(al, loading, std::nullopt)) == (std::vector<int>{0, 1, -1, -2, -3, 2}));
    CHECK(values(xf::project(al, loading, 0)) == std::vector<int>{xf::empty_row});
    CHECK(al.missing == (std::vector<std::size_t>{2, 1, 0}) && ml == canonical_model());
    // No game found: the "Nenhum jogo encontrado" row (empty path) follows the folders.
    const std::vector<xf::row> nothing{utility("@savedata", "u"), utility("@licenses", "l"), utility("", "Nenhum jogo encontrado")};
    CHECK(values(xf::project(al, nothing, std::nullopt)) == (std::vector<int>{0, 1, -1, -2, -3, 2}));

    // Slots and the folder row paths.
    CHECK(xf::folder_slot(3).is_folder() && xf::folder_slot(3).folder_index() == 3 && !xf::folder_slot(3).is_row());
    CHECK(xf::folder_slot(199).folder_index() == 199);
    CHECK(xf::row_slot(7).is_row() && xf::row_slot(7).row_index() == 7 && !xf::row_slot(0).is_folder());
    const xf::slot e{xf::empty_row};
    CHECK(e.is_empty_row() && !e.is_folder() && !e.is_row());
    CHECK(xf::folder_row_path(12) == "@folder:12" && xf::empty_row_path(12) == "@folder-empty:12");
    CHECK(xf::folder_row_id("@folder:12") == 12u && !xf::folder_row_id("@folder-empty:12") && xf::empty_row_id("@folder-empty:12") == 12u);
    CHECK(!xf::folder_row_id("@folder:0") && !xf::folder_row_id("@folder:12x") && !xf::folder_row_id("@folder:") && !xf::empty_row_id("@folder:3"));
}

// the operations, then all of them through the store and a recording I/O.
void test_operations()
{
    const auto rows = library();
    xf::refusal why = xf::refusal::none;
    std::uint32_t id = 0;
    {
        xf::model m = canonical_model();
        CHECK(xf::create(m, "  Nova   pasta ", id, &why) && id == 4 && m.folders.back().name == "Nova pasta" && why == xf::refusal::none);
        CHECK(xf::create(m, "Corrida", id, &why) && id == 5 && m.folders.back().name == "Corrida (2)");
        CHECK(xf::create(m, "CORRIDA", id, &why) && m.folders.back().name == "CORRIDA (3)");
        CHECK(xf::create(m, repeat("x", 40), id, &why) && m.folders.back().name == repeat("x", 32));
        CHECK(xf::create(m, repeat("X", 40), id, &why) && m.folders.back().name == repeat("X", 28) + " (2)");
        CHECK(!xf::create(m, " \t\x01 ", id, &why) && why == xf::refusal::empty_name);
        CHECK(!xf::create(m, "", id, &why) && why == xf::refusal::empty_name);
        // Ids: the highest + 1, so a removed folder's id comes back only when it was the highest.
        CHECK(id == 8 && xf::remove_folder(m, id, &why) == 0 && why == xf::refusal::none);
        CHECK(xf::create(m, "Outra", id, &why) && id == 8);
        CHECK(xf::remove_folder(m, 2, &why) == 1);
        CHECK(xf::create(m, "Luta", id, &why) && id == 9 && m.folders.back().name == "Luta");
        CHECK(xf::remove_folder(m, 77, &why) == 0 && why == xf::refusal::unknown_folder);
        // Every id up to the limit handed out: a free one is reused.
        xf::model high;
        high.folders.push_back({xf::max_id, "Alta", "", {}});
        CHECK(xf::create(high, "Nova", id, &why) && id == 1);
    }
    {
        xf::model m = canonical_model();
        CHECK(xf::rename(m, 2, "Luta livre", &why) && m.folders[1].name == "Luta livre");
        CHECK(!xf::rename(m, 2, " Luta  livre ", &why) && why == xf::refusal::none);
        CHECK(xf::rename(m, 2, "corrida", &why) && m.folders[1].name == "corrida (2)");
        CHECK(xf::rename(m, 1, "CORRIDA", &why) && m.folders[0].name == "CORRIDA"); // its own name in another case
        CHECK(!xf::rename(m, 9, "X", &why) && why == xf::refusal::unknown_folder);
        CHECK(!xf::rename(m, 1, "\x7F", &why) && why == xf::refusal::empty_name && m.folders[0].name == "CORRIDA");
    }
    {
        // Deleting a folder: its lines only; the library rows are untouched.
        xf::model m = canonical_model();
        const auto rows_before = rows;
        CHECK(xf::remove_folder(m, 1, &why) == 2 && why == xf::refusal::none);
        CHECK(m.folders.size() == 2 && m.folders[0].id == 2 && xf::member_count(m) == 1);
        xf::model again = m;
        const xf::assignment a = xf::assign(again, rows, always);
        CHECK(values(xf::project(a, rows, std::nullopt)) == (std::vector<int>{0, 1, -1, -2, 2, 3, 4, 5, 6}));
        CHECK(rows.size() == rows_before.size());
        for (std::size_t i = 0; i < rows.size(); ++i) CHECK(rows[i].path == rows_before[i].path && rows[i].title == rows_before[i].title);
    }
    {
        // Moving games: into a folder (appended), out of every folder, to a new folder, already there.
        xf::model m = canonical_model();
        m.folders[1].members[0].extra = "\tmine";
        CHECK(xf::move_to(m, rows[3], 1u, &why) && m.folders[0].members.size() == 3 && m.folders[0].members[2].path == "/data/rpcs3/games/Demon's Souls.iso");
        CHECK(m.folders[0].members[2].serial == "BLUS30443" && m.folders[0].members[2].title == "Demon's Souls" && m.folders[0].members[2].extra.empty());
        const xf::model there = m;
        CHECK(!xf::move_to(m, rows[2], 1u, &why) && why == xf::refusal::none && m == there); // position kept
        CHECK(!xf::move_to(m, rows[5], std::nullopt, &why) && why == xf::refusal::none);   // in no folder already
        CHECK(xf::move_to(m, rows[2], std::nullopt, &why) && m.folders[0].members.size() == 2 && m.folders[0].members[0].serial == "BCUS98114");
        CHECK(xf::create(m, "Nova", id, &why) && xf::move_to(m, rows[4], id, &why));
        CHECK(m.folders[0].members.size() == 1 && m.folders[3].members.size() == 1 && m.folders[3].members[0].path == "usb:/PS3ISO/Gran Turismo 5.iso");
        // A member moved from another folder keeps its own fields; a game not in the library now can still move.
        CHECK(xf::move_to(m, game("/data/rpcs3/pkgs/Tekken 6.pkg", "", "Tekken 6"), 1u, &why));
        CHECK(m.folders[1].members.empty() && m.folders[0].members.back().extra == "\tmine" && m.folders[0].members.back().title == "Tekken 6");
        const xf::model before = m;
        CHECK(!xf::move_to(m, rows[0], 1u, &why) && why == xf::refusal::not_a_game);
        CHECK(!xf::move_to(m, rows[6], 1u, &why) && why == xf::refusal::not_a_game);
        CHECK(!xf::move_to(m, game("relative.iso", "", "x"), 1u, &why) && why == xf::refusal::not_a_game);
        CHECK(!xf::move_to(m, game("/data/a\tb.iso", "", "x"), 1u, &why) && why == xf::refusal::not_a_game);
        CHECK(!xf::move_to(m, rows[3], 55u, &why) && why == xf::refusal::unknown_folder);
        CHECK(m == before);
        // A game listed twice by a hand edit ends listed once, in the folder asked for.
        m.folders[2].members.push_back(m.folders[0].members[0]);
        CHECK(xf::move_to(m, rows[3], 3u, &why));
        std::size_t lines = 0;
        for (const auto& f : m.folders)
            for (const auto& x : f.members) lines += x.path == "/data/rpcs3/games/Demon's Souls.iso";
        CHECK(lines == 1 && m.folders[2].members.back().path == "/data/rpcs3/games/Demon's Souls.iso");
    }
    {
        // The checklist: marked games become members (moved from other folders, in the list's order), unmarked listed
        // games leave, members not listed (not in the library now) stay.
        xf::model m;
        m.folders.push_back({1, "A", "", {{"BLUS30066", "/data/rpcs3/games/Burnout Paradise.iso", "BP", ""},
                                             {"", "/data/gone.iso", "Gone", ""},
                                             {"BLUS30443", "/data/rpcs3/games/Demon's Souls.iso", "DS", ""}}});
        m.folders.push_back({2, "B", "", {{"BCUS98114", "usb:/PS3ISO/Gran Turismo 5.iso", "GT5", "\tfrom B"}}});
        const std::vector<xf::row> listed{rows[2], rows[3], rows[4], rows[5]};
        const std::vector<xf::row> checked{rows[5], rows[3], rows[4], rows[3]};
        CHECK(xf::set_members(m, 1, checked, listed, &why));
        const auto& a = m.folders[0].members;
        CHECK(a.size() == 4);
        if (a.size() == 4)
        {
            CHECK(a[0].path == "/data/gone.iso" && a[1].path == "/data/rpcs3/games/Demon's Souls.iso");
            CHECK(a[2].path == "/data/rpcs3/games/Folder Game" && a[3].path == "usb:/PS3ISO/Gran Turismo 5.iso" && a[3].extra == "\tfrom B");
        }
        CHECK(m.folders[1].members.empty());
        const xf::model same = m;
        const std::vector<xf::row> again{rows[3], rows[4], rows[5]};
        CHECK(!xf::set_members(m, 1, again, listed, &why) && why == xf::refusal::none && m == same);
        CHECK(!xf::set_members(m, 7, again, listed, &why) && why == xf::refusal::unknown_folder);
        CHECK(xf::set_members(m, 1, {}, listed, &why) && m.folders[0].members.size() == 1 && m.folders[0].members[0].path == "/data/gone.iso");
    }
    {
        // Positions stop at the first and the last place.
        xf::model m = canonical_model();
        CHECK(!xf::move_folder(m, 0, -1, &why) && why == xf::refusal::none);
        CHECK(xf::move_folder(m, 0, 1, &why) && m.folders[0].id == 2 && m.folders[1].id == 1);
        CHECK(xf::move_folder(m, 0, 100, &why) && m.folders[2].id == 2 && m.folders[0].id == 1);
        CHECK(!xf::move_folder(m, 2, 5, &why) && why == xf::refusal::none);
        CHECK(xf::move_folder(m, 2, -100, &why) && m.folders[0].id == 2);
        CHECK(!xf::move_folder(m, 3, 1, &why) && why == xf::refusal::unknown_folder);
        CHECK(!xf::move_folder(m, 1, 0, &why) && why == xf::refusal::none);

        // A member moves among the shown games: member_of maps a shown row to its member, over hidden ones.
        xf::model f;
        f.folders.push_back({1, "A", "", {{"", "/data/rpcs3/games/Burnout Paradise.iso", "BP", ""},
                                             {"", "/data/gone.iso", "Gone", ""},
                                             {"", "/data/rpcs3/games/Demon's Souls.iso", "DS", ""}}});
        const xf::assignment a = xf::assign(f, rows, always);
        CHECK(a.member_of[0] == (std::vector<std::size_t>{0, 2}));
        const long long step = static_cast<long long>(a.member_of[0][0]) - static_cast<long long>(a.member_of[0][1]);
        CHECK(xf::move_member(f, 1, a.member_of[0][1], static_cast<int>(step), &why));
        CHECK(f.folders[0].members[0].path == "/data/rpcs3/games/Demon's Souls.iso" && f.folders[0].members[2].path == "/data/gone.iso");
        CHECK(!xf::move_member(f, 1, 0, -1, &why) && why == xf::refusal::none);
        CHECK(xf::move_member(f, 1, 0, 9, &why) && f.folders[0].members[2].path == "/data/rpcs3/games/Demon's Souls.iso");
        CHECK(!xf::move_member(f, 1, 3, 1, &why) && why == xf::refusal::unknown_folder);
        CHECK(!xf::move_member(f, 2, 0, 1, &why) && why == xf::refusal::unknown_folder);
    }
    {
        // Forgetting games not in the library now removes exactly those lines, by path.
        xf::model m = canonical_model();
        m.folders[0].members.push_back({"", "/data/gone 1.iso", "G1", ""});
        m.folders[0].members.push_back({"", "/data/gone 2.iso", "G2", ""});
        const xf::assignment a = xf::assign(m, rows, always);
        CHECK(a.missing[0] == 2);
        xf::model fresh = m; // the same folder read again, with one more line nobody has seen
        fresh.folders[0].members.push_back({"", "/data/gone 3.iso", "G3", ""});
        CHECK(xf::forget_missing(fresh, 1, a, &why) == 2 && fresh.folders[0].members.size() == 3 && fresh.folders[0].members[2].path == "/data/gone 3.iso");
        CHECK(xf::forget_missing(m, 1, a, &why) == 2 && m.folders[0].members.size() == 2);
        CHECK(xf::forget_missing(m, 3, a, &why) == 0 && why == xf::refusal::none);
        CHECK(xf::forget_missing(m, 8, a, &why) == 0 && why == xf::refusal::unknown_folder);
    }

    // Everything through the store: only the three folder files are read, written or renamed, and no game file changes.
    fake_io io;
    put_game_files(io);
    const auto games_before = game_files(io);
    xf::store s;
    s.load(io);
    const auto op_create = [&](std::string name) { return [&, name](xf::model& x, xf::refusal& w) { return xf::create(x, name, id, &w); }; };
    xf::save_result r = s.apply(io, op_create("Corrida"));
    CHECK(r.status == xf::save_status::saved && !r.reloaded && r.bytes == io.text(xf::file_path).size());
    CHECK(io.writes == std::vector<std::string>{xf::file_path} && !io.has(xf::backup_path)); // nothing good existed before
    const std::string first = io.text(xf::file_path);
    CHECK(is_folder_text(first, s.m));
    io.clear_records();
    r = s.apply(io, op_create("Luta"));
    CHECK(r.status == xf::save_status::saved);
    CHECK((io.writes == std::vector<std::string>{xf::backup_path, xf::file_path}) && io.text(xf::backup_path) == first);
    CHECK(io.stats == (std::vector<std::string>{xf::file_path, xf::file_path}) && io.reads.empty());
    const std::uint32_t luta = id;
    std::vector<std::function<bool(xf::model&, xf::refusal&)>> ops{
        [&](xf::model& x, xf::refusal& w) { return xf::move_to(x, rows[2], 1u, &w); },
        [&](xf::model& x, xf::refusal& w) { return xf::move_to(x, rows[3], luta, &w); },
        [&](xf::model& x, xf::refusal& w) { return xf::set_members(x, 1, std::span(rows).subspan(4, 2), std::span(rows).subspan(2, 4), &w); },
        [&](xf::model& x, xf::refusal& w) { return xf::rename(x, luta, "Luta livre", &w); },
        [&](xf::model& x, xf::refusal& w) { return xf::move_folder(x, 1, -1, &w); },
        [&](xf::model& x, xf::refusal& w) { return xf::move_member(x, 1, 0, 1, &w); },
        [&](xf::model& x, xf::refusal& w) { return xf::move_to(x, rows[5], std::nullopt, &w); },
        [&](xf::model& x, xf::refusal& w) { return xf::remove_folder(x, luta, &w), w == xf::refusal::none; },
    };
    for (auto& op : ops)
    {
        const std::string previous = io.text(xf::file_path);
        r = s.apply(io, op);
        CHECK(r.status == xf::save_status::saved);
        CHECK(is_folder_text(io.text(xf::file_path), s.m) && io.text(xf::backup_path) == previous);
    }
    CHECK(s.m.folders.size() == 1 && s.m.folders[0].name == "Corrida" && s.m.folders[0].members.size() == 1);
    // A member that is not in the library is forgotten through the store too.
    s.m.folders[0].members.push_back({"", "/data/gone.iso", "Gone", ""});
    xf::assignment a = xf::assign(s.m, rows, always);
    r = s.apply(io, [&](xf::model& x, xf::refusal& w) { return xf::forget_missing(x, 1, a, &w) > 0; });
    CHECK(r.status == xf::save_status::saved && s.m.folders[0].members.size() == 1);
    // A refusal or no change reads and writes nothing beyond the change check.
    io.clear_records();
    r = s.apply(io, [&](xf::model& x, xf::refusal& w) { return xf::rename(x, 1, "Corrida", &w); });
    CHECK(r.status == xf::save_status::unchanged && io.writes.empty() && io.renames.empty() && io.reads.empty());
    r = s.apply(io, [&](xf::model& x, xf::refusal& w) { return xf::rename(x, 99, "X", &w); });
    CHECK(r.status == xf::save_status::refused && r.why == xf::refusal::unknown_folder && io.writes.empty());

    // A damaged main file with a good backup: the damaged file is kept, the backup stays as it was.
    io.put(xf::file_path, "damaged by hand");
    const std::string backup_before = io.text(xf::backup_path);
    s.load(io);
    CHECK(s.status == xf::load_status::damaged && s.from_backup && is_folder_text(backup_before, s.m));
    io.clear_records();
    r = s.apply(io, op_create("Depois"));
    CHECK(r.status == xf::save_status::saved);
    CHECK(io.renames == std::vector<std::string>{std::string(xf::file_path) + " -> " + xf::damaged_path});
    CHECK(io.writes == std::vector<std::string>{xf::file_path});
    CHECK(io.text(xf::damaged_path) == "damaged by hand" && io.text(xf::backup_path) == backup_before && is_folder_text(io.text(xf::file_path), s.m));
    io.clear_records();
    r = s.apply(io, op_create("Mais"));
    CHECK(r.status == xf::save_status::saved && io.renames.empty() && (io.writes == std::vector<std::string>{xf::backup_path, xf::file_path}));

    // Both damaged: start empty; the first save keeps the damaged main file and leaves the backup alone.
    io.put(xf::file_path, "bad main");
    io.put(xf::backup_path, "bad backup");
    s.load(io);
    CHECK(s.status == xf::load_status::damaged && !s.from_backup && s.m == xf::model{});
    io.clear_records();
    r = s.apply(io, op_create("Nova"));
    CHECK(r.status == xf::save_status::saved && io.text(xf::damaged_path) == "bad main" && io.text(xf::backup_path) == "bad backup");
    CHECK(io.writes == std::vector<std::string>{xf::file_path});

    // A failed write of the main file: the damaged file gets its name back, the change stays in memory.
    io.put(xf::file_path, "bad again");
    io.files.erase(xf::damaged_path);
    io.mtimes.erase(xf::damaged_path);
    s.load(io);
    io.fail_write.insert(xf::file_path);
    r = s.apply(io, op_create("Memoria"));
    CHECK(r.status == xf::save_status::failed && r.failed_step == "main file");
    CHECK(io.text(xf::file_path) == "bad again" && !io.has(xf::damaged_path) && s.m.folders.size() == 1 && s.m.folders[0].name == "Memoria");
    io.fail_write.clear();
    r = s.apply(io, op_create("Depois da falha"));
    CHECK(r.status == xf::save_status::saved && s.m.folders.size() == 2 && is_folder_text(io.text(xf::file_path), s.m));
    CHECK(io.text(xf::damaged_path) == "bad again");
    // A failed backup write changes nothing on disk.
    const std::string main_before = io.text(xf::file_path);
    io.fail_write.insert(xf::backup_path);
    r = s.apply(io, op_create("Sem backup"));
    CHECK(r.status == xf::save_status::failed && r.failed_step == "backup" && io.text(xf::file_path) == main_before);
    io.fail_write.clear();
    // A failed rename of the damaged file writes nothing.
    io.put(xf::file_path, "bad once more");
    s.load(io);
    io.fail_rename.insert(xf::file_path);
    io.clear_records();
    r = s.apply(io, op_create("X"));
    CHECK(r.status == xf::save_status::failed && r.failed_step == "damaged copy" && io.writes.empty() && io.text(xf::file_path) == "bad once more");
    io.fail_rename.clear();

    CHECK(only_folder_files(io));
    CHECK(game_files(io) == games_before);
}

// name cleaning.
void test_clean_name()
{
    CHECK(xf::clean_name("  Corrida  ") == "Corrida");
    CHECK(xf::clean_name("A    B  C") == "A B C");
    CHECK(xf::clean_name("A\tB\r\nC") == "ABC");
    CHECK(xf::clean_name("A \t B") == "A B");
    CHECK(xf::clean_name("\x01X\x7F\x1F") == "X");
    CHECK(xf::clean_name("A" "\xC2\x85" "B") == "AB");          // C1 control U+0085
    CHECK(xf::clean_name("A" "\xC2\xA0" "B") == "A\xC2\xA0" "B"); // a no-break space is a character
    CHECK(xf::clean_name("\xE2\x80\xAE" "abc" "\xE2\x80\xAC") == "abc"); // RLO .. PDF
    CHECK(xf::clean_name("a" "\xE2\x81\xA6" "b" "\xE2\x81\xA9") == "ab"); // LRI .. PDI
    CHECK(xf::clean_name("a" "\xE2\x80\xA8" "b" "\xE2\x80\xA9" "c") == "abc");
    CHECK(xf::clean_name("\xE2\x80\x8E" "Abc") == "\xE2\x80\x8E" "Abc");             // LRM kept
    CHECK(xf::clean_name("x" "\xE2\x80\x8F") == "x" "\xE2\x80\x8F");                 // RLM kept
    CHECK(xf::clean_name("سباق السيارات") == "سباق السيارات");
    CHECK(xf::clean_name("Ação & Aventura ™") == "Ação & Aventura ™");
    CHECK(xf::clean_name(repeat("a", 40)) == repeat("a", 32));
    CHECK(xf::clean_name(repeat("a", 31) + "çççç") == repeat("a", 31) + "ç");
    CHECK(xf::clean_name(repeat("\xD8\xB3", 40)) == repeat("\xD8\xB3", 32));
    CHECK(xf::clean_name(repeat("\xF0\x9F\x8E\xAE", 33)) == repeat("\xF0\x9F\x8E\xAE", 32)); // 4-byte code points, 128 bytes
    CHECK(xf::clean_name(repeat("a", 31) + " b") == repeat("a", 31));            // never ends with the space before a cut
    CHECK(xf::clean_name("").empty() && xf::clean_name("    ").empty() && xf::clean_name("\t\r\n").empty());
    CHECK(xf::clean_name("A" "\xFF" "B") == "AB");
    CHECK(xf::clean_name("A" "\xC0\xAF" "B") == "AB");           // overlong
    CHECK(xf::clean_name("A" "\xED\xA0\x80" "B") == "AB");       // surrogate
    CHECK(xf::clean_name("A" "\xF4\x90\x80\x80" "B") == "AB");   // above U+10FFFF
    CHECK(xf::clean_name("A" "\xE2\x82") == "A");                // cut sequence at the end
    CHECK(xf::clean_name(std::string("A\0B", 3)) == "AB");
    for (const std::string& text : {std::string("  x  y "), repeat("ab ", 20), std::string("\xE2\x80\x8E a\xC2\x85 b"), repeat("é", 50)})
        CHECK(xf::clean_name(xf::clean_name(text)) == xf::clean_name(text));
    // Titles: up to 128 bytes on a code point boundary, controls left out.
    CHECK(xf::clean_title(repeat("é", 70)) == repeat("é", 64));
    CHECK(xf::clean_title("Jogo\tcom\ntab") == "Jogocomtab");
    // Log clip on a code point boundary.
    CHECK(xf::clip("ação", 2) == "a" && xf::clip("ação", 3) == "aç" && xf::clip("abc", 10) == "abc");
    for (const xf::refusal why : {xf::refusal::none, xf::refusal::read_only, xf::refusal::folder_limit, xf::refusal::member_limit,
             xf::refusal::file_limit, xf::refusal::empty_name, xf::refusal::unknown_folder, xf::refusal::not_a_game})
        CHECK(!xf::refusal_text(why).empty() && xf::refusal_text(why) != "?");
}

// the main file changed outside the emulator (FTP) between the load and a save: the operation is applied to what
// is in the file now, and the outside version becomes the backup.
void test_outside_edit()
{
    fake_io io;
    io.put(xf::file_path, canonical());
    xf::store s;
    s.load(io);
    const std::string outside = canonical() + "folder\t7\tExterna\n";
    io.put(xf::file_path, outside);
    std::uint32_t id = 0;
    xf::save_result r = s.apply(io, [&](xf::model& x, xf::refusal& w) { return xf::create(x, "Nova", id, &w); });
    CHECK(r.status == xf::save_status::saved && r.reloaded);
    CHECK(id == 8 && s.m.folders.size() == 5 && s.m.folders[3].name == "Externa" && s.m.folders[4].name == "Nova");
    CHECK(io.text(xf::backup_path) == outside && is_folder_text(io.text(xf::file_path), s.m));
    // Our own write is not an outside change.
    r = s.apply(io, [&](xf::model& x, xf::refusal& w) { return xf::rename(x, 8, "Nova 2", &w); });
    CHECK(r.status == xf::save_status::saved && !r.reloaded);

    // Same size, newer time: still a change.
    std::string same_size = io.text(xf::file_path);
    same_size.replace(same_size.find("Nova 2"), 6, "Nova 3");
    io.put(xf::file_path, same_size);
    r = s.apply(io, [&](xf::model& x, xf::refusal& w) { return xf::rename(x, 1, "Rally", &w); });
    CHECK(r.status == xf::save_status::saved && r.reloaded && s.m.folders[4].name == "Nova 3" && s.m.folders[0].name == "Rally");

    // Replaced by a newer version's file: nothing is written.
    io.put(xf::file_path, "RPCS3 XMB folders v2\nfolder\t1\tA\n");
    io.clear_records();
    r = s.apply(io, [&](xf::model& x, xf::refusal& w) { return xf::create(x, "B", id, &w); });
    CHECK(r.status == xf::save_status::refused && r.why == xf::refusal::read_only && r.reloaded && io.writes.empty() && io.renames.empty());

    // Removed outside: no folders now; an operation on a folder that is gone is refused and writes nothing.
    io.files.erase(xf::file_path);
    io.mtimes.erase(xf::file_path);
    s = {};
    io.put(xf::file_path, canonical());
    s.load(io);
    io.files.erase(xf::file_path);
    io.mtimes.erase(xf::file_path);
    io.clear_records();
    r = s.apply(io, [&](xf::model& x, xf::refusal& w) { return xf::rename(x, 1, "X", &w); });
    CHECK(r.status == xf::save_status::refused && r.why == xf::refusal::unknown_folder && r.reloaded && io.writes.empty());
    CHECK(s.m == xf::model{} && s.status == xf::load_status::absent);
    CHECK(only_folder_files(io));
}

// Every folder file the parser accepts serializes to a file it reads back to the same model, with nothing dropped, and
// every limit holds. Random byte flips, insertions (TAB, CR, LF, invalid UTF-8), deletions, line copies and cuts.
void test_mutations()
{
    std::mt19937 random(20261023);
    const std::string seed = canonical() + "style\t1\tx\nfolder\t9\tسباق\tmore\ngame\t9\tBLES01234\tusb:/GAMES/A\tTítulo\textra\n";
    const char specials[] = {'\t', '\n', '\r', '\0', '#', ' ', '1', static_cast<char>(0xFF), static_cast<char>(0xD8), static_cast<char>(0x80)};
    int bad = 0;
    std::string first_bad;
    for (int round = 0; round < 4000; ++round)
    {
        std::string text = seed;
        const int edits = 1 + static_cast<int>(random() % 6);
        for (int e = 0; e < edits && !text.empty(); ++e)
        {
            const std::size_t at = random() % text.size();
            switch (random() % 6)
            {
            case 0: text[at] = static_cast<char>(random() & 0xFF); break;
            case 1: text.insert(text.begin() + static_cast<std::ptrdiff_t>(at), specials[random() % sizeof(specials)]); break;
            case 2: text.erase(at, 1 + random() % 8); break;
            case 3:
            {
                const std::size_t start = text.rfind('\n', at);
                const std::size_t end = text.find('\n', at);
                if (start != std::string::npos && end != std::string::npos) text.insert(end, text.substr(start, end - start));
                break;
            }
            case 4: text.resize(at); break;
            default: text.insert(at, repeat("\xC3\xA9", random() % 40)); break;
            }
        }
        const xf::parsed p = xf::parse(text);
        bool ok = xf::member_count(p.m) <= xf::max_members && p.m.folders.size() <= xf::max_folders;
        std::set<std::uint32_t> ids;
        std::set<std::string> paths;
        for (const auto& f : p.m.folders)
        {
            ok = ok && f.id >= 1 && f.id <= xf::max_id && ids.insert(f.id).second && !f.name.empty() && xf::clean_name(f.name) == f.name;
            for (const auto& x : f.members)
                ok = ok && xf::valid_member_path(x.path) && (x.serial.empty() || xf::valid_serial(x.serial)) && xf::clean_title(x.title) == x.title &&
                    paths.insert(x.path).second;
        }
        if (p.status != xf::load_status::damaged)
        {
            const xf::parsed q = xf::parse(xf::serialize(p.m));
            xf::model expected = p.m;
            expected.read_only = false;
            ok = ok && q.status == xf::load_status::ok && q.dropped == 0 && q.m == expected && xf::serialize(q.m) == xf::serialize(p.m);
        }
        else ok = ok && p.m == xf::model{};
        const xf::parsed c = xf::choose(text, seed);
        ok = ok && (p.status == xf::load_status::damaged ? c.from_backup : c.m == p.m);
        if (!ok && !bad++) first_bad = text;
    }
    CHECK(bad == 0);
    if (bad)
    {
        std::fprintf(stderr, "mutations: %d bad, first one (hex):", bad);
        for (const char c : first_bad) std::fprintf(stderr, " %02x", static_cast<unsigned char>(c));
        std::fprintf(stderr, "\n");
    }
}
}

int main()
{
    test_round_trip();
    test_tolerated_input();
    test_choose();
    test_newer_version();
    test_bad_lines();
    test_limits();
    test_unknown_data_kept();
    test_portable_path();
    test_assign();
    test_project();
    test_operations();
    test_clean_name();
    test_outside_edit();
    test_mutations();
    if (failures)
    {
        std::fprintf(stderr, "xmb_folders_test: %d checks, %d failures\n", checks, failures);
        return 1;
    }
    std::printf("xmb_folders_test OK: %d checks, 0 failures\n", checks);
    return 0;
}
