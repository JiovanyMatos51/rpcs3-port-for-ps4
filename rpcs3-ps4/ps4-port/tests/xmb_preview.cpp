#define STB_TRUETYPE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image_write.h>
#include <stb_image.h>
#include "util/ps4_xmb.hpp"
#include "util/ps4_xmb_dialog.hpp"
#include "util/ps4_padlock_icon.hpp"
#include "util/ps4_folder_icon.hpp"
#include "util/ps4_xmb_media.hpp"
#include "util/ps4_cfg_catalog.hpp"
#include "util/ps4_cheats.hpp"
#include "3rdparty/zlib/zlib/zlib.h"
#include <cassert>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <chrono>

#if defined(__SANITIZE_ADDRESS__)
#define XMB_PREVIEW_SANITIZED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define XMB_PREVIEW_SANITIZED 1
#endif
#endif

#undef assert
#define assert(condition) do { if (!(condition)) { std::cerr << "FAIL at line " << __LINE__ << ": " << #condition << '\n'; return 1; } } while (false)

using namespace utils::ps4::xmb;

// The 0.29 compositor (float blending, one blend per ribbon row, a single 28 px atlas scaled to every
// size), kept as the reference for the integer paths. Its font packs the 230 code points as one range:
// the original Latin-1 + symbols split makes stb_truetype read past the glyph table when a glyph is
// missing, which crashed this test about half of the time.
namespace legacy
{
class canvas
{
    std::uint32_t* m_pixels;
public:
    int width, height;
    canvas(std::uint32_t* pixels, int w, int h) : m_pixels(pixels), width(w), height(h) {}
    void blend(int x, int y, std::uint32_t color, unsigned int opacity = 255)
    {
        if (x < 0 || y < 0 || x >= width || y >= height) return;
        const unsigned int alpha = (color >> 24) * std::min(opacity, 255u) / 255;
        if (!alpha) return;
        auto& destination = m_pixels[y * width + x];
        std::uint32_t value = 0xff000000;
        for (unsigned int shift : {0u, 8u, 16u})
            value |= ((((color >> shift) & 255) * alpha + ((destination >> shift) & 255) * (255 - alpha) + 127) / 255) << shift;
        destination = value;
    }
    void rectangle(int x, int y, int w, int h, std::uint32_t color)
    {
        for (int py = std::max(y, 0); py < std::min(y + h, height); ++py)
            for (int px = std::max(x, 0); px < std::min(x + w, width); ++px) blend(px, py, color);
    }
    void blit(const image& source, float x, float y, float w, float h, unsigned int opacity = 255)
    {
        if (!source || w <= 0 || h <= 0) return;
        const int x0 = std::max(0, int(std::floor(x))), y0 = std::max(0, int(std::floor(y)));
        const int x1 = std::min(width, int(std::ceil(x + w))), y1 = std::min(height, int(std::ceil(y + h)));
        for (int py = y0; py < y1; ++py) for (int px = x0; px < x1; ++px)
        {
            const float u = std::clamp((px + .5f - x) * source.width / w - .5f, 0.f, float(source.width - 1));
            const float v = std::clamp((py + .5f - y) * source.height / h - .5f, 0.f, float(source.height - 1));
            const int sx = int(u), sy = int(v), nx = std::min(sx + 1, source.width - 1), ny = std::min(sy + 1, source.height - 1);
            const float fx = u - sx, fy = v - sy;
            std::uint32_t color = 0;
            for (unsigned int shift : {0u, 8u, 16u, 24u})
            {
                const float a = float((source.pixels[sy * source.width + sx] >> shift) & 255);
                const float b = float((source.pixels[sy * source.width + nx] >> shift) & 255);
                const float c = float((source.pixels[ny * source.width + sx] >> shift) & 255);
                const float d = float((source.pixels[ny * source.width + nx] >> shift) & 255);
                color |= std::uint32_t(std::clamp((a + (b - a) * fx) * (1 - fy) + (c + (d - c) * fx) * fy, 0.f, 255.f)) << shift;
            }
            blend(px, py, color, opacity);
        }
    }
};

class font
{
    static constexpr int atlas_width = 1024, atlas_height = 512;
    std::vector<unsigned char> m_atlas;
    std::array<stbtt_packedchar, 230> m_glyphs{};
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
        const std::array<unsigned int, 6> extra{0x2122, 0x2013, 0x2014, 0x2026, 0x25b3, 0x25cb};
        const auto found = std::find(extra.begin(), extra.end(), c);
        return found == extra.end() ? '?' - 32 : int(224 + found - extra.begin());
    }
public:
    bool load(std::span<const std::uint8_t> bytes)
    {
        if (bytes.size() < 256) return false;
        stbtt_fontinfo info{};
        const int offset = stbtt_GetFontOffsetForIndex(bytes.data(), 0);
        if (offset < 0 || !stbtt_InitFont(&info, bytes.data(), offset)) return false;
        m_atlas.resize(atlas_width * atlas_height);
        stbtt_pack_context ctx{};
        if (!stbtt_PackBegin(&ctx, m_atlas.data(), atlas_width, atlas_height, 0, 1, nullptr)) return false;
        std::array<int, 230> codepoints{};
        for (int i = 0; i < 224; ++i) codepoints[i] = i + 32;
        for (int i = 0; const int extra : {0x2122, 0x2013, 0x2014, 0x2026, 0x25b3, 0x25cb}) codepoints[224 + i++] = extra;
        stbtt_pack_range range{28.f, 0, codepoints.data(), 230, m_glyphs.data(), 0, 0};
        const bool ok = stbtt_PackFontRanges(&ctx, bytes.data(), 0, &range, 1) != 0;
        stbtt_PackEnd(&ctx);
        if (!ok) m_atlas.clear();
        return ok;
    }
    void paint(canvas& target, float x, float y, std::string_view value, float size = 28.f, std::uint32_t color = 0xffeeeeee) const
    {
        if (m_atlas.empty() || size <= 0) return;
        const float scale = size / 28.f;
        for (std::size_t i = 0; i < value.size();)
        {
            const auto c = decode(value, i);
            const auto& g = m_glyphs[glyph(c)];
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
};

class scene
{
    static constexpr int width = 1280, height = 720;
    std::vector<std::uint32_t> m_background;
    float m_category = 6.f, m_row = 0.f;
public:
    static constexpr std::array<const char*, 10> categories{"Usuários", "Configurações", "Foto", "Música", "Vídeo", "TV/Serviços de vídeo", "Jogo", "Rede", "PlayStation Network", "Amigos"};
    static constexpr std::array<const char*, 10> icons{"tex_user", "tex_sysconf", "tex_photo", "tex_music", "tex_video", "tex_tv", "tex_game", "tex_network", "tex_psn", "tex_friend"};
    std::map<std::string, image, std::less<>> resources;
    scene() : m_background(width * height)
    {
        for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x)
        {
            const float v = float(y) / height, glow = 1.f - std::min(1.f, std::hypot((x - 1000.f) / 1400.f, (y - 20.f) / 1100.f));
            const unsigned int r = unsigned(65 + 25 * v + 33 * glow), g = unsigned(68 - 14 * v + 41 * glow), b = unsigned(105 - 1 * v + 45 * glow);
            m_background[y * width + x] = 0xff000000 | (r << 16) | (g << 8) | b;
        }
    }
    void background(std::uint32_t* pixels, double seconds) const
    {
        std::copy(m_background.begin(), m_background.end(), pixels);
        canvas c(pixels, width, height);
        for (int ribbon = 0; ribbon < 5; ++ribbon) for (int x = 0; x < width; ++x)
        {
            const float u = float(x) / width;
            const float middle = 486 - 103 * std::sin(u * 3.141593f) + 18 * std::sin(u * 4.1f + float(seconds) * .17f + ribbon * .18f) + 12 * std::sin(u * 6.3f - float(seconds) * .11f + ribbon * .36f) + ribbon * 9;
            for (int d = -38; d <= 38; ++d)
            {
                const float amount = 1.f - std::abs(d) / 39.f;
                c.blend(x, int(middle) + d, 0xffd4cef8, unsigned(amount * amount * (ribbon == 2 ? 27 : 19)));
            }
            c.blend(x, int(middle) - 5, 0xffe7e5ff, 28);
        }
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
    void draw(std::uint32_t* pixels, const font& typeface, int category, std::size_t selected,
        std::span<const entry> entries, double seconds, float elapsed, std::string_view clock, std::string_view message, bool settings = false)
    {
        const float ease = 1.f - std::exp(-std::clamp(elapsed, 0.f, .1f) / .065f);
        m_category += (category - m_category) * ease;
        m_row += (float(selected) - m_row) * ease;
        background(pixels, seconds);
        canvas c(pixels, width, height);
        c.rectangle(900, 50, 380, 39, 0x167d90ac);
        c.rectangle(900, 50, 380, 1, 0x54dae0f2); c.rectangle(900, 88, 380, 1, 0x30dae0f2);
        c.rectangle(900, 50, 1, 39, 0x54dae0f2);
        typeface.paint(c, 1050, 77, clock, 23);
        for (int i = 0; i < int(categories.size()); ++i)
        {
            const float x = 375 + (i - m_category) * 136;
            const float focus = std::max(0.f, 1.f - std::abs(i - m_category));
            const float size = 68 + 46 * focus;
            if (const auto found = resources.find(icons[i]); found != resources.end()) c.blit(found->second, x - size / 2, 190 - size / 2, size, size, unsigned(140 + 115 * focus));
            else { c.rectangle(int(x) - 19, 174, 38, 30, 0x65e1e0f3); }
        }
        typeface.paint(c, 352, 238, categories[category], 21);
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
            if (icon)
            {
                const float w = size, h = size * icon->height / icon->width;
                c.blit(*icon, column - w / 2, y - h / 2, w, h, opacity);
            }
            else c.rectangle(int(column) - int(size / 2), int(y) - int(size / 3), int(size), int(size * 2 / 3), (opacity << 24) | 0xd2d1e8);
            if (i == selected)
            {
                const auto alpha = std::uint32_t(255 * focus) << 24;
                typeface.paint(c, column + 75, y + 8, entries[i].title, 27, alpha | 0xfbfbff);
                typeface.paint(c, column + 75, y + 33, entries[i].subtitle, 17, alpha | 0xdfdce9);
            }
        }
        if (!message.empty()) typeface.paint(c, 60, 632, message, 20, 0xffffd8b3);
        const std::array<const char*, 3> buttons{"tex_cross", "tex_circle", settings ? "tex_square" : "tex_triangle"};
        const std::array<const char*, 3> hints{"Selecionar", "Voltar", settings ? "Restaurar" : "Atualizar"};
        for (int i = 0; i < 3; ++i)
        {
            const int x = 60 + i * 150;
            if (const auto found = resources.find(buttons[i]); found != resources.end()) c.blit(found->second, float(x), 668, 17, 17, 220);
            else typeface.paint(c, float(x), 682, i == 0 ? "X" : i == 1 ? "O" : settings ? "[]" : "T", 18, 0xffd5d2e5);
            typeface.paint(c, float(x + 26), 682, hints[i], 18, 0xffd5d2e5);
        }
        typeface.paint(c, 520, 682, settings ? "Esq/Dir alterar · R1 acelerar · Círculo voltar" : "Quadrado: configurar jogo · Botão PS: fechar", 18, 0xffd5d2e5);
    }
};
} // namespace legacy

std::vector<std::uint8_t> read(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), {}};
}
bool inflate_data(std::span<std::uint8_t> out, std::span<const std::uint8_t> in)
{
    uLongf count = out.size();
    return uncompress(out.data(), &count, in.data(), in.size()) == Z_OK && count == out.size();
}
bool write_png(const std::filesystem::path& path, const std::uint32_t* pixels)
{
    std::vector<unsigned char> rgb(1280 * 720 * 3);
    for (int i = 0; i < 1280 * 720; ++i) for (int channel = 0; channel < 3; ++channel)
        rgb[i * 3 + channel] = (pixels[i] >> (16 - channel * 8)) & 255;
    return stbi_write_png(path.string().c_str(), 1280, 720, 3, rgb.data(), 1280 * 3) != 0;
}
// A part of a frame enlarged 'scale' times (nearest pixel), for a close look at small details.
bool write_crop_png(const std::filesystem::path& path, const std::uint32_t* pixels, int x, int y, int w, int h, int scale)
{
    std::vector<unsigned char> rgb(std::size_t(w) * scale * h * scale * 3);
    for (int py = 0; py < h * scale; ++py) for (int px = 0; px < w * scale; ++px) for (int channel = 0; channel < 3; ++channel)
        rgb[(std::size_t(py) * w * scale + px) * 3 + channel] = (pixels[(y + py / scale) * 1280 + x + px / scale] >> (16 - channel * 8)) & 255;
    return stbi_write_png(path.string().c_str(), w * scale, h * scale, 3, rgb.data(), w * scale * 3) != 0;
}

// Largest difference of any channel between two images.
int max_diff(const std::uint32_t* a, const std::uint32_t* b, std::size_t count)
{
    int worst = 0;
    for (std::size_t i = 0; i < count; ++i)
        if (a[i] != b[i])
            for (unsigned int shift : {0u, 8u, 16u, 24u})
                worst = std::max(worst, std::abs(int((a[i] >> shift) & 255) - int((b[i] >> shift) & 255)));
    return worst;
}

struct random_bits
{
    std::uint32_t state = 0x9e3779b9u;
    std::uint32_t operator()() { state ^= state << 13; state ^= state >> 17; state ^= state << 5; return state; }
    int range(int lo, int hi) { return lo + int((*this)() % unsigned(hi - lo + 1)); }
    float real(float lo, float hi) { return lo + (hi - lo) * float((*this)() >> 8) / float(1 << 24); }
};

// blend/rectangle/blit against the float reference, each on a fresh copy of a random canvas.
int primitive_diff()
{
    constexpr int w = 97, h = 61;
    random_bits rng;
    std::vector<std::uint32_t> base(w * h), fast, slow;
    int worst = 0;
    for (int round = 0; round < 6000; ++round)
    {
        if (round % 40 == 0) for (auto& p : base) p = rng() | 0xff000000u;
        fast = base; slow = base;
        canvas a(fast.data(), w, h);
        legacy::canvas b(slow.data(), w, h);
        switch (round % 4)
        {
        case 0:
            for (int i = 0; i < 24; ++i)
            {
                const int x = rng.range(-3, w + 2), y = rng.range(-3, h + 2);
                const std::uint32_t color = rng();
                const unsigned int opacity = unsigned(rng.range(0, 300));
                a.blend(x, y, color, opacity); b.blend(x, y, color, opacity);
            }
            break;
        case 1:
        {
            const int x = rng.range(-20, w), y = rng.range(-20, h), rw = rng.range(-5, 80), rh = rng.range(-5, 60);
            const std::uint32_t color = rng();
            a.rectangle(x, y, rw, rh, color); b.rectangle(x, y, rw, rh, color);
            break;
        }
        default:
        {
            image source{rng.range(1, 40), rng.range(1, 40), {}};
            source.pixels.resize(std::size_t(source.width) * source.height);
            const int style = rng.range(0, 3);
            for (auto& p : source.pixels)
            {
                const std::uint32_t bits = rng();
                p = style == 0 ? bits : style == 1 ? (bits & 0xffffffu) | (rng.range(0, 2) ? 0xff000000u : 0u)
                    : style == 2 ? (rng.range(0, 3) ? bits & 0xffffffu : bits) : bits | 0xff000000u;
            }
            float x = rng.real(-30.f, w + 5.f), y = rng.real(-30.f, h + 5.f), bw = rng.real(.5f, 120.f), bh = rng.real(.5f, 120.f);
            if (round % 8 == 3) { x = std::floor(x); y = std::floor(y); bw = float(source.width); bh = float(source.height); }
            const unsigned int opacity = unsigned(rng.range(0, 300));
            a.blit(source, x, y, bw, bh, opacity); b.blit(source, x, y, bw, bh, opacity);
            break;
        }
        }
        worst = std::max(worst, max_diff(fast.data(), slow.data(), fast.size()));
    }
    return worst;
}

struct ink
{
    long long sum = 0;
    int left = INT_MAX, top = INT_MAX, right = INT_MIN, bottom = INT_MIN;
};
template <class Canvas, class Font> ink measure(const Font& typeface, std::string_view text, float size)
{
    std::vector<std::uint32_t> pixels(1280 * 96, 0xff000000u);
    Canvas target(pixels.data(), 1280, 96);
    typeface.paint(target, 20.3f, 60.6f, text, size, 0xffffffffu);
    ink result;
    for (int y = 0; y < 96; ++y) for (int x = 0; x < 1280; ++x)
        if (const int value = pixels[y * 1280 + x] & 255)
        {
            result.sum += value;
            result.left = std::min(result.left, x); result.right = std::max(result.right, x);
            result.top = std::min(result.top, y); result.bottom = std::max(result.bottom, y);
        }
    return result;
}

// The cheat screens (R2 on a game, the first row of its patch list): rows, options, dialogs and the header line as
// ps4_launcher.cpp builds them (cheat_entries, cheat_option_entries, cheat_lines_dialog, cheat_header, the delete
// question, the help of an empty list, the game row hint), over a model the real parser builds from sample files
namespace cheat_screens
{
namespace cheats = utils::ps4::cheats;
namespace i18n = utils::ps4::i18n;
using i18n::str;

inline constexpr char switch_on[] = "@switch_on", switch_off[] = "@switch_off";

// patch_switch_icon of the launcher (green, knob right = on; grey, knob left = off), kept under the names above
inline image switch_icon(bool on)
{
    constexpr int w = 96, h = 56;
    image out{w, h, std::vector<std::uint32_t>(std::size_t(w) * h, 0)};
    const std::uint32_t track = on ? 0x34c759 : 0x8e8e93;
    const float radius = h / 2.f - 2, cy = h / 2.f, left = radius + 2, right = w - radius - 2;
    const float knob = on ? right : left;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            const float px = x + .5f, py = y + .5f;
            const float edge = std::hypot(px - std::clamp(px, left, right), py - cy) - radius;
            const float cover = std::clamp(.5f - edge, 0.f, 1.f);
            if (cover <= 0.f) continue;
            const float white = std::clamp(.5f - (std::hypot(px - knob, py - cy) - (radius - 4)), 0.f, 1.f);
            const auto channel = [&](int shift)
            {
                const float base = float((track >> shift) & 0xff);
                return std::uint32_t(base + (255.f - base) * white + .5f) << shift;
            };
            out.pixels[std::size_t(y) * w + x] = (std::uint32_t(cover * 255.f + .5f) << 24) | channel(16) | channel(8) | channel(0);
        }
    return out;
}

inline std::size_t line_bytes(std::size_t latin) { return i18n::rtl() ? 2 * latin : latin; }

// patches::flatten and patches::clip of the launcher
inline std::string flatten(std::string_view text)
{
    std::string out;
    bool space = false;
    for (const char c : text)
    {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n')
        {
            space = !out.empty();
            continue;
        }
        if (space) out += ' ';
        space = false;
        out += c;
    }
    return out;
}

inline std::string clip(std::string text, std::size_t limit)
{
    if (text.size() <= limit) return text;
    std::size_t cut = limit > 3 ? limit - 3 : 0;
    while (cut && (static_cast<unsigned char>(text[cut]) & 0xc0) == 0x80) --cut;
    text.resize(cut);
    text += "...";
    return text;
}

inline std::pair<std::string, std::string> first_part(std::string_view text)
{
    constexpr std::string_view separator = " \xC2\xB7 ";
    const std::size_t cut = text.find(separator);
    if (cut == std::string_view::npos) return {std::string(text), {}};
    return {std::string(text.substr(0, cut)), std::string(text.substr(cut + separator.size()))};
}

inline std::string version_note(const cheats::row& row, const cheats::game_cheats& game)
{
    const std::string installed = cheats::detail::fold_version(game.app_version);
    if (row.version.empty() || installed.empty() || cheats::detail::fold_version(row.version) == installed) return {};
    return " \xC2\xB7 " + i18n::trf(str::CHT_VERSAO_DIFERENTE, {row.version, game.app_version});
}

inline std::vector<entry> rows(const cheats::game_cheats* game)
{
    std::vector<entry> entries;
    if (!game)
    {
        entries.push_back({{}, i18n::tr(str::CHT_LER), "", "item_tex_ps3util", {}});
        return entries;
    }
    for (std::size_t i = 0; i < game->rows.size(); ++i)
    {
        const cheats::row& row = game->rows[i];
        const char* icon = !row.valid ? "item_tex_ps3util" : row.on ? switch_on : switch_off;
        entries.push_back({"@cheat:" + std::to_string(i), clip(flatten(row.name), 64), clip(cheats::row_subtitle(row, *game) + version_note(row, *game), line_bytes(200)),
            icon, {}});
    }
    if (game->rows.empty())
    {
        const auto [title, folders] = first_part(i18n::tr(str::CHT_NENHUM));
        entries.push_back({"@cheats:none", title, folders, "item_tex_ps3util", {}});
    }
    entries.push_back({"@cheats:add", i18n::tr(str::CHT_ADICIONAR), i18n::tr(str::CHT_TECLADO_CODIGOS), "item_tex_ps3util", {}});
    entries.push_back({"@cheats:import", i18n::tr(str::CHT_IMPORTAR_JOGO), i18n::tr(str::CHT_IMPORTAR_HELP), "item_tex_disc_install", {}});
    return entries;
}

inline std::vector<entry> options(const cheats::game_cheats& game, std::size_t index)
{
    std::vector<entry> entries;
    if (index >= game.rows.size()) return entries;
    const cheats::row& row = game.rows[index];
    const auto add = [&](const char* path, std::string title, std::string help)
    {
        entries.push_back({path, std::move(title), clip(std::move(help), line_bytes(200)), "tex_sysconf", {}});
    };
    std::string modes;
    for (const cheats::mode m : {cheats::mode::automatic, cheats::mode::constant, cheats::mode::once, cheats::mode::boot})
        modes += (modes.empty() ? "" : " / ") + std::string(i18n::tr(cheats::mode_name(m)));
    add("@copt:mode", i18n::trf(str::CFGU_CHANGED, {str::CHT_OPC_MODO, cheats::mode_name(row.wanted)}), cheats::row_subtitle(row, game) + " \xC2\xB7 " + modes);
    if (row.wanted == cheats::mode::constant ||
        (row.wanted == cheats::mode::automatic && row.resolved != cheats::mode::boot && row.resolved != cheats::mode::once))
    {
        std::string periods;
        for (const std::uint32_t ms : cheats::periods_ms) periods += (periods.empty() ? "" : " / ") + std::to_string(ms);
        add("@copt:period", i18n::trf(str::CFGU_CHANGED, {str::CHT_OPC_INTERVALO, i18n::raw(i18n::trf(str::CHT_INTERVALO_MS, {row.period_ms}))}),
            i18n::trf(str::CHT_INTERVALO_MS, {periods}));
    }
    if (row.pc)
        add("@copt:value", row.value.empty() ? std::string(i18n::tr(str::CHT_OPC_VALOR)) : i18n::trf(str::CFGU_CHANGED, {str::CHT_OPC_VALOR, row.value}),
            i18n::trf(str::CHT_TECLADO_VALOR, {clip(flatten(row.name), 48), cheats::pc_type_names[std::size_t(row.pc_info.type)]}));
    add("@copt:lines", i18n::tr(str::CHT_OPC_VER_LINHAS), first_part(i18n::trf(str::CHT_N_LINHAS, {static_cast<unsigned>(row.lines.size())})).second);
    if (row.user)
    {
        add("@copt:edit", i18n::tr(str::CHT_OPC_EDITAR), i18n::tr(str::CHT_TECLADO_CODIGOS));
        add("@copt:rename", i18n::tr(str::CHT_OPC_RENOMEAR), i18n::tr(str::CHT_TECLADO_NOME));
        add("@copt:delete", i18n::tr(str::CHT_OPC_APAGAR), row.source);
    }
    else add("@copt:copy", i18n::tr(str::CHT_OPC_COPIAR), i18n::tr(str::CHT_ERR_SO_MEUS));
    return entries;
}

inline dialog lines_dialog(const cheats::game_cheats& game, std::size_t index, std::size_t page, std::size_t& pages)
{
    constexpr std::size_t per_page = 8;
    dialog d;
    pages = 1;
    if (index >= game.rows.size()) return d;
    const cheats::row& row = game.rows[index];
    const std::vector<std::string> details = cheats::row_details(row, game);
    pages = std::max<std::size_t>(1, (details.size() + per_page - 1) / per_page);
    page = std::min(page, pages - 1);
    d.title = clip(flatten(row.name), 64);
    if (pages > 1) d.title += " (" + std::to_string(page + 1) + "/" + std::to_string(pages) + ")";
    for (std::size_t i = page * per_page; i < details.size() && i < (page + 1) * per_page; ++i)
    {
        if (i < row.lines.size()) d.lines.push_back(clip(details[i], 72));
        else d.notes.push_back(clip(flatten(details[i]), line_bytes(240)));
    }
    if (page + 1 < pages) d.buttons = {cross_button(i18n::tr(str::COM_CONTINUAR)), circle_button(i18n::tr(str::COM_VOLTAR))};
    else d.buttons = {cross_button(i18n::tr(str::COM_OK))};
    return d;
}

