#pragma once

// 0.33: PS3 firmware installation from the XMB (Configurações > Instalar firmware do PS3), as 'Install Firmware' of the
// PC build (main_window::HandlePupInstallation). Used by ps4_launcher.cpp; decrypt_package() is in ps4_firmware.cpp.
//
// Same checks and same extraction as the PC: the PS3UPDAT.PUP is validated as pup_object does (header, size against the
// header, file table, HMAC-SHA1 of every entry), entry 0x100 holds the version, entry 0x300 is update_files.tar, and
// every dev_flash_* package in it is an SCE file decrypted with SCEDecrypter (SCEPKG_ERK / SCEPKG_RIV) whose third
// section is a TAR of dev_flash/... files. version.txt (dev_flash/vsh/etc) comes from that archive, as on the PC.
// Differences:
//   - the PUP is looked for in fixed places, in this order: USB:/PS3/UPDATE/PS3UPDAT.PUP (the PS3's own USB layout) and
//     USB:/PS3UPDAT.PUP on /mnt/usb0..7, then /data/rpcs3 and /data/rpcs3/import. It is only ever read;
//   - no PUP entry is held in memory whole: pup_object (validate_hashes) and pup_object::get_file read an entry into one
//     buffer, ~190 MB for update_files.tar. The PS4 heap (orbis-compat musl over 128 MiB direct-memory carve-outs, 2 GiB
//     cap, never given back) can serve a block that size only from a new physically contiguous carve-out, which fails
//     near the cap (after a game) or with fragmented memory, and a failed allocation ends the application (the code is
//     built without exceptions). The pup_object checks are therefore streamed in 4 MiB pieces, and the install copies
//     update_files.tar into <dev_flash>.installing, hashing exactly the bytes it writes (a USB drive pulled out or a
//     PUP changed since the check gives a mismatch), and reads the packages from that copy, one at a time;
//   - the packages are extracted into <dev_flash>.installing and every file is checked against its TAR size (TAR.cpp
//     ignores write results, and on the PS4 a full /data gives a short write, not an error) and flushed with fsync
//     (TAR.cpp closes without it), then the folders of the new tree are flushed. Only then is the installed firmware
//     renamed to <dev_flash>.old and the new tree renamed into its place;
//   - <dev_flash>.old is kept after the swap. The first XMB of a later start gives the new firmware a quick check
//     (complete(): version.txt matches the installed version, key files present with data): passed, .old is removed
//     by the cleanup job; failed, .old is put back and the XMB says so. <dev_flash>.state, written atomically before
//     the swap and after the check, records which version the swap installed (marker);
//   - a stop at any point leaves one firmware, the previous or the new one, or none when none was installed:
//     recover() puts <dev_flash>.old back when the swap itself was cut short, and every XMB line says which one is left;
//   - nothing outside /data/rpcs3 is written: a dev_flash that vfs.yml places elsewhere, or that is a link, is refused,
//     and the leftovers are removed without following links (remove_tree()).

#include "util/types.hpp"
#include "util/ps4_game_library.hpp"
#include "util/ps4_i18n.hpp"
#include "util/serialization.hpp"
#include "util/sysinfo.hpp"
#include "Utilities/File.h"
#include "Utilities/StrFmt.h"
#include "Crypto/key_vault.h"
#include "Crypto/sha1.h"
#include "Loader/PUP.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "Loader/TAR.h"

void ps4_startup_checkpoint(const char* stage); // ps4_launcher.cpp, any thread

namespace utils::ps4::firmware
{
// The only folder the installer writes to.
inline constexpr std::string_view data_root = "/data/rpcs3";
inline constexpr std::string_view pup_name = "PS3UPDAT.PUP";
// main_window.cpp 'cur_version': an older PUP still installs; the XMB only says it is not the newest.
inline constexpr std::string_view newest_version = "4.93";
// PUP entries the PC installer reads: the version text and update_files.tar.
inline constexpr u64 version_entry = 0x100;
inline constexpr u64 update_files_entry = 0x300;
// First bytes of a PS4 system update file (PS4UPDATE.PUP); a PS3 PUP starts with "SCEUF".
inline constexpr std::array<u8, 4> ps4_pup_magic{0x4f, 0x15, 0x3d, 0x1d};
// A PS3 PUP has a few dozen entries; a larger count is refused before its tables are allocated.
inline constexpr u64 max_entries = 1024;
// Read size of the streamed HMAC checks and of the update_files.tar copy.
inline constexpr u64 stream_chunk = 4ull << 20;

inline void note(const std::string& line)
{
    ps4_startup_checkpoint(line.c_str());
}

// Decrypts one dev_flash_* package (an SCE file) with SCEDecrypter and the SCEPKG keys, as main_window.cpp, and returns
// its third section, the TAR of dev_flash files; an empty file and 'error' when that fails. Defined in
// ps4_firmware.cpp, built with the loader: Crypto/unself.h includes "zlib.h", which the frontend's include path lacks.
fs::file decrypt_package(const fs::file& package, std::string& error);

// Cuts UTF-8 text to at most 'limit' bytes on a character boundary (one XMB message line is ~116 bytes).
inline std::string clip(std::string text, std::size_t limit)
{
    if (text.size() <= limit) return text;
    std::size_t cut = limit > 3 ? limit - 3 : 0;
    while (cut && (static_cast<unsigned char>(text[cut]) & 0xc0) == 0x80) --cut;
    text.resize(cut);
    return text + "...";
}

// Byte bound of one XMB message line in the interface language: an Arabic letter takes two bytes for about the width
// of a Latin one
inline std::size_t line_bytes() noexcept
{
    return i18n::rtl() ? 232 : 116;
}

// ---------------------------------------------------------------------------------------------------------------
// Folders

// Absolute path with '.', '..', repeated and trailing slashes resolved lexically, like the frontend's resolve_path.
inline std::string normalize(std::string_view path)
{
    if (!path.starts_with('/')) return {};
    std::vector<std::string_view> parts;
    for (std::size_t pos = 0; pos < path.size();)
    {
        const std::size_t end = std::min(path.find('/', pos), path.size());
        const std::string_view part = path.substr(pos, end - pos);
        pos = end + 1;
        if (part.empty() || part == ".") continue;
        if (part == "..")
        {
            if (!parts.empty()) parts.pop_back();
            continue;
        }
        parts.push_back(part);
    }
    std::string out;
    for (const std::string_view part : parts)
    {
        out += '/';
        out += part;
    }
    return out.empty() ? std::string("/") : out;
}

// fs::get_stat follows links (its is_symlink is never set on this host) and fs::remove_all walks into a linked
// folder, so a link is told apart with lstat.
inline bool is_link(const std::string& path)
{
    struct ::stat info{};
    return ::lstat(path.c_str(), &info) == 0 && S_ISLNK(info.st_mode);
}

// The emulator's dev_flash folder (vfs.yml, normally /data/rpcs3/dev_flash/) without the trailing slash. Empty, and the
// installer refuses, when it is not inside /data/rpcs3 (nothing else may be written) or its name does not start with
// "dev_flash": the install replaces the whole folder, so a vfs.yml that points dev_flash at dev_hdd0 or at /data/rpcs3
// itself must never be taken for it.
inline std::string target_folder(std::string_view dev_flash)
{
    const std::string path = normalize(dev_flash);
    const std::string root = std::string(data_root) + '/';
    if (!path.starts_with(root) || path.size() <= root.size()) return {};
    if (!std::string_view(path).substr(path.find_last_of('/') + 1).starts_with("dev_flash")) return {};
    // The install renames and removes the folder and its .installing/.old siblings; through a link that removal
    // would reach whatever the link points to (a USB drive, another app's data).
    if (is_link(path) || is_link(path + ".installing") || is_link(path + ".old")) return {};
    return path;
}

inline std::string staging_folder(const std::string& target)
{
    return target + ".installing";
}

inline std::string previous_folder(const std::string& target)
{
    return target + ".old";
}

inline bool has_entries(const std::string& path)
{
    fs::dir directory(path);
    fs::dir_entry entry;
    while (directory && directory.read(entry))
        if (entry.name != "." && entry.name != "..") return true;
    return false;
}

inline bool filled(const std::string& path)
{
    return fs::is_dir(path) && has_entries(path);
}

// Device and inode of 'path' itself (a link is not followed); inode 0 when it cannot be read.
struct folder_id
{
    u64 device = 0;
    u64 inode = 0;

