// Host test of the disc stream and the disc install: an in-process stand-in for the GoldHEN FTP server (v2.2 as
// measured on the console) behind net_ops, the reader (cache, stream reuse, cursors, 2 GiB boundaries, retries,
// threads), the drive status, read_header, the probe's stage 3, and the install (decryption, resume across a 2 GiB
// boundary, cancel, dropped connections, disc removed, power loss, full volume) into an in-memory file system.
// Build (from the repo root):
//   clang++ -std=c++20 -O2 -static -Wall -Wextra -I rpcs3 ps4-port/tests/disc_stream_test.cpp rpcs3/Crypto/aes.cpp
//       rpcs3/Crypto/aesni.cpp -o disc_stream_test.exe
#include "../../rpcs3/util/ps4_disc_stream.hpp"
#include "../../rpcs3/util/ps4_disc_probe.hpp"
#include "../../rpcs3/util/ps4_disc_install.hpp"

#include <cassert>
#include <cstdio>
#include <map>
#include <random>
#include <set>

namespace
{
namespace ds = utils::ps4::disc_stream;
namespace di = utils::ps4::disc_install;
namespace probe_ns = utils::ps4::disc_probe;
using namespace std::chrono_literals;

constexpr std::uint32_t lan_ip = 0xc0a80121; // 192.168.1.33, what PASV advertises
constexpr std::uint64_t sector = ds::sector_size;

// The disc bytes: byte (offset % 8) of a mix of offset / 8 (little-endian host)
std::uint64_t pattern_word(std::uint64_t word)
{
    std::uint64_t x = word + 0x9e3779b97f4a7c15ull;
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ull;
    x ^= x >> 27;
    return x;
}

void pattern_fill(std::uint64_t offset, unsigned char* out, std::size_t n)
{
    std::size_t i = 0;
    while (i < n)
    {
        const std::uint64_t at = offset + i;
        const std::uint64_t x = pattern_word(at / 8);
        if (at % 8 == 0 && n - i >= 8)
        {
            std::memcpy(out + i, &x, 8);
            i += 8;
        }
        else
        {
            out[i++] = static_cast<unsigned char>(x >> (8 * (at % 8)));
        }
    }
}

struct log_sink
{
    std::mutex mutex;
    std::vector<std::string> lines;

    ds::log_fn fn()
    {
        return [this](const std::string& line)
        {
            std::lock_guard lock(mutex);
            lines.push_back(line);
        };
    }

    bool has(std::string_view text)
    {
        std::lock_guard lock(mutex);
        for (const auto& line : lines)
            if (line.find(text) != std::string::npos) return true;
        return false;
    }

    std::size_t count(std::string_view text)
    {
        std::lock_guard lock(mutex);
        std::size_t n = 0;
        for (const auto& line : lines)
            if (line.find(text) != std::string::npos) ++n;
        return n;
    }
};

// GoldHEN FTP v2.2 as measured: 220 banner, USER -> 230, PASV only (LAN address advertised), REST + RETR /dev/cd0,
// 550 at offsets with bit 31 set, 451 for unaligned or past-the-end REST, 226 at the end of the disc, 426 when the
// client closes the data connection, ABOR 502. Commands are not read while a transfer runs (counted as violations).
class fake_server
{
public:
    std::uint64_t size = 0;
    std::set<std::uint32_t> listening{ds::loopback_ipv4};
    std::uint16_t control_port = ds::default_port;
    bool refuse_bit31 = true;
    bool refuse_second_stream = false;
    bool no_disc = false;
    std::size_t max_recv = 64 * 1024;
    bool random_recv = false;
    bool stall_data = false;
    std::uint64_t drop_data_at = ~0ull;    // the data connection resets there (once)
    int drop_control_after = -1;           // the server closes the control connection on the n-th next command (once)
    std::uint64_t fail_read_at = ~0ull;    // the device read fails there (451), fail_read_times times
    unsigned fail_read_times = 0;
    std::uint64_t remove_disc_at = ~0ull;  // the disc leaves the drive when a transfer reaches this offset (451, then 550)
    std::set<std::uint64_t> refuse_rest;   // RETR after REST at one of these offsets answers 550
    std::uint64_t cancel_at = ~0ull;       // a transfer reaching this offset sets *cancel_flag (once)
    std::atomic<bool>* cancel_flag = nullptr;
    std::string feat_reply;                // the answer to FEAT ("": 502 like any unknown command)
    bool feat_silent = false;              // FEAT gets no answer at all
    std::map<std::uint64_t, std::vector<unsigned char>> overlay; // plaintext

    std::mutex mutex;
    unsigned connects = 0, refused_connects = 0, retrs = 0, retr_550 = 0, aborted = 0, streams = 0, max_streams = 0, violations = 0;
    std::uint64_t bytes_sent = 0;
    std::vector<std::string> commands;
    std::vector<std::uint64_t> retr_offsets;

    // Sectors of the encrypted regions of 'table' are served encrypted with 'key' (AES-128-CBC, IV = LBA)
    void set_key(const di::key_bytes& key, const ds::layout& table)
    {
        aes_setkey_enc(&m_aes, key.data(), 128);
        m_table = table;
        m_encrypted = true;
    }

    void clear_key() { m_encrypted = false; }

    // The disc as the image would hold it decrypted
    void plain(std::uint64_t offset, unsigned char* out, std::size_t n) const
    {
        pattern_fill(offset, out, n);
        for (const auto& [at, bytes] : overlay)
        {
            const std::uint64_t end = at + bytes.size();
            if (end <= offset || at >= offset + n) continue;
            const std::uint64_t from = std::max(at, offset), to = std::min(end, offset + n);
            std::memcpy(out + (from - offset), bytes.data() + (from - at), static_cast<std::size_t>(to - from));
        }
    }

    // The disc as the drive returns it
    void fill(std::uint64_t offset, unsigned char* out, std::size_t n) const
    {
        plain(offset, out, n);
        if (!m_encrypted) return;
        for (std::uint64_t lba = offset / sector; lba * sector < offset + n;)
        {
            const auto [encrypted, run] = di::region_run(m_table, lba, ~0ull);
            if (!encrypted)
            {
                lba += run;
                continue;
            }
            unsigned char block[sector];
            plain(lba * sector, block, sector);
            unsigned char iv[16]{};
            iv[12] = static_cast<unsigned char>(lba >> 24);
            iv[13] = static_cast<unsigned char>(lba >> 16);
            iv[14] = static_cast<unsigned char>(lba >> 8);
            iv[15] = static_cast<unsigned char>(lba);
            aes_crypt_cbc(&m_aes, AES_ENCRYPT, sector, iv, block, block); // reads the context only
            const std::uint64_t from = std::max(lba * sector, offset), to = std::min((lba + 1) * sector, offset + n);
            std::memcpy(out + (from - offset), block + (from - lba * sector), static_cast<std::size_t>(to - from));
            ++lba;
        }
    }

    std::vector<unsigned char> expected(std::uint64_t offset, std::size_t n) const
    {
        std::vector<unsigned char> out(n);
        fill(offset, out.data(), n);
        return out;
    }

    void put(std::uint64_t offset, const std::vector<unsigned char>& bytes) { overlay[offset] = bytes; }

    // The server drops every session that has no transfer running (idle time-out)
    void drop_idle_sessions()
    {
        std::lock_guard lock(mutex);
        for (session& s : m_sessions)
            if (s.transfer < 0) s.closed = true;
    }

    unsigned retr_count()
    {
        std::lock_guard lock(mutex);
        return retrs;
    }

    ds::net_ops ops()
    {
        ds::net_ops o;
        o.connect = [this](const ds::endpoint& to, int, int, int& error) { return connect(to, error); };
        o.send = [this](int fd, const void* data, std::size_t n, int, int& error) { return send(fd, data, n, error); };
        o.recv = [this](int fd, void* data, std::size_t n, int timeout_ms, int& error) { return recv(fd, data, n, timeout_ms, error); };
        o.close = [this](int fd) { close(fd); };
        return o;
    }

private:
    struct session
    {
        bool closed = false; // ended by the server: recv returns what is queued, then 0
        bool gone = false;   // closed by the client
        std::string out, in;
        std::uint64_t rest = 0;
        std::uint16_t pasv_port = 0;
        int pending_data = -1;
        int transfer = -1;
    };

    struct data_socket
    {
        std::size_t session = 0;
        std::uint64_t pos = 0;
        bool started = false, ended = false, reset = false;
    };

    std::vector<session> m_sessions;
    std::map<int, std::size_t> m_controls;
    std::map<int, data_socket> m_data;
    int m_next_fd = 3;
    std::uint16_t m_next_port = 50000;
    std::mt19937 m_random{1234};
    mutable aes_context m_aes{}; // not copied: the context points into itself
    ds::layout m_table;
    bool m_encrypted = false;

    int connect(const ds::endpoint& to, int& error)
    {
        std::lock_guard lock(mutex);
        if (to.port == control_port && listening.count(to.ipv4))
        {
            const int fd = m_next_fd++;
            m_sessions.push_back({});
            m_sessions.back().out = "220 GoldHEN FTP server v2.2\r\n";
            m_controls[fd] = m_sessions.size() - 1;
            ++connects;
            return fd;
        }
        for (std::size_t i = 0; i < m_sessions.size(); ++i)
        {
            session& s = m_sessions[i];
            if (s.closed || s.gone || !s.pasv_port || s.pasv_port != to.port || !(listening.count(to.ipv4) || to.ipv4 == lan_ip)) continue;
            const int fd = m_next_fd++;
            m_data[fd] = data_socket{i};
            s.pending_data = fd;
            s.pasv_port = 0;
            return fd;
        }
        ++refused_connects;
        error = ECONNREFUSED;
        return -1;
    }

    void end_transfer(session& s, const char* reply)
    {
        if (s.transfer < 0) return;
        m_data[s.transfer].ended = true;
        s.transfer = -1;
        --streams;
        s.out += reply;
    }

    void retr(session& s, std::string_view path)
    {
        if (path != "/dev/cd0")
        {
            s.out += "550 Requested action not taken. File unavailable.\r\n";
            return;
        }
        if (s.pending_data < 0)
        {
            s.out += "425 Can't open data connection.\r\n";
            return;
        }
        const std::uint64_t offset = std::exchange(s.rest, 0);
        retr_offsets.push_back(offset);
        if (no_disc || (refuse_bit31 && (offset & 0x80000000ull)) || (refuse_second_stream && streams > 0) || refuse_rest.count(offset))
        {
            ++retr_550;
            s.out += "550 Requested action not taken. File unavailable.\r\n";
            return;
        }
        s.out += "150 File status okay; about to open data connection.\r\n";
        data_socket& d = m_data[s.pending_data];
        d.started = true;
        d.pos = offset;
        s.transfer = std::exchange(s.pending_data, -1);
        ++retrs;
        max_streams = std::max(max_streams, ++streams);
        if (offset % sector || offset > size) end_transfer(s, "451 Requested action aborted. Local error in processing.\r\n");
        else if (offset == size) end_transfer(s, "226 Closing data connection. Requested file action successful.\r\n");
    }

    void command(session& s, const std::string& line)
    {
        commands.push_back(line);
        if (s.transfer >= 0) ++violations; // GoldHEN would not read it before the transfer ends
        if (drop_control_after >= 0 && drop_control_after-- == 0)
        {
            s.closed = true;
            end_transfer(s, "");
            return;
        }
        const std::string verb = line.substr(0, line.find(' '));
        const std::string argument = line.find(' ') == std::string::npos ? std::string{} : line.substr(line.find(' ') + 1);
        if (verb == "USER" || verb == "PASS") s.out += "230 User logged in, proceed.\r\n";
        else if (verb == "TYPE" || verb == "NOOP") s.out += "200 Command okay.\r\n";
        else if (verb == "QUIT")
        {
            s.out += "221 Service closing control connection.\r\n";
            s.closed = true;
        }
        else if (verb == "PASV")
        {
            s.pasv_port = m_next_port++;
            s.pending_data = -1;
            char text[96];
            std::snprintf(text, sizeof(text), "227 Entering Passive Mode (192,168,1,33,%u,%u).\r\n", s.pasv_port >> 8, s.pasv_port & 0xff);
            s.out += text;
        }
        else if (verb == "REST")
        {
            s.rest = std::stoull(argument);
            s.out += "350 Resuming at " + argument + ".\r\n";
        }
        else if (verb == "RETR") retr(s, argument);
        else if (verb == "FEAT" && feat_silent) return;
        else if (verb == "FEAT" && !feat_reply.empty()) s.out += feat_reply;
        else s.out += "502 Command not implemented.\r\n";
    }

    std::int64_t send(int fd, const void* data, std::size_t n, int& error)
    {
        std::lock_guard lock(mutex);
        const auto found = m_controls.find(fd);
        if (found == m_controls.end()) return static_cast<std::int64_t>(n);
        session& s = m_sessions[found->second];
        if (s.closed)
        {
            error = EPIPE;
            return -1;
        }
        s.in.append(static_cast<const char*>(data), n);
        for (std::size_t eol; !s.closed && (eol = s.in.find("\r\n")) != std::string::npos;)
        {
            const std::string line = s.in.substr(0, eol);
            s.in.erase(0, eol + 2);
            command(s, line);
        }
        return static_cast<std::int64_t>(n);
    }