inline std::string header(const cheats::game_cheats& game, bool rpcn_online)
{
    std::vector<std::string> lines = cheats::header_lines(game, rpcn_online);
    if (const auto online = std::find(lines.begin(), lines.end(), i18n::tr(str::CHT_AVISO_ONLINE)); online != lines.end())
        std::rotate(lines.begin(), online, online + 1);
    if (lines.empty())
        lines.push_back(game.serial + " " + (game.app_version.empty() ? std::string(i18n::tr(str::COM_VERSAO_DESCONHECIDA)) : "v" + game.app_version));
    for (const cheats::problem& note : game.notes)
        if (note.id == str::CHT_ERR_ARQUIVO_GRANDE || note.id == str::CHT_ERRO_LER || note.id == str::CHT_ERR_MUITOS)
        {
            lines.push_back(cheats::problem_text(note));
            break;
        }
    std::string text = clip(i18n::trf(str::CHT_LISTA, {game.title.empty() ? game.serial : game.title}), line_bytes(60));
    for (const std::string& line : lines)
        if (text.size() + line.size() + 4 <= line_bytes(110)) text += " \xC2\xB7 " + line;
    return text;
}

// The first row of the patch list (patch_entries)
inline entry patch_list_row(int cheats_on)
{
    const auto [title, count] = first_part(i18n::trf(str::CHT_R1_LINHA, {cheats_on}));
    return {"@cheats:open", title, count + i18n::tr(str::CHT_DICA_JOGO), "item_tex_ps3util", {}};
}

// The question of Square > Apagar
inline dialog delete_question(const cheats::row& row)
{
    const std::string name = flatten(row.name);
    return {clip(name, 64), {i18n::trf(str::CHT_APAGAR_CONFIRMA, {clip(name, 64)})}, {row.source}, -1, {}, {}, false, nullptr,
        {cross_button(i18n::tr(str::COM_APAGAR)), circle_button(i18n::tr(str::COM_CANCELAR))}};
}

// X on the row of an empty list: show_notice of the folders, the names and the Action Replay note
inline dialog empty_help(const std::string& title)
{
    dialog d;
    d.title = i18n::trf(str::CHT_LISTA, {title});
    // split_into_dialog: the first part is the message, the others its notes (a sentence each)
    std::string_view text = i18n::tr(str::CHT_NENHUM);
    for (bool first = true; !text.empty(); first = false)
    {
        const std::size_t cut = text.find(" \xC2\xB7 ");
        std::string part(text.substr(0, cut));
        text = cut == std::string_view::npos ? std::string_view{} : text.substr(cut + 4);
        if (part.empty()) continue;
        if (!first && part.front() >= 'a' && part.front() <= 'z') part.front() = static_cast<char>(part.front() - 'a' + 'A');
        (first ? d.lines : d.notes).push_back(std::move(part));
    }
    d.notes.emplace_back(i18n::tr(str::CHT_NENHUM_HELP));
    d.notes.emplace_back(i18n::tr(str::CHT_ACTION_REPLAY));
    d.buttons = {cross_button(i18n::tr(str::COM_OK))};
    return d;
}

inline std::string game_hint() { return std::string(i18n::tr(str::LAU_HINT_GAME_ROW)) + i18n::tr(str::CHT_DICA_JOGO); }

// Sample files of one game: the console's own cheat, an imported list (codes for 01.00 with a code cheat, a pointer
// and an unknown code type) and a PC cheats.yml entry; the executable as the engine recorded it at the last boot
inline cheats::game_cheats sample_game(bool with_cheats)
{
    std::vector<cheats::source_file> files;
    if (with_cheats)
    {
        files.push_back({"BLUS30443_user.txt", "; RPCS3 PS4 cheats\n; serial: BLUS30443\n[Max money]\n; author: Rob\n0 01234560 05F5E0FF\n", true, false});
        files.push_back({"BLUS30443.txt", "; serial: BLUS30443\n; version: 01.00\n[Infinite health]\n0 0031A2B0 60000000\n[Pointer HP]\n6 01000000 00000010\n"
            "0 00000004 000003E7\n[Weird code]\n7 01234560 00000001\n", false, false});
        files.push_back({"BLUS30443.yml", "Demon's Souls:\n  0x1A2B3C4: [HP, Signed 32 bits, \"[$]+16\"]\n", false, true});
    }
    const std::string seen = "PPU-0123456789abcdef0123456789abcdef01234567 01.04 exec=00010000-0034ffff data=00350000-01ffffff\n";
    cheats::game_cheats g = cheats::build_game("BLUS30443", "Demon's Souls", "01.04", files, {}, seen);
    // Max money on (as a state line would say)
    cheats::edit_files written;
    std::string message;
    for (std::size_t i = 0; i < g.rows.size(); ++i)
        if (g.rows[i].name == "Max money") cheats::edit_on(g, i, true, message, written);
    return g;
}

inline std::size_t row_named(const cheats::game_cheats& g, std::string_view name)
{
    for (std::size_t i = 0; i < g.rows.size(); ++i)
        if (g.rows[i].name == name) return i;
    return 0;
}
} // namespace cheat_screens

// 0.40: synthetic stand-ins for a game's own PIC1 and PIC0, made here from gradients and shapes (no game art is used or
// stored anywhere). The launcher decodes the selected game's files and scales them to these sizes: PIC1 to 1280x720,
// PIC0 by 720/1080 (1000x560 becomes 667x373, right-aligned and vertically centred).
namespace media_samples
{
inline std::uint32_t pack(float r, float g, float b)
{
    const auto c = [](float v) { return std::uint32_t(std::clamp(v, 0.f, 255.f) + .5f); };
    return 0xff000000u | (c(r) << 16) | (c(g) << 8) | c(b);
}

// 1280x720, opaque. 0: an evening sky over two ridges with a bright sun behind the selected row's title (the hard case
// for legibility); 1: a night sky with a moon, stars and mountains.
inline image background(int variant)
{
    image out{1280, 720, std::vector<std::uint32_t>(std::size_t(1280) * 720)};
    const bool warm = variant == 0;
    for (int y = 0; y < 720; ++y)
        for (int x = 0; x < 1280; ++x)
        {
            const float u = float(x) / 1280, v = float(y) / 720;
            float r = warm ? 70 + 180 * v : 10 + 40 * v, g = warm ? 60 + 120 * v : 20 + 70 * v, b = warm ? 140 - 40 * v : 50 + 90 * v;
            const float sx = warm ? 760.f : 990.f, sy = warm ? 330.f : 150.f, sr = warm ? 80.f : 40.f, glow = warm ? 300.f : 140.f;
            const float d = std::hypot(float(x) - sx, float(y) - sy);
            const float halo = std::clamp(1 - (d - sr) / glow, 0.f, 1.f) * .6f, body = std::clamp(sr + .5f - d, 0.f, 1.f);
            const float light = body + (1 - body) * halo * halo;
            r += ((warm ? 255.f : 230.f) - r) * light;
            g += ((warm ? 236.f : 236.f) - g) * light;
            b += ((warm ? 190.f : 250.f) - b) * light;
            if (!warm && ((x * 7919 + y * 104729) % 977) == 0 && y < 420) r = g = b = 235; // stars
            const float ridge1 = warm ? 520 + 40 * std::sin(u * 7.1f) + 25 * std::sin(u * 17.3f) : 430 + 160 * std::abs(std::fmod(u * 5.f, 2.f) - 1.f);
            const float ridge2 = warm ? 600 + 30 * std::sin(u * 4.3f + 1.7f) + 12 * std::sin(u * 23.f) : 560 + 70 * std::abs(std::fmod(u * 9.f + .3f, 2.f) - 1.f);
            if (float(y) > ridge1) { r = warm ? 96 : 22; g = warm ? 52 : 36; b = warm ? 84 : 62; }
            if (float(y) > ridge2) { r = warm ? 46 : 10; g = warm ? 26 : 18; b = warm ? 52 : 34; }
            out.pixels[std::size_t(y) * 1280 + std::size_t(x)] = pack(r, g, b);
        }
    return out;
}

// 667x373, straight alpha: a translucent rounded panel with a light frame and an emblem made of a disc and bars,
// transparent around it
inline image overlay()
{
    constexpr int w = 667, h = 373;
    image out{w, h, std::vector<std::uint32_t>(std::size_t(w) * h, 0x00d8d8e4u)};
    const auto round_rect = [](float x, float y, float l, float t, float r, float b, float radius)
    {
        const float cx = std::clamp(x, l + radius, r - radius), cy = std::clamp(y, t + radius, b - radius);
        return std::clamp(radius + .5f - std::hypot(x - cx, y - cy), 0.f, 1.f) * float(x >= l - 1 && x <= r + 1 && y >= t - 1 && y <= b + 1);
    };
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
        {
            const float px = float(x) + .5f, py = float(y) + .5f;
            const float panel = round_rect(px, py, 24, 64, 643, 309, 18), inner = round_rect(px, py, 27, 67, 640, 306, 15);
            float a = panel * .62f, r = 16, g = 22, b = 48;
            const float frame = std::clamp(panel - inner, 0.f, 1.f);
            if (frame > 0) { r += (240 - r) * frame; g += (236 - g) * frame; b += (250 - b) * frame; a += (.86f - a) * frame; }
            const float emblem = std::clamp(62.5f - std::hypot(px - 140, py - 186), 0.f, 1.f), hole = std::clamp(40.5f - std::hypot(px - 140, py - 186), 0.f, 1.f);
            const float ring = std::clamp(emblem - hole, 0.f, 1.f);
            if (ring > 0) { r += (250 - r) * ring; g += (206 - g) * ring; b += (92 - b) * ring; a += (.95f - a) * ring; }
            for (const auto& [top, length] : {std::pair{140.f, 380.f}, std::pair{182.f, 300.f}, std::pair{224.f, 340.f}})
                if (const float bar = round_rect(px, py, 236, top, 236 + length, top + 22, 6); bar > 0)
                {
                    r += (246 - r) * bar; g += (244 - g) * bar; b += (252 - b) * bar; a += (.92f - a) * bar;
                }
            if (a <= 0) continue;
            const auto c = [](float v) { return std::uint32_t(std::clamp(v, 0.f, 255.f) + .5f); };
            out.pixels[std::size_t(y) * w + std::size_t(x)] = (c(a * 255) << 24) | (c(r) << 16) | (c(g) << 8) | c(b);
        }
    return out;
}
} // namespace media_samples

