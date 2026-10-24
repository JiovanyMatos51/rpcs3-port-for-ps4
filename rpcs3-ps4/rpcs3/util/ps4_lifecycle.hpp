#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

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

// 0.40: one quiet stretch of an activity signature, with two marks. The frontend runs it twice beside stall_watchdog:
// with the 0.38 activity (guest flips, progress counters, an open save list), whose marks get a STALL dump while the
// activity rule keeps the main clock running, and with that activity plus every legitimate wait (progress text, native
// dialogs, heavy file I/O, the home screen), whose second mark is the no-progress failure of the per-game safe mode.
// Pure state machine, main thread only; not running disarms it, as in stall_watchdog.
class quiet_clock
{
public:
    static constexpr std::uint64_t first_mark_us = 60'000'000;
    static constexpr std::uint64_t second_mark_us = 300'000'000;

    void reset_boot() noexcept { *this = quiet_clock{}; }

    // 1 when the stretch reaches first_mark_us, 2 when it reaches second_mark_us (each once per stretch, in that
    // order, one per call), else 0. A changed activity starts a new stretch.
    unsigned poll(bool running, std::uint64_t now, std::uint64_t activity) noexcept
    {
        if (!running)
        {
            m_armed = false;
            return 0;
        }
        if (!m_armed || activity != m_activity)
        {
            m_armed = true;
            m_activity = activity;
            m_since = now;
            m_marks = 0;
            return 0;
        }
        const std::uint64_t quiet = now > m_since ? now - m_since : 0;
        if (m_marks == 0 && quiet >= first_mark_us)
        {
            m_marks = 1;
            return 1;
        }
        if (m_marks == 1 && quiet >= second_mark_us)
        {
            m_marks = 2;
            return 2;
        }
        return 0;
    }

    std::uint64_t quiet_us(std::uint64_t now) const noexcept { return m_armed && now > m_since ? now - m_since : 0; }

private:
    bool m_armed = false;
    std::uint64_t m_activity = 0;
    std::uint64_t m_since = 0;
    unsigned m_marks = 0;
};

// 0.40: guest flips that draw nothing (Mortal Kombat Komplete 0.39 flipped empty frames for 110 s after its movie
// demuxer thread was refused). The frontend samples once per second whether the guest flipped and whether the renderer
// drew since the previous sample. A stretch starts at a flip without a draw; a draw, a pause, an excluded sample (a
// native dialog, the progress text, pipelines still compiling) or a renderer that cannot tell ends it. Pure, main thread.
class empty_frames_watch
{
public:
    static constexpr std::uint64_t dump_us = 60'000'000;
    static constexpr std::uint64_t failure_us = 120'000'000;

    void reset_boot() noexcept { *this = empty_frames_watch{}; }

    // 1 when the stretch reaches dump_us, 2 when it reaches failure_us (each once per stretch), else 0.
    unsigned poll(bool counted, std::uint64_t now, bool flipped, bool drew) noexcept
    {
        if (!counted || drew)
        {
            m_since = 0;
            m_marks = 0;
            return 0;
        }
        if (!m_since)
        {
            if (flipped) m_since = now ? now : 1;
            return 0;
        }
        const std::uint64_t empty = now > m_since ? now - m_since : 0;
        if (m_marks == 0 && empty >= dump_us)
        {
            m_marks = 1;
            return 1;
        }
        if (m_marks == 1 && empty >= failure_us)
        {
            m_marks = 2;
            return 2;
        }
        return 0;
    }

    std::uint64_t empty_us(std::uint64_t now) const noexcept { return m_since && now > m_since ? now - m_since : 0; }

private:
    std::uint64_t m_since = 0; // first flip of the stretch, 0 = no stretch
    unsigned m_marks = 0;
};

