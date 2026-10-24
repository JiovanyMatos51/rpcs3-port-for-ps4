// Host checks of two PS4 thread paths:
//  1. util/ps4_thread_priority.hpp: native priority direction, bounds, restoration and API failures.
//  2. Utilities/Thread.h (RPCS3_PS4): the stand-by choice of service threads. g_fxo builds them with the
//     launch_retainer constructor and starts them with '= thread_state::created'; idle services (ps4_service_idle) and,
//     with lazy service threads on, ps4_lazy contexts stay stand-by until their first use, while PPU/SPU contexts and
//     objects built with arguments (guest threads, savestates) are never affected. Thread.cpp is not built here: the
//     out-of-line thread_base members are stubs that count starts.
//  3. 0.40: NP Handler Thread stays stand-by while PSN is not RPCN (pure ps4_np_handler_idle of Thread.h).
#define RPCS3_PS4 1

#include "util/ps4_thread_priority.hpp"
#include "Utilities/Thread.h"
#include "util/fixed_typemap.hpp"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

using namespace std::literals;

namespace
{
int g_failures = 0;

#define CHECK(cond) \
    do \
    { \
        if (!(cond)) \
        { \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failures++; \
        } \
    } while (0)

// Mock configuration of the decisions Thread.cpp makes from g_cfg and the safe-mode rows
bool g_skip_idle = true;    // row idle_services_skip (keep-service-threads.flag turns it off)
bool g_lazy = false;        // row lazy_services (opt-in)
bool g_every_name_idle = false;
bool g_psn_rpcn = false;    // g_cfg.net.psn_status == psn_rpcn
std::vector<std::string> g_idle_asked;

// Counters of the stubbed thread_base
int g_starts = 0;       // must-succeed start()
int g_try_starts = 0;   // try_start_native()
int g_lazy_starts = 0;  // ps4_start_lazy() that started a thread
bool g_refuse = false;  // try_start_native() refuses

struct idle_ctx
{
    static constexpr auto thread_name = "Idle Service"sv;
    idle_ctx() = default;
    explicit idle_ctx(int) {}
    void operator()() {}
};

struct busy_ctx
{
    static constexpr auto thread_name = "Busy Service"sv;
    void operator()() {}
};

struct lazy_ctx
{
    static constexpr auto thread_name = "Lazy Service"sv;
    static constexpr bool ps4_lazy = true;
    void operator()() {}
};

// GemUpdateThread / RsxAudio Backend: a lazy context whose body is idle in some configurations
struct lazy_idle_ctx
{
    static constexpr auto thread_name = "Idle Lazy Service"sv;
    static constexpr bool ps4_lazy = true;
    void operator()() {}
};

// Like named_thread<np::np_handler> (Emu/NP/np_handler.h): a fixed std::string_view name, a default constructor for a
// boot and a serializer constructor for a savestate restore (an int here)
struct np_ctx
{
    static constexpr std::string_view thread_name = "NP Handler Thread";
    int restored = 0;
    np_ctx() = default;
    explicit np_ctx(int ar) : restored(ar) {}
    void operator()() {}
};

// Like ppu_thread / spu_thread: each instance names itself; the name is not a fixed string
struct guest_ctx
{
    const struct thread_name_t
    {
        const guest_ctx* _this;
        operator std::string() const { return "PPU[0x1000000] main_thread"; }
    } thread_name{this};

    int arg = 0;
    guest_ctx() = default;
    explicit guest_ctx(int a) : arg(a) {}
    void operator()() {}
};
} // namespace

// ---- Thread.cpp decisions (mocked) ----

bool ps4_service_idle(std::string_view name) noexcept
{
    g_idle_asked.emplace_back(name);
    if (name == "NP Handler Thread"sv)
    {
        // The branch of Thread.cpp: the reason of the pure helper decides
        return g_skip_idle && ps4_np_handler_idle(g_psn_rpcn) != nullptr;
    }
    return g_skip_idle && (g_every_name_idle || name == "Idle Service"sv || name == "Idle Lazy Service"sv);
}

bool ps4_lazy_fxo_threads_enabled() noexcept
{
    return g_lazy;
}

// ---- Out-of-line parts of Utilities/Thread.h and util/atomic.hpp (stubs) ----

