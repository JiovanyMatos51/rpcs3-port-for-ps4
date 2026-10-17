#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace utils::ps4
{
// Each view aliases the same direct-memory pages. Unlike PS4 shm_open,
// this storage does not consume the limited flexible-memory pool.
class shared_memory
{
    struct chunk { std::int64_t physical; std::size_t size; };
    struct view { void* address; bool restore_reservation; };
    std::size_t m_size;
    std::vector<chunk> m_chunks;
    std::vector<view> m_views;
    std::mutex m_mutex;
    bool m_valid = false, m_initialized = false;

    bool remove_view(const view& mapping);

public:
    explicit shared_memory(std::size_t size);
    ~shared_memory();
    shared_memory(const shared_memory&) = delete;
    shared_memory& operator=(const shared_memory&) = delete;
    bool valid() const { return m_valid; }
    void* map(void* address, int protection, bool cow = false, bool overwrite = true);
    bool unmap(void* address, bool restore_reservation);
};
}
