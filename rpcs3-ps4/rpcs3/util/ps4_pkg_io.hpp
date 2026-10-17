#pragma once

// Read policy of the PS3 package installer on the PS4 (Crypto/unpkg.cpp), free of file access so the host test
// (ps4-port/tests/pkg_check_test.cpp) drives it with simulated drives.
// Utilities/File.cpp retries a failing read for about 300 ms. A USB drive that is spinning up, was reset or is busy can
// take longer, and one entry cut short failed the whole installation, or, worse, left the next read on an offset that
// is not a multiple of 16, where the package keystream (counter = offset / 16) no longer matches: a corrupted file that
// was reported as installed.

#include <cstddef>
#include <cstdint>

namespace utils::ps4::pkg_io
{
// Pauses before each new attempt at the rest of a short read: about 26 s in all.
inline constexpr std::uint32_t retry_delays_ms[] = {100, 250, 500, 1000, 2000, 3000, 4000, 5000, 5000, 5000};

struct rest_result
{
    std::uint64_t done = 0;      // bytes read in all (the first short read included)
    std::uint32_t waited_ms = 0; // time spent waiting between attempts
    bool complete = false;       // every byte asked for was read
    bool at_end = false;         // the read stopped at the end of the file: not an error
    bool stopped = false;        // stop() asked to give up (Circle)
};

// The rest of a read of 'num_bytes' at 'offset' that returned only 'done' bytes.
// read(offset, done) -> bytes read now into the caller's buffer at 'done' (0 or fewer on failure);
// file_size() -> current size of the file (0 = unknown); wait(ms); stop() -> true to give up.
template <typename Read, typename Size, typename Wait, typename Stop>
rest_result read_rest(std::uint64_t offset, std::uint64_t num_bytes, std::uint64_t done, Read&& read, Size&& file_size, Wait&& wait, Stop&& stop)
{
    rest_result r;
    r.done = done;
    const auto end_reached = [&]
    {
        const std::uint64_t size = file_size();
        return size != 0 && offset + r.done >= size;
    };
    for (const std::uint32_t delay : retry_delays_ms)
    {
        if (r.done >= num_bytes) break;
        if (end_reached())
        {
            r.at_end = true;
            return r;
        }
        if (stop())
        {
            r.stopped = true;
            return r;
        }
        wait(delay);
        r.waited_ms += delay;
        r.done += read(offset + r.done, r.done);
    }
    r.complete = r.done >= num_bytes;
    if (!r.complete) r.at_end = end_reached();
    return r;
}

// Bytes of a read cut short by an I/O error that the package decryption may hand back: whole 16-byte blocks only, so
// the caller's next read starts where the keystream counter is right.
constexpr std::size_t whole_blocks(std::size_t got) noexcept
{
    return got & ~std::size_t{15};
}
}
