#include "stdafx.h"
#include "sys_time.h"

#include "sys_process.h"
#include "Emu/system_config.h"
#include "Emu/Cell/ErrorCodes.h"
#include "Emu/Cell/timers.hpp"
#include "util/tsc.hpp"

#include "util/sysinfo.hpp"

u64 g_timebase_offs{};
static u64 systemtime_offset;

#ifndef __linux__
#include "util/asm.hpp"
#endif

#ifdef _WIN32

#include <Windows.h>

struct time_aux_info_t
{
	u64 perf_freq;
	u64 start_time;
	u64 start_ftime; // time in 100ns units since Epoch
};

// Initialize time-related values
const auto s_time_aux_info = []() -> time_aux_info_t
{
	LARGE_INTEGER freq;
	if (!QueryPerformanceFrequency(&freq))
	{
		MessageBox(nullptr, L"Your hardware doesn't support a high-resolution performance counter", L"Error", MB_OK | MB_ICONERROR);
		return {};
	}

	LARGE_INTEGER start;
	QueryPerformanceCounter(&start); // get time in 1/perf_freq units from RDTSC

	FILETIME ftime;
	GetSystemTimeAsFileTime(&ftime); // get time in 100ns units since January 1, 1601 (UTC)

	time_aux_info_t result;
	result.perf_freq   = freq.QuadPart;
	result.start_time  = start.QuadPart;
	result.start_ftime = (ftime.dwLowDateTime | static_cast<u64>(ftime.dwHighDateTime) << 32) - 116444736000000000;

	return result;
}();

#elif __APPLE__

// XXX only supports a single timer
#if !defined(HAVE_CLOCK_GETTIME)
#define TIMER_ABSTIME -1
// The opengroup spec isn't clear on the mapping from REALTIME to CALENDAR being appropriate or not.
// http://pubs.opengroup.org/onlinepubs/009695299/basedefs/time.h.html
#define CLOCK_REALTIME  1 // #define CALENDAR_CLOCK 1 from mach/clock_types.h
#define CLOCK_MONOTONIC 0 // #define SYSTEM_CLOCK 0

// the mach kernel uses struct mach_timespec, so struct timespec is loaded from <sys/_types/_timespec.h> for compatability
// struct timespec { time_t tv_sec; long tv_nsec; };

#include <sys/types.h>
#include <sys/_types/_timespec.h>
#include <mach/mach.h>
#include <mach/clock.h>
#include <mach/mach_time.h>
#undef CPU_STATE_MAX

#define MT_NANO (+1.0E-9)
#define MT_GIGA UINT64_C(1000000000)

// TODO create a list of timers,
static double mt_timebase = 0.0;
static u64 mt_timestart = 0;

static int clock_gettime(int clk_id, struct timespec* tp)
{
	kern_return_t retval = KERN_SUCCESS;
	if (clk_id == TIMER_ABSTIME)
	{
		if (!mt_timestart)
		{
			// only one timer, initilized on the first call to the TIMER
			mach_timebase_info_data_t tb = {0};
			mach_timebase_info(&tb);
			mt_timebase = tb.numer;
			mt_timebase /= tb.denom;
			mt_timestart = mach_absolute_time();
		}

		double diff = (mach_absolute_time() - mt_timestart) * mt_timebase;
		tp->tv_sec  = diff * MT_NANO;
		tp->tv_nsec = diff - (tp->tv_sec * MT_GIGA);
	}
	else // other clk_ids are mapped to the coresponding mach clock_service
	{
		clock_serv_t cclock;
		mach_timespec_t mts;

		host_get_clock_service(mach_host_self(), clk_id, &cclock);
		retval = clock_get_time(cclock, &mts);
		mach_port_deallocate(mach_task_self(), cclock);

		tp->tv_sec  = mts.tv_sec;
		tp->tv_nsec = mts.tv_nsec;
	}

	return retval;
}
#endif

#endif

#ifndef _WIN32

#include <exception>
#include <sys/time.h>

static struct timespec start_time = []()
{
	struct timespec ts;

	if (::clock_gettime(CLOCK_REALTIME, &ts) != 0)
	{
		// Fatal error
		std::terminate();
	}

