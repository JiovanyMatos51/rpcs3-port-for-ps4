// Host test for rpcs3/util/ps4_yuv_convert.hpp (cellVdecGetPictureExt YUV420 -> ARGB/RGBA on the PS4).
//
// The reference below is a line-by-line port of what swscale (FFmpeg 8.1.1, ps4-port/deps/ffmpeg) runs for the call
// cellVdecGetPictureExt makes: sws_getCachedContext(w, h, YUVA420P, w, h, ARGB or RGBA, SWS_POINT) followed by
// sws_scale with the alpha plane filled with format->alpha. For that context swscale initialises the colour tables
// once with sws_setColorspaceDetails(ff_yuv2rgb_coeffs[SWS_CS_DEFAULT], src_range 0, ..., brightness 0, contrast and
// saturation 1 << 16) (utils.c ff_sws_init_single_context), and for an even height selects the unscaled converter
// ff_yuv2rgb_get_func_ptr (swscale_unscaled.c), which without x86asm is yuva2argb_c / yuva2rgba_c (yuv2rgb.c) built on
// ff_yuv2rgb_c_init_tables (32 bpp case, needAlpha = 1).
//
// Build and run (from the repository root):
//   clang++ -static -std=c++20 -O1 -Wall -Wextra -I rpcs3 ps4-port/tests/vdec_convert_test.cpp -o vdec_convert_test.exe
//   ./vdec_convert_test.exe