// SHA-256 as lowercase hex (the firmware folder texture check compares the decoded ARGB with a known hash)
inline std::string sha256_hex(std::span<const std::uint8_t> data)
{
    static constexpr std::array<std::uint32_t, 64> k{0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
        0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
        0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    std::array<std::uint32_t, 8> h{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::vector<std::uint8_t> m(data.begin(), data.end());
    const std::uint64_t bits = std::uint64_t(data.size()) * 8;
    m.push_back(0x80);
    while (m.size() % 64 != 56) m.push_back(0);
    for (int i = 7; i >= 0; --i) m.push_back(std::uint8_t(bits >> (i * 8)));
    const auto rotr = [](std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
    for (std::size_t chunk = 0; chunk < m.size(); chunk += 64)
    {
        std::array<std::uint32_t, 64> w{};
        for (std::size_t i = 0; i < 16; ++i)
            w[i] = (std::uint32_t(m[chunk + i * 4]) << 24) | (std::uint32_t(m[chunk + i * 4 + 1]) << 16) | (std::uint32_t(m[chunk + i * 4 + 2]) << 8) | m[chunk + i * 4 + 3];
        for (std::size_t i = 16; i < 64; ++i)
        {
            const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3), s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::array<std::uint32_t, 8> v = h;
        for (std::size_t i = 0; i < 64; ++i)
        {
            const std::uint32_t t1 = v[7] + (rotr(v[4], 6) ^ rotr(v[4], 11) ^ rotr(v[4], 25)) + ((v[4] & v[5]) ^ (~v[4] & v[6])) + k[i] + w[i];
            const std::uint32_t t2 = (rotr(v[0], 2) ^ rotr(v[0], 13) ^ rotr(v[0], 22)) + ((v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]));
            v = {t1 + t2, v[0], v[1], v[2], v[3] + t1, v[4], v[5], v[6]};
        }
        for (std::size_t i = 0; i < 8; ++i) h[i] += v[i];
    }
    std::string hex;
    for (const std::uint32_t word : h)
        for (int shift = 28; shift >= 0; shift -= 4) hex += "0123456789abcdef"[(word >> shift) & 15];
    return hex;
}

// 0.39: the interface languages. For pt-BR, English, Spanish and Arabic: 9 screens (game column with a locked game, a long
// message line and the footer; the Configurações column with the language row; the settings top rows with 'Idioma dos
// jogos'; disc install question; copy progress; firmware question; out-of-memory notice; boot-next countdown; emulator
// error), then 14 screens of the settings pages and 10 cheat screens (see there), written as preview_<lang>_<screen>.png. Texts come from the
// i18n tables; the launcher's own lines (its table is translated with its i18n conversion) are sample texts here. Arabic is
// checked by pixels: message line ending at x 1220, footer and dialog buttons mirrored, percentage caption on the right,
// a page's labels ending at x 1110, no '?' glyph in any drawn line.
namespace languages
{
namespace i18n = utils::ps4::i18n;
using i18n::str;

struct samples
{
    const char* disc_question;
    const char* disc_size;
    const char* disc_free;
    const char* copying;
    const char* copied;
    const char* fw_question;
    const char* fw_file;
    const char* fw_now;
    const char* oom_line;
    const char* boot_next;
    const char* boot_next_note;
    const char* close_hint;
    const char* rpcs3_settings;
    const char* rpcs3_settings_help;
    const char* storage_help;
};

// Launcher lines (test data in each language; the launcher's own table translates them in the build)
inline const samples& sample(i18n::lang l)
{
    static const samples pt{"Deseja instalar este disco no PS4?", "Tamanho: 10.6 GB (cerca de 12 min). O disco nunca é alterado.",
        "Espaço livre no PS4: 412.3 GB", "Copiando o disco...", "4.2 GB de 10.6 GB · 15.0 MB/s", "Deseja instalar o firmware 4.93?",
        "Arquivo: USB 1:/PS3/UPDATE/PS3UPDAT.PUP (209 MB)", "Instalado agora: nenhum", "O jogo foi fechado para proteger o console.",
        "Iniciando FIFA 14 (BLES01876) em 5 s", "Círculo cancela", "Para jogar de novo, feche o aplicativo pelo botão PS e abra-o outra vez.",
        "Configurações do RPCS3", "CPU, GPU, áudio e sistema · X abre", "/data/rpcs3/games e o pendrive"};
    static const samples en{"Install this disc on the PS4?", "Size: 10.6 GB (about 12 min). The disc is never changed.",
        "Free space on the PS4: 412.3 GB", "Copying the disc...", "4.2 GB of 10.6 GB · 15.0 MB/s", "Install firmware 4.93?",
        "File: USB 1:/PS3/UPDATE/PS3UPDAT.PUP (209 MB)", "Installed now: none", "The game was closed to protect the console.",
        "Starting FIFA 14 (BLES01876) in 5 s", "Circle cancels", "To play again, close the application with the PS button and open it again.",
        "RPCS3 settings", "CPU, GPU, audio and system · X opens", "/data/rpcs3/games and the USB drive"};
    static const samples es{"¿Quieres instalar este disco en el PS4?", "Tamaño: 10.6 GB (unos 12 min). El disco nunca se modifica.",
        "Espacio libre en el PS4: 412.3 GB", "Copiando el disco...", "4.2 GB de 10.6 GB · 15.0 MB/s", "¿Quieres instalar el firmware 4.93?",
        "Archivo: USB 1:/PS3/UPDATE/PS3UPDAT.PUP (209 MB)", "Instalado ahora: ninguno", "El juego se cerró para proteger la consola.",
        "Iniciando FIFA 14 (BLES01876) en 5 s", "Círculo cancela", "Para volver a jugar, cierra la aplicación con el botón PS y ábrela de nuevo.",
        "Ajustes de RPCS3", "CPU, GPU, audio y sistema · X abre", "/data/rpcs3/games y la memoria USB"};
    // Arabic as trf() builds it: Latin and number arguments isolated with U+200E
#define LRM "\xE2\x80\x8E"
    static const samples ar{"\xD9\x87\xD9\x84 \xD8\xAA\xD8\xB1\xD9\x8A\xD8\xAF \xD8\xAA\xD8\xAB\xD8\xA8\xD9\x8A\xD8\xAA \xD9\x87\xD8\xB0\xD8\xA7 \xD8\xA7\xD9\x84\xD9\x82\xD8\xB1\xD8\xB5 \xD8\xB9\xD9\x84\xD9\x89 PS4\xD8\x9F",
        "\xD8\xA7\xD9\x84\xD8\xAD\xD8\xAC\xD9\x85: " LRM "10.6 GB" LRM " (\xD8\xAD\xD9\x88\xD8\xA7\xD9\x84\xD9\x8A " LRM "12" LRM " \xD8\xAF\xD9\x82\xD9\x8A\xD9\x82\xD8\xA9). \xD9\x84\xD8\xA7 \xD9\x8A\xD8\xAA\xD8\xBA\xD9\x8A\xD8\xB1 \xD8\xA7\xD9\x84\xD9\x82\xD8\xB1\xD8\xB5 \xD8\xA3\xD8\xA8\xD8\xAF\xD8\xA7.",
        "\xD8\xA7\xD9\x84\xD9\x85\xD8\xB3\xD8\xA7\xD8\xAD\xD8\xA9 \xD8\xA7\xD9\x84\xD8\xAD\xD8\xB1\xD8\xA9 \xD9\x81\xD9\x8A PS4: " LRM "412.3 GB" LRM,
        "\xD8\xAC\xD8\xA7\xD8\xB1 \xD9\x86\xD8\xB3\xD8\xAE \xD8\xA7\xD9\x84\xD9\x82\xD8\xB1\xD8\xB5...",
        LRM "4.2 GB" LRM " \xD9\x85\xD9\x86 " LRM "10.6 GB" LRM " \xC2\xB7 " LRM "15.0 MB/s" LRM,
        "\xD9\x87\xD9\x84 \xD8\xAA\xD8\xB1\xD9\x8A\xD8\xAF \xD8\xAA\xD8\xAB\xD8\xA8\xD9\x8A\xD8\xAA \xD8\xA7\xD9\x84\xD8\xA8\xD8\xB1\xD9\x86\xD8\xA7\xD9\x85\xD8\xAC \xD8\xA7\xD9\x84\xD8\xAB\xD8\xA7\xD8\xA8\xD8\xAA " LRM "4.93" LRM "\xD8\x9F",
        "\xD8\xA7\xD9\x84\xD9\x85\xD9\x84\xD9\x81: " LRM "USB 1:/PS3/UPDATE/PS3UPDAT.PUP" LRM " (" LRM "209 MB" LRM ")",
        "\xD8\xA7\xD9\x84\xD9\x85\xD8\xAB\xD8\xA8\xD8\xAA \xD8\xA7\xD9\x84\xD8\xA2\xD9\x86: \xD9\x84\xD8\xA7 \xD8\xB4\xD9\x8A\xD8\xA1",
        "\xD8\xAA\xD9\x85 \xD8\xA5\xD8\xBA\xD9\x84\xD8\xA7\xD9\x82 \xD8\xA7\xD9\x84\xD9\x84\xD8\xB9\xD8\xA8\xD8\xA9 \xD9\x84\xD8\xAD\xD9\x85\xD8\xA7\xD9\x8A\xD8\xA9 \xD8\xA7\xD9\x84\xD8\xAC\xD9\x87\xD8\xA7\xD8\xB2.",
        "\xD8\xAC\xD8\xA7\xD8\xB1 \xD8\xAA\xD8\xB4\xD8\xBA\xD9\x8A\xD9\x84 " LRM "FIFA 14 (BLES01876)" LRM " \xD8\xAE\xD9\x84\xD8\xA7\xD9\x84 " LRM "5" LRM " \xD8\xAB\xD9\x88\xD8\xA7\xD9\x86",
        "\xD8\xA7\xD9\x84\xD8\xAF\xD8\xA7\xD8\xA6\xD8\xB1\xD8\xA9 \xD9\x84\xD9\x84\xD8\xA5\xD9\x84\xD8\xBA\xD8\xA7\xD8\xA1",
        "\xD9\x84\xD9\x84\xD8\xB9\xD8\xA8 \xD9\x85\xD8\xAC\xD8\xAF\xD8\xAF\xD8\xA7\xD8\x8C \xD8\xA3\xD8\xBA\xD9\x84\xD9\x82 \xD8\xA7\xD9\x84\xD8\xAA\xD8\xB7\xD8\xA8\xD9\x8A\xD9\x82 \xD8\xA8\xD8\xB2\xD8\xB1 PS \xD8\xAB\xD9\x85 \xD8\xA7\xD9\x81\xD8\xAA\xD8\xAD\xD9\x87 \xD9\x85\xD8\xB1\xD8\xA9 \xD8\xA3\xD8\xAE\xD8\xB1\xD9\x89.",
        "\xD8\xA5\xD8\xB9\xD8\xAF\xD8\xA7\xD8\xAF\xD8\xA7\xD8\xAA RPCS3",
        "CPU\xD8\x8C GPU\xD8\x8C \xD8\xA7\xD9\x84\xD8\xB5\xD9\x88\xD8\xAA \xD9\x88\xD8\xA7\xD9\x84\xD9\x86\xD8\xB8\xD8\xA7\xD9\x85 \xC2\xB7 X \xD9\x84\xD9\x84\xD9\x81\xD8\xAA\xD8\xAD",
        LRM "/data/rpcs3/games" LRM " \xD9\x88\xD8\xB0\xD8\xA7\xD9\x83\xD8\xB1\xD8\xA9 USB"};
#undef LRM
    switch (l)
    {
    case i18n::lang::en: return en;
    case i18n::lang::es: return es;
    case i18n::lang::ar: return ar;
    case i18n::lang::pt_br: break;
    }
    return pt;
}

// Mean x of the strongly red (colour 0xff0000) or green (0x00ff00) pixels in rows [y0, y1); -1 when there is none.
// The stand-in glyphs are drawn translucent, so the test is by dominance, not by equality.
inline int colour_x(const std::uint32_t* pixels, std::uint32_t colour, int y0, int y1)
{
    const bool red = (colour & 0xff0000u) != 0;
    long long sum = 0, n = 0;
    for (int y = y0; y < y1; ++y)
        for (int x = 0; x < 1280; ++x)
        {
            const std::uint32_t p = pixels[y * 1280 + x];
            const int r = int((p >> 16) & 255), g = int((p >> 8) & 255), b = int(p & 255);
            if (red ? (r > 160 && g < 90 && b < 110) : (g > 160 && r < 90 && b < 110)) { sum += x; ++n; }
        }
    return n ? int(sum / n) : -1;
}

// Rightmost and leftmost x of the message line's ink (its colour 0xffd8b3 is orange; the waves are blue or lilac)
inline std::pair<int, int> message_ink(const std::uint32_t* pixels, int y0, int y1)
{
    int left = 1280, right = -1;
    for (int y = y0; y < y1; ++y)
        for (int x = 0; x < 1280; ++x)
        {
            const std::uint32_t p = pixels[y * 1280 + x];
            const int r = int((p >> 16) & 255), b = int(p & 255);
            if (r - b > 40) { left = std::min(left, x); right = std::max(right, x); }
        }
    return {left, right};
}

// Ink (bright pixels) columns of rows [y0, y1)
inline std::pair<int, int> bright_ink(const std::uint32_t* pixels, int y0, int y1)
{
    int left = 1280, right = -1;
    for (int y = y0; y < y1; ++y)
        for (int x = 0; x < 1280; ++x)
            if ((pixels[y * 1280 + x] & 255) > 200 && ((pixels[y * 1280 + x] >> 8) & 255) > 200) { left = std::min(left, x); right = std::max(right, x); }
    return {left, right};
}

// The settings pages: catalog rows (util/ps4_cfg_catalog.hpp) as the launcher's page view shows them, with sample values
namespace cat = utils::ps4::cfg_catalog;

inline std::string row_label(std::string_view key)
{
    const cat::item* item = cat::find(key);
    return item ? std::string(i18n::tr(item->label)) : std::string(key);
}

inline std::string value_label(std::string_view key, std::string_view spelling)
{
    const cat::value_label* v = cat::find_value(key, spelling);
    if (!v) return std::string(spelling);
    return v->label != str::count_ ? std::string(i18n::tr(v->label)) : std::string(v->text);
}

enum class mark { none, automatic, game, pc, ps4, mine, this_game };

inline std::string badge_text(mark m)
{
    switch (m)
    {
    case mark::automatic: return i18n::tr(str::CFGU_BADGE_AUTO);
    case mark::game: return i18n::tr(str::CFGU_BADGE_GAME);
    case mark::pc: return i18n::tr(str::CFGU_BADGE_PC);
    case mark::ps4: return i18n::tr(str::CFGU_BADGE_PS4);
    case mark::mine: return i18n::tr(str::CFGU_BADGE_MINE);
    case mark::this_game: return i18n::tr(str::CFGU_BADGE_THIS_GAME);
    case mark::none: break;
    }
    return {};
}

struct sample_row
{
    std::string_view key;
    std::string value;
    mark badge = mark::none;
    bool greyed = false;
};

// A row's value when the scene gives none: its editor's summary, an enum's first value, a number, or "Off"
inline std::string default_value(const cat::item& item)
{
    switch (item.edit)
    {
    case cat::editor::zcull: return i18n::tr(str::CFGU_ZCULL_PRECISE);
    case cat::editor::libraries: return i18n::trf(str::CFGU_ITEMS, {0});
    case cat::editor::matrix: return i18n::trf(str::CFGU_ITEMS, {9});
    case cat::editor::log_levels: return i18n::trf(str::CFGU_CHANNELS, {1});
    case cat::editor::audio_formats: return i18n::trf(str::CFGU_AUDIO_FORMATS_N, {1});
    case cat::editor::microphones: return i18n::trf(str::CFGU_SLOTS_N, {0, 4});
    case cat::editor::midi: return i18n::trf(str::CFGU_SLOTS_N, {0, 3});
    case cat::editor::time_offset: return i18n::trf(str::CFGU_HOURS_OFFSET, {"2026-10-01 16:53", 0});
    case cat::editor::country: return "United States (us)";
    case cat::editor::font: return "n023055ms.ttf";
    case cat::editor::ipv4: return "0.0.0.0";
    case cat::editor::hex: return "0x0";
    default: break;
    }
    for (const cat::value_label& v : cat::value_labels())
        if (v.key == item.key) return v.label != str::count_ ? std::string(i18n::tr(v.label)) : std::string(v.text);
    if (!item.unit.empty()) return item.unit == "x" ? std::string("1x") : "100 " + std::string(item.unit);
    return i18n::tr(str::COM_DESLIGADO);
}

// The lines of a tab: its section captions and rows in the catalog's order ('hidden': with the hidden rows)
inline std::vector<page_line> tab_lines(cat::tab t, const std::vector<sample_row>& samples, bool hidden = false)
{
    std::vector<page_line> out;
    str last = str::count_;
    for (const cat::item& item : cat::items())
    {
        if (item.where != t || item.edit == cat::editor::zcull_member || (item.kind == cat::klass::hidden && !hidden)) continue;
        if (item.section != last)
        {
            page_line caption;
            caption.caption = true;
            caption.label = i18n::tr(item.section);
            out.push_back(std::move(caption));
            last = item.section;
        }
        const auto s = std::find_if(samples.begin(), samples.end(), [&](const sample_row& r) { return r.key == item.key; });
        page_line l;
        l.label = i18n::tr(item.label);
        l.value = s != samples.end() ? s->value : default_value(item);
        const mark m = item.kind == cat::klass::unavailable ? mark::ps4 : s != samples.end() ? s->badge : mark::none;
        l.badge = badge_text(m);
        l.padlock = m == mark::ps4;
        l.mine = m == mark::mine;
        l.greyed = item.kind == cat::klass::unavailable || item.kind == cat::klass::hidden || item.edit == cat::editor::readonly ||
            (s != samples.end() && s->greyed);
        out.push_back(std::move(l));
    }
    return out;
}

// The line of 'key' in tab_lines(t, ...)
inline std::size_t line_of(cat::tab t, std::string_view key, bool hidden = false)
{
    std::size_t at = 0;
    str last = str::count_;
    for (const cat::item& item : cat::items())
    {
        if (item.where != t || item.edit == cat::editor::zcull_member || (item.kind == cat::klass::hidden && !hidden)) continue;
        if (item.section != last) { ++at; last = item.section; }
        if (item.key == key) return at;
        ++at;
    }
    return 0;
}

inline int render(const std::filesystem::path& out, const std::map<std::string, image, std::less<>>& firmware_images, const font& typeface)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(out, ec);
    static constexpr std::array<std::pair<i18n::setting, const char*>, 4> languages{
        {{i18n::setting::pt_br, "pt"}, {i18n::setting::en, "en"}, {i18n::setting::es, "es"}, {i18n::setting::ar, "ar"}}};
    std::vector<std::uint32_t> frame(1280 * 720);
    int files = 0;
    for (const auto& [setting, code] : languages)
    {
        i18n::apply(setting, 17);
        const bool ar = i18n::rtl();
        const samples& s = sample(i18n::current());
        std::vector<std::string> drawn; // every line this language draws, for the '?' check
        const auto save = [&](const char* screen)
        {
            const fs::path file = out / (std::string("preview_") + code + "_" + screen + ".png");
            if (!write_png(file, frame.data())) return false;
            ++files;
            return true;
        };
        const auto run_scene = [&](scene& m, int category, std::size_t selected, const std::vector<entry>& list, std::string_view message, bool settings)
        {
            for (int i = 0; i < 40; ++i) m.draw(frame.data(), typeface, category, selected, list, 3. + i / 60., 1.f / 60, "01/10  16:53", message, settings);
            for (const entry& e : list) { drawn.push_back(e.title); drawn.push_back(e.subtitle); }
            drawn.emplace_back(message);
            drawn.emplace_back(scene::category_name(category));
        };
        // 1 game column: a locked disc game selected, a long message line, the footer
        {
            scene m;
            m.resources = firmware_images;
            std::vector<entry> games{{"@savedata", "Utilitário de dados guardados (PS3)", "", "item_tex_ps3util", {}},
                {"fifa.iso", "FIFA 14", std::string(i18n::tr(str::COM_SEU_DISCO)) + " · BLES01876", "item_tex_ps3game_folder", {}, "BLES01876"},
                {"terraria", "Terraria", "BLUS31537", "item_tex_ps3game_folder", {}}};
            games[1].locked = games[1].disc_bound = true;
            const std::string message = i18n::tr(str::LANG_SAVE_FAILED) + std::string(" · ") + i18n::tr(str::COM_RECOMENDADO_REINICIAR_APLICATIVO_ANTES_JOGAR);
            run_scene(m, 6, 1, games, message, false);
            drawn.push_back(games[1].subtitle + i18n::tr(str::XMB_PRECISA_DISCO_LEITOR));
            if (!save("xmb_game")) return -1;
            if (ar)
            {
                // The message line ends at x 1220 (fitted to 1160 px)
                const auto [left, right] = message_ink(frame.data(), 612, 640);
                std::cout << "ar message line ink: x " << left << ".." << right << '\n';
                if (std::abs(right - 1220) > 4 || left < 56) { std::cerr << "FAIL: Arabic message line not right-aligned at 1220\n"; return -1; }
            }
        }
        // 2 Configurações column with the language row selected
        {
            scene m;
            m.resources = firmware_images;
            std::vector<entry> rows{{"@rpcs3", s.rpcs3_settings, s.rpcs3_settings_help, "tex_sysconf", {}},
                {"@ui-language", i18n::tr(str::LANG_ROW_TITLE), i18n::trf(str::LANG_ROW_HELP, {i18n::raw(i18n::setting_label(i18n::current_setting()))}), "tex_sysconf", {}},
                {"@storage", i18n::tr(str::COM_ARMAZENAMENTO_JOGOS), s.storage_help, "tex_sysconf", {}}};
            run_scene(m, 1, 1, rows, i18n::trf(str::LANG_CHANGED, {i18n::raw(i18n::endonym(i18n::current()))}), false);
            if (!save("xmb_settings_column")) return -1;
        }
        // 3 settings top rows: audio continuity and the game language (0.40: 'Automático' is English (US))
        {
            scene m;
            m.resources = firmware_images;
            const std::string auto_label = i18n::trf(str::LANG_AUTO_FMT, {i18n::game_language_name(1)});
            std::vector<entry> rows{{"@setting", i18n::tr(str::SET_SALVAR_CONFIGURACOES), std::string(i18n::tr(str::SET_TODOS_JOGOS)) + " · " +
                    i18n::tr(str::COM_CONFIGURACAO_AUTOMATICA) + i18n::tr(str::COM_APLICADAS_PROXIMO_JOGO), "tex_sysconf", {}},
                {"@setting", i18n::tr(str::SET_MODO_FRIO_MENOS_CALOR_2), i18n::tr(str::SET_MENOS_CPU_GIRANDO_TOA_MENOS), "tex_sysconf", {}},
                {"@setting", i18n::tr(str::SET_AUDIO_CONTINUO_DESLIGADO), i18n::tr(str::SET_AUDIO_CONTINUO_HELP), "tex_sysconf", {}},
                {"@setting", i18n::trf(str::GL_TITLE_GLOBAL, {i18n::raw(auto_label)}), i18n::tr(str::GL_HELP_GLOBAL), "tex_sysconf", {}},
                {"@setting", i18n::tr(str::SET_AUDIO), i18n::tr(str::SET_ABRIR), "tex_sysconf", {}}};
            run_scene(m, 1, 3, rows, i18n::trf(str::GL_CHANGED, {i18n::raw(auto_label)}), true);
            if (!save("settings_top")) return -1;
        }
        // 3b 0.40: the PS4 tab's new rows: 'Áudio contínuo' off by default, the 'Idioma do PS4' value, the global
        // 'Modo seguro' row selected after X (its message)
        {
            scene m;
            m.resources = firmware_images;
            const std::string ps4_label = i18n::trf(str::GL_PS4_FMT, {i18n::game_language_name(i18n::game_language_auto())});
            std::vector<entry> rows{{"@setting", i18n::tr(str::SET_MODO_FRIO_MENOS_CALOR_2), i18n::tr(str::SET_MENOS_CPU_GIRANDO_TOA_MENOS), "tex_sysconf", {}},
                {"@setting", i18n::tr(str::SET_AUDIO_CONTINUO_AUTOMATICO), i18n::tr(str::SET_AUDIO_CONTINUO_HELP), "tex_sysconf", {}},
                {"@setting", i18n::trf(str::GL_TITLE_GLOBAL, {i18n::raw(ps4_label)}), i18n::tr(str::GL_HELP_GLOBAL), "tex_sysconf", {}},
                {"@setting", i18n::tr(str::SET_MODO_SEGURO_LIGADO), i18n::tr(str::SET_MODO_SEGURO_HELP), "tex_sysconf", {}}};
            run_scene(m, 1, 3, rows, i18n::tr(str::SET_MODO_SEGURO_LIGADO_MSG), true);
            if (!save("settings_ps4_safe_mode")) return -1;
            run_scene(m, 1, 1, rows, i18n::tr(str::SET_AUDIO_CONTINUO_AUTO_MSG), true);
            if (!save("settings_ps4_audio_continuity")) return -1;
            run_scene(m, 1, 2, rows, i18n::trf(str::GL_CHANGED, {i18n::raw(ps4_label)}), true);
            if (!save("settings_ps4_game_language")) return -1;
        }
        // 3c 0.40: a game's top rows at safe-mode level 2 after a GPU failure: the adjustment row names the level, the
        // per-game 'Modo seguro deste jogo' row (selected), and the boot toast on the message line; then the player's
        // own level 3 and level 3 that failed again
        {
            scene m;
            m.resources = firmware_images;
            const std::string level2 = i18n::trf(str::SET_MODO_SEGURO_NIVEL, {2u});
            const std::string adapt_help = i18n::trf(str::SET_MODO_SEGURO_NIVEL_MOTIVO, {2u, str::POL_FAIL_GPU}) + ", " + i18n::tr(str::SET_COMPILACAO_1_THREAD) +
                ", " + i18n::tr(str::SET_GPU_TRAVOU_SHADERS) + i18n::trf(str::SET_VALE_PROXIMOS_BOOT, {8u}) + i18n::tr(str::SET_REMOVE);
            std::vector<entry> rows{{"@setting", i18n::tr(str::SET_SALVAR_CONFIGURACOES), "BCUS98232 · " + std::string(i18n::tr(str::COM_CONFIGURACAO_AUTOMATICA)) +
                    i18n::tr(str::COM_APLICADAS_PROXIMO_JOGO), "tex_sysconf", {}},
                {"@setting", i18n::tr(str::SET_AJUSTE_AUTOMATICO_APOS_FALHA), adapt_help, "tex_sysconf", {}},
                {"@setting", i18n::trf(str::SET_MODO_SEGURO_JOGO, {i18n::raw(level2)}), i18n::trf(str::SET_MODO_SEGURO_JOGO_AUTO_HELP, {str::POL_FAIL_GPU, 8u}),
                    "tex_sysconf", {}},
                {"@setting", i18n::tr(cat::tab_name(cat::tab::gpu)), i18n::tr(str::SET_ABRIR), "tex_sysconf", {}}};
            run_scene(m, 1, 2, rows, i18n::trf(str::POL_SAFE_MODE_TOAST, {"BCUS98232", str::POL_FAIL_GPU, 2u}), true);
            if (!save("settings_game_safe_level")) return -1;
            run_scene(m, 1, 1, rows, i18n::trf(str::POL_SAFE_MODE_TOAST, {"BCUS98232", str::POL_FAIL_GPU, 2u}), true);
            if (!save("settings_game_safe_adapt")) return -1;
            const std::string level3 = i18n::trf(str::SET_MODO_SEGURO_NIVEL, {3u});
            rows[2] = {"@setting", i18n::trf(str::SET_MODO_SEGURO_JOGO, {i18n::raw(level3)}), i18n::trf(str::SET_MODO_SEGURO_JOGO_USER_HELP, {8u}), "tex_sysconf", {}};
            run_scene(m, 1, 2, rows, i18n::trf(str::SET_MODO_SEGURO_JOGO_CHANGED, {"BCUS98232", i18n::raw(level3)}), true);
            if (!save("settings_game_safe_user")) return -1;
            rows[2] = {"@setting", i18n::trf(str::SET_MODO_SEGURO_JOGO, {i18n::raw(level3)}), i18n::tr(str::SET_MODO_SEGURO_JOGO_EXHAUSTED_HELP), "tex_sysconf", {}};
            run_scene(m, 1, 2, rows, i18n::trf(str::POL_SAFE_MODE_EXHAUSTED, {"BCUS98232"}), true);
            if (!save("settings_game_safe_exhausted")) return -1;
            const std::string off = i18n::tr(str::COM_DESLIGADO);
            rows[2] = {"@setting", i18n::trf(str::SET_MODO_SEGURO_JOGO, {i18n::raw(off)}), i18n::tr(str::SET_MODO_SEGURO_JOGO_HELP), "tex_sysconf", {}};
            rows.erase(rows.begin() + 1);
            run_scene(m, 1, 1, rows, i18n::trf(str::POL_ONLINE_SEM_CONTA, {"BLUS31426"}), true);
            if (!save("settings_game_safe_off")) return -1;
        }
        // 3d 0.40: the new rows of the settings menu, built as util/ps4_settings.hpp builds them: a game's 'Novidades neste
        // jogo' row (none, the 0.39 audio rules after X, names written by hand in the file), the global 'Usar meus valores
        // salvos (N)' action (its question after the first X, the result after the second), the adjustment row after a
        // device loss (GPU commands split) and a title at level 2 after a thread failure. The boot notices are system
        // toasts: their texts are measured on the message line. A title or message wider than its room, or a help line on
        // more than two, is listed as 'cut' and counted (the screen edge cuts a Latin line): the menu's texts and their
        // composition are fixed in the string tables and the menu, not here.
        unsigned rows_cut = 0;
        {
            constexpr float room = 1240 - (375 + 75);
            const auto run_rows = [&](int category, const std::vector<entry>& list, std::size_t selected, const std::string& message, const char* screen)
            {
                scene m;
                m.resources = firmware_images;
                run_scene(m, category, selected, list, message, category == 1);
                for (const entry& e : list)
                {
                    if (const float w = typeface.measure(e.title, 27); w > room)
                    { ++rows_cut; std::cout << "cut " << code << ' ' << screen << ": title " << int(w) << " px > " << int(room) << " px '" << e.title << "'\n"; }
                    if (typeface.wrap(e.subtitle, 17, room, 3, ar).size() > 2) { ++rows_cut; std::cout << "cut " << code << ' ' << screen << ": help over two lines '" << e.subtitle << "'\n"; }
                }
                if (const float w = typeface.measure(message, 20); w > 1160)
                { ++rows_cut; std::cout << "cut " << code << ' ' << screen << ": message " << int(w) << " px > 1160 px '" << message << "'\n"; }
                return save(screen);
            };
            const std::string game = "BLUS30443", sep = " \xC2\xB7 ";
            const auto row = [](std::string title, std::string help) { return entry{"@setting", std::move(title), std::move(help), "tex_sysconf", {}}; };
            const auto tab_row = [&](cat::tab t, std::array<unsigned, 4> n)
            { return row(i18n::tr(cat::tab_name(t)), i18n::trf(str::CFGU_TAB_SUMMARY, {n[0], n[1], n[2], n[3]})); };
            const entry auto_on = row(i18n::tr(str::SET_CONFIGURACAO_AUTOMATICA_LIGADA), i18n::tr(str::SET_CADA_JOGO_INICIA_AJUSTES_RECOMENDADOS));
            const entry game_save = row(i18n::tr(str::SET_SALVAR_CONFIGURACOES), game + sep + i18n::tr(str::COM_CONFIGURACAO_AUTOMATICA) + i18n::tr(str::COM_APLICADAS_PROXIMO_JOGO));
            const entry game_mode = row(i18n::trf(str::CFGU_MODE_GAME_AUTO, {0u}), i18n::trf(str::CFGU_CHANGED, {"X", i18n::raw(i18n::tr(str::CFGU_MODE_GAME_MANUAL))}));
            const entry safe_off = row(i18n::trf(str::SET_MODO_SEGURO_JOGO, {i18n::raw(i18n::tr(str::COM_DESLIGADO))}), i18n::tr(str::SET_MODO_SEGURO_JOGO_HELP));
            const auto features = [&](const std::string& value) { return row(i18n::trf(str::CFGU_CHANGED, {str::SET_NOVIDADES_JOGO, i18n::raw(value)}), i18n::tr(str::SET_NOVIDADES_HELP)); };
            // a game: Salvar, the switch, the mode, the per-game safe mode, 'Novidades neste jogo' (selected), the first tabs
            std::vector<entry> game_rows{game_save, auto_on, game_mode, safe_off, features(i18n::tr(str::SET_NOVIDADES_NENHUMA)),
                tab_row(cat::tab::cpu, {7, 2, 0, 0}), tab_row(cat::tab::gpu, {36, 9, 4, 1})};
            if (!run_rows(1, game_rows, 4, {}, "settings_game_features")) return -1;
            const std::string audio_039 = i18n::tr(str::SET_NOVIDADES_AUDIO_039);
            game_rows[4] = features(audio_039);
            if (!run_rows(1, game_rows, 4, i18n::trf(str::SET_NOVIDADES_CHANGED, {game, i18n::raw(audio_039)}), "settings_game_features_039")) return -1;
            // rows named by hand in custom_configs/<TITLE_ID>.features: their names, the same in every language
            game_rows[4] = features("audio_hr_waits, pipelines_first");
            if (!run_rows(1, game_rows, 4, {}, "settings_game_features_named")) return -1;
            // the global menu in the automatic configuration: five values of the player's file the automatic one replaces
            const std::string scope_all = i18n::tr(str::SET_TODOS_JOGOS);
            std::string legacy;
            for (const char* key : {"Video > Frame limit", "Video > Shader Mode", "Video > Write Color Buffers"}) legacy += (legacy.empty() ? "" : ", ") + row_label(key);
            legacy += " (+2)";
            const std::string use_help = i18n::trf(str::SET_USAR_VALORES_SALVOS_HELP, {i18n::raw(legacy)});
            const auto global_rows = [&](bool use_saved, const std::string& help, bool pending)
            {
                std::vector<entry> rows{row(i18n::tr(str::SET_SALVAR_CONFIGURACOES), scope_all + sep + i18n::tr(str::COM_CONFIGURACAO_AUTOMATICA) +
                    i18n::tr(pending ? str::SET_ALTERACOES_PENDENTES : str::COM_APLICADAS_PROXIMO_JOGO)), auto_on};
                if (use_saved) rows.push_back(row(i18n::trf(str::SET_USAR_VALORES_SALVOS, {5u}), help));
                rows.push_back(tab_row(cat::tab::cpu, {7, 2, 0, 0}));
                rows.push_back(tab_row(cat::tab::gpu, {36, 9, 4, 1}));
                rows.push_back(row(i18n::tr(str::SET_APLICAR_PERFIL_AUTOMATICO), i18n::tr(str::SET_LLVM_VULKAN_720P_SHADERS_ASSINCRONOS)));
                return rows;
            };
            if (!run_rows(1, global_rows(true, use_help, false), 2, {}, "settings_use_saved")) return -1;
            const std::string asked = use_help + sep + i18n::tr(str::CFGU_CONFIRM_AGAIN);
            if (!run_rows(1, global_rows(true, asked, false), 2, asked, "settings_use_saved_confirm")) return -1;
            if (!run_rows(1, global_rows(false, {}, true), 0, i18n::trf(str::SET_USAR_VALORES_SALVOS_DONE, {5u}), "settings_use_saved_done")) return -1;
            // after a device loss of this game: shaders without the interpreter and the GPU commands split, with the boot's
            // adjustment notice
            {
                const entry adapt = row(i18n::tr(str::SET_AJUSTE_AUTOMATICO_APOS_FALHA), std::string(i18n::tr(str::SET_GPU_TRAVOU_SHADERS)) + ", " +
                    i18n::tr(str::SET_COMANDOS_GPU_DIVIDIDOS) + i18n::trf(str::SET_VALE_PROXIMOS_BOOT, {8u}) + i18n::tr(str::SET_REMOVE));
                const std::vector<entry> rows{game_save, auto_on, adapt, game_mode, safe_off, features(i18n::tr(str::SET_NOVIDADES_NENHUMA)),
                    tab_row(cat::tab::gpu, {36, 9, 4, 1})};
                const std::string items = std::string(i18n::tr(str::POL_SHADERS_SEM_INTERPRETADOR)) + i18n::tr(str::POL_LIST_SEPARATOR) +
                    i18n::tr(str::POL_COMANDOS_GPU_DIVIDIDOS);
                if (!run_rows(1, rows, 2, i18n::trf(str::POL_ADAPT_TOAST, {game, i18n::raw(items)}), "settings_game_adapt_cb_split")) return -1;
            }
            // level 2 after a thread failure (the music and PC database guards stay on: only the startup line shows it)
            {
                const std::string level2 = i18n::trf(str::SET_MODO_SEGURO_NIVEL, {2u});
                const entry adapt = row(i18n::tr(str::SET_AJUSTE_AUTOMATICO_APOS_FALHA), i18n::trf(str::SET_MODO_SEGURO_NIVEL_MOTIVO, {2u, str::POL_FAIL_THREADS}) +
                    i18n::trf(str::SET_VALE_PROXIMOS_BOOT, {8u}) + i18n::tr(str::SET_REMOVE));
                const entry safe = row(i18n::trf(str::SET_MODO_SEGURO_JOGO, {i18n::raw(level2)}), i18n::trf(str::SET_MODO_SEGURO_JOGO_AUTO_HELP, {str::POL_FAIL_THREADS, 8u}));
                const std::vector<entry> rows{game_save, auto_on, adapt, game_mode, safe, features(i18n::tr(str::SET_NOVIDADES_NENHUMA)), tab_row(cat::tab::gpu, {36, 9, 4, 1})};
                if (!run_rows(1, rows, 4, i18n::trf(str::POL_SAFE_MODE_TOAST, {game, str::POL_FAIL_THREADS, 2u}), "settings_game_safe_threads")) return -1;
            }
            // the boot notice of a game whose format-1 file holds values the automatic configuration replaced
            {
                const std::vector<entry> games{{"@savedata", i18n::tr(str::LAU_UTILITARIO_DADOS_GUARDADOS), "", "item_tex_ps3util", {}},
                    {"demons.iso", "Demon's Souls", game, "item_tex_ps3game_folder", {}, game}, {"terraria", "Terraria", "BLUS31537", "item_tex_ps3game_folder", {}, "BLUS31537"}};
                if (!run_rows(6, games, 1, i18n::trf(str::POL_AJUSTES_ARQUIVO_SEM_EFEITO, {4u}), "xmb_notice_saved_values")) return -1;
            }
            std::cout << "Settings rows " << code << ": 9 screens, " << rows_cut << " text(s) cut\n";
        }
        // Dialogs
        const auto run_dialog = [&](const dialog& d, const char* screen)
        {
            scene backdrop;
            backdrop.resources = firmware_images;
            for (int i = 0; i < 20; ++i) backdrop.backdrop(frame.data(), 4. + i / 60.);
            canvas c(frame.data(), 1280, 720);
            draw_dialog(c, typeface, backdrop.resources, d, 4.5);
            drawn.push_back(d.title);
            for (const auto& l : d.lines) drawn.push_back(l);
            for (const auto& n : d.notes) drawn.push_back(n);
            drawn.push_back(d.progress_left);
            drawn.push_back(d.progress_right);
            for (const auto& b : d.buttons) drawn.push_back(b.label);
            return save(screen);
        };
        const image* disc_icon = firmware_images.contains("item_tex_disc_install") ? &firmware_images.at("item_tex_disc_install") : nullptr;
        if (!run_dialog({"FIFA 14 (BLES01876)", {s.disc_question}, {s.disc_size, s.disc_free}, -1, {}, {}, false, disc_icon,
                {cross_button(i18n::tr(str::COM_INSTALAR)), square_button(i18n::tr(str::COM_APAGAR)), circle_button(i18n::tr(str::COM_VOLTAR))}}, "disc_question"))
            return -1;
        if (!run_dialog({"FIFA 14 (BLES01876)", {s.copying}, {s.copied}, 420, percent_text(420), remaining_text(6'400'000'000, 15'000'000), false, disc_icon,
                {cross_button(i18n::tr(str::COM_OK)), circle_button(i18n::tr(str::COM_CANCELAR))}}, "copy_progress"))
            return -1;
        if (!run_dialog({i18n::tr(str::COM_FIRMWARE_PS3), {s.fw_question}, {s.fw_file, s.fw_now}, -1, {}, {}, false, nullptr,
                {cross_button(i18n::tr(str::COM_SIM)), circle_button(i18n::tr(str::COM_NAO))}}, "firmware_question"))
            return -1;
        if (!run_dialog({i18n::tr(str::COM_MEMORIA_VIDEO_ESGOTADA), {s.oom_line}, {i18n::tr(str::COM_RECOMENDADO_REINICIAR_APLICATIVO_ANTES_JOGAR)}, -1, {}, {},
                false, nullptr, {cross_button(i18n::tr(str::COM_OK))}}, "out_of_memory"))
            return -1;
        if (!run_dialog({{}, {s.boot_next}, {}, -1, {}, {}, true, nullptr, {circle_button(s.boot_next_note)}}, "boot_next"))
            return -1;
        if (!run_dialog({{}, {i18n::tr(str::COM_OCORREU_ERRO_EMULADOR)}, {s.close_hint}, -1, {}, {}, false, nullptr, {cross_button(i18n::tr(str::COM_OK))}},
                "emulator_error"))
            return -1;

        // Settings pages (the launcher's page view over the settings browser; catalog texts, sample values): the top level,
        // the GPU tab globally and for a game, an unavailable row, the Shader Mode values, the keyboard prompt of a number,
        // a warned setting, the row options, the full help, the libraries and log levels sub-pages, Depuração with the
        // hidden settings, the save question, and an XMB help line on two lines. What fit() had to cut is listed.
        {
            const std::string scope_all = i18n::tr(str::SET_TODOS_JOGOS), scope_game = "BLUS30443";
            const std::string gpu = i18n::tr(cat::tab_name(cat::tab::gpu));
            const std::vector<page_button> page_buttons{{"tex_cross", i18n::tr(str::CFGU_FOOTER_CHANGE)}, {"tex_circle", i18n::tr(str::COM_VOLTAR)},
                {"tex_square", i18n::tr(str::CFGU_FOOTER_AUTO)}, {"tex_triangle", i18n::tr(str::CFGU_FOOTER_OPTIONS)}};
            const std::vector<page_button> popup_buttons{{"tex_cross", i18n::tr(str::COM_SELECIONAR)}, {"tex_circle", i18n::tr(str::COM_CANCELAR)}};
            const std::vector<page_button> sub_buttons{{"tex_cross", i18n::tr(str::CFGU_FOOTER_CHANGE)}, {"tex_circle", i18n::tr(str::COM_VOLTAR)}};
            unsigned cut = 0;
            // What the page drew had to be fitted: labels (room left by the value and the badge), values (360 px), help and
            // message (two lines each)
            const auto widths = [&](const settings_page& p, const char* screen)
            {
                for (const page_line& l : p.lines)
                {
                    if (l.caption) continue;
                    // the page's rule: the label keeps up to 560 px, the value at least 360 px, a short label leaves the rest
                    const float bw = l.badge.empty() ? 0 : typeface.measure(l.badge, 17) + 16 + (l.padlock ? 18 : 0);
                    const float room = 1110 - 170 - 20 - (bw > 0 ? bw + 14 : 0);
                    const float value_room = std::max(360.f, room - std::min(typeface.measure(l.label, 21), 560.f));
                    const float vw = std::min(typeface.measure(l.value, 21), value_room);
                    const float label_room = std::max(60.f, std::min(560.f, room - vw));
                    if (typeface.measure(l.value, 21) > value_room) { ++cut; std::cout << "fit " << code << ' ' << screen << ": value '" << l.value << "' " << typeface.measure(l.value, 21) << " px > " << value_room << '\n'; }
                    if (typeface.measure(l.label, 21) > label_room) { ++cut; std::cout << "fit " << code << ' ' << screen << ": label '" << l.label << "' " << typeface.measure(l.label, 21) << " px > " << label_room << '\n'; }
                }
                for (const auto& [text, size, width] : {std::tuple{p.help, 17.f, 940.f}, std::tuple{p.message, 20.f, 1160.f}})
                    if (const auto lines = typeface.wrap(text, size, width, 3, ar); lines.size() > 2) { ++cut; std::cout << "fit " << code << ' ' << screen << ": more than two lines: '" << text << "'\n"; }
            };
            const auto run_page = [&](const settings_page& p, const char* screen)
            {
                scene m;
                m.resources = firmware_images;
                for (int i = 0; i < 40; ++i) m.draw_settings_page(frame.data(), typeface, p, 3. + i / 60., 1.f / 60, "01/10  16:53");
                drawn.push_back(p.title);
                drawn.push_back(p.scope);
                drawn.push_back(p.help);
                drawn.push_back(p.message);
                drawn.push_back(p.footer_help);
                for (const page_line& l : p.lines) { drawn.push_back(l.label); drawn.push_back(l.value); drawn.push_back(l.badge); }
                for (const page_button& b : p.buttons) drawn.push_back(b.label);
                if (p.popup)
                {
                    drawn.push_back(p.popup->title);
                    for (const page_choice& c : p.popup->choices) drawn.push_back(c.label);
                }
                widths(p, screen);
                return save(screen);
            };
            const auto page_of = [&](cat::tab t, std::string scope, const std::vector<page_line>& lines, std::string_view key, std::string help, std::string message)
            {
                settings_page p;
                p.title = i18n::tr(cat::tab_name(t));
                p.scope = std::move(scope);
                p.lines = lines;
                p.selected = line_of(t, key);
                p.help = std::move(help);
                p.message = std::move(message);
                p.buttons = page_buttons;
                p.footer_help = i18n::tr(str::CFGU_FOOTER_HELP);
                return p;
            };
            // 1 top level: the tabs with their summaries (an XMB list, as the menu draws it)
            {
                scene m;
                m.resources = firmware_images;
                std::vector<entry> rows{{"@setting", i18n::tr(str::SET_SALVAR_CONFIGURACOES), scope_all + " \xC2\xB7 " + i18n::tr(str::COM_CONFIGURACAO_AUTOMATICA) +
                        i18n::tr(str::COM_APLICADAS_PROXIMO_JOGO), "tex_sysconf", {}},
                    {"@setting", i18n::tr(str::SET_CONFIGURACAO_AUTOMATICA_LIGADA), i18n::tr(str::SET_CADA_JOGO_INICIA_AJUSTES_RECOMENDADOS), "tex_sysconf", {}}};
                const std::array<std::array<unsigned, 4>, cat::tab_count> counts{{{7, 2, 0, 0}, {36, 9, 4, 1}, {18, 2, 1, 0}, {24, 0, 3, 0}, {17, 0, 2, 0},
                    {10, 0, 4, 0}, {54, 6, 3, 0}, {55, 1, 6, 0}, {38, 0, 4, 0}, {0, 0, 0, 0}}};
                for (std::size_t t = 0; t < cat::tab_count; ++t)
                    rows.push_back({"@setting", i18n::tr(cat::tab_name(cat::tab(t))), t + 1 == cat::tab_count ? std::string(i18n::tr(str::SET_ABRIR))
                        : i18n::trf(str::CFGU_TAB_SUMMARY, {counts[t][0], counts[t][1], counts[t][2], counts[t][3]}), "tex_sysconf", {}});
                rows.push_back({"@setting", i18n::tr(str::SET_APLICAR_PERFIL_AUTOMATICO), i18n::tr(str::SET_LLVM_VULKAN_720P_SHADERS_ASSINCRONOS), "tex_sysconf", {}});
                rows.push_back({"@setting", i18n::tr(str::SET_RESTAURAR_VALORES_PADRAO), i18n::tr(str::SET_VALORES_AUTOMATICOS_PS4_OPCOES_DESTE), "tex_sysconf", {}});
                run_scene(m, 1, 3, rows, i18n::trf(str::LAU_SETTINGS_GLOBAIS, {i18n::raw(i18n::tr(str::COM_CONFIGURACAO_AUTOMATICA))}), true);
                if (!save("cfg_top")) return -1;
            }
            // GPU samples: the automatic profile's values, a PS4 limit, the renderer
            const std::string shader_auto = value_label("Video > Shader Mode", "Async Recompiler with Shader Interpreter");
            const std::vector<sample_row> gpu_global{{"Video > Renderer", "Vulkan"}, {"Video > Vulkan > Adapter", "AMD Radeon (RADV GFX7)"},
                {"Video > Aspect ratio", "16:9"}, {"Video > Frame limit", value_label("Video > Frame limit", "Auto")},
                {"Video > MSAA", value_label("Video > MSAA", "Disabled"), mark::automatic}, {"Video > Resolution", "1280x720", mark::automatic},
                {"Video > Resolution Scale", "100 %"}, {"Video > Shader Mode", shader_auto, mark::automatic},
                {"Video > Write Color Buffers", i18n::tr(str::COM_DESLIGADO)}, {"Video > Vulkan > VRAM allocation limit (MB)", "768 MB", mark::ps4},
                {"Video > 3D Display Mode", value_label("Video > 3D Display Mode", "Disabled"), mark::none, true},
                {"Video > Screen size in inches", "50", mark::none, true}};
            const std::vector<page_line> gpu_lines = tab_lines(cat::tab::gpu, gpu_global);
            // 2 GPU, all games: the Shader Mode row the automatic configuration decides
            if (!run_page(page_of(cat::tab::gpu, scope_all, gpu_lines, "Video > Shader Mode",
                    i18n::trf(str::CFGU_NOTE_AUTO, {i18n::raw(shader_auto), i18n::raw(value_label("Video > Shader Mode", "Async Recompiler (multi-threaded)"))}), {}), "cfg_gpu"))
                return -1;
            // 3 GPU for a game: a value of this game and one of the player's own
            {
                // the game's values first (tab_lines takes the first sample of a key)
                std::vector<sample_row> game{{"Video > Write Color Buffers", i18n::tr(str::COM_LIGADO), mark::mine}, {"Video > Frame limit", "30", mark::this_game}};
                game.insert(game.end(), gpu_global.begin(), gpu_global.end());
                const std::vector<page_line> lines = tab_lines(cat::tab::gpu, game);
                if (!run_page(page_of(cat::tab::gpu, scope_game, lines, "Video > Write Color Buffers", i18n::tr(str::CFGU_NOTE_MINE),
                        i18n::trf(str::CFGU_PINNED, {i18n::raw(row_label("Video > Write Color Buffers"))})), "cfg_gpu_game"))
                    return -1;
            }
            // 4 an unavailable row (X on it): the reason, the padlock
            {
                const cat::item* mtrsx = cat::find("Video > Multithreaded RSX");
                const std::string reason = std::string(i18n::tr(str::COM_INDISPONIVEL_PS4)) + (mtrsx ? i18n::tr(mtrsx->reason) : "");
                if (!run_page(page_of(cat::tab::gpu, scope_all, gpu_lines, "Video > Multithreaded RSX", reason, reason), "cfg_unavailable_mtrsx"))
                    return -1;
            }
            // 5 the Shader Mode values (radio dots), the warned one under the cursor with its reason in the help line
            {
                const cat::item* item = cat::find("Video > Shader Mode");
                page_popup popup;
                popup.title = row_label("Video > Shader Mode");
                for (const char* spelling : {"Async Recompiler with Shader Interpreter", "Async Recompiler (multi-threaded)", "Legacy Recompiler (single-threaded)",
                         "Shader Interpreter only"})
                    popup.choices.push_back({value_label("Video > Shader Mode", spelling), false, popup.choices.empty()});
                popup.selected = 3;
                settings_page p = page_of(cat::tab::gpu, scope_all, gpu_lines, "Video > Shader Mode", item && item->value_reason != str::count_ ?
                    std::string(i18n::tr(item->value_reason)) : std::string{}, {});
                p.buttons = popup_buttons;
                p.footer_help.clear();
                p.popup = &popup;
                if (!run_page(p, "cfg_picker_shadermode")) return -1;
            }
            // 6 X on a number: the system keyboard opens (drawn by the system), the message line says what to type
            if (!run_page(page_of(cat::tab::gpu, scope_all, gpu_lines, "Video > Resolution Scale", i18n::tr(cat::find("Video > Resolution Scale")->help),
                    i18n::trf(str::CFGU_KB_TITLE_NUMBER, {i18n::raw(row_label("Video > Resolution Scale"))})), "cfg_number_resscale"))
                return -1;
            // 7 a warned setting: the question
            {
                const cat::item* item = cat::find("Core > Accurate SPU Reservations");
                if (!run_dialog({i18n::tr(str::CFGU_WARN_TITLE), {row_label("Core > Accurate SPU Reservations")}, {item ? std::string(i18n::tr(item->reason)) : std::string{}},
                        -1, {}, {}, false, nullptr, {cross_button(i18n::tr(str::CFGU_FOOTER_CHANGE)), circle_button(i18n::tr(str::COM_CANCELAR))}}, "cfg_advanced_warn"))
                    return -1;
            }
            // 8 Triangle: the row options of a game's row
            {
                const std::vector<page_line> lines = tab_lines(cat::tab::advanced, {{"Core > Accurate SPU Reservations", i18n::tr(str::COM_LIGADO), mark::game}});
                page_popup popup;
                popup.title = row_label("Core > Accurate SPU Reservations");
                popup.radio = false;
                for (const str id : {str::CFGU_OPT_BACK_AUTO, str::CFGU_OPT_UPSTREAM, str::CFGU_OPT_USE_GLOBAL, str::CFGU_OPT_PIN, str::CFGU_OPT_FULL_HELP, str::CFGU_OPT_YAML_KEY})
                    popup.choices.push_back({i18n::tr(id), false, false});
                settings_page p = page_of(cat::tab::advanced, scope_game, lines, "Core > Accurate SPU Reservations", i18n::tr(str::SET_CORRECAO_DESTE_JOGO_BOOT_MANTEM), {});
                p.buttons = popup_buttons;
                p.footer_help.clear();
                p.popup = &popup;
                if (!run_page(p, "cfg_options")) return -1;
            }
            // 9 Options > Full help: the PC tooltip as a dialog (its " · " parts are the notes)
            {
                const cat::item* item = cat::find("Video > Write Color Buffers");
                std::string tip = item && item->tip != str::count_ ? std::string(i18n::tr(item->tip)) : std::string{};
                dialog d{row_label("Video > Write Color Buffers"), {}, {}, -1, {}, {}, false, nullptr, {cross_button(i18n::tr(str::COM_OK))}};
                for (bool first = true; !tip.empty(); first = false)
                {
                    const std::size_t at = tip.find(" \xC2\xB7 ");
                    (first ? d.lines : d.notes).push_back(tip.substr(0, at));
                    tip = at == std::string::npos ? std::string{} : tip.substr(at + 4);
                }
                if (!run_dialog(d, "cfg_full_help")) return -1;
            }
            // 10 the libraries sub-page: PC's two lists, a checked library runs the other way
            {
                std::vector<page_line> lines;
                const auto caption = [&](str id) { page_line c; c.caption = true; c.label = i18n::tr(id); lines.push_back(std::move(c)); };
                const auto lib = [&](const char* name, const char* runs, bool mine) { page_line l; l.label = name; l.value = runs; l.mine = mine; l.badge = mine ? badge_text(mark::mine) : ""; lines.push_back(std::move(l)); };
                caption(str::CFGU_LIB_LLE_SECTION);
                for (const char* name : {"libatrac3plus.sprx", "libfont.sprx", "libfontFT.sprx", "libfreetype.sprx"}) lib(name, "LLE", false);
                lib("libpngdec.sprx", "HLE", true);
                caption(str::CFGU_LIB_HLE_SECTION);
                for (const char* name : {"libaudio.sprx", "libio.sprx", "libsysutil.sprx", "libsysutil_np.sprx"}) lib(name, "HLE", false);
                settings_page p;
                p.title = row_label("Core > Libraries Control");
                p.scope = i18n::tr(cat::tab_name(cat::tab::advanced)) + std::string(" \xC2\xB7 ") + scope_all;
                p.lines = lines;
                p.selected = 5;
                p.help = i18n::tr(str::CFGU_LIB_LLE_SECTION);
                p.buttons = sub_buttons;
                p.footer_help = i18n::tr(str::CFGU_LIB_CLEAR);
                if (!run_page(p, "cfg_libraries")) return -1;
            }
            // 11 the log levels sub-page
            {
                std::vector<page_line> lines;
                for (const char* channel : {"Loader", "PPU", "SPU", "RSX", "cellAudio", "cellGame", "cellSaveData", "sys_fs", "sys_net", "Vulkan"})
                {
                    page_line l;
                    l.label = channel;
                    const bool mine = std::string_view(channel) == "RSX";
                    l.value = i18n::tr(mine ? str::CFGU_LOG_LEVEL_TRACE : str::CFGU_LOG_LEVEL_NOTICE);
                    l.mine = mine;
                    l.badge = mine ? badge_text(mark::mine) : "";
                    lines.push_back(std::move(l));
                }
                settings_page p;
                p.title = row_label("Log");
                p.scope = i18n::tr(cat::tab_name(cat::tab::debug)) + std::string(" \xC2\xB7 ") + scope_all;
                p.lines = lines;
                p.selected = 3;
                p.help = i18n::tr(cat::find("Log")->help);
                p.buttons = sub_buttons;
                p.footer_help = i18n::tr(str::XMB_ESQ_DIR_ALTERAR_R1_ACELERAR);
                if (!run_page(p, "cfg_loglevels")) return -1;
            }
            // 12 Depuração with the hidden settings shown, the switch at the end
            {
                std::vector<page_line> lines = tab_lines(cat::tab::debug, {}, true);
                page_line toggle;
                toggle.label = i18n::tr(str::CFGU_SHOW_HIDDEN);
                toggle.value = i18n::tr(str::COM_LIGADO);
                lines.push_back(std::move(toggle));
                settings_page p = page_of(cat::tab::debug, scope_all, lines, "", i18n::tr(str::CFGU_READONLY), {});
                p.selected = lines.size() - 1;
                if (!run_page(p, "cfg_debug_hidden")) return -1;
            }
            // 13 Circle at the top level with edits not saved
            if (!run_dialog({scope_all, {i18n::tr(str::CFGU_SAVE_QUESTION)}, {}, -1, {}, {}, false, nullptr,
                    {cross_button(i18n::tr(str::CFGU_SAVE)), circle_button(i18n::tr(str::CFGU_DISCARD)), triangle_button(i18n::tr(str::CFGU_KEEP_EDITING))}}, "cfg_save_question"))
                return -1;
            // 14 an XMB help line wider than its room continues on a second line (firmware font): no ink right of x 1240
            {
                scene m;
                m.resources = firmware_images;
                std::vector<entry> rows{{"@storage", i18n::tr(str::COM_ARMAZENAMENTO_JOGOS), s.storage_help, "item_tex_plain_folder", {}},
                    {"@usb:none", i18n::tr(str::LAU_USB_NENHUM_CONECTADO), i18n::tr(str::LAU_USB_NENHUM_CONECTADO_HELP), "item_tex_plain_folder", {}},
                    {"@disc:keys", i18n::tr(str::LAU_IMPORTAR_CHAVES_DKEY), i18n::tr(str::LAU_IMPORTAR_CHAVES_DKEY_HELP), "item_tex_disc_install", {}}};
                run_scene(m, 7, 1, rows, {}, false);
                const bool two = typeface.measure(rows[1].subtitle, 17) > 1240 - (375 + 75);
                const auto [left, right] = bright_ink(frame.data(), 374, 396);
                std::cout << "help line " << code << ": " << typeface.measure(rows[1].subtitle, 17) << " px, second line ink x " << left << ".." << right << '\n';
                if (two != (right >= 0) || right > 1242) { std::cerr << "FAIL: help line wrap (" << code << ")\n"; return -1; }
                const auto [l1, r1] = bright_ink(frame.data(), 352, 373);
                if (r1 > 1242) { std::cerr << "FAIL: help line past x 1240 (" << code << ")\n"; return -1; }
                if (!save("xmb_help_two_lines")) return -1;
            }
            // Mirroring of a page: the first row's label from x 170, or ending at x 1110 right to left; the footer pairs
            {
                const std::vector<page_line> lines{{i18n::tr(str::CFGG_GPU_RENDERER), {}, {}, true}, {row_label("Video > Renderer"), "Vulkan", {}}};
                settings_page p = page_of(cat::tab::gpu, scope_all, lines, "Video > Renderer", {}, {});
                p.selected = 1;
                auto marked = firmware_images;
                marked.insert_or_assign("tex_cross", image{17, 17, std::vector<std::uint32_t>(17 * 17, 0xffff0000u)});
                marked.insert_or_assign("tex_circle", image{17, 17, std::vector<std::uint32_t>(17 * 17, 0xff00ff00u)});
                scene m;
                m.resources = marked;
                m.cached_background = false;
                m.draw_settings_page(frame.data(), typeface, p, 1., .016f, "01/10  16:53");
                const auto [left, right] = bright_ink(frame.data(), 204, 238);
                const float label = typeface.measure(lines[1].label, 21), value = typeface.measure("Vulkan", 21);
                // the left element: ink from 'left' until a gap of 24 px (the label left to right, the value right to left)
                const auto bright_column = [&](int x)
                {
                    for (int y = 204; y < 238; ++y)
                        if ((frame[y * 1280 + x] & 255) > 200 && ((frame[y * 1280 + x] >> 8) & 255) > 200) return true;
                    return false;
                };
                int end = left;
                for (int x = std::max(left, 0), gap = 0; x < 1280 && gap < 24; ++x)
                {
                    if (bright_column(x)) { end = x; gap = 0; }
                    else ++gap;
                }
                const float first = float(end - left + 1);
                std::cout << "page row ink " << code << ": x " << left << ".." << right << ", left element " << first << " px (label " << label << " px, value "
                          << value << " px)\n";
                if (std::abs(left - 170) > 4 || std::abs(right - 1110) > 4 || std::abs(first - (ar ? value : label)) > 6)
                {
                    std::cerr << "FAIL: page row layout (" << code << ")\n";
                    return -1;
                }
                const int cross = colour_x(frame.data(), 0xffff0000u, 668, 685), circle = colour_x(frame.data(), 0xff00ff00u, 668, 685);
                if (cross < 0 || circle < 0 || (ar ? cross <= circle : cross >= circle)) { std::cerr << "FAIL: page footer order (" << code << ")\n"; return -1; }
            }
            std::cout << "Settings pages " << code << ": 14 screens, " << cut << " text(s) fitted\n";

            // 0.40: the console limits and platform rules as the settings browser shows them (util/ps4_cfg_browser.hpp
            // help_of, judge, note_of; the page view adds a note after the help): VSync locked on Vulkan, Resolution Scale
            // above 200 %, OpenGL's shader mode, the renderer values (OpenGL warned, and its question), the Frame Skip and
            // per-executable notes, Audio Renderer Null replaced by Cubeb (the saved value without effect, then Square),
            // Enable Buffering off asks first, Mono greyed, an unavailable row with a saved value, and the Triangle option
            // 'Usar meu valor salvo (X)' with its result. What the page would cut of the selected line, the help, the
            // message or the list is listed as 'cut' and counted, as for the rows above; a key missing from its tab fails.
            unsigned limit_cut = 0;
            bool limit_key_missing = false;
            const auto strict = [&](const settings_page& p, const char* screen)
            {
                const page_line& l = p.lines[p.selected];
                if (l.caption) { limit_key_missing = true; std::cerr << "FAIL: " << screen << ": the selected line is a caption (the key is not in the tab)\n"; return; }
                // the page's rule (as in 'widths' above)
                const float bw = l.badge.empty() ? 0 : typeface.measure(l.badge, 17) + 16 + (l.padlock ? 18 : 0);
                const float room = 1110 - 170 - 20 - (bw > 0 ? bw + 14 : 0);
                const float value_room = std::max(360.f, room - std::min(typeface.measure(l.label, 21), 560.f));
                const float label_room = std::max(60.f, std::min(560.f, room - std::min(typeface.measure(l.value, 21), value_room)));
                if (typeface.measure(l.value, 21) > value_room) { ++limit_cut; std::cout << "cut " << code << ' ' << screen << ": value '" << l.value << "'\n"; }
                if (typeface.measure(l.label, 21) > label_room) { ++limit_cut; std::cout << "cut " << code << ' ' << screen << ": label '" << l.label << "'\n"; }
                for (const auto& [text, size, width] : {std::tuple{p.help, 17.f, 940.f}, std::tuple{p.message, 20.f, 1160.f}})
                    if (typeface.wrap(text, size, width, 3, ar).size() > 2) { ++limit_cut; std::cout << "cut " << code << ' ' << screen << ": more than two lines: '" << text << "'\n"; }
                if (p.popup)
                {
                    const float choice_room = 540 - 20 - (p.popup->radio || p.popup->checks ? 56 : 30);
                    for (const page_choice& c : p.popup->choices)
                        if (typeface.measure(c.label, 21) > choice_room) { ++limit_cut; std::cout << "cut " << code << ' ' << screen << ": option '" << c.label << "'\n"; }
                    if (typeface.measure(p.popup->title, 18) > 540 - 60) { ++limit_cut; std::cout << "cut " << code << ' ' << screen << ": heading '" << p.popup->title << "'\n"; }
                }
            };
            const auto run_limit = [&](const settings_page& p, const char* screen)
            {
                strict(p, screen);
                return run_page(p, screen);
            };
            const auto first = [](std::vector<sample_row> mine, const std::vector<sample_row>& rest)
            {
                mine.insert(mine.end(), rest.begin(), rest.end());
                return mine;
            };
            const auto sentence = [](std::string text)
            {
                if (!text.empty() && text.front() >= 'a' && text.front() <= 'z') text.front() = static_cast<char>(text.front() - 'a' + 'A');
                return text;
            };
            // the launcher's question for a warned value: the setting's name, its reason as a sentence
            const auto warn_dialog = [&](std::string_view key, str reason)
            {
                return dialog{i18n::tr(str::CFGU_WARN_TITLE), {row_label(key)}, {sentence(i18n::tr(reason))}, -1, {}, {}, false, nullptr,
                    {cross_button(i18n::tr(str::CFGU_FOOTER_CHANGE)), circle_button(i18n::tr(str::COM_CANCELAR))}};
            };
            const std::string unavailable = i18n::tr(str::COM_INDISPONIVEL_PS4), dot = " \xC2\xB7 ";
            // 1 VSync on Vulkan: locked with the reason (X on it shows the reason too)
            {
                const std::vector<page_line> lines = tab_lines(cat::tab::gpu, first({{"Video > VSync Mode", value_label("Video > VSync Mode", "Disabled"), mark::ps4, true}}, gpu_global));
                const std::string reason = unavailable + i18n::tr(str::CFGR_VSYNC_FIFO_PS4);
                if (!run_limit(page_of(cat::tab::gpu, scope_all, lines, "Video > VSync Mode", reason, reason), "cfg_vsync_locked")) return -1;
            }
            // 2 Resolution Scale above 200 % in the file: the boot runs 200 %
            {
                const std::vector<page_line> lines = tab_lines(cat::tab::gpu, first({{"Video > Resolution Scale", "200 %", mark::ps4}}, gpu_global));
                if (!run_limit(page_of(cat::tab::gpu, scope_all, lines, "Video > Resolution Scale", i18n::trf(str::CFGR_LIMIT_RESOLUTION_SCALE, {200u}), {}),
                        "cfg_resscale_limit"))
                    return -1;
            }
            // 3 OpenGL without gl-async.flag: every shader mode runs as the legacy recompiler
            {
                const std::vector<page_line> lines = tab_lines(cat::tab::gpu, first({{"Video > Renderer", "OpenGL", mark::mine},
                    {"Video > Shader Mode", value_label("Video > Shader Mode", "Legacy Recompiler (single-threaded)"), mark::ps4}}, gpu_global));
                if (!run_limit(page_of(cat::tab::gpu, scope_all, lines, "Video > Shader Mode", i18n::tr(str::CFGR_VALUE_GL_ASYNC), {}), "cfg_opengl_shader_mode")) return -1;
            }
            // 4 the renderer values (the setting's order): OpenGL under the cursor, warned for its own reason; X asks first
            {
                page_popup popup;
                popup.title = row_label("Video > Renderer");
                for (const char* spelling : {"Null", "OpenGL", "Vulkan"})
                    popup.choices.push_back({value_label("Video > Renderer", spelling), false, std::string_view(spelling) == "Vulkan"});
                popup.selected = 1;
                settings_page p = page_of(cat::tab::gpu, scope_all, gpu_lines, "Video > Renderer", i18n::tr(str::CFGH_AVISO_OPENGL_ZINK), {});
                p.buttons = popup_buttons;
                p.footer_help.clear();
                p.popup = &popup;
                if (!run_limit(p, "cfg_picker_renderer_opengl")) return -1;
                if (!run_dialog(warn_dialog("Video > Renderer", str::CFGH_AVISO_OPENGL_ZINK), "cfg_renderer_opengl_warn")) return -1;
            }
            // 5 Enable Frame Skip: the help with its note
            {
                const cat::item* item = cat::find("Video > Enable Frame Skip");
                const std::string help = std::string(item ? i18n::tr(item->help) : "") + dot + i18n::tr(str::CFGH_NOTA_FRAME_SKIP);
                if (!run_limit(page_of(cat::tab::gpu, scope_all, gpu_lines, "Video > Enable Frame Skip", help, {}), "cfg_frame_skip_note")) return -1;
            }
            const std::vector<page_line> audio_lines = tab_lines(cat::tab::audio, {{"Audio > Renderer", "Cubeb"}, {"Audio > Enable Buffering", i18n::tr(str::COM_LIGADO)},
                {"Audio > Audio Channel Layout", value_label("Audio > Audio Channel Layout", "Automatic")}, {"Audio > Audio Provider", value_label("Audio > Audio Provider", "CellAudio")}});
            // 6 Audio Renderer Null in the file runs as Cubeb: why, and that Square removes the saved value; after Square
            // the row's own reason again
            {
                const cat::item* renderer = cat::find("Audio > Renderer");
                const std::string help = unavailable + i18n::tr(str::CFGR_AUDIO_NULL_PS4) + dot + i18n::tr(str::CFGU_VALOR_SALVO_SEM_EFEITO);
                if (!run_limit(page_of(cat::tab::audio, scope_all, audio_lines, "Audio > Renderer", help, {}), "cfg_audio_renderer_null")) return -1;
                const std::string after = unavailable + (renderer ? i18n::tr(renderer->reason) : "");
                if (!run_limit(page_of(cat::tab::audio, scope_all, audio_lines, "Audio > Renderer", after,
                        i18n::trf(str::CFGU_RESET_AUTO_DONE, {i18n::raw(row_label("Audio > Renderer")), i18n::raw("Cubeb")})), "cfg_audio_renderer_removed"))
                    return -1;
            }
            // 7 Enable Buffering off asks first
            if (!run_dialog(warn_dialog("Audio > Enable Buffering", str::CFGH_AVISO_SEM_BUFFER), "cfg_buffering_warn")) return -1;
            // 8 the channel layouts: Mono greyed, its reason under the cursor, X refused
            {
                page_popup popup;
                popup.title = row_label("Audio > Audio Channel Layout");
                for (const char* spelling : {"Automatic", "Mono", "Stereo", "Stereo LFE", "Quadraphonic", "Quadraphonic LFE", "Surround 5.1", "Surround 7.1"})
                    popup.choices.push_back({value_label("Audio > Audio Channel Layout", spelling), std::string_view(spelling) == "Mono", std::string_view(spelling) == "Automatic"});
                popup.selected = 1;
                const std::string reason = i18n::tr(str::CFGR_AUDIO_MONO_PS4);
                settings_page p = page_of(cat::tab::audio, scope_all, audio_lines, "Audio > Audio Channel Layout", reason,
                    i18n::trf(str::CFGU_VALUE_REFUSED, {i18n::raw(popup.choices[1].label), i18n::raw(reason)}));
                p.buttons = popup_buttons;
                p.footer_help.clear();
                p.popup = &popup;
                if (!run_limit(p, "cfg_picker_mono")) return -1;
            }
            // 9 Audio Provider: the help with its note
            {
                const cat::item* item = cat::find("Audio > Audio Provider");
                const std::string help = std::string(item ? i18n::tr(item->help) : "") + dot + i18n::tr(str::CFGH_NOTA_POR_EXECUTAVEL);
                if (!run_limit(page_of(cat::tab::audio, scope_all, audio_lines, "Audio > Audio Provider", help, {}), "cfg_audio_provider_note")) return -1;
            }
            // 10 an unavailable row whose file holds a value it cannot use
            {
                const cat::item* mtrsx = cat::find("Video > Multithreaded RSX");
                const std::string help = unavailable + (mtrsx ? i18n::tr(mtrsx->reason) : "") + dot + i18n::tr(str::CFGU_VALOR_SALVO_SEM_EFEITO);
                if (!run_limit(page_of(cat::tab::gpu, scope_all, gpu_lines, "Video > Multithreaded RSX", help, {}), "cfg_unavailable_saved")) return -1;
            }
            // 11 Triangle on a row whose file holds another value than the one the boot runs: 'Usar meu valor salvo (30)'
            // after 'Fixar este valor'; X keeps the saved value as the player's own
            {
                const std::string automatic = value_label("Video > Frame limit", "Auto"), saved = value_label("Video > Frame limit", "30");
                const std::vector<page_line> lines = tab_lines(cat::tab::gpu, first({{"Video > Frame limit", automatic, mark::automatic}}, gpu_global));
                page_popup popup;
                popup.title = row_label("Video > Frame limit");
                popup.radio = false;
                for (const std::string& label : {std::string(i18n::tr(str::CFGU_OPT_BACK_AUTO)), std::string(i18n::tr(str::CFGU_OPT_UPSTREAM)), std::string(i18n::tr(str::CFGU_OPT_PIN)),
                         i18n::trf(str::SET_USAR_VALOR_SALVO, {i18n::raw(saved)}), std::string(i18n::tr(str::CFGU_OPT_FULL_HELP)), std::string(i18n::tr(str::CFGU_OPT_YAML_KEY))})
                    popup.choices.push_back({label, false, false});
                popup.selected = 3;
                settings_page p = page_of(cat::tab::gpu, scope_all, lines, "Video > Frame limit", i18n::trf(str::CFGU_NOTE_AUTO, {i18n::raw(automatic), i18n::raw(saved)}), {});
                p.buttons = popup_buttons;
                p.footer_help.clear();
                p.popup = &popup;
                if (!run_limit(p, "cfg_options_use_saved")) return -1;
                const std::vector<page_line> mine = tab_lines(cat::tab::gpu, first({{"Video > Frame limit", saved, mark::mine}}, gpu_global));
                if (!run_limit(page_of(cat::tab::gpu, scope_all, mine, "Video > Frame limit", i18n::tr(str::CFGU_NOTE_MINE),
                        i18n::trf(str::CFGU_PINNED, {i18n::raw(row_label("Video > Frame limit"))}) + dot + saved), "cfg_use_saved_done"))
                    return -1;
            }
            std::cout << "Settings limits " << code << ": 14 screens, " << limit_cut << " text(s) cut\n";
            if (limit_key_missing) return -1;
        }
        // Cheats: the game row's hint (R2), the first row of the patch list, the list (its header with the codes for
        // another version and the RPCN warning), an invalid cheat, the options of the console's own cheat and of a PC
        // entry, 'Ver linhas', the delete question, an empty list and its help. Every row of the lists is measured: the
        // title in the room right of the icon, the help in two lines, the message line in 1160 px.
        {
            namespace cs = cheat_screens;
            auto images = firmware_images;
            images.insert_or_assign(cs::switch_on, cs::switch_icon(true));
            images.insert_or_assign(cs::switch_off, cs::switch_icon(false));
            unsigned cut = 0;
            const auto fits = [&](const std::vector<entry>& list, std::string_view message, const char* screen)
            {
                constexpr float room = 1240 - (375 + 75);
                for (const entry& e : list)
                {
                    if (typeface.measure(e.title, 27) > room) { ++cut; std::cout << "fit " << code << ' ' << screen << ": title '" << e.title << "' " << typeface.measure(e.title, 27) << " px\n"; }
                    if (typeface.wrap(e.subtitle, 17, room, 3, ar).size() > 2) { ++cut; std::cout << "fit " << code << ' ' << screen << ": help over two lines '" << e.subtitle << "'\n"; }
                }
                if (typeface.measure(message, 20) > 1160) { ++cut; std::cout << "fit " << code << ' ' << screen << ": message " << typeface.measure(message, 20) << " px '" << message << "'\n"; }
            };
            const auto run_list = [&](const std::vector<entry>& list, std::size_t selected, const std::string& message, const char* screen)
            {
                scene m;
                m.resources = images;
                run_scene(m, 6, selected, list, message, false);
                fits(list, message, screen);
                return save(screen);
            };
            const cs::cheats::game_cheats full = cs::sample_game(true), empty = cs::sample_game(false);
            const std::size_t money = cs::row_named(full, "Max money"), weird = cs::row_named(full, "Weird code"), pointer = cs::row_named(full, "Pointer HP"),
                hp = cs::row_named(full, "HP");
            // 1 the game column: the hint of a game row names R2
            {
                std::vector<entry> games{{"@savedata", "Utilitário de dados guardados (PS3)", "", "item_tex_ps3util", {}},
                    {"demons.iso", "Demon's Souls", "BLUS30443", "item_tex_ps3game_folder", {}, "BLUS30443"},
                    {"terraria", "Terraria", "BLUS31537", "item_tex_ps3game_folder", {}, "BLUS31537"}};
                if (!run_list(games, 1, cs::game_hint(), "cheats_game_hint")) return -1;
            }
            // 2 the patch list of the game: its cheats first
            {
                std::vector<entry> patches{cs::patch_list_row(1),
                    {"@patch:0", "Disable SPU MLAA", std::string(i18n::tr(str::COM_DESLIGADO)), cs::switch_off, {}},
                    {"@patches:import", i18n::tr(str::LAU_IMPORTAR_PATCHES), i18n::tr(str::LAU_PATCH_IMPORT_HELP), "item_tex_disc_install", {}}};
                if (!run_list(patches, 0, i18n::trf(str::LAU_PATCH_RESUMO, {1, "BLUS30443", "v01.04"}), "cheats_patch_row")) return -1;
            }
            // 3 the list: the header names the codes for another version and the RPCN warning (online on)
            const std::vector<entry> list = cs::rows(&full);
            if (!run_list(list, money, cs::header(full, true), "cheats_list")) return -1;
            // 4 an invalid cheat (unknown code type) with the list's hint
            if (!run_list(list, weird, i18n::tr(str::CHT_DICA_LISTA), "cheats_invalid")) return -1;
            // 5, 6 Square: the options of the console's own cheat (mode row) and of an imported PC entry (value row)
            {
                const std::vector<entry> own = cs::options(full, money), pc = cs::options(full, hp);
                if (!run_list(own, 0, i18n::trf(str::CFGU_CHANGED, {full.rows[money].name, str::CFGU_FOOTER_OPTIONS}), "cheats_options")) return -1;
                std::size_t value_row = 0;
                for (std::size_t i = 0; i < pc.size(); ++i)
                    if (pc[i].path == "@copt:value") value_row = i;
                if (!run_list(pc, value_row, i18n::trf(str::CHT_TECLADO_VALOR, {full.rows[hp].name, cs::cheats::pc_type_names[std::size_t(full.rows[hp].pc_info.type)]}),
                        "cheats_options_pc"))
                    return -1;
            }
            // 7 Ver linhas of the pointer cheat (its lines, why it is refused, the executables)
            {
                std::size_t pages = 0;
                if (!run_dialog(cs::lines_dialog(full, pointer, 0, pages), "cheats_lines")) return -1;
            }
            // 8 Apagar asks first
            if (!run_dialog(cs::delete_question(full.rows[money]), "cheats_delete")) return -1;
            // 9, 10 a game without cheats: the folders, and X on that row
            if (!run_list(cs::rows(&empty), 0, cs::header(empty, false), "cheats_empty")) return -1;
            if (!run_dialog(cs::empty_help(empty.title), "cheats_empty_help")) return -1;
            std::cout << "Cheats " << code << ": 10 screens, " << cut << " text(s) cut\n";
            if (cut) { std::cerr << "FAIL: cheat texts do not fit (" << code << ")\n"; return -1; }
        }
        // 0.40: folders of the 'Jogo' column, as the folders glue builds them, with the folder drawn by
        // util/ps4_folder_icon.hpp (this default run never uses the firmware's folder): the top level, inside a folder,
        // halfway open, the options of a folder and of a game, 'Mover para uma pasta', the checklist, 'Mudar posição', the
        // delete question, an empty folder, a console without firmware. Checked by pixels: the open folder's icon at the
        // column's place; every option list changes nothing left of x 700 (never mirrored in the XMB) and its texts start
        // at x 730, or end at x 1210 right to left.
        unsigned folder_cut = 0;
        const image folder = folder_icon();
        const std::array<const char*, 3> folder_names = i18n::current() == i18n::lang::en ? std::array<const char*, 3>{"Racing", "Fighting", "Favourites"}
            : i18n::current() == i18n::lang::es ? std::array<const char*, 3>{"Carreras", "Lucha", "Favoritos"}
            : i18n::current() == i18n::lang::ar ? std::array<const char*, 3>{"\xD8\xB3\xD8\xA8\xD8\xA7\xD9\x82", "\xD9\x82\xD8\xAA\xD8\xA7\xD9\x84",
                "\xD8\xA7\xD9\x84\xD9\x85\xD9\x81\xD8\xB6\xD9\x84\xD8\xA9"}
            : std::array<const char*, 3>{"Corrida", "Luta", "Favoritos"};
        // The player's names go into the texts as typed: isolated left to right unless they are Arabic
        const auto name_arg = [](const char* name) { return utils::ps4::arabic::has_arabic(name) ? i18n::raw(name) : i18n::arg(name); };
        const entry savedata{"@savedata", i18n::tr(str::LAU_UTILITARIO_DADOS_GUARDADOS), "", "item_tex_ps3util", {}};
        const entry licenses{"@licenses", i18n::tr(str::LAU_IMPORTAR_LICENCAS_RAP), i18n::tr(str::LAU_IMPORTAR_LICENCAS_RAP_HELP), "item_tex_disc_install", {}};
        const std::vector<entry> library{{"/data/rpcs3/games/Gran Turismo 5.iso", "Gran Turismo 5", "BCUS98114", "item_tex_ps3game_folder", {}, "BCUS98114"},
            {"/data/rpcs3/games/MotorStorm Pacific Rift.iso", "MotorStorm: Pacific Rift", "BCUS98137", "item_tex_ps3game_folder", {}, "BCUS98137"},
            {"/mnt/usb0/PS3ISO/Burnout Paradise.iso", "Burnout Paradise", "BLUS30066", "item_tex_ps3game_folder", {}, "BLUS30066"},
            {"/data/rpcs3/games/Tekken 6.iso", "Tekken 6", "BLUS30307", "item_tex_ps3game_folder", {}, "BLUS30307"},
            {"/data/rpcs3/games/Mortal Kombat.iso", "Mortal Kombat", "BLUS30522", "item_tex_ps3game_folder", {}, "BLUS30522"},
            {"demons.iso", "Demon's Souls", "BLUS30443", "item_tex_ps3game_folder", {}, "BLUS30443"},
            {"terraria", "Terraria", "BLUS31537", "item_tex_ps3game_folder", {}, "BLUS31537"},
            {"minecraft.iso", "Minecraft: PlayStation\xC2\xAE" "3 Edition", "BLUS31426", "item_tex_ps3game_folder", {}, "BLUS31426"}};
        const auto folder_row = [&](int id, const char* name, std::size_t games, std::size_t missing)
        {
            std::string count = games == 0 ? std::string(i18n::tr(str::FLD_PASTA_SUB_0)) : games == 1 ? std::string(i18n::tr(str::FLD_PASTA_SUB_1))
                : i18n::trf(str::FLD_PASTA_SUB_N, {games});
            if (missing) count += i18n::trf(str::FLD_AUSENTES_SUFIXO, {missing});
            return entry{"@folder:" + std::to_string(id), name, std::move(count), "item_tex_plain_folder", {}};
        };
        // Top level: the utility rows, the folders in the player's order (Corrida 3 games, Luta 2 and 1 not in the library
        // now, Favoritos empty), then the games in no folder
        const std::vector<entry> top{savedata, licenses, folder_row(1, folder_names[0], 3, 0), folder_row(2, folder_names[1], 2, 1),
            folder_row(3, folder_names[2], 0, 0), library[5], library[6], library[7]};
        const std::vector<entry> racing{library[0], library[1], library[2]};
        auto folder_images = firmware_images;
        folder_images.insert_or_assign("item_tex_plain_folder", folder);
        const folder_header racing_header{&folder_images.at("item_tex_plain_folder"), folder_names[0]};
        const std::string options_hint = i18n::tr(str::FLD_OPCOES);
        // The room of a row's title and help (right of the icon), of a message line and of an option list's labels
        const auto folder_fits = [&](const std::vector<entry>& list, std::string_view message, const page_popup* popup, const char* screen)
        {
            constexpr float room = 1240 - (375 + 75);
            for (const entry& e : list)
            {
                if (typeface.measure(e.title, 27) > room) { ++folder_cut; std::cout << "fit " << code << ' ' << screen << ": title '" << e.title << "'\n"; }
                if (typeface.wrap(e.subtitle, 17, room, 3, ar).size() > 2) { ++folder_cut; std::cout << "fit " << code << ' ' << screen << ": help over two lines '" << e.subtitle << "'\n"; }
            }
            if (typeface.measure(message, 20) > 1160) { ++folder_cut; std::cout << "fit " << code << ' ' << screen << ": message '" << message << "'\n"; }
            if (popup)
            {
                const float label_room = 540 - 20 - (popup->radio || popup->checks ? 56 : 30);
                for (const page_choice& choice : popup->choices)
                    if (typeface.measure(choice.label, 21) > label_room) { ++folder_cut; std::cout << "fit " << code << ' ' << screen << ": option '" << choice.label << "'\n"; }
                if (typeface.measure(popup->title, 18) > 540 - 60) { ++folder_cut; std::cout << "fit " << code << ' ' << screen << ": heading '" << popup->title << "'\n"; }
            }
        };
        const auto note = [&](const std::vector<entry>& list, std::string_view message, const column_extras& extras)
        {
            for (const entry& e : list) { drawn.push_back(e.title); drawn.push_back(e.subtitle); }
            drawn.emplace_back(message);
            drawn.emplace_back(extras.triangle_hint);
            if (extras.header) drawn.push_back(extras.header->title);
            if (extras.popup)
            {
                drawn.push_back(extras.popup->title);
                for (const page_choice& choice : extras.popup->choices) drawn.push_back(choice.label);
            }
        };
        const auto draw_column = [&](scene& m, const std::vector<entry>& list, std::size_t selected, std::string_view message, const column_extras& extras,
            const font& face, int frames)
        {
            for (int i = 0; i < frames; ++i)
                m.draw(frame.data(), face, 6, selected, list, 3. + i / 60., 1.f / 60, "01/10  16:53", message, false, {}, &extras);
        };
        const auto run_column = [&](const std::vector<entry>& list, std::size_t selected, const std::string& message, const column_extras& extras, const char* screen)
        {
            scene m;
            m.resources = folder_images;
            draw_column(m, list, selected, message, extras, typeface, 40);
            note(list, message, extras);
            folder_fits(list, message, extras.popup, screen);
            if (!save(screen)) return false;
            if (!extras.popup) return true;
            // The same screen without the list: every pixel it changed is right of x 700
            const std::vector<std::uint32_t> shown = frame;
            column_extras bare = extras;
            bare.popup = nullptr;
            scene plain;
            plain.resources = folder_images;
            draw_column(plain, list, selected, message, bare, typeface, 40);
            int left = 1280;
            for (int y = 0; y < 720; ++y)
                for (int x = 0; x < left; ++x)
                    if (frame[std::size_t(y) * 1280 + std::size_t(x)] != shown[std::size_t(y) * 1280 + std::size_t(x)]) { left = x; break; }
            if (left < 700) { std::cerr << "FAIL: option list drawn left of x 700 (" << code << ' ' << screen << ": x " << left << ")\n"; return false; }
            if (!extras.popup->checks && !extras.popup->radio)
            {
                // The first label: from x 730, or ending at x 1210 right to left
                int ink_left = 1280, ink_right = -1;
                for (int y = 148; y < 184; ++y)
                    for (int x = 700; x < 1240; ++x)
                    {
                        const std::uint32_t p = shown[std::size_t(y) * 1280 + std::size_t(x)];
                        if (((p >> 16) & 255) > 200 && ((p >> 8) & 255) > 200 && (p & 255) > 200) { ink_left = std::min(ink_left, x); ink_right = std::max(ink_right, x); }
                    }
                std::cout << "option list " << code << ' ' << screen << ": changes from x " << left << ", first label ink x " << ink_left << ".." << ink_right << '\n';
                if (ink_right < 0 || (ar ? std::abs(ink_right - 1209) > 5 : std::abs(ink_left - 730) > 5))
                {
                    std::cerr << "FAIL: option list label alignment (" << code << ' ' << screen << ")\n";
                    return false;
                }
            }
            frame = shown;
            return true;
        };
        // 1 top level, a folder selected: its hint, Triangle names the options
        {
            const column_extras extras{nullptr, nullptr, options_hint};
            if (!run_column(top, 2, i18n::tr(str::FLD_DICA_PASTA), extras, "folders_top")) return -1;
            // Empty extras draw exactly what no extras draw (the launcher passes them only in the 'Jogo' column)
            const column_extras none{};
            scene with, without;
            with.resources = without.resources = folder_images;
            draw_column(with, top, 2, "", none, typeface, 40);
            const std::vector<std::uint32_t> drawn_with = frame;
            for (int i = 0; i < 40; ++i) without.draw(frame.data(), typeface, 6, 2, top, 3. + i / 60., 1.f / 60, "01/10  16:53", "", false);
            if (frame != drawn_with) { std::cerr << "FAIL: empty column extras change the frame (" << code << ")\n"; return -1; }
        }
        // 2 inside the folder: the folder at the column's place, its games
        {
            const column_extras extras{&racing_header, nullptr, options_hint};
            if (!run_column(racing, 0, i18n::tr(str::LAU_HINT_GAME_ROW), extras, "folders_inside")) return -1;
            // The header icon (a pure colour stand-in) at x 318..432, y 158..222
            auto marked = folder_images;
            const image magenta{314, 178, std::vector<std::uint32_t>(std::size_t(314) * 178, 0xffff00ffu)};
            const folder_header marked_header{&magenta, folder_names[0]};
            const column_extras marked_extras{&marked_header, nullptr, options_hint};
            scene m;
            m.resources = marked;
            draw_column(m, racing, 0, "", marked_extras, typeface, 40);
            int x0 = 1280, y0 = 720, x1 = -1, y1 = -1;
            for (int y = 0; y < 720; ++y)
                for (int x = 0; x < 1280; ++x)
                {
                    const std::uint32_t p = frame[std::size_t(y) * 1280 + std::size_t(x)];
                    if (((p >> 16) & 255) > 200 && ((p >> 8) & 255) < 60 && (p & 255) > 200) { x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y); }
                }
            std::cout << "folder header " << code << ": icon ink x " << x0 << ".." << x1 << ", y " << y0 << ".." << y1 << '\n';
            if (std::abs(x0 - 318) > 1 || std::abs(x1 - 432) > 1 || std::abs(y0 - 158) > 1 || std::abs(y1 - 222) > 1)
            {
                std::cerr << "FAIL: folder header icon placement (" << code << ")\n";
                return -1;
            }
        }
        // 3 halfway open (depth 0.5): the bar half a slot left, the folder and its name fading in, the games coming in
        {
            scene m;
            m.resources = folder_images;
            const column_extras closed{nullptr, nullptr, options_hint}, open{&racing_header, nullptr, options_hint};
            draw_column(m, top, 2, "", closed, typeface, 40);
            // one frame of 0.065 * ln 2 s eases the depth from 0 to 0.5
            m.draw(frame.data(), typeface, 6, 0, racing, 3.7, .0450561f, "01/10  16:53", "", false, {}, &open);
            note(racing, "", open);
            if (!save("folders_opening")) return -1;
        }
        // 4 the options of a folder with games not in the library now
        {
            page_popup popup;
            popup.title = folder_names[1];
            popup.radio = false;
            for (const std::string& label : {std::string(i18n::tr(str::FLD_ABRIR)), std::string(i18n::tr(str::FLD_ADICIONAR)), std::string(i18n::tr(str::FLD_RENOMEAR)),
                     std::string(i18n::tr(str::FLD_POSICAO)), std::string(i18n::tr(str::FLD_APAGAR)), i18n::trf(str::FLD_ESQUECER, {1u}),
                     std::string(i18n::tr(str::FLD_CRIAR)), std::string(i18n::tr(str::LAU_ATUALIZAR_BIBLIOTECA))})
                popup.choices.push_back({label, false, false});
            const column_extras extras{nullptr, &popup, options_hint};
            if (!run_column(top, 3, "", extras, "folders_options_folder")) return -1;
        }
        // 5 the options of a game in no folder
        {
            page_popup popup;
            popup.title = library[5].title;
            popup.radio = false;
            for (const str id : {str::FLD_MOVER, str::FLD_CRIAR, str::LAU_ATUALIZAR_BIBLIOTECA}) popup.choices.push_back({i18n::tr(id), false, false});
            const column_extras extras{nullptr, &popup, options_hint};
            if (!run_column(top, 5, "", extras, "folders_options_game")) return -1;
        }
        // 6 'Mover para uma pasta': the folders and a new one
        {
            page_popup popup;
            popup.title = i18n::trf(str::FLD_MOVER_TITULO, {library[5].title});
            popup.radio = false;
            for (const char* name : folder_names) popup.choices.push_back({name, false, false});
            popup.choices.push_back({i18n::tr(str::FLD_NOVA_PASTA_ITEM), false, false});
            popup.selected = 2;
            const column_extras extras{nullptr, &popup, options_hint};
            if (!run_column(top, 5, "", extras, "folders_move")) return -1;
        }
        // 7 'Adicionar jogos': every game of the library, checked = in this folder, another folder's name after a title
        {
            page_popup popup;
            popup.title = i18n::trf(str::FLD_ADICIONAR_TITULO, {name_arg(folder_names[2])});
            popup.checks = true;
            for (std::size_t g = 0; g < library.size(); ++g)
            {
                std::string label = library[g].title;
                if (g < 3) label += std::string(" (") + folder_names[0] + ")";
                else if (g < 5) label += std::string(" (") + folder_names[1] + ")";
                popup.choices.push_back({std::move(label), false, g == 5 || g == 6});
            }
            popup.selected = 6;
            const column_extras extras{nullptr, &popup, options_hint};
            if (!run_column(top, 4, i18n::tr(str::FLD_ADICIONAR_DICA), extras, "folders_checklist")) return -1;
        }
        // 8 'Mudar posição': the folder lifted among the folders
        {
            column_extras extras{nullptr, nullptr, options_hint};
            extras.lifted = 3;
            if (!run_column(top, 3, i18n::tr(str::FLD_POSICAO_DICA), extras, "folders_position")) return -1;
        }
        // 9 'Apagar pasta' asks first: no game is deleted
        if (!run_dialog({{}, {i18n::trf(str::FLD_APAGAR_PERGUNTA, {name_arg(folder_names[0])})}, {i18n::tr(str::FLD_APAGAR_NOTA)}, -1, {}, {}, false, &folder,
                {cross_button(i18n::tr(str::COM_SIM)), circle_button(i18n::tr(str::COM_NAO))}}, "folders_delete"))
            return -1;
        // 10 inside an empty folder: its one row, X shows its help
        {
            const folder_header header{&folder_images.at("item_tex_plain_folder"), folder_names[2]};
            const std::vector<entry> empty{{"@folder-empty:3", i18n::tr(str::FLD_VAZIA), i18n::tr(str::FLD_VAZIA_HELP), "item_tex_plain_folder", {}}};
            const column_extras extras{&header, nullptr, options_hint};
            if (!run_column(empty, 0, i18n::tr(str::FLD_VAZIA_HELP), extras, "folders_empty")) return -1;
        }
        // 11 no firmware: the bundled font, grey boxes for the category and game icons, the drawn folder
        {
            const auto dejavu = ::read(std::filesystem::path(__FILE__).parent_path() / "../../bin/fonts/DejaVuSans.ttf");
            font fallback;
            if (dejavu.empty() || !fallback.load(dejavu, dejavu)) { std::cerr << "FAIL: no DejaVu font for the no-firmware folder screen\n"; return -1; }
            scene m;
            m.resources.insert_or_assign("item_tex_plain_folder", folder);
            const column_extras extras{nullptr, nullptr, options_hint};
            draw_column(m, top, 2, i18n::tr(str::FLD_DICA_PASTA), extras, fallback, 40);
            std::size_t missing_fallback = fallback.missing_glyphs(i18n::tr(str::FLD_DICA_PASTA), ar) + fallback.missing_glyphs(options_hint, ar);
            for (const entry& e : top) missing_fallback += fallback.missing_glyphs(e.title, ar) + fallback.missing_glyphs(e.subtitle, ar);
            if (missing_fallback) { std::cerr << "FAIL: " << missing_fallback << " glyph(s) drawn as '?' with the bundled font (" << code << ")\n"; return -1; }
            if (!save("folders_no_firmware")) return -1;
        }
        std::cout << "Folders " << code << ": 11 screens, " << folder_cut << " text(s) cut\n";
        if (folder_cut) { std::cerr << "FAIL: folder texts do not fit (" << code << ")\n"; return -1; }

        // 0.40: the selected game's background and overlay (synthetic images): the game row at strength 0, 128 and 255,
        // each without and with the overlay, a cross-fade between two backgrounds, a dialog over a background; then the
        // two rows of the Configurações column in their six states. Strength 0 must leave the frame as without media.
        {
            const image pic1 = media_samples::background(0), pic1_night = media_samples::background(1), pic0 = media_samples::overlay();
            const int ov_x = 1280 - pic0.width, ov_y = (720 - pic0.height) / 2;
            const std::vector<entry> games{savedata, library[5], library[6]};
            const std::string hint = i18n::tr(str::LAU_HINT_GAME_ROW);
            const auto media_frame = [&](scene& m)
            {
                for (int i = 0; i < 40; ++i) m.draw(frame.data(), typeface, 6, 1, games, 3. + i / 60., 1.f / 60, "01/10  16:53", hint, false);
            };
            std::vector<std::uint32_t> waves;
            {
                scene m;
                m.resources = firmware_images;
                media_frame(m);
                waves = frame;
            }
            for (const unsigned strength : {0u, 128u, 255u})
                for (const bool with_overlay : {false, true})
                {
                    scene m;
                    m.resources = firmware_images;
                    m.set_media(pic1, with_overlay ? pic0 : image{}, ov_x, ov_y);
                    m.set_media_alpha(strength, 0, with_overlay ? 255 : 0);
                    media_frame(m);
                    if (strength == 0 && !with_overlay && frame != waves) { std::cerr << "FAIL: a background at strength 0 changed the frame (" << code << ")\n"; return -1; }
                    const std::string screen = "media_bg_" + std::to_string(strength) + (with_overlay ? "_overlay" : "");
                    if (!save(screen.c_str())) return -1;
                }
            {
                // the night background fading out while the evening one fades in
                scene m;
                m.resources = firmware_images;
                m.set_media(pic1_night, {}, 0, 0);
                m.set_media_alpha(255, 0, 0);
                m.set_media(pic1, {}, 0, 0);
                m.set_media_alpha(96, 160, 0);
                media_frame(m);
                if (!save("media_crossfade")) return -1;
            }
            {
                // Released before a boot: clear_media() frees every image and the XMB draws exactly as a scene that never
                // had media; images of the wrong size are never kept
                scene never, cleared;
                never.resources = cleared.resources = firmware_images;
                cleared.set_media(pic1, pic0, ov_x, ov_y);
                cleared.set_media_alpha(255, 0, 255);
                media_frame(never);
                media_frame(cleared);
                const std::size_t held = cleared.media_bytes(), freed = cleared.clear_media();
                const std::size_t expected = (std::size_t(1280) * 720 + std::size_t(pic0.width) * std::size_t(pic0.height)) * 4;
                if (held < expected || freed != held || cleared.media_bytes())
                {
                    std::cerr << "FAIL: clear_media freed " << freed << " of " << held << " bytes (" << code << ")\n";
                    return -1;
                }
                media_frame(never);
                const std::vector<std::uint32_t> plain = frame;
                media_frame(cleared);
                if (frame != plain) { std::cerr << "FAIL: the XMB after clear_media differs from one without media (" << code << ")\n"; return -1; }
                scene odd;
                odd.set_media(image{640, 360, std::vector<std::uint32_t>(std::size_t(640) * 360, 0xff102030u)},
                    image{1300, 10, std::vector<std::uint32_t>(std::size_t(1300) * 10, 0xff102030u)}, 0, 0);
                if (odd.media_bytes()) { std::cerr << "FAIL: a wrongly sized background or overlay was kept (" << code << ")\n"; return -1; }
            }
            {
                // a dialog over a background: the overlay stays out of a dialog's backdrop (the frame is the same with and
                // without it), so the dialog's texts never sit on the game's title art
                const dialog d{"FIFA 14 (BLES01876)", {s.disc_question}, {s.disc_size, s.disc_free}, -1, {}, {}, false, nullptr,
                    {cross_button(i18n::tr(str::COM_INSTALAR)), circle_button(i18n::tr(str::COM_VOLTAR))}};
                const auto dialog_over = [&](const image& overlay)
                {
                    scene backdrop;
                    backdrop.resources = firmware_images;
                    backdrop.set_media(pic1, overlay, ov_x, ov_y);
                    backdrop.set_media_alpha(255, 0, overlay ? 255 : 0);
                    for (int i = 0; i < 20; ++i) backdrop.backdrop(frame.data(), 4. + i / 60.);
                    canvas c(frame.data(), 1280, 720);
                    draw_dialog(c, typeface, backdrop.resources, d, 4.5);
                };
                dialog_over({});
                const std::vector<std::uint32_t> plain = frame;
                dialog_over(pic0);
                if (frame != plain) { std::cerr << "FAIL: the overlay was drawn under a dialog (" << code << ")\n"; return -1; }
                if (!save("media_dialog")) return -1;
            }
            // The rows after the language row: 'Fundo e música do jogo' and 'Música do jogo'
            unsigned media_cut = 0;
            const auto media_rows = [&](bool media_on, str music, std::size_t selected, std::string message, const char* screen)
            {
                std::vector<entry> rows{{"@rpcs3", s.rpcs3_settings, s.rpcs3_settings_help, "tex_sysconf", {}},
                    {"@ui-language", i18n::tr(str::LANG_ROW_TITLE), i18n::trf(str::LANG_ROW_HELP, {i18n::raw(i18n::setting_label(i18n::current_setting()))}), "tex_sysconf", {}},
                    {"@xmb-media", i18n::trf(str::LAU_XMED_ROW, {media_on ? str::COM_LIGADO : str::COM_DESLIGADO}), i18n::tr(str::LAU_XMED_ROW_HELP), "tex_sysconf", {}},
                    {"@xmb-music", i18n::trf(str::LAU_XMED_MUSICA_ROW, {music}), i18n::tr(media_on ? str::LAU_XMED_MUSICA_HELP : str::LAU_XMED_MUSICA_HELP_SEM_FUNDO),
                        "tex_sysconf", {}},
                    {"@storage", i18n::tr(str::COM_ARMAZENAMENTO_JOGOS), s.storage_help, "tex_sysconf", {}}};
                scene m;
                m.resources = firmware_images;
                run_scene(m, 1, selected, rows, message, false);
                constexpr float room = 1240 - (375 + 75);
                for (const entry& e : rows)
                {
                    if (typeface.measure(e.title, 27) > room) { ++media_cut; std::cout << "fit " << code << ' ' << screen << ": title '" << e.title << "'\n"; }
                    if (typeface.wrap(e.subtitle, 17, room, 3, ar).size() > 2) { ++media_cut; std::cout << "fit " << code << ' ' << screen << ": help over two lines '" << e.subtitle << "'\n"; }
                }
                if (typeface.measure(message, 20) > 1160) { ++media_cut; std::cout << "fit " << code << ' ' << screen << ": message '" << message << "'\n"; }
                return save(screen);
            };
            if (!media_rows(true, str::LAU_XMED_LIGADA, 2, i18n::tr(str::LAU_XMED_LIGADO_MSG), "media_rows_on")) return -1;
            if (!media_rows(false, str::LAU_XMED_LIGADA, 2, i18n::tr(str::LAU_XMED_DESLIGADO_MSG), "media_rows_off")) return -1;
            if (!media_rows(true, str::LAU_XMED_LIGADA, 3, {}, "media_music_on")) return -1;
            if (!media_rows(true, str::LAU_XMED_BAIXA, 3, {}, "media_music_low")) return -1;
            if (!media_rows(true, str::LAU_XMED_DESLIGADA, 3, {}, "media_music_off")) return -1;
            if (!media_rows(false, str::LAU_XMED_LIGADA, 3, {}, "media_music_no_background")) return -1;
            std::cout << "Media " << code << ": 14 screens, " << media_cut << " text(s) cut\n";
            if (media_cut) { std::cerr << "FAIL: game media texts do not fit (" << code << ")\n"; return -1; }
        }
        // 0.40: the media timeline as the launcher glue drives it. The media director of util/ps4_xmb_media.hpp gets the
        // selected row every frame; a job it posts is answered 120 ms later by a stand-in loader (synthetic images: game A
        // has a background, an overlay and music, game B nothing, game C a background and music). Its outputs go to the
        // scene in the glue's order: present -> set_media (before the music starts), the strengths -> set_media_alpha(bg,
        // 0, ov) every frame, clear -> clear_media(). A second scene that never gets media draws the same frames: while the
        // scene holds no media, or both strengths are 0, the two frames must be byte-identical. Then a disc dialog (the
        // background stays under it, the music stops and starts again after it), the Configurações column (the media
        // fades out and is freed) and the release before a boot (every image freed, the director sealed).
        {
            namespace xm = utils::ps4::xmb_media;
            struct payload
            {
                std::uint32_t generation = 0;
                image background, overlay;
                int overlay_x = 0, overlay_y = 0;
                bool music_file = false, track = false, only_music = false;
                bool has_background() const { return static_cast<bool>(background); }
                bool has_overlay() const { return static_cast<bool>(overlay); }
                bool has_music() const { return track; }
                bool has_music_file() const { return music_file; }
                bool music_only() const { return only_music; }
            };
            const xm::rect box = xm::overlay_box(1000, 560);
            const image evening = media_samples::background(0), night = media_samples::background(1), title_art = media_samples::overlay();
            if (title_art.width != box.w || title_art.height != box.h) { std::cerr << "FAIL: the overlay stand-in does not have the size of a scaled PIC0\n"; return -1; }
            const std::vector<entry> games{savedata, library[5], library[6], library[7]};
            // the first rows of the launcher's Configurações column, 'Fundo e música do jogo' selected
            const std::vector<entry> settings_rows{{"@info", i18n::tr(str::LAU_INFORMACOES_SISTEMA), "", "tex_sysconf", {}},
                {"@settings", i18n::tr(str::LAU_CONFIGURACOES_RPCS3), i18n::tr(str::LAU_CONFIGURACOES_RPCS3_HELP), "tex_sysconf", {}},
                {"@ui-language", i18n::tr(str::LANG_ROW_TITLE), i18n::trf(str::LANG_ROW_HELP, {i18n::raw(i18n::setting_label(i18n::current_setting()))}), "tex_sysconf", {}},
                {"@xmb-media", i18n::trf(str::LAU_XMED_ROW, {str::COM_LIGADO}), i18n::tr(str::LAU_XMED_ROW_HELP), "tex_sysconf", {}},
                {"@xmb-music", i18n::trf(str::LAU_XMED_MUSICA_ROW, {str::LAU_XMED_LIGADA}), i18n::tr(str::LAU_XMED_MUSICA_HELP), "tex_sysconf", {}},
                {"@refresh", i18n::tr(str::LAU_ATUALIZAR_BIBLIOTECA), "", "item_tex_plain_folder", {}},
                {"@patches:import", i18n::tr(str::LAU_IMPORTAR_PATCHES_JOGOS), "", "item_tex_disc_install", {}}};
            const dialog disc{games[3].title + " (" + games[3].serial + ")", {s.disc_question}, {s.disc_size, s.disc_free}, -1, {}, {}, false, nullptr,
                {cross_button(i18n::tr(str::COM_INSTALAR)), circle_button(i18n::tr(str::COM_VOLTAR))}};
            for (const entry& e : settings_rows) { drawn.push_back(e.title); drawn.push_back(e.subtitle); }
            drawn.push_back(disc.title);
            const std::string hint = i18n::tr(str::LAU_HINT_GAME_ROW);
            // the stand-in loader: what each game's own files give
            const auto job = [&](const xm::media_request& r)
            {
                auto p = std::make_shared<payload>();
                p->generation = r.generation;
                p->only_music = !r.want_images;
                const bool a = r.path == games[1].path, c = r.path == games[3].path;
                if (r.want_images && a)
                {
                    p->background = evening;
                    p->overlay = title_art;
                    p->overlay_x = box.x;
                    p->overlay_y = box.y;
                }
                if (r.want_images && c) p->background = night;
                p->music_file = a || c;
                p->track = (a || c) && r.want_music;
                return p;
            };
            struct pending
            {
                std::uint64_t ready;
                xm::media_request request;
            };
            struct shot
            {
                std::uint64_t at_ms;
                const char* screen;
                unsigned bg_lo, bg_hi, ov_lo, ov_hi;
            };
            // when each picture is taken, and the strengths the director must give then
            static constexpr shot shots[] = {{1033, "media_glue_background_in", 100, 155, 0, 0}, {1633, "media_glue_background", 255, 255, 0, 0},
                {2683, "media_glue_overlay_in", 255, 255, 100, 155}, {3200, "media_glue_full", 255, 255, 255, 255}, {3700, "media_glue_leaving", 100, 160, 30, 80},
                {6900, "media_glue_dialog", 255, 255, 0, 0}, {8200, "media_glue_settings_column", 1, 254, 0, 0}, {10700, "media_glue_again", 255, 255, 0, 0}};
            xm::basic_director<payload> director;
            director.configure(xm::settings{}, false); // background and music on, 16:9, a loader thread
            scene m, plain;
            m.resources = plain.resources = firmware_images;
            // the waves are drawn every frame in both: the 20 Hz layer cache would leave the two at different phases after
            // the scene with media skipped the waves while its background was fully shown (no visible difference)
            m.cached_background = plain.cached_background = false;
            std::vector<std::uint32_t> other(frame.size());
            std::vector<pending> jobs;
            unsigned posts = 0, presents = 0, plays = 0, stops = 0, clears = 0, empty_results = 0, identical = 0;
            std::uint64_t modal_stop = 0, modal_play = 0;
            bool music = false, shown_images = false;
            std::size_t next_shot = 0;
            double last = 3. - 1. / 60;
            const auto draw_both = [&](bool modal, int category, std::size_t selected, double seconds)
            {
                const float elapsed = float(seconds - last);
                last = seconds;
                for (scene* target : {&m, &plain})
                {
                    std::uint32_t* pixels = target == &m ? frame.data() : other.data();
                    if (modal)
                    {
                        // a disc question (disc_ui::ask) over the XMB's backdrop
                        target->backdrop(pixels, seconds);
                        canvas c(pixels, 1280, 720);
                        draw_dialog(c, typeface, target->resources, disc, seconds);
                    }
                    else
                        target->draw(pixels, typeface, category, selected, category == 6 ? games : settings_rows, seconds, elapsed, "01/10  16:53",
                            category == 6 ? std::string_view(hint) : std::string_view{}, false);
                }
            };
            // phases (ms): game A from 0, B from 3500, C from 4800, the disc question 6600..7300, the Configurações column
            // 8000..9000, C again, the boot at 10800
            for (std::uint64_t k = 0;; ++k)
            {
                const std::uint64_t now = k * 1'000'000 / 60, ms = now / 1000;
                if (ms >= 10800) break;
                const bool modal = ms >= 6600 && ms < 7300;
                const int category = ms >= 8000 && ms < 9000 ? 1 : 6;
                const std::size_t selected = category == 1 ? 3 : ms < 3500 ? 1 : ms < 4800 ? 2 : 3;
                // the loader's results first, then the director
                for (std::size_t i = 0; i < jobs.size();)
                {
                    if (jobs[i].ready > now) { ++i; continue; }
                    if (!director.arrived(job(jobs[i].request))) ++empty_results;
                    jobs.erase(jobs.begin() + std::ptrdiff_t(i));
                }
                const entry& row = category == 6 ? games[selected] : settings_rows[selected];
                xm::director_input in;
                in.row = category == 6 && xm::source_of(row.path) != xm::source_kind::none ? std::string_view(row.path) : std::string_view{};
                in.serial = row.serial;
                in.hold = modal;
                in.visible = category == 6;
                in.music_gate = !modal;
                auto out = director.tick(now, in);
                if (out.drop) jobs.clear();
                if (out.post)
                {
                    ++posts;
                    jobs.push_back({now + 120'000, std::move(*out.post)});
                }
                if (out.present)
                {
                    ++presents;
                    shown_images = out.present->has_background() || out.present->has_overlay();
                    m.set_media(std::move(out.present->background), std::move(out.present->overlay), out.present->overlay_x, out.present->overlay_y);
                }
                if (out.stop_music)
                {
                    ++stops;
                    music = false;
                    if (modal && !modal_stop) modal_stop = ms;
                }
                if (out.play)
                {
                    ++plays;
                    music = true;
                    // a game with images starts its music only once they are in the scene
                    if (shown_images && !m.media_bytes())
                    {
                        std::cerr << "FAIL: the music started before its background was in the scene (" << code << ", " << ms << " ms)\n";
                        return -1;
                    }
                    if (ms >= 7300 && !modal_play) modal_play = ms;
                }
                if (out.clear)
                {
                    ++clears;
                    m.clear_media();
                }
                m.set_media_alpha(out.background_alpha, 0, out.overlay_alpha);
                // drawn at 30 frames per second (the launcher's rate while a fade runs) and at every picture
                if (k % 2 && !(next_shot < std::size(shots) && ms >= shots[next_shot].at_ms)) continue;
                draw_both(modal, category, selected, 3. + double(now) / 1e6);
                if (!m.media_bytes() || (!out.background_alpha && !out.overlay_alpha))
                {
                    if (frame != other) { std::cerr << "FAIL: without media shown the frame differs from the XMB without media (" << code << ", " << ms << " ms)\n"; return -1; }
                    ++identical;
                }
                if (next_shot < std::size(shots) && ms >= shots[next_shot].at_ms)
                {
                    const shot& sh = shots[next_shot++];
                    if (out.background_alpha < sh.bg_lo || out.background_alpha > sh.bg_hi || out.overlay_alpha < sh.ov_lo || out.overlay_alpha > sh.ov_hi)
                    {
                        std::cerr << "FAIL: " << sh.screen << ": strengths " << out.background_alpha << '/' << out.overlay_alpha << " (" << code << ")\n";
                        return -1;
                    }
                    if (!save(sh.screen)) return -1;
                }
            }
            // The release before a boot, in the glue's order: the music, the jobs not started, what the director holds,
            // then the scene's images
            const bool music_at_boot = music;
            jobs.clear();
            unsigned released = 0;
            director.drain([&](std::shared_ptr<payload> p) { released += p ? 1 : 0; });
            const std::size_t held = m.media_bytes(), freed = m.clear_media();
            xm::director_input again;
            again.row = games[3].path;
            again.serial = games[3].serial;
            const auto sealed = director.tick(11'500'000, again);
            draw_both(false, 6, 3, 3. + 10.8);
            std::cout << "Media glue " << code << ": " << posts << " jobs, " << presents << " shown, " << empty_results << " without media, " << plays << " track(s) started, "
                      << stops << " stopped, " << clears << " freed after a fade, " << identical << " frames identical to the XMB without media; at the boot " << freed
                      << " bytes and " << released << " result(s) released\n";
            if (presents != 3 || empty_results != 1 || clears != 2 || modal_stop != 6600 || modal_play <= 7300 || modal_play > 7800 || !music_at_boot)
            {
                std::cerr << "FAIL: media timeline (" << code << "): the dialog stopped the music at " << modal_stop << " ms, it started again at " << modal_play << " ms\n";
                return -1;
            }
            if (!held || freed != held || m.media_bytes() || !released || sealed.post || sealed.present || sealed.play)
            {
                std::cerr << "FAIL: release before boot (" << code << "): " << freed << " of " << held << " bytes freed\n";
                return -1;
            }
            if (frame != other) { std::cerr << "FAIL: the XMB after the release differs from one that never had media (" << code << ")\n"; return -1; }
            if (next_shot != std::size(shots)) { std::cerr << "FAIL: media timeline pictures missing (" << code << ")\n"; return -1; }
        }
        // Mirroring by pixels, with pure-colour stand-ins for the cross (red) and circle (green) glyphs
        {
            auto marked = firmware_images;
            marked.insert_or_assign("tex_cross", image{17, 17, std::vector<std::uint32_t>(17 * 17, 0xffff0000u)});
            marked.insert_or_assign("tex_circle", image{17, 17, std::vector<std::uint32_t>(17 * 17, 0xff00ff00u)});
            scene m;
            m.resources = marked;
            m.cached_background = false;
            std::vector<entry> one{{"terraria", "Terraria", "BLUS31537", "item_tex_ps3game_folder", {}}};
            m.draw(frame.data(), typeface, 6, 0, one, 1., .016f, "01/10  16:53", "", false);
            const int cross = colour_x(frame.data(), 0xffff0000u, 668, 685), circle = colour_x(frame.data(), 0xff00ff00u, 668, 685);
            if (cross < 0 || circle < 0 || (ar ? cross <= circle : cross >= circle)) { std::cerr << "FAIL: footer glyph order (" << code << ")\n"; return -1; }
            // Dialog buttons: the first one (cross) is rightmost in Arabic
            std::fill(frame.begin(), frame.end(), 0xff000000u);
            canvas c(frame.data(), 1280, 720);
            draw_dialog(c, typeface, marked, dialog{{}, {"x"}, {}, -1, {}, {}, false, nullptr,
                {cross_button(i18n::tr(str::COM_SIM)), circle_button(i18n::tr(str::COM_NAO))}}, 1.0);
            const int dcross = colour_x(frame.data(), 0xffff0000u, 0, 720), dcircle = colour_x(frame.data(), 0xff00ff00u, 0, 720);
            if (dcross < 0 || dcircle < 0 || (ar ? dcross <= dcircle : dcross >= dcircle)) { std::cerr << "FAIL: dialog button order (" << code << ")\n"; return -1; }
            // The percentage caption alone: right half in Arabic, left half otherwise
            std::fill(frame.begin(), frame.end(), 0xff000000u);
            dialog only_percent{{}, {}, {}, 450, percent_text(450), {}, false, nullptr, {}};
            auto no_bar = marked;
            no_bar.erase("tex_msgdialog_bar");
            no_bar.erase("tex_default_progress_base");
            no_bar.erase("tex_default_progress_slider");
            draw_dialog(c, typeface, no_bar, only_percent, 1.0);
            const auto [left, right] = bright_ink(frame.data(), 300, 450);
            if (right < 0 || (ar ? left < 640 : right > 640)) { std::cerr << "FAIL: progress caption side (" << code << ") " << left << ".." << right << '\n'; return -1; }
        }
        // No '?' glyph in any line drawn
        std::size_t missing = 0;
        for (const std::string& line : drawn) missing += typeface.missing_glyphs(line, ar);
        for (const auto* hint : {i18n::tr(str::COM_SELECIONAR), i18n::tr(str::COM_VOLTAR), i18n::tr(str::COM_ATUALIZAR), i18n::tr(str::COM_RESTAURAR),
                 i18n::tr(str::XMB_ESQ_DIR_ALTERAR_R1_ACELERAR), i18n::tr(str::XMB_QUADRADO_CONFIGURAR_JOGO_BOTAO_PS)})
            missing += typeface.missing_glyphs(hint, ar);
        for (int category = 0; category < int(scene::category_count); ++category) missing += typeface.missing_glyphs(scene::category_name(category), ar);
        std::cout << "Language " << code << ": 33 screens, " << drawn.size() << " lines, " << missing << " glyph(s) drawn as '?'\n";
        if (missing) { std::cerr << "FAIL: '?' glyphs in " << code << "\n"; return -1; }
    }
    i18n::apply(i18n::setting::automatic, -1);
    return files;
}
} // namespace languages

// 0.40: --firmware <dev_flash>: the folder of that firmware (item_tex_plain_folder of explore_plugin_full.rco), decoded
// as the launcher decodes it, against the known hash of firmware 4.93 (other versions are only reported). Off by
// default: the default run uses only the drawn folder. Its one picture holds the firmware's texture, so it is written
// only to an output folder outside the source tree (refused inside it, before anything is written).
bool inside_source_tree(const std::filesystem::path& path)
{
    namespace fs = std::filesystem;
    const auto lower = [](const fs::path& p)
    {
        std::error_code ec;
        std::string s = fs::weakly_canonical(fs::absolute(p, ec), ec).generic_string();
        for (char& ch : s) ch = char(std::tolower(static_cast<unsigned char>(ch)));
        while (s.size() > 1 && s.back() == '/') s.pop_back();
        return s;
    };
    const std::string tree = lower(fs::path(__FILE__).parent_path() / "../.."), target = lower(path);
    return target == tree || target.starts_with(tree + '/');
}

int folder_firmware_check(const std::filesystem::path& dev_flash, const std::filesystem::path& output, const font& typeface)
{
    namespace fs = std::filesystem;
    namespace i18n = utils::ps4::i18n;
    constexpr std::string_view known_493 = "c81579585f590958aec3d8fb040b9dc89e451fb38db694da78c1cf150bcb58eb";
    const std::string_view abc = "abc";
    if (sha256_hex(std::span(reinterpret_cast<const std::uint8_t*>(abc.data()), abc.size())) != "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
    {
        std::cerr << "FAIL: sha256 self-test\n";
        return 1;
    }
    if (inside_source_tree(output))
    {
        std::cerr << "FAIL: --firmware writes the firmware's texture; give an output folder outside the source tree\n";
        return 1;
    }
    const auto bytes = read(dev_flash / "vsh/resource/explore_plugin_full.rco");
    const std::vector<std::string> wanted{"item_tex_plain_folder"};
    auto images = read_rco_images(bytes, wanted, inflate_data);
    const auto found = images.find("item_tex_plain_folder");
    if (found == images.end() || !found->second)
    {
        std::cerr << "FAIL: --firmware: no item_tex_plain_folder in " << (dev_flash / "vsh/resource/explore_plugin_full.rco").string() << '\n';
        return 1;
    }
    const image& icon = found->second;
    // The ARGB words as the launcher keeps them, little-endian
    std::vector<std::uint8_t> argb;
    argb.reserve(icon.pixels.size() * 4);
    for (const std::uint32_t p : icon.pixels)
        for (int shift = 0; shift < 32; shift += 8) argb.push_back(std::uint8_t(p >> shift));
    const std::string hash = sha256_hex(argb);
    const auto version = read(dev_flash / "vsh/etc/version.txt");
    const bool fw_493 = std::string_view(reinterpret_cast<const char*>(version.data()), version.size()).find("release:04.9300:") != std::string_view::npos;
    std::cout << "Firmware folder item_tex_plain_folder: " << icon.width << 'x' << icon.height << ", ARGB sha256 " << hash
              << (fw_493 ? (hash == known_493 ? " (firmware 4.93: matches)" : " (firmware 4.93: DIFFERENT)") : " (not firmware 4.93: reported only)") << '\n';
    if (fw_493 && hash != known_493) { std::cerr << "FAIL: the decoded firmware folder differs from the 4.93 reference\n"; return 1; }
    // The drawn folder replaces it at the same size, so folder rows keep their rhythm either way
    const image drawn = folder_icon();
    if (drawn.width != icon.width || drawn.height != icon.height) { std::cerr << "FAIL: drawn folder " << drawn.width << 'x' << drawn.height << " vs firmware\n"; return 1; }
    // The open folder as the launcher shows it with this firmware
    scene m;
    m.resources.insert_or_assign("item_tex_plain_folder", icon);
    const folder_header header{&m.resources.at("item_tex_plain_folder"), "Corrida"};
    const column_extras extras{&header, nullptr, i18n::tr(i18n::str::FLD_OPCOES)};
    const std::vector<entry> rows{{"@folder:1", "Corrida", i18n::trf(i18n::str::FLD_PASTA_SUB_N, {3u}), "item_tex_plain_folder", {}},
        {"@folder:2", "Luta", i18n::tr(i18n::str::FLD_PASTA_SUB_0), "item_tex_plain_folder", {}}};
    std::vector<std::uint32_t> frame(1280 * 720);
    for (int i = 0; i < 40; ++i) m.draw(frame.data(), typeface, 6, 0, rows, 3. + i / 60., 1.f / 60, "01/10  16:53", "", false, {}, &extras);
    std::error_code ec;
    fs::create_directories(output, ec);
    if (!write_png(output / "xmb-folders-firmware.png", frame.data())) { std::cerr << "FAIL: cannot write " << (output / "xmb-folders-firmware.png").string() << '\n'; return 1; }
    return 0;
}

int main(int argc, char** argv)
{
    // xmb_preview <dev_flash> <output dir> [ICON0.PNG] [--lang all <preview dir>] [--firmware <dev_flash>]
    // 0.40: --firmware checks that firmware's folder icon (folder_firmware_check); the default run never reads it
    std::filesystem::path folder_firmware;
    if (argc >= 5 && std::string_view(argv[argc - 2]) == "--firmware")
    {
        folder_firmware = argv[argc - 1];
        argc -= 2;
    }
    std::filesystem::path language_out;
    if (argc >= 5 && std::string_view(argv[argc - 3]) == "--lang" && std::string_view(argv[argc - 2]) == "all")
    {
        language_out = argv[argc - 1];
        argc -= 3;
    }
    if (argc != 3 && argc != 4) return 2;
    const std::filesystem::path firmware = argv[1], output = argv[2];
    if (!folder_firmware.empty() && inside_source_tree(output))
    {
        std::cerr << "FAIL: --firmware writes the firmware's texture; give an output folder outside the source tree\n";
        return 1;
    }
    scene menu;
    std::vector<std::string> names(scene::icons.begin(), scene::icons.end());
    names.push_back("item_tex_ps3util"); names.push_back("item_tex_ps3game_folder");
    for (const char* name : {"tex_cross", "tex_circle", "tex_triangle", "tex_square", "tex_lock_icon"}) names.push_back(name);
    for (const char* resource : {"xmb_ingame.rco", "explore_plugin_full.rco", "system_plugin.rco"})
    {
        const auto bytes = read(firmware / "vsh/resource" / resource);
        assert(!bytes.empty());
        auto images = read_rco_images(bytes, names, inflate_data);
        assert(!images.empty());
        for (auto& [name, img] : images) menu.resources.insert_or_assign(name, std::move(img));
        // Truncated/corrupt containers must not expose partial unchecked reads.
        for (std::size_t n : {0u, 15u, 163u, 164u, 175u, 1024u})
            assert(read_rco_images(std::span(bytes).first(std::min(n, bytes.size())), names, inflate_data).empty());
        auto malformed = bytes;
        malformed[168] = 0xff;
        assert(read_rco_images(malformed, names, inflate_data).empty());
    }
    for (const auto* name : scene::icons) assert(menu.resources.contains(name));
    const auto font_bytes = read(firmware / "data/font/SCE-PS3-RD-R-LATIN.TTF");
    font typeface;
    const auto font_start = std::chrono::steady_clock::now();
    assert(typeface.load(font_bytes));
    std::cout << "Font load (7 sizes): " << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - font_start).count() << " ms\n";
    assert(typeface.exact_sizes()); // all seven sizes fit the 1024x1024 atlas
    // The launcher's fallback font (/app0/fonts/DejaVuSans.ttf), when the source tree is next to the test.
    if (const auto dejavu = read(std::filesystem::path(__FILE__).parent_path() / "../../bin/fonts/DejaVuSans.ttf"); !dejavu.empty())
    {
        font fallback;
        assert(fallback.load(dejavu) && fallback.exact_sizes());
        std::vector<std::uint32_t> strip(1280 * 64, 0xff000000u);
        canvas target(strip.data(), 1280, 64);
        fallback.paint(target, 10, 40, "Utilitário de dados guardados (PS3) \xe2\x96\xb3\xe2\x97\x8b\xe2\x84\xa2\xe2\x80\xa6", 21, 0xffffffffu);
        assert(std::any_of(strip.begin(), strip.end(), [](std::uint32_t p) { return (p & 255) != 0; }));
        std::cout << "DejaVu fallback font: 7 exact sizes\n";
    }
    font scaled_typeface;
    assert(scaled_typeface.load(font_bytes, false) && !scaled_typeface.exact_sizes());
    legacy::font old_typeface;
    assert(old_typeface.load(font_bytes));
    font no_font;
    legacy::font no_old_font;
    std::vector<entry> games{{"@savedata", "Utilitário de dados guardados (PS3)", "", "item_tex_ps3util", {}},
        {"minecraft.iso", "Minecraft: PlayStation®3 Edition", "Disco PS3 · ISO · BLUS31426", "item_tex_ps3game_folder", {}, "BLUS31426"},
        {"terraria", "Terraria", "Jogo PS3 · BLUS31537", "item_tex_ps3game_folder", {}},
        {"ben10", "Ben 10", "Jogo PS3", "item_tex_ps3game_folder", {}}};
    assert(games[0].serial.empty() && games[1].serial == "BLUS31426"); // serial is optional and last
    if (argc == 4)
    {
        int w = 0, h = 0, channels = 0;
        auto* data = stbi_load(argv[3], &w, &h, &channels, 4);
        assert(data && w > 0 && h > 0 && w < 1024 && h < 1024);
        games[1].artwork = {w, h, std::vector<std::uint32_t>(std::size_t(w) * h)};
        for (int i = 0; i < w * h; ++i) games[1].artwork.pixels[i] = (std::uint32_t(data[i * 4 + 3]) << 24) | (std::uint32_t(data[i * 4]) << 16) | (std::uint32_t(data[i * 4 + 1]) << 8) | data[i * 4 + 2];
        stbi_image_free(data);
    }
    const std::vector<entry> audio_settings{
        {"@setting", "Master Volume", "100 · Volume principal (%)", "tex_sysconf", {}},
        {"@setting", "Enable Buffering", "true ·", "tex_sysconf", {}},
        {"@setting", "Desired Audio Buffer Duration", "64 · Duração em ms", "tex_sysconf", {}},
        {"@setting", "Enable Time Stretching", "true · Ajusta o áudio quando a emulação fica lenta", "tex_sysconf", {}}};

    // Integer primitives stay within 3/255 of the float reference.
    const int primitives = primitive_diff();
    std::cout << "Primitive max channel diff vs 0.29: " << primitives << "/255\n";
    assert(primitives <= 3);

    // Whole frames, uncached so every frame is rendered from scratch: without text, then with the
    // scaled 28 px font, which is the same path the old compositor used for every size.
    {
        legacy::scene old_menu;
        old_menu.resources = menu.resources;
        scene fast_menu;
        fast_menu.resources = menu.resources;
        fast_menu.cached_background = false;
        std::vector<std::uint32_t> fast_frame(1280 * 720), old_frame(1280 * 720);
        int text_free = 0, scaled_text = 0;
        auto both = [&](bool text, int category, std::size_t selected, std::span<const entry> list, double seconds, float elapsed, std::string_view message, bool settings)
        {
            fast_menu.draw(fast_frame.data(), text ? scaled_typeface : no_font, category, selected, list, seconds, elapsed, "01/10  16:53", message, settings);
            old_menu.draw(old_frame.data(), text ? old_typeface : no_old_font, category, selected, list, seconds, elapsed, "01/10  16:53", message, settings);
            int& worst = text ? scaled_text : text_free;
            worst = std::max(worst, max_diff(fast_frame.data(), old_frame.data(), fast_frame.size()));
        };
        for (const bool text : {false, true})
        {
            for (int i = 0; i < 30; ++i) both(text, 6, i < 12 ? 0 : 1, games, i / 60. + (text ? 40. : 0.), 1.f / 60, "", false);
            for (int category = 0; category < 10; ++category) both(text, category, 0, games, category + 3., .016f, "", false);
            // the settings rows and message line with the list footer (the legacy scene has the old settings footer)
            for (int i = 0; i < 12; ++i)
                both(text, 1, 2, audio_settings, 20. + i / 60., .016f, "Configurações do jogo BLUS31426 · Alterações pendentes", false);
        }
        std::cout << "Frame max channel diff vs 0.29: " << text_free << "/255 without text, " << scaled_text << "/255 with scaled text\n";
        assert(text_free <= 3 && scaled_text <= 3);
    }

    // Exact-size glyphs: same ink and placement as the scaled 28 px glyphs, give or take blur.
    for (const std::string_view text : {"Minecraft: PlayStation®3 Edition", "Utilitário de dados guardados (PS3)", "Configurações · Ação ñ ü ß © 01/10  16:53"})
        for (const float size : {17.f, 18.f, 20.f, 21.f, 23.f, 27.f, 28.f})
        {
            const ink exact = measure<canvas>(typeface, text, size), scaled = measure<legacy::canvas>(old_typeface, text, size);
            if (!(exact.sum * 5 >= scaled.sum * 4 && exact.sum * 4 <= scaled.sum * 5 && std::abs(exact.left - scaled.left) <= 3 &&
                std::abs(exact.right - scaled.right) <= 3 && std::abs(exact.top - scaled.top) <= 3 && std::abs(exact.bottom - scaled.bottom) <= 3))
                std::cerr << "text '" << text << "' " << size << " px: ink " << exact.sum << " vs " << scaled.sum << ", box " << exact.left << ',' << exact.top << '-' << exact.right << ',' << exact.bottom
                          << " vs " << scaled.left << ',' << scaled.top << '-' << scaled.right << ',' << scaled.bottom << '\n';
            assert(exact.sum * 5 >= scaled.sum * 4 && exact.sum * 4 <= scaled.sum * 5);
            assert(std::abs(exact.left - scaled.left) <= 3 && std::abs(exact.right - scaled.right) <= 3);
            assert(std::abs(exact.top - scaled.top) <= 3 && std::abs(exact.bottom - scaled.bottom) <= 3);
        }
    // Code points outside the table and glyphs the font lacks still draw something ('?').
    assert(measure<canvas>(typeface, "\xe2\x96\xb3\xe2\x97\x8b\xe2\x84\xa2 \xf0\x9f\x8e\xae", 21).sum > 0);

    // The cached layer is exact on refresh frames and only lags the ribbons by < 50 ms in between.
    {
        scene cached, uncached;
        cached.resources = uncached.resources = menu.resources;
        uncached.cached_background = false;
        std::vector<std::uint32_t> a(1280 * 720), b(1280 * 720);
        int identical = 0;
        for (int i = 0; i < 30; ++i)
        {
            cached.draw(a.data(), typeface, 6, 0, games, 5. + i / 60., 1.f / 60, "01/10  16:53", "");
            uncached.draw(b.data(), typeface, 6, 0, games, 5. + i / 60., 1.f / 60, "01/10  16:53", "");
            if (a == b) { ++identical; continue; }
            std::size_t off = 0;
            for (std::size_t k = 0; k < a.size(); ++k) off += max_diff(&a[k], &b[k], 1) > 3;
            assert(off < a.size() / 200);
        }
        assert(identical >= 7);
    }

    constexpr std::uint32_t sentinel = 0x12abcdef;
    std::vector<std::uint32_t> frame(1280 * 720 + 2, sentinel);
    auto* pixels = frame.data() + 1;
    // Best of three runs of 60 frames, for the default (cached), uncached and 0.29 compositors.
    auto timed = [&](auto& renderer, const auto& face)
    {
        double best = 1e9;
        for (int run = 0; run < 3; ++run)
        {
            const auto start = std::chrono::steady_clock::now();
            for (int i = 0; i < 60; ++i) renderer.draw(pixels, face, 6, 0, games, run * 10. + i / 60., 1.f / 60, "01/10  16:53", "");
            best = std::min(best, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() / 60);
        }
        return best;
    };
    double uncached_ms = 0, legacy_ms = 0;
    {
        scene uncached;
        uncached.resources = menu.resources;
        uncached.cached_background = false;
        uncached_ms = timed(uncached, typeface);
        legacy::scene old_menu;
        old_menu.resources = menu.resources;
        legacy_ms = timed(old_menu, old_typeface);
    }
    const double average = timed(menu, typeface);
    std::cout << "Host render average: " << average << " ms/frame (not a PS4 measurement)\n";
    std::cout << "Host render average without the layer cache: " << uncached_ms << " ms/frame; 0.29 compositor: " << legacy_ms << " ms/frame\n";
    // Speed target for optimized, uninstrumented builds (0.29 took about 7 ms here). The uncached path
    // is compared with the 0.29 compositor on the same host, so a slower or busy machine does not fail it.
#if defined(__OPTIMIZE__) && !defined(XMB_PREVIEW_SANITIZED)
    assert(average <= 3.0);
#endif
    assert(uncached_ms * 2 <= legacy_ms);
    assert(frame.front() == sentinel && frame.back() == sentinel);
    for (int i = 0; i < 60; ++i) menu.draw(pixels, typeface, 6, 0, games, i / 60., 1.f / 60, "01/10  16:53", "");
    const auto first = std::vector<std::uint32_t>(pixels, pixels + 1280 * 720);
    menu.draw(pixels, typeface, 6, 0, games, 2., 1.f / 60, "01/10  16:53", "");
    assert(!std::equal(first.begin(), first.end(), pixels)); // Waves really animate.
    for (const auto& file : {"xmb-menu.png", "xmb-game-selection.png"})
    {
        if (std::string_view(file) == "xmb-game-selection.png")
            for (int i = 0; i < 60; ++i) menu.draw(pixels, typeface, 6, 1, games, 2. + i / 60., 1.f / 60, "01/10  16:53", "");
        assert(write_png(output / file, pixels));
    }
    {
        // The same game selection drawn by the 0.29 compositor, for a side-by-side look at the text.
        legacy::scene old_menu;
        old_menu.resources = menu.resources;
        std::vector<std::uint32_t> old_frame(1280 * 720);
        for (int i = 0; i < 60; ++i) old_menu.draw(old_frame.data(), old_typeface, 6, 0, games, i / 60., 1.f / 60, "01/10  16:53", "");
        for (int i = 0; i < 60; ++i) old_menu.draw(old_frame.data(), old_typeface, 6, 1, games, 2. + i / 60., 1.f / 60, "01/10  16:53", "");
        assert(write_png(output / "xmb-game-selection-0.29.png", old_frame.data()));
    }
    for (int category = 0; category < 10; ++category)
        menu.draw(pixels, typeface, category, 0, games, category + 3., .016f, "01/10  16:53", "");
    assert(frame.front() == sentinel && frame.back() == sentinel);
    for (int i = 0; i < 60; ++i)
        menu.draw(pixels, typeface, 1, 2, audio_settings, 20. + i / 60., .016f, "01/10  16:53", "Configurações do jogo BLUS31426 · Alterações pendentes", true);
    assert(write_png(output / "xmb-settings.png", pixels));
    assert(frame.front() == sentinel && frame.back() == sentinel);
    // Clipping: everything drawn partly or fully off screen stays inside the frame.
    {
        canvas c(pixels, 1280, 720);
        c.rectangle(-50, -50, 100, 100, 0x80ffffff); c.rectangle(1260, 700, 100, 100, 0x80ffffff);
        c.blit(menu.resources.begin()->second, -40.5f, 690.25f, 96.f, 96.f, 255);
        c.blit(menu.resources.begin()->second, 1250.f, -60.f, 200.f, 90.f, 255);
        typeface.paint(c, 1200, 10, "Texto cortado na borda", 27);
        typeface.paint(c, -30, 725, "Texto fora da tela", 17);
        scaled_typeface.paint(c, 1240, 715, "Borda", 19);
    }
    assert(frame.front() == sentinel && frame.back() == sentinel);

    // 0.37: games installed from a PS3 disc whose disc is not in the drive: the icon dimmed, the padlock in its middle
    // and the subtitle saying why. Drawn with the firmware's tex_lock_icon (when this firmware has it) and with the
    // padlock drawn by code (util/ps4_padlock_icon.hpp), which the launcher uses when the firmware lacks it.
    {
        std::vector<entry> locked_games = games;
        for (const std::size_t row : {std::size_t{1}, std::size_t{2}}) locked_games[row].locked = locked_games[row].disc_bound = true;
        const auto render = [&](const std::vector<entry>& list, const image* lock, std::vector<std::uint32_t>& out)
        {
            scene lock_menu;
            lock_menu.resources = menu.resources;
            lock_menu.resources.erase("tex_lock_icon");
            if (lock) lock_menu.resources.emplace("tex_lock_icon", *lock);
            out.assign(1280 * 720, 0);
            for (int i = 0; i < 60; ++i) lock_menu.draw(out.data(), typeface, 6, 1, list, 2. + i / 60., 1.f / 60, "01/10  16:53", "");
        };
        const auto drawn_start = std::chrono::steady_clock::now();
        const image drawn = padlock_icon();
        std::cout << "Drawn padlock: " << drawn.width << "x" << drawn.height << " in "
                  << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - drawn_start).count() << " ms\n";
        assert(drawn && drawn.width == 100 && drawn.height == 120);
        std::vector<std::uint32_t> unlocked, dimmed, with_drawn, with_firmware;
        render(games, &drawn, unlocked);
        render(locked_games, nullptr, dimmed);
        render(locked_games, &drawn, with_drawn);
        // Locking dims the icons and adds the subtitle hint; the padlock is drawn only over the icon of the row
        assert(max_diff(unlocked.data(), dimmed.data(), unlocked.size()) > 0);
        std::size_t changed = 0, outside = 0;
        for (int y = 0; y < 720; ++y) for (int x = 0; x < 1280; ++x)
            if (dimmed[y * 1280 + x] != with_drawn[y * 1280 + x])
            {
                ++changed;
                outside += x < 300 || x > 450 || y < 280 || y > 520;
            }
        std::cout << "Locked rows: " << changed << " pixels carry the drawn padlock (" << outside << " outside the icons)\n";
        assert(changed > 400 && outside == 0);
        assert(write_png(output / "xmb-locked-drawn.png", with_drawn.data()));
        assert(write_crop_png(output / "xmb-locked-drawn-zoom.png", with_drawn.data(), 300, 280, 150, 240, 3));
        if (const auto firmware_lock = menu.resources.find("tex_lock_icon"); firmware_lock != menu.resources.end())
        {
            std::cout << "Firmware padlock tex_lock_icon: " << firmware_lock->second.width << "x" << firmware_lock->second.height << '\n';
            render(locked_games, &firmware_lock->second, with_firmware);
            assert(write_png(output / "xmb-locked-firmware.png", with_firmware.data()));
            assert(write_crop_png(output / "xmb-locked-firmware-zoom.png", with_firmware.data(), 300, 280, 150, 240, 3));
        }
        else std::cout << "This firmware has no tex_lock_icon: only the drawn padlock was rendered\n";
    }

    // 0.40: the folder drawn by util/ps4_folder_icon.hpp when the firmware lacks item_tex_plain_folder: the firmware
    // texture's 314x178 canvas and about its footprint (x 58-254, y 15-148), so folder rows keep their size either way
    {
        const auto drawn_start = std::chrono::steady_clock::now();
        const image drawn_folder = folder_icon();
        const double drawn_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - drawn_start).count();
        assert(drawn_folder && drawn_folder.width == 314 && drawn_folder.height == 178);
        int x0 = 314, y0 = 178, x1 = -1, y1 = -1;
        for (int y = 0; y < 178; ++y)
            for (int x = 0; x < 314; ++x)
                if (drawn_folder.pixels[std::size_t(y) * 314 + std::size_t(x)] >> 24) { x0 = std::min(x0, x); x1 = std::max(x1, x); y0 = std::min(y0, y); y1 = std::max(y1, y); }
        std::cout << "Drawn folder: " << drawn_folder.width << 'x' << drawn_folder.height << " in " << drawn_ms << " ms, visible x " << x0 << ".." << x1 << ", y " << y0 << ".." << y1 << '\n';
        assert(std::abs(x0 - 58) <= 2 && std::abs(x1 - 255) <= 2 && std::abs(y0 - 18) <= 2 && std::abs(y1 - 155) <= 2);
        // Sizes outside 16..1024 give no image; other sizes scale the same drawing
        assert(!folder_icon(15, 100) && !folder_icon(100, 1025) && !folder_icon(0, 0));
        const image small = folder_icon(16, 16);
        assert(small.width == 16 && small.height == 16 && small.pixels.size() == 256);
    }

    // 0.38: the PS3 system dialogs of the install, disc and firmware screens (util/ps4_xmb_dialog.hpp), each over the XMB
    // waves, once with this firmware's dialog images (system_plugin.rco) and once with the drawn stand-ins. The texts are
    // the launcher's (ps4_launcher.cpp builds the same dialogs from its jobs).
    {
        std::vector<std::string> dialog_names(dialog_images.begin(), dialog_images.end());
        dialog_names.push_back("item_tex_disc_install");
        auto firmware_images = menu.resources;
        for (const char* resource : {"system_plugin.rco", "explore_plugin_full.rco"})
            for (auto& [name, img] : read_rco_images(read(firmware / "vsh/resource" / resource), dialog_names, inflate_data))
                firmware_images.insert_or_assign(name, std::move(img));
        for (const char* name : dialog_images)
            if (!firmware_images.contains(name)) std::cout << "This firmware has no " << name << ": the drawn one is used\n";
        auto drawn_images = menu.resources;
        for (const char* name : dialog_images) drawn_images.erase(name);
        drawn_images.erase("tex_square");
        const auto drawn_start = std::chrono::steady_clock::now();
        add_dialog_fallbacks(drawn_images);
        add_dialog_fallbacks(firmware_images); // only fills what this firmware lacks (and tex_square)
        std::cout << "Drawn dialog images: " << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - drawn_start).count() << " ms\n";
        for (const char* name : dialog_images)
        {
            const image& drawn = drawn_images.at(name);
            const image& real = firmware_images.at(name);
            assert(drawn && real && drawn.width == real.width && drawn.height == real.height); // same size and placement
        }
        assert(drawn_images.contains("tex_square") && firmware_images.contains("tex_square"));
        // A fallback never replaces a firmware image
        {
            auto kept = firmware_images;
            add_dialog_fallbacks(kept);
            for (const char* name : dialog_images) assert(kept.at(name).pixels == firmware_images.at(name).pixels);
        }
        const image* disc_icon = firmware_images.contains("item_tex_disc_install") ? &firmware_images.at("item_tex_disc_install") : nullptr;
        const image* game_icon = games[1].artwork ? &games[1].artwork : disc_icon;
        dialog pkg_progress{"God of War: Ascension", {"Instalando... (1/2)"}, {"820 MB de 1,5 GB"}, 450, percent_text(450),
            remaining_text(700'000'000, 4'000'000), false, nullptr, {circle_button("Cancelar")}};
        dialog pkg_order{"God of War: Ascension · Update v01.12 (BCUS98232)",
            {"Este update foi feito para ser instalado sobre a v01.11.", "Seu disco (ISO) é v01.00 e não há nenhum update instalado.",
                "Alguns updates já trazem tudo e funcionam assim; outros travam o jogo ou nem deixam abrir.", "Deseja instalar mesmo assim?"},
            {"No PS3 seria preciso instalar antes os updates até a v01.11.", "Para desfazer, apague /data/rpcs3/dev_hdd0/game/BCUS98232 pelo FTP."}, -1, {},
            {}, false, nullptr, {cross_button("Sim"), circle_button("Não")}};
        dialog pkg_done{"God of War: Ascension · Update v01.12 · BCUS98232", {"A instalação foi concluída."},
            {"Pasta: /data/rpcs3/dev_hdd0/game/BCUS98232", "Seu disco (v01.00) agora abre na v01.12."}, -1, {}, {}, false, nullptr, {cross_button("OK")}};
        dialog pkg_cancel{"God of War: Ascension", {"Cancelando..."}, {"Aguarde a limpeza dos arquivos."}, -1, {}, {}, true, nullptr, {}};
        dialog pkg_failed{"God of War: Ascension", {"Não foi possível concluir a instalação."},
            {"Sem espaço em /data (precisa de ~5 GB livres): apague algo e tente de novo", "nada ficou instalado"}, -1, {}, {}, false, nullptr,
            {cross_button("OK")}};
        dialog disc_question{"FIFA 14 (BLES01876)", {"Deseja instalar este disco no PS4?"},
            {"Tamanho: 10,6 GB (cerca de 12 min). O disco nunca é alterado.", "O jogo vai para /data/rpcs3/games e abre pela lista, como uma ISO.",
                "Espaço livre no PS4: 412,3 GB", "Chave do disco obtida automaticamente"}, -1, {}, {}, false, disc_icon,
            {cross_button("Instalar"), circle_button("Voltar")}};
        dialog disc_key_wait{"FIFA 14 (BLES01876)", {"Deseja instalar este disco no PS4?", "Aguardando a chave do disco..."},
            {"Tamanho: 10,6 GB (cerca de 12 min). O disco nunca é alterado.", "O jogo vai para /data/rpcs3/games e abre pela lista, como uma ISO.",
                "Espaço livre no PS4: 412,3 GB", "Chave do disco: procurando (na primeira vez usa a internet)..."}, -1, {}, {}, true, disc_icon,
            {circle_button("Voltar")}};
        dialog disc_resume{"FIFA 14 (BLES01876)", {"Deseja continuar a instalação do disco?"},
            {"Faltam 6,4 GB de 10,6 GB (cerca de 7 min). O disco nunca é alterado.", "O jogo vai para /data/rpcs3/games e abre pela lista, como uma ISO.",
                "Espaço livre no PS4: 412,3 GB", "Chave do disco já está no console e abre o disco"},
            -1, {}, {}, false, disc_icon, {cross_button("Continuar"), square_button("Apagar a cópia"), circle_button("Voltar")}};
        dialog disc_copy{"FIFA 14 (BLES01876)", {"Copiando o disco..."}, {"4,2 GB de 10,6 GB · 15,0 MB/s", "A cópia continua se você voltar ao menu."}, 420,
            percent_text(420), remaining_text(6'400'000'000, 15'000'000), false, disc_icon, {cross_button("OK"), circle_button("Cancelar")}};
        dialog disc_cancel{"FIFA 14 (BLES01876)", {"Deseja cancelar a instalação do disco?"},
            {"O que já foi copiado fica salvo e a próxima instalação continua dali."}, -1, {}, {}, false, disc_icon, {cross_button("Sim"), circle_button("Não")}};
        dialog disc_done{"FIFA 14 (BLES01876)", {"A instalação foi concluída."}, {"O jogo já está na lista de jogos."}, -1, {}, {}, false, disc_icon,
            {cross_button("OK")}};
        dialog lock_verifying{"FIFA 14 (BLES01876)", {"Verificando o disco..."}, {"Procurando o disco deste jogo no leitor do PS4."}, -1, {}, {}, true,
            game_icon, {circle_button("Voltar")}};
        dialog lock_missing{"FIFA 14 (BLES01876)", {"Coloque o disco deste jogo no leitor do PS4."},
            {"Este jogo foi instalado a partir do disco de PS3: para jogar, o disco precisa estar no leitor.", "Agora não há nenhum disco no leitor.",
                "Esta tela muda sozinha quando o disco entrar."}, -1, {}, {}, false, game_icon, {circle_button("Voltar")}};
        dialog lock_recognized{"FIFA 14 (BLES01876)", {"Disco reconhecido."}, {"Já dá para jogar."}, -1, {}, {}, false, game_icon,
            {cross_button("Jogar"), circle_button("Voltar")}};
        dialog firmware_question{"Firmware do PS3", {"Deseja instalar o firmware 4.93?"},
            {"Arquivo: USB 1:/PS3/UPDATE/PS3UPDAT.PUP (209 MB)", "Instalado agora: nenhum"}, -1, {}, {}, false, nullptr,
            {cross_button("Sim"), circle_button("Não")}};
        dialog firmware_progress{"Firmware do PS3 4.93", {"Instalando..."}, {"Não desligue o sistema."}, 708, percent_text(708), "Pacote 24 de 39", false,
            nullptr, {circle_button("Cancelar")}};
        dialog firmware_done{"Firmware do PS3 4.93", {"A instalação foi concluída."},
            {"Firmware 4.93 instalado: os jogos já podem usá-lo", "o anterior fica guardado até a próxima abertura"}, -1, {}, {}, false, nullptr,
            {cross_button("OK")}};
        dialog exit_question{"FIFA 14 (BLES01876)", {"Deseja sair do RPCS3?"}, {"A instalação do disco para e continua de onde parou na próxima vez."}, -1,
            {}, {}, false, disc_icon, {cross_button("Sair"), circle_button("Voltar")}};
        dialog disc_reading{"FIFA 14 (BLES01876)", {"Lendo o disco..."}, {"A cópia continua se você voltar ao menu."}, -1, {}, {}, true, disc_icon,
            {cross_button("OK"), circle_button("Cancelar")}};
        dialog disc_key_failed{"FIFA 14 (BLES01876)", {"Deseja instalar este disco no PS4?"},
            {"Tamanho: 10,6 GB (cerca de 12 min). O disco nunca é alterado.", "O jogo vai para /data/rpcs3/games e abre pela lista, como uma ISO.",
                "Espaço livre no PS4: 412,3 GB", "A chave do disco não foi encontrada.",
                "Sem a chave o disco é copiado assim mesmo, mas o jogo só abre quando ela chegar: com internet, coloque o disco de novo; ou ponha o "
                ".ird em USB:/ird ou a .dkey em USB:/redump."}, -1, {}, {}, false, disc_icon, {cross_button("Instalar sem a chave"), circle_button("Voltar")}};
        dialog disc_discard{"FIFA 14 (BLES01876)", {"Deseja continuar a instalação do disco?", "Apagar os 4,2 GB já copiados? O disco não muda."},
            {"Faltam 6,4 GB de 10,6 GB (cerca de 7 min). O disco nunca é alterado.", "O jogo vai para /data/rpcs3/games e abre pela lista, como uma ISO.",
                "Espaço livre no PS4: 412,3 GB", "Chave do disco já está no console e abre o disco"},
            -1, {}, {}, false, disc_icon, {cross_button("Continuar"), square_button("Apagar"), circle_button("Voltar")}};
        dialog disc_cancelling{"FIFA 14 (BLES01876)", {"Cancelando..."}, {"O que já foi copiado fica salvo."}, -1, {}, {}, true, disc_icon, {}};
        dialog disc_failed{"FIFA 14 (BLES01876)", {"Não foi possível concluir a instalação."},
            {"O disco saiu do leitor ou não pôde ser lido: coloque-o e instale de novo (continua de onde parou)"}, -1, {}, {}, false, disc_icon,
            {cross_button("OK")}};
        dialog disc_removed{"FIFA 14 (BLES01876)", {"O disco foi removido."},
            {"O jogo foi fechado. Para jogar de novo, coloque o disco no leitor do PS4.", "Recomendado reiniciar o aplicativo antes de jogar de novo"},
            -1, {}, {}, false, game_icon, {cross_button("OK")}};
        dialog firmware_check{"Firmware do PS3", {"Verificando o PS3UPDAT.PUP..."}, {"USB 1:/PS3/UPDATE/PS3UPDAT.PUP"}, -1, {}, {}, true, nullptr, {}};
        dialog firmware_cancel{"Firmware do PS3 4.93", {"Deseja cancelar a instalação do firmware?"}, {"O firmware atual fica como está."}, -1, {}, {},
            false, nullptr, {cross_button("Sim"), circle_button("Não")}};
        dialog firmware_none{"Firmware do PS3",
            {"Nenhum PS3UPDAT.PUP: copie para o pendrive em PS3/UPDATE (ou na raiz) ou para /data/rpcs3/import"}, {}, -1, {}, {}, false, nullptr,
            {cross_button("OK")}};
        dialog dkey_result{"Chaves de disco (.dkey)", {"3 chave(s) .dkey importada(s) para /data/rpcs3/data/redump"}, {"1 arquivo(s) .dkey inválido(s)"}, -1,
            {}, {}, false, nullptr, {cross_button("OK")}};
        dialog emulator_error{{}, {"Ocorreu um erro no emulador."}, {"Para jogar de novo, feche o aplicativo pelo botão PS e abra-o outra vez."}, -1, {}, {},
            false, nullptr, {cross_button("OK")}};
        const std::vector<std::pair<const char*, const dialog*>> samples{{"pkg-progress", &pkg_progress}, {"pkg-out-of-order", &pkg_order},
            {"pkg-done", &pkg_done}, {"pkg-cancelling", &pkg_cancel}, {"pkg-failed", &pkg_failed}, {"disc-question", &disc_question},
            {"disc-key-wait", &disc_key_wait}, {"disc-resume", &disc_resume}, {"disc-copy", &disc_copy}, {"disc-cancel", &disc_cancel},
            {"disc-done", &disc_done}, {"disc-exit", &exit_question}, {"lock-verifying", &lock_verifying}, {"lock-missing", &lock_missing},
            {"lock-recognized", &lock_recognized}, {"firmware-question", &firmware_question}, {"firmware-progress", &firmware_progress},
            {"firmware-done", &firmware_done}, {"disc-reading", &disc_reading}, {"disc-key-failed", &disc_key_failed},
            {"disc-discard", &disc_discard}, {"disc-cancelling", &disc_cancelling}, {"disc-failed", &disc_failed},
            {"disc-removed", &disc_removed}, {"firmware-check", &firmware_check}, {"firmware-cancel", &firmware_cancel},
            {"firmware-none", &firmware_none}, {"dkey-result", &dkey_result}, {"emulator-error", &emulator_error}};
        std::vector<std::uint32_t> waves(1280 * 720 + 2, sentinel), shown(1280 * 720 + 2, sentinel);
        double dialog_ms = 0;
        int dialog_frames = 0;
        for (const auto& [name, sample] : samples)
            for (const bool real : {true, false})
            {
                scene backdrop;
                backdrop.resources = real ? firmware_images : drawn_images;
                std::uint32_t* const plain = waves.data() + 1;
                std::uint32_t* const frame_pixels = shown.data() + 1;
                for (int i = 0; i < 30; ++i) backdrop.backdrop(plain, 4. + i / 60.);
                std::copy(plain, plain + 1280 * 720, frame_pixels);
                canvas target(frame_pixels, 1280, 720);
                // Fade-in, then the dialog at full strength (best of three for the timing)
                draw_dialog(target, typeface, backdrop.resources, *sample, 4.5, 80);
                double best = 1e9;
                for (int run = 0; run < 3; ++run)
                {
                    std::copy(plain, plain + 1280 * 720, frame_pixels);
                    const auto start = std::chrono::steady_clock::now();
                    draw_dialog(target, typeface, backdrop.resources, *sample, 4.5);
                    best = std::min(best, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
                }
                dialog_ms += best;
                ++dialog_frames;
                assert(waves.front() == sentinel && waves.back() == sentinel && shown.front() == sentinel && shown.back() == sentinel);
                assert(max_diff(plain, frame_pixels, 1280 * 720) > 0);
                // The waves stay visible under the shade: the dialog keeps their texture, only darker
                std::size_t darker = 0, brighter = 0;
                for (int y = 0; y < 60; ++y)
                    for (int x = 0; x < 1280; ++x)
                    {
                        const std::uint32_t a = plain[y * 1280 + x], b = frame_pixels[y * 1280 + x];
                        const int la = int((a >> 16) & 255) + int((a >> 8) & 255) + int(a & 255), lb = int((b >> 16) & 255) + int((b >> 8) & 255) + int(b & 255);
                        darker += lb < la;
                        brighter += lb > la;
                    }
                assert(darker > 60 * 1280 * 9 / 10 && brighter == 0);
                const std::string file = std::string("dialog-") + name + (real ? "-firmware.png" : "-drawn.png");
                assert(write_png(output / file, frame_pixels));
                if (std::string_view(name) == "disc-copy")
                {
                    // The bar and the button guide up close
                    assert(write_crop_png(output / (std::string("dialog-bar-zoom") + (real ? "-firmware.png" : "-drawn.png")), frame_pixels, 300, 420, 680, 150, 2));
                }
                if (std::string_view(name) == "lock-verifying")
                    assert(write_crop_png(output / (std::string("dialog-busy-zoom") + (real ? "-firmware.png" : "-drawn.png")), frame_pixels, 560, 420, 160, 140, 4));
            }
        // No firmware installed at all: the bundled DejaVu font and the drawn images (when the font is next to the test)
        if (const auto dejavu = read(std::filesystem::path(__FILE__).parent_path() / "../../bin/fonts/DejaVuSans.ttf"); !dejavu.empty())
        {
            font fallback;
            assert(fallback.load(dejavu));
            for (const auto& [name, sample] : {std::pair{"disc-copy", &disc_copy}, std::pair{"lock-missing", &lock_missing}})
            {
                scene backdrop;
                backdrop.resources = drawn_images;
                std::vector<std::uint32_t> frame_out(1280 * 720);
                for (int i = 0; i < 30; ++i) backdrop.backdrop(frame_out.data(), 4. + i / 60.);
                canvas target(frame_out.data(), 1280, 720);
                dialog plain = *sample;
                plain.icon = nullptr; // without the firmware there is no disc icon either
                draw_dialog(target, fallback, backdrop.resources, plain, 4.5);
                assert(write_png(output / (std::string("dialog-") + name + "-nofirmware.png"), frame_out.data()));
            }
        }
        // The band's single blend equals the shade and the band blended one after the other (within rounding)
        {
            random_bits rng;
            int worst = 0;
            for (int i = 0; i < 20000; ++i)
            {
                const std::uint32_t dst = rng() | 0xff000000u, shade = (rng() & 0xffffffu) | (std::uint32_t(rng.range(0, 255)) << 24);
                const std::uint32_t band = (rng() & 0xffffffu) | (std::uint32_t(rng.range(0, 255)) << 24);
                std::uint32_t twice = dst, once = dst;
                canvas a(&twice, 1, 1), b(&once, 1, 1);
                a.rectangle(0, 0, 1, 1, shade);
                a.rectangle(0, 0, 1, 1, band);
                b.rectangle(0, 0, 1, 1, dialog_detail::over(shade, band));
                worst = std::max(worst, max_diff(&twice, &once, 1));
            }
            std::cout << "Band blend in one pass vs two: max channel diff " << worst << "/255\n";
            assert(worst <= 3);
        }
        // The busy clock turns: two times draw different hands
        {
            scene backdrop;
            backdrop.resources = firmware_images;
            std::vector<std::uint32_t> a(1280 * 720), b(1280 * 720);
            backdrop.backdrop(a.data(), 4.);
            b = a;
            canvas ca(a.data(), 1280, 720), cb(b.data(), 1280, 720);
            draw_dialog(ca, typeface, backdrop.resources, lock_verifying, 4.0);
            draw_dialog(cb, typeface, backdrop.resources, lock_verifying, 4.25);
            assert(a != b);
        }
        // Text is centred: the title's ink is balanced around the middle of the screen
        {
            auto text_only = firmware_images;
            text_only.erase("tex_msgdialog_bar"); // no separators: only the title is bright
            std::vector<std::uint32_t> ink(1280 * 720, 0xff000000u);
            canvas c(ink.data(), 1280, 720);
            dialog centred{"Instalando PKG PS3", {}, {}, -1, {}, {}, false, nullptr, {}};
            draw_dialog(c, typeface, text_only, centred, 0);
            int left = 1280, right = -1;
            for (int y = 0; y < 720; ++y)
                for (int x = 0; x < 1280; ++x)
                    if ((ink[y * 1280 + x] & 255) > 120) { left = std::min(left, x); right = std::max(right, x); }
            std::cout << "Centred title ink: x " << left << ".." << right << '\n';
            assert(right > left && std::abs((left + right) - 1280) <= 6);
        }
        std::cout << "Dialogs: " << samples.size() << " screens x 2 (firmware images, drawn ones), " << dialog_ms / dialog_frames
                  << " ms per dialog on top of the waves (host)\n";
        // 0.39: the four interface languages, firmware Latin glyphs + DejaVu Sans Arabic forms (the launcher's pair)
        if (!language_out.empty())
        {
            const auto dejavu = read(std::filesystem::path(__FILE__).parent_path() / "../../bin/fonts/DejaVuSans.ttf");
            assert(!dejavu.empty());
            font with_arabic;
            const auto load_start = std::chrono::steady_clock::now();
            assert(with_arabic.load(font_bytes, dejavu) && with_arabic.exact_sizes() && with_arabic.arabic());
            std::cout << "Font load with the Arabic forms (7 sizes): "
                      << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - load_start).count() << " ms\n";
            // The Arabic forms come from the second font; without it they draw as '?' and the Latin glyphs stay exact
            assert(!typeface.arabic() && typeface.missing_glyphs("\xD8\xA7\xD9\x84\xD9\x84\xD8\xBA\xD8\xA9", true) == 5);
            assert(with_arabic.missing_glyphs("\xD8\xA7\xD9\x84\xD9\x84\xD8\xBA\xD8\xA9", true) == 0);
            // Latin text renders exactly as before with the Arabic forms in the same atlas
            for (const float size : {17.f, 20.f, 27.f})
            {
                const ink a = measure<canvas>(typeface, "Configurações · Ação ñ ü ß © 01/10", size), b = measure<canvas>(with_arabic, "Configurações · Ação ñ ü ß © 01/10", size);
                assert(a.sum == b.sum && a.left == b.left && a.right == b.right);
            }
            const int written = languages::render(language_out, firmware_images, with_arabic);
            // 0.40: 8 safe-mode, audio and game language screens per language; 11 folder and 14 game media screens per language;
            // 9 new settings rows, 14 console-limit and platform screens and 8 pictures of the media timeline per language
            assert(written == 388);
            std::cout << "Language previews: " << written << " files in " << language_out.string() << '\n';
        }
    }
    if (!folder_firmware.empty() && folder_firmware_check(folder_firmware, output, typeface) != 0) return 1;
    std::cout << "PASS: original firmware RCO/GIM (RGBA+BC3), 10 category icons, PS3 font (7 exact sizes), bounded decode, animated frames, clipped drawing, "
                 "integer compositor within 3/255 of 0.29, PS3 system dialogs\n";
}