    bool operator==(const folder_id&) const = default;
};

inline folder_id identity(const std::string& path)
{
    struct ::stat info{};
    if (::lstat(path.c_str(), &info) != 0) return {};
    return {static_cast<u64>(info.st_dev), static_cast<u64>(info.st_ino)};
}

// A folder that a removal must never enter (dev_flash, or dev_flash.old while it is still the way back).
struct kept_folder
{
    std::string path;
    folder_id id;
};

// Removes 'path' and everything below it, as fs::remove_all, with two differences. A link is removed itself and never
// followed: fs::remove_all walks into a linked folder, so whatever it points to (a USB drive) would be emptied. And a
// folder that is one of 'keep' under another name is left in place, with the folders above it: a folder rename cut
// short by a power cut can leave one folder with two names until the file system is repaired, and emptying it through
// the second name would empty dev_flash. Everything else is removed; false when anything is left. A firmware tree is
// a few folders deep; deeper than 64 only a damaged file system (a folder inside itself) gets, and is left alone.
inline bool remove_tree(const std::string& path, const std::vector<kept_folder>& keep, u32 depth = 0)
{
    struct ::stat info{};
    if (::lstat(path.c_str(), &info) != 0)
    {
        fs::g_tls_error = fs::error::noent;
        return false;
    }
    if (!S_ISDIR(info.st_mode)) return fs::remove_file(path);
    if (depth > 64)
    {
        fs::g_tls_error = fs::error::toolong;
        return false;
    }
    const folder_id id{static_cast<u64>(info.st_dev), static_cast<u64>(info.st_ino)};
    for (const kept_folder& folder : keep)
    {
        if (!id.inode || id != folder.id) continue;
        note(fmt::format("firmware: '%s' is '%s' under a second name (a folder rename cut short by a power cut); it is left in place", path,
            folder.path));
        fs::g_tls_error = fs::error::exist;
        return false;
    }
    std::vector<std::string> names;
    {
        fs::dir listing(path);
        fs::dir_entry entry;
        while (listing && listing.read(entry))
            if (entry.name != "." && entry.name != "..") names.push_back(entry.name);
    }
    bool removed = true;
    for (const std::string& name : names)
        if (!remove_tree(path + '/' + name, keep, depth + 1)) removed = false;
    return removed && fs::remove_dir(path);
}

// Removes <target>.installing or <target>.old with remove_tree(), keeping <target> and <target>.old out of it.
inline bool remove_leftover(const std::string& target, const std::string& path)
{
    std::vector<kept_folder> keep;
    for (const std::string& folder : {target, previous_folder(target)})
        if (folder != path)
            if (const folder_id id = identity(folder); id.inode) keep.push_back({folder, id});
    return remove_tree(path, keep);
}

// fsync of a folder, so that the names created or renamed in it survive a power cut. Whether a folder can be opened
// for that is up to the file system, so a refusal only returns false and the caller goes on.
inline bool sync_folder(const std::string& path)
{
    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;
    int result = ::fsync(fd);
    for (int i = 0; i < 8 && result != 0 && errno == EINTR; ++i) result = ::fsync(fd);
    ::close(fd);
    return result == 0;
}

// ---------------------------------------------------------------------------------------------------------------
// Quick check of an installed firmware

// Read with data by complete(): version.txt, the modules a game boot loads (liblv2.sprx, which loads
// libsysmodule.sprx: PPUModule.cpp) and the VSH the PC build boots (vsh/module/vsh.self). Every official PUP has them.
inline constexpr std::array<std::string_view, 4> key_files{
    "vsh/etc/version.txt", "sys/external/liblv2.sprx", "sys/external/libsysmodule.sprx", "vsh/module/vsh.self"};

// utils::get_firmware_version()'s parsing ("release:04.9200:" -> "4.92"), for any folder.
inline std::string parse_version(std::string_view version)
{
    const usz start = version.find_first_of(':') + 1;
    const usz end = version.find_first_of(':', start);
    if (!start || end == umax) return {};
    version = version.substr(start, end - start);
    usz trim_start = version.find_first_not_of('0');
    if (trim_start == umax) return {};
    if (version[trim_start] == '.')
    {
        if (trim_start == 0) return {};
        trim_start--;
    }
    const usz dot_pos = version.find_first_of('.', trim_start);
    if (dot_pos == umax) return {};
    const usz trim_end = std::max(version.find_last_not_of('0', dot_pos + 1), std::min(dot_pos + 2, version.size()));
    return std::string(version.substr(trim_start, trim_end));
}

// The version in <folder>/vsh/etc/version.txt ("" when unreadable). The release line is the first one.
inline std::string version_of(const std::string& folder)
{
    const fs::file file(folder + "/vsh/etc/version.txt");
    if (!file) return {};
    std::string text(4096, '\0');
    text.resize(static_cast<std::size_t>(file.read(text.data(), text.size())));
    return parse_version(text);
}

// True when <folder> holds every key file with data and a readable version, equal to 'expected' when one is given.
// 'version' gets the version read, 'problem' the first thing that is wrong.
inline bool complete(const std::string& folder, std::string_view expected, std::string& version, std::string& problem)
{
    version = version_of(folder);
    for (const std::string_view name : key_files)
    {
        fs::stat_t stat{};
        if (!fs::get_stat(folder + '/' + std::string(name), stat))
        {
            problem = fmt::format("%s is missing", name);
            return false;
        }
        if (stat.is_directory || !stat.size)
        {
            problem = fmt::format("%s is %s", name, stat.is_directory ? "a folder" : "empty");
            return false;
        }
    }
    if (version.empty())
    {
        problem = "vsh/etc/version.txt has no readable version";
        return false;
    }
    if (!expected.empty() && version != expected)
    {
        problem = fmt::format("vsh/etc/version.txt reads '%s', but '%s' was installed", version, expected);
        return false;
    }
    return true;
}

// The firmware in <target> for an XMB line: "o 4.92", "o 4.92 incompleto" or "sem firmware" (interface language; pass
// it on with i18n::raw()).
inline std::string which(const std::string& target)
{
    std::string version, problem;
    if (complete(target, {}, version, problem)) return i18n::trf(i18n::str::FW_WHICH_VERSION, {version});
    return version.empty() ? std::string(i18n::tr(i18n::str::FW_SEM_FIRMWARE)) : i18n::trf(i18n::str::FW_WHICH_INCOMPLETE, {version});
}

// The same for startup.log: "4.92", "4.92 (incomplete)" or "none".
inline std::string which_log(const std::string& target)
{
    std::string version, problem;
    if (complete(target, {}, version, problem)) return version;
    return version.empty() ? std::string("none") : version + " (incomplete)";
}

// ---------------------------------------------------------------------------------------------------------------
// Install marker: <target>.state, "state=<name>\nversion=<x.yy>\n", replaced atomically (fs::pending_file: temporary
// file, fsync, rename), then its folder is flushed.

enum class install_state : u8
{
    none,     // no marker (never installed from the XMB) or unreadable
    pending,  // a swap to 'version' started: <target>.old holds the firmware it replaces until a later start checks it
    verified, // the firmware next to <target>.old passed that check: .old only waits for the cleanup job
    restored, // the install of 'version' was undone and the previous firmware put back
    failed,   // the install of 'version' ended before or during its swap and left the previous firmware
};

inline constexpr std::array<std::string_view, 5> install_state_names{"none", "pending", "verified", "restored", "failed"};

struct marker
{
    install_state state = install_state::none;
    std::string version;
};

inline std::string marker_path(const std::string& target)
{
    return target + ".state";
}

inline marker read_marker(const std::string& target)
{
    marker result;
    const fs::file file(marker_path(target));
    if (!file) return result;
    std::string text(256, '\0');
    text.resize(static_cast<std::size_t>(file.read(text.data(), text.size())));
    for (std::string_view rest = text; !rest.empty();)
    {
        const std::size_t end = std::min(rest.find('\n'), rest.size());
        std::string_view line = rest.substr(0, end);
        rest.remove_prefix(std::min(end + 1, rest.size()));
        if (line.ends_with('\r')) line.remove_suffix(1);
        if (line.starts_with("state="))
        {
            for (std::size_t i = 0; i < install_state_names.size(); ++i)
                if (line.substr(6) == install_state_names[i]) result.state = static_cast<install_state>(i);
        }
        else if (line.starts_with("version="))
        {
            result.version = std::string(line.substr(8, 16));
        }
    }
    return result;
}

// False when the marker could not be written (its old contents then stay as they were).
inline bool write_marker(const std::string& target, install_state state, const std::string& version)
{
    const std::string text = fmt::format("state=%s\nversion=%s\n", install_state_names[static_cast<std::size_t>(state)], version);
    fs::pending_file file(marker_path(target));
    if (!file.file || file.file.write(text.data(), text.size()) != text.size() || !file.commit())
    {
        note(fmt::format("firmware: cannot write '%s' (state %s, %s): %s", marker_path(target), install_state_names[static_cast<std::size_t>(state)],
            version, fs::g_tls_error));
        return false;
    }
    sync_folder(fs::get_parent_dir(target));
    return true;
}

// ---------------------------------------------------------------------------------------------------------------
// Recovery and leftovers

struct recovery
{
    bool restored = false; // the previous firmware was put back in <target>
    std::string log;       // startup.log line, "" when there was nothing to do
    std::string text;      // end of an XMB line (after "...: "): "o anterior voltou ao lugar e fica o 4.90" or why it did not
};

// An install stopped between its two renames (application closed, thread fatal) leaves the previous firmware in
// <target>.old and no <target>, or an empty one: Emu.Init() creates the dev_flash folder at every start. Puts the
// previous firmware back. Renames only, so the UI thread runs it before anything reads dev_flash. A non-empty <target>
// next to <target>.old means the swap finished (check_at_start() decides about .old).
inline recovery recover(const std::string& target)
{
    const std::string old = previous_folder(target);
    if (target.empty() || !fs::is_dir(old)) return {};
    const bool present = fs::is_dir(target);
    if (present && has_entries(target)) return {};
    const std::string stuck = i18n::trf(i18n::str::FW_OLD_STUCK, {old});
    if (present && !fs::remove_dir(target))
        return {false, fmt::format("firmware: cannot restore '%s': the empty '%s' cannot be removed (%s)", old, target, fs::g_tls_error), stuck};
    if (!fs::rename(old, target, false))
        return {false, fmt::format("firmware: cannot restore '%s' as '%s' (%s)", old, target, fs::g_tls_error), stuck};
    sync_folder(fs::get_parent_dir(target));
    // The swap the marker announced did not happen.
    if (const marker state = read_marker(target); state.state == install_state::pending) write_marker(target, install_state::restored, state.version);
    return {true, fmt::format("firmware: an install stopped while swapping folders; the previous firmware was restored from '%s' (now: %s)", old,
        which_log(target)), i18n::trf(i18n::str::FW_OLD_RESTORED, {i18n::raw(which(target))})};
}

// Folders removed by a cleanup job: <target>.installing (an install that failed or stopped, a firmware put aside by
// check_at_start()) and <target>.old once the firmware next to it passed the check at a later start.
inline bool has_leftovers(const std::string& target)
{
    if (target.empty()) return false;
    if (fs::is_dir(staging_folder(target))) return true;
    return fs::is_dir(previous_folder(target)) && filled(target) && read_marker(target).state == install_state::verified;
}

// ---------------------------------------------------------------------------------------------------------------
// Installed firmware

// What the XMB says about the installed firmware: the version from vsh/etc/version.txt (utils::get_firmware_version,
// read the same way by the boot and by the PC) and whether sys/external/liblv2.sprx exists, without which a boot that
// loads firmware modules fails with "Firmware is missing" (System.cpp).
struct installed_state
{
    std::string version;
    bool modules = false;

