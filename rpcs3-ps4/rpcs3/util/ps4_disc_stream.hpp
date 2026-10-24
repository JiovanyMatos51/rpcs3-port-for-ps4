#pragma once

// PS3 disc in the PS4 drive, read through the console's own GoldHEN FTP server.
//
// The app's /dev has no optical node, while the GoldHEN FTP server serves /dev/cd0. The reader talks to it over the
// loopback (127.0.0.1:2121; /data/rpcs3/disc-ftp.txt may name another 'host:port') with USER, TYPE I, PASV, REST and
// RETR /dev/cd0 only: the app opens no device, sends no ioctl or SCSI command and writes nothing on the console.
//
// Server behaviour (GoldHEN FTP v2.2, measured):
//   - USER anonymous logs in at once (230); PASV only (EPSV 502). ABOR is not implemented (502) and is not even read
//     while a transfer runs: a transfer ends when the client closes the data connection, answered by
//     "426 Connection closed; transfer aborted" (226 when everything had been sent), and the control connection stays
//     usable. A stream left unread for 20 s resumes at once.
//   - REST must be a multiple of 2048 (451 after the 150 otherwise). The end of the disc answers 226 after its last
//     byte, a REST past it 451.
//   - RETR answers "550 File unavailable" whenever bit 31 of the REST offset is set (offset mod 4 GiB >= 2 GiB),
//     while a stream started below such a boundary reads straight through it. Those ranges are reached by starting
//     at the last sector below their 2 GiB boundary and reading forward. The reader learns this from the first such
//     550, so a server that seeks there is used directly.
//
// Random access is therefore slow past each such boundary, while one RETR from offset 0 streams the whole device: a
// disc is installed (ps4_disc_install.hpp) rather than played from the drive.
//
// Pieces:
//   ftp_connection  one control connection and at most one RETR stream; NOOP on an idle connection
//   reader          thread-safe disc reads: chunk cache (LRU), read-ahead, stream reuse, two cursors, retries
//   locate_files    PARAM.SFO, LIC.DAT and EBOOT.BIN through the ISO 9660 tree of plain region 0
//   drive           disc status for the XMB and for a running game installed from a disc (ps4_disc_lock.hpp):
//                   poll() never waits for the network, a worker re-reads sectors 0-1; hold() keeps the drive to an
//                   install. When no FTP server answers the status keeps NetCtl's state (net_link): GoldHEN's FTP
//                   server does not listen without an IP address, so "no network" and "FTP server off" read apart
//   read_header     sectors 0-1 and 16 through a temporary connection (disc probe, stage 3)
//   network_line    NetCtl's state and the console clock against this build's date, for the offline paths
//   PS4 only        socket calls, server list, service::instance(), the virtual file the ISO loader opens
// Log lines start with "disc stream:"; the drive's state changes also go to RPCS3.log through log_mirror, the sink the
// frontend sets for the disc lines testers send most (drive, key search, disc lock).

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <pthread.h>
#endif

namespace utils::ps4::disc_stream
{
inline constexpr std::uint64_t sector_size = 2048;
inline constexpr std::uint16_t default_port = 2121;
inline constexpr std::uint32_t loopback_ipv4 = 0x7f000001; // 127.0.0.1
inline constexpr char node_path[] = "/dev/cd0";
inline constexpr char config_file[] = "/data/rpcs3/disc-ftp.txt";

using log_fn = std::function<void(const std::string&)>;

namespace detail
{
inline std::uint32_t be32(const unsigned char* p) noexcept
{
    return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) | std::uint32_t{p[3]};
}

inline std::uint32_t le32(const unsigned char* p) noexcept
{
    return std::uint32_t{p[0]} | (std::uint32_t{p[1]} << 8) | (std::uint32_t{p[2]} << 16) | (std::uint32_t{p[3]} << 24);
}

inline std::uint16_t le16(const unsigned char* p) noexcept
{
    return static_cast<std::uint16_t>(p[0] | (p[1] << 8));
}

inline std::string hex(std::uint64_t value)
{
    char text[24];
    std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(value));
    return text;
}

// The printable ASCII of a fixed-size text field, up to its first NUL and without its trailing padding
inline std::string printable(const unsigned char* data, std::size_t size)
{
    std::string out;
    for (std::size_t i = 0; i < size && data[i]; ++i) out += data[i] >= 0x20 && data[i] < 0x7f ? static_cast<char>(data[i]) : '?';
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

inline std::string mib(std::uint64_t bytes)
{
    char text[32];
    std::snprintf(text, sizeof(text), "%.1f MiB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    return text;
}

inline std::uint64_t fnv1a64(const unsigned char* data, std::size_t size, std::uint64_t hash = 0xcbf29ce484222325ull) noexcept
{
    for (std::size_t i = 0; i < size; ++i) hash = (hash ^ data[i]) * 0x100000001b3ull;
    return hash;
}

inline std::int64_t now_ms() noexcept
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

inline std::string errno_text(int error)
{
    const char* name = nullptr;
    switch (error)
    {
    case EPERM: name = "EPERM"; break;
    case EIO: name = "EIO"; break;
    case ENOMEM: name = "ENOMEM"; break;
    case EACCES: name = "EACCES"; break;
    case EINVAL: name = "EINVAL"; break;
    case EPIPE: name = "EPIPE"; break;
    case EAGAIN: name = "EAGAIN"; break;
    case ECONNREFUSED: name = "ECONNREFUSED"; break;
    case ECONNRESET: name = "ECONNRESET"; break;
    case ETIMEDOUT: name = "ETIMEDOUT"; break;
    case ENETUNREACH: name = "ENETUNREACH"; break;
    case EHOSTUNREACH: name = "EHOSTUNREACH"; break;
    case ENOTCONN: name = "ENOTCONN"; break;
    case ECANCELED: name = "ECANCELED"; break;
    default: break;
    }
    return "errno " + std::to_string(error) + (name ? std::string(" (") + name + ")" : std::string{});
}

inline bool iequals(std::string_view a, std::string_view b) noexcept
{
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        const char x = a[i] >= 'a' && a[i] <= 'z' ? static_cast<char>(a[i] - 32) : a[i];
        const char y = b[i] >= 'a' && b[i] <= 'z' ? static_cast<char>(b[i] - 32) : b[i];
        if (x != y) return false;
    }
    return true;
}

// Sleeps up to ms in short steps; false once 'stop' is set
inline bool pause(std::int64_t ms, const std::atomic<bool>& stop)
{
    const std::int64_t end = now_ms() + ms;
    while (!stop.load(std::memory_order_acquire))
    {
        const std::int64_t left = end - now_ms();
        if (left <= 0) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(std::min<std::int64_t>(left, 50)));
    }
    return false;
}
}

// "BLUS-30443" or "blus30443" -> "BLUS30443"; anything that is not 4 letters and 5 digits -> "".
inline std::string canonical_title_id(std::string_view text)
{
    std::string out;
    bool dash = false;
    for (const char c : text)
    {
        if (c == '-' && out.size() == 4 && !std::exchange(dash, true)) continue;
        out += c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
    }
    if (out.size() != 9) return {};
    for (std::size_t i = 0; i < 9; ++i)
        if (i < 4 ? (out[i] < 'A' || out[i] > 'Z') : (out[i] < '0' || out[i] > '9')) return {};
    return out;
}

// Sector 1 of a PS3 disc starts with "PlayStation3"; the title id follows at 0x10 ("BLUS-30443", space padded).
inline bool has_ps3_magic(const unsigned char* sector1, std::size_t size) noexcept
{
    return size >= 12 && std::memcmp(sector1, "PlayStation3", 12) == 0;
}

// Title id of a PS3 disc ("BLUS30443") from its sector 1, "" when the sector is not one of a PS3 disc.
inline std::string title_id_from_header(const unsigned char* sector1, std::size_t size)
{
    if (size < 0x20 || !has_ps3_magic(sector1, size)) return {};
    return canonical_title_id(detail::printable(sector1 + 0x10, 0x10));
}

// The NetCtl state in which the console has an IP address (sceNetCtlGetState)
inline constexpr int netctl_ip_obtained = 3;

