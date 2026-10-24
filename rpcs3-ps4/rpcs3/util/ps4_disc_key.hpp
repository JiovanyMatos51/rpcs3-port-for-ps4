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
// A disc must also work without internet once its key was known on this console or the player supplies it
// (environment::offline_keys, the default; off: the search above, unchanged):
//   - every verified key is also kept as <TITLE_ID>-<identity>.dkey (identity: FNV-1a of sectors 0-1, one per
//     pressing), so another pressing of the same title id never pushes it out; those copies, the copies of the title
//     id's other pressings and the files a wrong key was moved aside to (*.bad) are searched before anything else;
//   - the player's own files: /data/rpcs3/keys (one level of subfolders) and USB:/keys next to USB:/redump and
//     USB:/ird, decided by content: key files, IRDs of this disc (product code read from the file) and key lists
//     (scan_key_list); every value is tried as the key and as data1;
//   - the network is asked only when it can answer (network_usable: an IP address and a plausible clock); otherwise
//     the rest of USB is searched and the result says why nothing was downloaded (no_internet, clock_wrong).
// The player's files are only read; the search writes below /data/rpcs3 only (may_write), never into the inbox.
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
inline constexpr char inbox_dir[] = "/data/rpcs3/keys/";    // the player's own key files (FTP); only read
inline constexpr std::size_t max_inbox_key_files = 16384;   // per folder of the inbox and of USB:/keys
inline constexpr std::size_t list_limit = 8u << 20;         // bytes of one key list
inline constexpr std::size_t max_list_files = 64;           // key lists read by one search
inline constexpr std::size_t max_list_values = 65536;       // values taken from key lists by one search
inline constexpr std::size_t max_subfolders = 64;           // subfolders of the inbox / USB:/keys walked
inline constexpr std::size_t max_pressing_copies = 64;      // copies of other pressings of one title id tested
inline constexpr std::size_t ird_peek_bytes = 4096;         // read to learn an IRD's product code

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
    return disc_stream::detail::utc_text(unix_seconds);
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

// The search writes only below /data/rpcs3, and never into the player's key inbox: USB and the inbox are only read
inline bool may_write(std::string_view path) noexcept
{
    return path.starts_with("/data/rpcs3/") && path.find("/../") == std::string_view::npos && path.find("/./") == std::string_view::npos &&
        !path.starts_with(inbox_dir);
}

// "BLES01876-0123456789abcdef.dkey": the copy of a verified key kept for one pressing (identity: FNV-1a of its sectors
// 0-1). The ISO loader looks keys up by exact names, so these copies never stand in its way.
inline std::string pressing_file_name(std::string_view title_id, std::uint64_t identity)
{
    char hex[24];
    std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(identity));
    return std::string(title_id) + "-" + hex + ".dkey";
}

// A pressing copy of 'title_id' (any pressing)
inline bool is_pressing_file(std::string_view name, std::string_view title_id) noexcept
{
    if (title_id.empty() || name.size() != title_id.size() + 1 + 16 + 5 || !key_detail::iequals(name.substr(0, title_id.size()), title_id) ||
        name[title_id.size()] != '-' || !key_detail::iends_with(name, ".dkey"))
        return false;
    for (const char c : name.substr(title_id.size() + 1, 16))
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    return true;
}

// What a value of the player's (a key file, a list) stands for: the disc key itself, or data1 the key derives from
struct value_match
{
    key128 key{};
    bool data1 = false;
};

// ---------------------------------------------------------------------------------------------------------------
// The network, offline

// The boot TLS self-test (Emu/NP/ps4_tls_selftest.h): clock_refused when it found no root certificate valid at the
// console's date, the signature of a wrong clock
enum class tls_state
{
    unknown, // not finished, not run
    ok,
    failed,
    clock_refused,
};

enum class net_state
{
    usable,
    no_link,     // no IP address
    clock_wrong, // no certificate can be checked at the console's date
};

inline const char* to_string(net_state state)
{
    switch (state)
    {
    case net_state::usable: return "usable";
    case net_state::no_link: return "offline";
    case net_state::clock_wrong: return "clock wrong";
    }
    return "?";
}

// Whether a download can work before trying one: an IP address (or a NetCtl state that is unknown, which the download
// then decides) and a clock no earlier than the day before 'build_date' ("YYYY-MM-DD"). A negative 'now' is unknown.
inline net_state network_usable(int link, std::int64_t now, std::string_view build_date, tls_state tls) noexcept
{
    if (!disc_stream::clock_plausible(now, build_date) || tls == tls_state::clock_refused) return net_state::clock_wrong;
    if (link >= 0 && link != disc_stream::netctl_ip_obtained) return net_state::no_link;
    return net_state::usable;
}

// The boot TLS self-test's answer (ps4_tls_selftest_ok, ps4_tls_selftest_line): a failure whose CA bundle kept no root
// because of the roots' dates ("148 roots in 31 ms (0 accepted, 148 not yet valid)") is the console clock
inline tls_state tls_from_selftest(std::optional<bool> ok, std::string_view line) noexcept
{
    if (!ok) return tls_state::unknown;
    if (*ok) return tls_state::ok;
    const bool dated = line.find(" not yet valid") != std::string_view::npos || line.find(" expired") != std::string_view::npos;
    return dated && line.find("(0 accepted") != std::string_view::npos ? tls_state::clock_refused : tls_state::failed;
}

// ---------------------------------------------------------------------------------------------------------------
// Key lists

// One candidate value of a key list
struct list_value
{
    key128 value{};
    std::size_t line = 0; // 1-based
    bool named = false;   // its line names the disc
};

struct list_stats
{
    std::size_t lines = 0;   // lines of the text (comments included)
    std::size_t values = 0;  // 32-hex values found
    std::size_t named = 0;   // of those, on lines naming the disc
    std::size_t offered = 0; // values handed to the test: at most the bound
    bool truncated = false;  // the bound left values out
    bool accepted = false;   // the test took a value
};

