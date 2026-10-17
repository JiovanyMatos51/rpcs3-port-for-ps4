#pragma once

// Host-test stand-in for Emu/system_config.h (tests/cfg_browser_test.cpp): the REAL rpcs3/Emu/system_config.h, so the
// test walks the tree the console builds, compiled over the cfg mock (cfg-mocks/Utilities/Config.h) with the branch the
// PS4 launcher compiles (RPCS3_PS4, __ORBIS__, LLVM_AVAILABLE, HAVE_VULKAN) and the enum layout of the PS4 build (no
// _WIN32 enumerators). The test TU defines cfg_root::node_sys::get_random_system_name() / get_random_psid().
// Include order: -I ps4-port/tests/cfg-mocks before -I ps4-port/tests/policy-mocks and -I rpcs3.

#include "Utilities/Config.h"

#pragma push_macro("_WIN32")
#undef _WIN32
#include "../../../../rpcs3/Emu/system_config_types.h"
#pragma pop_macro("_WIN32")

enum CellNetCtlState : s32;
enum CellSysutilLicenseArea : s32;
enum CellSysutilLang : s32;
enum CellKbMappingType : s32;

namespace cfg_mock
{
#define CFG_MOCK_ENUM(T, ...) \
    template <> \
    struct enum_names<T> \
    { \
        static constexpr std::pair<T, std::string_view> values[] = {__VA_ARGS__}; \
    };
#include "../cfg_enum_names.inc"
#undef CFG_MOCK_ENUM
} // namespace cfg_mock

#pragma push_macro("RPCS3_PS4")
#pragma push_macro("__ORBIS__")
#pragma push_macro("LLVM_AVAILABLE")
#pragma push_macro("HAVE_VULKAN")
#undef RPCS3_PS4
#undef __ORBIS__
#undef LLVM_AVAILABLE
#undef HAVE_VULKAN
#define RPCS3_PS4 1
#define __ORBIS__ 1
#define LLVM_AVAILABLE 1
#define HAVE_VULKAN 1
#include "../../../../rpcs3/Emu/system_config.h"
#pragma pop_macro("HAVE_VULKAN")
#pragma pop_macro("LLVM_AVAILABLE")
#pragma pop_macro("__ORBIS__")
#pragma pop_macro("RPCS3_PS4")
