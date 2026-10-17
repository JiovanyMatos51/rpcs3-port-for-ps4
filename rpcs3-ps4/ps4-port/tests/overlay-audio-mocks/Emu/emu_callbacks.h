#pragma once
#include <functional>
#include <memory>
#include "util/video_source.h"
struct test_emu_callbacks
{
    std::function<std::unique_ptr<video_source>()> make_video_source;
};
inline test_emu_callbacks g_emu_callbacks;
