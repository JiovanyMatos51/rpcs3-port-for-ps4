#pragma once

// The selected game's own XMB media, as the PS3 shows it: its PIC1.PNG background, its PIC0.PNG (16:9) or PIC2.PNG
// (4:3) overlay and its SND0.AT3 music. Only the game's own files are ever used: a game without PIC1 keeps the normal
// XMB waves, a game without SND0 plays nothing; no image or sound is shipped, generated or substituted.
//
// This header is pure and host-tested (standard library only): the two settings files, the timeline, the director
// (when to read a game's files, what to show and how opaque), the file names, the bounds-checked RIFF/WAVE parser of
// SND0, the image geometry, the Q15 gain ramps, the PCM ring and the music mixer, the XMD1 cache record and the cache
// trim. The console part (file reads, PNG/JPEG decoding, ATRAC decoding, the AudioOut output) is rpcs3/ps4_xmb_media.cpp.

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace utils::ps4::xmb_media
{
// ---------------------------------------------------------------------------------------------------------------------
// Timeline (PS3-like values; one table, so a recording of a real PS3 can correct them in one place)

struct timing
{
    std::uint64_t settle_us = 500'000;        // the selection rests this long on a game before any file is read
    std::uint64_t background_in_us = 800'000; // PIC1 fades in over the waves
    std::uint64_t background_out_us = 400'000;
    std::uint64_t overlay_delay_us = 1'000'000; // PIC0/PIC2 starts this long after PIC1 is fully shown
    std::uint64_t overlay_in_us = 500'000;
    std::uint64_t overlay_out_us = 250'000;
    std::uint64_t music_in_us = 1'000'000;
    std::uint64_t music_out_us = 500'000;
    std::uint64_t music_level_us = 200'000;   // ramp of a level change (on / low)
    std::uint64_t music_system_ui_us = 150'000; // fade when the system UI comes to the front
    std::uint64_t music_boot_us = 30'000;     // ramp to silence before a boot or an exit (no click)
    std::uint64_t port_linger_us = 5'000'000; // the AudioOut port stays open this long after the music stopped
};

inline constexpr std::uint32_t output_rate = 48000;
inline constexpr int screen_width = 1280;
inline constexpr int screen_height = 720;

// Limits for the untrusted files (a larger file is refused, not truncated)
inline constexpr std::uint64_t background_file_limit = 16ull << 20;
inline constexpr std::uint64_t overlay_file_limit = 8ull << 20;
inline constexpr std::uint64_t music_file_limit = 16ull << 20;
inline constexpr int image_side_limit = 2048;
// 0.5 s of stereo S16 output for the decoded music, filled to 150 ms before a track starts
inline constexpr std::uint32_t ring_frames = 24000;
inline constexpr std::uint32_t prefill_frames = 7200;
// The output thread decodes one more frame while less than 100 ms is buffered
inline constexpr std::uint32_t low_water_frames = 4800;
// Ramp at the seam of a track that loops at its end (no loop points): avoids a click
inline constexpr std::uint32_t seam_frames = 960;
// On-disk cache of decoded backgrounds: trimmed to 72 MiB when it holds more than 96 MiB
inline constexpr std::uint64_t cache_budget_bytes = 96ull << 20;
inline constexpr std::uint64_t cache_trim_bytes = 72ull << 20;
inline constexpr std::uint64_t record_file_limit = 8ull << 20;

inline constexpr std::uint32_t frames_for(std::uint64_t us) noexcept
{
    return static_cast<std::uint32_t>(us * output_rate / 1'000'000);
}

// ---------------------------------------------------------------------------------------------------------------------
// Settings: two files, read when the XMB opens and after a change (never per frame)

enum class music_level : std::uint8_t { on, low, off };

inline constexpr char media_off_flag[] = "/data/rpcs3/xmb-media-off.flag"; // present: no background and no music
inline constexpr char music_level_file[] = "/data/rpcs3/xmb-music.txt";     // "low" or "off"; absent or other: on
inline constexpr char cache_dir[] = "/data/rpcs3/cache/xmb/media";

struct settings
{
    bool media = true;                     // background and music of the selected game
    music_level music = music_level::on;
    bool wide = true;                      // the next boot's aspect ratio is 16:9: PIC0, else PIC2
};

namespace detail
{
inline std::string_view trim(std::string_view text) noexcept
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r' || text.front() == '\n')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' || text.back() == '\n')) text.remove_suffix(1);
    return text;
}

// At most 'limit' bytes of a small file; nullopt when it cannot be opened
inline std::optional<std::string> read_small(const char* path, std::size_t limit)
{
    std::FILE* file = std::fopen(path, "rb");
    if (!file) return std::nullopt;
    std::string text(limit, '\0');
    const std::size_t got = std::fread(text.data(), 1, limit, file);
    std::fclose(file);
    text.resize(got);
    return text;
}

// Replaces 'path' with 'text' through a temporary file, so a power loss leaves the old file or the new one
inline bool write_small(const char* path, std::string_view text)
{
    const std::string temp = std::string(path) + ".tmp";
    std::FILE* file = std::fopen(temp.c_str(), "wb");
    if (!file) return false;
    const bool written = std::fwrite(text.data(), 1, text.size(), file) == text.size();
    const bool closed = std::fclose(file) == 0;
    if (!written || !closed)
    {
        std::remove(temp.c_str());
        return false;
    }
    if (std::rename(temp.c_str(), path) == 0) return true;
    // A host whose rename does not replace an existing file
    std::remove(path);
    if (std::rename(temp.c_str(), path) == 0) return true;
    std::remove(temp.c_str());
    return false;
}

inline bool file_exists(const char* path)
{
    std::FILE* file = std::fopen(path, "rb");
    if (!file) return false;
    std::fclose(file);
    return true;
}
} // namespace detail

inline music_level parse_music_level(std::string_view text) noexcept
{
    text = detail::trim(text);
    if (text == "low") return music_level::low;
    if (text == "off") return music_level::off;
    return music_level::on;
}

inline const char* music_level_token(music_level level) noexcept
{
    return level == music_level::low ? "low" : level == music_level::off ? "off" : "on";
}

// Left/Right and X on the game music row: on -> low -> off -> on (step +1), the other way round for -1
inline music_level step_music_level(music_level level, int step) noexcept
{
    const int n = (static_cast<int>(level) + (step < 0 ? 2 : 1)) % 3;
    return static_cast<music_level>(n);
}

// The 'Aspect ratio' key of the Video section of a settings YAML (RPCS3's own key): true for 4:3, false for any other
// value, nullopt when the file has no such key
inline std::optional<bool> yaml_aspect_is_4_3(std::string_view yaml) noexcept
{
    bool video = false;
    while (!yaml.empty())
    {
        const std::size_t end = yaml.find('\n');
        std::string_view line = yaml.substr(0, end);
        yaml = end == std::string_view::npos ? std::string_view{} : yaml.substr(end + 1);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty()) continue;
        if (line.front() != ' ' && line.front() != '\t')
        {
            video = detail::trim(line) == "Video:";
            continue;
        }
        if (!video) continue;
        const std::string_view item = detail::trim(line);
        if (item.starts_with("Aspect ratio:")) return detail::trim(item.substr(13)) == "4:3";
    }
    return std::nullopt;
}

