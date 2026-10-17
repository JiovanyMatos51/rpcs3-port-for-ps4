#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

namespace utils::ps4
{
class lifecycle
{
    enum class destination : unsigned char { none, menu, exit };
    static_assert(std::atomic<destination>::is_always_lock_free);
    std::atomic<destination> m_requested{destination::none};
    bool m_stop_started = false; // Accessed only by the main thread.

public:
    enum class action { none, stop_emulation, return_to_menu, exit_process };
    // Frontend requests use graceful shutdown. Termination signals only request an
    // exit (ps4_fatal.cpp); the process never calls _exit/_Exit/exit/abort on PS4
    // (each ends as CE-34878-0) - exit_process means "leave the application".
    void request_exit() noexcept { m_requested.store(destination::exit, std::memory_order_relaxed); }
    void request_menu() noexcept
    {
        auto expected = destination::none;
        m_requested.compare_exchange_strong(expected, destination::menu, std::memory_order_relaxed);
    }
    bool requested() const noexcept { return m_requested.load(std::memory_order_relaxed) != destination::none; }
    bool exiting() const noexcept { return m_requested.load(std::memory_order_relaxed) == destination::exit; }
    // Main thread only, after all guest threads and render resources stopped.
    bool consume_menu_request() noexcept
    {
        auto expected = destination::menu;
        if (!m_requested.compare_exchange_strong(expected, destination::none, std::memory_order_relaxed)) return false;
        m_stop_started = false;
        return true;
    }
    action poll(bool emulation_stopped) noexcept
    {
        const auto requested = m_requested.load(std::memory_order_relaxed);
        if (requested == destination::none) return action::none;
        if (emulation_stopped) return requested == destination::exit ? action::exit_process : action::return_to_menu;
        if (m_stop_started) return action::none;
        m_stop_started = true;
        return action::stop_emulation;
    }
};

// 0.25: always-on stall detector. Pure state machine, main thread
// only: the caller supplies a clock and an activity signature (guest flip counter plus
// progress counters). A dump is due after first_dump_us without activity while the guest
// runs, the second one second_dump_us later, then every repeat_us, at most max_dumps per game boot.
// 0.34: the first dump comes before the 30 s 'game looks frozen' toast (players close the app after it) and the
// second one 10 s later, so two samples of a freeze exist even when the app is closed at once.
class stall_watchdog
{
public:
    static constexpr std::uint64_t first_dump_us = 25'000'000;
    static constexpr std::uint64_t second_dump_us = 10'000'000;
    static constexpr std::uint64_t repeat_us = 120'000'000;
    static constexpr unsigned max_dumps = 5;

    // A new game boot re-arms the dump budget.
    void reset_boot() noexcept { *this = stall_watchdog{}; }

    // Returns true when a dump is due now. Pausing/stopping (running == false) disarms;
    // the stall clock restarts when the guest runs again.
    bool poll(bool running, std::uint64_t now, std::uint64_t activity) noexcept
    {
        if (!running)
        {
            m_armed = false;
            return false;
        }
        if (!m_armed || activity != m_activity)
        {
            m_armed = true;
            m_activity = activity;
            m_since = now;
            m_next = now + first_dump_us;
            m_episode_dumps = 0;
            return false;
        }
        if (m_dumps >= max_dumps || now < m_next) return false;
        ++m_dumps;
        // The short gap applies once per stall episode (a flip or a pause starts a new one).
        m_next = now + (++m_episode_dumps == 1 ? second_dump_us : repeat_us);
        return true;
    }

    std::uint64_t stalled_us(std::uint64_t now) const noexcept { return m_armed && now > m_since ? now - m_since : 0; }
    unsigned dumps() const noexcept { return m_dumps; }

private:
    bool m_armed = false;
    std::uint64_t m_activity = 0;
    std::uint64_t m_since = 0;
    std::uint64_t m_next = 0;
    unsigned m_dumps = 0;
    unsigned m_episode_dumps = 0;
};

// 0.34: pure helpers of the frontend (ps4_main.cpp), host-tested in lifecycle_test.cpp.

// A line from the part of RPCS3.log that the log history copy leaves out, still worth keeping: errors, fatals and
// TODOs (level marker U+00B7 then E/F/U; not the frequent SPU LLVM 'found in FMA' TODO), PS4 and AudioOut lines and
// focus notes.
inline bool keep_history_line(std::string_view line) noexcept
{
    if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xc2 && static_cast<unsigned char>(line[1]) == 0xb7 &&
        (line[2] == 'E' || line[2] == 'F' || line[2] == 'U'))
        return line.find("found in FMA") == std::string_view::npos;
    return line.find("PS4") != std::string_view::npos || line.find("AudioOut") != std::string_view::npos ||
        line.find("focus:") != std::string_view::npos;
}

