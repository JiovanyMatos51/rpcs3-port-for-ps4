# Build runtime libraries for the same PS4 ABI as the emulator. Host archives
# cannot be used in the final EBOOT even when their headers compile.
file(GLOB _ps4_compat_sources CONFIGURE_DEPENDS
    "${RPCS3_PS4_ORBIS_COMPAT}/src/*.c"
    "${RPCS3_PS4_ORBIS_COMPAT}/src/*.cpp")
add_library(rpcs3_ps4_orbis_compat STATIC ${_ps4_compat_sources})
set_target_properties(rpcs3_ps4_orbis_compat PROPERTIES CXX_STANDARD 17)
target_include_directories(rpcs3_ps4_orbis_compat BEFORE PRIVATE
    "${RPCS3_PS4_ORBIS_COMPAT}/include")
# 0.26: the musl heap lives in 128 MiB direct-memory carve-outs. 12 (1.5 GiB) ran out while LLVM linked
# all PPU modules of Demon's Souls. 16 (2 GiB) still leaves room in the 4.6 GiB pool for Mesa's 1 GiB
# arena, ~1.1 GiB of guest memory and the JIT code arenas.
# 0.31: the cap is given in bytes (2 GiB, unchanged) and the slot count grows to 48, so that a failed
# 128 MiB allocation can retry with smaller carve-outs (CARVE_LADDER) without lowering the cap.
# Trimming fully free carve-outs (TRIM) stays off until its host test passes. All four macros are
# opt-in: other ports sharing orbis-compat keep the old behaviour. An orbis-compat checkout without the
# byte cap derives the cap from the slot count, where 48 slots would mean a 6 GiB heap, so the old
# 16-slot setting is kept for it.
# 0.34: musl's small heap grows in steps of 16 KiB << (step/2) that are never returned (256 MiB steps past
# 512 MiB, a 512 MiB step past 1 GiB that no carve-out can serve); EXPAND_STEP_MAX_MIB caps a step at
# 16 MiB so free carve-out blocks serve it. FLEX_FLOOR_MIB refuses a flexible-memory fallback that would
# leave less than 64 MiB for thread stacks. Both are ignored by an orbis-compat without them.
# 0.38: the 0.37 overdraft took the last 128 MiB of the 4608 MiB direct pool and a 2 MiB JIT commit failed. New
# carve-outs are admitted against a reservation for the other direct-memory owners (orbis::mmapDirectSetReserve,
# fed by the budget authority from orbis::directLedger, a sceKernelDirectMemoryQuery walk); the overdraft always
# keeps 256 MiB free. HEAP_BAND_MIB places carve-outs at the top of the pool (guest memory, GPU and JIT allocate
# first fit from the bottom). SEGREGATE keeps musl's small-heap steps and large mappings in separate carve-outs,
# so large ones empty out and TRIM=1 returns them (at the XMB, at stage ends, and when a carve-out is refused).
# Host test: ps4-port/tests/orbis_mmap_trim_test.cpp (RPCS3 0.38 knob set). Kill switches read once per process:
# /data/rpcs3/no-trim.flag (no trim, no segregation) and /data/rpcs3/no-ledger-query.flag (estimated ledger).
set(_ps4_orbis_mmap_source "${RPCS3_PS4_ORBIS_COMPAT}/src/orbis_mmap.cpp")
set(_ps4_orbis_mmap_byte_cap)
set(_ps4_orbis_mmap_authority)
if(EXISTS "${_ps4_orbis_mmap_source}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_ps4_orbis_mmap_source}")
    file(STRINGS "${_ps4_orbis_mmap_source}" _ps4_orbis_mmap_byte_cap REGEX "ORBIS_MMAP_DIRECT_CAP_MIB")
    file(STRINGS "${_ps4_orbis_mmap_source}" _ps4_orbis_mmap_authority REGEX "mmapDirectSetReserve")
endif()
if(NOT _ps4_orbis_mmap_authority)
    message(FATAL_ERROR "orbis-compat at ${RPCS3_PS4_ORBIS_COMPAT} predates orbis::directLedger and "
        "orbis::mmapDirectSetReserve, which the 0.38 memory budget calls")
