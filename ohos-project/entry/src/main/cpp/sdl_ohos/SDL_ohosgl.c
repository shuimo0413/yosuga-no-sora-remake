/* SPDX-License-Identifier: MIT */
/*
 * EGL/GLES glue for the OpenHarmony video backend.
 *
 * EGL/GLES are linked directly into the app (libEGL.so / libGLESv3.so), so
 * SDL's dlopen-based EGL loader would fail inside the app sandbox. Every GL
 * entry point below therefore uses raw link-time EGL symbols instead of
 * SDL's EGL module (SDL_EGL_*).
 */

#include "../../SDL_internal.h"

#include "../SDL_egl_c.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <stdarg.h>
#include <dlfcn.h>
#include <native_window/external_window.h>

#include "SDL_ohosvideo.h"
#include "SDL_ohosgl.h"
#include "sdl_ohos_bridge.h"

/* Minimal EGL state shared across the entry points below. */
static EGLDisplay g_ohos_egl_display = EGL_NO_DISPLAY;
static EGLContext g_ohos_egl_context = EGL_NO_CONTEXT;

static int OHOS_EglInit(void)
{
	if (g_ohos_egl_display != EGL_NO_DISPLAY)
	{
		return 1;
	}
	g_ohos_egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (g_ohos_egl_display == EGL_NO_DISPLAY)
	{
		SDL_SetError("OHOS: eglGetDisplay failed");
		return 0;
	}
	EGLint major = 0, minor = 0;
	if (!eglInitialize(g_ohos_egl_display, &major, &minor))
	{
		SDL_SetError("OHOS: eglInitialize failed");
		return 0;
	}
	eglBindAPI(EGL_OPENGL_ES_API);
	return 1;
}

/* EGL_EXT_device_enumeration / EGL_EXT_platform_base compatibility stubs
 * (eglQueryDevicesEXT / eglGetPlatformDisplayEXT) are defined in libentry.so
 * (krkrsdl2_ohos_entry.cpp), because SDL resolves EGL extension symbols with
 * SDL_LoadFunction(NULL, ...) == dlsym(NULL, ...), which only sees the main
 * program's symbols. Defining them here in libkrkrsdl2.so would be invisible
 * to dlsym(NULL) and would also cause duplicate-symbol link errors. */

int OHOS_GL_LoadLibrary(_THIS, const char *path)
{
	(void)_this;
	(void)path;
	/* EGL is linked into the app; nothing to dlopen. SDL_GL_LoadLibrary()
	 * treats a 0 return as success and any non-zero as failure (calling
	 * GL_UnloadLibrary and marking the GL driver unavailable, so
	 * SDL_CreateRenderer(ACCELERATED) finds no render driver and falls back
	 * to the software renderer). OHOS_EglInit() returns 1 on success / 0 on
	 * failure, so translate that to SDL's 0==success / -1==failure. */
	int r = OHOS_EglInit() ? 0 : -1;
	return r;
}

void *OHOS_GL_GetProcAddress(_THIS, const char *proc)
{
	(void)_this;
	void *addr = (void *)eglGetProcAddress(proc);
	if (addr == NULL)
	{
		/* eglGetProcAddress only resolves extension entry points; core GLES
		 * functions come from the linked libGLESv3 via dlsym(RTLD_DEFAULT). */
		addr = (void *)dlsym(RTLD_DEFAULT, proc);
	}
	if (addr == NULL)
	{
		/* Only log failures: the GLES2 renderer probe requests many entry
		 * points and a missing one aborts its initialisation. */
	}
	return addr;
}

void OHOS_GL_UnloadLibrary(_THIS)
{
	(void)_this;
}

