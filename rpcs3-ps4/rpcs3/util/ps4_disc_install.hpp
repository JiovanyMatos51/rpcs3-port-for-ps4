#pragma once

// "Instalar disco de PS3": the PS3 disc in the PS4 drive copied to /data/rpcs3/games/<TITLE_ID> - <title>.iso through
// the console's own GoldHEN FTP server (ps4_disc_stream.hpp), the way the PS4 installs its own discs.
//
// One RETR from offset 0 streams the whole device: GoldHEN cannot start a transfer at an offset with bit 31 set, but
// reads straight through those ranges. The image goes to '<name>.iso.part', which is made durable (fsync) every
// 256 MiB; the durable size is then recorded in '<name>.iso.part.info'. An install that was cancelled or interrupted
// resumes: the transfer restarts at the last offset the server accepts at or below the durable size minus an overlap
// (the sector below the 2 GiB boundary when that point lies in a refused range), the overlap is compared with the
// file, and the copy goes on. Any later reconnection reads sectors 0-1 again, so another disc is never appended.
//
// With a disc key (<TITLE_ID>.dkey of the redump folder, or a registered key source) that decrypts LIC.DAT and
// EBOOT.BIN, the encrypted regions are decrypted while they are written (AES-128-CBC per sector, IV = LBA): the result
// is a decrypted image that keeps the region table, which the ISO loader plays without a key since its LIC.DAT and
// EBOOT.BIN carry their magic values in the clear. Without a key the image is written as it is on the disc and the
// loader needs <TITLE_ID>.dkey in the redump folder later. At the end sectors 0-1, PARAM.SFO and (decrypted) LIC.DAT
// and EBOOT.BIN are checked in the file, which is then renamed to '<name>.iso'. Right before that rename the lock
// record of the title (ps4_disc_lock.hpp) is written: the game then plays only with its disc in the drive.
//
// Writes only under the games folder and the lock folder; reads the disc only through FTP. Bounded memory (two 1 MiB
// buffers, a 2 MiB cache while the disc is checked). Log lines start with "disc install:".

#include "ps4_disc_stream.hpp"
#include "ps4_disc_lock.hpp"
#include "ps4_i18n.hpp"
#include "Crypto/aes.h"

#include <set>
#include <tuple>

namespace utils::ps4::disc_install
{
namespace ds = utils::ps4::disc_stream;

using key_bytes = std::array<unsigned char, 16>;

inline constexpr char default_games_dir[] = "/data/rpcs3/games/";
inline constexpr std::string_view lic_magic{"PS3LICDA", 8};
inline constexpr std::string_view eboot_magic{"SCE\0", 4};

namespace detail
{
inline bool ends_with_nocase(std::string_view text, std::string_view suffix) noexcept
{
    return text.size() >= suffix.size() && ds::detail::iequals(text.substr(text.size() - suffix.size()), suffix);
}

inline int hex_digit(char c) noexcept
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

inline std::string hex64(std::uint64_t value)
{
    char text[20];
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(value));
    return text;
}

// "12.3 MiB/s"
inline std::string rate_text(double bytes_per_s)
{
    char text[32];
    std::snprintf(text, sizeof(text), "%.2f MiB/s", bytes_per_s / (1024.0 * 1024.0));
    return text;
}
}

// A key file as the ISO loader reads it: 32 hex characters first (anything after them ignored), or exactly 16 bytes
inline std::optional<key_bytes> parse_key_file(const unsigned char* data, std::size_t size)
{
    key_bytes key{};
    if (size >= 32)
    {
        bool hex = true;
        for (std::size_t i = 0; i < 16 && hex; ++i)
        {
            const int high = detail::hex_digit(static_cast<char>(data[2 * i])), low = detail::hex_digit(static_cast<char>(data[2 * i + 1]));
            hex = high >= 0 && low >= 0;
            key[i] = static_cast<unsigned char>(high * 16 + low);
        }
        if (hex) return key;
    }
    if (size == 16)
    {
        std::memcpy(key.data(), data, 16);
        return key;
    }
    return std::nullopt;
}

// Non-zero fingerprint of a key (0 stands for "no key": the image as it is on the disc)
inline std::uint64_t key_id(const key_bytes& key) noexcept
{
    return ds::detail::fnv1a64(key.data(), key.size()) | 1;
}

// "<TITLE_ID> - <title>.iso": the title without what a file name cannot hold, cut to 64 bytes on a UTF-8 boundary;
// "<TITLE_ID>.iso" without a title.
inline std::string image_name(std::string_view title_id, std::string_view title)
{
    std::string clean;
    for (const char c : title)
    {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7f || std::string_view("/\\:*?\"<>|").find(c) != std::string_view::npos) continue;
        if (c == ' ' && (clean.empty() || clean.back() == ' ')) continue;
        clean += c;
    }
    if (clean.size() > 64)
    {
        std::size_t cut = 64;
        while (cut && (static_cast<unsigned char>(clean[cut]) & 0xc0) == 0x80) --cut;
        clean.resize(cut);
    }
    while (!clean.empty() && (clean.back() == ' ' || clean.back() == '.')) clean.pop_back();
    while (!clean.empty() && (clean.front() == '.' || clean.front() == ' ')) clean.erase(0, 1);
    return std::string(title_id) + (clean.empty() ? std::string{} : " - " + clean) + ".iso";
}

// A file name of this title: "<TITLE_ID>" (any case) followed by the end of the stem, a space, '-', '_' or '.'
inline bool names_title(std::string_view name, std::string_view title_id, std::string_view suffix)
{
    if (title_id.empty() || name.size() < title_id.size() + suffix.size() || !detail::ends_with_nocase(name, suffix)) return false;
    if (!ds::detail::iequals(name.substr(0, title_id.size()), title_id)) return false;
    const std::string_view rest = name.substr(title_id.size(), name.size() - title_id.size() - suffix.size());
    return rest.empty() || rest.front() == ' ' || rest.front() == '-' || rest.front() == '_' || rest.front() == '.';
}

// Whole sectors in place, AES-128-CBC with the IV of each sector built from its LBA (big endian in bytes 12-15).
// Not copyable: the AES context points into itself.
class sector_cipher
{
public:
    explicit sector_cipher(const key_bytes& key) noexcept { m_ok = aes_setkey_dec(&m_aes, key.data(), 128) == 0; }
    sector_cipher(const sector_cipher&) = delete;
    sector_cipher& operator=(const sector_cipher&) = delete;

    bool ok() const noexcept { return m_ok; }

    bool decrypt(std::uint64_t lba, unsigned char* data, std::uint64_t sectors) noexcept
    {
        for (std::uint64_t i = 0; i < sectors; ++i)
        {
            unsigned char iv[16]{};
            const std::uint32_t at = static_cast<std::uint32_t>(lba + i);
            iv[12] = static_cast<unsigned char>(at >> 24);
            iv[13] = static_cast<unsigned char>(at >> 16);
            iv[14] = static_cast<unsigned char>(at >> 8);
            iv[15] = static_cast<unsigned char>(at);
            unsigned char* sector = data + i * ds::sector_size;
            if (aes_crypt_cbc(&m_aes, AES_DECRYPT, ds::sector_size, iv, sector, sector) != 0) return false;
        }
        return true;
    }

private:
    aes_context m_aes{};
    bool m_ok = false;
};

// Whether 'lba' lies in an encrypted region (between two plain regions), and how many sectors from it (at most 'max')
// share that state
inline std::pair<bool, std::uint64_t> region_run(const ds::layout& table, std::uint64_t lba, std::uint64_t max) noexcept
{
    for (const auto& [first, last] : table.plain)
    {
        if (lba < first) return {true, std::min<std::uint64_t>(max, first - lba)};
        if (lba <= last) return {false, std::min<std::uint64_t>(max, std::uint64_t{last} + 1 - lba)};
    }
    return {false, max};
}

inline bool has_encrypted_regions(const ds::layout& table) noexcept
{
    return table.valid && table.plain.size() > 1;
}

// Whether 'key' turns the first 16 bytes of the sector at 'lba' (as on the disc) into 'magic'
inline bool key_opens(const key_bytes& key, std::uint64_t lba, const unsigned char* first16, std::string_view magic)
{
    unsigned char block[16];
    std::memcpy(block, first16, 16);
    unsigned char iv[16]{};
    iv[12] = static_cast<unsigned char>(lba >> 24);
    iv[13] = static_cast<unsigned char>(lba >> 16);
    iv[14] = static_cast<unsigned char>(lba >> 8);
    iv[15] = static_cast<unsigned char>(lba);
    aes_context aes{};
    if (aes_setkey_dec(&aes, key.data(), 128) != 0 || aes_crypt_cbc(&aes, AES_DECRYPT, 16, iv, block, block) != 0) return false;
    return std::memcmp(block, magic.data(), magic.size()) == 0;
}

// The file operations of the install (the console: POSIX under /data/rpcs3; the host test: memory)
struct file_ops
{
    // Read and write, created (0666) when 'create'; >= 0, or -1 with 'error' set
    std::function<int(const std::string& path, bool create, int& error)> open;
    // All 'size' bytes at 'offset': size; otherwise -1 with 'error' set (ENOSPC: the volume is full)
    std::function<std::int64_t(int fd, const void* data, std::size_t size, std::uint64_t offset, int& error)> write_at;
    // Bytes read (0 at the end), -1 with 'error' set
    std::function<std::int64_t(int fd, void* data, std::size_t size, std::uint64_t offset, int& error)> read_at;
    std::function<bool(int fd, std::uint64_t size, int& error)> truncate;
    std::function<bool(int fd, int& error)> sync;
    std::function<void(int fd)> close;
    std::function<std::int64_t(const std::string& path)> size; // -1 when absent
    std::function<bool(const std::string& from, const std::string& to, int& error)> rename; // replaces 'to'
    std::function<bool(const std::string& path)> remove;
    std::function<std::int64_t(const std::string& dir)> free_bytes;       // -1 when unknown
    std::function<std::vector<std::string>(const std::string& dir)> list; // file names
    // Creates one folder (0777) when it is missing; true when it is there. Optional (no call when empty).
    std::function<bool(const std::string& dir, int& error)> make_dir;
};

