// Host test for the PS4 seat rules in rpcs3/util/ps4_pad_slots.hpp (players 1-4 mapped to logged-in PS4 users) and
// for the idle-handle release used while a game runs.
//
// Build and run (any C++20 compiler, from ps4-port/):
//   clang++ -std=c++20 -I../rpcs3 tests/pad_slots_test.cpp -o pad_slots_test && ./pad_slots_test

#undef NDEBUG
#include "util/ps4_pad_slots.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <initializer_list>

namespace
{
using utils::ps4::assign_pad_slots;
using utils::ps4::no_user;
using utils::ps4::pad_idle;
using utils::ps4::pad_idle_action;
using utils::ps4::pad_slots;
using utils::ps4::update_pad_slots;

constexpr std::int32_t A = 0x10000001;
constexpr std::int32_t B = 0x10000002;
constexpr std::int32_t C = 0x10000003;
constexpr std::int32_t D = 0x10000004;
constexpr std::int32_t E = 0x10000005;
constexpr std::int32_t system_user = 0xff;

constexpr pad_slots none = {no_user, no_user, no_user, no_user};

pad_slots slots(std::int32_t a, std::int32_t b = no_user, std::int32_t c = no_user, std::int32_t d = no_user)
{
    return {a, b, c, d};
}

struct connected_set
{
    std::initializer_list<std::int32_t> users;
    bool operator()(std::int32_t user) const
    {
        for (const std::int32_t entry : users)
        {
            if (entry == user) return true;
        }
        return false;
    }
};

const auto all_on = [](std::int32_t) { return true; };
const auto all_off = [](std::int32_t) { return false; };

// 1. A single initial user lands in seat 0, in the menu and in game.
void test_single_initial_user()
{
    assert(assign_pad_slots(none, slots(A), A, all_on, true) == slots(A));
    assert(assign_pad_slots(none, slots(A), A, all_on, false) == slots(A));
    // Controller still off at boot: the only user still gets seat 0, so a single-player game always has player 1.
    assert(assign_pad_slots(none, slots(A), A, all_off, true) == slots(A));
}

// 2. The initial user is player 1 even when the login list has it at another index.
void test_initial_user_first()
{
    assert(assign_pad_slots(none, slots(B, A), A, all_on, true) == slots(A, B));
    assert(assign_pad_slots(none, slots(B, A), A, all_on, false) == slots(A, B));
}

// 3. In game a user who logs out leaves a free seat and nobody moves.
void test_logout_in_game()
{
    const pad_slots prev = slots(A, B, C);
    assert(assign_pad_slots(prev, slots(A, C), A, all_on, false) == slots(A, no_user, C));
    // Even player 1 keeps the seat free: the others do not shift down.
    assert(assign_pad_slots(prev, slots(B, C), A, all_on, false) == slots(no_user, B, C));
}

// 4. In game a new user takes the lowest free seat.
void test_login_in_game()
{
    assert(assign_pad_slots(slots(A, no_user, C), slots(A, C, D), A, all_on, false) == slots(A, D, C));
    assert(assign_pad_slots(slots(A, B, C), slots(A, B, C, D), A, all_on, false) == slots(A, B, C, D));
    // The initial user coming back takes seat 0 when it is free.
    assert(assign_pad_slots(slots(no_user, B), slots(B, A), A, all_on, false) == slots(A, B));
    // When seat 0 went to someone else at boot, the initial user takes the lowest free seat instead.
    assert(assign_pad_slots(slots(B), slots(B, A), A, all_on, false) == slots(B, A));
}

// 5. Boot compaction: connected controllers first, so a single-player game gets the controller that is on.
void test_boot_compaction()
{
    assert(assign_pad_slots(slots(A), slots(A, B), A, connected_set{{B}}, true) == slots(B, A));
    // Ties keep the previous seat order, then the login order.
    assert(assign_pad_slots(slots(A, C, B), slots(B, C, A), A, all_on, true) == slots(A, C, B));
    assert(assign_pad_slots(slots(A, no_user, no_user, C), slots(A, B, C), A, all_on, true) == slots(A, C, B));
    // Disconnected users still get a seat after the connected ones.
    assert(assign_pad_slots(none, slots(A, B, C, D), A, connected_set{{C, D}}, true) == slots(C, D, A, B));
}

// 6. A failed login-list query keeps the previous mapping instead of clearing it.
void test_list_failure_keeps_mapping()
{
    const pad_slots prev = slots(A, B, C);
    assert(update_pad_slots(prev, false, none, A, all_on, false) == prev);
    assert(update_pad_slots(prev, false, none, A, all_off, true) == prev);
    // With a good list the normal rules apply.
    assert(update_pad_slots(prev, true, slots(A, C), A, all_on, false) == slots(A, no_user, C));
}

// 7. Holes and invalid ids in the login list are ignored.
void test_holes_in_login_list()
{
    const pad_slots login = {no_user, E, no_user, D};
    assert(assign_pad_slots(none, login, E, all_on, true) == slots(E, D));
    assert(assign_pad_slots(none, login, E, all_on, false) == slots(E, D));
    assert(assign_pad_slots(slots(no_user, no_user, D), login, E, all_on, false) == slots(E, no_user, D));
    // The system user never owns a seat, and an unknown initial user changes nothing.
    assert(assign_pad_slots(none, slots(system_user, A), system_user, all_on, true) == slots(A));
    assert(assign_pad_slots(none, slots(A, B), no_user, all_on, false) == slots(A, B));
    // A user in prev who is not in the login list any more loses the seat in both modes.
    assert(assign_pad_slots(slots(C, A), slots(A), A, all_on, true) == slots(A));
}

// 8. Idempotence: feeding the result back gives the same mapping, so the generation does not churn.
void test_idempotence()
{
    const pad_slots login = slots(B, D, A);
    for (const bool compact : {true, false})
    {
        const pad_slots first = assign_pad_slots(none, login, A, connected_set{{A, D}}, compact);
        const pad_slots second = assign_pad_slots(first, login, A, connected_set{{A, D}}, compact);
        const pad_slots third = assign_pad_slots(second, login, A, connected_set{{A, D}}, compact);
        assert(first == second);
        assert(second == third);
    }
    // In game a seated user never moves, whatever the controller state.
    const pad_slots seated = slots(D, A, B);
    assert(assign_pad_slots(seated, login, A, all_off, false) == seated);
    assert(assign_pad_slots(seated, login, A, all_on, false) == seated);
}

// Idle-handle release: player 2-4 handles whose controller stays off in game are closed after 10 s and probed.
void test_idle_release()
{
    constexpr std::uint64_t s = 1'000'000;
    pad_idle idle;
    assert(idle.may_open());

    // Never released while releasing is not allowed (menu, player 1, flag).
    idle.observe(false, 1 * s);
    assert(idle.step(100 * s, true, false) == pad_idle_action::none);
    assert(!idle.released);

    // Off for 10 s with releasing allowed.
    idle.reset();
    idle.observe(false, 1 * s);
    assert(idle.step(10 * s, true, true) == pad_idle_action::none);
    assert(idle.step(11 * s, true, true) == pad_idle_action::release);
    assert(idle.released && !idle.may_open());

    // Closed: nothing until the probe time, then one reopen with a probe window.
    assert(idle.step(12 * s, false, true) == pad_idle_action::none);
    assert(idle.step(16 * s, false, true) == pad_idle_action::reopen);
    assert(idle.may_open());
    assert(idle.step(17 * s, true, true) == pad_idle_action::none);
    assert(idle.step(18 * s, true, true) == pad_idle_action::close_probe);
    assert(idle.released && !idle.may_open());
    assert(idle.step(20 * s, false, true) == pad_idle_action::none);
    assert(idle.step(23 * s, false, true) == pad_idle_action::reopen);

    // The controller is back during the probe: the handle stays open and the state is clean.
    idle.observe(true, 24 * s);
    assert(!idle.released && idle.may_open());
    assert(idle.step(40 * s, true, true) == pad_idle_action::none);

    // Back in the menu while released: reopen at once.
    idle.observe(false, 50 * s);
    assert(idle.step(61 * s, true, true) == pad_idle_action::release);
    assert(idle.step(62 * s, false, false) == pad_idle_action::reopen);
    assert(!idle.released && idle.may_open());

    // A controller that turns on again before 10 s restarts the count.
    idle.reset();
    idle.observe(false, 1 * s);
    idle.observe(true, 5 * s);
    idle.observe(false, 6 * s);
    assert(idle.step(12 * s, true, true) == pad_idle_action::none);
    assert(idle.step(16 * s, true, true) == pad_idle_action::release);

    // A handle that is already closed is not "released" again.
    idle.reset();
    idle.observe(false, 1 * s);
    assert(idle.step(30 * s, false, true) == pad_idle_action::none);

    // Time zero is still a valid "off since".
    idle.reset();
    idle.observe(false, 0);
    assert(idle.off_since != 0);
}
} // namespace

int main()
{
    test_single_initial_user();
    test_initial_user_first();
    test_logout_in_game();
    test_login_in_game();
    test_boot_compaction();
    test_list_failure_keeps_mapping();
    test_holes_in_login_list();
    test_idempotence();
    test_idle_release();
    static_assert(utils::ps4::empty_pad_slots() == none);
    std::puts("pad slots test passed");
    return 0;
}
