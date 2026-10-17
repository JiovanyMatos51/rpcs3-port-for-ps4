// Host test of the disc lock (rpcs3/util/ps4_disc_lock.hpp) and of the drawn padlock (rpcs3/util/ps4_padlock_icon.hpp):
// lock record text, fingerprint, registry and binding, classification of the drive status, the X screen (gate), the
// verified-boot token, the XMB check cadence, the pt-BR texts, the padlock image, and the removal watch of a running
// game (three checks without the disc over 10 s, then its return; server errors and 550 never counted), also on real
// drive checks against a stand-in of the console's FTP server. Binding is by the exact image path of the record.
// Build (from the repo root):
//   clang++ -std=c++20 -O2 -static -Wall -Wextra -I rpcs3 ps4-port/tests/disc_lock_test.cpp -o disc_lock_test.exe
#include "../../rpcs3/util/ps4_disc_lock.hpp"
#include "../../rpcs3/util/ps4_padlock_icon.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <map>
#include <set>
#include <thread>

namespace
{
namespace dl = utils::ps4::disc_lock;
namespace ds = utils::ps4::disc_stream;

int failures = 0;
#define CHECK(condition) \
    do \
    { \
        if (!(condition)) \
        { \
            std::fprintf(stderr, "FAIL at line %d: %s\n", __LINE__, #condition); \
            ++failures; \
        } \
    } while (false)

constexpr std::uint64_t sector = ds::sector_size;

bool valid_utf8(std::string_view text)
{
    for (std::size_t i = 0; i < text.size();)
    {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        const std::size_t extra = c < 0x80 ? 0 : (c & 0xe0) == 0xc0 ? 1 : (c & 0xf0) == 0xe0 ? 2 : (c & 0xf8) == 0xf0 ? 3 : 9;
        if (extra == 9 || i + extra >= text.size() + (extra ? 0 : 1)) return false;
        for (std::size_t k = 1; k <= extra; ++k)
            if ((static_cast<unsigned char>(text[i + k]) & 0xc0) != 0x80) return false;
        i += extra + 1;
    }
    return true;
}

dl::record fifa()
{
    dl::record r;
    r.title_id = "BLES01876";
    r.title = "FIFA 14";
    r.version = "01.00";
    r.app_ver = "01.00";
    r.size = 10'619'912'192ull;
    r.identity = 0x89abcdef01234567ull;
    r.fingerprint = 0x0123456789abcdefull;
    r.image = "BLES01876 - FIFA 14.iso";
    r.path = "/data/rpcs3/games/BLES01876 - FIFA 14.iso";
    return r;
}

ds::disc_status disc(std::string id, std::string title, std::uint64_t identity, std::uint64_t fingerprint, std::int64_t checked_ms)
{
    ds::disc_status s;
    s.state = ds::disc_state::ps3_disc;
    s.title_id = std::move(id);
    s.title = std::move(title);
    s.identity = identity;
    s.fingerprint = fingerprint;
    s.checked_ms = checked_ms;
    s.generation = 1;
    return s;
}

ds::disc_status drive(ds::disc_state state, std::int64_t checked_ms)
{
    ds::disc_status s;
    s.state = state;
    s.checked_ms = checked_ms;
    s.detail = state == ds::disc_state::no_disc ? "refused: 550 Requested action not taken" : "";
    return s;
}

ds::disc_status fifa_disc(std::int64_t checked_ms)
{
    return disc("BLES01876", "FIFA 14", 0x89abcdef01234567ull, 0x0123456789abcdefull, checked_ms);
}

void test_record()
{
    // Round trip
    const dl::record r = fifa();
    const std::string text = dl::to_text(r);
    CHECK(text.starts_with("rpcs3 disc lock 1\ntitle_id BLES01876\ntitle FIFA 14\n"));
    CHECK(text.find("size 10619912192\nidentity 89abcdef01234567\nfingerprint 0123456789abcdef\nimage BLES01876 - FIFA 14.iso\n"
                    "path /data/rpcs3/games/BLES01876 - FIFA 14.iso\n") != std::string::npos);
    std::string why;
    auto back = dl::parse(text, &why);
    CHECK(back && back->title_id == r.title_id && back->title == r.title && back->version == r.version && back->app_ver == r.app_ver);
    CHECK(back && back->size == r.size && back->identity == r.identity && back->fingerprint == r.fingerprint && back->image == r.image && back->valid);
    CHECK(back && back->path == r.path);

    // The image path: an install on USB or elsewhere keeps its own; a record of 0.37/0.38 (no 'path') names the image
    // its install wrote in /data/rpcs3/games; nothing usable, nothing named
    dl::record moved = r;
    moved.path = "/mnt/usb0/PS3ISO/BLES01876 - FIFA 14.iso";
    back = dl::parse(dl::to_text(moved));
    CHECK(back && back->path == moved.path);
    const std::string legacy = "rpcs3 disc lock 1\ntitle_id BLES01876\nsize 123\nidentity 1\nimage BLES01876 - FIFA 14.iso\n";
    back = dl::parse(legacy);
    CHECK(back && back->path == "/data/rpcs3/games/BLES01876 - FIFA 14.iso");
    back = dl::parse("rpcs3 disc lock 1\ntitle_id BLES01876\nsize 123\nidentity 1\n");
    CHECK(back && back->path.empty() && back->valid);
    CHECK(dl::parse(legacy + "path relative/x.iso")->path.empty());
    CHECK(dl::parse("rpcs3 disc lock 1\ntitle_id BLES01876\nsize 123\nidentity 1\nimage ../x.iso\n")->path.empty());
    CHECK(dl::parse("rpcs3 disc lock 1\ntitle_id BLES01876\nsize 123\nidentity 1\nimage ..\n")->path.empty());
    CHECK(dl::recorded_path("", "a.iso") == "/data/rpcs3/games/a.iso" && dl::recorded_path("/x/y.iso", "a.iso") == "/x/y.iso" && dl::recorded_path("", "").empty());

    // CRLF, unknown keys, the id written with a dash, upper-case hex
    back = dl::parse("rpcs3 disc lock 1\r\ntitle_id BLES-01876\r\nfuture_key 42\r\nsize 123\r\nidentity 00000000000000AB\r\n\r\n", &why);
    CHECK(back && back->title_id == "BLES01876" && back->size == 123 && back->identity == 0xab && back->fingerprint == 0 && back->title.empty());

    // Required fields
    const std::string base = "rpcs3 disc lock 1\ntitle_id BLES01876\nsize 123\nidentity 1\n";
    CHECK(dl::parse(base));
    CHECK(!dl::parse("rpcs3 disc lock 1\nsize 123\nidentity 1\n", &why) && why == "no valid title_id");
    CHECK(!dl::parse("rpcs3 disc lock 1\ntitle_id BLES01876\nidentity 1\n", &why) && why == "no valid size");
    CHECK(!dl::parse("rpcs3 disc lock 1\ntitle_id BLES01876\nsize 123\n", &why) && why == "no valid identity");
    CHECK(!dl::parse("rpcs3 disc lock 1\ntitle_id BLES01876\nsize 0\nidentity 1\n", &why) && why == "no valid size");
    CHECK(!dl::parse("rpcs3 disc lock 1\ntitle_id BLES01876\nsize 99999999999999999999\nidentity 1\n", &why) && why == "no valid size");
    CHECK(!dl::parse("rpcs3 disc lock 1\ntitle_id BLES0187X\nsize 1\nidentity 1\n", &why) && why == "no valid title_id");
    CHECK(!dl::parse("rpcs3 disc lock 1\ntitle_id BLES01876\nsize 1\nidentity xyz\n", &why) && why == "no valid identity");
    CHECK(!dl::parse("title_id BLES01876\nsize 1\nidentity 1\n", &why) && why == "no 'rpcs3 disc lock 1' header");
    CHECK(!dl::parse("", &why) && why == "no 'rpcs3 disc lock 1' header");
    CHECK(!dl::parse("\x89PNG\r\n\x1a\n garbage", &why));
    // A bad fingerprint is only unknown
    back = dl::parse(base + "fingerprint zz\n");
    CHECK(back && back->fingerprint == 0);

    // 4 KiB bound
    std::string padded = base + "comment ";
    padded.resize(dl::record_limit, 'x');
    CHECK(dl::parse(padded));
    padded += 'x';
    CHECK(!dl::parse(padded, &why) && why == "larger than 4 KiB");

    // A long title is cut to 128 bytes on a UTF-8 boundary; control characters are dropped
    dl::record accents = r;
    accents.title.clear();
    for (int i = 0; i < 150; ++i) accents.title += "\xc3\xa9";
    back = dl::parse(dl::to_text(accents));
    CHECK(back && back->title.size() == 128 && valid_utf8(back->title));
    accents.title = "a" + accents.title;
    back = dl::parse(dl::to_text(accents));
    CHECK(back && back->title.size() == 127 && valid_utf8(back->title));
    dl::record control = r;
    control.title = "FIFA\t14\x01\x7f";
    back = dl::parse(dl::to_text(control));
    CHECK(back && back->title == "FIFA14");
    CHECK(dl::to_text(control).find('\t') == std::string::npos);

    // File names
    CHECK(dl::file_name("BLES01876") == "BLES01876.txt" && dl::id_of_file("BLES01876.txt") == "BLES01876");
    CHECK(dl::id_of_file("bles01876.txt").empty() && dl::id_of_file("BLES01876.txt.tmp").empty() && dl::id_of_file("BLES01876.TXT").empty());
    CHECK(dl::id_of_file("BLES-1876.txt").empty() && dl::id_of_file("readme.txt").empty() && dl::id_of_file(".txt").empty());
}

void le32_at(std::vector<unsigned char>& v, std::size_t at, std::uint32_t value)
{
    for (int i = 0; i < 4; ++i) v[at + i] = static_cast<unsigned char>(value >> (8 * i));
}

std::vector<unsigned char> dir_record(std::string_view name, std::uint32_t lba, std::uint32_t bytes, bool directory)
{
    const std::size_t length = (33 + name.size() + 1) & ~std::size_t{1};
    std::vector<unsigned char> r(length, 0);
    r[0] = static_cast<unsigned char>(length);
    le32_at(r, 2, lba);
    le32_at(r, 10, bytes);
    r[25] = directory ? 2 : 0;
    r[32] = static_cast<unsigned char>(name.size());
    std::memcpy(r.data() + 33, name.data(), name.size());
    return r;
}

// A small PS3 disc image: header, PVD -> root (20) -> PS3_GAME (21) -> PARAM.SFO (22), then "encrypted" sectors
std::vector<unsigned char> small_image(std::string_view sfo_text)
{
    std::vector<unsigned char> image(64 * sector, 0);
    image[3] = 2; // two plain regions
    image[15] = 0x1f;
    std::memcpy(image.data() + sector, "PlayStation3", 12);
    std::memcpy(image.data() + sector + 0x10, "BLES-01876      ", 16);
    unsigned char* pvd = image.data() + 16 * sector;
    pvd[0] = 1;
    std::memcpy(pvd + 1, "CD001", 5);
    const auto root = dir_record(std::string_view("\0", 1), 20, sector, true);
    std::memcpy(pvd + 156, root.data(), 34);
    const auto put = [&image](std::uint32_t lba, const std::vector<std::vector<unsigned char>>& records)
    {
        std::size_t at = lba * sector;
        for (const auto& r : records)
        {
            std::memcpy(image.data() + at, r.data(), r.size());
            at += r.size();
        }
    };
    put(20, {dir_record(std::string_view("\0", 1), 20, sector, true), dir_record(std::string_view("\1", 1), 20, sector, true), dir_record("PS3_GAME", 21, sector, true)});
    put(21, {dir_record(std::string_view("\0", 1), 21, sector, true), dir_record(std::string_view("\1", 1), 20, sector, true),
        dir_record("PARAM.SFO;1", 22, static_cast<std::uint32_t>(sfo_text.size()), false)});
    std::memcpy(image.data() + 22 * sector, sfo_text.data(), sfo_text.size());
    for (std::size_t i = 32 * sector; i < image.size(); ++i) image[i] = static_cast<unsigned char>(i * 37 + 11);
    return image;
}

// The fingerprint of an image as the drive check takes it: sectors 0-1, the PVD that locate_files reads, PARAM.SFO
std::uint64_t fingerprint_of(const std::vector<unsigned char>& image, std::vector<unsigned char>* sfo = nullptr)
{
    std::vector<unsigned char> pvd;
    const ds::disc_files files = ds::locate_files([&](std::uint64_t offset, void* out, std::size_t size)
    {
        if (offset + size > image.size()) return false;
        std::memcpy(out, image.data() + offset, size);
        if (offset == 16 * sector && size == sector) pvd.assign(image.data() + offset, image.data() + offset + size);
        return true;
    }, false);
    if (sfo) *sfo = files.param;
    if (pvd.empty() || files.param.empty()) return 0;
    return ds::disc_fingerprint(image.data(), 2 * sector, pvd.data(), pvd.size(), files.param.data(), files.param.size());
}

void test_fingerprint()
{
    // The chained hash is the hash of the three parts one after the other
    std::vector<unsigned char> a(4096, 1), b(2048, 2), c(300, 3), all;
    a[7] = 9;
    all.insert(all.end(), a.begin(), a.end());
    all.insert(all.end(), b.begin(), b.end());
    all.insert(all.end(), c.begin(), c.end());
    CHECK(ds::disc_fingerprint(a.data(), a.size(), b.data(), b.size(), c.data(), c.size()) == ds::detail::fnv1a64(all.data(), all.size()));
    CHECK(ds::disc_fingerprint(nullptr, 0, nullptr, 0, nullptr, 0) != 0);

    const std::string sfo_text = std::string("\0PSF", 4) + "fake PARAM.SFO bytes of FIFA 14";
    const std::vector<unsigned char> disc_image = small_image(sfo_text);
    std::vector<unsigned char> sfo;
    const std::uint64_t from_disc = fingerprint_of(disc_image, &sfo);
    CHECK(from_disc != 0 && sfo == std::vector<unsigned char>(sfo_text.begin(), sfo_text.end()));
    // The decrypted image differs past plain region 0 only: same fingerprint
    std::vector<unsigned char> decrypted = disc_image;
    for (std::size_t i = 32 * sector; i < decrypted.size(); ++i) decrypted[i] ^= 0x5a;
    CHECK(fingerprint_of(decrypted) == from_disc);
    // One byte of the PVD, of PARAM.SFO or of sectors 0-1 changes it; sectors 0-1 change the identity too
    std::vector<unsigned char> other = disc_image;
    other[16 * sector + 40] ^= 1;
    CHECK(fingerprint_of(other) != from_disc);
    other = disc_image;
    other[22 * sector + 10] ^= 1;
    CHECK(fingerprint_of(other) != from_disc);
    other = disc_image;
    other[sector + 0x200] ^= 1;
    CHECK(fingerprint_of(other) != from_disc);
    CHECK(ds::detail::fnv1a64(other.data(), 2 * sector) != ds::detail::fnv1a64(disc_image.data(), 2 * sector));
}

struct fake_folder
{
    std::map<std::string, std::string> files; // name -> content
    std::set<std::string> unreadable;         // names whose read fails (EIO)
    std::set<std::string> closed;             // names the app may not open (EACCES)
    int list_error = 0;                       // the listing fails with it
    unsigned reads = 0;

    dl::list_fn list()
    {
        return [this](const std::string& dir, std::vector<std::string>& names, int& error)
        {
            if (dir != dl::registry_dir) return error = ENOENT, false;
            if (list_error) return error = list_error, false;
            for (const auto& [name, text] : files) names.push_back(name);
            return true;
        };
    }

    dl::read_fn read()
    {
        return [this](const std::string& path, std::size_t limit, int& error) -> std::optional<std::string>
        {
            ++reads;
            const std::string name = path.substr(std::string(dl::registry_dir).size());
            if (closed.count(name))
            {
                error = EACCES;
                return std::nullopt;
            }
            if (unreadable.count(name) || !files.count(name))
            {
                error = EIO;
                return std::nullopt;
            }
            return files[name].substr(0, limit);
        };
    }
};

void test_registry()
{
    fake_folder folder;
    folder.files["BLES01876.txt"] = dl::to_text(fifa());
    dl::record demon = fifa();
    demon.title_id = "BLUS30443";
    demon.title = "Demon's Souls";
    demon.size = 7'000'000'000ull;
    demon.image = "BLUS30443 - Demon's Souls.iso";
    demon.path.clear(); // a record of 0.38
    folder.files["BLUS30443.txt"] = dl::to_text(demon);
    folder.files["BCUS98232.txt"] = "this is not a record";
    folder.files["BLES01876.txt.tmp"] = "half written";
    folder.files["readme.txt"] = "x";
    folder.files["NPUB30001.txt"] = dl::to_text(fifa()); // names BLES01876 inside
    folder.files["BCES00001.txt"] = "";
    folder.unreadable.insert("BCES00001.txt");
    folder.files["BLJM60001.txt"] = dl::to_text(fifa());
    folder.closed.insert("BLJM60001.txt");
    dl::registry r = dl::load(folder.list(), folder.read());
    CHECK(r.unreadable.empty() && !r.truncated && r.by_id.size() == 5 && folder.reads == 6);
    // A record file the app may not open counts as absent (like a folder it cannot list); an I/O error fails closed
    CHECK(!r.by_id.count("BLJM60001") && r.closed == std::vector<std::string>{"BLJM60001.txt (errno 13 (EACCES))"});
    CHECK(dl::absent_error(ENOENT) && dl::absent_error(EACCES) && dl::absent_error(EPERM) && dl::absent_error(ENOTDIR) && !dl::absent_error(EIO));
    CHECK(r.by_id.at("BLES01876").valid && r.by_id.at("BLUS30443").valid);
    CHECK(!r.by_id.at("BCUS98232").valid && r.by_id.at("BCUS98232").problem == "no 'rpcs3 disc lock 1' header");
    CHECK(!r.by_id.at("NPUB30001").valid && r.by_id.at("NPUB30001").problem == "it names BLES01876");
    CHECK(!r.by_id.at("BCES00001").valid && r.by_id.at("BCES00001").problem.find("EIO") != std::string::npos);

    // Binding: only the exact image the install wrote (full path and size of its record)
    const std::string fifa_path = "/data/rpcs3/games/BLES01876 - FIFA 14.iso";
    CHECK(r.by_path.size() == 2 && r.by_path.at(fifa_path) == "BLES01876");
    CHECK(dl::bound(r, fifa_path, 10'619'912'192ull) == &r.by_id.at("BLES01876"));
    CHECK(dl::recorded_at(r, fifa_path) == &r.by_id.at("BLES01876") && dl::recorded_at(r, "/data/rpcs3/games/x.iso") == nullptr);
    // A Redump ISO of the same game with the same size, at another path: an ordinary ISO
    const std::string redump = "/data/rpcs3/games/FIFA 14 (Europe) (En,Fr,De,Es,It,Nl,Pt,Sv,No,Da,Fi,Ru,Pl).iso";
    CHECK(dl::bound(r, redump, 10'619'912'192ull) == nullptr);
    CHECK(dl::unbound_reason(r, redump, "BLES-01876", 10'619'912'192ull) ==
        "'" + redump + "' (BLES01876) is an ordinary ISO (the image installed from the disc is '" + fifa_path + "'): not locked");
    // The same image on a USB drive, or renamed: not bound
    CHECK(dl::bound(r, "/mnt/usb0/PS3ISO/BLES01876 - FIFA 14.iso", 10'619'912'192ull) == nullptr);
    CHECK(dl::bound(r, "/data/rpcs3/games/FIFA 14.iso", 10'619'912'192ull) == nullptr);
    // Unknown size (0), another size: not bound
    CHECK(dl::bound(r, fifa_path, 0) == nullptr && dl::bound(r, fifa_path, 10'619'912'193ull) == nullptr);
    CHECK(dl::unbound_reason(r, fifa_path, "BLES01876", 0) == "'" + fifa_path + "' (BLES01876): size not known now: not locked");
    CHECK(dl::unbound_reason(r, fifa_path, "BLES01876", 5) == "'" + fifa_path + "' (BLES01876): 5 bytes, its record says 10619912192: not locked");
    CHECK(dl::unbound_reason(r, fifa_path, "BLES01876", 10'619'912'192ull).empty() && dl::unbound_reason(r, redump, "BLUS31426", 1).empty());
    CHECK(dl::bound(r, "", 10'619'912'192ull) == nullptr);
    // A record of 0.38 (no path): only /data/rpcs3/games/<image>
    CHECK(dl::bound(r, "/data/rpcs3/games/BLUS30443 - Demon's Souls.iso", 7'000'000'000ull) == &r.by_id.at("BLUS30443"));
    CHECK(dl::bound(r, "/mnt/usb1/PS3ISO/BLUS30443 - Demon's Souls.iso", 7'000'000'000ull) == nullptr);
    // The record is found by the image, whatever TITLE_ID the ISO's PARAM.SFO has (header id != PARAM.SFO id)
    CHECK(dl::unbound_reason(r, fifa_path, "BCUS98472", 10'619'912'192ull).empty());
    // A record that cannot be read locks nothing (fail open) and is reported
    CHECK(dl::recorded_at(r, "/data/rpcs3/games/BCUS98232.iso") == nullptr && dl::unbound_reason(r, "/x.iso", "BCUS98232", 1).empty());
    CHECK(std::count_if(r.unbound.begin(), r.unbound.end(), [](const std::string& line) { return line.ends_with("nothing is locked for BCUS98232"); }) == 1);
    CHECK(std::find(r.unbound.begin(), r.unbound.end(), "record /data/rpcs3/data/disc-lock/BCUS98232.txt unreadable (no 'rpcs3 disc lock 1' header): nothing is locked for BCUS98232") !=
        r.unbound.end());
    CHECK(r.unbound.size() == 3); // BCES00001, BCUS98232, NPUB30001

    // Two records naming one image: the first by TITLE_ID binds it, the other is reported; a record without a path binds nothing
    fake_folder twice;
    twice.files["BLES01876.txt"] = dl::to_text(fifa());
    dl::record copy = fifa();
    copy.title_id = "BLUS31426";
    twice.files["BLUS31426.txt"] = dl::to_text(copy);
    twice.files["BCUS98472.txt"] = "rpcs3 disc lock 1\ntitle_id BCUS98472\nsize 123\nidentity 1\n";
    r = dl::load(twice.list(), twice.read());
    CHECK(r.by_path.size() == 1 && r.by_path.at(fifa_path) == "BLES01876" && dl::bound(r, fifa_path, 10'619'912'192ull) == &r.by_id.at("BLES01876"));
    CHECK(r.unbound.size() == 2 && r.unbound[0] == "record /data/rpcs3/data/disc-lock/BCUS98472.txt has no image path: nothing is locked for BCUS98472");
    CHECK(r.unbound[1] == "record /data/rpcs3/data/disc-lock/BLUS31426.txt names the image '" + fifa_path + "' of BLES01876: nothing is locked for BLUS31426");

    // 600 record files: the first 512 by name are read
    fake_folder many;
    for (int i = 0; i < 600; ++i)
    {
        char id[16];
        std::snprintf(id, sizeof(id), "BLES%05d", 10000 + i);
        dl::record x = fifa();
        x.title_id = id;
        many.files[dl::file_name(id)] = dl::to_text(x);
    }
    r = dl::load(many.list(), many.read());
    CHECK(r.truncated && r.by_id.size() == dl::registry_limit && many.reads == dl::registry_limit);
    CHECK(r.by_id.count("BLES10000") && r.by_id.count("BLES10511") && !r.by_id.count("BLES10512"));

    // No folder: nothing bound, nothing reported; a folder that cannot be listed: nothing bound, reported
    r = dl::load(folder.list(), folder.read(), "/data/rpcs3/data/elsewhere/");
    CHECK(r.by_id.empty() && r.unreadable.empty());
    folder.list_error = EACCES;
    r = dl::load(folder.list(), folder.read());
    CHECK(r.by_id.empty() && r.unreadable.find("EACCES") != std::string::npos);
    CHECK(dl::load({}, {}).by_id.empty());
}

void test_classify()
{
    const dl::record r = fifa();
    const std::int64_t now = 1'000'000;
    CHECK(dl::classify(r, fifa_disc(now - 1000), now) == dl::match::exact);
    // Another pressing: identity or fingerprint differ; a missing value on either side leaves it to the other
    CHECK(dl::classify(r, disc("BLES01876", "FIFA 14", 0x1111, r.fingerprint, now), now) == dl::match::same_title);
    CHECK(dl::classify(r, disc("BLES01876", "FIFA 14", r.identity, 0x2222, now), now) == dl::match::same_title);
    CHECK(dl::classify(r, disc("BLES01876", "FIFA 14", r.identity, 0, now), now) == dl::match::exact);
    dl::record unknown_fp = r;
    unknown_fp.fingerprint = 0;
    CHECK(dl::classify(unknown_fp, disc("BLES01876", "FIFA 14", r.identity, 0x2222, now), now) == dl::match::exact);
    CHECK(dl::classify(unknown_fp, disc("BLES01876", "FIFA 14", 0x3333, 0x2222, now), now) == dl::match::same_title);
    // Another game; the header id in another spelling; PARAM.SFO's id when the header holds no standard id
    CHECK(dl::classify(r, disc("BLUS30443", "Demon's Souls", 1, 2, now), now) == dl::match::other_disc);
    CHECK(dl::classify(r, disc("bles-01876", "FIFA 14", r.identity, r.fingerprint, now), now) == dl::match::exact);
    ds::disc_status odd = disc("PS3 GAME DISC", "FIFA 14", r.identity, r.fingerprint, now);
    CHECK(dl::classify(r, odd, now) == dl::match::other_disc);
    odd.sfo_title_id = "BLES01876";
    CHECK(dl::classify(r, odd, now) == dl::match::exact);
    // The other drive states
    CHECK(dl::classify(r, drive(ds::disc_state::no_disc, now), now) == dl::match::no_disc);
    CHECK(dl::classify(r, drive(ds::disc_state::not_ps3, now), now) == dl::match::not_ps3);
    CHECK(dl::classify(r, drive(ds::disc_state::no_server, now), now) == dl::match::no_server);
    CHECK(dl::classify(r, drive(ds::disc_state::unknown, now), now) == dl::match::unknown);
    CHECK(dl::classify(r, ds::disc_status{}, now) == dl::match::unknown);
    // Too old (the worker stopped while a game ran): stale, whatever it said
    CHECK(dl::classify(r, fifa_disc(now - 120'001), now) == dl::match::stale);
    CHECK(dl::classify(r, fifa_disc(now - 120'000), now) == dl::match::exact);
    CHECK(dl::classify(r, drive(ds::disc_state::no_disc, now - 200'000), now) == dl::match::stale);
    // A record that cannot be read: a disc of its TITLE_ID still opens it
    const dl::record broken = dl::unreadable_record("BLES01876", "garbage");
    CHECK(dl::classify(broken, fifa_disc(now), now) == dl::match::same_title);
    CHECK(dl::classify(broken, disc("BLUS30443", "", 1, 2, now), now) == dl::match::other_disc);

    // What unlocks; strict mode accepts the exact pressing only
    for (const dl::match m : {dl::match::unknown, dl::match::stale, dl::match::no_server, dl::match::no_disc, dl::match::not_ps3, dl::match::other_disc})
        CHECK(!dl::unlocks(m) && !dl::unlocks(m, false) && std::string(dl::to_string(m)).size() > 3);
    CHECK(dl::unlocks(dl::match::exact) && dl::unlocks(dl::match::exact, false));
    CHECK(dl::unlocks(dl::match::same_title) == dl::accept_other_pressing && !dl::unlocks(dl::match::same_title, false));

    // Log lines
    CHECK(dl::row_log_line(r, dl::match::no_disc, drive(ds::disc_state::no_disc, now)) == "BLES01876 'FIFA 14': locked (no disc in the drive)");
    CHECK(dl::row_log_line(r, dl::match::other_disc, disc("BLUS30187", "", 1, 2, now)) == "BLES01876 'FIFA 14': locked (another PS3 disc: BLUS30187)");
    CHECK(dl::row_log_line(r, dl::match::no_server, drive(ds::disc_state::no_server, now)) == "BLES01876 'FIFA 14': locked (FTP server off)");
    CHECK(dl::row_log_line(r, dl::match::exact, fifa_disc(now)) == "BLES01876 'FIFA 14': unlocked (disc in the drive: exact)");
    CHECK(dl::row_log_line(r, dl::match::same_title, disc("BLES01876", "FIFA 14", 0x1111, 0, now)) ==
        "BLES01876 'FIFA 14': unlocked (same TITLE_ID, another pressing: identity 89abcdef01234567 != 0000000000001111)");
    CHECK(dl::row_log_line(r, dl::match::same_title, disc("BLES01876", "FIFA 14", 0x1111, 0, now), false).starts_with("BLES01876 'FIFA 14': locked (same TITLE_ID"));
}

bool has_line(const dl::gate& g, std::string_view text)
{
    for (const auto& line : g.lines())
        if (line.text.find(text) != std::string::npos) return true;
    return false;
}

void test_gate()
{
    const std::int64_t t = 5'000'000;
    const dl::record r = fifa();
    const auto none = drive(ds::disc_state::unknown, 0);

    // A check from just before X decides at once: boot, no drive read
    {
        dl::gate g(r, "/data/rpcs3/games/f.iso", t);
        CHECK(g.update(fifa_disc(t - 2000), t, false, false) == dl::step::boot && !g.want_check());
        CHECK(g.take_log() == "'/data/rpcs3/games/f.iso': disc BLES01876 present (exact, checked in 0 ms): booting");
        CHECK(g.update(drive(ds::disc_state::no_disc, t + 10), t + 10, false, true) == dl::step::boot); // final
    }
    // An older check says present (the disc may have left since): X waits for a newer one; the request is repeated
    // every second until one comes; a check that started before X is ignored, the next one decides
    {
        dl::gate g(r, "f.iso", t);
        CHECK(g.update(fifa_disc(t - 50'000), t, false, false) == dl::step::verifying && g.want_check());
        CHECK(has_line(g, "Verificando o disco...") && has_line(g, "FIFA 14 (BLES01876)") && has_line(g, "Procurando o disco"));
        CHECK(g.update(fifa_disc(t - 50'000), t + 500, false, false) == dl::step::verifying && !g.want_check());
        CHECK(g.update(fifa_disc(t - 50'000), t + 1000, false, false) == dl::step::verifying && g.want_check());
        CHECK(g.update(fifa_disc(t - 3500), t + 1200, false, false) == dl::step::verifying && !g.want_check());
        CHECK(g.update(drive(ds::disc_state::no_disc, t + 1300), t + 1900, false, false) == dl::step::missing && !g.want_check());
        CHECK(g.take_log() == "'f.iso' not booted: no disc in the drive (state no readable disc, refused: 550 Requested action not taken)");
        CHECK(has_line(g, "Coloque o disco do jogo no PS4") && has_line(g, "Agora não há nenhum disco no leitor.") && has_line(g, "esta tela muda sozinha"));
        // The missing screen asks for a check every 2 s
        CHECK(g.update(drive(ds::disc_state::no_disc, t + 1300), t + 3800, false, false) == dl::step::missing && !g.want_check());
        CHECK(g.update(drive(ds::disc_state::no_disc, t + 1300), t + 3900, false, false) == dl::step::missing && g.want_check());
        CHECK(g.update(drive(ds::disc_state::no_disc, t + 4000), t + 5000, false, false) == dl::step::missing && !g.want_check());
        CHECK(g.update(drive(ds::disc_state::no_disc, t + 4000), t + 5900, false, false) == dl::step::missing && g.want_check());
        // The disc goes in while the screen is up: recognized; X plays
        CHECK(g.update(fifa_disc(t + 6000), t + 7300, false, false) == dl::step::recognized);
        CHECK(g.take_log() == "disc BLES01876 inserted while the screen was shown (after 5 s, exact)");
        CHECK(has_line(g, "Disco reconhecido!") && has_line(g, "O cadeado saiu") && has_line(g, "X: jogar"));
        CHECK(g.update(fifa_disc(t + 6000), t + 7400, false, false) == dl::step::recognized && !g.want_check());
        CHECK(g.update(fifa_disc(t + 6000), t + 9300, false, false) == dl::step::recognized && g.want_check());
        // Removed again before X: back to missing (X then does nothing)
        CHECK(g.update(drive(ds::disc_state::no_disc, t + 9400), t + 9500, true, false) == dl::step::missing);
        CHECK(g.take_log() == "disc BLES01876 no longer seen while the screen was shown (no disc in the drive)");
        CHECK(g.update(fifa_disc(t + 10'000), t + 10'100, false, false) == dl::step::recognized);
        CHECK(g.update(fifa_disc(t + 10'000), t + 10'200, true, false) == dl::step::boot);
        CHECK(g.take_log() == "'f.iso': disc BLES01876 present (exact, X on the screen): booting");
    }
    // The drive is slow: the text changes at 8 s; with no answer at all the missing screen says so at 25 s
    {
        dl::gate g(r, "f.iso", t);
        CHECK(g.update(none, t, false, false) == dl::step::verifying && g.want_check());
        CHECK(g.update(none, t + 7999, false, false) == dl::step::verifying && !has_line(g, "demorando"));
        CHECK(g.update(none, t + 8000, false, false) == dl::step::verifying && has_line(g, "O leitor está demorando para responder. Aguarde..."));
        CHECK(g.update(fifa_disc(t - 90'000), t + 24'999, false, false) == dl::step::verifying);
        CHECK(g.update(fifa_disc(t - 90'000), t + 25'000, false, false) == dl::step::missing);
        CHECK(g.take_log() == "'f.iso' not booted: no drive check came in 25 s");
        CHECK(has_line(g, "O leitor não respondeu. Tire o disco e coloque de novo.") && g.last_match() == dl::match::unknown);
        // An old status that said "present" never opens it; a new check does
        CHECK(g.update(fifa_disc(t - 90'000), t + 30'000, false, false) == dl::step::missing);
        CHECK(g.update(fifa_disc(t + 30'500), t + 31'000, false, false) == dl::step::recognized);
    }
    // The key search holds the shared reader (a forward read past a 2 GiB boundary): the check waits behind it, and so
    // does the screen, past the 25 s; the check that follows decides. Once the search ends without a check: give up.
    {
        dl::gate g(r, "f.iso", t);
        CHECK(g.update(fifa_disc(t - 50'000), t, false, false, true) == dl::step::verifying && g.want_check());
        CHECK(g.update(fifa_disc(t - 50'000), t + 60'000, false, false, true) == dl::step::verifying && g.want_check());
        CHECK(g.take_log() == "'f.iso': the drive is busy with the disc key search: waiting for it");
        CHECK(has_line(g, "O leitor está demorando para responder. Aguarde..."));
        CHECK(g.update(fifa_disc(t - 50'000), t + 61'000, false, false, true) == dl::step::verifying && g.take_log().empty());
        CHECK(g.update(fifa_disc(t + 60'500), t + 62'000, false, false, true) == dl::step::boot);
        dl::gate h(r, "f.iso", t);
        CHECK(h.update(none, t + 30'000, false, false, true) == dl::step::verifying);
        CHECK(h.update(none, t + 30'050, false, false, false) == dl::step::missing);
        CHECK(h.take_log() == "'f.iso' not booted: no drive check came in 30 s");
    }
    // FTP off: the screen says how to turn it on; on, the drive is seen, then the disc
    {
        dl::gate g(r, "f.iso", t);
        CHECK(g.update(drive(ds::disc_state::no_server, t + 20), t + 100, false, false) == dl::step::missing);
        CHECK(has_line(g, "O app não consegue ver o leitor: o FTP do GoldHEN está desligado.") && has_line(g, "Configurações > GoldHEN > Server Settings"));
        CHECK(g.take_log().find("not booted: FTP server off (state no FTP server") != std::string::npos);
        CHECK(g.update(drive(ds::disc_state::no_disc, t + 2100), t + 2200, false, false) == dl::step::missing && has_line(g, "nenhum disco"));
        CHECK(g.update(fifa_disc(t + 4100), t + 4200, false, false) == dl::step::recognized);
    }
    // Another PS3 disc: named on the screen; not a PS3 disc; another pressing of this game opens it
    {
        dl::gate g(r, "f.iso", t);
        CHECK(g.update(disc("BLUS30443", "Demon's \"Souls\"", 1, 2, t + 50), t + 60, false, false) == dl::step::missing);
        CHECK(has_line(g, "O disco no leitor é de outro jogo:") && has_line(g, "\"Demon's 'Souls'\" (BLUS30443).") && has_line(g, "Troque pelo disco deste jogo."));
        CHECK(g.take_log() == "'f.iso' not booted: another PS3 disc: BLUS30443 (state PS3 disc)");
        CHECK(g.update(drive(ds::disc_state::not_ps3, t + 2100), t + 2200, false, false) == dl::step::missing && has_line(g, "não é um disco de PS3"));
        CHECK(g.update(disc("BLES01876", "FIFA 14", 0x77, 0x88, t + 4100), t + 4200, false, false) == dl::step::recognized);
        CHECK(g.last_match() == dl::match::same_title);
    }
    // Strict mode: another pressing stays locked
    {
        dl::gate_config strict;
        strict.other_pressing = false;
        dl::gate g(r, "f.iso", t, strict);
        CHECK(g.update(disc("BLES01876", "FIFA 14", 0x77, 0x88, t + 50), t + 60, false, false) == dl::step::missing);
    }
    // Circle leaves from every screen
    {
        dl::gate verifying(r, "f.iso", t);
        CHECK(verifying.update(none, t + 10, false, true) == dl::step::back);
        CHECK(verifying.take_log() == "'f.iso': screen closed with Circle (drive not checked yet)");
        dl::gate missing(r, "f.iso", t);
        missing.update(drive(ds::disc_state::no_disc, t + 1), t + 2, false, false);
        CHECK(missing.update(drive(ds::disc_state::no_disc, t + 1), t + 500, true, true) == dl::step::back);
        dl::gate recognized(r, "f.iso", t);
        recognized.update(drive(ds::disc_state::no_disc, t + 1), t + 2, false, false);
        recognized.update(fifa_disc(t + 3), t + 4, false, false);
        CHECK(recognized.current() == dl::step::recognized && recognized.update(fifa_disc(t + 3), t + 5, true, true) == dl::step::back);
        CHECK(recognized.take_log() == "'f.iso': screen closed with Circle (disc in the drive: exact)");
    }
}

void test_texts()
{
    const std::int64_t t = 1'000'000;
    dl::record longest = fifa();
    longest.title.clear();
    for (int i = 0; i < 40; ++i) longest.title += "Ação \"X\" ";
    longest.title = dl::detail::clean(longest.title, dl::title_limit);
    std::vector<dl::record> games{fifa(), longest, dl::unreadable_record("BCUS98232", "garbage")};
    std::string long_other;
    for (int i = 0; i < 30; ++i) long_other += "Ü";
    long_other += " Very Long Other Game Title Without Any End In Sight";
    const std::vector<ds::disc_status> statuses{drive(ds::disc_state::no_disc, t + 1), drive(ds::disc_state::not_ps3, t + 1), drive(ds::disc_state::no_server, t + 1),
        disc("BLUS30443", long_other, 1, 2, t + 1), disc("BLUS30443", "", 1, 2, t + 1), disc("XXXX", "", 1, 2, t + 1), fifa_disc(t + 1)};
    for (const dl::record& r : games)
    {
        const std::string message = dl::message_text(r);
        CHECK(message.size() <= 116 && message.find('\n') == std::string::npos && valid_utf8(message) && message.starts_with("Insira o disco do jogo "));
        for (const ds::disc_status& s : statuses)
            for (const bool timeout : {false, true})
            {
                dl::gate g(r, "x.iso", t);
                g.update(timeout ? drive(ds::disc_state::unknown, 0) : s, timeout ? t + 30'000 : t + 2, false, false);
                for (const auto& line : g.lines())
                {
                    if (line.text.size() > 70 || !valid_utf8(line.text) || line.text.find('\n') != std::string::npos || line.y > 690)
                        std::fprintf(stderr, "screen line (%d, %zu bytes): %s\n", line.y, line.text.size(), line.text.c_str());
                    CHECK(line.text.size() <= 70 && valid_utf8(line.text) && line.text.find('\n') == std::string::npos && line.y > 0 && line.y <= 690);
                }
            }
    }
    CHECK(dl::message_text(fifa()) == "Insira o disco do jogo \"FIFA 14\" no PS4 para jogar.");
    CHECK(dl::message_text(dl::unreadable_record("BCUS98232", "x")) == "Insira o disco do jogo BCUS98232 no PS4 para jogar.");
    CHECK(dl::message_text(longest).find('"', 24) == dl::message_text(longest).size() - 20); // inner quotes replaced
    CHECK(std::string_view(dl::row_hint()).size() <= 116);
    CHECK(dl::game_name(fifa()) == "FIFA 14 (BLES01876)" && dl::game_name(dl::unreadable_record("BCUS98232", "x")) == "BCUS98232");
}

void test_token_and_cadence()
{
    dl::verified_boots boots;
    boots.remember("/data/rpcs3/games/a.iso", 1000);
    CHECK(!boots.take("/data/rpcs3/games/b.iso", 1001));
    CHECK(boots.take("/data/rpcs3/games/a.iso", 31'000));
    CHECK(!boots.take("/data/rpcs3/games/a.iso", 31'001)); // one use
    boots.remember("/data/rpcs3/games/a.iso", 1000);
    CHECK(!boots.take("/data/rpcs3/games/a.iso", 31'001)); // expired
    CHECK(!boots.take("/data/rpcs3/games/a.iso", 999));

    // XMB cadence: only a selected locked game with another disc (or a non-PS3 disc) in, while the player is active
    CHECK(dl::forced_check_ms(true, true, dl::match::other_disc, 0) == 10000);
    CHECK(dl::forced_check_ms(true, true, dl::match::not_ps3, 59'999) == 10000);
    CHECK(dl::forced_check_ms(true, true, dl::match::other_disc, 60'000) == 0);
    CHECK(dl::forced_check_ms(false, true, dl::match::other_disc, 0) == 0);
    CHECK(dl::forced_check_ms(true, false, dl::match::other_disc, 0) == 0);
    for (const dl::match m : {dl::match::unknown, dl::match::stale, dl::match::no_server, dl::match::no_disc, dl::match::same_title, dl::match::exact})
        CHECK(dl::forced_check_ms(true, true, m, 0) == 0);
}

void test_padlock()
{
    namespace xmb = utils::ps4::xmb;
    const auto start = std::chrono::steady_clock::now();
    const xmb::image lock = xmb::padlock_icon();
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::printf("padlock 100x120 drawn in %.2f ms (host)\n", ms);
    CHECK(lock && lock.width == 100 && lock.height == 120);
    const auto alpha = [&](int x, int y) { return lock.pixels[static_cast<std::size_t>(y) * 100 + static_cast<std::size_t>(x)] >> 24; };
    unsigned max_alpha = 0, clear = 0, dark_clear = 0;
    for (const std::uint32_t p : lock.pixels)
    {
        max_alpha = std::max(max_alpha, p >> 24);
        if (!(p >> 24))
        {
            ++clear;
            if ((p & 0xffffff) != 0xd8d8e4) ++dark_clear;
        }
    }
    std::printf("padlock: max alpha %u, %u transparent texels, keyhole centre alpha %u, below the body %u\n", max_alpha, clear, alpha(50, 57), alpha(50, 92));
    CHECK(max_alpha <= 230 && max_alpha >= 150 && clear > 2000 && dark_clear == 0);
    CHECK(alpha(50, 57) <= 26);            // the keyhole shows the game behind it
    CHECK(alpha(50, 72) <= 26);            // and so does its slot
    CHECK(alpha(50, 92) > 0);              // the soft shadow below the body
    CHECK(alpha(25, 70) >= 100 && alpha(75, 50) >= 100); // the body
    CHECK(alpha(2, 2) == 0 && alpha(97, 118) == 0);
    CHECK(alpha(22, 35) >= 100 && alpha(78, 35) >= 100); // the shackle legs
    CHECK(alpha(50, 22) < alpha(50, 12));  // inside the shackle only its shadow
    CHECK(!xmb::padlock_icon(4, 4) && !xmb::padlock_icon(1000, 1000) && !xmb::padlock_icon(100, 7));
    CHECK(xmb::padlock_icon().pixels == lock.pixels); // deterministic
    const xmb::image small = xmb::padlock_icon(20, 24);
    CHECK(small && small.pixels.size() == 480);
}

// A failed drive check as ds::drive::check fills it
ds::disc_status failed_check(std::int64_t checked_ms, ds::read_status read, int reply, const std::string& detail)
{
    ds::disc_status s;
    s.state = read == ds::read_status::no_server ? ds::disc_state::no_server : ds::disc_state::no_disc;
    s.read = read;
    s.reply = reply;
    s.detail = std::string(ds::to_string(read)) + ": " + detail;
    s.checked_ms = checked_ms;
    return s;
}

// The empty drive of the console (0.37 logs): RETR accepted, the transfer ends at once with 451
ds::disc_status empty_drive(std::int64_t checked_ms)
{
    return failed_check(checked_ms, ds::read_status::failed, 451, "read at LBA 0x0: transfer failed: '451 Requested action aborted. Local error in processing.'");
}

ds::disc_status refused_drive(std::int64_t checked_ms)
{
    return failed_check(checked_ms, ds::read_status::refused, 550, "REST 0 + RETR: refused: '550 Requested action not taken. File unavailable.'");
}

ds::disc_status timed_out(std::int64_t checked_ms)
{
    return failed_check(checked_ms, ds::read_status::failed, 0, "read at LBA 0x0: timed out");
}

ds::disc_status server_off(std::int64_t checked_ms)
{
    return failed_check(checked_ms, ds::read_status::no_server, 0, "127.0.0.1:2121: connect failed: ECONNREFUSED");
}

std::string take(dl::removal_watch& w)
{
    std::string out;
    for (const std::string& line : w.take_log()) out += line + "\n";
    return out;
}

void test_sighting()
{
    const dl::record r = fifa();
    const std::int64_t t = 1'000'000;
    // The server read the drive and said there is nothing: 451 at the end of the transfer, an early end
    CHECK(ds::drive_answered_empty(empty_drive(t)) && !ds::drive_answered_empty(refused_drive(t)));
    CHECK(ds::drive_answered_empty(failed_check(t, ds::read_status::out_of_range, 0, "")));
    CHECK(ds::drive_answered_empty(failed_check(t, ds::read_status::failed, 451, "x")) && !ds::drive_answered_empty(failed_check(t, ds::read_status::failed, 550, "x")));
    // Anything else says nothing about the drive: time-outs, dropped connections, other replies (450: file busy; 550:
    // the device could not be opened at that moment), the server off
    for (const int reply : {0, 421, 425, 426, 450, 500, 530, 550})
        CHECK(!ds::drive_answered_empty(failed_check(t, ds::read_status::failed, reply, "x")));
    CHECK(!ds::drive_answered_empty(timed_out(t)) && !ds::drive_answered_empty(server_off(t)));
    CHECK(!ds::drive_answered_empty(failed_check(t, ds::read_status::stopped, 451, "")) && !ds::drive_answered_empty(failed_check(t, ds::read_status::no_memory, 0, "")));
    CHECK(!ds::drive_answered_empty(fifa_disc(t)) && !ds::drive_answered_empty(drive(ds::disc_state::not_ps3, t)));
    ds::disc_status odd = empty_drive(t);
    odd.state = ds::disc_state::ps3_disc;
    CHECK(!ds::drive_answered_empty(odd));

    // A running game's disc: present, missing or nothing known
    CHECK(dl::sight(r, fifa_disc(t)) == dl::sighting::present);
    CHECK(dl::sight(r, disc("BLES01876", "FIFA 14", 0x77, 0x88, t)) == dl::sighting::present);        // another pressing
    CHECK(dl::sight(r, disc("BLES01876", "FIFA 14", 0x77, 0x88, t), false) == dl::sighting::missing); // strict mode
    CHECK(dl::sight(dl::unreadable_record("BLES01876", "x"), fifa_disc(t)) == dl::sighting::present);
    CHECK(dl::sight(r, disc("BLUS30443", "Demon's Souls", 1, 2, t)) == dl::sighting::missing);
    CHECK(dl::sight(r, drive(ds::disc_state::not_ps3, t)) == dl::sighting::missing);
    CHECK(dl::sight(r, empty_drive(t)) == dl::sighting::missing && dl::sight(r, refused_drive(t)) == dl::sighting::unknown);
    CHECK(dl::sight(r, timed_out(t)) == dl::sighting::unknown && dl::sight(r, server_off(t)) == dl::sighting::unknown);
    CHECK(dl::sight(r, failed_check(t, ds::read_status::failed, 425, "425 Can't open data connection.")) == dl::sighting::unknown);
    CHECK(dl::sight(r, drive(ds::disc_state::unknown, t)) == dl::sighting::unknown);
    CHECK(dl::sight(r, fifa_disc(0)) == dl::sighting::unknown && dl::sight(r, empty_drive(0)) == dl::sighting::unknown);
    CHECK(dl::sight(r, fifa_disc(5)) == dl::sighting::present); // judged as it was checked, whatever its age
    CHECK(std::string(dl::removed_notice) == "O disco foi removido." && valid_utf8(dl::removed_notice));
}

void test_removal_watch()
{
    const dl::record r = fifa();
    const std::int64_t t = 5'000'000;

    // Cadence: the first check 3 s after the boot; the next one 3 s after the last request once it was answered, or
    // after 15 s; checks from before the boot and a check seen again count for nothing
    {
        dl::removal_watch w(r, t);
        CHECK(!w.update(empty_drive(t - 10), t) && !w.want_check() && w.checks() == 0);
        CHECK(!w.update(empty_drive(t - 10), t + 2999) && !w.want_check());
        CHECK(!w.update(empty_drive(t - 10), t + 3000) && w.want_check() && w.misses() == 0);
        CHECK(!w.update(empty_drive(t - 10), t + 3250) && !w.want_check());
        CHECK(!w.update(fifa_disc(t + 3001), t + 3500) && !w.want_check() && w.checks() == 1 && w.misses() == 0);
        CHECK(!w.update(fifa_disc(t + 3001), t + 5999) && !w.want_check() && w.checks() == 1);
        CHECK(!w.update(fifa_disc(t + 3001), t + 6000) && w.want_check());
        CHECK(!w.update(fifa_disc(t + 3001), t + 9000) && !w.want_check());
        CHECK(!w.update(fifa_disc(t + 3001), t + 20'999) && !w.want_check());
        CHECK(!w.update(fifa_disc(t + 3001), t + 21'000) && w.want_check());
        CHECK(take(w).empty());
        // No check for 30 s (no drive worker): logged once, and the next check too
        CHECK(!w.update(fifa_disc(t + 3001), t + 33'001));
        CHECK(take(w) == "no drive check came in 30 s: the disc of BLES01876 is not watched until the drive answers\n");
        CHECK(!w.update(fifa_disc(t + 3001), t + 40'000) && take(w).empty());
        CHECK(!w.update(fifa_disc(t + 40'501), t + 40'600) && take(w) == "the drive answers again after 37 s without a check\n");
        CHECK(w.checks() == 2 && !w.removed());
    }
    // Three checks in a row without the disc spanning at least 10 s: removed, once; the watch goes on and the disc seen
    // again is its return (back), once
    using ev = dl::watch_event;
    {
        dl::removal_watch w(r, t);
        CHECK(w.step(fifa_disc(t + 3000), t + 3005) == ev::none);
        CHECK(w.step(empty_drive(t + 6000), t + 6800) == ev::none && w.misses() == 1 && !w.removed());
        CHECK(take(w) == "BLES01876 not in the drive (no disc in the drive: failed: read at LBA 0x0: transfer failed: '451 Requested action aborted. "
                         "Local error in processing.'): 3 checks over 10 s decide\n");
        CHECK(w.step(empty_drive(t + 9000), t + 9600) == ev::none && w.misses() == 2);
        CHECK(w.step(empty_drive(t + 12'000), t + 12'600) == ev::none && w.misses() == 3 && !w.removed()); // three, over 6 s only
        CHECK(w.step(empty_drive(t + 16'000), t + 16'600) == ev::removed && w.removed() && w.misses() == 4 && w.removals() == 1);
        const std::string line = take(w);
        CHECK(line.starts_with("FIFA 14 (BLES01876): not in the drive (no disc in the drive: failed: read at LBA 0x0") &&
            line.ends_with("; 4 checks over 10 s): disc removed during the game\n"));
        CHECK(w.step(empty_drive(t + 19'000), t + 19'500) == ev::none && w.removed() && take(w).empty());
        CHECK(w.step(empty_drive(t + 19'000), t + 22'500) == ev::none && w.want_check()); // still asked while the disc is away
        CHECK(w.step(fifa_disc(t + 22'501), t + 22'510) == ev::back && !w.removed() && w.misses() == 0);
        CHECK(take(w) == "FIFA 14 (BLES01876): in the drive again 6 s after the removal (exact): the game can go on\n");
        CHECK(w.step(fifa_disc(t + 25'600), t + 25'610) == ev::none);
        CHECK(w.summary() == "8 checks, 5 without the disc, 0 without an answer, 1 removal(s)");
        // A second removal later is reported again
        for (std::int64_t at = t + 30'000; at <= t + 40'000; at += 5000) w.step(empty_drive(at), at + 10);
        CHECK(w.removed() && w.removals() == 2);
    }
    // The update() form: true once per removal
    {
        dl::removal_watch w(r, t);
        CHECK(!w.update(empty_drive(t + 3000), t + 3010) && !w.update(empty_drive(t + 8000), t + 8010) && w.update(empty_drive(t + 13'000), t + 13'010));
        CHECK(!w.update(empty_drive(t + 16'000), t + 16'010) && w.removed());
    }
    // Checks are asked every 3 s, also after a miss
    {
        dl::removal_watch w(r, t);
        CHECK(w.step(fifa_disc(t - 1), t + 3000) == ev::none && w.want_check());
        CHECK(w.step(empty_drive(t + 3001), t + 3700) == ev::none && w.misses() == 1 && !w.want_check());
        CHECK(w.step(empty_drive(t + 3001), t + 5999) == ev::none && !w.want_check());
        CHECK(w.step(empty_drive(t + 3001), t + 6000) == ev::none && w.want_check());
    }
    // RETR 550 is not a miss; misses closer together than 10 s make no removal however many they are
    {
        dl::removal_watch w(r, t);
        CHECK(w.step(refused_drive(t + 3000), t + 3010) == ev::none && w.misses() == 0 && w.unanswered() == 1);
        for (std::int64_t i = 0; i < 9; ++i) CHECK(w.step(empty_drive(t + 4000 + i), t + 4010 + i) == ev::none);
        CHECK(w.misses() == 9 && !w.removed());
        CHECK(w.step(empty_drive(t + 14'000), t + 14'010) == ev::removed && w.removed());
    }
    // A shorter rule (confirm_misses 1, confirm_ms 0): the first miss is the removal
    {
        dl::watch_config at_once;
        at_once.confirm_misses = 1;
        at_once.confirm_ms = 0;
        dl::removal_watch w(r, t, at_once);
        CHECK(w.step(empty_drive(t + 3000), t + 3010) == ev::removed && w.removed());
        CHECK(w.step(fifa_disc(t + 6000), t + 6010) == ev::back);
    }
    // Checks without an answer about the drive never count, however many: FTP off, time-outs, other replies. The first
    // is logged at once, then one line a minute with the number left out.
    {
        dl::removal_watch w(r, t);
        std::string lines;
        for (int i = 1; i <= 60; ++i)
        {
            const std::int64_t at = t + 3000 * i;
            const ds::disc_status s = i % 3 == 0 ? server_off(at) : i % 3 == 1 ? timed_out(at) : failed_check(at, ds::read_status::failed, 425, "425 Can't open data connection.");
            CHECK(!w.update(s, at + 50));
            lines += take(w);
        }
        CHECK(!w.removed() && w.misses() == 0 && w.unanswered() == 60 && w.checks() == 60);
        CHECK(std::count(lines.begin(), lines.end(), '\n') == 3);
        CHECK(lines.starts_with("drive check without an answer about the disc (no answer about the drive: failed: read at LBA 0x0: timed out): not counted\n"));
        CHECK(lines.find("(FTP server off: no FTP server: 127.0.0.1:2121: connect failed: ECONNREFUSED): not counted (19 more since the last line)\n") != std::string::npos);
        // Misses among failed checks still add up
        CHECK(w.step(empty_drive(t + 200'000), t + 200'050) == ev::none && w.misses() == 1);
        CHECK(w.step(server_off(t + 203'000), t + 203'050) == ev::none && w.misses() == 1);
        CHECK(w.step(timed_out(t + 206'000), t + 206'050) == ev::none && w.misses() == 1);
        CHECK(w.step(empty_drive(t + 209'000), t + 209'050) == ev::none && w.misses() == 2);
        CHECK(w.step(empty_drive(t + 212'000), t + 212'050) == ev::removed && w.removed());
    }
    // The disc back before the removal: the game goes on and the count starts again
    {
        dl::removal_watch w(r, t);
        CHECK(w.step(empty_drive(t + 3000), t + 3010) == ev::none && w.misses() == 1);
        take(w);
        CHECK(w.step(fifa_disc(t + 6000), t + 6010) == ev::none && w.misses() == 0 && !w.removed());
        CHECK(take(w) == "BLES01876 is in the drive again 3000 ms after the check without it: the game goes on\n");
        CHECK(w.step(empty_drive(t + 9000), t + 9010) == ev::none && w.misses() == 1);
        CHECK(w.step(disc("BLES01876", "FIFA 14", 0x77, 0x88, t + 12'000), t + 12'010) == ev::none && w.misses() == 0); // another pressing of it
        CHECK(w.step(empty_drive(t + 15'000), t + 15'010) == ev::none && w.step(empty_drive(t + 18'000), t + 18'010) == ev::none);
        CHECK(w.step(empty_drive(t + 21'000), t + 21'010) == ev::none && w.misses() == 3);
        CHECK(w.step(empty_drive(t + 25'000), t + 25'010) == ev::removed && w.removed());
    }
    // A disc that keeps coming and going (one check without it, one with it) for 10 minutes: never removed, and the log
    // stays small (20 lines, then one a minute)
    {
        dl::removal_watch w(r, t);
        std::string lines;
        for (int i = 1; i <= 200; ++i)
        {
            const std::int64_t at = t + 3000 * i;
            CHECK(!w.update(i % 2 ? empty_drive(at) : fifa_disc(at), at + 10));
            lines += take(w);
        }
        const auto count = std::count(lines.begin(), lines.end(), '\n');
        CHECK(!w.removed() && count >= 25 && count <= 32 && lines.find(" such lines left out)") != std::string::npos);
    }
    // Another game's disc, or a disc that is not a PS3 game, is not the disc; strict mode: neither is another pressing
    {
        dl::removal_watch w(r, t);
        CHECK(w.step(disc("BLUS30443", "Demon's Souls", 1, 2, t + 3000), t + 3010) == ev::none);
        CHECK(take(w) == "BLES01876 not in the drive (another PS3 disc: BLUS30443): 3 checks over 10 s decide\n");
        CHECK(w.step(drive(ds::disc_state::not_ps3, t + 6000), t + 6010) == ev::none && w.step(drive(ds::disc_state::not_ps3, t + 9000), t + 9010) == ev::none);
        CHECK(w.step(drive(ds::disc_state::not_ps3, t + 13'000), t + 13'010) == ev::removed);
        CHECK(take(w).ends_with("(a disc that is not a PS3 game; 4 checks over 10 s): disc removed during the game\n"));
        // Another pressing of the game is its return
        CHECK(w.step(disc("BLES01876", "FIFA 14", 0x77, 0x88, t + 16'000), t + 16'010) == ev::back);
        CHECK(take(w).ends_with("in the drive again 3 s after the removal (same TITLE_ID): the game can go on\n"));
        dl::watch_config strict;
        strict.other_pressing = false;
        dl::removal_watch s(r, t, strict);
        CHECK(s.step(disc("BLES01876", "FIFA 14", 0x77, 0x88, t + 3000), t + 3010) == ev::none && s.misses() == 1);
        CHECK(s.step(disc("BLES01876", "FIFA 14", 0x77, 0x88, t + 7000), t + 7010) == ev::none);
        CHECK(s.step(disc("BLES01876", "FIFA 14", 0x77, 0x88, t + 13'000), t + 13'010) == ev::removed);
    }
}

// The GoldHEN FTP server (v2.2) as the console answered in the 0.37 tests, for real drive checks: RETR /dev/cd0 of an
// empty drive is accepted (150) and the transfer ends at once with 451. 'refuse' answers RETR with 550, 'off' refuses
// connections, 'stall' never sends data, 'reset' drops the data connection, 'short_device' ends the device after one
// sector (226). One thread.
class console_ftp
{
public:
    enum class tray
    {
        disc,
        empty,
        refuse,
        off,
        stall,
        reset,
        short_device,
    };
    tray now = tray::disc;
    std::vector<unsigned char> image;

    ds::net_ops ops()
    {
        ds::net_ops o;
        o.connect = [this](const ds::endpoint& to, int, int, int& error) { return connect(to, error); };
        o.send = [this](int fd, const void* data, std::size_t n, int, int& error) { return send(fd, static_cast<const char*>(data), n, error); };
        o.recv = [this](int fd, void* out, std::size_t n, int, int& error) { return recv(fd, static_cast<unsigned char*>(out), n, error); };
        o.close = [this](int fd) { close(fd); };
        return o;
    }

private:
    struct socket
    {
        int control = -1;          // a data connection: its control connection
        std::string replies;       // a control connection: replies not read yet
        std::string commands;      // and commands not complete yet
        std::uint16_t offered = 0; // the port of its last PASV
        int data = -1;             // the data connection made to it
        std::uint64_t rest = 0, pos = 0;
        bool sending = false, ended = false;
    };
    std::map<int, socket> m_sockets;
    int m_next = 3;
    std::uint16_t m_port = 40000;

    int connect(const ds::endpoint& to, int& error)
    {
        if (now == tray::off)
        {
            error = ECONNREFUSED;
            return -1;
        }
        if (to.port == ds::default_port)
        {
            m_sockets[m_next].replies = "220 GoldHEN FTP server v2.2\r\n";
            return m_next++;
        }
        for (auto& [fd, s] : m_sockets)
        {
            if (!s.offered || s.offered != to.port) continue;
            s.offered = 0;
            s.data = m_next;
            m_sockets[m_next].control = fd;
            return m_next++;
        }
        error = ECONNREFUSED;
        return -1;
    }

    void command(int fd, const std::string& line)
    {
        socket& s = m_sockets[fd];
        const std::string verb = line.substr(0, line.find(' '));
        if (verb == "USER" || verb == "PASS") s.replies += "230 User logged in, proceed.\r\n";
        else if (verb == "TYPE" || verb == "NOOP") s.replies += "200 Command okay.\r\n";
        else if (verb == "PASV")
        {
            s.offered = m_port++;
            char text[80];
            std::snprintf(text, sizeof(text), "227 Entering Passive Mode (127,0,0,1,%u,%u).\r\n", unsigned{s.offered} >> 8, unsigned{s.offered} & 0xffu);
            s.replies += text;
        }
        else if (verb == "REST")
        {
            s.rest = std::stoull(line.substr(5));
            s.replies += "350 Resuming at " + line.substr(5) + ".\r\n";
        }
        else if (verb == "RETR")
        {
            const std::uint64_t at = std::exchange(s.rest, 0);
            if (now == tray::refuse) s.replies += "550 Requested action not taken. File unavailable.\r\n";
            else if (s.data < 0) s.replies += "425 Can't open data connection.\r\n";
            else
            {
                s.replies += "150 File status okay; about to open data connection.\r\n";
                socket& d = m_sockets[s.data];
                d.sending = true;
                d.pos = at;
            }
        }
        else if (verb == "QUIT") s.replies += "221 Service closing control connection.\r\n";
        else s.replies += "502 Command not implemented.\r\n";
    }

    std::int64_t send(int fd, const char* data, std::size_t n, int& error)
    {
        const auto found = m_sockets.find(fd);
        if (found == m_sockets.end())
        {
            error = EPIPE;
            return -1;
        }
        if (found->second.control >= 0) return static_cast<std::int64_t>(n);
        found->second.commands.append(data, n);
        for (std::size_t eol; (eol = m_sockets[fd].commands.find("\r\n")) != std::string::npos;)
        {
            const std::string line = m_sockets[fd].commands.substr(0, eol);
            m_sockets[fd].commands.erase(0, eol + 2);
            command(fd, line);
        }
        return static_cast<std::int64_t>(n);
    }

    void end(socket& d, const char* reply)
    {
        d.ended = true;
        if (const auto c = m_sockets.find(d.control); c != m_sockets.end())
        {
            c->second.replies += reply;
            c->second.data = -1;
        }
    }

    std::int64_t recv(int fd, unsigned char* out, std::size_t n, int& error)
    {
        const auto found = m_sockets.find(fd);
        if (found == m_sockets.end())
        {
            error = ENOTCONN;
            return -1;
        }
        socket& s = found->second;
        const auto wait = [&error]
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            error = ETIMEDOUT;
            return std::int64_t{-1};
        };
        if (s.control < 0)
        {
            if (s.replies.empty()) return wait();
            const std::size_t k = std::min(n, s.replies.size());
            std::memcpy(out, s.replies.data(), k);
            s.replies.erase(0, k);
            return static_cast<std::int64_t>(k);
        }
        if (s.ended) return 0;
        if (!s.sending || now == tray::stall) return wait();
        if (now == tray::reset)
        {
            end(s, "426 Connection closed; transfer aborted.\r\n");
            error = ECONNRESET;
            return -1;
        }
        const bool readable = now == tray::disc || now == tray::short_device;
        const std::uint64_t size = now == tray::short_device ? std::min<std::uint64_t>(image.size(), 2048) : image.size();
        if (!readable || s.pos >= size)
        {
            end(s, readable ? "226 Closing data connection. Requested file action successful.\r\n" : "451 Requested action aborted. Local error in processing.\r\n");
            return 0;
        }
        const std::size_t k = static_cast<std::size_t>(std::min<std::uint64_t>({n, size - s.pos, 65536}));
        std::memcpy(out, image.data() + s.pos, k);
        s.pos += k;
        return static_cast<std::int64_t>(k);
    }

    void close(int fd)
    {
        const auto found = m_sockets.find(fd);
        if (found == m_sockets.end()) return;
        socket& s = found->second;
        if (s.control >= 0 && s.sending && !s.ended) end(s, "426 Connection closed; transfer aborted.\r\n");
        else if (const auto c = m_sockets.find(s.control); s.control >= 0 && c != m_sockets.end() && c->second.data == fd) c->second.data = -1;
        m_sockets.erase(found);
    }
};

// A PARAM.SFO with UTF-8 values
std::string sfo_text(const std::vector<std::pair<std::string, std::string>>& values)
{
    std::vector<unsigned char> table, head(20, 0);
    std::string keys, data;
    for (const auto& [key, value] : values)
    {
        std::vector<unsigned char> entry(16, 0);
        entry[0] = static_cast<unsigned char>(keys.size());
        entry[2] = 0x04;
        entry[3] = 0x02;
        le32_at(entry, 4, static_cast<std::uint32_t>(value.size() + 1));
        le32_at(entry, 8, static_cast<std::uint32_t>(value.size() + 1));
        le32_at(entry, 12, static_cast<std::uint32_t>(data.size()));
        table.insert(table.end(), entry.begin(), entry.end());
        keys += key + '\0';
        data += value + '\0';
    }
    while (keys.size() % 4) keys += '\0';
    std::memcpy(head.data(), "\0PSF", 4);
    le32_at(head, 4, 0x101);
    le32_at(head, 8, static_cast<std::uint32_t>(20 + table.size()));
    le32_at(head, 12, static_cast<std::uint32_t>(20 + table.size() + keys.size()));
    le32_at(head, 16, static_cast<std::uint32_t>(values.size()));
    return std::string(head.begin(), head.end()) + std::string(table.begin(), table.end()) + keys + data;
}

// The small disc of small_image() with a valid region table (one plain region, LBA 0 to 63) and PARAM.SFO
std::vector<unsigned char> console_disc()
{
    std::vector<unsigned char> image = small_image(sfo_text({{"TITLE", "FIFA 14"}, {"TITLE_ID", "BLES01876"}}));
    std::fill(image.begin(), image.begin() + 24, 0);
    image[3] = 1;
    image[15] = 63;
    return image;
}

// A drive on 'server' with short time limits and two attempts per read
std::unique_ptr<ds::drive> console_drive(console_ftp& server, std::int64_t stats_ms = 1'000'000'000, ds::log_fn log = {})
{
    ds::reader_config cfg;
    cfg.chunk_bytes = 64 * 1024;
    cfg.cache_chunks = 4;
    cfg.attempts = 2;
    cfg.stats_interval_ms = stats_ms;
    cfg.net.connect_ms = 100;
    cfg.net.reply_ms = 200;
    cfg.net.first_byte_ms = 150;
    cfg.net.data_ms = 150;
    cfg.net.slice_ms = 10;
    return std::make_unique<ds::drive>(std::make_unique<ds::reader>(server.ops(), ds::server_list{{{ds::loopback_ipv4, ds::default_port}}, {}, {}}, cfg, log),
        ds::drive::exists_fn{}, "", ds::drive_config{}, log);
}

// Checks every 3 s for a whole game: successful ones alone write no reader statistics line; failed ones still do
void test_check_statistics()
{
    console_ftp server;
    server.image = console_disc();
    std::vector<std::string> lines;
    const std::unique_ptr<ds::drive> drv = console_drive(server, 1, [&lines](const std::string& line) { lines.push_back(line); });
    const auto count = [&lines](std::string_view text)
    {
        return std::count_if(lines.begin(), lines.end(), [text](const std::string& line) { return line.find(text) != std::string::npos; });
    };
    // A new disc: its PARAM.SFO is read too (64 KiB), which the next check's line reports
    CHECK(drv->check_now().state == ds::disc_state::ps3_disc);
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
    CHECK(drv->check_now().state == ds::disc_state::ps3_disc);
    const auto after_first = count("disc stream: last ");
    CHECK(after_first == 1);
    for (int i = 0; i < 5; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
        CHECK(drv->check_now().state == ds::disc_state::ps3_disc);
    }
    CHECK(count("disc stream: last ") == after_first);
    server.now = console_ftp::tray::empty;
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
    CHECK(drv->check_now().state == ds::disc_state::no_disc);
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
    CHECK(drv->check_now().state == ds::disc_state::no_disc);
    CHECK(count("disc stream: last ") > after_first && count("errors") > 0);
}

// poll(false), as the watch calls it close to the thread limit: no worker is started, a running one is kept
void test_drive_worker_start()
{
    console_ftp server;
    server.image = console_disc();
    const std::unique_ptr<ds::drive> drv = console_drive(server);
    CHECK(drv->poll(false).checked_ms == 0);
    drv->request_check();
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    CHECK(drv->poll(false).checked_ms == 0 && drv->last().state == ds::disc_state::unknown);
    drv->poll();
    for (int i = 0; i < 300 && drv->last().checked_ms == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(drv->last().state == ds::disc_state::ps3_disc);
    const std::int64_t first = drv->poll(false).checked_ms;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    drv->request_check();
    for (int i = 0; i < 300 && drv->last().checked_ms == first; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK(drv->poll(false).checked_ms > first); // the running worker answers the request
}

void test_drive_checks()
{
    console_ftp server;
    server.image = console_disc();
    const std::unique_ptr<ds::drive> owned = console_drive(server);
    ds::drive& drv = *owned;
    const auto check = [&](console_ftp::tray tray)
    {
        server.now = tray;
        std::this_thread::sleep_for(std::chrono::milliseconds(2)); // each check starts in a millisecond of its own
        return drv.check_now();
    };

    const ds::disc_status in = check(console_ftp::tray::disc);
    CHECK(in.state == ds::disc_state::ps3_disc && in.read == ds::read_status::ok && in.reply == 0 && in.title_id == "BLES01876" && in.fingerprint);
    CHECK(in.title == "FIFA 14" && in.sfo_title_id == "BLES01876");
    dl::record r = fifa();
    r.identity = in.identity;
    r.fingerprint = in.fingerprint;
    CHECK(dl::classify(r, in, in.checked_ms) == dl::match::exact && dl::sight(r, in) == dl::sighting::present);

    // What each kind of failure leaves in the status
    const ds::disc_status empty = check(console_ftp::tray::empty);
    CHECK(empty.state == ds::disc_state::no_disc && empty.read == ds::read_status::failed && empty.reply == 451);
    CHECK(empty.detail.find("'451 Requested action aborted. Local error in processing.'") != std::string::npos);
    CHECK(ds::drive_answered_empty(empty) && dl::sight(r, empty) == dl::sighting::missing);
    const ds::disc_status refused = check(console_ftp::tray::refuse);
    CHECK(refused.state == ds::disc_state::no_disc && refused.read == ds::read_status::refused && refused.reply == 550 && dl::sight(r, refused) == dl::sighting::unknown);
    const ds::disc_status off = check(console_ftp::tray::off);
    CHECK(off.state == ds::disc_state::no_server && off.read == ds::read_status::no_server && off.reply == 0 && dl::sight(r, off) == dl::sighting::unknown);
    const ds::disc_status stall = check(console_ftp::tray::stall);
    CHECK(stall.state == ds::disc_state::no_disc && stall.read == ds::read_status::failed && stall.reply == 0 && dl::sight(r, stall) == dl::sighting::unknown);
    const ds::disc_status reset = check(console_ftp::tray::reset);
    CHECK(reset.state == ds::disc_state::no_disc && reset.read == ds::read_status::failed && reset.reply == 0 && dl::sight(r, reset) == dl::sighting::unknown);
    // A device that ends before sector 2 (226 early): the server read it, there is no disc to see
    const ds::disc_status ended = check(console_ftp::tray::short_device);
    CHECK(ended.state == ds::disc_state::no_disc && ended.read == ds::read_status::out_of_range && ended.reply == 0 && dl::sight(r, ended) == dl::sighting::missing);
    const ds::disc_status back = check(console_ftp::tray::disc);
    CHECK(back.state == ds::disc_state::ps3_disc && back.read == ds::read_status::ok && back.reply == 0 && dl::sight(r, back) == dl::sighting::present);
    CHECK(back.generation != in.generation);

    // The watch on these checks (two misses, confirm_ms 0: they are milliseconds apart). The disc goes and comes back
    // before the second check; it goes again, the server is off, then silent, then refuses RETR (none counted), and the
    // next answer without it is the removal; the disc back is its return.
    dl::watch_config quick;
    quick.confirm_misses = 2;
    quick.confirm_ms = 0;
    dl::removal_watch w(r, ds::detail::now_ms(), quick);
    const auto feed = [&](console_ftp::tray tray)
    {
        const ds::disc_status s = check(tray);
        return w.step(s, ds::detail::now_ms());
    };
    using ev = dl::watch_event;
    CHECK(feed(console_ftp::tray::disc) == ev::none && w.checks() == 1 && w.misses() == 0);
    CHECK(feed(console_ftp::tray::empty) == ev::none && w.misses() == 1);
    CHECK(feed(console_ftp::tray::disc) == ev::none && w.misses() == 0);
    CHECK(feed(console_ftp::tray::empty) == ev::none && w.misses() == 1);
    CHECK(feed(console_ftp::tray::off) == ev::none && w.misses() == 1);
    CHECK(feed(console_ftp::tray::stall) == ev::none && w.misses() == 1);
    CHECK(feed(console_ftp::tray::refuse) == ev::none && w.misses() == 1);
    CHECK(feed(console_ftp::tray::empty) == ev::removed && w.removed());
    CHECK(feed(console_ftp::tray::disc) == ev::back && !w.removed());
    CHECK(w.summary() == "9 checks, 3 without the disc, 3 without an answer, 1 removal(s)");
    const std::string log = take(w);
    CHECK(log.find("FIFA 14 (BLES01876): not in the drive (no disc in the drive: failed: read at LBA 0x0: transfer failed: ") != std::string::npos);

    // A stopped reader (the app is closing): the check says nothing, and the reply of an earlier check is not kept
    CHECK(check(console_ftp::tray::empty).reply == 451);
    drv.disc().stop();
    const ds::disc_status stopped = check(console_ftp::tray::empty);
    CHECK(stopped.read == ds::read_status::stopped && stopped.reply == 0 && dl::sight(r, stopped) == dl::sighting::unknown);
}
}

int main()
{
    test_record();
    test_fingerprint();
    test_registry();
    test_classify();
    test_gate();
    test_texts();
    test_token_and_cadence();
    test_padlock();
    test_sighting();
    test_removal_watch();
    test_drive_worker_start();
    test_drive_checks();
    test_check_statistics();
    if (failures)
    {
        std::fprintf(stderr, "disc_lock_test: %d check(s) failed\n", failures);
        return 1;
    }
    std::printf("disc_lock_test: OK\n");
    return 0;
}
