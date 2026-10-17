# The RPCS3 ffmpeg submodule contains headers and PC binaries, not the full
# source. build-ffmpeg.ps1 checks out n8.1.1 and validates its header versions
# before compiling the actual codec implementations for the console ABI.
set(_ps4_ffmpeg_source "${CMAKE_CURRENT_SOURCE_DIR}/deps/ffmpeg")
set(_ps4_ffmpeg_archives)
foreach(component avformat avcodec swscale swresample avutil)
    set(archive "${_ps4_ffmpeg_source}/lib${component}/lib${component}.a")
    list(APPEND _ps4_ffmpeg_archives "${archive}")
    add_library(rpcs3_ps4_${component} STATIC IMPORTED GLOBAL)
    set_target_properties(rpcs3_ps4_${component} PROPERTIES
        IMPORTED_LOCATION "${archive}"
        INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_SOURCE_DIR}/3rdparty/ffmpeg/include")
endforeach()
find_program(_ps4_powershell NAMES pwsh powershell REQUIRED)
add_custom_command(OUTPUT ${_ps4_ffmpeg_archives}
    COMMAND "${_ps4_powershell}" -NoProfile -File
        "${CMAKE_CURRENT_SOURCE_DIR}/build-ffmpeg.ps1"
        -Sdk "${RPCS3_PS4_SDK}" -Llvm "${RPCS3_PS4_LLVM_BIN}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/build-ffmpeg.ps1"
    VERBATIM)
add_custom_target(rpcs3_ps4_ffmpeg_build DEPENDS ${_ps4_ffmpeg_archives})
add_library(rpcs3_ps4_ffmpeg INTERFACE)
target_link_libraries(rpcs3_ps4_ffmpeg INTERFACE
    rpcs3_ps4_avformat rpcs3_ps4_avcodec rpcs3_ps4_swscale
    rpcs3_ps4_swresample rpcs3_ps4_avutil)
add_dependencies(rpcs3_ps4_ffmpeg rpcs3_ps4_ffmpeg_build)
