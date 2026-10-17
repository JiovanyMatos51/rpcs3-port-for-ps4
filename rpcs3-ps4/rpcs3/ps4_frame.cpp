#include "ps4_frame.h"

#ifdef RPCS3_PS4

#include "util/logs.hpp"
#include "ps4_launcher.h"
#include "ps4_fatal.h"
#include "Emu/system_config.h"
#include <EGL/eglext.h>
#include <EGL/eglmesaext.h>
#include <orbis/libkernel.h>
#include <cstdlib>
#include <cstring>

LOG_CHANNEL(ps4_video_log, "PS4 Video");

namespace
{
    // 0.39: native threads created process-wide while EGL/OpenGL starts (Mesa's own threads among them), logged once
    // per EGL start at the first bind of the primary context.
    unsigned long s_egl_threads_before = 0;
    bool s_egl_threads_pending = false;
}

void ps4_frame::close()
{
    m_visible = false;
    if (m_display != EGL_NO_DISPLAY)
    {
        if (m_window != EGL_NO_SURFACE) eglDestroySurface(m_display, m_window);
        eglTerminate(m_display);
        m_display = EGL_NO_DISPLAY;
        m_window = EGL_NO_SURFACE;
    }
}
void ps4_frame::reset() { m_visible = true; }
bool ps4_frame::shown() { return m_visible; }
void ps4_frame::hide() { m_visible = false; }
void ps4_frame::show() { m_visible = true; }
void ps4_frame::toggle_fullscreen() { /* The PS4 application is always fullscreen. */ }

namespace
{
    struct egl_context
    {
        EGLContext context;
        EGLSurface surface;
        bool primary;
    };
}

