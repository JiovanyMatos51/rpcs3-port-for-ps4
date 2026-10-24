#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <cassert>
#include <condition_variable>
#include <mutex>

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

// Lifetime gate of the PS4 AudioOut output thread (Emu/Audio/PS4/PS4AudioBackend.cpp): the thread is started once per
// backend and parks between streams, so a resume near the console's thread cap needs no new thread. pause() returns
// only once the thread is parked: no fill or output call runs after it (cellAudio flushes its ring and may install a
// new write callback next). play() wakes the thread, halt() is the thread's own stop after a failed output (the next
// play() resumes it), stop() ends it. Host test: ps4-port/tests/audio_stream_test.cpp.
class stream_gate
{
public:
    // Output thread: parks until play() or stop(). False: stop() was called, the thread ends.
    bool wait_play()
    {
        std::unique_lock lock(m_mutex);
        m_parked = true;
        m_cv.notify_all();
        m_cv.wait(lock, [this] { return m_stop || m_play; });
        if (m_stop) return false;
        m_parked = false;
        return true;
    }

    // Output thread, before each block: keep streaming?
    bool playing() const noexcept { return m_playing.load(std::memory_order_acquire); }

    // Output thread after a failed output: stop streaming without a pause().
    void halt()
    {
        std::lock_guard lock(m_mutex);
        m_play = false;
        m_playing.store(false, std::memory_order_release);
    }

    void play()
    {
        {
            std::lock_guard lock(m_mutex);
            m_play = true;
            m_playing.store(true, std::memory_order_release);
        }
        m_cv.notify_all();
    }

    // Returns once the thread is parked (at once when it never left the park or was never started).
    void pause()
    {
        std::unique_lock lock(m_mutex);
        m_play = false;
        m_playing.store(false, std::memory_order_release);
        m_cv.notify_all();
        m_cv.wait(lock, [this] { return m_parked; });
    }

    void stop()
    {
        {
            std::lock_guard lock(m_mutex);
            m_stop = true;
            m_play = false;
            m_playing.store(false, std::memory_order_release);
        }
        m_cv.notify_all();
    }

    // Before a new thread uses the gate (after stop() and the join of the previous one, or before the first start).
    void rearm()
    {
        std::lock_guard lock(m_mutex);
        m_stop = false;
        m_parked = true;
    }

private:
    std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_play = false;
    bool m_stop = false;
    bool m_parked = true;
    std::atomic<bool> m_playing{false};
};

// cellAudio rebuilds a failed backend (a new port and a new output thread try) at most this often while it keeps
// failing: near the console's thread cap a refused output thread can last minutes, and every rebuild costs a port
// open, a thread create and its log lines (upstream rebuilds every 128 loops, a few times per second).
inline constexpr std::uint64_t audio_rebuild_interval_us = 2'000'000;

// Whether a failed backend may be rebuilt now. 'last_us' is the time of the previous rebuild (0 = none yet), on the
// same clock as 'now_us'; a clock that went back counts as due.
constexpr bool audio_rebuild_due(std::uint64_t now_us, std::uint64_t last_us) noexcept
{
    return last_us == 0 || now_us < last_us || now_us - last_us >= audio_rebuild_interval_us;
}

// The recovery lines of the n-th failure or rebuild (1-based) are logged: the first, then every 64th.
constexpr bool audio_recovery_line_due(std::uint32_t n) noexcept
{
    return n <= 1 || n % 64 == 0;
}
}
