#pragma once

// Persistent SPU LLVM object cache, pure part: SHA-256, the pack fingerprint and program keys, the pack file format
// (header, records, index scan, record reads), stale pack and size trimming, and the object symbol rule.
// No RPCS3 or LLVM dependency: file access goes through callbacks, so ps4-port/tests/spu_object_cache_test.cpp runs
// this exact code on the host.
//
// One pack per fingerprint (build stamp, LLVM target, SPU settings) next to the title's spu-*.dat:
//   header (128 bytes, little endian):
//     0 magic "RPS4SPUO" | 8 u32 format | 12 u32 header size (128) | 16 u32 key text size | 20 u32 0 | 24 u64 created
//     32 fingerprint[32] | 64 SHA-256 of the key text[32] | 96 SHA-256 of bytes 0..95 [32]
//   key text (key=value lines, the fingerprint input)
//   records, appended one by one (each program as soon as its object exists):
//     0 u32 'SOB1' | 4 u32 flags (bit 0: zlib) | 8 u32 stored size | 12 u32 object size | 16 key[32]
//     48 SHA-256 of the stored payload[32] | 80 SHA-256 of bytes 0..79, first 16 bytes | 96 payload
// A record is trusted only with its magic, both hashes and sizes right; it is checked when the pack is indexed and
// again when it is read for a load. A cut-off or damaged record header ends the index (the tail is cut); a damaged
// payload drops that record (the pack is rewritten without it).

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace utils::ps4::spu_objcache
{
using digest = std::array<std::uint8_t, 32>;

// Reads exactly 'size' bytes at 'offset'; false on a short read or an error
using read_fn = std::function<bool(std::uint64_t offset, void* dst, std::size_t size)>;
// Appends 'size' bytes; false on an error
using write_fn = std::function<bool(const void* src, std::size_t size)>;

inline constexpr char pack_magic[8] = {'R', 'P', 'S', '4', 'S', 'P', 'U', 'O'};
inline constexpr std::uint32_t pack_format = 1;
inline constexpr std::uint32_t header_size = 128;
inline constexpr std::uint32_t max_key_text = 64u << 10;
inline constexpr std::uint32_t record_magic = 0x31424f53; // "SOB1"
inline constexpr std::uint32_t record_header_size = 96;
inline constexpr std::uint32_t max_object_size = 4u << 20;
inline constexpr std::uint32_t max_stored_size = max_object_size + (max_object_size >> 4) + 4096;
inline constexpr std::uint32_t flag_zlib = 1;

inline constexpr std::uint64_t default_cap_bytes = 192ull << 20;
inline constexpr std::uint64_t min_cap_bytes = 32ull << 20;
inline constexpr std::uint64_t max_cap_bytes = 1024ull << 20;

// ---------------------------------------------------------------------------------------------------------------------
// SHA-256 (FIPS 180-4)

class sha256
{
    std::uint32_t m_h[8]{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    std::uint8_t m_block[64]{};
    std::uint64_t m_total = 0;
    std::size_t m_fill = 0;

    static std::uint32_t rotr(std::uint32_t x, int n) noexcept { return (x >> n) | (x << (32 - n)); }

    void compress(const std::uint8_t* p) noexcept
    {
        static constexpr std::uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

        std::uint32_t w[64];

        for (int i = 0; i < 16; i++)
        {
            w[i] = (std::uint32_t{p[i * 4]} << 24) | (std::uint32_t{p[i * 4 + 1]} << 16) | (std::uint32_t{p[i * 4 + 2]} << 8) | p[i * 4 + 3];
        }

        for (int i = 16; i < 64; i++)
        {
            const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }

        std::uint32_t a = m_h[0], b = m_h[1], c = m_h[2], d = m_h[3], e = m_h[4], f = m_h[5], g = m_h[6], h = m_h[7];

        for (int i = 0; i < 64; i++)
        {
            const std::uint32_t t1 = h + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + k[i] + w[i];
            const std::uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }

        m_h[0] += a; m_h[1] += b; m_h[2] += c; m_h[3] += d; m_h[4] += e; m_h[5] += f; m_h[6] += g; m_h[7] += h;
    }

public:
    void update(const void* data, std::size_t size) noexcept
    {
        const auto* p = static_cast<const std::uint8_t*>(data);
        m_total += size;

        if (m_fill)
        {
            const std::size_t take = std::min<std::size_t>(64 - m_fill, size);
            std::memcpy(m_block + m_fill, p, take);
            m_fill += take, p += take, size -= take;

            if (m_fill < 64)
            {
                return;
            }

            compress(m_block);
            m_fill = 0;
        }

        for (; size >= 64; p += 64, size -= 64)
        {
            compress(p);
        }

        if (size)
        {
            std::memcpy(m_block, p, size);
            m_fill = size;
        }
    }

    void update(std::string_view text) noexcept
    {
        update(text.data(), text.size());
    }

    digest finish() noexcept
    {
        const std::uint64_t bits = m_total * 8;
        const std::uint8_t pad = 0x80;
        update(&pad, 1);

        const std::uint8_t zero[64]{};
        update(zero, (m_fill <= 56 ? 56 - m_fill : 120 - m_fill));

        std::uint8_t length[8];

        for (int i = 0; i < 8; i++)
        {
            length[i] = static_cast<std::uint8_t>(bits >> (56 - 8 * i));
        }

        update(length, 8);

        digest out{};

        for (int i = 0; i < 8; i++)
        {
            out[i * 4] = static_cast<std::uint8_t>(m_h[i] >> 24);
            out[i * 4 + 1] = static_cast<std::uint8_t>(m_h[i] >> 16);
            out[i * 4 + 2] = static_cast<std::uint8_t>(m_h[i] >> 8);
            out[i * 4 + 3] = static_cast<std::uint8_t>(m_h[i]);
        }

        return out;
    }
};

inline digest sha256_of(const void* data, std::size_t size) noexcept
{
    sha256 ctx;
    ctx.update(data, size);
    return ctx.finish();
}

inline std::string hex(const std::uint8_t* data, std::size_t size)
{
    static constexpr char digits[] = "0123456789abcdef";
    std::string out(size * 2, '0');

    for (std::size_t i = 0; i < size; i++)
    {
        out[i * 2] = digits[data[i] >> 4];
        out[i * 2 + 1] = digits[data[i] & 15];
    }

    return out;
}

inline std::string hex(const digest& d)
{
    return hex(d.data(), d.size());
}

struct digest_hash
{
    std::size_t operator()(const digest& d) const noexcept
    {
        std::uint64_t v;
        std::memcpy(&v, d.data(), sizeof(v));
        return static_cast<std::size_t>(v);
    }
};

// ---------------------------------------------------------------------------------------------------------------------
// Fingerprint and program key

// key=value lines sorted by key (std::map order); line breaks in a value become spaces, '=' in a key becomes '_'
inline std::string canonical_text(const std::map<std::string, std::string>& keys)
{
    std::string out;

    for (const auto& [key, value] : keys)
    {
        for (char c : key)
        {
            out += (c == '\n' || c == '\r' || c == '=') ? '_' : c;
        }

        out += '=';

        for (char c : value)
        {
            out += (c == '\n' || c == '\r') ? ' ' : c;
        }

        out += '\n';
    }

    return out;
}

inline digest fingerprint(std::string_view text) noexcept
{
    return sha256_of(text.data(), text.size());
}

inline std::map<std::string, std::string> parse_canonical(std::string_view text)
{
    std::map<std::string, std::string> out;

    while (!text.empty())
    {
        const std::size_t end = text.find('\n');
        const std::string_view line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);

        if (const std::size_t eq = line.find('='); eq != std::string_view::npos)
        {
            out.emplace(std::string(line.substr(0, eq)), std::string(line.substr(eq + 1)));
        }
    }

    return out;
}

// "key: old -> new" for the first key (in key order) whose value differs; empty when the texts are equal
inline std::string first_difference(std::string_view old_text, std::string_view new_text)
{
    const auto a = parse_canonical(old_text);
    const auto b = parse_canonical(new_text);
    auto ia = a.begin();
    auto ib = b.begin();

    while (ia != a.end() || ib != b.end())
    {
        if (ib == b.end() || (ia != a.end() && ia->first < ib->first))
        {
            return ia->first + ": " + ia->second + " -> (none)";
        }

        if (ia == a.end() || ib->first < ia->first)
        {
            return ib->first + ": (none) -> " + ib->second;
        }

        if (ia->second != ib->second)
        {
            return ia->first + ": " + ia->second + " -> " + ib->second;
        }

        ++ia, ++ib;
    }

    return {};
}

// Key of one compiled program: its place and words (entry, lower bound, word count, SHA-1 of the words) and the
// SHA-256 of the module text the code generator receives. Equal keys under one fingerprint = the same object.
inline digest program_key(std::uint32_t entry, std::uint32_t lower_bound, std::uint32_t words, const std::uint8_t (&data_sha1)[20], const digest& module_hash) noexcept
{
    std::uint8_t buf[8 + 12 + 20 + 32];
    std::memcpy(buf, "SPUOBJK1", 8);

    for (int i = 0; i < 4; i++)
    {
        buf[8 + i] = static_cast<std::uint8_t>(entry >> (8 * i));
        buf[12 + i] = static_cast<std::uint8_t>(lower_bound >> (8 * i));
        buf[16 + i] = static_cast<std::uint8_t>(words >> (8 * i));
    }

    std::memcpy(buf + 20, data_sha1, 20);
    std::memcpy(buf + 40, module_hash.data(), 32);
    return sha256_of(buf, sizeof(buf));
}

// ---------------------------------------------------------------------------------------------------------------------
// Pack file

namespace detail
{
    inline void put32(std::uint8_t* p, std::uint32_t v) noexcept
    {
        for (int i = 0; i < 4; i++) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
    }

    inline void put64(std::uint8_t* p, std::uint64_t v) noexcept
    {
        for (int i = 0; i < 8; i++) p[i] = static_cast<std::uint8_t>(v >> (8 * i));
    }

    inline std::uint32_t get32(const std::uint8_t* p) noexcept
    {
        std::uint32_t v = 0;
        for (int i = 0; i < 4; i++) v |= std::uint32_t{p[i]} << (8 * i);
        return v;
    }

    inline std::uint64_t get64(const std::uint8_t* p) noexcept
    {
        std::uint64_t v = 0;
        for (int i = 0; i < 8; i++) v |= std::uint64_t{p[i]} << (8 * i);
        return v;
    }
}

// Header and key text of a new pack
inline std::vector<std::uint8_t> make_header(const digest& fp, std::string_view key_text, std::uint64_t created)
{
    std::vector<std::uint8_t> out(header_size + key_text.size());
    std::uint8_t* h = out.data();
    std::memcpy(h, pack_magic, 8);
    detail::put32(h + 8, pack_format);
    detail::put32(h + 12, header_size);
    detail::put32(h + 16, static_cast<std::uint32_t>(key_text.size()));
    detail::put32(h + 20, 0);
    detail::put64(h + 24, created);
    std::memcpy(h + 32, fp.data(), 32);
    const digest text_hash = sha256_of(key_text.data(), key_text.size());
    std::memcpy(h + 64, text_hash.data(), 32);
    const digest self = sha256_of(h, 96);
    std::memcpy(h + 96, self.data(), 32);
    std::memcpy(h + header_size, key_text.data(), key_text.size());
    return out;
}

enum class header_status
{
    ok,
    empty,        // shorter than a header (a pack whose creation was cut off)
    damaged,      // magic, sizes or a hash wrong
    other_format, // a pack of another format version
    other_key,    // valid, but of another fingerprint
    read_error,
};

struct header_info
{
    header_status status = header_status::damaged;
    std::uint64_t records_begin = 0;
    std::uint64_t created = 0;
    digest fp{};
    std::string key_text;
};

inline header_info parse_header(const read_fn& read, std::uint64_t file_size, const digest& expected_fp)
{
    header_info info;
    std::uint8_t h[header_size];

    if (file_size < header_size)
    {
        info.status = header_status::empty;
        return info;
    }

    if (!read(0, h, header_size))
    {
        info.status = header_status::read_error;
        return info;
    }

    if (std::memcmp(h, pack_magic, 8) != 0)
    {
        return info;
    }

    const digest self = sha256_of(h, 96);

    if (std::memcmp(self.data(), h + 96, 32) != 0 || detail::get32(h + 12) != header_size)
    {
        return info;
    }

    if (detail::get32(h + 8) != pack_format)
    {
        info.status = header_status::other_format;
        return info;
    }

    const std::uint32_t text_size = detail::get32(h + 16);

    if (text_size > max_key_text || header_size + std::uint64_t{text_size} > file_size)
    {
        return info;
    }

    info.key_text.resize(text_size);

    if (text_size && !read(header_size, info.key_text.data(), text_size))
    {
        info.status = header_status::read_error;
        return info;
    }

    const digest text_hash = sha256_of(info.key_text.data(), text_size);

    if (std::memcmp(text_hash.data(), h + 64, 32) != 0)
    {
        info.key_text.clear();
        return info;
    }

    std::memcpy(info.fp.data(), h + 32, 32);
    info.created = detail::get64(h + 24);
    info.records_begin = header_size + std::uint64_t{text_size};
    info.status = info.fp == expected_fp ? header_status::ok : header_status::other_key;
    return info;
}

// One whole record (header + payload), ready to append
inline std::vector<std::uint8_t> make_record(const digest& key, std::uint32_t flags, std::uint32_t object_size, const void* payload, std::size_t stored_size)
{
    std::vector<std::uint8_t> out(record_header_size + stored_size);
    std::uint8_t* r = out.data();
    detail::put32(r, record_magic);
    detail::put32(r + 4, flags);
    detail::put32(r + 8, static_cast<std::uint32_t>(stored_size));
    detail::put32(r + 12, object_size);
    std::memcpy(r + 16, key.data(), 32);
    const digest payload_hash = sha256_of(payload, stored_size);
    std::memcpy(r + 48, payload_hash.data(), 32);
    const digest self = sha256_of(r, 80);
    std::memcpy(r + 80, self.data(), 16);

    if (stored_size)
    {
        std::memcpy(r + record_header_size, payload, stored_size);
    }

    return out;
}

struct entry
{
    std::uint64_t offset = 0; // record start
    std::uint32_t stored_size = 0;
    std::uint32_t object_size = 0;
    std::uint32_t flags = 0;

    std::uint64_t bytes() const noexcept
    {
        return record_header_size + std::uint64_t{stored_size};
    }
};

struct record_header
{
    std::uint32_t flags = 0;
    std::uint32_t stored_size = 0;
    std::uint32_t object_size = 0;
    digest key{};
    digest payload_hash{};
};

// Magic, own hash and size limits of a record header
inline bool parse_record_header(const std::uint8_t* r, record_header& out) noexcept
{
    if (detail::get32(r) != record_magic)
    {
        return false;
    }

    const digest self = sha256_of(r, 80);

    if (std::memcmp(self.data(), r + 80, 16) != 0)
    {
        return false;
    }

    out.flags = detail::get32(r + 4);
    out.stored_size = detail::get32(r + 8);
    out.object_size = detail::get32(r + 12);
    std::memcpy(out.key.data(), r + 16, 32);
    std::memcpy(out.payload_hash.data(), r + 48, 32);

    if (out.stored_size > max_stored_size || out.object_size > max_object_size || !out.object_size || (out.flags & ~flag_zlib))
    {
        return false;
    }

    if (!(out.flags & flag_zlib) && out.stored_size != out.object_size)
    {
        return false;
    }

    return true;
}

struct scan_result
{
    std::unordered_map<digest, entry, digest_hash> index; // later records win
    std::uint64_t begin = 0;      // first record
    std::uint64_t good_end = 0;   // end of the last record read whole with a valid header (append position)
    std::uint64_t file_size = 0;
    std::uint32_t records = 0;    // records with a valid header and payload
    std::uint32_t damaged = 0;    // valid header, payload hash wrong: dropped
    std::uint32_t superseded = 0; // replaced by a later record of the same key
    std::uint64_t live_bytes = 0; // bytes of the records in the index
    std::uint64_t dead_bytes = 0; // damaged and superseded records
    bool read_error = false;

    // A cut-off or damaged record header: everything from good_end on is unusable
    bool tail_damaged() const noexcept
    {
        return !read_error && good_end < file_size;
    }
};

// Indexes the records from 'begin' with sequential reads of 'chunk' bytes. Stops at the first record whose header is
// cut off or wrong (good_end marks it); a record whose payload hash is wrong is skipped and counted.
inline scan_result scan(const read_fn& read, std::uint64_t begin, std::uint64_t file_size, std::size_t chunk = 4u << 20)
{
    scan_result res;
    res.begin = begin;
    res.good_end = begin;
    res.file_size = file_size;

    std::vector<std::uint8_t> buf;
    std::uint64_t buf_start = 0;
    std::size_t buf_size = 0;

    // Makes [offset, offset + size) available in buf; false at a read error (offset + size <= file_size checked first)
    const auto need = [&](std::uint64_t offset, std::size_t size) -> const std::uint8_t*
    {
        if (offset >= buf_start && offset + size <= buf_start + buf_size)
        {
            return buf.data() + (offset - buf_start);
        }

        const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(std::max<std::size_t>(chunk, size), file_size - offset));

        if (buf.size() < want)
        {
            buf.resize(want);
        }

        if (!read(offset, buf.data(), want))
        {
            return nullptr;
        }

        buf_start = offset;
        buf_size = want;
        return buf.data();
    };

    std::uint64_t pos = begin;

    while (pos < file_size)
    {
        if (file_size - pos < record_header_size)
        {
            break;
        }

        const std::uint8_t* r = need(pos, record_header_size);

        if (!r)
        {
            res.read_error = true;
            break;
        }

        record_header h;

        if (!parse_record_header(r, h) || file_size - pos - record_header_size < h.stored_size)
        {
            break;
        }

        const std::uint8_t* p = need(pos, record_header_size + std::size_t{h.stored_size});

        if (!p)
        {
            res.read_error = true;
            break;
        }

        const digest payload_hash = sha256_of(p + record_header_size, h.stored_size);
        const entry e{pos, h.stored_size, h.object_size, h.flags};
        pos += e.bytes();
        res.good_end = pos;

        if (payload_hash != h.payload_hash)
        {
            res.damaged++;
            res.dead_bytes += e.bytes();
            continue;
        }

        res.records++;

        if (auto [it, added] = res.index.emplace(h.key, e); !added)
        {
            res.superseded++;
            res.dead_bytes += it->second.bytes();
            res.live_bytes -= it->second.bytes();
            it->second = e;
        }

        res.live_bytes += e.bytes();
    }

    return res;
}