SDL_GLContext OHOS_GL_CreateContext(_THIS, SDL_Window *window)
{
	SDL_WindowData *data;
	OHNativeWindow *native_window;
	EGLConfig config = NULL;
	EGLint num_configs = 0;
	{
		const char *dd = SDL_OHOS_GetFilesDir();
		if (dd && dd[0])
		{
			char lpath[512];
			snprintf(lpath, sizeof(lpath), "%s/diag.txt", dd);
			FILE *lf = fopen(lpath, "a");
			if (lf) { fprintf(lf, "egl: CreateContext enter\n"); fclose(lf); }
		}
	}

	if (window == NULL || (data = (SDL_WindowData *)window->driverdata) == NULL)
	{
		return NULL;
	}
	native_window = (OHNativeWindow *)SDL_OHOS_GetNativeWindow();
	if (native_window == NULL)
	{
		SDL_SetError("The XComponent native window is unavailable");
		return NULL;
	}

	if (!OHOS_EglInit())
	{
		return NULL;
	}

	/* The XComponent native window accepts only ONE EGL surface:
	 * a second eglCreateWindowSurface on the same window fails with
	 * EGL_BAD_ALLOC (0x3003). SDL's GLES2 renderer probe first creates a
	 * dummy context (surface #1) and later requests the real context on
	 * the same window - that second call used to fail, so SDL fell back to
	 * the software renderer. REUSE the existing surface for later contexts
	 * instead of creating a new one.
	 *
	 * The surface must also stay alive: destroying it (even to rebuild a
	 * fresh one) makes the window stop presenting (black screen), which is
	 * what SDL would do when the dummy context is torn down. */
	if (data->egl_surface == EGL_NO_SURFACE)
	{
		/* Size the native window to the SDL window (logical game size):
		 * EGL adopts the native window geometry as the surface size, and
		 * SDL sets its GL viewport from the window size - a mismatch makes
		 * the game picture occupy only a corner of the surface. The
		 * software path re-sets the physical geometry every frame, so this
		 * does not disturb the fallback. */
		int gw = window->w > 0 ? window->w : 1920;
		int gh = window->h > 0 ? window->h : 1080;
		OH_NativeWindow_NativeWindowHandleOpt(native_window, SET_BUFFER_GEOMETRY, gw, gh);
		EGLint config_attribs[] = {
			EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
			EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
			EGL_RED_SIZE, 8,
			EGL_GREEN_SIZE, 8,
			EGL_BLUE_SIZE, 8,
			EGL_ALPHA_SIZE, 8,
			EGL_NONE
		};
		if (!eglChooseConfig(g_ohos_egl_display, config_attribs, &config, 1, &num_configs) || num_configs < 1)
		{
			SDL_SetError("OHOS: eglChooseConfig failed");
			return NULL;
		}

		data->egl_surface = eglCreateWindowSurface(g_ohos_egl_display, config, (EGLNativeWindowType)(uintptr_t)native_window, NULL);
		if (data->egl_surface == EGL_NO_SURFACE)
		{
			SDL_SetError("OHOS: eglCreateWindowSurface failed");
			return NULL;
		}
	}
	else
	{
	}

	EGLint ctx_attribs[] = {
		EGL_CONTEXT_CLIENT_VERSION, 2,
		EGL_NONE
	};
	{
		static int ctx_serial = 0;
		g_ohos_egl_context = eglCreateContext(g_ohos_egl_display, config, EGL_NO_CONTEXT, ctx_attribs);
		if (g_ohos_egl_context == EGL_NO_CONTEXT)
		{
			SDL_SetError("OHOS: eglCreateContext failed");
			return NULL;
		}
		/* SDL_GL_CreateContext records the context as current WITHOUT calling
		 * the driver's MakeCurrent (SDL_GL_MakeCurrent then short-circuits),
		 * so bind it for real here - otherwise the renderer probe runs with
		 * no current context and its shader cache step fails. */
		if (!eglMakeCurrent(g_ohos_egl_display, data->egl_surface, data->egl_surface, g_ohos_egl_context))
		{
			SDL_SetError("OHOS: eglMakeCurrent after create failed");
			return NULL;
		}
	}
	{
		const char *dd = SDL_OHOS_GetFilesDir();
		if (dd && dd[0])
		{
			char lpath[512];
			snprintf(lpath, sizeof(lpath), "%s/diag.txt", dd);
			FILE *lf = fopen(lpath, "a");
			if (lf) { fprintf(lf, "egl: CreateContext done\n"); fclose(lf); }
		}
	}
	return (SDL_GLContext)g_ohos_egl_context;
}

int OHOS_GL_MakeCurrent(_THIS, SDL_Window *window, SDL_GLContext context)
{
	(void)_this;
	EGLBoolean ok;
	if (window && context)
	{
		SDL_WindowData *data = (SDL_WindowData *)window->driverdata;
		if (data == NULL)
		{
			return SDL_SetError("Window has no driver data");
		}
		ok = eglMakeCurrent(g_ohos_egl_display, data->egl_surface, data->egl_surface, g_ohos_egl_context);
		return ok ? 0 : SDL_SetError("OHOS: eglMakeCurrent failed");
	}
	ok = eglMakeCurrent(g_ohos_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	return ok ? 0 : SDL_SetError("OHOS: eglMakeCurrent(NULL) failed");
}

int OHOS_GL_SetSwapInterval(_THIS, int interval)
{
	(void)_this;
		/* OHOS: the composer presents via the shared native window; keep the
	 * swap unthrottled so krkz's 60fps DrawCycleTimer drives the frame
	 * pacing instead of the display refresh. */
	return eglSwapInterval(g_ohos_egl_display, 0) ? 0 : SDL_SetError("OHOS: eglSwapInterval failed");
}

int OHOS_GL_GetSwapInterval(_THIS)
{
	(void)_this;
	return 1;
}

int OHOS_GL_SwapWindow(_THIS, SDL_Window *window)
{
	SDL_WindowData *data;
	if (window == NULL || (data = (SDL_WindowData *)window->driverdata) == NULL)
	{
		return SDL_SetError("Window has no driver data");
	}
	{
		static int swap_count = 0;
		if (++swap_count <= 5 || swap_count % 300 == 0)
		{
		}
	}
	{
		EGLBoolean ok = eglSwapBuffers(g_ohos_egl_display, data->egl_surface);
		if (!ok)
		{
		}
		return ok ? 0 : SDL_SetError("OHOS: eglSwapBuffers failed");
	}
}

void OHOS_GL_DeleteContext(_THIS, SDL_GLContext context)
{
	(void)_this;
	if (context)
	{
		eglDestroyContext(g_ohos_egl_display, (EGLContext)context);
		g_ohos_egl_context = EGL_NO_CONTEXT;
	}
}
