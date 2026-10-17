// Host test of utils::ps4::pkg (util/ps4_pkg_check.hpp): PKG header parsing, version comparison, size texts, split
// part names, the package verdicts of the launcher and the version refusal texts; the installer's read policy
// (util/ps4_pkg_io.hpp) on simulated drives, the install failure texts, the texts for a package of the booting disc,
// and the same texts in English, Spanish and Arabic. Build (from the repo root):
//   clang++ -std=c++20 -static -Wall -Wextra -I rpcs3 ps4-port/tests/pkg_check_test.cpp -o pkg_check_test.exe
// With PKG paths as arguments it only prints the launcher's verdict for each file (read-only: 48 bytes and the size).
#include "util/ps4_pkg_check.hpp"
#include "util/ps4_pkg_io.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAILED %s:%d: %s\n", __FILE__, __LINE__, #x); std::exit(1); } } while (0)

namespace
{
namespace pkg = utils::ps4::pkg;
namespace pkg_io = utils::ps4::pkg_io;

// Header of '[God of War - Ascension - 01.12_Online Pass]-BCUS98232.pkg' as RPCS3.log printed it (console 0.36-test2):
// magic "\x7FPKG", pkg_type 0, platform 1, meta_offset 0xc0, meta_count 9, meta_size 0xd0, file_count 0x1c,
// pkg_size 0xb5410930, data_offset 0x190, data_size 0xb5410740.
std::vector<unsigned char> gow_header()
{
    return {0x7f, 0x50, 0x4b, 0x47, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0xc0, 0x00, 0x00, 0x00, 0x09,
        0x00, 0x00, 0x00, 0xd0, 0x00, 0x00, 0x00, 0x1c, 0x00, 0x00, 0x00, 0x00, 0xb5, 0x41, 0x09, 0x30,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x90, 0x00, 0x00, 0x00, 0x00, 0xb5, 0x41, 0x07, 0x40,
        // title_id follows in a real file
        'U', 'P', '9', '0', '0', '0', '-', 'B'};
}

void test_header()
{
    const auto bytes = gow_header();
    const pkg::header_info h = pkg::parse_header(bytes.data(), bytes.size());
    CHECK(h.kind == pkg::magic_kind::ps3);
    CHECK(h.magic == 0x7f504b47);
    CHECK(h.platform == 1);
    CHECK(h.pkg_size == 0xB5410930ull);
    CHECK(h.pkg_size == 3040938288ull);
    CHECK(h.data_offset == 0x190);
    CHECK(h.data_size == 0xb5410740ull);
    CHECK(h.bytes == bytes.size());

    // Exactly 0x30 bytes are enough; one fewer is a short read whatever the magic says.
    CHECK(pkg::parse_header(bytes.data(), pkg::header_size).kind == pkg::magic_kind::ps3);
    const pkg::header_info shorter = pkg::parse_header(bytes.data(), pkg::header_size - 1);
    CHECK(shorter.kind == pkg::magic_kind::short_read);
    CHECK(shorter.bytes == pkg::header_size - 1);
    CHECK(shorter.pkg_size == 0);
    CHECK(pkg::parse_header(nullptr, 64).kind == pkg::magic_kind::short_read);
    CHECK(pkg::parse_header(nullptr, 64).bytes == 0);
    CHECK(pkg::parse_header(bytes.data(), 0).kind == pkg::magic_kind::short_read);

    auto ps4 = bytes;
    std::memcpy(ps4.data(), "\x7F" "CNT", 4);
    CHECK(pkg::parse_header(ps4.data(), ps4.size()).kind == pkg::magic_kind::ps4);

    auto other = bytes;
    std::memcpy(other.data(), "PK\x03\x04", 4); // a zip renamed to .pkg
    const pkg::header_info zip = pkg::parse_header(other.data(), other.size());
    CHECK(zip.kind == pkg::magic_kind::other);
    CHECK(zip.magic == 0x504b0304);

    auto psp = bytes;
    psp[7] = 2;
    CHECK(pkg::parse_header(psp.data(), psp.size()).platform == 2);
}

void test_versions()
{
    CHECK(pkg::version_value("01.12") && *pkg::version_value("01.12") == 1.12);
    CHECK(pkg::version_value("01.00") && *pkg::version_value("01.00") == 1.0);
    CHECK(!pkg::version_value(""));
    CHECK(!pkg::version_value("01.12a"));
    CHECK(!pkg::version_value("v1"));
    CHECK(pkg::compare_versions("01.00", "01.12") == -1);
    CHECK(pkg::compare_versions("01.12", "01.11") == 1);
    CHECK(pkg::compare_versions("01.11", "1.11") == 0);
    CHECK(pkg::compare_versions("01.05", "01.05") == 0);
    CHECK(!pkg::compare_versions("", "01.00"));
    CHECK(!pkg::compare_versions("01.00", "x"));
}

void test_sizes()
{
    CHECK(pkg::size_text(3040938288ull) == "3,0 GB");
    CHECK(pkg::size_text(1250000000ull) == "1,2 GB" || pkg::size_text(1250000000ull) == "1,3 GB");
    CHECK(pkg::size_text(640'000'000) == "640 MB");
    CHECK(pkg::size_text(999'400'000) == "999 MB");
    CHECK(pkg::size_text(12'000) == "12 KB");
    CHECK(pkg::size_text(7) == "7 bytes");
    CHECK(pkg::size_text(0) == "0 bytes");
    CHECK(pkg::sizes_text(1'200'000'000, 3040938288ull) == "1,2 GB de 3,0 GB");
    // Both round to "3,0 GB": megabytes, rounded so the part never reads as the whole
    CHECK(pkg::sizes_text(3'035'000'000, 3040938288ull) == "3035 MB de 3041 MB");
    CHECK(pkg::sizes_text(3040938287ull, 3040938288ull) == "3040 MB de 3041 MB");
    CHECK(pkg::sizes_text(500, 1000) == "500 bytes de 1 KB");
}

void test_part_names()
{
    CHECK(pkg::part_name("/data/rpcs3/pkgs/Game_00.pkg", 1) == "/data/rpcs3/pkgs/Game_01.pkg");
    CHECK(pkg::part_name("/data/rpcs3/pkgs/Game_00.PKG", 12) == "/data/rpcs3/pkgs/Game_12.PKG");
    CHECK(pkg::part_name("/mnt/usb0/PKG/[X]-BLUS30000_00.Pkg", 2) == "/mnt/usb0/PKG/[X]-BLUS30000_02.Pkg");
    CHECK(pkg::part_name("_00.pkg", 1) == "_01.pkg");
    CHECK(pkg::part_name("a.pkg", 1).empty());
    CHECK(pkg::file_name("/data/rpcs3/pkgs/Game_01.pkg") == "Game_01.pkg");
    CHECK(pkg::file_name("Game.pkg") == "Game.pkg");
}

void test_verdicts()
{
    const auto bytes = gow_header();
    const pkg::header_info h = pkg::parse_header(bytes.data(), bytes.size());

    // Complete (and larger, as unpkg accepts): ready, no texts
    pkg::file_facts done{3040938288ull, 5, false, {}};
    CHECK(pkg::judge(h, done).ready());
    CHECK(pkg::judge(h, done).text.empty());
    done.have += 10;
    CHECK(pkg::judge(h, done).ready());
    done.idle_seconds = 100000; // an old complete file is ready too
    CHECK(pkg::judge(h, done).ready());

    // FileZilla still writing (the 0.36-test2 case: X pressed during the upload)
    const pkg::verdict copying = pkg::judge(h, {1'200'000'000, 3, false, {}});
    CHECK(copying.kind == pkg::state::copying);
    CHECK(!copying.ready() && !copying.final());
    CHECK(copying.have == 1'200'000'000 && copying.need == 3040938288ull);
    CHECK(copying.text == "Arquivo incompleto (1,2 GB de 3,0 GB): ainda copiando? Espere o FileZilla terminar");
    CHECK(copying.log == "incomplete file, 1200000000 of 3040938288 bytes, last write 3 s ago (still copying)");
    CHECK(copying.subtitle == "Copiando: 1,2 GB de 3,0 GB · aguarde (a lista atualiza sozinha)");
    // A clock a little behind the file's stamp counts as writing; a stamp hours ahead (local-time exFAT) does not
    CHECK(pkg::judge(h, {1'200'000'000, -40, false, {}}).kind == pkg::state::copying);
    CHECK(pkg::judge(h, {1'200'000'000, -3 * 3600, false, {}}).kind == pkg::state::stopped);
    CHECK(pkg::judge(h, {1'200'000'000, pkg::copying_seconds - 1, false, {}}).kind == pkg::state::copying);

    const pkg::verdict stopped = pkg::judge(h, {1'200'000'000, pkg::copying_seconds, false, {}});
    CHECK(stopped.kind == pkg::state::stopped);
    CHECK(stopped.text == "Arquivo incompleto (1,2 GB de 3,0 GB) e a cópia parou: envie o PKG de novo");
    CHECK(stopped.log == "incomplete file, 1200000000 of 3040938288 bytes, last write 120 s ago (copy stopped)");
    CHECK(stopped.subtitle == "Incompleto: 1,2 GB de 3,0 GB · a cópia parou, envie de novo");

    // Split package: a missing part is named once nothing is written any more; while a part is written it is copying
    const pkg::verdict missing = pkg::judge(h, {2'000'000'000, 600, true, "Game_01.pkg"});
    CHECK(missing.kind == pkg::state::part_missing);
    CHECK(missing.text == "Falta a parte Game_01.pkg do PKG dividido: copie todas as partes para a mesma pasta");
    CHECK(missing.log == "split package: part 'Game_01.pkg' missing (2000000000 of 3040938288 bytes)");
    CHECK(pkg::judge(h, {2'000'000'000, 10, true, "Game_01.pkg"}).kind == pkg::state::copying);
    CHECK(pkg::judge(h, {2'000'000'000, 600, true, {}}).kind == pkg::state::stopped);
    CHECK(pkg::judge(h, {3040938288ull, 600, true, {}}).ready());

    // Header shorter than 0x30 bytes
    const pkg::header_info empty = pkg::parse_header(bytes.data(), 12);
    const pkg::verdict starting = pkg::judge(empty, {12, 1, false, {}});
    CHECK(starting.kind == pkg::state::empty);
    CHECK(starting.text == "PKG ainda vazio: espere a cópia começar e terminar");
    CHECK(starting.log == "incomplete header (12 bytes)");
    CHECK(pkg::judge(empty, {12, 1000, false, {}}).text == "PKG vazio ou cortado: envie o PKG de novo");
    CHECK(pkg::judge(pkg::parse_header(bytes.data(), 0), {0, 0, false, {}}).log == "incomplete header (0 bytes)");

    // PS4 package: final, whatever its size
    auto ps4_bytes = bytes;
    std::memcpy(ps4_bytes.data(), "\x7F" "CNT", 4);
    const pkg::verdict ps4 = pkg::judge(pkg::parse_header(ps4_bytes.data(), ps4_bytes.size()), {100, 1, false, {}});
    CHECK(ps4.kind == pkg::state::ps4 && ps4.final() && !ps4.ready());
    CHECK(ps4.text == "PKG de PS4: instale pelo GoldHEN, não pelo emulador");
    CHECK(ps4.log == "PS4 package");

    // Wrong magic
    auto zip = bytes;
    std::memcpy(zip.data(), "PK\x03\x04", 4);
    const pkg::verdict other = pkg::judge(pkg::parse_header(zip.data(), zip.size()), {5000, 1000, false, {}});
    CHECK(other.kind == pkg::state::not_package && !other.final());
    CHECK(other.log == "not a package (magic 0x504b0304, pkg_size 0xb5410930, data_offset 0x190)");
    // A zero-filled header (a copy whose start was never written) is told apart in the log
    const std::vector<unsigned char> zeros(pkg::header_size, 0);
    CHECK(pkg::judge(pkg::parse_header(zeros.data(), zeros.size()), {4096, 1000, false, {}}).log == "not a package (magic 0x00000000, pkg_size 0x0, data_offset 0x0)");
    CHECK(other.text == "Não é um PKG de PS3 (cabeçalho errado): arquivo corrompido ou renomeado");

    // PSP / PS Vita
    auto psp = bytes;
    psp[7] = 2;
    const pkg::verdict vita = pkg::judge(pkg::parse_header(psp.data(), psp.size()), {3040938288ull, 1000, false, {}});
    CHECK(vita.kind == pkg::state::psp_vita && vita.final());
    CHECK(vita.log == "PSP/PS Vita package");
    CHECK(vita.text == "PKG de PSP/PS Vita: este emulador instala só PKG de PS3");

    // A file that could not be opened (removed between the listing and the check)
    const pkg::verdict gone = pkg::open_failed("Not found");
    CHECK(gone.kind == pkg::state::open_failed && !gone.ready() && !gone.final());
    CHECK(gone.text == "Não foi possível abrir o PKG (apagado ou sem permissão?)");
    CHECK(gone.log == "cannot open (Not found)");

    CHECK(std::string(pkg::state_name(pkg::state::ready)) == "complete");
    CHECK(std::string(pkg::state_name(pkg::state::copying)) == "still copying");
    CHECK(std::string(pkg::state_name(pkg::state::open_failed)) == "cannot open");
}

void test_refusals()
{
    // Installed data present (the installer never asks then)
    CHECK(pkg::version_refusal_text("01.12", "01.11", "01.12", "", "BCUS98232") == "Este update (v01.12) já está instalado em dev_hdd0/game/BCUS98232");
    CHECK(pkg::version_refusal_text("01.12", "01.11", "1.12", "", "BCUS98232") == "Este update (v01.12) já está instalado em dev_hdd0/game/BCUS98232");
    CHECK(pkg::version_refusal_text("01.05", "01.04", "01.12", "", "BCUS98232") == "Já está instalada a v01.12, mais nova que este update (v01.05)");
    CHECK(pkg::version_refusal_text("01.12", "01.11", "01.10", "", "BCUS98232") ==
        "Está instalada a v01.10; este update (v01.12) pede a v01.11 antes: instale os updates entre elas, em ordem");
    // No installed data: disc version known or not
    CHECK(pkg::version_refusal_text("01.12", "01.11", "", "01.00", "BCUS98232") == "Este update v01.12 pede a v01.11; seu disco é v01.00 e não há update instalado");
    CHECK(pkg::version_refusal_text("01.12", "01.11", "", "", "BCUS98232") ==
        "Este update v01.12 pede a v01.11; o jogo BCUS98232 não está na biblioteca nem tem update instalado");
    // First patch older than the installed game (no target version)
    CHECK(pkg::version_refusal_text("01.01", "", "01.03", "", "BLUS30000") == "Já está instalada a v01.03, mais nova que este update (v01.01)");
    // Unreadable versions (package_install_result 'other')
    CHECK(pkg::version_refusal_text("", "", "", "", "BCUS98232") == "Versão ilegível no PARAM.SFO do PKG ou de dev_hdd0/game/BCUS98232 (veja RPCS3.log 'PKG:')");
    // Every text fits the XMB line (116 bytes) with a 9-character serial
    for (const auto& text : {pkg::version_refusal_text("01.12", "01.11", "01.10", "", "BCUS98232"),
            pkg::version_refusal_text("01.12", "01.11", "", "", "BCUS98232"), pkg::version_refusal_text("", "", "", "", "BCUS98232")})
        CHECK(text.size() <= 116);
}

// PS3 header with the given sizes (magic, retail type, platform 1).
std::vector<unsigned char> header_with(std::uint64_t pkg_size, std::uint64_t data_offset, std::uint64_t data_size)
{
    std::vector<unsigned char> h(pkg::header_size, 0);
    const unsigned char start[] = {0x7f, 0x50, 0x4b, 0x47, 0x80, 0x00, 0x00, 0x01};
    std::memcpy(h.data(), start, sizeof(start));
    for (int i = 0; i < 8; ++i)
    {
        h[0x18 + i] = static_cast<unsigned char>(pkg_size >> (56 - 8 * i));
        h[0x20 + i] = static_cast<unsigned char>(data_offset >> (56 - 8 * i));
        h[0x28 + i] = static_cast<unsigned char>(data_size >> (56 - 8 * i));
    }
    return h;
}

// Sizes larger than 4 GiB, split parts and trailing bytes (no file is created: the sizes are the facts the launcher sees)
void test_big_sizes()
{
    // LEGO Dimensions A0122 (7.6 GB, the PC's copy): pkg_size == file size, 0x60-byte trailer inside pkg_size
    const auto lego = header_with(0x1c62c4fd0ull, 0x180, 0x1c62c4df0ull);
    const pkg::header_info h = pkg::parse_header(lego.data(), lego.size());
    CHECK(h.pkg_size == 0x1c62c4fd0ull && h.pkg_size > 0xFFFFFFFFull);
    CHECK(h.data_offset + h.data_size <= h.pkg_size);
    CHECK(pkg::judge(h, {0x1c62c4fd0ull, 1'000'000, false, {}}).ready());
    // Trailing padding or a signature after pkg_size: accepted
    CHECK(pkg::judge(h, {0x1c62c4fd0ull + 0x10000, 1'000'000, false, {}}).ready());
    // The same name half downloaded (rusty-psn copy on the PC, 0x219107c8 bytes), untouched for days
    const pkg::verdict cut = pkg::judge(h, {0x219107c8ull, 10 * 86400, false, {}});
    CHECK(cut.kind == pkg::state::stopped);
    CHECK(cut.text == "Arquivo incompleto (563 MB de 7,6 GB) e a cópia parou: envie o PKG de novo");
    CHECK(cut.log == "incomplete file, 563152840 of 7619760080 bytes, last write 864000 s ago (copy stopped)");
    // FAT32 pieces (_00 = 4 GiB - 1, _01 = the rest), as check_package_file sums them
    const std::uint64_t first = 0xFFFFFFFFull;
    CHECK(pkg::judge(h, {first + (0x1c62c4fd0ull - first), 1'000'000, true, {}}).ready());
    CHECK(pkg::judge(h, {first, 1'000'000, true, "Lego_01.pkg"}).kind == pkg::state::part_missing);
    // Being written (the second piece still growing): copying, never "corrupt"
    CHECK(pkg::judge(h, {first + 1'000'000, 2, true, {}}).kind == pkg::state::copying);
    // 64-bit size texts
    CHECK(pkg::size_text(0x1c62c4fd0ull) == "7,6 GB");
    CHECK(pkg::sizes_text(0x1c62c4fd0ull - 1, 0x1c62c4fd0ull) == "7619 MB de 7620 MB");
}

// Simulated drive for pkg_io::read_rest: 'data' is the file, reads fail (return 0) while 'failing' attempts remain,
// and a read returns at most 'chunk' bytes.
struct fake_drive
{
    std::vector<unsigned char> data;
    int failing = 0;
    std::uint64_t chunk = ~0ull;
    int reads = 0;
    std::uint64_t waited = 0;
    bool stop = false;

    std::uint64_t read(std::uint64_t at, unsigned char* dst, std::uint64_t size)
    {
        ++reads;
        if (failing > 0)
        {
            --failing;
            return 0;
        }
        if (at >= data.size()) return 0;
        const std::uint64_t n = std::min<std::uint64_t>({size, chunk, data.size() - at});
        std::memcpy(dst, data.data() + at, n);
        return n;
    }
};

pkg_io::rest_result rest_of(fake_drive& d, std::uint64_t offset, std::vector<unsigned char>& dst, std::uint64_t done)
{
    const std::uint64_t want = dst.size();
    return pkg_io::read_rest(offset, want, done,
        [&](std::uint64_t at, std::uint64_t have) { return d.read(at, dst.data() + have, want - have); },
        [&] { return static_cast<std::uint64_t>(d.data.size()); },
        [&](std::uint32_t ms) { d.waited += ms; },
        [&] { return d.stop; });
}

void test_read_rest()
{
    fake_drive d;
    d.data.resize(1 << 20);
    for (std::size_t i = 0; i < d.data.size(); ++i) d.data[i] = static_cast<unsigned char>(i * 7 + (i >> 8));

    // A USB drive waking up: the first 4 attempts fail (File.cpp already gave up once), then everything reads
    std::vector<unsigned char> dst(4096);
    d.failing = 4;
    pkg_io::rest_result r = rest_of(d, 1000, dst, 0);
    CHECK(r.complete && !r.at_end && !r.stopped);
    CHECK(r.done == 4096);
    CHECK(r.waited_ms == 100 + 250 + 500 + 1000 + 2000);
    CHECK(std::memcmp(dst.data(), d.data.data() + 1000, 4096) == 0);

    // Short reads that keep making progress (exFAT returning a little at a time)
    d = fake_drive{d.data};
    d.chunk = 1000;
    std::vector<unsigned char> part(4096);
    const std::uint64_t first = d.read(0, part.data(), part.size());
    CHECK(first == 1000);
    r = rest_of(d, 0, part, first);
    CHECK(r.complete && r.done == 4096 && std::memcmp(part.data(), d.data.data(), 4096) == 0);

    // The end of the file: no waiting at all, not an error
    d = fake_drive{d.data};
    std::vector<unsigned char> tail(100);
    const std::uint64_t at = d.data.size() - 40;
    const std::uint64_t got = d.read(at, tail.data(), tail.size());
    CHECK(got == 40);
    r = rest_of(d, at, tail, got);
    CHECK(!r.complete && r.at_end && r.waited_ms == 0 && r.done == 40);

    // A drive that never comes back: every attempt made, about 26 s waited, reported short (not at the end)
    d = fake_drive{d.data};
    d.failing = 1000;
    r = rest_of(d, 0, dst, 0);
    CHECK(!r.complete && !r.at_end && !r.stopped && r.done == 0);
    CHECK(r.waited_ms == 25850);
    CHECK(d.reads == static_cast<int>(std::size(pkg_io::retry_delays_ms)));

    // Circle while waiting: stops at once
    d = fake_drive{d.data};
    d.failing = 1000;
    d.stop = true;
    r = rest_of(d, 0, dst, 0);
    CHECK(r.stopped && !r.complete && d.reads == 0 && r.waited_ms == 0);

    // Unknown size (fstat failed on the drive): keeps retrying instead of taking it for the end
    d = fake_drive{d.data};
    d.failing = 2;
    std::uint64_t waits = 0;
    r = pkg_io::read_rest(0, dst.size(), 0, [&](std::uint64_t a, std::uint64_t have) { return d.read(a, dst.data() + have, dst.size() - have); },
        [] { return std::uint64_t{0}; }, [&](std::uint32_t ms) { waits += ms; }, [] { return false; });
    CHECK(r.complete && waits == 100 + 250 + 500);

    // Outside an installation unpkg.cpp stops the retries once 3500 ms were waited: a drive waking at the fifth
    // attempt still reads, a dead one gives up after five attempts and 3850 ms (not 26 s)
    const auto checked = [&](fake_drive& drive)
    {
        std::uint64_t waited = 0;
        return pkg_io::read_rest(0, dst.size(), 0, [&](std::uint64_t a, std::uint64_t have) { return drive.read(a, dst.data() + have, dst.size() - have); },
            [&] { return static_cast<std::uint64_t>(drive.data.size()); }, [&](std::uint32_t ms) { waited += ms; }, [&] { return waited >= 3500; });
    };
    d = fake_drive{d.data};
    d.failing = 4;
    r = checked(d);
    CHECK(r.complete && !r.stopped && r.waited_ms == 3850 && d.reads == 5);
    d = fake_drive{d.data};
    d.failing = 1000;
    r = checked(d);
    CHECK(r.stopped && !r.complete && !r.at_end && r.done == 0 && r.waited_ms == 3850 && d.reads == 5);
}

// Package keystream as unpkg.cpp's decrypt() uses it: one 16-byte block per counter value, counter = offset / 16, and
// the read's first byte is XORed with the first byte of block 'offset / 16'.
unsigned char key_byte(std::uint64_t counter, unsigned i)
{
    std::uint64_t x = counter * 0x9E3779B97F4A7C15ull + i;
    x ^= x >> 29;
    x *= 0xBF58476D1CE4E5B9ull;
    return static_cast<unsigned char>(x >> 56);
}

void crypt(std::uint64_t offset, unsigned char* buf, std::size_t n)
{
    for (std::size_t i = 0; i < n; ++i) buf[i] ^= key_byte(offset / 16 + i / 16, static_cast<unsigned>(i % 16));
}

// Reads 'size' bytes of an encrypted entry the way the extraction loop does: decrypt chunks, continue after what was
// returned. The first read is cut to 'cut' bytes; 'align' applies pkg_io::whole_blocks to that short count.
std::vector<unsigned char> extract(const std::vector<unsigned char>& encrypted, std::size_t cut, bool align)
{
    std::vector<unsigned char> out;
    std::uint64_t pos = 0;
    bool first = true;
    while (pos < encrypted.size())
    {
        std::size_t n = std::min<std::size_t>(4096, encrypted.size() - pos);
        if (first) n = std::min(n, cut);
        std::vector<unsigned char> buf(encrypted.begin() + static_cast<std::ptrdiff_t>(pos), encrypted.begin() + static_cast<std::ptrdiff_t>(pos + n));
        crypt(pos, buf.data(), buf.size());
        const std::size_t keep = first && align ? pkg_io::whole_blocks(n) : n;
        out.insert(out.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(keep));
        pos += keep;
        first = false;
    }
    return out;
}

void test_keystream_alignment()
{
    CHECK(pkg_io::whole_blocks(0) == 0 && pkg_io::whole_blocks(15) == 0 && pkg_io::whole_blocks(16) == 16 && pkg_io::whole_blocks(37) == 32);
    CHECK(pkg_io::whole_blocks(std::size_t{1} << 24) == std::size_t{1} << 24);
    std::vector<unsigned char> plain(10000);
    for (std::size_t i = 0; i < plain.size(); ++i) plain[i] = static_cast<unsigned char>(i ^ (i >> 5));
    std::vector<unsigned char> encrypted = plain;
    crypt(0, encrypted.data(), encrypted.size());
    // Whole reads: exact
    CHECK(extract(encrypted, 4096, false) == plain);
    // A read cut to 37 bytes: continuing at offset 37 decrypts everything after it with the wrong keystream (the
    // corrupted-but-installed case); continuing at 32 is exact
    CHECK(extract(encrypted, 37, false) != plain);
    CHECK(extract(encrypted, 37, true) == plain);
    CHECK(extract(encrypted, 4000, true) == plain);
}

void test_failure_texts()
{
    using pkg::install_failure;
    const std::string long_name = "/data/rpcs3/dev_hdd0/game/BLUS31473/USRDIR/A_VERY_LONG_FOLDER/AND_A_VERY_LONG_FILE_NAME_INDEED.BIN";
    for (const install_failure kind : {install_failure::none, install_failure::read, install_failure::write, install_failure::create_file,
             install_failure::create_dir, install_failure::edat, install_failure::entry_type, install_failure::path, install_failure::threads})
    {
        for (const bool permission : {false, true})
            for (const bool partial : {false, true})
            {
                const std::string text = pkg::install_failure_text(kind, long_name, permission, partial);
                CHECK(text.size() <= 116);
                CHECK(text.find(partial ? "sobraram arquivos parciais" : "nada ficou instalado") != std::string::npos);
            }
        CHECK(std::string(pkg::install_failure_name(kind)) != "?");
    }
    CHECK(pkg::install_failure_text(install_failure::write, "/data/rpcs3/dev_hdd0/game/X/USRDIR/DATA.BIN", false, true) ==
        "Erro ao gravar DATA.BIN em /data (veja RPCS3.log) · sobraram arquivos parciais");
    CHECK(pkg::install_failure_text(install_failure::create_file, "/data/rpcs3/dev_hdd0/game/X/ICON0.PNG", true, false) ==
        "Sem permissão para criar ICON0.PNG em dev_hdd0 · nada ficou instalado");
    CHECK(pkg::install_failure_text(install_failure::read, "USRDIR/EBOOT.BIN", false, false) ==
        "Erro ao ler o PKG (USB desconectado ou arquivo danificado): copie o PKG de novo · nada ficou instalado");
    CHECK(pkg::install_failure_text(install_failure::edat, "USRDIR/DATA/LEVEL.SDAT", false, false) ==
        "O arquivo LEVEL.SDAT do PKG não decifrou (PKG danificado) · nada ficou instalado");
    CHECK(std::string(pkg::install_failure_name(install_failure::read)) == "PKG read error");
    // Clipped on a character boundary: "ç" (2 bytes) is never split
    CHECK(pkg::clip_name("short.bin") == "short.bin");
    const std::string accents = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaç" "çççççç.bin";
    const std::string clipped = pkg::clip_name(accents);
    CHECK(clipped.size() <= 32 && clipped.ends_with("..."));
    CHECK((static_cast<unsigned char>(clipped[clipped.size() - 4]) & 0xC0) != 0xC0); // no dangling lead byte
    CHECK(std::string(pkg::read_error_text).size() <= 116);
}

constexpr pkg::state all_states[] = {pkg::state::ready, pkg::state::ps4, pkg::state::psp_vita, pkg::state::empty, pkg::state::not_package,
    pkg::state::copying, pkg::state::stopped, pkg::state::part_missing, pkg::state::open_failed};

// Packages inside the booting disc or ISO (INSDIR, PKGDIR, PS3_EXTRA): never "renamed" or "still copying"
void test_disc_package_texts()
{
    CHECK(pkg::disc_package_text(pkg::state::not_package, "PS3_GAME/INSDIR/UP0001-BLUS30000_00-0000000000000001.PKG", true) ==
        "Pacote da ISO ilegível (PS3_GAME/INSDIR/UP0001-BLUS30000_00-0...): ISO danificada ou incompleta, copie de novo");
    CHECK(pkg::disc_package_text(pkg::state::not_package, "PS3_GAME/PKGDIR/PATCH.PKG", false) ==
        "Pacote do disco ilegível (PS3_GAME/PKGDIR/PATCH.PKG): copie a pasta do jogo de novo");
    CHECK(pkg::disc_package_text(pkg::state::ps4, "PS3_EXTRA/DATA.PKG", true) == "O disco traz um pacote que não é de PS3 (PS3_EXTRA/DATA.PKG)");
    CHECK(pkg::disc_package_text(pkg::state::psp_vita, "PS3_EXTRA/DATA.PKG", false) == "O disco traz um pacote que não é de PS3 (PS3_EXTRA/DATA.PKG)");
    // The name is clipped to 40 bytes on a character boundary
    const std::string name_40 = pkg::clip_name("PS3_EXTRA/ÇÇÇÇÇÇÇÇÇÇÇÇÇÇÇÇÇÇÇÇÇÇÇÇÇÇÇÇÇÇÇÇÇÇ.PKG", 40);
    CHECK(name_40.size() <= 40 && name_40.ends_with("..."));
    CHECK(pkg::disc_package_text(pkg::state::stopped, "PS3_EXTRA/ÇÇÇÇÇÇÇÇÇÇÇÇÇÇÇÇÇÇÇÇÇÇÇÇÇÇÇÇÇÇÇÇÇÇ.PKG", true).find("(" + name_40 + ")") != std::string::npos);
    CHECK(pkg::clip_name(std::string(40, 'a'), 40) == std::string(40, 'a'));
    CHECK(pkg::clip_name(std::string(41, 'a'), 40) == std::string(37, 'a') + "...");
    // Every kind, both media, a 40-byte name: the XMB line limit (116 bytes) in pt-BR, English and Spanish; Arabic
    // letters take two bytes each and the line is fitted by width there
    namespace i18n = utils::ps4::i18n;
    const std::string long_rel = "PS3_GAME/INSDIR/" + std::string(60, 'X') + ".PKG";
    for (const i18n::setting language : {i18n::setting::pt_br, i18n::setting::en, i18n::setting::es, i18n::setting::ar})
    {
        i18n::apply(language, -1);
        for (const pkg::state kind : all_states)
            for (const bool image : {false, true})
            {
                const std::string text = pkg::disc_package_text(kind, long_rel, image);
                CHECK(!text.empty());
                CHECK(text.size() <= (language == i18n::setting::ar ? 220u : 116u));
                CHECK(text.find("PS3_GAME/INSDIR/XXXX") != std::string::npos);
                CHECK(text.find("renomead") == std::string::npos && text.find("FileZilla") == std::string::npos);
            }
    }
    i18n::apply(i18n::setting::automatic, -1);
    CHECK(i18n::current() == i18n::lang::pt_br);
}

// The same verdicts in the other interface languages: translated, numbers and names kept
void test_languages()
{
    namespace i18n = utils::ps4::i18n;
    const auto bytes = gow_header();
    const pkg::header_info h = pkg::parse_header(bytes.data(), bytes.size());
    i18n::apply(i18n::setting::en, -1);
    CHECK(pkg::size_text(3040938288ull) == "3.0 GB");
    CHECK(pkg::sizes_text(1'200'000'000, 3040938288ull) == "1.2 GB of 3.0 GB");
    CHECK(pkg::judge(h, {1'200'000'000, 3, false, {}}).text == "Incomplete file (1.2 GB of 3.0 GB): still copying? Wait for FileZilla to finish");
    CHECK(pkg::judge(h, {2'000'000'000, 600, true, "Game_01.pkg"}).subtitle == "Part Game_01.pkg missing · copy all the parts");
    CHECK(pkg::version_refusal_text("01.12", "01.11", "", "01.00", "BCUS98232") == "This update v01.12 needs v01.11; your disc is v01.00 and no update is installed");
    CHECK(std::string(pkg::psp_vita_text) == "PSP/PS Vita PKG: this emulator installs only PS3 PKG files");
    CHECK(pkg::install_failure_text(pkg::install_failure::write, "/x/DATA.BIN", true, true) == "No permission to write DATA.BIN in dev_hdd0 · partial files were left");
    i18n::apply(i18n::setting::es, -1);
    CHECK(pkg::size_text(3040938288ull) == "3.0 GB");
    CHECK(pkg::open_failed("x").subtitle == "No se pudo abrir el PKG (¿eliminado o sin permiso?)");
    i18n::apply(i18n::setting::ar, -1);
    // Arabic: numbers, versions and serials isolated with LRM, so they keep their left-to-right order
    const std::string lrm = "\xE2\x80\x8E";
    const std::string refusal = pkg::version_refusal_text("01.12", "01.11", "01.12", "", "BCUS98232");
    CHECK(refusal.find(lrm + "01.12" + lrm) != std::string::npos && refusal.find(lrm + "BCUS98232" + lrm) != std::string::npos);
    CHECK(pkg::install_failure_text(pkg::install_failure::none, {}, false, false).find("RPCS3.log") != std::string::npos);
    // Every verdict and failure text is translated (no pt-BR left) in the three other languages
    for (const i18n::setting language : {i18n::setting::en, i18n::setting::es, i18n::setting::ar})
    {
        i18n::apply(language, -1);
        const std::vector<pkg::verdict> verdicts = {pkg::judge(h, {1'200'000'000, 3, false, {}}), pkg::judge(h, {1'200'000'000, 600, false, {}}),
            pkg::judge(h, {2'000'000'000, 600, true, "Game_01.pkg"}), pkg::judge(pkg::parse_header(bytes.data(), 12), {12, 1, false, {}}),
            pkg::judge(pkg::parse_header(bytes.data(), 12), {12, 1000, false, {}}), pkg::open_failed("x")};
        for (const pkg::verdict& v : verdicts)
        {
            CHECK(v.text.find("Arquivo") == std::string::npos && v.text.find("envie") == std::string::npos);
            CHECK(v.subtitle.find("aguarde") == std::string::npos && v.subtitle.find("copie") == std::string::npos);
        }
        for (const pkg::install_failure kind : {pkg::install_failure::none, pkg::install_failure::read, pkg::install_failure::write,
                 pkg::install_failure::create_file, pkg::install_failure::create_dir, pkg::install_failure::edat, pkg::install_failure::entry_type,
                 pkg::install_failure::path, pkg::install_failure::threads})
            for (const bool permission : {false, true})
            {
                const std::string text = pkg::install_failure_text(kind, "/data/rpcs3/dev_hdd0/game/X/USRDIR/EBOOT.BIN", permission, true);
                CHECK(text.find("sobraram") == std::string::npos && text.find("Erro ao") == std::string::npos && text.find("permissão") == std::string::npos);
            }
    }
    i18n::apply(i18n::setting::automatic, -1);
    CHECK(pkg::size_text(3040938288ull) == "3,0 GB");
}

// Read-only verdict of real PKGs: every *.pkg below each argument (file or folder), 48 bytes and the size of each
// file, split parts summed as the launcher does. Returns the number of files that are not ready.
int report_files(int argc, char** argv)
{
    namespace fsys = std::filesystem;
    std::vector<fsys::path> files;
    const auto is_pkg = [](const fsys::path& p)
    {
        std::string ext = p.extension().string();
        for (char& c : ext) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c + 32 : c);
        return ext == ".pkg";
    };
    for (int i = 1; i < argc; ++i)
    {
        std::error_code ec;
        const fsys::path root(argv[i]);
        if (fsys::is_directory(root, ec))
        {
            for (auto it = fsys::recursive_directory_iterator(root, ec); !ec && it != fsys::recursive_directory_iterator(); it.increment(ec))
                if (it->is_regular_file(ec) && is_pkg(it->path())) files.push_back(it->path());
        }
        else
            files.push_back(root);
    }
    int bad = 0;
    for (const fsys::path& path : files)
    {
        unsigned char bytes[pkg::header_size]{};
        std::ifstream in(path, std::ios::binary);
        in.read(reinterpret_cast<char*>(bytes), sizeof(bytes));
        const pkg::header_info h = pkg::parse_header(bytes, static_cast<std::size_t>(in.gcount()));
        std::error_code ec;
        pkg::file_facts facts;
        facts.have = fsys::file_size(path, ec);
        facts.idle_seconds = 1'000'000;
        const std::wstring stem = path.stem().wstring();
        if (h.kind == pkg::magic_kind::ps3 && h.pkg_size > facts.have && stem.size() > 3 && stem.ends_with(L"_00"))
        {
            facts.split = true;
            for (unsigned part = 1; facts.have < h.pkg_size && part < 1000; ++part)
            {
                wchar_t number[8];
                std::swprintf(number, 8, L"_%02u", part);
                const fsys::path next = path.parent_path() / (stem.substr(0, stem.size() - 3) + number + path.extension().wstring());
                const auto size = fsys::file_size(next, ec);
                if (ec || !size)
                {
                    const auto missing = next.filename().u8string();
                    facts.missing_part.assign(reinterpret_cast<const char*>(missing.data()), missing.size());
                    break;
                }
                facts.have += size;
            }
        }
        const pkg::verdict v = pkg::judge(h, facts);
        const bool layout_ok = h.kind != pkg::magic_kind::ps3 || (h.data_size <= ~h.data_offset && h.data_offset + h.data_size <= h.pkg_size);
        const auto name = path.filename().u8string();
        std::printf("%.*s\n  size %llu pkg_size %llu data_offset 0x%llx data_size 0x%llx -> %s%s%s\n", static_cast<int>(name.size()),
            reinterpret_cast<const char*>(name.data()), static_cast<unsigned long long>(facts.have), static_cast<unsigned long long>(h.pkg_size),
            static_cast<unsigned long long>(h.data_offset), static_cast<unsigned long long>(h.data_size), pkg::state_name(v.kind), v.log.empty() ? "" : ": ",
            v.log.c_str());
        if (!layout_ok) std::printf("  data_offset + data_size beyond pkg_size: package_reader refuses it\n");
        bad += !v.ready() || !layout_ok;
    }
    return bad;
}
}

int main(int argc, char** argv)
{
    if (argc > 1) return report_files(argc, argv) ? 2 : 0;
    test_header();
    test_versions();
    test_sizes();
    test_part_names();
    test_verdicts();
    test_refusals();
    test_big_sizes();
    test_read_rest();
    test_keystream_alignment();
    test_failure_texts();
    test_disc_package_texts();
    test_languages();
    std::puts("pkg_check_test: all checks passed");
    return 0;
}
