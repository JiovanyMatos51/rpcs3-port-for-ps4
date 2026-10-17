#pragma once

// Physical PS3 discs on the PS4. Everything here only reads.
//
// disc_probe::allowed_node(): the only device nodes the emulator may treat as an optical drive. The console's other
// /dev nodes (internal disk, flash, controllers) are character devices as well and are never opened as a disc.
//
// disc_probe::probe: an opt-in test of the Blu-ray drive, started from the XMB, that answers with data:
//   stage 0  does the app see a drive node? (names in /dev and /mnt; stat of /dev/cd0../dev/cd3 and /mnt/disc)
//   stage 1  may it open the node? (open O_RDONLY|O_NONBLOCK|O_CLOEXEC, fstat, lseek(SEEK_END), close)
//   stage 2  does the drive return the sectors of a PS3 disc? (two aligned reads: sectors 0-1 and sector 16)
//   stage 3  only when stage 0 finds no node: the same sectors through the console's GoldHEN FTP server
//            (127.0.0.1:2121 or /data/rpcs3/disc-ftp.txt; REST/RETR /dev/cd0, see ps4_disc_stream.hpp)
// No ioctl is ever made and no device is ever opened for writing. A device call can block inside the kernel, where a
// thread cannot be cancelled, so the stages run on a detached thread and the caller stops waiting after a fixed time
// (8 s for stages 0 and 1, 20 s for stages 2 and 3: a Blu-ray disc can take about 10 s to spin up). The marker
// /data/rpcs3/disc-probe.blocked is written before anything is looked at and removed only after a clean finish, so
// a probe that hung (or took the app down) is not run again until the user deletes it. Every result goes to
// startup.log through the caller's log function ("disc probe: ...").
//
// disc_keys: what the .dkey import accepts (16 raw bytes or 32 hex characters) and how it names the copy.

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
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
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "ps4_disc_stream.hpp"
#include "ps4_i18n.hpp"

