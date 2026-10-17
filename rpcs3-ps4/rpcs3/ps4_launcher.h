#pragma once

#include <cstddef>
#include <deque>
#include <string>
#include <vector>

namespace utils::ps4 { struct signal_context; }

// 0.31: same as ps4_pads::initialize() (ps4_pads.h), which owns UserService and every pad handle.
bool ps4_initialize_pad();
void ps4_request_shutdown();
void ps4_request_return_to_menu();
bool ps4_shutdown_requested();
std::string ps4_select_game(const std::string& preferred, const std::string& error = {});

// 0.38: 'error' texts of ps4_select_game that also open a PS3 system dialog over the XMB (prefix match; the parts after
// " · " become the dialog's notes). 0.39: these pt-BR starts are the plain form of the notices; the launcher also reads
// the encoded form of util/ps4_xmb_notice.hpp and shows both in the interface language (LAUH_* texts).
// The clean-process rule refused a boot (ps4_main.cpp): close the application and open it again; the game is written
// to /data/rpcs3/boot-next.txt and boots by itself at the next start.
inline constexpr char ps4_restart_notice[] = "Para jogar, feche o RPCS3 e abra de novo.";
// The application was opened again for the game of boot-next.txt (" · <game name>" follows; 'preferred' is its path):
// it boots when the countdown of ps4_boot_next_seconds ends or on X; Circle cancels and leaves the XMB open.
inline constexpr char ps4_boot_next_notice[] = "O RPCS3 foi reaberto para jogar";
inline constexpr unsigned ps4_boot_next_seconds = 5;
// Flag of an encoded text notice (util/ps4_xmb_notice.hpp, between the kind and the separator): the XMB shows it as a
// system dialog (a failed boot, an incomplete ISO). The first " · " part is the message, the others are the steps that
// fix it; the game of 'preferred' names the dialog. The message line keeps the first part.
inline constexpr char ps4_notice_flag_dialog = 'd';

// 0.34: outcome of ps4_install_packages.
struct ps4_install_result
{
    bool ok = false;                  // every package installed
    bool display_unavailable = false; // the installer screen could not open: nothing was installed
    bool cancelled = false;           // Circle on the installer screen
    bool no_space = false;            // a file could not be written completely: /data is full
    std::string message;              // one XMB line in the interface language
    std::string select;               // 0.37: XMB row to open on (base game, installed game, or the PKG); "" = none
};

// 0.34: installs PS3 packages in order on the installer screen (summed progress, Circle cancels), like the PC
// build's multi-package install. 'from_disc': packages of the booting disc (PKGDIR/INSDIR/PS3_EXTRA; also recognized
// by those folder names in the paths), extracted with one thread and titled "Instalando conteúdo do disco i/n". The
// paths of the installed USRDIR/EBOOT.BIN files are appended to 'bootables' (an empty entry for a package without
// one). Main thread only (VideoOut and the pads); call it while no game renderer exists. With display_unavailable set
// nothing was installed and the caller may fall back to a plain install.
ps4_install_result ps4_install_packages(const std::vector<std::string>& paths, bool from_disc,
    std::deque<std::string>* bootables = nullptr);
// One package chosen in the XMB; returns the message line.
std::string ps4_install_package(const std::string& path);
// 0.37: called right before BootGame. "" when 'path' may boot; otherwise the XMB line asking for the PS3 disc of a game
// installed from it (the drive is read unless the XMB verified it just now).
std::string ps4_disc_lock_boot_check(const std::string& path);
// 0.39: true for an ISO of a disc game (CATEGORY DG) without a valid PS3_DISC.SFB, which the boot refuses as an invalid
// file or folder (e.g. a PSN game converted to an ISO). Opens the image: its directory tree and 4 bytes of the file
// (of an encrypted image without a working key only the directory counts).
bool ps4_iso_without_sfb(const std::string& path);
void ps4_prepare_game_directories();
void ps4_startup_checkpoint(const char* stage);
void ps4_startup_main_entered();
// Rotates startup.log and fatal.txt into previous-* once per process (idempotent).
void ps4_rotate_session_logs();
void ps4_trace_memory(const char* operation, const void* requested, std::size_t size,
    int flags, int result, const void* mapped);
void ps4_trace_fault(const utils::ps4::signal_context& context, const void* address, bool unhandled = false);
