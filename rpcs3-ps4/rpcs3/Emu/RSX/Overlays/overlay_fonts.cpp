#include "stdafx.h"
#include "overlay_fonts.h"
#include "Emu/emu_callbacks.h"
#include "Emu/vfs_config.h"

#ifndef _WIN32
#include <unistd.h>
#include <sys/types.h>
#include <pwd.h>

#if defined(__DragonFly__) || defined(__FreeBSD__) || defined(__NetBSD__)
#include <sys/sysctl.h>
#endif
#endif

#ifdef RPCS3_PS4
#include "util/ps4_hle_memory.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#endif

namespace rsx
{
	namespace overlays
	{
#ifdef RPCS3_PS4
		namespace
		{
			// 0.38: font list, page creation, the page cap and get_glyph_data. Recursive: initialize_codepage measures
			// 'm' through get_char.
			std::recursive_mutex g_ps4_font_mutex;
			u64 g_ps4_font_tick = 0;
			u32 g_ps4_pages_held = 0;    // pages whose 1 MiB bitmap is held
			u32 g_ps4_pages_peak = 0;    // since the last overlay teardown
			u64 g_ps4_pages_dropped = 0; // process totals
			u64 g_ps4_pages_rebuilt = 0;
			u32 g_ps4_font_lines = 0;    // 'fonts:' lines of this process outside the teardown (at most 16)

			// Copy of all pages of the font being uploaded (get_glyph_data); freed by font::ps4_release_upload_copy
			thread_local std::vector<u8> t_ps4_upload_copy;

			u32 ps4_page_cap()
			{
				namespace hle = utils::ps4::hle_memory;
				static const bool unbounded = hle::flag(hle::font_pages_flag);
				return hle::font_page_cap(unbounded ? 0 : hle::level(), unbounded);
			}

			void ps4_font_line(const char* text)
			{
				if (g_ps4_font_lines++ < 16)
				{
					ps4_startup_checkpoint(text);
				}
			}

			// Packs a page whose bitmap was dropped into 'dst' (one page) again, with the file, size and block it was
			// first packed from. 'ttf'/'ttf_path' keep the last font file read for the next page of the same upload.
			void ps4_rebuild_page(const codepage& page, u8* dst, std::vector<u8>& ttf, std::string& ttf_path)
			{
				constexpr u32 page_size = codepage::bitmap_width * codepage::bitmap_height;

				if (ttf.empty() || ttf_path != page.ps4_source)
				{
					ttf.clear();
					ttf_path = page.ps4_source;

					if (fs::file f{ttf_path}; !f || !f.read(ttf, f.size()))
					{
						ttf.clear();
					}
				}

				std::vector<stbtt_packedchar> pack(codepage::char_count);
				stbtt_pack_context context;

				if (ttf.empty() || !stbtt_PackBegin(&context, dst, codepage::bitmap_width, codepage::bitmap_height, 0, 1, nullptr))
				{
					std::memset(dst, 0, page_size);
					rsx_log.error("Font page 0x%x could not be packed again from '%s'", static_cast<u32>(page.ps4_codepage_id), page.ps4_source);
					return;
				}

				stbtt_PackSetOversampling(&context, codepage::oversample, codepage::oversample);
				const bool packed = stbtt_PackFontRange(&context, ttf.data(), 0, page.ps4_font_size, page.ps4_codepage_id * 256, codepage::char_count, pack.data()) != 0;
				stbtt_PackEnd(&context);

				if (!packed || page.pack_info.size() != pack.size() || std::memcmp(pack.data(), page.pack_info.data(), sizeof(stbtt_packedchar) * pack.size()) != 0)
				{
					rsx_log.error("Font page 0x%x packed again from '%s' with a different layout (packed=%d)", static_cast<u32>(page.ps4_codepage_id), page.ps4_source, packed);
				}
			}
		}
#endif

