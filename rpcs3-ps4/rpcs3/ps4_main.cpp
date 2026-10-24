#ifndef RPCS3_PS4
#error This translation unit is only for the PS4 target.
#endif

#include "stdafx.h"

#include "ps4_fatal.h"
#include "ps4_frame.h"
#include "ps4_launcher.h"
#include "ps4_pads.h"
#include "ps4_profiler.h"
#include "Emu/System.h"
#include "Emu/emu_callbacks.h"
#include "Emu/system_config.h"
#include "Emu/system_utils.hpp"
#include "Emu/RSX/Null/NullGSRender.h"
#include "Emu/RSX/GL/GLGSRender.h"
#ifdef HAVE_VULKAN
#include "Emu/RSX/VK/VKGSRender.h"
#endif
#include "Emu/Io/Null/NullKeyboardHandler.h"
#include "Emu/Io/Null/NullMouseHandler.h"
#include "Emu/Io/Null/null_camera_handler.h"
#include "Emu/Io/Null/null_music_handler.h"
#include "Emu/Audio/PS4/PS4AudioBackend.h"
#include "Emu/Audio/PS4/PS4AudioEnumerator.h"
#include "Emu/Cell/Modules/cellMsgDialog.h"
#include "Emu/Cell/Modules/cellOskDialog.h"
#include "Emu/Cell/Modules/cellSaveData.h"
#include "Emu/Cell/Modules/sceNpTrophy.h"
#include "Input/pad_thread.h"
#include "Emu/Io/pad_config.h"
#include "util/video_source.h"
#include "Utilities/Thread.h"
#include "util/ps4_game_library.hpp"
#include "util/ps4_localized.hpp"
#include "util/ps4_lifecycle.hpp"
#include "util/ps4_build_version.hpp"
#include "util/ps4_llvm_verify.hpp"
#include "util/ps4_virtual_memory.hpp"
#include "util/ps4_cpu_probe.hpp"
#include "util/ps4_adapt.hpp"
#include "util/ps4_gpu_health.hpp"
#include "util/ps4_health.hpp"
#include "util/ps4_hr_wait.hpp"
#include "util/ps4_disc_lock.hpp"
#include "util/ps4_i18n.hpp"
#include "util/ps4_safe_mode.hpp"
#include "util/ps4_xmb_notice.hpp"
#include "util/sysinfo.hpp"
#include "Emu/system_progress.hpp"
#include "Emu/ps4_cheat_engine.h"
#include "Emu/localized_string.h"
#include "Emu/IdManager.h"
#include "Emu/Cell/PPUThread.h"
#include "Emu/Cell/SPUThread.h"
#include "Emu/Cell/lv2/sys_sync.h"
#include "Emu/Cell/lv2/sys_event.h"
#include "Emu/Cell/lv2/sys_event_flag.h"
#include "Emu/Cell/lv2/sys_spu.h"
#include "Emu/Cell/timers.hpp"
#include "Emu/RSX/RSXThread.h"
#include "Emu/RSX/Overlays/overlay_manager.h"
#include "Emu/RSX/Overlays/overlay_message_dialog.h"
#include "Emu/RSX/Overlays/overlay_media_list_dialog.h"
#include "Emu/RSX/Overlays/overlay_osk.h"
#include "Emu/RSX/Overlays/overlay_save_dialog.h"
#include "Emu/RSX/Overlays/overlay_trophy_notification.h"
#include "Emu/RSX/Overlays/overlay_user_list_dialog.h"
#include "Emu/RSX/Overlays/HomeMenu/overlay_home_menu.h"
#include "Emu/Cell/Modules/cellSysutil.h"
#include "Emu/Cell/lv2/sys_fs.h"
#include "Emu/VFS.h"
#include "Emu/NP/ip_address.h"
#include "Emu/NP/ps4_tls_selftest.h"
#include "Emu/Memory/vm_reservation.h"
#include "Loader/ISO.h"
#include "Loader/PSF.h"
#include "Utilities/StrUtil.h"

#include <orbis/libkernel.h>
#include <orbis/Sysmodule.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <thread>
#include <unordered_map>
#include <vector>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <netinet/in.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wcast-qual"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#ifdef LLVM_AVAILABLE
#include "llvm/TargetParser/Host.h"
#endif
// stb_image is implemented in Emu/stb_image.cpp; the resizer is private to this file (static functions only).
#include <stb_image.h>
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_STATIC
#include <stb_image_resize2.h>
#pragma GCC diagnostic pop

LOG_CHANNEL(ps4_log, "PS4");

// 0.39: interface language of the toasts and XMB texts built here (util/ps4_i18n.hpp, table ps4_i18n_main.inc)
namespace i18n = utils::ps4::i18n;
// 0.40: the feature table and the per-game safe mode (util/ps4_safe_mode.hpp)
namespace safe_mode = utils::ps4::safe_mode;

[[noreturn]] void report_fatal_error(std::string_view text, bool is_html = false, bool include_help_text = true);

// System.cpp: non-zero while a PPU module compile holds the Stop Watchdog (it then waits 25 s instead of 10 s)
extern atomic_t<u64> g_watchdog_hold_ctr;

namespace rsx
{
	// RSXThread.cpp (0.26): native pthread_t of the RSX thread, for the STALL host stack sample.
	u64 ps4_rsx_native_thread() noexcept;
}
void ps4_log_atomic_waiters();
// 0.39: cellAudio.cpp, sys_rsxaudio.cpp, overlay_perf_metrics.cpp (declared as main_application.cpp does), for the
// settings the home menu changes in a running game.
namespace audio
{
	void configure_audio(bool force_reset);
	void configure_rsxaudio();
}
namespace rsx::overlays
{
	void reset_performance_overlay();
}
// ps4_launcher.cpp: the home menu's Salvar (only the settings the player changed in the menu, as that game's choices).
void ps4_home_menu_save();
// ps4-port/runtime/ps4_fenv.cpp: functional x86-64 fenv + SPU rounding self-test.
bool ps4_spu_rounding_ok() noexcept;
const char* ps4_fpu_selftest_detail() noexcept;
// util/sysinfo.cpp (0.29): the TSC decision (calibrated / kernel fallback / none) for the boot line.
std::string ps4_tsc_info() noexcept;

// orbis-compat include/orbis_thread.h (0.29), redeclared like ps4_fatal.cpp does: all three print through orbis_log
// into startup.log ('orbis:' lines; the sink keeps 256 per session) and must not run from a signal handler.
namespace orbis
{
void vmReport(const char* where) noexcept;      // VA-map walk + thread picture; throttled after its 12th call
void limitsReport() noexcept;                   // rlimits/sysctls in full once, then one short line per call
void threadLimitProbe(unsigned n) noexcept;     // flag-gated experiment: how many threads can coexist
// include/orbis_clock.h (0.33): clock ids 0/1/4 vs the process counter and the condvar setclock read-back, six
// 'clock ids:' lines once. Weak: a build against an orbis-compat without it skips that probe.
[[gnu::weak]] void clockIdProbe();
}

// libSceSystemService (sys_time.cpp declares it the same way): 4 = TIME_ZONE, 5 = SUMMERTIME.
extern "C" int32_t sceSystemServiceParamGetInt(int32_t param_id, int32_t* value);

// 0.34: libkernel's firmware version query (read only), for the 'environment:' line. Weak and bound under a private
// name like the CPU probe's mode queries (ps4_cpu_probe.hpp): a firmware without the export reads as null.
extern "C" int32_t ps4_env_system_sw_version(OrbisKernelSwVersion* version) __asm__("sceKernelGetSystemSwVersion") __attribute__((weak));

// 0.36: libSceGnmDriver (linked as SceGnmDriver; the OpenOrbis stub exports both), declared as Mesa's WSI imports
// them. sceGnmAreSubmitsAllowed: 1 when a submission would not stall sceGnmSubmitDone, 0 when it would.
extern "C" int32_t sceGnmSubmitDone(void);
extern "C" int32_t sceGnmAreSubmitsAllowed(void);
// 0.36: orbis-drm (ac_orbis_drm.c in libvulkan_radeon.a, linked whole): GPU submissions handed out so far, 0 before
// the first. Hidden in the archive, which still resolves inside this static link (Mesa's headless WSI imports it so).
extern "C" uint64_t ac_orbis_gpu_seq_now(void);
// Utilities/Thread.cpp: native priority of the main thread (700 on the console; lower values run first)
int ps4_thread_base_native_priority() noexcept;

// 0.33: file-scope locks of the core (CPUThread.cpp s_cpu_lock, sys_memory.cpp s_memstats_mtx, perf_meter.cpp
// s_perf_mutex), read by the post-stop lock audit below.
const shared_mutex& ps4_cpu_lock() noexcept;
const shared_mutex& ps4_memstats_lock() noexcept;
const shared_mutex& ps4_perf_lock() noexcept;

// sys_fs.cpp: head of the mount point chain ('/' -> /app_home -> ... -> /dev_hdd0 -> ... -> /dev_usb); sys_fs.h
// exports only /dev_hdd0 and the no-device entry.
extern lv2_fs_mount_point g_mp_sys_dev_root;

// util/logs.cpp (0.40): the summaries and notes of the repeated-line limiter of the live log; main thread, about once
// per second.
namespace logs
{
	void ps4_flush_repeated_lines();
}

// sys_fs.cpp (0.40): the movie file the game holds open now (the most recently opened one still open); false when
// none. 'name' gets its file name without folders (NUL-terminated, cut to name_size - 1 bytes), 'seconds' the whole
// seconds since it was opened. Any thread, lock-free, allocation-free.
bool ps4_fs_movie_status(char* name, std::size_t name_size, unsigned* seconds) noexcept;

// Image base (OpenOrbis link.x). .text starts at ELF address 0, so 'address - __text_start' of a global is its
// llvm-nm address in rpcs3_ps4.elf.
extern char __text_start[];

// These are normally supplied by the Qt frontend, which is not linked on PS4.
atomic_t<bool> g_headless = false;
std::string g_input_config_override;
cfg_input_configurations g_cfg_input_configs;