    std::int64_t recv(int fd, void* out, std::size_t n, int timeout_ms, int& error)
    {
        {
            std::lock_guard lock(mutex);
            if (const auto found = m_controls.find(fd); found != m_controls.end())
            {
                session& s = m_sessions[found->second];
                if (!s.out.empty())
                {
                    const std::size_t k = std::min(n, s.out.size());
                    std::memcpy(out, s.out.data(), k);
                    s.out.erase(0, k);
                    return static_cast<std::int64_t>(k);
                }
                if (s.closed) return 0;
            }
            else if (const auto data = m_data.find(fd); data != m_data.end())
            {
                data_socket& d = data->second;
                session& s = m_sessions[d.session];
                if (d.reset)
                {
                    error = ECONNRESET;
                    return -1;
                }
                if (d.ended) return 0;
                if (d.started && !stall_data)
                {
                    std::size_t limit = std::min<std::size_t>(n, random_recv ? 1 + m_random() % max_recv : max_recv);
                    limit = static_cast<std::size_t>(std::min<std::uint64_t>(limit, size - d.pos));
                    if (fail_read_times && fail_read_at >= d.pos && fail_read_at < d.pos + limit)
                    {
                        limit = static_cast<std::size_t>(fail_read_at - d.pos);
                        if (!limit)
                        {
                            --fail_read_times;
                            end_transfer(s, "451 Requested action aborted. Local error in processing.\r\n");
                            return 0;
                        }
                    }
                    if (drop_data_at >= d.pos && drop_data_at < d.pos + limit)
                    {
                        limit = static_cast<std::size_t>(drop_data_at - d.pos);
                        if (!limit)
                        {
                            drop_data_at = ~0ull;
                            d.reset = true;
                            end_transfer(s, "426 Connection closed; transfer aborted.\r\n");
                            error = ECONNRESET;
                            return -1;
                        }
                    }
                    if (remove_disc_at >= d.pos && remove_disc_at < d.pos + limit)
                    {
                        limit = static_cast<std::size_t>(remove_disc_at - d.pos);
                        if (!limit)
                        {
                            remove_disc_at = ~0ull;
                            no_disc = true;
                            end_transfer(s, "451 Requested action aborted. Local error in processing.\r\n");
                            return 0;
                        }
                    }
                    if (cancel_flag && cancel_at >= d.pos && cancel_at < d.pos + limit)
                    {
                        cancel_at = ~0ull;
                        cancel_flag->store(true);
                    }
                    fill(d.pos, static_cast<unsigned char*>(out), limit);
                    d.pos += limit;
                    bytes_sent += limit;
                    if (d.pos == size) end_transfer(s, "226 Closing data connection. Requested file action successful.\r\n");
                    return static_cast<std::int64_t>(limit);
                }
            }
            else
            {
                error = ENOTCONN;
                return -1;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(std::min(timeout_ms, 2)));
        error = ETIMEDOUT;
        return -1;
    }

    void close(int fd)
    {
        std::lock_guard lock(mutex);
        if (const auto found = m_controls.find(fd); found != m_controls.end())
        {
            session& s = m_sessions[found->second];
            s.gone = true;
            s.closed = true;
            end_transfer(s, "");
            m_controls.erase(found);
            return;
        }
        const auto data = m_data.find(fd);
        assert(data != m_data.end());
        session& s = m_sessions[data->second.session];
        if (s.transfer == fd)
        {
            ++aborted;
            end_transfer(s, "426 Connection closed; transfer aborted.\r\n");
        }
        if (s.pending_data == fd) s.pending_data = -1;
        m_data.erase(data);
    }
};

void le32_at(std::vector<unsigned char>& v, std::size_t at, std::uint32_t value)
{
    for (int i = 0; i < 4; ++i) v[at + i] = static_cast<unsigned char>(value >> (8 * i));
}

void be32_at(std::vector<unsigned char>& v, std::size_t at, std::uint32_t value)
{
    for (int i = 0; i < 4; ++i) v[at + i] = static_cast<unsigned char>(value >> (24 - 8 * i));
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

std::vector<unsigned char> sfo(const std::vector<std::pair<std::string, std::string>>& values)
{
    std::string keys;
    std::string data;
    std::vector<unsigned char> table;
    for (const auto& [key, value] : values)
    {
        std::vector<unsigned char> entry(16, 0);
        entry[0] = static_cast<unsigned char>(keys.size());
        entry[1] = static_cast<unsigned char>(keys.size() >> 8);
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
    std::vector<unsigned char> out(20, 0);
    std::memcpy(out.data(), "\0PSF", 4);
    le32_at(out, 4, 0x101);
    le32_at(out, 8, static_cast<std::uint32_t>(20 + table.size()));
    le32_at(out, 12, static_cast<std::uint32_t>(20 + table.size() + keys.size()));
    le32_at(out, 16, static_cast<std::uint32_t>(values.size()));
    out.insert(out.end(), table.begin(), table.end());
    out.insert(out.end(), keys.begin(), keys.end());
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

// A PS3 disc image: region table (plain 0-0x7ff and the last 64 sectors), PlayStation3 header with 'id', ISO 9660
// PVD at 16 -> root (LBA 20) -> PS3_GAME (LBA 21) -> PARAM.SFO (LBA 22) with TITLE 'title'.
void make_disc(fake_server& server, std::uint64_t bytes, std::string_view id, std::string_view title)
{
    server.size = bytes;
    server.overlay.clear();
    const std::uint32_t last = static_cast<std::uint32_t>(bytes / sector - 1);
    std::vector<unsigned char> s0(sector, 0);
    be32_at(s0, 0, 2);
    be32_at(s0, 8, 0);
    be32_at(s0, 12, 0x7ff);
    be32_at(s0, 16, last - 63);
    be32_at(s0, 20, last);
    server.put(0, s0);
    std::vector<unsigned char> s1(sector, 0);
    std::memcpy(s1.data(), "PlayStation3", 12);
    std::memset(s1.data() + 0x10, ' ', 0x10);
    std::memcpy(s1.data() + 0x10, id.data(), id.size());
    server.put(sector, s1);
    std::vector<unsigned char> pvd(sector, 0);
    pvd[0] = 1;
    std::memcpy(pvd.data() + 1, "CD001", 5);
    const auto root_self = dir_record(std::string_view("\0", 1), 20, sector, true);
    std::memcpy(pvd.data() + 156, root_self.data(), 34);
    server.put(16 * sector, pvd);
    const auto directory = [&](std::uint32_t lba, std::vector<std::vector<unsigned char>> records)
    {
        std::vector<unsigned char> d;
        for (const auto& r : records) d.insert(d.end(), r.begin(), r.end());
        d.resize(sector, 0);
        server.put(lba * sector, d);
    };
    directory(20, {dir_record(std::string_view("\0", 1), 20, sector, true), dir_record(std::string_view("\1", 1), 20, sector, true),
        dir_record("PS3_GAME", 21, sector, true), dir_record("PS3_DISC.SFB;1", 30, 0x800, false)});
    const auto param = sfo({{"CATEGORY", "DG"}, {"TITLE", std::string(title)}, {"TITLE_ID", std::string(id.substr(0, 4)) + std::string(id.substr(5))}});
    directory(21, {dir_record(std::string_view("\0", 1), 21, sector, true), dir_record(std::string_view("\1", 1), 20, sector, true),
        dir_record("ICON0.PNG;1", 40, 100, false), dir_record("PARAM.SFO;1", 22, static_cast<std::uint32_t>(param.size()), false)});
    server.put(22 * sector, param);
}

// The fingerprint of the disc the server holds (sectors 0-1, the PVD and PARAM.SFO, read directly)
std::uint64_t expected_fingerprint(const fake_server& server)
{
    const ds::disc_files files = ds::locate_files([&server](std::uint64_t offset, void* out, std::size_t n)
    {
        server.fill(offset, static_cast<unsigned char*>(out), n);
        return true;
    }, false);
    const auto head = server.expected(0, 2 * sector), pvd = server.expected(16 * sector, sector);
    assert(!files.param.empty());
    return ds::disc_fingerprint(head.data(), head.size(), pvd.data(), pvd.size(), files.param.data(), files.param.size());
}

ds::timeouts fast()
{
    ds::timeouts t;
    t.connect_ms = 300;
    t.reply_ms = 400;
    t.first_byte_ms = 500;
    t.data_ms = 400;
    t.slice_ms = 20;
    return t;
}

ds::reader_config small_config(std::uint32_t cursors = 2)
{
    ds::reader_config c;
    c.chunk_bytes = 64 * 1024;
    c.cache_chunks = 8;
    c.readahead_chunks = 1;
    c.cursors = cursors;
    c.forward_skip_bytes = 256 * 1024;
    c.attempts = 3;
    c.stats_interval_ms = 1'000'000;
    c.net = fast();
    return c;
}

ds::server_list loopback()
{
    return {{{ds::loopback_ipv4, ds::default_port}}, {}, {}};
}

bool read_matches(ds::reader& r, fake_server& server, std::uint64_t offset, std::size_t n, ds::read_status* status = nullptr)
{
    std::vector<unsigned char> got(n, 0xcd);
    ds::read_status st{};
    const std::uint64_t done = r.read(offset, got.data(), n, &st);
    if (status) *status = st;
    return done == n && st == ds::read_status::ok && got == server.expected(offset, n);
}

void test_pure()
{
    // Replies
    std::string buffer = "220 GoldHEN FTP server v2.2\r\n230 User logged in, proceed.\r\n211-Extensions:\r\n MDTM\r\n REST STREAM\r\n211 END\r\n150 partial";
    auto r = ds::take_reply(buffer);
    assert(r && r->code == 220 && r->text == "220 GoldHEN FTP server v2.2");
    r = ds::take_reply(buffer);
    assert(r && r->code == 230);
    r = ds::take_reply(buffer);
    assert(r && r->code == 211 && r->text == "211-Extensions:");
    assert(!ds::take_reply(buffer) && buffer == "150 partial");
    buffer += "\n";
    r = ds::take_reply(buffer);
    assert(r && r->code == 150 && buffer.empty());
    buffer = "garbage\r\n";
    r = ds::take_reply(buffer);
    assert(r && r->code == -1);

    // PASV (curl log of the console) and endpoints
    std::uint32_t host = 0;
    std::uint16_t port = 0;
    assert(ds::parse_pasv("227 Entering Passive Mode (192,168,1,33,195,226).", host, port) && host == lan_ip && port == 50146);
    assert(!ds::parse_pasv("227 Entering Passive Mode (192,168,1,33,195).", host, port));
    assert(!ds::parse_pasv("227 Entering Passive Mode (192,168,1,330,1,1).", host, port));
    assert(!ds::parse_pasv("227 no numbers", host, port));
    auto e = ds::parse_endpoint("192.168.1.33:2121\r\n");
    assert(e && e->ipv4 == lan_ip && e->port == 2121 && ds::to_string(*e) == "192.168.1.33:2121");
    e = ds::parse_endpoint("  127.0.0.1 ");
    assert(e && e->ipv4 == ds::loopback_ipv4 && e->port == 2121);
    e = ds::parse_endpoint("ftp://10.0.0.5:21/");
    assert(e && e->ipv4 == 0x0a000005 && e->port == 21);
    e = ds::parse_endpoint("\xef\xbb\xbf" "192.168.1.33:2121");
    assert(e && e->ipv4 == lan_ip);
    for (const char* bad : {"", "host:2121", "1.2.3:21", "1.2.3.4:0", "1.2.3.4:65536", "1.2.3.4:x", "256.1.1.1", "0.0.0.0", "1.2.3.4.5"})
        assert(!ds::parse_endpoint(bad));

    // 2 GiB boundaries as measured: 550 at 0x80000000 0x80000800 0xbffff800 0xfffff800 0x180000000 0x1bffff800
    // 0x1fffff800 0xc0000000; 150 at 0x7ffff800 0x17ffff800 0x100000000 0x200000000 0x260000000 0x278fe8000
    for (const std::uint64_t refused : {0x80000000ull, 0x80000800ull, 0xbffff800ull, 0xfffff800ull, 0x180000000ull, 0x1bffff800ull, 0x1fffff800ull, 0xc0000000ull})
    {
        assert(ds::rest_refused_at(refused));
        const std::uint64_t floor = ds::rest_floor(refused);
        assert(!ds::rest_refused_at(floor) && floor < refused && refused - floor <= 0x80000000ull && floor % sector == 0);
        assert(ds::rest_refused_at(floor + sector));
    }
    for (const std::uint64_t ok : {0ull, 0x7ffff800ull, 0x17ffff800ull, 0x100000000ull, 0x200000000ull, 0x260000000ull, 0x278fe8000ull})
        assert(!ds::rest_refused_at(ok));
    assert(ds::rest_floor(0x80000000ull) == 0x7ffff800ull && ds::rest_floor(0x1bffff800ull) == 0x17ffff800ull);

    // FIFA 14 (BLES01876) sector 0 as read through the FTP server
    const unsigned char fifa[] = {0, 0, 0, 4, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x08, 0x9f, 0, 0x4c, 0xfa, 0x60, 0, 0x4c, 0xfa, 0x7f, 0, 0x4c, 0xfc, 0xa0,
        0, 0x4c, 0xff, 0x3f, 0, 0x4d, 0x1f, 0xc0, 0, 0x4f, 0x1f, 0xdf};
    std::vector<unsigned char> s0(sector, 0);
    std::memcpy(s0.data(), fifa, sizeof(fifa));
    const ds::layout fl = ds::parse_layout(s0.data(), s0.size());
    assert(fl.valid && fl.plain_regions == 4 && fl.end_lba == 0x4f1fe0 && fl.size_bytes() == 10'619'912'192ull);
    assert(fl.plain.size() == 4 && fl.plain[1] == std::make_pair(0x4cfa60u, 0x4cfa7fu));
    std::vector<unsigned char> bad = s0;
    bad[3] = 0;
    assert(!ds::parse_layout(bad.data(), bad.size()).valid);
    bad = s0;
    bad[3] = 128;
    assert(!ds::parse_layout(bad.data(), bad.size()).valid);
    bad = s0;
    bad[18] = 0x08; // the second region (LBA 0x800) starts inside the first (0-0x89f)
    bad[17] = 0;
    bad[19] = 0;
    assert(!ds::parse_layout(bad.data(), bad.size()).valid);
    bad = s0;
    bad[11] = 1; // the first region does not start at LBA 0
    assert(!ds::parse_layout(bad.data(), bad.size()).valid);
    assert(!ds::parse_layout(s0.data(), 12).valid);

    // Title ids and the header
    std::vector<unsigned char> s1(sector, 0);
    std::memcpy(s1.data(), "PlayStation3\0\0\0\0BLES-01876      ", 32);
    assert(ds::title_id_from_header(s1.data(), s1.size()) == "BLES01876");
    assert(probe_ns::title_id_from_header(s1.data(), s1.size()) == "BLES01876"); // the probe's name for the same function
    assert(ds::canonical_title_id("bles-01876") == "BLES01876" && ds::canonical_title_id("BLES-0187").empty());

    // PARAM.SFO and the ISO 9660 records
    const auto param = sfo({{"TITLE", "FIFA 14"}, {"TITLE_ID", "BLES01876"}});
    assert(ds::sfo_string(param.data(), param.size(), "TITLE") == "FIFA 14");
    assert(ds::sfo_string(param.data(), param.size(), "TITLE_ID") == "BLES01876");
    assert(ds::sfo_string(param.data(), param.size(), "VERSION").empty());
    assert(ds::sfo_string(param.data(), 10, "TITLE").empty());
    auto broken = param;
    le32_at(broken, 12, 0xfffffff0);
    assert(ds::sfo_string(broken.data(), broken.size(), "TITLE").empty());
    std::vector<unsigned char> dir;
    for (const auto& rec : {dir_record(std::string_view("\0", 1), 21, 2048, true), dir_record("PARAM.SFO;1", 545, 1092, false)})
        dir.insert(dir.end(), rec.begin(), rec.end());
    dir.resize(4096, 0);
    const auto rec = dir_record("EBOOT.BIN;1", 9000, 4, false);
    std::memcpy(dir.data() + 2048, rec.data(), rec.size()); // a record in the second sector after padding
    auto found = ds::find_entry(dir.data(), dir.size(), "param.sfo");
    assert(found && found->lba == 545 && found->bytes == 1092 && !found->directory);
    found = ds::find_entry(dir.data(), dir.size(), "EBOOT.BIN");
    assert(found && found->lba == 9000);
    assert(!ds::find_entry(dir.data(), dir.size(), "ICON0.PNG"));
    dir[0] = 20; // a record shorter than 34 bytes
    assert(!ds::find_entry(dir.data(), dir.size(), "PARAM.SFO"));
}

void test_reader_basics()
{
    fake_server server;
    make_disc(server, 64ull << 20, "BLES-01876", "FIFA 14");
    log_sink logs;
    ds::reader r(server.ops(), loopback(), small_config(), logs.fn());
    r.set_size(server.size);

    // Sectors 0-1, then sequential reads: one transfer serves them, the next chunk comes ahead
    assert(read_matches(r, server, 0, 2 * sector));
    assert(logs.has("disc stream: connected to 127.0.0.1:2121") && logs.has("GoldHEN FTP server v2.2") && logs.has("disc stream: first read: LBA 0x0"));
    for (std::uint64_t at = 2 * sector; at < 1024 * 1024; at += 48 * 1024) assert(read_matches(r, server, at, 48 * 1024));
    ds::reader_stats st = r.stats();
    assert(server.retr_count() == 1 && st.opens == 1 && st.readahead > 0 && st.cache_hits > 0);

    // Unaligned, across chunks; the cache answers a re-read without the network
    assert(read_matches(r, server, 100, 200'000));
    const unsigned before = server.retr_count();
    assert(read_matches(r, server, 1000, 5000));
    assert(server.retr_count() == before);

    // A small gap forward is read through, a far seek and a backward seek start new transfers
    const std::uint64_t far = 32ull << 20;
    assert(read_matches(r, server, far, 4096));
    const unsigned at_far = server.retr_count();
    assert(read_matches(r, server, far + 192 * 1024, 4096)); // 128 KiB past where that stream stands
    assert(server.retr_count() == at_far);
    assert(read_matches(r, server, 8ull << 20, 4096));
    assert(read_matches(r, server, 4ull << 20, 4096));
    assert(server.retr_count() > at_far);

    // End of the disc: the last bytes, a range past it, and the read at the end
    assert(read_matches(r, server, server.size - 3000, 3000));
    std::vector<unsigned char> tail(8192);
    ds::read_status status{};
    assert(r.read(server.size - 4096, tail.data(), tail.size(), &status) == 4096 && status == ds::read_status::ok);
    assert(r.read(server.size, tail.data(), 10, &status) == 0 && status == ds::read_status::out_of_range);

    // Protocol: never a command while a transfer runs, ABOR never sent, at most one stream per cursor
    std::lock_guard lock(server.mutex);
    assert(server.violations == 0 && server.max_streams <= 2);
    for (const auto& c : server.commands) assert(c.rfind("ABOR", 0) != 0 && c.rfind("EPSV", 0) != 0);
    assert(server.aborted > 0); // transfers ended by closing the data connection
}

void test_reader_size_unknown_and_partial()
{
    fake_server server;
    make_disc(server, (8ull << 20) + 6 * sector, "BLUS-30443", "Game");
    server.random_recv = true;
    server.max_recv = 3000;
    ds::reader r(server.ops(), loopback(), small_config());
    // No size known: the server's 226 ends the disc inside the last chunk
    std::vector<unsigned char> buf(256 * 1024);
    ds::read_status status{};
    const std::uint64_t start = server.size - 100 * 1024;
    const std::uint64_t got = r.read(start, buf.data(), buf.size(), &status);
    assert(got == 100 * 1024 && status == ds::read_status::out_of_range);
    buf.resize(static_cast<std::size_t>(got));
    assert(buf == server.expected(start, buf.size()));
    // Small, random recv sizes do not change the data
    for (std::uint64_t at = 0; at < (2u << 20); at += 77'777) assert(read_matches(r, server, at, 50'000));
}

void test_boundaries()
{
    // A disc larger than 4 GiB; the server refuses REST with bit 31 set
    fake_server server;
    make_disc(server, (5ull << 30) + 64 * sector, "BLES-01876", "FIFA 14");
    log_sink logs;
    ds::reader r(server.ops(), loopback(), small_config(), logs.fn());
    r.set_size(server.size);
    assert(r.floor_mode() == -1);

    const std::uint64_t in_range = (2ull << 30) + (1u << 20); // 2 GiB + 1 MiB
    assert(read_matches(r, server, in_range, 4096));
    assert(r.floor_mode() == 1);
    assert(logs.has("refuses to start a transfer at an offset with bit 31 set"));
    ds::reader_stats st = r.stats();
    assert(st.floor_opens == 1 && st.skipped >= (1u << 20));
    unsigned refusals = 0;
    {
        std::lock_guard lock(server.mutex);
        refusals = server.retr_550;
    }
    assert(refusals == 1);

    // A read below the boundary goes to the other cursor: the stream parked past the boundary stays
    assert(read_matches(r, server, 1u << 20, 4096));
    const std::uint64_t skipped = r.stats().skipped;
    assert(read_matches(r, server, in_range + (3u << 20), 4096)); // 3 MiB further: read forward on the parked stream
    st = r.stats();
    assert(st.floor_opens == 1 && st.skipped - skipped >= (2u << 20));
    {
        std::lock_guard lock(server.mutex);
        assert(server.retr_550 == refusals); // learned once: no second 550
    }

    // Backwards inside the range: the cached chunk, then a new transfer from the sector below the boundary, no 550
    assert(read_matches(r, server, in_range + 4096, 4096));
    assert(r.stats().floor_opens == 1);
    assert(read_matches(r, server, (2ull << 30) + 64 * 1024, 4096));
    assert(r.stats().floor_opens == 2);
    // Above 4 GiB the server starts directly again; the last 2 GiB range again via its floor
    assert(read_matches(r, server, (4ull << 30) + 4096 * 3, 4096));
    assert(r.stats().floor_opens == 2);
    assert(logs.has("lies past a 2 GiB boundary") == false); // gaps under 8 MiB are not logged

    // A far target past a boundary is logged with the amount read forward
    fake_server big;
    make_disc(big, (3ull << 30), "BLES-01876", "FIFA 14");
    log_sink big_logs;
    ds::reader rb(big.ops(), loopback(), small_config(), big_logs.fn());
    rb.set_size(big.size);
    assert(read_matches(rb, big, (2ull << 30) + (20u << 20), 4096));
    assert(big_logs.has("lies past a 2 GiB boundary the server cannot start at: reading 20.0 MiB forward from LBA 0xfffff"));

    // A server that starts anywhere is used directly
    fake_server fixed;
    make_disc(fixed, (5ull << 30), "BLES-01876", "FIFA 14");
    fixed.refuse_bit31 = false;
    ds::reader rf(fixed.ops(), loopback(), small_config());
    rf.set_size(fixed.size);
    assert(read_matches(rf, fixed, (3ull << 30) + 8192, 4096));
    assert(rf.floor_mode() == 0 && rf.stats().floor_opens == 0 && rf.stats().skipped == 0);
}

void test_faults()
{
    {
        // The data connection resets mid-read: the next attempt reconnects and the data is right
        fake_server server;
        make_disc(server, 64ull << 20, "BLES-01876", "FIFA 14");
        server.drop_data_at = 100'000;
        log_sink logs;
        ds::reader r(server.ops(), loopback(), small_config(), logs.fn());
        r.set_size(server.size);
        assert(read_matches(r, server, 0, 300'000));
        assert(r.stats().errors >= 1 && logs.has("failed (connection closed by the server)"));
    }
    {
        // The device fails once at an offset (451): retried; failing every time: the read fails, bounded
        fake_server server;
        make_disc(server, 64ull << 20, "BLES-01876", "FIFA 14");
        server.fail_read_at = 3 * 64 * 1024 + 2048;
        server.fail_read_times = 1;
        ds::reader r(server.ops(), loopback(), small_config());
        r.set_size(server.size);
        assert(read_matches(r, server, 3 * 64 * 1024, 64 * 1024));
        server.fail_read_at = 9 * 64 * 1024;
        server.fail_read_times = 100;
        std::vector<unsigned char> buf(4096);
        ds::read_status status{};
        const auto started = std::chrono::steady_clock::now();
        assert(r.read(9 * 64 * 1024, buf.data(), buf.size(), &status) == 0 && status == ds::read_status::failed);
        assert(std::chrono::steady_clock::now() - started < 5s);
        assert(r.last_failure().find("451") != std::string::npos);
    }
    {
        // The server dropped an idle session (its transfer had ended at the end of the disc): the next transfer
        // reconnects at once, without a failed attempt
        fake_server server;
        make_disc(server, 64ull << 20, "BLES-01876", "FIFA 14");
        ds::reader r(server.ops(), loopback(), small_config(1));
        r.set_size(server.size);
        assert(read_matches(r, server, server.size - 4096, 4096));
        server.drop_idle_sessions();
        assert(read_matches(r, server, 40ull << 20, 4096));
        assert(r.stats().errors == 0 && r.stats().connects == 2);
    }
    {
        // The control connection is closed by the server on a command: reconnect, data right
        fake_server server;
        make_disc(server, 64ull << 20, "BLES-01876", "FIFA 14");
        ds::reader r(server.ops(), loopback(), small_config(1));
        r.set_size(server.size);
        assert(read_matches(r, server, 0, 4096));
        server.drop_control_after = 1;
        assert(read_matches(r, server, 50ull << 20, 4096));
        assert(read_matches(r, server, 20ull << 20, 4096));
    }
    {
        // A server that opens the device for one transfer at a time: 550 on the second stream -> one stream
        fake_server server;
        make_disc(server, 64ull << 20, "BLES-01876", "FIFA 14");
        server.refuse_second_stream = true;
        log_sink logs;
        ds::reader r(server.ops(), loopback(), small_config(2), logs.fn());
        r.set_size(server.size);
        assert(read_matches(r, server, 0, 4096));
        assert(read_matches(r, server, 30ull << 20, 4096));
        assert(read_matches(r, server, 10ull << 20, 4096));
        assert(logs.has("using one stream at a time"));
        std::lock_guard lock(server.mutex);
        assert(server.max_streams == 1);
    }
    {
        // No disc: RETR 550 at sector 0
        fake_server server;
        make_disc(server, 64ull << 20, "BLES-01876", "FIFA 14");
        server.no_disc = true;
        log_sink logs;
        ds::reader r(server.ops(), loopback(), small_config(), logs.fn());
        std::vector<unsigned char> buf(4096);
        ds::read_status status{};
        assert(r.read(0, buf.data(), buf.size(), &status) == 0 && status == ds::read_status::refused);
        // Checked again and again (the XMB checks every few seconds): one line, not one per attempt
        for (int i = 0; i < 3; ++i) assert(r.read_fresh(0, buf.data(), buf.size()) == ds::read_status::refused);
        assert(logs.count("RETR at LBA 0x0 refused: refused: '550 Requested action not taken. File unavailable.'") == 1);
        // A disc comes: reads work, and the next failure is logged again
        server.no_disc = false;
        make_disc(server, 64ull << 20, "BLES-01876", "FIFA 14");
        r.set_size(server.size);
        assert(read_matches(r, server, 0, 4096));
        server.no_disc = true;
        assert(r.read_fresh(0, buf.data(), buf.size()) == ds::read_status::refused);
        assert(logs.count("RETR at LBA 0x0 refused") == 2);
    }
    {
        // No server; then the fallback (the console's LAN address) when the loopback is refused
        fake_server server;
        make_disc(server, 64ull << 20, "BLES-01876", "FIFA 14");
        server.listening = {lan_ip};
        log_sink logs;
        ds::reader none(server.ops(), loopback(), small_config(), logs.fn());
        std::vector<unsigned char> buf(4096);
        ds::read_status status{};
        assert(none.read(0, buf.data(), buf.size(), &status) == 0 && status == ds::read_status::no_server);
        assert(logs.count("disc stream: no FTP server: 127.0.0.1:2121: connect failed: errno") == 1); // once, not per attempt
        unsigned asked = 0;
        ds::server_list list = loopback();
        list.fallback = [&asked]
        {
            ++asked;
            return std::optional<ds::endpoint>(ds::endpoint{lan_ip, ds::default_port});
        };
        ds::reader r(server.ops(), list, small_config());
        r.set_size(server.size);
        assert(read_matches(r, server, 0, 4096) && asked == 1 && ds::to_string(r.server()) == "192.168.1.33:2121");
        assert(read_matches(r, server, 30ull << 20, 4096) && asked == 1);
    }
    {
        // The data never comes: the read fails within the time limits; stop() ends a waiting read at once
        fake_server server;
        make_disc(server, 64ull << 20, "BLES-01876", "FIFA 14");
        server.stall_data = true;
        ds::reader r(server.ops(), loopback(), small_config());
        r.set_size(server.size);
        std::vector<unsigned char> buf(4096);
        ds::read_status status{};
        auto started = std::chrono::steady_clock::now();
        assert(r.read(0, buf.data(), buf.size(), &status) == 0 && status == ds::read_status::failed);
        assert(std::chrono::steady_clock::now() - started < 6s);
        std::atomic<bool> done{false};
        std::thread reader_thread([&] {
            ds::read_status s{};
            std::vector<unsigned char> b(4096);
            r.read(8192 * 64, b.data(), b.size(), &s);
            assert(s == ds::read_status::stopped || s == ds::read_status::failed);
            done = true;
        });
        std::this_thread::sleep_for(100ms);
        started = std::chrono::steady_clock::now();
        r.stop();
        reader_thread.join();
        assert(done && std::chrono::steady_clock::now() - started < 2s);
        assert(r.read(0, buf.data(), buf.size(), &status) == 0 && status == ds::read_status::stopped);
    }
}

void test_threads()
{
    fake_server server;
    make_disc(server, 64ull << 20, "BLES-01876", "FIFA 14");
    server.random_recv = true;
    server.max_recv = 20'000;
    ds::reader_config cfg = small_config(2);
    cfg.cache_chunks = 6;
    ds::reader r(server.ops(), loopback(), cfg);
    r.set_size(server.size);
    std::atomic<unsigned> bad{0}, total{0};
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < 8; ++t)
    {
        threads.emplace_back([&, t] {
            std::mt19937_64 random(t * 7919 + 1);
            for (int i = 0; i < 60; ++i)
            {
                // Mostly sequential runs from a few places, some random reads
                const std::uint64_t base = (random() % 4) * (12ull << 20) + (i % 2 ? random() % (6u << 20) : static_cast<std::uint64_t>(i) * 70'000);
                const std::size_t n = 1 + static_cast<std::size_t>(random() % (200 * 1024));
                if (!read_matches(r, server, std::min<std::uint64_t>(base, server.size - n), n)) ++bad;
                ++total;
            }
        });
    }
    for (auto& t : threads) t.join();
    assert(bad == 0 && total == 480);
    std::lock_guard lock(server.mutex);
    assert(server.violations == 0 && server.max_streams <= 2);
}

void test_drive()
{
    fake_server server;
    make_disc(server, 96ull << 20, "BLES-01876", "FIFA 14");
    log_sink logs;
    std::mutex keys_mutex;
    std::set<std::string> keys;
    const auto exists = [&](const std::string& path)
    {
        std::lock_guard lock(keys_mutex);
        return keys.count(path) > 0;
    };
    ds::drive_config cfg;
    cfg.check_ms_searching = 50;
    cfg.check_ms_present = 100'000;
    cfg.worker_idle_ms = 300;
    auto r = std::make_unique<ds::reader>(server.ops(), loopback(), small_config(), logs.fn());
    ds::drive d(std::move(r), exists, "/data/rpcs3/data/redump/", cfg, logs.fn());

    const auto wait_for = [&](auto&& done)
    {
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        for (;;)
        {
            const ds::disc_status s = d.poll();
            if (done(s)) return s;
            assert(std::chrono::steady_clock::now() < deadline);
            std::this_thread::sleep_for(5ms);
        }
    };

    // First poll: nothing known yet, the worker reads the disc
    ds::disc_status s = d.poll();
    assert(s.state == ds::disc_state::unknown);
    s = wait_for([](const ds::disc_status& x) { return x.state != ds::disc_state::unknown; });
    assert(s.state == ds::disc_state::ps3_disc && s.title_id == "BLES01876" && s.title == "FIFA 14");
    assert(s.plain_regions == 2 && s.size == server.size && d.disc().size() == server.size);
    assert(!s.key_present && s.key_path == "/data/rpcs3/data/redump/BLES01876.dkey");
    assert(s.server == "127.0.0.1:2121" && s.banner == "GoldHEN FTP server v2.2" && s.generation == 1);
    assert(logs.has("disc stream: drive: PS3 disc BLES01876 'FIFA 14', 2 plain regions, 96.0 MiB; key /data/rpcs3/data/redump/BLES01876.dkey missing"));
    // The fingerprint (disc lock) and PARAM.SFO's TITLE_ID come with the title
    const std::uint64_t fifa_fingerprint = expected_fingerprint(server);
    assert(s.fingerprint == fifa_fingerprint && s.fingerprint != 0 && s.sfo_title_id == "BLES01876");

    // The key arrives: seen on the next check, which reads sectors 0-1 only (the title, the PARAM.SFO id and the
    // fingerprint of the same disc are kept)
    std::size_t transfers = 0;
    {
        std::lock_guard lock(server.mutex);
        transfers = server.retr_offsets.size();
    }
    {
        std::lock_guard lock(keys_mutex);
        keys.insert("/data/rpcs3/data/redump/BLES01876.dkey");
    }
    d.request_check();
    s = wait_for([](const ds::disc_status& x) { return x.key_present; });
    assert(s.generation == 1 && s.title == "FIFA 14" && s.fingerprint == fifa_fingerprint && s.sfo_title_id == "BLES01876");
    {
        std::lock_guard lock(server.mutex);
        assert(server.retr_offsets.size() > transfers);
        for (std::size_t i = transfers; i < server.retr_offsets.size(); ++i) assert(server.retr_offsets[i] == 0);
    }

    // Another disc: new generation, title read again, the cache of the old disc is not served
    std::vector<unsigned char> old_chunk(4096);
    assert(d.disc().read(64 * 1024, old_chunk.data(), old_chunk.size()) == old_chunk.size());
    {
        std::lock_guard lock(server.mutex); // the worker may be reading
        make_disc(server, 80ull << 20, "BLUS-30443", "Demon's Souls");
        server.put(64 * 1024, std::vector<unsigned char>(4096, 0x5a));
    }
    d.request_check();
    s = wait_for([](const ds::disc_status& x) { return x.generation == 2; });
    assert(s.state == ds::disc_state::ps3_disc && s.title_id == "BLUS30443" && s.title == "Demon's Souls" && !s.key_present && s.size == server.size);
    assert(s.fingerprint == expected_fingerprint(server) && s.fingerprint != fifa_fingerprint && s.sfo_title_id == "BLUS30443");
    std::vector<unsigned char> new_chunk(4096);
    assert(d.disc().read(64 * 1024, new_chunk.data(), new_chunk.size()) == new_chunk.size());
    assert(new_chunk == std::vector<unsigned char>(4096, 0x5a));

    // Not a PS3 disc, no disc, no server
    {
        std::lock_guard lock(server.mutex);
        server.overlay[sector][0] = 'X';
    }
    d.request_check();
    s = wait_for([](const ds::disc_status& x) { return x.state == ds::disc_state::not_ps3; });
    assert(s.detail.find("PlayStation3") != std::string::npos);
    {
        std::lock_guard lock(server.mutex);
        server.no_disc = true;
    }
    d.request_check();
    s = wait_for([](const ds::disc_status& x) { return x.state == ds::disc_state::no_disc; });
    assert(s.detail.find("550") != std::string::npos);
    {
        std::lock_guard lock(server.mutex);
        server.no_disc = false;
        server.listening.clear();
    }
    d.disc().close_all();
    d.request_check();
    s = wait_for([](const ds::disc_status& x) { return x.state == ds::disc_state::no_server; });
    assert(logs.has("disc stream: drive: no FTP server"));
    // Without a link probe the cause is unknown: the 0.39 texts
    assert(s.net_link == -1 && !ds::no_network(s) && logs.has("disc stream: drive: no FTP server (network state unknown): "));

    // Every server refused while NetCtl has no IP address: the network is missing (GoldHEN's FTP server does not listen
    // without one), told apart from an FTP server switched off on a console with a network; each change of the cause is
    // one line, the drive row says which
    std::atomic<int> link{1};
    d.set_link_probe([&link] { return link.load(); });
    d.request_check();
    s = wait_for([](const ds::disc_status& x) { return x.net_link == 1; });
    assert(s.state == ds::disc_state::no_server && ds::no_network(s));
    assert(logs.has("disc stream: drive: no FTP server (no network: netctl state 1): 127.0.0.1:2121: connect failed"));
    assert(di::drive_row(s, nullptr).label == "Disco de PS3: sem rede, o FTP do GoldHEN não atende");
    assert(di::drive_row(s, nullptr).help.find("não precisa de internet") != std::string::npos);
    const std::size_t lines_before = logs.count("disc stream: drive: no FTP server");
    d.request_check();
    s = wait_for([&](const ds::disc_status& x) { return x.checked_ms > s.checked_ms; });
    assert(s.net_link == 1 && logs.count("disc stream: drive: no FTP server") == lines_before); // no change: no line
    link = ds::netctl_ip_obtained;
    d.request_check();
    s = wait_for([](const ds::disc_status& x) { return x.net_link == ds::netctl_ip_obtained; });
    assert(s.state == ds::disc_state::no_server && !ds::no_network(s));
    assert(logs.has("disc stream: drive: no FTP server (network up: GoldHEN FTP off?): "));
    assert(di::drive_row(s, nullptr).label == "Disco de PS3: ligue o FTP do GoldHEN");
    // The disc lines that go to RPCS3.log as well: nothing without a sink, every line given to one
    static std::vector<std::string> mirrored;
    ds::mirror("disc stream: drive: test");
    ds::log_mirror.store([](std::string_view line) { mirrored.emplace_back(line); });
    ds::mirror("disc stream: drive: test");
    ds::log_mirror.store(nullptr);
    ds::mirror("disc stream: drive: test 2");
    assert(mirrored == std::vector<std::string>{"disc stream: drive: test"});

    // The worker ends once poll() is no longer called
    assert(d.wait_idle(5000));

    // check_now on the caller's thread (generations: FIFA 1, Demon's Souls 2, the altered header 3, FIFA again 4)
    {
        std::lock_guard lock(server.mutex);
        server.listening = {ds::loopback_ipv4};
        make_disc(server, 96ull << 20, "BLES-01876", "FIFA 14");
    }
    s = d.check_now();
    assert(s.state == ds::disc_state::ps3_disc && s.key_present && s.generation == 4 && s.title == "FIFA 14");
    assert(logs.has("disc stream: reconnected to 127.0.0.1:2121"));
}

// request_check() while a check runs (the disc lock asks for one when X is pressed): the running check may have read the
// drive before the request, so another check follows it at once instead of the 60 s cadence
void test_drive_request_during_check()
{
    fake_server server;
    make_disc(server, 64ull << 20, "BLES-01876", "FIFA 14");
    log_sink logs;
    std::mutex mutex;
    std::condition_variable changed;
    bool hold = false, inside = false, release = false;
    std::atomic<unsigned> checks{0};
    // Called once per check of a PS3 disc, after sectors 0-1 were read
    const auto exists = [&](const std::string& path)
    {
        if (!path.ends_with(".dkey")) return false;
        ++checks;
        std::unique_lock lock(mutex);
        if (std::exchange(hold, false))
        {
            inside = true;
            changed.notify_all();
            changed.wait(lock, [&] { return release; });
        }
        return false;
    };
    ds::drive_config cfg;
    cfg.check_ms_present = 100'000;
    cfg.worker_idle_ms = 5000;
    ds::drive d(std::make_unique<ds::reader>(server.ops(), loopback(), small_config(), logs.fn()), exists, "/data/rpcs3/data/redump/", cfg, logs.fn());
    const auto wait_checks = [&](unsigned count)
    {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (checks.load() < count)
        {
            d.poll();
            assert(std::chrono::steady_clock::now() < deadline);
            std::this_thread::sleep_for(5ms);
        }
    };
    d.poll();
    wait_checks(1);
    {
        std::lock_guard lock(mutex);
        hold = true;
    }
    d.request_check();
    {
        std::unique_lock lock(mutex);
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!inside)
        {
            lock.unlock();
            d.poll();
            lock.lock();
            changed.wait_until(lock, std::min(deadline, std::chrono::steady_clock::now() + 20ms));
            assert(std::chrono::steady_clock::now() < deadline);
        }
    }
    d.request_check(); // arrives while that check runs
    {
        std::lock_guard lock(mutex);
        release = true;
        changed.notify_all();
    }
    wait_checks(3);
    std::this_thread::sleep_for(200ms);
    d.poll();
    assert(checks.load() == 3); // and then the cadence again
}

void test_read_header_and_probe()
{
    fake_server server;
    make_disc(server, 96ull << 20, "BLES-01876", "FIFA 14");
    ds::header_read h = ds::read_header(server.ops(), loopback(), fast(), 3000);
    assert(h.kind == ds::header_read::result::ok && h.pvd_ok && h.banner == "GoldHEN FTP server v2.2" && h.sectors.size() == 3 * sector);
    const auto head = server.expected(0, 2 * sector), pvd = server.expected(16 * sector, sector);
    assert(std::equal(head.begin(), head.end(), h.sectors.begin()));
    assert(std::equal(pvd.begin(), pvd.end(), h.sectors.begin() + 2 * sector));
    {
        std::lock_guard lock(server.mutex);
        assert(server.retrs == 2 && server.violations == 0);
    }

    fake_server down;
    down.listening.clear();
    h = ds::read_header(down.ops(), loopback(), fast(), 3000);
    assert(h.kind == ds::header_read::result::no_server && h.tried.find("127.0.0.1:2121: connect failed") != std::string::npos);

    fake_server lan;
    make_disc(lan, 96ull << 20, "BLES-01876", "FIFA 14");
    lan.listening = {lan_ip};
    ds::server_list list = loopback();
    list.fallback = [] { return std::optional<ds::endpoint>(ds::endpoint{lan_ip, ds::default_port}); };
    h = ds::read_header(lan.ops(), list, fast(), 3000);
    assert(h.kind == ds::header_read::result::ok && ds::to_string(h.server) == "192.168.1.33:2121");

    fake_server empty;
    make_disc(empty, 96ull << 20, "BLES-01876", "FIFA 14");
    empty.no_disc = true;
    h = ds::read_header(empty.ops(), loopback(), fast(), 3000);
    assert(h.kind == ds::header_read::result::refused && h.code == 550);

    // Probe stage 3: no node in the sandbox, the sectors through the FTP server
    const auto run_probe = [](fake_server& srv, log_sink& logs)
    {
        probe_ns::ops os;
        os.list_names = [](const std::string&, std::vector<std::string>& names) { names = {"null", "zero"}; return 0; };
        os.stat_path = [](const std::string&) { probe_ns::node_info i; i.error = ENOENT; return i; };
        os.write_marker = [](const std::string&) { return 0; };
        os.remove_marker = [](const std::string&) { return 0; };
        os.open_read = [](const std::string&, int& e) { e = ENOENT; assert(false); return -1; };
        os.ftp_read = [&srv] { return ds::read_header(srv.ops(), loopback(), fast(), 3000); };
        auto probe = probe_ns::probe::start(os, logs.fn());
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        std::optional<probe_ns::report> report;
        while (!(report = probe->poll()))
        {
            assert(std::chrono::steady_clock::now() < deadline);
            std::this_thread::sleep_for(2ms);
        }
        // The worker logs its last line after the report is out: wait until it let go of the probe
        const std::weak_ptr<probe_ns::probe> weak = probe;
        probe.reset();
        while (!weak.expired())
        {
            assert(std::chrono::steady_clock::now() < deadline);
            std::this_thread::sleep_for(2ms);
        }
        return *report;
    };
    {
        log_sink logs;
        const probe_ns::report rep = run_probe(server, logs);
        assert(rep.kind == probe_ns::outcome::ps3_disc && rep.via_ftp && rep.title_id == "BLES01876" && rep.plain_regions == 2 && rep.server == "127.0.0.1:2121");
        assert(logs.has("disc probe: no optical node visible to this app"));
        assert(logs.has("disc probe: stage 3: FTP: sectors 0-1 and 16 of /dev/cd0 through the GoldHEN FTP server"));
        assert(logs.has("disc probe: stage 3: FTP 127.0.0.1:2121 (GoldHEN FTP server v2.2): login"));
        assert(logs.has("disc probe: stage 3: sector 0: 2 plain regions (3 in all)"));
        assert(logs.has("disc probe: stage 3: sector 16: 'CD001' present"));
        assert(logs.has("disc probe: RAW READ OK (PS3 disc BLES01876, 2 plain regions) via FTP 127.0.0.1:2121"));
        assert(logs.has("disc-probe.blocked removed"));
        const std::string text = probe_ns::report_text(rep);
        assert(text.find("BLES01876") != std::string::npos && text.find("FTP") != std::string::npos && text.size() < 140);
    }
    {
        log_sink logs;
        const probe_ns::report rep = run_probe(down, logs);
        assert(rep.kind == probe_ns::outcome::ftp_no_server && rep.error == ECONNREFUSED);
        assert(logs.has("disc probe: stage 3: FTP: no server: 127.0.0.1:2121: connect failed"));
        const std::string text = probe_ns::report_text(rep);
        assert(text.find("ligue o FTP") != std::string::npos && text.size() < 140);
    }
    {
        log_sink logs;
        const probe_ns::report rep = run_probe(empty, logs);
        assert(rep.kind == probe_ns::outcome::ftp_refused && rep.error == 550);
        assert(logs.has("RAW READ REFUSED: refused: '550 Requested action not taken. File unavailable.'"));
        const std::string text = probe_ns::report_text(rep);
        assert(text.find("resposta 550") != std::string::npos && text.size() < 140);
    }
    {
        // Not a PS3 disc through FTP
        fake_server other;
        make_disc(other, 96ull << 20, "BLES-01876", "FIFA 14");
        other.overlay[sector][0] = 'X';
        log_sink logs;
        const probe_ns::report rep = run_probe(other, logs);
        assert(rep.kind == probe_ns::outcome::other_disc && rep.via_ftp);
        assert(logs.has("RAW READ OK but NOT A PS3 DISC (CD001 present) via FTP 127.0.0.1:2121"));
        assert(probe_ns::report_text(rep).find("FTP do GoldHEN") != std::string::npos);
    }
    {
        // Stage 3 keeps the time limit of stage 2: a server that never answers ends in a time-out of stage 3
        probe_ns::ops os;
        os.list_names = [](const std::string&, std::vector<std::string>&) { return 0; };
        os.stat_path = [](const std::string&) { probe_ns::node_info i; i.error = ENOENT; return i; };
        os.write_marker = [](const std::string&) { return 0; };
        unsigned removes = 0;
        os.remove_marker = [&removes](const std::string&) { ++removes; return 0; };
        auto gate = std::make_shared<std::atomic<bool>>(false);
        os.ftp_read = [gate]
        {
            while (!gate->load()) std::this_thread::sleep_for(1ms);
            return ds::header_read{};
        };
        log_sink logs;
        auto probe = probe_ns::probe::start(os, logs.fn(), {1000ms, 1000ms, 150ms});
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        std::optional<probe_ns::report> rep;
        while (!(rep = probe->poll()))
        {
            assert(std::chrono::steady_clock::now() < deadline);
            std::this_thread::sleep_for(2ms);
        }
        assert(rep->kind == probe_ns::outcome::timeout && rep->stage == 3);
        assert(logs.has("disc probe: TIMEOUT in stage 3 after 1 s"));
        gate->store(true);
        const std::weak_ptr<probe_ns::probe> weak = probe;
        probe.reset();
        while (!weak.expired())
        {
            assert(std::chrono::steady_clock::now() < deadline);
            std::this_thread::sleep_for(2ms);
        }
        assert(logs.has("disc probe: stage 3 returned after"));
        assert(removes == 0); // the marker stays after a time-out
    }
    // Every outcome has a line for the XMB
    for (const auto kind : {probe_ns::outcome::ftp_no_server, probe_ns::outcome::ftp_refused})
    {
        probe_ns::report r;
        r.kind = kind;
        assert(!probe_ns::report_text(r).empty() && probe_ns::report_text(r).size() < 140);
    }
    assert(std::string(probe_ns::confirm_text()).size() < 130);
}

void test_stats_line()
{
    fake_server server;
    make_disc(server, 64ull << 20, "BLES-01876", "FIFA 14");
    log_sink logs;
    ds::reader_config cfg = small_config();
    cfg.stats_interval_ms = 1;
    ds::reader r(server.ops(), loopback(), cfg, logs.fn());
    r.set_size(server.size);
    assert(read_matches(r, server, 0, 64 * 1024));
    std::this_thread::sleep_for(5ms);
    assert(read_matches(r, server, 30ull << 20, 64 * 1024));
    assert(logs.has("disc stream: last 0 s:") && logs.has("MiB/s") && logs.has("transfers (0 below a 2 GiB boundary)"));
}

// ---- the install ----

// An in-memory file system. A page that holds what 'oracle' says the image holds is kept as a mark, so an image of
// several GiB costs no memory; any other page (text files, wrong bytes) keeps its bytes.
class mem_fs
{
public:
    static constexpr std::uint64_t page = 64 * 1024;
    std::function<void(std::uint64_t offset, unsigned char* out, std::size_t n)> oracle;
    std::int64_t free = std::int64_t{1} << 40; // free_bytes(); -1: unknown
    std::uint64_t fail_write_at = ~0ull;       // a write to a .part covering this offset fails with fail_error (once)
    int fail_error = ENOSPC;
    std::uint64_t crash_at = ~0ull;            // a write to a .part covering this offset: power loss (see crash())
    std::string fail_open_prefix;              // opening a path that starts with it fails (EACCES)
    std::string fail_rename_suffix;            // renaming a path that ends with it fails once (EIO)
    std::string refuse_in_name;                // opening a path that contains it fails (EINVAL: a name the volume refuses)
    unsigned syncs = 0;

    di::file_ops ops()
    {
        di::file_ops f;
        f.open = [this](const std::string& path, bool create, int& error) -> int
        {
            if (m_dead) return error = EIO, -1;
            if (!fail_open_prefix.empty() && path.starts_with(fail_open_prefix)) return error = EACCES, -1;
            if (!refuse_in_name.empty() && path.find(refuse_in_name) != std::string::npos) return error = EINVAL, -1;
            if (!m_files.count(path) && !create) return error = ENOENT, -1;
            m_files[path];
            m_fds[m_next] = path;
            return m_next++;
        };
        f.write_at = [this](int fd, const void* data, std::size_t n, std::uint64_t offset, int& error) { return write(fd, data, n, offset, error); };
        f.read_at = [this](int fd, void* out, std::size_t n, std::uint64_t offset, int& error) { return read(fd, out, n, offset, error); };
        f.truncate = [this](int fd, std::uint64_t size, int& error)
        {
            if (m_dead) return error = EIO, false;
            truncate(m_files.at(m_fds.at(fd)), size);
            return true;
        };
        f.sync = [this](int fd, int& error)
        {
            if (m_dead) return error = EIO, false;
            file& target = m_files.at(m_fds.at(fd));
            target.synced = target.size;
            ++syncs;
            return true;
        };
        f.close = [this](int fd) { m_fds.erase(fd); };
        f.size = [this](const std::string& path) -> std::int64_t
        {
            const auto found = m_files.find(path);
            return found == m_files.end() ? -1 : static_cast<std::int64_t>(found->second.size);
        };
        f.rename = [this](const std::string& from, const std::string& to, int& error)
        {
            if (m_dead || !m_files.count(from)) return error = m_dead ? EIO : ENOENT, false;
            if (!fail_rename_suffix.empty() && from.ends_with(fail_rename_suffix))
            {
                fail_rename_suffix.clear();
                return error = EIO, false;
            }
            file moved = std::move(m_files.at(from));
            m_files.erase(from);
            m_files[to] = std::move(moved);
            return true;
        };
        f.remove = [this](const std::string& path) { return !m_dead && m_files.erase(path) > 0; };
        f.free_bytes = [this](const std::string&) { return free; };
        f.list = [this](const std::string& dir)
        {
            std::vector<std::string> names;
            for (const auto& [path, data] : m_files)
                if (path.starts_with(dir) && path.find('/', dir.size()) == std::string::npos) names.push_back(path.substr(dir.size()));
            return names;
        };
        return f;
    }

    // Power loss: every file keeps its size but the bytes past its last sync read as zeros, and every call fails (EIO)
    // until revive()
    void crash()
    {
        for (auto& [path, f] : m_files)
        {
            std::vector<unsigned char> buf(page);
            for (auto it = f.pages.begin(); it != f.pages.end();)
            {
                const std::uint64_t start = it->first * page;
                if (start >= f.synced)
                {
                    it = f.pages.erase(it);
                    continue;
                }
                if (start + page > f.synced)
                {
                    load(f, it->first, buf.data());
                    std::memset(buf.data() + (f.synced - start), 0, static_cast<std::size_t>(start + page - f.synced));
                    store(f, it->first, buf.data());
                }
                ++it;
            }
        }
        m_dead = true;
    }

    void revive() { m_dead = false; }

    bool exists(const std::string& path) const { return m_files.count(path) > 0; }

    std::uint64_t size_of(const std::string& path) const { return exists(path) ? m_files.at(path).size : 0; }

    // Pages of 'path' over [0, size) that do not hold what the oracle says
    std::uint64_t mismatches(const std::string& path, std::uint64_t size) const
    {
        if (!exists(path)) return ~0ull;
        const file& f = m_files.at(path);
        std::uint64_t bad = 0;
        for (std::uint64_t index = 0; index * page < size; ++index)
        {
            const auto found = f.pages.find(index);
            if (found == f.pages.end() || !found->second.marked) ++bad;
        }
        return bad;
    }

    std::string text(const std::string& path) const
    {
        if (!exists(path)) return {};
        const file& f = m_files.at(path);
        std::string out(static_cast<std::size_t>(f.size), '\0');
        std::vector<unsigned char> buf(page);
        for (std::uint64_t at = 0; at < f.size; at += page)
        {
            load(f, at / page, buf.data());
            std::memcpy(out.data() + at, buf.data(), static_cast<std::size_t>(std::min(page, f.size - at)));
        }
        return out;
    }

    // One byte of 'path' changed
    void corrupt(const std::string& path, std::uint64_t offset)
    {
        file& f = m_files.at(path);
        std::vector<unsigned char> buf(page);
        load(f, offset / page, buf.data());
        buf[offset % page] ^= 0x5a;
        store(f, offset / page, buf.data());
    }

private:
    struct page_data
    {
        bool marked = false;
        std::uint32_t length = 0;
        std::vector<unsigned char> bytes;
    };

    struct file
    {
        std::uint64_t size = 0, synced = 0;
        std::map<std::uint64_t, page_data> pages;
    };

    std::map<std::string, file> m_files;
    std::map<int, std::string> m_fds;
    int m_next = 100;
    bool m_dead = false;

    void load(const file& f, std::uint64_t index, unsigned char* out) const
    {
        std::memset(out, 0, page);
        const auto found = f.pages.find(index);
        if (found == f.pages.end()) return;
        if (found->second.marked) oracle(index * page, out, found->second.length);
        else std::memcpy(out, found->second.bytes.data(), found->second.bytes.size());
    }

    void store(file& f, std::uint64_t index, const unsigned char* data)
    {
        const std::uint64_t start = index * page;
        if (start >= f.size)
        {
            f.pages.erase(index);
            return;
        }
        page_data p;
        p.length = static_cast<std::uint32_t>(std::min(page, f.size - start));
        if (oracle)
        {
            std::vector<unsigned char> want(p.length);
            oracle(start, want.data(), p.length);
            p.marked = std::memcmp(want.data(), data, p.length) == 0;
        }
        if (!p.marked) p.bytes.assign(data, data + p.length);
        f.pages[index] = std::move(p);
    }

    void truncate(file& f, std::uint64_t size)
    {
        std::vector<unsigned char> buf(page);
        const bool partial = size % page && f.pages.count(size / page);
        if (partial) load(f, size / page, buf.data());
        f.pages.erase(f.pages.lower_bound((size + page - 1) / page), f.pages.end());
        f.size = size;
        f.synced = std::min(f.synced, size);
        if (partial)
        {
            std::memset(buf.data() + size % page, 0, static_cast<std::size_t>(page - size % page));
            store(f, size / page, buf.data());
        }
    }

    std::int64_t write(int fd, const void* data, std::size_t n, std::uint64_t offset, int& error)
    {
        if (m_dead) return error = EIO, -1;
        const std::string& path = m_fds.at(fd);
        file& f = m_files.at(path);
        if (path.ends_with(".part") && crash_at >= offset && crash_at < offset + n)
        {
            crash_at = ~0ull;
            crash();
            return error = EIO, -1;
        }
        if (path.ends_with(".part") && fail_write_at >= offset && fail_write_at < offset + n)
        {
            fail_write_at = ~0ull;
            return error = fail_error, -1;
        }
        const std::uint64_t end = offset + n;
        std::vector<unsigned char> buf(page);
        for (std::uint64_t index = offset / page; index * page < end; ++index)
        {
            const std::uint64_t start = index * page;
            const std::uint64_t from = std::max(offset, start), to = std::min(end, start + page);
            if (from > start || to < std::min(start + page, std::max(f.size, end))) load(f, index, buf.data());
            std::memcpy(buf.data() + (from - start), static_cast<const unsigned char*>(data) + (from - offset), static_cast<std::size_t>(to - from));
            f.size = std::max(f.size, to);
            store(f, index, buf.data());
        }
        return static_cast<std::int64_t>(n);
    }

    std::int64_t read(int fd, void* out, std::size_t n, std::uint64_t offset, int& error) const
    {
        if (m_dead) return error = EIO, -1;
        const file& f = m_files.at(m_fds.at(fd));
        if (offset >= f.size) return 0;
        n = static_cast<std::size_t>(std::min<std::uint64_t>(n, f.size - offset));
        std::vector<unsigned char> buf(page);
        for (std::uint64_t at = offset; at < offset + n;)
        {
            const std::uint64_t index = at / page, within = at % page;
            const std::size_t take = static_cast<std::size_t>(std::min<std::uint64_t>(page - within, offset + n - at));
            load(f, index, buf.data());
            std::memcpy(static_cast<unsigned char*>(out) + (at - offset), buf.data() + within, take);
            at += take;
        }
        return static_cast<std::int64_t>(n);
    }
};

// A PS3 game disc: region table from 'plain' (the gaps are encrypted), header, PVD -> root (20) -> PS3_GAME (21) ->
// PARAM.SFO (22), LICDIR (23) -> LIC.DAT, USRDIR (24) -> EBOOT.BIN. Served encrypted with 'key' when set.
struct game_disc
{
    std::uint64_t bytes = 40ull << 20;
    std::string id = "BLES-01876";
    std::string title = "FIFA 14";
    std::vector<std::pair<std::uint32_t, std::uint32_t>> plain;
    std::uint32_t lic_lba = 0x800;
    std::uint32_t eboot_lba = 0x3000;
    bool eboot_ok = true; // EBOOT.BIN starts with "SCE\0" once decrypted
    std::optional<di::key_bytes> key;
};

constexpr di::key_bytes test_key{0x38, 0x0b, 0xcf, 0x0b, 0x53, 0x45, 0x5b, 0x3c, 0x78, 0x17, 0xab, 0x4f, 0xa3, 0xba, 0x90, 0xed};
constexpr di::key_bytes wrong_key{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};

game_disc small_game(bool keyed = true)
{
    game_disc g;
    const std::uint32_t last = static_cast<std::uint32_t>(g.bytes / sector - 1);
    g.plain = {{0, 0x7ff}, {0x1000, 0x2fff}, {0x3800, last}};
    if (keyed) g.key = test_key;
    return g;
}

ds::layout make_game_disc(fake_server& server, const game_disc& g)
{
    server.size = g.bytes;
    server.overlay.clear();
    server.clear_key();
    std::vector<unsigned char> s0(sector, 0);
    be32_at(s0, 0, static_cast<std::uint32_t>(g.plain.size()));
    for (std::size_t i = 0; i < g.plain.size(); ++i)
    {
        be32_at(s0, 8 + i * 8, g.plain[i].first);
        be32_at(s0, 12 + i * 8, g.plain[i].second);
    }
    server.put(0, s0);
    std::vector<unsigned char> s1(sector, 0);
    std::memcpy(s1.data(), "PlayStation3", 12);
    std::memset(s1.data() + 0x10, ' ', 0x10);
    std::memcpy(s1.data() + 0x10, g.id.data(), g.id.size());
    server.put(sector, s1);
    std::vector<unsigned char> pvd(sector, 0);
    pvd[0] = 1;
    std::memcpy(pvd.data() + 1, "CD001", 5);
    const auto root_self = dir_record(std::string_view("\0", 1), 20, sector, true);
    std::memcpy(pvd.data() + 156, root_self.data(), 34);
    server.put(16 * sector, pvd);
    const auto directory = [&](std::uint32_t lba, std::uint32_t parent, std::vector<std::vector<unsigned char>> records)
    {
        std::vector<unsigned char> d;
        records.insert(records.begin(), {dir_record(std::string_view("\0", 1), lba, sector, true), dir_record(std::string_view("\1", 1), parent, sector, true)});
        for (const auto& r : records) d.insert(d.end(), r.begin(), r.end());
        d.resize(sector, 0);
        server.put(lba * sector, d);
    };
    const std::string id9 = g.id.substr(0, 4) + g.id.substr(5);
    const auto param = sfo({{"CATEGORY", "DG"}, {"TITLE", g.title}, {"TITLE_ID", id9}});
    directory(20, 20, {dir_record("PS3_GAME", 21, sector, true), dir_record("PS3_DISC.SFB;1", 30, 0x800, false)});
    directory(21, 20, {dir_record("LICDIR", 23, sector, true), dir_record("PARAM.SFO;1", 22, static_cast<std::uint32_t>(param.size()), false),
        dir_record("USRDIR", 24, sector, true)});
    directory(23, 21, {dir_record("LIC.DAT;1", g.lic_lba, 0x10000, false)});
    directory(24, 21, {dir_record("EBOOT.BIN;1", g.eboot_lba, 0x100000, false)});
    server.put(22 * sector, param);
    std::vector<unsigned char> lic(16, 0);
    std::memcpy(lic.data(), "PS3LICDA", 8);
    server.put(std::uint64_t{g.lic_lba} * sector, lic);
    std::vector<unsigned char> eboot(16, 0);
    std::memcpy(eboot.data(), g.eboot_ok ? "SCE\0\0\0\0\2" : "BAD!\0\0\0\2", 8);
    server.put(std::uint64_t{g.eboot_lba} * sector, eboot);
    const ds::layout table = ds::parse_layout(s0.data(), s0.size());
    assert(table.valid && table.size_bytes() == g.bytes);
    if (g.key) server.set_key(*g.key, table);
    return table;
}

di::key_source fixed_key(const di::key_bytes& key, std::string name)
{
    return [key, name](const di::key_request& request, std::string& source) -> std::optional<di::key_bytes>
    {
        assert(request.disc.state == ds::disc_state::ps3_disc && !request.files.param.empty() && request.head && !request.cancel.load());
        // A source may read the disc itself: sector 16 through the install's reader
        unsigned char pvd[16];
        assert(request.read(16 * sector, pvd, sizeof(pvd)) && std::memcmp(pvd + 1, "CD001", 5) == 0);
        source = name;
        return key;
    };
}

struct install_rig
{
    fake_server server;
    mem_fs fs;
    log_sink logs;
    std::vector<di::key_source> keys;
    di::install_config cfg;
    game_disc disc;

    install_rig()
    {
        cfg.net = fast();
        cfg.retry_pause_ms = 5;
        cfg.attempts = 3;
        cfg.sync_bytes = 4ull << 20;
        cfg.verify_overlap = 2ull << 20;
        cfg.log_interval_ms = 1'000'000;
    }

    // The disc in the drive; the image is expected decrypted (raw: as the drive returns it)
    void build(const game_disc& g, bool raw = false)
    {
        disc = g;
        make_game_disc(server, g);
        if (raw) fs.oracle = [this](std::uint64_t offset, unsigned char* out, std::size_t n) { server.fill(offset, out, n); };
        else fs.oracle = [this](std::uint64_t offset, unsigned char* out, std::size_t n) { server.plain(offset, out, n); };
    }

    // What the drive reports for the disc in it
    ds::disc_status status() const
    {
        ds::disc_status s;
        const auto head = server.expected(0, 2 * sector);
        s.state = ds::disc_state::ps3_disc;
        s.title_id = ds::title_id_from_header(head.data() + sector, sector);
        s.title = disc.title;
        s.size = ds::parse_layout(head.data(), sector).size_bytes();
        s.plain_regions = static_cast<std::uint32_t>(disc.plain.size());
        s.identity = ds::detail::fnv1a64(head.data(), head.size());
        return s;
    }

    std::shared_ptr<di::job> run(const ds::disc_status& s, std::uint64_t cancel_at = ~0ull)
    {
        auto j = std::make_shared<di::job>(s);
        {
            std::lock_guard lock(server.mutex);
            server.cancel_flag = &j->cancel;
            server.cancel_at = cancel_at;
        }
        di::environment env;
        env.net = server.ops();
        env.servers = loopback();
        env.files = fs.ops();
        env.keys = keys;
        env.cfg = cfg;
        env.log = logs.fn();
        di::run(env, *j);
        {
            std::lock_guard lock(server.mutex);
            server.cancel_flag = nullptr;
            assert(server.violations == 0 && server.streams == 0);
            for (const auto& c : server.commands) assert(c.rfind("ABOR", 0) != 0 && c != "REST 0");
        }
        assert(j->finished && j->current() == di::phase::finished);
        return j;
    }

    std::string image() const { return "/data/rpcs3/games/" + di::image_name("BLES01876", disc.title); }
    std::string part() const { return image() + ".part"; }

    std::optional<di::part_info> info() const
    {
        const std::string text = fs.text(part() + ".info");
        return di::parse_info(text);
    }

    std::uint64_t sent()
    {
        std::lock_guard lock(server.mutex);
        return server.bytes_sent;
    }

    unsigned refusals()
    {
        std::lock_guard lock(server.mutex);
        return server.retr_550;
    }
};

void test_install_pure()
{
    // File names
    assert(di::image_name("BLES01876", "FIFA 14") == "BLES01876 - FIFA 14.iso");
    assert(di::image_name("BLUS30443", "Demon's  Souls: A/B?\n") == "BLUS30443 - Demon's Souls AB.iso");
    assert(di::image_name("BLUS30443", "") == "BLUS30443.iso" && di::image_name("BLUS30443", " .. ") == "BLUS30443.iso");
    std::string accents;
    for (int i = 0; i < 40; ++i) accents += "\xc3\xa9";
    assert(di::image_name("BLES01876", accents) == "BLES01876 - " + accents.substr(0, 64) + ".iso");
    assert(di::image_name("BLES01876", "a" + accents) == "BLES01876 - a" + accents.substr(0, 62) + ".iso"); // not inside a character
    assert(di::names_title("BLES01876 - FIFA 14.iso", "BLES01876", ".iso") && di::names_title("bles01876.ISO", "BLES01876", ".iso"));
    assert(!di::names_title("BLES018760.iso", "BLES01876", ".iso") && !di::names_title("BLES01876 - FIFA 14.iso.part", "BLES01876", ".iso"));
    assert(di::names_title("BLES01876 - FIFA 14.iso.part", "BLES01876", ".iso.part") && !di::names_title("BLES01876.iso", "", ".iso"));

    // Key files as the ISO loader reads them
    const std::string hex = "380bcf0b53455b3c7817ab4fa3ba90ed\r\n";
    auto key = di::parse_key_file(reinterpret_cast<const unsigned char*>(hex.data()), hex.size());
    assert(key && *key == test_key);
    key = di::parse_key_file(test_key.data(), 16);
    assert(key && *key == test_key);
    const std::string bad = "380bcf0b53455b3c7817ab4fa3ba90eZ";
    assert(!di::parse_key_file(reinterpret_cast<const unsigned char*>(bad.data()), bad.size()));
    assert(!di::parse_key_file(test_key.data(), 15) && di::key_id(test_key) != 0 && di::key_id(test_key) != di::key_id(wrong_key));

    // .part.info
    const di::part_info info{"BLES01876", 10'619'912'192ull, 0x0123456789abcdefull, di::key_id(test_key), 2'415'919'104ull};
    const auto back = di::parse_info(di::to_text(info));
    assert(back && back->title_id == info.title_id && back->size == info.size && back->identity == info.identity && back->key == info.key && back->durable == info.durable);
    std::string broken = di::to_text(info);
    broken.erase(broken.find("durable"));
    assert(!di::parse_info(broken) && !di::parse_info("title_id BLES01876\n") && !di::parse_info(""));

    // Regions of FIFA 14 (plain 0-0x89f, 0x4cfa60-0x4cfa7f, 0x4cfca0-0x4cff3f, 0x4d1fc0-0x4f1fdf)
    ds::layout fifa;
    fifa.valid = true;
    fifa.plain = {{0, 0x89f}, {0x4cfa60, 0x4cfa7f}, {0x4cfca0, 0x4cff3f}, {0x4d1fc0, 0x4f1fdf}};
    fifa.plain_regions = 4;
    fifa.end_lba = 0x4f1fe0;
    assert(di::region_run(fifa, 0, ~0ull) == std::make_pair(false, std::uint64_t{0x8a0}));
    assert(di::region_run(fifa, 0x8a0, ~0ull) == std::make_pair(true, std::uint64_t{0x4cfa60 - 0x8a0}));
    assert(di::region_run(fifa, 0x4cff40, 10) == std::make_pair(true, std::uint64_t{10}));
    assert(di::region_run(fifa, 0x4f1fdf, ~0ull) == std::make_pair(false, std::uint64_t{1}));
    assert(di::has_encrypted_regions(fifa));

    // Sector decryption: AES-128-CBC per sector, IV = LBA
    std::vector<unsigned char> plain(2 * sector), data(2 * sector);
    pattern_fill(0x8a0 * sector, plain.data(), plain.size());
    std::memcpy(plain.data(), "PS3LICDA", 8);
    aes_context enc{};
    aes_setkey_enc(&enc, test_key.data(), 128);
    for (std::uint64_t i = 0; i < 2; ++i)
    {
        unsigned char iv[16]{};
        iv[14] = static_cast<unsigned char>((0x8a0 + i) >> 8);
        iv[15] = static_cast<unsigned char>(0x8a0 + i);
        aes_crypt_cbc(&enc, AES_ENCRYPT, sector, iv, plain.data() + i * sector, data.data() + i * sector);
    }
    assert(di::key_opens(test_key, 0x8a0, data.data(), di::lic_magic) && !di::key_opens(wrong_key, 0x8a0, data.data(), di::lic_magic));
    const auto cipher = std::make_unique<di::sector_cipher>(test_key);
    assert(cipher->ok() && cipher->decrypt(0x8a0, data.data(), 2) && data == plain);

    // What the games folder holds of a title
    mem_fs fs;
    di::file_ops ops = fs.ops();
    int error = 0;
    for (const std::string name : {"BLES01876 - FIFA 14.iso", "BLUS30443 - Demon's Souls.iso.part", "BLUS30443 - Demon's Souls.iso.part.info", "other.txt"})
    {
        const int fd = ops.open("/data/rpcs3/games/" + name, true, error);
        const std::string text = name.ends_with(".info") ? di::to_text({"BLUS30443", 8ull << 30, 1, 0, 3ull << 20}) : std::string(5ull << 20, 'x');
        assert(ops.write_at(fd, text.data(), text.size(), 0, error) == static_cast<std::int64_t>(text.size()));
        ops.close(fd);
    }
    // Only the image the lock record names is installed: a file named after the title without a record is an ISO the
    // player copied
    di::presence p = di::find_presence(ops, "/data/rpcs3/games/", "BLES01876");
    assert(p.installed.empty() && p.partial.empty());
    p = di::find_presence(ops, "/data/rpcs3/games/", "BLES01876", "/data/rpcs3/games/BLES01876 - FIFA 14.iso");
    assert(p.installed == "/data/rpcs3/games/BLES01876 - FIFA 14.iso" && p.partial.empty());
    assert(di::find_presence(ops, "/data/rpcs3/games/", "BLES01876", "/data/rpcs3/games/BLES01876 - renamed.iso").installed.empty());
    p = di::find_presence(ops, "/data/rpcs3/games/", "BLUS30443");
    assert(p.installed.empty() && p.partial == "/data/rpcs3/games/BLUS30443 - Demon's Souls.iso.part" && p.partial_bytes == (3ull << 20));
    assert(di::find_presence(ops, "/data/rpcs3/games/", "BCUS98232").installed.empty());
    // The record's image path (a record of 0.38 names only the file: /data/rpcs3/games/<image>)
    {
        const std::string record = "rpcs3 disc lock 1\ntitle_id BLES01876\nsize 123\nidentity 1\nimage BLES01876 - FIFA 14.iso\n";
        const int fd = ops.open("/locks/BLES01876.txt", true, error);
        assert(ops.write_at(fd, record.data(), record.size(), 0, error) == static_cast<std::int64_t>(record.size()));
        ops.close(fd);
        assert(di::recorded_image(ops, "/locks/", "BLES01876") == "/data/rpcs3/games/BLES01876 - FIFA 14.iso");
        assert(di::recorded_image(ops, "/locks/", "BLUS30443").empty() && di::recorded_image(ops, "", "BLES01876").empty());
    }

    // XMB texts
    assert(di::size_text(9'900'000'000ull) == "9,9 GB" && di::size_text(640'000'000ull) == "640 MB" && di::size_text(10'619'912'192ull) == "10,6 GB");
    ds::disc_status s;
    for (const auto state : {ds::disc_state::unknown, ds::disc_state::no_server, ds::disc_state::no_disc, ds::disc_state::not_ps3})
    {
        s.state = state;
        const di::row_text row = di::drive_row(s, nullptr);
        assert(!row.label.empty() && row.help.size() < 130);
    }
    s.state = ds::disc_state::ps3_disc;
    s.title_id = "BLES01876";
    s.title = "FIFA 14";
    s.size = 10'619'912'192ull;
    assert(di::drive_row(s, nullptr).label == "Instalar disco de PS3: FIFA 14" && di::drive_row(s, nullptr).help.find("10,6 GB") != std::string::npos);
    s.installed = "/data/rpcs3/games/BLES01876 - FIFA 14.iso";
    assert(di::drive_row(s, nullptr).label == "FIFA 14 (instalado)");
    s.partial = "/data/rpcs3/games/BLES01876 - FIFA 14.iso.part";
    s.partial_bytes = 2'500'000'000ull;
    assert(di::drive_row(s, nullptr).label == "Continuar instalação: FIFA 14" && di::drive_row(s, nullptr).help.find("2,5 GB de 10,6 GB") != std::string::npos);
    di::job j(s);
    j.total = s.size;
    for (const auto stage : {di::phase::starting, di::phase::checking, di::phase::resuming, di::phase::copying, di::phase::verifying})
    {
        j.stage = static_cast<int>(stage);
        j.done = 4'200'000'000ull;
        j.rate = 18'500'000;
        const std::string line = di::progress_text(j);
        assert(line.find("FIFA 14") != std::string::npos && line.size() < 130);
        assert(di::drive_row(s, &j).label == "Instalando FIFA 14" && di::drive_row(s, &j).help == line);
    }
    assert(di::progress_text(j).find("conferindo") != std::string::npos);
    j.stage = static_cast<int>(di::phase::checking);
    j.key_search = true;
    assert(di::progress_text(j) == "Instalando FIFA 14: buscando a chave do disco... · Círculo cancela");
    j.stage = static_cast<int>(di::phase::copying);
    assert(di::progress_text(j) == "Instalando FIFA 14: 4,2 GB de 10,6 GB (39%) · 18,5 MB/s · faltam ~6 min · Círculo cancela");
    for (const auto kind : {di::outcome::done, di::outcome::cancelled, di::outcome::no_server, di::outcome::no_disc, di::outcome::disc_changed,
             di::outcome::no_space, di::outcome::write_failed, di::outcome::read_failed, di::outcome::verify_failed, di::outcome::busy})
    {
        di::job done(s);
        done.need = 3'000'000'000ull;
        done.finish(kind, "", "");
        assert(!di::result_text(done).empty() && di::result_text(done).size() < 130 && std::string(di::to_string(kind)).size() > 3);
        assert(di::drive_row(s, &done).label == "Continuar instalação: FIFA 14"); // a finished install no longer owns the row
    }
}

void test_install_decrypted()
{
    install_rig rig;
    rig.build(small_game());
    rig.keys = {fixed_key(wrong_key, "a wrong key"), fixed_key(test_key, "the test key")};
    const auto j = rig.run(rig.status());
    assert(j->result() == di::outcome::done && j->path() == rig.image() && j->encrypted && j->decrypting && j->done == rig.disc.bytes);
    assert(rig.fs.exists(rig.image()) && !rig.fs.exists(rig.part()) && !rig.fs.exists(rig.part() + ".info") && !rig.fs.exists(rig.part() + ".info.tmp"));
    // Every page is the disc decrypted, sector 0 (the region table the ISO loader reads) included
    assert(rig.fs.size_of(rig.image()) == rig.disc.bytes && rig.fs.mismatches(rig.image(), rig.disc.bytes) == 0);
    const std::string image = rig.fs.text(rig.image());
    assert(image.compare(0x800 * sector, 8, "PS3LICDA") == 0 && image.compare(0x3000 * sector, 4, std::string("SCE\0", 4)) == 0);
    std::vector<unsigned char> raw(sector);
    rig.server.fill(0, raw.data(), raw.size());
    assert(std::memcmp(image.data(), raw.data(), sector) == 0);
    assert(rig.logs.has("disc install: start: BLES01876 'FIFA 14' -> /data/rpcs3/games/BLES01876 - FIFA 14.iso (40.0 MiB)"));
    assert(rig.logs.has("disc install: the key from a wrong key does not decrypt this disc (LIC.DAT WRONG)"));
    assert(rig.logs.has("disc install: key from the test key (id ") && rig.logs.has("): LIC.DAT ok, EBOOT.BIN ok"));
    assert(rig.logs.has("disc install: free 1048576.0 MiB, decrypting with the key from the test key"));
    assert(rig.logs.has("disc install: verify: PARAM.SFO TITLE_ID BLES01876 ok, LIC.DAT magic ok, EBOOT.BIN magic ok"));
    assert(rig.logs.has("disc install: done: /data/rpcs3/games/BLES01876 - FIFA 14.iso (decrypted, plays without a key)"));
    assert(rig.fs.syncs >= 10);
    // One transfer from offset 0 copied the disc (the check before it reads sectors 0-1, the tree, LIC.DAT and EBOOT.BIN,
    // and sectors 0-1 again after the key sources)
    {
        std::lock_guard lock(rig.server.mutex);
        assert(std::count(rig.server.retr_offsets.begin(), rig.server.retr_offsets.end(), 0ull) == 3 && rig.server.retr_550 == 0);
    }
    assert(di::result_text(*j) == "Disco instalado: FIFA 14 · já está na lista de jogos");

    // The lock record (disc lock): written before the rename, with the disc's identity and fingerprint; the drive reads
    // the same fingerprint from the disc as the install read from the decrypted file
    namespace dl = utils::ps4::disc_lock;
    const std::string record_path = "/data/rpcs3/data/disc-lock/BLES01876.txt";
    assert(rig.fs.exists(record_path) && !rig.fs.exists(record_path + ".tmp"));
    const auto record = dl::parse(rig.fs.text(record_path));
    assert(record && record->title_id == "BLES01876" && record->title == "FIFA 14" && record->size == rig.disc.bytes && record->valid);
    assert(record->identity == rig.status().identity && record->fingerprint == expected_fingerprint(rig.server) && record->image == "BLES01876 - FIFA 14.iso");
    assert(record->path == rig.image());
    {
        std::lock_guard lock(rig.logs.mutex);
        const auto at = [&](std::string_view text)
        {
            for (std::size_t i = 0; i < rig.logs.lines.size(); ++i)
                if (rig.logs.lines[i].find(text) != std::string::npos) return i;
            return ~std::size_t{0};
        };
        assert(at("disc install: lock record /data/rpcs3/data/disc-lock/BLES01876.txt (identity ") < at("disc install: done: "));
        assert(at("disc install: verify: ") < at("disc install: lock record "));
    }
    {
        ds::drive d(std::make_unique<ds::reader>(rig.server.ops(), loopback(), small_config(), rig.logs.fn()), [](const std::string&) { return false; },
            "/data/rpcs3/data/redump/", ds::drive_config{}, rig.logs.fn());
        const ds::disc_status seen = d.check_now();
        assert(seen.state == ds::disc_state::ps3_disc && seen.fingerprint == record->fingerprint && seen.identity == record->identity);
        assert(dl::classify(*record, seen, ds::detail::now_ms()) == dl::match::exact);
        d.disc().close_all();
    }

    // Installed again: the image is replaced in place, no second file; the record is written again
    const auto again = rig.run(rig.status());
    assert(again->result() == di::outcome::done && rig.fs.mismatches(rig.image(), rig.disc.bytes) == 0);
    assert(rig.fs.ops().list("/data/rpcs3/games/") == std::vector<std::string>{"BLES01876 - FIFA 14.iso"});
    assert(rig.logs.count("disc install: lock record /data/rpcs3/data/disc-lock/BLES01876.txt") == 2 && dl::parse(rig.fs.text(record_path)));
    assert(rig.fs.ops().list("/data/rpcs3/data/disc-lock/") == std::vector<std::string>{"BLES01876.txt"});

    // An ISO of the player with the install's file name and no record: never replaced; the install takes another name
    // and its record names that file
    {
        install_rig own;
        own.build(small_game());
        own.keys = {fixed_key(test_key, "the test key")};
        int error = 0;
        const std::string mine(3ull << 20, 'm');
        const int fd = own.fs.ops().open(own.image(), true, error);
        assert(own.fs.ops().write_at(fd, mine.data(), mine.size(), 0, error) == static_cast<std::int64_t>(mine.size()));
        own.fs.ops().close(fd);
        const auto j = own.run(own.status());
        const std::string other = "/data/rpcs3/games/BLES01876 - FIFA 14 (disco).iso";
        assert(j->result() == di::outcome::done && j->path() == other && own.fs.mismatches(other, own.disc.bytes) == 0);
        assert(own.fs.text(own.image()) == mine && own.logs.has("is a file that was not installed from the disc: the install writes '" + other + "'"));
        const auto written = dl::parse(own.fs.text(record_path));
        assert(written && written->path == other && written->image == "BLES01876 - FIFA 14 (disco).iso");
        // Installed again: the recorded image is replaced, the player's ISO is still not touched
        assert(own.run(own.status())->path() == other && own.fs.text(own.image()) == mine);
    }
}

void test_install_lock_record()
{
    namespace dl = utils::ps4::disc_lock;
    const std::string record_path = "/data/rpcs3/data/disc-lock/BLES01876.txt";
    {
        // The lock folder refuses the record: nothing is renamed and the complete .part stays; the next install
        // compares its end, writes the record and renames it
        install_rig rig;
        rig.build(small_game());
        rig.keys = {fixed_key(test_key, "the test key")};
        rig.fs.fail_open_prefix = "/data/rpcs3/data/disc-lock/";
        auto j = rig.run(rig.status());
        assert(j->result() == di::outcome::write_failed && j->path() == rig.part());
        assert(!rig.fs.exists(rig.image()) && rig.fs.exists(rig.part()) && !rig.fs.exists(record_path));
        const auto info = rig.info();
        assert(info && info->durable == rig.disc.bytes);
        assert(rig.logs.has("disc install: failed: write failed: lock record /data/rpcs3/data/disc-lock/BLES01876.txt: errno 13 (EACCES)"));
        rig.fs.fail_open_prefix.clear();
        const std::uint64_t before = rig.sent();
        j = rig.run(rig.status());
        assert(j->result() == di::outcome::done && rig.fs.exists(rig.image()) && !rig.fs.exists(rig.part()) && rig.fs.exists(record_path));
        assert(rig.sent() - before <= rig.cfg.verify_overlap + (2u << 20));
        assert(rig.fs.mismatches(rig.image(), rig.disc.bytes) == 0 && dl::parse(rig.fs.text(record_path)));
    }
    {
        // Power lost between the record and the rename: a record without an image binds nothing, and the next
        // install finishes
        install_rig rig;
        rig.build(small_game());
        rig.keys = {fixed_key(test_key, "the test key")};
        rig.fs.fail_rename_suffix = ".iso.part";
        auto j = rig.run(rig.status());
        assert(j->result() == di::outcome::write_failed && rig.fs.exists(record_path) && !rig.fs.exists(rig.image()) && rig.fs.exists(rig.part()));
        j = rig.run(rig.status());
        assert(j->result() == di::outcome::done && rig.fs.exists(rig.image()) && rig.fs.exists(record_path));
        assert(rig.fs.mismatches(rig.image(), rig.disc.bytes) == 0);
    }
    {
        // No lock folder configured: no record
        install_rig rig;
        rig.build(small_game());
        rig.cfg.lock_dir.clear();
        rig.keys = {fixed_key(test_key, "the test key")};
        const auto j = rig.run(rig.status());
        assert(j->result() == di::outcome::done && !rig.fs.exists(record_path) && !rig.logs.has("lock record"));
    }
}

void test_install_without_key()
{
    {
        // No key: the image as the drive returns it, which the loader opens with <TITLE_ID>.dkey later
        install_rig rig;
        rig.build(small_game(), true);
        const auto j = rig.run(rig.status());
        assert(j->result() == di::outcome::done && j->encrypted && !j->decrypting);
        assert(rig.fs.mismatches(rig.image(), rig.disc.bytes) == 0);
        assert(rig.logs.has("writing the image as it is on the disc (no key)"));
        assert(rig.logs.has("verify: PARAM.SFO TITLE_ID BLES01876 ok, LIC.DAT encrypted, EBOOT.BIN encrypted"));
        assert(rig.logs.has("(encrypted: the loader needs BLES01876.dkey in the redump folder)"));
        // The key comes by itself later (the key search runs for an installed disc without its key): no .dkey to copy
        assert(di::result_text(*j) == "Disco instalado: FIFA 14 · falta a chave: com o disco no leitor e internet ela vem sozinha");
        assert(di::result_text(*j).find(".dkey") == std::string::npos);
    }
    {
        // A key that opens LIC.DAT but not EBOOT.BIN, which the check before the copy could not read: the copy starts
        // over with the image as it is on the disc when it reaches EBOOT.BIN
        install_rig rig;
        game_disc g = small_game();
        g.eboot_ok = false;
        rig.build(g, true);
        rig.server.refuse_rest = {std::uint64_t{g.eboot_lba} * sector};
        rig.keys = {fixed_key(test_key, "the test key")};
        const auto j = rig.run(rig.status());
        assert(j->result() == di::outcome::done && !j->decrypting);
        assert(rig.logs.has("): LIC.DAT ok") && !rig.logs.has("EBOOT.BIN ok"));
        assert(rig.logs.has("disc install: the key from the test key does not decrypt EBOOT.BIN (LBA 0x3000): starting over with the image as it is on the disc"));
        assert(rig.fs.mismatches(rig.image(), g.bytes) == 0);
    }
    {
        // A disc with no encrypted region needs no key
        install_rig rig;
        game_disc g = small_game(false);
        g.plain = {{0, static_cast<std::uint32_t>(g.bytes / sector - 1)}};
        rig.build(g);
        rig.cfg.log_interval_ms = 0; // a progress line per block
        const auto j = rig.run(rig.status());
        assert(j->result() == di::outcome::done && !j->encrypted && rig.logs.has("the disc has no encrypted region"));
        assert(rig.logs.has("disc install: 40.0 MiB of 40.0 MiB (100%), ") && rig.logs.has(" MiB/s"));
        assert(rig.logs.has("disc install: copied: 40.0 MiB written in "));
        assert(rig.fs.mismatches(rig.image(), g.bytes) == 0);
    }
}

void test_install_cancel_and_resume()
{
    install_rig rig;
    rig.build(small_game());
    rig.keys = {fixed_key(test_key, "the test key")};
    auto j = rig.run(rig.status(), 25ull << 20);
    assert(j->result() == di::outcome::cancelled && j->path() == rig.part());
    auto info = rig.info();
    assert(info && info->durable >= (24ull << 20) && info->durable <= (27ull << 20) && info->durable % sector == 0 && info->key == di::key_id(test_key));
    assert(rig.logs.has("disc install: cancelled at") && rig.logs.has("for the next install"));
    assert(di::result_text(*j).find("continua dali") != std::string::npos);

    // The drive row offers to go on
    ds::disc_status s = rig.status();
    const di::presence p = di::find_presence(rig.fs.ops(), "/data/rpcs3/games/", "BLES01876");
    s.partial = p.partial;
    s.partial_bytes = p.partial_bytes;
    assert(p.partial == rig.part() && p.partial_bytes == info->durable && di::drive_row(s, nullptr).label == "Continuar instalação: FIFA 14");

    // Resumed: the overlap is compared, the rest copied
    const std::uint64_t before = rig.sent();
    j = rig.run(s);
    assert(j->result() == di::outcome::done && rig.fs.mismatches(rig.image(), rig.disc.bytes) == 0);
    assert(rig.logs.has("disc install: resume: ") && rig.logs.has("disc install: resume: the file matches the disc up to LBA"));
    assert(rig.sent() - before <= rig.disc.bytes - info->durable + rig.cfg.verify_overlap + (2u << 20));

    // Cancelled again, then the file damaged inside the overlap: the copy starts over and still ends right
    install_rig again;
    again.build(small_game());
    again.keys = {fixed_key(test_key, "the test key")};
    j = again.run(again.status(), 20ull << 20);
    info = again.info();
    assert(j->result() == di::outcome::cancelled && info);
    again.fs.corrupt(again.part(), info->durable - (1u << 20));
    j = again.run(again.status());
    assert(j->result() == di::outcome::done && again.logs.has("disc install: resume: the file differs from the disc at LBA"));
    assert(again.fs.mismatches(again.image(), again.disc.bytes) == 0);

    // A .part decrypted with a key cannot go on without it: it starts over as the image on the disc
    install_rig keyless;
    keyless.build(small_game());
    keyless.keys = {fixed_key(test_key, "the test key")};
    j = keyless.run(keyless.status(), 10ull << 20);
    keyless.keys.clear();
    keyless.build(small_game(), true);
    j = keyless.run(keyless.status());
    assert(j->result() == di::outcome::done && keyless.logs.has("cannot go on (it was decrypted with a key not found now): starting over"));
    assert(keyless.fs.mismatches(keyless.image(), keyless.disc.bytes) == 0);
}

void test_install_power_loss()
{
    // The console loses power while the copy runs: what was not synced is lost, the .part.info names what was
    install_rig rig;
    rig.build(small_game());
    rig.keys = {fixed_key(test_key, "the test key")};
    rig.fs.crash_at = (30ull << 20) + 12345;
    auto j = rig.run(rig.status());
    assert(j->result() == di::outcome::write_failed);
    rig.fs.revive();
    const auto info = rig.info();
    assert(info && info->durable == (28ull << 20) && rig.fs.size_of(rig.part()) == (30ull << 20));
    j = rig.run(rig.status());
    assert(j->result() == di::outcome::done && rig.fs.mismatches(rig.image(), rig.disc.bytes) == 0);
    assert(rig.logs.has("disc install: resume: 28.0 MiB of 40.0 MiB already written; the copy restarts at LBA 0x3400"));
}

void test_install_faults()
{
    {
        // The data connection resets, then the drive fails a read once: reconnections, sectors 0-1 read again each time
        install_rig rig;
        rig.build(small_game());
        rig.keys = {fixed_key(test_key, "the test key")};
        rig.server.drop_data_at = (10ull << 20) + 777;
        rig.server.fail_read_at = (20ull << 20) + 4096;
        rig.server.fail_read_times = 1;
        const auto j = rig.run(rig.status());
        assert(j->result() == di::outcome::done && rig.fs.mismatches(rig.image(), rig.disc.bytes) == 0);
        assert(rig.logs.has("failed (connection closed by the server); again in 5 ms (attempt 1 of 3)"));
        assert(rig.logs.has("failed (transfer failed: '451 Requested action aborted. Local error in processing.'); again in 5 ms"));
        std::lock_guard lock(rig.server.mutex);
        assert(std::count(rig.server.retr_offsets.begin(), rig.server.retr_offsets.end(), 0ull) == 5);
        assert(std::count(rig.server.retr_offsets.begin(), rig.server.retr_offsets.end(), 10ull << 20) == 1);
        assert(std::count(rig.server.retr_offsets.begin(), rig.server.retr_offsets.end(), (20ull << 20) + 4096) == 1);
    }
    {
        // The disc leaves the drive: no_disc after the attempts, the .part kept; the disc back: the install goes on
        install_rig rig;
        rig.build(small_game());
        rig.keys = {fixed_key(test_key, "the test key")};
        rig.server.remove_disc_at = 15ull << 20;
        auto j = rig.run(rig.status());
        assert(j->result() == di::outcome::no_disc && j->path() == rig.part());
        assert(rig.logs.has("disc install: failed: no readable PS3 disc: sectors 0-1 again: refused: '550"));
        assert(di::result_text(*j).find("saiu do leitor") != std::string::npos);
        const auto info = rig.info();
        assert(info && info->durable >= (14ull << 20));
        rig.server.no_disc = false;
        j = rig.run(rig.status());
        assert(j->result() == di::outcome::done && rig.fs.mismatches(rig.image(), rig.disc.bytes) == 0);
    }
    {
        // Another disc with the same title id: refused for the status of the first one; for its own status the old
        // .part is dropped
        install_rig rig;
        rig.build(small_game());
        rig.keys = {fixed_key(test_key, "the test key")};
        const ds::disc_status first = rig.status();
        auto j = rig.run(first, 10ull << 20);
        assert(j->result() == di::outcome::cancelled);
        game_disc other = small_game();
        other.plain = {{0, 0x7ff}, {0x1100, static_cast<std::uint32_t>(other.bytes / sector - 1)}};
        rig.build(other);
        j = rig.run(first);
        assert(j->result() == di::outcome::disc_changed && di::result_text(*j).find("outro disco") != std::string::npos);
        j = rig.run(rig.status());
        assert(j->result() == di::outcome::done && rig.logs.has("cannot go on (it was made from another disc): starting over"));
        assert(rig.fs.mismatches(rig.image(), other.bytes) == 0);
    }
    {
        // The disc is swapped while a key source runs (the key search may wait minutes): the install stops before it
        // judges the .part against the other disc, which keeps the .part as it was; the right disc back, it resumes
        install_rig rig;
        rig.build(small_game());
        const di::key_source fixed = fixed_key(test_key, "the test key");
        rig.keys = {fixed};
        auto j = rig.run(rig.status(), 20ull << 20);
        const auto kept = rig.info();
        assert(j->result() == di::outcome::cancelled && kept && kept->durable >= (18ull << 20));
        game_disc other = small_game();
        other.plain = {{0, 0x7ff}, {0x1100, static_cast<std::uint32_t>(other.bytes / sector - 1)}};
        rig.keys = {[&rig, &other, fixed](const di::key_request& request, std::string& source)
        {
            {
                std::lock_guard lock(rig.server.mutex);
                make_game_disc(rig.server, other);
            }
            return fixed(request, source);
        }};
        j = rig.run(rig.status());
        const auto after = rig.info();
        assert(j->result() == di::outcome::disc_changed && rig.logs.has("disc install: failed: another disc in the drive: sectors 0-1 after the key sources: changed"));
        assert(after && after->durable == kept->durable && rig.fs.size_of(rig.part()) >= kept->durable);
        {
            std::lock_guard lock(rig.server.mutex);
            make_game_disc(rig.server, rig.disc);
        }
        rig.keys = {fixed};
        j = rig.run(rig.status());
        assert(j->result() == di::outcome::done && rig.logs.has("disc install: resume: the file matches the disc up to LBA"));
        assert(rig.fs.mismatches(rig.image(), rig.disc.bytes) == 0);
    }
    {
        // Free space: refused before anything is written; a full volume while writing keeps the .part
        install_rig rig;
        rig.build(small_game());
        rig.keys = {fixed_key(test_key, "the test key")};
        rig.fs.free = 20ll << 20;
        auto j = rig.run(rig.status());
        assert(j->result() == di::outcome::no_space && !rig.fs.exists(rig.part()) && j->need == rig.disc.bytes + (1ull << 30));
        assert(di::result_text(*j) == "Sem espaço em /data: são precisos ~1,1 GB livres · apague algo e instale de novo");
        rig.fs.free = -1;
        rig.fs.fail_write_at = 12ull << 20;
        j = rig.run(rig.status());
        assert(j->result() == di::outcome::no_space && rig.logs.has("free space of /data/rpcs3/games/ unknown: going on"));
        const auto info = rig.info();
        assert(info && info->durable == (12ull << 20));
        j = rig.run(rig.status());
        assert(j->result() == di::outcome::done && rig.fs.mismatches(rig.image(), rig.disc.bytes) == 0);
    }
    {
        // A .part that cannot go on (written without the key found now) is dropped by the install: its bytes count as
        // free space
        install_rig rig;
        rig.build(small_game(), true);
        auto j = rig.run(rig.status(), 30ull << 20);
        assert(j->result() == di::outcome::cancelled && rig.fs.size_of(rig.part()) >= (28ull << 20));
        rig.build(small_game());
        rig.keys = {fixed_key(test_key, "the test key")};
        rig.fs.free = (1ll << 30) + (20ll << 20);
        j = rig.run(rig.status());
        assert(j->result() == di::outcome::done && rig.logs.has("cannot go on (it holds the image as it is on the disc): starting over"));
        assert(rig.fs.mismatches(rig.image(), rig.disc.bytes) == 0);
    }
    {
        // No FTP server; and a selection that is not a PS3 disc
        install_rig rig;
        rig.build(small_game());
        rig.server.listening.clear();
        auto j = rig.run(rig.status());
        assert(j->result() == di::outcome::no_server && !rig.fs.exists(rig.part()) && di::result_text(*j).find("FTP") != std::string::npos);
        ds::disc_status none;
        none.state = ds::disc_state::no_disc;
        j = rig.run(none);
        assert(j->result() == di::outcome::no_disc);
    }
}

void test_install_across_2gib()
{
    // A disc past 2 GiB with an encrypted region across the boundary; cancelled 32 MiB past it. The server cannot
    // start a transfer there (550): the resume starts at the sector below the boundary and compares up to the
    // cancel point, then copies the rest.
    install_rig rig;
    rig.cfg.sync_bytes = 256ull << 20;
    rig.cfg.verify_overlap = 8ull << 20;
    game_disc g;
    g.bytes = (2ull << 30) + (64ull << 20);
    const std::uint32_t last = static_cast<std::uint32_t>(g.bytes / sector - 1);
    g.plain = {{0, 0x7ff}, {0x1000, 0xfff00 - 1}, {0x100100, last}};
    g.lic_lba = 0x800;
    g.eboot_lba = 0xfff00;
    g.key = test_key;
    rig.build(g);
    rig.keys = {fixed_key(test_key, "the test key")};
    auto j = rig.run(rig.status(), (2ull << 30) + (32ull << 20));
    assert(j->result() == di::outcome::cancelled && rig.refusals() == 0);
    const auto info = rig.info();
    assert(info && info->durable >= (2ull << 30) + (32ull << 20) && info->durable < (2ull << 30) + (34ull << 20));

    const std::uint64_t before = rig.sent();
    j = rig.run(rig.status());
    assert(j->result() == di::outcome::done && rig.refusals() == 1);
    assert(rig.logs.has("disc install: the server refuses to start a transfer at an offset with bit 31 set (550 Requested action not taken. File unavailable.)"));
    assert(rig.logs.has("lies past a 2 GiB boundary: the copy restarts at LBA 0xfffff and compares"));
    assert(rig.logs.has("disc install: resume: the file matches the disc up to LBA"));
    assert(rig.sent() - before <= g.bytes - ((2ull << 30) - sector) + (4u << 20));
    assert(rig.fs.size_of(rig.image()) == g.bytes && rig.fs.mismatches(rig.image(), g.bytes) == 0);
}

// A spot of the disc the drive cannot read (451 at the same offset every time): the install retries, then gives up and
// names the sector, keeping the .part. Past a 2 GiB boundary each retry first re-reads from the sector below the
// boundary: that re-read is no progress, so the retries still run out (they used to start over forever).
void test_install_unreadable_spot()
{
    {
        install_rig rig;
        rig.build(small_game());
        rig.keys = {fixed_key(test_key, "the test key")};
        const std::uint64_t bad = (20ull << 20) + 4096;
        rig.server.fail_read_at = bad;
        rig.server.fail_read_times = 1000;
        const auto j = rig.run(rig.status());
        assert(j->result() == di::outcome::read_failed && j->unreadable_lba.load() == static_cast<std::int64_t>(bad / sector));
        assert(rig.logs.has("disc install: the drive cannot read the disc at LBA 0x2802 (4 transfers failed there)"));
        assert(di::result_text(*j) == "O leitor não conseguiu ler um trecho do disco (setor 0x2802): limpe o disco e instale de novo (continua dali)");
        assert(di::result_text(*j).size() < 130);
        const auto info = rig.info();
        assert(info && info->durable == bad / sector * sector);
        std::lock_guard lock(rig.server.mutex);
        assert(rig.server.fail_read_times == 1000 - 4);
    }
    {
        // A read that fails at a different place each time is no bad spot: no sector named, the generic text
        di::job other(ds::disc_status{});
        other.finish(di::outcome::read_failed, "", "");
        assert(other.unreadable_lba.load() < 0 && di::result_text(other).starts_with("Falha ao ler o disco"));
    }
    {
        install_rig rig;
        rig.cfg.sync_bytes = 256ull << 20;
        game_disc g;
        g.bytes = (2ull << 30) + (64ull << 20);
        const std::uint32_t last = static_cast<std::uint32_t>(g.bytes / sector - 1);
        g.plain = {{0, 0x7ff}, {0x1000, 0xfff00 - 1}, {0x100100, last}};
        g.key = test_key;
        rig.build(g);
        rig.keys = {fixed_key(test_key, "the test key")};
        const std::uint64_t bad = (2ull << 30) + (16ull << 20);
        rig.server.fail_read_at = bad;
        rig.server.fail_read_times = 1000;
        const auto j = rig.run(rig.status());
        assert(j->result() == di::outcome::read_failed && j->unreadable_lba.load() == static_cast<std::int64_t>(bad / sector));
        assert(rig.logs.has("lies past a 2 GiB boundary: the copy restarts at LBA 0xfffff and compares"));
        std::lock_guard lock(rig.server.mutex);
        assert(rig.server.fail_read_times == 1000 - 4);
    }
}

// A title the volume does not take in a file name (e.g. "God of War: Ascension™" if UTF-8 were refused): the image is
// named by the title id alone instead of failing the install
void test_install_name_refused()
{
    install_rig rig;
    game_disc g = small_game();
    g.title = "God of War: Ascension\xe2\x84\xa2";
    rig.build(g);
    rig.keys = {fixed_key(test_key, "the test key")};
    rig.fs.refuse_in_name = "\xe2\x84\xa2";
    const auto j = rig.run(rig.status());
    const std::string bare = "/data/rpcs3/games/BLES01876.iso";
    assert(j->result() == di::outcome::done && j->path() == bare && rig.fs.exists(bare) && !rig.fs.exists(bare + ".part.info"));
    assert(rig.logs.has("errno 22 (EINVAL); using BLES01876.iso instead"));
    assert(rig.fs.mismatches(bare, g.bytes) == 0);
    assert(rig.fs.text("/data/rpcs3/data/disc-lock/BLES01876.txt").find("image BLES01876.iso\n") != std::string::npos);
    // The next install of the disc finds that image and replaces it in place
    const auto again = rig.run(rig.status());
    assert(again->result() == di::outcome::done && again->path() == bare);
}

void test_drive_annotate_and_hold()
{
    fake_server server;
    make_disc(server, 64ull << 20, "BLES-01876", "FIFA 14");
    log_sink logs;
    ds::drive_config cfg;
    cfg.check_ms_present = 100'000;
    cfg.worker_idle_ms = 5000;
    ds::drive d(std::make_unique<ds::reader>(server.ops(), loopback(), small_config(), logs.fn()), [](const std::string&) { return false; },
        "/data/rpcs3/data/redump/", cfg, logs.fn());
    d.set_annotator([](ds::disc_status& s) { s.installed = "/data/rpcs3/games/" + s.title_id + ".iso"; });
    ds::disc_status s = d.check_now();
    const auto head = server.expected(0, 2 * sector);
    assert(s.state == ds::disc_state::ps3_disc && s.installed == "/data/rpcs3/games/BLES01876.iso" && s.identity == ds::detail::fnv1a64(head.data(), head.size()));
    assert(logs.has("; installed: /data/rpcs3/games/BLES01876.iso"));
    {
        // The drive is left without a transfer after a check
        std::lock_guard lock(server.mutex);
        assert(server.streams == 0);
    }
    // NOOP on an idle control connection
    assert(d.disc().keepalive(0));
    {
        std::lock_guard lock(server.mutex);
        assert(std::count(server.commands.begin(), server.commands.end(), std::string("NOOP")) >= 1);
    }
    // Held: no check reads the drive
    d.hold();
    const unsigned before = server.retr_count();
    s = d.check_now();
    assert(s.state == ds::disc_state::ps3_disc && server.retr_count() == before);
    d.poll();
    d.request_check();
    std::this_thread::sleep_for(150ms);
    assert(server.retr_count() == before);
    // Released: the worker checks at once
    d.release();
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (server.retr_count() == before)
    {
        d.poll();
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(5ms);
    }

    // The FTP client refuses an unaligned offset itself and sends no REST for offset 0
    const std::atomic<bool> never{false};
    const ds::net_ops ops = server.ops();
    const ds::timeouts limits = fast();
    ds::ftp_connection c(ops, limits, never);
    assert(c.connect({ds::loopback_ipv4, ds::default_port}) && !c.open_stream(100) && c.connected());
    assert(c.last_failure() == ds::ftp_connection::failure::protocol && c.noop());
    std::lock_guard lock(server.mutex);
    for (const auto& command : server.commands) assert(command != "REST 0" && command != "REST 100");
}
}

// FEAT logged once, the last server (another FTP payload's port), one attempt for a 451 at LBA 0 (empty drive) and the
// slow cadence of an XMB left alone
void test_feat_last_server_and_empty_drive()
{
    // The lines of a multi-line reply, also when it arrives in pieces
    std::string buffer = "211-Features:\r\n REST STREAM\r\n";
    std::string lines;
    assert(!ds::take_reply(buffer, &lines));
    buffer += " SITE CHMOD \r\n SIZE\r\n211 End\r\n200 next\r\n";
    auto r = ds::take_reply(buffer, &lines);
    assert(r && r->code == 211 && lines == "REST STREAM; SITE CHMOD; SIZE" && buffer == "200 next\r\n");
    r = ds::take_reply(buffer, &lines);
    assert(r && r->code == 200 && lines.empty());

    const auto first_read = [](fake_server& server, const ds::server_list& list, log_sink& logs)
    {
        ds::reader_config cfg = small_config();
        cfg.log_features = true;
        ds::reader rd(server.ops(), list, cfg, logs.fn());
        const bool ok = read_matches(rd, server, 0, 4096) && read_matches(rd, server, 1 << 20, 4096);
        rd.close_all();
        return ok;
    };
    {
        fake_server server;
        make_disc(server, 8ull << 20, "BLES-01876", "FIFA 14");
        server.feat_reply = "211-Features:\r\n REST STREAM\r\n SITE CHMOD\r\n211 End\r\n";
        log_sink logs;
        assert(first_read(server, loopback(), logs));
        assert(logs.count("disc stream: FEAT of 127.0.0.1:2121 (GoldHEN FTP server v2.2): REST STREAM; SITE CHMOD") == 1 && logs.count("FEAT of") == 1);
    }
    {
        fake_server server; // 502: none listed
        make_disc(server, 8ull << 20, "BLES-01876", "FIFA 14");
        log_sink logs;
        assert(first_read(server, loopback(), logs) && logs.has("FEAT of 127.0.0.1:2121 (GoldHEN FTP server v2.2): none listed"));
    }
    {
        fake_server server; // no answer: the connection is made again and the reads go on
        make_disc(server, 8ull << 20, "BLES-01876", "FIFA 14");
        server.feat_silent = true;
        log_sink logs;
        assert(first_read(server, loopback(), logs) && logs.has("): no answer, connecting again"));
        std::lock_guard lock(server.mutex);
        assert(std::count(server.commands.begin(), server.commands.end(), "FEAT") == 1 && server.connects >= 2);
    }
    {
        // Only a server on 1337: reached as the last one; without it in the list nothing is reached
        fake_server server;
        make_disc(server, 8ull << 20, "BLES-01876", "FIFA 14");
        server.control_port = 1337;
        log_sink logs;
        ds::server_list list = loopback();
        list.last.push_back({ds::loopback_ipv4, 1337});
        assert(first_read(server, list, logs) && logs.has("connected to 127.0.0.1:1337"));
        ds::reader none(server.ops(), loopback(), small_config());
        std::vector<unsigned char> out(4096);
        ds::read_status st{};
        assert(none.read(0, out.data(), out.size(), &st) == 0 && st == ds::read_status::no_server);
        // The fallback first, then the last one
        std::vector<std::string> order;
        ds::server_list both = loopback();
        both.fallback = [&order] { order.push_back("fallback"); return std::optional<ds::endpoint>(ds::endpoint{lan_ip, ds::default_port}); };
        both.last.push_back({ds::loopback_ipv4, 1337});
        ds::header_read h = ds::read_header(server.ops(), both, fast(), 3000);
        assert(h.kind == ds::header_read::result::ok && h.server.port == 1337 && order.size() == 1 && h.tried.find("192.168.1.33:2121") != std::string::npos);
    }
    {
        // An empty drive: RETR at LBA 0 accepted, the transfer ends with 451 at once. One transfer per check
        fake_server server;
        make_disc(server, 8ull << 20, "BLES-01876", "FIFA 14");
        server.fail_read_at = 0;
        server.fail_read_times = 1000;
        ds::reader rd(server.ops(), loopback(), small_config());
        std::vector<unsigned char> head(2 * sector);
        int reply = 0;
        assert(rd.read_fresh(0, head.data(), head.size(), &reply) == ds::read_status::failed && reply == 451 && server.retr_count() == 1);
        // Elsewhere a 451 is still tried again (a scratched spot)
        server.fail_read_at = 1 << 20;
        server.fail_read_times = 1000;
        assert(rd.read_fresh(1 << 20, head.data(), head.size(), &reply) == ds::read_status::failed && reply == 451 && server.retr_count() == 1 + 3);
        rd.close_all();

        // The XMB left alone: one check, then none for check_ms_idle; active again: the searching cadence at once
        ds::drive_config cfg;
        cfg.check_ms_searching = 20;
        cfg.check_ms_idle = 100'000;
        cfg.worker_idle_ms = 5000;
        server.fail_read_at = 0;
        ds::drive d(std::make_unique<ds::reader>(server.ops(), loopback(), small_config()), [](const std::string&) { return false; }, "", cfg);
        d.set_idle(true);
        const unsigned before = server.retr_count();
        d.poll();
        for (int i = 0; i < 400 && d.last().checked_ms == 0; ++i) std::this_thread::sleep_for(5ms);
        assert(d.last().state == ds::disc_state::no_disc && d.last().reply == 451);
        std::this_thread::sleep_for(300ms);
        d.poll();
        assert(server.retr_count() == before + 1);
        d.set_idle(false);
        for (int i = 0; i < 400 && server.retr_count() < before + 3; ++i)
        {
            d.poll();
            std::this_thread::sleep_for(5ms);
        }
        assert(server.retr_count() >= before + 3);
        d.stop();
        d.wait_idle(5000);
    }
}

int main()
{
    test_pure();
    test_reader_basics();
    test_reader_size_unknown_and_partial();
    test_boundaries();
    test_faults();
    test_threads();
    test_drive();
    test_drive_request_during_check();
    test_read_header_and_probe();
    test_stats_line();
    test_drive_annotate_and_hold();
    test_install_pure();
    test_install_decrypted();
    test_install_without_key();
    test_install_cancel_and_resume();
    test_install_power_loss();
    test_install_lock_record();
    test_install_faults();
    test_install_across_2gib();
    test_install_unreadable_spot();
    test_install_name_refused();
    test_feat_last_server_and_empty_drive();
    std::puts("disc_stream_test: OK");
    return 0;
}
