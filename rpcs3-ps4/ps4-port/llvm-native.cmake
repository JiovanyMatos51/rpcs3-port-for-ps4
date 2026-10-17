# Original RPCS3 CPU translators linked with the same pinned LLVM revision.
# Build the libraries with llvm-toolchain.cmake before enabling this option.
option(RPCS3_PS4_WITH_LLVM "Link the original PPU/SPU LLVM recompilers" OFF)
set(RPCS3_PS4_LLVM_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.build-llvm-orbis/lib/cmake/llvm" CACHE PATH
    "LLVM configured with the native OpenOrbis toolchain")
if(RPCS3_PS4_WITH_LLVM)
    find_package(LLVM 22.1 CONFIG REQUIRED PATHS "${RPCS3_PS4_LLVM_DIR}" NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
    if(NOT LLVM_HOST_TRIPLE STREQUAL "x86_64-pc-freebsd12")
        message(FATAL_ERROR "LLVM libraries must target the console, not the build host: ${LLVM_HOST_TRIPLE}")
    endif()
    llvm_map_components_to_libnames(_ps4_llvm_libraries
        X86CodeGen X86AsmParser X86Disassembler Core ExecutionEngine MCJIT Passes)
    add_library(rpcs3_ps4_llvm INTERFACE)
    target_include_directories(rpcs3_ps4_llvm SYSTEM INTERFACE ${LLVM_INCLUDE_DIRS})
    target_compile_definitions(rpcs3_ps4_llvm INTERFACE LLVM_AVAILABLE)
    # LLVM's Triple enum has a member named PS4. The Orbis-specific macros
    # __ORBIS__/RPCS3_PS4 already identify the SDK without that collision.
    target_compile_options(rpcs3_ps4_llvm INTERFACE -UPS4)
    target_link_libraries(rpcs3_ps4_llvm INTERFACE ${_ps4_llvm_libraries})
    target_sources(rpcs3_ps4_cpu PRIVATE ../rpcs3/Emu/Cell/PPUTranslator.cpp)
    # PPU compile workers must unwind their semaphore permits and JIT arenas
    # on allocation/code-generation failures rather than leave boot waiting.
    set_property(SOURCE ../rpcs3/Emu/Cell/PPUThread.cpp APPEND PROPERTY COMPILE_OPTIONS -fexceptions)
    if(NOT LLVM_ENABLE_RTTI)
        # RPCS3's memory manager/cache/listener derive from LLVM classes.
        # Match their library's RTTI mode; the rest of RPCS3 retains its mode.
        set_property(SOURCE ../Utilities/JITLLVM.cpp APPEND PROPERTY COMPILE_OPTIONS -fno-rtti)
    endif()
    target_link_libraries(rpcs3_ps4_frontend PRIVATE asmjit)
    # cfg_root's inline constructors depend on LLVM_AVAILABLE. Apply the same
    # definition to every emulator component to keep their defaults consistent.
    foreach(component rpcs3_ps4_frontend rpcs3_ps4_crypto_extra rpcs3_ps4_vm
        rpcs3_ps4_runtime rpcs3_ps4_utilities rpcs3_ps4_emulation rpcs3_ps4_cpu
        rpcs3_ps4_lv2 rpcs3_ps4_cell_modules rpcs3_ps4_core rpcs3_ps4_loader
        rpcs3_ps4_vulkan rpcs3_ps4_opengl rpcs3_ps4_host_services rpcs3_ps4_rsx
        rpcs3_ps4_np rpcs3_ps4_input rpcs3_ps4_audio)
        target_link_libraries(${component} PRIVATE rpcs3_ps4_llvm)
    endforeach()
    target_link_libraries(rpcs3_ps4_emulator_core PUBLIC rpcs3_ps4_llvm)
    message(STATUS "Original PPU/SPU LLVM recompilers enabled (${LLVM_PACKAGE_VERSION}, ${LLVM_HOST_TRIPLE})")
endif()
