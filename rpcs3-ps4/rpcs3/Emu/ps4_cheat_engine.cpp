#include "stdafx.h"
#include "Emu/ps4_cheat_engine.h"

#include "Emu/System.h"
#include "Emu/system_config.h"
#include "Emu/IdManager.h"
#include "Emu/Memory/vm.h"
#include "Emu/Memory/vm_reservation.h"
#include "Emu/CPU/CPUThread.h"
#include "Emu/Cell/PPUAnalyser.h"
#include "Emu/Cell/PPUInterpreter.h"
#include "Emu/Cell/timers.hpp"
#include "Emu/Cell/lv2/sys_prx.h"
#include "Emu/Cell/lv2/sys_sync.h"
#include "util/ps4_cheats.hpp"
#include "util/ps4_safe_mode.hpp"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>

LOG_CHANNEL(cheat_log, "Cheats");

void ps4_startup_checkpoint(const char* stage);
void ps4_notify(const char* message) noexcept;
bool ps4_in_background() noexcept;
extern void ppu_register_function_at(u32 addr, u32 size, ppu_intrp_func_t ptr);

namespace ps4_cheats
{
namespace
{
namespace cheats = utils::ps4::cheats;
namespace i18n = utils::ps4::i18n;

constexpr u32 tick_ops = 4096;                  // memory accesses per pass
constexpr u64 tick_budget_us = 2000;            // time per pass
constexpr u64 retry_us = 1'000'000;             // a once cheat that could not be written tries again after 1 s
constexpr u64 summary_us = 60'000'000;          // pointer misses are logged once a minute per cheat
constexpr u64 rsx_query_us = 10'000'000;        // the RSX context is looked up every 10 s until the game has one
constexpr u64 privileged_gap_us = 1'000'000;    // at most one suspend_all write per second

// One switchable cheat of the running game
struct entry
{
    std::string name;
    std::size_t row = 0;              // index in session::game.rows
    cheats::program live;             // ops of the tick (empty: the cheat acts at load only)
    cheats::mode mode = cheats::mode::constant;
    u64 period_us = 100'000;
    bool boot_part = false;           // some ops are written at load
    bool boot_applied = false;        // ...and were written at this load
    std::atomic<bool> on{false};
    std::atomic<bool> rearm{false};   // switched on in the home menu: run at the next pass
    // the tick's own state (main thread only)
    cheats::cursor cursor;
    u64 enabled_at = 0;               // running time it was first due; 0 = not yet
    u64 next_due = 0;
    u64 failing_since = 0;            // running time of the first write failure since the last success, + 1; 0 = none
    u64 next_summary = 0;
    u32 misses = 0;                   // passes skipped by a pointer that was not ready
    bool once_done = false;
    bool off_for_session = false;
    bool written = false;
    bool first_logged = false;
    bool code_noted = false;
    bool refused_noted = false;
    std::string first_text;           // "0x01234560 00000064 -> 05f5e0ff" of the first write
    u32 first_addr = 0;
};

struct session
{
    cheats::game_cheats game;
    std::string hash;
    std::vector<std::unique_ptr<entry>> entries;
    bool from_savestate = false;
    bool interpreter = false;         // PPU interpreter: a once cheat may change code (upstream sequence)
    // the tick's own state (main thread only)
    u64 run_us = 0;                   // running time of the game seen by the tick
    u64 last_now = 0;
    std::size_t next = 0;             // round robin start
    u32 rsx_begin = 0, rsx_end = 0;
    u64 rsx_next_query = 0;
    u64 last_privileged = 0;
};

std::mutex g_mutex;                   // g_session and the switches of its game (never held across a vm call)
std::shared_ptr<session> g_session;
std::atomic<bool> g_live{false};      // the session has at least one cheat with a live part

i18n::lang overlay_language()
{
    // the RSX overlay font has no Arabic shaping (as ps4_localized_string)
    return i18n::current() == i18n::lang::ar ? i18n::lang::en : i18n::current();
}

// Guest memory for the interpreter: only u32 guest addresses, every access checked by vm
struct vm_port
{
    session& s;
    entry& e;

