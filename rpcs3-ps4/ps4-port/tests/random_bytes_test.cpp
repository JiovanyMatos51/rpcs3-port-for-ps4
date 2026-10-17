#include "util/ps4_random.hpp"
#include <array>
#include <cassert>
#include <cstdio>
#include <vector>

int main()
{
    for (std::size_t length : {0u, 1u, 63u, 64u, 65u, 128u, 129u, 1024u})
    {
        std::vector<std::uint8_t> storage(length + 2, 0xa5);
        std::size_t total = 0, calls = 0;
        const int result = utils::ps4::random_bytes(std::span(storage).subspan(1, length), [&](void* buffer, std::size_t count)
        {
            assert(count > 0 && count <= 64);
            assert(buffer == storage.data() + 1 + total);
            std::fill_n(static_cast<std::uint8_t*>(buffer), count, 0x37);
            total += count; ++calls; return 0;
        });
        assert(result == 0 && total == length && calls == (length + 63) / 64);
        assert(storage.front() == 0xa5 && storage.back() == 0xa5);
        assert(std::all_of(storage.begin() + 1, storage.end() - 1, [](auto byte) { return byte == 0x37; }));
    }
    std::array<std::uint8_t, 129> failure{};
    int calls = 0;
    const int result = utils::ps4::random_bytes(failure, [&](void*, std::size_t)
    { return ++calls == 2 ? -31 : 0; });
    assert(result == -31 && calls == 2);
    assert(utils::ps4::random_bytes(failure, [](void*, std::size_t) { return 1; }) == 1);
    std::puts("PS4 entropy adapter: chunk limits, boundaries and failure propagation passed");
}
