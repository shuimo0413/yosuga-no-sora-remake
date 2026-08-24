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
#include <native_window/external_window.h>
#include <string.h>
#include <sys/mman.h>
#include <dlfcn.h>
#include <hilog/log.h>

#define OHOS_FALLBACK_WIDTH 1920
#define OHOS_FALLBACK_HEIGHT 1080

/* OH_NativeWindow_LockBuffer / OH_NativeWindow_UnlockAndFlushBuffer are API
 * 23+; the CI builds against the API 12 sysroot, so reference them through
 * dlsym at run time instead of the headers (guarded availability). */
typedef int32_t (*OHNW_LockBufferFn)(OHNativeWindow *, Region, OHNativeWindowBuffer **);
typedef int32_t (*OHNW_UnlockFlushFn)(OHNativeWindow *);
static OHNW_LockBufferFn OHOS_NW_LockBuffer = NULL;
static OHNW_UnlockFlushFn OHOS_NW_UnlockAndFlushBuffer = NULL;
static void OHOS_ResolveNativeWindowCpuApi(void)
{
	static int resolved = 0;
	if (resolved) return;
	resolved = 1;
	void *handle = dlopen("libnative_window.so", RTLD_NOW);
	if (handle == NULL) return;
	OHOS_NW_LockBuffer = (OHNW_LockBufferFn)dlsym(handle, "OH_NativeWindow_LockBuffer");
	OHOS_NW_UnlockAndFlushBuffer = (OHNW_UnlockFlushFn)dlsym(handle, "OH_NativeWindow_UnlockAndFlushBuffer");
}

static SDL_VideoDevice *OHOS_CreateDevice(void);
static void OHOS_DestroyDevice(SDL_VideoDevice *device);

static int OHOS_VideoInit(_THIS);
static void OHOS_VideoQuit(_THIS);
static void OHOS_GetDisplayModes(_THIS, SDL_VideoDisplay *display);
static int OHOS_SetDisplayMode(_THIS, SDL_VideoDisplay *display, SDL_DisplayMode *mode);
static int OHOS_GetDisplayBounds(_THIS, SDL_VideoDisplay *display, SDL_Rect *rect);
static int OHOS_CreateSDLWindow(_THIS, SDL_Window *window);
static void OHOS_GetSurfaceSize(int *width, int *height);

/* --- Software framebuffer support --------------------------------------- */
/* Paints an SDL_Surface into an OH_NativeWindow buffer. Kept simple: one
 * surface lives in window->driverdata and is re-uploaded on Update. */

static int OHOS_CreateWindowFramebuffer(_THIS, SDL_Window *window,
	Uint32 *format, void **pixels, int *pitch)
{
	SDL_WindowData *data = (SDL_WindowData *)window->driverdata;
	if (data == NULL)
	{
		return SDL_SetError("Window has no driver data");
	}
	int w = 0, h = 0;
	OHOS_GetSurfaceSize(&w, &h);
	if (w <= 0 || h <= 0)
	{
		w = OHOS_FALLBACK_WIDTH;
		h = OHOS_FALLBACK_HEIGHT;
	}

	if (data->framebuffer != NULL)
	{
		SDL_FreeSurface(data->framebuffer);
		data->framebuffer = NULL;
	}
	data->framebuffer = SDL_CreateRGBSurface(0, w, h, 32,
		0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000);
	if (data->framebuffer == NULL)
	{
		return SDL_OutOfMemory();
	}
	*format = data->framebuffer->format->format;
	*pixels = data->framebuffer->pixels;
	*pitch = data->framebuffer->pitch;
	return 0;
}