namespace detail
{
// Days from 1970-01-01 to a civil date (H. Hinnant's days_from_civil)
inline std::int64_t days_from_civil(std::int64_t year, unsigned month, unsigned day) noexcept
{
    year -= month <= 2;
    const std::int64_t era = (year >= 0 ? year : year - 399) / 400;
    const auto yoe = static_cast<unsigned>(year - era * 400);
    const unsigned doy = (153 * (month > 2 ? month - 3 : month + 9) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

// "2026-10-16" of a time in unix seconds (UTC), with " 14:05 UTC" when 'with_time'; "an unknown date" when negative
inline std::string utc_text(std::int64_t unix_seconds, bool with_time = false)
{
    if (unix_seconds < 0) return "an unknown date";
    // Civil date of a day count (the inverse of days_from_civil)
    const std::int64_t z = unix_seconds / 86400 + 719468;
    const std::int64_t era = z / 146097, doe = z - era * 146097;
    const std::int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    const std::int64_t day = doy - (153 * mp + 2) / 5 + 1, month = mp < 10 ? mp + 3 : mp - 9, year = yoe + era * 400 + (month <= 2);
    const std::int64_t minutes = unix_seconds % 86400 / 60;
    char text[48];
    if (with_time)
        std::snprintf(text, sizeof(text), "%04lld-%02lld-%02lld %02lld:%02lld UTC", static_cast<long long>(year), static_cast<long long>(month),
            static_cast<long long>(day), static_cast<long long>(minutes / 60), static_cast<long long>(minutes % 60));
    else
        std::snprintf(text, sizeof(text), "%04lld-%02lld-%02lld", static_cast<long long>(year), static_cast<long long>(month), static_cast<long long>(day));
    return text;
}

// Unix seconds at 00:00 UTC of "YYYY-MM-DD"; -1 when the text is not such a date
inline std::int64_t unix_of_date(std::string_view date) noexcept
{
    if (date.size() != 10 || date[4] != '-' || date[7] != '-') return -1;
    unsigned value[3]{};
    const std::size_t at[3] = {0, 5, 8}, length[3] = {4, 2, 2};
    for (int part = 0; part < 3; ++part)
        for (std::size_t i = at[part]; i < at[part] + length[part]; ++i)
        {
            if (date[i] < '0' || date[i] > '9') return -1;
            value[part] = value[part] * 10 + static_cast<unsigned>(date[i] - '0');
        }
    if (value[0] < 1970 || value[1] < 1 || value[1] > 12 || value[2] < 1 || value[2] > 31) return -1;
    return days_from_civil(value[0], value[1], value[2]) * 86400;
}
}

// A console clock earlier than the day before this build's date cannot be right: no console runs an application built
// after its own date (a reset clock breaks every certificate check). True when either value is unknown (a negative
// clock, a malformed date), so nothing is blamed on the clock without evidence.
inline bool clock_plausible(std::int64_t now_unix, std::string_view build_date) noexcept
{
    const std::int64_t built = detail::unix_of_date(build_date);
    return now_unix < 0 || built < 0 || now_unix >= built - 86400;
}

// "network: netctl state 1, IP no, clock 2026-10-23 14:05 UTC (plausible)": what the disc paths depend on offline
inline std::string network_line(int link, std::int64_t now_unix, std::string_view build_date)
{
    return "network: netctl state " + std::to_string(link) + (link < 0 ? std::string(" (unknown)") : std::string{}) + ", IP " +
        (link == netctl_ip_obtained ? "yes" : link < 0 ? "unknown" : "no") + ", clock " + detail::utc_text(now_unix, true) + " (" +
        (now_unix < 0 ? "unknown" : clock_plausible(now_unix, build_date) ? "plausible" : "before the build") + ")";
}

// A second sink for the few disc lines support needs from RPCS3.log, the log testers send far more often than
// startup.log: the drive's state changes, the key search's result and sources, the disc lock's decisions. The frontend
// points it at its RPCS3.log channel; nullptr (the default, and in the host tests) mirrors nothing. The lines it gets
// never hold a key, only check values.
using mirror_fn = void (*)(std::string_view line);
inline std::atomic<mirror_fn> log_mirror{nullptr};

inline void mirror(std::string_view line)
{
    if (const mirror_fn sink = log_mirror.load(std::memory_order_acquire)) sink(line);
}

// Runs 'work' on a detached thread; false when no thread could be created. On the console pthread_create reports a
// refused thread (the per-process thread cap) as an error code, where std::thread would end the app (that build has
// no exceptions).
inline bool run_detached(std::function<void()> work)
{
#ifndef _WIN32
    auto* owned = new (std::nothrow) std::function<void()>(std::move(work));
    if (!owned) return false;
    pthread_t thread{};
    const auto entry = [](void* arg) -> void*
    {
        const std::unique_ptr<std::function<void()>> held(static_cast<std::function<void()>*>(arg));
        (*held)();
        return nullptr;
    };
    if (::pthread_create(&thread, nullptr, +entry, owned) != 0)
    {
        delete owned;
        return false;
    }
    ::pthread_detach(thread);
    return true;
#else
    std::thread(std::move(work)).detach();
    return true;
#endif
}

// GoldHEN FTP 2.2 cannot start a transfer at an offset whose bit 31 is set.
inline constexpr bool rest_refused_at(std::uint64_t offset) noexcept
{
    return (offset & 0x80000000ull) != 0;
}

// For such an offset: the last sector below the 2 GiB boundary that starts its range, where a transfer can start.
inline constexpr std::uint64_t rest_floor(std::uint64_t offset) noexcept
{
    return (offset & ~std::uint64_t{0x7fffffff}) - sector_size;
}

// Sector 0 of a PS3 disc: u32 BE count of plain regions, u32 0, then the first and last LBA of each plain region.
// The encrypted regions lie between them and the last plain region ends the disc.
struct layout
{
    bool valid = false;
    std::uint32_t plain_regions = 0; // the count field as read
    std::uint64_t end_lba = 0;       // one past the last LBA of the last plain region
    std::vector<std::pair<std::uint32_t, std::uint32_t>> plain;

    std::uint64_t size_bytes() const noexcept { return end_lba * sector_size; }
};

inline layout parse_layout(const unsigned char* sector0, std::size_t size)
{
    layout out;
    if (size < 16) return out;
    out.plain_regions = detail::be32(sector0);
    if (out.plain_regions < 1 || out.plain_regions > 127 || 8 + std::size_t{out.plain_regions} * 8 > size) return out;
    for (std::uint32_t i = 0; i < out.plain_regions; ++i)
    {
        const std::uint32_t first = detail::be32(sector0 + 8 + i * 8);
        const std::uint32_t last = detail::be32(sector0 + 12 + i * 8);
        if (last < first || (i == 0 && first != 0) || (i > 0 && first <= out.plain.back().second)) return {};
        out.plain.emplace_back(first, last);
    }
    out.end_lba = std::uint64_t{out.plain.back().second} + 1;
    out.valid = true;
    return out;
}

// A UTF-8 value of a PARAM.SFO image ("TITLE"), "" when the key or the image is not valid. Line breaks become spaces.
inline std::string sfo_string(const unsigned char* data, std::size_t size, std::string_view key)
{
    if (size < 20 || std::memcmp(data, "\0PSF", 4) != 0) return {};
    const std::size_t keys = detail::le32(data + 8), values = detail::le32(data + 12), count = detail::le32(data + 16);
    if (keys > size || values > size || count > 1024 || 20 + count * 16 > size) return {};
    for (std::size_t i = 0; i < count; ++i)
    {
        const unsigned char* entry = data + 20 + i * 16;
        const std::size_t name_at = keys + detail::le16(entry);
        if (name_at >= size) continue;
        const void* end = std::memchr(data + name_at, 0, size - name_at);
        if (!end) continue;
        const std::string_view name(reinterpret_cast<const char*>(data + name_at), static_cast<std::size_t>(static_cast<const unsigned char*>(end) - (data + name_at)));
        if (name != key) continue;
        const std::uint16_t format = detail::le16(entry + 2);
        const std::size_t length = detail::le32(entry + 4), value_at = values + detail::le32(entry + 12);
        if ((format != 0x0204 && format != 0x0004) || value_at > size || length > size - value_at) return {};
        std::string out(reinterpret_cast<const char*>(data + value_at), length);
        if (const std::size_t nul = out.find('\0'); nul != std::string::npos) out.resize(nul);
        for (char& c : out) if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        return out;
    }
    return {};
}

// ISO 9660 directory record target
struct extent
{
    std::uint32_t lba = 0;
    std::uint32_t bytes = 0;
    bool directory = false;
};

// Root directory of a primary volume descriptor (sector 16)
inline std::optional<extent> root_directory(const unsigned char* pvd, std::size_t size)
{
    if (size < 190 || pvd[0] != 1 || std::memcmp(pvd + 1, "CD001", 5) != 0) return std::nullopt;
    const unsigned char* record = pvd + 156;
    return extent{detail::le32(record + 2), detail::le32(record + 10), (record[25] & 2) != 0};
}

// The record called 'name' (any case, ";1" ignored) in the data of a directory
inline std::optional<extent> find_entry(const unsigned char* dir, std::size_t size, std::string_view name)
{
    std::size_t at = 0;
    while (at < size)
    {
        const std::size_t length = dir[at];
        if (length == 0)
        {
            at = (at / sector_size + 1) * sector_size; // records do not cross a sector: the rest of this one is padding
            continue;
        }
        if (length < 34 || at + length > size) return std::nullopt;
        const std::size_t name_length = dir[at + 32];
        if (33 + name_length <= length)
        {
            std::string_view entry(reinterpret_cast<const char*>(dir + at + 33), name_length);
            if (const std::size_t semicolon = entry.find(';'); semicolon != std::string_view::npos) entry = entry.substr(0, semicolon);
            if (detail::iequals(entry, name)) return extent{detail::le32(dir + at + 2), detail::le32(dir + at + 10), (dir[at + 25] & 2) != 0};
        }
        at += length;
    }
    return std::nullopt;
}

// read(offset, out, size): true when all 'size' bytes came
using read_fn = std::function<bool(std::uint64_t offset, void* out, std::size_t size)>;

// Files of a PS3 disc found through the ISO 9660 tree (plain region 0)
struct disc_files
{
    bool tree = false;                // the PVD, the root and PS3_GAME were read
    std::optional<extent> param_sfo;  // PS3_GAME/PARAM.SFO
    std::optional<extent> lic_dat;    // PS3_GAME/LICDIR/LIC.DAT: "PS3LICDA" once decrypted
    std::optional<extent> eboot;      // PS3_GAME/USRDIR/EBOOT.BIN: "SCE" once decrypted
    std::vector<unsigned char> param; // PARAM.SFO (at most 64 KiB), empty when unreadable
};

// PARAM.SFO always; LIC.DAT and EBOOT.BIN too when 'all'. Directories over 1 MiB are not read.
inline disc_files locate_files(const read_fn& read, bool all = true)
{
    disc_files out;
    std::array<unsigned char, sector_size> pvd{};
    if (!read(16 * sector_size, pvd.data(), pvd.size())) return out;
    const auto root = root_directory(pvd.data(), pvd.size());
    if (!root || !root->directory) return out;
    std::vector<unsigned char> data;
    const auto load = [&](const extent& e, std::uint32_t limit)
    {
        if (!e.bytes || e.bytes > limit) return false;
        data.assign(e.bytes, 0);
        return read(std::uint64_t{e.lba} * sector_size, data.data(), data.size());
    };
    const auto file = [](const std::optional<extent>& e) -> std::optional<extent>
    {
        if (e && !e->directory) return e;
        return std::nullopt;
    };
    if (!load(*root, 1u << 20)) return out;
    const auto game = find_entry(data.data(), data.size(), "PS3_GAME");
    if (!game || !game->directory || !load(*game, 1u << 20)) return out;
    out.tree = true;
    const std::vector<unsigned char> game_dir = std::move(data);
    out.param_sfo = file(find_entry(game_dir.data(), game_dir.size(), "PARAM.SFO"));
    if (all)
    {
        const auto lic_dir = find_entry(game_dir.data(), game_dir.size(), "LICDIR");
        if (lic_dir && lic_dir->directory && load(*lic_dir, 1u << 20)) out.lic_dat = file(find_entry(data.data(), data.size(), "LIC.DAT"));
        const auto usr_dir = find_entry(game_dir.data(), game_dir.size(), "USRDIR");
        if (usr_dir && usr_dir->directory && load(*usr_dir, 1u << 20)) out.eboot = file(find_entry(data.data(), data.size(), "EBOOT.BIN"));
    }
    if (out.param_sfo && load(*out.param_sfo, 65536)) out.param = std::move(data);
    return out;
}

// What tells one pressing of a PS3 disc from another: FNV-1a 64 chained over sectors 0-1, the PVD (sector 16) and
// PARAM.SFO. All three lie in plain region 0, so a disc and its decrypted image give the same value. Never 0.
inline std::uint64_t disc_fingerprint(const unsigned char* head, std::size_t head_size, const unsigned char* pvd, std::size_t pvd_size,
    const unsigned char* sfo, std::size_t sfo_size) noexcept
{
    const std::uint64_t hash = detail::fnv1a64(sfo, sfo_size, detail::fnv1a64(pvd, pvd_size, detail::fnv1a64(head, head_size)));
    return hash ? hash : 1;
}

struct endpoint
{
    std::uint32_t ipv4 = 0; // host order
    std::uint16_t port = 0;

    bool operator==(const endpoint&) const = default;
};

inline std::string to_string(const endpoint& e)
{
    char text[32];
    std::snprintf(text, sizeof(text), "%u.%u.%u.%u:%u", e.ipv4 >> 24, (e.ipv4 >> 16) & 0xff, (e.ipv4 >> 8) & 0xff, e.ipv4 & 0xff, unsigned{e.port});
    return text;
}

inline std::optional<std::uint32_t> parse_ipv4(std::string_view text)
{
    std::uint32_t out = 0;
    for (int part = 0; part < 4; ++part)
    {
        if (part && (text.empty() || text.front() != '.')) return std::nullopt;
        if (part) text.remove_prefix(1);
        unsigned value = 0, digits = 0;
        while (!text.empty() && text.front() >= '0' && text.front() <= '9' && digits < 4)
        {
            value = value * 10 + static_cast<unsigned>(text.front() - '0');
            text.remove_prefix(1);
            ++digits;
        }
        if (!digits || value > 255) return std::nullopt;
        out = (out << 8) | value;
    }
    if (!text.empty()) return std::nullopt;
    return out;
}

// "192.168.1.33:2121", "127.0.0.1" (default port) or "ftp://192.168.1.33:2121/": the first line of the text, spaces
// around it ignored. Host names are not resolved.
inline std::optional<endpoint> parse_endpoint(std::string_view text, std::uint16_t port = default_port)
{
    if (const std::size_t eol = text.find_first_of("\r\n"); eol != std::string_view::npos) text = text.substr(0, eol);
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\xef' || text.front() == '\xbb' || text.front() == '\xbf')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '/')) text.remove_suffix(1);
    if (text.size() >= 6 && detail::iequals(text.substr(0, 6), "ftp://")) text.remove_prefix(6);
    endpoint out{0, port};
    if (const std::size_t colon = text.find(':'); colon != std::string_view::npos)
    {
        const std::string_view number = text.substr(colon + 1);
        unsigned value = 0;
        if (number.empty() || number.size() > 5) return std::nullopt;
        for (const char c : number)
        {
            if (c < '0' || c > '9') return std::nullopt;
            value = value * 10 + static_cast<unsigned>(c - '0');
        }
        if (!value || value > 65535) return std::nullopt;
        out.port = static_cast<std::uint16_t>(value);
        text = text.substr(0, colon);
    }
    const auto ip = parse_ipv4(text);
    if (!ip || !*ip) return std::nullopt;
    out.ipv4 = *ip;
    return out;
}

// One FTP reply: the code (0 when none came, -1 for a line that is not a reply) and its first line
struct reply
{
    int code = 0;
    std::string text;
};

// Takes one complete reply (single or multi-line) off the front of 'buffer'; nullopt while it is incomplete.
// 'lines': the lines between the first and the last of a multi-line reply, trimmed, "; " between them (at most 512 bytes).
inline std::optional<reply> take_reply(std::string& buffer, std::string* lines = nullptr)
{
    const auto code_of = [](std::string_view line) -> int
    {
        if (line.size() < 3 || line[0] < '1' || line[0] > '5' || line[1] < '0' || line[1] > '9' || line[2] < '0' || line[2] > '9') return -1;
        return (line[0] - '0') * 100 + (line[1] - '0') * 10 + (line[2] - '0');
    };
    std::size_t at = 0;
    int code = 0;
    std::string first;
    if (lines) lines->clear(); // each call reads the buffer from its start
    for (;;)
    {
        const std::size_t eol = buffer.find('\n', at);
        if (eol == std::string::npos) return std::nullopt;
        std::string_view line(buffer.data() + at, eol - at);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        const std::size_t next = eol + 1;
        if (code == 0)
        {
            code = code_of(line);
            first = std::string(line);
            if (code < 0 || line.size() == 3 || line[3] != '-')
            {
                buffer.erase(0, next);
                return reply{code, std::move(first)};
            }
        }
        else if (line.size() >= 4 && line[3] == ' ' && code_of(line) == code)
        {
            buffer.erase(0, next);
            return reply{code, std::move(first)};
        }
        else if (lines)
        {
            while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
            while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) line.remove_suffix(1);
            if (!line.empty() && lines->size() + line.size() + 2 <= 512) *lines += (lines->empty() ? "" : "; ") + std::string(line);
        }
        at = next;
    }
}

// "227 Entering Passive Mode (h1,h2,h3,h4,p1,p2)." -> the advertised IPv4 (host order) and port
inline bool parse_pasv(std::string_view text, std::uint32_t& host, std::uint16_t& port)
{
    const std::size_t open = text.find('(');
    if (open == std::string_view::npos) return false;
    text.remove_prefix(open + 1);
    unsigned values[6]{};
    for (int i = 0; i < 6; ++i)
    {
        unsigned value = 0, digits = 0;
        while (!text.empty() && text.front() == ' ') text.remove_prefix(1);
        while (!text.empty() && text.front() >= '0' && text.front() <= '9' && digits < 4)
        {
            value = value * 10 + static_cast<unsigned>(text.front() - '0');
            text.remove_prefix(1);
            ++digits;
        }
        if (!digits || value > 255) return false;
        values[i] = value;
        if (i < 5)
        {
            if (text.empty() || text.front() != ',') return false;
            text.remove_prefix(1);
        }
    }
    host = (values[0] << 24) | (values[1] << 16) | (values[2] << 8) | values[3];
    port = static_cast<std::uint16_t>((values[4] << 8) | values[5]);
    return port != 0;
}

// The socket calls, so that a test can stand in for the console. Every call is bounded by its timeout.
struct net_ops
{
    // A connected TCP socket (>= 0), or -1 with 'error' set (ETIMEDOUT when the time ran out). receive_buffer: the
    // SO_RCVBUF to ask for, 0 for the default.
    std::function<int(const endpoint& to, int timeout_ms, int receive_buffer, int& error)> connect;
    // All 'size' bytes sent: size; otherwise -1 with 'error' set.
    std::function<std::int64_t(int socket, const void* data, std::size_t size, int timeout_ms, int& error)> send;
    // Bytes received (> 0), 0 when the peer closed the connection, -1 with 'error' set (ETIMEDOUT: nothing came).
    std::function<std::int64_t(int socket, void* data, std::size_t size, int timeout_ms, int& error)> recv;
    std::function<void(int socket)> close;
};

struct timeouts
{
    int connect_ms = 3000;
    int reply_ms = 5000;
    int first_byte_ms = 15000; // a drive that spun down answers after about 2 s
    int data_ms = 10000;
    int slice_ms = 250;        // waits are cut into slices so that a stop is seen
};

// One control connection and at most one RETR stream (a cursor at a disc position). Not thread-safe: the reader
// serializes its use.
class ftp_connection
{
public:
    enum class failure
    {
        none,
        connect,  // the TCP connection was refused or did not come up
        timeout,  // no reply or no data in time
        closed,   // the server closed the connection
        protocol, // an unexpected reply
        login,    // the login was refused
        refused,  // RETR answered 550 (the device could not be opened there)
        failed,   // the transfer failed (451, 426 before the end)
        stopped,  // the reader is stopping
    };

