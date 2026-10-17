// Functional x86-64 <fenv.h> for the PS4 build (0.25).
//
// The OpenOrbis SDK libc.a ships musl's architecture-neutral fenv stubs: fenv.lo defines
// feclearexcept, feraiseexcept, fetestexcept, fegetround, __fesetround, fegetenv and fesetenv as
// "xor eax,eax; ret", and fesetround.lo validates its argument and tail-calls the dummy
// __fesetround. std::fesetround(FE_TOWARDZERO) in spu_thread::cpu_task therefore did nothing, SPU
// threads ran with MXCSR.RC = round-to-nearest, and the SPU recompilers' cvtdq2ps-based CLZ was off
// by one for words with 25+ leading significant ones, which made the libsre SPURS context-save loop
// spin forever (black screen with SPU ASMJIT/LLVM).
//
// These definitions follow musl's src/fenv/x86_64/fenv.s (MXCSR for SSE plus the x87 control and
// status words). Every symbol fenv.lo and fesetround.lo provide is defined here, so lld never
// extracts those members. The file must stay in the OBJECT target rpcs3_ps4_frontend: object files
// are linked before every archive, while a definition inside a static archive could lose against
// libc.a's lazy member and silently bring the dummies back. ps4-port/verify-native-dispatch.py
// rejects an ELF in which fegetround/fetestexcept/fesetround/__fesetround are 3-byte stubs.
//
// PS4_FENV_HOST_TEST: host unit test build (ps4-port/tests/fenv_test.cpp). The SDK fenv_t layout is
// replicated and every C symbol gets a ps4_test_ prefix so the host CRT's fenv is left alone.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <immintrin.h>
#include <pthread.h>

#ifdef PS4_FENV_HOST_TEST
#define PS4_FENV(name) ps4_test_##name

extern "C"
{
// Byte-exact copy of the SDK's include/bits/fenv.h fenv_t (the SDK's bit-field pair
// __opcode:11/__unused4:5 is a plain 16-bit field here because MS bit-field layout differs).
struct ps4_test_fenv_t
{
    unsigned short __control_word;
    unsigned short __unused1;
    unsigned short __status_word;
    unsigned short __unused2;
    unsigned short __tags;
    unsigned short __unused3;
    unsigned int __eip;
    unsigned short __cs_selector;
    unsigned short __opcode_bits;
    unsigned int __data_offset;
    unsigned short __data_selector;
    unsigned short __unused5;
    unsigned int __mxcsr;
};
typedef unsigned short ps4_test_fexcept_t;
}

namespace
{
using env_type = ps4_test_fenv_t;
using except_type = ps4_test_fexcept_t;
const env_type* const default_env = reinterpret_cast<const env_type*>(-1);
constexpr int fe_invalid = 1, fe_divbyzero = 4, fe_overflow = 8, fe_underflow = 16, fe_inexact = 32;
constexpr int fe_all_except = 63;
constexpr int fe_tonearest = 0, fe_downward = 0x400, fe_upward = 0x800, fe_towardzero = 0xc00;
}
#else
#include <fenv.h>
#define PS4_FENV(name) name

namespace
{
using env_type = fenv_t;
using except_type = fexcept_t;
const env_type* const default_env = FE_DFL_ENV;
constexpr int fe_invalid = FE_INVALID, fe_divbyzero = FE_DIVBYZERO, fe_overflow = FE_OVERFLOW;
constexpr int fe_underflow = FE_UNDERFLOW, fe_inexact = FE_INEXACT, fe_all_except = FE_ALL_EXCEPT;
constexpr int fe_tonearest = FE_TONEAREST, fe_downward = FE_DOWNWARD, fe_upward = FE_UPWARD;
constexpr int fe_towardzero = FE_TOWARDZERO;
}
#endif

