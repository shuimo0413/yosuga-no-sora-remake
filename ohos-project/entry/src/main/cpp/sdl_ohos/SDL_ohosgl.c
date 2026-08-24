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

static void OHOS_EglLog(const char *fmt, ...)
{
	char line[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);

	/* Write to the SANDBOX files dir (hdc shell can read it) as both an
	 * egl-specific log and engine.log, plus the public Download dir. The
	 * public dir is what the user's logs.zip captures; the sandbox one is
	 * what hdc shell can read directly for live debugging. */
	const char *dd = getenv("KRKR_OHOS_DATA_DIR");
	const char *sandbox = SDL_OHOS_GetFilesDir();
	if (sandbox && sandbox[0])
	{
		char epath[512];
		snprintf(epath, sizeof(epath), "%s/yosuga-egl.log", sandbox);
		FILE *ef = fopen(epath, "a");
		if (ef) { fprintf(ef, "%s\n", line); fclose(ef); }
		snprintf(epath, sizeof(epath), "%s/engine.log", sandbox);
		ef = fopen(epath, "a");
		if (ef) { fprintf(ef, "egl: %s\n", line); fclose(ef); }
	}
	if (dd && dd[0])
	{
		char epath[512];
		snprintf(epath, sizeof(epath), "%s/yosuga-egl.log", dd);
		FILE *ef = fopen(epath, "a");
		if (ef) { fprintf(ef, "%s\n", line); fclose(ef); }
		snprintf(epath, sizeof(epath), "%s/engine.log", dd);
		ef = fopen(epath, "a");
		if (ef) { fprintf(ef, "egl: %s\n", line); fclose(ef); }
	}
}

static int OHOS_EglInit(void)
{
	if (g_ohos_egl_display != EGL_NO_DISPLAY)
	{
		return 1;
	}
	g_ohos_egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if (g_ohos_egl_display == EGL_NO_DISPLAY)
	{
		OHOS_EglLog("eglGetDisplay failed err=%#x", (unsigned)eglGetError());
		SDL_SetError("OHOS: eglGetDisplay failed");
		return 0;
	}
	EGLint major = 0, minor = 0;
	if (!eglInitialize(g_ohos_egl_display, &major, &minor))
	{
		OHOS_EglLog("eglInitialize failed err=%#x", (unsigned)eglGetError());
		SDL_SetError("OHOS: eglInitialize failed");
		return 0;
	}
	OHOS_EglLog("eglInitialize ok %d.%d", (int)major, (int)minor);
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
	OHOS_EglLog("OHOS_GL_LoadLibrary called - path=%s", path ? path : "(null)");
	int r = OHOS_EglInit() ? 0 : -1;
	OHOS_EglLog("OHOS_GL_LoadLibrary returning %d", r);
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
	FILE *lf = fopen("/data/local/tmp/yosuga-egl.log", "a");
	if (lf) { fprintf(lf, "OHOS_GL_CreateContext enter\n"); fclose(lf); }

	if (window == NULL || (data = (SDL_WindowData *)window->driverdata) == NULL)
	{
		OHOS_EglLog("CreateContext: no window driver data");
		return NULL;
	}
	native_window = (OHNativeWindow *)SDL_OHOS_GetNativeWindow();
	if (native_window == NULL)
	{
		OHOS_EglLog("CreateContext: no native window");
		SDL_SetError("The XComponent native window is unavailable");
		return NULL;
	}
	OHOS_EglLog("CreateContext: native_window=%p", (void *)native_window);

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
			OHOS_EglLog("eglChooseConfig failed err=%#x n=%d", (unsigned)eglGetError(), num_configs);
			SDL_SetError("OHOS: eglChooseConfig failed");
			return NULL;
		}

		data->egl_surface = eglCreateWindowSurface(g_ohos_egl_display, config, (EGLNativeWindowType)(uintptr_t)native_window, NULL);
		if (data->egl_surface == EGL_NO_SURFACE)
		{
			OHOS_EglLog("eglCreateWindowSurface failed err=%#x", (unsigned)eglGetError());
			SDL_SetError("OHOS: eglCreateWindowSurface failed");
			return NULL;
		}
		OHOS_EglLog("egl_surface=%p created", (void *)data->egl_surface);
	}
	else
	{
		OHOS_EglLog("egl_surface=%p reused for a second context", (void *)data->egl_surface);
	}

	EGLint ctx_attribs[] = {
		EGL_CONTEXT_CLIENT_VERSION, 2,
		EGL_NONE
	};
	g_ohos_egl_context = eglCreateContext(g_ohos_egl_display, config, EGL_NO_CONTEXT, ctx_attribs);
	if (g_ohos_egl_context == EGL_NO_CONTEXT)
	{
		OHOS_EglLog("eglCreateContext failed err=%#x", (unsigned)eglGetError());
		SDL_SetError("OHOS: eglCreateContext failed");
		return NULL;
	}
	OHOS_EglLog("context=%p", (void *)g_ohos_egl_context);
	return (SDL_GLContext)g_ohos_egl_context;
}

int OHOS_GL_MakeCurrent(_THIS, SDL_Window *window, SDL_GLContext context)
{
	(void)_this;
	if (window && context)
	{
		SDL_WindowData *data = (SDL_WindowData *)window->driverdata;
		if (data == NULL)
		{
			return SDL_SetError("Window has no driver data");
		}
		return eglMakeCurrent(g_ohos_egl_display, data->egl_surface, data->egl_surface, g_ohos_egl_context) ? 0 : SDL_SetError("OHOS: eglMakeCurrent failed");
	}
	return eglMakeCurrent(g_ohos_egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT) ? 0 : SDL_SetError("OHOS: eglMakeCurrent(NULL) failed");
}

int OHOS_GL_SetSwapInterval(_THIS, int interval)
{
	(void)_this;
	return eglSwapInterval(g_ohos_egl_display, interval) ? 0 : SDL_SetError("OHOS: eglSwapInterval failed");
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
			OHOS_EglLog("swap #%d egl_surface=%p", swap_count, (void *)data->egl_surface);
		}
	}
	return eglSwapBuffers(g_ohos_egl_display, data->egl_surface) ? 0 : SDL_SetError("OHOS: eglSwapBuffers failed");
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
