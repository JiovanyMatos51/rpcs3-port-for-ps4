#pragma once

// Decryption key of the PS3 disc in the PS4 drive, found without the player handling key files: the same source the PC
// disc dumpers use (PS3 Disc Dumper, MPF/LibIRD), the public IRD database, with the key taken from the disc's IRD.
//
// IRD file (little-endian; usually gzip-wrapped):
//   "3IRD" | u8 version 6..9 | 9 product code | 7-bit length + UTF-8 title | 4 system version | 5 game version
//   (PARAM.SFO VERSION) | 5 app version (APP_VER) | [v7: u32 id] | u32 + gzip header (sectors 0 .. first file) |
//   u32 + gzip footer | u8 regions + 16-byte MD5 each | u32 files + (u64 sector, 16-byte MD5) each |
//   u16 extra config, u16 attachments | [v9: 115 PIC] | 16 data1 | 16 data2 | [v < 9: 115 PIC] | [u32 uid] |
//   u32 CRC32 of everything before it.
//   Writers differ on the trailing uid of v6/v7 files, so its presence follows from the bytes left (151 or 147).
// Disc key = AES-128-CBC encryption of data1 (key 380bcf0b..., IV 69474772..., one block), as both tools derive it.
// Index: https://flexby420.github.io/playstation_3_ird_database/all.json, {"BLES01876": [{"title", "fw-ver",
// "game-ver", "app-ver", "file-count", "disc-size", "link"}, ...], ...}; files under the repository root named by
// "link". Every IRD listed for the disc's product code is a candidate, best match first (versions, size).
//
// No key is trusted before it decrypts the disc: the first block of PS3_GAME/LICDIR/LIC.DAT ("PS3LICDA") and of
// PS3_GAME/USRDIR/EBOOT.BIN ("SCE") must both come out right, the check the ISO loader repeats before using a key
// (ISO.cpp read_magic_blocks / decrypts_all_blocks). Only then is <TITLE_ID>.dkey written to the redump key folder,
// where the loader finds it by the image name of the disc (ps4_disc_stream.hpp image_path).
//
// Sources, in order: the key already in place; other key files in the redump folder; IRDs downloaded earlier
// (/data/rpcs3/data/ird); files named after the disc in USB:/redump (.dkey/.key) and USB:/ird (.ird); the IRD
// database over HTTPS (the IRD is kept in /data/rpcs3/data/ird, the index as all.json, which answers when the download
// fails); the rest of those USB folders. USB is only read. The same search runs on an encrypted ISO file
// (find_key_for_image): its own key names and the IRDs next to it come first, and the key is also written under the
// image's name.
//
// Layout: IRD, index, link, disc and verification logic need only the standard library, zlib and Crypto/aes (host
// tested in ps4-port/tests/disc_key_test.cpp); obtain() takes its file, disc and network access as functions.
// PS4 only: HTTPS through libcurl with the packaged root certificates, console file access, and service: request()
// with the drive's status starts one search per disc on a worker thread and never waits; busy_text() while it runs;
// take() hands the finished result (pt-BR text) over once; cancel() ends it at its next step.
// Log lines start with "disc key:"; a key is never logged, only its check value (AES-ECB(key, zeros), 4 bytes).

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Crypto/aes.h"
#include "3rdparty/zlib/zlib/zlib.h"
#include "util/ps4_disc_probe.hpp"
#include "util/ps4_i18n.hpp"

namespace utils::ps4::disc_key
{
using byte = std::uint8_t;
using key128 = std::array<byte, 16>;

inline constexpr std::uint64_t sector_size = 2048;
inline constexpr char index_url[] = "https://flexby420.github.io/playstation_3_ird_database/all.json";
inline constexpr char file_base_url[] = "https://raw.githubusercontent.com/FlexBy420/playstation_3_ird_database/main/";
inline constexpr std::size_t index_limit = 8u << 20;        // all.json: 1.8 MB for 8418 IRDs
inline constexpr std::size_t ird_limit = 32u << 20;         // largest IRD in the database: 8.5 MB
inline constexpr std::size_t ird_unpacked_limit = 48u << 20;
inline constexpr std::size_t key_file_limit = 64;
inline constexpr std::size_t max_candidates = 10;           // at most 10 IRDs per product code in the database
inline constexpr std::size_t max_key_files = 8192;          // per folder (a redump key pack holds ~4500)
inline constexpr std::size_t max_ird_files = 256;           // per folder
inline constexpr std::size_t text_limit = 110;              // XMB line
inline constexpr int fetch_attempts = 3;                     // a download that failed on the way (TLS, reset, HTTP 5xx)

// Data1 -> disc key: AES-128-CBC encryption with these (PS3 Disc Dumper Decrypter.cs, LibIRD IRD.cs D1AesKey/IV)
inline constexpr key128 d1_key{0x38, 0x0b, 0xcf, 0x0b, 0x53, 0x45, 0x5b, 0x3c, 0x78, 0x17, 0xab, 0x4f, 0xa3, 0xba, 0x90, 0xed};
inline constexpr key128 d1_iv{0x69, 0x47, 0x47, 0x72, 0xaf, 0x6f, 0xda, 0xb3, 0x42, 0x74, 0x3a, 0xef, 0xaa, 0x18, 0x62, 0x87};

namespace key_detail
{
inline std::uint32_t le32(const byte* p) noexcept
{
    return std::uint32_t{p[0]} | (std::uint32_t{p[1]} << 8) | (std::uint32_t{p[2]} << 16) | (std::uint32_t{p[3]} << 24);
}

inline std::uint16_t le16(const byte* p) noexcept
{
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

inline char upper(char c) noexcept
{
    return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
}

inline std::string upper(std::string_view text)
{
    std::string out(text);
    for (char& c : out) c = upper(c);
    return out;
}

inline bool iequals(std::string_view a, std::string_view b) noexcept
{
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (upper(a[i]) != upper(b[i])) return false;
    return true;
}

inline bool iends_with(std::string_view text, std::string_view tail) noexcept
{
    return text.size() >= tail.size() && iequals(text.substr(text.size() - tail.size()), tail);
}

// Fixed-size ASCII field without its NUL and space padding
inline std::string field(const byte* data, std::size_t size)
{
    std::string out;
    for (std::size_t i = 0; i < size && data[i]; ++i) out += data[i] >= 0x20 && data[i] < 0x7f ? static_cast<char>(data[i]) : '?';
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

// Printable, single-line, bounded copy of a text for log lines
inline std::string loggable(std::string_view text, std::size_t limit = 96)
{
    std::string out;
    for (const char c : text)
    {
        if (out.size() >= limit) break;
        out += static_cast<unsigned char>(c) < 0x20 || c == 0x7f ? '?' : c;
    }
    return out;
}
}

inline std::string to_hex(const byte* data, std::size_t size)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(size * 2);
    for (std::size_t i = 0; i < size; ++i)
    {
        out += digits[data[i] >> 4];
        out += digits[data[i] & 15];
    }
    return out;
}

// The text "1.2 GB" of a size, for log lines
inline std::string gigabytes(std::uint64_t bytes)
{
    char text[32];
    std::snprintf(text, sizeof(text), "%.2f GB", static_cast<double>(bytes) / 1e9);
    return text;
}

// "2026-10-16" of a time in unix seconds (UTC), "an unknown date" for a negative one
inline std::string date_text(std::int64_t unix_seconds)
{
    if (unix_seconds < 0) return "an unknown date";
    // Civil date of a day count (H. Hinnant's days_from_civil inverse)
    const std::int64_t z = unix_seconds / 86400 + 719468;
    const std::int64_t era = z / 146097, doe = z - era * 146097;
    const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    const std::int64_t day = doy - (153 * mp + 2) / 5 + 1, month = mp < 10 ? mp + 3 : mp - 9, year = yoe + era * 400 + (month <= 2);
    char text[32];
    std::snprintf(text, sizeof(text), "%04lld-%02lld-%02lld", static_cast<long long>(year), static_cast<long long>(month), static_cast<long long>(day));
    return text;
}

// At most 'limit' bytes, cut on a UTF-8 character boundary
inline std::string clip(std::string text, std::size_t limit = text_limit)
{
    if (text.size() <= limit) return text;
    std::size_t cut = limit;
    while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xc0) == 0x80) --cut;
    text.resize(cut);
    return text;
}

// ---------------------------------------------------------------------------------------------------------------
// Keys

inline key128 derive_key(const key128& data1)
{
    aes_context aes{};
    key128 iv = d1_iv;
    key128 out{};
    aes_setkey_enc(&aes, d1_key.data(), 128);
    aes_crypt_cbc(&aes, AES_ENCRYPT, out.size(), iv.data(), data1.data(), out.data());
    return out;
}

// First 4 bytes of AES-ECB(key, 16 zero bytes): tells keys apart in logs without revealing them
inline std::string check_value(const key128& key)
{
    aes_context aes{};
    std::array<byte, 16> zero{}, out{};
    aes_setkey_enc(&aes, key.data(), 128);
    aes_crypt_ecb(&aes, AES_ENCRYPT, zero.data(), out.data());
    return to_hex(out.data(), 4);
}

