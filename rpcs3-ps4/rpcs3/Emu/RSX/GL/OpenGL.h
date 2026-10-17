#pragma once
#if !defined(_WIN32) && !defined(RPCS3_PS4)
#include <GL/glew.h>
#endif

#ifdef RPCS3_PS4
// Resolve the original renderer's entry points through the bundled Mesa EGL
// driver. No desktop GL library or GLEW/GLX context is available on Orbis.
#include <GL/glcorearb.h>
#include <GL/glext.h>
using ps4_gl_draw_pixels_proc = void (APIENTRY*)(GLsizei, GLsizei, GLenum, GLenum, const void*);
#define OPENGL_PROC(p, n) extern p gl##n
#define OPENGL_PROC2(p, n, tn) OPENGL_PROC(p, n)
#define WGL_PROC(p, n)
#include "GLProcTable.h"
#include "PS4GLProcTable.h"
#undef OPENGL_PROC
#undef OPENGL_PROC2
#undef WGL_PROC
#elif defined(_WIN32)
#include <Windows.h>
#include "GL/gl.h"
#include <glext.h>
typedef BOOL (WINAPI* PFNWGLSWAPINTERVALEXTPROC) (int interval);

#define OPENGL_PROC(p, n) extern p gl##n
#define WGL_PROC(p, n) extern p wgl##n
#define OPENGL_PROC2(p, n, tn) OPENGL_PROC(p, n)
	#include "GLProcTable.h"
#undef OPENGL_PROC
#undef WGL_PROC
#undef OPENGL_PROC2
#else
#include <GL/gl.h>
#ifdef HAVE_X11
#include <GL/glxew.h>
#include <GL/glx.h>
#include <GL/glxext.h>
#endif
#endif

#ifndef GL_TEXTURE_BUFFER_BINDING
//During spec release, this enum was removed during upgrade from ARB equivalent
//See https://www.khronos.org/bugzilla/show_bug.cgi?id=844
#define GL_TEXTURE_BUFFER_BINDING 0x8C2A
#endif

namespace gl
{
	void init();
	void set_swapinterval(int interval);
}