namespace utils::ps4::disc_probe
{
inline constexpr std::array<std::string_view, 4> drive_nodes{"/dev/cd0", "/dev/cd1", "/dev/cd2", "/dev/cd3"};

// Exact names only: no prefix or pattern match, so no other device can ever pass as the drive.
inline bool allowed_node(std::string_view path) noexcept
{
    return std::find(drive_nodes.begin(), drive_nodes.end(), path) != drive_nodes.end();
}

inline constexpr std::size_t sector_size = 2048;
inline constexpr const char* blocked_marker = "/data/rpcs3/disc-probe.blocked";

namespace detail
{
using disc_stream::detail::be32;
using disc_stream::detail::hex;
using disc_stream::detail::printable;

inline std::string errno_text(int error)
{
    const char* name = nullptr;
    switch (error)
    {
    case EPERM: name = "EPERM"; break;
    case ENOENT: name = "ENOENT"; break;
    case EIO: name = "EIO"; break;
    case ENXIO: name = "ENXIO"; break;
    case EACCES: name = "EACCES"; break;
    case EBUSY: name = "EBUSY"; break;
    case ENODEV: name = "ENODEV"; break;
    case EINVAL: name = "EINVAL"; break;
    case EROFS: name = "EROFS"; break;
    case EAGAIN: name = "EAGAIN"; break;
    case ETIMEDOUT: name = "ETIMEDOUT"; break;
    default: break;
    }
    return "errno " + std::to_string(error) + (name ? std::string(" (") + name + ")" : std::string{});
}
}

// Title ids and the PS3 disc header (ps4_disc_stream.hpp): canonical_title_id("BLUS-30443") == "BLUS30443",
// has_ps3_magic(sector 1), title_id_from_header(sector 1).
using disc_stream::canonical_title_id;
using disc_stream::has_ps3_magic;
using disc_stream::title_id_from_header;

// Sector 0 of a PS3 disc: u32 BE count of plain regions, u32 0, then the first and last LBA of each plain region.
struct region_table
{
    std::uint32_t plain_regions = 0; // the count field as read
    bool valid = false;              // 1..127, the range the ISO loader accepts
    std::vector<std::pair<std::uint32_t, std::uint32_t>> first; // first plain regions (first, last LBA)
};

inline region_table parse_region_table(const unsigned char* sector0, std::size_t size, std::size_t max_pairs = 3)
{
    region_table out;
    if (size < 8) return out;
    out.plain_regions = detail::be32(sector0);
    out.valid = out.plain_regions >= 1 && out.plain_regions <= 127;
    if (!out.valid) return out;
    for (std::size_t i = 0; i < out.plain_regions && i < max_pairs && 16 + i * 8 <= size; ++i)
        out.first.emplace_back(detail::be32(sector0 + 8 + i * 8), detail::be32(sector0 + 12 + i * 8));
    return out;
}

// No region table in sector 0 (count 0) and a PS3_GAME/PARAM.SFO in the tree: a game folder made into an ISO by a generic
// tool, every file in the clear (a Redump image always has its table). The ISO loader reads it as one unencrypted region.
inline bool plain_image_without_header(std::uint32_t region_count, bool has_ps3_game_sfo) noexcept
{
    return region_count == 0 && has_ps3_game_sfo;
}

struct node_info
{
    int error = 0;            // errno of the failed call, 0 when it succeeded
    bool character = false;
    bool block = false;
    bool directory = false;
    std::uint64_t device = 0; // st_dev
    std::uint64_t rdev = 0;   // st_rdev
    std::int64_t size = 0;    // st_size
};

// Every call the probe makes, so that a test can stand in for the console. system_ops() is the real set.
struct ops
{
    std::function<int(const std::string& directory, std::vector<std::string>& names)> list_names; // 0 or errno
    std::function<node_info(const std::string& path)> stat_path;
    std::function<int(const std::string& path)> write_marker;  // 0 or errno
    std::function<int(const std::string& path)> remove_marker; // 0 or errno (a missing file is 0)
    std::function<int(const std::string& path, int& error)> open_read; // descriptor, or -1 with error set
    std::function<node_info(int fd)> stat_fd;
    std::function<std::int64_t(int fd, int& error)> seek_end; // offset of the end, or -1 with error set
    std::function<std::int64_t(int fd, void* buffer, std::size_t size, std::int64_t offset, int& error)> read_at;
    std::function<void(int fd)> close_fd;
    // Stage 3: sectors 0-1 and 16 through the GoldHEN FTP server. Empty: no stage 3.
    std::function<disc_stream::header_read()> ftp_read;
};

enum class outcome
{
    running,
    blocked,       // the marker of an earlier probe is there: nothing was done
    marker_failed, // the marker could not be written: nothing was done
    no_thread,     // no thread for the probe: nothing was done
    failed,        // stopped: no memory for the read buffer
    no_node,       // stage 0: no allow-listed node is a device
    open_failed,   // stage 1: open() refused
    no_medium,     // stage 1: the node opened and reports a size of 0
    read_failed,   // stage 2: the sectors could not be read
    ps3_disc,      // stage 2 or 3: sector 1 of a PS3 disc came back
    other_disc,    // stage 2 or 3: sectors came back, not of a PS3 disc
    timeout,       // a stage did not finish in time
    ftp_no_server, // stage 3: no FTP server took the connection
    ftp_refused,   // stage 3: the FTP server answered but did not give the sectors
};

struct report
{
    outcome kind = outcome::running;
    int error = 0;          // errno behind open_failed / read_failed / marker_failed / ftp_no_server; reply code of ftp_refused
    unsigned stage = 0;     // stage that timed out
    unsigned seconds = 0;   // its time limit
    std::string node;       // node of stages 1 and 2
    std::int64_t size = -1; // lseek(SEEK_END) of stage 1, -1 when unknown
    std::string title_id;   // ps3_disc ("BLUS30443"; the raw text when it is not a standard id)
    std::uint32_t plain_regions = 0;
    bool via_ftp = false;   // the sectors came through the FTP server (stage 3)
    std::string server;     // stage 3: the server that answered or was tried last ("127.0.0.1:2121")
};

// XMB message line texts in the interface language (pt-BR: 20 px line of about 1220 px).
inline const char* confirm_text() noexcept { return i18n::tr(i18n::str::DPROBE_TESTE_SO_LEITURA_LEITOR_FTP); }
inline const char* running_text() noexcept { return i18n::tr(i18n::str::DPROBE_TESTANDO_LEITOR_DISCO_SO_LEITURA); }

inline std::string report_text(const report& r)
{
    using i18n::str;
    const std::string error = std::to_string(r.error);
    const std::string id = r.title_id.empty() ? std::string("?") : r.title_id;
    switch (r.kind)
    {
    case outcome::running: return running_text();
    case outcome::blocked: return i18n::tr(str::DPROBE_TESTE_DESLIGADO_ANTERIOR_NAO_TERMINOU);
    case outcome::marker_failed: return i18n::trf(str::DPROBE_NAO_FOI_POSSIVEL_GRAVAR_BLOCKED, {error});
    case outcome::no_thread: return i18n::tr(str::DPROBE_SEM_RECURSOS_TESTE_AGORA_TENTE);
    case outcome::failed: return i18n::tr(str::DPROBE_TESTE_PAROU_FALTA_MEMORIA_DETALHES);
    case outcome::no_node: return i18n::tr(str::DPROBE_APP_NAO_VE_NENHUM_LEITOR);
    case outcome::open_failed:
        if (r.error == EPERM || r.error == EACCES) return i18n::trf(str::DPROBE_PS4_NEGOU_ACESSO_LEITOR, {error});
        return i18n::trf(str::DPROBE_LEITOR_NAO_ABRIU, {error});
    case outcome::no_medium: return i18n::tr(str::DPROBE_LEITOR_ABRIU_MAS_INFORMA_TAMANHO);
    case outcome::read_failed: return i18n::trf(str::DPROBE_LEITOR_ABRIU_MAS_LEITURA_FALHOU, {error});
    case outcome::ps3_disc: return i18n::trf(r.via_ftp ? str::DPROBE_LEITURA_OK_FTP_DISCO_PS3 : str::DPROBE_LEITURA_OK_LEITOR_LEU, {id});
    case outcome::other_disc: return i18n::tr(r.via_ftp ? str::DPROBE_LEITURA_OK_FTP_GOLDHEN_MAS : str::DPROBE_LEITURA_OK_MAS_DISCO_NAO);
    case outcome::timeout: return i18n::trf(str::DPROBE_LEITOR_NAO_RESPONDEU, {r.seconds});
    case outcome::ftp_no_server:
        return i18n::trf(str::DPROBE_APP_NAO_VE_LEITOR_FTP, {r.server.empty() ? std::string("127.0.0.1:2121") : r.server});
    case outcome::ftp_refused:
        return i18n::trf(str::DPROBE_FTP_GOLDHEN_RESPONDEU_MAS_NAO, {r.error ? i18n::trf(str::DPROBE_RESPOSTA, {error}) : std::string(i18n::tr(str::DPROBE_SEM_RESPOSTA))});
    }
    return {};
}

// Runs 'work' on a detached thread; false when no thread could be created (ps4_disc_stream.hpp).
using disc_stream::run_detached;

// How long the caller waits for each stage
struct limits
{
    std::chrono::milliseconds stage0{8000};
    std::chrono::milliseconds open{8000};
    std::chrono::milliseconds read{20000};
};

class probe
{
public:
    using log_fn = std::function<void(const std::string&)>;

