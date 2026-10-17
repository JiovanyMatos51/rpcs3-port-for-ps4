#include "stdafx.h"

// 0.33: the SCEDecrypter step of the XMB firmware installer (util/ps4_firmware.hpp). Built with the loader objects:
// Crypto/unself.h includes "zlib.h", which only their include path reaches.

#include "util/ps4_firmware.hpp"
#include "Crypto/key_vault.h"
#include "Crypto/unself.h"

namespace
{
// SCEDecrypter::DecryptData() trusts the section table LoadMetadata() decrypted: a section outside the package, or an
// IV index at the end of the key table, ends the calling thread (narrow<u32>, read_from_ptr's ensure), and a huge size
// makes it allocate a buffer that size, whose failure ends the application (no exceptions). Packages reach this only
// after the PUP HMAC matched, and official ones keep their disjoint sections inside the file, so only hand-made
// packages are refused here.
struct checked_decrypter final : SCEDecrypter
{
    using SCEDecrypter::SCEDecrypter;

    bool sections_fit(u64 package_size, std::string& error) const
    {
        u64 total = 0;
        for (const MetadataSectionHeader& section : meta_shdr)
        {
            if (section.data_offset > package_size || package_size - section.data_offset < section.data_size)
            {
                error = fmt::format("SCE section at 0x%x (0x%x bytes) lies outside the 0x%x-byte package", section.data_offset, section.data_size, package_size);
                return false;
            }
            total += section.data_size;
            if (total > package_size)
            {
                error = fmt::format("SCE sections add up to more than the 0x%x-byte package", package_size);
                return false;
            }
            if (section.encrypted == 3 && section.key_idx < meta_hdr.key_count && section.iv_idx >= meta_hdr.key_count)
            {
                error = fmt::format("SCE section IV index %u is past the %u keys", section.iv_idx, meta_hdr.key_count);
                return false;
            }
        }
        return true;
    }
};
} // namespace

namespace utils::ps4::firmware
{
fs::file decrypt_package(const fs::file& package, std::string& error)
{
    // main_window.cpp ignores these results; here a package that fails any step stops the install before the swap.
    checked_decrypter decrypter(package);
    if (!decrypter.LoadHeaders())
    {
        error = "not an SCE file";
        return {};
    }
    if (!decrypter.LoadMetadata(SCEPKG_ERK, SCEPKG_RIV))
    {
        error = "SCE metadata could not be decrypted with the SCEPKG keys";
        return {};
    }
    if (!decrypter.sections_fit(package.size(), error)) return {};
    if (!decrypter.DecryptData())
    {
        error = "SCE data could not be decrypted";
        return {};
    }
    std::vector<fs::file> sections = decrypter.MakeFile();
    if (sections.size() < 3)
    {
        error = fmt::format("%u usable section(s) after decompression, 3 expected", sections.size());
        return {};
    }
    return std::move(sections[2]);
}
} // namespace utils::ps4::firmware
