#pragma once

// PS3 package checks of the PS4 launcher that need no file access (host-tested by ps4-port/tests/pkg_check_test.cpp):
// what the first 0x30 bytes and the size of a PKG say before package_reader reads it, the reason of every refusal
// in the interface language (ps4_i18n_core.inc, PKG_*), and the version texts of an update refused by its
// TARGET_APP_VER. Log texts stay English.

#include "ps4_i18n.hpp"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

namespace utils::ps4::pkg
{
// PKGHeader up to data_size (unpkg.h): magic 0, pkg_type 4, platform 6, pkg_size 0x18, data_offset 0x20, data_size 0x28.
inline constexpr std::size_t header_size = 0x30;
// A part written within this many seconds is still being copied (FTP writes refresh the mtime).
inline constexpr std::int64_t copying_seconds = 120;
inline constexpr std::uint32_t magic_ps3 = 0x7F504B47; // "\x7FPKG": PS3, PSP and PS Vita
inline constexpr std::uint32_t magic_ps4 = 0x7F434E54; // "\x7FCNT"
inline constexpr std::uint16_t platform_ps3 = 1;
inline constexpr std::uint16_t platform_psp_vita = 2;

// A table text where a std::string is expected: resolved in the interface language when it is converted
struct ui_text
{
    i18n::str id;
    operator std::string() const { return i18n::tr(id); }
    const char* c_str() const noexcept { return i18n::tr(id); }
};

inline constexpr ui_text open_failed_text{i18n::str::PKG_NAO_FOI_POSSIVEL_ABRIR_PKG};
inline constexpr ui_text ps4_text{i18n::str::PKG_PKG_PS4_INSTALE_GOLDHEN_NAO};
inline constexpr ui_text psp_vita_text{i18n::str::PKG_PKG_PSP_PS_VITA_ESTE};
inline constexpr ui_text unreadable_text{i18n::str::PKG_PKG_CORROMPIDO_TIPO_DESCONHECIDO_NAO};
inline constexpr char unreadable_log[] = "unreadable package (size fine; header or item table failed, see 'PKG:' lines)";

enum class magic_kind
{
    short_read, // fewer than header_size bytes
    ps3,        // PS3/PSP/PS Vita magic
    ps4,
    other,
};

struct header_info
{
    magic_kind kind = magic_kind::short_read;
    std::size_t bytes = 0;     // bytes that were available
    std::uint32_t magic = 0;   // first four bytes, read big-endian
    std::uint16_t platform = 0;
    std::uint64_t pkg_size = 0, data_offset = 0, data_size = 0;
};

namespace detail
{
inline std::uint64_t be(const unsigned char* p, unsigned bytes)
{
    std::uint64_t value = 0;
    for (unsigned i = 0; i < bytes; ++i) value = value << 8 | p[i];
    return value;
}

[[gnu::format(printf, 1, 2)]] inline std::string format(const char* pattern, ...)
{
    char text[512];
    va_list args;
    va_start(args, pattern);
    const int n = std::vsnprintf(text, sizeof(text), pattern, args);
    va_end(args);
    return n <= 0 ? std::string() : std::string(text, static_cast<std::size_t>(n) < sizeof(text) ? static_cast<std::size_t>(n) : sizeof(text) - 1);
}
}

inline header_info parse_header(const unsigned char* data, std::size_t size)
{
    header_info h;
    h.bytes = data ? size : 0;
    if (h.bytes < header_size) return h;
    h.magic = static_cast<std::uint32_t>(detail::be(data, 4));
    h.kind = h.magic == magic_ps3 ? magic_kind::ps3 : h.magic == magic_ps4 ? magic_kind::ps4 : magic_kind::other;
    h.platform = static_cast<std::uint16_t>(detail::be(data + 6, 2));
    h.pkg_size = detail::be(data + 0x18, 8);
    h.data_offset = detail::be(data + 0x20, 8);
    h.data_size = detail::be(data + 0x28, 8);
    return h;
}

// Version strings as unpkg.cpp compares them: std::strtod over the whole text ("01.12" -> 1.12); empty or trailing
// characters -> nullopt.
inline std::optional<double> version_value(std::string_view text)
{
    if (text.empty() || text.size() > 32) return std::nullopt;
    const std::string copy(text);
    char* end = nullptr;
    const double value = std::strtod(copy.c_str(), &end);
    if (end != copy.c_str() + copy.size()) return std::nullopt;
    return value;
}

// -1, 0 or 1 like a <=> b; nullopt when either version is unreadable.
inline std::optional<int> compare_versions(std::string_view a, std::string_view b)
{
    const auto x = version_value(a), y = version_value(b);
    if (!x || !y) return std::nullopt;
    return *x < *y ? -1 : *x > *y ? 1 : 0;
}

// "2,8 GB", "640 MB", "12 KB", "7 bytes" (decimal units; pt-BR comma, '.' in the other languages).
inline std::string size_text(std::uint64_t bytes)
{
    std::string text = bytes >= 1'000'000'000 ? detail::format("%.1f GB", static_cast<double>(bytes) / 1e9)
        : bytes >= 1'000'000 ? detail::format("%llu MB", static_cast<unsigned long long>((bytes + 500'000) / 1'000'000))
        : bytes >= 1'000 ? detail::format("%llu KB", static_cast<unsigned long long>((bytes + 500) / 1'000))
        : detail::format("%llu bytes", static_cast<unsigned long long>(bytes));
    if (i18n::current() == i18n::lang::pt_br)
        for (char& c : text)
            if (c == '.') c = ',';
    return text;
}

// "1,2 GB de 3,0 GB"; in MB when both round to the same text, so a file that is not complete never reads as full.
inline std::string sizes_text(std::uint64_t have, std::uint64_t need)
{
    std::string a = size_text(have), b = size_text(need);
    if (a == b && have != need && need >= 1'000'000)
    {
        a = detail::format("%llu MB", static_cast<unsigned long long>(have / 1'000'000));
        b = detail::format("%llu MB", static_cast<unsigned long long>((need + 999'999) / 1'000'000));
    }
    return i18n::trf(i18n::str::PKG_SIZES_OF, {a, b});
}

// Part 'index' of a split package as unpkg.cpp names it: the first part's path without "_00.pkg", "_%02d", and the
// first part's extension in its own spelling. Empty when 'first' is too short to be a part name.
inline std::string part_name(std::string_view first, unsigned index)
{
    if (first.size() < 7) return {};
    return std::string(first.substr(0, first.size() - 7)) + detail::format("_%02u", index) + std::string(first.substr(first.size() - 4));
}

inline std::string_view file_name(std::string_view path)
{
    const auto slash = path.find_last_of('/');
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

// What the launcher found on disk for one package.
struct file_facts
{
    std::uint64_t have = 0;        // bytes of the file, plus every following part of a split package (up to the first missing)
    // since the newest write of any part; a small negative value (clock skew) counts as writing, a timestamp more than
    // copying_seconds in the future (a USB drive's local-time stamps) does not
    std::int64_t idle_seconds = 0;
    bool split = false;            // a "_00.pkg" first part whose header asks for more than its own size
    std::string missing_part;      // file name of the first part that is absent or empty
};

enum class state
{
    ready,        // complete PS3 package (package_reader may read it)
    ps4,          // PS4 package
    psp_vita,     // PSP or PS Vita package
    empty,        // fewer than header_size bytes
    not_package,  // wrong magic
    copying,      // smaller than its header says, written within copying_seconds
    stopped,      // smaller than its header says, not written for longer
    part_missing, // split package with a part absent or empty, not written for longer
    open_failed,  // the file could not be opened (set by the caller: judge() never returns it)
};

// Short English name for startup.log.
inline const char* state_name(state kind)
{
    switch (kind)
    {
    case state::ready: return "complete";
    case state::ps4: return "PS4 package";
    case state::psp_vita: return "PSP/PS Vita package";
    case state::empty: return "header not written yet";
    case state::not_package: return "not a package";
    case state::copying: return "still copying";
    case state::stopped: return "copy stopped";
    case state::part_missing: return "split part missing";
    case state::open_failed: return "cannot open";
    }
    return "?";
}

struct verdict
{
    state kind = state::ready;
    std::uint64_t have = 0, need = 0;
    std::string text;     // XMB message line (interface language)
    std::string log;      // startup.log reason
    std::string subtitle; // library row (interface language)

    bool ready() const { return kind == state::ready; }
    // Cannot change while the file stays as it is: the library may cache the row.
    bool final() const { return kind == state::ps4 || kind == state::psp_vita; }
};

// The verdict for a package file that could not be opened ('error': the file system's reason).
inline verdict open_failed(std::string_view error)
{
    verdict v;
    v.kind = state::open_failed;
    v.text = open_failed_text;
    v.log = "cannot open (" + std::string(error) + ")";
    v.subtitle = i18n::tr(i18n::str::PKG_PKG_NAO_PODE_SER_ABERTO);
    return v;
}

inline verdict judge(const header_info& h, const file_facts& f)
{
    using i18n::str;
    verdict v;
    v.have = f.have;
    v.need = h.pkg_size;
    const bool writing = f.idle_seconds < copying_seconds && f.idle_seconds > -copying_seconds;
    switch (h.kind)
    {
    case magic_kind::short_read:
        v.kind = state::empty;
        v.text = i18n::tr(writing ? str::PKG_PKG_AINDA_VAZIO_ESPERE_COPIA : str::PKG_PKG_VAZIO_CORTADO_ENVIE_PKG);
        v.log = detail::format("incomplete header (%zu bytes)", h.bytes);
        v.subtitle = i18n::tr(writing ? str::PKG_PKG_AINDA_VAZIO_AGUARDE_COPIA : str::PKG_PKG_VAZIO_CORTADO_ENVIE);
        return v;
    case magic_kind::ps4:
        v.kind = state::ps4;
        v.text = v.subtitle = ps4_text;
        v.log = "PS4 package";
        return v;
    case magic_kind::other:
        v.kind = state::not_package;
        v.text = i18n::tr(str::PKG_NAO_PKG_PS3_CABECALHO_ERRADO);
        // Zeros (a copy whose tail was never written), ciphertext or another format tell different stories
        v.log = detail::format("not a package (magic 0x%08x, pkg_size 0x%llx, data_offset 0x%llx)", h.magic, static_cast<unsigned long long>(h.pkg_size),
            static_cast<unsigned long long>(h.data_offset));
        v.subtitle = i18n::tr(str::PKG_NAO_PKG_PS3_CABECALHO);
        return v;
    case magic_kind::ps3:
        break;
    }
    if (h.platform == platform_psp_vita)
    {
        v.kind = state::psp_vita;
        v.text = v.subtitle = psp_vita_text;
        v.log = "PSP/PS Vita package";
        return v;
    }
    if (f.have >= h.pkg_size) return v;
    const auto have = static_cast<unsigned long long>(f.have), need = static_cast<unsigned long long>(h.pkg_size);
    const auto idle = static_cast<long long>(f.idle_seconds);
    const std::string sizes = sizes_text(f.have, h.pkg_size);
    if (writing)
    {
        v.kind = state::copying;
        v.text = i18n::trf(str::PKG_COPYING_TEXT, {sizes});
        v.log = detail::format("incomplete file, %llu of %llu bytes, last write %lld s ago (still copying)", have, need, idle);
        v.subtitle = i18n::trf(str::PKG_COPYING_SUBTITLE, {sizes});
    }
    else if (f.split && !f.missing_part.empty())
    {
        v.kind = state::part_missing;
        v.text = i18n::trf(str::PKG_PART_MISSING_TEXT, {f.missing_part});
        v.log = detail::format("split package: part '%s' missing (%llu of %llu bytes)", f.missing_part.c_str(), have, need);
        v.subtitle = i18n::trf(str::PKG_PART_MISSING_SUBTITLE, {f.missing_part});
    }
    else
    {
        v.kind = state::stopped;
        v.text = i18n::trf(str::PKG_STOPPED_TEXT, {sizes});
        v.log = detail::format("incomplete file, %llu of %llu bytes, last write %lld s ago (copy stopped)", have, need, idle);
        v.subtitle = i18n::trf(str::PKG_STOPPED_SUBTITLE, {sizes});
    }
    return v;
}

// Reason for an update refused by its version (package_install_result::versions: app_ver, expected =
// TARGET_APP_VER, installed = APP_VER in dev_hdd0/game/<serial>). 'disc': APP_VER of the base game in the library
// ("" when unknown). Used when the installer does not ask (installed data present, a disc's own package list, no screen).
inline std::string version_refusal_text(std::string_view app, std::string_view expected, std::string_view installed, std::string_view disc,
    std::string_view serial)
{
    using i18n::str;
    const std::string a(app), e(expected), i(installed), d(disc), s(serial);
    if (!e.empty() && !i.empty())
    {
        const auto order = compare_versions(i, a);
        if (order ? *order == 0 : i == a) return i18n::trf(str::PKG_UPDATE_ALREADY_INSTALLED, {a, s});
        if (order && *order > 0) return i18n::trf(str::PKG_NEWER_INSTALLED, {i, a});
        return i18n::trf(str::PKG_UPDATE_NEEDS_ORDER, {i, a, e});
    }
    if (!e.empty())
    {
        if (!d.empty()) return i18n::trf(str::PKG_UPDATE_NEEDS_DISC, {a, e, d});
        return i18n::trf(str::PKG_UPDATE_NEEDS_GAME, {a, e, s});
    }
    if (!i.empty()) return i18n::trf(str::PKG_NEWER_INSTALLED, {i, a});
    return i18n::trf(str::PKG_VERSION_UNREADABLE, {s});
}

// A read of the PKG that stayed short after the installer's retries (USB drive gone or stalled, file cut or damaged).
inline constexpr ui_text read_error_text{i18n::str::COM_ERRO_LER_PKG_USB_DESCONECTADO};
inline constexpr char read_error_log[] = "read error (the PKG could not be read; see 'PKG:' lines)";

// First failure of a PS3 package installation (package_reader, PS4 build).
enum class install_failure : std::uint8_t
{
    none,
    read,        // the PKG could not be read: USB drive gone or stalled, file cut or damaged
    write,       // a file in dev_hdd0 could not be written (a full disk is reported as no space instead)
    create_file, // a file in dev_hdd0 could not be created
    create_dir,  // a folder in dev_hdd0 could not be created
    edat,        // an SDAT inside the PKG did not decrypt
    entry_type,  // item of an unknown type
    path,        // item or install folder name that would leave the install folder
    threads,     // an extraction thread failed
};

// Short English name for startup.log.
inline const char* install_failure_name(install_failure kind)
{
    switch (kind)
    {
    case install_failure::none: return "unknown";
    case install_failure::read: return "PKG read error";
    case install_failure::write: return "write error";
    case install_failure::create_file: return "cannot create file";
    case install_failure::create_dir: return "cannot create folder";
    case install_failure::edat: return "EDAT/SDAT decryption failed";
    case install_failure::entry_type: return "unknown entry type";
    case install_failure::path: return "path outside the install folder";
    case install_failure::threads: return "extraction thread failed";
    }
    return "?";
}

// At most 'limit' bytes of a file name, cut on a UTF-8 character boundary ("..." marks the cut).
inline std::string clip_name(std::string_view name, std::size_t limit = 32)
{
    if (name.size() <= limit) return std::string(name);
    std::size_t cut = limit > 3 ? limit - 3 : 0;
    while (cut && (static_cast<unsigned char>(name[cut]) & 0xC0) == 0x80) --cut;
    return std::string(name.substr(0, cut)) + "...";
}

// XMB line for a failed installation. 'file': path or name of the file involved ("" when none), 'permission': the
// file system refused access, 'partial': partial files stayed in dev_hdd0 (a folder that existed before is never removed).
inline std::string install_failure_text(install_failure kind, std::string_view file, bool permission, bool partial)
{
    using i18n::str;
    const std::string name = clip_name(file_name(file));
    std::string text;
    switch (kind)
    {
    case install_failure::read: text = i18n::tr(str::COM_ERRO_LER_PKG_USB_DESCONECTADO); break;
    case install_failure::write: text = i18n::trf(permission ? str::PKG_WRITE_DENIED : str::PKG_WRITE_FAILED, {name}); break;
    case install_failure::create_file: text = i18n::trf(permission ? str::PKG_CREATE_FILE_DENIED : str::PKG_CREATE_FILE_FAILED, {name}); break;
    case install_failure::create_dir: text = i18n::trf(permission ? str::PKG_CREATE_DIR_DENIED : str::PKG_CREATE_DIR_FAILED, {name}); break;
    case install_failure::edat: text = i18n::trf(str::PKG_EDAT_FAILED, {name}); break;
    case install_failure::entry_type: text = i18n::trf(str::PKG_ENTRY_TYPE, {name}); break;
    case install_failure::path: text = i18n::trf(str::PKG_BAD_NAME, {name}); break;
    case install_failure::threads: text = i18n::tr(str::PKG_INSTALACAO_NAO_PODE_RODAR_THREADS); break;
    case install_failure::none: text = i18n::tr(str::COM_FALHA_INSTALAR_PKG_CONFIRA_RPCS3); break;
    }
    return i18n::trf(partial ? str::PKG_FAILURE_PARTIAL : str::PKG_FAILURE_NOTHING, {i18n::raw(text)});
}

// XMB line for a package of the booting disc or ISO (INSDIR, PKGDIR, PS3_EXTRA) that is refused before the install:
// the player did not copy this file, so it never reads as a renamed or half-copied PKG. 'rel': the path from
// PS3_GAME/ or PS3_EXTRA/ on; 'image': the disc is an ISO file (else a folder or the drive).
inline std::string disc_package_text(state kind, std::string_view rel, bool image)
{
    using i18n::str;
    const std::string name = clip_name(rel, 40);
    if (kind == state::ps4 || kind == state::psp_vita) return i18n::trf(str::PKG_DISC_NOT_PS3, {name});
    return i18n::trf(image ? str::PKG_DISC_IMAGE_UNREADABLE : str::PKG_DISC_FOLDER_UNREADABLE, {name});
}
}