    // Starts a probe. A refused start (marker present or not writable, no thread) comes back already finished:
    // its first poll() returns the report.
    static std::shared_ptr<probe> start(ops os, log_fn log, limits limit = limits{})
    {
        std::shared_ptr<probe> self(new probe(std::move(os), std::move(log), limit));
        self->log("disc probe: started (read-only; no ioctl; time limits " + std::to_string(limit.stage0.count() / 1000) + "/" +
            std::to_string(limit.open.count() / 1000) + "/" + std::to_string(limit.read.count() / 1000) + " s)");

        if (self->m_os.stat_path(blocked_marker).error == 0)
        {
            self->log("disc probe: refused: /data/rpcs3/disc-probe.blocked exists (an earlier probe did not finish); delete it to test again");
            self->finish_now(outcome::blocked, 0);
            return self;
        }

        if (const int error = self->m_os.write_marker(blocked_marker))
        {
            self->log("disc probe: refused: cannot write /data/rpcs3/disc-probe.blocked (" + detail::errno_text(error) + "); nothing probed");
            self->finish_now(outcome::marker_failed, error);
            return self;
        }

        self->begin(0);

        // The thread keeps the probe alive until it returns, even after the caller dropped it
        if (!run_detached([self] { self->run(); }))
        {
            self->m_os.remove_marker(blocked_marker);
            self->log("disc probe: refused: the probe thread could not be created; nothing probed");
            self->finish_now(outcome::no_thread, 0);
        }

        return self;
    }