// What '<name>.iso.part.info' records
struct part_info
{
    std::string title_id;
    std::uint64_t size = 0;     // the image
    std::uint64_t identity = 0; // FNV-1a of sectors 0-1
    std::uint64_t key = 0;      // key_id() of the decryption key, 0: the image as on the disc
    std::uint64_t durable = 0;  // bytes of the .part made durable
};

inline std::string to_text(const part_info& info)
{
    return "rpcs3 disc install 1\ntitle_id " + info.title_id + "\nsize " + std::to_string(info.size) + "\nidentity " + detail::hex64(info.identity) +
        "\nkey " + detail::hex64(info.key) + "\ndurable " + std::to_string(info.durable) + "\n";
}

inline std::optional<part_info> parse_info(std::string_view text)
{
    const auto number = [](std::string_view value, int base, std::uint64_t& out)
    {
        if (value.empty() || value.size() > 20) return false;
        std::uint64_t v = 0;
        for (const char c : value)
        {
            const int d = base == 16 ? detail::hex_digit(c) : (c >= '0' && c <= '9' ? c - '0' : -1);
            if (d < 0) return false;
            v = v * static_cast<std::uint64_t>(base) + static_cast<std::uint64_t>(d);
        }
        out = v;
        return true;
    };
    part_info out;
    unsigned seen = 0;
    bool header = false;
    while (!text.empty())
    {
        const std::size_t eol = text.find('\n');
        std::string_view line = text.substr(0, eol);
        text.remove_prefix(eol == std::string_view::npos ? text.size() : eol + 1);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line == "rpcs3 disc install 1")
        {
            header = true;
            continue;
        }
        const std::size_t space = line.find(' ');
        if (space == std::string_view::npos) continue;
        const std::string_view name = line.substr(0, space), value = line.substr(space + 1);
        if (name == "title_id" && (out.title_id = ds::canonical_title_id(value), !out.title_id.empty())) seen |= 1;
        else if (name == "size" && number(value, 10, out.size)) seen |= 2;
        else if (name == "identity" && number(value, 16, out.identity)) seen |= 4;
        else if (name == "key" && number(value, 16, out.key)) seen |= 8;
        else if (name == "durable" && number(value, 10, out.durable)) seen |= 16;
    }
    if (!header || seen != 31) return std::nullopt;
    return out;
}

// What the games folder holds of a title: the installed image, an interrupted install and its durable bytes
struct presence
{
    std::string installed; // full path
    std::string partial;   // full path of the .iso.part
    std::uint64_t partial_bytes = 0;
};

inline std::optional<std::string> read_text(const file_ops& files, const std::string& path, std::size_t limit = 4096)
{
    int error = 0;
    const int fd = files.open(path, false, error);
    if (fd < 0) return std::nullopt;
    std::string text(limit, '\0');
    const std::int64_t got = files.read_at(fd, text.data(), text.size(), 0, error);
    files.close(fd);
    if (got < 0) return std::nullopt;
    text.resize(static_cast<std::size_t>(got));
    return text;
}

// The image path the lock record of 'title_id' in 'lock_dir' names ("" without a readable record of that title)
inline std::string recorded_image(const file_ops& files, const std::string& lock_dir, std::string_view title_id)
{
    if (lock_dir.empty() || title_id.empty()) return {};
    const auto text = read_text(files, lock_dir + disc_lock::file_name(title_id), disc_lock::record_limit + 1);
    const auto r = text ? disc_lock::parse(*text) : std::nullopt;
    return r && r->title_id == title_id ? r->path : std::string{};
}

// 'recorded': the image the title's lock record names (recorded_image). Only that file is the installed image; an ISO
// of the user whose name starts with the TITLE_ID is an ordinary ISO of the game, never replaced by an install.
inline presence find_presence(const file_ops& files, const std::string& games_dir, std::string_view title_id, const std::string& recorded = {})
{
    presence out;
    if (title_id.empty()) return out;
    if (!recorded.empty() && files.size && files.size(recorded) >= 0) out.installed = recorded;
    if (!files.list) return out;
    for (const std::string& name : files.list(games_dir))
        if (out.partial.empty() && names_title(name, title_id, ".iso.part")) out.partial = games_dir + name;
    if (!out.partial.empty())
    {
        const std::int64_t size = files.size(out.partial);
        const auto text = read_text(files, out.partial + ".info");
        const auto info = text ? parse_info(*text) : std::nullopt;
        if (size > 0 && info && info->title_id == title_id) out.partial_bytes = std::min<std::uint64_t>(info->durable, static_cast<std::uint64_t>(size));
    }
    return out;
}

// What a key source gets: the disc, its files, sectors 0-1 and random-access reads of the disc (slow past a 2 GiB
// boundary). The key it returns is tested on LIC.DAT and EBOOT.BIN before it is used.
struct key_request
{
    const ds::disc_status& disc;
    const ds::disc_files& files;
    const unsigned char* head; // sectors 0-1
    const ds::read_fn& read;
    const std::atomic<bool>& cancel; // the install was cancelled: return at once
    std::atomic<bool>& searching;    // set while a source waits for a key search (shown on the XMB)
};

// A key, with where it came from in 'source' (for the log; the key itself is never logged)
using key_source = std::function<std::optional<key_bytes>(const key_request& request, std::string& source)>;

enum class outcome
{
    done,
    cancelled,     // the .part stays; the next install of the disc resumes
    no_server,     // no FTP server answered (GoldHEN FTP off)
    no_disc,       // no readable PS3 disc (removed, or the drive refused it)
    disc_changed,  // another disc than the one selected
    no_space,      // not enough free space, before or while writing
    write_failed,  // the games folder refused a write, a sync or the rename
    read_failed,   // the drive or the connection failed past the retries
    verify_failed, // the finished image does not hold what it should (the next install starts over)
    busy,          // another install runs
};

inline const char* to_string(outcome kind)
{
    switch (kind)
    {
    case outcome::done: return "done";
    case outcome::cancelled: return "cancelled";
    case outcome::no_server: return "no FTP server";
    case outcome::no_disc: return "no readable PS3 disc";
    case outcome::disc_changed: return "another disc in the drive";
    case outcome::no_space: return "not enough free space";
    case outcome::write_failed: return "write failed";
    case outcome::read_failed: return "read failed";
    case outcome::verify_failed: return "verification failed";
    case outcome::busy: return "another install runs";
    }
    return "?";
}

enum class phase : int
{
    starting,
    checking,  // sectors 0-1, the file tree, the key
    resuming,  // reading what is already in the file again, to compare it
    copying,
    verifying, // the finished file
    finished,
};

// One install, shared between its thread and the XMB (which only reads it, and sets 'cancel')
class job
{
public:
    explicit job(ds::disc_status disc) : m_disc(std::move(disc)) {}

    const ds::disc_status& disc() const noexcept { return m_disc; }

    std::atomic<int> stage{static_cast<int>(phase::starting)};
    std::atomic<std::uint64_t> done{0};  // bytes of the image copied (or compared, while resuming)
    std::atomic<std::uint64_t> total{0}; // bytes of the image
    std::atomic<std::uint64_t> rate{0};  // bytes per second from the drive, smoothed
    std::atomic<std::uint64_t> need{0};  // free bytes asked for (no_space)
    std::atomic<bool> encrypted{false};  // the disc has encrypted regions
    std::atomic<bool> decrypting{false}; // they are decrypted into the image
    std::atomic<bool> key_search{false}; // waiting for the disc key search
    std::atomic<bool> cancel{false};     // Circle
    std::atomic<bool> finished{false};
    std::atomic<std::int64_t> unreadable_lba{-1}; // read_failed: the sector the drive failed to read every time, -1: none

    phase current() const noexcept { return static_cast<phase>(stage.load(std::memory_order_acquire)); }

    outcome result() const
    {
        std::lock_guard lock(m_mutex);
        return m_result;
    }

    std::string path() const // the image when done, else the .part (or "")
    {
        std::lock_guard lock(m_mutex);
        return m_path;
    }

    std::string detail() const // English, as logged
    {
        std::lock_guard lock(m_mutex);
        return m_detail;
    }

    void finish(outcome kind, std::string path, std::string detail)
    {
        {
            std::lock_guard lock(m_mutex);
            m_result = kind;
            m_path = std::move(path);
            m_detail = std::move(detail);
        }
        stage.store(static_cast<int>(phase::finished), std::memory_order_release);
        finished.store(true, std::memory_order_release);
    }

private:
    const ds::disc_status m_disc;
    mutable std::mutex m_mutex;
    outcome m_result = outcome::read_failed;
    std::string m_path;
    std::string m_detail;
};

