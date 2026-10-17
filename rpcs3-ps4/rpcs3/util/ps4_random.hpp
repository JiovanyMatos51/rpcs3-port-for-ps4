#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

namespace utils::ps4
{
// libSceRandom accepts at most 64 bytes per request. Never replace a failed
// system entropy request with predictable bytes. The caller must check the
// result before copying this temporary output into guest memory.
template <class Generate>
int random_bytes(std::span<std::uint8_t> output, Generate generate)
{
    while (!output.empty())
    {
        const auto count = std::min<std::size_t>(output.size(), 64);
        if (const int error = generate(output.data(), count); error != 0) return error;
        output = output.subspan(count);
    }
    return 0;
}
}
