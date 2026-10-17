#pragma once

#include <string>

namespace utils::ps4
{
// Read supplies optional {name, symlink}; query supplies metadata for an
// absolute path. The PS4 SDK's fstatat is an ENOSYS stub, so native callers
// must retain the opened directory path and use stat on each full name.
// Keep skipping unreadable entries iterative: a long run must not recurse.
template <typename Read, typename Query, typename Entry>
bool read_directory_entry(const std::string& root, Read read, Query query, Entry& output)
{
    while (const auto item = read())
    {
        const std::string name(item->name);
        if (name.empty() || name.find('/') != name.npos) continue;
        const std::string path = root + (root.empty() || root.back() != '/' ? "/" : "") + name;
        if (!query(path, output)) continue;
        output.name = name;
        output.is_symlink = item->symlink;
        return true;
    }
    return false;
}
}
