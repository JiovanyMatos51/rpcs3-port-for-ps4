#include "util/ps4_audio_conceal.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace conceal = utils::ps4::audio_conceal;

namespace
{
int g_failures = 0;

void check(bool condition, const char* what, int line)
{
    if (!condition)
    {
        std::printf("FAIL line %d: %s\n", line, what);
        ++g_failures;
    }
}

#define CHECK(x) check((x), #x, __LINE__)

constexpr std::uint32_t frames = 256;
constexpr std::uint32_t float_bytes = frames * 2 * sizeof(float);
constexpr std::uint32_t s16_bytes = frames * 2 * sizeof(std::int16_t);

// Stereo test signal: two sines of different frequency and phase per channel, continuous across blocks.
float signal(std::uint64_t frame, int channel, float amplitude)
{
    const double t = static_cast<double>(frame);
    return static_cast<float>(amplitude * (channel == 0 ? std::sin(t * 0.071 + 0.3) : 0.6 * std::sin(t * 0.193 + 1.1) + 0.3 * std::sin(t * 0.017)));
}

struct float_block
{
    alignas(64) float v[frames * 2]{};
    std::uint8_t* bytes() { return reinterpret_cast<std::uint8_t*>(v); }
    float at(std::uint32_t f, int c) const { return v[2 * f + c]; }
};

struct s16_block
{
    alignas(64) std::int16_t v[frames * 2]{};
    std::uint8_t* bytes() { return reinterpret_cast<std::uint8_t*>(v); }
    float at(std::uint32_t f, int c) const { return v[2 * f + c]; }
};

void make_float(float_block& b, std::uint64_t first_frame, std::uint32_t real)
{
    // Garbage beyond 'real': the filler must overwrite it.
    for (std::uint32_t f = 0; f < frames; ++f)
        for (int c = 0; c < 2; ++c)
            b.v[2 * f + c] = f < real ? signal(first_frame + f, c, 0.8f) : 12345.0f;
}

void make_s16(s16_block& b, std::uint64_t first_frame, std::uint32_t real)
{
    for (std::uint32_t f = 0; f < frames; ++f)
        for (int c = 0; c < 2; ++c)
            b.v[2 * f + c] = f < real ? static_cast<std::int16_t>(std::lround(signal(first_frame + f, c, 20000.0f))) : std::int16_t{0x5a5a};
}

// Largest |x[f] - x[f-1]| over the last 8 real frames before 'junction' (in the played history).
template <typename Get>
float max_slope(Get get, std::int64_t junction, int c)
{
    float slope = 0.0f;
    for (std::int64_t f = junction - 8; f < junction; ++f)
        slope = std::max(slope, std::fabs(get(f, c) - get(f - 1, c)));
    return slope;
}

// Checks the concealed tail of a block: continuity at the junction, raised-cosine fade to zero within 128 frames,
// zeros after. 'played' holds every frame played so far (history + this block).
template <typename Get>
void check_tail(Get get, std::int64_t junction, std::uint32_t real, std::uint32_t synthesized, std::uint32_t expect_len, float tolerance)
{
    CHECK(synthesized == expect_len);
    for (int c = 0; c < 2; ++c)
    {
        const float last_real = get(junction - 1, c);
        const float first_syn = get(junction, c);
        CHECK(std::fabs(first_syn - last_real) <= max_slope(get, junction, c) + tolerance);
        // Mirror: frame junction+k repeats junction-1-k under a gain <= 1.
        for (std::uint32_t k = 0; k < expect_len; ++k)
        {
            const float src = get(junction - 1 - k, c);
            CHECK(std::fabs(get(junction + k, c)) <= std::fabs(src) + tolerance);
        }
        // Zero by frame 128 after the junction and zeros up to the end of the block.
        for (std::uint32_t k = expect_len; real + k < frames; ++k)
            CHECK(get(junction + k, c) == 0.0f);
        CHECK(expect_len <= conceal::fade_out_frames);
    }
}
}

