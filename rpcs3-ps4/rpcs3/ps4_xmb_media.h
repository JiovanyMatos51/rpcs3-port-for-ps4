#pragma once

// The selected game's own XMB media on the console: its PIC1/PIC0/PIC2 images and its SND0.AT3 music, read, decoded and
// played without any game running. Only the game's own files are used; nothing is shipped, generated or substituted (a
// game without PIC1 keeps the waves, a game without SND0 plays nothing). The pure rules (director, file names, RIFF
// parser, geometry, gains, cache record) are util/ps4_xmb_media.hpp; this is the part that reads files, decodes and
// owns the AudioOut port.
//
// Threads: media_job() runs on the XMB loader thread (or, without one, on the UI thread with request.cache_only set);
// every other function runs on the UI thread. The only thread this adds is the AudioOut output thread of the music's
// PS4AudioBackend, started only under the optional-thread rule and joined when the port closes.
//
// Use from the XMB loop, in this order:
//   at the XMB open:   s = read_settings(); log_open(s, inline_loader); d.configure(s, inline_loader);
//                      post one media_trim() job (once per process, it returns at once afterwards)
//   every frame:       out = d.tick(now_us, input);
//                      out.drop  -> remove the queued media jobs; out.post -> post media_job(*out.post, ...)
//                      out.present -> scene.set_media(std::move(p->background), std::move(p->overlay), p->overlay_x,
//                                     p->overlay_y) (before out.play: both can be the same payload)
//                      out.play -> music.play(*out.play, level); out.stop_music -> music.fade_out(out.stop_us)
//                      out.clear -> scene.clear_media(); scene.set_media_alpha(out.background_alpha, 0, out.overlay_alpha)
//                      out.animating -> keep at least 30 frames per second; music.tick(now_us)
//   a media result:    d.arrived(payload) false -> drop it
//   every exit:        media_release r(why, music, d, [&] { drop the queued media jobs; });  (first statement)
//                      ... join the loader ...; r.dropped(payload) for every media result still queued;
//                      r.end(scene.clear_media());  (writes the release line, before BootGame)

