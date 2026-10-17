// Host test for the console patch manager helpers (rpcs3/util/ps4_patches.hpp): the rpcs3.net reply parser, the
// import file naming, the names each import source takes, the import result texts (XMB line and startup.log line, in
// pt-BR and the other interface languages), the curl error texts, the patch list of one game (serial and app version
// chosen as patch_engine::apply chooses them,
// one row per description over several hashes, patch groups) and the map handed to patch_engine::save_config.
// Recommended patches (rpcs3/util/ps4_recommended_patches.hpp): table lookup, the boot gate per configuration mode,
// the switch the boot turns on (same as the XMB row), and the ps4_recommended_off.txt round trip.
//
// Only the standard-library and patch_engine-type parts of the header are built here (RPCS3_PS4 is not defined, so
// the file and network code stays out); Utilities/bin_patch.h is used for its types only.
//
// Build and run from the repository root (llvm-mingw, already used for the other host tests):
//   clang++ -std=c++23 -static -Wall -Wextra -I . -I rpcs3 -I 3rdparty/yaml-cpp/yaml-cpp/include
//     ps4-port/tests/patches_test.cpp -o patches_test.exe
//   patches_test.exe

#include "util/ps4_patches.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace
{
int g_failures = 0;

#define CHECK(expr) \
    do \
    { \
        if (!(expr)) \
        { \
            std::printf("FAILED line %d: %s\n", __LINE__, #expr); \
            ++g_failures; \
        } \
    } while (0)

namespace patches = utils::ps4::patches;

void test_text()
{
    CHECK(patches::flatten("  a\r\n\tb   c \n") == "a b c");
    CHECK(patches::flatten("") == "");
    CHECK(patches::clip("abcdef", 6) == "abcdef");
    CHECK(patches::clip("abcdefg", 6) == "abc...");
    // "é" is two bytes: the cut never splits it
    CHECK(patches::clip("ab\xC3\xA9" "cdef", 6) == "ab...");
    CHECK(patches::lower("PATCH.Yml") == "patch.yml");

    std::size_t count = 0;
    CHECK(patches::page("", 0, 20, count).empty() && count == 1);
    const std::string text = "one two three four five six seven eight nine ten";
    CHECK(patches::page(text, 0, 20, count) == "one two three four" && count == 3);
    CHECK(patches::page(text, 1, 20, count) == "five six seven eight");
    CHECK(patches::page(text, 2, 20, count) == "nine ten");
    CHECK(patches::page(text, 3, 20, count) == "one two three four"); // wraps around
    // A long word is cut at the width, never inside a UTF-8 character
    const std::string word = "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9"; // 10 bytes, 5 characters
    const std::string first = patches::page(word, 0, 9, count);
    CHECK(first == "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9" && count == 2);
}

void test_json()
{
    std::map<std::string, std::string, std::less<>> fields;
    CHECK(patches::parse_json_fields(R"( {"return_code":0,"version":"1.2","sha256":"AbC","patch":"Version: 1.2\n\"x\": \\ \/ \u00e9 \ud83d\ude00"} )", fields));
    CHECK(fields["return_code"] == "0");
    CHECK(fields["version"] == "1.2");
    CHECK(fields["sha256"] == "AbC");
    CHECK(fields["patch"] == "Version: 1.2\n\"x\": \\ / \xC3\xA9 \xF0\x9F\x98\x80");

    CHECK(patches::parse_json_fields(R"({"return_code":-1,"extra":{"a":[1,2,{"b":"}"}]},"ok":true,"none":null})", fields));
    CHECK(fields["return_code"] == "-1");
    CHECK(!fields.contains("extra"));
    CHECK(fields["ok"] == "true" && fields["none"] == "null");

    // Lone surrogates become U+FFFD
    CHECK(patches::parse_json_fields(R"({"a":"\ud83d x","b":"\ude00"})", fields));
    CHECK(fields["a"] == "\xEF\xBF\xBD x" && fields["b"] == "\xEF\xBF\xBD");

    CHECK(patches::parse_json_fields("{}", fields) && fields.empty());
    CHECK(!patches::parse_json_fields("", fields));
    CHECK(!patches::parse_json_fields("[]", fields));
    CHECK(!patches::parse_json_fields(R"({"a":1,})", fields));
    CHECK(!patches::parse_json_fields(R"({"a":"x)", fields));
    CHECK(!patches::parse_json_fields(R"({"a":"x"} trailing)", fields));
    CHECK(!patches::parse_json_fields(R"({"a":"\q"})", fields));
    CHECK(!patches::parse_json_fields(R"({"a":"\u12"})", fields));
    CHECK(!patches::parse_json_fields("{\"a\":\"line\nbreak\"}", fields)); // raw control character
    CHECK(!patches::parse_json_fields(R"({"a":{"b":1})", fields));
    CHECK(!patches::parse_json_fields(R"({"a":})", fields));
}

void test_import_naming()
{
    auto t = patches::import_target_for("patch.yml");
    CHECK(!t.merge && t.name == "patch.yml");
    t = patches::import_target_for("PATCH.YML");
    CHECK(!t.merge && t.name == "patch.yml");
    t = patches::import_target_for("Imported_Patch.yml");
    CHECK(!t.merge && t.name == "imported_patch.yml");
    t = patches::import_target_for("blus30339_patch.yml");
    CHECK(!t.merge && t.name == "BLUS30339_patch.yml");
    t = patches::import_target_for("BLUS3033_patch.yml"); // 8 characters: not a title ID
    CHECK(t.merge);
    t = patches::import_target_for("my 60fps.yml");
    CHECK(t.merge);

    CHECK(patches::never_imported("RPCN.yml"));
    CHECK(patches::never_imported("patch_config.yml"));
    CHECK(!patches::never_imported("patch.yml"));

    CHECK(patches::looks_like_patch_file("Version: 1.2\n\nPPU-x:\n"));
    CHECK(patches::looks_like_patch_file("\xEF\xBB\xBFVersion: 1.2\n"));
    CHECK(patches::looks_like_patch_file("# comment\r\nVersion : 1.2\r\n"));
    CHECK(!patches::looks_like_patch_file("  Version: 1.2\n"));
    CHECK(!patches::looks_like_patch_file("Versions: 2\n"));
    CHECK(!patches::looks_like_patch_file("Username: x\nPassword: y\n"));
}

// Which names each import source takes
void test_import_sources()
{
    // Import folders (USB:/rpcs3/patches, /data/rpcs3/import): every *.yml but dot files and the never_imported names
    CHECK(patches::import_folder_candidate("patch.yml"));
    CHECK(patches::import_folder_candidate("My 60fps.YML"));
    CHECK(patches::import_folder_candidate("BLUS30443_patch.yml"));
    CHECK(!patches::import_folder_candidate("._patch.yml"));
    CHECK(!patches::import_folder_candidate("patch.yaml"));
    CHECK(!patches::import_folder_candidate(".yml"));
    CHECK(!patches::import_folder_candidate("rpcn.yml"));
    CHECK(!patches::import_folder_candidate("config.yml"));

    // /data/rpcs3 itself: only the names the boot reads (rpcn.yml, games.yml, config.yml, vfs.yml stay out)
    CHECK(patches::data_root_candidate("patch.yml"));
    CHECK(patches::data_root_candidate("PATCH.yml"));
    CHECK(patches::data_root_candidate("imported_patch.yml"));
    CHECK(patches::data_root_candidate("blus30443_patch.yml"));
    CHECK(!patches::data_root_candidate("my 60fps.yml"));
    CHECK(!patches::data_root_candidate("rpcn.yml"));
    CHECK(!patches::data_root_candidate("games.yml"));
    CHECK(!patches::data_root_candidate("config.yml"));
    CHECK(!patches::data_root_candidate("vfs.yml"));
    CHECK(!patches::data_root_candidate("ps4-settings.yml"));
    CHECK(!patches::data_root_candidate("patch_config.yml"));
    CHECK(!patches::data_root_candidate("._patch.yml"));
}

// The XMB line of every import outcome (at most 110 bytes) and the startup.log line
void test_import_result()
{
    using patches::import_failure;
    const auto fits = [](const std::string& text) { return text.size() <= patches::import_text_limit && !text.ends_with("..."); };

    patches::import_outcome o;
    std::string text = patches::import_result_text(o);
    CHECK(text == utils::ps4::i18n::tr(patches::import_nothing_text) && fits(text));
    CHECK(text.find("/data/rpcs3/patches/") != std::string::npos);

    o.ready_files = {"patch.yml"};
    o.ready_patches = 5123;
    text = patches::import_result_text(o);
    CHECK(text == "patch.yml já está em /data/rpcs3/patches: 5123 patches prontos · R1 num jogo para ligar" && fits(text));
    o.ready_files = {"patch.yml", "BLUS30443_patch.yml"};
    text = patches::import_result_text(o);
    CHECK(text.starts_with("patch.yml, BLUS30443_patch.yml já estão em /data/rpcs3/patches: 5123 patches") && text.size() <= 110);

    o = {};
    o.unchanged = 2;
    text = patches::import_result_text(o);
    CHECK(text == "Os patches já estavam importados (nada mudou)");

    o = {};
    o.done = {"patch.yml (5123 patches)"};
    text = patches::import_result_text(o);
    CHECK(text == "Patches importados: patch.yml (5123 patches) · R1 num jogo para ligar" && fits(text));
    o.warnings = {"patch.yml"};
    CHECK(patches::import_result_text(o) == "Patches importados: patch.yml (5123 patches) · R1 num jogo para ligar · com entradas ignoradas (RPCS3.log)");

    // One failure and nothing imported: the failure's own text
    o = {};
    o.problems = {{"patch.yml", import_failure::unreadable, "erro Access violation"}};
    text = patches::import_result_text(o);
    CHECK(text == "Não foi possível ler patch.yml (erro Access violation): copie de novo" && fits(text));
    o.problems = {{"patch.yml", import_failure::unreadable, "maior que 32 MB"}};
    CHECK(patches::import_result_text(o) == "Não foi possível ler patch.yml (maior que 32 MB): copie de novo");
    o.problems = {{"notes.yml", import_failure::not_patch, {}}};
    text = patches::import_result_text(o);
    CHECK(text == "notes.yml não é um arquivo de patch (falta 'Version:')" && fits(text));
    o.problems = {{"patch.yml", import_failure::rejected, {}}};
    text = patches::import_result_text(o);
    CHECK(text.starts_with("patch.yml recusado pelo motor de patches (RPCS3.log)") && text.find("FTP") != std::string::npos && fits(text));
    o.problems = {{"BLUS30443_patch.yml", import_failure::rejected, {}}};
    CHECK(fits(patches::import_result_text(o)));
    o.problems = {{"patch.yml", import_failure::write_failed, "erro Not enough space on the device"}};
    text = patches::import_result_text(o);
    CHECK(text == "Não foi possível gravar patch.yml em /data/rpcs3/patches (erro Not enough space on the device)" && fits(text));
    o.problems = {{"", import_failure::write_failed, "erro Read only"}};
    CHECK(patches::import_result_text(o) == "Não foi possível criar /data/rpcs3/patches (erro Read only)");

    // A refused permission explains everything else
    o = {};
    o.done = {"a.yml (3 novos)"};
    o.problems = {{"patch.yml", import_failure::rejected, {}}, {"x.yml", import_failure::not_writable, {}}};
    text = patches::import_result_text(o);
    CHECK(text == utils::ps4::i18n::tr(patches::import_not_writable_text) && fits(text));
    CHECK(text.find("folders:") != std::string::npos);

    // Mixed
    o = {};
    o.done = {"patch.yml (5123 patches)"};
    o.problems = {{"notes.yml", import_failure::not_patch, {}}};
    o.skipped = {"patch.yml"};
    text = patches::import_result_text(o);
    CHECK(text.starts_with("Importados: patch.yml (5123 patches) · 1 inválido(s): notes.yml · 1 ignorado(s), mesmo nome") && text.size() <= 110);
    o.done.clear();
    o.problems.push_back({"b.yml", import_failure::rejected, {}});
    CHECK(patches::import_result_text(o).starts_with("Nenhum patch importado · 2 inválido(s): notes.yml, b.yml"));

    // Scanned places and the log line
    std::vector<patches::import_scan> scans;
    scans.push_back({"usb0:/rpcs3/patches", true, 0, 2, {}});
    scans.push_back({"/data/rpcs3/import", false, 0, 0, {}});
    scans.push_back({"/data/rpcs3/patches", true, 0, 1, "check-only, 5123 patches"});
    scans.push_back({"usb1:/rpcs3/patches", false, 13, 0, {}});
    scans.push_back({"USB", false, 0, 0, "no drive"});
    const std::string scanned = patches::import_scan_text(scans);
    CHECK(scanned == "usb0:/rpcs3/patches (2 file(s)); /data/rpcs3/import (absent); /data/rpcs3/patches (1 file(s), check-only, 5123 patches); "
                     "usb1:/rpcs3/patches (errno 13); USB (absent, no drive)");
    CHECK(patches::import_result_line("Os patches já estavam importados (nada mudou)", scanned) ==
        "patches: import result: Os patches já estavam importados (nada mudou) | scanned: " + scanned);
}

void test_curl_text()
{
    CHECK(patches::curl_error_text(27) == "falha interna do curl (recursos do sistema)");
    CHECK(patches::curl_error_text(1).find("network ABI") != std::string::npos);
    CHECK(patches::curl_error_text(56) == "a conexão caiu no meio");
    CHECK(patches::curl_error_text(60).find("data e hora") != std::string::npos);
    CHECK(patches::curl_error_text(77) == "certificados raiz inválidos no pacote");
    CHECK(patches::curl_error_text(7) == "sem conexão com rpcs3.net");
    CHECK(patches::curl_error_text(28) == "tempo esgotado");
    CHECK(patches::curl_error_text(35) == "falha na conexão segura (TLS)");
    CHECK(patches::curl_error_text(99) == "erro 99");
    // "Falha no download dos patches: " + the longest text still fits the XMB line
    for (const int code : {1, 6, 7, 23, 27, 28, 35, 42, 55, 56, 60, 77})
        CHECK(std::string("Falha no download dos patches: ").size() + patches::curl_error_text(code).size() <= 110);
    CHECK(patches::curl_error_text(23) == "resposta grande demais");
    CHECK(patches::curl_error_text(42) == "cancelado");
}

// The same texts in the other interface languages: no pt-BR left, names and numbers kept, the XMB line limit
void test_languages()
{
    namespace i18n = utils::ps4::i18n;
    using patches::import_failure;
    i18n::apply(i18n::setting::en, -1);
    patches::import_outcome o;
    o.ready_files = {"patch.yml"};
    o.ready_patches = 5123;
    CHECK(patches::import_result_text(o) == "patch.yml is already in /data/rpcs3/patches: 5123 patches ready · R1 in a game to turn them on");
    o = {};
    o.done = {"patch.yml (5123 patches)"};
    o.problems = {{"notes.yml", import_failure::not_patch, {}}};
    o.skipped = {"patch.yml"};
    CHECK(patches::import_result_text(o).starts_with("Imported: patch.yml (5123 patches) · 1 invalid: notes.yml · 1 skipped, same name"));
    CHECK(patches::curl_error_text(99) == "error 99");
    CHECK(patches::curl_error_text(28) == "timed out");
    for (const i18n::setting language : {i18n::setting::en, i18n::setting::es, i18n::setting::ar})
    {
        i18n::apply(language, -1);
        const std::vector<patches::import_outcome> outcomes = [&]
        {
            std::vector<patches::import_outcome> list(9);
            list[1].ready_files = {"patch.yml", "BLUS30443_patch.yml"};
            list[1].ready_patches = 5123;
            list[2].unchanged = 1;
            list[3].done = {"patch.yml (5123 patches)"};
            list[3].warnings = {"patch.yml"};
            list[4].problems = {{"patch.yml", import_failure::unreadable, "x"}};
            list[5].problems = {{"BLUS30443_patch.yml", import_failure::rejected, {}}};
            list[6].problems = {{"patch.yml", import_failure::write_failed, "x"}};
            list[7].problems = {{"", import_failure::write_failed, "x"}};
            list[8].problems = {{"x.yml", import_failure::not_writable, {}}};
            return list;
        }();
        for (const auto& outcome : outcomes)
        {
            const std::string text = patches::import_result_text(outcome);
            // Arabic letters take two bytes: twice the bytes for about the same width
            const std::size_t limit = language == i18n::setting::ar ? 2 * patches::import_text_limit : patches::import_text_limit;
            CHECK(!text.empty() && text.size() <= limit && patches::line_bytes(patches::import_text_limit) == limit);
            // Only the two-file line is cut (as in pt-BR)
            CHECK(&outcome == &outcomes[1] || !text.ends_with("..."));
            CHECK(text.find("patches prontos") == std::string::npos && text.find("Não foi") == std::string::npos && text.find("Patches importados") == std::string::npos &&
                text.find("estavam importados") == std::string::npos && text.find("recusado") == std::string::npos && text.find("permissão") == std::string::npos);
        }
        // "cancelado" is the same word in Spanish
        for (const int code : {1, 6, 7, 23, 27, 35, 42, 55, 60, 77})
            CHECK((code == 42 && language == i18n::setting::es) ||
                patches::curl_error_text(code) != [&] { i18n::apply(i18n::setting::pt_br, -1); std::string pt = patches::curl_error_text(code); i18n::apply(language, -1); return pt; }());
        for (const auto& e : utils::ps4::recommended::table)
            CHECK(std::string_view(i18n::tr(e.why)) != i18n::tr(e.why, i18n::lang::pt_br));
    }
    i18n::apply(i18n::setting::automatic, -1);
}

using patch_map = patch_engine::patch_map;

patch_engine::patch_info& add(patch_map& map, const std::string& hash, const std::string& description, const std::string& title,
    const std::string& serial, std::initializer_list<const char*> versions)
{
    auto& info = map[hash].patch_info_map[description];
    info.description = description;
    info.hash = hash;
    for (const char* version : versions) info.titles[title][serial][version] = {};
    return info;
}

void enable(patch_map& config, const std::string& hash, const std::string& description, const std::string& title,
    const std::string& serial, const std::string& version, bool on = true)
{
    config[hash].patch_info_map[description].titles[title][serial][version].enabled = on;
}

void test_rows()
{
    patch_map catalog;
    auto& fps = add(catalog, "PPU-a", "60 FPS", "Game", "BLUS00001", {"01.00", "01.01"});
    fps.author = "Someone";
    fps.notes = "Unlocks the frame rate";
    fps.patch_group = "fps";
    fps.default_config_values["Target"].value = 60;
    add(catalog, "SPU-b", "60 FPS", "Game", "BLUS00001", {"01.01"}).patch_group = "fps";
    add(catalog, "PPU-a", "30 FPS cap", "Game", "BLUS00001", {"All"}).patch_group = "fps";
    add(catalog, "PPU-a", "Skip intro", "Game", "BLUS00001", {"01.01"}).patch_group = "fps";
    add(catalog, "PPU-c", "Other game", "Other", "BLES00002", {"01.00"});
    add(catalog, "PPU-d", "Global thing", "All", "All", {"All"});

    patch_map config;
    enable(config, "PPU-a", "60 FPS", "Game", "BLUS00001", "01.01");

    // Version 01.01: the two parts of "60 FPS" are one row (one part enabled), version "All" rows follow apply()
    auto rows = patches::rows_for(catalog, config, "BLUS00001", "01.01");
    CHECK(rows.size() == 4);
    CHECK(rows[0].description == "30 FPS cap" && rows[0].version_key == "All");
    CHECK(rows[1].description == "60 FPS" && rows[1].parts.size() == 2 && rows[1].enabled_parts == 1 && rows[1].partial() && !rows[1].on());
    CHECK(rows[1].author == "Someone" && rows[1].group == "fps" && rows[1].configurable == 1);
    CHECK(rows[2].description == "Skip intro");
    CHECK(rows[3].description == "Global thing" && rows[3].global() && rows[3].serial_key == "All");

    // Version 01.00: only the PPU part exists
    rows = patches::rows_for(catalog, config, "BLUS00001", "01.00");
    CHECK(rows.size() == 3);
    CHECK(rows[1].description == "60 FPS" && rows[1].parts.size() == 1 && rows[1].version_key == "01.00" && rows[1].enabled_parts == 0);

    // Unknown version: every version of the serial
    rows = patches::rows_for(catalog, config, "BLUS00001", "");
    CHECK(rows.size() == 5);
    CHECK(rows[1].version_key == "01.00" && rows[2].version_key == "01.01" && rows[2].description == "60 FPS");

    // No serial (homebrew): only patches for every serial
    rows = patches::rows_for(catalog, config, "", "01.00");
    CHECK(rows.size() == 1 && rows[0].global());

    // A serial key wins over "All" even when its versions do not match (apply() does not fall back)
    patch_map both;
    add(both, "PPU-z", "Mixed", "All", "All", {"All"});
    add(both, "PPU-z", "Mixed", "All", "BLUS00001", {"01.00"});
    CHECK(patches::rows_for(both, {}, "BLUS00001", "02.00").empty());
    CHECK(patches::rows_for(both, {}, "BLUS00001", "01.00").size() == 1);
}

void test_switches()
{
    patch_map catalog;
    auto& fps = add(catalog, "PPU-a", "60 FPS", "Game", "BLUS00001", {"01.01"});
    fps.patch_group = "fps";
    fps.default_config_values["Target"].value = 60;
    add(catalog, "SPU-b", "60 FPS", "Game", "BLUS00001", {"01.01"}).patch_group = "fps";
    add(catalog, "PPU-a", "Skip intro", "Game", "BLUS00001", {"01.01"}).patch_group = "fps";
    add(catalog, "PPU-a", "Unrelated", "Game", "BLUS00001", {"01.01"});

    patches::game_patches game;
    game.serial = "BLUS00001";
    game.app_version = "01.01";
    enable(game.config, "PPU-a", "Skip intro", "Game", "BLUS00001", "01.01");
    // patch_config.yml entry of a patch that is in no patch file, with a configured value
    auto& orphan = game.config["PPU-gone"].patch_info_map["Removed"].titles["Game"]["BLUS00001"]["01.00"];
    orphan.enabled = true;
    orphan.config_values["Speed"].value = 2;
    game.rows = patches::rows_for(catalog, game.config, game.serial, game.app_version);
    patches::collect_defaults(game, catalog);
    CHECK(game.rows.size() == 3);
    CHECK(game.rows[0].description == "60 FPS" && game.rows[1].description == "Skip intro" && game.rows[1].on());
    CHECK(game.defaults.contains({"PPU-a", "60 FPS"}) && game.defaults.contains({"SPU-b", "60 FPS"}));
    CHECK(!game.defaults.contains({"PPU-gone", "Removed"}));

    // Turning "60 FPS" on switches both parts on and the other row of its group off
    const auto off = patches::set_row(game, 0, true);
    CHECK(off.size() == 1 && off[0] == 1);
    CHECK(game.rows[0].on() && !game.rows[1].on() && game.rows[1].enabled_parts == 0);
    const auto* a = patches::find_config(game.config, "PPU-a", "60 FPS", "Game", "BLUS00001", "01.01");
    const auto* b = patches::find_config(game.config, "SPU-b", "60 FPS", "Game", "BLUS00001", "01.01");
    const auto* skip = patches::find_config(game.config, "PPU-a", "Skip intro", "Game", "BLUS00001", "01.01");
    CHECK(a && a->enabled && b && b->enabled && skip && !skip->enabled);

    // A row without a group leaves the others alone
    CHECK(patches::set_row(game, 2, true).empty() && game.rows[0].on() && game.rows[2].on());
    CHECK(patches::set_row(game, 2, false).empty() && !game.rows[2].on());
    CHECK(patches::set_row(game, 99, true).empty());

    // save_config input: real defaults for known patches, a placeholder that keeps the stored values of the orphan
    const patch_map saved = patches::save_map(game);
    CHECK(saved.at("PPU-a").patch_info_map.at("60 FPS").default_config_values.size() == 1);
    CHECK(saved.at("PPU-a").patch_info_map.at("60 FPS").default_config_values.at("Target").value == 60);
    const auto& removed = saved.at("PPU-gone").patch_info_map.at("Removed");
    const auto& removed_values = removed.titles.at("Game").at("BLUS00001").at("01.00");
    CHECK(removed_values.enabled && removed_values.config_values.at("Speed").value == 2);
    CHECK(!removed.default_config_values.empty() && removed.default_config_values != removed_values.config_values);

    // Read-back check
    CHECK(patches::row_saved(game.config, game.rows[0]));
    CHECK(patches::row_saved(game.config, game.rows[1]));
    patch_map stale = game.config;
    enable(stale, "SPU-b", "60 FPS", "Game", "BLUS00001", "01.01", false);
    CHECK(!patches::row_saved(stale, game.rows[0]));
    CHECK(!patches::row_saved({}, game.rows[0]));
    CHECK(patches::row_saved({}, game.rows[1])); // off and absent are the same
}

namespace rec = utils::ps4::recommended;

constexpr const char* gowa_spu = "SPU-7cd211ff1cbd33163eb0711440dccbb3c1dbcf6c";
constexpr const char* gowa_spu_name = "Disable SPU MLAA - God of War: Ascension (01)";
constexpr const char* gowa_104 = "PPU-3a63ecc6e9b81a984d2fe0a481febc3e26b57810";
constexpr const char* gowa_112 = "PPU-3a0b43e4a5f4bfea64f53612ee7c5d990f88129c";

void test_recommended_table()
{
    // Tier A: only for the serials the entry names
    CHECK(rec::tier_for(gowa_spu, gowa_spu_name, "BCUS98232") == rec::tier::automatic);
    CHECK(rec::tier_for(gowa_spu, gowa_spu_name, "BCES01741") == rec::tier::automatic);
    CHECK(rec::tier_for(gowa_spu, gowa_spu_name, "BLUS30443") == rec::tier::none);
    CHECK(rec::tier_for(gowa_spu, gowa_spu_name, "") == rec::tier::none);
    CHECK(rec::tier_for(gowa_104, "Disable MLAA", "BCUS98232") == rec::tier::automatic);
    CHECK(rec::tier_for(gowa_112, "Disable MLAA", "NPUA80918") == rec::tier::automatic);
    CHECK(rec::tier_for("PPU-19724fde16a5b111b7b4d2a065f5dccaf8e01962", "Disable MLAA", "BCUS98111") == rec::tier::automatic);
    CHECK(rec::tier_for("SPU-2239af4827b17317522bd6323c646b45b34ebf14", "Disable SPU MLAA", "BCES00510") == rec::tier::automatic);
    // The same SPU hash holds a serial-pinned and a global description: each keeps its own games
    CHECK(rec::tier_for("SPU-9001b44fd7278b5a6fa5385939fe928a0e549394", "Disable SPU MLAA", "BCUS98227") == rec::tier::automatic);
    CHECK(rec::tier_for("SPU-9001b44fd7278b5a6fa5385939fe928a0e549394", "Disable SPU MLAA", "NPUA80523") == rec::tier::none);
    CHECK(rec::tier_for("SPU-9001b44fd7278b5a6fa5385939fe928a0e549394", "Disable SPU MLAA - Multiple titles (01)", "NPUA80523") == rec::tier::automatic);
    // Tier B: marked for any game, never automatic; unknown descriptions of a listed hash are not recommended
    CHECK(rec::tier_for(gowa_104, "Disable Motion Blur", "BCUS98232") == rec::tier::suggested);
    CHECK(rec::tier_for(gowa_104, "Disable Depth of Field", "") == rec::tier::suggested);
    CHECK(rec::tier_for("PPU-9df60dc1aa5005a0c80e9066e4951dc0471553e6", "Enable GPU Lighting", "BCUS98174") == rec::tier::suggested);
    CHECK(rec::tier_for(gowa_104, "Unlock FPS", "BCUS98232") == rec::tier::none);
    CHECK(rec::tier_for(gowa_104, "Skip intro", "BCUS98232") == rec::tier::none);
    CHECK(rec::tier_for("PPU-0000", "Disable MLAA", "BCUS98232") == rec::tier::none);

    // Every automatic entry names its games (they gate the patch-file read); hashes are PPU-/SPU- + 40 hex digits
    for (const auto& e : rec::table)
    {
        CHECK(!e.hashes.empty() && !e.descriptions.empty() && *utils::ps4::i18n::tr(e.why));
        CHECK(e.level != rec::tier::automatic || !e.serials.empty());
        CHECK(e.level != rec::tier::suggested || e.serials.empty());
        for (const auto hash : e.hashes)
        {
            CHECK(hash.size() == 44 && (hash.starts_with("PPU-") || hash.starts_with("SPU-")));
            CHECK(std::all_of(hash.begin() + 4, hash.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }));
        }
        for (const auto serial : e.serials)
        {
            CHECK(serial.size() == 9);
            CHECK(std::all_of(serial.begin(), serial.begin() + 4, [](char c) { return c >= 'A' && c <= 'Z'; }));
            CHECK(std::all_of(serial.begin() + 4, serial.end(), [](char c) { return c >= '0' && c <= '9'; }));
        }
    }

    CHECK(rec::listed("BCUS98232") && rec::listed("BCUS98111") && rec::listed("NPUA80662"));
    CHECK(!rec::listed("BLUS30443") && !rec::listed(""));
    // A game with only suggested patches (Uncharted 2) does not read the patch files for them
    CHECK(!rec::listed("BCUS98123"));
}

// The boot gate per configuration mode: only the automatic configuration (not user-tuned, not with automatic
// configuration switched off, not with keep-user-settings.flag), without the game's marker and with a patch file.
void test_recommended_gate()
{
    enum class mode { automatic, user_tuned, auto_config_off, keep_user_settings };
    for (const mode m : {mode::automatic, mode::user_tuned, mode::auto_config_off, mode::keep_user_settings})
    {
        const bool automatic = m == mode::automatic;
        CHECK(rec::candidate("BCUS98232", automatic, false, true) == automatic);
        CHECK(!rec::candidate("BCUS98232", automatic, true, true));
        CHECK(!rec::candidate("BCUS98232", automatic, false, false));
        CHECK(!rec::candidate("BLUS30443", automatic, false, true));
        CHECK(!rec::candidate("", automatic, false, true));
    }
}

// Finds the (title, serial key, version key) of a switch returned by boot_slot.
bool keys_of(const patch_engine::patch_titles& titles, const patch_engine::patch_config_values* slot, std::string& title, std::string& serial, std::string& version)
{
    for (const auto& [t, serials] : titles)
        for (const auto& [s, versions] : serials)
            for (const auto& [v, values] : versions)
                if (&values == slot)
                {
                    title = t;
                    serial = s;
                    version = v;
                    return true;
                }
    return false;
}

void test_recommended_slot()
{
    patch_map catalog;
    // GoW Ascension: the PPU patch per executable version, the SPU patch for every serial and version
    add(catalog, gowa_104, "Disable MLAA", "God of War: Ascension", "BCUS98232", {"01.04"});
    add(catalog, gowa_104, "Disable MLAA", "God of War: Ascension", "BCES01741", {"01.04"});
    add(catalog, gowa_112, "Disable MLAA", "God of War: Ascension", "BCUS98232", {"01.12"});
    add(catalog, gowa_spu, gowa_spu_name, "All", "All", {"All"});
    // A patch listed for the serial (other versions) and for every serial under one title: apply() takes the serial
    // key and does not fall back to "All" when the version does not match
    add(catalog, "PPU-z", "Mixed", "All", "All", {"All"});
    add(catalog, "PPU-z", "Mixed", "All", "BCUS98232", {"01.00"});
    // The same under two titles: the second title's "All" switch is found
    add(catalog, "PPU-y", "Split", "All", "All", {"All"});
    add(catalog, "PPU-y", "Split", "Game", "BCUS98232", {"01.00"});

    struct sample { const char* serial; const char* version; };
    for (const auto& [serial, version] : {sample{"BCUS98232", "01.04"}, sample{"BCUS98232", "01.12"}, sample{"BCUS98232", "01.00"},
        sample{"BCES01741", "01.04"}, sample{"BCES01741", "01.12"}, sample{"BLUS30443", "01.00"}})
    {
        const auto rows = patches::rows_for(catalog, {}, serial, version);
        for (auto& [hash, container] : catalog)
        {
            for (auto& [description, info] : container.patch_info_map)
            {
                bool already_on = true;
                const auto* slot = rec::boot_slot(info.titles, serial, version, patch_key::all, already_on);
                CHECK(!already_on);
                // The XMB row of this patch (same choice as the boot) exists exactly when the boot finds a switch
                const auto row = std::find_if(rows.begin(), rows.end(), [&](const patches::patch_row& r)
                {
                    return r.description == description && std::find_if(r.parts.begin(), r.parts.end(), [&](const patches::row_part& p) { return p.hash == hash; }) != r.parts.end();
                });
                CHECK((slot != nullptr) == (row != rows.end()));
                if (!slot || row == rows.end()) continue;
                std::string title, serial_key, version_key;
                CHECK(keys_of(info.titles, slot, title, serial_key, version_key));
                CHECK(serial_key == row->serial_key && version_key == row->version_key);
            }
        }
    }

    // A switch already on (patch_config.yml) is reported, the first switch is still returned
    auto& info = catalog[gowa_104].patch_info_map["Disable MLAA"];
    info.titles["God of War: Ascension"]["BCUS98232"]["01.04"].enabled = true;
    bool already_on = false;
    const auto* slot = rec::boot_slot(info.titles, "BCUS98232", "01.04", patch_key::all, already_on);
    CHECK(slot && slot->enabled && already_on);
    CHECK(!rec::boot_slot(info.titles, "BCUS98232", "01.12", patch_key::all, already_on) && !already_on);

    // What the boot does: switch the slot on in memory; apply() then sees it enabled
    auto& spu = catalog[gowa_spu].patch_info_map[gowa_spu_name];
    auto* spu_slot = rec::boot_slot(spu.titles, "BCUS98232", "01.00", patch_key::all, already_on);
    CHECK(spu_slot && !already_on);
    spu_slot->enabled = true;
    CHECK(spu.titles["All"]["All"]["All"].enabled);
}

void test_recommended_refusals()
{
    const std::string line = rec::off_line("BCUS98232", gowa_spu, gowa_spu_name);
    CHECK(line == std::string("BCUS98232|") + gowa_spu + "|" + gowa_spu_name);

    // Round trip: added once, kept with other lines, removed again; CRLF and trailing spaces written by hand still match
    std::string content = rec::with_line("", line, true);
    CHECK(content.starts_with("#") && rec::refused(content, "BCUS98232", gowa_spu, gowa_spu_name));
    CHECK(!rec::refused(content, "BCES01741", gowa_spu, gowa_spu_name));
    CHECK(rec::with_line(content, line, true) == content);
    const std::string other = rec::off_line("BCUS98111", "SPU-2239af4827b17317522bd6323c646b45b34ebf14", "Disable SPU MLAA");
    content = rec::with_line(content, other, true);
    CHECK(rec::refused(content, "BCUS98111", "SPU-2239af4827b17317522bd6323c646b45b34ebf14", "Disable SPU MLAA"));
    content = rec::with_line(content, line, false);
    CHECK(!rec::refused(content, "BCUS98232", gowa_spu, gowa_spu_name));
    CHECK(rec::refused(content, "BCUS98111", "SPU-2239af4827b17317522bd6323c646b45b34ebf14", "Disable SPU MLAA"));
    CHECK(rec::refused("# x\r\n" + line + "  \r\n", "BCUS98232", gowa_spu, gowa_spu_name));
    CHECK(!rec::refused("x" + line, "BCUS98232", gowa_spu, gowa_spu_name));
    CHECK(!rec::refused("", "BCUS98232", gowa_spu, gowa_spu_name));

    // The XMB list of the game: rows marked, switched through the refusal list
    patch_map catalog;
    add(catalog, gowa_104, "Disable MLAA", "God of War: Ascension", "BCUS98232", {"01.04"});
    add(catalog, gowa_104, "Disable Motion Blur", "God of War: Ascension", "BCUS98232", {"01.04"});
    add(catalog, gowa_104, "Unlock FPS", "God of War: Ascension", "BCUS98232", {"01.04"});
    add(catalog, gowa_spu, gowa_spu_name, "All", "All", {"All"});
    patches::game_patches game;
    game.serial = "BCUS98232";
    game.app_version = "01.04";
    game.rows = patches::rows_for(catalog, game.config, game.serial, game.app_version);
    CHECK(game.rows.size() == 4);
    patches::mark_recommended(game, true);
    const auto row_named = [&](const char* name) -> patches::patch_row&
    {
        for (auto& row : game.rows)
            if (row.description == name) return row;
        return game.rows[0];
    };
    auto& mlaa = row_named("Disable MLAA");
    auto& blur = row_named("Disable Motion Blur");
    auto& fps = row_named("Unlock FPS");
    auto& spu = row_named(gowa_spu_name);
    CHECK(mlaa.recommendation == rec::tier::automatic && mlaa.auto_on && mlaa.active() && !mlaa.on() && !mlaa.why.empty());
    CHECK(spu.recommendation == rec::tier::automatic && spu.auto_on && spu.global());
    CHECK(blur.recommendation == rec::tier::suggested && !blur.auto_on && !blur.active());
    CHECK(fps.recommendation == rec::tier::none && !fps.auto_on && fps.why.empty());
    CHECK(patches::auto_managed(game, mlaa) && !patches::auto_managed(game, blur) && !patches::auto_managed(game, fps));

    // Off: one line per recommended part, nothing for other rows; on removes it again
    const std::size_t index = static_cast<std::size_t>(&mlaa - game.rows.data());
    game.refusals = patches::refusals_after(game, index, false);
    CHECK(rec::refused(game.refusals, "BCUS98232", gowa_104, "Disable MLAA"));
    CHECK(!rec::refused(game.refusals, "BCUS98232", gowa_spu, gowa_spu_name));
    patches::mark_recommended(game, true);
    CHECK(!mlaa.auto_on && !mlaa.active() && spu.auto_on);
    game.refusals = patches::refusals_after(game, index, true);
    CHECK(!rec::refused(game.refusals, "BCUS98232", gowa_104, "Disable MLAA"));
    patches::mark_recommended(game, true);
    CHECK(mlaa.auto_on);
    // Suggested and plain rows never write a refusal
    CHECK(patches::refusals_after(game, static_cast<std::size_t>(&blur - game.rows.data()), false) == game.refusals);
    CHECK(patches::refusals_after(game, 99, false) == game.refusals);

    // Not the automatic configuration (or the game's marker): marked, never on by themselves, switched as plain rows
    patches::mark_recommended(game, false);
    CHECK(mlaa.recommendation == rec::tier::automatic && !mlaa.auto_on && !mlaa.active() && !patches::auto_managed(game, mlaa));

    // Another game: the global SPU row is neither recommended nor on
    patches::game_patches unrelated;
    unrelated.serial = "BLUS30443";
    unrelated.app_version = "01.00";
    unrelated.rows = patches::rows_for(catalog, unrelated.config, unrelated.serial, unrelated.app_version);
    patches::mark_recommended(unrelated, true);
    CHECK(unrelated.rows.size() == 1 && unrelated.rows[0].recommendation == rec::tier::none && !unrelated.rows[0].auto_on);
}
} // namespace

int main()
{
    test_text();
    test_json();
    test_import_naming();
    test_import_sources();
    test_import_result();
    test_curl_text();
    test_languages();
    test_rows();
    test_switches();
    test_recommended_table();
    test_recommended_gate();
    test_recommended_slot();
    test_recommended_refusals();
    if (g_failures)
    {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("patches_test: all checks passed\n");
    return 0;
}
