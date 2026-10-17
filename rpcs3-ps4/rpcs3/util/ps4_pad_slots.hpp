#pragma once

// 0.31: seat rules for the PS4 pads (rpcs3/ps4_pads.cpp). RPCS3 players 1-4 are "seats" that map to logged-in PS4
// users. This header is pure logic without SCE headers, so the rules can be checked on a PC
// (ps4-port/tests/pad_slots_test.cpp).

#include <array>
#include <cstddef>
#include <cstdint>

namespace utils::ps4
{
inline constexpr std::size_t pad_players = 4;     // the PS4 allows at most 4 logged-in users
inline constexpr std::int32_t no_user = -1;       // ORBIS_USER_SERVICE_USER_ID_INVALID
inline constexpr std::int32_t system_user = 0xff; // ORBIS_USER_SERVICE_USER_ID_SYSTEM: never owns a controller

using pad_slots = std::array<std::int32_t, pad_players>;

constexpr bool valid_pad_user(std::int32_t user) noexcept
{
    return user != no_user && user != system_user;
}

constexpr pad_slots empty_pad_slots() noexcept
{
    return {no_user, no_user, no_user, no_user};
}

constexpr bool contains_pad_user(const pad_slots& list, std::int32_t user) noexcept
{
    for (const std::int32_t entry : list)
    {
        if (entry == user) return true;
    }
    return false;
}

constexpr std::size_t count_pad_users(const pad_slots& list) noexcept
{
    std::size_t n = 0;
    for (const std::int32_t entry : list) n += valid_pad_user(entry) ? 1 : 0;
    return n;
}

// Computes the next seat mapping.
//   prev:      current seats (no_user = free seat)
//   login:     the system login list, any order, holes allowed
//   initial:   the PS4 user who started the application
//   connected: connected(user) -> true when that user's controller reports connected (only used when compact)
//   compact:   true only while no game has seen the seats (XMB, game boot). Users whose controller is connected
//              come first, the initial user first among them, then the other logged-in users. Ties keep the
//              previous seat order, then the login order.
//              false in game: a seated user never moves, a user who logged out leaves a free seat and a new user
//              takes the lowest free seat (the initial user takes seat 0 when it is free).
template <typename Connected>
constexpr pad_slots assign_pad_slots(const pad_slots& prev, const pad_slots& login, std::int32_t initial,
    Connected&& connected, bool compact)
{
    const auto logged = [&](std::int32_t user) { return valid_pad_user(user) && contains_pad_user(login, user); };
    pad_slots next = empty_pad_slots();
    const auto seated = [&](std::int32_t user) { return contains_pad_user(next, user); };

    if (compact)
    {
        std::size_t n = 0;
        for (int pass = 0; pass < 2; ++pass)
        {
            const auto take = [&](std::int32_t user)
            {
                if (n < pad_players && logged(user) && !seated(user) && (pass == 1 || connected(user)))
                    next[n++] = user;
            };
            take(initial);
            for (const std::int32_t user : prev) take(user);
            for (const std::int32_t user : login) take(user);
        }
        return next;
    }

    for (std::size_t i = 0; i < pad_players; ++i)
    {
        if (logged(prev[i]) && !seated(prev[i])) next[i] = prev[i];
    }
    if (logged(initial) && !seated(initial) && next[0] == no_user) next[0] = initial;
    for (const std::int32_t user : login)
    {
        if (!logged(user) || seated(user)) continue;
        for (std::int32_t& seat : next)
        {
            if (seat == no_user)
            {
                seat = user;
                break;
            }
        }
    }
    return next;
}

// Same as assign_pad_slots, but a failed login-list query (list_ok = false) keeps the previous mapping. The seats are
// never cleared because a system call failed (for example right after resuming from rest mode).
template <typename Connected>
constexpr pad_slots update_pad_slots(const pad_slots& prev, bool list_ok, const pad_slots& login, std::int32_t initial,
    Connected&& connected, bool compact)
{
    if (!list_ok) return prev;
    return assign_pad_slots(prev, login, initial, connected, compact);
}

// Idle-handle release for seats whose controller stays off during a game. scePad handles stay open across games, so
// a logged-in user whose controller is off would otherwise keep a handle (and whatever the pad library allocates for
// it) for the whole session. After release_after_us the handle is closed; while released it is reopened every
// probe_every_us for probe_window_us so a controller that comes back is noticed. The seat itself never changes.
struct pad_idle_timing
{
    std::uint64_t release_after_us = 10'000'000;
    std::uint64_t probe_every_us = 5'000'000;
    std::uint64_t probe_window_us = 2'000'000;
};

enum class pad_idle_action
{
    none,
    release,     // close the handle (controller off too long)
    reopen,      // open the handle again (probe, or releasing is no longer allowed)
    close_probe, // the probe window ended without a connected report: close quietly
};

struct pad_idle
{
    std::uint64_t off_since = 0;   // first poll that saw the controller off (0 = on or unknown)
    std::uint64_t next_probe = 0;  // released: time of the next reopen
    std::uint64_t probe_until = 0; // released and reopened: end of the probe window
    bool released = false;

    constexpr void reset() noexcept
    {
        off_since = 0;
        next_probe = 0;
        probe_until = 0;
        released = false;
    }

    // Called for every successful read of an open handle.
    constexpr void observe(bool connected, std::uint64_t now) noexcept
    {
        if (connected)
        {
            reset();
        }
        else if (!off_since)
        {
            off_since = now ? now : 1;
        }
    }

    // Called periodically. open: the handle is open now. may_release: a game runs, the seat is not player 1 and
    // releasing is not disabled.
    constexpr pad_idle_action step(std::uint64_t now, bool open, bool may_release, const pad_idle_timing& t = {}) noexcept
    {
        if (!may_release)
        {
            if (!released) return pad_idle_action::none;
            reset();
            return open ? pad_idle_action::none : pad_idle_action::reopen;
        }
        if (released)
        {
            if (!open)
            {
                if (now < next_probe) return pad_idle_action::none;
                probe_until = now + t.probe_window_us;
                return pad_idle_action::reopen;
            }
            if (!probe_until) probe_until = now + t.probe_window_us; // opened by someone else: time it as a probe
            if (now >= probe_until)
            {
                probe_until = 0;
                next_probe = now + t.probe_every_us;
                return pad_idle_action::close_probe;
            }
            return pad_idle_action::none;
        }
        if (open && off_since && now >= off_since && now - off_since >= t.release_after_us)
        {
            released = true;
            probe_until = 0;
            next_probe = now + t.probe_every_us;
            return pad_idle_action::release;
        }
        return pad_idle_action::none;
    }

    // While released, the handle may only be opened by step() (probe), not by an ordinary read.
    constexpr bool may_open() const noexcept
    {
        return !released || probe_until != 0;
    }
};
} // namespace utils::ps4