void ps4_frame::initialize_gl()
{
    if (m_display != EGL_NO_DISPLAY) return;
    unsigned long finished = 0;
    ps4_thread_census(&s_egl_threads_before, &finished);
    s_egl_threads_pending = true;
    // Match the Vulkan frontend geometry. Mesa owns VideoOut for both APIs.
    ::setenv("ORBIS_EGL_MODE", "1280x720", 1);
    m_display = eglGetPlatformDisplay(EGL_PLATFORM_ORBIS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    EGLint major = 0, minor = 0;
    if (m_display == EGL_NO_DISPLAY || !eglInitialize(m_display, &major, &minor))
        fmt::throw_exception("PS4 EGL initialization failed: 0x%x", eglGetError());
    const EGLint attributes[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT | EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_SAMPLES, 0, EGL_NONE};
    EGLint count = 0;
    if (!eglChooseConfig(m_display, attributes, &m_config, 1, &count) || !count)
        fmt::throw_exception("PS4 EGL: no OpenGL window configuration (0x%x)", eglGetError());
    // Orbis has no OS window. This stable non-null token is required by EGL;
    // platform_orbis never dereferences it and presents through kopper/WSI.
    m_window = eglCreatePlatformWindowSurface(m_display, m_config, this, nullptr);
    if (m_window == EGL_NO_SURFACE)
        fmt::throw_exception("PS4 EGL window creation failed: 0x%x", eglGetError());
    ps4_startup_checkpoint("OpenGL: native EGL window ready (Zink/RADV, 1280x720)");
}

ps4_frame::~ps4_frame()
{
    close();
}

draw_context_t ps4_frame::make_context()
{
    if (g_cfg.video.renderer != video_renderer::opengl) return nullptr;
    std::lock_guard lock(m_context_mutex);
    initialize_gl();
    ensure(eglBindAPI(EGL_OPENGL_API));
    const EGLint attributes[] = {EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
    const bool primary = m_share_context == EGL_NO_CONTEXT;
    const auto context = eglCreateContext(m_display, m_config, m_share_context, attributes);
    if (context == EGL_NO_CONTEXT)
    {
        if (!primary)
        {
            // 0.39: a shader worker's shared context is optional: no context, the renderer compiles on its own thread.
            const EGLint error = eglGetError();
            ps4_video_log.error("EGL shared context for a shader worker failed (0x%x)", error);
            ps4_startup_checkpoint(fmt::format("OpenGL: shared EGL context for the shader worker failed (0x%x)", error).c_str());
            return nullptr;
        }
        fmt::throw_exception("PS4 EGL OpenGL 4.3 context creation failed: 0x%x", eglGetError());
    }
    EGLSurface surface = m_window;
    if (!primary)
    {
        const EGLint pbuffer[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
        surface = eglCreatePbufferSurface(m_display, m_config, pbuffer);
        if (surface == EGL_NO_SURFACE)
        {
            // 0.39: the worker only compiles, so a context without a surface serves when the display offers one;
            // otherwise no worker context (the renderer compiles on its own thread) instead of a fatal error.
            const EGLint error = eglGetError();
            const char* extensions = eglQueryString(m_display, EGL_EXTENSIONS);
            const bool surfaceless = extensions && std::strstr(extensions, "EGL_KHR_surfaceless_context");
            ps4_startup_checkpoint(fmt::format("OpenGL: pbuffer surface for the shader worker failed (0x%x): %s", error,
                surfaceless ? "surfaceless context instead" : "no surfaceless contexts either, no worker context").c_str());
            if (!surfaceless)
            {
                eglDestroyContext(m_display, context);
                return nullptr;
            }
        }
    }
    if (primary) m_share_context = context;
    return new egl_context{context, surface, primary};
}

void ps4_frame::delete_context(draw_context_t handle)
{
    if (!handle) return;
    std::lock_guard lock(m_context_mutex);
    auto* ctx = static_cast<egl_context*>(handle);
    if (eglGetCurrentContext() == ctx->context)
        eglMakeCurrent(m_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(m_display, ctx->context);
    if (!ctx->primary)
    {
        if (ctx->surface != EGL_NO_SURFACE) eglDestroySurface(m_display, ctx->surface);
    }
    else m_share_context = EGL_NO_CONTEXT;
    delete ctx;
}

void ps4_frame::set_current(draw_context_t handle)
{
    if (!handle) return;
    const auto& ctx = *static_cast<egl_context*>(handle);
    ensure(eglBindAPI(EGL_OPENGL_API));
    if (!eglMakeCurrent(m_display, ctx.surface, ctx.surface, ctx.context))
        fmt::throw_exception("PS4 EGL context bind failed: 0x%x", eglGetError());

    if (ctx.primary && s_egl_threads_pending)
    {
        // 0.39: Mesa creates its OpenGL threads (Zink, RADV) itself; they are not in the emulator's thread registry
        // and no list of them is reachable here, so their names and priorities cannot be read or changed. Logged:
        // how many native threads started while EGL came up, and the priority they could inherit from this thread.
        s_egl_threads_pending = false;
        unsigned long created = 0, finished = 0;
        ps4_thread_census(&created, &finished);
        int prio = 0;
        const int rc = scePthreadGetprio(scePthreadSelf(), &prio);
        ps4_startup_checkpoint(fmt::format("OpenGL: Mesa threads: %lu native thread(s) started while EGL/OpenGL came up "
            "(process-wide count, the emulator's shader workers included); creating thread prio %d (rc 0x%x); their priorities are left as Mesa set them",
            created - s_egl_threads_before, prio, static_cast<u32>(rc)).c_str());
    }
}

void ps4_frame::flip(draw_context_t handle, bool skip_frame)
{
    if (!handle || skip_frame) return;
    const auto& ctx = *static_cast<egl_context*>(handle);
    if (eglSwapBuffers(m_display, ctx.surface))
    {
        if (m_present_failures)
        {
            ps4_video_log.notice("EGL present recovered after %u dropped frames", m_present_failures);
            m_present_failures = 0;
        }
        return;
    }
    const EGLint error = eglGetError();
    // A present may fail while the PS4 home screen owns the display. Throwing here used to
    // end the RSX thread through emergency_exit (black screen, emulator a zombie). Drop the
    // frame instead and keep the emulator alive; persistent failures stay visible in the logs.
    const u32 failures = ++m_present_failures;
    if (failures == 1 || failures % 600 == 0)
    {
        ps4_video_log.error("EGL present failed (0x%x); frame dropped (%u consecutive, background=%d)",
            error, failures, ps4_in_background());
        ps4_startup_checkpoint(fmt::format("OpenGL: EGL present failed (0x%x); %u consecutive frames dropped",
            error, failures).c_str());
    }
}

int ps4_frame::client_width() { return 1280; }
int ps4_frame::client_height() { return 720; }
f64 ps4_frame::client_display_rate() { return 60.0; }
bool ps4_frame::has_alpha() { return false; }
display_handle_t ps4_frame::handle() const { return nullptr; }

bool ps4_frame::can_consume_frame() const { return false; }

void ps4_frame::present_frame(std::vector<u8>&&, u32, u32, u32, bool) const
{
	ps4_video_log.error("CPU frame presentation requires a VideoOut backend");
}

void ps4_frame::take_screenshot(std::vector<u8>&&, u32, u32, bool)
{
	ps4_video_log.error("Screenshot encoding is not available in the PS4 frontend yet");
}

void ps4_frame::update_title(double) { /* The PS4 shell owns the application title. */ }

#endif
