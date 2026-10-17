// Host test for rpcs3/Emu/RSX/Common/BufferUtils_sse41.h. The SSSE3/SSE4.1 skip-restart index kernels must return the
// same count, entries and min/max as the scalar loop upload_untouched_skip_restart<T> of BufferUtils.cpp (copied
// below verbatim with its helpers) for both index widths: every length 0-64 plus large ones, 0-100 % restart indices,
// restart values at and next to the type limits, unaligned source and destination, and destinations holding only the
// source count or only the kept count (the kernel must stay inside dst: guard entries after it are checked).
// Build (from the repo root):
//   clang++ -std=c++20 -O2 -static -Wall -Wextra -I rpcs3 ps4-port/tests/index_skip_restart_test.cpp -o index_skip_restart_test.exe

#include "Emu/RSX/Common/BufferUtils_sse41.h"

#include <cstdio>
#include <cstdlib>
#include <vector>

// ::size32 reports a narrowing failure through this (never reached here)
namespace fmt
{
	[[noreturn]] void raw_verify_error(std::source_location, const char8_t*, usz)
	{
		std::puts("index_skip_restart_test: size32 narrowing failure");
		std::abort();
	}
}

namespace reference
{
	// BufferUtils.cpp (anonymous namespace), unchanged
	template <typename T>
	constexpr T index_limit()
	{
		return -1;
	}

	template <typename T>
	const T& min_max(T& min, T& max, const T& value)
	{
		if (value < min)
			min = value;

		if (value > max)
			max = value;

		return value;
	}

	template <typename T>
	std::tuple<T, T, u32> upload_untouched_skip_restart(std::span<to_be_t<const T>> src, std::span<T> dst, T restart_index)
	{
		T min_index = index_limit<T>();
		T max_index = 0;
		u32 written = 0;
		u32 length = ::size32(src);

		for (u32 i = written; i < length; ++i)
		{
			T index = src[i];
			if (index != restart_index)
			{
				dst[written++] = min_max(min_index, max_index, index);
			}
		}

		return std::make_tuple(min_index, max_index, written);
	}
}

namespace
{
	u64 s_seed = 0x9e3779b97f4a7c15ull;
	u64 s_cases = 0;

	u64 next()
	{
		s_seed ^= s_seed << 13;
		s_seed ^= s_seed >> 7;
		s_seed ^= s_seed << 17;
		return s_seed;
	}

	[[noreturn]] void fail(const char* what, u32 bits, u32 length, u32 restart, u32 density, u32 detail)
	{
		std::printf("FAIL %s: u%u length=%u restart=0x%x density=%u/64 detail=%u (case %llu)\n", what, bits, length, restart, density, detail,
			static_cast<unsigned long long>(s_cases));
		std::exit(1);
	}

	// value_mode 0: full range; 1: restart and its neighbours; 2: a small range (repeats, min = max cases)
	template <typename T>
	T make_value(T restart, u32 value_mode)
	{
		switch (value_mode)
		{
		case 1:
			return static_cast<T>(restart + static_cast<T>(next() % 3) - 1);
		case 2:
			return static_cast<T>(next() % 5);
		default:
			return static_cast<T>(next());
		}
	}