    bool ready() const { return !version.empty() && modules; }
};

// 'dev_flash': the configured folder with its trailing slash (g_cfg_vfs.get_dev_flash()).
inline installed_state installed(const std::string& dev_flash)
{
    return {::utils::get_firmware_version(), !dev_flash.empty() && fs::is_file(dev_flash + "sys/external/liblv2.sprx")};
}

// The PC compares the version strings the same way.
inline bool older_than_newest(std::string_view version)
{
    return version < newest_version;
}

// ---------------------------------------------------------------------------------------------------------------
// Finding and checking a PS3UPDAT.PUP

// Entry 'name' of 'folder', matched without case (exFAT and FAT32 keep the case it was written with, /data is
// case-sensitive); the exact name wins. "" when absent.
inline std::string child(const std::string& folder, std::string_view name, bool directory)
{
    const auto same = [&](std::string_view other)
    {
        return other.size() == name.size() && std::equal(other.begin(), other.end(), name.begin(), [](char a, char b)
        {
            const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
            return lower(a) == lower(b);
        });
    };
    fs::dir listing(folder);
    fs::dir_entry entry;
    std::string found;
    while (listing && listing.read(entry))
    {
        if (entry.is_directory != directory || !same(entry.name)) continue;
        if (entry.name == name) return folder + '/' + entry.name;
        if (found.empty()) found = folder + '/' + entry.name;
    }
    return found;
}

// Every PS3UPDAT.PUP in the search order. Directory listings only; nothing is opened.
inline std::vector<std::string> find_files()
{
    std::vector<std::string> found;
    const auto add = [&](std::string path)
    {
        if (!path.empty()) found.push_back(std::move(path));
    };
    for (unsigned i = 0; i < usb_drive_count; ++i)
    {
        const std::string mount = usb_mount(i);
        if (!fs::is_dir(mount)) continue;
        if (const std::string ps3 = child(mount, "PS3", true); !ps3.empty())
            if (const std::string update = child(ps3, "UPDATE", true); !update.empty()) add(child(update, pup_name, false));
    }
    for (unsigned i = 0; i < usb_drive_count; ++i)
    {
        const std::string mount = usb_mount(i);
        if (fs::is_dir(mount)) add(child(mount, pup_name, false));
    }
    add(child(std::string(data_root), pup_name, false));
    add(child(std::string(data_root) + "/import", pup_name, false));
    return found;
}

// "USB 1:/PS3/UPDATE/PS3UPDAT.PUP" for /mnt/usb0/..., the path itself on the console's storage.
inline std::string source_label(const std::string& path)
{
    const int drive = usb_drive_of(path);
    if (drive < 0) return path;
    return origin_label(path) + ":" + path.substr(usb_mount(static_cast<unsigned>(drive)).size());
}

// A PS3UPDAT.PUP that passed the PC build's checks.
struct package
{
    std::string path;
    std::string version;           // first line of entry 0x100
    u64 size = 0;                  // the PUP file
    u64 update_files_offset = 0;   // entry 0x300 in the PUP
    u64 update_files_size = 0;
    std::array<u8, 20> update_files_hash{}; // its HMAC from the PUP hash table
    fs::file file;                 // the PUP, kept open until the install
};

// pup_object's tables: the header, file_count file entries, then file_count hash entries (matched by index).
struct pup_tables
{
    PUPHeader header{};
    std::vector<PUPFileEntry> files;
    std::vector<PUPHashEntry> hashes;

