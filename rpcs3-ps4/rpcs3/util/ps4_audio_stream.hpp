#pragma once

#include <array>
#include <cstdint>
#include <cassert>

namespace utils::ps4
{
// AudioOut may keep the submitted pointer until the following output call
// returns. Fill the other block while that pointer belongs to the device;
// drain the last block before these stack buffers are destroyed.
template <typename Running, typename Fill, typename Output>
int audio_stream(unsigned bytes, Running running, Fill fill, Output output)
{
    alignas(64) std::array<std::array<std::uint8_t, 2048>, 2> blocks{};
    assert(bytes && bytes <= blocks[0].size());
    unsigned next = 0;
    bool submitted = false;
    int error = 0;
    while (running())
    {
        auto& block = blocks[next];
        fill(block.data(), bytes);
        error = output(block.data());
        // Even a failed submission may leave the previous block in flight.
        submitted = true;
        if (error < 0) break;
        next ^= 1;
    }
    if (submitted)
    {
        const int drained = output(nullptr);
        if (error >= 0) error = drained;
    }
    return error;
}
}
