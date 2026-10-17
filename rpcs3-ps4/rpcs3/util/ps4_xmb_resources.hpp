#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <vector>

namespace utils::ps4::xmb
{
struct image
{
    int width = 0, height = 0;
    std::vector<std::uint32_t> pixels; // ARGB, matching the linear VideoOut buffer.
    explicit operator bool() const { return width > 0 && height > 0 && pixels.size() == std::size_t(width) * height; }
};

inline bool region(std::span<const std::uint8_t> data, std::size_t at, std::size_t count)
{
    return at <= data.size() && count <= data.size() - at;
}
inline std::uint32_t be32(std::span<const std::uint8_t> b, std::size_t p)
{
    return (std::uint32_t(b[p]) << 24) | (std::uint32_t(b[p + 1]) << 16) | (std::uint32_t(b[p + 2]) << 8) | b[p + 3];
}
inline std::uint16_t be16(std::span<const std::uint8_t> b, std::size_t p)
{
    return (std::uint16_t(b[p]) << 8) | b[p + 1];
}

// Read the first linear RGBA8888 or BC3 image in a big-endian PS3 GIM.
// Unsupported formats remain unavailable; they are never reinterpreted as RGBA.
inline image decode_gim(std::span<const std::uint8_t> data)
{
    if (!region(data, 0, 32) || be32(data, 0) != 0x2e47494d || be32(data, 4) != 0x312e3030) return {};
    std::size_t block = 16;
    for (unsigned int depth = 0; depth < 32 && region(data, block, 16); ++depth)
    {
        const auto type = be16(data, block);
        const auto size_header = be32(data, block + 12);
        const auto size_block = be32(data, block + 4), next = be32(data, block + 8);
        if (size_block < 16 || !region(data, block, size_block)) return {};
        if (type != 4)
        {
            if (next < 16 || next >= size_block) return {};
            block += next;
            continue;
        }
        if (size_header < 16 || size_header > size_block || !region(data, block + size_header, 48)) return {};
        const std::size_t header = block + size_header;
        const auto format = be16(data, header + 4), order = be16(data, header + 6);
        const int width = be16(data, header + 8), height = be16(data, header + 10);
        const auto alignment = be16(data, header + 14);
        const auto offset = be32(data, header + 28);
        if (width <= 0 || height <= 0 || width > 1024 || height > 1024 || order != 0 || offset < 48 || offset > size_block - size_header) return {};
        const auto source = data.subspan(header + offset, size_block - size_header - offset);
        image result{width, height, std::vector<std::uint32_t>(std::size_t(width) * height)};
        if (format == 3)
        {
            if (!alignment || alignment > 256 || (alignment & (alignment - 1))) return {};
            const std::size_t stride = (width * 4 + alignment - 1) & ~std::size_t(alignment - 1);
            if (!region(source, 0, stride * height)) return {};
            for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x)
            {
                const auto p = source.data() + y * stride + x * 4;
                result.pixels[y * width + x] = (std::uint32_t(p[3]) << 24) | (std::uint32_t(p[0]) << 16) | (std::uint32_t(p[1]) << 8) | p[2];
            }
            return result;
        }
        if (format != 10) return {}; // BC3/DXT5.
        const int cols = (width + 3) / 4, rows = (height + 3) / 4;
        if (!region(source, 0, std::size_t(cols) * rows * 16)) return {};
        for (int by = 0; by < rows; ++by) for (int bx = 0; bx < cols; ++bx)
        {
            const auto* p = source.data() + (by * cols + bx) * 16;
            std::array<unsigned int, 8> alpha{p[0], p[1]};
            if (p[0] > p[1]) for (int k = 2; k < 8; ++k) alpha[k] = ((8 - k) * p[0] + (k - 1) * p[1]) / 7;
            else
            {
                for (int k = 2; k < 6; ++k) alpha[k] = ((6 - k) * p[0] + (k - 1) * p[1]) / 5;
                alpha[6] = 0; alpha[7] = 255;
            }
            std::uint64_t alpha_bits = 0;
            for (int k = 0; k < 6; ++k) alpha_bits |= std::uint64_t(p[2 + k]) << (k * 8);
            std::array<std::array<unsigned int, 3>, 4> rgb{};
            for (int k = 0; k < 2; ++k)
            {
                const unsigned int c = p[8 + k * 2] | (p[9 + k * 2] << 8);
                rgb[k] = {((c >> 11) & 31) * 255 / 31, ((c >> 5) & 63) * 255 / 63, (c & 31) * 255 / 31};
            }
            for (int k = 0; k < 3; ++k)
            {
                rgb[2][k] = (2 * rgb[0][k] + rgb[1][k]) / 3;
                rgb[3][k] = (rgb[0][k] + 2 * rgb[1][k]) / 3;
            }
            const std::uint32_t bits = p[12] | (std::uint32_t(p[13]) << 8) | (std::uint32_t(p[14]) << 16) | (std::uint32_t(p[15]) << 24);
            for (int k = 0; k < 16; ++k)
            {
                const int x = bx * 4 + k % 4, y = by * 4 + k / 4;
                if (x >= width || y >= height) continue;
                const auto& c = rgb[(bits >> (k * 2)) & 3];
                result.pixels[y * width + x] = (alpha[(alpha_bits >> (k * 3)) & 7] << 24) | (c[0] << 16) | (c[1] << 8) | c[2];
            }
        }
        return result;
    }
    return {};
}