struct install_config
{
    std::string games_dir = default_games_dir;    // ends with '/'
    std::string lock_dir = disc_lock::registry_dir; // the lock record of the title goes here; "" writes none
    std::uint32_t block_bytes = 1u << 20;         // one read, decrypt and write; a multiple of 2048
    std::uint64_t sync_bytes = 256ull << 20;      // fsync and .part.info this often
    std::uint64_t free_margin = 1ull << 30;       // free space asked beyond what is left to write
    std::uint64_t verify_overlap = 8ull << 20;    // compared with the file when an install resumes
    unsigned attempts = 4;                        // failed transfers in a row before the install gives up
    std::int64_t retry_pause_ms = 1000;           // doubled per failed attempt, at most 8 s
    std::int64_t log_interval_ms = 30000;
    ds::timeouts net{};
};

struct environment
{
    ds::net_ops net;
    ds::server_list servers;
    file_ops files;
    std::vector<key_source> keys; // asked in order
    install_config cfg;
    ds::log_fn log;
    // Optional: opens a folder that refused a write (EACCES/EPERM) at once; true when it takes writes now. The write is
    // then tried once more.
    std::function<bool(const std::string& folder)> reopen;
};

// Runs one install on the calling thread; the result is in the job when it returns.
class installer
{
public:
    installer(const environment& env, job& j)
        : m_env(env), m_job(j), m_cfg(sanitize(env.cfg)), m_ftp(env.net, m_cfg.net, j.cancel)
    {
    }

    ~installer() { close_file(); }

    installer(const installer&) = delete;
    installer& operator=(const installer&) = delete;

    void run()
    {
        const outcome kind = install();
        m_ftp.quit();
        if (m_fd >= 0 && kind != outcome::done && kind != outcome::verify_failed)
        {
            // What was written becomes the resume point of the next install
            if (commit()) log("kept " + ds::detail::mib(m_durable) + " of " + ds::detail::mib(m_size) + " in " + m_part + " for the next install");
        }
        close_file();
        if (kind != outcome::done && kind != outcome::cancelled) log("failed: " + std::string(to_string(kind)) + ": " + m_detail);
        m_job.finish(kind, kind == outcome::done ? m_final : m_part, m_detail);
    }

private:
    struct magic_block
    {
        std::uint64_t lba = 0;
        std::string_view magic;
        const char* name = "";
    };

    // How a transfer could not be opened
    enum class open_kind
    {
        ok,
        retry,  // worth another attempt; 'hint' says what the failure means when the attempts run out
        fatal,
        cancelled,
    };

    enum class step
    {
        ok,
        restart, // the copy starts over from LBA 0
        error,
    };

    const environment& m_env;
    job& m_job;
    const install_config m_cfg;
    ds::ftp_connection m_ftp;

    std::string m_id, m_final, m_part, m_info, m_detail;
    std::uint64_t m_size = 0;
    std::uint64_t m_identity = 0;
    std::array<unsigned char, 2 * ds::sector_size> m_head{};
    std::array<unsigned char, ds::sector_size> m_pvd{}; // sector 16 of the finished file (the lock record's fingerprint)
    bool m_pvd_read = false;
    std::vector<unsigned char> m_param;                 // PARAM.SFO of the finished file
    ds::layout m_layout;
    std::vector<magic_block> m_magics;
    std::unique_ptr<sector_cipher> m_cipher;
    std::uint64_t m_key_id = 0;
    std::string m_key_source;
    int m_fd = -1;
    std::unique_ptr<unsigned char[]> m_buffer, m_compare;

    std::uint64_t m_pos = 0;        // next byte of the image to take from the stream
    std::uint64_t m_verify_end = 0; // bytes below this are already in the file: compared, not written
    std::uint64_t m_durable = 0;    // bytes made durable and recorded
    int m_floor_mode = -1;          // -1 unknown, 0 the server starts anywhere, 1 not at offsets with bit 31 set
    unsigned m_transfers = 0;
    unsigned m_restarts = 0;
    bool m_connected_logged = false;
    std::set<std::string> m_reopen_tried; // folders opened through the environment once
    outcome m_error = outcome::write_failed;

    std::int64_t m_started = 0, m_rate_at = 0, m_logged_at = 0;
    std::uint64_t m_received = 0, m_rate_base = 0, m_logged_pos = 0, m_copied = 0;
    double m_rate = 0;

    static install_config sanitize(install_config cfg)
    {
        cfg.block_bytes = std::max<std::uint32_t>(static_cast<std::uint32_t>(ds::sector_size), cfg.block_bytes - cfg.block_bytes % static_cast<std::uint32_t>(ds::sector_size));
        cfg.sync_bytes = std::max<std::uint64_t>(cfg.sync_bytes, cfg.block_bytes);
        cfg.verify_overlap -= cfg.verify_overlap % ds::sector_size;
        cfg.attempts = std::max(1u, cfg.attempts);
        if (cfg.games_dir.empty() || cfg.games_dir.back() != '/') cfg.games_dir += '/';
        if (!cfg.lock_dir.empty() && cfg.lock_dir.back() != '/') cfg.lock_dir += '/';
        return cfg;
    }

    void log(const std::string& line) const
    {
        if (m_env.log) m_env.log("disc install: " + line);
    }

    outcome fail(outcome kind, std::string detail)
    {
        m_detail = std::move(detail);
        return kind;
    }

    bool cancelled() const noexcept { return m_job.cancel.load(std::memory_order_acquire); }

    // A refused permission in 'folder' (ends with '/'): opened at once through the environment, once per folder and install
    bool reopened(const std::string& folder, int error)
    {
        const std::string path = folder.size() > 1 && folder.back() == '/' ? folder.substr(0, folder.size() - 1) : folder;
        if ((error != EACCES && error != EPERM) || !m_env.reopen || !m_reopen_tried.insert(path).second) return false;
        const bool opened = m_env.reopen(path);
        log(path + " refused a write (" + ds::detail::errno_text(error) + "): " + (opened ? "opened, trying again" : "still closed"));
        return opened;
    }

    // The image name of a new install, " (disco)" / " (disco N)" before ".iso" when a file of that name is already in the
    // games folder (an ISO the player copied, which the install must not replace)
    std::string unused_image_path(const std::string& id, std::string_view title) const
    {
        const std::string name = image_name(id, title), stem = m_cfg.games_dir + name.substr(0, name.size() - 4);
        const auto taken = [this](const std::string& path) { return m_env.files.size && (m_env.files.size(path) >= 0 || m_env.files.size(path + ".part") >= 0); };
        if (!taken(stem + ".iso")) return stem + ".iso";
        for (int n = 1; n <= 9; ++n)
        {
            const std::string path = stem + (n == 1 ? std::string(" (disco)") : " (disco " + std::to_string(n) + ")") + ".iso";
            if (!taken(path))
            {
                log("'" + stem + ".iso' is a file that was not installed from the disc: the install writes '" + path + "'");
                return path;
            }
        }
        return stem + ".iso";
    }

    std::uint64_t lba(std::uint64_t offset) const noexcept { return offset / ds::sector_size; }

    void close_file()
    {
        if (m_fd >= 0) m_env.files.close(m_fd);
        m_fd = -1;
    }

    std::uint64_t written_end() const noexcept { return std::max(m_pos, m_verify_end); }

    bool write_info()
    {
        const part_info info{m_id, m_size, m_identity, m_key_id, m_durable};
        const std::string text = to_text(info), temp = m_info + ".tmp";
        int error = 0;
        const int fd = m_env.files.open(temp, true, error);
        bool ok = fd >= 0 && m_env.files.truncate(fd, 0, error) &&
            m_env.files.write_at(fd, text.data(), text.size(), 0, error) == static_cast<std::int64_t>(text.size()) && m_env.files.sync(fd, error);
        if (fd >= 0) m_env.files.close(fd);
        ok = ok && m_env.files.rename(temp, m_info, error);
        if (!ok)
        {
            m_error = error == ENOSPC ? outcome::no_space : outcome::write_failed;
            m_detail = "writing " + m_info + ": " + ds::detail::errno_text(error);
        }
        return ok;
    }

    // fsync, then the durable size into .part.info
    bool commit()
    {
        int error = 0;
        if (!m_env.files.sync(m_fd, error))
        {
            m_error = error == ENOSPC ? outcome::no_space : outcome::write_failed;
            m_detail = "fsync " + m_part + ": " + ds::detail::errno_text(error);
            return false;
        }
        m_durable = std::max(m_durable, written_end());
        return write_info();
    }

    bool read_file(std::uint64_t offset, void* out, std::size_t size)
    {
        std::size_t done = 0;
        while (done < size)
        {
            int error = 0;
            const std::int64_t got = m_env.files.read_at(m_fd, static_cast<unsigned char*>(out) + done, size - done, offset + done, error);
            if (got <= 0) return false;
            done += static_cast<std::size_t>(got);
        }
        return true;
    }