// 0.40: "<ID>.session" (adapt folder, util/ps4_adapt.hpp write_record): the state of the last session of a title, one
// line: "boot <s> <draw s> <name>" when it started, "alive <s> <draw s> <name>" every minute, "compile <s> <draw s>
// <name>" instead while a progress text (a cache build) is shown, "end <why>" when it ended in a way the emulator saw
// (back to the XMB, a failure). A record left without "end" after less than 10 min, and not during a cache build, is a
// session the player closed from the home screen early: the next start suggests the per-game safe mode once.
namespace session_record
{
inline constexpr std::string_view suffix = ".session";
inline constexpr std::uint64_t short_session_s = 600;
inline constexpr std::size_t name_bytes = 120;

struct state
{
    bool valid = false;
    bool ended = false;
    bool compiling = false; // the last record was written during a cache build
    std::uint64_t seconds = 0;
    std::uint64_t draw_seconds = 0;
    std::string_view name;
};

// "<what> <seconds> <draw seconds> <name>" into 'out' (NUL-terminated); control characters of the name become spaces
// and it is cut at a UTF-8 boundary. Returns the length (0 when it did not fit). Allocation-free.
inline std::size_t format(char* out, std::size_t size, std::string_view what, std::uint64_t seconds, std::uint64_t draw_seconds, std::string_view name) noexcept
{
    if (!out || !size) return 0;
    const int head = std::snprintf(out, size, "%.*s %llu %llu ", static_cast<int>(what.size()), what.data(), static_cast<unsigned long long>(seconds),
        static_cast<unsigned long long>(draw_seconds));
    if (head <= 0 || static_cast<std::size_t>(head) >= size)
    {
        out[0] = '\0';
        return 0;
    }
    std::size_t n = static_cast<std::size_t>(head);
    std::size_t take = std::min(name.size(), name_bytes);
    while (take && take < name.size() && (static_cast<unsigned char>(name[take]) & 0xc0) == 0x80) --take;
    for (std::size_t i = 0; i < take && n + 1 < size; i++)
        out[n++] = static_cast<unsigned char>(name[i]) < 0x20 ? ' ' : name[i];
    while (n > static_cast<std::size_t>(head) && out[n - 1] == ' ') --n;
    out[n] = '\0';
    return n;
}

inline state parse(std::string_view text) noexcept
{
    state s;
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.remove_suffix(1);
    if (const auto nl = text.find('\n'); nl != std::string_view::npos) text = text.substr(0, nl);
    const auto word = [&text]() -> std::string_view
    {
        while (!text.empty() && text.front() == ' ') text.remove_prefix(1);
        const std::size_t end = std::min(text.find(' '), text.size());
        const std::string_view w = text.substr(0, end);
        text.remove_prefix(end);
        return w;
    };
    const auto number = [](std::string_view w, std::uint64_t& v) noexcept
    {
        if (w.empty() || w.size() > 12) return false;
        v = 0;
        for (const char c : w)
        {
            if (c < '0' || c > '9') return false;
            v = v * 10 + static_cast<std::uint64_t>(c - '0');
        }
        return true;
    };
    const std::string_view what = word();
    if (what == "end")
    {
        s.valid = s.ended = true;
        return s;
    }
    if (what != "boot" && what != "alive" && what != "compile") return s;
    s.compiling = what == "compile";
    if (!number(word(), s.seconds) || !number(word(), s.draw_seconds)) return s;
    while (!text.empty() && text.front() == ' ') text.remove_prefix(1);
    s.name = text;
    s.valid = true;
    return s;
}

// The session did not end through the emulator, lasted less than short_session_s and was not closed during a cache
// build (the first boot's compile is long by design; the safe mode would not shorten it).
inline bool hint_due(const state& s) noexcept
{
    return s.valid && !s.ended && !s.compiling && s.seconds < short_session_s;
}
} // namespace session_record

// 0.40: what the most recent HLE/LV2 calls of one PPU thread repeat. 'Call' has 'const char* name' (one static string
// per function, nullptr = unknown) and an unsigned 'ret' (r3 after the call); calls[0] is the newest. Pure, no allocation.
struct call_pattern
{
    const char* first = nullptr;  // the most frequent name of the window
    const char* second = nullptr; // the next one, nullptr when a single name repeats
    unsigned distinct = 0;        // names in the window; 0 when it is too short or holds an unknown name
    bool pair = false;            // first/second differ only by Open/Close, Create/Destroy, Init/End, ... (any order)
    bool same_error = false;      // every call returned the same CELL error code (0x8xxxxxxx in the low word)
    unsigned long long ret = 0;   // result of the newest call
};

