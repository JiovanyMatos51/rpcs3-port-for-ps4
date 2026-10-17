// Host test for ps4-port/runtime/ps4_fenv.cpp (functional x86-64 fenv for the PS4 build).
// The runtime file is compiled into this test with every C symbol prefixed by ps4_test_, so the
// host CRT's own fenv is never used or replaced.
//
// Build (llvm-mingw, Windows host):
//   clang++ -std=c++20 -O2 -static -o fenv-test.exe ps4-port/tests/fenv_test.cpp -lpthread
#define PS4_FENV_HOST_TEST 1
#include "../runtime/ps4_fenv.cpp"

#include <bit>
#include <cstdint>
#include <iterator>
#include <cstdio>
#include <cstdlib>

namespace
{
int failures = 0;

void check(bool condition, const char* what, unsigned long long a = 0, unsigned long long b = 0)
{
    if (!condition)
    {
        ++failures;
        std::printf("FAIL: %s (0x%llx vs 0x%llx)\n", what, a, b);
    }
}

constexpr int modes[] = {fe_tonearest, fe_downward, fe_upward, fe_towardzero};
const char* const mode_names[] = {"nearest", "downward", "upward", "towardzero"};

// Exact int32 -> float32 conversion under a rounding mode, in integer arithmetic only.
std::uint32_t reference_cvt(std::int32_t value, int mode)
{
    if (value == 0) return 0;
    const std::uint32_t sign = value < 0 ? 1u : 0u;
    std::uint64_t mag = sign ? std::uint64_t{0} - static_cast<std::uint64_t>(static_cast<std::int64_t>(value))
                             : static_cast<std::uint64_t>(value);
    mag &= 0xffffffffull;
    const int width = std::bit_width(mag);
    int shift = width > 24 ? width - 24 : 0;
    std::uint64_t mant = mag >> shift;
    const std::uint64_t rem = shift ? mag & ((1ull << shift) - 1) : 0;
    if (rem)
    {
        const std::uint64_t half = 1ull << (shift - 1);
        bool up = false;
        switch (mode)
        {
        case fe_tonearest: up = rem > half || (rem == half && (mant & 1)); break;
        case fe_upward: up = !sign; break;
        case fe_downward: up = sign; break;
        default: break;
        }
        if (up && ++mant == (1ull << 24))
        {
            mant >>= 1;
            ++shift;
        }
    }
    // Normalise small values to a 24-bit mantissa.
    int exponent = shift;
    while (mant < (1ull << 23))
    {
        mant <<= 1;
        --exponent;
    }
    return (sign << 31) | static_cast<std::uint32_t>(127 + 23 + exponent) << 23 | static_cast<std::uint32_t>(mant & 0x7fffff);
}

[[gnu::noinline]] void cvt4(const std::int32_t (&in)[4], std::uint32_t (&out)[4])
{
    // Volatile copies keep the conversion between the surrounding MXCSR writes.
    volatile std::int32_t a = in[0], b = in[1], c = in[2], d = in[3];
    const __m128 f = _mm_cvtepi32_ps(_mm_set_epi32(d, c, b, a));
    alignas(16) std::uint32_t tmp[4];
    _mm_store_si128(reinterpret_cast<__m128i*>(tmp), _mm_castps_si128(f));
    for (int i = 0; i < 4; ++i) static_cast<volatile std::uint32_t&>(out[i]) = tmp[i];
}

// spu_recompiler::CLZ (SPUASMJITRecompiler.cpp), instruction for instruction.
[[gnu::noinline]] void spu_asmjit_clz(const std::uint32_t (&in)[4], std::uint32_t (&out)[4])
{
    volatile std::uint32_t a0 = in[0], a1 = in[1], a2 = in[2], a3 = in[3];
    const __m128i va = _mm_set_epi32(static_cast<int>(a3), static_cast<int>(a2), static_cast<int>(a1), static_cast<int>(a0));
    constexpr int exp_bias = 127;
    const __m128i vf = _mm_srli_epi32(_mm_castps_si128(_mm_cvtepi32_ps(va)), 23);
    __m128i v1 = _mm_cmpeq_epi32(_mm_setzero_si128(), va);
    v1 = _mm_and_si128(v1, _mm_set1_epi32(32 ^ (31 + exp_bias)));
    v1 = _mm_xor_si128(v1, _mm_set1_epi32(31 + exp_bias));
    v1 = _mm_subs_epu16(v1, vf);
    alignas(16) std::uint32_t tmp[4];
    _mm_store_si128(reinterpret_cast<__m128i*>(tmp), v1);
    for (int i = 0; i < 4; ++i) static_cast<volatile std::uint32_t&>(out[i]) = tmp[i];
}

std::uint32_t rng_state = 0x9e3779b9;
std::uint32_t next()
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 17;
    rng_state ^= rng_state << 5;
    return rng_state;
}

