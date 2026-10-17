#pragma once

// 0.38: memory caps of the HLE modules and of the services next to them: the video decoder picture queue (cellVdec),
// the glyph pages of the native dialogs (overlay fonts), the savestate writer and the patch file parses. Each
// decision is a pure function of a memory sample (constexpr, host-testable without the console).
//
// On the PS4 the level (0 = fine, 1 = low, 2 = critical) and the heap room come from the memory governor
// (util/ps4_memory_governor.hpp). A newer memory authority can replace both without touching the callers: it stores
// its own functions in level_hook / room_hook (any thread, at any time; null = the governor again). The functions
// must be cheap, allocation-free and safe on any thread.
//
// Kill switches (files under /data/rpcs3, read once per process):
//   vdec-queue-60.flag          the decoder picture queue keeps upstream's 60 pictures whatever the memory state
//   font-pages-unbounded.flag   glyph pages are never dropped (upstream behaviour, still reset at the overlay teardown)
//   patch-extract-off.flag      the boot always parses the whole patch files (no per-title extract)
//   savedata-preload.flag       cellSaveData reads every file of the save directory into memory first (0.37)

#include "util/types.hpp"

#include <atomic>

namespace utils::ps4::hle_memory
{
// Heap room the sources report when no heap cap is known (no limit applies).
inline constexpr u64 room_unknown = ~0ull;

// ---------------------------------------------------------------------------------------------------------------
// cellVdec picture queue. Upstream queues up to 60 decoded pictures per decoder (plus the DPB and the pictures in
// flight): ~213-247 MB for one 1080p stream when the game stops taking pictures. A PS3 keeps its pictures inside the
// memSize the game gave cellVdecOpen (54.6 MiB for AVC level 4.x, 13.8 MiB for MPEG-2 MP@HL), so the queue is bounded
// by that size per picture, never below 4 pictures (the decoder still runs ahead of the game) and never above 60.
inline constexpr u32 vdec_queue_max = 60;
inline constexpr u32 vdec_queue_min = 4;
inline constexpr u64 vdec_queue_floor_bytes = 24ull << 20; // budget used when the game's memSize is smaller
inline constexpr char vdec_queue_flag[] = "/data/rpcs3/vdec-queue-60.flag";

// Pictures the queue may hold for a decoder opened with 'mem_size' whose pictures take 'picture_bytes' each (0 = no
// picture decoded yet), at memory level 'level'.
constexpr u32 vdec_queue_limit(u64 mem_size, u64 picture_bytes, int level) noexcept
{
    // Under memory pressure the queue shrinks to at most 8 pictures, never below the game's own budget allows (min 4):
    // a game that queues several pictures before reading any must not stall on a too-short queue.
    constexpr u32 pressure_max = 8;
    if (!picture_bytes) return level >= 1 ? pressure_max : vdec_queue_max;
    const u64 budget = mem_size > vdec_queue_floor_bytes ? mem_size : vdec_queue_floor_bytes;
    const u64 pictures = budget / picture_bytes;
    const u32 limit = pictures < vdec_queue_min ? vdec_queue_min : pictures > vdec_queue_max ? vdec_queue_max : static_cast<u32>(pictures);
    return level >= 1 && limit > pressure_max ? pressure_max : limit;
}

// Bytes of one 8-bit 4:2:0 picture as FFmpeg holds it (padded line sizes of the three planes).
constexpr u64 yuv420_picture_bytes(s64 luma_pitch, s64 cb_pitch, s64 cr_pitch, s64 height) noexcept
{
    if (luma_pitch <= 0 || height <= 0) return 0;
    const u64 chroma_rows = static_cast<u64>((height + 1) / 2);
    return static_cast<u64>(luma_pitch) * static_cast<u64>(height) + (cb_pitch > 0 ? static_cast<u64>(cb_pitch) * chroma_rows : 0) +
        (cr_pitch > 0 ? static_cast<u64>(cr_pitch) * chroma_rows : 0);
}

// ---------------------------------------------------------------------------------------------------------------
// Overlay font glyph pages: 1 MiB bitmap per (font, size, 256 code point block). The bitmap is only read when the
// renderer uploads the font texture; a dropped bitmap is packed again from the TTF at the next upload.
inline constexpr u32 font_pages_max = 24;
inline constexpr u32 font_pages_critical = 8;
inline constexpr char font_pages_flag[] = "/data/rpcs3/font-pages-unbounded.flag";

constexpr u32 font_page_cap(int level, bool unbounded) noexcept
{
    return unbounded ? ~0u : level >= 2 ? font_pages_critical : font_pages_max;
}

// ---------------------------------------------------------------------------------------------------------------
// Savestate writer: upstream starts up to 32 compression threads (5 on the PS4) at zstd level 8, 300-400 MiB at the
// moment the heap is fullest. The PS4 writes with one thread at level 3 and refuses before starting when the heap
// room is short or the memory level is critical.
inline constexpr u32 savestate_threads = 1;
inline constexpr int savestate_zstd_level = 3;
inline constexpr u64 savestate_room_min = 512ull << 20;

enum class savestate_refusal : u32
{
    none = 0,
    room,  // heap room below savestate_room_min
    level, // memory level 2 or above
};

constexpr savestate_refusal savestate_check(u64 room, int level) noexcept
{
    if (level >= 2) return savestate_refusal::level;
    if (room < savestate_room_min) return savestate_refusal::room;
    return savestate_refusal::none;
}

// ---------------------------------------------------------------------------------------------------------------
// Patch files (yaml-cpp): ~30 MiB of node allocations per MB of YAML. A file above patch_file_limit is not read, and
// a parse starts only with patch_parse_factor x its size of heap room.
inline constexpr u64 patch_file_limit = 8ull << 20;
inline constexpr u64 patch_parse_factor = 40;
inline constexpr char patch_extract_flag[] = "/data/rpcs3/patch-extract-off.flag";

constexpr bool patch_parse_allowed(u64 room, u64 file_bytes) noexcept
{
    return room == room_unknown || room / patch_parse_factor >= file_bytes;
}

// 64-bit FNV-1a, names the per-title patch extract after the patch files it was cut from.
constexpr u64 fnv1a64(const char* data, usz size, u64 hash = 0xcbf29ce484222325ull) noexcept
{
    for (usz i = 0; i < size; i++)
    {
        hash ^= static_cast<u8>(data[i]);
        hash *= 0x100000001b3ull;
    }
    return hash;
}

// ---------------------------------------------------------------------------------------------------------------
// cellSaveData: kill switch of the lazy save file entries.
inline constexpr char savedata_preload_flag[] = "/data/rpcs3/savedata-preload.flag";

// ---------------------------------------------------------------------------------------------------------------
// Optional sources (see the top of this file).
inline std::atomic<int (*)() noexcept> level_hook{nullptr};
inline std::atomic<u64 (*)() noexcept> room_hook{nullptr};
} // namespace utils::ps4::hle_memory

