#pragma once

#include <cstddef>
#include <cstdint>

#if defined(__SSE2__) || defined(_M_X64)
#include <emmintrin.h>
#endif

// 8-bit YUV 4:2:0 planar to 32-bit ARGB or RGBA (byte order in memory) with a constant alpha.
//
// This is the conversion cellVdecGetPictureExt asks swscale for: sws_getCachedContext(YUVA420P -> ARGB/RGBA,
// SWS_POINT) with the default colorspace details (ITU-R BT.601, limited range, brightness 0, contrast and
// saturation 1.0) and an alpha plane filled with the alpha byte. For an even height swscale picks its unscaled table
// converter (libswscale/yuv2rgb.c: ff_yuv2rgb_c_init_tables plus yuva2argb_c / yuva2rgba_c; the SIMD versions are
// x86asm, which the console's FFmpeg is built without). That converter reads every channel from one clipped luma table:
//
//   channel = clip8((yb0 + (yoffs + Y + term) * cy + 0x8000) >> 16)
//
// where term adds ((C * k) >> 16) - (k >> 9) for each chroma sample C feeding the channel (R: V, G: U and V, B: U),
// k being the table coefficient already divided by the luma scale cy. The functions below compute the same integers,
// so the output is bit-identical to swscale's for every input. The SSE2 path does 2 rows x 16 pixels per step: one
// U/V load serves 4 pixels and every multiply is a 16-bit pmulhuw.
namespace utils::ps4::yuv
{
	namespace detail
	{
		// ff_yuv2rgb_c_init_tables(inv_table = ff_yuv2rgb_coeffs[SWS_CS_DEFAULT], fullRange = 0, brightness = 0,
		// contrast = saturation = 1 << 16). Integer division truncates toward zero and >> of a negative value is an
		// arithmetic shift, as in the C code.
		inline constexpr std::int64_t contrast = std::int64_t{1} << 16;
		inline constexpr std::int64_t saturation = std::int64_t{1} << 16;
		inline constexpr std::int64_t cy = (((std::int64_t{1} << 16) * 255 / 219) * contrast) >> 16;
		inline constexpr std::int64_t oy = std::int64_t{16} << 16;

		constexpr std::int64_t table_coeff(std::int64_t inv)
		{
			const std::int64_t c = (inv * contrast * saturation) >> 32;
			return (c * (std::int64_t{1} << 16) + 0x8000) / (cy > 1 ? cy : 1);
		}

		inline constexpr std::int64_t crv = table_coeff(104597);
		inline constexpr std::int64_t cbu = table_coeff(132201);
		inline constexpr std::int64_t cgu = table_coeff(-25675);
		inline constexpr std::int64_t cgv = table_coeff(-53279);

		// Luma table entry i = clip8((yb0 + i * cy + 0x8000) >> 16) (the C loop adds cy per entry); entries are read at
		// i = yoffs + Y + terms. table_plane_size is the number of entries per channel plane.
		inline constexpr std::int64_t yb0 = -(std::int64_t{384} << 16) - 512 * cy - oy;
		inline constexpr int yoffs = 326 + 512;
		inline constexpr int table_plane_size = 1024 + 2 * 512;

		// fill_table / fill_gv_table for one chroma sample c in [0, 255]
		constexpr int term(int c, std::int64_t k)
		{
			return static_cast<int>(((c * k) >> 16) - (k >> 9));
		}

		constexpr int term_r(int v) { return term(v, crv); }
		constexpr int term_g(int u, int v) { return term(u, cgu) + term(v, cgv); }
		constexpr int term_b(int u) { return term(u, cbu); }

		// t = Y + terms
		constexpr std::uint8_t channel(int t)
		{
			const std::int64_t v = (yb0 + (yoffs + t) * cy + 0x8000) >> 16;
			return static_cast<std::uint8_t>(v < 0 ? 0 : v > 255 ? 255 : v);
		}

		struct term_bounds
		{
			int lo;
			int hi;
		};

		constexpr term_bounds bounds(std::int64_t k)
		{
			term_bounds b{term(0, k), term(0, k)};

			for (int c = 1; c < 256; c++)
			{
				b.lo = term(c, k) < b.lo ? term(c, k) : b.lo;
				b.hi = term(c, k) > b.hi ? term(c, k) : b.hi;
			}

			return b;
		}