    // Caller's thread. Empty while the probe runs; the final report once it finished or ran out of time.
    std::optional<report> poll()
    {
        if (m_final) return m_final;

        if (m_state.load(std::memory_order_acquire) == state_finished)
        {
            std::lock_guard lock(m_mutex);
            m_final = m_report;
            return m_final;
        }

        const unsigned stage = m_stage.load(std::memory_order_acquire);
        const std::int64_t started = m_stage_started.load(std::memory_order_acquire);
        const std::chrono::milliseconds limit = stage >= 2 ? m_limits.read : stage == 1 ? m_limits.open : m_limits.stage0;

        if (now_ms() - started <= limit.count()) return std::nullopt;

        int expected = state_running;
        if (!m_state.compare_exchange_strong(expected, state_timed_out, std::memory_order_acq_rel))
        {
            return poll(); // the worker finished at this very moment
        }

        report out;
        out.kind = outcome::timeout;
        out.stage = stage;
        out.seconds = static_cast<unsigned>((limit.count() + 999) / 1000);
        log("disc probe: TIMEOUT in stage " + std::to_string(stage) + " after " + std::to_string(out.seconds) +
            " s; giving up (disc-probe.blocked kept: delete it to test again)");
        m_final = out;
        return m_final;
    }

private:
    enum : int
    {
        state_running,
        state_finished,
        state_timed_out,
    };

    ops m_os;
    log_fn m_log;
    limits m_limits;
    std::atomic<int> m_state{state_running};
    std::atomic<unsigned> m_stage{0};
    std::atomic<std::int64_t> m_stage_started{0};
    std::int64_t m_started = 0;
    std::mutex m_mutex;
    report m_report;              // worker -> caller, under m_mutex
    std::optional<report> m_final; // caller's thread only

    probe(ops os, log_fn log, limits limit)
        : m_os(std::move(os)), m_log(std::move(log)), m_limits(limit), m_started(now_ms())
    {
    }