    static bool any_exec(u32 addr, u32 n)
    {
        for (u64 p = addr & ~u64{4095}; p < u64{addr} + n; p += 4096)
            if (vm::get_addr_flags(static_cast<u32>(p)).second & vm::page_executable) return true;
        return false;
    }

    bool allowed(u32 addr, u32 n) const
    {
        const u64 end = u64{addr} + n;
        if (!n || addr < cheats::guest_begin || end > cheats::guest_end) return false;
        return !(s.rsx_end > s.rsx_begin && addr < s.rsx_end && end > s.rsx_begin);
    }

    cheats::access read(u32 addr, void* out, u32 n)
    {
        if (!allowed(addr, n)) return cheats::access::refused;
        return vm::try_access(addr, out, n, false) ? cheats::access::ok : cheats::access::unmapped;
    }

    cheats::access write(u32 addr, const void* in, u32 n)
    {
        if (!allowed(addr, n)) return cheats::access::refused;
        const bool exec = any_exec(addr, n);
        // game code is never written live under PPU LLVM (the compiled code would not change)
        if (exec && !(s.interpreter && e.mode == cheats::mode::once)) return cheats::access::code;
        if (!e.first_logged && e.first_text.empty())
        {
            const u32 shown = std::min<u32>(n, 16);
            std::string old(shown, '\0');
            if (vm::try_access(addr, old.data(), shown, false))
            {
                e.first_text = fmt::format("0x%08x %s -> %s", addr, cheats::detail::hex_bytes(old), cheats::detail::hex_bytes({static_cast<const char*>(in), shown}));
                e.first_addr = addr;
            }
        }
        if (!exec && vm::try_access(addr, const_cast<void*>(in), n, true))
        {
            vm::ps4_touch_reservations(addr, n);
            return cheats::access::ok;
        }
        if (!vm::check_addr(addr, vm::page_readable, n)) return cheats::access::unmapped;
        // readable but not writable (read-only data, or code under the interpreter): once cheats only
        if (e.mode != cheats::mode::once) return exec ? cheats::access::code : cheats::access::read_only;
        return privileged(addr, in, n, exec);
    }

    // The PC cheat manager's write: all CPU threads paused, the store through the sudo mapping, the interpreter
    // cache of the changed code words refreshed
    cheats::access privileged(u32 addr, const void* in, u32 n, bool exec)
    {
        if (s.last_privileged && s.run_us < s.last_privileged + privileged_gap_us) return cheats::access::later;
        s.last_privileged = std::max<u64>(s.run_us, 1);
        const bool ok = cpu_thread::suspend_all(nullptr, {}, [&]() -> bool
        {
            if (!vm::check_addr(addr, vm::page_readable, n)) return false;
            std::memcpy(vm::get_super_ptr<u8>(addr), in, n);
            if (exec)
            {
                for (u64 p = addr & ~u64{4095}; p < u64{addr} + n; p += 4096)
                {
                    if (!(vm::get_addr_flags(static_cast<u32>(p)).second & vm::page_executable)) continue;
                    const u64 begin = std::max<u64>(p, addr) & ~u64{3};
                    const u64 end = (std::min<u64>(p + 4096, u64{addr} + n) + 3) & ~u64{3};
                    ppu_register_function_at(static_cast<u32>(begin), static_cast<u32>(end - begin), nullptr);
                }
            }
            return true;
        });
        if (!ok) return cheats::access::unmapped;
        vm::ps4_touch_reservations(addr, n);
        cheat_log.notice("cheat '%s': privileged write of %u byte(s) at 0x%08x%s", e.name, n, addr, exec ? " (code, interpreter)" : "");
        return cheats::access::ok;
    }
};

// At most tick_ops accesses and tick_budget_us per pass (the clock is read every 64 accesses)
struct tick_budget
{
    u32 ops = tick_ops;
    u64 deadline = 0;
    u32 since_check = 0;

