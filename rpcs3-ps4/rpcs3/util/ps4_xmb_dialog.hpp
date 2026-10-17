#pragma once

// PS3 system message dialogs drawn over the XMB waves: the look of the firmware's msgdialog. Its textures come from the
// console's installed firmware at runtime (system_plugin.rco: tex_msgdialog_bg, tex_msgdialog_bar,
// tex_default_progress_base/_slider, tex_busy, the pad glyphs); add_dialog_fallbacks() draws stand-ins of the same
// sizes when the firmware lacks them. Nothing of the firmware is embedded here.
// 0.39: texts in the interface language; a right-to-left interface keeps the rows centred and mirrors the button guide
// and the progress captions.

#include "ps4_xmb.hpp"
#include <cmath>
#include <functional>
#include <initializer_list>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace utils::ps4::xmb
{
// One entry of the button guide under the band: a pad glyph and its label
struct dialog_button
{
    std::string glyph; // tex_cross, tex_circle, tex_triangle or tex_rectangle
    std::string label;
    bool operator==(const dialog_button&) const = default;
};

struct dialog
{
    std::string title;              // first line, 23 px (may be empty)
    std::vector<std::string> lines; // message, 21 px, wrapped to the band
    std::vector<std::string> notes; // details under the message, 18 px, grey
    int progress = -1;              // per mille 0..1000 draws the progress bar; < 0: none
    std::string progress_left;      // under the bar, left end ("45%")
    std::string progress_right;     // under the bar, right end ("Tempo restante: cerca de 3 min")
    bool busy = false;              // the busy clock where a bar would be
    const image* icon = nullptr;    // artwork above the message (not owned)
    std::vector<dialog_button> buttons;
    bool operator==(const dialog&) const = default;
};

// Firmware images the dialogs use (all in system_plugin.rco)
inline constexpr std::array<const char*, 9> dialog_images{"tex_msgdialog_bg", "tex_msgdialog_bar", "tex_default_progress_base",
    "tex_default_progress_slider", "tex_busy", "tex_cross", "tex_circle", "tex_triangle", "tex_rectangle"};

inline dialog_button cross_button(std::string label) { return {"tex_cross", std::move(label)}; }
inline dialog_button circle_button(std::string label) { return {"tex_circle", std::move(label)}; }
inline dialog_button square_button(std::string label) { return {"tex_rectangle", std::move(label)}; }
inline dialog_button triangle_button(std::string label) { return {"tex_triangle", std::move(label)}; }

// "45%"
inline std::string percent_text(int per_mille)
{
    return std::to_string(std::clamp(per_mille, 0, 1000) / 10) + "%";
}

// "Tempo restante: cerca de 12 min" (interface language); "" while the rate is unknown
inline std::string remaining_text(std::uint64_t left_bytes, std::uint64_t bytes_per_second)
{
    using i18n::str;
    if (!bytes_per_second) return {};
    const std::uint64_t seconds = left_bytes / bytes_per_second;
    if (seconds < 60) return i18n::tr(str::DLG_TEMPO_RESTANTE_MENOS_1);
    const std::uint64_t minutes = (seconds + 30) / 60;
    if (minutes < 120) return i18n::trf(str::DLG_TEMPO_RESTANTE_MIN, {static_cast<unsigned long long>(minutes)});
    return i18n::trf(str::DLG_TEMPO_RESTANTE_H, {static_cast<unsigned long long>((minutes + 30) / 60)});
}

namespace dialog_detail
{
// Coverage of a shape by 4x4 supersampling; 'inside' takes texel coordinates (x right, y down, texel centres at +.5)
inline image shape(int w, int h, std::uint32_t rgb, const std::function<float(float, float)>& inside, const std::function<float(float, float)>& alpha)
{
    image out{w, h, std::vector<std::uint32_t>(std::size_t(w) * h, rgb & 0xffffffu)};
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            float cover = 0;
            for (int sy = 0; sy < 4; ++sy)
                for (int sx = 0; sx < 4; ++sx) cover += inside(x + (sx + .5f) / 4, y + (sy + .5f) / 4);
            const float a = std::clamp(cover / 16 * alpha(x + .5f, y + .5f), 0.f, 1.f);
            out.pixels[std::size_t(y) * w + x] = (std::uint32_t(a * 255 + .5f) << 24) | (rgb & 0xffffffu);
        }
    return out;
}

inline float segment_distance(float px, float py, float ax, float ay, float bx, float by)
{
    const float dx = bx - ax, dy = by - ay, len = dx * dx + dy * dy;
    const float t = len > 0 ? std::clamp(((px - ax) * dx + (py - ay) * dy) / len, 0.f, 1.f) : 0.f;
    const float ex = px - (ax + t * dx), ey = py - (ay + t * dy);
    return std::sqrt(ex * ex + ey * ey);
}

// Signed distance to a horizontal capsule from (x0, cy) to (x1, cy) of radius r (negative inside)
inline float capsule(float px, float py, float x0, float x1, float cy, float r)
{
    return segment_distance(px, py, x0, cy, x1, cy) - r;
}

inline float one(float, float) { return 1.f; }

// Grey of a vertical gradient through 'stops' (value 0..1 at evenly spaced rows of [top, bottom])
inline std::uint32_t grey_at(float y, float top, float bottom, std::initializer_list<float> stops)
{
    const std::vector<float> s(stops);
    const float t = std::clamp((y - top) / std::max(bottom - top, 1.f), 0.f, 1.f) * float(s.size() - 1);
    const std::size_t i = std::min(std::size_t(t), s.size() - 2);
    const float v = s[i] + (s[i + 1] - s[i]) * (t - float(i));
    const unsigned int g = unsigned(std::clamp(v, 0.f, 1.f) * 255 + .5f);
    return (g << 16) | (g << 8) | g;
}

// The part [x, x + w) of 'source' (all rows)
inline image columns(const image& source, int x, int w)
{
    image out{w, source.height, std::vector<std::uint32_t>(std::size_t(w) * source.height)};
    for (int y = 0; y < source.height; ++y)
        for (int k = 0; k < w; ++k) out.pixels[std::size_t(y) * w + k] = source.pixels[std::size_t(y) * source.width + std::clamp(x + k, 0, source.width - 1)];
    return out;
}

// Draws a bar texture stretched to 'w' with its 'cap' columns at each end unscaled (the round ends keep their shape)
inline void three_slice(canvas& c, const image& source, float x, float y, float w, int cap, unsigned int opacity)
{
    if (!source || source.width < 2 * cap + 1 || w < 2 * cap) return;
    c.blit(columns(source, 0, cap), x, y, float(cap), float(source.height), opacity);
    c.blit(columns(source, cap, source.width - 2 * cap), x + cap, y, w - 2 * cap, float(source.height), opacity);
    c.blit(columns(source, source.width - cap, cap), x + w - cap, y, float(cap), float(source.height), opacity);
}

// Anti-aliased round-capped line (the hands of the busy clock)
inline void stroke(canvas& c, float ax, float ay, float bx, float by, float width, std::uint32_t colour)
{
    const float r = width / 2;
    const int x0 = int(std::floor(std::min(ax, bx) - r - 1)), x1 = int(std::ceil(std::max(ax, bx) + r + 1));
    const int y0 = int(std::floor(std::min(ay, by) - r - 1)), y1 = int(std::ceil(std::max(ay, by) + r + 1));
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x)
        {
            const float cover = std::clamp(r + .5f - segment_distance(x + .5f, y + .5f, ax, ay, bx, by), 0.f, 1.f);
            if (cover > 0) c.blend(x, y, colour, unsigned(cover * 255 + .5f));
        }
}