    static std::int64_t now_ms()
    {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    void log(const std::string& line)
    {
        if (m_log) m_log(line);
    }

    void begin(unsigned stage)
    {
        // The start time first: a caller that sees the new stage also sees when it started
        m_stage_started.store(now_ms(), std::memory_order_release);
        m_stage.store(stage, std::memory_order_release);
    }

    bool abandoned() const
    {
        return m_state.load(std::memory_order_acquire) != state_running;
    }

    bool late(unsigned stage)
    {
        log("disc probe: stage " + std::to_string(stage) + " returned after " + std::to_string(now_ms() - m_started) +
            " ms, after the time-out; disc-probe.blocked kept");
        return false;
    }

    void finish_now(outcome kind, int error)
    {
        {
            std::lock_guard lock(m_mutex);
            m_report.kind = kind;
            m_report.error = error;
        }
        m_state.store(state_finished, std::memory_order_release);
    }

    void run()
    {
        report out;

        if (!stages(out)) return; // abandoned after a time-out: the marker stays

        {
            std::lock_guard lock(m_mutex);
            m_report = out;
        }

        int expected = state_running;
        if (!m_state.compare_exchange_strong(expected, state_finished, std::memory_order_acq_rel))
        {
            late(m_stage.load(std::memory_order_acquire));
            return;
        }

        const int error = m_os.remove_marker(blocked_marker);
        log("disc probe: finished in " + std::to_string(now_ms() - m_started) + " ms; disc-probe.blocked " +
            (error ? "could not be removed (" + detail::errno_text(error) + ")" : std::string("removed")));
    }

    static std::string describe(const node_info& info)
    {
        if (info.error) return "not visible (" + detail::errno_text(info.error) + ")";
        if (info.character) return "character device, rdev " + detail::hex(info.rdev);
        if (info.block) return "block device, rdev " + detail::hex(info.rdev);
        if (info.directory) return "directory";
        return "not a device";
    }

    void log_names(const std::string& directory)
    {
        std::vector<std::string> names;
        if (const int error = m_os.list_names(directory, names))
        {
            log("disc probe: stage 0: " + directory + " cannot be listed (" + detail::errno_text(error) + ")");
            return;
        }

        std::sort(names.begin(), names.end());
        std::string line = "disc probe: stage 0: " + directory + " lists " + std::to_string(names.size()) + " names:";
        for (const std::string& name : names)
        {
            if (line.size() + name.size() > 900)
            {
                log(line);
                line = "disc probe: stage 0: " + directory + " (more):";
            }
            line += ' ';
            line += detail::printable(reinterpret_cast<const unsigned char*>(name.data()), name.size());
        }
        log(line);
    }

    // false: the caller already gave up on this probe
    bool stages(report& out)
    {
        // Stage 0: names and metadata only, no device is opened
        log_names("/dev");
        log_names("/mnt");

        for (const std::string_view node : drive_nodes)
        {
            const std::string path(node);
            const node_info info = m_os.stat_path(path);
            log("disc probe: stage 0: " + path + ": " + describe(info));
            if (!info.error && (info.character || info.block) && out.node.empty()) out.node = path;
        }

        const node_info mnt = m_os.stat_path("/mnt");
        const node_info disc = m_os.stat_path("/mnt/disc");
        std::string mount = describe(disc);
        if (!disc.error && disc.directory)
        {
            mount += mnt.error ? std::string(", mount point unknown") : disc.device != mnt.device
                ? ", mount point (st_dev " + detail::hex(disc.device) + ", /mnt " + detail::hex(mnt.device) + ")"
                : std::string(", not a mount point");
        }
        log("disc probe: stage 0: /mnt/disc: " + mount);

        if (abandoned()) return late(0);

        if (out.node.empty())
        {
            log("disc probe: no optical node visible to this app");
            out.kind = outcome::no_node;
            return m_os.ftp_read ? ftp_stage(out) : true;
        }

        // Stage 1: open only. Nothing of the medium is read here.
        begin(1);
        log("disc probe: stage 1: open('" + out.node + "', O_RDONLY|O_NONBLOCK|O_CLOEXEC)");
        int error = 0;
        int fd = m_os.open_read(out.node, error);

        if (abandoned())
        {
            if (fd >= 0) m_os.close_fd(fd);
            return late(1);
        }

        if (fd < 0)
        {
            log("disc probe: stage 1: open failed: " + detail::errno_text(error));
            out.kind = outcome::open_failed;
            out.error = error;
            return true;
        }

        const node_info opened = m_os.stat_fd(fd);
        int seek_error = 0;
        const std::int64_t size = m_os.seek_end(fd, seek_error);
        m_os.close_fd(fd);

        log("disc probe: stage 1: opened; fstat: " + describe(opened) + (opened.error ? std::string{} : ", st_size " + std::to_string(opened.size)) +
            "; lseek(SEEK_END): " + (size >= 0 ? std::to_string(size) + " bytes (" + std::to_string(size >> 20) + " MiB)" : detail::errno_text(seek_error)) +
            "; closed");
        out.size = size;

        if (abandoned()) return late(1);

        if (size == 0)
        {
            log("disc probe: stage 1: size 0: no disc in the drive; stage 2 skipped");
            out.kind = outcome::no_medium;
            return true;
        }

        // Stage 2: sectors 0-1 (region table, "PlayStation3" and the title id) and sector 16 ("CD001")
        begin(2);
        log("disc probe: stage 2: reading sectors 0-1 and 16 of " + out.node);

        struct aligned_free
        {
            void operator()(unsigned char* p) const noexcept { ::operator delete(p, std::align_val_t{sector_size}); }
        };
        std::unique_ptr<unsigned char, aligned_free> buffer(
            static_cast<unsigned char*>(::operator new(3 * sector_size, std::align_val_t{sector_size}, std::nothrow)));

        if (!buffer)
        {
            log("disc probe: stage 2: no memory for the 6 KiB read buffer; stopped");
            out.kind = outcome::failed;
            return true;
        }

        std::memset(buffer.get(), 0, 3 * sector_size);

        fd = m_os.open_read(out.node, error);

        if (abandoned())
        {
            if (fd >= 0) m_os.close_fd(fd);
            return late(2);
        }

        if (fd < 0)
        {
            log("disc probe: RAW READ REFUSED " + detail::errno_text(error) + " (open)");
            out.kind = outcome::read_failed;
            out.error = error;
            return true;
        }

        int header_error = 0, pvd_error = 0;
        const std::int64_t header = m_os.read_at(fd, buffer.get(), 2 * sector_size, 0, header_error);
        std::int64_t pvd = -1;

        if (header == static_cast<std::int64_t>(2 * sector_size))
        {
            pvd = m_os.read_at(fd, buffer.get() + 2 * sector_size, sector_size, 16 * sector_size, pvd_error);
        }

        m_os.close_fd(fd);

        if (abandoned()) return late(2);

        if (header != static_cast<std::int64_t>(2 * sector_size))
        {
            log("disc probe: RAW READ REFUSED " + detail::errno_text(header_error) + " (sectors 0-1: " + std::to_string(header) + " bytes)");
            out.kind = outcome::read_failed;
            out.error = header_error;
            return true;
        }

        classify(2, buffer.get(), pvd == static_cast<std::int64_t>(sector_size), detail::errno_text(pvd_error) + " (" + std::to_string(pvd) + " bytes)", {}, out);
        return true;
    }

    // Stage 3, when the app sees no drive node: the same sectors through the GoldHEN FTP server
    bool ftp_stage(report& out)
    {
        begin(3);
        log(std::string("disc probe: stage 3: FTP: sectors 0-1 and 16 of ") + disc_stream::node_path + " through the GoldHEN FTP server (REST/RETR, read-only)");
        const disc_stream::header_read ftp = m_os.ftp_read();

        if (abandoned()) return late(3);

        out.via_ftp = true;
        out.server = disc_stream::to_string(ftp.server);
        const std::string server = out.server + (ftp.banner.empty() ? std::string{} : " (" + ftp.banner + ")");

        switch (ftp.kind)
        {
        case disc_stream::header_read::result::no_server:
            log("disc probe: stage 3: FTP: no server: " + ftp.tried);
            out.kind = outcome::ftp_no_server;
            out.error = ftp.error;
            return true;
        case disc_stream::header_read::result::refused:
        case disc_stream::header_read::result::failed:
            log("disc probe: stage 3: FTP " + server + ": RAW READ REFUSED: " + ftp.reply);
            out.kind = outcome::ftp_refused;
            out.error = ftp.code;
            return true;
        case disc_stream::header_read::result::ok:
            break;
        }

        log("disc probe: stage 3: FTP " + server + ": login " + std::to_string(ftp.connect_ms) + " ms, sectors 0-1 in " +
            std::to_string(ftp.header_ms) + " ms, sector 16 in " + std::to_string(ftp.pvd_ms) + " ms");
        classify(3, ftp.sectors.data(), ftp.pvd_ok, ftp.reply, " via FTP " + out.server, out);
        return true;
    }

    // Sectors 0, 1 and 16 (3 * 2048 bytes; sector 16 only when pvd_read) -> the report
    void classify(unsigned stage, const unsigned char* sectors, bool pvd_read, const std::string& pvd_failure, const std::string& via, report& out)
    {
        const std::string prefix = "disc probe: stage " + std::to_string(stage) + ": ";
        const region_table table = parse_region_table(sectors, sector_size);
        std::string regions;
        if (table.valid)
        {
            regions = std::to_string(table.plain_regions) + " plain regions (" + std::to_string(table.plain_regions * 2 - 1) + " in all); LBA";
            for (const auto& [first, last] : table.first) regions += " " + detail::hex(first) + "-" + detail::hex(last);
        }
        else
        {
            regions = "no PS3 region table (count field " + detail::hex(table.plain_regions) + ")";
        }
        log(prefix + "sector 0: " + regions);

        const unsigned char* sector1 = sectors + sector_size;
        const bool magic = has_ps3_magic(sector1, sector_size);
        const std::string title_text = detail::printable(sector1 + 0x10, 0x10);
        log(magic ? prefix + "sector 1: 'PlayStation3' magic present, title id '" + title_text + "'" : prefix + "sector 1: no 'PlayStation3' magic");

        bool cd001 = false;
        if (pvd_read)
        {
            cd001 = std::memcmp(sectors + 2 * sector_size + 1, "CD001", 5) == 0;
            log(prefix + "sector 16: 'CD001' " + (cd001 ? "present" : "absent"));
        }
        else
        {
            log(prefix + "sector 16: read failed: " + pvd_failure);
        }

        out.plain_regions = table.valid ? table.plain_regions : 0;

        if (magic)
        {
            const std::string id = title_id_from_header(sector1, sector_size);
            out.title_id = id.empty() ? title_text : id;
            log("disc probe: RAW READ OK (PS3 disc " + out.title_id + ", " + std::to_string(out.plain_regions) + " plain regions)" + via);
            out.kind = outcome::ps3_disc;
        }
        else
        {
            log(std::string("disc probe: RAW READ OK but NOT A PS3 DISC (CD001 ") + (cd001 ? "present" : "absent") + ")" + via);
            out.kind = outcome::other_disc;
        }
    }
};

#ifndef _WIN32
namespace detail
{
inline node_info from_stat(const struct ::stat& st)
{
    node_info info;
    info.character = S_ISCHR(st.st_mode);
    info.block = S_ISBLK(st.st_mode);
    info.directory = S_ISDIR(st.st_mode);
    info.device = static_cast<std::uint64_t>(st.st_dev);
    info.rdev = static_cast<std::uint64_t>(st.st_rdev);
    info.size = static_cast<std::int64_t>(st.st_size);
    return info;
}
}

// The console's calls. The drive node is only ever opened with O_RDONLY | O_NONBLOCK | O_CLOEXEC.
inline ops system_ops()
{
    ops os;

    os.list_names = [](const std::string& directory, std::vector<std::string>& names) -> int
    {
        // Names only: no entry is opened or stat'ed
        DIR* const dir = ::opendir(directory.c_str());
        if (!dir) return errno ? errno : EIO;
        while (const dirent* entry = ::readdir(dir))
        {
            const std::string_view name = entry->d_name;
            if (name == "." || name == "..") continue;
            names.emplace_back(name);
            if (names.size() >= 4096) break;
        }
        ::closedir(dir);
        return 0;
    };

    os.stat_path = [](const std::string& path)
    {
        struct ::stat st{};
        if (::stat(path.c_str(), &st) != 0)
        {
            node_info info;
            info.error = errno ? errno : EIO;
            return info;
        }
        return detail::from_stat(st);
    };

    os.write_marker = [](const std::string& path) -> int
    {
        const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
        if (fd < 0) return errno ? errno : EIO;
        static constexpr char text[] = "O teste do leitor de disco não terminou. Apague este arquivo para testar de novo.\n";
        const bool ok = ::write(fd, text, sizeof(text) - 1) == static_cast<ssize_t>(sizeof(text) - 1);
        const int error = ok ? 0 : errno ? errno : EIO;
        ::close(fd);
        return error;
    };

    os.remove_marker = [](const std::string& path) -> int
    {
        return ::unlink(path.c_str()) == 0 || errno == ENOENT ? 0 : errno;
    };

    os.open_read = [](const std::string& path, int& error) -> int
    {
        const int fd = ::open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        error = fd < 0 ? (errno ? errno : EIO) : 0;
        return fd;
    };

    os.stat_fd = [](int fd)
    {
        struct ::stat st{};
        if (::fstat(fd, &st) != 0)
        {
            node_info info;
            info.error = errno ? errno : EIO;
            return info;
        }
        return detail::from_stat(st);
    };

    os.seek_end = [](int fd, int& error) -> std::int64_t
    {
        const off_t end = ::lseek(fd, 0, SEEK_END);
        error = end < 0 ? (errno ? errno : EIO) : 0;
        return static_cast<std::int64_t>(end);
    };

    os.read_at = [](int fd, void* buffer, std::size_t size, std::int64_t offset, int& error) -> std::int64_t
    {
        const ssize_t got = ::pread(fd, buffer, size, static_cast<off_t>(offset));
        error = got < 0 ? (errno ? errno : EIO) : 0;
        return static_cast<std::int64_t>(got);
    };

    os.close_fd = [](int fd)
    {
        ::close(fd);
    };

#if defined(__ORBIS__) && defined(RPCS3_PS4)
    // Stage 3 ends within 16 s, inside the 20 s the caller waits for it
    os.ftp_read = []
    {
        disc_stream::timeouts limits;
        limits.connect_ms = 2000;
        limits.reply_ms = 3000;
        limits.first_byte_ms = 9000;
        limits.data_ms = 4000;
        return disc_stream::read_header(disc_stream::sys::net(), disc_stream::sys::servers(), limits, 16000);
    };
#endif

    return os;
}
#endif
}

namespace utils::ps4::disc_keys
{
// What an imported .dkey file holds: the 16 raw bytes of a binary key, or the 32 hex characters of a text key
// (trailing spaces or a line break are dropped). Empty when the file holds no key. The ISO loader reads exactly
// these two forms (16 bytes: binary, 32 bytes: hex).
inline std::string contents(const unsigned char* data, std::size_t size)
{
    if (size == 16) return std::string(reinterpret_cast<const char*>(data), 16);
    if (size < 32 || size > 40) return {};
    for (std::size_t i = 0; i < 32; ++i)
    {
        const char c = static_cast<char>(data[i]);
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return {};
    }
    for (std::size_t i = 32; i < size; ++i)
    {
        if (data[i] != ' ' && data[i] != '\t' && data[i] != '\r' && data[i] != '\n') return {};
    }
    return std::string(reinterpret_cast<const char*>(data), 32);
}

// Name of the imported copy, empty for anything that is not a .dkey file. A title id stem becomes "BLUS30443.dkey",
// the name the ISO loader looks up by the title id of the disc; any other stem is kept, as the loader also looks up
// "<image name>.dkey". The extension is written in lower case: /data is case-sensitive.
inline std::string target_name(std::string_view name)
{
    if (name.size() <= 5 || name.front() == '.' || name.find('/') != std::string_view::npos) return {};
    const std::string_view extension = name.substr(name.size() - 5);
    static constexpr std::string_view dkey = ".dkey";
    for (std::size_t i = 0; i < dkey.size(); ++i)
    {
        const char c = extension[i] >= 'A' && extension[i] <= 'Z' ? static_cast<char>(extension[i] - 'A' + 'a') : extension[i];
        if (c != dkey[i]) return {};
    }
    const std::string_view stem = name.substr(0, name.size() - 5);
    const std::string title_id = disc_probe::canonical_title_id(stem);
    return (title_id.empty() ? std::string(stem) : title_id) + ".dkey";
}
}

// ISO image files as the library finds them: a copy cut short (an FTP upload stopped or still running) and a disc
// image that the boot refuses for want of PS3_DISC.SFB. Pure: the callers read the sectors.
namespace utils::ps4::disc_image
{
inline constexpr std::uint64_t sector_size = 2048;
// A file at most this much shorter than its image still counts as complete (a tool that rounds the volume size up)
inline constexpr std::uint64_t short_tolerance = 1ull << 20;

// The bytes an image describes: the volume space size of its PVD (sector 16: type 1 and "CD001"; bytes 80-83, little
// endian) times its logical block size (bytes 128-129, little endian) and the end of the last plain region of a PS3
// disc (sector 0 region table, 'PlayStation3' in sector 1). When both can be read the smaller one counts (a disc install
// writes exactly the region-table size, so an image is never called short by the larger of two declared sizes); 0 when
// neither can be read.
inline std::uint64_t expected_bytes(const unsigned char* head, std::size_t head_size, const unsigned char* pvd, std::size_t pvd_size) noexcept
{
    std::uint64_t volume = 0, regions = 0;
    if (pvd && pvd_size >= sector_size && pvd[0] == 1 && std::memcmp(pvd + 1, "CD001", 5) == 0)
    {
        const std::uint64_t blocks = std::uint64_t{pvd[80]} | std::uint64_t{pvd[81]} << 8 | std::uint64_t{pvd[82]} << 16 | std::uint64_t{pvd[83]} << 24;
        const std::uint64_t block = std::uint64_t{pvd[128]} | std::uint64_t{pvd[129]} << 8;
        if (blocks && (block == 512 || block == 1024 || block == 2048)) volume = blocks * block;
    }
    if (head && head_size >= 2 * sector_size && disc_stream::has_ps3_magic(head + sector_size, sector_size))
        if (const disc_stream::layout table = disc_stream::parse_layout(head, sector_size); table.valid) regions = table.size_bytes();
    if (volume && regions) return std::min(volume, regions);
    return volume ? volume : regions;
}

struct completeness
{
    std::uint64_t file = 0;  // bytes of the file
    std::uint64_t image = 0; // bytes the image describes, 0: not known

