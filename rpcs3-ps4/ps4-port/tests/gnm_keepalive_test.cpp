// Host test of utils::ps4::health::gnm_keepalive_policy (util/ps4_health.hpp): when the GNM keepalive thread calls
// sceGnmSubmitDone. Build: clang++ -std=c++20 -static -I rpcs3 -I . ps4-port/tests/gnm_keepalive_test.cpp
#include "util/ps4_health.hpp"

#include <cassert>
#include <cstdio>
#include <vector>

using utils::ps4::health::gnm_call;
using utils::ps4::health::gnm_keepalive_policy;

namespace
{
constexpr u64 tick = 100'000;  // one pass of the thread
constexpr u64 start = 5'000'000;

// Runs passes from `from` (exclusive) to `to` (inclusive) with a fixed sequence and allowed value; every requested
// call returns at once. Returns the times of the calls and their kinds.
struct run_result
{
    std::vector<u64> at;
    std::vector<gnm_call> kind;
};

run_result run(gnm_keepalive_policy& p, u64 from, u64 to, u64 seq, s32 allowed)
{
    run_result r;
    for (u64 now = from + tick; now <= to; now += tick)
    {
        const gnm_call c = p.step(now, seq, allowed);
        if (c == gnm_call::none) continue;
        r.at.push_back(now);
        r.kind.push_back(c);
        p.called(seq, now);
    }
    return r;
}

void test_no_submission_no_call()
{
    gnm_keepalive_policy p(start);
    // XMB only: sequence 0 for a minute, never a call, even when 'allowed' reads 0
    for (u64 now = start + tick; now <= start + 60'000'000; now += tick)
    {
        assert(p.step(now, 0, 1) == gnm_call::none);
        assert(p.step(now, 0, 0) == gnm_call::none);
    }
    assert(!p.pending_off);
}

void test_rendering_never_calls()
{
    gnm_keepalive_policy p(start);
    u64 seq = 0;
    // A new submission on every pass (rendering): no call
    for (u64 now = start + tick; now <= start + 30'000'000; now += tick)
        assert(p.step(now, ++seq, 1) == gnm_call::none);
    // Submissions every 0.4 s: still never idle for 0.5 s
    for (u64 now = start + 30'000'000 + tick, n = 0; now <= start + 60'000'000; now += tick, n++)
        assert(p.step(now, n % 4 == 0 ? ++seq : seq, 1) == gnm_call::none);
}

void test_idle_cadence()
{
    gnm_keepalive_policy p(start);
    u64 now = start + tick;
    assert(p.step(now, 10, 1) == gnm_call::none); // first submissions seen
    // Idle from here: the first call 0.5 s after the last change, then once per second
    const run_result r = run(p, now, now + 5'000'000, 10, 1);
    assert(!r.at.empty());
    assert(r.at[0] == now + 500'000);
    for (usz i = 0; i < r.at.size(); i++) assert(r.kind[i] == gnm_call::idle);
    for (usz i = 1; i < r.at.size(); i++) assert(r.at[i] - r.at[i - 1] == 1'000'000);
    assert(r.at.size() == 5); // 0.5, 1.5, 2.5, 3.5, 4.5 s
}

void test_burst_after_idle_call()
{
    gnm_keepalive_policy p(start);
    u64 now = start + tick;
    p.step(now, 10, 1);
    run_result r = run(p, now, now + 600'000, 10, 1);
    assert(r.at.size() == 1);
    // A short burst of submissions (one frame after an overlay flip), then idle again: the next call comes 0.5 s after
    // the burst, before the 1 s repeat, because the sequence moved since the last call
    now += 700'000;
    assert(p.step(now, 11, 1) == gnm_call::none);
    r = run(p, now, now + 600'000, 11, 1);
    assert(r.at.size() == 1 && r.at[0] == now + 500'000);
}

void test_pending_trigger()
{
    gnm_keepalive_policy p(start);
    u64 now = start + tick;
    u64 seq = 1;
    // Rendering, then a suspend becomes pending while submissions still move: a call at once and every 250 ms
    for (; now < start + 2'000'000; now += tick) assert(p.step(now, ++seq, 1) == gnm_call::none);
    std::vector<u64> calls;
    for (u64 end = now + 2'000'000; now <= end; now += tick)
    {
        const gnm_call c = p.step(now, ++seq, 0);
        if (c == gnm_call::none) continue;
        assert(c == gnm_call::pending);
        calls.push_back(now);
        p.called(seq, now);
    }
    assert(calls.size() >= 6);
    for (usz i = 1; i < calls.size(); i++) assert(calls[i] - calls[i - 1] == 300'000); // first pass at or after 250 ms
    // Allowed again: back to the idle rule (nothing while submissions move)
    for (u64 end = now + 1'000'000; now <= end; now += tick) assert(p.step(now, ++seq, 1) == gnm_call::none);
    assert(!p.pending_off);
}

void test_pending_gives_up()
{
    gnm_keepalive_policy p(start);
    u64 now = start + tick;
    u64 seq = 1;
    bool gave_up = false;
    u64 gave_up_at = 0;
    u32 pending_calls = 0;
    // Submits not allowed for good while rendering goes on: pending calls for 30 s, then the trigger turns off once
    for (u64 end = now + 40'000'000; now <= end; now += tick)
    {
        const gnm_call c = p.step(now, ++seq, 0);
        if (p.gave_up_now)
        {
            assert(!gave_up);
            gave_up = true;
            gave_up_at = now;
        }
        if (c == gnm_call::pending)
        {
            assert(!gave_up);
            pending_calls++;
            p.called(seq, now);
        }
        else
        {
            assert(c == gnm_call::none);
        }
    }
    assert(gave_up && p.pending_off);
    assert(gave_up_at == start + tick + 30'000'000);
    assert(pending_calls >= 90 && pending_calls <= 101);
    // The idle trigger still works with 'allowed' stuck at 0: 0.5 s after the last new submission (the last pass above)
    const u64 last_change = now - tick;
    const run_result r = run(p, now, now + 2'000'000, seq, 0);
    assert(!r.at.empty() && r.kind[0] == gnm_call::idle && r.at[0] == last_change + 500'000);
}

void test_pending_while_idle_does_not_double()
{
    gnm_keepalive_policy p(start);
    u64 now = start + tick;
    p.step(now, 7, 1);
    run_result r = run(p, now, now + 600'000, 7, 1); // idle call at +0.5 s
    assert(r.at.size() == 1);
    now += 600'000;
    // Now a suspend becomes pending (no new submission): calls every 250 ms (passes of 300 ms), all 'pending'
    r = run(p, now, now + 3'000'000, 7, 0);
    assert(r.at.size() == 10);
    for (const gnm_call c : r.kind) assert(c == gnm_call::pending);
}
}

int main()
{
    test_no_submission_no_call();
    test_rendering_never_calls();
    test_idle_cadence();
    test_burst_after_idle_call();
    test_pending_trigger();
    test_pending_gives_up();
    test_pending_while_idle_does_not_double();
    std::puts("gnm_keepalive_test: all passed");
    return 0;
}