		constexpr int min3(int a, int b, int c) { return a < b ? (a < c ? a : c) : (b < c ? b : c); }
		constexpr int max3(int a, int b, int c) { return a > b ? (a > c ? a : c) : (b > c ? b : c); }

		// Smallest and largest table index (relative to yoffs) any Y/U/V combination reads
		inline constexpr int t_min = min3(bounds(crv).lo, bounds(cgu).lo + bounds(cgv).lo, bounds(cbu).lo);
		inline constexpr int t_max = 255 + max3(bounds(crv).hi, bounds(cgu).hi + bounds(cgv).hi, bounds(cbu).hi);

		static_assert(yoffs + t_min >= 0 && yoffs + t_max < table_plane_size, "swscale reads inside its luma table");

		// SIMD form. With s = yoffs - 512 + t (positive, below 1024 here) the channel is
		//   s - 400 + ((s * (cy - 65536) + 0x8000) >> 16)
		// because yb0 + 512 * cy = -(400 << 16) for limited range. The rounding term is replaced by an exact 16-bit form,
		// ((s + luma_bias) * luma_mul) >> 16, valid over every s the conversion can produce (checked below).
		inline constexpr int s_base = yoffs - 512;
		inline constexpr int luma_bias = 3;
		inline constexpr int luma_mul = 10774;

		constexpr bool luma_form_exact()
		{
			if (yb0 + 512 * cy != -(std::int64_t{400} << 16))
			{
				return false;
			}

			for (int s = s_base + t_min; s <= s_base + t_max; s++)
			{
				const std::int64_t want = (s * (cy - 65536) + 0x8000) >> 16;
				const std::int64_t got = (std::int64_t{s + luma_bias} * luma_mul) >> 16;

				if (s + luma_bias <= 0 || s + luma_bias > 32767 || want != got)
				{
					return false;
				}
			}

			return true;
		}

		static_assert(luma_form_exact(), "16-bit luma form differs from the swscale table");

		// The chroma multiplies use unsigned 16-bit high products: k must lie in [-65536, 131072)
		static_assert(crv >= -65536 && crv < 131072 && cbu >= -65536 && cbu < 131072);
		static_assert(cgu >= -65536 && cgu < 131072 && cgv >= -65536 && cgv < 131072);

		// Index bases per channel: s + luma_bias = Y + chroma part + these constants
		inline constexpr int base_r = s_base + luma_bias - static_cast<int>(crv >> 9);
		inline constexpr int base_g = s_base + luma_bias - static_cast<int>(cgu >> 9) - static_cast<int>(cgv >> 9);
		inline constexpr int base_b = s_base + luma_bias - static_cast<int>(cbu >> 9);

		inline void pixel(std::uint8_t* d, int y, int u, int v, bool argb, std::uint8_t alpha)
		{
			const std::uint8_t r = channel(y + term_r(v));
			const std::uint8_t g = channel(y + term_g(u, v));
			const std::uint8_t b = channel(y + term_b(u));

			if (argb)
			{
				d[0] = alpha;
				d[1] = r;
				d[2] = g;
				d[3] = b;
			}
			else
			{
				d[0] = r;
				d[1] = g;
				d[2] = b;
				d[3] = alpha;
			}
		}

		// One output row: pixels [x0, w) of luma row y, chroma rows u/v
		inline void row_scalar(const std::uint8_t* y, const std::uint8_t* u, const std::uint8_t* v, std::uint8_t* d, int x0, int w, bool argb, std::uint8_t alpha)
		{
			for (int x = x0; x < w; x++)
			{
				pixel(d + 4 * x, y[x], u[x >> 1], v[x >> 1], argb, alpha);
			}
		}

#if defined(__SSE2__) || defined(_M_X64)
		inline constexpr bool simd = true;

		// floor(c * k / 65536) for 8 lanes c in [0, 255], with k split around the unsigned 16-bit range
		template <std::int64_t K>
		inline __m128i mul_k(__m128i c)
		{
			if constexpr (K >= 65536)
			{
				return _mm_add_epi16(c, _mm_mulhi_epu16(c, _mm_set1_epi16(static_cast<short>(K - 65536))));
			}
			else if constexpr (K >= 0)
			{
				return _mm_mulhi_epu16(c, _mm_set1_epi16(static_cast<short>(K)));
			}
			else
			{
				return _mm_sub_epi16(_mm_mulhi_epu16(c, _mm_set1_epi16(static_cast<short>(K + 65536))), c);
			}
		}