namespace
{
// The implementation below assumes the x86-64 musl encoding: exception bits equal the x87/MXCSR
// flag bits and rounding modes equal the x87 control-word RC field (bits 10-11).
static_assert(fe_invalid == 1 && fe_divbyzero == 4 && fe_overflow == 8 && fe_underflow == 16 && fe_inexact == 32);
static_assert(fe_all_except == 63);
static_assert(fe_tonearest == 0 && fe_downward == 0x400 && fe_upward == 0x800 && fe_towardzero == 0xc00);
// fnstenv stores 28 bytes; musl's fenv.s keeps MXCSR right after them.
static_assert(offsetof(env_type, __mxcsr) == 28 && sizeof(env_type) == 32);

constexpr std::uint32_t c_mxcsr_rc = 0x6000; // MXCSR bits 13-14 = FE_* rounding value << 3
constexpr std::uint16_t c_x87_rc = 0xc00;    // x87 control word bits 10-11
constexpr std::uint32_t c_default_mxcsr = 0x1f80;
constexpr std::uint16_t c_default_x87_cw = 0x37f;

inline std::uint32_t read_mxcsr()
{
    std::uint32_t value;
    __asm__ volatile("stmxcsr %0" : "=m"(value) : : "memory");
    return value;
}

inline void write_mxcsr(std::uint32_t value)
{
    __asm__ volatile("ldmxcsr %0" : : "m"(value) : "memory");
}

inline std::uint16_t read_x87_cw()
{
    std::uint16_t value;
    __asm__ volatile("fnstcw %0" : "=m"(value) : : "memory");
    return value;
}

inline void write_x87_cw(std::uint16_t value)
{
    __asm__ volatile("fldcw %0" : : "m"(value) : "memory");
}

inline std::uint16_t read_x87_sw()
{
    std::uint16_t value;
    __asm__ volatile("fnstsw %0" : "=m"(value) : : "memory");
    return value;
}
}

extern "C"
{
int PS4_FENV(feclearexcept)(int mask)
{
    // musl x86_64 policy: exception flags live in MXCSR. Pending x87 flags are moved there (and
    // cleared from the x87 status word) before the requested ones are cleared.
    mask &= fe_all_except;
    const std::uint32_t x87 = read_x87_sw() & fe_all_except;
    if (x87 & static_cast<std::uint32_t>(mask))
    {
        __asm__ volatile("fnclex" : : : "memory");
    }

    const std::uint32_t csr = read_mxcsr() | x87;
    if (csr & static_cast<std::uint32_t>(mask))
    {
        write_mxcsr(csr & ~static_cast<std::uint32_t>(mask));
    }
    return 0;
}

int PS4_FENV(feraiseexcept)(int mask)
{
    write_mxcsr(read_mxcsr() | static_cast<std::uint32_t>(mask & fe_all_except));
    return 0;
}

int PS4_FENV(fetestexcept)(int mask)
{
    return static_cast<int>((read_mxcsr() | read_x87_sw()) & static_cast<std::uint32_t>(mask & fe_all_except));
}

int PS4_FENV(fegetround)(void)
{
    return static_cast<int>((read_mxcsr() & c_mxcsr_rc) >> 3);
}

int PS4_FENV(__fesetround)(int round)
{
    const std::uint16_t rc = static_cast<std::uint16_t>(round) & c_x87_rc;
    write_x87_cw(static_cast<std::uint16_t>((read_x87_cw() & ~c_x87_rc) | rc));
    write_mxcsr((read_mxcsr() & ~c_mxcsr_rc) | (static_cast<std::uint32_t>(rc) << 3));
    return 0;
}

int PS4_FENV(fesetround)(int round)
{
    if (round != fe_tonearest && round != fe_downward && round != fe_upward && round != fe_towardzero)
    {
        return -1;
    }
    return PS4_FENV(__fesetround)(round);
}

int PS4_FENV(fegetenv)(env_type* env)
{
    // fnstenv masks every x87 exception as a side effect; fldenv restores the stored state.
    __asm__ volatile("fnstenv %0\n\tfldenv %0" : "+m"(*env) : : "memory");
    env->__mxcsr = read_mxcsr();
    return 0;
}

int PS4_FENV(fesetenv)(const env_type* env)
{
    if (env == default_env)
    {
        env_type dfl{};
        dfl.__control_word = c_default_x87_cw;
        dfl.__tags = 0xffff;
        __asm__ volatile("fldenv %0" : : "m"(dfl) : "memory");
        write_mxcsr(c_default_mxcsr);
        return 0;
    }
    __asm__ volatile("fldenv %0" : : "m"(*env) : "memory");
    write_mxcsr(env->__mxcsr);
    return 0;
}

// The remaining members (fegetexceptflag.lo, fesetexceptflag.lo, feholdexcept.lo, feupdateenv.lo)
// are portable C on top of the functions above; defining them here keeps the whole API in one place.
int PS4_FENV(fegetexceptflag)(except_type* flags, int mask)
{
    *flags = static_cast<except_type>(PS4_FENV(fetestexcept)(mask));
    return 0;
}

int PS4_FENV(fesetexceptflag)(const except_type* flags, int mask)
{
    PS4_FENV(feclearexcept)(~*flags & mask);
    PS4_FENV(feraiseexcept)(*flags & mask);
    return 0;
}

int PS4_FENV(feholdexcept)(env_type* env)
{
    PS4_FENV(fegetenv)(env);
    PS4_FENV(feclearexcept)(fe_all_except);
    return 0;
}

int PS4_FENV(feupdateenv)(const env_type* env)
{
    const int raised = PS4_FENV(fetestexcept)(fe_all_except);
    PS4_FENV(fesetenv)(env);
    PS4_FENV(feraiseexcept)(raised);
    return 0;
}
}

