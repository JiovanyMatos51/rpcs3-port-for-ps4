// Host test of util/ps4_xmb_media.hpp: settings files, file names, the RIFF/WAVE parser (with random mutations, also
// built with ASan + UBSan), geometry, gain ramps, the PCM ring, the music mixer, the director, the release order, the
// XMD1 record and the cache trim. Every test input is synthetic and stays in memory (or in a temporary folder for the
// settings files); no game file is read or written.
//   clang++ -std=c++23 -O2 -Wall -Wextra -I rpcs3 -I . -static ps4-port/tests/xmb_media_test.cpp

#include "util/ps4_xmb_media.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <random>
#include <string>
#include <vector>

namespace xm = utils::ps4::xmb_media;
namespace stdfs = std::filesystem;

namespace
{
int g_checks = 0;
int g_failures = 0;

#define CHECK(cond) check(static_cast<bool>(cond), #cond, __LINE__)

void check(bool ok, const char* what, int line)
{
    ++g_checks;
    if (ok) return;
    ++g_failures;
    std::printf("FAIL line %d: %s\n", line, what);
}

// ---------------------------------------------------------------------------------------------------------------------
// Synthetic RIFF builders

using bytes = std::vector<std::uint8_t>;

void put16(bytes& b, std::uint32_t v)
{
    b.push_back(static_cast<std::uint8_t>(v));
    b.push_back(static_cast<std::uint8_t>(v >> 8));
}
void put32(bytes& b, std::uint32_t v)
{
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}

struct chunk
{
    std::string id;
    bytes body;
    std::optional<std::uint32_t> declared; // size field when it should lie
};

bytes riff(const std::vector<chunk>& chunks, std::optional<std::uint32_t> riff_size = {})
{
    bytes b{'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E'};
    for (const chunk& c : chunks)
    {
        b.insert(b.end(), c.id.begin(), c.id.end());
        put32(b, c.declared.value_or(static_cast<std::uint32_t>(c.body.size())));
        b.insert(b.end(), c.body.begin(), c.body.end());
        if (c.body.size() & 1) b.push_back(0);
    }
    const std::uint32_t size = riff_size.value_or(static_cast<std::uint32_t>(b.size() - 8));
    for (int i = 0; i < 4; ++i) b[4 + i] = static_cast<std::uint8_t>(size >> (8 * i));
    return b;
}

chunk fmt_atrac3(std::uint16_t channels = 2, std::uint32_t rate = 44100, std::uint16_t block = 384)
{
    bytes f;
    put16(f, 0x0270);
    put16(f, channels);
    put32(f, rate);
    put32(f, 16538);
    put16(f, block);
    put16(f, 0);
    put16(f, 14); // codec bytes
    put16(f, 1);
    put32(f, 0x1000);
    put16(f, 0);
    put16(f, 0);
    put16(f, 1);
    put16(f, 0);
    return {"fmt ", f, {}};
}

chunk fmt_atrac3plus(std::uint16_t channels = 2, std::uint32_t rate = 44100, std::uint16_t block = 0x230)
{
    bytes f;
    put16(f, 0xFFFE);
    put16(f, channels);
    put32(f, rate);
    put32(f, 12058);
    put16(f, block);
    put16(f, 0);
    put16(f, 34);
    put16(f, 0);      // valid bits
    put32(f, 3);      // channel mask
    f.insert(f.end(), xm::atrac3plus_guid.begin(), xm::atrac3plus_guid.end());
    for (int i = 0; i < 12; ++i) f.push_back(static_cast<std::uint8_t>(i));
    return {"fmt ", f, {}};
}

chunk fmt_pcm(std::uint16_t channels = 2, std::uint32_t rate = 48000)
{
    bytes f;
    put16(f, 1);
    put16(f, channels);
    put32(f, rate * channels * 2);
    put32(f, rate * channels * 2);
    put16(f, static_cast<std::uint16_t>(channels * 2));
    put16(f, 16);
    // fix byte rate / sample rate order
    bytes g;
    put16(g, 1);
    put16(g, channels);
    put32(g, rate);
    put32(g, rate * channels * 2);
    put16(g, static_cast<std::uint16_t>(channels * 2));
    put16(g, 16);
    return {"fmt ", g, {}};
}

chunk fact(std::uint32_t total, std::uint32_t delay = 0x800)
{
    bytes f;
    put32(f, total);
    put32(f, delay);
    return {"fact", f, {}};
}

chunk smpl(std::uint32_t start, std::uint32_t end)
{
    bytes f(28, 0);
    put32(f, 1); // loops
    put32(f, 0); // sampler data
    put32(f, 0); // cue id
    put32(f, 0); // type
    put32(f, start);
    put32(f, end);
    put32(f, 0);
    put32(f, 0);
    return {"smpl", f, {}};
}

chunk data(std::size_t n)
{
    bytes d(n);
    for (std::size_t i = 0; i < n; ++i) d[i] = static_cast<std::uint8_t>(i * 7 + 3);
    return {"data", d, {}};
}

xm::riff_info parse(const bytes& b)
{
    // An exact-size heap copy, so a sanitizer build sees any read past the end
    std::unique_ptr<std::uint8_t[]> copy(new std::uint8_t[b.size() ? b.size() : 1]);
    if (!b.empty()) std::memcpy(copy.get(), b.data(), b.size());
    return xm::parse_riff({copy.get(), b.size()});
}

void test_riff()
{
    // ATRAC3: 100 blocks of 384 bytes, 1024 samples each
    {
        const auto i = parse(riff({fmt_atrac3(), fact(100000), data(384 * 100)}));
        CHECK(i.ok);
        CHECK(i.codec == xm::riff_codec::atrac3);
        CHECK(i.channels == 2 && i.sample_rate == 44100 && i.block_align == 384);
        CHECK(i.extra_size == 14);
        CHECK(i.total_samples == 100000); // fact below the 102400 the blocks hold
        CHECK(i.fact_delay == 0x800);
        CHECK(xm::packet_count(i) == 100 && xm::packet_samples(i.codec) == 1024);
        CHECK(!i.loop);
    }
    // ATRAC3plus with a valid loop
    {
        const auto i = parse(riff({fmt_atrac3plus(), fact(204800), smpl(4096, 150000), data(0x230 * 100)}));
        CHECK(i.ok);
        CHECK(i.codec == xm::riff_codec::atrac3plus);
        CHECK(i.extra_size == 12);
        CHECK(i.total_samples == 204800);
        CHECK(i.loop && i.loop_start == 4096 && i.loop_end == 150001);
        CHECK(xm::packet_samples(i.codec) == 2048 && xm::packet_count(i) == 100);
    }
    // smpl end <= start, end past the total, too short: no loop (the file still plays)
    {
        CHECK(!parse(riff({fmt_atrac3plus(), fact(204800), smpl(5000, 5000), data(0x230 * 100)})).loop);
        CHECK(!parse(riff({fmt_atrac3plus(), fact(204800), smpl(5000, 4000), data(0x230 * 100)})).loop);
        CHECK(!parse(riff({fmt_atrac3plus(), fact(204800), smpl(0, 204800), data(0x230 * 100)})).loop);
        CHECK(parse(riff({fmt_atrac3plus(), fact(204800), smpl(0, 204799), data(0x230 * 100)})).loop);
        CHECK(!parse(riff({fmt_atrac3plus(), fact(204800), smpl(100, 200), data(0x230 * 100)})).loop);
        const auto i = parse(riff({fmt_atrac3plus(), fact(204800), smpl(5000, 4000), data(0x230 * 100)}));
        CHECK(i.ok);
    }
    // fact past the data is clipped to what the blocks hold; no fact: all blocks
    {
        CHECK(parse(riff({fmt_atrac3plus(), fact(999999), data(0x230 * 10)})).total_samples == 20480);
        CHECK(parse(riff({fmt_atrac3plus(), data(0x230 * 10 + 7)})).total_samples == 20480);
        CHECK(parse(riff({fmt_atrac3plus(), fact(0), data(0x230 * 10)})).total_samples == 20480);
    }
    // PCM16 (odd data length: whole frames only, then padding)
    {
        const auto i = parse(riff({fmt_pcm(2, 48000), data(4 * 3000 + 3)}));
        CHECK(i.ok && i.codec == xm::riff_codec::pcm16);
        CHECK(i.data_size == 12000 && i.total_samples == 3000);
        CHECK(xm::packet_count(i) == 3);
        const auto m = parse(riff({fmt_pcm(1, 22050), data(2 * 1024)}));
        CHECK(m.ok && m.channels == 1 && m.total_samples == 1024);
    }
    // Odd chunk padding before the data
    {
        chunk odd{"LIST", bytes(5, 'x'), {}};
        const auto i = parse(riff({fmt_atrac3plus(), odd, data(0x230 * 4)}));
        CHECK(i.ok && i.chunks == 3);
    }
    // Refusals
    {
        CHECK(!parse({}).ok);
        CHECK(!parse(bytes{'R', 'I', 'F', 'F'}).ok);
        bytes notwave = riff({fmt_pcm(), data(400)});
        notwave[8] = 'X';
        CHECK(!parse(notwave).ok);
        CHECK(!parse(riff({data(400), fmt_pcm()})).ok); // data before fmt
        CHECK(!parse(riff({fmt_pcm()})).ok);            // no data
        CHECK(!parse(riff({data(400)})).ok);            // no fmt
        CHECK(!parse(riff({fmt_pcm(), fmt_pcm(), data(400)})).ok);
        CHECK(!parse(riff({fmt_atrac3(6), data(4000)})).ok);         // 6 channels
        CHECK(!parse(riff({fmt_atrac3(2, 96000), data(4000)})).ok);  // 96 kHz
        CHECK(!parse(riff({fmt_atrac3(2, 8000), data(4000)})).ok);   // 8 kHz
        CHECK(!parse(riff({fmt_atrac3(2, 44100, 0), data(4000)})).ok);
        CHECK(!parse(riff({fmt_atrac3plus(), data(0x22f)})).ok);     // less than one block
        chunk tag = fmt_pcm();
        tag.body[0] = 0x55; // MP3
        const auto mp3 = parse(riff({tag, data(400)}));
        CHECK(!mp3.ok && mp3.format_tag == 0x55);
        chunk guid = fmt_atrac3plus();
        guid.body[24] ^= 1;
        CHECK(!parse(riff({guid, data(0x230 * 4)})).ok);
        chunk shortext = fmt_atrac3plus();
        shortext.body[16] = 10;
        CHECK(!parse(riff({shortext, data(0x230 * 4)})).ok);
        chunk a3 = fmt_atrac3();
        a3.body[16] = 12; // ATRAC3 without its 14 codec bytes
        CHECK(!parse(riff({a3, data(4000)})).ok);
        chunk tiny{"fmt ", bytes(10, 0), {}};
        CHECK(!parse(riff({tiny, data(400)})).ok);
        // 8-bit PCM
        chunk pcm8 = fmt_pcm();
        pcm8.body[14] = 8;
        CHECK(!parse(riff({pcm8, data(400)})).ok);
    }
    // 64 chunks are read, 65 are refused
    {
        std::vector<chunk> many{fmt_pcm()};
        for (int i = 0; i < 62; ++i) many.push_back({"junk", bytes(2, 0), {}});
        many.push_back(data(400));
        CHECK(parse(riff(many)).ok);
        many.insert(many.begin() + 1, chunk{"junk", bytes(2, 0), {}});
        const auto i = parse(riff(many));
        CHECK(!i.ok && i.chunks == 65);
    }
    // Sizes past the end: the data chunk is clipped to the buffer; a RIFF size past the end is ignored
    {
        chunk big = data(0x230 * 3);
        big.declared = 0xFFFFFFF0u;
        const auto i = parse(riff({fmt_atrac3plus(), big}));
        CHECK(i.ok && i.data_size == 0x230 * 3);
        const bytes b = riff({fmt_atrac3plus(), data(0x230 * 3)}, 0xFFFFFFFFu);
        CHECK(parse(b).ok);
        chunk hugefmt = fmt_atrac3plus();
        hugefmt.declared = 0x7FFFFFFFu;
        CHECK(!parse(riff({hugefmt, data(0x230 * 3)})).ok); // the fmt swallows the data chunk
    }
    // Truncated at every byte: never ok with fewer bytes than the header and data need, never a read past the end
    {
        const bytes full = riff({fmt_atrac3plus(), fact(4096), smpl(0, 3000), data(0x230 * 2)});
        int ok = 0;
        for (std::size_t n = 0; n <= full.size(); ++n)
        {
            const auto i = parse(bytes(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(n)));
            if (!i.ok) continue;
            ++ok;
            CHECK(i.data_offset + i.data_size <= n);
        }
        CHECK(ok > 0);
    }
}

// Random mutations of three valid files; a sanitizer build of this test proves no read leaves the buffer
void test_riff_mutations()
{
    const std::vector<bytes> seeds{
        riff({fmt_atrac3(), fact(100000), smpl(1024, 50000), data(384 * 20)}),
        riff({fmt_atrac3plus(), fact(40960), smpl(2048, 30000), data(0x230 * 20)}),
        riff({fmt_pcm(1, 32000), {"LIST", bytes(9, 'a'), {}}, data(2 * 2000)}),
    };
    std::mt19937 rng(0x5a3d0u);
    int accepted = 0;
    std::uint64_t touched = 0;
    for (int round = 0; round < 10000; ++round)
    {
        bytes b = seeds[static_cast<std::size_t>(round) % seeds.size()];
        const int edits = 1 + static_cast<int>(rng() % 8);
        for (int e = 0; e < edits && !b.empty(); ++e)
        {
            const std::size_t at = rng() % b.size();
            switch (rng() % 6)
            {
            case 0: b[at] ^= static_cast<std::uint8_t>(1u << (rng() % 8)); break;
            case 1: b[at] = (rng() & 1) ? 0x00 : 0xFF; break;
            case 2:
            {
                static constexpr std::uint32_t edge[] = {0, 1, 2, 3, 4, 7, 8, 14, 16, 18, 22, 40, 0x7FFFFFFF, 0x80000000u, 0xFFFFFFFFu, 0xFFFFFFFEu};
                const std::uint32_t v = (rng() & 1) ? edge[rng() % std::size(edge)] : static_cast<std::uint32_t>(rng());
                for (int i = 0; i < 4 && at + i < b.size(); ++i) b[at + i] = static_cast<std::uint8_t>(v >> (8 * i));
                break;
            }
            case 3: b.resize(rng() % (b.size() + 1)); break;
            case 4:
            {
                const std::size_t n = 1 + rng() % 9;
                bytes junk(n);
                for (auto& c : junk) c = static_cast<std::uint8_t>(rng());
                b.insert(b.begin() + static_cast<std::ptrdiff_t>(at), junk.begin(), junk.end());
                break;
            }
            default: b.erase(b.begin() + static_cast<std::ptrdiff_t>(at), b.begin() + static_cast<std::ptrdiff_t>(std::min(b.size(), at + 1 + rng() % 9))); break;
            }
        }
        std::unique_ptr<std::uint8_t[]> copy(new std::uint8_t[b.size() ? b.size() : 1]);
        if (!b.empty()) std::memcpy(copy.get(), b.data(), b.size());
        const std::span<const std::uint8_t> view(copy.get(), b.size());
        const xm::riff_info i = xm::parse_riff(view);
        if (!i.ok) continue;
        ++accepted;
        // What the console part reads of an accepted file stays inside it
        CHECK(i.data_offset <= b.size() && i.data_size <= b.size() - i.data_offset);
        CHECK(i.extra_offset + i.extra_size <= b.size());
        CHECK(i.channels >= 1 && i.channels <= 2);
        CHECK(i.total_samples > 0);
        CHECK(!i.loop || (i.loop_start < i.loop_end && i.loop_end <= i.total_samples));
        const std::uint64_t pb = xm::packet_bytes(i), pc = xm::packet_count(i);
        CHECK(pb > 0 && pc > 0);
        for (std::uint64_t k = 0; k < pc; ++k)
        {
            const std::uint64_t off = k * pb;
            const std::uint64_t n = std::min<std::uint64_t>(pb, i.data_size - off);
            touched += view[static_cast<std::size_t>(i.data_offset + off)] + view[static_cast<std::size_t>(i.data_offset + off + n - 1)];
        }
        for (std::uint64_t k = 0; k < i.extra_size; ++k) touched += view[static_cast<std::size_t>(i.extra_offset + k)];
    }
    std::printf("riff mutations: 10000 files, %d accepted (checksum %llu)\n", accepted, static_cast<unsigned long long>(touched & 0xffff));
    CHECK(accepted > 100);
}

// ---------------------------------------------------------------------------------------------------------------------

void test_geometry()
{
    CHECK((xm::background_crop(1920, 1080) == xm::rect{0, 0, 1920, 1080}));
    CHECK((xm::background_crop(1280, 720) == xm::rect{0, 0, 1280, 720}));
    CHECK((xm::background_crop(1920, 1200) == xm::rect{0, 0, 1920, 1200})); // 16:10, within 10 %: stretched
    CHECK((xm::background_crop(1000, 1000) == xm::rect{0, 218, 1000, 563}));
    CHECK((xm::background_crop(2048, 600) == xm::rect{490, 0, 1067, 600}));
    CHECK((xm::background_crop(640, 480) == xm::rect{0, 60, 640, 360})); // 4:3 is outside the 10 %
    CHECK((xm::background_crop(1, 1) == xm::rect{0, 0, 1, 1}));
    CHECK((xm::background_crop(0, 5) == xm::rect{}));
    CHECK((xm::background_crop(5, -1) == xm::rect{}));
    for (int w : {1, 2, 3, 17, 640, 1000, 1920, 2048})
        for (int h : {1, 2, 3, 17, 480, 1000, 1080, 2048})
        {
            const xm::rect r = xm::background_crop(w, h);
            CHECK(r.w >= 1 && r.h >= 1 && r.x >= 0 && r.y >= 0 && r.x + r.w <= w && r.y + r.h <= h);
        }

    CHECK((xm::overlay_box(1000, 560) == xm::rect{613, 173, 667, 373}));
    CHECK((xm::overlay_box(310, 250) == xm::rect{1073, 276, 207, 167}));
    CHECK((xm::overlay_box(1, 1) == xm::rect{1279, 359, 1, 1}));
    CHECK((xm::overlay_box(2048, 2048) == xm::rect{560, 0, 720, 720}));
    CHECK((xm::overlay_box(2048, 100) == xm::rect{0, 328, 1280, 63})); // 1365x67 at 720 lines, fitted to the width
    CHECK((xm::overlay_box(0, 10) == xm::rect{}));
    for (int w : {1, 3, 310, 1000, 1500, 2048})
        for (int h : {1, 3, 250, 560, 1500, 2048})
        {
            const xm::rect r = xm::overlay_box(w, h);
            CHECK(r.w >= 1 && r.h >= 1 && r.x >= 0 && r.y >= 0 && r.x + r.w == xm::screen_width && r.y + r.h <= xm::screen_height);
        }

    const std::uint8_t rgb[] = {1, 2, 3, 250, 251, 252};
    std::uint32_t out[2];
    xm::pack_rgb(rgb, 2, out);
    CHECK(out[0] == 0xff010203u && out[1] == 0xfffafbfcu);
    const std::uint8_t rgba[] = {1, 2, 3, 0, 9, 8, 7, 128};
    xm::pack_rgba(rgba, 2, out);
    CHECK(out[0] == 0x00010203u && out[1] == 0x80090807u);
}

void test_resolve()
{
    CHECK(xm::source_of("/data/rpcs3/games/Game.iso") == xm::source_kind::iso);
    CHECK(xm::source_of("/mnt/usb0/PS3ISO/GAME.ISO") == xm::source_kind::iso);
    CHECK(xm::source_of("/data/rpcs3/dev_hdd0/game/NPUB12345") == xm::source_kind::files);
    CHECK(xm::source_of("/mnt/usb0/PS3/GAMES/BLUS30443") == xm::source_kind::files);
    CHECK(xm::source_of("/mnt/usb0/PS3/GAMES/BLUS30443/PS3_GAME/USRDIR/EBOOT.BIN") == xm::source_kind::files);
    CHECK(xm::source_of("/data/rpcs3/packages/game.pkg") == xm::source_kind::none);
    CHECK(xm::source_of("/data/rpcs3/packages/GAME.PKG") == xm::source_kind::none);
    CHECK(xm::source_of("@disc:install") == xm::source_kind::none);
    CHECK(xm::source_of("@folder:3") == xm::source_kind::none);
    CHECK(xm::source_of("@folder-empty:1") == xm::source_kind::none);
    CHECK(xm::source_of("/vfsv0_virtual_iso_overlay_fs_dev/PS3_GAME") == xm::source_kind::none);
    CHECK(xm::source_of("") == xm::source_kind::none);
    CHECK(xm::source_of("/mnt/usb0/my.iso.games/GAME") == xm::source_kind::files); // the dot is in a folder name

    CHECK(xm::files_base("/mnt/usb0/PS3/GAMES/BLUS30443", true, true) == "/mnt/usb0/PS3/GAMES/BLUS30443/PS3_GAME/");
    CHECK(xm::files_base("/data/rpcs3/dev_hdd0/game/NPUB12345/", true, false) == "/data/rpcs3/dev_hdd0/game/NPUB12345/");
    CHECK(xm::files_base("/mnt/usb0/G/PS3_GAME/USRDIR/EBOOT.BIN", false, false) == "/mnt/usb0/G/PS3_GAME/USRDIR/../");

    const auto pic1 = xm::candidate_names(xm::media_kind::background, 1, true);
    CHECK(pic1.size() == 2 && pic1[0] == "PIC1_01.PNG" && pic1[1] == "PIC1.PNG");
    const auto pic0 = xm::candidate_names(xm::media_kind::overlay, 10, true);
    CHECK(pic0.size() == 2 && pic0[0] == "PIC0_10.PNG" && pic0[1] == "PIC0.PNG");
    const auto pic2 = xm::candidate_names(xm::media_kind::overlay, 0, false);
    CHECK(pic2.size() == 2 && pic2[0] == "PIC2_00.PNG" && pic2[1] == "PIC2.PNG");
    const auto snd = xm::candidate_names(xm::media_kind::music, 19, true);
    CHECK(snd.size() == 2 && snd[0] == "SND0_19.AT3" && snd[1] == "SND0.AT3");
    CHECK(xm::candidate_names(xm::media_kind::music, -1, true).size() == 1);

    CHECK(xm::media_language(5, 1) == 5);
    CHECK(xm::media_language(-1, 1) == 1);  // automatic: English (US)
    CHECK(xm::media_language(-2, 10) == 10); // the PS4 language, resolved by the caller
    CHECK(xm::media_language(25, -3) == 1);
}

void test_gain()
{
    CHECK(xm::level_gain(xm::music_level::on) == 32768 && xm::level_gain(xm::music_level::low) == 13107 && xm::level_gain(xm::music_level::off) == 0);
    for (std::uint32_t frames : {1u, 2u, 7u, 1440u, 48000u})
    {
        xm::gain_ramp up(0);
        up.set(xm::gain_unity, frames);
        std::int32_t last = 0;
        bool monotonic = true;
        for (std::uint32_t i = 0; i < frames; ++i)
        {
            const std::int32_t g = up.next();
            if (g < last) monotonic = false;
            last = g;
        }
        CHECK(monotonic && last == xm::gain_unity && up.done());
        xm::gain_ramp down(xm::gain_unity);
        down.set(0, frames);
        last = xm::gain_unity;
        for (std::uint32_t i = 0; i < frames; ++i)
        {
            const std::int32_t g = down.next();
            if (g > last) monotonic = false;
            last = g;
        }
        CHECK(monotonic && last == 0 && down.done() && down.next() == 0);
    }
    xm::gain_ramp level(xm::gain_unity);
    level.set(xm::gain_low, xm::frames_for(200'000));
    CHECK(level.remaining() == 9600);
    for (int i = 0; i < 9600; ++i) level.next();
    CHECK(level.value() == xm::gain_low);
    xm::gain_ramp now(5);
    now.set(77, 0);
    CHECK(now.value() == 77 && now.done());

    CHECK(xm::scale_sample(32767, 2 * xm::gain_unity) == 32767);
    CHECK(xm::scale_sample(-32768, 2 * xm::gain_unity) == -32768);
    CHECK(xm::scale_sample(-32768, xm::gain_unity) == -32768);
    CHECK(xm::scale_sample(32767, xm::gain_unity) == 32767);
    CHECK(xm::scale_sample(10000, xm::gain_low) == 3999);
    CHECK(xm::scale_sample(1234, 0) == 0);
}

void test_ring()
{
    xm::pcm_ring ring(10);
    std::int16_t in[2 * 8], out[2 * 10];
    for (int i = 0; i < 16; ++i) in[i] = static_cast<std::int16_t>(i + 1);
    CHECK(ring.push(in, 8) == 8 && ring.size() == 8 && ring.space() == 2);
    CHECK(ring.pop(out, 6) == 6 && out[0] == 1 && out[11] == 12);
    CHECK(ring.push(in, 8) == 8); // wraps around
    CHECK(ring.size() == 10 && ring.push(in, 1) == 0);
    CHECK(ring.pop(out, 10) == 10);
    CHECK(out[0] == 13 && out[3] == 16 && out[4] == 1 && out[19] == 16);
    CHECK(ring.pop(out, 1) == 0);
    std::int16_t loud[2 * 4];
    for (auto& s : loud) s = 20000;
    ring.push(loud, 4);
    ring.fade_tail(4);
    CHECK(ring.pop(out, 4) == 4);
    CHECK(out[0] == 15000 && out[2] == 10000 && out[4] == 5000 && out[6] == 0 && out[7] == 0);
}

// A track of constant samples; 'limit' frames, then nothing more ('ended') or underruns
struct fake_track
{
    std::int16_t value = 10000;
    std::uint64_t limit = ~0ull;
    bool end_after = false;
    std::uint64_t given = 0;
    int* freed = nullptr;
    ~fake_track()
    {
        if (freed) ++*freed;
    }
    std::uint32_t pull(std::int16_t* out, std::uint32_t frames)
    {
        const std::uint32_t n = static_cast<std::uint32_t>(std::min<std::uint64_t>(frames, limit - given));
        for (std::uint32_t i = 0; i < n * 2; ++i) out[i] = value;
        given += n;
        return n;
    }
    bool ended() const { return end_after && given >= limit; }
};

void test_mixer()
{
    using mixer = xm::basic_mixer<fake_track>;
    int freed = 0;
    auto track = [&](std::int16_t v) {
        auto t = std::make_unique<fake_track>();
        t->value = v;
        t->freed = &freed;
        return t;
    };
    std::int16_t block[512];
    // Silence while nothing plays
    {
        mixer m;
        for (auto& s : block) s = 99;
        m.render(block, 256);
        bool zero = true;
        for (auto s : block) zero = zero && s == 0;
        CHECK(zero && m.silent());
    }
    // Fade in, cross to a second track after the first has faded out, stop
    {
        mixer m;
        CHECK(!m.play(track(10000), 512, 256));
        m.render(block, 256);
        CHECK(block[0] > 0 && block[0] < block[510] && block[510] < 10000);
        bool monotonic = true;
        for (int i = 2; i < 512; i += 2) monotonic = monotonic && block[i] >= block[i - 2];
        CHECK(monotonic);
        m.render(block, 256);
        CHECK(block[510] == 10000 && block[511] == 10000);
        // B waits until A has faded out (256 frames)
        CHECK(!m.play(track(-8000), 256, 256));
        m.render(block, 256);
        CHECK(block[0] > 0 && block[510] == 0);
        auto retired = m.take_retired();
        CHECK(retired[0] && retired[0]->value == 10000);
        retired = {};
        CHECK(freed == 1);
        m.render(block, 256);
        CHECK(block[0] < 0 && block[0] > -8000 && block[510] == -8000);
        // A third replaces nothing pending; stop drops a pending one and fades the current
        CHECK(!m.play(track(5), 256, 256));
        auto dropped = m.stop(128);
        CHECK(dropped && dropped->value == 5);
        dropped.reset();
        CHECK(freed == 2);
        m.render(block, 128);
        CHECK(block[254] == 0 && m.silent());
        m.take_retired();
        CHECK(freed == 3);
    }
    // Level low (200 ms ramp) and the boot ramp shortening a long fade-out
    {
        mixer m;
        static_cast<void>(m.play(track(10000), 0, 0));
        m.set_level(xm::gain_low, 0);
        m.render(block, 256);
        CHECK(block[0] == 3999 && block[511] == 3999);
        static_cast<void>(m.stop(24000));
        m.render(block, 256);
        CHECK(block[510] > 3900);
        static_cast<void>(m.stop(xm::frames_for(30'000)));
        for (int i = 0; i < 6; ++i) m.render(block, 256);
        CHECK(m.silent() && block[510] == 0);
        // A longer fade asked later does not lengthen the running one
        static_cast<void>(m.play(track(10000), 0, 0));
        m.set_level(xm::gain_unity, 0);
        m.render(block, 256);
        static_cast<void>(m.stop(256));
        static_cast<void>(m.stop(48000));
        m.render(block, 256);
        CHECK(m.silent());
    }
    // Underruns are counted, a finished track is freed by itself
    {
        mixer m;
        auto t = track(1000);
        t->limit = 300;
        static_cast<void>(m.play(std::move(t), 0, 0));
        m.render(block, 256);
        m.render(block, 256);
        // 300 frames: 256, then 44 (indices 0..87) and silence
        CHECK(m.underrun_blocks() == 1 && block[86] == 1000 && block[87] == 1000 && block[88] == 0 && block[511] == 0);
        auto e = track(1000);
        e->limit = 100;
        e->end_after = true;
        mixer n;
        static_cast<void>(n.play(std::move(e), 0, 0));
        n.render(block, 256);
        CHECK(n.underrun_blocks() == 0 && n.silent());
    }
    // Every retired slot taken: the faded track stays silent until a slot frees, nothing is lost
    {
        mixer m;
        for (int i = 0; i < 5; ++i)
        {
            static_cast<void>(m.play(track(100), 0, 0));
            m.render(block, 16);
            static_cast<void>(m.stop(0));
            m.render(block, 16);
        }
        CHECK(!m.silent());
        auto r = m.take_retired();
        int n = 0;
        for (auto& p : r) n += p ? 1 : 0;
        CHECK(n == 4);
        m.render(block, 16);
        CHECK(m.silent());
    }
    // take_all (port closed): playing, waiting and finished tracks all come back, the mixer is silent and starts over
    {
        freed = 0;
        mixer m;
        static_cast<void>(m.play(track(100), 0, 0));
        m.render(block, 16);
        static_cast<void>(m.stop(0));
        m.render(block, 16); // the first one is finished (retired)
        static_cast<void>(m.play(track(200), 0, 0));
        m.render(block, 16); // the second one plays
        static_cast<void>(m.play(track(300), 0, 1000)); // the third waits for the second's fade-out
        auto all = m.take_all();
        int n = 0;
        for (auto& p : all) n += p ? 1 : 0;
        CHECK(n == 3 && m.silent());
        CHECK(all[0] && all[0]->value == 200 && all[1] && all[1]->value == 300);
        for (auto& p : all) p.reset();
        CHECK(freed == 3);
        for (auto& s : block) s = 1;
        m.render(block, 256);
        bool zero = true;
        for (auto s : block) zero = zero && s == 0;
        CHECK(zero);
        static_cast<void>(m.play(track(1000), 0, 0));
        m.render(block, 256);
        CHECK(block[0] == 1000 && block[511] == 1000);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Director

struct fake_payload
{
    std::uint32_t generation = 0;
    bool background = true, overlay = false, music = false, music_file = false, only = false;
    bool has_background() const { return background; }
    bool has_overlay() const { return overlay; }
    bool has_music() const { return music; }
    bool has_music_file() const { return music_file; }
    bool music_only() const { return only; }
};
using director = xm::basic_director<fake_payload>;

std::shared_ptr<fake_payload> answer(const xm::media_request& r, bool bg = true, bool ov = false, bool music = false)
{
    auto p = std::make_shared<fake_payload>();
    p->generation = r.generation;
    p->background = bg && r.want_images;
    p->overlay = ov && r.want_images;
    p->music = music && r.want_music;
    p->music_file = music;
    p->only = !r.want_images;
    return p;
}

constexpr std::uint64_t ms = 1000;

void test_director_settle()
{
    director d;
    d.configure({}, false);
    int posts = 0;
    std::uint64_t now = 1'000'000;
    std::vector<std::string> rows;
    for (int i = 0; i < 30; ++i) rows.push_back("/data/rpcs3/games/game" + std::to_string(i) + ".iso");
    // 30 rows at the 60 ms key repeat: nothing is read
    for (int i = 0; i < 30; ++i)
        for (int f = 0; f < 4; ++f, now += 15 * ms)
            posts += d.tick(now, {rows[static_cast<std::size_t>(i)], "BLUS00000"}).post ? 1 : 0;
    CHECK(posts == 0);
    // Resting on the last row: one job after 500 ms, not before
    const std::uint64_t rest = now - 4 * 15 * ms + 15 * ms * 0; // the last change happened 60 ms ago
    std::optional<xm::media_request> job;
    for (; now < rest + 2000 * ms && !job; now += ms) job = d.tick(now, {rows[29], "BLUS00000"}).post;
    CHECK(job.has_value());
    CHECK(job && job->path == rows[29] && job->serial == "BLUS00000" && job->want_images && job->want_music && !job->cache_only && job->wide);
    CHECK(job && now - ms - (rest - 60 * ms + 15 * ms) >= 500 * ms - 15 * ms);
    CHECK(job && job->state && job->state->wanted.load() == job->generation);
    int more = 0;
    for (int i = 0; i < 200; ++i, now += 16 * ms) more += d.tick(now, {rows[29], "BLUS00000"}).post ? 1 : 0;
    CHECK(more == 0);
}

void test_director_flow()
{
    xm::timing t;
    director d(t);
    xm::settings s;
    d.configure(s, false);
    std::uint64_t now = 0;
    const std::string a = "/mnt/usb0/PS3ISO/A.iso", b = "/data/rpcs3/dev_hdd0/game/NPUB00001";
    auto run_until_post = [&](const std::string& row) {
        std::optional<xm::media_request> job;
        for (int i = 0; i < 100 && !job; ++i, now += 10 * ms) job = d.tick(now, {row, "S"}).post;
        return job;
    };
    auto ja = run_until_post(a);
    CHECK(ja.has_value());
    // A stale result: the selection moved to B before A's job finished
    auto out = d.tick(now, {b, "S"});
    CHECK(out.drop && !out.post);
    CHECK(ja->state->wanted.load() != ja->generation); // A's running job sees it and abandons
    CHECK(!d.arrived(answer(*ja)));
    auto jb = run_until_post(b);
    CHECK(jb.has_value());
    CHECK(d.arrived(answer(*jb, true, true, true)));
    out = d.tick(now, {b, "S"});
    CHECK(out.present && out.background_alpha == 0);
    CHECK(out.play == out.present); // the music starts with the background
    CHECK(out.animating);
    // Background fades in over 800 ms, the overlay starts 1 s after that and takes 500 ms
    const std::uint64_t t0 = now;
    out = d.tick(t0 + 400 * ms, {b, "S"});
    CHECK(out.background_alpha == 127 && out.overlay_alpha == 0 && !out.play);
    out = d.tick(t0 + 800 * ms, {b, "S"});
    CHECK(out.background_alpha == 255 && out.overlay_alpha == 0 && out.animating);
    out = d.tick(t0 + 1799 * ms, {b, "S"});
    CHECK(out.overlay_alpha == 0);
    out = d.tick(t0 + 2050 * ms, {b, "S"});
    CHECK(out.overlay_alpha == 127);
    out = d.tick(t0 + 2300 * ms, {b, "S"});
    CHECK(out.background_alpha == 255 && out.overlay_alpha == 255 && !out.animating);
    now = t0 + 2300 * ms;
    // A list of this game holds the media (the cursor row is not the game any more)
    out = d.tick(now, {"@patches", "", true});
    CHECK(!out.drop && !out.stop_music && out.background_alpha == 255);
    for (int i = 0; i < 100; ++i, now += 10 * ms) CHECK(!d.tick(now, {"@patches", "", true}).post);
    // Context loss (another column): everything fades out, the music stops
    out = d.tick(now, {b, "S", false, false});
    CHECK(out.stop_music && out.stop_us == t.music_out_us && out.drop);
    CHECK(out.background_alpha == 255 && out.overlay_alpha == 255);
    out = d.tick(now + 125 * ms, {b, "S", false, false});
    CHECK(out.overlay_alpha == 128 && out.background_alpha == 176 && out.animating);
    out = d.tick(now + 400 * ms, {b, "S", false, false});
    CHECK(out.clear && out.background_alpha == 0 && !d.showing());
    now += 400 * ms;
    out = d.tick(now + 10 * ms, {b, "S", false, false});
    CHECK(!out.clear && !out.post);
}

void test_director_one_at_a_time()
{
    xm::timing t;
    t.settle_us = 100 * ms;
    t.background_out_us = 400 * ms;
    director d(t);
    d.configure({}, false);
    std::uint64_t now = 0;
    auto settle = [&](const std::string& row) {
        std::optional<xm::media_request> job;
        for (int i = 0; i < 100 && !job; ++i, now += 10 * ms) job = d.tick(now, {row, "S"}).post;
        return job;
    };
    auto ja = settle("A");
    CHECK(d.arrived(answer(*ja)));
    CHECK(d.tick(now, {"A", "S"}).present);
    now += 1000 * ms;
    CHECK(d.tick(now, {"A", "S"}).background_alpha == 255);
    // B arrives while A still fades out: it waits for the clear
    auto jb = settle("B");
    CHECK(jb.has_value());
    CHECK(d.arrived(answer(*jb)));
    bool presented = false, cleared = false, overlap = false;
    for (int i = 0; i < 100 && !presented; ++i, now += 10 * ms)
    {
        const auto out = d.tick(now, {"B", "S"});
        if (out.clear) cleared = true;
        if (out.present)
        {
            presented = true;
            overlap = !cleared;
        }
    }
    CHECK(presented && cleared && !overlap);
}

void test_director_gates()
{
    xm::timing t;
    // Inline loader: cached backgrounds only, no music
    {
        director d(t);
        d.configure({}, true);
        std::optional<xm::media_request> job;
        std::uint64_t now = 0;
        for (int i = 0; i < 100 && !job; ++i, now += 10 * ms) job = d.tick(now, {"A", "S"}).post;
        CHECK(job && job->cache_only && !job->want_music);
    }
    // Music off: images only; music on again later: a music job for the shown game
    {
        director d(t);
        xm::settings s;
        s.music = xm::music_level::off;
        d.configure(s, false);
        std::optional<xm::media_request> job;
        std::uint64_t now = 0;
        for (int i = 0; i < 100 && !job; ++i, now += 10 * ms) job = d.tick(now, {"A", "S"}).post;
        CHECK(job && !job->want_music);
        CHECK(d.arrived(answer(*job, true, false, true)));
        auto out = d.tick(now, {"A", "S"});
        CHECK(out.present && !out.play);
        s.music = xm::music_level::low;
        d.configure(s, false);
        out = d.tick(now += 10 * ms, {"A", "S"});
        CHECK(out.post && !out.post->want_images && out.post->want_music && out.post->path == "A");
        CHECK(d.arrived(answer(*out.post, true, false, true)));
        out = d.tick(now += 10 * ms, {"A", "S"});
        CHECK(out.play && !out.present);
        // Music off: it stops, the background stays
        s.music = xm::music_level::off;
        d.configure(s, false);
        out = d.tick(now += 10 * ms, {"A", "S"});
        CHECK(out.stop_music && !out.clear && out.background_alpha > 0);
    }
    // The previous game still stopping: background first, the music once the gate opens
    {
        director d(t);
        d.configure({}, false);
        std::optional<xm::media_request> job;
        std::uint64_t now = 0;
        for (int i = 0; i < 100 && !job; ++i, now += 10 * ms) job = d.tick(now, {"A", "S", false, true, false, false}).post;
        CHECK(job && job->want_music);
        CHECK(d.arrived(answer(*job, true, false, true)));
        auto out = d.tick(now, {"A", "S", false, true, false, false});
        CHECK(out.present && !out.play);
        out = d.tick(now += 500 * ms, {"A", "S", false, true, false, false});
        CHECK(!out.play);
        out = d.tick(now += 10 * ms, {"A", "S"});
        CHECK(out.play);
        // A modal dialog: the music fades out, the background stays; after it, a new track from the start
        out = d.tick(now += 10 * ms, {"A", "S", true, true, false, false});
        CHECK(out.stop_music && out.stop_us == t.music_out_us && !out.clear && !out.drop);
        out = d.tick(now += 10 * ms, {"A", "S", true, true, false, false});
        CHECK(!out.post && !out.play);
        out = d.tick(now += 10 * ms, {"A", "S"});
        CHECK(out.post && !out.post->want_images && out.post->want_music);
        const auto music = *out.post;
        out = d.tick(now += 10 * ms, {"A", "S"});
        CHECK(!out.post); // one music job at a time
        CHECK(d.arrived(answer(music, true, false, true)));
        out = d.tick(now += 10 * ms, {"A", "S"});
        CHECK(out.play && out.play->music_only());
        // The system UI in front: a short fade, the images stay; back: a new track
        out = d.tick(now += 10 * ms, {"A", "S", false, true, true, true});
        CHECK(out.stop_music && out.stop_us == t.music_system_ui_us && !out.clear && !out.drop && out.background_alpha > 0);
        out = d.tick(now += 2000 * ms, {"A", "S", false, true, true, true});
        CHECK(!out.post && !out.clear);
        out = d.tick(now += 10 * ms, {"A", "S"});
        CHECK(out.post && !out.post->want_images);
        // A music job that comes back without a track (SND0 refused): nothing plays, nothing loops
        CHECK(!d.arrived(answer(*out.post, true, false, false)));
        for (int i = 0; i < 50; ++i) CHECK(!d.tick(now += 10 * ms, {"A", "S"}).post);
    }
    // Settings off: the fade-out starts at once
    {
        director d(t);
        xm::settings s;
        d.configure(s, false);
        std::optional<xm::media_request> job;
        std::uint64_t now = 0;
        for (int i = 0; i < 100 && !job; ++i, now += 10 * ms) job = d.tick(now, {"A", "S"}).post;
        CHECK(d.arrived(answer(*job, true, false, true)));
        CHECK(d.tick(now, {"A", "S"}).present);
        now += 1000 * ms;
        s.media = false;
        d.configure(s, false);
        auto out = d.tick(now, {"A", "S"});
        CHECK(out.stop_music && out.drop && out.background_alpha == 255);
        out = d.tick(now + 400 * ms, {"A", "S"});
        CHECK(out.clear);
        for (int i = 0; i < 100; ++i) CHECK(!d.tick(now += 10 * ms, {"A", "S"}).post);
    }
    // A result with nothing in it is dropped and not asked again
    {
        director d(t);
        d.configure({}, false);
        std::optional<xm::media_request> job;
        std::uint64_t now = 0;
        for (int i = 0; i < 100 && !job; ++i, now += 10 * ms) job = d.tick(now, {"A", "S"}).post;
        CHECK(!d.arrived(answer(*job, false, false, false)));
        for (int i = 0; i < 100; ++i) CHECK(!d.tick(now += 10 * ms, {"A", "S"}).post);
        CHECK(!d.arrived(nullptr));
    }
}

void test_release_order()
{
    director d;
    d.configure({}, false);
    std::optional<xm::media_request> job;
    std::uint64_t now = 0;
    for (int i = 0; i < 100 && !job; ++i, now += 10 * ms) job = d.tick(now, {"A", "S"}).post;
    CHECK(d.arrived(answer(*job, true, false, true)));
    const auto out = d.tick(now, {"A", "S"});
    CHECK(out.present && out.play);
    std::vector<std::string> steps;
    int drained = 0;
    const xm::release_totals totals = xm::release_in_order(
        [&] { steps.push_back("music"); return std::uint64_t(3000); },
        [&] { steps.push_back("jobs"); },
        [&](xm::release_totals& t) {
            steps.push_back("results");
            d.drain([&](std::shared_ptr<fake_payload>) { ++drained; ++t.results; });
        },
        [&] { steps.push_back("images"); return std::uint64_t(1280 * 720 * 4); });
    CHECK((steps == std::vector<std::string>{"music", "jobs", "results", "images"}));
    CHECK(drained == 1 && totals.results == 1 && totals.music_bytes == 3000 && totals.image_bytes == 1280u * 720 * 4);
    CHECK(!d.showing());
    CHECK(job->state->wanted.load() != job->generation);
    // Released: inert (no job, no alpha, no music) until configured again
    bool quiet = true;
    for (int i = 0; i < 100; ++i)
    {
        const auto o = d.tick(now += 10 * ms, {"A", "S"});
        quiet = quiet && !o.post && !o.present && !o.play && !o.background_alpha && !o.overlay_alpha;
    }
    CHECK(quiet);
    d.configure({}, false);
    std::optional<xm::media_request> again;
    for (int i = 0; i < 100 && !again; ++i) again = d.tick(now += 10 * ms, {"A", "S"}).post;
    CHECK(again.has_value());
}

// ---------------------------------------------------------------------------------------------------------------------

xm::media_record sample_record()
{
    xm::media_record r;
    r.key.path = "/mnt/usb0/PS3ISO/Game.iso";
    r.key.language = 1;
    r.key.wide = true;
    r.key.stamps = {{true, 7'000'000'000ull, 1'700'000'000}};
    r.background = true;
    r.overlay = true;
    r.music = true;
    r.music_name = 1;
    r.codec = xm::riff_codec::atrac3plus;
    r.music_rate = 44100;
    r.music_channels = 2;
    r.background_w = 1920;
    r.background_h = 1080;
    r.overlay_w = 1000;
    r.overlay_h = 560;
    r.jpeg = bytes(3000, 0xAB);
    r.overlay_box_px = xm::overlay_box(1000, 560);
    r.overlay_zlib = bytes(500, 0xCD);
    return r;
}

void test_record()
{
    const xm::media_record r = sample_record();
    const bytes enc = xm::encode_record(r);
    const auto back = xm::decode_record(enc, r.key);
    CHECK(back.has_value());
    if (back)
    {
        CHECK(back->background && back->overlay && back->music && !back->music_unsupported && back->music_name == 1);
        CHECK(back->codec == xm::riff_codec::atrac3plus && back->music_rate == 44100 && back->music_channels == 2);
        CHECK(back->background_w == 1920 && back->overlay_h == 560);
        CHECK(back->jpeg == r.jpeg && back->overlay_zlib == r.overlay_zlib && back->overlay_box_px == r.overlay_box_px);
    }
    // "No media" is a valid record
    xm::media_record none;
    none.key = r.key;
    CHECK(xm::decode_record(xm::encode_record(none), r.key).has_value());
    // Folder sources: six stamps, one changed mtime is a miss
    xm::media_record files = none;
    files.key.path = "/data/rpcs3/dev_hdd0/game/NPUB00001";
    files.key.stamps = {{true, 100, 1}, {false, 0, 0}, {true, 5, 2}, {false, 0, 0}, {false, 0, 0}, {true, 9, 3}};
    const bytes fenc = xm::encode_record(files);
    CHECK(xm::decode_record(fenc, files.key).has_value());
    auto changed = files.key;
    changed.stamps[5].mtime = 4;
    CHECK(!xm::decode_record(fenc, changed));
    changed = files.key;
    changed.stamps[1].present = true;
    CHECK(!xm::decode_record(fenc, changed));
    // Misses: path, language, aspect, size, mtime
    auto key = r.key;
    key.path += "x";
    CHECK(!xm::decode_record(enc, key));
    key = r.key;
    key.language = 2;
    CHECK(!xm::decode_record(enc, key));
    key = r.key;
    key.wide = false;
    CHECK(!xm::decode_record(enc, key));
    key = r.key;
    key.stamps[0].size++;
    CHECK(!xm::decode_record(enc, key));
    key = r.key;
    key.stamps[0].mtime--;
    CHECK(!xm::decode_record(enc, key));
    // Wrong magic or version, trailing bytes: refused by the fields themselves (checksum made valid again)
    const auto reseal = [](bytes b) {
        b.resize(b.size() - 8);
        const std::uint64_t sum = xm::fnv1a64({reinterpret_cast<const char*>(b.data()), b.size()});
        for (int i = 0; i < 8; ++i) b.push_back(static_cast<std::uint8_t>(sum >> (8 * i)));
        return b;
    };
    CHECK(xm::decode_record(reseal(enc), r.key).has_value());
    bytes bad = enc;
    bad[0] ^= 1;
    CHECK(!xm::decode_record(bad, r.key));
    CHECK(!xm::decode_record(reseal(bad), r.key));
    bad = enc;
    bad[4] = 2;
    CHECK(!xm::decode_record(reseal(bad), r.key));
    bad = enc;
    bad.insert(bad.end() - 8, 0);
    CHECK(!xm::decode_record(reseal(bad), r.key));
    bad = enc;
    bad.push_back(0);
    CHECK(!xm::decode_record(bad, r.key));
    // An overlay outside the screen, or a flag without its data
    xm::media_record off = r;
    off.overlay_box_px.x = 700;
    CHECK(!xm::decode_record(xm::encode_record(off), r.key));
    off = r;
    off.jpeg.clear();
    CHECK(!xm::decode_record(xm::encode_record(off), r.key));
    off = r;
    off.music = false;
    off.music_unsupported = true;
    CHECK(!xm::decode_record(xm::encode_record(off), r.key));
    // Truncated at every byte: a miss, never a read past the end
    int hits = 0;
    for (std::size_t n = 0; n < enc.size(); ++n)
    {
        std::unique_ptr<std::uint8_t[]> copy(new std::uint8_t[n ? n : 1]);
        if (n) std::memcpy(copy.get(), enc.data(), n);
        hits += xm::decode_record({copy.get(), n}, r.key) ? 1 : 0;
    }
    CHECK(hits == 0);
    // Random byte changes: never a crash; a hit only with the original content (the checksum)
    std::mt19937 rng(77);
    int changed_hits = 0;
    for (int i = 0; i < 3000; ++i)
    {
        bytes m = enc;
        for (int k = 0; k < 4; ++k) m[rng() % m.size()] = static_cast<std::uint8_t>(rng());
        std::unique_ptr<std::uint8_t[]> copy(new std::uint8_t[m.size()]);
        std::memcpy(copy.get(), m.data(), m.size());
        const auto got = xm::decode_record({copy.get(), m.size()}, r.key);
        if (got && m != enc) ++changed_hits;
        if (got && got->overlay) CHECK(got->overlay_box_px.x + got->overlay_box_px.w <= xm::screen_width);
    }
    CHECK(changed_hits == 0);
    // One flipped bit anywhere (the JPEG included): a miss
    int flipped_hits = 0;
    for (std::size_t at = 0; at < enc.size(); at += 7)
    {
        bytes m = enc;
        m[at] ^= 0x10;
        flipped_hits += xm::decode_record(m, r.key) ? 1 : 0;
    }
    CHECK(flipped_hits == 0);
    // Without its checksum (a record cut before it): a miss
    CHECK(!xm::decode_record(bytes(enc.begin(), enc.end() - 8), r.key));
    // File name
    CHECK(xm::is_cache_name(xm::record_name(r.key)));
    auto other = r.key;
    other.wide = false;
    CHECK(xm::record_name(other) != xm::record_name(r.key));
    other = r.key;
    other.stamps[0].mtime++; // the stamps are checked inside, they do not name the file
    CHECK(xm::record_name(other) == xm::record_name(r.key));
    CHECK(xm::fnv1a64("") == 0xcbf29ce484222325ull);
}

void test_trim()
{
    CHECK(xm::is_cache_name("0123456789abcdef.bin"));
    CHECK(!xm::is_cache_name("0123456789ABCDEF.bin"));
    CHECK(!xm::is_cache_name("0123456789abcdef.bin.tmp"));
    CHECK(!xm::is_cache_name("0123456789abcde.bin"));
    CHECK(!xm::is_cache_name("PIC1.PNG"));
    CHECK(!xm::is_cache_name("0123456789abcdeg.bin"));
    std::vector<xm::cache_file> files;
    for (int i = 0; i < 100; ++i)
    {
        char name[32];
        std::snprintf(name, sizeof(name), "%016x.bin", i * 7919);
        files.push_back({name, 1u << 20, 1000 - i}); // the later ones are older
    }
    files.push_back({"notes.txt", 500u << 20, 0});
    files.push_back({"0000000000000000.BIN", 500u << 20, 0});
    // 100 MiB of records: the 28 oldest go (down to 72 MiB)
    const auto removed = xm::trim_selection(files);
    CHECK(removed.size() == 28);
    bool oldest = true;
    for (const auto& name : removed)
    {
        const int i = static_cast<int>(std::stoul(name.substr(0, 16), nullptr, 16) / 7919);
        oldest = oldest && i >= 72;
    }
    CHECK(oldest);
    for (const auto& name : removed) CHECK(xm::is_cache_name(name));
    // Within the budget: nothing
    files.resize(90);
    CHECK(xm::trim_selection(files).empty());
    CHECK(xm::trim_selection({}).empty());
}

void test_settings(const stdfs::path& dir)
{
    const std::string flag = (dir / "xmb-media-off.flag").string(), level = (dir / "xmb-music.txt").string();
    std::error_code ec;
    stdfs::remove(flag, ec);
    stdfs::remove(level, ec);
    auto s = xm::read_settings(flag.c_str(), level.c_str());
    CHECK(s.media && s.music == xm::music_level::on && s.wide);
    CHECK(xm::write_media_switch(false, flag.c_str()));
    CHECK(stdfs::exists(flag));
    CHECK(!xm::read_settings(flag.c_str(), level.c_str()).media);
    CHECK(xm::write_media_switch(false, flag.c_str()));
    CHECK(xm::write_media_switch(true, flag.c_str()));
    CHECK(!stdfs::exists(flag) && xm::read_settings(flag.c_str(), level.c_str()).media);
    CHECK(xm::write_media_switch(true, flag.c_str()));
    CHECK(xm::write_music_level(xm::music_level::low, level.c_str()));
    CHECK(xm::read_settings(flag.c_str(), level.c_str()).music == xm::music_level::low);
    CHECK(xm::write_music_level(xm::music_level::off, level.c_str()));
    CHECK(xm::read_settings(flag.c_str(), level.c_str()).music == xm::music_level::off);
    CHECK(xm::write_music_level(xm::music_level::on, level.c_str()));
    CHECK(!stdfs::exists(level) && xm::read_settings(flag.c_str(), level.c_str()).music == xm::music_level::on);
    CHECK(!stdfs::exists(level + ".tmp"));
    // A folder that refuses: false, the old value stays
    const std::string missing = (dir / "no-such-folder" / "xmb-music.txt").string();
    CHECK(!xm::write_music_level(xm::music_level::low, missing.c_str()));
    CHECK(!xm::write_media_switch(false, (dir / "no-such-folder" / "x.flag").string().c_str()));

    CHECK(xm::parse_music_level(" low\r\n") == xm::music_level::low);
    CHECK(xm::parse_music_level("off") == xm::music_level::off);
    CHECK(xm::parse_music_level("OFF") == xm::music_level::on);
    CHECK(xm::parse_music_level("") == xm::music_level::on);
    CHECK(xm::step_music_level(xm::music_level::on, 1) == xm::music_level::low);
    CHECK(xm::step_music_level(xm::music_level::low, 1) == xm::music_level::off);
    CHECK(xm::step_music_level(xm::music_level::off, 1) == xm::music_level::on);
    CHECK(xm::step_music_level(xm::music_level::on, -1) == xm::music_level::off);
    CHECK(xm::step_music_level(xm::music_level::off, -1) == xm::music_level::low);
    CHECK(std::string(xm::music_level_token(xm::music_level::low)) == "low");

    CHECK(!xm::yaml_aspect_4_3(""));
    CHECK(!xm::yaml_aspect_4_3("Video:\n  Renderer: Vulkan\n  Aspect ratio: 16:9\n"));
    CHECK(xm::yaml_aspect_4_3("Core:\n  PPU Decoder: LLVM\r\nVideo:\r\n  Renderer: Vulkan\r\n  Aspect ratio: 4:3\r\nAudio:\n"));
    CHECK(!xm::yaml_aspect_4_3("Core:\n  Aspect ratio: 4:3\nVideo:\n  Renderer: Vulkan\n"));
    CHECK(!xm::yaml_aspect_4_3("Video:\n  Renderer: Vulkan\nAudio:\n  Aspect ratio: 4:3\n"));
    CHECK(!xm::yaml_aspect_is_4_3("Video:\n  Renderer: Vulkan\nAudio:\n  Aspect ratio: 4:3\n").has_value());
    CHECK(xm::yaml_aspect_is_4_3("Video:\n  Aspect ratio: 16:9\n") == std::optional<bool>(false));
    CHECK(xm::yaml_aspect_is_4_3("Video:\n  Aspect ratio:   4:3  \n") == std::optional<bool>(true));

    CHECK(std::string(xm::media_off_flag) == "/data/rpcs3/xmb-media-off.flag");
    CHECK(std::string(xm::cache_dir).starts_with("/data/rpcs3/"));
    CHECK(std::string(xm::music_level_file).starts_with("/data/rpcs3/"));
}
} // namespace

int main()
{
    std::error_code ec;
    const stdfs::path dir = stdfs::temp_directory_path(ec) / ("xmb_media_test_" + std::to_string(std::random_device{}() & 0xffffff));
    stdfs::create_directories(dir, ec);
    test_riff();
    test_riff_mutations();
    test_geometry();
    test_resolve();
    test_gain();
    test_ring();
    test_mixer();
    test_director_settle();
    test_director_flow();
    test_director_one_at_a_time();
    test_director_gates();
    test_release_order();
    test_record();
    test_trim();
    test_settings(dir);
    stdfs::remove_all(dir, ec);
    std::printf("xmb_media_test: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
