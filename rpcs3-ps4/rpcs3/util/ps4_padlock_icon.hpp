#pragma once

// The padlock drawn over a locked game of the XMB when the installed firmware has no tex_lock_icon
// (dev_flash/vsh/resource/explore_plugin_full.rco). An original drawing made of simple shapes: white translucent
// glass with a grey bevel and a soft shadow, on the same 100x120 canvas as the firmware texture, so both are drawn the
// same way. Straight (not premultiplied) ARGB like every utils::ps4::xmb::image.

#include "ps4_xmb_resources.hpp"

#include <cmath>
#include <utility>

namespace utils::ps4::xmb
{
namespace padlock_detail
{
// Geometry in units of the image width, y downwards
struct shape
{
    float body_left = .04f, body_right = .96f, body_top = .38f, body_bottom = .90f, body_radius = .06f;
    float shackle_cx = .50f, shackle_cy = .31f, shackle_outer = .30f, shackle_inner = .13f;
    float key_cy = .57f, key_r = .095f;
    float slot_top = .60f, slot_bottom = .80f, slot_half_top = .035f, slot_half_bottom = .09f;
    float rim = .03f; // the bevel: full weight within this distance of an edge, 45 % up to twice it
    float shadow_dy = .03f, shadow_blur = .035f, shadow_alpha = .38f;
};

inline bool in_round_rect(float x, float y, float l, float t, float r, float b, float radius)
{
    if (x < l || x > r || y < t || y > b) return false;
    const float cx = std::clamp(x, l + radius, r - radius), cy = std::clamp(y, t + radius, b - radius);
    return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= radius * radius;
}

// Circle plus a slot widening downwards; it shows what is behind the padlock
inline bool in_keyhole(const shape& s, float x, float y)
{
    const float dx = x - s.shackle_cx;
    if (dx * dx + (y - s.key_cy) * (y - s.key_cy) <= s.key_r * s.key_r) return true;
    if (y < s.slot_top || y > s.slot_bottom) return false;
    const float f = (y - s.slot_top) / (s.slot_bottom - s.slot_top);
    return std::abs(dx) <= s.slot_half_top + (s.slot_half_bottom - s.slot_half_top) * f;
}

// Upper half ring with straight legs down into the body
inline bool in_shackle(const shape& s, float x, float y)
{
    const float dx = x - s.shackle_cx;
    if (y <= s.shackle_cy)
    {
        const float d2 = dx * dx + (y - s.shackle_cy) * (y - s.shackle_cy);
        return d2 <= s.shackle_outer * s.shackle_outer && d2 >= s.shackle_inner * s.shackle_inner;
    }
    return y <= s.body_top + .02f && std::abs(dx) <= s.shackle_outer && std::abs(dx) >= s.shackle_inner;
}

inline bool in_body(const shape& s, float x, float y)
{
    return in_round_rect(x, y, s.body_left, s.body_top, s.body_right, s.body_bottom, s.body_radius) && !in_keyhole(s, x, y);
}

inline bool in_lock(const shape& s, float x, float y)
{
    return in_body(s, x, y) || in_shackle(s, x, y);
}
}

// width x height (8..512 each, else an empty image). Coverage by 4x4 supersampling: about 0.2 M shape tests at
// 100x120, done once on the loader thread and only when the firmware image is missing.
inline image padlock_icon(int width = 100, int height = 120)
{
    using namespace padlock_detail;
    if (width < 8 || height < 8 || width > 512 || height > 512) return {};
    const shape s{};
    const float unit = static_cast<float>(width);
    const std::size_t n = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    std::vector<float> fill(n), edge(n), cover(n);
    const std::array<std::pair<float, float>, 8> ring{{{1.f, 0.f}, {-1.f, 0.f}, {0.f, 1.f}, {0.f, -1.f}, {.707f, .707f}, {-.707f, .707f}, {.707f, -.707f}, {-.707f, -.707f}}};
    for (int py = 0; py < height; ++py)
        for (int px = 0; px < width; ++px)
        {
            float inside = 0, rim = 0, body = 0;
            for (int sy = 0; sy < 4; ++sy)
                for (int sx = 0; sx < 4; ++sx)
                {
                    const float x = (static_cast<float>(px) + (static_cast<float>(sx) + .5f) / 4) / unit;
                    const float y = (static_cast<float>(py) + (static_cast<float>(sy) + .5f) / 4) / unit;
                    if (!in_lock(s, x, y)) continue;
                    inside += 1;
                    if (in_body(s, x, y)) body += 1;
                    float e = 0;
                    for (const auto& [ox, oy] : ring)
                        if (!in_lock(s, x + ox * s.rim, y + oy * s.rim))
                        {
                            e = 1;
                            break;
                        }
                    if (e == 0)
                        for (const auto& [ox, oy] : ring)
                            if (!in_lock(s, x + ox * 2 * s.rim, y + oy * 2 * s.rim))
                            {
                                e = .45f;
                                break;
                            }
                    rim += e;
                }
            const std::size_t i = static_cast<std::size_t>(py) * static_cast<std::size_t>(width) + static_cast<std::size_t>(px);
            cover[i] = inside / 16;
            edge[i] = rim / 16;
            // Glass: more opaque at the top of the body, softer towards the bottom; the shackle in between
            const float t = std::clamp(((static_cast<float>(py) + .5f) / unit - s.body_top) / (s.body_bottom - s.body_top), 0.f, 1.f);
            const float body_alpha = .72f - .26f * t + (t < .16f ? .08f * (1 - t / .16f) : 0.f);
            fill[i] = (body * body_alpha + (inside - body) * .70f) / 16;
        }

    // Shadow: the silhouette moved down and blurred (two box passes per axis)
    std::vector<float> shadow(n, 0.f), pass(n);
    const int dy = std::max(1, static_cast<int>(std::lround(s.shadow_dy * unit)));
    for (int py = dy; py < height; ++py)
        for (int px = 0; px < width; ++px)
            shadow[static_cast<std::size_t>(py) * static_cast<std::size_t>(width) + static_cast<std::size_t>(px)] =
                cover[static_cast<std::size_t>(py - dy) * static_cast<std::size_t>(width) + static_cast<std::size_t>(px)];
    const int r = std::max(1, static_cast<int>(std::lround(s.shadow_blur * unit)));
    const auto at = [width](int x, int y) { return static_cast<std::size_t>(y) * static_cast<std::size_t>(width) + static_cast<std::size_t>(x); };
    for (int blur = 0; blur < 2; ++blur)
    {
        for (int py = 0; py < height; ++py)
            for (int px = 0; px < width; ++px)
            {
                float sum = 0;
                for (int k = -r; k <= r; ++k) sum += shadow[at(std::clamp(px + k, 0, width - 1), py)];
                pass[at(px, py)] = sum / static_cast<float>(2 * r + 1);
            }
        for (int py = 0; py < height; ++py)
            for (int px = 0; px < width; ++px)
            {
                float sum = 0;
                for (int k = -r; k <= r; ++k) sum += pass[at(px, std::clamp(py + k, 0, height - 1))];
                shadow[at(px, py)] = sum / static_cast<float>(2 * r + 1);
            }
    }

    image out{width, height, std::vector<std::uint32_t>(n)};
    const auto byte = [](float v) { return static_cast<std::uint32_t>(std::clamp(std::lround(v), 0l, 255l)); };
    for (std::size_t i = 0; i < n; ++i)
    {
        // White fill; the bevel grey-lilac (0xb8b8ca) and more opaque
        const float bevel = edge[i] > 0 ? std::min(1.f, edge[i] / std::max(cover[i], 1e-6f)) : 0.f;
        const float lock_a = std::min(1.f, fill[i] * (1 - bevel) + cover[i] * .80f * bevel);
        const float lr = 255 * (1 - bevel) + 0xb8 * bevel, lg = lr, lb = 255 * (1 - bevel) + 0xca * bevel;
        // The shadow (0x0c0c20) only where the padlock is not, under it ('over' in straight alpha)
        const float shade = shadow[i] * s.shadow_alpha * (1 - cover[i]);
        const float a = lock_a + shade * (1 - lock_a);
        // Fully transparent texels keep a light colour: the bilinear blit shows no dark fringe
        const std::uint32_t alpha = byte(a * 255);
        float cr = 0xd8, cg = 0xd8, cb = 0xe4;
        if (alpha)
        {
            cr = (lr * lock_a + 0x0c * shade * (1 - lock_a)) / a;
            cg = (lg * lock_a + 0x0c * shade * (1 - lock_a)) / a;
            cb = (lb * lock_a + 0x20 * shade * (1 - lock_a)) / a;
        }
        out.pixels[i] = (alpha << 24) | (byte(cr) << 16) | (byte(cg) << 8) | byte(cb);
    }
    return out;
}
}
