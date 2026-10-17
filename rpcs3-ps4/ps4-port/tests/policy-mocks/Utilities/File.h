#pragma once

// Host-test stand-in (tests/policy_test.cpp): an in-memory file system for the flag and marker files that
// util/ps4_policy.hpp reads and writes under /data/rpcs3. Nothing touches the real disk; the adaptation files
// (util/ps4_adapt.hpp) use their own real I/O in a test directory.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <string_view>

namespace fs
{
inline std::map<std::string, std::string>& test_files()
{
    static std::map<std::string, std::string> files;
    return files;
}

enum open_mode : int
{
    read = 1,
    write = 2,
    create = 4,
    trunc = 8,
};

inline constexpr int rewrite = write | create | trunc;

inline bool is_file(const std::string& path)
{
    return test_files().count(path) != 0;
}

inline bool remove_file(const std::string& path)
{
    return test_files().erase(path) != 0;
}

inline bool create_path(const std::string&)
{
    return true;
}

// 0.33 (util/ps4_config_db.hpp): size and a modification time. The time is a hash of the contents, so any rewrite
// that changes the file looks newer, as a real copy would.
struct stat_t
{
    bool is_directory = false;
    std::uint64_t size = 0;
    std::int64_t mtime = 0;
};

inline bool get_stat(const std::string& path, stat_t& info)
{
    const auto it = test_files().find(path);
    if (it == test_files().end()) return false;
    info = {};
    info.size = it->second.size();
    info.mtime = static_cast<std::int64_t>(std::hash<std::string>{}(it->second) >> 1);
    return true;
}

class file
{
    std::string m_path;
    bool m_open = false;
    mutable std::size_t m_pos = 0;

public:
    file() = default;

    explicit file(const std::string& path, int mode = open_mode::read)
        : m_path(path)
    {
        auto& files = test_files();
        if (mode & create)
        {
            std::string& text = files[path];
            if (mode & trunc) text.clear();
            m_open = true;
        }
        else
        {
            m_open = files.count(path) != 0;
        }
    }

    explicit operator bool() const { return m_open; }

    std::uint64_t size() const { return test_files()[m_path].size(); }

    std::uint64_t read(void* buffer, std::uint64_t count) const
    {
        const std::string& text = test_files()[m_path];
        const std::size_t n = std::min<std::size_t>(static_cast<std::size_t>(count), text.size() - std::min(m_pos, text.size()));
        std::memcpy(buffer, text.data() + m_pos, n);
        m_pos += n;
        return n;
    }

    std::uint64_t write(const void* buffer, std::uint64_t count) const
    {
        test_files()[m_path].append(static_cast<const char*>(buffer), static_cast<std::size_t>(count));
        return count;
    }

    std::string to_string() const { return test_files()[m_path]; }
};

// The settings menu's paths and its atomic save (fs::pending_file: the text replaces the file only on commit()).
inline std::string get_config_dir(bool = false)
{
    return "/data/rpcs3/config/";
}

inline std::string get_parent_dir(std::string_view path)
{
    const auto slash = path.rfind('/');
    return std::string(path.substr(0, slash == std::string_view::npos ? 0 : slash));
}

class pending_file
{
    std::string m_dest, m_temp;

public:
    fs::file file;

    explicit pending_file(const std::string& path)
        : m_dest(path), m_temp(path + ".pending"), file(m_temp, rewrite)
    {
    }

    bool commit()
    {
        auto& files = test_files();
        files[m_dest] = files[m_temp];
        files.erase(m_temp);
        return true;
    }
};
} // namespace fs
