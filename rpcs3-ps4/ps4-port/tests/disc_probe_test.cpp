#include "../../rpcs3/util/ps4_disc_probe.hpp"

#include <cassert>
#include <condition_variable>
#include <cstdio>
#include <set>

namespace
{
using namespace std::chrono_literals;
namespace probe_ns = utils::ps4::disc_probe;
namespace keys = utils::ps4::disc_keys;

// Sectors 0-1 of a PS3 disc (local header sample: two plain regions 0-0x8bf and 0xdce0-0x2dcff, title BLUS-31426)
// and sector 16 with the ISO 9660 primary volume descriptor.
std::vector<unsigned char> ps3_sectors(bool ps3)
{
    std::vector<unsigned char> disc(17 * probe_ns::sector_size, 0);
    const unsigned char table[] = {0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x08, 0xbf, 0, 0, 0xdc, 0xe0, 0, 0x02, 0xdc, 0xff};
    if (ps3)
    {
        std::memcpy(disc.data(), table, sizeof(table));
        std::memcpy(disc.data() + 0x800, "PlayStation3\0\0\0\0BLUS-31426      ", 32);
    }
    std::memcpy(disc.data() + 16 * probe_ns::sector_size, "\x01" "CD001\x01", 7);
    return disc;
}

// Stands in for the console: device nodes, the marker file and the drive. A call can be made to block until
// release() to imitate a drive that never answers.
struct fake_console
{
    std::mutex mutex;
    std::condition_variable wake;
    bool marker = false;
    int marker_write_error = 0;
    unsigned marker_writes = 0, marker_removes = 0;
    std::set<std::string> devices{"/dev/cd0"};
    std::set<std::string> other_paths; // visible, not devices
    int open_error = 0;
    std::int64_t size = 25'000'000'000;
    int read_error = 0;
    bool block_open = false, block_read = false, released = false;
    std::vector<std::string> opened;
    unsigned closes = 0, reads = 0, lists = 0;
    std::vector<unsigned char> disc = ps3_sectors(true);
    std::vector<std::string> lines;

    void wait_release(std::unique_lock<std::mutex>& lock)
    {
        wake.wait(lock, [&] { return released; });
    }

    void release()
    {
        std::lock_guard lock(mutex);
        released = true;
        wake.notify_all();
    }

    probe_ns::ops ops()
    {
        probe_ns::ops os;
        os.list_names = [this](const std::string& directory, std::vector<std::string>& names) -> int
        {
            std::lock_guard lock(mutex);
            lists++;
            if (directory == "/dev") names = {"null", "zero", "cd0", "da0"};
            else if (directory == "/mnt") names = {"usb0", "sandbox"};
            else return ENOENT;
            return 0;
        };
        os.stat_path = [this](const std::string& path)
        {
            std::lock_guard lock(mutex);
            probe_ns::node_info info;
            if (path == probe_ns::blocked_marker)
            {
                if (!marker) info.error = ENOENT;
                return info;
            }
            if (devices.count(path))
            {
                info.character = true;
                info.rdev = 0x77;
                return info;
            }
            if (path == "/mnt")
            {
                info.directory = true;
                info.device = 1;
                return info;
            }
            if (other_paths.count(path))
            {
                info.directory = true;
                info.device = path == "/mnt/disc" ? 2 : 1;
                return info;
            }
            info.error = ENOENT;
            return info;
        };
        os.write_marker = [this](const std::string& path) -> int
        {
            std::lock_guard lock(mutex);
            assert(path == probe_ns::blocked_marker);
            marker_writes++;
            if (marker_write_error) return marker_write_error;
            marker = true;
            return 0;
        };
        os.remove_marker = [this](const std::string& path) -> int
        {
            std::lock_guard lock(mutex);
            assert(path == probe_ns::blocked_marker);
            marker_removes++;
            marker = false;
            return 0;
        };
        os.open_read = [this](const std::string& path, int& error) -> int
        {
            std::unique_lock lock(mutex);
            // Only the exact drive names may ever be opened
            assert(probe_ns::allowed_node(path));
            opened.push_back(path);
            if (block_open) wait_release(lock);
            if (open_error)
            {
                error = open_error;
                return -1;
            }
            error = 0;
            return 40 + static_cast<int>(opened.size());
        };
        os.stat_fd = [](int)
        {
            probe_ns::node_info info;
            info.character = true;
            return info;
        };
        os.seek_end = [this](int, int& error) -> std::int64_t
        {
            std::lock_guard lock(mutex);
            error = size < 0 ? EINVAL : 0;
            return size;
        };
        os.read_at = [this](int, void* buffer, std::size_t size, std::int64_t offset, int& error) -> std::int64_t
        {
            std::unique_lock lock(mutex);
            reads++;
            // Aligned, whole sectors only, as a raw optical device requires
            assert(reinterpret_cast<std::uintptr_t>(buffer) % probe_ns::sector_size == 0);
            assert(size % probe_ns::sector_size == 0 && offset % static_cast<std::int64_t>(probe_ns::sector_size) == 0);
            if (block_read) wait_release(lock);
            if (read_error)
            {
                error = read_error;
                return -1;
            }
            assert(static_cast<std::size_t>(offset) + size <= disc.size());
            std::memcpy(buffer, disc.data() + offset, size);
            error = 0;
            return static_cast<std::int64_t>(size);
        };
        os.close_fd = [this](int)
        {
            std::lock_guard lock(mutex);
            closes++;
        };
        return os;
    }

