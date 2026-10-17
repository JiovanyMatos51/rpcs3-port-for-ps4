// Host test for the PS4 TSC conversion in rpcs3/Emu/Cell/lv2/sys_time.cpp (ps4_tsc_divider, ps4_tsc_scale).
// get_system_time() and get_timebased_time() must stay bit-identical to the generic
// 'tsc / freq * mul + tsc % freq * mul / freq' for every input: the SPU decrementer compares the JIT's own exact
// division against ch_dec_start_timestamp. The divider below is a verbatim copy; main() also checks that the
// key lines are still present in sys_time.cpp (pass its path as argv[1]).
//
// Build: clang++ -static -std=c++20 -O1 tsc_div_test.cpp -o tsc_div_test.exe
// Run:   tsc_div_test.exe rpcs3/Emu/Cell/lv2/sys_time.cpp   (from the repository root)

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using u64 = std::uint64_t;

namespace utils
{
	constexpr u64 umulh64(u64 x, u64 y)
	{
		return static_cast<u64>((static_cast<unsigned __int128>(x) * y) >> 64);
	}
}

// ---- copy of sys_time.cpp ----
namespace
{
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

	// Same body as sys_time.cpp, with the divider passed in instead of a function-local static
	u64 ps4_tsc_scale_with(const ps4_tsc_divider& s_div, u64 tsc, u64 freq, u64 mul) noexcept
	{
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
// ---- end of copy ----

namespace
{
	u64 g_checks = 0;
	u64 g_failures = 0;

	struct rng
	{
		u64 state;

		u64 next() noexcept
		{
			// splitmix64
			u64 z = (state += 0x9e3779b97f4a7c15ull);
			z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ull;
			z = (z ^ (z >> 27)) * 0x94d049bb133111ebull;
			return z ^ (z >> 31);
		}

		// Random value with a random bit length (1..64), so small and large magnitudes are both covered
		u64 next_sized() noexcept
		{
			const unsigned bits = static_cast<unsigned>(next() % 64) + 1;
			const u64 v = next();
			return bits == 64 ? v : (v & ((1ull << bits) - 1));
		}
	};

	inline void check_div(const ps4_tsc_divider& d, u64 n)
	{
		u64 rem = 0;
		const u64 q = d.divide(n, rem);
		g_checks++;

		if (q != n / d.div || rem != n % d.div) [[unlikely]]
		{
			if (g_failures++ < 20)
			{
				std::printf("FAIL divide: n=%llu d=%llu -> q=%llu r=%llu, expected q=%llu r=%llu\n",
					static_cast<unsigned long long>(n), static_cast<unsigned long long>(d.div), static_cast<unsigned long long>(q),
					static_cast<unsigned long long>(rem), static_cast<unsigned long long>(n / d.div), static_cast<unsigned long long>(n % d.div));
			}
		}
	}

	inline void check_scale(const ps4_tsc_divider& d, u64 tsc, u64 mul)
	{
		const u64 freq = d.div;
		const u64 expected = tsc / freq * mul + tsc % freq * mul / freq;
		const u64 got = ps4_tsc_scale_with(d, tsc, freq, mul);
		g_checks++;

		if (got != expected) [[unlikely]]
		{
			if (g_failures++ < 20)
			{
				std::printf("FAIL scale: tsc=%llu freq=%llu mul=%llu -> %llu, expected %llu\n", static_cast<unsigned long long>(tsc),
					static_cast<unsigned long long>(freq), static_cast<unsigned long long>(mul), static_cast<unsigned long long>(got),
					static_cast<unsigned long long>(expected));
			}
		}
	}

	// Edge numerators for divisor d: around 0, around multiples of d (small and largest), around powers of two, top of range
	void edge_numerators(u64 d, std::vector<u64>& out)
	{
		out.clear();
		const u64 top = ~0ull;
		const u64 qmax = top / d;

		for (u64 v : {u64{0}, u64{1}, u64{2}, u64{3}})
		{
			out.push_back(v);
			out.push_back(top - v);
		}

		for (u64 k : {u64{1}, u64{2}, u64{3}, u64{7}, u64{1000}, qmax / 2, qmax - 2, qmax - 1, qmax})
		{
			if (k == 0 || k > qmax)
			{
				continue;
			}

			const u64 m = k * d; // no overflow: k <= qmax
			out.push_back(m);
			out.push_back(m - 1);
			if (m != top) out.push_back(m + 1);
			if (m <= top - (d - 1)) out.push_back(m + (d - 1));
		}

		for (unsigned b = 1; b < 64; b++)
		{
			const u64 p = 1ull << b;
			out.push_back(p - 1);
			out.push_back(p);
			out.push_back(p + 1);
		}
	}

	std::vector<u64> divisor_set(rng& r)
	{
		std::vector<u64> ds;

		// Every small divisor
		for (u64 d = 1; d <= 4096; d++) ds.push_back(d);

		// Powers of two and their neighbours
		for (unsigned b = 0; b < 64; b++)
		{
			const u64 p = 1ull << b;
			ds.push_back(p);
			if (p > 1) ds.push_back(p - 1);
			ds.push_back(p + 1);
		}

		// TSC rates seen on the console and nearby values (kernel, calibrated, Pro, Boost Mode)
		for (u64 f : {1594000000ull, 1600000000ull, 1583000000ull, 1584000000ull, 1586000000ull, 1599999999ull, 1600000001ull,
			 2080000000ull, 2081000000ull, 2086000000ull, 2133000000ull, 2133333333ull, 1000000000ull, 3000000000ull, 4294967295ull,
			 4294967296ull, 4294967297ull, 10000000000ull, 100000000ull, 80000000ull, 1000000ull, 24000000ull, 19200000ull})
		{
			ds.push_back(f);
		}

		// Largest divisors
		for (u64 v : {~0ull, ~0ull - 1, ~0ull / 2, ~0ull / 2 + 1, ~0ull / 3, ~0ull / 3 + 1, 0x8000000000000001ull, 0xfffffffffffffff0ull})
		{
			ds.push_back(v);
		}

		// Random divisors of every size
		for (int i = 0; i < 20000; i++)
		{
			u64 d = r.next_sized();
			ds.push_back(d ? d : 1);
		}

		return ds;
	}

	bool check_source(const char* path)
	{
		std::ifstream file(path, std::ios::binary);

		if (!file)
		{
			std::printf("FAIL: cannot open %s\n", path);
			return false;
		}

		std::stringstream ss;
		ss << file.rdbuf();
		const std::string text = ss.str();

		const char* const needles[] =
		{
			"struct ps4_tsc_divider",
			", magic(~0ull / d)",
			"u64 q = utils::umulh64(n, magic);",
			"u64 r = n - q * div;",
			"if (r >= div)",
			"static const ps4_tsc_divider s_div(freq);",
			"if (s_div.div != freq) [[unlikely]]",
			"return tsc / freq * mul + tsc % freq * mul / freq;",
			"const u64 whole = s_div.divide(tsc, rem);",
			"return whole * mul + s_div.divide(rem * mul, unused);",
			"const u64 result = ps4_tsc_scale(tsc, freq, g_timebase_freq) * g_cfg.core.clocks_scale / 100u;",
			"const u64 result = ps4_tsc_scale(tsc, freq, 1000000ull);",
		};

		bool ok = true;

		for (const char* needle : needles)
		{
			if (text.find(needle) == std::string::npos)
			{
				std::printf("FAIL: sys_time.cpp no longer contains: %s\n", needle);
				ok = false;
			}
		}

		return ok;
	}
}

int main(int argc, char** argv)
{
	rng r{0x0123456789abcdefull};
	std::vector<u64> edges;

	// 1. Edge numerators for every divisor of the set
	const std::vector<u64> ds = divisor_set(r);

	for (const u64 d : ds)
	{
		const ps4_tsc_divider div(d);
		edge_numerators(d, edges);

		for (const u64 n : edges)
		{
			check_div(div, n);
		}

		// Random numerators of every size, and random multiples of d with small offsets
		for (int i = 0; i < 256; i++)
		{
			check_div(div, r.next_sized());
			check_div(div, r.next());

			const u64 qmax = ~0ull / d;
			const u64 k = qmax ? r.next() % qmax + 1 : 0;
			const u64 m = k * d;
			check_div(div, m);
			check_div(div, m - 1);
			if (m <= ~0ull - (d - 1)) check_div(div, m + (d - 1));
		}
	}

	std::printf("edge + random per divisor: %zu divisors, %llu checks, %llu failures\n", ds.size(), static_cast<unsigned long long>(g_checks),
		static_cast<unsigned long long>(g_failures));

	// 2. Exhaustive: every divisor 1..2048 against every numerator in [0, 65536) and in [2^64 - 65536, 2^64)
	for (u64 d = 1; d <= 2048; d++)
	{
		const ps4_tsc_divider div(d);

		for (u64 n = 0; n < 65536; n++)
		{
			check_div(div, n);
			check_div(div, ~0ull - n);
		}
	}

	// Exhaustive numerators [0, 2^24) for the console TSC rates and the 1 MHz / 80 MHz scale factors' products
	for (const u64 d : {1594000000ull, 1600000000ull, 2133000000ull, 80000000ull, 1000000ull})
	{
		const ps4_tsc_divider div(d);

		for (u64 n = 0; n < (1ull << 24); n++)
		{
			check_div(div, n);
		}
	}

	std::printf("after exhaustive ranges: %llu checks, %llu failures\n", static_cast<unsigned long long>(g_checks), static_cast<unsigned long long>(g_failures));

	// 3. Random pairs of every size
	for (int i = 0; i < 40'000'000; i++)
	{
		u64 d = r.next_sized();
		d = d ? d : 1;
		const ps4_tsc_divider div(d);
		check_div(div, r.next_sized());
		check_div(div, r.next());
	}

	std::printf("after random pairs: %llu checks, %llu failures\n", static_cast<unsigned long long>(g_checks), static_cast<unsigned long long>(g_failures));

	// 4. The complete conversion of get_system_time (x 1000000) and get_timebased_time (x 80000000), including the
	// generic expression's u64 wrap-around for huge TSC values and frequencies
	const u64 muls[] = {1000000ull, 80000000ull};

	for (const u64 freq : ds)
	{
		const ps4_tsc_divider div(freq);

		for (const u64 mul : muls)
		{
			edge_numerators(freq, edges);

			for (const u64 tsc : edges)
			{
				check_scale(div, tsc, mul);
			}

			for (int i = 0; i < 64; i++)
			{
				check_scale(div, r.next_sized(), mul);
				check_scale(div, r.next(), mul);
			}
		}
	}

	// Realistic uptimes: TSC counts up to 2^56 (about 1.4 years at 1.6 GHz), every console rate, all remainders near freq
	for (const u64 freq : {1594000000ull, 1600000000ull, 1583000000ull, 2080000000ull, 2133000000ull})
	{
		const ps4_tsc_divider div(freq);

		for (const u64 mul : muls)
		{
			for (int i = 0; i < 2'000'000; i++)
			{
				check_scale(div, r.next() >> 8, mul);
			}

			for (u64 sec = 0; sec < 4096; sec++)
			{
				const u64 base = sec * freq;

				for (u64 off : {u64{0}, u64{1}, freq / 2, freq - 2, freq - 1})
				{
					check_scale(div, base + off, mul);
				}
			}
		}
	}

	// A divider that does not match the frequency takes the generic path
	{
		const ps4_tsc_divider other(1600000000ull);

		for (int i = 0; i < 100000; i++)
		{
			const u64 tsc = r.next();
			const u64 freq = 1594000000ull;
			const u64 expected = tsc / freq * 1000000ull + tsc % freq * 1000000ull / freq;
			g_checks++;

			if (ps4_tsc_scale_with(other, tsc, freq, 1000000ull) != expected)
			{
				g_failures++;
			}
		}
	}

	std::printf("after conversions: %llu checks, %llu failures\n", static_cast<unsigned long long>(g_checks), static_cast<unsigned long long>(g_failures));

	// Informative only: host timing of the old expression and the divider (Jaguar's DIV r64 is much slower than a host's)
	{
		const u64 freq = 1594000000ull;
		const ps4_tsc_divider div(freq);
		volatile u64 sink = 0;
		u64 tsc = 0x0001234567890abcull;
		const int iters = 20'000'000;

		auto t0 = std::chrono::steady_clock::now();
		for (int i = 0; i < iters; i++)
		{
			volatile u64 f = freq;
			const u64 fv = f;
			sink = sink + (tsc / fv * 1000000ull + tsc % fv * 1000000ull / fv);
			tsc += 1237;
		}
		auto t1 = std::chrono::steady_clock::now();
		for (int i = 0; i < iters; i++)
		{
			sink = sink + ps4_tsc_scale_with(div, tsc, freq, 1000000ull);
			tsc += 1237;
		}
		auto t2 = std::chrono::steady_clock::now();

		const double old_ns = std::chrono::duration<double, std::nano>(t1 - t0).count() / iters;
		const double new_ns = std::chrono::duration<double, std::nano>(t2 - t1).count() / iters;
		std::printf("host timing (informative): DIV path %.2f ns, divider %.2f ns per conversion\n", old_ns, new_ns);
	}

	bool source_ok = true;

	if (argc > 1)
	{
		source_ok = check_source(argv[1]);
		std::printf("source check of %s: %s\n", argv[1], source_ok ? "ok" : "FAILED");
	}
	else
	{
		std::printf("source check skipped (pass the path of sys_time.cpp)\n");
	}

	if (g_failures || !source_ok)
	{
		std::printf("tsc_div_test: FAILED (%llu failures in %llu checks)\n", static_cast<unsigned long long>(g_failures), static_cast<unsigned long long>(g_checks));
		return 1;
	}

	std::printf("tsc_div_test: OK (%llu checks)\n", static_cast<unsigned long long>(g_checks));
	return 0;
}