// 'Aspect ratio: 4:3' in a settings YAML; anything else (or no key) is 16:9
inline bool yaml_aspect_4_3(std::string_view yaml) noexcept
{
    return yaml_aspect_is_4_3(yaml).value_or(false);
}

// The two switch files ('wide' is read by the console part from the global settings file)
inline settings read_settings(const char* off_flag = media_off_flag, const char* level_file = music_level_file)
{
    settings out;
    out.media = !detail::file_exists(off_flag);
    if (const auto text = detail::read_small(level_file, 64)) out.music = parse_music_level(*text);
    return out;
}

// Creates or removes the off flag; false when the file system refused (the caller keeps the old value)
inline bool write_media_switch(bool on, const char* off_flag = media_off_flag)
{
    if (on) return !detail::file_exists(off_flag) || std::remove(off_flag) == 0;
    return detail::file_exists(off_flag) || detail::write_small(off_flag, "off\n");
}

inline bool write_music_level(music_level level, const char* level_file = music_level_file)
{
    if (level == music_level::on) return !detail::file_exists(level_file) || std::remove(level_file) == 0;
    return detail::write_small(level_file, level == music_level::low ? "low\n" : "off\n");
}

// ---------------------------------------------------------------------------------------------------------------------
// Where the files come from

enum class source_kind : std::uint8_t { none, files, iso };

// Rows without media: '@' rows (utilities, folders, the drive row without an installed image), PKG files and the
// drive's virtual device (that would be read over the network). An ISO is read inside the image without mounting it.
inline source_kind source_of(std::string_view path) noexcept
{
    if (path.empty() || path.front() == '@' || path.starts_with("/vfsv0_")) return source_kind::none;
    const std::size_t dot = path.find_last_of('.');
    const std::size_t slash = path.find_last_of('/');
    if (dot != std::string_view::npos && (slash == std::string_view::npos || dot > slash))
    {
        std::string ext(path.substr(dot));
        for (char& c : ext) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
        if (ext == ".iso") return source_kind::iso;
        if (ext == ".pkg") return source_kind::none;
    }
    return source_kind::files;
}

// The folder that holds PARAM.SFO and the media files of a folder game (disc layout: its PS3_GAME/, HDD game: the
// folder itself) or of the game around an EBOOT.BIN (its parent's parent), as the launcher reads ICON0.PNG
inline std::string files_base(std::string_view path, bool is_dir, bool has_ps3_game)
{
    std::string base(path);
    while (base.size() > 1 && base.back() == '/') base.pop_back();
    if (is_dir) return base + (has_ps3_game ? "/PS3_GAME/" : "/");
    const std::size_t slash = base.find_last_of('/');
    return (slash == std::string::npos ? std::string(".") : base.substr(0, slash)) + "/../";
}

// Inside an ISO
inline constexpr char iso_folder[] = "PS3_GAME/";

enum class media_kind : std::uint8_t { background, overlay, music };

// RPCS3's order: the copy for the game's PS3 language (_NN) before the plain file; the overlay is PIC0 on a 16:9 output
// and PIC2 on 4:3
inline std::vector<std::string> candidate_names(media_kind kind, std::int32_t language, bool wide)
{
    const char* base = kind == media_kind::background ? "PIC1" : kind == media_kind::music ? "SND0" : wide ? "PIC0" : "PIC2";
    const char* ext = kind == media_kind::music ? ".AT3" : ".PNG";
    std::vector<std::string> names;
    if (language >= 0 && language <= 99)
    {
        char suffix[8];
        std::snprintf(suffix, sizeof(suffix), "_%02d", static_cast<int>(language));
        names.push_back(std::string(base) + suffix + ext);
    }
    names.push_back(std::string(base) + ext);
    return names;
}

// The PS3 language number (CELL_SYSUTIL_LANG_*) of the next boot: a chosen language as it is, else what the
// automatic choice runs ('automatic', resolved by the caller); out of range -> English (US)
inline std::int32_t media_language(std::int32_t chosen, std::int32_t automatic) noexcept
{
    if (chosen >= 0 && chosen < 20) return chosen;
    return automatic >= 0 && automatic < 20 ? automatic : 1;
}

// ---------------------------------------------------------------------------------------------------------------------
// SND0.AT3: RIFF/WAVE with ATRAC3 (tag 0x0270), ATRAC3plus (WAVE_FORMAT_EXTENSIBLE with Sony's GUID) or PCM16. Our own
// parser checks every size against the buffer before any decoder sees a byte; the decoder then only gets whole
// blocks of the data chunk.

enum class riff_codec : std::uint8_t { none, atrac3, atrac3plus, pcm16 };

inline const char* codec_name(riff_codec codec) noexcept
{
    switch (codec)
    {
    case riff_codec::atrac3: return "ATRAC3";
    case riff_codec::atrac3plus: return "ATRAC3plus";
    case riff_codec::pcm16: return "PCM16";
    default: return "none";
    }
}

inline constexpr std::array<std::uint8_t, 16> atrac3plus_guid{0xBF, 0xAA, 0x23, 0xE9, 0x58, 0xCB, 0x71, 0x44, 0xA1, 0x19, 0xFF, 0xFA, 0x01, 0xE4, 0xCE, 0x62};
inline constexpr std::uint32_t riff_chunk_limit = 64;

struct riff_info
{
    bool ok = false;
    const char* error = "";                  // why it is refused (static text)
    riff_codec codec = riff_codec::none;
    std::uint16_t format_tag = 0;
    std::uint16_t channels = 0;
    std::uint32_t sample_rate = 0;
    std::uint32_t byte_rate = 0;
    std::uint16_t block_align = 0;
    std::uint16_t bits = 0;
    std::uint64_t extra_offset = 0, extra_size = 0; // codec data after the format fields (ATRAC3: 14 bytes)
    std::uint64_t data_offset = 0, data_size = 0;   // the data chunk, clipped to the buffer
    std::uint64_t total_samples = 0;                // to play: 'fact' when present, never past the data
    std::uint32_t fact_delay = 0;                   // second 'fact' field (encoder delay; recorded, not applied)
    bool loop = false;                              // 'smpl' loop used
    std::uint64_t loop_start = 0, loop_end = 0;     // [start, end) in samples
    std::uint32_t chunks = 0;
};

// Samples per decoder packet: one ATRAC3 block holds 1024 samples, one ATRAC3plus block 2048; PCM goes in 1024-frame
// packets
inline std::uint32_t packet_samples(riff_codec codec) noexcept
{
    return codec == riff_codec::atrac3plus ? 2048 : codec == riff_codec::none ? 0 : 1024;
}

inline std::uint64_t packet_bytes(const riff_info& info) noexcept
{
    return info.codec == riff_codec::pcm16 ? std::uint64_t(info.block_align) * 1024 : info.block_align;
}

inline std::uint64_t packet_count(const riff_info& info) noexcept
{
    const std::uint64_t bytes = packet_bytes(info);
    if (!bytes) return 0;
    // ATRAC: whole blocks only; PCM: the last packet may be short
    return info.codec == riff_codec::pcm16 ? (info.data_size + bytes - 1) / bytes : info.data_size / bytes;
}