		void codepage::initialize_glyphs(char32_t codepage_id, f32 font_size, const std::vector<u8>& ttf_data)
		{
			glyph_base = (codepage_id * 256);
			glyph_data.resize(bitmap_width * bitmap_height);
			pack_info.resize(256);

			stbtt_pack_context context;
			if (!stbtt_PackBegin(&context, glyph_data.data(), bitmap_width, bitmap_height, 0, 1, nullptr))
			{
				rsx_log.error("Font packing failed");
				return;
			}

			stbtt_PackSetOversampling(&context, oversample, oversample);

			if (!stbtt_PackFontRange(&context, ttf_data.data(), 0, font_size, (codepage_id * 256), 256, pack_info.data()))
			{
				rsx_log.error("Font packing failed");
				stbtt_PackEnd(&context);
				return;
			}

			stbtt_PackEnd(&context);
		}

		stbtt_aligned_quad codepage::get_char(char32_t c, f32& x_advance, f32& y_advance)
		{
			stbtt_aligned_quad quad;
			stbtt_GetPackedQuad(pack_info.data(), bitmap_width, bitmap_height, (c - glyph_base), &x_advance, &y_advance, &quad, false);

			quad.t0 += sampler_z;
			quad.t1 += sampler_z;
			return quad;
		}

		font::font(std::string_view ttf_name, f32 size)
		{
			// Convert pt to px
			size_px = ceilf(size * 96.f / 72.f);
			size_pt = size;

			font_name = std::string(ttf_name);
			initialized = true;
		}

		language_class font::classify(char32_t codepage_id)
		{
			switch (codepage_id)
			{
			case 00: // Extended ASCII
			case 04: // Cyrillic
			{
				return language_class::default_;
			}
			case 0x11: // Hangul jamo
			case 0x31: // Compatibility jamo 3130-318F
			// case 0xA9: // Hangul jamo extended block A A960-A97F
			{
				return language_class::hangul;
			}
			case 0xFF: // Halfwidth and Fullwidth Forms
			{
				// Found in SCE-PS3-SR-R-JPN.TTF, so we'll use cjk_base for now
				return language_class::cjk_base;
			}
			default:
			{
				if (codepage_id >= 0xAC && codepage_id <= 0xD7)
				{
					// Hangul syllables + jamo extended block B
					return language_class::hangul;
				}

				if (codepage_id >= 0x2E && codepage_id <= 0x9F)
				{
					// Generic CJK blocks, mostly chinese and japanese kana
					// Should typically be compatible with JPN ttf fonts
					return language_class::cjk_base;
				}

				// TODO
				return language_class::default_;
			}
			}
		}