    static constexpr int receive_buffer = 1 << 20;

    ftp_connection(const net_ops& net, const timeouts& limits, const std::atomic<bool>& stop)
        : m_net(net), m_limits(limits), m_stop(stop)
    {
    }

    ~ftp_connection() { disconnect(); }

    ftp_connection(const ftp_connection&) = delete;
    ftp_connection& operator=(const ftp_connection&) = delete;

    bool connected() const noexcept { return m_control >= 0; }
    bool streaming() const noexcept { return m_data >= 0; }
    std::uint64_t position() const noexcept { return m_position; }
    const endpoint& server() const noexcept { return m_server; }
    const std::string& banner() const noexcept { return m_banner; }
    std::int64_t connect_ms() const noexcept { return m_connect_ms; }
    failure last_failure() const noexcept { return m_failure; }
    int last_error() const noexcept { return m_error; }
    int last_code() const noexcept { return m_code; }
    const std::string& last_reply() const noexcept { return m_reply; }
    int end_code() const noexcept { return m_end_code; } // final reply of the last stream that ended (226, 451, 426)

    // Every later wait ends by this steady-clock time (ms); 0: no limit
    void set_deadline(std::int64_t deadline_ms) noexcept { m_deadline = deadline_ms; }

    std::string failure_text() const
    {
        switch (m_failure)
        {
        case failure::none: return "ok";
        case failure::connect: return "connect failed: " + detail::errno_text(m_error);
        case failure::timeout: return "timed out";
        case failure::closed: return "connection closed by the server";
        case failure::protocol: return "unexpected reply '" + m_reply + "'";
        case failure::login: return "login refused: '" + m_reply + "'";
        case failure::refused: return "refused: '" + m_reply + "'";
        case failure::failed: return "transfer failed: '" + m_reply + "'";
        case failure::stopped: return "stopped";
        }
        return {};
    }

    // Control connection, banner, anonymous login, TYPE I
    bool connect(const endpoint& to)
    {
        disconnect();
        const std::int64_t started = detail::now_ms();
        m_server = to;
        int error = 0;
        m_control = m_net.connect(to, limit(m_limits.connect_ms), 0, error);
        if (m_control < 0) return fail(failure::connect, error);
        reply r = read_reply(m_limits.reply_ms);
        if (r.code != 220) return drop(r.code ? failure::protocol : m_failure, r);
        m_banner = r.text.size() > 4 ? r.text.substr(4) : r.text;
        r = command("USER anonymous");
        if (r.code == 331) r = command("PASS anonymous@");
        if (r.code != 230 && r.code != 202) return drop(r.code ? failure::login : m_failure, r);
        r = command("TYPE I");
        if (r.code != 200) return drop(r.code ? failure::protocol : m_failure, r);
        m_connect_ms = detail::now_ms() - started;
        m_failure = failure::none;
        return true;
    }

    // FEAT on a connection without a stream: the features the server lists ("; " between them; "" when it lists none
    // or does not know FEAT). nullopt when no complete reply came: the connection is then closed, so a late reply
    // never answers the next command.
    std::optional<std::string> features()
    {
        if (!connected() || streaming()) return std::nullopt;
        std::string lines;
        const std::string line = "FEAT\r\n";
        int error = 0;
        reply r;
        if (m_net.send(m_control, line.data(), line.size(), limit(m_limits.reply_ms), error) == static_cast<std::int64_t>(line.size()))
            r = read_reply(m_limits.reply_ms, &lines);
        if (r.code <= 0)
        {
            disconnect();
            return std::nullopt;
        }
        m_failure = failure::none;
        return r.code == 211 ? lines : std::string{};
    }

    // PASV, data connection, REST offset (none at 0), RETR /dev/cd0. offset must be a multiple of 2048 (the server
    // answers 451 otherwise). On a refusal (550) the control connection stays up; on a time-out or an unexpected
    // reply it is closed.
    bool open_stream(std::uint64_t offset)
    {
        if (!connected()) return fail(failure::closed);
        if (offset % sector_size) return fail(failure::protocol, EINVAL, {-1, "unaligned offset " + std::to_string(offset)});
        if (streaming() && !close_stream()) return false;
        reply r = command("PASV");
        std::uint32_t host = 0;
        std::uint16_t port = 0;
        if (r.code != 227 || !parse_pasv(r.text, host, port)) return drop(r.code ? failure::protocol : m_failure, r);
        // The reply names the server's LAN address; the address this connection reached is tried first
        int error = 0;
        int data = m_net.connect({m_server.ipv4, port}, limit(m_limits.connect_ms), receive_buffer, error);
        if (data < 0 && host && host != m_server.ipv4) data = m_net.connect({host, port}, limit(m_limits.connect_ms), receive_buffer, error);
        if (data < 0)
        {
            fail(failure::connect, error);
            disconnect();
            return false;
        }
        if (offset)
        {
            r = command("REST " + std::to_string(offset));
            if (r.code != 350)
            {
                m_net.close(data);
                return drop(r.code ? failure::protocol : m_failure, r);
            }
        }
        r = command(std::string("RETR ") + node_path);
        if (r.code != 150 && r.code != 125)
        {
            m_net.close(data);
            if (r.code == 550) return fail(failure::refused, 0, r);
            if (r.code > 0) return fail(failure::failed, 0, r);
            return drop(m_failure, r);
        }
        m_data = data;
        m_position = offset;
        m_first_byte = true;
        m_end_code = 0;
        m_failure = failure::none;
        return true;
    }

    // Some bytes of the stream: > 0; 0 at the end of the disc (226); -1 on a failure (last_failure()).
    std::int64_t read_some(void* buffer, std::size_t size)
    {
        if (!streaming())
        {
            fail(failure::closed);
            return -1;
        }
        int error = 0;
        const std::int64_t got = recv_wait(m_data, buffer, size, m_first_byte ? m_limits.first_byte_ms : m_limits.data_ms, error);
        if (got > 0)
        {
            m_first_byte = false;
            m_position += static_cast<std::uint64_t>(got);
            return got;
        }
        if (got == 0)
        {
            // The server ended the transfer; its reply says why
            m_net.close(m_data);
            m_data = -1;
            const reply r = read_reply(m_limits.reply_ms);
            m_end_code = r.code;
            if (r.code == 226) return 0;
            if (r.code > 0) fail(failure::failed, 0, r); // 451: the drive failed to read there
            else disconnect();
            return -1;
        }
        fail(m_stop.load(std::memory_order_acquire) ? failure::stopped : error == ETIMEDOUT ? failure::timeout : failure::closed, error);
        disconnect();
        return -1;
    }

    // Exactly 'size' bytes unless the stream ended (end_code()) or failed (last_failure()); returns the count.
    std::size_t read_full(void* buffer, std::size_t size)
    {
        std::size_t done = 0;
        while (done < size)
        {
            const std::int64_t got = read_some(static_cast<unsigned char*>(buffer) + done, size - done);
            if (got <= 0) break;
            done += static_cast<std::size_t>(got);
        }
        return done;
    }

    // Ends the transfer: closes the data connection and reads the server's reply (426, or 226/451 when it had ended).
    // false when no reply came: the control connection is closed too.
    bool close_stream()
    {
        if (!streaming()) return connected();
        m_net.close(m_data);
        m_data = -1;
        const reply r = read_reply(m_limits.reply_ms);
        m_end_code = r.code;
        if (r.code >= 200 && r.code < 600) return true;
        disconnect();
        return fail(r.code < 0 ? failure::protocol : m_failure == failure::none ? failure::timeout : m_failure, 0, r);
    }

    // NOOP on an idle control connection (keeps it open, proves it is alive). Not while a stream is open: the server
    // reads no command before the transfer ends. false: the connection is closed.
    bool noop()
    {
        if (!connected() || streaming()) return connected();
        const reply r = command("NOOP");
        if (r.code == 200) return true;
        drop(r.code ? failure::protocol : m_failure, r);
        return false;
    }

    // Orderly end: the stream, then QUIT (its reply is not waited for)
    void quit()
    {
        if (streaming()) close_stream();
        if (connected())
        {
            int error = 0;
            m_net.send(m_control, "QUIT\r\n", 6, 500, error);
        }
        disconnect();
    }

    void disconnect()
    {
        if (m_data >= 0) m_net.close(m_data);
        if (m_control >= 0) m_net.close(m_control);
        m_data = -1;
        m_control = -1;
        m_buffer.clear();
    }

private:
    const net_ops& m_net;
    const timeouts& m_limits;
    const std::atomic<bool>& m_stop;
    int m_control = -1;
    int m_data = -1;
    std::string m_buffer; // control bytes not taken yet
    endpoint m_server;
    std::string m_banner;
    std::uint64_t m_position = 0;
    bool m_first_byte = false;
    std::int64_t m_deadline = 0;
    std::int64_t m_connect_ms = 0;
    failure m_failure = failure::none;
    int m_error = 0;
    int m_code = 0;
    int m_end_code = 0;
    std::string m_reply;

    int limit(int timeout_ms) const noexcept
    {
        if (!m_deadline) return timeout_ms;
        const std::int64_t left = m_deadline - detail::now_ms();
        return static_cast<int>(std::max<std::int64_t>(0, std::min<std::int64_t>(timeout_ms, left)));
    }

    bool fail(failure kind, int error = 0, const reply& r = {})
    {
        m_failure = kind;
        m_error = error;
        m_code = r.code;
        m_reply = r.text;
        return false;
    }

    // A failure after which the control connection cannot be trusted
    bool drop(failure kind, const reply& r)
    {
        fail(kind == failure::none ? failure::protocol : kind, m_error, r);
        disconnect();
        return false;
    }

    std::int64_t recv_wait(int socket, void* buffer, std::size_t size, int timeout_ms, int& error)
    {
        const std::int64_t end = detail::now_ms() + limit(timeout_ms);
        for (;;)
        {
            if (m_stop.load(std::memory_order_acquire))
            {
                error = ECANCELED;
                return -1;
            }
            const std::int64_t left = end - detail::now_ms();
            const int slice = static_cast<int>(std::max<std::int64_t>(0, std::min<std::int64_t>(left, m_limits.slice_ms)));
            error = 0;
            const std::int64_t got = m_net.recv(socket, buffer, size, slice, error);
            if (got >= 0) return got;
            if (error != ETIMEDOUT || left <= slice) return -1;
        }
    }

    reply read_reply(int timeout_ms, std::string* lines = nullptr)
    {
        const std::int64_t end = detail::now_ms() + limit(timeout_ms);
        char chunk[512];
        for (;;)
        {
            if (auto r = take_reply(m_buffer, lines)) return *r;
            if (m_buffer.size() > 16384)
            {
                fail(failure::protocol);
                return {-1, "reply too long"};
            }
            const std::int64_t left = end - detail::now_ms();
            int error = 0;
            const std::int64_t got = recv_wait(m_control, chunk, sizeof(chunk), static_cast<int>(std::max<std::int64_t>(0, left)), error);
            if (got <= 0)
            {
                fail(got == 0 ? failure::closed : m_stop.load(std::memory_order_acquire) ? failure::stopped : error == ETIMEDOUT ? failure::timeout : failure::closed, error);
                return {};
            }
            m_buffer.append(chunk, static_cast<std::size_t>(got));
        }
    }

