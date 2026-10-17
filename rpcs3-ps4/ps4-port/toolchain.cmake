# OpenOrbis cross-compilation toolchain. Pass -DRPCS3_PS4_SDK=<OpenOrbis SDK>
# and -DRPCS3_PS4_LLVM_BIN=<directory containing clang++.exe and ld.lld.exe>.
set(CMAKE_SYSTEM_NAME FreeBSD)
set(CMAKE_SYSTEM_PROCESSOR x86_64)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(RPCS3_PS4_SDK "$ENV{OO_PS4_TOOLCHAIN}" CACHE PATH "OpenOrbis PS4Toolchain root")
set(RPCS3_PS4_LLVM_BIN "$ENV{RPCS3_PS4_LLVM_BIN}" CACHE PATH "LLVM bin directory for the host")
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES RPCS3_PS4_SDK RPCS3_PS4_LLVM_BIN)
list(APPEND CMAKE_TRY_COMPILE_PLATFORM_VARIABLES RPCS3_PS4_EXECUTABLE_CHECKS)

if(NOT EXISTS "${RPCS3_PS4_SDK}/include/orbis" OR
   NOT EXISTS "${RPCS3_PS4_SDK}/lib/libc++.a" OR
   NOT EXISTS "${RPCS3_PS4_SDK}/link.x")
    message(FATAL_ERROR "RPCS3_PS4_SDK must point at a complete OpenOrbis PS4Toolchain directory")
endif()

if(NOT RPCS3_PS4_LLVM_BIN)
    find_program(_RPCS3_PS4_CLANG clang++ REQUIRED)
    get_filename_component(RPCS3_PS4_LLVM_BIN "${_RPCS3_PS4_CLANG}" DIRECTORY)
endif()

if(WIN32)
    set(_RPCS3_PS4_EXE .exe)
endif()

set(CMAKE_C_COMPILER "${RPCS3_PS4_LLVM_BIN}/clang${_RPCS3_PS4_EXE}")
set(CMAKE_CXX_COMPILER "${RPCS3_PS4_LLVM_BIN}/clang++${_RPCS3_PS4_EXE}")
set(CMAKE_AR "${RPCS3_PS4_LLVM_BIN}/llvm-ar${_RPCS3_PS4_EXE}")
set(CMAKE_RANLIB "${RPCS3_PS4_LLVM_BIN}/llvm-ranlib${_RPCS3_PS4_EXE}")
set(RPCS3_PS4_LLD "${RPCS3_PS4_LLVM_BIN}/ld.lld${_RPCS3_PS4_EXE}" CACHE FILEPATH "LLVM ELF linker")

foreach(_tool CMAKE_C_COMPILER CMAKE_CXX_COMPILER CMAKE_AR CMAKE_RANLIB RPCS3_PS4_LLD)
    if(NOT EXISTS "${${_tool}}")
        message(FATAL_ERROR "Missing ${_tool}: ${${_tool}}")
    endif()
endforeach()

set(CMAKE_C_COMPILER_TARGET x86_64-pc-freebsd12-elf)
set(CMAKE_CXX_COMPILER_TARGET x86_64-pc-freebsd12-elf)

# The SDK libc++ wraps C headers, so its include directory must precede the
# SDK C include directory in C++ builds. Otherwise math overloads resolve to
# the integer C functions and silently truncate floating-point values.
# Restrict include lookup to the PS4 SDK, never host C++ headers.
file(TO_CMAKE_PATH "${RPCS3_PS4_SDK}" _RPCS3_PS4_SDK_POSIX)
set(_RPCS3_PS4_COMMON_FLAGS
    "-fPIC -funwind-tables -D_GNU_SOURCE -D_BSD_SOURCE=1 -D_POSIX_C_SOURCE=200809L -D__ORBIS__ -D__PS4__ -DPS4 -DORBIS=1 -DRPCS3_PS4=1")
set(CMAKE_C_FLAGS_INIT
    "${_RPCS3_PS4_COMMON_FLAGS} -isystem \"${_RPCS3_PS4_SDK_POSIX}/include\"")
set(CMAKE_CXX_FLAGS_INIT
    "${_RPCS3_PS4_COMMON_FLAGS} -nostdinc++ -isystem \"${_RPCS3_PS4_SDK_POSIX}/include/c++/v1\" -isystem \"${_RPCS3_PS4_SDK_POSIX}/include\"")

set(CMAKE_FIND_ROOT_PATH "${RPCS3_PS4_SDK}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Executable feature checks need the console startup object and SDK runtime.
# Keep this opt-in for standalone dependencies such as curl. try_compile
# reloads the toolchain but does not forward CMAKE_C_STANDARD_LIBRARIES.
if(RPCS3_PS4_EXECUTABLE_CHECKS)
    set(CMAKE_C_STANDARD_LIBRARIES
        "\"${RPCS3_PS4_SDK}/lib/crt1.o\" -L\"${RPCS3_PS4_SDK}/lib\" -lc -lm -lkernel")
endif()
