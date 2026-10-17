#pragma once

#include <algorithm>
#include <cstdint>

// cellAudio decisions on the PS4 (cellAudio.cpp, buffering on): when to wait for a game's audio block, when to give a
// period up, and the tempo that keeps the output continuous while the game produces audio slower than real time.
// Pure functions and plain state; the cellAudio thread owns every instance. All times in microseconds.
// Host test: ps4-port/tests/audio_policy_test.cpp.
namespace utils::ps4::audio_policy
{
// A port that wrote nothing for this long (or since it started) is idle: its untouched blocks are silence, not late.
inline constexpr std::uint64_t idle_after_us = 100'000;
// Periods in a row a port must have been written in before a late block of it holds up the ports that are ready.
inline constexpr std::uint32_t regular_streak = 2;

struct limits
{
    std::uint64_t fully_untouched_timeout = 10'666;     // upstream: 2 periods, no port written
    std::uint64_t partially_untouched_timeout = 21'333; // upstream: 4 periods, some ports written
    std::uint64_t reserve_guard = 16'000;               // longer waits only while the output holds more than this
    std::uint64_t wait_cap = 64'000;                    // longest wait for one period
};

struct port_counts
{
    std::uint32_t active = 0;      // started ports
    std::uint32_t untouched = 0;   // of 'active': nothing written into the block due now
    std::uint32_t idle = 0;        // of 'untouched': idle (idle_after_us)
    std::uint32_t regular = 0;     // of 'untouched', not idle: written in each of the last regular_streak periods
    std::uint32_t in_progress = 0; // of 'active': still being written
};

enum class action : std::uint8_t
{
    mix,              // mix the period now (untouched ports add nothing)
    silence,          // nothing to wait for and nothing written: enqueue silence and advance
    skip,             // an active port is late past every limit: advance without enqueueing (its block is lost)
    wait_untouched,   // poll again for an untouched block
    wait_in_progress, // poll again for a block still being written
};

struct decision
{
    action what = action::mix;
    bool past_timeout = false; // wait_untouched beyond the upstream timeout, allowed by the output reserve
};

// since_period_end: time since the last period advanced; enqueued_us: audio held by the output (ring + resampler);
// no_wait: 'Disable Sampling Skip' (upstream: never wait for an untouched block).
//
// When no port has written its block, every active port is waited for: up to the upstream timeout, then while the
// output still holds more than the guard, up to the cap. Only then the period is given up (skip). Idle ports never
// hold a period up. When some ports have written, only late ports with a regular cadence are waited for, with the same
// limits; the written ports are mixed afterwards whatever the late ones did (upstream drops the whole period).
constexpr decision decide(const port_counts& p, std::uint64_t since_period_end, std::uint64_t enqueued_us, const limits& l, bool no_wait) noexcept
{
    if (p.active == 0)
    {
        return {action::silence, false};
    }

    const bool all_untouched = p.untouched >= p.active;
    const std::uint32_t untouched_active = p.untouched - std::min(p.idle, p.untouched);
    const std::uint32_t waitable = all_untouched ? untouched_active : std::min(p.regular, untouched_active);
    const std::uint64_t timeout = all_untouched ? l.fully_untouched_timeout : l.partially_untouched_timeout;

    if (waitable > 0 && !no_wait)
    {
        if (since_period_end <= timeout)
        {
            return {action::wait_untouched, false};
        }

        if (enqueued_us > l.reserve_guard && since_period_end <= l.wait_cap)
        {
            return {action::wait_untouched, true};
        }
    }

    if (all_untouched)
    {
        return {untouched_active > 0 ? action::skip : action::silence, false};
    }

    if (p.in_progress > 0)
    {
        return {action::wait_in_progress, false};
    }

    return {action::mix, false};
}

// Buckets of the AudioOut line 'late<5/5-10/10-20/20-40/>40=' (time cellAudio waited for a block, in ms).
inline constexpr std::uint32_t late_buckets = 5;

constexpr std::uint32_t late_bucket(std::uint64_t us) noexcept
{
    return us < 5'000 ? 0 : us < 10'000 ? 1 : us < 20'000 ? 2 : us < 40'000 ? 3 : 4;
}

// Periods longer than this (a pause, an audio restart, a host stall) say nothing about the game's pace.
inline constexpr std::uint64_t max_measured_period_us = 200'000;

// The game's audio production rate: blocks delivered per block of real time over about the last 2 s of periods in
// which the game was expected to deliver (periods with every port idle or no port do not count). A period cellAudio
// did not wait for counts at most one block period: above its target fill cellAudio stretches the periods itself,
// which says nothing about the game; below it, short periods that the game kept up with count as they are (refill).
struct rate_tracker
{
    double blocks = 0; // decayed sums
    double time_us = 0;
    std::uint64_t seen_us = 0; // measured time since the reset, saturating at 1 s