		// 8 channel values s - 400 + ((s + luma_bias) * luma_mul >> 16) from t = s + luma_bias (the pack clips them)
		inline __m128i luma(__m128i t)
		{
			return _mm_add_epi16(_mm_sub_epi16(t, _mm_set1_epi16(luma_bias + 400)), _mm_mulhi_epu16(t, _mm_set1_epi16(luma_mul)));
		}

		inline void store(std::uint8_t* d, __m128i x, bool nt)
		{
			if (nt)
			{
				_mm_stream_si128(reinterpret_cast<__m128i*>(d), x);
			}
			else
			{
				_mm_storeu_si128(reinterpret_cast<__m128i*>(d), x);
			}
		}

		struct chroma
		{
			__m128i r, g, b;
		};

		// 16 pixels of one row
		template <bool Argb>
		inline void pixels16(const std::uint8_t* y, std::uint8_t* d, const chroma& lo, const chroma& hi, __m128i a, bool nt)
		{
			const __m128i zero = _mm_setzero_si128();
			const __m128i y8 = _mm_loadu_si128(reinterpret_cast<const __m128i*>(y));
			const __m128i ylo = _mm_unpacklo_epi8(y8, zero);
			const __m128i yhi = _mm_unpackhi_epi8(y8, zero);

			const __m128i r = _mm_packus_epi16(luma(_mm_add_epi16(ylo, lo.r)), luma(_mm_add_epi16(yhi, hi.r)));
			const __m128i g = _mm_packus_epi16(luma(_mm_add_epi16(ylo, lo.g)), luma(_mm_add_epi16(yhi, hi.g)));
			const __m128i b = _mm_packus_epi16(luma(_mm_add_epi16(ylo, lo.b)), luma(_mm_add_epi16(yhi, hi.b)));

			// Byte order in memory: A,R,G,B or R,G,B,A
			const __m128i p0 = Argb ? a : r;
			const __m128i p1 = Argb ? r : g;
			const __m128i p2 = Argb ? g : b;
			const __m128i p3 = Argb ? b : a;

			const __m128i q01l = _mm_unpacklo_epi8(p0, p1);
			const __m128i q01h = _mm_unpackhi_epi8(p0, p1);
			const __m128i q23l = _mm_unpacklo_epi8(p2, p3);
			const __m128i q23h = _mm_unpackhi_epi8(p2, p3);

			store(d + 0, _mm_unpacklo_epi16(q01l, q23l), nt);
			store(d + 16, _mm_unpackhi_epi16(q01l, q23l), nt);
			store(d + 32, _mm_unpacklo_epi16(q01h, q23h), nt);
			store(d + 48, _mm_unpackhi_epi16(q01h, q23h), nt);
		}

		// One or two output rows sharing a chroma row
		template <bool Argb>
		inline void rows_sse2(const std::uint8_t* y0, const std::uint8_t* y1, const std::uint8_t* u, const std::uint8_t* v,
			std::uint8_t* d0, std::uint8_t* d1, int w, bool two, bool nt0, bool nt1, std::uint8_t alpha)
		{
			const __m128i zero = _mm_setzero_si128();
			const __m128i a = _mm_set1_epi8(static_cast<char>(alpha));
			const __m128i br = _mm_set1_epi16(base_r);
			const __m128i bg = _mm_set1_epi16(base_g);
			const __m128i bb = _mm_set1_epi16(base_b);

			int x = 0;

			for (; x + 16 <= w; x += 16)
			{
				const __m128i u16 = _mm_unpacklo_epi8(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(u + x / 2)), zero);
				const __m128i v16 = _mm_unpacklo_epi8(_mm_loadl_epi64(reinterpret_cast<const __m128i*>(v + x / 2)), zero);

				const __m128i cr = _mm_add_epi16(mul_k<crv>(v16), br);
				const __m128i cg = _mm_add_epi16(_mm_add_epi16(mul_k<cgu>(u16), mul_k<cgv>(v16)), bg);
				const __m128i cb = _mm_add_epi16(mul_k<cbu>(u16), bb);

				// Each chroma sample covers two horizontal pixels
				const chroma lo{_mm_unpacklo_epi16(cr, cr), _mm_unpacklo_epi16(cg, cg), _mm_unpacklo_epi16(cb, cb)};
				const chroma hi{_mm_unpackhi_epi16(cr, cr), _mm_unpackhi_epi16(cg, cg), _mm_unpackhi_epi16(cb, cb)};

				pixels16<Argb>(y0 + x, d0 + 4 * x, lo, hi, a, nt0);

				if (two)
				{
					pixels16<Argb>(y1 + x, d1 + 4 * x, lo, hi, a, nt1);
				}
			}

