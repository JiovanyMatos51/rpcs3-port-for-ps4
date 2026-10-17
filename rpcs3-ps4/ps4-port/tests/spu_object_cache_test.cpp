// Host test of rpcs3/util/ps4_spu_object_cache.hpp (persistent SPU object cache, pure part).
// Build: clang++ -std=c++20 -static -Wall -Wextra -I rpcs3 ps4-port/tests/spu_object_cache_test.cpp
#include "util/ps4_spu_object_cache.hpp"

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <random>
#include <set>
#include <thread>

namespace oc = utils::ps4::spu_objcache;

static int g_failures = 0;

#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; } } while (0)

// In-memory file with the callbacks of the header
struct mem_file
{
    std::vector<std::uint8_t> bytes;

    oc::read_fn reader()
    {
        return [this](std::uint64_t off, void* dst, std::size_t n)
        {
            if (off > bytes.size() || bytes.size() - off < n) return false;
            std::memcpy(dst, bytes.data() + off, n);
            return true;
        };
    }

    oc::write_fn writer()
    {
        return [this](const void* src, std::size_t n)
        {
            const auto* p = static_cast<const std::uint8_t*>(src);
            bytes.insert(bytes.end(), p, p + n);
            return true;
        };
    }
};

static oc::digest make_digest(std::uint32_t seed)
{
    std::uint8_t s[4];
    for (int i = 0; i < 4; i++) s[i] = static_cast<std::uint8_t>(seed >> (8 * i));
    return oc::sha256_of(s, sizeof(s));
}

static std::vector<std::uint8_t> make_payload(std::mt19937& rng, std::size_t size)
{
    std::vector<std::uint8_t> p(size);
    for (auto& b : p) b = static_cast<std::uint8_t>(rng());
    return p;
}

