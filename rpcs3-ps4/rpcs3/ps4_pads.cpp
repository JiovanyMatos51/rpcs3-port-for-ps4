#include "stdafx.h"
#include "ps4_pads.h"

#ifdef RPCS3_PS4

#include "ps4_fatal.h"
#include "ps4_launcher.h"
#include "util/ps4_i18n.hpp"
#include "util/ps4_pad_slots.hpp"

#include <orbis/Pad.h>
#include <orbis/UserService.h>
#include <orbis/libkernel.h>

#include <unistd.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>

LOG_CHANNEL(ps4_pads_log, "PS4 Pads");

// Defined in Emu/RSX/GSFrameBase.cpp. The desktop UI sets it from the game window focus. There is no such window on
// PS4, and pad_thread stops polling every controller while it is false unless "Background input" is enabled, which
// the RSX home menu can switch off at runtime.
extern atomic_t<bool> g_game_window_focused;

// 0.33: libScePad export, declared as void() by the SDK header. Bound under a private name with its real prototype,
// weak so a missing export only leaves the SIXAXIS sensors neutral instead of failing the load. The console's loader
// leaves weak imports from modules other than libkernel NULL even though libScePad is loaded, so motion_entry() falls
// back to a lookup by name.
extern "C" s32 ps4_pad_set_motion_sensor_state(s32 handle, bool enable) __asm__("scePadSetMotionSensorState") __attribute__((weak));

