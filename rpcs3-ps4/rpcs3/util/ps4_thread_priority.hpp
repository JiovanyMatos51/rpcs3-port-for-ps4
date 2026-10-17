#pragma once

#include <algorithm>

namespace utils::ps4
{
// Orbis native priorities run from 256 (highest) to 767 (lowest).
// Keep a small adjustment from the inherited priority, rather than switching
// to a scheduler extreme. A zero request must restore that inherited value.
class thread_priority
{
    int m_original = 0;
    bool m_saved = false;

public:
    template <typename Get, typename Set>
    int apply(int request, Get get, Set set)
    {
        if (!m_saved)
        {
            if (!request) return 0;
            int original = 0;
            if (const int error = get(&original)) return error;
            m_original = original;
            m_saved = true;
        }

        const int target = request == 0 ? m_original :
            std::clamp(m_original + (request > 0 ? -8 : 8), 256, 767);
        const int error = set(target);
        // Retain the saved value after a failed restoration so it can be retried.
        if (!error && !request) m_saved = false;
        return error;
    }
};
}