    outcome install()
    {
        m_started = m_rate_at = m_logged_at = ds::detail::now_ms();
        const ds::disc_status& disc = m_job.disc();
        m_id = ds::canonical_title_id(disc.title_id);
        if (disc.state != ds::disc_state::ps3_disc || m_id.empty() || !disc.size || disc.size % ds::sector_size)
            return fail(outcome::no_disc, "no PS3 disc with a title id was selected");
        m_size = disc.size;
        m_job.total.store(m_size, std::memory_order_release);

        // An interrupted install of this disc keeps its file name; the image its lock record names is replaced at the
        // end; a new install never takes the name of a file that is already there (an ISO the player copied)
        const presence found = find_presence(m_env.files, m_cfg.games_dir, m_id, recorded_image(m_env.files, m_cfg.lock_dir, m_id));
        m_final = !found.partial.empty() ? found.partial.substr(0, found.partial.size() - 5)
            : !found.installed.empty() ? found.installed : unused_image_path(m_id, disc.title);
        m_part = m_final + ".part";
        m_info = m_part + ".info";
        log("start: " + m_id + (disc.title.empty() ? std::string{} : " '" + disc.title + "'") + " -> " + m_final + " (" + ds::detail::mib(m_size) + ")");

        m_job.stage.store(static_cast<int>(phase::checking), std::memory_order_release);
        if (const outcome checked = check_disc(); checked != outcome::done) return checked;
        if (cancelled())
        {
            log("cancelled before the copy");
            return fail(outcome::cancelled, "cancelled");
        }

        // A .part of this disc made with the same key goes on; anything else starts over
        std::uint64_t durable = 0;
        const std::int64_t part_size = m_env.files.size(m_part);
        if (part_size >= 0)
        {
            const auto text = read_text(m_env.files, m_info);
            const auto info = text ? parse_info(*text) : std::nullopt;
            std::string why;
            if (!info) why = "no valid " + m_info;
            else if (info->title_id != m_id || info->size != m_size || info->identity != m_identity) why = "it was made from another disc";
            else if (info->key != m_key_id)
                why = !info->key ? "it holds the image as it is on the disc" : m_key_id ? "it was decrypted with another key" : "it was decrypted with a key not found now";
            else durable = std::min<std::uint64_t>({info->durable, static_cast<std::uint64_t>(part_size), m_size}) / ds::sector_size * ds::sector_size;
            if (!why.empty()) log("the interrupted install " + m_part + " cannot go on (" + why + "): starting over");
        }

        // The bytes of the .part past 'durable' are dropped below (truncate): their space is free for the copy
        const std::uint64_t dropped = part_size > 0 ? static_cast<std::uint64_t>(part_size) - std::min(durable, static_cast<std::uint64_t>(part_size)) : 0;
        const std::uint64_t need = m_size - durable + m_cfg.free_margin;
        m_job.need.store(need - std::min(need, dropped), std::memory_order_release);
        const std::int64_t free = m_env.files.free_bytes ? m_env.files.free_bytes(m_cfg.games_dir) : -1;
        if (free >= 0 && static_cast<std::uint64_t>(free) + dropped < need)
            return fail(outcome::no_space, m_cfg.games_dir + " has " + ds::detail::mib(static_cast<std::uint64_t>(free)) + " free" +
                (dropped ? " (+ " + ds::detail::mib(dropped) + " of " + m_part + " dropped)" : std::string{}) + ", the install needs " + ds::detail::mib(need));
        if (free < 0) log("free space of " + m_cfg.games_dir + " unknown: going on");

        int error = 0;
        m_fd = m_env.files.open(m_part, true, error);
#ifdef EILSEQ
        const bool bad_name = error == EINVAL || error == ENAMETOOLONG || error == EILSEQ;
#else
        const bool bad_name = error == EINVAL || error == ENAMETOOLONG;
#endif
        if (m_fd < 0 && bad_name && m_final != m_cfg.games_dir + m_id + ".iso" && !(m_env.files.size && m_env.files.size(m_cfg.games_dir + m_id + ".iso") >= 0))
        {
            // A title the file system does not take in a name (characters, length): the bare title id instead
            log("open " + m_part + ": " + ds::detail::errno_text(error) + "; using " + m_id + ".iso instead");
            m_final = m_cfg.games_dir + m_id + ".iso";
            m_part = m_final + ".part";
            m_info = m_part + ".info";
            durable = 0; // nothing of this name was written (an earlier one would have been found as the .part)
            error = 0;
            m_fd = m_env.files.open(m_part, true, error);
        }
        if (m_fd < 0 && reopened(m_cfg.games_dir, error))
        {
            error = 0;
            m_fd = m_env.files.open(m_part, true, error);
        }
        if (m_fd < 0) return fail(error == ENOSPC ? outcome::no_space : outcome::write_failed, "open " + m_part + ": " + ds::detail::errno_text(error));
        m_durable = durable; // before anything can fail: run() records it for the next install
        if (!m_env.files.truncate(m_fd, durable, error))
            return fail(outcome::write_failed, "truncate " + m_part + " to " + std::to_string(durable) + ": " + ds::detail::errno_text(error));
        if (!write_info()) return m_error;

        m_buffer.reset(new (std::nothrow) unsigned char[m_cfg.block_bytes]);
        m_compare.reset(new (std::nothrow) unsigned char[m_cfg.block_bytes]);
        if (!m_buffer || !m_compare) return fail(outcome::read_failed, "no memory for the transfer buffers");

        if (durable)
        {
            m_pos = (durable - std::min(durable, m_cfg.verify_overlap)) / ds::sector_size * ds::sector_size;
            m_verify_end = durable;
            log("resume: " + ds::detail::mib(durable) + " of " + ds::detail::mib(m_size) + " already written; the copy restarts at LBA " +
                ds::detail::hex(lba(m_pos)) + " and compares what is already in the file first");
        }
        log(std::string("free ") + (free >= 0 ? ds::detail::mib(static_cast<std::uint64_t>(free)) : std::string("unknown")) + ", " +
            (m_cipher ? "decrypting with the key from " + m_key_source : has_encrypted_regions(m_layout) ? std::string("writing the image as it is on the disc (no key)")
                : std::string("the disc has no encrypted region")));

        if (const outcome copied = copy(); copied != outcome::done) return copied;
        return finish();
    }

    // Sectors 0-1, the file tree and the key, through random-access reads of a reader of its own
    outcome check_disc()
    {
        ds::reader_config rc;
        rc.cursors = 1;
        rc.cache_chunks = 8;
        rc.readahead_chunks = 0;
        rc.attempts = 2;
        rc.stats_interval_ms = std::int64_t{1} << 40;
        rc.net = m_cfg.net;
        ds::reader disc(m_env.net, m_env.servers, rc, m_env.log);

        ds::read_status status{};
        if (disc.read(0, m_head.data(), m_head.size(), &status) != m_head.size())
        {
            const std::string why = std::string(ds::to_string(status)) + ": " + disc.last_failure();
            return fail(status == ds::read_status::no_server ? outcome::no_server : outcome::no_disc, "sectors 0-1: " + why);
        }
        m_identity = ds::detail::fnv1a64(m_head.data(), m_head.size());
        m_layout = ds::parse_layout(m_head.data(), ds::sector_size);
        const unsigned char* sector1 = m_head.data() + ds::sector_size;
        if (!m_layout.valid || ds::title_id_from_header(sector1, ds::sector_size) != m_id || m_layout.size_bytes() != m_size ||
            (m_job.disc().identity && m_job.disc().identity != m_identity))
        {
            disc.close_all();
            return fail(outcome::disc_changed, "the disc in the drive is not " + m_id + " (" + std::to_string(m_size) + " bytes) any more");
        }
        disc.set_size(m_size);
        m_job.encrypted.store(has_encrypted_regions(m_layout), std::memory_order_release);

        const ds::read_fn read = [&disc](std::uint64_t offset, void* out, std::size_t size) { return disc.read(offset, out, size) == size; };
        const ds::disc_files files = ds::locate_files(read);
        if (!files.tree) log("the ISO 9660 tree could not be read (" + disc.last_failure() + ")");
        for (const auto& [file, magic, name] : {std::tuple{files.lic_dat, lic_magic, "LIC.DAT"}, std::tuple{files.eboot, eboot_magic, "EBOOT.BIN"}})
            if (file && file->bytes >= 16 && region_run(m_layout, file->lba, 1).first) m_magics.push_back({file->lba, magic, name});

        if (has_encrypted_regions(m_layout) && !m_magics.empty())
        {
            disc.end_streams(); // a key source may read the drive through connections of its own
            const key_request request{m_job.disc(), files, m_head.data(), read, m_job.cancel, m_job.key_search};
            for (const key_source& source : m_env.keys)
            {
                if (cancelled()) break;
                std::string from;
                std::optional<key_bytes> key;
                if (source) key = source(request, from);
                if (!key) continue;
                // Tested on each file the server can start a transfer at; the others when the copy reaches them
                std::string tested;
                bool good = true;
                for (const magic_block& m : m_magics)
                {
                    const std::uint64_t offset = m.lba * ds::sector_size;
                    if (ds::rest_refused_at(offset) && disc.floor_mode() != 0) continue;
                    unsigned char first[16];
                    if (!read(offset, first, sizeof(first))) continue;
                    good = key_opens(*key, m.lba, first, m.magic);
                    tested += std::string(tested.empty() ? "" : ", ") + m.name + (good ? " ok" : " WRONG");
                    if (!good) break;
                }
                if (!good)
                {
                    log("the key from " + from + " does not decrypt this disc (" + tested + ")");
                    continue;
                }
                m_cipher = std::make_unique<sector_cipher>(*key);
                if (!m_cipher->ok())
                {
                    m_cipher.reset();
                    continue;
                }
                m_key_id = key_id(*key);
                m_key_source = from;
                log("key from " + from + " (id " + detail::hex64(m_key_id).substr(0, 8) + "): " + (tested.empty() ? std::string("checked when the copy reaches LIC.DAT / EBOOT.BIN") : tested));
                break;
            }
            // A key source may wait minutes (the key search): the disc must still be the one read above, or the resume
            // decision below would judge the .part (and drop it) against another disc
            if (!m_env.keys.empty() && !cancelled())
            {
                std::array<unsigned char, 2 * ds::sector_size> again{};
                const ds::read_status fresh = disc.read_fresh(0, again.data(), again.size());
                if (fresh != ds::read_status::ok || again != m_head)
                {
                    const std::string why = fresh == ds::read_status::ok ? std::string("changed") : std::string(ds::to_string(fresh)) + ": " + disc.last_failure();
                    disc.close_all();
                    return fail(fresh == ds::read_status::ok ? outcome::disc_changed : fresh == ds::read_status::no_server ? outcome::no_server : outcome::no_disc,
                        "sectors 0-1 after the key sources: " + why);
                }
            }
        }
        else if (has_encrypted_regions(m_layout))
        {
            log("neither LIC.DAT nor EBOOT.BIN found in an encrypted region: the image is written as it is on the disc");
        }
        m_job.decrypting.store(m_cipher != nullptr, std::memory_order_release);
        disc.close_all();
        return outcome::done;
    }