// Reads one indexed record again and checks it whole (magic, header hash, key, sizes, payload hash): the bytes handed
// back are the bytes that were checked, whatever happened to the file since the scan.
inline bool read_record(const read_fn& read, const entry& e, const digest& key, std::vector<std::uint8_t>& payload)
{
    std::uint8_t r[record_header_size];
    record_header h;

    if (!read(e.offset, r, record_header_size) || !parse_record_header(r, h))
    {
        return false;
    }

    if (h.key != key || h.stored_size != e.stored_size || h.object_size != e.object_size || h.flags != e.flags)
    {
        return false;
    }

    payload.resize(h.stored_size);

    if (h.stored_size && !read(e.offset + record_header_size, payload.data(), h.stored_size))
    {
        return false;
    }

    return sha256_of(payload.data(), payload.size()) == h.payload_hash;
}

// Records kept by a rewrite: every indexed record in file order; above 'limit' bytes of records the oldest (first in
// the file) are dropped until the rest fits.
inline std::vector<entry> select_for_rewrite(const scan_result& res, std::uint64_t limit)
{
    std::vector<entry> kept;
    kept.reserve(res.index.size());

    for (const auto& [key, e] : res.index)
    {
        kept.push_back(e);
    }

    std::sort(kept.begin(), kept.end(), [](const entry& a, const entry& b) { return a.offset < b.offset; });

    std::uint64_t total = 0;

    for (const entry& e : kept)
    {
        total += e.bytes();
    }

    std::size_t first = 0;

    while (total > limit && first < kept.size())
    {
        total -= kept[first++].bytes();
    }

    kept.erase(kept.begin(), kept.begin() + static_cast<std::ptrdiff_t>(first));
    return kept;
}