    reply command(const std::string& text)
    {
        if (!connected())
        {
            fail(failure::closed);
            return {};
        }
        const std::string line = text + "\r\n";
        int error = 0;
        if (m_net.send(m_control, line.data(), line.size(), limit(m_limits.reply_ms), error) != static_cast<std::int64_t>(line.size()))
        {
            fail(error == ETIMEDOUT ? failure::timeout : failure::closed, error);
            return {};
        }
        return read_reply(m_limits.reply_ms);
    }
};

// Servers tried in order; 'fallback' is asked once all of them refused, e.g. for the console's LAN address; 'last'
// is tried after the fallback (another FTP server's usual port).
struct server_list
{
    std::vector<endpoint> fixed;
    std::function<std::optional<endpoint>()> fallback;
    std::vector<endpoint> last;
};

enum class read_status
{
    ok,
    no_server,    // no FTP server answered
    refused,      // the server refused the device (550): no disc, or the drive cannot be opened
    failed,       // time-outs, dropped connections or read errors outlasted the retries
    stopped,      // stop() was called
    out_of_range, // at or past the end of the disc
    no_memory,
};

inline const char* to_string(read_status status)
{
    switch (status)
    {
    case read_status::ok: return "ok";
    case read_status::no_server: return "no FTP server";
    case read_status::refused: return "refused by the server";
    case read_status::failed: return "failed";
    case read_status::stopped: return "stopped";
    case read_status::out_of_range: return "past the end of the disc";
    case read_status::no_memory: return "no memory";
    }
    return "?";
}

struct reader_config
{
    std::uint32_t chunk_bytes = 256 * 1024;   // a multiple of 2048
    std::uint32_t cache_chunks = 32;          // 8 MiB
    std::uint32_t readahead_chunks = 1;       // fetched after a sequential miss, from the stream already there
    std::uint32_t cursors = 2;                // FTP connections; one more keeps a stream parked past a 2 GiB boundary
    std::uint64_t forward_skip_bytes = 1 << 20; // a stream this far behind is read forward instead of reopened
    unsigned attempts = 3;
    std::int64_t stats_interval_ms = 30000;
    bool log_features = false;                // FEAT of the first server reached, one line
    timeouts net{};
};

struct reader_stats
{
    std::uint64_t reads = 0;          // read() calls
    std::uint64_t cache_hits = 0;     // chunks served from the cache
    std::uint64_t cache_misses = 0;   // chunks fetched for a caller
    std::uint64_t readahead = 0;      // chunks fetched ahead
    std::uint64_t bytes = 0;          // bytes received for chunks
    std::uint64_t skipped = 0;        // bytes received only to move a stream forward
    std::uint64_t opens = 0;          // REST + RETR
    std::uint64_t floor_opens = 0;    // of those, started below a 2 GiB boundary
    std::uint64_t connects = 0;       // control connections made
    std::uint64_t errors = 0;         // failed attempts
};

// Thread-safe reads of the disc through the FTP server. Chunks of chunk_bytes are cached (LRU). A miss is served
// by a cursor (FTP connection) whose stream is at or shortly before the chunk, else one cursor starts a new transfer.
class reader
{
public:
    reader(net_ops net, server_list servers, reader_config cfg = {}, log_fn log = {})
        : m_net(std::move(net)), m_servers(std::move(servers)), m_cfg(sanitize(cfg)), m_log(std::move(log))
    {
        m_cursors.resize(m_cfg.cursors);
        m_slots.resize(m_cfg.cache_chunks);
    }

    ~reader()
    {
        stop();
    }

    reader(const reader&) = delete;
    reader& operator=(const reader&) = delete;

    // End of the disc (bytes); reads stop there. 0: unknown (only the server's end of stream applies).
    void set_size(std::uint64_t bytes) noexcept { m_size.store(bytes, std::memory_order_release); }
    std::uint64_t size() const noexcept { return m_size.load(std::memory_order_acquire); }
    const reader_config& config() const noexcept { return m_cfg; }

    // Bytes copied; fewer at the end of the disc or after a failure, which 'status' names.
    std::uint64_t read(std::uint64_t offset, void* buffer, std::uint64_t size, read_status* status = nullptr)
    {
        read_status result = read_status::ok;
        std::uint64_t done = 0;
        const std::uint64_t end = m_size.load(std::memory_order_acquire);
        m_reads.fetch_add(1, std::memory_order_relaxed);
        if (end && offset >= end && size) result = read_status::out_of_range;
        else if (end) size = std::min(size, end - offset);
        auto* out = static_cast<unsigned char*>(buffer);

        while (result == read_status::ok && done < size)
        {
            const std::uint64_t at = offset + done;
            const std::uint64_t index = at / m_cfg.chunk_bytes;
            const std::size_t within = static_cast<std::size_t>(at % m_cfg.chunk_bytes);
            const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(size - done, m_cfg.chunk_bytes - within));
            std::optional<std::size_t> got = copy_cached(index, within, out + done, want);
            if (got) m_hits.fetch_add(1, std::memory_order_relaxed);
            else
            {
                std::lock_guard io(m_io);
                got = copy_cached(index, within, out + done, want); // another thread may have fetched it meanwhile
                if (!got)
                {
                    result = fetch(index);
                    if (result == read_status::ok) got = copy_cached(index, within, out + done, want);
                }
                log_stats();
            }
            if (!got) break;
            done += *got;
            if (*got < want)
            {
                result = read_status::out_of_range; // the disc ended inside this chunk
                break;
            }
        }
        if (status) *status = result;
        return done;
    }

    // Reads [offset, offset + size) from the drive, not from the cache, and drops the cached chunks it covers.
    // offset and size: multiples of 2048, size at most one chunk. For the disc check (sectors 0-1). 'reply': the
    // server's reply code of the last failed attempt of this read (550, 451, ...), 0 when none came or it succeeded.
    read_status read_fresh(std::uint64_t offset, void* buffer, std::size_t size, int* reply = nullptr)
    {
        if (reply) *reply = 0;
        if (offset % sector_size || size % sector_size || size > m_cfg.chunk_bytes) return read_status::failed;
        std::lock_guard io(m_io);
        {
            std::lock_guard lock(m_cache_mutex);
            for (slot& s : m_slots)
                if (s.bytes && s.index * m_cfg.chunk_bytes < offset + size && (s.index + 1) * m_cfg.chunk_bytes > offset) s.bytes = 0;
        }
        {
            std::lock_guard lock(m_info_mutex);
            m_info_code = 0;
        }
        std::size_t got = 0;
        cursor* served = nullptr;
        const std::uint64_t window_bytes = m_window.bytes;
        // 451 for a new transfer at LBA 0 is the drive's own answer (no disc, or the tray moving): one attempt, not three
        const read_status result = fill(offset, static_cast<unsigned char*>(buffer), size, got, served, offset == 0);
        // Successful checks alone write no statistics line (a running game installed from a disc is checked every 3 s)
        m_window.bytes = window_bytes;
        log_stats();
        if (reply && result != read_status::ok)
        {
            std::lock_guard lock(m_info_mutex);
            *reply = m_info_code;
        }
        if (result != read_status::ok) return result;
        return got == size ? read_status::ok : read_status::out_of_range;
    }

    void drop_cache()
    {
        std::lock_guard lock(m_cache_mutex);
        for (slot& s : m_slots) s.bytes = 0;
    }

    // Ends every stream and connection (they are made again on the next read)
    void close_all()
    {
        std::lock_guard io(m_io);
        close_cursors();
    }

    // The same without waiting: false when a read holds the connections right now
    bool try_close_all()
    {
        std::unique_lock io(m_io, std::try_to_lock);
        if (!io.owns_lock()) return false;
        close_cursors();
        return true;
    }

    // Ends the open transfers and keeps the control connections. Does not wait for a read in progress: false when
    // one holds the connections right now.
    bool end_streams()
    {
        std::unique_lock io(m_io, std::try_to_lock);
        if (!io.owns_lock()) return false;
        for (cursor& c : m_cursors)
        {
            if (!c.ftp || !c.ftp->streaming()) continue;
            c.ftp->close_stream();
            c.last_ms = detail::now_ms();
        }
        return true;
    }

    // NOOP on the control connections idle (no stream) for idle_ms; a dead one is closed and made again by the next
    // read. Never waits: false when a read holds the connections right now.
    bool keepalive(std::int64_t idle_ms)
    {
        std::unique_lock io(m_io, std::try_to_lock);
        if (!io.owns_lock()) return false;
        const std::int64_t now = detail::now_ms();
        for (cursor& c : m_cursors)
        {
            if (!c.ftp || !c.ftp->connected() || c.ftp->streaming() || now - c.last_ms < idle_ms) continue;
            c.ftp->noop();
            c.last_ms = now;
        }
        return true;
    }

    // Permanent: waits in progress end within one slice and every later read fails with 'stopped'
    void stop()
    {
        m_stop.store(true, std::memory_order_release);
        std::lock_guard io(m_io);
        for (cursor& c : m_cursors)
            if (c.ftp) c.ftp->disconnect();
    }

    bool stopped() const noexcept { return m_stop.load(std::memory_order_acquire); }

    reader_stats stats() const
    {
        reader_stats s;
        s.reads = m_reads.load(std::memory_order_relaxed);
        s.cache_hits = m_hits.load(std::memory_order_relaxed);
        s.cache_misses = m_misses.load(std::memory_order_relaxed);
        s.readahead = m_readahead.load(std::memory_order_relaxed);
        s.bytes = m_bytes.load(std::memory_order_relaxed);
        s.skipped = m_skipped.load(std::memory_order_relaxed);
        s.opens = m_opens.load(std::memory_order_relaxed);
        s.floor_opens = m_floor_opens.load(std::memory_order_relaxed);
        s.connects = m_connects.load(std::memory_order_relaxed);
        s.errors = m_errors.load(std::memory_order_relaxed);
        return s;
    }

    // -1 not known yet, 0 the server starts transfers anywhere, 1 it refuses offsets with bit 31 set
    int floor_mode() const noexcept { return m_floor_mode.load(std::memory_order_acquire); }

    // Server and banner of the last connection made, and why the last failed attempt failed
    endpoint server() const
    {
        std::lock_guard lock(m_info_mutex);
        return m_info_server;
    }

    std::string banner() const
    {
        std::lock_guard lock(m_info_mutex);
        return m_info_banner;
    }

    std::string last_failure() const
    {
        std::lock_guard lock(m_info_mutex);
        return m_info_failure;
    }

private:
    struct cursor
    {
        std::unique_ptr<ftp_connection> ftp;
        std::uint64_t used = 0;
        std::int64_t last_ms = 0; // last command or data on this connection
        bool ever_connected = false;
    };

    struct slot
    {
        std::uint64_t index = 0;
        std::uint64_t used = 0;
        std::size_t bytes = 0; // 0: empty
        std::unique_ptr<unsigned char[]> data;
    };

    net_ops m_net;
    server_list m_servers;
    reader_config m_cfg;
    log_fn m_log;
    std::atomic<bool> m_stop{false};
    std::atomic<std::uint64_t> m_size{0};
    std::atomic<int> m_floor_mode{-1};

    std::mutex m_io; // every network operation, the cursors and the buffers below
    std::vector<cursor> m_cursors;
    std::unique_ptr<unsigned char[]> m_spare; // chunk being filled
    bool m_single_stream = false;
    std::uint64_t m_cursor_clock = 0;
    static constexpr std::uint64_t no_index = ~std::uint64_t{0};
    std::uint64_t m_last_index = no_index; // chunk of the last fetch, for read-ahead
    bool m_first_read_logged = false;
    bool m_floor_logged = false;
    bool m_server_down = false;
    std::optional<endpoint> m_fallback;
    bool m_fallback_asked = false;
    bool m_features_logged = false;
    std::int64_t m_reconnect_logged = 0;
    std::uint64_t m_reconnects_unlogged = 0;
    bool m_failing = false;            // a failure line since the last successful read
    std::int64_t m_failure_logged = 0; // time of the last failure line
    std::uint64_t m_failures_quiet = 0; // failure lines left out since

    // Activity since the last statistics line, and the totals it started from
    struct window
    {
        std::int64_t start = 0;
        std::uint64_t bytes = 0, skipped = 0, opens = 0, floor_opens = 0;
        reader_stats base{};
    } m_window;

    mutable std::mutex m_cache_mutex;
    std::vector<slot> m_slots;
    std::uint64_t m_cache_clock = 0;

    mutable std::mutex m_info_mutex;
    endpoint m_info_server{};
    std::string m_info_banner;
    std::string m_info_failure;
    int m_info_code = 0; // reply code of the last failure (0: none came, e.g. a time-out)

    std::atomic<std::uint64_t> m_reads{0}, m_hits{0}, m_misses{0}, m_readahead{0}, m_bytes{0}, m_skipped{0}, m_opens{0},
        m_floor_opens{0}, m_connects{0}, m_errors{0};

    static reader_config sanitize(reader_config cfg)
    {
        cfg.chunk_bytes = std::max<std::uint32_t>(static_cast<std::uint32_t>(sector_size), cfg.chunk_bytes - cfg.chunk_bytes % static_cast<std::uint32_t>(sector_size));
        cfg.cache_chunks = std::max<std::uint32_t>(2, cfg.cache_chunks);
        cfg.cursors = std::clamp<std::uint32_t>(cfg.cursors, 1, 4);
        cfg.attempts = std::max(1u, cfg.attempts);
        return cfg;
    }

    void log(const std::string& line) const
    {
        if (m_log) m_log("disc stream: " + line);
    }

    // Failure lines: the first one after a successful read, then at most one a minute with the count left out (a
    // drive without a disc is checked every few seconds)
    void log_failure(const std::string& line)
    {
        const std::int64_t now = detail::now_ms();
        if (m_failing && now - m_failure_logged < 60000)
        {
            ++m_failures_quiet;
            return;
        }
        log(line + (m_failures_quiet ? " (" + std::to_string(m_failures_quiet) + " similar line(s) left out)" : std::string{}));
        m_failing = true;
        m_failure_logged = now;
        m_failures_quiet = 0;
    }

    // Closes the sockets without waiting for any reply (the server ends a transfer and a session on a closed socket)
    void close_cursors()
    {
        for (cursor& c : m_cursors)
            if (c.ftp) c.ftp->disconnect();
        m_first_read_logged = false;
    }

    void note_failure(const std::string& text, int code = 0)
    {
        std::lock_guard lock(m_info_mutex);
        m_info_failure = text;
        m_info_code = code;
    }

    // Copies from a cached chunk; nullopt when it is not cached. Fewer bytes than asked: the chunk is the last one.
    std::optional<std::size_t> copy_cached(std::uint64_t index, std::size_t within, unsigned char* out, std::size_t want)
    {
        std::lock_guard lock(m_cache_mutex);
        for (slot& s : m_slots)
        {
            if (!s.bytes || s.index != index) continue;
            const std::size_t count = s.bytes > within ? std::min(want, s.bytes - within) : 0;
            if (count) std::memcpy(out, s.data.get() + within, count);
            s.used = ++m_cache_clock;
            return count;
        }
        return std::nullopt;
    }