namespace detail
{
constexpr char lower_ascii(char c) noexcept
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

constexpr bool same_text_nocase(std::string_view a, std::string_view b) noexcept
{
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); i++)
        if (lower_ascii(a[i]) != lower_ascii(b[i])) return false;
    return true;
}

// Two function names that open and close the same object: the parts after their common prefix are a known pair.
constexpr bool opposite_calls(std::string_view a, std::string_view b) noexcept
{
    std::size_t p = 0;
    while (p < a.size() && p < b.size() && a[p] == b[p]) p++;
    // the shared prefix ends at a word start (cellAudioPort|Open, sys_event_queue_|create)
    while (p && !(a[p - 1] == '_' || (p < a.size() && a[p] >= 'A' && a[p] <= 'Z'))) p--;
    const std::string_view x = a.substr(p), y = b.substr(p);
    constexpr std::string_view pairs[][2] = {{"open", "close"}, {"create", "destroy"}, {"init", "end"}, {"init", "term"}, {"init", "exit"},
        {"init", "quit"}, {"initialize", "finalize"}, {"start", "stop"}, {"load", "unload"}, {"register", "unregister"}};
    for (const auto& pair : pairs)
        if ((same_text_nocase(x, pair[0]) && same_text_nocase(y, pair[1])) || (same_text_nocase(x, pair[1]) && same_text_nocase(y, pair[0])))
            return true;
    return false;
}

constexpr bool same_name(const char* a, const char* b) noexcept
{
    return a == b || (a && b && std::string_view(a) == std::string_view(b));
}
} // namespace detail

inline constexpr unsigned call_pattern_min_calls = 8;

template <typename Call>
call_pattern recent_pattern(const Call* calls, unsigned n) noexcept
{
    call_pattern out;
    if (!calls || n < call_pattern_min_calls) return out;
    out.ret = calls[0].ret;
    const char* names[4]{};
    unsigned counts[4]{};
    unsigned distinct = 0;
    const unsigned error = static_cast<unsigned>(calls[0].ret & 0xffffffffu);
    out.same_error = (error & 0x80000000u) != 0;
    for (unsigned i = 0; i < n; i++)
    {
        const char* name = calls[i].name;
        if (!name) return call_pattern{};
        if (static_cast<unsigned>(calls[i].ret & 0xffffffffu) != error) out.same_error = false;
        unsigned k = 0;
        while (k < distinct && !detail::same_name(names[k], name)) k++;
        if (k == distinct)
        {
            if (distinct == 4)
            {
                out.distinct = 5; // more than four names: not a short loop
                out.pair = out.same_error = false;
                return out;
            }
            names[distinct++] = name;
        }
        counts[k]++;
    }
    unsigned best = 0, next = 4;
    for (unsigned k = 1; k < distinct; k++)
        if (counts[k] > counts[best]) best = k;
    for (unsigned k = 0; k < distinct; k++)
        if (k != best && (next == 4 || counts[k] > counts[next])) next = k;
    out.distinct = distinct;
    out.first = names[best];
    out.second = next < 4 ? names[next] : nullptr;
    out.pair = out.second && detail::opposite_calls(out.first, out.second);
    // An open/close pair beside a third call of the loop (a sleep between the two) is still the pair: first/second
    // become the pair, the more frequent one first
    for (unsigned a = 0; a < distinct && !out.pair; a++)
    {
        for (unsigned b = a + 1; b < distinct && !out.pair; b++)
        {
            if (!detail::opposite_calls(names[a], names[b])) continue;
            const bool a_first = counts[a] >= counts[b];
            out.first = names[a_first ? a : b];
            out.second = names[a_first ? b : a];
            out.pair = true;
        }
    }
    return out;
}

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
            if (!m_first && keep_history_line(m_line) && !repeated_todo(m_line))
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

    // 0.40: TODO lines left out because their text was already kept todo_keep times
    unsigned repeats() const noexcept { return m_repeats; }

    static constexpr unsigned todo_keep = 3;
    static constexpr std::size_t todo_texts = 4096; // distinct TODO texts counted; later new texts are all kept