inline std::uint32_t with_alpha(std::uint32_t colour, unsigned int opacity)
{
    return (canvas::div255((colour >> 24) * std::min(opacity, 255u)) << 24) | (colour & 0xffffffu);
}

// The translucent 'upper' over the translucent 'lower' as one colour, so the band costs one blend per pixel
inline std::uint32_t over(std::uint32_t lower, std::uint32_t upper)
{
    const float a1 = float(lower >> 24) / 255, a2 = float(upper >> 24) / 255, a = 1 - (1 - a1) * (1 - a2);
    if (!(a > 0)) return 0;
    std::uint32_t out = std::uint32_t(a * 255 + .5f) << 24;
    for (const unsigned int shift : {0u, 8u, 16u})
    {
        const float value = (float((lower >> shift) & 255) * a1 * (1 - a2) + float((upper >> shift) & 255) * a2) / a;
        out |= std::uint32_t(std::clamp(value, 0.f, 255.f) + .5f) << shift;
    }
    return out;
}

// Splits 'text' at spaces into lines no wider than 'width' pixels (a single longer word stays whole)
inline std::vector<std::string> wrap(const font& typeface, std::string_view text, float size, float width)
{
    std::vector<std::string> out;
    std::string current;
    while (!text.empty())
    {
        const std::size_t space = text.find(' ');
        const std::string_view word = text.substr(0, space);
        text = space == std::string_view::npos ? std::string_view{} : text.substr(space + 1);
        std::string candidate = current.empty() ? std::string(word) : current + ' ' + std::string(word);
        if (!current.empty() && typeface.measure(candidate, size) > width) out.push_back(std::exchange(current, std::string(word)));
        else current = std::move(candidate);
    }
    if (!current.empty()) out.push_back(std::move(current));
    return out;
}
} // namespace dialog_detail