		glyph_load_setup font::get_glyph_files(language_class class_) const
		{
			glyph_load_setup result;
			result.font_names.push_back(font_name);

			const std::vector<std::string> font_dirs = g_emu_callbacks.get_font_dirs();
			result.lookup_font_dirs.insert(result.lookup_font_dirs.end(), font_dirs.begin(), font_dirs.end());
			// Search dev_flash for the font too
			result.lookup_font_dirs.push_back(g_cfg_vfs.get_dev_flash() + "data/font/");
			result.lookup_font_dirs.push_back(g_cfg_vfs.get_dev_flash() + "data/font/SONY-CC/");

			switch (class_)
			{
			case language_class::default_:
			{
				result.font_names.emplace_back("Arial.ttf");
				result.font_names.emplace_back("arial.ttf");
#if defined(RPCS3_PS4)
				result.font_names.emplace_back("DejaVuSans.ttf");
#elif defined(__APPLE__)
				result.font_names.emplace_back("DejaVuSans.ttf");
				result.font_names.emplace_back("NotoSans-Regular.ttf");
				result.font_names.emplace_back("Roboto-Regular.ttf");
				result.font_names.emplace_back("OpenSans-Regular.ttf");
				result.font_names.emplace_back("FreeSans.ttf");
#elif defined(_WIN32)
				// Covers symbol blocks (e.g. Roman numerals) that plain Arial may be missing glyphs for.
				result.font_names.emplace_back("tahoma.ttf");
#else
				result.font_names.emplace_back("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"); // ubuntu
				result.font_names.emplace_back("/usr/share/fonts/TTF/DejaVuSans.ttf");             // arch
#endif
				// Attempt to load a font from dev_flash as a last resort
				result.font_names.emplace_back("SCE-PS3-VR-R-LATIN.TTF");
				break;
			}
			case language_class::cjk_base:
			{
				// Skip loading font files directly
				result.font_names.clear();

				// Attempt to load a font from dev_flash before any other source
				result.font_names.emplace_back("SCE-PS3-SR-R-JPN.TTF");
				result.font_names.emplace_back("SCE-PS3-DH-R-CGB.TTF");

				// Known system font as last fallback
				result.font_names.emplace_back("Yu Gothic.ttf");
				result.font_names.emplace_back("YuGothR.ttc");
#ifdef _WIN32
				result.font_names.emplace_back("msyh.ttc");
				result.font_names.emplace_back("simsunb.ttc");
				result.font_names.emplace_back("simsun.ttc");
				result.font_names.emplace_back("SimsunExtG.ttf");
#endif
				break;
			}
			case language_class::hangul:
			{
				// Skip loading font files directly
				result.font_names.clear();

				// Attempt to load a font from dev_flash before any other source
				result.font_names.emplace_back("SCE-PS3-YG-R-KOR.TTF");

				// Known system font as last fallback
				result.font_names.emplace_back("Malgun Gothic.ttf");
				result.font_names.emplace_back("malgun.ttf");
				break;
			}
			}

			return result;
		}

		codepage* font::initialize_codepage(char32_t c)
		{
			// Init glyph
			const auto codepage_id = get_page_id(c);
			const auto class_ = classify(codepage_id);
			const auto fs_settings = get_glyph_files(class_);

			// Attemt to load requested font
			std::vector<u8> bytes;
			std::vector<u8> fallback_bytes;
			std::string fallback_file;
			bool font_found = false;
#ifdef RPCS3_PS4
			std::string ps4_file; // file the page is packed from
#endif

			const auto get_font = [&](const std::string& file_path) -> bool
			{
				// Read font
				fs::file f(file_path);
				f.read(bytes, f.size());

				// Check if the character exists in the font
				stbtt_fontinfo info;
				if (stbtt_InitFont(&info, bytes.data(), stbtt_GetFontOffsetForIndex(bytes.data(), 0)) != 0)
				{
					font_found = stbtt_FindGlyphIndex(&info, c) != 0;
				}

#ifdef RPCS3_PS4
				if (font_found)
				{
					ps4_file = file_path;
				}
#endif

				if (!font_found)
				{
					if (fallback_bytes.empty())
					{
						// Save this font as a fallback so we don't get a segfault or exception
						fallback_bytes = std::move(bytes);
						fallback_file = file_path;
					}

					bytes.clear();
				}

				return font_found;
			};

			for (const std::string& font_file : fs_settings.font_names)
			{
				if (fs::is_file(font_file))
				{
					// Check for absolute paths or fonts 'installed' to executable folder
					if (get_font(font_file))
					{
						break;
					}

					continue;
				}

				std::string extension;
				if (const auto extension_start = font_file.find_last_of('.');
					extension_start != umax)
				{
					extension = font_file.substr(extension_start + 1);
				}

				std::string file_name = font_file;
				if (extension.length() != 3)
				{
					// Allow other extensions to support other truetype formats
					file_name += ".ttf";
				}

				for (const auto& font_dir : fs_settings.lookup_font_dirs)
				{
					const std::string file_path = font_dir + file_name;
					if (fs::is_file(file_path))
					{
						if (get_font(file_path))
						{
							break;
						}
					}
				}

				if (font_found)
				{
					break;
				}
			}

			if (!font_found)
			{
				if (fallback_bytes.empty())
				{
					fmt::throw_exception("Failed to initialize font for character 0x%x on codepage %d.\nLookup dirs:\n%s\nTarget fonts:\n%s", static_cast<u32>(c), static_cast<u32>(codepage_id), fmt::merge(fs_settings.lookup_font_dirs, "\n"), fmt::merge(fs_settings.font_names, "\n"));
				}

				rsx_log.error("Failed to initialize font for character 0x%x on codepage %d. Falling back to font '%s'", static_cast<u32>(c), static_cast<u32>(codepage_id), fallback_file);
				bytes = std::move(fallback_bytes);
#ifdef RPCS3_PS4
				ps4_file = fallback_file;
#endif
			}

			codepage_cache.page = nullptr;
			auto page = std::make_unique<codepage>();
#ifdef RPCS3_PS4
			try
			{
				page->initialize_glyphs(codepage_id, size_px, bytes);
			}
			catch (const std::bad_alloc&)
			{
				// Out of heap for the 1 MiB bitmap: drop every held bitmap (they are packed again when needed), retry once
				fontmgr::ps4_trim(nullptr, 0);
				page = std::make_unique<codepage>();
				page->initialize_glyphs(codepage_id, size_px, bytes);
			}

			page->ps4_source = std::move(ps4_file);
			page->ps4_codepage_id = codepage_id;
			page->ps4_font_size = size_px;
			page->ps4_last_use = ++g_ps4_font_tick;
#else
			page->initialize_glyphs(codepage_id, size_px, bytes);
#endif
			page->sampler_z = static_cast<f32>(m_glyph_map.size());

			auto ret = page.get();
			m_glyph_map.emplace_back(codepage_id, std::move(page));

#ifdef RPCS3_PS4
			if (!ret->glyph_data.empty())
			{
				g_ps4_pages_held++;
				g_ps4_pages_peak = std::max(g_ps4_pages_peak, g_ps4_pages_held);
			}

			fontmgr::ps4_trim(ret, ps4_page_cap());
#endif

			if (codepage_id == 0)
			{
				// Latin-1
				f32 unused;
				get_char('m', em_size, unused);
			}

			return ret;
		}

