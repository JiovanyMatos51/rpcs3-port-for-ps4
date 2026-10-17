#pragma once

// Host-test stand-in (tests/policy_test.cpp): the per-game config directory of the PS4 build.

#include <string>
#include <string_view>

namespace rpcs3::utils
{
inline std::string get_custom_config_dir()
{
    return "/data/rpcs3/config/custom_configs/";
}

inline std::string get_custom_config_path(std::string_view title_id)
{
    return get_custom_config_dir() + "config_" + std::string(title_id) + ".yml";
}
} // namespace rpcs3::utils