namespace stx
{
atomic_t<u32> g_launch_retainer{0};
}

void atomic_wait_engine::wait(const void*, u32, u64, atomic_wait::info*)
{
}

void atomic_wait_engine::notify_one(const void*)
{
}

void atomic_wait_engine::notify_all(const void*)
{
}

[[noreturn]] void fmt::raw_verify_error(std::source_location loc, const char8_t*, usz)
{
    std::printf("FAIL: ensure() at %s:%u\n", loc.file_name(), static_cast<unsigned>(loc.line()));
    std::exit(2);
}

thread_base::thread_base(native_entry entry, std::string) noexcept
    : entry_point(entry)
{
}

thread_base::~thread_base() noexcept
{
}

void thread_base::start()
{
    g_starts++;
    m_sync = (m_sync & ~3u) | static_cast<u32>(thread_state::created);
    m_thread = 0x1000 + g_starts;
}

bool thread_base::try_start_native() noexcept
{
    g_try_starts++;

    if (g_refuse)
    {
        m_sync = (m_sync & ~3u) | static_cast<u32>(thread_state::finished);
        return false;
    }

    m_sync = (m_sync & ~3u) | static_cast<u32>(thread_state::created);
    m_thread = 0x2000 + g_try_starts;
    return true;
}

// The same states as Thread.cpp (0 started or not lazy, 1 stand-by, 2 being started)
bool thread_base::ps4_start_lazy() noexcept
{
    if (m_ps4_lazy == 0)
    {
        return true;
    }

    m_ps4_lazy = 2;
    const bool started = try_start_native();
    m_ps4_lazy = started ? 0 : 1;
    g_lazy_starts += started;
    return started;
}

bool thread_base::join(bool) const
{
    return true;
}

void thread_base::initialize(void (*)())
{
}

u64 thread_base::finalize(thread_state) noexcept
{
    return 0;
}

thread_base::native_entry thread_base::make_trampoline(u64 (*)(thread_base*))
{
    return nullptr;
}

