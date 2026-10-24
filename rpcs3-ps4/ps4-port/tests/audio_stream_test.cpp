#include "util/ps4_audio_stream.hpp"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <vector>
#include <iostream>
#include <stdexcept>
#include <thread>

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

// The output thread of PS4AudioBackend with the parked lifetime: one thread for every play, parked in between.
int test_gate()
{
    using namespace std::chrono_literals;
    utils::ps4::stream_gate gate;
    std::atomic<unsigned> fills{0}, streams{0}, starts{0};
    std::atomic<int> fail_at{-1};
    gate.rearm();
    std::thread out([&]
    {
        starts++;
        while (gate.wait_play())
        {
            streams++;
            device hw{1024, nullptr, {}};
            const int r = utils::ps4::audio_stream(1024, [&] { return gate.playing(); },
                [&](std::uint8_t* b, unsigned n) { std::fill_n(b, n, static_cast<std::uint8_t>(fills.fetch_add(1) + 1)); },
                [&](const void* p)
                {
                    std::this_thread::sleep_for(1ms);
                    if (p && static_cast<int>(fills.load()) == fail_at.load()) return -5;
                    return hw.submit(p);
                });
            if (r < 0) gate.halt();
        }
    });
    std::this_thread::sleep_for(20ms);
    if (fills.load() != 0) return 10;              // parked until play()
    gate.pause();                                  // before any play: returns at once
    gate.play();
    while (fills.load() < 10) std::this_thread::sleep_for(1ms);
    const auto pause_start = std::chrono::steady_clock::now();
    gate.pause();                                  // returns once parked
    const auto pause_took = std::chrono::steady_clock::now() - pause_start;
    const unsigned at_pause = fills.load();
    std::this_thread::sleep_for(20ms);
    if (fills.load() != at_pause) return 11;       // no fill after pause() returned
    if (pause_took > 2s) return 14;                // one block in flight and its drain, not a stream
    gate.play();                                   // the same thread resumes
    while (fills.load() < at_pause + 10) std::this_thread::sleep_for(1ms);
    fail_at = static_cast<int>(fills.load() + 3);  // a failed output halts the stream by itself
    while (gate.playing()) std::this_thread::sleep_for(1ms);
    gate.pause();
    const unsigned at_halt = fills.load();
    std::this_thread::sleep_for(20ms);
    if (fills.load() != at_halt) return 12;
    fail_at = -1;
    gate.play();                                   // play() after a halt resumes
    while (fills.load() < at_halt + 5) std::this_thread::sleep_for(1ms);
    gate.stop();                                   // ends the thread from inside a stream
    out.join();
    if (starts.load() != 1 || streams.load() != 3) return 13; // one thread for every play
    if (gate.playing()) return 15;
    return 0;
}

// The backend's destructor and its kill-flag lifetime: stop() of a parked thread, then rearm() and a new thread on the
// same gate; pause() while no thread exists.
int test_gate_rearm()
{
    using namespace std::chrono_literals;
    utils::ps4::stream_gate gate;
    gate.pause();                                  // never started: returns at once
    std::atomic<unsigned> fills{0};
    const auto body = [&]
    {
        while (gate.wait_play())
        {
            utils::ps4::audio_stream(1024, [&] { return gate.playing(); },
                [&](std::uint8_t* b, unsigned n) { std::fill_n(b, n, static_cast<std::uint8_t>(++fills)); },
                [&](const void*) { std::this_thread::sleep_for(1ms); return 0; });
        }
    };
    for (int round = 0; round < 3; round++)
    {
        gate.rearm();
        std::thread t(body);
        const unsigned before = fills.load();
        gate.play();
        while (fills.load() < before + 4) std::this_thread::sleep_for(1ms);
        gate.pause();
        const unsigned at = fills.load();
        std::this_thread::sleep_for(10ms);
        if (fills.load() != at) return 20 + round;
        gate.stop();                               // parked: ends at once
        t.join();
    }
    gate.rearm();                                  // stop() while nothing waits, then a thread that is stopped at once
    gate.stop();
    std::thread late(body);
    late.join();
    return 0;
}

int test_rebuild_due()
{
    using utils::ps4::audio_rebuild_due;
    using utils::ps4::audio_recovery_line_due;
    static_assert(utils::ps4::audio_rebuild_interval_us == 2'000'000);
    if (!audio_rebuild_due(5'000'000, 0)) return 30;           // never rebuilt: due
    if (audio_rebuild_due(5'000'000, 4'000'000)) return 31;    // 1 s after the last rebuild: not yet
    if (audio_rebuild_due(5'999'999, 4'000'000)) return 32;
    if (!audio_rebuild_due(6'000'000, 4'000'000)) return 33;   // 2 s: due
    if (!audio_rebuild_due(1'000'000, 4'000'000)) return 34;   // a clock that went back: due, never stuck
    // A rebuild chance every 0.5 s (128 loops) for 60 s: one rebuild per 2 s (30) instead of 120
    unsigned rebuilds = 0, chances = 0;
    std::uint64_t last = 0;
    for (std::uint64_t now = 500'000; now <= 60'000'000; now += 500'000)
    {
        chances++;
        if (audio_rebuild_due(now, last))
        {
            rebuilds++;
            last = now;
        }
    }
    if (chances != 120 || rebuilds != 30) return 35;
    if (!audio_recovery_line_due(1) || audio_recovery_line_due(2) || audio_recovery_line_due(63) || !audio_recovery_line_due(64) ||
        audio_recovery_line_due(65) || !audio_recovery_line_due(128))
    {
        return 36;
    }
    unsigned lines = 0;
    for (std::uint32_t n = 1; n <= 1000; n++) lines += audio_recovery_line_due(n);
    if (lines != 1 + 15) return 37;                            // the first, then 64, 128, ..., 960
    return 0;
}

int main()
{
    if (const int e = test_gate()) { std::cout << "FAIL: gate " << e << "\n"; return e; }
    if (const int e = test_gate_rearm()) { std::cout << "FAIL: gate rearm " << e << "\n"; return e; }
    if (const int e = test_rebuild_due()) { std::cout << "FAIL: rebuild " << e << "\n"; return e; }

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
    std::cout << "PASS: old PCM corruption detected; S16/float blocks retained; pause/error drain; idle stop; parked output thread gate; rebuild backoff\n";
}