    // Index of the first entry 'id' (pup_object::get_file takes the first one too); files.size() when there is none.
    std::size_t find(u64 id) const
    {
        for (std::size_t i = 0; i < files.size(); ++i)
            if (files[i].entry_id == id) return i;
        return files.size();
    }
};

enum class stream_result : u8
{
    ok,
    short_read,  // the PUP ended early or a read failed (USB drive pulled out)
    short_write, // the copy took fewer bytes (/data full)
    stopped,     // job::stop
};

// HMAC-SHA1 with the PUP key of 'length' bytes at 'offset', the value pup_object compares, read in stream_chunk pieces.
// 'copy', when given, receives every piece as it is hashed; 'done_mib', when given, the MiB done so far.
inline stream_result hmac_range(const fs::file& file, u64 offset, u64 length, std::array<u8, 20>& digest, const std::atomic<bool>& stop,
    const fs::file* copy = nullptr, std::atomic<u32>* done_mib = nullptr)
{
    std::vector<u8> buffer(static_cast<std::size_t>(std::min<u64>(length, stream_chunk)));
    sha1_context context{};
    sha1_hmac_starts(&context, PUP_KEY, sizeof(PUP_KEY));
    for (u64 at = 0; at < length;)
    {
        if (stop.load(std::memory_order_relaxed)) return stream_result::stopped;
        const u64 chunk = std::min<u64>(buffer.size(), length - at);
        if (file.read_at(offset + at, buffer.data(), chunk) != chunk) return stream_result::short_read;
        sha1_hmac_update(&context, buffer.data(), static_cast<std::size_t>(chunk));
        if (copy && copy->write(buffer.data(), chunk) != chunk) return stream_result::short_write;
        at += chunk;
        if (done_mib) done_mib->store(static_cast<u32>(at >> 20), std::memory_order_relaxed);
    }
    sha1_hmac_finish(&context, digest.data());
    return stream_result::ok;
}

// The checks of pup_object's constructor, in its order and with its results ('detail' gets its formatted error), with
// each entry hashed in pieces instead of one buffer. 'stopped' is set when job::stop ended the check.
inline pup_error validate(const fs::file& file, pup_tables& tables, std::string& detail, const std::atomic<bool>& stop, bool& stopped)
{
    const u64 size = file.size();
    PUPHeader& header = tables.header;
    if (file.read_at(0, &header, sizeof(header)) != sizeof(header))
    {
        detail = fmt::format("Too small PUP file to contain header: 0x%x", size);
        return pup_error::header_read;
    }
    if (header.magic != "SCEUF\0\0\0"_u64) return pup_error::header_magic;
    if (size < header.header_length || size - header.header_length < header.data_length)
    {
        detail = fmt::format("Firmware size mismatch, expected: 0x%x + 0x%x, actual: 0x%x", header.header_length, header.data_length, size);
        return pup_error::expected_size;
    }
    // pup_object refuses a count whose two tables do not fit in the file (fs::file::read of a vector).
    const u64 count = header.file_count;
    if (!count || count > max_entries || (size - sizeof(PUPHeader)) / (sizeof(PUPFileEntry) + sizeof(PUPHashEntry)) < count)
        return pup_error::header_file_count;
    tables.files.resize(static_cast<std::size_t>(count));
    tables.hashes.resize(static_cast<std::size_t>(count));
    const u64 files_bytes = count * sizeof(PUPFileEntry), hashes_bytes = count * sizeof(PUPHashEntry);
    if (file.read_at(sizeof(PUPHeader), tables.files.data(), files_bytes) != files_bytes ||
        file.read_at(sizeof(PUPHeader) + files_bytes, tables.hashes.data(), hashes_bytes) != hashes_bytes)
        return pup_error::header_file_count;
    for (std::size_t i = 0; i < tables.files.size(); ++i)
    {
        const PUPFileEntry& entry = tables.files[i];
        const u64 offset = entry.data_offset, length = entry.data_length;
        if (size < offset || size - offset < length)
        {
            detail = fmt::format("File database entry is invalid. (offset=0x%x, length=0x%x, PUP.size=0x%x)", offset, length, size);
            return pup_error::file_entries;
        }
        std::array<u8, 20> digest{};
        const stream_result read = hmac_range(file, offset, length, digest, stop);
        if (read == stream_result::stopped)
        {
            stopped = true;
            return pup_error::stream;
        }
        // pup_object ignores a short read and hashes the zeros it left: a mismatch as well.
        if (read != stream_result::ok || std::memcmp(digest.data(), tables.hashes[i].hash, digest.size()) != 0)
        {
            detail = fmt::format("Hash mismatch for file 0x%x (offset=0x%x, length=0x%x)%s", u64{entry.entry_id}, offset, length,
                read != stream_result::ok ? ", read failed" : "");
            return pup_error::hash_mismatch;
        }
    }
    return pup_error::ok;
}

// Opens and checks one PUP exactly as the PC does before it asks to install. 'error' gets the XMB reason; 'stopped' is
// set, without a reason, when job::stop ended the check (the whole file is read, ~200 MB from USB).
inline std::unique_ptr<package> verify(const std::string& path, std::string& error, const std::atomic<bool>& stop, bool& stopped)
{
    using i18n::str;
    fs::file file(path);
    if (!file)
    {
        error = i18n::tr(str::FW_ARQUIVO_NAO_PODE_SER);
        note(fmt::format("firmware: cannot open '%s' (%s)", path, fs::g_tls_error));
        return {};
    }
    const u64 size = file.size();
    if (std::array<u8, 4> magic{}; size >= magic.size() && file.read_at(0, magic.data(), magic.size()) == magic.size() && magic == ps4_pup_magic)
    {
        error = i18n::tr(str::FW_ATUALIZACAO_PS4_NAO_PS3_USE);
        note(fmt::format("firmware: '%s' rejected: PS4 system update file (magic 4f153d1d), not a PS3 PUP", path));
        return {};
    }
    pup_tables tables;
    std::string detail;
    if (const pup_error status = validate(file, tables, detail, stop, stopped); status != pup_error::ok)
    {
        if (stopped) return {};
        const char* name = "invalid";
        switch (status)
        {
        case pup_error::header_read:
            error = i18n::tr(str::FW_ARQUIVO_VAZIO_PEQUENO_DEMAIS_SER);
            name = "header_read";
            break;
        case pup_error::header_magic:
            error = i18n::tr(str::FW_NAO_ARQUIVO_ATUALIZACAO_PS3_PUP);
            name = "header_magic";
            break;
        case pup_error::expected_size:
            error = i18n::tr(str::FW_ARQUIVO_INCOMPLETO_BAIXE_PS3UPDAT_PUP);
            name = "expected_size";
            break;
        case pup_error::hash_mismatch:
            error = i18n::tr(str::FW_CONTEUDO_CORROMPIDO_VERIFICACAO_FALHOU_BAIXE);
            name = "hash_mismatch";
            break;
        default:
            error = i18n::tr(str::FW_ARQUIVO_CORROMPIDO_BAIXE_PS3UPDAT_PUP);
            name = status == pup_error::stream ? "stream" : status == pup_error::file_entries ? "file_entries" : "header_file_count";
            break;
        }
        // A drive pulled out during the check reads short, which pup_object reports as a size or hash error: the file
        // is fine, so it must not be called corrupted.
        const bool unplugged = usb_drive_of(path) >= 0 && !fs::is_file(path);
        if (unplugged) error = i18n::tr(str::COM_PENDRIVE_FOI_DESCONECTADO_DURANTE_VERIFICACAO);
        note(fmt::format("firmware: '%s' rejected by the pup_object checks: %s%s%s%s", path, name, detail.empty() ? "" : ": ", detail,
            unplugged ? " (the USB drive is gone)" : ""));
        return {};
    }
    auto result = std::make_unique<package>();
    result->path = path;
    result->size = size;
    if (const std::size_t i = tables.find(update_files_entry); i < tables.files.size())
    {
        result->update_files_offset = tables.files[i].data_offset;
        result->update_files_size = tables.files[i].data_length;
        std::memcpy(result->update_files_hash.data(), tables.hashes[i].hash, result->update_files_hash.size());
    }
    if (!result->update_files_size)
    {
        error = i18n::tr(str::FW_CONTEUDO_CORROMPIDO_SEM_PACOTES_INSTALACAO);
        note(fmt::format("firmware: '%s' rejected: no update_files.tar (entry 0x300)", path));
        return {};
    }
    // As main_window.cpp: the first line of entry 0x100 (a few bytes, e.g. "4.92"). validate() checked its bounds.
    if (const std::size_t i = tables.find(version_entry); i < tables.files.size())
    {
        std::string text(static_cast<std::size_t>(std::min<u64>(tables.files[i].data_length, 64)), '\0');
        if (file.read_at(tables.files[i].data_offset, text.data(), text.size()) == text.size()) result->version = std::move(text);
    }
    if (const std::size_t end = result->version.find_first_of("\r\n"); end != std::string::npos) result->version.erase(end);
    if (result->version.empty() || result->version.size() > 16 || result->version.find('\0') != std::string::npos)
    {
        // That read is the last one of the check: a drive pulled out just then is not a corrupted file either.
        const bool unplugged = usb_drive_of(path) >= 0 && !fs::is_file(path);
        error = i18n::tr(unplugged ? str::COM_PENDRIVE_FOI_DESCONECTADO_DURANTE_VERIFICACAO : str::FW_CONTEUDO_CORROMPIDO_SEM_VERSAO_FIRMWARE);
        note(fmt::format("firmware: '%s' rejected: no usable version text (entry 0x100)%s", path, unplugged ? " (the USB drive is gone)" : ""));
        return {};
    }
    result->file = std::move(file);
    return result;
}

// Free space the install wants on /data: the copy of update_files.tar, the extracted dev_flash (somewhat larger than
// update_files.tar: zlib packages) and a margin. The previous firmware keeps the space it already uses: it stays in
// <dev_flash>.old until a later start has checked the new one.
inline u64 space_needed(u64 update_files_size)
{
    return update_files_size / 2 * 5 + (64ull << 20);
}

// False with the reason for the XMB line when /data is too full. Unknown free space (statfs refused) lets the install
// go on: it is staged and every file is checked, so a full disk only fails it.
inline bool enough_space(u64 update_files_size, std::string& reason)
{
    const u64 need = space_needed(update_files_size);
    fs::device_stat stat{};
    if (!fs::statfs(std::string(data_root) + '/', stat))
    {
        note(fmt::format("firmware: /data free space unknown (statfs: %s); going on, the install is staged and checked", fs::g_tls_error));
        return true;
    }
    if (stat.avail_free >= need) return true;
    reason = i18n::trf(i18n::str::FW_POUCO_ESPACO_DATA_PRECISA_MIB, {need >> 20, stat.avail_free >> 20});
    note(fmt::format("firmware: not enough space on /data (%llu MiB free, %llu MiB needed)", stat.avail_free >> 20, need >> 20));
    return false;
}

// ---------------------------------------------------------------------------------------------------------------
// Jobs (one at a time, only on the 'PS4 Firmware Installer' thread; without that thread the XMB does not start them)

enum class task : u8
{
    check,   // find the PUP and validate it (reads the whole file)
    install, // extract the confirmed PUP and swap dev_flash
    cleanup, // remove what a failed or interrupted install left behind
};

enum class stage : u32
{
    idle,
    searching,
    verifying,
    reading,      // update_files.tar copied to <dev_flash>.installing and hashed ('mib' of 'mib_total')
    extracting,   // package 'step' of 'steps'
    flushing,     // its files checked and flushed to the disk (fsync), then the folders of the new tree
    swapping,     // from here on job::stop is not looked at: the swap finishes
    removing_staging,
    cleaning,
};

// An install still stops for job::stop (the XMB asks before it sets it).
inline bool can_cancel(stage value)
{
    return value < stage::swapping;
}

struct job
{
    task kind = task::check;
    std::string target;            // target_folder()
    std::unique_ptr<package> pup;  // install: the confirmed PUP; check: the PUP found (on success)
    std::atomic<bool> stop{false}; // X on the row during an install, or the XMB closing (also ends a check)
    std::atomic<bool> done{false}; // the results below are final (release / acquire)
    std::atomic<u32> phase{0};
    std::atomic<u32> step{0}, steps{0};
    std::atomic<u32> mib{0}, mib_total{0};