// A pack is rewritten (temporary file + rename) when it holds damaged records, when dropped and superseded records
// are more than a quarter of it and 4 MiB, or when it is above its cap.
inline bool needs_rewrite(const scan_result& res, std::uint64_t cap) noexcept
{
    if (res.damaged)
    {
        return true;
    }

    if (res.dead_bytes > (4ull << 20) && res.dead_bytes * 4 > res.good_end)
    {
        return true;
    }

    return res.begin + res.live_bytes > cap;
}

// Copies records (checked again one by one) to 'write'; returns the number copied, or -1 on a write error.
// A record that no longer reads back whole is left out.
inline long long copy_records(const read_fn& read, const std::vector<entry>& records, const std::unordered_map<digest, entry, digest_hash>& index, const write_fn& write)
{
    // Key of each kept record (entries do not carry it)
    std::unordered_map<std::uint64_t, const digest*> keys;
    keys.reserve(index.size());

    for (const auto& [key, e] : index)
    {
        keys.emplace(e.offset, &key);
    }

    long long copied = 0;
    std::vector<std::uint8_t> payload;

    for (const entry& e : records)
    {
        const auto it = keys.find(e.offset);

        if (it == keys.end() || !read_record(read, e, *it->second, payload))
        {
            continue;
        }

        const std::vector<std::uint8_t> rec = make_record(*it->second, e.flags, e.object_size, payload.data(), payload.size());

        if (!write(rec.data(), rec.size()))
        {
            return -1;
        }

        copied++;
    }

    return copied;
}

