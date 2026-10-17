#pragma once

// 0.31: process-wide owner of the PS4 users and their DualShock 4 pads (ps4_pads.cpp, frontend target).
// It is the only code that calls scePadOpen, scePadGetHandle and scePadClose; the XMB launcher and the in-game pad
// handler only read through this API. RPCS3 players 1-4 are "seats" mapped to logged-in PS4 users, and the PS4 user
// who started the app is player 1. Pad handles stay open across games.
//
// Every function is thread-safe (one internal mutex), never throws and keeps the last good mapping when a system
// call fails. Seat numbers (slot) are 0-based: slot 0 is player 1.

#ifdef RPCS3_PS4

#include <orbis/_types/pad.h>

#include <array>
#include <cstdint>
#include <string>

namespace ps4_pads
{
inline constexpr unsigned max_players = 4; // the PS4 allows at most 4 logged-in users

// Present: the seat light bars keep the system's user colours.
inline constexpr char no_lightbar_flag[] = "/data/rpcs3/no-lightbar.flag";

// Present: handles of seats 2-4 stay open during a game even when their controller has been off for a long time.
// Without it, such a handle is closed after 10 s and reopened briefly every 5 s to notice the controller again.
inline constexpr char keep_open_flag[] = "/data/rpcs3/pad-keep-open.flag";

struct player
{
    std::int32_t user = -1; // PS4 user id in this seat, -1 = free seat
    bool connected = false; // this user's controller is on and reporting
    std::string name;       // PS4 user name (UTF-8), empty for a free seat
};

// Light bar colour of a seat: blue, red, green, pink (the PS4 palette, in player order).
constexpr OrbisPadColor light_color(unsigned slot) noexcept
{
    switch (slot)
    {
    case 0: return {0x00, 0x40, 0xff, 0xff};
    case 1: return {0xff, 0x10, 0x20, 0xff};
    case 2: return {0x10, 0xd0, 0x30, 0xff};
    default: return {0xff, 0x40, 0xb0, 0xff};
    }
}

// One-time setup, safe to call again: sceUserServiceInitialize ("already initialized" is fine) and scePadInit.
// Also marks the game window as focused, so pad input never depends on the "Background input" option.
// Returns false when the pad library could not be initialized.
bool initialize();

// Drains user login/logout events and, every 500 ms (at once when 'force' is set or an event arrived), re-reads
// the login list, opens pads of new users, closes pads of users who logged out and recomputes the seats.
// compact = true only while no game has seen the seats (XMB, game boot): connected controllers first, the initial
// user first among them. compact = false in game: a seated player never moves and a new user takes the lowest free
// seat. If the system calls fail the previous mapping is kept, never cleared. Returns the mapping generation.
unsigned refresh(bool compact, bool force = false);

// Incremented every time the seat mapping changes.
unsigned generation();

// PS4 user id in 'slot', or -1 for a free seat or an invalid slot.
std::int32_t user(unsigned slot);

// Current state of the pad in 'slot' (scePadReadState). Returns true only when the seat has an open pad that
// reports connected; otherwise 'out' is zeroed and the result is false. Re-applies the seat light bar when the
// controller (re)connects.
bool read(unsigned slot, OrbisPadData& out);

// Samples buffered since the previous call (scePadRead), oldest first, at most 'max' (the library keeps up to
// ORBIS_PAD_MAX_DATA_NUM). Falls back to one scePadReadState sample when scePadRead fails. Returns the number of
// samples written, 0 when there is nothing new, or -1 when the seat has no open pad.
// Samples already returned (same or older timestamp) are not returned again. A sample taken while the controller is
// off is neutral (connected = 0, no buttons, centred sticks, triggers released), so buttons held when it turned off
// are released; it is returned once per disconnection.
int read_samples(unsigned slot, OrbisPadData* out, int max);

// Rumble on the pad in 'slot' (0 = motor off). Ignored for a free seat.
void set_vibration(unsigned slot, std::uint8_t large, std::uint8_t small);

// Same as set_vibration, but returns true only when the pad accepted the values (the caller may cache them).
bool vibrate(unsigned slot, std::uint8_t large, std::uint8_t small);

// Call when the system reports ON_RESUME (any thread, lock-free). The next refresh() re-reads the login list at once,
// re-checks every handle and re-applies the light bars. A long gap between refresh() calls has the same effect.
void on_resume();

std::array<player, max_players> snapshot();

// Seats whose controller is connected.
unsigned connected_count();

// One line for the XMB and the settings menu, e.g. "J1 Ana · J2 Convidado · J3 livre · J4 livre".
std::string summary();
} // namespace ps4_pads

#endif