// The whole lines of consecutive chunks that keep_history_line() accepts, at most `budget` bytes: the newest ones
// when more match. The first line is dropped (the first chunk starts inside a line), a line longer than 4 KiB is
// kept cut, and a line without its newline at the end of the input is not kept. Memory stays below about 2 x budget.
class history_line_filter
{
public:
    explicit history_line_filter(std::size_t budget) noexcept : m_budget(budget) {}

    void feed(const char* data, std::size_t size)
    {
        for (const char *p = data, *end = data + size; p < end;)
        {
            const char* nl = static_cast<const char*>(std::memchr(p, '\n', static_cast<std::size_t>(end - p)));
            const char* stop = nl ? nl + 1 : end;
            if (m_line.size() < max_line) m_line.append(p, std::min<std::size_t>(static_cast<std::size_t>(stop - p), max_line - m_line.size()));
            p = stop;
            if (!nl) return;
            if (!m_first && keep_history_line(m_line))
            {
                if (m_line.back() != '\n') m_line += '\n';
                m_kept += m_line;
                m_matched++;
                if (m_kept.size() > 2 * m_budget) trim();
            }
            m_first = false;
            m_line.clear();
        }
    }

    // The kept lines, oldest first, each ending in '\n'.
    const std::string& kept()
    {
        trim();
        return m_kept;
    }

    unsigned matched() const noexcept { return m_matched; }

private:
    static constexpr std::size_t max_line = 4096;

    // Drops the oldest whole lines until at most m_budget bytes remain (m_kept always ends with '\n').
    void trim()
    {
        if (m_kept.size() <= m_budget) return;
        const std::size_t cut = m_kept.find('\n', m_kept.size() - m_budget - 1);
        m_kept.erase(0, cut == std::string::npos ? m_kept.size() : cut + 1);
    }

    std::size_t m_budget;
    std::string m_kept;
    std::string m_line;
    unsigned m_matched = 0;
    bool m_first = true;
};

// The NPDRM header (NPD_HEADER) in the first bytes of a SELF (in its plain control info) or of an EDAT: magic "NPD\0",
// the license type big endian at +8 (1 network and 2 local both need a .rap, 3 free), the content id at +0x10 (0x30
// bytes, NUL padded). license 0: no header in the data.
struct npd_license
{
    unsigned license = 0;
    std::string content_id;
};

inline npd_license find_npd_license(const unsigned char* data, std::size_t size)
{
    for (std::size_t at = 0; at + 0x40 <= size; at++)
    {
        if (data[at] != 'N' || data[at + 1] != 'P' || data[at + 2] != 'D' || data[at + 3] != 0) continue;
        npd_license result;
        result.license = unsigned{data[at + 8]} << 24 | unsigned{data[at + 9]} << 16 | unsigned{data[at + 10]} << 8 | data[at + 11];
        const char* const id = reinterpret_cast<const char*>(data + at + 0x10);
        result.content_id.assign(id, std::find(id, id + 0x30, '\0'));
        return result;
    }
    return {};
}

inline bool license_needs_rap(unsigned license) noexcept { return license == 1 || license == 2; }

// A file of /data/rpcs3 listed at boot as a switch: every *.flag and *.txt except the fatal.txt session records.
inline bool switch_file(std::string_view name) noexcept
{
    return (name.ends_with(".flag") || name.ends_with(".txt")) && name != "fatal.txt" && name != "previous-fatal.txt";
}

// The switches that change every game and must not stay behind after a console test: fault injectors (test-*,
// refuse-*) and keep-user-settings.flag, which turns off every console limit.
inline bool test_switch_file(std::string_view name) noexcept
{
    return switch_file(name) && (name.starts_with("test-") || name.starts_with("refuse-") || name == "keep-user-settings.flag");
}

// Whole minutes (1 to 9999) until `left` modules are compiled at `per_minute` modules per minute.
inline std::uint64_t compile_minutes_left(unsigned left, double per_minute) noexcept
{
    if (!(per_minute > 0)) return 1;
    return static_cast<std::uint64_t>(std::clamp(std::ceil(left / per_minute), 1., 9999.));
}

// The first-boot compile toast: 40 or more modules to compile and some left, at least 30 s into the stage, and a
// measured rate (2 modules finished) or 90 s without one.
inline bool first_boot_toast_due(unsigned modules, unsigned left, std::uint64_t elapsed_us, bool measured) noexcept
{
    return modules >= 40 && left && elapsed_us >= 30'000'000 && (measured || elapsed_us >= 90'000'000);
}
}