// ---------------------------------------------------------------------------------------------------------------------
// Pack files of one cache folder

// spu-obj-v1-<block>-<first 16 hex digits of the fingerprint>.pack
inline std::string pack_name(std::string_view block_size, const digest& fp)
{
    std::string name = "spu-obj-v";
    name += std::to_string(pack_format);
    name += '-';

    for (char c : block_size)
    {
        name += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) ? c : '_';
    }

    name += '-';
    name += hex(fp.data(), 8);
    name += ".pack";
    return name;
}

// A pack of any format version (or its leftover temporary file)
inline bool is_pack_file(std::string_view name) noexcept
{
    if (name.substr(0, 9) != "spu-obj-v")
    {
        return false;
    }

    const auto ends_with = [&](std::string_view suffix) { return name.size() >= suffix.size() && name.substr(name.size() - suffix.size()) == suffix; };
    return ends_with(".pack") || ends_with(".pack.tmp");
}

struct pack_file
{
    std::string name;
    std::uint64_t size = 0;
    std::int64_t mtime = 0;
};

// Packs to delete in one folder: temporary leftovers, then all but the newest 'keep_others' packs other than the
// current one, then (oldest first) whatever makes the folder exceed 'budget' bytes together with the current pack.
inline std::vector<std::string> stale_packs(std::vector<pack_file> packs, std::string_view current, std::size_t keep_others, std::uint64_t budget)
{
    std::vector<std::string> remove;
    std::uint64_t total = 0;
    std::vector<pack_file> others;

    for (pack_file& p : packs)
    {
        if (!is_pack_file(p.name))
        {
            continue;
        }

        if (p.name.size() >= 4 && p.name.substr(p.name.size() - 4) == ".tmp")
        {
            remove.push_back(p.name);
        }
        else if (p.name == current)
        {
            total += p.size;
        }
        else
        {
            others.push_back(std::move(p));
        }
    }

    std::sort(others.begin(), others.end(), [](const pack_file& a, const pack_file& b)
    {
        return a.mtime != b.mtime ? a.mtime > b.mtime : a.name < b.name;
    });

    for (std::size_t i = 0; i < others.size(); i++)
    {
        if (i < keep_others && total + others[i].size <= budget)
        {
            total += others[i].size;
            continue;
        }

        remove.push_back(others[i].name);
    }

    return remove;
}

