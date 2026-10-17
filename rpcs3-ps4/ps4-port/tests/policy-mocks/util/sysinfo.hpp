#pragma once

// Host-test stand-in (tests/policy_test.cpp): the PS4's Jaguar cores have no FMA3.

namespace utils
{
inline bool has_fma3()
{
    return false;
}
} // namespace utils
