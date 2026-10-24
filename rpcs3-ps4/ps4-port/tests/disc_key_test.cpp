// Host test of the automatic PS3 disc key (rpcs3/util/ps4_disc_key.hpp): disc key derivation (known answers of the
// PS3 Disc Dumper tests), IRD parsing on synthetic IRDs of every layout (versions 6-9, gzip or plain, 7-bit title
// length, trailing uid or not) and their corruptions, the IRD index JSON, links and cache names, the identity of a
// synthetic encrypted PS3 disc image (ISO 9660 tree, PARAM.SFO, LIC.DAT, EBOOT.BIN) with the ISO.cpp key check, and
// obtain() end to end over an in-memory console (key in place, redump folder, IRD cache, USB, the database,
// failures and their pt-BR texts). Without internet (test_offline): the copies kept per pressing, the files a wrong
// key was moved aside to, the player's inbox and USB:/keys (key files, IRDs of any name, key lists, data1 values), the
// order and the outcomes without a network or with a wrong clock, the enum order, the bounds. No log line may hold a
// key or a data1; the player's files are never written or renamed. Synthetic keys only.
//
// Build and run from the repository root (llvm-mingw; zlib and Crypto/aes as the console build links them):
//   B=<scratch folder>
//   for f in adler32 crc32 deflate inflate inftrees inffast trees zutil; do
//     clang -c -O1 -w 3rdparty/zlib/zlib/$f.c -o $B/$f.o; done
//   clang++ -std=c++23 -static -Wall -Wextra -I . -I rpcs3 ps4-port/tests/disc_key_test.cpp rpcs3/Crypto/aes.cpp
//     rpcs3/Crypto/aesni.cpp $B/adler32.o $B/crc32.o $B/deflate.o $B/inflate.o $B/inftrees.o $B/inffast.o $B/trees.o
//     $B/zutil.o -o $B/disc_key_test.exe
//   $B/disc_key_test.exe [path of a downloaded all.json, checked when given]