    static constexpr double window_us = 2'000'000;
    static constexpr std::uint64_t valid_after_us = 1'000'000;

    constexpr void add(std::uint64_t period_us, bool delivered, bool waited, std::uint64_t block_period_us) noexcept
    {
        if (period_us > max_measured_period_us)
        {
            return;
        }

        const std::uint64_t effective = waited || !delivered ? period_us : std::min(period_us, block_period_us);
        const double keep = 1.0 - std::min(static_cast<double>(period_us) / window_us, 0.5);
        blocks = blocks * keep + (delivered ? 1.0 : 0.0);
        time_us = time_us * keep + static_cast<double>(effective);
        seen_us = std::min(seen_us + period_us, valid_after_us);
    }

    constexpr bool valid() const noexcept
    {
        return seen_us >= valid_after_us && time_us > 0;
    }

    // 1.0 = real time; 1.0 while not valid.
    constexpr double rate(std::uint64_t block_period_us) const noexcept
    {
        return valid() ? blocks * static_cast<double>(block_period_us) / time_us : 1.0;
    }
};

// Rate-matched tempo (replaces the upstream halving controller on the PS4): follows the game's production rate, a
// little lower while the output is below its target fill so it refills, back to 1.00 when the game keeps up.
inline constexpr double tempo_min = 0.70;    // audio_resampler.h RESAMPLER_MIN_FREQ_VAL on the PS4
inline constexpr double tempo_slew = 0.004;  // per period: at most ~0.75 per second
inline constexpr double tempo_apply = 0.005; // smallest change handed to the resampler (except the return to 1.00)

// fill: output audio / its target (1.0 = at target).
constexpr double tempo_target(double rate, double fill) noexcept
{
    return std::clamp(rate * (1.0 + 0.5 * (fill - 1.0)), tempo_min, 1.0);
}

constexpr double tempo_step(double tempo, double target) noexcept
{
    return std::clamp(tempo + std::clamp(target - tempo, -tempo_slew, tempo_slew), tempo_min, 1.0);
}

// Whether the resampler should get 'tempo' now ('current' is its tempo).
constexpr bool tempo_should_apply(double tempo, double current) noexcept
{
    const double delta = tempo > current ? tempo - current : current - tempo;
    return delta >= tempo_apply || (tempo >= 1.0 && current < 1.0);
}

// Automatic continuity: engages once, for 1 s of periods in which the game was expected to deliver, its rate stayed
// below 0.97 or the averaged output fill below 0.40. A period that meets neither restarts the count.
struct continuity_trigger
{
    std::uint64_t held_us = 0;

    static constexpr double slow_rate = 0.97;
    static constexpr double low_fill = 0.40;
    static constexpr std::uint64_t hold_us = 1'000'000;

    constexpr bool add(std::uint64_t period_us, bool rate_valid, double rate, double fill_avg) noexcept
    {
        if (period_us > max_measured_period_us)
        {
            return false;
        }

        if ((rate_valid && rate < slow_rate) || fill_avg < low_fill)
        {
            held_us += period_us;
        }
        else
        {
            held_us = 0;
        }

        return held_us >= hold_us;
    }
};
}
