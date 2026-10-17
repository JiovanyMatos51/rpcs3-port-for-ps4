#pragma once

#ifdef RPCS3_PS4

#include "Emu/RSX/GSFrameBase.h"

#include <atomic>
#include <mutex>
#include <EGL/egl.h>

// Vulkan owns presentation through the Orbis headless WSI. This class supplies
// the host display geometry and lifecycle expected by the existing RSX core.
class ps4_frame final : public GSFrameBase
{
	std::atomic<bool> m_visible{true};
	std::mutex m_context_mutex;
	EGLDisplay m_display = EGL_NO_DISPLAY;
	EGLConfig m_config = nullptr;
	EGLContext m_share_context = EGL_NO_CONTEXT;
	EGLSurface m_window = EGL_NO_SURFACE;
	u32 m_present_failures = 0; // consecutive eglSwapBuffers failures (RSX thread only)

    void initialize_gl();

public:
    ~ps4_frame() override;
	void close() override;
	void reset() override;
	bool shown() override;
	void hide() override;
	void show() override;
	void toggle_fullscreen() override;

	void delete_context(draw_context_t ctx) override;
	draw_context_t make_context() override;
	void set_current(draw_context_t ctx) override;
	void flip(draw_context_t ctx, bool skip_frame = false) override;
	int client_width() override;
	int client_height() override;
	f64 client_display_rate() override;
	bool has_alpha() override;
	display_handle_t handle() const override;

	bool can_consume_frame() const override;
	void present_frame(std::vector<u8>&& data, u32 pitch, u32 width, u32 height, bool is_bgra) const override;
	void take_screenshot(std::vector<u8>&& sshot_data, u32 sshot_width, u32 sshot_height, bool is_bgra) override;
	void update_title(double fps = 0.0) override;
};

#endif