int main()
{
    // 1. Float stream: three complete blocks, then an empty block (the common AudioOut underrun).
    {
        conceal::conceal_state state;
        std::vector<float> played[2];
        std::uint64_t frame = 0;
        float_block b;

        for (int i = 0; i < 3; ++i)
        {
            make_float(b, frame, frames);
            float_block ref = b;
            CHECK(conceal::fill(state, b.bytes(), float_bytes, float_bytes, false) == 0);
            CHECK(std::memcmp(ref.v, b.v, float_bytes) == 0); // complete blocks pass bit-exact
            for (std::uint32_t f = 0; f < frames; ++f)
                for (int c = 0; c < 2; ++c) played[c].push_back(b.at(f, c));
            frame += frames;
        }

        make_float(b, frame, 0);
        const std::uint32_t syn = conceal::fill(state, b.bytes(), 0, float_bytes, false);
        const std::int64_t junction = static_cast<std::int64_t>(played[0].size());
        for (std::uint32_t f = 0; f < frames; ++f)
            for (int c = 0; c < 2; ++c) played[c].push_back(b.at(f, c));
        auto get = [&](std::int64_t f, int c) { return played[c][static_cast<std::size_t>(f)]; };
        check_tail(get, junction, 0, syn, 128, 1e-6f);
        CHECK(get(junction, 0) == get(junction - 1, 0)); // exact continuity at the junction
        CHECK(state.after_gap && !state.last_valid);

        // 2. One block max: a second empty block in a row stays silent (no buzz loop over the history).
        make_float(b, frame, 0);
        CHECK(conceal::fill(state, b.bytes(), 0, float_bytes, false) == 0);
        for (std::uint32_t f = 0; f < frames; ++f)
            CHECK(b.at(f, 0) == 0.0f && b.at(f, 1) == 0.0f);

        // 3. Fade-in from 0 on the first real block after the gap, untouched from frame 32 on.
        frame += 2 * frames;
        make_float(b, frame, frames);
        float_block ref = b;
        CHECK(conceal::fill(state, b.bytes(), float_bytes, float_bytes, false) == 0);
        CHECK(b.at(0, 0) == 0.0f && b.at(0, 1) == 0.0f);
        for (std::uint32_t f = 0; f < conceal::fade_in_frames; ++f)
            for (int c = 0; c < 2; ++c)
                CHECK(std::fabs(b.at(f, c)) <= std::fabs(ref.at(f, c)) + 1e-7f);
        for (std::uint32_t f = 1; f < conceal::fade_in_frames; ++f)
            CHECK(conceal::detail::fade_in_gain(f) > conceal::detail::fade_in_gain(f - 1));
        CHECK(std::memcmp(ref.v + 2 * conceal::fade_in_frames, b.v + 2 * conceal::fade_in_frames,
            (frames - conceal::fade_in_frames) * 2 * sizeof(float)) == 0);
        CHECK(!state.after_gap && state.last_valid);

        // The next complete block is bit-exact again.
        frame += frames;
        make_float(b, frame, frames);
        ref = b;
        CHECK(conceal::fill(state, b.bytes(), float_bytes, float_bytes, false) == 0);
        CHECK(std::memcmp(ref.v, b.v, float_bytes) == 0);
    }

    // 4. Short block (100 real frames) after real audio: the mirror walks through this block's frames first.
    // 5. Short block with fewer missing frames than the fade (200 real): the fade spans only the missing frames.
    for (const std::uint32_t real : {100u, 200u, 255u, 1u})
    {
        conceal::conceal_state state;
        std::vector<float> played[2];
        float_block b;
        make_float(b, 0, frames);
        conceal::fill(state, b.bytes(), float_bytes, float_bytes, false);
        for (std::uint32_t f = 0; f < frames; ++f)
            for (int c = 0; c < 2; ++c) played[c].push_back(b.at(f, c));

        make_float(b, frames, real);
        float_block ref = b;
        // A byte count that is not a whole frame counts as the whole frames before it.
        const std::uint32_t syn = conceal::fill(state, b.bytes(), real * 8 + 3, float_bytes, false);
        CHECK(std::memcmp(ref.v, b.v, real * 8) == 0); // the real frames are not changed
        const std::int64_t junction = frames + real;
        for (std::uint32_t f = 0; f < frames; ++f)
            for (int c = 0; c < 2; ++c) played[c].push_back(b.at(f, c));
        auto get = [&](std::int64_t f, int c) { return played[c][static_cast<std::size_t>(f)]; };
        check_tail(get, junction, real, syn, std::min(128u, frames - real), 1e-6f);
    }

    // 6. S16 path: same rules on integer samples (rounded, clamped).
    {
        conceal::conceal_state state;
        std::vector<float> played[2];
        s16_block b;
        for (int i = 0; i < 2; ++i)
        {
            make_s16(b, static_cast<std::uint64_t>(i) * frames, frames);
            s16_block ref = b;
            CHECK(conceal::fill(state, b.bytes(), s16_bytes, s16_bytes, true) == 0);
            CHECK(std::memcmp(ref.v, b.v, s16_bytes) == 0);
            for (std::uint32_t f = 0; f < frames; ++f)
                for (int c = 0; c < 2; ++c) played[c].push_back(b.at(f, c));
        }

        make_s16(b, 2 * frames, 40);
        const std::uint32_t syn = conceal::fill(state, b.bytes(), 40 * 4, s16_bytes, true);
        const std::int64_t junction = 2 * frames + 40;
        for (std::uint32_t f = 0; f < frames; ++f)
            for (int c = 0; c < 2; ++c) played[c].push_back(b.at(f, c));
        auto get = [&](std::int64_t f, int c) { return played[c][static_cast<std::size_t>(f)]; };
        check_tail(get, junction, 40, syn, 128, 0.5f);

        // Fade-in after the gap starts at 0 in s16 too.
        make_s16(b, 3 * frames, frames);
        CHECK(conceal::fill(state, b.bytes(), s16_bytes, s16_bytes, true) == 0);
        CHECK(b.v[0] == 0 && b.v[1] == 0);

        // Full-scale samples survive the round trip without wrapping.
        conceal::conceal_state edge;
        for (std::uint32_t i = 0; i < frames * 2; ++i) b.v[i] = (i & 2) ? std::int16_t{32767} : std::int16_t{-32768};
        conceal::fill(edge, b.bytes(), s16_bytes, s16_bytes, true);
        CHECK(conceal::fill(edge, b.bytes(), 0, s16_bytes, true) == 128);
        CHECK(b.v[0] == 32767 && b.v[1] == 32767);   // frame 255 repeated at gain 1
        CHECK(b.v[2] < -32000 && b.v[3] < -32000);   // frame 254, still negative full scale
    }

    // 7. Empty history: a fresh state turns an empty block into zeros.
    {
        conceal::conceal_state state;
        float_block b;
        make_float(b, 0, 0);
        CHECK(conceal::fill(state, b.bytes(), 0, float_bytes, false) == 0);
        for (std::uint32_t i = 0; i < frames * 2; ++i)
            CHECK(b.v[i] == 0.0f);

        s16_block s;
        make_s16(s, 0, 0);
        conceal::conceal_state state16;
        CHECK(conceal::fill(state16, s.bytes(), 0, s16_bytes, true) == 0);
        for (std::uint32_t i = 0; i < frames * 2; ++i)
            CHECK(s.v[i] == 0);
    }

    // 8. No previous block but some real frames: the mirror uses only those frames (shorter fade).
    {
        conceal::conceal_state state;
        float_block b;
        make_float(b, 0, 20);
        const std::uint32_t syn = conceal::fill(state, b.bytes(), 20 * 8, float_bytes, false);
        CHECK(syn == 20);
        for (int c = 0; c < 2; ++c)
        {
            CHECK(b.at(20, c) == b.at(19, c));
            for (std::uint32_t f = 40; f < frames; ++f)
                CHECK(b.at(f, c) == 0.0f);
        }
    }

    // 9. A grain larger than the state can hold is zero-filled and drops the history.
    {
        conceal::conceal_state state;
        std::vector<float> big(frames * 4, 1.0f);
        auto* bytes = reinterpret_cast<std::uint8_t*>(big.data());
        CHECK(conceal::fill(state, bytes, 100 * 8, static_cast<std::uint32_t>(big.size() * sizeof(float)), false) == 0);
        for (std::size_t i = 200; i < big.size(); ++i)
            CHECK(big[i] == 0.0f);
        CHECK(!state.last_valid);
    }

    // Raised-cosine fade: exactly 1 at the junction, monotonic, ends near 0.
    CHECK(conceal::detail::fade_out_gain(0, 128) == 1.0f);
    for (std::uint32_t k = 1; k < 128; ++k)
        CHECK(conceal::detail::fade_out_gain(k, 128) < conceal::detail::fade_out_gain(k - 1, 128));
    CHECK(conceal::detail::fade_out_gain(127, 128) < 0.001f);
    CHECK(conceal::detail::fade_in_gain(0) == 0.0f);

    if (g_failures)
    {
        std::printf("audio_conceal_test: %d failure(s)\n", g_failures);
        return 1;
    }

    std::printf("audio_conceal_test: PASS\n");
    return 0;
}