    bool cached(std::uint64_t index)
    {
        std::lock_guard lock(m_cache_mutex);
        for (const slot& s : m_slots)
            if (s.bytes && s.index == index) return true;
        return false;
    }

    // Moves the filled m_spare into the cache; m_spare becomes the evicted (or a new) buffer. Under m_io.
    void insert(std::uint64_t index, std::size_t bytes)
    {
        std::lock_guard lock(m_cache_mutex);
        slot* target = nullptr;
        for (slot& s : m_slots)
            if (s.bytes && s.index == index) target = &s;
        if (!target)
            for (slot& s : m_slots)
                if (!s.data && !target) target = &s;
        if (!target)
            for (slot& s : m_slots)
                if (!target || !s.bytes || (target->bytes && s.used < target->used)) target = &s;
        std::swap(target->data, m_spare);
        target->index = index;
        target->bytes = bytes;
        target->used = ++m_cache_clock;
    }

    bool spare_ready()
    {
        if (!m_spare) m_spare.reset(new (std::nothrow) unsigned char[m_cfg.chunk_bytes]);
        return m_spare != nullptr;
    }

    // Fetches chunk 'index' into the cache, and the next ones when the reads are sequential. Under m_io.
    read_status fetch(std::uint64_t index)
    {
        const std::uint64_t start = index * m_cfg.chunk_bytes;
        const std::uint64_t end = m_size.load(std::memory_order_acquire);
        if (end && start >= end) return read_status::out_of_range;
        const std::size_t length = static_cast<std::size_t>(end ? std::min<std::uint64_t>(m_cfg.chunk_bytes, end - start) : m_cfg.chunk_bytes);
        if (!spare_ready()) return read_status::no_memory;
        m_misses.fetch_add(1, std::memory_order_relaxed);
        std::size_t got = 0;
        cursor* served = nullptr;
        const std::int64_t started = detail::now_ms();
        const read_status result = fill(start, m_spare.get(), length, got, served);
        if (result != read_status::ok) return result;
        if (!got) return read_status::out_of_range;
        insert(index, got);
        if (!m_first_read_logged)
        {
            m_first_read_logged = true;
            log("first read: LBA " + detail::hex(start / sector_size) + ", " + std::to_string(got / 1024) + " KiB in " +
                std::to_string(detail::now_ms() - started) + " ms");
        }

        // Read-ahead: the same stream goes on with the next chunks, which a sequential reader asks for next
        const bool sequential = m_last_index != no_index && index == m_last_index + 1;
        m_last_index = index;
        if (!sequential || got < length || !served) return read_status::ok;
        for (std::uint32_t ahead = 1; ahead <= m_cfg.readahead_chunks; ++ahead)
        {
            const std::uint64_t next = index + ahead;
            const std::uint64_t next_start = next * m_cfg.chunk_bytes;
            if ((end && next_start >= end) || cached(next) || !served->ftp || !served->ftp->streaming() ||
                served->ftp->position() != next_start || !spare_ready())
                break;
            const std::size_t next_length = static_cast<std::size_t>(end ? std::min<std::uint64_t>(m_cfg.chunk_bytes, end - next_start) : m_cfg.chunk_bytes);
            const std::size_t more = served->ftp->read_full(m_spare.get(), next_length);
            m_bytes.fetch_add(more, std::memory_order_relaxed);
            m_window.bytes += more;
            const bool disc_end = more && !served->ftp->streaming() && served->ftp->end_code() == 226;
            if (more != next_length && !disc_end)
            {
                // The stream failed: the next fetch of that chunk starts again
                m_errors.fetch_add(1, std::memory_order_relaxed);
                note_failure("read-ahead at LBA " + detail::hex((next_start + more) / sector_size) + ": " + served->ftp->failure_text(), served->ftp->last_code());
                log_failure("read-ahead at LBA " + detail::hex((next_start + more) / sector_size) + " failed (" + served->ftp->failure_text() + ")");
                if (served->ftp->connected()) served->ftp->close_stream();
                break;
            }
            insert(next, more);
            m_readahead.fetch_add(1, std::memory_order_relaxed);
            m_last_index = next;
            if (disc_end) break;
        }
        return read_status::ok;
    }

    // [start, start + length) into 'out' through a cursor, with retries. final_451: a transfer the server ended with
    // 451 is not tried again. Under m_io.
    read_status fill(std::uint64_t start, unsigned char* out, std::size_t length, std::size_t& got, cursor*& served, bool final_451 = false)
    {
        read_status result = read_status::failed;
        for (unsigned attempt = 0; attempt < m_cfg.attempts; ++attempt)
        {
            if (m_stop.load(std::memory_order_acquire)) return read_status::stopped;
            if (attempt && !detail::pause(std::min<std::int64_t>(1000, 200 * attempt), m_stop)) return read_status::stopped;
            cursor* c = nullptr;
            result = position(start, c);
            if (result != read_status::ok)
            {
                m_errors.fetch_add(1, std::memory_order_relaxed);
                if (result == read_status::stopped) return result;
                continue;
            }
            got = c->ftp->read_full(out, length);
            m_bytes.fetch_add(got, std::memory_order_relaxed);
            m_window.bytes += got;
            c->used = ++m_cursor_clock;
            c->last_ms = detail::now_ms();
            if (got == length || (!c->ftp->streaming() && c->ftp->end_code() == 226 && c->ftp->connected()))
            {
                served = c;
                m_failing = false;
                return read_status::ok; // complete, or the disc ended inside the range
            }
            m_errors.fetch_add(1, std::memory_order_relaxed);
            const std::string why = c->ftp->failure_text();
            note_failure("read at LBA " + detail::hex((start + got) / sector_size) + ": " + why, c->ftp->last_code());
            log_failure("read at LBA " + detail::hex((start + got) / sector_size) + " failed (" + why + "); attempt " + std::to_string(attempt + 1) +
                " of " + std::to_string(m_cfg.attempts));
            const int code = c->ftp->last_code();
            if (c->ftp->connected()) c->ftp->close_stream();
            got = 0;
            result = c->ftp->last_failure() == ftp_connection::failure::stopped ? read_status::stopped : read_status::failed;
            if (final_451 && code == 451) break;
        }
        return result;
    }

    // A cursor whose stream stands at 'target'. Under m_io.
    read_status position(std::uint64_t target, cursor*& out)
    {
        const bool floor_mode = m_floor_mode.load(std::memory_order_acquire) == 1;
        const bool past_boundary = floor_mode && rest_refused_at(target);

        // A stream at or a little before the target is read forward. Inside a range the server cannot start a
        // transfer in, any stream between the range's start and the target is as close as a new one could get.
        cursor* best = nullptr;
        std::uint64_t best_gap = 0;
        for (cursor& c : m_cursors)
        {
            if (!c.ftp || !c.ftp->streaming() || c.ftp->position() > target) continue;
            const std::uint64_t gap = target - c.ftp->position();
            const bool near = gap <= m_cfg.forward_skip_bytes || (past_boundary && c.ftp->position() >= rest_floor(target));
            if (near && (!best || gap < best_gap))
            {
                best = &c;
                best_gap = gap;
            }
        }
        if (best)
        {
            if (best_gap && !skip(*best, best_gap)) return read_status::failed;
            out = best;
            return read_status::ok;
        }

        cursor& c = pick_cursor();
        if (m_single_stream)
            for (cursor& other : m_cursors)
                if (&other != &c && other.ftp && other.ftp->streaming()) other.ftp->close_stream();
        const bool reused = c.ftp && c.ftp->connected();
        if (const read_status connected = connect(c); connected != read_status::ok) return connected;

        std::uint64_t start = past_boundary ? rest_floor(target) : target;
        bool reconnected = false;
        for (int round = 0;; ++round)
        {
            if (c.ftp->open_stream(start)) break;
            const ftp_connection::failure why = c.ftp->last_failure();
            if (why == ftp_connection::failure::stopped) return read_status::stopped;
            if (why != ftp_connection::failure::refused)
            {
                // A connection that sat idle may have been dropped by the server: one fresh connection first
                if (reused && !reconnected)
                {
                    reconnected = true;
                    c.ftp->disconnect();
                    if (const read_status again = connect(c); again != read_status::ok) return again;
                    continue;
                }
                note_failure("REST " + std::to_string(start) + " + RETR: " + c.ftp->failure_text(), c.ftp->last_code());
                log_failure("RETR at LBA " + detail::hex(start / sector_size) + " failed: " + c.ftp->failure_text());
                return read_status::failed;
            }
            if (round >= 3)
            {
                note_failure("REST " + std::to_string(start) + " + RETR: " + c.ftp->failure_text(), c.ftp->last_code());
                log_failure("RETR at LBA " + detail::hex(start / sector_size) + " refused: " + c.ftp->failure_text());
                return read_status::refused;
            }
            if (rest_refused_at(start) && m_floor_mode.load(std::memory_order_acquire) != 1)
            {
                m_floor_mode.store(1, std::memory_order_release);
                if (!std::exchange(m_floor_logged, true))
                    log("the server refuses to start a transfer at an offset with bit 31 set (" + c.ftp->last_reply() +
                        "): offsets in [2,4) GiB, [6,8) GiB, ... are reached by reading forward from the sector below each 2 GiB boundary");
                start = rest_floor(target);
                continue;
            }
            if (!m_single_stream && other_streaming(c))
            {
                // The server may not open the device twice at once: one stream from now on
                m_single_stream = true;
                log("RETR refused while another stream was open (" + c.ftp->last_reply() + "): using one stream at a time");
                for (cursor& other : m_cursors)
                    if (&other != &c && other.ftp && other.ftp->streaming()) other.ftp->close_stream();
                continue;
            }
            note_failure("REST " + std::to_string(start) + " + RETR: " + c.ftp->failure_text(), c.ftp->last_code());
            log_failure("RETR at LBA " + detail::hex(start / sector_size) + " refused: " + c.ftp->failure_text());
            return read_status::refused;
        }

        m_opens.fetch_add(1, std::memory_order_relaxed);
        m_window.opens++;
        if (rest_refused_at(start) && m_floor_mode.load(std::memory_order_acquire) == -1) m_floor_mode.store(0, std::memory_order_release);
        if (start != target)
        {
            m_floor_opens.fetch_add(1, std::memory_order_relaxed);
            m_window.floor_opens++;
            if (target - start >= (8u << 20))
                log("LBA " + detail::hex(target / sector_size) + " lies past a 2 GiB boundary the server cannot start at: reading " +
                    detail::mib(target - start) + " forward from LBA " + detail::hex(start / sector_size));
            if (!skip(c, target - start)) return read_status::failed;
        }
        out = &c;
        return read_status::ok;
    }

    bool other_streaming(const cursor& c) const
    {
        for (const cursor& other : m_cursors)
            if (&other != &c && other.ftp && other.ftp->streaming()) return true;
        return false;
    }

    // The cursor to start a new transfer on: the one whose stream is cheapest to give up (a stream past a 2 GiB
    // boundary took a long read-forward to get there), then the least recently used.
    cursor& pick_cursor()
    {
        if (m_single_stream) return m_cursors.front();
        const bool floor_mode = m_floor_mode.load(std::memory_order_acquire) == 1;
        cursor* best = nullptr;
        std::uint64_t best_value = 0;
        for (cursor& c : m_cursors)
        {
            std::uint64_t value = 0;
            if (c.ftp && c.ftp->streaming() && floor_mode && rest_refused_at(c.ftp->position()))
                value = c.ftp->position() - rest_floor(c.ftp->position());
            if (!best || value < best_value || (value == best_value && c.used < best->used))
            {
                best = &c;
                best_value = value;
            }
        }
        return *best;
    }

    // Control connection of a cursor: the configured servers in order, then the fallback, then the last ones. Under m_io.
    read_status connect(cursor& c)
    {
        if (!c.ftp) c.ftp = std::make_unique<ftp_connection>(m_net, m_cfg.net, m_stop);
        if (c.ftp->connected()) return read_status::ok;
        std::vector<endpoint> servers = m_servers.fixed;
        if (m_fallback) servers.push_back(*m_fallback);
        std::string tried;
        bool any_answered = false, last_added = false;
        for (std::size_t i = 0;; ++i)
        {
            if (i == servers.size())
            {
                if (any_answered) break;
                if (!m_fallback_asked && m_servers.fallback)
                {
                    m_fallback_asked = true;
                    m_fallback = m_servers.fallback();
                    if (m_fallback && std::find(servers.begin(), servers.end(), *m_fallback) == servers.end()) servers.push_back(*m_fallback);
                }
                if (i == servers.size() && !std::exchange(last_added, true))
                    for (const endpoint& e : m_servers.last)
                        if (std::find(servers.begin(), servers.end(), e) == servers.end()) servers.push_back(e);
                if (i == servers.size()) break;
            }
            if (m_stop.load(std::memory_order_acquire)) return read_status::stopped;
            if (c.ftp->connect(servers[i]))
            {
                // The server's feature list once (SITE CHMOD, REST...): which build of the server this console runs
                if (m_cfg.log_features && !std::exchange(m_features_logged, true))
                {
                    const std::optional<std::string> feat = c.ftp->features();
                    log("FEAT of " + to_string(servers[i]) + " (" + c.ftp->banner() + "): " + (!feat ? "no answer, connecting again" : feat->empty() ? "none listed" : *feat));
                    if (!feat && !c.ftp->connect(servers[i]))
                    {
                        any_answered = any_answered || c.ftp->last_failure() != ftp_connection::failure::connect;
                        tried += (tried.empty() ? "" : "; ") + to_string(servers[i]) + ": " + c.ftp->failure_text();
                        continue;
                    }
                }
                m_connects.fetch_add(1, std::memory_order_relaxed);
                {
                    std::lock_guard lock(m_info_mutex);
                    m_info_server = servers[i];
                    m_info_banner = c.ftp->banner();
                }
                // First connection of a cursor and the one after an outage: a line each; other reconnections at most
                // one line a minute, with their count
                const std::int64_t now = detail::now_ms();
                const std::string where = to_string(servers[i]) + " in " + std::to_string(c.ftp->connect_ms()) + " ms";
                if (std::exchange(m_server_down, false) || !c.ever_connected)
                    log(std::string(c.ever_connected ? "reconnected to " : "connected to ") + where + " (" + c.ftp->banner() + ")");
                else if (++m_reconnects_unlogged, now - m_reconnect_logged >= 60000)
                {
                    log("reconnected to " + where + " (" + std::to_string(m_reconnects_unlogged) + " reconnection(s) since the last line)");
                    m_reconnect_logged = now;
                    m_reconnects_unlogged = 0;
                }
                c.ever_connected = true;
                c.last_ms = now;
                return read_status::ok;
            }
            any_answered = any_answered || c.ftp->last_failure() != ftp_connection::failure::connect;
            tried += (tried.empty() ? "" : "; ") + to_string(servers[i]) + ": " + c.ftp->failure_text();
        }
        note_failure(tried.empty() ? std::string("no server configured") : tried);
        if (!std::exchange(m_server_down, true)) log("no FTP server: " + (tried.empty() ? std::string("none configured") : tried));
        return any_answered ? read_status::failed : read_status::no_server;
    }

