#pragma once

// 0.31: per-title adaptation markers. When a game dies from a resource limit (thread exhaustion, an out-of-memory
// compile, a GPU hang, video memory pressure), the failure path appends one key to /data/rpcs3/adapt/<TITLE>.txt.
// The boot policy reads the active keys of the title it is booting, applies a lighter configuration for them and
// then appends a "boot" line. A key stops applying once 3 "boot" lines follow its last occurrence, so a single bad
// session never slows a game down for good, while a game that keeps failing keeps its adjustment. Once no key
// applies, the policy deletes the file. A game that relaunches itself while its title is still set (another
// executable of the same game, a restart from the home menu) is not counted as a new boot.
//
// File format: one entry per line, either "boot" or a key ("threads=low", "llvm_threads=1", "vram=low",
// "gpu_hang", "heap=cap", ...). Empty lines and lines starting with '#' are ignored, so the file can be edited by hand.
//
// note(), append() and note_boot() never allocate: they use open(O_APPEND)/write/fsync/close only, which makes them
// safe on the fatal, terminate and out-of-memory paths. keys() and parse() allocate and belong to the boot policy
// and the settings menu. This header has no RPCS3 or PS4 includes so host tests can use it; such a test may
// define RPCS3_PS4_ADAPT_DIR to a temporary directory before including it.

#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifndef RPCS3_PS4_ADAPT_DIR
#define RPCS3_PS4_ADAPT_DIR "/data/rpcs3/adapt"
#endif