// Stand-ins for the firmware's dialog images, same sizes and placement, emplaced only where the firmware image is
// missing. tex_square (the name the XMB guide asks for) is the firmware's tex_rectangle.
inline void add_dialog_fallbacks(std::map<std::string, image, std::less<>>& resources)
{
    using namespace dialog_detail;
    const auto add = [&](const char* name, const auto& make)
    {
        if (const auto found = resources.find(name); found == resources.end() || !found->second) resources.insert_or_assign(name, make());
    };
    // 30x30 white outline glyphs, strokes about 3 px
    add("tex_cross", [] {
        return shape(30, 30, 0xffffff, [](float x, float y) {
            return float(std::min(segment_distance(x, y, 8, 8, 22, 22), segment_distance(x, y, 22, 8, 8, 22)) <= 1.6f);
        }, one);
    });
    add("tex_circle", [] {
        return shape(30, 30, 0xffffff, [](float x, float y) {
            const float d = std::sqrt((x - 15) * (x - 15) + (y - 15) * (y - 15));
            return float(d <= 9.4f && d >= 6.4f);
        }, one);
    });
    add("tex_triangle", [] {
        return shape(30, 30, 0xffffff, [](float x, float y) {
            const float d = std::min({segment_distance(x, y, 15, 6.5f, 6.2f, 21.8f), segment_distance(x, y, 15, 6.5f, 23.8f, 21.8f),
                segment_distance(x, y, 6.2f, 21.8f, 23.8f, 21.8f)});
            return float(d <= 1.5f);
        }, one);
    });
    add("tex_rectangle", [] {
        return shape(30, 30, 0xffffff, [](float x, float y) {
            const bool outer = x >= 6 && x <= 24 && y >= 6 && y <= 24, inner = x >= 8.6f && x <= 21.4f && y >= 8.6f && y <= 21.4f;
            return float(outer && !inner);
        }, one);
    });
    if (const auto square = resources.find("tex_rectangle"); square != resources.end() && !resources.contains("tex_square"))
        resources.emplace("tex_square", square->second);
    // Separator: 960x1 white, bright in the middle and fading to nothing at both ends
    add("tex_msgdialog_bar", [] {
        image bar{960, 1, std::vector<std::uint32_t>(960)};
        for (int x = 0; x < 960; ++x)
        {
            const float d = std::abs(x + .5f - 480.f);
            const float a = 185.f * std::exp(-(d / 105.f) * (d / 105.f)) + 70.f * std::pow(std::max(0.f, 1.f - d / 480.f), 1.5f);
            bar.pixels[x] = (std::uint32_t(std::clamp(a, 0.f, 255.f) + .5f) << 24) | 0xffffffu;
        }
        return bar;
    });
    // Shade over the whole screen: 12x12 dark grey at 45 %
    add("tex_msgdialog_bg", [] { return image{12, 12, std::vector<std::uint32_t>(144, 0x74363836u)}; });
    // Progress track: 24x20, a translucent grey capsule (rows 3-15) with a lighter rim and a darker groove
    add("tex_default_progress_base", [] {
        image out{24, 20, std::vector<std::uint32_t>(480)};
        for (int y = 0; y < 20; ++y)
            for (int x = 0; x < 24; ++x)
            {
                float cover = 0, rim = 0;
                for (int sy = 0; sy < 4; ++sy)
                    for (int sx = 0; sx < 4; ++sx)
                    {
                        const float d = capsule(x + (sx + .5f) / 4, y + (sy + .5f) / 4, 8.5f, 15.5f, 9.5f, 6.4f);
                        cover += d <= 0;
                        rim += d <= 0 && d > -1.6f;
                    }
                const float a = (cover - rim) / 16 * .41f + rim / 16 * .53f;
                out.pixels[std::size_t(y) * 24 + x] = (std::uint32_t(a * 255 + .5f) << 24) | grey_at(y + .5f, 3, 16, {.80f, .84f, .62f, .66f, .78f, .93f, .86f});
            }
        return out;
    });
    // Progress fill: 24x20, an opaque silver capsule (rows 6-12) inside the track
    add("tex_default_progress_slider", [] {
        image out{24, 20, std::vector<std::uint32_t>(480)};
        for (int y = 0; y < 20; ++y)
            for (int x = 0; x < 24; ++x)
            {
                float cover = 0;
                for (int sy = 0; sy < 4; ++sy)
                    for (int sx = 0; sx < 4; ++sx) cover += capsule(x + (sx + .5f) / 4, y + (sy + .5f) / 4, 8.5f, 15.5f, 9.5f, 3.4f) <= 0;
                out.pixels[std::size_t(y) * 24 + x] = (std::uint32_t(cover / 16 * 255 + .5f) << 24) | grey_at(y + .5f, 6, 13, {1.f, .78f, .58f, .54f, .64f, .80f});
            }
        return out;
    });
    // Busy clock face: 36x36 ring and centre dot (the hands are drawn on top)
    add("tex_busy", [] {
        return shape(36, 36, 0xffffff, [](float x, float y) {
            const float d = std::sqrt((x - 18) * (x - 18) + (y - 18) * (y - 18));
            return float((d <= 16.f && d >= 13.2f) || d <= 1.9f);
        }, one);
    });
}