		stbtt_aligned_quad font::get_char(char32_t c, f32& x_advance, f32& y_advance)
		{
			if (!initialized)
				return {};

			const auto page_id = get_page_id(c);

			if (codepage_cache.codepage_id == page_id && codepage_cache.page) [[likely]]
			{
				return codepage_cache.page->get_char(c, x_advance, y_advance);
			}
			else
			{
#ifdef RPCS3_PS4
				// The page list may grow on another thread (dialogs are built on PPU threads, drawn on the RSX thread)
				std::lock_guard lock(g_ps4_font_mutex);
#endif
				codepage_cache.codepage_id = page_id;
				codepage_cache.page = nullptr;

				for (const auto& e : m_glyph_map)
				{
					if (e.first == page_id)
					{
						codepage_cache.page = e.second.get();
						break;
					}
				}

				if (!codepage_cache.page) [[unlikely]]
				{
					codepage_cache.page = initialize_codepage(c);
				}

				return codepage_cache.page->get_char(c, x_advance, y_advance);
			}
		}

		std::vector<vertex> font::render_text_ex(f32& x_advance, f32& y_advance, const char32_t* text, usz char_limit, u16 max_width, bool wrap)
		{
			x_advance = 0.f;
			y_advance = 0.f;
			std::vector<vertex> result;

			if (!initialized)
			{
				return result;
			}

			// Render as many characters as possible as glyphs.
			for (usz i = 0u, begin_of_word = 0u; i < char_limit; i++)
			{
				switch (const auto& c = text[i])
				{
				case '\0':
				{
					// We're done.
					return result;
				}
				case '\n':
				{
					// Reset x to 0 and increase y to advance to the new line.
					x_advance = 0.f;
					y_advance += size_px + 2.f;
					begin_of_word = result.size();
					continue;
				}
				case '\r':
				{
					// Reset x to 0.
					x_advance = 0.f;
					begin_of_word = result.size();
					continue;
				}
				default:
				{
					const bool is_whitespace = c == ' ';
					stbtt_aligned_quad quad{};

					if (is_whitespace)
					{
						// Skip whitespace if we are at the start of a line.
						if (x_advance <= 0.f)
						{
							// Set the glyph to the current position.
							// This is necessary for downstream linewidth calculations.
							quad.x0 = quad.x1 = x_advance;
							quad.y0 = quad.y1 = y_advance;
						}
						else
						{
							const f32 x_advance_old = x_advance;
							const f32 y_advance_old = y_advance;

							// Get the glyph size.
							quad = get_char(c, x_advance, y_advance);

							// Reset the result if the glyph would protrude out of the given space anyway.
							if (x_advance > max_width)
							{
								// Set the glyph to the previous position.
								// This is necessary for downstream linewidth calculations.
								quad.x0 = quad.x1 = x_advance_old;
								quad.y0 = quad.y1 = y_advance_old;
							}
						}
					}
					else
					{
						// No whitespace. Get the glyph size.
						quad = get_char(c, x_advance, y_advance);
					}

					// Add the glyph's vertices.
					result.emplace_back(quad.x0, quad.y0, quad.s0, quad.t0);
					result.emplace_back(quad.x1, quad.y0, quad.s1, quad.t0);
					result.emplace_back(quad.x0, quad.y1, quad.s0, quad.t1);
					result.emplace_back(quad.x1, quad.y1, quad.s1, quad.t1);

					// The next word will begin after any whitespaces. CJK/Hangul scripts don't use spaces
					// between words, so also allow breaking right after each such character.
					if (is_whitespace || classify(get_page_id(c)) != language_class::default_)
					{
						begin_of_word = result.size();
					}

					// Check if we reached the end of the available space.
					if (x_advance > max_width)
					{
						// Try to wrap the protruding text
						if (wrap)
						{
							// Increase y to advance to the next line.
							y_advance += size_px + 2.f;

							// We can just reset x and move on to the next character if this is a whitespace.
							if (is_whitespace)
							{
								x_advance = 0.f;
								break;
							}

							// Get the leftmost offset of the current word.
							const f32 base_x = result[begin_of_word].x();

							// Move all characters of the current word one line down and to the left.
							for (usz n = begin_of_word; n < result.size(); ++n)
							{
								result[n].x() -= base_x;
								result[n].y() += size_px + 2.f;
							}

							// Set x offset to the rightmost position of the current word
							x_advance = result.back().x();
						}
						else
						{
							// TODO: Ellipsize
						}
					}

					break;
				}
				} // switch
			}
			return result;
		}