private:
    static constexpr std::size_t max_line = 4096;

    // 0.40: a TODO line (U+00B7 then U) is written once per call: Mortal Kombat Komplete's audio probe wrote 19351 of them and the
    // kept part held nothing else. The first todo_keep lines of each text are kept; the text is the line after its time
    // stamp and thread block, so the same TODO from another thread or at another time counts as the same.
    bool repeated_todo(std::string_view line)
    {
        if (line.size() < 3 || static_cast<unsigned char>(line[0]) != 0xc2 || static_cast<unsigned char>(line[1]) != 0xb7 || line[2] != 'U')
            return false;
        std::string_view body = line.substr(3);
        if (const auto brace = body.find("} "); brace != std::string_view::npos)
        {
            body.remove_prefix(brace + 2);
        }
        else
        {
            while (!body.empty() && body.front() == ' ') body.remove_prefix(1);
            const auto space = body.find(' ');
            body.remove_prefix(space == std::string_view::npos ? body.size() : space + 1);
        }
        while (!body.empty() && (body.back() == '\n' || body.back() == '\r')) body.remove_suffix(1);
        const std::size_t h = std::hash<std::string_view>{}(body);
        const auto it = m_todo_seen.find(h);
        if (it == m_todo_seen.end())
        {
            if (m_todo_seen.size() < todo_texts) m_todo_seen.emplace(h, 1u);
            return false;
        }
        if (++it->second <= todo_keep) return false;
        m_repeats++;
        return true;
    }

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
    unsigned m_repeats = 0;
    bool m_first = true;
    std::unordered_map<std::size_t, unsigned> m_todo_seen;
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

// 0.40: what the boot progress text (or the shader preload dialog) says the emulator is doing, for the RPCS3.log
// heartbeat. First boots spent 5-59 min compiling while the heartbeat said "starting or stopping", and testers closed
// several of them, taking that for a hang.
enum class progress_stage : unsigned char
{
    none,         // no progress text
    ppu_compile,  // compiling PPU modules
    ppu_prepare,  // scanning, analyzing, loading or linking PPU modules, applying PPU code
    spu_cache,    // building the SPU cache
    shader_cache, // the disk shader cache preload (its own dialog, before the RSX loop runs)
    other,        // another progress text (stopping, savestate): the heartbeat keeps its state text
};

// The heartbeat text of a stage: "compiling the game (PPU 12 of 88 modules, ~20 min left: do not close)". 'done' is
// clamped to 'total'; total 0 = counts unknown; minutes_left 0 = no measured rate yet. Empty for none and other.
inline std::string progress_stage_text(progress_stage stage, unsigned done, unsigned total, std::uint64_t minutes_left)
{
    char line[160];
    done = std::min(done, total);
    switch (stage)
    {
    case progress_stage::ppu_compile:
        if (!total) return "compiling the game (PPU modules: do not close)";
        if (minutes_left)
            std::snprintf(line, sizeof(line), "compiling the game (PPU %u of %u modules, ~%llu min left: do not close)", done, total,
                static_cast<unsigned long long>(minutes_left));
        else
            std::snprintf(line, sizeof(line), "compiling the game (PPU %u of %u modules: do not close)", done, total);
        return line;
    case progress_stage::ppu_prepare:
        if (!total) return "preparing the game's PPU code (do not close)";
        std::snprintf(line, sizeof(line), "preparing the game's PPU code (%u of %u: do not close)", done, total);
        return line;
    case progress_stage::spu_cache:
        if (!total) return "building the SPU cache (do not close)";
        std::snprintf(line, sizeof(line), "building the SPU cache (%u of %u programs: do not close)", done, total);
        return line;
    case progress_stage::shader_cache: return "loading the shader cache (do not close)";
    case progress_stage::none:
    case progress_stage::other: break;
    }
    return {};
}

// 0.40: the draw part of the heartbeat: 1 s samples with a draw among the samples of the renderer that tells draws,
// since the previous heartbeat. A minute with flips and no draws is a black screen, one without flips a frozen one.
inline std::string heartbeat_draws_text(unsigned draw_seconds, unsigned measured_seconds)
{
    if (!measured_seconds) return "draws not measured";
    char line[64];
    std::snprintf(line, sizeof(line), "draws>0 for %u of %u s", std::min(draw_seconds, measured_seconds), measured_seconds);
    return line;
}

