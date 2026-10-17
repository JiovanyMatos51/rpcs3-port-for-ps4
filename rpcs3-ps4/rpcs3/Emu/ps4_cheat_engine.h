#pragma once

// Cheats while a game runs (PS4 only). Formats, files and the op programs: util/ps4_cheats.hpp.
//   boot   code cheats (and data cheats set to "no início") are written into the main executable while it loads,
//          right after the patch engine, so PPU analysis and LLVM compile them in;
//   live   constant and once cheats are run by tick() from the PS4 Main Thread loop (no thread of its own): every
//          access through vm::try_access (range-locked, page-checked, atomic for aligned 1/2/4/8/16 bytes) plus
//          vm::ps4_touch_reservations, within 4096 accesses / 2 ms per pass. Game code is never written live under
//          PPU LLVM; RSX local memory, the RSX context, stacks and SPU are refused.

#include "util/types.hpp"

#include <cstddef>
#include <string>
#include <vector>

struct lv2_obj;
template <typename T>
struct ppu_module;

namespace ps4_cheats
{
// ppu_load_exec, after both patch_engine::apply calls (a cheat wins over a patch on the same bytes). Reads the
// game's cheat files, records the executable in <TITLE_ID>.seen, writes the boot part of the cheats that are on and
// pushes the written code words into 'applied'. from_savestate: memory comes from the savestate, nothing is written.
void boot_apply(std::vector<u32>& applied, const ppu_module<lv2_obj>& main, bool from_savestate);

// PS4 Main Thread loop, every pass (now_us: sceKernelGetProcessTime()). Returns at once without live cheats, while
// the game is not running or the app is in the background.
void tick(u64 now_us) noexcept;

// The emulator stopped
void reset() noexcept;

// Home menu (RSX overlay thread): one row per cheat that can be switched in game
struct live_row
{
    std::string label;      // "[x] Max money", "[ ] Infinite health (próximo início)"
    bool on = false;
    bool next_boot_only = false;
};
std::vector<live_row> rows();
// Switches row 'index'; the state file is written on the main thread. Returns a line for the log.
std::string toggle(std::size_t index);
} // namespace ps4_cheats