static int OHOS_UpdateWindowFramebuffer(_THIS, SDL_Window *window,
	const SDL_Rect *rects, int numrects)
{
	SDL_WindowData *data = (SDL_WindowData *)window->driverdata;
	OHNativeWindow *native_window;
	OHNativeWindowBuffer *buffer = NULL;
	BufferHandle *handle = NULL;
	int fence_fd = -1;
	int32_t dummy = 0;
	static int fb_count = 0;
	/* Mirror progress into the sandbox engine.log (hdc-readable). */
	{
		if (++fb_count % 100 == 1 || fb_count <= 5)
		{
			const char *dd = SDL_OHOS_GetFilesDir();
			if (dd && dd[0])
			{
				FILE *ef;
				char epath[512];
				snprintf(epath, sizeof(epath), "%s/engine.log", dd);
				ef = fopen(epath, "a");
				if (ef) { fprintf(ef, "engine: UpdateWindowFramebuffer #%d\n", fb_count); fclose(ef); }
			}
			/* Mirror into the public Download app dir too. */
			const char *pub = getenv("KRKR_OHOS_DATA_DIR");
			if (pub && pub[0] && (!dd || strncmp(pub, dd, 512) != 0))
			{
				FILE *ef;
				char epath[512];
				snprintf(epath, sizeof(epath), "%s/engine.log", pub);
				ef = fopen(epath, "a");
				if (ef) { fprintf(ef, "engine: UpdateWindowFramebuffer #%d\n", fb_count); fclose(ef); }
			}
		}
	}

	/* FB-status diagnostic file, also in the sandbox so hdc can read it. */
	FILE *fb = NULL;
	{
		const char *dd = SDL_OHOS_GetFilesDir();
		if (dd && dd[0])
		{
			char fpath[512];
			snprintf(fpath, sizeof(fpath), "%s/engine.log", dd);
			fb = fopen(fpath, "a");
			if (fb) fprintf(fb, "engine: UpdateWindowFramebuffer enter fb_count=%d\n", fb_count);
		}
	}
	/* FB-status in the public Download app dir as well. */
	FILE *fb_pub = NULL;
	{
		const char *pub = getenv("KRKR_OHOS_DATA_DIR");
		if (pub && pub[0])
		{
			char fpath[512];
			snprintf(fpath, sizeof(fpath), "%s/engine.log", pub);
			fb_pub = fopen(fpath, "a");
			if (fb_pub) fprintf(fb_pub, "engine: UpdateWindowFramebuffer enter fb_count=%d\n", fb_count);
		}
	}

	if (data == NULL || data->framebuffer == NULL)
	{
		return SDL_SetError("No framebuffer");
	}
	native_window = (OHNativeWindow *)SDL_OHOS_GetNativeWindow();
	if (native_window == NULL)
	{
		return SDL_SetError("No native window");
	}

	int32_t w = data->framebuffer->w;
	int32_t h = data->framebuffer->h;
	/* The native window (XComponent surface) has the PHYSICAL size reported
	 * by ArkTS; request buffers at that geometry. The logical framebuffer is
	 * then stretched into it. */
	int32_t bw = w, bh = h;
	{
		int pw = 0, ph = 0;
		if (SDL_OHOS_GetPhysicalSize(&pw, &ph) && pw > 0 && ph > 0)
		{
			bw = pw;
			bh = ph;
		}
	}
	if (bw <= 0 || bh <= 0)
	{
		return SDL_SetError("OHOS: invalid buffer size");
	}
	if (OH_NativeWindow_NativeWindowHandleOpt(native_window, SET_BUFFER_GEOMETRY, bw, bh) != 0)
	{
		if (fb) { fprintf(fb, "  SET_BUFFER_GEOMETRY failed\n"); fclose(fb); }
		return SDL_SetError("OHOS: SET_BUFFER_GEOMETRY failed");
	}

	/* API 23+ (HarmonyOS NEXT): OH_NativeWindow_LockBuffer makes the buffer
	 * CPU-writable and must be paired with OH_NativeWindow_UnlockAndFlushBuffer.
	 * The legacy RequestBuffer path leaves handle->virAddr NULL for XComponent
	 * surfaces, so every software frame failed with "no virAddr" and the game
	 * picture (and the opening movie around it) froze. */
	Region lock_region = { NULL, 0 };
	int locked = 0;
	buffer = NULL;
	OHOS_ResolveNativeWindowCpuApi();
	if (OHOS_NW_LockBuffer != NULL && OHOS_NW_UnlockAndFlushBuffer != NULL &&
		OHOS_NW_LockBuffer(native_window, lock_region, &buffer) == 0 && buffer != NULL)
	{
		locked = 1;
	}
	else if (OH_NativeWindow_NativeWindowRequestBuffer(native_window, &buffer, &fence_fd) != 0 || buffer == NULL)
	{
		if (fb) { fprintf(fb, "  RequestBuffer failed\n"); fclose(fb); }
		return SDL_SetError("OHOS: NativeWindowRequestBuffer failed");
	}

	void *mapped = NULL;
	Uint8 *fb_dst = NULL;
	handle = OH_NativeWindow_GetBufferHandleFromNative(buffer);
	if (handle != NULL && handle->virAddr != NULL)
	{
		fb_dst = (Uint8 *)handle->virAddr;
	}
	else if (handle != NULL && handle->fd >= 0)
	{
		mapped = mmap(NULL, (size_t)handle->size, PROT_READ | PROT_WRITE, MAP_SHARED, handle->fd, 0);
		if (mapped != MAP_FAILED)
			fb_dst = (Uint8 *)mapped;
		else
			mapped = NULL;
	}
	if (fb_dst == NULL)
	{
		if (locked)
			OHOS_NW_UnlockAndFlushBuffer(native_window);
		else
			OH_NativeWindow_NativeWindowAbortBuffer(native_window, buffer);
		if (fb) { fprintf(fb, "  no writable address\n"); fclose(fb); }
		return SDL_SetError("OHOS: buffer has no writable address");
	}

	/* Copy the SDL surface (ARGB8888) into the native buffer, scaling from
	 * the logical framebuffer (1920x1080) to the physical buffer size. The
	 * buffer stride may differ from bw*4 (alignment) - always use it. */
	{
		Uint8 *dst = fb_dst;
		const Uint8 *src = (const Uint8 *)data->framebuffer->pixels;
		int src_pitch = data->framebuffer->pitch;
		int dst_stride = handle->stride;
		if (dst_stride <= 0) dst_stride = bw * 4;
		if (bw == w && bh == h)
		{
			/* same size: fast path */
			for (int y = 0; y < h; y++)
			{
				memcpy(dst + (size_t)y * (size_t)dst_stride, src + (size_t)y * (size_t)src_pitch, (size_t)w * 4);
			}
		}
		else
		{
			/* nearest-neighbor scale */
			for (int dy = 0; dy < bh; dy++)
			{
				int sy = dy * h / bh;
				const Uint8 *srow = src + (size_t)sy * (size_t)src_pitch;
				Uint8 *drow = dst + (size_t)dy * (size_t)dst_stride;
				for (int dx = 0; dx < bw; dx++)
				{
					int sx = dx * w / bw;
					const Uint8 *p = srow + (size_t)sx * 4;
					drow[dx * 4 + 0] = p[0];
					drow[dx * 4 + 1] = p[1];
					drow[dx * 4 + 2] = p[2];
					drow[dx * 4 + 3] = p[3];
				}
			}
		}
		{
			const char *dd = SDL_OHOS_GetFilesDir();
			if (dd && dd[0])
			{
				char lpath[512];
				snprintf(lpath, sizeof(lpath), "%s/engine.log", dd);
				FILE *lf = fopen(lpath, "a");
				if (lf) { fprintf(lf, "engine: FB copy %dx%d -> %dx%d stride=%d\n", (int)w, (int)h, (int)bw, (int)bh, (int)dst_stride); fclose(lf); }
			}
			/* Also mirror into the public Download app dir. */
			const char *pub = getenv("KRKR_OHOS_DATA_DIR");
			if (pub && pub[0] && (!dd || strncmp(pub, dd, 512) != 0))
			{
				char lpath[512];
				snprintf(lpath, sizeof(lpath), "%s/engine.log", pub);
				FILE *lf = fopen(lpath, "a");
				if (lf) { fprintf(lf, "engine: FB copy %dx%d -> %dx%d stride=%d\n", (int)w, (int)h, (int)bw, (int)bh, (int)dst_stride); fclose(lf); }
			}
			OH_LOG_Print(LOG_APP, LOG_INFO, 0x0000, "YosugaOHOS", "FB copy %dx%d -> %dx%d", (int)w, (int)h, (int)bw, (int)bh);
		}
	}

	if (mapped != NULL)
		munmap(mapped, (size_t)handle->size);
	if (locked)
	{
		if (OHOS_NW_UnlockAndFlushBuffer(native_window) != 0)
		{
			if (fb) { fprintf(fb, "  UnlockAndFlushBuffer failed\n"); fclose(fb); }
			return SDL_SetError("OHOS: UnlockAndFlushBuffer failed");
		}
	}
	else
	{
		Region region;
		region.rects = NULL;
		region.rectNumber = 0;
		if (OH_NativeWindow_NativeWindowFlushBuffer(native_window, buffer, fence_fd, region) != 0)
		{
			if (fb) { fprintf(fb, "  FlushBuffer failed\n"); fclose(fb); }
			if (fb_pub) { fprintf(fb_pub, "  FlushBuffer failed\n"); fclose(fb_pub); }
			return SDL_SetError("OHOS: FlushBuffer failed");
		}
	}
	if (fb) { fprintf(fb, "  flushed %dx%d\n", (int)w, (int)h); fclose(fb); }
	if (fb_pub) { fprintf(fb_pub, "  flushed %dx%d\n", (int)w, (int)h); fclose(fb_pub); }
	(void)rects;
	(void)numrects;
	(void)dummy;
	return 0;
}

