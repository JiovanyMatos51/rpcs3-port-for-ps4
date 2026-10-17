#pragma once

// Patches recommended on the console. The automatic configuration switches a few patches of the player's own patch
// files on for one boot: the ones that remove the MLAA anti-aliasing the game runs on its SPUs (several percent of the
// SPU time of a frame, and the console has fewer host cores than the PS3 has SPUs). Nothing is bundled: a patch is used
// only when patch.yml, imported_patch.yml or <TITLE_ID>_patch.yml holds it, and nothing is written to
// patch_config.yml. A second group is only marked in the XMB list as a suggestion and is never switched on.
//
// Opt-outs: /data/rpcs3/custom_configs/<TITLE_ID>.no-recommended-patches (every recommended patch of that game) and
// /data/rpcs3/patches/ps4_recommended_off.txt (one 'SERIAL|HASH|DESCRIPTION' line per refused patch, written by
// the XMB switch).
//
// Hashes, descriptions and serials are those of the RPCS3 patch.yml (patch engine 1.2). The serial lists are the
// games the entries name: in the entry itself, or for the entries listed for every serial, the serials the same file
// gives those games elsewhere. A serial decides whether the patch files are read at boot at all, the hash decides
// whether the patch touches anything (an SPU hash names one exact SPU program).
//
// Standard library only (host tested in ps4-port/tests/patches_test.cpp).

#include "ps4_i18n.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

