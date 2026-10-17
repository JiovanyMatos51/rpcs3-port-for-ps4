#pragma once

// 0.31: RPCN account helpers for the PS4 account service (Emu/NP/ps4_rpcn_service.cpp) and its host test
// (ps4-port/tests/rpcn_account_test.cpp).
//
// derive_password is the derivation of the PC account dialog (rpcs3qt/rpcn_settings_dialog.cpp, derive_password):
// PBKDF2-HMAC-SHA3-256, 200000 iterations, the fixed salt below, 32 bytes written as 64 uppercase hex digits. The
// server only ever receives that hex string, so an account made on the PC signs in here with the same typed
// password, and rpcn.yml files can be copied between the two. It must stay byte-identical to the PC version.
//
// Needs the wolfSSL headers: the NP target (WOLFSSL_USE_OPTIONS_H) or the host test (WOLFSSL_USER_SETTINGS). Never
// include this from the frontend, which is built without the wolfSSL ABI defines. Nothing here logs anything.

#include "Emu/NP/ps4_rpcn_service.h"
#include "util/ps4_i18n.hpp"

#include <climits>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wold-style-cast"
#pragma clang diagnostic ignored "-Wextern-c-compat"
#pragma clang diagnostic ignored "-Wunsafe-buffer-usage"
#endif
#include <wolfssl/wolfcrypt/settings.h>
#include <wolfssl/wolfcrypt/hash.h>
#include <wolfssl/wolfcrypt/pwdbased.h>
#include <wolfssl/wolfcrypt/sha3.h>
#ifdef __clang__
#pragma clang diagnostic pop
#endif

namespace utils::ps4::rpcn_account
{
inline constexpr std::string_view salt = "No matter where you go, everybody's connected.";
inline constexpr int iterations = 200'000;
inline constexpr std::size_t digest_size = 32;        // SHA3-256
inline constexpr std::size_t derived_size = 64;       // hex digits stored as rpcn.yml "Password"
inline constexpr std::size_t username_min = 3, username_max = 16;
inline constexpr std::size_t token_size = 16;
inline constexpr std::size_t email_max = 254;
inline constexpr std::size_t password_max = 256;      // typed password (the keyboard allows far less)
inline constexpr std::size_t import_max_bytes = 64 * 1024;
inline constexpr unsigned config_version = 2;         // rpcn.yml "Version" written by current PC builds
inline constexpr char default_avatar[] = "https://rpcs3.net/cdn/netplay/DefaultAvatar.png";

static_assert(digest_size == WC_SHA3_256_DIGEST_SIZE);

constexpr bool is_ascii_alnum(char c) noexcept
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

constexpr bool is_hex_digit(char c) noexcept
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f');
}

// Same rule as the PC dialog: 3-16 characters of A-Z, a-z, 0-9, '-' and '_' (case sensitive on the server).
constexpr bool validate_username(std::string_view name) noexcept
{
    if (name.size() < username_min || name.size() > username_max) return false;
    for (const char c : name)
        if (!is_ascii_alnum(c) && c != '-' && c != '_') return false;
    return true;
}

// The email token: 16 characters of 0-9 and A-Z.
constexpr bool validate_token(std::string_view token) noexcept
{
    if (token.size() != token_size) return false;
    for (const char c : token)
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z'))) return false;
    return true;
}

// ASCII version of the PC check (local part of letters, digits and !#$%&'*+/=?^_`{|}~.- , then '@', then labels of
// letters, digits and '-' separated by single dots), plus at least one dot in the domain: the server mails the
// token, so a domain without a dot can never receive it.
constexpr bool validate_email(std::string_view email) noexcept
{
    if (email.empty() || email.size() > email_max) return false;
    const auto at = email.find('@');
    if (at == 0 || at == std::string_view::npos || email.find('@', at + 1) != std::string_view::npos) return false;
    constexpr std::string_view local_extra = ".!#$%&'*+/=?^_`{|}~-";
    for (const char c : email.substr(0, at))
        if (!is_ascii_alnum(c) && local_extra.find(c) == std::string_view::npos) return false;
    const std::string_view domain = email.substr(at + 1);
    if (domain.empty() || domain.front() == '.' || domain.back() == '.') return false;
    bool dot = false;
    char prev = 0;
    for (const char c : domain)
    {
        if (c == '.')
        {
            if (prev == '.') return false;
            dot = true;
        }
        else if (!is_ascii_alnum(c) && c != '-')
        {
            return false;
        }
        prev = c;
    }
    return dot;
}

// A stored rpcn.yml "Password": the 64 hex digits of derive_password (the PC writes uppercase).
constexpr bool is_derived_password(std::string_view value) noexcept
{
    if (value.size() != derived_size) return false;
    for (const char c : value)
        if (!is_hex_digit(c)) return false;
    return true;
}

// Strips spaces, tabs and line breaks the on-screen keyboard may leave at either end.
constexpr std::string_view trim(std::string_view s) noexcept
{
    constexpr std::string_view blanks = " \t\r\n";
    const auto first = s.find_first_not_of(blanks);
    if (first == std::string_view::npos) return {};
    return s.substr(first, s.find_last_not_of(blanks) - first + 1);
}

// Tokens are uppercase; accept one typed in lowercase.
inline std::string normalize_token(std::string_view token)
{
    std::string out(trim(token));
    for (char& c : out)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return out;
}

