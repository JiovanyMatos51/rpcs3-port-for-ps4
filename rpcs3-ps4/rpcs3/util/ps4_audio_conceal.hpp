#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

// Underrun concealment for the PS4 AudioOut output thread. A block the mixer did not deliver in time used to be
// played as hard digital silence: a 5.33 ms gap with a step at both ends (the click/buzz of a starved stream).
// Here the missing frames continue the audio before the gap, mirrored in time around the junction (so the first
// synthesized frame equals the last real one), under a raised-cosine fade to zero; the first real frames after
// such a gap fade in. No content is recovered and at most half a block of history is ever replayed, once.
namespace utils::ps4::audio_conceal
{
// Interleaved stereo blocks of up to one AudioOut grain.
inline constexpr std::uint32_t max_frames = 256;
inline constexpr std::uint32_t fade_out_frames = 128;
inline constexpr std::uint32_t fade_in_frames = 32;

struct conceal_state
{
    // Last complete block as played. S16 blocks keep their integer sample values here.
    float last[max_frames * 2]{};
    std::uint32_t last_frames = 0;
    // 'last' ends exactly where the next block starts (false after any gap: history is never reused).
    bool last_valid = false;
    // The output went quiet (concealed or silent tail): the next real frames fade in from zero.
    bool after_gap = false;
};

namespace detail
{
    inline constexpr float pi = 3.14159265358979f;

    // 1 at k = 0, falling to 0 at k = len.
    inline float fade_out_gain(std::uint32_t k, std::uint32_t len)
    {
        return 0.5f + 0.5f * std::cos(pi * static_cast<float>(k) / static_cast<float>(len));
    }

    // 0 at i = 0, rising to 1 at i = fade_in_frames.
    inline float fade_in_gain(std::uint32_t i)
    {
        return 0.5f - 0.5f * std::cos(pi * static_cast<float>(i) / static_cast<float>(fade_in_frames));
    }

    inline void load(float* dst, const std::uint8_t* src, std::uint32_t frames, bool s16)
    {
        if (!s16)
        {
            std::memcpy(dst, src, frames * 2 * sizeof(float));
            return;
        }

        for (std::uint32_t i = 0; i < frames * 2; ++i)
        {
            std::int16_t value;
            std::memcpy(&value, src + i * sizeof(value), sizeof(value));
            dst[i] = static_cast<float>(value);
        }
    }

    inline void store(std::uint8_t* dst, const float* src, std::uint32_t frames, bool s16)
    {
        if (!s16)
        {
            std::memcpy(dst, src, frames * 2 * sizeof(float));
            return;
        }

        for (std::uint32_t i = 0; i < frames * 2; ++i)
        {
            const float rounded = std::nearbyint(std::clamp(src[i], -32768.0f, 32767.0f));
            const std::int16_t value = static_cast<std::int16_t>(rounded);
            std::memcpy(dst + i * sizeof(value), &value, sizeof(value));
        }
    }
}

// Completes one output block of 'length' bytes (stereo float, or stereo s16 when 's16') whose first
// 'complete_bytes' bytes hold real audio. The rest is synthesized or zeroed; real frames are only changed by the
// fade-in after a gap. Returns the number of synthesized (non-silent) frames: 0 for a complete block or when no
// adjacent history exists.
inline std::uint32_t fill(conceal_state& state, std::uint8_t* block, std::uint32_t complete_bytes, std::uint32_t length, bool s16)
{
    const std::uint32_t frame_bytes = s16 ? 2 * sizeof(std::int16_t) : 2 * sizeof(float);
    const std::uint32_t frames = length / frame_bytes;
    const std::uint32_t real = std::min(complete_bytes, length) / frame_bytes;

    if (frames == 0 || frames > max_frames)
    {
        // Not a grain this state can hold: plain silence, history dropped.
        std::memset(block + real * frame_bytes, 0, length - real * frame_bytes);
        state.last_valid = false;
        state.after_gap = false;
        return 0;
    }

    if (real == frames && !state.after_gap)
    {
        // Common case: a complete block that follows real audio. Only the history changes.
        detail::load(state.last, block, frames, s16);
        state.last_frames = frames;
        state.last_valid = true;
        return 0;
    }

    float x[max_frames * 2];
    detail::load(x, block, real, s16);

    const bool fade_in = state.after_gap;

    if (fade_in)
    {
        for (std::uint32_t i = 0, n = std::min(real, fade_in_frames); i < n; ++i)
        {
            const float g = detail::fade_in_gain(i);
            x[2 * i] *= g;
            x[2 * i + 1] *= g;
        }
    }

    std::uint32_t synthesized = 0;

    if (real < frames)
    {
        // Mirror around the junction: frame real+k repeats frame real-1-k, walking back through this block's real
        // frames first and then (only when it is adjacent) the previous block.
        const std::uint32_t missing = frames - real;
        const std::uint32_t history = real + (state.last_valid ? state.last_frames : 0);
        const std::uint32_t len = std::min({fade_out_frames, missing, history});

        for (std::uint32_t k = 0; k < len; ++k)
        {
            const float* src = k < real ? &x[2 * (real - 1 - k)] : &state.last[2 * (state.last_frames - 1 - (k - real))];
            const float g = detail::fade_out_gain(k, len);
            x[2 * (real + k)] = src[0] * g;
            x[2 * (real + k) + 1] = src[1] * g;
        }

        std::fill(x + 2 * (real + len), x + 2 * frames, 0.0f);
        synthesized = len;
        state.last_valid = false;
        state.after_gap = true;
    }
    else
    {
        std::memcpy(state.last, x, frames * 2 * sizeof(float));
        state.last_frames = frames;
        state.last_valid = true;
        state.after_gap = false;
    }

    // Real frames are untouched unless they were faded in.
    const std::uint32_t first = fade_in ? 0 : real;
    detail::store(block + first * frame_bytes, x + 2 * first, frames - first, s16);
    return synthesized;
}
}
