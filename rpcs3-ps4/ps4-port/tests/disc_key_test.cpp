// Host test of the automatic PS3 disc key (rpcs3/util/ps4_disc_key.hpp): disc key derivation (known answers of the
// PS3 Disc Dumper tests), IRD parsing on synthetic IRDs of every layout (versions 6-9, gzip or plain, 7-bit title
// length, trailing uid or not) and their corruptions, the IRD index JSON, links and cache names, the identity of a
// synthetic encrypted PS3 disc image (ISO 9660 tree, PARAM.SFO, LIC.DAT, EBOOT.BIN) with the ISO.cpp key check, and
// obtain() end to end over an in-memory console (key in place, redump folder, IRD cache, USB, the database,
// failures and their pt-BR texts). No log line may hold a key.
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

    dk::environment env()
    {
        dk::environment e;
        e.read_disc = reader(image);
        e.fetch = [this](const std::string& url, std::size_t limit)
        {
            fetched.push_back(url);
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
            if (read_only || !path.starts_with("/data/rpcs3/")) return false;
            files[path].assign(static_cast<const std::uint8_t*>(data), static_cast<const std::uint8_t*>(data) + size);
            return true;
        };
        e.rename_file = [this](const std::string& from, const std::string& to)
        {
            const auto it = files.find(from);
            if (read_only || it == files.end()) return false;
            files[to] = it->second;
            files.erase(from);
            return true;
        };
        e.list_files = [this](const std::string& folder)
        {
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
};

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

        // Key removed: the cached IRD answers without the network
        c.files.erase(key_path);
        c.network = false;
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
    // A wrong key next to the ISO: moved aside when it may be (the console's rename refuses anything outside
    // /data/rpcs3, i.e. USB), and the right key is written in the redump folder in any case
    {
        console c;
        c.image = build_disc({}, key);
        c.network = false;
        c.files[folder + "FIFA 14 (Europe).dkey"] = as_bytes(hex(other));
        c.files[id_key] = as_bytes(hex(key));
        const dk::result r = dk::obtain(image_env(c));
        CHECK(r.what == dk::outcome::already_present && c.text(name_key) == hex(key) && c.text(folder + "FIFA 14 (Europe).dkey.bad") == hex(other));
        c.files[folder + "FIFA 14 (Europe).dkey"] = as_bytes(hex(other));
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
    if (g_failures)
    {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("disc_key_test: all checks passed\n");
    return 0;
}