    bool connect()
    {
        std::vector<ds::endpoint> list = m_env.servers.fixed;
        std::string tried;
        for (std::size_t i = 0;; ++i)
        {
            if (i == list.size())
            {
                if (i != m_env.servers.fixed.size() || !m_env.servers.fallback) break;
                const auto extra = m_env.servers.fallback();
                if (!extra || std::find(list.begin(), list.end(), *extra) != list.end()) break;
                list.push_back(*extra);
            }
            if (cancelled()) return false;
            if (m_ftp.connect(list[i]))
            {
                if (!std::exchange(m_connected_logged, true))
                    log("connected to " + ds::to_string(list[i]) + " (" + m_ftp.banner() + ") in " + std::to_string(m_ftp.connect_ms()) + " ms");
                return true;
            }
            tried += (tried.empty() ? "" : "; ") + ds::to_string(list[i]) + ": " + m_ftp.failure_text();
        }
        m_detail = tried.empty() ? std::string("no server configured") : tried;
        return false;
    }

    // A transfer whose stream stands at 'start' (returned) <= target: target itself, or the sector below its 2 GiB
    // boundary when the server cannot start there. A transfer after the first one reads sectors 0-1 again first.
    open_kind open_at(std::uint64_t target, std::uint64_t& start, outcome& hint)
    {
        hint = outcome::read_failed;
        if (!m_ftp.connected() && !connect())
        {
            hint = outcome::no_server;
            return cancelled() ? open_kind::cancelled : open_kind::retry;
        }
        if (m_transfers)
        {
            std::array<unsigned char, 2 * ds::sector_size> head{};
            const bool opened = m_ftp.open_stream(0);
            const std::size_t got = opened ? m_ftp.read_full(head.data(), head.size()) : 0;
            if (opened) m_ftp.close_stream();
            if (cancelled()) return open_kind::cancelled;
            if (got != head.size())
            {
                if (!opened && m_ftp.last_failure() == ds::ftp_connection::failure::refused) hint = outcome::no_disc;
                m_detail = "sectors 0-1 again: " + m_ftp.failure_text();
                return open_kind::retry;
            }
            if (head != m_head)
            {
                m_detail = "sectors 0-1 changed: another disc is in the drive";
                hint = outcome::disc_changed;
                return open_kind::fatal;
            }
        }
        start = ds::rest_refused_at(target) && m_floor_mode == 1 ? ds::rest_floor(target) : target;
        while (!m_ftp.open_stream(start))
        {
            if (cancelled()) return open_kind::cancelled;
            m_detail = "RETR at LBA " + ds::detail::hex(lba(start)) + ": " + m_ftp.failure_text();
            if (m_ftp.last_failure() != ds::ftp_connection::failure::refused) return open_kind::retry;
            if (ds::rest_refused_at(start) && m_floor_mode != 1)
            {
                m_floor_mode = 1;
                log("the server refuses to start a transfer at an offset with bit 31 set (" + m_ftp.last_reply() +
                    "): such points are reached from the sector below their 2 GiB boundary");
                start = ds::rest_floor(target);
                continue;
            }
            hint = outcome::no_disc;
            return open_kind::retry;
        }
        if (ds::rest_refused_at(start) && m_floor_mode < 0) m_floor_mode = 0;
        if (start != target)
            log("LBA " + ds::detail::hex(lba(target)) + " lies past a 2 GiB boundary: the copy restarts at LBA " + ds::detail::hex(lba(start)) + " and compares " +
                ds::detail::mib(target - start) + " with the file on the way");
        ++m_transfers;
        return open_kind::ok;
    }