// 0.40: the sample of a log message for the limiter's lines: the start of its format string (never formatted),
// control characters as spaces, cut at a UTF-8 boundary after at most repeat_sample_max bytes, "..." when cut.
// Allocation-free; 'out' (size > 0) is NUL-terminated; returns the length.
inline constexpr std::size_t repeat_sample_max = 60;

inline std::size_t repeat_sample(const char* text, char* out, std::size_t size) noexcept
{
    if (!out || !size) return 0;
    out[0] = '\0';
    if (!text) return 0;
    const std::size_t max = size >= repeat_sample_max + 4 ? repeat_sample_max : size > 4 ? size - 4 : 0;
    std::size_t n = 0;
    while (text[n] && n <= max) n++;
    const bool cut = n > max;
    std::size_t take = std::min(n, max);
    if (cut)
        while (take && (static_cast<unsigned char>(text[take]) & 0xc0) == 0x80) --take;
    std::size_t len = 0;
    for (; len < take && len + 1 < size; len++) out[len] = static_cast<unsigned char>(text[len]) < 0x20 ? ' ' : text[len];
    for (const char* dots = cut ? "..." : ""; *dots && len + 1 < size; ++dots) out[len++] = *dots;
    out[len] = '\0';
    return len;
}

// 0.40: limits a log message that repeats without end (Fallout NV wrote 135,182 'invalid dimensions' lines, Far Cry
// 83,997 'Transfer cropped in Y'). One key (format string, channel and level) passes its first free_lines lines, then
// window_lines per window_us; the lines left out of a window are counted for one summary line. Lock-free, any thread:
// a slot is claimed once with a compare-exchange and never freed, and a key that finds no free slot within 'probe'
// steps is never limited. The claiming line copies its sample into the slot: a format string may live in a caller's
// array that is gone by the time the main thread writes a summary. Constant-initialized: a static instance works
// before main.
class repeat_limiter
{
public:
    static constexpr unsigned slots = 512;
    static constexpr unsigned probe = 8;
    static constexpr std::uint64_t free_lines = 64;
    static constexpr std::uint32_t window_lines = 8;
    static constexpr std::uint64_t window_us = 10'000'000;
    // A format string with fewer literal characters than this ("%s", "%s (%s)") carries whole texts that differ from
    // line to line: its key is never limited.
    static constexpr unsigned min_literal = 8;

    struct verdict
    {
        bool pass = true;
        std::uint32_t summary = 0;    // lines of this key left out of its previous window: report them first
        const char* sample = nullptr; // the key's sample (repeat_sample) when summary is set; lives as long as this object
    };

    // key: the caller's identity of the message (0 is taken as 1); text: its format string, read only during this call;
    // tag: opaque, kept and handed back to flush. now_us: a clock shared by every caller.
    verdict admit(std::uint64_t key, const char* text, const void* tag, std::uint64_t now_us) noexcept
    {
        slot* const s = find(key ? key : 1, text, tag);
        if (!s || s->state.load(std::memory_order_acquire) != limitable) return {};
        if (s->lines.fetch_add(1, std::memory_order_relaxed) < free_lines) return {};
        verdict v;
        v.sample = s->sample;
        const std::uint64_t now = now_us ? now_us : 1;
        std::uint64_t window = s->window.load(std::memory_order_acquire);
        if (!window && s->window.compare_exchange_strong(window, now, std::memory_order_acq_rel)) window = now;
        if (window && now > window && now - window >= window_us && s->window.compare_exchange_strong(window, now, std::memory_order_acq_rel))
        {
            v.summary = s->dropped.exchange(0, std::memory_order_acq_rel);
            s->passed.store(0, std::memory_order_relaxed);
        }
        if (s->passed.fetch_add(1, std::memory_order_relaxed) < window_lines) return v;
        s->dropped.fetch_add(1, std::memory_order_relaxed);
        v.pass = false;
        return v;
    }