static void OHOS_DestroyWindowFramebuffer(_THIS, SDL_Window *window)
{
	SDL_WindowData *data = (SDL_WindowData *)window->driverdata;
	if (data != NULL && data->framebuffer != NULL)
	{
		SDL_FreeSurface(data->framebuffer);
		data->framebuffer = NULL;
	}
}

static void OHOS_DestroyWindow(_THIS, SDL_Window *window)
{
	SDL_WindowData *data;
	if (window == NULL || (data = (SDL_WindowData *)window->driverdata) == NULL)
	{
		return;
	}
	if (data->egl_surface != EGL_NO_SURFACE)
	{
		EGLDisplay dpy = eglGetCurrentDisplay();
		if (dpy != EGL_NO_DISPLAY)
		{
			eglDestroySurface(dpy, data->egl_surface);
		}
		data->egl_surface = EGL_NO_SURFACE;
	}
	if (data->framebuffer != NULL)
	{
		SDL_FreeSurface(data->framebuffer);
		data->framebuffer = NULL;
	}
	SDL_free(data);
	window->driverdata = NULL;
}

static void OHOS_SetWindowTitle(_THIS, SDL_Window *window);
static void OHOS_SetWindowPosition(_THIS, SDL_Window *window);
static void OHOS_SetWindowSize(_THIS, SDL_Window *window);
static void OHOS_ShowWindow(_THIS, SDL_Window *window);
static void OHOS_HideWindow(_THIS, SDL_Window *window);
static int OHOS_CreateWindowFramebuffer(_THIS, SDL_Window *window, Uint32 *format, void **pixels, int *pitch);
static int OHOS_UpdateWindowFramebuffer(_THIS, SDL_Window *window, const SDL_Rect *rects, int numrects);
static void OHOS_DestroyWindowFramebuffer(_THIS, SDL_Window *window);
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
	device->DestroyWindow = OHOS_DestroyWindow;
	device->CreateWindowFramebuffer = OHOS_CreateWindowFramebuffer;
	device->UpdateWindowFramebuffer = OHOS_UpdateWindowFramebuffer;
	device->DestroyWindowFramebuffer = OHOS_DestroyWindowFramebuffer;
	device->SetWindowTitle = OHOS_SetWindowTitle;
	device->SetWindowPosition = OHOS_SetWindowPosition;
	device->SetWindowSize = OHOS_SetWindowSize;
	device->ShowWindow = OHOS_ShowWindow;
	device->HideWindow = OHOS_HideWindow;
	device->SetWindowFullscreen = OHOS_SetWindowFullscreen;

	/* Register the OpenGLES backend so SDL_CreateRenderer(SDL_RENDERER_ACCELERATED)
	 * uses the hardware GLES render driver (EGL via OH_NativeWindow) instead of
	 * falling back to the software surface renderer. OHOS_GL_* (SDL_ohosgl.c)
	 * uses eglCreateWindowSurface/eglCreateContext on the XComponent native
	 * window - it does NOT depend on eglQueryDevicesEXT. */
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