		std::vector<vertex> font::render_text(const char32_t* text, u16 max_width, bool wrap)
		{
			f32 unused_x, unused_y;

			return render_text_ex(unused_x, unused_y, text, -1, max_width, wrap);
		}

		void font::get_char_offset(f32& loc_x, f32& loc_y, const char32_t* text, usz max_length, u16 max_width, bool wrap)
		{
			render_text_ex(loc_x, loc_y, text, max_length, max_width, wrap);
		}

		const std::vector<u8>& font::get_glyph_data() const
		{
			constexpr u32 page_size = codepage::bitmap_width * codepage::bitmap_height;
			const auto size = page_size * m_glyph_map.size();

#ifdef RPCS3_PS4
			std::lock_guard lock(g_ps4_font_mutex);

			std::vector<u8>& bytes = t_ps4_upload_copy;
			bytes.resize(size);

			u8* data = bytes.data();
			const u64 tick = ++g_ps4_font_tick;
			std::vector<u8> ttf;
			std::string ttf_path;
			u32 rebuilt = 0;

			for (const auto& e : m_glyph_map)
			{
				codepage& page = *e.second;

				if (!page.glyph_data.empty())
				{
					std::memcpy(data, page.glyph_data.data(), page_size);
				}
				else
				{
					// Dropped by the page cap: packed again straight into the copy, not held again
					ps4_rebuild_page(page, data, ttf, ttf_path);
					rebuilt++;
				}

				page.ps4_last_use = tick;
				data += page_size;
			}

			if (rebuilt)
			{
				g_ps4_pages_rebuilt += rebuilt;
				char line[192];
				std::snprintf(line, sizeof(line), "fonts: pages %u of '%s' %dpt uploaded, %u packed again (bitmaps held %u, cap %u)",
					::size32(m_glyph_map), font_name.c_str(), static_cast<int>(size_pt), rebuilt, g_ps4_pages_held, ps4_page_cap());
				ps4_font_line(line);
			}
#else
			static thread_local std::vector<u8> bytes;
			bytes.resize(size);

			u8* data = bytes.data();

			for (const auto& e : m_glyph_map)
			{
				std::memcpy(data, e.second->glyph_data.data(), page_size);
				data += page_size;
			}
#endif
			return bytes;
		}

#ifdef RPCS3_PS4
		void font::ps4_release_upload_copy()
		{
			std::vector<u8>().swap(t_ps4_upload_copy);
		}