static void test_sha256()
{
    CHECK(oc::hex(oc::sha256_of("", 0)) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(oc::hex(oc::sha256_of("abc", 3)) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    const char* two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    CHECK(oc::hex(oc::sha256_of(two, std::strlen(two))) == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");

    // Split updates give the same digest
    std::string million(1000000, 'a');
    oc::sha256 ctx;
    for (std::size_t i = 0; i < million.size(); i += 777) ctx.update(million.data() + i, std::min<std::size_t>(777, million.size() - i));
    CHECK(oc::hex(ctx.finish()) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

static std::map<std::string, std::string> sample_keys()
{
    return {
        {"format", "1"}, {"stamp", "00ff"}, {"target", "btver2|-avx2,-fma"}, {"tsc", "1600000000"}, {"fast_fma", "0"},
        {"cfg.spu_block_size", "Safe"}, {"cfg.spu_xfloat_accuracy", "Approximate"}, {"cfg.clocks_scale", "100"}};
}

static void test_keys()
{
    const auto keys = sample_keys();
    const std::string text = oc::canonical_text(keys);
    CHECK(text.find("cfg.clocks_scale=100\n") == 0);
    CHECK(oc::parse_canonical(text) == keys);
    const oc::digest fp = oc::fingerprint(text);

    // Any single change changes the fingerprint
    for (const auto& [k, v] : keys)
    {
        auto changed = keys;
        changed[k] = v + "x";
        CHECK(oc::fingerprint(oc::canonical_text(changed)) != fp);
        CHECK(oc::first_difference(text, oc::canonical_text(changed)) == k + ": " + v + " -> " + v + "x");
    }

    auto added = keys;
    added["cfg.new_setting"] = "true";
    CHECK(oc::fingerprint(oc::canonical_text(added)) != fp);
    CHECK(oc::first_difference(text, oc::canonical_text(added)) == "cfg.new_setting: (none) -> true");
    CHECK(oc::first_difference(text, text).empty());

    // Line breaks cannot forge another key
    std::map<std::string, std::string> forged{{"a", "1\nb=2"}};
    CHECK(oc::parse_canonical(oc::canonical_text(forged)).size() == 1);

    // Program key: every part counts
    std::uint8_t sha1[20]{};
    for (int i = 0; i < 20; i++) sha1[i] = static_cast<std::uint8_t>(i);
    const oc::digest ir = make_digest(1);
    const oc::digest base = oc::program_key(0x100, 0x100, 40, sha1, ir);
    CHECK(oc::program_key(0x100, 0x100, 40, sha1, ir) == base);
    CHECK(oc::program_key(0x104, 0x100, 40, sha1, ir) != base);
    CHECK(oc::program_key(0x100, 0x0fc, 40, sha1, ir) != base);
    CHECK(oc::program_key(0x100, 0x100, 41, sha1, ir) != base);
    CHECK(oc::program_key(0x100, 0x100, 40, sha1, make_digest(2)) != base);
    std::uint8_t sha1b[20];
    std::memcpy(sha1b, sha1, 20);
    sha1b[19] ^= 1;
    CHECK(oc::program_key(0x100, 0x100, 40, sha1b, ir) != base);

    CHECK(oc::cap_from_text("") == oc::default_cap_bytes);
    CHECK(oc::cap_from_text("abc") == oc::default_cap_bytes);
    CHECK(oc::cap_from_text(" 256\n") == (256ull << 20));
    CHECK(oc::cap_from_text("1") == oc::min_cap_bytes);
    CHECK(oc::cap_from_text("99999999999") == oc::max_cap_bytes);
}

struct built_pack
{
    mem_file file;
    std::vector<oc::digest> keys;
    std::vector<std::vector<std::uint8_t>> payloads;
    std::vector<std::uint64_t> offsets;
    std::uint64_t begin = 0;
};

static built_pack build_pack(const oc::digest& fp, std::uint32_t count, std::mt19937& rng)
{
    built_pack b;
    const std::string text = oc::canonical_text(sample_keys());
    b.file.bytes = oc::make_header(fp, text, 1234);
    b.begin = b.file.bytes.size();

    for (std::uint32_t i = 0; i < count; i++)
    {
        b.keys.push_back(make_digest(i + 1000));
        b.payloads.push_back(make_payload(rng, 16 + rng() % 600));
        b.offsets.push_back(b.file.bytes.size());
        const auto rec = oc::make_record(b.keys.back(), oc::flag_zlib, 5000, b.payloads.back().data(), b.payloads.back().size());
        b.file.bytes.insert(b.file.bytes.end(), rec.begin(), rec.end());
    }

    return b;
}

static void test_round_trip()
{
    std::mt19937 rng(7);
    const oc::digest fp = make_digest(77);
    built_pack b = build_pack(fp, 5000, rng);

    const auto info = oc::parse_header(b.file.reader(), b.file.bytes.size(), fp);
    CHECK(info.status == oc::header_status::ok);
    CHECK(info.records_begin == b.begin);
    CHECK(info.created == 1234);
    CHECK(info.key_text == oc::canonical_text(sample_keys()));

    // Small chunks exercise the refill path
    const auto res = oc::scan(b.file.reader(), info.records_begin, b.file.bytes.size(), 4096);
    CHECK(res.records == 5000);
    CHECK(res.index.size() == 5000);
    CHECK(res.damaged == 0 && res.superseded == 0 && !res.read_error);
    CHECK(res.good_end == b.file.bytes.size());
    CHECK(!res.tail_damaged());
    CHECK(!oc::needs_rewrite(res, oc::default_cap_bytes));

    std::vector<std::uint8_t> payload;
    bool all = true;

    for (std::size_t i = 0; i < b.keys.size(); i++)
    {
        const auto it = res.index.find(b.keys[i]);
        all = all && it != res.index.end() && it->second.offset == b.offsets[i] && it->second.object_size == 5000;
        all = all && oc::read_record(b.file.reader(), it->second, b.keys[i], payload) && payload == b.payloads[i];
    }

    CHECK(all);

    // The wrong key never reads a record
    CHECK(!oc::read_record(b.file.reader(), res.index.at(b.keys[0]), b.keys[1], payload));

    // Fingerprint mismatch and other format
    CHECK(oc::parse_header(b.file.reader(), b.file.bytes.size(), make_digest(78)).status == oc::header_status::other_key);
    CHECK(oc::parse_header(b.file.reader(), 100, fp).status == oc::header_status::empty);
}

static void test_truncation()
{
    std::mt19937 rng(11);
    const oc::digest fp = make_digest(5);
    built_pack b = build_pack(fp, 20, rng);
    const std::uint64_t last = b.offsets.back();
    const std::uint64_t end = b.file.bytes.size();
    bool all = true;

    // Every cut inside the last record keeps the 19 before it and marks the tail
    for (std::uint64_t cut = last; cut < end; cut++)
    {
        mem_file f;
        f.bytes.assign(b.file.bytes.begin(), b.file.bytes.begin() + static_cast<std::ptrdiff_t>(cut));
        const auto res = oc::scan(f.reader(), b.begin, cut, 512);
        all = all && res.records == 19 && res.index.size() == 19 && res.good_end == last && res.tail_damaged() == (cut != last);
    }

    CHECK(all);

    // A cut inside the header: the pack is unusable (deleted by the caller)
    for (std::uint64_t cut = 0; cut < b.begin; cut += 7)
    {
        mem_file f;
        f.bytes.assign(b.file.bytes.begin(), b.file.bytes.begin() + static_cast<std::ptrdiff_t>(cut));
        const auto st = oc::parse_header(f.reader(), cut, fp).status;
        all = all && st != oc::header_status::ok;
    }

    CHECK(all);
}

static void test_corruption()
{
    std::mt19937 rng(13);
    const oc::digest fp = make_digest(9);
    const built_pack b = build_pack(fp, 10, rng);

    // Any flipped bit of the header or key text: not ok
    bool all = true;

    for (std::uint64_t i = 0; i < b.begin; i++)
    {
        mem_file f = b.file;
        f.bytes[i] ^= 0x10;
        all = all && oc::parse_header(f.reader(), f.bytes.size(), fp).status != oc::header_status::ok;
    }

    CHECK(all);

    // Flipped bit in record 4's header: the index ends before it (later records are cut with the tail)
    for (std::uint32_t i = 0; i < oc::record_header_size; i++)
    {
        mem_file f = b.file;
        f.bytes[b.offsets[4] + i] ^= 0x01;
        const auto res = oc::scan(f.reader(), b.begin, f.bytes.size());
        all = all && res.records == 4 && res.good_end == b.offsets[4] && res.tail_damaged();
    }

    CHECK(all);

    // Flipped bit in record 4's payload: only that record is dropped, the pack must be rewritten
    {
        mem_file f = b.file;
        f.bytes[b.offsets[4] + oc::record_header_size + 3] ^= 0x80;
        const auto res = oc::scan(f.reader(), b.begin, f.bytes.size());
        CHECK(res.records == 9 && res.damaged == 1 && res.good_end == f.bytes.size());
        CHECK(!res.index.count(b.keys[4]));
        CHECK(oc::needs_rewrite(res, oc::default_cap_bytes));

        // Rewrite: header + copied records, 9 left, nothing damaged
        mem_file out;
        out.bytes = oc::make_header(fp, oc::canonical_text(sample_keys()), 1);
        const auto kept = oc::select_for_rewrite(res, oc::default_cap_bytes);
        CHECK(kept.size() == 9);
        CHECK(oc::copy_records(f.reader(), kept, res.index, out.writer()) == 9);
        const auto res2 = oc::scan(out.reader(), b.begin, out.bytes.size());
        CHECK(res2.records == 9 && res2.damaged == 0 && !oc::needs_rewrite(res2, oc::default_cap_bytes));
    }

    // Damage after the scan: the load-time read refuses the record
    {
        mem_file f = b.file;
        const auto res = oc::scan(f.reader(), b.begin, f.bytes.size());
        f.bytes[b.offsets[2] + oc::record_header_size] ^= 0x02;
        std::vector<std::uint8_t> payload;
        CHECK(!oc::read_record(f.reader(), res.index.at(b.keys[2]), b.keys[2], payload));
        CHECK(oc::read_record(f.reader(), res.index.at(b.keys[3]), b.keys[3], payload) && payload == b.payloads[3]);

        // A record header that claims another size or flags than indexed
        mem_file g = b.file;
        auto e = res.index.at(b.keys[5]);
        e.stored_size += 1;
        CHECK(!oc::read_record(g.reader(), e, b.keys[5], payload));
    }

    // Records with impossible sizes are refused
    {
        const std::uint8_t data[4] = {1, 2, 3, 4};
        auto rec = oc::make_record(make_digest(1), 0, 5, data, 4); // raw record with object size != stored size
        mem_file f;
        f.bytes = rec;
        CHECK(oc::scan(f.reader(), 0, f.bytes.size()).records == 0);
        rec = oc::make_record(make_digest(1), 0, 4, data, 4);
        f.bytes = rec;
        CHECK(oc::scan(f.reader(), 0, f.bytes.size()).records == 1);
        rec = oc::make_record(make_digest(1), 2, 4, data, 4); // unknown flag
        f.bytes = rec;
        CHECK(oc::scan(f.reader(), 0, f.bytes.size()).records == 0);
    }
}

static void test_supersede_and_cap()
{
    std::mt19937 rng(17);
    const oc::digest fp = make_digest(3);
    built_pack b = build_pack(fp, 8, rng);

    // Same key appended again: the later record wins
    const auto newer = make_payload(rng, 300);
    const auto rec = oc::make_record(b.keys[2], oc::flag_zlib, 5000, newer.data(), newer.size());
    const std::uint64_t at = b.file.bytes.size();
    b.file.bytes.insert(b.file.bytes.end(), rec.begin(), rec.end());

    const auto res = oc::scan(b.file.reader(), b.begin, b.file.bytes.size());
    CHECK(res.records == 9 && res.index.size() == 8 && res.superseded == 1);
    CHECK(res.index.at(b.keys[2]).offset == at);
    std::vector<std::uint8_t> payload;
    CHECK(oc::read_record(b.file.reader(), res.index.at(b.keys[2]), b.keys[2], payload) && payload == newer);

    // Cap: a rewrite keeps the newest records that fit
    std::uint64_t newest3 = 0;
    std::vector<oc::entry> all;
    for (const auto& [k, e] : res.index) all.push_back(e);
    std::sort(all.begin(), all.end(), [](const oc::entry& a, const oc::entry& c) { return a.offset > c.offset; });
    for (int i = 0; i < 3; i++) newest3 += all[i].bytes();

    const auto kept = oc::select_for_rewrite(res, newest3);
    CHECK(kept.size() == 3);
    CHECK(kept.back().offset == at);
    CHECK(kept.front().offset == all[2].offset);
    CHECK(oc::needs_rewrite(res, b.begin + newest3));
    CHECK(!oc::needs_rewrite(res, oc::default_cap_bytes));
}

static void test_trim()
{
    const std::vector<oc::pack_file> packs = {
        {"spu-obj-v1-safe-aaaaaaaaaaaaaaaa.pack", 10 << 20, 100},
        {"spu-obj-v1-safe-bbbbbbbbbbbbbbbb.pack", 20 << 20, 300}, // newest other
        {"spu-obj-v1-mega-cccccccccccccccc.pack", 5 << 20, 200},
        {"spu-obj-v0-safe-dddddddddddddddd.pack", 1 << 20, 50},
        {"spu-obj-v1-safe-eeeeeeeeeeeeeeee.pack.tmp", 1 << 20, 400},
        {"spu-safe-v1-tane.dat", 3 << 20, 10},
        {"spu-obj-v1-safe-ffffffffffffffff.pack", 30 << 20, 500}, // current
    };

    const std::string current = "spu-obj-v1-safe-ffffffffffffffff.pack";
    auto removed = oc::stale_packs(packs, current, 1, 192ull << 20);
    std::set<std::string> r(removed.begin(), removed.end());
    CHECK(r.size() == 4);
    CHECK(r.count("spu-obj-v1-safe-eeeeeeeeeeeeeeee.pack.tmp"));
    CHECK(r.count("spu-obj-v1-safe-aaaaaaaaaaaaaaaa.pack"));
    CHECK(r.count("spu-obj-v1-mega-cccccccccccccccc.pack"));
    CHECK(r.count("spu-obj-v0-safe-dddddddddddddddd.pack"));
    CHECK(!r.count(current) && !r.count("spu-safe-v1-tane.dat") && !r.count("spu-obj-v1-safe-bbbbbbbbbbbbbbbb.pack"));

    // The newest other does not fit the budget next to the current pack: removed as well
    removed = oc::stale_packs(packs, current, 1, 40ull << 20);
    r = std::set<std::string>(removed.begin(), removed.end());
    CHECK(r.size() == 5 && r.count("spu-obj-v1-safe-bbbbbbbbbbbbbbbb.pack"));

    CHECK(oc::pack_name("Safe", make_digest(1)).rfind("spu-obj-v1-safe-", 0) == 0);
    CHECK(oc::pack_name("Safe", make_digest(1)).size() == std::string("spu-obj-v1-safe-").size() + 16 + 5);
    CHECK(oc::is_pack_file(oc::pack_name("Mega", make_digest(2))));
    CHECK(!oc::is_pack_file("spu-safe-v1-tane.dat"));
}

static void test_symbols()
{
    const std::set<std::string> decl = {"spu_dispatcher", "spu_timebase_offs", "__spu-0x00100-abc-pp-0", "spu_segment_base"};
    const auto declared = [&](std::string_view n) { return decl.count(std::string(n)) != 0; };
    const std::string main = "__spu-0x00100-abc";

    CHECK(oc::check_symbols(main, {main, "__spu-0x00100-abc-0x00104"}, {"spu_dispatcher", "memcpy", "fmaf", "_GLOBAL_OFFSET_TABLE_"}, declared).ok);
    CHECK(oc::check_symbols(main, {main}, {"spu_timebase_offs", "__spu-0x00100-abc-pp-0"}, declared).ok);

    auto r = oc::check_symbols(main, {"other"}, {}, declared);
    CHECK(!r.ok && r.name == main);
    r = oc::check_symbols(main, {main}, {"spu_unknown_helper"}, declared);
    CHECK(!r.ok && r.name == "spu_unknown_helper" && std::string(r.reason) == "unknown symbol");
    r = oc::check_symbols(main, {main}, {main}, declared);
    CHECK(!r.ok && std::string(r.reason) == "entry symbol undefined");
    r = oc::check_symbols(main, {main}, {"__spu-0x00100-abc-pp-1"}, declared);
    CHECK(!r.ok);
}

static void test_stamp()
{
    std::uint8_t payload[oc::stamp_payload_size];
    for (std::size_t i = 0; i < sizeof(payload); i++) payload[i] = static_cast<std::uint8_t>(i);
    CHECK(oc::stamp_payload_hex(payload).empty());
    std::memset(payload, 0, sizeof(payload));
    CHECK(oc::stamp_payload_hex(payload).empty());
    const oc::digest d = make_digest(42);
    CHECK(oc::stamp_payload_hex(d.data()) == oc::hex(d));
    CHECK(oc::stamp_marker_size + oc::stamp_payload_size + 8 == 64);
}

static void test_timebase()
{
    using tm = oc::timebase_mode;

    // The pack fingerprint follows the timer read mode
    auto named = sample_keys();
    named["timebase"] = oc::timebase_text(tm::named);
    auto baked = named;
    baked["timebase"] = oc::timebase_text(tm::baked);
    CHECK(oc::fingerprint(oc::canonical_text(named)) != oc::fingerprint(oc::canonical_text(baked)));
    CHECK(oc::first_difference(oc::canonical_text(baked), oc::canonical_text(named)) == "timebase: baked -> named");

    // First boot opens a pack: named for the process; a later boot with the cache off keeps it, its pack is keyed named
    {
        std::atomic<std::uint8_t> state{0};
        const tm open = oc::timebase_for_open(static_cast<tm>(state.load()), false);
        CHECK(open == tm::named);
        CHECK(oc::timebase_settle(state, open) == tm::named);
        CHECK(oc::timebase_settle(state, tm::baked) == tm::named);
        CHECK(oc::timebase_for_open(static_cast<tm>(state.load()), true) == tm::named);
    }

    // Kill switch, unstamped build or any other cache-off reason at the first boot: baked for the process
    {
        std::atomic<std::uint8_t> state{0};
        CHECK(oc::timebase_settle(state, tm::baked) == tm::baked);
        CHECK(oc::timebase_for_open(static_cast<tm>(state.load()), false) == tm::baked);
        CHECK(oc::timebase_settle(state, tm::named) == tm::baked);
    }

    // spu-timebase-baked.flag with the cache on: baked, and the pack is keyed baked
    {
        std::atomic<std::uint8_t> state{0};
        const tm open = oc::timebase_for_open(tm::undecided, true);
        CHECK(open == tm::baked);
        CHECK(oc::timebase_settle(state, open) == tm::baked);
    }

    // A compile before any open decides baked; the open then keys its pack baked
    {
        std::atomic<std::uint8_t> state{0};
        CHECK(oc::timebase_settle(state, tm::baked) == tm::baked);
        const tm open = oc::timebase_for_open(static_cast<tm>(state.load()), false);
        CHECK(open == tm::baked && oc::timebase_settle(state, open) == open);
    }

    // Concurrent first decisions: exactly one wins, every caller sees it
    {
        std::atomic<std::uint8_t> state{0};
        std::atomic<int> named_seen{0}, baked_seen{0};
        std::vector<std::thread> threads;

        for (int t = 0; t < 8; t++)
        {
            threads.emplace_back([&, t]()
            {
                const tm got = oc::timebase_settle(state, t & 1 ? tm::named : tm::baked);
                (got == tm::named ? named_seen : baked_seen)++;
            });
        }

        for (auto& t : threads) t.join();
        CHECK((named_seen == 8) != (baked_seen == 8));
        CHECK(named_seen + baked_seen == 8);
    }
}

static void test_threads()
{
    // 6 writers append through one mutex (as the console does): every record stays whole and readable
    std::mt19937 seed_rng(23);
    const oc::digest fp = make_digest(11);
    mem_file f;
    f.bytes = oc::make_header(fp, "format=1\n", 1);
    const std::uint64_t begin = f.bytes.size();
    std::mutex mutex;
    std::vector<std::thread> threads;

    for (std::uint32_t t = 0; t < 6; t++)
    {
        threads.emplace_back([&, t, seed = seed_rng()]()
        {
            std::mt19937 rng(seed);

            for (std::uint32_t i = 0; i < 2000; i++)
            {
                const auto payload = make_payload(rng, 8 + rng() % 200);
                const auto rec = oc::make_record(make_digest(t * 100000 + i), 0, static_cast<std::uint32_t>(payload.size()), payload.data(), payload.size());
                std::lock_guard lock(mutex);
                f.bytes.insert(f.bytes.end(), rec.begin(), rec.end());
            }
        });
    }

    for (auto& t : threads) t.join();

    const auto res = oc::scan(f.reader(), begin, f.bytes.size());
    CHECK(res.records == 12000 && res.index.size() == 12000 && res.good_end == f.bytes.size() && res.damaged == 0);
}

int main()
{
    test_sha256();
    test_keys();
    test_round_trip();
    test_truncation();
    test_corruption();
    test_supersede_and_cap();
    test_trim();
    test_symbols();
    test_stamp();
    test_timebase();
    test_threads();

    if (g_failures)
    {
        std::printf("spu_object_cache_test: %d failure(s)\n", g_failures);
        return 1;
    }

    std::printf("spu_object_cache_test: all passed\n");
    return 0;
}