namespace detail
{
inline std::uint16_t le16(std::span<const std::uint8_t> b, std::size_t at) noexcept
{
    return static_cast<std::uint16_t>(b[at] | (b[at + 1] << 8));
}
inline std::uint32_t le32(std::span<const std::uint8_t> b, std::size_t at) noexcept
{
    return std::uint32_t(b[at]) | (std::uint32_t(b[at + 1]) << 8) | (std::uint32_t(b[at + 2]) << 16) | (std::uint32_t(b[at + 3]) << 24);
}
inline bool tag_is(std::span<const std::uint8_t> b, std::size_t at, const char (&tag)[5]) noexcept
{
    return b[at] == std::uint8_t(tag[0]) && b[at + 1] == std::uint8_t(tag[1]) && b[at + 2] == std::uint8_t(tag[2]) && b[at + 3] == std::uint8_t(tag[3]);
}

inline bool parse_fmt(std::span<const std::uint8_t> b, std::size_t at, std::size_t size, riff_info& info) noexcept
{
    if (size < 16)
    {
        info.error = "fmt chunk too short";
        return false;
    }
    info.format_tag = le16(b, at);
    info.channels = le16(b, at + 2);
    info.sample_rate = le32(b, at + 4);
    info.byte_rate = le32(b, at + 8);
    info.block_align = le16(b, at + 12);
    info.bits = le16(b, at + 14);
    std::size_t extension = 0;
    if (size >= 18) extension = std::min<std::size_t>(le16(b, at + 16), size - 18);
    if (info.format_tag == 0x0270)
    {
        info.codec = riff_codec::atrac3;
        info.extra_offset = at + 18;
        info.extra_size = extension;
    }
    else if (info.format_tag == 0xFFFE)
    {
        // valid bits (2), channel mask (4), sub-format GUID (16)
        if (extension < 22)
        {
            info.error = "extensible format header too short";
            return false;
        }
        if (!std::equal(atrac3plus_guid.begin(), atrac3plus_guid.end(), b.begin() + static_cast<std::ptrdiff_t>(at + 24)))
        {
            info.error = "extensible format is not ATRAC3plus";
            return false;
        }
        info.codec = riff_codec::atrac3plus;
        info.extra_offset = at + 40;
        info.extra_size = extension - 22;
    }
    else if (info.format_tag == 0x0001)
    {
        info.codec = riff_codec::pcm16;
    }
    else
    {
        info.error = "unsupported format tag";
        return false;
    }
    return true;
}
} // namespace detail

inline riff_info parse_riff(std::span<const std::uint8_t> b) noexcept
{
    riff_info info;
    if (b.size() < 12)
    {
        info.error = "file too short";
        return info;
    }
    if (!detail::tag_is(b, 0, "RIFF") || !detail::tag_is(b, 8, "WAVE"))
    {
        info.error = "not a RIFF WAVE file";
        return info;
    }
    // A RIFF size past the buffer (or too small) is clipped to the buffer
    std::uint64_t end = 8ull + detail::le32(b, 4);
    if (end > b.size() || end < 12) end = b.size();

    bool got_fmt = false, got_data = false, got_fact = false, got_smpl = false;
    std::uint64_t smpl_start = 0, smpl_end = 0;
    std::uint64_t pos = 12;
    while (pos + 8 <= end)
    {
        if (++info.chunks > riff_chunk_limit)
        {
            info.error = "more than 64 chunks";
            return info;
        }
        const std::size_t at = static_cast<std::size_t>(pos);
        const std::uint64_t declared = detail::le32(b, at + 4);
        const std::uint64_t body = pos + 8;
        const std::uint64_t size = std::min<std::uint64_t>(declared, end - body);
        const std::size_t body_at = static_cast<std::size_t>(body);
        if (detail::tag_is(b, at, "fmt "))
        {
            if (got_fmt)
            {
                info.error = "two fmt chunks";
                return info;
            }
            if (!detail::parse_fmt(b, body_at, static_cast<std::size_t>(size), info)) return info;
            got_fmt = true;
        }
        else if (detail::tag_is(b, at, "fact"))
        {
            if (size >= 4)
            {
                info.total_samples = detail::le32(b, body_at);
                got_fact = true;
            }
            if (size >= 8) info.fact_delay = detail::le32(b, body_at + 4);
        }
        else if (detail::tag_is(b, at, "smpl"))
        {
            // 36 bytes of sampler fields (the loop count at 28), then 24 bytes per loop (start at 8, end at 12)
            if (size >= 36 + 24 && detail::le32(b, body_at + 28) >= 1)
            {
                smpl_start = detail::le32(b, body_at + 36 + 8);
                smpl_end = detail::le32(b, body_at + 36 + 12);
                got_smpl = true;
            }
        }
        else if (detail::tag_is(b, at, "data"))
        {
            if (!got_fmt)
            {
                info.error = "data before fmt";
                return info;
            }
            if (got_data)
            {
                info.error = "two data chunks";
                return info;
            }
            info.data_offset = body;
            info.data_size = size;
            got_data = true;
        }
        // Chunks are padded to an even size
        pos = body + declared + (declared & 1);
    }
    if (!got_fmt || !got_data)
    {
        info.error = got_fmt ? "no data chunk" : "no fmt chunk";
        return info;
    }
    if (info.channels < 1 || info.channels > 2)
    {
        info.error = "not mono or stereo";
        return info;
    }
    if (info.sample_rate < 22050 || info.sample_rate > 48000)
    {
        info.error = "sample rate outside 22050-48000 Hz";
        return info;
    }
    switch (info.codec)
    {
    case riff_codec::atrac3:
        if (info.extra_size != 14)
        {
            info.error = "ATRAC3 header without its 14 codec bytes";
            return info;
        }
        [[fallthrough]];
    case riff_codec::atrac3plus:
        if (info.block_align < 8)
        {
            info.error = "ATRAC block size too small";
            return info;
        }
        break;
    case riff_codec::pcm16:
        if (info.bits != 16 || info.block_align != info.channels * 2)
        {
            info.error = "PCM that is not 16-bit";
            return info;
        }
        // Whole frames only
        info.data_size -= info.data_size % info.block_align;
        break;
    default:
        info.error = "unsupported format tag";
        return info;
    }
    const std::uint64_t packets = packet_count(info);
    if (!packets)
    {
        info.error = "data chunk shorter than one block";
        return info;
    }
    const std::uint64_t playable = info.codec == riff_codec::pcm16 ? info.data_size / info.block_align : packets * packet_samples(info.codec);
    info.total_samples = got_fact && info.total_samples ? std::min(info.total_samples, playable) : playable;
    // The first loop, when it lies inside the samples to play and lasts at least one ATRAC3 block (the end is
    // inclusive in 'smpl')
    if (got_smpl && smpl_start < smpl_end && smpl_end < info.total_samples && smpl_end - smpl_start + 1 >= 1024)
    {
        info.loop = true;
        info.loop_start = smpl_start;
        info.loop_end = smpl_end + 1;
    }
    info.ok = info.total_samples > 0;
    if (!info.ok) info.error = "no samples";
    return info;
}