void test_selftest()
{
    const bool ok = ps4_spu_rounding_ok();
    std::printf("%s\n", ps4_fpu_selftest_detail());
    check(ok, "ps4_spu_rounding_ok()");
    check(ps4_spu_rounding_ok() == ok && ps4_fpu_selftest_detail() == ps4_fpu_selftest_detail(), "self-test is cached");
}

void test_rounding_modes()
{
    check(ps4_test_fegetround() == fe_tonearest, "default rounding is nearest", ps4_test_fegetround(), 0);
    static const std::int32_t edges[] = {0, 1, -1, 0x7fffffff, -0x7fffffff, INT32_MIN, 0x01ffffff, -0x01ffffff, 0x00ffffff,
        0x01000001, 0x01000003, 0x3fffffff, 0x7fffff80, 0x7fffffc0, 0x7fffffbf, 0x40000040, 0x40000041, -0x40000040};
    std::size_t converted = 0;
    for (int m = 0; m < 4; ++m)
    {
        const int mode = modes[m];
        check(ps4_test_fesetround(mode) == 0, mode_names[m]);
        check(ps4_test_fegetround() == mode, "fegetround matches", ps4_test_fegetround(), mode);
        check((read_mxcsr() & c_mxcsr_rc) == static_cast<std::uint32_t>(mode) << 3, "MXCSR.RC", read_mxcsr(), mode);
        check((read_x87_cw() & c_x87_rc) == mode, "x87 RC", read_x87_cw(), mode);

        std::int32_t in[4];
        std::uint32_t out[4];
        for (std::size_t i = 0; i < std::size(edges); i += 4)
        {
            for (std::size_t j = 0; j < 4; ++j) in[j] = edges[(i + j) % std::size(edges)];
            cvt4(in, out);
            for (std::size_t j = 0; j < 4; ++j)
            {
                check(out[j] == reference_cvt(in[j], mode), "cvtdq2ps edge", static_cast<std::uint32_t>(in[j]), out[j]);
                ++converted;
            }
        }
        for (int pass = 0; pass < 100000; ++pass)
        {
            for (auto& v : in)
            {
                // Mix plain random values with long runs of ones (the SPURS bit-mask pattern).
                const std::uint32_t r = next();
                v = static_cast<std::int32_t>((pass & 1) ? r : (r | ((1u << (r % 31)) - 1)) >> (r % 7));
            }
            cvt4(in, out);
            for (std::size_t j = 0; j < 4; ++j)
            {
                check(out[j] == reference_cvt(in[j], mode), "cvtdq2ps random", static_cast<std::uint32_t>(in[j]), out[j]);
                ++converted;
            }
            if (failures > 20) std::exit(1);
        }
    }
    check(ps4_test_fesetround(0x123) == -1 && ps4_test_fesetround(fe_towardzero | 1) == -1, "invalid modes rejected");
    check(ps4_test_fegetround() == fe_towardzero, "invalid mode kept the previous mode");
    ps4_test_fesetround(fe_tonearest);
    std::printf("Rounding modes: 4 modes, %zu cvtdq2ps conversions match the integer reference\n", converted);
}

