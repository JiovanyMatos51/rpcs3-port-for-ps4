#pragma once

// Host-test stand-in for util/logs.hpp (tests/cfg_browser_test.cpp): the log levels with the real values and YAML
// spellings (fmt_class_string<logs::level> in util/logs.cpp) and a fixed channel list for get_channels().

#include <optional>
#include <set>
#include <string>
#include <string_view>

namespace logs
{
enum class level : unsigned char
{
    always = 0,
    fatal = 1,
    error = 2,
    todo = 3,
    success = 4,
    warning = 5,
    notice = 6,
    trace = 7,

    _default = notice
};

// The channels a test sees (the real list holds every LOG_CHANNEL of the binary)
inline std::set<std::string>& test_channels()
{
    static std::set<std::string> channels{"CFG", "PPU", "SPU", "RSX", "sys_prx", "cellAudio", "cellGame"};
    return channels;
}

inline std::set<std::string> get_channels()
{
    return test_channels();
}
} // namespace logs

namespace cfg_mock
{
inline constexpr std::string_view log_level_names[] = {"Nothing", "Fatal", "Error", "TODO", "Success", "Warning", "Notice", "Trace"};

inline std::string_view log_level_name(logs::level l)
{
    const auto i = static_cast<unsigned>(l);
    return i < std::size(log_level_names) ? log_level_names[i] : std::string_view{};
}

// Like cfg::try_to_enum_value over the level spellings: a spelling, or its number
inline std::optional<logs::level> log_level_from_name(std::string_view text)
{
    for (unsigned i = 0; i < std::size(log_level_names); i++)
        if (log_level_names[i] == text) return static_cast<logs::level>(i);
    if (text.size() == 1 && text[0] >= '0' && text[0] <= '7') return static_cast<logs::level>(text[0] - '0');
    return std::nullopt;
}
} // namespace cfg_mock
