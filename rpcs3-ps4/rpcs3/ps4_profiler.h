#pragma once

#include <cstdint>

// 0.29: low-overhead sampling profiler for the console build (rpcs3/ps4_profiler.cpp).
// It creates no thread, mutex, condvar or ScePthread attr (thread creation is what fails on this console): it is
// driven from the ps4_main loop and uses the live-thread registry of Utilities/Thread.h (ps4_thread_registry).
//
// Light mode (default, always on): 45 s after the guest starts running and then every 60 s, a CPU census of every
// registered RPCS3 thread (getrusage(RUSAGE_THREAD) taken inside each thread through native signal 31), followed by
// a 1 s burst of ~1 kHz RIP samples on the 4 busiest threads (>= 5% CPU). Light mode never signals the RSX thread
// family (the RSX thread and every thread named 'RSX...'/'rsx...'/'Vulkan...': they can block inside the
// Mesa/orbis-drm winsys, whose EINTR handling is unverified); their CPU time is not in the census then.
// Full mode: /data/rpcs3/profile.flag exists. Optional key=value lines (defaults in brackets):
//   period=15 first=20 burst_ms=2000 threads=12 top=20 min=1 lines=48 rsx=1 cpuusage=0
//   (burst_ms == period*1000 samples continuously; cpuusage=1 also probes sceKernelGetCpuUsage for threads outside
//   the registry such as Mesa and audio, validated against getrusage first).
// Kill switch: /data/rpcs3/no-profile.flag. A boot self-test (signal 31 to the main thread) turns the profiler off
// when delivery, the ucontext layout or getrusage misbehave.
// Output: "PROF ..." lines appended to /data/rpcs3/startup.log; "elf+0x..." offsets are symbolizable with
// ps4-port/tools/prof_symbolize.py (llvm-symbolizer --obj=rpcs3_ps4.elf of the same build).

// Main thread only (ps4_main.cpp), once, after Emu.Init. Registers the main thread in ps4_thread_registry.
void ps4_profiler_init() noexcept;

// Main thread only, every main-loop iteration. Returns true while the profiler needs ~1 ms ticks (a census waiting
// for replies, or a burst in progress); false lets the main loop sleep longer.
bool ps4_profiler_tick(std::uint64_t now_us, bool guest_running) noexcept;

// Installs once (and reads back) the shared handler of native signal 31, also used by ps4_sample_thread_stack
// (ps4_fatal.cpp, STALL dumps). Nothing may send signal 31 unless this returned true: its default action ends the
// process.
bool ps4_profiler_install_signal() noexcept;
