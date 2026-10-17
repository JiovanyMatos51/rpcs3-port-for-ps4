#include "../../rpcs3/util/ps4_directory.hpp"

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <vector>

int main(int argc, char** argv)
{
    assert(argc == 2);
    namespace fs = std::filesystem;
    const fs::path root(argv[1]);
    assert(!fs::exists(root));
    fs::create_directories(root / "Disc/PS3_GAME/USRDIR");
    const std::string long_name = "Minecraft - PlayStation 3 Edition (USA) (En,Ja,Fr,De,Es,It,Nl,Pt,Sv,No,Da,Fi,Ko,Pl,Ru,Tr).iso";
    std::ofstream(root / long_name) << "ISO fixture";
    const auto base = root.generic_string();
    struct raw_entry { std::string name; bool symlink; };
    struct metadata { std::string name; bool is_symlink = false, is_directory = false; std::uintmax_t size = 0; };
    std::vector<raw_entry> records;
    // Broken/unreadable entries must be skipped without growing the stack.
    for (unsigned i = 0; i < 20000; ++i) records.push_back({"missing", false});
    records.push_back({"", false});
    records.push_back({"../outside.iso", false});
    records.push_back({long_name, false});
    records.push_back({"Disc", false});
    records.push_back({long_name, true});
    std::size_t cursor = 0, queries = 0;
    auto read = [&]() -> std::optional<raw_entry>
    {
        if (cursor == records.size()) return std::nullopt;
        return records[cursor++];
    };
    auto query = [&](const std::string& path, metadata& out)
    {
        ++queries;
        assert(path.find(base + '/') == 0);
        std::error_code error;
        const auto status = fs::status(path, error);
        if (error || !fs::exists(status)) return false;
        out.is_directory = fs::is_directory(status);
        out.size = out.is_directory ? 0 : fs::file_size(path);
        return true;
    };
    metadata out;
    assert(utils::ps4::read_directory_entry(base, read, query, out));
    assert(out.name == long_name && out.size == 11 && !out.is_directory && !out.is_symlink);
    assert(queries == 20001);
    assert(utils::ps4::read_directory_entry(base + '/', read, query, out));
    assert(out.name == "Disc" && out.is_directory && !out.is_symlink);
    assert(utils::ps4::read_directory_entry(base, read, query, out));
    assert(out.name == long_name && out.is_symlink && !out.is_directory);
    assert(!utils::ps4::read_directory_entry(base, read, query, out));
    cursor = 20002; // Rewind to the first readable raw directory record.
    assert(utils::ps4::read_directory_entry(base, read, query, out) && out.name == long_name);
    fs::remove_all(root); // Newly created fixture only; existing dirs refused.
}