namespace
{
void reset()
{
    g_skip_idle = true;
    g_lazy = false;
    g_every_name_idle = false;
    g_psn_rpcn = false;
    g_idle_asked.clear();
    g_starts = g_try_starts = g_lazy_starts = 0;
    g_refuse = false;
}

bool asked(std::string_view name)
{
    for (const auto& n : g_idle_asked)
        if (n == name) return true;
    return false;
}

template <typename T>
bool finished(const named_thread<T>& t)
{
    return static_cast<thread_state>(t) == thread_state::finished;
}

void check_standby()
{
    // g_fxo build of an idle service: stand-by, the launch loop's '= created' starts nothing
    reset();
    {
        named_thread<idle_ctx> t(stx::launch_retainer{});
        CHECK(asked("Idle Service"));
        CHECK(t.ps4_lazy_standby());
        CHECK(finished(t));
        t = thread_state::created;
        CHECK(g_starts == 0 && g_try_starts == 0);
        CHECK(t.ps4_lazy_standby() && finished(t));
        // a first use (ps4_start_lazy, also what push() calls) still starts it
        CHECK(t.ps4_start_lazy());
        CHECK(g_lazy_starts == 1 && !t.ps4_lazy_standby());
        // started by its use before the launch loop: '= created' must not start it again
        t = thread_state::created;
        CHECK(g_starts == 0 && g_try_starts == 1);
    }

    // keep-service-threads.flag (row idle_services_skip off): started at boot, as in 0.38
    reset();
    g_skip_idle = false;
    {
        named_thread<idle_ctx> t(stx::launch_retainer{});
        CHECK(!t.ps4_lazy_standby());
        t = thread_state::created;
        CHECK(g_starts == 1);
    }

    // a service that is not idle starts at the launch loop, as in 0.38
    reset();
    {
        named_thread<busy_ctx> t(stx::launch_retainer{});
        CHECK(asked("Busy Service"));
        CHECK(!t.ps4_lazy_standby());
        t = thread_state::created;
        CHECK(g_starts == 1);
    }

    // lazy contexts: started at boot unless lazy service threads are on (opt-in)
    reset();
    {
        named_thread<lazy_ctx> t(stx::launch_retainer{});
        CHECK(!t.ps4_lazy_standby());
        t = thread_state::created;
        CHECK(g_starts == 1);
    }

    reset();
    g_lazy = true;
    {
        named_thread<lazy_ctx> t(stx::launch_retainer{});
        CHECK(t.ps4_lazy_standby());
        t = thread_state::created;
        CHECK(g_starts == 0);
        // a refused first use leaves it stand-by for the next use
        g_refuse = true;
        CHECK(!t.ps4_start_lazy());
        CHECK(t.ps4_lazy_standby() && finished(t));
        g_refuse = false;
        CHECK(t.ps4_start_lazy());
        CHECK(!t.ps4_lazy_standby() && g_lazy_starts == 1);
    }

    // a lazy context that is idle in this configuration (GemUpdateThread without a real Move handler) stays stand-by
    // even without lazy service threads; in the configuration where it works it starts at boot
    reset();
    {
        named_thread<lazy_idle_ctx> t(stx::launch_retainer{});
        CHECK(t.ps4_lazy_standby());
        t = thread_state::created;
        CHECK(g_starts == 0);
    }

    reset();
    g_skip_idle = false;
    {
        named_thread<lazy_idle_ctx> t(stx::launch_retainer{});
        CHECK(!t.ps4_lazy_standby());
        t = thread_state::created;
        CHECK(g_starts == 1);
    }

    // PPU/SPU-like contexts (per-instance name) built with their parameters: never asked, never stand-by, started by
    // ps4_try_start as before; even with every name 'idle' and lazy threads on
    reset();
    g_every_name_idle = true;
    g_lazy = true;
    {
        named_thread<guest_ctx> t(stx::launch_retainer{}, 5);
        CHECK(g_idle_asked.empty());
        CHECK(!t.ps4_lazy_standby());
        CHECK(t.arg == 5);
        CHECK(t.ps4_try_start());
        CHECK(g_try_starts == 1 && g_starts == 0);
    }

    // without parameters a per-instance name is still never a service name
    reset();
    g_every_name_idle = true;
    {
        named_thread<guest_ctx> t(stx::launch_retainer{});
        CHECK(g_idle_asked.empty());
        CHECK(!t.ps4_lazy_standby());
    }

    // an object built with arguments (a savestate restore passes the serializer) starts at the launch loop
    reset();
    {
        named_thread<idle_ctx> t(stx::launch_retainer{}, 1);
        CHECK(g_idle_asked.empty());
        CHECK(!t.ps4_lazy_standby());
        t = thread_state::created;
        CHECK(g_starts == 1);
    }

    // default-name constructor (g_fxo->need/init<T>(), direct construction): idle services stand-by, others start
    reset();
    {
        named_thread<idle_ctx> idle;
        CHECK(idle.ps4_lazy_standby() && g_starts == 0);
        named_thread<busy_ctx> busy;
        CHECK(!busy.ps4_lazy_standby() && g_starts == 1);
        named_thread<lazy_ctx> lazy_off;
        CHECK(!lazy_off.ps4_lazy_standby() && g_starts == 2);
        g_lazy = true;
        named_thread<lazy_ctx> lazy_on;
        CHECK(lazy_on.ps4_lazy_standby() && g_starts == 2);
        // a lazy context that is idle in this configuration: stand-by with lazy service threads off, as with g_fxo
        g_lazy = false;
        named_thread<lazy_idle_ctx> lazy_idle;
        CHECK(lazy_idle.ps4_lazy_standby() && g_starts == 2);
        CHECK(asked("Idle Lazy Service"));
    }
}

// NP Handler Thread: its body returns at once unless PSN is RPCN, so a boot without RPCN keeps it stand-by
void check_np_handler()
{
    static_assert(ps4_np_handler_idle(true) == nullptr);
    static_assert(ps4_np_handler_idle(false) != nullptr);
    CHECK(std::string_view(ps4_np_handler_idle(false)) == "PSN not RPCN"sv);

    // offline / PSN disabled / fake PSN: stand-by at the g_fxo build, the launch loop's '= created' starts nothing
    reset();
    {
        named_thread<np_ctx> t(stx::launch_retainer{});
        CHECK(asked("NP Handler Thread"));
        CHECK(t.ps4_lazy_standby() && finished(t));
        t = thread_state::created;
        CHECK(g_starts == 0 && g_try_starts == 0);
        // the stop's '= aborting' / '= finished' on the never-started object waits for nothing
        t = thread_state::aborting;
        t = thread_state::finished;
        CHECK(g_starts == 0 && finished(t));
    }

    // RPCN: started at boot, as before
    reset();
    g_psn_rpcn = true;
    {
        named_thread<np_ctx> t(stx::launch_retainer{});
        CHECK(asked("NP Handler Thread"));
        CHECK(!t.ps4_lazy_standby());
        t = thread_state::created;
        CHECK(g_starts == 1);
    }

    // keep-service-threads.flag (row idle_services_skip off): started at boot without RPCN too, as in 0.39
    reset();
    g_skip_idle = false;
    {
        named_thread<np_ctx> t(stx::launch_retainer{});
        CHECK(!t.ps4_lazy_standby());
        t = thread_state::created;
        CHECK(g_starts == 1);
    }

    // a savestate restore builds it with the serializer: not asked, started by the launch loop as before
    reset();
    {
        named_thread<np_ctx> t(stx::launch_retainer{}, 7);
        CHECK(g_idle_asked.empty());
        CHECK(t.restored == 7 && !t.ps4_lazy_standby());
        t = thread_state::created;
        CHECK(g_starts == 1);
    }

    // the default-name constructor makes the same choice
    reset();
    {
        named_thread<np_ctx> t;
        CHECK(asked("NP Handler Thread") && t.ps4_lazy_standby() && g_starts == 0);
        g_psn_rpcn = true;
        named_thread<np_ctx> online;
        CHECK(!online.ps4_lazy_standby() && g_starts == 1);
    }
}

void check_priority()
{
    utils::ps4::thread_priority priority;
    int running = 700, reads = 0, writes = 0, get_error = 0, set_error = 0;
    const auto get = [&](int* result)
    {
        ++reads;
        if (get_error) return get_error;
        *result = running;
        return 0;
    };
    const auto set = [&](int value)
    {
        ++writes;
        if (set_error) return set_error;
        running = value;
        return 0;
    };
    // Native scheduler chooses lower numeric priorities first. A raised RSX
    // must precede a guest at 700 and restoration must make them peers again.
    CHECK(priority.apply(0, get, set) == 0 && reads == 0 && writes == 0);
    CHECK(priority.apply(1, get, set) == 0 && running < 700 && running > 256);
    CHECK(priority.apply(1, get, set) == 0 && reads == 1);
    CHECK(priority.apply(0, get, set) == 0 && running == 700);
    CHECK(priority.apply(-1, get, set) == 0 && running > 700 && running < 767);
    CHECK(priority.apply(0, get, set) == 0 && running == 700);
    running = 259;
    CHECK(priority.apply(1, get, set) == 0 && running == 256);
    CHECK(priority.apply(0, get, set) == 0 && running == 259);
    running = 764;
    CHECK(priority.apply(-1, get, set) == 0 && running == 767);
    CHECK(priority.apply(0, get, set) == 0 && running == 764);
    running = 700;
    get_error = 3;
    const int before = writes;
    CHECK(priority.apply(1, get, set) == 3 && writes == before && running == 700);
    get_error = 0;
    set_error = 4;
    CHECK(priority.apply(1, get, set) == 4 && running == 700);
    set_error = 0;
    CHECK(priority.apply(1, get, set) == 0 && running < 700);
    set_error = 5;
    CHECK(priority.apply(0, get, set) == 5 && running < 700);
    set_error = 0;
    CHECK(priority.apply(0, get, set) == 0 && running == 700);
}
} // namespace

int main()
{
    check_priority();
    if (!g_failures) std::cout << "PASS: native priority direction, bounds, restoration and API failures\n";
    const int before = g_failures;
    check_standby();
    if (g_failures == before) std::cout << "PASS: service thread stand-by (idle services, opt-in lazy services; PPU/SPU and argument builds unaffected)\n";
    const int before_np = g_failures;
    check_np_handler();
    if (g_failures == before_np) std::cout << "PASS: NP Handler Thread stand-by while PSN is not RPCN (RPCN, keep-service-threads.flag and savestate restores start it)\n";
    if (g_failures) std::cout << "thread_priority_test: " << g_failures << " failure(s)\n";
    return g_failures ? 1 : 0;
}
