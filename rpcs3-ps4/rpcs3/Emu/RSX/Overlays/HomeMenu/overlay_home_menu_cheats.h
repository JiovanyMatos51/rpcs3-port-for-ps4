#pragma once

#include "overlay_home_menu_page.h"

namespace rsx
{
	namespace overlays
	{
		// PS4: the switches of the running game's cheats (Emu/ps4_cheat_engine.h). X switches a cheat at once; a
		// cheat that changes code says "(próximo início)". Built each time the home menu opens.
		struct home_menu_cheats : public home_menu_page
		{
			home_menu_cheats(s16 x, s16 y, u16 width, u16 height, bool use_separators, home_menu_page* parent);

			// The running game has at least one cheat that can be switched
			static bool available();
		};
	}
}