    bool short_file() const noexcept { return image > file && image - file > short_tolerance; }
    std::uint64_t missing() const noexcept { return short_file() ? image - file : 0; }
};

// Decimal mark of sizes in the interface language: ',' in pt-BR and Spanish, '.' in English and Arabic
inline char decimal_mark() noexcept
{
    const i18n::lang l = i18n::current();
    return l == i18n::lang::en || l == i18n::lang::ar ? '.' : ',';
}

// "9,9 GB", "640 MB", "12 MB" (decimal units, decimal_mark())
inline std::string size_text(std::uint64_t bytes)
{
    char text[32];
    if (bytes >= 1'000'000'000ull) std::snprintf(text, sizeof(text), "%.1f GB", static_cast<double>(bytes) / 1e9);
    else std::snprintf(text, sizeof(text), "%llu MB", static_cast<unsigned long long>(std::max<std::uint64_t>(1, (bytes + 500'000) / 1'000'000)));
    const char mark = decimal_mark();
    for (char& c : text)
        if (c == '.') c = mark;
    return text;
}

// The XMB line of an incomplete ISO (row and refusal; pt-BR at most 116 bytes)
inline std::string incomplete_text(const completeness& c)
{
    return i18n::trf(i18n::str::DPROBE_ISO_INCOMPLETA_FALTAM, {size_text(c.missing())});
}

// A read of the image that ends at least one sector past the end of its file: the game asked for data an incomplete
// copy does not have. A read of the last, partial sector never counts.
inline bool read_past_image_end(std::uint64_t file_size, std::uint64_t offset, std::uint64_t size) noexcept
{
    return size && offset + size >= file_size + sector_size;
}

// A disc game image (CATEGORY DG) without a valid PS3_DISC.SFB (absent, or not starting with ".SFB"): the boot refuses
// it as Emulator::IsValidSfb does (e.g. a PSN game converted to an ISO)
inline bool iso_disc_sfb_missing(std::string_view category, bool sfb_exists, std::string_view sfb_first_bytes) noexcept
{
    return category == "DG" && (!sfb_exists || sfb_first_bytes.substr(0, 4) != ".SFB");
}
}