// Only read named image resources. Firmware stays in the installed dev_flash;
// nothing from it is embedded in the application package.
// Field layout: https://github.com/kakaroto/RCOMage/blob/master/src/rcofile.h
template <typename Inflate>
std::map<std::string, image, std::less<>> read_rco_images(std::span<const std::uint8_t> file,
    std::span<const std::string> wanted, Inflate inflate)
{
    std::map<std::string, image, std::less<>> result;
    if (!region(file, 0, 164) || be32(file, 0) != 0x46525000 || file.size() > 32 * 1024 * 1024) return result;
    const auto compression = be32(file, 12) >> 4;
    std::vector<std::uint8_t> expanded;
    std::span<const std::uint8_t> table = file;
    if (compression == 1)
    {
        if (!region(file, 164, 12)) return result;
        const auto packed = be32(file, 164), unpacked = be32(file, 168);
        if (unpacked > 32 * 1024 * 1024 || !region(file, 176, packed)) return result;
        expanded.resize(164 + unpacked);
        std::copy_n(file.data(), 164, expanded.data());
        if (!inflate(std::span(expanded).subspan(164), file.subspan(176, packed))) return result;
        table = expanded;
    }
    else if (compression != 0) return result;
    const auto root = be32(file, 36), labels = be32(file, 64), labels_size = be32(file, 68);
    const auto image_data = be32(file, 128), image_size = be32(file, 132);
    if (!region(table, root, 40) || !region(table, labels, labels_size) || !region(file, image_data, image_size)) return result;
    const auto count = be32(table, root + 16);
    if (count > 4096) return result;
    std::size_t entry = std::size_t(root) + 40;
    for (std::uint32_t i = 0; i < count; ++i)
    {
        if (!region(table, entry, 56)) break;
        const auto label = be32(table, entry + 4), size = be32(table, entry + 12), next = be32(table, entry + 20);
        const auto step = size ? size : next;
        if (step < 56 || !region(table, entry, step)) break;
        if (label < labels_size)
        {
            const auto name_bytes = table.subspan(labels + label, labels_size - label);
            const auto end = std::find(name_bytes.begin(), name_bytes.end(), 0);
            if (end != name_bytes.end() && end - name_bytes.begin() < 256)
            {
                const std::string name(name_bytes.begin(), end);
                if (std::find(wanted.begin(), wanted.end(), name) != wanted.end())
                {
                    const auto format = be16(table, entry + 40), method = be16(table, entry + 42);
                    const auto packed = be32(table, entry + 44), offset = be32(table, entry + 48);
                    if (format == 5 && offset <= image_size && packed <= image_size - offset && packed <= 8 * 1024 * 1024)
                    {
                        auto data = file.subspan(std::size_t(image_data) + offset, packed);
                        std::vector<std::uint8_t> decoded;
                        bool valid = method == 0;
                        if (method == 1 && step >= 60)
                        {
                            const auto unpacked = be32(table, entry + 56);
                            if (unpacked <= 8 * 1024 * 1024)
                            {
                                decoded.resize(unpacked);
                                valid = inflate(std::span(decoded), data);
                                data = decoded;
                            }
                        }
                        if (valid) if (auto img = decode_gim(data)) result.emplace(name, std::move(img));
                    }
                }
            }
        }
        entry += step;
    }
    return result;
}
}
