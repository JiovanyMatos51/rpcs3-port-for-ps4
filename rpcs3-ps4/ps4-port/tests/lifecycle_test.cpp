#include "util/ps4_lifecycle.hpp"
#include "ps4_fatal.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

utils::ps4::lifecycle state;

// logs.cpp keeps its limiter in a static that log lines written before main reach: it must be constant-initialized
constinit utils::ps4::repeat_limiter g_constant_initialized_limiter;

namespace
{
void test_lifecycle()
{
    using action = utils::ps4::lifecycle::action;
    assert(state.poll(true) == action::none);
    assert(state.poll(false) == action::none);
    state.request_exit();
    assert(state.requested());
    assert(state.poll(false) == action::stop_emulation);
    std::thread input([] { for (int i = 0; i < 100; ++i) state.request_exit(); });
    for (int i = 0; i < 100; ++i) assert(state.poll(false) == action::none);
    input.join();
    assert(state.poll(true) == action::exit_process);
    utils::ps4::lifecycle menu_exit;
    menu_exit.request_exit();
    assert(menu_exit.poll(true) == action::exit_process);
    utils::ps4::lifecycle returning;
    for (int game = 0; game < 3; ++game)
    {
        assert(returning.poll(false) == action::none);
        returning.request_menu();
        assert(!returning.exiting());
        assert(returning.poll(false) == action::stop_emulation);
        std::thread repeated([&] { for (int i = 0; i < 100; ++i) returning.request_menu(); });
        for (int i = 0; i < 100; ++i) assert(returning.poll(false) == action::none);
        repeated.join();
        assert(returning.poll(true) == action::return_to_menu);
        assert(returning.consume_menu_request());
        assert(!returning.requested());
    }
    // Process exit wins even if a menu request races with shutdown.
    returning.request_menu();
    assert(returning.poll(false) == action::stop_emulation);
    returning.request_exit();
    returning.request_menu();
    assert(returning.exiting());
    assert(!returning.consume_menu_request());
    assert(returning.poll(false) == action::none);
    assert(returning.poll(true) == action::exit_process);
    utils::ps4::lifecycle natural_exit;
    natural_exit.request_menu();
    assert(natural_exit.poll(true) == action::return_to_menu);
    assert(natural_exit.consume_menu_request());
}

void test_stall_watchdog()
{
    using wd = utils::ps4::stall_watchdog;
    constexpr std::uint64_t s = 1'000'000;
    wd w;
    // Not running: never dumps, whatever the clock says.
    for (std::uint64_t t = 0; t < 1000 * s; t += 10 * s) assert(!w.poll(false, t, 7));
    static_assert(wd::first_dump_us == 25 * s && wd::second_dump_us == 10 * s && wd::repeat_us == 120 * s);
    // The first dump comes before the 30 s 'game looks frozen' toast, the second one 10 s later.
    static_assert(wd::first_dump_us < 30 * s && wd::first_dump_us + wd::second_dump_us <= 35 * s);
    // Guest starts at 100 s: first dump exactly 25 s later without activity.
    assert(!w.poll(true, 100 * s, 1));
    assert(!w.poll(true, 124 * s, 1));
    assert(w.stalled_us(124 * s) == 24 * s);
    assert(w.poll(true, 125 * s, 1));
    assert(w.dumps() == 1);
    // The second one 10 s later.
    assert(!w.poll(true, 134 * s, 1));
    assert(w.poll(true, 135 * s, 1));
    assert(w.dumps() == 2);
    // Then every 120 s.
    assert(!w.poll(true, 200 * s, 1));
    assert(!w.poll(true, 254 * s, 1));
    assert(w.poll(true, 255 * s, 1));
    assert(!w.poll(true, 265 * s, 1));
    assert(w.dumps() == 3);
    // A flip re-arms the 25 s clock and the 10 s second dump of the new episode, but keeps the per-boot budget.
    assert(!w.poll(true, 266 * s, 2));
    assert(!w.poll(true, 290 * s, 2));
    assert(w.poll(true, 291 * s, 2));
    assert(w.dumps() == 4);
    // Pause disarms; resuming restarts the clock from the resume time.
    assert(!w.poll(false, 295 * s, 2));
    assert(w.stalled_us(296 * s) == 0);
    assert(!w.poll(true, 1000 * s, 2));
    assert(!w.poll(true, 1024 * s, 2));
    assert(w.poll(true, 1025 * s, 2));
    assert(w.dumps() == wd::max_dumps);
    // Budget exhausted: no more dumps this boot.
    for (std::uint64_t t = 1026 * s; t < 5000 * s; t += 5 * s) assert(!w.poll(true, t, 2));
    // A new boot re-arms the budget and the short second gap.
    w.reset_boot();
    assert(w.dumps() == 0);
    assert(!w.poll(true, 6000 * s, 9));
    assert(w.poll(true, 6025 * s, 9));
    assert(w.poll(true, 6035 * s, 9));
    assert(!w.poll(true, 6154 * s, 9));
    assert(w.poll(true, 6155 * s, 9));
    // A clock that jumps far ahead still yields one dump per poll.
    wd late;
    assert(!late.poll(true, 0, 3));
    assert(late.poll(true, 10'000 * s, 3));
    assert(!late.poll(true, 10'000 * s, 3));
    // Continuous flips never dump.
    wd healthy;
    for (std::uint64_t i = 0; i < 10000; ++i) assert(!healthy.poll(true, i * s, i));
}

void test_fixed_text()
{
    ps4_fatal_detail::fixed_text<16> t;
    t.s("abc").u(0).s(" ").x(0x1f);
    assert(std::strcmp(t.b, "abc0 0x1f") == 0);
    t.s("0123456789abcdef0123");
    assert(t.n == 15 && t.b[15] == 0 && t.full());
    t.u(123456789).x(~0ull).s(std::string_view{"more"});
    assert(t.n == 15 && t.b[15] == 0);
    ps4_fatal_detail::fixed_text<64> big;
    big.u(18446744073709551615ull).s(" ").x(0);
    assert(std::strcmp(big.b, "18446744073709551615 0x0") == 0);
    big.s(nullptr);
    assert(std::strcmp(big.b, "18446744073709551615 0x0") == 0);
}

void test_previous_fatal_notice()
{
    using ps4_fatal_detail::previous_fatal_notice;
    assert(previous_fatal_notice("").empty());
    assert(previous_fatal_notice("\r\n \n").empty());
    // 0.24 and older: free-form text.
    assert(previous_fatal_notice("RPCS3 has abnormally terminated.") ==
        "Sessão anterior terminou com erro: RPCS3 has abnormally terminated.");
    // 0.25 record: reason and exception, not the header or backtrace.
    const std::string record =
        "RPCS3 PS4 EDITION 0.25 fatal error\norigin: terminate\nreason: std::terminate (uncaught)\r\n"
        "thread: PPU[0x1000000] tid=0x1\nuptime_us: 5\nexception: type=St9bad_alloc what=std::bad_alloc\n"
        "backtrace (offsets):\n  #0 +0x10\n";
    assert(previous_fatal_notice(record) ==
        "Sessão anterior terminou com erro: std::terminate (uncaught) | type=St9bad_alloc what=std::bad_alloc");
    // Long text is cut at a UTF-8 boundary with an ellipsis.
    std::string longer = "reason: ";
    for (int i = 0; i < 100; ++i) longer += "é";
    const std::string notice = previous_fatal_notice(longer);
    assert(notice.size() < 140);
    assert(notice.ends_with("..."));
    const std::string body = notice.substr(std::strlen("Sessão anterior terminou com erro: "));
    assert(body.size() == 80 + 3); // 40 two-byte characters + "..."
}

void test_history_filter()
{
    using utils::ps4::keep_history_line;
    // Level marker U+00B7 (C2 B7) then the level letter
    assert(keep_history_line("\xc2\xb7" "E 0:00:01.000000 {PPU} sys_fs: something failed\n"));
    assert(keep_history_line("\xc2\xb7" "F 0:00:01.000000 {RSX} VK: fatal\n"));
    assert(keep_history_line("\xc2\xb7" "U 0:00:01.000000 {SPU} cellGem: TODO\n"));
    assert(!keep_history_line("\xc2\xb7" "U 0:00:01.000000 {SPUW.1} SPU: [0x1:0x00040] Unmatched spu_re(a) found in FMA\n"));
    assert(keep_history_line("\xc2\xb7" "W 0:00:01.000000 {Main} PS4: shader interpreter warmup replaced\n"));
    assert(keep_history_line("\xc2\xb7" "! 0:00:01.000000 {cellAudio} AudioOut: backend recovered\n"));
    assert(keep_history_line("\xc2\xb7" "! 0:00:01.000000 {Main} focus: home screen\n"));
    assert(!keep_history_line("\xc2\xb7" "W 0:00:01.000000 {PPU} sys_fs: plain warning\n"));
    assert(!keep_history_line(""));

    const std::string err = "\xc2\xb7" "E error line\n";
    const std::string note = "\xc2\xb7" "! notice line\n";
    // The first line is the end of a line cut by the head copy: dropped even when it matches.
    const std::string input = "tail of PS4 cut line\n" + err + note + err + "\xc2\xb7" "W PS4: x\n" + note + "\xc2\xb7" "E no newline at the end";
    utils::ps4::history_line_filter whole(1 << 20);
    whole.feed(input.data(), input.size());
    const std::string expected = err + err + "\xc2\xb7" "W PS4: x\n";
    assert(whole.kept() == expected);
    assert(whole.matched() == 3);
    // Any chunking gives the same result.
    for (std::size_t step : {1u, 2u, 3u, 7u, 64u})
    {
        utils::ps4::history_line_filter chunked(1 << 20);
        for (std::size_t at = 0; at < input.size(); at += step) chunked.feed(input.data() + at, std::min(step, input.size() - at));
        assert(chunked.kept() == expected && chunked.matched() == 3);
    }
    // Over the budget the newest whole lines stay, within the budget.
    utils::ps4::history_line_filter bounded(350);
    std::string lines = "x\n";
    for (int i = 0; i < 10; ++i)
    {
        std::string line = "\xc2\xb7" "E line " + std::to_string(i) + " ";
        line.resize(99, '.');
        lines += line + "\n";
    }
    bounded.feed(lines.data(), lines.size());
    const std::string& newest = bounded.kept();
    assert(bounded.matched() == 10);
    assert(newest.size() == 300 && newest.starts_with("\xc2\xb7" "E line 7 ") && newest.ends_with(".\n"));
    // A very long line is kept cut at 4 KiB (plus its newline); memory stays bounded.
    utils::ps4::history_line_filter long_line(1 << 20);
    std::string huge = "x\n\xc2\xb7" "E " + std::string(10000, 'a') + "\n";
    long_line.feed(huge.data(), huge.size());
    assert(long_line.kept().size() == 4097 && long_line.kept().back() == '\n');
    // Nothing to keep.
    utils::ps4::history_line_filter empty(100);
    empty.feed("", 0);
    assert(empty.kept().empty() && empty.matched() == 0);
}

void test_history_todo_dedupe()
{
    using utils::ps4::history_line_filter;
    const auto todo = [](int second, const char* thread, const char* text)
    {
        return std::string("\xc2\xb7" "U 0:00:") + (second < 10 ? "0" : "") + std::to_string(second) + ".123456 {" + thread + "} " + text + "\n";
    };
    // 10 identical TODO texts with other time stamps and threads keep 3; every distinct text is kept
    std::string input = "cut line\n";
    for (int i = 0; i < 10; ++i) input += todo(i, i % 2 ? "PPU[0x1000020] Thread (PoolThread) [HLE:0x0001]" : "PPU[0x1000021] Thread (main)", "cellAudio: TODO: cellAudioPortOpen(): CELL_AUDIO_PORTATTR_OUT_SECONDARY");
    for (int i = 0; i < 5; ++i) input += todo(20 + i, "PPU[0x1000020] Thread (PoolThread)", ("cellMic: TODO: distinct text " + std::to_string(i)).c_str());
    input += "\xc2\xb7" "E 0:00:30.000000 {PPU} an error line\n";
    // the same TODO text again later still counts as a repeat
    input += todo(31, "PPU[0x1000022] Thread (other)", "cellAudio: TODO: cellAudioPortOpen(): CELL_AUDIO_PORTATTR_OUT_SECONDARY");
    // a TODO without a thread block: the text after the time stamp
    for (int i = 0; i < 5; ++i) input += "\xc2\xb7" "U 0:00:4" + std::to_string(i) + ".000000 bare: TODO: no thread block\n";
    for (std::size_t step : {std::size_t(1), std::size_t(5), input.size()})
    {
        history_line_filter f(1 << 20);
        for (std::size_t at = 0; at < input.size(); at += step) f.feed(input.data() + at, std::min(step, input.size() - at));
        const std::string& kept = f.kept();
        std::size_t audio = 0, mic = 0, bare = 0, errors = 0;
        for (std::size_t at = 0; (at = kept.find("cellAudioPortOpen", at)) != std::string::npos; ++at) audio++;
        for (std::size_t at = 0; (at = kept.find("distinct text", at)) != std::string::npos; ++at) mic++;
        for (std::size_t at = 0; (at = kept.find("no thread block", at)) != std::string::npos; ++at) bare++;
        for (std::size_t at = 0; (at = kept.find("an error line", at)) != std::string::npos; ++at) errors++;
        assert(audio == history_line_filter::todo_keep && mic == 5 && bare == 3 && errors == 1);
        assert(f.repeats() == 7 + 1 + 2);
        assert(f.matched() == 3 + 5 + 1 + 3);
    }
    // Other levels repeat as before (errors are never thinned out)
    history_line_filter errors(1 << 20);
    std::string same = "x\n";
    for (int i = 0; i < 6; ++i) same += "\xc2\xb7" "E 0:00:01.000000 {PPU} same error\n";
    errors.feed(same.data(), same.size());
    assert(errors.matched() == 6 && errors.repeats() == 0);
}

void test_quiet_clock()
{
    using utils::ps4::quiet_clock;
    constexpr std::uint64_t s = 1'000'000;
    static_assert(quiet_clock::first_mark_us == 60 * s && quiet_clock::second_mark_us == 300 * s);
    quiet_clock q;
    assert(q.poll(true, 10 * s, 5) == 0);
    assert(q.poll(true, 69 * s, 5) == 0);
    assert(q.quiet_us(69 * s) == 59 * s);
    assert(q.poll(true, 70 * s, 5) == 1);
    assert(q.poll(true, 71 * s, 5) == 0);
    assert(q.poll(true, 309 * s, 5) == 0);
    assert(q.poll(true, 310 * s, 5) == 2);
    for (std::uint64_t t = 311 * s; t < 2000 * s; t += 7 * s) assert(q.poll(true, t, 5) == 0); // each mark once per stretch
    // Activity starts a new stretch; a pause disarms
    assert(q.poll(true, 2000 * s, 6) == 0);
    assert(q.poll(true, 2059 * s, 6) == 0);
    assert(q.poll(false, 2061 * s, 6) == 0);
    assert(q.quiet_us(2062 * s) == 0);
    assert(q.poll(true, 2100 * s, 6) == 0);
    assert(q.poll(true, 2160 * s, 6) == 1);
    // A clock that jumps gives one mark per call, in order
    quiet_clock jump;
    assert(jump.poll(true, 0, 1) == 0);
    assert(jump.poll(true, 1000 * s, 1) == 1);
    assert(jump.poll(true, 1000 * s, 1) == 2);
    assert(jump.poll(true, 1001 * s, 1) == 0);
    // Activity that changes every pass (an excluded wait) never reaches a mark
    quiet_clock busy;
    for (std::uint64_t i = 0; i < 1000; ++i) assert(busy.poll(true, i * s, 7 ^ i) == 0);
    busy.reset_boot();
    assert(busy.quiet_us(5 * s) == 0);
}

void test_empty_frames_watch()
{
    using utils::ps4::empty_frames_watch;
    constexpr std::uint64_t s = 1'000'000;
    empty_frames_watch w;
    // Draws or an excluded sample never start a stretch
    for (std::uint64_t t = 0; t < 500; ++t) assert(w.poll(true, t * s, true, true) == 0);
    for (std::uint64_t t = 500; t < 1000; ++t) assert(w.poll(false, t * s, true, false) == 0);
    // Flips without draws: a dump at 60 s, the failure at 120 s, once each
    assert(w.poll(true, 1000 * s, true, false) == 0);
    for (std::uint64_t t = 1001; t < 1060; ++t) assert(w.poll(true, t * s, t % 3 != 0, false) == 0); // a sample without a flip keeps it
    assert(w.empty_us(1059 * s) == 59 * s);
    assert(w.poll(true, 1060 * s, true, false) == 1);
    for (std::uint64_t t = 1061; t < 1120; ++t) assert(w.poll(true, t * s, true, false) == 0);
    assert(w.poll(true, 1120 * s, true, false) == 2);
    for (std::uint64_t t = 1121; t < 1300; ++t) assert(w.poll(true, t * s, true, false) == 0);
    // One draw ends it; the next stretch starts at its first empty flip
    assert(w.poll(true, 1300 * s, true, true) == 0);
    assert(w.empty_us(1300 * s) == 0);
    assert(w.poll(true, 1301 * s, false, false) == 0); // no flip: nothing starts
    assert(w.poll(true, 1302 * s, true, false) == 0);
    assert(w.poll(true, 1361 * s, true, false) == 0);
    assert(w.poll(true, 1362 * s, true, false) == 1);
    // A dialog (excluded sample) in between ends the stretch
    assert(w.poll(false, 1363 * s, true, false) == 0);
    assert(w.poll(true, 1364 * s, true, false) == 0);
    assert(w.poll(true, 1423 * s, true, false) == 0);
    assert(w.poll(true, 1424 * s, true, false) == 1);
    w.reset_boot();
    assert(w.empty_us(1500 * s) == 0);
}

void test_session_record()
{
    namespace sr = utils::ps4::session_record;
    char line[256];
    assert(sr::format(line, sizeof(line), "boot", 0, 0, "Mortal Kombat\nKomplete Edition") > 0);
    assert(std::string(line) == "boot 0 0 Mortal Kombat Komplete Edition");
    sr::state st = sr::parse(line);
    assert(st.valid && !st.ended && st.seconds == 0 && st.draw_seconds == 0 && st.name == "Mortal Kombat Komplete Edition");
    assert(sr::hint_due(st));
    assert(sr::format(line, sizeof(line), "alive", 599, 12, "Demon's Souls") > 0);
    const std::string with_newline = std::string(line) + "\n"; // st.name views into it
    st = sr::parse(with_newline);
    assert(st.valid && st.seconds == 599 && st.draw_seconds == 12 && st.name == "Demon's Souls" && sr::hint_due(st));
    assert(sr::format(line, sizeof(line), "alive", 600, 590, "Demon's Souls") > 0);
    assert(!sr::hint_due(sr::parse(line))); // 10 minutes or more: no hint
    // Closed during a cache build (a progress text was shown): no hint, whatever the time
    assert(sr::format(line, sizeof(line), "compile", 120, 0, "Demon's Souls") > 0);
    st = sr::parse(line);
    assert(st.valid && st.compiling && !st.ended && st.seconds == 120 && st.name == "Demon's Souls" && !sr::hint_due(st));
    assert(!sr::parse("alive 5 0 x").compiling && sr::hint_due(sr::parse("alive 5 0 x")));
    assert(sr::parse("end").ended && !sr::hint_due(sr::parse("end crash\n")) && sr::parse("end threads").valid);
    assert(!sr::parse("").valid && !sr::parse("garbage 1 2 x").valid && !sr::parse("alive x 2 y").valid && !sr::parse("alive 1").valid);
    assert(!sr::hint_due(sr::parse("boot 12345678901234 0 x"))); // a number that does not fit is not valid
    // A name without spaces after the numbers is fine; an empty name too
    assert(sr::parse("boot 5 0 ").valid && sr::parse("boot 5 0 ").name.empty());
    // A long UTF-8 name is cut on a character boundary and fits the adapt record limit
    std::string longer;
    for (int i = 0; i < 100; ++i) longer += "é";
    const std::size_t n = sr::format(line, sizeof(line), "alive", 1, 1, longer);
    assert(n > 0 && n < 160 && (static_cast<unsigned char>(line[n - 1]) & 0xc0) != 0xc0);
    assert(sr::parse(line).name.size() % 2 == 0 && sr::parse(line).name.size() <= sr::name_bytes);
    // Too small a buffer gives nothing
    char tiny[4];
    assert(sr::format(tiny, sizeof(tiny), "alive", 123, 4, "x") == 0 && tiny[0] == 0);
}

struct fake_call
{
    const char* name;
    unsigned long long ret;
};

void test_call_pattern()
{
    using utils::ps4::recent_pattern;
    static const char open[] = "cellAudioPortOpen";
    static const char close[] = "cellAudioPortClose";
    static const char usleep[] = "sys_timer_usleep";
    std::vector<fake_call> calls;
    for (int i = 0; i < 16; ++i) calls.push_back({i % 2 ? open : close, i % 2 ? 0ull : 0x80310701ull});
    auto p = recent_pattern(calls.data(), static_cast<unsigned>(calls.size()));
    assert(p.distinct == 2 && p.pair && !p.same_error && p.ret == 0x80310701ull);
    assert((p.first == open && p.second == close) || (p.first == close && p.second == open));
    // Names compare by text too (two copies of one string)
    std::string copy = "cellAudioPortOpen";
    calls[1].name = copy.c_str();
    assert(recent_pattern(calls.data(), 16).distinct == 2);
    // Open, close and a sleep in between, equally often: the pair is still found, in the order of their counts
    std::vector<fake_call> three;
    for (int i = 0; i < 12; ++i) three.push_back({i % 3 == 0 ? usleep : i % 3 == 1 ? open : close, 0});
    three.push_back({close, 0});
    p = recent_pattern(three.data(), static_cast<unsigned>(three.size()));
    assert(p.distinct == 3 && p.pair && p.first == close && p.second == open);
    // Create/destroy with a lower-case suffix
    std::vector<fake_call> queue(10, fake_call{"sys_event_queue_create", 0});
    for (std::size_t i = 0; i < queue.size(); i += 2) queue[i].name = "sys_event_queue_destroy";
    assert(recent_pattern(queue.data(), 10).pair);
    // One name with one CELL error code over and over
    std::vector<fake_call> errors(12, fake_call{"cellGameBootCheck", 0xffffffff8002cb01ull});
    p = recent_pattern(errors.data(), 12);
    assert(p.distinct == 1 && !p.pair && p.same_error && p.second == nullptr);
    // A sleep loop is no pair and no error
    std::vector<fake_call> sleeps(32, fake_call{usleep, 0});
    p = recent_pattern(sleeps.data(), 32);
    assert(p.distinct == 1 && !p.pair && !p.same_error);
    // Lock/unlock and read are not opposite calls
    std::vector<fake_call> mix = {{"sys_lwmutex_lock", 0}, {"sys_lwmutex_unlock", 0}, {"sys_lwmutex_lock", 0}, {"sys_lwmutex_unlock", 0},
        {"sys_lwmutex_lock", 0}, {"sys_lwmutex_unlock", 0}, {"sys_lwmutex_lock", 0}, {"sys_lwmutex_unlock", 0}};
    assert(!recent_pattern(mix.data(), 8).pair);
    std::vector<fake_call> check = {{"cellGameDataCheck", 0}, {"cellGameDataClose", 0}, {"cellGameDataCheck", 0}, {"cellGameDataClose", 0},
        {"cellGameDataCheck", 0}, {"cellGameDataClose", 0}, {"cellGameDataCheck", 0}, {"cellGameDataClose", 0}};
    assert(!recent_pattern(check.data(), 8).pair);
    // Too short a window, an unknown name, or more than four names: nothing
    assert(recent_pattern(calls.data(), 7).distinct == 0);
    calls[3].name = nullptr;
    assert(recent_pattern(calls.data(), 16).distinct == 0 && recent_pattern(calls.data(), 16).first == nullptr);
    const char* many[] = {"a1", "a2", "a3", "a4", "a5", "a1", "a2", "a3"};
    std::vector<fake_call> wide;
    for (const char* m : many) wide.push_back({m, 0});
    p = recent_pattern(wide.data(), 8);
    assert(p.distinct == 5 && !p.pair && !p.first);
    assert(recent_pattern<fake_call>(nullptr, 20).distinct == 0);
}

void test_npd_license()
{
    std::vector<unsigned char> self(4096, 0xee);
    const std::size_t at = 0x2a0; // inside the SELF control info, not aligned on purpose
    const unsigned char header[] = {'N', 'P', 'D', 0, 0, 0, 0, 3, 0, 0, 0, 2, 0, 0, 0, 0};
    std::memcpy(&self[at + 1], header, sizeof(header));
    const char id[] = "UP0001-NPUB30554_00-0000000000000001";
    std::memset(&self[at + 1 + 0x10], 0, 0x30);
    std::memcpy(&self[at + 1 + 0x10], id, 36);
    const auto npd = utils::ps4::find_npd_license(self.data(), self.size());
    assert(npd.license == 2 && npd.content_id == id);
    assert(utils::ps4::license_needs_rap(1) && utils::ps4::license_needs_rap(2));
    assert(!utils::ps4::license_needs_rap(3) && !utils::ps4::license_needs_rap(0));
    // No header, or one too close to the end to hold the content id.
    std::vector<unsigned char> plain(4096, 0);
    assert(utils::ps4::find_npd_license(plain.data(), plain.size()).license == 0);
    std::memcpy(&plain[4096 - 0x20], header, sizeof(header));
    assert(utils::ps4::find_npd_license(plain.data(), plain.size()).license == 0);
    assert(utils::ps4::find_npd_license(plain.data(), 0).license == 0);
    // A content id filling all 0x30 bytes is taken whole (no NUL inside).
    std::vector<unsigned char> full(0x40, 'A');
    std::memcpy(full.data(), header, sizeof(header));
    assert(utils::ps4::find_npd_license(full.data(), full.size()).content_id == std::string(0x30, 'A'));
}

void test_switch_files()
{
    using utils::ps4::switch_file;
    using utils::ps4::test_switch_file;
    assert(switch_file("no-bg-pause.flag") && switch_file("boot.txt") && switch_file("mesa-env.txt"));
    assert(!switch_file("fatal.txt") && !switch_file("previous-fatal.txt") && !switch_file("startup.log") && !switch_file("config.yml"));
    assert(test_switch_file("test-io-fail.flag") && test_switch_file("test-heap-cap-mib.txt") && test_switch_file("refuse-threads.flag"));
    assert(test_switch_file("keep-user-settings.flag"));
    assert(!test_switch_file("no-bg-pause.flag") && !test_switch_file("test-notes.md") && !test_switch_file("cool-mode.flag"));
}

void test_first_boot_compile()
{
    using utils::ps4::compile_minutes_left;
    using utils::ps4::first_boot_toast_due;
    constexpr std::uint64_t s = 1'000'000;
    assert(compile_minutes_left(0, 4.) == 1);
    assert(compile_minutes_left(88, 4.) == 22);
    assert(compile_minutes_left(89, 4.) == 23);
    assert(compile_minutes_left(40, 0.) == 1);
    assert(compile_minutes_left(4'000'000'000u, 1e-9) == 9999);
    assert(!first_boot_toast_due(39, 39, 100 * s, true));
    assert(!first_boot_toast_due(40, 40, 29 * s, true));
    assert(first_boot_toast_due(40, 40, 30 * s, true));
    assert(!first_boot_toast_due(40, 40, 30 * s, false));
    assert(!first_boot_toast_due(40, 40, 89 * s, false));
    assert(first_boot_toast_due(40, 40, 90 * s, false));
    assert(!first_boot_toast_due(225, 0, 90 * s, true));
}

void test_heartbeat_texts()
{
    using utils::ps4::progress_stage;
    using utils::ps4::progress_stage_text;
    assert(progress_stage_text(progress_stage::none, 1, 2, 3).empty());
    assert(progress_stage_text(progress_stage::other, 1, 2, 3).empty());
    assert(progress_stage_text(progress_stage::ppu_compile, 12, 88, 20) == "compiling the game (PPU 12 of 88 modules, ~20 min left: do not close)");
    assert(progress_stage_text(progress_stage::ppu_compile, 12, 88, 0) == "compiling the game (PPU 12 of 88 modules: do not close)");
    assert(progress_stage_text(progress_stage::ppu_compile, 90, 88, 0) == "compiling the game (PPU 88 of 88 modules: do not close)");
    assert(progress_stage_text(progress_stage::ppu_compile, 0, 0, 5) == "compiling the game (PPU modules: do not close)");
    assert(progress_stage_text(progress_stage::ppu_prepare, 3, 7, 0) == "preparing the game's PPU code (3 of 7: do not close)");
    assert(progress_stage_text(progress_stage::ppu_prepare, 0, 0, 0) == "preparing the game's PPU code (do not close)");
    assert(progress_stage_text(progress_stage::spu_cache, 100, 3370, 9) == "building the SPU cache (100 of 3370 programs: do not close)");
    assert(progress_stage_text(progress_stage::spu_cache, 0, 0, 0) == "building the SPU cache (do not close)");
    assert(progress_stage_text(progress_stage::shader_cache, 0, 0, 0) == "loading the shader cache (do not close)");
    using utils::ps4::heartbeat_draws_text;
    assert(heartbeat_draws_text(0, 0) == "draws not measured");
    assert(heartbeat_draws_text(5, 0) == "draws not measured");
    assert(heartbeat_draws_text(57, 60) == "draws>0 for 57 of 60 s");
    assert(heartbeat_draws_text(0, 60) == "draws>0 for 0 of 60 s");
    assert(heartbeat_draws_text(70, 60) == "draws>0 for 60 of 60 s");
}

void test_repeat_limiter()
{
    using utils::ps4::repeat_limiter;
    constexpr std::uint64_t s = 1'000'000;
    static repeat_limiter limiter; // static: 512 slots, as in logs.cpp
    static const char flood[] = "Transfer cropped in Y, src_h=%d, dst_h=%d";
    static const char other[] = "Image at address 0x%x has invalid dimensions";
    static const char bare[] = "%s (%s)";
    const int tag = 0;
    std::vector<std::pair<std::string, std::uint32_t>> summaries;
    std::vector<std::string> limited;
    const auto flush = [&](std::uint64_t now)
    {
        limiter.flush(now, [&](const char* text, const void* t) { assert(t == &tag); limited.emplace_back(text); },
            [&](const char* text, const void* t, std::uint32_t n) { assert(t == &tag); summaries.emplace_back(text, n); });
    };
    // the first 64 lines of a key pass
    std::uint64_t now = 5 * s;
    for (unsigned i = 0; i < repeat_limiter::free_lines; i++)
    {
        const auto v = limiter.admit(0x1234, flood, &tag, now);
        assert(v.pass && !v.summary);
    }
    flush(now);
    assert(limited.empty() && summaries.empty());
    // then 8 per 10 s
    unsigned passed = 0, dropped = 0;
    for (unsigned i = 0; i < 100; i++)
    {
        const auto v = limiter.admit(0x1234, flood, &tag, now + i * 1000);
        assert(!v.summary);
        (v.pass ? passed : dropped)++;
    }
    assert(passed == repeat_limiter::window_lines && dropped == 92);
    flush(now + 2 * s);
    assert(limited.size() == 1 && limited[0] == flood);
    assert(summaries.empty()); // the window is still open
    // the next line after the window reports the left-out ones and passes
    {
        const auto v = limiter.admit(0x1234, flood, &tag, now + 10 * s + 1);
        assert(v.pass && v.summary == 92 && v.sample && std::string(v.sample) == flood);
    }
    for (unsigned i = 0; i < 7; i++) assert(limiter.admit(0x1234, flood, &tag, now + 11 * s).pass);
    for (unsigned i = 0; i < 5; i++) assert(!limiter.admit(0x1234, flood, &tag, now + 12 * s).pass);
    // a window that ends without a later line is reported by the flush, once, and the next line opens a new window
    flush(now + 15 * s);
    assert(summaries.empty());
    flush(now + 21 * s);
    assert(summaries.size() == 1 && summaries[0].first == flood && summaries[0].second == 5);
    flush(now + 40 * s);
    assert(summaries.size() == 1 && limited.size() == 1);
    {
        const auto v = limiter.admit(0x1234, flood, &tag, now + 41 * s);
        assert(v.pass && !v.summary);
    }
    // another key is independent; key 0 is a key too
    for (unsigned i = 0; i < 64; i++) assert(limiter.admit(0x99, other, &tag, now).pass);
    assert(limiter.admit(0, other, &tag, now).pass);
    // a format that carries whole texts is never limited
    assert(repeat_limiter::literal_chars(bare) == 2);
    assert(repeat_limiter::literal_chars("%s") == 0);
    assert(repeat_limiter::literal_chars("%08llx %% %.*s ab") == 3);
    assert(repeat_limiter::literal_chars(flood) == repeat_limiter::min_literal);
    for (unsigned i = 0; i < 1000; i++) assert(limiter.admit(0x77, bare, &tag, now).pass);
    // a full probe window: later keys of that window are never limited (and never crash)
    static repeat_limiter crowded;
    unsigned crowded_pass = 0;
    for (std::uint64_t k = 1; k <= 4 * repeat_limiter::slots; k++)
        for (unsigned i = 0; i < 80; i++) crowded_pass += crowded.admit(k * 0x9e3779b97f4a7c15ull, flood, &tag, now).pass;
    assert(crowded_pass >= 4 * repeat_limiter::slots * 64);
    // many threads on one key (claimed by a first line): exactly 64 + 8 pass in one window, the rest is counted
    static repeat_limiter shared;
    assert(shared.admit(0x42, flood, &tag, now).pass);
    std::atomic<unsigned> shared_pass{1};
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; t++)
        threads.emplace_back([&] { for (int i = 0; i < 1000; i++) shared_pass += shared.admit(0x42, flood, &tag, now).pass; });
    for (auto& t : threads) t.join();
    assert(shared_pass.load() == repeat_limiter::free_lines + repeat_limiter::window_lines);
    std::uint32_t left = 0;
    shared.flush(now + 11 * s, [](const char*, const void*) {}, [&](const char*, const void*, std::uint32_t n) { left += n; });
    assert(left == 8001 - repeat_limiter::free_lines - repeat_limiter::window_lines);
    // the sample is copied when the key is claimed: the caller's buffer may change or go away afterwards
    {
        static repeat_limiter copied;
        char buffer[80] = "Volatile buffer text that will change, %u";
        for (unsigned i = 0; i < 64 + 8 + 3; i++) copied.admit(0x5150, buffer, &tag, now);
        std::memset(buffer, 'Z', sizeof(buffer) - 1);
        std::string seen;
        copied.flush(now + 11 * s, [](const char*, const void*) {}, [&](const char* sample, const void*, std::uint32_t n) { seen = sample; assert(n == 3); });
        assert(seen == "Volatile buffer text that will change, %u");
    }
    // the texts
    using utils::ps4::repeat_sample;
    const auto sample = [](const char* text, std::size_t size = 64)
    {
        char out[128];
        const std::size_t n = repeat_sample(text, out, size);
        assert(n == std::strlen(out) && n < size);
        return std::string(out);
    };
    assert(sample(nullptr).empty());
    assert(repeat_sample("x", nullptr, 4) == 0);
    assert(sample("a\nb\tc") == "a b c");
    assert(sample(std::string(60, 'x').c_str()) == std::string(60, 'x'));
    assert(sample(std::string(70, 'x').c_str()) == std::string(60, 'x') + "...");
    const std::string accents = std::string(59, 'x') + "\xc3\xa9" + "yy";
    assert(sample(accents.c_str()) == std::string(59, 'x') + "...");
    assert(sample(std::string(70, 'x').c_str(), 10) == std::string(6, 'x') + "...");
    assert(sample("abc", 1).empty());
    assert(sample("abcdef", 4) == "...");
    assert(utils::ps4::repeat_summary_line(92, flood) == "log: 92 line(s) like 'Transfer cropped in Y, src_h=%d, dst_h=%d' left out in the last 10 s");
    assert(utils::ps4::repeat_limited_line(flood) == "log: 'Transfer cropped in Y, src_h=%d, dst_h=%d' was written 64 times; from now on at most 8 such lines "
        "per 10 s are written, with a count of the rest (log-flood-off.flag keeps every line)");
}

void test_env_lines()
{
    std::string key, value;
    using ps4_fatal_detail::parse_env_line;
    assert(parse_env_line("ORBIS_COUNT_API=1", key, value) && key == "ORBIS_COUNT_API" && value == "1");
    assert(parse_env_line("  ZINK_DEBUG = nir,spirv \r", key, value) && key == "ZINK_DEBUG" && value == "nir,spirv");
    assert(parse_env_line("EMPTY=", key, value) && key == "EMPTY" && value.empty());
    assert(parse_env_line("A=b=c", key, value) && key == "A" && value == "b=c");
    assert(!parse_env_line("", key, value));
    assert(!parse_env_line("# comment=1", key, value));
    assert(!parse_env_line("no equals sign", key, value));
    assert(!parse_env_line("=value", key, value));
    assert(!parse_env_line("BAD KEY=1", key, value));
    assert(!parse_env_line("1ABC=1", key, value));
    assert(!parse_env_line("PATH;rm=1", key, value));
}
}

int main()
{
    test_lifecycle();
    test_stall_watchdog();
    test_fixed_text();
    test_previous_fatal_notice();
    test_env_lines();
    test_history_filter();
    test_history_todo_dedupe();
    test_quiet_clock();
    test_empty_frames_watch();
    test_session_record();
    test_call_pattern();
    test_npd_license();
    test_switch_files();
    test_first_boot_compile();
    test_heartbeat_texts();
    test_repeat_limiter();
    std::cout << "PASS: lifecycle (one stop, return only after stop, three game cycles, exit precedence, natural game exit), "
                 "stall watchdog (25 s/+10 s/120 s/5 dumps, pause, re-arm), fixed text, previous-fatal notice, mesa-env parsing, "
                 "history line filter, TODO dedupe (3 of each text), quiet clock (60 s/300 s), empty frames (60 s/120 s), "
                 "session record, HLE call pattern, NPD license, switch files, first-boot compile estimate, heartbeat stage and "
                 "draw texts, repeated-line limiter (64 pass, 8 per 10 s, summaries, exempt formats, full table, 8 threads)\n";
}