    // The copy from the start position to the end of the disc, with reconnections
    outcome copy()
    {
        unsigned failures = 0;
        // Furthest point a read failed at: the failure count starts again only past it. A transfer that has to restart
        // below a 2 GiB boundary re-reads (and compares) what lies before that point, which is no progress.
        std::uint64_t failed_at = 0;
        bool failed_once = false;
        outcome hint = outcome::read_failed;
        const auto give_up = [&](const std::string& what) -> bool
        {
            if (++failures > m_cfg.attempts)
            {
                m_detail = what + " (" + std::to_string(m_cfg.attempts) + " attempts)";
                return true;
            }
            const std::int64_t wait = std::min<std::int64_t>(m_cfg.retry_pause_ms << (failures - 1), 8000);
            log(what + "; again in " + std::to_string(wait) + " ms (attempt " + std::to_string(failures) + " of " + std::to_string(m_cfg.attempts) + ")");
            ds::detail::pause(wait, m_job.cancel); // a cancel ends the wait and is taken at the top of the loop
            return false;
        };

        m_job.stage.store(static_cast<int>(m_pos < m_verify_end ? phase::resuming : phase::copying), std::memory_order_release);
        while (m_pos < m_size)
        {
            if (cancelled()) return cancel_now();
            if (!m_ftp.streaming())
            {
                std::uint64_t start = m_pos;
                const open_kind opened = open_at(m_pos, start, hint);
                if (opened == open_kind::cancelled) return cancel_now();
                if (opened == open_kind::fatal) return fail(hint, m_detail);
                if (opened == open_kind::retry)
                {
                    if (give_up(m_detail)) return fail(hint, m_detail);
                    continue;
                }
                if (start < m_pos)
                {
                    m_verify_end = std::max(m_verify_end, m_pos);
                    m_pos = start;
                    m_job.stage.store(static_cast<int>(phase::resuming), std::memory_order_release);
                }
            }

            const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(m_cfg.block_bytes, m_size - m_pos));
            const std::size_t got = m_ftp.read_full(m_buffer.get(), want);
            m_received += got;
            const std::size_t whole = got - got % ds::sector_size;
            if (whole)
            {
                const step done = process(whole);
                if (done == step::error) return m_error;
                if (done == step::restart)
                {
                    if (++m_restarts > 2) return fail(outcome::read_failed, "the copy started over twice already: " + m_detail);
                    continue;
                }
                m_pos += whole;
                if (!failed_once || m_pos > failed_at) failures = 0;
                if (m_pos >= m_durable + m_cfg.sync_bytes && !commit()) return m_error;
                report();
            }
            if (got < want)
            {
                if (cancelled()) return cancel_now();
                const bool ended = !m_ftp.streaming() && m_ftp.end_code() == 226;
                // 451 while the data flows: the drive could not read there (a scratch, dirt), not a network fault
                const bool drive_error = !m_ftp.streaming() && (m_ftp.end_code() == 451 || m_ftp.last_code() == 451);
                const std::string what = "read at LBA " + ds::detail::hex(lba(m_pos)) + " failed (" +
                    (ended ? std::string("the server ended the transfer before the end of the disc") : m_ftp.failure_text()) + ")";
                if (m_ftp.streaming()) m_ftp.close_stream();
                hint = outcome::read_failed;
                const bool same_place = failed_once && m_pos <= failed_at;
                failed_at = std::max(failed_at, m_pos);
                failed_once = true;
                if (give_up(what))
                {
                    if (drive_error && same_place)
                    {
                        m_job.unreadable_lba.store(static_cast<std::int64_t>(lba(m_pos)), std::memory_order_release);
                        log("the drive cannot read the disc at LBA " + ds::detail::hex(lba(m_pos)) + " (" + std::to_string(m_cfg.attempts + 1) +
                            " transfers failed there): clean the disc and install again; the copy goes on from there");
                    }
                    return fail(hint, m_detail);
                }
            }
        }
        if (m_ftp.streaming()) m_ftp.close_stream();
        return outcome::done;
    }

    // Decrypts, checks, compares and writes the 'size' bytes (whole sectors) of m_buffer that belong at m_pos
    step process(std::size_t size)
    {
        unsigned char* data = m_buffer.get();
        const std::uint64_t first = lba(m_pos), count = size / ds::sector_size;
        if (m_cipher)
        {
            for (std::uint64_t i = 0; i < count;)
            {
                const auto [encrypted, run] = region_run(m_layout, first + i, count - i);
                if (encrypted && !m_cipher->decrypt(first + i, data + i * ds::sector_size, run))
                {
                    m_error = outcome::read_failed;
                    m_detail = "decryption failed at LBA " + ds::detail::hex(first + i);
                    return step::error;
                }
                i += run;
            }
            for (const magic_block& m : m_magics)
            {
                if (m.lba < first || m.lba >= first + count) continue;
                if (std::memcmp(data + (m.lba - first) * ds::sector_size, m.magic.data(), m.magic.size()) == 0) continue;
                log(std::string("the key from ") + m_key_source + " does not decrypt " + m.name + " (LBA " + ds::detail::hex(m.lba) +
                    "): starting over with the image as it is on the disc");
                m_cipher.reset();
                m_key_id = 0;
                m_job.decrypting.store(false, std::memory_order_release);
                m_detail = std::string("the key does not decrypt ") + m.name;
                return restart();
            }
        }

        std::size_t compared = 0;
        if (m_pos < m_verify_end)
        {
            compared = static_cast<std::size_t>(std::min<std::uint64_t>(size, m_verify_end - m_pos));
            if (!read_file(m_pos, m_compare.get(), compared) || std::memcmp(m_compare.get(), data, compared) != 0)
            {
                std::size_t at = 0;
                while (at + ds::sector_size <= compared && std::memcmp(m_compare.get() + at, data + at, ds::sector_size) == 0) at += ds::sector_size;
                log("resume: the file differs from the disc at LBA " + ds::detail::hex(lba(m_pos + at)) + ": starting over");
                m_detail = "the file differs from the disc";
                return restart();
            }
            if (m_pos + compared == m_verify_end)
            {
                log("resume: the file matches the disc up to LBA " + ds::detail::hex(lba(m_verify_end)) + ": copying");
                m_job.stage.store(static_cast<int>(phase::copying), std::memory_order_release);
            }
        }
        if (size > compared)
        {
            int error = 0;
            if (m_env.files.write_at(m_fd, data + compared, size - compared, m_pos + compared, error) != static_cast<std::int64_t>(size - compared))
            {
#ifdef EDQUOT
                if (error == EDQUOT) error = ENOSPC;
#endif
                m_error = error == ENOSPC ? outcome::no_space : outcome::write_failed;
                m_detail = "write at " + std::to_string(m_pos + compared) + " of " + m_part + ": " + ds::detail::errno_text(error);
                return step::error;
            }
            m_copied += size - compared;
        }
        return step::ok;
    }

    // Everything in the file is dropped and the copy starts again at LBA 0
    step restart()
    {
        if (m_ftp.streaming()) m_ftp.close_stream();
        int error = 0;
        if (!m_env.files.truncate(m_fd, 0, error))
        {
            m_error = outcome::write_failed;
            m_detail = "truncate " + m_part + ": " + ds::detail::errno_text(error);
            return step::error;
        }
        m_pos = m_verify_end = m_durable = 0;
        if (!write_info()) return step::error;
        m_job.stage.store(static_cast<int>(phase::copying), std::memory_order_release);
        return step::restart;
    }

    outcome cancel_now()
    {
        m_ftp.disconnect(); // the server ends the transfer on the closed connections
        log("cancelled at " + ds::detail::mib(m_pos) + " of " + ds::detail::mib(m_size));
        m_detail = "cancelled";
        return outcome::cancelled;
    }

    // Progress for the XMB; a log line every log_interval_ms
    void report()
    {
        const std::int64_t now = ds::detail::now_ms();
        m_job.done.store(m_pos, std::memory_order_release);
        if (now - m_rate_at >= 1000)
        {
            const double sample = static_cast<double>(m_received - m_rate_base) * 1000.0 / static_cast<double>(now - m_rate_at);
            m_rate = m_rate > 0 ? m_rate * 0.7 + sample * 0.3 : sample;
            m_job.rate.store(static_cast<std::uint64_t>(m_rate), std::memory_order_release);
            m_rate_at = now;
            m_rate_base = m_received;
        }
        if (now - m_logged_at >= m_cfg.log_interval_ms)
        {
            const double span = static_cast<double>(now - m_logged_at) / 1000.0;
            const double window = static_cast<double>(m_pos > m_logged_pos ? m_pos - m_logged_pos : 0) / span;
            const std::int64_t left = window > 0 ? static_cast<std::int64_t>(static_cast<double>(m_size - m_pos) / window) : -1;
            log(ds::detail::mib(m_pos) + " of " + ds::detail::mib(m_size) + " (" + std::to_string(m_pos * 100 / m_size) + "%), " + detail::rate_text(window) +
                (left >= 0 ? ", about " + std::to_string(left) + " s left" : std::string{}) + (m_pos < m_verify_end ? " (comparing with the file)" : ""));
            m_logged_at = now;
            m_logged_pos = m_pos;
        }
    }

    // <lock_dir><TITLE_ID>.txt through a .tmp (written, synced, renamed): the title, the image size and what identifies
    // the disc, read from the verified file
    bool write_lock_record()
    {
        if (m_cfg.lock_dir.empty()) return true;
        disc_lock::record r;
        r.title_id = m_id;
        r.title = ds::sfo_string(m_param.data(), m_param.size(), "TITLE");
        if (r.title.empty()) r.title = m_job.disc().title;
        r.version = ds::sfo_string(m_param.data(), m_param.size(), "VERSION");
        r.app_ver = ds::sfo_string(m_param.data(), m_param.size(), "APP_VER");
        r.size = m_size;
        r.identity = m_identity;
        if (m_pvd_read && !m_param.empty())
            r.fingerprint = ds::disc_fingerprint(m_head.data(), m_head.size(), m_pvd.data(), m_pvd.size(), m_param.data(), m_param.size());
        r.image = m_final.substr(m_final.rfind('/') + 1);
        r.path = m_final; // the only file the record binds (ps4_disc_lock.hpp)
        const std::string path = m_cfg.lock_dir + disc_lock::file_name(m_id), temp = path + ".tmp", text = disc_lock::to_text(r);
        int error = 0;
        bool ok = !m_env.files.make_dir || m_env.files.make_dir(m_cfg.lock_dir.substr(0, m_cfg.lock_dir.size() - 1), error);
        int fd = ok ? m_env.files.open(temp, true, error) : -1;
        if (ok && fd < 0 && reopened(m_cfg.lock_dir, error))
        {
            error = 0;
            fd = m_env.files.open(temp, true, error);
        }
        ok = fd >= 0 && m_env.files.truncate(fd, 0, error) &&
            m_env.files.write_at(fd, text.data(), text.size(), 0, error) == static_cast<std::int64_t>(text.size()) && m_env.files.sync(fd, error);
        if (fd >= 0) m_env.files.close(fd);
        ok = ok && m_env.files.rename(temp, path, error);
        if (!ok)
        {
            m_error = error == ENOSPC ? outcome::no_space : outcome::write_failed;
            m_detail = "lock record " + path + ": " + ds::detail::errno_text(error);
            if (fd >= 0) m_env.files.remove(temp);
            return false;
        }
        log("lock record " + path + " (identity " + detail::hex64(r.identity) + ", fingerprint " + detail::hex64(r.fingerprint) + ")");
        return true;
    }

    // The finished file: durable, checked, renamed
    outcome finish()
    {
        m_ftp.quit();
        m_job.done.store(m_size, std::memory_order_release);
        m_job.stage.store(static_cast<int>(phase::verifying), std::memory_order_release);
        if (!commit()) return m_error;
        const double seconds = std::max(0.001, static_cast<double>(ds::detail::now_ms() - m_started) / 1000.0);
        log("copied: " + ds::detail::mib(m_copied) + " written in " + std::to_string(static_cast<std::int64_t>(seconds)) + " s (" +
            detail::rate_text(static_cast<double>(m_received) / seconds) + " from the drive)");

        std::string checked;
        if (!verify(checked))
        {
            log("verify: " + checked);
            m_durable = 0;
            write_info(); // the next install starts over
            return fail(outcome::verify_failed, checked);
        }
        log("verify: " + checked);
        // The record first: no image of this install ever exists without it. When it cannot be written the .part stays
        // complete and durable, and the next install compares its end, writes the record and renames it.
        if (!write_lock_record()) return m_error;
        close_file();
        int error = 0;
        if (!m_env.files.rename(m_part, m_final, error)) return fail(outcome::write_failed, "rename " + m_part + ": " + ds::detail::errno_text(error));
        m_env.files.remove(m_info);
        log("done: " + m_final + " (" + (m_cipher ? std::string("decrypted, plays without a key") : has_encrypted_regions(m_layout)
            ? "encrypted: the loader needs " + m_id + ".dkey in the redump folder" : std::string("no encrypted region")) + ")");
        m_detail = "installed";
        return outcome::done;
    }

    bool verify(std::string& report)
    {
        const ds::read_fn read = [this](std::uint64_t offset, void* out, std::size_t size) { return read_file(offset, out, size); };
        std::array<unsigned char, 2 * ds::sector_size> head{};
        if (!read(0, head.data(), head.size()) || head != m_head)
        {
            report = "sectors 0-1 of the file differ from the disc";
            return false;
        }
        const ds::disc_files files = ds::locate_files(read);
        if (!files.tree || files.param.empty())
        {
            report = "no PS3_GAME/PARAM.SFO in the file";
            return false;
        }
        m_param = files.param;
        m_pvd_read = read(16 * ds::sector_size, m_pvd.data(), m_pvd.size());
        const std::string id = ds::canonical_title_id(ds::sfo_string(files.param.data(), files.param.size(), "TITLE_ID"));
        if (!id.empty() && id != m_id)
        {
            report = "PARAM.SFO names " + id;
            return false;
        }
        report = "PARAM.SFO " + (id.empty() ? std::string("(no TITLE_ID)") : "TITLE_ID " + id) + " ok";
        for (const auto& [file, magic, name] : {std::tuple{files.lic_dat, lic_magic, "LIC.DAT"}, std::tuple{files.eboot, eboot_magic, "EBOOT.BIN"}})
        {
            if (!file || file->bytes < 16) continue;
            unsigned char first[16];
            if (!read(std::uint64_t{file->lba} * ds::sector_size, first, sizeof(first)))
            {
                report += std::string(", ") + name + " unreadable";
                return false;
            }
            const bool clear = std::memcmp(first, magic.data(), magic.size()) == 0;
            const bool encrypted_here = region_run(m_layout, file->lba, 1).first;
            if (m_cipher || !encrypted_here)
            {
                report += std::string(", ") + name + (clear ? " magic ok" : " magic WRONG");
                if (!clear) return false;
            }
            else
            {
                report += std::string(", ") + name + " encrypted";
            }
        }
        return true;
    }
};