// The key in a key file: 32 hex characters (optionally followed by blanks) or 16 raw bytes, as the ISO loader reads
inline std::optional<key128> key_from_file(const byte* data, std::size_t size)
{
    const std::string text = disc_keys::contents(data, size);
    key128 key{};
    if (text.size() == 16)
    {
        std::memcpy(key.data(), text.data(), 16);
        return key;
    }
    if (text.size() != 32) return std::nullopt;
    const auto nibble = [](char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
    for (std::size_t i = 0; i < 16; ++i) key[i] = static_cast<byte>((nibble(text[i * 2]) << 4) | nibble(text[i * 2 + 1]));
    return key;
}

// ---------------------------------------------------------------------------------------------------------------
// gzip

enum class unpack_status
{
    ok,
    corrupt,   // not a complete gzip stream
    too_large, // more than the limit once inflated
};

// Inflates a gzip stream into 'out'. With 'head_only' it stops after 'limit' bytes (ok when that many came out).
inline unpack_status gunzip(const byte* data, std::size_t size, std::size_t limit, std::vector<byte>& out, bool head_only = false)
{
    out.clear();
    if (!data || size < 18 || size > (std::size_t{1} << 30)) return unpack_status::corrupt;
    z_stream z{};
    if (inflateInit2(&z, 16 + MAX_WBITS) != Z_OK) return unpack_status::corrupt;
    z.next_in = const_cast<Bytef*>(data);
    z.avail_in = static_cast<uInt>(size);
    unpack_status status = unpack_status::corrupt;
    for (;;)
    {
        // One byte past the limit tells "exactly the limit" from "larger"
        const std::size_t cap = head_only ? limit : limit + 1;
        if (out.size() >= cap)
        {
            status = head_only ? unpack_status::ok : unpack_status::too_large;
            break;
        }
        const std::size_t at = out.size();
        const std::size_t chunk = std::min<std::size_t>(cap - at, std::size_t{1} << 20);
        out.resize(at + chunk);
        z.next_out = out.data() + at;
        z.avail_out = static_cast<uInt>(chunk);
        const int rc = inflate(&z, Z_NO_FLUSH);
        out.resize(at + chunk - z.avail_out);
        if (rc == Z_STREAM_END)
        {
            status = out.size() > limit ? unpack_status::too_large : unpack_status::ok;
            break;
        }
        if (rc != Z_OK && rc != Z_BUF_ERROR) break;
        if (rc == Z_BUF_ERROR && z.avail_in == 0) break; // input ended before the stream did
    }
    inflateEnd(&z);
    if (status != unpack_status::ok) out.clear();
    return status;
}

inline std::uint32_t crc32_of(const byte* data, std::size_t size)
{
    uLong crc = crc32(0L, Z_NULL, 0);
    while (size)
    {
        const uInt part = static_cast<uInt>(std::min<std::size_t>(size, std::size_t{1} << 30));
        crc = crc32(crc, data, part);
        data += part;
        size -= part;
    }
    return static_cast<std::uint32_t>(crc);
}

// ---------------------------------------------------------------------------------------------------------------
// IRD

struct ird
{
    int version = 0;
    std::string product_code;   // "BLES01876"
    std::string title;
    std::string system_version; // "4.46"
    std::string game_version;   // PARAM.SFO VERSION, "01.00"
    std::string app_version;    // PARAM.SFO APP_VER
    std::uint32_t header_size = 0;
    std::uint32_t footer_size = 0;
    std::vector<std::array<byte, 16>> region_md5;
    std::uint32_t file_count = 0;
    std::uint16_t extra_config = 0;
    std::uint16_t attachments = 0;
    std::array<byte, 115> pic{};
    key128 data1{};
    key128 data2{};
    bool has_uid = false;
    std::uint32_t uid = 0;
    std::uint32_t crc = 0;
    std::size_t unpacked_size = 0;
    bool has_sector0 = false;            // sector 0 (region table) of the dumped disc, from the header
    std::array<byte, 2048> sector0{};
};

enum class ird_status
{
    ok,
    not_ird,     // neither "3IRD" nor gzip
    corrupt,     // broken gzip stream
    too_large,
    bad_crc,
    bad_version,
    truncated,   // a field runs past the end
    bad_layout,  // the fields do not end where the PIC, data1, data2 and CRC must lie
};

inline const char* to_string(ird_status status)
{
    switch (status)
    {
    case ird_status::ok: return "ok";
    case ird_status::not_ird: return "not an IRD file";
    case ird_status::corrupt: return "broken gzip data";
    case ird_status::too_large: return "too large";
    case ird_status::bad_crc: return "CRC mismatch";
    case ird_status::bad_version: return "unsupported version";
    case ird_status::truncated: return "truncated";
    case ird_status::bad_layout: return "unexpected layout";
    }
    return "?";
}

inline ird_status parse_ird(const byte* raw, std::size_t raw_size, ird& out)
{
    out = {};
    if (!raw || raw_size < 4) return ird_status::not_ird;
    std::vector<byte> unpacked;
    const byte* data = raw;
    std::size_t size = raw_size;
    if (std::memcmp(raw, "3IRD", 4) == 0)
    {
        if (raw_size > ird_unpacked_limit) return ird_status::too_large;
    }
    else if (raw[0] == 0x1f && raw[1] == 0x8b)
    {
        switch (gunzip(raw, raw_size, ird_unpacked_limit, unpacked))
        {
        case unpack_status::ok: break;
        case unpack_status::too_large: return ird_status::too_large;
        case unpack_status::corrupt: return ird_status::corrupt;
        }
        data = unpacked.data();
        size = unpacked.size();
        if (size < 4 || std::memcmp(data, "3IRD", 4) != 0) return ird_status::not_ird;
    }
    else
    {
        return ird_status::not_ird;
    }

    // Magic, version, code, title length, versions, header/footer sizes, region and file counts, extra config and
    // attachments, PIC, data1, data2, CRC
    if (size < 4 + 1 + 9 + 1 + 14 + 8 + 1 + 4 + 4 + 147 + 4) return ird_status::truncated;
    out.unpacked_size = size;
    const std::size_t end = size - 4;
    out.crc = key_detail::le32(data + end);
    if (crc32_of(data, end) != out.crc) return ird_status::bad_crc;

    std::size_t at = 4;
    const auto take = [&](std::size_t count) -> const byte*
    {
        if (count > end - at) return nullptr;
        const byte* p = data + at;
        at += count;
        return p;
    };
    const auto u32 = [&](std::uint32_t& value)
    {
        const byte* p = take(4);
        if (p) value = key_detail::le32(p);
        return p != nullptr;
    };

    out.version = data[at++];
    if (out.version < 6 || out.version > 9) return ird_status::bad_version;
    const byte* code = take(9);
    if (!code) return ird_status::truncated;
    out.product_code = key_detail::field(code, 9);
    // Title: one length byte (PS3 Disc Dumper) or a 7-bit encoded length (LibIRD, .NET BinaryWriter); equal below 128
    std::size_t title_size = 0;
    for (int shift = 0;; shift += 7)
    {
        const byte* b = take(1);
        if (!b || shift > 14) return ird_status::truncated;
        title_size |= std::size_t{static_cast<byte>(*b & 0x7f)} << shift;
        if (!(*b & 0x80)) break;
    }
    const byte* title = take(title_size);
    if (!title) return ird_status::truncated;
    out.title.assign(reinterpret_cast<const char*>(title), title_size);
    const byte* versions = take(14);
    if (!versions) return ird_status::truncated;
    out.system_version = key_detail::field(versions, 4);
    out.game_version = key_detail::field(versions + 4, 5);
    out.app_version = key_detail::field(versions + 9, 5);
    if (out.version == 7)
    {
        std::uint32_t id = 0;
        if (!u32(id)) return ird_status::truncated;
    }
    if (!u32(out.header_size)) return ird_status::truncated;
    const byte* header = take(out.header_size);
    if (!header) return ird_status::truncated;
    if (!u32(out.footer_size) || !take(out.footer_size)) return ird_status::truncated;
    const byte* regions = take(1);
    if (!regions) return ird_status::truncated;
    for (unsigned i = 0; i < *regions; ++i)
    {
        const byte* md5 = take(16);
        if (!md5) return ird_status::truncated;
        auto& hash = out.region_md5.emplace_back();
        std::memcpy(hash.data(), md5, 16);
    }
    if (!u32(out.file_count)) return ird_status::truncated;
    if (out.file_count > (end - at) / 24 || !take(std::size_t{out.file_count} * 24)) return ird_status::truncated;
    const byte* extra = take(4);
    if (!extra) return ird_status::truncated;
    out.extra_config = key_detail::le16(extra);
    out.attachments = key_detail::le16(extra + 2);

    const std::size_t tail = end - at;
    if (tail != 147 && tail != 151) return ird_status::bad_layout;
    if (out.version >= 9) std::memcpy(out.pic.data(), take(115), 115);
    std::memcpy(out.data1.data(), take(16), 16);
    std::memcpy(out.data2.data(), take(16), 16);
    if (out.version < 9) std::memcpy(out.pic.data(), take(115), 115);
    if (tail == 151)
    {
        out.has_uid = true;
        u32(out.uid);
    }

    // Sector 0 of the dumped disc leads the gzip header; a header that does not inflate leaves it unknown
    std::vector<byte> head;
    if (gunzip(header, out.header_size, out.sector0.size(), head, true) == unpack_status::ok && head.size() == out.sector0.size())
    {
        std::memcpy(out.sector0.data(), head.data(), head.size());
        out.has_sector0 = true;
    }
    return ird_status::ok;
}

// The product code of an IRD from its first bytes (a gzip file needs only its first few hundred), "" when unknown
inline std::string peek_product_code(const byte* data, std::size_t size)
{
    if (!data || size < 14) return {};
    std::vector<byte> head;
    if (data[0] == 0x1f && data[1] == 0x8b)
    {
        if (gunzip(data, size, 14, head, true) != unpack_status::ok) return {};
    }
    else
    {
        head.assign(data, data + 14);
    }
    if (head.size() < 14 || std::memcmp(head.data(), "3IRD", 4) != 0 || head[4] < 6 || head[4] > 9) return {};
    return key_detail::field(head.data() + 5, 9);
}

// ---------------------------------------------------------------------------------------------------------------
// Index (all.json)

struct index_entry
{
    std::string title;
    std::string system_version; // "fw-ver"
    std::string game_version;   // "game-ver"
    std::string app_version;    // "app-ver"
    std::string link;           // path of the file in the repository
    std::uint64_t disc_size = 0;
    std::uint64_t file_count = 0;
};

namespace json_detail
{
struct cursor
{
    const char* p;
    const char* end;
};

inline void spaces(cursor& c) noexcept
{
    while (c.p < c.end && (*c.p == ' ' || *c.p == '\t' || *c.p == '\n' || *c.p == '\r')) ++c.p;
}

inline bool literal(cursor& c, std::string_view word) noexcept
{
    if (static_cast<std::size_t>(c.end - c.p) < word.size() || std::string_view(c.p, word.size()) != word) return false;
    c.p += word.size();
    return true;
}

inline void utf8(std::string& out, std::uint32_t cp)
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

inline bool hex4(cursor& c, std::uint32_t& value) noexcept
{
    if (c.end - c.p < 4) return false;
    value = 0;
    for (int i = 0; i < 4; ++i)
    {
        const char h = *c.p++;
        value <<= 4;
        if (h >= '0' && h <= '9') value |= static_cast<std::uint32_t>(h - '0');
        else if ((h | 0x20) >= 'a' && (h | 0x20) <= 'f') value |= static_cast<std::uint32_t>((h | 0x20) - 'a' + 10);
        else return false;
    }
    return true;
}

// A JSON string; 'out' (when given) gets at most 'limit' bytes of it
inline bool string(cursor& c, std::string* out, std::size_t limit = 1024)
{
    if (c.p >= c.end || *c.p != '"') return false;
    ++c.p;
    std::string scratch;
    std::string& text = out ? *out : scratch;
    text.clear();
    while (c.p < c.end)
    {
        const char ch = *c.p++;
        if (ch == '"')
        {
            if (text.size() > limit) text = clip(std::move(text), limit);
            return true;
        }
        if (static_cast<unsigned char>(ch) < 0x20) return false;
        const bool keep = out && text.size() <= limit;
        if (ch != '\\')
        {
            if (keep) text += ch;
            continue;
        }
        if (c.p >= c.end) return false;
        const char escape = *c.p++;
        std::uint32_t cp = 0;
        switch (escape)
        {
        case '"': case '\\': case '/': cp = static_cast<std::uint32_t>(escape); break;
        case 'b': cp = '\b'; break;
        case 'f': cp = '\f'; break;
        case 'n': cp = '\n'; break;
        case 'r': cp = '\r'; break;
        case 't': cp = '\t'; break;
        case 'u':
            if (!hex4(c, cp)) return false;
            if (cp >= 0xd800 && cp <= 0xdbff)
            {
                std::uint32_t low = 0;
                if (c.end - c.p >= 2 && c.p[0] == '\\' && c.p[1] == 'u')
                {
                    c.p += 2;
                    if (!hex4(c, low)) return false;
                }
                cp = low >= 0xdc00 && low <= 0xdfff ? 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00) : 0xfffd;
            }
            else if (cp >= 0xdc00 && cp <= 0xdfff)
            {
                cp = 0xfffd;
            }
            break;
        default: return false;
        }
        if (keep) utf8(text, cp);
    }
    return false;
}

// A JSON number; 'value' gets its integer part when it is a non-negative integer that fits
inline bool number(cursor& c, std::uint64_t* value) noexcept
{
    const char* start = c.p;
    if (c.p < c.end && *c.p == '-') ++c.p;
    std::uint64_t n = 0;
    bool digits = false, fits = true;
    while (c.p < c.end && *c.p >= '0' && *c.p <= '9')
    {
        const std::uint64_t d = static_cast<std::uint64_t>(*c.p - '0');
        if (n > (UINT64_MAX - d) / 10) fits = false;
        n = n * 10 + d;
        digits = true;
        ++c.p;
    }
    if (!digits) return false;
    bool integer = true;
    if (c.p < c.end && *c.p == '.')
    {
        integer = false;
        ++c.p;
        if (c.p >= c.end || *c.p < '0' || *c.p > '9') return false;
        while (c.p < c.end && *c.p >= '0' && *c.p <= '9') ++c.p;
    }
    if (c.p < c.end && (*c.p == 'e' || *c.p == 'E'))
    {
        integer = false;
        ++c.p;
        if (c.p < c.end && (*c.p == '+' || *c.p == '-')) ++c.p;
        if (c.p >= c.end || *c.p < '0' || *c.p > '9') return false;
        while (c.p < c.end && *c.p >= '0' && *c.p <= '9') ++c.p;
    }
    if (value) *value = integer && fits && *start != '-' ? n : 0;
    return true;
}

inline bool skip(cursor& c, int depth)
{
    if (depth > 32) return false;
    spaces(c);
    if (c.p >= c.end) return false;
    switch (*c.p)
    {
    case '"': return string(c, nullptr);
    case 't': return literal(c, "true");
    case 'f': return literal(c, "false");
    case 'n': return literal(c, "null");
    case '{':
    case '[':
    {
        const bool object = *c.p == '{';
        const char close = object ? '}' : ']';
        ++c.p;
        spaces(c);
        if (c.p < c.end && *c.p == close)
        {
            ++c.p;
            return true;
        }
        for (;;)
        {
            spaces(c);
            if (object)
            {
                if (!string(c, nullptr)) return false;
                spaces(c);
                if (c.p >= c.end || *c.p != ':') return false;
                ++c.p;
            }
            if (!skip(c, depth + 1)) return false;
            spaces(c);
            if (c.p >= c.end) return false;
            if (*c.p == ',')
            {
                ++c.p;
                continue;
            }
            if (*c.p != close) return false;
            ++c.p;
            return true;
        }
    }
    default: return number(c, nullptr);
    }
}

inline bool entry(cursor& c, index_entry& out)
{
    spaces(c);
    if (c.p >= c.end || *c.p != '{') return false;
    ++c.p;
    spaces(c);
    if (c.p < c.end && *c.p == '}')
    {
        ++c.p;
        return true;
    }
    for (;;)
    {
        spaces(c);
        std::string name;
        if (!string(c, &name, 64)) return false;
        spaces(c);
        if (c.p >= c.end || *c.p != ':') return false;
        ++c.p;
        spaces(c);
        std::string* text = name == "title" ? &out.title : name == "fw-ver" ? &out.system_version : name == "game-ver" ? &out.game_version
            : name == "app-ver" ? &out.app_version : name == "link" ? &out.link : nullptr;
        std::uint64_t* value = name == "disc-size" ? &out.disc_size : name == "file-count" ? &out.file_count : nullptr;
        if (text && c.p < c.end && *c.p == '"')
        {
            if (!string(c, text, 512)) return false;
        }
        else if (value && c.p < c.end && (*c.p == '-' || (*c.p >= '0' && *c.p <= '9')))
        {
            if (!number(c, value)) return false;
        }
        else if (!skip(c, 2))
        {
            return false;
        }
        spaces(c);
        if (c.p >= c.end) return false;
        if (*c.p == ',')
        {
            ++c.p;
            continue;
        }
        if (*c.p != '}') return false;
        ++c.p;
        return true;
    }
}
}