// Boot self-test: the SPU recompilers are only correct when spu_thread::cpu_task really programs
// MXCSR.RC = round toward zero (SPUASMJITRecompiler.cpp / SPULLVMRecompiler.cpp CLZ via cvtdq2ps).
namespace
{
struct fpu_probe
{
    std::uint32_t initial_mxcsr;
    int initial_round;
    // After the spu_thread::cpu_task sequence (fesetround + _mm_setcsr + gv_set_zeroing_denormals).
    int set_rz;
    int round_rz;
    std::uint32_t mxcsr_rz;
    std::uint16_t x87_rz;
    std::uint32_t cvt_7fffffff_rz;
    std::uint32_t cvt_01ffffff_rz;
    // After fesetround(FE_TONEAREST) alone (no direct MXCSR write).
    int set_rn;
    int round_rn;
    std::uint32_t mxcsr_rn;
    std::uint16_t x87_rn;
    std::uint32_t cvt_7fffffff_rn;
    // fesetround(FE_TOWARDZERO) alone (SPU precise interpreter path), then restored to RN.
    std::uint32_t mxcsr_rz_only;
    int bad_mode_result;
    int inexact_raised;
    int inexact_cleared;
};

[[gnu::noinline]] std::uint32_t cvtdq2ps_bits(std::int32_t value)
{
    // Volatile in and out: the conversion cannot be folded or moved across the MXCSR writes.
    volatile std::int32_t input = value;
    volatile std::uint32_t output = 0;
    const __m128 converted = _mm_cvtepi32_ps(_mm_set1_epi32(input));
    output = static_cast<std::uint32_t>(_mm_cvtsi128_si32(_mm_castps_si128(converted)));
    return output;
}

// CLZ as computed by the SPU recompilers from the cvtdq2ps exponent.
constexpr std::uint32_t clz_from_bits(std::uint32_t bits)
{
    return 158 - (bits >> 23);
}

void run_probe(fpu_probe& r)
{
    r.initial_mxcsr = _mm_getcsr();
    r.initial_round = PS4_FENV(fegetround)();

    // Same sequence as spu_thread::cpu_task (rpcs3/Emu/Cell/SPUThread.cpp).
    r.set_rz = PS4_FENV(fesetround)(fe_towardzero);
    _mm_setcsr((_mm_getcsr() & ~_MM_ROUND_MASK) | _MM_ROUND_TOWARD_ZERO);
    // gv_set_zeroing_denormals() (rpcs3/util/simd.hpp): FTZ + DAZ + masked invalid, RC preserved.
    _mm_setcsr(_mm_getcsr() | _MM_FLUSH_ZERO_ON | _MM_DENORMALS_ZERO_ON | _MM_MASK_INVALID);

    r.round_rz = PS4_FENV(fegetround)();
    r.mxcsr_rz = _mm_getcsr();
    r.x87_rz = read_x87_cw();
    r.cvt_7fffffff_rz = cvtdq2ps_bits(0x7fffffff);
    r.cvt_01ffffff_rz = cvtdq2ps_bits(0x01ffffff);

    // fesetround alone must reach MXCSR (PPU threads, SPU precise interpreter).
    r.set_rn = PS4_FENV(fesetround)(fe_tonearest);
    r.round_rn = PS4_FENV(fegetround)();
    r.mxcsr_rn = _mm_getcsr();
    r.x87_rn = read_x87_cw();
    r.cvt_7fffffff_rn = cvtdq2ps_bits(0x7fffffff);

    PS4_FENV(fesetround)(fe_towardzero);
    r.mxcsr_rz_only = _mm_getcsr();
    PS4_FENV(fesetround)(fe_tonearest);
    r.bad_mode_result = PS4_FENV(fesetround)(0x123);

    // Exception flags (SPU precise interpreter FPSCR emulation uses feclearexcept/fetestexcept).
    PS4_FENV(feclearexcept)(fe_all_except);
    volatile float one = 1.0f, three = 3.0f;
    volatile float third = one / three;
    (void)third;
    r.inexact_raised = PS4_FENV(fetestexcept)(fe_inexact);
    PS4_FENV(feclearexcept)(fe_all_except);
    r.inexact_cleared = PS4_FENV(fetestexcept)(fe_all_except);
}

void* probe_thread(void* arg)
{
    run_probe(*static_cast<fpu_probe*>(arg));
    return nullptr;
}

struct selftest_state
{
    bool ok = false;
    char detail[512]{};
};

const char* first_failure(const fpu_probe& r)
{
    if (r.set_rz != 0) return "fesetround(FE_TOWARDZERO) failed";
    if (r.round_rz != fe_towardzero) return "fegetround() != FE_TOWARDZERO after the SPU sequence";
    if ((r.mxcsr_rz & c_mxcsr_rc) != c_mxcsr_rc) return "MXCSR.RC is not round-toward-zero after the SPU sequence";
    if ((r.x87_rz & c_x87_rc) != c_x87_rc) return "x87 RC is not round-toward-zero after fesetround";
    if (r.cvt_7fffffff_rz != 0x4effffff || clz_from_bits(r.cvt_7fffffff_rz) != 1) return "cvtdq2ps(0x7fffffff) is not 0x4effffff (SPU CLZ off by one)";
    if (r.cvt_01ffffff_rz != 0x4bffffff || clz_from_bits(r.cvt_01ffffff_rz) != 7) return "cvtdq2ps(0x01ffffff) is not 0x4bffffff (SPU CLZ off by one)";
    if (r.set_rn != 0 || r.round_rn != fe_tonearest) return "fesetround(FE_TONEAREST) did not restore round-to-nearest";
    if ((r.mxcsr_rn & c_mxcsr_rc) != 0 || (r.x87_rn & c_x87_rc) != 0) return "fesetround(FE_TONEAREST) did not reach MXCSR/x87";
    if (r.cvt_7fffffff_rn != 0x4f000000) return "cvtdq2ps(0x7fffffff) is not 0x4f000000 under round-to-nearest";
    if ((r.mxcsr_rz_only & c_mxcsr_rc) != c_mxcsr_rc) return "fesetround(FE_TOWARDZERO) alone did not reach MXCSR";
    if (r.bad_mode_result == 0) return "fesetround accepted an invalid mode";
    if (!r.inexact_raised) return "fetestexcept(FE_INEXACT) missed 1.0f/3.0f";
    if (r.inexact_cleared) return "feclearexcept(FE_ALL_EXCEPT) left flags set";
    return nullptr;
}

selftest_state run_selftest() noexcept
{
    selftest_state state;
    fpu_probe r{};

    // A fresh native thread starts with the platform's default FPU state, like every SPU thread.
    pthread_t thread{};
    const int create_error = pthread_create(&thread, nullptr, probe_thread, &r);
    const bool fresh_thread = create_error == 0 && pthread_join(thread, nullptr) == 0;
    if (!fresh_thread)
    {
        // Fallback: probe on the calling thread and restore its complete FPU environment.
        env_type saved{};
        PS4_FENV(fegetenv)(&saved);
        r = fpu_probe{};
        run_probe(r);
        PS4_FENV(fesetenv)(&saved);
    }

    const char* failure = first_failure(r);
    state.ok = failure == nullptr;
    std::snprintf(state.detail, sizeof(state.detail),
        "fenv self-test %s%s%s: thread=%s(%d) initial mxcsr=0x%x round=0x%x; SPU sequence -> round=0x%x mxcsr=0x%x x87cw=0x%x "
        "cvtdq2ps(0x7fffffff)=0x%x clz=%u cvtdq2ps(0x01ffffff)=0x%x clz=%u; RN -> round=0x%x mxcsr=0x%x cvtdq2ps(0x7fffffff)=0x%x; "
        "RZ-only mxcsr=0x%x; inexact=%d",
        state.ok ? "passed" : "FAILED", failure ? ": " : "", failure ? failure : "",
        fresh_thread ? "fresh" : "caller", create_error,
        r.initial_mxcsr, r.initial_round, r.round_rz, r.mxcsr_rz, r.x87_rz,
        r.cvt_7fffffff_rz, clz_from_bits(r.cvt_7fffffff_rz), r.cvt_01ffffff_rz, clz_from_bits(r.cvt_01ffffff_rz),
        r.round_rn, r.mxcsr_rn, r.cvt_7fffffff_rn, r.mxcsr_rz_only, r.inexact_raised != 0);
    return state;
}

const selftest_state& selftest() noexcept
{
    // Thread-safe one-time initialisation; never aborts (a failure only selects the SPU interpreter).
    static const selftest_state state = run_selftest();
    return state;
}
}

bool ps4_spu_rounding_ok() noexcept
{
    return selftest().ok;
}

const char* ps4_fpu_selftest_detail() noexcept
{
    return selftest().detail;
}
