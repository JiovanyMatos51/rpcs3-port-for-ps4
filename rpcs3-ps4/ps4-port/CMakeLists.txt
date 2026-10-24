# Cross-compile the RPCS3 components and their runtime libraries for the PS4
# with the OpenOrbis toolchain. native_link.cmake links the console ELF and
# package-native.ps1 turns it into the EBOOT/PKG.
if(NOT CMAKE_CROSSCOMPILING OR NOT CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
    message(FATAL_ERROR "Configure with ps4-port/toolchain.cmake for x86_64 OpenOrbis")
endif()

# Correct PS4 kernel ABI declarations that the stock OpenOrbis headers inherit
# incorrectly from Linux (notably sigaction flags and the alternate stack).
set(RPCS3_PS4_ORBIS_COMPAT "${CMAKE_SOURCE_DIR}/../orbis-compat" CACHE PATH
    "orbis-compat checkout with PS4 ABI header corrections")
if(NOT EXISTS "${RPCS3_PS4_ORBIS_COMPAT}/include/signal.h")
    message(FATAL_ERROR "RPCS3_PS4_ORBIS_COMPAT must point at an orbis-compat checkout")
endif()
include_directories(BEFORE "${RPCS3_PS4_ORBIS_COMPAT}/include")

# OpenOrbis libc++ 11 predates C++20 constexpr std::allocator. RPCS3 uses
# constexpr allocation to prove that a base pointer has offset zero. Rebuild
# only this header in the build tree with the C++20 constant-evaluation path
# from modern libc++; normal allocation keeps the SDK's original ABI calls.
set(_ps4_memory_source "${RPCS3_PS4_SDK}/include/c++/v1/memory")
file(READ "${_ps4_memory_source}" _ps4_memory)
set(_ps4_memory_original "${_ps4_memory}")
macro(_patch_ps4_allocator _before _after)
    string(FIND "${_ps4_memory}" "${_before}" _position)
    if(_position EQUAL -1)
        message(FATAL_ERROR "OpenOrbis libc++ memory layout changed; constexpr allocator patch needs review")
    endif()
    string(REPLACE "${_before}" "${_after}" _ps4_memory "${_ps4_memory}")
endmacro()
_patch_ps4_allocator(
    "_LIBCPP_NODISCARD_AFTER_CXX17 _LIBCPP_INLINE_VISIBILITY _Tp* allocate(size_t __n)"
    "_LIBCPP_NODISCARD_AFTER_CXX17 _LIBCPP_INLINE_VISIBILITY _LIBCPP_CONSTEXPR_AFTER_CXX17 _Tp* allocate(size_t __n)")
_patch_ps4_allocator(
    "return static_cast<_Tp*>(_VSTD::__libcpp_allocate(__n * sizeof(_Tp), _LIBCPP_ALIGNOF(_Tp)));"
    "return __builtin_is_constant_evaluated() ? static_cast<_Tp*>(::operator new(__n * sizeof(_Tp))) : static_cast<_Tp*>(_VSTD::__libcpp_allocate(__n * sizeof(_Tp), _LIBCPP_ALIGNOF(_Tp)));")
_patch_ps4_allocator(
    "_LIBCPP_INLINE_VISIBILITY void deallocate(_Tp* __p, size_t __n) _NOEXCEPT"
    "_LIBCPP_INLINE_VISIBILITY _LIBCPP_CONSTEXPR_AFTER_CXX17 void deallocate(_Tp* __p, size_t __n) _NOEXCEPT")
_patch_ps4_allocator(
    "{_VSTD::__libcpp_deallocate((void*)__p, __n * sizeof(_Tp), _LIBCPP_ALIGNOF(_Tp));}"
    "{if (__builtin_is_constant_evaluated()) ::operator delete(__p); else _VSTD::__libcpp_deallocate((void*)__p, __n * sizeof(_Tp), _LIBCPP_ALIGNOF(_Tp));}")
if(_ps4_memory STREQUAL _ps4_memory_original)
    message(FATAL_ERROR "Failed to prepare constexpr allocator header")
endif()
set(_ps4_cpp_overlay "${CMAKE_CURRENT_BINARY_DIR}/compat_cpp")
file(MAKE_DIRECTORY "${_ps4_cpp_overlay}")
if(EXISTS "${_ps4_cpp_overlay}/memory")
    file(READ "${_ps4_cpp_overlay}/memory" _ps4_memory_existing)
endif()
if(NOT _ps4_memory_existing STREQUAL _ps4_memory)
    file(WRITE "${_ps4_cpp_overlay}/memory" "${_ps4_memory}")
endif()
include_directories(BEFORE "${_ps4_cpp_overlay}")

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# RPCS3 already uses CMPXCHG16B for its 128-bit atomics on x86-64. Let Clang
# inline atomic exchanges instead of importing a missing host libatomic call.
add_compile_options(-mcx16)

add_library(rpcs3_ps4_crypto STATIC
    ../rpcs3/Crypto/aes.cpp
    ../rpcs3/Crypto/aesni.cpp
    ../rpcs3/Crypto/ec.cpp
    ../rpcs3/Crypto/lz.cpp
    ../rpcs3/Crypto/md5.cpp
    ../rpcs3/Crypto/sha1.cpp
    ../rpcs3/Crypto/sha256.cpp
)

target_include_directories(rpcs3_ps4_crypto BEFORE PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/compat"
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/rpcs3"
    "${CMAKE_SOURCE_DIR}/3rdparty"
)
target_compile_options(rpcs3_ps4_crypto PRIVATE -fno-exceptions)

add_library(rpcs3_ps4_crypto_extra OBJECT
    ../rpcs3/Crypto/decrypt_binaries.cpp
    ../rpcs3/Crypto/key_vault.cpp
    ../rpcs3/Crypto/unedat.cpp
    ../rpcs3/Crypto/unpkg.cpp
    ../rpcs3/Crypto/unzip.cpp
    ../rpcs3/Crypto/utils.cpp
)
target_include_directories(rpcs3_ps4_crypto_extra BEFORE PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/compat"
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/rpcs3"
    "${CMAKE_SOURCE_DIR}/3rdparty"
    "${CMAKE_SOURCE_DIR}/3rdparty/zlib/zlib"
    "${CMAKE_SOURCE_DIR}/3rdparty/zstd/zstd/lib"
    "${CMAKE_SOURCE_DIR}/3rdparty/asmjit/asmjit/src"
    "${CMAKE_SOURCE_DIR}/3rdparty/yaml-cpp/yaml-cpp/include"
)
target_compile_options(rpcs3_ps4_crypto_extra PRIVATE
    -fno-exceptions
    -include concepts
    -D__cpp_coroutines=201703L
)
target_compile_definitions(rpcs3_ps4_crypto_extra PRIVATE ZLIB_CONST=1)

# Native input (DS4 pad handler and pad thread). The small concepts
# compatibility header is force-included because RPCS3 expects a newer libc++
# than the OpenOrbis SDK provides.
add_library(rpcs3_ps4_input STATIC
    ../rpcs3/Input/ps4_pad_handler.cpp
    ../rpcs3/Input/pad_thread.cpp
    ../rpcs3/Input/product_info.cpp
    ../rpcs3/Input/ps_move_config.cpp
    ../rpcs3/Input/ps_move_tracker.cpp
)
target_include_directories(rpcs3_ps4_input BEFORE PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/compat"
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/rpcs3"
    "${CMAKE_SOURCE_DIR}/3rdparty"
    "${CMAKE_SOURCE_DIR}/3rdparty/stblib/stb"
)
target_compile_options(rpcs3_ps4_input PRIVATE
    -fno-exceptions
    -include concepts
)

add_library(rpcs3_ps4_audio STATIC
    ../rpcs3/Emu/Audio/PS4/PS4AudioBackend.cpp
    ../rpcs3/Emu/Audio/PS4/PS4AudioEnumerator.cpp
)
target_include_directories(rpcs3_ps4_audio BEFORE PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/compat"
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/rpcs3"
    "${CMAKE_SOURCE_DIR}/3rdparty"
)
target_compile_options(rpcs3_ps4_audio PRIVATE -fno-exceptions -include concepts)

# Console frontend (entry point, launcher, fatal handler) and the PS4 runtime
# objects, compiled against the same headers as the RPCS3 core.
add_library(rpcs3_ps4_frontend OBJECT
    ../rpcs3/ps4_main.cpp
    ../rpcs3/ps4_fatal.cpp
    ../rpcs3/ps4_frame.cpp
    ../rpcs3/ps4_launcher.cpp
    ../rpcs3/ps4_profiler.cpp
    # 0.31: player seats for up to four DS4 controllers. Exception-free, so it stays
    # -fno-exceptions. Objects of this target are always linked, so rpcs3_ps4_input
    # resolves ps4_pads:: here. The on-screen keyboard wrapper is added below.
    ../rpcs3/ps4_pads.cpp
    # 0.40: the selected game's own XMB background and music (its PIC1/PIC0/PIC2 and SND0.AT3), read, decoded and
    # played only while no game runs. Exception-free, so it stays -fno-exceptions. FFmpeg's headers come from the
    # include block of the Vulkan section below, zlib's from the source property after this target.
    ../rpcs3/ps4_xmb_media.cpp
    runtime/ps4_virtual_memory.cpp
    runtime/ps4_shared_memory.cpp
    # These override libc.a members (real x86-64 fenv instead of the SDK's
    # dummy fenv.lo/fesetround.lo, and exit()). They must stay in this OBJECT
    # target: objects always win, while an archive member could lose to the
    # lazy libc.a definition and the fix would silently not apply.
    runtime/ps4_fenv.cpp
    runtime/ps4_exit_guard.c
    ../rpcs3/rpcs3_version.cpp
)
# 0.31: sources that arrive together with their first callers. While one is missing,
# its target is configured without it and a warning names the file, so the rest of
# the tree still builds. The glob is re-checked on every build and re-runs CMake as
# soon as the file appears.
function(ps4_add_pending_source target source)
    file(GLOB _ps4_pending_found CONFIGURE_DEPENDS "${source}")
    if(_ps4_pending_found)
        target_sources(${target} PRIVATE ${_ps4_pending_found})
    else()
        message(WARNING "${source} does not exist yet; ${target} is configured without it")
    endif()
endfunction()
# The system on-screen keyboard wrapper. Exception-free, so it stays -fno-exceptions.
ps4_add_pending_source(rpcs3_ps4_frontend "${CMAKE_SOURCE_DIR}/rpcs3/ps4_ime.cpp")
include("${CMAKE_SOURCE_DIR}/rpcs3/git-version.cmake")
gen_git_version("${CMAKE_SOURCE_DIR}/rpcs3")
target_include_directories(rpcs3_ps4_frontend BEFORE PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/compat"
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/rpcs3"
    "${CMAKE_SOURCE_DIR}/3rdparty"
    "${CMAKE_SOURCE_DIR}/3rdparty/stblib/stb"
)
# C++-only options: the frontend also contains a C object (ps4_exit_guard.c),
# which gets the SDK C include path from CMAKE_C_FLAGS_INIT (toolchain.cmake).
target_compile_options(rpcs3_ps4_frontend PRIVATE
    "$<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions;-include;concepts>")
# The fatal/terminate handler must rethrow/catch to read std::current_exception().
# Source options follow the target options, so -fexceptions wins (as for yaml.cpp).
set_source_files_properties("${CMAKE_SOURCE_DIR}/rpcs3/ps4_fatal.cpp"
    PROPERTIES COMPILE_OPTIONS -fexceptions)
# The XMB media cache stores its overlays zlib-compressed (zlib is linked already). Only this source gets zlib's
# folder, so no other frontend file's includes resolve differently.
set_source_files_properties("${CMAKE_SOURCE_DIR}/rpcs3/ps4_xmb_media.cpp"
    PROPERTIES INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/3rdparty/zlib/zlib")

# Guest address-space mapping and JIT memory protection for the console ABI.
add_library(rpcs3_ps4_vm OBJECT
    ../rpcs3/util/vm_native.cpp
)
target_include_directories(rpcs3_ps4_vm BEFORE PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/compat"
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/rpcs3"
    "${CMAKE_SOURCE_DIR}/3rdparty"
)
target_compile_options(rpcs3_ps4_vm PRIVATE -fno-exceptions -include concepts)

add_library(rpcs3_ps4_runtime OBJECT
    ../Utilities/File.cpp
    ../Utilities/Thread.cpp
)
target_include_directories(rpcs3_ps4_runtime BEFORE PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/compat"
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/rpcs3"
    "${CMAKE_SOURCE_DIR}/3rdparty"
    "${CMAKE_SOURCE_DIR}/3rdparty/zlib/zlib"
    "${CMAKE_SOURCE_DIR}/3rdparty/asmjit/asmjit/src"
)
# libc++ 11 detects the old Coroutines TS feature macro, while Clang 18 only
# defines the standardized __cpp_impl_coroutine macro. Its TS header uses the
# same compiler coroutine builtins, and RPCS3 aliases those types into std.
target_compile_options(rpcs3_ps4_runtime PRIVATE
    -fno-exceptions
    -include concepts
    -D__cpp_coroutines=201703L
)

add_library(rpcs3_ps4_utilities OBJECT
    ../rpcs3/util/atomic.cpp
    ../rpcs3/util/console.cpp
    ../rpcs3/util/emu_utils.cpp
    ../rpcs3/util/media_utils.cpp
    ../rpcs3/util/video_provider.cpp
    ../rpcs3/util/logs.cpp
    ../rpcs3/util/yaml.cpp
    ../rpcs3/util/dyn_lib.cpp
    ../rpcs3/util/sysinfo.cpp
    ../rpcs3/util/cpu_stats.cpp
    ../rpcs3/util/serialization_ext.cpp
    ../Utilities/bin_patch.cpp
    ../Utilities/cheat_info.cpp
    ../Utilities/cond.cpp
    ../Utilities/Config.cpp
    ../Utilities/date_time.cpp
    ../Utilities/JITASM.cpp
    ../Utilities/JITLLVM.cpp
    ../Utilities/LUrlParser.cpp
    ../Utilities/mutex.cpp
    ../Utilities/rXml.cpp
    ../Utilities/sema.cpp
    ../Utilities/simple_ringbuf.cpp
    ../Utilities/stack_trace.cpp
    ../Utilities/stereo_config.cpp
    ../Utilities/StrFmt.cpp
    ../Utilities/version.cpp
)
target_include_directories(rpcs3_ps4_utilities BEFORE PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/compat"
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/rpcs3"
    "${CMAKE_SOURCE_DIR}/3rdparty"
    "${CMAKE_SOURCE_DIR}/3rdparty/zlib/zlib"
    "${CMAKE_SOURCE_DIR}/3rdparty/asmjit/asmjit/src"
    "${CMAKE_SOURCE_DIR}/3rdparty/yaml-cpp/yaml-cpp/include"
    "${CMAKE_SOURCE_DIR}/3rdparty/stblib/stb"
    "${CMAKE_SOURCE_DIR}/3rdparty/ffmpeg/include"
    "${CMAKE_SOURCE_DIR}/3rdparty/zstd/zstd/lib"
    "${CMAKE_SOURCE_DIR}/3rdparty/pugixml/src"
)
target_compile_options(rpcs3_ps4_utilities PRIVATE
    -fno-exceptions
    -include concepts
    -D__cpp_coroutines=201703L
)
target_compile_definitions(rpcs3_ps4_utilities PRIVATE ZLIB_CONST=1)
set_source_files_properties("${CMAKE_SOURCE_DIR}/rpcs3/util/yaml.cpp"
    PROPERTIES COMPILE_OPTIONS -fexceptions)

# Emulator system core and the PPU/SPU/RSX threads (original RPCS3
# translation units).
add_library(rpcs3_ps4_emulation OBJECT
    ../rpcs3/Emu/System.cpp
    ../rpcs3/Emu/Cell/PPUThread.cpp
    ../rpcs3/Emu/Cell/SPUThread.cpp
    ../rpcs3/Emu/RSX/RSXThread.cpp
)
target_include_directories(rpcs3_ps4_emulation BEFORE PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/compat"
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/rpcs3"
    "${CMAKE_SOURCE_DIR}/3rdparty"
    "${CMAKE_SOURCE_DIR}/3rdparty/zlib/zlib"
    "${CMAKE_SOURCE_DIR}/3rdparty/asmjit/asmjit/src"
    "${CMAKE_SOURCE_DIR}/3rdparty/stblib/stb"
    "${CMAKE_SOURCE_DIR}/3rdparty/yaml-cpp/yaml-cpp/include"
)
target_compile_options(rpcs3_ps4_emulation PRIVATE
    -fno-exceptions
    -include concepts
    -D__cpp_coroutines=201703L
)

add_library(rpcs3_ps4_cpu OBJECT
    ../rpcs3/Emu/Cell/ErrorCodes.cpp
    ../rpcs3/Emu/Cell/MFC.cpp
    ../rpcs3/Emu/Cell/PPUAnalyser.cpp
    ../rpcs3/Emu/Cell/PPUDisAsm.cpp
    ../rpcs3/Emu/Cell/PPUFunction.cpp
    ../rpcs3/Emu/Cell/PPUInterpreter.cpp
    ../rpcs3/Emu/Cell/PPUModule.cpp
    ../rpcs3/Emu/Cell/RawSPUThread.cpp
    ../rpcs3/Emu/Cell/SPUAnalyser.cpp
    ../rpcs3/Emu/Cell/SPUASMJITRecompiler.cpp
    ../rpcs3/Emu/Cell/SPUDisAsm.cpp
    ../rpcs3/Emu/Cell/SPUInterpreter.cpp
    ../rpcs3/Emu/Cell/SPULLVMRecompiler.cpp
    ../rpcs3/Emu/Cell/SPUCommonRecompiler.cpp
)
target_include_directories(rpcs3_ps4_cpu BEFORE PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/compat"
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/rpcs3"
    "${CMAKE_SOURCE_DIR}/3rdparty"
    "${CMAKE_SOURCE_DIR}/3rdparty/zlib/zlib"
    "${CMAKE_SOURCE_DIR}/3rdparty/zstd/zstd/lib"
    "${CMAKE_SOURCE_DIR}/3rdparty/asmjit/asmjit/src"
    "${CMAKE_SOURCE_DIR}/3rdparty/yaml-cpp/yaml-cpp/include"
    "${CMAKE_SOURCE_DIR}/3rdparty/stblib/stb"
)
target_compile_options(rpcs3_ps4_cpu PRIVATE
    -fno-exceptions
    -include concepts
    -D__cpp_coroutines=201703L
)

# Jaguar code generation for every RPCS3 target is applied once, near the end
# of this file (see RPCS3_PS4_JAGUAR_FLAGS), after all component targets exist.

file(STRINGS "${CMAKE_SOURCE_DIR}/rpcs3/Emu/CMakeLists.txt" _ps4_lv2_sources
    REGEX "^[ \t]*Cell/lv2/.*[.]cpp[ \t]*$")
list(TRANSFORM _ps4_lv2_sources STRIP)
list(TRANSFORM _ps4_lv2_sources PREPEND "${CMAKE_SOURCE_DIR}/rpcs3/Emu/")
if(NOT _ps4_lv2_sources)
    message(FATAL_ERROR "Could not extract the upstream RPCS3 lv2 source list")
endif()
add_library(rpcs3_ps4_lv2 OBJECT ${_ps4_lv2_sources})
target_include_directories(rpcs3_ps4_lv2 BEFORE PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/compat"
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/rpcs3"
    "${CMAKE_SOURCE_DIR}/3rdparty"
    "${CMAKE_SOURCE_DIR}/3rdparty/zlib/zlib"
    "${CMAKE_SOURCE_DIR}/3rdparty/zstd/zstd/lib"
    "${CMAKE_SOURCE_DIR}/3rdparty/asmjit/asmjit/src"
    "${CMAKE_SOURCE_DIR}/3rdparty/yaml-cpp/yaml-cpp/include"
    "${CMAKE_SOURCE_DIR}/3rdparty/pugixml/src"
    "${CMAKE_SOURCE_DIR}/3rdparty/stblib/stb"
    "${CMAKE_SOURCE_DIR}/3rdparty/SoundTouch/soundtouch/include"
    "${CMAKE_SOURCE_DIR}/3rdparty/wolfssl/wolfssl"
    "${CMAKE_SOURCE_DIR}/3rdparty/miniupnp/miniupnp/miniupnpc/include"
    "${CMAKE_SOURCE_DIR}/3rdparty/libusb/libusb/libusb"
    "${CMAKE_SOURCE_DIR}/3rdparty/rtmidi/rtmidi"
)
target_compile_options(rpcs3_ps4_lv2 PRIVATE
    -fexceptions
    -include concepts
    -D__cpp_coroutines=201703L
)
target_compile_definitions(rpcs3_ps4_lv2 PRIVATE ZLIB_CONST=1)

file(STRINGS "${CMAKE_SOURCE_DIR}/rpcs3/Emu/CMakeLists.txt" _ps4_cell_modules
    REGEX "^[ \t]*Cell/Modules/.*[.]cpp[ \t]*$")
list(TRANSFORM _ps4_cell_modules STRIP)
list(TRANSFORM _ps4_cell_modules PREPEND "${CMAKE_SOURCE_DIR}/rpcs3/Emu/")
if(NOT _ps4_cell_modules)
    message(FATAL_ERROR "Could not extract the upstream RPCS3 Cell module list")
endif()
set(_ps4_libpng_headers "${CMAKE_CURRENT_BINARY_DIR}/libpng_compat")
file(MAKE_DIRECTORY "${_ps4_libpng_headers}")
configure_file("${CMAKE_SOURCE_DIR}/3rdparty/libpng/libpng/scripts/pnglibconf.h.prebuilt"
    "${_ps4_libpng_headers}/pnglibconf.h" COPYONLY)
add_library(rpcs3_ps4_cell_modules OBJECT ${_ps4_cell_modules})
target_include_directories(rpcs3_ps4_cell_modules BEFORE PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/compat"
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/rpcs3"
    "${CMAKE_SOURCE_DIR}/3rdparty"
    "${CMAKE_SOURCE_DIR}/3rdparty/zlib/zlib"
    "${CMAKE_SOURCE_DIR}/3rdparty/zstd/zstd/lib"
    "${CMAKE_SOURCE_DIR}/3rdparty/asmjit/asmjit/src"
    "${CMAKE_SOURCE_DIR}/3rdparty/yaml-cpp/yaml-cpp/include"
    "${CMAKE_SOURCE_DIR}/3rdparty/pugixml/src"
    "${CMAKE_SOURCE_DIR}/3rdparty/stblib/stb"
    "${CMAKE_SOURCE_DIR}/3rdparty/SoundTouch/soundtouch/include"
    "${CMAKE_SOURCE_DIR}/3rdparty/wolfssl/wolfssl"
    "${CMAKE_SOURCE_DIR}/3rdparty/miniupnp/miniupnp/miniupnpc/include"
    "${CMAKE_SOURCE_DIR}/3rdparty/libusb/libusb/libusb"
    "${CMAKE_SOURCE_DIR}/3rdparty/rtmidi/rtmidi"
    "${CMAKE_SOURCE_DIR}/3rdparty/ffmpeg/include"
    "${CMAKE_SOURCE_DIR}/3rdparty/OpenAL/openal-soft/include/AL"
    "${CMAKE_SOURCE_DIR}/3rdparty/libpng/libpng"
    "${_ps4_libpng_headers}"
    "${CMAKE_SOURCE_DIR}/3rdparty/curl/curl/include"
)
target_compile_options(rpcs3_ps4_cell_modules PRIVATE
    -fexceptions
    -include concepts
    -D__cpp_coroutines=201703L
)
target_compile_definitions(rpcs3_ps4_cell_modules PRIVATE ZLIB_CONST=1)

# Core services from RPCS3's own rpcs3_emu target. Keep these separate from
# hardware-specific threads so platform errors remain attributable.
add_library(rpcs3_ps4_core OBJECT
    ../rpcs3/Emu/cache_utils.cpp
    ../rpcs3/Emu/games_config.cpp
    ../rpcs3/Emu/IdManager.cpp
    ../rpcs3/Emu/localized_string.cpp
    ../rpcs3/Emu/savestate_utils.cpp
    ../rpcs3/Emu/scoped_progress_dialog.cpp
    ../rpcs3/Emu/stb_image.cpp
    ../rpcs3/Emu/system_config.cpp
    ../rpcs3/Emu/system_config_types.cpp
    ../rpcs3/Emu/system_progress.cpp
    ../rpcs3/Emu/system_utils.cpp
    ../rpcs3/Emu/vfs_config.cpp
    ../rpcs3/Emu/VFS.cpp
    ../rpcs3/Emu/GDB.cpp
    ../rpcs3/Emu/title.cpp
    ../rpcs3/Emu/perf_meter.cpp
    ../rpcs3/Emu/perf_monitor.cpp
    ../rpcs3/Emu/IPC_config.cpp
    ../rpcs3/Emu/IPC_socket.cpp
)
target_include_directories(rpcs3_ps4_core BEFORE PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/compat"
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/rpcs3"
    "${CMAKE_SOURCE_DIR}/3rdparty"
    "${CMAKE_SOURCE_DIR}/3rdparty/stblib/stb"
    "${CMAKE_SOURCE_DIR}/3rdparty/yaml-cpp/yaml-cpp/include"
    "${CMAKE_SOURCE_DIR}/3rdparty/asmjit/asmjit/src"
    "${CMAKE_SOURCE_DIR}/3rdparty/zlib/zlib"
)
target_compile_options(rpcs3_ps4_core PRIVATE
    -fno-exceptions
    -include concepts
    -D__cpp_coroutines=201703L
)
# system_config.cpp constructs g_cfg; it must see the same HAVE_VULKAN as the
# frontend (ps4_settings.hpp builds another cfg_root) or cfg_root's inline
# constructors differ between TUs (ODR; system_config.h default renderer).
# Do not move this to toolchain.cmake: System.cpp would then include
# <vulkan/vulkan.h> without the Mesa include dir, and the flag would leak into
# the curl/LLVM sub-builds. system_config.cpp #errors if this is missing.
target_compile_definitions(rpcs3_ps4_core PRIVATE HAVE_VULKAN=1)
# Cheats of the running game (boot writes from ppu_load_exec, the Main Thread tick, the
# home-menu rows). Exception-free, so it stays -fno-exceptions.
ps4_add_pending_source(rpcs3_ps4_core "${CMAKE_SOURCE_DIR}/rpcs3/Emu/ps4_cheat_engine.cpp")

add_library(rpcs3_ps4_loader OBJECT
    ../rpcs3/Emu/Memory/vm.cpp
    ../rpcs3/Crypto/unself.cpp
    ../rpcs3/Loader/disc.cpp
    ../rpcs3/Loader/ELF.cpp
    ../rpcs3/Loader/mself.cpp
    ../rpcs3/Loader/PSF.cpp
    ../rpcs3/Loader/PUP.cpp
    ../rpcs3/Loader/TAR.cpp
    ../rpcs3/util/ps4_firmware.cpp # 0.33: XMB firmware installer's SCEDecrypter step (unself.h needs this target's zlib path)
    ../rpcs3/Loader/ISO.cpp
    ../rpcs3/Loader/iso_cache.cpp
    ../rpcs3/Loader/content_validation.cpp
    ../rpcs3/Loader/TROPUSR.cpp
    ../rpcs3/Loader/TRP.cpp
)
target_include_directories(rpcs3_ps4_loader BEFORE PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/compat"
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/rpcs3"
    "${CMAKE_SOURCE_DIR}/3rdparty"
    "${CMAKE_SOURCE_DIR}/3rdparty/zlib/zlib"
    "${CMAKE_SOURCE_DIR}/3rdparty/asmjit/asmjit/src"
    "${CMAKE_SOURCE_DIR}/3rdparty/yaml-cpp/yaml-cpp/include"
    "${CMAKE_SOURCE_DIR}/3rdparty/zstd/zstd/lib"
    "${CMAKE_SOURCE_DIR}/3rdparty/pugixml/src"
)
target_compile_options(rpcs3_ps4_loader PRIVATE
    -fno-exceptions
    -include concepts
    -D__cpp_coroutines=201703L
)

set(RPCS3_PS4_MESA_BUNDLE "${CMAKE_SOURCE_DIR}/../orbis-mesa-bundle/orbis-mesa-4c934ebf9cda"
    CACHE PATH "Mesa PS4 bundle containing matching RADV archive and Vulkan headers")
if(NOT EXISTS "${RPCS3_PS4_MESA_BUNDLE}/include/vulkan/vulkan.h" OR
   NOT EXISTS "${RPCS3_PS4_MESA_BUNDLE}/build-orbis/src/amd/vulkan/libvulkan_radeon.a")
    message(FATAL_ERROR "RPCS3_PS4_MESA_BUNDLE must contain matching PS4 RADV headers and archive")
endif()

set(ORBIS_MESA_BUILD "${RPCS3_PS4_MESA_BUNDLE}/build-orbis" CACHE PATH "Mesa PS4 RADV build")
set(ORBIS_MESA_SRC "${RPCS3_PS4_MESA_BUNDLE}" CACHE PATH "Mesa PS4 Vulkan headers")
set(RPCS3_PS4_PORTING_KIT "${CMAKE_SOURCE_DIR}/../orbis-porting-kit" CACHE PATH
    "orbis-porting-kit checkout with the generated Vulkan loader")
if(NOT EXISTS "${RPCS3_PS4_PORTING_KIT}/vkloader/CMakeLists.txt")
    message(FATAL_ERROR "RPCS3_PS4_PORTING_KIT must point at orbis-porting-kit")
endif()
add_subdirectory("${RPCS3_PS4_PORTING_KIT}/vkloader" "${CMAKE_CURRENT_BINARY_DIR}/ps4-vkloader")

# Mesa's dispatch tables refer to many entry points weakly. Their object files
# must all be retained. Standalone whole-archive flags in the external loader
# are de-duplicated by CMake with the compatibility runtime's flags, leaving
# --whole-archive --no-whole-archive before RADV and silently omitting handlers.
# Associate the feature with this archive so CMake emits an indivisible group.
add_library(rpcs3_ps4_radv STATIC IMPORTED GLOBAL)
set_target_properties(rpcs3_ps4_radv PROPERTIES IMPORTED_LOCATION
    "${ORBIS_MESA_BUILD}/src/amd/vulkan/libvulkan_radeon.a")
get_target_property(_ps4_vkloader_links ps4-vkloader INTERFACE_LINK_LIBRARIES)
set(_ps4_vkloader_fixed_links)
foreach(_ps4_vkloader_link IN LISTS _ps4_vkloader_links)
    if(_ps4_vkloader_link MATCHES "^-Wl,--(no-)?whole-archive$")
        continue()
    elseif(_ps4_vkloader_link MATCHES "[/\\\\]libvulkan_radeon[.]a$")
        list(APPEND _ps4_vkloader_fixed_links
            "$<LINK_LIBRARY:WHOLE_ARCHIVE,rpcs3_ps4_radv>")
    else()
        list(APPEND _ps4_vkloader_fixed_links "${_ps4_vkloader_link}")
    endif()
endforeach()
set_property(TARGET ps4-vkloader PROPERTY INTERFACE_LINK_LIBRARIES "${_ps4_vkloader_fixed_links}")

# The frontend must instantiate the same VKGSRender implementation compiled
# below. Keep its Vulkan headers and support-library headers aligned with RADV.
target_include_directories(rpcs3_ps4_frontend BEFORE PRIVATE
    "${CMAKE_SOURCE_DIR}/3rdparty/GPUOpen/VulkanMemoryAllocator/include"
    "${CMAKE_SOURCE_DIR}/3rdparty/ffmpeg/include"
    "${CMAKE_SOURCE_DIR}/3rdparty/yaml-cpp/yaml-cpp/include"
    "${RPCS3_PS4_MESA_BUNDLE}/include"
)
target_compile_definitions(rpcs3_ps4_frontend PRIVATE HAVE_VULKAN=1)
target_compile_options(rpcs3_ps4_frontend PRIVATE -D__cpp_coroutines=201703L)

# Consume the upstream Vulkan translation-unit list so renderer changes in
# RPCS3 cannot silently leave PS4 with only a partial VKGSRender build.
file(STRINGS "${CMAKE_SOURCE_DIR}/rpcs3/Emu/CMakeLists.txt" _ps4_vulkan_sources
    REGEX "^[ \t]*RSX/VK/.*[.]cpp[ \t]*$")
list(TRANSFORM _ps4_vulkan_sources STRIP)
list(TRANSFORM _ps4_vulkan_sources PREPEND "${CMAKE_SOURCE_DIR}/rpcs3/Emu/")
if(NOT _ps4_vulkan_sources)
    message(FATAL_ERROR "Could not extract the upstream RPCS3 Vulkan source list")
endif()
add_library(rpcs3_ps4_vulkan OBJECT ${_ps4_vulkan_sources})
target_include_directories(rpcs3_ps4_vulkan BEFORE PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/compat"
    "${CMAKE_SOURCE_DIR}"
    "${CMAKE_SOURCE_DIR}/rpcs3"
    "${CMAKE_SOURCE_DIR}/3rdparty"
    "${CMAKE_SOURCE_DIR}/3rdparty/zlib/zlib"
    "${CMAKE_SOURCE_DIR}/3rdparty/asmjit/asmjit/src"
    "${CMAKE_SOURCE_DIR}/3rdparty/yaml-cpp/yaml-cpp/include"
    "${CMAKE_SOURCE_DIR}/3rdparty/GPUOpen/VulkanMemoryAllocator/include"
    "${CMAKE_SOURCE_DIR}/3rdparty/ffmpeg/include"
    "${CMAKE_SOURCE_DIR}/3rdparty/stblib/stb"
    "${RPCS3_PS4_MESA_BUNDLE}/include"
)
target_compile_definitions(rpcs3_ps4_vulkan PRIVATE HAVE_VULKAN=1)
target_compile_options(rpcs3_ps4_vulkan PRIVATE
    -fno-exceptions
    -include concepts
    -D__cpp_coroutines=201703L
)

# Continue with RPCS3's shared RSX and host services. Pull source lists from
# upstream so the console build cannot silently omit newly added services.
function(ps4_add_emu_component target source_pattern)
    file(STRINGS "${CMAKE_SOURCE_DIR}/rpcs3/Emu/CMakeLists.txt" sources
        REGEX "^[ \t]*(${source_pattern})[ \t]*$")
    list(TRANSFORM sources STRIP)
    list(REMOVE_ITEM sources "RSX/RSXThread.cpp") # Already in emulation.
    list(TRANSFORM sources PREPEND "${CMAKE_SOURCE_DIR}/rpcs3/Emu/")
    if(NOT sources)
        message(FATAL_ERROR "Empty upstream source list for ${target}")
    endif()
    add_library(${target} OBJECT ${sources})
    target_include_directories(${target} BEFORE PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/compat"
        "${CMAKE_SOURCE_DIR}" "${CMAKE_SOURCE_DIR}/rpcs3"
        "${CMAKE_SOURCE_DIR}/3rdparty"
        "${CMAKE_SOURCE_DIR}/3rdparty/zlib/zlib"
        "${CMAKE_SOURCE_DIR}/3rdparty/zstd/zstd/lib"
        "${CMAKE_SOURCE_DIR}/3rdparty/asmjit/asmjit/src"
        "${CMAKE_SOURCE_DIR}/3rdparty/yaml-cpp/yaml-cpp/include"
        "${CMAKE_SOURCE_DIR}/3rdparty/pugixml/src"
        "${CMAKE_SOURCE_DIR}/3rdparty/stblib/stb"
        "${CMAKE_SOURCE_DIR}/3rdparty/SoundTouch/soundtouch/include"
        "${CMAKE_SOURCE_DIR}/3rdparty/rtmidi/rtmidi"
        "${CMAKE_SOURCE_DIR}/3rdparty/ffmpeg/include"
        "${CMAKE_SOURCE_DIR}/3rdparty/OpenAL/openal-soft/include/AL"
        "${CMAKE_SOURCE_DIR}/3rdparty/libpng/libpng"
        "${_ps4_libpng_headers}"
        "${CMAKE_SOURCE_DIR}/3rdparty/glslang/glslang"
        "${CMAKE_SOURCE_DIR}/3rdparty/protobuf/protobuf/src"
        "${CMAKE_CURRENT_SOURCE_DIR}/deps/abseil-cpp"
        "${CMAKE_SOURCE_DIR}/3rdparty/wolfssl/wolfssl"
        "${CMAKE_SOURCE_DIR}/3rdparty/miniupnp/miniupnp/miniupnpc/include"
        "${CMAKE_SOURCE_DIR}/3rdparty/libusb/libusb/libusb"
        "${CMAKE_SOURCE_DIR}/3rdparty/curl/curl/include"
        "${RPCS3_PS4_MESA_BUNDLE}/include")
    target_compile_options(${target} PRIVATE
        -fexceptions -include concepts -D__cpp_coroutines=201703L)
    target_compile_definitions(${target} PRIVATE
        ZLIB_CONST=1 HAVE_VULKAN=1 WITHOUT_OPENGL=1)
endfunction()

ps4_add_emu_component(rpcs3_ps4_host_services "(CPU/[^/]*|Io/[^/]*|Audio/[^/]*)[.]cpp")
ps4_add_emu_component(rpcs3_ps4_rsx "RSX/(Capture|Common|Core|Host|Null|NV47|Overlays|Program|Utils)/.*[.]cpp|RSX/[^/]*[.]cpp")
# The home-menu cheats page (not in the upstream source list)
ps4_add_pending_source(rpcs3_ps4_rsx "${CMAKE_SOURCE_DIR}/rpcs3/Emu/RSX/Overlays/HomeMenu/overlay_home_menu_cheats.cpp")
ps4_add_emu_component(rpcs3_ps4_opengl "RSX/GL/.*[.]cpp")
# Reuse every original GL translation unit, including shader compilation,
# texture cache, RSX transfers and presentation. Mesa EGL replaces only the
# platform context/window and entry-point loader.
set_property(TARGET rpcs3_ps4_opengl PROPERTY COMPILE_DEFINITIONS ZLIB_CONST=1 HAVE_VULKAN=1)
set(_ps4_gallium "${ORBIS_MESA_BUILD}/src/gallium/targets/dri/libgallium-26.3.0-devel.a")
set(_ps4_egl "${ORBIS_MESA_BUILD}/src/egl/libEGL.a")
foreach(_ps4_gl_archive IN ITEMS "${_ps4_egl}" "${_ps4_gallium}")
    if(NOT EXISTS "${_ps4_gl_archive}")
        message(FATAL_ERROR "The PS4 OpenGL backend needs ${_ps4_gl_archive}")
    endif()
endforeach()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(_ps4_gl_prepared "${CMAKE_CURRENT_BINARY_DIR}/mesa-gl")
set(_ps4_gl_outputs "${_ps4_gl_prepared}/libEGL.rpcs3.a" "${_ps4_gl_prepared}/libGallium.rpcs3.a")
add_custom_command(OUTPUT ${_ps4_gl_outputs}
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/prepare-mesa-gl.py"
        --radv "${ORBIS_MESA_BUILD}/src/amd/vulkan/libvulkan_radeon.a"
        --gallium "${_ps4_gallium}" --egl "${_ps4_egl}"
        --output "${_ps4_gl_prepared}" --ar "${CMAKE_AR}"
    DEPENDS "${_ps4_gallium}" "${_ps4_egl}"
        "${ORBIS_MESA_BUILD}/src/amd/vulkan/libvulkan_radeon.a"
        "${CMAKE_CURRENT_SOURCE_DIR}/prepare-mesa-gl.py"
    VERBATIM)
add_custom_target(rpcs3_ps4_mesa_gl DEPENDS ${_ps4_gl_outputs})
ps4_add_emu_component(rpcs3_ps4_np "NP/.*[.](cpp|cc)")
# 0.31: the console RPCN service (account import, connect, status) uses rpcn_client and
# therefore wolfSSL and exceptions. It lives in the NP target, which already builds with
# -fexceptions and links wolfssl/protobuf, so the wolfSSL ABI defines never reach the
# frontend. The frontend only sees the wolfSSL-free ps4_rpcn_service.h API.
ps4_add_pending_source(rpcs3_ps4_np "${CMAKE_SOURCE_DIR}/rpcs3/Emu/NP/ps4_rpcn_service.cpp")
# Boot TLS self-test (libcurl + wolfSSL over loopback). Same reason as the RPCN service: it needs the
# wolfSSL ABI defines, so it lives here and the frontend sees only the wolfSSL-free ps4_tls_selftest.h.
ps4_add_pending_source(rpcs3_ps4_np "${CMAKE_SOURCE_DIR}/rpcs3/Emu/NP/ps4_tls_selftest.cpp")

include(dependencies.cmake)

# Assemble the original emulator objects into one archive for the native
# executable. Dependencies stay separate and retain their link order
# and ABI definitions through this CMake target.
add_library(rpcs3_ps4_emulator_core STATIC)
target_link_libraries(rpcs3_ps4_emulator_core PRIVATE
    rpcs3_ps4_crypto_extra rpcs3_ps4_vm rpcs3_ps4_runtime
    rpcs3_ps4_utilities rpcs3_ps4_emulation rpcs3_ps4_cpu
    rpcs3_ps4_lv2 rpcs3_ps4_cell_modules rpcs3_ps4_core
    rpcs3_ps4_loader rpcs3_ps4_vulkan rpcs3_ps4_opengl rpcs3_ps4_host_services
    rpcs3_ps4_rsx rpcs3_ps4_np)
target_link_libraries(rpcs3_ps4_emulator_core PUBLIC
    rpcs3_ps4_crypto rpcs3_ps4_input rpcs3_ps4_audio
    asmjit yaml-cpp pugixml soundtouch Fusion libzstd_static SPIRV libprotobuf
    wolfssl libcurl_static rpcs3_ps4_capture rpcs3_ps4_ffmpeg rpcs3_ps4_midi rpcs3_ps4_usb
    libminiupnpc-static png_static
    ${_ps4_gl_outputs} ps4-vkloader)
add_dependencies(rpcs3_ps4_emulator_core rpcs3_ps4_mesa_gl)

# Both PS4 models use Jaguar (btver2): SSE4.2, AVX with 128-bit units, F16C,
# BMI1, LZCNT, MOVBE, AES, PCLMUL; no AVX2, FMA or BMI2. Build every RPCS3
# translation unit for it so original simd.hpp/BufferUtils/SPU helpers take
# their SSSE3/SSE4.1/AVX paths instead of SSE2 fallbacks. Keep 128-bit vectors:
# Jaguar splits 256-bit operations. -march=btver2 does not define __AVX2__,
# __FMA__ or __BMI2__, so code guarded by those stays on its generic paths.
# Third-party sub-builds (ffmpeg, curl, LLVM, abseil, Mesa) and the in-tree
# third-party libraries keep their own flags; toolchain.cmake is unchanged.
set(RPCS3_PS4_JAGUAR_FLAGS -march=btver2 -mtune=btver2 -mprefer-vector-width=128)
set(_ps4_jaguar_targets
    rpcs3_ps4_crypto rpcs3_ps4_crypto_extra rpcs3_ps4_input rpcs3_ps4_audio
    rpcs3_ps4_frontend rpcs3_ps4_vm rpcs3_ps4_runtime rpcs3_ps4_utilities
    rpcs3_ps4_emulation rpcs3_ps4_cpu rpcs3_ps4_lv2 rpcs3_ps4_cell_modules
    rpcs3_ps4_core rpcs3_ps4_loader rpcs3_ps4_vulkan rpcs3_ps4_opengl
    rpcs3_ps4_host_services rpcs3_ps4_rsx rpcs3_ps4_np
    rpcs3_ps4_usb rpcs3_ps4_capture rpcs3_ps4_midi)
foreach(_ps4_jaguar_target IN LISTS _ps4_jaguar_targets)
    if(NOT TARGET ${_ps4_jaguar_target})
        message(FATAL_ERROR "Jaguar flags: missing target ${_ps4_jaguar_target}")
    endif()
    target_compile_options(${_ps4_jaguar_target} PRIVATE ${RPCS3_PS4_JAGUAR_FLAGS})
endforeach()

include(llvm-native.cmake)
include(native_link.cmake)

# 0.31: keep build-machine paths out of the console binary. __FILE__ and
# std::source_location (assertion and exception texts) otherwise embed the absolute
# host path of every source root, and debug info records the build directory.
# Every target compiled by this tree (RPCS3 components, orbis-compat, the in-tree
# third-party libraries and the Vulkan loader) gets one -ffile-prefix-map per root.
# Clang 18 applies the last matching -fdebug-prefix-map but the longest matching
# -fmacro-prefix-map, so the maps are passed from the shortest prefix to the longest
# one and the most specific root wins for both. Clang compares the prefix without
# regard to case or slash direction on a Windows host. Both the raw and the
# normalized spelling of a root are mapped, because CMake keeps '..' in include
# directories while it normalizes source paths. curl, ffmpeg, LLVM and Mesa are
# built by their own scripts and keep their own flags.
set(_ps4_prefix_map_entries)
set(_ps4_prefix_map_seen)
function(_ps4_add_prefix_map root name)
    if(NOT root)
        return()
    endif()
    file(TO_CMAKE_PATH "${root}" _raw)
    cmake_path(NORMAL_PATH _raw OUTPUT_VARIABLE _normal)
    set(_entries ${_ps4_prefix_map_entries})
    set(_seen ${_ps4_prefix_map_seen})
    foreach(_path IN ITEMS "${_raw}" "${_normal}")
        string(REGEX REPLACE "/+$" "" _path "${_path}")
        cmake_path(HAS_RELATIVE_PART _path _has_relative_part)
        if(NOT _has_relative_part)
            continue() # a drive or filesystem root would rename every path
        endif()
        if(_path MATCHES "=")
            message(WARNING "Cannot map a source root containing '=': ${_path}")
            continue()
        endif()
        set(_key "${_path}")
        if(CMAKE_HOST_WIN32)
            string(TOLOWER "${_key}" _key)
        endif()
        if("${_key}" IN_LIST _seen)
            continue()
        endif()
        list(APPEND _seen "${_key}")
        string(LENGTH "${_path}" _length)
        math(EXPR _order "100000 + ${_length}")
        list(APPEND _entries "${_order}|${_path}=${name}")
    endforeach()
    set(_ps4_prefix_map_entries ${_entries} PARENT_SCOPE)
    set(_ps4_prefix_map_seen ${_seen} PARENT_SCOPE)
endfunction()

_ps4_add_prefix_map("${CMAKE_SOURCE_DIR}" rpcs3)
_ps4_add_prefix_map("${CMAKE_BINARY_DIR}" build)
_ps4_add_prefix_map("${RPCS3_PS4_ORBIS_COMPAT}" orbis-compat)
_ps4_add_prefix_map("${RPCS3_PS4_ABSEIL}" abseil-cpp)
_ps4_add_prefix_map("${RPCS3_PS4_MESA_BUNDLE}" orbis-mesa-bundle)
_ps4_add_prefix_map("${RPCS3_PS4_PORTING_KIT}" orbis-porting-kit)
_ps4_add_prefix_map("${RPCS3_PS4_SDK}" openorbis)
if(RPCS3_PS4_LLVM_BIN)
    # Clang's own headers (lib/clang/<version>/include) live next to its bin folder.
    file(TO_CMAKE_PATH "${RPCS3_PS4_LLVM_BIN}" _ps4_llvm_bin)
    string(REGEX REPLACE "/+$" "" _ps4_llvm_bin "${_ps4_llvm_bin}")
    cmake_path(GET _ps4_llvm_bin PARENT_PATH _ps4_llvm_toolchain_root)
    _ps4_add_prefix_map("${_ps4_llvm_toolchain_root}" llvm-toolchain)
endif()
if(RPCS3_PS4_WITH_LLVM)
    _ps4_add_prefix_map("${LLVM_BUILD_MAIN_SRC_DIR}" llvm)
    _ps4_add_prefix_map("${LLVM_BUILD_BINARY_DIR}" llvm-build)
endif()
# Any other folder beside the checkout (the shortest prefix, so it never wins over
# the roots above).
cmake_path(GET CMAKE_SOURCE_DIR PARENT_PATH _ps4_source_parent)
_ps4_add_prefix_map("${_ps4_source_parent}" src)

list(SORT _ps4_prefix_map_entries)
set(RPCS3_PS4_PREFIX_MAP_FLAGS)
foreach(_ps4_prefix_map_entry IN LISTS _ps4_prefix_map_entries)
    string(SUBSTRING "${_ps4_prefix_map_entry}" 7 -1 _ps4_prefix_map)
    list(APPEND RPCS3_PS4_PREFIX_MAP_FLAGS "-ffile-prefix-map=${_ps4_prefix_map}")
endforeach()

function(_ps4_collect_build_targets directory out_var)
    get_property(_targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
    get_property(_subdirectories DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
    foreach(_subdirectory IN LISTS _subdirectories)
        _ps4_collect_build_targets("${_subdirectory}" _nested)
        list(APPEND _targets ${_nested})
    endforeach()
    set(${out_var} ${_targets} PARENT_SCOPE)
endfunction()
_ps4_collect_build_targets("${CMAKE_CURRENT_SOURCE_DIR}" _ps4_build_targets)
set(_ps4_prefix_mapped_targets 0)
foreach(_ps4_build_target IN LISTS _ps4_build_targets)
    get_target_property(_ps4_build_target_type ${_ps4_build_target} TYPE)
    if(_ps4_build_target_type MATCHES "^(STATIC_LIBRARY|OBJECT_LIBRARY|SHARED_LIBRARY|MODULE_LIBRARY|EXECUTABLE)$")
        target_compile_options(${_ps4_build_target} PRIVATE ${RPCS3_PS4_PREFIX_MAP_FLAGS})
        math(EXPR _ps4_prefix_mapped_targets "${_ps4_prefix_mapped_targets} + 1")
    endif()
endforeach()
if(NOT "rpcs3_ps4_orbis_compat" IN_LIST _ps4_build_targets OR NOT "wolfssl" IN_LIST _ps4_build_targets)
    message(WARNING "Source path mapping did not reach every console target; host paths may remain in the binary")
endif()
list(LENGTH RPCS3_PS4_PREFIX_MAP_FLAGS _ps4_prefix_map_count)
message(STATUS "Source paths mapped for ${_ps4_prefix_mapped_targets} console targets (${_ps4_prefix_map_count} roots)")

file(STRINGS "${RPCS3_PS4_SDK}/include/c++/v1/__config" _RPCS3_PS4_LIBCPP_VERSION
    REGEX "^#define _LIBCPP_VERSION ")
message(STATUS "OpenOrbis C++ runtime: ${_RPCS3_PS4_LIBCPP_VERSION}")
message(STATUS "PS4 components configured; build rpcs3_ps4_elf for the native executable, then run ps4-port/package-native.ps1 for EBOOT/PKG")
