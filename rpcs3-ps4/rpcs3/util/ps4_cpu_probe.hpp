#pragma once

// 0.29: one-time CPU clock probe. The package declares no PS4 Pro support (param.sfo ATTRIBUTE=0), so on a
// PS4 Pro the CPU most likely runs at the base clock (~1.6 GHz) unless the system's Boost Mode is on (~2.13 GHz:
// up to +33 % on every CPU-bound thread). Do not change param.sfo ATTRIBUTE to get Pro mode instead: Pro (NEO)
// mode changes the GPU configuration orbis-drm assumes.
// Console-only, header-only (frontend TUs: ps4_launcher.cpp shows it, ps4_main.cpp may log it at boot).

#include "ps4_i18n.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <utility>

namespace utils::ps4
{
namespace cpu_probe_detail
{
// libkernel. All five are exported by the OpenOrbis lib/libkernel.so stub (llvm-nm -D, 0.29). Bound under
// private names (asm labels) so this header never collides with <orbis/libkernel.h> declarations. The mode
// queries are weak: what a homebrew title gets from them on FW 13.02 is unverified; a missing one reads as -1.
extern "C" std::uint64_t ps4_probe_process_time() __asm__("sceKernelGetProcessTime");
extern "C" std::uint64_t ps4_probe_tsc_frequency() __asm__("sceKernelGetTscFrequency");
extern "C" std::int32_t ps4_probe_cpumode() __asm__("sceKernelGetCpumode") __attribute__((weak));
extern "C" std::int32_t ps4_probe_is_neo_mode() __asm__("sceKernelIsNeoMode") __attribute__((weak));
extern "C" std::int32_t ps4_probe_has_neo_mode() __asm__("sceKernelHasNeoMode") __attribute__((weak));
} // namespace cpu_probe_detail

struct cpu_probe_result
{
    double mhz = 0;              // measured core clock; 0 = the probe could not time itself
    std::uint64_t best_us = 0;   // fastest of the timed runs
    std::uint64_t tsc_hz = 0;    // sceKernelGetTscFrequency()
    std::int32_t cpumode = -1;   // sceKernelGetCpumode(); -1 = not exported
    std::int32_t is_neo = -1;    // sceKernelIsNeoMode(); -1 = not exported
    std::int32_t has_neo = -1;   // sceKernelHasNeoMode(); -1 = not exported
};

inline cpu_probe_result run_cpu_probe() noexcept
{
    using namespace cpu_probe_detail;
    // A chain of dependent 1-cycle adds retires one add per core clock on Jaguar, whatever the TSC rate. Eight
    // adds per iteration keep it latency-bound (11 instructions on 2 ALUs with 2-wide decode = 5.5 cycles < 8).
    // 4 M iterations = 32 M cycles = ~20 ms at 1.6 GHz; the best of 5 runs filters preemption (~100 ms in all,
    // once per process, on the calling thread).
    constexpr std::uint64_t iterations = 4'000'000;
    constexpr int runs = 5;
    std::uint64_t best = ~std::uint64_t{0};
    for (int run = 0; run < runs; run++)
    {
        std::uint64_t x = static_cast<std::uint64_t>(run);
        const std::uint64_t t0 = ps4_probe_process_time();
        for (std::uint64_t i = 0; i < iterations; i++)
        {
            __asm__ volatile("add $1, %0\n\tadd $1, %0\n\tadd $1, %0\n\tadd $1, %0\n\t"
                             "add $1, %0\n\tadd $1, %0\n\tadd $1, %0\n\tadd $1, %0" : "+r"(x));
        }
        const std::uint64_t t1 = ps4_probe_process_time();
        if (t1 > t0) best = std::min(best, t1 - t0);
    }
    cpu_probe_result result;
    if (best != ~std::uint64_t{0})
    {
        result.best_us = best;
        result.mhz = double(iterations * 8) / double(best); // cycles per microsecond = MHz
    }
    result.tsc_hz = ps4_probe_tsc_frequency();
    if (&ps4_probe_cpumode != nullptr) result.cpumode = ps4_probe_cpumode();
    if (&ps4_probe_is_neo_mode != nullptr) result.is_neo = ps4_probe_is_neo_mode();
    if (&ps4_probe_has_neo_mode != nullptr) result.has_neo = ps4_probe_has_neo_mode();
    return result;
}

// Cached: the first call runs the ~100 ms probe (thread-safe static init), later calls are free.
inline const cpu_probe_result& cpu_probe() noexcept
{
    static const cpu_probe_result s_result = run_cpu_probe();
    return s_result;
}

// Base PS4/PS4 Pro clock 1.6 GHz, PS4 Pro Boost/Pro mode 2.13 GHz; anything between is reported as such.
inline bool cpu_clock_is_base(const cpu_probe_result& r) noexcept { return r.mhz > 0 && r.mhz < 1800; }
inline bool cpu_clock_is_boost(const cpu_probe_result& r) noexcept { return r.mhz >= 1900; }

// "CPU ~1600 MHz (normal)" / "CPU ~2130 MHz (Boost)" for the XMB (interface language).
inline std::string cpu_clock_text()
{
    using i18n::str;
    const cpu_probe_result& r = cpu_probe();
    if (r.mhz <= 0) return i18n::tr(str::CPU_CPU_CLOCK_NAO_MEDIDO);
    const str mode = cpu_clock_is_base(r) ? str::CPU_NORMAL : !cpu_clock_is_boost(r) ? str::CPU_INTERMEDIARIO : r.is_neo == 1 ? str::CPU_MODO_PS4_PRO : str::CPU_BOOST;
    return i18n::trf(str::CPU_CPU_MHZ, {std::lround(r.mhz / 10) * 10, mode});
}

// Boost Mode would still give ~33 % more CPU: the measured clock is the base ~1.6 GHz. Shown whatever sceKernelHasNeoMode
// says, because its meaning for a title without Pro support is unverified (on a base PS4 the hint is harmless).
inline bool cpu_boost_hint()
{
    return cpu_clock_is_base(cpu_probe());
}

// The Boost Mode hint in the interface language where a C string is expected, or appended to a std::string (the
// operators live in their own namespace, found by argument-dependent lookup only)
namespace cpu_hint_detail
{
struct boost_hint
{
    operator const char*() const noexcept { return i18n::tr(i18n::str::CPU_ATIVE_MODO_TURBO_BOOST_MODE); }
};
inline std::string operator+(const std::string& text, boost_hint hint) { return text + static_cast<const char*>(hint); }
inline std::string operator+(std::string&& text, boost_hint hint) { return std::move(text) + static_cast<const char*>(hint); }
} // namespace cpu_hint_detail
inline constexpr cpu_hint_detail::boost_hint cpu_boost_hint_text{};

// One ASCII line for startup.log (the frontend logs it at boot).
inline std::string cpu_probe_log_line()
{
    const cpu_probe_result& r = cpu_probe();
    const char* mode = r.mhz <= 0 ? "not measured" : cpu_clock_is_base(r) ? "base clock" : !cpu_clock_is_boost(r) ? "intermediate" : "Boost/Pro clock";
    char text[384];
    std::snprintf(text, sizeof(text), "CPU clock probe: ~%.0f MHz (%s; 32M dependent adds, best of 5 = %llu us); TSC %llu Hz; "
        "sceKernelGetCpumode=%d sceKernelIsNeoMode=%d sceKernelHasNeoMode=%d (-1 = not exported); "
        "~1600 MHz = base clock (enable Boost Mode), ~2130 MHz = PS4 Pro Boost/Pro mode",
        r.mhz, mode, static_cast<unsigned long long>(r.best_us), static_cast<unsigned long long>(r.tsc_hz),
        static_cast<int>(r.cpumode), static_cast<int>(r.is_neo), static_cast<int>(r.has_neo));
    return text;
}
} // namespace utils::ps4