	tzset();

	return ts;
}();

#endif

LOG_CHANNEL(sys_time);

static constexpr u64 g_timebase_freq = /*79800000*/ 80000000ull; // 80 Mhz

#ifdef RPCS3_PS4
namespace
{
	// Exact unsigned 64-bit division by a divisor fixed at run time, without DIV (tens of cycles on Jaguar; the clock
	// is read ~300k times per second by the RSX timers alone). magic = floor((2^64 - 1) / d) lies in [2^64/d - 1, 2^64/d],
	// so umulh(n, magic) is floor(n / d) or one less for every n, and one compare of the remainder corrects it.
	struct ps4_tsc_divider
	{
		u64 div;
		u64 magic;

		explicit ps4_tsc_divider(u64 d) noexcept
			: div(d)
			, magic(~0ull / d)
		{
		}

		// floor(n / div); 'rem' receives n % div
		u64 divide(u64 n, u64& rem) const noexcept
		{
			u64 q = utils::umulh64(n, magic);
			u64 r = n - q * div;

			if (r >= div)
			{
				q++;
				r -= div;
			}

			rem = r;
			return q;
		}
	};

	// tsc / freq * mul + tsc % freq * mul / freq, bit-identical to the generic expression (same u64 wrap-around).
	// The TSC frequency is decided once during static initialization and never changes after that.
	u64 ps4_tsc_scale(u64 tsc, u64 freq, u64 mul) noexcept
	{
		static const ps4_tsc_divider s_div(freq);

		if (s_div.div != freq) [[unlikely]]
		{
			return tsc / freq * mul + tsc % freq * mul / freq;
		}

		u64 rem = 0;
		const u64 whole = s_div.divide(tsc, rem);
		u64 unused = 0;
		return whole * mul + s_div.divide(rem * mul, unused);
	}
}
#endif

// Convert time is microseconds to timebased time
u64 convert_to_timebased_time(u64 time)
{
	const u64 result = time * (g_timebase_freq / 1000000ull) * g_cfg.core.clocks_scale / 100u;
	ensure(result >= g_timebase_offs);
	return result - g_timebase_offs;
}

u64 get_timebased_time()
{
	if (u64 freq = utils::get_tsc_freq())
	{
		const u64 tsc = utils::get_tsc();

#if _MSC_VER
		const u64 result = static_cast<u64>(u128_from_mul(tsc, g_timebase_freq) / freq) * g_cfg.core.clocks_scale / 100u;
#elif defined(RPCS3_PS4)
		const u64 result = ps4_tsc_scale(tsc, freq, g_timebase_freq) * g_cfg.core.clocks_scale / 100u;
#else
		const u64 result = (tsc / freq * g_timebase_freq + tsc % freq * g_timebase_freq / freq) * g_cfg.core.clocks_scale / 100u;
#endif
		return result - g_timebase_offs;
	}

	while (true)
	{
#ifdef _WIN32
		LARGE_INTEGER count;
		ensure(QueryPerformanceCounter(&count));

		const u64 time = count.QuadPart;
		const u64 freq = s_time_aux_info.perf_freq;

#if _MSC_VER
		const u64 result = static_cast<u64>(u128_from_mul(time * g_cfg.core.clocks_scale, g_timebase_freq) / freq / 100u);
#else
		const u64 result = (time / freq * g_timebase_freq + time % freq * g_timebase_freq / freq) * g_cfg.core.clocks_scale / 100u;
#endif
#else
		struct timespec ts;
		ensure(::clock_gettime(CLOCK_MONOTONIC, &ts) == 0);

		const u64 result = (static_cast<u64>(ts.tv_sec) * g_timebase_freq + static_cast<u64>(ts.tv_nsec) * g_timebase_freq / 1000000000ull) * g_cfg.core.clocks_scale / 100u;
#endif
		if (result) return result - g_timebase_offs;
	}
}

