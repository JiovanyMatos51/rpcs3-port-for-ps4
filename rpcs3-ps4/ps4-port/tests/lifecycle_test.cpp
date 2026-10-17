#include "util/ps4_lifecycle.hpp"
#include "ps4_fatal.h"
#include <cassert>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

utils::ps4::lifecycle state;

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
    test_npd_license();
    test_switch_files();
    test_first_boot_compile();
    std::cout << "PASS: lifecycle (one stop, return only after stop, three game cycles, exit precedence, natural game exit), "
                 "stall watchdog (25 s/+10 s/120 s/5 dumps, pause, re-arm), fixed text, previous-fatal notice, mesa-env parsing, "
                 "history line filter, NPD license, switch files, first-boot compile estimate\n";
}
