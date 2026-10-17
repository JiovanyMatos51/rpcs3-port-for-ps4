#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// PS4 process-termination policy (0.25).
// On this console abort() ends as SIGABRT + core dump and _exit()/_Exit()/exit()
// end as SIGSYS inside libkernel _exit; both are reported by the system as
// CE-34878-0. Only the shell's own kill ("Close application") is clean.
// Therefore: record everything durably, then never self-terminate.
void ps4_fatal_init() noexcept;                                    // first statement of main()
[[noreturn]] void ps4_fatal_report(const char* reason, const char* origin) noexcept;
[[noreturn]] void ps4_terminate_handler() noexcept;                 // std::set_terminate target
void ps4_note_thread_fatal(const char* reason) noexcept;           // emergency_exit on a named thread
[[noreturn]] void ps4_leave_application(const char* why) noexcept; // frontend exit ladder
bool ps4_try_leave_application(const char* why) noexcept;          // returns only if the shell did not close us
bool ps4_in_background() noexcept;                                 // PS button / home screen owns the display
extern "C" [[noreturn]] void ps4_exit_called(int code);            // runtime/ps4_exit_guard.c (exit/_Exit)

// Frontend-only helpers (main thread unless stated otherwise).
bool ps4_fatal_pending() noexcept;
// True once per emergency_exit; copies the recorded reason (NUL-terminated, truncated).
// 0.29: a reason containing "in function 'start'" (thread resources exhausted) is kept apart and returned first.
bool ps4_take_thread_fatal(char* reason, std::size_t size) noexcept;
// 0.29: true while a thread-resource exhaustion reason waits for ps4_take_thread_fatal (any thread; no side effect).
bool ps4_thread_exhaustion_pending() noexcept;
// One-line XMB notice for the previous session's fatal record, or empty.
// 0.39: 'reason' gets the record's first "reason: " line and 'title' its "title: " line (empty when absent).
std::string ps4_take_previous_fatal(std::string* reason = nullptr, std::string* title = nullptr);
// 0.39: the result of orbis::threadLimitProbe (thread-limit-probe.flag), read from its startup.log line: true once a
// result was seen; 'extra' = threads it held at once, 'limit' = it stopped at a per-process thread limit.
bool ps4_take_thread_limit_probe(unsigned& extra, bool& limit) noexcept;
// Focus/background/resume logging; rate-limited internally (about every 100 ms).
void ps4_poll_system_service() noexcept;
// Durable (fsync) startup.log line without allocation; any thread.
void ps4_fatal_note(const char* prefix, const char* detail) noexcept;
// PS4 system notification (toast); any thread.
void ps4_notify(const char* message) noexcept;

// Pure helpers, host-testable (ps4-port/tests/lifecycle_test.cpp). No PS4 headers here.
namespace ps4_fatal_detail
{
// Fixed-size, allocation-free text builder (usable under OOM, in signal and terminate context).
template <std::size_t N>
struct fixed_text
{
    static_assert(N >= 2);
    char b[N];
    std::size_t n;