enum class index_status
{
    ok,
    not_json, // not one JSON object of arrays
};

// The entries listed for 'product_code' (any case). 'codes' gets the number of product codes in the index.
inline index_status parse_index(std::string_view json, std::string_view product_code, std::vector<index_entry>& out, std::size_t* codes = nullptr)
{
    using namespace json_detail;
    out.clear();
    std::size_t count = 0;
    if (codes) *codes = 0;
    cursor c{json.data(), json.data() + json.size()};
    if (c.end - c.p >= 3 && std::memcmp(c.p, "\xef\xbb\xbf", 3) == 0) c.p += 3;
    spaces(c);
    if (c.p >= c.end || *c.p != '{') return index_status::not_json;
    ++c.p;
    spaces(c);
    if (c.p < c.end && *c.p == '}') return index_status::ok;
    for (;;)
    {
        spaces(c);
        std::string code;
        if (!string(c, &code, 64)) return index_status::not_json;
        spaces(c);
        if (c.p >= c.end || *c.p != ':') return index_status::not_json;
        ++c.p;
        ++count;
        if (codes) *codes = count;
        spaces(c);
        if (key_detail::iequals(code, product_code) && c.p < c.end && *c.p == '[')
        {
            ++c.p;
            spaces(c);
            if (c.p < c.end && *c.p == ']') ++c.p;
            else
            {
                for (;;)
                {
                    index_entry e;
                    if (!entry(c, e)) return index_status::not_json;
                    if (out.size() < 64 && !e.link.empty()) out.push_back(std::move(e));
                    spaces(c);
                    if (c.p >= c.end) return index_status::not_json;
                    if (*c.p == ',')
                    {
                        ++c.p;
                        continue;
                    }
                    if (*c.p != ']') return index_status::not_json;
                    ++c.p;
                    break;
                }
            }
        }
        else if (!skip(c, 1))
        {
            return index_status::not_json;
        }
        spaces(c);
        if (c.p >= c.end) return index_status::not_json;
        if (*c.p == ',')
        {
            ++c.p;
            continue;
        }
        if (*c.p != '}') return index_status::not_json;
        return index_status::ok;
    }
}

// ---------------------------------------------------------------------------------------------------------------
// Links and file names

// A link of the index the console may request: a relative path to a .ird file, nothing that leaves the repository
inline bool safe_link(std::string_view link) noexcept
{
    if (link.empty() || link.size() > 300 || link.front() == '/' || !key_detail::iends_with(link, ".ird")) return false;
    if (link.find("..") != std::string_view::npos || link.find("//") != std::string_view::npos) return false;
    for (const char c : link)
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f || c == '\\' || c == '?' || c == '#' || c == '%' || c == ':') return false;
    return true;
}

inline std::string file_url(std::string_view link)
{
    static constexpr char digits[] = "0123456789ABCDEF";
    std::string url = file_base_url;
    for (const char ch : link)
    {
        const auto c = static_cast<unsigned char>(ch);
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~' || c == '/') url += ch;
        else
        {
            url += '%';
            url += digits[c >> 4];
            url += digits[c & 15];
        }
    }
    return url;
}

// Cache file name of a link: its path with '/' as '_', only portable characters, at most 160 bytes, ".ird" kept
inline std::string cache_name(std::string_view link)
{
    std::string out;
    for (const char ch : link)
    {
        const auto c = static_cast<unsigned char>(ch);
        const bool plain = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || std::strchr(" ._-()+,&", ch);
        out += c && plain ? ch : '_';
    }
    if (out.size() > 160) out = out.substr(0, 156) + ".ird";
    return out;
}

// A file name that names 'product_code' ("BLES01876_...", "BLES-01876 - FIFA 14 ...", any case)
inline bool name_mentions(std::string_view name, std::string_view product_code)
{
    std::string squeezed;
    for (const char c : name)
        if (c != '-' && c != ' ' && c != '_') squeezed += key_detail::upper(c);
    return !product_code.empty() && squeezed.find(key_detail::upper(product_code)) != std::string::npos;
}

// ---------------------------------------------------------------------------------------------------------------
// The disc

// The leading bytes of a well known file of the disc, the ones its magic lies in once decrypted
struct magic_block
{
    std::string_view path;
    std::string_view magic;
    std::uint64_t offset = 0; // first byte of the file (sector aligned): the IV is built from its LBA
    std::array<byte, 16> data{};
    bool encrypted_region = true; // the file starts in an encrypted region of the region table

    bool clear() const noexcept { return std::memcmp(data.data(), magic.data(), magic.size()) == 0; }
    // A file in a plain region that reads in the clear is never decrypted (ISO.cpp decrypt() skips plain regions):
    // no key can be tested on it
    bool testable() const noexcept { return encrypted_region || !clear(); }
};

struct disc_identity
{
    std::string header_id;    // canonical title id of sector 1 ("BLES01876"): the key file's name
    std::string product_code; // PARAM.SFO TITLE_ID (the header id when unreadable): what the IRD lists
    std::string title;
    std::string version;      // PARAM.SFO VERSION
    std::string app_version;  // PARAM.SFO APP_VER
    std::uint32_t plain_regions = 0;
    std::uint64_t size = 0;   // bytes, end of the last plain region
    std::array<byte, 2048> sector0{};
    std::vector<magic_block> blocks;

    // A disc image whose well known files read in the clear needs no key (as ISO.cpp is_any_block_encrypted)
    bool needs_key() const
    {
        return std::any_of(blocks.begin(), blocks.end(), [](const magic_block& b) { return !b.clear(); });
    }
};

using read_fn = std::function<bool(std::uint64_t offset, void* buffer, std::size_t size)>;

enum class identity_status
{
    ok,
    unreadable,    // a read failed
    not_ps3,       // no 'PlayStation3' header or region table
    no_filesystem, // no ISO 9660 tree with PS3_GAME/PARAM.SFO
};

