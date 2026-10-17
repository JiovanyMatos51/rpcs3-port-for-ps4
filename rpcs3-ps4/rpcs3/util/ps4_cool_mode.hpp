#pragma once

// "Modo frio": an opt-in setting that trades a little speed for less CPU load (and so less heat and fan
// noise). It never touches the fan, clocks or any system setting; it only makes the emulator wait instead
// of spinning and run fewer background compiles. The switch is /data/rpcs3/cool-mode.flag, flipped by the
// settings menu; refresh() re-reads it (at the start of every boot and after the menu changes it).
//
// Consumers read enabled() on their own schedule; it is a relaxed atomic load.

#include <atomic>
#include <cerrno>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

namespace utils::ps4::cool_mode
{
inline constexpr const char* flag_path = "/data/rpcs3/cool-mode.flag";

namespace detail
{
inline std::atomic<int>& state() noexcept
{
    // -1: not read yet
    static std::atomic<int> s{-1};
    return s;
}
}

// Re-reads the flag file and returns the new state.
inline bool refresh() noexcept
{
    struct stat st{};
    const bool on = ::stat(flag_path, &st) == 0 && S_ISREG(st.st_mode);
    detail::state().store(on ? 1 : 0, std::memory_order_relaxed);
    return on;
}

inline bool enabled() noexcept
{
    const int v = detail::state().load(std::memory_order_relaxed);
    return v < 0 ? refresh() : v != 0;
}

// Creates or removes the flag; returns false when the file system refused.
inline bool set_enabled(bool on) noexcept
{
    if (on)
    {
        const int fd = ::open(flag_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd < 0) return false;
        static const char text[] = "1\n";
        const bool ok = ::write(fd, text, sizeof(text) - 1) == static_cast<ssize_t>(sizeof(text) - 1);
        ::close(fd);
        if (!ok) return false;
    }
    else if (::unlink(flag_path) != 0 && errno != ENOENT)
    {
        return false;
    }

    detail::state().store(on ? 1 : 0, std::memory_order_relaxed);
    return true;
}
}