    probe_ns::probe::log_fn logger()
    {
        return [this](const std::string& line)
        {
            std::lock_guard lock(mutex);
            lines.push_back(line);
        };
    }

    bool logged(std::string_view text)
    {
        std::lock_guard lock(mutex);
        return std::any_of(lines.begin(), lines.end(), [&](const std::string& line) { return line.find(text) != std::string::npos; });
    }
};

probe_ns::report wait_report(probe_ns::probe& probe)
{
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    for (;;)
    {
        if (const auto report = probe.poll()) return *report;
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(2ms);
    }
}

// Waits until the detached worker has dropped its reference (it returned)
void wait_worker(const std::shared_ptr<probe_ns::probe>& probe)
{
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (probe.use_count() > 1)
    {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(2ms);
    }
}

probe_ns::report run(fake_console& console, probe_ns::limits limit = {})
{
    const auto probe = probe_ns::probe::start(console.ops(), console.logger(), limit);
    const auto report = wait_report(*probe);
    if (report.kind != probe_ns::outcome::timeout) wait_worker(probe);
    assert(probe->poll()->kind == report.kind); // the report stays
    return report;
}

void test_pure()
{
    for (const auto node : {"/dev/cd0", "/dev/cd1", "/dev/cd2", "/dev/cd3"}) assert(probe_ns::allowed_node(node));
    for (const auto node : {"/dev/cd4", "/dev/cd", "/dev/cd0s1", "/dev/cd0/", "/dev//cd0", "dev/cd0", "/dev/da0", "/dev/md0",
             "/dev/sflash0", "/dev/icc_power", "/mnt/disc", "", "/dev/CD0", " /dev/cd0"})
        assert(!probe_ns::allowed_node(node));

    assert(probe_ns::canonical_title_id("BLUS-30443") == "BLUS30443");
    assert(probe_ns::canonical_title_id("blus30443") == "BLUS30443");
    assert(probe_ns::canonical_title_id("BCUS98232") == "BCUS98232");
    for (const auto bad : {"", "BLUS-3044", "BLUS-304431", "BLU-30443", "BLUS--30443", "BLUS3044X", "B1US30443", "BLUS 30443", "BLUS30443-"})
        assert(probe_ns::canonical_title_id(bad).empty());

    const auto disc = ps3_sectors(true);
    assert(probe_ns::has_ps3_magic(disc.data() + 0x800, 0x800));
    assert(probe_ns::title_id_from_header(disc.data() + 0x800, 0x800) == "BLUS31426");
    assert(probe_ns::title_id_from_header(disc.data() + 0x800, 0x1f).empty());
    const auto other = ps3_sectors(false);
    assert(!probe_ns::has_ps3_magic(other.data() + 0x800, 0x800));
    assert(probe_ns::title_id_from_header(other.data() + 0x800, 0x800).empty());

    const auto table = probe_ns::parse_region_table(disc.data(), probe_ns::sector_size);
    assert(table.valid && table.plain_regions == 2 && table.first.size() == 2);
    assert(table.first[0] == std::make_pair(0u, 0x8bfu) && table.first[1] == std::make_pair(0xdce0u, 0x2dcffu));
    std::vector<unsigned char> sector(probe_ns::sector_size, 0);
    assert(!probe_ns::parse_region_table(sector.data(), sector.size()).valid);
    sector[3] = 128;
    assert(!probe_ns::parse_region_table(sector.data(), sector.size()).valid);
    sector[3] = 9;
    const auto many = probe_ns::parse_region_table(sector.data(), sector.size());
    assert(many.valid && many.plain_regions == 9 && many.first.size() == 3);
    // An image without the PS3 disc header: read as plain only with a PS3_GAME/PARAM.SFO and no region table
    assert(probe_ns::plain_image_without_header(0, true) && !probe_ns::plain_image_without_header(0, false));
    for (const std::uint32_t count : {1u, 2u, 127u, 128u}) assert(!probe_ns::plain_image_without_header(count, true));

    const auto key = [](std::string_view text) { return keys::contents(reinterpret_cast<const unsigned char*>(text.data()), text.size()); };
    const std::string hex = "0123456789abcdefABCDEF0123456789";
    assert(key(hex) == hex);
    assert(key(hex + "\r\n") == hex);
    assert(key(hex + "  \n") == hex);
    assert(key(std::string(16, '\x9c')) == std::string(16, '\x9c'));
    assert(key(hex.substr(0, 31)).empty());
    assert(key(hex.substr(0, 17)).empty());
    assert(key("0123456789abcdefABCDEF012345678g").empty());
    assert(key(hex + "x").empty());
    assert(key(hex + std::string(9, ' ')).empty());
    assert(key("").empty());

    assert(keys::target_name("BLUS-30443.DKEY") == "BLUS30443.dkey");
    assert(keys::target_name("blus30443.dkey") == "BLUS30443.dkey");
    assert(keys::target_name("Demon's Souls (USA).dkey") == "Demon's Souls (USA).dkey");
    assert(keys::target_name("Game.DKey") == "Game.dkey");
    for (const auto bad : {"Game.key", "Game.dkey.txt", ".dkey", ".hidden.dkey", "a/b.dkey", "dkey", ""})
        assert(keys::target_name(bad).empty());

    using probe_ns::outcome;
    for (const auto kind : {outcome::running, outcome::blocked, outcome::marker_failed, outcome::no_thread, outcome::failed, outcome::no_node,
             outcome::open_failed, outcome::no_medium, outcome::read_failed, outcome::ps3_disc, outcome::other_disc, outcome::timeout})
    {
        probe_ns::report r;
        r.kind = kind;
        r.error = 13;
        r.title_id = "BLUS30443";
        r.seconds = 20;
        const std::string text = probe_ns::report_text(r);
        assert(!text.empty() && text.size() < 140);
    }
    probe_ns::report r;
    r.kind = outcome::ps3_disc;
    r.title_id = "BLUS30443";
    assert(probe_ns::report_text(r).find("BLUS30443") != std::string::npos);
    r.kind = outcome::open_failed;
    r.error = EPERM;
    assert(probe_ns::report_text(r).find("negou") != std::string::npos);
    r.kind = outcome::timeout;
    r.seconds = 8;
    assert(probe_ns::report_text(r).find(" 8 s") != std::string::npos);
}

void test_probe()
{
    using probe_ns::outcome;

    {
        // An earlier probe hung: nothing is looked at
        fake_console console;
        console.marker = true;
        const auto report = run(console);
        assert(report.kind == outcome::blocked);
        assert(console.marker && console.marker_writes == 0 && console.marker_removes == 0);
        assert(console.lists == 0 && console.opened.empty());
        assert(console.logged("disc probe: refused: /data/rpcs3/disc-probe.blocked exists"));
    }
    {
        // The marker cannot be written: nothing is looked at
        fake_console console;
        console.marker_write_error = ENOSPC;
        const auto report = run(console);
        assert(report.kind == outcome::marker_failed && report.error == ENOSPC);
        assert(console.lists == 0 && console.opened.empty());
    }
    {
        // No drive node: stage 0 only, marker written then removed
        fake_console console;
        console.devices.clear();
        console.other_paths = {"/dev/cd1"}; // visible but not a device: never opened
        const auto report = run(console);
        assert(report.kind == outcome::no_node);
        assert(console.opened.empty() && console.reads == 0);
        assert(console.marker_writes == 1 && console.marker_removes == 1 && !console.marker);
        assert(console.logged("disc probe: stage 0: /dev lists 4 names: cd0 da0 null zero"));
        assert(console.logged("disc probe: stage 0: /mnt lists 2 names: sandbox usb0"));
        assert(console.logged("disc probe: stage 0: /dev/cd0: not visible (errno"));
        assert(console.logged("disc probe: stage 0: /mnt/disc: not visible"));
        assert(console.logged("disc probe: no optical node visible to this app"));
        assert(console.logged("disc-probe.blocked removed"));
    }
    {
        // The sandbox refuses the open: no read is attempted
        fake_console console;
        console.open_error = EPERM;
        const auto report = run(console);
        assert(report.kind == outcome::open_failed && report.error == EPERM);
        assert(console.opened.size() == 1 && console.opened[0] == "/dev/cd0" && console.reads == 0 && console.closes == 0);
        assert(!console.marker && console.marker_removes == 1);
        assert(console.logged("disc probe: stage 1: open failed: errno"));
        assert(console.logged("(EPERM)"));
    }
    {
        // First device node in the allow-list order is the one probed; no disc in the drive
        fake_console console;
        console.devices = {"/dev/cd2", "/dev/cd1"};
        console.other_paths = {"/mnt/disc"};
        console.size = 0;
        const auto report = run(console);
        assert(report.kind == outcome::no_medium && report.size == 0);
        assert(console.opened == std::vector<std::string>{"/dev/cd1"} && console.closes == 1 && console.reads == 0);
        assert(console.logged("disc probe: stage 0: /mnt/disc: directory, mount point (st_dev 0x2, /mnt 0x1)"));
        assert(console.logged("size 0: no disc in the drive; stage 2 skipped"));
        assert(!console.marker);
    }
    {
        // A PS3 disc comes back
        fake_console console;
        const auto report = run(console);
        assert(report.kind == outcome::ps3_disc);
        assert(report.title_id == "BLUS31426" && report.plain_regions == 2 && report.size == 25'000'000'000);
        assert(console.opened.size() == 2 && console.closes == 2 && console.reads == 2);
        assert(console.logged("disc probe: stage 2: sector 0: 2 plain regions (3 in all); LBA 0x0-0x8bf 0xdce0-0x2dcff"));
        assert(console.logged("'PlayStation3' magic present, title id 'BLUS-31426'"));
        assert(console.logged("sector 16: 'CD001' present"));
        assert(console.logged("disc probe: RAW READ OK (PS3 disc BLUS31426, 2 plain regions)"));
        assert(!console.marker && console.marker_removes == 1);
    }
    {
        // Size unknown (lseek failed): the reads are still tried; another kind of disc
        fake_console console;
        console.size = -1;
        console.disc = ps3_sectors(false);
        const auto report = run(console);
        assert(report.kind == outcome::other_disc && report.size == -1);
        assert(console.logged("lseek(SEEK_END): errno"));
        assert(console.logged("no PS3 region table"));
        assert(console.logged("RAW READ OK but NOT A PS3 DISC (CD001 present)"));
    }
    {
        // The drive refuses the read: sector 16 is not tried after sectors 0-1 failed
        fake_console console;
        console.read_error = EIO;
        const auto report = run(console);
        assert(report.kind == outcome::read_failed && report.error == EIO);
        assert(console.reads == 1 && console.closes == 2);
        assert(console.logged("disc probe: RAW READ REFUSED errno"));
        assert(!console.marker);
    }
    {
        // The open never returns: the caller gives up after the stage 1 limit and the marker stays
        fake_console console;
        console.block_open = true;
        const auto probe = probe_ns::probe::start(console.ops(), console.logger(), {1000ms, 150ms, 300ms});
        const auto started = std::chrono::steady_clock::now();
        const auto report = wait_report(*probe);
        assert(report.kind == outcome::timeout && report.stage == 1 && report.seconds == 1);
        assert(std::chrono::steady_clock::now() - started >= 150ms);
        assert(console.logged("disc probe: TIMEOUT in stage 1 after 1 s"));
        assert(console.marker && console.marker_removes == 0);
        // The call returns later: the descriptor is closed, nothing more is done, the marker is kept
        console.release();
        wait_worker(probe);
        assert(console.closes == 1 && console.reads == 0);
        assert(console.marker && console.marker_removes == 0);
        assert(console.logged("disc probe: stage 1 returned after"));
        assert(probe->poll()->kind == outcome::timeout);
    }
    {
        // The read never returns: time-out in stage 2
        fake_console console;
        console.block_read = true;
        const auto probe = probe_ns::probe::start(console.ops(), console.logger(), {1000ms, 1000ms, 150ms});
        const auto report = wait_report(*probe);
        assert(report.kind == outcome::timeout && report.stage == 2);
        assert(console.marker);
        console.release();
        wait_worker(probe);
        assert(console.marker && console.marker_removes == 0 && console.closes == 2);
        assert(console.logged("disc probe: stage 2 returned after"));
    }
    {
        // A probe dropped by its caller still finishes on its own and removes the marker
        fake_console console;
        console.block_read = true;
        std::weak_ptr<probe_ns::probe> weak;
        {
            const auto probe = probe_ns::probe::start(console.ops(), console.logger());
            weak = probe;
        }
        console.release();
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (!weak.expired())
        {
            assert(std::chrono::steady_clock::now() < deadline);
            std::this_thread::sleep_for(2ms);
        }
        assert(!console.marker && console.marker_removes == 1);
        assert(console.logged("RAW READ OK (PS3 disc BLUS31426"));
    }
}

void test_run_detached()
{
    std::atomic<int> value{0};
    assert(probe_ns::run_detached([&value] { value.store(7); }));
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (value.load() != 7)
    {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(1ms);
    }
}
}

// An ISO cut short, a read past its end, a disc image without PS3_DISC.SFB
void test_disc_image()
{
    namespace di = utils::ps4::disc_image;
    std::vector<unsigned char> disc = ps3_sectors(true);
    unsigned char* pvd = disc.data() + 16 * probe_ns::sector_size;
    // The region table alone: the end of the last plain region (0x2dd00 sectors)
    assert(di::expected_bytes(disc.data(), 2 * probe_ns::sector_size, pvd, probe_ns::sector_size) == 0x2dd00ull * 2048);
    // PVD and region table both readable: the smaller declared size counts (a disc install writes the region-table size)
    pvd[80] = 0x00;
    pvd[81] = 0x10;
    pvd[82] = 0x2f;
    pvd[83] = 0x00; // 0x2f1000 blocks
    pvd[128] = 0x00;
    pvd[129] = 0x08; // 2048
    assert(di::expected_bytes(disc.data(), 2 * probe_ns::sector_size, pvd, probe_ns::sector_size) == 0x2dd00ull * 2048);
    pvd[81] = 0x00;
    pvd[82] = 0x02;
    pvd[83] = 0x00; // 0x20000 blocks, fewer than the region table's 0x2dd00
    assert(di::expected_bytes(disc.data(), 2 * probe_ns::sector_size, pvd, probe_ns::sector_size) == 0x20000ull * 2048);
    pvd[81] = 0x10;
    pvd[82] = 0x2f; // 0x2f1000 blocks again
    pvd[129] = 0x07; // a block size no disc has: the region table alone
    assert(di::expected_bytes(disc.data(), 2 * probe_ns::sector_size, pvd, probe_ns::sector_size) == 0x2dd00ull * 2048);
    pvd[129] = 0x08;
    assert(di::expected_bytes(nullptr, 0, pvd, probe_ns::sector_size) == 0x2f1000ull * 2048); // a plain ISO 9660 image: the PVD alone
    const std::vector<unsigned char> other = ps3_sectors(false);
    assert(di::expected_bytes(other.data(), 2 * probe_ns::sector_size, nullptr, 0) == 0);
    pvd[1] = 'X';
    assert(di::expected_bytes(other.data(), 2 * probe_ns::sector_size, pvd, probe_ns::sector_size) == 0);

    // The GTA IV Europe ISO of the logs: EBOOT.BIN at 10.9 GB, the file shorter. 1 MiB short or less is complete.
    const di::completeness gta{9'000'000'000ull, 14'000'000'000ull};
    assert(gta.short_file() && gta.missing() == 5'000'000'000ull);
    assert(di::incomplete_text(gta) == "ISO incompleta: faltam 5,0 GB (cópia interrompida?) · copie de novo" && di::incomplete_text(gta).size() <= 116);
    const auto is_short = [](std::uint64_t file, std::uint64_t image) { return di::completeness{file, image}.short_file(); };
    assert(!is_short(1'000'000'000ull, 1'000'000'000ull + (1 << 20)) && is_short(1'000'000'000ull, 1'000'000'000ull + (1 << 20) + 2048));
    assert(!is_short(5, 0) && !is_short(20'000'000'000ull, 10'000'000'000ull));
    assert(di::size_text(640'000'000ull) == "640 MB" && di::size_text(10'619'912'192ull) == "10,6 GB" && di::size_text(1) == "1 MB");

    // Mortal Kombat Komplete EU (the logs): file 9,939,128,320 bytes, ICON0.PNG read at its end (+63,060): past the end.
    // The last, partial sector and anything inside: no.
    assert(di::read_past_image_end(9'939'128'320ull, 9'939'128'320ull, 63'060));
    assert(!di::read_past_image_end(10'000, 8192, 2048) && !di::read_past_image_end(10'000, 9000, 1000) && !di::read_past_image_end(10'000, 0, 4096));
    assert(di::read_past_image_end(10'000, 9000, 3048) && !di::read_past_image_end(10'000, 9000, 3047) && !di::read_past_image_end(10'000, 20'000, 0));

    // PS3_DISC.SFB
    assert(di::iso_disc_sfb_missing("DG", false, {}) && di::iso_disc_sfb_missing("DG", true, "XSFB") && di::iso_disc_sfb_missing("DG", true, ".SF"));
    assert(!di::iso_disc_sfb_missing("DG", true, std::string_view(".SFB\0\0\x01\0", 8)) && !di::iso_disc_sfb_missing("HG", false, {}) && !di::iso_disc_sfb_missing("", false, {}));
}

int main()
{
    test_pure();
    test_probe();
    test_run_detached();
    test_disc_image();
    std::puts("disc_probe_test: OK");
    return 0;
}