// ---------------------------------------------------------------------------------------------------------------------
// Symbol rule: an object is loadable only if it defines the program's entry symbol and every symbol it needs from
// outside is declared by the module being compiled now (helpers, patchpoints, globals mapped by this compile) or is
// one of the C library routines registered with the JIT's symbol resolver.

inline bool is_libcall(std::string_view name) noexcept
{
    static constexpr std::string_view names[] = {
        "memcpy", "memmove", "memset", "memcmp", "fma", "fmaf", "_GLOBAL_OFFSET_TABLE_",
        "sqrt", "sqrtf", "cbrt", "cbrtf", "sin", "sinf", "cos", "cosf", "tan", "tanf", "asin", "asinf", "acos", "acosf",
        "atan", "atanf", "exp", "expf", "exp2", "exp2f", "log", "logf", "log2", "log2f", "log10", "log10f",
        "floor", "floorf", "ceil", "ceilf", "trunc", "truncf", "round", "roundf", "rint", "rintf", "nearbyint", "nearbyintf",
        "pow", "powf", "atan2", "atan2f", "fmod", "fmodf", "remainder", "remainderf", "copysign", "copysignf"};

    for (std::string_view n : names)
    {
        if (n == name)
        {
            return true;
        }
    }

    return false;
}

struct symbol_check
{
    bool ok = false;
    const char* reason = "";
    std::string name;
};