namespace list_detail
{
inline bool hex_digit(char c) noexcept
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

inline bool word_char(char c) noexcept
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

// The line names 'id' (canonical "BLES01876"), written "BLES01876", "bles-01876", "BLES_01876" or "BLES 01876", not
// inside a longer word or number
inline bool names(std::string_view line, std::string_view id) noexcept
{
    if (id.size() != 9 || line.size() < 9) return false;
    for (std::size_t i = 0; i + 9 <= line.size(); ++i)
    {
        if (i > 0 && word_char(line[i - 1])) continue;
        bool same = true;
        for (std::size_t k = 0; k < 4 && same; ++k) same = key_detail::upper(line[i + k]) == id[k];
        if (!same) continue;
        std::size_t at = i + 4;
        if (line[at] == '-' || line[at] == '_' || line[at] == ' ') ++at;
        if (at + 5 > line.size()) continue;
        for (std::size_t k = 0; k < 5 && same; ++k) same = line[at + k] == id[4 + k];
        if (same && (at + 5 == line.size() || !(line[at + 5] >= '0' && line[at + 5] <= '9'))) return true;
    }
    return false;
}

// "#", ";" or "//" after optional blanks
inline bool comment(std::string_view line) noexcept
{
    std::size_t at = 0;
    while (at < line.size() && (line[at] == ' ' || line[at] == '\t')) ++at;
    const std::string_view rest = line.substr(at);
    return rest.starts_with('#') || rest.starts_with(';') || rest.starts_with("//");
}
}

// The candidate values of a key list in any reasonable layout ("BLES01876 3F2A...", "BLES-01876;FIFA 14;3F2A...",
// "\"FIFA 14 (Europe)\",\"3F2A...\"", "FIFA 14.dkey = 3F2A...", a bare column, GetKey "data1:" lines): every run of
// exactly 32 hexadecimal characters (any other character separates; a longer run, e.g. a 64-hex hash, is no value).
// UTF-8 or ASCII, BOM skipped, LF or CRLF, comment lines skipped. Lines naming the disc's product code or header id
// come first, the others after them. 'test' gets each value (at most 'max_values'); true stops the scan.
template <typename Test>
inline list_stats scan_key_list(std::string_view text, std::string_view product_code, std::string_view header_id, std::size_t max_values, Test&& test)
{
    list_stats out;
    if (text.starts_with("\xEF\xBB\xBF")) text.remove_prefix(3);
    for (int pass = 0; pass < 2 && !out.accepted; ++pass)
    {
        std::string_view rest = text;
        std::size_t number = 0;
        while (!rest.empty() && !out.accepted)
        {
            const std::size_t eol = rest.find('\n');
            std::string_view line = rest.substr(0, eol);
            rest.remove_prefix(eol == std::string_view::npos ? rest.size() : eol + 1);
            ++number;
            if (pass == 0) ++out.lines;
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            if (line.empty() || list_detail::comment(line)) continue;
            const bool named = list_detail::names(line, product_code) || (header_id != product_code && list_detail::names(line, header_id));
            if (named != (pass == 0)) continue;
            for (std::size_t i = 0; i < line.size() && !out.accepted;)
            {
                if (!list_detail::hex_digit(line[i]))
                {
                    ++i;
                    continue;
                }
                std::size_t end = i;
                while (end < line.size() && list_detail::hex_digit(line[end])) ++end;
                if (end - i == 32)
                {
                    ++out.values;
                    out.named += named;
                    if (out.offered >= max_values) out.truncated = true;
                    else
                    {
                        ++out.offered;
                        list_value v;
                        if (const auto key = key_from_file(reinterpret_cast<const byte*>(line.data() + i), 32)) v.value = *key;
                        v.line = number;
                        v.named = named;
                        if (test(static_cast<const list_value&>(v))) out.accepted = true;
                    }
                }
                i = end;
            }
        }
    }
    return out;
}

// How the search reads a file of the player's by its name: key files, IRDs, key lists (text, or no extension); hidden
// files and anything else are left alone. The content decides in the end (a list name holding one key is a key file).
enum class user_file
{
    key,
    ird,
    list,
    other,
};

