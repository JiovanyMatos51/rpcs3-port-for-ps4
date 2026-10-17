#pragma once

// PS4 (Jaguar has no AVX-512): SSSE3/SSE4.1 versions of upload_untouched_skip_restart<T> (BufferUtils.cpp), used for
// disjoint primitives with primitive restart: byteswap, drop the restart indices, keep the order, unsigned min/max of
// the kept indices. Same count, entries and min/max as the scalar loop for every input. Like the AVX-512 kernels, a
// vector store may also write up to 7 (u16) or 3 (u32) entries past the returned count, always inside dst and below
// the read position. Checked against the scalar loop by ps4-port/tests/index_skip_restart_test.cpp.

#include "util/types.hpp"
#include "util/endian.hpp"
#include "util/to_endian.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <span>
#include <tuple>

#if defined(ARCH_X64)
#include <immintrin.h>

#if defined(_MSC_VER) || !defined(__SSE2__)
#define BUFFERUTILS_SSE41_FUNC
#else
#define BUFFERUTILS_SSE41_FUNC __attribute__((__target__("ssse3,sse4.1")))
#endif

namespace rsx::ps4_index
{
	// pshufb controls: entry k moves the lanes whose bit is set in k to the front, in order; the other bytes read zero
	template <u32 LaneBytes, u32 Lanes>
	constexpr std::array<std::array<u8, 16>, (1u << Lanes)> make_compaction_table()
	{
		std::array<std::array<u8, 16>, (1u << Lanes)> table{};

		for (u32 mask = 0; mask < (1u << Lanes); mask++)
		{
			u32 out = 0;

			for (u32 lane = 0; lane < Lanes; lane++)
			{
				if (mask & (1u << lane))
				{
					for (u32 b = 0; b < LaneBytes; b++)
					{
						table[mask][out * LaneBytes + b] = static_cast<u8>(lane * LaneBytes + b);
					}

					out++;
				}
			}

			for (u32 i = out * LaneBytes; i < 16; i++)
			{
				table[mask][i] = 0x80;
			}
		}

		return table;
	}

	alignas(16) inline constexpr auto s_compact_u16 = make_compaction_table<2, 8>(); // 4 KiB
	alignas(16) inline constexpr auto s_compact_u32 = make_compaction_table<4, 4>(); // 256 B

	BUFFERUTILS_SSE41_FUNC inline std::tuple<u16, u16, u32> upload_u16_swapped_sse41_skip_restart(std::span<to_be_t<const u16>> src, std::span<u16> dst, u16 restart_index)
	{
		const u32 count = ::size32(src);
		const u32 vector_end = std::min<u32>(count, ::size32(dst)) & ~7u;
		const auto* in = reinterpret_cast<const u8*>(src.data());
		u16* out = dst.data();

		const __m128i bswap = _mm_set_epi8(14, 15, 12, 13, 10, 11, 8, 9, 6, 7, 4, 5, 2, 3, 0, 1);
		const __m128i restart = _mm_set1_epi16(static_cast<s16>(restart_index));
		__m128i vmin = _mm_set1_epi16(-1);
		__m128i vmax = _mm_setzero_si128();

		u32 written = 0;
		u32 i = 0;

		for (; i < vector_end; i += 8)
		{
			const __m128i value = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(in + i * 2)), bswap);
			const __m128i is_restart = _mm_cmpeq_epi16(value, restart);

			// Restart lanes are neutral: all ones for the minimum, zero for the maximum
			vmin = _mm_min_epu16(vmin, _mm_or_si128(value, is_restart));
			vmax = _mm_max_epu16(vmax, _mm_andnot_si128(is_restart, value));

			// One bit per kept lane; the kept lanes are packed to the front and stored at the write position
			const u32 keep = ~static_cast<u32>(_mm_movemask_epi8(_mm_packs_epi16(is_restart, is_restart))) & 0xffu;
			const __m128i packed = _mm_shuffle_epi8(value, _mm_loadu_si128(reinterpret_cast<const __m128i*>(s_compact_u16[keep].data())));
			_mm_storeu_si128(reinterpret_cast<__m128i*>(out + written), packed);
			written += static_cast<u32>(std::popcount(keep));
		}

		u16 min_index = static_cast<u16>(_mm_cvtsi128_si32(_mm_minpos_epu16(vmin)));
		u16 max_index = static_cast<u16>(~_mm_cvtsi128_si32(_mm_minpos_epu16(_mm_xor_si128(vmax, _mm_set1_epi32(-1)))));

		for (; i < count; i++)
		{
			const u16 index = src[i];

			if (index != restart_index)
			{
				out[written++] = index;
				min_index = std::min(min_index, index);
				max_index = std::max(max_index, index);
			}
		}

		return std::make_tuple(min_index, max_index, written);
	}

	BUFFERUTILS_SSE41_FUNC inline std::tuple<u32, u32, u32> upload_u32_swapped_sse41_skip_restart(std::span<to_be_t<const u32>> src, std::span<u32> dst, u32 restart_index)
	{
		const u32 count = ::size32(src);
		const u32 vector_end = std::min<u32>(count, ::size32(dst)) & ~3u;
		const auto* in = reinterpret_cast<const u8*>(src.data());
		u32* out = dst.data();

		const __m128i bswap = _mm_set_epi8(12, 13, 14, 15, 8, 9, 10, 11, 4, 5, 6, 7, 0, 1, 2, 3);
		const __m128i restart = _mm_set1_epi32(static_cast<s32>(restart_index));
		__m128i vmin = _mm_set1_epi32(-1);
		__m128i vmax = _mm_setzero_si128();

		u32 written = 0;
		u32 i = 0;

		for (; i < vector_end; i += 4)
		{
			const __m128i value = _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(in + i * 4)), bswap);
			const __m128i is_restart = _mm_cmpeq_epi32(value, restart);

			// Restart lanes are neutral: all ones for the minimum, zero for the maximum
			vmin = _mm_min_epu32(vmin, _mm_or_si128(value, is_restart));
			vmax = _mm_max_epu32(vmax, _mm_andnot_si128(is_restart, value));

			// One bit per kept lane; the kept lanes are packed to the front and stored at the write position
			const u32 keep = ~static_cast<u32>(_mm_movemask_ps(_mm_castsi128_ps(is_restart))) & 0xfu;
			const __m128i packed = _mm_shuffle_epi8(value, _mm_loadu_si128(reinterpret_cast<const __m128i*>(s_compact_u32[keep].data())));
			_mm_storeu_si128(reinterpret_cast<__m128i*>(out + written), packed);
			written += static_cast<u32>(std::popcount(keep));
		}

		vmin = _mm_min_epu32(vmin, _mm_shuffle_epi32(vmin, 0x4e));
		vmin = _mm_min_epu32(vmin, _mm_shuffle_epi32(vmin, 0xb1));
		vmax = _mm_max_epu32(vmax, _mm_shuffle_epi32(vmax, 0x4e));
		vmax = _mm_max_epu32(vmax, _mm_shuffle_epi32(vmax, 0xb1));

		u32 min_index = static_cast<u32>(_mm_cvtsi128_si32(vmin));
		u32 max_index = static_cast<u32>(_mm_cvtsi128_si32(vmax));

		for (; i < count; i++)
		{
			const u32 index = src[i];

			if (index != restart_index)
			{
				out[written++] = index;
				min_index = std::min(min_index, index);
				max_index = std::max(max_index, index);
			}
		}

		return std::make_tuple(min_index, max_index, written);
	}
}
#endif