namespace utils::ps4::adapt
{
inline constexpr char adapt_dir[] = RPCS3_PS4_ADAPT_DIR;
inline constexpr unsigned expiry_boots = 3;            // a key applies to the next 3 boots of its title
inline constexpr std::size_t title_size = 16;          // NUL included; title IDs are 9 characters
inline constexpr std::size_t key_size = 48;            // NUL included
inline constexpr std::size_t read_limit = 16 * 1024;   // keys() reads only the tail of a long file
inline constexpr std::string_view boot_entry = "boot"; // written by note_boot(), never a key

// Keys written by the failure paths and understood by the boot policy.
inline constexpr char key_threads_low[] = "threads=low";       // the console refused a thread
inline constexpr char key_llvm_threads_1[] = "llvm_threads=1"; // a compile ran out of memory
inline constexpr char key_vram_low[] = "vram=low";             // video or direct memory ran out
inline constexpr char key_gpu_hang[] = "gpu_hang";             // the GPU stopped responding
inline constexpr char key_heap_cap[] = "heap=cap";             // the CPU heap reached its cap (governor level 2)

// 0.40: per-game safe mode (util/ps4_safe_mode.hpp). A failed session appends "fail=<kind>" and the next level
// ("safe=<n>"); a clean session at a level appends "safe_ok", which counts as an occurrence of the level in force, so a
// game that works only in safe mode keeps it. The player's choice in the per-game row appends "safe=0" (forget the
// levels before it), then "fail=user" and the chosen level. This family applies for 8 boots instead of 3, so a title
// that failed keeps its level long enough to be tested, and one false failure does not slow it down for good.
inline constexpr unsigned safe_expiry_boots = 8;
inline constexpr char key_safe0[] = "safe=0"; // the player turned the per-game safe mode off
inline constexpr char key_safe1[] = "safe=1";
inline constexpr char key_safe2[] = "safe=2";
inline constexpr char key_safe3[] = "safe=3";
inline constexpr char key_fail_crash[] = "fail=crash";
inline constexpr char key_fail_threads[] = "fail=threads";
inline constexpr char key_fail_stall[] = "fail=stall";
inline constexpr char key_fail_gpu[] = "fail=gpu";
inline constexpr char key_fail_user[] = "fail=user";
inline constexpr char key_safe_ok[] = "safe_ok";

constexpr bool safe_family(std::string_view key) noexcept
{
    return key == key_safe_ok || key.substr(0, 5) == "safe=" || key.substr(0, 5) == "fail=";
}

// 0.40: the GPU device was lost in a session of this title (the device-loss note writes it next to gpu_hang). The boot
// policy turns it into the command-buffer split of the Vulkan renderer for the title's next boots (feature row
// cb_split_after_loss). The loss came after minutes of play at one place of the game, so the split lives 8 boots, as
// the safe family does, long enough to get back there, instead of the 3 boots of the other keys.
inline constexpr char key_cb_split[] = "cb_split";
inline constexpr unsigned cb_split_expiry_boots = 8;
static_assert(cb_split_expiry_boots == safe_expiry_boots, "expiry_for() has one long expiry");

// Keys that apply for 8 boots instead of 3.
constexpr bool long_lived(std::string_view key) noexcept
{
    return safe_family(key) || key == key_cb_split;
}

// Expiry of 'key' when the caller asks for 'expiry' boots: the long-lived keys never expire sooner than 8 boots.
constexpr unsigned expiry_for(std::string_view key, unsigned expiry = expiry_boots) noexcept
{
    return long_lived(key) && expiry < safe_expiry_boots ? safe_expiry_boots : expiry;
}

// Title of the running game, set by the main thread after a successful boot and cleared when the XMB opens again
// (health::on_return_to_xmb does that), so a failure in the XMB is never charged to the last game. Other
// threads (including a fatal path) read it through current_title(), which copies it under g_title_seq because the
// main thread may be switching games at that moment.
inline char g_title[title_size]{};
inline std::atomic<std::uint32_t> g_title_seq{0}; // odd while set_title() is writing

namespace detail
{
// Keys already written for the current title in this session (hash of title + key, 0 = free slot). A failure
// path that runs many times (e.g. every refused guest allocation) then costs one line instead of thousands.
inline std::atomic<std::uint32_t> g_noted[32]{};

constexpr bool title_char(char c) noexcept
{
    return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

constexpr bool key_char(char c) noexcept
{
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '=' || c == '.' || c == '-';
}

// Title IDs only: no path separators, no dots, so the file name can never leave adapt_dir.
constexpr bool valid_title(std::string_view title) noexcept
{
    if (title.empty() || title.size() >= title_size) return false;
    for (const char c : title)
        if (!title_char(c)) return false;
    return true;
}

constexpr bool valid_key(std::string_view key) noexcept
{
    if (key.empty() || key.size() >= key_size || key == boot_entry) return false;
    for (const char c : key)
        if (!key_char(c)) return false;
    return true;
}

constexpr std::uint32_t hash(std::string_view title, std::string_view key) noexcept
{
    std::uint32_t h = 2166136261u; // FNV-1a
    for (const char c : title) h = (h ^ static_cast<unsigned char>(c)) * 16777619u;
    h = (h ^ 0x2fu) * 16777619u;
    for (const char c : key) h = (h ^ static_cast<unsigned char>(c)) * 16777619u;
    return h ? h : 1;
}

constexpr std::size_t bounded_length(const char* s, std::size_t max) noexcept
{
    std::size_t n = 0;
    while (n < max && s[n]) n++;
    return n;
}

// "<adapt_dir>/<title>.txt" (or another suffix) into a caller buffer. False if the title is invalid or the path does
// not fit.
inline bool make_path(std::string_view title, char* out, std::size_t size, std::string_view suffix = ".txt") noexcept
{
    if (!out || !size || !valid_title(title)) return false;
    std::size_t n = 0;
    auto put = [&](std::string_view part)
    {
        for (const char c : part)
        {
            if (n + 1 >= size) return false;
            out[n++] = c;
        }
        return true;
    };
    const bool ok = put(adapt_dir) && put("/") && put(title) && put(suffix);
    out[ok ? n : 0] = '\0';
    return ok;
}

inline int open_append(const char* path, bool create) noexcept
{
#ifdef _WIN32
    return ::_open(path, _O_WRONLY | _O_APPEND | _O_BINARY | (create ? _O_CREAT : 0), _S_IREAD | _S_IWRITE);
#else
    return ::open(path, O_WRONLY | O_APPEND | (create ? O_CREAT : 0), 0666);
#endif
}

inline void make_dir() noexcept
{
#ifdef _WIN32
    ::_mkdir(adapt_dir);
#else
    ::mkdir(adapt_dir, 0777); // EEXIST is fine
#endif
}

inline bool write_all(int fd, const char* p, std::size_t n) noexcept
{
    while (n)
    {
#ifdef _WIN32
        const int w = ::_write(fd, p, static_cast<unsigned>(n));
#else
        const auto w = ::write(fd, p, n);
#endif
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return false;
        p += w;
        n -= static_cast<std::size_t>(w);
    }
    return true;
}

// fsync before close: the shell kill that usually follows a fatal error must not lose the line.
inline void sync_close(int fd) noexcept
{
#ifdef _WIN32
    ::_commit(fd);
    ::_close(fd);
#else
    ::fsync(fd);
    ::close(fd);
#endif
}

inline bool append_line(std::string_view title, std::string_view entry, bool create) noexcept
{
    char path[256];
    if (!make_path(title, path, sizeof(path))) return false;
    char line[key_size + 1];
    std::size_t n = 0;
    for (const char c : entry)
    {
        if (n + 1 >= sizeof(line)) return false;
        line[n++] = c;
    }
    line[n++] = '\n';
    int fd = open_append(path, create);
    if (fd < 0 && create)
    {
        make_dir();
        fd = open_append(path, true);
    }
    if (fd < 0) return false;
    const bool ok = write_all(fd, line, n);
    sync_close(fd);
    return ok;
}
} // namespace detail

// Sets the running game's title (main thread only). An empty or invalid title clears it, so later notes are
// dropped. Also forgets which keys were already written, so the next session can record them again.
// Returns true when a valid title was stored.
inline bool set_title(std::string_view title) noexcept
{
    const bool ok = detail::valid_title(title);
    g_title_seq.fetch_add(1, std::memory_order_acq_rel);
    volatile char* dst = g_title;
    for (std::size_t i = 0; i < title_size; i++) dst[i] = ok && i < title.size() ? title[i] : '\0';
    g_title_seq.fetch_add(1, std::memory_order_release);
    for (auto& slot : detail::g_noted) slot.store(0, std::memory_order_relaxed);
    return ok;
}

inline void clear_title() noexcept
{
    set_title({});
}

// Copies the current title (NUL-terminated). Any thread, allocation-free. False when no title is set.
inline bool current_title(char (&out)[title_size]) noexcept
{
    for (int attempt = 0; attempt < 64; attempt++)
    {
        const std::uint32_t before = g_title_seq.load(std::memory_order_acquire);
        if (before & 1) continue;
        const volatile char* src = g_title;
        for (std::size_t i = 0; i < title_size; i++) out[i] = src[i];
        std::atomic_thread_fence(std::memory_order_acquire);
        if (g_title_seq.load(std::memory_order_relaxed) != before) continue;
        out[title_size - 1] = '\0';
        return out[0] != '\0';
    }
    out[0] = '\0';
    return false;
}

// Appends 'key' to the adapt file of 'title', creating the file and the directory when needed. No deduplication.
// Allocation-free; any thread. False if the title or key is invalid or the write failed.
inline bool append(std::string_view title, std::string_view key) noexcept
{
    return detail::valid_key(key) && detail::append_line(title, key, true);
}

// Records 'key' for the running game (see set_title). Each key is written at most once per session; later calls
// return true without touching the file. Allocation-free and safe on fatal paths; any thread.
// Returns false when no title is set, the key is invalid or the write failed.
inline bool note(const char* key) noexcept
{
    if (!key) return false;
    const std::string_view k(key, detail::bounded_length(key, key_size));
    if (!detail::valid_key(k)) return false;
    char title[title_size];
    if (!current_title(title)) return false;
    const std::string_view t(title, detail::bounded_length(title, title_size));

    const std::uint32_t h = detail::hash(t, k);
    // Look through every slot first: a slot released after a failed write leaves a hole, and claiming that hole
    // while the key sits in a later slot would write the key twice.
    for (const auto& slot : detail::g_noted)
        if (slot.load(std::memory_order_acquire) == h) return true;
    std::atomic<std::uint32_t>* claimed = nullptr;
    for (auto& slot : detail::g_noted)
    {
        std::uint32_t v = 0;
        if (slot.compare_exchange_strong(v, h, std::memory_order_acq_rel))
        {
            claimed = &slot;
            break;
        }
        if (v == h) return true;
    }
    if (detail::append_line(t, k, true)) return true;
    if (claimed) claimed->store(0, std::memory_order_release); // let a later call try again
    return false;
}

// Appends a "boot" line for 'title'. Only titles that already have an adapt file get one, so games that never
// failed never get a file. Call it after reading keys() for that boot (the settings menu must not call it).
inline bool note_boot(std::string_view title) noexcept
{
    return detail::append_line(title, boot_entry, false);
}

struct entry
{
    std::string key;
    unsigned boots_since = 0; // "boot" lines after the key's last occurrence
};

// Parses adapt file text. Returns every key once, in order of first appearance, with the number of boots since
// its last occurrence. Invalid lines are skipped.
inline std::vector<entry> parse(std::string_view text)
{
    struct seen
    {
        std::string_view key;
        std::size_t boot;
    };
    std::vector<seen> list;
    std::size_t boots = 0;
    while (!text.empty())
    {
        const auto nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text.remove_prefix(nl == std::string_view::npos ? text.size() : nl + 1);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.remove_suffix(1);
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
        if (line.empty() || line.front() == '#') continue;
        if (line == boot_entry)
        {
            boots++;
            continue;
        }
        if (!detail::valid_key(line)) continue;
        bool found = false;
        for (auto& s : list)
        {
            if (s.key != line) continue;
            s.boot = boots;
            found = true;
            break;
        }
        if (!found) list.push_back({line, boots});
    }
    std::vector<entry> out;
    out.reserve(list.size());
    for (const auto& s : list) out.push_back({std::string(s.key), static_cast<unsigned>(boots - s.boot)});
    return out;
}

constexpr bool is_active(const entry& e, unsigned expiry = expiry_boots) noexcept
{
    return e.boots_since < expiry_for(e.key, expiry);
}

// 0.40: boots, the next one included, before 'e' stops applying (0 once expired). Use this rather than
// expiry_boots - boots_since, which underflows for the longer-lived safe family.
constexpr unsigned boots_left(const entry& e, unsigned expiry = expiry_boots) noexcept
{
    const unsigned limit = expiry_for(e.key, expiry);
    return e.boots_since < limit ? limit - e.boots_since : 0;
}

// 0.40: the per-game safe-mode state of an adapt file's text (pure, allocation-free).
struct safe_state
{
    unsigned level = 0;       // 0..3: the level the next boot of the title runs
    unsigned boots_since = 0; // "boot" lines since the level was last written or confirmed by safe_ok
    unsigned boots_left = 0;  // boots, the next one included, that the level still applies
    std::string_view kind;    // "crash", "threads", "stall", "gpu", "user": the last fail= entry; empty at level 0
    bool exhausted = false;   // level 3 failed again (the boot asks for the logs)
};

constexpr safe_state safe_status(std::string_view text, unsigned expiry = safe_expiry_boots) noexcept
{
    // Boot index of the last occurrence of each level (1..3); valid only when seen.
    bool seen[4]{};
    unsigned at[4]{};
    unsigned boots = 0;
    // Boot index of the last "safe=3" line; safe_ok does not move it, so a level-3 session that wrote safe_ok and then
    // failed still counts as level 3 failing again
    unsigned written3 = 0;
    std::string_view kind;
    bool exhausted = false;
    const auto level_now = [&]() noexcept -> unsigned
    {
        for (unsigned n = 3; n >= 1; n--)
            if (seen[n] && boots - at[n] < expiry) return n;
        return 0;
    };
    while (!text.empty())
    {
        const auto nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text.remove_prefix(nl == std::string_view::npos ? text.size() : nl + 1);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.remove_suffix(1);
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
        if (line.empty() || line.front() == '#') continue;
        if (line == boot_entry)
        {
            boots++;
            continue;
        }
        if (line == key_safe0)
        {
            for (bool& s : seen) s = false;
            kind = {};
            exhausted = false;
            continue;
        }
        if (line.size() == 6 && line.substr(0, 5) == "safe=" && line[5] >= '1' && line[5] <= '3')
        {
            const unsigned n = static_cast<unsigned>(line[5] - '0');
            // Level 3 written again in a later session (not the player's choice): level 3 did not help either
            exhausted = n == 3 && level_now() == 3 && written3 < boots && kind != "user";
            seen[n] = true;
            at[n] = boots;
            if (n == 3) written3 = boots;
            continue;
        }
        if (line == key_safe_ok)
        {
            if (const unsigned n = level_now())
            {
                at[n] = boots;
                if (n == 3) exhausted = false;
            }
            continue;
        }
        if (line.size() > 5 && line.substr(0, 5) == "fail=")
        {
            kind = line.substr(5);
        }
    }
    safe_state out;
    out.level = level_now();
    if (!out.level) return out;
    out.boots_since = boots - at[out.level];
    out.boots_left = expiry - out.boots_since;
    out.kind = kind;
    out.exhausted = exhausted && out.level == 3;
    return out;
}

// Keys of 'text' that still apply.
inline std::vector<std::string> active_keys(std::string_view text, unsigned expiry = expiry_boots)
{
    std::vector<std::string> out;
    for (auto& e : parse(text))
        if (is_active(e, expiry)) out.push_back(std::move(e.key));
    return out;
}

// Path of the adapt file of 'title', or empty for an invalid title.
inline std::string path(std::string_view title)
{
    char buffer[256];
    return detail::make_path(title, buffer, sizeof(buffer)) ? std::string(buffer) : std::string();
}

// Text of the adapt file (at most the last read_limit bytes, starting at a line boundary); empty if missing.
inline std::string read_text(std::string_view title)
{
    char file_path[256];
    if (!detail::make_path(title, file_path, sizeof(file_path))) return {};
    std::FILE* file = std::fopen(file_path, "rb");
    if (!file) return {};
    long start = 0;
    if (std::fseek(file, 0, SEEK_END) == 0)
    {
        const long size = std::ftell(file);
        if (size > static_cast<long>(read_limit)) start = size - static_cast<long>(read_limit);
    }
    std::string text;
    if (std::fseek(file, start, SEEK_SET) == 0)
    {
        text.resize(read_limit);
        text.resize(std::fread(text.data(), 1, read_limit, file));
    }
    std::fclose(file);
    if (start > 0)
    {
        const auto nl = text.find('\n');
        text.erase(0, nl == std::string::npos ? text.size() : nl + 1);
    }
    return text;
}

// 0.40: the player's choice in the per-game safe-mode row: 0 = off, 1..3 = that level for the next 8 boots of 'title'.
// Earlier levels are forgotten ("safe=0"). Allocation-free; false when a write failed or the level is out of range.
inline bool set_user_level(std::string_view title, unsigned level) noexcept
{
    if (level > 3) return false;
    if (!append(title, key_safe0)) return false;
    if (!level) return true;
    const char* const key = level == 1 ? key_safe1 : level == 2 ? key_safe2 : key_safe3;
    return append(title, key_fail_user) && append(title, key);
}

// Active keys of 'title' (empty when it has no adapt file). Reading has no side effect.
inline std::vector<std::string> keys(std::string_view title)
{
    return active_keys(read_text(title));
}

inline bool has(const std::vector<std::string>& active, std::string_view key) noexcept
{
    for (const auto& k : active)
        if (k == key) return true;
    return false;
}

// Deletes the adapt file of 'title' (e.g. once none of its keys applies any more). True if it is gone.
inline bool remove(std::string_view title)
{
    char file_path[256];
    if (!detail::make_path(title, file_path, sizeof(file_path))) return false;
    if (std::remove(file_path) == 0) return true;
    std::FILE* file = std::fopen(file_path, "rb");
    if (!file) return true;
    std::fclose(file);
    return false;
}

// 0.38: per-title memory record ("<adapt_dir>/<title>.mem", one line in the util/ps4_mem_budget.hpp format),
// rewritten at every stop of the game. Kept apart from the keys: it never expires and never lightens a boot.
inline constexpr std::string_view mem_suffix = ".mem";
inline constexpr std::size_t mem_record_limit = 512;

// 0.39: per-title ring sizes ("<adapt_dir>/<title>.rings", one line in the ring record format of
// util/ps4_vram_policy.hpp), rewritten when a ring of the Vulkan backend grows and read when the next renderer of the
// title creates its rings. Never expires, like the memory record.
inline constexpr std::string_view rings_suffix = ".rings";

// Replaces the record "<title><suffix>" with 'line' (a newline is added; at most mem_record_limit bytes).
// Allocation-free.
inline bool write_record(std::string_view title, std::string_view suffix, std::string_view line) noexcept
{
    char file_path[256];
    if (!detail::make_path(title, file_path, sizeof(file_path), suffix) || line.size() + 1 > mem_record_limit) return false;
    char text[mem_record_limit];
    for (std::size_t i = 0; i < line.size(); i++) text[i] = line[i];
    text[line.size()] = '\n';
#ifdef _WIN32
    int fd = ::_open(file_path, _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY, _S_IREAD | _S_IWRITE);
#else
    int fd = ::open(file_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
#endif
    if (fd < 0)
    {
        detail::make_dir();
#ifdef _WIN32
        fd = ::_open(file_path, _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY, _S_IREAD | _S_IWRITE);
#else
        fd = ::open(file_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
#endif
    }
    if (fd < 0) return false;
    const bool ok = detail::write_all(fd, text, line.size() + 1);
    detail::sync_close(fd);
    return ok;
}

// Text of the record "<title><suffix>" (at most mem_record_limit bytes), empty when there is none.
inline std::string read_record(std::string_view title, std::string_view suffix)
{
    char file_path[256];
    if (!detail::make_path(title, file_path, sizeof(file_path), suffix)) return {};
    std::FILE* file = std::fopen(file_path, "rb");
    if (!file) return {};
    std::string text(mem_record_limit, '\0');
    text.resize(std::fread(text.data(), 1, text.size(), file));
    std::fclose(file);
    return text;
}

// Replaces the memory record of 'title' with 'line' (a newline is added). Allocation-free.
inline bool write_mem_record(std::string_view title, std::string_view line) noexcept
{
    return write_record(title, mem_suffix, line);
}

// Text of the memory record of 'title' (at most mem_record_limit bytes), empty when there is none.
inline std::string read_mem_record(std::string_view title)
{
    return read_record(title, mem_suffix);
}
} // namespace utils::ps4::adapt