    // Reads 'gap' bytes of a cursor's stream and drops them. Under m_io; uses m_spare.
    bool skip(cursor& c, std::uint64_t gap)
    {
        if (!spare_ready()) return false;
        const std::int64_t started = detail::now_ms();
        std::int64_t reported = started;
        std::uint64_t left = gap;
        while (left)
        {
            const std::size_t piece = static_cast<std::size_t>(std::min<std::uint64_t>(left, m_cfg.chunk_bytes));
            const std::size_t got = c.ftp->read_full(m_spare.get(), piece);
            m_skipped.fetch_add(got, std::memory_order_relaxed);
            m_window.skipped += got;
            left -= got;
            if (got != piece)
            {
                note_failure("reading forward: " + c.ftp->failure_text(), c.ftp->last_code());
                log_failure("reading forward stopped " + detail::mib(left) + " before LBA " + detail::hex(c.ftp->position() / sector_size + left / sector_size) +
                    " (" + c.ftp->failure_text() + ")");
                if (c.ftp->connected()) c.ftp->close_stream();
                return false;
            }
            const std::int64_t now = detail::now_ms();
            if (now - reported >= m_cfg.stats_interval_ms && m_cfg.stats_interval_ms > 0)
            {
                reported = now;
                log("reading forward: " + detail::mib(gap - left) + " of " + detail::mib(gap) + " in " + std::to_string((now - started) / 1000) + " s");
            }
        }
        c.used = ++m_cursor_clock;
        c.last_ms = detail::now_ms();
        return true;
    }

    // One line per interval with activity. Under m_io.
    void log_stats()
    {
        const std::int64_t now = detail::now_ms();
        if (!m_window.start) m_window.start = now;
        const std::int64_t span = now - m_window.start;
        if (span < m_cfg.stats_interval_ms || span <= 0) return;
        const reader_stats total = stats();
        const std::uint64_t hits = total.cache_hits - m_window.base.cache_hits, misses = total.cache_misses - m_window.base.cache_misses;
        const std::uint64_t connects = total.connects - m_window.base.connects, errors = total.errors - m_window.base.errors;
        if (m_window.bytes || m_window.skipped || misses || errors)
        {
            char rate[32];
            std::snprintf(rate, sizeof(rate), "%.2f MiB/s", static_cast<double>(m_window.bytes + m_window.skipped) / (1024.0 * 1024.0) / (static_cast<double>(span) / 1000.0));
            log("last " + std::to_string(span / 1000) + " s: " + detail::mib(m_window.bytes) + " read + " + detail::mib(m_window.skipped) +
                " read forward (" + rate + "), " + std::to_string(m_window.opens) + " transfers (" + std::to_string(m_window.floor_opens) +
                " below a 2 GiB boundary), cache " + std::to_string(hits) + " hits / " + std::to_string(misses) + " misses, " +
                std::to_string(connects) + " connections, " + std::to_string(errors) + " errors");
        }
        m_window = {};
        m_window.start = now;
        m_window.base = total;
    }
};

enum class disc_state
{
    unknown,   // not checked yet
    no_server, // no FTP server answered (GoldHEN FTP off)
    no_disc,   // the server refused or failed to read the drive (no disc, or not readable)
    not_ps3,   // a disc without the PS3 header
    ps3_disc,
};

inline const char* to_string(disc_state state)
{
    switch (state)
    {
    case disc_state::unknown: return "not checked";
    case disc_state::no_server: return "no FTP server";
    case disc_state::no_disc: return "no readable disc";
    case disc_state::not_ps3: return "not a PS3 disc";
    case disc_state::ps3_disc: return "PS3 disc";
    }
    return "?";
}

struct disc_status
{
    disc_state state = disc_state::unknown;
    std::string title_id;         // "BLES01876" (the raw header text when it is not a standard id)
    std::string title;            // TITLE of PS3_GAME/PARAM.SFO, "" when unreadable
    std::uint32_t plain_regions = 0;
    std::uint64_t size = 0;       // bytes, end of the last plain region
    bool key_present = false;
    std::string key_path;         // the key file found, else the one expected
    std::string installed;        // the installed image of this disc, "" when none (set by the drive's annotator)
    std::string partial;          // an interrupted install of this disc (.iso.part), "" when none
    std::uint64_t partial_bytes = 0; // bytes of it already safe on disk
    std::uint64_t identity = 0;   // FNV-1a of sectors 0-1 (region table and header), 0 when not a PS3 disc
    std::uint64_t fingerprint = 0; // disc_fingerprint(), 0 when the PVD or PARAM.SFO could not be read
    std::string sfo_title_id;     // TITLE_ID of PS3_GAME/PARAM.SFO, "" when unreadable
    std::string server;           // "127.0.0.1:2121"
    std::string banner;           // "GoldHEN FTP server v2.2"
    std::string detail;           // why a check failed
    read_status read = read_status::ok; // the read of sectors 0-1
    int reply = 0;                // when it failed: the server's last reply code (550, 451, ...), 0 when none came
    std::uint32_t generation = 0; // changes whenever another disc is seen
    std::int64_t checked_ms = 0;  // steady clock of the check, 0: never
    int net_link = -1;            // no_server: NetCtl's state at that check (netctl_ip_obtained: an IP address); -1 unknown
};

// No FTP server answered while the console had no IP address: GoldHEN's FTP server does not listen without one, so the
// drive stays invisible until the console joins a network (internet is not needed). False when the state is unknown.
inline bool no_network(const disc_status& s) noexcept
{
    return s.state == disc_state::no_server && s.net_link >= 0 && s.net_link != netctl_ip_obtained;
}

// Why no FTP server answered, for log lines: " (no network: netctl state 1)", " (network up: GoldHEN FTP off?)"
inline std::string net_clause(int link)
{
    if (link < 0) return " (network state unknown)";
    if (link != netctl_ip_obtained) return " (no network: netctl state " + std::to_string(link) + ")";
    return " (network up: GoldHEN FTP off?)";
}

// A failed check in which the server read the drive and said there is nothing to read: the device read failed (451:
// the console's answer for an empty drive, also while the tray moves) or the device ended before sector 2. A
// connection that failed, timed out or dropped says nothing about the disc, nor does 450 (file busy: the device may be
// in use by another transfer) or a refused RETR (550: the server could not open the device at that moment).
inline bool drive_answered_empty(const disc_status& s) noexcept
{
    if (s.state != disc_state::no_disc) return false;
    switch (s.read)
    {
    case read_status::out_of_range: return true;
    case read_status::failed: return s.reply == 451;
    default: return false;
    }
}

struct drive_config
{
    std::int64_t check_ms_searching = 5000; // no PS3 disc seen: sectors 0-1 are read again this often
    std::int64_t check_ms_present = 60000;  // a PS3 disc is in: checked this rarely, so the drive may spin down
    std::int64_t check_ms_other = 20000;    // a disc that is not a PS3 game: no need to keep the drive busy
    std::int64_t check_ms_idle = 30000;     // no PS3 disc seen while the XMB is left alone (set_idle): at most this often
    std::int64_t worker_idle_ms = 20000;    // the worker ends when poll() was not called for this long
    std::int64_t keepalive_ms = 30000;      // NOOP on control connections idle this long
};

// The disc in the drive, for the XMB and the disc watch of a running game. poll() returns the last status at once and
// keeps a worker thread checking while it is called; check_now() checks on the caller's thread. hold() keeps the drive
// to one user (an install).
class drive
{
public:
    using exists_fn = std::function<bool(const std::string& path)>;
    using annotate_fn = std::function<void(disc_status&)>; // adds what is installed of a PS3 disc (worker thread)
    using link_fn = std::function<int()>;                  // NetCtl state now (netctl_ip_obtained: an IP address), -1 unknown

    drive(std::unique_ptr<reader> disc, exists_fn exists, std::string key_dir, drive_config cfg = {}, log_fn log = {})
        : m_state(std::make_shared<state>())
    {
        m_state->disc = std::move(disc);
        m_state->exists = std::move(exists);
        m_state->key_dir = std::move(key_dir);
        m_state->cfg = cfg;
        m_state->log = std::move(log);
    }

    ~drive()
    {
        stop();
        wait_idle(10000);
    }

    drive(const drive&) = delete;
    drive& operator=(const drive&) = delete;

    reader& disc() noexcept { return *m_state->disc; }

    // Never waits for the network. Starts the worker when it is not running (start_worker false: only keeps a
    // running one alive, e.g. close to the thread limit).
    disc_status poll(bool start_worker = true)
    {
        state& s = *m_state;
        std::lock_guard lock(s.mutex);
        s.last_poll = detail::now_ms();
        if (start_worker && !s.worker && !s.stop)
        {
            s.worker = true;
            const std::shared_ptr<state> held = m_state;
            if (!run_detached([held] { worker(held); })) s.worker = false;
        }
        return s.status;
    }

    disc_status last() const
    {
        std::lock_guard lock(m_state->mutex);
        return m_state->status;
    }

    // The worker checks at once (e.g. the player selected the disc row). Asked while a check runs, another one follows
    // it: that check may have read the drive before the request.
    void request_check()
    {
        std::lock_guard lock(m_state->mutex);
        ++m_state->requests;
        m_state->next_check = 0;
        m_state->wake.notify_all();
    }

    // Blocking check on the caller's thread (the last status while the drive is held)
    disc_status check_now()
    {
        {
            std::lock_guard lock(m_state->mutex);
            if (m_state->held) return m_state->status;
        }
        return check(*m_state);
    }

    void set_annotator(annotate_fn fn)
    {
        std::lock_guard lock(m_state->mutex);
        m_state->annotate = std::move(fn);
    }

    // Asked when no FTP server answers, to tell a console without a network from an FTP server switched off
    // (disc_status::net_link); without it the state stays unknown
    void set_link_probe(link_fn fn)
    {
        std::lock_guard lock(m_state->mutex);
        m_state->link = std::move(fn);
    }

    // The XMB is left alone (its slowest idle tier): without a PS3 disc the drive is checked every check_ms_idle. Back
    // to active, the next such check is at most check_ms_searching away. Requested checks are never delayed.
    void set_idle(bool idle)
    {
        std::lock_guard lock(m_state->mutex);
        state& s = *m_state;
        if (s.idle == idle) return;
        s.idle = idle;
        if (!idle && s.status.state != disc_state::ps3_disc && s.next_check > 0)
        {
            s.next_check = std::min(s.next_check, detail::now_ms() + s.cfg.check_ms_searching);
            s.wake.notify_all();
        }
    }

    // Until release(): no check reads the drive and the reader's connections are closed. Waits for a check in
    // progress to end (bounded by the reader's time limits), so not for the XMB thread.
    void hold()
    {
        {
            std::lock_guard lock(m_state->mutex);
            m_state->held = true;
        }
        {
            std::lock_guard check_lock(m_state->check_mutex);
        }
        m_state->disc->close_all();
    }

    // The next check runs at once
    void release()
    {
        std::lock_guard lock(m_state->mutex);
        m_state->held = false;
        m_state->next_check = 0;
        m_state->wake.notify_all();
    }

    void stop()
    {
        {
            std::lock_guard lock(m_state->mutex);
            m_state->stop = true;
            m_state->wake.notify_all();
        }
        m_state->disc->stop();
    }

    bool wait_idle(std::int64_t timeout_ms)
    {
        std::unique_lock lock(m_state->mutex);
        return m_state->wake.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] { return !m_state->worker; });
    }