    bool take()
    {
        if (!ops) return false;
        --ops;
        if (++since_check == 64)
        {
            since_check = 0;
            if (get_system_time() >= deadline) ops = 0;
        }
        return true;
    }
};

void notify(const std::string& text)
{
    ps4_notify(text.c_str());
}

// Name of the loaded PRX module whose segments hold 'addr', or empty
std::string prx_at(u32 addr)
{
    std::string name;
    idm::select<lv2_obj, lv2_prx>([&](u32, lv2_prx& prx)
    {
        for (const ppu_segment& seg : prx.segs)
            if (name.empty() && addr >= seg.addr && u64{addr} < u64{seg.addr} + seg.size) name = prx.name;
    });
    return name;
}

// One pass of one cheat
void run_entry(session& s, entry& e, tick_budget& budget, bool& stopped)
{
    const u64 t = s.run_us;
    vm_port port{s, e};
    const cheats::run_result r = cheats::run(e.live, e.cursor, port, budget);
    const u64 period = e.mode == cheats::mode::once ? retry_us : e.period_us;
    switch (r.status)
    {
    case cheats::run_status::done:
        e.written |= r.writes != 0;
        e.failing_since = 0;
        if (!e.first_logged && !e.first_text.empty())
        {
            cheat_log.notice("cheat '%s' line %u: %s", e.name, r.line, e.first_text);
            e.first_logged = true;
            // a PRX is relocated: its addresses may differ between runs and from a real PS3
            if (const std::string prx = prx_at(e.first_addr); !prx.empty())
                cheat_log.warning("cheat '%s': 0x%08x is inside the PRX module '%s' (it may move between runs)", e.name, e.first_addr, prx);
        }
        if (e.mode == cheats::mode::once) e.once_done = true;
        else e.next_due = t + e.period_us;
        break;
    case cheats::run_status::budget:
        stopped = true;
        break;
    case cheats::run_status::not_ready:
        ++e.misses;
        e.first_text.clear();
        e.next_due = t + period;
        break;
    case cheats::run_status::failed:
        e.first_text.clear();
        // + 1: the first pass of a session runs at running time 0 (a plain 't - since' would wrap and turn the cheat off)
        if (!e.failing_since) e.failing_since = t + 1;
        if (!e.written && t + 1 - e.failing_since >= u64{cheats::unmapped_off_ms} * 1000)
        {
            e.off_for_session = true;
            const std::string address = cheats::detail::address_text(r.addr);
            notify(i18n::trf(i18n::str::CHT_RUN_NAO_MAPEADO, {e.name, address}));
            cheat_log.error("cheat '%s' line %u: 0x%08x was not writable for %u s; turned off for this session", e.name, r.line, r.addr, cheats::unmapped_off_ms / 1000);
            ps4_startup_checkpoint(fmt::format("cheats: '%s' turned off: 0x%08x (line %u) not writable for %u s", e.name, r.addr, r.line, cheats::unmapped_off_ms / 1000).c_str());
            break;
        }
        e.next_due = t + period;
        break;
    case cheats::run_status::code:
        e.first_text.clear();
        if (!e.code_noted)
        {
            e.code_noted = true;
            notify(i18n::trf(i18n::str::CHT_RUN_PROXIMO_INICIO, {e.name}));
            cheat_log.warning("cheat '%s' line %u: 0x%08x is game code; it is only written at load", e.name, r.line, r.addr);
        }
        if (e.mode == cheats::mode::once) e.once_done = true;
        else e.next_due = t + e.period_us;
        break;
    case cheats::run_status::refused:
        e.first_text.clear();
        if (!e.refused_noted)
        {
            e.refused_noted = true;
            cheat_log.warning("cheat '%s' line %u: 0x%08x is outside the allowed memory (RSX context, video, stack or SPU); skipped", e.name, r.line, r.addr);
        }
        e.next_due = t + period;
        break;
    }
    if (e.misses && t >= e.next_summary)
    {
        if (e.next_summary) cheat_log.notice("cheat '%s': %u pass(es) skipped in the last minute (pointer not ready)", e.name, e.misses);
        e.misses = 0;
        e.next_summary = t + summary_us;
    }
}

std::shared_ptr<session> current()
{
    std::lock_guard lock(g_mutex);
    return g_session;
}
} // namespace

void reset() noexcept
{
    std::shared_ptr<session> old;
    {
        std::lock_guard lock(g_mutex);
        old = std::move(g_session);
        g_live = false;
    }
    // freed here, outside the lock (a tick still running keeps its own reference)
}

void boot_apply(std::vector<u32>& applied, const ppu_module<lv2_obj>& main, bool from_savestate)
{
    reset();
    const std::string serial = Emu.GetTitleID();
    if (!cheats::valid_serial(serial)) return;
    // 0.40: a player without cheats gets no file written on a boot (0.38 had no cheats at all): the executable record is
    // kept only once the cheats folder exists (an import or a cheat list made it)
    const bool folder = fs::is_dir(cheats::folder);

    // 0.40: the feature row live_cheats; at safe-mode level 1 a game boots without its cheats (boot writes and the
    // tick), so a cheat cannot be what keeps failing
    if (!utils::ps4::safe_mode::enabled(utils::ps4::safe_mode::feature::live_cheats))
    {
        if (folder)
        {
            const cheats::game_cheats game = cheats::load_game(serial, Emu.GetTitle(), Emu.GetAppVersion());
            const auto on = std::count_if(game.rows.begin(), game.rows.end(), [](const cheats::row& r) { return r.on; });
            if (on) ps4_startup_checkpoint(fmt::format("cheats: %u on for %s, none applied in this boot (safe mode)", static_cast<u32>(on), serial).c_str());
        }
        return;
    }

    std::vector<cheats::segment> segs;
    for (const ppu_segment& seg : main.segs)
        if (seg.type == 0x1 && seg.size) segs.push_back({seg.addr, static_cast<u32>(std::min<u64>(u64{seg.addr} + seg.size, 0xffffffffu)), (seg.flags & 0x1) != 0});
    const std::string hash = Emu.GetExecutableHash();
    if (folder) cheats::write_seen(serial, {hash, Emu.GetAppVersion(), segs});

    auto s = std::make_shared<session>();
    s->game = cheats::load_game(serial, Emu.GetTitle(), Emu.GetAppVersion());
    // file notes: caps, skipped lines, cheats listed once, unreadable files
    for (const cheats::problem& p : s->game.notes) cheat_log.notice("%s: %s", serial, cheats::problem_text(p));
    if (s->game.rows.empty()) return;
    s->hash = hash;
    s->from_savestate = from_savestate;
    s->interpreter = g_cfg.core.ppu_decoder != ppu_decoder_type::llvm;

    // code words the patch engine wrote (overlap report)
    std::vector<u32> patched;
    for (const u32 a : applied)
        if (a != umax) patched.push_back(a);
    std::sort(patched.begin(), patched.end());

    u32 on_boot = 0, on_constant = 0, on_once = 0, invalid = 0, boot_bytes = 0, boot_ops = 0, code_words = 0;
    const auto boot_write = [&](const cheats::row& r, u32 addr, std::string_view data) -> bool
    {
        u8* const ptr = main.get_ptr<u8>(addr, static_cast<u32>(data.size()));
        if (!ptr)
        {
            ps4_startup_checkpoint(fmt::format("cheats: '%s': 0x%08x is outside the executable's segments; not written", r.name, addr).c_str());
            return false;
        }
        const bool code = cheats::classify(addr, u64{addr} + data.size(), segs) == cheats::target::code;
        for (u64 w = addr & ~u64{3}; w < u64{addr} + data.size(); w += 4)
            if (std::binary_search(patched.begin(), patched.end(), static_cast<u32>(w)))
            {
                ps4_startup_checkpoint(fmt::format("cheats: '%s' overwrites patch bytes at 0x%08x", r.name, w).c_str());
                break;
            }
        std::memcpy(ptr, data.data(), data.size());
        if (code)
        {
            for (u64 w = addr & ~u64{3}; w < u64{addr} + data.size(); w += 4)
            {
                applied.push_back(static_cast<u32>(w));
                ++code_words;
            }
        }
        else applied.push_back(umax);
        boot_bytes += static_cast<u32>(data.size());
        ++boot_ops;
        return true;
    };

    for (std::size_t i = 0; i < s->game.rows.size(); ++i)
    {
        const cheats::row& r = s->game.rows[i];
        if (!r.valid || (r.needs_value && r.value.empty()))
        {
            invalid += !r.valid;
            continue;
        }
        if (!r.hash.empty() && r.hash != hash)
        {
            cheat_log.notice("cheat '%s' is for %s; this executable is %s", r.name, r.hash, hash);
            continue;
        }
        const cheats::plan pl = cheats::make_plan(r.prog, r.wanted, &segs);
        if (std::any_of(pl.problems.begin(), pl.problems.end(), [](const cheats::problem& p) { return p.fatal; }))
        {
            ++invalid;
            ps4_startup_checkpoint(fmt::format("cheats: '%s' refused: a pointer or condition on the game's code", r.name).c_str());
            continue;
        }
        for (const cheats::problem& p : pl.problems)
            if (r.on) ps4_startup_checkpoint(fmt::format("cheats: '%s': %s", r.name, cheats::problem_text(p)).c_str());

        auto e = std::make_unique<entry>();
        e->name = r.name;
        e->row = i;
        e->live = pl.live;
        e->mode = pl.resolved == cheats::mode::once ? cheats::mode::once : cheats::mode::constant;
        e->period_us = u64{cheats::clamp_period(r.period_ms)} * 1000;
        e->boot_part = !pl.boot.empty();
        e->on = r.on;
        if (r.on && !from_savestate && e->boot_part)
        {
            for (const cheats::op& o : pl.boot)
            {
                if (o.kind == cheats::op_kind::write)
                {
                    boot_write(r, o.addr, o.bytes);
                    continue;
                }
                // a series: one store each
                const u64 mask = o.width == 8 ? ~u64{0} : (u64{1} << (8 * o.width)) - 1;
                const u64 first = cheats::detail::read_be(o.bytes.data(), o.width);
                for (u32 k = 0; k < o.count; ++k)
                {
                    std::string value;
                    cheats::detail::append_be(value, (first + u64{k} * o.value_step) & mask, o.width);
                    boot_write(r, static_cast<u32>(u64{o.addr} + u64{k} * o.addr_step), value);
                }
            }
            e->boot_applied = true;
        }
        if (r.on)
        {
            if (pl.resolved == cheats::mode::boot) ++on_boot;
            else if (pl.resolved == cheats::mode::once) ++on_once;
            else ++on_constant;
        }
        s->entries.push_back(std::move(e));
    }

    const u32 on = on_boot + on_constant + on_once;
    ps4_startup_checkpoint(fmt::format("cheats: %u on for %s (boot %u, constant %u, once %u; %u invalid; %u listed)%s", on, serial, on_boot, on_constant,
        on_once, invalid, s->game.rows.size(), from_savestate ? "; savestate: nothing written at load" : "").c_str());
    if (boot_ops) ps4_startup_checkpoint(fmt::format("cheats: boot wrote %u byte(s) in %u op(s) (code words %u)", boot_bytes, boot_ops, code_words).c_str());
    if (on)
    {
        notify(i18n::trf(i18n::str::CHT_TOAST_ATIVOS, {on}));
        if (g_cfg.net.psn_status == np_psn_status::psn_rpcn)
        {
            // RPCN on: warned, never blocked
            notify(i18n::tr(i18n::str::CHT_AVISO_ONLINE));
            ps4_startup_checkpoint("cheats: RPCN is on while cheats are on (warned)");
        }
    }
    const bool live = std::any_of(s->entries.begin(), s->entries.end(), [](const std::unique_ptr<entry>& e) { return !e->live.ops.empty(); });
    std::lock_guard lock(g_mutex);
    g_session = std::move(s);
    g_live = live;
}

void tick(u64 now_us) noexcept
{
    // 0.40: boot_apply already leaves no live session at safe-mode level 1; the row is read here too (one relaxed load)
    if (!g_live.load(std::memory_order_relaxed) || !utils::ps4::safe_mode::enabled(utils::ps4::safe_mode::feature::live_cheats)) return;
    const std::shared_ptr<session> s = current();
    if (!s) return;
    if (!Emu.IsRunning() || ps4_in_background())
    {
        s->last_now = 0;
        return;
    }
    // running time: the gap between passes, at most 100 ms (a pause or a long main-thread call is not counted)
    if (s->last_now && now_us > s->last_now) s->run_us += std::min<u64>(now_us - s->last_now, 100'000);
    s->last_now = now_us;
    const u64 t = s->run_us;

    if (!s->rsx_end && t >= s->rsx_next_query)
    {
        s->rsx_next_query = t + rsx_query_us;
        if (const auto block = vm::get(vm::rsx_context))
        {
            s->rsx_begin = block->addr;
            s->rsx_end = static_cast<u32>(std::min<u64>(u64{block->addr} + block->size, 0xffffffffu));
        }
    }

    tick_budget budget;
    budget.deadline = get_system_time() + tick_budget_us;
    const std::size_t n = s->entries.size();
    bool stopped = false;
    for (std::size_t k = 0; k < n && !stopped && budget.ops; ++k)
    {
        const std::size_t index = (s->next + k) % n;
        entry& e = *s->entries[index];
        if (e.live.ops.empty()) continue;
        if (!e.on.load(std::memory_order_relaxed))
        {
            e.cursor = {};
            continue;
        }
        if (e.rearm.exchange(false))
        {
            // switched on in the home menu: due now, its earlier failures forgotten
            e.off_for_session = false;
            e.code_noted = false;
            e.once_done = false;
            e.cursor = {};
            e.failing_since = 0;
            e.enabled_at = std::max<u64>(t, 1);
            e.next_due = t;
        }
        if (e.off_for_session) continue;
        if (!e.enabled_at)
        {
            e.enabled_at = std::max<u64>(t, 1);
            e.next_due = e.mode == cheats::mode::once ? t + u64{cheats::once_delay_ms} * 1000 : t;
        }
        if (e.mode == cheats::mode::once && e.once_done) continue;
        if (!e.cursor.pc && !e.cursor.sub && t < e.next_due) continue;
        run_entry(*s, e, budget, stopped);
        if (stopped) s->next = index;
    }
    if (!stopped && n) s->next = (s->next + 1) % n;
}

std::vector<live_row> rows()
{
    std::vector<live_row> out;
    std::lock_guard lock(g_mutex);
    if (!g_session) return out;
    const i18n::lang l = overlay_language();
    for (const auto& e : g_session->entries)
    {
        const bool on = e->on.load();
        const bool live = !e->live.ops.empty();
        // the switch acts at the next start only: no live part, or a code part that differs from this load
        const bool next_boot = !live || (e->boot_part && e->boot_applied != on);
        std::string label = (on ? "[x] " : "[ ] ") + e->name;
        if (g_session->from_savestate && e->boot_part) label += std::string(" ") + i18n::tr(i18n::str::CHT_HOME_DO_SAVESTATE, l);
        else if (next_boot) label += std::string(" ") + i18n::tr(i18n::str::CHT_HOME_PROXIMO_INICIO, l);
        out.push_back({std::move(label), on, next_boot});
    }
    return out;
}

std::string toggle(std::size_t index)
{
    std::string state, serial, name;
    bool on = false;
    {
        std::lock_guard lock(g_mutex);
        if (!g_session || index >= g_session->entries.size()) return {};
        entry& e = *g_session->entries[index];
        cheats::row& r = g_session->game.rows[e.row];
        on = !e.on.load();
        r.on = on;
        e.on = on;
        // the tick resets its own state when it sees the rearm
        if (on) e.rearm = true;
        state = cheats::state_text(g_session->game);
        serial = g_session->game.serial;
        name = e.name;
    }
    Emu.CallFromMainThread([serial = std::move(serial), state = std::move(state)]()
    {
        if (!cheats::detail::write_file(cheats::path_for(serial, ".state"), state))
            ps4_startup_checkpoint(fmt::format("cheats: cannot write %s.state: %s", serial, fs::g_tls_error).c_str());
    }, nullptr, false);
    return fmt::format("cheat '%s' switched %s in the home menu", name, on ? "on" : "off");
}
} // namespace ps4_cheats