#include "util/ps4_disc_key.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace
{
int g_failures = 0;

#define CHECK(expr) \
    do \
    { \
        if (!(expr)) \
        { \
            std::printf("FAILED line %d: %s\n", __LINE__, #expr); \
            ++g_failures; \
        } \
    } while (0)

namespace dk = utils::ps4::disc_key;
using bytes = std::vector<std::uint8_t>;
constexpr std::size_t sector = 2048;

dk::key128 from_hex(std::string_view hex)
{
    dk::key128 out{};
    for (std::size_t i = 0; i < 16; ++i) out[i] = static_cast<std::uint8_t>(std::stoi(std::string(hex.substr(i * 2, 2)), nullptr, 16));
    return out;
}

std::string hex(const dk::key128& key)
{
    return dk::to_hex(key.data(), key.size());
}

void put32(bytes& out, std::uint32_t v)
{
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}

void put64(bytes& out, std::uint64_t v)
{
    for (int i = 0; i < 8; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}

void append(bytes& out, std::string_view text)
{
    out.insert(out.end(), text.begin(), text.end());
}

bytes gzip(const bytes& data)
{
    z_stream z{};
    deflateInit2(&z, 6, Z_DEFLATED, 16 + MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
    bytes out(deflateBound(&z, static_cast<uLong>(data.size())) + 64);
    z.next_in = const_cast<Bytef*>(data.data());
    z.avail_in = static_cast<uInt>(data.size());
    z.next_out = out.data();
    z.avail_out = static_cast<uInt>(out.size());
    const int rc = deflate(&z, Z_FINISH);
    CHECK(rc == Z_STREAM_END);
    out.resize(z.total_out);
    deflateEnd(&z);
    return out;
}

// data1 of a disc key: the inverse of the derivation (AES-128-CBC decryption with the D1 key and IV)
dk::key128 data1_for(const dk::key128& key)
{
    aes_context aes{};
    dk::key128 iv = dk::d1_iv, out{};
    aes_setkey_dec(&aes, dk::d1_key.data(), 128);
    aes_crypt_cbc(&aes, AES_DECRYPT, 16, iv.data(), key.data(), out.data());
    return out;
}

struct ird_spec
{
    int version = 9;
    std::string code = "BLES01876";
    std::string title = "FIFA 14";
    std::string system = "4.46";
    std::string game = "01.00";
    std::string app = "01.00";
    dk::key128 key{};
    bool trailing_uid = true;
    bool wrap = true;
    bytes sector0 = bytes(sector, 0x5a);
    std::uint32_t files = 3;
};

bytes build_ird(const ird_spec& s)
{
    bytes out;
    append(out, "3IRD");
    out.push_back(static_cast<std::uint8_t>(s.version));
    std::string code = s.code;
    code.resize(9, '\0');
    append(out, code);
    // 7-bit encoded length, as .NET BinaryWriter.Write(string)
    for (std::size_t n = s.title.size();; n >>= 7)
    {
        out.push_back(static_cast<std::uint8_t>((n & 0x7f) | (n >= 0x80 ? 0x80 : 0)));
        if (n < 0x80) break;
    }
    append(out, s.title);
    std::string versions = s.system;
    versions.resize(4, '\0');
    std::string game = s.game, app = s.app;
    game.resize(5, '\0');
    app.resize(5, '\0');
    append(out, versions + game + app);
    if (s.version == 7) put32(out, 0x12345678);
    bytes header = s.sector0;
    header.resize(3 * sector, 0x11);
    const bytes header_gz = gzip(header);
    put32(out, static_cast<std::uint32_t>(header_gz.size()));
    out.insert(out.end(), header_gz.begin(), header_gz.end());
    const bytes footer_gz = gzip(bytes(sector, 0));
    put32(out, static_cast<std::uint32_t>(footer_gz.size()));
    out.insert(out.end(), footer_gz.begin(), footer_gz.end());
    out.push_back(3);
    for (int i = 0; i < 3; ++i) out.insert(out.end(), 16, static_cast<std::uint8_t>(0xa0 + i));
    put32(out, s.files);
    for (std::uint32_t i = 0; i < s.files; ++i)
    {
        put64(out, 0x8a0 + i * 32);
        out.insert(out.end(), 16, static_cast<std::uint8_t>(i));
    }
    out.push_back(1); // extra config (u16 LE)
    out.push_back(0);
    out.push_back(0); // attachments
    out.push_back(0);
    const dk::key128 d1 = data1_for(s.key);
    bytes pic(115, 0x7e);
    dk::key128 d2{};
    d2.fill(0x22);
    if (s.version >= 9) out.insert(out.end(), pic.begin(), pic.end());
    out.insert(out.end(), d1.begin(), d1.end());
    out.insert(out.end(), d2.begin(), d2.end());
    if (s.version < 9) out.insert(out.end(), pic.begin(), pic.end());
    if (s.trailing_uid) put32(out, 0xcafebabe);
    put32(out, dk::crc32_of(out.data(), out.size()));
    return s.wrap ? gzip(out) : out;
}

// ---------------------------------------------------------------------------------------------------------------

void test_crypto()
{
    // FIPS-197 appendix C.1 through the build's AES
    aes_context aes{};
    const dk::key128 key = from_hex("000102030405060708090a0b0c0d0e0f");
    const dk::key128 plain = from_hex("00112233445566778899aabbccddeeff");
    dk::key128 out{};
    aes_setkey_enc(&aes, key.data(), 128);
    aes_crypt_ecb(&aes, AES_ENCRYPT, plain.data(), out.data());
    CHECK(hex(out) == "69c4e0d86a7b0430d8cdb78070b4c55a");

    // Known answers of PS3 Disc Dumper Tests/IrdTests.cs:41-42 (the key's first 4 bytes), with the data1 of the
    // public IRDs of those discs (BLUS31604 and BLUS30259 in the IRD database)
    CHECK(hex(dk::derive_key(from_hex("afd27bb93894faf1aef01091b74c670e"))).starts_with("0a37a83c"));
    CHECK(hex(dk::derive_key(from_hex("3f5a196344a295193bc269502e6fa49c"))).starts_with("4f5e86e7"));

    // One CBC block: AES-ECB(K, data1 xor IV)
    const dk::key128 data1 = from_hex("00112233445566778899aabbccddeeff");
    dk::key128 mixed{};
    for (std::size_t i = 0; i < 16; ++i) mixed[i] = data1[i] ^ dk::d1_iv[i];
    aes_setkey_enc(&aes, dk::d1_key.data(), 128);
    aes_crypt_ecb(&aes, AES_ENCRYPT, mixed.data(), out.data());
    CHECK(dk::derive_key(data1) == out);
    // data1_for is its inverse
    CHECK(dk::derive_key(data1_for(key)) == key);

    // Check value: AES-128 of zeros under the zero key is 66e94bd4ef8a2c3b884cfa59ca342b2e
    CHECK(dk::check_value(dk::key128{}) == "66e94bd4");
    CHECK(dk::check_value(key).size() == 8 && dk::check_value(key) != hex(key).substr(0, 8));

    // Key files: the two forms the ISO loader reads
    const std::string text = "000102030405060708090A0B0C0D0E0F";
    CHECK(dk::key_from_file(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()) == key);
    const std::string line = "000102030405060708090a0b0c0d0e0f\r\n";
    CHECK(dk::key_from_file(reinterpret_cast<const std::uint8_t*>(line.data()), line.size()) == key);
    CHECK(dk::key_from_file(key.data(), key.size()) == key);
    const std::string bad = "000102030405060708090a0b0c0d0e0g";
    CHECK(!dk::key_from_file(reinterpret_cast<const std::uint8_t*>(bad.data()), bad.size()));
    CHECK(!dk::key_from_file(key.data(), 15));

    CHECK(dk::clip(std::string(200, 'a')).size() == dk::text_limit);
    // "é" (2 bytes) is never split
    std::string accents;
    for (int i = 0; i < 60; ++i) accents += "\xC3\xA9";
    const std::string clipped = dk::clip(accents);
    CHECK(clipped.size() == 110 && (static_cast<unsigned char>(clipped.back()) & 0xc0) == 0x80);
    CHECK(dk::clip(accents, 109).size() == 108);
}

void test_gzip()
{
    bytes data(300000);
    for (std::size_t i = 0; i < data.size(); ++i) data[i] = static_cast<std::uint8_t>(i * 7 + i / 1000);
    const bytes gz = gzip(data);
    bytes out;
    CHECK(dk::gunzip(gz.data(), gz.size(), data.size(), out) == dk::unpack_status::ok && out == data);
    CHECK(dk::gunzip(gz.data(), gz.size(), data.size() - 1, out) == dk::unpack_status::too_large && out.empty());
    CHECK(dk::gunzip(gz.data(), gz.size(), 100, out, true) == dk::unpack_status::ok && out.size() == 100 && std::equal(out.begin(), out.end(), data.begin()));
    CHECK(dk::gunzip(gz.data(), gz.size() / 2, data.size(), out) == dk::unpack_status::corrupt);
    bytes broken = gz;
    broken[broken.size() / 2] ^= 0xff;
    broken[broken.size() / 2 + 1] ^= 0x55;
    CHECK(dk::gunzip(broken.data(), broken.size(), data.size(), out) != dk::unpack_status::ok);
    CHECK(dk::gunzip(data.data(), 1000, data.size(), out) == dk::unpack_status::corrupt);
}

void test_ird()
{
    const dk::key128 key = from_hex("8f1e2d3c4b5a69788796a5b4c3d2e1f0");
    for (const int version : {6, 7, 8, 9})
    {
        for (const bool uid : {true, false})
        {
            for (const bool wrap : {true, false})
            {
                ird_spec s;
                s.version = version;
                s.key = key;
                s.trailing_uid = uid;
                s.wrap = wrap;
                const bytes raw = build_ird(s);
                dk::ird file;
                CHECK(dk::parse_ird(raw.data(), raw.size(), file) == dk::ird_status::ok);
                CHECK(file.version == version && file.product_code == "BLES01876" && file.title == "FIFA 14");
                CHECK(file.system_version == "4.46" && file.game_version == "01.00" && file.app_version == "01.00");
                CHECK(file.region_md5.size() == 3 && file.region_md5[2][0] == 0xa2 && file.file_count == 3);
                CHECK(file.extra_config == 1 && file.attachments == 0);
                CHECK(file.pic[0] == 0x7e && file.pic[114] == 0x7e && file.data2[0] == 0x22);
                CHECK(file.has_uid == uid && (!uid || file.uid == 0xcafebabe));
                CHECK(file.has_sector0 && file.sector0[0] == 0x5a && file.sector0[2047] == 0x5a);
                CHECK(dk::derive_key(file.data1) == key);
                CHECK(dk::peek_product_code(raw.data(), raw.size()) == "BLES01876");
            }
        }
    }

    // A title of 200 bytes takes a 2-byte length
    ird_spec s;
    s.key = key;
    s.title = std::string(150, 'T') + "\xC3\xA9" + std::string(48, 'U');
    bytes raw = build_ird(s);
    dk::ird file;
    CHECK(dk::parse_ird(raw.data(), raw.size(), file) == dk::ird_status::ok && file.title == s.title && dk::derive_key(file.data1) == key);

    // Short version fields are NUL padded
    s = {};
    s.key = key;
    s.game = "";
    s.app = "01";
    raw = build_ird(s);
    CHECK(dk::parse_ird(raw.data(), raw.size(), file) == dk::ird_status::ok && file.game_version.empty() && file.app_version == "01");

    // Corruptions
    s = {};
    s.key = key;
    s.wrap = false;
    raw = build_ird(s);
    bytes bad = raw;
    bad[40] ^= 1;
    CHECK(dk::parse_ird(bad.data(), bad.size(), file) == dk::ird_status::bad_crc);
    bad = raw;
    bad[4] = 5; // version
    put32(bad, 0);
    bad.resize(bad.size() - 8);
    put32(bad, dk::crc32_of(bad.data(), bad.size()));
    CHECK(dk::parse_ird(bad.data(), bad.size(), file) == dk::ird_status::bad_version);
    // One byte too many before the CRC: the tail is neither 147 nor 151 bytes
    bad.assign(raw.begin(), raw.end() - 4);
    bad.push_back(0);
    put32(bad, dk::crc32_of(bad.data(), bad.size()));
    CHECK(dk::parse_ird(bad.data(), bad.size(), file) == dk::ird_status::bad_layout);
    // A file count larger than the file
    bad = raw;
    {
        // header size field follows the 34 bytes of magic .. app version ("FIFA 14": 7 bytes)
        const std::size_t header_at = 4 + 1 + 9 + 1 + 7 + 14;
        const std::uint32_t header_size = dk::key_detail::le32(bad.data() + header_at);
        const std::size_t footer_at = header_at + 4 + header_size;
        const std::uint32_t footer_size = dk::key_detail::le32(bad.data() + footer_at);
        const std::size_t count_at = footer_at + 4 + footer_size + 1 + 3 * 16;
        CHECK(dk::key_detail::le32(bad.data() + count_at) == 3);
        bad[count_at + 3] = 0x10;
        bad.resize(bad.size() - 4);
        put32(bad, dk::crc32_of(bad.data(), bad.size()));
        CHECK(dk::parse_ird(bad.data(), bad.size(), file) == dk::ird_status::truncated);
    }
    bad = {'3', 'I', 'R', 'D', 9};
    CHECK(dk::parse_ird(bad.data(), bad.size(), file) == dk::ird_status::truncated);
    const std::string html = "<html>404: Not Found</html>";
    CHECK(dk::parse_ird(reinterpret_cast<const std::uint8_t*>(html.data()), html.size(), file) == dk::ird_status::not_ird);
    const bytes not_ird = gzip(bytes(400, 'x'));
    CHECK(dk::parse_ird(not_ird.data(), not_ird.size(), file) == dk::ird_status::not_ird);
    s.wrap = true;
    raw = build_ird(s);
    bad.assign(raw.begin(), raw.begin() + static_cast<std::ptrdiff_t>(raw.size() / 2));
    CHECK(dk::parse_ird(bad.data(), bad.size(), file) == dk::ird_status::corrupt);
    CHECK(dk::parse_ird(nullptr, 0, file) == dk::ird_status::not_ird);
    CHECK(dk::peek_product_code(not_ird.data(), not_ird.size()).empty());
}

void test_index(const char* real_index)
{
    const std::string json = "\xEF\xBB\xBF{\"BLJS93008\":[{\"title\":\".hack//Versus\",\"fw-ver\":\"4.11\",\"game-ver\":\"01.01\",\"app-ver\":\"01.00\","
        "\"file-count\":160,\"disc-size\":49223499776,\"link\":\"Redump/BLJS-93008 - Dot Hack (Japan, Korea).ird\"}],\n"
        "  \"BLES01876\" : [ {\"title\":\"FIFA 14 \\u00e9\\ud83d\\ude00 \\\"q\\\"\",\"fw-ver\":\"4.46\",\"game-ver\":\"01.00\",\"app-ver\":\"01.00\","
        "\"file-count\":83,\"disc-size\":10619912192,\"extra\":{\"a\":[1,2.5e3,true,false,null,{\"b\":\"]}\"}]},"
        "\"link\":\"ps3ird.free.fr/BLES01876_3D9E0907_F6BDB9D4_70282D0A_015D13EA.ird\"},"
        "{\"title\":\"FIFA 14\",\"game-ver\":\"01.01\",\"disc-size\":-1,\"link\":\"Redump/BLES-01876 - FIFA 14 (Europe, Asia) (En,Fr).ird\"},"
        "{\"title\":\"no link\"}],"
        "\"BLUS30001\":[]}";
    std::vector<dk::index_entry> entries;
    std::size_t codes = 0;
    CHECK(dk::parse_index(json, "bles01876", entries, &codes) == dk::index_status::ok);
    CHECK(codes == 3 && entries.size() == 2);
    if (entries.size() == 2)
    {
        CHECK(entries[0].title == "FIFA 14 \xC3\xA9\xF0\x9F\x98\x80 \"q\"" && entries[0].system_version == "4.46");
        CHECK(entries[0].game_version == "01.00" && entries[0].app_version == "01.00" && entries[0].file_count == 83);
        CHECK(entries[0].disc_size == 10619912192ull && entries[0].link == "ps3ird.free.fr/BLES01876_3D9E0907_F6BDB9D4_70282D0A_015D13EA.ird");
        CHECK(entries[1].game_version == "01.01" && entries[1].app_version.empty() && entries[1].disc_size == 0);
    }
    CHECK(dk::parse_index(json, "BLUS30001", entries) == dk::index_status::ok && entries.empty());
    CHECK(dk::parse_index(json, "BLUS99999", entries) == dk::index_status::ok && entries.empty());
    CHECK(dk::parse_index("{}", "BLES01876", entries, &codes) == dk::index_status::ok && codes == 0);
    CHECK(dk::parse_index("", "BLES01876", entries) == dk::index_status::not_json);
    CHECK(dk::parse_index("<html>", "BLES01876", entries) == dk::index_status::not_json);
    CHECK(dk::parse_index(json.substr(0, json.size() / 2), "BLUS30001", entries) == dk::index_status::not_json);
    CHECK(dk::parse_index("{\"A\":[1,2],}", "B", entries) == dk::index_status::not_json);
    std::string deep = "{\"A\":";
    for (int i = 0; i < 64; ++i) deep += "[";
    for (int i = 0; i < 64; ++i) deep += "]";
    CHECK(dk::parse_index(deep + "}", "B", entries) == dk::index_status::not_json);

    // Ranking: game version 4, app version 2, size 1; stable within a score
    dk::disc_identity disc;
    disc.version = "01.00";
    disc.app_version = "01.00";
    disc.size = 10619912192ull;
    std::vector<dk::index_entry> list(4);
    list[0].link = "a.ird";
    list[0].game_version = "01.01";
    list[1].link = "b.ird";
    list[1].game_version = "01.00";
    list[2].link = "c.ird";
    list[2].game_version = "01.00";
    list[2].app_version = "01.00";
    list[2].disc_size = disc.size;
    list[3].link = "d.ird";
    list[3].disc_size = disc.size;
    CHECK(dk::match_score(list[2], disc) == 7 && dk::match_score(list[0], disc) == 0);
    const auto ranked = dk::rank_candidates(list, disc);
    CHECK(ranked.size() == 4 && ranked[0].link == "c.ird" && ranked[1].link == "b.ird" && ranked[2].link == "d.ird" && ranked[3].link == "a.ird");
    CHECK(dk::rank_candidates(std::vector<dk::index_entry>(25), disc).size() == dk::max_candidates);

    if (real_index)
    {
        std::string body;
        if (FILE* f = std::fopen(real_index, "rb"))
        {
            char buffer[65536];
            std::size_t got = 0;
            while ((got = std::fread(buffer, 1, sizeof(buffer), f)) > 0) body.append(buffer, got);
            std::fclose(f);
        }
        CHECK(!body.empty());
        CHECK(dk::parse_index(body, "BLES01876", entries, &codes) == dk::index_status::ok);
        std::printf("real index: %zu bytes, %zu product codes, %zu IRD(s) for BLES01876\n", body.size(), codes, entries.size());
        CHECK(codes > 4000 && entries.size() >= 1);
        for (const auto& e : entries) CHECK(dk::safe_link(e.link) && e.disc_size == 10619912192ull);
    }
}

void test_links()
{
    const std::string redump = "Redump/BLES-01876 - FIFA 14 (Europe, Asia) (En,Fr,De,Es,It,Nl,Pt,Sv,No,Da,Pl,Ru,Cs,Hu).ird";
    CHECK(dk::safe_link(redump) && dk::safe_link("ps3ird.free.fr/BLES01876_3D9E0907_F6BDB9D4_70282D0A_015D13EA.ird"));
    CHECK(dk::safe_link("Redump/BLUS-30001 - Tom Clancy's Rainbow Six + Vegas! & Co; @x.IRD"));
    CHECK(!dk::safe_link("") && !dk::safe_link("/etc/passwd.ird") && !dk::safe_link("../x.ird") && !dk::safe_link("a/../../x.ird"));
    CHECK(!dk::safe_link("x.ird?y") && !dk::safe_link("x\\y.ird") && !dk::safe_link("https://evil.example/x.ird") && !dk::safe_link("x.zip"));
    CHECK(!dk::safe_link("a//x.ird") && !dk::safe_link("a%2f..%2fx.ird") && !dk::safe_link(std::string(301, 'a') + ".ird"));
    CHECK(!dk::safe_link(std::string("a\0b.ird", 7)) && !dk::safe_link("a\nb.ird"));

    CHECK(dk::file_url(redump) == std::string(dk::file_base_url) +
        "Redump/BLES-01876%20-%20FIFA%2014%20%28Europe%2C%20Asia%29%20%28En%2CFr%2CDe%2CEs%2CIt%2CNl%2CPt%2CSv%2CNo%2CDa%2CPl%2CRu%2CCs%2CHu%29.ird");
    CHECK(dk::file_url("a/b'c+d&e;f@g!h~i_j.ird") == std::string(dk::file_base_url) + "a/b%27c%2Bd%26e%3Bf%40g%21h~i_j.ird");
    CHECK(dk::file_url("\xC3\xA9.ird").ends_with("%C3%A9.ird"));

    CHECK(dk::cache_name(redump) == "Redump_BLES-01876 - FIFA 14 (Europe, Asia) (En,Fr,De,Es,It,Nl,Pt,Sv,No,Da,Pl,Ru,Cs,Hu).ird");
    CHECK(dk::cache_name("ps3.aldostools.org/BLES01876-ABC.ird") == "ps3.aldostools.org_BLES01876-ABC.ird");
    CHECK(dk::cache_name("a/b'c:\xC3\xA9.ird") == "a_b_c___.ird");
    const std::string long_name = dk::cache_name(std::string(300, 'x') + ".ird");
    CHECK(long_name.size() == 160 && long_name.ends_with(".ird"));

    CHECK(dk::name_mentions("Redump_BLES-01876 - FIFA 14 (Europe).ird", "BLES01876"));
    CHECK(dk::name_mentions("ps3ird.free.fr_bles01876_3D9E.ird", "BLES01876"));
    CHECK(dk::name_mentions("BLES 01876.ird", "BLES01876"));
    CHECK(!dk::name_mentions("BLES01877.ird", "BLES01876") && !dk::name_mentions("x.ird", ""));
}

// ---------------------------------------------------------------------------------------------------------------
// A synthetic PS3 disc image

struct disc_spec
{
    std::string header_id = "BLES-01876";
    std::string sfo_id = "BLES01876";
    std::string version = "01.00";
    std::string app = "01.00";
    bool encrypted = true;
    bool lic = true;
    bool eboot = true;
};

constexpr std::uint32_t lic_lba = 0x8a0;   // first sector of the encrypted region, as on FIFA 14
constexpr std::uint32_t eboot_lba = 0x8c0;
constexpr std::uint32_t plain2_lba = 0x900; // second plain region [0x900, 0x91f]
constexpr std::uint32_t disc_sectors = 0x920;

void be32(bytes& image, std::size_t at, std::uint32_t v)
{
    for (int i = 0; i < 4; ++i) image[at + i] = static_cast<std::uint8_t>(v >> (24 - 8 * i));
}

void record(bytes& image, std::size_t& at, std::uint32_t lba, std::uint32_t size, bool dir, std::string_view name)
{
    const std::size_t length = (33 + name.size() + 1) & ~std::size_t{1};
    image[at] = static_cast<std::uint8_t>(length);
    for (int i = 0; i < 4; ++i)
    {
        image[at + 2 + i] = static_cast<std::uint8_t>(lba >> (8 * i));
        image[at + 10 + i] = static_cast<std::uint8_t>(size >> (8 * i));
    }
    image[at + 25] = dir ? 2 : 0;
    image[at + 32] = static_cast<std::uint8_t>(name.size());
    std::memcpy(image.data() + at + 33, name.data(), name.size());
    at += length;
}

void directory(bytes& image, std::uint32_t lba, const std::vector<std::tuple<std::uint32_t, std::uint32_t, bool, std::string>>& entries)
{
    std::size_t at = std::size_t{lba} * sector;
    record(image, at, lba, sector, true, std::string_view("\0", 1));
    record(image, at, lba, sector, true, std::string_view("\1", 1));
    for (const auto& [target, size, dir, name] : entries) record(image, at, target, size, dir, name);
}

bytes sfo(const std::vector<std::pair<std::string, std::string>>& values)
{
    bytes keys, data;
    std::vector<std::array<std::uint32_t, 3>> index; // key offset, length, data offset
    for (const auto& [k, v] : values)
    {
        index.push_back({static_cast<std::uint32_t>(keys.size()), static_cast<std::uint32_t>(v.size() + 1), static_cast<std::uint32_t>(data.size())});
        append(keys, k);
        keys.push_back(0);
        append(data, v);
        data.push_back(0);
        while (data.size() % 4) data.push_back(0);
    }
    while (keys.size() % 4) keys.push_back(0);
    bytes out;
    append(out, std::string_view("\0PSF", 4));
    put32(out, 0x101);
    const std::uint32_t key_table = 20 + static_cast<std::uint32_t>(index.size()) * 16;
    put32(out, key_table);
    put32(out, key_table + static_cast<std::uint32_t>(keys.size()));
    put32(out, static_cast<std::uint32_t>(index.size()));
    for (const auto& [key_at, length, data_at] : index)
    {
        out.push_back(static_cast<std::uint8_t>(key_at));
        out.push_back(static_cast<std::uint8_t>(key_at >> 8));
        out.push_back(0x04); // 0x0204 UTF-8
        out.push_back(0x02);
        put32(out, length);
        put32(out, length);
        put32(out, data_at);
    }
    out.insert(out.end(), keys.begin(), keys.end());
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

bytes build_disc(const disc_spec& d, const dk::key128& key)
{
    bytes image(std::size_t{disc_sectors} * sector, 0);
    // Sector 0: two plain regions [0, 0x89f] and [0x900, 0x91f]
    be32(image, 0, 2);
    be32(image, 8, 0);
    be32(image, 12, lic_lba - 1);
    be32(image, 16, plain2_lba);
    be32(image, 20, disc_sectors - 1);
    std::memcpy(image.data() + sector, "PlayStation3", 12);
    std::string id = d.header_id;
    id.resize(16, ' ');
    std::memcpy(image.data() + sector + 0x10, id.data(), 16);
    // PVD
    image[16 * sector] = 1;
    std::memcpy(image.data() + 16 * sector + 1, "CD001", 5);
    std::size_t at = 16 * sector + 156;
    record(image, at, 20, sector, true, std::string_view("\0", 1));
    // Tree: root 20, PS3_GAME 21, LICDIR 22, USRDIR 23, PARAM.SFO 24
    const bytes param = sfo({{"APP_VER", d.app}, {"CATEGORY", "DG"}, {"TITLE", "FIFA 14"}, {"TITLE_ID", d.sfo_id}, {"VERSION", d.version}});
    directory(image, 20, {{21, sector, true, "PS3_GAME"}, {30, 6, false, "PS3_DISC.SFB;1"}});
    directory(image, 21, {{22, sector, true, "LICDIR"}, {24, static_cast<std::uint32_t>(param.size()), false, "PARAM.SFO;1"}, {23, sector, true, "USRDIR"}});
    std::vector<std::tuple<std::uint32_t, std::uint32_t, bool, std::string>> lic, usr;
    if (d.lic) lic.emplace_back(lic_lba, 0x10000, false, "LIC.DAT;1");
    if (d.eboot) usr.emplace_back(eboot_lba, 0x1000, false, "EBOOT.BIN;1");
    directory(image, 22, lic);
    directory(image, 23, usr);
    std::memcpy(image.data() + 24 * sector, param.data(), param.size());
    std::memcpy(image.data() + std::size_t{lic_lba} * sector, "PS3LICDA", 8);
    std::memcpy(image.data() + std::size_t{eboot_lba} * sector, "SCE\0\0\0\0\2", 8);
    for (std::uint32_t lba = lic_lba; lba < plain2_lba; ++lba)
        for (std::size_t i = 16; i < sector; ++i) image[std::size_t{lba} * sector + i] = static_cast<std::uint8_t>(lba + i);
    if (d.encrypted)
    {
        aes_context aes{};
        aes_setkey_enc(&aes, key.data(), 128);
        for (std::uint32_t lba = lic_lba; lba < plain2_lba; ++lba)
        {
            std::array<std::uint8_t, 16> iv{};
            iv[12] = static_cast<std::uint8_t>(lba >> 24);
            iv[13] = static_cast<std::uint8_t>(lba >> 16);
            iv[14] = static_cast<std::uint8_t>(lba >> 8);
            iv[15] = static_cast<std::uint8_t>(lba);
            std::uint8_t* p = image.data() + std::size_t{lba} * sector;
            aes_crypt_cbc(&aes, AES_ENCRYPT, sector, iv.data(), p, p);
        }
    }
    return image;
}

dk::read_fn reader(const bytes& image, int* reads = nullptr)
{
    return [&image, reads](std::uint64_t offset, void* buffer, std::size_t size)
    {
        if (reads) ++*reads;
        if (offset > image.size() || size > image.size() - offset) return false;
        std::memcpy(buffer, image.data() + offset, size);
        return true;
    };
}

void test_disc()
{
    const dk::key128 key = from_hex("00112233445566778899aabbccddeeff");
    const bytes image = build_disc({}, key);
    dk::disc_identity disc;
    std::string detail;
    CHECK(dk::read_identity(reader(image), disc, detail) == dk::identity_status::ok);
    CHECK(disc.header_id == "BLES01876" && disc.product_code == "BLES01876" && disc.title == "FIFA 14");
    CHECK(disc.version == "01.00" && disc.app_version == "01.00" && disc.plain_regions == 2 && disc.size == std::uint64_t{disc_sectors} * sector);
    CHECK(disc.blocks.size() == 2 && disc.needs_key());
    if (disc.blocks.size() == 2)
    {
        CHECK(disc.blocks[0].path == "PS3_GAME/LICDIR/LIC.DAT" && disc.blocks[0].offset == std::uint64_t{lic_lba} * sector);
        CHECK(disc.blocks[1].path == "PS3_GAME/USRDIR/EBOOT.BIN" && disc.blocks[1].magic == "SCE");
    }
    CHECK(dk::decrypts_all(key, disc.blocks));
    dk::key128 wrong = key;
    wrong[15] ^= 1;
    CHECK(!dk::decrypts_all(wrong, disc.blocks));
    CHECK(!dk::decrypts_all(key, {}));
    // Every block must decrypt: a key that opens only LIC.DAT is refused
    std::vector<dk::magic_block> mixed = disc.blocks;
    mixed[1].data[0] ^= 1; // EBOOT.BIN's block no longer what that key encrypted
    CHECK(dk::decrypts_all(key, {mixed[0]}) && !dk::decrypts_all(key, mixed));

    // Decrypted image: no key needed
    disc_spec plain;
    plain.encrypted = false;
    const bytes clear = build_disc(plain, key);
    CHECK(dk::read_identity(reader(clear), disc, detail) == dk::identity_status::ok && !disc.needs_key());

    // Only EBOOT.BIN
    disc_spec only_eboot;
    only_eboot.lic = false;
    CHECK(dk::read_identity(reader(build_disc(only_eboot, key)), disc, detail) == dk::identity_status::ok && disc.blocks.size() == 1 && dk::decrypts_all(key, disc.blocks));

    // PARAM.SFO TITLE_ID differs from the header (the key file follows the header, the IRD lookup PARAM.SFO)
    disc_spec other;
    other.sfo_id = "BLES-01877";
    CHECK(dk::read_identity(reader(build_disc(other, key)), disc, detail) == dk::identity_status::ok && disc.header_id == "BLES01876" && disc.product_code == "BLES01877");

    // Failures
    bytes broken = image;
    std::memcpy(broken.data() + sector, "PlayStation4", 12);
    CHECK(dk::read_identity(reader(broken), disc, detail) == dk::identity_status::not_ps3);
    broken = image;
    broken[16 * sector + 1] = 'X';
    CHECK(dk::read_identity(reader(broken), disc, detail) == dk::identity_status::no_filesystem);
    const bytes truncated(image.begin(), image.begin() + 24 * sector);
    CHECK(dk::read_identity(reader(truncated), disc, detail) == dk::identity_status::unreadable);
    CHECK(dk::read_identity({}, disc, detail) == dk::identity_status::unreadable);
}

// ---------------------------------------------------------------------------------------------------------------
// obtain() over an in-memory console

struct console
{
    bytes image;
    std::map<std::string, bytes> files;
    std::map<std::string, dk::fetch_reply> web;
    std::vector<std::string> fetched;
    std::vector<std::string> logs;
    bool network = true;
    bool read_only = false;
    int stop_after_fetches = -1;
    std::set<std::string> folders{"/mnt/usb0/"};
    // Every write and rename asked for (also the refused ones), and the folder listings and downloads in order
    std::vector<std::string> written, renamed, order;
    // The offline environment of the console: off (0.39) or on, NetCtl's state, the clock, the TLS self-test
    bool offline_keys = true;
    int link = -1;
    std::int64_t clock = -1;
    dk::tls_state tls = dk::tls_state::unknown;
    std::vector<std::string> reopened;
    bool reopen_opens = false;

    dk::environment env()
    {
        dk::environment e;
        e.read_disc = reader(image);
        e.offline_keys = offline_keys;
        e.inbox_dir = dk::inbox_dir;
        e.build_date = "2026-10-23";
        if (link != -2) e.link_state = [this] { return link; };
        e.clock = [this] { return clock; };
        e.tls = [this] { return tls; };
        e.reopen = [this](const std::string& folder)
        {
            reopened.push_back(folder);
            if (reopen_opens) read_only = false;
            return reopen_opens;
        };
        e.fetch = [this](const std::string& url, std::size_t limit)
        {
            fetched.push_back(url);
            order.push_back("fetch " + url);
            dk::fetch_reply r;
            if (!network)
            {
                r.status = dk::fetch_reply::kind::offline;
                r.log = "DNS failed";
                return r;
            }
            const auto it = web.find(url);
            if (it == web.end())
            {
                r.status = dk::fetch_reply::kind::http;
                r.http = 404;
                r.log = "HTTP 404";
                return r;
            }
            r = it->second;
            if (r.body.size() > limit) r.status = dk::fetch_reply::kind::too_large;
            return r;
        };
        e.read_file = [this](const std::string& path, std::size_t limit, bytes& out)
        {
            const auto it = files.find(path);
            if (it == files.end() || it->second.size() > limit) return false;
            out = it->second;
            return true;
        };
        e.write_file = [this](const std::string& path, const void* data, std::size_t size)
        {
            written.push_back(path);
            if (read_only || !path.starts_with("/data/rpcs3/")) return false;
            files[path].assign(static_cast<const std::uint8_t*>(data), static_cast<const std::uint8_t*>(data) + size);
            return true;
        };
        e.rename_file = [this](const std::string& from, const std::string& to)
        {
            renamed.push_back(from + " -> " + to);
            const auto it = files.find(from);
            if (read_only || it == files.end()) return false;
            files[to] = it->second;
            files.erase(from);
            return true;
        };
        e.list_files = [this](const std::string& folder)
        {
            order.push_back("list " + folder);
            std::vector<std::string> names;
            for (const auto& [path, data] : files)
                if (path.starts_with(folder) && path.find('/', folder.size()) == std::string::npos) names.push_back(path.substr(folder.size()));
            return names;
        };
        e.list_folders = [this](const std::string& root)
        {
            std::set<std::string> names;
            for (const auto& [path, data] : files)
                if (path.starts_with(root + "/"))
                    if (const std::size_t slash = path.find('/', root.size() + 1); slash != std::string::npos) names.insert(path.substr(root.size() + 1, slash - root.size() - 1));
            return std::vector<std::string>(names.begin(), names.end());
        };
        e.stopped = [this] { return stop_after_fetches >= 0 && static_cast<int>(fetched.size()) >= stop_after_fetches; };
        e.log = [this](const std::string& line) { logs.push_back(line); };
        e.key_dir = "/data/rpcs3/data/redump/";
        e.ird_dir = "/data/rpcs3/data/ird/";
        e.usb_roots = {"/mnt/usb0", "/mnt/usb1"};
        return e;
    }

    void serve(const std::string& url, const bytes& body)
    {
        dk::fetch_reply r;
        r.status = dk::fetch_reply::kind::ok;
        r.http = 200;
        r.body.assign(body.begin(), body.end());
        r.log = "HTTP 200, " + std::to_string(body.size()) + " bytes";
        web[url] = r;
    }

    std::string text(const std::string& path) const
    {
        const auto it = files.find(path);
        return it == files.end() ? std::string("<absent>") : std::string(it->second.begin(), it->second.end());
    }

    // No log line, result line or text may hold the key
    bool clean(const dk::key128& key, const dk::result& r) const
    {
        const std::string h = hex(key);
        for (const std::string& line : logs)
            if (line.find(h) != std::string::npos || !line.starts_with("disc key: ")) return false;
        return r.line.find(h) == std::string::npos && r.text.find(h) == std::string::npos;
    }

    // Neither a key nor its data1 in any case, in any line, the result line, the text or the summary
    bool clean_all(const std::vector<dk::key128>& keys, const dk::result& r) const
    {
        std::vector<std::string> lines = logs;
        lines.push_back(r.line);
        lines.push_back(r.text);
        lines.push_back(r.summary);
        for (std::string line : lines)
        {
            for (char& ch : line) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            for (const dk::key128& k : keys)
                if (line.find(hex(k)) != std::string::npos || line.find(hex(data1_for(k))) != std::string::npos) return false;
        }
        return true;
    }

    // No write or rename of the player's files (the inbox, USB)
    bool read_only_sources() const
    {
        const auto theirs = [](const std::string& path) { return path.starts_with("/data/rpcs3/keys/") || path.starts_with("/mnt/usb"); };
        return std::none_of(written.begin(), written.end(), theirs) &&
            std::none_of(renamed.begin(), renamed.end(), [&](const std::string& r) { return theirs(r) || theirs(r.substr(r.find(" -> ") + 4)); });
    }
};

// <redump>/<TITLE_ID>-<identity>.dkey of an image (identity: FNV-1a of its sectors 0-1)
std::string pressing_path(const bytes& image, std::string_view title_id = "BLES01876")
{
    return "/data/rpcs3/data/redump/" + dk::pressing_file_name(title_id, utils::ps4::disc_stream::detail::fnv1a64(image.data(), 2 * sector));
}

std::string index_json(const std::vector<std::pair<std::string, std::string>>& entries) // link, game version
{
    std::string json = "{\"BLUS30001\":[{\"title\":\"x\",\"link\":\"Redump/BLUS-30001.ird\"}],\"BLES01876\":[";
    for (std::size_t i = 0; i < entries.size(); ++i)
        json += std::string(i ? "," : "") + "{\"title\":\"FIFA 14\",\"fw-ver\":\"4.46\",\"game-ver\":\"" + entries[i].second +
            "\",\"app-ver\":\"01.00\",\"file-count\":83,\"disc-size\":" + std::to_string(std::uint64_t{disc_sectors} * sector) + ",\"link\":\"" + entries[i].first + "\"}";
    return json + "]}";
}

bytes as_bytes(std::string_view text)
{
    return bytes(text.begin(), text.end());
}

void test_obtain()
{
    const dk::key128 key = from_hex("0f1e2d3c4b5a69788796a5b4c3d2e1f0");
    dk::key128 other = key;
    other[0] ^= 0x80;
    const std::string key_path = "/data/rpcs3/data/redump/BLES01876.dkey";
    const std::string good_link = "Redump/BLES-01876 - FIFA 14 (Europe, Asia) (En,Fr).ird";
    const std::string bad_link = "ps3ird.free.fr/BLES01876_3D9E0907_5E8897E0_70282D0A_015D13EA.ird";
    ird_spec good;
    good.key = key;
    ird_spec bad;
    bad.key = other;
    bad.game = "01.01";

    // The database: the wrong IRD comes first in the index, the ranking tries the matching version first
    {
        console c;
        c.image = build_disc({}, key);
        c.serve(dk::index_url, as_bytes(index_json({{bad_link, "01.01"}, {good_link, "01.00"}})));
        c.serve(dk::file_url(good_link), build_ird(good));
        c.serve(dk::file_url(bad_link), build_ird(bad));
        const dk::result r = dk::obtain(c.env());
        CHECK(r.what == dk::outcome::network && r.ok && r.changed && r.text == "Chave do disco obtida automaticamente");
        CHECK(c.text(key_path) == hex(key) && r.key_path == key_path && r.title_id == "BLES01876" && r.tested == 1);
        CHECK(c.fetched.size() == 2 && c.fetched[1] == dk::file_url(good_link));
        CHECK(c.files.contains("/data/rpcs3/data/ird/" + dk::cache_name(good_link)));
        CHECK(r.check_value == dk::check_value(key) && r.key == key && c.clean(key, r));
        CHECK(r.line.starts_with("disc key: result: Chave do disco obtida automaticamente | IRD database; saved " + key_path));

        // Second time: the key in place is verified, nothing is downloaded
        c.fetched.clear();
        c.logs.clear();
        const dk::result again = dk::obtain(c.env());
        CHECK(again.what == dk::outcome::already_present && again.ok && !again.changed && again.key == key && c.fetched.empty() && c.clean(key, again));

        // The verified key was also kept for this pressing: with the key removed that copy answers, nothing downloaded
        const std::string pressing = pressing_path(c.image);
        CHECK(c.text(pressing) == hex(key) && r.line.find("; kept for this pressing as " + pressing) != std::string::npos);
        c.files.erase(key_path);
        c.network = false;
        const dk::result kept = dk::obtain(c.env());
        CHECK(kept.what == dk::outcome::cache_pressing && kept.ok && kept.changed && c.text(key_path) == hex(key) && c.fetched.empty());
        CHECK(kept.text == "Chave deste disco já guardada neste PS4" && kept.source == pressing && c.clean(key, kept));

        // Both removed: the cached IRD answers without the network
        c.files.erase(key_path);
        c.files.erase(pressing);
        const dk::result cached = dk::obtain(c.env());
        CHECK(cached.what == dk::outcome::cached_ird && cached.ok && c.text(key_path) == hex(key) && c.fetched.empty());
        CHECK(cached.text == "Chave do disco obtida automaticamente (IRD já baixado)");
    }

    // Every candidate wrong
    {
        console c;
        c.image = build_disc({}, key);
        c.serve(dk::index_url, as_bytes(index_json({{bad_link, "01.01"}})));
        c.serve(dk::file_url(bad_link), build_ird(bad));
        const dk::result r = dk::obtain(c.env());
        CHECK(r.what == dk::outcome::no_matching_key && !r.ok && !r.key && !c.files.contains(key_path));
        CHECK(r.text == "Nenhuma chave da base abre este disco (1 IRD testado): ponha a .dkey em USB:/redump");
        CHECK(!c.files.contains("/data/rpcs3/data/ird/" + dk::cache_name(bad_link)));
    }

    // An IRD of another product code is never used, whatever its key
    {
        console c;
        c.image = build_disc({}, key);
        ird_spec foreign = good;
        foreign.code = "BLUS30001";
        c.serve(dk::index_url, as_bytes(index_json({{good_link, "01.00"}})));
        c.serve(dk::file_url(good_link), build_ird(foreign));
        CHECK(dk::obtain(c.env()).what == dk::outcome::no_matching_key && !c.files.contains(key_path));
    }

    // Not in the database, offline, server error, broken index, failed download, unsafe link
    {
        console c;
        c.image = build_disc({}, key);
        c.serve(dk::index_url, as_bytes("{\"BLUS30001\":[]}"));
        dk::result r = dk::obtain(c.env());
        CHECK(r.what == dk::outcome::not_in_database);
        CHECK(r.text == "Este disco não está na base de chaves (BLES01876): ponha o .ird em USB:/ird ou a .dkey em USB:/redump");

        // The index is kept as all.json: a later download that fails is answered by that copy, with its date
        const std::string index_copy = "/data/rpcs3/data/ird/all.json";
        CHECK(c.text(index_copy) == "{\"BLUS30001\":[]}");
        c.network = false;
        c.logs.clear();
        dk::environment dated = c.env();
        dated.file_time = [](const std::string&) -> std::int64_t { return 1'792'108'800; }; // 2026-10-16
        r = dk::obtain(dated);
        CHECK(r.what == dk::outcome::not_in_database);
        CHECK(std::count_if(c.logs.begin(), c.logs.end(), [](const std::string& l) { return l.starts_with("disc key: index: download failed (DNS failed): the copy from 2026-10-16 is used"); }) == 1);
        CHECK(std::count_if(c.logs.begin(), c.logs.end(), [](const std::string& l) { return l.find("IRD(s) for BLES01876 (cached copy from 2026-10-16)") != std::string::npos; }) == 1);
        CHECK(dk::date_text(0) == "1970-01-01" && dk::date_text(951'782'400) == "2000-02-29" && dk::date_text(-1) == "an unknown date");
        c.files.erase(index_copy);

        r = dk::obtain(c.env());
        CHECK(r.what == dk::outcome::no_internet && r.text == "Sem internet: não foi possível buscar a chave do disco");

        c.network = true;
        c.web[dk::index_url].status = dk::fetch_reply::kind::http;
        c.web[dk::index_url].http = 503;
        r = dk::obtain(c.env());
        CHECK(r.what == dk::outcome::server_error && r.text == "A base de chaves não respondeu (HTTP 503): tente mais tarde");

        c.serve(dk::index_url, as_bytes("<html>maintenance</html>"));
        r = dk::obtain(c.env());
        CHECK(r.what == dk::outcome::server_error && r.text == "Resposta inválida da base de chaves: tente mais tarde");

        c.web[dk::index_url].status = dk::fetch_reply::kind::failed;
        c.web[dk::index_url].cause = "falha no TLS (confira data e hora do PS4)";
        r = dk::obtain(c.env());
        CHECK(r.what == dk::outcome::fetch_failed && r.text == "Falha ao buscar a chave do disco: falha no TLS (confira data e hora do PS4)");

        c.web[dk::index_url].status = dk::fetch_reply::kind::unavailable;
        r = dk::obtain(c.env());
        CHECK(r.what == dk::outcome::unavailable);

        // Listed, but every download fails with HTTP 404
        c.serve(dk::index_url, as_bytes(index_json({{good_link, "01.00"}})));
        r = dk::obtain(c.env());
        CHECK(r.what == dk::outcome::server_error && r.text == "A base de chaves não respondeu (HTTP 404): tente mais tarde");

        // A link that leaves the repository is never requested
        c.fetched.clear();
        c.serve(dk::index_url, as_bytes(index_json({{"../../x.ird", "01.00"}})));
        r = dk::obtain(c.env());
        CHECK(r.what == dk::outcome::no_matching_key && c.fetched.size() == 1);
    }

    // A wrong key under the target name is moved aside and replaced
    {
        console c;
        c.image = build_disc({}, key);
        c.files[key_path] = as_bytes(hex(other));
        c.serve(dk::index_url, as_bytes(index_json({{good_link, "01.00"}})));
        c.serve(dk::file_url(good_link), build_ird(good));
        const dk::result r = dk::obtain(c.env());
        CHECK(r.what == dk::outcome::network && c.text(key_path) == hex(key) && c.text(key_path + ".bad") == hex(other));
    }

    // A key pack in the redump folder (other names); a binary .key
    {
        console c;
        c.image = build_disc({}, key);
        c.network = false;
        for (int i = 0; i < 40; ++i)
        {
            dk::key128 k = other;
            k[1] = static_cast<std::uint8_t>(i);
            c.files["/data/rpcs3/data/redump/Game " + std::to_string(i) + ".dkey"] = as_bytes(hex(k) + "\n");
        }
        c.files["/data/rpcs3/data/redump/notes.txt"] = as_bytes(hex(key));
        c.files["/data/rpcs3/data/redump/FIFA 14 (Europe, Asia) (En,Fr).key"] = bytes(key.begin(), key.end());
        const dk::result r = dk::obtain(c.env());
        CHECK(r.what == dk::outcome::local_key && c.text(key_path) == hex(key) && r.text == "Chave do disco encontrada em /data/rpcs3/data/redump e conferida");
        CHECK(r.source == "/data/rpcs3/data/redump/FIFA 14 (Europe, Asia) (En,Fr).key" && c.clean(key, r));
    }

    // USB:/redump (any case), then USB:/ird
    {
        console c;
        c.image = build_disc({}, key);
        c.network = false;
        c.files["/mnt/usb1/REDUMP/other.dkey"] = as_bytes(hex(other));
        c.files["/mnt/usb1/REDUMP/BLES01876.dkey"] = as_bytes(hex(key));
        dk::result r = dk::obtain(c.env());
        CHECK(r.what == dk::outcome::usb_key && r.source == "USB1:/REDUMP/BLES01876.dkey" && c.text(key_path) == hex(key));
        CHECK(r.text == "Chave do disco importada de USB:/redump e conferida" && c.fetched.empty());

        // A key pack on USB (names other than the title id) is searched after the database
        console p;
        p.image = build_disc({}, key);
        p.serve(dk::index_url, as_bytes("{\"BLUS30001\":[]}"));
        p.files["/mnt/usb0/redump/Another Game (USA).dkey"] = as_bytes(hex(other));
        p.files["/mnt/usb0/redump/FIFA 14 (Europe, Asia).dkey"] = as_bytes(hex(key));
        r = dk::obtain(p.env());
        CHECK(r.what == dk::outcome::usb_key && p.fetched.size() == 1 && r.source == "USB0:/redump/FIFA 14 (Europe, Asia).dkey" && p.clean(key, r));
        // Nothing anywhere: the database's answer is the result
        p.files.erase("/mnt/usb0/redump/FIFA 14 (Europe, Asia).dkey");
        p.files.erase(key_path);
        p.files.erase(pressing_path(p.image));
        CHECK(dk::obtain(p.env()).what == dk::outcome::not_in_database);

        console d;
        d.image = build_disc({}, key);
        d.network = false;
        d.files["/mnt/usb0/Ird/other.ird"] = build_ird(bad);
        d.files["/mnt/usb0/Ird/broken.ird"] = as_bytes("not an ird");
        d.files["/mnt/usb0/Ird/fifa.IRD"] = build_ird(good);
        r = dk::obtain(d.env());
        CHECK(r.what == dk::outcome::usb_ird && d.text(key_path) == hex(key) && r.text == "Chave do disco obtida do IRD em USB:/ird");
        CHECK(d.files.contains("/data/rpcs3/data/ird/usb_BLES01876 fifa.IRD"));
        // The copy is found by the cache on the next disc check
        d.files.erase(key_path);
        d.files.erase(pressing_path(d.image));
        d.files.erase("/mnt/usb0/Ird/fifa.IRD");
        CHECK(dk::obtain(d.env()).what == dk::outcome::cached_ird);
    }

    // Decrypted disc, unreadable disc, no known files, write failure, cancel
    {
        console c;
        disc_spec plain;
        plain.encrypted = false;
        c.image = build_disc(plain, key);
        dk::result r = dk::obtain(c.env());
        CHECK(r.what == dk::outcome::not_needed && r.ok && !r.key && c.fetched.empty() && !c.files.contains(key_path));

        c.image.resize(10 * sector);
        r = dk::obtain(c.env());
        CHECK(r.what == dk::outcome::disc_unreadable && r.text == "Não foi possível ler o disco para conferir a chave");

        disc_spec none;
        none.lic = none.eboot = false;
        c.image = build_disc(none, key);
        CHECK(dk::obtain(c.env()).what == dk::outcome::cannot_verify);

        c.image = build_disc({}, key);
        c.read_only = true;
        c.serve(dk::index_url, as_bytes(index_json({{good_link, "01.00"}})));
        c.serve(dk::file_url(good_link), build_ird(good));
        r = dk::obtain(c.env());
        CHECK(r.what == dk::outcome::write_failed && !r.ok && r.key == key && r.text == "Chave conferida, mas não foi possível gravá-la em /data/rpcs3/data/redump");
        CHECK(c.clean(key, r));

        c.read_only = false;
        c.fetched.clear();
        c.stop_after_fetches = 1;
        r = dk::obtain(c.env());
        CHECK(r.what == dk::outcome::cancelled && c.fetched.size() == 1 && !c.files.contains(key_path));
    }

    // Every text fits the XMB line
    for (int i = 0; i <= static_cast<int>(dk::outcome::cancelled); ++i)
    {
        const auto what = static_cast<dk::outcome>(i);
        CHECK(dk::outcome_text(what, "BLES01876", 10, 503, "certificado do servidor recusado (confira data e hora do PS4)").size() <= dk::text_limit);
        CHECK(!dk::outcome_text(what).empty() && std::string(dk::to_string(what)) != "?");
    }
}

// Layouts of other real discs (from their IRD headers): Demon's Souls BLUS30443 has no LIC.DAT and directories of
// 84-90 KiB; a well known file may lie in a plain region; downloads that break are retried; the service retries a
// search that failed for the network later and later.
void test_generality()
{
    const dk::key128 key = from_hex("00112233445566778899aabbccddeeff");
    dk::key128 wrong = key;
    wrong[3] ^= 0x40;
    dk::disc_identity disc;
    std::string detail;

    // No LIC.DAT and a USRDIR of 80 KiB (about 1800 entries) with EBOOT.BIN at its end: the key is verified on EBOOT.BIN
    {
        disc_spec only_eboot;
        only_eboot.lic = false;
        bytes image = build_disc(only_eboot, key);
        constexpr std::uint32_t big_lba = 0x100, big_sectors = 40;
        std::size_t at = std::size_t{big_lba} * sector;
        record(image, at, big_lba, big_sectors * sector, true, std::string_view("\0", 1));
        record(image, at, 21, sector, true, std::string_view("\1", 1));
        char name[16];
        const std::size_t last_sector = std::size_t{big_lba + big_sectors - 1} * sector;
        for (int i = 0;; ++i)
        {
            std::snprintf(name, sizeof(name), "F%05d.DAT;1", i);
            if (at % sector + 46 > sector) at = (at / sector + 1) * sector; // records never cross a sector
            if (at >= last_sector) break;
            record(image, at, 0x300, 2048, false, name);
        }
        at = last_sector;
        record(image, at, eboot_lba, 0x1000, false, "EBOOT.BIN;1");
        // PS3_GAME points USRDIR at the big directory (records rewritten in place, same lengths)
        at = std::size_t{21} * sector;
        std::fill(image.begin() + 21 * sector, image.begin() + 22 * sector, std::uint8_t{0});
        record(image, at, 21, sector, true, std::string_view("\0", 1));
        record(image, at, 20, sector, true, std::string_view("\1", 1));
        record(image, at, 22, sector, true, "LICDIR");
        record(image, at, 24, 512, false, "PARAM.SFO;1");
        record(image, at, big_lba, big_sectors * sector, true, "USRDIR");
        CHECK(dk::read_identity(reader(image), disc, detail) == dk::identity_status::ok);
        CHECK(utils::ps4::disc_stream::find_entry(image.data() + big_lba * sector, big_sectors * sector, "EBOOT.BIN").has_value());
        CHECK(disc.blocks.size() == 1 && disc.needs_key() && dk::decrypts_all(key, disc.blocks) && !dk::decrypts_all(wrong, disc.blocks));
        if (!disc.blocks.empty()) CHECK(disc.blocks[0].path == "PS3_GAME/USRDIR/EBOOT.BIN" && disc.blocks[0].encrypted_region);
    }

    // LIC.DAT in a plain region, in the clear (the loader never decrypts it there): only EBOOT.BIN decides
    {
        bytes image = build_disc({}, key);
        constexpr std::uint32_t plain_lic = 0x200;
        std::fill(image.begin() + 22 * sector, image.begin() + 23 * sector, std::uint8_t{0});
        directory(image, 22, {{plain_lic, 0x10000, false, "LIC.DAT;1"}});
        std::memcpy(image.data() + std::size_t{plain_lic} * sector, "PS3LICDA", 8);
        CHECK(dk::read_identity(reader(image), disc, detail) == dk::identity_status::ok && disc.blocks.size() == 2);
        if (disc.blocks.size() == 2) CHECK(!disc.blocks[0].encrypted_region && !disc.blocks[0].testable() && disc.blocks[1].testable());
        CHECK(disc.needs_key() && dk::decrypts_all(key, disc.blocks) && !dk::decrypts_all(wrong, disc.blocks));

        // The search takes the key from the database for it
        console c;
        c.image = image;
        const std::string link = "Redump/BLES-01876 - FIFA 14 (Europe, Asia) (En,Fr).ird";
        ird_spec good;
        good.key = key;
        c.serve(dk::index_url, as_bytes(index_json({{link, "01.00"}})));
        c.serve(dk::file_url(link), build_ird(good));
        const dk::result r = dk::obtain(c.env());
        CHECK(r.what == dk::outcome::network && r.key == key);
        // Nothing testable at all (every file plain and clear): no key needed
        std::vector<dk::magic_block> none = disc.blocks;
        none.pop_back();
        CHECK(!dk::decrypts_all(key, none));
    }

    // Downloads that break on the way are asked again (3 attempts, 2 s then 4 s apart); HTTP 404 is final at once
    {
        const std::string link = "Redump/BLES-01876 - FIFA 14 (Europe, Asia) (En,Fr).ird";
        ird_spec good;
        good.key = key;
        const auto run = [&](int index_failures, dk::fetch_reply::kind kind, long http, std::vector<std::int64_t>& pauses, console& c)
        {
            c.image = build_disc({}, key);
            c.serve(dk::index_url, as_bytes(index_json({{link, "01.00"}})));
            c.serve(dk::file_url(link), build_ird(good));
            dk::environment e = c.env();
            const auto inner = e.fetch;
            e.fetch = [inner, &index_failures, kind, http](const std::string& url, std::size_t limit)
            {
                if (url == dk::index_url && index_failures > 0)
                {
                    --index_failures;
                    dk::fetch_reply r;
                    r.status = kind;
                    r.http = http;
                    r.log = "curl 56 'Connection reset'";
                    r.cause = "conexão interrompida";
                    return r;
                }
                return inner(url, limit);
            };
            e.pause = [&pauses](std::int64_t ms) { pauses.push_back(ms); };
            return dk::obtain(e);
        };
        std::vector<std::int64_t> pauses;
        console a;
        dk::result r = run(2, dk::fetch_reply::kind::failed, 0, pauses, a);
        CHECK(r.what == dk::outcome::network && r.key == key && pauses == std::vector<std::int64_t>({2000, 4000}));
        CHECK(std::count_if(a.logs.begin(), a.logs.end(), [](const std::string& l) { return l.find("disc key: index: attempt 1 of 3 failed (curl 56") == 0; }) == 1);
        pauses.clear();
        console b;
        r = run(3, dk::fetch_reply::kind::failed, 0, pauses, b);
        CHECK(r.what == dk::outcome::fetch_failed && pauses.size() == 2 && r.text == "Falha ao buscar a chave do disco: conexão interrompida");
        pauses.clear();
        console s;
        r = run(1, dk::fetch_reply::kind::http, 503, pauses, s);
        CHECK(r.what == dk::outcome::network && pauses.size() == 1);
        pauses.clear();
        console n;
        r = run(5, dk::fetch_reply::kind::http, 404, pauses, n);
        CHECK(r.what == dk::outcome::server_error && pauses.empty());
        pauses.clear();
        console o;
        r = run(5, dk::fetch_reply::kind::offline, 0, pauses, o);
        CHECK(r.what == dk::outcome::no_internet && pauses.empty());
    }

    // Which failures the service searches again for, and when
    CHECK(dk::retry_later(dk::outcome::no_internet) && dk::retry_later(dk::outcome::server_error) && dk::retry_later(dk::outcome::fetch_failed));
    CHECK(!dk::retry_later(dk::outcome::not_in_database) && !dk::retry_later(dk::outcome::no_matching_key) && !dk::retry_later(dk::outcome::network) &&
        !dk::retry_later(dk::outcome::disc_unreadable) && !dk::retry_later(dk::outcome::cancelled));
    CHECK(dk::next_backoff(0) == 60000 && dk::next_backoff(60000) == 120000 && dk::next_backoff(480000) == 600000 && dk::next_backoff(600000) == 600000);
}
}

// An encrypted ISO file without its key (find_key_for_image builds this environment on the console)
void test_image()
{
    const dk::key128 key = from_hex("0f1e2d3c4b5a69788796a5b4c3d2e1f0");
    dk::key128 other = key;
    other[0] ^= 0x80;
    ird_spec good;
    good.key = key;
    const std::string folder = "/mnt/usb0/PS3ISO/";
    const std::string id_key = "/data/rpcs3/data/redump/BLES01876.dkey", name_key = "/data/rpcs3/data/redump/FIFA 14 (Europe).dkey";
    const auto image_env = [&](console& c)
    {
        dk::environment e = c.env();
        e.image_name = "FIFA 14 (Europe)";
        e.image_key_files = {folder + "FIFA 14 (Europe).dkey", folder + "FIFA 14 (Europe).key", name_key, "/data/rpcs3/data/redump/FIFA 14 (Europe).key"};
        e.image_ird_folders = {folder, folder + "ird/"};
        return e;
    };

    // The IRD next to the ISO (another disc's IRD in the same folder is passed over): no network needed; the key is
    // written under the header id and under the image's name, a copy of the IRD kept
    {
        console c;
        c.image = build_disc({}, key);
        c.network = false;
        ird_spec foreign = good;
        foreign.code = "BLUS30001";
        c.files[folder + "Another Game.ird"] = build_ird(foreign);
        c.files[folder + "FIFA 14 (Europe).ird"] = build_ird(good);
        const dk::result r = dk::obtain(image_env(c));
        CHECK(r.what == dk::outcome::image_ird && r.ok && r.changed && r.text == "Chave obtida do IRD ao lado da ISO e conferida");
        CHECK(c.text(id_key) == hex(key) && c.text(name_key) == hex(key) && c.fetched.empty() && c.clean(key, r));
        CHECK(r.line.find("saved " + id_key + " and " + name_key) != std::string::npos);
        CHECK(std::any_of(c.files.begin(), c.files.end(), [](const auto& f) { return f.first.starts_with("/data/rpcs3/data/ird/") && f.first.ends_with(".ird"); }));
        // Then the key in place answers
        const dk::result again = dk::obtain(image_env(c));
        CHECK(again.what == dk::outcome::already_present && !again.changed && again.key_path == name_key);
    }
    // In <folder>/ird/ under any name
    {
        console c;
        c.image = build_disc({}, key);
        c.network = false;
        c.files[folder + "ird/x.ird"] = build_ird(good);
        CHECK(dk::obtain(image_env(c)).what == dk::outcome::image_ird && c.text(name_key) == hex(key));
    }
    // A wrong key under the image's name, the right one under the header id: the wrong one (which the loader takes first)
    // is moved aside and replaced
    {
        console c;
        c.image = build_disc({}, key);
        c.network = false;
        c.files[name_key] = as_bytes(hex(other));
        c.files[id_key] = as_bytes(hex(key));
        const dk::result r = dk::obtain(image_env(c));
        CHECK(r.what == dk::outcome::already_present && r.ok && r.changed && c.text(name_key) == hex(key) && c.text(name_key + ".bad") == hex(other));
    }
    // A wrong key next to the ISO: never moved on USB (the search writes and renames only below /data/rpcs3), and the
    // right key is written in the redump folder in any case
    {
        console c;
        c.image = build_disc({}, key);
        c.network = false;
        c.files[folder + "FIFA 14 (Europe).dkey"] = as_bytes(hex(other));
        c.files[id_key] = as_bytes(hex(key));
        const dk::result r = dk::obtain(image_env(c));
        CHECK(r.what == dk::outcome::already_present && c.text(name_key) == hex(key) && c.text(folder + "FIFA 14 (Europe).dkey") == hex(other));
        CHECK(!c.files.contains(folder + "FIFA 14 (Europe).dkey.bad") && c.renamed.empty());
        CHECK(std::any_of(c.logs.begin(), c.logs.end(), [&](const std::string& l) { return l.find("could not move " + folder + "FIFA 14 (Europe).dkey aside") != std::string::npos; }));
        // An ISO kept below /data/rpcs3: the wrong key next to it is moved aside there
        console g;
        g.image = build_disc({}, key);
        g.network = false;
        const std::string games = "/data/rpcs3/games/";
        g.files[games + "FIFA 14 (Europe).dkey"] = as_bytes(hex(other));
        g.files[id_key] = as_bytes(hex(key));
        dk::environment ge = g.env();
        ge.image_name = "FIFA 14 (Europe)";
        ge.image_key_files = {games + "FIFA 14 (Europe).dkey", games + "FIFA 14 (Europe).key", name_key, "/data/rpcs3/data/redump/FIFA 14 (Europe).key"};
        ge.image_ird_folders = {games, games + "ird/"};
        CHECK(dk::obtain(ge).what == dk::outcome::already_present && g.text(games + "FIFA 14 (Europe).dkey.bad") == hex(other) && g.text(name_key) == hex(key));
        c.files.erase(name_key);
        c.read_only = true;
        CHECK(dk::obtain(image_env(c)).what == dk::outcome::write_failed);
        CHECK(std::any_of(c.logs.begin(), c.logs.end(), [&](const std::string& l) { return l.find("could not move " + folder + "FIFA 14 (Europe).dkey aside") != std::string::npos; }));
    }
    // Nothing anywhere and no internet: the drive's answer
    {
        console c;
        c.image = build_disc({}, key);
        c.network = false;
        CHECK(dk::obtain(image_env(c)).what == dk::outcome::no_internet && !c.files.contains(name_key));
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Without internet

const std::string redump_dir = "/data/rpcs3/data/redump/";
const std::string fifa_key_path = redump_dir + "BLES01876.dkey";

// Another pressing of the same title id: sector 1 differs past the header id (another identity), its own key
bytes pressing_image(const dk::key128& key, std::uint8_t tag)
{
    bytes image = build_disc({}, key);
    image[sector + 0x400] = tag;
    return image;
}

std::size_t count_lines(const console& c, std::string_view text)
{
    return static_cast<std::size_t>(std::count_if(c.logs.begin(), c.logs.end(), [&](const std::string& l) { return l.find(text) != std::string::npos; }));
}

// The copies kept per pressing (two pressings, the 0.39 loss), the files moved aside by 0.37-0.39, the backfill
void test_pressings()
{
    const dk::key128 key_a = from_hex("1f1e2d3c4b5a69788796a5b4c3d2e1f0"), key_b = from_hex("2f2e2d3c4b5a69788796a5b4c3d2e1f0");
    const std::string link_a = "Redump/BLES-01876 - FIFA 14 (Europe).ird", link_b = "Redump/BLES-01876 - FIFA 14 (Europe) (Rev 1).ird";
    ird_spec ia, ib;
    ia.key = key_a;
    ib.key = key_b;
    const bytes disc_a = pressing_image(key_a, 0xa1), disc_b = pressing_image(key_b, 0xb2);
    CHECK(pressing_path(disc_a) != pressing_path(disc_b));
    for (const bool offline : {true, false})
    {
        console c;
        c.offline_keys = offline;
        c.serve(dk::index_url, as_bytes(index_json({{link_a, "01.00"}, {link_b, "01.00"}})));
        c.serve(dk::file_url(link_a), build_ird(ia));
        c.serve(dk::file_url(link_b), build_ird(ib));
        c.image = disc_a;
        dk::result r = dk::obtain(c.env());
        CHECK(r.what == dk::outcome::network && r.key == key_a);
        c.image = disc_b;
        r = dk::obtain(c.env());
        CHECK(r.what == dk::outcome::network && r.key == key_b && c.text(fifa_key_path) == hex(key_b) && c.text(fifa_key_path + ".bad") == hex(key_a));
        // The IRDs they came from are gone (the IRD folder cleaned, or the keys came from a USB stick): only key files
        for (auto it = c.files.begin(); it != c.files.end();) it = it->first.starts_with("/data/rpcs3/data/ird/") ? c.files.erase(it) : std::next(it);
        c.image = disc_a;
        c.network = false;
        c.link = 1;
        c.fetched.clear();
        r = dk::obtain(c.env());
        if (offline)
        {
            CHECK(r.what == dk::outcome::cache_pressing && r.ok && r.key == key_a && c.fetched.empty() && c.text(fifa_key_path) == hex(key_a));
            CHECK(c.text(pressing_path(disc_a)) == hex(key_a) && c.text(pressing_path(disc_b)) == hex(key_b) && r.source == pressing_path(disc_a));
            // B's key moved aside under a name of its own: the .bad of 0.39 (A's) keeps its place
            CHECK(c.text(fifa_key_path + ".bad") == hex(key_a) && c.text(fifa_key_path + "." + dk::check_value(key_b) + ".bad") == hex(key_b));
            CHECK(r.text == "Chave deste disco já guardada neste PS4" && c.clean_all({key_a, key_b}, r));
            // B again: its own copy, still nothing downloaded
            c.image = disc_b;
            r = dk::obtain(c.env());
            CHECK(r.what == dk::outcome::cache_pressing && r.key == key_b && c.fetched.empty() && c.text(fifa_key_path) == hex(key_b));
            CHECK(c.clean_all({key_a, key_b}, r) && c.read_only_sources());
            // The copies are never moved or removed
            CHECK(std::none_of(c.renamed.begin(), c.renamed.end(), [&](const std::string& m) { return m.starts_with(pressing_path(disc_a)) || m.starts_with(pressing_path(disc_b)); }));
        }
        else
        {
            // 0.39 (disc-keys-039.flag): A's key went to .bad and is never read again: lost without the network
            CHECK(r.what == dk::outcome::no_internet && !r.key && c.text(fifa_key_path) == hex(key_b) && !c.files.contains(pressing_path(disc_a)));
            CHECK(!c.fetched.empty() && r.text == "Sem internet: não foi possível buscar a chave do disco");
        }
    }

    // A key file under another pressing's name of the title id (two pressings sharing one key): tested before the rest
    {
        console c;
        c.image = build_disc({}, key_a);
        c.network = false;
        c.files[redump_dir + dk::pressing_file_name("BLES01876", 0x0123456789abcdefull)] = as_bytes(hex(key_a));
        c.files[redump_dir + "BLES01877-0123456789abcdef.dkey"] = as_bytes(hex(key_b)); // another title id: not a copy of this one
        const dk::result r = dk::obtain(c.env());
        CHECK(r.what == dk::outcome::cache_pressing && r.key == key_a && c.text(pressing_path(c.image)) == hex(key_a) && c.text(fifa_key_path) == hex(key_a));
        CHECK(dk::is_pressing_file("BLES01876-0123456789ABCDEF.dkey", "BLES01876") && !dk::is_pressing_file("BLES01876-0123456789abcde.dkey", "BLES01876"));
        CHECK(!dk::is_pressing_file("BLES01876-0123456789abcdeg.dkey", "BLES01876") && !dk::is_pressing_file("BLES01876.dkey", "BLES01876"));
        CHECK(!dk::is_pressing_file("BLES01877-0123456789abcdef.dkey", "BLES01876") && !dk::is_pressing_file("x", ""));
        CHECK(dk::pressing_file_name("BLES01876", 0xabcull) == "BLES01876-0000000000000abc.dkey");
    }

    // 0.37-0.39 leftovers: the right key only in <ID>.dkey.bad, a wrong one in place: found without the network and put
    // back in place; the wrong one moved aside without pushing the .bad out
    {
        const dk::key128 key = key_a, other = key_b;
        console c;
        c.image = build_disc({}, key);
        c.network = false;
        c.link = 1;
        c.files[fifa_key_path] = as_bytes(hex(other));
        c.files[fifa_key_path + ".bad"] = as_bytes(hex(key));
        dk::result r = dk::obtain(c.env());
        CHECK(r.what == dk::outcome::local_key && r.ok && c.text(fifa_key_path) == hex(key) && r.source == fifa_key_path + ".bad" && c.fetched.empty());
        CHECK(c.text(fifa_key_path + ".bad") == hex(key) && c.text(fifa_key_path + "." + dk::check_value(other) + ".bad") == hex(other));
        CHECK(c.clean_all({key, other}, r));
        // Only the .bad: the same
        console d;
        d.image = build_disc({}, key);
        d.network = false;
        d.files[fifa_key_path + ".bad"] = as_bytes(hex(key));
        CHECK(dk::obtain(d.env()).what == dk::outcome::local_key && d.text(fifa_key_path) == hex(key));
        // 0.39: a .bad file is never read
        console e;
        e.offline_keys = false;
        e.image = build_disc({}, key);
        e.network = false;
        e.files[fifa_key_path + ".bad"] = as_bytes(hex(key));
        CHECK(dk::obtain(e.env()).what == dk::outcome::no_internet && !e.files.contains(fifa_key_path));
    }

    // Backfill: the key in place verifies and has no copy for this pressing yet: written once, then never again
    {
        console c;
        c.image = build_disc({}, key_a);
        c.files[fifa_key_path] = as_bytes(hex(key_a));
        dk::result r = dk::obtain(c.env());
        CHECK(r.what == dk::outcome::already_present && !r.changed && c.text(pressing_path(c.image)) == hex(key_a) && c.fetched.empty());
        CHECK(r.line.find("; kept for this pressing as " + pressing_path(c.image)) != std::string::npos);
        const std::size_t writes = c.written.size();
        CHECK(writes == 1);
        r = dk::obtain(c.env());
        CHECK(r.what == dk::outcome::already_present && c.written.size() == writes && r.line.find("kept for this pressing") == std::string::npos);
        // A copy holding another value is written again
        c.files[pressing_path(c.image)] = as_bytes(hex(key_b));
        r = dk::obtain(c.env());
        CHECK(r.what == dk::outcome::already_present && c.text(pressing_path(c.image)) == hex(key_a) && c.written.size() == writes + 1);
        // 0.39: no copy
        console z;
        z.offline_keys = false;
        z.image = build_disc({}, key_a);
        z.files[fifa_key_path] = as_bytes(hex(key_a));
        CHECK(dk::obtain(z.env()).what == dk::outcome::already_present && z.written.empty());
    }

    // data1 under the loader's name: moved aside, the key derived from it written in its place
    {
        console c;
        c.image = build_disc({}, key_a);
        c.network = false;
        c.files[fifa_key_path] = as_bytes(hex(data1_for(key_a)));
        const dk::result r = dk::obtain(c.env());
        CHECK(r.what == dk::outcome::local_key && c.text(fifa_key_path) == hex(key_a) && c.text(fifa_key_path + ".bad") == hex(data1_for(key_a)));
        CHECK(r.source == fifa_key_path + " (data1)" && c.clean_all({key_a}, r));
    }

    // A write refused by the folder: opened once (the frontend's permission repair) and written again; the key stays
    // usable from memory when that fails too
    {
        console c;
        c.image = build_disc({}, key_a);
        c.network = false;
        c.files["/mnt/usb0/redump/BLES01876.dkey"] = as_bytes(hex(key_a));
        c.read_only = true;
        c.reopen_opens = true;
        dk::result r = dk::obtain(c.env());
        CHECK(r.what == dk::outcome::usb_key && r.ok && c.text(fifa_key_path) == hex(key_a) && c.reopened == std::vector<std::string>{"/data/rpcs3/data/redump"});
        CHECK(count_lines(c, "could not write " + fifa_key_path + ": /data/rpcs3/data/redump opened again, writing once more") == 1);
        console d;
        d.image = build_disc({}, key_a);
        d.network = false;
        d.files["/mnt/usb0/redump/BLES01876.dkey"] = as_bytes(hex(key_a));
        d.read_only = true;
        r = dk::obtain(d.env());
        CHECK(r.what == dk::outcome::write_failed && r.key == key_a && d.reopened.size() == 1 && count_lines(d, "could not be opened") == 1);
        CHECK(d.read_only_sources() && d.clean_all({key_a}, r));
    }
}

// Key lists: every layout, the order, the bounds
void test_key_lists()
{
    const std::string k1 = "00112233445566778899aabbccddeeff", k2 = "FFEEDDCCBBAA99887766554433221100", k3 = "0123456789abcdef0123456789ABCDEF";
    const std::string text = "\xEF\xBB\xBF# comment " + k1 + "\r\n" + "; also a comment " + k1 + "\r\n" + "  // and this " + k1 + "\r\n" + "BLUS30443;Demon's Souls;" + k2 +
        "\r\n" + "\"FIFA 14 (Europe)\",\"" + k3 + "\"\r\n" + "bles-01876 " + k1 + " " + k2 + "\r\n" + "sha256 " + std::string(64, 'a') + "\r\n" + "x " +
        std::string(33, 'b') + "\r\n" + k3 + "\n" + "FIFA 14.dkey = " + k2 + "\n" + "data1: " + k1 + "\n";
    std::vector<std::pair<std::string, std::size_t>> seen;
    const auto collect = [&](const dk::list_value& v)
    {
        seen.emplace_back(hex(v.value), v.line);
        return false;
    };
    dk::list_stats st = dk::scan_key_list(text, "BLES01876", "BLES01876", 100, collect);
    const std::string l1 = k1, l2 = "ffeeddccbbaa99887766554433221100", l3 = "0123456789abcdef0123456789abcdef";
    // The line naming the disc first (both of its values), then the others in file order; comments, the 64-hex hash and
    // the 33-hex run give nothing
    const std::vector<std::pair<std::string, std::size_t>> expected{{l1, 6}, {l2, 6}, {l2, 4}, {l3, 5}, {l3, 9}, {l2, 10}, {l1, 11}};
    CHECK(seen == expected);
    CHECK(st.lines == 11 && st.values == 7 && st.named == 2 && st.offered == 7 && !st.truncated && !st.accepted);
    // The header id names a line too (a PARAM.SFO id that differs from the header)
    seen.clear();
    dk::scan_key_list("BLUS30443 " + k1 + "\nBLES-01877 " + k2 + "\n", "BLES01877", "BLUS30443", 100, collect);
    CHECK(seen.size() == 2 && seen[0].second == 1 && seen[1].second == 2);
    seen.clear();
    dk::scan_key_list("BLUS30443 " + k1 + "\nBLES-01877 " + k2 + "\n", "BLES01877", "BLES01877", 100, collect);
    CHECK(seen.size() == 2 && seen[0].second == 2);
    // An id inside a longer word or number does not name the line
    CHECK(dk::list_detail::names("BLES01876", "BLES01876") && dk::list_detail::names("x bles_01876;", "BLES01876") && dk::list_detail::names("BLES 01876", "BLES01876"));
    CHECK(!dk::list_detail::names("XBLES01876", "BLES01876") && !dk::list_detail::names("BLES018765", "BLES01876") && !dk::list_detail::names("BLES0187", "BLES01876"));
    // The test takes a value: the scan stops there
    std::size_t calls = 0;
    st = dk::scan_key_list(text, "BLES01876", "BLES01876", 100, [&](const dk::list_value&) { return ++calls == 3; });
    CHECK(st.accepted && calls == 3 && st.offered == 3);
    // Empty, comment-only, without values
    CHECK(dk::scan_key_list("", "BLES01876", "BLES01876", 10, collect).values == 0);
    CHECK(dk::scan_key_list("# " + k1 + "\n\n\r\n", "BLES01876", "BLES01876", 10, collect).values == 0);
    // 65537 values: the bound of one search offers 65536 and counts the rest, quickly
    std::string big;
    for (int i = 0; i < 65537; ++i)
    {
        char line[48];
        std::snprintf(line, sizeof(line), "%032x\n", i);
        big += line;
    }
    calls = 0;
    const auto started = std::chrono::steady_clock::now();
    st = dk::scan_key_list(big, "BLES01876", "BLES01876", dk::max_list_values, [&](const dk::list_value&) { return ++calls, false; });
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
    CHECK(st.values == 65537 && st.offered == dk::max_list_values && calls == dk::max_list_values && st.truncated && st.lines == 65537);
    CHECK(ms < 2000);
    std::printf("key list: 65537 values scanned in %lld ms\n", static_cast<long long>(ms));

    // Names of the player's files
    CHECK(dk::user_file_type("a.dkey") == dk::user_file::key && dk::user_file_type("A.KEY") == dk::user_file::key && dk::user_file_type("x.IRD") == dk::user_file::ird);
    CHECK(dk::user_file_type("keys.txt") == dk::user_file::list && dk::user_file_type("k.CSV") == dk::user_file::list && dk::user_file_type("dump") == dk::user_file::list);
    CHECK(dk::user_file_type("k.lst") == dk::user_file::list && dk::user_file_type("k.dat") == dk::user_file::list && dk::user_file_type("k.keys") == dk::user_file::list);
    CHECK(dk::user_file_type(".hidden.txt") == dk::user_file::other && dk::user_file_type("a.iso") == dk::user_file::other && dk::user_file_type("") == dk::user_file::other);
}

// The player's files: /data/rpcs3/keys, USB:/keys and the IRD cache by content; data1; the order and the outcomes
// offline; the clock; the enum; the texts
void test_offline()
{
    const dk::key128 key = from_hex("3c1e2d3c4b5a69788796a5b4c3d2e1f0");
    dk::key128 other = key;
    other[5] ^= 0x10;
    const std::string inbox = dk::inbox_dir;
    ird_spec good;
    good.key = key;

    // A key list in the inbox, the disc's data1 on a line naming it: found without the network
    {
        console c;
        c.image = build_disc({}, key);
        c.network = false;
        c.link = 1;
        c.files[inbox + "meus-discos.txt"] = as_bytes("# minhas chaves\r\nBLUS30443 " + hex(other) + "\r\nBLES-01876;FIFA 14;" + hex(data1_for(key)) + "\r\n");
        const dk::result r = dk::obtain(c.env());
        CHECK(r.what == dk::outcome::inbox_key && r.ok && c.text(fifa_key_path) == hex(key) && c.fetched.empty());
        CHECK(r.source == inbox + "meus-discos.txt (line 3, data1)");
        CHECK(r.text == "Chave do disco encontrada em /data/rpcs3/keys/meus-discos.txt e guardada neste PS4");
        CHECK(r.summary == "disc key: sources tested: cache 0, redump 0, inbox 1, usb 0, lists 1 (1 values), ird 0, network not needed");
        CHECK(c.clean_all({key, other}, r) && c.read_only_sources() && c.files.contains(inbox + "meus-discos.txt"));
        // The disc's data2 (an IRD's other value) opens nothing
        console d;
        d.image = build_disc({}, key);
        d.network = false;
        d.link = 1;
        dk::key128 data2{};
        data2.fill(0x22);
        d.files[inbox + "lista.txt"] = as_bytes("BLES01876 " + hex(data2) + "\n" + std::string(80, '#') + "\n");
        const dk::result none = dk::obtain(d.env());
        CHECK(none.what == dk::outcome::no_internet && !none.key && none.text == "Sem internet; 1 arquivo(s) de chave testado(s), nenhum abre este disco");
        CHECK(count_lines(d, "lista.txt: 1 value(s) on 2 line(s) (1 on lines naming BLES01876), none decrypts this disc") == 1);
        // 0.39: the inbox is not read
        console z;
        z.offline_keys = false;
        z.image = c.image;
        z.network = false;
        z.files = {{inbox + "meus-discos.txt", c.files[inbox + "meus-discos.txt"]}};
        CHECK(dk::obtain(z.env()).what == dk::outcome::no_internet && !z.files.contains(fifa_key_path));
    }

    // Any name, one level of subfolders, decided by content: a key file named otherwise, a raw 16-byte key without an
    // extension, an IRD of this disc under another name; another disc's IRD passed over
    {
        console c;
        c.image = build_disc({}, key);
        c.network = false;
        c.link = 1;
        c.files[inbox + "a/primeiro.dkey"] = as_bytes(hex(other));
        c.files[inbox + "b/chave"] = bytes(key.begin(), key.end());
        dk::result r = dk::obtain(c.env());
        CHECK(r.what == dk::outcome::inbox_key && r.source == inbox + "b/chave" && c.text(fifa_key_path) == hex(key));
        console d;
        d.image = build_disc({}, key);
        d.network = false;
        d.link = 1;
        ird_spec foreign = good;
        foreign.code = "BLUS30443";
        d.files[inbox + "ird/outro.ird"] = build_ird(foreign);
        d.files[inbox + "ird/meu disco.ird"] = build_ird(good);
        r = dk::obtain(d.env());
        CHECK(r.what == dk::outcome::inbox_key && r.source == inbox + "ird/meu disco.ird" && d.text(fifa_key_path) == hex(key));
        // The inbox's IRD is read in place: no copy is made from it
        CHECK(std::none_of(d.files.begin(), d.files.end(), [](const auto& f) { return f.first.starts_with("/data/rpcs3/data/ird/"); }) && d.read_only_sources());
        CHECK(d.clean_all({key, other}, r));
    }

    // The IRD cache by content: an IRD the player put there over FTP under any name
    {
        console c;
        c.image = build_disc({}, key);
        c.network = false;
        c.files["/data/rpcs3/data/ird/meu.ird"] = build_ird(good);
        dk::result r = dk::obtain(c.env());
        CHECK(r.what == dk::outcome::cached_ird && c.text(fifa_key_path) == hex(key));
        c.files.erase(fifa_key_path);
        c.files.erase(pressing_path(c.image));
        c.offline_keys = false; // 0.39: only names that mention the product code
        CHECK(dk::obtain(c.env()).what == dk::outcome::no_internet);
    }

    // USB:/keys (any case, a subfolder), and USB:/redump read by content (a list there)
    {
        console c;
        c.image = build_disc({}, key);
        c.network = false;
        c.link = 1;
        c.files["/mnt/usb1/KEYS/redump pack/FIFA 14 (Europe).txt"] = as_bytes("x " + hex(key) + "\n" + std::string(100, ' ') + "\n");
        dk::result r = dk::obtain(c.env());
        CHECK(r.what == dk::outcome::usb_list && r.source == "USB1:/KEYS/redump pack/FIFA 14 (Europe).txt (line 1)" && c.text(fifa_key_path) == hex(key));
        CHECK(r.text == "Chave do disco encontrada em USB1:/KEYS/redump pack/FIFA 14 (Europe).txt e guardada neste PS4");
        CHECK(c.read_only_sources() && c.clean_all({key}, r));
        console d;
        d.image = build_disc({}, key);
        d.network = false;
        d.link = 1;
        d.files["/mnt/usb0/redump/lista.txt"] = as_bytes("BLES01876 " + hex(key) + "\n" + std::string(100, '#') + "\n");
        r = dk::obtain(d.env());
        CHECK(r.what == dk::outcome::usb_list && d.text(fifa_key_path) == hex(key));
        // A key file of USB:/redump keeps its 0.39 outcome and text
        d.files.erase(fifa_key_path);
        d.files.erase(pressing_path(d.image));
        d.files.erase("/mnt/usb0/redump/lista.txt");
        d.files["/mnt/usb0/redump/x.dkey"] = as_bytes(hex(key));
        r = dk::obtain(d.env());
        CHECK(r.what == dk::outcome::usb_key && r.text == "Chave do disco importada de USB:/redump e conferida");
    }

    // The order without a network: the rest of USB before anything else, nothing downloaded; with one, the database
    // before the rest of USB (0.39 order)
    {
        for (const int link : {1, 3})
        {
            console c;
            c.image = build_disc({}, key);
            c.link = link;
            c.serve(dk::index_url, as_bytes("{\"BLUS30001\":[]}"));
            c.files["/mnt/usb0/keys/pack.txt"] = as_bytes(hex(other) + "\n" + hex(key) + "\n" + std::string(80, ' ') + "\n");
            const dk::result r = dk::obtain(c.env());
            CHECK(r.what == dk::outcome::usb_list && r.key == key);
            const auto at = [&](std::string_view what, bool last)
            {
                std::size_t found = c.order.size();
                for (std::size_t i = 0; i < c.order.size(); ++i)
                    if (c.order[i] == what && (last || found == c.order.size())) found = i;
                return found;
            };
            const std::size_t index_fetch = at("fetch " + std::string(dk::index_url), false), keys_last = at("list /mnt/usb0/keys/", true),
                              keys_first = at("list /mnt/usb0/keys/", false);
            if (link == 1)
            {
                CHECK(c.fetched.empty() && index_fetch == c.order.size() && keys_first < keys_last);
                CHECK(r.summary.ends_with("network skipped (offline: netctl state 1)"));
                CHECK(count_lines(c, "disc key: sources tested: cache 0, redump 0, inbox 0, usb 1, lists 1 (2 values), ird 0, network skipped (offline: netctl state 1)") == 1);
            }
            else CHECK(c.fetched.size() == 1 && keys_first < index_fetch && index_fetch < keys_last && r.summary.ends_with("network asked (1 request(s))"));
            CHECK(c.read_only_sources());
        }
        // Nothing anywhere, offline: no download, no retries or pauses, the text counts the files tried
        console c;
        c.image = build_disc({}, key);
        c.link = 0;
        c.files[fifa_key_path] = as_bytes(hex(other));
        c.files["/mnt/usb0/keys/x.dkey"] = as_bytes(hex(other));
        dk::environment e = c.env();
        int pauses = 0;
        e.pause = [&pauses](std::int64_t) { ++pauses; };
        const dk::result r = dk::obtain(e);
        CHECK(r.what == dk::outcome::no_internet && c.fetched.empty() && pauses == 0 && dk::retry_later(r.what));
        CHECK(r.text == "Sem internet; 2 arquivo(s) de chave testado(s), nenhum abre este disco" && r.local_tested == 2);
        CHECK(r.line.find("nothing downloaded (offline: netctl state 0)") != std::string::npos);
        CHECK(c.text(fifa_key_path) == hex(other)); // a wrong key in place stays until a right one is found
    }

    // The clock: before the build -> clock_wrong without a download, whatever the link; a certificate check that failed
    // while the TLS self-test found no root valid at the console's date -> clock_wrong too
    {
        console c;
        c.image = build_disc({}, key);
        c.link = 3;
        c.clock = 1'420'070'400; // 2015-01-01
        c.serve(dk::index_url, as_bytes(index_json({{"Redump/x.ird", "01.00"}})));
        dk::result r = dk::obtain(c.env());
        CHECK(r.what == dk::outcome::clock_wrong && !r.ok && c.fetched.empty() && dk::retry_later(r.what));
        CHECK(r.text == "Data e hora do PS4 erradas: a busca da chave pela internet falha. Acerte em Configurações > Data e Hora.");
        CHECK(r.line.find("console clock 2015-01-01 is before this build (2026-10-23)") != std::string::npos);
        // The day before the build is still plausible (a console a few hours behind on release day)
        c.clock = utils::ps4::disc_stream::detail::unix_of_date("2026-10-22") + 3600;
        c.fetched.clear();
        r = dk::obtain(c.env());
        CHECK(r.what != dk::outcome::clock_wrong && !c.fetched.empty());

        console t;
        t.image = build_disc({}, key);
        t.link = 3;
        dk::environment e = t.env();
        e.fetch = [&t](const std::string& url, std::size_t)
        {
            t.fetched.push_back(url);
            t.tls = dk::tls_state::clock_refused; // the self-test ended meanwhile
            dk::fetch_reply reply;
            reply.status = dk::fetch_reply::kind::failed;
            reply.curl = 77;
            reply.log = "curl 77 'error importing CA certificate blob'";
            reply.cause = "erro do curl 77";
            return reply;
        };
        e.pause = [](std::int64_t) {};
        r = dk::obtain(e);
        CHECK(r.what == dk::outcome::clock_wrong && r.line.find("the TLS self-test found no root certificate valid at the console's date") != std::string::npos);
        // Without the self-test's verdict it stays a download failure
        console u;
        u.image = build_disc({}, key);
        dk::environment f = u.env();
        f.fetch = [](const std::string&, std::size_t)
        {
            dk::fetch_reply reply;
            reply.status = dk::fetch_reply::kind::failed;
            reply.curl = 77;
            reply.cause = "erro do curl 77";
            return reply;
        };
        f.pause = [](std::int64_t) {};
        CHECK(dk::obtain(f).what == dk::outcome::fetch_failed);
    }

    // network_usable and the TLS self-test's verdict
    {
        const char* build = "2026-10-23";
        namespace ds = utils::ps4::disc_stream;
        const std::int64_t day = ds::detail::unix_of_date(build);
        CHECK(day == 1'792'713'600 && ds::detail::unix_of_date("2026-1-23") == -1 && ds::detail::unix_of_date("2026-13-01") == -1);
        CHECK(ds::clock_plausible(-1, build) && ds::clock_plausible(day - 86400, build) && !ds::clock_plausible(day - 86401, build) && ds::clock_plausible(0, "x"));
        CHECK(ds::network_line(1, day + 14 * 3600 + 5 * 60, build) == "network: netctl state 1, IP no, clock 2026-10-23 14:05 UTC (plausible)");
        CHECK(ds::network_line(3, 1'420'070'400, build) == "network: netctl state 3, IP yes, clock 2015-01-01 00:00 UTC (before the build)");
        CHECK(ds::network_line(-1, -1, build) == "network: netctl state -1 (unknown), IP unknown, clock an unknown date (unknown)");
        CHECK(dk::network_usable(3, day, build, dk::tls_state::ok) == dk::net_state::usable);
        CHECK(dk::network_usable(-1, -1, build, dk::tls_state::unknown) == dk::net_state::usable); // unknown: the download decides
        CHECK(dk::network_usable(1, day, build, dk::tls_state::ok) == dk::net_state::no_link);
        CHECK(dk::network_usable(0, day, build, dk::tls_state::unknown) == dk::net_state::no_link);
        CHECK(dk::network_usable(3, day - 86400, build, dk::tls_state::ok) == dk::net_state::usable);
        CHECK(dk::network_usable(3, day - 86401, build, dk::tls_state::ok) == dk::net_state::clock_wrong);
        CHECK(dk::network_usable(1, day - 86401, build, dk::tls_state::ok) == dk::net_state::clock_wrong); // the clock is named first
        CHECK(dk::network_usable(3, day, build, dk::tls_state::clock_refused) == dk::net_state::clock_wrong);
        CHECK(dk::network_usable(3, 0, "not a date", dk::tls_state::ok) == dk::net_state::usable);
        CHECK(dk::tls_from_selftest(std::nullopt, "") == dk::tls_state::unknown && dk::tls_from_selftest(true, "x") == dk::tls_state::ok);
        CHECK(dk::tls_from_selftest(false, "TLS self-test: libcurl 8.22.0-DEV wolfSSL/5.9.4; multi ok; RNG ok; CA bundle 148 roots in 31 ms (0 accepted, 148 not yet valid); "
                                           "loopback https://example.com:49152 -> 127.0.0.1: curl 77 -> TLS FAILED") == dk::tls_state::clock_refused);
        CHECK(dk::tls_from_selftest(false, "CA bundle 148 roots in 31 ms (0 accepted, 148 expired)") == dk::tls_state::clock_refused);
        CHECK(dk::tls_from_selftest(false, "CA bundle 148 roots in 31 ms (140 accepted, 8 expired); loopback curl 35") == dk::tls_state::failed);
        CHECK(dk::tls_from_selftest(false, "RNG failed") == dk::tls_state::failed);
    }

    // The bounds: a list larger than 8 MiB is not read; at most 64 key lists per search
    {
        console c;
        c.image = build_disc({}, key);
        c.network = false;
        c.link = 1;
        std::string huge(9u << 20, ' ');
        huge += "\nBLES01876 " + hex(key) + "\n";
        c.files[inbox + "enorme.txt"] = as_bytes(huge);
        dk::result r = dk::obtain(c.env());
        CHECK(r.what == dk::outcome::no_internet && count_lines(c, "enorme.txt: not read (unreadable, or larger than 8 MiB)") == 1);
        console d;
        d.image = build_disc({}, key);
        d.network = false;
        d.link = 1;
        for (int i = 0; i < 65; ++i)
        {
            char name[32];
            std::snprintf(name, sizeof(name), "lista%02d.txt", i);
            d.files[inbox + name] = as_bytes(hex(other) + "\n" + std::string(80, '#') + "\n");
        }
        d.files[inbox + "lista64.txt"] = as_bytes(hex(key) + "\n" + std::string(80, '#') + "\n"); // the 65th list
        r = dk::obtain(d.env());
        CHECK(r.what == dk::outcome::no_internet && count_lines(d, "lista64.txt: not read (a search reads at most 64 key lists and 65536 values)") == 1);
        CHECK(r.summary.find("lists 64 (64 values)") != std::string::npos);
        // Small files (a key with a comment around it) do not count as lists
        console s;
        s.image = build_disc({}, key);
        s.network = false;
        s.link = 1;
        for (int i = 0; i < 70; ++i)
        {
            char name[32];
            std::snprintf(name, sizeof(name), "k%02d.txt", i);
            s.files[inbox + name] = as_bytes("# k\n" + hex(i == 69 ? key : other) + "\n");
        }
        r = dk::obtain(s.env());
        CHECK(r.what == dk::outcome::inbox_key && s.text(fifa_key_path) == hex(key) && r.source == inbox + "k69.txt (line 2)" && r.summary.find("lists 70 (70 values)") != std::string::npos);
    }

    // Writes: only below /data/rpcs3, never into the inbox, never a path with a parent step
    CHECK(dk::may_write("/data/rpcs3/data/redump/BLES01876.dkey") && dk::may_write("/data/rpcs3/data/ird/x.ird"));
    CHECK(!dk::may_write("/data/rpcs3/keys/BLES01876.dkey") && !dk::may_write("/mnt/usb0/redump/BLES01876.dkey") && !dk::may_write("/data/rpcs3x/a"));
    CHECK(!dk::may_write("/data/rpcs3/data/../keys/a") && !dk::may_write("/data/rpcs3/./keys/a") && !dk::may_write("relative/a") && !dk::may_write(""));

    // The outcomes: successes before 'network', failures after it; every one has its texts; the retried ones
    {
        const std::set<dk::outcome> successes{dk::outcome::not_needed, dk::outcome::already_present, dk::outcome::cache_pressing, dk::outcome::local_key,
            dk::outcome::cached_ird, dk::outcome::inbox_key, dk::outcome::usb_key, dk::outcome::usb_ird, dk::outcome::usb_list, dk::outcome::image_ird,
            dk::outcome::network};
        const std::set<dk::outcome> retried{dk::outcome::no_internet, dk::outcome::server_error, dk::outcome::fetch_failed, dk::outcome::clock_wrong};
        std::set<std::string> names;
        for (int i = 0; i <= static_cast<int>(dk::last_outcome); ++i)
        {
            const auto what = static_cast<dk::outcome>(i);
            CHECK(dk::succeeded(what) == successes.contains(what));
            CHECK(dk::retry_later(what) == retried.contains(what));
            names.insert(dk::to_string(what));
            const std::string long_path = "USB0:/keys/" + std::string(200, 'p') + "/meus-discos.txt";
            CHECK(!dk::outcome_text(what, "BLES01876", 10, 503, "x", long_path).empty());
            for (const int language : {0, 1, 2, 3})
            {
                utils::ps4::i18n::detail::g_lang.store(static_cast<std::uint8_t>(language));
                const std::string shown = dk::outcome_text(what, "BLES01876", 12345, 503, "certificado do servidor recusado (confira data e hora do PS4)", long_path);
                CHECK(shown.size() <= dk::line_limit());
                if (what == dk::outcome::inbox_key || what == dk::outcome::usb_list) CHECK(shown.find("meus-discos.txt") != std::string::npos && shown.find("...") != std::string::npos);
            }
            utils::ps4::i18n::detail::g_lang.store(0);
        }
        CHECK(names.size() == static_cast<std::size_t>(dk::last_outcome) + 1 && !names.contains("?"));
        CHECK(dk::outcome_text(dk::outcome::inbox_key, {}, 0, 0, {}, "/data/rpcs3/keys/a.txt") == "Chave do disco encontrada em /data/rpcs3/keys/a.txt e guardada neste PS4");
        CHECK(dk::outcome_text(dk::outcome::no_internet) == "Sem internet: não foi possível buscar a chave do disco");
    }
}

int main(int argc, char** argv)
{
    test_crypto();
    test_gzip();
    test_ird();
    test_index(argc > 1 ? argv[1] : nullptr);
    test_links();
    test_disc();
    test_obtain();
    test_generality();
    test_image();
    test_pressings();
    test_key_lists();
    test_offline();
    if (g_failures)
    {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("disc_key_test: all checks passed\n");
    return 0;
}