// Add an offset to get_timebased_time to avoid leaking PC's uptime into the game
// As if PS3 starts at value 0 (base time) when the game boots
// If none-zero arg is specified it will become the base time (for savestates)
void initialize_timebased_time(u64 timebased_init, bool reset)
{
	g_timebase_offs = 0;

	if (reset)
	{
		// We simply want to zero-out these values
		systemtime_offset = 0;
		return;
	}

	const u64 current = get_timebased_time();
	timebased_init = current - timebased_init;

	g_timebase_offs = timebased_init;
	systemtime_offset = timebased_init / (g_timebase_freq / 1000000);
}

// Returns some relative time in microseconds, don't change this fact
u64 get_system_time()
{
	if (u64 freq = utils::get_tsc_freq())
	{
		const u64 tsc = utils::get_tsc();

#if _MSC_VER
		const u64 result = static_cast<u64>(u128_from_mul(tsc, 1000000ull) / freq);
#elif defined(RPCS3_PS4)
		const u64 result = ps4_tsc_scale(tsc, freq, 1000000ull);
#else
		const u64 result = (tsc / freq * 1000000ull + tsc % freq * 1000000ull / freq);
#endif
		return result;
	}

	while (true)
	{
#ifdef _WIN32
		LARGE_INTEGER count;
		ensure(QueryPerformanceCounter(&count));

		const u64 time = count.QuadPart;
		const u64 freq = s_time_aux_info.perf_freq;

#if _MSC_VER
		const u64 result = static_cast<u64>(u128_from_mul(time, 1000000ull) / freq);
#else
		const u64 result = time / freq * 1000000ull + (time % freq) * 1000000ull / freq;
#endif
#else
		struct timespec ts;
		ensure(::clock_gettime(CLOCK_MONOTONIC, &ts) == 0);

		const u64 result = static_cast<u64>(ts.tv_sec) * 1000000ull + static_cast<u64>(ts.tv_nsec) / 1000u;
#endif

		if (result) return result;
	}
}

// As get_system_time but obeys Clocks scaling setting
u64 get_guest_system_time(u64 time)
{
	const u64 result = (time != umax ? time : get_system_time()) * g_cfg.core.clocks_scale / 100;
	return result - systemtime_offset;
}

#ifdef RPCS3_PS4
// libSceSystemService, already linked for the frontend (ps4-port/native_link.cmake)
extern "C" s32 sceSystemServiceParamGetInt(s32 param_id, s32* value);

// Frontend object (ps4_launcher.cpp), always linked.
void ps4_startup_checkpoint(const char* stage);

// 0.33: musl's gettimeofday() never fills struct timezone and the sandbox has no zoneinfo, so the generic path always
// reported UTC without DST: guest local time (cellRtc local clock, RFC 2822/3339 strings) was UTC while PC uses the
// host zone. The host zone here is the console setting: TIME_ZONE (assumed minutes east of UTC like the PS3 parameter,
// e.g. -180 for UTC-3) and SUMMERTIME (the DST switch, 0/1). Cached because cellRtc asks on every local time
// conversion; re-read once a minute so a settings change is picked up, as the Windows build does. A failed or
// out-of-range read keeps the last good value (UTC until the first one). A raw result is logged when it changes.
static std::pair<s32, s32> ps4_get_host_timezone()
{
	constexpr s32 param_time_zone = 4;  // ORBIS_SYSTEM_SERVICE_PARAM_ID_TIME_ZONE
	constexpr s32 param_summertime = 5; // ORBIS_SYSTEM_SERVICE_PARAM_ID_SUMMERTIME
	constexpr u64 refresh_us = 60'000'000;

	static atomic_t<u64> s_last_query{umax};
	static atomic_t<u32> s_value{0};     // u16 timezone | u16 summertime << 16
	static atomic_t<u64> s_logged{umax}; // last raw result written to the logs

	const u64 now = get_system_time();
	const u64 last = s_last_query.observe();

	// Unsigned difference: a time base that went backwards (rest mode) also forces a re-read
	if (last == umax || now - last >= refresh_us)
	{
		// Racing callers may query twice; both store the same result
		s_last_query.release(now);

		s32 raw_tz = 0;
		s32 raw_dst = 0;
		const s32 rc_tz = sceSystemServiceParamGetInt(param_time_zone, &raw_tz);
		const s32 rc_dst = sceSystemServiceParamGetInt(param_summertime, &raw_dst);

		// Real UTC offsets span -12:00..+14:00; anything else means other units or sign and must not reach the guest
		const bool ok = rc_tz == 0 && rc_dst == 0 && raw_tz >= -12 * 60 && raw_tz <= 14 * 60;

		if (ok)
		{
			s_value.release(u32{static_cast<u16>(raw_tz)} | (raw_dst ? 60u : 0u) << 16);
		}

		const u64 key = u64{static_cast<u32>(raw_tz)} << 32 | u64{static_cast<u16>(raw_dst)} << 16 | (rc_tz ? 1u : 0u) | (rc_dst ? 2u : 0u);

		if (s_logged.exchange(key) != key)
		{
			const u32 value = s_value.observe();
			const std::string line = fmt::format("sys_time: console time zone: TIME_ZONE=%d (rc=0x%x) SUMMERTIME=%d (rc=0x%x) -> %s timezone=%d min summertime=%d min",
				raw_tz, static_cast<u32>(rc_tz), raw_dst, static_cast<u32>(rc_dst), ok ? "guest" : "unusable, guest keeps", static_cast<s16>(value), static_cast<s16>(value >> 16));

			if (ok)
			{
				sys_time.notice("%s", line);
			}
			else
			{
				sys_time.warning("%s", line);
			}

			ps4_startup_checkpoint(line.c_str());
		}
	}

	const u32 value = s_value.observe();
	return {static_cast<s16>(value), static_cast<s16>(value >> 16)};
}
#endif