void test_spu_clz()
{
    // Under RZ the recompilers' CLZ equals countl_zero for every input; under RN it must not
    // (that is the 0.24 black-screen bug, e.g. 0x7fffffff -> 0 instead of 1).
    std::size_t rz_checked = 0, rn_wrong = 0;
    for (int mode : {fe_towardzero, fe_tonearest})
    {
        ps4_test_fesetround(mode);
        std::uint32_t in[4], out[4];
        for (int pass = 0; pass < 200000; ++pass)
        {
            for (auto& v : in)
            {
                const std::uint32_t r = next();
                v = (pass & 1) ? r >> (r % 32) : ~0u >> (1 + r % 31) & ~((1u << (r % 8)) - 1);
            }
            if (pass == 0) { in[0] = 0x7fffffff; in[1] = 0x01ffffff; in[2] = 0; in[3] = 0x80000000; }
            spu_asmjit_clz(in, out);
            for (int j = 0; j < 4; ++j)
            {
                const auto expected = static_cast<std::uint32_t>(std::countl_zero(in[j]));
                if (mode == fe_towardzero)
                {
                    check(out[j] == expected, "SPU CLZ under RZ", in[j], out[j]);
                    ++rz_checked;
                }
                else if (out[j] != expected)
                {
                    ++rn_wrong;
                }
            }
        }
    }
    ps4_test_fesetround(fe_tonearest);
    check(rn_wrong > 0, "SPU CLZ is wrong under RN (test must discriminate)");
    std::printf("SPU CLZ (ASMJIT sequence): %zu values exact under RZ; %zu wrong under RN (expected > 0)\n", rz_checked, rn_wrong);
}

void x87_divide()
{
    // 1/3 on the x87 stack raises the x87 precision (inexact) flag only.
    const float one = 1.0f, three = 3.0f;
    float result;
    __asm__ volatile("flds %1\n\tfdivs %2\n\tfstps %0" : "=m"(result) : "m"(one), "m"(three) : "memory", "st");
    (void)result;
}

void test_exceptions()
{
    ps4_test_feclearexcept(fe_all_except);
    check(ps4_test_fetestexcept(fe_all_except) == 0, "flags clear");
    check(ps4_test_feraiseexcept(fe_overflow | fe_inexact) == 0, "feraiseexcept");
    check(ps4_test_fetestexcept(fe_all_except) == (fe_overflow | fe_inexact), "raised flags",
        ps4_test_fetestexcept(fe_all_except), fe_overflow | fe_inexact);
    ps4_test_feclearexcept(fe_overflow);
    check(ps4_test_fetestexcept(fe_all_except) == fe_inexact, "partial clear", ps4_test_fetestexcept(fe_all_except), fe_inexact);
    ps4_test_feclearexcept(fe_all_except);

    // SSE division sets MXCSR.PE.
    volatile float a = 1.0f, b = 3.0f;
    volatile float c = a / b;
    (void)c;
    check(ps4_test_fetestexcept(fe_inexact) == fe_inexact, "SSE inexact seen");
    ps4_test_feclearexcept(fe_all_except);

    // x87 flags are reported and cleared too (musl policy: moved into MXCSR, then cleared).
    x87_divide();
    check((read_x87_sw() & fe_inexact) != 0, "x87 division sets PE", read_x87_sw(), fe_inexact);
    check(ps4_test_fetestexcept(fe_inexact) == fe_inexact, "x87 inexact seen");
    ps4_test_feclearexcept(fe_inexact);
    check(ps4_test_fetestexcept(fe_all_except) == 0, "x87 inexact cleared", ps4_test_fetestexcept(fe_all_except), 0);
    check((read_x87_sw() & fe_all_except) == 0, "x87 status word cleared", read_x87_sw(), 0);

    except_type flags = 0;
    ps4_test_feraiseexcept(fe_divbyzero);
    check(ps4_test_fegetexceptflag(&flags, fe_all_except) == 0 && flags == fe_divbyzero, "fegetexceptflag", flags, fe_divbyzero);
    ps4_test_feclearexcept(fe_all_except);
    flags = fe_underflow | fe_invalid;
    ps4_test_fesetexceptflag(&flags, fe_underflow);
    check(ps4_test_fetestexcept(fe_all_except) == fe_underflow, "fesetexceptflag honours the mask",
        ps4_test_fetestexcept(fe_all_except), fe_underflow);
    ps4_test_feclearexcept(fe_all_except);
    std::printf("Exception flags: raise/test/clear, SSE + x87 flags, get/set exceptflag passed\n");
}