namespace dialog_style
{
inline constexpr std::uint32_t text = 0xffffffff, note = 0xffc6c6ce, band = 0x2a000000;
inline constexpr float title_size = 23, line_size = 21, note_size = 18, button_size = 20, caption_size = 18;
inline constexpr int title_step = 34, line_step = 31, note_step = 27;
inline constexpr float wrap_width = 1040;
inline constexpr int bar_width = 640, bar_height = 20, busy_size = 36, glyph_size = 26;
} // namespace dialog_style

namespace dialog_detail
{
// One wrapped row of a dialog's text
struct text_row
{
    std::string text;
    float size;
    std::uint32_t colour;
    int step; // height of the row, including 'gap'
    int gap;  // space above the text (the first note keeps a little apart from the message)
};

// 0.39: the rows of the last dialog drawn, kept while its texts, the font and the interface language stay the same
// (wrapping measures every word prefix, and an Arabic prefix is shaped each time; a dialog is drawn at 30 Hz)
struct wrapped_rows
{
    std::mutex lock;
    std::uint64_t generation = 0;
    bool rtl = false;
    std::string title;
    std::vector<std::string> lines, notes;
    std::vector<text_row> rows;
};

inline wrapped_rows& last_wrapped()
{
    static wrapped_rows cache;
    return cache;
}

inline std::vector<text_row> wrap_rows(const font& typeface, const dialog& d)
{
    namespace style = dialog_style;
    std::vector<text_row> rows;
    for (std::string& t : wrap(typeface, d.title, style::title_size, style::wrap_width))
        rows.push_back({std::move(t), style::title_size, style::text, style::title_step, 0});
    for (const std::string& line : d.lines)
        for (std::string& t : wrap(typeface, line, style::line_size, style::wrap_width))
            rows.push_back({std::move(t), style::line_size, style::text, style::line_step, 0});
    const std::size_t first_note = rows.size();
    for (const std::string& line : d.notes)
        for (std::string& t : wrap(typeface, line, style::note_size, style::wrap_width))
        {
            const int gap = rows.size() == first_note && first_note ? 6 : 0;
            rows.push_back({std::move(t), style::note_size, style::note, style::note_step + gap, gap});
        }
    return rows;
}

inline std::vector<text_row> cached_rows(const font& typeface, const dialog& d, bool rtl)
{
    wrapped_rows& cache = last_wrapped();
    std::lock_guard guard(cache.lock);
    if (!typeface.generation() || cache.generation != typeface.generation() || cache.rtl != rtl || cache.title != d.title ||
        cache.lines != d.lines || cache.notes != d.notes)
    {
        cache.rows = wrap_rows(typeface, d);
        cache.generation = typeface.generation();
        cache.rtl = rtl;
        cache.title = d.title;
        cache.lines = d.lines;
        cache.notes = d.notes;
    }
    return cache.rows;
}
} // namespace dialog_detail