// Functions
error_code sys_time_set_timezone(s32 timezone, s32 summertime)
{
	sys_time.trace("sys_time_set_timezone(timezone=0x%x, summertime=0x%x)", timezone, summertime);

	if (!g_ps3_process_info.has_root_perm())
	{
		return CELL_ENOSYS;
	}

	return CELL_OK;
}

error_code sys_time_get_timezone(vm::ptr<s32> timezone, vm::ptr<s32> summertime)
{
	sys_time.trace("sys_time_get_timezone(timezone=*0x%x, summertime=*0x%x)", timezone, summertime);

#ifdef _WIN32
	TIME_ZONE_INFORMATION tz{};
	switch (GetTimeZoneInformation(&tz))
	{
	case TIME_ZONE_ID_UNKNOWN:
	{
		*timezone = -tz.Bias;
		*summertime = 0;
		break;
	}
	case TIME_ZONE_ID_STANDARD:
	{
		*timezone = -tz.Bias;
		*summertime = -tz.StandardBias;

		if (tz.StandardBias)
		{
			sys_time.error("Unexpected timezone bias (base=%d, std=%d, daylight=%d)", tz.Bias, tz.StandardBias, tz.DaylightBias);
		}
		break;
	}
	case TIME_ZONE_ID_DAYLIGHT:
	{
		*timezone = -tz.Bias;
		*summertime = -tz.DaylightBias;
		break;
	}
	default:
	{
		ensure(0);
	}
	}
#elif __linux__
	*timezone = ::narrow<s16>(-::timezone / 60);
	*summertime = !::daylight ? 0 : []() -> s32
	{
		struct tm test{};
		ensure(&test == localtime_r(&start_time.tv_sec, &test));

		// Check bounds [0,1]
		if (test.tm_isdst & -2)
		{
			sys_time.error("No information for timezone DST bias (timezone=%.2fh, tm_gmtoff=%d)", -::timezone / 3600.0, test.tm_gmtoff);
			return 0;
		}
		else
		{
			return test.tm_isdst ? ::narrow<s16>((test.tm_gmtoff + ::timezone) / 60) : 0;
		}
	}();
#elif defined(RPCS3_PS4)
	// The generic path below reads struct timezone from gettimeofday(), which musl leaves zeroed
	const auto [tz_minutes, dst_minutes] = ps4_get_host_timezone();
	*timezone = tz_minutes;
	*summertime = dst_minutes;
#else
	// gettimeofday doesn't return timezone on linux anymore, but this should work on other OSes?
	struct timezone tz{};
	ensure(gettimeofday(nullptr, &tz) == 0);
	*timezone = ::narrow<s16>(-tz.tz_minuteswest);
	*summertime = !tz.tz_dsttime ? 0 : [&]() -> s32
	{
		struct tm test{};
		ensure(&test == localtime_r(&start_time.tv_sec, &test));

		return test.tm_isdst ? ::narrow<s16>(test.tm_gmtoff / 60 + tz.tz_minuteswest) : 0;
	}();
#endif

	return CELL_OK;
}