// Sectors 0-1, the ISO 9660 tree down to PARAM.SFO, LIC.DAT and EBOOT.BIN, and the first 16 bytes of those two files
inline identity_status read_identity(const read_fn& read, disc_identity& out, std::string& detail)
{
    namespace ds = disc_stream;
    out = {};
    std::array<byte, 2 * sector_size> head{};
    if (!read || !read(0, head.data(), head.size()))
    {
        detail = "sectors 0-1 unreadable";
        return identity_status::unreadable;
    }
    const ds::layout table = ds::parse_layout(head.data(), sector_size);
    out.header_id = ds::title_id_from_header(head.data() + sector_size, sector_size);
    if (!table.valid || !ds::has_ps3_magic(head.data() + sector_size, sector_size))
    {
        detail = table.valid ? "no 'PlayStation3' header" : "no valid region table";
        return identity_status::not_ps3;
    }
    std::memcpy(out.sector0.data(), head.data(), out.sector0.size());
    out.plain_regions = table.plain_regions;
    out.size = table.size_bytes();

    std::array<byte, sector_size> pvd{};
    if (!read(16 * sector_size, pvd.data(), pvd.size()))
    {
        detail = "sector 16 unreadable";
        return identity_status::unreadable;
    }
    const auto root = ds::root_directory(pvd.data(), pvd.size());
    if (!root || !root->directory)
    {
        detail = "no ISO 9660 volume descriptor";
        return identity_status::no_filesystem;
    }
    bool failed_read = false;
    // Directories up to 1 MiB (as locate_files): a USRDIR of a few thousand entries passes 64 KiB, and a disc without a
    // LIC.DAT (Demon's Souls BLUS30443) has nothing else to test a key against than EBOOT.BIN
    const auto load = [&](const ds::extent& e, std::vector<byte>& data)
    {
        if (!e.bytes || e.bytes > (e.directory ? (1u << 20) : 65536u)) return false;
        data.assign(e.bytes, 0);
        if (read(std::uint64_t{e.lba} * sector_size, data.data(), data.size())) return true;
        failed_read = true;
        return false;
    };
    std::vector<byte> dir, game, file;
    std::optional<ds::extent> game_dir;
    if (load(*root, dir)) game_dir = ds::find_entry(dir.data(), dir.size(), "PS3_GAME");
    if (!game_dir || !game_dir->directory || !load(*game_dir, game))
    {
        detail = failed_read ? "directory unreadable" : "no PS3_GAME folder";
        return failed_read ? identity_status::unreadable : identity_status::no_filesystem;
    }
    const auto sfo = ds::find_entry(game.data(), game.size(), "PARAM.SFO");
    if (!sfo || sfo->directory || !load(*sfo, file))
    {
        detail = failed_read ? "PARAM.SFO unreadable" : "no PS3_GAME/PARAM.SFO";
        return failed_read ? identity_status::unreadable : identity_status::no_filesystem;
    }
    out.title = ds::sfo_string(file.data(), file.size(), "TITLE");
    out.version = ds::sfo_string(file.data(), file.size(), "VERSION");
    out.app_version = ds::sfo_string(file.data(), file.size(), "APP_VER");
    out.product_code = ds::canonical_title_id(ds::sfo_string(file.data(), file.size(), "TITLE_ID"));
    if (out.product_code.empty()) out.product_code = out.header_id;

    // The files ISO.cpp read_magic_blocks checks, in its order
    static constexpr std::array<std::array<std::string_view, 3>, 2> known{{
        {"LICDIR", "LIC.DAT", "PS3LICDA"},
        {"USRDIR", "EBOOT.BIN", "SCE"},
    }};
    for (const auto& [folder, name, magic] : known)
    {
        const auto sub = ds::find_entry(game.data(), game.size(), folder);
        if (!sub || !sub->directory || !load(*sub, dir))
        {
            if (failed_read)
            {
                detail = "PS3_GAME/" + std::string(folder) + " unreadable";
                return identity_status::unreadable;
            }
            continue;
        }
        const auto target = ds::find_entry(dir.data(), dir.size(), name);
        if (!target || target->directory) continue;
        magic_block block;
        block.path = name == "LIC.DAT" ? std::string_view("PS3_GAME/LICDIR/LIC.DAT") : std::string_view("PS3_GAME/USRDIR/EBOOT.BIN");
        block.magic = magic;
        block.offset = std::uint64_t{target->lba} * sector_size;
        block.encrypted_region = false;
        for (const auto& [first, last] : table.plain)
        {
            if (target->lba < first)
            {
                block.encrypted_region = true;
                break;
            }
            if (target->lba <= last) break;
        }
        if (!read(block.offset, block.data.data(), block.data.size()))
        {
            detail = std::string(block.path) + " unreadable";
            return identity_status::unreadable;
        }
        out.blocks.push_back(block);
    }
    return identity_status::ok;
}

// ISO.cpp decrypt_data for a sector-aligned block: AES-128-CBC with the sector's LBA as the IV (big-endian, bytes 12-15)
inline bool decrypts_to(const key128& key, const magic_block& block)
{
    aes_context aes{};
    if (aes_setkey_dec(&aes, key.data(), 128) != 0) return false;
    std::array<byte, 16> iv{}, plain{};
    const auto lba = static_cast<std::uint32_t>(block.offset / sector_size);
    iv[12] = static_cast<byte>(lba >> 24);
    iv[13] = static_cast<byte>(lba >> 16);
    iv[14] = static_cast<byte>(lba >> 8);
    iv[15] = static_cast<byte>(lba);
    if (aes_crypt_cbc(&aes, AES_DECRYPT, plain.size(), iv.data(), block.data.data(), plain.data()) != 0) return false;
    return std::memcmp(plain.data(), block.magic.data(), block.magic.size()) == 0;
}

// The key decrypts every well known file of the disc (ISO.cpp decrypts_all_blocks); never true without one
inline bool decrypts_all(const key128& key, const std::vector<magic_block>& blocks)
{
    return std::any_of(blocks.begin(), blocks.end(), [](const magic_block& b) { return b.testable(); }) &&
        std::all_of(blocks.begin(), blocks.end(), [&key](const magic_block& b) { return !b.testable() || decrypts_to(key, b); });
}

// How well an index entry describes the disc: game version 4, app version 2, size 1
inline int match_score(const index_entry& e, const disc_identity& disc)
{
    return (!disc.version.empty() && e.game_version == disc.version) * 4 + (!disc.app_version.empty() && e.app_version == disc.app_version) * 2 +
        (disc.size && e.disc_size == disc.size);
}

// Best match first (stable within a score); at most max_candidates
inline std::vector<index_entry> rank_candidates(std::vector<index_entry> entries, const disc_identity& disc)
{
    std::stable_sort(entries.begin(), entries.end(), [&disc](const index_entry& a, const index_entry& b) { return match_score(a, disc) > match_score(b, disc); });
    if (entries.size() > max_candidates) entries.resize(max_candidates);
    return entries;
}

// ---------------------------------------------------------------------------------------------------------------
// The search

enum class outcome
{
    not_needed,      // the disc's files read in the clear
    already_present, // <TITLE_ID>.dkey (or .key) decrypts the disc
    local_key,       // another key file of the redump folder decrypts it; copied as <TITLE_ID>.dkey
    cached_ird,      // an IRD downloaded earlier
    usb_key,         // USB:/redump
    usb_ird,         // USB:/ird
    image_ird,       // an IRD next to the ISO file
    network,         // the IRD database
    no_internet,
    not_in_database,
    no_matching_key, // IRDs found, none decrypts the disc
    server_error,    // HTTP error or an unreadable index
    fetch_failed,    // TLS or another transfer failure
    unavailable,     // no root certificates in the package
    disc_unreadable,
    cannot_verify,   // neither LIC.DAT nor EBOOT.BIN on the disc
    write_failed,
    cancelled,
};

inline const char* to_string(outcome what)
{
    switch (what)
    {
    case outcome::not_needed: return "not needed";
    case outcome::already_present: return "already present";
    case outcome::local_key: return "redump folder";
    case outcome::cached_ird: return "cached IRD";
    case outcome::usb_key: return "USB key";
    case outcome::usb_ird: return "USB IRD";
    case outcome::image_ird: return "IRD next to the ISO";
    case outcome::network: return "IRD database";
    case outcome::no_internet: return "no internet";
    case outcome::not_in_database: return "not in the database";
    case outcome::no_matching_key: return "no matching key";
    case outcome::server_error: return "server error";
    case outcome::fetch_failed: return "download failed";
    case outcome::unavailable: return "unavailable";
    case outcome::disc_unreadable: return "disc unreadable";
    case outcome::cannot_verify: return "cannot verify";
    case outcome::write_failed: return "write failed";
    case outcome::cancelled: return "cancelled";
    }
    return "?";
}

// A verified key is in place (or none is needed)
inline bool succeeded(outcome what) noexcept
{
    return what <= outcome::network;
}

// A search that failed for the network or the database server is repeated for the same disc while it stays in the
// drive (the console may get its connection after the disc went in); every other answer stands until the disc changes
inline bool retry_later(outcome what) noexcept
{
    return what == outcome::no_internet || what == outcome::server_error || what == outcome::fetch_failed;
}

// 1 min, then doubled up to 10 min
inline std::int64_t next_backoff(std::int64_t previous) noexcept
{
    return previous <= 0 ? 60000 : std::min<std::int64_t>(previous * 2, 600000);
}

struct fetch_reply
{
    enum class kind
    {
        ok,
        offline,     // no link, no DNS, no connection
        http,        // an HTTP status other than 200
        failed,      // TLS or another transfer error
        too_large,
        cancelled,
        unavailable, // no root certificates
    };

    kind status = kind::failed;
    long http = 0;
    std::string body;
    std::string log;   // startup.log detail: host, address, curl code, timing
    std::string cause; // short pt-BR cause for the XMB
};

struct environment
{
    read_fn read_disc;
    std::function<fetch_reply(const std::string& url, std::size_t limit)> fetch;                  // empty: no network
    std::function<bool(const std::string& path, std::size_t limit, std::vector<byte>& out)> read_file;
    std::function<bool(const std::string& path, const void* data, std::size_t size)> write_file; // replaces atomically
    std::function<bool(const std::string& from, const std::string& to)> rename_file;
    std::function<std::vector<std::string>(const std::string& folder)> list_files;               // regular files
    std::function<std::vector<std::string>(const std::string& folder)> list_folders;
    std::function<bool()> stopped;
    std::function<void(const std::string& line)> log;
    std::function<void(std::int64_t ms)> pause; // between attempts of a download (empty: none); ends early on a stop
    std::function<std::int64_t(const std::string& path)> file_time; // modification time (unix seconds), -1 when absent
    std::string key_dir;                // "/data/rpcs3/data/redump/"
    std::string ird_dir;                // "/data/rpcs3/data/ird/"
    std::vector<std::string> usb_roots; // "/mnt/usb0" ...
    // An ISO file instead of the drive (find_key_for_image): the key files the ISO loader looks up by the image's name
    // (tested first, like the key in place), the name the key is also written as (<key_dir><image_name>.dkey) and the
    // folders of the image whose IRDs are tried before the cached ones ("<folder>/", "<folder>/ird/")
    std::vector<std::string> image_key_files;
    std::string image_name;
    std::vector<std::string> image_ird_folders;
};