// ---------------------------------------------------------------------------------------------------------------------
// Image geometry (the XMB frame is 1280x720)

struct rect
{
    int x = 0, y = 0, w = 0, h = 0;
    bool operator==(const rect&) const = default;
};

// The part of a background of w x h that fills the screen: all of it when its aspect is within 10 % of 16:9 (it is
// stretched), else the centred 16:9 part (scaled to cover the screen)
inline rect background_crop(int w, int h) noexcept
{
    if (w <= 0 || h <= 0) return {};
    const std::int64_t a = std::int64_t(w) * 90, lo = std::int64_t(h) * 144, hi = std::int64_t(h) * 176;
    if (a >= lo && a <= hi) return {0, 0, w, h};
    if (a > hi)
    {
        const int cw = static_cast<int>(std::clamp<std::int64_t>((std::int64_t(h) * 16 + 4) / 9, 1, w));
        return {(w - cw) / 2, 0, cw, h};
    }
    const int ch = static_cast<int>(std::clamp<std::int64_t>((std::int64_t(w) * 9 + 8) / 16, 1, h));
    return {0, (h - ch) / 2, w, ch};
}

// PIC0 (1000x560) and PIC2 (310x250) are laid out for 1080 lines: scaled by 720/1080, kept inside the screen,
// right-aligned and vertically centred
inline rect overlay_box(int w, int h) noexcept
{
    if (w <= 0 || h <= 0) return {};
    std::int64_t ow = std::max<std::int64_t>(1, (std::int64_t(w) * 720 + 540) / 1080);
    std::int64_t oh = std::max<std::int64_t>(1, (std::int64_t(h) * 720 + 540) / 1080);
    if (ow > screen_width || oh > screen_height)
    {
        if (ow * screen_height > oh * screen_width)
        {
            oh = std::max<std::int64_t>(1, (oh * screen_width + ow / 2) / ow);
            ow = screen_width;
        }
        else
        {
            ow = std::max<std::int64_t>(1, (ow * screen_height + oh / 2) / oh);
            oh = screen_height;
        }
    }
    return {screen_width - static_cast<int>(ow), (screen_height - static_cast<int>(oh)) / 2, static_cast<int>(ow), static_cast<int>(oh)};
}

// RGB / straight-alpha RGBA bytes to the XMB's ARGB pixels
inline void pack_rgb(const std::uint8_t* rgb, std::size_t count, std::uint32_t* out) noexcept
{
    for (std::size_t i = 0; i < count; ++i, rgb += 3) out[i] = 0xff000000u | (std::uint32_t(rgb[0]) << 16) | (std::uint32_t(rgb[1]) << 8) | rgb[2];
}

inline void pack_rgba(const std::uint8_t* rgba, std::size_t count, std::uint32_t* out) noexcept
{
    for (std::size_t i = 0; i < count; ++i, rgba += 4)
        out[i] = (std::uint32_t(rgba[3]) << 24) | (std::uint32_t(rgba[0]) << 16) | (std::uint32_t(rgba[1]) << 8) | rgba[2];
}

// ---------------------------------------------------------------------------------------------------------------------
// Gain (Q15: 32768 = unity)

inline constexpr std::int32_t gain_unity = 32768;
inline constexpr std::int32_t gain_low = 13107; // 0.4

inline std::int32_t level_gain(music_level level) noexcept
{
    return level == music_level::on ? gain_unity : level == music_level::low ? gain_low : 0;
}

// A linear ramp per output frame that ends exactly on its target
class gain_ramp
{
public:
    constexpr explicit gain_ramp(std::int32_t value = 0) noexcept : m_from(value), m_to(value) {}

    // From the current value to 'to' over 'frames' frames (0: at once)
    void set(std::int32_t to, std::uint32_t frames) noexcept
    {
        m_from = value();
        m_to = to;
        m_frames = frames;
        m_pos = 0;
    }
    std::int32_t value() const noexcept
    {
        if (m_pos >= m_frames) return m_to;
        return m_from + static_cast<std::int32_t>((std::int64_t(m_to) - m_from) * m_pos / m_frames);
    }
    // The gain of the next frame; the last frame of the ramp gets the target itself
    std::int32_t next() noexcept
    {
        if (m_pos < m_frames) ++m_pos;
        return value();
    }
    bool done() const noexcept { return m_pos >= m_frames; }
    std::int32_t target() const noexcept { return m_to; }
    std::uint32_t remaining() const noexcept { return m_frames - std::min(m_pos, m_frames); }

private:
    std::int32_t m_from = 0, m_to = 0;
    std::uint32_t m_frames = 0, m_pos = 0;
};

inline std::int16_t scale_sample(std::int32_t sample, std::int32_t gain) noexcept
{
    const std::int64_t v = (std::int64_t(sample) * gain) >> 15;
    return static_cast<std::int16_t>(std::clamp<std::int64_t>(v, -32768, 32767));
}

// ---------------------------------------------------------------------------------------------------------------------
// Stereo S16 ring of decoded music (single thread: the loader fills it before the track starts, then only the output
// thread touches it)

class pcm_ring
{
public:
    explicit pcm_ring(std::uint32_t frames = ring_frames) : m_data(std::size_t(frames) * 2), m_capacity(frames) {}

    std::uint32_t capacity() const noexcept { return m_capacity; }
    std::uint32_t size() const noexcept { return m_size; }
    std::uint32_t space() const noexcept { return m_capacity - m_size; }
    void clear() noexcept { m_read = m_size = 0; }

    std::uint32_t push(const std::int16_t* frames, std::uint32_t n) noexcept
    {
        n = std::min(n, space());
        std::uint32_t at = (m_read + m_size) % m_capacity;
        for (std::uint32_t done = 0; done < n;)
        {
            const std::uint32_t run = std::min(n - done, m_capacity - at);
            std::memcpy(m_data.data() + std::size_t(at) * 2, frames + std::size_t(done) * 2, std::size_t(run) * 4);
            done += run;
            at = (at + run) % m_capacity;
        }
        m_size += n;
        return n;
    }

    std::uint32_t pop(std::int16_t* out, std::uint32_t n) noexcept
    {
        n = std::min(n, m_size);
        for (std::uint32_t done = 0; done < n;)
        {
            const std::uint32_t run = std::min(n - done, m_capacity - m_read);
            std::memcpy(out + std::size_t(done) * 2, m_data.data() + std::size_t(m_read) * 2, std::size_t(run) * 4);
            done += run;
            m_read = (m_read + run) % m_capacity;
        }
        m_size -= n;
        return n;
    }

    // Ramps the newest 'n' buffered frames down to silence (the end of a track before it starts again)
    void fade_tail(std::uint32_t n) noexcept
    {
        n = std::min(n, m_size);
        if (!n) return;
        const std::uint32_t first = (m_read + m_size - n) % m_capacity;
        for (std::uint32_t i = 0; i < n; ++i)
        {
            const std::int32_t gain = static_cast<std::int32_t>(std::int64_t(gain_unity) * (n - 1 - i) / n);
            std::int16_t* frame = m_data.data() + std::size_t((first + i) % m_capacity) * 2;
            frame[0] = scale_sample(frame[0], gain);
            frame[1] = scale_sample(frame[1], gain);
        }
    }

private:
    std::vector<std::int16_t> m_data;
    std::uint32_t m_capacity = 0, m_read = 0, m_size = 0;
};