#include "util/ps4_yuv_convert.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace
{
int g_failures = 0;

#define CHECK(cond, ...)                                    \
	do                                                      \
	{                                                       \
		if (!(cond))                                        \
		{                                                   \
			std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
			std::printf(__VA_ARGS__);                       \
			std::printf("\n");                              \
			if (++g_failures > 20)                          \
				std::exit(1);                               \
		}                                                   \
	} while (0)

// ---------------------------------------------------------------------------------------------------------------------
// Reference: libswscale/yuv2rgb.c
// ---------------------------------------------------------------------------------------------------------------------
namespace sws
{
constexpr int YUVRGB_TABLE_HEADROOM = 512;
constexpr int YUVRGB_TABLE_LUMA_HEADROOM = 512;

// ff_yuv2rgb_coeffs[SWS_CS_DEFAULT] (SWS_CS_DEFAULT = SWS_CS_ITU601 = 5): {crv, cbu, cgu, cgv}
constexpr int32_t coeffs_default[4] = {104597, 132201, 25675, 53279};

int av_clip_uint8(int a)
{
	return a < 0 ? 0 : a > 255 ? 255 : a;
}

struct context
{
	// Destination AV_PIX_FMT_ARGB is AV_PIX_FMT_BGR32_1 on little endian, AV_PIX_FMT_RGBA is AV_PIX_FMT_BGR32
	bool argb = false;
	int dst_w = 0;

	std::vector<uint32_t> yuvTable;

	// uint8_t* in C (pointers into yuvTable); byte offsets from yuvTable here
	int64_t table_rV[256 + 2 * YUVRGB_TABLE_HEADROOM]{};
	int64_t table_gU[256 + 2 * YUVRGB_TABLE_HEADROOM]{};
	int table_gV[256 + 2 * YUVRGB_TABLE_HEADROOM]{};
	int64_t table_bU[256 + 2 * YUVRGB_TABLE_HEADROOM]{};

	// static void fill_table(uint8_t* table[256 + 2*YUVRGB_TABLE_HEADROOM], const int elemsize, const int64_t inc, void *y_tab)
	static void fill_table(int64_t* table, const int elemsize, const int64_t inc, int64_t y_tab)
	{
		int64_t y_table = y_tab;

		y_table -= elemsize * (inc >> 9);

		for (int i = 0; i < 256 + 2 * YUVRGB_TABLE_HEADROOM; i++)
		{
			int64_t cb = av_clip_uint8(i - YUVRGB_TABLE_HEADROOM) * inc;
			table[i] = y_table + elemsize * (cb >> 16);
		}
	}

	static void fill_gv_table(int* table, const int elemsize, const int64_t inc)
	{
		int i;
		int off = -(inc >> 9);

		for (i = 0; i < 256 + 2 * YUVRGB_TABLE_HEADROOM; i++)
		{
			int64_t cb = av_clip_uint8(i - YUVRGB_TABLE_HEADROOM) * inc;
			table[i] = elemsize * (off + (cb >> 16));
		}
	}

	// ff_yuv2rgb_c_init_tables, case 32 (dstFormatBpp of ARGB/RGBA), needAlpha = 1 (source YUVA420P)
	void init_tables(const int32_t inv_table[4], int fullRange, int brightness, int contrast, int saturation)
	{
		const int isRgb = 0; // neither BGR32_1 nor BGR32 is in the isRgb list (RGB32, RGB32_1, BGR24, ...)
		int base, rbase, gbase, bbase;
		const int yoffs = (fullRange ? 384 : 326) + YUVRGB_TABLE_LUMA_HEADROOM;
		const int table_plane_size = 1024 + 2 * YUVRGB_TABLE_LUMA_HEADROOM;

		int64_t crv = inv_table[0];
		int64_t cbu = inv_table[1];
		int64_t cgu = -inv_table[2];
		int64_t cgv = -inv_table[3];
		int64_t cy = 1 << 16;
		int64_t oy = 0;
		int64_t yb = 0;

		if (!fullRange)
		{
			cy = (cy * 255) / 219;
			oy = 16 << 16;
		}
		else
		{
			crv = (crv * 224) / 255;
			cbu = (cbu * 224) / 255;
			cgu = (cgu * 224) / 255;
			cgv = (cgv * 224) / 255;
		}

		cy = (cy * contrast) >> 16;
		crv = (crv * contrast * saturation) >> 32;
		cbu = (cbu * contrast * saturation) >> 32;
		cgu = (cgu * contrast * saturation) >> 32;
		cgv = (cgv * contrast * saturation) >> 32;
		oy -= 256LL * brightness;

		// scale coefficients by cy
		crv = ((crv * (1 << 16)) + 0x8000) / (cy > 1 ? cy : 1);
		cbu = ((cbu * (1 << 16)) + 0x8000) / (cy > 1 ? cy : 1);
		cgu = ((cgu * (1 << 16)) + 0x8000) / (cy > 1 ? cy : 1);
		cgv = ((cgv * (1 << 16)) + 0x8000) / (cy > 1 ? cy : 1);

		base = argb ? 8 : 0; // dst == RGB32_1 || dst == BGR32_1
		rbase = base + (isRgb ? 16 : 0);
		gbase = base + 8;
		bbase = base + (isRgb ? 0 : 16);

		yuvTable.assign(table_plane_size * 3, 0);
		uint32_t* y_table32 = yuvTable.data();
		yb = -(384 << 16) - YUVRGB_TABLE_LUMA_HEADROOM * cy - oy;

		for (int i = 0; i < table_plane_size; i++)
		{
			unsigned yval = av_clip_uint8((yb + 0x8000) >> 16);
			y_table32[i] = (yval << rbase) + 0; // needAlpha: no constant alpha in the table
			y_table32[i + table_plane_size] = yval << gbase;
			y_table32[i + 2 * table_plane_size] = yval << bbase;
			yb += cy;
		}

		fill_table(table_rV, 4, crv, 4 * int64_t{yoffs});
		fill_table(table_gU, 4, cgu, 4 * int64_t{yoffs + table_plane_size});
		fill_table(table_bU, 4, cbu, 4 * int64_t{yoffs + 2 * table_plane_size});
		fill_gv_table(table_gV, 4, cgv);
	}

	context(bool argb_, int w)
		: argb(argb_), dst_w(w)
	{
		init_tables(coeffs_default, 0, 0, 1 << 16, 1 << 16);
	}

	// r[Y] for a byte offset 'p' into yuvTable read as uint32_t*
	uint32_t at(int64_t p, int index) const
	{
		const int64_t byte = p + 4 * int64_t{index};

		if (byte < 0 || byte % 4 || byte / 4 >= static_cast<int64_t>(yuvTable.size()))
		{
			std::printf("reference table read out of range (%lld)\n", static_cast<long long>(byte));
			std::exit(1);
		}

		return yuvTable[static_cast<size_t>(byte / 4)];
	}

	// YUV420FUNC(yuva2argb_c / yuva2rgba_c, uint32_t, alpha 1, abase, PUTRGBA, dst_delta 8, 1)
	int convert(const uint8_t* const src[4], const int srcStride[4], int srcSliceY, int srcSliceH, uint8_t* const dst[4], const int dstStride[4]) const
	{
		const int abase = argb ? 0 : 24; // little endian: yuva2rgba_c abase 24, yuva2argb_c abase 0
		int y;

		for (y = 0; y < srcSliceH; y += 2)
		{
			int yd = y + srcSliceY;
			uint8_t* dst_1 = dst[0] + (yd)*dstStride[0];
			uint8_t* dst_2 = dst[0] + (yd + 1) * dstStride[0];
			const uint8_t* py_1 = src[0] + y * srcStride[0];
			const uint8_t* py_2 = py_1 + srcStride[0];
			const uint8_t* pu_1 = src[1] + (y >> 1) * srcStride[1];
			const uint8_t* pv_1 = src[2] + (y >> 1) * srcStride[2];
			const uint8_t* pa_1 = src[3] + y * srcStride[3];
			const uint8_t* pa_2 = pa_1 + srcStride[3];
			unsigned int h_size = dst_w >> 3;

			int64_t r = 0, g = 0, b = 0;
			int U, V, Y;

			const auto load_chroma = [&](int i)
			{
				U = pu_1[i];
				V = pv_1[i];
				r = table_rV[V + YUVRGB_TABLE_HEADROOM];
				g = table_gU[U + YUVRGB_TABLE_HEADROOM] + table_gV[V + YUVRGB_TABLE_HEADROOM];
				b = table_bU[U + YUVRGB_TABLE_HEADROOM];
			};

			// PUTRGBA(l, i, abase) for line l (1 or 2), pixel pair i
			const auto put = [&](int l, int i)
			{
				const uint8_t* py = l == 1 ? py_1 : py_2;
				const uint8_t* pa = l == 1 ? pa_1 : pa_2;
				uint8_t* d = l == 1 ? dst_1 : dst_2;

				for (int k = 0; k < 2; k++)
				{
					Y = py[2 * i + k];
					const uint32_t value = at(r, Y) + at(g, Y) + at(b, Y) + (static_cast<uint32_t>(pa[2 * i + k]) << abase);
					std::memcpy(d + 4 * (2 * i + k), &value, 4);
				}
			};

			const auto advance = [&](int ss)
			{
				pu_1 += 4 >> ss;
				pv_1 += 4 >> ss;
				py_1 += 8 >> ss;
				py_2 += 8 >> ss;
				pa_1 += 8 >> ss;
				pa_2 += 8 >> ss;
				dst_1 += 4 * (8 >> ss);
				dst_2 += 4 * (8 >> ss);
			};

			while (h_size--)
			{
				load_chroma(0);
				put(1, 0);
				put(2, 0);

				load_chroma(1);
				put(2, 1);
				put(1, 1);

				load_chroma(2);
				put(1, 2);
				put(2, 2);

				load_chroma(3);
				put(2, 3);
				put(1, 3);

				advance(0);
			}

			if (dst_w & (4 >> 0))
			{
				load_chroma(0);
				put(1, 0);
				put(2, 0);

				load_chroma(1);
				put(2, 1);
				put(1, 1);

				advance(1);
			}

			if (dst_w & (4 >> 1))
			{
				load_chroma(0);
				put(1, 0);
				put(2, 0);
			}
		}

		return srcSliceH;
	}
};
}

// ---------------------------------------------------------------------------------------------------------------------
// Frames
// ---------------------------------------------------------------------------------------------------------------------
struct rng
{
	uint64_t s;

	uint32_t next()
	{
		s ^= s << 13;
		s ^= s >> 7;
		s ^= s << 17;
		return static_cast<uint32_t>(s >> 11);
	}
};

struct frame
{
	int w = 0, h = 0;
	int y_stride = 0, u_stride = 0, v_stride = 0;
	std::vector<uint8_t> y, u, v;

	frame(int w_, int h_, int y_pad, int u_pad, int v_pad)
		: w(w_), h(h_)
	{
		const int cw = (w + 1) / 2;
		const int ch = (h + 1) / 2;
		y_stride = w + y_pad;
		u_stride = cw + u_pad;
		v_stride = cw + v_pad;
		y.assign(static_cast<size_t>(y_stride) * h, 0);
		u.assign(static_cast<size_t>(u_stride) * ch, 0);
		v.assign(static_cast<size_t>(v_stride) * ch, 0);
	}

	void randomize(rng& r)
	{
		for (auto& b : y) b = static_cast<uint8_t>(r.next());
		for (auto& b : u) b = static_cast<uint8_t>(r.next());
		for (auto& b : v) b = static_cast<uint8_t>(r.next());

		// Some saturated and video-range edges
		for (int i = 0; i < w && i < 8; i++)
		{
			y[static_cast<size_t>(i)] = static_cast<uint8_t>(i & 1 ? 255 : 0);
		}
	}
};

// Destination with guard bytes before/after and in the row padding
struct dest
{
	static constexpr uint8_t guard = 0xcd;
	std::vector<uint8_t> mem;
	size_t offset;
	int pitch;
	int h;

	dest(int pitch_, int h_, size_t misalign)
		: pitch(pitch_), h(h_)
	{
		mem.assign(static_cast<size_t>(pitch) * h + 256, guard);
		const uintptr_t base = reinterpret_cast<uintptr_t>(mem.data());
		offset = ((64 - (base & 63)) & 63) + misalign; // 64-byte aligned start plus the requested misalignment
	}

	uint8_t* data() { return mem.data() + offset; }
	const uint8_t* data() const { return mem.data() + offset; }
};

struct diff_stats
{
	int max_rgb = 0;
	long long differing = 0;
	long long pixels = 0;
	bool alpha_ok = true;
	bool guards_ok = true;
};

// Compares two outputs pixel by pixel; checks the alpha byte and that bytes outside w*4 of each row are untouched
void compare(const dest& got, const dest& want, int w, int h, bool argb, uint8_t alpha, diff_stats& st)
{
	const int a_index = argb ? 0 : 3;

	for (size_t i = 0; i < got.offset; i++)
	{
		st.guards_ok = st.guards_ok && got.mem[i] == dest::guard;
	}

	for (int row = 0; row < h; row++)
	{
		const uint8_t* g = got.data() + static_cast<size_t>(row) * got.pitch;
		const uint8_t* r = want.data() + static_cast<size_t>(row) * want.pitch;

		for (int x = 0; x < w; x++)
		{
			bool differs = false;

			for (int c = 0; c < 4; c++)
			{
				const int d = std::abs(int{g[4 * x + c]} - int{r[4 * x + c]});

				if (c == a_index)
				{
					st.alpha_ok = st.alpha_ok && g[4 * x + c] == alpha && r[4 * x + c] == alpha;
				}
				else
				{
					st.max_rgb = d > st.max_rgb ? d : st.max_rgb;
					differs = differs || d != 0;
				}
			}

			st.differing += differs;
			st.pixels++;
		}

		for (int x = 4 * w; x < got.pitch; x++)
		{
			st.guards_ok = st.guards_ok && g[x] == dest::guard;
		}
	}

	for (size_t i = got.offset + static_cast<size_t>(got.pitch) * h; i < got.mem.size(); i++)
	{
		st.guards_ok = st.guards_ok && got.mem[i] == dest::guard;
	}
}

// What cellVdecGetPictureExt does on the PC path: alpha plane memset to the alpha byte, then the unscaled converter
void run_reference(const frame& f, dest& out, bool argb, uint8_t alpha)
{
	std::vector<uint8_t> alpha_plane(static_cast<size_t>(f.w) * f.h, alpha);
	const sws::context ctx(argb, f.w);
	const uint8_t* in_data[4] = {f.y.data(), f.u.data(), f.v.data(), alpha_plane.data()};
	const int in_line[4] = {f.y_stride, f.u_stride, f.v_stride, f.w};
	uint8_t* out_data[4] = {out.data(), nullptr, nullptr, nullptr};
	const int out_line[4] = {out.pitch, 0, 0, 0};
	ctx.convert(in_data, in_line, 0, f.h, out_data, out_line);
}

void run_ps4(const frame& f, dest& out, bool argb, uint8_t alpha, bool scalar)
{
	if (scalar)
	{
		utils::ps4::yuv::yuv420_to_rgb32_scalar(f.y.data(), f.y_stride, f.u.data(), f.u_stride, f.v.data(), f.v_stride, out.data(), out.pitch, f.w, f.h, argb, alpha);
	}
	else
	{
		utils::ps4::yuv::yuv420_to_rgb32(f.y.data(), f.y_stride, f.u.data(), f.u_stride, f.v.data(), f.v_stride, out.data(), out.pitch, f.w, f.h, argb, alpha);
	}
}

void report(const char* what, const diff_stats& st)
{
	std::printf("%-58s pixels=%lld max|diff|=%d differing=%lld alpha=%s guards=%s\n", what, st.pixels, st.max_rgb, st.differing,
		st.alpha_ok ? "exact" : "WRONG", st.guards_ok ? "intact" : "OVERWRITTEN");
	CHECK(st.max_rgb <= 1, "%s: max |diff| %d > 1", what, st.max_rgb);
	CHECK(st.alpha_ok, "%s: alpha byte differs", what);
	CHECK(st.guards_ok, "%s: bytes outside the picture were written", what);
}

// Every (Y, U, V) triple once: 64 frames of 512x512 (chroma 256x256, U = column, V = row; Y walks 4 values per
// 2x2 block and frame).
void test_exhaustive(bool argb, bool scalar)
{
	diff_stats st;
	const uint8_t alpha = argb ? 0x80 : 0xff;

	for (int k = 0; k < 64; k++)
	{
		frame f(512, 512, 0, 0, 0);

		for (int r = 0; r < 256; r++)
		{
			for (int c = 0; c < 256; c++)
			{
				f.u[static_cast<size_t>(r) * f.u_stride + c] = static_cast<uint8_t>(c);
				f.v[static_cast<size_t>(r) * f.v_stride + c] = static_cast<uint8_t>(r);

				for (int i = 0; i < 2; i++)
				{
					for (int j = 0; j < 2; j++)
					{
						f.y[static_cast<size_t>(2 * r + i) * f.y_stride + 2 * c + j] = static_cast<uint8_t>(4 * k + 2 * i + j);
					}
				}
			}
		}

		dest got(512 * 4, 512, 0), want(512 * 4, 512, 0);
		run_reference(f, want, argb, alpha);
		run_ps4(f, got, argb, alpha, scalar);
		compare(got, want, f.w, f.h, argb, alpha, st);
	}

	char what[128];
	std::snprintf(what, sizeof(what), "all 2^24 YUV triples, %s, %s", argb ? "argb" : "rgba", scalar ? "scalar" : "sse2");
	report(what, st);
	CHECK(st.pixels == (1ll << 24), "exhaustive frames covered %lld pixels", st.pixels);
}

// Random frames: even sizes 2..1920 (w % 16 != 0 included), padded and odd strides/pitches, aligned and unaligned
// destinations, small and large (non-temporal) pictures, several alpha values.
void test_random(bool scalar)
{
	rng r{0x9e3779b97f4a7c15ull};
	diff_stats st;
	int cases = 0;

	const int widths[] = {2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30, 32, 34, 46, 48, 50, 62, 64, 66, 126, 130, 254, 258,
		318, 320, 322, 510, 514, 638, 640, 642, 718, 720, 722, 1278, 1280, 1282, 1918, 1920};
	const int heights[] = {2, 4, 6, 16, 34};
	const int y_pads[] = {0, 1, 7, 32};
	const int c_pads[] = {0, 3, 5, 16};
	const int pitch_pads[] = {0, 4, 12, 3, 64};
	const size_t misaligns[] = {0, 4, 8, 1};

	for (int w : widths)
	{
		for (int h : heights)
		{
			const int y_pad = y_pads[r.next() % 4];
			const int u_pad = c_pads[r.next() % 4];
			const int v_pad = c_pads[r.next() % 4];
			const int pitch = 4 * w + pitch_pads[r.next() % 5];
			const size_t mis = misaligns[r.next() % 4];
			const bool argb = r.next() & 1;
			const uint8_t alpha = static_cast<uint8_t>(r.next());

			frame f(w, h, y_pad, u_pad, v_pad);
			f.randomize(r);

			dest got(pitch, h, mis), want(pitch, h, mis);
			run_reference(f, want, argb, alpha);
			run_ps4(f, got, argb, alpha, scalar);
			compare(got, want, w, h, argb, alpha, st);
			cases++;
		}
	}

	// Large pictures: 16-byte aligned rows take the non-temporal path, the others the unaligned one
	const int big[][2] = {{1280, 720}, {1920, 1080}, {720, 480}, {1272, 720}, {1280, 718}};

	for (const auto& wh : big)
	{
		for (size_t mis : {size_t{0}, size_t{4}})
		{
			for (int pitch_pad : {0, 12})
			{
				const bool argb = r.next() & 1;
				const uint8_t alpha = static_cast<uint8_t>(r.next());
				frame f(wh[0], wh[1], 32, 16, 16);
				f.randomize(r);

				dest got(4 * wh[0] + pitch_pad, wh[1], mis), want(4 * wh[0] + pitch_pad, wh[1], mis);
				run_reference(f, want, argb, alpha);
				run_ps4(f, got, argb, alpha, scalar);
				compare(got, want, wh[0], wh[1], argb, alpha, st);
				cases++;
			}
		}
	}

	char what[128];
	std::snprintf(what, sizeof(what), "%d random frames (2..1920 wide, odd strides), %s", cases, scalar ? "scalar" : "sse2");
	report(what, st);
}

// Odd sizes: swscale does not take this table path for them (cellVdecGetPictureExt keeps swscale there), so the
// converter is compared with the per-pixel table formula and checked not to write outside the picture.
void test_odd_sizes()
{
	rng r{12345};
	bool ok = true;
	bool guards = true;

	for (int w : {1, 3, 15, 17, 33, 641})
	{
		for (int h : {1, 3, 5, 7})
		{
			frame f(w, h, 5, 3, 1);
			f.randomize(r);
			const bool argb = r.next() & 1;
			const uint8_t alpha = static_cast<uint8_t>(r.next());
			dest got(4 * w + 8, h, 4);
			run_ps4(f, got, argb, alpha, false);

			const sws::context ctx(argb, w);

			for (int row = 0; row < h; row++)
			{
				for (int x = 0; x < w; x++)
				{
					const int Y = f.y[static_cast<size_t>(row) * f.y_stride + x];
					const int U = f.u[static_cast<size_t>(row / 2) * f.u_stride + x / 2];
					const int V = f.v[static_cast<size_t>(row / 2) * f.v_stride + x / 2];
					const int64_t rp = ctx.table_rV[V + 512];
					const int64_t gp = ctx.table_gU[U + 512] + ctx.table_gV[V + 512];
					const int64_t bp = ctx.table_bU[U + 512];
					const uint32_t value = ctx.at(rp, Y) + ctx.at(gp, Y) + ctx.at(bp, Y) + (static_cast<uint32_t>(alpha) << (argb ? 0 : 24));
					uint32_t have;
					std::memcpy(&have, got.data() + static_cast<size_t>(row) * got.pitch + 4 * x, 4);
					ok = ok && have == value;
				}

				for (int x = 4 * w; x < got.pitch; x++)
				{
					guards = guards && got.data()[static_cast<size_t>(row) * got.pitch + x] == dest::guard;
				}
			}
		}
	}

	std::printf("%-58s %s, guards %s\n", "odd sizes vs the per-pixel table formula", ok ? "identical" : "DIFFERENT", guards ? "intact" : "OVERWRITTEN");
	CHECK(ok, "odd-size output differs from the table formula");
	CHECK(guards, "odd-size conversion wrote outside the picture");
}

// Zero sizes must not touch memory
void test_empty()
{
	uint8_t byte = 0x5a;
	utils::ps4::yuv::yuv420_to_rgb32(nullptr, 0, nullptr, 0, nullptr, 0, &byte, 0, 0, 16, true, 0);
	utils::ps4::yuv::yuv420_to_rgb32(nullptr, 0, nullptr, 0, nullptr, 0, &byte, 0, 16, 0, true, 0);
	CHECK(byte == 0x5a, "empty picture wrote memory");
}

void timing()
{
	rng r{777};
	frame f(1280, 720, 0, 0, 0);
	f.randomize(r);
	dest a(1280 * 4, 720, 0), b(1280 * 4, 720, 0);

	const auto t0 = std::chrono::steady_clock::now();
	for (int i = 0; i < 20; i++) run_ps4(f, a, true, 0xff, false);
	const auto t1 = std::chrono::steady_clock::now();
	for (int i = 0; i < 5; i++) run_reference(f, b, true, 0xff);
	const auto t2 = std::chrono::steady_clock::now();

	const double sse = std::chrono::duration<double, std::milli>(t1 - t0).count() / 20;
	const double ref = std::chrono::duration<double, std::milli>(t2 - t1).count() / 5;
	std::printf("1280x720 argb on this PC (-O1, informative): sse2 %.2f ms, reference port %.2f ms\n", sse, ref);
}
}

int main()
{
	std::printf("table: cy=%lld crv=%lld cbu=%lld cgu=%lld cgv=%lld, index range %d..%d, simd=%d\n",
		static_cast<long long>(utils::ps4::yuv::detail::cy), static_cast<long long>(utils::ps4::yuv::detail::crv),
		static_cast<long long>(utils::ps4::yuv::detail::cbu), static_cast<long long>(utils::ps4::yuv::detail::cgu),
		static_cast<long long>(utils::ps4::yuv::detail::cgv), utils::ps4::yuv::detail::t_min, utils::ps4::yuv::detail::t_max,
		utils::ps4::yuv::yuv420_to_rgb32_simd ? 1 : 0);

	CHECK(utils::ps4::yuv::yuv420_to_rgb32_simd, "the SSE2 path is not compiled in");

	test_exhaustive(true, false);
	test_exhaustive(false, false);
	test_exhaustive(true, true);
	test_exhaustive(false, true);
	test_random(false);
	test_random(true);
	test_odd_sizes();
	test_empty();
	timing();

	if (g_failures)
	{
		std::printf("vdec_convert_test: %d FAILURE(S)\n", g_failures);
		return 1;
	}

	std::puts("vdec_convert_test: all checks passed");
	return 0;
}