// Draws 'd' over the frame in 'c' (the XMB waves): shade, band, message, bar or busy clock, button guide. 'opacity'
// fades the whole dialog in (0..255); 'seconds' turns the busy clock. 0.39: 'rtl' mirrors the button guide (first button
// rightmost, its glyph right of its label) and the progress captions; it follows the interface language by default.
inline void draw_dialog(canvas& c, const font& typeface, const std::map<std::string, image, std::less<>>& resources, const dialog& d,
    double seconds, unsigned int opacity = 255, bool rtl = i18n::rtl())
{
    using namespace dialog_detail;
    namespace style = dialog_style;
    opacity = std::min(opacity, 255u);
    const auto find = [&](std::string_view name) -> const image*
    {
        const auto found = resources.find(name);
        return found != resources.end() && found->second ? &found->second : nullptr;
    };
    const int width = c.width;
    const float middle = width / 2.f;
    // Shade: the firmware's tex_msgdialog_bg is one flat colour; its centre texel is that colour
    std::uint32_t shade = 0x74363836u;
    if (const image* bg = find("tex_msgdialog_bg")) shade = bg->pixels[bg->pixels.size() / 2];
    shade = with_alpha(shade, opacity);

    using row = text_row;
    const std::vector<row> rows = cached_rows(typeface, d, rtl);
    const image* icon = d.icon && *d.icon ? d.icon : nullptr;
    float icon_w = 0, icon_h = 0;
    if (icon)
    {
        icon_w = std::min(176.f, float(icon->width));
        icon_h = icon_w * icon->height / icon->width;
        if (icon_h > 112) { icon_w *= 112 / icon_h; icon_h = 112; }
    }
    const bool bar = d.progress >= 0;
    int height = 0;
    if (icon) height += int(icon_h) + 16;
    for (const row& r : rows) height += r.step;
    if (bar) height += 14 + style::bar_height + 30;
    else if (d.busy) height += 12 + style::busy_size + 6;
    const int top = std::clamp(340 - height / 2, 96, std::max(96, 560 - height));
    const int top_line = top - 28, bottom_line = top + height + 22;

    // The shade over the whole screen, a little darker between the two separators (the band)
    c.rectangle(0, 0, width, top_line, shade);
    c.rectangle(0, top_line, width, bottom_line - top_line, over(shade, with_alpha(style::band, opacity)));
    c.rectangle(0, bottom_line, width, c.height - bottom_line, shade);
    if (const image* separator = find("tex_msgdialog_bar"))
        for (const int y : {top_line, bottom_line}) c.blit(*separator, 0, float(y), float(width), 1, opacity);

    float y = float(top);
    if (icon)
    {
        c.blit(*icon, middle - icon_w / 2, y, icon_w, icon_h, opacity);
        y += icon_h + 16;
    }
    for (const row& r : rows)
    {
        const float baseline = y + float(r.gap) + r.size * .98f;
        typeface.paint(c, std::round(middle - typeface.measure(r.text, r.size) / 2), std::round(baseline), r.text, r.size, with_alpha(r.colour, opacity));
        y += r.step;
    }
    if (bar)
    {
        y += 14;
        const float x0 = middle - style::bar_width / 2.f;
        if (const image* base = find("tex_default_progress_base")) three_slice(c, *base, x0, y, float(style::bar_width), 8, opacity);
        if (const image* slider = find("tex_default_progress_slider"); slider && d.progress > 0)
        {
            // 0.39: right to left the bar fills from its right end
            const float fill = 16 + (style::bar_width - 16) * std::min(d.progress, 1000) / 1000.f;
            three_slice(c, *slider, rtl ? x0 + style::bar_width - fill : x0, y, fill, 8, opacity);
        }
        const float caption = std::round(y + style::bar_height + 21);
        // progress_left (the percentage) starts the bar's reading direction: right to left it is at the right end
        const std::string& at_left = rtl ? d.progress_right : d.progress_left;
        const std::string& at_right = rtl ? d.progress_left : d.progress_right;
        if (!at_left.empty()) typeface.paint(c, std::round(x0 + 4), caption, at_left, style::caption_size, with_alpha(style::text, opacity));
        if (!at_right.empty())
            typeface.paint(c, std::round(x0 + style::bar_width - 4 - typeface.measure(at_right, style::caption_size)), caption, at_right,
                style::caption_size, with_alpha(style::text, opacity));
    }
    else if (d.busy)
    {
        y += 12;
        const float s = style::busy_size, cx = middle, cy = y + s / 2;
        if (const image* face = find("tex_busy")) c.blit(*face, cx - s / 2, y, s, s, opacity);
        // Hands as on the firmware's busy clock: the long one turns once a second, the short one every twelve
        const float turn = float(std::fmod(seconds, 12.0));
        const float long_angle = turn * 6.2831853f, short_angle = turn / 12 * 6.2831853f;
        const std::uint32_t hand = with_alpha(0xffffffffu, opacity);
        stroke(c, cx, cy, cx + std::sin(long_angle) * s * .37f, cy - std::cos(long_angle) * s * .37f, s * .105f, hand);
        stroke(c, cx, cy, cx + std::sin(short_angle) * s * .24f, cy - std::cos(short_angle) * s * .24f, s * .105f, hand);
    }

    // Button guide, centred under the band
    if (!d.buttons.empty())
    {
        constexpr float gap = 8, spacing = 46;
        float total = 0;
        for (const dialog_button& b : d.buttons) total += style::glyph_size + gap + typeface.measure(b.label, style::button_size);
        total += spacing * float(d.buttons.size() - 1);
        const float glyph_top = float(bottom_line + 16), baseline = float(bottom_line + 36);
        if (!rtl)
        {
            float x = std::round(middle - total / 2);
            for (const dialog_button& b : d.buttons)
            {
                if (const image* glyph = find(b.glyph)) c.blit(*glyph, x, glyph_top, style::glyph_size, style::glyph_size, opacity);
                x += style::glyph_size + gap;
                typeface.paint(c, x, baseline, b.label, style::button_size, with_alpha(style::text, opacity));
                x += typeface.measure(b.label, style::button_size) + spacing;
            }
        }
        else
        {
            // Mirrored: from the right end, each glyph right of its label
            float x = std::round(middle + total / 2);
            for (const dialog_button& b : d.buttons)
            {
                x -= style::glyph_size;
                if (const image* glyph = find(b.glyph)) c.blit(*glyph, x, glyph_top, style::glyph_size, style::glyph_size, opacity);
                x -= gap + typeface.measure(b.label, style::button_size);
                typeface.paint(c, std::round(x), baseline, b.label, style::button_size, with_alpha(style::text, opacity));
                x -= spacing;
            }
        }
    }
}
} // namespace utils::ps4::xmb
