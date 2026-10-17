#include "stdafx.h"
#include "overlay_home_menu_cheats.h"
#include "overlay_home_menu_components.h"
#include "Emu/ps4_cheat_engine.h"
#include "util/ps4_i18n.hpp"

namespace rsx
{
	namespace overlays
	{
		namespace
		{
			std::string page_title()
			{
				namespace i18n = utils::ps4::i18n;
				// the overlay font has no Arabic shaping (as ps4_localized_string)
				const i18n::lang l = i18n::current() == i18n::lang::ar ? i18n::lang::en : i18n::current();
				return i18n::tr(i18n::str::CHT_HOME_TITULO, l);
			}

			// A home-menu row whose text follows the cheat's switch
			struct cheat_entry : public horizontal_layout
			{
				label* text = nullptr;

				cheat_entry(std::string_view caption, u16 width)
				{
					auto stack = std::make_unique<vertical_layout>();
					auto padding = std::make_unique<spacer>();
					auto title = std::make_unique<label>(caption);

					padding->set_size(1, 1);
					title->set_size(width, menu_entry_height);
					title->set_font(14);
					title->set_wrap_text(true);
					title->align_text(text_align::left);
					title->back_color.a = 0.f;

					stack->pack_padding = 8;
					stack->add_element(padding);
					text = stack->add_element(title);
					add_element(stack);
				}
			};
		}

		bool home_menu_cheats::available()
		{
			return !ps4_cheats::rows().empty();
		}

		home_menu_cheats::home_menu_cheats(s16 x, s16 y, u16 width, u16 height, bool use_separators, home_menu_page* parent)
			: home_menu_page(x, y, width, height, use_separators, parent, page_title())
		{
			const std::vector<ps4_cheats::live_row> list = ps4_cheats::rows();

			for (usz i = 0; i < list.size(); i++)
			{
				auto entry = std::make_unique<cheat_entry>(list[i].label, width);
				label* const text = entry->text;
				std::unique_ptr<overlay_element> element = std::move(entry);

				add_item(element, [i, text](pad_button btn) -> page_navigation
				{
					if (btn != pad_button::cross) return page_navigation::stay;

					rsx_log.notice("%s", ps4_cheats::toggle(i));

					if (const std::vector<ps4_cheats::live_row> now = ps4_cheats::rows(); i < now.size())
					{
						text->set_text(now[i].label);
					}

					return page_navigation::stay;
				});
			}

			apply_layout();
		}
	}
}