endif()
if(_ps4_orbis_mmap_byte_cap)
    target_compile_definitions(rpcs3_ps4_orbis_compat PRIVATE
        ORBIS_MMAP_MAX_CARVES=48
        ORBIS_MMAP_DIRECT_CAP_MIB=2048
        ORBIS_MMAP_CARVE_LADDER=1
        ORBIS_MMAP_TRIM=1
        ORBIS_MMAP_EXPAND_STEP_MAX_MIB=16
        ORBIS_MMAP_FLEX_FLOOR_MIB=64
        ORBIS_MMAP_SEGREGATE=1
        ORBIS_MMAP_HEAP_BAND_MIB=2304
        "ORBIS_MMAP_TRIM_OFF_FLAG=\"/data/rpcs3/no-trim.flag\""
        "ORBIS_MMAP_QUERY_OFF_FLAG=\"/data/rpcs3/no-ledger-query.flag\"")
else()
    message(WARNING "orbis-compat has no ORBIS_MMAP_DIRECT_CAP_MIB; keeping 16 heap carve-outs (2 GiB cap)")
    target_compile_definitions(rpcs3_ps4_orbis_compat PRIVATE ORBIS_MMAP_MAX_CARVES=16)
endif()
# 0.34: clock_gettime's call counters (a locked add on one shared line per steady_clock::now()) are not
# read by this build.
target_compile_definitions(rpcs3_ps4_orbis_compat PRIVATE ORBIS_CLOCK_COUNTERS=0)
target_compile_options(rpcs3_ps4_orbis_compat PRIVATE
    -U_GNU_SOURCE
    "$<$<COMPILE_LANGUAGE:CXX>:-include>"
    "$<$<COMPILE_LANGUAGE:CXX>:orbis_prefix.h>")

