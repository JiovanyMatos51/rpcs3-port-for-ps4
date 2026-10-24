#pragma once

// The folder of the XMB 'Jogo' column when the installed firmware has no item_tex_plain_folder
// (dev_flash/vsh/resource/explore_plugin_full.rco). An original drawing made of simple shapes: a back panel with a
// rounded tab, a white-lilac front panel with a rim and a highlight line, and a soft shadow, on the same 314x178 canvas
// as the firmware texture, so folder rows keep the size and rhythm of the other rows. Straight (not premultiplied) ARGB
// like every utils::ps4::xmb::image. Nothing of the firmware is embedded here.

#include "ps4_xmb_resources.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace utils::ps4::xmb
{
namespace folder_detail
{
// Geometry in the units of the 314x178 reference canvas (x right, y down); other sizes scale each axis
struct shape
{
    float left = 64, right = 250, bottom = 146, radius = 11;
    float back_top = 38;                                // the back panel, under the front one
    float tab_right = 146, tab_top = 20, tab_bottom = 50; // the tab on the back panel
    float front_top = 56;
    float rim = 1.6f;                                   // inset of the front panel's rim
    float highlight_top = 57.6f, highlight_bottom = 60.4f;
    float shadow_dy = 4, shadow_blur = 3, shadow_alpha = .34f;
    float back_alpha = .80f, front_alpha = .86f, rim_alpha = .55f, highlight_alpha = .90f;
};

inline bool in_round_rect(float x, float y, float l, float t, float r, float b, float radius)
{
    if (x < l || x > r || y < t || y > b) return false;
    radius = std::min({radius, (r - l) / 2, (b - t) / 2});
    const float cx = std::clamp(x, l + radius, r - radius), cy = std::clamp(y, t + radius, b - radius);
    return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= radius * radius;
}

inline bool in_back(const shape& s, float x, float y)
{
    return in_round_rect(x, y, s.left, s.back_top, s.right, s.bottom, s.radius) ||
        in_round_rect(x, y, s.left, s.tab_top, s.tab_right, s.tab_bottom, s.radius);
}

inline bool in_front(const shape& s, float x, float y)
{
    return in_round_rect(x, y, s.left, s.front_top, s.right, s.bottom, s.radius);
}

inline bool in_front_inner(const shape& s, float x, float y)
{
    return in_round_rect(x, y, s.left + s.rim, s.front_top + s.rim, s.right - s.rim, s.bottom - s.rim, s.radius - s.rim);
}

inline bool in_highlight(const shape& s, float x, float y)
{
    return in_round_rect(x, y, s.left + s.rim, s.highlight_top, s.right - s.rim, s.highlight_bottom, 1.2f);
}

// Straight-alpha 'over' of colour (r, g, b) at strength a (0..1) on a texel whose colour channels are 0..255
struct texel
{
    float r = 0, g = 0, b = 0, a = 0;
    void over(float cr, float cg, float cb, float ca)
    {
        if (!(ca > 0)) return;
        const float keep = a * (1 - ca), out = ca + keep;
        r = (cr * ca + r * keep) / out;
        g = (cg * ca + g * keep) / out;
        b = (cb * ca + b * keep) / out;
        a = out;
    }
};

// Vertical blend of two 0xRRGGBB colours, t in 0..1
inline void lerp_rgb(std::uint32_t from, std::uint32_t to, float t, float& r, float& g, float& b)
{
    const auto channel = [t](std::uint32_t a, std::uint32_t c, int shift)
    { return float((a >> shift) & 255) * (1 - t) + float((c >> shift) & 255) * t; };
    r = channel(from, to, 16);
    g = channel(from, to, 8);
    b = channel(from, to, 0);
}
}

// width x height (16..1024 each, else an empty image). Coverage by 4x4 supersampling: about 0.9 M samples of four
// shapes at 314x178, done once per process on the loader thread and only when the firmware image is missing.
inline image folder_icon(int width = 314, int height = 178)
{
    using namespace folder_detail;
    if (width < 16 || height < 16 || width > 1024 || height > 1024) return {};
    const shape s{};
    const float sx = float(width) / 314, sy = float(height) / 178; // pixels per reference unit
    const std::size_t w = std::size_t(width), n = w * std::size_t(height);
    std::vector<float> back(n), front(n), rim(n), highlight(n), silhouette(n);
    for (int py = 0; py < height; ++py)
        for (int px = 0; px < width; ++px)
        {
            float b = 0, f = 0, inner = 0, h = 0, any = 0;
            for (int ky = 0; ky < 4; ++ky)
                for (int kx = 0; kx < 4; ++kx)
                {
                    const float x = (float(px) + (float(kx) + .5f) / 4) / sx, y = (float(py) + (float(ky) + .5f) / 4) / sy;
                    const bool in_b = in_back(s, x, y), in_f = in_front(s, x, y);
                    b += in_b;
                    f += in_f;
                    any += in_b || in_f;
                    inner += in_f && in_front_inner(s, x, y);
                    h += in_highlight(s, x, y);
                }
            const std::size_t i = std::size_t(py) * w + std::size_t(px);
            back[i] = b / 16;
            front[i] = f / 16;
            rim[i] = std::clamp((f - inner) / 16, 0.f, 1.f);
            highlight[i] = h / 16;
            silhouette[i] = any / 16;
        }

    // Shadow: the silhouette moved down and blurred (two box passes per axis), only outside the silhouette
    std::vector<float> shadow(n, 0.f), pass(n);
    const int dy = std::max(1, int(std::lround(s.shadow_dy * sy)));
    for (int py = dy; py < height; ++py)
        std::copy_n(silhouette.begin() + std::ptrdiff_t(std::size_t(py - dy) * w), w, shadow.begin() + std::ptrdiff_t(std::size_t(py) * w));
    const int rx = std::max(1, int(std::lround(s.shadow_blur * sx))), ry = std::max(1, int(std::lround(s.shadow_blur * sy)));
    const auto at = [w](int x, int y) { return std::size_t(y) * w + std::size_t(x); };
    for (int blur = 0; blur < 2; ++blur)
    {
        for (int py = 0; py < height; ++py)
            for (int px = 0; px < width; ++px)
            {
                float sum = 0;
                for (int k = -rx; k <= rx; ++k) sum += shadow[at(std::clamp(px + k, 0, width - 1), py)];
                pass[at(px, py)] = sum / float(2 * rx + 1);
            }
        for (int py = 0; py < height; ++py)
            for (int px = 0; px < width; ++px)
            {
                float sum = 0;
                for (int k = -ry; k <= ry; ++k) sum += pass[at(px, std::clamp(py + k, 0, height - 1))];
                shadow[at(px, py)] = sum / float(2 * ry + 1);
            }
    }

    image out{width, height, std::vector<std::uint32_t>(n)};
    const auto byte = [](float v) { return std::uint32_t(std::clamp(std::lround(v), 0l, 255l)); };
    for (int py = 0; py < height; ++py)
    {
        const float y = (float(py) + .5f) / sy;
        float back_r, back_g, back_b, front_r, front_g, front_b;
        lerp_rgb(0xc4c3d6, 0xa9a8bf, std::clamp((y - s.tab_top) / (s.bottom - s.tab_top), 0.f, 1.f), back_r, back_g, back_b);
        lerp_rgb(0xf1f0f8, 0xcdccde, std::clamp((y - s.front_top) / (s.bottom - s.front_top), 0.f, 1.f), front_r, front_g, front_b);
        for (int px = 0; px < width; ++px)
        {
            const std::size_t i = at(px, py);
            texel t;
            t.over(0x10, 0x10, 0x26, shadow[i] * (1 - silhouette[i]) * s.shadow_alpha);
            t.over(back_r, back_g, back_b, back[i] * s.back_alpha);
            t.over(front_r, front_g, front_b, front[i] * s.front_alpha);
            t.over(0x9a, 0x99, 0xb4, rim[i] * s.rim_alpha);
            t.over(0xff, 0xff, 0xff, highlight[i] * s.highlight_alpha);
            const std::uint32_t alpha = byte(t.a * 255);
            // Fully transparent texels keep a light colour: the bilinear blit shows no dark fringe
            if (!alpha) out.pixels[i] = 0x00d8d8e4u;
            else out.pixels[i] = (alpha << 24) | (byte(t.r) << 16) | (byte(t.g) << 8) | byte(t.b);
        }
    }
    return out;
}
}