    bool ok = false;
    bool cancelled = false;
    u32 invalid = 0;     // check: PUPs found earlier in the search order that failed
    std::string text;    // XMB line (failures, install result)
    std::string version; // install: version.txt of the firmware now installed

    // Sequentially consistent with job::stop: when the UI sets stop and the install, having set 'swapping', still read
    // it as false, the UI's next read of the stage sees 'swapping', so the XMB never shows a cancel that was not taken.
    void set(stage value) { phase.store(static_cast<u32>(value)); }
    stage current() const { return static_cast<stage>(phase.load()); }
    // The file being checked, for the progress line (written by the job, read by the UI).
    std::string shown() const
    {
        std::lock_guard lock(m_shown_mutex);
        return m_shown;
    }
    void show(std::string value)
    {
        std::lock_guard lock(m_shown_mutex);
        m_shown = std::move(value);
    }

private:
    mutable std::mutex m_shown_mutex;
    std::string m_shown;
};

inline void check(job& j)
{
    j.set(stage::searching);
    const std::vector<std::string> files = find_files();
    std::string list;
    for (const std::string& path : files) list += (list.empty() ? "" : ", ") + path;
    note(fmt::format("firmware: %u PS3UPDAT.PUP found%s%s", files.size(), files.empty() ? " (USB:/PS3/UPDATE, USB:/, /data/rpcs3, /data/rpcs3/import)" : ": ", list));
    if (files.empty())
    {
        j.text = i18n::tr(i18n::str::FW_NENHUM_PS3UPDAT_PUP_COPIE_PENDRIVE);
        return;
    }
    std::string first_error;
    for (const std::string& path : files)
    {
        if (j.stop.load())
        {
            j.cancelled = true;
            return;
        }
        j.show(source_label(path));
        j.set(stage::verifying);
        std::string error;
        bool stopped = false;
        std::unique_ptr<package> found = verify(path, error, j.stop, stopped);
        if (stopped)
        {
            j.cancelled = true;
            return;
        }
        if (!found)
        {
            if (!j.invalid++) first_error = source_label(path) + ": " + error;
            continue;
        }
        note(fmt::format("firmware: '%s' is a valid PS3 firmware %s PUP (%llu bytes; update_files.tar %llu bytes)", path, found->version,
            found->size, found->update_files_size));
        if (std::string reason; !enough_space(found->update_files_size, reason))
        {
            j.text = i18n::trf(i18n::str::FW_NOT_INSTALLED_REASON, {i18n::raw(reason)});
            return;
        }
        j.pup = std::move(found);
        j.ok = true;
        return;
    }
    j.text = clip(first_error + (j.invalid > 1 ? i18n::trf(i18n::str::FW_MAIS_INVALIDO, {j.invalid - 1}) : std::string{}), line_bytes());
}

enum class flush_result : u8
{
    ok,
    mismatch, // missing, or not the size its TAR header gives
    unsynced, // fsync failed
    stopped,  // job::stop
};

// Every file of 'tar' extracted under 'root': present with the size its header gives (TAR.cpp ignores write results)
// and flushed to the disk (TAR.cpp closes its files without fsync). One open per archive entry; job::stop is looked
// at before each. 'folders' collects every folder below 'root' that the entries live in, for sync_folder();
// 'flushed' counts the files, 'bad' names the first one that fails.
inline flush_result flush_extracted(tar_object& tar, const std::string& root, std::set<std::string>& folders, std::string& bad,
    const std::atomic<bool>& stop, u64& flushed)
{
    // The folder and its parents below 'root', each one once.
    const auto add_folder = [&](std::string folder)
    {
        while (folder.size() > root.size() && folders.insert(folder).second) folder.resize(folder.find_last_of('/'));
    };
    for (const std::string& name : tar.get_filenames())
    {
        if (stop.load()) return flush_result::stopped;
        // TAR.cpp skips these names (savestate compatibility); firmware archives have none.
        if (name.find(reinterpret_cast<const char*>(u8"＄")) != std::string::npos) continue;
        const std::string path = normalize(root + '/' + name);
        fs::stat_t stat{};
        if (!fs::get_stat(path, stat))
        {
            bad = name + " (missing)";
            return flush_result::mismatch;
        }
        if (stat.is_directory)
        {
            add_folder(path);
            continue;
        }
        add_folder(path.substr(0, path.find_last_of('/')));
        const std::unique_ptr<::utils::serial> entry = tar.get_file(name);
        const u64 expected = entry ? entry->get_size(umax) - entry->pos : umax;
        // Opened for writing, though nothing is written: the same kind of handle as fs::pending_file's, whose fsync
        // the configuration saves on the console already depend on.
        const fs::file file(path, fs::read + fs::write);
        if (!file)
        {
            bad = fmt::format("%s (cannot be opened: %s)", name, fs::g_tls_error);
            return flush_result::mismatch;
        }
        if (const u64 size = file.size(); size != expected)
        {
            bad = fmt::format("%s (%llu of %llu bytes)", name, size, expected);
            return flush_result::mismatch;
        }
        // unix_file::sync() reports a failed fsync (or an earlier failed write on this handle) through g_tls_error.
        fs::g_tls_error = fs::error::ok;
        file.sync();
        if (fs::g_tls_error != fs::error::ok)
        {
            bad = fmt::format("%s (fsync: %s)", name, fs::g_tls_error);
            return flush_result::unsynced;
        }
        ++flushed;
    }
    return flush_result::ok;
}

inline void install(job& j)
{
    using i18n::str;
    const auto started = std::chrono::steady_clock::now();
    if (!j.pup || j.target.empty())
    {
        j.text = i18n::tr(str::FW_FIRMWARE_NAO_INSTALADO_NENHUM_PS3UPDAT);
        return;
    }
    package& pup = *j.pup;
    const std::string staging = staging_folder(j.target);
    const std::string old = previous_folder(j.target);
    const std::string fresh = staging + "/dev_flash"; // the archives name their files dev_flash/...
    bool staged = false;
    const auto remove_staging = [&]
    {
        if (staged && !remove_leftover(j.target, staging))
            note(fmt::format("firmware: cannot remove '%s' (%s); removed at the next XMB", staging, fs::g_tls_error));
    };
    // The XMB line is cut at ~116 bytes, so the outcome and the firmware left come first and only the end of a long
    // reason can be lost.
    const auto fail_with = [&](const std::string& text, const std::string& detail)
    {
        remove_staging();
        j.text = clip(text, line_bytes());
        note(fmt::format("firmware: install of %s from '%s' FAILED: %s; version.txt in '%s' now reads '%s'", pup.version, pup.path, detail,
            j.target, version_of(j.target)));
    };
    // Every failure before the first rename, or undone after it: <target> is as it was.
    const auto fail = [&](const std::string& reason, const std::string& detail)
    {
        fail_with(i18n::trf(str::FW_NOT_INSTALLED_STAYS, {i18n::raw(which(j.target)), i18n::raw(reason)}), detail);
    };
    // dev_flash itself is missing or empty and the previous firmware still sits in <target>.old (recover() is tried
    // again by the XMB at once and at every open).
    const auto fail_in_old = [&](const std::string& detail)
    {
        fail_with(i18n::trf(str::FW_NOT_INSTALLED_OLD_STUCK, {old}), detail);
    };
    const std::string unplugged_hint = usb_drive_of(pup.path) >= 0 ? i18n::tr(str::FW_PENDRIVE_DESCONECTADO) : "";
    const auto cancelled = [&]
    {
        remove_staging();
        j.cancelled = true;
        j.text = clip(i18n::trf(str::FW_CANCELLED_STAYS, {i18n::raw(which(j.target))}), line_bytes());
        note(fmt::format("firmware: install of %s cancelled; '%s' is unchanged", pup.version, j.target));
    };

    if (std::string reason; !enough_space(pup.update_files_size, reason)) return fail(reason, "not enough space on /data");
    if (!pup.file) return fail(i18n::tr(str::FW_NENHUM_PS3UPDAT_PUP_VERIFICADO), "the checked PUP is not open");
    if (fs::exists(staging) && !remove_leftover(j.target, staging))
        return fail(i18n::trf(str::FW_CANNOT_CLEAN, {staging}), fmt::format("cannot remove the leftover '%s' (%s)", staging, fs::g_tls_error));
    if (!fs::create_path(staging)) return fail(i18n::trf(str::FW_CANNOT_CREATE, {staging}), fmt::format("cannot create '%s' (%s)", staging, fs::g_tls_error));
    staged = true;

    // update_files.tar is copied to /data in pieces and hashed from the very bytes written, so a drive pulled out or a
    // PUP changed since the check ends here, and everything below reads the verified copy, not the USB drive.
    const std::string archive = staging + "/update_files.tar"; // the packages' entries all start with dev_flash/
    j.mib_total.store(static_cast<u32>(pup.update_files_size >> 20));
    j.set(stage::reading);
    {
        const fs::file copy(archive, fs::rewrite);
        if (!copy) return fail(i18n::tr(str::FW_NAO_FOI_POSSIVEL_CRIAR_COPIA), fmt::format("cannot create '%s' (%s)", archive, fs::g_tls_error));
        std::array<u8, 20> digest{};
        switch (hmac_range(pup.file, pup.update_files_offset, pup.update_files_size, digest, j.stop, &copy, &j.mib))
        {
        case stream_result::ok:
            break;
        case stream_result::stopped:
            return cancelled();
        case stream_result::short_read:
            return fail(i18n::tr(str::FW_PS3UPDAT_PUP_NAO_PODE) + unplugged_hint, fmt::format("reading update_files.tar from the PUP failed (%s)%s",
                fs::g_tls_error, fs::is_file(pup.path) ? "" : "; the file is gone"));
        case stream_result::short_write:
            return fail(i18n::tr(str::FW_ERRO_GRAVAR_DATA_CHEIO), fmt::format("writing '%s' failed (%s)", archive, fs::g_tls_error));
        }
        if (digest != pup.update_files_hash)
            return fail(i18n::tr(str::FW_PS3UPDAT_PUP_MUDOU_DESDE), "update_files.tar HMAC mismatch while copying it to /data");
    }
    // The PUP (maybe on a USB drive) is not read again.
    pup.file.close();
    const fs::file update_files_f(archive);
    if (!update_files_f || update_files_f.size() != pup.update_files_size)
        return fail(i18n::tr(str::FW_COPIA_PS3UPDAT_PUP_DATA_FICOU), fmt::format("'%s' does not read back with %llu bytes (%s)", archive,
            pup.update_files_size, fs::g_tls_error));

    tar_object update_files(update_files_f);
    std::vector<std::string> names = update_files.get_filenames();
    std::erase_if(names, [](const std::string& name) { return name.find("dev_flash_") == std::string::npos; });
    if (names.empty()) return fail(i18n::tr(str::FW_PUP_NAO_TEM_PACOTES_DEV), "no dev_flash_* package in update_files.tar");

    j.steps.store(static_cast<u32>(names.size()));
    u64 files_written = 0, files_flushed = 0;
    std::set<std::string> folders{fresh};
    for (std::size_t i = 0; i < names.size(); ++i)
    {
        if (j.stop.load()) return cancelled();
        const std::string& name = names[i];
        j.step.store(static_cast<u32>(i + 1));
        j.set(stage::extracting);
        // The package names (~40 characters) go to startup.log; the XMB line counts them.
        const std::size_t number = i + 1, count = names.size();
        fs::file dev_flash_tar_f;
        {
            const std::unique_ptr<::utils::serial> stream = update_files.get_file(name);
            if (!stream) return fail(i18n::trf(str::FW_PACKAGE_UNREADABLE, {number, count}), fmt::format("package '%s' cannot be opened in update_files.tar", name));
            // As the PC: read the whole entry, then decrypt it. The package bytes are freed before the extraction.
            if (stream->m_file_handler) stream->m_file_handler->handle_file_op(*stream, 0, stream->get_size(umax), nullptr);
            const fs::file package_f = fs::make_stream(std::move(stream->data));
            std::string error;
            dev_flash_tar_f = decrypt_package(package_f, error);
            if (!dev_flash_tar_f) return fail(i18n::trf(str::FW_PACKAGE_NOT_DECRYPTED, {number, count}), fmt::format("'%s': %s", name, error));
        }
        tar_object dev_flash_tar(dev_flash_tar_f);
        if (!dev_flash_tar.extract(staging, false))
            return fail(i18n::tr(str::FW_ERRO_GRAVAR_DATA_VEJA), fmt::format("TAR extraction of '%s' into '%s' failed (see RPCS3.log)", name, staging));
        // Sizes checked and every file flushed before the next package (or the swap) counts it as written.
        j.set(stage::flushing);
        std::string bad;
        switch (flush_extracted(dev_flash_tar, staging, folders, bad, j.stop, files_flushed))
        {
        case flush_result::ok:
            break;
        case flush_result::stopped:
            return cancelled();
        case flush_result::mismatch:
            return fail(i18n::tr(str::FW_ARQUIVO_GRAVADO_INCOMPLETO_DATA_CHEIO), fmt::format("'%s': extracted file does not match the archive: %s", name, bad));
        case flush_result::unsynced:
            return fail(i18n::tr(str::FW_ERRO_CONFIRMAR_GRAVACAO_DATA_VEJA), fmt::format("'%s': extracted file not flushed: %s", name, bad));
        }
        files_written += dev_flash_tar.get_filenames().size();
    }
    // The folder entries of the new tree, children before their parents (reverse order of the paths). A folder that
    // cannot be flushed is counted and tolerated: its files are, and the check at the next start covers the rest.
    u32 folders_unsynced = 0;
    for (auto it = folders.rbegin(); it != folders.rend(); ++it)
    {
        if (j.stop.load()) return cancelled();
        if (!sync_folder(*it)) ++folders_unsynced;
    }
    if (!sync_folder(staging)) ++folders_unsynced;
    // What the boot needs and what the check at the next start looks for (complete()).
    std::string version, problem;
    if (!complete(fresh, {}, version, problem))
        return fail(i18n::tr(str::FW_FALTAM_ARQUIVOS_ESSENCIAIS_FIRMWARE_EXTRAIDO), fmt::format("'%s': %s", fresh, problem));

    // The last point where job::stop ends the install. The stage is set first (see job::set).
    j.set(stage::swapping);
    if (j.stop.load()) return cancelled();
    // A <target>.old next to a missing or empty <target> still holds the previous firmware (recover() could not put it
    // back at the XMB): try again, and never remove it in that state.
    if (const recovery restored = recover(j.target); !restored.log.empty()) note(restored.log);
    if (fs::is_dir(old))
    {
        if (!filled(j.target)) return fail_in_old(fmt::format("'%s' could not be restored as '%s'", old, j.target));
        // Next to a firmware that passed the check at a start (verified) .old is a leftover. Otherwise it is the way
        // back of an install not checked yet: the firmware in <target> takes that place only if it passes the same
        // check, and the marker says verified before .old is removed, so a removal cut short is never put back.
        const marker before = read_marker(j.target);
        if (before.state != install_state::verified)
        {
            std::string current, why;
            // Nothing changes; the check at the next start puts .old back.
            if (!complete(j.target, before.state == install_state::pending ? before.version : std::string{}, current, why))
                return fail_with(i18n::tr(str::FW_FIRMWARE_NAO_INSTALADO_DEV_FLASH),
                    fmt::format("'%s' fails the check (%s) and '%s' holds the firmware before it", j.target, why, old));
            if (!write_marker(j.target, install_state::verified, current))
                return fail(i18n::tr(str::COM_NAO_FOI_POSSIVEL_GRAVAR_ARQUIVO), "the marker cannot be written");
        }
        if (!remove_leftover(j.target, old))
            return fail(i18n::trf(str::FW_CANNOT_DELETE, {old}), fmt::format("cannot remove the leftover '%s' (%s)", old, fs::g_tls_error));
        // Its removal reaches the disk before the marker below: a .old cut short in its removal, still there next to
        // a marker naming the new version, would be taken at the next start for the way back of a swap that never
        // happened, and put in place of the firmware in <target>.
        sync_folder(fs::get_parent_dir(j.target));
    }
    // An empty folder stands for "no firmware", so that the swap always leaves a <target>.old to go back to.
    if (!fs::is_dir(j.target) && !fs::create_dir(j.target))
        return fail(i18n::trf(str::FW_CANNOT_CREATE, {j.target}), fmt::format("cannot create '%s' (%s)", j.target, fs::g_tls_error));
    // The marker names the version swapped in. From here until a later start checks it, .old is the way back.
    if (!write_marker(j.target, install_state::pending, version))
        return fail(i18n::tr(str::COM_NAO_FOI_POSSIVEL_GRAVAR_ARQUIVO), "the marker cannot be written");
    if (!fs::rename(j.target, old, false))
    {
        const std::string detail = fmt::format("rename '%s' -> '%s' failed (%s)", j.target, old, fs::g_tls_error);
        write_marker(j.target, install_state::failed, version);
        return fail(i18n::tr(str::FW_NAO_FOI_POSSIVEL_MOVER), detail);
    }
    if (!fs::rename(fresh, j.target, false))
    {
        const std::string detail = fmt::format("rename '%s' -> '%s' failed (%s)", fresh, j.target, fs::g_tls_error);
        if (!fs::rename(old, j.target, false))
        {
            // The marker stays pending: recover() puts .old back and records it.
            note(fmt::format("firmware: cannot move '%s' back to '%s' (%s)", old, j.target, fs::g_tls_error));
            return fail_in_old(detail);
        }
        sync_folder(fs::get_parent_dir(j.target));
        write_marker(j.target, install_state::failed, version);
        return fail(i18n::tr(str::FW_NAO_FOI_POSSIVEL_COLOCAR_NOVO), detail);
    }
    // Both renames on the disk.
    const bool swap_synced = sync_folder(fs::get_parent_dir(j.target));
    sync_folder(staging);

    // The new firmware is in place and the previous one stays in .old; what follows only frees space.
    j.set(stage::removing_staging);
    if (!remove_leftover(j.target, staging))
        note(fmt::format("firmware: cannot remove '%s' (%s); removed at the next XMB", staging, fs::g_tls_error));

    j.version = ::utils::get_firmware_version();
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    note(fmt::format("firmware: PS3 firmware %s installed from '%s' into '%s' (%u dev_flash packages, %llu entries, %llu files flushed, "
        "%u of %u folders not flushed, swap %s, %.1f s); vsh/etc/version.txt reads '%s'; the previous firmware stays in '%s' until a later start "
        "checks this one", pup.version, pup.path, j.target, names.size(), files_written, files_flushed, folders_unsynced, folders.size() + 1,
        swap_synced ? "flushed" : "not flushed", seconds, j.version, old));
    j.ok = true;
    j.text = clip(i18n::trf(str::FW_FIRMWARE_INSTALADO_JOGOS_JA_PODEM, {j.version.empty() ? version : j.version}), line_bytes());
}

inline void cleanup(job& j)
{
    j.set(stage::cleaning);
    const std::string staging = staging_folder(j.target);
    const std::string old = previous_folder(j.target);
    for (const std::string& path : {staging, old})
    {
        if (!fs::is_dir(path)) continue;
        // .old goes only once the firmware next to it passed the check at a later start (has_leftovers()).
        if (path == old && !(filled(j.target) && read_marker(j.target).state == install_state::verified)) continue;
        if (remove_leftover(j.target, path)) note(fmt::format("firmware: removed '%s' left by an earlier install", path));
        else note(fmt::format("firmware: cannot remove '%s' left by an earlier install (%s)", path, fs::g_tls_error));
    }
    j.ok = true;
}

// ---------------------------------------------------------------------------------------------------------------
// Check at a later start

struct start_report
{
    std::string log;  // startup.log line, "" when there was nothing to check
    std::string text; // XMB line, "" when there is nothing to tell
};

// The firmware in <target> ('label': its version, or a word) failed the check at start: it goes to a free rejectedN in
// <target>.installing, which the cleanup job removes, and <target>.old takes its place. When /data is too full for
// that folder, <target> itself becomes <target>.installing (a rename needs no space). A rename that fails puts the
// folder already moved back, so <target> is never left empty by this step alone.
inline start_report restore_previous(const std::string& target, const std::string& version, const std::string& problem)
{
    const std::string old = previous_folder(target);
    const std::string staging = staging_folder(target);
    const std::string label = version.empty() ? std::string(i18n::tr(i18n::str::FW_LABEL_INSTALADO)) : version;
    const std::string head = fmt::format("firmware: '%s' (%s) failed the check at start: %s", target, version.empty() ? "installed" : version, problem);
    const std::string not_back = clip(i18n::trf(i18n::str::FW_FIRMWARE_FALHOU_CONFERENCIA_ABRIR_ANTERIOR, {label}), line_bytes());
    // Not <target>.installing itself when it can be made: a power cut in the middle of that rename can leave
    // <target> under both names on some file systems, and the next attempt could not move <target> into itself.
    std::string aside;
    if (fs::create_path(staging))
    {
        for (u32 i = 0; i < 16 && aside.empty(); ++i)
            if (std::string path = fmt::format("%s/rejected%u", staging, i); !fs::exists(path)) aside = std::move(path);
    }
    else if (!fs::exists(staging))
    {
        aside = staging;
    }
    if (aside.empty() || !fs::rename(target, aside, false))
        return {fmt::format("%s; it cannot be moved into '%s' (%s), so '%s' is kept and the check runs again at the next start", head, staging,
            fs::g_tls_error, old), not_back};
    if (!fs::rename(old, target, false))
    {
        const std::string error = fmt::format("%s", fs::g_tls_error);
        if (fs::rename(aside, target, false))
            return {fmt::format("%s; '%s' cannot be put back (%s): the checked firmware stays and '%s' is kept for the next start", head, old, error, old),
                not_back};
        return {fmt::format("%s; neither '%s' (%s) nor the checked firmware (%s) went back to '%s'; recover() puts '%s' back at the next XMB",
                    head, old, error, fs::g_tls_error, target, old),
            clip(i18n::trf(i18n::str::FW_OUT_OF_PLACE, {old}), line_bytes())};
    }
    sync_folder(fs::get_parent_dir(target));
    sync_folder(staging);
    write_marker(target, install_state::restored, version);
    return {fmt::format("%s; the previous firmware was put back from '%s' (version.txt now reads '%s')", head, old, version_of(target)),
        clip(i18n::trf(i18n::str::FW_FIRMWARE_FALHOU_CONFERENCIA_ABRIR_ANTERIOR_2, {label, i18n::raw(which(target))}), line_bytes())};
}

// The firmware an install left next to <target>.old is checked with complete(): passed, the marker says verified and
// the cleanup job removes .old; failed, restore_previous().
inline start_report check_swap_at_start(const std::string& target)
{
    const std::string old = previous_folder(target);
    const marker state = read_marker(target);
    std::string version, problem;
    if (!fs::is_dir(old))
    {
        // A pending marker without .old: the install stopped between the marker and its first rename (application
        // closed), or a failed swap was undone and the 'failed' marker could not be written. <target> was not replaced.
        if (state.state != install_state::pending) return {};
        if (complete(target, state.version, version, problem))
        {
            write_marker(target, install_state::verified, version);
            return {fmt::format("firmware: %s in '%s' passed the check at start (no previous firmware to remove)", version, target), {}};
        }
        write_marker(target, install_state::failed, state.version);
        return {fmt::format("firmware: the install of %s did not finish; '%s' was not replaced (%s)", state.version, target, problem),
            clip(i18n::trf(i18n::str::FW_INSTALACAO_FIRMWARE_NAO_TERMINOU_FICA, {state.version, i18n::raw(which(target))}), line_bytes())};
    }
    // .old next to a missing or empty <target>: recover() could not put it back, and its line says so.
    if (!filled(target)) return {};
    // Only waiting for the cleanup job.
    if (state.state == install_state::verified) return {};
    // Without a pending marker (none, or one from an earlier install) the version is not compared; the files are.
    const std::string expected = state.state == install_state::pending ? state.version : std::string{};
    if (!complete(target, expected, version, problem)) return restore_previous(target, !expected.empty() ? expected : version, problem);
    if (!write_marker(target, install_state::verified, version))
        return {fmt::format("firmware: %s in '%s' passed the check at start, but the marker cannot be written; '%s' is kept until the next start",
            version, target, old), {}};
    return {fmt::format("firmware: %s in '%s' passed the check at start; the previous firmware in '%s' is removed in the background", version,
                target, old),
        clip(i18n::trf(i18n::str::FW_FIRMWARE_CONFERIDO_NESTA_ABERTURA_COPIA, {version, old}), line_bytes())};
}

// Run once per start, at the first XMB (so always a later start than any install), after recover(), on the UI thread:
// one marker, one version.txt, a few stats and at most a few renames. check_swap_at_start(); otherwise an install cut
// short before its swap (application closed, power cut) is named once: its copy of update_files.tar is still in
// <target>.installing, which the cleanup job then removes. <target> was not touched by it. 'recovered': recover()
// acted at this start and its own line already says which firmware is left.
inline start_report check_at_start(const std::string& target, bool recovered = false)
{
    if (target.empty()) return {};
    start_report report = check_swap_at_start(target);
    if (report.text.empty() && !recovered && fs::is_file(staging_folder(target) + "/update_files.tar"))
    {
        report.text = i18n::trf(i18n::str::FW_INTERRUPTED_BEFORE_SWAP, {i18n::raw(which(target))});
        report.log += (report.log.empty() ? "" : "; ") +
            fmt::format("firmware: an install was cut short before its swap; '%s' is unchanged (%s), its leftovers are removed", target, which_log(target));
    }
    return report;
}

inline void run(job& j)
{
    switch (j.kind)
    {
    case task::check: check(j); break;
    case task::install: install(j); break;
    case task::cleanup: cleanup(j); break;
    }
    j.done.store(true, std::memory_order_release);
}
} // namespace utils::ps4::firmware