inline user_file user_file_type(std::string_view name) noexcept
{
    using key_detail::iends_with;
    if (name.empty() || name.front() == '.') return user_file::other;
    if (iends_with(name, ".dkey") || iends_with(name, ".key")) return user_file::key;
    if (iends_with(name, ".ird")) return user_file::ird;
    for (const std::string_view extension : {".txt", ".csv", ".lst", ".dat", ".keys"})
        if (iends_with(name, extension)) return user_file::list;
    return name.find('.') == std::string_view::npos ? user_file::list : user_file::other;
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
    std::uint64_t identity = 0; // FNV-1a of sectors 0-1, as disc_stream::disc_status::identity: tells pressings apart
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
    out.identity = ds::detail::fnv1a64(head.data(), head.size());

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

// Every success comes before 'network', every failure after it (succeeded())
enum class outcome
{
    not_needed,      // the disc's files read in the clear
    already_present, // <TITLE_ID>.dkey (or .key) decrypts the disc
    cache_pressing,  // a copy this console kept for this pressing (or for another pressing of the title id)
    local_key,       // another key file of the redump folder decrypts it; copied as <TITLE_ID>.dkey
    cached_ird,      // an IRD downloaded earlier
    inbox_key,       // a file of /data/rpcs3/keys (key file, IRD or key list)
    usb_key,         // USB:/redump
    usb_ird,         // USB:/ird
    usb_list,        // a key list on USB, or any file of USB:/keys
    image_ird,       // an IRD next to the ISO file
    network,         // the IRD database
    no_internet,
    clock_wrong,     // the console clock is wrong: no download can verify the server
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

inline constexpr outcome last_outcome = outcome::cancelled;
static_assert(outcome::cache_pressing < outcome::network && outcome::inbox_key < outcome::network && outcome::usb_list < outcome::network &&
        outcome::network < outcome::no_internet && outcome::network < outcome::clock_wrong,
    "succeeded() counts every outcome up to 'network' as a success: successes before it, failures after it");

inline const char* to_string(outcome what)
{
    switch (what)
    {
    case outcome::not_needed: return "not needed";
    case outcome::already_present: return "already present";
    case outcome::cache_pressing: return "kept for this pressing";
    case outcome::local_key: return "redump folder";
    case outcome::cached_ird: return "cached IRD";
    case outcome::inbox_key: return "keys inbox";
    case outcome::usb_key: return "USB key";
    case outcome::usb_ird: return "USB IRD";
    case outcome::usb_list: return "USB keys or key list";
    case outcome::image_ird: return "IRD next to the ISO";
    case outcome::network: return "IRD database";
    case outcome::no_internet: return "no internet";
    case outcome::clock_wrong: return "console clock wrong";
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
// drive (the console may get its connection after the disc went in, or its clock set right); every other answer stands
// until the disc changes
inline bool retry_later(outcome what) noexcept
{
    return what == outcome::no_internet || what == outcome::server_error || what == outcome::fetch_failed || what == outcome::clock_wrong;
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
    int curl = 0;      // the curl code of a failed transfer (60, 77: a certificate check), 0 when none
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
    // Offline: the per-pressing copies, the player's files (inbox, USB:/keys, key lists, data1 values) and the network
    // asked only when usable. False: the search as 0.39 made it (the disc_keys_offline feature row).
    bool offline_keys = true;
    std::string inbox_dir;                          // inbox_dir on the console; "" leaves the inbox out
    std::function<int()> link_state;                // NetCtl state now (3: an IP address), -1 unknown; empty: not asked
    std::function<std::int64_t()> clock;            // the console clock (unix seconds); empty: not judged
    std::string build_date;                         // "YYYY-MM-DD" of this build, for the clock
    std::function<tls_state()> tls;                 // the boot TLS self-test; empty: unknown
    std::function<bool(const std::string& folder)> reopen; // opens a folder that refused a write (fs repair); empty: none
    // The first 'bytes' of a file (fewer when it is shorter); empty: the whole file is read through read_file
    std::function<bool(const std::string& path, std::size_t bytes, std::vector<byte>& out)> read_head;
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
    std::uint32_t local_tested = 0; // files of this console and of USB tested (key files, IRDs, key lists)
    std::string summary;            // the "disc key: sources tested: ..." line ("" when no source was searched)
};

// What one search looked at, for its summary line
struct source_tally
{
    bool started = false;
    std::uint32_t cache = 0, redump = 0, inbox = 0, usb = 0, ird = 0, lists = 0;
    std::uint64_t list_values = 0;
    std::uint32_t requests = 0;            // downloads asked for
    std::string network = "not needed";

    std::uint32_t local() const noexcept { return cache + redump + inbox + usb + ird; }

    // "cache 1, redump 12, inbox 0, usb 3, lists 1 (214 values), ird 0, network skipped (offline: netctl state 1)"
    std::string text() const
    {
        return "cache " + std::to_string(cache) + ", redump " + std::to_string(redump) + ", inbox " + std::to_string(inbox) + ", usb " + std::to_string(usb) +
            ", lists " + std::to_string(lists) + " (" + std::to_string(list_values) + " values), ird " + std::to_string(ird) + ", network " + network;
    }
};

// Byte limit of a result line in the interface language: text_limit, twice that in Arabic (two bytes per letter; the
// XMB fits the line to the screen)
inline std::size_t line_limit() noexcept { return i18n::rtl() ? 2 * text_limit : text_limit; }

// "Chave do disco encontrada em <source> e guardada neste PS4" within line_limit(): a long path keeps its end (the file
// name), "..." marking the cut
inline std::string found_in_text(std::string_view source)
{
    using i18n::str;
    if (source.empty()) source = "?";
    // The frame around the path, with the marks an argument gets in Arabic
    const std::size_t frame = i18n::trf(str::DKEY_CHAVE_ENCONTRADA_EM, {std::string_view("x")}).size() - 1;
    const std::size_t budget = line_limit() > frame + 8 ? line_limit() - frame : 8;
    std::string shown(source);
    if (shown.size() > budget)
    {
        std::size_t from = shown.size() - (budget - 3);
        while (from < shown.size() && (static_cast<unsigned char>(shown[from]) & 0xc0) == 0x80) ++from;
        shown = "..." + shown.substr(from);
    }
    return i18n::trf(str::DKEY_CHAVE_ENCONTRADA_EM, {shown});
}

// Text of an outcome in the interface language. 'title_id', 'count', 'http', 'cause' and 'source' (the file of the
// offline sources) fill the texts that name them. 'count' with no_internet: the files tested locally.
inline std::string outcome_text(outcome what, std::string_view title_id = {}, std::uint32_t count = 0, long http = 0, std::string_view cause = {},
    std::string_view source = {})
{
    using i18n::str;
    std::string text;
    switch (what)
    {
    case outcome::not_needed: text = i18n::tr(str::DKEY_ESTE_DISCO_NAO_PRECISA_CHAVE); break;
    case outcome::already_present: text = i18n::tr(str::DKEY_CHAVE_DISCO_JA_ESTA_CONSOLE); break;
    case outcome::cache_pressing: text = i18n::tr(str::DKEY_CHAVE_GUARDADA_PRENSAGEM); break;
    case outcome::local_key: text = i18n::tr(str::DKEY_CHAVE_DISCO_ENCONTRADA_DATA_RPCS3); break;
    case outcome::cached_ird: text = i18n::tr(str::DKEY_CHAVE_DISCO_OBTIDA_AUTOMATICAMENTE_IRD); break;
    case outcome::inbox_key:
    case outcome::usb_list: text = found_in_text(source); break;
    case outcome::usb_key: text = i18n::tr(str::DKEY_CHAVE_DISCO_IMPORTADA_USB_REDUMP); break;
    case outcome::usb_ird: text = i18n::tr(str::DKEY_CHAVE_DISCO_OBTIDA_IRD); break;
    case outcome::image_ird: text = i18n::tr(str::DKEY_CHAVE_OBTIDA_IRD_LADO_ISO); break;
    case outcome::network: text = i18n::tr(str::DKEY_CHAVE_DISCO_OBTIDA_AUTOMATICAMENTE); break;
    case outcome::no_internet: text = count ? i18n::trf(str::DKEY_SEM_INTERNET_TESTADOS, {count}) : std::string(i18n::tr(str::DKEY_SEM_INTERNET_NAO_FOI_POSSIVEL)); break;
    case outcome::clock_wrong: text = i18n::tr(str::DKEY_RELOGIO_ERRADO); break;
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
    std::vector<std::string> copies_tested; // pressing copies tested before the redump folder's scan
    source_tally count;
    const bool offline_on = env.offline_keys;
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
        if (count.started)
        {
            r.local_tested = count.local();
            r.summary = "disc key: sources tested: " + count.text();
            if (env.log) env.log(r.summary);
        }
        r.line = "disc key: result: " + r.text + " | " + std::string(to_string(what)) + (detail.empty() ? std::string{} : "; " + detail);
        if (env.log) env.log(r.line);
        return std::move(r);
    };
    const auto read_small = [&env](const std::string& path, std::size_t limit, std::vector<byte>& data)
    {
        data.clear();
        return env.read_file && env.read_file(path, limit, data);
    };
    // The first 'bytes' of a file; without read_head the whole file up to 'limit'
    const auto read_head = [&env, &read_small](const std::string& path, std::size_t bytes, std::size_t limit, std::vector<byte>& data)
    {
        data.clear();
        if (env.read_head) return env.read_head(path, bytes, data);
        return read_small(path, limit, data);
    };
    const auto write = [&env](const std::string& path, const void* bytes, std::size_t size)
    {
        return may_write(path) && env.write_file && env.write_file(path, bytes, size);
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
    count.started = true;

    const std::string stem = env.key_dir + r.title_id;
    r.key_path = stem + ".dkey";
    // The copy kept for this pressing (offline): never renamed or removed by the app
    const std::string pressing_path = offline_on && disc.identity ? env.key_dir + pressing_file_name(r.title_id, disc.identity) : std::string{};
    std::vector<byte> data;

    // A value as the disc key or, offline, as data1 the key derives from (people often hold data1: IRDs, GetKey dumps)
    const auto test_value = [&](const key128& value) -> std::optional<value_match>
    {
        if (decrypts_all(value, disc.blocks)) return value_match{value, false};
        if (offline_on)
            if (const key128 derived = derive_key(value); decrypts_all(derived, disc.blocks)) return value_match{derived, true};
        return std::nullopt;
    };

    // Writes 'hex' as 'path' and reads it back. Offline, a folder that refuses it is opened once (the permission repair
    // of the frontend) and the write tried again; the key stays usable from memory either way.
    const auto write_key = [&](const std::string& path, const std::string& hex) -> bool
    {
        std::vector<byte> back;
        const auto attempt = [&] { return write(path, hex.data(), hex.size()) && read_small(path, key_file_limit, back) && std::string(back.begin(), back.end()) == hex; };
        if (attempt()) return true;
        if (!offline_on || !env.reopen || !may_write(path)) return false;
        const std::string folder = path.substr(0, path.rfind('/'));
        const bool opened = env.reopen(folder);
        log("could not write " + path + (opened ? ": " + folder + " opened again, writing once more" : ": " + folder + " could not be opened"));
        return opened && attempt();
    };

    // The copy of a verified key for this pressing, written when it is missing or holds another value; its path when written
    const auto keep_pressing_copy = [&](const std::string& hex) -> std::string
    {
        if (pressing_path.empty() || pressing_path == r.key_path) return {};
        std::vector<byte> held;
        if (read_small(pressing_path, key_file_limit, held) && std::string(held.begin(), held.end()) == hex) return {};
        if (write(pressing_path, hex.data(), hex.size())) return pressing_path;
        log("could not write " + pressing_path + " (the copy kept for this pressing)");
        return {};
    };

    // Writes the verified key as <TITLE_ID>.dkey (32 hex characters, the form the loader reads first). 'shown': the file
    // the result text names (the player's own files)
    const auto install = [&](const key128& key, outcome what, const std::string& source, const std::string& shown = {}) -> result
    {
        r.source = source;
        r.check_value = check_value(key);
        r.key = key; // verified, so usable even when the file cannot be written
        for (const std::string& wrong : wrong_files)
        {
            // Kept for the player to look at, out of the loader's way. Offline, an older file moved aside keeps its place
            // (it may hold another pressing's key): this one gets a name of its own.
            std::string aside = wrong + ".bad";
            if (offline_on)
            {
                std::vector<byte> before, now;
                if (read_small(aside, key_file_limit, before) && read_small(wrong, key_file_limit, now) && before != now)
                {
                    const auto held = key_from_file(now.data(), now.size());
                    aside = wrong + "." + (held ? check_value(*held) : std::string("x")) + ".bad";
                }
            }
            if (may_write(wrong) && may_write(aside) && env.rename_file && env.rename_file(wrong, aside)) log("moved " + wrong + " (does not decrypt this disc) to " + aside);
            else log("could not move " + wrong + " aside (it does not decrypt this disc)");
        }
        const std::string hex = to_hex(key.data(), key.size());
        if (!write_key(r.key_path, hex))
            return finish(outcome::write_failed, outcome_text(outcome::write_failed), "cannot write " + r.key_path + " (key " + r.check_value + " verified, from " + source + ")");
        r.changed = true;
        std::string also;
        if (!env.image_name.empty())
        {
            // The name the ISO loader looks up first: two pressings with one header id never share it
            const std::string copy = env.key_dir + env.image_name + ".dkey";
            if (copy != r.key_path)
            {
                if (write(copy, hex.data(), hex.size())) also = " and " + copy;
                else log("could not write " + copy + " (the loader still finds " + r.key_path + ")");
            }
        }
        const std::string pressing = keep_pressing_copy(hex);
        return finish(what, outcome_text(what, r.title_id, 0, 0, {}, shown), "saved " + r.key_path + also + " (key check " + r.check_value + ", from " + source + ")" +
            (pressing.empty() ? std::string{} : "; kept for this pressing as " + pressing));
    };

    // 1. The key already in place: for an ISO first the key files its loader looks up by the image's name. A wrong file
    // the loader would take before the right one is moved aside (install()).
    std::vector<std::string> in_place = env.image_key_files;
    for (const char* extension : {".dkey", ".key"}) in_place.push_back(stem + extension);
    for (const std::string& path : in_place)
    {
        if (!read_small(path, key_file_limit, data)) continue;
        ++count.cache;
        const auto key = key_from_file(data.data(), data.size());
        const std::optional<value_match> found = key ? test_value(*key) : std::nullopt;
        if (found && !found->data1)
        {
            if (!wrong_files.empty()) return install(found->key, outcome::already_present, path);
            r.key_path = path;
            r.check_value = check_value(found->key);
            r.source = path;
            r.key = found->key;
            // Keys cached before the pressing copies existed gain theirs the next time the disc is seen
            const std::string pressing = keep_pressing_copy(to_hex(found->key.data(), found->key.size()));
            return finish(outcome::already_present, outcome_text(outcome::already_present), path + " verified (key check " + r.check_value + ")" +
                (pressing.empty() ? std::string{} : "; kept for this pressing as " + pressing));
        }
        wrong_files.push_back(path);
        // data1 where the loader wants the key: moved aside, the key written in its place
        if (found) return install(found->key, outcome::local_key, path + " (data1)");
        log(path + (key ? " does not decrypt this disc (key check " + check_value(*key) + ")" : " holds no key"));
    }

    std::optional<std::vector<std::string>> redump_listing;
    const auto redump_names = [&]() -> const std::vector<std::string>&
    {
        if (!redump_listing) redump_listing = env.list_files ? env.list_files(env.key_dir) : std::vector<std::string>{};
        return *redump_listing;
    };

    // 1b. Offline: the copy kept for this pressing, then those of the title id's other pressings (a key shared by two
    // pressings)
    if (offline_on)
    {
        std::vector<std::string> copies;
        if (!pressing_path.empty()) copies.push_back(pressing_path);
        for (const std::string& name : redump_names())
        {
            if (copies.size() > max_pressing_copies) break;
            if (is_pressing_file(name, r.title_id) && env.key_dir + name != pressing_path) copies.push_back(env.key_dir + name);
        }
        for (const std::string& path : copies)
        {
            if (stopped()) break;
            copies_tested.push_back(path);
            if (!read_small(path, key_file_limit, data)) continue;
            ++count.cache;
            const auto key = key_from_file(data.data(), data.size());
            if (key && decrypts_all(*key, disc.blocks)) return install(*key, outcome::cache_pressing, path);
        }
    }

    // 2. Other key files on the console (a key pack imported with its own names); offline then the files a wrong key was
    // moved aside to (*.bad, also by 0.37-0.39), named after the disc first
    const auto test_key_files = [&](const std::string& folder, outcome what, const std::string& label) -> std::optional<result>
    {
        if (!env.list_files) return std::nullopt;
        std::vector<std::string> names = folder == env.key_dir ? redump_names() : env.list_files(folder);
        // The file named after the disc first
        std::stable_partition(names.begin(), names.end(), [&](const std::string& n) { return key_detail::iequals(n, r.title_id + ".dkey") || key_detail::iequals(n, r.title_id + ".key"); });
        std::size_t tried = 0;
        const auto tested_before = [&](const std::string& path)
        {
            return std::find(wrong_files.begin(), wrong_files.end(), path) != wrong_files.end() || std::find(copies_tested.begin(), copies_tested.end(), path) != copies_tested.end();
        };
        for (const std::string& name : names)
        {
            if (tried >= max_key_files || stopped()) break;
            if (!key_detail::iends_with(name, ".dkey") && !key_detail::iends_with(name, ".key")) continue;
            const std::string path = folder + name;
            if (tested_before(path)) continue;
            ++tried;
            if (!read_small(path, key_file_limit, data)) continue;
            ++count.redump;
            const auto key = key_from_file(data.data(), data.size());
            const std::optional<value_match> found = key ? test_value(*key) : std::nullopt;
            if (found) return install(found->key, what, label + name + (found->data1 ? " (data1)" : ""));
        }
        if (offline_on)
        {
            std::stable_partition(names.begin(), names.end(), [&](const std::string& n) { return n.size() > r.title_id.size() && key_detail::iequals(n.substr(0, r.title_id.size()), r.title_id); });
            for (const std::string& name : names)
            {
                if (tried >= max_key_files || stopped()) break;
                if (!key_detail::iends_with(name, ".bad")) continue;
                ++tried;
                if (!read_small(folder + name, key_file_limit, data)) continue;
                ++count.redump;
                const auto key = key_from_file(data.data(), data.size());
                const std::optional<value_match> found = key ? test_value(*key) : std::nullopt;
                if (found) return install(found->key, what, label + name + (found->data1 ? " (data1)" : ""));
            }
        }
        if (tried) log(std::to_string(tried) + " key file(s) in " + folder + " tested, none decrypts this disc");
        return std::nullopt;
    };

    // Tests one IRD file; on success 'save_as' (when given) keeps a copy in the IRD folder
    const auto test_ird = [&](const std::vector<byte>& bytes, const std::string& label, outcome what, const std::string& save_as, const std::string& shown = {}) -> std::optional<result>
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
        if (!save_as.empty() && !write(save_as, bytes.data(), bytes.size())) log("could not keep a copy as " + save_as);
        return install(key, what, label, shown);
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
            ++count.ird;
            if (auto done = test_ird(data, folder + name, outcome::image_ird, env.ird_dir + cache_name("image/" + code + " " + name))) return std::move(*done);
        }
        if (tried) log(std::to_string(tried) + " IRD file(s) in " + folder + " looked at, none decrypts this disc");
    }

    // 3. IRDs downloaded earlier; offline also those of other names whose product code (read from the file's head) is
    // this disc's (an IRD the player put there over FTP)
    const std::vector<std::string> cached_names = env.list_files ? env.list_files(env.ird_dir) : std::vector<std::string>{};
    {
        std::size_t tried = 0;
        for (const std::string& name : cached_names)
        {
            if (tried >= max_ird_files || stopped()) break;
            if (!key_detail::iends_with(name, ".ird") || !name_mentions(name, disc.product_code)) continue;
            ++tried;
            if (!read_small(env.ird_dir + name, ird_limit, data)) continue;
            ++count.ird;
            if (auto done = test_ird(data, env.ird_dir + name, outcome::cached_ird, {})) return std::move(*done);
        }
        for (const std::string& name : cached_names)
        {
            if (!offline_on || tried >= max_ird_files || stopped()) break;
            if (!key_detail::iends_with(name, ".ird") || name_mentions(name, disc.product_code)) continue;
            ++tried;
            if (!read_head(env.ird_dir + name, ird_peek_bytes, ird_limit, data)) continue;
            const std::string code = peek_product_code(data.data(), data.size());
            if (!key_detail::iequals(code, disc.product_code) && !key_detail::iequals(code, disc.header_id)) continue;
            if (!read_small(env.ird_dir + name, ird_limit, data)) continue;
            ++count.ird;
            if (auto done = test_ird(data, env.ird_dir + name, outcome::cached_ird, {})) return std::move(*done);
        }
    }

    // The player's own files (offline): key lists, with every value tried as the key and as data1
    std::size_t lists_read = 0, values_left = max_list_values;
    const auto test_list = [&](const std::vector<byte>& text, const std::string& label, outcome what) -> std::optional<result>
    {
        // A file the size of a key file (a key with a name or a comment around it) holds one value at most: it does not
        // count as one of the lists a search reads
        const bool small = text.size() <= key_file_limit;
        if ((!small && lists_read >= max_list_files) || !values_left)
        {
            log(label + ": not read (a search reads at most " + std::to_string(max_list_files) + " key lists and " + std::to_string(max_list_values) + " values)");
            return std::nullopt;
        }
        lists_read += !small;
        ++count.lists;
        std::optional<std::pair<value_match, std::size_t>> hit;
        const list_stats stats = scan_key_list(std::string_view(reinterpret_cast<const char*>(text.data()), text.size()), disc.product_code, disc.header_id,
            values_left, [&](const list_value& v)
            {
                if (const std::optional<value_match> found = test_value(v.value)) hit.emplace(*found, v.line);
                return hit.has_value();
            });
        values_left -= std::min(values_left, stats.offered);
        count.list_values += stats.offered;
        if (hit) return install(hit->first.key, what, label + " (line " + std::to_string(hit->second) + (hit->first.data1 ? ", data1" : "") + ")", label);
        log(label + ": " + std::to_string(stats.values) + " value(s) on " + std::to_string(stats.lines) + " line(s)" +
            (stats.named ? " (" + std::to_string(stats.named) + " on lines naming " + r.title_id + ")" : std::string{}) +
            (stats.truncated ? ", " + std::to_string(stats.offered) + " tested (the bound of one search)" : std::string{}) + ", none decrypts this disc");
        return std::nullopt;
    };

    // Where a file of the player's lies: the outcome its key gets
    enum class place
    {
        inbox,      // /data/rpcs3/keys
        usb_redump, // USB:/redump: key files keep their 0.39 outcome
        usb_ird,    // USB:/ird: IRDs keep theirs
        usb_keys,   // USB:/keys
    };
    const auto outcome_for = [](place where, user_file type)
    {
        if (where == place::inbox) return outcome::inbox_key;
        if (where == place::usb_redump && type == user_file::key) return outcome::usb_key;
        if (where == place::usb_ird && type == user_file::ird) return outcome::usb_ird;
        return outcome::usb_list;
    };

    // One file of the player's, decided by its content: a key file (16 bytes or 32 hex characters), an IRD (gzip or
    // "3IRD") of this disc, else a key list. 'name' names the IRD copy kept from USB.
    const auto test_content = [&](const std::vector<byte>& bytes, const std::string& label, const std::string& name, place where) -> std::optional<result>
    {
        if (const auto key = key_from_file(bytes.data(), bytes.size()))
        {
            const std::optional<value_match> found = test_value(*key);
            if (!found) return std::nullopt;
            return install(found->key, outcome_for(where, user_file::key), label + (found->data1 ? " (data1)" : ""), label);
        }
        if (bytes.size() >= 4 && (std::memcmp(bytes.data(), "3IRD", 4) == 0 || (bytes[0] == 0x1f && bytes[1] == 0x8b)))
        {
            const std::string code = peek_product_code(bytes.data(), bytes.size());
            if (!key_detail::iequals(code, disc.product_code) && !key_detail::iequals(code, disc.header_id)) return std::nullopt;
            return test_ird(bytes, label, outcome_for(where, user_file::ird), where == place::inbox ? std::string{} : env.ird_dir + cache_name("usb/" + code + " " + name), label);
        }
        return test_list(bytes, label, outcome_for(where, user_file::list));
    };

    // The player's files in 'folder' ("/.../"; with 'subfolders' also one level below it), only read. 'named_only': the
    // files whose name (or subfolder) mentions the disc; false: the others. Bounded per folder and per search.
    const auto scan_user_folder = [&](const std::string& folder, const std::string& shown, place where, bool named_only, bool subfolders) -> std::optional<result>
    {
        if (!env.list_files) return std::nullopt;
        struct spot
        {
            std::string path, label;
            bool named = false;
        };
        std::vector<spot> spots{{folder, shown, false}};
        if (subfolders && env.list_folders)
            for (const std::string& sub : env.list_folders(folder.substr(0, folder.size() - 1)))
            {
                if (spots.size() > max_subfolders) break;
                spots.push_back({folder + sub + "/", shown + sub + "/", name_mentions(sub, disc.product_code) || name_mentions(sub, disc.header_id)});
            }
        std::uint32_t& counter = where == place::inbox ? count.inbox : count.usb;
        const std::size_t key_cap = where == place::inbox || where == place::usb_keys ? max_inbox_key_files : max_key_files;
        for (const spot& s : spots)
        {
            std::size_t keys = 0, irds = 0, texts = 0, tried = 0;
            for (const std::string& name : env.list_files(s.path))
            {
                if (stopped()) return std::nullopt;
                const bool named = s.named || name_mentions(name, disc.product_code) || name_mentions(name, disc.header_id);
                if (named != named_only) continue;
                const user_file type = user_file_type(name);
                const std::string path = s.path + name, label = s.label + name;
                switch (type)
                {
                case user_file::key:
                    if (keys >= key_cap) continue;
                    ++keys;
                    // A key file holds a key; a bigger one is read as a key list
                    if (!read_small(path, key_file_limit, data) && !read_small(path, list_limit, data)) continue;
                    break;
                case user_file::ird:
                {
                    if (irds >= max_ird_files) continue;
                    ++irds;
                    if (!read_head(path, ird_peek_bytes, ird_limit, data)) continue;
                    const std::string code = peek_product_code(data.data(), data.size());
                    if (!key_detail::iequals(code, disc.product_code) && !key_detail::iequals(code, disc.header_id)) continue;
                    if (!read_small(path, ird_limit, data)) continue;
                    break;
                }
                case user_file::list:
                    // Tiny text files do not count as key lists (test_list): a folder of thousands is bounded here
                    if (texts >= key_cap) continue;
                    ++texts;
                    if (!read_small(path, list_limit, data))
                    {
                        log(label + ": not read (unreadable, or larger than " + std::to_string(list_limit >> 20) + " MiB)");
                        continue;
                    }
                    break;
                case user_file::other: continue;
                }
                ++tried;
                ++counter;
                if (auto done = test_content(data, label, name, where)) return done;
            }
            if (tried) log(std::to_string(tried) + " file(s) in " + s.label + " tested" + (named_only ? " (named after the disc)" : "") + ", none decrypts this disc");
        }
        return std::nullopt;
    };

    // 4. The inbox /data/rpcs3/keys (offline): files named after the disc first, then the others
    if (offline_on && !env.inbox_dir.empty())
        for (const bool named : {true, false})
            if (auto done = scan_user_folder(env.inbox_dir, env.inbox_dir, place::inbox, named, true)) return std::move(*done);

    // 5. USB drives (read only): redump/ (.dkey, .key) and ird/ (.ird), folder names in any case; offline also keys/
    // (one level of subfolders), and every kind of the player's files in all three. Files named after the disc are tried
    // before the download, the rest of those folders only when the database gave no key (or no download can work).
    const auto test_usb = [&](bool named_only) -> std::optional<result>
    {
        if (!env.list_folders || !env.list_files) return std::nullopt;
        for (const std::string& root : env.usb_roots)
        {
            for (const std::string& folder : env.list_folders(root))
            {
                const bool keys = key_detail::iequals(folder, "redump");
                const bool inbox = offline_on && key_detail::iequals(folder, "keys");
                if (!keys && !inbox && !key_detail::iequals(folder, "ird")) continue;
                const std::string path = root + "/" + folder + "/";
                const std::string shown = "USB" + std::string(root.empty() ? "" : root.substr(root.size() - 1)) + ":/" + folder + "/";
                if (offline_on)
                {
                    if (auto done = scan_user_folder(path, shown, inbox ? place::usb_keys : keys ? place::usb_redump : place::usb_ird, named_only, inbox)) return done;
                    continue;
                }
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
                        ++count.usb;
                        const auto key = key_from_file(data.data(), data.size());
                        if (key && decrypts_all(*key, disc.blocks)) return install(*key, outcome::usb_key, shown + name);
                        continue;
                    }
                    if (!key_detail::iends_with(name, ".ird")) continue;
                    ++tried;
                    if (!read_small(path + name, ird_limit, data)) continue;
                    ++count.usb;
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

    // Offline, nothing is downloaded when no download can work (no IP address, or a clock no certificate is valid at):
    // the rest of USB is searched and the result says why, without the retries and time-outs of each URL
    if (offline_on && env.fetch)
    {
        const int link = env.link_state ? env.link_state() : -1;
        const std::int64_t now = env.clock ? env.clock() : -1;
        const tls_state tls = env.tls ? env.tls() : tls_state::unknown;
        const net_state net = network_usable(link, now, env.build_date, tls);
        if (net != net_state::usable)
        {
            const std::string why = net == net_state::no_link ? "offline: netctl state " + std::to_string(link)
                : !disc_stream::clock_plausible(now, env.build_date) ? "console clock " + date_text(now) + " is before this build (" + env.build_date + ")"
                : std::string("the TLS self-test found no root certificate valid at the console's date");
            count.network = "skipped (" + why + ")";
            if (auto done = test_usb(false)) return std::move(*done);
            if (stopped()) return finish(outcome::cancelled, outcome_text(outcome::cancelled), "after the USB folders");
            const outcome what = net == net_state::clock_wrong ? outcome::clock_wrong : outcome::no_internet;
            return finish(what, outcome_text(what, {}, count.local()), "nothing downloaded (" + why + ")");
        }
    }

    // 6. The IRD database. Its failure is kept while the rest of the USB folders are searched.
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
        // A certificate check that failed (curl 60, 77) while the boot TLS self-test found no root valid at the console's
        // date: the clock, not the server (the self-test may have finished only after the check before the download)
        if (offline_on && (reply.curl == 60 || reply.curl == 77) && env.tls && env.tls() == tls_state::clock_refused)
            return fail(outcome::clock_wrong, outcome_text(outcome::clock_wrong), why + " (the TLS self-test found no root certificate valid at the console's date)");
        return fail(outcome::fetch_failed, outcome_text(outcome::fetch_failed, {}, 0, 0, reply.cause), why);
    };
    // A transfer that broke on the way or a server error is asked again (2 s, then 4 s later); no link, a refusal
    // (HTTP 4xx) or a stop is the answer at once
    const auto fetch = [&](const std::string& url, std::size_t limit, const std::string& what) -> fetch_reply
    {
        ++count.requests;
        fetch_reply reply = env.fetch(url, limit);
        for (int attempt = 1; attempt < fetch_attempts && !stopped(); ++attempt)
        {
            const bool again = reply.status == fetch_reply::kind::failed || (reply.status == fetch_reply::kind::http && (reply.http >= 500 || reply.http == 429));
            if (!again) break;
            log(what + ": attempt " + std::to_string(attempt) + " of " + std::to_string(fetch_attempts) + " failed (" + reply.log + "); trying again");
            if (env.pause) env.pause(2000ll << (attempt - 1));
            if (stopped()) break;
            ++count.requests;
            reply = env.fetch(url, limit);
        }
        count.network = "asked (" + std::to_string(count.requests) + " request(s))";
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
            if (env.write_file && !write(index_copy, index.body.data(), index.body.size())) log("index: could not keep a copy as " + index_copy);
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
    if (!env.fetch) count.network = "not available in this build";
    if (auto done = database()) return std::move(*done);

    // 7. The rest of the USB folders (key packs, IRDs named otherwise)
    if (pending.what != outcome::cancelled && !stopped())
        if (auto done = test_usb(false)) return std::move(*done);
    if (stopped()) pending = {outcome::cancelled, outcome_text(outcome::cancelled), "after the download"};
    // Offline the text counts the files that were tried
    if (offline_on && pending.what == outcome::no_internet) pending.text = outcome_text(outcome::no_internet, {}, count.local());
    return finish(pending.what, std::move(pending.text), pending.detail);
}
} // namespace utils::ps4::disc_key

#ifdef RPCS3_PS4

#include "Utilities/File.h"
#include "Emu/NP/ps4_tls_selftest.h"
#include "util/ps4_build_version.hpp"
#include "util/ps4_fs_repair.hpp"
#include "util/ps4_net.hpp"
#include "util/ps4_safe_mode.hpp"
#include "util/sysinfo.hpp"
#include "curl/curl/include/curl/curl.h"

#include <ctime>

namespace utils::ps4::disc_key
{
inline constexpr char ca_bundle[] = "/app0/certs/cacert.pem";
inline constexpr char data_root[] = "/data/rpcs3/";

namespace sys
{
// startup.log; the result and the summary of a search also go to RPCS3.log (disc_stream::log_mirror), where testers
// look first. Neither line ever holds a key.
inline void log_line(const std::string& line)
{
    ps4_startup_checkpoint(line.c_str());
    if (line.starts_with("disc key: result: ") || line.starts_with("disc key: sources tested: ") || line.find("': result: ") != std::string::npos ||
        line.find("': sources tested: ") != std::string::npos)
        disc_stream::mirror(line);
}

// A folder of /data/rpcs3 that refused the key's write, opened at once as the frontend does after a refused write: the
// app's own chmod, else SITE CHMOD through the console's FTP server (fs_repair::reopen_folder). Nothing is tried with
// /data/rpcs3/no-fs-repair.flag or for an error other than a permission refusal.
inline bool reopen(const std::string& folder)
{
#ifdef __ORBIS__
    namespace fr = utils::ps4::fs_repair;
    const fr::fs_ops calls = fr::sys::fs();
    std::uint32_t owner = 0;
    const int error = calls.probe(folder, owner);
    if (!error) return true;
    if (!fr::permission_error(error) || fs::is_file("/data/rpcs3/no-fs-repair.flag")) return false;
    std::string line;
    const bool opened = fr::reopen_folder(calls, disc_stream::sys::net(), fr::sys::server(), folder, line);
    ps4_startup_checkpoint(line.c_str());
    return opened;
#else
    (void)folder;
    return false;
#endif
}

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
        out.curl = static_cast<int>(code);
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
    env.log = log_line;
    env.key_dir = rpcs3::utils::get_redump_key_dir();
    if (env.key_dir.empty() || env.key_dir.back() != '/') env.key_dir += '/';
    constexpr std::string_view redump = "redump/";
    env.ird_dir = env.key_dir.ends_with(redump) ? env.key_dir.substr(0, env.key_dir.size() - redump.size()) + "ird/" : std::string("/data/rpcs3/data/ird/");
    for (unsigned i = 0; i < 8; ++i)
        if (const std::string root = "/mnt/usb" + std::to_string(i); fs::is_dir(root)) env.usb_roots.push_back(root);
    // Offline sources and order (feature row disc_keys_offline; disc-keys-039.flag gives the search of 0.39)
    env.offline_keys = utils::ps4::safe_mode::enabled(utils::ps4::safe_mode::feature::disc_keys_offline);
    env.inbox_dir = disc_key::inbox_dir;
    env.link_state = [] { return utils::ps4::net::link_state(); };
    env.clock = [] { return static_cast<std::int64_t>(std::time(nullptr)); };
    env.build_date = ps4_build_date;
    env.tls = [] { return tls_from_selftest(ps4_tls_selftest_ok(), ps4_tls_selftest_line()); };
    env.reopen = reopen;
    env.read_head = [](const std::string& path, std::size_t bytes, std::vector<byte>& out)
    {
        const fs::file file(path);
        if (!file) return false;
        const u64 size = file.size();
        out.resize(static_cast<std::size_t>(std::min<u64>(size, bytes)));
        return out.empty() || file.read(out.data(), out.size()) == out.size();
    };
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
        sys::log_line(prefix + std::string(text));
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
                (m_last_what == outcome::not_in_database ? " (it was not in the database a day ago)"
                    : m_last_what == outcome::clock_wrong ? " (the console clock was wrong at the last search)"
                                                          : " (the last search failed for the network)")).c_str());
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

    // A USB drive came: the next request() searches again for the disc in the drive when its last search found no key
    // (the stick may hold it). True when that was the case; a key found, or a search running, is left alone.
    bool forget_if_failed()
    {
        std::lock_guard lock(m_mutex);
        if (m_running || !m_handled || !m_last_known || succeeded(m_last_what)) return false;
        m_handled = false;
        m_retry_at = 0;
        return true;
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