struct result
{
    outcome what = outcome::disc_unreadable;
    bool ok = false;         // a verified key is in place, or none is needed
    bool changed = false;    // a key file was written
    std::string text;        // interface language, at most line_limit() bytes
    std::string line;        // the final startup.log line
    std::string title_id;
    std::string key_path;
    std::string source;      // where the key came from
    std::string check_value; // AES-ECB(key, zeros), 4 bytes in hex; never the key
    std::uint32_t tested = 0; // IRDs tried from the database
    std::optional<key128> key; // the verified key (in memory only)
};

// Byte limit of a result line in the interface language: text_limit, twice that in Arabic (two bytes per letter; the
// XMB fits the line to the screen)
inline std::size_t line_limit() noexcept { return i18n::rtl() ? 2 * text_limit : text_limit; }

// Text of an outcome in the interface language. 'title_id', 'count', 'http' and 'cause' fill the texts that name them.
inline std::string outcome_text(outcome what, std::string_view title_id = {}, std::uint32_t count = 0, long http = 0, std::string_view cause = {})
{
    using i18n::str;
    std::string text;
    switch (what)
    {
    case outcome::not_needed: text = i18n::tr(str::DKEY_ESTE_DISCO_NAO_PRECISA_CHAVE); break;
    case outcome::already_present: text = i18n::tr(str::DKEY_CHAVE_DISCO_JA_ESTA_CONSOLE); break;
    case outcome::local_key: text = i18n::tr(str::DKEY_CHAVE_DISCO_ENCONTRADA_DATA_RPCS3); break;
    case outcome::cached_ird: text = i18n::tr(str::DKEY_CHAVE_DISCO_OBTIDA_AUTOMATICAMENTE_IRD); break;
    case outcome::usb_key: text = i18n::tr(str::DKEY_CHAVE_DISCO_IMPORTADA_USB_REDUMP); break;
    case outcome::usb_ird: text = i18n::tr(str::DKEY_CHAVE_DISCO_OBTIDA_IRD); break;
    case outcome::image_ird: text = i18n::tr(str::DKEY_CHAVE_OBTIDA_IRD_LADO_ISO); break;
    case outcome::network: text = i18n::tr(str::DKEY_CHAVE_DISCO_OBTIDA_AUTOMATICAMENTE); break;
    case outcome::no_internet: text = i18n::tr(str::DKEY_SEM_INTERNET_NAO_FOI_POSSIVEL); break;
    case outcome::not_in_database:
        text = title_id.empty() ? std::string(i18n::tr(str::DKEY_ESTE_DISCO_NAO_ESTA_BASE)) : i18n::trf(str::DKEY_ESTE_DISCO_NAO_ESTA_BASE_ID, {title_id});
        break;
    case outcome::no_matching_key: text = i18n::trf(count == 1 ? str::DKEY_NENHUMA_CHAVE_BASE_ABRE_1 : str::DKEY_NENHUMA_CHAVE_BASE_ABRE, {count}); break;
    case outcome::server_error:
        text = http ? i18n::trf(str::DKEY_BASE_CHAVES_NAO_RESPONDEU_HTTP, {http}) : std::string(i18n::tr(str::DKEY_RESPOSTA_INVALIDA_BASE_CHAVES_TENTE));
        break;
    case outcome::fetch_failed:
        text = cause.empty() ? i18n::trf(str::DKEY_FALHA_BUSCAR_CHAVE_DISCO, {str::DKEY_ERRO_REDE}) : i18n::trf(str::DKEY_FALHA_BUSCAR_CHAVE_DISCO, {cause});
        break;
    case outcome::unavailable: text = i18n::tr(str::DKEY_BUSCA_AUTOMATICA_CHAVE_INDISPONIVEL_PACOTE); break;
    case outcome::disc_unreadable: text = i18n::tr(str::DKEY_NAO_FOI_POSSIVEL_LER_DISCO); break;
    case outcome::cannot_verify: text = i18n::tr(str::DKEY_ESTE_DISCO_NAO_TEM_LIC); break;
    case outcome::write_failed: text = i18n::tr(str::DKEY_CHAVE_CONFERIDA_MAS_NAO_FOI); break;
    case outcome::cancelled: text = i18n::tr(str::DKEY_BUSCA_CHAVE_DISCO_CANCELADA); break;
    }
    return clip(std::move(text), line_limit());
}

