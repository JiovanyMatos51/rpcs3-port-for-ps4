# Strict native link of the console ELF (no unresolved symbols allowed). It is
# excluded from the default build; package-native.ps1 builds it and converts it
# into the EBOOT/PKG.
set(_ps4_native_linker "${RPCS3_PS4_LLD}")
if(CMAKE_HOST_WIN32)
    set(_ps4_linker_wrapper_source "${CMAKE_CURRENT_BINARY_DIR}/linker_wrapper.c")
    configure_file(linker_wrapper.c.in "${_ps4_linker_wrapper_source}" @ONLY)
    set(_ps4_native_linker "${CMAKE_CURRENT_BINARY_DIR}/rpcs3-ps4-ld.exe")
    add_custom_command(OUTPUT "${_ps4_native_linker}"
        COMMAND "${RPCS3_PS4_LLVM_BIN}/clang.exe" -std=c11 -O2
            -Wall -Wextra -Werror "${_ps4_linker_wrapper_source}"
            -o "${_ps4_native_linker}"
        DEPENDS "${_ps4_linker_wrapper_source}"
        VERBATIM)
    add_custom_target(rpcs3_ps4_linker DEPENDS "${_ps4_native_linker}")
    add_dependencies(rpcs3_ps4_curl_build rpcs3_ps4_linker)
    add_dependencies(rpcs3_ps4_ffmpeg_build rpcs3_ps4_linker)
endif()
set(_ps4_libc16_dir "${CMAKE_CURRENT_BINARY_DIR}/libc16")
file(MAKE_DIRECTORY "${_ps4_libc16_dir}")
set(_ps4_libc16_members regcomp regexec vfscanf mbtowc mbrtowc)
set(_ps4_libc16_inputs)
foreach(member IN LISTS _ps4_libc16_members)
    list(APPEND _ps4_libc16_inputs "${_ps4_libc16_dir}/${member}.lo")
endforeach()
add_custom_command(OUTPUT ${_ps4_libc16_inputs}
    COMMAND "${CMAKE_AR}" x "${RPCS3_PS4_SDK}/lib/libc.a"
        regcomp.lo regexec.lo vfscanf.lo mbtowc.lo mbrtowc.lo
    WORKING_DIRECTORY "${_ps4_libc16_dir}"
    DEPENDS "${RPCS3_PS4_SDK}/lib/libc.a"
    VERBATIM)

# The overlay uses 32-bit wchar_t. Keep the SDK's five 16-bit callers and
# conversion functions together, as required by orbis-compat/build.sh.
set(_ps4_libc16_objects)
set(_ps4_objcopy "${RPCS3_PS4_LLVM_BIN}/llvm-objcopy${_RPCS3_PS4_EXE}")
if(NOT EXISTS "${_ps4_objcopy}")
    message(FATAL_ERROR "Native PS4 link requires llvm-objcopy")
endif()
foreach(member IN LISTS _ps4_libc16_members)
    if(member MATCHES "^(regcomp|regexec|mbtowc)$")
        set(rename "mbtowc=__orbis_libc16_mbtowc")
    else()
        set(rename "mbrtowc=__orbis_libc16_mbrtowc")
    endif()
    set(object "${_ps4_libc16_dir}/libc16_${member}.o")
    add_custom_command(OUTPUT "${object}"
        COMMAND "${_ps4_objcopy}" --redefine-sym "${rename}"
            "${_ps4_libc16_dir}/${member}.lo" "${object}"
        DEPENDS "${_ps4_libc16_dir}/${member}.lo"
        VERBATIM)
    set_source_files_properties("${object}" PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
    list(APPEND _ps4_libc16_objects "${object}")
endforeach()

add_executable(rpcs3_ps4_elf EXCLUDE_FROM_ALL
    $<TARGET_OBJECTS:rpcs3_ps4_frontend> ${_ps4_libc16_objects})
set_target_properties(rpcs3_ps4_elf PROPERTIES
    LINKER_LANGUAGE CXX OUTPUT_NAME rpcs3_ps4 SUFFIX .elf SKIP_BUILD_RPATH TRUE
    LINK_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/link-data-tail.x")
target_link_options(rpcs3_ps4_elf PRIVATE
    "-fuse-ld=${_ps4_native_linker}" -nostdlib -pie
    -Wl,-m,elf_x86_64
    "-Wl,-T,${RPCS3_PS4_SDK}/link.x" "-Wl,-T,${CMAKE_CURRENT_LIST_DIR}/link-data-tail.x" -Wl,--eh-frame-hdr
    -Wl,--no-dynamic-linker -Wl,--error-limit=0)
target_link_directories(rpcs3_ps4_elf PRIVATE "${RPCS3_PS4_SDK}/lib")
add_library(rpcs3_ps4_sdk_runtime INTERFACE)
target_link_libraries(rpcs3_ps4_sdk_runtime INTERFACE
    c++ c++abi unwind c m kernel
    ScePad SceUserService SceAudioOut SceVideoOut SceGnmDriver SceSysmodule SceNet SceRandom
    # libSceSystemService.so stub: sceSystemServiceLoadExec/GetStatus/ReceiveEvent
    # used by rpcs3/ps4_fatal.cpp to leave the application without _exit (SIGSYS).
    SceSystemService
    # 0.31: network state for RPCN (sceNetCtlInit/Term/GetState/GetInfo/GetResult/
    # RegisterCallback/CheckCallback/GetNatInfo) and the on-screen keyboard used for
    # RPCN credentials (sceImeDialogInit/GetStatus/GetResult/Term/Abort/ForceClose,
    # sceCommonDialogInitialize/IsUsed). Callers load each module through
    # libSceSysmodule before its first use.
    SceNetCtl SceImeDialog SceCommonDialog)
# RADV supplies its C11 mutex functions. Process the whole driver before libc
# so libc does not extract conflicting mtx_lock/mtx_timedlock implementations.
target_link_libraries(ps4-vkloader INTERFACE rpcs3_ps4_sdk_runtime)
target_link_libraries(rpcs3_ps4_elf PRIVATE
    rpcs3_ps4_emulator_core
    "$<LINK_LIBRARY:WHOLE_ARCHIVE,rpcs3_ps4_orbis_compat>"
    "${RPCS3_PS4_SDK}/lib/crt1.o")
if(TARGET rpcs3_ps4_linker)
    add_dependencies(rpcs3_ps4_elf rpcs3_ps4_linker)
endif()