error_code sys_time_get_current_time(vm::ptr<s64> sec, vm::ptr<s64> nsec)
{
	sys_time.trace("sys_time_get_current_time(sec=*0x%x, nsec=*0x%x)", sec, nsec);

	if (!sec)
	{
		return CELL_EFAULT;
	}

#ifdef _WIN32
	LARGE_INTEGER count;
	ensure(QueryPerformanceCounter(&count));

	const u64 diff_base = count.QuadPart - s_time_aux_info.start_time;

	// Get time difference in nanoseconds (using 128 bit accumulator)
	const u64 diff_sl = diff_base * 1000000000ull;
	const u64 diff_sh = utils::umulh64(diff_base, 1000000000ull);
	const u64 diff = utils::udiv128(diff_sh, diff_sl, s_time_aux_info.perf_freq);

	// get time since Epoch in nanoseconds
	const u64 time = s_time_aux_info.start_ftime * 100u + (diff * g_cfg.core.clocks_scale / 100u);

	// scale to seconds, and add the console time offset (which might be negative)
	*sec = (time / 1000000000ull) + g_cfg.sys.console_time_offset;

	if (!nsec)
	{
		return CELL_EFAULT;
	}

	*nsec = time % 1000000000ull;
#else
	struct timespec ts;
	ensure(::clock_gettime(CLOCK_REALTIME, &ts) == 0);

	if (g_cfg.core.clocks_scale == 100)
	{
		// get the seconds from the system clock, and add the console time offset (which might be negative)
		*sec  = ts.tv_sec + g_cfg.sys.console_time_offset;

		if (!nsec)
		{
			return CELL_EFAULT;
		}

		*nsec = ts.tv_nsec;
		return CELL_OK;
	}

	u64 tv_sec = ts.tv_sec, stv_sec = start_time.tv_sec;
	u64 tv_nsec = ts.tv_nsec, stv_nsec = start_time.tv_nsec;

	// Substruct time since Epoch and since start time
	tv_sec -= stv_sec;

	if (tv_nsec < stv_nsec)
	{
		// Correct value if borrow encountered
		tv_sec -= 1;
		tv_nsec = 1'000'000'000ull - (stv_nsec - tv_nsec);
	}
	else
	{
		tv_nsec -= stv_nsec;
	}

	// Scale nanocseconds
	tv_nsec = stv_nsec + (tv_nsec * g_cfg.core.clocks_scale / 100);

	// Scale seconds and add from nanoseconds / 1'000'000'000, and add the console time offset (which might be negative)
	*sec = stv_sec + (tv_sec * g_cfg.core.clocks_scale / 100u) + (tv_nsec / 1000000000ull) + g_cfg.sys.console_time_offset;

	if (!nsec)
	{
		return CELL_EFAULT;
	}

	// Set nanoseconds
	*nsec = tv_nsec % 1000000000ull;
#endif

	return CELL_OK;
}

error_code sys_time_set_current_time(s64 sec, s64 nsec)
{
	sys_time.trace("sys_time_set_current_time(sec=0x%x, nsec=0x%x)", sec, nsec);

	if (!g_ps3_process_info.has_root_perm())
	{
		return CELL_ENOSYS;
	}

	return CELL_OK;
}

u64 sys_time_get_timebase_frequency()
{
	sys_time.trace("sys_time_get_timebase_frequency()");

	return g_timebase_freq;
}

error_code sys_time_get_rtc(vm::ptr<u64> rtc)
{
	sys_time.todo("sys_time_get_rtc(rtc=*0x%x)", rtc);

	return CELL_OK;
}