inline symbol_check check_symbols(std::string_view main, const std::vector<std::string>& defined, const std::vector<std::string>& undefined,
    const std::function<bool(std::string_view)>& declared)
{
    symbol_check res;

    if (std::find(defined.begin(), defined.end(), main) == defined.end())
    {
        res.reason = "entry symbol not defined";
        res.name = std::string(main);
        return res;
    }

    for (const std::string& name : undefined)
    {
        if (name == main)
        {
            res.reason = "entry symbol undefined";
            res.name = name;
            return res;
        }

        if (!declared(name) && !is_libcall(name))
        {
            res.reason = "unknown symbol";
            res.name = name;
            return res;
        }
    }

    res.ok = true;
    return res;
}

// ---------------------------------------------------------------------------------------------------------------------
// Build stamp: a 64-byte array in the ELF = 24-byte marker + 32-byte payload + 8 zero bytes. The build leaves the
// payload as the placeholder 0x00..0x1f; ps4-port/tools/stamp_build.py writes the SHA-256 of the file bytes of every
// PT_LOAD segment (payload zeroed) into it after linking. The marker text is never spelled anywhere else in the
// program, so the tool finds exactly one copy.

inline constexpr std::size_t stamp_marker_size = 24;
inline constexpr std::size_t stamp_payload_size = 32;