	// dst_mode 0: dst holds the source count; 1: dst holds only the kept count (what the scalar loop writes)
	template <typename T, typename Kernel>
	void check_one(Kernel kernel, u32 length, T restart, u32 density, u32 value_mode, u32 src_shift, u32 dst_shift, u32 dst_mode)
	{
		constexpr u32 bits = sizeof(T) * 8;
		constexpr u32 guard = 16;
		s_cases++;

		std::vector<be_t<T>> src_storage(length + 4);
		std::vector<T> values(length);
		u32 kept = 0;

		for (u32 i = 0; i < length; i++)
		{
			T value = (next() % 64) < density ? restart : make_value<T>(restart, value_mode);
			values[i] = value;
			src_storage[src_shift + i] = value;
			kept += value != restart;
		}

		const std::span<to_be_t<const T>> src(src_storage.data() + src_shift, length);
		const u32 dst_size = dst_mode ? kept : length;

		std::vector<T> expected_dst(length + 1, 0);
		const auto [e_min, e_max, e_count] = reference::upload_untouched_skip_restart<T>(src, std::span<T>(expected_dst.data(), length), restart);

		if (e_count != kept)
		{
			fail("reference count", bits, length, static_cast<u32>(restart), density, e_count);
		}

		// Guard entries before and after the destination window must survive
		constexpr T poison = static_cast<T>(0x5a5a5a5au);
		std::vector<T> dst_storage(dst_shift + dst_size + guard, poison);
		const auto [min, max, count] = kernel(src, std::span<T>(dst_storage.data() + dst_shift, dst_size), restart);

		if (count != e_count)
			fail("count", bits, length, static_cast<u32>(restart), density, count);
		if (min != e_min)
			fail("min", bits, length, static_cast<u32>(restart), density, static_cast<u32>(min));
		if (max != e_max)
			fail("max", bits, length, static_cast<u32>(restart), density, static_cast<u32>(max));

		for (u32 i = 0; i < count; i++)
		{
			if (dst_storage[dst_shift + i] != expected_dst[i])
				fail("entry", bits, length, static_cast<u32>(restart), density, i);
		}

		for (u32 i = 0; i < dst_shift; i++)
		{
			if (dst_storage[i] != poison)
				fail("write before dst", bits, length, static_cast<u32>(restart), density, i);
		}

		for (u32 i = 0; i < guard; i++)
		{
			if (dst_storage[dst_shift + dst_size + i] != poison)
				fail("write past dst", bits, length, static_cast<u32>(restart), density, i);
		}

		// The source is read-only for the kernel
		for (u32 i = 0; i < length; i++)
		{
			if (static_cast<T>(src_storage[src_shift + i]) != values[i])
				fail("source modified", bits, length, static_cast<u32>(restart), density, i);
		}
	}

	template <typename T, typename Kernel>
	void check_width(Kernel kernel)
	{
		constexpr T max = static_cast<T>(-1);
		const T restarts[] = {0, 1, static_cast<T>(max / 2), static_cast<T>(max / 2 + 1), static_cast<T>(max - 1), max};
		const u32 densities[] = {0, 1, 8, 16, 32, 48, 63, 64};
		const u32 large[] = {1000, 4097, 65536 + 3, 200000};

		for (const T restart : restarts)
		{
			for (const u32 density : densities)
			{
				for (u32 value_mode = 0; value_mode < 3; value_mode++)
				{
					for (u32 length = 0; length <= 64; length++)
					{
						for (u32 dst_mode = 0; dst_mode < 2; dst_mode++)
						{
							check_one<T>(kernel, length, restart, density, value_mode, length % 3, length % 5, dst_mode);
						}
					}

					for (const u32 length : large)
					{
						check_one<T>(kernel, length, restart, density, value_mode, 1, 3, 0);
						check_one<T>(kernel, length, restart, density, value_mode, 0, 0, 1);
					}
				}
			}

			// Random restart value, random density, random lengths
			for (u32 n = 0; n < 2000; n++)
			{
				const T random_restart = static_cast<T>(next());
				check_one<T>(kernel, static_cast<u32>(next() % 300), random_restart, static_cast<u32>(next() % 65), static_cast<u32>(next() % 3),
					static_cast<u32>(next() % 4), static_cast<u32>(next() % 4), static_cast<u32>(next() % 2));
			}
		}
	}
}

int main()
{
	if (!__builtin_cpu_supports("ssse3") || !__builtin_cpu_supports("sse4.1"))
	{
		std::puts("index_skip_restart_test: host CPU without SSSE3/SSE4.1, kernels not run");
		return 1;
	}

	check_width<u16>(rsx::ps4_index::upload_u16_swapped_sse41_skip_restart);
	check_width<u32>(rsx::ps4_index::upload_u32_swapped_sse41_skip_restart);

	std::printf("index_skip_restart_test: OK (%llu cases, u16 and u32 kernels identical to the scalar loop)\n", static_cast<unsigned long long>(s_cases));
	return 0;
}