// Runs one install on the calling thread
inline void run(const environment& env, job& j)
{
    installer(env, j).run();
}

// XMB texts in the interface language (pt-BR: one message line of about 120 characters)

// Decimal mark of sizes and rates: ',' in pt-BR and Spanish, '.' in English and Arabic
inline char decimal_mark() noexcept
{
    const i18n::lang l = i18n::current();
    return l == i18n::lang::en || l == i18n::lang::ar ? '.' : ',';
}

// "9,9 GB", "640 MB" (decimal units, decimal_mark())
inline std::string size_text(std::uint64_t bytes)
{
    char text[32];
    if (bytes >= 1'000'000'000) std::snprintf(text, sizeof(text), "%.1f GB", static_cast<double>(bytes) / 1e9);
    else std::snprintf(text, sizeof(text), "%llu MB", static_cast<unsigned long long>((bytes + 500'000) / 1'000'000));
    std::string out = text;
    if (const std::size_t dot = out.find('.'); dot != std::string::npos) out[dot] = decimal_mark();
    return out;
}

inline std::string short_title(const ds::disc_status& s)
{
    std::string title = s.title.empty() ? ds::canonical_title_id(s.title_id) : s.title;
    if (title.empty()) title = i18n::tr(i18n::str::DINST_DISCO_PS3);
    if (title.size() > 40)
    {
        std::size_t cut = 37;
        while (cut && (static_cast<unsigned char>(title[cut]) & 0xc0) == 0x80) --cut;
        title = title.substr(0, cut) + "...";
    }
    return title;
}

inline unsigned percent(std::uint64_t part, std::uint64_t whole)
{
    return whole ? static_cast<unsigned>(std::min<std::uint64_t>(100, part * 100 / whole)) : 0;
}

// The line under the XMB row while an install runs
inline std::string progress_text(const job& j)
{
    using i18n::str;
    const std::string title = short_title(j.disc());
    const std::uint64_t done = j.done.load(std::memory_order_acquire), total = j.total.load(std::memory_order_acquire);
    const std::uint64_t rate = j.rate.load(std::memory_order_acquire);
    switch (j.current())
    {
    case phase::starting:
    case phase::checking:
        return i18n::trf(j.key_search.load(std::memory_order_acquire) ? str::DINST_INSTALANDO_BUSCANDO_CHAVE : str::DINST_INSTALANDO_LENDO_DISCO, {title});
    case phase::resuming: return i18n::trf(str::DINST_RETOMANDO_CONFERINDO, {title, percent(done, total)});
    case phase::copying:
    {
        if (!rate) return i18n::trf(str::DINST_INSTALANDO_PROGRESSO, {title, size_text(done), size_text(total), percent(done, total)});
        char speed[32];
        std::snprintf(speed, sizeof(speed), "%.1f", static_cast<double>(rate) / 1e6);
        std::string text = speed;
        if (const std::size_t dot = text.find('.'); dot != std::string::npos) text[dot] = decimal_mark();
        const std::uint64_t minutes = (total > done ? total - done : 0) / rate / 60 + 1;
        return i18n::trf(minutes > 1 ? str::DINST_INSTALANDO_PROGRESSO_FALTAM : str::DINST_INSTALANDO_PROGRESSO_FALTA_1,
            {title, size_text(done), size_text(total), percent(done, total), text, minutes});
    }
    case phase::verifying: return i18n::trf(str::DINST_INSTALANDO_CONFERINDO_IMAGEM, {title});
    case phase::finished: break;
    }
    return i18n::trf(str::DINST_INSTALANDO_TITULO_ETC, {title});
}

// The line after the install ended
inline std::string result_text(const job& j)
{
    using i18n::str;
    const std::string title = short_title(j.disc());
    switch (j.result())
    {
    case outcome::done:
        return i18n::trf(j.encrypted.load() && !j.decrypting.load() ? str::DINST_DISCO_INSTALADO_FALTA_CHAVE : str::DINST_DISCO_INSTALADO_LISTA, {title});
    case outcome::cancelled: return i18n::tr(str::DINST_INSTALACAO_CANCELADA_FOI_COPIADO_FICA);
    case outcome::no_server: return i18n::tr(str::DINST_FTP_GOLDHEN_NAO_RESPONDEU_LIGUE);
    case outcome::no_disc: return i18n::tr(str::DINST_DISCO_SAIU_LEITOR_NAO_PODE);
    case outcome::disc_changed: return i18n::tr(str::DINST_HA_OUTRO_DISCO_LEITOR_ESCOLHA);
    case outcome::no_space: return i18n::trf(str::DINST_SEM_ESPACO_DATA_PRECISOS, {size_text(j.need.load())});
    case outcome::write_failed: return i18n::tr(str::DINST_FALHA_GRAVAR_DATA_RPCS3_GAMES);
    case outcome::read_failed:
        if (const std::int64_t bad = j.unreadable_lba.load(std::memory_order_acquire); bad >= 0)
        {
            char sector_text[24];
            std::snprintf(sector_text, sizeof(sector_text), "0x%llx", static_cast<unsigned long long>(bad));
            return i18n::trf(str::DINST_LEITOR_NAO_CONSEGUIU_LER_SETOR, {sector_text});
        }
        return i18n::tr(str::DINST_FALHA_LER_DISCO_INSTALE_NOVO);
    case outcome::verify_failed: return i18n::tr(str::DINST_IMAGEM_COPIADA_NAO_CONFERE_DISCO);
    case outcome::busy: return i18n::tr(str::DINST_JA_HA_INSTALACAO_DISCO_ANDAMENTO);
    }
    return {};
}

struct row_text
{
    std::string label; // the XMB row
    std::string help;  // the line under it
};

// The drive's XMB row: what is in the drive and what X does. 'active': the running install, if any.
inline row_text drive_row(const ds::disc_status& s, const job* active)
{
    using i18n::str;
    if (active && !active->finished.load(std::memory_order_acquire)) return {i18n::trf(str::DINST_INSTALANDO_TITULO, {short_title(active->disc())}), progress_text(*active)};
    switch (s.state)
    {
    case ds::disc_state::unknown: return {i18n::tr(str::DINST_DISCO_PS3_2), i18n::tr(str::COM_VERIFICANDO_LEITOR_DISCOS)};
    case ds::disc_state::no_server:
        // Without an IP address GoldHEN's FTP server does not listen: the cause is the missing network, not the setting
        if (ds::no_network(s)) return {i18n::tr(str::DOFF_ROW_SEM_REDE), i18n::tr(str::DOFF_ROW_SEM_REDE_HELP)};
        return {i18n::tr(str::DINST_DISCO_PS3_LIGUE_FTP), i18n::tr(str::DINST_APP_LE_LEITOR_SERVIDOR_FTP)};
    case ds::disc_state::no_disc: return {i18n::tr(str::DINST_DISCO_PS3_LEITOR_VAZIO), i18n::tr(str::DINST_COLOQUE_DISCO_JOGO_PS3_LEITOR)};
    case ds::disc_state::not_ps3: return {i18n::tr(str::DINST_DISCO_LEITOR_NAO_PS3), i18n::tr(str::DINST_SO_DISCOS_JOGOS_PS3_PODEM)};
    case ds::disc_state::ps3_disc: break;
    }
    const std::string title = short_title(s), id = ds::canonical_title_id(s.title_id);
    if (!s.partial.empty())
        return {i18n::trf(str::DINST_CONTINUAR_INSTALACAO, {title}), i18n::trf(str::DINST_JA_COPIADOS_CONTINUA, {id, size_text(s.partial_bytes), size_text(s.size)})};
    if (!s.installed.empty()) return {i18n::trf(str::DINST_TITULO_INSTALADO, {title}), i18n::trf(str::DINST_JA_INSTALADO_DATA_RPCS3_GAMES, {id})};
    return {i18n::trf(str::DINST_INSTALAR_DISCO_PS3, {title}), i18n::trf(str::DINST_COPIA_DISCO_DATA_RPCS3_GAMES, {id, size_text(s.size)})};
}
}

#ifdef RPCS3_PS4
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wold-style-cast"
#endif
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#ifdef __clang__
#pragma clang diagnostic pop
#endif

#if __has_include("util/ps4_disc_key.hpp")
#include "util/ps4_disc_key.hpp"
#endif

namespace utils::ps4::disc_install
{
namespace sys
{
// POSIX under /data/rpcs3 (the console applies no umask: new files 0666, which the FTP server may delete)
inline const file_ops& posix_files()
{
    static const file_ops* const ops = []
    {
        auto* f = new file_ops();
        f->open = [](const std::string& path, bool create, int& error) -> int
        {
            const int fd = ::open(path.c_str(), O_RDWR | O_CLOEXEC | (create ? O_CREAT : 0), 0666);
            if (fd < 0) error = errno ? errno : EIO;
            return fd;
        };
        f->write_at = [](int fd, const void* data, std::size_t size, std::uint64_t offset, int& error) -> std::int64_t
        {
            // lseek + write instead of pwrite: the image is written by its install thread only, and both functions are
            // imports the app already has (a new strong import that the firmware lacks stops the app from loading)
            std::size_t done = 0;
            while (done < size)
            {
                if (::lseek(fd, static_cast<off_t>(offset + done), SEEK_SET) < 0)
                {
                    if (errno == EINTR) continue;
                    error = errno ? errno : EIO;
                    return -1;
                }
                const ssize_t n = ::write(fd, static_cast<const char*>(data) + done, size - done);
                if (n > 0) done += static_cast<std::size_t>(n);
                else if (n < 0 && errno == EINTR) continue;
                else
                {
                    error = n < 0 && errno ? errno : EIO;
                    return -1;
                }
            }
            return static_cast<std::int64_t>(done);
        };
        f->read_at = [](int fd, void* data, std::size_t size, std::uint64_t offset, int& error) -> std::int64_t
        {
            for (;;)
            {
                const ssize_t n = ::pread(fd, data, size, static_cast<off_t>(offset));
                if (n >= 0) return n;
                if (errno != EINTR)
                {
                    error = errno ? errno : EIO;
                    return -1;
                }
            }
        };
        f->truncate = [](int fd, std::uint64_t size, int& error)
        {
            if (::ftruncate(fd, static_cast<off_t>(size)) == 0) return true;
            error = errno ? errno : EIO;
            return false;
        };
        f->sync = [](int fd, int& error)
        {
            if (::fsync(fd) == 0) return true;
            error = errno ? errno : EIO;
            return false;
        };
        f->close = [](int fd) { ::close(fd); };
        f->size = [](const std::string& path) -> std::int64_t
        {
            struct ::stat info{};
            if (::stat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode)) return -1;
            return static_cast<std::int64_t>(info.st_size);
        };
        f->rename = [](const std::string& from, const std::string& to, int& error)
        {
            if (::rename(from.c_str(), to.c_str()) == 0) return true;
            error = errno ? errno : EIO;
            return false;
        };
        f->remove = [](const std::string& path) { return ::unlink(path.c_str()) == 0; };
        f->free_bytes = [](const std::string& dir) -> std::int64_t
        {
            struct ::statvfs vs{};
            if (::statvfs(dir.c_str(), &vs) != 0) return -1;
            const std::uint64_t unit = vs.f_frsize ? static_cast<std::uint64_t>(vs.f_frsize) : static_cast<std::uint64_t>(vs.f_bsize);
            const std::uint64_t blocks = static_cast<std::uint64_t>(vs.f_blocks), avail = static_cast<std::uint64_t>(vs.f_bavail);
            if (!unit || (unit & (unit - 1)) || !blocks || avail > blocks) return -1;
            return static_cast<std::int64_t>(avail * unit);
        };
        f->list = [](const std::string& dir)
        {
            std::vector<std::string> names;
            if (DIR* d = ::opendir(dir.c_str()))
            {
                while (const dirent* entry = ::readdir(d))
                {
                    const std::string_view name = entry->d_name;
                    if (name != "." && name != ".." && names.size() < 100000) names.emplace_back(name);
                }
                ::closedir(d);
            }
            return names;
        };
        f->make_dir = [](const std::string& dir, int& error)
        {
            if (::mkdir(dir.c_str(), 0777) == 0 || errno == EEXIST) return true;
            error = errno ? errno : EIO;
            return false;
        };
        return f;
    }();
    return *ops;
}

inline std::optional<key_bytes> read_key_file(const std::string& path)
{
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return std::nullopt;
    unsigned char data[64];
    const ssize_t got = ::read(fd, data, sizeof(data));
    ::close(fd);
    return got > 0 ? parse_key_file(data, static_cast<std::size_t>(got)) : std::nullopt;
}

// <redump folder>/<TITLE_ID>.dkey or .key, where the key import of the XMB puts keys
inline key_source redump_folder()
{
    return [](const key_request& request, std::string& source) -> std::optional<key_bytes>
    {
        const std::string id = ds::canonical_title_id(request.disc.title_id);
        if (id.empty()) return std::nullopt;
        const std::string dir = rpcs3::utils::get_redump_key_dir();
        for (const char* extension : {".dkey", ".key"})
        {
            if (auto key = read_key_file(dir + id + extension))
            {
                source = dir + id + extension;
                return key;
            }
        }
        return std::nullopt;
    };
}

#if __has_include("util/ps4_disc_key.hpp")
// The automatic key search of ps4_disc_key.hpp (redump folder, IRDs, USB, the IRD database): started for this disc
// unless it ran already, waited for (at most 10 min, ended by a cancel), then the verified key it wrote is read
inline key_source disc_key_search()
{
    return [](const key_request& request, std::string& source) -> std::optional<key_bytes>
    {
        const std::string id = ds::canonical_title_id(request.disc.title_id);
        if (id.empty()) return std::nullopt;
        disc_key::service& search = disc_key::service::instance();
        search.request(request.disc);
        // A disc whose EBOOT.BIN lies past a 2 GiB boundary is read forward up to 2 GiB first (~2-3 min at the drive's
        // 15-19 MiB/s), then up to 10 IRDs with retries: wait up to 10 min
        const std::int64_t end = ds::detail::now_ms() + 600000;
        request.searching.store(true, std::memory_order_release);
        while (search.busy() && !request.cancel.load(std::memory_order_acquire) && ds::detail::now_ms() < end)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        request.searching.store(false, std::memory_order_release);
        const std::string path = rpcs3::utils::get_redump_key_dir() + id + ".dkey";
        auto key = read_key_file(path);
        if (key) source = "the disc key search (" + path + ")";
        return key;
    };
}
#endif

struct registry
{
    std::mutex mutex;
    std::vector<key_source> sources; // after the redump folder
    std::function<bool(const std::string& folder)> reopen; // environment::reopen of every install
    std::shared_ptr<job> current;
    bool annotator = false;
};

inline registry& state()
{
    static registry* const r = new registry(); // never destroyed: an install thread may outlive main()
    return *r;
}

inline void log_line(const std::string& line)
{
    ps4_startup_checkpoint(line.c_str());
}
}

// Another source of disc keys (e.g. a key database), asked after the redump folder, in the order added
inline void add_key_source(key_source source)
{
    sys::registry& r = sys::state();
    std::lock_guard lock(r.mutex);
    r.sources.push_back(std::move(source));
}

// How an install opens a folder that refused a write (the permission repair of the frontend); runs on the install thread
inline void set_folder_reopen(std::function<bool(const std::string& folder)> reopen)
{
    sys::registry& r = sys::state();
    std::lock_guard lock(r.mutex);
    r.reopen = std::move(reopen);
}

// XMB: the drive status with what is installed of the disc. Never waits for the network or the disk.
inline ds::disc_status poll()
{
    ds::service& drive = ds::service::instance();
    {
        sys::registry& r = sys::state();
        std::lock_guard lock(r.mutex);
        if (!std::exchange(r.annotator, true))
        {
            drive.set_annotator([](ds::disc_status& s)
            {
                const std::string id = ds::canonical_title_id(s.title_id);
                const presence found = find_presence(sys::posix_files(), default_games_dir, id, recorded_image(sys::posix_files(), disc_lock::registry_dir, id));
                s.installed = found.installed;
                s.partial = found.partial;
                s.partial_bytes = found.partial_bytes;
            });
            drive.request_check();
        }
    }
    return drive.poll();
}

// The running install, or the last one until another starts
inline std::shared_ptr<job> current()
{
    sys::registry& r = sys::state();
    std::lock_guard lock(r.mutex);
    return r.current;
}

// Starts installing 'disc' (a PS3 disc status from poll()) on a thread of its own. nullptr when an install runs or
// the thread could not be made. Returns at once.
inline std::shared_ptr<job> start(const ds::disc_status& disc)
{
    sys::registry& r = sys::state();
    std::lock_guard lock(r.mutex);
    if (r.current && !r.current->finished.load(std::memory_order_acquire)) return nullptr;
    auto j = std::make_shared<job>(disc);
    std::vector<key_source> keys{sys::redump_folder()};
#if __has_include("util/ps4_disc_key.hpp")
    keys.push_back(sys::disc_key_search());
#endif
    keys.insert(keys.end(), r.sources.begin(), r.sources.end());
    const bool started = ds::run_detached([j, keys = std::move(keys), reopen = r.reopen]() mutable
    {
        ds::service& drive = ds::service::instance();
        drive.hold();
        environment env;
        env.net = ds::sys::net();
        env.servers = ds::sys::servers(sys::log_line);
        env.files = sys::posix_files();
        env.keys = std::move(keys);
        env.log = sys::log_line;
        env.reopen = std::move(reopen);
        run(env, *j);
        drive.release();
    });
    if (!started) return nullptr;
    r.current = j;
    return j;
}

// Removes the interrupted install of 'disc' (.iso.part and .iso.part.info) on a thread of its own; false when an
// install runs, there is none, or no thread could be made.
inline bool discard_partial(const ds::disc_status& disc)
{
    if (disc.partial.empty() || !names_title(disc.partial.substr(disc.partial.rfind('/') + 1), ds::canonical_title_id(disc.title_id), ".iso.part")) return false;
    if (const auto running = current(); running && !running->finished.load(std::memory_order_acquire)) return false;
    const std::string part = disc.partial;
    return ds::run_detached([part]
    {
        const bool removed = sys::posix_files().remove(part);
        sys::posix_files().remove(part + ".info");
        sys::log_line("disc install: " + part + (removed ? " removed" : " could not be removed"));
        ds::service::instance().request_check();
    });
}
}
#endif
