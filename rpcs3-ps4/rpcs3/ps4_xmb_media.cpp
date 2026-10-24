#include "stdafx.h"
#include "ps4_xmb_media.h"
#include "ps4_launcher.h"
#include "Emu/Audio/PS4/PS4AudioBackend.h"
#include "Loader/ISO.h"
#include "Utilities/File.h"
#include "util/ps4_i18n.hpp"
#include "util/ps4_memory_governor.hpp"
#include "util/ps4_policy.hpp"

#include <zlib.h>

#ifdef _MSC_VER
#pragma warning(push, 0)
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#pragma GCC diagnostic ignored "-Wold-style-cast"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wcast-qual"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#endif
extern "C"
{
#include "libavcodec/avcodec.h"
#include "libavutil/channel_layout.h"
#include "libavutil/mem.h"
#include "libavutil/samplefmt.h"
#include "libswresample/swresample.h"
}
// stb_image is implemented in Emu/stb_image.cpp; the resizer and the JPEG writer are private to this file (static
// functions only, and the writer without its file functions: the cache is written through fs::pending_file).
#include <stb_image.h>
#include <cstdlib>
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_STATIC
// The resizer's allocator without its user-data argument: its default macros evaluate that argument, and the free of
// its own state reads it through the pointer that was just cleared (a null read)
#define STBIR_MALLOC(size, user_data) std::malloc(size)
#define STBIR_FREE(ptr, user_data) std::free(ptr)
#include <stb_image_resize2.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#define STBI_WRITE_NO_STDIO
#include <stb_image_write.h>
#ifdef _MSC_VER
#pragma warning(pop)
#else
#pragma GCC diagnostic pop
#endif

#include <algorithm>
#include <array>
#include <climits>
#include <cstring>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

LOG_CHANNEL(xmb_media_log, "XMB media");