    constexpr fixed_text() noexcept : b{}, n(0) {}
    fixed_text& s(const char* p) noexcept
    {
        while (p && *p && n + 1 < N) b[n++] = *p++;
        b[n] = 0;
        return *this;
    }
    fixed_text& s(std::string_view p) noexcept
    {
        for (const char c : p)
        {
            if (n + 1 >= N) break;
            b[n++] = c;
        }
        b[n] = 0;
        return *this;
    }
    fixed_text& u(unsigned long long v) noexcept
    {
        char t[24];
        int k = 0;
        do t[k++] = static_cast<char>('0' + v % 10); while (v /= 10);
        while (k && n + 1 < N) b[n++] = t[--k];
        b[n] = 0;
        return *this;
    }
    fixed_text& x(unsigned long long v) noexcept
    {
        s("0x");
        char t[17];
        int k = 0;
        do t[k++] = "0123456789abcdef"[v & 15]; while (v >>= 4);
        while (k && n + 1 < N) b[n++] = t[--k];
        b[n] = 0;
        return *this;
    }
    bool full() const noexcept { return n + 1 >= N; }
};

// Cut at a UTF-8 boundary so the XMB font never receives half a code point.
inline std::string utf8_prefix(std::string_view text, std::size_t max_bytes)
{
    if (text.size() <= max_bytes) return std::string(text);
    std::size_t cut = max_bytes;
    while (cut && (static_cast<unsigned char>(text[cut]) & 0xc0) == 0x80) --cut;
    return std::string(text.substr(0, cut)) + "...";
}

// Builds the single-line XMB notice from a fatal.txt record (0.25 format with
// "reason:/exception:/thread:" lines, or any older free-form text).
// 0.39: 'prefix' is the translated start of the line (the frontend passes the interface language's text).
inline std::string previous_fatal_notice(std::string_view all, std::string_view prefix = "Sessão anterior terminou com erro: ")
{
    while (!all.empty() && (all.back() == '\n' || all.back() == '\r' || all.back() == ' ')) all.remove_suffix(1);
    if (all.empty()) return {};
    std::string notice;
    for (const std::string_view key : {std::string_view{"reason: "}, std::string_view{"exception: "}})
    {
        const auto at = all.find(key);
        if (at == std::string_view::npos) continue;
        auto end = all.find('\n', at);
        if (end == std::string_view::npos) end = all.size();
        std::string_view value = all.substr(at + key.size(), end - at - key.size());
        while (!value.empty() && (value.back() == '\r' || value.back() == ' ')) value.remove_suffix(1);
        if (value.empty()) continue;
        if (!notice.empty()) notice += " | ";
        notice += value;
    }
    if (notice.empty())
    {
        auto end = all.find('\n');
        std::string_view first = all.substr(0, end == std::string_view::npos ? all.size() : end);
        while (!first.empty() && first.back() == '\r') first.remove_suffix(1);
        notice = first;
    }
    // The XMB draws one 20 px line (about 110 characters fit).
    return std::string(prefix) + utf8_prefix(notice, 80);
}

// 0.39: the value of the first line of a fatal.txt record that starts with 'key' ("reason: ", "title: "), without its
// line break and trailing spaces; empty when there is none.
inline std::string record_line_value(std::string_view all, std::string_view key)
{
    for (std::size_t at = 0; at < all.size();)
    {
        std::size_t end = all.find('\n', at);
        if (end == std::string_view::npos) end = all.size();
        std::string_view line = all.substr(at, end - at);
        if (line.starts_with(key))
        {
            line.remove_prefix(key.size());
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.remove_suffix(1);
            return std::string(line);
        }
        at = end + 1;
    }
    return {};
}

// 0.39: the record's reason (its first "reason: " line) and the game it belongs to ("title: " line, written since 0.39)
inline std::string previous_fatal_reason(std::string_view all) { return record_line_value(all, "reason: "); }
inline std::string previous_fatal_title(std::string_view all) { return record_line_value(all, "title: "); }

// 0.39: two fatal reasons count as the same failure when the first 40 bytes of their first lines are equal (both
// non-empty): enough for the failing check or the faulting address, not the parts that change between sessions.
inline bool same_fatal_reason(std::string_view a, std::string_view b)
{
    const auto first = [](std::string_view text)
    {
        text = text.substr(0, text.find('\n'));
        while (!text.empty() && (text.back() == '\r' || text.back() == ' ')) text.remove_suffix(1);
        return text.substr(0, 40);
    };
    const std::string_view x = first(a), y = first(b);
    return !x.empty() && x == y;
}

// Parses one "KEY=VALUE" line of /data/rpcs3/mesa-env.txt. Blank lines, comments
// ('#'), lines without '=', empty keys and keys that are not [A-Za-z0-9_] are rejected.
inline bool parse_env_line(std::string_view line, std::string& key, std::string& value)
{
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ' || line.back() == '\t')) line.remove_suffix(1);
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
    if (line.empty() || line.front() == '#') return false;
    const auto eq = line.find('=');
    if (eq == std::string_view::npos || eq == 0) return false;
    std::string_view name = line.substr(0, eq);
    while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) name.remove_suffix(1);
    if (name.empty() || name.size() > 128) return false;
    for (const char c : name)
    {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
        if (!ok) return false;
    }
    if (name.front() >= '0' && name.front() <= '9') return false;
    std::string_view data = line.substr(eq + 1);
    while (!data.empty() && (data.front() == ' ' || data.front() == '\t')) data.remove_prefix(1);
    if (data.size() > 1024) return false;
    key.assign(name);
    value.assign(data);
    return true;
}
} // namespace ps4_fatal_detail

// 0.26: host stack sample (RIP + candidate return addresses as offsets into rpcs3_ps4.elf) of another
// thread, for STALL dumps. Main thread only; waits at most ~300 ms. native_thread = pthread_t as u64.
// 0.29: native signal 31 is shared with the sampling profiler (ps4_profiler.cpp owns the handler and calls
// ps4_stall_sample_signal first); only the requested thread captures.
std::string ps4_sample_thread_stack(std::uint64_t native_thread);

// 0.28: free bytes in libkernel's internal pool (ScePthread threads/mutexes/condvars); 0 if unknown.
unsigned long long ps4_internal_memory_free() noexcept;
// 0.28: free flexible memory (thread stacks); threads created / returned through orbis-compat.
// (Kept for Utilities/Thread.cpp. 'returned' counts only start routines that returned: thread_base threads
// leave through pthread_exit, so created - returned is not a live count; see ps4_thread_census_line.)
unsigned long long ps4_flexible_memory_free() noexcept;
void ps4_thread_census(unsigned long* created, unsigned long* finished) noexcept;
// 0.29: free flexible memory when main() started (recorded by ps4_fatal_init); 0 if unknown.
unsigned long long ps4_boot_flexible_memory_free() noexcept;
// 0.29: one allocation-free line with the orbis-compat thread census (created, failed, running,
// exited but not joined, joined, returned), the libkernel stack estimate (flexible memory used since main / 2 MiB)
// and the live-thread registry counters. Any thread, also the fatal path. Returns the length (NUL-terminated).
std::size_t ps4_thread_census_line(char* out, std::size_t size) noexcept;