inline result obtain(const environment& env)
{
    result r;
    std::vector<std::string> wrong_files; // key files under the target name that do not decrypt the disc
    const auto log = [&env](const std::string& line)
    {
        if (env.log) env.log("disc key: " + line);
    };
    const auto stopped = [&env] { return env.stopped && env.stopped(); };
    const auto finish = [&](outcome what, std::string text, const std::string& detail) -> result
    {
        r.what = what;
        r.ok = succeeded(what);
        r.text = clip(std::move(text), line_limit());
        r.line = "disc key: result: " + r.text + " | " + std::string(to_string(what)) + (detail.empty() ? std::string{} : "; " + detail);
        if (env.log) env.log(r.line);
        return std::move(r);
    };
    const auto read_small = [&env](const std::string& path, std::size_t limit, std::vector<byte>& data)
    {
        data.clear();
        return env.read_file && env.read_file(path, limit, data);
    };

    disc_identity disc;
    std::string detail;
    switch (read_identity(env.read_disc, disc, detail))
    {
    case identity_status::ok: break;
    case identity_status::unreadable: return finish(outcome::disc_unreadable, outcome_text(outcome::disc_unreadable), detail);
    case identity_status::not_ps3:
    case identity_status::no_filesystem: return finish(outcome::disc_unreadable, outcome_text(outcome::disc_unreadable), "not a PS3 game disc: " + detail);
    }
    r.title_id = disc.header_id.empty() ? disc.product_code : disc.header_id;
    std::string files;
    for (const magic_block& b : disc.blocks) files += std::string(files.empty() ? "" : ", ") + std::string(b.path) + (b.clear() ? " (clear)" : " (encrypted)");
    log(r.title_id + " '" + key_detail::loggable(disc.title) + "', TITLE_ID " + disc.product_code + ", VERSION " + disc.version + ", APP_VER " +
        disc.app_version + ", " + gigabytes(disc.size) + ", " + std::to_string(disc.plain_regions) + " plain regions; " + (files.empty() ? "no LIC.DAT/EBOOT.BIN" : files));
    if (r.title_id.empty()) return finish(outcome::disc_unreadable, outcome_text(outcome::disc_unreadable), "no title id in sector 1 or PARAM.SFO");
    if (disc.blocks.empty()) return finish(outcome::cannot_verify, outcome_text(outcome::cannot_verify), "nothing to test a key against");
    if (!disc.needs_key()) return finish(outcome::not_needed, outcome_text(outcome::not_needed), "LIC.DAT/EBOOT.BIN read in the clear");

    const std::string stem = env.key_dir + r.title_id;
    r.key_path = stem + ".dkey";
    std::vector<byte> data;

    // Writes the verified key as <TITLE_ID>.dkey (32 hex characters, the form the loader reads first)
    const auto install = [&](const key128& key, outcome what, const std::string& source) -> result
    {
        r.source = source;
        r.check_value = check_value(key);
        r.key = key; // verified, so usable even when the file cannot be written
        for (const std::string& wrong : wrong_files)
        {
            // Kept for the player to look at, out of the loader's way
            if (env.rename_file && env.rename_file(wrong, wrong + ".bad")) log("moved " + wrong + " (does not decrypt this disc) to " + wrong + ".bad");
            else log("could not move " + wrong + " aside (it does not decrypt this disc)");
        }
        const std::string hex = to_hex(key.data(), key.size());
        std::vector<byte> back;
        if (!env.write_file || !env.write_file(r.key_path, hex.data(), hex.size()) || !read_small(r.key_path, key_file_limit, back) ||
            std::string(back.begin(), back.end()) != hex)
            return finish(outcome::write_failed, outcome_text(outcome::write_failed), "cannot write " + r.key_path + " (key " + r.check_value + " verified, from " + source + ")");
        r.changed = true;
        std::string also;
        if (!env.image_name.empty())
        {
            // The name the ISO loader looks up first: two pressings with one header id never share it
            const std::string copy = env.key_dir + env.image_name + ".dkey";
            if (copy != r.key_path)
            {
                if (env.write_file(copy, hex.data(), hex.size())) also = " and " + copy;
                else log("could not write " + copy + " (the loader still finds " + r.key_path + ")");
            }
        }
        return finish(what, outcome_text(what, r.title_id), "saved " + r.key_path + also + " (key check " + r.check_value + ", from " + source + ")");
    };

    // 1. The key already in place: for an ISO first the key files its loader looks up by the image's name. A wrong file
    // the loader would take before the right one is moved aside (install()).
    std::vector<std::string> in_place = env.image_key_files;
    for (const char* extension : {".dkey", ".key"}) in_place.push_back(stem + extension);
    for (const std::string& path : in_place)
    {
        if (!read_small(path, key_file_limit, data)) continue;
        const auto key = key_from_file(data.data(), data.size());
        if (key && decrypts_all(*key, disc.blocks))
        {
            if (!wrong_files.empty()) return install(*key, outcome::already_present, path);
            r.key_path = path;
            r.check_value = check_value(*key);
            r.source = path;
            r.key = key;
            return finish(outcome::already_present, outcome_text(outcome::already_present), path + " verified (key check " + r.check_value + ")");
        }
        log(path + (key ? " does not decrypt this disc (key check " + check_value(*key) + ")" : " holds no key"));
        wrong_files.push_back(path);
    }

    const auto test_key_files = [&](const std::string& folder, outcome what, const std::string& label) -> std::optional<result>
    {
        if (!env.list_files) return std::nullopt;
        std::vector<std::string> names = env.list_files(folder);
        // The file named after the disc first
        std::stable_partition(names.begin(), names.end(), [&](const std::string& n) { return key_detail::iequals(n, r.title_id + ".dkey") || key_detail::iequals(n, r.title_id + ".key"); });
        std::size_t tried = 0;
        for (const std::string& name : names)
        {
            if (tried >= max_key_files || stopped()) break;
            if (!key_detail::iends_with(name, ".dkey") && !key_detail::iends_with(name, ".key")) continue;
            const std::string path = folder + name;
            if (std::find(wrong_files.begin(), wrong_files.end(), path) != wrong_files.end()) continue;
            ++tried;
            if (!read_small(path, key_file_limit, data)) continue;
            const auto key = key_from_file(data.data(), data.size());
            if (key && decrypts_all(*key, disc.blocks)) return install(*key, what, label + name);
        }
        if (tried) log(std::to_string(tried) + " key file(s) in " + folder + " tested, none decrypts this disc");
        return std::nullopt;
    };

    // Tests one IRD file; on success 'save_as' (when given) keeps a copy in the IRD folder
    const auto test_ird = [&](const std::vector<byte>& bytes, const std::string& label, outcome what, const std::string& save_as) -> std::optional<result>
    {
        ird file;
        const ird_status status = parse_ird(bytes.data(), bytes.size(), file);
        if (status != ird_status::ok)
        {
            log(label + ": " + to_string(status));
            return std::nullopt;
        }
        const key128 key = derive_key(file.data1);
        const bool same_code = key_detail::iequals(file.product_code, disc.product_code) || key_detail::iequals(file.product_code, disc.header_id);
        const bool ok = same_code && decrypts_all(key, disc.blocks);
        log(label + ": v" + std::to_string(file.version) + " " + file.product_code + " " + file.game_version + "/" + file.app_version + ", " +
            std::to_string(file.file_count) + " files, CRC ok, sector 0 " + (!file.has_sector0 ? "unknown" : file.sector0 == disc.sector0 ? "equal" : "differs") +
            "; key " + check_value(key) + (!same_code ? " not tested (another product code)" : ok ? " verified OK" : " does not decrypt this disc"));
        if (!ok) return std::nullopt;
        if (!save_as.empty() && env.write_file && !env.write_file(save_as, bytes.data(), bytes.size())) log("could not keep a copy as " + save_as);
        return install(key, what, label);
    };

    // 2. Other key files on the console (a key pack imported with its own names)
    if (auto done = test_key_files(env.key_dir, outcome::local_key, env.key_dir)) return std::move(*done);

    // 2b. IRDs next to an ISO file ("<folder>/", "<folder>/ird/"): named after the image or the disc first; only an IRD of
    // this product code is tested; a copy of the one that fits is kept in the IRD folder
    for (const std::string& folder : env.image_ird_folders)
    {
        if (!env.list_files || stopped()) break;
        std::vector<std::string> names = env.list_files(folder);
        std::stable_partition(names.begin(), names.end(), [&](const std::string& n)
        {
            return (!env.image_name.empty() && key_detail::iequals(n, env.image_name + ".ird")) || name_mentions(n, disc.product_code) || name_mentions(n, disc.header_id);
        });
        std::size_t tried = 0;
        for (const std::string& name : names)
        {
            if (tried >= max_ird_files || stopped()) break;
            if (!key_detail::iends_with(name, ".ird")) continue;
            ++tried;
            if (!read_small(folder + name, ird_limit, data)) continue;
            const std::string code = peek_product_code(data.data(), data.size());
            if (!key_detail::iequals(code, disc.product_code) && !key_detail::iequals(code, disc.header_id)) continue;
            if (auto done = test_ird(data, folder + name, outcome::image_ird, env.ird_dir + cache_name("image/" + code + " " + name))) return std::move(*done);
        }
        if (tried) log(std::to_string(tried) + " IRD file(s) in " + folder + " looked at, none decrypts this disc");
    }

    // 3. IRDs downloaded earlier
    const std::vector<std::string> cached_names = env.list_files ? env.list_files(env.ird_dir) : std::vector<std::string>{};
    {
        std::size_t tried = 0;
        for (const std::string& name : cached_names)
        {
            if (tried >= max_ird_files || stopped()) break;
            if (!key_detail::iends_with(name, ".ird") || !name_mentions(name, disc.product_code)) continue;
            ++tried;
            if (!read_small(env.ird_dir + name, ird_limit, data)) continue;
            if (auto done = test_ird(data, env.ird_dir + name, outcome::cached_ird, {})) return std::move(*done);
        }
    }

    // 4. USB drives (read only): redump/ (.dkey, .key) and ird/ (.ird), folder names in any case. Files named after the
    // disc are tried before the download, the rest of those folders only when the database gave no key.
    const auto test_usb = [&](bool named_only) -> std::optional<result>
    {
        if (!env.list_folders || !env.list_files) return std::nullopt;
        for (const std::string& root : env.usb_roots)
        {
            for (const std::string& folder : env.list_folders(root))
            {
                const bool keys = key_detail::iequals(folder, "redump");
                if (!keys && !key_detail::iequals(folder, "ird")) continue;
                const std::string path = root + "/" + folder + "/";
                const std::string shown = "USB" + std::string(root.empty() ? "" : root.substr(root.size() - 1)) + ":/" + folder + "/";
                std::size_t tried = 0;
                for (const std::string& name : env.list_files(path))
                {
                    if (tried >= (keys ? max_key_files : max_ird_files) || stopped()) break;
                    const bool named = name_mentions(name, disc.product_code) || name_mentions(name, disc.header_id);
                    if (named != named_only) continue; // the second pass takes what the first one left
                    if (keys)
                    {
                        if (!key_detail::iends_with(name, ".dkey") && !key_detail::iends_with(name, ".key")) continue;
                        ++tried;
                        if (!read_small(path + name, key_file_limit, data)) continue;
                        const auto key = key_from_file(data.data(), data.size());
                        if (key && decrypts_all(*key, disc.blocks)) return install(*key, outcome::usb_key, shown + name);
                        continue;
                    }
                    if (!key_detail::iends_with(name, ".ird")) continue;
                    ++tried;
                    if (!read_small(path + name, ird_limit, data)) continue;
                    const std::string code = peek_product_code(data.data(), data.size());
                    if (!key_detail::iequals(code, disc.product_code) && !key_detail::iequals(code, disc.header_id)) continue;
                    if (auto done = test_ird(data, shown + name, outcome::usb_ird, env.ird_dir + cache_name("usb/" + code + " " + name))) return done;
                }
                if (tried) log(std::to_string(tried) + " file(s) in " + shown + " tested" + (named_only ? " (named after the disc)" : "") + ", none decrypts this disc");
            }
        }
        return std::nullopt;
    };
    if (auto done = test_usb(true)) return std::move(*done);
    if (stopped()) return finish(outcome::cancelled, outcome_text(outcome::cancelled), "before the download");

    // 5. The IRD database. Its failure is kept while the rest of the USB folders are searched.
    struct failure
    {
        outcome what = outcome::unavailable;
        std::string text;
        std::string detail;
    };
    failure pending{outcome::unavailable, outcome_text(outcome::unavailable), "no network access in this build"};
    const auto fail = [&pending](outcome what, std::string text, std::string why) -> std::optional<result>
    {
        pending = {what, std::move(text), std::move(why)};
        return std::nullopt;
    };
    const auto fetch_failure = [&](const fetch_reply& reply, const std::string& what_failed) -> std::optional<result>
    {
        const std::string why = what_failed + ": " + reply.log;
        switch (reply.status)
        {
        case fetch_reply::kind::offline: return fail(outcome::no_internet, outcome_text(outcome::no_internet), why);
        case fetch_reply::kind::http: return fail(outcome::server_error, outcome_text(outcome::server_error, {}, 0, reply.http), why);
        case fetch_reply::kind::cancelled: return fail(outcome::cancelled, outcome_text(outcome::cancelled), why);
        case fetch_reply::kind::unavailable: return fail(outcome::unavailable, outcome_text(outcome::unavailable), why);
        case fetch_reply::kind::too_large: return fail(outcome::server_error, outcome_text(outcome::server_error), why + " (too large)");
        case fetch_reply::kind::ok:
        case fetch_reply::kind::failed: break;
        }
        return fail(outcome::fetch_failed, outcome_text(outcome::fetch_failed, {}, 0, 0, reply.cause), why);
    };
    // A transfer that broke on the way or a server error is asked again (2 s, then 4 s later); no link, a refusal
    // (HTTP 4xx) or a stop is the answer at once
    const auto fetch = [&](const std::string& url, std::size_t limit, const std::string& what) -> fetch_reply
    {
        fetch_reply reply = env.fetch(url, limit);
        for (int attempt = 1; attempt < fetch_attempts && !stopped(); ++attempt)
        {
            const bool again = reply.status == fetch_reply::kind::failed || (reply.status == fetch_reply::kind::http && (reply.http >= 500 || reply.http == 429));
            if (!again) break;
            log(what + ": attempt " + std::to_string(attempt) + " of " + std::to_string(fetch_attempts) + " failed (" + reply.log + "); trying again");
            if (env.pause) env.pause(2000ll << (attempt - 1));
            if (stopped()) break;
            reply = env.fetch(url, limit);
        }
        return reply;
    };
    const auto database = [&]() -> std::optional<result>
    {
        if (!env.fetch) return std::nullopt;
        // A valid index is kept as all.json; when the download fails or is not the expected JSON, that copy answers (the
        // IRDs it names may still download)
        const std::string index_copy = env.ird_dir + "all.json";
        fetch_reply index = fetch(index_url, index_limit, "index");
        std::vector<index_entry> entries;
        std::size_t codes = 0;
        const bool fresh = index.status == fetch_reply::kind::ok && parse_index(index.body, disc.product_code, entries, &codes) == index_status::ok && codes;
        if (fresh)
        {
            if (env.write_file && !env.write_file(index_copy, index.body.data(), index.body.size())) log("index: could not keep a copy as " + index_copy);
        }
        else if (index.status != fetch_reply::kind::cancelled && read_small(index_copy, index_limit, data) && !data.empty())
        {
            std::string kept(data.begin(), data.end());
            std::vector<index_entry> kept_entries;
            std::size_t kept_codes = 0;
            if (parse_index(kept, disc.product_code, kept_entries, &kept_codes) == index_status::ok && kept_codes)
            {
                const std::string when = date_text(env.file_time ? env.file_time(index_copy) : -1);
                log("index: " + std::string(index.status == fetch_reply::kind::ok ? "not the expected JSON" : "download failed") + " (" + index.log + "): the copy from " +
                    when + " is used");
                index.status = fetch_reply::kind::ok;
                index.body = std::move(kept);
                index.log = "cached copy from " + when;
                entries = std::move(kept_entries);
                codes = kept_codes;
            }
        }
        if (index.status != fetch_reply::kind::ok) return fetch_failure(index, "index");
        if (!codes) return fail(outcome::server_error, outcome_text(outcome::server_error), "index is not the expected JSON (" + index.log + ")");
        if (entries.empty() && !disc.header_id.empty() && disc.header_id != disc.product_code) parse_index(index.body, disc.header_id, entries);
        index.body = {};
        log("index: " + std::to_string(codes) + " product codes, " + std::to_string(entries.size()) + " IRD(s) for " + disc.product_code + " (" + index.log + ")");
        if (entries.empty())
            return fail(outcome::not_in_database, outcome_text(outcome::not_in_database, disc.product_code), disc.product_code + " is not in the index");

        const std::vector<index_entry> candidates = rank_candidates(std::move(entries), disc);
        std::uint32_t downloaded = 0;
        fetch_reply last_failure;
        for (std::size_t i = 0; i < candidates.size(); ++i)
        {
            if (stopped()) return fail(outcome::cancelled, outcome_text(outcome::cancelled), "during the download");
            const index_entry& e = candidates[i];
            const std::string label = "IRD " + std::to_string(i + 1) + "/" + std::to_string(candidates.size()) + " '" + key_detail::loggable(e.link, 160) + "'";
            if (!safe_link(e.link))
            {
                log(label + ": link refused");
                continue;
            }
            const std::string name = cache_name(e.link);
            const std::string cached = env.ird_dir + name;
            if (std::find(cached_names.begin(), cached_names.end(), name) != cached_names.end())
            {
                if (name_mentions(name, disc.product_code)) log(label + ": already in " + cached + " (tested above)");
                else if (read_small(cached, ird_limit, data))
                    if (auto done = test_ird(data, cached, outcome::cached_ird, {})) return done;
                continue;
            }
            fetch_reply reply = fetch(file_url(e.link), ird_limit, label);
            if (reply.status != fetch_reply::kind::ok)
            {
                log(label + ": download failed (" + reply.log + ")");
                if (reply.status == fetch_reply::kind::offline || reply.status == fetch_reply::kind::cancelled) return fetch_failure(reply, label);
                last_failure = std::move(reply);
                continue;
            }
            ++downloaded;
            r.tested = downloaded;
            log(label + " (score " + std::to_string(match_score(e, disc)) + "): " + reply.log);
            const std::vector<byte> bytes(reply.body.begin(), reply.body.end());
            reply.body = {};
            if (auto done = test_ird(bytes, label, outcome::network, cached)) return done;
        }
        if (!downloaded && !last_failure.log.empty()) return fetch_failure(last_failure, "every IRD download");
        return fail(outcome::no_matching_key, outcome_text(outcome::no_matching_key, {}, downloaded),
            std::to_string(candidates.size()) + " IRD(s) listed, " + std::to_string(downloaded) + " downloaded, none decrypts " + r.title_id);
    };
    if (auto done = database()) return std::move(*done);

    // 6. The rest of the USB folders (key packs, IRDs named otherwise)
    if (pending.what != outcome::cancelled && !stopped())
        if (auto done = test_usb(false)) return std::move(*done);
    if (stopped()) pending = {outcome::cancelled, outcome_text(outcome::cancelled), "after the download"};
    return finish(pending.what, std::move(pending.text), pending.detail);
}
} // namespace utils::ps4::disc_key