#ifdef RPCS3_PS4
#include "util/ps4_i18n.hpp"
#include "util/ps4_memory_governor.hpp"

#include <cstdio>
#include <unistd.h>

namespace utils::ps4::hle_memory
{
// Memory level: the registered source, else the governor's heap level (sampled at most every 50 ms).
inline int level() noexcept
{
    if (const auto hook = level_hook.load(std::memory_order_acquire)) return hook();
    return ::utils::ps4::governor::heap_pressure();
}

// Heap room in bytes, room_unknown without a known cap. Never a syscall on the governor path.
inline u64 room() noexcept
{
    if (const auto hook = room_hook.load(std::memory_order_acquire)) return hook();
    const ::utils::ps4::governor::heap_sample s = ::utils::ps4::governor::sample_heap(false);
    return s.cap ? ::utils::ps4::governor::heap_room(s) : room_unknown;
}

inline bool flag(const char* path) noexcept
{
    return ::access(path, F_OK) == 0;
}

// One notification per game boot through 'last' (a static of the caller, initialised to ~0u). Any thread,
// allocation-free.
inline void notify_once_per_boot(std::atomic<u32>& last, const char* message) noexcept
{
    const u32 generation = ::utils::ps4::adapt::g_title_seq.load(std::memory_order_acquire);
    u32 seen = last.load(std::memory_order_relaxed);
    if (seen == generation || !last.compare_exchange_strong(seen, generation, std::memory_order_acq_rel)) return;
    ps4_notify(message);
}

// An HLE allocation failed and the call returned an error code to the game: one startup.log line per process and
// subsystem ('noted' is a static of the caller). No heap allocation of its own.
inline void note_out_of_memory(std::atomic<bool>& noted, const char* what, u64 bytes)
{
    if (noted.exchange(true, std::memory_order_relaxed)) return;
    const u64 r = room();
    char line[256];
    std::snprintf(line, sizeof(line), "%s: out of emulator memory (%llu KiB requested, memory level %d, heap room %lld MiB); error returned to the game",
        what, static_cast<unsigned long long>(bytes >> 10), level(), r == room_unknown ? -1ll : static_cast<long long>(r >> 20));
    ps4_startup_checkpoint(line);
}

// Savestate admission, before the emulator stops for a savestate (home menu). False: refused, with one startup.log
// line and a notification; the game keeps running. True: one startup.log line with the numbers.
inline bool savestate_admit() noexcept
{
    const int lvl = level();
    const u64 r = room();
    const savestate_refusal why = savestate_check(r, lvl);
    char room_text[32];
    if (r == room_unknown) std::snprintf(room_text, sizeof(room_text), "unknown");
    else std::snprintf(room_text, sizeof(room_text), "%llu MiB", static_cast<unsigned long long>(r >> 20));

    char line[256];
    if (why == savestate_refusal::none)
    {
        std::snprintf(line, sizeof(line), "savestate: started (memory level %d, heap room %s; writer: %u thread, zstd level %d)", lvl, room_text,
            savestate_threads, savestate_zstd_level);
        ps4_startup_checkpoint(line);
        return true;
    }

    std::snprintf(line, sizeof(line), "savestate: refused before stopping the game (%s: memory level %d, heap room %s; needs level below 2 and %llu MiB)",
        why == savestate_refusal::level ? "memory critical" : "heap room short", lvl, room_text, static_cast<unsigned long long>(savestate_room_min >> 20));
    ps4_startup_checkpoint(line);
    ps4_notify(i18n::tr(i18n::str::HLEMEM_SAVESTATE_RECUSADO_MEMORIA_EMULADOR_BAIXA));
    return false;
}
} // namespace utils::ps4::hle_memory
#endif
