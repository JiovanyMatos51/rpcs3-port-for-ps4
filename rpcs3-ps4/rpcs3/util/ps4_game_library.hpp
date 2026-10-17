#pragma once

#include <algorithm>
#include <cstddef>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace utils::ps4
{
enum class game_kind { none, folder, iso, package, executable };

inline game_kind file_kind(std::string_view path)
{
    const auto dot = path.find_last_of('.');
    if (dot == path.npos) return game_kind::none;
    std::string extension(path.substr(dot));
    for (char& c : extension) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    if (extension == ".iso") return game_kind::iso;
    if (extension == ".pkg") return game_kind::package;
    if (extension == ".elf" || extension == ".self") return game_kind::executable;
    return game_kind::none;
}

struct library_item
{
    std::string name;
    bool directory = false;
    bool symlink = false;
};

// Kinds of games a scanned folder may contribute (bit set).
enum : unsigned
{
    accept_folder = 1u << 0,
    accept_iso = 1u << 1,
    accept_package = 1u << 2,
    accept_executable = 1u << 3,
    accept_all = accept_folder | accept_iso | accept_package | accept_executable,
};

inline unsigned accept_bit(game_kind kind)
{
    switch (kind)
    {
    case game_kind::folder: return accept_folder;
    case game_kind::iso: return accept_iso;
    case game_kind::package: return accept_package;
    case game_kind::executable: return accept_executable;
    default: return 0;
    }
}

inline std::string ascii_upper(std::string_view text)
{
    std::string out(text);
    for (char& c : out) if (c >= 'a' && c <= 'z') c -= 'a' - 'A';
    return out;
}

inline std::string ascii_lower(std::string_view text)
{
    std::string out(text);
    for (char& c : out) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return out;
}

// 0.34: entries of a game folder that are never games of their own (disc dump and PSN layouts).
inline bool game_internal_entry(std::string_view name, bool directory)
{
    const std::string upper = ascii_upper(name);
    if (!directory) return upper == "PS3_DISC.SFB" || upper == "PARAM.SFO";
    if (upper.size() == 8 && upper.starts_with("PS3_GM") && upper[6] >= '0' && upper[6] <= '9' && upper[7] >= '0' && upper[7] <= '9') return true;
    return upper == "PS3_GAME" || upper == "PS3_UPDATE" || upper == "PS3_EXTRA" || upper == "PS3_DVD" || upper == "USRDIR" ||
        upper == "TROPDIR" || upper == "LICDIR";
}

// 0.34: number of a package part ("NAME_07.pkg" -> 7, any case) and its lower-case "path/name" stem; -1 for
// anything else. Publishers split large PKGs this way; the first part reads the others when it is installed.
inline int package_part(std::string_view path, std::string* stem = nullptr)
{
    constexpr std::size_t tail = 7; // "_NN.pkg"
    if (file_kind(path) != game_kind::package || path.size() <= tail) return -1;
    const std::string_view end = path.substr(path.size() - tail);
    if (end[0] != '_' || end[1] < '0' || end[1] > '9' || end[2] < '0' || end[2] > '9' || end[3] != '.') return -1;
    if (stem) *stem = ascii_lower(path.substr(0, path.size() - tail));
    return (end[1] - '0') * 10 + (end[2] - '0');
}

// 0.34: the later parts of a split PKG are hidden when its _00 part is listed: only that one installs.
inline void hide_package_parts(std::vector<std::string>& games)
{
    std::vector<std::string> firsts;
    std::string stem;
    for (const auto& game : games)
        if (package_part(game, &stem) == 0) firsts.push_back(stem);
    if (firsts.empty()) return;
    std::sort(firsts.begin(), firsts.end());
    std::erase_if(games, [&](const std::string& game) { return package_part(game, &stem) > 0 && std::binary_search(firsts.begin(), firsts.end(), stem); });
}

namespace library_detail
{
// Walk below one library folder. Recognized game folders are terminal: their internal files are never listed as
// separate games. Links, dot entries and incomplete FTP temporary files are skipped. Every enumerated entry costs one
// unit of 'budget'; when it runs out the walk stops and 'truncated' is set.
// 0.34: 'top' is the library folder itself. When it is a game (a dump copied straight into it), it is listed and its
// other entries are still walked, so one wrong copy no longer hides every other game of that folder.
template <typename Enumerate, typename IsFile>
void scan_tree(std::vector<std::string>& games, std::string path, unsigned depth, unsigned kinds, Enumerate& enumerate,
    IsFile& is_file, std::size_t& budget, bool& truncated, bool top = false)
{
    while (!path.empty() && path.back() == '/') path.pop_back();
    const bool game = is_file(path + "/PS3_GAME/USRDIR/EBOOT.BIN") || is_file(path + "/USRDIR/EBOOT.BIN");
    if (game)
    {
        if (kinds & accept_folder) games.push_back(path);
        if (!top) return;
    }
    if (truncated) return;
    // 0.34: at the last level only files can be listed; a folder-only root (dev_hdd0/game) needs no listing there.
    if (!depth && !(kinds & ~unsigned{accept_folder})) return;
    for (const auto& item : enumerate(path))
    {
        if (!budget)
        {
            truncated = true;
            return;
        }
        --budget;
        if (item.name.empty() || item.name[0] == '.' || item.symlink) continue;
        if (game && game_internal_entry(item.name, item.directory)) continue;
        const auto child = path + '/' + item.name;
        if (item.directory)
        {
            if (depth) scan_tree(games, child, depth - 1, kinds, enumerate, is_file, budget, truncated);
            if (truncated) return;
        }
        else if (accept_bit(file_kind(child)) & kinds) games.push_back(child);
    }
}

inline void sort_unique(std::vector<std::string>& games)
{
    std::sort(games.begin(), games.end());
    games.erase(std::unique(games.begin(), games.end()), games.end());
}
}

// 0.34: one library folder with its own walk depth and accepted kinds.
struct library_root
{
    std::string path;
    unsigned depth = 2;
    unsigned kinds = accept_all;
};

// Directory enumeration and file queries are supplied by RPCS3's fs layer.
// Recognized game folders are terminal: don't list their internal files as
// separate games. Never follow links or incomplete FTP temporary files.
template <typename Enumerate, typename IsFile>
std::vector<std::string> scan_game_library_roots(const std::vector<library_root>& roots,
    const std::string& preferred, Enumerate enumerate, IsFile is_file)
{
    std::vector<std::string> games;
    std::size_t budget = std::numeric_limits<std::size_t>::max();
    bool truncated = false;
    for (const auto& root : roots)
        library_detail::scan_tree(games, root.path, root.depth, root.kinds, enumerate, is_file, budget, truncated, true);
    if (!preferred.empty()) games.push_back(preferred);
    library_detail::sort_unique(games);
    hide_package_parts(games);
    return games;
}

// Every root walked two levels deep, all kinds accepted.
template <typename Enumerate, typename IsFile>
std::vector<std::string> scan_game_library(const std::vector<std::string>& roots,
    const std::string& preferred, Enumerate enumerate, IsFile is_file)
{
    std::vector<library_root> all;
    for (const auto& root : roots) all.push_back({root});
    return scan_game_library_roots(all, preferred, std::move(enumerate), std::move(is_file));
}

// 0.34: folders of /data/rpcs3 read as game libraries (besides dev_hdd0/game), matched without case. /data is
// case-sensitive, so before only 'games', 'GAMES' and 'pkgs' were read and e.g. 'Games' or 'PS3ISO' were ignored.
inline bool internal_library_folder(std::string_view name)
{
    const std::string lower = ascii_lower(name);
    for (const char* known : {"games", "gamez", "ps3iso", "iso", "isos", "pkg", "pkgs", "packages"})
        if (lower == known) return true;
    return false;
}

// 0.34: name under which RPCS3 finds a license: exdata/<CONTENT_ID>.rap, the content id in upper case and the
// extension in lower case (system_utils.cpp opens exactly that name, and /data is case-sensitive). Empty when 'name'
// is not "<content id>.rap" in any case. A content id is 36 characters of A-Z, 0-9, '-' and '_'
// (e.g. UP0001-NPUB30554_00-0000000000000001).
inline std::string rap_license_name(std::string_view name)
{
    constexpr std::size_t id_size = 36;
    if (name.size() != id_size + 4 || ascii_lower(name.substr(id_size)) != ".rap") return {};
    const std::string id = ascii_upper(name.substr(0, id_size));
    for (const char c : id)
        if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return {};
    return id + ".rap";
}

// 0.33: USB drives. The PS4 mounts USB mass storage formatted on a PC (exFAT or FAT32) at /mnt/usb0 .. /mnt/usb7;
// a drive formatted as PS4 extended storage is encrypted with the console's keys and is never mounted there.
inline constexpr unsigned usb_drive_count = 8;
inline constexpr std::size_t usb_scan_budget = 20'000; // directory entries read per drive, so huge drives stay quick

inline std::string usb_mount(unsigned index)
{
    return "/mnt/usb" + std::to_string(index);
}

// Index of the USB drive a path lives on (/mnt/usbN or below it), -1 for the console's own storage.
inline int usb_drive_of(std::string_view path)
{
    constexpr std::string_view prefix = "/mnt/usb";
    if (!path.starts_with(prefix) || path.size() == prefix.size()) return -1;
    const char digit = path[prefix.size()];
    if (digit < '0' || digit >= static_cast<char>('0' + usb_drive_count)) return -1;
    if (path.size() > prefix.size() + 1 && path[prefix.size() + 1] != '/') return -1;
    return digit - '0';
}

// Where the XMB says a game is stored: "USB 1" for /mnt/usb0, "USB 2" for /mnt/usb1 ..., "PS4" otherwise.
inline std::string origin_label(std::string_view path)
{
    const int drive = usb_drive_of(path);
    return drive < 0 ? std::string("PS4") : "USB " + std::to_string(drive + 1);
}

// 0.34: a game folder that is a library folder itself (a dump copied straight into /data/rpcs3/games or USB:/GAMES):
// a folder right below /data/rpcs3 or a drive root, the only places the walk lists a folder as its own root.
inline bool library_root_is_game(std::string_view path)
{
    if (file_kind(path) != game_kind::none) return false;
    const auto slash = path.find_last_of('/');
    if (slash == std::string_view::npos || slash + 1 == path.size()) return false;
    const std::string_view parent = path.substr(0, slash);
    return parent == "/data/rpcs3" || (usb_drive_of(parent) >= 0 && parent.size() == usb_mount(0).size());
}

// Root folders of the PS3 USB layout (multiMAN / webMAN names; PS3 is the older one; 0.34: PACKAGES, the PS3 HEN
// package folder). Matched without case, as exFAT and FAT32 ignore it.
inline bool usb_layout_folder(std::string_view name)
{
    const std::string upper = ascii_upper(name);
    return upper == "PS3ISO" || upper == "GAMES" || upper == "GAMEZ" || upper == "PS3" || upper == "PKG" || upper == "PACKAGES";
}

struct usb_scan
{
    std::vector<std::string> games;   // sorted, without duplicates
    std::vector<std::string> folders; // layout folders found at the root, spelled as on the drive
    std::size_t entries = 0;          // directory entries read, the root listing included
    bool truncated = false;           // the entry budget ran out before the walk ended
};

// 'root' is the listing of 'mount' itself. The root contributes disc images and packages only (0.34: PS3 packages at
// the root, where a PS3 reads them too; the PS4 packages often found there are told apart and hidden by the caller).
// Layout folders are walked two levels deep (GAMES/<game>, PS3ISO/<group>/<name>.iso), like the library folders on
// /data.
template <typename Enumerate, typename IsFile>
usb_scan scan_usb_drive(const std::string& mount, const std::vector<library_item>& root, Enumerate enumerate, IsFile is_file,
    std::size_t max_entries = usb_scan_budget)
{
    usb_scan out;
    std::size_t budget = max_entries;
    for (const auto& item : root)
    {
        if (!budget)
        {
            out.truncated = true;
            break;
        }
        --budget;
        if (item.name.empty() || item.name[0] == '.' || item.symlink) continue;
        if (item.directory)
        {
            if (usb_layout_folder(item.name)) out.folders.push_back(item.name);
        }
        else if (const game_kind kind = file_kind(item.name); kind == game_kind::iso || kind == game_kind::package)
            out.games.push_back(mount + '/' + item.name);
    }
    std::sort(out.folders.begin(), out.folders.end());
    for (const auto& folder : out.folders)
    {
        if (out.truncated) break;
        library_detail::scan_tree(out.games, mount + '/' + folder, 2, accept_all, enumerate, is_file, budget, out.truncated, true);
    }
    out.entries = max_entries - budget;
    library_detail::sort_unique(out.games);
    hide_package_parts(out.games);
    return out;
}
}
