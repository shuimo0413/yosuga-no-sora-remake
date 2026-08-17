/* SPDX-License-Identifier: MIT */
/*
 * EGL/GLES glue for the OpenHarmony video backend. Mirrors the SDL2 RPi
 * driver: every GL entry point is a thin wrapper over SDL's EGL module.
 */

#include "../../SDL_internal.h"

#include "../SDL_egl_c.h"
#include <native_window/external_window.h>

#include "SDL_ohosvideo.h"
#include "SDL_ohosgl.h"
#include "sdl_ohos_bridge.h"

int OHOS_GL_LoadLibrary(_THIS, const char *path)
{
	return SDL_EGL_LoadLibrary(_this, path, (NativeDisplayType)EGL_DEFAULT_DISPLAY, 0);
}

void *OHOS_GL_GetProcAddress(_THIS, const char *proc)
{
	return SDL_EGL_GetProcAddress(_this, proc);
}

void OHOS_GL_UnloadLibrary(_THIS)
{
	SDL_EGL_UnloadLibrary(_this);
}

SDL_GLContext OHOS_GL_CreateContext(_THIS, SDL_Window *window)
{
	SDL_WindowData *data = (SDL_WindowData *)window->driverdata;
	OHNativeWindow *native_window;

	if (data == NULL)
	{
		return NULL;
	}
	native_window = (OHNativeWindow *)SDL_OHOS_GetNativeWindow();
	if (native_window == NULL)
	{
		SDL_SetError("The XComponent native window is unavailable");
		return NULL;
	}

	data->egl_surface = (EGLSurface)SDL_EGL_CreateSurface(_this, (NativeWindowType)native_window);
	if (data->egl_surface == EGL_NO_SURFACE)
	{
		return NULL;
	}
	return SDL_EGL_CreateContext(_this, data->egl_surface);
}

int OHOS_GL_MakeCurrent(_THIS, SDL_Window *window, SDL_GLContext context)
{
	if (window && context)
	{
		SDL_WindowData *data = (SDL_WindowData *)window->driverdata;
		if (data == NULL)
		{
			return SDL_SetError("Window has no driver data");
		}
		return SDL_EGL_MakeCurrent(_this, data->egl_surface, context);
	}
	return SDL_EGL_MakeCurrent(_this, NULL, NULL);
}

int OHOS_GL_SetSwapInterval(_THIS, int interval)
{
	return SDL_EGL_SetSwapInterval(_this, interval);
}

int OHOS_GL_GetSwapInterval(_THIS)
{
	return SDL_EGL_GetSwapInterval(_this);
}

int OHOS_GL_SwapWindow(_THIS, SDL_Window *window)
{
	SDL_WindowData *data = (SDL_WindowData *)window->driverdata;
	if (data == NULL)
	{
		return SDL_SetError("Window has no driver data");
	}
	return SDL_EGL_SwapBuffers(_this, data->egl_surface);
}

void OHOS_GL_DeleteContext(_THIS, SDL_GLContext context)
{
	SDL_EGL_DeleteContext(_this, context);
}