// Overwrites the characters before releasing them, so a password does not linger in freed memory. The whole
// capacity is cleared: a string that was moved from (move assignment keeps the old short-string bytes after the
// first one) or shortened still holds old characters past size().
inline void wipe(std::string& s) noexcept
{
    s.resize(s.capacity()); // never reallocates; zero-fills [size, capacity)
    volatile char* p = s.data();
    for (std::size_t i = 0; i < s.size(); i++) p[i] = 0;
    s.clear();
}

inline void wipe(void* data, std::size_t size) noexcept
{
    volatile unsigned char* p = static_cast<unsigned char*>(data);
    for (std::size_t i = 0; i < size; i++) p[i] = 0;
}

inline std::string hex_upper(const std::uint8_t* data, std::size_t size)
{
    constexpr char pal[] = "0123456789ABCDEF";
    std::string out(size * 2, '0');
    for (std::size_t i = 0; i < size; i++)
    {
        out[i * 2] = pal[data[i] >> 4];
        out[i * 2 + 1] = pal[data[i] & 15];
    }
    return out;
}

// The PC derivation. Returns an empty string if wolfSSL fails (the PC build stops there; the PS4 reports it).
inline std::string derive_password(std::string_view typed)
{
    if (typed.size() > static_cast<std::size_t>(INT_MAX)) return {};
    std::uint8_t digest[digest_size]{};
    const int ret = wc_PBKDF2(digest, reinterpret_cast<const byte*>(typed.data()), static_cast<int>(typed.size()),
        reinterpret_cast<const byte*>(salt.data()), static_cast<int>(salt.size()), iterations,
        static_cast<int>(digest_size), WC_SHA3_256);
    std::string out = ret == 0 ? hex_upper(digest, digest_size) : std::string{};
    wipe(digest, sizeof(digest));
    return out;
}

// rpcn.yml "Hosts": "description|address" pairs joined by "|||". Malformed entries are skipped, as cfg_rpcn does.
struct host_entry
{
    std::string description, address;
};

inline std::vector<host_entry> parse_hosts(std::string_view value)
{
    std::vector<host_entry> out;
    std::size_t pos = 0;
    while (pos <= value.size())
    {
        const auto end = value.find("|||", pos);
        const std::string_view item = value.substr(pos, end == std::string_view::npos ? std::string_view::npos : end - pos);
        std::vector<std::string_view> parts;
        std::size_t p = 0;
        while (p <= item.size())
        {
            const auto bar = item.find('|', p);
            const std::string_view part = item.substr(p, bar == std::string_view::npos ? std::string_view::npos : bar - p);
            if (!part.empty()) parts.push_back(part);
            if (bar == std::string_view::npos) break;
            p = bar + 1;
        }
        if (parts.size() == 2) out.push_back({std::string(parts[0]), std::string(parts[1])});
        if (end == std::string_view::npos) break;
        pos = end + 3;
    }
    return out;
}

// The address after 'current' in the list (wrapping), or the first one when 'current' is not listed.
inline std::string next_host(const std::vector<std::string>& addresses, std::string_view current)
{
    if (addresses.empty()) return std::string(current);
    for (std::size_t i = 0; i < addresses.size(); i++)
        if (addresses[i] == current) return addresses[(i + 1) % addresses.size()];
    return addresses.front();
}

// Checks an rpcn.yml before it replaces the saved one. Empty: usable; otherwise the reason for the XMB (interface
// language, static storage).
inline std::string_view check_import(std::string_view npid, std::string_view password, unsigned version)
{
    using i18n::str;
    if (version < config_version) return i18n::tr(str::RPCN_RPCN_YML_ANTIGO_ENTRE_CONTA);
    if (!validate_username(npid)) return i18n::tr(str::RPCN_RPCN_YML_SEM_NOME_USUARIO);
    if (!is_derived_password(password)) return i18n::tr(str::RPCN_RPCN_YML_SEM_SENHA);
    return {};
}

// Checks what the XMB collected for a job, after trim()/normalize_token(). Empty: complete; otherwise the text to
// show (interface language, static storage). The password is checked for presence and length only; it is never part
// of the returned text.
inline std::string_view check_request(ps4_rpcn::job type, std::string_view user, std::string_view pass,
    std::string_view email, std::string_view token)
{
    using ps4_rpcn::job;
    using i18n::str;
    const bool needs_account = type == job::login || type == job::create;
    if (needs_account)
    {
        if (!validate_username(user)) return i18n::tr(str::RPCN_NOME_USUARIO_INVALIDO_3_16);
        if (pass.empty()) return i18n::tr(str::RPCN_DIGITE_SENHA);
        if (pass.size() > password_max) return i18n::tr(str::RPCN_SENHA_LONGA_DEMAIS);
    }
    if (type == job::create && !validate_email(email)) return i18n::tr(str::RPCN_EMAIL_INVALIDO);
    if (type == job::token && !validate_token(token)) return i18n::tr(str::RPCN_TOKEN_INVALIDO_16_LETRAS_MAIUSCULAS);
    if (type == job::login && !token.empty() && !validate_token(token)) return i18n::tr(str::RPCN_TOKEN_INVALIDO_16_LETRAS_MAIUSCULAS);
    return {};
}
} // namespace utils::ps4::rpcn_account