#ifdef RPCS3_PS4

#include "Utilities/File.h"
#include "util/ps4_net.hpp"
#include "util/sysinfo.hpp"
#include "curl/curl/include/curl/curl.h"

namespace utils::ps4::disc_key
{
inline constexpr char ca_bundle[] = "/app0/certs/cacert.pem";
inline constexpr char data_root[] = "/data/rpcs3/";

namespace sys
{
// Cause of a curl result in the interface language
inline std::string curl_cause(CURLcode code)
{
    using i18n::str;
    switch (code)
    {
    case CURLE_COULDNT_RESOLVE_HOST: return i18n::tr(str::DKEY_SEM_DNS);
    case CURLE_COULDNT_CONNECT: return i18n::tr(str::DKEY_SEM_CONEXAO_SERVIDOR);
    case CURLE_OPERATION_TIMEDOUT: return i18n::tr(str::COM_TEMPO_ESGOTADO);
    case CURLE_SSL_CONNECT_ERROR: return i18n::tr(str::DKEY_FALHA_TLS_CONFIRA_DATA_HORA);
    case CURLE_PEER_FAILED_VERIFICATION: return i18n::tr(str::DKEY_CERTIFICADO_SERVIDOR_RECUSADO_CONFIRA_DATA);
    case CURLE_SEND_ERROR:
    case CURLE_RECV_ERROR: return i18n::tr(str::DKEY_CONEXAO_INTERROMPIDA);
    case CURLE_OUT_OF_MEMORY: return i18n::tr(str::DKEY_FALHA_INTERNA_CURL);
    default: return i18n::trf(str::DKEY_ERRO_CURL, {static_cast<int>(code)});
    }
}

struct transfer
{
    std::string body;
    std::size_t limit = 0;
    bool too_large = false;
    const std::function<bool()>* stopped = nullptr;
};

inline std::size_t on_data(char* data, std::size_t size, std::size_t count, void* user)
{
    auto& t = *static_cast<transfer*>(user);
    const std::size_t bytes = size * count;
    if (t.body.size() + bytes > t.limit)
    {
        t.too_large = true;
        return 0;
    }
    t.body.append(data, bytes);
    return bytes;
}

inline int on_progress(void* user, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    const auto& t = *static_cast<const transfer*>(user);
    return t.stopped && *t.stopped && (*t.stopped)() ? 1 : 0;
}

// HTTPS GET of 'url' (at most 'limit' bytes): own DNS lookup pinned with CURLOPT_RESOLVE, peer and host verified
// against the packaged root certificates, TLS 1.2 or newer, every step bounded in time.
inline fetch_reply https_get(const std::string& url, std::size_t limit, const std::function<bool()>& stopped)
{
    fetch_reply out;
    const std::size_t host_at = url.find("://");
    const std::size_t host_end = host_at == std::string::npos ? std::string::npos : url.find('/', host_at + 3);
    if (host_at == std::string::npos || host_end == std::string::npos || url.compare(0, host_at, "https") != 0)
    {
        out.log = "not an https URL";
        out.cause = i18n::tr(i18n::str::DKEY_ENDERECO_INVALIDO);
        return out;
    }
    const std::string host = url.substr(host_at + 3, host_end - host_at - 3);

    const int link = utils::ps4::net::link_state();
    if (link >= 0 && link != utils::ps4::net::netctl_state_ip_obtained)
    {
        out.status = fetch_reply::kind::offline;
        out.log = host + ": network link state " + std::to_string(link) + " (no IP address)";
        return out;
    }
    std::string certificates;
    {
        const fs::file file(ca_bundle);
        const u64 size = file ? file.size() : 0;
        if (size && size <= (4u << 20))
        {
            certificates.resize(static_cast<std::size_t>(size));
            if (file.read(certificates.data(), size) != size) certificates.clear();
        }
    }
    if (certificates.find("-----BEGIN CERTIFICATE-----") == std::string::npos)
    {
        out.status = fetch_reply::kind::unavailable;
        out.log = std::string("no CA bundle at ") + ca_bundle;
        return out;
    }
    std::string how;
    const u32 address = utils::ps4::net::resolve_ipv4(host, how);
    if (!address)
    {
        out.status = fetch_reply::kind::offline;
        out.log = host + ": DNS failed (" + how + ")";
        return out;
    }
    std::array<u8, 4> ip{};
    std::memcpy(ip.data(), &address, sizeof(address));
    const std::string resolve_line = host + ":443:" + std::to_string(ip[0]) + "." + std::to_string(ip[1]) + "." + std::to_string(ip[2]) + "." + std::to_string(ip[3]);

    CURL* curl = curl_easy_init();
    if (!curl)
    {
        out.log = "curl_easy_init failed";
        out.cause = i18n::tr(i18n::str::DKEY_FALHA_INICIAR_CURL);
        return out;
    }
    curl_slist* resolve = curl_slist_append(nullptr, resolve_line.c_str());
    transfer t;
    t.limit = limit;
    t.stopped = &stopped;
    std::array<char, CURL_ERROR_SIZE> error{};
    curl_blob blob{certificates.data(), certificates.size(), CURL_BLOB_COPY};
    const std::string agent = utils::get_user_agent();
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_RESOLVE, resolve);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https");
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 2L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_SSLVERSION, static_cast<long>(CURL_SSLVERSION_TLSv1_2));
    curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &blob);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 512L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 240L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, agent.c_str());
    curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error.data());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &on_data);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &t);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &on_progress);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &t);
    const std::int64_t started = disc_stream::detail::now_ms();
    const CURLcode code = curl_easy_perform(curl);
    long http = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http);
    curl_slist_free_all(resolve);
    curl_easy_cleanup(curl);

    out.http = http;
    out.log = resolve_line + " via " + how + ", HTTP " + std::to_string(http) + ", " + std::to_string(t.body.size()) + " bytes in " +
        std::to_string(disc_stream::detail::now_ms() - started) + " ms";
    if (code == CURLE_ABORTED_BY_CALLBACK)
    {
        out.status = fetch_reply::kind::cancelled;
        return out;
    }
    if (t.too_large)
    {
        out.status = fetch_reply::kind::too_large;
        out.log += " (larger than " + std::to_string(limit) + " bytes)";
        return out;
    }
    if (code != CURLE_OK)
    {
        out.log += "; curl " + std::to_string(static_cast<int>(code)) + " '" + error.data() + "' [" + curl_easy_strerror(code) + "]";
        out.cause = curl_cause(code);
        const bool no_link = code == CURLE_COULDNT_RESOLVE_HOST || code == CURLE_COULDNT_CONNECT || (code == CURLE_OPERATION_TIMEDOUT && !http && t.body.empty());
        out.status = no_link ? fetch_reply::kind::offline : fetch_reply::kind::failed;
        return out;
    }
    if (http != 200)
    {
        out.status = fetch_reply::kind::http;
        return out;
    }
    out.status = fetch_reply::kind::ok;
    out.body = std::move(t.body);
    return out;
}

// Writes go only below /data/rpcs3
inline bool writable(const std::string& path)
{
    return path.starts_with(data_root) && path.find("/../") == std::string::npos;
}

