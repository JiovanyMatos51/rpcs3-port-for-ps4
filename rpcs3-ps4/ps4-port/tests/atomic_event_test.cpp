#include "util/ps4_atomic_event.hpp"
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <iostream>
#include <mutex>
#include <thread>

// Host model of sceKernel*EventFlag with ATTR_SINGLE and WAITMODE_OR | CLEAR_PAT.
struct mock_kernel
{
    struct flag { std::mutex mutex; std::condition_variable ready; bool set = false; };
    using handle = flag*;
    static constexpr int timeout = 60;
    static inline std::atomic<unsigned> creates{0}, destroys{0}, sets{0}, waits{0};
    static int create(handle* event, const char*) { *event = new flag; creates++; return 0; }
    static int destroy(handle event) { delete event; destroys++; return 0; }
    static int set(handle event)
    {
        sets++;
        std::lock_guard lock(event->mutex);
        event->set = true;
        event->ready.notify_one();
        return 0;
    }
    static int wait(handle event, std::uint32_t* duration)
    {
        waits++;
        std::unique_lock lock(event->mutex);
        if (duration)
        {
            if (!event->ready.wait_for(lock, std::chrono::microseconds(*duration), [&] { return event->set; }))
                return timeout;
        }
        else event->ready.wait(lock, [&] { return event->set; });
        event->set = false; // OR | CLEAR_PAT, not a counting semaphore.
        return 0;
    }
};

struct argument_recorder
{
    using handle = unsigned;
    static constexpr int timeout = 60;
    static inline std::uint32_t duration{};
    static inline bool infinite{};
    static inline int error{};
    static inline int create_error{};
    static inline unsigned set_calls{};
    static inline unsigned wait_calls{};
    static int create(handle* event, const char*) { *event = 1; return create_error; }
    static int destroy(handle) { return error; }
    static int set(handle) { set_calls++; return error; }
    static int wait(handle, std::uint32_t* value)
    {
        wait_calls++;
        infinite = !value;
        if (value) duration = *value;
        return error;
    }
};

#define CHECK(cond) do { if (!(cond)) { std::cerr << "FAILED line " << __LINE__ << ": " #cond "\n"; std::exit(1); } } while (0)

// Model of the rpcs3/util/atomic.cpp cond protocol around one reusable slot:
// owner: prepare() (unpublished) -> sync=1 (publish) -> loop { value changed? sync>1? kernel wait }
//        -> sync=0 -> drop ref; the slot is reused only when no notifier holds a reference.
// notifier: take a ref only while the slot is owned (refs != 0) and sync != 0, CAS sync 1->2,
//           notify(), drop ref.
struct slot_model
{
    utils::ps4::atomic_event<mock_kernel> event;
    std::atomic<unsigned> sync{0};
    std::atomic<unsigned> refs{0};
};

static bool notifier_alert(slot_model& slot)
{
    // Like cond_id_lock(): never reference a free slot (refs == 0)
    unsigned refs = slot.refs.load();
    do
    {
        if (refs == 0) return false;
    }
    while (!slot.refs.compare_exchange_weak(refs, refs + 1));
    bool alerted = false;
    if (slot.sync.load() != 0)
    {
        unsigned expected = 1;
        if (slot.sync.compare_exchange_strong(expected, 2))
        {
            CHECK(slot.event.notify() == 0);
            alerted = true;
        }
    }
    slot.refs.fetch_sub(1);
    return alerted;
}