namespace ps4_pads
{
namespace
{
using utils::ps4::no_user;
using utils::ps4::pad_idle;
using utils::ps4::pad_idle_action;
using utils::ps4::pad_slots;
using utils::ps4::valid_pad_user;

static_assert(max_players == utils::ps4::pad_players);

constexpr u64 scan_period_us = 500'000;    // login-list poll
constexpr u64 tick_period_us = 100'000;    // user-event drain and idle-handle step
constexpr u64 retry_open_us = 1'000'000;   // after a failed open or read
constexpr u64 gap_force_us = 3'000'000;    // no refresh() for this long: re-check everything, as after a resume
constexpr u64 state_settle_us = 250'000;   // a freshly opened pad may report "off" until its first report arrives
constexpr int max_events_per_tick = 8;
constexpr s32 err_no_event = static_cast<s32>(ORBIS_USER_SERVICE_ERROR_NO_EVENT);
constexpr s32 err_pad_not_initialized = static_cast<s32>(ORBIS_PAD_ERROR_NOT_INITIALIZED);
constexpr s32 err_lightbar_refused = static_cast<s32>(ORBIS_PAD_ERROR_INVALID_LIGHTBAR_SETTING);

struct user_pad
{
    s32 user = no_user;
    s32 handle = -1;
    u64 retry_at = 0;
    u64 opened_at = 0;
    bool connected = false;   // last read reported the controller on
    bool state_known = false; // at least one read since the handle was opened
    bool count_valid = false;
    u8 count = 0;             // connection counter of the pad data; changes when the controller reconnects
    int lit_slot = -1;        // seat whose colour was last applied, -1 = none
    u64 lit_at = 0;           // when the colour was last applied
    pad_idle idle{};
    u64 sample_ts = 0;        // newest timestamp returned by read_samples
    bool sample_on = false;   // the last sample returned by read_samples was connected
    u32 open_error = 0;       // last open failure, logged once per code
    u32 read_error = 0;       // last read failure, logged once per code
    char name[64]{};          // UTF-8; the system limit is 16 characters
};

std::mutex s_mutex;
std::array<user_pad, max_players> s_pads{};
pad_slots s_map = utils::ps4::empty_pad_slots();
std::atomic<s32> s_seat_user[max_players]{no_user, no_user, no_user, no_user}; // lock-free copy of s_map
s32 s_initial = no_user;
u64 s_init_retry_at = 0;
u64 s_next_scan = 0;
long long s_last_wall = 0;     // wall clock (s) at the previous tick
u64 s_last_wall_at = 0;        // process clock (us) at the previous tick
unsigned s_probes = 0;         // idle-handle probe reopens since start
bool s_no_lightbar = false;
bool s_keep_open = false;
bool s_lightbar_warned = false;
bool s_read_samples_warned = false;
bool s_motion_logged = false;  // first scePadSetMotionSensorState result logged
u32 s_motion_error = 0;        // last scePadSetMotionSensorState failure, logged once per code
u32 s_list_error = 0;
unsigned s_event_lines = 0;
unsigned s_open_lines = 0;     // startup.log lines about opened handles (a handle that keeps failing reopens every second)
unsigned s_init_failures = 0;

std::atomic<bool> s_ready{false};
std::atomic<bool> s_resume{false};
std::atomic<bool> s_mode_game{false}; // the last tick ran for a game (compact = false)
std::atomic<u64> s_next_tick{0};
std::atomic<u64> s_last_call{0};
std::atomic<unsigned> s_generation{0};

struct toast_list
{
    char text[max_players * 2][96]{};
    unsigned count = 0;
};

u64 now_us()
{
    return sceKernelGetProcessTime();
}

[[gnu::format(printf, 1, 2)]] void checkpoint(const char* format, ...)
{
    char line[256];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    ps4_startup_checkpoint(line);
}

// 0.39: the light bar colour of a seat in the interface language
utils::ps4::i18n::str color_name(unsigned slot)
{
    using utils::ps4::i18n::str;
    static constexpr str names[max_players] = {str::PADS_COLOUR_BLUE, str::PADS_COLOUR_RED, str::PADS_COLOUR_GREEN, str::PADS_COLOUR_PINK};
    return names[slot < max_players ? slot : max_players - 1];
}

user_pad* find_pad(s32 user)
{
    if (!valid_pad_user(user)) return nullptr;
    for (user_pad& pad : s_pads)
    {
        if (pad.user == user) return &pad;
    }
    return nullptr;
}

user_pad* seat_pad(unsigned slot)
{
    return slot < max_players ? find_pad(s_map[slot]) : nullptr;
}

int seat_of(s32 user)
{
    for (unsigned i = 0; i < max_players; ++i)
    {
        if (valid_pad_user(user) && s_map[i] == user) return static_cast<int>(i);
    }
    return -1;
}

void publish_map()
{
    for (unsigned i = 0; i < max_players; ++i) s_seat_user[i].store(s_map[i], std::memory_order_relaxed);
}

void apply_light(user_pad& pad, unsigned slot, u64 now)
{
    pad.lit_slot = static_cast<int>(slot);
    pad.lit_at = now;
    if (s_no_lightbar || pad.handle < 0) return;
    OrbisPadColor color = light_color(slot);
    const s32 result = scePadSetLightBar(pad.handle, &color);
    if (result < 0 && !s_lightbar_warned)
    {
        s_lightbar_warned = true;
        ps4_pads_log.warning("scePadSetLightBar(J%u) failed: 0x%08x%s", slot + 1, static_cast<u32>(result),
            result == err_lightbar_refused ? " (setting refused)" : "");
        checkpoint("pads: scePadSetLightBar failed 0x%08x; light bars keep the system colours", static_cast<u32>(result));
    }
}

// Stops rumble, gives the light bar back to the system and closes the handle. Errors are ignored: the handle may
// already be invalid (for example after its PS4 user logged out).
void close_pad(user_pad& pad, bool quiet)
{
    if (pad.handle >= 0)
    {
        const OrbisPadVibeParam off{0, 0};
        scePadSetVibration(pad.handle, &off);
        if (pad.lit_slot >= 0 && !s_no_lightbar) scePadResetLightBar(pad.handle);
        const s32 result = scePadClose(pad.handle);
        if (!quiet)
        {
            ps4_pads_log.notice("Closed the pad of user 0x%x (handle %d, scePadClose 0x%x)", pad.user, pad.handle, static_cast<u32>(result));
        }
    }
    pad.handle = -1;
    pad.connected = false;
    pad.state_known = false;
    pad.count_valid = false;
    pad.lit_slot = -1;
    pad.sample_ts = 0;
    pad.sample_on = false;
}

using motion_fn = decltype(&ps4_pad_set_motion_sensor_state);

// scePadSetMotionSensorState: the weak import when the loader bound it, else the export found by name in the loaded
// modules (sceKernelDlsym by plain name, as the profiler does on the console). Resolved once; nullptr = not exported.
motion_fn motion_entry()
{
    static const motion_fn entry = []() -> motion_fn
    {
        if (ps4_pad_set_motion_sensor_state) return ps4_pad_set_motion_sensor_state;
        OrbisKernelModule handles[256]{};
        std::size_t count = 0;
        if (sceKernelGetModuleList(handles, 256, &count) != 0) return nullptr;
        for (std::size_t i = 0; i < count && i < 256; ++i)
        {
            void* address = nullptr;
            if (sceKernelDlsym(static_cast<s32>(handles[i]), "scePadSetMotionSensorState", &address) == 0 && address)
            {
                ps4_pads_log.notice("scePadSetMotionSensorState bound through sceKernelDlsym (the loader left the weak import NULL)");
                return reinterpret_cast<motion_fn>(address);
            }
        }
        return nullptr;
    }();
    return entry;
}

// The game handler reports the accelerometer and gyro as DS3 SIXAXIS data. The state belongs to the handle, so it is
// set after every open, including a handle the process already held (the launcher never reads the motion values).
void enable_motion(const user_pad& pad)
{
    const motion_fn set_motion = motion_entry();
    if (!set_motion)
    {
        if (!s_motion_logged)
        {
            s_motion_logged = true;
            ps4_pads_log.warning("scePadSetMotionSensorState is not exported; SIXAXIS data may stay neutral");
        }
        return;
    }
    const s32 result = set_motion(pad.handle, true);
    if (result < 0)
    {
        if (static_cast<u32>(result) != s_motion_error)
        {
            s_motion_error = static_cast<u32>(result);
            ps4_pads_log.warning("scePadSetMotionSensorState(user 0x%x, handle %d) failed: 0x%08x; SIXAXIS data may stay neutral",
                pad.user, pad.handle, static_cast<u32>(result));
        }
        return;
    }
    if (!s_motion_logged)
    {
        s_motion_logged = true;
        ps4_pads_log.notice("Motion sensors enabled (user 0x%x, handle %d)", pad.user, pad.handle);
    }
}

// scePadGetHandle first (a handle the process already holds), then scePadOpen. 'quiet' is used by the idle probes,
// which reopen every few seconds and must not flood the logs.
bool ensure_open(user_pad& pad, u64 now, bool force, bool quiet)
{
    if (pad.handle >= 0) return true;
    if (!valid_pad_user(pad.user)) return false;
    if (!force && now < pad.retry_at) return false;

    const bool detail = !quiet && s_open_lines < 16;
    const unsigned long long flexible_before = detail ? ps4_flexible_memory_free() : 0;
    const unsigned long long internal_before = detail ? ps4_internal_memory_free() : 0;
    s32 handle = scePadGetHandle(pad.user, ORBIS_PAD_PORT_TYPE_STANDARD, 0);
    const bool existing = handle >= 0;
    if (!existing) handle = scePadOpen(pad.user, ORBIS_PAD_PORT_TYPE_STANDARD, 0, nullptr);
    if (handle < 0)
    {
        pad.retry_at = now + retry_open_us;
        if (static_cast<u32>(handle) != pad.open_error)
        {
            pad.open_error = static_cast<u32>(handle);
            ps4_pads_log.error("scePadOpen(user 0x%x) failed: 0x%08x; retrying every second", pad.user, static_cast<u32>(handle));
            checkpoint("pads: scePadOpen(user 0x%x) failed 0x%08x; retrying every second", static_cast<u32>(pad.user), static_cast<u32>(handle));
        }
        return false;
    }

    pad.handle = handle;
    pad.open_error = 0;
    pad.read_error = 0;
    pad.opened_at = now;
    pad.connected = false;
    pad.state_known = false;
    pad.count_valid = false;
    pad.lit_slot = -1;
    pad.sample_ts = 0;
    pad.sample_on = false;
    if (!detail)
    {
        if (!quiet) ps4_pads_log.notice("Pad of user 0x%x opened again (handle %d)", pad.user, handle);
    }
    else
    {
        ++s_open_lines;
        // The information layout is reverse-engineered: logged only. The memory figures show what one more open
        // handle costs the process (thread stacks come from flexible memory, pthread objects from the internal pool).
        OrbisPadInformation info{};
        const s32 info_result = scePadGetControllerInformation(handle, &info);
        const unsigned long long flexible_after = ps4_flexible_memory_free();
        const unsigned long long internal_after = ps4_internal_memory_free();
        checkpoint("pads: user 0x%x pad handle %d (%s); info 0x%08x connection %u class %d connected %d; flexible free %llu -> %llu KiB, kernel internal free %llu -> %llu KiB",
            static_cast<u32>(pad.user), handle, existing ? "existing handle" : "scePadOpen", static_cast<u32>(info_result),
            info.connectionType, info.deviceClass, info.connected, flexible_before >> 10, flexible_after >> 10,
            internal_before >> 10, internal_after >> 10);
    }
    enable_motion(pad);
    return true;
}

void read_failed(user_pad& pad, s32 result, u64 now, const char* what)
{
    if (static_cast<u32>(result) != pad.read_error)
    {
        pad.read_error = static_cast<u32>(result);
        ps4_pads_log.warning("%s(user 0x%x, handle %d) failed: 0x%08x; reopening", what, pad.user, pad.handle, static_cast<u32>(result));
    }
    close_pad(pad, true);
    pad.retry_at = now + retry_open_us;
}

// Updates the connection state from one read. slot < 0: the seat is not known yet (no light bar change).
void observe_state(user_pad& pad, int slot, const OrbisPadData& data, u64 now)
{
    const bool on = data.connected != 0;
    const bool rising = on && !pad.connected;
    // A new connection counter means another controller (or the same one reconnected between two reads). At most
    // once per second, so a counter that moves for another reason cannot flood the controller with light reports.
    const bool swapped = on && pad.count_valid && data.count != pad.count && now - pad.lit_at >= retry_open_us;
    if (on && pad.idle.released)
    {
        ps4_pads_log.notice("Controller of user 0x%x is back; its pad handle stays open", pad.user);
    }
    if (rising || (!on && pad.connected))
    {
        if (slot >= 0) ps4_pads_log.notice("J%d: controller %s (user 0x%x)", slot + 1, on ? "on" : "off", pad.user);
        else ps4_pads_log.notice("Controller of user 0x%x %s (no seat yet)", pad.user, on ? "on" : "off");
    }
    // 0.33: a controller that comes back on a handle that stayed open (switched off and on, or another one between two
    // reads) may come back with its motion sensors off. A fresh handle (state not known yet) was set up by ensure_open.
    if ((rising && pad.state_known) || swapped) enable_motion(pad);
    pad.connected = on;
    pad.state_known = true;
    pad.count = data.count;
    pad.count_valid = true;
    pad.idle.observe(on, now);
    if (on && slot >= 0 && (rising || swapped || pad.lit_slot != slot)) apply_light(pad, static_cast<unsigned>(slot), now);
}

// After a resume or a long gap the handles may have been invalidated or the light bars reset by the system.
void reawaken(const char* why)
{
    for (user_pad& pad : s_pads)
    {
        pad.retry_at = 0;
        pad.lit_slot = -1;
        if (pad.idle.released && pad.handle < 0) pad.idle.reset();
    }
    s_next_scan = 0;
    if (why) checkpoint("pads: %s; re-checking users, handles and light bars", why);
}

void drain_events(bool& rescan)
{
    for (int i = 0; i < max_events_per_tick; ++i)
    {
        OrbisUserServiceEvent event{};
        const s32 result = sceUserServiceGetEvent(&event);
        if (result < 0)
        {
            if (result != err_no_event && s_event_lines < 8)
            {
                ++s_event_lines;
                checkpoint("pads: sceUserServiceGetEvent returned 0x%08x", static_cast<u32>(result));
            }
            return;
        }
        rescan = true;
        const bool login = event.event == SCE_USER_SERVICE_EVENT_TYPE_LOGIN;
        ps4_pads_log.notice("User event: %s user 0x%x", login ? "login" : "logout", event.userId);
        if (s_event_lines < 32)
        {
            ++s_event_lines;
            checkpoint("pads: user event %s (%d) user 0x%x", login ? "LOGIN" : "LOGOUT", static_cast<int>(event.event), static_cast<u32>(event.userId));
        }
    }
}

void describe_seat(char* out, std::size_t size, unsigned slot)
{
    const user_pad* pad = seat_pad(slot);
    if (!pad)
    {
        std::snprintf(out, size, "J%u=-", slot + 1);
        return;
    }
    std::snprintf(out, size, "J%u=0x%x %s", slot + 1, static_cast<u32>(pad->user),
        pad->handle < 0 ? "closed" : pad->connected ? "on" : "off");
}

void scan(u64 now, bool compact, bool force, toast_list& toasts)
{
    OrbisUserServiceLoginUserIdList list{};
    const s32 list_result = sceUserServiceGetLoginUserIdList(&list);
    pad_slots login = utils::ps4::empty_pad_slots();
    if (list_result >= 0)
    {
        for (unsigned i = 0; i < max_players; ++i) login[i] = valid_pad_user(list.userId[i]) ? list.userId[i] : no_user;
    }
    // The application always runs for a logged-in user, so a list without any user can only be a transient answer
    // (for example while the system resumes) and is treated like a failed call: clearing the seats would reshuffle
    // players 2-4 in game.
    const s32 list_status = list_result < 0 ? list_result : utils::ps4::count_pad_users(login) ? 0 : -1;
    const bool list_ok = list_status == 0;
    if (list_ok)
    {
        s_list_error = 0;
    }
    else if (static_cast<u32>(list_status) != s_list_error)
    {
        s_list_error = static_cast<u32>(list_status);
        if (list_result < 0)
            checkpoint("pads: sceUserServiceGetLoginUserIdList failed 0x%08x; keeping the current players", static_cast<u32>(list_result));
        else
            checkpoint("pads: sceUserServiceGetLoginUserIdList returned no user; keeping the current players");
    }

    if (!valid_pad_user(s_initial))
    {
        s32 initial = no_user;
        if (sceUserServiceGetInitialUser(&initial) >= 0 && valid_pad_user(initial))
        {
            s_initial = initial;
            checkpoint("pads: initial user 0x%x is player 1", static_cast<u32>(initial));
        }
    }

    if (list_ok)
    {
        for (user_pad& pad : s_pads)
        {
            if (!valid_pad_user(pad.user) || utils::ps4::contains_pad_user(login, pad.user)) continue;
            ps4_pads_log.notice("User 0x%x logged out", pad.user);
            close_pad(pad, false);
            pad = user_pad{};
        }
        for (const s32 user : login)
        {
            if (!valid_pad_user(user) || find_pad(user)) continue;
            for (user_pad& pad : s_pads)
            {
                if (valid_pad_user(pad.user)) continue;
                pad = user_pad{};
                pad.user = user;
                if (sceUserServiceGetUserName(user, pad.name, sizeof(pad.name) - 1) < 0) pad.name[0] = 0;
                pad.name[sizeof(pad.name) - 1] = 0;
                ps4_pads_log.notice("User 0x%x logged in: '%s'", user, pad.name);
                break;
            }
        }
    }

    for (user_pad& pad : s_pads)
    {
        if (!valid_pad_user(pad.user)) continue;
        if (pad.handle < 0 && pad.idle.may_open()) ensure_open(pad, now, force, pad.idle.released);
        if (compact && pad.handle >= 0)
        {
            OrbisPadData data{};
            const s32 result = scePadReadState(pad.handle, &data);
            if (result < 0) read_failed(pad, result, now, "scePadReadState");
            else observe_state(pad, seat_of(pad.user), data, now);
        }
    }

    // A pad opened a moment ago may still report "off": count it as connected so the initial user keeps seat 0.
    const auto connected = [now](s32 user)
    {
        const user_pad* pad = find_pad(user);
        if (!pad || pad->handle < 0) return false;
        return pad->connected || !pad->state_known || now - pad->opened_at < state_settle_us;
    };
    const pad_slots next = utils::ps4::update_pad_slots(s_map, list_ok, login, s_initial, connected, compact);
    if (next != s_map)
    {
        const pad_slots old = s_map;
        s_map = next;
        publish_map();
        const unsigned generation = s_generation.fetch_add(1, std::memory_order_acq_rel) + 1;
        if (!compact)
        {
            // 0.39: in the interface language; trs() writes into the fixed buffers without allocating
            namespace i18n = utils::ps4::i18n;
            for (unsigned i = 0; i < max_players && toasts.count < std::size(toasts.text); ++i)
            {
                if (old[i] == next[i]) continue;
                if (valid_pad_user(next[i]) && s_no_lightbar)
                    i18n::trs(toasts.text[toasts.count++], i18n::str::PADS_RPCS3_JOGADOR_ENTROU, {i + 1});
                else if (valid_pad_user(next[i]))
                    i18n::trs(toasts.text[toasts.count++], i18n::str::PADS_RPCS3_JOGADOR_ENTROU_LUZ, {i + 1, color_name(i)});
                else if (valid_pad_user(old[i]))
                    i18n::trs(toasts.text[toasts.count++], i18n::str::PADS_RPCS3_JOGADOR_SAIU, {i + 1});
            }
        }
        char seats[max_players][40];
        for (unsigned i = 0; i < max_players; ++i) describe_seat(seats[i], sizeof(seats[i]), i);
        checkpoint("pads: %s %s %s %s (gen %u, %s)", seats[0], seats[1], seats[2], seats[3], generation, compact ? "menu" : "game");
        ps4_pads_log.notice("Players: %s %s %s %s (gen %u, %s)", seats[0], seats[1], seats[2], seats[3], generation, compact ? "menu" : "game");
    }

    for (unsigned i = 0; i < max_players; ++i)
    {
        user_pad* pad = seat_pad(i);
        if (pad && pad->handle >= 0 && pad->connected && pad->lit_slot != static_cast<int>(i)) apply_light(*pad, i, now);
    }
}

void idle_step(u64 now, bool game)
{
    for (user_pad& pad : s_pads)
    {
        if (!valid_pad_user(pad.user)) continue;
        const int seat = seat_of(pad.user);
        const bool may_release = game && !s_keep_open && seat > 0;
        switch (pad.idle.step(now, pad.handle >= 0, may_release))
        {
        case pad_idle_action::none:
            break;
        case pad_idle_action::release:
            checkpoint("pads: J%d controller off for 10 s; its handle is closed and probed every 5 s", seat + 1);
            close_pad(pad, true);
            break;
        case pad_idle_action::reopen:
            if (ensure_open(pad, now, false, pad.idle.released) && pad.idle.released)
            {
                // Probes are silent, so log the free memory at probe 1, 10, 100, ...: a leak in the open/close
                // cycle shows up as a steady drop between these lines.
                ++s_probes;
                unsigned milestone = 1;
                while (milestone < s_probes && milestone <= 100'000'000u) milestone *= 10;
                if (milestone == s_probes)
                {
                    checkpoint("pads: idle probe %u; flexible free %llu KiB, kernel internal free %llu KiB", s_probes,
                        ps4_flexible_memory_free() >> 10, ps4_internal_memory_free() >> 10);
                }
            }
            break;
        case pad_idle_action::close_probe:
            close_pad(pad, true);
            break;
        }
    }
}

void tick(u64 now, bool compact, bool force, bool resumed, toast_list& toasts)
{
    const bool game = !compact;
    const bool mode_changed = s_mode_game.exchange(game, std::memory_order_relaxed) != game;
    s_next_tick.store(now + tick_period_us, std::memory_order_relaxed);

    // Rest mode: the wall clock moves on even if the process clock does not. Only a wall clock that ran ahead of the
    // process clock counts; a long pause of the callers (game boot, return to the menu) moves both and is handled
    // by refresh() as a plain gap, without a log line.
    const long long wall = static_cast<long long>(std::time(nullptr));
    const u64 process_elapsed = now > s_last_wall_at ? now - s_last_wall_at : 0;
    const bool wall_gap = s_last_wall > 0 && wall > s_last_wall &&
        static_cast<u64>(wall - s_last_wall) * 1'000'000 > process_elapsed + gap_force_us;
    s_last_wall = wall;
    s_last_wall_at = now;
    if (resumed) reawaken("system resume");
    else if (wall_gap) reawaken("wall clock jumped (rest mode?)");
    else if (force) reawaken(nullptr);

    bool rescan = force || resumed || wall_gap || mode_changed || now >= s_next_scan;
    drain_events(rescan);
    if (rescan)
    {
        s_next_scan = now + scan_period_us;
        scan(now, compact, force || resumed || wall_gap, toasts);
    }
    idle_step(now, game);
}
} // namespace

bool initialize()
{
    g_game_window_focused = true;
    if (s_ready.load(std::memory_order_acquire)) return true;

    std::lock_guard lock(s_mutex);
    if (s_ready.load(std::memory_order_relaxed)) return true;
    const u64 now = now_us();
    if (s_init_retry_at && now < s_init_retry_at) return false;
    s_init_retry_at = now + retry_open_us;

    // "Already initialized" is fine: the audio backend initializes UserService too.
    const s32 user_result = sceUserServiceInitialize(nullptr);
    const s32 pad_result = scePadInit();
    bool usable = pad_result >= 0;
    s32 probe = 0;
    if (!usable)
    {
        // A second scePadInit in the process may be refused while the library works. Only "not initialized"
        // from a harmless query means the pad library is really unusable.
        s32 initial = no_user;
        sceUserServiceGetInitialUser(&initial);
        probe = scePadGetHandle(initial, ORBIS_PAD_PORT_TYPE_STANDARD, 0);
        usable = probe != err_pad_not_initialized;
    }
    s_no_lightbar = ::access(no_lightbar_flag, F_OK) == 0;
    s_keep_open = ::access(keep_open_flag, F_OK) == 0;
    if (usable || s_init_failures < 3)
    {
        checkpoint("pads: sceUserServiceInitialize 0x%08x, scePadInit 0x%08x%s; light bars %s; idle handles of players 2-4 %s",
            static_cast<u32>(user_result), static_cast<u32>(pad_result), usable ? "" : " (pad library unusable, retrying every second)",
            s_no_lightbar ? "off (no-lightbar.flag)" : "on", s_keep_open ? "kept open (pad-keep-open.flag)" : "released after 10 s");
    }
    if (!usable)
    {
        if (s_init_failures++ < 3)
        {
            ps4_pads_log.error("Pad library unusable: scePadInit 0x%08x, scePadGetHandle 0x%08x", static_cast<u32>(pad_result), static_cast<u32>(probe));
        }
        return false;
    }
    s_ready.store(true, std::memory_order_release);
    return true;
}

unsigned refresh(bool compact, bool force)
{
    if (!s_ready.load(std::memory_order_acquire) && !initialize()) return s_generation.load(std::memory_order_relaxed);

    const u64 now = now_us();
    const u64 last = s_last_call.exchange(now, std::memory_order_relaxed);
    const bool gap = last && now > last && now - last >= gap_force_us;
    const bool resumed = s_resume.load(std::memory_order_relaxed) && s_resume.exchange(false, std::memory_order_acq_rel);
    if (!force && !gap && !resumed && now < s_next_tick.load(std::memory_order_relaxed))
    {
        return s_generation.load(std::memory_order_relaxed);
    }

    toast_list toasts;
    {
        std::lock_guard lock(s_mutex);
        tick(now, compact, force || gap, resumed, toasts);
    }
    // Outside the lock: the notification request is a system call.
    for (unsigned i = 0; i < toasts.count; ++i) ps4_notify(toasts.text[i]);
    return s_generation.load(std::memory_order_relaxed);
}

unsigned generation()
{
    return s_generation.load(std::memory_order_relaxed);
}

std::int32_t user(unsigned slot)
{
    return slot < max_players ? s_seat_user[slot].load(std::memory_order_relaxed) : no_user;
}

bool read(unsigned slot, OrbisPadData& out)
{
    std::memset(&out, 0, sizeof(out));
    if (slot >= max_players || !s_ready.load(std::memory_order_acquire)) return false;

    std::lock_guard lock(s_mutex);
    user_pad* pad = seat_pad(slot);
    if (!pad) return false;
    const u64 now = now_us();
    if (pad->handle < 0 && (!pad->idle.may_open() || !ensure_open(*pad, now, false, pad->idle.released))) return false;

    OrbisPadData data{};
    const s32 result = scePadReadState(pad->handle, &data);
    if (result < 0)
    {
        read_failed(*pad, result, now, "scePadReadState");
        return false;
    }
    observe_state(*pad, static_cast<int>(slot), data, now);
    if (!data.connected) return false;
    out = data;
    return true;
}

int read_samples(unsigned slot, OrbisPadData* out, int max)
{
    if (slot >= max_players || !s_ready.load(std::memory_order_acquire)) return -1;

    std::lock_guard lock(s_mutex);
    user_pad* pad = seat_pad(slot);
    if (!pad) return -1;
    const u64 now = now_us();
    if (pad->handle < 0 && (!pad->idle.may_open() || !ensure_open(*pad, now, false, pad->idle.released))) return -1;
    if (!out || max <= 0) return 0;

    const int limit = std::min<int>(max, ORBIS_PAD_MAX_DATA_NUM);
    static OrbisPadData buffer[ORBIS_PAD_MAX_DATA_NUM]; // guarded by s_mutex; too large for a pad-thread stack frame
    s32 count = scePadRead(pad->handle, buffer, limit);
    if (count < 0)
    {
        if (!s_read_samples_warned)
        {
            s_read_samples_warned = true;
            ps4_pads_log.warning("scePadRead failed: 0x%08x; using scePadReadState", static_cast<u32>(count));
        }
        count = 0;
    }
    count = std::min<s32>(count, limit);
    if (count == 0)
    {
        // Nothing buffered (or scePadRead is not usable): one state sample keeps the connection state honest, so a
        // controller that turned off while a button was held still produces its release.
        const s32 result = scePadReadState(pad->handle, &buffer[0]);
        if (result < 0)
        {
            read_failed(*pad, result, now, "scePadReadState");
            return -1;
        }
        count = 1;
    }

    u64 newest = 0; // newest timestamp in this batch, 0 = the samples carry none
    for (s32 i = 0; i < count; ++i) newest = std::max<u64>(newest, buffer[i].timestamp);
    if (newest)
    {
        // Oldest first. The order scePadRead uses is not documented, so sort (stable insertion sort, n <= 64). A
        // sample without a timestamp (the "off" state may come without one) counts as the newest.
        const auto key = [](const OrbisPadData& sample) { return sample.timestamp ? sample.timestamp : ~u64{0}; };
        for (s32 i = 1; i < count; ++i)
        {
            const OrbisPadData item = buffer[i];
            s32 j = i;
            for (; j > 0 && key(buffer[j - 1]) > key(item); --j) buffer[j] = buffer[j - 1];
            buffer[j] = item;
        }
        // A smaller timestamp than the last one returned: the controller reconnected and its clock restarted.
        if (newest < pad->sample_ts) pad->sample_ts = 0;
    }
    observe_state(*pad, static_cast<int>(slot), buffer[count - 1], now);

    int written = 0;
    for (s32 i = 0; i < count && written < limit; ++i)
    {
        OrbisPadData sample = buffer[i];
        if (!sample.connected)
        {
            // Released buttons and centred sticks once per disconnection, whatever the timestamp: the "off" state
            // may repeat the timestamp of the last real report, which was already returned.
            if (!pad->sample_on) continue;
            sample = OrbisPadData{};
            sample.leftStick = {0x80, 0x80};
            sample.rightStick = {0x80, 0x80};
            sample.timestamp = buffer[i].timestamp;
            sample.count = buffer[i].count;
        }
        else if (newest && sample.timestamp && sample.timestamp <= pad->sample_ts)
        {
            continue;
        }
        out[written++] = sample;
        pad->sample_on = sample.connected != 0;
    }
    if (newest) pad->sample_ts = std::max<u64>(pad->sample_ts, newest);
    return written;
}

bool vibrate(unsigned slot, std::uint8_t large, std::uint8_t small)
{
    if (slot >= max_players || !s_ready.load(std::memory_order_acquire)) return false;
    std::lock_guard lock(s_mutex);
    user_pad* pad = seat_pad(slot);
    if (!pad || pad->handle < 0) return false;
    const OrbisPadVibeParam param{large, small};
    return scePadSetVibration(pad->handle, &param) >= 0;
}

void set_vibration(unsigned slot, std::uint8_t large, std::uint8_t small)
{
    vibrate(slot, large, small);
}

void on_resume()
{
    s_resume.store(true, std::memory_order_release);
}

std::array<player, max_players> snapshot()
{
    std::array<player, max_players> players{};
    std::lock_guard lock(s_mutex);
    for (unsigned i = 0; i < max_players; ++i)
    {
        players[i].user = s_map[i];
        if (const user_pad* pad = seat_pad(i))
        {
            players[i].connected = pad->handle >= 0 && pad->connected;
            players[i].name = pad->name;
        }
    }
    return players;
}

unsigned connected_count()
{
    unsigned count = 0;
    std::lock_guard lock(s_mutex);
    for (unsigned i = 0; i < max_players; ++i)
    {
        const user_pad* pad = seat_pad(i);
        count += pad && pad->handle >= 0 && pad->connected ? 1 : 0;
    }
    return count;
}

std::string summary()
{
    // 0.39: in the interface language (the settings row shows it; the launcher passes the player count itself)
    namespace i18n = utils::ps4::i18n;
    const auto players = snapshot();
    std::string text;
    for (unsigned i = 0; i < max_players; ++i)
    {
        if (i) text += " \xc2\xb7 "; // U+00B7 middle dot
        text += i18n::trf(i18n::str::PADS_J, {i + 1});
        const player& p = players[i];
        if (!valid_pad_user(p.user))
        {
            text += i18n::tr(i18n::str::PADS_LIVRE);
            continue;
        }
        text += p.name.empty() ? std::string(i18n::tr(i18n::str::PADS_USUARIO)) : p.name;
        if (!p.connected) text += i18n::tr(i18n::str::PADS_DESLIGADO);
    }
    return text;
}
} // namespace ps4_pads

#endif
