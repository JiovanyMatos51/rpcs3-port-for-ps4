#pragma once

#include <atomic>
#include <cstdint>
#include <limits>

namespace utils::ps4
{
// Native auto-reset event for a single atomic-wait sleeper. Notifications
// remain latched if they arrive between checking the value and sleeping.
// API is injected so races and timeout conversion can be tested on the host.
//
// One event per cond slot, created lazily and kept for the lifetime of the
// slot table. Protocol used by rpcs3/util/atomic.cpp:
//  - prepare(): owner thread only, while its registration is unpublished.
//    Creates the kernel event once; afterwards drains a latched notification
//    only if some successful set was not consumed (no syscall otherwise).
//  - notify(): a notifier holding a reference to the registration. It counts
//    a successful set after the set and before it drops its reference, so the
//    next owner's prepare() observes the count.
//  - wait(): owner thread only. A successful CLEAR_PAT wait consumes one set.
// 0.29: the drain decision compares 'sets' with 'consumed' instead of a latched
// flag, which made every notified sleep cost a useless zero-timeout drain
// (3 syscalls instead of 2). No wake-up is lost: every clear is a successful
// wait (+1 consumed) or a drain (consumed = all sets), so a set event at
// prepare() implies sets > consumed. Coalesced sets only cause one extra drain.
// 0.29: the counters are 16-bit so the event stays 16 bytes and atomic.cpp's
// cond_handle stays 64 bytes (one cache line; s_cond_list 4 MiB instead of 8).
// Only equality is tested and the real gap is a handful of sets, so wrap-around
// is exact; a false 'equal' only skips a drain (one spurious wake-up).
// Nothing here throws: kernel errors are returned to the caller, which degrades
// (atomic.cpp: rate-limited report, bounded polling sleep). A stale latched
// notification only causes a spurious wake-up; the wait loop re-checks the value.
template <class Api>
class atomic_event
{
    typename Api::handle m_handle{};
    bool m_valid = false;
    // Successful Api::set calls (notifiers), modulo 2^16.
    std::atomic<std::uint16_t> m_sets{0};
    // Sets consumed by an owner wait or a drain, modulo 2^16. Written only by
    // the owner of the moment (cond slots change owner; atomic for that hand-over).
    std::atomic<std::uint16_t> m_consumed{0};

public:
    static constexpr std::uint64_t infinite = std::numeric_limits<std::uint64_t>::max();
    static constexpr unsigned notify_attempts = 3;

    atomic_event() = default;
    atomic_event(const atomic_event&) = delete;
    atomic_event& operator=(const atomic_event&) = delete;

    bool valid() const noexcept { return m_valid; }

    // True if some successful set was not consumed by an owner wait or a drain
    // (diagnostics/tests). The event itself may already be clear (coalesced sets).
    bool maybe_latched() const noexcept
    {
        return m_sets.load(std::memory_order_relaxed) != m_consumed.load(std::memory_order_relaxed);
    }

    // Create the kernel event once. Returns 0 if it already exists.
    int ensure(const char* name) noexcept
    {
        // 0.29: keep atomic.cpp's cond_handle at 64 bytes (see the counters above).
        static_assert(sizeof(typename Api::handle) > sizeof(void*) || sizeof(atomic_event) <= 16,
            "atomic_event grew past 16 bytes: cond_handle would no longer fit one cache line");
        if (m_valid) return 0;
        typename Api::handle handle{};
        if (const int error = Api::create(&handle, name)) return error;
        m_handle = handle;
        m_sets.store(0, std::memory_order_relaxed);
        m_consumed.store(0, std::memory_order_relaxed);
        m_valid = true;
        return 0;
    }

    // Owner, before publishing its registration (see the protocol above).
    int prepare(const char* name) noexcept
    {
        if (!m_valid) return ensure(name);
        const std::uint16_t sets = m_sets.load(std::memory_order_acquire);
        // Every set was taken by an owner wait: nothing can be latched, no syscall.
        if (sets == m_consumed.load(std::memory_order_relaxed)) return 0;
        const int result = reset();
        // Drained (or already clear): every set so far is accounted for, even if
        // the drain failed (as before: the waiter tolerates one spurious wake-up).
        m_consumed.store(sets, std::memory_order_relaxed);
        return result;
    }

    // Teardown and tests only: the slot table never deletes its events.
    int destroy() noexcept
    {
        if (!m_valid) return 0;
        const auto handle = m_handle;
        m_handle = {};
        m_valid = false;
        m_sets.store(0, std::memory_order_relaxed);
        m_consumed.store(0, std::memory_order_relaxed);
        return Api::destroy(handle);
    }

    // Set the event, retrying a failed set a few times. Without a kernel event
    // (creation failed) this is a no-op: that waiter polls with bounded sleeps.
    int notify() noexcept
    {
        if (!m_valid) return 0;
        int error = 0;
        for (unsigned attempt = 0; attempt < notify_attempts; ++attempt)
        {
            error = Api::set(m_handle);
            if (!error)
            {
                m_sets.fetch_add(1, std::memory_order_release);
                return 0;
            }
        }
        return error;
    }

    // Drain a latched notification (zero timeout). Only while the owner's
    // registration is unpublished and no notifier holds a reference.
    int reset() noexcept
    {
        if (!m_valid) return 0;
        std::uint32_t immediate = 0;
        const int result = Api::wait(m_handle, &immediate);
        return result == Api::timeout ? 0 : result;
    }

    int wait(std::uint64_t nanoseconds) noexcept
    {
        int result;
        if (nanoseconds == infinite)
        {
            result = Api::wait(m_handle, nullptr);
        }
        else
        {
            // Ceil without overflow; a positive sub-microsecond wait must sleep.
            const auto micros = nanoseconds / 1000 + (nanoseconds % 1000 != 0);
            std::uint32_t duration = static_cast<std::uint32_t>(
                micros > std::numeric_limits<std::uint32_t>::max()
                    ? std::numeric_limits<std::uint32_t>::max() : micros);
            result = Api::wait(m_handle, &duration);
        }
        // A successful CLEAR_PAT wait cleared the event: it consumed (at least) one set.
        if (result == 0)
            m_consumed.store(static_cast<std::uint16_t>(m_consumed.load(std::memory_order_relaxed) + 1), std::memory_order_relaxed);
        return result;
    }
};
}