		font* fontmgr::get(std::string_view name, int size)
		{
			std::lock_guard lock(g_ps4_font_mutex);

			if (m_instance == nullptr)
				m_instance = new fontmgr;

			return m_instance->find(name, size);
		}

		font* fontmgr::find(std::string_view name, int size)
		{
			for (const auto& f : fonts)
			{
				if (f->matches(name, size))
					return f.get();
			}

			fonts.push_back(std::make_unique<font>(name, static_cast<f32>(size)));
			return fonts.back().get();
		}

		void fontmgr::ps4_trim(const codepage* keep, u32 cap)
		{
			if (!m_instance)
			{
				return;
			}

			u32 dropped = 0;

			while (g_ps4_pages_held > cap)
			{
				codepage* oldest = nullptr;

				for (const auto& f : m_instance->fonts)
				{
					for (const auto& e : f->m_glyph_map)
					{
						codepage* const page = e.second.get();

						if (page != keep && !page->glyph_data.empty() && (!oldest || page->ps4_last_use < oldest->ps4_last_use))
						{
							oldest = page;
						}
					}
				}

				if (!oldest)
				{
					break;
				}

				std::vector<u8>().swap(oldest->glyph_data);
				g_ps4_pages_held--;
				dropped++;
			}

			if (dropped)
			{
				g_ps4_pages_dropped += dropped;
				u32 pages = 0;

				for (const auto& f : m_instance->fonts)
				{
					pages += ::size32(f->m_glyph_map);
				}

				char line[192];
				std::snprintf(line, sizeof(line), "fonts: pages %u in %u font(s), %u bitmap(s) dropped, %u held (cap %u, peak %u)",
					pages, ::size32(m_instance->fonts), dropped, g_ps4_pages_held, cap, g_ps4_pages_peak);
				ps4_font_line(line);
			}
		}

		void fontmgr::ps4_reset()
		{
			std::lock_guard lock(g_ps4_font_mutex);

			if (!m_instance || m_instance->fonts.empty())
			{
				return;
			}

			u32 pages = 0;

			for (const auto& f : m_instance->fonts)
			{
				pages += ::size32(f->m_glyph_map);
			}

			const u32 font_count = ::size32(m_instance->fonts);
			const u32 held = g_ps4_pages_held;
			const u32 peak = g_ps4_pages_peak;

			m_instance->fonts.clear();
			g_ps4_pages_held = 0;
			g_ps4_pages_peak = 0;

			if (pages)
			{
				char line[224];
				std::snprintf(line, sizeof(line), "fonts: pages %u of %u font(s) released at the overlay teardown (%u bitmap(s) held, peak %u, cap %u; dropped %llu, packed again %llu in this process)",
					pages, font_count, held, peak, ps4_page_cap(), static_cast<unsigned long long>(g_ps4_pages_dropped), static_cast<unsigned long long>(g_ps4_pages_rebuilt));
				ps4_startup_checkpoint(line);
			}
		}
#endif
	} // namespace overlays
} // namespace rsx