namespace ps4_xmb_media
{
namespace
{
namespace governor = utils::ps4::governor;
using bytes_t = std::vector<std::uint8_t>;
using xmb_image = utils::ps4::xmb::image;

// Lines per XMB visit that also go to startup.log (all of them go to RPCS3.log)
constexpr std::uint32_t startup_lines = 16;
// Decoded music the output thread keeps after a conversion: one ATRAC3plus frame at 22.05 kHz is about 4,460 frames
constexpr std::uint32_t scratch_frames = 8192;
// Blocks in a row the decoder refuses before the track is given up
constexpr std::uint32_t decode_error_limit = 16;

std::uint64_t now_us() noexcept
{
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

double mib(std::uint64_t bytes) noexcept
{
    return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

void log_line(const std::string& text, bool startup)
{
    xmb_media_log.notice("%s", text);
    if (startup) ps4_startup_checkpoint(text.c_str());
}

enum class read_status : std::uint8_t { ok, too_large, error };

// A whole file of at most 'limit' bytes. A short read (I/O error, a USB drive pulled or waking up) only fails the read
// instead of ending in a fatal error; a file above the limit is not read at all.
read_status read_whole(const fs::file& source, std::uint64_t limit, bytes_t& out)
{
    out.clear();
    if (!source) return read_status::error;
    const std::uint64_t size = source.size();
    if (size > limit) return read_status::too_large;
    out.resize(static_cast<std::size_t>(size));
    if (size && source.read(out.data(), size) != size)
    {
        bytes_t().swap(out);
        return read_status::error;
    }
    return read_status::ok;
}

// The PS3 language of the next boot of 'serial': the game's or the global choice; the PS4-language choice runs the PS4
// language, the automatic one English (US)
std::int32_t media_language_of(const std::string& serial)
{
    const utils::ps4::game_language_choice choice = utils::ps4::read_game_language(serial);
    const std::int32_t automatic = choice.value == utils::ps4::game_language_system ? utils::ps4::i18n::game_language_auto() : 1;
    return xm::media_language(choice.value, automatic);
}

// ---------------------------------------------------------------------------------------------------------------------
// Images

bool image_info(const bytes_t& png, int& w, int& h, std::string& why)
{
    int channels = 0;
    w = h = 0;
    if (png.empty() || png.size() > static_cast<std::size_t>(INT_MAX) ||
        !stbi_info_from_memory(png.data(), static_cast<int>(png.size()), &w, &h, &channels))
    {
        why = "not a readable image";
        return false;
    }
    if (w <= 0 || h <= 0 || w > xm::image_side_limit || h > xm::image_side_limit)
    {
        why = fmt::format("%dx%d above %dx%d", w, h, xm::image_side_limit, xm::image_side_limit);
        return false;
    }
    return true;
}

// PIC1: the part of the image that covers the screen, scaled to exactly 1280x720 (opaque ARGB), and its JPEG for the cache
bool decode_background(const bytes_t& png, xmb_image& out, bytes_t* jpeg, int& w, int& h, std::string& why)
{
    if (!image_info(png, w, h, why)) return false;
    int channels = 0;
    stbi_uc* rgb = stbi_load_from_memory(png.data(), static_cast<int>(png.size()), &w, &h, &channels, 3);
    if (!rgb)
    {
        why = "decode failed";
        return false;
    }
    const xm::rect crop = xm::background_crop(w, h);
    bytes_t scaled(std::size_t(xm::screen_width) * xm::screen_height * 3);
    const unsigned char* first = rgb + (std::size_t(crop.y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(crop.x)) * 3;
    const bool resized = crop.w > 0 && crop.h > 0 &&
        stbir_resize_uint8_linear(first, crop.w, crop.h, w * 3, scaled.data(), xm::screen_width, xm::screen_height, xm::screen_width * 3, STBIR_RGB) != nullptr;
    // The full-size picture goes at once (up to 12 MiB for a 2048x2048 one)
    stbi_image_free(rgb);
    if (!resized)
    {
        why = "scaling failed";
        return false;
    }
    if (jpeg)
    {
        jpeg->clear();
        jpeg->reserve(512 * 1024);
        const int written = stbi_write_jpg_to_func([](void* context, void* data, int size)
        {
            auto* target = static_cast<bytes_t*>(context);
            const auto* begin = static_cast<const std::uint8_t*>(data);
            if (size > 0) target->insert(target->end(), begin, begin + size);
        }, jpeg, xm::screen_width, xm::screen_height, 3, scaled.data(), 92);
        if (!written || jpeg->size() > xm::record_blob_limit) bytes_t().swap(*jpeg);
    }
    out.width = xm::screen_width;
    out.height = xm::screen_height;
    out.pixels.resize(std::size_t(xm::screen_width) * xm::screen_height);
    xm::pack_rgb(scaled.data(), out.pixels.size(), out.pixels.data());
    return true;
}

// The cached background: a 1280x720 JPEG of our own cache
bool decode_cached_background(const bytes_t& jpeg, xmb_image& out)
{
    if (jpeg.empty() || jpeg.size() > static_cast<std::size_t>(INT_MAX)) return false;
    int w = 0, h = 0, channels = 0;
    // The record is read back from the disk: the size its JPEG header claims is checked before anything is allocated
    if (!stbi_info_from_memory(jpeg.data(), static_cast<int>(jpeg.size()), &w, &h, &channels) || w != xm::screen_width || h != xm::screen_height)
        return false;
    stbi_uc* rgb = stbi_load_from_memory(jpeg.data(), static_cast<int>(jpeg.size()), &w, &h, &channels, 3);
    if (!rgb) return false;
    const bool ok = w == xm::screen_width && h == xm::screen_height;
    if (ok)
    {
        out.width = w;
        out.height = h;
        out.pixels.resize(std::size_t(w) * static_cast<std::size_t>(h));
        xm::pack_rgb(rgb, out.pixels.size(), out.pixels.data());
    }
    stbi_image_free(rgb);
    return ok;
}

// PIC0 / PIC2: scaled for the 720-line screen (straight alpha kept), placed by xm::overlay_box; zlib for the cache
bool decode_overlay(const bytes_t& png, xmb_image& out, xm::rect& box, bytes_t* deflated, int& w, int& h, std::string& why)
{
    if (!image_info(png, w, h, why)) return false;
    int channels = 0;
    stbi_uc* rgba = stbi_load_from_memory(png.data(), static_cast<int>(png.size()), &w, &h, &channels, 4);
    if (!rgba)
    {
        why = "decode failed";
        return false;
    }
    box = xm::overlay_box(w, h);
    bytes_t scaled(std::size_t(box.w) * static_cast<std::size_t>(box.h) * 4);
    const bool resized = box.w > 0 && box.h > 0 &&
        stbir_resize_uint8_linear(rgba, w, h, w * 4, scaled.data(), box.w, box.h, box.w * 4, STBIR_RGBA) != nullptr;
    stbi_image_free(rgba);
    if (!resized)
    {
        why = "scaling failed";
        return false;
    }
    out.width = box.w;
    out.height = box.h;
    out.pixels.resize(std::size_t(box.w) * static_cast<std::size_t>(box.h));
    xm::pack_rgba(scaled.data(), out.pixels.size(), out.pixels.data());
    if (deflated)
    {
        const uLong raw = static_cast<uLong>(out.pixels.size() * sizeof(std::uint32_t));
        uLongf size = compressBound(raw);
        deflated->resize(size);
        if (compress2(deflated->data(), &size, reinterpret_cast<const Bytef*>(out.pixels.data()), raw, 1) == Z_OK && size <= xm::record_blob_limit)
            deflated->resize(size);
        else
            bytes_t().swap(*deflated);
    }
    return true;
}

bool inflate_overlay(const bytes_t& deflated, const xm::rect& box, xmb_image& out)
{
    if (deflated.empty() || box.w <= 0 || box.h <= 0) return false;
    std::vector<std::uint32_t> pixels(std::size_t(box.w) * static_cast<std::size_t>(box.h));
    uLongf size = static_cast<uLongf>(pixels.size() * sizeof(std::uint32_t));
    if (uncompress(reinterpret_cast<Bytef*>(pixels.data()), &size, deflated.data(), static_cast<uLong>(deflated.size())) != Z_OK ||
        size != pixels.size() * sizeof(std::uint32_t))
        return false;
    out.width = box.w;
    out.height = box.h;
    out.pixels = std::move(pixels);
    return true;
}
} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// Music: the SND0 bytes stay in memory (no file handle stays open, so a USB drive pulled while it plays does not stop
// it). Our RIFF parser has validated the header; the decoder only gets whole blocks of the data chunk, one per packet,
// so no demuxer reads the file. Decoded to stereo S16 at 48 kHz into a 0.5 s ring.

class music_track
{
public:
    music_track() = default;
    ~music_track()
    {
        if (m_ctx) avcodec_free_context(&m_ctx);
        if (m_packet) av_packet_free(&m_packet);
        if (m_frame) av_frame_free(&m_frame);
        if (m_swr) swr_free(&m_swr);
    }
    music_track(const music_track&) = delete;
    music_track& operator=(const music_track&) = delete;

    // Loader thread: the decoder and the first 150 ms decoded. Null and 'why' when it cannot play.
    static std::unique_ptr<music_track> open(bytes_t&& file, const xm::riff_info& info, const std::string& serial, std::string& why)
    {
        auto track = std::make_unique<music_track>();
        track->m_file = std::move(file);
        track->m_info = info;
        track->m_serial = serial;
        if (!track->start(why)) return nullptr;
        return track;
    }

    // Output thread (through the mixer): at most one block is decoded per call, and only while less than 100 ms waits
    std::uint32_t pull(std::int16_t* stereo, std::uint32_t frames) noexcept
    {
        if (m_ring.size() < xm::low_water_frames && !m_failed) decode_packet();
        const std::uint32_t n = m_ring.pop(stereo, frames);
        m_played += n;
        if (n < frames && !ended()) ++m_underruns;
        return n;
    }
    bool ended() const noexcept { return m_failed && !m_ring.size(); }

    std::uint64_t bytes() const noexcept
    {
        return m_file.capacity() + std::uint64_t(m_ring.capacity()) * 4 + m_scratch.capacity() * sizeof(std::int16_t);
    }
    const std::string& serial() const noexcept { return m_serial; }
    double played_seconds() const noexcept { return static_cast<double>(m_played) / xm::output_rate; }
    std::uint64_t underruns() const noexcept { return m_underruns; }
    std::uint32_t loops() const noexcept { return m_loops; }

private:
    bool start(std::string& why)
    {
        m_packet_bytes = xm::packet_bytes(m_info);
        m_packets = xm::packet_count(m_info);
        m_packet_samples = xm::packet_samples(m_info.codec);
        m_end = m_info.loop ? m_info.loop_end : m_info.total_samples;
        if (!m_packet_bytes || !m_packets || !m_packet_samples || !m_end || m_info.data_offset > m_file.size() ||
            m_info.data_size > m_file.size() - m_info.data_offset || m_info.extra_offset + m_info.extra_size > m_file.size())
        {
            why = "no playable blocks";
            return false;
        }
        AVSampleFormat in_format = AV_SAMPLE_FMT_S16;
        int in_rate = static_cast<int>(m_info.sample_rate);
        int channels = m_info.channels;
        if (m_info.codec != xm::riff_codec::pcm16)
        {
            const AVCodec* codec = avcodec_find_decoder(m_info.codec == xm::riff_codec::atrac3 ? AV_CODEC_ID_ATRAC3 : AV_CODEC_ID_ATRAC3P);
            if (!codec)
            {
                why = "no decoder for it in this build";
                return false;
            }
            m_ctx = avcodec_alloc_context3(codec);
            m_packet = av_packet_alloc();
            m_frame = av_frame_alloc();
            if (!m_ctx || !m_packet || !m_frame)
            {
                why = "out of memory";
                return false;
            }
            m_ctx->sample_rate = in_rate;
            av_channel_layout_default(&m_ctx->ch_layout, channels);
            m_ctx->block_align = m_info.block_align;
            m_ctx->bit_rate = static_cast<std::int64_t>(m_info.byte_rate) * 8;
            // No hidden decoder threads
            m_ctx->thread_count = 1;
            if (m_info.extra_size)
            {
                // The codec bytes after the format fields, as a WAV header hands them to the decoder (ATRAC3: 14)
                m_ctx->extradata = static_cast<std::uint8_t*>(av_mallocz(static_cast<std::size_t>(m_info.extra_size) + AV_INPUT_BUFFER_PADDING_SIZE));
                if (!m_ctx->extradata)
                {
                    why = "out of memory";
                    return false;
                }
                std::memcpy(m_ctx->extradata, m_file.data() + m_info.extra_offset, static_cast<std::size_t>(m_info.extra_size));
                m_ctx->extradata_size = static_cast<int>(m_info.extra_size);
            }
            if (const int error = avcodec_open2(m_ctx, codec, nullptr); error < 0)
            {
                why = fmt::format("the decoder refused its header (error %d)", error);
                return false;
            }
            in_format = m_ctx->sample_fmt;
            in_rate = m_ctx->sample_rate;
            channels = m_ctx->ch_layout.nb_channels;
            if (channels < 1 || channels > 2 || in_rate <= 0)
            {
                why = "unexpected decoder output";
                return false;
            }
        }
        AVChannelLayout in_layout{}, out_layout{};
        av_channel_layout_default(&in_layout, channels);
        av_channel_layout_default(&out_layout, 2);
        const int error = swr_alloc_set_opts2(&m_swr, &out_layout, AV_SAMPLE_FMT_S16, static_cast<int>(xm::output_rate), &in_layout, in_format, in_rate, 0, nullptr);
        av_channel_layout_uninit(&in_layout);
        av_channel_layout_uninit(&out_layout);
        if (error < 0 || !m_swr || swr_init(m_swr) < 0)
        {
            why = "resampler not available";
            return false;
        }
        m_in_format = in_format;
        m_channels = channels;
        m_scratch.resize(std::size_t(scratch_frames) * 2);
        for (int i = 0; i < 64 && m_ring.size() < xm::prefill_frames && !m_failed; ++i) decode_packet();
        if (!m_ring.size())
        {
            why = "no sound decoded";
            return false;
        }
        return true;
    }

    void decode_packet() noexcept
    {
        if (m_failed) return;
        if (m_next >= m_packets || m_pos >= m_end) restart();
        if (m_failed || m_next >= m_packets) return;
        const std::uint64_t at = m_info.data_offset + m_next * m_packet_bytes;
        const std::uint64_t size = std::min(m_packet_bytes, m_info.data_size - m_next * m_packet_bytes);
        ++m_next;
        if (!m_ctx)
        {
            // PCM16: whole frames straight from the file
            const std::uint8_t* planes[2] = {m_file.data() + at, nullptr};
            convert(planes, static_cast<int>(size / m_info.block_align));
        }
        else
        {
            // Not reference-counted: the decoder copies the block into its own padded buffer
            m_packet->data = m_file.data() + at;
            m_packet->size = static_cast<int>(size);
            int error = avcodec_send_packet(m_ctx, m_packet);
            m_packet->data = nullptr;
            m_packet->size = 0;
            if (error >= 0)
            {
                while ((error = avcodec_receive_frame(m_ctx, m_frame)) == 0)
                {
                    m_errors = 0;
                    convert(m_frame->extended_data, m_frame->nb_samples);
                    av_frame_unref(m_frame);
                }
                if (error == AVERROR(EAGAIN) || error == AVERROR_EOF) error = 0;
            }
            if (error < 0 && ++m_errors > decode_error_limit) m_failed = true;
        }
        if (m_pos >= m_end && !m_failed) restart();
    }

    // 'samples' decoded samples (per channel) at the current position: the part inside [loop start, end) is resampled
    void convert(const std::uint8_t* const* planes, int samples) noexcept
    {
        if (samples <= 0 || !planes || !planes[0]) return;
        const std::uint64_t first = m_pos;
        m_pos += static_cast<std::uint64_t>(samples);
        const std::uint64_t from = std::max(first, m_skip), to = std::min(m_pos, m_end);
        if (to <= from) return;
        const std::size_t offset = static_cast<std::size_t>(from - first);
        const int count = static_cast<int>(to - from);
        const std::size_t bytes_per_sample = static_cast<std::size_t>(av_get_bytes_per_sample(m_in_format));
        const std::uint8_t* in[2] = {};
        if (av_sample_fmt_is_planar(m_in_format))
        {
            for (int ch = 0; ch < m_channels; ++ch)
            {
                if (!planes[ch]) return;
                in[ch] = planes[ch] + offset * bytes_per_sample;
            }
        }
        else
        {
            in[0] = planes[0] + offset * bytes_per_sample * static_cast<std::size_t>(m_channels);
        }
        std::uint8_t* out[1] = {reinterpret_cast<std::uint8_t*>(m_scratch.data())};
        const int got = swr_convert(m_swr, out, static_cast<int>(scratch_frames), in, count);
        if (got < 0)
        {
            m_failed = true;
            return;
        }
        std::int16_t* frames = m_scratch.data();
        // After a restart from the beginning: a short ramp up (the end was ramped down)
        for (int i = 0; i < got && m_fade_in; ++i, --m_fade_in)
        {
            const std::int32_t gain = static_cast<std::int32_t>(std::int64_t(xm::gain_unity) * (xm::seam_frames - m_fade_in) / xm::seam_frames);
            frames[i * 2] = xm::scale_sample(frames[i * 2], gain);
            frames[i * 2 + 1] = xm::scale_sample(frames[i * 2 + 1], gain);
        }
        m_ring.push(frames, static_cast<std::uint32_t>(got));
    }

    // The end of the loop or of the file
    void restart() noexcept
    {
        ++m_loops;
        if (m_ctx) avcodec_flush_buffers(m_ctx);
        if (m_info.loop)
        {
            // Sample-accurate: decode from one block before the loop start (it rebuilds the decoder's overlap) and drop
            // what comes before the start. The resampler keeps its state, so the start follows the end seamlessly.
            const std::uint64_t block = m_info.loop_start / m_packet_samples;
            const std::uint64_t pre = block ? block - 1 : 0;
            m_next = pre;
            m_pos = pre * m_packet_samples;
            m_skip = m_info.loop_start;
        }
        else
        {
            // From the beginning, with a 20 ms ramp down and up at the seam (no click)
            m_ring.fade_tail(xm::seam_frames);
            swr_close(m_swr);
            if (swr_init(m_swr) < 0) m_failed = true;
            m_next = 0;
            m_pos = 0;
            m_skip = 0;
            m_fade_in = xm::seam_frames;
        }
    }

    bytes_t m_file;
    xm::riff_info m_info{};
    std::string m_serial;
    AVCodecContext* m_ctx = nullptr;
    AVPacket* m_packet = nullptr;
    AVFrame* m_frame = nullptr;
    SwrContext* m_swr = nullptr;
    AVSampleFormat m_in_format = AV_SAMPLE_FMT_S16;
    int m_channels = 2;
    xm::pcm_ring m_ring{xm::ring_frames};
    std::vector<std::int16_t> m_scratch;
    std::uint64_t m_packet_bytes = 0, m_packets = 0, m_packet_samples = 0;
    std::uint64_t m_next = 0; // next block
    std::uint64_t m_pos = 0;  // position (samples per channel) of the next decoded sample
    std::uint64_t m_skip = 0; // samples before this are dropped (a loop start)
    std::uint64_t m_end = 0;  // the loop end or the last sample to play
    std::uint32_t m_fade_in = 0;
    std::uint32_t m_errors = 0;
    bool m_failed = false;
    std::uint64_t m_played = 0, m_underruns = 0;
    std::uint32_t m_loops = 0;
};

// ---------------------------------------------------------------------------------------------------------------------
// Payload

media_payload::media_payload() = default;
media_payload::~media_payload() = default;

std::uint64_t media_payload::image_bytes() const noexcept
{
    return (background.pixels.capacity() + overlay.pixels.capacity()) * sizeof(std::uint32_t);
}

std::uint64_t media_payload::music_bytes() const noexcept
{
    return track ? track->bytes() : 0;
}

// ---------------------------------------------------------------------------------------------------------------------
// Settings

settings read_settings()
{
    settings s = xm::read_settings();
    // The overlay follows the aspect ratio of the next boot: the global settings, else config.yml
    std::optional<bool> four_three;
    for (const std::string& path : {std::string(utils::ps4::global_settings_path), fs::get_config_dir(true) + "config.yml"})
    {
        bytes_t text;
        if (read_whole(fs::file(path), 1 << 20, text) != read_status::ok) continue;
        four_three = xm::yaml_aspect_is_4_3({reinterpret_cast<const char*>(text.data()), text.size()});
        if (four_three) break;
    }
    s.wide = !four_three.value_or(false);
    return s;
}

bool set_media_switch(bool on)
{
    if (!xm::write_media_switch(on))
    {
        log_line(fmt::format("settings: XMB media switch not saved: '%s' could not be %s", xm::media_off_flag, on ? "removed" : "created"), true);
        return false;
    }
    log_line(on ? "settings: XMB media switched ON (xmb-media-off.flag removed)" : "settings: XMB media switched OFF (xmb-media-off.flag created)", true);
    return true;
}

bool set_music_level(music_level level)
{
    if (!xm::write_music_level(level))
    {
        log_line(fmt::format("settings: XMB music level not saved: '%s' could not be written", xm::music_level_file), true);
        return false;
    }
    log_line(level == music_level::on ? std::string("settings: XMB music set to on (xmb-music.txt removed)")
                                      : fmt::format("settings: XMB music set to %s (xmb-music.txt)", xm::music_level_token(level)), true);
    return true;
}

void log_open(const settings& s, bool inline_loader)
{
    if (!s.media)
    {
        log_line("XMB media: off (xmb-media-off.flag)", true);
        return;
    }
    log_line(fmt::format("XMB media: on (background + music %s; overlay %s)%s", xm::music_level_token(s.music), s.wide ? "PIC0 16:9" : "PIC2 4:3",
        inline_loader ? "; no loader thread: cached backgrounds only, no music" : ""), true);
}

// ---------------------------------------------------------------------------------------------------------------------
// Media job (loader thread)

namespace
{
// Where a job reads from: the folder of a folder/HDD game, or the ISO image (opened once per job, never mounted)
struct media_source
{
    xm::source_kind kind = xm::source_kind::none;
    std::string path, base;
    std::unique_ptr<iso_archive> iso;
    bool iso_failed = false;

    bool open_iso()
    {
        if (kind != xm::source_kind::iso || iso) return true;
        if (iso_failed) return false;
        iso = std::make_unique<iso_archive>(path);
        if (!iso->is_valid())
        {
            iso.reset();
            iso_failed = true;
            return false;
        }
        return true;
    }

    std::string inner(const std::string& name) const
    {
        return kind == xm::source_kind::iso ? std::string(xm::iso_folder) + name : base + name;
    }

    // ISO only: the file exists inside the image
    bool iso_has(const std::string& name)
    {
        if (!open_iso()) return false;
        const iso_fs_node* node = iso->retrieve(inner(name));
        return node && !node->metadata.is_directory;
    }

    read_status read(const std::string& name, std::uint64_t limit, bytes_t& out)
    {
        if (kind == xm::source_kind::iso)
        {
            if (!open_iso()) return read_status::error;
            return read_whole(fs::file(iso->open(inner(name))), limit, out);
        }
        return read_whole(fs::file(inner(name)), limit, out);
    }
};

// The stamp of one candidate file of a folder game; false on an error other than "not there" (nothing is recorded then)
bool stamp_file(const std::string& path, xm::source_stamp& stamp)
{
    stamp = {};
    fs::stat_t info{};
    if (fs::get_stat(path, info))
    {
        if (info.is_directory) return true;
        stamp.present = true;
        stamp.size = info.size;
        stamp.mtime = info.mtime;
        return true;
    }
    return fs::g_tls_error == fs::error::noent || fs::g_tls_error == fs::error::notdir;
}

std::optional<xm::media_record> read_record(const std::string& file, const xm::record_key& key)
{
    bytes_t bytes;
    if (read_whole(fs::file(file), xm::record_file_limit, bytes) != read_status::ok) return std::nullopt;
    return xm::decode_record(bytes, key);
}

// Atomic: a temporary file renamed over the old record, so a crash or a power loss leaves the old record or none
bool write_record(const std::string& file, const xm::media_record& record)
{
    const bytes_t bytes = xm::encode_record(record);
    fs::pending_file pending(file);
    // The folder may have been removed while the XMB is open (an FTP client, a cache clean-up): made again once
    if (!pending.file && fs::create_path(xm::cache_dir)) pending.open(file);
    if (!pending.file) return false;
    if (!bytes.empty() && pending.file.write(bytes.data(), bytes.size()) != bytes.size()) return false;
    return pending.commit();
}

bool cache_folder_ready(job_context& context)
{
    if (!std::exchange(context.cache_tried, true))
    {
        context.cache_ready = fs::create_path(xm::cache_dir);
        if (!context.cache_ready) xmb_media_log.error("XMB media cache folder '%s' is not available: %s", xm::cache_dir, fs::g_tls_error);
    }
    return context.cache_ready;
}

// What a job found, decoded or refused, joined into its one line
struct job_line
{
    std::string text;
    void add(const std::string& part)
    {
        if (!text.empty()) text += "; ";
        text += part;
    }
};
} // namespace

std::shared_ptr<media_payload> media_job(const media_request& request, job_context& context)
{
    const std::uint64_t start = now_us();
    const auto stale = [&] { return request.state && request.state->wanted.load(std::memory_order_relaxed) != request.generation; };
    if (stale()) return nullptr;

    auto payload = std::make_shared<media_payload>();
    payload->generation = request.generation;
    payload->path = request.path;
    payload->serial = request.serial;
    payload->images = request.want_images;

    job_line line;
    const auto finish = [&]() -> std::shared_ptr<media_payload>
    {
        payload->cost_us = now_us() - start;
        if (line.text.empty()) line.add("no PIC1, PIC0/PIC2 or SND0");
        const std::uint32_t number = request.state ? request.state->lines.fetch_add(1, std::memory_order_relaxed) : 0;
        log_line(fmt::format("XMB media: %s %s (%.1f ms)", request.serial.empty() ? request.path : request.serial, line.text,
            static_cast<double>(payload->cost_us) / 1000), number < startup_lines);
        return payload;
    };

    media_source source;
    source.kind = xm::source_of(request.path);
    source.path = request.path;
    if (source.kind == xm::source_kind::none) return finish();

    const std::int32_t language = media_language_of(request.serial);
    const std::array<std::vector<std::string>, 3> names{
        xm::candidate_names(xm::media_kind::background, language, request.wide),
        xm::candidate_names(xm::media_kind::overlay, language, request.wide),
        xm::candidate_names(xm::media_kind::music, language, request.wide)};
    const std::array<std::size_t, 3> first_stamp{0, names[0].size(), names[0].size() + names[1].size()};

    // The record key: the ISO's own stamp, or every candidate file of a folder game (present or not)
    bool read_error = false; // a file could not be read now: nothing of this job is recorded
    xm::record_key key;
    key.path = request.path;
    key.language = language;
    key.wide = request.wide;
    if (source.kind == xm::source_kind::iso)
    {
        fs::stat_t info{};
        if (!fs::get_stat(request.path, info) || info.is_directory)
        {
            line.add("image not readable now: not cached");
            return finish();
        }
        key.stamps.push_back({true, info.size, info.mtime});
    }
    else
    {
        const bool is_dir = fs::is_dir(request.path);
        source.base = xm::files_base(request.path, is_dir, is_dir && fs::is_file(request.path + "/PS3_GAME/PARAM.SFO"));
        for (const auto& list : names)
            for (const std::string& name : list)
            {
                xm::source_stamp stamp;
                if (!stamp_file(source.base + name, stamp)) read_error = true;
                key.stamps.push_back(stamp);
            }
        if (read_error) line.add("a media file could not be checked (I/O error): not cached");
    }
    // Which candidate of a kind the game has (folder games: the stamps; ISO: looked up inside the image)
    const auto present = [&](std::size_t kind) -> std::optional<std::size_t>
    {
        for (std::size_t i = 0; i < names[kind].size(); ++i)
        {
            if (source.kind == xm::source_kind::iso ? source.iso_has(names[kind][i]) : key.stamps[first_stamp[kind] + i].present) return i;
        }
        return std::nullopt;
    };

    const std::string record_file = std::string(xm::cache_dir) + "/" + xm::record_name(key);
    std::optional<xm::media_record> record; // from the cache, or what this job found
    bool write = false;                     // 'record' was made now (or changed) and may be recorded
    bool cached = false;                    // 'record' came from the cache
    if (!read_error) record = read_record(record_file, key);
    cached = record.has_value();
    // The record of what was decoded ("no media" too), written atomically, only into the cache folder
    const auto store = [&]
    {
        if (!write || !record) return;
        write = false;
        if (write_record(record_file, *record))
            line.add(fmt::format("cache written %.0f KB", static_cast<double>(record->jpeg.size() + record->overlay_zlib.size()) / 1024));
        else
            line.add("cache not written");
    };
    // The selection moved on: what was decoded is still recorded, the result is dropped
    const auto abandon = [&]() -> std::shared_ptr<media_payload>
    {
        store();
        line.add("abandoned (the selection moved)");
        static_cast<void>(finish());
        return nullptr;
    };

    if (record && request.want_images)
    {
        // A record whose images do not decode any more counts as no record
        const bool background_ok = !record->background || decode_cached_background(record->jpeg, payload->background);
        const bool overlay_ok = !record->overlay || inflate_overlay(record->overlay_zlib, record->overlay_box_px, payload->overlay);
        if (background_ok && overlay_ok)
        {
            payload->overlay_x = record->overlay_box_px.x;
            payload->overlay_y = record->overlay_box_px.y;
            payload->from_cache = true;
            line.add(fmt::format("cache hit (%s%s%s)", record->background ? "PIC1" : "no PIC1", record->overlay ? (request.wide ? ", PIC0" : ", PIC2") : "",
                record->music ? ", SND0" : ""));
        }
        else
        {
            payload->background = {};
            payload->overlay = {};
            record.reset();
            cached = false;
        }
    }

    if (!record && request.want_images)
    {
        if (request.cache_only)
        {
            line.add("not cached yet (no loader thread: cached backgrounds only)");
            return finish();
        }
        if (const int pressure = governor::heap_pressure(); pressure >= 1)
        {
            line.add(fmt::format("memory level %d: not decoded now", pressure));
            return finish();
        }
        if (!source.open_iso())
        {
            line.add("image not readable now: not cached");
            return finish();
        }

        xm::media_record fresh;
        fresh.key = key;
        const bool cache = !read_error && cache_folder_ready(context);
        // PIC1
        if (const auto which = present(0))
        {
            bytes_t file;
            const std::string& name = names[0][*which];
            switch (source.read(name, xm::background_file_limit, file))
            {
            case read_status::too_large:
                line.add(fmt::format("%s refused: file above %u MiB", name, static_cast<unsigned>(xm::background_file_limit >> 20)));
                break;
            case read_status::error:
                read_error = true;
                line.add(fmt::format("%s read error: not cached", name));
                break;
            case read_status::ok:
            {
                const std::uint64_t t0 = now_us();
                int w = 0, h = 0;
                std::string why;
                if (decode_background(file, payload->background, cache ? &fresh.jpeg : nullptr, w, h, why))
                {
                    fresh.background = !fresh.jpeg.empty();
                    fresh.background_w = static_cast<std::uint16_t>(w);
                    fresh.background_h = static_cast<std::uint16_t>(h);
                    line.add(fmt::format("%s %dx%d decoded in %.0f ms", name, w, h, static_cast<double>(now_us() - t0) / 1000));
                }
                else line.add(fmt::format("%s refused: %s", name, why));
                break;
            }
            }
        }
        if (stale()) return abandon();

        // PIC0 (16:9) or PIC2 (4:3)
        if (const auto which = present(1))
        {
            bytes_t file;
            const std::string& name = names[1][*which];
            switch (source.read(name, xm::overlay_file_limit, file))
            {
            case read_status::too_large:
                line.add(fmt::format("%s refused: file above %u MiB", name, static_cast<unsigned>(xm::overlay_file_limit >> 20)));
                break;
            case read_status::error:
                read_error = true;
                line.add(fmt::format("%s read error: not cached", name));
                break;
            case read_status::ok:
            {
                int w = 0, h = 0;
                std::string why;
                xm::rect box{};
                if (decode_overlay(file, payload->overlay, box, cache ? &fresh.overlay_zlib : nullptr, w, h, why))
                {
                    payload->overlay_x = box.x;
                    payload->overlay_y = box.y;
                    fresh.overlay = !fresh.overlay_zlib.empty();
                    fresh.overlay_box_px = box;
                    fresh.overlay_w = static_cast<std::uint16_t>(w);
                    fresh.overlay_h = static_cast<std::uint16_t>(h);
                    line.add(fmt::format("%s %dx%d", name, w, h));
                }
                else line.add(fmt::format("%s refused: %s", name, why));
                break;
            }
            }
        }

        // SND0: only whether it is there here; its header is checked below when the music is wanted
        if (const auto which = present(2))
        {
            fresh.music = true;
            fresh.music_name = static_cast<std::uint8_t>(*which);
        }

        // Recorded only when every file could be read, every image shown is also in the record (a JPEG or zlib failure
        // must not record "no image"), and the image's content is readable: an encrypted ISO without its key gives
        // garbage until its .dkey is imported, so nothing is recorded for it
        write = cache && !read_error && payload->has_background() == fresh.background && payload->has_overlay() == fresh.overlay;
        if (write && source.kind == xm::source_kind::iso && source.iso->get_key_status() != iso_key_status::OK)
        {
            write = false;
            line.add("ISO key not ready: not cached");
        }
        record = std::move(fresh);
    }
    else if (!record)
    {
        // A music-only job without a record: look the SND0 up (nothing is recorded, the images were not decoded)
        if (const auto which = present(2))
        {
            xm::media_record found;
            found.music = true;
            found.music_name = static_cast<std::uint8_t>(*which);
            record = std::move(found);
        }
    }
    payload->music_file = record && record->music;
    if (stale()) return abandon();

    // The music, when it is wanted and the game has an SND0 that was not refused before
    if (request.want_music && !request.cache_only && record && record->music && !record->music_unsupported)
    {
        // An SND0 refused now is recorded as such (a cached record is written again), so it is not read at every visit
        const auto refused = [&]
        {
            record->music_unsupported = true;
            if (cached) write = !read_error && cache_folder_ready(context);
        };
        const std::string& name = names[2][std::min<std::size_t>(record->music_name, names[2].size() - 1)];
        bytes_t file;
        if (const int pressure = governor::heap_pressure(); pressure >= 1)
        {
            line.add(fmt::format("%s not prepared (memory level %d)", name, pressure));
        }
        else switch (source.read(name, xm::music_file_limit, file))
        {
        case read_status::too_large:
            refused();
            line.add(fmt::format("%s refused: file above %u MiB", name, static_cast<unsigned>(xm::music_file_limit >> 20)));
            break;
        case read_status::error:
            // Only the music is missing now: the record (which never holds the SND0 itself) stays right
            line.add(fmt::format("%s read error", name));
            break;
        case read_status::ok:
        {
            const xm::riff_info info = xm::parse_riff(file);
            if (!info.ok)
            {
                refused();
                line.add(info.format_tag && !std::strcmp(info.error, "unsupported format tag")
                    ? fmt::format("%s: RIFF tag 0x%04x unsupported", name, info.format_tag)
                    : fmt::format("%s refused: %s", name, info.error));
                break;
            }
            record->codec = info.codec;
            record->music_rate = info.sample_rate;
            record->music_channels = info.channels;
            const std::uint64_t size = file.size();
            if (stale()) return abandon();
            std::string why;
            payload->track = music_track::open(std::move(file), info, request.serial, why);
            if (!payload->track)
            {
                line.add(fmt::format("%s %s not playable: %s", name, xm::codec_name(info.codec), why));
                break;
            }
            line.add(fmt::format("%s %s %u Hz %u ch %.1f MB%s", name, xm::codec_name(info.codec), info.sample_rate, info.channels,
                static_cast<double>(size) / 1e6, info.loop ? fmt::format(" loop smpl %llu..%llu", static_cast<unsigned long long>(info.loop_start),
                    static_cast<unsigned long long>(info.loop_end)) : std::string(", loops at its end")));
            break;
        }
        }
    }
    else if (record && record->music && record->music_unsupported && request.want_music)
    {
        line.add("SND0 refused before (not tried again until it changes)");
    }
    if (stale()) return abandon();
    store();
    return finish();
}

void media_trim(job_context& context)
{
    static std::atomic<bool> s_trimmed{false};
    if (s_trimmed.exchange(true) || !cache_folder_ready(context)) return;
    const std::uint64_t start = now_us();
    std::vector<xm::cache_file> files;
    std::uint64_t total = 0;
    {
        fs::dir folder(xm::cache_dir);
        if (!folder) return;
        fs::dir_entry entry;
        while (folder.read(entry))
        {
            if (entry.is_directory || !xm::is_cache_name(entry.name)) continue;
            files.push_back({entry.name, entry.size, entry.mtime});
            total += entry.size;
        }
    }
    std::uint64_t removed_bytes = 0;
    std::uint32_t removed = 0;
    for (const std::string& name : xm::trim_selection(files))
    {
        const auto it = std::find_if(files.begin(), files.end(), [&](const xm::cache_file& f) { return f.name == name; });
        if (fs::remove_file(std::string(xm::cache_dir) + "/" + name))
        {
            ++removed;
            removed_bytes += it != files.end() ? it->size : 0;
        }
    }
    log_line(fmt::format("XMB media: cache %u file(s), %.1f MiB%s (%.1f ms)", static_cast<unsigned>(files.size()), mib(total),
        removed ? fmt::format("; the %u oldest removed (%.1f MiB) to stay under %u MiB", removed, mib(removed_bytes),
            static_cast<unsigned>(xm::cache_budget_bytes >> 20)) : std::string(), static_cast<double>(now_us() - start) / 1000), true);
}

// ---------------------------------------------------------------------------------------------------------------------
// Music output

struct xmb_music::state
{
    timing t;
    // The mixer: the UI thread moves tracks in and out under it, the output thread renders under try_lock only (it
    // writes silence for a block rather than wait)
    std::mutex mutex;
    xm::basic_mixer<music_track> mixer;
    // The port and its output thread; reset() = Close() (parked within about two blocks, port closed) + join
    std::unique_ptr<PS4AudioBackend> backend;
    bool refused = false;
    music_level level = music_level::on;
    std::uint64_t silent_since = 0;
    std::uint32_t lines = 0;
};

namespace
{
// Output thread, once per 256-frame block: always the whole block, silence when nothing plays, so the backend's
// underrun concealment never acts on intended silence. No allocation and no wait here.
std::uint32_t render_block(xmb_music::state& s, std::uint32_t bytes, void* buffer) noexcept
{
    const std::uint32_t frames = bytes / 4;
    std::unique_lock lock(s.mutex, std::try_to_lock);
    if (lock.owns_lock()) s.mixer.render(static_cast<std::int16_t*>(buffer), frames);
    else std::memset(buffer, 0, std::size_t(frames) * 4);
    return frames * 4;
}

void track_line(xmb_music::state& s, const music_track& track)
{
    log_line(fmt::format("XMB music: %s played %.1f s, %llu underrun(s), %u loop(s)", track.serial().empty() ? std::string("game") : track.serial(),
        track.played_seconds(), static_cast<unsigned long long>(track.underruns()), track.loops()), s.lines++ < startup_lines);
}

bool open_port(xmb_music::state& s)
{
    if (s.backend) return true;
    if (s.refused) return false;
    std::unique_ptr<PS4AudioBackend> backend;
    const auto refuse = [&](const char* why)
    {
        s.refused = true;
        log_line(fmt::format("XMB music: not started (%s): background only until the XMB opens again", why), true);
        return false;
    };
    // The optional-thread rule before the port is opened: its output thread is the one thread the music adds, and the
    // last slots of the console's thread cap stay for the game
    if (governor::thread_pressure() != 0 || !governor::optional_thread_allowed("PS4 XMB Music", 1)) return refuse("thread budget");
    backend = std::make_unique<PS4AudioBackend>();
    xmb_music::state* self = &s;
    backend->SetWriteCallback([self](u32 bytes, void* buffer) -> u32 { return render_block(*self, bytes, buffer); });
    if (!backend->Open("", AudioFreq::FREQ_48K, AudioSampleSize::S16, AudioChannelCnt::STEREO, audio_channel_layout::stereo)) return refuse("port open failed");
    if (!backend->Operational()) return refuse("output thread refused");
    backend->Play();
    if (!backend->IsPlaying()) return refuse("output thread refused");
    s.backend = std::move(backend);
    log_line("XMB music: AudioOut opened (S16 stereo 48 kHz, 256 frames)", true);
    return true;
}

// The port closed and its thread joined (after a short ramp while it plays), then every track freed
std::uint64_t release_all(xmb_music::state& s, bool lines)
{
    std::uint64_t bytes = 0;
    if (s.backend)
    {
        std::unique_ptr<music_track> dropped;
        {
            std::lock_guard lock(s.mutex);
            dropped = s.mixer.stop(xm::frames_for(s.t.music_boot_us));
        }
        if (dropped) bytes += dropped->bytes();
        dropped.reset();
        // The ramp plays out on the output thread; 40 ms at most
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(40);
        for (;;)
        {
            {
                std::lock_guard lock(s.mutex);
                if (s.mixer.silent()) break;
            }
            if (std::chrono::steady_clock::now() >= until) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        s.backend.reset();
    }
    std::array<std::unique_ptr<music_track>, xm::basic_mixer<music_track>::retired_slots + 2> all;
    {
        std::lock_guard lock(s.mutex);
        all = s.mixer.take_all();
    }
    for (auto& track : all)
    {
        if (!track) continue;
        bytes += track->bytes();
        if (lines) track_line(s, *track);
        track.reset();
    }
    s.silent_since = 0;
    return bytes;
}
} // namespace

xmb_music::xmb_music(const timing& t) : m(std::make_unique<state>())
{
    m->t = t;
}

xmb_music::~xmb_music()
{
    if (m) static_cast<void>(release_all(*m, false));
}

bool xmb_music::play(media_payload& payload, music_level level)
{
    std::unique_ptr<music_track> track = std::move(payload.track);
    if (!track || level == music_level::off || !open_port(*m)) return false;
    std::unique_ptr<music_track> replaced;
    {
        std::lock_guard lock(m->mutex);
        const std::int32_t gain = xm::level_gain(level);
        if (m->mixer.level_target() != gain) m->mixer.set_level(gain, m->mixer.silent() ? 0 : xm::frames_for(m->t.music_level_us));
        replaced = m->mixer.play(std::move(track), xm::frames_for(m->t.music_in_us), xm::frames_for(m->t.music_out_us));
    }
    m->level = level;
    m->silent_since = 0;
    // 'replaced' (a track that never started) is freed here, outside the mixer's lock
    return true;
}

void xmb_music::fade_out(std::uint64_t us)
{
    // Declared before the lock, so the dropped track is freed after the lock is released
    std::unique_ptr<music_track> dropped;
    std::lock_guard lock(m->mutex);
    dropped = m->mixer.stop(xm::frames_for(us));
}

void xmb_music::set_level(music_level level)
{
    m->level = level;
    if (level == music_level::off)
    {
        fade_out(m->t.music_out_us);
        return;
    }
    std::lock_guard lock(m->mutex);
    m->mixer.set_level(xm::level_gain(level), xm::frames_for(m->t.music_level_us));
}

void xmb_music::tick(std::uint64_t now)
{
    std::array<std::unique_ptr<music_track>, xm::basic_mixer<music_track>::retired_slots> retired;
    bool silent = true;
    {
        std::lock_guard lock(m->mutex);
        retired = m->mixer.take_retired();
        silent = m->mixer.silent();
    }
    for (auto& track : retired)
    {
        if (!track) continue;
        track_line(*m, *track);
        track.reset();
    }
    if (!m->backend) return;
    if (!m->backend->Operational())
    {
        // The output failed (an output change, rest mode): nothing renders any more, so nothing would ever finish
        m->backend.reset();
        static_cast<void>(release_all(*m, true));
        log_line("XMB music: AudioOut output failed; port closed (the next track opens it again)", true);
        return;
    }
    if (!silent)
    {
        m->silent_since = 0;
        return;
    }
    if (!m->silent_since || now < m->silent_since)
    {
        m->silent_since = std::max<std::uint64_t>(now, 1);
        return;
    }
    if (now - m->silent_since < m->t.port_linger_us) return;
    // No open/close churn while browsing, and no port or thread left once the music has been silent for a while
    m->backend.reset();
    m->silent_since = 0;
    log_line(fmt::format("XMB music: port closed after %.0f s of silence", static_cast<double>(m->t.port_linger_us) / 1e6), true);
}

std::uint64_t xmb_music::shutdown(std::string_view why)
{
    const bool open = static_cast<bool>(m->backend);
    const std::uint64_t bytes = release_all(*m, true);
    if (open) log_line(fmt::format("XMB music: stopped before %s (AudioOut closed, output thread joined)", why), false);
    return bytes;
}

bool xmb_music::port_open() const noexcept
{
    return static_cast<bool>(m->backend);
}

bool xmb_music::refused() const noexcept
{
    return m->refused;
}

// ---------------------------------------------------------------------------------------------------------------------
// Release before a boot

void media_release::count(std::shared_ptr<media_payload>& payload)
{
    if (!payload) return;
    m_totals.image_bytes += payload->image_bytes();
    m_totals.music_bytes += payload->music_bytes();
    payload.reset();
}

void media_release::end(std::uint64_t scene_bytes)
{
    if (std::exchange(m_ended, true)) return;
    m_totals.image_bytes += scene_bytes;
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - m_start).count();
    log_line(fmt::format("XMB media: released before %s in %.1f ms: %s, %.1f MiB images and %.1f MiB music freed, %u result(s) dropped", m_why, ms,
        m_port_was_open ? "music stopped (AudioOut closed, output thread joined)" : "no music port was open", mib(m_totals.image_bytes),
        mib(m_totals.music_bytes), m_totals.results), true);
}

media_release::~media_release()
{
    end(0);
}
} // namespace ps4_xmb_media
