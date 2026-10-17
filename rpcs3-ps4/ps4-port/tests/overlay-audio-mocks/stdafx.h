#pragma once
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
struct test_overlay_log
{
    unsigned warnings = 0;
    template<class... Args> void warning(const char*, const Args&...) { ++warnings; }
};
inline test_overlay_log rsx_log;