void test_environment()
{
    // Build a distinctive state: RZ, FTZ/DAZ (as SPU threads), pending overflow.
    ps4_test_fesetround(fe_towardzero);
    _mm_setcsr(_mm_getcsr() | _MM_FLUSH_ZERO_ON | _MM_DENORMALS_ZERO_ON);
    ps4_test_feraiseexcept(fe_overflow);
    const std::uint32_t saved_csr = read_mxcsr();
    const std::uint16_t saved_cw = read_x87_cw();

    env_type env{};
    check(ps4_test_fegetenv(&env) == 0, "fegetenv");
    check(env.__mxcsr == saved_csr, "fegetenv stores MXCSR at byte 28", env.__mxcsr, saved_csr);
    check(env.__control_word == saved_cw, "fegetenv stores the x87 control word", env.__control_word, saved_cw);
    check(read_x87_cw() == saved_cw, "fegetenv keeps the x87 control word", read_x87_cw(), saved_cw);

    // Change everything, then restore.
    ps4_test_fesetround(fe_upward);
    ps4_test_feclearexcept(fe_all_except);
    _mm_setcsr(_mm_getcsr() & ~(_MM_FLUSH_ZERO_ON | _MM_DENORMALS_ZERO_ON));
    check(ps4_test_fesetenv(&env) == 0, "fesetenv");
    check(read_mxcsr() == saved_csr, "fesetenv restores MXCSR", read_mxcsr(), saved_csr);
    check(read_x87_cw() == saved_cw, "fesetenv restores the x87 control word", read_x87_cw(), saved_cw);
    check(ps4_test_fegetround() == fe_towardzero, "fesetenv restores RZ");
    check(ps4_test_fetestexcept(fe_overflow) == fe_overflow, "fesetenv restores flags");

    // feholdexcept clears flags, feupdateenv restores the environment and re-raises the new flags.
    env_type held{};
    ps4_test_feholdexcept(&held);
    check(ps4_test_fetestexcept(fe_all_except) == 0, "feholdexcept clears flags");
    ps4_test_fesetround(fe_downward);
    ps4_test_feraiseexcept(fe_underflow);
    ps4_test_feupdateenv(&held);
    check(ps4_test_fegetround() == fe_towardzero, "feupdateenv restores the rounding mode");
    check(ps4_test_fetestexcept(fe_all_except) == (fe_overflow | fe_underflow), "feupdateenv merges flags",
        ps4_test_fetestexcept(fe_all_except), fe_overflow | fe_underflow);

    // FE_DFL_ENV.
    check(ps4_test_fesetenv(default_env) == 0, "fesetenv(FE_DFL_ENV)");
    check(read_mxcsr() == c_default_mxcsr, "default MXCSR", read_mxcsr(), c_default_mxcsr);
    check((read_x87_cw() & 0x1f3f) == (c_default_x87_cw & 0x1f3f), "default x87 control word", read_x87_cw(), c_default_x87_cw);
    check(ps4_test_fegetround() == fe_tonearest && ps4_test_fetestexcept(fe_all_except) == 0, "default rounding and flags");
    std::printf("Environment: fegetenv/fesetenv round trip, feholdexcept/feupdateenv, FE_DFL_ENV passed\n");
}
}

int main()
{
    test_selftest();
    test_rounding_modes();
    test_spu_clz();
    test_exceptions();
    test_environment();
    if (failures)
    {
        std::printf("PS4 fenv test FAILED (%d checks)\n", failures);
        return 1;
    }
    std::printf("PS4 fenv: self-test, rounding modes, SPU CLZ, exception flags and environment passed\n");
    return 0;
}
