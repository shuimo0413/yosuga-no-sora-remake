/* SPDX-License-Identifier: MIT */
/*
 * OpenHarmony video backend for SDL2.
 *
 * Modeled on the SDL2 RPi/Android EGL drivers. The XComponent surface state
 * is owned by the NAPI entry module and exchanged through sdl_ohos_bridge.h;
 * this backend only consumes the native window and surface size.
 */

#include "../../SDL_internal.h"

#include "../SDL_sysvideo.h"
#include "../../events/SDL_mouse_c.h"
#include "../../events/SDL_keyboard_c.h"

#include "SDL_ohosvideo.h"
#include "SDL_ohosevents.h"
#include "SDL_ohosgl.h"
#include "sdl_ohos_bridge.h"

#define OHOS_FALLBACK_WIDTH 1920
#define OHOS_FALLBACK_HEIGHT 1080

static SDL_VideoDevice *OHOS_CreateDevice(void);
static void OHOS_DestroyDevice(SDL_VideoDevice *device);

static int OHOS_VideoInit(_THIS);
static void OHOS_VideoQuit(_THIS);
static void OHOS_GetDisplayModes(_THIS, SDL_VideoDisplay *display);
static int OHOS_SetDisplayMode(_THIS, SDL_VideoDisplay *display, SDL_DisplayMode *mode);
static int OHOS_GetDisplayBounds(_THIS, SDL_VideoDisplay *display, SDL_Rect *rect);
static int OHOS_CreateSDLWindow(_THIS, SDL_Window *window);
static void OHOS_SetWindowTitle(_THIS, SDL_Window *window);
static void OHOS_SetWindowPosition(_THIS, SDL_Window *window);
static void OHOS_SetWindowSize(_THIS, SDL_Window *window);
static void OHOS_ShowWindow(_THIS, SDL_Window *window);
static void OHOS_HideWindow(_THIS, SDL_Window *window);
static void OHOS_SetWindowFullscreen(_THIS, SDL_Window *window, SDL_VideoDisplay *display, SDL_bool fullscreen);

VideoBootStrap OHOS_bootstrap = {
	"ohos", "OpenHarmony XComponent video driver", OHOS_CreateDevice, NULL
};

static void OHOS_DestroyDevice(SDL_VideoDevice *device)
{
	if (device->driverdata)
	{
		SDL_free(device->driverdata);
	}
	SDL_free(device);
}

static SDL_VideoDevice *OHOS_CreateDevice(void)
{
	SDL_VideoDevice *device;
	OHOS_VideoData *videodata;

	device = (SDL_VideoDevice *)SDL_calloc(1, sizeof(SDL_VideoDevice));
	if (device == NULL)
	{
		return NULL;
	}
	videodata = (OHOS_VideoData *)SDL_calloc(1, sizeof(OHOS_VideoData));
	if (videodata == NULL)
	{
		SDL_free(device);
		return NULL;
	}
	device->driverdata = videodata;

	device->VideoInit = OHOS_VideoInit;
	device->VideoQuit = OHOS_VideoQuit;
	device->GetDisplayModes = OHOS_GetDisplayModes;
	device->SetDisplayMode = OHOS_SetDisplayMode;
	device->GetDisplayBounds = OHOS_GetDisplayBounds;
	device->PumpEvents = OHOS_PumpEvents;
	device->CreateSDLWindow = OHOS_CreateSDLWindow;
	device->SetWindowTitle = OHOS_SetWindowTitle;
	device->SetWindowPosition = OHOS_SetWindowPosition;
	device->SetWindowSize = OHOS_SetWindowSize;
	device->ShowWindow = OHOS_ShowWindow;
	device->HideWindow = OHOS_HideWindow;
	device->SetWindowFullscreen = OHOS_SetWindowFullscreen;

	device->GL_LoadLibrary = OHOS_GL_LoadLibrary;
	device->GL_GetProcAddress = OHOS_GL_GetProcAddress;
	device->GL_UnloadLibrary = OHOS_GL_UnloadLibrary;
	device->GL_CreateContext = OHOS_GL_CreateContext;
	device->GL_MakeCurrent = OHOS_GL_MakeCurrent;
	device->GL_SetSwapInterval = OHOS_GL_SetSwapInterval;
	device->GL_GetSwapInterval = OHOS_GL_GetSwapInterval;
	device->GL_SwapWindow = OHOS_GL_SwapWindow;
	device->GL_DeleteContext = OHOS_GL_DeleteContext;

	device->free = OHOS_DestroyDevice;
	return device;
}