// Hex of a stamped payload; empty for the placeholder or an all-zero payload (unstamped build: the cache stays off)
inline std::string stamp_payload_hex(const std::uint8_t* payload)
{
    bool placeholder = true;
    bool zero = true;

    for (std::size_t i = 0; i < stamp_payload_size; i++)
    {
        placeholder = placeholder && payload[i] == i;
        zero = zero && payload[i] == 0;
    }

    return placeholder || zero ? std::string() : hex(payload, stamp_payload_size);
}

#ifdef RPCS3_PS4
// Stamp of the running ELF (64 hex digits), empty when unstamped (defined in SPULLVMRecompiler.cpp)
std::string build_stamp_hex();
#endif

// Pack size cap from the text of spu-objcache-mib.txt (MiB, clamped to 32..1024); the default when empty or invalid
inline std::uint64_t cap_from_text(std::string_view text) noexcept
{
    std::uint64_t mib = 0;
    bool digits = false;

    for (char c : text)
    {
        if (c >= '0' && c <= '9')
        {
            digits = true;
            mib = mib * 10 + static_cast<std::uint64_t>(c - '0');

            if (mib > 1u << 20)
            {
                return max_cap_bytes;
            }
        }
        else if (digits || (c != ' ' && c != '\t' && c != '\r' && c != '\n'))
        {
            break;
        }
    }

    if (!digits)
    {
        return default_cap_bytes;
    }

    return std::clamp<std::uint64_t>(mib << 20, min_cap_bytes, max_cap_bytes);
}

// ---------------------------------------------------------------------------------------------------------------------
// How the SPU decrementer reads find g_timebase_offs. 'named': an external global mapped by each compile, so no
// process address is part of the module text and those programs can hit. 'baked': the 0.37 code (the address as an
// IR constant; such a program only hits while the address is the same). Decided once per process, before the first
// SPU compile, by the first boot that opens a pack ('named' unless forced) or that finds the cache off for any reason
// ('baked'); a compile that comes first decides 'baked'. The mode in force is part of the pack fingerprint.

enum class timebase_mode : std::uint8_t
{
    undecided = 0,
    baked = 1,
    named = 2,
};

inline const char* timebase_text(timebase_mode mode) noexcept
{
    return mode == timebase_mode::named ? "named" : mode == timebase_mode::baked ? "baked" : "undecided";
}

// Mode a pack opened now would be keyed with: the one in force, else 'named' unless spu-timebase-baked.flag
inline timebase_mode timebase_for_open(timebase_mode current, bool force_baked) noexcept
{
    return current != timebase_mode::undecided ? current : force_baked ? timebase_mode::baked : timebase_mode::named;
}

// First decision of the process wins; returns the mode in force
inline timebase_mode timebase_settle(std::atomic<std::uint8_t>& state, timebase_mode wanted) noexcept
{
    std::uint8_t expected = 0;
    state.compare_exchange_strong(expected, static_cast<std::uint8_t>(wanted));
    return static_cast<timebase_mode>(state.load());
}
}
