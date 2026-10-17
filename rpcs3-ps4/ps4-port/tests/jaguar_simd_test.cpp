#include "util/simd.hpp"
#include <cassert>
#include <cstdio>

// These tests exercise inline SIMD arithmetic only, without RPCS3's runtime
// code generator or thread manager. Disable that unrelated AVX dispatch.
namespace utils
{
u64 _get_main_tid() { return 1; }
bool has_avx() { return false; }
}

int main()
{
    // Verify the original unsigned VMX/SPU helpers against scalar arithmetic,
    // including sign boundaries where signed SSE substitutions would be wrong.
    u32 seed = 0x71393;
    auto next = [&] { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; };
    for (int pass = 0; pass < 10000; ++pass)
    {
        v128 a{}, b{};
        for (int i = 0; i < 4; ++i) { a._u32[i] = next(); b._u32[i] = next(); }
        const auto gt16 = gv_gtu16(a, b), gt32 = gv_gtu32(a, b);
        const auto min16 = gv_minu16(a, b), max16 = gv_maxu16(a, b);
        const auto min32 = gv_minu32(a, b), max32 = gv_maxu32(a, b);
        for (int i = 0; i < 8; ++i)
        {
            assert(gt16._u16[i] == (a._u16[i] > b._u16[i] ? 0xffff : 0));
            assert(min16._u16[i] == std::min(a._u16[i], b._u16[i]));
            assert(max16._u16[i] == std::max(a._u16[i], b._u16[i]));
        }
        for (int i = 0; i < 4; ++i)
        {
            assert(gt32._u32[i] == (a._u32[i] > b._u32[i] ? 0xffffffffu : 0u));
            assert(min32._u32[i] == std::min(a._u32[i], b._u32[i]));
            assert(max32._u32[i] == std::max(a._u32[i], b._u32[i]));
        }
        const auto sums = gv_haddu8x4(a);
        const auto signed_sums = gv_hadds8x4(a, b);
        for (int i = 0; i < 4; ++i)
        {
            u32 sum = 0, signed_sum = b._u32[i];
            for (int j = 0; j < 4; ++j)
            { sum += a._u8[i * 4 + j]; signed_sum += static_cast<s32>(a._s8[i * 4 + j]); }
            assert(sums._u32[i] == sum && signed_sums._u32[i] == signed_sum);
        }
    }
    std::puts("Original RPCS3 integer SIMD: 10,000 comparison/min/max/reduction vectors passed");
}