set(BUILD_SHARED_LIBS OFF CACHE BOOL "Static console dependencies" FORCE)
add_subdirectory("${CMAKE_SOURCE_DIR}/3rdparty/asmjit" "deps/asmjit")
add_subdirectory("${CMAKE_SOURCE_DIR}/3rdparty/yaml-cpp" "deps/yaml-cpp")
set(PUGIXML_INSTALL OFF CACHE BOOL "No console dependency installation" FORCE)
add_subdirectory("${CMAKE_SOURCE_DIR}/3rdparty/pugixml" "deps/pugixml")
add_subdirectory("${CMAKE_SOURCE_DIR}/3rdparty/SoundTouch" "deps/soundtouch")
add_subdirectory("${CMAKE_SOURCE_DIR}/3rdparty/fusion" "deps/fusion")
add_subdirectory("${CMAKE_SOURCE_DIR}/3rdparty/zstd" "deps/zstd")
target_sources(libzstd_static PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/runtime/qsort_r.c")
target_include_directories(libzstd_static PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/compat")
target_compile_options(libzstd_static PRIVATE -include ps4_qsort_r.h)
add_subdirectory("${CMAKE_SOURCE_DIR}/3rdparty/glslang" "deps/glslang")

set(ABSL_BUILD_TESTING OFF CACHE BOOL "No host Abseil tests" FORCE)
set(ABSL_ENABLE_INSTALL OFF CACHE BOOL "No console dependency installation" FORCE)
set(ABSL_PROPAGATE_CXX_STD ON CACHE BOOL "Use the console language standard" FORCE)
set(RPCS3_PS4_ABSEIL "${CMAKE_CURRENT_SOURCE_DIR}/deps/abseil-cpp" CACHE PATH
    "Abseil 20250512.1 checkout for Protobuf 33.4")
if(NOT EXISTS "${RPCS3_PS4_ABSEIL}/CMakeLists.txt")
    message(FATAL_ERROR "Run ps4-port/prepare-abseil.ps1 before configuring the console port")
endif()
# OpenOrbis libc++ undefines __FreeBSD__. Abseil otherwise removes mmap-based
# allocation and synchronization implementations while Protobuf still calls
# them. Recognize Orbis explicitly without changing libc++'s platform macros.
file(READ "${RPCS3_PS4_ABSEIL}/absl/base/config.h" _ps4_absl_config)
string(FIND "${_ps4_absl_config}" "defined(__FreeBSD__) ||" _ps4_absl_platform)
if(_ps4_absl_platform EQUAL -1)
    message(FATAL_ERROR "Abseil platform configuration changed; Orbis overlay needs review")
endif()
string(REPLACE "defined(__FreeBSD__) ||" "defined(__FreeBSD__) || defined(__ORBIS__) ||"
    _ps4_absl_config "${_ps4_absl_config}")
set(_ps4_absl_overlay "${CMAKE_CURRENT_BINARY_DIR}/abseil_compat")
file(MAKE_DIRECTORY "${_ps4_absl_overlay}/absl/base")
if(EXISTS "${_ps4_absl_overlay}/absl/base/config.h")
    file(READ "${_ps4_absl_overlay}/absl/base/config.h" _ps4_absl_existing)
endif()
if(NOT _ps4_absl_existing STREQUAL _ps4_absl_config)
    file(WRITE "${_ps4_absl_overlay}/absl/base/config.h" "${_ps4_absl_config}")
endif()
include_directories(BEFORE "${_ps4_absl_overlay}")
add_subdirectory("${RPCS3_PS4_ABSEIL}" "deps/abseil-cpp")
set(protobuf_WITH_ZLIB OFF CACHE BOOL "Protobuf does not need gzip IO" FORCE)
set(protobuf_LOCAL_DEPENDENCIES_ONLY ON CACHE BOOL "Use pinned console dependencies" FORCE)
add_subdirectory("${CMAKE_SOURCE_DIR}/3rdparty/protobuf" "deps/protobuf")

# Match upstream RPCS3's TLS feature set and expose the generated options.h
# to every caller. The default headers alone describe a different wolfSSL ABI.
add_subdirectory("${CMAKE_SOURCE_DIR}/3rdparty/wolfssl" "deps/wolfssl")
# 0.31: seed wolfSSL's Hash-DRBG from libSceRandom instead of /dev/urandom, which the application
# cannot rely on (RPCN, curl and clans share this build). The definition is PRIVATE: it only selects
# the wc_GenerateSeed() branch in random.c and leaves the public wolfSSL ABI defines untouched. The
# prototype is force-included into C sources only, because the wolfSSL project also enables ASM.
target_sources(wolfssl PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/runtime/ps4_wolfssl_seed.c")
target_compile_definitions(wolfssl PRIVATE CUSTOM_RAND_GENERATE_SEED=rpcs3_ps4_wolfssl_seed)
target_compile_options(wolfssl PRIVATE
    "$<$<COMPILE_LANGUAGE:C>:-include;${CMAKE_CURRENT_SOURCE_DIR}/compat/ps4_wolfssl_seed.h>")
include(curl.cmake)
include(ffmpeg.cmake)
add_library(rpcs3_ps4_usb STATIC runtime/ps4_libusb.cpp)
target_include_directories(rpcs3_ps4_usb PRIVATE
    "${CMAKE_SOURCE_DIR}/3rdparty/libusb/libusb/libusb")
target_link_directories(rpcs3_ps4_usb INTERFACE "${RPCS3_PS4_SDK}/lib")
target_link_libraries(rpcs3_ps4_usb INTERFACE SceUsbd SceSysmodule)
add_library(rpcs3_ps4_capture STATIC runtime/ps4_capture.cpp)
target_include_directories(rpcs3_ps4_capture PRIVATE
    "${CMAKE_SOURCE_DIR}/3rdparty/OpenAL/openal-soft/include/AL"
    "${CMAKE_SOURCE_DIR}/3rdparty/ffmpeg/include")
target_compile_definitions(rpcs3_ps4_capture PRIVATE AL_LIBTYPE_STATIC=1)
target_compile_options(rpcs3_ps4_capture PRIVATE -fexceptions)
target_link_directories(rpcs3_ps4_capture INTERFACE "${RPCS3_PS4_SDK}/lib")
target_link_libraries(rpcs3_ps4_capture INTERFACE rpcs3_ps4_ffmpeg
    SceAudioIn SceUserService SceSysmodule)
# Extend the C API's enum in a generated header; the original PC backends
# remain unchanged. The USB MIDI service is a native input implementation.
set(_ps4_midi_headers "${CMAKE_CURRENT_BINARY_DIR}/midi_compat")
file(MAKE_DIRECTORY "${_ps4_midi_headers}")
file(READ "${CMAKE_SOURCE_DIR}/3rdparty/rtmidi/rtmidi/rtmidi_c.h" _ps4_midi_header)
string(REPLACE "    RTMIDI_API_NUM" "    RTMIDI_API_PS4_USB,\n    RTMIDI_API_NUM" _ps4_midi_header "${_ps4_midi_header}")
if(EXISTS "${_ps4_midi_headers}/rtmidi_c.h")
    file(READ "${_ps4_midi_headers}/rtmidi_c.h" _ps4_midi_existing)
endif()
if(NOT _ps4_midi_existing STREQUAL _ps4_midi_header)
    file(WRITE "${_ps4_midi_headers}/rtmidi_c.h" "${_ps4_midi_header}")
endif()
add_library(rpcs3_ps4_midi STATIC runtime/ps4_midi.cpp)
target_include_directories(rpcs3_ps4_midi PRIVATE "${_ps4_midi_headers}"
    "${CMAKE_SOURCE_DIR}/3rdparty/libusb/libusb/libusb")
target_include_directories(rpcs3_ps4_host_services BEFORE PRIVATE "${_ps4_midi_headers}")
target_compile_options(rpcs3_ps4_midi PRIVATE -fexceptions)
target_link_libraries(rpcs3_ps4_midi INTERFACE rpcs3_ps4_usb)
add_subdirectory("${CMAKE_SOURCE_DIR}/3rdparty/miniupnp" "deps/miniupnp")
file(GLOB _ps4_zlib_archive "${ORBIS_MESA_BUILD}/subprojects/zlib-*/libz.a")
list(LENGTH _ps4_zlib_archive _ps4_zlib_count)
if(NOT _ps4_zlib_count EQUAL 1)
    message(FATAL_ERROR "Expected exactly one zlib archive in the PS4 Mesa bundle")
endif()
set(ZLIB_LIBRARY "${_ps4_zlib_archive}" CACHE FILEPATH "PS4 Mesa zlib archive" FORCE)
set(ZLIB_LIBRARY_RELEASE "${_ps4_zlib_archive}" CACHE FILEPATH "PS4 Mesa zlib archive" FORCE)
set(ZLIB_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/3rdparty/zlib/zlib" CACHE PATH "Pinned zlib headers" FORCE)
set(PNG_TOOLS OFF CACHE BOOL "No host libpng tools" FORCE)
add_subdirectory("${CMAKE_SOURCE_DIR}/3rdparty/libpng" "deps/libpng")

add_custom_target(rpcs3_ps4_dependencies DEPENDS
    rpcs3_ps4_orbis_compat
    asmjit yaml-cpp pugixml soundtouch Fusion libzstd_static SPIRV libprotobuf
    wolfssl rpcs3_ps4_curl_build rpcs3_ps4_ffmpeg_build rpcs3_ps4_usb
    rpcs3_ps4_capture rpcs3_ps4_midi libminiupnpc-static png_static)

# Propagate library configuration as well as headers. SoundTouch sample type
# and the Abseil ABI options must match the actual console libraries.
target_link_libraries(rpcs3_ps4_host_services PRIVATE soundtouch)
target_link_libraries(rpcs3_ps4_lv2 PRIVATE soundtouch)
target_link_libraries(rpcs3_ps4_cell_modules PRIVATE soundtouch)
target_link_libraries(rpcs3_ps4_rsx PRIVATE SPIRV)
target_link_libraries(rpcs3_ps4_np PRIVATE libprotobuf)
target_link_libraries(rpcs3_ps4_cpu PRIVATE asmjit)
target_link_libraries(rpcs3_ps4_utilities PRIVATE asmjit yaml-cpp)
target_link_libraries(rpcs3_ps4_runtime PRIVATE asmjit)
target_link_libraries(rpcs3_ps4_host_services PRIVATE Fusion)
target_link_libraries(rpcs3_ps4_np PRIVATE wolfssl libminiupnpc-static)
target_link_libraries(rpcs3_ps4_lv2 PRIVATE wolfssl)
target_link_libraries(rpcs3_ps4_cell_modules PRIVATE wolfssl png_static)