// ---------------------------------------------------------------------------------------------------------------------
// Music mixer: one track playing ('current'), one waiting for it ('pending', it starts once the current one has faded
// out) and a few finished ones for the UI thread to free. It never allocates. Not thread-safe: the caller serializes
// every call under one mutex, which the output thread only try-locks, so it never waits for the UI thread.
// Track: std::uint32_t pull(std::int16_t* stereo, std::uint32_t frames) (frames produced; fewer = underrun) and
// bool ended() const (nothing more will come, e.g. a decode failure).

template <typename Track>
class basic_mixer
{
public:
    static constexpr std::size_t retired_slots = 4;

    // UI thread. Returns the pending track it replaces (the caller frees it outside its lock).
    [[nodiscard]] std::unique_ptr<Track> play(std::unique_ptr<Track> track, std::uint32_t in_frames, std::uint32_t out_frames) noexcept
    {
        std::unique_ptr<Track> replaced = std::move(m_pending);
        m_pending = std::move(track);
        m_pending_in = in_frames;
        if (m_current) leave(out_frames);
        return replaced;
    }

    // Fades the current track out (then it is freed); drops the pending one, which it returns
    [[nodiscard]] std::unique_ptr<Track> stop(std::uint32_t out_frames) noexcept
    {
        std::unique_ptr<Track> dropped = std::move(m_pending);
        if (m_current) leave(out_frames);
        return dropped;
    }

    void set_level(std::int32_t gain, std::uint32_t frames) noexcept { m_level.set(gain, frames); }
    std::int32_t level_target() const noexcept { return m_level.target(); }

    std::array<std::unique_ptr<Track>, retired_slots> take_retired() noexcept { return std::exchange(m_retired, {}); }

    // Only while no output thread renders (the port is closed): every track it holds, playing, waiting or finished,
    // for the caller to free; the mixer is then silent and its fades reset (the level stays)
    std::array<std::unique_ptr<Track>, retired_slots + 2> take_all() noexcept
    {
        std::array<std::unique_ptr<Track>, retired_slots + 2> all{};
        all[0] = std::move(m_current);
        all[1] = std::move(m_pending);
        for (std::size_t i = 0; i < retired_slots; ++i) all[i + 2] = std::move(m_retired[i]);
        m_fade = gain_ramp(0);
        m_leaving = false;
        return all;
    }

    // Nothing playing and nothing waiting (finished tracks may still wait in the retired slots)
    bool silent() const noexcept { return !m_current && !m_pending; }
    std::uint64_t underrun_blocks() const noexcept { return m_underruns; }

    // Output thread: always writes 'frames' stereo frames (silence when nothing plays)
    void render(std::int16_t* out, std::uint32_t frames) noexcept
    {
        if (!m_current && m_pending)
        {
            m_current = std::move(m_pending);
            m_fade = gain_ramp(0);
            m_fade.set(gain_unity, m_pending_in);
            m_leaving = false;
        }
        if (!m_current)
        {
            std::memset(out, 0, std::size_t(frames) * 4);
            return;
        }
        const std::uint32_t got = std::min(m_current->pull(out, frames), frames);
        if (got < frames)
        {
            std::memset(out + std::size_t(got) * 2, 0, std::size_t(frames - got) * 4);
            if (!m_current->ended()) ++m_underruns;
        }
        for (std::uint32_t i = 0; i < frames; ++i)
        {
            const std::int32_t gain = static_cast<std::int32_t>((std::int64_t(m_fade.next()) * m_level.next()) >> 15);
            out[i * 2] = scale_sample(out[i * 2], gain);
            out[i * 2 + 1] = scale_sample(out[i * 2 + 1], gain);
        }
        // Faded out, or nothing more to come: free it (if every slot is still taken, it stays silent until one frees)
        const bool faded = m_leaving && m_fade.done() && m_fade.target() == 0;
        if ((faded || m_current->ended()) && retire(m_current)) m_leaving = false;
    }

private:
    void leave(std::uint32_t out_frames) noexcept
    {
        // A running fade-out is only ever shortened (the 30 ms ramp before a boot)
        if (m_leaving && m_fade.target() == 0 && m_fade.remaining() <= out_frames) return;
        m_leaving = true;
        m_fade.set(0, out_frames);
    }

    bool retire(std::unique_ptr<Track>& track) noexcept
    {
        for (auto& slot : m_retired)
        {
            if (slot) continue;
            slot = std::move(track);
            return true;
        }
        return false;
    }

    std::unique_ptr<Track> m_current, m_pending;
    std::array<std::unique_ptr<Track>, retired_slots> m_retired{};
    gain_ramp m_fade{0}, m_level{gain_unity};
    std::uint32_t m_pending_in = 0;
    bool m_leaving = false;
    std::uint64_t m_underruns = 0;
};

// ---------------------------------------------------------------------------------------------------------------------
// Director (UI thread): turns the selection into media jobs and the shown media into alphas and music commands.
// Rules: nothing is read while the selection moves (it must rest 'settle_us' on a game); one background at a time
// (a new one fades in only after the old one has faded out); the overlay follows the background; the music starts
// with the background and only while its gate is open; a list or dialog of the selected game holds what is shown.

// Shared with the loader jobs: the generation the director wants now (a job checks it between stages and abandons
// when the selection moved on) and the per-visit budget of job log lines
struct request_state
{
    std::atomic<std::uint32_t> wanted{0};
    std::atomic<std::uint32_t> lines{0};
};

struct media_request
{
    std::string path;           // the media source: a game folder, an EBOOT.BIN or an ISO
    std::string serial;         // TITLE_ID of the row ("" = unknown: no localized names)
    std::uint32_t generation = 0;
    bool want_images = true;    // false: only the music (the background is already shown)
    bool want_music = false;    // prepare SND0 (music not off, loader thread)
    bool cache_only = false;    // no loader thread: cached backgrounds only, no music
    bool wide = true;           // PIC0, else PIC2
    std::shared_ptr<request_state> state;
};

struct director_input
{
    std::string_view row;       // media source of the selected row ("" = none: '@' rows, folders, PKGs, other columns)
    std::string_view serial;
    bool hold = false;          // a list, popup, keyboard or dialog of the selected game is open: keep, ask for nothing
    bool visible = true;        // the game column is shown (not the settings or save-data pages, not another column)
    bool system_ui = false;     // the system UI is in front: the music stops, the images stay (nothing is drawn)
    bool music_gate = true;     // the previous game has fully stopped and no modal dialog runs
};

// Payload: std::uint32_t generation; bool has_background() const; bool has_overlay() const; bool has_music() const
// (a prepared track); bool has_music_file() const (an SND0 exists); bool music_only() const.
template <typename Payload>
class basic_director
{
public:
    struct output
    {
        bool drop = false;                        // drop the media jobs that did not start yet (the selection moved)
        std::optional<media_request> post;        // post this media job
        std::shared_ptr<Payload> present;         // move its images into the scene now (the scene holds no media now)
        bool clear = false;                       // the shown media has faded out: free the scene's media images
        std::shared_ptr<Payload> play;            // start the music track this payload holds
        bool stop_music = false;                  // fade the music out over 'stop_us' (it is freed after)
        std::uint64_t stop_us = 0;
        unsigned background_alpha = 0, overlay_alpha = 0;
        bool animating = false;                   // a fade runs: keep at least 30 frames per second
    };