    // Main thread, about once per second. limited(sample, tag): once per key, when it first passed free_lines lines.
    // summary(sample, tag, count): a window of a key ended with lines left out and no later line reported them; the
    // key's next line starts a new window.
    template <typename Limited, typename Summary>
    void flush(std::uint64_t now_us, Limited&& limited, Summary&& summary)
    {
        for (slot& s : m_slots)
        {
            if (s.state.load(std::memory_order_acquire) != limitable) continue;
            const char* const text = s.sample;
            const void* const tag = s.tag.load(std::memory_order_relaxed);
            if (s.lines.load(std::memory_order_relaxed) > free_lines && !s.told.exchange(true, std::memory_order_relaxed)) limited(text, tag);
            std::uint64_t window = s.window.load(std::memory_order_acquire);
            if (!window || now_us <= window || now_us - window < window_us || !s.dropped.load(std::memory_order_relaxed)) continue;
            if (!s.window.compare_exchange_strong(window, 0, std::memory_order_acq_rel)) continue;
            const std::uint32_t count = s.dropped.exchange(0, std::memory_order_acq_rel);
            s.passed.store(0, std::memory_order_relaxed);
            if (count) summary(text, tag, count);
        }
    }

    // Literal characters of a printf-style format string, at most min_literal (a "%%" counts as one).
    static unsigned literal_chars(const char* text) noexcept
    {
        unsigned n = 0;
        for (const char* p = text; p && *p && n < min_literal; ++p)
        {
            if (*p != '%')
            {
                n += static_cast<unsigned char>(*p) > ' ';
                continue;
            }
            if (p[1] == '%')
            {
                ++n;
                ++p;
                continue;
            }
            // flags, width, precision and length modifiers, then the conversion character
            ++p;
            while (*p && std::strchr("-+ #0123456789.*hlLqjzt", *p)) ++p;
            if (!*p) break;
        }
        return n;
    }

private:
    static constexpr std::uint32_t claiming = 0, limitable = 1, exempt = 2;

    struct slot
    {
        std::atomic<std::uint64_t> key{0};
        std::atomic<const void*> tag{nullptr};
        std::atomic<std::uint32_t> state{claiming};
        std::atomic<std::uint32_t> passed{0};  // lines passed in the current window
        std::atomic<std::uint32_t> dropped{0}; // lines left out of the current window
        std::atomic<bool> told{false};
        std::atomic<std::uint64_t> lines{0};   // lines seen in total
        std::atomic<std::uint64_t> window{0};  // start of the current window, 0 = none
        char sample[repeat_sample_max + 4]{};  // written once by the claiming line, before 'state' is published
    };

    slot* find(std::uint64_t key, const char* text, const void* tag) noexcept
    {
        std::uint64_t h = (key ^ (key >> 31)) * 0xff51afd7ed558ccdull;
        h ^= h >> 33;
        for (unsigned i = 0; i < probe; i++)
        {
            slot& s = m_slots[(h + i) % slots];
            std::uint64_t seen = s.key.load(std::memory_order_acquire);
            if (seen == key) return &s;
            if (seen) continue;
            if (s.key.compare_exchange_strong(seen, key, std::memory_order_acq_rel))
            {
                repeat_sample(text, s.sample, sizeof(s.sample));
                s.tag.store(tag, std::memory_order_relaxed);
                s.state.store(literal_chars(text) >= min_literal ? limitable : exempt, std::memory_order_release);
                return &s;
            }
            if (seen == key) return &s;
        }
        return nullptr;
    }

    slot m_slots[slots]{};
};

// "log: N line(s) like '<sample>' left out in the last 10 s" (sample: from repeat_sample)
inline std::string repeat_summary_line(std::uint32_t count, const char* sample)
{
    return "log: " + std::to_string(count) + " line(s) like '" + std::string(sample ? sample : "") + "' left out in the last " +
        std::to_string(repeat_limiter::window_us / 1'000'000) + " s";
}

// The one line per key when the limiter starts acting on it.
inline std::string repeat_limited_line(const char* sample)
{
    return "log: '" + std::string(sample ? sample : "") + "' was written " + std::to_string(repeat_limiter::free_lines) + " times; from now on at most " +
        std::to_string(repeat_limiter::window_lines) + " such lines per " + std::to_string(repeat_limiter::window_us / 1'000'000) +
        " s are written, with a count of the rest (log-flood-off.flag keeps every line)";
}
}
