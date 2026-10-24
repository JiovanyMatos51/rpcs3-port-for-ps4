// Host test of util/ps4_audio_policy.hpp (cellAudio wait/skip decisions, production rate, automatic continuity and the
// rate-matched tempo). From the repo root:
//   clang++ -std=c++20 -O2 -static -Wall -Wextra -I rpcs3 ps4-port/tests/audio_policy_test.cpp -o audio_policy_test
//   ./audio_policy_test        (prints PASS and exits 0)
#include "util/ps4_audio_policy.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string_view>

namespace ap = utils::ps4::audio_policy;

namespace
{
int g_failures = 0;
int g_checks = 0;

void check(bool condition, const char* what, int line)
{
    ++g_checks;
    if (!condition)
    {
        std::printf("FAIL line %d: %s\n", line, what);
        ++g_failures;
    }
}

#define CHECK(x) check((x), #x, __LINE__)

constexpr std::uint64_t block_us = 5'333;
constexpr ap::limits lim{}; // 10.67 / 21.33 ms upstream timeouts, 16 ms guard, 64 ms cap

ap::port_counts ports(std::uint32_t active, std::uint32_t untouched, std::uint32_t idle = 0, std::uint32_t regular = 0, std::uint32_t in_progress = 0)
{
    ap::port_counts p;
    p.active = active;
    p.untouched = untouched;
    p.idle = idle;
    p.regular = regular;
    p.in_progress = in_progress;
    return p;
}

void test_decide()
{
    using ap::action;

    // No started port: silence, as upstream.
    CHECK(ap::decide(ports(0, 0), 6'000, 30'000, lim, false).what == action::silence);

    // The game is on time.
    CHECK(ap::decide(ports(1, 0), 5'400, 30'000, lim, false).what == action::mix);
    CHECK(ap::decide(ports(2, 0), 5'400, 0, lim, false).what == action::mix);

    // Late within the upstream timeout: wait, whatever the reserve.
    {
        const auto d = ap::decide(ports(1, 1), 8'000, 0, lim, false);
        CHECK(d.what == action::wait_untouched && !d.past_timeout);
    }

    // Late 15 ms with a 34 ms reserve: wait (upstream would drop the block at 10.7 ms).
    {
        const auto d = ap::decide(ports(1, 1), 15'000, 34'000, lim, false);
        CHECK(d.what == action::wait_untouched && d.past_timeout);
    }

    // Late 15 ms with an empty ring: the block is given up.
    CHECK(ap::decide(ports(1, 1), 15'000, 5'000, lim, false).what == action::skip);
    // Exactly at the guard: no reserve left.
    CHECK(ap::decide(ports(1, 1), 15'000, lim.reserve_guard, lim, false).what == action::skip);

    // Past the cap even with reserve.
    CHECK(ap::decide(ports(1, 1), 64'001, 60'000, lim, false).what == action::skip);
    CHECK(ap::decide(ports(1, 1), 64'000, 60'000, lim, false).what == action::wait_untouched);

    // The only port is idle (stopped writing 100 ms ago): silence at once, never a wait or a skip.
    CHECK(ap::decide(ports(1, 1, 1), 5'400, 0, lim, false).what == action::silence);
    CHECK(ap::decide(ports(2, 2, 2), 30'000, 60'000, lim, false).what == action::silence);

    // Every port untouched, one idle and one active: wait for the active one, then skip (its block is lost).
    CHECK(ap::decide(ports(2, 2, 1), 9'000, 0, lim, false).what == action::wait_untouched);
    CHECK(ap::decide(ports(2, 2, 1), 30'000, 40'000, lim, false).what == action::wait_untouched);
    CHECK(ap::decide(ports(2, 2, 1), 30'000, 10'000, lim, false).what == action::skip);

    // Two ports, one late with a regular cadence: wait for it up to the partial timeout ...
    CHECK(ap::decide(ports(2, 1, 0, 1), 15'000, 0, lim, false).what == action::wait_untouched);
    // ... then the written port is mixed, never dropped with it.
    CHECK(ap::decide(ports(2, 1, 0, 1), 25'000, 10'000, lim, false).what == action::mix);
    // With reserve the late port gets up to the cap, then the other one is mixed.
    {
        const auto d = ap::decide(ports(2, 1, 0, 1), 30'000, 40'000, lim, false);
        CHECK(d.what == action::wait_untouched && d.past_timeout);
    }
    CHECK(ap::decide(ports(2, 1, 0, 1), 70'000, 40'000, lim, false).what == action::mix);

    // A sibling without a regular cadence (or idle) never holds the written ports up.
    CHECK(ap::decide(ports(2, 1, 0, 0), 6'000, 60'000, lim, false).what == action::mix);
    CHECK(ap::decide(ports(3, 1, 1, 0), 6'000, 60'000, lim, false).what == action::mix);

    // A port still being written is waited for after the untouched ones were settled.
    CHECK(ap::decide(ports(2, 0, 0, 0, 1), 6'000, 0, lim, false).what == action::wait_in_progress);
    CHECK(ap::decide(ports(3, 1, 0, 0, 1), 6'000, 0, lim, false).what == action::wait_in_progress);

    // 'Disable Sampling Skip': never wait for an untouched block.
    CHECK(ap::decide(ports(1, 1), 1'000, 60'000, lim, true).what == action::skip);
    CHECK(ap::decide(ports(2, 1, 0, 1), 1'000, 60'000, lim, true).what == action::mix);

    // Inconsistent counts from a caller must not underflow.
    CHECK(ap::decide(ports(1, 1, 5, 5), 30'000, 0, lim, false).what == action::silence);
}

void test_late_bucket()
{
    CHECK(ap::late_bucket(0) == 0);
    CHECK(ap::late_bucket(4'999) == 0);
    CHECK(ap::late_bucket(5'000) == 1);
    CHECK(ap::late_bucket(9'999) == 1);
    CHECK(ap::late_bucket(10'000) == 2);
    CHECK(ap::late_bucket(20'000) == 3);
    CHECK(ap::late_bucket(39'999) == 3);
    CHECK(ap::late_bucket(40'000) == 4);
    CHECK(ap::late_bucket(1'000'000'000) == 4);
}

// One port written every period until 'stop_at', then never again: the decisions of a 250 us poll loop.
void test_port_stops()
{
    using ap::action;

    std::uint64_t now = 0;
    std::uint64_t period_end = 0;
    std::uint64_t last_touch = 0;
    std::uint64_t ring_us = 64'000;
    const std::uint64_t stop_at = 1'000'000;
    std::uint32_t waits_after_idle = 0;
    std::uint32_t skips = 0;
    std::uint64_t first_silence = 0;

    while (now < 2'000'000)
    {
        // Period wait (the dynamic period, kept at one block here).
        const std::uint64_t due = period_end + block_us;
        if (now < due)
        {
            ring_us -= std::min(ring_us, due - now);
            now = due;
        }

        const bool written = now < stop_at;
        if (written)
        {
            last_touch = now;
        }

        const bool idle = now - last_touch >= ap::idle_after_us;
        const auto d = ap::decide(ports(1, written ? 0 : 1, !written && idle ? 1 : 0), now - period_end, ring_us, lim, false);

        if (d.what == action::wait_untouched || d.what == action::wait_in_progress)
        {
            if (idle)
            {
                ++waits_after_idle;
            }

            ring_us -= std::min<std::uint64_t>(ring_us, 250);
            now += 250;
            continue;
        }

        if (d.what == action::skip)
        {
            ++skips;
        }
        else if (d.what == action::silence)
        {
            if (!first_silence)
            {
                first_silence = now;
            }

            ring_us += block_us;
        }
        else
        {
            ring_us += block_us;
        }

        period_end = now;
    }

    CHECK(waits_after_idle == 0);
    // Silence starts once the port went idle (100 ms after its last write), not before.
    CHECK(last_touch < stop_at && last_touch + 2 * block_us >= stop_at);
    CHECK(first_silence >= last_touch + ap::idle_after_us);
    CHECK(first_silence <= last_touch + ap::idle_after_us + 2 * lim.wait_cap);
    // Between the stop and idle the stopped port is waited for and given up, never silenced.
    CHECK(skips > 0 && skips < 20);
}

void test_rate_tracker()
{
    ap::rate_tracker r;
    CHECK(!r.valid());
    CHECK(r.rate(block_us) == 1.0);

    // Real time, nothing waited: 1.00, valid after 1 s.
    for (int i = 0; i < 180; i++) r.add(block_us, true, false, block_us);
    CHECK(!r.valid());
    for (int i = 0; i < 400; i++) r.add(block_us, true, false, block_us);
    CHECK(r.valid());
    CHECK(std::fabs(r.rate(block_us) - 1.0) < 0.002);

    // cellAudio stretching its own periods above the target fill (6.4 ms, no wait): still 1.00.
    for (int i = 0; i < 1000; i++) r.add(6'400, true, false, block_us);
    CHECK(std::fabs(r.rate(block_us) - 1.0) < 0.002);

    // A game that needs 5.8 ms per block (cellAudio waits for each): ~0.92.
    for (int i = 0; i < 1500; i++) r.add(5'800, true, true, block_us);
    CHECK(std::fabs(r.rate(block_us) - 5'333.0 / 5'800.0) < 0.01);

    // Refill: short periods the game kept up with count as they are (above 1.00).
    ap::rate_tracker refill;
    for (int i = 0; i < 600; i++) refill.add(2'700, true, false, block_us);
    CHECK(refill.rate(block_us) > 1.9);

    // Lost blocks (skips) lower the rate: one in four periods lost -> ~0.75.
    ap::rate_tracker lossy;
    for (int i = 0; i < 2000; i++) lossy.add(block_us, i % 4 != 0, i % 4 == 0, block_us);
    CHECK(std::fabs(lossy.rate(block_us) - 0.75) < 0.02);

    // A pause or restart period is ignored.
    ap::rate_tracker paused = r;
    paused.add(5'000'000, false, true, block_us);
    CHECK(paused.blocks == r.blocks && paused.time_us == r.time_us);

    // The window is ~2 s: 4 s after the game sped up, the old rate is gone.
    for (int i = 0; i < 750; i++) r.add(block_us, true, false, block_us);
    CHECK(r.rate(block_us) > 0.985);
}

void test_tempo()
{
    CHECK(ap::tempo_target(1.0, 1.0) == 1.0);
    CHECK(std::fabs(ap::tempo_target(0.9, 1.0) - 0.9) < 1e-9);
    CHECK(ap::tempo_target(0.5, 1.0) == ap::tempo_min);
    CHECK(ap::tempo_target(0.0, 0.0) == ap::tempo_min);
    CHECK(std::fabs(ap::tempo_target(1.2, 0.6) - 0.96) < 1e-9);
    CHECK(ap::tempo_target(0.8, 2.0) == 1.0);
    CHECK(ap::tempo_target(5.0, 9.0) == 1.0);
    // Below the target fill the tempo is lower than the rate (the ring refills).
    CHECK(ap::tempo_target(0.92, 0.5) < 0.92);

    // Slew: 0.004 per period, never past the bounds.
    CHECK(std::fabs(ap::tempo_step(1.0, 0.7) - 0.996) < 1e-9);
    CHECK(std::fabs(ap::tempo_step(0.8, 1.0) - 0.804) < 1e-9);
    CHECK(ap::tempo_step(0.701, 0.0) == ap::tempo_min);
    CHECK(ap::tempo_step(0.999, 2.0) == 1.0);
    double t = 1.0;
    int steps = 0;
    while (t > ap::tempo_min && steps < 1000)
    {
        t = ap::tempo_step(t, 0.0);
        ++steps;
    }
    CHECK(steps == 75);

    CHECK(!ap::tempo_should_apply(0.997, 1.0));
    CHECK(ap::tempo_should_apply(0.994, 1.0));
    CHECK(ap::tempo_should_apply(1.0, 0.997));
    CHECK(!ap::tempo_should_apply(1.0, 1.0));
    CHECK(!ap::tempo_should_apply(0.9, 0.9));
}

void test_trigger()
{
    // Slow game: engages after 1 s of slow periods, not before.
    ap::continuity_trigger c;
    bool engaged = false;
    std::uint64_t at = 0;
    for (std::uint64_t t = 0; t < 3'000'000 && !engaged; t += 5'800)
    {
        engaged = c.add(5'800, true, 0.92, 0.8);
        at = t + 5'800;
    }
    CHECK(engaged);
    CHECK(at >= 1'000'000 && at <= 1'010'000);

    // Low fill alone (rate not measured yet) engages too.
    ap::continuity_trigger low;
    bool e2 = false;
    for (int i = 0; i < 200 && !e2; i++) e2 = low.add(block_us, false, 1.0, 0.3);
    CHECK(e2);

    // A clean game never: fill and rate fine.
    ap::continuity_trigger clean;
    bool e3 = false;
    for (int i = 0; i < 20'000; i++) e3 = e3 || clean.add(block_us, true, 0.995, 0.95);
    CHECK(!e3);

    // One good period restarts the count.
    ap::continuity_trigger broken;
    bool e4 = false;
    for (int i = 0; i < 2'000; i++) e4 = e4 || broken.add(block_us, true, i % 150 == 149 ? 1.0 : 0.9, 1.0);
    CHECK(!e4);

    // A pause period neither counts nor restarts.
    ap::continuity_trigger paused;
    for (int i = 0; i < 100; i++) paused.add(block_us, true, 0.9, 1.0);
    const std::uint64_t held = paused.held_us;
    CHECK(!paused.add(10'000'000, true, 0.9, 1.0));
    CHECK(paused.held_us == held);
}

// Closed loop: cellAudio periods, a game that writes one block per mix event, the output ring drained in real time,
// the rate tracker, the trigger and the tempo. Mirrors the cellAudio.cpp use of these functions.
struct loop_result
{
    double blocks_per_s = 0;
    double underrun_ms = 0; // after the first 5 s
    double tempo_avg = 0;   // after the first 5 s
    double tempo_min = 1;
    double engage_s = -1;
};

loop_result run_loop(double game_us, double jitter_us, double seconds, bool engaged, std::uint32_t seed)
{
    std::mt19937 rng(seed);
    std::normal_distribution<double> jit(0.0, jitter_us);

    const double desired = 64'000;
    const double desired_adj = desired + block_us / 2.0;
    const double min_period = block_us / 2.0;
    const double max_period = 6.0 * block_us / 5.0;

    double now = 0;
    double period_end = 0;
    double ring = desired + 3 * block_us; // restart: desired_full_buffers of silence
    double resampler_in = 0;              // input waiting in the time stretcher (us of input audio)
    double tempo = 1.0;
    double avg_play = ring;
    double ready = game_us;
    double tempo_sum = 0;
    std::uint64_t measured_periods = 0;
    std::uint64_t blocks = 0;
    double underrun = 0;
    ap::rate_tracker rate;
    ap::continuity_trigger trigger;
    loop_result out;

    while (now < seconds * 1e6)
    {
        const double held = ring + (engaged ? resampler_in / tempo : 0.0);
        const double fill = held / desired_adj;
        const double dynamic = fill >= 1.0 ? max_period - (max_period - block_us) / fill : min_period + (block_us - min_period) * fill * fill;
        now = std::max(now, period_end + dynamic);

        bool waited = false;
        for (;;)
        {
            const bool written = ready <= now;
            const double left = std::max(0.0, held - (now - period_end));
            const auto d = ap::decide(ports(1, written ? 0 : 1), static_cast<std::uint64_t>(now - period_end), static_cast<std::uint64_t>(left), lim, false);
            if (d.what != ap::action::wait_untouched)
            {
                break;
            }
            waited = true;
            now += 250;
        }

        const double period_us = now - period_end;
        if (engaged)
        {
            ring += resampler_in / tempo;
            resampler_in = 0;
        }
        if (ring >= period_us)
        {
            ring -= period_us;
        }
        else
        {
            if (now > 5e6) underrun += period_us - ring;
            ring = 0;
        }

        const bool delivered = ready <= now;
        if (delivered)
        {
            ++blocks;
            if (engaged) resampler_in += block_us;
            else ring = std::min(ring + block_us, desired + 20'000);
        }

        rate.add(static_cast<std::uint64_t>(period_us), delivered, waited, block_us);
        avg_play = 0.02 * (ring + (engaged ? resampler_in / tempo : 0.0)) + 0.98 * avg_play;

        if (!engaged)
        {
            if (trigger.add(static_cast<std::uint64_t>(period_us), rate.valid(), rate.rate(block_us), avg_play / desired_adj))
            {
                engaged = true;
                out.engage_s = now / 1e6;
            }
        }
        else
        {
            const double f = (ring + resampler_in / tempo) / desired_adj;
            tempo = ap::tempo_step(tempo, ap::tempo_target(rate.rate(block_us), f));
        }

        out.tempo_min = std::min(out.tempo_min, tempo);
        if (now > 5e6)
        {
            tempo_sum += tempo;
            ++measured_periods;
        }
        period_end = now;

        if (delivered)
        {
            ready = std::max(ready, now) + std::max(500.0, game_us + jit(rng));
        }
    }

    out.blocks_per_s = static_cast<double>(blocks) / seconds;
    out.underrun_ms = underrun / 1000.0;
    out.tempo_avg = measured_periods ? tempo_sum / static_cast<double>(measured_periods) : 1.0;
    return out;
}

void test_closed_loop()
{
    // A game faster than real time: never engages, tempo 1.00, no underrun.
    {
        const auto r = run_loop(4'800, 300, 60, false, 1);
        CHECK(r.engage_s < 0);
        CHECK(r.tempo_min == 1.0);
        CHECK(r.underrun_ms < 1.0);
        CHECK(r.blocks_per_s > 187.0);
    }

    // Engaged from the start (player's Time Stretching) with a fast game: stays at 1.00 (within the dead band).
    {
        const auto r = run_loop(4'800, 300, 60, true, 2);
        CHECK(r.tempo_avg > 0.99);
        CHECK(r.underrun_ms < 1.0);
    }

    // Max Payne 3 class (game-paced at ~92 %): engages within a few seconds, then plays at ~0.90 without underruns.
    {
        const auto r = run_loop(5'333.0 / 0.92, 300, 60, false, 3);
        CHECK(r.engage_s > 0 && r.engage_s < 4);
        CHECK(r.tempo_avg > 0.85 && r.tempo_avg < 0.97);
        CHECK(r.tempo_min >= ap::tempo_min);
        CHECK(r.underrun_ms < 50.0);
    }

    // 66 % (Dragon Ball RB2 class): tempo held at the 0.70 floor, never below; the rest stays underruns.
    {
        const auto r = run_loop(5'333.0 / 0.66, 600, 60, false, 4);
        CHECK(r.engage_s > 0);
        CHECK(r.tempo_min >= ap::tempo_min);
        CHECK(r.tempo_avg < 0.75);
    }

    // Under 50 % (GTA IV CE gameplay): floor 0.70, never the old 0.1x.
    {
        const auto r = run_loop(5'333.0 / 0.45, 900, 60, false, 5);
        CHECK(r.engage_s > 0);
        CHECK(r.tempo_min >= ap::tempo_min);
    }
}

// Boot rules from the switches: every switch off is the 0.38 cellAudio (tick waits, 100/100 us polls or 1000/500 with
// audio-poll-2ms.flag, upstream checks and controller), whatever the HR timer probe said.
void test_boot_rules()
{
    for (const bool timer : {false, true})
    {
        const ap::boot_rules off = ap::rules_for(false, false, timer, false, false);
        CHECK(!off.hr && off.untouched_poll_us == 100 && off.in_progress_poll_us == 100);
        CHECK(!off.policy && !off.continuity);

        const ap::boot_rules off_2ms = ap::rules_for(false, true, timer, false, false);
        CHECK(!off_2ms.hr && off_2ms.untouched_poll_us == 1000 && off_2ms.in_progress_poll_us == 500);
    }

    // HR waits on: 250/500 us on the HR timer; tick polls when the probe failed; audio-poll-2ms.flag still wins.
    const ap::boot_rules on = ap::rules_for(true, false, true, false, false);
    CHECK(on.hr && on.untouched_poll_us == 250 && on.in_progress_poll_us == 500);
    const ap::boot_rules no_timer = ap::rules_for(true, false, false, false, false);
    CHECK(!no_timer.hr && no_timer.untouched_poll_us == 100 && no_timer.in_progress_poll_us == 100);
    const ap::boot_rules killed = ap::rules_for(true, true, true, false, false);
    CHECK(!killed.hr && killed.untouched_poll_us == 1000 && killed.in_progress_poll_us == 500);

    // The policy and continuity switches pass through on their own.
    const ap::boot_rules policy = ap::rules_for(false, false, true, true, false);
    CHECK(policy.policy && !policy.continuity && !policy.hr && policy.untouched_poll_us == 100);
    const ap::boot_rules cont = ap::rules_for(false, false, true, false, true);
    CHECK(!cont.policy && cont.continuity && !cont.hr);

    static_assert(!ap::rules_for(false, false, true, false, false).hr, "the HR waits are opt-in");
}

// Unbuffered cellAudio period clock: rebased when more than 8 periods behind (boot wait, pause, stall), never for a
// late period.
void test_unbuffered_rebase()
{
    constexpr std::uint64_t start = 54'600'000; // the clock starts before the boot wait
    CHECK(ap::unbuffered_behind_us(0, start, start, block_us) == 0);
    CHECK(ap::unbuffered_behind_us(10, start, start, block_us) == 0); // ahead of time: not behind
    CHECK(ap::unbuffered_behind_us(10, start, start + 10 * block_us + 7, block_us) == 7);

    // A 61.4 s boot wait (0.39: 13,378 periods with 11,501 drops in the first window) rebases
    CHECK(ap::unbuffered_rebase_due(0, start, start + 61'400'000, block_us));
    // A 7 s PS-button pause rebases
    CHECK(ap::unbuffered_rebase_due(5'000, start, start + 5'000 * block_us + 7'000'000, block_us));
    // 2 periods behind (a late wake-up) does not; exactly 8 does not; 8 and 1 us does
    CHECK(!ap::unbuffered_rebase_due(5'000, start, start + 5'000 * block_us + 2 * block_us, block_us));
    CHECK(!ap::unbuffered_rebase_due(8 * block_us, block_us));
    CHECK(ap::unbuffered_rebase_due(8 * block_us + 1, block_us));
    CHECK(!ap::unbuffered_rebase_due(0, block_us));

    // After the rebase the next period is due now (behind 0, not ahead)
    const std::uint64_t now = start + 61'400'000;
    const std::uint64_t rebased = ap::unbuffered_rebased_start(1234, now, block_us);
    CHECK(ap::unbuffered_behind_us(1234, rebased, now, block_us) == 0);
    CHECK(1234 * block_us + rebased == now);
    CHECK(ap::unbuffered_rebased_start(5, 1'000, block_us) == 0); // never below zero
    static_assert(ap::unbuffered_rebase_periods == 8);

    // The unbuffered loop of cellAudio after a 61.4 s wait: periods fired within the next 100 ms
    const auto burst = [](bool rebase)
    {
        std::uint64_t counter = 0, s = start, t = start + 61'400'000;
        const std::uint64_t until = t + 100'000;
        std::uint32_t fired = 0;
        while (t < until)
        {
            if (rebase && ap::unbuffered_rebase_due(counter, s, t, block_us))
            {
                s = ap::unbuffered_rebased_start(counter, t, block_us);
            }
            const std::uint64_t period_end = counter * block_us + s;
            const std::int64_t time_left = static_cast<std::int64_t>(period_end - t);
            if (time_left > 100)
            {
                t += static_cast<std::uint64_t>(time_left); // the period wait
                continue;
            }
            fired++;       // mix and send the MIX event
            counter++;
            t += 20;       // the mix itself
        }
        return fired;
    };
    CHECK(burst(false) > 4'000); // upstream: thousands of MIX events back to back
    const std::uint32_t paced = burst(true);
    CHECK(paced >= 18 && paced <= 20); // ~100 ms / 5.33 ms
}

void test_out_tail()
{
    char out[64];
    CHECK(ap::format_out_tail(out, sizeof(out), 0) == std::string_view(" fill_silent=0 assist=off").size());
    CHECK(std::string_view(out) == " fill_silent=0 assist=off");
    ap::format_out_tail(out, sizeof(out), 1234);
    CHECK(std::string_view(out) == " fill_silent=1234 assist=off");
    ap::format_out_tail(out, sizeof(out), 5, 0, true);
    CHECK(std::string_view(out) == " fill_silent=5 assist=watch");
    ap::format_out_tail(out, sizeof(out), 5, 1, true);
    CHECK(std::string_view(out) == " fill_silent=5 assist=wait");
    ap::format_out_tail(out, sizeof(out), 5, 2, false);
    CHECK(std::string_view(out) == " fill_silent=5 assist=cont");
    ap::format_out_tail(out, sizeof(out), UINT64_MAX);
    CHECK(std::string_view(out) == " fill_silent=18446744073709551615 assist=off");

    // Truncated to fit and always terminated
    char small[8];
    CHECK(ap::format_out_tail(small, sizeof(small), 99) == 7);
    CHECK(std::string_view(small) == " fill_s");
    char one[1] = {'x'};
    CHECK(ap::format_out_tail(one, 1, 99) == 0 && one[0] == '\0');
    CHECK(ap::format_out_tail(nullptr, 10, 99) == 0);
}
}

int main()
{
    test_boot_rules();
    test_decide();
    test_late_bucket();
    test_port_stops();
    test_rate_tracker();
    test_tempo();
    test_trigger();
    test_closed_loop();
    test_unbuffered_rebase();
    test_out_tail();

    if (g_failures)
    {
        std::printf("FAIL: %d of %d checks\n", g_failures, g_checks);
        return 1;
    }

    std::printf("PASS: %d checks\n", g_checks);
    return 0;
}