    explicit basic_director(const timing& t = {}) : m_t(t), m_state(std::make_shared<request_state>()) {}

    // At the XMB open and after a settings change; 'inline_loader': the jobs run on the UI thread (no loader thread)
    void configure(const settings& s, bool inline_loader) noexcept
    {
        const bool music_back = m_settings.music == music_level::off && s.music != music_level::off;
        m_settings = s;
        m_inline = inline_loader;
        m_sealed = false;
        if (music_back && showing() && m_show.music_file && !m_show.track) m_show.music_again = true;
    }

    output tick(std::uint64_t now, const director_input& in)
    {
        output out;
        // Released (a boot or an exit follows): nothing is asked for again until the next configure()
        if (m_sealed) return out;
        std::string_view want = m_settings.media && in.visible ? in.row : std::string_view{};
        if (in.hold || in.system_ui) want = m_target;
        if (want != m_target)
        {
            m_target.assign(want);
            m_serial.assign(in.serial);
            m_rest_since = now;
            m_posted = false;
            m_music_posted = false;
            bump();
            out.drop = true;
            m_ready.reset();
            if (showing() && m_show.path != m_target) leave(now, out);
        }

        // Settled on a game: one job for it
        if (!m_target.empty() && !m_posted && !in.hold && !in.system_ui && now >= m_rest_since && now - m_rest_since >= m_t.settle_us)
        {
            m_posted = true;
            out.post = request(true);
        }

        // One background at a time
        if (m_ready && !m_show.active) present(now, out);

        // Music: with the background, while its gate is open
        const bool music_ok = showing() && in.music_gate && !in.system_ui && m_settings.music != music_level::off && !m_inline;
        if (m_show.music_playing && !music_ok)
        {
            out.stop_music = true;
            out.stop_us = in.system_ui ? m_t.music_system_ui_us : m_t.music_out_us;
            m_show.music_playing = false;
            // Its track is gone: a fresh one when the gate opens again (it starts from the beginning, as on the PS3)
            m_show.music_again = showing() && m_show.music_file;
        }
        if (music_ok && !m_show.music_playing)
        {
            if (m_show.track)
            {
                out.play = m_show.track_payload;
                m_show.track = false;
                m_show.track_payload.reset();
                m_show.music_playing = true;
            }
            else if (m_show.music_again && !m_music_posted && !in.hold && !out.post)
            {
                m_show.music_again = false;
                m_music_posted = true;
                out.post = request(false);
            }
        }

        // Alphas
        out.background_alpha = background_alpha(now);
        out.overlay_alpha = overlay_alpha(now);
        if (m_show.active && m_show.leaving && !out.background_alpha && !out.overlay_alpha)
        {
            out.clear = true;
            m_show = {};
        }
        out.animating = m_show.active && (m_show.leaving || (m_show.background && out.background_alpha < 255) ||
            (m_show.overlay && out.overlay_alpha < 255));
        return out;
    }

    // A media job's result. True: it is for the current request and the director keeps it; false: stale or empty,
    // the caller drops it.
    bool arrived(const std::shared_ptr<Payload>& payload)
    {
        if (!payload || payload->generation != m_generation) return false;
        if (payload->music_only())
        {
            m_music_posted = false;
            if (!showing() || !payload->has_music()) return false;
            m_show.track = true;
            m_show.track_payload = payload;
            return true;
        }
        if (!payload->has_background() && !payload->has_overlay() && !payload->has_music() && !payload->has_music_file()) return false;
        m_ready = payload;
        return true;
    }

    // Every payload it holds goes to 'each' (the release before a boot counts and frees them), then it forgets them and
    // stays inert until configure() is called again
    template <typename F>
    void drain(F&& each)
    {
        if (m_show.track_payload == m_show.payload) m_show.track_payload.reset();
        for (std::shared_ptr<Payload>* p : {&m_ready, &m_show.payload, &m_show.track_payload})
            if (*p) each(std::move(*p));
        m_ready.reset();
        m_show = {};
        m_target.clear();
        m_posted = true;
        m_sealed = true;
        bump();
    }

    const std::shared_ptr<request_state>& state() const noexcept { return m_state; }
    const settings& current_settings() const noexcept { return m_settings; }
    // Media shown and not fading out
    bool showing() const noexcept { return m_show.active && !m_show.leaving; }
    std::uint32_t generation() const noexcept { return m_generation; }

private:
    struct shown
    {
        bool active = false;
        std::string path, serial;
        std::shared_ptr<Payload> payload;       // the presented payload (its images are in the scene)
        std::shared_ptr<Payload> track_payload; // the payload whose track has not started yet
        bool background = false, overlay = false;
        bool track = false;                     // a prepared track waits in track_payload
        bool music_file = false;
        bool music_playing = false;
        bool music_again = false;               // ask for a new track when the music may play again
        bool leaving = false;
        std::uint64_t in_us = 0, out_us = 0;
        unsigned background_from = 0, overlay_from = 0;
    };

    void bump() noexcept { m_state->wanted.store(++m_generation, std::memory_order_relaxed); }

    media_request request(bool images)
    {
        bump();
        media_request r;
        r.path = images ? m_target : m_show.path;
        r.serial = images ? m_serial : m_show.serial;
        r.generation = m_generation;
        r.want_images = images;
        r.want_music = m_settings.music != music_level::off && !m_inline;
        r.cache_only = m_inline;
        r.wide = m_settings.wide;
        r.state = m_state;
        return r;
    }

    void present(std::uint64_t now, output& out)
    {
        m_show = {};
        m_show.active = true;
        m_show.path = m_target;
        m_show.serial = m_serial;
        m_show.payload = std::move(m_ready);
        m_show.background = m_show.payload->has_background();
        m_show.overlay = m_show.payload->has_overlay();
        m_show.music_file = m_show.payload->has_music_file();
        m_show.track = m_show.payload->has_music();
        if (m_show.track) m_show.track_payload = m_show.payload;
        m_show.in_us = now;
        out.present = m_show.payload;
    }

    void leave(std::uint64_t now, output& out)
    {
        m_show.background_from = background_alpha(now);
        m_show.overlay_from = overlay_alpha(now);
        m_show.leaving = true;
        m_show.out_us = now;
        m_show.track = false;
        m_show.track_payload.reset();
        m_show.music_again = false;
        if (m_show.music_playing)
        {
            out.stop_music = true;
            out.stop_us = m_t.music_out_us;
            m_show.music_playing = false;
        }
    }

    static unsigned ramp_up(std::uint64_t now, std::uint64_t start, std::uint64_t length) noexcept
    {
        if (now < start) return 0;
        if (!length || now - start >= length) return 255;
        return static_cast<unsigned>(255 * (now - start) / length);
    }