namespace
{
    void verify_native_udp_bind()
    {
        const int fd = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (fd < 0)
        {
            ps4_startup_checkpoint(fmt::format("network ABI: cannot open UDP socket, errno=%d", errno).c_str());
            return;
        }
        sockaddr_in request{};
        request.sin_len = sizeof(request);
        request.sin_family = AF_INET;
        const int bound = ::bind(fd, reinterpret_cast<sockaddr*>(&request), sizeof(request));
        sockaddr_storage address{};
        socklen_t length = sizeof(address);
        const int queried = bound == 0 ? ::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) : -1;
        const int error = errno;
        ::close(fd);
        if (bound == 0 && queried == 0 && length == sizeof(sockaddr_in) &&
            address.ss_len == sizeof(sockaddr_in) && address.ss_family == AF_INET)
            ps4_startup_checkpoint("network ABI: native UDP bind/getsockname passed (length=16 family=2)");
        else
            ps4_startup_checkpoint(fmt::format("network ABI: bind=%d getsockname=%d length=%u native_length=%u family=%u errno=%d",
                bound, queried, length, address.ss_len, address.ss_family, error).c_str());
    }

    void verify_native_socket_address()
    {
        verify_native_udp_bind();
        // 0.34: a host recvfrom that blocks inside a guest syscall freezes the game. One bounded non-blocking UDP
        // receive (at most ~200 ms, SO_RCVTIMEO) shows whether O_NONBLOCK, FIONBIO and the FreeBSD MSG_DONTWAIT
        // value reach this kernel.
        ps4_startup_checkpoint(np::ps4_probe_nonblocking_recvfrom().c_str());
    }

    void verify_native_atomic_wait()
    {
        atomic_t<u32> sequence{0};
        const auto started = sceKernelGetProcessTime();
        sequence.wait(0, atomic_wait_timeout{10'000'000});
        const auto elapsed = sceKernelGetProcessTime() - started;
        // Verify an actual kernel timeout before launching emulation threads.
        if (elapsed < 5'000 || elapsed > 1'000'000)
            report_fatal_error(fmt::format("Native atomic wait timeout mismatch: %llu us", elapsed));

        constexpr u32 exchanges = 32;
        constexpr u32 cancelled = umax;
        const auto deadline = sceKernelGetProcessTime() + 5'000'000;
        std::atomic<bool> failed{false};
        auto wait_value = [&](u32 expected)
        {
            for (;;)
            {
                const u32 current = sequence.load();
                if (current == expected) return true;
                if (current == cancelled || sceKernelGetProcessTime() >= deadline) return false;
                sequence.wait(current, atomic_wait_timeout{1'000'000});
            }
        };
        std::thread worker([&]
        {
            for (u32 i = 0; i < exchanges; ++i)
            {
                if (!wait_value(2 * i + 1))
                {
                    failed = true;
                    sequence.store(cancelled);
                    sequence.notify_all();
                    return;
                }
                sequence.store(2 * i + 2);
                sequence.notify_all();
            }
        });
        for (u32 i = 0; i < exchanges; ++i)
        {
            sequence.store(2 * i + 1);
            sequence.notify_all();
            if (!wait_value(2 * i + 2))
            {
                failed = true;
                sequence.store(cancelled);
                sequence.notify_all();
                break;
            }
        }
        worker.join();
        if (failed) report_fatal_error("Native atomic wait wakeup verification failed");
        ps4_startup_checkpoint(fmt::format("atomic wait: kernel timeout verified (%llu us); %u round trips passed",
            elapsed, exchanges).c_str());
    }

    // 0.34: a libkernel export by its plain name, searched over the loaded module handles (the idiom of
    // ps4_profiler.cpp dlsym_any and ps4_shared_memory.cpp, which works on the console). Probes of exports the
    // OpenOrbis headers or stubs may lack add no static import, so a missing export cannot break the link or the load.
    void* ps4_kernel_symbol(const char* symbol) noexcept
    {
        u32 handles[256]{};
        usz count = 0;
        if (sceKernelGetModuleList(handles, 256, &count) != 0) return nullptr;
        for (usz i = 0; i < std::min<usz>(count, 256); ++i)
        {
            void* address = nullptr;
            if (sceKernelDlsym(static_cast<s32>(handles[i]), symbol, &address) == 0 && address) return address;
        }
        return nullptr;
    }

    // 0.34: precise-wait candidates for the audio poll, the RSX semaphore sleep and guest usleeps above 100 us:
    // libkernel's high-resolution equeue timer (its declaration is commented out in the OpenOrbis header) and
    // sceKernelNanosleep, both resolved at run time. Every HR sample gets a private equeue that is deleted right after
    // its wait, so a timer that is not one-shot cannot fire into a later sample. Each wait is bounded (20 ms) and a
    // duration stops at its first error, so this adds well under 0.2 s to the probe thread.
    // 0.36: then the reuse test of utils::ps4::hr_wait_us (one equeue, many waits); `hr` receives the functions and
    // `reuse_ok` whether that test passed.
    template <typename Stamp, typename ToNs>
    std::string probe_precise_waits(const Stamp& stamp, const ToNs& to_ns, utils::ps4::hr_wait_detail::functions& hr, bool& reuse_ok)
    {
        // OrbisKernelTimespec: FreeBSD amd64 struct timespec.
        using kernel_timespec = utils::ps4::hr_wait_detail::kernel_timespec;

        using eq_create_fn = s32 (*)(OrbisKernelEqueue*, const char*);
        using eq_delete_fn = s32 (*)(OrbisKernelEqueue);
        using eq_wait_fn = s32 (*)(OrbisKernelEqueue, void*, s32, s32*, OrbisKernelUseconds*);
        using hr_add_fn = s32 (*)(OrbisKernelEqueue, s32, kernel_timespec*, void*);
        using nanosleep_fn = s32 (*)(const kernel_timespec*, kernel_timespec*);

        const auto eq_create = reinterpret_cast<eq_create_fn>(ps4_kernel_symbol("sceKernelCreateEqueue"));
        const auto eq_delete = reinterpret_cast<eq_delete_fn>(ps4_kernel_symbol("sceKernelDeleteEqueue"));
        const auto eq_wait = reinterpret_cast<eq_wait_fn>(ps4_kernel_symbol("sceKernelWaitEqueue"));
        const auto hr_add = reinterpret_cast<hr_add_fn>(ps4_kernel_symbol("sceKernelAddHRTimerEvent"));
        const auto nano = reinterpret_cast<nanosleep_fn>(ps4_kernel_symbol("sceKernelNanosleep"));

        const auto avg_max = [](const std::vector<u64>& ns) -> std::string
        {
            u64 sum = 0, max = 0;
            for (const u64 value : ns)
            {
                sum += value;
                max = std::max(max, value);
            }
            return ns.empty() ? std::string("n/a") : fmt::format("%.1f/%.1f", sum / 1000. / ns.size(), max / 1000.);
        };

        std::string line = "HR timer";
        if (!hr_add)
        {
            line += " not exported";
        }
        else if (!eq_create || !eq_delete || !eq_wait)
        {
            line += " equeue functions not exported";
        }
        else
        {
            for (const u64 us : {u64{100}, u64{300}, u64{1000}})
            {
                std::vector<u64> ns;
                std::string failure;
                for (u32 i = 0; i < 8 && failure.empty(); i++)
                {
                    OrbisKernelEqueue queue{};
                    if (const s32 rc = eq_create(&queue, "rpcs3_hr_probe"); rc != 0)
                    {
                        failure = fmt::format("create rc 0x%x", static_cast<u32>(rc));
                        break;
                    }
                    kernel_timespec period{0, static_cast<s64>(us * 1000)};
                    alignas(16) u8 events[128]{}; // one event (32 bytes on this kernel), with room to spare
                    s32 got = 0;
                    OrbisKernelUseconds timeout = 20'000;
                    const u64 t0 = stamp();
                    s32 rc = hr_add(queue, static_cast<s32>(i + 1), &period, nullptr);
                    if (rc == 0) rc = eq_wait(queue, events, 1, &got, &timeout);
                    const u64 t1 = stamp();
                    eq_delete(queue);
                    if (rc != 0) failure = fmt::format("rc 0x%x", static_cast<u32>(rc));
                    else if (got < 1) failure = "no event";
                    else ns.push_back(to_ns(t1 - t0));
                }
                fmt::append(line, " %u=%s", us, failure.empty() ? avg_max(ns) : failure);
            }

            // 0.36: the hr_wait_us pattern: one equeue for 16 waits of 200 us, each arming the same timer ident. Every
            // odd wait follows a 1 ms gap in which a timer that is not one-shot would queue another event; such an
            // event shows as a stray (more than one event returned, or a wait ending before 3/4 of its time). After
            // the last wait, 3 ms without a timer must bring no event.
            hr = {eq_create, eq_delete, eq_wait, hr_add};
            constexpr u64 reuse_us = 200;
            std::vector<u64> ns;
            u32 strays = 0, errors = 0;
            s32 first_error = 0;
            OrbisKernelEqueue queue{};
            if (const s32 rc = eq_create(&queue, "rpcs3_hr_reuse"); rc != 0)
            {
                errors++;
                first_error = rc;
            }
            else
            {
                for (u32 i = 0; i < 16; i++)
                {
                    if (i % 2) sceKernelUsleep(1000);
                    s32 got = 0;
                    const u64 t0 = stamp();
                    const s32 rc = utils::ps4::hr_wait_detail::wait_once(hr, queue, reuse_us, got);
                    const u64 elapsed = to_ns(stamp() - t0);
                    if (rc != 0)
                    {
                        if (!errors++) first_error = rc;
                        continue;
                    }
                    if (got > 1) strays += static_cast<u32>(got - 1);
                    else if (elapsed < reuse_us * 1000 * 3 / 4) strays++;
                    ns.push_back(elapsed);
                }
                alignas(16) u8 events[128]{};
                s32 got = 0;
                OrbisKernelUseconds timeout = 3000;
                if (eq_wait(queue, events, 4, &got, &timeout) == 0 && got > 0) strays += static_cast<u32>(got);
                eq_delete(queue);
            }
            reuse_ok = errors == 0 && strays == 0 && ns.size() == 16;
            fmt::append(line, "; HR reuse: 16 waits on one equeue avg/max %s us, stray events %u, errors %u%s -> %s", avg_max(ns), strays, errors,
                errors ? fmt::format(" (first 0x%x)", static_cast<u32>(first_error)) : std::string(), reuse_ok ? "reuse OK" : "reuse NOT OK");
        }

        line += "; nanosleep";
        if (!nano)
        {
            line += " not exported";
        }
        else
        {
            for (const u64 us : {u64{100}, u64{300}})
            {
                std::vector<u64> ns;
                std::string failure;
                for (u32 i = 0; i < 8 && failure.empty(); i++)
                {
                    const kernel_timespec request{0, static_cast<s64>(us * 1000)};
                    const u64 t0 = stamp();
                    const s32 rc = nano(&request, nullptr);
                    const u64 t1 = stamp();
                    if (rc != 0) failure = fmt::format("rc 0x%x", static_cast<u32>(rc));
                    else ns.push_back(to_ns(t1 - t0));
                }
                fmt::append(line, " %u=%s", us, failure.empty() ? avg_max(ns) : failure);
            }
        }

        return line;
    }

    // 0.29: what short host waits really cost on this kernel. Every short-wait decision rests on it (lv2 'As Host'
    // guest usleep = thread_ctrl::wait_on, the 0.29 precise-sleep bound of 100 us, progress_backoff's 1 ms yield
    // window, the RSX idle sleeps), and the only measurement so far was one 10 ms timeout. About 100 ms once, on a
    // short-lived named_thread, before any game runs.
    void run_timer_probe()
    {
        struct sample_set
        {
            u64 sum = 0, max = 0, count = 0;

            void add(u64 ns)
            {
                sum += ns;
                max = std::max(max, ns);
                count++;
            }

            std::string text() const
            {
                return count ? fmt::format("%.1f/%.1f", sum / 1000. / count, max / 1000.) : std::string("n/a");
            }
        };

        // TSC ticks (sceKernelReadTsc) when the kernel reports the rate, else microseconds of process time.
        const u64 tsc_hz = sceKernelGetTscFrequency();
        const u64 ticks_per_us = tsc_hz >= 100'000'000 && tsc_hz <= 10'000'000'000ull ? tsc_hz / 1'000'000 : 0;
        const auto stamp = [ticks_per_us]() -> u64 { return ticks_per_us ? sceKernelReadTsc() : sceKernelGetProcessTime(); };
        const auto to_ns = [ticks_per_us](u64 delta) -> u64 { return ticks_per_us ? delta * 1000 / ticks_per_us : delta * 1000; };

        constexpr u32 pings = 24;
        constexpr u32 cancelled = umax;
        struct probe_state
        {
            sample_set wait30, wait100, wait1000, usleep30, usleep100, usleep1000, yield;
            sample_set wait500;      // 0.34: the sub-tick block poll candidate of the audio thread
            std::string precise;     // 0.34: HR timer and sceKernelNanosleep part of the line
            utils::ps4::hr_wait_detail::functions hr{}; // 0.36: read after the join
            bool hr_reuse_ok = false;
            atomic_t<u32> ready{0}, ping{0}, pong{0};
            u64 woke[pings]{};
        } st;

        const auto probe_body = [&st, &stamp, &to_ns]()
        {
            const auto measure = [&](sample_set& set, u32 count, auto&& wait)
            {
                for (u32 i = 0; i < count; i++)
                {
                    const u64 t0 = stamp();
                    wait();
                    set.add(to_ns(stamp() - t0));
                }
            };
            // thread_ctrl::wait_for is the atomic-wait engine with a timeout: the host side of 'As Host' guest sleeps.
            measure(st.wait30, 8, [] { thread_ctrl::wait_for(30); });
            measure(st.wait100, 8, [] { thread_ctrl::wait_for(100); });
            measure(st.wait1000, 4, [] { thread_ctrl::wait_for(1000); });
            measure(st.usleep30, 8, [] { sceKernelUsleep(30); });
            measure(st.usleep100, 8, [] { sceKernelUsleep(100); });
            measure(st.usleep1000, 4, [] { sceKernelUsleep(1000); });
            measure(st.yield, 256, [] { std::this_thread::yield(); });
            measure(st.wait500, 8, [] { thread_ctrl::wait_for(500); });
            st.precise = probe_precise_waits(stamp, to_ns, st.hr, st.hr_reuse_ok);
            st.ready.release(1);
            st.ready.notify_one();
            // Notify-to-wake: the main thread stores k into ping and notifies; this thread records when it woke and
            // answers through pong. Every wait is bounded, and 'cancelled' ends the exchange.
            const u64 deadline = sceKernelGetProcessTime() + 3'000'000;
            for (u32 k = 1; k <= pings; k++)
            {
                u32 value = st.ping.load();
                while (value != k && value != cancelled && sceKernelGetProcessTime() < deadline)
                {
                    st.ping.wait(value, atomic_wait_timeout{200'000'000});
                    value = st.ping.load();
                }
                if (value != k) break;
                st.woke[k - 1] = stamp();
                st.pong.release(k);
                st.pong.notify_one();
            }
        };

        // 0.36: HR waits (util/ps4_hr_wait.hpp) for the RSX idle and semaphore sleeps and short guest sleeps, only when the
        // reuse test of this probe passed. 0.37: on by default (the reuse test passed on the consoles tested); lv2 object
        // waits keep the interruptible wait (lv2.cpp), /data/rpcs3/no-hr-wait.flag turns it off.
        const auto configure_hr_wait = [&st](bool probed)
        {
            const bool wanted = !fs::is_file("/data/rpcs3/no-hr-wait.flag");
            utils::ps4::hr_wait_configure(st.hr, wanted && probed && st.hr_reuse_ok);
            ps4_startup_checkpoint(utils::ps4::hr_wait_enabled() ? "hr wait: on (default; no-hr-wait.flag disables)" : !wanted ? "hr wait: off (no-hr-wait.flag)" : "hr wait: off (probe failed)");
        };

        named_thread probe(stx::try_launch, "PS4 Timer Probe", probe_body);
        if (!probe.ps4_try_start())
        {
            ps4_startup_checkpoint("timer probe: skipped (the console refused its thread)");
            configure_hr_wait(false);
            return;
        }

        std::vector<u64> wake_ns, round_ns;
        const u64 deadline = sceKernelGetProcessTime() + 3'000'000;
        while (!st.ready.load() && sceKernelGetProcessTime() < deadline) st.ready.wait(0, atomic_wait_timeout{200'000'000});
        for (u32 k = 1; k <= pings && st.ready.load(); k++)
        {
            // Give the probe thread time to fall asleep in its wait: this measures the wake-up of a sleeping waiter.
            sceKernelUsleep(1000);
            const u64 t0 = stamp();
            st.ping.release(k);
            st.ping.notify_one();
            u32 value = st.pong.load();
            while (value != k && sceKernelGetProcessTime() < deadline)
            {
                st.pong.wait(value, atomic_wait_timeout{200'000'000});
                value = st.pong.load();
            }
            if (value != k) break;
            const u64 t1 = stamp();
            round_ns.push_back(to_ns(t1 - t0));
            if (st.woke[k - 1] >= t0) wake_ns.push_back(to_ns(st.woke[k - 1] - t0));
        }
        st.ping.release(cancelled);
        st.ping.notify_one();
        probe(); // join

        const auto median_max = [](std::vector<u64>& v) -> std::string
        {
            if (v.empty()) return "n/a";
            std::sort(v.begin(), v.end());
            return fmt::format("%.1f/%.1f", v[v.size() / 2] / 1000., v.back() / 1000.);
        };
        const u32 exchanged = static_cast<u32>(round_ns.size());
        ps4_startup_checkpoint(fmt::format("timer probe (us, avg/max): thread_ctrl::wait_for 30=%s 100=%s 1000=%s; sceKernelUsleep 30=%s 100=%s 1000=%s; "
            "sched_yield %s; atomic notify->wake (median/max) %s, round trip %s (%u/%u exchanges); clock %s; %s; wait_for 500=%s",
            st.wait30.text(), st.wait100.text(), st.wait1000.text(), st.usleep30.text(), st.usleep100.text(), st.usleep1000.text(),
            st.yield.text(), median_max(wake_ns), median_max(round_ns), exchanged, pings,
            ticks_per_us ? fmt::format("TSC %llu Hz", tsc_hz) : std::string("process time (us)"),
            st.precise.empty() ? std::string("HR timer n/a; nanosleep n/a") : st.precise, st.wait500.text()).c_str());
        configure_hr_wait(st.ready.load() != 0);
    }

    // 0.29: one file of the log history (log/history/0). Plain open/read/write with every result checked, a 256 KiB
    // buffer and a size bound: the whole file when it fits in `limit`, else its first `head` bytes, a marker line and
    // its last `limit - head` bytes. 0.27/0.28 used fs::copy_file, whose Orbis build takes the generic iostream
    // fallback: a char-by-char copy (~0.5 s per boot for ~2.5 MB in 0.28)
    // that returns true without checking the copy, of files up to 64 MiB each (RPCS3.log cap). fs::file is not used
    // either: its POSIX read/write ensure() success, i.e. an I/O error would end the app (fatal + park).
    struct archive_result
    {
        bool present = false; // the source exists (or could not be opened for another reason)
        bool ok = false;
        u64 size = 0;
        u64 written = 0;
        const char* step = "";
        int error = 0;
    };

    // 0.34: middle: when the file is cut, at most this many bytes of error/PS4/AudioOut lines from the part left out
    // (utils::ps4::keep_history_line) are kept between the head and the tail, the newest ones (0 = none).
    // scan_budget: bytes that may still be read for that in this boot, across the files (bounds the boot time with
    // 64 MiB logs); only the last that many bytes of the part left out are read.
    archive_result archive_log_file(const std::string& from, const std::string& to, u64 limit, u64 head, std::vector<char>& buffer,
        u64 middle = 0, u64* scan_budget = nullptr)
    {
        archive_result r;
        const int in = ::open(from.c_str(), O_RDONLY);
        if (in < 0)
        {
            r.error = errno;
            r.present = r.error != ENOENT;
            r.step = "open";
            return r;
        }
        r.present = true;
        const off_t end = ::lseek(in, 0, SEEK_END);
        if (end < 0 || ::lseek(in, 0, SEEK_SET) != 0)
        {
            r.error = errno;
            r.step = "seek";
            ::close(in);
            return r;
        }
        r.size = static_cast<u64>(end);
        const int out = ::open(to.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (out < 0)
        {
            r.error = errno;
            r.step = "create";
            ::close(in);
            return r;
        }
        const auto put = [&](const char* data, u64 count) -> bool
        {
            while (count)
            {
                const ssize_t done = ::write(out, data, count);
                if (done <= 0)
                {
                    r.error = done < 0 ? errno : 0;
                    r.step = "write";
                    return false;
                }
                data += done;
                count -= static_cast<u64>(done);
                r.written += static_cast<u64>(done);
            }
            return true;
        };
        const auto copy = [&](u64 count) -> bool
        {
            while (count)
            {
                const ssize_t got = ::read(in, buffer.data(), std::min<u64>(count, buffer.size()));
                if (got <= 0)
                {
                    r.error = got < 0 ? errno : 0; // 0: the file got shorter
                    r.step = "read";
                    return false;
                }
                if (!put(buffer.data(), static_cast<u64>(got))) return false;
                count -= static_cast<u64>(got);
            }
            return true;
        };
        if (r.size <= limit)
        {
            r.ok = copy(r.size);
        }
        else if (copy(head))
        {
            const u64 tail = limit - head;
            const u64 left_out = r.size - head - tail;
            std::string marker = fmt::format("\n[log history 0.29: %llu bytes left out here; the original file had %llu bytes]\n", left_out, r.size);
            std::string kept_part; // 0.34: the kept lines and a closing marker
            if (middle && scan_budget && *scan_budget)
            {
                // A failed read only costs the kept lines: the tail is still copied.
                const u64 window = std::min<u64>(left_out, *scan_budget);
                const u64 start = r.size - tail - window;
                utils::ps4::history_line_filter filter(middle);
                u64 scanned = 0;
                bool read_ok = ::lseek(in, static_cast<off_t>(start), SEEK_SET) == static_cast<off_t>(start);
                if (!read_ok)
                {
                    r.error = errno;
                    r.step = "seek";
                }
                while (read_ok && scanned < window)
                {
                    const ssize_t got = ::read(in, buffer.data(), std::min<u64>(window - scanned, buffer.size()));
                    if (got <= 0)
                    {
                        r.error = got < 0 ? errno : 0;
                        r.step = "read";
                        read_ok = false;
                        break;
                    }
                    scanned += static_cast<u64>(got);
                    filter.feed(buffer.data(), static_cast<usz>(got));
                }
                *scan_budget -= std::min(*scan_budget, scanned);
                if (read_ok)
                {
                    const std::string& kept = filter.kept();
                    const auto kept_lines = static_cast<u32>(std::count(kept.begin(), kept.end(), '\n'));
                    marker = fmt::format("\n[log history 0.34: %llu bytes left out here; the original file had %llu bytes. Of its last %llu bytes, the %u newest "
                        "error/PS4/AudioOut line(s) of %u follow%s]\n", left_out, r.size, scanned, kept_lines, filter.matched(),
                        filter.repeats() ? fmt::format("; %u more TODO line(s) left out (the first %u of each text are kept)", filter.repeats(),
                            utils::ps4::history_line_filter::todo_keep) : std::string());
                    kept_part = kept + "[log history 0.34: end of the kept lines; the end of the file follows]\n";
                }
            }
            if (put(marker.data(), marker.size()) && put(kept_part.data(), kept_part.size()))
            {
                if (::lseek(in, static_cast<off_t>(r.size - tail), SEEK_SET) == static_cast<off_t>(r.size - tail))
                {
                    r.ok = copy(tail);
                }
                else
                {
                    r.error = errno;
                    r.step = "seek";
                }
            }
        }
        ::close(in);
        if (::close(out) != 0 && r.ok)
        {
            r.error = errno;
            r.step = "close";
            r.ok = false;
        }
        return r;
    }

    // 0.33: resolve_path / resolve_path_may_not_exist. The PC frontend returns Qt's canonical path: one '/' between
    // segments, no '.' or '..' and no trailing '/'. System.cpp computes resolve_path(hdd0_game) + '/' and cuts the
    // title folder out of the resolved boot path at that length; vfs::get("/dev_hdd0/game/") ends in '/', so the
    // identity callback gave '.../game//' and every boot from /dev_hdd0/game (PSN titles, disc games installed to the
    // HDD, discs and ISOs with an installed update) lost the first letter of the folder in m_dir and argv[0].
    // Lexical only: links are not followed, and a path that does not exist keeps its value instead of becoming empty
    // (the ISO overlay device path is not on disk). A relative path is returned unchanged.
    std::string normalize_host_path(std::string_view path)
    {
        if (!path.starts_with('/')) return std::string(path);
        std::vector<std::string_view> parts;
        for (usz pos = 0; pos < path.size();)
        {
            const usz end = std::min(path.find('/', pos), path.size());
            const std::string_view part = path.substr(pos, end - pos);
            pos = end + 1;
            if (part.empty() || part == ".") continue;
            if (part == "..")
            {
                if (!parts.empty()) parts.pop_back();
                continue;
            }
            parts.push_back(part);
        }
        if (parts.empty()) return "/";
        std::string result;
        result.reserve(path.size());
        for (const std::string_view part : parts)
        {
            result += '/';
            result += part;
        }
        return result;
    }

    // 0.33: process-wide shared_mutex objects that outlive a session. thread_ctrl::emergency_exit ends a thread with
    // pthread_exit, which does not unwind on the PS4, so a lock guard of a thread that died in a fatal error never
    // runs; a hold left on one of these blocks the next boot in the same process (0.32 Wolverine: the /dev_hdd0 mount
    // lock). A shared_mutex waiter waits on the lock's own address, so an 'atomic waiter: address=' line that equals
    // an address printed here names the lock.
    struct global_lock
    {
        std::string name;
        const shared_mutex* lock;
    };

    std::vector<global_lock> global_locks()
    {
        std::vector<global_lock> locks;
        locks.push_back({"lv2_obj::g_mutex (lv2 scheduler)", &lv2_obj::g_mutex});
        locks.push_back({"id_manager::g_mutex (IDM)", &id_manager::g_mutex});
        locks.push_back({"s_cpu_lock (cpu_thread::suspend_all)", &ps4_cpu_lock()});
        locks.push_back({"s_memstats_mtx (sys_memory)", &ps4_memstats_lock()});
        locks.push_back({"s_perf_mutex (perf_meter)", &ps4_perf_lock()});
        for (const lv2_fs_mount_point* mp = &g_mp_sys_dev_root; mp; mp = mp->next)
            locks.push_back({fmt::format("mount point '%s' (sys_fs)", mp->root), &mp->mutex});
        locks.push_back({"mount point <no device> (sys_fs)", &g_mp_sys_no_device.mutex});
        return locks;
    }

    // "'name' at 0x... (elf+0x...): held exclusive, with waiters"; atomics only, any thread
    std::string describe_global_lock(const global_lock& entry)
    {
        const shared_mutex& lock = *entry.lock;
        const char* state = "busy (lock hand-over or broken state)";
        if (lock.is_free()) state = "free";
        else if (lock.ps4_looks_held(true)) state = lock.has_waiters() ? "held exclusive, with waiters" : "held exclusive";
        else if (lock.ps4_looks_held(false)) state = lock.has_waiters() ? "held shared, with waiters" : "held shared";
        const u64 address = reinterpret_cast<u64>(entry.lock);
        return fmt::format("'%s' at 0x%llx (elf+0x%llx): %s", entry.name, address, address - reinterpret_cast<u64>(__text_start), state);
    }

    std::vector<std::string> held_global_locks()
    {
        std::vector<std::string> held;
        for (const global_lock& entry : global_locks())
            if (!entry.lock->is_free()) held.push_back(describe_global_lock(entry));
        return held;
    }

    // Stop-timeout and STALL dumps: the known global locks that are not free, for the atomic waiter lines above them
    void report_global_locks()
    {
        const std::vector<std::string> held = held_global_locks();
        ps4_startup_checkpoint(fmt::format("global locks: %u of %u known process-wide locks not free (image base 0x%llx; elf+ = llvm-nm address)",
            held.size(), global_locks().size(), reinterpret_cast<u64>(__text_start)).c_str());
        for (const std::string& line : held) ps4_startup_checkpoint(("global lock " + line).c_str());
    }

    void report_guest_threads()
    {
        const std::string scheduler = lv2_obj::ps4_scheduler_snapshot();
        ps4_startup_checkpoint(scheduler.c_str());
        std::vector<std::string> samples;
        // Non-blocking like report_stall: this also runs on the Stop Watchdog thread exactly when a
        // guest thread is stuck, possibly holding (or with a writer queued on) the IDM lock.
        if (id_manager::g_mutex.try_lock_shared())
        {
            idm::select<named_thread<ppu_thread>>([&](u32 id, named_thread<ppu_thread>& ppu)
            {
                samples.push_back(fmt::format("PPU snapshot: id=0x%x name='%s' pc~=0x%x state=%s priority=%d",
                    id, ppu.get_name(), ppu.get_pc(), ppu.state.load(), ppu.prio.load().prio));
            }, idm::unlocked);
            idm::select<named_thread<spu_thread>>([&](u32 id, named_thread<spu_thread>& spu)
            {
                const auto events = spu.ch_events.load();
                samples.push_back(fmt::format("SPU snapshot: id=0x%x name='%s' pc~=0x%x state=%s events=0x%x mask=0x%x channel_wait=%u",
                    id, spu.get_name(), spu.get_pc(), spu.state.load(), static_cast<u32>(events.events),
                    static_cast<u32>(events.mask), static_cast<u32>(events.waiting)));
            }, idm::unlocked);
            id_manager::g_mutex.unlock_shared();
        }
        else
        {
            samples.emplace_back("thread snapshot skipped: IDM lock busy");
        }
        // get_pc() is the core's observational API; these are samples, not a
        // coherent register dump. Release the IDM lock before doing file I/O.
        for (const auto& entry : samples) ps4_startup_checkpoint(entry.c_str());
        ps4_log_atomic_waiters();
        report_global_locks();
    }

	struct main_call
	{
		std::function<void()> function;
		atomic_t<u32>* wake_up;
	};

	std::mutex s_main_calls_mutex;
	std::deque<main_call> s_main_calls;
    // 0.29: set when a call is queued for the main thread; the main loop sleeps on it while a
    // game runs (up to 8 ms) instead of polling every 1 ms.
    atomic_t<u32> s_main_wakeup{0};
    utils::ps4::lifecycle s_lifecycle;
    utils::ps4::stall_watchdog s_stall;
    // 0.33: native save data lists being shown (ps4_save_dialog). cellSaveData blocks the calling PPU thread until the
    // player picks an entry, so a game that renders on that thread stops flipping although nothing is frozen.
    std::atomic<u32> s_save_list_open{0};

    [[noreturn]] void exit_native_process()
    {
        ps4_startup_checkpoint("shutdown: emulator stopped; leave application");
        // The emulation threads have already been joined, so the log writer is idle.
        logs::listener::sync_all();
        // Never _Exit/_exit/exit/abort on PS4: libkernel _exit => SIGSYS, abort => SIGABRT,
        // both shown as CE-34878-0. Ask the shell to close us, else park.
        ps4_leave_application("frontend exit");
    }

    // 0.28: the console ran out of thread resources. Stopping the guest needs new threads (GracefulShutdown's
    // kill thread, Emulation Join Thread, Stop Watchdog); in 0.27 that std::thread threw and terminated the app with
    // the game frozen. Quiet the guest without creating threads, then ask the shell to close the application. Guest
    // threads are not joined here. (0.29: shared by the main loop and the synchronous stop waits below.)
    [[noreturn]] void leave_for_thread_exhaustion(const char* reason, unsigned long long internal_free)
    {
        // 0.31: the next boots of this game use fewer threads (boot policy). Written first and without allocating;
        // nothing is written when no game title is set (XMB, a failed boot).
        if (utils::ps4::adapt::note(utils::ps4::adapt::key_threads_low))
            ps4_fatal_note("adapt: ", "thread resources exhausted; the next boots of this game use a lighter thread budget (threads=low)");
        // 0.40: and the per-game safe mode goes one level up; the session record ends (the application is left)
        safe_mode::note_failure(safe_mode::fail_kind::threads);
        if (char title[utils::ps4::adapt::title_size]; utils::ps4::adapt::current_title(title))
            utils::ps4::adapt::write_record(title, utils::ps4::session_record::suffix, "end threads");
        ps4_startup_checkpoint(fmt::format("lifecycle: thread resources exhausted (%s; kernel internal memory free %llu bytes;"
            " flexible memory free %llu bytes); leaving application instead of stopping the guest",
            reason, internal_free, ps4_flexible_memory_free()).c_str());
        if (Emu.IsRunning()) Emu.Pause(false, false);
        exit_native_process();
    }

    // 0.29: a synchronous stop wait on the main thread (Emu.Kill in select_and_boot's failure
    // path, qt_events_aware_op under GracefulShutdown/stop with async_op=false) never ends when the 'Emulation Join
    // Thread' was refused: m_state stays 'stopping'. Leave the application as the main loop does. Only an exhaustion
    // reason is taken here; any other pending thread fatal stays for the main loop.
    void leave_if_threads_exhausted()
    {
        if (!ps4_thread_exhaustion_pending()) return;
        if (char reason[512]; ps4_take_thread_fatal(reason, sizeof(reason)))
            leave_for_thread_exhaustion(reason, ps4_internal_memory_free());
    }

    // 0.31: set once a thread died through emergency_exit in this session (it ends without unwinding, so its stack
    // and whatever it held are never returned), after a GPU device loss, or after a stop that did not finish. From
    // then on every XMB notice advises restarting the application before the next game. Main thread only.
    bool s_restart_recommended = false;

    // 0.33: a thread ended through emergency_exit in this process (taken by the main loop, or discarded before a
    // boot). Arms the boot gate of the post-stop lock audit (global_locks_allow_boot). Main thread only.
    bool s_thread_fatal_seen = false;

    // 0.34: the heap reached the governor's critical level in this process, or it is at its cap and spills into
    // flexible memory right now. musl never returns that heap, so the next game in this process starts short of
    // memory (Minecraft after Wolverine boots with the heap AT CAP): every later XMB notice advises a restart.
    void note_heap_restart_advice()
    {
        if (s_restart_recommended || !utils::ps4::governor::restart_advised()) return;
        s_restart_recommended = true;
        ps4_startup_checkpoint("lifecycle: heap critical or at its cap with fallback in this process; the XMB advises restarting the application");
    }

    // 0.39: start of the notices the XMB shows as the emulator-error dialog (ps4_launcher.cpp ps4_select_game); that
    // dialog itself asks to close and reopen the application.
    constexpr std::string_view emulator_error_notice = "Ocorreu um erro no emulador.";

    // 0.39: every notice of this file is built in the encoded form of util/ps4_xmb_notice.hpp (kind, flags, detail in
    // the interface language) and handed to ps4_select_game through xmb_text(): as is, since the launcher decodes such
    // notices (ps4_launcher.cpp read_notice); false hands it the 0.38 plain pt-BR texts instead (legacy_xmb_text),
    // which read_notice still recognizes. Plain texts (the launcher's own lines, the previous-session line) pass both
    // ways unchanged.
    constexpr bool xmb_decodes_notices = true;

    // Flag of an emulator_error notice: reopening the application does not help this game (an incomplete ISO, the same
    // fatal error again). xmb_notice::decode ignores flags it does not know.
    constexpr char flag_no_reopen = 'n';

    std::string emulator_error_encoded(std::string_view detail, bool reopen_helps = true)
    {
        std::string notice = utils::ps4::xmb_notice::encode(utils::ps4::xmb_notice::kind::emulator_error, detail);
        if (!reopen_helps) notice.insert(2, 1, flag_no_reopen); // after the marker and the kind
        return notice;
    }

    bool is_emulator_error(const std::string& notice)
    {
        return utils::ps4::xmb_notice::is_encoded(notice) ? utils::ps4::xmb_notice::decode(notice).k == utils::ps4::xmb_notice::kind::emulator_error
                                                          : notice.starts_with(emulator_error_notice);
    }

    // Plain notice with the restart advice appended. The XMB draws the notice on one 20 px line, so a long text is cut
    // to leave room for the advice.
    std::string plain_with_advice(const std::string& text, bool advised)
    {
        if (!advised || text.starts_with(emulator_error_notice)) return text;
        // 0.39: in the interface language, except for the notices the launcher replaces by its own pt-BR texts and only
        // searches for the pt-BR advice ("reiniciar" next to a firmware result, "Recomendado reiniciar" after an
        // out-of-memory stop)
        namespace vram = utils::ps4::vram;
        const bool launcher_reads = text.find("Firmware is missing") != std::string::npos || text.find("Firmware is too old") != std::string::npos ||
            text.starts_with(vram::stop_notice(vram::stop_cause::vram)) || text.starts_with(vram::stop_notice(vram::stop_cause::cpu_heap));
        const i18n::lang language = launcher_reads ? i18n::lang::pt_br : i18n::current();
        const std::string_view advice = i18n::tr(i18n::str::COM_RECOMENDADO_REINICIAR_APLICATIVO_ANTES_JOGAR, language);
        const std::string_view short_advice = i18n::tr(i18n::str::MAIN_RECOMENDADO_REINICIAR_APLICATIVO, language);
        if (text.empty()) return std::string(advice);
        if (text.size() <= 48) return text + " · " + std::string(advice);
        return ps4_fatal_detail::utf8_prefix(text, 70) + " · " + std::string(short_advice);
    }

    // The notice of 'text' (plain text becomes kind text) with the restart flag when a restart is advised. The
    // emulator-error, restart and clean-process notices ask to close and reopen the application by themselves.
    std::string xmb_notice(const std::string& text)
    {
        namespace xn = utils::ps4::xmb_notice;
        note_heap_restart_advice();
        if (text.empty() && !s_restart_recommended) return {};
        std::string notice = xn::is_encoded(text) ? text : xn::encode(xn::kind::text, text);
        if (!s_restart_recommended) return notice;
        const xn::decoded parts = xn::decode(notice);
        if (parts.restart_advised || parts.k == xn::kind::emulator_error || parts.k == xn::kind::emulator_restart || parts.k == xn::kind::clean_process)
            return notice;
        // Marker, kind, flags, separator: the flag joins the others (unknown ones are kept); a malformed text is rebuilt
        if (const std::size_t sep = notice.find(xn::separator, 1); sep == std::string::npos || sep < 2)
            return xn::encode(parts.k, parts.detail, true);
        notice.insert(2, 1, xn::flag_restart);
        return notice;
    }

    // The 0.38 plain form of a notice: the pt-BR starts and the advice the launcher compares (ps4_select_game)
    std::string legacy_xmb_text(const std::string& notice)
    {
        namespace xn = utils::ps4::xmb_notice;
        namespace vram = utils::ps4::vram;
        if (!xn::is_encoded(notice)) return notice;
        const xn::decoded parts = xn::decode(notice);
        switch (parts.k)
        {
        case xn::kind::emulator_error: return std::string(emulator_error_notice) + " · " + parts.detail;
        case xn::kind::emulator_restart: return "O emulador precisa reiniciar após o erro anterior: feche o aplicativo pelo botão PS";
        case xn::kind::clean_process: return std::string(ps4_restart_notice) + " · " + parts.detail;
        case xn::kind::boot_next: return std::string(ps4_boot_next_notice) + " · " + parts.detail;
        case xn::kind::firmware_missing:
            return plain_with_advice(i18n::trf(i18n::str::MAIN_FALHA_INICIAR, {fmt::format("%s", game_boot_result::firmware_missing)}), parts.restart_advised);
        case xn::kind::firmware_old:
            return plain_with_advice(i18n::trf(i18n::str::MAIN_FALHA_INICIAR, {fmt::format("%s", game_boot_result::firmware_version)}), parts.restart_advised);
        case xn::kind::vram_stop: return plain_with_advice(vram::stop_notice(vram::stop_cause::vram), parts.restart_advised);
        case xn::kind::heap_stop: return plain_with_advice(vram::stop_notice(vram::stop_cause::cpu_heap), parts.restart_advised);
        default: return plain_with_advice(parts.detail, parts.restart_advised);
        }
    }

    // The notice as ps4_select_game takes it
    std::string xmb_text(const std::string& notice)
    {
        return xmb_decodes_notices ? notice : legacy_xmb_text(notice);
    }

    // 0.31: gpu::lost() stays set for the rest of the process, so each device loss is handled once. Checked by the
    // main loop and after every stop wait, so a loss during a teardown is never mistaken for one in the next game.
    // Main thread only.
    bool s_gpu_lost_seen = false;

    // True the first time a device loss is seen.
    bool take_gpu_lost() noexcept
    {
        if (s_gpu_lost_seen || !utils::ps4::gpu::lost()) return false;
        s_gpu_lost_seen = true;
        s_restart_recommended = true;
        return true;
    }

    // 0.38: clean process between games. musl never returns its small heap, a thread that died in a fatal error keeps
    // what it held, and a stop that did not finish may still hold the previous game: in those cases the next game boots
    // only in a new process. The XMB asks to close the application and open it again (a PS3 system dialog), and the
    // chosen game is written to boot-next.txt, which the next start boots after a short countdown. The application never
    // closes itself (exit/abort end in CE-34878-0). boot-dirty.flag turns the rule off (tests).
    constexpr char boot_next_file[] = "/data/rpcs3/boot-next.txt";
    constexpr char boot_dirty_flag[] = "/data/rpcs3/boot-dirty.flag";
    u32 s_games_booted = 0;          // successful boots in this process (the memory check applies from the second)
    bool s_stop_unfinished = false;  // a stop gave up waiting for the emulator (main thread only)
    bool s_heap_stop_seen = false;   // the renderer stopped a game: the CPU heap could not serve it (main thread only)
    // 0.39 (main thread only): the game this process booted from boot-next.txt and the first reason line of the
    // previous session's fatal record; a thread fatal of that game with the same reason means reopening the
    // application does not help, so its refusal writes no boot-next.txt (no loop of the same failure).
    std::string s_boot_next_game;
    std::string s_previous_fatal_reason;
    bool s_same_fatal_again = false;
    // 0.39: an ISO image the game read past its end (an incomplete copy; Loader/ISO.cpp): booting it again in a new
    // process fails the same way, so its refusal writes no boot-next.txt (main thread only)
    std::string s_incomplete_image;
    // 0.39: the frozen emulation of this boot (a guest access violation) was already sent back to the XMB
    bool s_frozen_seen = false;

    // The game's file or folder name, without the folders above it and the .iso extension, for the XMB texts.
    std::string game_display_name(const std::string& path)
    {
        std::string name = path;
        while (name.size() > 1 && name.back() == '/') name.pop_back();
        if (const auto slash = name.find_last_of('/'); slash != std::string::npos) name.erase(0, slash + 1);
        if (name == "EBOOT.BIN") return game_display_name(fs::get_parent_dir(fs::get_parent_dir(path)));
        if (name.size() > 4 && fmt::to_lower(name.substr(name.size() - 4)) == ".iso") name.resize(name.size() - 4);
        return name.empty() ? std::string(i18n::tr(i18n::str::COM_JOGO)) : name;
    }

    // Writes the game to boot at the next start. False when it could not be written (logged).
    bool write_boot_next(const std::string& path)
    {
        const bool ok = fs::write_file(boot_next_file, fs::rewrite, path + "\n");
        ps4_startup_checkpoint(fmt::format("lifecycle: %s '%s' in boot-next.txt for the next start%s", ok ? "wrote" : "could NOT write", path,
            ok ? "" : fmt::format(" (%s)", fs::g_tls_error)).c_str());
        return ok;
    }

    // The game of boot-next.txt, removed at once (a failing boot never loops); "" when absent or the path is gone.
    std::string take_boot_next()
    {
        if (!fs::is_file(boot_next_file)) return {};
        std::string path;
        if (fs::file file{boot_next_file}) path = file.to_string();
        fs::remove_file(boot_next_file);
        while (!path.empty() && (path.back() == '\n' || path.back() == '\r' || path.back() == ' ')) path.pop_back();
        if (path.find('\n') != std::string::npos || (!fs::is_file(path) && !fs::is_dir(path)))
        {
            ps4_startup_checkpoint(fmt::format("lifecycle: boot-next.txt names '%s', which is not there; the XMB opens as usual", path).c_str());
            return {};
        }
        ps4_startup_checkpoint(fmt::format("lifecycle: boot-next.txt: '%s' boots after a %u s countdown (X now, Circle cancels)", path, ps4_boot_next_seconds).c_str());
        return path;
    }

    std::string boot_title_id(const std::string& path);
    std::string boot_title_name(const std::string& path);

    // Writes 'path' to boot-next.txt and returns the dialog note that tells so (or how to go on if it failed).
    // 0.39: the game is named by its PARAM.SFO title.
    std::string boot_next_note(const std::string& path)
    {
        return write_boot_next(path) ? i18n::trf(i18n::str::MAIN_ABRE_SOZINHO_RPCS3_ABERTO_NOVO, {boot_title_name(path)})
                                     : std::string(i18n::tr(i18n::str::MAIN_DEPOIS_ABRIR_RPCS3_NOVO_ESCOLHA));
    }

    // Why this process must not boot 'path' (pt-BR note for the dialog), or "" when it may. Main thread, right before
    // BootGame, after the previous stop finished and the XMB returned what it could (health::on_return_to_xmb).
    std::string clean_process_refusal(const std::string& path, std::string& log_reason)
    {
        static bool s_override_logged = false;
        if (fs::is_file(boot_dirty_flag))
        {
            if (!std::exchange(s_override_logged, true)) ps4_startup_checkpoint("lifecycle: boot-dirty.flag present: the clean-process rule is off");
            return {};
        }
        if (s_gpu_lost_seen)
        {
            log_reason = "the GPU device was lost in this process";
            return i18n::tr(i18n::str::MAIN_GPU_PAROU_RESPONDER_JOGO_ANTERIOR);
        }
        if (s_stop_unfinished)
        {
            log_reason = "a stop did not finish in this process";
            return i18n::tr(i18n::str::MAIN_JOGO_ANTERIOR_NAO_TERMINOU);
        }
        if (s_heap_stop_seen)
        {
            log_reason = "the renderer stopped a game because the CPU heap could not serve it";
            return i18n::tr(i18n::str::COM_MEMORIA_EMULADOR_ESGOTOU_JOGO_ANTERIOR);
        }
        if (utils::ps4::governor::level3_seen() || utils::ps4::governor::restart_advised())
        {
            log_reason = "the memory governor reached level 3 (an allocation found no heap room) or heap bytes are in flexible memory";
            return i18n::tr(i18n::str::COM_MEMORIA_EMULADOR_ESGOTOU_JOGO_ANTERIOR);
        }
        if (!s_games_booted) return {};
        namespace mb = utils::ps4::mem_budget;
        const std::string title = boot_title_id(path);
        const std::string text = title.empty() ? std::string() : utils::ps4::adapt::read_mem_record(title);
        const mb::record record = mb::parse_record(text.data(), text.size());
        const s64 room = utils::ps4::governor::room_now();
        const u64 need = mb::heap_need(record);
        ps4_startup_checkpoint(fmt::format("lifecycle: pre-boot memory check of '%s' (%s): heap room %lld MiB, needs %llu MiB (%s)", path,
            title.empty() ? "no title id" : title, room / s64{1 << 20}, need >> 20, record.valid ? "its memory record" : "no record: default").c_str());
        if (mb::boot_fits(false, room, record)) return {};
        log_reason = fmt::format("heap room %lld MiB below the %llu MiB the game needs", room / s64{1 << 20}, need >> 20);
        return i18n::trf(i18n::str::MAIN_ESTE_JOGO_PRECISA_CERCA_MB, {need >> 20, std::max<s64>(room, 0) / s64{1 << 20}});
    }

    // 0.29: previous-mesa.log belongs to the previous session only if this boot renamed mesa.log (else it is older).
    bool s_mesa_log_rotated = false;

    void apply_mesa_environment()
    {
        // Mesa (RADV/Zink/EGL) is linked into this eboot, so setenv() here reaches its
        // getenv() (orbis-compat/src/orbis_env.cpp). Runs before any EGL/Vulkan init:
        // Mesa reads these once, on its first log/driver initialization.
        const std::string log_dir = fs::get_log_dir();
        const std::string mesa_log = log_dir + "mesa.log";
        if (fs::is_file(mesa_log))
        {
            s_mesa_log_rotated = fs::rename(mesa_log, log_dir + "previous-mesa.log", true);
            if (!s_mesa_log_rotated) ps4_startup_checkpoint("Mesa log: could not preserve previous mesa.log");
        }
        ::setenv("MESA_LOG_FILE", mesa_log.c_str(), 0);
        ::setenv("MESA_LOG_LEVEL", "info", 0);
        ps4_startup_checkpoint(fmt::format("Mesa log: %s", mesa_log).c_str());

        // Mesa's on-disk shader cache: apply_mesa_shader_cache, right after this (a MESA_SHADER_CACHE_DIR of
        // mesa-env.txt wins).

        // Optional operator knobs, one KEY=VALUE per line (e.g. ORBIS_COUNT_API=1,
        // ORBIS_FRAME_STATS=1, ZINK_DEBUG=...). Malformed lines are ignored.
        fs::file file("/data/rpcs3/mesa-env.txt");
        if (!file) return;
        if (file.size() > 64 * 1024)
        {
            ps4_startup_checkpoint("Mesa environment: mesa-env.txt larger than 64 KiB; ignored");
            return;
        }
        u32 applied = 0, ignored = 0;
        std::string key, value;
        for (const std::string& line : fmt::split(file.to_string(), {"\n"}))
        {
            if (!ps4_fatal_detail::parse_env_line(line, key, value))
            {
                const auto first = line.find_first_not_of(" \t\r");
                if (first != umax && line[first] != '#') ignored++;
                continue;
            }
            if (applied >= 64)
            {
                ignored++;
                continue;
            }
            ::setenv(key.c_str(), value.c_str(), 1);
            applied++;
            ps4_startup_checkpoint(fmt::format("Mesa environment: %s=%s", key, value).c_str());
        }
        ps4_startup_checkpoint(fmt::format("Mesa environment: %u variables applied, %u malformed lines ignored", applied, ignored).c_str());
    }

    // 0.27: Mesa's on-disk shader cache is compiled in but stays off on the console without a cache folder (neither
    // $HOME nor $XDG_CACHE_HOME exists), so RADV/ACO recompiled every pipeline and interpreter variant (and Zink every
    // GL shader) on every boot. 0.28 made it opt-in after a ScePthread pool exhaustion in Demon's Souls 0.27, later
    // traced to 2 MiB stack exhaustion (handled by the own-stack rescue since 0.29-0.31).
    // 0.39 turned it on by default; 0.40 makes it opt-in again (the feature row mesa_cache: mesa-cache.flag, or the name
    // in features.txt; mesa-cache-off.flag and the global safe mode keep it off). Mesa's 'disk$' queue threads run
    // outside the RPCS3 thread registry and count against the console's thread cap: 0.39 sessions carried 5-8 threads
    // outside the registry instead of 2-3, and Mortal Kombat's movie demuxer and Demon's Souls' libsail thread were
    // refused. When on: Mesa's default cache database, bounded by MESA_SHADER_CACHE_MAX_SIZE and evicting old entries
    // (the single-file cache ignores that limit, so it is not selected). The variables are set before the first thread
    // of the process (musl's setenv may free the environment array a getenv on another thread reads, e.g. curl's proxy
    // lookup in the TLS self-test) and long before any EGL/Vulkan initialization (the first game boot), which reads
    // them, so the row is fixed for the life of the process. A folder that refuses writes (made by the GoldHEN FTP) makes
    // Mesa turn the cache off by itself; it is probed here and, when it refused, again after the permission repair.
    constexpr char mesa_cache_dir[] = "/data/rpcs3/cache/mesa/";
    bool s_mesa_cache_recheck = false; // the folder refused writes before the permission repair

    // "" when 'dir' exists (created if needed) and takes a file write (the folder may exist with another owner's
    // mode), else why not, for the log
    std::string mesa_cache_refusal(const std::string& dir)
    {
        if (!fs::create_path(dir)) return fmt::format("cannot be created (%s)", fs::g_tls_error);
        const std::string probe = dir + ".rpcs3-write-probe";
        int error = 0;
        if (const int fd = ::open(probe.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666); fd < 0)
        {
            error = errno;
        }
        else
        {
            if (::write(fd, "1", 1) != 1) error = errno ? errno : EIO;
            ::close(fd);
            ::unlink(probe.c_str());
        }
        if (!error) return {};
        struct ::stat st{};
        if (::stat(dir.c_str(), &st) != 0) return fmt::format("refuses writes (errno %d)", error);
        return fmt::format("refuses writes (errno %d, mode 0%o uid %u; made by the GoldHEN FTP?)", error, static_cast<u32>(st.st_mode & 07777), static_cast<u32>(st.st_uid));
    }

    // After safe_mode::init_process(), which fixes the row for the process.
    void apply_mesa_shader_cache()
    {
        const char* const env_dir = ::getenv("MESA_SHADER_CACHE_DIR");
        const bool env_set = env_dir && env_dir[0];
        // The reason only names the file in the line; the value is the one init_process() fixed
        const safe_mode::reason why = safe_mode::why(safe_mode::read_inputs(), safe_mode::row(safe_mode::feature::mesa_cache));
        if (!safe_mode::enabled(safe_mode::feature::mesa_cache))
        {
            const char* const off = why == safe_mode::reason::kill_flag ? "Mesa shader cache: off (mesa-cache-off.flag)"
                : why == safe_mode::reason::safe_level ? "Mesa shader cache: off (global safe mode, safe-mode.flag)"
                : "Mesa shader cache: off (default; mesa-cache.flag turns it on: fewer pipeline compiles per boot, more threads)";
            // 0.38 behaviour: no cache folder is set; a folder of mesa-env.txt is still Mesa's own choice
            if (env_set)
                ps4_startup_checkpoint(fmt::format("%s; MESA_SHADER_CACHE_DIR=%s from mesa-env.txt is used by Mesa as set", off, env_dir).c_str());
            else
                ps4_startup_checkpoint(off);
            return;
        }
        if (env_set)
        {
            ps4_startup_checkpoint(fmt::format("Mesa shader cache: MESA_SHADER_CACHE_DIR=%s from mesa-env.txt kept", env_dir).c_str());
            return;
        }
        const std::string refusal = mesa_cache_refusal(mesa_cache_dir);
        ::setenv("MESA_SHADER_CACHE_DIR", mesa_cache_dir, 0);
        ::setenv("MESA_SHADER_CACHE_MAX_SIZE", "512M", 0);
        if (refusal.empty())
        {
            // The 0.28-0.38 line, then where the switch came from
            ps4_startup_checkpoint(fmt::format("Mesa shader cache: %s (512 MiB)%s", mesa_cache_dir,
                why == safe_mode::reason::enable_flag ? "; Mesa's cache database; mesa-cache.flag" : "; Mesa's cache database; features.txt").c_str());
            return;
        }
        s_mesa_cache_recheck = true;
        ps4_startup_checkpoint(fmt::format("Mesa shader cache: %s %s; checked again after the permission repair (Mesa turns the cache off by itself while it refuses)",
            mesa_cache_dir, refusal).c_str());
    }

    // After the permission repair: whether the cache folder that refused writes takes them now (no setenv here)
    void recheck_mesa_shader_cache()
    {
        if (!std::exchange(s_mesa_cache_recheck, false)) return;
        const std::string refusal = mesa_cache_refusal(mesa_cache_dir);
        const std::string line = refusal.empty() ? fmt::format("Mesa shader cache: %s takes writes after the permission repair: on (512 MiB)", mesa_cache_dir)
            : fmt::format("Mesa shader cache: %s still %s after the permission repair: Mesa turns the cache off by itself this session", mesa_cache_dir, refusal);
        ps4_startup_checkpoint(line.c_str());
    }

    // 0.36: the system suspends an application (PS -> Close application, rest mode) only once the process has called
    // sceGnmSubmitDone() after its GPU submissions; otherwise phase 3 of the suspend times out and the kernel ends the
    // app with 0xa0d0c00a (CPU_FAULT_SUBMITDONE_TIMEOUT_IN_SUSPEND_ASYNC, shown as CE-34878-0). Mesa's WSI calls it per
    // present only with ORBIS_SUBMIT_DONE, and a paused, stalled or dead renderer presents nothing. This thread calls
    // it from outside the renderer: once no new submission has been handed out for 0.5 s (again when the sequence
    // moved or after 1 s), and at most every 250 ms while sceGnmAreSubmitsAllowed() reports 0. It never takes
    // vk::g_submit_mutex: a submit stalled by a pending suspend could be holding it.
    struct gnm_keepalive_state
    {
        std::atomic<bool> running{false};
        std::atomic<bool> pending_trigger_off{false};
        std::atomic<s32> allowed{1};
        std::atomic<s32> last_error{0};
        std::atomic<u64> seq{0};
        std::atomic<u64> calls{0};
        std::atomic<u64> idle_calls{0};
        std::atomic<u64> pending_calls{0};
        std::atomic<u64> failures{0};
        std::atomic<u64> total_us{0};
        std::atomic<u64> worst_us{0};
        std::atomic<u64> last_call_at{0}; // process time the last call returned (0: none yet)
        std::atomic<u64> call_started{0}; // process time the call in progress began (0: none)
    };
    gnm_keepalive_state s_gnm;

    void* gnm_keepalive_main(void*)
    {
        pthread_set_name_np(pthread_self(), "GNM keepalive");
        // One step above the guest threads, so busy cores never starve it; it sleeps 100 ms per pass
        if (const int base = ps4_thread_base_native_priority(); base - 16 >= 256)
            scePthreadSetprio(scePthreadSelf(), base - 16);
        utils::ps4::health::gnm_keepalive_policy policy(sceKernelGetProcessTime());
        u32 transition_lines = 0, call_lines = 0;
        bool submissions_seen = false;
        for (;;)
        {
            sceKernelUsleep(100'000);
            const u64 now = sceKernelGetProcessTime();
            const u64 seq = ac_orbis_gpu_seq_now();
            // Not queried before the first submission (XMB only: nothing to suspend)
            const s32 allowed = seq ? sceGnmAreSubmitsAllowed() : 1;
            const utils::ps4::health::gnm_call what = policy.step(now, seq, allowed);
            if (!seq) continue;
            s_gnm.seq.store(seq, std::memory_order_relaxed);
            const s32 before = s_gnm.allowed.exchange(allowed, std::memory_order_relaxed);
            if (!submissions_seen)
            {
                submissions_seen = true;
                ps4_startup_checkpoint(fmt::format("gnm keepalive: GPU submissions started (seq %llu); submits allowed %d", seq, allowed).c_str());
            }
            else if (allowed != before && transition_lines < 16)
            {
                transition_lines++;
                ps4_startup_checkpoint(fmt::format("gnm keepalive: submits allowed -> %d (seq %llu, %llu ms since the last new submission)",
                    allowed, seq, (now - policy.seq_changed_at) / 1000).c_str());
            }
            if (policy.gave_up_now)
            {
                s_gnm.pending_trigger_off.store(true, std::memory_order_relaxed);
                ps4_startup_checkpoint("gnm keepalive: sceGnmAreSubmitsAllowed stayed 0 for 30 s without a suspend; that trigger is off (the idle trigger stays)");
            }
            if (what == utils::ps4::health::gnm_call::none) continue;
            const bool pending = what == utils::ps4::health::gnm_call::pending;
            const char* const why = pending ? "submits not allowed (suspend pending?)" : "GPU idle";
            const u64 t0 = sceKernelGetProcessTime();
            s_gnm.call_started.store(t0, std::memory_order_relaxed);
            // Blocks here while the system suspends the process: intended
            const s32 rc = sceGnmSubmitDone();
            const u64 t1 = sceKernelGetProcessTime();
            s_gnm.call_started.store(0, std::memory_order_relaxed);
            const u64 took = t1 - t0;
            policy.called(seq, t1);
            const u64 calls = s_gnm.calls.fetch_add(1, std::memory_order_relaxed) + 1;
            (pending ? s_gnm.pending_calls : s_gnm.idle_calls).fetch_add(1, std::memory_order_relaxed);
            s_gnm.total_us.fetch_add(took, std::memory_order_relaxed);
            if (took > s_gnm.worst_us.load(std::memory_order_relaxed)) s_gnm.worst_us.store(took, std::memory_order_relaxed);
            s_gnm.last_call_at.store(t1, std::memory_order_relaxed);
            if (rc < 0)
            {
                s_gnm.failures.fetch_add(1, std::memory_order_relaxed);
                s_gnm.last_error.store(rc, std::memory_order_relaxed);
            }
            if (call_lines < 16 && (calls <= 3 || rc < 0 || pending))
            {
                call_lines++;
                ps4_startup_checkpoint(fmt::format("gnm keepalive: sceGnmSubmitDone (%s) -> 0x%08x in %llu us; seq %llu, submits allowed %d",
                    why, static_cast<u32>(rc), took, seq, allowed).c_str());
            }
        }
        return nullptr;
    }

    // Before any Vulkan/EGL initialization. Off: /data/rpcs3/no-gnm-keepalive.flag
    void start_gnm_keepalive()
    {
        if (fs::is_file("/data/rpcs3/no-gnm-keepalive.flag"))
        {
            ps4_startup_checkpoint("gnm keepalive: off (no-gnm-keepalive.flag); closing the application from the PS menu after a game ends in CE-34878-0 (0xa0d0c00a)");
            return;
        }
        pthread_attr_t attr;
        int error = pthread_attr_init(&attr);
        if (error == 0)
        {
            pthread_attr_setstacksize(&attr, 64 * 1024);
            pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
            pthread_t thread{};
            error = pthread_create(&thread, &attr, gnm_keepalive_main, nullptr);
            pthread_attr_destroy(&attr);
        }
        s_gnm.running.store(error == 0, std::memory_order_relaxed);
        if (error == 0)
            ps4_startup_checkpoint("gnm keepalive: started (100 ms tick; sceGnmSubmitDone after 0.5 s without a new GPU submission, then every 1 s while idle, every 250 ms while submits are not allowed; no-gnm-keepalive.flag disables)");
        else
            ps4_startup_checkpoint(fmt::format("gnm keepalive: thread NOT created (error %d); closing the application from the PS menu after a game ends in CE-34878-0 (0xa0d0c00a)", error).c_str());
    }

    // 60 s report line; empty while the keepalive is off
    std::string gnm_keepalive_report()
    {
        if (!s_gnm.running.load(std::memory_order_relaxed)) return {};
        const u64 now = sceKernelGetProcessTime();
        const u64 calls = s_gnm.calls.load(std::memory_order_relaxed);
        const u64 last = s_gnm.last_call_at.load(std::memory_order_relaxed);
        std::string line = fmt::format("gnm: submitDone calls %llu (idle %llu, suspend-pending %llu), failures %llu (last 0x%08x), mean %llu us worst %llu us, submits allowed %d, last call %s, GPU submissions seq %llu",
            calls, s_gnm.idle_calls.load(std::memory_order_relaxed), s_gnm.pending_calls.load(std::memory_order_relaxed),
            s_gnm.failures.load(std::memory_order_relaxed), static_cast<u32>(s_gnm.last_error.load(std::memory_order_relaxed)),
            calls ? s_gnm.total_us.load(std::memory_order_relaxed) / calls : 0, s_gnm.worst_us.load(std::memory_order_relaxed),
            s_gnm.allowed.load(std::memory_order_relaxed), last && now >= last ? fmt::format("%llu ms ago", (now - last) / 1000) : std::string("never"),
            s_gnm.seq.load(std::memory_order_relaxed));
        if (const u64 started = s_gnm.call_started.load(std::memory_order_relaxed); started && now >= started)
            fmt::append(line, "; a call has been running for %llu ms", (now - started) / 1000);
        if (s_gnm.pending_trigger_off.load(std::memory_order_relaxed))
            line += "; suspend-pending trigger off";
        return line;
    }

    std::string describe_ppu_wait(ppu_thread& ppu)
    {
        // Diagnostic reads of another thread's state: values may be stale, never modified.
        const char* func = ppu.current_function;
        std::string out;
        if (!func)
        {
            if (const char* last = ppu.last_function) fmt::append(out, " last=%s", last);
            return out;
        }
        fmt::append(out, " in=%s(0x%llx, 0x%llx, 0x%llx, 0x%llx)", func,
            ppu.syscall_args[0], ppu.syscall_args[1], ppu.syscall_args[2], ppu.syscall_args[3]);
        const std::string_view name{func};
        if (name == "sys_event_queue_receive" || name == "sys_event_queue_tryreceive")
        {
            if (auto queue = idm::get_unlocked<lv2_obj, lv2_event_queue>(static_cast<u32>(ppu.syscall_args[0])))
            {
                std::unique_lock lock(queue->mutex, std::try_to_lock);
                if (lock.owns_lock())
                    fmt::append(out, " queue{key=0x%llx type=%u size=%u pending=%u}", queue->key, queue->type, queue->size, queue->events.size());
                else
                    fmt::append(out, " queue{key=0x%llx busy}", queue->key);
            }
            else
            {
                fmt::append(out, " queue{missing}");
            }
        }
        else if (name == "sys_event_flag_wait")
        {
            if (auto flag = idm::get_unlocked<lv2_obj, lv2_event_flag>(static_cast<u32>(ppu.syscall_args[0])))
                fmt::append(out, " flag{pattern=0x%llx want=0x%llx mode=0x%llx}", flag->pattern.load(), ppu.syscall_args[1], ppu.syscall_args[2]);
        }
        return out;
    }

    // 0.34: saved LRs of up to 8 frames of the r1 back chain (PPC64 ELF ABI: back chain at 0(sp), LR save slot at
    // 16(sp)), each frame checked readable and above the previous one: names the guest loop a thread spins in. The
    // first entry can be stale (a leaf function has not stored its LR); the lr register is printed beside it.
    // 0.39: read through the sudo view: the main thread is not a thread_ctrl thread, so a page the texture cache or
    // ZCULL protected on the host would end the app instead of being handled.
    std::string describe_ppu_back_chain(const ppu_thread& ppu)
    {
        std::string out;
        u64 sp = ppu.gpr[1];
        for (u32 depth = 0; depth < 8; depth++)
        {
            if (sp % 16 || sp > u32{umax} - 24 || !vm::check_addr(static_cast<u32>(sp), vm::page_readable, 24)) break;
            const u64 next = *vm::get_super_ptr<u64>(static_cast<u32>(sp));
            const u64 saved_lr = *vm::get_super_ptr<u64>(static_cast<u32>(sp + 16));
            fmt::append(out, "%s0x%x", depth ? "," : " chain=", static_cast<u32>(saved_lr));
            if (next <= sp) break;
            sp = next;
        }
        return out;
    }

    // 0.34: what an SPU that is not blocked executes and polls: 16 LS words around pc (pc in brackets), r3-r10 in SPU
    // word order (preferred slot first), its last MFC command and the 16 bytes at that effective address (checked).
    // Diagnostic reads of another thread's state: values may be stale, nothing is modified.
    std::string describe_running_spu(const spu_thread& spu)
    {
        constexpr u32 ls_mask = SPU_LS_SIZE - 4;
        const u32 pc = spu.pc & ls_mask;
        const u32 first = (pc - 0x20) & ls_mask;
        std::string out = fmt::format(" ls@0x%05x:", first);
        for (u32 i = 0; i < 16; i++)
        {
            const u32 lsa = (first + i * 4) & ls_mask;
            const u32 word = spu._ref<u32>(lsa);
            if (lsa == pc) fmt::append(out, " [%08x]", word);
            else fmt::append(out, " %08x", word);
        }
        for (u32 r = 3; r <= 10; r++)
        {
            const v128& value = spu.gpr[r];
            fmt::append(out, " r%u=%08x%08x%08x%08x", r, value._u32[3], value._u32[2], value._u32[1], value._u32[0]);
        }
        const spu_mfc_cmd cmd = spu.ch_mfc_cmd;
        fmt::append(out, " last_mfc{cmd=%s tag=%u size=0x%x lsa=0x%05x eal=0x%08x}", cmd.cmd, cmd.tag, cmd.size, cmd.lsa, cmd.eal);
        if (vm::check_addr(cmd.eal, vm::page_readable, 16))
        {
            // 0.39: sudo view (see describe_ppu_back_chain)
            u8 bytes[16];
            std::memcpy(bytes, vm::get_super_ptr<u8>(cmd.eal), sizeof(bytes));
            out += " at_eal=";
            for (const u8 byte : bytes) fmt::append(out, "%02x", byte);
        }
        else
        {
            out += " at_eal=unmapped";
        }
        return out;
    }

    // 0.37: an SPU thread of a SPURS kernel group and the workload its kernel runs: SpursKernelContext lives at LS 0x100
    // (cellSpurs.h), spuNum at LS 0x1C8 and wklCurrentId at LS 0x1DC (big-endian words).
    struct spurs_kernel
    {
        u32 spurs = 0, id = 0, spu_num = 0, wid = 0;
    };

    // Under the IDM lock: the SPURS instance this SPU thread serves (at most 8 kept) and its kernel's current workload.
    void collect_spurs(const spu_thread& spu, u32 id, std::vector<u32>& instances, std::vector<spurs_kernel>& kernels)
    {
        // SPUThread.cpp marks a thread outside a SPURS kernel group with 0 - 0x80
        const u32 spurs = spu.spurs_addr;
        if (!spurs || spurs == 0u - 0x80) return;
        if (instances.size() < 8 && std::find(instances.begin(), instances.end(), spurs) == instances.end()) instances.push_back(spurs);
        if (kernels.size() < 32) kernels.push_back({spurs, id, static_cast<u32>(spu._ref<u32>(0x1c8)), static_cast<u32>(spu._ref<u32>(0x1dc))});
    }

    // The SPURS part of a STALL or RSX-timeout dump, per instance: the workload flag state (0.36: CellSpurs header:
    // wklReadyCount1 at 0x00, the SPURS2 ready counts of wids 16-31 at 0x10, wklFlag.flag at 0x6c, wklSignal1 at 0x70,
    // wklFlagReceiver at 0x77, wklSignal2 at 0x78; a workload waiting for the flag stays unready while the flag is unset
    // or another receiver holds it); 0.37: the first two 128-byte lines of the header in hex (line 0: ready counts,
    // contention, pending, flag, signals, idle bits; line 1: workload state and priorities) and each kernel's current
    // workload. Reads bypass page protection (the texture cache may have locked these pages).
    void append_spurs_lines(std::vector<std::string>& lines, const char* prefix, const std::vector<u32>& instances, const std::vector<spurs_kernel>& kernels)
    {
        for (const u32 spurs : instances)
        {
            if (!vm::check_addr(spurs, 0, 0x80))
            {
                lines.push_back(fmt::format("%s SPURS 0x%x: header not mapped", prefix, spurs));
                continue;
            }
            const u32 flag = *vm::get_super_ptr<u32>(spurs + 0x6c);
            const u16 signal1 = *vm::get_super_ptr<u16>(spurs + 0x70);
            const u8 receiver = *vm::get_super_ptr<u8>(spurs + 0x77);
            const u16 signal2 = *vm::get_super_ptr<u16>(spurs + 0x78);
            std::string line = fmt::format("%s SPURS 0x%x: wklFlag=0x%08x receiver=%u", prefix, spurs, flag, receiver);
            if (receiver < 32)
                fmt::append(line, " readyCount[%u]=%u", receiver, *vm::get_super_ptr<u8>(spurs + (receiver < 16 ? receiver : 0x10 + (receiver & 0xf))));
            fmt::append(line, " signal1=0x%04x signal2=0x%04x", signal1, signal2);
            lines.push_back(std::move(line));
            for (u32 index = 0; index < 2; index++)
            {
                const u32 addr = spurs + index * 128;
                if (index && !vm::check_addr(addr, 0, 128)) break;
                std::string hex = fmt::format("%s SPURS 0x%x line%u: ", prefix, spurs, index);
                const u8* bytes = vm::get_super_ptr<u8>(addr);
                for (u32 i = 0; i < 128; i++) fmt::append(hex, "%02x", bytes[i]);
                lines.push_back(std::move(hex));
            }
            std::string running = fmt::format("%s SPURS 0x%x kernels:", prefix, spurs);
            u32 count = 0;
            for (const spurs_kernel& kernel : kernels)
                if (kernel.spurs == spurs)
                    fmt::append(running, "%s SPU id=0x%x spuNum=%u wklCurrentId=0x%x", count++ ? ";" : "", kernel.id, kernel.spu_num, kernel.wid);
            if (count) lines.push_back(std::move(running));
        }
    }

    // 0.40: " last: <name>(<result>)@<address> ..." of a PPU thread's newest HLE/LV2 calls (at most 8, newest first), for
    // its STALL line; empty before its first call. The caller holds the IDM lock (the thread stays alive).
    std::string recent_calls_text(const ppu_thread& ppu)
    {
        ps4_hle_call calls[8];
        const u32 n = ps4_ppu_recent_calls(ppu, calls, static_cast<u32>(std::size(calls)));
        if (!n) return {};
        std::string text = " last:";
        for (u32 i = 0; i < n; i++) fmt::append(text, " %s(0x%llx)@0x%x", calls[i].name ? calls[i].name : "?", calls[i].ret, calls[i].cia);
        return text;
    }

    void report_stall(const std::string& reason)
    {
        // Always-on stall dump.
        // Main thread only (g_fxo is reset on the main thread, so the RSX object stays valid).
        // Bounded and non-blocking: the thread list is read only if the IDM lock is free,
        // lv2 and queue mutexes are try-locked, guest state is never modified, and no
        // signals are sent to emulation threads.
        std::string headline = "STALL: " + reason;
        // 0.34: a freeze that follows an NV406E semaphore the RSX gave up on (GoW Ascension 0.33)
        if (const u64 timeout_at = rsx::g_ps4_rsx_counters.sema_timeout_at.load())
        {
            const u64 now = get_system_time();
            if (now >= timeout_at && now - timeout_at < 10'000'000)
                fmt::append(headline, "; nv406e timeout in the last 10 s (%.1f s ago, %llu in this process)",
                    (now - timeout_at) / 1'000'000., rsx::g_ps4_rsx_counters.sema_timeouts.load());
        }
        ps4_startup_checkpoint(headline.c_str());
        {
            char memory_line[512];
            ps4_memory_report(memory_line, sizeof(memory_line));
            ps4_startup_checkpoint(memory_line);
            ps4_startup_checkpoint(fmt::format("kernel internal memory: %llu bytes free", ps4_internal_memory_free()).c_str());
        }
        static std::unordered_map<u32, u64> s_spu_blocks; // block_counter at the previous dump
        std::vector<std::string> lines;
        lines.reserve(64);
        std::vector<u32> spurs_instances; // 0.36: SPURS instances the SPU kernels run for (at most 8)
        std::vector<spurs_kernel> spurs_kernels; // 0.37: the workload each of their kernels runs
        if (id_manager::g_mutex.try_lock_shared())
        {
            const u64 guest_now = get_guest_system_time();
            idm::select<named_thread<ppu_thread>>([&](u32 id, named_thread<ppu_thread>& ppu)
            {
                const auto state = ppu.state.load();
                std::string line = fmt::format("STALL PPU id=0x%x '%s' tid=0x%llx state=%s prio=%d pc~=0x%x cia=0x%08x lr=0x%08x sp=0x%08x r3=0x%llx r4=0x%llx%s",
                    id, ppu.get_name(), thread_ctrl::get_native_id(ppu), state, ppu.prio.load().prio, ppu.get_pc(), ppu.cia,
                    static_cast<u32>(ppu.lr), static_cast<u32>(ppu.gpr[1]), ppu.gpr[3], ppu.gpr[4], describe_ppu_wait(ppu));
                if (const u64 since = ppu.start_time; !!(state & cpu_flag::wait) && since && guest_now > since)
                    fmt::append(line, " sleeping=%.1fs", (guest_now - since) / 1'000'000.);
                // 0.37: host locks it holds while blocked or parked by lv2, and the one it waits for (lv2.cpp)
                if (const std::string locks = lv2_obj::ps4_lock_state_text(ppu); !locks.empty()) line += " " + locks;
                line += describe_ppu_back_chain(ppu);
                line += recent_calls_text(ppu);
                lines.push_back(std::move(line));
            }, idm::unlocked);
            idm::select<named_thread<spu_thread>>([&](u32 id, named_thread<spu_thread>& spu)
            {
                const auto state = spu.state.load();
                const auto events = spu.ch_events.load();
                const auto status = spu.status_npc.load();
                const u64 blocks = spu.block_counter;
                u64& previous = s_spu_blocks[id];
                const u64 delta = blocks >= previous ? blocks - previous : blocks;
                previous = blocks;
                lines.push_back(fmt::format("STALL SPU id=0x%x '%s' tid=0x%llx state=%s pc~=0x%05x status=0x%x npc=0x%x blocks=%llu dblocks=%llu "
                    "events=0x%x mask=0x%x channel_wait=%u tagmask=0x%x tagstat=%u:0x%x mfcq=%u raddr=0x%x in_mbox=%u out_mbox=%u out_intr=%u",
                    id, spu.get_name(), thread_ctrl::get_native_id(spu), state, spu.get_pc(), status.status, status.npc,
                    blocks, delta, static_cast<u32>(events.events), static_cast<u32>(events.mask), static_cast<u32>(events.waiting),
                    spu.ch_tag_mask, spu.ch_tag_stat.get_count(), spu.ch_tag_stat.get_value(), spu.mfc_size, spu.raddr,
                    spu.ch_in_mbox.get_count(), spu.ch_out_mbox.get_count(), spu.ch_out_intr_mbox.get_count()));
                // 0.34: an SPU that is neither stopped nor blocked in a channel or lv2 wait is spinning on something
                if (!(state & (cpu_flag::wait + cpu_flag::stop + cpu_flag::exit)) && !events.waiting)
                    lines.push_back(fmt::format("STALL SPU id=0x%x running:%s", id, describe_running_spu(spu)));
                collect_spurs(spu, id, spurs_instances, spurs_kernels);
            }, idm::unlocked);
            idm::select<lv2_spu_group>([&](u32 id, lv2_spu_group& group)
            {
                lines.push_back(fmt::format("STALL SPU group id=0x%x '%s' run_state=%u running=%u spurs_running=%u stop_count=%u",
                    id, group.name, static_cast<u32>(group.run_state.load()), group.running.load(),
                    group.spurs_running.load(), group.stop_count.load()));
            }, idm::unlocked);
            id_manager::g_mutex.unlock_shared();
        }
        else
        {
            lines.emplace_back("STALL: thread list busy (IDM lock held); thread dump skipped this time");
        }
        append_spurs_lines(lines, "STALL", spurs_instances, spurs_kernels);
        // File I/O only after the IDM lock is released.
        for (const auto& line : lines) ps4_startup_checkpoint(line.c_str());
        // 0.39: the RSX line and the RSX host stack sample come first, before the scheduler, vm, wait-state and label
        // lines: a fault in one of those optional readers can no longer lose the one line that names where the RSX is.
        const auto rsx = rsx::get_current_renderer();
        if (rsx)
        {
            std::string line = fmt::format("STALL RSX: guest_flips=%llu vblank=%llu fifo_state=%u in_begin_end=%u flip_status=%u pending_events=0x%llx",
                __atomic_load_n(&rsx->int_flip_index, __ATOMIC_RELAXED), rsx->vblank_count.load(),
                static_cast<u32>(rsx->performance_counters.state), rsx->in_begin_end, rsx->flip_status, rsx->unsent_gcm_events.load());
            if (const auto ctrl = rsx->ctrl)
            {
                const u32 get = ctrl->get.load();
                fmt::append(line, " get=0x%x put=0x%x", get, ctrl->put.load());
                // 0.26: the command word at get (method/count) - which handler the RSX is in. 0.39: sudo view (the
                // command buffer page may be protected on the host).
                if (const u32 ea = rsx->iomap_table.get_addr(get); ea != umax && vm::check_addr(ea, vm::page_readable, 8))
                {
                    const u32 cmd = *vm::get_super_ptr<u32>(ea);
                    const u32 arg = *vm::get_super_ptr<u32>(ea + 4);
                    fmt::append(line, " cmd@get=0x%08x(method=0x%04x count=%u) arg0=0x%08x", cmd, cmd & 0x1ffc, (cmd >> 18) & 0x7ff, arg);
                }
            }
            ps4_startup_checkpoint(line.c_str());
            // 0.26: where the RSX host thread is (RIP + candidate return addresses into rpcs3_ps4.elf).
            ps4_startup_checkpoint(("STALL RSX thread: " + ps4_sample_thread_stack(rsx::ps4_rsx_native_thread())).c_str());
        }
        ps4_startup_checkpoint(lv2_obj::ps4_scheduler_snapshot().c_str());
        // 0.37: the threads that hold vm passive-lock slots and the range lock bits (vm.cpp; lock-free reads)
        ps4_startup_checkpoint(("STALL vm: " + vm::ps4_passive_lock_owners_text()).c_str());
        if (rsx)
        {
            // 0.29: RSX loop heartbeat age, last FIFO method/argument and what the RSX thread is
            // blocked on (wait reason bits and for how long). Atomics only: safe while the RSX or the IDM lock is stuck.
            ps4_startup_checkpoint(rsx::ps4_rsx_wait_state().c_str());
            // 0.34: the last RSX label operations (semaphore acquires/releases, label writes) with each label's
            // current value; atomics and checked guest reads only.
            ps4_startup_checkpoint(rsx::ps4_label_ring_text().c_str());
        }
        ps4_log_atomic_waiters();
        report_global_locks();
    }

    // 0.31: whether the boot progress text is shown, checked once per second by the stall watchdog. While it is shown
    // the game is compiling (or loading), which is never reported as a stall. When it goes away after a PPU/SPU stage
    // (scanning, compiling, linking, building the SPU cache), the heap the compile workers left behind is released
    // (health::on_stage_change). The text itself is copied only while no PPU/SPU stage was seen yet.
    bool poll_compile_stage()
    {
        static bool s_compile_stage = false;
        if (g_progr_text)
        {
            if (!s_compile_stage)
            {
                const std::string text = g_progr_text;
                s_compile_stage = text.find("PPU") != std::string::npos || text.find("SPU") != std::string::npos;
            }
            return true;
        }
        if (s_compile_stage)
        {
            s_compile_stage = false;
            utils::ps4::health::on_stage_change();
        }
        return false;
    }

    // 0.34: a first boot compiles every PPU module (6-45 minutes on the Jaguar; GoW Ascension 13 min, Lollipop ~50 min
    // on a Slim) and players closed the app thinking it hung. Main thread, from the 1 s watchdog pass.
    struct first_boot_compile
    {
        bool toast_shown = false; // once per game boot
        u64 stage_since = 0;      // process time the PPU compile text was first seen (0 = not in that stage)
        u32 done_at_start = 0;    // g_progr_pdone then
        std::string eta;          // appended to the 60 s 'progress:' line while the stage lasts
        u64 minutes_left = 0;     // 0.40: the same estimate for the heartbeat (0 = no measured rate yet)
    };
    first_boot_compile s_first_compile;

    // From the first time the progress text shows the PPU compile stage until the progress dialog closes (nested module
    // dialogs switch the text back and forth meanwhile): the rate measured since (modules per minute) and the time left
    // for the 60 s progress line. Once per boot, when 40 or more modules are compiled, one toast with the count and an
    // estimate: after 30 s, with the measured rate once 2 modules finished, else (after 90 s at the latest) with 4
    // modules/min, the rate of a base PS4 on 2 workers (Minecraft: 88 modules in 21.4 min).
    void poll_first_boot_compile(u64 now, bool text_shown)
    {
        auto& s = s_first_compile;
        if (!text_shown)
        {
            s.stage_since = 0;
            s.eta.clear();
            s.minutes_left = 0;
            return;
        }
        const u32 total = g_progr_ptotal.load();
        const u32 done = g_progr_pdone.load();
        if (!s.stage_since)
        {
            // 0.39: read at each check (once per second): the overlay texts follow the interface language
            if (g_progr_text.operator std::string() != get_localized_string(localized_string_id::PROGRESS_DIALOG_COMPILING_PPU_MODULES)) return;
            s.stage_since = now;
            s.done_at_start = std::min(done, total);
            return;
        }
        const u64 elapsed = now - s.stage_since;
        const u32 modules = total > s.done_at_start ? total - s.done_at_start : 0;
        const u32 left = total > done ? total - done : 0;
        const u32 finished = done > s.done_at_start ? done - s.done_at_start : 0;
        const double measured = finished >= 2 && elapsed >= 30'000'000 ? finished * 60'000'000. / static_cast<double>(elapsed) : 0.;
        s.minutes_left = measured > 0 ? utils::ps4::compile_minutes_left(left, measured) : 0;
        s.eta = measured > 0 ? fmt::format("; PPU compile %.1f modules/min, ~%llu min left", measured, s.minutes_left) : std::string();
        if (s.toast_shown || !utils::ps4::first_boot_toast_due(modules, left, elapsed, measured > 0)) return;
        s.toast_shown = true;
        const u64 minutes = utils::ps4::compile_minutes_left(left, measured > 0 ? measured : 4.);
        ps4_notify(i18n::trf(i18n::str::MAIN_PRIMEIRA_ABERTURA_COMPILANDO_MODULOS_PODE, {modules, minutes}).c_str());
        ps4_startup_checkpoint(fmt::format("first boot: compiling %u PPU modules, %u left after %llu s, about %llu min (%s); toast shown",
            modules, left, elapsed / 1'000'000, minutes, measured > 0 ? fmt::format("measured %.1f modules/min", measured) : std::string("4 modules/min assumed")).c_str());
    }

    // STALL dumps written since the last game boot (0.34: was per process, so long sessions ran out after 15)
    u32 s_stall_dumps_total = 0;

    // 0.37: RSX semaphore timeouts counted before this boot, and whether its first one got the SPURS dump
    u64 s_sema_timeouts_at_boot = 0;
    bool s_timeout_spurs_dumped = false;

    // 0.39: the "stuck in the video" toast of this boot was shown
    bool s_video_stuck_told = false;

    // 0.39: the previous heartbeat line of this boot (process time, guest flips)
    // 0.40: and the session's draw samples then, so each line tells its own minute
    struct heartbeat_state
    {
        u64 at = 0;
        u64 flips = 0;
        u32 draw_seconds = 0;
        u32 measured_seconds = 0;
    };
    heartbeat_state s_heartbeat;

    // Stall activity beyond guest flips, progress counters and the save list (s_save_list_open): a native message dialog
    // on screen, kept from the last pass that could read the overlay list (its lock may be busy)
    bool s_native_dialog = false;

    // 0.40: any native dialog of the emulator on screen that waits for the player (message dialog, on-screen keyboard,
    // user list, media list, home menu), kept the same way. Not activity of the toast rule (that keeps the 0.39
    // message-dialog rule); it only keeps the per-game safe mode from taking a wait for the player for a failure.
    bool s_native_ui = false;

    // Main thread (g_fxo is reset on this thread, so the manager cannot go away during the call)
    void poll_native_ui()
    {
        const auto manager = g_fxo->try_get<rsx::overlays::display_manager>();
        if (!manager)
        {
            s_native_dialog = s_native_ui = false;
            return;
        }
        const int dialog = manager->ps4_try_has<rsx::overlays::message_dialog>();
        if (dialog >= 0) s_native_dialog = dialog > 0;
        int any = dialog;
        for (const int has : {manager->ps4_try_has<rsx::overlays::osk_dialog>(), manager->ps4_try_has<rsx::overlays::user_list_dialog>(),
                 manager->ps4_try_has<rsx::overlays::media_list_dialog>(), manager->ps4_try_has<rsx::overlays::home_menu_dialog>()})
        {
            if (any < 0) break;
            any = has < 0 ? has : any | has;
        }
        if (any >= 0) s_native_ui = any > 0;
    }

    // Guest sys_fs_read + sys_fs_write totals of the last passes (one per second, in a ring), for the file I/O rule
    struct fs_io_ring
    {
        static constexpr u32 size = 5;
        u64 calls[size]{};
        u32 next = 0;   // slot of the next sample; once full, the slot of the oldest one
        u32 filled = 0; // samples stored
        u64 last_at = 0; // process time of the newest sample
    };
    fs_io_ring s_io_ring;

    // The guest flip counter and since when it stays the same, for the one line per boot that names the activity rule
    // which kept the stall clock running
    struct flip_watch
    {
        u64 flips = 0, since = 0;
        bool told = false;
    };
    flip_watch s_flip_watch;

    // 0.40: the 0.38 stall clock beside the activity rule (its dumps are written even when the rule keeps the main
    // clock running) and the no-progress clock of the per-game safe mode; the reason that kept the main clock running
    // last, for the dump's headline; the suppressed dumps of this boot (at most two).
    utils::ps4::quiet_clock s_quiet;
    utils::ps4::quiet_clock s_no_progress;
    std::string s_quiet_reason;
    u32 s_suppressed_dumps = 0;
    bool s_no_progress_noted = false;

    // 0.40: guest flips that draw nothing (MK Komplete 0.39: 110 s of empty frames after its movie demuxer was refused)
    utils::ps4::empty_frames_watch s_empty_frames;
    u64 s_render_passes_seen = 0; // rsx::g_ps4_rsx_phase.render_passes at the previous 1 s sample

    // 0.40: one game session (from a successful boot to the next return to the XMB), for the session line, the
    // heartbeat, the session record and the clean-session note of the per-game safe mode. Main thread only.
    struct session_state
    {
        std::string title;          // TITLE_ID, empty while no game runs
        std::string name;           // PARAM.SFO title, for the session record
        u64 booted_at = 0;          // process time of the boot
        u64 running_us = 0;         // time the game ran in the foreground
        u64 last_sample_at = 0;     // previous 1 s sample while running
        u64 flips = 0;              // guest flips at the last sample
        u32 draw_seconds = 0;       // 1 s samples with a draw
        u32 measured_seconds = 0;   // 1 s samples on a renderer that tells draws
        u32 peak_threads = 0;       // highest console thread count (live + exited not joined)
        u32 loops = 0;              // HLE loops reported
        u8 level = 0;               // safe-mode level this boot runs
        bool refused_before = false; // a thread create had been refused in this process before this boot
        bool ok_noted = false;      // the clean-session note of this boot was written
        bool empty_dumped = false;  // the empty-frames dump of this boot was written
        bool empty_told = false;    // the empty-frames verdict of this boot was logged
        bool compile_noted = false; // the record said "compile" once at the start of a progress text
    };
    session_state s_session;

    // The title and safe-mode level of the last game booted in this process (kept after it ended, for the notice of the
    // next refusal: the safe mode may have raised that game's level)
    std::string s_last_boot_title;
    u8 s_last_boot_level = 0;

    // 0.40: per-thread state of the HLE loop detector (by PPU thread id; cleared at every boot)
    struct hle_loop_state
    {
        u64 last_index = 0;   // the thread's call counter at the previous pass
        u64 window[6]{};      // calls per 10 s pass, last minute
        u32 next = 0, filled = 0;
        u64 told_at = 0;      // first LOOP line of the thread (0 = none)
        bool seen = false;    // the counter was read once
        bool told_again = false;
    };
    std::unordered_map<u32, hle_loop_state> s_hle_loops;
    u64 s_hle_next = 0;

    // The 0.38 activity rule: only guest flips, progress counters and the save list are stall activity (native dialogs
    // and guest file I/O are not). 0.40: the feature row stall_activity, read on every pass (off with stall-strict.flag
    // or at safe-mode level 1); one line per boot.
    bool s_strict_told = false;
    bool stall_strict()
    {
        const bool strict = !safe_mode::enabled(safe_mode::feature::stall_activity);
        if (strict && !std::exchange(s_strict_told, true))
        {
            ps4_startup_checkpoint(::access("/data/rpcs3/stall-strict.flag", F_OK) == 0
                ? "STALL: 0.38 activity rule (stall-strict.flag): native dialogs and guest file I/O are not activity"
                : "STALL: 0.38 activity rule (safe mode): native dialogs and guest file I/O are not activity");
        }
        return strict;
    }

    // A new game boot (or a self-relaunch) re-arms the stall clock, the dump budget and the first-boot compile toast
    void rearm_stall_watchdog()
    {
        s_stall.reset_boot();
        s_stall_dumps_total = 0;
        s_first_compile = {};
        s_sema_timeouts_at_boot = rsx::g_ps4_rsx_counters.sema_timeouts.load();
        s_timeout_spurs_dumped = false;
        s_video_stuck_told = false;
        s_heartbeat = {};
        s_native_dialog = false;
        s_native_ui = false;
        s_io_ring = {};
        s_flip_watch = {};
        s_quiet.reset_boot();
        s_no_progress.reset_boot();
        s_quiet_reason.clear();
        s_suppressed_dumps = 0;
        s_no_progress_noted = false;
        s_empty_frames.reset_boot();
        s_render_passes_seen = 0;
        s_hle_loops.clear();
        s_hle_next = 0;
        s_strict_told = false;
    }

    // 0.39: age of the RSX loop heartbeat in ms, read from rsx::ps4_rsx_wait_state() ("heartbeat N ms ago"); empty
    // before the RSX loop ran. Atomics only.
    std::optional<u64> rsx_heartbeat_age_ms()
    {
        const std::string state = rsx::ps4_rsx_wait_state();
        constexpr std::string_view key = "heartbeat ";
        const usz at = state.find(key);
        if (at == std::string::npos) return std::nullopt;
        u64 ms = 0;
        usz i = at + key.size();
        const usz first = i;
        for (; i < state.size() && state[i] >= '0' && state[i] <= '9' && ms < 1'000'000'000'000ull; i++) ms = ms * 10 + static_cast<u64>(state[i] - '0');
        if (i == first || state.compare(i, 7, " ms ago") != 0) return std::nullopt;
        return ms;
    }

    // 0.40: whether the renderer drew since the previous 1 s sample. Vulkan counts the render passes its draws open
    // (rsx::g_ps4_rsx_phase.render_passes, set back to 0 by the RSX thread's 10 s line), so a sample sees the draws since
    // the previous one or since that reset. Empty on other renderers, without a renderer, and with the phase counters
    // off (rsx-profiler-off.flag). Main thread, once per second.
    std::optional<bool> drew_since_last_sample()
    {
        if (g_cfg.video.renderer.get() != video_renderer::vulkan || !rsx::get_current_renderer() || !rsx::g_ps4_rsx_phase.on.observe())
        {
            s_render_passes_seen = 0;
            return std::nullopt;
        }
        const u64 passes = rsx::g_ps4_rsx_phase.render_passes.observe();
        const bool drew = passes && passes != s_render_passes_seen;
        s_render_passes_seen = passes;
        return drew;
    }

    // 0.40: the thread classes of the census line (Utilities/Thread.h), without its "PS4 threads: " prefix
    std::string census_classes()
    {
        char line[768];
        std::string_view text(line, ps4_thread_census_line(line, sizeof(line)));
        if (constexpr std::string_view prefix = "PS4 threads: "; text.starts_with(prefix)) text.remove_prefix(prefix.size());
        return std::string(text);
    }

    // 0.40: "<what> <s> <draw s> <name>" as the session record of the running game (util/ps4_lifecycle.hpp)
    void write_session_record(const char* what, u64 now)
    {
        if (s_session.title.empty()) return;
        char line[256];
        const u64 seconds = now > s_session.booted_at ? (now - s_session.booted_at) / 1'000'000 : 0;
        if (utils::ps4::session_record::format(line, sizeof(line), what, seconds, s_session.draw_seconds, s_session.name))
            utils::ps4::adapt::write_record(s_session.title, utils::ps4::session_record::suffix, line);
    }

    // 0.40: what the progress text says the emulator does, for the heartbeat; the overlay texts follow the interface
    // language, so they are compared with the localized ones. Without a progress text, a message dialog on screen while
    // a renderer exists and its RSX loop has not run yet is the shader cache preload (the guest draws nothing before
    // that loop runs). Main thread.
    utils::ps4::progress_stage boot_progress_stage(bool rsx_loop_ran)
    {
        using utils::ps4::progress_stage;
        if (!g_progr_text)
        {
            if (rsx_loop_ran || !rsx::get_current_renderer()) return progress_stage::none;
            const auto manager = g_fxo->try_get<rsx::overlays::display_manager>();
            return manager && manager->ps4_try_has<rsx::overlays::message_dialog>() > 0 ? progress_stage::shader_cache : progress_stage::none;
        }
        const std::string text = g_progr_text;
        const auto is = [&text](localized_string_id id) { return text == get_localized_string(id); };
        if (is(localized_string_id::PROGRESS_DIALOG_COMPILING_PPU_MODULES)) return progress_stage::ppu_compile;
        if (is(localized_string_id::PROGRESS_DIALOG_BUILDING_SPU_CACHE)) return progress_stage::spu_cache;
        for (const localized_string_id id : {localized_string_id::PROGRESS_DIALOG_SCANNING_PPU_EXECUTABLE, localized_string_id::PROGRESS_DIALOG_ANALYZING_PPU_EXECUTABLE,
                 localized_string_id::PROGRESS_DIALOG_SCANNING_PPU_MODULES, localized_string_id::PROGRESS_DIALOG_LOADING_PPU_MODULES,
                 localized_string_id::PROGRESS_DIALOG_LINKING_PPU_MODULES, localized_string_id::PROGRESS_DIALOG_APPLYING_PPU_CODE})
        {
            if (is(id)) return progress_stage::ppu_prepare;
        }
        return progress_stage::other;
    }

    // 0.39: one RPCS3.log line per minute while a game exists (testers often send RPCS3.log alone): a session that ends
    // without a stop or fatal line still shows whether the game was drawing, frozen or paused when it ended. The 'PS4'
    // channel is kept by the log history when a long RPCS3.log is cut.
    // 0.40: plus the thread classes and the safe-mode level, and the "alive" session record; the stage of a boot
    // progress text instead of "starting or stopping" (a first boot compiles for up to an hour, and testers closed such
    // boots), and the seconds with a draw since the previous line (flips without draws = a black screen).
    void log_heartbeat(u64 now)
    {
        u64 flips = 0;
        if (const auto renderer = rsx::get_current_renderer()) flips = __atomic_load_n(&renderer->int_flip_index, __ATOMIC_RELAXED);
        const u64 elapsed = s_heartbeat.at && now > s_heartbeat.at ? now - s_heartbeat.at : 0;
        const std::string fps = elapsed && flips >= s_heartbeat.flips ? fmt::format("%.1f FPS over %llu s", (flips - s_heartbeat.flips) * 1'000'000. / elapsed, elapsed / 1'000'000)
                                                                       : std::string("first sample");
        // A new session restarts its sample counts: then the line tells the samples of that session
        const bool samples_restarted = s_session.measured_seconds < s_heartbeat.measured_seconds;
        const u32 draw_seconds = samples_restarted ? s_session.draw_seconds : s_session.draw_seconds - std::min(s_heartbeat.draw_seconds, s_session.draw_seconds);
        const u32 measured_seconds = samples_restarted ? s_session.measured_seconds : s_session.measured_seconds - s_heartbeat.measured_seconds;
        s_heartbeat = {now, flips, s_session.draw_seconds, s_session.measured_seconds};
        std::string state = "starting or stopping";
        bool known_state = true;
        switch (Emu.GetStatus(false))
        {
        case system_state::running: state = ps4_in_background() ? "running, in the background" : "running"; break;
        case system_state::paused: state = "paused"; break;
        case system_state::frozen: state = "frozen"; break;
        default: known_state = false; break;
        }
        const std::optional<u64> age = rsx_heartbeat_age_ms();
        const utils::ps4::progress_stage stage = boot_progress_stage(age.has_value());
        const std::string stage_text = utils::ps4::progress_stage_text(stage, g_progr_pdone.load(), g_progr_ptotal.load(),
            stage == utils::ps4::progress_stage::ppu_compile ? s_first_compile.minutes_left : 0);
        if (!stage_text.empty()) state = known_state ? state + ", " + stage_text : stage_text;
        // 0.40: the movie file the game holds open (played by its own decoder or through VDEC), so a black or frozen
        // minute inside a cutscene shows as one. The name comes from the game's files: control characters are masked.
        std::string movie;
        {
            char name[96] = {};
            unsigned seconds = 0;
            if (ps4_fs_movie_status(name, sizeof(name), &seconds))
            {
                name[sizeof(name) - 1] = 0;
                for (char* p = name; *p; ++p)
                    if (static_cast<unsigned char>(*p) < 0x20) *p = '?';
                movie = fmt::format("; movie '%s' open for %u s", name, seconds);
            }
        }
        const ps4_thread_load load = ps4_thread_load_now();
        const u64 rate = ps4_cell_audio_rate_milli();
        ps4_log.notice("heartbeat: %s %s; guest flips %llu (%s); %s%s; RSX loop %s; threads %u (live %u + exited not joined %u); audio rate %s; %s; safe level %u",
            Emu.GetTitleID(), state, flips, fps, utils::ps4::heartbeat_draws_text(draw_seconds, measured_seconds), movie,
            age ? fmt::format("%llu ms ago", *age) : std::string("not started"),
            load.total, load.live, load.unjoined, rate == umax ? std::string("not measured") : fmt::format("%.2f", rate / 1000.), census_classes(),
            safe_mode::g_level.load(std::memory_order_relaxed));
        // "compile" while a progress text is shown (a cache build): a session closed then gets no safe-mode hint
        write_session_record(g_progr_text ? "compile" : "alive", now);
    }

    // 0.37: the SPURS state at the first RSX semaphore timeout of a boot (the RSX thread wrote its label ring then), seen
    // by the 1 s watchdog pass: a SPURS title that waits for an RSX kick shows which workloads were ready and what every
    // kernel ran. Non-blocking like report_stall: when the IDM lock is busy the next pass tries again.
    void poll_rsx_timeout_dump(bool running)
    {
        if (s_timeout_spurs_dumped || !running || rsx::g_ps4_rsx_counters.sema_timeouts.load() <= s_sema_timeouts_at_boot) return;
        std::vector<u32> instances;
        std::vector<spurs_kernel> kernels;
        if (!id_manager::g_mutex.try_lock_shared()) return;
        idm::select<named_thread<spu_thread>>([&](u32 id, named_thread<spu_thread>& spu) { collect_spurs(spu, id, instances, kernels); }, idm::unlocked);
        id_manager::g_mutex.unlock_shared();
        s_timeout_spurs_dumped = true;
        std::vector<std::string> lines;
        append_spurs_lines(lines, "TIMEOUT", instances, kernels);
        ps4_startup_checkpoint(fmt::format("TIMEOUT: first RSX semaphore timeout of this boot; %u SPURS instance(s)", instances.size()).c_str());
        for (const auto& line : lines) ps4_startup_checkpoint(line.c_str());
    }

    // Guest file calls in 5 s that count as stall activity: the Fallout NV first-boot install did 196-289 per 5 s
    // without a guest flip; no PPU thread was in sys_fs_* at the GTA IV freezes
    constexpr u64 busy_fs_io_calls = 100;

    // Guest file calls over the last fs_io_ring::size passes (one per second), 0 while fewer samples are stored. A gap
    // between passes (a pause, a stop wait, the XMB) or a total below the oldest sample starts the ring again; umax
    // (unknown) leaves it as it is.
    u64 recent_fs_io_calls(u64 now)
    {
        const u64 io = lv2_obj::ps4_fs_io_calls();
        if (io == umax) return 0;
        fs_io_ring& r = s_io_ring;
        if (r.filled && (now - r.last_at > 2'000'000 || io < r.calls[r.filled < fs_io_ring::size ? 0 : r.next])) r = {};
        const u64 recent = r.filled == fs_io_ring::size ? io - r.calls[r.next] : 0;
        r.calls[r.next] = io;
        r.next = (r.next + 1) % fs_io_ring::size;
        r.filled = std::min(r.filled + 1, fs_io_ring::size);
        r.last_at = now;
        return recent;
    }

    // 0.40: a 1 s sample of the running game for the session line, the heartbeat and the clean-session note of the
    // per-game safe mode: >= 10 min in the foreground with guest flips and, where the renderer tells, draws, and no
    // failure in the session (safe_mode::note_ok checks that and writes only at a title level).
    void sample_session(u64 now, bool running, u64 flips, const std::optional<bool>& drew)
    {
        session_state& s = s_session;
        if (!running || !s.booted_at)
        {
            s.last_sample_at = 0;
            return;
        }
        if (s.last_sample_at && now > s.last_sample_at && !ps4_in_background()) s.running_us += std::min<u64>(now - s.last_sample_at, 2'000'000);
        s.last_sample_at = now;
        s.flips = flips;
        if (drew)
        {
            s.measured_seconds++;
            s.draw_seconds += *drew;
        }
        s.peak_threads = std::max(s.peak_threads, ps4_thread_load_now().total);
        if (s.ok_noted || s.running_us < 600'000'000 || flips < 600 || (s.measured_seconds && s.draw_seconds < 60)) return;
        s.ok_noted = true;
        safe_mode::note_ok();
        // note_ok writes only at a level of the title itself (not for the global safe mode alone) and only when this
        // session noted no failure
        if (s.level && safe_mode::detail::g_title_level.load(std::memory_order_relaxed) && !safe_mode::detail::g_session_kind.load(std::memory_order_acquire))
            ps4_startup_checkpoint(fmt::format("safe mode: %s ran 10 min in the foreground at level %u without a failure (safe_ok: the level stays for 8 more boots)",
                s.title, s.level).c_str());
    }

    // 0.40: a successful boot starts the session of its game (main thread, right after adapt::set_title)
    void begin_session()
    {
        s_session = {};
        s_session.title = Emu.GetTitleID();
        s_session.name = Emu.GetTitle();
        s_session.booted_at = sceKernelGetProcessTime();
        s_session.level = safe_mode::g_level.load(std::memory_order_relaxed);
        ps4_thread_load at{};
        s_session.refused_before = ps4_thread_first_refusal(at);
        s_last_boot_title = s_session.title;
        s_last_boot_level = s_session.level;
        write_session_record("boot", s_session.booted_at);
    }

    // 0.40: back to the XMB: one line that sums up the session (startup.log and RPCS3.log), and the end of its record.
    // Not on the way out of the application: a session closed from the home screen keeps a record without its end.
    void end_session(const char* why)
    {
        session_state& s = s_session;
        if (!s.booted_at) return;
        const u64 now = sceKernelGetProcessTime();
        ps4_thread_load at{};
        const bool refused = !s.refused_before && ps4_thread_first_refusal(at);
        const std::string line = fmt::format("session: %s ran %llu s (%llu s in the foreground), flips %llu (%s), peak threads %u/%u, thread create refused in "
            "this boot: %s, stall dumps %u (suppressed by the activity rule %u, empty frames %u), loops %u, safe level %u, features 0x%llx; %s",
            s.title.empty() ? std::string("(no title id)") : s.title, now > s.booted_at ? (now - s.booted_at) / 1'000'000 : 0, s.running_us / 1'000'000, s.flips,
            s.measured_seconds ? fmt::format("draws>0 for %u of %u sampled s", s.draw_seconds, s.measured_seconds) : std::string("draws not measured"),
            s.peak_threads, utils::ps4::governor::thread_cap(), refused ? fmt::format("yes (at %u threads)", at.total) : std::string("no"),
            s_stall_dumps_total, s_suppressed_dumps, s.empty_dumped ? 1u : 0u, s.loops, s.level, safe_mode::g_on.load(std::memory_order_relaxed), why);
        ps4_startup_checkpoint(line.c_str());
        ps4_log.notice("%s", line);
        if (!s.title.empty()) utils::ps4::adapt::write_record(s.title, utils::ps4::session_record::suffix, "end xmb");
        s = {};
    }

    void poll_stall_watchdog(u64 now)
    {
        static u64 next_check = 0;
        u32& total_dumps = s_stall_dumps_total;
        if (now < next_check) return;
        next_check = now + 1'000'000;
        // 0.40: a focus change, a resume from rest mode, or a gap in these passes (the main loop did not get here for
        // 10 s) restarts the safe mode's clocks below: time spent away says nothing about the game
        static u32 s_focus_seen = 0;
        static u64 s_last_pass = 0;
        const u32 focus = ps4_focus_epoch();
        const bool away = std::exchange(s_focus_seen, focus) != focus || (s_last_pass && now - s_last_pass > 10'000'000);
        s_last_pass = now;
        // 0.40: the class census Utilities/Thread.cpp asked for (a thread level change, a refused create), printed here and
        // never on the thread that creates or refuses
        if (ps4_census_due())
        {
            char census[1024];
            if (ps4_thread_census_line(census, sizeof(census))) ps4_startup_checkpoint(census);
        }
        const bool running = Emu.IsRunning();
        poll_rsx_timeout_dump(running);
        u64 flips = 0;
        if (running)
        {
            if (const auto rsx = rsx::get_current_renderer())
                flips = __atomic_load_n(&rsx->int_flip_index, __ATOMIC_RELAXED);
        }
        const u64 progress = (u64{g_progr_pdone.load()} << 32) ^ (u64{g_progr_ptotal.load()} << 48) ^
            (u64{g_progr_fdone.load()} << 8) ^ g_progr_ftotal.load();
        // 0.33: an open native save data list counts as activity: native UI frames are not guest flips, and the game
        // thread waiting in the list must not earn the 'game looks frozen' toast, fatal note and STALL dumps.
        // 0.40: this is the 0.38 activity, also the one of the quiet clocks below.
        const bool save_list = s_save_list_open.load(std::memory_order_relaxed) != 0;
        // Each signature is built from 'moved' once (a wait mixes 'now' in a single time, so two waits cannot cancel)
        const u64 moved = (flips * 0x9e3779b97f4a7c15ull) ^ progress;
        const u64 strict_activity = moved ^ (save_list ? now : 0);
        // The same for a native message dialog on screen (a game's install progress bar, a question not answered yet)
        // and for a game busy with its own file I/O, while the RSX loop runs: the RSX thread draws the dialog, so a stale
        // heartbeat means the picture is frozen too, and the dumps and the video toast stay.
        // 0.40: dialogs and file I/O are read in every pass while the game runs (the safe mode's no-progress rule waits
        // for them too); only the toast rule depends on stall_activity.
        std::string reason;
        std::optional<u64> rsx_age;
        bool heavy_io = false;
        if (running)
        {
            poll_native_ui();
            const u64 io_calls = recent_fs_io_calls(now);
            // The safe mode's no-progress rule waits for file I/O whatever the RSX loop does (a heartbeat is not
            // reported by every renderer); the toast rule keeps its 0.39 condition
            heavy_io = io_calls >= busy_fs_io_calls;
            rsx_age = rsx_heartbeat_age_ms();
            if (!stall_strict() && rsx_age && *rsx_age < 20'000)
            {
                if (s_native_dialog) reason = "a native message dialog is on screen";
                else if (heavy_io) reason = fmt::format("the game reads or writes files (%llu calls in 5 s)", io_calls);
            }
        }
        else
        {
            s_io_ring = {};
        }
        const u64 activity = moved ^ (save_list || !reason.empty() ? now : 0);
        // One line per boot once such a reason kept the stall clock running for as long as the first dump takes
        if (flips != s_flip_watch.flips || !s_flip_watch.since) s_flip_watch = {flips, now, s_flip_watch.told};
        if (!save_list && !reason.empty() && !s_flip_watch.told && now - s_flip_watch.since >= utils::ps4::stall_watchdog::first_dump_us)
        {
            s_flip_watch.told = true;
            ps4_startup_checkpoint(fmt::format("STALL: no guest flip for %llu s while %s; RSX loop %llu ms ago: counted as activity, no 'game looks frozen' "
                "toast; STALL dumps at 60 s and 300 s (stall-strict.flag restores the 0.38 rule)", (now - s_flip_watch.since) / 1'000'000, reason, *rsx_age).c_str());
        }
        const bool compiling = poll_compile_stage();
        poll_first_boot_compile(now, compiling);
        // 0.40: the session record says so as soon as a progress text appears, not only at the next heartbeat
        if (compiling && s_session.booted_at && !std::exchange(s_session.compile_noted, true)) write_session_record("compile", now);
        const bool dump_due = s_stall.poll(running, now, activity);
        // 0.31: after 30 s without a guest flip or progress, one toast and one fatal note, so the player learns how to
        // close the game and the next boot shows why. Called on every pass: a pass without a long stall re-arms it.
        // Never while the progress text is shown, in the background or while paused.
        utils::ps4::health::on_stall(s_stall.stalled_us(now), running, ps4_in_background(), compiling);

        // 0.40: the same stall under the 0.38 rule. While the activity rule keeps the main clock running, its 60 s and
        // 300 s marks still get a dump, without a toast or a fatal note (the Minecraft 0.39 world loads left none).
        const unsigned quiet_mark = s_quiet.poll(running, now, strict_activity);
        if (!s_quiet.quiet_us(now)) s_quiet_reason.clear();
        if (!reason.empty()) s_quiet_reason = reason;
        if (quiet_mark && s_suppressed_dumps < 2 && !s_quiet_reason.empty() && s_stall.stalled_us(now) + 2'000'000 < s_quiet.quiet_us(now))
        {
            s_suppressed_dumps++;
            report_stall(fmt::format("no guest flip or progress for %llu s (guest flips=%llu) (suppressed by the activity rule: %s)",
                s_quiet.quiet_us(now) / 1'000'000, flips, s_quiet_reason));
        }

        // 0.40: the no-progress failure of the per-game safe mode: 5 min without a guest flip or a progress counter, and
        // nothing a legitimate long wait shows (any progress text, e.g. a PPU/SPU/pipeline cache build; a native dialog,
        // on-screen keyboard, user or media list, the home menu or the save list; heavy guest file I/O; pipelines still
        // queued or compiling; the home screen or a system overlay; a resume or a gap in these passes). A pause stops
        // the clock.
        const bool pipelines = rsx::g_ps4_rsx_counters.pipe_pending.load() != 0;
        const bool waiting = compiling || save_list || s_native_ui || s_native_dialog || heavy_io || pipelines || ps4_in_background() || away;
        if (s_no_progress.poll(running, now, moved ^ (waiting ? now : 0)) == 2 && !std::exchange(s_no_progress_noted, true))
        {
            safe_mode::note_failure(safe_mode::fail_kind::stall);
            ps4_startup_checkpoint(fmt::format("STALL: no guest flip or progress for %llu s, and no progress text, native dialog, file activity, pipeline compile "
                "or home screen to wait for: a failure of %s for the per-game safe mode (fail=stall; one failure per session)", s_no_progress.quiet_us(now) / 1'000'000,
                s_session.title.empty() ? std::string("the game") : s_session.title).c_str());
        }

        // 0.40: guest flips that draw nothing. One PPU dump at 60 s; at 120 s a failure when a thread create was refused in
        // this boot (Mortal Kombat Komplete 0.39). Not while something legitimate is shown or loads, nor while pipelines
        // still compile (their draws are skipped by design).
        const std::optional<bool> drew = drew_since_last_sample();
        const bool counted = running && drew && !waiting;
        switch (s_empty_frames.poll(counted, now, flips != s_session.flips, drew.value_or(false)))
        {
        case 1:
            if (!std::exchange(s_session.empty_dumped, true))
                report_stall(fmt::format("guest flips without a draw for %llu s (guest flips=%llu)", s_empty_frames.empty_us(now) / 1'000'000, flips));
            break;
        case 2:
            if (s_session.empty_told) break;
            s_session.empty_told = true;
            if (ps4_thread_load refused{}; !s_session.refused_before && ps4_thread_first_refusal(refused))
            {
                safe_mode::note_failure(safe_mode::fail_kind::stall);
                ps4_startup_checkpoint(fmt::format("STALL: guest flips without a draw for %llu s after a refused thread create (at %u threads): a failure of %s "
                    "for the per-game safe mode (fail=stall; one failure per session)", s_empty_frames.empty_us(now) / 1'000'000, refused.total,
                    s_session.title.empty() ? std::string("the game") : s_session.title).c_str());
            }
            else
            {
                ps4_startup_checkpoint(fmt::format("STALL: guest flips without a draw for %llu s; no thread create was refused in this boot: not counted as a failure",
                    s_empty_frames.empty_us(now) / 1'000'000).c_str());
            }
            break;
        default:
            break;
        }
        sample_session(now, running, flips, drew);

        if (!dump_due || total_dumps >= 15) return;
        total_dumps++;
        report_stall(fmt::format("no guest flip or progress for %llu s (guest flips=%llu, dump %u/%u)",
            s_stall.stalled_us(now) / 1'000'000, flips, s_stall.dumps(), utils::ps4::stall_watchdog::max_dumps));
        // 0.39: from the second dump of a stall on, an RSX loop that has not run for as long means the video side is
        // stuck (GTA IV CE 0.38: heartbeat 26 s old at every dump) and the picture never comes back: one toast per boot
        // says how to leave, instead of a silent frozen picture
        using watchdog = utils::ps4::stall_watchdog;
        if (s_video_stuck_told || s_stall.stalled_us(now) < watchdog::first_dump_us + watchdog::second_dump_us) return;
        if (const std::optional<u64> age = rsx_heartbeat_age_ms(); age && *age >= 20'000)
        {
            s_video_stuck_told = true;
            ps4_startup_checkpoint(fmt::format("STALL: the RSX loop heartbeat is %llu ms old after %llu s without a guest flip; toast asks to close the application",
                *age, s_stall.stalled_us(now) / 1'000'000).c_str());
            ps4_notify(i18n::tr(i18n::str::MAIN_JOGO_TRAVOU_VIDEO));
        }
    }

    // 0.40: a PPU thread stuck repeating a short list of HLE/LV2 calls while the game still flips (Mortal Kombat Komplete
    // 0.39: 'PoolThread' opened and closed an audio port about 80 times a second, 19351 TODO lines). Every 10 s, with the
    // IDM lock only tried (as report_stall), each thread's call counter and its last 32 calls (PPUThread.cpp
    // ps4_ppu_recent_calls). A thread with 1000 calls a minute or more whose recent calls are at most three names and an
    // open/close (create/destroy, init/end, ...) pair or one CELL error code over and over gets one LOOP line, and one more
    // if it still repeats 5 min later. A log line only: such a loop may be the game's own polling.
    void poll_hle_loops(u64 now)
    {
        if (now < s_hle_next) return;
        s_hle_next = now + 10'000'000;
        if (!Emu.IsRunning() || !id_manager::g_mutex.try_lock_shared()) return;
        std::vector<std::string> lines;
        u32 first_lines = 0;
        idm::select<named_thread<ppu_thread>>([&](u32 id, named_thread<ppu_thread>& ppu)
        {
            hle_loop_state& st = s_hle_loops[id];
            // Written by that thread on every call (aligned u64): a stale value only delays a verdict
            const u64 index = ppu.syscall_history.index;
            const u64 delta = st.seen && index >= st.last_index ? index - st.last_index : 0;
            const bool counted = st.seen;
            st.seen = true;
            st.last_index = index;
            if (!counted) return;
            st.window[st.next] = delta;
            st.next = (st.next + 1) % std::size(st.window);
            st.filled = std::min<u32>(st.filled + 1, static_cast<u32>(std::size(st.window)));
            u64 calls = 0;
            for (u32 i = 0; i < st.filled; i++) calls += st.window[i];
            const u64 seconds = u64{st.filled} * 10;
            if (calls * 60 < 1000 * seconds) return;
            if (st.told_at && (st.told_again || now - st.told_at < 300'000'000)) return;
            ps4_hle_call recent[32];
            const u32 n = ps4_ppu_recent_calls(ppu, recent, static_cast<u32>(std::size(recent)));
            const utils::ps4::call_pattern p = utils::ps4::recent_pattern(recent, n);
            if (!p.first || p.distinct > 3 || !(p.pair || p.same_error)) return;
            const bool again = st.told_at != 0;
            if (again) st.told_again = true;
            else
            {
                st.told_at = now;
                first_lines++;
            }
            lines.push_back(fmt::format("LOOP: PPU 0x%x '%s' repeats %s%s%s (%llu calls in %llu s, ret 0x%llx%s)%s", id, ppu.get_name(), p.first,
                p.second ? "/" : "", p.second ? p.second : "", calls, seconds, p.ret, p.same_error ? ", the same error every call" : "",
                again ? "; still repeating 5 min later" : ""));
        }, idm::unlocked);
        id_manager::g_mutex.unlock_shared();
        s_session.loops += first_lines;
        // File I/O only after the IDM lock is released
        for (const std::string& line : lines)
        {
            ps4_startup_checkpoint(line.c_str());
            ps4_log.warning("%s", line);
        }
    }

	void run_main_calls()
	{
		std::deque<main_call> pending;
		{
			std::lock_guard lock(s_main_calls_mutex);
			pending.swap(s_main_calls);
		}

		for (auto& call : pending)
		{
			call.function();
			if (call.wake_up)
			{
				*call.wake_up = true;
				call.wake_up->notify_one();
			}
		}
	}

    // 0.31: Emu.IsStopped() is already true while the 'Emulation Join Thread' still tears the game down (state
    // 'stopping'). The last part of that teardown (objects, guest memory, then the state 'stopped') runs as a call on
    // the main thread, so the XMB must not open before it: the XMB loop does not serve main-thread calls, and a
    // BootGame in that state fails with "Game is still running". This waits on the main thread, at most limit_us,
    // serving main-thread calls and focus events. A thread-resource exhaustion seen meanwhile means the join thread
    // itself was refused, so the stop can never finish: leave the application. A stop stuck for any other reason is
    // handled by the emulator's Stop Watchdog (10 s, then it leaves the application).
    enum class stop_wait
    {
        stopped,    // the state is 'stopped'
        relaunched, // the stop finished and the game booted another executable from it (exitspawn)
        timed_out,  // still stopping after limit_us
    };

    stop_wait wait_for_full_stop(u64 limit_us)
    {
        if (Emu.IsStopped(true)) return stop_wait::stopped;
        const auto emulation = Emu.GetEmulationIdentifier(); // changes when a stop completes
        const u64 started = sceKernelGetProcessTime();
        for (;;)
        {
            run_main_calls();
            if (Emu.IsStopped(true)) break;
            if (Emu.GetEmulationIdentifier() != emulation)
            {
                ps4_startup_checkpoint("lifecycle: the stop finished and the game started another executable (exitspawn)");
                return stop_wait::relaunched;
            }
            leave_if_threads_exhausted();
            ps4_poll_system_service();
            if (sceKernelGetProcessTime() - started >= limit_us) return stop_wait::timed_out;
            sceKernelUsleep(1000);
        }
        ps4_startup_checkpoint(fmt::format("lifecycle: emulator fully stopped after %llu ms", (sceKernelGetProcessTime() - started) / 1000).c_str());
        return stop_wait::stopped;
    }

    // 0.33: run by select_and_boot after the previous stop has fully finished, right before BootGame. Every emulation
    // thread is joined by then, so a known global lock still taken after 0.5 s belongs to a thread that died without
    // unwinding: the boot would block on it for good and its stop would time out (0.32 Wolverine, second boot).
    // After a thread fatal in this process that means no further boot here: one durable note names the lock(s), a
    // toast asks for a restart and the application is left (LoadExec("exit"), as on a stop timeout). Without a
    // thread fatal the lock(s) are only reported. Mount point locks are normally reset by Emulator::Kill already.
    bool global_locks_allow_boot()
    {
        std::vector<std::string> held = held_global_locks();
        // A live thread (the log writer, a service thread) may hold one of them for a moment
        for (const u64 started = sceKernelGetProcessTime(); !held.empty() && sceKernelGetProcessTime() - started < 500'000;)
        {
            sceKernelUsleep(10'000);
            run_main_calls();
            held = held_global_locks();
        }
        if (held.empty())
        {
            if (s_thread_fatal_seen)
                ps4_startup_checkpoint(fmt::format("lifecycle: global locks free after the stop that followed a thread fatal (dead-thread holds still recorded: %u)",
                    ps4_dead_thread_lock_count()).c_str());
            return true;
        }
        const std::string list = fmt::merge(held, "; ");
        if (!s_thread_fatal_seen)
        {
            ps4_startup_checkpoint(fmt::format("lifecycle: global lock(s) still taken 0.5 s after the stop, with no thread fatal in this process; booting anyway: %s", list).c_str());
            return true;
        }
        s_restart_recommended = true;
        ps4_fatal_note("lifecycle: ", fmt::format("global lock(s) left taken by a thread that died in a fatal error (dead-thread holds recorded: %u): %s; no further boot in this process",
            ps4_dead_thread_lock_count(), list).c_str());
        ps4_notify(i18n::tr(i18n::str::MAIN_RPCS3_EMULADOR_PRECISA_REINICIAR_APOS));
        logs::listener::sync_all();
        // Returns only when the shell did not close the application (or no-loadexec.flag): the XMB then refuses boots
        ps4_try_leave_application("global lock left by a thread that died in a fatal error");
        return false;
    }

    // 0.33: TITLE_ID of the game about to boot, for the cache trim, which never removes that title's cache folder. Same
    // PARAM.SFO lookup as the launcher's library: an ISO's PS3_GAME/PARAM.SFO, a disc folder's PS3_GAME/, a game
    // folder's own, or two levels above an EBOOT.BIN. Empty when unreadable (the trim then keeps the newest cache).
    psf::registry boot_param_sfo(const std::string& path)
    {
        psf::registry sfo;
        if (utils::ps4::file_kind(path) == utils::ps4::game_kind::iso)
        {
            iso_archive archive(path);
            if (archive.is_valid()) sfo = archive.open_psf("PS3_GAME/PARAM.SFO");
        }
        else
        {
            const std::string base = fs::is_dir(path) ? path + (fs::is_file(path + "/PS3_GAME/PARAM.SFO") ? "/PS3_GAME/" : "/")
                : fs::get_parent_dir(path) + "/../";
            sfo = psf::load_object(base + "PARAM.SFO");
        }
        return sfo;
    }

    std::string boot_title_id(const std::string& path)
    {
        return std::string(psf::get_string(boot_param_sfo(path), "TITLE_ID", ""));
    }

    // 0.39: the game's PARAM.SFO TITLE on one line (the same lookup), else its file or folder name, for the XMB texts
    std::string boot_title_name(const std::string& path)
    {
        std::string title(psf::get_string(boot_param_sfo(path), "TITLE", ""));
        std::replace_if(title.begin(), title.end(), [](char c) { return c == '\n' || c == '\r' || c == '\t'; }, ' ');
        while (!title.empty() && title.back() == ' ') title.pop_back();
        return title.empty() ? game_display_name(path) : ps4_fatal_detail::utf8_prefix(title, 70);
    }

    // 0.40: the safe-mode level the next boot of the game at 'path' runs when a failure of its last session in this
    // process raised it (its adapt file holds a level above the one that session ran), else 0; also 0 once level 3
    // failed again. For the notice of the refusal after a thread fatal: the game is worth a new start then.
    unsigned raised_safe_level(const std::string& path)
    {
        if (s_last_boot_title.empty() || boot_title_id(path) != s_last_boot_title) return 0;
        const std::string text = utils::ps4::adapt::read_text(s_last_boot_title);
        const utils::ps4::adapt::safe_state st = utils::ps4::adapt::safe_status(text);
        return st.level > s_last_boot_level && !st.exhausted ? st.level : 0;
    }

    // 0.40: session records left without their end after less than 10 min (util/ps4_lifecycle.hpp session_record): the
    // player closed the game from the home screen soon after it started, e.g. because the menus worked but the game
    // never got past them. Not a failure, so no level is written; one toast per title (at most two) points at the
    // per-game row, and the record is closed so the toast comes once. Main thread, before the first XMB.
    void suggest_safe_mode_after_short_sessions()
    {
        namespace adapt = utils::ps4::adapt;
        namespace sr = utils::ps4::session_record;
        u32 shown = 0;
        for (const auto& entry : fs::dir(std::string(adapt::adapt_dir) + "/"))
        {
            if (entry.is_directory || entry.name.size() <= sr::suffix.size() || !entry.name.ends_with(sr::suffix)) continue;
            const std::string title = entry.name.substr(0, entry.name.size() - sr::suffix.size());
            const std::string record = adapt::read_record(title, sr::suffix);
            const sr::state st = sr::parse(record);
            if (!sr::hint_due(st)) continue;
            adapt::write_record(title, sr::suffix, "end hint");
            // A failure of that session already raised the title's level: its next boot says so itself
            const std::string text = adapt::read_text(title);
            if (const adapt::safe_state safe = adapt::safe_status(text); safe.level && !safe.boots_since)
            {
                ps4_startup_checkpoint(fmt::format("session: %s ended after %llu s without returning to the XMB; a failure was noted (safe level %u): no hint",
                    title, st.seconds, safe.level).c_str());
                continue;
            }
            ps4_startup_checkpoint(fmt::format("session: %s ended after %llu s without returning to the XMB (closed from the home screen?); toast suggests "
                "the per-game safe mode%s", title, st.seconds, shown < 2 ? "" : " (not shown: two toasts already)").c_str());
            if (shown++ < 2) ps4_notify(i18n::trf(i18n::str::MAIN_DICA_MODO_SEGURO, {st.name.empty() ? std::string_view(title) : st.name}).c_str());
        }
    }

    // 0.34: the disc package install that ran inside the current BootGame (on_install_pkgs, main thread), for the
    // boot result shown afterwards: the installer's own line for install_failed, and for an install-only disc
    // (nothing_to_boot) the game it installed. Reset by select_and_boot before every BootGame.
    struct disc_install_state
    {
        bool ok = false;                   // every package installed
        std::string message;               // pt-BR line of the installer screen (failure or cancel)
        std::deque<std::string> bootables; // USRDIR/EBOOT.BIN of each installed package (empty entry: none)
    };
    disc_install_state s_disc_install;

    // 0.38: the disc of a running game installed from a PS3 disc. Two drive checks without it (about 3 s apart) close
    // the game the way L1+R1+Triangle does and the XMB says "O disco foi removido."; games without a lock record are
    // never watched. Main thread only.
    // 0.39: only the exact image of a lock record is watched (game_watch::start binds by path and size). A removal
    // (three checks without the disc over at least 10 s) pauses the game and a toast asks for the disc again (every
    // 30 s while it is away and the game is on screen); when a disc of the game is back the game resumes, unless the
    // home screen or a thermal pause holds it (it resumes when that ends). A game resumed by another path while the
    // disc is out is paused again after 10 s. L1+R1+Triangle and the PS4's Close application work as in any pause.
    utils::ps4::disc_lock::sys::game_watch s_disc_watch;
    bool s_disc_paused = false;     // the game is paused because its disc left the drive
    bool s_disc_resume_due = false; // the disc is back: resume as soon as no other pause holds the game
    u64 s_disc_toast_at = 0;
    u64 s_disc_running_since = 0;   // the game runs although its disc is out (resumed by another path)
    unsigned s_disc_repauses = 0;   // pauses again of this boot (logged up to 8)

    void start_disc_watch(const std::string& path)
    {
        s_disc_paused = false;
        s_disc_resume_due = false;
        s_disc_running_since = 0;
        s_disc_repauses = 0;
        if (utils::ps4::file_kind(path) != utils::ps4::game_kind::iso)
        {
            s_disc_watch.stop("another boot");
            return;
        }
        fs::stat_t stat{};
        s_disc_watch.start(path, Emu.GetTitleID(), fs::get_stat(path, stat) && !stat.is_directory ? stat.size : 0);
    }

    bool s_thermal_paused = false; // a critical temperature pause holds the game (poll_thermal_pause)

    // Main loop, every pass. The drive is read by the drive service's own worker; tick_event only reads its last
    // status (at most every 250 ms).
    // 0.40: the pause is the feature row disc_pause; at safe-mode level 1 a removal closes the game as in 0.38 (the same
    // graceful stop as L1+R1+Triangle) and 'menu_notice' says why.
    void poll_disc_watch(u64 now, std::string& menu_notice)
    {
        using utils::ps4::disc_lock::watch_event;
        if (s_disc_watch.due())
        {
            switch (s_disc_watch.tick_event(utils::ps4::governor::thread_pressure() < 2))
            {
            case watch_event::removed:
            {
                if (!safe_mode::enabled(safe_mode::feature::disc_pause))
                {
                    ps4_startup_checkpoint("disc lock: the game's disc left the drive; the game is closed (safe mode: the 0.38 rule, no pause)");
                    // The untranslated marker, as in 0.38: the XMB recognises it and shows the translated system dialog
                    if (menu_notice.empty()) menu_notice = utils::ps4::disc_lock::removed_notice;
                    s_lifecycle.request_menu();
                    break;
                }
                const bool running = Emu.IsRunning();
                if (running) Emu.Pause(false, false);
                s_disc_paused = true;
                s_disc_resume_due = false;
                s_disc_toast_at = now;
                ps4_startup_checkpoint(fmt::format("disc lock: the game's disc left the drive; game paused until a disc of it is back%s",
                    running ? "" : " (it was not running)").c_str());
                ps4_notify(i18n::tr(i18n::str::MAIN_DISCO_SAIU_JOGO_PAUSADO));
                break;
            }
            case watch_event::back:
                if (!s_disc_paused) break;
                s_disc_paused = false;
                s_disc_resume_due = true;
                ps4_startup_checkpoint("disc lock: a disc of the game is back in the drive; the game resumes");
                break;
            default:
                break;
            }
        }
        // Resumed by another path while the disc is out (the home menu closing, START): paused again once the game has
        // run 10 s that way, well after a graceful exit request (which keeps resuming the game for at most 6 s while it
        // waits for the game's answer) has ended or turned into a stop
        if (!s_disc_paused || !Emu.IsRunning())
        {
            s_disc_running_since = 0;
        }
        else if (!s_disc_running_since)
        {
            s_disc_running_since = now;
        }
        else if (now - s_disc_running_since >= 10'000'000)
        {
            s_disc_running_since = 0;
            Emu.Pause(false, false);
            if (++s_disc_repauses <= 8)
                ps4_startup_checkpoint("disc lock: the game ran 10 s without its disc (resumed by another path); paused again");
            if (now - s_disc_toast_at >= 10'000'000)
            {
                s_disc_toast_at = now;
                ps4_notify(i18n::tr(i18n::str::MAIN_JOGO_PAUSADO_COLOQUE_DISCO));
            }
        }
        if (s_disc_paused && Emu.IsPaused() && !ps4_in_background() && now - s_disc_toast_at >= 30'000'000)
        {
            s_disc_toast_at = now;
            ps4_notify(i18n::tr(i18n::str::MAIN_JOGO_PAUSADO_COLOQUE_DISCO));
        }
        if (s_disc_resume_due && !ps4_in_background() && !s_thermal_paused)
        {
            s_disc_resume_due = false;
            if (Emu.GetStatus(false) == system_state::paused)
            {
                Emu.Resume();
                ps4_startup_checkpoint("disc lock: game resumed (its disc is back)");
                ps4_notify(i18n::tr(i18n::str::MAIN_DISCO_VOLTA_JOGO_RETOMADO));
            }
        }
    }

    // Free space on /data for the install line: "N MiB", or "unknown" when statvfs is refused or looks wrong.
    std::string data_free_text()
    {
        struct ::statvfs vs{};
        if (::statvfs("/data/rpcs3/", &vs) != 0) return fmt::format("unknown (statvfs errno %d)", errno);
        const u64 unit = vs.f_frsize ? static_cast<u64>(vs.f_frsize) : static_cast<u64>(vs.f_bsize);
        const u64 blocks = static_cast<u64>(vs.f_blocks);
        const u64 avail = static_cast<u64>(vs.f_bavail);
        if (!unit || (unit & (unit - 1)) || !blocks || avail > blocks) return "unknown";
        return fmt::format("%llu MiB", (avail * unit) >> 20);
    }

    // The game folder ('.../dev_hdd0/game/<ID>') of the first installed package that has a USRDIR/EBOOT.BIN.
    std::string installed_game_folder(const std::deque<std::string>& bootables)
    {
        constexpr std::string_view tail = "/USRDIR/EBOOT.BIN";
        for (const std::string& eboot : bootables)
        {
            const std::string path = normalize_host_path(eboot);
            if (path.size() > tail.size() && path.ends_with(tail) && fs::is_file(path))
                return path.substr(0, path.size() - tail.size());
        }
        return {};
    }

    // 0.34: after decryption_error. The NPDRM header (NPD_HEADER: magic "NPD\0", license at +8, content id at +0x10)
    // sits in the plain SELF control info of EBOOT.BIN, or at the start of ISO.BIN.EDAT, readable without decryption.
    // Returns the content id when the license type needs a .rap (1 network, 2 local) and no exdata/<id>.rap exists;
    // else empty. Folders, dev_hdd0 games and loose executables only (the EBOOT is found as GetElfPathFromDir does).
    std::string missing_license(const std::string& path)
    {
        std::string elf;
        if (fs::is_dir(path))
        {
            for (const char* name : {"/EBOOT.BIN", "/USRDIR/EBOOT.BIN", "/USRDIR/ISO.BIN.EDAT", "/PS3_GAME/USRDIR/EBOOT.BIN"})
            {
                if (fs::is_file(path + name))
                {
                    elf = path + name;
                    break;
                }
            }
        }
        else if (utils::ps4::file_kind(path) != utils::ps4::game_kind::iso && fs::is_file(path))
        {
            elf = path;
        }
        if (elf.empty()) return {};
        std::vector<u8> head(4096);
        if (fs::file file{elf}; file) head.resize(file.read(head.data(), head.size()));
        else return {};
        const utils::ps4::npd_license npd = utils::ps4::find_npd_license(head.data(), head.size());
        const std::string& id = npd.content_id;
        // Only a valid content id (36 characters of A-Z 0-9 - _) names a .rap
        if (!utils::ps4::license_needs_rap(npd.license) || utils::ps4::rap_license_name(id + ".rap").empty()) return {};
        if (const std::string rap = rpcs3::utils::get_rap_file_path(id); !rap.empty() && fs::is_file(rap)) return {};
        ps4_startup_checkpoint(fmt::format("boot: '%s' needs the license %s.rap (NPD license type %u), not found in exdata", elf, id, npd.license).c_str());
        return id;
    }

    // 0.39: the ISO image the running game read past its end (Loader/ISO.h iso_ps4_take_incomplete_image: once per
    // noted image; the loader keeps at most 255 bytes of the path). Only the image of 'game': one noted by an earlier
    // boot that failed while loading is not this game's and is dropped with one line.
    bool take_incomplete_image(std::string& image, const std::string& game)
    {
        char buffer[256]{};
        if (!iso_ps4_take_incomplete_image(buffer, sizeof(buffer))) return false;
        image = buffer;
        if (image.empty()) return false;
        if (normalize_host_path(image) == normalize_host_path(game)) return true;
        ps4_startup_checkpoint(fmt::format("lifecycle: '%s' was read past its end outside the game that ran ('%s'): not named in its notice", image, game).c_str());
        image.clear();
        return false;
    }

    // A failed boot (or an incomplete ISO) as a system dialog of the XMB: the message, then the steps that fix it, as the
    // " · " parts of a text notice with the dialog flag (ps4_launcher.h). Every part is in the interface language; paths
    // and file names are arguments, so an Arabic line keeps them as one left-to-right run.
    std::string failure_dialog(std::string message, std::initializer_list<std::string> steps = {})
    {
        for (const std::string& step : steps) message.append(" · ").append(step);
        std::string notice = utils::ps4::xmb_notice::encode(utils::ps4::xmb_notice::kind::text, message);
        notice.insert(2, 1, ps4_notice_flag_dialog); // after the marker and the kind
        return notice;
    }

    // The steps after an ISO that is incomplete or unreadable
    std::string iso_size_step() { return i18n::tr(i18n::str::MAIN_ISO_COPIE_MESMO_TAMANHO); }
    std::string iso_fat32_step() { return i18n::trf(i18n::str::MAIN_ISO_FAT32_4_GB, {"4 GB", "/data/rpcs3/games"}); }

    // 0.39: notices for a game that read past the end of its ISO, after a thread fatal (the emulator-error dialog) and
    // after any other stop (a dialog with the steps)
    std::string incomplete_iso_fatal_notice()
    {
        return emulator_error_encoded(i18n::tr(i18n::str::MAIN_ISO_INCOMPLETA_REABRIR_NAO_RESOLVE), false);
    }

    std::string incomplete_iso_notice()
    {
        return failure_dialog(i18n::tr(i18n::str::MAIN_ISO_INCOMPLETA_ARQUIVO_TERMINA), {iso_size_step(), iso_fat32_step()});
    }

    // 0.34: the XMB line after a failed BootGame, naming what to fix, instead of the English result. 0.39: in the
    // interface language. 'select' gets the row the XMB opens on: the failed path, or the game an install-only disc just
    // installed. The firmware results are notices of their own kind, which the launcher turns into its firmware hints.
    // Every other result is a system dialog (failure_dialog): the message line of before, then the steps that fix it.
    std::string boot_failure_message(game_boot_result result, const std::string& path, std::string& select)
    {
        using i18n::str;
        select = path;
        switch (result)
        {
        case game_boot_result::firmware_missing:
            return utils::ps4::xmb_notice::encode(utils::ps4::xmb_notice::kind::firmware_missing);
        case game_boot_result::firmware_version:
            return utils::ps4::xmb_notice::encode(utils::ps4::xmb_notice::kind::firmware_old);
        case game_boot_result::nothing_to_boot:
            if (const std::string folder = s_disc_install.ok ? installed_game_folder(s_disc_install.bootables) : std::string(); !folder.empty())
            {
                const psf::registry sfo = psf::load_object(folder + "/PARAM.SFO");
                std::string title(psf::get_string(sfo, "TITLE", ""));
                std::replace_if(title.begin(), title.end(), [](char c) { return c == '\n' || c == '\r'; }, ' ');
                if (title.empty()) title = folder.substr(folder.find_last_of('/') + 1);
                select = folder;
                ps4_startup_checkpoint(fmt::format("boot: install disc finished (nothing to boot on it); '%s' preselected in the XMB", folder).c_str());
                return failure_dialog(i18n::trf(str::MAIN_DISCO_INSTALACAO_CONTEUDO_INSTALADO_ABRA, {title}));
            }
            return failure_dialog(i18n::tr(str::MAIN_NADA_INICIAR_FALTA_USRDIR_EBOOT));
        case game_boot_result::install_failed:
            return failure_dialog(s_disc_install.message.empty() ? std::string(i18n::tr(str::MAIN_PKG_DISCO_NAO_INSTALOU_VEJA)) : s_disc_install.message);
        case game_boot_result::decryption_error:
            if (const std::string id = missing_license(path); !id.empty())
            {
                return failure_dialog(i18n::trf(str::MAIN_LICENCA_RAP_PSN_PRECISA, {id}),
                    {i18n::trf(str::MAIN_LICENCA_RAP_COPIE_RAPS, {".rap", "/data/rpcs3/raps", "USB:/raps"}), i18n::trf(str::MAIN_LICENCA_RAP_IMPORTE_ESCOLHA, {".rap"})});
            }
            return failure_dialog(i18n::tr(str::MAIN_DESCRIPTOGRAFAR_FALHOU), {i18n::trf(str::MAIN_DESCRIPTOGRAFAR_RAP_DESTE_JOGO, {".rap"}),
                i18n::tr(str::MAIN_DESCRIPTOGRAFAR_ARQUIVO_INCOMPLETO), i18n::trf(str::MAIN_DETALHES_LOGS, {"/data/rpcs3/startup.log", "/data/rpcs3/log/RPCS3.log"})});
        case game_boot_result::invalid_file_or_folder:
            // 0.39: on an ISO this is a copy the boot could not read (a short executable header, a broken PARAM.SFO or
            // executable: System.cpp), or an image without a valid PS3_DISC.SFB, which upstream refuses too
            if (utils::ps4::file_kind(path) == utils::ps4::game_kind::iso)
            {
                if (ps4_iso_without_sfb(path))
                {
                    ps4_startup_checkpoint(fmt::format("boot: '%s' failed: ISO without a valid PS3_DISC.SFB (CATEGORY DG)", path).c_str());
                    return failure_dialog(i18n::tr(str::MAIN_ISO_SEM_PS3_DISC_SFB)); // its second part is the step
                }
                ps4_startup_checkpoint(fmt::format("boot: '%s' failed: invalid file or folder on an ISO; the XMB asks to copy the image again", path).c_str());
                return failure_dialog(i18n::tr(str::MAIN_ISO_INCOMPLETA_ILEGIVEL), {iso_size_step(), iso_fat32_step()});
            }
            return failure_dialog(i18n::tr(str::MAIN_FALHA_INICIAR_ARQUIVO_PASTA_INVALIDO), {i18n::tr(str::MAIN_PASTA_COPIE_INTEIRA)});
        case game_boot_result::invalid_bdvd_folder:
        case game_boot_result::wrong_disc_location:
            return failure_dialog(i18n::tr(str::MAIN_FALHA_INICIAR_PASTA_DISCO_INVALIDA), {i18n::trf(str::MAIN_PASTA_DISCO_ONDE, {"/data/rpcs3/games"})});
        case game_boot_result::file_creation_error:
            return failure_dialog(i18n::tr(str::MAIN_FALHA_INICIAR_NAO_FOI_POSSIVEL), {i18n::tr(str::MAIN_LIBERE_ESPACO_PS4)});
        case game_boot_result::unsupported_disc_type:
            return failure_dialog(i18n::tr(str::MAIN_FALHA_INICIAR_TIPO_DISCO_AINDA));
        case game_boot_result::disc_key_missing:
        case game_boot_result::disc_key_invalid:
        {
            // 0.37: the key of a PS3 disc comes by itself when the disc is in the drive with internet (disc key search).
            // 0.39: the launcher searches the key of an encrypted ISO before BootGame, so this is the final failure.
            std::string id = boot_title_id(path);
            if (id.empty()) id = game_display_name(path);
            const bool invalid = result == game_boot_result::disc_key_invalid;
            return failure_dialog(i18n::trf(invalid ? str::MAIN_ISO_CHAVE_OUTRO_DISCO : str::MAIN_ISO_CRIPTOGRAFADA_SEM_CHAVE, {id}),
                {invalid ? i18n::trf(str::MAIN_ISO_DKEY_ERRADA_APAGUE, {".dkey", "/data/rpcs3/data/redump"}) : i18n::trf(str::MAIN_ISO_DKEY_USB_REDUMP, {".dkey", "USB:/redump"})});
        }
        case game_boot_result::still_running:
            return failure_dialog(i18n::tr(str::MAIN_FALHA_INICIAR_JOGO_ANTERIOR_AINDA), {i18n::tr(str::MAIN_SE_REPETIR_FECHE_APLICATIVO)});
        case game_boot_result::savestate_corrupted:
        case game_boot_result::savestate_version_unsupported:
            return failure_dialog(i18n::tr(str::MAIN_FALHA_INICIAR_SAVESTATE_INVALIDO_OUTRA));
        default:
            return failure_dialog(i18n::trf(str::MAIN_FALHA_INICIAR_VEJA_RPCS3, {fmt::format("%s", result)}),
                {i18n::trf(str::MAIN_DETALHES_LOGS, {"/data/rpcs3/startup.log", "/data/rpcs3/log/RPCS3.log"})});
        }
    }

    // 0.31: console safety. While the temperature stays critical (health::pause_requested(): read-only sensor queries,
    // sampled once per second; nothing here or there touches fan, clock or power settings), a running game is paused.
    // It is never resumed automatically: once the console is back below health::thermal_clear_c, the player resumes it
    // the usual way, by pressing PS and going back to the game. Main thread only.
    // s_thermal_paused (declared with the disc watch): this pause holds the game
    bool s_thermal_cool_told = false; // the "cooled down" toast was shown for this pause
    u64 s_thermal_toast_at = 0;
    u64 s_thermal_paused_at = 0;      // last time this code paused the game
    u64 s_thermal_note_at = 0;        // last "paused again" note

    void thermal_toast(const char* text, bool always)
    {
        const u64 now = sceKernelGetProcessTime();
        if (!always && s_thermal_toast_at && now - s_thermal_toast_at < 30'000'000) return;
        s_thermal_toast_at = now;
        ps4_notify(text);
    }

    // too_hot: health::pause_requested() of this pass; returned: the app came back from the home screen on this pass;
    // stopping: a stop or exit was requested (the stop resumes the game briefly to deliver the exit request, which
    // must not be paused again).
    void poll_thermal_pause(bool too_hot, bool returned, bool stopping)
    {
        if (too_hot && Emu.IsRunning() && !stopping)
        {
            // First pause of this episode, or the game was resumed by another path while the console is still
            // critical: START held while paused, or a stop that does not go through s_lifecycle (the home menu's
            // 'Exit game' resumes the game to deliver its exit request; a pause then ends it with a forced kill after
            // 1 s). Pause again at most every 2 s, so something that keeps resuming the game cannot turn this into a
            // pause/resume loop, and write the durable note at most every 30 s.
            const u64 now = sceKernelGetProcessTime();
            const bool first = !s_thermal_paused;
            if (!first && now - s_thermal_paused_at < 2'000'000) return;
            s_thermal_paused = true;
            s_thermal_cool_told = false;
            s_thermal_paused_at = now;
            if (first)
            {
                ps4_fatal_note("THERMAL: ", "console critically hot; emulation paused until the player resumes it after cooling down");
            }
            else if (!s_thermal_note_at || now - s_thermal_note_at >= 30'000'000)
            {
                s_thermal_note_at = now;
                ps4_fatal_note("THERMAL: ", "game resumed while still critically hot; paused again");
            }
            Emu.Pause(false, false);
            thermal_toast(i18n::tr(i18n::str::COM_CONSOLE_MUITO_QUENTE_JOGO_PAUSADO), first);
            return;
        }
        if (!s_thermal_paused) return;
        if (!Emu.IsPaused())
        {
            // Stopped (XMB, exit), or resumed by another path once it was cool enough.
            s_thermal_paused = false;
            return;
        }
        const bool cool = !too_hot && utils::ps4::health::thermal_state() == utils::ps4::health::thermal_ok;
        if (returned)
        {
            if (cool && s_disc_paused)
            {
                // 0.39: the game's disc is still out of the drive: that pause goes on (poll_disc_watch resumes it)
                s_thermal_paused = false;
                ps4_fatal_note("THERMAL: ", "console cooled down; the game stays paused until its disc is back in the drive");
            }
            else if (cool)
            {
                s_thermal_paused = false;
                ps4_fatal_note("THERMAL: ", "console cooled down and the player returned to the game; resuming emulation");
                Emu.Resume();
                thermal_toast(i18n::tr(i18n::str::MAIN_TEMPERATURA_NORMAL_JOGO_RETOMADO), true);
            }
            else
            {
                ps4_fatal_note("THERMAL: ", "user returned to the game but the console is still hot; emulation stays paused");
                thermal_toast(i18n::tr(i18n::str::MAIN_CONSOLE_AINDA_QUENTE_JOGO_CONTINUA), false);
            }
            return;
        }
        if (cool && !s_thermal_cool_told)
        {
            s_thermal_cool_told = true;
            ps4_fatal_note("THERMAL: ", "console back below the clear temperature; waiting for the player to resume");
            thermal_toast(i18n::tr(i18n::str::MAIN_CONSOLE_ESFRIOU_APERTE_PS_VOLTE), true);
        }
    }

	std::string boot_path(int argc, char** argv)
	{
		if (argc > 1 && argv && argv[1] && argv[1][0])
		{
			return argv[1];
		}

		FILE* file = std::fopen("/data/rpcs3/boot.txt", "rb");
		if (!file)
		{
			return {};
		}

		char path[4096]{};
		const bool read = std::fgets(path, sizeof(path), file) != nullptr;
		std::fclose(file);
		if (!read)
		{
			return {};
		}

		std::string result(path);
		while (!result.empty() && (result.back() == '\r' || result.back() == '\n' || result.back() == ' '))
		{
			result.pop_back();
		}
		return result;
	}

    // 0.33: image callbacks (cellPhotoDecode, cellSearch photo info). The PC frontend uses QImageReader; this uses
    // stb_image (already linked for the RSX overlays) and stb_image_resize2, on the calling guest thread.
    namespace image
    {
        // Decoded RGBA above this is refused (also Qt 6's default QImageReader allocation limit): the 2 GiB process
        // heap is shared with the PPU/SPU compilers.
        constexpr u64 max_rgba_bytes = 256ull << 20;

        // cellSearch codec name ("jpg", "png", ...) from the file signature; the extension only tells MPO from JPEG
        std::string format_name(const std::vector<u8>& head, std::string_view path)
        {
            const auto starts = [&head](std::initializer_list<u8> magic)
            {
                return head.size() >= magic.size() && std::equal(magic.begin(), magic.end(), head.begin());
            };
            if (starts({0xff, 0xd8, 0xff})) return fmt::to_lower(path).ends_with(".mpo") ? "mpo" : "jpg";
            if (starts({0x89, 'P', 'N', 'G'})) return "png";
            if (starts({'G', 'I', 'F', '8'})) return "gif";
            if (starts({'B', 'M'})) return "bmp";
            if (starts({'I', 'I', 0x2a, 0x00}) || starts({'M', 'M', 0x00, 0x2a})) return "tiff";
            return {};
        }

        // TIFF block of an EXIF segment: IFD0 tag 0x112 (orientation), mapped the way the PC frontend reports Qt's
        // QImageReader::transformation(): 1 -> 1 (0 degrees), 6 -> 2 (90), 3 -> 3 (180), 8 -> 4 (270), the mirrored
        // values 2/4/5/7 -> 0 (unknown). No tag or a malformed block counts as 1.
        s32 exif_orientation(const u8* tiff, usz size)
        {
            if (size < 8) return 1;
            const bool le = tiff[0] == 'I' && tiff[1] == 'I';
            if (!le && !(tiff[0] == 'M' && tiff[1] == 'M')) return 1;
            const auto rd16 = [&](usz at) -> u32
            {
                return le ? u32{tiff[at]} | u32{tiff[at + 1]} << 8 : u32{tiff[at]} << 8 | u32{tiff[at + 1]};
            };
            const auto rd32 = [&](usz at) -> u32 { return le ? rd16(at) | rd16(at + 2) << 16 : rd16(at) << 16 | rd16(at + 2); };
            if (rd16(2) != 42) return 1;
            const usz ifd = rd32(4);
            if (ifd > size || size - ifd < 2) return 1;
            const u32 count = rd16(ifd);
            for (u32 i = 0; i < count; i++)
            {
                const usz entry = ifd + 2 + usz{i} * 12;
                if (entry + 12 > size) return 1;
                if (rd16(entry) != 0x112) continue;
                if (rd16(entry + 2) != 3) return 1; // SHORT, stored in the first half of the value field
                switch (rd16(entry + 8))
                {
                case 6: return 2;
                case 3: return 3;
                case 8: return 4;
                case 2: case 4: case 5: case 7: return 0;
                default: return 1;
                }
            }
            return 1;
        }

        // JPEG markers up to the first scan; the APP1 'Exif' segment, if any
        s32 jpeg_orientation(const std::vector<u8>& data)
        {
            for (usz pos = 2; pos + 4 <= data.size();)
            {
                if (data[pos] != 0xff) return 1;
                const u8 marker = data[pos + 1];
                if (marker == 0xff)
                {
                    pos++; // fill byte
                    continue;
                }
                if (marker == 0xda || marker == 0xd9) return 1; // scan data or end of image: no EXIF before it
                const usz length = usz{data[pos + 2]} << 8 | data[pos + 3];
                if (length < 2 || pos + 2 + length > data.size()) return 1;
                if (marker == 0xe1 && length >= 2 + 6 + 8 && std::memcmp(&data[pos + 4], "Exif\0\0", 6) == 0)
                    return exif_orientation(&data[pos + 10], length - 8);
                pos += 2 + length;
            }
            return 1;
        }

        // The PC rule (main_application.cpp get_scaled_image): when the image is larger than the target box or
        // force_fit is set, scale it into the box keeping the aspect ratio; else keep its size. Never 0 or above the box.
        std::pair<s32, s32> fit_size(s32 width, s32 height, s32 target_width, s32 target_height, bool force_fit)
        {
            if (!force_fit && width <= target_width && height <= target_height) return {width, height};
            const f32 target_ratio = target_width / static_cast<f32>(target_height);
            const f32 image_ratio = width / static_cast<f32>(height);
            const f32 convert_ratio = image_ratio / target_ratio;
            s32 fit_width = target_width;
            s32 fit_height = target_height;
            if (convert_ratio > 1.0f) fit_height = static_cast<s32>(target_height / convert_ratio);
            else if (convert_ratio < 1.0f) fit_width = static_cast<s32>(target_width * convert_ratio);
            return {std::clamp(fit_width, 1, target_width), std::clamp(fit_height, 1, target_height)};
        }

        const char* stb_reason()
        {
            const char* reason = stbi_failure_reason();
            return reason ? reason : "unknown error";
        }

        bool info(const std::string& path, std::string& sub_type, s32& width, s32& height, s32& orientation)
        {
            sub_type.clear();
            width = 0;
            height = 0;
            orientation = 0; // CELL_SEARCH_ORIENTATION_UNKNOWN
            // The signature, and for a JPEG its EXIF APP1 segment (at most 64 KiB, before the frame data)
            std::vector<u8> head;
            if (fs::file file{path}; file)
            {
                head.resize(std::min<u64>(file.size(), 256 * 1024));
                head.resize(file.read(head.data(), head.size()));
            }
            int w = 0, h = 0, channels = 0;
            if (head.empty() || !stbi_info(path.c_str(), &w, &h, &channels) || w <= 0 || h <= 0)
            {
                ps4_log.error("get_image_info failed to read '%s' (%s)", path, head.empty() ? "cannot read the file" : stb_reason());
                return false;
            }
            sub_type = format_name(head, path);
            orientation = sub_type == "jpg" || sub_type == "mpo" ? jpeg_orientation(head) : 1;
            width = w;
            height = h;
            ps4_log.notice("get_image_info found image: filename='%s', sub_type='%s', width=%d, height=%d, orientation=%d", path, sub_type, width, height, orientation);
            return true;
        }

        // RGBA8888 rows of the result packed into dst (stride = result width * 4), at most target_width *
        // target_height * 4 bytes, as the PC frontend copies them
        bool scaled(const std::string& path, s32 target_width, s32 target_height, s32& width, s32& height, u8* dst, bool force_fit)
        {
            width = 0;
            height = 0;
            if (target_width <= 0 || target_height <= 0 || !dst || !fs::is_file(path)) return false;
            int w = 0, h = 0, channels = 0;
            if (!stbi_info(path.c_str(), &w, &h, &channels) || w <= 0 || h <= 0)
            {
                ps4_log.error("get_scaled_image failed to read '%s' (%s)", path, stb_reason());
                return false;
            }
            if (u64{static_cast<u32>(w)} * static_cast<u32>(h) * 4 > max_rgba_bytes)
            {
                ps4_log.error("get_scaled_image: '%s' is %dx%d, above the %u MiB decode limit", path, w, h, max_rgba_bytes >> 20);
                return false;
            }
            stbi_uc* const pixels = stbi_load(path.c_str(), &w, &h, &channels, STBI_rgb_alpha);
            if (!pixels)
            {
                ps4_log.error("get_scaled_image failed to decode '%s' (%s)", path, stb_reason());
                return false;
            }
            const auto [fit_width, fit_height] = fit_size(w, h, target_width, target_height, force_fit);
            bool ok = true;
            if (fit_width == w && fit_height == h)
            {
                const u64 room = u64{static_cast<u32>(target_width)} * static_cast<u32>(target_height) * 4;
                std::memcpy(dst, pixels, std::min<u64>(room, u64{static_cast<u32>(w)} * static_cast<u32>(h) * 4));
            }
            else
            {
                // fit_width <= target_width and fit_height <= target_height, so the packed result fits in dst
                ok = stbir_resize_uint8_linear(pixels, w, h, 0, dst, fit_width, fit_height, 0, STBIR_RGBA) != nullptr;
            }
            stbi_image_free(pixels);
            if (!ok)
            {
                ps4_log.error("get_scaled_image failed to scale '%s' from %dx%d to %dx%d", path, w, h, fit_width, fit_height);
                return false;
            }
            width = fit_width;
            height = fit_height;
            ps4_log.notice("get_scaled_image scaled image: path='%s', width=%d, height=%d", path, width, height);
            return true;
        }
    }

    // 0.33: cellPhotoExport target, built as main_application.cpp does: vfs /dev_hdd0/photo/YYYY/MM/DD/<title>
    // DD-MM-YYYY HH-MM-SS<ext>, with " N" added until the name is free (the export copy never overwrites). Before,
    // every export went to /data/rpcs3/photos/<title><ext>, outside the VFS, and the second one failed. Local time
    // from the console's time zone parameters: musl has no zone data, so localtime() would be UTC.
    std::string photo_path(std::string_view title)
    {
        std::string_view extension = ".png";
        if (const auto start = title.find_last_of('.'); start != umax)
        {
            extension = title.substr(start);
            title = title.substr(0, start);
        }
        std::time_t now = std::time(nullptr);
        s32 zone = 0, summer = 0;
        if (sceSystemServiceParamGetInt(4, &zone) == 0 && zone >= -12 * 60 && zone <= 14 * 60)
        {
            now += static_cast<std::time_t>(zone) * 60;
            if (sceSystemServiceParamGetInt(5, &summer) == 0 && summer) now += 3600;
        }
        std::tm t{};
        ::gmtime_r(&now, &t);
        const std::string path = vfs::get(fmt::format("/dev_hdd0/photo/%04d/%02d/%02d/%s %02d-%02d-%04d %02d-%02d-%02d",
            t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, vfs::escape(title, true),
            t.tm_mday, t.tm_mon + 1, t.tm_year + 1900, t.tm_hour, t.tm_min, t.tm_sec));
        std::string suffix(extension);
        for (u32 counter = 1; counter <= 9999 && !Emu.IsStopped() && fs::is_file(path + suffix); counter++)
            suffix = fmt::format(" %u%s", counter, extension);
        return path + suffix;
    }

    // 0.33: the save data list of the cellSaveData list and delete operations. Without it every list save, load or
    // delete returned CELL_CANCEL, so games that use the system list could never save or load. Same native overlay
    // and DRAWING_BEGIN/END bracket as the PC frontend (rpcs3qt/save_data_dialog.cpp); there is no Qt fallback here,
    // so no overlay manager means cancel (-2).
    class ps4_save_dialog final : public SaveDialogBase
    {
    public:
        s32 ShowSaveDataList(const std::string& base_dir, std::vector<SaveDataEntry>& save_entries, s32 focused, u32 op,
            vm::ptr<CellSaveDataListSet> listSet, bool enable_overlay) override
        {
            ps4_log.notice("ShowSaveDataList(save_entries=%d, focused=%d, op=0x%x, listSet=*0x%x, enable_overlay=%d)",
                save_entries.size(), focused, op, listSet, enable_overlay);
            const bool use_end = sysutil_send_system_cmd(CELL_SYSUTIL_DRAWING_BEGIN, 0) >= 0;
            if (!use_end) ps4_log.error("ShowSaveDataList(): Not able to notify DRAWING_BEGIN callback because one has already been sent!");
            s32 result = rsx::overlays::user_interface::selection_code::canceled;
            if (auto manager = g_fxo->try_get<rsx::overlays::display_manager>())
            {
                // Read by poll_stall_watchdog while this thread waits for the player
                struct open_scope
                {
                    open_scope() { s_save_list_open.fetch_add(1, std::memory_order_relaxed); }
                    ~open_scope() { s_save_list_open.fetch_sub(1, std::memory_order_relaxed); }
                } open_list;
                result = manager->create<rsx::overlays::save_dialog>()->show(base_dir, save_entries, focused, op, listSet, enable_overlay);
                if (result == rsx::overlays::user_interface::selection_code::error)
                {
                    ps4_log.error("ShowSaveDataList: native UI dialog returned an error; reported to the game as a cancel");
                    result = rsx::overlays::user_interface::selection_code::canceled;
                }
                else
                {
                    ps4_log.notice("ShowSaveDataList: native UI dialog returned with selection %d", result);
                }
            }
            else
            {
                ps4_log.error("ShowSaveDataList: no overlay manager (renderer without overlays); reported to the game as a cancel");
            }
            if (use_end) sysutil_send_system_cmd(CELL_SYSUTIL_DRAWING_END, 0);
            return result;
        }
    };

    // 0.33: trophy unlock popup through the native overlay, as trophy_notification_helper.cpp does on PC (sound stays
    // off: play_sound is a no-op here). Without an overlay manager (null renderer) a system toast names the trophy.
    class ps4_trophy_notification final : public TrophyNotificationBase
    {
    public:
        s32 ShowTrophyNotification(const SceNpTrophyDetails& trophy, const std::vector<uchar>& trophy_icon_buffer) override
        {
            if (auto manager = g_fxo->try_get<rsx::overlays::display_manager>())
            {
                // Several popups may be queued; the overlay schedules them
                auto popup = std::make_shared<rsx::overlays::trophy_notification>();
                return manager->add(popup, false)->show(trophy, trophy_icon_buffer);
            }
            const std::string name(trophy.name, ::strnlen(trophy.name, sizeof(trophy.name)));
            ps4_notify(i18n::trf(i18n::str::MAIN_RPCS3_TROFEU_DESBLOQUEADO, {name}).c_str());
            return 0;
        }
    };

    // 0.34: switch files on /data/rpcs3 change the emulator for every game and nothing on screen said so: fault
    // injectors left from a console test (test-*.flag/.txt, refuse-threads.flag) or keep-user-settings.flag, which
    // turns off every console limit. One startup.log line lists every *.flag and switch *.txt (not the fatal.txt
    // records); one toast names the test-*, refuse-* and keep-user-settings ones. Read only: nothing is renamed or removed.
    void report_switch_files()
    {
        std::vector<std::string> present, risky;
        for (const auto& entry : fs::dir("/data/rpcs3/"))
        {
            if (entry.is_directory || !utils::ps4::switch_file(entry.name)) continue;
            // 0.36: leftovers nothing in this build reads: spu-mode.txt is read only by builds without LLVM
            // (System.cpp fixup_settings), port_probe.txt is written by the standalone probe (ps4-port/probe)
#ifdef LLVM_AVAILABLE
            constexpr bool spu_mode_inert = true;
#else
            constexpr bool spu_mode_inert = false;
#endif
            const bool inert = entry.name == "port_probe.txt" || (spu_mode_inert && entry.name == "spu-mode.txt");
            present.push_back(inert ? entry.name + " (inert)" : entry.name);
            if (utils::ps4::test_switch_file(entry.name)) risky.push_back(entry.name);
        }
        std::sort(present.begin(), present.end());
        std::sort(risky.begin(), risky.end());
        ps4_startup_checkpoint(fmt::format("flags: %u switch file(s) in /data/rpcs3%s%s", present.size(), present.empty() ? "" : ": ",
            fmt::merge(present, ", ")).c_str());
        if (risky.empty()) return;
        std::string names;
        for (usz i = 0; i < risky.size() && i < 4; i++) fmt::append(names, "%s%s", i ? ", " : "", risky[i]);
        if (risky.size() > 4) names += i18n::trf(i18n::str::MAIN_MAIS, {risky.size() - 4});
        ps4_notify(i18n::trf(i18n::str::MAIN_RPCS3_ARQUIVOS_TESTE_ATIVOS_DATA, {names}).c_str());
        ps4_startup_checkpoint(fmt::format("flags: test/debug switches active, toast shown: %s", fmt::merge(risky, ", ")).c_str());
    }

    // 0.34: one 'environment:' line so a report from another console or firmware can be triaged: the firmware version,
    // the PS4 Pro (Neo) mode queries and the self-tests the app depends on (they park the app before this when they fail).
    std::string environment_line(bool fpu_rounding_ok)
    {
        std::string firmware = "unknown (sceKernelGetSystemSwVersion not exported)";
        if (&ps4_env_system_sw_version != nullptr)
        {
            OrbisKernelSwVersion version{};
            version.Size = sizeof(version);
            if (const s32 rc = ps4_env_system_sw_version(&version); rc == 0)
            {
                std::string text(version.VersionString, ::strnlen(version.VersionString, sizeof(version.VersionString)));
                text.erase(0, std::min(text.find_first_not_of(' '), text.size()));
                firmware = fmt::format("%s (0x%08x)", text, version.Version);
            }
            else
            {
                firmware = fmt::format("unknown (rc 0x%x)", static_cast<u32>(rc));
            }
        }
        const utils::ps4::cpu_probe_result& cpu = utils::ps4::cpu_probe();
#ifdef LLVM_AVAILABLE
        constexpr const char* jit = "JIT executable memory verified";
#else
        constexpr const char* jit = "no LLVM JIT in this build";
#endif
        return fmt::format("environment: firmware %s; Neo mode is=%d has=%d cpumode=%d (-1 = not exported); %s; SPU rounding self-test %s; build %s",
            firmware, cpu.is_neo, cpu.has_neo, cpu.cpumode, jit, fpu_rounding_ok ? "passed" : "FAILED", ps4_build_version);
    }

    // 0.33: one startup.log line per host fact the port relies on that was so far only read from the SDK headers:
    // fs::utime through sceKernelUtimes and libc's utimensat (orbis-compat shim), the statvfs layout, flock, the
    // condvar clock ids (orbis-compat probe), the raw time zone parameters and the host CPU as LLVM sees it. Once per
    // process: a few ms plus ~60 ms for the clock probe. It writes only /data/rpcs3/.abi-probe and removes it again.
    // Off with /data/rpcs3/no-abi-probe.flag.
    void run_boot_abi_probes()
    {
        if (fs::is_file("/data/rpcs3/no-abi-probe.flag"))
        {
            ps4_startup_checkpoint("ABI probe: skipped (no-abi-probe.flag)");
            return;
        }
        const std::string probe = "/data/rpcs3/.abi-probe";
        bool created = false;
        if (const int fd = ::open(probe.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0666); fd >= 0)
        {
            created = ::write(fd, "abi", 3) == 3;
            ::close(fd);
        }
        if (!created)
        {
            ps4_startup_checkpoint(fmt::format("ABI probe: cannot create %s (errno %d); utime and flock probes skipped", probe, errno).c_str());
        }
        else
        {
            constexpr s64 stamp_kernel = 1'000'000'000; // 2001-09-09
            constexpr s64 stamp_libc = 1'100'000'000;   // 2004-11-09
            fs::stat_t st{};
            const bool set = fs::utime(probe, stamp_kernel, stamp_kernel);
            const fs::error set_error = fs::g_tls_error;
            const s64 back_kernel = fs::get_stat(probe, st) ? st.mtime : -1;
            ps4_startup_checkpoint(fmt::format("ABI probe fs::utime (sceKernelUtimes): %s, mtime read back %lld -> %s",
                set ? std::string("ok") : fmt::format("failed (%s)", set_error), back_kernel,
                back_kernel == stamp_kernel ? "times set" : "times NOT set").c_str());

            const timespec times[2]{{stamp_libc, 0}, {stamp_libc, 0}};
            errno = 0;
            const int rc = ::utimensat(AT_FDCWD, probe.c_str(), times, 0);
            const int error = rc ? errno : 0;
            const s64 back_libc = fs::get_stat(probe, st) ? st.mtime : -1;
            ps4_startup_checkpoint(fmt::format("ABI probe utimensat (libc): rc %d errno %d, mtime read back %lld -> %s",
                rc, error, back_libc, back_libc == stamp_libc ? "times set" : "times NOT set").c_str());

            // fs::file lock opens (sys_fs simplefs, board storage, usio) depend on a refused second exclusive lock
            const int first = ::open(probe.c_str(), O_RDWR);
            const int second = ::open(probe.c_str(), O_RDWR);
            int rc_first = -1, rc_second = -1, error_first = 0, error_second = 0;
            if (first >= 0)
            {
                rc_first = ::flock(first, LOCK_EX | LOCK_NB);
                error_first = rc_first ? errno : 0;
            }
            if (second >= 0 && rc_first == 0)
            {
                rc_second = ::flock(second, LOCK_EX | LOCK_NB);
                error_second = rc_second ? errno : 0;
            }
            const char* verdict = first < 0 || second < 0 ? "probe file not opened twice"
                : rc_first != 0 ? "flock FAILS (fs::file lock opens fail)"
                : rc_second != 0 ? "enforced (second descriptor refused)" : "NOT enforced (granted twice)";
            if (rc_first == 0) ::flock(first, LOCK_UN);
            if (rc_second == 0) ::flock(second, LOCK_UN);
            if (first >= 0) ::close(first);
            if (second >= 0) ::close(second);
            ps4_startup_checkpoint(fmt::format("ABI probe flock(LOCK_EX|LOCK_NB): first rc %d errno %d, second descriptor rc %d errno %d -> %s",
                rc_first, error_first, rc_second, error_second, verdict).c_str());
            fs::remove_file(probe);
        }
        {
            struct ::statvfs vs{};
            errno = 0;
            const int rc = ::statvfs("/data/rpcs3/", &vs);
            const int error = rc ? errno : 0;
            const u64 frsize = static_cast<u64>(vs.f_frsize);
            const u64 blocks = static_cast<u64>(vs.f_blocks);
            const u64 bfree = static_cast<u64>(vs.f_bfree);
            const u64 bavail = static_cast<u64>(vs.f_bavail);
            const bool plausible = rc == 0 && frsize >= 512 && frsize <= (1u << 20) && (frsize & (frsize - 1)) == 0 &&
                blocks && bfree <= blocks && bavail <= blocks;
            ps4_startup_checkpoint(fmt::format("ABI probe statvfs('/data/rpcs3/'): rc %d errno %d bsize %llu frsize %llu blocks %llu bfree %llu bavail %llu files %llu ffree %llu namemax %llu -> %s",
                rc, error, static_cast<u64>(vs.f_bsize), frsize, blocks, bfree, bavail, static_cast<u64>(vs.f_files),
                static_cast<u64>(vs.f_ffree), static_cast<u64>(vs.f_namemax),
                plausible ? fmt::format("plausible, %llu MiB free of %llu MiB", (bavail * frsize) >> 20, (blocks * frsize) >> 20) : std::string("NOT plausible")).c_str());
        }
        // 0.36: statvfs is orbis-compat's open + libkernel _fstatfs on the directory, refused with EPERM on the
        // console. Other routes to the free space: _fstatfs on a regular file's descriptor, and getfsstat (every
        // mount; resolved at run time, the SDK stub does not export it). Kernel replies are read at the FreeBSD 9
        // struct statfs offsets (472 bytes: version 0x00, bsize 0x10, blocks 0x20, bavail 0x30, fstypename 0x118,
        // mntonname 0x180). Read only.
        {
            using fstatfs_fn = int (*)(int, void*);
            using getfsstat_fn = int (*)(void*, long, int);
            constexpr usz statfs9_size = 0x1d8;
            const auto describe = [](const u8* k) -> std::string
            {
                u32 version = 0;
                u64 bsize = 0, blocks = 0;
                s64 bavail = 0;
                char type[17]{}, on[89]{};
                std::memcpy(&version, k, 4);
                std::memcpy(&bsize, k + 0x10, 8);
                std::memcpy(&blocks, k + 0x20, 8);
                std::memcpy(&bavail, k + 0x30, 8);
                std::memcpy(type, k + 0x118, 16);
                std::memcpy(on, k + 0x180, 88);
                const bool plausible = (version == 0x20030518 || version == 0x20140518) && bsize >= 512 && bsize <= (32ull << 20) &&
                    (bsize & (bsize - 1)) == 0 && blocks && bavail <= static_cast<s64>(blocks);
                return fmt::format("'%s' (%s, version 0x%08x, bsize %llu, blocks %llu, bavail %lld%s)", on, type, version, bsize, blocks, bavail,
                    plausible ? fmt::format(": %llu MiB free", (static_cast<u64>(std::max<s64>(bavail, 0)) * bsize) >> 20) : std::string(", not plausible"));
            };
            std::string line = "ABI probe free space:";
            if (const auto kernel_fstatfs = reinterpret_cast<fstatfs_fn>(ps4_kernel_symbol("_fstatfs")))
            {
                for (const char* path : {"/data/rpcs3/", "/data/rpcs3/startup.log"})
                {
                    alignas(16) u8 raw[0x1000]{};
                    errno = 0;
                    const int fd = ::open(path, O_RDONLY);
                    const int open_errno = fd < 0 ? errno : 0;
                    const int rc = fd >= 0 ? kernel_fstatfs(fd, raw) : -1;
                    const int error = fd >= 0 && rc != 0 ? errno : 0;
                    if (fd >= 0) ::close(fd);
                    if (fd < 0) fmt::append(line, " _fstatfs(open '%s') open errno %d;", path, open_errno);
                    else if (rc != 0) fmt::append(line, " _fstatfs('%s') rc %d errno %d;", path, rc, error);
                    else fmt::append(line, " _fstatfs('%s') -> %s;", path, describe(raw));
                }
            }
            else
            {
                line += " _fstatfs not exported;";
            }
            const auto kernel_getfsstat = reinterpret_cast<getfsstat_fn>(ps4_kernel_symbol("getfsstat"));
            if (!kernel_getfsstat)
            {
                line += " getfsstat not exported";
            }
            else
            {
                errno = 0;
                const int count = kernel_getfsstat(nullptr, 0, 2); // MNT_NOWAIT
                const int count_errno = count < 0 ? errno : 0;
                if (count <= 0)
                {
                    fmt::append(line, " getfsstat(count) rc %d errno %d", count, count_errno);
                }
                else
                {
                    const usz entries = std::min<usz>(static_cast<usz>(count), 32);
                    std::vector<u8> buffer(entries * statfs9_size + 0x1000);
                    errno = 0;
                    const int got = kernel_getfsstat(buffer.data(), static_cast<long>(entries * statfs9_size), 2);
                    const int got_errno = got < 0 ? errno : 0;
                    fmt::append(line, " getfsstat: %d mount(s), %d returned (errno %d)", count, got, got_errno);
                    for (int i = 0; i < got && static_cast<usz>(i) < entries; i++)
                        fmt::append(line, "%s %s", i ? ";" : ":", describe(buffer.data() + i * statfs9_size));
                }
            }
            ps4_startup_checkpoint(line.c_str());
        }
        // Path resolution used by the PKG installer (weakly_canonical -> realpath) and by relative-path users (getcwd)
        {
            errno = 0;
            char* real = ::realpath("/data/rpcs3/./dev_hdd0/../", nullptr);
            const int real_errno = real ? 0 : errno;
            char cwd[512] = {};
            const char* cwd_rc = ::getcwd(cwd, sizeof(cwd));
            const int cwd_errno = cwd_rc ? 0 : errno;
            std::error_code ec;
            const std::string weak = std::filesystem::weakly_canonical("/data/rpcs3/dev_hdd0/game/__probe__/USRDIR", ec).string();
            const bool ok = real && std::string_view(real) == "/data/rpcs3" && weak == "/data/rpcs3/dev_hdd0/game/__probe__/USRDIR" && !ec;
            ps4_startup_checkpoint(fmt::format("ABI probe paths: realpath -> '%s' (errno %d), getcwd -> '%s' (errno %d), weakly_canonical -> '%s' (%s) -> %s",
                real ? real : "(null)", real_errno, cwd_rc ? cwd : "(null)", cwd_errno, weak, ec ? ec.message() : std::string("no error"),
                ok ? "PKG install paths OK" : "UNEXPECTED, PKG installs may fail").c_str());
            std::free(real);
        }
        // sceKernelClockGettime ids against the process counter and the condvar setclock read-back ('clock ids:')
        if (orbis::clockIdProbe)
        {
            orbis::clockIdProbe();
            // 0.36: the VERDICT line's "(id 1 is CPU time, as Mesa's condvars get today)" is outdated: Mesa's condition
            // variables wait through the orbis-drm futex shim, which measures their deadlines against CLOCK_REALTIME
            ps4_startup_checkpoint("ABI probe clock ids: note - Mesa's condition variables no longer wait on clock id 1: they go through the orbis-drm futex shim "
                "(deadlines against CLOCK_REALTIME; mesa.log 'futex shim' and 'futex timed self-test' lines)");
        }
        else
        {
            ps4_startup_checkpoint("ABI probe clock ids: this orbis-compat has no clockIdProbe; skipped");
        }
        {
            s32 zone = 0, summer = 0;
            const s32 rc_zone = sceSystemServiceParamGetInt(4, &zone);
            const s32 rc_summer = sceSystemServiceParamGetInt(5, &summer);
            ps4_startup_checkpoint(fmt::format("ABI probe time zone: TIME_ZONE=%d (rc 0x%x) SUMMERTIME=%d (rc 0x%x); minutes east of UTC if UTC-3 reads -180",
                zone, static_cast<u32>(rc_zone), summer, static_cast<u32>(rc_summer)).c_str());
        }
#ifdef LLVM_AVAILABLE
        {
            std::vector<std::string> present, absent;
            for (const auto& feature : llvm::sys::getHostCPUFeatures())
                (feature.getValue() ? present : absent).push_back(feature.getKey().str());
            std::sort(present.begin(), present.end());
            // The absent ones a PC CPU name or a PC build could assume
            std::string notable;
            for (const char* name : {"avx2", "fma", "bmi2", "fma4", "xop", "tbm", "lwp", "avx512f", "gfni", "vaes", "vpclmulqdq", "adx", "sha", "rdseed"})
                if (std::find(absent.begin(), absent.end(), name) != absent.end()) fmt::append(notable, "%s%s", notable.empty() ? "" : " ", name);
            ps4_startup_checkpoint(fmt::format("ABI probe CPU: LLVM host '%s', brand '%s', family 0x%x model 0x%x; %u features present: %s",
                llvm::sys::getHostCPUName().str(), utils::get_cpu_brand(), utils::get_cpu_family(), utils::get_cpu_model(),
                present.size(), fmt::merge(present, " ")).c_str());
            ps4_startup_checkpoint(fmt::format("ABI probe CPU: %u features absent, including: %s", absent.size(), notable.empty() ? "none of the listed ones" : notable).c_str());
        }
#endif
    }

    // 0.39: the interface language before any toast or XMB frame: /data/rpcs3/ui-language.txt ('auto', 'pt-BR', 'en',
    // 'es', 'ar'; absent or not understood = automatic), automatic follows the PS4 system language (parameter 1). The
    // same choice gives the automatic game language (util/ps4_i18n.hpp game_language_auto). 'pt-BR' keeps today's texts.
    void init_interface_language()
    {
        s32 system = -1;
        const s32 rc = sceSystemServiceParamGetInt(1, &system);
        std::string token = "auto";
        const char* origin = "absent";
        std::optional<i18n::setting> setting;
        if (fs::file file{i18n::setting_path})
        {
            if (file.size() > i18n::setting_max_bytes)
            {
                origin = "larger than 64 bytes, ignored";
            }
            else
            {
                token = file.to_string();
                setting = i18n::parse_setting(token);
                origin = setting ? "read" : "not understood, ignored";
                while (!token.empty() && (token.back() == '\n' || token.back() == '\r' || token.back() == ' ')) token.pop_back();
                // one startup.log line
                std::replace_if(token.begin(), token.end(), [](char c) { return static_cast<unsigned char>(c) < 0x20; }, ' ');
            }
        }
        i18n::apply(setting.value_or(i18n::setting::automatic), rc == 0 ? system : -1);
        // 0.40: the automatic game language is English (US) again (as 0.25-0.38); the PS4 language is the row value
        // "Idioma do PS4" of Configurações > Idioma dos jogos (or game_language_ps4 in features.txt). Each boot's policy
        // line names the language that boot uses.
        ps4_startup_checkpoint(fmt::format("i18n: ui-language.txt '%s' (%s), PS4 system language %d (rc 0x%x) -> %s; game language auto -> English (US) "
            "(default; Configurações > Idioma dos jogos chooses another; the PS4 language there: %s)",
            ps4_fatal_detail::utf8_prefix(token, 64), origin, rc == 0 ? system : -1, static_cast<u32>(rc), i18n::lang_token(i18n::current()),
            i18n::tr(i18n::game_language_name(i18n::cell_lang_from_ps4(i18n::system_language())), i18n::lang::en)).c_str());
    }

	void configure_callbacks()
	{
		g_emu_callbacks.call_from_main_thread = [](std::function<void()> function, atomic_t<u32>* wake_up)
		{
			if (thread_ctrl::is_main())
			{
				function();
				if (wake_up)
				{
					*wake_up = true;
					wake_up->notify_one();
				}
				return;
			}

			{
				std::lock_guard lock(s_main_calls_mutex);
				s_main_calls.push_back({std::move(function), wake_up});
			}

			// 0.29: wake the main loop at once (it may be sleeping up to 8 ms on this flag)
			s_main_wakeup.release(1);
			s_main_wakeup.notify_one();
		};

		g_emu_callbacks.try_to_quit = [](bool force_quit, std::function<void()> on_exit)
		{
			if (!force_quit)
			{
				return false;
			}
			if (on_exit)
			{
				on_exit();
			}
			s_lifecycle.request_exit();
			return true;
		};

		g_emu_callbacks.init_kb_handler = []
		{
			ensure(g_fxo->init<KeyboardHandlerBase, NullKeyboardHandler>(Emu.DeserialManager()));
		};
		g_emu_callbacks.init_mouse_handler = []
		{
			ensure(g_fxo->init<MouseHandlerBase, NullMouseHandler>(Emu.DeserialManager()));
		};
		g_emu_callbacks.init_pad_handler = [](std::string_view title_id)
		{
			ensure(g_fxo->init<named_thread<pad_thread>>(nullptr, nullptr, title_id));
			while (!pad::g_started)
			{
				// 0.31: a synchronous wait inside BootGame (main thread): if the pad thread died because thread
				// resources ran out, it never starts, so leave the application instead of waiting forever.
				if (thread_ctrl::is_main()) leave_if_threads_exhausted();
				sceKernelUsleep(1000);
			}
		};

		g_emu_callbacks.init_gs_render = [](utils::serial* archive)
		{
			// 0.37: the shader mode is the automatic profile's ('Async Recompiler with Shader Interpreter', the PC default:
			// pending pipelines draw with the interpreter instead of being skipped); it is no longer replaced here.
			switch (g_cfg.video.renderer.get())
			{
			case video_renderer::opengl:
				g_fxo->init<rsx::thread, named_thread<GLGSRender>>(archive);
				break;
#ifdef HAVE_VULKAN
			case video_renderer::vulkan:
				g_fxo->init<rsx::thread, named_thread<VKGSRender>>(archive);
				break;
#endif
			case video_renderer::null:
				g_fxo->init<rsx::thread, named_thread<NullGSRender>>(archive);
				break;
			default:
				report_fatal_error("The requested renderer is unavailable on PS4");
			}
		};
		g_emu_callbacks.get_gs_frame = []() -> std::unique_ptr<GSFrameBase>
		{
			return std::make_unique<ps4_frame>();
		};
		g_emu_callbacks.close_gs_frame = [] {};

		g_emu_callbacks.get_audio = []() -> std::shared_ptr<AudioBackend>
		{
			return std::make_shared<PS4AudioBackend>();
		};
		g_emu_callbacks.get_audio_enumerator = [](u64) -> std::shared_ptr<audio_device_enumerator>
		{
			return std::make_shared<PS4AudioEnumerator>();
		};
		g_emu_callbacks.get_camera_handler = []() -> std::shared_ptr<camera_handler_base>
		{
			return std::make_shared<null_camera_handler>();
		};
		g_emu_callbacks.get_music_handler = []() -> std::shared_ptr<music_handler_base>
		{
			return std::make_shared<null_music_handler>();
		};

		g_emu_callbacks.on_run = [](bool) {};
		g_emu_callbacks.on_pause = [] {};
		g_emu_callbacks.on_resume = [] {};
		g_emu_callbacks.on_stop = [] {};
		g_emu_callbacks.on_ready = [] {};
		g_emu_callbacks.on_missing_fw = [] { ps4_log.error("PS3 firmware is missing from /data/rpcs3"); };
		g_emu_callbacks.on_emulation_stop_no_response = [](std::shared_ptr<atomic_t<bool>> stopped, int seconds)
		{
			if (stopped && *stopped) return;
            // Runs on the Stop Watchdog thread. A guest thread is stuck (e.g. a JIT loop with
            // no syscalls); its memory and the GPU are still in use, so returning to the XMB
            // is unsafe. Record it and ask the shell to close us; never abort/_Exit (CE-34878-0).
            ps4_startup_checkpoint(fmt::format("shutdown: emulation threads did not stop within %d s", seconds).c_str());
            // 0.40: a stop that never finished is a failure of the game for the per-game safe mode (allocation-free notes).
            // Not while a PPU module compile holds the stop: a module compile cannot be interrupted, may outlast the
            // watchdog on the Jaguar, and no safe-mode level makes it shorter.
            if (!g_watchdog_hold_ctr)
                safe_mode::note_failure(safe_mode::fail_kind::stall);
            else
                ps4_startup_checkpoint("shutdown: a PPU module compile still holds the stop; not counted as a failure of the game");
            if (char title[utils::ps4::adapt::title_size]; utils::ps4::adapt::current_title(title))
                utils::ps4::adapt::write_record(title, utils::ps4::session_record::suffix, "end stall");
            report_guest_threads();
            if (!ps4_try_leave_application("emulation stop timed out"))
                ps4_notify(i18n::tr(i18n::str::MAIN_RPCS3_PS4_JOGO_NAO_RESPONDEU));
            // Returning lets the watchdog wait as upstream does; if the guest ever stops,
            // the main loop continues normally.
		};
		g_emu_callbacks.on_save_state_progress = [](std::shared_ptr<atomic_t<bool>>, stx::shared_ptr<utils::serial>, stx::atomic_ptr<std::string>*, std::shared_ptr<void>) {};
		g_emu_callbacks.enable_disc_eject = [](bool) {};
		g_emu_callbacks.enable_disc_insert = [](bool) {};
		g_emu_callbacks.handle_taskbar_progress = [](s32, s32) {};

		g_emu_callbacks.get_msg_dialog = []() -> std::shared_ptr<MsgDialogBase> { return {}; };
		g_emu_callbacks.get_osk_dialog = []() -> std::shared_ptr<OskDialogBase> { return {}; };
		g_emu_callbacks.get_save_dialog = []() -> std::unique_ptr<SaveDialogBase> { return std::make_unique<ps4_save_dialog>(); };
		g_emu_callbacks.get_trophy_notification_dialog = []() -> std::unique_ptr<TrophyNotificationBase> { return std::make_unique<ps4_trophy_notification>(); };
		g_emu_callbacks.get_localized_string = ps4_localized_string;
		g_emu_callbacks.get_localized_u32string = ps4_localized_u32string;
		g_emu_callbacks.get_localized_setting = [](const cfg::_base*, u32) { return std::string{}; };
		g_emu_callbacks.get_database_config = [](const std::string&) { return std::string{}; };

		g_emu_callbacks.resolve_path = [](std::string_view path) { return normalize_host_path(path); };
		g_emu_callbacks.resolve_path_may_not_exist = g_emu_callbacks.resolve_path;
		g_emu_callbacks.get_font_dirs = [] { return std::vector<std::string>{"/app0/fonts/", "/data/rpcs3/fonts/"}; };
		g_emu_callbacks.get_image_info = [](const std::string& path, std::string& sub_type, s32& width, s32& height, s32& orientation)
		{
			return image::info(path, sub_type, width, height, orientation);
		};
		g_emu_callbacks.get_scaled_image = [](const std::string& path, s32 target_width, s32 target_height, s32& width, s32& height, u8* dst, bool force_fit)
		{
			return image::scaled(path, target_width, target_height, width, height, dst, force_fit);
		};
		g_emu_callbacks.get_photo_path = [](std::string_view title) { return photo_path(title); };
		g_emu_callbacks.on_install_pkgs = [](const std::vector<std::string>& paths, bool from_disc)
		{
			// 0.34: free space cannot be checked against the packages beforehand (statvfs may be refused); this line
			// puts both numbers in startup.log before a multi-GB disc install starts.
			u64 package_bytes = 0;
			for (const auto& path : paths)
				if (fs::stat_t st{}; fs::get_stat(path, st)) package_bytes += st.size;
			ps4_startup_checkpoint(fmt::format("PKG install: %llu MiB to write, /data free %s", package_bytes >> 20, data_free_text()).c_str());
			// 0.34: the packages of a booting disc (PKGDIR/INSDIR/PS3_EXTRA) go through the installer screen: progress,
			// Circle cancels, one extraction thread. Every call here comes from a disc boot; System.cpp passes
			// launching_from_optical_drive, which is false for every disc image on the PS4, hence 'true'. Main thread,
			// before the game's renderer exists. Without the screen nothing was installed: plain install below.
			const ps4_install_result installed = ps4_install_packages(paths, true, &s_disc_install.bootables);
			if (!installed.display_unavailable)
			{
				s_disc_install.ok = installed.ok;
				s_disc_install.message = installed.message;
				if (!installed.ok)
				{
					ps4_startup_checkpoint(fmt::format("PKG install failed: '%s'%s (%s): %s; see the 'PKG:' lines in RPCS3.log", paths.front(),
						paths.size() > 1 ? fmt::format(" and %u more", paths.size() - 1) : std::string(), from_disc ? "from the game disc" : "package",
						installed.message).c_str());
					return false;
				}
				for (const auto& path : paths)
					ps4_startup_checkpoint(fmt::format("PKG installed: '%s' (%s)", path, from_disc ? "from the game disc" : "package").c_str());
				return true;
			}
			ps4_startup_checkpoint("PKG install: installer screen unavailable; installing without it");
			for (const auto& path : paths)
			{
				if (!rpcs3::utils::install_pkg(path, from_disc))
				{
					ps4_startup_checkpoint(fmt::format("PKG install failed: '%s' (%s); see the 'PKG:' lines in RPCS3.log", path, from_disc ? "from the game disc" : "package").c_str());
					return false;
				}

				ps4_startup_checkpoint(fmt::format("PKG installed: '%s' (%s)", path, from_disc ? "from the game disc" : "package").c_str());
			}
			s_disc_install.ok = true; // the plain install reports no bootables
			return true;
		};

		g_emu_callbacks.update_emu_settings = []
		{
			rpcs3::utils::configure_logs(Emu.IsStopped());
			// 0.39: the home menu's audio and overlay settings take effect in the running game, as on PC
			// (main_application::OnEmuSettingsChange, also run on the main thread there). configure_audio only stores a
			// request for the cellAudio thread; the call is skipped when the game stopped meanwhile.
			// 0.40: the feature row home_menu_apply. No title level reverts it (the near-cap titles that reach a level are
			// the ones whose sound needs the change), but the global safe mode is the player's request for the 0.38 path
			// everywhere, so there only the log settings apply at once (the saved values apply from the next boot).
			if (!safe_mode::enabled(safe_mode::feature::home_menu_apply) || safe_mode::global_this_boot())
			{
				// One line per game boot (the adapt title sequence changes at every boot), not one per changed value
				static std::atomic<u32> s_told_seq{0};
				if (const u32 seq = utils::ps4::adapt::g_title_seq.load(std::memory_order_acquire); s_told_seq.exchange(seq, std::memory_order_acq_rel) != seq)
					ps4_startup_checkpoint("home menu: audio and overlay settings apply from the next boot (safe mode: the 0.38 rule)");
				return;
			}
			Emu.CallFromMainThread([]
			{
				if (Emu.IsStopped()) return;
				audio::configure_audio(false);
				audio::configure_rsxaudio();
				rsx::overlays::reset_performance_overlay();
			});
		};
		// The home menu's Salvar writes only the settings the player changed in it (ps4_launcher.cpp), never the whole
		// running config: it holds the values the boot policy forced
		g_emu_callbacks.save_emu_settings = [] { ps4_home_menu_save(); };
		g_emu_callbacks.get_sendmessage_dialog = []() -> std::shared_ptr<SendMessageDialogBase> { return {}; };
		g_emu_callbacks.get_recvmessage_dialog = []() -> std::shared_ptr<RecvMessageDialogBase> { return {}; };
		g_emu_callbacks.play_sound = [](const std::string&, std::optional<f32>) {};
		g_emu_callbacks.add_breakpoint = [](u32) {};
		g_emu_callbacks.display_sleep_control_supported = [] { return false; };
		g_emu_callbacks.enable_display_sleep = [](bool) {};
		g_emu_callbacks.check_microphone_permissions = [] {};
		g_emu_callbacks.make_video_source = []() -> std::unique_ptr<video_source> { return {}; };
		g_emu_callbacks.enable_gamemode = [](bool) {};
	}
}

void ps4_request_shutdown()
{
    s_lifecycle.request_exit();
}

void ps4_request_return_to_menu()
{
    s_lifecycle.request_menu();
}

bool ps4_shutdown_requested()
{
    return s_lifecycle.exiting();
}

void qt_events_aware_op(int repeat_duration_ms, std::function<bool()> wrapped_op)
{
	ensure(wrapped_op);
	while (!wrapped_op())
	{
		if (thread_ctrl::is_main())
		{
			run_main_calls();
			// 0.29: a stop that can never finish (refused 'Emulation Join Thread') leaves the application instead.
			// 0.31: such a reason is never consumed elsewhere while a stop is unfinished (the main loop and the
			// pre-boot check leave for it too), so it is always still pending here; leaving also writes the
			// threads=low note for the running game.
			leave_if_threads_exhausted();
		}
		if (repeat_duration_ms > 0)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(repeat_duration_ms));
		}
		else
		{
			std::this_thread::yield();
		}
	}
}

[[noreturn]] void report_fatal_error(std::string_view text, bool, bool)
{
	// No allocation (may run under OOM, e.g. from LLVM's bad_alloc handler) and no abort():
	// on this console abort() => SIGABRT core => CE-34878-0. Record, notify, then park
	// (or SIGABRT with /data/rpcs3/coredump.flag), see ps4_fatal.cpp.
	char message[2048];
	const auto size = std::min<std::size_t>(text.size(), sizeof(message) - 1);
	std::memcpy(message, text.data(), size);
	message[size] = 0;
	ps4_fatal_report(message, "report_fatal_error");
}

int main(int argc, char** argv)
{
    // First: terminate/abort/exit policy and termination-signal handlers.
    ps4_fatal_init();
    fs::create_path("/data/rpcs3/");
    ps4_startup_main_entered();
    ps4_startup_checkpoint(fmt::format("RPCS3 PS4 EDITION %s", ps4_build_version).c_str());
    init_interface_language(); // before the first toast or XMB text built here
    // 0.40: the feature table (util/ps4_safe_mode.hpp): enable/kill flags, features.txt, safe-mode.flag; fixes the rows
    // that hold for the whole process (the Mesa cache below) and logs 'features (process): ...'
    safe_mode::init_process();
    {
        // 0.26: round-robin among equal priorities (Utilities/Thread.cpp). Before any emulator thread
        // exists, so std::thread/Mesa children inherit it.
        extern std::string ps4_thread_apply_round_robin() noexcept;
        ps4_startup_checkpoint(ps4_thread_apply_round_robin().c_str());
    }
    std::string previous_fatal_title;
    std::string previous_fatal = ps4_take_previous_fatal(&s_previous_fatal_reason, &previous_fatal_title);
    if (!previous_fatal.empty()) ps4_startup_checkpoint(previous_fatal.c_str());
    // 0.39: a savestate that cannot be read back ends the emulator in a fatal error while it loads (inFAMOUS EU, 0.38);
    // the first XMB says which game's savestate it was instead of the raw reason. Nothing is moved or removed.
    if (s_previous_fatal_reason.find("serial_breathe_and_tag") != std::string::npos)
    {
        previous_fatal = previous_fatal_title.empty() ? std::string(i18n::tr(i18n::str::MAIN_SAVESTATE_NAO_LIDO))
                                                      : i18n::trf(i18n::str::MAIN_SAVESTATE_JOGO_NAO_LIDO, {previous_fatal_title});
        ps4_startup_checkpoint(fmt::format("lifecycle: the previous session ended while reading a savestate%s (serial_breathe_and_tag); the XMB notice says so",
            previous_fatal_title.empty() ? std::string() : " of " + previous_fatal_title).c_str());
    }
    // 2 MB direct-memory pages are opt-in: partial mprotect inside a large mapping is unverified.
    if (fs::is_file("/data/rpcs3/large-pages.flag"))
    {
        utils::ps4::memory::set_large_pages(true);
        ps4_startup_checkpoint("memory: 2 MB direct-memory pages enabled by large-pages.flag");
    }
	// The RSX backend and callbacks are the original RPCS3 emulation core.
	// Keep the main thread alive to service callbacks from emulation threads.
	for (const OrbisSysModuleInternal module : {
		ORBIS_SYSMODULE_INTERNAL_USER_SERVICE,
		ORBIS_SYSMODULE_INTERNAL_PAD,
		ORBIS_SYSMODULE_INTERNAL_AUDIOOUT})
	{
		if (const auto result = static_cast<s32>(sceSysmoduleLoadModuleInternal(module)); result < 0)
		{
			report_fatal_error("Failed to load a required PS4 system module");
		}
	}

	if (sceSysmoduleIsLoaded(ORBIS_SYSMODULE_RANDOM) != 0 &&
		sceSysmoduleLoadModule(ORBIS_SYSMODULE_RANDOM) != 0)
		report_fatal_error("Failed to load PS4 system entropy module");
	extern s32 ps4_verify_system_entropy();
	if (ps4_verify_system_entropy() != 0)
		report_fatal_error("PS4 system entropy check failed");
	ps4_startup_checkpoint("native entropy: libSceRandom 64-byte request passed");

	fs::create_path(fs::get_config_dir());
    fs::create_path(fs::get_log_dir());
    apply_mesa_environment(); // before any EGL/Vulkan initialization
    apply_mesa_shader_cache(); // after mesa-env.txt, before the first thread (start_gnm_keepalive)
    start_gnm_keepalive();
    const std::string log_path = fs::get_log_dir();
    bool rpcs3_log_rotated = false; // 0.29: as s_mesa_log_rotated
    if (fs::is_file(log_path + "RPCS3.log"))
    {
        rpcs3_log_rotated = fs::rename(log_path + "RPCS3.log", log_path + "previous-RPCS3.log", true);
        if (!rpcs3_log_rotated) ps4_startup_checkpoint("log: could not preserve previous uncompressed RPCS3 log");
    }
    {
        // 0.27: keep the last 8 sessions in log/history/0..7 (0 = the session before this one); with only two, a
        // failing game tested earlier lost its logs when the next game ran.
        // 0.29: each file is copied with checked I/O and a size bound (see archive_log_file), and the line below lists
        // every file with its size (or the failing step and errno), so startup.log shows what the slot holds.
        const u64 archive_started = sceKernelGetProcessTime();
        const std::string history = log_path + "history/";
        fs::create_path(history);
        fs::remove_all(history + "7", true);
        u32 rotation_failures = 0;
        for (int i = 6; i >= 0; --i)
        {
            if (fs::is_dir(history + std::to_string(i)) &&
                !fs::rename(history + std::to_string(i), history + std::to_string(i + 1), true))
                rotation_failures++;
        }
        const std::string slot = history + "0/";
        if (fs::create_path(slot))
        {
            struct archive_item
            {
                std::string from;
                const char* to;
                u64 limit;    // bytes kept at most
                u64 head;     // of which from the start of the file when it is larger
                bool current; // the source belongs to the previous session (else it is older and is skipped)
                u64 middle;   // 0.34: kept error/PS4/AudioOut lines from the part left out, at most (0 = none)
            };
            // 0.34: RPCS3.log rotates at its cap (logs.cpp). Until the listener below renames them, the previous
            // session's parts keep their names RPCS3.part<N>.log; the newest one holds what came before previous-RPCS3.log.
            std::string newest_part;
            u32 newest_part_index = 0;
            for (const auto& entry : fs::dir(log_path))
            {
                const std::string_view name = entry.name;
                constexpr std::string_view prefix = "RPCS3.part", suffix = ".log";
                if (entry.is_directory || name.size() <= prefix.size() + suffix.size() || !name.starts_with(prefix) || !name.ends_with(suffix)) continue;
                const std::string_view digits = name.substr(prefix.size(), name.size() - prefix.size() - suffix.size());
                u32 index = 0;
                bool number = digits.size() <= 6;
                for (const char c : digits)
                {
                    number = number && c >= '0' && c <= '9';
                    index = index * 10 + static_cast<u32>(c - '0');
                }
                if (!number || index < newest_part_index) continue;
                newest_part_index = index;
                newest_part = entry.name;
            }
            // Bytes the kept-line filter may read in this boot (~0.2-0.4 s on the internal disk at most)
            u64 scan_budget = 32ull << 20;
            std::vector<char> buffer(256 * 1024);
            std::string files;
            u32 copied = 0, failed = 0;
            u64 bytes = 0;
            for (const archive_item& item : {
                // previous-startup.log / previous-fatal.txt are rotated (or removed) on every boot (ps4_rotate_session_logs)
                archive_item{"/data/rpcs3/previous-startup.log", "startup.log", 4ull << 20, 1ull << 20, true, 0},
                archive_item{"/data/rpcs3/previous-fatal.txt", "fatal.txt", 256ull << 10, 128ull << 10, true, 0},
                // RPCS3.log is capped at 64 MiB per file by its listener: keep the boot/config head, the last ~1.75 MiB
                // and (0.34) up to 512 KiB of error/PS4/AudioOut lines from between them
                archive_item{log_path + "previous-RPCS3.log", "RPCS3.log", 2ull << 20, 256ull << 10, rpcs3_log_rotated, 512ull << 10},
                archive_item{log_path + newest_part, "RPCS3.part.log", 1ull << 20, 256ull << 10, !newest_part.empty(), 256ull << 10},
                archive_item{log_path + "previous-mesa.log", "mesa.log", 2ull << 20, 256ull << 10, s_mesa_log_rotated, 0}})
            {
                if (!item.current) continue;
                const archive_result r = archive_log_file(item.from, slot + item.to, item.limit, item.head, buffer, item.middle, &scan_budget);
                if (!r.present) continue; // no such file in the previous session (e.g. no fatal.txt)
                fmt::append(files, "%s%s ", files.empty() ? "" : ", ", item.to);
                if (r.ok)
                {
                    copied++;
                    bytes += r.written;
                    if (r.size > item.limit) fmt::append(files, "%llu of %llu B (%s)", r.written, r.size, item.middle ? "head+kept lines+tail" : "head+tail");
                    else fmt::append(files, "%llu B", r.written);
                }
                else
                {
                    failed++;
                    fmt::append(files, "FAILED at %s (errno %d) after %llu of %llu B", r.step, r.error, r.written, r.size);
                }
            }
            ps4_startup_checkpoint(fmt::format("log: previous session archived in log/history/0 (%u file(s), %llu KiB in %llu ms%s%s): %s",
                copied, bytes >> 10, (sceKernelGetProcessTime() - archive_started) / 1000,
                failed ? fmt::format(", %u FAILED", failed) : std::string(),
                rotation_failures ? fmt::format(", %u slot rename(s) failed", rotation_failures) : std::string(),
                files.empty() ? std::string("nothing to archive") : files).c_str());
        }
        else
        {
            ps4_startup_checkpoint(fmt::format("log: could not create log/history/0 (%s); previous session not archived", fs::g_tls_error).c_str());
        }
    }
	auto log_file = logs::make_file_listener(fs::get_log_dir() + "RPCS3.log", 64 * 1024 * 1024);
    // 0.38: until set_init every log message was also stored in RAM for a listener added later, and the PS4 frontend
    // never called it (FIFA 14: 36 MiB after 2.6 h, kept across games). The messages logged before the file listener
    // existed go to RPCS3.log once, then the store is released; nothing is stored any more.
    logs::set_init({});
    ps4_startup_checkpoint("required modules and RPCS3 log ready; log: message store released (early messages written to RPCS3.log)");
    // 0.31: /data free space (toast when low), open-file limit raised to its maximum, memory census, first
    // temperature reading (read-only). Each check only logs or notifies and is skipped when its query fails.
    utils::ps4::health::boot_checks();
    report_switch_files();
    // 0.29: RPCS3-owned stack pool and its self-test (crash marker
    // /data/rpcs3/own-stacks-selftest.pending). Until this has run, a thread libkernel refuses is not rescued.
    ps4_startup_checkpoint(ps4_thread_stacks_init().c_str());
    // 0.29: orbis-compat prints its rlimit/sysctl report by itself at the first thread create after its logger is set
    // (the self-test above creates one); then this prints only 'reported earlier; max_threads_hits now N', else the
    // full report.
    orbis::limitsReport();
    run_boot_abi_probes();
    verify_native_atomic_wait();
    verify_native_socket_address();
    // 0.37: HTTPS self-test (loopback only, no internet) on its own thread; it runs curl_global_init on this (main) thread
    // first and writes its 'TLS self-test:' line itself. A definite failure disables 'Baixar patches do RPCS3'.
    ps4_tls_selftest_start();
#ifdef LLVM_AVAILABLE
    ps4_startup_checkpoint("LLVM: verify original JIT compiler and native executable memory");
    std::string llvm_error;
    if (!utils::ps4::verify_llvm(llvm_error))
        report_fatal_error("Native LLVM verification failed: " + llvm_error);
    ps4_startup_checkpoint("LLVM: generated arithmetic and native SysV calls passed (4 cases)");
#endif
    // SPU recompilers need round-toward-zero on SPU threads. The
    // test runs once on a fresh native thread; fixup_settings forces the SPU interpreter
    // when it fails (System.cpp).
    const bool fpu_rounding_ok = ps4_spu_rounding_ok();
    ps4_startup_checkpoint(fmt::format("FPU rounding self-test: %s", ps4_fpu_selftest_detail()).c_str());
    if (!fpu_rounding_ok)
        ps4_startup_checkpoint("FPU rounding self-test FAILED: SPU recompilers will be replaced by the SPU interpreter");
    // 0.29: whether get_system_time()/mftb and the SPU decrementer read the TSC or fall back to
    // clock_gettime on every call, the CPU features the recompilers see, and the measured core clock (Boost Mode).
    ps4_startup_checkpoint(fmt::format("CPU features: AVX=%d FMA3=%d, %u hardware threads; %s",
        utils::has_avx() ? 1 : 0, utils::has_fma3() ? 1 : 0, utils::get_thread_count(), ps4_tsc_info()).c_str());
    // ~100 ms once per process (cached; the launcher logs it again when a game is selected).
    ps4_startup_checkpoint(utils::ps4::cpu_probe_log_line().c_str());
    ps4_startup_checkpoint(environment_line(fpu_rounding_ok).c_str());
    // 0.37: the memory the console gives this process (read-only queries), for comparing Fat/Slim/Pro reports
    ps4_startup_checkpoint(fmt::format("memory: direct %llu MiB (sceKernelGetDirectMemorySize), flexible avail %llu MiB",
        static_cast<u64>(sceKernelGetDirectMemorySize()) >> 20, ps4_flexible_memory_free() >> 20).c_str());
    run_timer_probe();
    {
        // 0.39: /data/rpcs3 folders made by the GoldHEN FTP server (uid 0, 0755) refuse the app's writes: probe and
        // repair through the console's own FTP server before Emu.Init and the launcher write there (ps4_launcher.cpp)
        extern void ps4_repair_data_permissions();
        ps4_repair_data_permissions();
    }
    recheck_mesa_shader_cache(); // a cache folder the repair may have opened
	configure_callbacks();
	Emu.SetHasGui(false);
	Emu.SetHeadless(false);
	Emu.SetUsr("00000001");

#ifdef HAVE_VULKAN
	Emu.SetSupportedRenderers({video_renderer::vulkan, video_renderer::opengl, video_renderer::null});
	Emu.SetDefaultRenderer(video_renderer::vulkan);
	// An unmatched name deliberately makes VKGSRender use the first device.
	Emu.SetDefaultGraphicsAdapter("PS4 default Vulkan device");
#else
	Emu.SetSupportedRenderers({video_renderer::null});
	Emu.SetDefaultRenderer(video_renderer::null);
#endif
	Emu.Init();
    ps4_prepare_game_directories();
    ps4_startup_checkpoint("RPCS3 Emu.Init complete");
    ps4_startup_checkpoint(fmt::format("kernel internal memory at start: %llu bytes free; flexible memory: %llu bytes free (%llu at main)",
        ps4_internal_memory_free(), ps4_flexible_memory_free(), ps4_boot_flexible_memory_free()).c_str());
    if (fs::is_file("/data/rpcs3/thread-limit-probe.flag"))
    {
        // 0.29: measures a per-process thread cap directly (threads parked on 64 KiB caller
        // stacks, then released and joined). Only here: no game runs, so no RPCS3 create can be refused meanwhile.
        ps4_startup_checkpoint("thread limit probe: thread-limit-probe.flag present; holding up to 512 extra threads");
        const ps4_thread_load before = ps4_thread_load_now();
        orbis::threadLimitProbe(512);
        orbis::vmReport("after thread limit probe");
        // 0.39: a probe stopped by the per-process limit measured this console's thread cap in the governor's units
        // (threads alive before it + the ones it held): stored for this firmware in thread-cap.txt and applied at once
        unsigned extra = 0;
        bool limit = false;
        if (!ps4_take_thread_limit_probe(extra, limit))
        {
            ps4_startup_checkpoint("thread limit probe: no result line; thread-cap.txt unchanged");
        }
        else if (!limit)
        {
            ps4_startup_checkpoint(fmt::format("thread limit probe: %u extra thread(s) without a per-process thread limit answer; thread-cap.txt unchanged", extra).c_str());
        }
        else
        {
            const u32 cap = before.total + extra;
            ps4_startup_checkpoint(fmt::format("thread limit probe: measured cap %u = %u threads before the probe (live %u + exited not joined %u) + %u held by it",
                cap, before.total, before.live, before.unjoined, extra).c_str());
            utils::ps4::governor::set_thread_cap(cap, ps4_firmware_key());
        }
    }
    // 0.29: sampling profiler. 0.33: off by default (it never signals a thread then); /data/rpcs3/profile.flag turns
    // on the light profile (guest and compile threads only), full=1 in it the full profile (every thread but the
    // service/I-O deny list), rsx=1 adds the RSX/Vulkan threads; no-profile.flag still wins. Creates no thread; when
    // on, its self-test signals the main thread once.
    ps4_profiler_init();
    // 0.31: users and pads (up to four players) before the XMB opens. ps4_pads owns scePadInit and every pad handle
    // and keeps them open across games; it also marks the game window focused, so input never depends on
    // 'Background input'. The launcher's ps4_initialize_pad() is the same call. The force refresh seats the players
    // and writes the 'pads:' line.
    if (ps4_pads::initialize())
        ps4_pads::refresh(/*compact=*/true, /*force=*/true);
    else
        ps4_startup_checkpoint("pads: pad library not ready at boot; retried on the next refresh");
    // 0.40: a game closed from the home screen soon after it started: one toast suggests its per-game safe mode
    suggest_safe_mode_after_short_sessions();
    std::string last_game_path;
    const auto select_and_boot = [&](const std::string& preferred, const std::string& notice = {})
    {
      std::string path = ps4_select_game(preferred, xmb_text(notice));
      u32 lock_refusals = 0;
      u32 package_echoes = 0; // the same package handed back at once (no display): stops after 3
      u32 fatal_echoes = 0;   // a boot refused after a thread fatal handed back at once (no display): stops after 3
      while (!path.empty())
      {
        if (fs::is_file(path) && utils::ps4::file_kind(path) == utils::ps4::game_kind::package)
        {
            // 0.37: the XMB opens on the installed content's game (or on the PKG after a refusal)
            const ps4_install_result installed = ps4_install_packages({path}, false);
            // Without a display ps4_select_game hands 'preferred' straight back: the same package would be installed
            // again forever. A package returned again faster than anyone can choose it means the XMB did not show.
            const std::string package = path;
            const auto asked_at = std::chrono::steady_clock::now();
            path = ps4_select_game(installed.display_unavailable ? std::string{} : installed.select.empty() ? path : installed.select, installed.message);
            if (path == package && std::chrono::steady_clock::now() - asked_at < std::chrono::milliseconds(300) && ++package_echoes >= 3)
            {
                ps4_startup_checkpoint("XMB: the game list could not be shown; the package is not installed again");
                path.clear();
            }
            else if (path != package)
            {
                package_echoes = 0;
            }
            continue;
        }
        ps4_startup_checkpoint("starting original RPCS3 BootGame");
        // 0.31: BootGame needs the state 'stopped'. During a stop it fails with "Game is still running" (and replaces
        // the stop's pending after-stop action); with a game running it ends in a failed assertion, which is a fatal
        // error here. The previous game may still be tearing down (the XMB opened after a stop that took too long), or
        // it may have started another of its executables when that stop finished: stop such a game first.
        if (!Emu.IsStopped())
        {
            ps4_startup_checkpoint("lifecycle: a game is still running; stopping it before the new boot");
            Emu.Kill(false);
        }
        stop_wait previous = wait_for_full_stop(10'000'000);
        if (previous == stop_wait::relaunched)
        {
            ps4_startup_checkpoint("lifecycle: the previous game started another executable; stopping it before the new boot");
            Emu.Kill(false);
            previous = wait_for_full_stop(10'000'000);
        }
        // A thread fatal raised while no game was running (PKG installer, a failed boot's workers, late threads of the
        // previous game) is taken here, so it cannot stop the game booted next. It still needs a new process: the dead
        // thread kept what it held, so s_thread_fatal_seen is set below and ps4_fatal_pending() stays true, and the
        // check after this loop refuses the boot (the XMB asks to close and reopen the application).
        // 0.29: a loop, since an exhaustion reason and another reason can both be pending (two slots in ps4_fatal.cpp).
        for (char stale[512]; ps4_take_thread_fatal(stale, sizeof(stale));)
        {
            // 0.31: thread resources ran out while the previous stop is still unfinished: that stop can never finish
            // (its join thread was refused) and every boot would fail, so leave instead of discarding it.
            if (std::strstr(stale, "in function 'start'") && !Emu.IsStopped(true))
                leave_for_thread_exhaustion(stale, ps4_internal_memory_free());
            s_restart_recommended = true;
            s_thread_fatal_seen = true;
            ps4_startup_checkpoint(fmt::format("lifecycle: discarding thread fatal raised outside a game: %s", stale).c_str());
        }
        if (previous != stop_wait::stopped || !Emu.IsStopped(true))
        {
            // Booting now could only fail (or assert); keep the XMB and let the player try again.
            s_restart_recommended = true;
            ps4_startup_checkpoint("lifecycle: the previous game has not finished stopping; boot not attempted");
            path = ps4_select_game(path, xmb_text(xmb_notice(i18n::tr(i18n::str::COM_JOGO_ANTERIOR_AINDA_ESTA_FECHANDO))));
            continue;
        }
        // 0.38: a thread ended in a fatal error in this process (ps4_fatal_pending: any fatal record of this session,
        // also one the PKG installer took). It did not unwind (no exceptions on this console), so what it held (locks,
        // its LLVM compiler, heap blocks) stays held: no further boot in this process. The XMB stays usable (PKG
        // installs, settings) and asks to close the application with the PS button and open it again; the refusal
        // never leaves the application by itself (exit/abort end in CE-34878-0). The player choosing again is refused
        // again; only a path handed back faster than anyone can choose (no display) gives up, as the gates below do.
        if (s_thread_fatal_seen || ps4_fatal_pending())
        {
            s_restart_recommended = true;
            ps4_startup_checkpoint(fmt::format("lifecycle: boot of '%s' refused: a thread ended in a fatal error in this process; the XMB asks to close and reopen the application", path).c_str());
            const std::string refused = path;
            const auto asked_at = std::chrono::steady_clock::now();
            // 0.38: the chosen game boots by itself at the next start (boot-next.txt). 0.39: not a game a new process
            // cannot help: its ISO is incomplete, or it ended again with the fatal error of the previous session after a
            // boot-next start (the incomplete ISO is named first).
            std::string notice;
            if (!s_incomplete_image.empty() && normalize_host_path(path) == normalize_host_path(s_incomplete_image))
            {
                ps4_startup_checkpoint(fmt::format("lifecycle: boot-next not written for '%s': incomplete image", path).c_str());
                notice = incomplete_iso_fatal_notice();
            }
            else if (const unsigned raised = raised_safe_level(path); s_same_fatal_again && path == s_boot_next_game && !raised)
            {
                ps4_startup_checkpoint(fmt::format("lifecycle: '%s' ended with the same fatal as the previous session after a boot-next start; boot-next not written", path).c_str());
                notice = emulator_error_encoded(i18n::trf(i18n::str::MAIN_FECHOU_NOVO_MESMO_ERRO, {boot_title_name(path)}), false);
            }
            else
            {
                // The dialog asks to close and reopen the application; the boot-next note follows. 0.40: while the per-game
                // safe mode raises the game's level (up to 3), a new start is worth it even after the same fatal: the
                // next boot runs with fewer changes, and the dialog says so.
                std::string note = boot_next_note(path);
                if (raised)
                {
                    ps4_startup_checkpoint(fmt::format("lifecycle: '%s': boot-next written; its next boot opens in safe mode level %u%s", path, raised,
                        s_same_fatal_again && path == s_boot_next_game ? " (same fatal as the previous session)" : "").c_str());
                    note += " · " + i18n::trf(i18n::str::MAIN_ABRE_MODO_SEGURO_NIVEL, {raised});
                }
                notice = emulator_error_encoded(note);
            }
            path = ps4_select_game(path, xmb_text(notice));
            if (path == refused && std::chrono::steady_clock::now() - asked_at < std::chrono::milliseconds(300))
            {
                if (++fatal_echoes >= 3) return false;
            }
            else
            {
                fatal_echoes = 0;
            }
            continue;
        }
        // 0.33: a lock left taken by a thread that died in a fatal error would hang this boot; leave instead
        if (!global_locks_allow_boot())
        {
            // The launcher draws the dialog's own lines
            path = ps4_select_game(path, xmb_text(utils::ps4::xmb_notice::encode(utils::ps4::xmb_notice::kind::emulator_restart)));
            continue;
        }
        // 0.38: clean-process rule (clean_process_refusal): after a GPU loss, an unfinished stop, the memory governor's
        // level 3 or a renderer stop for lack of heap, or when the game's memory record does not fit the heap room the
        // previous game left (musl keeps its small heap), the game boots only in a new process. The XMB asks in a PS3
        // dialog to close and reopen the application; the game is in boot-next.txt and boots by itself then.
        {
            std::string why;
            const std::string refusal = clean_process_refusal(path, why);
            if (!refusal.empty())
            {
                s_restart_recommended = true;
                ps4_startup_checkpoint(fmt::format("lifecycle: boot of '%s' refused by the clean-process rule (%s); the XMB asks to close and reopen the application", path, why).c_str());
                const std::string refused = path;
                const auto asked_at = std::chrono::steady_clock::now();
                path = ps4_select_game(path, xmb_text(utils::ps4::xmb_notice::encode(utils::ps4::xmb_notice::kind::clean_process, refusal + " · " + boot_next_note(path))));
                if (path == refused && std::chrono::steady_clock::now() - asked_at < std::chrono::milliseconds(300))
                {
                    if (++fatal_echoes >= 3) return false;
                }
                else
                {
                    fatal_echoes = 0;
                }
                continue;
            }
        }
        // 0.33: per-title cache budget (ps4_health.hpp). Nothing runs here, and the folder of the game about to boot is
        // never removed; a removal only happens while /data is short of space or the cache is above its limit.
        utils::ps4::health::trim_caches_before_boot(boot_title_id(path));
        // 0.37: a game installed from a PS3 disc boots only with a disc of it in the drive (the XMB checked it just now
        // when it was chosen there)
        if (const std::string refused = ps4_disc_lock_boot_check(path); !refused.empty())
        {
            ps4_startup_checkpoint(fmt::format("disc lock: boot of '%s' refused before BootGame", path).c_str());
            if (++lock_refusals > 2) return false; // the XMB could not be shown (display failed): never loop
            path = ps4_select_game(path, xmb_text(xmb_notice(refused)));
            continue;
        }
        lock_refusals = 0;
        s_disc_install = {};
        Emu.SetForceBoot(true);
        const game_boot_result result = Emu.BootGame(path);
        if (result == game_boot_result::no_errors)
        {
            ps4_startup_checkpoint("RPCS3 BootGame succeeded");
            // 0.31: failure notes (thread budget, out of memory, GPU) are written for this game from now on; the next
            // return to the XMB clears the title again (health::on_return_to_xmb). Set only after a successful boot,
            // so the boot policy counts this boot and treats a later self-relaunch of the same game as one session.
            if (!utils::ps4::adapt::set_title(Emu.GetTitleID()))
                ps4_startup_checkpoint(fmt::format("adapt: title id '%s' cannot name an adapt file; failure notes are off for this game", Emu.GetTitleID()).c_str());
            last_game_path = path;
            s_frozen_seen = false;
            start_disc_watch(path);
            rearm_stall_watchdog();
            // 0.40: the session line, the session record and the safe mode's clean-session note count from here
            begin_session();
            // 0.38: the session's memory peaks for its record (written when the XMB opens again)
            s_games_booted++;
            utils::ps4::governor::begin_session();
            return true;
        }
        // 0.34: the XMB shows a pt-BR line that names what to fix; startup.log keeps the English result beside it
        std::string select;
        const std::string message = boot_failure_message(result, path, select);
        ps4_log.error("Failed to boot '%s': %s", path, result);
        ps4_startup_checkpoint(fmt::format("Falha ao iniciar: %s (XMB: %s)", result, utils::ps4::xmb_notice::describe(message)).c_str());
        Emu.Kill(false);
        // 0.31: Kill only starts the stop (or finds one in progress), and Emu.IsStopped() is already true while it
        // runs; wait for the real end so the next BootGame does not fail with "Game is still running". The wait
        // leaves the application when the stop can never finish (refused 'Emulation Join Thread').
        if (wait_for_full_stop(15'000'000) != stop_wait::stopped)
        {
            s_restart_recommended = true;
            s_stop_unfinished = true;
            ps4_startup_checkpoint("lifecycle: emulator still stopping 15 s after a failed boot; opening the XMB anyway");
        }
        if (take_gpu_lost()) ps4_fatal_note("lifecycle: ", "GPU device lost during a failed boot");
        utils::ps4::health::on_return_to_xmb();
        path = ps4_select_game(select, xmb_text(xmb_notice(message)));
      }
      return false;
    };
    {
        // 0.38: a game chosen in the previous process, which had to be closed first (boot-next.txt), boots after a
        // short countdown in the XMB (X now, Circle cancels)
        const std::string next = take_boot_next();
        // 0.39: kept for the same-fatal rule of the refusal after a thread fatal; the countdown names the PARAM.SFO title
        s_boot_next_game = next;
        const bool booted = next.empty() ? select_and_boot(boot_path(argc, argv), previous_fatal)
                                         : select_and_boot(next, utils::ps4::xmb_notice::encode(utils::ps4::xmb_notice::kind::boot_next, boot_title_name(next)));
        if (!booted) s_lifecycle.request_exit();
    }

    u64 next_progress_report = sceKernelGetProcessTime() + 10'000'000;
    u64 next_log_flush = 0;
    const bool detailed_diagnostics = fs::is_file("/data/rpcs3/diagnostics.flag");
    std::string menu_notice;
    bool gpu_lost_stop = false; // 0.31: the pending return to the XMB follows a GPU device loss
    bool frontend_stop = false; // 0.31: the frontend asked the game to stop (user request, emulator error, GPU loss)
	while (true)
	{
        // 0.29: cleared before the queue is drained, so a call queued from here on makes the wait below return at once.
        s_main_wakeup.release(0);
		run_main_calls();
        ps4_poll_system_service();
        {
            // 0.26: pause the guest while the PS4 home screen / system UI owns the app (PS button). GPU
            // submissions, audio and the emulation threads are then idle if the player closes the application
            // from there (0.25: the shell kill while a game was running ended in CE-34878-0). The game resumes
            // by itself when focus returns. Opt-out: /data/rpcs3/no-bg-pause.flag.
            static bool s_paused_for_background = false;
            static const bool s_background_pause = !fs::is_file("/data/rpcs3/no-bg-pause.flag");
            static bool s_was_background = false;
            static u64 s_background_since = 0;
            const bool background = ps4_in_background();
            const u64 focus_now = sceKernelGetProcessTime();
            if (background && !s_was_background) s_background_since = focus_now;
            // 0.31: the player came back to the game on this pass after at least 1 s on the home screen or system UI
            // (also with no-bg-pause.flag). A shorter blip is not taken as a deliberate return.
            bool returned = s_was_background && !background && focus_now - s_background_since >= 1'000'000;
            s_was_background = background;
            // 0.31: critical temperature (read-only sensors, sampled once per second inside)
            const bool too_hot = utils::ps4::health::pause_requested();
            if (s_background_pause && background && !s_paused_for_background && Emu.IsRunning())
            {
                s_paused_for_background = true;
                ps4_fatal_note("focus: home screen; pausing emulation until the game is resumed", "");
                Emu.Pause(false, false);
                logs::listener::sync_all();
                ps4_fatal_note("focus: emulation paused; GPU, audio and guest threads idle", "");
            }
            else if (s_paused_for_background && !background)
            {
                s_paused_for_background = false;
                if (Emu.IsPaused() && too_hot)
                {
                    // 0.31: the console got critically hot meanwhile: the pause goes on as a thermal pause.
                    returned = false;
                    s_thermal_paused = true;
                    s_thermal_cool_told = false;
                    s_thermal_paused_at = focus_now;
                    ps4_fatal_note("focus: back in the game, but the console is critically hot; emulation stays paused", "");
                    thermal_toast(i18n::tr(i18n::str::COM_CONSOLE_MUITO_QUENTE_JOGO_PAUSADO), true);
                }
                else if (Emu.IsPaused() && s_disc_paused)
                {
                    // 0.39: the game's disc is out of the drive: that pause goes on (poll_disc_watch resumes it)
                    ps4_fatal_note("focus: back in the game; it stays paused until its disc is back in the drive", "");
                    returned = false;
                }
                else if (Emu.IsPaused())
                {
                    ps4_fatal_note("focus: back in the game; resuming emulation", "");
                    Emu.Resume();
                    returned = false;
                }
            }
            poll_thermal_pause(too_hot, returned, s_lifecycle.requested());
        }
        // 0.31: the Vulkan device was lost (VK_ERROR_DEVICE_LOST): the game cannot draw any more, so it is stopped.
        // The XMB draws with the CPU into VideoOut buffers and can open again once the emulator has stopped; the
        // return-to-menu branch below gives that stop 10 s before it leaves the application.
        if (take_gpu_lost())
        {
            gpu_lost_stop = true;
            ps4_fatal_note("lifecycle: ", "GPU device lost; stopping the game (the application is left if the stop does not finish within 10 s)");
            if (!Emu.IsStopped()) s_lifecycle.request_menu();
        }
        // A named thread died through emergency_exit: stop the guest and return to the XMB
        // with the reason (its F line, fatal.txt and a toast were already written).
        if (char reason[512]; ps4_take_thread_fatal(reason, sizeof(reason)))
        {
            // 0.28: the console ran out of thread resources (Demon's Souls 0.27 entering its menu): see
            // leave_for_thread_exhaustion. 0.29: such a reason is returned first (ps4_fatal.cpp keeps it apart).
            const unsigned long long internal_free = ps4_internal_memory_free();
            if (std::strstr(reason, "in function 'start'") || (internal_free && internal_free < 1024 * 1024))
                leave_for_thread_exhaustion(reason, internal_free);
            ps4_startup_checkpoint(fmt::format("lifecycle: emulator thread failed (%s); returning to XMB", reason).c_str());
            // 0.39: the emulator-error dialog with the first line of the reason (not the PC advice of the lines after
            // it, such as "Try increasing ..." of a dead FIFO)
            const std::string_view first_line = std::string_view(reason).substr(0, std::string_view(reason).find('\n'));
            menu_notice = emulator_error_encoded(ps4_fatal_detail::utf8_prefix(first_line, 70));
            // 0.39: the game read past the end of its ISO before this (an incomplete copy): that is what to fix
            if (std::string image; take_incomplete_image(image, last_game_path))
            {
                s_incomplete_image = image;
                ps4_startup_checkpoint(fmt::format("lifecycle: '%s' read past the end of its image; the notice names the incomplete ISO", image).c_str());
                menu_notice = incomplete_iso_fatal_notice();
            }
            // 0.39: the game of boot-next.txt failed again as in the previous session (see the refusal in select_and_boot)
            s_same_fatal_again = !s_boot_next_game.empty() && last_game_path == s_boot_next_game &&
                ps4_fatal_detail::same_fatal_reason(reason, s_previous_fatal_reason);
            s_restart_recommended = true; // 0.31: the failed thread's resources stay lost in this process
            s_thread_fatal_seen = true;   // 0.33: arms the post-stop lock audit before the next boot
            if (!Emu.IsStopped()) s_lifecycle.request_menu();
        }
        // 0.39: a guest access violation freezes the emulation for a PC debugger the PS4 does not have (Thread.cpp,
        // Emu.Pause(true), then the overlay "The PS3 application has likely crashed"): return to the XMB and say why. No
        // host thread died (the faulting guest thread leaves at the stop, which kills a frozen emulation), so the next
        // game may boot in this process. /data/rpcs3/frozen-stays.flag keeps the 0.38 frozen picture and overlay.
        if (!s_frozen_seen && !Emu.IsStopped() && Emu.GetStatus(false) == system_state::frozen)
        {
            s_frozen_seen = true;
            static const bool s_frozen_stays = fs::is_file("/data/rpcs3/frozen-stays.flag");
            char fault[160]{};
            const bool known = ps4_take_guest_fault(fault, sizeof(fault));
            // 0.40: a crash of the game for the per-game safe mode (also when it stays frozen)
            safe_mode::note_failure(safe_mode::fail_kind::crash);
            if (s_frozen_stays)
            {
                ps4_startup_checkpoint(fmt::format("lifecycle: the game froze after a guest access violation (%s); it stays frozen (frozen-stays.flag)",
                    known ? fault : "not recorded").c_str());
            }
            else
            {
                ps4_startup_checkpoint(fmt::format("lifecycle: the game froze after a guest access violation (%s); returning to the XMB",
                    known ? fault : "not recorded").c_str());
                const std::string_view text(fault);
                const std::string_view thread = known ? text.substr(0, text.find(": ")) : std::string_view();
                // A thread fatal of the same pass keeps its own notice
                if (menu_notice.empty())
                {
                    menu_notice = thread.empty() ? std::string(i18n::tr(i18n::str::MAIN_JOGO_EMULADO_PAROU))
                        : i18n::trf(i18n::str::MAIN_JOGO_EMULADO_TRAVOU_ACESSO_INVALIDO, {thread});
                }
                s_lifecycle.request_menu();
            }
        }
        // 0.38: the disc of a game installed from a PS3 disc left the drive: the game was closed. 0.39: it pauses until
        // a disc of the game is back (poll_disc_watch).
        if (!Emu.IsStopped() && !s_lifecycle.requested()) poll_disc_watch(sceKernelGetProcessTime(), menu_notice);
        // 0.38: the renderer could not serve a required allocation (GPU window, or the CPU heap for an RSX container)
        // and asked for a graceful stop (util/ps4_vram_policy.hpp; it already showed a toast and asked for the XMB): the
        // XMB says why in a system dialog. A CPU heap stop also arms the clean-process rule.
        if (utils::ps4::vram::stop_requested())
        {
            char detail[utils::ps4::vram::stop_detail_size]{};
            if (const auto cause = utils::ps4::vram::take_stop_request(detail, sizeof(detail)); cause != utils::ps4::vram::stop_cause::none)
            {
                const bool heap = cause == utils::ps4::vram::stop_cause::cpu_heap;
                // 0.39: the log names every cause (0.38 printed "GPU window" for each one that was not the heap)
                const char* const label = heap ? "CPU heap" : cause == utils::ps4::vram::stop_cause::vram ? "GPU window"
                    : cause == utils::ps4::vram::stop_cause::gpu_hang ? "GPU hang" : cause == utils::ps4::vram::stop_cause::rsx_dead_fifo ? "RSX dead FIFO" : "renderer";
                ps4_startup_checkpoint(fmt::format("lifecycle: the renderer asked to stop the game (%s): %s", label, detail).c_str());
                if (heap)
                {
                    s_heap_stop_seen = true;
                    s_restart_recommended = true;
                }
                menu_notice = utils::ps4::vram::stop_notice_encoded(cause);
                if (!Emu.IsStopped()) s_lifecycle.request_menu();
            }
        }
        // A game may request its own exit through the original LV2 path.
        if (Emu.IsStopped() && !s_lifecycle.requested()) s_lifecycle.request_menu();
        switch (s_lifecycle.poll(Emu.IsStopped()))
        {
        case utils::ps4::lifecycle::action::stop_emulation:
            ps4_startup_checkpoint("shutdown: request original RPCS3 graceful stop");
            frontend_stop = true;
            Emu.GracefulShutdown(false, true);
            break;
        case utils::ps4::lifecycle::action::return_to_menu:
            if (s_lifecycle.consume_menu_request())
            {
                ps4_startup_checkpoint("lifecycle: guest stopped; returning to XMB");
                // 0.31: open the XMB only once the teardown has finished (the 0.29 loop opened it while the state was
                // still 'stopping', before the final teardown call had run on this thread). After a GPU device loss
                // the stop gets 10 s: a teardown that waits on the dead device may never finish, and then the
                // application is left instead.
                stop_wait stop = wait_for_full_stop(gpu_lost_stop ? 10'000'000 : 15'000'000);
                if (stop == stop_wait::relaunched)
                {
                    if (!frontend_stop && !gpu_lost_stop && menu_notice.empty())
                    {
                        // 0.31: the game stopped itself and started another of its executables (exitspawn, or a
                        // restart from the home menu): stay in the game. The adapt title stays set, so the boot
                        // policy treats it as the same session; a relaunch into another title moves it there.
                        ps4_startup_checkpoint("lifecycle: staying in the game (it relaunched itself); XMB not opened");
                        const std::string running = Emu.GetTitleID();
                        char noted[utils::ps4::adapt::title_size];
                        // 0.40: a relaunch into another title ends this session and starts that title's one
                        if (running != s_session.title)
                        {
                            end_session("the game started another title's executable");
                            if (!running.empty() && (!utils::ps4::adapt::current_title(noted) || running != noted))
                                utils::ps4::adapt::set_title(running);
                            rearm_stall_watchdog();
                            begin_session();
                            next_progress_report = sceKernelGetProcessTime() + 10'000'000;
                            break;
                        }
                        if (!running.empty() && (!utils::ps4::adapt::current_title(noted) || running != noted))
                            utils::ps4::adapt::set_title(running);
                        rearm_stall_watchdog();
                        next_progress_report = sceKernelGetProcessTime() + 10'000'000;
                        break;
                    }
                    // The frontend stopped the game (the player's request, an emulator error or a GPU loss), but the
                    // stop started another executable. Stop that one too, without an exit request it could answer
                    // with yet another relaunch.
                    ps4_startup_checkpoint("lifecycle: the game started another executable while the frontend was stopping it; stopping that one too");
                    Emu.Kill(false);
                    stop = wait_for_full_stop(10'000'000);
                }
                frontend_stop = false;
                if (stop != stop_wait::stopped)
                {
                    s_restart_recommended = true;
                    s_stop_unfinished = true;
                    if (gpu_lost_stop)
                    {
                        ps4_fatal_note("lifecycle: ", "the emulator did not stop within 10 s after the GPU device was lost; leaving application");
                        if (!ps4_try_leave_application("GPU lost"))
                            ps4_notify(i18n::tr(i18n::str::MAIN_RPCS3_GPU_PAROU_RESPONDER_FECHE));
                    }
                    else
                    {
                        ps4_startup_checkpoint("lifecycle: emulator still stopping after 15 s; opening the XMB anyway");
                    }
                    // 0.40: a stop that did not finish is a failure of the game for the per-game safe mode, except while a
                    // PPU module compile holds it (uninterruptible; the Stop Watchdog waits 25 s for it)
                    if (!g_watchdog_hold_ctr) safe_mode::note_failure(safe_mode::fail_kind::stall);
                }
                if (take_gpu_lost())
                {
                    gpu_lost_stop = true;
                    ps4_fatal_note("lifecycle: ", "GPU device lost while the game was stopping");
                }
                std::string notice = menu_notice.empty() ? std::string(i18n::tr(i18n::str::MAIN_JOGO_ENCERRADO)) : std::move(menu_notice);
                // 0.39: the game read past the end of its ISO (an incomplete copy): that is the reason to show, whatever
                // stopped the game (a thread fatal already took it)
                if (std::string image; take_incomplete_image(image, last_game_path))
                {
                    s_incomplete_image = image;
                    ps4_startup_checkpoint(fmt::format("lifecycle: '%s' read past the end of its image; the notice names the incomplete ISO", image).c_str());
                    notice = is_emulator_error(notice) ? incomplete_iso_fatal_notice() : incomplete_iso_notice();
                }
                if (gpu_lost_stop) notice = i18n::tr(i18n::str::MAIN_GPU_PAROU_RESPONDER_DISPOSITIVO_PERDIDO);
                menu_notice.clear();
                gpu_lost_stop = false;
                s_disc_watch.stop("back to the XMB");
                s_disc_paused = false;
                s_disc_resume_due = false;
                // the cheat session of the game that ended (the next boot starts its own)
                ps4_cheats::reset();
                // 0.40: the session line and the end of the session record, while the adapt title is still set
                end_session("back to the XMB");
                // 0.31: on the main thread, after the stop and right before the XMB: clears the adapt title (a failure
                // from now on is not charged to the game that just ended) and returns free heap carve-outs.
                utils::ps4::health::on_return_to_xmb();
                if (!select_and_boot(last_game_path, xmb_notice(notice))) s_lifecycle.request_exit();
                next_progress_report = sceKernelGetProcessTime() + 10'000'000;
            }
            break;
        case utils::ps4::lifecycle::action::exit_process:
            exit_native_process();
        default: break;
        }
        if (const u64 now = sceKernelGetProcessTime(); now >= next_progress_report)
        {
            ps4_startup_checkpoint(fmt::format("progress: files=%u/%u, programs=%u/%u, stage='%s'%s",
                g_progr_fdone.load(), g_progr_ftotal.load(), g_progr_pdone.load(),
                g_progr_ptotal.load(), g_progr_text.operator std::string(), s_first_compile.eta).c_str());
            {
                // Direct-memory / lazy-arena accounting: shows slot pressure before ENOMEM.
                char memory_line[512];
                ps4_memory_report(memory_line, sizeof(memory_line));
                ps4_startup_checkpoint(memory_line);
                // 0.28: libkernel internal pool (ScePthread mutexes/condvars/attrs). A falling value between reports is a
                // leak. 0.29: it is not what refuses threads: Demon's Souls 0.28 failed scePthreadCreate with 13.6 MB of it
                // free; thread stacks come from flexible memory (census line below).
                static unsigned long long s_last_internal = 0;
                const unsigned long long internal_free = ps4_internal_memory_free();
                ps4_startup_checkpoint(fmt::format("kernel internal memory: %llu bytes free (%+lld since last report)%s",
                    internal_free, s_last_internal ? static_cast<long long>(internal_free) - static_cast<long long>(s_last_internal) : 0ll,
                    internal_free && internal_free < 1024 * 1024 ? " - LOW: mutex/condvar/attr creation may start failing" : "").c_str());
                s_last_internal = internal_free;
                // 0.29: created - returned is not a live count ('returned' counts only start routines that returned;
                // thread_base threads leave through pthread_exit). The census line shows created/failed, running (orbis
                // live estimate from exit notes), exited but not joined (stack still pinned), joined, returned, and
                // libkernel stacks held = flexible used since main / 2 MiB.
                char census[768];
                ps4_orbis_census_line(census, sizeof(census));
                ps4_startup_checkpoint(census);
            }
            // 0.29: live RPCS3 threads by name (registry) and the own-stack pool / rescue state.
            ps4_startup_checkpoint(ps4_thread_live_summary().c_str());
            ps4_startup_checkpoint(ps4_thread_stacks_report().c_str());
            // 0.31: one 'health:' line (heap, flexible memory, governor, /data free space, temperature), the VRAM line
            // the RSX thread published, and a memory census every 15 minutes (5 under heap pressure).
            utils::ps4::health::periodic(now);
            // 0.36: sceGnmSubmitDone calls of the keepalive thread
            if (const std::string gnm = gnm_keepalive_report(); !gnm.empty()) ps4_startup_checkpoint(gnm.c_str());
            // 0.36: HR waits since boot (0.37: on by default), and the threads an equeue error put back on sceKernelUsleep
            if (utils::ps4::hr_wait_enabled())
            {
                namespace hr = utils::ps4::hr_wait_detail;
                ps4_startup_checkpoint(fmt::format("hr wait: %llu waits since boot, %llu thread(s) back on sceKernelUsleep after an equeue error (last 0x%x)",
                    hr::g_waits.load(std::memory_order_relaxed), hr::g_thread_failures.load(std::memory_order_relaxed),
                    static_cast<u32>(hr::g_last_error.load(std::memory_order_relaxed))).c_str());
            }
            {
                // 0.29: orbis-compat VA-map walk + thread picture. Its cost (one kernel query per map entry) is unknown
                // on the console: every report while a walk takes under 20 ms, else every 5th report (~5 min). orbis
                // itself prints every call up to the 12th, then every 5th.
                static u32 s_vm_reports = 0;
                static u64 s_vm_cost_us = 0;
                if (s_vm_cost_us < 20'000 || s_vm_reports % 5 == 0)
                {
                    const u64 vm_started = sceKernelGetProcessTime();
                    orbis::vmReport("periodic");
                    s_vm_cost_us = sceKernelGetProcessTime() - vm_started;
                }
                s_vm_reports++;
            }
            // 0.29: guest sleep counters since the previous report (precise vs host usleeps, oversleep,
            // backoff tick sleeps). Called from here only: the function keeps the interval state.
            // 0.34: plus the average busy SPURS kernels of the same window (SPUThread.cpp; also keeps its own window).
            if (std::string sleep_stats = ps4_lv2_sleep_stats(); !sleep_stats.empty())
            {
                extern std::string ps4_spurs_busy_text() noexcept;
                if (const std::string spurs = ps4_spurs_busy_text(); !spurs.empty()) sleep_stats += " | " + spurs;
                ps4_startup_checkpoint(sleep_stats.c_str());
            }
            // diagnostics.flag keeps the periodic full snapshot; the stall watchdog below is always on.
            if (Emu.IsRunning() && detailed_diagnostics) report_guest_threads();
            // 0.39: the RPCS3.log heartbeat (FPS, RSX loop age, threads, audio rate)
            if (!Emu.IsStopped()) log_heartbeat(now);
            // Snapshot diagnostics open/close many log records. Keep those
            // off the ten-second gameplay cadence; RSX reports actual FPS.
            next_progress_report = now + 60'000'000;
        }
        // Live cheats (constant and once) of the running game: returns at once without one, while paused, stopping or
        // in the background; at most 4096 accesses / 2 ms per pass
        ps4_cheats::tick(sceKernelGetProcessTime());
        poll_stall_watchdog(sceKernelGetProcessTime());
        // 0.40: HLE loop lines (every 10 s, log only)
        poll_hle_loops(sceKernelGetProcessTime());
        // 0.40: the repeated-line limiter's summaries and notes (util/logs.cpp), once per second
        if (const u64 now = sceKernelGetProcessTime(); now >= next_log_flush)
        {
            next_log_flush = now + 1'000'000;
            logs::ps4_flush_repeated_lines();
        }
        // 0.29: the sampling profiler runs here (no thread of its own); it asks for ~1 ms ticks only while a census
        // waits for replies or a burst samples.
        const bool running = Emu.IsRunning();
        const bool profiler_busy = ps4_profiler_tick(sceKernelGetProcessTime(), running && !ps4_in_background());
        if (running && !profiler_busy)
        {
            // 0.29: nothing in this loop is per-frame. While a game runs, sleep until a call is
            // queued for the main thread or 8 ms pass (focus, thread fatal, lifecycle and the 1 s stall watchdog all
            // tolerate that), instead of ~500-1000 wakeups/s on a busy core.
            s_main_wakeup.wait(0, atomic_wait_timeout{8'000'000});
        }
        else if (!profiler_busy && !s_lifecycle.requested())
        {
            // 0.34: no 1 ms polling while nothing runs: a paused game (home screen, thermal pause) waits 50 ms, so
            // the resume after the focus event stays within 50 ms; any other state (starting) waits one frame.
            // A main-thread call still wakes the loop at once.
            s_main_wakeup.wait(0, atomic_wait_timeout{Emu.IsPaused() ? 50'000'000u : 16'000'000u});
        }
        else
        {
            // A stop or exit in progress, or the profiler asking for ~1 ms ticks.
            sceKernelUsleep(1000);
        }
	}
}