			row_scalar(y0, u, v, d0, x, w, Argb, alpha);

			if (two)
			{
				row_scalar(y1, u, v, d1, x, w, Argb, alpha);
			}
		}
#else
		inline constexpr bool simd = false;
#endif

		// Non-temporal stores only for pictures larger than half a Jaguar L2 (2 MiB per 4-core cluster): a 720p
		// picture (3.5 MiB) would only evict the caller's working set; small pictures stay cached for the reader.
		inline constexpr std::size_t stream_min_bytes = std::size_t{1} << 20;
	}

	// True when yuv420_to_rgb32 uses the SSE2 path
	inline constexpr bool yuv420_to_rgb32_simd = detail::simd;

	// Scalar version of the same conversion (also the tail of the SIMD rows)
	inline void yuv420_to_rgb32_scalar(const std::uint8_t* y, std::ptrdiff_t y_stride, const std::uint8_t* u, std::ptrdiff_t u_stride,
		const std::uint8_t* v, std::ptrdiff_t v_stride, std::uint8_t* dst, std::ptrdiff_t dst_stride, int w, int h, bool argb, std::uint8_t alpha)
	{
		for (int row = 0; row < h; row++)
		{
			detail::row_scalar(y + row * y_stride, u + (row >> 1) * u_stride, v + (row >> 1) * v_stride, dst + row * dst_stride, 0, w, argb, alpha);
		}
	}

	// Converts a w x h picture. Strides and the destination pitch are in bytes and may be any value (no alignment is
	// required); a 16-byte aligned destination row of a large picture is written with non-temporal stores. Odd sizes
	// are converted too (chroma of the last column/row is the one covering it), but swscale itself only takes this
	// table path for an even height and leaves the last column of an odd width unwritten, so the caller keeps swscale
	// for odd sizes to stay identical to the PC.
	inline void yuv420_to_rgb32(const std::uint8_t* y, std::ptrdiff_t y_stride, const std::uint8_t* u, std::ptrdiff_t u_stride,
		const std::uint8_t* v, std::ptrdiff_t v_stride, std::uint8_t* dst, std::ptrdiff_t dst_stride, int w, int h, bool argb, std::uint8_t alpha)
	{
		if (w <= 0 || h <= 0)
		{
			return;
		}

#if defined(__SSE2__) || defined(_M_X64)
		const bool stream = static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 4 >= detail::stream_min_bytes;
		bool streamed = false;

		for (int row = 0; row < h; row += 2)
		{
			const bool two = row + 1 < h;
			const std::uint8_t* y0 = y + row * y_stride;
			const std::uint8_t* y1 = two ? y0 + y_stride : y0;
			const std::uint8_t* ur = u + (row >> 1) * u_stride;
			const std::uint8_t* vr = v + (row >> 1) * v_stride;
			std::uint8_t* d0 = dst + row * dst_stride;
			std::uint8_t* d1 = two ? d0 + dst_stride : d0;

			const bool nt0 = stream && (reinterpret_cast<std::uintptr_t>(d0) & 15) == 0;
			const bool nt1 = stream && two && (reinterpret_cast<std::uintptr_t>(d1) & 15) == 0;
			streamed = streamed || nt0 || nt1;

			if (argb)
			{
				detail::rows_sse2<true>(y0, y1, ur, vr, d0, d1, w, two, nt0, nt1, alpha);
			}
			else
			{
				detail::rows_sse2<false>(y0, y1, ur, vr, d0, d1, w, two, nt0, nt1, alpha);
			}
		}

		// Non-temporal stores are weakly ordered: make them visible before the caller returns the picture
		if (streamed)
		{
			_mm_sfence();
		}
#else
		yuv420_to_rgb32_scalar(y, y_stride, u, u_stride, v, v_stride, dst, dst_stride, w, h, argb, alpha);
#endif
	}
}