    // Down from 'from' at the full fade's rate (a partly shown image takes a part of the time)
    static unsigned ramp_down(std::uint64_t now, std::uint64_t start, unsigned from, std::uint64_t length) noexcept
    {
        const std::uint64_t own = length * from / 255;
        if (!own || now - start >= own) return 0;
        return static_cast<unsigned>(from - from * (now - start) / own);
    }

    unsigned background_alpha(std::uint64_t now) const noexcept
    {
        if (!m_show.active || !m_show.background) return 0;
        if (m_show.leaving) return ramp_down(now, m_show.out_us, m_show.background_from, m_t.background_out_us);
        return ramp_up(now, m_show.in_us, m_t.background_in_us);
    }

    unsigned overlay_alpha(std::uint64_t now) const noexcept
    {
        if (!m_show.active || !m_show.overlay) return 0;
        if (m_show.leaving) return ramp_down(now, m_show.out_us, m_show.overlay_from, m_t.overlay_out_us);
        const std::uint64_t start = m_show.in_us + (m_show.background ? m_t.background_in_us : 0) + m_t.overlay_delay_us;
        return ramp_up(now, start, m_t.overlay_in_us);
    }

    timing m_t;
    settings m_settings;
    bool m_inline = false;
    std::shared_ptr<request_state> m_state;
    std::uint32_t m_generation = 0;
    std::string m_target, m_serial;
    std::uint64_t m_rest_since = 0;
    bool m_posted = false;
    bool m_music_posted = false;
    bool m_sealed = false;
    std::shared_ptr<Payload> m_ready;
    shown m_show;
};

// ---------------------------------------------------------------------------------------------------------------------
// The order every exit of the XMB loop releases the media in, before a game boots: the music first (its AudioOut
// port closed and its output thread joined), then the media jobs that did not start, then the results nobody took
// (after the loader is joined), then the scene's images. Each step returns what it freed.

struct release_totals
{
    std::uint64_t music_bytes = 0, image_bytes = 0;
    std::uint32_t results = 0;
};

template <typename StopMusic, typename DropJobs, typename DrainResults, typename ClearImages>
release_totals release_in_order(StopMusic&& stop_music, DropJobs&& drop_jobs, DrainResults&& drain_results, ClearImages&& clear_images)
{
    release_totals totals;
    totals.music_bytes += stop_music();
    drop_jobs();
    drain_results(totals);
    totals.image_bytes += clear_images();
    return totals;
}

// ---------------------------------------------------------------------------------------------------------------------
// XMD1: one cache record per game, /data/rpcs3/cache/xmb/media/<fnv1a64 of path, language and aspect>.bin, written
// atomically by the console part. It holds the background as a 1280x720 JPEG and the overlay as zlib-compressed ARGB,
// plus what the SND0 lookup found. A record is used only while every source file it was made from has the same size
// and mtime ("no media" is a valid record too). Little-endian, ending with a checksum of the rest; every field is
// bounds-checked when read.

inline constexpr std::uint32_t record_magic = 0x31444d58; // "XMD1"
inline constexpr std::uint32_t record_version = 1;
inline constexpr std::size_t record_path_limit = 4096;
inline constexpr std::size_t record_stamp_limit = 8;
inline constexpr std::size_t record_blob_limit = 4u << 20;

struct source_stamp
{
    bool present = false;
    std::uint64_t size = 0;
    std::int64_t mtime = 0;
    bool operator==(const source_stamp&) const = default;
};

struct record_key
{
    std::string path;
    std::int32_t language = 1;
    bool wide = true;
    std::vector<source_stamp> stamps; // ISO: the image; files: every candidate name, present or not
};

struct media_record
{
    record_key key;
    bool background = false, overlay = false;
    bool music = false;              // an SND0 file exists
    bool music_unsupported = false;  // ... and its header was refused (not tried again until it changes)
    std::uint8_t music_name = 0;     // which SND0 candidate it is
    riff_codec codec = riff_codec::none;
    std::uint32_t music_rate = 0;
    std::uint16_t music_channels = 0;
    std::uint16_t background_w = 0, background_h = 0, overlay_w = 0, overlay_h = 0; // source sizes (log lines)
    std::vector<std::uint8_t> jpeg;  // the background, 1280x720
    rect overlay_box_px{};           // where the overlay goes and its size
    std::vector<std::uint8_t> overlay_zlib; // overlay_box_px.w x h ARGB pixels, deflated
};

inline std::uint64_t fnv1a64(std::string_view text) noexcept
{
    std::uint64_t hash = 0xcbf29ce484222325ull;
    for (const char c : text)
    {
        hash ^= static_cast<unsigned char>(c);
        hash *= 0x100000001b3ull;
    }
    return hash;
}

inline std::string record_name(const record_key& key)
{
    char lang[16];
    std::snprintf(lang, sizeof(lang), "\n%d\n", static_cast<int>(key.language));
    const std::uint64_t hash = fnv1a64(key.path + lang + (key.wide ? "16:9" : "4:3"));
    char name[32];
    std::snprintf(name, sizeof(name), "%016llx.bin", static_cast<unsigned long long>(hash));
    return name;
}

namespace detail
{
struct record_writer
{
    std::vector<std::uint8_t> bytes;
    void u8v(std::uint8_t v) { bytes.push_back(v); }
    void u16v(std::uint16_t v) { for (int i = 0; i < 2; ++i) bytes.push_back(static_cast<std::uint8_t>(v >> (8 * i))); }
    void u32v(std::uint32_t v) { for (int i = 0; i < 4; ++i) bytes.push_back(static_cast<std::uint8_t>(v >> (8 * i))); }
    void u64v(std::uint64_t v) { for (int i = 0; i < 8; ++i) bytes.push_back(static_cast<std::uint8_t>(v >> (8 * i))); }
    void blob(std::span<const std::uint8_t> data)
    {
        u32v(static_cast<std::uint32_t>(data.size()));
        bytes.insert(bytes.end(), data.begin(), data.end());
    }
};

struct record_reader
{
    std::span<const std::uint8_t> bytes;
    std::size_t at = 0;
    bool ok = true;

    bool take(std::size_t n) noexcept
    {
        if (!ok || n > bytes.size() - at) ok = false;
        return ok;
    }
    std::uint64_t le(std::size_t n) noexcept
    {
        if (!take(n)) return 0;
        std::uint64_t v = 0;
        for (std::size_t i = 0; i < n; ++i) v |= std::uint64_t(bytes[at + i]) << (8 * i);
        at += n;
        return v;
    }
    std::vector<std::uint8_t> blob(std::size_t limit)
    {
        const std::size_t n = static_cast<std::size_t>(le(4));
        if (n > limit) ok = false;
        if (!take(n)) return {};
        std::vector<std::uint8_t> out(bytes.begin() + static_cast<std::ptrdiff_t>(at), bytes.begin() + static_cast<std::ptrdiff_t>(at + n));
        at += n;
        return out;
    }
};
} // namespace detail

