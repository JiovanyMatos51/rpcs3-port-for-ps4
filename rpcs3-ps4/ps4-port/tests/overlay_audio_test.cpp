// Compile the real overlay_audio.cpp with only the frontend callback mocked.
#include "stdafx.h"
#include "Emu/emu_callbacks.h"
#include "Emu/RSX/Overlays/overlay_audio.h"
#include <cassert>
#include <iostream>

namespace fmt
{
[[noreturn]] void raw_verify_error(std::source_location, const char8_t*, usz)
{
    std::cerr << "FAIL: original ensure rejected the null presentation player\n";
    std::exit(1);
}
}
namespace
{
struct record
{
    std::string audio_path, iso_path;
    bool archive = false, active = false;
    unsigned destroyed = 0;
};
struct player final : video_source
{
    record& state;
    explicit player(record& r) : state(r) {}
    ~player() override { ++state.destroyed; }
    void set_iso_path(const std::string& path) override { state.iso_path = path; }
    void set_video_path(const std::string&, bool) override {}
    void set_audio_path(const std::string& path, bool archive) override { state.audio_path = path; state.archive = archive; }
    void set_active(bool active) override { state.active = active; }
    bool get_active() const override { return state.active; }
    bool has_new() const override { return false; }
    void get_image(std::vector<u8>&, int&, int&, int&, int&) override {}
};
}
int main()
{
    try
    {
        unsigned calls = 0;
        g_emu_callbacks.make_video_source = [&]() -> std::unique_ptr<video_source> { ++calls; return {}; };
        {
            rsx::overlays::audio_player absent("/vfsv0_virtual_iso_overlay_fs_dev/PS3_GAME/SND0.AT3", false, "");
            absent.set_active(true);
            absent.set_active(false);
        }
        assert(calls == 1 && rsx_log.warnings == 1);
        {
            rsx::overlays::audio_player empty("", false, "");
            empty.set_active(true);
        }
        assert(calls == 1);
        g_emu_callbacks.make_video_source = {};
        {
            rsx::overlays::audio_player no_callback("SND0.AT3", false, "");
            no_callback.set_active(true);
        }
        assert(rsx_log.warnings == 2);
        record normal;
        g_emu_callbacks.make_video_source = [&]() { return std::make_unique<player>(normal); };
        {
            rsx::overlays::audio_player direct("/data/game/SND0.AT3", false, "unused.iso");
            assert(normal.audio_path == "/data/game/SND0.AT3" && !normal.archive && normal.iso_path.empty());
            direct.set_active(true); assert(normal.active);
            direct.set_active(false); assert(!normal.active);
        }
        assert(normal.destroyed == 1);
        record archive;
        g_emu_callbacks.make_video_source = [&]() { return std::make_unique<player>(archive); };
        {
            rsx::overlays::audio_player packed("PS3_GAME/SND0.AT3", true, "game.iso");
            assert(archive.audio_path == "PS3_GAME/SND0.AT3" && archive.archive && archive.iso_path == "game.iso");
            packed.set_active(true); assert(archive.active);
        }
        assert(archive.destroyed == 1 && rsx_log.warnings == 2);
        std::cout << "PASS: missing player/callback, empty path, direct/ISO paths, activation and destruction\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
