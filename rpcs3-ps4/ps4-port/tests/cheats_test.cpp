// Host test of the console cheats (rpcs3/util/ps4_cheats.hpp, standard-library part):
//  1 text parser: code types, case, tabs, CRLF, BOM, comments, directives, [Title] / #Title / bare titles, .ncl
//    blocks and constant flags, codes before a title, empty titles, odd hex, 8192-digit values, quoted text with
//    escapes, decimal floats, 9- and 16-digit addresses, unknown and not yet supported types; ids and line numbers
//  2 converter: the worked examples byte for byte, mode derivation over a segment table (code / data / mixed /
//    dynamic on code), caps
//  3 interpreter over a sparse fake memory: pointer chains (null, unmapped, past 4 GiB), D/E guards, series wrap,
//    copy overlap, find-replace, budget split across passes, unmapped / code / refused / read-only targets
//  4 PC formats: cheats.yml (block and flow, quoted titles, type names, bad entries), redirection scripts, the
//    clipboard string, typed values
//  5 state and seen files: round trip, key stable under spacing and case, changed codes start off, rename keeps the
//    key, typed values, the console's own file read back
//  6 import naming: serial from the file name / directive / metadata / header, title matching, cheats.yml split
//  7 robustness: 10^5 random lines and random bytes never crash and never pass a cap
// (the CHT strings are checked by i18n_test, area CHT)
// Build and run from the repo root (llvm-mingw, as the other host tests):
//   clang++ -std=c++20 -O2 -static -Wall -Wextra -I rpcs3 ps4-port/tests/cheats_test.cpp -o cheats_test.exe
//   cheats_test.exe
#include "util/ps4_cheats.hpp"

#include <array>
#include <cstdio>
#include <map>
#include <random>
#include <string>
#include <vector>

namespace
{
namespace ch = utils::ps4::cheats;
namespace i18n = utils::ps4::i18n;
using S = i18n::str;
using ch::u32;
using ch::u64;
using ch::u8;

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

std::string bytes(std::initializer_list<int> v)
{
    std::string out;
    for (const int b : v) out += static_cast<char>(b);
    return out;
}

bool has_problem(const ch::cheat& c, S id, u32 line = 0)
{
    for (const ch::problem& p : c.problems)
        if (p.id == id && (!line || p.line == line)) return true;
    return false;
}

bool has_note(const std::vector<ch::problem>& notes, S id)
{
    for (const ch::problem& p : notes)
        if (p.id == id) return true;
    return false;
}

const ch::cheat* named(const ch::parsed_file& f, std::string_view name)
{
    for (const ch::cheat& c : f.cheats)
        if (c.name == name) return &c;
    return nullptr;
}

// Sparse guest memory: 4 KiB pages with their protection, plus refused ranges (the RSX context)
struct fake_memory
{
    struct page
    {
        std::array<char, 4096> data{};
        bool writable = true;
        bool exec = false;
    };
    std::map<u32, page> pages;
    std::vector<std::pair<u64, u64>> refused;
    u32 reads = 0, writes = 0;