#include "util/ps4_xmb_media.hpp"
#include "util/ps4_xmb_resources.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace ps4_xmb_media
{
namespace xm = utils::ps4::xmb_media;
using xm::director_input;
using xm::media_request;
using xm::music_level;
using xm::settings;
using xm::timing;

// The decoded SND0 of one game, ready to play (defined in ps4_xmb_media.cpp)
class music_track;

// What one media job produced. Held by std::shared_ptr only (the loader result, the director, the music).
struct media_payload
{
    std::uint32_t generation = 0;
    std::string path, serial;
    bool images = true;                      // false: a music-only job (the background is shown already)
    utils::ps4::xmb::image background;       // exactly 1280x720, opaque; empty: the waves stay
    utils::ps4::xmb::image overlay;          // PIC0 (16:9) or PIC2 (4:3), straight alpha; empty: none
    int overlay_x = 0, overlay_y = 0;
    bool music_file = false;                 // the game has an SND0 (prepared or not)
    std::unique_ptr<music_track> track;      // its prepared music, taken by xmb_music::play()
    // For the loader statistics
    bool from_cache = false;
    std::uint64_t cost_us = 0;

    media_payload();
    ~media_payload();
    media_payload(const media_payload&) = delete;
    media_payload& operator=(const media_payload&) = delete;

    bool has_background() const noexcept { return static_cast<bool>(background); }
    bool has_overlay() const noexcept { return static_cast<bool>(overlay); }
    bool has_music() const noexcept { return static_cast<bool>(track); }
    bool has_music_file() const noexcept { return music_file; }
    bool music_only() const noexcept { return !images; }
    std::uint64_t image_bytes() const noexcept;
    std::uint64_t music_bytes() const noexcept;
};

using director = xm::basic_director<media_payload>;

// The two switch files and the overlay aspect of the next boot (the global settings, else config.yml; 16:9 by default)
settings read_settings();
// The settings rows: write the file, then one startup.log line. False: the file system refused, nothing changed.
bool set_media_switch(bool on);
bool set_music_level(music_level level);
// One startup.log line at the XMB open: what is on, and whether the jobs have a loader thread
void log_open(const settings& s, bool inline_loader);

// State of the media jobs of one XMB visit (loader thread, or the UI thread without one)
struct job_context
{
    bool cache_tried = false, cache_ready = false;
};

// Loader thread: the media of 'request.path' (cache record, files or ISO image, decode and scale, SND0 prepared).
// Returns null when the selection moved on while it ran (the director asks again). Never writes outside
// /data/rpcs3/cache/xmb/media; never records a read error or an ISO without its key as "no media".
std::shared_ptr<media_payload> media_job(const media_request& request, job_context& context);

// Loader thread, once per process (later calls return at once): the cache trimmed to 72 MiB when it holds more than
// 96 MiB, oldest records first, only the cache's own file names. One startup.log line.
void media_trim(job_context& context);

// The music of the selected game: one PS4AudioBackend of its own (the AudioOut init stays the backend's), opened at the
// first track, closed after port_linger_us of silence and at shutdown(). UI thread only.
class xmb_music
{
public:
    explicit xmb_music(const timing& t = {});
    ~xmb_music(); // closes the port and joins its thread if shutdown() was not called
    xmb_music(const xmb_music&) = delete;
    xmb_music& operator=(const xmb_music&) = delete;

    // Starts the payload's prepared track at 'level' (it fades in after the playing one has faded out). The port opens
    // here when it is closed: the optional-thread rule first, then the port and its output thread. A refusal is final
    // for this XMB visit (one line) and the track is freed. False: nothing plays.
    bool play(media_payload& payload, music_level level);
    // Fades the playing track out over 'us' and drops the waiting one
    void fade_out(std::uint64_t us);
    // On / low (200 ms ramp); off fades the music out
    void set_level(music_level level);
    // Every frame: frees finished tracks (one line each), closes the port after port_linger_us of silence
    void tick(std::uint64_t now_us);
    // Before a boot or an exit: a 30 ms ramp, then the port closed and its output thread joined, every track freed.
    // Returns the bytes freed.
    std::uint64_t shutdown(std::string_view why);

    bool port_open() const noexcept;
    bool refused() const noexcept;

    struct state;

private:
    std::unique_ptr<state> m;
};

// The release before a boot or an exit, in its fixed order: the music first (port closed, output thread joined), the
// queued media jobs, the director's payloads; then, after the loader is joined, the results nobody took; then the
// scene's images; then the line "XMB media: released before <why> in N ms: ...".
class media_release
{
public:
    template <typename DropJobs>
    media_release(std::string_view why, xmb_music& music, director& d, DropJobs&& drop_jobs)
        : m_why(why), m_start(std::chrono::steady_clock::now()), m_port_was_open(music.port_open())
    {
        m_totals.music_bytes += music.shutdown(why);
        drop_jobs();
        d.drain([this](std::shared_ptr<media_payload> payload) { count(payload); });
    }
    ~media_release(); // end(0) when end() was not called
    media_release(const media_release&) = delete;
    media_release& operator=(const media_release&) = delete;

    // A media result taken from the joined loader: counted, then freed with the last reference
    void dropped(std::shared_ptr<media_payload> payload)
    {
        if (payload) ++m_totals.results;
        count(payload);
    }
    // The bytes the scene freed (scene.clear_media()); writes the line once
    void end(std::uint64_t scene_bytes);

    const xm::release_totals& totals() const noexcept { return m_totals; }

private:
    void count(std::shared_ptr<media_payload>& payload);

    std::string m_why;
    std::chrono::steady_clock::time_point m_start;
    bool m_port_was_open = false;
    xm::release_totals m_totals;
    bool m_ended = false;
};
} // namespace ps4_xmb_media