inline std::vector<std::uint8_t> encode_record(const media_record& r)
{
    detail::record_writer w;
    w.u32v(record_magic);
    w.u32v(record_version);
    w.blob({reinterpret_cast<const std::uint8_t*>(r.key.path.data()), r.key.path.size()});
    w.u32v(static_cast<std::uint32_t>(r.key.language));
    w.u8v(r.key.wide ? 1 : 0);
    w.u32v(static_cast<std::uint32_t>(r.key.stamps.size()));
    for (const source_stamp& s : r.key.stamps)
    {
        w.u8v(s.present ? 1 : 0);
        w.u64v(s.size);
        w.u64v(static_cast<std::uint64_t>(s.mtime));
    }
    w.u8v(static_cast<std::uint8_t>((r.background ? 1 : 0) | (r.overlay ? 2 : 0) | (r.music ? 4 : 0) | (r.music_unsupported ? 8 : 0)));
    w.u8v(r.music_name);
    w.u8v(static_cast<std::uint8_t>(r.codec));
    w.u32v(r.music_rate);
    w.u16v(r.music_channels);
    w.u16v(r.background_w);
    w.u16v(r.background_h);
    w.u16v(r.overlay_w);
    w.u16v(r.overlay_h);
    w.blob(r.jpeg);
    const rect box = r.overlay ? r.overlay_box_px : rect{};
    w.u32v(static_cast<std::uint32_t>(box.x));
    w.u32v(static_cast<std::uint32_t>(box.y));
    w.u32v(static_cast<std::uint32_t>(box.w));
    w.u32v(static_cast<std::uint32_t>(box.h));
    w.blob(r.overlay_zlib);
    // Last: FNV-1a 64 of everything before it, so a record damaged on the disk is a miss, not a wrong picture
    w.u64v(fnv1a64({reinterpret_cast<const char*>(w.bytes.data()), w.bytes.size()}));
    return std::move(w.bytes);
}

// The record for 'expect' (same path, language, aspect and source stamps), or nullopt for anything else: another
// version, other sources, a field out of range, a truncated or over-long file
inline std::optional<media_record> decode_record(std::span<const std::uint8_t> bytes, const record_key& expect)
{
    if (bytes.size() > record_file_limit || bytes.size() < 16) return std::nullopt;
    {
        detail::record_reader tail{bytes, bytes.size() - 8};
        const std::uint64_t sum = tail.le(8);
        bytes = bytes.first(bytes.size() - 8);
        if (sum != fnv1a64({reinterpret_cast<const char*>(bytes.data()), bytes.size()})) return std::nullopt;
    }
    detail::record_reader in{bytes};
    if (in.le(4) != record_magic || in.le(4) != record_version) return std::nullopt;
    media_record r;
    const std::vector<std::uint8_t> path = in.blob(record_path_limit);
    r.key.path.assign(path.begin(), path.end());
    r.key.language = static_cast<std::int32_t>(static_cast<std::uint32_t>(in.le(4)));
    const std::uint64_t wide = in.le(1);
    r.key.wide = wide == 1;
    const std::uint64_t stamps = in.le(4);
    if (!in.ok || wide > 1 || stamps > record_stamp_limit) return std::nullopt;
    for (std::uint64_t i = 0; i < stamps; ++i)
    {
        source_stamp s;
        const std::uint64_t present = in.le(1);
        s.present = present == 1;
        s.size = in.le(8);
        s.mtime = static_cast<std::int64_t>(in.le(8));
        if (present > 1) return std::nullopt;
        r.key.stamps.push_back(s);
    }
    const std::uint64_t flags = in.le(1);
    r.background = flags & 1;
    r.overlay = flags & 2;
    r.music = flags & 4;
    r.music_unsupported = flags & 8;
    r.music_name = static_cast<std::uint8_t>(in.le(1));
    const std::uint64_t codec = in.le(1);
    r.music_rate = static_cast<std::uint32_t>(in.le(4));
    r.music_channels = static_cast<std::uint16_t>(in.le(2));
    r.background_w = static_cast<std::uint16_t>(in.le(2));
    r.background_h = static_cast<std::uint16_t>(in.le(2));
    r.overlay_w = static_cast<std::uint16_t>(in.le(2));
    r.overlay_h = static_cast<std::uint16_t>(in.le(2));
    r.jpeg = in.blob(record_blob_limit);
    r.overlay_box_px.x = static_cast<int>(static_cast<std::int32_t>(static_cast<std::uint32_t>(in.le(4))));
    r.overlay_box_px.y = static_cast<int>(static_cast<std::int32_t>(static_cast<std::uint32_t>(in.le(4))));
    const std::uint64_t ow = in.le(4), oh = in.le(4);
    r.overlay_zlib = in.blob(record_blob_limit);
    if (!in.ok || in.at != bytes.size() || flags > 15 || codec > static_cast<std::uint8_t>(riff_codec::pcm16)) return std::nullopt;
    r.codec = static_cast<riff_codec>(codec);
    // Consistency: a background needs its JPEG, an overlay its pixels inside the screen
    if (r.background != !r.jpeg.empty() || r.overlay != !r.overlay_zlib.empty()) return std::nullopt;
    if (r.overlay)
    {
        if (!ow || !oh || ow > std::uint64_t(screen_width) || oh > std::uint64_t(screen_height)) return std::nullopt;
        if (r.overlay_box_px.x < 0 || r.overlay_box_px.y < 0 || r.overlay_box_px.x + std::int64_t(ow) > screen_width ||
            r.overlay_box_px.y + std::int64_t(oh) > screen_height) return std::nullopt;
        r.overlay_box_px.w = static_cast<int>(ow);
        r.overlay_box_px.h = static_cast<int>(oh);
    }
    else if (ow || oh) return std::nullopt;
    if (r.music_unsupported && !r.music) return std::nullopt;
    if (r.key.path != expect.path || r.key.language != expect.language || r.key.wide != expect.wide || r.key.stamps != expect.stamps)
        return std::nullopt;
    return r;
}

// ---------------------------------------------------------------------------------------------------------------------
// Cache trim (once per process, on the loader): the oldest records go first. Only names of this cache's own form
// (16 lower-case hex digits + ".bin") are ever selected.

struct cache_file
{
    std::string name;
    std::uint64_t size = 0;
    std::int64_t mtime = 0;
};

inline bool is_cache_name(std::string_view name) noexcept
{
    if (name.size() != 20 || !name.ends_with(".bin")) return false;
    for (std::size_t i = 0; i < 16; ++i)
    {
        const char c = name[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

// The names to remove: none while the cache's own files total at most 'budget', else the oldest until at most 'keep'
inline std::vector<std::string> trim_selection(std::vector<cache_file> files, std::uint64_t budget = cache_budget_bytes,
    std::uint64_t keep = cache_trim_bytes)
{
    std::erase_if(files, [](const cache_file& f) { return !is_cache_name(f.name); });
    std::uint64_t total = 0;
    for (const cache_file& f : files) total += f.size;
    std::vector<std::string> out;
    if (total <= budget) return out;
    std::sort(files.begin(), files.end(), [](const cache_file& a, const cache_file& b) { return a.mtime != b.mtime ? a.mtime < b.mtime : a.name < b.name; });
    for (const cache_file& f : files)
    {
        if (total <= keep) break;
        out.push_back(f.name);
        total -= f.size;
    }
    return out;
}
} // namespace utils::ps4::xmb_media