    void map(u32 addr, u32 size, bool writable = true, bool exec = false)
    {
        for (u64 p = addr & ~u64{4095}; p < u64(addr) + size; p += 4096)
        {
            page& pg = pages[static_cast<u32>(p >> 12)];
            pg.writable = writable;
            pg.exec = exec;
        }
    }
    page* find(u64 addr)
    {
        const auto it = pages.find(static_cast<u32>(addr >> 12));
        return it == pages.end() ? nullptr : &it->second;
    }
    ch::access read(u32 addr, void* out, u32 n)
    {
        ++reads;
        for (u64 i = 0; i < n; ++i)
            if (!find(addr + i)) return ch::access::unmapped;
        for (u64 i = 0; i < n; ++i) static_cast<char*>(out)[i] = find(addr + i)->data[(addr + i) & 4095];
        return ch::access::ok;
    }
    ch::access write(u32 addr, const void* in, u32 n)
    {
        for (const auto& [b, e] : refused)
            if (addr < e && addr + u64(n) > b) return ch::access::refused;
        for (u64 i = 0; i < n; ++i)
        {
            const page* p = find(addr + i);
            if (!p) return ch::access::unmapped;
            if (p->exec) return ch::access::code;
            if (!p->writable) return ch::access::read_only;
        }
        ++writes;
        for (u64 i = 0; i < n; ++i) find(addr + i)->data[(addr + i) & 4095] = static_cast<const char*>(in)[i];
        return ch::access::ok;
    }
    std::string get(u32 addr, u32 n)
    {
        std::string out(n, '\0');
        for (u32 i = 0; i < n; ++i)
            if (page* p = find(addr + i)) out[i] = p->data[(addr + i) & 4095];
        return out;
    }
    void put(u32 addr, std::string_view b)
    {
        for (u32 i = 0; i < b.size(); ++i)
            if (page* p = find(addr + i)) p->data[(addr + i) & 4095] = b[i];
    }
    void put32(u32 addr, u32 v) { put(addr, bytes({int(v >> 24), int((v >> 16) & 255), int((v >> 8) & 255), int(v & 255)})); }
};

struct budget
{
    u32 left = 1u << 30;
    bool take()
    {
        if (!left) return false;
        --left;
        return true;
    }
};

ch::run_result run_all(const ch::program& p, fake_memory& m)
{
    ch::cursor c;
    budget b;
    return ch::run(p, c, m, b);
}

// ---------------------------------------------------------------------------------------------------------------
void test_parser()
{
    {
        const auto f = ch::parse_text("[Infinite health]\n0 0031A2B0 60000000\n");
        CHECK(f.cheats.size() == 1 && f.notes.empty());
        const ch::cheat& c = f.cheats[0];
        CHECK(c.name == "Infinite health" && c.problems.empty() && c.line == 1);
        CHECK(c.prog.ops.size() == 1 && c.prog.ops[0].kind == ch::op_kind::write && c.prog.ops[0].addr == 0x0031A2B0);
        CHECK(c.prog.ops[0].bytes == bytes({0x60, 0, 0, 0}) && c.prog.ops[0].line == 2 && c.prog.plain);
        CHECK(c.lines.size() == 1 && c.lines[0] == "0 0031A2B0 60000000");
    }
    // case, tabs, CRLF, BOM give the same op and the same key
    {
        const auto a = ch::parse_text("[A]\n0 0031A2B0 60000000\n");
        const auto b = ch::parse_text("\xEF\xBB\xBF[A]\r\n0\t0031a2b0\t\t60000000  \r\n");
        const auto c = ch::parse_text("[A]\n0   31a2b0    60000000\n");
        CHECK(b.cheats.size() == 1 && b.cheats[0].prog.ops.size() == 1 && b.cheats[0].prog.ops[0].addr == 0x31A2B0);
        CHECK(a.cheats[0].key == b.cheats[0].key && a.cheats[0].key == c.cheats[0].key);
        CHECK(b.cheats[0].prog.ops[0].bytes == a.cheats[0].prog.ops[0].bytes);
    }
    // comments, directives, titles
    {
        const auto f = ch::parse_text(
            "// a list\n"
            "; serial: BLUS30443\n"
            "; version: 01.02\n"
            "; hash: PPU-0123456789ABCDEF0123456789abcdef01234567\n"
            "; a plain comment\n"
            "#Max money\n"
            "; mode: constant 250\n"
            "; author: Someone\n"
            "; notes: keep it low\n"
            "0 01234560 05F5E0FF\n"
            "\n"
            "[Infinite health]\n"
            "; mode: once\n"
            "0 0031A2B0 60000000\n"
            "Bare title\n"
            "0 00A00000 01\n"
            "\n"
            "0 00A00004 02\n");
        CHECK(f.serial == "BLUS30443" && f.version == "01.02");
        CHECK(f.cheats.size() == 3);
        const ch::cheat* money = named(f, "Max money");
        CHECK(money && money->wanted == ch::mode::constant && money->period_ms == 250 && money->author == "Someone" && money->notes == "keep it low");
        CHECK(money && money->hash == "PPU-0123456789abcdef0123456789abcdef01234567" && money->version == "01.02");
        const ch::cheat* health = named(f, "Infinite health");
        CHECK(health && health->wanted == ch::mode::once);
        // a blank line does not end a cheat
        const ch::cheat* bare = named(f, "Bare title");
        CHECK(bare && bare->prog.ops.size() == 2 && bare->lines.size() == 2);
    }
    // a bad hash is a note; "; file:" resets the file directives
    {
        const auto f = ch::parse_text("; hash: PPU-12\n; version: 1.00\n[A]\n0 00A00000 01\n; file: other.txt\n[B]\n0 00A00000 02\n");
        CHECK(has_note(f.notes, S::CHT_ERR_HASH));
        CHECK(f.cheats.size() == 2 && f.cheats[0].version == "1.00" && f.cheats[1].version.empty() && f.cheats[0].hash.empty());
    }
    // .ncl-like blocks and constant flags; codes before a title; a title without codes
    {
        const auto f = ch::parse_text(
            "0 00A00000 01\n"
            "{\n"
            "Infinite ammo\n"
            "0 00A00010 63\n"
            "1\n"
            "}\n"
            "{\n"
            "One shot\n"
            "0 00A00020 01\n"
            "0\n"
            "}\n"
            "Empty one\n"
            "[Another]\n"
            "0 00A00030 01\n");
        CHECK(f.cheats.size() == 4);
        CHECK(f.cheats[0].name == "Cheat 1" && f.cheats[0].prog.ops.size() == 1);
        CHECK(named(f, "Infinite ammo") && named(f, "Infinite ammo")->wanted == ch::mode::constant);
        CHECK(named(f, "One shot") && named(f, "One shot")->wanted == ch::mode::once);
        CHECK(!named(f, "Empty one") && has_note(f.notes, S::CHT_ERR_SEM_CODIGO));
    }
    // a game header and metadata lines
    {
        const auto f = ch::parse_text("BLUS30443 v1.02\nAuthor: Bob\n[A]\n0 00A00000 01\nNotes: hold L1\nAuthor: Alice\n[B]\n0 00A00004 01\n");
        CHECK(f.serial == "BLUS30443" && f.version == "1.02" && f.cheats.size() == 2);
        CHECK(f.cheats[0].author == "Bob" && f.cheats[1].author == "Alice" && f.cheats[0].notes == "hold L1");
        const auto g = ch::parse_text("Game: Demon's Souls (BLES00932)\nTitle ID: BLUS30443\n[A]\n0 00A00000 01\n");
        CHECK(g.serial == "BLES00932" && g.game == "Demon's Souls (BLES00932)");
        const auto h = ch::parse_text("Title ID: BLUS30443\n[A]\n0 00A00000 01\n");
        CHECK(h.serial == "BLUS30443");
    }
    // titles that start like a code line stay titles
    {
        const auto f = ch::parse_text("A Bad Day\n0 00A00000 01\n1 Hit Kill\n0 00A00004 01\n2 Player mode\n0 00A00008 01\n");
        CHECK(f.cheats.size() == 3 && named(f, "A Bad Day") && named(f, "1 Hit Kill") && named(f, "2 Player mode"));
    }
    // odd hex, long values, joined tokens, trailing comments
    {
        const auto f = ch::parse_text("[A]\n0 00A00000 123\n[B]\n0 00A00000 0000 270F\n[C]\n0 00A1B2C4 0000270F // money\n");
        const ch::cheat* a = named(f, "A");
        CHECK(a && a->prog.ops.size() == 1 && a->prog.ops[0].bytes == bytes({0x01, 0x23}) && has_problem(*a, S::CHT_NOTA_HEX_IMPAR, 2) && !a->fatal());
        CHECK(named(f, "B") && named(f, "B")->prog.ops[0].bytes == bytes({0, 0, 0x27, 0x0F}));
        CHECK(named(f, "C") && named(f, "C")->prog.ops[0].bytes == bytes({0, 0, 0x27, 0x0F}));
        const std::string big(8192, 'A');
        const auto g = ch::parse_text("[Big]\n0 00A00000 " + big + "\n[Huge]\n0 00A00000 " + big + "AA\n");
        CHECK(named(g, "Big") && named(g, "Big")->prog.ops.size() == 1 && named(g, "Big")->prog.ops[0].bytes.size() == 4096);
        CHECK(named(g, "Huge") && has_problem(*named(g, "Huge"), S::CHT_ERR_GRANDE, 4) && named(g, "Huge")->fatal());
    }
    // text: quoted with escapes, raw rest of the line, bad quotes
    {
        const auto f = ch::parse_text("[Q]\n1 00A00000 \"Hi \\\"you\\\"\\n\"\n[R]\n1 00A00100 Hello World  \n[Bad]\n1 00A00200 \"open\n[Bad2]\n1 00A00200 \"x\\q\"\n");
        CHECK(named(f, "Q") && named(f, "Q")->prog.ops.size() == 1 && named(f, "Q")->prog.ops[0].bytes == "Hi \"you\"\n");
        CHECK(named(f, "R") && named(f, "R")->prog.ops[0].bytes == "Hello World");
        CHECK(named(f, "Bad") && has_problem(*named(f, "Bad"), S::CHT_ERR_TEXTO, 6));
        CHECK(named(f, "Bad2") && has_problem(*named(f, "Bad2"), S::CHT_ERR_TEXTO));
    }
    // floats
    {
        const auto f = ch::parse_text("[F1]\n2 00A1B2C0 1.5\n[F2]\n2 00A1B2C0 -0.25\n[F3]\n2 00A1B2C0 1e3\n[F4]\n2 00A1B2C0 1,5\n[F5]\n2 00A1B2C0 3FC00000\n[F6]\n2 00A1B2C0 .5\n[F7]\n2 00A1B2C0 1e40\n");
        CHECK(named(f, "F1") && named(f, "F1")->prog.ops[0].bytes == bytes({0x3F, 0xC0, 0, 0}));
        CHECK(named(f, "F2") && named(f, "F2")->prog.ops[0].bytes == bytes({0xBE, 0x80, 0, 0}));
        CHECK(named(f, "F3") && named(f, "F3")->prog.ops[0].bytes == bytes({0x44, 0x7A, 0, 0}));
        CHECK(named(f, "F4") && has_problem(*named(f, "F4"), S::CHT_ERR_FLOAT, 8));
        CHECK(named(f, "F5") && named(f, "F5")->prog.ops[0].bytes == bytes({0x3F, 0xC0, 0, 0}));
        CHECK(named(f, "F6") && named(f, "F6")->prog.ops[0].bytes == bytes({0x3F, 0, 0, 0}));
        CHECK(named(f, "F7") && has_problem(*named(f, "F7"), S::CHT_ERR_FLOAT));
    }
    // addresses: 9-16 digits fitting 32 bits are taken; larger ones are the lv2 kernel; more digits are not hex
    {
        const auto f = ch::parse_text("[A]\n0 000031A2B0 01\n[K]\n0 8000000000001234 01\n[H]\n0 80000000000012345 01\n[Z]\n0 00A0G000 01\n");
        CHECK(named(f, "A") && named(f, "A")->prog.ops.size() == 1 && named(f, "A")->prog.ops[0].addr == 0x31A2B0);
        CHECK(named(f, "K") && has_problem(*named(f, "K"), S::CHT_ERR_KERNEL, 4) && named(f, "K")->fatal());
        CHECK(named(f, "H") && has_problem(*named(f, "H"), S::CHT_ERR_HEX, 6));
        // a non-hex address makes the line a title
        CHECK(!named(f, "Z") || named(f, "Z")->prog.ops.empty() || has_problem(*named(f, "Z"), S::CHT_ERR_SEM_CODIGO));
    }
    // regions
    {
        const auto f = ch::parse_text("[Stack]\n0 D0000000 01\n[Low]\n0 00000100 01\n[Video]\n0 C0000000 01\n[Edge]\n0 BFFFFFFF 0102\n[Top]\n0 BFFFFFFE 0102\n");
        CHECK(has_problem(*named(f, "Stack"), S::CHT_ERR_REGIAO, 2));
        CHECK(has_problem(*named(f, "Low"), S::CHT_ERR_REGIAO));
        CHECK(has_problem(*named(f, "Video"), S::CHT_ERR_REGIAO));
        CHECK(has_problem(*named(f, "Edge"), S::CHT_ERR_REGIAO));
        CHECK(!named(f, "Top")->fatal());
    }
    // unknown and not yet supported types
    {
        const auto f = ch::parse_text("[U]\n3 00A00000 01\n[P]\n6 00A00000 00000010\n0 00000004 00000001\n[D]\nD 00A00000 01\n0 00A00004 02\n[M]\n0 00A00000 01\n7 00A00000 01\n");
        const ch::cheat* u = named(f, "U");
        CHECK(u && has_problem(*u, S::CHT_ERR_TIPO_DESCONHECIDO, 2) && u->fatal() && u->problems[0].arg1 == "3");
        const ch::cheat* p = named(f, "P");
        CHECK(p && p->unsupported && has_problem(*p, S::CHT_ERR_TIPO_NAO_SUPORTADO, 4) && p->fatal() && !p->prog.plain);
        CHECK(p && p->prog.ops.size() == 2 && p->prog.ops[0].kind == ch::op_kind::pointer && p->prog.ops[0].aux == 0x10 && p->prog.ops[1].relative &&
            p->prog.ops[1].addr == 4);
        const ch::cheat* d = named(f, "D");
        CHECK(d && d->unsupported && d->prog.ops.size() == 2 && d->prog.ops[0].kind == ch::op_kind::if_equal && d->prog.ops[0].count == 1);
        CHECK(named(f, "M") && has_problem(*named(f, "M"), S::CHT_ERR_TIPO_DESCONHECIDO, 11));
        // the problem text in pt-BR names the line and the type
        CHECK(ch::problem_text(u->problems[0]).starts_with("Linha 2: tipo de código 3 desconhecido"));
        CHECK(ch::problem_text(p->problems[0]) == "Linha 4: código tipo 6 ainda não é suportado no PS4");
    }
    // a missing second line, bad second lines
    {
        const auto f = ch::parse_text("[S]\n4 00F00000 00000063\n[S2]\n4 00F00000 00000063\n4 0032000G 00000000\n[C]\nA 00A00000 00000010\n");
        CHECK(has_problem(*named(f, "S"), S::CHT_ERR_LINHA_FALTANDO, 2));
        CHECK(named(f, "S2")->fatal());
        CHECK(has_problem(*named(f, "C"), S::CHT_ERR_LINHA_FALTANDO));
    }
}

// ---------------------------------------------------------------------------------------------------------------
void test_converter()
{
    const std::vector<ch::segment> segs = {{0x00010000, 0x00350000, true}, {0x00350000, 0x00400000, false}, {0x01000000, 0x01300000, false}};
    // worked examples
    {
        const auto f = ch::parse_text(
            "[Infinite health]\n0 0031A2B0 60000000\n"
            "[Max money]\n0 01234560 05F5E0FF\n"
            "[All items x99]\n4 00F00000 00000063\n4 00320004 00000000\n"
            "[Speed]\n2 00A1B2C0 1.5\n");
        CHECK(f.cheats.size() == 4);
        const ch::cheat& health = *named(f, "Infinite health");
        const ch::plan h = ch::make_plan(health.prog, ch::mode::automatic, &segs);
        CHECK(h.resolved == ch::mode::boot && h.boot.size() == 1 && h.live.ops.empty() && h.code_part);
        CHECK(h.boot[0].addr == 0x0031A2B0 && h.boot[0].bytes == bytes({0x60, 0, 0, 0}));
        const ch::cheat& money = *named(f, "Max money");
        const ch::plan m = ch::make_plan(money.prog, ch::mode::automatic, &segs);
        CHECK(m.resolved == ch::mode::constant && m.boot.empty() && m.live.ops.size() == 1 && !m.code_part);
        CHECK(m.live.ops[0].addr == 0x01234560 && m.live.ops[0].bytes == bytes({0x05, 0xF5, 0xE0, 0xFF}));
        const ch::cheat& items = *named(f, "All items x99");
        CHECK(items.prog.ops.size() == 1 && items.lines.size() == 2);
        const ch::op& s = items.prog.ops[0];
        CHECK(s.kind == ch::op_kind::write_series && s.addr == 0x00F00000 && s.width == 4 && s.bytes == bytes({0, 0, 0, 0x63}) && s.count == 0x32 &&
            s.addr_step == 4 && s.value_step == 0);
        const ch::cheat& speed = *named(f, "Speed");
        CHECK(speed.prog.ops[0].addr == 0x00A1B2C0 && speed.prog.ops[0].bytes == bytes({0x3F, 0xC0, 0, 0}));
        // heap (outside the executable) data
        const ch::plan sp = ch::make_plan(speed.prog, ch::mode::automatic, &segs);
        CHECK(sp.resolved == ch::mode::constant && sp.live.ops.size() == 1);
        // no segments yet: the XMB shows the wanted mode
        CHECK(ch::make_plan(health.prog, ch::mode::automatic, nullptr).resolved == ch::mode::automatic);
        CHECK(ch::make_plan(money.prog, ch::mode::once, nullptr).resolved == ch::mode::once);
    }
    // mixed cheats split: code at load, data live; a series straddling code and data becomes single writes
    {
        const auto f = ch::parse_text("[Mixed]\n0 0031A2B0 60000000\n0 01234560 00000001\n[Series]\n4 0034FFF8 00000001\n4 00040004 00000001\n");
        const ch::plan mx = ch::make_plan(f.cheats[0].prog, ch::mode::automatic, &segs);
        CHECK(mx.resolved == ch::mode::constant && mx.boot.size() == 1 && mx.live.ops.size() == 1 && mx.code_part);
        const ch::plan sr = ch::make_plan(f.cheats[1].prog, ch::mode::automatic, &segs);
        CHECK(sr.boot.size() == 2 && sr.live.ops.size() == 2 && sr.boot[0].addr == 0x0034FFF8 && sr.boot[1].addr == 0x0034FFFC);
        CHECK(sr.live.ops[0].addr == 0x00350000 && sr.live.ops[0].bytes == bytes({0, 0, 0, 3}) && sr.live.ops[1].bytes == bytes({0, 0, 0, 4}));
        // once / constant with only code ops are written at load
        CHECK(ch::make_plan(named(ch::parse_text("[C]\n0 0031A2B0 60000000\n"), "C")->prog, ch::mode::once, &segs).resolved == ch::mode::boot);
        // boot chosen for data inside the executable: written at load; outside it: a note, the op is dropped
        const auto g = ch::parse_text("[D]\n0 00360000 01\n0 00A00000 01\n");
        const ch::plan bd = ch::make_plan(g.cheats[0].prog, ch::mode::boot, &segs);
        CHECK(bd.resolved == ch::mode::boot && bd.boot.size() == 1 && bd.live.ops.empty() && bd.problems.size() == 1 &&
            bd.problems[0].id == S::CHT_ERR_ENDERECO_FORA_EXEC && !bd.problems[0].fatal && bd.problems[0].line == 3);
    }
    // dynamic programs run live; a dynamic op on code is invalid; boot is refused with a note
    {
        const auto f = ch::parse_text("[Ptr]\n6 01234560 00000010\n0 00000004 00000001\n[Find]\nB 00010000 00020000\nB 60000000 38600001\n[Copy]\nA 01234560 00000010\nA 00300000 00000000\n");
        const ch::plan p = ch::make_plan(named(f, "Ptr")->prog, ch::mode::boot, &segs);
        CHECK(p.resolved == ch::mode::constant && p.live.ops.size() == 2 && p.boot.empty() && p.problems.size() == 1 && p.problems[0].id == S::CHT_ERR_INICIO_DINAMICO);
        const ch::plan fr = ch::make_plan(named(f, "Find")->prog, ch::mode::automatic, &segs);
        CHECK(fr.resolved == ch::mode::once && fr.problems.size() == 1 && fr.problems[0].id == S::CHT_ERR_CODIGO_DINAMICO && fr.problems[0].fatal);
        const ch::plan cp = ch::make_plan(named(f, "Copy")->prog, ch::mode::automatic, &segs);
        CHECK(cp.problems.size() == 1 && cp.problems[0].id == S::CHT_ERR_CODIGO_DINAMICO);
        CHECK(named(f, "Find")->prog.find && named(f, "Find")->prog.ops[0].bytes == bytes({0x60, 0, 0, 0}) && named(f, "Find")->prog.ops[0].bytes2 == bytes({0x38, 0x60, 0, 1}));
    }
    // caps
    {
        const auto f = ch::parse_text("[S]\n4 00F00000 00000063\n4 40010004 00000000\n[C]\nA 00A00000 00010001\nA 00B00000 00000000\n[F]\nB 00A00000 01A00001\nB 01 02\n");
        CHECK(has_problem(*named(f, "S"), S::CHT_ERR_GRANDE, 2));
        CHECK(has_problem(*named(f, "C"), S::CHT_ERR_GRANDE));
        CHECK(has_problem(*named(f, "F"), S::CHT_ERR_GRANDE));
        std::string deep = "[Deep]\n";
        for (int i = 0; i < 9; ++i) deep += "6 00A00000 00000004\n";
        deep += "0 00000000 01\n";
        const auto d = ch::parse_text(deep);
        CHECK(has_problem(d.cheats[0], S::CHT_ERR_PONTEIRO_FUNDO, 10));
        std::string many_lines = "[Long]\n";
        for (int i = 0; i < 4100; ++i) many_lines += "0 00A00000 01\n";
        const auto l = ch::parse_text(many_lines);
        CHECK(l.cheats.size() == 1 && l.cheats[0].prog.ops.size() == ch::max_code_lines && has_problem(l.cheats[0], S::CHT_ERR_LINHAS_DEMAIS) && !l.cheats[0].fatal());
        CHECK(l.ignored_count == 4 && has_note(l.notes, S::CHT_NOTA_LINHAS_IGNORADAS));
        std::string many = "";
        for (int i = 0; i < 520; ++i) many += "[C" + std::to_string(i) + "]\n0 00A00000 " + ch::detail::hex_upper(u64(i), 8) + "\n";
        const auto m = ch::parse_text(many, "BLUS30443.txt");
        CHECK(m.cheats.size() == ch::max_cheats && has_note(m.notes, S::CHT_ERR_MUITOS));
        CHECK(ch::parse_text(std::string(ch::file_limit + 1, ' '), "x.txt").notes[0].id == S::CHT_ERR_ARQUIVO_GRANDE);
    }
}

// ---------------------------------------------------------------------------------------------------------------
void test_interpreter()
{
    // writes, series wrap
    {
        fake_memory m;
        m.map(0x00A00000, 0x1000);
        const auto f = ch::parse_text("[S]\n0 00A00000 11223344\n4 00A00010 FE\n4 00040001 01\n");
        const ch::run_result r = run_all(f.cheats[0].prog, m);
        CHECK(r.status == ch::run_status::done && r.writes == 5);
        CHECK(m.get(0x00A00000, 4) == bytes({0x11, 0x22, 0x33, 0x44}) && m.get(0x00A00010, 4) == bytes({0xFE, 0xFF, 0x00, 0x01}));
    }
    // pointer chains
    {
        fake_memory m;
        m.map(0x00100000, 0x1000);
        m.map(0x00200000, 0x1000);
        m.map(0x00300000, 0x1000);
        m.put32(0x00100000, 0x00200000);
        m.put32(0x00200010 + 8, 0x00300000);
        const auto one = ch::parse_text("[P]\n6 00100000 00000010\n0 00000004 0000002A\n");
        CHECK(run_all(one.cheats[0].prog, m).status == ch::run_status::done && m.get(0x00200014, 4) == bytes({0, 0, 0, 0x2A}));
        const auto two = ch::parse_text("[P2]\n6 00100000 00000010\n6 00000008 00000020\n0 00000000 07\n");
        CHECK(two.cheats[0].prog.ops.size() == 3 && two.cheats[0].prog.ops[1].relative);
        CHECK(run_all(two.cheats[0].prog, m).status == ch::run_status::done && m.get(0x00300020, 1) == bytes({7}));
        // null, unmapped, past 4 GiB
        m.put32(0x00100000, 0);
        CHECK(run_all(one.cheats[0].prog, m).status == ch::run_status::not_ready);
        m.put32(0x00100000, 0x00900000);
        CHECK(run_all(one.cheats[0].prog, m).status == ch::run_status::failed);
        m.put32(0x00100000, 0xFFFFFFF8);
        CHECK(run_all(one.cheats[0].prog, m).status == ch::run_status::not_ready);
        const auto unmapped = ch::parse_text("[U]\n6 00500000 00000000\n0 00000000 01\n");
        CHECK(run_all(unmapped.cheats[0].prog, m).status == ch::run_status::not_ready);
    }
    // D / E guards
    {
        fake_memory m;
        m.map(0x00A00000, 0x1000);
        m.put(0x00A00000, bytes({0x12, 0x34}));
        const auto f = ch::parse_text("[Eq]\nD 00A00000 1234\n0 00A00010 01\n0 00A00011 02\n[Ne]\nD 00A00000 1235\n0 00A00020 01\n0 00A00021 02\n[Mask]\nE 00A00000 1030\n0 00A00030 01\n[NoMask]\nE 00A00000 0101\n0 00A00040 01\n");
        CHECK(run_all(named(f, "Eq")->prog, m).status == ch::run_status::done && m.get(0x00A00010, 2) == bytes({1, 2}));
        CHECK(run_all(named(f, "Ne")->prog, m).status == ch::run_status::done && m.get(0x00A00020, 2) == bytes({0, 2}));
        CHECK(run_all(named(f, "Mask")->prog, m).status == ch::run_status::done && m.get(0x00A00030, 1) == bytes({1}));
        CHECK(run_all(named(f, "NoMask")->prog, m).status == ch::run_status::done && m.get(0x00A00040, 1) == bytes({0}));
    }
    // copy with overlap, find-replace (first match)
    {
        fake_memory m;
        m.map(0x00A00000, 0x3000);
        m.put(0x00A00000, bytes({1, 2, 3, 4, 5, 6, 7, 8}));
        const auto f = ch::parse_text("[Copy]\nA 00A00000 00000008\nA 00A00004 00000000\n");
        CHECK(run_all(f.cheats[0].prog, m).status == ch::run_status::done && m.get(0x00A00000, 12) == bytes({1, 2, 3, 4, 1, 2, 3, 4, 5, 6, 7, 8}));
        m.put(0x00A01FFE, bytes({0xAA, 0xBB, 0xCC}));
        m.put(0x00A02100, bytes({0xAA, 0xBB, 0xCC}));
        const auto g = ch::parse_text("[Find]\nB 00A00010 00A03000\nB AABBCC 010203\n");
        CHECK(run_all(g.cheats[0].prog, m).status == ch::run_status::done);
        CHECK(m.get(0x00A01FFE, 3) == bytes({1, 2, 3}) && m.get(0x00A02100, 3) == bytes({0xAA, 0xBB, 0xCC}));
    }
    // budget: a series split over passes resumes where it stopped
    {
        fake_memory m;
        m.map(0x00A00000, 0x1000);
        const auto f = ch::parse_text("[S]\n4 00A00000 00\n4 00640001 01\n0 00A00200 FF\n");
        ch::cursor c;
        int passes = 0;
        ch::run_result r;
        do
        {
            budget b{30};
            r = ch::run(f.cheats[0].prog, c, m, b);
            ++passes;
        } while (r.status == ch::run_status::budget && passes < 10);
        CHECK(r.status == ch::run_status::done && passes == 4 && m.writes == 101);
        CHECK(m.get(0x00A00000, 1) == bytes({0}) && m.get(0x00A00063, 1) == bytes({0x63}) && m.get(0x00A00200, 1) == bytes({0xFF}));
        CHECK(c.pc == 0 && c.sub == 0);
    }
    // targets: unmapped, code, refused, read-only
    {
        fake_memory m;
        m.map(0x00010000, 0x1000, true, true);
        m.map(0x00A00000, 0x1000, false);
        m.map(0x40000000, 0x1000);
        m.refused.push_back({0x40000000, 0x40001000});
        const auto f = ch::parse_text("[U]\n0 00B00000 01\n[C]\n0 00010000 01\n[R]\n0 40000000 01\n[O]\n0 00A00000 01\n");
        const ch::run_result u = run_all(named(f, "U")->prog, m);
        CHECK(u.status == ch::run_status::failed && u.addr == 0x00B00000 && u.line == 2);
        CHECK(run_all(named(f, "C")->prog, m).status == ch::run_status::code);
        CHECK(run_all(named(f, "R")->prog, m).status == ch::run_status::refused);
        CHECK(run_all(named(f, "O")->prog, m).status == ch::run_status::failed);
    }
    // scripts
    {
        fake_memory m;
        m.map(0x01A2B000, 0x1000);
        m.map(0x00200000, 0x1000);
        m.put32(0x01A2B3C4, 0x00200000);
        m.put32(0x00200010, 0x00200100);
        u64 out = 0;
        const auto read = [&](u32 a, u32& v)
        {
            char b[4];
            if (m.read(a, b, 4) != ch::access::ok) return false;
            v = static_cast<u32>(ch::detail::read_be(b, 4));
            return true;
        };
        CHECK(ch::detail::eval_script("[$]+16", 0x01A2B3C4, read, out, 0) && out == 0x00200010);
        CHECK(ch::detail::eval_script("[[$]+16]+4", 0x01A2B3C4, read, out, 0) && out == 0x00200104);
        CHECK(ch::detail::eval_script("$ - 4", 0x01A2B3C4, read, out, 0) && out == 0x01A2B3C0);
        CHECK(ch::detail::eval_script("100 200", 0, read, out, 0) && out == 200);
        CHECK(!ch::detail::eval_script("$-4294967295-5", 0x10, read, out, 0));
        CHECK(!ch::detail::eval_script("[16]", 0, read, out, 0));
        CHECK(ch::check_script("[$]+16") && ch::check_script("[[$]+16]+4") && ch::check_script("$-70000"));
        CHECK(!ch::check_script("0x10") && !ch::check_script("[$") && !ch::check_script("$]") && !ch::check_script("1234567890123456789012345"));
        CHECK(!ch::check_script("[[[[[[[[[$]]]]]]]]]") && ch::check_script("[[[[[[[[$]]]]]]]]"));
    }
}

// ---------------------------------------------------------------------------------------------------------------
void test_pc_formats()
{
    // block form, as the PC dialog's emitter writes it
    {
        const auto f = ch::parse_cheats_yml(
            "Demon's Souls:\n"
            "  0x1a2b3c4:\n"
            "    - HP\n"
            "    - Signed 32 bits\n"
            "    - \"[$]+16\"\n"
            "  0x2000:\n"
            "    - \"Money: max\"\n"
            "    - Unsigned 32 bits\n"
            "    - \"\"\n"
            "\"Game: Subtitle\":\n"
            "  0x10000: [Speed, Float 32 bits, '']\n"
            "  65536: ['It''s', unsigned 8 bits, \"\"]  # a comment\n"
            "'Third':\n"
            "  0x10004:\n"
            "  - A\n"
            "  - Signed 64 bits\n"
            "  - ''\n");
        CHECK(f.cheats.size() == 5 && f.notes.empty());
        const ch::cheat* hp = named(f, "HP");
        CHECK(hp && hp->pc && hp->pc_info.game == "Demon's Souls" && hp->pc_info.offset == 0x1a2b3c4 && hp->pc_info.type == ch::pc_type::s32_ &&
            hp->pc_info.script == "[$]+16" && !hp->prog.plain && hp->problems.empty());
        CHECK(named(f, "Money: max") && named(f, "Money: max")->pc_info.offset == 0x2000 && named(f, "Money: max")->prog.plain);
        CHECK(named(f, "Speed") && named(f, "Speed")->pc_info.game == "Game: Subtitle" && named(f, "Speed")->pc_info.type == ch::pc_type::f32_);
        CHECK(named(f, "It's") && named(f, "It's")->pc_info.offset == 65536 && named(f, "It's")->pc_info.type == ch::pc_type::u8_);
        CHECK(named(f, "A") && named(f, "A")->pc_info.game == "Third" && named(f, "A")->pc_info.type == ch::pc_type::s64_);
        CHECK(hp && hp->lines[0] == "0x01a2b3c4 Signed 32 bits [$]+16");
        // the value typed on the console makes the program: the script then a write at its result
        std::string b, canonical;
        CHECK(ch::parse_pc_value(ch::pc_type::s32_, "999", b, canonical) && b == bytes({0, 0, 3, 0xE7}));
        const ch::program p = ch::pc_program(hp->pc_info, b, 2);
        CHECK(p.ops.size() == 2 && p.ops[0].kind == ch::op_kind::script && p.ops[0].addr == 0x1a2b3c4 && p.ops[1].relative && p.ops[1].addr == 0 && !p.plain);
        fake_memory m;
        m.map(0x01A2B000, 0x1000);
        m.map(0x00200000, 0x1000);
        m.put32(0x01A2B3C4, 0x00200000);
        CHECK(run_all(p, m).status == ch::run_status::done && m.get(0x00200010, 4) == bytes({0, 0, 3, 0xE7}));
    }
    // bad entries are notes, never exceptions
    {
        const auto f = ch::parse_cheats_yml(
            "Game:\n"
            "  0x10: [A, Signed 33 bits, \"\"]\n"
            "  0x14: [B, Signed 32 bits]\n"
            "  zz: [C, Signed 32 bits, \"\"]\n"
            "  0x18: [D, Signed 32 bits, \"0x10\"]\n"
            "  0x1C:\n"
            "    - E\n"
            "\tbad: tab\n"
            "  0x10020: [F, Signed 32 bits, \"\"]\n"
            "  0x20: [Low, Signed 32 bits, \"\"]\n"
            "  0x24: [G, Signed 32 bits, \"\"\n"
            "Top level scalar: 3\n");
        CHECK(has_note(f.notes, S::CHT_ERR_YML_TIPO) && has_note(f.notes, S::CHT_ERR_YML_CAMPOS) && has_note(f.notes, S::CHT_ERR_ENDERECO) && has_note(f.notes, S::CHT_ERR_YML_LINHA));
        CHECK(named(f, "D") && has_problem(*named(f, "D"), S::CHT_ERR_SCRIPT) && named(f, "D")->fatal());
        CHECK(named(f, "F") && !named(f, "F")->fatal());
        CHECK(!named(f, "A") && !named(f, "B") && !named(f, "C"));
        // offset 0x20 is below guest memory: refused for a script-less entry
        CHECK(named(f, "Low") && has_problem(*named(f, "Low"), S::CHT_ERR_REGIAO, 10) && named(f, "Low")->fatal());
        CHECK(!named(f, "G"));
    }
    // the writer reads back
    {
        const auto f = ch::parse_cheats_yml("Demon's Souls:\n  0x1a2b3c4: [\"HP \\\"x\\\"\", Signed 32 bits, \"[$]+16\"]\n");
        CHECK(f.cheats.size() == 1 && f.cheats[0].name == "HP \"x\"");
        const std::string text = ch::cheats_yml_text("Demon's Souls: Remake", {&f.cheats[0]});
        const auto g = ch::parse_cheats_yml(text);
        CHECK(g.cheats.size() == 1 && g.cheats[0].pc_info.game == "Demon's Souls: Remake" && g.cheats[0].name == "HP \"x\"" && g.cheats[0].key == f.cheats[0].key);
    }
    // the clipboard string
    {
        const auto f = ch::parse_text("[T]\nDemon's Souls@@@HP@@@6@@@27411396@@@[$]+16@@@^^^Demon's Souls@@@MP@@@2@@@1048576@@@@@@\n");
        CHECK(f.cheats.size() == 2 && named(f, "HP") && named(f, "HP")->pc && named(f, "HP")->pc_info.offset == 27411396 && named(f, "HP")->pc_info.type == ch::pc_type::s32_);
        CHECK(named(f, "MP") && named(f, "MP")->pc_info.offset == 0x100000 && named(f, "MP")->pc_info.script.empty() && named(f, "MP")->pc_info.type == ch::pc_type::u32_);
        // the empty title before the clipboard line is dropped with a note
        CHECK(has_note(f.notes, S::CHT_ERR_SEM_CODIGO));
        for (const std::string garbage : {"a@@@b@@@x@@@y@@@", "@@@@@@@@@", "g@@@d@@@9@@@1@@@@@@", "g@@@d@@@1@@@99999999999999999999999@@@@@@", "^^^^^^@@@", "g@@@d@@@1@@@1@@@"})
        {
            const auto g = ch::parse_text(garbage);
            CHECK(g.cheats.size() <= 1);
        }
        CHECK(ch::parse_text("g@@@d@@@9@@@1@@@@@@").cheats.empty());
        CHECK(ch::parse_text("g@@@d@@@1@@@99999999999999999999999@@@@@@").cheats.empty());
    }
    // typed values
    {
        std::string b, c;
        CHECK(ch::parse_pc_value(ch::pc_type::s32_, "-1", b, c) && b == bytes({0xFF, 0xFF, 0xFF, 0xFF}));
        CHECK(!ch::parse_pc_value(ch::pc_type::u8_, "256", b, c) && ch::parse_pc_value(ch::pc_type::u8_, "255", b, c) && b == bytes({0xFF}));
        CHECK(ch::parse_pc_value(ch::pc_type::u16_, "0xFFFF", b, c) && b == bytes({0xFF, 0xFF}));
        CHECK(!ch::parse_pc_value(ch::pc_type::s8_, "-129", b, c) && ch::parse_pc_value(ch::pc_type::s8_, "-128", b, c) && b == bytes({0x80}));
        CHECK(!ch::parse_pc_value(ch::pc_type::s8_, "128", b, c) && !ch::parse_pc_value(ch::pc_type::u32_, "-1", b, c));
        CHECK(ch::parse_pc_value(ch::pc_type::f32_, "1.5", b, c) && b == bytes({0x3F, 0xC0, 0, 0}) && !ch::parse_pc_value(ch::pc_type::f32_, "1,5", b, c));
        CHECK(ch::parse_pc_value(ch::pc_type::u64_, "18446744073709551615", b, c) && b == std::string(8, '\xFF'));
        CHECK(!ch::parse_pc_value(ch::pc_type::u64_, "18446744073709551616", b, c));
        CHECK(ch::parse_pc_value(ch::pc_type::s64_, "-9223372036854775808", b, c) && b == bytes({0x80, 0, 0, 0, 0, 0, 0, 0}));
        CHECK(!ch::parse_pc_value(ch::pc_type::s32_, "12abc", b, c) && !ch::parse_pc_value(ch::pc_type::s32_, "", b, c));
    }
}

// ---------------------------------------------------------------------------------------------------------------
void test_state_and_game()
{
    const std::string text =
        "; serial: BLUS30443\n; version: 01.00\n"
        "[Max money]\n0 01234560 05F5E0FF\n"
        "[Infinite health]\n0 0031A2B0 60000000\n"
        "[Bad]\n3 00A00000 01\n"
        "[Ptr]\n6 01234560 00000010\n0 00000004 00000001\n";
    const std::string yml = "\"Demon's Souls\":\n  0x1a2b3c4: [HP, Signed 32 bits, \"[$]+16\"]\nOther game:\n  0x10000: [X, Unsigned 8 bits, \"\"]\n";
    const std::string user = "[Mine]\n0 00A00000 01\n[Max money copy]\n0 01234560 05f5e0ff\n";
    const std::string seen = "# RPCS3 PS4 cheats seen 1\nPPU-0123456789abcdef0123456789abcdef01234567 01.00 exec=00010000-0034ffff data=00350000-003fffff,01000000-012fffff\n";
    std::vector<ch::source_file> files = {{"BLUS30443_user.txt", user, true, false}, {"BLUS30443.txt", text, false, false}, {"BLUS30443.yml", yml, false, true}};
    ch::game_cheats g = ch::build_game("BLUS30443", "Demon's Souls", "01.02", files, "", seen);
    CHECK(g.rows.size() == 6);
    // the user copy replaces the import (same codes: listed once)
    CHECK(g.rows[0].name == "Mine" && g.rows[0].user && g.rows[1].name == "Max money copy" && g.rows[1].user);
    CHECK(has_note(g.notes, S::CHT_NOTA_DUPLICADO));
    std::size_t health = 99, bad = 99, ptr = 99, hp = 99;
    for (std::size_t i = 0; i < g.rows.size(); ++i)
    {
        if (g.rows[i].name == "Infinite health") health = i;
        if (g.rows[i].name == "Bad") bad = i;
        if (g.rows[i].name == "Ptr") ptr = i;
        if (g.rows[i].name == "HP") hp = i;
    }
    CHECK(health < 6 && bad < 6 && ptr < 6 && hp < 6);
    // nothing is on by default
    CHECK(ch::count_on(g) == 0);
    CHECK(g.rows[health].resolved == ch::mode::boot && g.rows[1].resolved == ch::mode::constant && !g.rows[bad].valid && !g.rows[ptr].valid);
    CHECK(g.rows[hp].pc && g.rows[hp].needs_value && g.rows[hp].value.empty() && g.rows[hp].dynamic);
    // texts (pt-BR before i18n::apply)
    CHECK(ch::row_subtitle(g.rows[1], g) == "Desligado · reescrito a cada 100 ms · 1 linha(s)");
    CHECK(ch::row_subtitle(g.rows[health], g) == "Desligado · no início do jogo (código) · 1 linha(s)");
    CHECK(ch::row_subtitle(g.rows[bad], g).starts_with("Inválido: Linha 8: tipo de código 3 desconhecido"));
    CHECK(ch::row_subtitle(g.rows[hp], g) == "Desligado · reescrito a cada 100 ms · valor: digite com X · 1 linha(s) · ponteiro · RPCS3 de PC: Signed 32 bits");
    const auto header = ch::header_lines(g, false);
    CHECK(header.size() == 1 && header[0] == "Códigos para a versão 01.00; instalada: 01.02 · podem não funcionar");
    // switches
    std::string message;
    ch::edit_files written;
    CHECK(!ch::edit_on(g, bad, true, message, written) && message.starts_with("Não pode ser ligado: Linha 8"));
    CHECK(!ch::edit_on(g, hp, true, message, written) && message == "Digite o valor primeiro (X)");
    CHECK(!ch::edit_value(g, hp, "abc", message, written) && message == "'abc' não é um valor válido para Signed 32 bits");
    CHECK(ch::edit_value(g, hp, "999", message, written) && g.rows[hp].value == "999" && written.state);
    CHECK(ch::edit_on(g, hp, true, message, written) && ch::edit_on(g, 1, true, message, written) && message == "Salvo · vale no próximo início do jogo");
    CHECK(ch::edit_mode(g, 1, ch::mode::constant, 250, message, written) && g.rows[1].period_ms == 250);
    CHECK(!ch::edit_mode(g, 1, ch::mode::constant, 5, message, written) && message == "Intervalo de 16 a 1000 ms");
    CHECK(ch::row_subtitle(g.rows[1], g) == "Ligado · reescrito a cada 250 ms · 1 linha(s)");
    CHECK(ch::header_lines(g, true).size() == 2 && ch::header_lines(g, true).back() == "Cheats podem afetar troféus e placares online (RPCN)");
    CHECK(ch::header_lines(ch::game_cheats{}, true).empty());
    // the state file round trip
    const std::string state = ch::state_text(g);
    CHECK(state.starts_with("# RPCS3 PS4 cheats state 1\n"));
    CHECK(state.find(" = 999\n") != std::string::npos);
    ch::game_cheats again = ch::build_game("BLUS30443", "Demon's Souls", "01.02", files, state, seen);
    CHECK(ch::count_on(again) == 2 && again.rows[1].on && again.rows[1].period_ms == 250 && again.rows[hp].on && again.rows[hp].value == "999");
    CHECK(again.rows[hp].prog.ops.size() == 2 && ch::state_text(again) == state);
    // spacing and case of the codes keep the key; changed codes start off
    {
        std::vector<ch::source_file> f2 = files;
        f2[0].content = "[Mine]\n0 00A00000 01\n[Max money copy]\n0   1234560\t05F5E0FF\n";
        CHECK(ch::count_on(ch::build_game("BLUS30443", "Demon's Souls", "01.02", f2, state, seen)) == 2);
        f2[0].content = "[Mine]\n0 00A00000 01\n[Max money copy]\n0 01234560 05F5E0FE\n";
        ch::game_cheats changed = ch::build_game("BLUS30443", "Demon's Souls", "01.02", f2, state, seen);
        CHECK(!changed.rows[1].on);
    }
    // a state line saying "on" for an invalid cheat stays off
    CHECK(!ch::build_game("BLUS30443", "Demon's Souls", "01.02", files, "on auto 100 " + ch::detail::lower_ascii(ch::detail::hex_upper(g.rows[bad].key, 8)) + " Bad\n", seen).rows[bad].on);
    // the console's own cheats
    CHECK(!ch::edit_add_user(again, "", "0 00A00000 01", message, written) && message == "Digite um nome");
    CHECK(!ch::edit_add_user(again, "New", "0 00A00000 01;3 00A00000 02", message, written) && message.starts_with("Linha 2: tipo de código 3"));
    CHECK(!ch::edit_add_user(again, "New", "0 00A00000 01;hello;0 00A00004 01", message, written) && message.starts_with("Linha 2: formato inválido"));
    CHECK(!ch::edit_add_user(again, "New", "0 00A00000 01", message, written) && message.starts_with("'New' repete"));
    CHECK(!ch::edit_add_user(again, "New", "", message, written) && message == "'New' não tem nenhuma linha de código");
    CHECK(ch::edit_add_user(again, "New", "0 00A00000 02; 1 00A00010 \"a;b\"", message, written) && written.user);
    CHECK(again.rows.back().name == "New" && again.rows.back().lines.size() == 2 && again.rows.back().prog.ops[1].bytes == "a;b");
    CHECK(ch::edit_add_user(again, "Typed PC", "G@@@desc@@@2@@@1048576@@@@@@", message, written) && again.rows.back().pc && again.rows.back().name == "Typed PC");
    CHECK(again.rows.back().code_text.size() == 1 && again.rows.back().code_text[0] == "G@@@Typed PC@@@2@@@1048576@@@@@@");
    CHECK(ch::edit_delete(again, again.rows.size() - 1, message, written));
    const std::size_t added = again.rows.size() - 1;
    const u32 old_key = again.rows[added].key;
    CHECK(ch::edit_rename(again, added, "Renamed", message, written) && again.rows[added].key == old_key);
    CHECK(!ch::edit_codes(again, health, "0 00A00000 05", message, written) && message.starts_with("Só os cheats criados no PS4"));
    CHECK(ch::edit_on(again, added, true, message, written));
    CHECK(ch::edit_codes(again, added, "0 00A00000 07", message, written) && again.rows[added].key != old_key && !again.rows[added].on);
    CHECK(ch::edit_copy(again, health, message, written) && again.rows[health].user && message == "Copiado para meus cheats: Infinite health");
    CHECK(ch::edit_copy(again, hp, message, written) && again.rows[hp].user && again.rows[hp].code_text[0].find("@@@") != std::string::npos);
    CHECK(!ch::edit_delete(again, ptr, message, written));
    CHECK(ch::edit_delete(again, 0, message, written) && message == "Apagado: Mine");
    // the user file reads back with the same cheats and keys
    const std::string mine = ch::user_text(again);
    const auto back = ch::parse_text(mine);
    std::vector<u32> user_keys, back_keys;
    for (const ch::row& r : again.rows)
        if (r.user) user_keys.push_back(r.key);
    for (const ch::cheat& c : back.cheats) back_keys.push_back(c.key);
    CHECK(!user_keys.empty() && user_keys == back_keys && back.notes.empty());
    for (const ch::cheat& c : back.cheats) CHECK(!c.fatal() || c.name == "Ptr");
    const ch::cheat* hp_back = named(back, "HP");
    CHECK(hp_back && hp_back->pc && hp_back->pc_info.script == "[$]+16");
    // seen file
    {
        ch::seen_info s = ch::parse_seen(seen);
        CHECK(s.lines.size() == 1 && s.lines[0].segs.size() == 3 && s.lines[0].segs[0].exec && s.lines[0].segs[0].end == 0x00350000 && s.lines[0].app_version == "01.00");
        CHECK(ch::seen_text(s) == seen);
        CHECK(!ch::seen_update(s, s.lines[0]));
        ch::seen_line other{"PPU-ffffffffffffffffffffffffffffffffffffffff", "", {{0x10000, 0x20000, true}}};
        CHECK(ch::seen_update(s, other) && s.lines.size() == 2 && s.lines[0].hash == other.hash);
        CHECK(ch::parse_seen(ch::seen_text(s)).lines.size() == 2 && ch::parse_seen(ch::seen_text(s)).lines[0].app_version.empty());
        ch::game_cheats two = ch::build_game("BLUS30443", "Demon's Souls", "01.00", files, "", ch::seen_text(s));
        CHECK(ch::header_lines(two, false).size() == 1 && ch::header_lines(two, false)[0].starts_with("2 executáveis vistos"));
        CHECK(ch::parse_seen("PPU-12 x exec=1-2\ngarbage\nPPU-0123456789abcdef0123456789abcdef01234567 1 exec=zz-10\n").lines.empty());
    }
    // the state parser ignores garbage
    CHECK(ch::parse_state("on\nfoo bar\non auto x 12345678 a\non auto 100 1234567 a\non weird 100 12345678 a\n").empty());
    CHECK(ch::parse_state("off once 5 0000abcd  Name with  spaces = 5\n").size() == 1 && ch::parse_state("off once 5 0000abcd  Name\n")[0].period_ms == 16);
}

// ---------------------------------------------------------------------------------------------------------------
void test_import_naming()
{
    CHECK(ch::serial_from_name("BLUS30443.txt") == "BLUS30443");
    CHECK(ch::serial_from_name("blus30443_cheats.ncl") == "BLUS30443");
    CHECK(ch::serial_from_name("BLUS30443 v1.02.txt") == "BLUS30443");
    CHECK(ch::serial_from_name("BLUS304431.txt").empty() && ch::serial_from_name("cheats.yml").empty() && ch::serial_from_name("BLU30443.txt").empty());
    CHECK(ch::import_candidate("a.TXT") && ch::import_candidate("b.ncl") && ch::import_candidate("cheats.yml"));
    CHECK(!ch::import_candidate("._a.txt") && !ch::import_candidate("a.pkg") && !ch::import_candidate(""));
    CHECK(ch::parse_text("; serial: blus30443\n[A]\n0 00A00000 01\n").serial == "BLUS30443");
    CHECK(ch::titles_match("Demon's Souls", "Demon's Souls") && ch::titles_match("DEMONS SOULS", "Demon's Souls") && !ch::titles_match("Demon's Souls 2", "Demon's Souls"));
    CHECK(!ch::titles_match("", "!!"));
    const auto counts = ch::file_counts(ch::parse_text("[A]\n0 00A00000 01\n[B]\n3 00A00000 01\n[C]\n0 D0000000 01\n"));
    CHECK(counts.first == 1 && counts.second == 2);
    // a PC cheats.yml split by title; a single other title is taken (with a note)
    {
        const std::string yml = "Demon's Souls:\n  0x10000: [A, Signed 32 bits, \"\"]\nOther:\n  0x10004: [B, Signed 32 bits, \"\"]\n";
        const ch::game_cheats g = ch::build_game("BLUS30443", "DEMONS SOULS", "", {{"BLUS30443.yml", yml, false, true}}, "", "");
        CHECK(g.rows.size() == 1 && g.rows[0].name == "A");
        const ch::game_cheats h = ch::build_game("BLUS30443", "Nothing alike", "", {{"BLUS30443.yml", "Some title:\n  0x10000: [A, Signed 32 bits, \"\"]\n", false, true}}, "", "");
        CHECK(h.rows.size() == 1 && has_note(h.notes, S::CHT_NOTA_TITULO_DIFERENTE));
        const ch::game_cheats k = ch::build_game("BLUS30443", "Nothing alike", "", {{"BLUS30443.yml", yml, false, true}}, "", "");
        CHECK(k.rows.empty());
    }
}

// ---------------------------------------------------------------------------------------------------------------
void test_robustness()
{
    std::mt19937 rng(0x5eed);
    const std::vector<std::string> pieces = {"0", "1", "2", "4", "6", "A", "B", "D", "E", "F", "3", "9", " ", "\t", "00A00000", "0031A2B0", "FFFFFFFF",
        "8000000000001234", "01234567890ABCDEF0", "\"", "\\", "[", "]", "#", ";", "; mode:", "; hash:", "{", "}", "@@@", "^^^", ":", "-", "1.5", "e9",
        "Author: x", "BLUS30443", "\r", "x", "\xC3\xA9", "\xFF", "0x10", "$", "[$]+16"};
    std::uniform_int_distribution<std::size_t> pick(0, pieces.size() - 1);
    std::uniform_int_distribution<int> count(0, 9), byte(0, 255), coin(0, 3);
    std::string text;
    int rounds = 0;
    const auto hex_run = [&](int digits)
    {
        std::string out;
        for (int k = 0; k < digits; ++k) out += "0123456789ABCDEFabcdefG."[byte(rng) % (coin(rng) ? 16 : 24)];
        return out;
    };
    std::uniform_int_distribution<int> kind(0, 9), digits(1, 18);
    for (int line = 0; line < 100000; ++line)
    {
        const int k_line = kind(rng);
        if (k_line < 5)
        {
            // code-like: a type, an address and a value of random shapes
            text += "0124 6ABDEF3 9"[byte(rng) % 14];
            text += coin(rng) ? " " : "\t";
            text += hex_run(digits(rng) % 10 + 1);
            text += ' ';
            const int v = kind(rng);
            if (v < 6) text += hex_run(digits(rng) * (coin(rng) ? 1 : 3));
            else if (v < 8) text += pieces[pick(rng)] + pieces[pick(rng)];
            else text += "\"" + hex_run(digits(rng)) + (coin(rng) ? "\"" : "\\");
        }
        else if (k_line < 6) text += "[T" + std::to_string(line) + "]";
        else
        {
            const int n = count(rng);
            for (int k = 0; k < n; ++k) text += coin(rng) ? pieces[pick(rng)] : std::string(1, static_cast<char>(byte(rng)));
        }
        text += '\n';
        if (text.size() > 200000)
        {
            const auto f = ch::parse_text(text);
            CHECK(f.cheats.size() <= ch::max_cheats);
            for (const ch::cheat& c : f.cheats)
            {
                CHECK(c.lines.size() <= ch::max_code_lines + 1);
                for (const ch::op& o : c.prog.ops)
                {
                    CHECK(o.bytes.size() <= ch::max_write && o.bytes2.size() <= ch::max_write);
                    if (o.kind == ch::op_kind::write_series) CHECK(u64(o.count) * o.width <= ch::max_series);
                    if (o.kind == ch::op_kind::copy) CHECK(o.aux <= ch::max_series);
                    if (!o.relative && (o.kind == ch::op_kind::write || o.kind == ch::op_kind::write_series))
                    {
                        const auto [b, e] = ch::op_span(o);
                        CHECK(b >= ch::guest_begin && e <= ch::guest_end);
                    }
                }
                // every program runs over an empty memory without trouble
                fake_memory m;
                m.map(0x00A00000, 0x2000);
                ch::cursor cur;
                budget b{512};
                ch::run(c.prog, cur, m, b);
                for (const ch::problem& p : c.problems) (void)ch::problem_text(p);
                (void)ch::make_plan(c.prog, ch::mode::automatic, nullptr);
            }
            const auto y = ch::parse_cheats_yml(text);
            CHECK(y.cheats.size() <= ch::max_cheats);
            (void)ch::parse_state(text);
            (void)ch::parse_seen(text);
            const auto g = ch::build_game("BLUS30443", "T", "1.00", {{"a.txt", text, false, false}, {"a.yml", text, false, true}}, text, text);
            CHECK(g.rows.size() <= ch::max_cheats);
            for (const ch::row& r : g.rows) (void)ch::row_subtitle(r, g);
            text.clear();
            ++rounds;
        }
    }
    CHECK(rounds >= 5);
    // random bytes and random scripts
    for (int round = 0; round < 200; ++round)
    {
        std::string junk(static_cast<std::size_t>(byte(rng)) * 16, '\0');
        for (char& c : junk) c = static_cast<char>(byte(rng));
        (void)ch::parse_text(junk);
        (void)ch::parse_cheats_yml(junk);
        std::string script;
        for (int k = 0; k < count(rng) * 3; ++k) script += "$[]+- 0123456789x"[byte(rng) % 17];
        (void)ch::check_script(script);
        u64 out = 0;
        (void)ch::detail::eval_script(script, 0x10000, [](u32, u32& v) { v = 0x10000; return true; }, out, 0);
    }
}
} // namespace

int main()
{
    test_parser();
    test_converter();
    test_interpreter();
    test_pc_formats();
    test_state_and_game();
    test_import_naming();
    test_robustness();
    std::printf("cheats_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
