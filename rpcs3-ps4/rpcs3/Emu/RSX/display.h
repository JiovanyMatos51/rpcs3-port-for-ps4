#pragma once

#ifdef _WIN32
#include <windows.h>
#elif defined(__APPLE__)
// nothing
#elif defined(HAVE_X11)
// Cannot include Xlib.h before Qt
// and we don't need all of Xlib anyway
using Display = struct _XDisplay;
using Window  = unsigned long;
#endif

#ifdef HAVE_WAYLAND
#include <wayland-client.h>
#endif

#ifdef _WIN32
using display_handle_t = HWND;
#elif defined(RPCS3_PS4)
// The Orbis RADV WSI uses VK_EXT_headless_surface. Presentation is tied to
// sceVideoOut by the driver, so there is no native window handle to pass.
using display_handle_t = void*;
#elif defined(__APPLE__)
using display_handle_t = void*; // NSView
#else
#include <variant>
using display_handle_t = std::variant<
#if defined(HAVE_X11) && defined(HAVE_WAYLAND)
	std::pair<Display*, Window>, std::pair<wl_display*, wl_surface*>
#elif defined(HAVE_X11)
	std::pair<Display*, Window>
#elif defined(HAVE_WAYLAND)
	std::pair<wl_display*, wl_surface*>
#elif defined(ANDROID)
	struct ANativeWindow*
#endif
>;
#endif

using draw_context_t = void*;