static void OHOS_GetSurfaceSize(int *width, int *height)
{
	int w = 0;
	int h = 0;
	if (!SDL_OHOS_GetSurfaceSize(&w, &h) || w <= 0 || h <= 0)
	{
		w = OHOS_FALLBACK_WIDTH;
		h = OHOS_FALLBACK_HEIGHT;
	}
	*width = w;
	*height = h;
}

static int OHOS_VideoInit(_THIS)
{
	SDL_VideoDisplay display;
	SDL_DisplayMode mode;
	int w = 0;
	int h = 0;

	if (!SDL_OHOS_WaitForNativeWindow(60000))
	{
		return SDL_SetError("Timed out waiting for the XComponent native window");
	}

	OHOS_GetSurfaceSize(&w, &h);

	SDL_zero(display);
	display.name = "OpenHarmony XComponent";
	SDL_zero(mode);
	mode.format = SDL_PIXELFORMAT_RGB888;
	mode.w = w;
	mode.h = h;
	mode.refresh_rate = 60;
	mode.driverdata = NULL;
	display.desktop_mode = mode;
	display.current_mode = mode;
	SDL_AddVideoDisplay(&display, SDL_FALSE);
	return 0;
}

static void OHOS_VideoQuit(_THIS)
{
	OHOS_VideoData *videodata = (OHOS_VideoData *)_this->driverdata;
	if (videodata != NULL && videodata->window != NULL)
	{
		SDL_Window *window = videodata->window;
		if (window->driverdata != NULL)
		{
			SDL_free(window->driverdata);
			window->driverdata = NULL;
		}
		videodata->window = NULL;
	}
}

static void OHOS_GetDisplayModes(_THIS, SDL_VideoDisplay *display)
{
	SDL_DisplayMode mode;
	int w = 0;
	int h = 0;

	OHOS_GetSurfaceSize(&w, &h);

	SDL_zero(mode);
	mode.format = SDL_PIXELFORMAT_RGB888;
	mode.w = w;
	mode.h = h;
	mode.refresh_rate = 60;
	SDL_AddDisplayMode(display, &mode);
}

static int OHOS_SetDisplayMode(_THIS, SDL_VideoDisplay *display, SDL_DisplayMode *mode)
{
	(void)display;
	(void)mode;
	/* The surface size is controlled by the XComponent; accept any request. */
	return 0;
}

static int OHOS_GetDisplayBounds(_THIS, SDL_VideoDisplay *display, SDL_Rect *rect)
{
	int w = 0;
	int h = 0;

	(void)display;
	OHOS_GetSurfaceSize(&w, &h);
	rect->x = 0;
	rect->y = 0;
	rect->w = w;
	rect->h = h;
	return 0;
}

static int OHOS_CreateSDLWindow(_THIS, SDL_Window *window)
{
	OHOS_VideoData *videodata = (OHOS_VideoData *)_this->driverdata;
	SDL_WindowData *data;
	int w = 0;
	int h = 0;

	data = (SDL_WindowData *)SDL_calloc(1, sizeof(SDL_WindowData));
	if (data == NULL)
	{
		return SDL_OutOfMemory();
	}
	window->driverdata = data;
	data->window = window;
	data->egl_surface = EGL_NO_SURFACE;

	OHOS_GetSurfaceSize(&w, &h);
	window->w = w;
	window->h = h;

	/* The XComponent always covers the whole screen. */
	window->flags |= SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_SHOWN;

	videodata->window = window;

	SDL_SetKeyboardFocus(window);
	SDL_SetMouseFocus(window);
	return 0;
}

static void OHOS_SetWindowTitle(_THIS, SDL_Window *window)
{
	(void)_this;
	(void)window;
}

static void OHOS_SetWindowPosition(_THIS, SDL_Window *window)
{
	(void)_this;
	(void)window;
}

static void OHOS_SetWindowSize(_THIS, SDL_Window *window)
{
	(void)_this;
	(void)window;
}

static void OHOS_ShowWindow(_THIS, SDL_Window *window)
{
	(void)_this;
	(void)window;
}

static void OHOS_HideWindow(_THIS, SDL_Window *window)
{
	(void)_this;
	(void)window;
}

static void OHOS_SetWindowFullscreen(_THIS, SDL_Window *window, SDL_VideoDisplay *display, SDL_bool fullscreen)
{
	(void)_this;
	(void)window;
	(void)display;
	(void)fullscreen;
}