namespace utils::ps4::recommended
{
enum class tier : unsigned char
{
    none,
    automatic, // switched on at boot in the automatic configuration, unless refused
    suggested, // only marked in the XMB list
};

struct entry
{
    std::span<const std::string_view> hashes;
    std::span<const std::string_view> descriptions;
    std::span<const std::string_view> serials; // empty: any game whose patch list shows it (suggested entries only)
    tier level;
    i18n::str why; // reason shown with the patch notes in the XMB (interface language)
};

inline constexpr std::string_view off_file_name = "ps4_recommended_off.txt"; // in the patches folder
inline constexpr std::string_view game_off_suffix = ".no-recommended-patches"; // custom_configs/<TITLE_ID> + this

namespace data
{
using list = std::string_view;

inline constexpr i18n::str why_mlaa = i18n::str::RPATCH_RECOMENDADO_PS4_TIRA_ANTISSERRILHADO_MLAA;
inline constexpr i18n::str why_post = i18n::str::RPATCH_SUGERIDO_PS4_TIRA_EFEITO_POS;
inline constexpr i18n::str why_lighting = i18n::str::RPATCH_SUGERIDO_PS4_ILUMINACAO_SAI_SPUS;

inline constexpr std::array<list, 8> gow_ascension_serials{"BCAS25016", "BCES01741", "BCES01742", "BCJS37008", "BCUS98232", "NPEA00445", "NPHA80258", "NPUA80918"};
inline constexpr std::array<list, 6> gow3_serials{"BCAS25003", "BCES00510", "BCES00799", "BCJS37001", "BCKS15003", "BCUS98111"};
inline constexpr std::array<list, 10> puppeteer_serials{"BCAS20281", "BCES00935", "BCJS30078", "BCUS98227", "NPEA00439", "NPHA80250", "NPJA00098", "NPUA80959", "NPEA90125", "NPJA90254"};
inline constexpr std::array<list, 4> dragon_age2_serials{"BLES01084", "BLUS30645", "NPEB00569", "NPUB30476"};
inline constexpr std::array<list, 3> wwe12_serials{"BLES01439", "BLJM60436", "BLUS30841"};
inline constexpr std::array<list, 4> alice_serials{"BLES01265", "BLUS30607", "NPEB00625", "NPUB30545"};
inline constexpr std::array<list, 1> prince_of_persia_serials{"BLUS30754"};
// Tokyo Jungle, SoulCalibur V, The Ratchet & Clank Trilogy, Portal 2
inline constexpr std::array<list, 16> edge_mlaa_01_serials{"BCAS20233", "BCJS30069", "NPEA00275", "NPJA00075", "NPUA80523",
    "BLES01250", "BLJS10115", "BLUS30736", "NPEB01363", "NPJB00381", "NPUB31195", "BCES01503", "NPEA00385", "NPEA00386", "NPEA00387",
    "NPUB31077"};
// ICO and Shadow of the Colossus (the collection and the separate PSN releases)
inline constexpr std::array<list, 7> ico_sotc_serials{"BCES01097", "BCES01115", "BCUS98259", "NPEA00279", "NPEA00280", "NPUA80676", "NPUA80677"};
inline constexpr std::array<list, 1> metro_serials{"NPEB01955"};
inline constexpr std::array<list, 3> twisted_metal_serials{"BCUS98106", "NPEA00362", "NPUA80079"};
// DmC Devil May Cry, MotorStorm: Apocalypse, MotorStorm RC, Sly Cooper: Thieves in Time, I Am Alive
inline constexpr std::array<list, 22> edge_mlaa_05_serials{"BLES01698", "BLJM60502", "BLUS30723", "NPEB01020", "NPJB00278", "NPUB30830",
    "BCES00484", "BCES01104", "BCUS98242", "NPEA00315", "NPUA80661", "NPEA00333", "NPHA80190", "NPJA00077", "NPUA80678",
    "BCES01284", "BCES01285", "BCUS98247", "NPEA00429", "NPUA80875", "NPEB00476", "NPUB30383"};
inline constexpr std::array<list, 14> lbp2_serials{"BCAS20113", "BCAS20201", "BCES00850", "BCES01086", "BCES01345", "BCES01346", "BCES01693",
    "BCES01694", "BCJS30058", "BCUS98245", "BCUS98372", "NPEA00324", "NPEA00437", "NPUA80662"};
inline constexpr std::array<list, 22> lbp2_lbp3_serials{"BCAS20113", "BCAS20201", "BCES00850", "BCES01086", "BCES01345", "BCES01346", "BCES01693",
    "BCES01694", "BCJS30058", "BCUS98245", "BCUS98372", "NPEA00324", "NPEA00437", "NPUA80662",
    "BCAS20322", "BCES01663", "BCES02068", "BCUS81138", "BCUS98362", "NPEA00515", "NPHA80277", "NPUA81116"};

inline constexpr std::array<list, 3> gow_ascension_spu{"SPU-7cd211ff1cbd33163eb0711440dccbb3c1dbcf6c", "SPU-1549476fe258150ff9f902229ffaed69a932a9c1",
    "SPU-77ce25dab2a5ca1d96038cc3dc57c50c705ce38c"};
inline constexpr std::array<list, 3> gow_ascension_spu_names{"Disable SPU MLAA - God of War: Ascension (01)",
    "Disable SPU MLAA - God of War: Ascension (02)", "Disable SPU MLAA - God of War: Ascension (03)"};
// GOWA.SELF 1.04 and 1.12
inline constexpr std::array<list, 2> gow_ascension_ppu{"PPU-3a63ecc6e9b81a984d2fe0a481febc3e26b57810", "PPU-3a0b43e4a5f4bfea64f53612ee7c5d990f88129c"};
// Versions 1.00 and 1.03
inline constexpr std::array<list, 2> gow3_spu{"SPU-530c255936b07b25467a58e24ceff5fd4e2960b7", "SPU-2239af4827b17317522bd6323c646b45b34ebf14"};
inline constexpr std::array<list, 2> gow3_ppu{"PPU-19724fde16a5b111b7b4d2a065f5dccaf8e01962", "PPU-4d5c51503a81a327c2a99427390a395b8dcb3767"};
inline constexpr std::array<list, 4> gow_ppu{"PPU-3a63ecc6e9b81a984d2fe0a481febc3e26b57810", "PPU-3a0b43e4a5f4bfea64f53612ee7c5d990f88129c",
    "PPU-19724fde16a5b111b7b4d2a065f5dccaf8e01962", "PPU-4d5c51503a81a327c2a99427390a395b8dcb3767"};
inline constexpr std::array<list, 1> puppeteer_spu{"SPU-9001b44fd7278b5a6fa5385939fe928a0e549394"};
inline constexpr std::array<list, 1> dragon_age2_spu{"SPU-7b5ea49122ec7f023d4a72452dc7a9208d9d6dbf"};
inline constexpr std::array<list, 1> wwe12_spu{"SPU-9c270732b4276ec71df9a5255d5d8a63267de645"};
inline constexpr std::array<list, 1> alice_spu{"SPU-191fe1c92c8360992b3240348e70ea37d50812d4"};
inline constexpr std::array<list, 1> prince_of_persia_spu{"SPU-5177cbc4bf45c8a0a6968c2a722da3a9e6cfb28b"};
inline constexpr std::array<list, 1> sotc_spu{"SPU-df5b1c3353cc36bb2f0fb59197d849bb99c3fecd"};
inline constexpr std::array<list, 1> ico_spu{"SPU-ac189d7f87091160a94e69803ac0cff0a8bb7813"};
inline constexpr std::array<list, 1> metro_spu{"SPU-e3780fe1dc8953f849ac844ec9688ff4da3ca3ae"};
inline constexpr std::array<list, 1> twisted_metal_spu{"SPU-82b3399c8e6533ba991eedb0e139bf20c7783bac"};
inline constexpr std::array<list, 1> edge_mlaa_05_spu{"SPU-969cf3e9db75f52a6b41074ccbff74106b709854"};
inline constexpr std::array<list, 2> lbp2_spu{"SPU-976d2128f08c362731413b75c934101b76c3d73b", "SPU-a129a01a270246c85df18eee0e959ef4263b6510"};
inline constexpr std::array<list, 2> lbp2_spu_names{"Disable SPU MLAA - LittleBigPlanet 2 (01)", "Disable SPU MLAA - LittleBigPlanet 2 (02)"};
inline constexpr std::array<list, 1> lbp2_lbp3_spu{"SPU-702d0205a89d445d15dc0f96548546c4e2e7a59f"};
// The Last of Us, Uncharted 2 and Uncharted 3 executables (retail, PSN, updates, demos)
inline constexpr std::array<list, 15> naughty_dog_ppu{"PPU-9df60dc1aa5005a0c80e9066e4951dc0471553e6", "PPU-120fb71f7352d62521c639b0e99f960018c10a56",
    "PPU-9e5c67eaf69077c591b7c503bed2b48617643134", "PPU-7b8a70c24456205b776915643a2acacb6ef6ea56", "PPU-9fe364d48b610e4b0a1c41d526bec9b1b2039c07",
    "PPU-a3a5789c12711291dfe16a7d5d81c906d2b4c0c2", "PPU-599461c6c88935ff0ed26ef20e649c8c38b7be9c", "PPU-bfee77256ce53064051a6b0ec9d292e75284aad4",
    "PPU-49c96996b6f3c0b32422375978c41e0c7ed95183", "PPU-48fa849dfeaef2f36ec042a576f29bda19ec1de0", "PPU-664fb0e4a94c62c15a6474841c202fb2bf80ce16",
    "PPU-02a88c3c6cd415b0bb81f1606bc743835881a4ba", "PPU-9584776e7fca409ed410f8a713832267020d8ac3", "PPU-40958bda5885891f0a4eb69f2c87ed4942dfa5e7",
    "PPU-0ff0b00602e5ffc67f32ab69840bbb2c7df735fe"};

inline constexpr std::array<list, 1> disable_mlaa{"Disable MLAA"};
inline constexpr std::array<list, 1> disable_spu_mlaa{"Disable SPU MLAA"};
inline constexpr std::array<list, 1> prince_of_persia_name{"Disable SPU MLAA - Prince of Persia series"};
inline constexpr std::array<list, 1> edge_mlaa_01_name{"Disable SPU MLAA - Multiple titles (01)"};
inline constexpr std::array<list, 1> sotc_name{"Disable SPU MLAA - Multiple titles (03)"};
inline constexpr std::array<list, 1> ico_name{"Disable SPU MLAA - ICO"};
inline constexpr std::array<list, 1> metro_name{"Disable SPU MLAA - Metro Last Light"};
inline constexpr std::array<list, 1> twisted_metal_name{"Disable SPU MLAA - Twisted Metal, Resident Evil: Operation Raccoon City"};
inline constexpr std::array<list, 1> edge_mlaa_05_name{"Disable SPU MLAA - Multiple titles (05)"};
inline constexpr std::array<list, 1> lbp2_lbp3_name{"Disable SPU MLAA - LittleBigPlanet 2, LittleBigPlanet 3, LittleBigPlanet Hub"};
inline constexpr std::array<list, 2> post_effects{"Disable Depth of Field", "Disable Motion Blur"};
inline constexpr std::array<list, 3> naughty_dog_post{"Disable SSAO", "Disable Depth of Field", "Disable Motion Blur"};
inline constexpr std::array<list, 1> gpu_lighting{"Enable GPU Lighting"};
inline constexpr std::array<list, 0> any_serial{};
} // namespace data

inline constexpr std::array<entry, 20> table{{
    {data::gow_ascension_spu, data::gow_ascension_spu_names, data::gow_ascension_serials, tier::automatic, data::why_mlaa},
    {data::gow_ascension_ppu, data::disable_mlaa, data::gow_ascension_serials, tier::automatic, data::why_mlaa},
    {data::gow3_spu, data::disable_spu_mlaa, data::gow3_serials, tier::automatic, data::why_mlaa},
    {data::gow3_ppu, data::disable_mlaa, data::gow3_serials, tier::automatic, data::why_mlaa},
    {data::puppeteer_spu, data::disable_spu_mlaa, data::puppeteer_serials, tier::automatic, data::why_mlaa},
    {data::dragon_age2_spu, data::disable_spu_mlaa, data::dragon_age2_serials, tier::automatic, data::why_mlaa},
    {data::wwe12_spu, data::disable_spu_mlaa, data::wwe12_serials, tier::automatic, data::why_mlaa},
    {data::alice_spu, data::disable_spu_mlaa, data::alice_serials, tier::automatic, data::why_mlaa},
    {data::prince_of_persia_spu, data::prince_of_persia_name, data::prince_of_persia_serials, tier::automatic, data::why_mlaa},
    {data::puppeteer_spu, data::edge_mlaa_01_name, data::edge_mlaa_01_serials, tier::automatic, data::why_mlaa},
    {data::sotc_spu, data::sotc_name, data::ico_sotc_serials, tier::automatic, data::why_mlaa},
    {data::ico_spu, data::ico_name, data::ico_sotc_serials, tier::automatic, data::why_mlaa},
    {data::metro_spu, data::metro_name, data::metro_serials, tier::automatic, data::why_mlaa},
    {data::twisted_metal_spu, data::twisted_metal_name, data::twisted_metal_serials, tier::automatic, data::why_mlaa},
    {data::edge_mlaa_05_spu, data::edge_mlaa_05_name, data::edge_mlaa_05_serials, tier::automatic, data::why_mlaa},
    {data::lbp2_spu, data::lbp2_spu_names, data::lbp2_serials, tier::automatic, data::why_mlaa},
    {data::lbp2_lbp3_spu, data::lbp2_lbp3_name, data::lbp2_lbp3_serials, tier::automatic, data::why_mlaa},
    {data::gow_ppu, data::post_effects, data::any_serial, tier::suggested, data::why_post},
    {data::naughty_dog_ppu, data::gpu_lighting, data::any_serial, tier::suggested, data::why_lighting},
    {data::naughty_dog_ppu, data::naughty_dog_post, data::any_serial, tier::suggested, data::why_post},
}};

namespace detail
{
inline bool contains(std::span<const std::string_view> list, std::string_view value)
{
    return std::find(list.begin(), list.end(), value) != list.end();
}
} // namespace detail

// The entry for one patch (hash and description as in the patch file) of the game 'serial', or null.
inline const entry* find(std::string_view hash, std::string_view description, std::string_view serial)
{
    for (const entry& e : table)
    {
        if (!detail::contains(e.hashes, hash) || !detail::contains(e.descriptions, description)) continue;
        if (!e.serials.empty() && (serial.empty() || !detail::contains(e.serials, serial))) continue;
        return &e;
    }
    return nullptr;
}

inline tier tier_for(std::string_view hash, std::string_view description, std::string_view serial)
{
    const entry* e = find(hash, description, serial);
    return e ? e->level : tier::none;
}

// True when some patch is switched on automatically for 'serial': only those games read the patch files at boot when
// patch_config.yml enables nothing for them.
inline bool listed(std::string_view serial)
{
    if (serial.empty()) return false;
    for (const entry& e : table)
        if (e.level == tier::automatic && detail::contains(e.serials, serial)) return true;
    return false;
}

// Boot gate: the patch files are read for the recommended patches only in the automatic configuration, without the
// game's opt-out marker, and when a patch file exists.
inline bool candidate(std::string_view serial, bool automatic_configuration, bool game_refused, bool patch_file_present)
{
    return automatic_configuration && !game_refused && patch_file_present && listed(serial);
}

// The switch patch_engine::apply reads for one patch at boot (patch_engine::patch_info::titles): for each title the
// game's serial before "All", then the app version before "All". Returns the first title's switch (null: the patch
// does not apply to this serial and version); 'already_on' tells whether any title's switch is already on, in which
// case apply() runs the patch without help.
template <typename Titles>
auto* boot_slot(Titles& titles, const std::string& serial, const std::string& app_version, const std::string& all, bool& already_on)
{
    using values = std::remove_reference_t<decltype(titles.begin()->second.begin()->second.begin()->second)>;
    values* first = nullptr;
    already_on = false;
    for (auto& [title, serials] : titles)
    {
        auto found = serial.empty() ? serials.end() : serials.find(serial);
        if (found == serials.end()) found = serials.find(all);
        if (found == serials.end()) continue;
        auto version = found->second.find(app_version);
        if (version == found->second.end()) version = found->second.find(all);
        if (version == found->second.end()) continue;
        if (version->second.enabled) already_on = true;
        if (!first) first = &version->second;
    }
    return first;
}

// ---------------------------------------------------------------------------------------------------------------
// ps4_recommended_off.txt: one 'SERIAL|HASH|DESCRIPTION' line per refused patch; '#' lines are comments.

inline constexpr std::string_view off_file_header = "# Patches recomendados no PS4 desligados pelo jogador: SERIAL|HASH|DESCRIPTION (um por linha)";

inline std::string off_line(std::string_view serial, std::string_view hash, std::string_view description)
{
    std::string line;
    line.reserve(serial.size() + hash.size() + description.size() + 2);
    line.append(serial).append(1, '|').append(hash).append(1, '|').append(description);
    return line;
}

namespace detail
{
// Calls 'visit' with each line without its line break and trailing spaces.
template <typename Visit>
void for_each_line(std::string_view content, Visit&& visit)
{
    while (!content.empty())
    {
        const std::size_t end = content.find('\n');
        std::string_view line = content.substr(0, end);
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.remove_suffix(1);
        visit(line);
        if (end == std::string_view::npos) break;
        content.remove_prefix(end + 1);
    }
}
} // namespace detail

inline bool refused(std::string_view content, std::string_view serial, std::string_view hash, std::string_view description)
{
    const std::string wanted = off_line(serial, hash, description);
    bool found = false;
    detail::for_each_line(content, [&](std::string_view line) { found = found || line == wanted; });
    return found;
}

// The file with 'line' present once ('refuse') or absent; other lines are kept in their order.
inline std::string with_line(std::string_view content, std::string_view line, bool refuse)
{
    std::string out;
    detail::for_each_line(content, [&](std::string_view existing)
    {
        if (existing.empty() || existing == line) return;
        out.append(existing).append(1, '\n');
    });
    if (out.empty()) out.append(off_file_header).append(1, '\n');
    if (refuse) out.append(line).append(1, '\n');
    return out;
}
} // namespace utils::ps4::recommended