int main()
{
    // 1. Timeout conversion and error propagation (never throws: every member is noexcept).
    static_assert(noexcept(std::declval<utils::ps4::atomic_event<argument_recorder>&>().notify()));
    static_assert(noexcept(std::declval<utils::ps4::atomic_event<argument_recorder>&>().wait(0)));
    static_assert(noexcept(std::declval<utils::ps4::atomic_event<argument_recorder>&>().prepare("x")));
    {
        utils::ps4::atomic_event<argument_recorder> conversion;
        CHECK(!conversion.valid());
        CHECK(conversion.notify() == 0 && argument_recorder::set_calls == 0); // no event: no-op
        CHECK(conversion.ensure("arguments") == 0 && conversion.valid());
        CHECK(conversion.ensure("again") == 0);
        conversion.wait(1); CHECK(argument_recorder::duration == 1);
        conversion.wait(1000); CHECK(argument_recorder::duration == 1);
        conversion.wait(1001); CHECK(argument_recorder::duration == 2);
        conversion.wait(0); CHECK(argument_recorder::duration == 0);
        conversion.wait(conversion.infinite - 1);
        CHECK(argument_recorder::duration == UINT32_MAX);
        conversion.wait(conversion.infinite); CHECK(argument_recorder::infinite);

        argument_recorder::error = 22;
        CHECK(conversion.wait(1000) == 22 && conversion.reset() == 22);
        // A failed set is retried, reported to the caller and does not count as a set. (A fresh event: the recorder's
        // waits above "succeed" without any set, which the 0.29 consumed counter treats as consumptions.)
        utils::ps4::atomic_event<argument_recorder> unset;
        CHECK(unset.ensure("unset") == 0 && unset.valid());
        argument_recorder::set_calls = 0;
        CHECK(unset.notify() == 22);
        CHECK(argument_recorder::set_calls == unset.notify_attempts);
        CHECK(!unset.maybe_latched());
        argument_recorder::wait_calls = 0;
        CHECK(unset.prepare("x") == 0 && argument_recorder::wait_calls == 0); // nothing set: no drain syscall, no error
        CHECK(unset.destroy() == 22 && !unset.valid());
        CHECK(conversion.destroy() == 22 && !conversion.valid());
        argument_recorder::error = 0;

        // Creation failure: the event stays invalid and prepare() reports it; notify() stays a no-op.
        argument_recorder::create_error = 33;
        utils::ps4::atomic_event<argument_recorder> failed;
        CHECK(failed.prepare("fails") == 33 && !failed.valid());
        argument_recorder::set_calls = 0;
        CHECK(failed.notify() == 0 && argument_recorder::set_calls == 0);
        argument_recorder::create_error = 0;
        CHECK(failed.prepare("works") == 0 && failed.valid());
    }

    // 2. Latching, coalescing and the conditional drain.
    utils::ps4::atomic_event<mock_kernel> request, reply;
    CHECK(request.prepare("request") == 0 && reply.prepare("reply") == 0);
    CHECK(mock_kernel::creates == 2 && mock_kernel::waits == 0); // creation does not drain
    CHECK(request.prepare("request") == 0 && mock_kernel::waits == 0); // nothing set: no syscall
    // Notification before kernel wait must stay latched. Two notifications coalesce.
    CHECK(request.notify() == 0 && request.notify() == 0 && request.maybe_latched());
    CHECK(request.wait(0) == 0 && request.wait(0) == mock_kernel::timeout);
    // A reused registration drains a stale notification, but only when one was set.
    CHECK(request.notify() == 0);
    const unsigned waits_before = mock_kernel::waits;
    CHECK(request.prepare("request") == 0 && mock_kernel::waits == waits_before + 1);
    CHECK(!request.maybe_latched());
    CHECK(request.wait(1'000'000) == mock_kernel::timeout);
    CHECK(request.prepare("request") == 0 && mock_kernel::waits == waits_before + 2); // no second drain

    // 0.29: a set consumed by the owner's own wait leaves nothing to drain (0.28's latched flag drained here).
    CHECK(request.notify() == 0 && request.wait(0) == 0);
    const unsigned waits_consumed = mock_kernel::waits;
    CHECK(request.prepare("request") == 0 && mock_kernel::waits == waits_consumed);
    CHECK(!request.maybe_latched());
    // Coalesced sets (2 sets, 1 clear) cost at most one unneeded drain, never a missed one.
    CHECK(request.notify() == 0 && request.notify() == 0 && request.wait(0) == 0);
    CHECK(request.maybe_latched());
    const unsigned waits_coalesced = mock_kernel::waits;
    CHECK(request.prepare("request") == 0 && mock_kernel::waits == waits_coalesced + 1);
    CHECK(!request.maybe_latched() && request.wait(0) == mock_kernel::timeout);
    // A set after the owner's last wait is still drained.
    CHECK(request.notify() == 0 && request.wait(0) == 0 && request.notify() == 0);
    const unsigned waits_pending = mock_kernel::waits;
    CHECK(request.prepare("request") == 0 && mock_kernel::waits == waits_pending + 1);
    CHECK(request.wait(0) == mock_kernel::timeout);
    // N notified sleeps (prepare, set, wait) cost N sets + N waits: no drain syscalls (0.28: one per round).
    {
        const unsigned w0 = mock_kernel::waits, s0 = mock_kernel::sets;
        for (unsigned i = 0; i < 100; ++i)
        {
            CHECK(request.prepare("request") == 0);
            CHECK(request.notify() == 0);
            CHECK(request.wait(1'000'000'000) == 0);
        }
        CHECK(mock_kernel::waits - w0 == 100 && mock_kernel::sets - s0 == 100);
    }
    // 0.29: the 16-bit counters wrap (cond_handle must stay 64 bytes). Past the wrap a consumed set still
    // needs no drain and a set after the owner's last wait is still drained.
    static_assert(sizeof(utils::ps4::atomic_event<mock_kernel>) <= 16);
    {
        constexpr unsigned wrap_rounds = 70000;
        const unsigned w0 = mock_kernel::waits;
        for (unsigned i = 0; i < wrap_rounds; ++i)
        {
            CHECK(request.notify() == 0 && request.wait(0) == 0);
            CHECK(request.prepare("request") == 0);
        }
        CHECK(mock_kernel::waits - w0 == wrap_rounds && !request.maybe_latched());
        CHECK(request.notify() == 0);
        const unsigned w1 = mock_kernel::waits;
        CHECK(request.prepare("request") == 0 && mock_kernel::waits == w1 + 1);
        CHECK(request.wait(0) == mock_kernel::timeout);
    }

    // 3. 1000 request/reply exchanges on persistent events.
    constexpr unsigned rounds = 1000;
    {
        std::atomic<unsigned> payload{0};
        std::atomic<bool> failed{false};
        std::thread worker([&]
        {
            for (unsigned i = 1; i <= rounds; ++i)
            {
                if (request.wait(1'000'000'000) != 0 || payload.load() != i)
                {
                    failed = true;
                    reply.notify();
                    return;
                }
                if ((i & 7) == 0) std::this_thread::yield();
                reply.notify();
            }
        });
        for (unsigned i = 1; i <= rounds; ++i)
        {
            payload.store(i);
            request.notify();
            if (reply.wait(1'000'000'000) != 0 || failed) { failed = true; break; }
        }
        worker.join();
        CHECK(!failed);
    }

    // 4. Full slot protocol with reuse: a waiter re-registers the same slot every round (no
    //    create/delete), notifiers race with publication, timeouts and unpublication.
    //    A 2-second wait timing out while the value had changed would be a lost wake-up.
    constexpr unsigned protocol_rounds = 20000;
    slot_model slot;
    std::atomic<unsigned> value{0};
    std::atomic<unsigned> acked{0};
    std::atomic<unsigned> lost{0}, spurious{0}, drains{0};
    const unsigned creates_before = mock_kernel::creates;
    std::thread owner([&]
    {
        for (unsigned round = 0; round < protocol_rounds; ++round)
        {
            const unsigned old = round;
            while (slot.refs.load() != 0) std::this_thread::yield(); // slot free => reusable
            const unsigned w0 = mock_kernel::waits;
            CHECK(slot.event.prepare("slot") == 0);
            drains += mock_kernel::waits - w0;
            slot.refs.store(1);
            slot.sync.store(1); // publish
            while (value.load() == old)
            {
                if (slot.sync.load() > 1)
                {
                    unsigned two = 2;
                    if (!slot.sync.compare_exchange_strong(two, 1)) break;
                    continue;
                }
                // Mix infinite-like (long) and short timed waits, as atomic.cpp does
                const bool short_wait = (round % 5) == 0;
                const int r = slot.event.wait(short_wait ? 50'000 : 2'000'000'000);
                if (r == mock_kernel::timeout && !short_wait) lost++;
                else if (r == 0 && value.load() == old && slot.sync.load() == 1) spurious++;
            }
            slot.sync.store(0); // unpublish
            slot.refs.fetch_sub(1);
            acked.store(round + 1);
        }
    });
    std::thread extra_notifier([&]
    {
        // Notifies without changing the value: produces stale latches and spurious wake-ups.
        while (acked.load() < protocol_rounds)
        {
            notifier_alert(slot);
            std::this_thread::yield();
        }
    });
    for (unsigned round = 0; round < protocol_rounds; ++round)
    {
        value.store(round + 1);
        notifier_alert(slot);
        while (acked.load() != round + 1)
        {
            std::this_thread::yield();
        }
    }
    owner.join();
    extra_notifier.join();
    CHECK(lost == 0);
    CHECK(mock_kernel::creates == creates_before + 1); // one persistent event for the slot
    CHECK(drains <= protocol_rounds);
    CHECK(slot.event.destroy() == 0);
    CHECK(request.destroy() == 0 && reply.destroy() == 0);
    std::cout << "Native atomic event: conversion, errors without throwing, persistent event, conditional drain, "
              << "no drain after a consumed set, latched wakeup, "
              << rounds << " exchanges and " << protocol_rounds << " protocol rounds passed (drains=" << drains
              << ", spurious=" << spurious << ", lost=" << lost << ")\n";
}
