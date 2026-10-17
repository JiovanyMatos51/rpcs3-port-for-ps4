#include "stdafx.h"
#include "ps4_launcher.h"
#include "ps4_fatal.h"
#include "ps4_pads.h"
#include "ps4_ime.h"
#include "Emu/NP/ps4_rpcn_service.h"
#include "util/ps4_build_version.hpp"
#include "Utilities/File.h"
#include "util/ps4_signal_context.hpp"
#include "util/ps4_game_library.hpp"
#include "util/ps4_xmb.hpp"
#include "util/ps4_xmb_dialog.hpp"
#include "util/ps4_settings.hpp"
#include "util/ps4_cpu_probe.hpp"
#include "util/ps4_memory_governor.hpp"
#include "util/ps4_patches.hpp"
#include "util/ps4_cheats.hpp"
#include "util/ps4_health.hpp"
#include "util/ps4_config_db.hpp"
#include "util/ps4_firmware.hpp"
#include "util/ps4_disc_probe.hpp"
#include "util/ps4_disc_install.hpp"
#include "util/ps4_disc_lock.hpp"
#include "util/ps4_padlock_icon.hpp"
#include "util/ps4_pkg_check.hpp"
#include "util/ps4_fs_repair.hpp"
#include "util/ps4_i18n.hpp"
#include "util/ps4_xmb_notice.hpp"
#include "Emu/NP/ps4_tls_selftest.h"
#include "Emu/System.h"
#include "Emu/system_utils.hpp"
#include "Emu/vfs_config.h"
#include "Loader/ISO.h"
#include "Loader/PSF.h"
#include "Crypto/unpkg.h"
#include "Utilities/Thread.h"
#include <orbis/Pad.h>
#include <orbis/VideoOut.h>
#include <orbis/libkernel.h>
#include <stb_truetype.h>
#include <stb_image.h>
#include "3rdparty/zlib/zlib/zlib.h"

#include <emmintrin.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <map>
#include <cctype>
#include <utility>
#include <sys/stat.h>

LOG_CHANNEL(ps4_launcher_log, "PS4 Launcher");

// 0.39: every text of the launcher comes from util/ps4_i18n_launcher.inc (and the shared tables) in the interface language
namespace i18n = utils::ps4::i18n;
using i18n::str;

namespace
{
// Zero-initialized state, usable while the original RPCS3 constructors run.
std::atomic<unsigned int> s_memory_trace_count{0};
std::atomic<bool> s_main_entered{false};
std::atomic<bool> s_session_rotated{false};
// 0.29: ps4_startup_checkpoint keeps one append stream per session. fopen+fprintf+fclose
// per line cost 3-11 ms on /data (consecutive-line deltas in startup.log), paid by the RSX thread every 10 s,
// inside sys_rsx_context_iomap's VM writer lock and ~480 times per boot. The stream is used only after the
// rotation below has finished (s_session_ready), so the rename can never strand it on previous-startup.log.
std::atomic<bool> s_session_ready{false};
std::mutex s_checkpoint_mutex; // constexpr constructor: constant-initialized, usable before main
FILE* s_checkpoint_file = nullptr;
}

void ps4_trace_memory(const char* operation, const void* requested, std::size_t size,
    int flags, int result, const void* mapped)
{
    if (s_main_entered.load(std::memory_order_relaxed) && result == 0) return;
    const auto sequence = s_memory_trace_count.fetch_add(1, std::memory_order_relaxed);
    if (sequence >= 256) return;
    if (sequence == 0)
    {
        ps4_rotate_session_logs();
        ps4_startup_checkpoint("RPCS3 native: global initialization memory trace");
    }
    if (FILE* file = std::fopen("/data/rpcs3/startup.log", "ab"))
    {
        std::fprintf(file, "%s requested=%p size=0x%llx flags=0x%x result=0x%x mapped=%p\n",
            operation, requested, static_cast<unsigned long long>(size), flags, result, mapped);
        std::fclose(file);
    }
}

void ps4_rotate_session_logs()
{
    // Once per process, before the first line of this session is written. Runs from the
    // first pre-main memory trace or from ps4_fatal_init(), whichever comes first.
    if (s_session_rotated.exchange(true)) return;
    sceKernelMkdir("/data/rpcs3", 0777);
    // Keep the last session when reopening after a crash. A relaunch must
    // not erase the evidence needed to diagnose a game boot failure.
    if (FILE* previous = std::fopen("/data/rpcs3/startup.log", "rb"))
    {
        std::fclose(previous);
        std::remove("/data/rpcs3/previous-startup.log");
        std::rename("/data/rpcs3/startup.log", "/data/rpcs3/previous-startup.log");
    }
    // Always drop the old record: a session that ended without fatal.txt must not inherit
    // the previous session's error (0.24 showed a stale 0.23 "abnormally terminated").
    std::remove("/data/rpcs3/previous-fatal.txt");
    std::remove("/data/rpcs3/previous-fatal.shown");
    if (FILE* previous = std::fopen("/data/rpcs3/fatal.txt", "rb"))
    {
        std::fclose(previous);
        std::rename("/data/rpcs3/fatal.txt", "/data/rpcs3/previous-fatal.txt");
    }
    if (FILE* file = std::fopen("/data/rpcs3/startup.log", "wb")) std::fclose(file);
    s_session_ready.store(true, std::memory_order_release); // 0.29: this session's startup.log exists now
}

void ps4_startup_main_entered()
{
    s_main_entered = true;
    ps4_startup_checkpoint("entered RPCS3 main");
}

void ps4_trace_fault(const utils::ps4::signal_context& context, const void* address, bool unhandled)
{
    // Managed guest faults are normal. Capture post-main state only after
    // all recovery paths have rejected the fault, so we retain its caller.
    if (s_main_entered && !unhandled) return;
    extern char __text_start[];
    const auto& machine = context.machine;
    const auto base = reinterpret_cast<std::uintptr_t>(__text_start);
    if (FILE* file = std::fopen("/data/rpcs3/startup.log", "ab"))
    {
        if (unhandled && s_main_entered && !g_tls_serialize_name.empty())
            std::fprintf(file, "active component: %.*s\n", static_cast<int>(g_tls_serialize_name.size()), g_tls_serialize_name.data());
        std::fprintf(file, "%s fault: addr=%p RIP=0x%llx module=0x%llx offset=0x%llx RSP=0x%llx RBP=0x%llx error=0x%llx trap=%u\n",
            unhandled ? "unhandled" : "pre-main", address, static_cast<unsigned long long>(machine.rip), static_cast<unsigned long long>(base),
            static_cast<unsigned long long>(machine.rip - base), static_cast<unsigned long long>(machine.rsp),
            static_cast<unsigned long long>(machine.rbp), static_cast<unsigned long long>(machine.error), machine.trapno);
        std::fprintf(file, "RAX=0x%llx RBX=0x%llx RCX=0x%llx RDX=0x%llx RSI=0x%llx RDI=0x%llx\n",
            static_cast<unsigned long long>(machine.rax), static_cast<unsigned long long>(machine.rbx),
            static_cast<unsigned long long>(machine.rcx), static_cast<unsigned long long>(machine.rdx),
            static_cast<unsigned long long>(machine.rsi), static_cast<unsigned long long>(machine.rdi));
        void* begin = nullptr;
        void* end = nullptr;
        int32_t protection = 0;
        constexpr std::size_t count = 24;
        // The fault's stack might itself be invalid. Read only after the kernel
        // confirms this entire span belongs to a readable mapping.
        if (sceKernelQueryMemoryProtection(reinterpret_cast<void*>(machine.rsp), &begin, &end, &protection) == 0 &&
            (protection & 1) && machine.rsp >= reinterpret_cast<std::uintptr_t>(begin) &&
            machine.rsp < reinterpret_cast<std::uintptr_t>(end) &&
            reinterpret_cast<std::uintptr_t>(end) - machine.rsp >= count * sizeof(std::uint64_t))
        {
            const auto* stack = reinterpret_cast<const std::uint64_t*>(machine.rsp);
            for (std::size_t i = 0; i < count; ++i)
                std::fprintf(file, "stack[%02u]=0x%llx\n", static_cast<unsigned int>(i), static_cast<unsigned long long>(stack[i]));
        }
        else std::fprintf(file, "fault stack could not be validated as readable\n");
        std::fclose(file);
    }
}

// 0.31: ps4_pads owns UserService, scePadInit and every pad handle; this stays for existing callers.
bool ps4_initialize_pad()
{
    return ps4_pads::initialize();
}

void ps4_startup_checkpoint(const char* stage)
{
    sceKernelDebugOutText(0, stage);
    const auto now = static_cast<unsigned long long>(sceKernelGetProcessTime());
    // 0.29: one persistent "ab" stream (see s_session_ready). fflush still hands every line to the kernel at once
    // (same durability as before: no fsync either way; ps4_fatal.cpp keeps its own O_APPEND+fsync path). O_APPEND
    // keeps whole lines when other writers (memory/fault traces, the fatal path) append in between. try_lock, not
    // lock: a contended or re-entered call (e.g. from a signal handler) takes the old open/close path instead of
    // waiting, so this can never deadlock. Any stream error drops the stream and falls back for that line.
    if (s_session_ready.load(std::memory_order_acquire))
    {
        std::unique_lock lock(s_checkpoint_mutex, std::try_to_lock);
        if (lock.owns_lock())
        {
            if (!s_checkpoint_file) s_checkpoint_file = std::fopen("/data/rpcs3/startup.log", "ab");
            if (s_checkpoint_file)
            {
                if (std::fprintf(s_checkpoint_file, "%llu %s\n", now, stage) >= 0 && std::fflush(s_checkpoint_file) == 0) return;
                std::fclose(s_checkpoint_file); // reopened by the next line
                s_checkpoint_file = nullptr;
            }
        }
    }
    if (FILE* file = std::fopen("/data/rpcs3/startup.log", "ab"))
    {
        std::fprintf(file, "%llu %s\n", now, stage);
        std::fclose(file);
    }
}

u32 ps4_import_licenses();

namespace
{
using xmb_entry = utils::ps4::xmb::entry;
using xmb_image = utils::ps4::xmb::image;
using xmb_dialog = utils::ps4::xmb::dialog;
constexpr std::size_t no_row = static_cast<std::size_t>(-1);

// 0.39: byte bound of an XMB line of about 'latin' bytes in the interface language: an Arabic letter takes two bytes
// (the XMB fits a right-to-left line to the screen and ends it with an ellipsis)
std::size_t line_bytes(std::size_t latin)
{
    return i18n::rtl() ? 2 * latin : latin;
}

// Reads a whole file of at most 'limit' bytes. Unlike fs::file::to_vector, a short read (I/O error, USB drive
// pulled) only fails the read instead of ending in a fatal error.
bool read_whole(const fs::file& source, u64 limit, std::vector<uint8_t>& out)
{
    out.clear();
    if (!source) return false;
    const u64 size = source.size();
    if (size > limit) return false;
    out.resize(static_cast<std::size_t>(size));
    return size == 0 || source.read(out.data(), size) == size;
}

// Copies a finished frame into write-combined scanout memory with non-temporal stores. The destination is
// 16-byte aligned: the mapping is 2 MiB aligned and every frame is a multiple of 16 bytes.
void stream_copy(uint32_t* dst, const uint32_t* src, std::size_t count)
{
    auto* d = reinterpret_cast<__m128i*>(dst);
    const auto* s = reinterpret_cast<const __m128i*>(src);
    const std::size_t blocks = count / 4;
    std::size_t i = 0;
    for (; i + 4 <= blocks; i += 4)
    {
        _mm_stream_si128(d + i, _mm_loadu_si128(s + i));
        _mm_stream_si128(d + i + 1, _mm_loadu_si128(s + i + 1));
        _mm_stream_si128(d + i + 2, _mm_loadu_si128(s + i + 2));
        _mm_stream_si128(d + i + 3, _mm_loadu_si128(s + i + 3));
    }
    for (; i < blocks; ++i) _mm_stream_si128(d + i, _mm_loadu_si128(s + i));
    for (std::size_t k = blocks * 4; k < count; ++k) dst[k] = src[k];
    _mm_sfence();
}

// 0.31: frames are drawn into a cached back buffer and copied once into the scanout buffer that is not on screen.
// Blending straight into the write-combined VideoOut memory made every blend an uncached read. present() no longer
// blocks until its own flip: it waits for the previous one (that is what frees the buffer it writes), so drawing
// the next frame overlaps the vblank wait.
struct launcher_display
{
    static constexpr int width = 1280, height = 720;
    static constexpr size_t frame_bytes = width * height * 4;
    static constexpr size_t memory_bytes = 8 * 1024 * 1024;
    // Longest wait for the previous flip in present(); 0.29 polled up to 2000 x 1 ms.
    static constexpr u64 flip_timeout_us = 100'000;
    // One flip-event wait never sleeps longer than this, so a queue that stops delivering events costs a few
    // milliseconds of polling instead of the whole timeout.
    static constexpr u64 flip_event_slice_us = 4'000;
    int video = -1;
    int buffers = -1;
    off_t offset = -1;
    void* memory = nullptr;
    int current = 0;        // scanout buffer that receives the next frame
    int64_t submitted = 0;  // flipArg of the last submitted flip (1, 2, 3, ...)
    OrbisKernelEqueue flip_queue{};
    bool flip_queue_created = false;
    bool flip_events = false; // sleep on flip events instead of polling the flip status
    unsigned flip_event_errors = 0, flip_event_misses = 0;
    u64 last_wait_us = 0, last_copy_us = 0;
    std::vector<uint32_t> back; // frame being drawn, in cached memory
    utils::ps4::xmb::font typeface;

    ~launcher_display()
    {
        // Never unregister or unmap a buffer that a pending flip still has to show.
        if (video >= 0 && submitted) wait_flip(submitted, flip_timeout_us);
        if (video >= 0)
        {
            if (buffers >= 0) sceVideoOutUnregisterBuffers(video, buffers);
            sceVideoOutClose(video);
        }
        if (flip_queue_created) sceKernelDeleteEqueue(flip_queue);
        if (memory) sceKernelMunmap(memory, memory_bytes);
        if (offset >= 0) sceKernelReleaseDirectMemory(offset, memory_bytes);
    }
    bool open()
    {
        video = sceVideoOutOpen(ORBIS_VIDEO_USER_MAIN, ORBIS_VIDEO_OUT_BUS_MAIN, 0, nullptr);
        if (video < 0) return false;
        if (sceKernelAllocateDirectMemory(0, sceKernelGetDirectMemorySize(), memory_bytes, 0x200000, 3, &offset) < 0)
            return false;
        if (sceKernelMapDirectMemory(&memory, memory_bytes, 0x33, 0, offset, 0x200000) < 0)
            return false;
        void* addresses[] = {memory, static_cast<char*>(memory) + frame_bytes};
        OrbisVideoOutBufferAttribute attributes{};
        // Linear BGRA8 display buffers, matching OpenOrbis' software display.
        sceVideoOutSetBufferAttribute(&attributes, 0x80000000, 1, 0, width, height, width);
        buffers = sceVideoOutRegisterBuffers(video, 0, addresses, 2, &attributes);
        if (buffers < 0) return false;
        // Flip arguments continue from whatever the port reports already, so a flipArg left by an earlier
        // VideoOut user can never make a new flip look completed before it is.
        if (OrbisVideoOutFlipStatus status{}; sceVideoOutGetFlipStatus(video, &status) >= 0 && status.flipArg > 0 && status.flipArg < (int64_t{1} << 62))
            submitted = status.flipArg;
        back.assign(std::size_t(width) * height, 0xff000000u);
        // Flip completion events let present() sleep until the display is done with a buffer. Polling the flip
        // status (what Mesa's WSI does) stays the automatic fallback when the queue or the event is refused.
        if (sceKernelCreateEqueue(&flip_queue, "RPCS3 XMB flip") >= 0)
        {
            flip_queue_created = true;
            flip_events = sceVideoOutAddFlipEvent(flip_queue, video, nullptr) >= 0;
        }
        if (!load_font()) return false;
        ps4_initialize_pad();
        ps4_startup_checkpoint(fmt::format("launcher VideoOut and font ready (flip wait: %s)", flip_events ? "event queue" : "polling").c_str());
        return true;
    }
    // The firmware's font when dev_flash has it, else the bundled one. 0.33: called again after a firmware install; a
    // font that fails to load keeps the current one.
    // 0.39: the Arabic forms always come from the bundled DejaVu Sans (no PS3 firmware font has them); its bytes are read
    // per call and not kept (font::load packs them into the atlas).
    bool load_font()
    {
        static constexpr char dejavu[] = "/app0/fonts/DejaVuSans.ttf";
        std::vector<uint8_t> arabic;
        if (!read_whole(fs::file(dejavu), 8 * 1024 * 1024, arabic)) arabic.clear();
        for (const char* path : {"/data/rpcs3/dev_flash/data/font/SCE-PS3-RD-R-LATIN.TTF", dejavu})
        {
            std::vector<uint8_t> bytes;
            const bool bundled = std::string_view(path) == dejavu && !arabic.empty();
            if (!bundled && !read_whole(fs::file(path), 8 * 1024 * 1024, bytes)) continue;
            if (!typeface.load(bundled ? std::span<const uint8_t>(arabic) : std::span<const uint8_t>(bytes), arabic)) continue;
            ps4_startup_checkpoint(fmt::format("XMB font: '%s' (%s)%s", path, typeface.exact_sizes() ? "7 sizes" : "28 px scaled",
                typeface.arabic() ? " + Arabic from DejaVuSans (129 glyphs)" : arabic.empty() ? "; no Arabic: DejaVuSans unreadable" : "; no Arabic: the forms did not fit").c_str());
            return true;
        }
        return false;
    }
    uint32_t* pixels() { return back.data(); }
    uint32_t* scanout(int index) { return reinterpret_cast<uint32_t*>(static_cast<char*>(memory) + index * frame_bytes); }
    void text(float x, float y, const std::string& value, uint32_t color = 0xffeeeeee)
    {
        utils::ps4::xmb::canvas target(pixels(), width, height);
        typeface.paint(target, x, y, value, 28.f, color);
    }
    bool flip_done(int64_t arg) const
    {
        OrbisVideoOutFlipStatus status{};
        return sceVideoOutGetFlipStatus(video, &status) >= 0 && status.flipArg >= arg;
    }
    void disable_flip_events(const std::string& why)
    {
        flip_events = false;
        ps4_startup_checkpoint(fmt::format("XMB: %s; polling the flip status from now on", why).c_str());
    }
    // Waits until the flip with 'arg' has reached the screen, at most timeout_us.
    bool wait_flip(int64_t arg, u64 timeout_us)
    {
        const u64 start = sceKernelGetProcessTime();
        unsigned instant_events = 0; // events returned at once while this flip was still pending
        while (!flip_done(arg))
        {
            const u64 now = sceKernelGetProcessTime();
            const u64 waited = now - start;
            if (waited >= timeout_us) return false;
            if (!flip_events)
            {
                sceKernelUsleep(1000);
                continue;
            }
            OrbisKernelEvent event{};
            int32_t count = 0;
            const u64 slice = std::min(timeout_us - waited, flip_event_slice_us);
            OrbisKernelUseconds left = static_cast<OrbisKernelUseconds>(slice);
            const int32_t result = sceKernelWaitEqueue(flip_queue, &event, 1, &count, &left);
            const u64 spent = sceKernelGetProcessTime() - now;
            if (result >= 0 && count > 0)
            {
                flip_event_errors = 0;
                flip_event_misses = 0;
                // One stale event from an earlier flip is normal. An event that keeps firing at once (never cleared
                // by the wait) would turn this loop into a busy spin until the flip completes; poll instead.
                if (spent < 500 && ++instant_events >= 8) disable_flip_events("flip events fire without a new flip");
                continue;
            }
            if (spent + 500 < slice)
            {
                // Failed at once, so not a timeout. A few in a row mean the queue cannot be used.
                sceKernelUsleep(1000);
                if (++flip_event_errors >= 8) disable_flip_events(fmt::format("sceKernelWaitEqueue keeps failing (0x%x)", result));
            }
            else if (flip_done(arg) && ++flip_event_misses >= 8)
            {
                // The flip completed during a whole slice without waking the queue, several times in a row.
                disable_flip_events("flip events do not arrive");
            }
        }
        return true;
    }
    bool present()
    {
        const u64 start = sceKernelGetProcessTime();
        // The last submit put buffer 'current ^ 1' on screen; 'current' is free once that flip has completed.
        if (submitted && !wait_flip(submitted, flip_timeout_us)) return false;
        const u64 copy_start = sceKernelGetProcessTime();
        last_wait_us = copy_start - start;
        stream_copy(scanout(current), back.data(), back.size());
        last_copy_us = sceKernelGetProcessTime() - copy_start;
        if (sceVideoOutSubmitFlip(video, current, ORBIS_VIDEO_OUT_FLIP_VSYNC, submitted + 1) < 0) return false;
        ++submitted;
        current ^= 1;
        return true;
    }
};

// 0.31: menu input from every seated controller through ps4_pads (the only owner of the pad handles). Each seat is
// edge-detected over all samples buffered since the previous frame, so a tap shorter than a frame is never lost
// and one player holding a button cannot hide another player's press. Held directions repeat after 300 ms, then
// every 100 ms; in the game list Up/Down speed up to every 60 ms.
struct launcher_input
{
    std::array<uint32_t, ps4_pads::max_players> seat{};
    std::vector<OrbisPadData> samples = std::vector<OrbisPadData>(ORBIS_PAD_MAX_DATA_NUM);
    uint32_t buttons = 0; // buttons held on any seat
    uint32_t held_direction = 0;
    u64 repeat_at = 0;
    u32 repeats = 0;
    bool ready = false;
    bool drop_edges = false;

    // The next poll only updates the held state: presses buffered while a system dialog or the home screen had
    // the controllers are not replayed into the menu.
    void flush() { drop_edges = true; }

    uint32_t poll(u64 now, bool fast_list)
    {
        if (!ready) ready = ps4_pads::initialize(); // retries are throttled inside ps4_pads
        ps4_pads::refresh(/*compact=*/true);       // only while no game runs (XMB, installer)
        uint32_t pressed = 0, held = 0;
        for (unsigned slot = 0; slot < ps4_pads::max_players; ++slot)
        {
            const int count = ps4_pads::read_samples(slot, samples.data(), static_cast<int>(samples.size()));
            if (count < 0) seat[slot] = 0; // no pad in this seat
            for (int i = 0; i < count; ++i)
            {
                const uint32_t state = samples[i].connected ? samples[i].buttons : 0;
                pressed |= state & ~seat[slot];
                seat[slot] = state;
            }
            held |= seat[slot];
        }
        buttons = held;
        if (std::exchange(drop_edges, false)) pressed = 0;
        constexpr uint32_t directions = ORBIS_PAD_BUTTON_UP | ORBIS_PAD_BUTTON_DOWN | ORBIS_PAD_BUTTON_LEFT | ORBIS_PAD_BUTTON_RIGHT;
        const uint32_t direction = held & directions;
        if (direction != held_direction)
        {
            held_direction = direction;
            repeat_at = now + 300'000;
            repeats = 0;
        }
        else if (direction && now >= repeat_at)
        {
            pressed |= direction;
            const bool vertical = !(direction & (ORBIS_PAD_BUTTON_LEFT | ORBIS_PAD_BUTTON_RIGHT));
            repeat_at = now + (fast_list && vertical && ++repeats > 3 ? 60'000 : 100'000);
        }
        return pressed;
    }
};

// 0.38: the install, disc and firmware screens are PS3 system dialogs (util/ps4_xmb_dialog.hpp) over the XMB waves,
// redrawn about 30 times a second while one waits (the busy clock and the waves move). A dialog fades in over 150 ms.
constexpr u64 dialog_fade_us = 150'000;
constexpr u32 dialog_frame_us = 33'000;

void draw_dialog_frame(launcher_display& display, utils::ps4::xmb::scene& backdrop, const xmb_dialog& d, double seconds, u64 shown_us)
{
    backdrop.backdrop(display.pixels(), seconds);
    utils::ps4::xmb::canvas target(display.pixels(), display.width, display.height);
    utils::ps4::xmb::draw_dialog(target, display.typeface, backdrop.resources, d, seconds,
        shown_us >= dialog_fade_us ? 255u : static_cast<unsigned>(shown_us * 255 / dialog_fade_us));
}

// An XMB line "part · part · ..." as a dialog: the first part is the message, the others its notes
void split_into_dialog(std::string_view text, xmb_dialog& d)
{
    for (bool first = true; !text.empty(); first = false)
    {
        const std::size_t cut = text.find(" · ");
        std::string part(text.substr(0, cut));
        text = cut == std::string_view::npos ? std::string_view{} : text.substr(cut + std::string_view(" · ").size());
        if (part.empty()) continue;
        // A note is a sentence of its own (the first part may start with a file name: left as it is)
        if (!first && part.front() >= 'a' && part.front() <= 'z') part.front() = static_cast<char>(part.front() - 'a' + 'A');
        (first ? d.lines : d.notes).push_back(std::move(part));
    }
}

// 0.34: idle tiers of the XMB. The CPU draws every frame (~6 ms at 1.6 GHz), so a menu left alone kept a fifth of a
// core busy. After 10 s without input it is drawn at 30 fps, after 60 s at 10 fps and after 5 min at 2 fps (the clock
// still moves); any input returns to the full rate (every vblank) at once.
constexpr std::array<u64, 4> xmb_idle_after_us{0, 10'000'000, 60'000'000, 300'000'000};
constexpr std::array<u64, 4> xmb_idle_interval_us{0, 33'333, 100'000, 500'000};
constexpr std::array<const char*, 4> xmb_idle_names{"full rate", "30 fps", "10 fps", "2 fps"};

unsigned xmb_idle_tier(u64 inactive_us)
{
    unsigned tier = 0;
    while (tier + 1 < xmb_idle_after_us.size() && inactive_us >= xmb_idle_after_us[tier + 1]) ++tier;
    return tier;
}

// 0.31: 'XMB perf' line: frame rate, draw / copy / flip-wait split and slow frames, every 30 s and when the XMB
// closes. Intervals paced by an idle tier and pauses in the background are kept apart.
// 0.34: idle frames are counted per tier, and the line names the tier of the last frame.
struct xmb_perf
{
    u64 since = 0, paused_us = 0, busy_us = 0, idle_us = 0;
    u64 draw_us = 0, draw_max_us = 0, copy_us = 0, wait_us = 0;
    u32 frames = 0, busy_frames = 0, idle_frames = 0, slow = 0, dropped = 0;
    std::array<u32, 4> tier_frames{};
    unsigned tier = 0;

    // 'idle_tier': the idle tier that paced this interval (0 = full rate).
    void frame(u64 draw, u64 copy, u64 wait, u64 interval, unsigned idle_tier)
    {
        ++frames;
        draw_us += draw;
        draw_max_us = std::max(draw_max_us, draw);
        copy_us += copy;
        wait_us += wait;
        tier = std::min<unsigned>(idle_tier, tier_frames.size() - 1);
        if (!interval) return; // first frame after opening or resuming
        if (tier)
        {
            ++idle_frames;
            ++tier_frames[tier];
            idle_us += interval;
            return;
        }
        ++busy_frames;
        busy_us += interval;
        if (interval > 17'000) ++slow;
        if (interval > 25'000) ++dropped;
    }
    void report(u64 now, const char* when)
    {
        const double n = frames ? frames : 1;
        const double fps = busy_us ? busy_frames * 1e6 / double(busy_us) : 0;
        const double idle_fps = idle_us ? idle_frames * 1e6 / double(idle_us) : 0;
        ps4_startup_checkpoint(fmt::format("XMB perf (%s): %u frames in %.1f s (%.1f fps; %u idle frames at %.1f fps; idle tiers 30/10/2 fps: "
            "%u/%u/%u frames, tier now %s; paused %.1f s); draw avg %.1f max %.1f ms; copy avg %.1f ms; flip wait avg %.1f ms; "
            "%u frames > 17 ms, %u > 25 ms",
            when, frames, double(now - since) / 1e6, fps, idle_frames, idle_fps, tier_frames[1], tier_frames[2], tier_frames[3],
            xmb_idle_names[tier], double(paused_us) / 1e6, draw_us / n / 1000, double(draw_max_us) / 1000, copy_us / n / 1000,
            wait_us / n / 1000, slow, dropped).c_str());
        *this = {};
        since = now;
    }
};

// Library folders on the console's own storage. USB drives are listed per drive (list_games).
// 0.34: every folder of /data/rpcs3 with a library name in any case (games, PS3ISO, pkg, packages...), two levels
// deep; dev_hdd0/game one level and folders only (its data folders and the files in them are not games).
// 0.37: 'library_roots' receives those folders of /data/rpcs3 (not dev_hdd0/game) for the change watch.
std::vector<std::string> list_internal_games(std::vector<std::string>* library_roots = nullptr)
{
    std::vector<utils::ps4::library_root> roots;
    if (fs::dir data{"/data/rpcs3"})
        for (const auto& item : data)
            if (item.is_directory && !item.is_symlink && utils::ps4::internal_library_folder(item.name)) roots.push_back({"/data/rpcs3/" + item.name});
    if (roots.empty()) roots = {{"/data/rpcs3/games"}, {"/data/rpcs3/pkgs"}}; // the listing failed: the folders the app creates
    std::sort(roots.begin(), roots.end(), [](const auto& a, const auto& b) { return a.path < b.path; });
    if (library_roots)
        for (const auto& root : roots) library_roots->push_back(root.path);
    roots.push_back({"/data/rpcs3/dev_hdd0/game", 1, utils::ps4::accept_folder});
    return utils::ps4::scan_game_library_roots(roots,
        {}, [](const std::string& root)
        {
            std::vector<utils::ps4::library_item> entries;
            fs::dir directory(root);
            if (directory) for (const auto& item : directory)
                entries.push_back({item.name, item.is_directory, item.is_symlink});
            ps4_launcher_log.notice("Library scan '%s': opened=%d, entries=%u", root, bool(directory), entries.size());
            return entries;
        }, [](const std::string& path) { return fs::is_file(path); });
}

// 0.33: what is mounted at /mnt/usb0 .. /mnt/usb7, one key per drive ("" when none). Two stat calls per drive and no
// disk access (the root of a mounted file system stays cached), cheap enough to compare every 2 s. A mount point left
// behind without a drive is a plain folder of /mnt: same st_dev as /mnt and nothing in it. The key is the device and
// inode of the root only: exFAT and FAT32 keep no timestamps for the root directory, so a synthesized one could change
// between two polls and trigger a rescan every 2 s. Every XMB visit reads all drives again anyway.
using usb_keys = std::array<std::string, utils::ps4::usb_drive_count>;

bool has_entries(const std::string& path)
{
    fs::dir directory(path);
    fs::dir_entry entry;
    while (directory && directory.read(entry))
        if (entry.name != "." && entry.name != "..") return true;
    return false;
}

usb_keys probe_usb_drives()
{
    usb_keys keys;
    struct ::stat parent{};
    const bool parent_known = ::stat("/mnt", &parent) == 0;
    for (unsigned i = 0; i < keys.size(); ++i)
    {
        const std::string mount = utils::ps4::usb_mount(i);
        struct ::stat info{};
        if (::stat(mount.c_str(), &info) != 0 || !S_ISDIR(info.st_mode)) continue;
        // Another device than /mnt is a mounted drive, even an empty one. Otherwise only a folder with something in it
        // counts: an empty one is a mount point left behind.
        if ((!parent_known || info.st_dev == parent.st_dev) && !has_entries(mount)) continue;
        keys[i] = fmt::format("%llx:%llx", static_cast<unsigned long long>(info.st_dev), static_cast<unsigned long long>(info.st_ino));
    }
    return keys;
}

// 0.33: a drive formatted as PS4 extended storage is mounted at /mnt/ext0 (another device than /mnt), never at
// /mnt/usbN. Its contents are encrypted by the console, so it only explains why no drive is listed.
bool extended_storage_mounted()
{
    struct ::stat parent{}, info{};
    return ::stat("/mnt", &parent) == 0 && ::stat("/mnt/ext0", &info) == 0 && S_ISDIR(info.st_mode) && info.st_dev != parent.st_dev;
}

bool inflate_xmb(std::span<uint8_t> output, std::span<const uint8_t> input)
{
    uLongf length = output.size();
    return uncompress(output.data(), &length, input.data(), input.size()) == Z_OK && length == output.size();
}

// 0.31: XMB caches under /data/rpcs3/cache/xmb/ (the cache size limit only trims hdd1/caches). Every file starts
// with a magic and a version and records the size and mtime of the file it was made from; a file that fails any
// check is ignored and rebuilt. Writes go to a temporary file that is renamed over the old one, so a crash or a
// power loss leaves the old file or none, never a torn one.
namespace xmb_cache
{
constexpr char icons[] = "/data/rpcs3/cache/xmb/icons";
constexpr char library_path[] = "/data/rpcs3/cache/xmb/library-v1.tsv";
constexpr char rco_path[] = "/data/rpcs3/cache/xmb/rco-v1.bin";
constexpr char library_header[] = "RPCS3 XMB library v1";
constexpr u32 icon_magic = 0x31434958; // "XIC1"
constexpr u32 rco_magic = 0x31435258;  // "XRC1"

u64 fnv1a64(std::string_view text)
{
    u64 hash = 0xcbf29ce484222325ull;
    for (const char c : text)
    {
        hash ^= static_cast<unsigned char>(c);
        hash *= 0x100000001b3ull;
    }
    return hash;
}

// Little-endian fields, as the console writes them.
struct writer
{
    std::vector<uint8_t> bytes;
    void u32v(u32 value) { for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<uint8_t>(value >> (8 * i))); }
    void u64v(u64 value) { for (int i = 0; i < 8; ++i) bytes.push_back(static_cast<uint8_t>(value >> (8 * i))); }
    void raw(const void* data, std::size_t size)
    {
        if (!size) return;
        const auto* p = static_cast<const uint8_t*>(data);
        bytes.insert(bytes.end(), p, p + size);
    }
    void text(const std::string& value)
    {
        u32v(static_cast<u32>(value.size()));
        raw(value.data(), value.size());
    }
    void picture(const xmb_image& image)
    {
        const bool valid = static_cast<bool>(image);
        u32v(valid ? static_cast<u32>(image.width) : 0);
        u32v(valid ? static_cast<u32>(image.height) : 0);
        if (valid) raw(image.pixels.data(), image.pixels.size() * sizeof(uint32_t));
    }
};

// Bounds-checked reader: any field past the end or over its limit marks the whole file as bad.
struct reader
{
    std::span<const uint8_t> bytes;
    std::size_t at = 0;
    bool ok = true;

    bool take(std::size_t size)
    {
        if (!ok || size > bytes.size() - at) ok = false;
        return ok;
    }
    u32 u32v()
    {
        if (!take(4)) return 0;
        u32 value = 0;
        for (int i = 0; i < 4; ++i) value |= u32(bytes[at + i]) << (8 * i);
        at += 4;
        return value;
    }
    u64 u64v()
    {
        if (!take(8)) return 0;
        u64 value = 0;
        for (int i = 0; i < 8; ++i) value |= u64(bytes[at + i]) << (8 * i);
        at += 8;
        return value;
    }
    std::string text(std::size_t limit)
    {
        const u32 size = u32v();
        if (size > limit) ok = false;
        if (!take(size)) return {};
        std::string value(reinterpret_cast<const char*>(bytes.data() + at), size);
        at += size;
        return value;
    }
    // 0 x 0 is a valid "no picture" record.
    bool picture(xmb_image& image, u32 max_side)
    {
        const u32 w = u32v(), h = u32v();
        if (!ok || w > max_side || h > max_side || (w == 0) != (h == 0)) return ok = false;
        const std::size_t count = std::size_t(w) * h;
        if (!take(count * sizeof(uint32_t))) return false;
        image.width = static_cast<int>(w);
        image.height = static_cast<int>(h);
        image.pixels.resize(count);
        if (count) std::memcpy(image.pixels.data(), bytes.data() + at, count * sizeof(uint32_t));
        at += count * sizeof(uint32_t);
        return true;
    }
    bool finished() const { return ok && at == bytes.size(); }
};

bool write_file(const std::string& path, const void* data, std::size_t size)
{
    fs::pending_file file(path);
    if (!file.file) return false;
    if (size && file.file.write(data, size) != size) return false;
    return file.commit();
}

std::string icon_file(const std::string& source)
{
    return fmt::format("%s/%016llx.bin", icons, fnv1a64(source));
}

// Thumbnail made from 'source' while it had this size and mtime (0 x 0 when it has no usable icon).
bool read_icon(const std::string& source, const fs::stat_t& stat, xmb_image& out)
{
    std::vector<uint8_t> bytes;
    if (!read_whole(fs::file(icon_file(source)), 1 << 20, bytes)) return false;
    reader in{bytes};
    if (in.u32v() != icon_magic || in.u64v() != stat.size || static_cast<s64>(in.u64v()) != stat.mtime || in.text(4096) != source)
        return false;
    xmb_image image;
    if (!in.picture(image, 256) || !in.finished()) return false;
    out = std::move(image);
    return true;
}

bool write_icon(const std::string& source, const fs::stat_t& stat, const xmb_image& image)
{
    writer out;
    out.u32v(icon_magic);
    out.u64v(stat.size);
    out.u64v(static_cast<u64>(stat.mtime));
    out.text(source);
    out.picture(image);
    return write_file(icon_file(source), out.bytes.data(), out.bytes.size());
}

bool read_rco(u64 key, std::map<std::string, xmb_image, std::less<>>& out)
{
    std::vector<uint8_t> bytes;
    if (!read_whole(fs::file(rco_path), 64 << 20, bytes)) return false;
    reader in{bytes};
    if (in.u32v() != rco_magic || in.u64v() != key) return false;
    const u32 count = in.u32v();
    if (!in.ok || !count || count > 64) return false;
    std::map<std::string, xmb_image, std::less<>> images;
    for (u32 i = 0; i < count; ++i)
    {
        std::string name = in.text(64);
        xmb_image image;
        if (!in.picture(image, 2048) || name.empty() || !image) return false;
        images.insert_or_assign(std::move(name), std::move(image));
    }
    if (!in.finished()) return false;
    out = std::move(images);
    return true;
}

bool write_rco(u64 key, const std::map<std::string, xmb_image, std::less<>>& images)
{
    writer out;
    out.u32v(rco_magic);
    out.u64v(key);
    u32 count = 0;
    for (const auto& [name, image] : images) count += !name.empty() && name.size() <= 64 && image && image.width <= 2048 && image.height <= 2048;
    out.u32v(count);
    for (const auto& [name, image] : images)
    {
        if (name.empty() || name.size() > 64 || !image || image.width > 2048 || image.height > 2048) continue;
        out.text(name);
        out.picture(image);
    }
    return count && write_file(rco_path, out.bytes.data(), out.bytes.size());
}
} // namespace xmb_cache

// 0.33: the per-game settings database of the PC build (GuiConfigs/config_database.dat, JSON). The boot reads it from
// /data/rpcs3/config_database.dat. It is never downloaded or bundled: the user brings it from a PC install, by USB or
// by FTP into /data/rpcs3/import. Same flow as the patch import: checked first, the previous file kept as .bak, the
// new one written through a temporary file.
namespace config_db
{
constexpr char target[] = "/data/rpcs3/config_database.dat";
constexpr char import_copy[] = "/data/rpcs3/import/config_database.dat";
constexpr u64 file_limit = utils::ps4::config_db::max_size; // the boot's limit; the PC file is ~300 KB for ~2200 titles

struct summary
{
    u32 titles = 0;
    s64 timestamp = -1; // database date, seconds since 1970; -1 when the file has none
};

// Integer value of a top-level "key" of the JSON text.
std::optional<s64> json_integer(std::string_view text, std::string_view key)
{
    const std::string quoted = "\"" + std::string(key) + "\"";
    std::size_t at = text.find(quoted);
    if (at == std::string_view::npos) return std::nullopt;
    at = text.find_first_not_of(" \t\r\n", at + quoted.size());
    if (at == std::string_view::npos || text[at] != ':') return std::nullopt;
    at = text.find_first_not_of(" \t\r\n", at + 1);
    if (at == std::string_view::npos) return std::nullopt;
    s64 value = 0;
    if (std::from_chars(text.data() + at, text.data() + text.size(), value).ec != std::errc{}) return std::nullopt;
    return value;
}

// Checked with the reader the boot uses (util/ps4_config_db.hpp, which mirrors rpcs3qt/config_database.cpp): a file
// the boot would reject is never imported. 'titles' counts the usable entries.
bool check(std::string_view text, summary& out, std::string& error)
{
    utils::ps4::config_db::detail::game_map games;
    if (!utils::ps4::config_db::detail::parse(text, games, error)) return false;
    out = {};
    out.titles = static_cast<u32>(games.size());
    out.timestamp = json_integer(text, "timestamp").value_or(-1);
    if (!out.titles) error = "no game with a 'config' entry";
    return out.titles > 0;
}

// dd/mm/yyyy (UTC) of a database timestamp. Plain calendar arithmetic: gmtime would share its static buffer with the
// UI thread's localtime.
std::string date_text(s64 seconds)
{
    if (seconds < 0) return {};
    const s64 z = seconds / 86400 + 719468;
    const s64 era = z / 146097;
    const s64 doe = z - era * 146097;
    const s64 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const s64 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const s64 mp = (5 * doy + 2) / 153;
    const s64 day = doy - (153 * mp + 2) / 5 + 1;
    const s64 month = mp < 10 ? mp + 3 : mp - 9;
    const s64 year = yoe + era * 400 + (month <= 2 ? 1 : 0);
    return fmt::format("%02d/%02d/%04d", static_cast<int>(day), static_cast<int>(month), static_cast<int>(year));
}

std::string row_help()
{
    return i18n::tr(fs::is_file(target) ? str::LAU_CONFIGDB_INSTALADO : str::LAU_CONFIGDB_AJUSTES_POR_JOGO);
}

// Places a PC install leaves the file in when copied to a drive, on every USB port, then the FTP import folder.
std::vector<std::string> sources()
{
    std::vector<std::string> found;
    for (unsigned i = 0; i < utils::ps4::usb_drive_count; ++i)
    {
        const std::string mount = utils::ps4::usb_mount(i);
        if (!fs::is_dir(mount)) continue;
        for (const char* name : {"/rpcs3/config_database.dat", "/config_database.dat", "/GuiConfigs/config_database.dat", "/rpcs3/GuiConfigs/config_database.dat"})
            if (fs::is_file(mount + name)) found.push_back(mount + name);
    }
    if (fs::is_file(import_copy)) found.emplace_back(import_copy);
    return found;
}

// Imports the newest valid copy found (by its own timestamp). Returns the XMB line; every step writes a
// 'config database:' startup.log line. Sources are only read.
std::string import()
{
    std::string best_path;
    std::vector<uint8_t> best_bytes;
    summary best{};
    u32 invalid = 0;
    for (const auto& source : sources())
    {
        std::vector<uint8_t> bytes;
        summary info;
        std::string error = fmt::format("unreadable or larger than %u MiB", file_limit >> 20);
        if (!read_whole(fs::file(source), file_limit, bytes) || !check({reinterpret_cast<const char*>(bytes.data()), bytes.size()}, info, error))
        {
            ++invalid;
            ps4_startup_checkpoint(fmt::format("config database: import: '%s' is not a usable RPCS3 config_database.dat: %s", source, error).c_str());
            continue;
        }
        ps4_startup_checkpoint(fmt::format("config database: import: '%s': %u title(s), timestamp %lld", source, info.titles, info.timestamp).c_str());
        if (best_path.empty() || info.timestamp > best.timestamp)
        {
            best_path = source;
            best_bytes = std::move(bytes);
            best = info;
        }
    }
    if (best_path.empty())
        return invalid ? i18n::trf(str::LAU_CONFIGDB_INVALIDOS, {invalid}) : std::string(i18n::tr(str::LAU_CONFIGDB_NENHUM));
    const std::string date = date_text(best.timestamp);
    const std::string details = date.empty() ? i18n::trf(str::LAU_CONFIGDB_JOGOS, {best.titles}) : i18n::trf(str::LAU_CONFIGDB_JOGOS_DATA, {best.titles, date});
    if (std::vector<uint8_t> current; read_whole(fs::file(target), file_limit, current) && current == best_bytes)
    {
        ps4_startup_checkpoint(fmt::format("config database: import: '%s' is identical to '%s'", best_path, target).c_str());
        return i18n::trf(str::LAU_CONFIGDB_JA_ESTA, {i18n::raw(details)});
    }
    const std::string backup = std::string(target) + ".bak";
    if (fs::is_file(target) && !fs::copy_file(target, backup, true))
    {
        ps4_startup_checkpoint(fmt::format("config database: import: cannot back up '%s': %s", target, fs::g_tls_error).c_str());
        return i18n::tr(str::LAU_CONFIGDB_ERRO_BACKUP);
    }
    if (!xmb_cache::write_file(target, best_bytes.data(), best_bytes.size()))
    {
        ps4_startup_checkpoint(fmt::format("config database: import: cannot write '%s': %s", target, fs::g_tls_error).c_str());
        return i18n::tr(str::LAU_CONFIGDB_ERRO_GRAVAR);
    }
    // The boot's cached copy is keyed by size and mtime (seconds): a same-size file written within the same second
    // would otherwise keep the old entries until the process restarts.
    utils::ps4::config_db::forget();
    ps4_startup_checkpoint(fmt::format("config database: imported '%s' -> '%s' (%u titles, %u bytes, timestamp %lld)", best_path, target,
        best.titles, best_bytes.size(), best.timestamp).c_str());
    return utils::ps4::patches::clip(invalid ? i18n::trf(str::LAU_CONFIGDB_IMPORTADO_INVALIDOS, {i18n::raw(details), invalid})
                                             : i18n::trf(str::LAU_CONFIGDB_IMPORTADO, {i18n::raw(details)}), line_bytes(110));
}
} // namespace config_db

// 0.39: an ISO row is an ISO, never a disc; only the image a disc install wrote (disc_lock) says it needs the disc
inline const char* iso_row_label(i18n::lang l = i18n::current()) { return i18n::tr(str::LAU_ISO_DE_PS3, l); }
constexpr std::string_view iso_row_label_038 = "Disco PS3 · ISO"; // the same rows in a pt-BR library cache written before 0.39

// What the library scan learns about one game path; the library cache stores exactly this.
struct game_info
{
    std::string path;
    bool keyed = false; // size and mtime of the file the description was read from are known
    u64 size = 0;
    s64 mtime = 0;
    std::string title, subtitle, icon_name, serial, category, app_ver;
    bool operator==(const game_info&) const = default;
};

// 0.31: last library scan, one tab-separated line per game, in scan order. The XMB shows it at once when it opens
// and the loader then re-checks every entry against the size and mtime of its source file.
struct library_cache
{
    std::vector<game_info> games;
    std::map<std::string, std::size_t, std::less<>> index;
    // 0.39: interface language the subtitles were written in. The header line names it (pt-BR keeps the 0.38 header);
    // a scan in another language describes every game again.
    i18n::lang language = i18n::lang::pt_br;

    static std::string header(i18n::lang l)
    {
        std::string text = xmb_cache::library_header;
        if (l != i18n::lang::pt_br) text += std::string(" ") + i18n::lang_token(l);
        return text;
    }

    static bool storable(const game_info& game)
    {
        for (const std::string* field : {&game.path, &game.title, &game.subtitle, &game.icon_name, &game.serial, &game.category, &game.app_ver})
            if (field->find_first_of("\t\r\n") != std::string::npos) return false;
        return !game.path.empty() && game.path.front() == '/';
    }
    static bool parse(std::string_view line, game_info& out)
    {
        std::array<std::string_view, 10> field{};
        std::size_t count = 0, start = 0;
        for (;;)
        {
            if (count == field.size()) return false;
            const std::size_t tab = line.find('\t', start);
            field[count++] = line.substr(start, tab == std::string_view::npos ? std::string_view::npos : tab - start);
            if (tab == std::string_view::npos) break;
            start = tab + 1;
        }
        if (count != field.size() || field[0].empty() || field[0].front() != '/' || (field[1] != "0" && field[1] != "1")) return false;
        const auto number = [](std::string_view text, auto& value)
        {
            const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
            return !text.empty() && result.ec == std::errc{} && result.ptr == text.data() + text.size();
        };
        if (!number(field[2], out.size) || !number(field[3], out.mtime)) return false;
        out.path = field[0];
        out.keyed = field[1] == "1";
        out.title = field[4];
        out.subtitle = field[5];
        if (out.subtitle.starts_with(iso_row_label_038)) out.subtitle.replace(0, iso_row_label_038.size(), iso_row_label(i18n::lang::pt_br));
        out.icon_name = field[6];
        out.serial = field[7];
        out.category = field[8];
        out.app_ver = field[9];
        return true;
    }
    void reindex()
    {
        index.clear();
        for (std::size_t i = 0; i < games.size(); ++i) index.emplace(games[i].path, i);
    }
    // False when the file is missing, has another header or holds no game.
    bool load()
    {
        games.clear();
        index.clear();
        language = i18n::lang::pt_br;
        std::vector<uint8_t> bytes;
        if (!read_whole(fs::file(xmb_cache::library_path), 4 << 20, bytes)) return false;
        std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        bool header = true;
        while (!text.empty())
        {
            const std::size_t end = text.find('\n');
            std::string_view line = text.substr(0, end);
            text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
            if (std::exchange(header, false))
            {
                bool known = false;
                for (const i18n::lang l : {i18n::lang::pt_br, i18n::lang::en, i18n::lang::es, i18n::lang::ar})
                    if (line == library_cache::header(l))
                    {
                        language = l;
                        known = true;
                    }
                if (!known) return false;
                continue;
            }
            game_info game;
            if (line.empty() || !parse(line, game) || index.contains(game.path)) continue;
            index.emplace(game.path, games.size());
            games.push_back(std::move(game));
        }
        return !games.empty();
    }
    const game_info* find(const std::string& path) const
    {
        const auto it = index.find(path);
        return it == index.end() ? nullptr : &games[it->second];
    }
    // Rewrites the file only when the scan differs from it. 'scanned_in': the interface language of the scan.
    bool store(const std::vector<game_info>& scanned, bool writable, i18n::lang scanned_in)
    {
        std::vector<game_info> kept;
        kept.reserve(scanned.size());
        for (const auto& game : scanned) if (storable(game)) kept.push_back(game);
        if (kept == games && scanned_in == language) return true;
        games = std::move(kept);
        language = scanned_in;
        reindex();
        if (!writable) return false;
        std::string text = header(language);
        text += '\n';
        for (const auto& game : games)
        {
            text += game.path;
            text += '\t';
            text += game.keyed ? '1' : '0';
            text += '\t';
            text += std::to_string(game.size);
            text += '\t';
            text += std::to_string(game.mtime);
            for (const std::string* field : {&game.title, &game.subtitle, &game.icon_name, &game.serial, &game.category, &game.app_ver})
            {
                text += '\t';
                text += *field;
            }
            text += '\n';
        }
        return xmb_cache::write_file(xmb_cache::library_path, text.data(), text.size());
    }
};

// 0.33: one USB drive as the last library scan of this XMB saw it.
struct usb_drive
{
    std::string key;              // probe key the listing below was read with; "" = no drive
    bool scanned = false;
    bool readable = false;        // the root folder could be listed
    bool truncated = false;       // the entry budget ran out (very large drive): the listing is partial
    // PS4 packages among 'games'. They stay in the listing so the library cache records them (category @PS4PKG) and the
    // next visit needs no read of them, but the game column hides them and the counts below leave them out.
    std::size_t ps4_packages = 0;
    std::vector<std::string> games, folders;

    // What the game column lists from this drive.
    std::size_t shown_games() const { return games.size() - std::min(ps4_packages, games.size()); }
};

enum class usb_state : u8 { absent, games, empty, no_layout, unreadable };

usb_state state_of(const usb_drive& drive)
{
    if (drive.key.empty()) return usb_state::absent;
    if (!drive.readable) return usb_state::unreadable;
    if (drive.shown_games()) return usb_state::games;
    if (!drive.folders.empty()) return usb_state::empty;
    // A freshly formatted drive has an empty root; the probe already left out empty mount points without a drive. A
    // drive pulled out between the probe and the scan is dropped by the next poll.
    return usb_state::no_layout;
}

// State of the loader jobs. Everything except 'stopping' is touched only by the code that runs the jobs: the loader
// thread, or the UI thread when the loader runs inline.
struct load_context
{
    std::atomic<bool> stopping{false}; // set by the UI before it joins the loader
    std::string preferred;
    library_cache library;
    utils::ps4::patches::catalog patch_catalog; // 0.33: patch files parsed once, re-read when they change
    std::array<usb_drive, utils::ps4::usb_drive_count> usb{}; // 0.33: listing per drive, reused while its key holds
    bool usb_known = false; // a library scan of this XMB already saw the drives (later changes are announced)
    bool dirs_tried = false, dirs_ready = false;
    // 0.37: PKGs on /data that the last scan found incomplete (still copying, stopped, part missing) and their state
    // then; the 2 s poll re-checks them so a finished upload refreshes its row
    std::map<std::string, utils::ps4::pkg::state, std::less<>> waiting_packages;
    // 0.37: library folders of /data/rpcs3 and their mtime at the last scan (an FTP upload creating a file changes it)
    std::vector<std::pair<std::string, s64>> watched_roots;

    // Creates the cache folders once; when /data refuses, the caches are only read and never written.
    bool cache_ready()
    {
        if (!std::exchange(dirs_tried, true))
        {
            dirs_ready = fs::create_path(xmb_cache::icons);
            if (!dirs_ready) ps4_launcher_log.error("XMB cache folder '%s' is not available: %s", xmb_cache::icons, fs::g_tls_error);
        }
        return dirs_ready;
    }
};

constexpr std::array<const char*, 3> rco_files{"xmb_ingame.rco", "explore_plugin_full.rco", "system_plugin.rco"};
constexpr char rco_folder[] = "/data/rpcs3/dev_flash/vsh/resource/";

// 0.37: the padlock of a game installed from a PS3 disc whose disc is not in the drive: the firmware's own
// (tex_lock_icon of explore_plugin_full.rco), else an original one drawn once per process. The drawn one is added after
// the firmware cache is written, so a later firmware install brings the real one.
void add_padlock(std::map<std::string, xmb_image, std::less<>>& resources)
{
    const bool firmware = resources.contains("tex_lock_icon");
    if (!firmware)
    {
        static const xmb_image drawn = utils::ps4::xmb::padlock_icon();
        resources.emplace("tex_lock_icon", drawn);
    }
    static int logged = -1;
    if (std::exchange(logged, firmware ? 1 : 0) != (firmware ? 1 : 0))
        ps4_startup_checkpoint(firmware ? "disc lock: padlock icon: tex_lock_icon from the firmware (explore_plugin_full.rco)"
                                        : "disc lock: padlock icon: firmware image missing; the drawn padlock is used");
}

// 0.38: the system dialog images (util/ps4_xmb_dialog.hpp: system_plugin.rco) the firmware lacks are drawn once per
// process, also after the cache is written, so a later firmware install brings the real ones.
void add_dialog_images(std::map<std::string, xmb_image, std::less<>>& resources)
{
    std::string missing;
    for (const char* name : utils::ps4::xmb::dialog_images)
        if (!resources.contains(name)) missing += std::string(missing.empty() ? "" : ", ") + name;
    if (const auto square = resources.find("tex_rectangle"); square != resources.end() && !resources.contains("tex_square"))
        resources.emplace("tex_square", square->second); // the XMB guide's name for the firmware's square glyph
    static std::map<std::string, xmb_image, std::less<>> drawn;
    if (!missing.empty() && drawn.empty()) utils::ps4::xmb::add_dialog_fallbacks(drawn);
    if (!missing.empty())
        for (const auto& [name, image] : drawn) resources.try_emplace(name, image);
    static std::string logged = "?";
    if (std::exchange(logged, missing) != missing)
        ps4_startup_checkpoint(missing.empty() ? "XMB dialogs: firmware images (system_plugin.rco)"
                                               : ("XMB dialogs: drawn stand-ins for " + missing).c_str());
}

std::map<std::string, xmb_image, std::less<>> load_xmb_resources(load_context& context, bool& from_cache)
{
    // 0.38: tex_rectangle and the tex_msgdialog/progress/busy images for the system dialogs (tex_square is not a
    // firmware name: add_dialog_images maps it to tex_rectangle)
    const std::vector<std::string> names{"tex_user", "tex_sysconf", "tex_photo", "tex_music", "tex_video", "tex_tv",
        "tex_game", "tex_network", "tex_psn", "tex_friend", "item_tex_ps3util", "item_tex_plain_folder",
        "item_tex_ps3game_folder", "item_tex_disc_install", "tex_infobar", "tex_cross", "tex_circle", "tex_triangle", "tex_square",
        "tex_lock_icon", "tex_rectangle", "tex_msgdialog_bg", "tex_msgdialog_bar", "tex_default_progress_base", "tex_default_progress_slider",
        "tex_busy"};
    // The cache key covers the wanted names and the size and mtime of the three firmware files.
    std::string key_text;
    for (const auto& name : names)
    {
        key_text += name;
        key_text += ',';
    }
    for (const char* file : rco_files)
    {
        fs::stat_t stat{};
        const bool present = fs::get_stat(std::string(rco_folder) + file, stat) && !stat.is_directory;
        key_text += fmt::format("%s:%d:%llu:%lld;", file, present ? 1 : 0, present ? stat.size : 0, present ? stat.mtime : 0);
    }
    const u64 key = xmb_cache::fnv1a64(key_text);
    std::map<std::string, xmb_image, std::less<>> resources;
    if (xmb_cache::read_rco(key, resources))
    {
        from_cache = true;
        ps4_startup_checkpoint(fmt::format("XMB: %u firmware images loaded (cache)", resources.size()).c_str());
        add_padlock(resources);
        add_dialog_images(resources);
        return resources;
    }
    for (const char* name : rco_files)
    {
        std::vector<uint8_t> bytes;
        if (!read_whole(fs::file(std::string(rco_folder) + name), 32 * 1024 * 1024, bytes)) continue;
        auto images = utils::ps4::xmb::read_rco_images(bytes, names, inflate_xmb);
        // The full explorer contains the small category artwork used by the
        // standard menu; retain the in-game versions for missing categories.
        for (auto& [image_name, img] : images) resources.insert_or_assign(image_name, std::move(img));
    }
    if (!resources.empty() && context.cache_ready()) xmb_cache::write_rco(key, resources);
    ps4_startup_checkpoint(fmt::format("XMB: %u firmware images loaded", resources.size()).c_str());
    add_padlock(resources);
    add_dialog_images(resources);
    return resources;
}

// Thumbnail at most 96 px wide, the largest size the XMB draws. Box filter weighted by alpha, so the downscale
// neither aliases nor pulls colour out of transparent pixels.
xmb_image decode_icon(std::span<const uint8_t> bytes)
{
    int width = 0, height = 0, channels = 0;
    if (bytes.empty() || bytes.size() > 8 * 1024 * 1024 ||
        !stbi_info_from_memory(bytes.data(), ::narrow<int>(bytes.size()), &width, &height, &channels) ||
        width <= 0 || height <= 0 || width > 1024 || height > 1024) return {};
    auto* pixels = stbi_load_from_memory(bytes.data(), ::narrow<int>(bytes.size()), &width, &height, &channels, 4);
    if (!pixels) return {};
    const int w = std::min(width, 96), h = std::max(1, height * w / width);
    xmb_image image{w, h, std::vector<uint32_t>(std::size_t(w) * h)};
    for (int y = 0; y < h; ++y)
    {
        const int y0 = y * height / h, y1 = std::max(y0 + 1, (y + 1) * height / h);
        for (int x = 0; x < w; ++x)
        {
            const int x0 = x * width / w, x1 = std::max(x0 + 1, (x + 1) * width / w);
            u64 a = 0, r = 0, g = 0, b = 0;
            for (int sy = y0; sy < y1; ++sy)
                for (int sx = x0; sx < x1; ++sx)
                {
                    const auto* p = pixels + (std::size_t(sy) * width + sx) * 4;
                    a += p[3];
                    r += u64(p[0]) * p[3];
                    g += u64(p[1]) * p[3];
                    b += u64(p[2]) * p[3];
                }
            const u64 n = u64(y1 - y0) * u64(x1 - x0);
            uint32_t value = uint32_t((a + n / 2) / n) << 24;
            if (a) value |= (uint32_t((r + a / 2) / a) << 16) | (uint32_t((g + a / 2) / a) << 8) | uint32_t((b + a / 2) / a);
            image.pixels[std::size_t(y) * w + x] = value;
        }
    }
    stbi_image_free(pixels);
    return image;
}

constexpr u64 icon_file_limit = 8 * 1024 * 1024;

// 'known' is false when the disc has an ICON0.PNG that could not be read (I/O error, e.g. a USB drive waking up).
// Such an empty result must not be cached as "no icon", or the thumbnail would stay missing until the ISO changes.
xmb_image icon_from_archive(iso_archive& archive, bool& known)
{
    known = true;
    const fs::file file(archive.open("PS3_GAME/ICON0.PNG"));
    if (!file) return {}; // the disc has no icon
    std::vector<uint8_t> bytes;
    if (!read_whole(file, icon_file_limit, bytes))
    {
        known = file.size() > icon_file_limit; // too large is final; a failed read is tried again next time
        return {};
    }
    return decode_icon(bytes);
}

// Thumbnail of an ICON0.PNG through the icon cache.
xmb_image load_png_icon(load_context& context, const std::string& png, bool& from_cache)
{
    fs::stat_t stat{};
    if (!fs::get_stat(png, stat) || stat.is_directory) return {};
    xmb_image image;
    if (xmb_cache::read_icon(png, stat, image))
    {
        from_cache = true;
        return image;
    }
    std::vector<uint8_t> bytes;
    const bool read = read_whole(fs::file(png), icon_file_limit, bytes);
    if (read) image = decode_icon(bytes);
    // A file that could not be read is not recorded as "no icon"; the next request reads it again.
    if ((read || stat.size > icon_file_limit) && context.cache_ready()) xmb_cache::write_icon(png, stat, image);
    return image;
}

// 0.26: what a PS3 PKG contains (update, DLC/add-on data, full PSN game), read from its header and the
// PARAM.SFO inside it by the original package_reader (only the item table is decrypted, not the payload).
// 0.33: library category given to a PS4 package (header magic "\x7FCNT"). Drives used with GoldHEN often hold them;
// they are never PS3 content, so USB listings leave them out (PARAM.SFO categories are two letters: no clash).
constexpr char ps4_package_category[] = "@PS4PKG";

bool is_ps4_package(const std::string& path)
{
    char magic[4]{};
    const fs::file file(path);
    return file && file.read(magic, sizeof(magic)) == sizeof(magic) && std::memcmp(magic, "\x7F" "CNT", 4) == 0;
}

// 0.37: what the first 0x30 bytes and the size of a PKG (every part of a split one) say before package_reader reads it:
// a file still being copied is never offered for install, and every refusal names its exact reason. One stat per part
// and a 48-byte read; package_reader (and its 'PKG file size mismatch') only runs on a complete PS3 package.
utils::ps4::pkg::verdict check_package_file(const std::string& path)
{
    namespace pkg = utils::ps4::pkg;
    fs::stat_t stat{};
    fs::file file;
    if (!fs::get_stat(path, stat) || stat.is_directory || !file.open(path)) return pkg::open_failed(fmt::format("%s", fs::g_tls_error));
    unsigned char bytes[pkg::header_size]{};
    const u64 got = file.read(bytes, sizeof(bytes));
    const pkg::header_info header = pkg::parse_header(bytes, static_cast<std::size_t>(std::min<u64>(got, sizeof(bytes))));
    pkg::file_facts facts;
    facts.have = stat.size;
    s64 newest = stat.mtime;
    // Split package: the parts unpkg.cpp gathers (same names), up to the first one that is absent or empty
    if (header.kind == pkg::magic_kind::ps3 && header.pkg_size > facts.have && utils::ps4::package_part(path) == 0)
    {
        facts.split = true;
        for (unsigned index = 1; facts.have < header.pkg_size && index < 1000; ++index)
        {
            const std::string part = pkg::part_name(path, index);
            fs::stat_t info{};
            if (part.empty() || !fs::get_stat(part, info) || info.is_directory || !info.size)
            {
                facts.missing_part = std::string(pkg::file_name(part));
                break;
            }
            facts.have += info.size;
            newest = std::max(newest, info.mtime);
        }
    }
    facts.idle_seconds = static_cast<s64>(std::time(nullptr)) - newest;
    return pkg::judge(header, facts);
}

std::string describe_package(const std::string& path, std::string& title, bool* valid = nullptr, bool* ps4 = nullptr)
{
    if (ps4) *ps4 = false;
    // 0.37: a PS4, PSP/Vita or incomplete package is described from its header and size (PS4 and PSP/Vita rows are final
    // and cached; the others are read again by the next scan)
    if (const auto check = check_package_file(path); !check.ready())
    {
        if (valid) *valid = check.final();
        if (ps4) *ps4 = check.kind == utils::ps4::pkg::state::ps4;
        return check.subtitle;
    }
    package_reader reader(path);
    if (valid) *valid = reader.is_valid();
    if (!reader.is_valid()) return i18n::tr(str::LAU_PKG_CORROMPIDO_TIPO_DESCONHECIDO);
    if (reader.get_header().pkg_platform != PKG_PLATFORM_TYPE_PS3) return i18n::tr(str::LAU_PKG_NAO_PS3);
    const auto& sfo = reader.get_psf();
    const std::string serial(psf::get_string(sfo, "TITLE_ID", ""));
    const std::string category(psf::get_string(sfo, "CATEGORY", ""));
    const std::string version(psf::get_string(sfo, "APP_VER", ""));
    if (const auto sfo_title = psf::get_string(sfo, "TITLE", ""); !sfo_title.empty()) title = std::string(sfo_title);
    const u32 content = reader.get_metadata().content_type;
    std::string kind;
    if (category == "GD" && !version.empty()) kind = i18n::trf(str::LAU_UPDATE_V, {version});
    else if (content == PKG_CONTENT_TYPE_GAME_EXEC || category == "HG") kind = i18n::tr(str::COM_JOGO_PSN);
    else if (content == PKG_CONTENT_TYPE_LICENSE) kind = i18n::tr(str::COM_LICENCA);
    else kind = i18n::tr(str::COM_DLC_CONTEUDO_ADICIONAL);
    return serial.empty() ? i18n::trf(str::LAU_PACOTE_INSTALAR, {i18n::raw(kind)}) : i18n::trf(str::LAU_PACOTE_PARA_INSTALAR, {i18n::raw(kind), serial});
}

utils::ps4::game_kind game_kind_of(const std::string& path)
{
    return fs::is_file(path) ? utils::ps4::file_kind(path) : utils::ps4::game_kind::folder;
}

// Folder that holds PARAM.SFO and ICON0.PNG of a folder game (or of the game around an EBOOT).
std::string metadata_base(const std::string& path)
{
    return fs::is_dir(path) ? path + (fs::is_file(path + "/PS3_GAME/PARAM.SFO") ? "/PS3_GAME/" : "/") : fs::get_parent_dir(path) + "/../";
}

// File whose size and mtime decide whether a cached description is still valid.
std::string metadata_source(const std::string& path, utils::ps4::game_kind kind)
{
    if (kind == utils::ps4::game_kind::iso || kind == utils::ps4::game_kind::package) return path;
    return metadata_base(path) + "PARAM.SFO";
}

// The XMB draws one line per text, and the library cache is one line per game. Some PARAM.SFO titles carry line
// breaks (two-line titles on the PS3 XMB), which would otherwise be drawn as '?' and keep the game out of the cache.
void one_line(std::string& text)
{
    for (char& c : text)
        if (c == '\t' || c == '\r' || c == '\n') c = ' ';
}

// 0.39: an image file of the library (not the drive's virtual device, which would be read over FTP)
bool iso_file_on_disk(const std::string& path)
{
    return utils::ps4::file_kind(path) == utils::ps4::game_kind::iso && !path.starts_with("/vfsv0_");
}

// 0.39: the size of an ISO file against the size of the image its PVD (or PS3 region table) describes: 6 KiB read
utils::ps4::disc_image::completeness iso_completeness(const std::string& path)
{
    utils::ps4::disc_image::completeness out;
    if (!iso_file_on_disk(path)) return out;
    const fs::file file(path);
    if (!file) return out;
    out.file = file.size();
    std::array<u8, 3 * 2048> head{}; // sectors 0-1 and 16
    if (file.read_at(0, head.data(), 4096) != 4096 || file.read_at(16 * 2048, head.data() + 4096, 2048) != 2048) return out;
    out.image = utils::ps4::disc_image::expected_bytes(head.data(), 4096, head.data() + 4096, 2048);
    return out;
}

// One startup.log line per incomplete image and size (the row is described again at every scan until it is complete)
void log_incomplete_iso(const std::string& path, const utils::ps4::disc_image::completeness& size)
{
    static std::mutex mutex;
    static std::map<std::string, u64>* const logged = new std::map<std::string, u64>(); // never destroyed
    {
        std::lock_guard lock(mutex);
        if (const auto [it, added] = logged->try_emplace(path, size.file); !added)
        {
            if (it->second == size.file || logged->size() > 256) return;
            it->second = size.file;
        }
    }
    ps4_startup_checkpoint(fmt::format("XMB: '%s' is incomplete: file %llu bytes, image %llu bytes", path, size.file, size.image).c_str());
}

// 0.39: an ISO of a disc game (CATEGORY DG) without a valid PS3_DISC.SFB: the boot refuses it (Emulator::IsValidSfb)
bool iso_sfb_missing(iso_archive& archive, std::string_view category)
{
    if (category != "DG") return false;
    const bool exists = archive.exists("PS3_DISC.SFB");
    std::string first;
    if (exists)
        if (const auto file = archive.open("PS3_DISC.SFB"))
        {
            char magic[4]{};
            first.assign(magic, static_cast<std::size_t>(file->read(magic, sizeof(magic))));
        }
    // The directory always reads in the clear; the file's bytes do not in an encrypted image read without its key, so
    // their magic is judged only when the key status is OK
    return utils::ps4::disc_image::iso_disc_sfb_missing(category, exists, first) && (!exists || archive.get_key_status() == iso_key_status::OK);
}


// 'iso_icon' (ISO only): the thumbnail is decoded from the same parse of the ISO directory tree; 'icon_known' says
// whether it may be cached. 'complete' is false when the metadata could not be read (unreadable image or PARAM.SFO,
// unfinished package) or the image is incomplete: that description is shown but not cached as final, so the next scan
// reads it again.
game_info describe_game(const std::string& path, utils::ps4::game_kind kind, bool& complete, xmb_image* iso_icon = nullptr,
    bool* icon_known = nullptr)
{
    complete = false;
    if (icon_known) *icon_known = false;
    game_info info;
    info.path = path;
    info.title = path;
    if (const auto slash = info.title.find_last_of('/'); slash != std::string::npos) info.title.erase(0, slash + 1);
    info.subtitle = i18n::tr(kind == utils::ps4::game_kind::package ? str::LAU_PACOTE_PS3_INSTALAR :
        kind == utils::ps4::game_kind::iso ? str::LAU_ISO_DE_PS3 : str::LAU_JOGO_PS3);
    info.icon_name = "item_tex_ps3game_folder";
    if (kind == utils::ps4::game_kind::package)
    {
        info.icon_name = "item_tex_disc_install";
        bool ps4 = false;
        info.subtitle = describe_package(path, info.title, &complete, &ps4);
        if (ps4) info.category = ps4_package_category;
        one_line(info.title);
        one_line(info.subtitle);
        return info;
    }
    psf::registry metadata;
    bool no_sfb = false;
    utils::ps4::disc_image::completeness iso_size;
    if (kind == utils::ps4::game_kind::iso)
    {
        iso_archive archive(path);
        if (archive.is_valid())
        {
            metadata = archive.open_psf("PS3_GAME/PARAM.SFO");
            if (iso_icon)
            {
                bool known = false;
                *iso_icon = icon_from_archive(archive, known);
                if (icon_known) *icon_known = known;
            }
            no_sfb = iso_sfb_missing(archive, psf::get_string(metadata, "CATEGORY", ""));
        }
        iso_size = iso_completeness(path);
    }
    else metadata = psf::load_object(metadata_base(path) + "PARAM.SFO");
    complete = !metadata.empty();
    if (const auto title = psf::get_string(metadata, "TITLE"); !title.empty()) info.title = title;
    info.category = psf::get_string(metadata, "CATEGORY", "");
    info.app_ver = psf::get_string(metadata, "APP_VER", "");
    // 0.39: an install that stopped before its PARAM.SFO (the installer writes it last) is not a game yet
    if (metadata.empty() && kind == utils::ps4::game_kind::folder && path.starts_with("/data/rpcs3/dev_hdd0/game/") &&
        !fs::exists(metadata_base(path) + "PARAM.SFO") && fs::g_tls_error == fs::error::noent)
        info.subtitle = i18n::tr(str::LAU_INSTALACAO_INCOMPLETA_PKG);
    // 0.39: an ISO shorter than the image it describes (an FTP copy cut short or still running) boots into a fatal
    // error; the row says so and is not cached as final, so the next scan of a finished upload shows the game
    if (iso_size.short_file())
    {
        info.subtitle = utils::ps4::disc_image::incomplete_text(iso_size);
        complete = false;
        log_incomplete_iso(path, iso_size);
    }
    if (const auto serial = psf::get_string(metadata, "TITLE_ID"); !serial.empty())
    {
        info.subtitle += " · " + std::string(serial);
        info.serial = serial;
    }
    if (no_sfb) info.subtitle += std::string(" · ") + i18n::tr(str::LAU_ISO_SEM_SFB_NAO_INICIA);
    for (std::string* field : {&info.title, &info.subtitle, &info.category, &info.app_ver, &info.serial}) one_line(*field);
    return info;
}

// 0.33: APP_VER the boot will report for this game (Emu.GetAppVersion), which decides the patches that apply: the
// update's when a disc game has one in dev_hdd0/game/<serial> (the boot then runs that EBOOT, System.cpp), else the
// game's own PARAM.SFO; VERSION when APP_VER is missing. Empty when PARAM.SFO cannot be read.
std::string boot_app_version(const std::string& path, const std::string& serial)
{
    const auto kind = game_kind_of(path);
    if (kind == utils::ps4::game_kind::package) return {};
    psf::registry metadata;
    if (kind == utils::ps4::game_kind::iso)
    {
        iso_archive archive(path);
        if (archive.is_valid()) metadata = archive.open_psf("PS3_GAME/PARAM.SFO");
    }
    else metadata = psf::load_object(metadata_base(path) + "PARAM.SFO");
    const auto version_of = [](const psf::registry& sfo)
    {
        const auto app = psf::get_string(sfo, "APP_VER", "");
        return std::string(app.empty() ? psf::get_string(sfo, "VERSION", "") : app);
    };
    std::string version = version_of(metadata);
    const bool disc = kind == utils::ps4::game_kind::iso || psf::get_string(metadata, "CATEGORY", "") == "DG";
    const std::string update = "/data/rpcs3/dev_hdd0/game/" + serial;
    if (disc && !serial.empty() && fs::is_file(update + "/USRDIR/EBOOT.BIN"))
    {
        const auto sfo = psf::load_object(update + "/PARAM.SFO");
        if (psf::get_string(sfo, "TITLE_ID", "") == serial && psf::get_string(sfo, "CATEGORY", "") == "GD")
            if (std::string installed = version_of(sfo); !installed.empty()) version = std::move(installed);
    }
    one_line(version);
    return version;
}

// 0.33: reads one drive with the PS3 layout (PS3ISO, GAMES, GAMEZ, PS3 and PKG folders, disc images at the root),
// on the loader thread. Nothing is ever written to a drive.
void scan_usb(usb_drive& drive, unsigned index, const std::string& key, const library_cache& library, const std::atomic<bool>& stopping)
{
    drive = {};
    drive.key = key;
    drive.scanned = true;
    const std::string mount = utils::ps4::usb_mount(index);
    const u64 start = sceKernelGetProcessTime();
    // A game was chosen and the XMB joins this thread: the rest of the walk lists nothing, so the boot does not wait for
    // a large drive (the listing is discarded with the XMB).
    const auto stopped = [&] { return stopping.load(std::memory_order_relaxed); };
    std::vector<utils::ps4::library_item> root;
    {
        fs::dir directory(mount);
        drive.readable = static_cast<bool>(directory);
        if (directory) for (const auto& item : directory)
            if (item.name != "." && item.name != "..") root.push_back({item.name, item.is_directory, item.is_symlink});
    }
    auto scan = utils::ps4::scan_usb_drive(mount, root, [&](const std::string& path)
    {
        std::vector<utils::ps4::library_item> entries;
        if (stopped()) return entries;
        fs::dir directory(path);
        if (directory) for (const auto& item : directory) entries.push_back({item.name, item.is_directory, item.is_symlink});
        return entries;
    }, [&](const std::string& path) { return !stopped() && fs::is_file(path); });
    // PS4 packages (a GoldHEN 'PKG' folder holds them) are never PS3 content: counted here, so the drive's row and the
    // hot-plug message count only what the game column lists. A package the library cache already describes (same size
    // and time) costs one stat; any other one a 4-byte read.
    for (const std::string& path : scan.games)
    {
        if (stopped()) break;
        if (utils::ps4::file_kind(path) != utils::ps4::game_kind::package) continue;
        fs::stat_t stat{};
        const game_info* known = library.find(path);
        const bool cached = known && known->keyed && fs::get_stat(path, stat) && !stat.is_directory && known->size == stat.size && known->mtime == stat.mtime;
        if (cached ? known->category == ps4_package_category : is_ps4_package(path)) ++drive.ps4_packages;
    }
    drive.games = std::move(scan.games);
    drive.folders = std::move(scan.folders);
    drive.truncated = scan.truncated;
    ps4_startup_checkpoint(fmt::format("library: %s (%s): %s; %u game(s)/package(s), %u PS4 package(s) hidden; folders: %s; %u entries read in %.1f ms%s%s",
        utils::ps4::origin_label(mount), mount, drive.readable ? "readable" : "ROOT NOT READABLE", drive.shown_games(), drive.ps4_packages,
        drive.folders.empty() ? std::string("none") : fmt::merge(drive.folders, ", "), scan.entries,
        double(sceKernelGetProcessTime() - start) / 1000, drive.truncated ? "; entry budget reached, listing partial" : "",
        stopped() ? "; stopped (the XMB closed)" : "").c_str());
}

// Row of a drive in the storage column, also shown in the game column while the drive holds no game.
// Path "@usb:<state>:<index>".
xmb_entry usb_row(const usb_drive& drive, unsigned index)
{
    const std::string mount = utils::ps4::usb_mount(index);
    const std::string label = utils::ps4::origin_label(mount);
    switch (state_of(drive))
    {
    case usb_state::games:
    {
        std::string help = i18n::trf(str::LAU_USB_PASTAS_SO_LEITURA, {mount, fmt::merge(drive.folders, ", ")});
        if (drive.truncated) help += i18n::tr(str::LAU_USB_DISCO_GRANDE_LISTA_PARCIAL);
        if (drive.ps4_packages) help += i18n::trf(str::LAU_USB_PKG_PS4_IGNORADOS, {drive.ps4_packages});
        return {fmt::format("@usb:games:%u", index), i18n::trf(str::LAU_USB_JOGOS_OU_PACOTES, {label, drive.shown_games()}), std::move(help),
            "item_tex_plain_folder", {}};
    }
    case usb_state::empty:
        // Only PS4 packages (GoldHEN): say so, instead of an empty PKG folder.
        if (drive.ps4_packages)
            return {fmt::format("@usb:empty:%u", index), i18n::trf(str::LAU_USB_SO_PKG_PS4, {label, drive.ps4_packages}),
                i18n::tr(str::LAU_USB_PKG_PS4_GOLDHEN_HELP), "item_tex_plain_folder", {}};
        return {fmt::format("@usb:empty:%u", index), utils::ps4::patches::clip(i18n::trf(str::LAU_USB_NENHUM_JOGO_PASTAS, {label, fmt::merge(drive.folders, ", ")}), line_bytes(64)),
            i18n::tr(str::LAU_USB_LAYOUT_HELP), "item_tex_plain_folder", {}};
    case usb_state::no_layout:
        return {fmt::format("@usb:layout:%u", index), i18n::trf(str::LAU_USB_SEM_PASTAS, {label}), i18n::tr(str::LAU_USB_CRIE_PASTAS_PC), "item_tex_plain_folder", {}};
    case usb_state::unreadable:
        return {fmt::format("@usb:read:%u", index), i18n::trf(str::LAU_USB_NAO_FOI_POSSIVEL_LER, {label}), i18n::tr(str::LAU_USB_USE_DISCO_EXFAT),
            "item_tex_plain_folder", {}};
    default:
        return {};
    }
}

const char* storage_help() { return i18n::tr(str::LAU_STORAGE_HELP); }

// 0.37: the PS3 disc in the PS4 drive (read through the GoldHEN FTP server). The library scan puts this placeholder in
// the storage column; the XMB loop fills in what the drive holds (disc_ui).
constexpr char disc_row_path[] = "@disc:install";
xmb_entry disc_drive_row()
{
    return {disc_row_path, i18n::tr(str::COM_DISCO_PS3_LEITOR_PS4), i18n::tr(str::COM_VERIFICANDO_LEITOR_DISCOS), "item_tex_disc_install", {}};
}

// 0.34: '@disc:keys' copies the keys of encrypted disc images from USB:/redump; '@disc:probe' is the read-only test of
// the Blu-ray drive (X twice), which only writes what it finds to startup.log: nothing ever boots from the drive.
xmb_entry disc_keys_row()
{
    return {"@disc:keys", i18n::tr(str::LAU_IMPORTAR_CHAVES_DKEY), i18n::tr(str::LAU_IMPORTAR_CHAVES_DKEY_HELP), "item_tex_disc_install", {}};
}
xmb_entry disc_probe_row()
{
    return {"@disc:probe", i18n::tr(str::LAU_TESTAR_LEITOR), i18n::tr(str::LAU_TESTAR_LEITOR_HELP), "item_tex_disc_install", {}};
}

// 0.34: USB:/redump/*.dkey (folder name in any case, every USB port) into the redump key folder, where the ISO loader
// looks for <image name>.dkey and <TITLE_ID>.dkey. Only a file holding a key in one of the two forms the loader reads
// (16 raw bytes or 32 hex characters) is taken, and the copy holds just that key. The USB drives are only read.
// Returns the XMB message.
std::string import_disc_keys()
{
    const std::string target_dir = rpcs3::utils::get_redump_key_dir();
    u32 folders = 0, imported = 0, present = 0, refused = 0, failed = 0;
    for (unsigned i = 0; i < utils::ps4::usb_drive_count; ++i)
    {
        const std::string mount = utils::ps4::usb_mount(i);
        std::vector<std::string> sources;
        if (fs::dir root(mount); root)
            for (const auto& item : root)
                if (item.is_directory && !item.is_symlink && utils::ps4::ascii_lower(item.name) == "redump") sources.push_back(mount + "/" + item.name);
        for (const std::string& folder : sources)
        {
            ++folders;
            std::vector<fs::dir_entry> entries;
            if (fs::dir dir(folder); dir)
                for (auto&& item : dir) entries.push_back(std::move(item));
            for (const auto& item : entries)
            {
                if (item.is_directory || item.is_symlink) continue;
                const std::string name = utils::ps4::disc_keys::target_name(item.name);
                if (name.empty()) continue;
                const std::string source = folder + "/" + item.name;
                std::vector<uint8_t> data;
                const std::string key = item.size <= 64 && read_whole(fs::file(source), 64, data)
                    ? utils::ps4::disc_keys::contents(data.data(), data.size()) : std::string{};
                // A key pack holds thousands of files: RPCS3.log gets the first 32 of each kind, startup.log the totals
                if (key.empty())
                {
                    if (++refused <= 32) ps4_launcher_log.warning("Disc key '%s' skipped: a .dkey holds 32 hex characters or 16 bytes (%u bytes)", source, item.size);
                    continue;
                }
                const std::string target = target_dir + name;
                std::vector<uint8_t> current;
                if (fs::file existing(target); existing && read_whole(existing, 64, current) && std::string(current.begin(), current.end()) == key)
                {
                    present++;
                    continue;
                }
                fs::file out;
                if (!fs::create_path(target_dir) || !out.open(target, fs::rewrite) || out.write(key.data(), key.size()) != key.size())
                {
                    if (++failed <= 32) ps4_launcher_log.error("Cannot import disc key '%s' to '%s': %s", source, target, fs::g_tls_error);
                    continue;
                }
                if (++imported <= 32) ps4_launcher_log.success("Disc key imported: %s -> %s", source, target);
            }
        }
    }
    ps4_startup_checkpoint(fmt::format("disc keys: %u .dkey imported into %s (%u already there, %u not a key, %u failed; %u USB redump folder(s))",
        imported, target_dir, present, refused, failed, folders).c_str());
    if (!folders) return i18n::tr(str::LAU_DKEY_NENHUMA_PASTA_REDUMP);
    if (failed) return i18n::trf(str::LAU_DKEY_ERRO_GRAVAR, {failed, imported});
    std::string text = imported ? i18n::trf(str::LAU_DKEY_IMPORTADAS, {imported}) : i18n::trf(str::LAU_DKEY_NENHUMA_NOVA, {present});
    if (refused) text += i18n::trf(str::LAU_DKEY_INVALIDOS, {refused});
    return text;
}

// The import runs on a thread of its own: a redump key pack holds thousands of files. Null when no thread was given.
struct disc_key_import
{
    std::atomic<bool> done{false};
    std::string text; // written before 'done'
};
std::shared_ptr<disc_key_import> start_disc_key_import()
{
    if (utils::ps4::governor::thread_pressure() >= 2) return nullptr;
    auto job = std::make_shared<disc_key_import>();
    if (!utils::ps4::disc_probe::run_detached([job]
        {
            job->text = import_disc_keys();
            job->done.store(true, std::memory_order_release);
        }))
        return nullptr;
    return job;
}

struct library_listing
{
    std::vector<std::string> paths;
    std::vector<xmb_entry> notes;   // game column: drives that hold no game
    std::vector<xmb_entry> storage; // 'Armazenamento' column
    std::string changes;            // drives that came or went since the previous scan of this XMB
};

// rescan: read every drive again (Triangle, 'Atualizar biblioteca'). Otherwise a drive whose probe key did not change
// keeps its listing, so a drive plugged in next to a large one costs only its own scan.
library_listing list_games(load_context& context, bool rescan)
{
    library_listing out;
    std::vector<std::string> library_roots;
    out.paths = list_internal_games(&library_roots);
    // 0.37: the baseline of the folder watch (usb_poll); a folder that cannot be read now is not watched
    context.watched_roots.clear();
    for (auto& root : library_roots)
        if (fs::stat_t info{}; fs::get_stat(root, info) && info.is_directory) context.watched_roots.emplace_back(std::move(root), info.mtime);
    const usb_keys keys = probe_usb_drives();
    std::vector<std::string> changes;
    u32 read = 0, kept = 0;
    out.storage.push_back({"@storage", i18n::tr(str::COM_ARMAZENAMENTO_JOGOS), storage_help(), "item_tex_plain_folder", {}});
    for (unsigned i = 0; i < keys.size(); ++i)
    {
        usb_drive& drive = context.usb[i];
        const usb_state before = state_of(drive);
        const bool had = before != usb_state::absent;
        if (keys[i].empty()) drive = {};
        else if (rescan || !drive.scanned || drive.key != keys[i])
        {
            scan_usb(drive, i, keys[i], context.library, context.stopping);
            ++read;
        }
        else ++kept;
        const usb_state state = state_of(drive);
        const std::string label = utils::ps4::origin_label(utils::ps4::usb_mount(i));
        if (context.usb_known && had && state == usb_state::absent)
            changes.push_back(i18n::trf(before == usb_state::games ? str::LAU_USB_DESCONECTADO_JOGOS_SAIRAM : str::LAU_USB_DESCONECTADO, {label}));
        if (context.usb_known && !had && state != usb_state::absent)
        {
            changes.push_back(state == usb_state::games ? i18n::trf(str::LAU_USB_CONECTADO_JOGOS, {label, drive.shown_games()})
                : i18n::trf(state == usb_state::empty && drive.ps4_packages ? str::LAU_USB_CONECTADO_SO_PKG_PS4
                    : state == usb_state::empty ? str::LAU_USB_CONECTADO_NENHUM_JOGO
                    : state == usb_state::no_layout ? str::LAU_USB_CONECTADO_SEM_PASTAS
                    : str::LAU_USB_CONECTADO_NAO_LEU, {label}));
        }
        if (state == usb_state::absent) continue;
        out.paths.insert(out.paths.end(), drive.games.begin(), drive.games.end());
        out.storage.push_back(usb_row(drive, i));
        if (state != usb_state::games) out.notes.push_back(out.storage.back());
    }
    if (out.storage.size() == 1)
    {
        if (extended_storage_mounted())
            out.storage.push_back({"@usb:none", i18n::tr(str::LAU_USB_SO_ARMAZENAMENTO_ESTENDIDO), i18n::tr(str::LAU_USB_ESTENDIDO_CRIPTOGRAFADO),
                "item_tex_plain_folder", {}});
        else
            out.storage.push_back({"@usb:none", i18n::tr(str::LAU_USB_NENHUM_CONECTADO), i18n::tr(str::LAU_USB_NENHUM_CONECTADO_HELP),
                "item_tex_plain_folder", {}});
    }
    out.storage.push_back(disc_drive_row());
    // 0.39: keys come by themselves (drive and encrypted ISOs); the manual import only when a drive brings a redump folder
    bool redump_folder = false;
    for (unsigned i = 0; i < keys.size() && !redump_folder; ++i)
        if (!keys[i].empty())
            if (fs::dir root(utils::ps4::usb_mount(i)); root)
                for (const auto& item : root)
                    if (item.is_directory && !item.is_symlink && utils::ps4::ascii_lower(item.name) == "redump") redump_folder = true;
    if (redump_folder) out.storage.push_back(disc_keys_row());
    out.storage.push_back(disc_probe_row());
    context.usb_known = true;
    out.changes = utils::ps4::patches::clip(fmt::merge(changes, " · "), line_bytes(110));
    // The game that was running stays listed even outside the library folders, unless it is gone (e.g. its USB drive
    // was removed while it ran).
    if (!context.preferred.empty() && fs::exists(context.preferred)) out.paths.push_back(context.preferred);
    std::sort(out.paths.begin(), out.paths.end());
    out.paths.erase(std::unique(out.paths.begin(), out.paths.end()), out.paths.end());
    ps4_launcher_log.notice("Library found %u games/packages: %s", out.paths.size(), fmt::merge(out.paths, "\n"));
    ps4_startup_checkpoint(fmt::format("library scan: %u games/packages found (USB: %u drive(s) read, %u unchanged)", out.paths.size(), read, kept).c_str());
    return out;
}

// 'language': the interface language of this scan; descriptions cached in another one are made again
std::vector<game_info> scan_library(load_context& context, const std::vector<std::string>& paths, u32& cached, u32& read, bool& partial,
    i18n::lang language)
{
    std::vector<game_info> games;
    context.waiting_packages.clear();
    const bool same_language = context.library.language == language;
    for (const auto& path : paths)
    {
        if (context.stopping.load(std::memory_order_relaxed))
        {
            partial = true;
            break;
        }
        const auto kind = game_kind_of(path);
        fs::stat_t stat{};
        const bool keyed = fs::get_stat(metadata_source(path, kind), stat) && !stat.is_directory;
        if (const game_info* hit = keyed && same_language ? context.library.find(path) : nullptr; hit && hit->keyed && hit->size == stat.size && hit->mtime == stat.mtime)
        {
            games.push_back(*hit);
            ++cached;
            continue;
        }
        const bool iso = kind == utils::ps4::game_kind::iso;
        xmb_image icon;
        bool complete = false, icon_known = false;
        game_info info = describe_game(path, kind, complete, iso && keyed ? &icon : nullptr, &icon_known);
        // 0.37: an incomplete PKG on /data (never cached) is watched until its state changes. USB drives cannot be
        // written while they are mounted here, so theirs are not polled.
        if (kind == utils::ps4::game_kind::package && !complete && utils::ps4::usb_drive_of(path) < 0)
            if (const auto check = check_package_file(path); !check.ready() && check.kind != utils::ps4::pkg::state::not_package)
                context.waiting_packages.insert_or_assign(path, check.kind);
        // A description made after a read error is not keyed, so the next scan reads the game again.
        info.keyed = keyed && complete;
        info.size = info.keyed ? stat.size : 0;
        info.mtime = info.keyed ? stat.mtime : 0;
        // The ISO tree was just walked for PARAM.SFO; keep its thumbnail so the icon job does not walk it again.
        if (iso && keyed && icon_known && context.cache_ready()) xmb_cache::write_icon(path, stat, icon);
        games.push_back(std::move(info));
        ++read;
    }
    return games;
}

// 0.33: a PSN game kept as a folder (CATEGORY HG) runs with that folder mounted as its /dev_hdd0/game/<dir> and writes
// its data there (System.cpp, add_external_hdd_game). From a USB drive it would write to the drive, which this port
// never does: such a game is listed but has to be copied to the PS4 or installed from its PKG.
bool writes_to_usb(const std::string& path, std::string_view category)
{
    return utils::ps4::usb_drive_of(path) >= 0 && category == "HG";
}

std::string file_name_of(const std::string& path)
{
    const auto slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// 0.34: a disc game kept as a folder boots only with a valid PS3_DISC.SFB next to its PS3_GAME (Emulator::GetBdvdDir);
// without one the boot ends in "Invalid file or folder". 'category' is read from <path>/PS3_GAME/PARAM.SFO.
bool disc_folder_without_sfb(const std::string& path, std::string_view category)
{
    return category == "DG" && utils::ps4::file_kind(path) == utils::ps4::game_kind::none && fs::is_file(path + "/PS3_GAME/PARAM.SFO") &&
        !Emulator::IsValidSfb(path + "/PS3_DISC.SFB");
}

// Game column: the two utility rows, then the games. 0.26: an installed update (or DLC data) lives in
// dev_hdd0/game/<TITLE_ID> with CATEGORY "GD". It is not a bootable game: RPCS3 applies it when the disc/ISO
// with the same TITLE_ID boots. Hide it and show it on that game instead.
// 0.33: every game row starts its subtitle with where it is stored ("PS4", "USB 1", ...); 'notes' (USB drives that
// hold no game) follow the games.
// 0.34: 'check_files' (the loader's scans, not the cached list shown at open) looks for the PS3_DISC.SFB of disc
// folders; a library folder that is a game itself asks to be moved into a sub-folder.
// 0.37: 'locks' (disc_lock registry): an ISO installed from a PS3 disc is bound to its lock record and starts locked;
// the UI unlocks it from the drive's status.
// 0.39: only the exact image a disc install wrote is bound (path and size of its record); every other ISO of the same
// game is an ordinary ISO (one startup.log line when its TITLE_ID has a record).
std::vector<xmb_entry> game_column(const std::vector<game_info>& games, const std::vector<xmb_entry>& notes = {}, bool check_files = false,
    const utils::ps4::disc_lock::registry* locks = nullptr)
{
    namespace dl = utils::ps4::disc_lock;
    std::vector<xmb_entry> entries{{"@savedata", i18n::tr(str::LAU_UTILITARIO_DADOS_GUARDADOS), "", "item_tex_ps3util", {}},
        {"@licenses", i18n::tr(str::LAU_IMPORTAR_LICENCAS_RAP), i18n::tr(str::LAU_IMPORTAR_LICENCAS_RAP_HELP), "item_tex_disc_install", {}}};
    const auto installed_update = [](const game_info& game)
    { return game.category == "GD" && game.path.starts_with("/data/rpcs3/dev_hdd0/game/"); };
    std::map<std::string, std::string> installed_updates;
    for (const auto& game : games)
        if (installed_update(game) && !game.serial.empty()) installed_updates[game.serial] = game.app_ver.empty() ? "?" : game.app_ver;
    for (const auto& game : games)
    {
        if (installed_update(game)) continue;
        if (game.category == ps4_package_category && utils::ps4::usb_drive_of(game.path) >= 0) continue;
        xmb_entry entry{game.path, game.title, utils::ps4::origin_label(game.path) + " · " + game.subtitle, game.icon_name, {}, game.serial};
        if (const auto it = game.serial.empty() ? installed_updates.end() : installed_updates.find(game.serial); it != installed_updates.end())
            entry.subtitle += i18n::trf(str::LAU_UPDATE_INSTALADO, {it->second});
        if (writes_to_usb(game.path, game.category)) entry.subtitle += i18n::tr(str::LAU_JOGO_PSN_PASTA_COPIE);
        if (check_files && disc_folder_without_sfb(game.path, game.category)) entry.subtitle += i18n::tr(str::LAU_FALTA_PS3_DISC_SFB);
        if (utils::ps4::library_root_is_game(game.path)) entry.subtitle += i18n::tr(str::LAU_COPIADO_DIRETO_PASTA);
        if (locks && utils::ps4::file_kind(game.path) == utils::ps4::game_kind::iso)
        {
            const u64 size = game.keyed ? game.size : 0;
            entry.locked = entry.disc_bound = dl::bound(*locks, game.path, size) != nullptr;
            if (entry.disc_bound)
            {
                // The row's label in the language its description was cached in (the next scan writes it in the current one)
                const std::string origin = utils::ps4::origin_label(game.path) + " · ";
                for (const i18n::lang l : {i18n::current(), i18n::lang::pt_br, i18n::lang::en, i18n::lang::es, i18n::lang::ar})
                    if (const std::string_view label = iso_row_label(l); entry.subtitle.size() > origin.size() && entry.subtitle.starts_with(origin) &&
                        std::string_view(entry.subtitle).substr(origin.size()).starts_with(label))
                    {
                        entry.subtitle.replace(origin.size(), label.size(), i18n::tr(str::LAU_INSTALADO_DISCO_PRECISA));
                        break;
                    }
            }
            else if (const std::string why = dl::unbound_reason(*locks, game.path, game.serial, size); !why.empty()) dl::sys::note_once(why);
        }
        entries.push_back(std::move(entry));
    }
    if (entries.size() == 2)
        entries.push_back({{}, i18n::tr(str::LAU_NENHUM_JOGO_ENCONTRADO), i18n::tr(str::LAU_NENHUM_JOGO_ENCONTRADO_HELP), "item_tex_plain_folder", {}});
    entries.insert(entries.end(), notes.begin(), notes.end());
    return entries;
}

std::vector<xmb_entry> loading_column()
{
    auto entries = game_column({});
    entries.back() = {{}, i18n::tr(str::LAU_CARREGANDO_BIBLIOTECA), i18n::tr(str::LAU_LENDO_GAMES_USB), "item_tex_ps3game_folder", {}};
    return entries;
}

// Thumbnail of a game row through the icon cache (ISO: the ISO itself is the cache key).
xmb_image load_game_icon(load_context& context, const std::string& path, bool& from_cache)
{
    const auto kind = game_kind_of(path);
    if (kind == utils::ps4::game_kind::package) return {};
    if (kind != utils::ps4::game_kind::iso) return load_png_icon(context, metadata_base(path) + "ICON0.PNG", from_cache);
    fs::stat_t stat{};
    if (!fs::get_stat(path, stat) || stat.is_directory) return {};
    xmb_image image;
    if (xmb_cache::read_icon(path, stat, image))
    {
        from_cache = true;
        return image;
    }
    iso_archive archive(path);
    bool known = false; // an image that cannot be opened now (I/O error) is tried again later, not cached
    if (archive.is_valid()) image = icon_from_archive(archive, known);
    if (known && context.cache_ready()) xmb_cache::write_icon(path, stat, image);
    return image;
}

std::vector<xmb_entry> xmb_savedata(load_context& context)
{
    constexpr const char* root = "/data/rpcs3/dev_hdd0/home/00000001/savedata/";
    std::vector<xmb_entry> entries;
    if (fs::dir dir{root}) for (const auto& file : dir)
    {
        if (context.stopping.load(std::memory_order_relaxed)) break;
        if (!file.is_directory || file.is_symlink || file.name == "." || file.name == "..") continue;
        const std::string path = std::string(root) + file.name + "/";
        const auto metadata = psf::load_object(path + "PARAM.SFO");
        bool from_cache = false;
        entries.push_back({"@save:" + path, std::string(psf::get_string(metadata, "TITLE", file.name)),
            std::string(psf::get_string(metadata, "SUB_TITLE")), "item_tex_ps3util", load_png_icon(context, path + "ICON0.PNG", from_cache)});
        one_line(entries.back().title);
        one_line(entries.back().subtitle);
    }
    if (entries.empty()) entries.push_back({{}, i18n::tr(str::LAU_NAO_HA_DADOS_GUARDADOS), "", "item_tex_ps3util", {}});
    return entries;
}

// 0.31: disk and ISO work of the XMB runs on the 'PS4 XMB Loader' thread, so the menu keeps drawing while the
// library is scanned, icons are decoded or licenses are imported. When the thread is refused (or the process is
// close to the console's thread limit) the same jobs run on the UI thread, one per frame.
// 0.33: patch_list parses the patch files (seconds for a full patch.yml), patch_import validates and copies them.
// usb_poll compares the USB mounts with the last library scan, cache_clear removes one game's cache and
// config_db_import copies the PC config database. cheat_list reads one game's cheat files, cheat_import validates and
// copies cheat files from USB and /data/rpcs3/import/cheats.
enum class load_kind : u8 { none, resources, licenses, library, icon, savedata, patch_list, patch_import, usb_poll, cache_clear, config_db_import,
    cheat_list, cheat_import };

// 0.33: hot-plug check, every 2 s while the XMB is shown: the drives that differ from what the last library scan read
// ("usb1 added, usb0 removed"). Empty when nothing changed, and before the first scan of this XMB (it reads them all).
std::string usb_changes(const load_context& context)
{
    if (!context.usb_known) return {};
    const usb_keys keys = probe_usb_drives();
    std::vector<std::string> changed;
    for (unsigned i = 0; i < keys.size(); ++i)
    {
        const std::string& before = context.usb[i].key;
        if (keys[i] != before) changed.push_back(fmt::format("usb%u %s", i, before.empty() ? "added" : keys[i].empty() ? "removed" : "replaced"));
    }
    return fmt::merge(changed, ", ");
}

// 0.37: the same poll for the /data library folders: a folder whose mtime moved since the last scan (an FTP client
// created or removed something in it), and every PKG the last scan found incomplete whose state changed since
// ("PKG complete: <file>" when an upload finished; 'completed' gets the first such file name). One stat per folder,
// one stat and a 48-byte read per waiting package; empty before the first scan of this XMB.
std::string library_changes(const load_context& context, std::string& completed)
{
    namespace pkg = utils::ps4::pkg;
    std::vector<std::string> changed;
    for (const auto& [root, mtime] : context.watched_roots)
        if (fs::stat_t info{}; !fs::get_stat(root, info) || info.mtime != mtime) changed.push_back(std::string(pkg::file_name(root)) + " changed");
    for (const auto& [path, before] : context.waiting_packages)
    {
        const pkg::verdict now = check_package_file(path);
        if (now.kind == before) continue;
        const std::string name(pkg::file_name(path));
        if (now.ready())
        {
            changed.push_back("PKG complete: " + name);
            if (completed.empty()) completed = name;
        }
        else changed.push_back(fmt::format("PKG %s: %s", pkg::state_name(now.kind), name));
    }
    return fmt::merge(changed, ", ");
}

// 0.33: 'Limpar cache deste jogo': removes /data/rpcs3/cache/cache/<TITLE ID>/ (PPU modules, SPU and shader caches,
// rebuilt on the next boot) through the health module, which only ever deletes a real title-id folder.
std::string clear_title_cache(const std::string& serial)
{
    namespace health = utils::ps4::health;
    bool present = false;
    // A link named like the title is not a cache folder (lstat is stat on this SDK: only the entry type tells).
    if (fs::dir folder{health::title_cache_dir}) for (const auto& entry : folder)
    {
        if (entry.name != serial) continue;
        present = entry.is_directory && !entry.is_symlink;
        break;
    }
    if (!health::remove_title_cache(serial)) return i18n::trf(str::LAU_CACHE_NAO_LIMPO, {serial});
    if (!present) return i18n::trf(str::LAU_CACHE_NAO_TEM, {serial});
    const u64 free_bytes = health::disk_free_bytes();
    return free_bytes ? i18n::trf(str::LAU_CACHE_LIMPO_LIVRES, {serial, fmt::format("%.1f", double(free_bytes) / 1e9)}) : i18n::trf(str::LAU_CACHE_LIMPO, {serial});
}

struct load_job
{
    load_kind kind = load_kind::none;
    u32 generation = 0;
    // icon, patch_list, cache_clear, cheat_list: game path; licenses: who asked ("", "@refresh", "@licenses");
    // library: "" (first scan), "@rescan" (every USB drive read again) or "@hotplug" (only the drives that changed)
    std::string path;
    std::string serial, title; // patch_list, cache_clear, cheat_list; cheat_import: the game ("" = every game)
    std::vector<std::pair<std::string, std::string>> library; // cheat_import: {title, serial} of the library's games
};

struct load_result
{
    load_kind kind = load_kind::none;
    u32 generation = 0;
    std::string path;
    std::vector<xmb_entry> entries;
    std::map<std::string, xmb_image, std::less<>> resources;
    xmb_image artwork;
    std::shared_ptr<utils::ps4::patches::game_patches> patches; // patch_list
    std::shared_ptr<utils::ps4::cheats::game_cheats> cheats;    // cheat_list; patch_list: the count of its first row
    // patch_import, cache_clear, config_db_import, cheat_import: result line; library: USB drives that came or went;
    // usb_poll: the drives that changed ("" = none)
    std::string text;
    // 0.37 usb_poll: /data library folders and incomplete PKGs that changed ("" = none); the first PKG whose copy finished
    std::string changes, completed;
    std::vector<xmb_entry> storage; // library: 'Armazenamento' column
    std::shared_ptr<const utils::ps4::disc_lock::registry> locks; // library: the disc lock records the rows were bound with
    u32 imported = 0, cached = 0, read = 0;
    bool from_cache = false, partial = false;
    u64 cost_us = 0;
};

struct load_queue
{
    std::mutex mutex;
    std::deque<load_job> jobs;
    std::deque<load_result> done;
    bool inline_mode = false;
    load_context context;

    load_result run(const load_job& job)
    {
        const u64 start = sceKernelGetProcessTime();
        load_result result;
        result.kind = job.kind;
        result.generation = job.generation;
        result.path = job.path;
        switch (job.kind)
        {
        case load_kind::resources:
            result.resources = load_xmb_resources(context, result.from_cache);
            break;
        case load_kind::licenses:
            result.imported = ps4_import_licenses();
            break;
        case load_kind::library:
        {
            const i18n::lang language = i18n::current();
            auto listing = list_games(context, job.path == "@rescan");
            const auto games = scan_library(context, listing.paths, result.cached, result.read, result.partial, language);
            // A drive walk cut short by the XMB closing is incomplete even when no game was left to describe.
            if (!result.partial && !context.stopping.load(std::memory_order_relaxed)) context.library.store(games, context.cache_ready(), language);
            result.locks = std::make_shared<const utils::ps4::disc_lock::registry>(utils::ps4::disc_lock::sys::load_registry());
            result.entries = game_column(games, listing.notes, true, result.locks.get());
            result.storage = std::move(listing.storage);
            result.text = std::move(listing.changes);
            break;
        }
        case load_kind::icon:
            result.artwork = load_game_icon(context, job.path, result.from_cache);
            break;
        case load_kind::savedata:
            result.entries = xmb_savedata(context);
            break;
        case load_kind::patch_list:
        {
            const std::string version = boot_app_version(job.path, job.serial);
            result.patches = std::make_shared<utils::ps4::patches::game_patches>(utils::ps4::patches::load_game(context.patch_catalog,
                job.path, job.title, job.serial, version));
            // the first row of the patch list counts the game's cheats that are on (a few small files)
            result.cheats = std::make_shared<utils::ps4::cheats::game_cheats>(utils::ps4::cheats::load_game(job.serial, job.title, version));
            break;
        }
        case load_kind::cheat_list:
            result.cheats = std::make_shared<utils::ps4::cheats::game_cheats>(utils::ps4::cheats::load_game(job.serial, job.title,
                boot_app_version(job.path, job.serial)));
            break;
        case load_kind::cheat_import:
        {
            const auto report = utils::ps4::cheats::import_files(job.serial, job.library);
            result.text = report.text;
            result.imported = report.files;
            break;
        }
        case load_kind::patch_import:
        {
            const auto report = utils::ps4::patches::import_files();
            result.text = report.text;
            result.imported = report.replaced + report.merged;
            break;
        }
        case load_kind::usb_poll:
            result.text = usb_changes(context);
            result.changes = library_changes(context, result.completed);
            break;
        case load_kind::cache_clear:
            result.text = clear_title_cache(job.serial);
            break;
        case load_kind::config_db_import:
            result.text = config_db::import();
            break;
        default:
            break;
        }
        result.cost_us = sceKernelGetProcessTime() - start;
        return result;
    }
    void post(load_job job)
    {
        std::lock_guard lock(mutex);
        jobs.push_back(std::move(job));
    }
    // Icons go first, nearest row first.
    void post_icons(const std::vector<std::string>& paths, u32 generation)
    {
        std::lock_guard lock(mutex);
        for (auto it = paths.rbegin(); it != paths.rend(); ++it) jobs.push_front({load_kind::icon, generation, *it});
    }
    // Removes icon jobs that did not start yet and returns their paths.
    std::vector<std::string> drop_icons()
    {
        std::vector<std::string> dropped;
        std::lock_guard lock(mutex);
        std::erase_if(jobs, [&](load_job& job)
        {
            if (job.kind != load_kind::icon) return false;
            dropped.push_back(std::move(job.path));
            return true;
        });
        return dropped;
    }
    // Removes library scans that did not start yet: a newer refresh replaces them and their result would be
    // discarded anyway (repeated Triangle presses would otherwise queue one full scan each).
    void drop_library()
    {
        std::lock_guard lock(mutex);
        std::erase_if(jobs, [](const load_job& job) { return job.kind == load_kind::library; });
    }
    bool pop(load_job& job)
    {
        std::lock_guard lock(mutex);
        if (jobs.empty()) return false;
        job = std::move(jobs.front());
        jobs.pop_front();
        return true;
    }
    void finish(load_result result)
    {
        std::lock_guard lock(mutex);
        done.push_back(std::move(result));
    }
    std::deque<load_result> take()
    {
        std::lock_guard lock(mutex);
        return std::exchange(done, {});
    }
    // Inline mode: one job on the calling (UI) thread.
    bool run_one()
    {
        load_job job;
        if (!pop(job)) return false;
        finish(run(job));
        return true;
    }
};

struct load_worker
{
    load_queue* queue = nullptr;
    void operator()()
    {
        while (thread_ctrl::state() != thread_state::aborting && !queue->context.stopping.load())
        {
            load_job job;
            if (!queue->pop(job))
            {
                thread_ctrl::wait_for(100'000); // woken at once by thread_ctrl::notify when a job is posted
                continue;
            }
            queue->finish(queue->run(job));
        }
    }
};

// Totals for the 'XMB loader' line written when the XMB closes.
struct loader_stats
{
    u32 resources = 0, imported = 0;
    bool resources_cache = false;
    u64 resources_us = 0, licenses_us = 0;
    u32 library_runs = 0, library_rows = 0, library_cached = 0, library_read = 0;
    u64 library_us = 0;
    u32 icons = 0, icon_hits = 0;
    u64 icon_us = 0, icon_max_us = 0;
    u32 savedata_runs = 0;
    u64 savedata_us = 0;
    u32 patch_runs = 0;
    u64 patch_us = 0, patch_max_us = 0;
    u32 usb_polls = 0;
    u64 usb_poll_us = 0, usb_poll_max_us = 0;

    void add(const load_result& r)
    {
        switch (r.kind)
        {
        case load_kind::usb_poll: ++usb_polls; usb_poll_us += r.cost_us; usb_poll_max_us = std::max(usb_poll_max_us, r.cost_us); break;
        case load_kind::patch_list:
        case load_kind::patch_import: ++patch_runs; patch_us += r.cost_us; patch_max_us = std::max(patch_max_us, r.cost_us); break;
        case load_kind::resources: resources = static_cast<u32>(r.resources.size()); resources_cache = r.from_cache; resources_us += r.cost_us; break;
        case load_kind::licenses: imported += r.imported; licenses_us += r.cost_us; break;
        case load_kind::library:
            ++library_runs;
            library_rows = static_cast<u32>(r.entries.size());
            library_cached += r.cached;
            library_read += r.read;
            library_us += r.cost_us;
            break;
        case load_kind::icon: ++icons; icon_hits += r.from_cache ? 1 : 0; icon_us += r.cost_us; icon_max_us = std::max(icon_max_us, r.cost_us); break;
        case load_kind::savedata: ++savedata_runs; savedata_us += r.cost_us; break;
        default: break;
        }
    }
    std::string line(bool inline_mode, u64 join_us) const
    {
        return fmt::format("XMB loader (%s): resources %u images in %.1f ms%s; licenses %.1f ms (%u imported); library %u run(s) "
            "%.1f ms, %u rows (%u cached, %u read); icons %u (%u from cache) avg %.1f max %.1f ms; savedata %u run(s) %.1f ms; "
            "patches %u run(s) %.1f ms (max %.1f); usb polls %u avg %.2f max %.1f ms; join %.1f ms",
            inline_mode ? "UI thread" : "thread", resources, double(resources_us) / 1000, resources_cache ? " from cache" : "",
            double(licenses_us) / 1000, imported, library_runs, double(library_us) / 1000, library_rows, library_cached, library_read,
            icons, icon_hits, icons ? double(icon_us) / 1000 / icons : 0., double(icon_max_us) / 1000, savedata_runs,
            double(savedata_us) / 1000, patch_runs, double(patch_us) / 1000, double(patch_max_us) / 1000,
            usb_polls, usb_polls ? double(usb_poll_us) / 1000 / usb_polls : 0., double(usb_poll_max_us) / 1000, double(join_us) / 1000);
    }
};

std::size_t find_path(const std::vector<xmb_entry>& entries, const std::string& path)
{
    if (path.empty()) return no_row;
    for (std::size_t i = 0; i < entries.size(); ++i)
        if (entries[i].path == path) return i;
    return no_row;
}

// 0 = data utility, 1 = license import (0.26): start on the first game when there is one.
std::size_t default_game_row(const std::vector<xmb_entry>& entries)
{
    return entries.size() > 2 && !entries[2].path.empty() ? 2 : 0;
}

// 0.31: 'Usuários' column: the four RPCS3 players and the PS4 user in each seat.
std::vector<xmb_entry> xmb_users(bool lightbar)
{
    static constexpr std::array<str, 4> colours{str::PADS_COLOUR_BLUE, str::PADS_COLOUR_RED, str::PADS_COLOUR_GREEN, str::PADS_COLOUR_PINK};
    static_assert(ps4_pads::max_players <= colours.size());
    const auto players = ps4_pads::snapshot();
    std::vector<xmb_entry> rows;
    for (unsigned slot = 0; slot < ps4_pads::max_players; ++slot)
    {
        const auto& player = players[slot];
        const bool seated = player.user >= 0;
        const std::string name = !seated ? std::string(i18n::tr(str::LAU_JOGADOR_LIVRE)) : player.name.empty() ? std::string(i18n::tr(str::LAU_USUARIO)) : player.name;
        std::string help;
        if (!seated) help = i18n::tr(str::LAU_JOGADOR_LIGUE_OUTRO_CONTROLE);
        else if (!player.connected) help = i18n::tr(str::LAU_CONTROLE_DESLIGADO_APERTE_PS);
        else if (lightbar) help = i18n::trf(str::LAU_CONTROLE_CONECTADO_LUZ, {colours[slot]});
        else help = i18n::tr(str::LAU_CONTROLE_CONECTADO);
        rows.push_back({fmt::format("@player%u", slot + 1), i18n::trf(str::LAU_JOGADOR_N, {slot + 1, name}), std::move(help), "tex_user", {}});
    }
    return rows;
}

// 0.31: 'PlayStation Network' column: the online switches and the RPCN account (ps4_rpcn service, ps4_ime).
std::vector<xmb_entry> xmb_psn()
{
    namespace rows = utils::ps4::online_rows;
    const bool online = utils::ps4::online_enabled();
    const auto hosts = ps4_rpcn::hosts();
    const std::string host = ps4_rpcn::current_host();
    std::vector<xmb_entry> entries;
    entries.push_back(rows::online_entry(online));
    entries.push_back({"@rpcn:status", ps4_rpcn::account_line(), i18n::tr(online ? str::LAU_RPCN_TESTA_CONEXAO : str::LAU_RPCN_TESTA_CONTA_OFFLINE), "tex_psn", {}});
    entries.push_back({"@rpcn:login", i18n::tr(str::LAU_RPCN_ENTRAR), i18n::tr(str::LAU_RPCN_ENTRAR_HELP), "tex_psn", {}});
    entries.push_back({"@rpcn:create", i18n::tr(str::LAU_RPCN_CRIAR), i18n::tr(str::LAU_RPCN_CRIAR_HELP), "tex_psn", {}});
    entries.push_back({"@rpcn:token", i18n::tr(str::LAU_RPCN_TOKEN), i18n::tr(str::LAU_RPCN_TOKEN_HELP), "tex_psn", {}});
    entries.push_back({"@rpcn:resend", i18n::tr(str::LAU_RPCN_REENVIAR), i18n::tr(str::LAU_RPCN_REENVIAR_HELP), "tex_psn", {}});
    entries.push_back({"@rpcn:import", i18n::tr(str::LAU_RPCN_IMPORTAR), i18n::tr(str::LAU_RPCN_IMPORTAR_HELP), "tex_psn", {}});
    entries.push_back({"@rpcn:server", host.empty() ? i18n::trf(str::LAU_RPCN_SERVIDOR, {str::LAU_RPCN_PADRAO}) : i18n::trf(str::LAU_RPCN_SERVIDOR, {host}),
        hosts.size() > 1 ? i18n::trf(str::LAU_RPCN_TROCA_SERVIDOR, {hosts.size()}) : std::string(i18n::tr(str::LAU_RPCN_SERVIDOR_LISTA)),
        "tex_psn", {}});
    entries.push_back(rows::upnp_entry(online, utils::ps4::upnp_enabled()));
    entries.push_back({"@rpcn:diag", i18n::tr(str::LAU_RPCN_DIAGNOSTICO), i18n::tr(str::LAU_RPCN_DIAGNOSTICO_HELP), "tex_psn", {}});
    return entries;
}

// Same rule as the RPCN account form: 3 to 16 letters, digits, '-' or '_'.
bool valid_rpcn_user(std::string_view name)
{
    if (name.size() < 3 || name.size() > 16) return false;
    return std::all_of(name.begin(), name.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_'; });
}

std::string trimmed(std::string_view text)
{
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    return std::string(text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1));
}

// Cuts UTF-8 text to about 'limit' bytes on a character boundary.
std::string shorten(const std::string& text, std::size_t limit)
{
    if (text.size() <= limit) return text;
    std::size_t cut = limit - 3;
    while (cut && (static_cast<unsigned char>(text[cut]) & 0xc0) == 0x80) --cut;
    return text.substr(0, cut) + "...";
}

// Overwrites a string that held a password or a token before it is released, including the bytes past size()
// that a moved-from short string can still hold.
void wipe_secret(std::string& text)
{
    text.resize(text.capacity());
    volatile char* bytes = text.data();
    for (std::size_t i = 0; i < text.size(); ++i) bytes[i] = 0;
    text.clear();
}

// Steps of the RPCN account forms. Account creation asks for the name and the email, then for an explicit
// confirmation, and only then for the password, which goes straight to the service.
enum class rpcn_form : u8 { none, login_user, login_pass, create_user, create_email, create_confirm, create_pass, token };

// 0.33: switch drawn as the artwork of a patch row (green, knob right = on; amber, knob centred = some parts on; grey,
// knob left = off), so every row shows its state without being selected. Straight alpha, like decode_icon.
xmb_image patch_switch_icon(int state)
{
    constexpr int w = 96, h = 56;
    xmb_image image{w, h, std::vector<uint32_t>(std::size_t(w) * h, 0)};
    const uint32_t track = state > 0 ? 0x34c759 : state < 0 ? 0x8e8e93 : 0xffb340;
    const float radius = h / 2.f - 2, cy = h / 2.f, left = radius + 2, right = w - radius - 2;
    const float knob = state > 0 ? right : state < 0 ? left : (left + right) / 2;
    for (int y = 0; y < h; ++y)
    {
        for (int x = 0; x < w; ++x)
        {
            const float px = x + .5f, py = y + .5f;
            const float edge = std::hypot(px - std::clamp(px, left, right), py - cy) - radius;
            const float cover = std::clamp(.5f - edge, 0.f, 1.f);
            if (cover <= 0.f) continue;
            const float white = std::clamp(.5f - (std::hypot(px - knob, py - cy) - (radius - 4)), 0.f, 1.f);
            const auto channel = [&](int shift)
            {
                const float base = float((track >> shift) & 0xff);
                return uint32_t(base + (255.f - base) * white + .5f) << shift;
            };
            image.pixels[std::size_t(y) * w + x] = (uint32_t(cover * 255.f + .5f) << 24) | channel(16) | channel(8) | channel(0);
        }
    }
    return image;
}

struct patch_icons
{
    xmb_image on = patch_switch_icon(1), partial = patch_switch_icon(0), off = patch_switch_icon(-1);
};

// 0.37: every source import_files() reads, in the length one XMB subtitle shows (/data/rpcs3/patches works without import)
const char* patch_import_help() { return i18n::tr(str::LAU_PATCH_IMPORT_HELP); }
// 0.37: the boot TLS self-test (ps4_tls_selftest.h) failed at a step, so a download would fail the same way
const char* patch_download_tls_text() { return i18n::tr(str::LAU_PATCH_DOWNLOAD_TLS_FALHOU); }

std::string patch_download_help()
{
    if (!utils::ps4::patches::can_download()) return i18n::tr(str::LAU_PATCH_DOWNLOAD_INDISPONIVEL_HELP);
    // nullopt (still running, or could not run) never blocks the download
    if (ps4_tls_selftest_ok() == false) return i18n::tr(str::LAU_PATCH_DOWNLOAD_TLS_HELP);
    return i18n::tr(str::LAU_PATCH_DOWNLOAD_HELP);
}

// The parts of an XMB text "part · part · ...": the first one, and the others joined again
std::pair<std::string, std::string> first_part(std::string_view text)
{
    constexpr std::string_view separator = " · ";
    const std::size_t cut = text.find(separator);
    if (cut == std::string_view::npos) return {std::string(text), {}};
    return {std::string(text.substr(0, cut)), std::string(text.substr(cut + separator.size()))};
}

// The row of a list path "<prefix><row>" ("@patch:3"), or no_row
std::size_t row_index(std::string_view path, std::string_view prefix)
{
    if (!path.starts_with(prefix)) return no_row;
    path.remove_prefix(prefix.size());
    std::size_t row = 0;
    const auto [end, error] = std::from_chars(path.data(), path.data() + path.size(), row);
    return error == std::errc{} && end == path.data() + path.size() ? row : no_row;
}

// 0.33: rows of a game's patch list: its patches (path "@patch:<row>"), then the import and download rows. A null
// list is still being read. First, once the list is read, the game's cheats ("@cheats:open", the list R2 on the game
// opens) with the number switched on ('cheats_on' < 0: no row).
std::vector<xmb_entry> patch_entries(const utils::ps4::patches::game_patches* game, const patch_icons& icons, int cheats_on)
{
    namespace patches = utils::ps4::patches;
    std::vector<xmb_entry> entries;
    if (!game)
    {
        entries.push_back({{}, i18n::tr(str::LAU_LENDO_PATCHES), i18n::tr(str::LAU_LENDO_PATCHES_HELP), "item_tex_ps3util", {}});
        return entries;
    }
    if (cheats_on >= 0)
    {
        const auto [title, count] = first_part(i18n::trf(str::CHT_R1_LINHA, {cheats_on}));
        entries.push_back({"@cheats:open", title, count + i18n::tr(str::CHT_DICA_JOGO), "item_tex_ps3util", {}});
    }
    const std::string version = game->app_version.empty() ? std::string(i18n::tr(str::COM_VERSAO_DESCONHECIDA)) : "v" + game->app_version;
    for (std::size_t i = 0; i < game->rows.size(); ++i)
    {
        const auto& row = game->rows[i];
        const bool partial = !row.auto_on && row.partial();
        std::vector<std::string> parts{i18n::tr(row.active() ? str::COM_LIGADO : partial ? str::LAU_PATCH_LIGADO_EM_PARTE : str::COM_DESLIGADO)};
        // 0.34: right after the state, so the subtitle clip never hides it
        if (row.recommendation == utils::ps4::recommended::tier::automatic) parts.emplace_back(i18n::tr(str::LAU_PATCH_RECOMENDADO_PS4));
        else if (row.recommendation == utils::ps4::recommended::tier::suggested) parts.emplace_back(i18n::tr(str::LAU_PATCH_SUGERIDO_PS4));
        if (row.global()) parts.emplace_back(i18n::tr(str::LAU_PATCH_TODOS_JOGOS));
        if (row.version_key == patch_key::all) parts.emplace_back(i18n::tr(str::LAU_PATCH_TODAS_VERSOES));
        else if (game->app_version.empty()) parts.push_back("v" + row.version_key);
        if (!row.author.empty()) parts.push_back(i18n::trf(str::LAU_PATCH_AUTOR, {patches::flatten(row.author)}));
        if (!row.patch_version.empty()) parts.push_back("patch v" + row.patch_version);
        if (row.configurable) parts.push_back(i18n::trf(str::LAU_PATCH_VALORES_AJUSTAVEIS_PC, {row.configurable}));
        if (!row.notes.empty() || !row.why.empty()) parts.emplace_back(i18n::tr(str::LAU_PATCH_QUADRADO_NOTAS));
        xmb_entry entry{fmt::format("@patch:%u", i), patches::clip(patches::flatten(row.description), 64), patches::clip(fmt::merge(parts, " · "), line_bytes(100)),
            "item_tex_ps3util", row.active() ? icons.on : partial ? icons.partial : icons.off};
        entries.push_back(std::move(entry));
    }
    if (game->rows.empty())
        entries.push_back({{}, i18n::tr(str::COM_NENHUM_PATCH_ESTE_JOGO), i18n::trf(str::LAU_PATCH_NENHUM_HELP, {game->serial, version}), "item_tex_ps3util", {}});
    entries.push_back({"@patches:import", i18n::tr(str::LAU_IMPORTAR_PATCHES), patch_import_help(), "item_tex_disc_install", {}});
    entries.push_back({"@patches:download", i18n::tr(str::COM_BAIXAR_PATCHES_RPCS3), patch_download_help(), "item_tex_disc_install", {}});
    return entries;
}

// Names of the switch images in the scene's resources: the cheat rows show them by name (one copy for every row)
constexpr char cheat_switch_on[] = "@switch_on", cheat_switch_off[] = "@switch_off";
// System keyboard sizes of the cheat forms (characters)
constexpr unsigned cheat_name_chars = 64, cheat_code_chars = 2048;
// The order the mode option steps through
constexpr std::array<utils::ps4::cheats::mode, 4> cheat_modes{utils::ps4::cheats::mode::automatic, utils::ps4::cheats::mode::constant,
    utils::ps4::cheats::mode::once, utils::ps4::cheats::mode::boot};

// The note of a cheat whose file names another version than the installed one (the list's header may lack room for it)
std::string cheat_version_note(const utils::ps4::cheats::row& row, const utils::ps4::cheats::game_cheats& game)
{
    namespace cheats = utils::ps4::cheats;
    const std::string installed = cheats::detail::fold_version(game.app_version);
    if (row.version.empty() || installed.empty() || cheats::detail::fold_version(row.version) == installed) return {};
    return " · " + i18n::trf(str::CHT_VERSAO_DIFERENTE, {row.version, game.app_version});
}

// Rows of a game's cheat list (R2 on the game, or the first row of its patch list): its cheats (path "@cheat:<row>",
// the switch by name; an invalid cheat keeps the plain icon and says its first problem; codes for another version say
// so), the folders when there is none, then a cheat typed on the system keyboard and the import. A null list is still
// being read.
std::vector<xmb_entry> cheat_entries(const utils::ps4::cheats::game_cheats* game)
{
    namespace cheats = utils::ps4::cheats;
    namespace patches = utils::ps4::patches;
    std::vector<xmb_entry> entries;
    if (!game)
    {
        entries.push_back({{}, i18n::tr(str::CHT_LER), "", "item_tex_ps3util", {}});
        return entries;
    }
    for (std::size_t i = 0; i < game->rows.size(); ++i)
    {
        const cheats::row& row = game->rows[i];
        const char* icon = !row.valid ? "item_tex_ps3util" : row.on ? cheat_switch_on : cheat_switch_off;
        entries.push_back({fmt::format("@cheat:%u", i), patches::clip(patches::flatten(row.name), 64),
            patches::clip(cheats::row_subtitle(row, *game) + cheat_version_note(row, *game), line_bytes(200)), icon, {}});
    }
    if (game->rows.empty())
    {
        const auto [title, folders] = first_part(i18n::tr(str::CHT_NENHUM));
        entries.push_back({"@cheats:none", title, folders, "item_tex_ps3util", {}});
    }
    entries.push_back({"@cheats:add", i18n::tr(str::CHT_ADICIONAR), i18n::tr(str::CHT_TECLADO_CODIGOS), "item_tex_ps3util", {}});
    entries.push_back({"@cheats:import", i18n::tr(str::CHT_IMPORTAR_JOGO), i18n::tr(str::CHT_IMPORTAR_HELP), "item_tex_disc_install", {}});
    return entries;
}

// Square on a cheat: its options (path "@copt:<what>"), those the cheat allows: mode and interval (X and Right step
// forward, Left back), the value of a PC entry, its lines, and editing, renaming and deleting the console's own cheats
// (an imported one is copied to them first)
std::vector<xmb_entry> cheat_option_entries(const utils::ps4::cheats::game_cheats& game, std::size_t index)
{
    namespace cheats = utils::ps4::cheats;
    namespace patches = utils::ps4::patches;
    std::vector<xmb_entry> entries;
    if (index >= game.rows.size()) return entries;
    const cheats::row& row = game.rows[index];
    const auto add = [&](const char* path, std::string title, std::string help)
    {
        entries.push_back({path, std::move(title), patches::clip(std::move(help), line_bytes(200)), "tex_sysconf", {}});
    };
    // the mode: what the cheat does with it now, then the modes X steps through
    std::string modes;
    for (const cheats::mode m : cheat_modes) modes += (modes.empty() ? "" : " / ") + std::string(i18n::tr(cheats::mode_name(m)));
    add("@copt:mode", i18n::trf(str::CFGU_CHANGED, {str::CHT_OPC_MODO, cheats::mode_name(row.wanted)}), cheats::row_subtitle(row, game) + " · " + modes);
    // the interval of a cheat that is rewritten (automatic is constant unless it resolved to the game start or once)
    if (row.wanted == cheats::mode::constant ||
        (row.wanted == cheats::mode::automatic && row.resolved != cheats::mode::boot && row.resolved != cheats::mode::once))
    {
        std::string periods;
        for (const u32 ms : cheats::periods_ms) periods += (periods.empty() ? "" : " / ") + std::to_string(ms);
        add("@copt:period", i18n::trf(str::CFGU_CHANGED, {str::CHT_OPC_INTERVALO, i18n::raw(i18n::trf(str::CHT_INTERVALO_MS, {row.period_ms}))}),
            i18n::trf(str::CHT_INTERVALO_MS, {periods}));
    }
    if (row.pc)
        add("@copt:value", row.value.empty() ? std::string(i18n::tr(str::CHT_OPC_VALOR)) : i18n::trf(str::CFGU_CHANGED, {str::CHT_OPC_VALOR, row.value}),
            i18n::trf(str::CHT_TECLADO_VALOR, {patches::clip(patches::flatten(row.name), 48), cheats::pc_type_names[std::size_t(row.pc_info.type)]}));
    add("@copt:lines", i18n::tr(str::CHT_OPC_VER_LINHAS), first_part(i18n::trf(str::CHT_N_LINHAS, {static_cast<unsigned>(row.lines.size())})).second);
    if (row.user)
    {
        add("@copt:edit", i18n::tr(str::CHT_OPC_EDITAR), i18n::tr(str::CHT_TECLADO_CODIGOS));
        add("@copt:rename", i18n::tr(str::CHT_OPC_RENOMEAR), i18n::tr(str::CHT_TECLADO_NOME));
        add("@copt:delete", i18n::tr(str::CHT_OPC_APAGAR), row.source);
    }
    else add("@copt:copy", i18n::tr(str::CHT_OPC_COPIAR), i18n::tr(str::CHT_ERR_SO_MEUS));
    return entries;
}

// 'Ver linhas' of a cheat as a PS3 dialog, 8 entries a page: its code lines, then its problems, notes and executables
// (X pages on, Circle closes)
xmb_dialog cheat_lines_dialog(const utils::ps4::cheats::game_cheats& game, std::size_t index, std::size_t page, std::size_t& pages)
{
    namespace cheats = utils::ps4::cheats;
    namespace patches = utils::ps4::patches;
    namespace xmb = utils::ps4::xmb;
    constexpr std::size_t per_page = 8;
    xmb_dialog d;
    pages = 1;
    if (index >= game.rows.size()) return d;
    const cheats::row& row = game.rows[index];
    const std::vector<std::string> details = cheats::row_details(row, game);
    pages = std::max<std::size_t>(1, (details.size() + per_page - 1) / per_page);
    page = std::min(page, pages - 1);
    d.title = patches::clip(patches::flatten(row.name), 64);
    if (pages > 1) d.title += fmt::format(" (%u/%u)", page + 1, pages);
    for (std::size_t i = page * per_page; i < details.size() && i < (page + 1) * per_page; ++i)
    {
        // a long value is one word the dialog cannot wrap: cut to the band; problems and notes wrap
        if (i < row.lines.size()) d.lines.push_back(patches::clip(details[i], 72));
        else d.notes.push_back(patches::clip(patches::flatten(details[i]), line_bytes(240)));
    }
    if (page + 1 < pages) d.buttons = {xmb::cross_button(i18n::tr(str::COM_CONTINUAR)), xmb::circle_button(i18n::tr(str::COM_VOLTAR))};
    else d.buttons = {xmb::cross_button(i18n::tr(str::COM_OK))};
    return d;
}

// The line shown when a cheat list arrives: whose cheats, then the lines under the title (the RPCN warning first, codes
// for another version, executables seen; else the title ID and the installed version) and a file that could not be
// read, each one only while the whole line fits (the cheats for another version say so in their own rows too)
std::string cheat_header(const utils::ps4::cheats::game_cheats& game, bool rpcn_online)
{
    namespace cheats = utils::ps4::cheats;
    std::vector<std::string> lines = cheats::header_lines(game, rpcn_online);
    if (const auto online = std::find(lines.begin(), lines.end(), i18n::tr(str::CHT_AVISO_ONLINE)); online != lines.end())
        std::rotate(lines.begin(), online, online + 1);
    if (lines.empty())
        lines.push_back(game.serial + " " + (game.app_version.empty() ? std::string(i18n::tr(str::COM_VERSAO_DESCONHECIDA)) : "v" + game.app_version));
    for (const cheats::problem& note : game.notes)
        if (note.id == str::CHT_ERR_ARQUIVO_GRANDE || note.id == str::CHT_ERRO_LER || note.id == str::CHT_ERR_MUITOS)
        {
            lines.push_back(cheats::problem_text(note));
            break;
        }
    std::string text = utils::ps4::patches::clip(i18n::trf(str::CHT_LISTA, {game.title.empty() ? game.serial : game.title}), line_bytes(60));
    for (const std::string& line : lines)
        if (text.size() + line.size() + 4 <= line_bytes(110)) text += " · " + line;
    return text;
}

// 0.33: the patch.yml download on its own thread (stx::try_launch), so neither the menu nor the loader waits for it.
struct patch_download_worker
{
    utils::ps4::patches::download_state* state = nullptr;
    void operator()() { utils::ps4::patches::download(*state); }
};

// 0.33: PS3 firmware jobs (check of the PS3UPDAT.PUP, install, removal of leftovers) on their own thread
// (stx::try_launch). An install reads ~200 MB and writes the whole dev_flash: neither the menu nor the loader waits.
struct firmware_worker
{
    utils::ps4::firmware::job* job = nullptr;
    void operator()() { utils::ps4::firmware::run(*job); }
};

const char* firmware_missing_hint() { return i18n::tr(str::LAU_FIRMWARE_NAO_INSTALADO_HINT); }

// Subtitle of 'Instalar firmware do PS3'.
std::string firmware_row_help(const utils::ps4::firmware::installed_state& state)
{
    if (state.ready()) return i18n::trf(str::LAU_FIRMWARE_ROW_ATUAL, {state.version});
    if (!state.version.empty()) return i18n::trf(str::LAU_FIRMWARE_ROW_INCOMPLETO, {state.version});
    return i18n::tr(str::LAU_FIRMWARE_ROW_NENHUM);
}

// Firmware part of the 'Informações do sistema' line.
std::string firmware_info_text(const utils::ps4::firmware::installed_state& state)
{
    if (state.ready()) return i18n::trf(str::LAU_FIRMWARE_VERSAO, {state.version});
    return state.version.empty() ? std::string(i18n::tr(str::LAU_SEM_FIRMWARE_PS3)) : i18n::trf(str::LAU_FIRMWARE_VERSAO_INCOMPLETO, {state.version});
}

// Progress line of a running check or install ('version': the PUP being installed).
std::string firmware_progress_text(const utils::ps4::firmware::job& job, const std::string& version)
{
    using utils::ps4::firmware::stage;
    const stage current = job.current();
    // A stop set too late for the install (the swap had begun) changes nothing: the stage line stays.
    if (job.stop.load() && utils::ps4::firmware::can_cancel(current)) return i18n::tr(str::LAU_FIRMWARE_CANCELANDO);
    const std::string name = version.empty() ? std::string(i18n::tr(str::LAU_FIRMWARE_NOME)) : i18n::trf(str::LAU_FIRMWARE_NOME_VERSAO, {version});
    switch (current)
    {
    case stage::searching: return i18n::tr(str::COM_PROCURANDO_PS3UPDAT_PUP_PENDRIVE_PS3);
    case stage::verifying: return i18n::trf(str::LAU_FIRMWARE_VERIFICANDO, {job.shown()});
    case stage::reading:
        return i18n::trf(str::LAU_FIRMWARE_COPIANDO_PUP, {i18n::raw(name), job.mib.load(std::memory_order_relaxed), job.mib_total.load(std::memory_order_relaxed)});
    case stage::extracting:
        return i18n::trf(str::LAU_FIRMWARE_PACOTES, {job.step.load(std::memory_order_relaxed), job.steps.load(std::memory_order_relaxed)});
    case stage::flushing:
        return i18n::trf(str::LAU_FIRMWARE_PACOTES_GRAVANDO, {job.step.load(std::memory_order_relaxed), job.steps.load(std::memory_order_relaxed)});
    case stage::swapping: return i18n::trf(str::LAU_FIRMWARE_TROCANDO_DEV_FLASH, {i18n::raw(name)});
    case stage::removing_staging: return i18n::trf(str::LAU_FIRMWARE_APAGANDO_TEMPORARIOS, {i18n::raw(name)});
    default: return {}; // not started yet: the line set when the job was started stays
    }
}

// 0.37: a PS3 game disc in the PS4 drive. The drive is read only through the console's own GoldHEN FTP server
// (loopback); the disc is copied once into /data/rpcs3/games, decrypted when its key is found, and then plays as an
// ISO. The XMB polls the drive and the key search, which never wait; the copy runs on a thread of its own.
namespace disc_ui
{
namespace ds = utils::ps4::disc_stream;
namespace di = utils::ps4::disc_install;
namespace dk = utils::ps4::disc_key;

// Copy rate assumed until an install has measured the drive (bytes per second)
constexpr u64 assumed_rate = 15'000'000;
const char* cancel_text() { return i18n::tr(str::LAU_DISC_CANCELAR_INSTALACAO); }
const char* exit_text() { return i18n::tr(str::LAU_DISC_SAIR_INTERROMPE); }
const char* boot_refused_text() { return i18n::tr(str::LAU_DISC_AGUARDE_INSTALACAO); }

// What the XMB keeps between its visits (the drive, the key search and an install outlive it)
struct memory
{
    std::string asked_id;              // disc the last key search was started for
    std::string key_id;                // disc of the last finished key search
    bool key_ok = false;               // a verified key is in place, or none is needed
    std::string key_text;              // its result line (interface language)
    u64 rate = 0;                      // copy rate measured by the last install
    std::shared_ptr<di::job> reported; // finished install whose result was shown
    // Padlock state last logged per TITLE_ID of a game installed from a disc (disc_lock)
    std::map<std::string, utils::ps4::disc_lock::match, std::less<>> lock_logged;
};

memory& remembered()
{
    static memory m;
    return m;
}

bool running(const std::shared_ptr<di::job>& job)
{
    return job && !job->finished.load(std::memory_order_acquire);
}

std::string disc_id(const ds::disc_status& disc)
{
    return ds::canonical_title_id(disc.title_id);
}

enum class key_state : u8 { unknown, searching, ready, failed };

key_state key_of(const ds::disc_status& disc)
{
    if (dk::service::instance().busy()) return key_state::searching;
    const memory& m = remembered();
    if (!m.key_id.empty() && m.key_id == disc_id(disc)) return m.key_ok ? key_state::ready : key_state::failed;
    return disc.key_present ? key_state::ready : key_state::unknown;
}

// "FIFA 14 (BLES01876)"
std::string disc_name(const ds::disc_status& disc)
{
    const std::string id = disc_id(disc);
    std::string name = di::short_title(disc);
    if (!disc.title.empty() && !id.empty()) name += " (" + id + ")";
    one_line(name);
    return name;
}

u64 minutes_for(u64 bytes, u64 rate)
{
    return bytes / std::max<u64>(rate, 1) / 60 + 1;
}

// The row's line while the disc installs (the message line has the longer one)
std::string row_progress(const di::job& job)
{
    const u64 done = job.done.load(std::memory_order_acquire), total = job.total.load(std::memory_order_acquire);
    const u64 rate = job.rate.load(std::memory_order_acquire);
    switch (job.current())
    {
    case di::phase::starting:
    case di::phase::checking:
        return i18n::tr(job.key_search.load(std::memory_order_acquire) ? str::LAU_DISC_ROW_BUSCANDO_CHAVE : str::LAU_DISC_ROW_LENDO_DISCO);
    case di::phase::resuming:
        return i18n::trf(str::LAU_DISC_ROW_CONFERINDO_COPIADO, {di::percent(done, total)});
    case di::phase::copying:
        if (!rate) return i18n::trf(str::LAU_DISC_ROW_COPIANDO, {di::size_text(done), di::size_text(total), di::percent(done, total)});
        return i18n::trf(str::LAU_DISC_ROW_COPIANDO_FALTAM, {di::size_text(done), di::size_text(total), di::percent(done, total),
            minutes_for(total > done ? total - done : 0, rate)});
    case di::phase::verifying: return i18n::tr(str::LAU_DISC_ROW_CONFERINDO_IMAGEM);
    case di::phase::finished: break;
    }
    return i18n::tr(str::LAU_DISC_ROW_INSTALANDO);
}

struct row_texts
{
    std::string label, help;
    bool library = false; // also listed ahead of the games
};

row_texts texts(const ds::disc_status& disc, const std::shared_ptr<di::job>& job)
{
    if (running(job)) return {i18n::trf(str::LAU_DISC_ROW_TITULO, {disc_name(job->disc())}), row_progress(*job), true};
    if (disc.state != ds::disc_state::ps3_disc)
    {
        di::row_text row = di::drive_row(disc, nullptr);
        if (disc.state == ds::disc_state::unknown) row.label = i18n::tr(str::COM_DISCO_PS3_LEITOR_PS4);
        // Without the FTP server no disc can be seen at all: the game column says so too
        return {std::move(row.label), std::move(row.help), disc.state == ds::disc_state::no_server};
    }
    std::string help;
    if (!disc.partial.empty())
        help = i18n::trf(str::LAU_DISC_ROW_JA_COPIADOS, {di::size_text(disc.partial_bytes), di::size_text(disc.size)});
    else if (!disc.installed.empty())
        help = i18n::tr(str::LAU_DISC_ROW_JA_INSTALADO);
    else
        help = i18n::trf(str::LAU_DISC_ROW_INSTALAR, {di::size_text(disc.size)});
    const key_state key = key_of(disc);
    if (key == key_state::searching) help += i18n::tr(str::LAU_DISC_ROW_BUSCANDO_CHAVE_SUFIXO);
    else if (key == key_state::failed) help += i18n::tr(str::LAU_DISC_ROW_SEM_CHAVE_SUFIXO);
    return {i18n::trf(str::LAU_DISC_ROW_TITULO, {disc_name(disc)}), std::move(help), true};
}

// Puts the drive row into the storage column (always there) and into the game column (while a PS3 disc is in the
// drive or installs), keeping the selected game where it is. True when the game column gained or lost the row.
bool apply_rows(std::vector<xmb_entry>& games, std::size_t& selected, std::vector<xmb_entry>& storage, const row_texts& t)
{
    if (const std::size_t row = find_path(storage, disc_row_path); row != no_row)
    {
        storage[row].title = t.label;
        storage[row].subtitle = t.help;
    }
    const std::size_t at = find_path(games, disc_row_path);
    if (t.library && at != no_row)
    {
        games[at].title = t.label;
        games[at].subtitle = t.help;
        return false;
    }
    if (t.library)
    {
        // After the data utility and the license import
        const std::size_t position = std::min<std::size_t>(2, games.size());
        games.insert(games.begin() + static_cast<std::ptrdiff_t>(position), xmb_entry{disc_row_path, t.label, t.help, "item_tex_disc_install", {}});
        if (selected >= position && selected + 1 < games.size()) ++selected;
        return true;
    }
    if (at == no_row) return false;
    games.erase(games.begin() + static_cast<std::ptrdiff_t>(at));
    if (selected > at) --selected;
    return true;
}

// play: the disc lock screen found the game's disc (disc_lock)
enum class answer : u8 { install, back, gone, discard, shutdown, play };

// The state of one question
struct question
{
    bool again = false;         // the disc is installed: install it once more
    bool want = false;          // X while the key search runs: install when it ends
    bool discard_asked = false; // Square once on an interrupted copy
};

// The install question about 'disc' (a PS3 system dialog). 'key_text': the key search's result for this disc;
// 'free_bytes' < 0: unknown; 'icon': the disc install icon of the firmware, or null.
xmb_dialog question_dialog(const ds::disc_status& disc, const question& q, key_state key, const std::string& key_text, s64 free_bytes, u64 rate,
    const xmb_image* icon)
{
    namespace xmb = utils::ps4::xmb;
    const bool resume = !disc.partial.empty();
    const u64 copied = resume ? std::min(disc.partial_bytes, disc.size) : 0;
    const u64 left = disc.size - copied;
    const u64 need = left + (1ull << 30); // what the install asks for (the disc install's free margin)
    const bool waiting = q.want && key == key_state::searching;
    xmb_dialog d;
    d.icon = icon;
    d.title = shorten(disc_name(disc), 70);
    d.lines.emplace_back(i18n::tr(resume ? str::LAU_DISC_Q_CONTINUAR : q.again ? str::LAU_DISC_Q_DE_NOVO : str::LAU_DISC_Q_INSTALAR));
    d.notes.push_back(resume ? i18n::trf(str::LAU_DISC_Q_FALTAM, {di::size_text(left), di::size_text(disc.size), minutes_for(left, rate)})
                             : i18n::trf(str::LAU_DISC_Q_TAMANHO, {di::size_text(disc.size), minutes_for(left, rate)}));
    d.notes.emplace_back(i18n::tr(str::LAU_DISC_Q_DESTINO));
    if (free_bytes >= 0)
    {
        const u64 free = static_cast<u64>(free_bytes);
        d.notes.push_back(free >= need ? i18n::trf(str::LAU_DISC_Q_ESPACO_LIVRE, {di::size_text(free)})
                                       : i18n::trf(str::LAU_DISC_Q_POUCO_ESPACO, {di::size_text(free), di::size_text(need)}));
    }
    switch (key)
    {
    case key_state::searching: d.notes.emplace_back(i18n::tr(str::LAU_DISC_Q_CHAVE_PROCURANDO)); break;
    case key_state::ready: d.notes.push_back(key_text.empty() ? std::string(i18n::tr(str::LAU_DISC_Q_CHAVE_PRONTA)) : key_text); break;
    case key_state::failed:
        d.notes.push_back(key_text.empty() ? std::string(i18n::tr(str::LAU_DISC_Q_CHAVE_NAO_ENCONTRADA)) : key_text);
        d.notes.emplace_back(i18n::tr(str::LAU_DISC_Q_SEM_CHAVE_NOTA));
        break;
    case key_state::unknown: d.notes.emplace_back(i18n::tr(str::LAU_DISC_Q_CHAVE_INSTALACAO_PROCURA)); break;
    }
    if (waiting)
    {
        d.lines.emplace_back(i18n::tr(str::LAU_DISC_Q_AGUARDANDO_CHAVE));
        d.busy = true;
    }
    if (q.discard_asked) d.lines.push_back(i18n::trf(str::LAU_DISC_Q_APAGAR_COPIADOS, {di::size_text(copied)}));
    if (!waiting) d.buttons.push_back(xmb::cross_button(i18n::tr(key == key_state::failed ? str::LAU_DISC_Q_INSTALAR_SEM_CHAVE : resume ? str::COM_CONTINUAR : str::COM_INSTALAR)));
    if (resume) d.buttons.push_back(xmb::square_button(i18n::tr(q.discard_asked ? str::COM_APAGAR : str::LAU_DISC_Q_APAGAR_COPIA)));
    d.buttons.push_back(xmb::circle_button(i18n::tr(str::COM_VOLTAR)));
    return d;
}

// A system dialog on the whole screen until 'step' answers. 'step' gets the buttons pressed since its last call (none
// during the first 400 ms: the X that opened it) and fills the dialog. Frames are drawn about 30 times a second, never
// while the system UI is in front; the waves go on from the XMB's ('waves_since': the XMB's start).
answer ask(launcher_display& display, utils::ps4::xmb::scene& backdrop, u64 waves_since, launcher_input& input,
    const std::function<std::optional<answer>(u32 pressed, xmb_dialog& shown)>& step)
{
    const u64 start = sceKernelGetProcessTime();
    bool background = false;
    for (;;)
    {
        if (ps4_shutdown_requested()) return answer::shutdown;
        ps4_poll_system_service();
        const u64 now = sceKernelGetProcessTime();
        if (ps4_in_background())
        {
            background = true;
            sceKernelUsleep(50'000);
            continue;
        }
        if (std::exchange(background, false)) input.flush();
        u32 pressed = input.poll(now, false);
        if (now - start < 400'000) pressed = 0;
        xmb_dialog shown;
        if (const std::optional<answer> done = step(pressed, shown)) return *done;
        draw_dialog_frame(display, backdrop, shown, double(now - waves_since) / 1'000'000, now - start);
        display.present();
        sceKernelUsleep(dialog_frame_us);
    }
}

// The copy of a disc on the whole screen: progress while it runs, then its result. X closes it (the copy goes on, the
// XMB row and line show it); Circle asks whether to cancel (X yes, Circle no).
struct copy_screen
{
    bool cancel_asked = false;
    bool cancel_sent = false;
};

// 0.39: the result line; a write the console refused (EACCES/EPERM) names the folder instead of the generic failure
std::string install_result_text(const di::job& job)
{
    namespace fr = utils::ps4::fs_repair;
    if (job.result() == di::outcome::write_failed)
        if (const std::string detail = job.detail(); fr::permission_detail(detail))
            return fr::no_write_text(fr::folder_in_detail(detail, "/data/rpcs3/games"));
    return di::result_text(job);
}

xmb_dialog copy_dialog(const di::job& job, const copy_screen& screen, const xmb_image* icon)
{
    namespace xmb = utils::ps4::xmb;
    xmb_dialog d;
    d.icon = icon;
    d.title = shorten(disc_name(job.disc()), 70);
    if (job.finished.load(std::memory_order_acquire))
    {
        if (job.result() == di::outcome::done)
        {
            d.lines.emplace_back(i18n::tr(str::COM_INSTALACAO_FOI_CONCLUIDA));
            d.notes.emplace_back(i18n::tr(job.encrypted.load() && !job.decrypting.load() ? str::LAU_DISC_COPY_FALTA_CHAVE : str::LAU_DISC_COPY_JA_NA_LISTA));
        }
        else
        {
            split_into_dialog(install_result_text(job), d);
            if (job.result() != di::outcome::cancelled)
            {
                // A failure as the PKG installer shows it: the outcome first, the reason under it
                d.notes.insert(d.notes.begin(), d.lines.begin(), d.lines.end());
                d.lines = {i18n::tr(str::COM_NAO_FOI_POSSIVEL_CONCLUIR)};
            }
        }
        d.buttons.push_back(xmb::cross_button(i18n::tr(str::COM_OK)));
        return d;
    }
    if (screen.cancel_sent)
    {
        d.lines.emplace_back(i18n::tr(str::LAU_CANCELANDO));
        d.notes.emplace_back(i18n::tr(str::LAU_DISC_COPY_FICA_SALVO));
        d.busy = true;
        return d;
    }
    if (screen.cancel_asked)
    {
        d.lines.emplace_back(i18n::tr(str::COM_DESEJA_CANCELAR_INSTALACAO_DISCO));
        d.notes.emplace_back(i18n::tr(str::COM_JA_FOI_COPIADO_FICA_SALVO));
        d.buttons = {xmb::cross_button(i18n::tr(str::COM_SIM)), xmb::circle_button(i18n::tr(str::COM_NAO))};
        return d;
    }
    const u64 done = job.done.load(std::memory_order_acquire), total = job.total.load(std::memory_order_acquire);
    const u64 rate = job.rate.load(std::memory_order_acquire);
    const int per_mille = total ? static_cast<int>(std::min<u64>(1000, done * 1000 / total)) : 0;
    switch (job.current())
    {
    case di::phase::starting:
    case di::phase::checking:
        d.lines.emplace_back(i18n::tr(job.key_search.load(std::memory_order_acquire) ? str::LAU_DISC_COPY_PROCURANDO_CHAVE : str::LAU_DISC_COPY_LENDO_DISCO));
        d.busy = true;
        break;
    case di::phase::resuming:
        d.lines.emplace_back(i18n::tr(str::LAU_DISC_COPY_CONFERINDO_COPIADO));
        d.progress = per_mille;
        d.progress_left = xmb::percent_text(per_mille);
        break;
    case di::phase::copying:
    {
        d.lines.emplace_back(i18n::tr(str::LAU_DISC_COPY_COPIANDO));
        if (rate)
        {
            std::string speed = fmt::format("%.1f", double(rate) / 1e6);
            if (const std::size_t dot = speed.find('.'); dot != std::string::npos) speed[dot] = di::decimal_mark();
            d.notes.push_back(i18n::trf(str::LAU_DISC_COPY_TAMANHO_VELOCIDADE, {di::size_text(done), di::size_text(total), speed}));
        }
        else d.notes.push_back(i18n::trf(str::LAU_DISC_COPY_TAMANHO, {di::size_text(done), di::size_text(total)}));
        d.progress = per_mille;
        d.progress_left = xmb::percent_text(per_mille);
        d.progress_right = xmb::remaining_text(total > done ? total - done : 0, rate);
        break;
    }
    case di::phase::verifying:
    case di::phase::finished:
        d.lines.emplace_back(i18n::tr(str::LAU_DISC_COPY_CONFERINDO_IMAGEM));
        d.busy = true;
        break;
    }
    d.notes.emplace_back(i18n::tr(str::LAU_DISC_COPY_CONTINUA_MENU));
    d.buttons = {xmb::cross_button(i18n::tr(str::COM_OK)), xmb::circle_button(i18n::tr(str::COM_CANCELAR))};
    return d;
}
} // namespace disc_ui
}

// 0.39 (ps4_launcher.h): an ISO of a disc game without a valid PS3_DISC.SFB, for the boot-failure text
bool ps4_iso_without_sfb(const std::string& path)
{
    if (!iso_file_on_disk(path)) return false;
    iso_archive archive(path);
    return archive.is_valid() && iso_sfb_missing(archive, psf::get_string(archive.open_psf("PS3_GAME/PARAM.SFO"), "CATEGORY", ""));
}

// 0.39: /data/rpcs3/no-fs-repair.flag: no permission repair at start and no folder opened later (0.38 behaviour)
static bool ps4_fs_repair_off()
{
    static const bool off = fs::is_file("/data/rpcs3/no-fs-repair.flag");
    return off;
}

// 0.39: a folder of /data/rpcs3 that refused a write just now (EACCES/EPERM) is opened at once (fs_repair: the app's own
// chmod, or SITE CHMOD through the console's FTP server) instead of at the next start. True when it takes writes now
// (or never refused them); one 'fs perms:' line in startup.log when something was tried.
static bool ps4_reopen_refused_folder(const std::string& folder)
{
    namespace fr = utils::ps4::fs_repair;
    const fr::fs_ops calls = fr::sys::fs();
    std::uint32_t owner = 0;
    const int error = calls.probe(folder, owner);
    if (!error) return true;
    if (!fr::permission_error(error) || ps4_fs_repair_off()) return false;
    std::string line;
    const bool opened = fr::reopen_folder(calls, utils::ps4::disc_stream::sys::net(), fr::sys::server(), folder, line);
    ps4_startup_checkpoint(line.c_str());
    return opened;
}

// 0.26: PSN content (DLC, PSN games) needs its license, a <CONTENT_ID>.rap file, in the emulated user's
// exdata directory (as on the PC RPCS3). Copy every license found in the upload folders or on USB that is not
// there yet. Returns how many were imported.
// 0.34: RPCS3 opens exactly exdata/<CONTENT_ID>.rap and /data is case-sensitive, so the copy is named with the
// upper-case content id and a lower-case .rap whatever the source name ("x.RAP" copied verbatim was never found).
// Only 16-byte files named after a content id are taken; a target of another size (a damaged earlier copy) is
// replaced. .rif files are left out: RPCS3 reads only .rap. Each folder is read with its sub-folders one level deep
// (raps/<game>/, games/<PSN folder>/).
u32 ps4_import_licenses()
{
    const std::string exdata = "/data/rpcs3/dev_hdd0/home/00000001/exdata/";
    // 0.39: a user folder made over FTP refuses the new exdata folder: opened at once (once per process) and tried again
    static std::atomic<bool> s_user_reopen_tried{false};
    if (!fs::create_path(exdata) && !(fs::g_tls_error == fs::error::acces && !s_user_reopen_tried.exchange(true) &&
        ps4_reopen_refused_folder("/data/rpcs3/dev_hdd0/home/00000001") && fs::create_path(exdata)))
        return 0;
    u32 imported = 0, rif = 0;
    std::vector<std::string> roots{"/data/rpcs3/pkgs", "/data/rpcs3/raps", "/data/rpcs3/games", "/data/rpcs3/exdata"};
    // 0.33: every USB port (only read; an absent mount fails at once)
    for (const char* folder : {"/exdata", "/PKG", "/raps"})
        for (unsigned i = 0; i < utils::ps4::usb_drive_count; ++i) roots.push_back(utils::ps4::usb_mount(i) + folder);
    const auto import_folder = [&](const std::string& folder, std::vector<std::string>* subfolders)
    {
        fs::dir dir(folder);
        if (!dir) return;
        for (const auto& item : dir)
        {
            if (item.is_symlink || item.name.empty() || item.name[0] == '.') continue;
            if (item.is_directory)
            {
                if (subfolders) subfolders->push_back(folder + "/" + item.name);
                continue;
            }
            if (item.name.size() < 5) continue;
            const std::string extension = utils::ps4::ascii_lower(std::string_view(item.name).substr(item.name.size() - 4));
            if (extension == ".rif") ++rif;
            if (extension != ".rap") continue;
            const std::string source = folder + "/" + item.name;
            const std::string name = utils::ps4::rap_license_name(item.name);
            if (name.empty() || item.size != 16)
            {
                ps4_launcher_log.warning("License '%s' skipped: a license is a 16-byte <CONTENT_ID>.rap file (%u bytes)", source, item.size);
                continue;
            }
            const std::string target = exdata + name;
            fs::stat_t current{};
            const bool exists = fs::get_stat(target, current);
            if (exists && (current.is_directory || current.size == 16)) continue;
            bool copied = fs::copy_file(source, target, exists);
            // 0.39: exdata refused the write: opened at once and the copy tried again (once per process)
            static std::atomic<bool> s_reopen_tried{false};
            if (!copied && fs::g_tls_error == fs::error::acces && !s_reopen_tried.exchange(true) &&
                ps4_reopen_refused_folder(utils::ps4::fs_repair::parent_folder(target)))
                copied = fs::copy_file(source, target, exists);
            if (copied)
            {
                imported++;
                ps4_launcher_log.success("License imported: %s -> %s%s", source, target, exists ? fmt::format(" (replaced a %u-byte file)", current.size) : std::string{});
            }
            else
            {
                const fs::error error = fs::g_tls_error;
                ps4_launcher_log.error("Cannot import license '%s': %s", source, error);
                // 0.39: a refused permission names the folder, once per process (the scan runs again and again)
                static std::atomic<bool> s_denied_shown{false};
                if (error == fs::error::acces && !s_denied_shown.exchange(true))
                {
                    const std::string denied_in = utils::ps4::fs_repair::parent_folder(target);
                    ps4_startup_checkpoint(fmt::format("licenses: no write permission in %s", denied_in).c_str());
                    ps4_notify(i18n::trf(str::LAU_TOAST_LICENCA_NAO_IMPORTADA, {i18n::raw(utils::ps4::fs_repair::no_write_text(denied_in))}).c_str());
                }
            }
        }
    };
    for (const auto& root : roots)
    {
        std::vector<std::string> subfolders;
        import_folder(root, &subfolders);
        for (const auto& subfolder : subfolders) import_folder(subfolder, nullptr);
    }
    static std::atomic<bool> s_rif_logged{false};
    if (rif && !s_rif_logged.exchange(true))
        ps4_launcher_log.notice("Licenses: %u .rif file(s) ignored: RPCS3 reads only <CONTENT_ID>.rap licenses", rif);
    if (imported) ps4_startup_checkpoint(fmt::format("licenses: %u .rap imported into exdata", imported).c_str());
    return imported;
}

// 0.39: what the start-up permission repair left closed, for one dialog in the XMB (ps4_select_game)
static std::vector<std::string> s_fs_blocked_folders;
static bool s_fs_blocked_more = false;
static bool s_fs_ftp_answered = false;

// 0.39: folders the GoldHEN FTP server made in /data/rpcs3 (uid 0, mode 0755: made with FileZilla before the first start,
// or a copied rpcs3 folder) refuse the app's writes and its chmod, so PKG installs, licenses and the XMB cache fail.
// Before Emu.Init: write probe, bounded walk and repair through the console's own FTP server (fs_repair: SITE CHMOD
// only, paths inside /data/rpcs3 only), 'fs perms:' lines in startup.log.
void ps4_repair_data_permissions()
{
    namespace fr = utils::ps4::fs_repair;
    if (ps4_fs_repair_off())
    {
        ps4_startup_checkpoint("fs perms: permission repair off (no-fs-repair.flag): nothing checked, opened or moved");
        return;
    }
    // A disc install that meets a folder refusing writes opens it at once and tries again
    utils::ps4::disc_install::set_folder_reopen([](const std::string& folder) { return ps4_reopen_refused_folder(folder); });
    const fr::fs_ops fs_calls = fr::sys::fs();
    const utils::ps4::disc_stream::net_ops net = utils::ps4::disc_stream::sys::net();
    const fr::report report = fr::run(fs_calls, net, fr::sys::server(), {}, [](std::size_t count)
    {
        if (count >= 200) ps4_notify(i18n::trf(str::LAU_TOAST_LIBERANDO_PASTAS, {count}).c_str());
    });
    for (const std::string& line : report.log_lines()) ps4_startup_checkpoint(line.c_str());
    if (!report.blocked()) return;
    s_fs_blocked_folders = report.blocked_folders(4);
    s_fs_blocked_more = s_fs_blocked_folders.size() > 3;
    if (s_fs_blocked_more) s_fs_blocked_folders.resize(3);
    s_fs_ftp_answered = report.ftp_answered();
}

// The GoldHEN FTP service uses a different identity from this app, so a folder the app created 0755 refuses its uploads.
// 0.37: every folder the player is told to fill over FTP is created and opened (0777) at boot, which also fixes the
// ones earlier versions created 0755. One startup.log line; a folder the app cannot open (created by the FTP identity
// itself with a closed mode) is named and, once, toasted.
void ps4_prepare_game_directories()
{
    // 0.37: data/disc-lock holds the lock records of games installed from a PS3 disc (disc_lock)
    static constexpr const char* folders[] = {"games", "pkgs", "raps", "import", "patches", "custom_configs", "data", "data/redump", "data/disc-lock", "exdata"};
    std::vector<std::string> ready, problems;
    std::string first_closed;
    for (const char* name : folders)
    {
        const std::string path = std::string("/data/rpcs3/") + name;
        if (!fs::create_path(path))
        {
            ps4_launcher_log.error("Cannot create FTP upload directory '%s': %s", path, fs::g_tls_error);
            problems.push_back(fmt::format("%s (cannot create: %s)", path, fs::g_tls_error));
            continue;
        }
        const int result = sceKernelChmod(path.c_str(), 0777);
        struct ::stat info{};
        if (::stat(path.c_str(), &info) != 0)
        {
            problems.push_back(fmt::format("%s (chmod 0x%x; stat errno %d)", path, static_cast<u32>(result), errno));
            continue;
        }
        if ((info.st_mode & 0777) == 0777)
        {
            ready.emplace_back(name);
            continue;
        }
        ps4_launcher_log.error("Cannot set FTP upload permissions on '%s': 0x%x (mode 0%o uid %u)", path, static_cast<u32>(result),
            static_cast<u32>(info.st_mode & 0777), static_cast<u32>(info.st_uid));
        problems.push_back(fmt::format("%s (mode 0%o uid %u: created by another program)", path, static_cast<u32>(info.st_mode & 0777), static_cast<u32>(info.st_uid)));
        if (first_closed.empty()) first_closed = path;
    }
    ps4_startup_checkpoint(fmt::format("folders: %u ready for FTP (0777): %s%s", ready.size(), fmt::merge(ready, " "),
        problems.empty() ? std::string() : "; not changed: " + fmt::merge(problems, "; ")).c_str());
    // 0.39: the XMB dialog of the start-up repair explains blocked folders; this toast only covers one it missed
    if (!first_closed.empty() && s_fs_blocked_folders.empty())
        ps4_notify(i18n::trf(str::LAU_TOAST_PASTA_CRIADA_FTP, {first_closed}).c_str());
}

// 0.29: one startup.log line per process with the measured CPU clock, so every test session
// says whether Boost Mode was on. Written when 'Informações do sistema' is opened or a game is selected (the
// cached probe costs ~100 ms once, on the launcher thread, before the game boots).
static void ps4_log_cpu_probe_once()
{
    static bool s_logged = false;
    if (!std::exchange(s_logged, true)) ps4_startup_checkpoint(utils::ps4::cpu_probe_log_line().c_str());
}

// 0.34: toasts shown once per process, when the first game is chosen (a toast stays on screen while the game boots,
// the XMB message line does not). A PS4 Pro measured at the base clock gets the Boost Mode hint: about +31 % CPU
// clock, the largest speedup available; Fat and Slim have no Boost Mode. Without the PC per-game settings database
// the toast points to its import row.
static void ps4_game_selection_hints_once()
{
    static bool s_shown = false;
    if (std::exchange(s_shown, true)) return;
    if (utils::ps4::cpu_boost_hint() && utils::ps4::cpu_probe().has_neo == 1)
    {
        ps4_startup_checkpoint("boost: PS4 Pro at base clock (Boost Mode off)");
        ps4_notify((std::string("RPCS3: ") + utils::ps4::cpu_boost_hint_text).c_str()); // the hint is in the interface language
    }
    // 0.37: the boot also reads a copy bundled in the package (/app0), so ask for the copy that is in use
    namespace db = utils::ps4::config_db;
    if (const db::file_info database = db::status(); database.state != db::file_state::loaded)
    {
        ps4_startup_checkpoint(fmt::format("config database: %s when the first game was chosen; toast points to 'Importar config_database.dat do PC'",
            database.state == db::file_state::absent ? "not present" : "not usable").c_str());
        ps4_notify(i18n::tr(str::LAU_TOAST_SEM_AJUSTES_PC));
    }
}

namespace
{
// 0.39: the notice ps4_main.cpp hands to the XMB: the encoded form of util/ps4_xmb_notice.hpp (kind, flags, detail in the
// interface language) or the plain text of 0.38, read through its pt-BR start (both forms show the same dialogs).
struct xmb_incoming
{
    utils::ps4::xmb_notice::kind k = utils::ps4::xmb_notice::kind::text;
    bool encoded = false;
    bool restart_advised = false;
    bool no_reopen = false; // reopening the application does not help this game (incomplete ISO, the same fatal error)
    bool dialog = false;    // a text notice shown as a system dialog (ps4_notice_flag_dialog: a failed boot)
    std::string detail;     // interface language
    std::string line;       // the message line
};

std::string after_first_part(std::string_view text)
{
    const std::size_t cut = text.find(" · ");
    return cut == std::string_view::npos ? std::string{} : std::string(text.substr(cut + std::string_view(" · ").size()));
}

// True when 'text' can be 'pattern' with its {n} filled in: the literal pieces appear in order, the first at the start
bool fills_template(std::string_view text, std::string_view pattern)
{
    std::size_t at = 0;
    for (bool first = true;; first = false)
    {
        const std::size_t brace = pattern.find('{');
        if (const std::string_view piece = pattern.substr(0, brace); !piece.empty())
        {
            const std::size_t found = text.find(piece, at);
            if (found == std::string_view::npos || (first && found != 0)) return false;
            at = found + piece.size();
        }
        if (brace == std::string_view::npos || brace + 3 > pattern.size()) return true;
        pattern.remove_prefix(brace + 3);
    }
}

xmb_incoming read_notice(const std::string& error)
{
    namespace xn = utils::ps4::xmb_notice;
    using xn::kind;
    xmb_incoming in;
    const std::string_view text = error;
    if (xn::is_encoded(text))
    {
        const xn::decoded d = xn::decode(text);
        in.encoded = true;
        in.k = d.k;
        in.restart_advised = d.restart_advised;
        in.detail = d.detail;
        // Flags sit between the kind and the separator; 'n' marks a notice whose game reopening does not help
        if (const std::size_t sep = text.find(xn::separator, 1); sep != std::string_view::npos && sep > 2)
        {
            const std::string_view flags = text.substr(2, sep - 2);
            in.no_reopen = flags.find('n') != std::string_view::npos;
            in.dialog = in.k == kind::text && flags.find(ps4_notice_flag_dialog) != std::string_view::npos && !in.detail.empty();
        }
    }
    else
    {
        in.detail = after_first_part(text);
        if (text.starts_with(utils::ps4::disc_lock::removed_notice)) in.k = kind::disc_removed;
        else if (text.starts_with(i18n::tr(str::COM_OCORREU_ERRO_EMULADOR, i18n::lang::pt_br))) in.k = kind::emulator_error;
        else if (text.starts_with("O emulador precisa reiniciar")) in.k = kind::emulator_restart;
        else if (text.starts_with(ps4_restart_notice)) in.k = kind::clean_process;
        else if (text.starts_with(i18n::tr(str::COM_MEMORIA_VIDEO_ESGOTADA, i18n::lang::pt_br))) in.k = kind::vram_stop;
        else if (text.starts_with(i18n::tr(str::COM_MEMORIA_EMULADOR_ESGOTADA, i18n::lang::pt_br))) in.k = kind::heap_stop;
        else if (text.starts_with(ps4_boot_next_notice)) in.k = kind::boot_next;
        else if (text.find("Firmware is missing") != std::string_view::npos) in.k = kind::firmware_missing;
        else if (text.find("Firmware is too old") != std::string_view::npos) in.k = kind::firmware_old;
        if (in.k == kind::text || in.k == kind::vram_stop || in.k == kind::heap_stop) in.detail = error;
        // The plain form carries the restart advice as pt-BR text (ps4_main.cpp xmb_notice)
        if (in.k == kind::vram_stop || in.k == kind::heap_stop) in.restart_advised = text.find("Recomendado reiniciar") != std::string_view::npos;
        if (in.k == kind::firmware_missing || in.k == kind::firmware_old) in.restart_advised = text.find("reiniciar") != std::string_view::npos;
        if (in.k == kind::emulator_error)
            in.no_reopen = in.detail.find("n\xC3\xA3o resolve") != std::string::npos ||
                in.detail.find(i18n::tr(str::MAIN_ISO_INCOMPLETA_REABRIR_NAO_RESOLVE)) != std::string::npos ||
                fills_template(in.detail, i18n::tr(str::MAIN_FECHOU_NOVO_MESMO_ERRO));
        // In pt-BR (and for a plain line) the text is the message line as it is (0.38 bytes); in another interface
        // language the line is built from the tables below, as for an encoded notice (the plain form starts in pt-BR)
        if (in.k == kind::text || i18n::current() == i18n::lang::pt_br)
        {
            in.line = error;
            return in;
        }
        namespace vram = utils::ps4::vram;
        if (in.k == kind::vram_stop || in.k == kind::heap_stop)
            in.detail = vram::stop_text(in.k == kind::vram_stop ? vram::stop_cause::vram : vram::stop_cause::cpu_heap, i18n::current());
    }
    const auto with = [&](str id) { return in.detail.empty() ? std::string(i18n::tr(id)) : std::string(i18n::tr(id)) + " · " + in.detail; };
    const auto advised = [&](std::string line)
    {
        if (!in.restart_advised) return line;
        const std::string advice = i18n::tr(str::COM_RECOMENDADO_REINICIAR_APLICATIVO_ANTES_JOGAR);
        if (line.empty()) return advice;
        // One 20 px line: a long text is cut so that the advice stays on screen (the rule of the plain form)
        if (line.size() <= line_bytes(48)) return line + " · " + advice;
        return ps4_fatal_detail::utf8_prefix(line, line_bytes(70)) + " · " + i18n::tr(str::MAIN_RECOMENDADO_REINICIAR_APLICATIVO);
    };
    switch (in.k)
    {
    case kind::disc_removed: in.line = in.detail.empty() ? std::string(utils::ps4::disc_lock::removed_text()) : utils::ps4::disc_lock::removed_text() + std::string(" · ") + in.detail; break;
    case kind::emulator_error: in.line = with(str::COM_OCORREU_ERRO_EMULADOR); break;
    case kind::emulator_restart: in.line = with(str::LAU_NOTICE_EMULADOR_PRECISA_REINICIAR_LINHA); break;
    case kind::clean_process: in.line = with(str::LAUH_JOGAR_FECHE_RPCS3_ABRA_NOVO); break;
    case kind::boot_next: in.line = with(str::LAUH_RPCS3_FOI_REABERTO_JOGAR); break;
    case kind::vram_stop: in.line = advised(in.detail.empty() ? std::string(i18n::tr(str::COM_MEMORIA_VIDEO_ESGOTADA)) : in.detail); break;
    case kind::heap_stop: in.line = advised(in.detail.empty() ? std::string(i18n::tr(str::COM_MEMORIA_EMULADOR_ESGOTADA)) : in.detail); break;
    case kind::firmware_missing:
    case kind::firmware_old: break; // the firmware branch of ps4_select_game writes the line
    case kind::text:
    case kind::previous_fatal:
        // A dialog notice keeps its message on the line; the steps are the dialog's notes
        in.line = advised(in.dialog ? in.detail.substr(0, in.detail.find(" · ")) : in.detail);
        break;
    }
    return in;
}

// 0.39: Configurações > Idioma: applies 'next' at once (the PS4 system language stays the base of the automatic value)
// and writes ui-language.txt. True when the file was written; the language applies for this session either way.
bool ps4_set_interface_language(i18n::setting next)
{
    i18n::apply(next, i18n::system_language());
    const std::string token = std::string(i18n::setting_token(next)) + "\n";
    const bool saved = xmb_cache::write_file(i18n::setting_path, token.data(), token.size());
    ps4_startup_checkpoint(saved ? fmt::format("i18n: interface language changed to '%s' (saved)", i18n::setting_token(next)).c_str()
                                 : fmt::format("i18n: interface language changed to '%s' (NOT saved: %s)", i18n::setting_token(next), fs::g_tls_error).c_str());
    return saved;
}

// Font files the overlay font settings can name: /app0/fonts and /data/rpcs3/fonts (the get_font_dirs of ps4_main.cpp),
// read once per process
const std::vector<std::string>& settings_font_names()
{
    static const std::vector<std::string> names = []
    {
        std::set<std::string> found;
        for (const char* folder : {"/app0/fonts/", "/data/rpcs3/fonts/"})
            for (const fs::dir_entry& entry : fs::dir(folder))
            {
                if (entry.is_directory || entry.name.size() < 5) continue;
                std::string extension = entry.name.substr(entry.name.size() - 4);
                for (char& ch : extension) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                if (extension == ".ttf" || extension == ".otf" || extension == ".ttc") found.insert(entry.name);
            }
        return std::vector<std::string>(found.begin(), found.end());
    }();
    return names;
}

// The settings page view: a browser tab of the settings menu (util/ps4_settings.hpp over util/ps4_cfg_browser.hpp) drawn
// as a PS3 settings list (xmb::scene::draw_settings_page). On top of the menu it opens the list of values (X on an
// enum, a composite or a font / country row), the row options (Triangle), the sub-pages (libraries, log levels,
// anaglyph matrices, audio formats, microphone and MIDI slots), the system keyboard for numbers, hexadecimal values,
// texts and the console date, and the PS3 dialogs it asks with: a warned setting or value, leaving with edits that are
// not saved, the full PC help. The menu keeps its own state (tab, cursor, staged values, pins); every edit goes through
// its browser, so the rules, bounds and parsers are the browser's.
class settings_page_view
{
    using str = i18n::str;
    using menu_t = utils::ps4::settings_menu;
    using line_t = utils::ps4::xmb::page_line;
    using sub_page = utils::ps4::cfg_browser::sub_page;
    static constexpr std::size_t none = static_cast<std::size_t>(-1);

    enum class question { none, warn, save, help };
    enum class popup_kind { values, options };
    enum class action { back_auto, upstream, use_global, pin, full_help, yaml_key };
    enum class keyboard { none, row, cell };
    // A selectable line of a sub-page: the library, log channel, matrix cell, format bit or device slot it edits
    struct sub_item
    {
        std::string name;
        std::uint32_t number = 0;
        std::string help;
    };

    std::vector<line_t> m_lines;
    std::vector<std::string> m_help;   // per line
    std::vector<std::size_t> m_list;   // the selectable lines in order (the menu's list rows; the sub-page's items)
    std::vector<sub_item> m_items;     // per selectable line of a sub-page
    bool m_dirty = true;
    int m_built_tab = -1;
    bool m_built_hidden = false, m_built_sub = false;
    std::uint8_t m_built_lang = 0xff;
    // the open sub-page
    sub_page m_sub = sub_page::none;
    std::string m_sub_key, m_sub_title, m_sub_help;
    std::size_t m_sub_cursor = 0;
    // the list over the page
    std::optional<utils::ps4::xmb::page_popup> m_popup;
    popup_kind m_popup_kind = popup_kind::values;
    std::size_t m_popup_row = none;
    std::vector<std::string> m_reasons; // per value of the list: why it is greyed or warned
    std::vector<action> m_actions;      // per entry of the options list
    // the system keyboard
    keyboard m_keyboard = keyboard::none;
    std::size_t m_keyboard_row = none, m_keyboard_cell = 0;
    std::string m_keyboard_label;
    // the question on screen; 'm_retry' repeats the edit a warning held back
    question m_question = question::none;
    std::optional<xmb_dialog> m_dialog;
    std::function<std::string()> m_retry;

    static std::optional<utils::ps4::cfg_catalog::tab> page_tab(const menu_t& menu)
    {
        return menu.page_open() ? menu.current_tab() : std::nullopt;
    }

    std::span<const utils::ps4::cfg_browser::row> rows(menu_t& menu) const
    {
        const auto tab = page_tab(menu);
        if (!tab) return {};
        return menu.browser().page(*tab, menu.show_hidden());
    }

    // The lines of the open tab: one per browser row (the row index is the line index), then Depuração's switch of the
    // hidden settings
    void build_page(menu_t& menu)
    {
        const auto tab = page_tab(menu);
        if (!tab) return;
        const std::uint8_t lang = static_cast<std::uint8_t>(i18n::current());
        if (!m_dirty && !m_built_sub && m_built_tab == int(*tab) && m_built_hidden == menu.show_hidden() && m_built_lang == lang) return;
        m_lines.clear();
        m_help.clear();
        m_list.clear();
        m_items.clear();
        for (const utils::ps4::cfg_browser::row& r : rows(menu))
        {
            line_t l;
            l.label = r.label;
            if (r.section)
            {
                l.caption = true;
                m_lines.push_back(std::move(l));
                m_help.emplace_back();
                continue;
            }
            l.value = r.value;
            l.badge = r.badge_text;
            l.padlock = r.mark == utils::ps4::cfg_browser::badge::ps4;
            l.mine = r.mark == utils::ps4::cfg_browser::badge::mine;
            l.greyed = r.locked || r.dimmed;
            m_list.push_back(m_lines.size());
            m_lines.push_back(std::move(l));
            m_help.push_back(r.note.empty() ? r.help : r.help.empty() ? r.note : r.help + " \xC2\xB7 " + r.note);
        }
        if (*tab == utils::ps4::cfg_catalog::tab::debug)
        {
            line_t l;
            l.label = i18n::tr(str::CFGU_SHOW_HIDDEN);
            l.value = i18n::tr(menu.show_hidden() ? str::COM_LIGADO : str::COM_DESLIGADO);
            m_list.push_back(m_lines.size());
            m_lines.push_back(std::move(l));
            m_help.emplace_back(i18n::tr(str::CFGU_READONLY));
        }
        m_dirty = false;
        m_built_sub = false;
        m_built_tab = int(*tab);
        m_built_hidden = menu.show_hidden();
        m_built_lang = lang;
    }

    void add_item(std::string label, std::string value, sub_item item, bool mine = false, bool greyed = false)
    {
        line_t l;
        l.label = std::move(label);
        l.value = std::move(value);
        if (mine) l.badge = i18n::tr(str::CFGU_BADGE_MINE);
        l.mine = mine;
        l.greyed = greyed;
        m_list.push_back(m_lines.size());
        m_lines.push_back(std::move(l));
        m_help.push_back(item.help);
        m_items.push_back(std::move(item));
    }

    void add_caption(std::string label)
    {
        line_t l;
        l.caption = true;
        l.label = std::move(label);
        m_lines.push_back(std::move(l));
        m_help.emplace_back();
    }

    // The lines of the open sub-page, from the browser's lists
    void build_sub(menu_t& menu)
    {
        namespace cb = utils::ps4::cfg_browser;
        namespace cat = utils::ps4::cfg_catalog;
        const std::uint8_t lang = static_cast<std::uint8_t>(i18n::current());
        if (!m_dirty && m_built_sub && m_built_lang == lang) return;
        m_lines.clear();
        m_help.clear();
        m_list.clear();
        m_items.clear();
        cb::model& browser = menu.browser();
        const std::string on = i18n::tr(str::COM_LIGADO), off = i18n::tr(str::COM_DESLIGADO);
        switch (m_sub)
        {
        case sub_page::libraries:
        {
            // PC's two lists: a checked library runs the other way (LLE by default -> HLE, HLE by default -> LLE)
            const std::vector<cb::library_row> libraries = browser.libraries();
            for (const bool hle : {false, true})
            {
                const std::string section = i18n::tr(hle ? str::CFGU_LIB_HLE_SECTION : str::CFGU_LIB_LLE_SECTION);
                add_caption(section);
                for (const cb::library_row& lib : libraries)
                {
                    if (lib.default_hle != hle) continue;
                    const bool runs_hle = hle != lib.checked;
                    add_item(lib.name, runs_hle ? "HLE" : "LLE", {lib.name, 0, lib.dev_only ? std::string(i18n::tr(str::CFGU_LIB_DEV_ONLY)) : section}, lib.checked);
                }
            }
            break;
        }
        case sub_page::log_levels:
            for (const cb::log_row& channel : browser.log_levels())
                add_item(channel.channel, i18n::tr(channel.label), {channel.channel, 0, m_sub_help}, channel.changed);
            break;
        case sub_page::matrix:
        {
            const std::array<double, 9> cells = browser.matrix(m_sub_key);
            for (std::uint32_t c = 0; c < 9; ++c)
            {
                char value[32];
                std::snprintf(value, sizeof(value), "%.3f", cells[c]);
                std::string text = value;
                while (text.size() > 1 && text.back() == '0' && text.find('.') != std::string::npos) text.pop_back();
                if (!text.empty() && text.back() == '.') text.pop_back();
                add_item(i18n::trf(str::CFGU_MATRIX_CELL, {c / 3 + 1, c % 3 + 1}), text, {{}, c, m_sub_help});
            }
            break;
        }
        case sub_page::audio_formats:
        {
            // The format bits in the catalog's order, with their PC names; the first one is always on
            std::vector<std::string> names;
            for (const cat::value_label& v : cat::value_labels())
                if (v.key == "Audio > Audio Formats") names.emplace_back(v.label != str::count_ ? std::string(i18n::tr(v.label)) : std::string(v.text));
            const auto formats = browser.audio_formats();
            for (std::size_t i = 0; i < formats.size(); ++i)
                add_item(i < names.size() ? names[i] : std::to_string(formats[i].first), formats[i].second ? on : off,
                    {{}, formats[i].first, m_sub_help}, false, formats[i].first == 0);
            break;
        }
        case sub_page::microphones:
        case sub_page::midi:
        {
            const bool midi = m_sub == sub_page::midi;
            const std::vector<cb::slot_row> slots = browser.slots(m_sub_key);
            for (std::uint32_t n = 0; n < slots.size(); ++n)
            {
                const std::string device = slots[n].device.empty() ? std::string(i18n::tr(str::CFGU_NONE)) : slots[n].device;
                std::string value = device;
                if (midi)
                {
                    const cat::value_label* type = cat::find_value(m_sub_key, slots[n].type);
                    const std::string type_text = !type ? slots[n].type : type->label != str::count_ ? std::string(i18n::tr(type->label)) : std::string(type->text);
                    value = (type_text.empty() ? std::string(i18n::tr(str::CFGU_NONE)) : type_text) + " \xC2\xB7 " + device;
                }
                add_item(i18n::trf(str::CFGU_SLOT, {n + 1}), value, {{}, n, m_sub_help});
            }
            break;
        }
        case sub_page::none:
            break;
        }
        m_sub_cursor = m_items.empty() ? 0 : std::min(m_sub_cursor, m_items.size() - 1);
        m_dirty = false;
        m_built_sub = true;
        m_built_lang = lang;
    }

    void build(menu_t& menu)
    {
        if (m_sub != sub_page::none) build_sub(menu);
        else build_page(menu);
    }

    std::size_t selected_line(menu_t& menu) const
    {
        if (m_list.empty()) return 0;
        const std::size_t at = m_sub != sub_page::none ? m_sub_cursor : menu.selection();
        return m_list[std::min(at, m_list.size() - 1)];
    }

    // After an edit: a warned setting or value held it back, so the question opens; X repeats it through 'retry'
    void ask_if_warned(menu_t& menu, std::string& message, const std::string& label, std::function<std::string()> retry)
    {
        if (!menu.browser().confirmation()) return;
        std::string reason = message;
        const std::string head = std::string(i18n::tr(str::CFGU_WARN_TITLE)) + ": ";
        const std::string hint = std::string(" \xC2\xB7 ") + i18n::tr(str::CFGU_WARN_CONFIRM);
        if (reason.starts_with(head)) reason.erase(0, head.size());
        if (reason.ends_with(hint)) reason.erase(reason.size() - hint.size());
        xmb_dialog d;
        d.title = i18n::tr(str::CFGU_WARN_TITLE);
        d.lines.push_back(label);
        if (!reason.empty())
        {
            xmb_dialog parts;
            split_into_dialog(reason, parts);
            for (std::string& part : parts.lines) d.notes.push_back(std::move(part));
            for (std::string& part : parts.notes) d.notes.push_back(std::move(part));
            // every part is a sentence of its own under the setting's name
            if (!d.notes.empty() && d.notes.front().front() >= 'a' && d.notes.front().front() <= 'z')
                d.notes.front().front() = static_cast<char>(d.notes.front().front() - 'a' + 'A');
        }
        d.buttons = {utils::ps4::xmb::cross_button(i18n::tr(str::CFGU_FOOTER_CHANGE)), utils::ps4::xmb::circle_button(i18n::tr(str::COM_CANCELAR))};
        m_dialog = std::move(d);
        m_question = question::warn;
        m_retry = std::move(retry);
        message.clear();
    }

    std::string row_label(menu_t& menu, std::size_t row) const
    {
        const auto page = rows(menu);
        return row < page.size() ? page[row].label : std::string{};
    }

    // Square on a row, and Options > Back to automatic
    void reset_row(menu_t& menu, std::size_t row, std::string& message)
    {
        if (row == none) return;
        if (!menu.reset_option())
        {
            const auto page = rows(menu);
            if (row < page.size()) message = page[row].help;
            return;
        }
        const auto page = rows(menu);
        if (row < page.size()) message = i18n::trf(str::CFGU_RESET_AUTO_DONE, {i18n::raw(page[row].label), i18n::raw(page[row].value)});
    }

    void open_values(menu_t& menu, std::size_t row, const utils::ps4::cfg_browser::row& r)
    {
        const std::vector<utils::ps4::cfg_browser::choice> list = menu.browser().choices(row);
        if (list.empty()) return;
        utils::ps4::xmb::page_popup p;
        p.title = r.label;
        m_reasons.clear();
        for (std::size_t i = 0; i < list.size(); ++i)
        {
            p.choices.push_back({list[i].label, !list[i].allowed, list[i].current});
            m_reasons.push_back(list[i].reason);
            if (list[i].current) p.selected = i;
        }
        m_popup = std::move(p);
        m_popup_kind = popup_kind::values;
        m_popup_row = row;
    }

    void open_options(menu_t& menu, std::size_t row)
    {
        const auto page = rows(menu);
        if (row >= page.size()) return;
        const utils::ps4::cfg_browser::row& r = page[row];
        m_actions.clear();
        if (!r.locked)
        {
            m_actions = {action::back_auto, action::upstream};
            if (menu.game_scope()) m_actions.push_back(action::use_global);
            m_actions.push_back(action::pin);
        }
        m_actions.push_back(action::full_help);
        m_actions.push_back(action::yaml_key);
        utils::ps4::xmb::page_popup p;
        p.title = r.label;
        p.radio = false;
        for (const action a : m_actions)
        {
            const str id = a == action::back_auto ? str::CFGU_OPT_BACK_AUTO : a == action::upstream ? str::CFGU_OPT_UPSTREAM
                : a == action::use_global ? str::CFGU_OPT_USE_GLOBAL : a == action::pin ? (r.pinned ? str::CFGU_OPT_UNPIN : str::CFGU_OPT_PIN)
                : a == action::full_help ? str::CFGU_OPT_FULL_HELP : str::CFGU_OPT_YAML_KEY;
            p.choices.push_back({i18n::tr(id), false, false});
        }
        m_reasons.assign(m_actions.size(), {});
        m_popup = std::move(p);
        m_popup_kind = popup_kind::options;
        m_popup_row = row;
    }

    // X on a number, hexadecimal, text or date row: the system keyboard starts with the value. A number with a sign or
    // decimals uses the Latin keyboard (the number keyboard has digits only).
    void open_keyboard_row(menu_t& menu, std::size_t row, const utils::ps4::cfg_browser::row& r, std::string& message)
    {
        namespace cb = utils::ps4::cfg_browser;
        std::string title, initial;
        const cb::keyboard_kind k = menu.browser().keyboard(row, title, initial);
        ps4_ime::kind kind = ps4_ime::kind::text;
        unsigned limit = 0;
        switch (k)
        {
        case cb::keyboard_kind::number:
        {
            bool latin = false;
            if (r.leaf)
                for (const std::string& bound : r.leaf->to_list()) latin |= bound.find_first_of("-.") != std::string::npos;
            kind = latin ? ps4_ime::kind::hex : ps4_ime::kind::number;
            break;
        }
        case cb::keyboard_kind::hex: kind = ps4_ime::kind::hex; break;
        case cb::keyboard_kind::date: kind = ps4_ime::kind::hex; limit = 16; break;
        // a long text (a swap list) keeps room to grow instead of being cut to the default 256 characters
        case cb::keyboard_kind::text: kind = ps4_ime::kind::text; limit = initial.size() > 192 ? unsigned(std::min<std::size_t>(2048, initial.size() + 256)) : 0; break;
        case cb::keyboard_kind::none: return;
        }
        // Without the keyboard the row keeps its value (Left/Right still step a number); startup.log says why
        if (!ps4_ime::open(kind, title, limit, initial))
        {
            message = i18n::trf(str::CFGU_CHANGED, {i18n::raw(r.label), i18n::raw(r.value)});
            return;
        }
        m_keyboard = keyboard::row;
        m_keyboard_row = row;
        m_keyboard_label = r.label;
        message = title;
    }

    void open_sub(const utils::ps4::cfg_browser::row& r)
    {
        m_sub = r.page;
        m_sub_key = r.key;
        m_sub_title = r.label;
        m_sub_help = r.help;
        m_sub_cursor = 0;
        m_dirty = true;
    }

    void close_question()
    {
        m_dialog.reset();
        m_question = question::none;
    }

    // A press while a question is on screen (only its own buttons act)
    std::optional<bool> answer(std::uint32_t pressed, menu_t& menu, std::string& message)
    {
        switch (m_question)
        {
        case question::warn:
            if (pressed & ORBIS_PAD_BUTTON_CROSS)
            {
                menu.browser().confirm();
                close_question();
                std::function<std::string()> retry = std::move(m_retry);
                m_retry = nullptr;
                if (!retry) break;
                message = retry();
                // a warned value can follow the warned setting
                ask_if_warned(menu, message, m_dialog_label, retry);
            }
            else if (pressed & ORBIS_PAD_BUTTON_CIRCLE)
            {
                if (menu.browser().confirmation()) menu.back();
                close_question();
                m_retry = nullptr;
                message.clear();
            }
            break;
        case question::save:
            if (pressed & ORBIS_PAD_BUTTON_CROSS)
            {
                close_question();
                menu.selection() = 0; // the top level's 'Salvar'
                if (menu.select(message)) return true;
            }
            else if (pressed & ORBIS_PAD_BUTTON_CIRCLE)
            {
                close_question();
                return false;
            }
            else if (pressed & ORBIS_PAD_BUTTON_TRIANGLE)
            {
                close_question();
                message.clear();
            }
            break;
        case question::help:
            if (pressed & (ORBIS_PAD_BUTTON_CROSS | ORBIS_PAD_BUTTON_CIRCLE)) close_question();
            break;
        case question::none:
            break;
        }
        return std::nullopt;
    }

    void popup_press(std::uint32_t pressed, menu_t& menu, std::string& message)
    {
        utils::ps4::xmb::page_popup& p = *m_popup;
        if ((pressed & ORBIS_PAD_BUTTON_DOWN) && p.selected + 1 < p.choices.size()) ++p.selected;
        if ((pressed & ORBIS_PAD_BUTTON_UP) && p.selected > 0) --p.selected;
        if (pressed & ORBIS_PAD_BUTTON_CIRCLE)
        {
            m_popup.reset();
            return;
        }
        if (!(pressed & ORBIS_PAD_BUTTON_CROSS) || p.choices.empty()) return;
        const std::size_t c = p.selected, row = m_popup_row;
        const popup_kind kind = m_popup_kind;
        m_popup.reset();
        const std::string label = row_label(menu, row);
        m_dialog_label = label;
        if (kind == popup_kind::values)
        {
            const auto pick = [&menu, row, c] { return menu.browser().pick(row, c); };
            message = pick();
            ask_if_warned(menu, message, label, pick);
            return;
        }
        if (c >= m_actions.size()) return;
        switch (m_actions[c])
        {
        case action::back_auto: reset_row(menu, row, message); break;
        case action::upstream:
        {
            const auto upstream = [&menu, row] { return menu.browser().reset_upstream(row); };
            message = upstream();
            ask_if_warned(menu, message, label, upstream);
            break;
        }
        case action::use_global: message = menu.browser().use_global(row); break;
        case action::pin: message = menu.browser().toggle_pin(row); break;
        case action::full_help:
        {
            xmb_dialog d;
            d.title = label;
            split_into_dialog(menu.browser().full_help(row), d);
            d.buttons = {utils::ps4::xmb::cross_button(i18n::tr(str::COM_OK))};
            m_dialog = std::move(d);
            m_question = question::help;
            break;
        }
        case action::yaml_key:
            message = i18n::trf(str::CFGU_CHANGED, {str::CFGU_OPT_YAML_KEY, menu.browser().yaml_key(row)});
            break;
        }
    }

    void page_press(std::uint32_t pressed, std::uint32_t held, menu_t& menu, std::string& message)
    {
        namespace cb = utils::ps4::cfg_browser;
        namespace cat = utils::ps4::cfg_catalog;
        const auto tab = page_tab(menu);
        if (!tab) return;
        if (pressed & ORBIS_PAD_BUTTON_CIRCLE)
        {
            menu.back();
            message.clear();
            return;
        }
        if (pressed & (ORBIS_PAD_BUTTON_L2 | ORBIS_PAD_BUTTON_R2))
        {
            const int n = int(cat::tab_count), step = (pressed & ORBIS_PAD_BUTTON_R2) ? 1 : -1;
            menu.open_tab(cat::tab((int(*tab) + step + n) % n));
            message.clear();
            return;
        }
        build_page(menu);
        std::size_t& cursor = menu.selection();
        if (pressed & ORBIS_PAD_BUTTON_DOWN)
        {
            if (cursor + 1 < m_list.size()) ++cursor;
            message.clear();
        }
        if (pressed & ORBIS_PAD_BUTTON_UP)
        {
            if (cursor > 0) --cursor;
            message.clear();
        }
        const std::size_t row = menu.page_row(); // none on Depuração's switch of the hidden settings
        m_dialog_label = row != none ? row_label(menu, row) : std::string{};
        if (pressed & (ORBIS_PAD_BUTTON_LEFT | ORBIS_PAD_BUTTON_RIGHT))
        {
            const int direction = (pressed & ORBIS_PAD_BUTTON_RIGHT) ? 1 : -1;
            const bool coarse = (held & ORBIS_PAD_BUTTON_R1) != 0;
            const auto adjust = [&menu, direction, coarse] { return menu.adjust(direction, coarse); };
            message = adjust();
            ask_if_warned(menu, message, m_dialog_label, adjust);
            return;
        }
        if (pressed & ORBIS_PAD_BUTTON_SQUARE)
        {
            reset_row(menu, row, message);
            return;
        }
        if (pressed & ORBIS_PAD_BUTTON_TRIANGLE)
        {
            if (row != none) open_options(menu, row);
            return;
        }
        if (!(pressed & ORBIS_PAD_BUTTON_CROSS)) return;
        if (row != none)
        {
            const auto page = rows(menu);
            if (row < page.size())
            {
                const cb::row& r = page[row];
                if (!r.locked && !r.dimmed && r.redirect == cb::ps4_switch::none)
                {
                    if (r.page != sub_page::none) { open_sub(r); message.clear(); return; }
                    if (r.picker) { open_values(menu, row, r); return; }
                    if (r.keyboard != cb::keyboard_kind::none) { open_keyboard_row(menu, row, r, message); return; }
                }
            }
        }
        // a switch, a refused row (its reason), a row that follows a PS4 switch (the PS4 tab opens on it)
        const auto select = [&menu] { std::string text; menu.select(text); return text; };
        message = select();
        ask_if_warned(menu, message, m_dialog_label, select);
    }

    void sub_press(std::uint32_t pressed, menu_t& menu, std::string& message)
    {
        build_sub(menu);
        if (pressed & ORBIS_PAD_BUTTON_CIRCLE)
        {
            m_sub = sub_page::none;
            m_dirty = true;
            message.clear();
            return;
        }
        if (pressed & ORBIS_PAD_BUTTON_DOWN)
        {
            if (m_sub_cursor + 1 < m_items.size()) ++m_sub_cursor;
            message.clear();
        }
        if (pressed & ORBIS_PAD_BUTTON_UP)
        {
            if (m_sub_cursor > 0) --m_sub_cursor;
            message.clear();
        }
        if (m_items.empty()) return;
        const sub_item item = m_items[m_sub_cursor];
        const int direction = (pressed & ORBIS_PAD_BUTTON_RIGHT) ? 1 : (pressed & ORBIS_PAD_BUTTON_LEFT) ? -1 : 0;
        const bool cross = (pressed & ORBIS_PAD_BUTTON_CROSS) != 0;
        const std::string key = m_sub_key;
        std::function<std::string()> edit;
        switch (m_sub)
        {
        case sub_page::libraries:
            if (pressed & ORBIS_PAD_BUTTON_SQUARE) edit = [&menu] { return menu.browser().clear_libraries(); };
            else if (direction || cross) edit = [&menu, name = item.name] { return menu.browser().toggle_library(name); };
            break;
        case sub_page::log_levels:
            if (direction || cross) edit = [&menu, channel = item.name, step = direction ? direction : 1] { return menu.browser().step_log(channel, step); };
            break;
        case sub_page::matrix:
            if (direction) edit = [&menu, key, cell = item.number, direction] { return menu.browser().step_matrix(key, cell, direction); };
            else if (cross)
            {
                // the cell on the keyboard: decimals and a sign, so the Latin keyboard
                if (!ps4_ime::open(ps4_ime::kind::hex, m_lines[m_list[m_sub_cursor]].label, 16, m_lines[m_list[m_sub_cursor]].value))
                {
                    message = i18n::trf(str::CFGU_CHANGED, {i18n::raw(m_lines[m_list[m_sub_cursor]].label), i18n::raw(m_lines[m_list[m_sub_cursor]].value)});
                    return;
                }
                m_keyboard = keyboard::cell;
                m_keyboard_cell = item.number;
                m_keyboard_label = m_sub_title;
                message = i18n::trf(str::CFGU_KB_TITLE_NUMBER, {i18n::raw(m_lines[m_list[m_sub_cursor]].label)});
                return;
            }
            break;
        case sub_page::audio_formats:
            if (direction || cross) edit = [&menu, bit = item.number] { return menu.browser().toggle_audio_format(bit); };
            break;
        case sub_page::microphones:
            if (direction || cross) edit = [&menu, key, slot = std::size_t(item.number), step = direction ? direction : 1] { return menu.browser().step_slot(key, slot, 1, step); };
            break;
        case sub_page::midi:
            // Left/Right: the device; X: the device type
            if (direction) edit = [&menu, key, slot = std::size_t(item.number), direction] { return menu.browser().step_slot(key, slot, 1, direction); };
            else if (cross) edit = [&menu, key, slot = std::size_t(item.number)] { return menu.browser().step_slot(key, slot, 0, 1); };
            break;
        case sub_page::none:
            break;
        }
        if (!edit) return;
        message = edit();
        m_dialog_label = m_sub_title;
        ask_if_warned(menu, message, m_sub_title, edit);
    }

    std::string m_dialog_label; // the setting a warning names

public:
    // What the launcher does after a press: nothing more, close the menu after a committed save, or close it without one
    enum class outcome { none, saved, discarded };

    // A new menu session (settings_menu::open)
    void reset() { *this = settings_page_view{}; }
    // Something outside the view changed the menu (a switch, the players, the language): lines are built again
    void touch() { m_dirty = true; }
    bool sub_open() const { return m_sub != sub_page::none; }
    bool keyboard_open() const { return m_keyboard != keyboard::none; }
    // The question on screen, drawn over the waves instead of the page
    const std::optional<xmb_dialog>& dialog() const { return m_dialog; }

    // O at the top level with edits that are not saved
    void ask_save(menu_t& menu)
    {
        xmb_dialog d;
        d.title = menu.scope_text();
        d.lines.emplace_back(i18n::tr(str::CFGU_SAVE_QUESTION));
        d.buttons = {utils::ps4::xmb::cross_button(i18n::tr(str::CFGU_SAVE)), utils::ps4::xmb::circle_button(i18n::tr(str::CFGU_DISCARD)),
            utils::ps4::xmb::triangle_button(i18n::tr(str::CFGU_KEEP_EDITING))};
        m_dialog = std::move(d);
        m_question = question::save;
    }

    // One press while the menu shows a page, a sub-page or a question of this view
    outcome handle(std::uint32_t pressed, std::uint32_t held, menu_t& menu, std::string& message)
    {
        m_dirty = true;
        if (m_question != question::none)
        {
            const std::optional<bool> done = answer(pressed, menu, message);
            return !done ? outcome::none : *done ? outcome::saved : outcome::discarded;
        }
        if (!menu.page_open())
        {
            m_sub = sub_page::none;
            m_popup.reset();
            return outcome::none;
        }
        if (m_popup) popup_press(pressed, menu, message);
        else if (m_sub != sub_page::none) sub_press(pressed, menu, message);
        else page_press(pressed, held, menu, message);
        m_dirty = true; // the edit changed values after the lines were read
        return outcome::none;
    }

    // The keyboard of a row or a matrix cell closed
    void keyboard_done(ps4_ime::state state, const std::string& text, menu_t& menu, std::string& message)
    {
        const keyboard what = std::exchange(m_keyboard, keyboard::none);
        m_dirty = true;
        if (state != ps4_ime::state::done)
        {
            message.clear();
            return;
        }
        m_dialog_label = m_keyboard_label;
        if (what == keyboard::row)
        {
            const auto set = [&menu, row = m_keyboard_row, text] { return menu.browser().set_text(row, text); };
            message = set();
            ask_if_warned(menu, message, m_keyboard_label, set);
        }
        else if (what == keyboard::cell)
        {
            const auto set = [&menu, key = m_sub_key, cell = std::size_t(m_keyboard_cell), text] { return menu.browser().set_matrix_text(key, cell, text); };
            message = set();
            ask_if_warned(menu, message, m_keyboard_label, set);
        }
    }

    // The page (or sub-page) with its list over it, the selected line's help, the message and the footer
    void draw(utils::ps4::xmb::scene& scene, std::uint32_t* pixels, const utils::ps4::xmb::font& typeface, menu_t& menu, double seconds, float elapsed,
        std::string_view clock, const std::string& message)
    {
        namespace xmb = utils::ps4::xmb;
        const auto tab = page_tab(menu);
        if (!tab) return;
        build(menu);
        xmb::settings_page page;
        const std::string tab_name = i18n::tr(utils::ps4::cfg_catalog::tab_name(*tab));
        page.title = m_sub != sub_page::none ? m_sub_title : tab_name;
        page.scope = m_sub != sub_page::none ? tab_name + " \xC2\xB7 " + menu.scope_text() : menu.scope_text();
        page.lines = m_lines;
        page.selected = selected_line(menu);
        if (m_popup && m_popup->selected < m_reasons.size() && !m_reasons[m_popup->selected].empty()) page.help = m_reasons[m_popup->selected];
        else if (page.selected < m_help.size()) page.help = m_help[page.selected];
        page.message = message;
        if (m_popup)
        {
            page.buttons = {{"tex_cross", i18n::tr(str::COM_SELECIONAR)}, {"tex_circle", i18n::tr(str::COM_CANCELAR)}};
            page.popup = &*m_popup;
        }
        else if (m_sub != sub_page::none)
        {
            page.buttons = {{"tex_cross", i18n::tr(str::CFGU_FOOTER_CHANGE)}, {"tex_circle", i18n::tr(str::COM_VOLTAR)}};
            page.footer_help = i18n::tr(m_sub == sub_page::libraries ? str::CFGU_LIB_CLEAR : str::XMB_ESQ_DIR_ALTERAR_R1_ACELERAR);
        }
        else
        {
            page.buttons = {{"tex_cross", i18n::tr(str::CFGU_FOOTER_CHANGE)}, {"tex_circle", i18n::tr(str::COM_VOLTAR)},
                {"tex_square", i18n::tr(str::CFGU_FOOTER_AUTO)}, {"tex_triangle", i18n::tr(str::CFGU_FOOTER_OPTIONS)}};
            page.footer_help = i18n::tr(str::CFGU_FOOTER_HELP);
        }
        scene.draw_settings_page(pixels, typeface, page, seconds, elapsed, clock);
    }
};

// The dynamic settings as the in-game home menu (Options+Touchpad) opened them; its Salvar compares against this
std::mutex s_home_menu_lock;
std::string s_home_menu_snapshot;
} // namespace

// Input/pad_thread.cpp, pad thread: the home menu opens
void ps4_home_menu_opened()
{
    std::string snapshot = utils::ps4::cfg_browser::dynamic_snapshot(g_cfg);
    std::lock_guard lock(s_home_menu_lock);
    s_home_menu_snapshot = std::move(snapshot);
}

// The home menu's Salvar (g_emu_callbacks.save_emu_settings, ps4_main.cpp; the overlay's thread). Only the settings the
// player changed in the menu are written, as their own choices (pins): the running config also holds every value the
// boot policy forced (automatic profile, console and runtime limits, adaptation), which must never reach a file.
// Per game: a format-2 config_<ID>.yml (or none yet) gets those settings merged into its own; a format-1 file (a full
// dump) gets their values in its dump. Without a title: ps4-settings.yml, its full dump with them pinned (format 2 or
// no file yet), or a format-1 file as a game's format-1 file. The running config stays as the menu set it.
void ps4_home_menu_save()
{
    namespace ps4 = utils::ps4;
    namespace cb = utils::ps4::cfg_browser;
    namespace cat = utils::ps4::cfg_catalog;
    std::string snapshot;
    {
        std::lock_guard lock(s_home_menu_lock);
        snapshot = s_home_menu_snapshot;
    }
    const std::string title = Emu.GetTitleID();
    const bool game = ps4::settings_menu::valid_title_id(title);
    const std::string who = game ? title : std::string("all games");
    std::vector<std::string> keys = snapshot.empty() ? std::vector<std::string>{} : cb::dynamic_changes(g_cfg, snapshot);
    // the settings the PS4 keeps at one value, or that have no meaning here, are never the player's
    std::erase_if(keys, [](const std::string& key)
    {
        const cat::item* item = cat::find(key);
        return item && (item->kind == cat::klass::unavailable || item->kind == cat::klass::hidden);
    });
    const auto finish = [&]
    {
        g_backup_cfg.from_string(g_cfg.to_string());
        std::string again = cb::dynamic_snapshot(g_cfg);
        std::lock_guard lock(s_home_menu_lock);
        s_home_menu_snapshot = std::move(again);
    };
    if (keys.empty())
    {
        ps4_startup_checkpoint(fmt::format("home menu: Salvar for %s: no live setting changed since the menu opened; nothing written", who).c_str());
        finish();
        return;
    }
    // The files' values (never the running ones), then the menu's edits over them
    const auto read = [](const std::string& path)
    {
        const fs::file file(path);
        return file ? file.to_string() : std::string{};
    };
    const std::string path = game ? rpcs3::utils::get_custom_config_path(title) : std::string(ps4::global_settings_path);
    const std::string own = read(path);
    const auto saved = std::make_unique<cfg_root>();
    if (const std::string base = read(fs::get_config_dir(true) + "config.yml"); !base.empty()) saved->from_string(base);
    if (game)
        if (const std::string global = read(ps4::global_settings_path); !global.empty()) saved->from_string(global);
    if (!own.empty()) saved->from_string(own);
    for (const std::string& key : keys)
        if (cfg::_base* to = cb::find(*saved, key))
            if (const cfg::_base* from = cb::find(g_cfg, key)) cb::detail::restore(*to, cb::detail::canonical(*from));
    const bool format2 = own.empty() || ps4::saved_settings_format(own) >= ps4::settings_format_version;
    std::vector<std::string> pins;
    std::string text;
    if (format2)
    {
        pins = ps4::pins_of(own);
        for (const std::string& key : keys)
            if (std::find(pins.begin(), pins.end(), key) == pins.end()) pins.push_back(key);
        text = ps4::with_settings_stamp(game ? cb::sparse_yaml_for(*saved, pins) : saved->to_string(), pins);
    }
    else
    {
        // A format-1 file keeps the profile stamp it had (none: older than the stamps), so the values an older profile
        // forced into its dump still migrate at boot (apply_profile_migration); the current stamp would freeze them
        text = saved->to_string();
        if (const unsigned stamp = ps4::saved_profile_version(own))
        {
            if (!text.empty() && text.back() != '\n') text += '\n';
            text += std::string(ps4::profile_stamp_key) + ": " + std::to_string(stamp) + "\n";
        }
    }
    std::string listed;
    for (std::size_t i = 0; i < keys.size() && i < 12; ++i) listed += (i ? ", " : "") + keys[i];
    if (keys.size() > 12) listed += fmt::format(" (+%u)", keys.size() - 12);
    fs::create_path(fs::get_parent_dir(path));
    fs::pending_file output(path);
    if (!output.file || output.file.write(text.data(), text.size()) != text.size() || !output.commit())
    {
        ps4_startup_checkpoint(fmt::format("home menu: could not save %u live setting(s) for %s to '%s' (%s): %s", keys.size(), who, path, fs::g_tls_error, listed).c_str());
        ps4_notify((i18n::tr(str::COM_ERRO_GRAVAR) + path).c_str());
        return;
    }
    ps4_startup_checkpoint(fmt::format("home menu: saved %u live setting(s) for %s (%s, %s): %s", keys.size(), who, path,
        format2 ? fmt::format("settings format 2, %u pin(s)", pins.size()) : std::string("settings format 1, full dump"), listed).c_str());
    ps4_notify(i18n::trf(str::CFGU_HOME_SAVED, {game ? title : std::string(i18n::tr(str::SET_TODOS_JOGOS)), keys.size()}).c_str());
    finish();
}

std::string ps4_select_game(const std::string& preferred, const std::string& error)
{
    // 0.33: PS3 firmware. An install cut short while it swapped folders is undone before anything reads dev_flash (the
    // XMB font comes from it). Every write of the installer stays under /data/rpcs3 (empty target: dev_flash elsewhere).
    namespace firmware = utils::ps4::firmware;
    const std::string firmware_dev_flash = g_cfg_vfs.get_dev_flash();
    const std::string firmware_target = firmware::target_folder(firmware_dev_flash);
    std::string firmware_notice; // what recover() or the check at start did, for the first message line
    const firmware::recovery firmware_recovered = firmware::recover(firmware_target);
    if (!firmware_recovered.log.empty())
    {
        ps4_startup_checkpoint(firmware_recovered.log.c_str());
        firmware_notice = shorten(i18n::trf(firmware_recovered.restored ? str::LAU_FIRMWARE_INTERROMPIDA : str::LAU_FIRMWARE_PS3_PREFIXO,
            {i18n::raw(firmware_recovered.text)}), line_bytes(116));
    }
    // The firmware an install left next to dev_flash.old is checked once per start, at the first XMB (before any game
    // ran): never in the run that installed it (an install started here sets the flag too). Postponed while a game is
    // still stopping (the check may put dev_flash.old back).
    static bool s_firmware_start_checked = false;
    if (!s_firmware_start_checked && Emu.IsStopped(true))
    {
        s_firmware_start_checked = true;
        const firmware::start_report report = firmware::check_at_start(firmware_target, !firmware_recovered.log.empty());
        if (!report.log.empty()) ps4_startup_checkpoint(report.log.c_str());
        if (!report.text.empty()) firmware_notice = report.text;
    }
    firmware::installed_state firmware_state = firmware::installed(firmware_dev_flash);
    {
        static bool s_firmware_logged = false;
        if (!std::exchange(s_firmware_logged, true))
        {
            ps4_startup_checkpoint((firmware_state.ready() ? fmt::format("firmware: PS3 firmware %s in '%s'", firmware_state.version, firmware_dev_flash)
                : fmt::format("firmware: no usable PS3 firmware in '%s' (version.txt '%s', liblv2.sprx %s); the XMB points to 'Instalar firmware do PS3'",
                    firmware_dev_flash, firmware_state.version, firmware_state.modules ? "present" : "missing")).c_str());
            if (firmware_target.empty())
                ps4_startup_checkpoint(fmt::format("firmware: dev_flash '%s' is not a dev_flash folder inside /data/rpcs3 (or it, its .installing or "
                    "its .old is a link); the XMB installer refuses it", firmware_dev_flash).c_str());
        }
    }
    launcher_display display;
    if (!display.open())
    {
        ps4_startup_checkpoint("launcher initialization failed; check RPCS3.log");
        ps4_launcher_log.error("Native launcher initialization failed (video=%d, buffers=%d)", display.video, display.buffers);
        return preferred;
    }
    const u64 started = sceKernelGetProcessTime();
    utils::ps4::xmb::scene menu;
    utils::ps4::settings_menu settings;
    settings_page_view page_view; // the browser tabs of the menu as PS3 settings pages
    bool settings_open = false;
    bool settings_dirty = true;  // settings rows are rebuilt only after something changed them
    bool players_pending = true; // the settings 'Controles conectados' row needs the current seats
    std::vector<xmb_entry> settings_entries;
    const bool lightbar = !fs::is_file(ps4_pads::no_lightbar_flag);
    std::array<std::vector<xmb_entry>, 10> columns;
    columns[0] = xmb_users(lightbar);
    constexpr char info_title[] = "RPCS3 PORT FOR PS4 by roberix studio & mifi studios";
    // 0.39: the 'Configurações' column, built again when the interface language changes
    const auto settings_column = [&]() -> std::vector<xmb_entry>
    {
        return {{"@info", i18n::tr(str::LAU_INFORMACOES_SISTEMA), std::string(info_title) + " · " + firmware_info_text(firmware_state), "tex_sysconf", {}},
            {"@settings", i18n::tr(str::LAU_CONFIGURACOES_RPCS3), i18n::tr(str::LAU_CONFIGURACOES_RPCS3_HELP), "tex_sysconf", {}},
            // 0.39: interface language (ui-language.txt); its title reads the same in every language
            {"@ui-language", i18n::tr(str::LANG_ROW_TITLE), i18n::trf(str::LANG_ROW_HELP, {i18n::raw(i18n::setting_label(i18n::current_setting()))}),
                "tex_sysconf", {}},
            {"@refresh", i18n::tr(str::LAU_ATUALIZAR_BIBLIOTECA), "", "item_tex_plain_folder", {}},
            // 0.33: patch files for every game; R1 on a game lists and switches its patches
            // 0.37: the R1 hint stays on the download row (the import sources fill this subtitle)
            {"@patches:import", i18n::tr(str::LAU_IMPORTAR_PATCHES_JOGOS), patch_import_help(), "item_tex_disc_install", {}},
            {"@patches:download", i18n::tr(str::COM_BAIXAR_PATCHES_RPCS3), patch_download_help(), "item_tex_disc_install", {}},
            // cheat files for every game (a PC cheats.yml is split by the library's titles); R2 on a game lists its cheats
            {"@cheats:import-all", i18n::tr(str::CHT_IMPORTAR_TODOS), i18n::tr(str::CHT_IMPORTAR_HELP), "item_tex_disc_install", {}},
            // 0.33: per-game settings of the PC build, from USB or /data/rpcs3/import
            {"@configdb:import", i18n::tr(str::LAU_IMPORTAR_CONFIG_DATABASE), config_db::row_help(), "item_tex_disc_install", {}},
            // 0.33: PS3UPDAT.PUP from USB or /data/rpcs3, as 'Install Firmware' of the PC build
            {"@firmware:install", i18n::tr(str::LAU_INSTALAR_FIRMWARE_PS3), firmware_row_help(firmware_state), "item_tex_disc_install", {}},
            {"@exit", i18n::tr(str::LAU_SAIR_RPCS3), "", "tex_user", {}}};
    };
    columns[1] = settings_column();
    const auto empty_columns = [&]
    {
        for (int i : {2, 3, 4, 5, 9}) columns[i] = {{"", i18n::tr(str::COM_NAO_HA_CONTEUDO), "", menu.icons[i], {}}};
    };
    empty_columns();
    // Rows per USB drive arrive with the library scan.
    columns[7] = {{"@storage", i18n::tr(str::COM_ARMAZENAMENTO_JOGOS), storage_help(), "item_tex_plain_folder", {}}, disc_drive_row(), disc_probe_row()};
    columns[8] = {{"", "PlayStation Network", "", "tex_psn", {}}}; // filled when the column is first shown
    std::array<std::size_t, 10> selections{};

    // The library of the previous scan is shown at once; the loader re-checks it and replaces it.
    load_queue loader;
    loader.context.preferred = preferred;
    u32 generation = 1;       // library generation the UI waits for (Triangle starts a new one)
    u32 shown_generation = 1; // generation of the list in columns[6]
    bool library_first = true;
    const auto refreshing_text = [] { return i18n::tr(str::LAU_ATUALIZANDO_BIBLIOTECA); };
    // 0.33: games of USB drives that are not connected now are left out of the cached list until the scan confirms it.
    const usb_keys usb_at_open = probe_usb_drives();
    const auto usb_connected = [&](const std::string& path)
    {
        const int drive = utils::ps4::usb_drive_of(path);
        return drive < 0 || !usb_at_open[drive].empty();
    };
    bool library_cached = loader.context.library.load();
    // 0.37: lock records of the games installed from a PS3 disc (disc_lock); the cached list shows its padlocks at once
    // (a small folder: one listing, a read of at most 4 KiB per record), every library scan brings them again
    utils::ps4::disc_lock::registry lock_registry;
    if (library_cached)
    {
        lock_registry = utils::ps4::disc_lock::sys::load_registry();
        std::vector<game_info> shown;
        for (const auto& game : loader.context.library.games)
            if (usb_connected(game.path)) shown.push_back(game);
        library_cached = !shown.empty();
        columns[6] = library_cached ? game_column(shown, {}, false, &lock_registry) : loading_column();
        ps4_startup_checkpoint(fmt::format("XMB: library shown from cache (%u games/packages; %u on USB drives not connected left out)",
            shown.size(), loader.context.library.games.size() - shown.size()).c_str());
    }
    else columns[6] = loading_column();
    // While the list is loading the cursor rests on the 'Carregando' row.
    if (const std::size_t row = find_path(columns[6], preferred); row != no_row) selections[6] = row;
    else selections[6] = library_cached ? default_game_row(columns[6]) : 2;

    // Declared after the queue, so the thread is joined before the queue goes away.
    std::optional<named_thread<load_worker>> loader_thread;
    if (utils::ps4::governor::thread_pressure() >= 2)
    {
        loader.inline_mode = true;
        ps4_startup_checkpoint("XMB loader: close to the thread limit; loading on the UI thread");
    }
    else
    {
        loader_thread.emplace(stx::try_launch, "PS4 XMB Loader", load_worker{&loader});
        if (!loader_thread->ps4_try_start())
        {
            loader_thread.reset();
            loader.inline_mode = true;
            ps4_startup_checkpoint("XMB loader: thread refused; loading on the UI thread");
        }
    }
    const auto wake_loader = [&]
    {
        if (loader_thread) thread_ctrl::notify(*loader_thread);
    };
    loader.post({load_kind::resources});
    loader.post({load_kind::licenses});
    loader.post({load_kind::library, generation});
    wake_loader();
    loader_stats stats;

    int category = 6;
    bool savedata_open = false;
    std::vector<xmb_entry> savedata;
    std::size_t save_selection = 0;
    // 0.39: the notice of the game that just ended, decoded once; its dialogs and line are built in the interface language
    namespace xn = utils::ps4::xmb_notice;
    const xmb_incoming incoming = read_notice(error);
    if (incoming.encoded) ps4_startup_checkpoint(("XMB: notice " + xn::describe(error)).c_str());
    std::string message = incoming.line;
    // 0.33: the game that just ran was on a USB drive that is gone (pulled out while it ran, or between two games).
    if (!usb_connected(preferred))
    {
        const std::string drive = utils::ps4::origin_label(preferred);
        message = incoming.line.empty() ? i18n::trf(str::LAU_NOTICE_USB_JOGO_SAIU_LISTA, {drive})
                                        : shorten(i18n::trf(str::LAU_NOTICE_USB_DESCONECTADO, {drive, i18n::raw(incoming.line)}), line_bytes(116));
        ps4_startup_checkpoint(fmt::format("XMB: the last game was on %s, which is not connected any more ('%s')", drive, preferred).c_str());
    }
    // 0.33: a boot refused for the firmware (System.cpp game_boot_result texts), or no firmware at all, points to the
    // install row instead of the bare English reason.
    if (message == incoming.line)
    {
        const std::string restart = incoming.restart_advised ? i18n::tr(str::LAU_REINICIE_APLICATIVO_SUFIXO) : "";
        if (incoming.k == xn::kind::firmware_missing) message = i18n::tr(str::LAU_FIRMWARE_JOGO_PRECISA) + restart;
        else if (incoming.k == xn::kind::firmware_old) message = i18n::tr(str::LAU_FIRMWARE_JOGO_PEDE_MAIS_NOVO) + restart;
        else if (message.empty() && !firmware_state.ready()) message = firmware_missing_hint();
    }
    // 0.33: the firmware was put back or checked just now: that line says which firmware is installed.
    if (!firmware_notice.empty()) message = firmware_notice;
    launcher_input input;
    input.poll(started, false); // presses buffered during the game are not menu input
    u64 last_frame = sceKernelGetProcessTime();
    u64 last_activity = last_frame, last_present = 0, background_since = 0;
    u64 last_draw_start = 0;     // start of the last frame drawn (idle pacing)
    unsigned previous_tier = 0;  // idle tier of the last frame drawn
    bool drive_idle = false;     // 0.39: the drive service checks an empty drive at the idle cadence
    bool first_frame = true;
    bool wait_for_button_release = true;
    bool backgrounded = false, resources_ready = false;
    bool any_pad = false, psn_dirty = true;
    std::string clock;
    std::time_t clock_second = -1;
    std::set<std::string> artwork_attempted;
    std::size_t icon_anchor = no_row;
    unsigned flip_failures = 0;
    unsigned users_generation = ps4_pads::generation(), users_connected = ps4_pads::connected_count();
    xmb_perf perf;
    perf.since = started;
    // RPCN account state. A running job blocks game boots; the keyboard blocks all menu input.
    bool rpcn_busy = false;
    rpcn_form form = rpcn_form::none;
    std::string form_user, form_email;
    std::string rpcn_progress_text; // progress line last shown; updated only while no menu message replaced it
    std::string rpcn_result;        // result line of the last job; marked green (ok) or red while it is shown
    bool rpcn_result_ok = false;
    // 0.33: patch list of one game (R1 on a game row), patch import (loader job) and patch.yml download (own thread).
    const patch_icons switch_icons;
    bool patches_open = false;
    std::vector<xmb_entry> patch_rows;
    std::size_t patch_selection = 0;
    std::shared_ptr<utils::ps4::patches::game_patches> patch_list; // null while the loader reads it
    xmb_entry patch_game;            // game row the list belongs to
    u32 patch_generation = 0;        // list request the UI waits for
    bool patch_import_busy = false;
    std::size_t notes_row = no_row, notes_page = 0;
    std::string download_progress;   // progress line last shown
    std::unique_ptr<utils::ps4::patches::download_state> download; // declared before its thread: outlives it
    std::optional<named_thread<patch_download_worker>> download_thread;
    int patch_cheats_on = -1;           // cheats on for the game of the patch list (its first row); -1 until read
    // Cheats of one game (R2 on a game row, or the first row of its patch list): the list (loader job), the options of
    // one cheat (Square), the system keyboard forms and the questions over the list
    bool cheats_open = false;
    bool cheats_from_patches = false;   // opened from the patch list: Circle goes back there
    bool cheat_options_open = false;
    std::vector<xmb_entry> cheat_rows, cheat_option_rows;
    std::size_t cheat_selection = 0, cheat_option_selection = 0, cheat_wanted = 0;
    std::shared_ptr<utils::ps4::cheats::game_cheats> cheat_list; // null while the loader reads it
    xmb_entry cheat_game;               // game row the list belongs to
    u32 cheat_generation = 0;           // list request the UI waits for
    u32 cheat_options_key = 0;          // key of the cheat whose options are shown (rows move when the files are read again)
    std::string cheat_opening_text;     // message shown while the list loads; replaced by its header
    bool cheat_import_busy = false;
    bool cheat_reload_due = false;      // an import finished while a cheat form had the keyboard
    enum class cheat_form : u8 { none, add_name, add_codes, edit_codes, rename, value };
    cheat_form cheat_keyboard = cheat_form::none;
    std::string cheat_form_name;        // name typed for a new cheat
    u32 cheat_form_key = 0;             // cheat the keyboard edits
    bool cheat_form_then_on = false;    // a PC entry is switched on once its typed value is valid
    enum class cheat_ask : u8 { none, remove, lines };
    cheat_ask cheat_asking = cheat_ask::none; // the delete question or a page of 'Ver linhas' (cheat_dialog)
    std::optional<xmb_dialog> cheat_dialog;
    u32 cheat_dialog_key = 0;
    std::size_t cheat_lines_page = 0;
    // the cheat rows show the patch rows' switches by name
    menu.resources.insert_or_assign(cheat_switch_on, switch_icons.on);
    menu.resources.insert_or_assign(cheat_switch_off, switch_icons.off);
    // 0.33: USB hot-plug, 'Limpar cache deste jogo' (L1 on a game) and the config database import.
    bool library_busy = true;          // the library job of the current generation has not reported yet
    u32 hotplug_generation = 0;        // library generation started by a drive change (not by Triangle)
    bool usb_poll_pending = false;
    u64 next_usb_poll = 0, last_hotplug = 0;
    std::string usb_message;           // last drive change shown; the next one may replace it
    const auto usb_refresh_text = [] { return i18n::tr(str::LAU_USB_MUDARAM_ATUALIZANDO); };
    std::optional<xmb_entry> cache_confirm; // game whose cache removal waits for X
    std::string cache_confirm_text;
    bool cache_clear_busy = false, config_db_busy = false;
    // 0.33: PS3 firmware install. One job at a time; the job is declared before its thread, so the thread is joined first.
    std::unique_ptr<firmware::job> firmware_job;
    std::optional<named_thread<firmware_worker>> firmware_thread;
    std::unique_ptr<firmware::package> firmware_ready;    // checked PUP whose question waits for X
    std::string firmware_confirm_text, firmware_progress; // the question; progress line last shown
    std::string firmware_installing;                      // version being installed
    bool firmware_cancel_asked = false;                   // the cancel question of the install waits for the next press
    std::optional<xmb_dialog> firmware_question;          // 0.38: that question as a system dialog
    const auto firmware_cancel_text = [] { return i18n::tr(str::LAU_FIRMWARE_CANCELAR_PERGUNTA); };
    // 0.38: questions, progress and results of the installs are PS3 system dialogs over the waves (xmb_overlay). A result
    // dialog waits for X or Circle; 'overlay_since' starts the fade-in of a dialog that was not on screen.
    std::optional<xmb_dialog> notice;
    u64 overlay_since = 0;
    const auto show_notice = [&](std::string title, std::string_view text)
    {
        xmb_dialog result;
        result.title = std::move(title);
        split_into_dialog(text, result);
        result.buttons = {utils::ps4::xmb::cross_button(i18n::tr(str::COM_OK))};
        notice = std::move(result);
    };
    // 0.38: the game closed because its PS3 disc left the drive (disc_lock::game_watch, ps4_main.cpp): the notice is a
    // system dialog over the XMB as well as the message line. The game's name and icon join it once its row is listed
    // (xmb_overlay); the icon is a copy, the rows may move while it is shown. The boot failure dialogs below name the
    // game the same way: 'game_dialog_line' is the first line of the dialog that gets them.
    std::string game_dialog_line;
    xmb_image game_dialog_icon;
    if (incoming.k == xn::kind::disc_removed)
    {
        show_notice({}, incoming.line);
        notice->notes.insert(notice->notes.begin(), i18n::tr(str::LAU_NOTICE_JOGO_FECHADO_DISCO));
        game_dialog_line = utils::ps4::disc_lock::removed_text();
        ps4_startup_checkpoint("XMB: the disc-removed notice is shown as a system dialog");
    }
    // 0.38: a boot refused because a thread of this process ended in a fatal error, or a lock it held stays taken
    // (ps4_main.cpp select_and_boot): the request to reopen the application is a system dialog over the XMB as well
    else if (const bool fatal = incoming.k == xn::kind::emulator_error; fatal || incoming.k == xn::kind::emulator_restart)
    {
        xmb_dialog restart;
        restart.lines.emplace_back(i18n::tr(fatal ? str::COM_OCORREU_ERRO_EMULADOR : str::LAU_NOTICE_EMULADOR_PRECISA_REINICIAR));
        // 0.39: when the reason says that reopening does not help this game (an incomplete ISO, the same error again),
        // reopening is only offered for another game; how to close the application follows
        restart.notes.emplace_back(i18n::tr(incoming.no_reopen ? str::LAU_NOTICE_JOGAR_OUTRO_FECHE : str::LAU_NOTICE_JOGAR_DE_NOVO_FECHE));
        restart.notes.emplace_back(i18n::tr(str::LAU_NOTICE_COMO_FECHAR));
        // 0.38: the parts after the first " · " (the boot-next note) follow as notes
        if (!incoming.detail.empty())
        {
            xmb_dialog extra;
            split_into_dialog(incoming.detail, extra);
            for (std::string& part : extra.lines) restart.notes.push_back(std::move(part));
            for (std::string& part : extra.notes) restart.notes.push_back(std::move(part));
        }
        restart.buttons = {utils::ps4::xmb::cross_button(i18n::tr(str::COM_OK))};
        notice = std::move(restart);
        ps4_startup_checkpoint("XMB: the emulator-error notice is shown as a system dialog");
    }
    // 0.38: the clean-process rule refused a boot (ps4_main.cpp): the request to close and reopen the application, its
    // reason and the boot-next note ("<game> abre sozinho ...") as a system dialog
    else if (incoming.k == xn::kind::clean_process)
    {
        show_notice({}, incoming.line);
        notice->notes.emplace_back(i18n::tr(str::LAU_NOTICE_COMO_FECHAR));
        ps4_startup_checkpoint("XMB: the close-and-reopen request of the clean-process rule is shown as a system dialog");
    }
    // 0.38: the renderer stopped the game because the GPU window or the CPU heap could not serve it (ps4_vram_policy.hpp
    // stop notices; the XMB line may be cut, so only the start is compared)
    else if (const bool vram = incoming.k == xn::kind::vram_stop; vram || incoming.k == xn::kind::heap_stop)
    {
        xmb_dialog stopped;
        stopped.lines.emplace_back(i18n::tr(vram ? str::LAU_NOTICE_VRAM_FECHADO : str::LAU_NOTICE_HEAP_FECHADO));
        stopped.notes.emplace_back(i18n::tr(vram ? str::LAU_NOTICE_VRAM_PROXIMA : str::LAU_NOTICE_HEAP_PROXIMA));
        if (vram && incoming.restart_advised) stopped.notes.emplace_back(i18n::tr(str::LAU_NOTICE_RECOMENDADO_FECHAR));
        stopped.buttons = {utils::ps4::xmb::cross_button(i18n::tr(str::COM_OK))};
        notice = std::move(stopped);
        ps4_startup_checkpoint("XMB: the out-of-memory stop notice is shown as a system dialog");
    }
    // A failed boot or an incomplete ISO (ps4_main.cpp failure_dialog): what went wrong and the steps that fix it, in a
    // system dialog named after the game; the message line keeps the first part
    else if (incoming.dialog)
    {
        show_notice({}, incoming.detail);
        if (incoming.restart_advised) notice->notes.emplace_back(i18n::tr(str::COM_RECOMENDADO_REINICIAR_APLICATIVO_ANTES_JOGAR));
        if (!notice->lines.empty()) game_dialog_line = notice->lines.front();
        ps4_startup_checkpoint(fmt::format("XMB: the boot failure notice is shown as a system dialog (%u step(s))", notice->notes.size()).c_str());
    }
    // A boot refused for the PS3 firmware: the same as a system dialog, with the version installed now and how to install one
    else if (const bool missing = incoming.k == xn::kind::firmware_missing; missing || incoming.k == xn::kind::firmware_old)
    {
        xmb_dialog firmware_needed;
        firmware_needed.lines.emplace_back(i18n::tr(missing ? str::LAU_FIRMWARE_DLG_PRECISA : str::LAU_FIRMWARE_DLG_MAIS_NOVO));
        const std::string installed_now = firmware_state.version.empty() ? std::string(i18n::tr(str::LAU_FIRMWARE_NENHUM))
            : firmware_state.ready() ? firmware_state.version : i18n::trf(str::LAU_FIRMWARE_VERSAO_INCOMPLETO, {firmware_state.version});
        firmware_needed.notes.push_back(i18n::trf(str::LAU_FIRMWARE_INSTALADO_AGORA, {installed_now}));
        firmware_needed.notes.push_back(i18n::trf(str::LAU_FIRMWARE_DLG_COPIE_PUP, {"/data/rpcs3"}));
        firmware_needed.notes.emplace_back(i18n::tr(str::LAU_FIRMWARE_DLG_INSTALE_ESCOLHA));
        if (incoming.restart_advised) firmware_needed.notes.emplace_back(i18n::tr(str::COM_RECOMENDADO_REINICIAR_APLICATIVO_ANTES_JOGAR));
        firmware_needed.buttons = {utils::ps4::xmb::cross_button(i18n::tr(str::COM_OK))};
        notice = std::move(firmware_needed);
        game_dialog_line = notice->lines.front();
        ps4_startup_checkpoint(fmt::format("XMB: the firmware boot failure is shown as a system dialog (installed: %s)",
            firmware_state.version.empty() ? std::string("none") : firmware_state.version).c_str());
    }
    // 0.38: the application was opened again for the game of boot-next.txt ('preferred'): it boots when the countdown
    // ends or on X; Circle cancels and leaves the XMB open
    std::optional<u64> boot_next_until;
    std::string boot_next_name;
    if (!preferred.empty() && incoming.k == xn::kind::boot_next)
    {
        boot_next_name = shorten(incoming.detail.empty() ? std::string(i18n::tr(str::COM_JOGO)) : incoming.detail, 70);
        boot_next_until = started + u64{ps4_boot_next_seconds} * 1'000'000;
        notice.reset();
        ps4_startup_checkpoint(fmt::format("XMB: boot-next countdown of %u s for '%s' shown as a system dialog", ps4_boot_next_seconds, preferred).c_str());
    }
    // 0.39: folders the start-up repair could not open (ps4_repair_data_permissions), once per process when no other
    // dialog is due
    if (!notice && !boot_next_until && !s_fs_blocked_folders.empty())
    {
        xmb_dialog blocked;
        blocked.lines.emplace_back(i18n::tr(str::LAU_FS_PASTAS_BLOQUEADAS));
        blocked.notes.emplace_back(i18n::tr(s_fs_ftp_answered ? str::LAU_FS_FTP_LIGADO_NAO_LIBEROU : str::LAU_FS_LIGUE_FTP_REABRA));
        blocked.notes.emplace_back(i18n::tr(str::LAU_FS_FILEZILLA));
        blocked.notes.push_back(i18n::trf(s_fs_blocked_more ? str::LAU_FS_BLOQUEADAS_OUTRAS : str::LAU_FS_BLOQUEADAS, {fmt::merge(s_fs_blocked_folders, ", ")}));
        blocked.buttons = {utils::ps4::xmb::cross_button(i18n::tr(str::COM_OK))};
        notice = std::move(blocked);
        s_fs_blocked_folders.clear();
        ps4_startup_checkpoint("XMB: the blocked-folders notice (fs perms) is shown as a system dialog");
    }
    const auto firmware_too_late_text = [] { return i18n::tr(str::LAU_FIRMWARE_TARDE_CANCELAR); };

    const auto finish = [&](const char* why)
    {
        const u64 at = sceKernelGetProcessTime();
        perf.report(at, why);
        if (download_thread)
        {
            // The transfer stops at its next progress callback (about once per second, also while connecting).
            download->stop = true;
            download_thread.reset();
            ps4_startup_checkpoint("patches: download stopped because the XMB closed");
        }
        if (firmware_thread)
        {
            // An install stops before its next package and keeps the installed firmware (a swap in progress finishes).
            firmware_job->stop = true;
            firmware_thread.reset();
            ps4_startup_checkpoint("firmware: job stopped because the XMB closed");
        }
        // 0.37: a disc key search stops (the next XMB runs it again); on shutdown a disc install records what it
        // copied, and the next install goes on from there
        disc_ui::dk::service::instance().cancel();
        disc_ui::ds::service::instance().set_idle(false);
        if (const auto disc_install = disc_ui::di::current(); disc_ui::running(disc_install) && std::string_view(why) == "shutdown")
        {
            disc_install->cancel.store(true);
            ps4_startup_checkpoint("XMB: PS3 disc install stopped because the application is closing");
        }
        loader.context.stopping = true;
        u64 join_us = 0;
        if (loader_thread)
        {
            const u64 join_start = sceKernelGetProcessTime();
            thread_ctrl::notify(*loader_thread);
            loader_thread.reset(); // at most the job in progress (one ISO parse, or the directory a drive walk is in) is waited for
            join_us = sceKernelGetProcessTime() - join_start;
        }
        ps4_startup_checkpoint(stats.line(loader.inline_mode, join_us).c_str());
    };

    struct view_t
    {
        std::vector<xmb_entry>* entries;
        std::size_t* selected;
    };
    const auto view = [&]() -> view_t
    {
        if (settings_open && settings_dirty)
        {
            settings_entries = settings.rows();
            page_view.touch();
            settings_dirty = false;
        }
        // a cheat list (and the options of one of its cheats) is over the patch list it was opened from
        auto& entries = settings_open ? settings_entries : savedata_open ? savedata : cheats_open ? (cheat_options_open ? cheat_option_rows : cheat_rows)
            : patches_open ? patch_rows : columns[category];
        auto& selected = settings_open ? settings.selection() : savedata_open ? save_selection
            : cheats_open ? (cheat_options_open ? cheat_option_selection : cheat_selection) : patches_open ? patch_selection : selections[category];
        if (entries.empty()) entries.push_back({{}, i18n::tr(str::COM_NAO_HA_CONTEUDO), "", "tex_sysconf", {}});
        selected = std::min(selected, entries.size() - 1);
        return {&entries, &selected};
    };

    // rescan: Triangle / 'Atualizar biblioteca' read every USB drive again; a hot-plug refresh reads only the drives
    // that changed and announces itself only over an empty line or an earlier drive message.
    // 0.37: usb_notice false: a refresh for a /data folder change, which shows no drive notice.
    const auto refresh_library = [&](bool rescan, bool usb_notice = true)
    {
        ++generation;
        for (const auto& dropped : loader.drop_icons()) artwork_attempted.erase(dropped);
        artwork_attempted.clear();
        icon_anchor = no_row;
        loader.drop_library();
        loader.post({load_kind::licenses, generation, "@refresh"});
        loader.post({load_kind::library, generation, rescan ? "@rescan" : "@hotplug"});
        library_busy = true;
        hotplug_generation = rescan ? 0 : generation;
        if (savedata_open) loader.post({load_kind::savedata, generation});
        wake_loader();
        // 0.37: Triangle also reads the disc drive again
        if (rescan) disc_ui::ds::service::instance().request_check();
        if (rescan) message = refreshing_text();
        else if (usb_notice && (message.empty() || message == usb_message)) message = usb_message = usb_refresh_text();
    };

    // 0.33: 'Limpar cache deste jogo'. The cache of a title is /data/rpcs3/cache/cache/<TITLE ID>/; removing it costs a
    // full PPU/SPU compile on the next boot, so the next press must be X to go ahead.
    const auto ask_cache_clear = [&](const xmb_entry& game)
    {
        if (!utils::ps4::health::is_title_cache_name(game.serial))
        {
            message = i18n::tr(str::LAU_CACHE_SEM_TITLE_ID);
            return;
        }
        if (cache_clear_busy)
        {
            message = i18n::tr(str::LAU_CACHE_LIMPEZA_ANDAMENTO);
            return;
        }
        // The XMB can open while a previous game is still stopping (and writing its caches).
        if (!Emu.IsStopped(true))
        {
            message = i18n::tr(str::COM_JOGO_ANTERIOR_AINDA_ESTA_FECHANDO);
            return;
        }
        cache_confirm = game;
        // The selected row names the game; the question names its title ID (one ~110 character line).
        message = cache_confirm_text = i18n::trf(str::LAU_CACHE_LIMPAR_PERGUNTA, {game.serial});
    };
    const auto start_cache_clear = [&](const xmb_entry& game)
    {
        cache_clear_busy = true;
        loader.post({load_kind::cache_clear, generation, game.path, game.serial, game.title});
        wake_loader();
        message = i18n::trf(str::LAU_CACHE_LIMPANDO, {game.serial});
        ps4_startup_checkpoint(fmt::format("XMB: cache removal of %s requested ('%s')", game.serial, game.path).c_str());
    };
    const auto start_config_db_import = [&]
    {
        if (config_db_busy)
        {
            message = i18n::tr(str::LAU_CONFIGDB_IMPORTACAO_ANDAMENTO);
            return;
        }
        config_db_busy = true;
        loader.post({load_kind::config_db_import, generation});
        wake_loader();
        message = i18n::tr(str::LAU_CONFIGDB_IMPORTANDO);
        ps4_startup_checkpoint("config database: import requested");
    };

    // 0.34: physical-disc rows of the storage column. The drive test starts on the second X (the first one says what it
    // does); both jobs run on their own threads and the loop below takes their results.
    std::shared_ptr<utils::ps4::disc_probe::probe> disc_probe;
    std::shared_ptr<disc_key_import> disc_keys_job;
    const auto disc_probe_select = [&]
    {
        if (disc_probe)
        {
            message = utils::ps4::disc_probe::running_text();
            return;
        }
        if (message != utils::ps4::disc_probe::confirm_text())
        {
            message = utils::ps4::disc_probe::confirm_text();
            return;
        }
        if (utils::ps4::governor::thread_pressure() >= 2)
        {
            message = i18n::tr(str::LAU_PROBE_SEM_RECURSOS_THREADS);
            ps4_startup_checkpoint("disc probe: refused: close to the thread limit; nothing probed");
            return;
        }
        disc_probe = utils::ps4::disc_probe::probe::start(utils::ps4::disc_probe::system_ops(),
            [](const std::string& line) { ps4_startup_checkpoint(line.c_str()); });
        message = utils::ps4::disc_probe::running_text();
    };
    const auto disc_keys_select = [&]
    {
        if (disc_keys_job)
        {
            message = i18n::tr(str::LAU_DKEY_IMPORTACAO_ANDAMENTO);
            return;
        }
        disc_keys_job = start_disc_key_import();
        // Without a thread it runs here: the menu stops drawing until it is done
        message = disc_keys_job ? std::string(i18n::tr(str::LAU_DKEY_IMPORTANDO)) : import_disc_keys();
        if (!disc_keys_job) show_notice(i18n::tr(str::COM_CHAVES_DISCO_DKEY), message); // 0.38: the result as a system dialog
    };

    // 0.37: the PS3 disc in the PS4 drive (disc_ui). The loop polls the drive, the key search and the install every
    // 500 ms (none of them waits); X on the drive row boots the installed image or asks on the whole screen.
    namespace ds = disc_ui::ds;
    namespace di = disc_ui::di;
    namespace dk = disc_ui::dk;
    ds::disc_status disc_now = ds::service::instance().last();
    std::shared_ptr<di::job> disc_job = di::current();
    std::string disc_progress; // install progress line last shown
    u64 next_disc_poll = 0;
    bool copy_screen_open = false; // 0.38: the copy dialog (show_copy) is on screen and shows the result itself
    const auto disc_rows = [&]
    {
        if (disc_ui::apply_rows(columns[6], selections[6], columns[7], disc_ui::texts(disc_now, disc_job))) icon_anchor = no_row;
    };
    // 0.37: padlocks (disc_lock). A game installed from a PS3 disc is unlocked while the drive's last check, at most two
    // minutes old, saw a disc of it; each change is logged once per TITLE_ID. With a locked game selected and another
    // disc in the drive, the drive is read every 10 s while the player is active (else every 60 s).
    namespace dl = utils::ps4::disc_lock;
    std::int64_t lock_forced_at = 0;
    const auto lock_rows = [&](u64 now)
    {
        const std::int64_t now_ms = ds::detail::now_ms();
        auto& logged = disc_ui::remembered().lock_logged;
        for (xmb_entry& row : columns[6])
        {
            if (!row.disc_bound) continue;
            // 0.39: the record of the row's own image (the scan bound it by path)
            const dl::record* found = dl::recorded_at(lock_registry, row.path);
            if (!found)
            {
                row.locked = false; // the record went away after the scan that bound it: X reads it again
                continue;
            }
            const dl::match m = dl::classify(*found, disc_now, now_ms);
            row.locked = !dl::unlocks(m);
            if (m == dl::match::unknown || m == dl::match::stale) continue; // nothing new about the drive yet
            if (const auto [it, added] = logged.try_emplace(found->title_id, m); added || it->second != m)
            {
                it->second = m;
                ps4_startup_checkpoint(("disc lock: " + dl::row_log_line(*found, m, disc_now)).c_str());
            }
        }
        if (category != 6 || settings_open || savedata_open || patches_open || cheats_open || columns[6].empty()) return;
        const xmb_entry& chosen = columns[6][std::min(selections[6], columns[6].size() - 1)];
        if (!chosen.locked) return;
        const dl::record* found = dl::recorded_at(lock_registry, chosen.path);
        if (!found) return;
        const std::int64_t every = dl::forced_check_ms(true, true, dl::classify(*found, disc_now, now_ms),
            static_cast<std::int64_t>((now - std::min(now, last_activity)) / 1000));
        if (every && now_ms - lock_forced_at >= every)
        {
            lock_forced_at = now_ms;
            ds::service::instance().request_check();
        }
    };
    const auto disc_refresh = [&](u64 now)
    {
        if (now < next_disc_poll) return;
        next_disc_poll = now + 500'000;
        disc_ui::memory& memory = disc_ui::remembered();
        dk::service& keys = dk::service::instance();
        // The drive worker is a thread of its own: close to the thread limit it is not started and the last status stays
        disc_now = utils::ps4::governor::thread_pressure() < 2 ? di::poll() : ds::service::instance().last();
        disc_job = di::current();
        if (std::optional<dk::result> found = keys.take())
        {
            // A search stopped when the XMB closed runs again at the next poll
            if (found->what == dk::outcome::cancelled) keys.forget();
            else
            {
                const std::string found_id = ds::canonical_title_id(found->title_id);
                memory.key_id = found_id.empty() ? memory.asked_id : found_id;
                memory.key_ok = found->ok;
                memory.key_text = found->text;
                if ((!found->ok || found->changed) && (message.empty() || message == disc_progress)) message = found->text;
            }
        }
        // The key is looked for once per disc in the drive, before anything asks for it (and again later after a network
        // failure). An installed disc whose key is in place needs no search; one installed without its key gets it here,
        // so its encrypted image opens without the player handling key files
        if (disc_now.state == ds::disc_state::ps3_disc && (disc_now.installed.empty() || !disc_now.key_present) && !disc_ui::running(disc_job) &&
            keys.request(disc_now))
        {
            memory.asked_id = disc_ui::disc_id(disc_now);
            ps4_startup_checkpoint(fmt::format("XMB: PS3 disc %s in the drive: key search started", memory.asked_id).c_str());
        }
        if (disc_ui::running(disc_job))
        {
            if (const u64 rate = disc_job->rate.load(std::memory_order_acquire)) memory.rate = rate;
            // Progress replaces only itself or an empty line
            std::string progress = di::progress_text(*disc_job);
            if (message.empty() || message == disc_progress) message = progress;
            disc_progress = std::move(progress);
        }
        else if (disc_job && memory.reported != disc_job)
        {
            // The end of an install: its result once, and the library lists the new image
            memory.reported = disc_job;
            if (const u64 rate = disc_job->rate.load(std::memory_order_acquire)) memory.rate = rate;
            const di::outcome result = disc_job->result();
            message = disc_ui::install_result_text(*disc_job);
            disc_progress.clear();
            last_activity = now;
            // 0.38: outside the copy dialog the result comes as a system dialog over the XMB (X or Circle closes it)
            if (!copy_screen_open)
            {
                disc_ui::copy_screen ended;
                notice = disc_ui::copy_dialog(*disc_job, ended, nullptr);
            }
            ps4_startup_checkpoint(fmt::format("XMB: PS3 disc install of %s ended: %s ('%s')", disc_ui::disc_id(disc_job->disc()), di::to_string(result),
                disc_job->path()).c_str());
            if (result == di::outcome::done) refresh_library(false, false);
        }
        disc_rows();
        lock_rows(now);
    };
    // 0.38: a firmware image of the XMB (null until the loader has brought them, or when missing)
    const auto xmb_resource = [&](std::string_view name) -> const xmb_image*
    {
        const auto found = menu.resources.find(name);
        return found != menu.resources.end() && found->second ? &found->second : nullptr;
    };
    // 0.38: the running disc copy as a system dialog (disc_ui::copy_dialog), opened by X on the drive row and right after
    // the install question. X closes it while the copy goes on; Circle asks whether to cancel; the result, when the copy
    // ends while it is shown, waits for X.
    const auto show_copy = [&](const std::shared_ptr<di::job>& job)
    {
        if (!job) return;
        disc_ui::copy_screen screen;
        copy_screen_open = true;
        const disc_ui::answer answer = disc_ui::ask(display, menu, started, input, [&](u32 pressed, xmb_dialog& shown) -> std::optional<disc_ui::answer>
        {
            disc_refresh(sceKernelGetProcessTime());
            const bool finished = job->finished.load(std::memory_order_acquire);
            if (finished && (pressed & (ORBIS_PAD_BUTTON_CROSS | ORBIS_PAD_BUTTON_CIRCLE))) return disc_ui::answer::back;
            if (!finished && !screen.cancel_sent)
            {
                if (screen.cancel_asked)
                {
                    if (pressed & ORBIS_PAD_BUTTON_CROSS)
                    {
                        screen.cancel_sent = true;
                        if (!job->cancel.exchange(true)) ps4_startup_checkpoint("XMB: PS3 disc install cancel confirmed on the copy screen");
                    }
                    else if (pressed & ORBIS_PAD_BUTTON_CIRCLE) screen.cancel_asked = false;
                }
                else if (pressed & ORBIS_PAD_BUTTON_CROSS) return disc_ui::answer::back;
                else if (pressed & ORBIS_PAD_BUTTON_CIRCLE) screen.cancel_asked = true;
            }
            shown = disc_ui::copy_dialog(*job, screen, xmb_resource("item_tex_disc_install"));
            return std::nullopt;
        });
        copy_screen_open = false;
        last_frame = sceKernelGetProcessTime();
        last_present = 0;
        last_activity = last_frame;
        wait_for_button_release = true;
        input.flush();
        if (answer == disc_ui::answer::back && disc_ui::running(job)) message = disc_progress = di::progress_text(*job);
    };
    // X on the drive row ('again': Square, install an installed disc once more). Returns the image to boot, else "".
    const auto disc_select = [&](bool again) -> std::string
    {
        next_disc_poll = 0;
        disc_refresh(sceKernelGetProcessTime());
        if (disc_ui::running(disc_job))
        {
            show_copy(disc_job);
            return {};
        }
        switch (disc_now.state)
        {
        case ds::disc_state::unknown:
            ds::service::instance().request_check();
            message = i18n::tr(str::LAU_DISC_VERIFICANDO_AGUARDE);
            return {};
        case ds::disc_state::no_server:
            ds::service::instance().request_check();
            message = i18n::tr(str::LAU_DISC_LIGUE_FTP);
            return {};
        case ds::disc_state::no_disc:
            ds::service::instance().request_check();
            message = i18n::tr(str::LAU_DISC_COLOQUE_DISCO);
            return {};
        case ds::disc_state::not_ps3:
            message = i18n::tr(str::LAU_DISC_NAO_E_PS3);
            return {};
        case ds::disc_state::ps3_disc:
            break;
        }
        const std::string id = disc_ui::disc_id(disc_now);
        if (!again && disc_now.partial.empty() && !disc_now.installed.empty())
        {
            if (fs::is_file(disc_now.installed))
            {
                ps4_startup_checkpoint(fmt::format("XMB: PS3 disc %s: booting the image installed from it ('%s')", id, disc_now.installed).c_str());
                return disc_now.installed;
            }
            ds::service::instance().request_check();
            message = i18n::tr(str::LAU_DISC_IMAGEM_SUMIU);
            return {};
        }
        disc_ui::memory& memory = disc_ui::remembered();
        dk::service& keys = dk::service::instance();
        // A search that failed (or never ran) is tried again now: the network or a USB key may be there now
        if (const disc_ui::key_state key = disc_ui::key_of(disc_now); key == disc_ui::key_state::failed || key == disc_ui::key_state::unknown)
        {
            keys.forget();
            if (keys.request(disc_now))
            {
                memory.asked_id = id;
                ps4_startup_checkpoint(fmt::format("XMB: PS3 disc %s: key search started again before the install", id).c_str());
            }
        }
        const u32 generation = disc_now.generation;
        disc_ui::question q;
        q.again = again && !disc_now.installed.empty();
        s64 free_bytes = -1;
        u64 free_at = 0;
        const disc_ui::answer answer = disc_ui::ask(display, menu, started, input, [&](u32 pressed, xmb_dialog& shown) -> std::optional<disc_ui::answer>
        {
            const u64 at = sceKernelGetProcessTime();
            disc_refresh(at);
            if (disc_now.state != ds::disc_state::ps3_disc || disc_now.generation != generation) return disc_ui::answer::gone;
            const disc_ui::key_state key = disc_ui::key_of(disc_now);
            if (pressed & ORBIS_PAD_BUTTON_CIRCLE) return disc_ui::answer::back;
            if ((pressed & ORBIS_PAD_BUTTON_SQUARE) && !disc_now.partial.empty())
            {
                if (q.discard_asked) return disc_ui::answer::discard;
                q.discard_asked = true;
            }
            if (pressed & ORBIS_PAD_BUTTON_CROSS)
            {
                q.discard_asked = false;
                if (key != disc_ui::key_state::searching) return disc_ui::answer::install;
                q.want = true;
            }
            // X while the search ran: a key installs at once; without one the question explains and asks again
            if (q.want && key != disc_ui::key_state::searching)
            {
                if (key == disc_ui::key_state::ready) return disc_ui::answer::install;
                q.want = false;
            }
            if (!free_at || at - free_at >= 1'000'000)
            {
                free_bytes = di::sys::posix_files().free_bytes(di::default_games_dir);
                free_at = at;
            }
            const disc_ui::memory& known = disc_ui::remembered();
            shown = disc_ui::question_dialog(disc_now, q, key, known.key_id == id ? known.key_text : std::string{}, free_bytes,
                known.rate ? known.rate : disc_ui::assumed_rate, xmb_resource("item_tex_disc_install"));
            return std::nullopt;
        });
        // Back on the XMB: no animation jump, no press carried over
        last_frame = sceKernelGetProcessTime();
        last_present = 0;
        last_activity = last_frame;
        wait_for_button_release = true;
        input.flush();
        switch (answer)
        {
        case disc_ui::answer::install:
        {
            const disc_ui::key_state key = disc_ui::key_of(disc_now);
            const std::shared_ptr<di::job> started = di::start(disc_now);
            if (!started)
            {
                message = i18n::tr(str::LAU_DISC_NAO_COMECOU);
                ps4_startup_checkpoint(fmt::format("XMB: PS3 disc %s: install not started (another install runs or no thread)", id).c_str());
                break;
            }
            disc_job = started;
            message = disc_progress = di::progress_text(*started);
            ps4_startup_checkpoint(fmt::format("XMB: PS3 disc %s '%s': install confirmed with X (key %s; %u MiB of %u MiB already copied; free %s)", id,
                disc_now.title, key == disc_ui::key_state::ready ? "ready" : key == disc_ui::key_state::searching ? "search running" : "missing",
                disc_now.partial.empty() ? 0 : disc_now.partial_bytes >> 20, disc_now.size >> 20,
                free_bytes >= 0 ? fmt::format("%u MiB", static_cast<u64>(free_bytes) >> 20) : std::string("unknown")).c_str());
            disc_rows();
            show_copy(started); // the copy on the whole screen, as the PS3 shows an install
            break;
        }
        case disc_ui::answer::back:
            message = i18n::tr(str::LAU_DISC_NAO_INICIADA);
            ps4_startup_checkpoint(fmt::format("XMB: PS3 disc %s: install question closed with Circle", id).c_str());
            break;
        case disc_ui::answer::gone:
            message = i18n::tr(str::LAU_DISC_SAIU_TROCADO);
            ps4_startup_checkpoint(fmt::format("XMB: PS3 disc %s: the disc left the drive during the install question", id).c_str());
            break;
        case disc_ui::answer::discard:
            message = i18n::tr(di::discard_partial(disc_now) ? str::LAU_DISC_COPIA_APAGADA : str::LAU_DISC_COPIA_NAO_APAGADA);
            ps4_startup_checkpoint(fmt::format("XMB: PS3 disc %s: removal of the interrupted copy '%s' requested", id, disc_now.partial).c_str());
            break;
        case disc_ui::answer::shutdown:
        case disc_ui::answer::play:
            break;
        }
        return {};
    };
    // 0.37: X on a game installed from a PS3 disc (disc_lock). Its lock record is read again (one small file) and the
    // drive checked again: the game boots when a disc of it is in the drive; otherwise a screen asks for the disc and
    // turns into "Disco reconhecido!" when it goes in. True: boot 'path'.
    // 0.39: the record is the one of this exact image (path and size); any other ISO of the game boots without the disc
    const auto lock_gate = [&](const std::string& path, const std::string& serial) -> bool
    {
        if (utils::ps4::file_kind(path) != utils::ps4::game_kind::iso) return true;
        fs::stat_t stat{};
        const u64 size = fs::get_stat(path, stat) && !stat.is_directory ? stat.size : 0;
        const std::optional<dl::record> record = dl::sys::record_for_path(path, size, serial);
        if (!record) return true;
        const std::int64_t pressed_ms = ds::detail::now_ms();
        const ds::disc_status before = ds::service::instance().last();
        ps4_startup_checkpoint(fmt::format("disc lock: X on '%s' (%s): checking the drive (last check %s)", path, record->title_id,
            before.checked_ms > 0 ? fmt::format("%d ms ago", pressed_ms - before.checked_ms) : std::string("never")).c_str());
        dl::gate gate(*record, path, pressed_ms);
        next_disc_poll = 0;
        // 0.38: the game's own icon above the message (a copy: the drive row may move the game rows while it is shown)
        xmb_image game_icon;
        if (const std::size_t row = find_path(columns[6], path); row != no_row) game_icon = columns[6][row].artwork;
        const std::string game = shorten(dl::game_name(*record), 70);
        const std::int64_t slow_ms = dl::gate_config{}.slow_ms;
        const disc_ui::answer answer = disc_ui::ask(display, menu, started, input, [&](u32 pressed, xmb_dialog& shown) -> std::optional<disc_ui::answer>
        {
            disc_refresh(sceKernelGetProcessTime());
            // The key search (started for every PS3 disc put in) may hold the shared reader for a long forward read; the
            // check waits behind it, so the gate waits too instead of saying the drive did not answer
            const std::int64_t now_ms = ds::detail::now_ms();
            const dl::step step = gate.update(ds::service::instance().last(), now_ms, (pressed & ORBIS_PAD_BUTTON_CROSS) != 0,
                (pressed & ORBIS_PAD_BUTTON_CIRCLE) != 0, dk::service::instance().busy());
            if (gate.want_check()) ds::service::instance().request_check();
            if (const std::string line = gate.take_log(); !line.empty()) ps4_startup_checkpoint(("disc lock: " + line).c_str());
            if (step == dl::step::boot) return disc_ui::answer::play;
            if (step == dl::step::back) return disc_ui::answer::back;
            // The PS3's "insert the disc" dialog: the gate's state, its reason lines as one note
            namespace xmb = utils::ps4::xmb;
            shown.icon = game_icon ? &game_icon : xmb_resource("item_tex_ps3game_folder");
            shown.title = game;
            switch (step)
            {
            case dl::step::missing:
            {
                shown.lines.emplace_back(i18n::tr(str::LAU_LOCK_COLOQUE_DISCO));
                shown.notes.emplace_back(i18n::tr(str::LAU_LOCK_INSTALADO_DO_DISCO));
                std::string reason;
                for (const std::string& part : gate.reason_lines()) reason += (reason.empty() ? "" : " ") + part;
                if (!reason.empty()) shown.notes.push_back(std::move(reason));
                shown.notes.emplace_back(i18n::tr(str::LAU_LOCK_TELA_MUDA));
                shown.buttons = {xmb::circle_button(i18n::tr(str::COM_VOLTAR))};
                break;
            }
            case dl::step::recognized:
                shown.lines.emplace_back(i18n::tr(str::LAU_LOCK_DISCO_RECONHECIDO));
                shown.notes.emplace_back(i18n::tr(str::LAU_LOCK_JA_DA_JOGAR));
                shown.buttons = {xmb::cross_button(i18n::tr(str::COM_JOGAR)), xmb::circle_button(i18n::tr(str::COM_VOLTAR))};
                break;
            default:
                shown.lines.emplace_back(i18n::tr(str::COM_VERIFICANDO_DISCO));
                shown.notes.emplace_back(i18n::tr(now_ms - pressed_ms >= slow_ms ? str::COM_LEITOR_ESTA_DEMORANDO_RESPONDER_AGUARDE
                                                                                 : str::COM_PROCURANDO_DISCO_DESTE_JOGO_LEITOR));
                shown.busy = true;
                shown.buttons = {xmb::circle_button(i18n::tr(str::COM_VOLTAR))};
                break;
            }
            return std::nullopt;
        });
        // Back on the XMB (or on the way to the game): no animation jump, no press carried over
        last_frame = sceKernelGetProcessTime();
        last_present = 0;
        last_activity = last_frame;
        wait_for_button_release = true;
        input.flush();
        if (answer == disc_ui::answer::play)
        {
            dl::sys::remember_verified(path);
            return true;
        }
        if (answer == disc_ui::answer::back) message = dl::message_text(*record);
        return false;
    };
    // 0.39: X on a game: dev_hdd0/game and the user's folder must take writes (a game creates its data and reads its user
    // name there; folders made over FTP refuse them and the game shows a black screen). A refusing folder is opened at
    // once (fs_repair: the app's chmod or SITE CHMOD through the FTP server); one that stays closed refuses the boot,
    // unless dev-hdd0-gate-off.flag. Two file operations per folder (create and remove a probe file). True: boot.
    const auto dev_hdd0_ready = [&](const std::string& path) -> bool
    {
        namespace fr = utils::ps4::fs_repair;
        const fr::fs_ops calls = fr::sys::fs();
        for (const char* folder : {"/data/rpcs3/dev_hdd0/game", "/data/rpcs3/dev_hdd0/home/00000001"})
        {
            std::uint32_t owner = 0;
            const int error = calls.probe(folder, owner);
            if (!error) continue;
            if (!fr::permission_error(error))
            {
                if (error != ENOENT) ps4_startup_checkpoint(fmt::format("XMB: '%s': %s refuses a test write (errno %d): booting", path, folder, error).c_str());
                continue;
            }
            if (ps4_reopen_refused_folder(folder)) continue;
            if (fs::is_file("/data/rpcs3/dev-hdd0-gate-off.flag"))
            {
                ps4_startup_checkpoint(fmt::format("XMB: '%s': %s refuses writes (errno %d): booting anyway (dev-hdd0-gate-off.flag)", path, folder, error).c_str());
                continue;
            }
            message = i18n::tr(str::LAU_DEV_HDD0_SEM_GRAVACAO);
            ps4_startup_checkpoint(fmt::format("XMB: '%s' not booted: %s refuses writes (errno %d); the game could not create its data", path, folder, error).c_str());
            return false;
        }
        return true;
    };
    // 0.39: X on an encrypted ISO without a key that decrypts it: the key is searched now on the image's own sectors
    // (as for a disc in the drive: key files, IRDs next to the image, the IRD database), on a worker with a dialog that
    // Circle cancels; it lands as <redump>/<image name>.dkey, which the loader reads first. True: boot.
    const auto iso_key_ready = [&](const std::string& path) -> bool
    {
        iso_key_status status = iso_key_status::OK;
        {
            iso_archive archive(path);
            if (!archive.is_valid()) return true; // the boot says why
            status = archive.get_key_status();
        }
        if (status == iso_key_status::OK) return true;
        ps4_startup_checkpoint(fmt::format("XMB: '%s' is an encrypted ISO %s: searching for its key", path,
            status == iso_key_status::MISSING ? "without a key" : "with a key file that does not decrypt it").c_str());
        const std::shared_ptr<dk::image_search> job = utils::ps4::governor::thread_pressure() < 2 ? dk::start_image_search(path) : nullptr;
        if (!job)
        {
            ps4_startup_checkpoint(fmt::format("XMB: '%s': no thread for the key search (close to the thread limit): booting without it", path).c_str());
            return true;
        }
        xmb_image game_icon;
        if (const std::size_t row = find_path(columns[6], path); row != no_row) game_icon = columns[6][row].artwork;
        std::string title = file_name_of(path);
        if (const std::size_t row = find_path(columns[6], path); row != no_row && !columns[6][row].title.empty()) title = columns[6][row].title;
        title = shorten(title, 70);
        const disc_ui::answer answer = disc_ui::ask(display, menu, started, input, [&](u32 pressed, xmb_dialog& shown) -> std::optional<disc_ui::answer>
        {
            if (job->finished.load(std::memory_order_acquire)) return disc_ui::answer::play;
            if (pressed & ORBIS_PAD_BUTTON_CIRCLE)
            {
                job->stop->store(true, std::memory_order_relaxed);
                return disc_ui::answer::back;
            }
            namespace xmb = utils::ps4::xmb;
            shown.icon = game_icon ? &game_icon : xmb_resource("item_tex_ps3game_folder");
            shown.title = title;
            shown.lines.emplace_back(i18n::tr(str::LAU_ISOKEY_BUSCANDO));
            shown.notes.emplace_back(i18n::tr(str::LAU_ISOKEY_CRIPTOGRAFADA));
            shown.notes.emplace_back(i18n::tr(str::LAU_ISOKEY_CIRCULO_CANCELA));
            shown.busy = true;
            shown.buttons = {xmb::circle_button(i18n::tr(str::COM_CANCELAR))};
            return std::nullopt;
        });
        last_frame = sceKernelGetProcessTime();
        last_present = 0;
        last_activity = last_frame;
        wait_for_button_release = true;
        input.flush();
        if (answer != disc_ui::answer::play)
        {
            message = i18n::tr(str::LAU_ISOKEY_CANCELADA);
            ps4_startup_checkpoint(fmt::format("XMB: '%s' not booted: the key search was cancelled", path).c_str());
            return false;
        }
        const dk::result& found = job->outcome_of;
        if (found.ok)
        {
            ps4_startup_checkpoint(fmt::format("XMB: '%s': %s (%s): booting", path, found.text, dk::to_string(found.what)).c_str());
            return true;
        }
        const std::string id = found.title_id.empty() ? std::string("?") : found.title_id;
        switch (found.what)
        {
        case dk::outcome::not_in_database:
        case dk::outcome::no_matching_key: message = i18n::trf(str::LAU_ISOKEY_NAO_ESTA_BASE, {id}); break;
        case dk::outcome::no_internet: message = i18n::tr(str::LAU_ISOKEY_SEM_INTERNET); break;
        case dk::outcome::disc_unreadable:
        case dk::outcome::cannot_verify: message = i18n::tr(str::LAU_ISOKEY_NAO_LEU); break;
        case dk::outcome::write_failed: message = found.text; break;
        default: message = i18n::tr(str::LAU_ISOKEY_TENTE_MAIS_TARDE); break;
        }
        ps4_startup_checkpoint(fmt::format("XMB: '%s' not booted: no key for the encrypted ISO (%s)", path, dk::to_string(found.what)).c_str());
        return false;
    };
    disc_refresh(sceKernelGetProcessTime());

    // 0.33: 'Instalar firmware do PS3'. X checks the PUP (a job: it reads the whole file), the question shows its version
    // and the installed one, X installs (another job), X during the install cancels it.
    const auto firmware_rows_refresh = [&]
    {
        firmware_state = firmware::installed(firmware_dev_flash);
        if (const std::size_t row = find_path(columns[1], "@firmware:install"); row != no_row) columns[1][row].subtitle = firmware_row_help(firmware_state);
        if (const std::size_t row = find_path(columns[1], "@info"); row != no_row)
            columns[1][row].subtitle = std::string(info_title) + " · " + firmware_info_text(firmware_state);
    };
    // Runs 'job' on the firmware thread. Without that thread (refused, or the process is close to the thread limit) the
    // job is dropped and 'refused_text' shown (null: silently, for the leftover removal). Never run on this thread: an
    // install takes minutes in which the menu could neither draw nor take the cancel, and TAR.cpp's extract starts a
    // must-succeed helper thread for files over 8 MiB, whose refusal here would end the application.
    const auto start_firmware = [&](std::unique_ptr<firmware::job> job, const std::string& text, const char* refused_text)
    {
        firmware_job = std::move(job);
        const char* refusal = "close to the thread limit";
        if (utils::ps4::governor::thread_pressure() < 2)
        {
            firmware_thread.emplace(stx::try_launch, "PS4 Firmware Installer", firmware_worker{firmware_job.get()});
            if (firmware_thread->ps4_try_start())
            {
                if (!text.empty()) message = firmware_progress = text;
                return;
            }
            firmware_thread.reset();
            refusal = "thread refused";
        }
        firmware_job.reset();
        ps4_startup_checkpoint(fmt::format("firmware: %s; %s", refusal,
            refused_text ? "job not started, the XMB asks to reopen the application" : "leftover removal postponed to the next XMB").c_str());
        if (refused_text) message = refused_text;
    };
    const auto firmware_no_thread_text = [] { return i18n::tr(str::LAU_FIRMWARE_SEM_THREAD); };
    const auto firmware_select = [&]
    {
        if (firmware_job)
        {
            // X during an install asks first; the next press answers (handle()).
            if (firmware_job->kind == firmware::task::install)
            {
                if (firmware_job->stop.load()) message = firmware_progress = firmware_progress_text(*firmware_job, firmware_installing);
                else if (!firmware::can_cancel(firmware_job->current())) message = firmware_too_late_text();
                else
                {
                    message = firmware_cancel_text();
                    firmware_cancel_asked = true;
                    ps4_startup_checkpoint("firmware: X during the install, asking whether to cancel");
                }
            }
            else message = i18n::tr(firmware_job->kind == firmware::task::check ? str::LAU_FIRMWARE_AGUARDE_VERIFICANDO
                                                                                : str::COM_AGUARDE_REMOVENDO_RESTOS_INSTALACAO_ANTERIOR);
            return;
        }
        if (firmware_target.empty())
        {
            message = i18n::tr(str::LAU_FIRMWARE_DEV_FLASH_FORA);
            return;
        }
        auto job = std::make_unique<firmware::job>();
        job->kind = firmware::task::check;
        job->target = firmware_target;
        firmware_installing.clear();
        ps4_startup_checkpoint("firmware: install requested from the XMB; looking for PS3UPDAT.PUP");
        start_firmware(std::move(job), i18n::tr(str::COM_PROCURANDO_PS3UPDAT_PUP_PENDRIVE_PS3), firmware_no_thread_text());
    };
    // The checked PUP: one question, answered by the next press (X installs, any other button cancels).
    const auto ask_firmware_install = [&](std::unique_ptr<firmware::package> pup, u32 invalid)
    {
        const std::string current = firmware_state.version.empty() ? std::string(i18n::tr(str::LAU_FIRMWARE_NENHUM)) : firmware_state.version;
        firmware_confirm_text = shorten(i18n::trf(str::LAU_FIRMWARE_INSTALAR_PERGUNTA, {pup->version, firmware::source_label(pup->path), current}), line_bytes(116));
        message = firmware_confirm_text;
        // As the PC's 'Old firmware detected' question: an older PUP installs, the row says it is not the newest.
        if (const std::size_t row = find_path(columns[1], "@firmware:install"); row != no_row)
        {
            std::string subtitle = i18n::trf(str::LAU_FIRMWARE_PUP_VERIFICADO, {pup->version, fmt::format("%.0f", double(pup->size) / 1e6)});
            if (firmware::older_than_newest(pup->version)) subtitle += i18n::trf(str::LAU_FIRMWARE_VERSAO_ANTIGA_SUFIXO, {firmware::newest_version});
            if (invalid) subtitle += i18n::trf(str::LAU_FIRMWARE_PUP_INVALIDOS_SUFIXO, {invalid});
            columns[1][row].subtitle = std::move(subtitle);
        }
        ps4_startup_checkpoint(fmt::format("firmware: asking to install %s from '%s' (installed: %s)", pup->version, pup->path, current).c_str());
        // 0.38: the same question as a system dialog (shown while the message line holds it)
        xmb_dialog question;
        question.title = i18n::tr(str::COM_FIRMWARE_PS3);
        question.lines.push_back(i18n::trf(str::LAU_FIRMWARE_DESEJA_INSTALAR, {pup->version}));
        question.notes.push_back(shorten(i18n::trf(str::LAU_FIRMWARE_ARQUIVO, {firmware::source_label(pup->path), fmt::format("%.0f", double(pup->size) / 1e6)}),
            line_bytes(100)));
        question.notes.push_back(i18n::trf(str::LAU_FIRMWARE_INSTALADO_AGORA, {current}));
        if (firmware::older_than_newest(pup->version)) question.notes.push_back(i18n::trf(str::LAU_FIRMWARE_VERSAO_ANTIGA, {firmware::newest_version}));
        if (invalid) question.notes.push_back(i18n::trf(str::LAU_FIRMWARE_PUP_INVALIDOS, {invalid}));
        question.buttons = {utils::ps4::xmb::cross_button(i18n::tr(str::COM_SIM)), utils::ps4::xmb::circle_button(i18n::tr(str::COM_NAO))};
        firmware_question = std::move(question);
        firmware_ready = std::move(pup);
    };
    const auto start_firmware_install = [&](std::unique_ptr<firmware::package> pup)
    {
        // The XMB can open while the previous game is still stopping (its modules come from dev_flash).
        if (!Emu.IsStopped(true))
        {
            message = i18n::tr(str::COM_JOGO_ANTERIOR_AINDA_ESTA_FECHANDO);
            return;
        }
        firmware_installing = pup->version;
        s_firmware_start_checked = true; // this run installs: its firmware is checked at the next start, not here
        ps4_startup_checkpoint(fmt::format("firmware: install of %s from '%s' confirmed", pup->version, pup->path).c_str());
        auto job = std::make_unique<firmware::job>();
        job->kind = firmware::task::install;
        job->target = firmware_target;
        job->pup = std::move(pup);
        start_firmware(std::move(job), i18n::trf(str::LAU_FIRMWARE_LENDO_PUP, {firmware_installing}), firmware_no_thread_text());
    };
    // 'died': the job's thread ended in a fatal error without a result (fatal.txt and startup.log have the reason).
    const auto firmware_finished = [&](firmware::job& job, bool died)
    {
        firmware_cancel_asked = false; // the result line replaces an unanswered cancel question
        if (died)
        {
            ps4_startup_checkpoint("firmware: the job's thread ended without a result (thread fatal)");
            // A swap cut short is undone now; the rest of the leftovers goes at the next XMB.
            const firmware::recovery restored = firmware::recover(firmware_target);
            if (!restored.log.empty()) ps4_startup_checkpoint(restored.log.c_str());
            firmware_rows_refresh();
            // The thread may have died before or after the swap: name the firmware that is installed now, or where the
            // previous one waits when it could not be put back.
            if (job.kind == firmware::task::install)
            {
                message = !restored.log.empty() && !restored.restored ? shorten(i18n::trf(str::LAU_FIRMWARE_ERRO_INTERNO, {i18n::raw(restored.text)}), line_bytes(116))
                    : i18n::trf(str::LAU_FIRMWARE_ERRO_INTERNO_INSTALADO, {i18n::raw(firmware_info_text(firmware_state))});
                show_notice(i18n::tr(str::COM_FIRMWARE_PS3), message); // 0.38
                display.load_font();
                loader.post({load_kind::resources});
                wake_loader();
            }
            else if (job.kind == firmware::task::check) message = i18n::tr(str::LAU_FIRMWARE_ERRO_INTERNO_VERIFICAR);
            return;
        }
        switch (job.kind)
        {
        case firmware::task::check:
            if (job.ok && job.pup) ask_firmware_install(std::move(job.pup), job.invalid);
            else if (!job.cancelled)
            {
                message = job.text;
                show_notice(i18n::tr(str::COM_FIRMWARE_PS3), message); // 0.38: why there is nothing to install
            }
            break;
        case firmware::task::install:
            message = job.text;
            // A swap whose move back failed left dev_flash missing: restore it now rather than at the next XMB, and
            // say so instead of "não voltou ao lugar".
            if (!job.ok)
                if (const firmware::recovery restored = firmware::recover(firmware_target); !restored.log.empty())
                {
                    ps4_startup_checkpoint(restored.log.c_str());
                    message = shorten(i18n::trf(job.cancelled ? str::LAU_FIRMWARE_CANCELADA_PREFIXO : str::LAU_FIRMWARE_NAO_INSTALADO_PREFIXO,
                        {i18n::raw(restored.text)}), line_bytes(116));
                }
            firmware_rows_refresh();
            if (job.ok)
            {
                // What the XMB took from the old dev_flash: its font now, the firmware images through the loader (the
                // image cache is keyed by the size and mtime of the .rco files, so it is rebuilt).
                display.load_font();
                loader.post({load_kind::resources});
                wake_loader();
            }
            // 0.38: the result as a system dialog until a button is pressed (the message line keeps it afterwards)
            {
                xmb_dialog result;
                split_into_dialog(message, result);
                if (job.ok)
                {
                    // The firmware's own line and its notes become the details of "concluída"
                    result.notes.insert(result.notes.begin(), result.lines.begin(), result.lines.end());
                    result.lines = {i18n::tr(str::COM_INSTALACAO_FOI_CONCLUIDA)};
                }
                result.title = job.ok && !job.version.empty() ? i18n::trf(str::LAU_FIRMWARE_PS3_VERSAO, {job.version}) : std::string(i18n::tr(str::COM_FIRMWARE_PS3));
                result.buttons = {utils::ps4::xmb::cross_button(i18n::tr(str::COM_OK))};
                notice = std::move(result);
            }
            break;
        case firmware::task::cleanup:
            break;
        }
    };
    // 0.38: a running firmware check or install as a system dialog. The install's bar: copying the PUP to /data is the
    // first quarter, its packages (each extracted, then flushed) the rest.
    const auto firmware_dialog = [&](const firmware::job& job) -> xmb_dialog
    {
        namespace xmb = utils::ps4::xmb;
        using firmware::stage;
        xmb_dialog d;
        const stage current = job.current();
        const bool install = job.kind == firmware::task::install;
        d.title = install && !firmware_installing.empty() ? i18n::trf(str::LAU_FIRMWARE_PS3_VERSAO, {firmware_installing}) : std::string(i18n::tr(str::COM_FIRMWARE_PS3));
        if (firmware_cancel_asked && message == firmware_cancel_text())
        {
            d.lines.emplace_back(i18n::tr(str::LAU_FIRMWARE_DESEJA_CANCELAR));
            d.notes.emplace_back(i18n::tr(str::COM_FIRMWARE_ATUAL_FICA_COMO));
            d.buttons = {xmb::cross_button(i18n::tr(str::COM_SIM)), xmb::circle_button(i18n::tr(str::COM_NAO))};
            return d;
        }
        if (job.stop.load() && firmware::can_cancel(current))
        {
            d.lines.emplace_back(i18n::tr(str::LAU_CANCELANDO));
            d.notes.emplace_back(i18n::tr(str::COM_FIRMWARE_ATUAL_FICA_COMO));
            d.busy = true;
            return d;
        }
        const u32 step = job.step.load(std::memory_order_relaxed), steps = job.steps.load(std::memory_order_relaxed);
        const u32 mib = job.mib.load(std::memory_order_relaxed), mib_total = job.mib_total.load(std::memory_order_relaxed);
        const auto bar = [&](int per_mille, std::string right)
        {
            d.progress = std::clamp(per_mille, 0, 1000);
            d.progress_left = xmb::percent_text(d.progress);
            d.progress_right = std::move(right);
        };
        switch (current)
        {
        case stage::verifying:
            d.lines.emplace_back(i18n::tr(str::LAU_FIRMWARE_VERIFICANDO_PUP));
            d.notes.push_back(shorten(job.shown(), 100));
            d.busy = true;
            break;
        case stage::reading:
            d.lines.emplace_back(i18n::tr(str::LAU_DISC_ROW_INSTALANDO));
            d.notes.emplace_back(i18n::tr(str::LAU_FIRMWARE_COPIANDO_PUP_PS4));
            bar(mib_total ? static_cast<int>(250ull * std::min(mib, mib_total) / mib_total) : 0, i18n::trf(str::LAU_FIRMWARE_MB_DE, {mib, mib_total}));
            break;
        case stage::extracting:
        case stage::flushing:
        {
            // Package 'at' of 'steps': its share starts when it is extracted and is half done when its flush begins
            d.lines.emplace_back(i18n::tr(str::LAU_DISC_ROW_INSTALANDO));
            const u64 at = std::clamp<u32>(step, 1, std::max<u32>(steps, 1));
            bar(steps ? 250 + static_cast<int>(750 * (2 * at - (current == stage::extracting ? 2 : 1)) / (2ull * steps)) : 250,
                i18n::trf(str::LAU_FIRMWARE_PACOTE_DE, {step, steps}));
            break;
        }
        case stage::swapping:
            d.lines.emplace_back(i18n::tr(str::LAU_FIRMWARE_CONCLUINDO));
            d.busy = true;
            break;
        case stage::removing_staging:
            d.lines.emplace_back(i18n::tr(str::LAU_FIRMWARE_APAGANDO_TEMPORARIOS_LINHA));
            d.busy = true;
            break;
        default:
            d.lines.emplace_back(i18n::tr(install ? str::LAU_FIRMWARE_PREPARANDO : str::LAU_FIRMWARE_PROCURANDO_PUP));
            if (!install) d.notes.emplace_back(i18n::tr(str::LAU_FIRMWARE_ONDE_PROCURA));
            d.busy = true;
            break;
        }
        if (install)
        {
            d.notes.emplace_back(i18n::tr(str::LAU_FIRMWARE_NAO_DESLIGUE));
            if (firmware::can_cancel(current)) d.buttons = {xmb::circle_button(i18n::tr(str::COM_CANCELAR))};
        }
        return d;
    };
    // 0.38: the system dialog the XMB shows instead of its menu, if any: a result waiting for X or Circle, the firmware
    // question, check and install, the disc install's cancel question and the exit question during it. While one is
    // shown only its own buttons act (handle()).
    const auto xmb_overlay = [&]() -> std::optional<xmb_dialog>
    {
        namespace xmb = utils::ps4::xmb;
        if (boot_next_until)
        {
            // The boot-next countdown: its bar fills while the seconds run down
            const u64 at = sceKernelGetProcessTime();
            const u64 total = u64{ps4_boot_next_seconds} * 1'000'000;
            const u64 left = *boot_next_until > at ? std::min(*boot_next_until - at, total) : 0;
            xmb_dialog d;
            d.title = boot_next_name;
            if (const std::size_t row = find_path(columns[6], preferred); row != no_row && columns[6][row].artwork) d.icon = &columns[6][row].artwork;
            d.lines.emplace_back(i18n::tr(str::LAU_BOOT_NEXT_REABERTO));
            d.notes.emplace_back(i18n::tr(str::LAU_BOOT_NEXT_BARRA));
            d.progress = static_cast<int>(1000 - left * 1000 / total);
            d.progress_left = fmt::format("%u s", static_cast<unsigned>((left + 999'999) / 1'000'000));
            d.buttons = {xmb::cross_button(i18n::tr(str::LAU_BOOT_NEXT_JOGAR_AGORA)), xmb::circle_button(i18n::tr(str::COM_CANCELAR))};
            return d;
        }
        if (notice && !game_dialog_line.empty() && !game_dialog_icon && !notice->lines.empty() && notice->lines.front() == game_dialog_line)
        {
            // The disc-removed or boot failure notice: the game's row (name, then its artwork when the loader has brought it)
            const std::size_t row = find_path(columns[6], preferred);
            if (row != no_row)
            {
                const xmb_entry& game = columns[6][row];
                if (notice->title.empty())
                    notice->title = shorten(game.serial.empty() ? game.title : fmt::format("%s (%s)", game.title, game.serial), 70);
                if (game.artwork) game_dialog_icon = game.artwork;
            }
            // Taken again every frame until the artwork is copied: a reload of the firmware images moves the folder icon
            notice->icon = game_dialog_icon ? &game_dialog_icon : (row != no_row || notice->icon) ? xmb_resource("item_tex_ps3game_folder") : nullptr;
        }
        if (notice) return notice;
        // the cheat list's delete question or a page of 'Ver linhas'
        if (cheats_open && cheat_dialog) return cheat_dialog;
        // the settings pages' questions (a warned value, leaving with edits not saved, the full help)
        if (settings_open && page_view.dialog()) return page_view.dialog();
        if (firmware_ready && firmware_question && message == firmware_confirm_text) return firmware_question;
        if (firmware_job && firmware_job->kind != firmware::task::cleanup) return firmware_dialog(*firmware_job);
        if (disc_ui::running(disc_job) && !settings_open && !savedata_open && !patches_open && !cheats_open &&
            (message == disc_ui::cancel_text() || message == disc_ui::exit_text()))
        {
            xmb_dialog d;
            d.icon = xmb_resource("item_tex_disc_install");
            d.title = shorten(disc_ui::disc_name(disc_job->disc()), 70);
            if (message == disc_ui::cancel_text())
            {
                d.lines.emplace_back(i18n::tr(str::COM_DESEJA_CANCELAR_INSTALACAO_DISCO));
                d.notes.emplace_back(i18n::tr(str::COM_JA_FOI_COPIADO_FICA_SALVO));
                d.buttons = {xmb::cross_button(i18n::tr(str::COM_SIM)), xmb::circle_button(i18n::tr(str::COM_NAO))};
            }
            else
            {
                d.lines.emplace_back(i18n::tr(str::LAU_DESEJA_SAIR_RPCS3));
                d.notes.emplace_back(i18n::tr(str::LAU_DISC_SAIR_PARA_CONTINUA));
                d.buttons = {xmb::cross_button(i18n::tr(str::COM_SAIR)), xmb::circle_button(i18n::tr(str::COM_VOLTAR))};
            }
            return d;
        }
        return std::nullopt;
    };

    const auto rpcn_progress = [](const ps4_rpcn::status& status)
    {
        return status.message.empty() ? std::string(i18n::tr(str::COM_CONECTANDO_RPCN)) : status.message;
    };
    const auto rpcn_finished = [&](const ps4_rpcn::status& status)
    {
        rpcn_busy = false;
        message = status.message.empty() ? std::string(i18n::tr(status.ok ? str::LAU_RPCN_OPERACAO_CONCLUIDA : str::LAU_RPCN_OPERACAO_NAO_CONCLUIDA)) : status.message;
        rpcn_result = message;
        rpcn_result_ok = status.ok;
        rpcn_progress_text.clear();
        psn_dirty = true; // the account line or the server may have changed
    };
    // Called with the result of ps4_rpcn::start(). A refused start (invalid input, a job already running, no thread)
    // leaves its reason in the status message, like a finished job.
    const auto rpcn_started = [&](bool started)
    {
        const auto status = ps4_rpcn::poll();
        if (status.busy)
        {
            rpcn_busy = true;
            message = rpcn_progress_text = rpcn_progress(status);
        }
        else
        {
            rpcn_finished(status); // refused, invalid input, or an import that ran at once
            if (!started) ps4_launcher_log.notice("RPCN job not started: %s", message);
        }
    };
    const auto cancel_form = [&](const char* why)
    {
        form = rpcn_form::none;
        form_user.clear();
        form_email.clear();
        message = why;
    };
    // 0.39: titles and prompts in the interface language; the system keyboard gets the logical text (it shapes Arabic itself)
    const auto open_keyboard = [&](ps4_ime::kind kind, str title, rpcn_form next, str prompt)
    {
        if (!ps4_ime::open(kind, i18n::tr(title), 0))
        {
            cancel_form(i18n::tr(str::LAU_RPCN_TECLADO_NAO_ABRIU));
            return;
        }
        form = next;
        message = i18n::tr(prompt);
    };
    const auto keyboard_result = [&](ps4_ime::state state, std::string& text)
    {
        const rpcn_form step = std::exchange(form, rpcn_form::none);
        if (state != ps4_ime::state::done || text.empty())
        {
            cancel_form(i18n::tr(state == ps4_ime::state::failed ? str::LAU_RPCN_TECLADO_FALHOU : str::COM_OPERACAO_RPCN_CANCELADA));
            return;
        }
        switch (step)
        {
        case rpcn_form::login_user:
            form_user = trimmed(text);
            open_keyboard(ps4_ime::kind::password, str::LAU_IME_SENHA_RPCN, rpcn_form::login_pass, str::LAU_IME_DIGITE_SENHA_RPCN);
            break;
        case rpcn_form::login_pass:
            rpcn_started(ps4_rpcn::start(ps4_rpcn::job::login, std::exchange(form_user, {}), std::move(text)));
            break;
        case rpcn_form::create_user:
            form_user = trimmed(text);
            if (!valid_rpcn_user(form_user))
            {
                cancel_form(i18n::tr(str::LAU_RPCN_USUARIO_INVALIDO));
                break;
            }
            open_keyboard(ps4_ime::kind::email, str::LAU_IME_EMAIL_NOVA_CONTA, rpcn_form::create_email, str::LAU_IME_DIGITE_EMAIL);
            break;
        case rpcn_form::create_email:
            form_email = trimmed(text);
            if (form_email.find('@') == std::string::npos)
            {
                cancel_form(i18n::tr(str::LAU_RPCN_EMAIL_INVALIDO));
                break;
            }
            form = rpcn_form::create_confirm;
            message = i18n::trf(str::LAU_RPCN_CRIAR_CONTA_PERGUNTA, {form_user, shorten(form_email, 32)});
            break;
        case rpcn_form::create_pass:
            // The only place an account is created: after the confirmation above and the password typed on the keyboard.
            rpcn_started(ps4_rpcn::start(ps4_rpcn::job::create, std::exchange(form_user, {}), std::move(text), std::exchange(form_email, {})));
            break;
        case rpcn_form::token:
            rpcn_started(ps4_rpcn::start(ps4_rpcn::job::token, {}, {}, {}, std::move(text)));
            break;
        default:
            break;
        }
    };
    const auto rpcn_select = [&](const std::string& path)
    {
        if (path == "@rpcn:online" || path == "@rpcn:upnp")
        {
            std::string event;
            message = path == "@rpcn:online" ? utils::ps4::online_rows::toggle_online(event) : utils::ps4::online_rows::toggle_upnp(event);
            if (!event.empty()) ps4_startup_checkpoint(event.c_str());
            psn_dirty = true;
            settings_dirty = true;
        }
        else if (path == "@rpcn:status") rpcn_started(ps4_rpcn::start(ps4_rpcn::job::test));
        else if (path == "@rpcn:resend") rpcn_started(ps4_rpcn::start(ps4_rpcn::job::resend));
        else if (path == "@rpcn:import") rpcn_started(ps4_rpcn::start(ps4_rpcn::job::import_yml));
        else if (path == "@rpcn:diag") rpcn_started(ps4_rpcn::start(ps4_rpcn::job::diag));
        else if (path == "@rpcn:server")
        {
            ps4_rpcn::cycle_host();
            const std::string host = ps4_rpcn::current_host();
            message = host.empty() ? std::string(i18n::tr(str::LAU_RPCN_NENHUM_SERVIDOR)) : i18n::trf(str::LAU_RPCN_SERVIDOR, {host});
            psn_dirty = true;
        }
        else if (path == "@rpcn:login")
            open_keyboard(ps4_ime::kind::username, str::LAU_IME_USUARIO_RPCN, rpcn_form::login_user, str::LAU_IME_DIGITE_USUARIO_RPCN);
        else if (path == "@rpcn:create")
            open_keyboard(ps4_ime::kind::username, str::LAU_IME_USUARIO_NOVA_CONTA, rpcn_form::create_user, str::LAU_IME_DIGITE_NOME_NOVA_CONTA);
        else if (path == "@rpcn:token")
            open_keyboard(ps4_ime::kind::token, str::LAU_IME_TOKEN_EMAIL, rpcn_form::token, str::LAU_IME_DIGITE_TOKEN);
    };

    // 0.33: game patches. The loader reads the list (the patch files take seconds to parse); the rows say so meanwhile.
    std::size_t patch_wanted = 0;    // row to select when the requested list arrives
    std::string patch_opening_text;  // message shown while it loads; replaced by the list summary
    const auto request_patches = [&](std::size_t keep_row)
    {
        patch_list.reset();
        patch_rows = patch_entries(nullptr, switch_icons, -1);
        patch_wanted = keep_row;
        notes_row = no_row;
        ++patch_generation;
        loader.post({load_kind::patch_list, patch_generation, patch_game.path, patch_game.serial, patch_game.title});
        wake_loader();
    };
    const auto open_patches = [&](const xmb_entry& game)
    {
        patches_open = true;
        patch_game = {game.path, game.title, game.subtitle, game.icon_name, {}, game.serial};
        patch_selection = 0;
        request_patches(0);
        message = patch_opening_text = utils::ps4::patches::clip(i18n::trf(str::LAU_PATCHES_DE_LENDO, {game.title, game.serial}), line_bytes(110));
        ps4_startup_checkpoint(fmt::format("XMB: patch list of %s requested ('%s')", game.serial, game.path).c_str());
    };
    const auto reload_patches = [&]
    {
        if (patches_open) request_patches(patch_selection);
    };
    const auto close_patches = [&]
    {
        patches_open = false;
        patch_list.reset();
        patch_rows.clear();
        notes_row = no_row;
    };
    const auto toggle_patch = [&](std::size_t index)
    {
        namespace patches = utils::ps4::patches;
        if (!patch_list || index >= patch_list->rows.size()) return;
        auto& game = *patch_list;
        // 0.34: a recommended patch in the automatic configuration is switched through ps4_recommended_off.txt
        if (patches::auto_managed(game, game.rows[index]))
        {
            const bool on = !game.rows[index].active();
            const bool saved = patches::set_recommended(game, index, on);
            const auto& row = game.rows[index];
            ps4_startup_checkpoint(fmt::format("patches: recommended '%s' %s for %s v%s (%u part(s)): %s", row.description, on ? "enabled" : "refused",
                game.serial, row.version_key, row.parts.size(), saved ? "patches/ps4_recommended_off.txt saved" : "SAVE FAILED").c_str());
            if (!saved)
            {
                message = i18n::tr(str::LAU_PATCH_ERRO_GRAVAR_RECOMENDADOS);
                reload_patches();
                return;
            }
            message = patches::clip(i18n::trf(on ? str::LAU_PATCH_LIGADO_RECOMENDADO : str::LAU_PATCH_DESLIGADO_NESTE_JOGO,
                {patches::clip(patches::flatten(row.description), 56)}), line_bytes(110));
            patch_rows = patch_entries(patch_list.get(), switch_icons, patch_cheats_on);
            return;
        }
        const bool on = !game.rows[index].on();
        const auto off = patches::set_row(game, index, on);
        const auto& row = game.rows[index];
        const bool saved = patches::save(game, row);
        ps4_startup_checkpoint(fmt::format("patches: '%s' %s for %s v%s (%u part(s), %u of its group switched off): %s", row.description,
            on ? "enabled" : "disabled", game.serial, row.version_key, row.parts.size(), off.size(), saved ? "patch_config.yml saved" : "SAVE FAILED").c_str());
        if (!saved)
        {
            message = i18n::tr(str::LAU_PATCH_ERRO_GRAVAR_CONFIG);
            reload_patches();
            return;
        }
        std::string text = i18n::trf(on ? str::LAU_PATCH_LIGADO_PROXIMO_BOOT : str::LAU_PATCH_DESLIGADO_PROXIMO_BOOT, {patches::clip(patches::flatten(row.description), 56)});
        if (!off.empty()) text += i18n::trf(str::LAU_PATCH_GRUPO_DESLIGADOS, {off.size()});
        message = patches::clip(std::move(text), line_bytes(110));
        patch_rows = patch_entries(patch_list.get(), switch_icons, patch_cheats_on);
    };
    const auto show_notes = [&](std::size_t index)
    {
        namespace patches = utils::ps4::patches;
        if (!patch_list || index >= patch_list->rows.size()) return;
        const auto& row = patch_list->rows[index];
        notes_page = notes_row == index ? notes_page + 1 : 0;
        notes_row = index;
        // 0.34: the reason of a recommendation comes first
        const std::string notes = patches::flatten(row.why.empty() ? row.notes : row.why + " " + row.notes);
        if (notes.empty())
        {
            message = row.author.empty() ? std::string(i18n::tr(str::LAU_PATCH_SEM_NOTAS))
                                         : patches::clip(i18n::trf(str::LAU_PATCH_SEM_NOTAS_AUTOR, {patches::flatten(row.author)}), line_bytes(110));
            return;
        }
        std::size_t pages = 1;
        const std::string text = patches::page(notes, notes_page, 92, pages);
        message = pages > 1 ? i18n::trf(str::LAU_PATCH_NOTAS_PAGINA, {notes_page % pages + 1, pages, i18n::raw(text)}) : i18n::trf(str::LAU_PATCH_NOTAS, {i18n::raw(text)});
    };
    const auto start_import = [&]
    {
        if (patch_import_busy) { message = i18n::tr(str::LAU_PATCH_IMPORTACAO_ANDAMENTO); return; }
        if (download_thread) { message = i18n::tr(str::LAU_PATCH_AGUARDE_DOWNLOAD); return; }
        patch_import_busy = true;
        loader.post({load_kind::patch_import, generation});
        wake_loader();
        message = i18n::tr(str::LAU_PATCH_IMPORTANDO);
        ps4_startup_checkpoint("patches: import requested");
    };
    const auto start_download = [&]
    {
        if (download_thread)
        {
            download->stop = true;
            message = i18n::tr(str::LAU_PATCH_CANCELANDO_DOWNLOAD);
            return;
        }
        if (!utils::ps4::patches::can_download())
        {
            message = i18n::tr(str::COM_DOWNLOAD_INDISPONIVEL_NESTE_PACOTE_SEM);
            return;
        }
        // 0.37: only a definite failure of the boot TLS self-test blocks it (running or not run: try the download)
        if (ps4_tls_selftest_ok() == false)
        {
            message = patch_download_tls_text();
            ps4_startup_checkpoint("patches: download not started: the boot TLS self-test failed (see its 'TLS self-test:' line)");
            return;
        }
        if (patch_import_busy) { message = i18n::tr(str::LAU_PATCH_AGUARDE_IMPORTACAO); return; }
        download = std::make_unique<utils::ps4::patches::download_state>();
        download_thread.emplace(stx::try_launch, "PS4 Patch Download", patch_download_worker{download.get()});
        if (!download_thread->ps4_try_start())
        {
            download_thread.reset();
            download.reset();
            message = i18n::tr(str::LAU_PATCH_THREAD_RECUSADA);
            ps4_startup_checkpoint("patches: download thread refused");
            return;
        }
        message = download_progress = i18n::tr(str::COM_BAIXANDO_PATCHES_RPCS3_PROCURANDO_RPCS3);
        ps4_startup_checkpoint("patches: download started");
    };

    // Cheats of one game (R2 on a game row, or the first row of its patch list). The loader reads the files; X switches
    // a cheat for the next start, Square opens its options, Triangle reads the files again; the system keyboard adds
    // and edits the console's own cheats (code lines separated by ';'). Every change is written at once.
    namespace cheats = utils::ps4::cheats;
    const auto cheat_message = [&](std::string text) { message = utils::ps4::patches::clip(std::move(text), line_bytes(116)); };
    const auto cheat_log = [&](std::string_view what, std::string_view name, bool saved, std::string_view why)
    {
        ps4_startup_checkpoint(fmt::format("cheats: '%s' %s for %s: %s", name, what, cheat_game.serial,
            saved ? std::string("saved") : "not changed (" + std::string(why) + ")").c_str());
    };
    const auto cheat_index = [&](u32 key) -> std::size_t
    {
        if (cheat_list)
            for (std::size_t i = 0; i < cheat_list->rows.size(); ++i)
                if (cheat_list->rows[i].key == key) return i;
        return no_row;
    };
    // 'announce': the message says the list is being read (and its header replaces that line)
    const auto request_cheats = [&](std::size_t keep_row, bool announce)
    {
        cheat_list.reset();
        cheat_rows = cheat_entries(nullptr);
        cheat_wanted = keep_row;
        cheat_options_open = false;
        cheat_option_rows.clear();
        cheat_dialog.reset();
        cheat_asking = cheat_ask::none;
        ++cheat_generation;
        if (announce) message = cheat_opening_text = i18n::tr(str::CHT_LER);
        loader.post({load_kind::cheat_list, cheat_generation, cheat_game.path, cheat_game.serial, cheat_game.title});
        wake_loader();
    };
    const auto open_cheats = [&](const xmb_entry& game, bool from_patches)
    {
        cheats_open = true;
        cheats_from_patches = from_patches;
        cheat_game = {game.path, game.title, game.subtitle, game.icon_name, {}, game.serial};
        cheat_selection = 0;
        request_cheats(0, true);
        ps4_startup_checkpoint(fmt::format("XMB: cheat list of %s requested ('%s')", game.serial, game.path).c_str());
    };
    const auto close_cheats = [&]
    {
        // back in the patch list, its first row counts what is on now
        if (cheats_from_patches && cheat_list && patches_open)
        {
            patch_cheats_on = static_cast<int>(cheats::count_on(*cheat_list));
            patch_rows = patch_entries(patch_list.get(), switch_icons, patch_cheats_on);
        }
        cheats_open = cheat_options_open = false;
        cheat_list.reset();
        cheat_rows.clear();
        cheat_option_rows.clear();
        cheat_dialog.reset();
        cheat_asking = cheat_ask::none;
    };
    // The rows again after a change (the cursor on 'select' unless no_row); the options follow their cheat or close
    const auto cheats_changed = [&](std::size_t select)
    {
        cheat_rows = cheat_entries(cheat_list.get());
        if (select != no_row) cheat_selection = select;
        cheat_selection = std::min(cheat_selection, cheat_rows.size() - 1);
        if (!cheat_options_open) return;
        const std::size_t index = cheat_index(cheat_options_key);
        cheat_option_rows = index != no_row ? cheat_option_entries(*cheat_list, index) : std::vector<xmb_entry>{};
        if (cheat_option_rows.empty()) cheat_options_open = false;
        else cheat_option_selection = std::min(cheat_option_selection, cheat_option_rows.size() - 1);
    };
    // The system keyboard for a cheat form: 'title' is the prompt, or why the text typed before was refused (it is the
    // message line too, under the keyboard)
    const auto open_cheat_keyboard = [&](cheat_form next, ps4_ime::kind kind, const std::string& title, unsigned limit, std::string_view initial)
    {
        if (!ps4_ime::open(kind, title, limit, initial))
        {
            cheat_keyboard = cheat_form::none;
            message = first_part(i18n::tr(str::LAU_RPCN_TECLADO_NAO_ABRIU)).first; // startup.log has the reason
            return;
        }
        cheat_keyboard = next;
        cheat_message(title);
    };
    // The number keyboard has digits only: signed and float values of a PC entry use the Latin one
    const auto value_keyboard = [](cheats::pc_type type)
    {
        const bool digits = type == cheats::pc_type::u8_ || type == cheats::pc_type::u16_ || type == cheats::pc_type::u32_ || type == cheats::pc_type::u64_;
        return digits ? ps4_ime::kind::number : ps4_ime::kind::hex;
    };
    // A PC cheats.yml entry has no value of its own: the player types it ('then_on': X on the row, switched on after)
    const auto open_value_keyboard = [&](std::size_t index, bool then_on)
    {
        const cheats::row& row = cheat_list->rows[index];
        cheat_form_key = row.key;
        cheat_form_then_on = then_on;
        open_cheat_keyboard(cheat_form::value, value_keyboard(row.pc_info.type), i18n::trf(str::CHT_TECLADO_VALOR,
            {utils::ps4::patches::clip(utils::ps4::patches::flatten(row.name), 48), cheats::pc_type_names[std::size_t(row.pc_info.type)]}), 0, row.value);
    };
    // Cheats are warned about online (RPCN), never blocked: the line that switched one on says so (after the whole
    // result when the line has room, else after its first part)
    const auto online_note = [](std::string& text)
    {
        if (!utils::ps4::online_enabled()) return;
        const std::string warning = i18n::tr(str::CHT_AVISO_ONLINE);
        if (text.size() + warning.size() + 4 > line_bytes(116)) text = first_part(text).first;
        text += " · " + warning;
    };
    const auto toggle_cheat = [&](std::size_t index)
    {
        if (!cheat_list || index >= cheat_list->rows.size()) return;
        cheats::game_cheats& game = *cheat_list;
        const cheats::row& row = game.rows[index];
        if (!row.on && row.valid && row.needs_value && row.value.empty())
        {
            open_value_keyboard(index, true);
            return;
        }
        const bool on = !row.on;
        const std::string name = row.name;
        std::string text;
        const bool saved = cheats::set_on(game, index, on, text);
        cheat_log(on ? "switched on" : "switched off", name, saved, text);
        if (saved && on) online_note(text);
        cheat_message(std::move(text));
        cheats_changed(index);
    };
    const auto open_cheat_options = [&](std::size_t index)
    {
        if (!cheat_list || index >= cheat_list->rows.size()) return;
        cheat_options_key = cheat_list->rows[index].key;
        cheat_option_rows = cheat_option_entries(*cheat_list, index);
        cheat_option_selection = 0;
        cheat_options_open = !cheat_option_rows.empty();
        cheat_message(i18n::trf(str::CFGU_CHANGED, {utils::ps4::patches::flatten(cheat_list->rows[index].name), str::CFGU_FOOTER_OPTIONS}));
    };
    // X (step 0) or Left/Right (step -1 / 1) on an option row
    const auto cheat_option = [&](const std::string& path, int step)
    {
        namespace patches = utils::ps4::patches;
        const std::size_t index = cheat_index(cheat_options_key);
        if (index == no_row)
        {
            cheat_options_open = false;
            return;
        }
        cheats::game_cheats& game = *cheat_list;
        const cheats::row& row = game.rows[index];
        const std::string name = patches::flatten(row.name);
        std::string text;
        if (path == "@copt:mode" || path == "@copt:period")
        {
            const bool forward = step >= 0;
            bool saved = false;
            if (path == "@copt:mode")
            {
                const auto at = std::find(cheat_modes.begin(), cheat_modes.end(), row.wanted);
                const std::size_t from = at == cheat_modes.end() ? 0 : static_cast<std::size_t>(at - cheat_modes.begin()), n = cheat_modes.size();
                const cheats::mode next = cheat_modes[forward ? (from + 1) % n : (from + n - 1) % n];
                saved = cheats::set_mode(game, index, next, 0, text);
                cheat_log("mode " + std::string(cheats::mode_token(next)), name, saved, text);
            }
            else
            {
                const auto& periods = cheats::periods_ms;
                std::size_t from = 0;
                while (from + 1 < periods.size() && periods[from] < row.period_ms) ++from;
                const std::size_t n = periods.size();
                const u32 next = periods[forward ? (from + 1) % n : (from + n - 1) % n];
                saved = cheats::set_mode(game, index, row.wanted, next, text);
                cheat_log(fmt::format("interval %u ms", next), name, saved, text);
            }
            cheat_message(std::move(text));
            cheats_changed(no_row);
            return;
        }
        if (step) return; // Left/Right step only the mode and the interval
        if (path == "@copt:value")
        {
            open_value_keyboard(index, false);
            return;
        }
        if (path == "@copt:lines")
        {
            std::size_t pages = 1;
            cheat_lines_page = 0;
            cheat_dialog = cheat_lines_dialog(game, index, 0, pages);
            cheat_dialog_key = row.key;
            cheat_asking = cheat_ask::lines;
            return;
        }
        if (path == "@copt:edit")
        {
            // the keyboard starts with the lines as they are; one it cannot hold whole is not offered (nothing is cut)
            const std::string lines = fmt::merge(row.code_text, "; ");
            if (lines.size() > cheat_code_chars)
            {
                cheat_message(i18n::trf(str::CFGU_CHANGED, {str::CHT_OPC_EDITAR, fmt::format("%u > %u", lines.size(), cheat_code_chars)}));
                return;
            }
            cheat_form_key = row.key;
            open_cheat_keyboard(cheat_form::edit_codes, ps4_ime::kind::text, i18n::tr(str::CHT_TECLADO_CODIGOS), cheat_code_chars, lines);
            return;
        }
        if (path == "@copt:rename")
        {
            cheat_form_key = row.key;
            open_cheat_keyboard(cheat_form::rename, ps4_ime::kind::text, i18n::tr(str::CHT_TECLADO_NOME), cheat_name_chars, name);
            return;
        }
        if (path == "@copt:copy")
        {
            const bool saved = cheats::copy_to_user(game, index, text);
            cheat_log("copied to the console's cheats", name, saved, text);
            cheat_message(std::move(text));
            cheats_changed(no_row);
            return;
        }
        if (path == "@copt:delete")
        {
            namespace xmb = utils::ps4::xmb;
            xmb_dialog d;
            d.title = patches::clip(name, 64);
            d.lines.push_back(i18n::trf(str::CHT_APAGAR_CONFIRMA, {patches::clip(name, 64)}));
            d.notes.push_back(row.source);
            d.buttons = {xmb::cross_button(i18n::tr(str::COM_APAGAR)), xmb::circle_button(i18n::tr(str::COM_CANCELAR))};
            cheat_dialog = std::move(d);
            cheat_dialog_key = row.key;
            cheat_asking = cheat_ask::remove;
        }
    };
    // A cheat form's text confirmed. A name asks for the codes next; codes and values are checked on a copy of the list
    // first, so a typing error opens the keyboard again with the text and the reason (with its line number).
    const auto cheat_form_done = [&](cheat_form step, const std::string& typed)
    {
        cheats::game_cheats& game = *cheat_list;
        std::string why, text;
        cheats::edit_files unused;
        const std::size_t index = cheat_index(cheat_form_key);
        switch (step)
        {
        case cheat_form::add_name:
            cheat_form_name = trimmed(typed);
            if (cheat_form_name.empty()) message = i18n::tr(str::CHT_ERR_NOME_VAZIO);
            else open_cheat_keyboard(cheat_form::add_codes, ps4_ime::kind::text, i18n::tr(str::CHT_TECLADO_CODIGOS), cheat_code_chars, {});
            break;
        case cheat_form::add_codes:
        {
            if (cheats::game_cheats trial = game; !cheats::edit_add_user(trial, cheat_form_name, typed, why, unused))
            {
                open_cheat_keyboard(cheat_form::add_codes, ps4_ime::kind::text, why, cheat_code_chars, typed);
                break;
            }
            const bool saved = cheats::add_user(game, cheat_form_name, typed, text);
            cheat_log("added", cheat_form_name, saved, text);
            cheat_message(std::move(text));
            cheats_changed(saved ? game.rows.size() - 1 : no_row);
            break;
        }
        case cheat_form::edit_codes:
        {
            if (index == no_row) break;
            if (cheats::game_cheats trial = game; !cheats::edit_codes(trial, index, typed, why, unused))
            {
                open_cheat_keyboard(cheat_form::edit_codes, ps4_ime::kind::text, why, cheat_code_chars, typed);
                break;
            }
            const std::string name = game.rows[index].name;
            const bool saved = cheats::edit_user(game, index, typed, text);
            cheat_log("codes edited", name, saved, text);
            // changed codes get a new key (and start off): the options stay with the cheat
            if (saved && cheat_options_key == cheat_form_key) cheat_options_key = game.rows[index].key;
            cheat_message(std::move(text));
            cheats_changed(no_row);
            break;
        }
        case cheat_form::rename:
        {
            if (index == no_row) break;
            const std::string name = game.rows[index].name;
            const bool saved = cheats::rename_user(game, index, typed, text);
            cheat_log("renamed to '" + trimmed(typed) + "'", name, saved, text);
            cheat_message(std::move(text));
            cheats_changed(no_row);
            break;
        }
        case cheat_form::value:
        {
            if (index == no_row) break;
            const cheats::pc_type type = game.rows[index].pc_info.type;
            if (cheats::game_cheats trial = game; !cheats::edit_value(trial, index, typed, why, unused))
            {
                open_cheat_keyboard(cheat_form::value, value_keyboard(type), why, 0, typed);
                break;
            }
            const std::string name = game.rows[index].name;
            bool saved = cheats::set_value(game, index, typed, text);
            if (saved && cheat_form_then_on)
            {
                saved = cheats::set_on(game, index, true, text);
                if (saved) online_note(text);
            }
            cheat_log(cheat_form_then_on ? "value typed and switched on" : "value typed", name, saved, text);
            cheat_message(std::move(text));
            cheats_changed(no_row);
            break;
        }
        case cheat_form::none:
            break;
        }
    };
    // The keyboard of a cheat form closed
    const auto cheat_keyboard_result = [&](ps4_ime::state state, const std::string& typed)
    {
        const cheat_form step = std::exchange(cheat_keyboard, cheat_form::none);
        if (state == ps4_ime::state::done && cheat_list) cheat_form_done(step, typed);
        else if (state == ps4_ime::state::failed) message = first_part(i18n::tr(str::LAU_RPCN_TECLADO_FALHOU)).first;
        else message.clear();
        // an import that finished meanwhile is read now (the typed text was not lost)
        if (cheat_keyboard == cheat_form::none && std::exchange(cheat_reload_due, false) && cheats_open) request_cheats(cheat_selection, false);
    };
    // Cheat files from USB:/cheats, USB:/rpcs3/cheats and /data/rpcs3/import/cheats (loader job), for the game of the
    // list or for every game of the library (a PC cheats.yml is split by the library's titles)
    const auto start_cheat_import = [&](bool this_game)
    {
        if (cheat_import_busy)
        {
            message = i18n::tr(str::CHT_IMPORTAR_HELP);
            return;
        }
        load_job job{load_kind::cheat_import, generation};
        if (this_game) job.serial = cheat_game.serial;
        for (const xmb_entry& row : columns[6])
            if (!row.path.empty() && row.path.front() != '@' && !row.serial.empty()) job.library.emplace_back(row.title, row.serial);
        cheat_import_busy = true;
        loader.post(std::move(job));
        wake_loader();
        message = i18n::tr(str::CHT_IMPORTAR_HELP);
        ps4_startup_checkpoint(this_game ? fmt::format("cheats: import for %s requested", cheat_game.serial).c_str() : "cheats: import for every game requested");
    };

    // 0.39: Configurações > Idioma: the next (step 1) or previous (-1) interface language, applied at once. Every text built
    // from the tables is built again: the columns of this function, the settings and patch rows; the library scan writes
    // the game and storage rows again in the new language (their descriptions are cached per language).
    const auto change_language = [&](int step)
    {
        constexpr unsigned values = unsigned(i18n::setting::ar) + 1;
        const auto next = i18n::setting((unsigned(i18n::current_setting()) + (step > 0 ? 1 : values - 1)) % values);
        const bool saved = ps4_set_interface_language(next);
        columns[0] = xmb_users(lightbar);
        columns[1] = settings_column();
        if (const std::size_t row = find_path(columns[1], "@ui-language"); row != no_row) selections[1] = row;
        empty_columns();
        psn_dirty = true;
        players_pending = true;
        settings_dirty = true;
        if (patches_open) patch_rows = patch_entries(patch_list.get(), switch_icons, patch_cheats_on);
        if (cheats_open) cheats_changed(no_row);
        disc_rows();
        refresh_library(true);
        message = saved ? i18n::trf(str::LANG_CHANGED, {i18n::raw(i18n::setting_label(next))}) : std::string(i18n::tr(str::LANG_SAVE_FAILED));
    };

    // Applies one frame of menu input. Returns the path to boot when a game was chosen.
    const auto handle = [&](uint32_t pressed, uint32_t buttons, u64 now) -> std::optional<std::string>
    {
        const view_t current = view();
        auto& entries = *current.entries;
        auto& selected = *current.selected;
        // 0.38: the boot-next countdown: X boots now, Circle cancels it (the XMB stays), other buttons do nothing
        if (boot_next_until)
        {
            if (pressed & ORBIS_PAD_BUTTON_CROSS)
            {
                ps4_startup_checkpoint("XMB: boot-next: X pressed, booting now");
                return preferred;
            }
            if (pressed & ORBIS_PAD_BUTTON_CIRCLE)
            {
                boot_next_until.reset();
                ps4_startup_checkpoint("XMB: boot-next countdown cancelled with Circle");
            }
            return std::nullopt;
        }
        // 0.38: a result dialog closes with X or Circle, which do nothing else; other buttons keep it
        if (notice)
        {
            if (pressed & (ORBIS_PAD_BUTTON_CROSS | ORBIS_PAD_BUTTON_CIRCLE)) notice.reset();
            return std::nullopt;
        }
        // The cheat delete question (X deletes) or a page of 'Ver linhas' (X pages on): X or Circle answer, other
        // buttons keep it
        if (cheat_asking != cheat_ask::none)
        {
            if (!(pressed & (ORBIS_PAD_BUTTON_CROSS | ORBIS_PAD_BUTTON_CIRCLE))) return std::nullopt;
            const cheat_ask what = std::exchange(cheat_asking, cheat_ask::none);
            cheat_dialog.reset();
            const std::size_t index = cheat_index(cheat_dialog_key);
            if (!(pressed & ORBIS_PAD_BUTTON_CROSS) || index == no_row) return std::nullopt;
            if (what == cheat_ask::lines)
            {
                std::size_t pages = 1;
                xmb_dialog next = cheat_lines_dialog(*cheat_list, index, cheat_lines_page + 1, pages);
                if (cheat_lines_page + 1 < pages)
                {
                    ++cheat_lines_page;
                    cheat_dialog = std::move(next);
                    cheat_asking = cheat_ask::lines;
                }
                return std::nullopt;
            }
            const std::string name = cheat_list->rows[index].name;
            std::string text;
            const bool saved = cheats::delete_user(*cheat_list, index, text);
            cheat_log("deleted", name, saved, text);
            cheat_message(std::move(text));
            if (saved)
            {
                cheat_options_open = false;
                cheat_option_rows.clear();
            }
            cheats_changed(saved ? index : no_row);
            return std::nullopt;
        }
        if (form == rpcn_form::create_confirm)
        {
            if (pressed & ORBIS_PAD_BUTTON_CROSS)
                open_keyboard(ps4_ime::kind::password, str::LAU_IME_SENHA_NOVA_CONTA, rpcn_form::create_pass, str::LAU_IME_DIGITE_SENHA_NOVA_CONTA);
            else if (pressed & ORBIS_PAD_BUTTON_CIRCLE)
                cancel_form(i18n::tr(str::LAU_RPCN_CRIACAO_CANCELADA));
            return std::nullopt;
        }
        // 0.33: the 'Limpar cache' question takes the next press: X removes the cache, any other button cancels. A
        // question that another message has replaced on screen counts as cancelled, and the press is handled as usual.
        if (cache_confirm)
        {
            const xmb_entry game = std::move(*cache_confirm);
            cache_confirm.reset();
            if (message == cache_confirm_text)
            {
                if (pressed & ORBIS_PAD_BUTTON_CROSS) start_cache_clear(game);
                else message = i18n::tr(str::LAU_CACHE_LIMPEZA_CANCELADA);
                return std::nullopt;
            }
        }
        // 0.33: the cancel question of a running install: X stops it (before its swap), Circle lets it go on.
        // 0.38: it is a system dialog: other buttons keep the question.
        if (firmware_cancel_asked)
        {
            if (message == firmware_cancel_text() && !(pressed & (ORBIS_PAD_BUTTON_CROSS | ORBIS_PAD_BUTTON_CIRCLE)) && firmware_job)
                return std::nullopt;
            firmware_cancel_asked = false;
            if (message == firmware_cancel_text())
            {
                const bool running = firmware_job && firmware_job->kind == firmware::task::install;
                if (running && (pressed & ORBIS_PAD_BUTTON_CROSS))
                {
                    if (!firmware::can_cancel(firmware_job->current())) message = firmware_too_late_text();
                    else
                    {
                        if (!firmware_job->stop.exchange(true)) ps4_startup_checkpoint("firmware: install cancel confirmed from the XMB");
                        message = firmware_progress = firmware_progress_text(*firmware_job, firmware_installing);
                    }
                }
                else
                {
                    ps4_startup_checkpoint("firmware: install goes on (cancel not confirmed)");
                    // The progress line takes over again.
                    message = firmware_progress = running ? firmware_progress_text(*firmware_job, firmware_installing) : std::string{};
                }
                return std::nullopt;
            }
        }
        // 0.33: the firmware question works the same way. Either answer closes the PUP file unless X installs it.
        // 0.38: X (Sim) or Circle (Não) answer the dialog; other buttons keep it.
        if (firmware_ready)
        {
            if (message == firmware_confirm_text && !(pressed & (ORBIS_PAD_BUTTON_CROSS | ORBIS_PAD_BUTTON_CIRCLE))) return std::nullopt;
            std::unique_ptr<firmware::package> pup = std::move(firmware_ready);
            firmware_question.reset();
            firmware_rows_refresh(); // the row showed the checked PUP
            if (message == firmware_confirm_text)
            {
                if (pressed & ORBIS_PAD_BUTTON_CROSS) start_firmware_install(std::move(pup));
                else
                {
                    message = i18n::tr(str::LAU_FIRMWARE_INSTALACAO_CANCELADA);
                    ps4_startup_checkpoint("firmware: install declined at the question");
                }
                return std::nullopt;
            }
        }
        // 0.38: a firmware check or install is a system dialog (firmware_dialog): during an install X or Circle asks
        // whether to cancel it (until its swap begins); nothing else acts until it ends.
        if (firmware_job && firmware_job->kind != firmware::task::cleanup)
        {
            if (firmware_job->kind == firmware::task::install && (pressed & (ORBIS_PAD_BUTTON_CROSS | ORBIS_PAD_BUTTON_CIRCLE)) &&
                !firmware_job->stop.load() && firmware::can_cancel(firmware_job->current()))
            {
                message = firmware_cancel_text();
                firmware_cancel_asked = true;
                ps4_startup_checkpoint("firmware: cancel asked on the install dialog");
            }
            return std::nullopt;
        }
        // 0.37: Circle where the disc install shows (its progress line or its row) asks to cancel it.
        // 0.38: the question is a system dialog: X (Sim) cancels (what was copied stays for the next install), Circle
        // (Não) lets it go on, other buttons keep it.
        if (disc_ui::running(disc_job) && !settings_open && !savedata_open && !patches_open && !cheats_open)
        {
            if (message == disc_ui::cancel_text())
            {
                if (pressed & ORBIS_PAD_BUTTON_CROSS)
                {
                    if (!disc_job->cancel.exchange(true)) ps4_startup_checkpoint("XMB: PS3 disc install cancel confirmed with X");
                    message = i18n::tr(str::LAU_DISC_CANCELANDO_FICA_SALVO);
                }
                else if (pressed & ORBIS_PAD_BUTTON_CIRCLE) message = disc_progress;
                return std::nullopt;
            }
            if ((pressed & ORBIS_PAD_BUTTON_CIRCLE) && ((!disc_progress.empty() && message == disc_progress) || entries[selected].path == disc_row_path))
            {
                message = disc_ui::cancel_text();
                return std::nullopt;
            }
            // The exit question (X on 'Sair' during the install): X again leaves, Circle stays
            if (message == disc_ui::exit_text() && !((pressed & ORBIS_PAD_BUTTON_CROSS) && entries[selected].path == "@exit"))
            {
                if (pressed & ORBIS_PAD_BUTTON_CIRCLE) message.clear();
                return std::nullopt;
            }
        }
        // A browser tab of the settings menu is a settings page (its lists, sub-pages and questions take every button)
        if (settings_open && (settings.page_open() || page_view.dialog()))
        {
            const settings_page_view::outcome done = page_view.handle(pressed, buttons, settings, message);
            if (done == settings_page_view::outcome::saved)
            {
                settings_open = false;
                ps4_startup_checkpoint(message.c_str());
            }
            else if (done == settings_page_view::outcome::discarded)
            {
                settings_open = false;
                message = i18n::tr(str::LAU_SETTINGS_EDICAO_ENCERRADA);
                ps4_startup_checkpoint(fmt::format("settings: left without saving (%u pending change(s) dropped)", settings.pending()).c_str());
            }
            if (const std::string event = settings.take_event(); !event.empty()) ps4_startup_checkpoint(event.c_str());
            settings_dirty = true;
            psn_dirty = true;
            return std::nullopt;
        }
        // Options no longer leaves the application (it used to reach _Exit => CE-34878-0
        // and is also part of the L1+R1+Options return-to-XMB chord). Use the "Sair" item.
        if (pressed & ORBIS_PAD_BUTTON_CIRCLE)
        {
            if (settings_open)
            {
                // At the top level: closing with edits that are not saved asks first (Salvar / Descartar / Continuar)
                if (settings.back()) message.clear();
                else if (settings.pending()) page_view.ask_save(settings);
                else { settings_open = false; message = i18n::tr(str::LAU_SETTINGS_EDICAO_ENCERRADA); }
                settings_dirty = true;
                return std::nullopt;
            }
            // From a cheat's options back to its list; from the list back to the patch list it was opened from (or
            // to the games)
            if (cheats_open)
            {
                message.clear();
                if (cheat_options_open)
                {
                    cheat_options_open = false;
                    cheat_option_rows.clear();
                    return std::nullopt;
                }
                const bool to_patches = cheats_from_patches && patches_open;
                close_cheats();
                if (to_patches) return std::nullopt;
            }
            if (patches_open) close_patches();
            savedata_open = false; category = 6; message.clear();
            return std::nullopt;
        }
        if (settings_open)
        {
            // L2/R2 on the PS4 tab: the previous / next tab, as on the pages
            if (const auto tab = settings.current_tab(); tab && (pressed & (ORBIS_PAD_BUTTON_L2 | ORBIS_PAD_BUTTON_R2)))
            {
                const int n = int(utils::ps4::cfg_catalog::tab_count), step = (pressed & ORBIS_PAD_BUTTON_R2) ? 1 : -1;
                settings.open_tab(utils::ps4::cfg_catalog::tab((int(*tab) + step + n) % n));
                message.clear();
                settings_dirty = true;
                return std::nullopt;
            }
            // 0.32: the 'Indisponível no PS4' rows trail their group and the cursor stops before them.
            if (pressed & ORBIS_PAD_BUTTON_DOWN) { if (selected + 1 < settings.selectable_rows(entries.size())) ++selected; message.clear(); }
            if (pressed & ORBIS_PAD_BUTTON_UP) { if (selected > 0) --selected; message.clear(); }
            if (pressed & ORBIS_PAD_BUTTON_LEFT) { message = settings.adjust(-1, buttons & ORBIS_PAD_BUTTON_R1); settings_dirty = true; }
            if (pressed & ORBIS_PAD_BUTTON_RIGHT) { message = settings.adjust(1, buttons & ORBIS_PAD_BUTTON_R1); settings_dirty = true; }
            if ((pressed & ORBIS_PAD_BUTTON_SQUARE) && settings.reset_option())
            {
                message = i18n::tr(str::LAU_SETTINGS_VALOR_RESTAURADO);
                settings_dirty = true;
            }
            if (pressed & ORBIS_PAD_BUTTON_CROSS)
            {
                if (settings.select(message))
                {
                    settings_open = false;
                    ps4_startup_checkpoint(message.c_str());
                }
                // 0.29: the automatic-configuration switch and 'Voltar para automático' change files without
                // closing the menu; record them too.
                if (const std::string event = settings.take_event(); !event.empty()) ps4_startup_checkpoint(event.c_str());
                settings_dirty = true;
                psn_dirty = true; // the menu has the same online switches as the PlayStation Network column
            }
            return std::nullopt;
        }
        auto& choice = entries[selected];
        // In a cheat list Square opens the options of a cheat and Triangle reads the files again; in the options
        // Left/Right step the mode and the interval
        if (cheats_open)
        {
            if (pressed & ORBIS_PAD_BUTTON_SQUARE)
            {
                if (const std::size_t index = row_index(choice.path, "@cheat:"); !cheat_options_open && index != no_row) open_cheat_options(index);
                return std::nullopt;
            }
            if (pressed & ORBIS_PAD_BUTTON_TRIANGLE)
            {
                if (!cheat_options_open) request_cheats(cheat_selection, true);
                return std::nullopt;
            }
            if (cheat_options_open && (pressed & (ORBIS_PAD_BUTTON_LEFT | ORBIS_PAD_BUTTON_RIGHT)))
            {
                cheat_option(choice.path, (pressed & ORBIS_PAD_BUTTON_RIGHT) ? 1 : -1);
                return std::nullopt;
            }
        }
        // 0.33: in a patch list, Square pages through the notes of the patch and Triangle reads the files again.
        if (patches_open && (pressed & ORBIS_PAD_BUTTON_SQUARE))
        {
            if (const std::size_t index = row_index(choice.path, "@patch:"); index != no_row) show_notes(index);
            return std::nullopt;
        }
        if (patches_open && (pressed & ORBIS_PAD_BUTTON_TRIANGLE))
        {
            reload_patches();
            message = i18n::tr(str::COM_RELENDO_PATCHES);
            return std::nullopt;
        }
        // 0.33: R1 on a game opens its patches (the title ID and the path come from the library scan).
        if ((pressed & ORBIS_PAD_BUTTON_R1) && !savedata_open && !patches_open && !cheats_open && category == 6 &&
            !choice.path.empty() && choice.path.front() != '@')
        {
            if (!utils::ps4::settings_menu::valid_title_id(choice.serial))
            {
                message = i18n::tr(str::LAU_PATCH_SEM_TITLE_ID);
                return std::nullopt;
            }
            open_patches(choice);
            return std::nullopt;
        }
        // R2 on a game opens its cheats (the files are named after its title ID)
        if ((pressed & ORBIS_PAD_BUTTON_R2) && !savedata_open && !patches_open && !cheats_open && category == 6 &&
            !choice.path.empty() && choice.path.front() != '@')
        {
            if (!utils::ps4::settings_menu::valid_title_id(choice.serial) || !cheats::valid_serial(choice.serial))
            {
                message = i18n::tr(str::LAU_PATCH_SEM_TITLE_ID);
                return std::nullopt;
            }
            open_cheats(choice, false);
            return std::nullopt;
        }
        // 0.37: Square on the PS3 disc row asks to install the disc (again, when it is installed)
        if ((pressed & ORBIS_PAD_BUTTON_SQUARE) && !savedata_open && choice.path == disc_row_path)
        {
            disc_select(true);
            return std::nullopt;
        }
        if ((pressed & ORBIS_PAD_BUTTON_SQUARE) && !savedata_open && category == 6 &&
            !choice.path.empty() && choice.path.front() != '@')
        {
            // The title ID comes from the library scan; the game is not parsed again.
            const std::string serial = choice.serial;
            if (!utils::ps4::settings_menu::valid_title_id(serial))
            {
                message = i18n::tr(str::LAU_SETTINGS_SEM_TITLE_ID);
                return std::nullopt;
            }
            settings.open(serial); settings_open = true; settings_dirty = true; players_pending = true; category = 1;
            page_view.reset();
            settings.browser().set_fonts(settings_font_names());
            message = i18n::trf(str::LAU_SETTINGS_DO_JOGO, {serial, i18n::raw(settings.status())});
            return std::nullopt;
        }
        // 0.33: L1 on a game asks to remove its cache (PPU modules, SPU and shader caches).
        if ((pressed & ORBIS_PAD_BUTTON_L1) && !savedata_open && !patches_open && !cheats_open && category == 6 &&
            !choice.path.empty() && choice.path.front() != '@')
        {
            ask_cache_clear(choice);
            return std::nullopt;
        }
        if (pressed & ORBIS_PAD_BUTTON_TRIANGLE)
        {
            refresh_library(true);
            return std::nullopt;
        }
        // 0.39: on the language row Left/Right change the value (X too, below); Up/Down leave the row
        if ((pressed & (ORBIS_PAD_BUTTON_LEFT | ORBIS_PAD_BUTTON_RIGHT)) && !savedata_open && !patches_open && choice.path == "@ui-language")
        {
            change_language((pressed & ORBIS_PAD_BUTTON_RIGHT) ? 1 : -1);
            return std::nullopt;
        }
        if ((pressed & ORBIS_PAD_BUTTON_LEFT) && category > 0)
        {
            if (cheats_open) close_cheats();
            if (patches_open) close_patches();
            savedata_open = false; --category; message.clear(); return std::nullopt;
        }
        if ((pressed & ORBIS_PAD_BUTTON_RIGHT) && category < 9)
        {
            if (cheats_open) close_cheats();
            if (patches_open) close_patches();
            savedata_open = false; ++category; message.clear(); return std::nullopt;
        }
        if (pressed & ORBIS_PAD_BUTTON_DOWN) { if (selected + 1 < entries.size()) ++selected; message.clear(); notes_row = no_row; }
        if (pressed & ORBIS_PAD_BUTTON_UP) { if (selected > 0) --selected; message.clear(); notes_row = no_row; }
        if (!(pressed & ORBIS_PAD_BUTTON_CROSS)) return std::nullopt;
        std::string path = entries[selected].path;
        // 0.37: X on the PS3 disc row boots the image installed from it; anything else is answered there
        if (path == disc_row_path)
        {
            path = disc_select(false);
            if (path.empty()) return std::nullopt;
        }
        if (path == "@exit")
        {
            // 0.33: leaving during an install would cut it short (the previous firmware stays, but the work is lost).
            if (firmware_job && firmware_job->kind == firmware::task::install)
            {
                message = i18n::tr(str::COM_AGUARDE_INSTALACAO_FIRMWARE_TERMINAR_INSTALAR);
                return std::nullopt;
            }
            // 0.37: a disc install stops when the application closes (it goes on from there next time): X twice
            if (disc_ui::running(disc_job) && message != disc_ui::exit_text())
            {
                message = disc_ui::exit_text();
                return std::nullopt;
            }
            perf.report(now, "exit item");
            if (!ps4_try_leave_application("XMB exit item"))
                message = i18n::tr(str::LAU_USE_BOTAO_PS_SAIR);
            return std::nullopt;
        }
        if (path == "@ui-language")
        {
            change_language(1);
            return std::nullopt;
        }
        // A running RPCN job holds the account file and a server connection: no game boot, no package install
        // and no second job until it ends. Moving around the menu stays possible, because a server that stops
        // answering can keep a job waiting for a long time.
        const bool rpcn_job_row = path.starts_with("@rpcn:") && path != "@rpcn:online" && path != "@rpcn:upnp";
        if (rpcn_busy && (rpcn_job_row || (!path.empty() && path.front() != '@')))
        {
            message = i18n::tr(str::LAU_RPCN_AGUARDE_OPERACAO);
            return std::nullopt;
        }
        // 0.33: a game boot would close the menu and stop a patch download half-way.
        if (download_thread && !path.empty() && path.front() != '@')
        {
            message = i18n::tr(str::LAU_PATCH_AGUARDE_DOWNLOAD_CANCELA);
            return std::nullopt;
        }
        // 0.33: a game boot closes the menu; a cache removal or an import that has not started yet would be dropped.
        if ((cache_clear_busy || config_db_busy) && !path.empty() && path.front() != '@')
        {
            message = i18n::tr(cache_clear_busy ? str::LAU_CACHE_AGUARDE_LIMPEZA : str::LAU_CONFIGDB_AGUARDE_IMPORTACAO);
            return std::nullopt;
        }
        // 0.33: no game boots while a firmware job runs: the boot would close the menu (stopping the job) and load modules
        // from a dev_flash that is being replaced.
        if (firmware_job && !path.empty() && path.front() != '@')
        {
            message = i18n::tr(firmware_job->kind == firmware::task::install ? str::COM_AGUARDE_INSTALACAO_FIRMWARE_TERMINAR_INSTALAR
                : firmware_job->kind == firmware::task::check ? str::LAU_FIRMWARE_AGUARDE_VERIFICACAO
                : str::COM_AGUARDE_REMOVENDO_RESTOS_INSTALACAO_ANTERIOR);
            return std::nullopt;
        }
        // 0.37: nor while a PS3 disc installs (the boot would close the menu while the copy holds the drive)
        if (disc_ui::running(disc_job) && !path.empty() && path.front() != '@')
        {
            message = disc_ui::boot_refused_text();
            return std::nullopt;
        }
        if (path == "@firmware:install") { firmware_select(); return std::nullopt; }
        if (path.starts_with("@patch:")) { toggle_patch(row_index(path, "@patch:")); return std::nullopt; }
        // Cheats: the first row of the patch list opens the game's list; in it X switches a cheat, adds one, imports
        if (path == "@cheats:open") { open_cheats(patch_game, true); return std::nullopt; }
        if (path.starts_with("@cheat:")) { toggle_cheat(row_index(path, "@cheat:")); return std::nullopt; }
        if (path.starts_with("@copt:")) { cheat_option(path, 0); return std::nullopt; }
        if (path == "@cheats:add")
        {
            if (cheat_list) open_cheat_keyboard(cheat_form::add_name, ps4_ime::kind::text, i18n::tr(str::CHT_TECLADO_NOME), cheat_name_chars, {});
            return std::nullopt;
        }
        if (path == "@cheats:import" || path == "@cheats:import-all") { start_cheat_import(path == "@cheats:import"); return std::nullopt; }
        if (path == "@cheats:none")
        {
            // where the files go, their names, and why Action Replay codes of the PS3 are not memory cheats
            show_notice(i18n::trf(str::CHT_LISTA, {cheat_game.title}), i18n::tr(str::CHT_NENHUM));
            notice->notes.emplace_back(i18n::tr(str::CHT_NENHUM_HELP));
            notice->notes.emplace_back(i18n::tr(str::CHT_ACTION_REPLAY));
            return std::nullopt;
        }
        if (path == "@patches:import") { start_import(); return std::nullopt; }
        if (path == "@patches:download") { start_download(); return std::nullopt; }
        if (path == "@configdb:import") { start_config_db_import(); return std::nullopt; }
        if (path == "@savedata")
        {
            savedata = {{{}, i18n::tr(str::LAU_CARREGANDO_DADOS_GUARDADOS), "", "item_tex_ps3util", {}}};
            savedata_open = true; save_selection = 0; message.clear();
            loader.post({load_kind::savedata, generation});
            wake_loader();
            return std::nullopt;
        }
        if (path == "@refresh")
        {
            refresh_library(true);
            category = 6;
            return std::nullopt;
        }
        if (path == "@licenses")
        {
            loader.post({load_kind::licenses, generation, "@licenses"});
            wake_loader();
            message = i18n::tr(str::LAU_PROCURANDO_LICENCAS);
            return std::nullopt;
        }
        if (path == "@info")
        {
            // 0.29: measured CPU clock (one ~100 ms probe per process, cached). The XMB message
            // is one line (~110 characters), so at ~1.6 GHz X alternates between the Boost Mode hint (shown first)
            // and the version line.
            const std::string cpu_clock = utils::ps4::cpu_clock_text();
            // 0.33: and the installed PS3 firmware (vsh/etc/version.txt, as the PC title bar).
            const std::string info = fmt::format("%s %s · %s · %s", info_title, ps4_build_version, cpu_clock, firmware_info_text(firmware_state));
            const std::string hint = utils::ps4::cpu_boost_hint() ? cpu_clock + " · " + utils::ps4::cpu_boost_hint_text : std::string{};
            message = !hint.empty() && message != hint ? hint : info;
            ps4_log_cpu_probe_once();
            return std::nullopt;
        }
        if (path == "@settings")
        {
            settings.open(); settings_open = true; settings_dirty = true; players_pending = true;
            page_view.reset();
            settings.browser().set_fonts(settings_font_names());
            message = i18n::trf(str::LAU_SETTINGS_GLOBAIS, {i18n::raw(settings.status())});
            return std::nullopt;
        }
        if (path.starts_with("@player"))
        {
            const char* add_players = i18n::tr(str::LAU_PLAYERS_MAIS_JOGADORES);
            message = message == add_players ? i18n::tr(str::LAU_PLAYERS_TROCAR_USUARIO) : add_players;
            return std::nullopt;
        }
        if (path == "@storage") { message = i18n::tr(str::LAU_STORAGE_MENSAGEM); return std::nullopt; }
        if (path == "@disc:keys") { disc_keys_select(); return std::nullopt; }
        if (path == "@disc:probe")
        {
            // 0.37: the test reads the drive too; it waits for a disc install
            if (disc_ui::running(disc_job)) message = i18n::tr(str::LAU_PROBE_AGUARDE_INSTALACAO);
            else disc_probe_select();
            return std::nullopt;
        }
        // 0.33: rows of the USB drives (storage column, and the game column for drives without games)
        if (path.starts_with("@usb:"))
        {
            if (path.starts_with("@usb:games:")) message = i18n::tr(str::LAU_USB_MSG_JOGOS_ABREM_DIRETO);
            else if (path.starts_with("@usb:empty:")) message = i18n::tr(str::LAU_USB_MSG_COPIE_ISO);
            else if (path.starts_with("@usb:read:")) message = i18n::tr(str::LAU_USB_MSG_NAO_LIDO);
            else if (path == "@usb:none") message = i18n::tr(str::LAU_USB_MSG_CONECTE);
            else message = i18n::tr(str::LAU_USB_MSG_CRIE_NA_RAIZ);
            return std::nullopt;
        }
        if (path.starts_with("@rpcn:")) { rpcn_select(path); return std::nullopt; }
        if (path.starts_with("@save:")) { message = i18n::trf(str::LAU_DADOS_GUARDADOS_DE, {entries[selected].title}); return std::nullopt; }
        if (path.empty()) return std::nullopt;
        // 0.33: the drive may have been pulled out after the last probe (at most 2 s ago); one stat instead of a failed boot.
        if (utils::ps4::usb_drive_of(path) >= 0 && !fs::exists(path))
        {
            message = i18n::trf(str::LAU_USB_DESCONECTADO_CONECTE, {utils::ps4::origin_label(path)});
            ps4_startup_checkpoint(fmt::format("XMB: '%s' not booted: its USB drive is not connected", path).c_str());
            return std::nullopt;
        }
        // 0.33: nothing is ever written to a USB drive (see writes_to_usb). PARAM.SFO is read again here, not taken from
        // the library cache, as the boot will read it.
        if (const auto kind = game_kind_of(path); utils::ps4::usb_drive_of(path) >= 0 &&
            (kind == utils::ps4::game_kind::folder || kind == utils::ps4::game_kind::executable) &&
            writes_to_usb(path, psf::get_string(psf::load_object(metadata_base(path) + "PARAM.SFO"), "CATEGORY", "")))
        {
            message = i18n::tr(str::LAU_JOGO_PSN_PASTA_USB);
            ps4_startup_checkpoint(fmt::format("XMB: '%s' not booted: PSN (HG) folder game on a USB drive would write its data to the drive", path).c_str());
            return std::nullopt;
        }
        // 0.34: a disc folder without its PS3_DISC.SFB can only fail to boot ("Invalid file or folder"); say what is missing.
        if (game_kind_of(path) == utils::ps4::game_kind::folder &&
            disc_folder_without_sfb(path, psf::get_string(psf::load_object(path + "/PS3_GAME/PARAM.SFO"), "CATEGORY", "")))
        {
            message = shorten(i18n::trf(str::LAU_FALTA_SFB_COPIE, {file_name_of(path)}), line_bytes(116));
            ps4_startup_checkpoint(fmt::format("XMB: '%s' not booted: missing PS3_DISC.SFB", path).c_str());
            return std::nullopt;
        }
        // 0.37: a PKG still being copied (or missing a part, or not a PS3 one) cannot install: say why and stay in the menu.
        if (game_kind_of(path) == utils::ps4::game_kind::package)
            if (const auto check = check_package_file(path); !check.ready())
            {
                message = shorten(check.text, line_bytes(116));
                ps4_startup_checkpoint(fmt::format("XMB: '%s' not installed: %s", path, check.log).c_str());
                return std::nullopt;
            }
        // 0.39: an ISO shorter than the image it describes boots into a fatal error, and a disc image without a valid
        // PS3_DISC.SFB can only fail as an invalid file or folder: say why and stay in the menu (also for a row of a
        // library cache written before these checks)
        if (iso_file_on_disk(path))
        {
            if (const auto size = iso_completeness(path); size.short_file())
            {
                message = utils::ps4::disc_image::incomplete_text(size);
                ps4_startup_checkpoint(fmt::format("XMB: '%s' not booted: the image is incomplete (file %llu bytes, image %llu bytes)", path, size.file,
                    size.image).c_str());
                return std::nullopt;
            }
            if (ps4_iso_without_sfb(path))
            {
                message = i18n::tr(str::LAU_ISO_SEM_SFB_PSN_CONVERTIDO);
                ps4_startup_checkpoint(fmt::format("XMB: '%s' not booted: ISO without a valid PS3_DISC.SFB (CATEGORY DG)", path).c_str());
                return std::nullopt;
            }
        }
        if (game_kind_of(path) != utils::ps4::game_kind::package && !dev_hdd0_ready(path)) return std::nullopt;
        // 0.37: a game installed from a PS3 disc boots only with a disc of it in the drive (disc_lock); the TITLE_ID comes
        // from the library row (also when X was on the drive row, which boots the installed image)
        if (utils::ps4::file_kind(path) == utils::ps4::game_kind::iso)
        {
            std::string serial;
            if (const std::size_t row = find_path(columns[6], path); row != no_row) serial = columns[6][row].serial;
            if (serial.empty() && path == disc_now.installed) serial = disc_ui::disc_id(disc_now);
            // A row without a TITLE_ID (its scan could not read PARAM.SFO): the image's own, which the check before BootGame
            // reads as well, so that check never refuses a game the XMB let through (three refusals end the application)
            if (serial.empty())
                if (iso_archive archive(path); archive.is_valid()) serial = std::string(psf::get_string(archive.open_psf("PS3_GAME/PARAM.SFO"), "TITLE_ID", ""));
            if (!lock_gate(path, serial)) return std::nullopt;
            if (iso_file_on_disk(path) && !iso_key_ready(path)) return std::nullopt;
        }
        ps4_startup_checkpoint(fmt::format("XMB: selected '%s' (%s)", path, utils::ps4::origin_label(path)).c_str());
        ps4_log_cpu_probe_once();
        if (game_kind_of(path) != utils::ps4::game_kind::package) ps4_game_selection_hints_once();
        return path;
    };

    // 0.33: what a failed or interrupted firmware install left behind (a few hundred MB) is removed in the background.
    if (firmware::has_leftovers(firmware_target))
    {
        auto job = std::make_unique<firmware::job>();
        job->kind = firmware::task::cleanup;
        job->target = firmware_target;
        start_firmware(std::move(job), {}, nullptr);
    }

    for (;;)
    {
        if (ps4_shutdown_requested())
        {
            finish("shutdown");
            return {};
        }
        ps4_poll_system_service();
        u64 now = sceKernelGetProcessTime();
        const bool keyboard = ps4_ime::active();
        // While the home screen or another system UI owns the display, draw nothing and flip nothing. The
        // on-screen keyboard is a system overlay too, but it is drawn over the app's frames, which must keep coming.
        if (ps4_in_background() && !keyboard)
        {
            if (!backgrounded)
            {
                backgrounded = true;
                background_since = now;
                ps4_startup_checkpoint("XMB: paused while the system UI is in front");
            }
            sceKernelUsleep(50'000);
            last_frame = sceKernelGetProcessTime(); // no animation jump on return
            continue;
        }
        if (backgrounded)
        {
            backgrounded = false;
            perf.paused_us += now - background_since;
            last_present = 0;
            last_activity = now;
            wait_for_button_release = true;
            input.flush();
            ps4_startup_checkpoint(fmt::format("XMB: resumed after %.1f s behind the system UI", double(now - background_since) / 1e6).c_str());
        }

        for (auto& result : loader.take())
        {
            stats.add(result);
            switch (result.kind)
            {
            case load_kind::resources:
                for (auto& [name, img] : result.resources) menu.resources.insert_or_assign(name, std::move(img));
                resources_ready = true;
                break;
            case load_kind::licenses:
                if (result.path == "@licenses")
                {
                    message = result.imported ? i18n::trf(str::LAU_LICENCAS_IMPORTADAS_EXDATA, {result.imported})
                        : std::string(i18n::tr(str::LAU_LICENCAS_NENHUMA_NOVA));
                }
                else if (result.imported && result.path == "@refresh") message = i18n::trf(str::LAU_LICENCAS_IMPORTADAS, {result.imported});
                // 0.26: licenses copied over FTP/USB since the last visit are installed without asking.
                else if (result.imported && message.empty()) message = i18n::trf(str::LAU_LICENCAS_IMPORTADAS_DLC, {result.imported});
                break;
            case load_kind::library:
            {
                if (result.generation != generation) break;
                library_busy = false;
                // 0.33: the 'Armazenamento' column follows the drives this scan read (cursor kept on the same row).
                if (!result.storage.empty())
                {
                    const auto& storage = columns[7];
                    const std::string kept = storage.empty() ? std::string{} : storage[std::min(selections[7], storage.size() - 1)].path;
                    columns[7] = std::move(result.storage);
                    const std::size_t at = find_path(columns[7], kept);
                    selections[7] = at != no_row ? at : 0;
                }
                // 0.33: drives that came or went replace only an empty line, the refresh notice or an earlier drive message.
                if (message == usb_refresh_text()) message.clear();
                if (!result.text.empty() && (message.empty() || message == usb_message || message == refreshing_text()))
                    message = usb_message = result.text;
                auto& games = columns[6];
                const std::string keep = games.empty() ? std::string{} : games[std::min(selections[6], games.size() - 1)].path;
                // Same generation: the rows already shown came from the cache, so their thumbnails stay valid.
                const bool carry = shown_generation == result.generation;
                // 0.33: a hot-plug refresh keeps the thumbnails of games stored on the PS4 (no flicker when a drive comes or
                // goes); rows of USB drives load theirs again, as a drive may have been swapped.
                const bool carry_internal = !carry && result.generation == hotplug_generation;
                if (carry || carry_internal)
                    for (auto& entry : result.entries)
                        if (carry || utils::ps4::usb_drive_of(entry.path) < 0)
                            if (const std::size_t row = find_path(games, entry.path); row != no_row && games[row].artwork) entry.artwork = std::move(games[row].artwork);
                games = std::move(result.entries);
                // 0.37: the PS3 disc row stays ahead of the games (and its storage row keeps the drive's state)
                {
                    std::size_t unused = 0;
                    disc_ui::apply_rows(games, unused, columns[7], disc_ui::texts(disc_now, disc_job));
                }
                // 0.37: the padlocks of the new rows, with the lock records they were bound with
                if (result.locks) lock_registry = *result.locks;
                lock_rows(now);
                shown_generation = result.generation;
                std::size_t row = find_path(games, keep);
                if (row == no_row && library_first) row = find_path(games, preferred);
                selections[6] = row != no_row ? row : default_game_row(games);
                if (!carry) artwork_attempted.clear();
                icon_anchor = no_row;
                if (message == refreshing_text()) message.clear();
                ps4_startup_checkpoint(fmt::format("XMB: library %s (%u rows in %.1f ms; %u unchanged, %u read)", library_first ? "ready" : "refreshed",
                    games.size(), double(result.cost_us) / 1000, result.cached, result.read).c_str());
                library_first = false;
                break;
            }
            case load_kind::icon:
                if (result.generation != generation || shown_generation != generation) break;
                if (const std::size_t row = find_path(columns[6], result.path); row != no_row) columns[6][row].artwork = std::move(result.artwork);
                break;
            case load_kind::savedata:
                if (!savedata_open || result.entries.empty()) break;
                savedata = std::move(result.entries);
                save_selection = std::min(save_selection, savedata.size() - 1);
                break;
            case load_kind::patch_list:
            {
                if (!patches_open || result.generation != patch_generation || !result.patches) break;
                patch_list = std::move(result.patches);
                patch_cheats_on = result.cheats ? static_cast<int>(utils::ps4::cheats::count_on(*result.cheats)) : -1;
                patch_rows = patch_entries(patch_list.get(), switch_icons, patch_cheats_on);
                patch_selection = std::min(patch_wanted, patch_rows.size() - 1);
                const auto& game = *patch_list;
                const std::string version = game.app_version.empty() ? std::string(i18n::tr(str::COM_VERSAO_DESCONHECIDA)) : "v" + game.app_version;
                // 0.34: recommended patches the next boot switches on by itself (the line keeps the X hint within 110 bytes)
                const auto automatic = static_cast<std::size_t>(std::count_if(game.rows.begin(), game.rows.end(), [](const auto& row) { return row.auto_on; }));
                std::string summary = game.rows.empty() ? i18n::trf(str::LAU_PATCH_RESUMO_NENHUM, {game.serial, version})
                    : automatic ? i18n::trf(str::LAU_PATCH_RESUMO_RECOMENDADOS, {game.rows.size(), game.serial, version, automatic})
                    : i18n::trf(str::LAU_PATCH_RESUMO, {game.rows.size(), game.serial, version});
                if (game.invalid_files) summary += i18n::tr(str::LAU_PATCH_RESUMO_ARQUIVO_ERROS);
                if (message.empty() || message == patch_opening_text || message == i18n::tr(str::COM_RELENDO_PATCHES))
                    message = utils::ps4::patches::clip(std::move(summary), line_bytes(110));
                patch_opening_text.clear();
                break;
            }
            case load_kind::patch_import:
                patch_import_busy = false;
                message = result.text;
                if (result.imported) reload_patches();
                break;
            case load_kind::cheat_list:
            {
                if (!cheats_open || result.generation != cheat_generation || !result.cheats) break;
                cheat_list = std::move(result.cheats);
                cheat_rows = cheat_entries(cheat_list.get());
                cheat_selection = std::min(cheat_wanted, cheat_rows.size() - 1);
                const cheats::game_cheats& game = *cheat_list;
                if (message.empty() || message == cheat_opening_text) message = cheat_header(game, utils::ps4::online_enabled());
                cheat_opening_text.clear();
                const auto invalid = static_cast<unsigned>(std::count_if(game.rows.begin(), game.rows.end(), [](const cheats::row& row) { return !row.valid; }));
                ps4_startup_checkpoint(fmt::format("cheats: list of %s v%s: %u cheat(s), %u on, %u invalid, %u file(s), %u note(s)", game.serial,
                    game.app_version, game.rows.size(), cheats::count_on(game), invalid, game.files.size(), game.notes.size()).c_str());
                break;
            }
            case load_kind::cheat_import:
                cheat_import_busy = false;
                cheat_message(result.text);
                // the list shown takes the new files (after the keyboard, when a form has it)
                if (result.imported && cheats_open)
                {
                    if (cheat_keyboard != cheat_form::none) cheat_reload_due = true;
                    else request_cheats(cheat_selection, false);
                }
                break;
            // 0.33: a drive came, went or was swapped since the last scan: read the library again (only the drives
            // that changed are walked).
            case load_kind::usb_poll:
            {
                usb_poll_pending = false;
                // A library scan queued after this probe (Triangle) probes the drives itself; a hot-plug refresh here
                // would replace it and turn a full re-read of every drive into one of the changed drives only.
                if ((result.text.empty() && result.changes.empty()) || library_busy) break;
                // 0.37: a PS3 disc install keeps rewriting its .part in /data/rpcs3/games: that folder is read again when
                // the install ends (a USB change or a finished PKG still refreshes at once)
                if (result.text.empty() && result.completed.empty() && disc_ui::running(disc_job)) break;
                // A drive that keeps coming and going (loose cable, failing drive) is probed every 10 s instead; the
                // same for a /data folder that keeps changing (an FTP upload of many files).
                const u64 at = sceKernelGetProcessTime();
                if (last_hotplug && at - last_hotplug < 6'000'000) next_usb_poll = at + 10'000'000;
                last_hotplug = at;
                if (!result.text.empty()) ps4_startup_checkpoint(fmt::format("USB hot-plug: %s; reading the library again", result.text).c_str());
                // 0.37: /data library folders and PKGs that were still being copied
                if (!result.changes.empty()) ps4_startup_checkpoint(fmt::format("library change: %s; reading the library again", result.changes).c_str());
                refresh_library(false, !result.text.empty());
                if (!result.completed.empty()) message = shorten(i18n::trf(str::LAU_PKG_PRONTO_INSTALAR, {result.completed}), line_bytes(116));
                break;
            }
            case load_kind::cache_clear:
                cache_clear_busy = false;
                message = result.text;
                break;
            case load_kind::config_db_import:
                config_db_busy = false;
                message = result.text;
                if (const std::size_t row = find_path(columns[1], "@configdb:import"); row != no_row) columns[1][row].subtitle = config_db::row_help();
                break;
            default:
                break;
            }
        }
        // Give the loader a moment for the firmware icons before the first frame, so the menu does not open with
        // placeholder boxes. Inline, the resources are the first job.
        if (first_frame && !resources_ready)
        {
            if (loader.inline_mode)
            {
                if (loader.run_one()) continue;
            }
            else if (now - started < 150'000)
            {
                sceKernelUsleep(5'000);
                continue;
            }
        }

        // On-screen keyboard and RPCN jobs.
        if (keyboard)
        {
            std::string text;
            const auto state = ps4_ime::poll(text);
            if (state == ps4_ime::state::done || state == ps4_ime::state::cancelled || state == ps4_ime::state::failed)
            {
                // a settings value typed for a page row or a matrix cell, a cheat form step, else an RPCN form step
                if (page_view.keyboard_open())
                {
                    page_view.keyboard_done(state, text, settings, message);
                    settings_dirty = true;
                }
                else if (cheat_keyboard != cheat_form::none) cheat_keyboard_result(state, text);
                else keyboard_result(state, text);
                wait_for_button_release = true;
                input.flush();
            }
            wipe_secret(text);
        }
        else if (form != rpcn_form::none && form != rpcn_form::create_confirm)
        {
            cancel_form(i18n::tr(str::COM_OPERACAO_RPCN_CANCELADA)); // the keyboard closed without reporting a result
        }
        // a cheat form whose keyboard closed without reporting a result
        if (!keyboard && !ps4_ime::active() && cheat_keyboard != cheat_form::none) cheat_keyboard = cheat_form::none;
        if (rpcn_busy)
        {
            const auto status = ps4_rpcn::poll();
            if (!status.busy) rpcn_finished(status);
            else
            {
                // Progress replaces only itself or an empty line; a menu message stays until the next input.
                std::string progress = rpcn_progress(status);
                if (message.empty() || message == rpcn_progress_text) message = progress;
                rpcn_progress_text = std::move(progress);
            }
        }
        // 0.33: patch.yml download. The result replaces any message; progress only replaces itself or an empty line.
        if (download_thread)
        {
            if (download->done.load(std::memory_order_acquire))
            {
                download_thread.reset(); // already finished: the join returns at once
                message = download->text;
                // 0.39: a write failure in a patches folder that refuses writes names the folder (fs_repair); the result
                // kind, not the (translated) text, says it was a write failure
                if (!download->ok && download->kind == utils::ps4::patches::result_kind::write_error)
                {
                    namespace fr = utils::ps4::fs_repair;
                    std::uint32_t owner = 0;
                    if (fr::permission_error(fr::sys::fs().probe("/data/rpcs3/patches", owner)))
                    {
                        message = fr::no_write_text("/data/rpcs3/patches");
                        ps4_startup_checkpoint("patches: download: no write permission in /data/rpcs3/patches");
                    }
                }
                const bool changed = download->changed;
                download.reset();
                download_progress.clear();
                if (changed) reload_patches();
            }
            else
            {
                const double received = double(download->received.load(std::memory_order_relaxed)) / 1e6;
                const double expected = double(download->expected.load(std::memory_order_relaxed)) / 1e6;
                std::string progress;
                switch (download->stage.load(std::memory_order_relaxed))
                {
                case 0: progress = i18n::tr(str::COM_BAIXANDO_PATCHES_RPCS3_PROCURANDO_RPCS3); break;
                case 1:
                    progress = received <= 0 ? std::string(i18n::tr(str::LAU_DOWNLOAD_CONECTANDO))
                        : expected > 0 ? i18n::trf(str::LAU_DOWNLOAD_MB_DE, {fmt::format("%.1f", received), fmt::format("%.1f", expected)})
                        : i18n::trf(str::LAU_DOWNLOAD_MB, {fmt::format("%.1f", received)});
                    break;
                case 2: progress = i18n::tr(str::LAU_DOWNLOAD_CONFERINDO); break;
                default: progress = i18n::tr(str::LAU_DOWNLOAD_GRAVANDO); break;
                }
                progress += i18n::tr(str::LAU_DOWNLOAD_CANCELA_SUFIXO);
                if (message.empty() || message == download_progress) message = progress;
                download_progress = std::move(progress);
            }
        }
        // 0.33: firmware job. The result replaces any message; progress only replaces itself or an empty line. The thread
        // state is read before 'done': a thread that ended without 'done' died in a fatal error.
        if (firmware_job)
        {
            const bool thread_ended = firmware_thread && static_cast<thread_state>(*firmware_thread) >= thread_state::errored;
            const bool finished = firmware_job->done.load(std::memory_order_acquire);
            if (finished || thread_ended)
            {
                firmware_thread.reset(); // already finished: the join returns at once
                const std::unique_ptr<firmware::job> ended = std::move(firmware_job);
                firmware_progress.clear();
                firmware_finished(*ended, !finished);
            }
            else if (firmware_job->kind != firmware::task::cleanup)
            {
                if (std::string progress = firmware_progress_text(*firmware_job, firmware_installing); !progress.empty())
                {
                    if (message.empty() || message == firmware_progress) message = progress;
                    firmware_progress = std::move(progress);
                }
            }
        }
        // 0.34: physical-disc jobs. A result replaces any message and brings the menu back to full rate to show it.
        if (disc_probe)
        {
            if (const auto report = disc_probe->poll())
            {
                message = utils::ps4::disc_probe::report_text(*report);
                disc_probe.reset();
                last_activity = now;
            }
        }
        if (disc_keys_job && disc_keys_job->done.load(std::memory_order_acquire))
        {
            message = std::move(disc_keys_job->text);
            disc_keys_job.reset();
            last_activity = now;
            show_notice(i18n::tr(str::COM_CHAVES_DISCO_DKEY), message); // 0.38: the result as a system dialog
            // 0.37: a disc in the drive gets its key checked again (an imported .dkey may be the one it lacked)
            dk::service::instance().forget();
        }
        // 0.37: the PS3 disc drive, its key search and its install (every 500 ms)
        disc_refresh(now);

        // 0.33: USB hot-plug. One probe every 2 s on the loader (two stat calls per mount point), never while a
        // library scan is pending (it probes the drives itself) or the previous probe has not answered. The loop does
        // not get here while the system UI is in front.
        if (!library_busy && !usb_poll_pending && now >= next_usb_poll)
        {
            next_usb_poll = now + 2'000'000;
            usb_poll_pending = true;
            loader.post({load_kind::usb_poll, generation});
            wake_loader();
        }

        // 0.38: the boot-next countdown ended: the game of boot-next.txt boots
        if (boot_next_until && now >= *boot_next_until)
        {
            ps4_startup_checkpoint("XMB: boot-next countdown ended; booting");
            finish("boot-next");
            return preferred;
        }

        // Input before drawing, so a press shows in the frame drawn right after it.
        uint32_t pressed = input.poll(now, category == 6 && !settings_open && !savedata_open);
        if (pressed || input.buttons || keyboard) last_activity = now;
        if (wait_for_button_release)
        {
            // A return-to-XMB chord includes Options. Consume its held buttons
            // until release so reopening the menu cannot immediately quit the app.
            if (input.buttons) pressed = 0;
            else wait_for_button_release = false;
        }
        if (keyboard || ps4_ime::active()) pressed = 0; // the system keyboard has the controllers
        if (pressed)
        {
            if (auto path = handle(pressed, input.buttons, now))
            {
                finish("game selected");
                return *path;
            }
        }

        // Seats: the 'Usuários' rows and the settings row follow ps4_pads.
        {
            const unsigned seats = ps4_pads::generation(), connected = ps4_pads::connected_count();
            any_pad = connected > 0;
            if (seats != users_generation || connected != users_connected)
            {
                users_generation = seats;
                users_connected = connected;
                columns[0] = xmb_users(lightbar);
                players_pending = true;
            }
            if (settings_open && players_pending)
            {
                settings.set_players(ps4_pads::summary(), static_cast<int>(connected));
                settings_dirty = true;
                players_pending = false;
            }
        }
        if (category == 8 && psn_dirty)
        {
            columns[8] = xmb_psn();
            psn_dirty = false;
        }

        // Thumbnails near the selected game, requested when the selection moves. Rows farther than 32 lines
        // drop theirs; the icon cache brings them back quickly.
        if (category == 6 && !savedata_open && !settings_open && shown_generation == generation && !columns[6].empty())
        {
            auto& games = columns[6];
            const std::size_t selected = std::min(selections[6], games.size() - 1);
            if (selected != icon_anchor)
            {
                icon_anchor = selected;
                for (const auto& dropped : loader.drop_icons()) artwork_attempted.erase(dropped);
                for (std::size_t i = 0; i < games.size(); ++i)
                {
                    if ((i + 32 < selected || i > selected + 32) && games[i].artwork)
                    {
                        games[i].artwork = {};
                        artwork_attempted.erase(games[i].path);
                    }
                }
                std::vector<std::string> paths;
                for (int offset : {0, -1, 1, -2, 2, -3, 3, -4, 4, -5, 5})
                {
                    const auto row = static_cast<std::ptrdiff_t>(selected) + offset;
                    if (row < 0 || std::size_t(row) >= games.size()) continue;
                    const auto& entry = games[std::size_t(row)];
                    if (entry.path.empty() || entry.path.front() == '@' || entry.artwork || !artwork_attempted.insert(entry.path).second) continue;
                    paths.push_back(entry.path);
                }
                if (!paths.empty())
                {
                    loader.post_icons(paths, generation);
                    wake_loader();
                }
            }
        }

        // 0.38: a system dialog over the waves instead of the menu (xmb_overlay)
        const std::optional<xmb_dialog> overlay = xmb_overlay();
        // 0.34: idle pacing (xmb_idle_tier). Frames start one tier interval apart; until the next one is due the loop
        // sleeps in slices of at most 25 ms and polls the controllers again, so a press is drawn at once.
        // 0.38: a dialog with a bar or the busy clock keeps at least 30 fps.
        unsigned tier = xmb_idle_tier(now - last_activity);
        if (overlay && (overlay->busy || overlay->progress >= 0)) tier = std::min(tier, 1u);
        // 0.39: an XMB left alone at 2 fps checks a drive without a PS3 disc every 30 s instead of every 5 s
        if (const bool idle = tier >= 3; idle != drive_idle)
        {
            drive_idle = idle;
            disc_ui::ds::service::instance().set_idle(idle);
        }
        if (tier && !first_frame && last_draw_start && now < last_draw_start + xmb_idle_interval_us[tier])
        {
            if (!(loader.inline_mode && loader.run_one()))
                sceKernelUsleep(static_cast<u32>(std::min<u64>(last_draw_start + xmb_idle_interval_us[tier] - now, 25'000)));
            continue;
        }

        const view_t current = view();
        if (const std::time_t second = std::time(nullptr); second != clock_second)
        {
            clock_second = second;
            if (const auto* local = std::localtime(&second))
            {
                char value[32]{};
                std::strftime(value, sizeof(value), "%d/%m  %H:%M", local);
                clock = value;
            }
        }
        // 0.33: with no message, a game row and the patch list name their extra buttons on the message line.
        std::string_view shown = message;
        std::string game_hint;
        if (shown.empty() && !settings_open && !savedata_open)
        {
            const auto& row = (*current.entries)[*current.selected];
            if (cheats_open)
            {
                if (!cheat_options_open && row.path.starts_with("@cheat:")) shown = i18n::tr(str::CHT_DICA_LISTA);
            }
            else if (patches_open && row.path.starts_with("@patch:")) shown = i18n::tr(str::LAU_HINT_PATCH_ROW);
            else if (!patches_open && category == 6 && !row.path.empty() && row.path.front() != '@' && !row.serial.empty())
            {
                // R2 (cheats) follows the game row's other buttons
                if (row.locked) shown = utils::ps4::disc_lock::row_hint();
                else shown = game_hint = std::string(i18n::tr(str::LAU_HINT_GAME_ROW)) + i18n::tr(str::CHT_DICA_JOGO);
            }
        }
        const u64 draw_start = sceKernelGetProcessTime();
        if (overlay)
        {
            if (!overlay_since) overlay_since = draw_start;
            draw_dialog_frame(display, menu, *overlay, double(draw_start - started) / 1'000'000, draw_start - overlay_since);
        }
        else if (settings_open && settings.page_open())
        {
            overlay_since = 0;
            page_view.draw(menu, display.pixels(), display.typeface, settings, double(draw_start - started) / 1'000'000,
                float(draw_start - last_frame) / 1'000'000, clock, message);
        }
        else
        {
            overlay_since = 0;
            // A settings list: the PS4 tab names its tab keys (L2/R2), the top level has none
            const std::string_view settings_help = settings_open && settings.current_tab() ? i18n::tr(str::CFGU_FOOTER_TABS) : "";
            menu.draw(display.pixels(), display.typeface, category, *current.selected, *current.entries, double(draw_start - started) / 1'000'000,
                float(draw_start - last_frame) / 1'000'000, clock, shown, settings_open, settings_help);
        }
        last_frame = draw_start;
        last_draw_start = draw_start;
        // 0.38: under a dialog's button guide when one is shown
        // 0.39: right-aligned at x 1220 in a right-to-left interface (as the message line), fitted to its 1160 px
        if (!any_pad)
        {
            const bool rtl = i18n::rtl();
            const char* const no_pad = i18n::tr(str::LAU_NENHUM_CONTROLE);
            const std::string line = rtl ? display.typeface.fit(no_pad, 28, 1160, true) : std::string(no_pad);
            // a settings page has its help lines where the XMB draws this line: between its message lines instead
            const float y = overlay ? 690.f : settings_open && settings.page_open() ? 645.f : 600.f;
            display.text(rtl ? std::round(1220 - display.typeface.measure(line, 28)) : 60.f, y, line, 0xffffab7a);
        }
        if (!overlay && !rpcn_result.empty() && message == rpcn_result)
        {
            utils::ps4::xmb::canvas target(display.pixels(), display.width, display.height);
            target.rectangle(i18n::rtl() ? 1226 : 46, 620, 8, 8, rpcn_result_ok ? 0xff4cd964 : 0xffff5a52);
        }
        const u64 draw_us = sceKernelGetProcessTime() - draw_start;
        if (!display.present())
        {
            // A flip can fail or stall around a switch to the home screen (the background check above runs only
            // every 100 ms). Leaving here used to mean _Exit => SIGSYS => CE-34878-0. Keep retrying:
            // the application can always be closed from the PS button menu.
            ++flip_failures;
            if (!ps4_in_background() && (flip_failures == 1 || flip_failures % 30 == 0))
                ps4_startup_checkpoint(fmt::format("XMB flip failed; retrying (%u consecutive)", flip_failures).c_str());
            last_present = 0;
            sceKernelUsleep(100'000);
            continue;
        }
        const u64 presented = sceKernelGetProcessTime();
        if (flip_failures)
        {
            ps4_startup_checkpoint(fmt::format("XMB flip recovered after %u failures", flip_failures).c_str());
            flip_failures = 0;
        }
        // An interval that began in an idle tier was paced by it, also when a press ended it.
        perf.frame(draw_us, display.last_copy_us, display.last_wait_us, last_present ? presented - last_present : 0, std::max(tier, previous_tier));
        last_present = presented;
        previous_tier = tier;
        if (first_frame)
        {
            ps4_startup_checkpoint("XMB first frame presented");
            if (!any_pad) ps4_startup_checkpoint("launcher pad unavailable (no controller connected)");
            first_frame = false;
            // Normally no job outlives the menu (game boots are refused while one runs); check anyway. The first
            // poll reads rpcn.yml, so it waits until the menu is on screen.
            rpcn_busy = ps4_rpcn::poll().busy;
        }
        if (loader.inline_mode) loader.run_one();
        if (presented - perf.since >= 30'000'000) perf.report(presented, "30 s");
    }
}

namespace
{
// 0.37: the game a package belongs to (same TITLE_ID), for the out-of-order question and the install summary.
struct base_game
{
    std::string path, app_ver, title;
    std::string source; // "library" (the XMB's last scan) or "games.yml"
    bool iso = false;   // a disc image (else a folder)
};

// PARAM.SFO of a disc image or a game folder, as the library reads it.
psf::registry game_sfo(const std::string& path)
{
    if (utils::ps4::file_kind(path) == utils::ps4::game_kind::iso)
    {
        iso_archive archive(path);
        return archive.is_valid() ? archive.open_psf("PS3_GAME/PARAM.SFO") : psf::registry{};
    }
    return psf::load_object(metadata_base(path) + "PARAM.SFO");
}

// The XMB's last scan (library cache: an ISO row's app_ver is the disc's own APP_VER), else games.yml (every disc boot
// registers the image or folder, System.cpp). Installed updates (GD), PKG rows and games whose file is gone (a USB drive
// not connected) are skipped; a disc (ISO or CATEGORY DG) wins over a PSN game.
base_game find_base_game(const std::string& serial)
{
    base_game best;
    if (serial.empty()) return best;
    bool best_disc = false;
    if (library_cache cache; cache.load())
    {
        for (const auto& game : cache.games)
        {
            if (game.serial != serial || game.category == "GD" || game.category == ps4_package_category) continue;
            const auto kind = utils::ps4::file_kind(game.path);
            if (kind == utils::ps4::game_kind::package || !fs::exists(game.path)) continue;
            const bool disc = kind == utils::ps4::game_kind::iso || game.category == "DG";
            if (!best.path.empty() && (best_disc || !disc)) continue;
            best = {game.path, game.app_ver, game.title, "library", kind == utils::ps4::game_kind::iso};
            best_disc = disc;
        }
    }
    if (best.path.empty())
    {
        std::string path = Emu.GetGamesConfig().get_path(serial);
        if (path.ends_with("/./")) path.resize(path.size() - 2);
        if (path.empty() || !fs::exists(path)) return {};
        best = {path, {}, {}, "games.yml", utils::ps4::file_kind(path) == utils::ps4::game_kind::iso};
    }
    if (best.app_ver.empty() || best.title.empty())
    {
        const psf::registry sfo = game_sfo(best.path);
        if (best.app_ver.empty()) best.app_ver = std::string(psf::get_string(sfo, "APP_VER", ""));
        if (best.title.empty()) best.title = std::string(psf::get_string(sfo, "TITLE", ""));
    }
    one_line(best.app_ver);
    one_line(best.title);
    return best;
}

// Mirrors Emulator::Load ("Check game updates", System.cpp): a disc boot switches to dev_hdd0/game/<serial> when its
// USRDIR/EBOOT.BIN exists and its PARAM.SFO has the same TITLE_ID and CATEGORY "GD".
bool update_used_by_disc(const std::string& serial)
{
    const std::string update = rpcs3::utils::get_hdd0_dir() + "game/" + serial;
    if (serial.empty() || !fs::is_file(update + "/USRDIR/EBOOT.BIN")) return false;
    const auto sfo = psf::load_object(update + "/PARAM.SFO");
    return psf::get_string(sfo, "TITLE_ID", "") == serial && psf::get_string(sfo, "CATEGORY", "") == "GD";
}

// Folder a package installs into below dev_hdd0/game (unpkg.cpp: the metadata install dir, else the 9 characters at
// offset 7 of the header's content id).
std::string package_folder(const package_reader& reader)
{
    if (!reader.get_metadata().install_dir.empty()) return reader.get_metadata().install_dir;
    const char* id = reader.get_header().title_id;
    return std::string(id + 7, ::strnlen(id + 7, 9));
}

enum class screen_answer : u8 { cross, circle, timeout, shutdown };

// 0.38: the installer's screens are PS3 system dialogs over the XMB waves. The waves and the firmware images (glyphs,
// bar, separators) come from the XMB's image cache, else from the firmware, with the drawn stand-ins for what is missing.
struct installer_backdrop
{
    utils::ps4::xmb::scene scene;
    u64 since = 0; // start of the waves
    void load()
    {
        load_context context;
        bool from_cache = false;
        scene.resources = load_xmb_resources(context, from_cache);
        since = sceKernelGetProcessTime();
    }
    double seconds(u64 now) const { return double(now - since) / 1'000'000; }
};

// One question or result on the installer screen until X or Circle: about 30 frames a second, nothing drawn while the
// system UI is in front, presses of the first 400 ms ignored (the X that chose the package). timeout_us 0: no timeout.
screen_answer wait_on_dialog(launcher_display& display, installer_backdrop& backdrop, launcher_input& input, const xmb_dialog& shown, u64 timeout_us)
{
    const u64 start = sceKernelGetProcessTime();
    bool background = false;
    for (;;)
    {
        if (ps4_shutdown_requested()) return screen_answer::shutdown;
        ps4_poll_system_service();
        const u64 now = sceKernelGetProcessTime();
        if (timeout_us && now - start >= timeout_us) return screen_answer::timeout;
        if (ps4_in_background())
        {
            background = true;
            sceKernelUsleep(50'000);
            continue;
        }
        if (std::exchange(background, false)) input.flush();
        draw_dialog_frame(display, backdrop.scene, shown, backdrop.seconds(now), now - start);
        display.present();
        const uint32_t pressed = input.poll(now, false);
        if (now - start >= 400'000)
        {
            if (pressed & ORBIS_PAD_BUTTON_CROSS) return screen_answer::cross;
            if (pressed & ORBIS_PAD_BUTTON_CIRCLE) return screen_answer::circle;
        }
        sceKernelUsleep(dialog_frame_us);
    }
}

// 0.37: the question before an update is installed over a version that is not there (no dev_hdd0 data, the disc older
// than TARGET_APP_VER or no base game). Nothing is installed by default: only X (cross) installs.
screen_answer confirm_out_of_order(launcher_display& display, installer_backdrop& backdrop, launcher_input& input, const std::string& title,
    const std::string& serial, const package_install_result::versions& version, const base_game& base, const std::string& folder)
{
    std::string game_line;
    if (base.path.empty()) game_line = i18n::trf(str::LAU_UPDATE_JOGO_FORA_BIBLIOTECA, {serial});
    else if (base.app_ver.empty()) game_line = i18n::tr(base.iso ? str::LAU_UPDATE_SEU_JOGO_ISO_SEM_VERSAO : str::LAU_UPDATE_SEU_JOGO_PASTA_SEM_VERSAO);
    else game_line = i18n::trf(base.iso ? str::LAU_UPDATE_SEU_JOGO_ISO_VERSAO : str::LAU_UPDATE_SEU_JOGO_PASTA_VERSAO, {base.app_ver});
    xmb_dialog d;
    d.title = shorten(i18n::trf(str::LAU_UPDATE_TITULO, {title.empty() ? serial : title, version.app_ver, serial}), 90);
    d.lines = {i18n::trf(str::LAU_UPDATE_FEITO_PARA, {version.expected}), game_line, i18n::tr(str::LAU_UPDATE_ALGUNS_TRAZEM_TUDO),
        i18n::tr(str::LAU_UPDATE_INSTALAR_MESMO_ASSIM)};
    d.notes = {i18n::trf(str::LAU_UPDATE_NO_PS3_ANTES, {version.expected}),
        shorten(i18n::trf(str::LAU_UPDATE_PARA_DESFAZER, {rpcs3::utils::get_hdd0_dir() + "game/" + folder}), line_bytes(100))};
    d.buttons = {utils::ps4::xmb::cross_button(i18n::tr(str::COM_SIM)), utils::ps4::xmb::circle_button(i18n::tr(str::COM_NAO))};
    return wait_on_dialog(display, backdrop, input, d, 0);
}

// 0.37: what one installed package was and where it went, for the summary screen, the XMB line and startup.log.
struct install_summary
{
    std::vector<std::string> lines; // summary screen, below the title line
    std::string message;            // XMB line
    std::string log;                // startup.log
    std::string select;             // XMB row to open on
};

install_summary summarize_install(const package_reader& reader, const std::string& package, bool out_of_order, const std::string& expected)
{
    install_summary s;
    const auto& sfo = reader.get_psf();
    const std::string serial(psf::get_string(sfo, "TITLE_ID", ""));
    std::string title(psf::get_string(sfo, "TITLE", ""));
    one_line(title);
    const std::string category(psf::get_string(sfo, "CATEGORY", ""));
    const std::string app_ver(psf::get_string(sfo, "APP_VER", ""));
    const u32 content = reader.get_metadata().content_type;
    const std::string install = reader.get_install_path(); // ".../dev_hdd0/<kind>/<dir>/"
    std::string folder = install;
    while (folder.size() > 1 && folder.back() == '/') folder.pop_back();
    const std::string hdd0 = rpcs3::utils::get_hdd0_dir();
    const std::string shown = folder.starts_with(hdd0) ? "dev_hdd0/" + folder.substr(hdd0.size()) : folder;
    // Licence: NPDRM content (DRM type 1 network, 2 local, or the REQUIRE_LICENSE flag) runs only with <content id>.rap
    const PKGHeader& header = reader.get_header();
    const std::string content_id(header.title_id, ::strnlen(header.title_id, sizeof(header.title_id)));
    const u32 drm = reader.get_metadata().drm_type;
    const bool needs_license = drm == 1 || drm == 2 || (reader.get_metadata().package_type & PKG_FLAG_REQUIRE_LICENSE) != 0u;
    std::string license = "not needed";
    bool license_missing = false;
    if (needs_license)
    {
        bool found = false;
        // A valid content id (rpcs3::utils::get_rap_file_path logs an error for any other)
        if (const std::string rap_name = utils::ps4::rap_license_name(content_id + ".rap"); !rap_name.empty())
        {
            const std::string rap = rpcs3::utils::get_rap_file_path(content_id);
            found = !rap.empty() && fs::is_file(rap);
            // Upload folders the XMB imports from when it opens (ps4_import_licenses)
            for (const char* root : {"/data/rpcs3/raps/", "/data/rpcs3/pkgs/", "/data/rpcs3/exdata/"})
                found = found || fs::is_file(root + rap_name);
        }
        license_missing = !found;
        license = fmt::format("%s.rap %s", content_id, found ? "present" : "missing");
    }
    const psf::registry installed_sfo = psf::load_object(install + "PARAM.SFO");
    const std::string landed = installed_sfo.empty() ? std::string("PARAM.SFO missing")
        : fmt::format("PARAM.SFO %s %s", std::string(psf::get_string(installed_sfo, "CATEGORY", "?")), std::string(psf::get_string(installed_sfo, "TITLE_ID", "?")));
    const bool eboot = fs::is_file(install + "USRDIR/EBOOT.BIN");
    std::string kind, label, outcome; // startup.log / screen
    s.select = package;
    // 0.39: the screen lines are dialog rows (wrapped by width): their byte bounds double in Arabic
    const std::size_t screen_bytes = line_bytes(75);
    if (category == "GD" && !app_ver.empty())
    {
        kind = "update v" + app_ver;
        label = i18n::trf(str::LAU_UPDATE_V, {app_ver});
        const base_game base = find_base_game(serial);
        if (base.path.empty())
        {
            s.lines.push_back(shorten(i18n::trf(str::LAU_SUMMARY_COPIE_JOGO_BIBLIOTECA, {serial}), screen_bytes));
            s.message = i18n::trf(str::LAU_SUMMARY_UPDATE_COPIE_JOGO, {app_ver, shown, serial});
            outcome = "base game not in the library";
        }
        else
        {
            s.select = base.path;
            if (update_used_by_disc(serial))
            {
                s.lines.push_back(base.app_ver.empty() ? i18n::trf(str::LAU_SUMMARY_SEU_JOGO_ABRE_NA, {app_ver})
                    : i18n::trf(str::LAU_SUMMARY_SEU_JOGO_VERSAO_ABRE_NA, {base.app_ver, app_ver}));
                s.message = i18n::trf(str::LAU_SUMMARY_UPDATE_ABRE_NA, {app_ver, shown, app_ver});
                outcome = fmt::format("disc '%s' v%s now boots v%s", base.path, base.app_ver.empty() ? "?" : base.app_ver, app_ver);
            }
            else
            {
                s.lines.emplace_back(i18n::tr(str::LAU_SUMMARY_UPDATE_SO_DADOS_EBOOT));
                s.message = i18n::trf(str::LAU_SUMMARY_UPDATE_SO_DADOS, {app_ver, shown});
                outcome = fmt::format("data only: '%s' boots its own EBOOT", base.path);
            }
        }
    }
    else if (content == PKG_CONTENT_TYPE_GAME_EXEC || category == "HG")
    {
        kind = "PSN game";
        label = i18n::tr(str::COM_JOGO_PSN);
        s.lines.emplace_back(i18n::tr(str::LAU_SUMMARY_JOGO_PSN_LISTA));
        s.message = i18n::trf(str::LAU_SUMMARY_JOGO_INSTALADO, {title.empty() ? serial : title, shown});
        s.select = folder;
        outcome = "listed as a game";
    }
    else if (content == PKG_CONTENT_TYPE_LICENSE)
    {
        kind = "license";
        label = i18n::tr(str::COM_LICENCA);
        s.message = i18n::trf(str::LAU_SUMMARY_LICENCA_INSTALADA, {shown});
        outcome = "license data";
    }
    else
    {
        kind = "add-on data";
        label = i18n::tr(str::COM_DLC_CONTEUDO_ADICIONAL);
        const base_game base = find_base_game(serial);
        if (base.path.empty())
        {
            s.lines.push_back(shorten(i18n::trf(str::LAU_SUMMARY_DLC_JOGO_FORA, {serial}), screen_bytes));
            s.message = i18n::trf(str::LAU_SUMMARY_DLC_INSTALADO_FORA, {serial, shown});
            outcome = "base game not in the library";
        }
        else
        {
            s.select = base.path;
            s.lines.push_back(shorten(i18n::trf(str::LAU_SUMMARY_DLC_VALE, {base.title.empty() ? serial : base.title}), screen_bytes));
            s.message = i18n::trf(str::LAU_SUMMARY_DLC_INSTALADO, {serial, shown});
            outcome = fmt::format("base game '%s'", base.path);
        }
    }
    if (out_of_order) s.lines.push_back(shorten(i18n::trf(str::LAU_SUMMARY_FORA_DE_ORDEM, {expected}), screen_bytes));
    if (license_missing)
    {
        s.lines.emplace_back(i18n::tr(str::LAU_SUMMARY_FALTA_LICENCA));
        s.lines.push_back(shorten(i18n::trf(str::LAU_SUMMARY_LICENCA_RAPS, {content_id}), screen_bytes));
        s.message += i18n::tr(str::LAU_SUMMARY_FALTA_LICENCA_SUFIXO);
    }
    s.lines.insert(s.lines.begin(), shorten(i18n::trf(str::LAU_SUMMARY_PASTA, {folder}), screen_bytes));
    s.lines.insert(s.lines.begin(), shorten(fmt::format("%s · %s · %s", title.empty() ? serial : title, label, serial), line_bytes(72)));
    s.message = shorten(s.message, line_bytes(116));
    s.log = fmt::format("PKG installed: %s of %s '%s' at %s (EBOOT %s, %s): %s; license: %s", kind, serial, title, install, eboot ? "yes" : "no",
        landed, outcome, license);
    return s;
}

// 0.34: a package of a disc's install folders, as the boot lists them (System.cpp: PS3_GAME/INSDIR/*.PKG,
// PS3_GAME/PKGDIR/PKG*/INSTALL.PKG, PS3_EXTRA/D*/DATA000.PKG).
bool disc_package_path(std::string_view path)
{
    return path.find("/PS3_GAME/INSDIR/") != std::string_view::npos || path.find("/PS3_GAME/PKGDIR/") != std::string_view::npos ||
        path.find("/PS3_EXTRA/") != std::string_view::npos;
}
}

ps4_install_result ps4_install_packages(const std::vector<std::string>& requested, bool from_disc_flag, std::deque<std::string>* bootables)
{
    ps4_install_result out;
    if (requested.empty())
    {
        out.ok = true;
        return out;
    }
    // The boot's own flag (launching_from_optical_drive) is false for every disc image on the PS4, so the disc's
    // install folders are recognized by their paths too.
    const bool from_disc = from_disc_flag || std::all_of(requested.begin(), requested.end(), [](const std::string& path) { return disc_package_path(path); });
    // With several packages the line names the one it is about. 0.39: a disc's package text names it inside the disc.
    const auto about = [&](const std::string& path, const std::string& text)
    { return requested.size() > 1 && !from_disc ? shorten(file_name_of(path) + ": " + text, line_bytes(116)) : shorten(text, line_bytes(116)); };
    // 0.37: the XMB opens on the PKG row after a refusal or a failure
    const auto refuse = [&](const std::string& path, const std::string& text, const std::string& why)
    {
        ps4_startup_checkpoint(fmt::format("PKG installer: '%s' refused: %s", path, why).c_str());
        out.message = about(path, text);
        out.select = path;
        return out;
    };
    namespace pkg = utils::ps4::pkg;
    // 0.39: a package inside the booting disc: its path from PS3_GAME/ or PS3_EXTRA/ on, and whether the disc is an ISO
    const auto disc_rel = [](const std::string& p) -> std::string
    {
        for (const std::string_view root : {std::string_view("/PS3_GAME/"), std::string_view("/PS3_EXTRA/")})
            if (const auto at = p.find(root); at != std::string::npos) return p.substr(at + 1);
        return file_name_of(p);
    };
    const bool image = requested.front().starts_with(iso_device::virtual_device_name);
    // Checked before the screen opens. Only the first package's target version is checked here: a later update may
    // need what an earlier package of the same list installs (extract_data checks each one in turn).
    // 0.37: the header and size first (a PKG still being copied, a missing part, PS4/PSP/Vita or not a package at all),
    // so package_reader only reads complete PS3 packages and an invalid one can only be a damaged one.
    // 0.39: a disc's PS3_EXTRA package is optional (a PS3 only offers it in its XMB): one that cannot install is left out
    // and the game boots without it; INSDIR/PKGDIR packages (game data) still refuse the boot.
    std::vector<std::string> paths;
    std::deque<package_reader> readers;
    std::string skipped_extra;
    u64 total_bytes = 0;
    for (const auto& path : requested)
    {
        const bool extra = from_disc && path.find("/PS3_EXTRA/") != std::string::npos;
        const auto skip = [&](const std::string& why)
        {
            ps4_startup_checkpoint(fmt::format("PKG installer: disc extra '%s' skipped: %s", disc_rel(path), why).c_str());
            if (skipped_extra.empty()) skipped_extra = disc_rel(path);
        };
        if (const pkg::verdict check = check_package_file(path); !check.ready())
        {
            if (extra)
            {
                skip(check.log);
                continue;
            }
            if (from_disc) return refuse(path, pkg::disc_package_text(check.kind, disc_rel(path), image), check.log + " (package of the booting disc)");
            return refuse(path, check.text, check.log);
        }
        const package_reader& reader = readers.emplace_back(path);
        if (!reader.is_valid())
        {
            const char* why = reader.ps4_read_failed() ? pkg::read_error_log : pkg::unreadable_log;
            if (extra)
            {
                readers.pop_back();
                skip(why);
                continue;
            }
            if (from_disc) return refuse(path, pkg::disc_package_text(pkg::state::not_package, disc_rel(path), image), std::string(why) + " (package of the booting disc)");
            return reader.ps4_read_failed() ? refuse(path, pkg::read_error_text, pkg::read_error_log) : refuse(path, pkg::unreadable_text, pkg::unreadable_log);
        }
        if (reader.get_header().pkg_platform != PKG_PLATFORM_TYPE_PS3)
        {
            if (extra)
            {
                readers.pop_back();
                skip("PSP/PS Vita package");
                continue;
            }
            if (from_disc) return refuse(path, pkg::disc_package_text(pkg::state::psp_vita, disc_rel(path), image), "PSP/PS Vita package (package of the booting disc)");
            return refuse(path, pkg::psp_vita_text, "PSP/PS Vita package");
        }
        paths.push_back(path);
        total_bytes += static_cast<u64>(reader.get_header().data_size);
    }
    if (paths.empty())
    {
        // Only extras, every one left out: the boot goes on (the boot's lock file then keeps them from being tried again)
        out.ok = true;
        out.message = shorten(i18n::trf(str::LAU_PKG_EXTRA_IGNORADO, {pkg::clip_name(skipped_extra, 40)}), line_bytes(116));
        return out;
    }
    const std::size_t count = paths.size();
    // 0.39: a target folder that refuses writes (made over FTP) is opened now (fs_repair) rather than met half way
    // through the install; one that stays closed refuses the install before anything is written
    {
        namespace fr = utils::ps4::fs_repair;
        const fr::fs_ops calls = fr::sys::fs();
        for (std::size_t i = 0; i < readers.size(); ++i)
            for (const std::string& folder : {std::string("/data/rpcs3/dev_hdd0/game"), "/data/rpcs3/dev_hdd0/game/" + package_folder(readers[i])})
            {
                // The install folder name comes from the package: one leaving /data/rpcs3 (or empty) is never probed
                if (!fr::allowed_path(folder)) continue;
                std::uint32_t owner = 0;
                if (!fr::permission_error(calls.probe(folder, owner)) || ps4_reopen_refused_folder(folder)) continue;
                return refuse(paths[i], fr::no_write_text(folder), "target folder " + folder + " refuses writes");
            }
    }

    // 0.37: the screen opens only when needed (the out-of-order question, or the install itself)
    // 0.38: with it the waves and the firmware images of its system dialogs
    launcher_display display;
    std::optional<installer_backdrop> backdrop;
    bool display_open = false;
    const auto open_display = [&]
    {
        if (display_open) return true;
        if (!(display_open = display.open())) return false;
        backdrop.emplace();
        backdrop->load();
        return true;
    };
    // 0.31: Circle on any seated controller cancels.
    launcher_input input;
    input.poll(sceKernelGetProcessTime(), false); // the X that chose the package is not an installer press
    // 0.37: an update whose TARGET_APP_VER cannot be compared because nothing of its title is installed (a disc or ISO
    // game before its first update) is no longer refused outright. The disc's own APP_VER decides: the update's target
    // version installs without a question (what the update was made for), a disc as new as the update is refused, and
    // anything else asks (X installs anyway, Circle cancels). Never for a disc's own package list. Every other version
    // refusal keeps its precise reason.
    bool out_of_order = false;
    std::string out_of_order_expected;
    if (const auto version = readers.front().check_target_app_version(); version.error != package_install_result::error_type::no_error)
    {
        const auto& v = version.version;
        const std::string serial(psf::get_string(readers.front().get_psf(), "TITLE_ID", ""));
        const bool no_data = version.error == package_install_result::error_type::app_version && v.installed.empty() && !v.expected.empty();
        const base_game base = no_data ? find_base_game(serial) : base_game{};
        if (version.error != package_install_result::error_type::app_version)
            ps4_startup_checkpoint(fmt::format("PKG installer: '%s' (%s): a version of the PKG or of dev_hdd0/game/%s is unreadable (see 'PKG:' in RPCS3.log)",
                paths.front(), serial, serial).c_str());
        else
            ps4_startup_checkpoint(fmt::format("PKG installer: '%s' is update v%s of %s and needs %s; dev_hdd0: %s; base game: %s", paths.front(), v.app_ver, serial,
                v.expected.empty() ? std::string("no target version") : "v" + v.expected, v.installed.empty() ? std::string("nothing installed") : "v" + v.installed,
                !no_data ? std::string("not needed (the installed data decides)") : base.path.empty() ? std::string("not in the library")
                    : fmt::format("'%s' v%s (%s)", base.path, base.app_ver.empty() ? "?" : base.app_ver, base.source)).c_str());
        if (!no_data || from_disc)
            return refuse(paths.front(), pkg::version_refusal_text(v.app_ver, v.expected, v.installed, base.app_ver, serial), "target version");
        const auto disc_vs = [&](const std::string& other) { return base.app_ver.empty() ? std::nullopt : pkg::compare_versions(base.app_ver, other); };
        if (const auto order = disc_vs(v.app_ver); order && *order >= 0)
            return refuse(paths.front(), i18n::trf(str::LAU_PKG_SEU_JOGO_JA_E, {base.app_ver, v.app_ver}),
                "target version: the disc is not older than the update");
        if (const auto order = disc_vs(v.expected); order && *order == 0)
            ps4_startup_checkpoint(fmt::format("PKG installer: v%s is the disc's own version: installing over the disc", v.expected).c_str());
        else
        {
            if (!open_display())
                return refuse(paths.front(), pkg::version_refusal_text(v.app_ver, v.expected, {}, base.app_ver, serial), "target version (no screen to ask)");
            std::string title(psf::get_string(readers.front().get_psf(), "TITLE", ""));
            one_line(title);
            if (const screen_answer answer = confirm_out_of_order(display, *backdrop, input, title, serial, v, base, package_folder(readers.front()));
                answer != screen_answer::cross)
                return refuse(paths.front(), i18n::tr(str::LAU_PKG_CANCELADA_NADA_INSTALADO),
                    answer == screen_answer::circle ? "target version: cancelled with Circle" : "target version: not confirmed (application closing)");
            ps4_startup_checkpoint(fmt::format("PKG installer: install over a missing v%s confirmed with X (disc %s)", v.expected,
                base.app_ver.empty() ? std::string("unknown") : "v" + base.app_ver).c_str());
            out_of_order = true;
            out_of_order_expected = v.expected;
        }
        readers.front().allow_missing_target_app_version();
    }

    if (!open_display())
    {
        ps4_startup_checkpoint("PKG installer: the installer screen did not open; nothing installed");
        out.display_unavailable = true;
        out.message = i18n::tr(str::LAU_PKG_TELA_NAO_ABRIU);
        return out;
    }
    std::deque<std::string> installed;
    package_install_result result;
    fs::error worker_error = fs::error::ok;
    bool cancelled = false;
    ps4_startup_checkpoint(fmt::format("starting original RPCS3 PS3 package installer: %u package(s)%s, %llu MiB to write", count,
        from_disc ? " from the game disc" : "", total_bytes >> 20).c_str());
    named_thread worker("PS4 PKG Installer", [&]
    {
        // A disc's packages are extracted by one thread, as upstream does for optical drives: the image usually sits on
        // a USB drive (one sequential reader), and the boot that asked for them keeps its thread budget.
        result = package_reader::extract_data(readers, installed, from_disc);
        // The path and folder creation runs on this thread: its error explains a refused folder.
        worker_error = fs::g_tls_error;
        return result.error == package_install_result::error_type::no_error;
    });
    std::size_t current = 0;
    bool flip_failure_logged = false;
    const u64 install_start = sceKernelGetProcessTime();
    // 0.38: the install as a PS3 progress dialog: the package's title, one bar for all packages, the time left from the
    // average rate (after 3 s and 1 %), Circle cancels. About 30 frames a second, nothing drawn behind the system UI.
    while (worker <= thread_state::aborting)
    {
        // Packages are extracted in order; one at 100 % hands over to the next (as the PC progress dialog).
        while (current + 1 < count && readers[current].get_progress(1000) >= 1000) ++current;
        u64 done = 0;
        for (const auto& reader : readers) done += static_cast<u64>(reader.get_header().data_size) * static_cast<u64>(reader.get_progress(1000)) / 1000;
        const int per_mille = total_bytes ? static_cast<int>(std::min<u64>(1000, done * 1000 / total_bytes)) : 1000;
        const u64 now = sceKernelGetProcessTime();
        if (!cancelled && (input.poll(now, false) & ORBIS_PAD_BUTTON_CIRCLE))
        {
            // Every reader: the one in progress stops, the later ones never start.
            for (auto& reader : readers) reader.abort_extract();
            cancelled = true;
            ps4_startup_checkpoint("PKG installer: cancelled with Circle");
        }
        ps4_poll_system_service(); // main thread; at most every 100 ms
        if (ps4_in_background())
        {
            sceKernelUsleep(50'000);
            continue;
        }
        // A disc package's path ends in INSTALL.PKG or DATA000.PKG: its PARAM.SFO title says more.
        std::string package_title(psf::get_string(readers[current].get_psf(), "TITLE", ""));
        one_line(package_title);
        xmb_dialog shown;
        shown.title = shorten(package_title.empty() ? file_name_of(paths[current]) : package_title, 90);
        if (cancelled)
        {
            shown.lines.emplace_back(i18n::tr(str::LAU_CANCELANDO));
            shown.notes.emplace_back(i18n::tr(str::LAU_PKG_AGUARDE_LIMPEZA));
            shown.busy = true;
        }
        else
        {
            const std::string step = count > 1 ? fmt::format(" (%u/%u)", current + 1, count) : std::string{};
            shown.lines.push_back(i18n::tr(from_disc ? str::LAU_PKG_INSTALANDO_DADOS_DISCO : str::LAU_DISC_ROW_INSTALANDO) + step);
            namespace di = utils::ps4::disc_install;
            shown.notes.push_back(i18n::trf(str::LAU_DISC_COPY_TAMANHO, {di::size_text(done), di::size_text(total_bytes)}));
            const u64 elapsed = now - install_start;
            const u64 rate = elapsed >= 3'000'000 && per_mille >= 10 ? done * 1'000'000 / elapsed : 0;
            shown.progress = per_mille;
            shown.progress_left = utils::ps4::xmb::percent_text(per_mille);
            shown.progress_right = utils::ps4::xmb::remaining_text(total_bytes > done ? total_bytes - done : 0, rate);
            shown.buttons = {utils::ps4::xmb::circle_button(i18n::tr(str::COM_CANCELAR))};
        }
        draw_dialog_frame(display, backdrop->scene, shown, backdrop->seconds(now), now - install_start);
        // A failed flip (e.g. home screen in front) is not a reason to cancel the install.
        if (!display.present() && !flip_failure_logged)
        {
            ps4_startup_checkpoint("PKG installer: progress flip failed; installation continues");
            flip_failure_logged = true;
        }
        sceKernelUsleep(dialog_frame_us);
    }
    const bool succeeded = worker(); // Join before readers/display leave scope.
    if (bootables) bootables->insert(bootables->end(), installed.begin(), installed.end());
    // 0.38: a failed install says why on its dialog (X, Circle or 20 s); the XMB line repeats it afterwards
    const auto failure = [&](std::size_t index) -> ps4_install_result
    {
        std::string package_title(psf::get_string(readers[index].get_psf(), "TITLE", ""));
        one_line(package_title);
        xmb_dialog shown, parts;
        split_into_dialog(out.message, parts);
        shown.title = shorten(package_title.empty() ? file_name_of(paths[index]) : package_title, 90);
        shown.lines.emplace_back(i18n::tr(str::COM_NAO_FOI_POSSIVEL_CONCLUIR));
        shown.notes = std::move(parts.lines);
        shown.notes.insert(shown.notes.end(), parts.notes.begin(), parts.notes.end());
        shown.buttons = {utils::ps4::xmb::cross_button(i18n::tr(str::COM_OK))};
        wait_on_dialog(display, *backdrop, input, shown, 20'000'000);
        return out;
    };
    if (char reason[512]; ps4_take_thread_fatal(reason, sizeof(reason)))
    {
        ps4_startup_checkpoint(fmt::format("PS3 package installer thread failed: %s", reason).c_str());
        out.message = i18n::tr(str::LAU_PKG_ERRO_INTERNO);
        return failure(current);
    }
    // The first package that did not finish is the one that failed or was cancelled; the ones before it are installed.
    std::size_t failed = count;
    for (std::size_t i = 0; i < count; ++i)
    {
        if (readers[i].get_result() == package_reader::success) continue;
        failed = i;
        break;
    }
    if (succeeded && failed == count)
    {
        ps4_startup_checkpoint(count > 1 ? fmt::format("PS3 package installed successfully (%u packages)", count).c_str() : "PS3 package installed successfully");
        out.ok = true;
        out.message = from_disc ? i18n::trf(str::LAU_PKG_CONTEUDO_DISCO_INSTALADO, {count})
            : count > 1 ? i18n::trf(str::LAU_PKG_INSTALADOS, {count}) : std::string(i18n::tr(str::LAU_PKG_INSTALADO));
        // 0.37: what each package was, where it went and what the game boots now; one PKG chosen in the XMB also gets the
        // summary screen (X, Circle or 20 s) and the XMB opens on its game. A disc's own packages install during its boot.
        if (!from_disc)
        {
            install_summary last;
            for (std::size_t i = 0; i < count; ++i)
            {
                last = summarize_install(readers[i], paths[i], i == 0 && out_of_order, out_of_order_expected);
                ps4_startup_checkpoint(last.log.c_str());
            }
            out.select = last.select;
            if (count == 1)
            {
                out.message = last.message;
                // 0.38: "<title> · <kind> · <id>" over "concluída", the folder and what the game boots now under it
                xmb_dialog done_dialog;
                if (!last.lines.empty()) done_dialog.title = last.lines.front();
                done_dialog.lines.emplace_back(i18n::tr(str::COM_INSTALACAO_FOI_CONCLUIDA));
                for (std::size_t i = 1; i < last.lines.size(); ++i) done_dialog.notes.push_back(std::move(last.lines[i]));
                done_dialog.buttons = {utils::ps4::xmb::cross_button(i18n::tr(str::COM_OK))};
                wait_on_dialog(display, *backdrop, input, done_dialog, 20'000'000);
            }
        }
        return out;
    }
    if (failed == count) failed = count - 1; // an error after the last package (no reader left unfinished)
    const std::string& path = paths[failed];
    out.select = path;
    const std::string before = failed ? i18n::trf(str::LAU_PKG_ANTERIORES_INSTALADOS, {failed}) : std::string{};
    if (cancelled)
    {
        const bool dirty = readers[failed].get_result() == package_reader::aborted_dirty || readers[failed].get_result() == package_reader::error_dirty;
        ps4_startup_checkpoint(fmt::format("PS3 package installation cancelled at '%s' (%s)", path, dirty ? "partial files kept" : "partial files removed").c_str());
        out.cancelled = true;
        out.message = shorten(i18n::tr(dirty ? str::LAU_PKG_CANCELADA_PARCIAIS : str::LAU_PKG_CANCELADA_NADA_DESTE) + before, line_bytes(116));
        return out;
    }
    if (result.error == package_install_result::error_type::app_version)
    {
        ps4_startup_checkpoint(fmt::format("PS3 package installation failed: '%s' is for another version (app %s, expected %s, installed %s)", path,
            result.version.app_ver, result.version.expected, result.version.installed).c_str());
        out.message = about(path, utils::ps4::pkg::version_refusal_text(result.version.app_ver, result.version.expected, result.version.installed, {},
            std::string(psf::get_string(readers[failed].get_psf(), "TITLE_ID", ""))) + before);
        return failure(failed);
    }
    if (std::any_of(readers.begin(), readers.end(), [](const package_reader& reader) { return reader.ran_out_of_space(); }))
    {
        // 0.37: how far it got and whether partial files stayed (a folder that existed before is never removed)
        const package_reader& reader = readers[failed];
        const u64 data_size = static_cast<u64>(reader.get_header().data_size);
        const bool dirty = reader.get_result() == package_reader::error_dirty || reader.get_result() == package_reader::aborted_dirty;
        ps4_startup_checkpoint(fmt::format("PS3 package installation failed: no space on /data after %llu MiB of %llu (partial files %s)",
            (data_size * static_cast<u64>(reader.get_progress(1000)) / 1000) >> 20, data_size >> 20, dirty ? "kept" : "removed").c_str());
        out.no_space = true;
        const u64 gib = (data_size >> 30) + 1;
        out.message = about(path, i18n::trf(str::LAU_PKG_SEM_ESPACO, {gib, dirty ? str::LAU_PKG_FICARAM_PARCIAIS : str::LAU_PKG_NADA_FICOU}));
        return failure(failed);
    }
    // 0.39: the first failure package_reader recorded (what, which file, partial files kept or removed)
    for (std::size_t i = 0; i < count; ++i)
    {
        const auto& f = readers[i].ps4_failure();
        if (f.kind == pkg::install_failure::none) continue;
        const bool dirty = readers[i].get_result() == package_reader::error_dirty || readers[i].get_result() == package_reader::aborted_dirty;
        ps4_startup_checkpoint(fmt::format("PS3 package installation failed: %s: '%s' (error %s, partial files %s)", pkg::install_failure_name(f.kind), f.file,
            f.error, dirty ? "kept" : "removed").c_str());
        out.select = paths[i];
        // 0.39: a write the console refused names the folder that refused it (the next start repairs it through FTP)
        const bool denied = f.error == fs::error::acces &&
            (f.kind == pkg::install_failure::write || f.kind == pkg::install_failure::create_file || f.kind == pkg::install_failure::create_dir);
        out.message = about(paths[i], (denied ? utils::ps4::fs_repair::no_write_text(f.file.starts_with("/data/rpcs3/") ? utils::ps4::fs_repair::parent_folder(f.file) : std::string("/data/rpcs3/dev_hdd0/game"))
            : pkg::install_failure_text(f.kind, f.file, f.error == fs::error::acces || f.error == fs::error::readonly, dirty)) + before);
        return failure(i);
    }
    if (worker_error == fs::error::acces || worker_error == fs::error::readonly)
    {
        ps4_startup_checkpoint(fmt::format("PS3 package installation failed: permission denied in /data/rpcs3/dev_hdd0 (%s)", worker_error).c_str());
        out.message = about(path, (worker_error == fs::error::acces ? utils::ps4::fs_repair::no_write_text("/data/rpcs3/dev_hdd0")
            : std::string(i18n::tr(str::LAU_PKG_SEM_PERMISSAO_DEV_HDD0))) + before);
        return failure(failed);
    }
    ps4_startup_checkpoint("PS3 package installation failed; see RPCS3 log");
    out.message = about(path, i18n::tr(str::COM_FALHA_INSTALAR_PKG_CONFIRA_RPCS3) + before);
    return failure(failed);
}

std::string ps4_install_package(const std::string& path)
{
    return ps4_install_packages({path}, false).message;
}

// 0.37: the last check before BootGame (disc_lock): a game installed from a PS3 disc boots only with a disc of it in the
// drive. "" lets the boot go on; otherwise the XMB line asking for the disc. The XMB's own check leaves a token for
// 30 s, so a boot chosen there does not read the drive again; any other way in (a failed display) reads it here, on the
// calling thread, bounded by the reader's time limits. Games without a lock record cost one listing of a small folder.
std::string ps4_disc_lock_boot_check(const std::string& path)
{
    namespace dl = utils::ps4::disc_lock;
    namespace ds = utils::ps4::disc_stream;
    if (utils::ps4::file_kind(path) != utils::ps4::game_kind::iso || dl::sys::take_verified(path)) return {};
    const dl::registry registry = dl::sys::load_registry();
    if (registry.by_path.empty()) return {};
    fs::stat_t stat{};
    if (!fs::get_stat(path, stat) || stat.is_directory) return {};
    // 0.39: only the exact image of a record (path and size); nothing to parse
    const dl::record* const bound = dl::bound(registry, path, stat.size);
    if (!bound) return {};
    const std::optional<dl::record> record = *bound;
    const std::int64_t started = ds::detail::now_ms();
    const ds::disc_status status = ds::service::instance().check_now();
    const std::int64_t now_ms = ds::detail::now_ms();
    const dl::match m = dl::classify(*record, status, now_ms);
    if (dl::unlocks(m))
    {
        ps4_startup_checkpoint(fmt::format("disc lock: '%s': disc %s present (%s, checked in %d ms): booting", path, record->title_id, dl::to_string(m),
            now_ms - started).c_str());
        return {};
    }
    ps4_startup_checkpoint(fmt::format("disc lock: '%s' not booted: %s (checked in %d ms)", path, dl::describe(m, *record, status), now_ms - started).c_str());
    return dl::message_text(*record);
}