inline environment console(std::shared_ptr<std::atomic<bool>> stop)
{
    environment env;
    env.read_disc = [](std::uint64_t offset, void* buffer, std::size_t size)
    {
        disc_stream::read_status status = disc_stream::read_status::ok;
        return disc_stream::service::instance().disc().read(offset, buffer, size, &status) == size;
    };
    env.stopped = [stop] { return stop->load(std::memory_order_relaxed); };
    env.pause = [stop](std::int64_t ms)
    {
        const std::int64_t end = disc_stream::detail::now_ms() + ms;
        while (!stop->load(std::memory_order_relaxed) && disc_stream::detail::now_ms() < end) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    };
    const auto stopped = env.stopped;
    env.fetch = [stopped](const std::string& url, std::size_t limit) { return https_get(url, limit, stopped); };
    env.read_file = [](const std::string& path, std::size_t limit, std::vector<byte>& out)
    {
        const fs::file file(path);
        if (!file) return false;
        const u64 size = file.size();
        if (size > limit) return false;
        out.resize(static_cast<std::size_t>(size));
        return size == 0 || file.read(out.data(), size) == size;
    };
    env.write_file = [](const std::string& path, const void* data, std::size_t size)
    {
        if (!writable(path)) return false;
        const std::size_t slash = path.rfind('/');
        if (slash == std::string::npos || !fs::create_path(path.substr(0, slash))) return false;
        fs::pending_file file(path);
        if (!file.file || (size && file.file.write(data, size) != size)) return false;
        return file.commit();
    };
    env.rename_file = [](const std::string& from, const std::string& to) { return writable(from) && writable(to) && fs::rename(from, to, true); };
    const auto list = [](const std::string& folder, bool folders)
    {
        std::vector<std::string> names;
        std::size_t seen = 0;
        if (fs::dir dir(folder); dir)
            for (const auto& entry : dir)
            {
                if (++seen > 65536) break;
                if (entry.name == "." || entry.name == ".." || entry.is_symlink || entry.is_directory != folders) continue;
                names.push_back(entry.name);
            }
        return names;
    };
    env.list_files = [list](const std::string& folder) { return list(folder, false); };
    env.list_folders = [list](const std::string& folder) { return list(folder, true); };
    env.file_time = [](const std::string& path) -> std::int64_t
    {
        fs::stat_t info{};
        return fs::get_stat(path, info) && !info.is_directory ? static_cast<std::int64_t>(info.mtime) : -1;
    };
    env.log = [](const std::string& line) { ps4_startup_checkpoint(line.c_str()); };
    env.key_dir = rpcs3::utils::get_redump_key_dir();
    if (env.key_dir.empty() || env.key_dir.back() != '/') env.key_dir += '/';
    constexpr std::string_view redump = "redump/";
    env.ird_dir = env.key_dir.ends_with(redump) ? env.key_dir.substr(0, env.key_dir.size() - redump.size()) + "ird/" : std::string("/data/rpcs3/data/ird/");
    for (unsigned i = 0; i < 8; ++i)
        if (const std::string root = "/mnt/usb" + std::to_string(i); fs::is_dir(root)) env.usb_roots.push_back(root);
    return env;
}
}

// The search on the caller's thread (never the XMB's), e.g. as a key source of the disc install: reads the disc
// through 'read' (the shared disc reader when empty) and returns the verified key, installed as <TITLE_ID>.dkey when
// possible; 'source' names where it came from. nullopt when no key decrypts the disc or none is needed.
inline std::optional<key128> find_key(const read_fn& read, std::string& source, std::shared_ptr<std::atomic<bool>> stop = {})
{
    environment env = sys::console(stop ? std::move(stop) : std::make_shared<std::atomic<bool>>(false));
    if (read) env.read_disc = read;
    result r = obtain(env);
    if (!r.key) return std::nullopt;
    source = r.source;
    if (r.changed) disc_stream::service::instance().request_check();
    return r.key;
}

// The key of an encrypted ISO file that has none (as the drive's search, on the file's own sectors), on the caller's
// thread (never the XMB's). Tested first: the key files the ISO loader looks up by the image's name (next to the image
// and in the redump folder); then the redump folder, the IRDs next to the image ("<stem>.ird", any IRD of this disc in
// its folder and in "<folder>/ird/"), the IRDs downloaded earlier, USB and the IRD database. A verified key is written
// as <redump>/<header id>.dkey and as <redump>/<image name>.dkey, the name the loader looks up first. Log lines start
// with "disc key: ISO '<path>': ".
inline result find_key_for_image(const std::string& path, std::shared_ptr<std::atomic<bool>> stop = {})
{
    environment env = sys::console(stop ? std::move(stop) : std::make_shared<std::atomic<bool>>(false));
    auto image = std::make_shared<fs::file>(path);
    env.read_disc = [image](std::uint64_t offset, void* buffer, std::size_t size)
    {
        return *image && image->read_at(offset, buffer, size) == size;
    };
    // The names ISO.cpp check_type() builds: the path without its extension, and the file name of that
    const std::string name_path = path.substr(0, std::min(path.size(), path.rfind('.')));
    const std::size_t slash = name_path.rfind('/');
    const std::string folder = slash == std::string::npos ? std::string("./") : path.substr(0, slash + 1);
    env.image_name = name_path.substr(slash == std::string::npos ? 0 : slash + 1);
    env.image_key_files = {name_path + ".dkey", name_path + ".key", env.key_dir + env.image_name + ".dkey", env.key_dir + env.image_name + ".key"};
    env.image_ird_folders = {folder, folder + "ird/"};
    const std::string prefix = "disc key: ISO '" + path + "': ";
    env.log = [prefix](const std::string& line)
    {
        constexpr std::string_view head = "disc key: ";
        const std::string_view text = std::string_view(line).starts_with(head) ? std::string_view(line).substr(head.size()) : std::string_view(line);
        ps4_startup_checkpoint((prefix + std::string(text)).c_str());
    };
    return obtain(env);
}

// The search for an ISO file on a thread of its own (find_key_for_image), for the XMB's "Buscando a chave" dialog
struct image_search
{
    std::shared_ptr<std::atomic<bool>> stop = std::make_shared<std::atomic<bool>>(false);
    std::atomic<bool> finished{false};
    result outcome_of; // written before 'finished'
};

// Null when no thread could be made
inline std::shared_ptr<image_search> start_image_search(const std::string& path)
{
    auto job = std::make_shared<image_search>();
    if (!disc_stream::run_detached([job, path]
        {
            const std::int64_t started = disc_stream::detail::now_ms();
            job->outcome_of = find_key_for_image(path, job->stop);
            ps4_startup_checkpoint(("disc key: ISO '" + path + "': search took " + std::to_string(disc_stream::detail::now_ms() - started) + " ms").c_str());
            job->finished.store(true, std::memory_order_release);
        }))
        return nullptr;
    return job;
}

// One search at a time for the whole app, on a worker thread. The XMB calls request() whenever it sees the drive's
// status (never waits) and take() to get the finished result once.
class service
{
public:
    static service& instance()
    {
        // Never destroyed: the worker may still hold it while the process ends
        static service* const s = new service();
        return *s;
    }

    // Starts the search for a PS3 disc not handled yet (a new drive generation). False when nothing was started.
    bool request(const disc_stream::disc_status& status)
    {
        if (status.state != disc_stream::disc_state::ps3_disc) return false;
        std::lock_guard lock(m_mutex);
        if (m_running) return false;
        if (m_handled && m_generation == status.generation)
        {
            // The same disc again: a search that failed for the network is repeated, later and later; one that did not
            // find the disc in the database once a day (the database grows)
            if (!m_retry_at || disc_stream::detail::now_ms() < m_retry_at) return false;
            m_automatic = true;
            ps4_startup_checkpoint(("disc key: searching again for " + disc_stream::canonical_title_id(status.title_id) +
                (m_last_what == outcome::not_in_database ? " (it was not in the database a day ago)" : " (the last search failed for the network)")).c_str());
        }
        else
        {
            m_automatic = false;
            m_backoff = 0;
            m_last_known = false;
        }
        m_retry_at = 0;
        m_handled = true;
        m_generation = status.generation;
        m_running = true;
        m_title_id = disc_stream::canonical_title_id(status.title_id);
        m_stop = std::make_shared<std::atomic<bool>>(false);
        const std::shared_ptr<std::atomic<bool>> stop = m_stop;
        if (disc_stream::run_detached([this, stop] { run(stop); })) return true;
        m_running = false;
        m_handled = false;
        ps4_startup_checkpoint("disc key: no worker thread could be started; search skipped");
        return false;
    }

    // Lets the next request() search again for the same disc (e.g. the player asked to retry)
    void forget()
    {
        std::lock_guard lock(m_mutex);
        if (!m_running)
        {
            m_handled = false;
            m_retry_at = 0;
        }
    }

    bool busy() const
    {
        std::lock_guard lock(m_mutex);
        return m_running;
    }

    std::string busy_text() const
    {
        std::lock_guard lock(m_mutex);
        return m_running ? i18n::trf(i18n::str::DKEY_BUSCANDO_CHAVE_DISCO, {m_title_id}) : std::string{};
    }

    std::optional<result> take()
    {
        std::lock_guard lock(m_mutex);
        std::optional<result> out = std::move(m_finished);
        m_finished.reset();
        return out;
    }

    // Ends a running search at its next step (downloads are interrupted within about a second)
    void cancel()
    {
        std::lock_guard lock(m_mutex);
        if (m_stop) m_stop->store(true, std::memory_order_relaxed);
    }

private:
    mutable std::mutex m_mutex;
    bool m_running = false;
    bool m_handled = false;
    std::uint32_t m_generation = 0;
    std::string m_title_id;
    std::shared_ptr<std::atomic<bool>> m_stop;
    std::optional<result> m_finished;
    std::int64_t m_retry_at = 0;  // steady ms after which the same disc is searched again, 0: never
    std::int64_t m_backoff = 0;   // the wait before that retry (1 min, doubled up to 10 min)
    bool m_automatic = false;     // the running search is such a retry
    bool m_last_known = false;
    outcome m_last_what = outcome::disc_unreadable;

    service() = default;

    void run(const std::shared_ptr<std::atomic<bool>>& stop)
    {
        const std::int64_t started = disc_stream::detail::now_ms();
        result r = obtain(sys::console(stop));
        // No transfer of the shared reader stays parked on the drive (an install may open its own next); a check that
        // holds the reader right now ends them itself
        disc_stream::service::instance().disc().end_streams();
        ps4_startup_checkpoint(("disc key: search took " + std::to_string(disc_stream::detail::now_ms() - started) + " ms").c_str());
        if (r.changed) disc_stream::service::instance().request_check(); // the drive status picks the new key up
        std::lock_guard lock(m_mutex);
        if (retry_later(r.what))
        {
            m_backoff = next_backoff(m_backoff);
            m_retry_at = disc_stream::detail::now_ms() + m_backoff;
        }
        else if (r.what == outcome::not_in_database) m_retry_at = disc_stream::detail::now_ms() + 24ll * 3600 * 1000;
        else m_retry_at = 0;
        // A retry that ends the way the last search did tells the player nothing new
        const bool quiet = m_automatic && m_last_known && r.what == m_last_what;
        m_last_what = r.what;
        m_last_known = true;
        if (!quiet) m_finished = std::move(r);
        m_running = false;
    }
};
} // namespace utils::ps4::disc_key

#endif // RPCS3_PS4