private:
    struct state
    {
        std::unique_ptr<reader> disc;
        exists_fn exists;
        std::string key_dir;
        drive_config cfg;
        log_fn log;
        std::mutex mutex;
        std::condition_variable wake;
        disc_status status; // [mutex]
        annotate_fn annotate; // [mutex]
        link_fn link;         // [mutex]
        bool held = false;    // [mutex]
        bool idle = false;    // [mutex] set_idle()
        bool worker = false;
        bool stop = false;
        std::int64_t last_poll = 0;
        std::int64_t next_check = 0;
        std::uint64_t requests = 0; // request_check() calls
        std::mutex check_mutex; // one check at a time; the fields below belong to it
        std::array<unsigned char, 2 * sector_size> identity{};
        bool has_identity = false;
        disc_status checked;
    };

    std::shared_ptr<state> m_state;

    static void worker(const std::shared_ptr<state>& held)
    {
        state& s = *held;
        for (;;)
        {
            bool due = false, busy = false;
            {
                std::unique_lock lock(s.mutex);
                const std::int64_t now = detail::now_ms();
                if (s.stop || now - s.last_poll > s.cfg.worker_idle_ms)
                {
                    s.worker = false;
                    s.wake.notify_all();
                    return;
                }
                busy = s.held;
                due = !busy && now >= s.next_check;
                if (!due) s.wake.wait_for(lock, std::chrono::milliseconds(busy ? 1000 : std::min<std::int64_t>(s.next_check - now, 1000)));
            }
            if (due) check(s);
            else if (!busy) s.disc->keepalive(s.cfg.keepalive_ms);
        }
    }

    static void log(const state& s, const std::string& line)
    {
        if (s.log) s.log("disc stream: " + line);
    }

    struct disc_info
    {
        std::string title, sfo_title_id;
        std::uint64_t fingerprint = 0;
    };

    // TITLE and TITLE_ID of PS3_GAME/PARAM.SFO through the ISO 9660 tree, and the fingerprint of the disc from the
    // PVD locate_files reads anyway (no extra transfer); empty / 0 when a step fails
    static disc_info read_info(reader& disc, const std::array<unsigned char, 2 * sector_size>& head)
    {
        std::array<unsigned char, sector_size> pvd{};
        bool pvd_read = false;
        const disc_files files = locate_files([&disc, &pvd, &pvd_read](std::uint64_t offset, void* out, std::size_t size)
        {
            const bool ok = disc.read(offset, out, size) == size;
            if (ok && offset == 16 * sector_size && size == pvd.size())
            {
                std::memcpy(pvd.data(), out, size);
                pvd_read = true;
            }
            return ok;
        }, false);
        disc_info info;
        info.title = sfo_string(files.param.data(), files.param.size(), "TITLE");
        if (info.title.size() > 96) info.title.resize(96);
        info.sfo_title_id = canonical_title_id(sfo_string(files.param.data(), files.param.size(), "TITLE_ID"));
        if (pvd_read && !files.param.empty())
            info.fingerprint = disc_fingerprint(head.data(), head.size(), pvd.data(), pvd.size(), files.param.data(), files.param.size());
        return info;
    }

    static disc_status check(state& s)
    {
        std::lock_guard check_lock(s.check_mutex);
        std::uint64_t requests = 0;
        {
            std::lock_guard lock(s.mutex);
            if (s.held) return s.status;
            requests = s.requests;
        }
        disc_status out;
        out.checked_ms = detail::now_ms();
        std::array<unsigned char, 2 * sector_size> head{};
        const read_status result = s.disc->read_fresh(0, head.data(), head.size(), &out.reply);
        out.read = result;
        out.server = to_string(s.disc->server());
        out.banner = s.disc->banner();
        out.generation = s.checked.generation;
        std::string failure;

        if (result == read_status::ok)
        {
            if (!s.has_identity || s.identity != head)
            {
                // Another disc (or the first one): nothing cached may be served for it
                if (s.has_identity) s.disc->drop_cache();
                s.identity = head;
                s.has_identity = true;
                out.generation = s.checked.generation + 1;
            }
            const layout table = parse_layout(head.data(), sector_size);
            const unsigned char* sector1 = head.data() + sector_size;
            if (!has_ps3_magic(sector1, sector_size) || !table.valid)
            {
                out.state = disc_state::not_ps3;
                out.detail = has_ps3_magic(sector1, sector_size) ? "no valid region table" : "no 'PlayStation3' header";
                s.disc->set_size(0);
            }
            else
            {
                out.state = disc_state::ps3_disc;
                out.title_id = title_id_from_header(sector1, sector_size);
                if (out.title_id.empty()) out.title_id = detail::printable(sector1 + 0x10, 0x10);
                out.plain_regions = table.plain_regions;
                out.size = table.size_bytes();
                s.disc->set_size(out.size);
                const bool same = out.generation == s.checked.generation && s.checked.state == disc_state::ps3_disc && !s.checked.title.empty();
                if (same)
                {
                    out.title = s.checked.title;
                    out.sfo_title_id = s.checked.sfo_title_id;
                    out.fingerprint = s.checked.fingerprint;
                }
                else
                {
                    disc_info info = read_info(*s.disc, head);
                    out.title = std::move(info.title);
                    out.sfo_title_id = std::move(info.sfo_title_id);
                    out.fingerprint = info.fingerprint;
                }
                // The ISO loader looks the key up by the image name, which is the title id (image_path)
                if (const std::string id = canonical_title_id(out.title_id); !id.empty())
                {
                    const std::string stem = s.key_dir + id;
                    out.key_path = stem + ".dkey";
                    if (s.exists && s.exists(stem + ".dkey")) out.key_present = true;
                    else if (s.exists && s.exists(stem + ".key"))
                    {
                        out.key_present = true;
                        out.key_path = stem + ".key";
                    }
                }
                out.identity = detail::fnv1a64(head.data(), head.size());
                annotate_fn annotate;
                {
                    std::lock_guard lock(s.mutex);
                    annotate = s.annotate;
                }
                if (annotate) annotate(out);
            }
        }
        else
        {
            if (s.has_identity) s.disc->drop_cache();
            s.has_identity = false;
            out.state = result == read_status::no_server ? disc_state::no_server : disc_state::no_disc;
            failure = s.disc->last_failure();
            out.detail = std::string(to_string(result)) + ": " + failure;
            if (result == read_status::stopped)
            {
                out.state = s.checked.state;
                out.net_link = s.checked.net_link;
            }
            else if (out.state == disc_state::no_server)
            {
                link_fn link;
                {
                    std::lock_guard lock(s.mutex);
                    link = s.link;
                }
                out.net_link = link ? link() : -1;
            }
        }

        // The drive is left without a transfer between checks; the control connections stay (NOOP keepalive)
        s.disc->end_streams();

        // A line when something changed
        const disc_status& before = s.checked;
        if (out.state != before.state || out.generation != before.generation || out.key_present != before.key_present || out.title != before.title ||
            out.installed != before.installed || out.partial != before.partial || out.net_link != before.net_link)
        {
            switch (out.state)
            {
            case disc_state::ps3_disc:
                log(s, "drive: PS3 disc " + out.title_id + (out.title.empty() ? std::string{} : " '" + out.title + "'") + ", " +
                    std::to_string(out.plain_regions) + " plain regions, " + detail::mib(out.size) + "; key " + out.key_path +
                    (out.key_present ? " present" : " missing") + " (" + out.server + ", " + out.banner + ")" +
                    (out.installed.empty() ? std::string{} : "; installed: " + out.installed) +
                    (out.partial.empty() ? std::string{} : "; interrupted install: " + out.partial + " (" + detail::mib(out.partial_bytes) + ")"));
                break;
            case disc_state::not_ps3: log(s, "drive: a disc that is not a PS3 game disc (" + out.detail + ")"); break;
            case disc_state::no_disc: log(s, "drive: no readable disc (" + out.detail + ")"); break;
            case disc_state::no_server: log(s, std::string("drive: ") + to_string(read_status::no_server) + net_clause(out.net_link) + ": " + failure); break;
            case disc_state::unknown: break;
            }
        }
        s.checked = out;

        std::lock_guard lock(s.mutex);
        s.status = out;
        // A request that came while this check ran is not lost: the next check runs at once
        const std::int64_t interval = out.state == disc_state::ps3_disc ? s.cfg.check_ms_present
            : out.state == disc_state::not_ps3 ? s.cfg.check_ms_other : s.cfg.check_ms_searching;
        s.next_check = s.requests != requests ? 0 : detail::now_ms() + (s.idle && out.state != disc_state::ps3_disc ? std::max(interval, s.cfg.check_ms_idle) : interval);
        return out;
    }
};

// Sectors 0-1 and 16 of /dev/cd0 through a connection of its own, for the disc probe. Everything ends by budget_ms.
struct header_read
{
    enum class result
    {
        ok,
        no_server, // no server took the connection
        refused,   // the server answered and refused (login, or RETR 550)
        failed,    // time-out, dropped connection or a short read
    };

    result kind = result::failed;
    endpoint server;       // the server that answered, or the last one tried
    std::string tried;     // each server tried and why it failed
    std::string banner;
    int error = 0;         // errno of the failed connection
    int code = 0;          // reply code of a refusal
    std::string reply;     // its text, or the failure
    std::vector<unsigned char> sectors; // sectors 0, 1 and 16 (3 * 2048 bytes) when kind is ok
    bool pvd_ok = false;   // sector 16 was read
    std::int64_t connect_ms = 0, header_ms = 0, pvd_ms = 0;
};

inline header_read read_header(const net_ops& net, const server_list& servers, const timeouts& limits, std::int64_t budget_ms)
{
    header_read out;
    const std::atomic<bool> never{false};
    ftp_connection ftp(net, limits, never);
    ftp.set_deadline(detail::now_ms() + budget_ms);
    std::vector<endpoint> list = servers.fixed;
    bool fallback_asked = false, last_added = false;
    bool connected = false;
    for (std::size_t i = 0; !connected; ++i)
    {
        if (i == list.size())
        {
            if (!fallback_asked && servers.fallback)
            {
                fallback_asked = true;
                if (const auto extra = servers.fallback(); extra && std::find(list.begin(), list.end(), *extra) == list.end()) list.push_back(*extra);
            }
            if (i == list.size() && !std::exchange(last_added, true))
                for (const endpoint& e : servers.last)
                    if (std::find(list.begin(), list.end(), e) == list.end()) list.push_back(e);
            if (i == list.size()) break;
        }
        out.server = list[i];
        connected = ftp.connect(list[i]);
        if (!connected)
        {
            out.tried += (out.tried.empty() ? "" : "; ") + to_string(list[i]) + ": " + ftp.failure_text();
            if (ftp.last_failure() != ftp_connection::failure::connect)
            {
                out.kind = ftp.last_failure() == ftp_connection::failure::login ? header_read::result::refused : header_read::result::failed;
                out.code = ftp.last_code();
                out.reply = ftp.failure_text();
                return out;
            }
            out.error = ftp.last_error();
        }
    }
    if (!connected)
    {
        out.kind = header_read::result::no_server;
        out.reply = out.tried;
        return out;
    }
    out.banner = ftp.banner();
    out.connect_ms = ftp.connect_ms();
    out.sectors.assign(3 * sector_size, 0);

    std::int64_t started = detail::now_ms();
    const bool header = ftp.open_stream(0) && ftp.read_full(out.sectors.data(), 2 * sector_size) == 2 * sector_size;
    out.header_ms = detail::now_ms() - started;
    if (!header)
    {
        out.kind = ftp.last_failure() == ftp_connection::failure::refused ? header_read::result::refused : header_read::result::failed;
        out.code = ftp.last_code() ? ftp.last_code() : ftp.end_code();
        out.reply = ftp.failure_text();
        out.sectors.clear();
        ftp.quit();
        return out;
    }
    ftp.close_stream();
    started = detail::now_ms();
    out.pvd_ok = ftp.open_stream(16 * sector_size) && ftp.read_full(out.sectors.data() + 2 * sector_size, sector_size) == sector_size;
    out.pvd_ms = detail::now_ms() - started;
    if (!out.pvd_ok) out.reply = ftp.failure_text();
    ftp.quit();
    out.kind = header_read::result::ok;
    return out;
}
}

#ifdef __ORBIS__
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wold-style-cast"
#endif
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#ifdef __clang__
#pragma clang diagnostic pop
#endif

namespace np
{
// Emu/NP/ip_address.cpp: SO_NBIO once the boot probe proved it (the kernel refuses fcntl and FIONBIO)
bool ps4_set_socket_non_blocking(int socket);
}

