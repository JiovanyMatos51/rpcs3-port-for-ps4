#pragma once

// 0.39: the interface languages (util/ps4_i18n.hpp): the font packs the Arabic forms from a second font next to the
// Latin glyphs and draws every line in its visual order (util/ps4_arabic.hpp); the scene mirrors the message line and
// the footer for a right-to-left interface and fits long lines with an ellipsis instead of cutting them.

#include "ps4_xmb_resources.hpp"
#include "ps4_arabic.hpp"
#include "ps4_i18n.hpp"
#include <stb_truetype.h>
#include <atomic>
#include <cmath>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace utils::ps4::xmb
{
class canvas
{
    std::uint32_t* m_pixels;
public:
    int width, height;
    canvas(std::uint32_t* pixels, int w, int h) : m_pixels(pixels), width(std::max(w, 0)), height(std::max(h, 0)) {}
    std::uint32_t* data() const { return m_pixels; }

    // 0..255 alpha to the 0..256 weight used by mix(): exact round(a * 256 / 255).
    static unsigned int to256(unsigned int a) { return a + (a >> 7); }
    // Exact round(v / 255) and floor(v / 255) for v <= 255 * 255.
    static unsigned int div255(unsigned int v) { v += 128; return (v + (v >> 8)) >> 8; }
    static unsigned int floor255(unsigned int v) { return (v + 1 + (v >> 8)) >> 8; }
    // Blend the RGB of 'rgb' over 'dst' with weight a / 256 (a <= 256), rounded. Red and blue share one
    // multiply: each 16-bit lane holds at most 255 * 256 + 128, so nothing carries into the other lane.
    static std::uint32_t mix(std::uint32_t dst, std::uint32_t rgb, unsigned int a)
    {
        const unsigned int keep = 256 - a;
        const std::uint32_t rb = ((rgb & 0xff00ffu) * a + (dst & 0xff00ffu) * keep + 0x800080u) >> 8;
        const std::uint32_t g = ((rgb & 0x00ff00u) * a + (dst & 0x00ff00u) * keep + 0x008000u) >> 8;
        return 0xff000000u | (rb & 0xff00ffu) | (g & 0x00ff00u);
    }
    // Bilinear filter of four ARGB texels (a b / c d) with weights fx, fy in 1/256 steps, truncated once
    // like the old float path. The horizontal pass keeps two 16-bit lanes per word; the vertical pass
    // widens them to 32-bit lanes of a 64-bit word, so no intermediate rounding is needed.
    static std::uint32_t bilinear(std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d, unsigned int fx, unsigned int fy)
    {
        const unsigned int kx = 256 - fx, ky = 256 - fy;
        const auto widen = [](std::uint32_t lanes) { return std::uint64_t(lanes & 0xffffu) | (std::uint64_t(lanes >> 16) << 32); };
        const std::uint64_t rb = widen((a & 0xff00ffu) * kx + (b & 0xff00ffu) * fx) * ky + widen((c & 0xff00ffu) * kx + (d & 0xff00ffu) * fx) * fy;
        const std::uint64_t ag = widen(((a >> 8) & 0xff00ffu) * kx + ((b >> 8) & 0xff00ffu) * fx) * ky + widen(((c >> 8) & 0xff00ffu) * kx + ((d >> 8) & 0xff00ffu) * fx) * fy;
        return std::uint32_t((rb >> 16) & 0xffu) | (std::uint32_t((rb >> 48) & 0xffu) << 16) |
            (std::uint32_t((ag >> 16) & 0xffu) << 8) | (std::uint32_t((ag >> 48) & 0xffu) << 24);
    }

    void blend(int x, int y, std::uint32_t color, unsigned int opacity = 255)
    {
        if (unsigned(x) >= unsigned(width) || unsigned(y) >= unsigned(height)) return;
        const unsigned int alpha = div255((color >> 24) * std::min(opacity, 255u));
        if (!alpha) return;
        auto& destination = m_pixels[std::size_t(y) * unsigned(width) + unsigned(x)];
        destination = mix(destination, color, to256(alpha));
    }
    void rectangle(int x, int y, int w, int h, std::uint32_t color)
    {
        const unsigned int alpha = color >> 24;
        if (!alpha || w <= 0 || h <= 0) return;
        const int x0 = std::max(x, 0), y0 = std::max(y, 0);
        const int x1 = int(std::min<long long>(static_cast<long long>(x) + w, width));
        const int y1 = int(std::min<long long>(static_cast<long long>(y) + h, height));
        const unsigned int weight = to256(alpha);
        for (int py = y0; py < y1; ++py)
        {
            std::uint32_t* const row = m_pixels + std::size_t(py) * unsigned(width);
            for (int px = x0; px < x1; ++px) row[px] = mix(row[px], color, weight);
        }
    }
    // Bilinear scaled copy. The destination is clipped once and source coordinates step in 16.16 fixed
    // point; the mapping is the same as the old per-pixel float version (texel centres, edge clamp).
    void blit(const image& source, float x, float y, float w, float h, unsigned int opacity = 255)
    {
        if (!source || !(w > 0) || !(h > 0) || !(w < 1e6f) || !(h < 1e6f) || !(std::abs(x) < 1e6f) || !(std::abs(y) < 1e6f)) return;
        const unsigned int strength = std::min(opacity, 255u);
        if (!strength) return;
        const int x0 = std::max(0, int(std::floor(x))), y0 = std::max(0, int(std::floor(y)));
        const int x1 = std::min(width, int(std::ceil(x + w))), y1 = std::min(height, int(std::ceil(y + h)));
        if (x0 >= x1 || y0 >= y1) return;
        const int sw = source.width, sh = source.height;
        const double step_x = double(sw) / w, step_y = double(sh) / h;
        // Keeps the 16.16 coordinates far from int64 overflow: only destinations thinner than 1/2^24
        // of the source are skipped.
        if (!(step_x < 16777216.0) || !(step_y < 16777216.0)) return;
        const std::int64_t du = std::llround(65536.0 * step_x);
        const std::int64_t u_first = std::llround(65536.0 * ((x0 + .5 - x) * step_x - .5));
        const std::int64_t u_max = std::int64_t(sw - 1) << 16, v_max = std::int64_t(sh - 1) << 16;
        const std::uint32_t* const texels = source.pixels.data();
        // Coordinates are clamped, then offset by half a weight step (128) so the 8-bit weights are
        // rounded rather than truncated; at the clamp limit this still selects the last texel, weight 0.
        for (int py = y0; py < y1; ++py)
        {
            const std::int64_t v = std::clamp<std::int64_t>(std::llround(65536.0 * ((py + .5 - y) * step_y - .5)), 0, v_max) + 128;
            const int sy = int(v >> 16), ny = std::min(sy + 1, sh - 1);
            const unsigned int fy = unsigned(v >> 8) & 255;
            const std::uint32_t* const top = texels + std::size_t(sy) * unsigned(sw);
            const std::uint32_t* const bottom = texels + std::size_t(ny) * unsigned(sw);
            std::uint32_t* const row = m_pixels + std::size_t(py) * unsigned(width);
            std::int64_t u = u_first;
            for (int px = x0; px < x1; ++px, u += du)
            {
                const std::int64_t uc = std::clamp<std::int64_t>(u, 0, u_max) + 128;
                const int sx = int(uc >> 16), nx = std::min(sx + 1, sw - 1);
                const std::uint32_t a = top[sx], b = top[nx], c = bottom[sx], d = bottom[nx];
                if (((a | b | c | d) >> 24) == 0) continue; // fully transparent quad
                const unsigned int fx = unsigned(uc >> 8) & 255;
                const std::uint32_t texel = bilinear(a, b, c, d, fx, fy);
                // Floor, like the float path: the filtered alpha is already truncated, and rounding
                // here as well could add up to 4/255 against it on high-contrast edges.
                if (const unsigned int alpha = floor255((texel >> 24) * strength)) row[px] = mix(row[px], texel, to256(alpha));
            }
        }
    }
};

class font
{
    static constexpr int atlas_width = 1024;
    static constexpr std::size_t latin_count = 230; // U+0020..U+00FF plus the six symbols below
    static constexpr std::array<int, 6> extra{0x2122, 0x2013, 0x2014, 0x2026, 0x25b3, 0x25cb};
    // 0.39: Arabic comma, semicolon, question mark and tatweel, then every presentation form U+FE80..U+FEFC (all letter
    // forms and the lam-alef ligatures the shaper produces), packed from a second font: the PS3 firmware fonts have no
    // Arabic, the bundled DejaVu Sans has all 129.
    static constexpr std::array<int, 4> arabic_punctuation{0x060c, 0x061b, 0x061f, 0x0640};
    static constexpr int arabic_first = 0xfe80, arabic_last = 0xfefc;
    static constexpr std::size_t arabic_count = arabic_punctuation.size() + std::size_t(arabic_last - arabic_first + 1);
    static constexpr std::size_t glyph_count = latin_count + arabic_count; // 359
    // DejaVu's Arabic is small next to the PS3 Latin glyphs of the same pixel size
    static constexpr float arabic_scale = 1.10f;
    // Every size the XMB and the launcher draw gets its own glyph table, so text is not resampled.
    static constexpr std::array<float, 7> sizes{17.f, 18.f, 20.f, 21.f, 23.f, 27.f, 28.f};
    static constexpr std::size_t base = sizes.size() - 1; // 28 px table, scaled for any other size
    std::vector<unsigned char> m_atlas;
    std::vector<stbtt_packedchar> m_glyphs; // sizes.size() tables of glyph_count entries
    bool m_exact = false;
    bool m_arabic = false; // the Arabic forms are in the atlas
    std::uint64_t m_generation = 0;
    // 0.39: visual forms of lines that hold Arabic or bidi marks, and fitted lines, so a line is transformed once and
    // not every frame. A copy of a font starts empty; load() clears it.
    struct line_cache
    {
        std::mutex lock;
        std::unordered_map<std::string, std::string> lines;
        line_cache() = default;
        line_cache(const line_cache&) {}
        line_cache& operator=(const line_cache&)
        {
            std::lock_guard guard(lock);
            lines.clear();
            return *this;
        }
    };
    static constexpr std::size_t cache_limit = 256;
    mutable line_cache m_cache;

    static bool needs_visual(std::string_view text)
    {
        return arabic::has_arabic(text) || arabic::has_bidi_mark(text);
    }
    static std::uint64_t next_generation()
    {
        static std::atomic<std::uint64_t> counter{0};
        return counter.fetch_add(1, std::memory_order_relaxed) + 1;
    }
    // Looks 'key' up, or stores make() under it; the cache is emptied when it is full.
    template <typename Make>
    std::string cached(std::string&& key, Make&& make) const
    {
        std::lock_guard guard(m_cache.lock);
        if (const auto found = m_cache.lines.find(key); found != m_cache.lines.end()) return found->second;
        if (m_cache.lines.size() >= cache_limit) m_cache.lines.clear();
        return m_cache.lines.emplace(std::move(key), make()).first->second;
    }
    static unsigned int decode(std::string_view text, std::size_t& i)
    {
        const unsigned char c = text[i++];
        if (c < 128) return c;
        const unsigned int count = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc2 ? 1 : 0;
        if (!count || i + count > text.size()) return '?';
        unsigned int value = c & ((1u << (6 - count)) - 1);
        for (unsigned int k = 0; k < count; ++k)
        {
            const unsigned char continuation = text[i];
            if ((continuation & 0xc0) != 0x80) return '?';
            ++i; value = (value << 6) | (continuation & 63);
        }
        return value;
    }
    static int glyph(unsigned int c)
    {
        if (c >= 32 && c <= 255) return int(c - 32);
        if (c >= unsigned(arabic_first) && c <= unsigned(arabic_last))
            return int(latin_count + arabic_punctuation.size() + (c - unsigned(arabic_first)));
        if (const auto p = std::find(arabic_punctuation.begin(), arabic_punctuation.end(), int(c)); p != arabic_punctuation.end())
            return int(latin_count + std::size_t(p - arabic_punctuation.begin()));
        const auto found = std::find(extra.begin(), extra.end(), int(c));
        return found == extra.end() ? '?' - 32 : int(224 + found - extra.begin());
    }
    static int codepoint(std::size_t index)
    {
        if (index < 224) return int(index + 32);
        if (index < latin_count) return extra[index - 224];
        if (index < latin_count + arabic_punctuation.size()) return arabic_punctuation[index - latin_count];
        return arabic_first + int(index - latin_count - arabic_punctuation.size());
    }
    // Packs sizes[first..last] with one range per size: the Latin glyphs from 'bytes', then (0.39) the Arabic forms
    // from 'arabic_bytes' into the same atlas (stb_rect_pack keeps its state between the two calls). Within one call
    // every range lists the same code points: stb_truetype copies missing glyphs using an index from the range where it
    // first saw one, which reads past a shorter range (the old Latin-1 + symbols split read 2.5 KB past the glyph table).
    // Without an Arabic font its forms draw as '?'.
    static bool pack(std::span<const std::uint8_t> bytes, const stbtt_fontinfo& info, std::span<const std::uint8_t> arabic_bytes,
        const stbtt_fontinfo* arabic_info, std::vector<unsigned char>& atlas, int atlas_height, std::vector<stbtt_packedchar>& glyphs,
        std::size_t first, std::size_t last)
    {
        atlas.assign(std::size_t(atlas_width) * atlas_height, 0);
        std::fill(glyphs.begin(), glyphs.end(), stbtt_packedchar{});
        std::array<int, glyph_count> codepoints{};
        for (std::size_t i = 0; i < glyph_count; ++i) codepoints[i] = codepoint(i);
        stbtt_pack_context ctx{};
        if (!stbtt_PackBegin(&ctx, atlas.data(), atlas_width, atlas_height, 0, 1, nullptr)) return false;
        std::array<stbtt_pack_range, sizes.size()> ranges{};
        int count = 0;
        for (std::size_t k = first; k <= last; ++k)
            ranges[count++] = {sizes[k], 0, codepoints.data(), int(latin_count), glyphs.data() + k * glyph_count, 0, 0};
        bool ok = stbtt_PackFontRanges(&ctx, bytes.data(), 0, ranges.data(), count) != 0;
        if (ok && arabic_info)
        {
            count = 0;
            for (std::size_t k = first; k <= last; ++k)
                ranges[count++] = {sizes[k] * arabic_scale, 0, codepoints.data() + latin_count, int(arabic_count), glyphs.data() + k * glyph_count + latin_count, 0, 0};
            ok = stbtt_PackFontRanges(&ctx, arabic_bytes.data(), 0, ranges.data(), count) != 0;
        }
        stbtt_PackEnd(&ctx);
        if (!ok) return false;
        // Glyphs the font lacks draw as '?', like code points outside the table. Anything outside the
        // atlas is dropped, so paint() never reads past it.
        for (std::size_t k = first; k <= last; ++k)
        {
            stbtt_packedchar* const table = glyphs.data() + k * glyph_count;
            for (std::size_t i = 0; i < glyph_count; ++i)
            {
                auto& g = table[i];
                const bool arabic_form = i >= latin_count;
                if (arabic_form && !arabic_info) { g = table['?' - 32]; continue; }
                if (g.x1 > atlas_width || g.y1 > atlas_height || g.x0 > g.x1 || g.y0 > g.y1) g = stbtt_packedchar{};
                if (!stbtt_FindGlyphIndex(arabic_form ? arabic_info : &info, codepoints[i]) && codepoints[i] != '?') g = table['?' - 32];
            }
        }
        return true;
    }
    // Glyphs rasterized at the requested size: coverage is copied 1:1 at whole-pixel positions.
    void paint_exact(canvas& target, float x, float y, std::string_view value, const stbtt_packedchar* table, std::uint32_t color) const
    {
        const unsigned int color_alpha = color >> 24;
        if (!color_alpha) return;
        std::uint32_t* const pixels = target.data();
        const int width = target.width, height = target.height;
        for (std::size_t i = 0; i < value.size();)
        {
            const auto& g = table[glyph(decode(value, i))];
            if (g.x1 <= g.x0 || g.y1 <= g.y0) { x += g.xadvance; continue; }
            const int left = int(std::lround(x + g.xoff)), top = int(std::lround(y + g.yoff));
            const int c0 = std::max(0, -left), c1 = std::min(int(g.x1 - g.x0), width - left);
            const int r0 = std::max(0, -top), r1 = std::min(int(g.y1 - g.y0), height - top);
            for (int row = r0; row < r1; ++row)
            {
                const unsigned char* const coverage = m_atlas.data() + std::size_t(g.y0 + row) * atlas_width + g.x0;
                std::uint32_t* const line = pixels + std::size_t(top + row) * unsigned(width);
                for (int column = c0; column < c1; ++column)
                {
                    if (!coverage[column]) continue;
                    if (const unsigned int alpha = canvas::div255(color_alpha * coverage[column]))
                        line[left + column] = canvas::mix(line[left + column], color, canvas::to256(alpha));
                }
            }
            x += g.xadvance;
            if (x > width - 24) break;
        }
    }
    // Any other size: bilinear scaling of the 28 px table (the original path).
    void paint_scaled(canvas& target, float x, float y, std::string_view value, float size, std::uint32_t color) const
    {
        const stbtt_packedchar* const table = m_glyphs.data() + base * glyph_count;
        const float scale = size / 28.f;
        for (std::size_t i = 0; i < value.size();)
        {
            const auto c = decode(value, i);
            const auto& g = table[glyph(c)];
            if (g.x1 <= g.x0 || g.y1 <= g.y0) { x += g.xadvance * scale; continue; }
            const float left = x + g.xoff * scale, top = y + g.yoff * scale;
            const float right = x + g.xoff2 * scale, bottom = y + g.yoff2 * scale;
            for (int py = std::max(0, int(std::floor(top))); py < std::min(target.height, int(std::ceil(bottom))); ++py)
                for (int px = std::max(0, int(std::floor(left))); px < std::min(target.width, int(std::ceil(right))); ++px)
                {
                    const float u = std::clamp(g.x0 + (px + .5f - left) / scale, float(g.x0), float(std::max(g.x0, std::uint16_t(g.x1 - 1))));
                    const float v = std::clamp(g.y0 + (py + .5f - top) / scale, float(g.y0), float(std::max(g.y0, std::uint16_t(g.y1 - 1))));
                    const int sx = int(u), sy = int(v), nx = std::min(sx + 1, int(g.x1) - 1), ny = std::min(sy + 1, int(g.y1) - 1);
                    if (nx < 0 || ny < 0) continue;
                    const float fx = u - sx, fy = v - sy;
                    const float a = m_atlas[sy * atlas_width + sx], b = m_atlas[sy * atlas_width + nx];
                    const float cc = m_atlas[ny * atlas_width + sx], d = m_atlas[ny * atlas_width + nx];
                    target.blend(px, py, color, static_cast<unsigned int>((a + (b - a) * fx) * (1 - fy) + (cc + (d - cc) * fx) * fy));
                }
            x += g.xadvance * scale;
            if (x > target.width - 24) break;
        }
    }
    // Advance width of a line already in visual order
    float measure_visual(std::string_view value, float size) const
    {
        if (m_atlas.empty() || !(size > 0)) return 0.f;
        const stbtt_packedchar* table = m_glyphs.data() + base * glyph_count;
        float scale = size / 28.f;
        if (m_exact)
            for (std::size_t k = 0; k < sizes.size(); ++k)
                if (size == sizes[k]) { table = m_glyphs.data() + k * glyph_count; scale = 1.f; break; }
        float width = 0.f;
        for (std::size_t i = 0; i < value.size();) width += table[glyph(decode(value, i))].xadvance * scale;
        return width;
    }
    // Width of a logical line without the cache (the reordering does not change it, the shaping does)
    float width_uncached(std::string_view logical, float size) const
    {
        return needs_visual(logical) ? measure_visual(arabic::visual_utf8(logical, false), size) : measure_visual(logical, size);
    }
public:
    // exact_sizes = false keeps the single 28 px atlas (used by the host test to compare paths).
    bool load(std::span<const std::uint8_t> bytes, bool exact_sizes = true)
    {
        return load(bytes, std::span<const std::uint8_t>{}, exact_sizes);
    }
    // 0.39: 'arabic' (DejaVu Sans; may be the same bytes as 'bytes') provides the Arabic forms. Neither span is kept.
    // A font whose glyphs do not fit with the Arabic forms keeps its Latin glyphs alone.
    bool load(std::span<const std::uint8_t> bytes, std::span<const std::uint8_t> arabic_bytes, bool exact_sizes = true)
    {
        if (bytes.size() < 256) return false;
        stbtt_fontinfo info{};
        const int offset = stbtt_GetFontOffsetForIndex(bytes.data(), 0);
        if (offset < 0 || !stbtt_InitFont(&info, bytes.data(), offset)) return false;
        stbtt_fontinfo arabic_info{};
        bool has_arabic_font = false;
        if (arabic_bytes.size() >= 256)
        {
            const int arabic_offset = stbtt_GetFontOffsetForIndex(arabic_bytes.data(), 0);
            has_arabic_font = arabic_offset >= 0 && stbtt_InitFont(&arabic_info, arabic_bytes.data(), arabic_offset) &&
                stbtt_FindGlyphIndex(&arabic_info, arabic_first + 0x5b) != 0; // U+FEDB, a lam form
        }
        std::vector<unsigned char> atlas;
        std::vector<stbtt_packedchar> glyphs(sizes.size() * glyph_count);
        // All sizes share a 1024x1024 atlas (1 MiB). A font too large for it keeps the old 28 px atlas.
        bool arabic = has_arabic_font;
        bool exact = false;
        if (exact_sizes)
        {
            exact = arabic && pack(bytes, info, arabic_bytes, &arabic_info, atlas, 1024, glyphs, 0, base);
            if (!exact && (exact = pack(bytes, info, {}, nullptr, atlas, 1024, glyphs, 0, base))) arabic = false;
        }
        if (!exact)
        {
            if (!arabic || !pack(bytes, info, arabic_bytes, &arabic_info, atlas, 512, glyphs, base, base))
            {
                arabic = false;
                if (!pack(bytes, info, {}, nullptr, atlas, 512, glyphs, base, base)) return false;
            }
        }
        m_atlas = std::move(atlas);
        m_glyphs = std::move(glyphs);
        m_exact = exact;
        m_arabic = arabic;
        m_generation = next_generation();
        std::lock_guard guard(m_cache.lock);
        m_cache.lines.clear();
        return true;
    }
    bool exact_sizes() const { return m_exact && !m_atlas.empty(); }
    // 0.39: the Arabic forms are in the atlas (load() was given an Arabic font and they fit)
    bool arabic() const { return m_arabic && !m_atlas.empty(); }
    // 0.39: changes with every load(), for caches of laid-out text
    std::uint64_t generation() const { return m_generation; }

    // 0.39: the visual order of a logical line (shaped, reordered, bidi marks removed). A line without Arabic or a
    // bidi mark is returned as it is; the others come from a cache. rtl = right-to-left interface.
    std::string visual(std::string_view logical, bool rtl) const
    {
        if (!needs_visual(logical)) return std::string(logical);
        std::string key;
        key.reserve(logical.size() + 2);
        key += rtl ? "V1" : "V0";
        key += logical;
        return cached(std::move(key), [&] { return arabic::visual_utf8(logical, rtl); });
    }

    // 0.39: code points of a logical line that would draw as '?' (outside the glyph table, missing in the font, or an
    // Arabic form without the Arabic font); a '?' of the text itself does not count
    std::size_t missing_glyphs(std::string_view logical, bool rtl) const
    {
        if (m_atlas.empty()) return 0;
        const std::string shown = visual(logical, rtl);
        const stbtt_packedchar* const table = m_glyphs.data() + base * glyph_count;
        const stbtt_packedchar& question = table['?' - 32];
        std::size_t missing = 0;
        for (std::size_t i = 0; i < shown.size();)
        {
            const unsigned int c = decode(shown, i);
            if (c == '?') continue;
            const int index = glyph(c);
            if (index == '?' - 32) { ++missing; continue; }
            const stbtt_packedchar& g = table[index];
            if (c != ' ' && g.x0 == question.x0 && g.y0 == question.y0 && g.x1 == question.x1 && g.y1 == question.y1) ++missing;
        }
        return missing;
    }

    // 0.38: advance width of 'value' at 'size', as paint() lays it out (for centred text)
    float measure(std::string_view value, float size = 28.f) const
    {
        if (needs_visual(value)) return measure_visual(visual(value, i18n::rtl()), size);
        return measure_visual(value, size);
    }

    // 0.39: 'logical' when it is at most max_width wide at 'size', else its longest prefix (cut on a UTF-8 boundary,
    // trailing spaces dropped) that fits with "…" (U+2026) appended. The ellipsis is at the logical end, so the visual
    // transform puts it on the left of an Arabic line. Results are cached.
    std::string fit(std::string_view logical, float size, float max_width, bool rtl) const
    {
        if (m_atlas.empty() || !(size > 0) || !(max_width > 0) || measure(logical, size) <= max_width) return std::string(logical);
        std::string key;
        key.reserve(logical.size() + 24);
        key += 'F';
        key += rtl ? '1' : '0';
        key += std::to_string(int(size * 4)) + ':' + std::to_string(int(max_width)) + ':';
        key += logical;
        return cached(std::move(key), [&]
        {
            static constexpr std::string_view ellipsis = "\xE2\x80\xA6";
            std::vector<std::size_t> cuts; // byte offsets of the code points after the first
            for (std::size_t i = 1; i < logical.size(); ++i)
                if ((static_cast<unsigned char>(logical[i]) & 0xc0) != 0x80) cuts.push_back(i);
            const auto candidate = [&](std::size_t bytes)
            {
                std::string_view prefix = logical.substr(0, bytes);
                while (!prefix.empty() && prefix.back() == ' ') prefix.remove_suffix(1);
                return std::string(prefix) + std::string(ellipsis);
            };
            // Largest prefix that fits (widths grow with the prefix)
            std::size_t lo = 0, hi = cuts.size();
            while (lo < hi)
            {
                const std::size_t mid = (lo + hi + 1) / 2;
                if (width_uncached(candidate(cuts[mid - 1]), size) <= max_width) lo = mid;
                else hi = mid - 1;
            }
            return lo ? candidate(cuts[lo - 1]) : std::string(ellipsis);
        });
    }

    // 'logical' broken at its spaces into at most 'max_lines' lines no wider than max_width at 'size', each line in
    // logical order (paint() shapes and reorders it). What does not fit in the last line ends it with "…", as does a
    // single word wider than a line. A line that fits is returned as it is (one line).
    std::vector<std::string> wrap(std::string_view logical, float size, float max_width, std::size_t max_lines, bool rtl) const
    {
        std::vector<std::string> out;
        if (!max_lines || logical.empty()) return out;
        if (m_atlas.empty() || !(size > 0) || !(max_width > 0) || measure(logical, size) <= max_width)
        {
            out.emplace_back(logical);
            return out;
        }
        std::string current;
        std::string_view rest = logical;
        while (!rest.empty())
        {
            if (out.size() + 1 == max_lines)
            {
                out.push_back(fit(current.empty() ? std::string(rest) : current + ' ' + std::string(rest), size, max_width, rtl));
                return out;
            }
            const std::size_t space = rest.find(' ');
            const std::string_view word = rest.substr(0, space);
            std::string candidate = current.empty() ? std::string(word) : current + ' ' + std::string(word);
            if (!current.empty() && measure(candidate, size) > max_width)
            {
                out.push_back(fit(current, size, max_width, rtl));
                current.clear();
                continue; // the word starts the next line
            }
            current = std::move(candidate);
            rest = space == std::string_view::npos ? std::string_view{} : rest.substr(space + 1);
        }
        if (!current.empty()) out.push_back(fit(current, size, max_width, rtl));
        return out;
    }

    void paint(canvas& target, float x, float y, std::string_view value, float size = 28.f, std::uint32_t color = 0xffeeeeee) const
    {
        if (m_atlas.empty() || !(size > 0) || !(std::abs(x) < 1e6f) || !(std::abs(y) < 1e6f)) return;
        std::string shown;
        if (needs_visual(value))
        {
            shown = visual(value, i18n::rtl());
            value = shown;
        }
        if (m_exact)
            for (std::size_t k = 0; k < sizes.size(); ++k)
                if (size == sizes[k]) { paint_exact(target, x, y, value, m_glyphs.data() + k * glyph_count, color); return; }
        paint_scaled(target, x, y, value, size, color);
    }
};

struct entry
{
    std::string path, title, subtitle, icon_name;
    image artwork;
    // Title ID (e.g. BLUS31426) of a game row, empty otherwise. Kept last so the existing
    // five-field brace initializers stay valid.
    std::string serial;
    // 0.37: installed from a PS3 disc (it has a lock record), and that disc is not in the drive now
    bool disc_bound = false;
    bool locked = false;
};

// The lines of one wrapped text, laid out again only when the text, the room, the font or the direction change
struct wrapped_text
{
    std::string text;
    int room = -1;
    float size = 0;
    std::size_t max_lines = 0;
    std::uint64_t generation = 0;
    bool rtl = false;
    std::vector<std::string> lines;

    const std::vector<std::string>& get(const font& typeface, std::string_view value, float text_size, float width, std::size_t most, bool right_to_left)
    {
        const int r = int(width);
        if (r != room || text_size != size || most != max_lines || right_to_left != rtl || typeface.generation() != generation || value != text)
        {
            text = std::string(value);
            room = r;
            size = text_size;
            max_lines = most;
            rtl = right_to_left;
            generation = typeface.generation();
            lines = typeface.wrap(value, text_size, float(r), most, right_to_left);
        }
        return lines;
    }
};

// A settings page in the PS3 'Settings' list look (scene::draw_settings_page): section captions and setting lines with
// the value that runs and a badge saying who decides it. Texts are final (interface language); the scene only fits them.
struct page_line
{
    std::string label, value, badge;
    bool caption = false; // a section caption: not selectable
    bool padlock = false; // a PS4 limit decides the value: the badge carries the padlock
    bool greyed = false;  // unavailable, read only, or waiting for another setting
    bool mine = false;    // the player's own choice: a brighter badge
};

// A pad glyph and its label in a page's footer
struct page_button
{
    std::string glyph; // tex_cross, tex_circle, tex_square, tex_triangle
    std::string label;
};

// One value of the list a page opens over its right half (the PS3 option list)
struct page_choice
{
    std::string label;
    bool greyed = false;  // refused here (its reason is the page's help line)
    bool current = false; // the radio dot
};

struct page_popup
{
    std::string title;
    std::vector<page_choice> choices;
    std::size_t selected = 0;
    bool radio = true; // values of a setting (radio dots); false: a list of actions
};

struct settings_page
{
    std::string icon = "tex_sysconf"; // resource of the tab icon
    std::string title, scope;         // tab name; whose settings ("Todos os jogos", the game's title ID)
    std::span<const page_line> lines;
    std::size_t selected = 0;         // a line that is not a caption
    std::string help;                 // the selected line's help (two lines at most)
    std::string message;              // what the last action did (two lines at most)
    std::vector<page_button> buttons;
    std::string footer_help;
    const page_popup* popup = nullptr;
};

class scene
{
    static constexpr int width = 1280, height = 720;
    static constexpr int ribbons = 5, band = 38; // each ribbon covers middle - 38 .. middle + 38
    static constexpr std::uint32_t ribbon_color = 0xd4cef8, highlight_color = 0xe7e5ff;
    // The ribbons drift by less than 5 px/s, so redrawing them at 20 Hz looks the same as every frame.
    static constexpr double layer_period = 1.0 / 20;
    std::vector<std::uint32_t> m_background;
    std::vector<std::uint32_t> m_layer; // gradient + ribbons, reused between refreshes
    double m_layer_time = 0;
    bool m_layer_valid = false;
    // 256 - ribbon weight for each row offset: [0] outer ribbons, [1] the brighter middle one.
    std::array<std::array<std::uint16_t, 2 * band + 1>, 2> m_keep{};
    float m_category = 6.f, m_row = 0.f;
    wrapped_text m_subtitle; // the selected row's help line

    // The settings page: list geometry, eased cursor and scroll, the fitted texts of its lines (fitted again only when a
    // line, the font or the direction changes), its help and message lines
    static constexpr int page_list_top = 170, page_row_height = 34, page_rows = 11;
    static constexpr int page_list_bottom = page_list_top + page_row_height * page_rows;
    static constexpr float page_left = 170, page_right = 1110, page_label_max = 560, page_value_max = 360;
    static constexpr int popup_left = 700, popup_width = 540, popup_top = 100, popup_row_height = 36, popup_rows = 11;
    struct fitted_line
    {
        bool valid = false, caption = false, padlock = false;
        std::string label, value, badge;            // the line as given
        std::string shown_label, shown_value;      // fitted
        float label_width = 0, value_width = 0, badge_width = 0, badge_text_width = 0;
    };
    std::vector<fitted_line> m_page_fit;
    std::uint64_t m_page_generation = 0;
    bool m_page_rtl = false;
    std::string m_page_key, m_popup_key;
    std::size_t m_page_first = 0, m_popup_first = 0;
    float m_page_scroll = 0, m_page_cursor = 0, m_popup_scroll = 0, m_popup_cursor = 0;
    wrapped_text m_page_help, m_page_message;

    const image* resource(std::string_view name) const
    {
        const auto found = resources.find(name);
        return found != resources.end() && found->second ? &found->second : nullptr;
    }

    // An anti-aliased disc centred on (cx, cy); inner > 0 leaves a ring
    static void disc(canvas& c, float cx, float cy, float outer, float inner, std::uint32_t color)
    {
        for (int y = int(std::floor(cy - outer - 1)); y <= int(std::ceil(cy + outer + 1)); ++y)
            for (int x = int(std::floor(cx - outer - 1)); x <= int(std::ceil(cx + outer + 1)); ++x)
            {
                const float d = std::hypot(x + .5f - cx, y + .5f - cy);
                const float cover = std::clamp(outer + .5f - d, 0.f, 1.f) - (inner > 0 ? std::clamp(inner + .5f - d, 0.f, 1.f) : 0.f);
                if (cover > 0) c.blend(x, y, color, unsigned(cover * 255 + .5f));
            }
    }

    // A rectangle with its corners cut by two pixels (the badges)
    static void chip(canvas& c, int x, int y, int w, int h, std::uint32_t color)
    {
        if (w < 5 || h < 5) return;
        c.rectangle(x + 2, y, w - 4, 1, color);
        c.rectangle(x + 1, y + 1, w - 2, 1, color);
        c.rectangle(x, y + 2, w, h - 4, color);
        c.rectangle(x + 1, y + h - 2, w - 2, 1, color);
        c.rectangle(x + 2, y + h - 1, w - 4, 1, color);
    }

    // The first line of a list window that keeps 'selected' two lines away from its edges
    static std::size_t window_first(std::size_t first, std::size_t selected, std::size_t count, std::size_t rows)
    {
        if (selected < first + 2) first = selected >= 2 ? selected - 2 : 0;
        if (selected + 3 > first + rows) first = selected + 3 - rows;
        return std::min(first, count > rows ? count - rows : 0);
    }

    fitted_line& fit_line(const font& typeface, std::size_t i, const page_line& l, bool rtl)
    {
        fitted_line& f = m_page_fit[i];
        if (f.valid && f.caption == l.caption && f.padlock == l.padlock && f.label == l.label && f.value == l.value && f.badge == l.badge) return f;
        f.valid = true;
        f.caption = l.caption;
        f.padlock = l.padlock;
        f.label = l.label;
        f.value = l.value;
        f.badge = l.badge;
        if (l.caption)
        {
            f.shown_label = typeface.fit(l.label, 18, page_right - 150, rtl);
            f.label_width = typeface.measure(f.shown_label, 18);
            return f;
        }
        f.badge_text_width = l.badge.empty() ? 0 : typeface.measure(l.badge, 17);
        f.badge_width = l.badge.empty() ? 0 : f.badge_text_width + 16 + (l.padlock ? 18 : 0);
        // The label keeps up to 560 px and the value at least 360 px; a short label leaves the rest to the value
        const float room = page_right - page_left - 20 - (f.badge_width > 0 ? f.badge_width + 14 : 0);
        const float label_natural = std::min(typeface.measure(l.label, 21), page_label_max);
        f.shown_value = typeface.fit(l.value, 21, std::max(page_value_max, room - label_natural), rtl);
        f.value_width = typeface.measure(f.shown_value, 21);
        f.shown_label = typeface.fit(l.label, 21, std::max(std::min(page_label_max, room - f.value_width), 60.f), rtl);
        f.label_width = typeface.measure(f.shown_label, 21);
        return f;
    }

    // Gradient plus the five ribbons. All ribbons share one colour, so blending them one after another
    // equals one blend with the product of their transmittances; each column is mixed once.
    void layer(std::uint32_t* pixels, double seconds) const
    {
        std::copy(m_background.begin(), m_background.end(), pixels);
        const float time = float(seconds);
        std::array<std::uint32_t, height> transmit; // 16.16, 65536 = untouched
        for (int x = 0; x < width; ++x)
        {
            const float u = float(x) / width;
            const float arch = 486 - 103 * std::sin(u * 3.141593f);
            std::array<int, ribbons> middle{};
            int lo = height, hi = -1;
            for (int ribbon = 0; ribbon < ribbons; ++ribbon)
            {
                middle[ribbon] = int(arch + 18 * std::sin(u * 4.1f + time * .17f + ribbon * .18f) + 12 * std::sin(u * 6.3f - time * .11f + ribbon * .36f) + ribbon * 9);
                lo = std::min(lo, middle[ribbon] - band);
                hi = std::max(hi, middle[ribbon] + band);
            }
            const int first = std::max(lo, 0), last = std::min(hi, height - 1);
            if (first <= last)
            {
                std::fill(transmit.begin() + first, transmit.begin() + last + 1, 65536u);
                for (int ribbon = 0; ribbon < ribbons; ++ribbon)
                {
                    const auto& keep = m_keep[ribbon == 2];
                    const int top = middle[ribbon] - band;
                    for (int d = std::max(0, first - top), end = std::min(2 * band, last - top); d <= end; ++d)
                        transmit[top + d] = (transmit[top + d] * keep[d]) >> 8;
                }
                for (int y = first; y <= last; ++y)
                    if (const unsigned int weight = (65536u - transmit[y] + 128) >> 8)
                    {
                        auto& p = pixels[std::size_t(y) * width + x];
                        p = canvas::mix(p, ribbon_color, weight);
                    }
            }
            for (int ribbon = 0; ribbon < ribbons; ++ribbon)
                if (const int y = middle[ribbon] - 5; unsigned(y) < unsigned(height))
                {
                    auto& p = pixels[std::size_t(y) * width + x];
                    p = canvas::mix(p, highlight_color, canvas::to256(28));
                }
        }
    }
    static void particles(std::uint32_t* pixels, double seconds)
    {
        canvas c(pixels, width, height);
        for (unsigned int i = 0; i < 190; ++i)
        {
            const float u = float((i * 719 + 17) % 1280);
            const int x = int(std::fmod(u + float(seconds) * (1 + i % 3), 1280.f));
            const int y = int(490 - 103 * std::sin(x / 1280.f * 3.141593f) + 18 * std::sin(x / 1280.f * 4.1f + float(seconds) * .17f) + int((i * 79) % 148) - 70);
            const unsigned int alpha = unsigned(30 + 90 * (.5f + .5f * std::sin(float(seconds) * .7f + i)));
            c.blend(x, y, 0xfff4f0ff, alpha);
            if (i % 11 == 0) { c.blend(x - 1, y, 0xfff4f0ff, alpha / 3); c.blend(x + 1, y, 0xfff4f0ff, alpha / 3); }
        }
    }
public:
    static constexpr std::size_t category_count = 10;
    // 0.39: category names in the interface language ("PlayStation Network" is a name in every language)
    static const char* category_name(int category)
    {
        using i18n::str;
        static constexpr std::array<str, category_count> names{str::XMB_USUARIOS, str::XMB_CONFIGURACOES, str::XMB_FOTO, str::XMB_MUSICA,
            str::XMB_VIDEO, str::XMB_TV_SERVICOS_VIDEO, str::XMB_JOGO, str::XMB_REDE, str::count_, str::XMB_AMIGOS};
        if (category < 0 || category >= int(category_count)) return "";
        if (names[std::size_t(category)] == str::count_) return "PlayStation Network";
        return i18n::tr(names[std::size_t(category)]);
    }
    static constexpr std::array<const char*, category_count> icons{"tex_user", "tex_sysconf", "tex_photo", "tex_music", "tex_video", "tex_tv", "tex_game", "tex_network", "tex_psn", "tex_friend"};
    std::map<std::string, image, std::less<>> resources;
    // 0.39: right-to-left layout (message line and footer mirrored); unset follows the interface language
    std::optional<bool> rtl;
    bool rtl_layout() const { return rtl.value_or(i18n::rtl()); }
    // draw() reuses the gradient + ribbon layer for up to 50 ms and draws the particles every frame.
    // The layer is built in cached memory and copied once, which also keeps blending out of the
    // write-combined scanout buffer. false redraws everything per frame (background()).
    bool cached_background = true;
    scene() : m_background(width * height), m_layer(width * height)
    {
        for (int y = 0; y < height; ++y)
        {
            const float v = float(y) / height, dy = (y - 20.f) / 1100.f, dy2 = dy * dy;
            for (int x = 0; x < width; ++x)
            {
                const float dx = (x - 1000.f) / 1400.f;
                const float glow = 1.f - std::min(1.f, std::sqrt(dx * dx + dy2));
                const unsigned int r = unsigned(65 + 25 * v + 33 * glow), g = unsigned(68 - 14 * v + 41 * glow), b = unsigned(105 - 1 * v + 45 * glow);
                m_background[y * width + x] = 0xff000000 | (r << 16) | (g << 8) | b;
            }
        }
        for (int k = 0; k < 2; ++k) for (int d = -band; d <= band; ++d)
        {
            const float amount = 1.f - std::abs(d) / 39.f;
            m_keep[k][d + band] = std::uint16_t(256 - canvas::to256(unsigned(amount * amount * (k ? 27 : 19))));
        }
    }
    void background(std::uint32_t* pixels, double seconds) const
    {
        layer(pixels, seconds);
        particles(pixels, seconds);
    }
    // 0.38: the waves alone (cached layer + particles), under the system dialogs (ps4_xmb_dialog.hpp)
    void backdrop(std::uint32_t* pixels, double seconds)
    {
        if (cached_background && m_layer.size() == m_background.size())
        {
            if (!m_layer_valid || !(seconds >= m_layer_time) || seconds - m_layer_time >= layer_period)
            {
                layer(m_layer.data(), seconds);
                m_layer_time = seconds;
                m_layer_valid = true;
            }
            std::copy(m_layer.begin(), m_layer.end(), pixels);
            particles(pixels, seconds);
        }
        else background(pixels, seconds);
    }
    void draw(std::uint32_t* pixels, const font& typeface, int category, std::size_t selected,
        std::span<const entry> entries, double seconds, float elapsed, std::string_view clock, std::string_view message, bool settings = false,
        std::string_view settings_help = {})
    {
        // A single slow frame moves the selection at most as far as a 50 ms step.
        const float step = elapsed > 0.f ? std::min(elapsed, .05f) : 0.f;
        const float ease = 1.f - std::exp(-step / .065f);
        m_category += (category - m_category) * ease;
        m_row += (float(selected) - m_row) * ease;
        backdrop(pixels, seconds);
        canvas c(pixels, width, height);
        c.rectangle(900, 50, 380, 39, 0x167d90ac);
        c.rectangle(900, 50, 380, 1, 0x54dae0f2); c.rectangle(900, 88, 380, 1, 0x30dae0f2);
        c.rectangle(900, 50, 1, 39, 0x54dae0f2);
        typeface.paint(c, 1050, 77, clock, 23);
        const bool right_to_left = rtl_layout();
        for (int i = 0; i < int(category_count); ++i)
        {
            const float x = 375 + (i - m_category) * 136;
            const float focus = std::max(0.f, 1.f - std::abs(i - m_category));
            const float size = 68 + 46 * focus;
            if (const auto found = resources.find(icons[i]); found != resources.end()) c.blit(found->second, x - size / 2, 190 - size / 2, size, size, unsigned(140 + 115 * focus));
            else { c.rectangle(int(x) - 19, 174, 38, 30, 0x65e1e0f3); }
        }
        typeface.paint(c, 352, 238, category_name(category), 21);
        const float column = 375 + (category - m_category) * 136;
        for (std::size_t i = 0; i < entries.size(); ++i)
        {
            const float distance = float(i) - m_row;
            const float y = distance < -1 ? 112 + (distance + 1) * 68 : distance < 0 ? 335 + distance * 223 : distance < 1 ? 335 + distance * 124 : 459 + (distance - 1) * 65;
            if (y < -90 || y > 692) continue;
            const float focus = std::max(0.f, 1.f - std::abs(distance));
            const float size = 42 + 54 * focus;
            const unsigned int opacity = unsigned(96 + 159 * focus);
            const image* icon = entries[i].artwork ? &entries[i].artwork : nullptr;
            if (!icon) if (const auto found = resources.find(entries[i].icon_name); found != resources.end()) icon = &found->second;
            // 0.37: a locked game is dimmed and carries the padlock (tex_lock_icon) in its middle
            const bool locked = entries[i].locked;
            const unsigned int icon_opacity = locked ? opacity * 3 / 5 : opacity;
            float icon_height = size * 2 / 3;
            if (icon)
            {
                const float w = size, h = size * icon->height / icon->width;
                icon_height = h;
                c.blit(*icon, column - w / 2, y - h / 2, w, h, icon_opacity);
            }
            else c.rectangle(int(column) - int(size / 2), int(y) - int(size / 3), int(size), int(size * 2 / 3), (icon_opacity << 24) | 0xd2d1e8);
            if (locked)
                if (const auto lock = resources.find("tex_lock_icon"); lock != resources.end() && lock->second)
                {
                    const float lh = std::max(18.f, icon_height * .70f), lw = lh * lock->second.width / lock->second.height;
                    c.blit(lock->second, column - lw / 2, y - lh / 2 + lh * .05f, lw, lh, opacity);
                }
            if (i == selected)
            {
                // 0.39: the text stays on the icon side in both directions. A line with Arabic wider than the screen ends
                // with "…" (the right edge would cut the start of its sentence); other lines keep the edge cut.
                const auto alpha = std::uint32_t(255 * focus) << 24;
                const float room = 1240 - (column + 75);
                const auto fitted = [&](const std::string& text, float size)
                { return arabic::has_arabic(text) ? typeface.fit(text, size, room, right_to_left) : text; };
                typeface.paint(c, column + 75, y + 8, fitted(entries[i].title, 27), 27, alpha | 0xfbfbff);
                // The help line wider than the room continues on a second line (the firmware font is wider than DejaVu)
                const std::string subtitle = locked ? entries[i].subtitle + i18n::tr(i18n::str::XMB_PRECISA_DISCO_LEITOR) : entries[i].subtitle;
                const std::vector<std::string>& lines = m_subtitle.get(typeface, subtitle, 17, room, 2, right_to_left);
                for (std::size_t k = 0; k < lines.size(); ++k)
                    typeface.paint(c, column + 75, y + 33 + float(k) * 21, lines[k], 17, alpha | 0xdfdce9);
            }
        }
        // 0.39: the message line: from x 60 left to right, or ending at x 1220 right to left, fitted to 1160 px when it
        // is right-aligned or holds Arabic
        if (!message.empty())
        {
            const std::string line = right_to_left || arabic::has_arabic(message) ? typeface.fit(message, 20, 1160, right_to_left) : std::string(message);
            const float x = right_to_left ? std::round(1220 - typeface.measure(line, 20)) : 60.f;
            typeface.paint(c, x, 632, line, 20, 0xffffd8b3);
        }
        using i18n::str;
        // The settings lists (top level, PS4 tab) take X and Circle only; 'settings_help' names their other buttons
        const std::array<const char*, 3> buttons{"tex_cross", "tex_circle", "tex_triangle"};
        const std::array<const char*, 3> hints{i18n::tr(str::COM_SELECIONAR), i18n::tr(str::COM_VOLTAR), i18n::tr(str::COM_ATUALIZAR)};
        for (int i = 0; i < (settings ? 2 : 3); ++i)
        {
            // 0.39: mirrored for a right-to-left interface: the pairs from the right edge, each hint left of its glyph
            const int x = right_to_left ? 1280 - 60 - 17 - i * 150 : 60 + i * 150;
            if (const auto found = resources.find(buttons[i]); found != resources.end()) c.blit(found->second, float(x), 668, 17, 17, 220);
            else typeface.paint(c, float(x), 682, i == 0 ? "X" : i == 1 ? "O" : "T", 18, 0xffd5d2e5);
            const float hint_x = right_to_left ? std::round(float(x) - 9 - typeface.measure(hints[i], 18)) : float(x + 26);
            typeface.paint(c, hint_x, 682, hints[i], 18, 0xffd5d2e5);
        }
        // Options no longer leaves the application (0.25): closing is the PS button menu.
        const std::string_view help = settings ? settings_help : std::string_view(i18n::tr(str::XMB_QUADRADO_CONFIGURAR_JOGO_BOTAO_PS));
        if (!help.empty()) typeface.paint(c, right_to_left ? std::round(760 - typeface.measure(help, 18)) : 520.f, 682, help, 18, 0xffd5d2e5);
    }

    // A settings page (one tab of the settings browser) in the PS3 'Settings' list look: the waves, the tab's icon and
    // name over whose settings these are, about 11 lines at a time (captions, then label, badge and value; the list
    // scrolls with an eased cursor and a scroll bar), the selected line's help and the message in two lines each, and
    // the footer. page.popup draws a list of values (radio dots) or actions over the right half. Right to left the
    // whole layout is mirrored: labels right-aligned at x 1110, values from x 170, the popup over the left half.
    void draw_settings_page(std::uint32_t* pixels, const font& typeface, const settings_page& page, double seconds, float elapsed, std::string_view clock)
    {
        const float step = elapsed > 0.f ? std::min(elapsed, .05f) : 0.f;
        const float ease = 1.f - std::exp(-step / .065f);
        backdrop(pixels, seconds);
        canvas c(pixels, width, height);
        c.rectangle(900, 50, 380, 39, 0x167d90ac);
        c.rectangle(900, 50, 380, 1, 0x54dae0f2); c.rectangle(900, 88, 380, 1, 0x30dae0f2);
        c.rectangle(900, 50, 1, 39, 0x54dae0f2);
        typeface.paint(c, 1050, 77, clock, 23);
        const bool rtl = rtl_layout();
        // x of a span that starts at 'x' left to right, mirrored for a right-to-left interface
        const auto at = [&](float x, float w) { return rtl ? std::round(float(width) - x - w) : x; };
        const auto box = [&](int x, int y, int w, int h, std::uint32_t color) { c.rectangle(int(at(float(x), float(w))), y, w, h, color); };

        // Header: the tab's icon, its name and whose settings
        if (const image* icon = resource(page.icon)) c.blit(*icon, at(118, 48), 98, 48, 48, 235);
        const std::string title = typeface.fit(page.title, 27, 700, rtl), scope = typeface.fit(page.scope, 17, 700, rtl);
        typeface.paint(c, at(180, typeface.measure(title, 27)), 128, title, 27, 0xfffbfbff);
        typeface.paint(c, at(180, typeface.measure(scope, 17)), 152, scope, 17, 0xffc6c6ce);
        box(120, 162, 1040, 1, 0x30dae0f2);

        // Lines: the window follows the cursor; a new page (or a font or direction change) starts without easing
        const std::size_t count = page.lines.size();
        const std::size_t selected = count ? std::min(page.selected, count - 1) : 0;
        std::string key = page.title + '\x1f' + page.scope + '\x1f' + std::to_string(count);
        const bool fresh = key != m_page_key || typeface.generation() != m_page_generation || rtl != m_page_rtl;
        if (fresh || m_page_fit.size() != count)
        {
            m_page_fit.assign(count, {});
            m_page_generation = typeface.generation();
            m_page_rtl = rtl;
        }
        m_page_first = window_first(fresh ? 0 : m_page_first, selected, count, page_rows);
        if (fresh)
        {
            m_page_key = std::move(key);
            m_page_scroll = float(m_page_first);
            m_page_cursor = float(selected);
        }
        else
        {
            m_page_scroll += (float(m_page_first) - m_page_scroll) * ease;
            m_page_cursor += (float(selected) - m_page_cursor) * ease;
        }
        const float row_h = float(page_row_height);
        const float band_y = page_list_top + (m_page_cursor - m_page_scroll) * row_h;
        if (count && band_y > page_list_top - row_h && band_y < page_list_bottom)
        {
            const int y = int(std::round(band_y));
            box(140, y, 1000, page_row_height, 0x1cffffff);
            box(140, y, 1000, 1, 0x4cffffff);
            box(140, y + page_row_height - 1, 1000, 1, 0x30ffffff);
        }
        const int first = std::max(0, int(std::floor(m_page_scroll)) - 1);
        const int last = std::min(int(count), int(std::ceil(m_page_scroll)) + page_rows + 1);
        for (int i = first; i < last; ++i)
        {
            const float top = page_list_top + (float(i) - m_page_scroll) * row_h;
            const float fade = top < page_list_top ? 1 - (page_list_top - top) / row_h
                : top + row_h > page_list_bottom ? 1 - (top + row_h - page_list_bottom) / row_h : 1.f;
            if (fade <= .05f) continue;
            const unsigned opacity = unsigned(255 * std::clamp(fade, 0.f, 1.f));
            const auto alpha = [&](unsigned a) { return std::uint32_t(a * opacity / 255) << 24; };
            const page_line& l = page.lines[std::size_t(i)];
            const fitted_line& f = fit_line(typeface, std::size_t(i), l, rtl);
            const float baseline = std::round(top + 24);
            if (l.caption)
            {
                typeface.paint(c, at(150, f.label_width), baseline + 1, f.shown_label, 18, alpha(255) | 0xb3b0c8);
                const float line_x = 150 + f.label_width + 14;
                if (line_x < page_right) box(int(line_x), int(top) + 19, int(page_right - line_x), 1, alpha(48) | 0xdae0f2);
                continue;
            }
            const unsigned text = l.greyed ? 128 : 255;
            typeface.paint(c, at(page_left, f.label_width), baseline, f.shown_label, 21, alpha(text) | 0xffffff);
            const float value_x = page_right - f.value_width;
            typeface.paint(c, at(value_x, f.value_width), baseline, f.shown_value, 21, alpha(l.greyed ? 128 : 235) | 0xe7e5f2);
            if (f.badge_width > 0)
            {
                const float bx = value_x - 14 - f.badge_width;
                const std::uint32_t fill = l.mine ? 0x5ef2c46b : l.padlock ? 0x48ffffff : 0x34ffffff;
                chip(c, int(at(bx, f.badge_width)), int(top) + 6, int(f.badge_width), 22, alpha(fill >> 24) | (fill & 0xffffff));
                float tx = bx + 8;
                if (l.padlock)
                    if (const image* lock = resource("tex_lock_icon"))
                    {
                        const float lh = 15, lw = lh * float(lock->width) / float(std::max(lock->height, 1));
                        c.blit(*lock, at(bx + 7, lw), top + 9.5f, lw, lh, opacity);
                    }
                if (l.padlock) tx += 18;
                typeface.paint(c, at(tx, f.badge_text_width), baseline - 2, l.badge, 17, alpha(l.greyed ? 170 : 255) | 0xf4f2fa);
            }
        }
        // Scroll bar of a list longer than the window
        if (count > std::size_t(page_rows))
        {
            const int track = page_rows * page_row_height;
            const int thumb = std::max(24, int(track * page_rows / int(count)));
            const float range = float(count - std::size_t(page_rows));
            const int y = page_list_top + int(std::round(float(track - thumb) * std::clamp(m_page_scroll / range, 0.f, 1.f)));
            box(1150, page_list_top, 3, track, 0x22ffffff);
            box(1150, y, 3, thumb, 0x90ffffff);
        }

        // The selected line's help and the message, two lines each
        const auto lines_at = [&](wrapped_text& cache, std::string_view value, float size, float left, float room, std::initializer_list<float> baselines,
            std::uint32_t color)
        {
            const std::vector<std::string>& lines = cache.get(typeface, value, size, room, baselines.size(), rtl);
            std::size_t k = 0;
            for (const float y : baselines)
            {
                if (k >= lines.size()) break;
                typeface.paint(c, at(left, typeface.measure(lines[k], size)), y, lines[k], size, color);
                ++k;
            }
        };
        lines_at(m_page_help, page.help, 17, page_left, page_right - page_left, {577, 598}, 0xffdfdce9);
        lines_at(m_page_message, page.message, 20, 60, 1160, {632, 654}, 0xffffd8b3);

        // Footer: each glyph and its label, then the help, from the left (mirrored: from the right)
        float x = 60;
        for (const page_button& b : page.buttons)
        {
            const float label_width = typeface.measure(b.label, 18);
            if (const image* glyph = resource(b.glyph)) c.blit(*glyph, at(x, 17), 668, 17, 17, 220);
            else
            {
                const char* stand_in = b.glyph == "tex_cross" ? "X" : b.glyph == "tex_circle" ? "O" : b.glyph == "tex_triangle" ? "T" : "[]";
                typeface.paint(c, at(x, typeface.measure(stand_in, 18)), 682, stand_in, 18, 0xffd5d2e5);
            }
            typeface.paint(c, at(x + 26, label_width), 682, b.label, 18, 0xffd5d2e5);
            x += 26 + label_width + 34;
        }
        if (!page.footer_help.empty())
        {
            const std::string help = typeface.fit(page.footer_help, 18, std::max(1220.f - x, 60.f), rtl);
            typeface.paint(c, at(x + 6, typeface.measure(help, 18)), 682, help, 18, 0xffd5d2e5);
        }

        // The list of values or actions over the right half
        if (!page.popup)
        {
            m_popup_key.clear();
            return;
        }
        const page_popup& popup = *page.popup;
        const std::size_t choices = popup.choices.size();
        const std::size_t current = choices ? std::min(popup.selected, choices - 1) : 0;
        std::string popup_key = popup.title + '\x1f' + std::to_string(choices) + (popup.radio ? "r" : "a");
        const bool popup_fresh = popup_key != m_popup_key;
        m_popup_first = window_first(popup_fresh ? 0 : m_popup_first, current, choices, popup_rows);
        if (popup_fresh)
        {
            m_popup_key = std::move(popup_key);
            m_popup_scroll = float(m_popup_first);
            m_popup_cursor = float(current);
        }
        else
        {
            m_popup_scroll += (float(m_popup_first) - m_popup_scroll) * ease;
            m_popup_cursor += (float(current) - m_popup_cursor) * ease;
        }
        // as tall as its values (at most popup_rows), above the help lines
        const int panel_height = 60 + int(std::min(choices, std::size_t(popup_rows))) * popup_row_height;
        box(popup_left, popup_top, popup_width, panel_height, 0xf0121426);
        box(popup_left, popup_top, 1, panel_height, 0x60dae0f2);
        const std::string heading = typeface.fit(popup.title, 18, float(popup_width - 60), rtl);
        typeface.paint(c, at(popup_left + 30, typeface.measure(heading, 18)), popup_top + 32, heading, 18, 0xffc6c6ce);
        const int items_top = popup_top + 48, items_bottom = items_top + popup_rows * popup_row_height;
        const float item_h = float(popup_row_height);
        if (const float band = items_top + (m_popup_cursor - m_popup_scroll) * item_h; choices && band > items_top - item_h && band < items_bottom)
        {
            const int y = int(std::round(band));
            box(popup_left + 10, y, popup_width - 20, popup_row_height, 0x2cffffff);
            box(popup_left + 10, y, popup_width - 20, 1, 0x50ffffff);
            box(popup_left + 10, y + popup_row_height - 1, popup_width - 20, 1, 0x34ffffff);
        }
        const float text_x = float(popup_left + (popup.radio ? 56 : 30));
        const float text_room = float(popup_left + popup_width - 20) - text_x;
        const int from = std::max(0, int(std::floor(m_popup_scroll)) - 1);
        const int to = std::min(int(choices), int(std::ceil(m_popup_scroll)) + popup_rows + 1);
        for (int i = from; i < to; ++i)
        {
            const float top = items_top + (float(i) - m_popup_scroll) * item_h;
            const float fade = top < items_top ? 1 - (items_top - top) / item_h : top + item_h > items_bottom ? 1 - (top + item_h - items_bottom) / item_h : 1.f;
            if (fade <= .05f) continue;
            const unsigned opacity = unsigned(255 * std::clamp(fade, 0.f, 1.f));
            const page_choice& choice = popup.choices[std::size_t(i)];
            const unsigned strength = (choice.greyed ? 120 : 255) * opacity / 255;
            if (popup.radio)
            {
                const float cx = at(float(popup_left + 30), 0), cy = top + item_h / 2;
                disc(c, cx, cy, 7.f, 5.2f, (std::uint32_t(strength * 210 / 255) << 24) | 0xffffff);
                if (choice.current) disc(c, cx, cy, 3.6f, 0, (std::uint32_t(strength) << 24) | 0xffffff);
            }
            const std::string label = typeface.fit(choice.label, 21, text_room, rtl);
            typeface.paint(c, at(text_x, typeface.measure(label, 21)), std::round(top + 25), label, 21, (std::uint32_t(strength) << 24) | 0xffffff);
        }
        if (choices > std::size_t(popup_rows))
        {
            const int track = popup_rows * popup_row_height;
            const int thumb = std::max(24, int(track * popup_rows / int(choices)));
            const float range = float(choices - std::size_t(popup_rows));
            const int y = items_top + int(std::round(float(track - thumb) * std::clamp(m_popup_scroll / range, 0.f, 1.f)));
            box(popup_left + popup_width - 8, items_top, 3, track, 0x22ffffff);
            box(popup_left + popup_width - 8, y, 3, thumb, 0x90ffffff);
        }
    }
};
}
