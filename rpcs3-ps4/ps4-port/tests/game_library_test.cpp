#include "../../rpcs3/util/ps4_game_library.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>

int main(int argc, char** argv)
{
    assert(argc == 2);
    namespace fs = std::filesystem;
    using namespace utils::ps4;
    const fs::path root(argv[1]);
    assert(!fs::exists(root)); // Never remove an existing user directory.
    fs::create_directories(root);
    const auto base = root.generic_string();
    auto file = [&](const std::string& name)
    {
        const auto path = root / name;
        fs::create_directories(path.parent_path());
        std::ofstream stream(path, std::ios::binary);
        stream << "library fixture";
        assert(stream.good());
    };
    file("Disc/PS3_GAME/USRDIR/EBOOT.BIN");
    file("Disc/PS3_GAME/USRDIR/INTERNAL.ELF");
    file("Digital/USRDIR/EBOOT.BIN");
    file("Groups/Racing.pkg/PS3_GAME/USRDIR/EBOOT.BIN");
    file("Groups/Nested/Upper.ISO");
    file("Groups/Nested/Update.PkG");
    file("Homebrew.SELF");
    file("demo.ElF");
    file("Disk.iso");
    file("Install.pkg");
    file("Disk.iso.filepart");
    file("Install.pkg.part");
    file(".hidden.iso");
    file(".hidden/Disk.iso");
    file("NotAGame/README.txt");
    file("Too/Deep/Outside/Skip.iso");
    auto enumerate = [](const std::string& directory)
    {
        std::vector<library_item> items;
        std::error_code error;
        for (const auto& entry : fs::directory_iterator(directory, error))
            items.push_back({entry.path().filename().generic_string(), entry.is_directory(), entry.is_symlink()});
        return items;
    };
    auto is_file = [](const std::string& path) { return fs::is_regular_file(path); };
    const std::vector<std::string> expected = {
        base + "/Digital", base + "/Disc", base + "/Disk.iso",
        base + "/Groups/Nested/Update.PkG", base + "/Groups/Nested/Upper.ISO",
        base + "/Groups/Racing.pkg", base + "/Homebrew.SELF", base + "/Install.pkg", base + "/demo.ElF"
    };
    assert(scan_game_library({base, base + "/missing", base + "/Groups/"}, base + "/Disk.iso", enumerate, is_file) == expected);
    assert(scan_game_library({base + "/Disc"}, {}, enumerate, is_file) == std::vector<std::string>{base + "/Disc"});
    assert(file_kind("TEST.ISO") == game_kind::iso);
    assert(file_kind("TEST.PkG") == game_kind::package);
    assert(file_kind("TEST.iso.filepart") == game_kind::none);
    assert(file_kind("TEST.pkg.sha256") == game_kind::none);
    // Even if the host cannot create symlinks without special permissions,
    // an enumerated directory link must never be traversed.
    assert(scan_game_library({"/mock"}, {}, [](const std::string&) {
        return std::vector<library_item>{{"link", true, true}, {"link.iso", false, true}};
    }, [](const std::string&) { return false; }).empty());

    // 0.34: a library folder that is itself a game still lists the games next to its PS3_GAME, never its internals.
    file("Copied/PS3_GAME/USRDIR/EBOOT.BIN");
    file("Copied/PS3_GAME/USRDIR/Inside.self");
    file("Copied/PS3_DISC.SFB");
    file("Copied/PS3_UPDATE/Update.pkg");
    file("Copied/USRDIR/Data.iso");
    file("Copied/Other.iso");
    file("Copied/Nested/Inner/PS3_GAME/USRDIR/EBOOT.BIN");
    assert(scan_game_library({base + "/Copied"}, {}, enumerate, is_file) ==
        (std::vector<std::string>{base + "/Copied", base + "/Copied/Nested/Inner", base + "/Copied/Other.iso"}));
    // Below the library folder game folders stay terminal (the first scan above lists Disc alone).
    assert(scan_game_library({base + "/Disc"}, {}, enumerate, is_file) == std::vector<std::string>{base + "/Disc"});

    // 0.34: only the first part of a split PKG is listed (any case); a part without its _00 stays visible.
    file("Parts/Game_00.PKG");
    file("Parts/Game_01.pkg");
    file("Parts/game_02.PKG");
    file("Parts/Solo_01.pkg");
    file("Parts/Other.pkg");
    assert(scan_game_library({base + "/Parts"}, {}, enumerate, is_file) ==
        (std::vector<std::string>{base + "/Parts/Game_00.PKG", base + "/Parts/Other.pkg", base + "/Parts/Solo_01.pkg"}));
    assert(package_part("A/B_07.PkG") == 7 && package_part("A/B_00.pkg") == 0 && package_part("A/B_7.pkg") == -1 && package_part("A/B_07.iso") == -1);

    // 0.34: dev_hdd0/game: one level, folders only; data folders are not listed or walked.
    file("Hdd0/BLUS00001/USRDIR/EBOOT.BIN");
    file("Hdd0/BLUS00001-DATA/USRDIR/Stray.iso");
    file("Hdd0/Loose.pkg");
    unsigned listings = 0;
    auto counting = [&](const std::string& directory) { ++listings; return enumerate(directory); };
    assert(scan_game_library_roots({{base + "/Hdd0", 1, accept_folder}}, {}, counting, is_file) == std::vector<std::string>{base + "/Hdd0/BLUS00001"});
    assert(listings == 1);

    // 0.34: USB drive: ISO and PKG at the root, PACKAGES folder, a layout folder that is itself a game.
    file("Usb/PS3ISO/Group/A.iso");
    file("Usb/PACKAGES/P.pkg");
    file("Usb/Root.iso");
    file("Usb/Root.pkg");
    file("Usb/README.txt");
    file("Usb/GAMES/PS3_GAME/USRDIR/EBOOT.BIN");
    file("Usb/GAMES/Other/PS3_GAME/USRDIR/EBOOT.BIN");
    file("Usb/Unrelated/Skip.iso");
    const std::string mount = base + "/Usb";
    const usb_scan usb = scan_usb_drive(mount, enumerate(mount), enumerate, is_file);
    assert(usb.games == (std::vector<std::string>{mount + "/GAMES", mount + "/GAMES/Other", mount + "/PACKAGES/P.pkg", mount + "/PS3ISO/Group/A.iso",
        mount + "/Root.iso", mount + "/Root.pkg"}));
    assert(usb.folders == (std::vector<std::string>{"GAMES", "PACKAGES", "PS3ISO"}));
    assert(!usb.truncated);

    // 0.34: helpers.
    assert(rap_license_name("up0001-npub30554_00-0000000000000001.RAP") == "UP0001-NPUB30554_00-0000000000000001.rap");
    assert(rap_license_name("UP0001-NPUB30554_00-0000000000000001.rap") == "UP0001-NPUB30554_00-0000000000000001.rap");
    assert(rap_license_name("UP0001-NPUB30554_00-0000000000000001.rif").empty());
    assert(rap_license_name("UP0001-NPUB30554_00-000000000000001.rap").empty());
    assert(rap_license_name("UP0001-NPUB30554_00-00000000000 0001.rap").empty());
    assert(rap_license_name("license.rap").empty());
    assert(internal_library_folder("games") && internal_library_folder("PS3ISO") && internal_library_folder("Packages") && internal_library_folder("isos"));
    assert(!internal_library_folder("cache") && !internal_library_folder("raps") && !internal_library_folder("dev_hdd0"));
    assert(game_internal_entry("ps3_gm01", true) && game_internal_entry("PS3_GAME", true) && game_internal_entry("TROPDIR", true));
    assert(!game_internal_entry("PS3_GAME", false) && game_internal_entry("PS3_DISC.SFB", false) && !game_internal_entry("Other", true));
    assert(usb_layout_folder("packages") && usb_layout_folder("PS3ISO") && !usb_layout_folder("Unrelated"));
    assert(library_root_is_game("/data/rpcs3/games") && library_root_is_game("/mnt/usb1/GAMES"));
    assert(!library_root_is_game("/data/rpcs3/games/Name") && !library_root_is_game("/mnt/usb1/GAMES/Name") && !library_root_is_game("/mnt/usb1/Root.iso"));
    fs::remove_all(root); // Exact newly created test fixture only.
}