namespace utils::ps4::disc_stream::sys
{
// poll() for one socket: > 0 ready, 0 time-out, -1 error (errno)
inline int wait_for(int socket, short events, int timeout_ms)
{
    const std::int64_t end = detail::now_ms() + timeout_ms;
    for (;;)
    {
        pollfd p{};
        p.fd = socket;
        p.events = events;
        const std::int64_t left = std::max<std::int64_t>(0, end - detail::now_ms());
        const int ready = ::poll(&p, 1, static_cast<int>(left));
        if (ready >= 0 || errno != EINTR || left == 0) return ready;
    }
}

// Sockets as the port's network code uses them on this kernel: SO_NBIO (proven at boot), connect EINPROGRESS +
// poll(POLLOUT) + SO_ERROR, poll() before every recv/send and MSG_DONTWAIT on both. A socket the kernel left blocking
// is still bounded by SO_RCVTIMEO / SO_SNDTIMEO.
inline net_ops net()
{
    net_ops ops;
    ops.connect = [](const endpoint& to, int timeout_ms, int receive_buffer, int& error) -> int
    {
        const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0)
        {
            error = errno ? errno : EIO;
            return -1;
        }
        timeval bound{};
        bound.tv_sec = 5;
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &bound, sizeof(bound));
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &bound, sizeof(bound));
        const int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        if (receive_buffer > 0) ::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &receive_buffer, sizeof(receive_buffer));
        np::ps4_set_socket_non_blocking(fd);

        sockaddr_in addr{};
        addr.sin_len = sizeof(addr);
        addr.sin_family = AF_INET;
        const std::uint16_t port_be = static_cast<std::uint16_t>((to.port >> 8) | (to.port << 8));
        const unsigned char ip_be[4] = {static_cast<unsigned char>(to.ipv4 >> 24), static_cast<unsigned char>(to.ipv4 >> 16),
            static_cast<unsigned char>(to.ipv4 >> 8), static_cast<unsigned char>(to.ipv4)};
        addr.sin_port = port_be;
        std::memcpy(&addr.sin_addr, ip_be, 4);

        if (::connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) == 0) return fd;
        const int connect_error = errno;
        if (connect_error != EINPROGRESS && connect_error != EINTR && connect_error != EALREADY)
        {
            ::close(fd);
            error = connect_error ? connect_error : EIO;
            return -1;
        }
        const int ready = wait_for(fd, POLLOUT, timeout_ms);
        int so_error = 0;
        socklen_t length = sizeof(so_error);
        if (ready <= 0 || ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &length) != 0 || so_error)
        {
            error = ready == 0 ? ETIMEDOUT : so_error ? so_error : errno ? errno : EIO;
            ::close(fd);
            return -1;
        }
        return fd;
    };
    ops.send = [](int fd, const void* data, std::size_t size, int timeout_ms, int& error) -> std::int64_t
    {
        const std::int64_t end = detail::now_ms() + timeout_ms;
        std::size_t done = 0;
        while (done < size)
        {
            const int ready = wait_for(fd, POLLOUT, static_cast<int>(std::max<std::int64_t>(0, end - detail::now_ms())));
            if (ready <= 0)
            {
                error = ready == 0 ? ETIMEDOUT : errno ? errno : EIO;
                return -1;
            }
            const ssize_t sent = ::send(fd, static_cast<const char*>(data) + done, size - done, MSG_DONTWAIT);
            if (sent > 0) done += static_cast<std::size_t>(sent);
            else if (sent < 0 && errno != EAGAIN && errno != EINTR)
            {
                error = errno ? errno : EIO;
                return -1;
            }
        }
        return static_cast<std::int64_t>(done);
    };
    ops.recv = [](int fd, void* data, std::size_t size, int timeout_ms, int& error) -> std::int64_t
    {
        const std::int64_t end = detail::now_ms() + timeout_ms;
        for (;;)
        {
            const int ready = wait_for(fd, POLLIN, static_cast<int>(std::max<std::int64_t>(0, end - detail::now_ms())));
            if (ready < 0)
            {
                error = errno ? errno : EIO;
                return -1;
            }
            if (ready == 0)
            {
                error = ETIMEDOUT;
                return -1;
            }
            const ssize_t got = ::recv(fd, data, size, MSG_DONTWAIT);
            if (got >= 0) return got;
            if (errno != EAGAIN && errno != EINTR)
            {
                error = errno ? errno : EIO;
                return -1;
            }
            if (detail::now_ms() >= end)
            {
                error = ETIMEDOUT;
                return -1;
            }
        }
    };
    ops.close = [](int fd) { ::close(fd); };
    return ops;
}

// /data/rpcs3/disc-ftp.txt: 'host:port' of the FTP server (first line); empty when the file is absent or invalid
inline std::optional<endpoint> configured_server(std::string* problem = nullptr)
{
    const int fd = ::open(config_file, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return std::nullopt;
    char text[256]{};
    const ssize_t got = ::read(fd, text, sizeof(text) - 1);
    ::close(fd);
    const auto parsed = got > 0 ? parse_endpoint(std::string_view(text, static_cast<std::size_t>(got))) : std::nullopt;
    if (!parsed && problem) *problem = std::string(config_file) + " does not hold 'a.b.c.d:port'; using 127.0.0.1:2121";
    return parsed;
}
}
#endif

#ifdef RPCS3_PS4
#include "Utilities/File.h"
#include "util/ps4_build_version.hpp"
#include "util/ps4_net.hpp"

#include <ctime>

// Frontend object (ps4_launcher.cpp), always linked: one line in startup.log
void ps4_startup_checkpoint(const char* stage);

namespace rpcs3::utils
{
std::string get_redump_key_dir(); // Emu/system_utils.hpp
}

namespace utils::ps4::disc_stream
{
inline constexpr char device_name[] = "ps4_disc_drive_ftp";
inline constexpr char device_prefix[] = "/vfsv0_virtual_ps4_disc_drive_ftp"; // fs::get_virtual_device reads "ps4_disc_drive_ftp"

// The image path of a disc: <prefix>/<TITLE_ID>.iso. Booting it reads the disc through the FTP server; the ISO loader
// finds the key by that name in the redump folder (<TITLE_ID>.dkey).
inline std::string image_path(std::string_view title_id)
{
    return std::string(device_prefix) + "/" + (title_id.empty() ? std::string("disc") : std::string(title_id)) + ".iso";
}

namespace sys
{
// The console's own IPv4 (NetCtl), in case the server takes no loopback connection
inline std::optional<endpoint> lan_server(std::uint16_t port)
{
    const utils::ps4::net::status st = utils::ps4::net::prepare();
    if (!st.fn.netctl_get_info) return std::nullopt;
    alignas(8) char info[512]{};
    if (st.fn.netctl_get_info(14, info) < 0) return std::nullopt; // ORBIS_NET_CTL_INFO_IP_ADDRESS
    info[sizeof(info) - 1] = '\0';
    const auto ip = parse_ipv4(std::string_view(info, std::strlen(info)));
    if (!ip || !*ip || *ip == loopback_ipv4) return std::nullopt;
    return endpoint{*ip, port};
}

// disc-ftp.txt when present, else 127.0.0.1:2121 and, should the loopback be refused, the console's LAN address, then
// 127.0.0.1:1337 (the port of the FTP payloads other than GoldHEN's)
inline server_list servers(const log_fn& log = {})
{
    server_list list;
    std::string problem;
    if (const auto configured = configured_server(&problem))
    {
        list.fixed.push_back(*configured);
        return list;
    }
    if (!problem.empty() && log) log("disc stream: " + problem);
    list.fixed.push_back({loopback_ipv4, default_port});
    list.fallback = [] { return lan_server(default_port); };
    list.last.push_back({loopback_ipv4, 1337});
    return list;
}

// network_line() for now: NetCtl's state and the console clock against this build's date
inline std::string network_line_now()
{
    return network_line(utils::ps4::net::link_state(), static_cast<std::int64_t>(std::time(nullptr)), ps4_build_date);
}
}

class disc_file;

// The disc drive of this process: one reader and one status worker for the whole app.
class service
{
public:
    static service& instance()
    {
        // Never destroyed: detached workers may still hold it while the process ends
        static service* const s = new service();
        return *s;
    }

    // XMB: last status, never waits for the network (a worker re-reads sectors 0-1 while this keeps being called).
    // start_worker false: a stopped worker is not started again (close to the thread limit).
    disc_status poll(bool start_worker = true) { return m_drive->poll(start_worker); }
    void request_check() { m_drive->request_check(); }
    disc_status check_now() { return m_drive->check_now(); }
    disc_status last() const { return m_drive->last(); }
    reader& disc() { return m_drive->disc(); }

    // The path to boot when the last status is a PS3 disc, "" otherwise. Registers the virtual device.
    std::string boot_path()
    {
        const disc_status s = m_drive->last();
        if (s.state != disc_state::ps3_disc || !register_device()) return {};
        const std::string path = image_path(canonical_title_id(s.title_id));
        ps4_startup_checkpoint(("disc stream: boot path " + path + " (" + s.title_id + ", key " + (s.key_present ? "present" : "missing") + ")").c_str());
        return path;
    }

    // Ends the open transfers (e.g. back in the XMB after a game), so no server thread stays parked on the drive.
    // Never waits: false when a check holds the connections (they are released by the next call).
    bool release_streams() { return m_drive->disc().try_close_all(); }

    // The drive to one user (an install): see drive::hold(). Not for the XMB thread.
    void hold() { m_drive->hold(); }
    void release() { m_drive->release(); }
    void set_annotator(drive::annotate_fn fn) { m_drive->set_annotator(std::move(fn)); }
    void set_idle(bool idle) { m_drive->set_idle(idle); }

    void shutdown() { m_drive->stop(); }

private:
    std::unique_ptr<drive> m_drive;
    std::mutex m_device_mutex;
    bool m_device = false;

    service()
    {
        const log_fn log = [](const std::string& line)
        {
            ps4_startup_checkpoint(line.c_str());
            // The drive's state changes also go to RPCS3.log; no FTP server answering is followed by the network and the
            // clock (a console without an IP address, or with a clock no certificate is valid at)
            if (!line.starts_with("disc stream: drive: ")) return;
            mirror(line);
            if (line.starts_with("disc stream: drive: no FTP server"))
            {
                const std::string network = sys::network_line_now();
                ps4_startup_checkpoint(network.c_str());
                mirror(network);
            }
        };
        reader_config cfg;
        cfg.log_features = true;
        auto disc = std::make_unique<reader>(sys::net(), sys::servers(log), cfg, log);
        const auto exists = [](const std::string& path)
        {
            fs::stat_t info{};
            return fs::get_stat(path, info) && !info.is_directory;
        };
        m_drive = std::make_unique<drive>(std::move(disc), exists, rpcs3::utils::get_redump_key_dir(), drive_config{}, log);
        m_drive->set_link_probe([] { return utils::ps4::net::link_state(); });
    }

    inline bool register_device();
};

// The disc as one read-only file. Reads go to the shared reader; each file keeps its own position.
class disc_file final : public fs::file_base
{
public:
    disc_file(reader& disc, u64 size)
        : m_disc(disc), m_size(size)
    {
    }

    fs::stat_t get_stat() override
    {
        fs::stat_t info{};
        info.size = m_size;
        return info;
    }

    bool trunc(u64) override
    {
        fs::g_tls_error = fs::error::readonly;
        return false;
    }

    u64 read(void* buffer, u64 size) override
    {
        const u64 done = read_at(m_pos, buffer, size);
        m_pos += done;
        return done;
    }

    u64 read_at(u64 offset, void* buffer, u64 size) override
    {
        if (offset >= m_size || !size) return 0;
        const u64 count = std::min<u64>(size, m_size - offset);
        read_status status = read_status::ok;
        const u64 done = m_disc.read(offset, buffer, count, &status);
        if (done != count) fs::g_tls_error = fs::error::unknown;
        return done;
    }

    u64 write(const void*, u64) override
    {
        fs::g_tls_error = fs::error::readonly;
        return 0;
    }

    u64 seek(s64 offset, fs::seek_mode whence) override
    {
        const s64 base = whence == fs::seek_set ? 0 : whence == fs::seek_cur ? static_cast<s64>(m_pos) : static_cast<s64>(m_size);
        if (base + offset < 0)
        {
            fs::g_tls_error = fs::error::inval;
            return umax;
        }
        m_pos = static_cast<u64>(base + offset);
        return m_pos;
    }

    u64 size() override { return m_size; }

private:
    reader& m_disc;
    u64 m_size = 0;
    u64 m_pos = 0;
};

// "<prefix>/<TITLE_ID>.iso" for the disc in the drive (the last status), and the prefix itself as its folder
class disc_device final : public fs::device_base
{
public:
    disc_device() { fs_prefix = device_prefix; }

    bool stat(const std::string& path, fs::stat_t& info) override
    {
        info = {};
        if (is_root(path))
        {
            info.is_directory = true;
            return true;
        }
        const disc_status s = service::instance().last();
        if (!is_image(path, s))
        {
            fs::g_tls_error = fs::error::noent;
            return false;
        }
        info.size = s.size;
        return true;
    }

    bool statfs(const std::string& path, fs::device_stat& info) override
    {
        const disc_status s = service::instance().last();
        if (!is_root(path) && !is_image(path, s))
        {
            fs::g_tls_error = fs::error::noent;
            return false;
        }
        info = fs::device_stat{sector_size, s.size, 0, 0};
        return true;
    }

    std::unique_ptr<fs::file_base> open(const std::string& path, bs_t<fs::open_mode> mode) override
    {
        if (mode & (fs::write + fs::create + fs::trunc + fs::append))
        {
            fs::g_tls_error = fs::error::readonly;
            return nullptr;
        }
        const disc_status s = service::instance().last();
        if (!is_image(path, s))
        {
            fs::g_tls_error = is_root(path) ? fs::error::isdir : fs::error::noent;
            return nullptr;
        }
        return std::make_unique<disc_file>(service::instance().disc(), s.size);
    }

    std::unique_ptr<fs::dir_base> open_dir(const std::string& path) override
    {
        if (!is_root(path))
        {
            fs::g_tls_error = fs::error::notdir;
            return nullptr;
        }
        return std::make_unique<listing>(service::instance().last());
    }

private:
    class listing final : public fs::dir_base
    {
    public:
        explicit listing(const disc_status& s)
        {
            if (s.state == disc_state::ps3_disc)
            {
                m_entry.name = image_path(canonical_title_id(s.title_id)).substr(sizeof(device_prefix));
                m_entry.size = s.size;
                m_has = true;
            }
        }

        bool read(fs::dir_entry& entry) override
        {
            if (!m_has || m_read) return false;
            entry = m_entry;
            m_read = true;
            return true;
        }

        void rewind() override { m_read = false; }

    private:
        fs::dir_entry m_entry;
        bool m_has = false;
        bool m_read = false;
    };

    static bool is_root(std::string_view path)
    {
        if (!path.starts_with(device_prefix)) return false;
        path.remove_prefix(sizeof(device_prefix) - 1);
        while (!path.empty() && path.front() == '/') path.remove_prefix(1);
        return path.empty();
    }

    static bool is_image(std::string_view path, const disc_status& s)
    {
        return s.state == disc_state::ps3_disc && s.size && path == image_path(canonical_title_id(s.title_id));
    }
};

inline bool service::register_device()
{
    std::lock_guard lock(m_device_mutex);
    if (m_device) return true;
    m_device = !!fs::set_virtual_device(device_name, stx::make_shared<disc_device>());
    if (!m_device) ps4_startup_checkpoint("disc stream: the virtual device could not be registered");
    return m_device;
}
}
#endif
