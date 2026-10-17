#include "util/ps4_audio_stream.hpp"
#include <algorithm>
#include <vector>
#include <iostream>
#include <stdexcept>

// Model the device retaining the last submitted pointer, as AudioOut does.
// Completing a block verifies that the producer has not changed its bytes.
struct device
{
    unsigned bytes;
    const std::uint8_t* pending = nullptr;
    std::vector<std::uint8_t> expected;
    unsigned completed = 0, drains = 0;
    int submit(const void* pointer)
    {
        if (pending)
        {
            if (!std::equal(expected.begin(), expected.end(), pending))
                throw std::runtime_error("PCM changed before playback completed");
            ++completed;
        }
        pending = static_cast<const std::uint8_t*>(pointer);
        if (pending) expected.assign(pending, pending + bytes);
        else ++drains;
        return 0;
    }
};

int main()
{
    // This is the old ordering: modifying a live buffer before the drain.
    device old{1024};
    std::array<std::uint8_t, 1024> reused{};
    old.submit(reused.data());
    reused.fill(17);
    bool detected = false;
    try { old.submit(nullptr); }
    catch (const std::runtime_error&) { detected = true; }
    if (!detected) return 1;

    for (const unsigned bytes : {1024u, 2048u})
    {
        device hardware{bytes};
        unsigned produced = 0;
        const int result = utils::ps4::audio_stream(bytes,
            [&] { return produced < 32; },
            [&](std::uint8_t* buffer, unsigned length)
            {
                std::fill_n(buffer, length, static_cast<std::uint8_t>(++produced));
            },
            [&](const void* buffer) { return hardware.submit(buffer); });
        if (result || hardware.completed != 32 || hardware.drains != 1 || hardware.pending) return 2;
    }
    device failing{1024};
    unsigned produced = 0;
    const int result = utils::ps4::audio_stream(1024,
        [] { return true; },
        [&](std::uint8_t* buffer, unsigned length) { std::fill_n(buffer, length, ++produced); },
        [&](const void* buffer)
        {
            if (buffer && produced == 3) return -7;
            return failing.submit(buffer);
        });
    if (result != -7 || failing.completed != 2 || failing.drains != 1 || failing.pending) return 3;
    unsigned output_calls = 0;
    utils::ps4::audio_stream(1024, [] { return false; }, [](auto*, auto) {},
        [&](const void*) { ++output_calls; return 0; });
    if (output_calls) return 4;
    std::cout << "PASS: old PCM corruption detected; S16/float blocks retained; pause/error drain; idle stop\n";
}
